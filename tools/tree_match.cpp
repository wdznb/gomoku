// gomoku_tree_match —— 方案 C（docs/ENGINE_REDESIGN.md :555「棋力回归」那一格）的整树赛夹具。
//
// 两个 gomoku_server 进程各持一个引擎（旧树 vs 新树，或同一棵树的两套配置），经 WS 对打。
// 每侧走的都是线上那条正常会话（自己的 TT / 战术层 / 时间预算；开局库按下面「起服务器时必须带
// `--no-book`」那条规矩关掉），夹具只扮演「玩家」，把一侧的 ai_move 原样递给另一侧 ⇒ 不是进程内的近似。
//
// 计价口径：跨树只能用墙钟（v0.5.0-phase4 的 set_config 只认 timeout_ms/max_depth，
// max_nodes 会被旧树忽略）；同一棵树的两套配置可以按节点出价，那条才是负载无关的尺子。
// 座位：相邻两局换坐，开局集固定，两个座位各跑满 —— 轮次131 立的规矩（先量夹具自己）。
// 作废局（连不上 / 等帧超时 / 对侧 error）**不计入分数**，只计数：一次负载抖动既不是胜负也不是和棋。
//
// 起服务器时必须带 `--no-book`（轮次176 的探针 ⑤/⑥ 量出来的）：开局库的加权抽签用
// `std::random_device` 每进程播种（src/optimization/opening_book.h:57），所以库一开，同一套配置
// 两跑在第 0 手就可能各抽各的 —— 实测 90 局翻 9 局，其中 6 局在第 0 手分岔。库一关，同配置两跑
// 逐手零分岔、双方都读 0.5000。这不是负载，也不是战术层的预算口径（那条同轮按自己的理由改了，
// 但它动不了这个地板）。开局的不确定性留在库里，就该由开局集来定，不该由抽签来定。
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using nlohmann::json;

namespace {

// 一步最多等多久：思考预算 + 富余。富余有意给大 —— 判定只看「有没有回帧」，
// 富余收紧会把一次机器抖动记成对手弃权，而那正是夹具最不该有的自由度。
constexpr int kStepSlackMs = 30000;
constexpr int kMaxPlies = 225;

std::mt19937& rng() {
    static std::mt19937 g{std::random_device{}()};
    return g;
}

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool read_exact(int fd, unsigned char* buf, size_t n, int timeout_ms) {
    if (timeout_ms < 0) timeout_ms = 0;
    size_t got = 0;
    while (got < n) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, timeout_ms) <= 0) return false;
        const ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

bool write_all(int fd, const unsigned char* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        const ssize_t w = send(fd, buf + sent, n - sent, MSG_NOSIGNAL);
        if (w <= 0) return false;
        sent += static_cast<size_t>(w);
    }
    return true;
}

std::string base64(const unsigned char* data, size_t n) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const unsigned long v = (static_cast<unsigned long>(data[i]) << 16) |
            ((i + 1 < n ? data[i + 1] : 0) << 8) | (i + 2 < n ? data[i + 2] : 0);
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? kTable[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? kTable[v & 63] : '=');
    }
    return out;
}

// 最小 WS 客户端：客户端帧按 RFC 6455 必须掩码，服务端（Beast）不掩码；
// ping 就地回 pong，close 记成断开，分片消息攒到 FIN 再交出去。
class Ws {
public:
    Ws() = default;
    ~Ws() { close(); }
    Ws(const Ws&) = delete;
    Ws& operator=(const Ws&) = delete;

    bool open(const std::string& host, int port, std::string* err) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) { *err = "socket() 失败"; return false; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            *err = "地址不是点分十进制: " + host;
            return false;
        }
        if (connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            *err = "connect " + host + ":" + std::to_string(port) + ": " + strerror(errno);
            return false;
        }
        unsigned char key[16];
        for (auto& b : key) b = static_cast<unsigned char>(rng()() & 0xFF);
        const std::string req = "GET / HTTP/1.1\r\nHost: " + host + ":" +
            std::to_string(port) + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + base64(key, sizeof key) +
            "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!write_all(fd_, reinterpret_cast<const unsigned char*>(req.data()), req.size())) {
            *err = "握手写不出去";
            return false;
        }
        std::string head;
        char c = 0;
        while (head.find("\r\n\r\n") == std::string::npos) {
            pollfd p{fd_, POLLIN, 0};
            if (poll(&p, 1, 10000) <= 0) { *err = "握手超时"; return false; }
            if (recv(fd_, &c, 1, 0) != 1) { *err = "握手中途断开"; return false; }
            head.push_back(c);
            if (head.size() > 8192) { *err = "握手响应过大"; return false; }
        }
        if (head.find(" 101 ") == std::string::npos) {
            *err = "握手不是 101: " + head.substr(0, head.find("\r\n"));
            return false;
        }
        return true;
    }

    void close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

    bool send_text(const json& msg, uint8_t opcode = 0x1) {
        return send_raw(msg.dump(), opcode);
    }

    bool send_raw(const std::string& payload, uint8_t opcode) {
        if (fd_ < 0) return false;
        std::vector<unsigned char> out;
        out.push_back(0x80 | opcode);
        const size_t n = payload.size();
        if (n < 126) {
            out.push_back(0x80 | static_cast<unsigned char>(n));
        } else if (n <= 0xFFFF) {
            out.push_back(0x80 | 126);
            out.push_back(static_cast<unsigned char>(n >> 8));
            out.push_back(static_cast<unsigned char>(n & 0xFF));
        } else {
            out.push_back(0x80 | 127);
            for (int shift = 56; shift >= 0; shift -= 8)
                out.push_back(static_cast<unsigned char>((n >> shift) & 0xFF));
        }
        unsigned char mask[4];
        for (auto& b : mask) b = static_cast<unsigned char>(rng()() & 0xFF);
        out.insert(out.end(), mask, mask + 4);
        for (size_t i = 0; i < n; ++i) out.push_back(static_cast<unsigned char>(payload[i]) ^ mask[i & 3]);
        return write_all(fd_, out.data(), out.size());
    }

    // 一条完整消息；超时或断开返回 false，*closed 只在服务端关闭时置真。
    bool recv_message(std::string* out, int timeout_ms, bool* closed) {
        const long long deadline = now_ms() + timeout_ms;
        std::string message;
        for (;;) {
            const int left = static_cast<int>(deadline - now_ms());
            if (left <= 0) return false;
            unsigned char h[2];
            if (!read_exact(fd_, h, 2, left)) return false;
            const bool fin = (h[0] & 0x80) != 0;
            const uint8_t opcode = h[0] & 0x0F;
            const bool masked = (h[1] & 0x80) != 0;
            size_t len = h[1] & 0x7F;
            if (len == 126) {
                unsigned char ext[2];
                if (!read_exact(fd_, ext, 2, remaining(deadline))) return false;
                len = (static_cast<size_t>(ext[0]) << 8) | ext[1];
            } else if (len == 127) {
                unsigned char ext[8];
                if (!read_exact(fd_, ext, 8, remaining(deadline))) return false;
                len = 0;
                for (unsigned char b : ext) len = (len << 8) | b;
            }
            unsigned char mask_key[4] = {0, 0, 0, 0};
            if (masked && !read_exact(fd_, mask_key, 4, remaining(deadline))) return false;
            std::vector<unsigned char> payload(len);
            if (len && !read_exact(fd_, payload.data(), len, remaining(deadline))) return false;
            if (masked)
                for (size_t i = 0; i < len; ++i) payload[i] ^= mask_key[i & 3];

            if (opcode == 0x8) { *closed = true; return false; }
            if (opcode == 0x9) {
                send_raw(std::string(payload.begin(), payload.end()), 0xA);
                continue;
            }
            if (opcode == 0xA) continue;
            if (opcode != 0x1 && opcode != 0x0 && opcode != 0x2) continue;
            message.append(reinterpret_cast<char*>(payload.data()), len);
            if (fin) { *out = std::move(message); return true; }
        }
    }

private:
    static int remaining(long long deadline) {
        const long long left = deadline - now_ms();
        return left > 0 ? static_cast<int>(left) : 0;
    }

    int fd_ = -1;
};

struct Options {
    std::string a_host = "127.0.0.1", b_host = "127.0.0.1";
    int a_port = 8081, b_port = 8082;
    std::string a_name = "new", b_name = "old";
    std::string openings_path, out_path;
    int games = 2, timeout_ms = 1000, max_depth = 8;
    int a_max_nodes = 0, b_max_nodes = 0;
    int shard = 0, shards = 1;
    bool append = false;
};

// 开局集：一行一个 position 文档（{"moves":[[r,c]...],"first":"black"}）。
// 只收「偶数子、黑先行」的开局：夹具的循环固定从执黑那一侧读第一帧，奇数子会让它
// 等一个永远不会来的 ai_move ⇒ 与其悄悄产出一堆作废局，不如在门口就拒不收。
std::vector<json> load_openings(const std::string& path) {
    std::vector<json> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        json parsed = json::parse(line, nullptr, false);
        if (parsed.is_discarded()) {
            std::fprintf(stderr, "开局集跳过一行非 JSON: %s\n", line.substr(0, 60).c_str());
            continue;
        }
        if (parsed.empty()) { out.push_back(std::move(parsed)); continue; }
        if (!parsed.contains("moves") || !parsed["moves"].is_array()) {
            std::fprintf(stderr, "开局集跳过一行（没有 moves 数组）: %s\n",
                         line.substr(0, 60).c_str());
            continue;
        }
        const size_t stones = parsed["moves"].size();
        const std::string first = parsed.value("first", std::string("black"));
        if (stones % 2 != 0 || first != "black") {
            std::fprintf(stderr,
                         "开局集只收偶数子且黑先行的开局，跳过 %zu 子 / first=%s\n",
                         stones, first.c_str());
            continue;
        }
        out.push_back(std::move(parsed));
    }
    return out;
}

struct GameResult {
    std::string winner = "abort";   // a / b / draw / abort
    std::string reason;
    int plies = 0;
    long long ms_a = 0, ms_b = 0;
    // 逐手记录 [row, col, 落子方, 该手思考毫秒]。同配置两跑比的是「第一个分歧出现在第几手」，
    // 只有手数与胜负的赛具答不出这个问题（轮次176：新树与旧树各读 9/90 翻局，而进程内 3 轮
    // ×44 局指纹零差异 ⇒ 分歧不在引擎，但赛具量不到它落在哪一手）。
    json moves = json::array();
    // 作废那一刻从两侧各取一份 replay 比对出来的话（「两侧盘面一致」或「第 N 子起不同」）。
    // 空串 = 这局没作废，或作废在取不到 replay 的分支上。
    std::string drift;
};

// 读到某一侧出现 want 里的帧类型为止；error 帧、断开、超时都算这局作废。
// 作废必须说得出是谁、说什么 —— 一半对局作废的赛具和没有赛具是同一件事。
bool wait_event(Ws& ws, const std::vector<std::string>& want, int step_ms, json* out,
                std::string* why, const std::string& who) {
    for (;;) {
        std::string frame;
        bool closed = false;
        if (!ws.recv_message(&frame, step_ms, &closed)) {
            *why = who + (closed ? " 断开" : " 等帧超时");
            return false;
        }
        json msg = json::parse(frame, nullptr, false);
        if (msg.is_discarded() || !msg.contains("type") || !msg["type"].is_string()) continue;
        const std::string type = msg["type"].get<std::string>();
        if (type == "error") {
            *why = who + " 回 error: " + msg.value("message", std::string("(无 message 字段)"));
            return false;
        }
        if (std::find(want.begin(), want.end(), type) != want.end()) {
            *out = std::move(msg);
            return true;
        }
    }
}

// game_over 的 winner 是盘面色（"black"/"white"/"draw"/"none"），要换算回「哪一侧」。
std::string winner_side(const json& msg, bool a_is_black) {
    const std::string w = msg.value("winner", std::string(""));
    if (w == "draw" || w == "none") return "draw";
    if (w == "black") return a_is_black ? "a" : "b";
    if (w == "white") return a_is_black ? "b" : "a";
    return "abort";
}

// 作废的那一刻，向两侧各要一份 replay —— 把「谁的盘和谁的不一样」变成记录里的一个字段，
// 而不是留给下一轮猜。轮次177 的 game 247 就是猜不出来的那种：两跑前 13 手逐字相同，
// 同一颗黑子 (4,2) 一跑被收、一跑被 `make_move` 拒；`make_move` 的六个拒绝条件里
// 「盘面已占」和「轮到谁」都只取决于盘与状态，所以真正要分的不是「谁拒的」，是
// 「两侧的盘到那一刻为止是不是一样的」（src/server/game_manager.cpp:214-248 的六个拒绝条件）。
bool query_replay(Ws& ws, int budget_ms, json* out) {
    if (!ws.send_text({{"type", "request_replay"}})) return false;
    const long long deadline = now_ms() + budget_ms;
    for (int tries = 0; tries < 32; ++tries) {
        std::string frame;
        bool closed = false;
        const int left = static_cast<int>(deadline - now_ms());
        if (left <= 0 || !ws.recv_message(&frame, left, &closed)) return false;
        json msg = json::parse(frame, nullptr, false);
        if (msg.is_discarded()) continue;
        if (msg.value("type", std::string("")) == "replay") { *out = std::move(msg); return true; }
    }
    return false;
}

// 一侧的「盘」= 手摆局面摆下的 base 子 + 这局落的 moves 子，按顺序取 (行,列,色)。
std::vector<std::string> stones_of(const json& replay) {
    std::vector<std::string> stones;
    const auto push = [&stones](const json& entry) {
        stones.push_back(std::to_string(entry.value("row", -1)) + "," +
                         std::to_string(entry.value("col", -1)) + "," +
                         entry.value("color", std::string("?")));
    };
    if (replay.contains("base")) for (const auto& s : replay["base"]) push(s);
    if (replay.contains("moves")) for (const auto& m : replay["moves"]) push(m);
    return stones;
}

std::string drift_note(const json& ra, const json& rb) {
    const std::vector<std::string> a = stones_of(ra), b = stones_of(rb);
    if (a == b) return "两侧盘面一致（" + std::to_string(a.size()) + " 子）⇒ 拒子与盘无关，是回合/状态那一类";
    size_t k = 0;
    while (k < a.size() && k < b.size() && a[k] == b[k]) ++k;
    return "两侧盘面第 " + std::to_string(k + 1) + " 子起不同（A=" +
           (k < a.size() ? a[k] : std::string("(无)")) + " B=" + (k < b.size() ? b[k] : std::string("(无)")) +
           "，共 " + std::to_string(a.size()) + "/" + std::to_string(b.size()) + " 子）";
}

// 某一侧不再说话了：先看对面是不是早就把「结束」那一帧压在缓冲里。
// 认输发生在 AI 自己那一手之后：它发完 ai_move 才补 game_over，而夹具这时已经换去读被递子
// 的那一侧，于是输家的 game_over 一直躺在原 socket 里 —— 下一子递回去时它只会回
// 「Invalid move」。不先排空就把这局算作废，等于把「对手认输了」读成「我对面坏了」。
// 判据口径与 game_over 帧本身一致：只信盘面色（"black"/"white"/"draw"），座位换算交给
// 调用方，省得在这一条支路上再造一份颜色学。
bool drain_end(Ws& ws, const std::string& who, GameResult& res, bool a_is_black) {
    json msg;
    std::string why;
    if (!wait_event(ws, {"game_over"}, 3000, &msg, &why, who)) return false;
    if (msg["type"] != "game_over") return false;
    const std::string color = msg.value("winner", std::string(""));
    if (color == "draw" || color == "none") res.winner = "draw";
    else if (color == "black") res.winner = a_is_black ? "a" : "b";
    else if (color == "white") res.winner = a_is_black ? "b" : "a";
    else return false;
    res.reason = "对面局终帧迟到（" + msg.value("reason", std::string("")) + "）";
    return true;
}

GameResult play_game(const Options& opt, const json& opening, bool a_is_black) {
    GameResult res;
    Ws A, B;
    std::string err;
    if (!A.open(opt.a_host, opt.a_port, &err)) { res.reason = "A 连不上: " + err; return res; }
    if (!B.open(opt.b_host, opt.b_port, &err)) { res.reason = "B 连不上: " + err; return res; }

    A.send_text({{"type", "restart"}});
    B.send_text({{"type", "restart"}});
    json cfg_a = {{"type", "set_config"}, {"timeout_ms", opt.timeout_ms},
                  {"max_depth", opt.max_depth}};
    json cfg_b = cfg_a;
    if (opt.a_max_nodes > 0) cfg_a["max_nodes"] = opt.a_max_nodes;
    if (opt.b_max_nodes > 0) cfg_b["max_nodes"] = opt.b_max_nodes;
    A.send_text(cfg_a);
    B.send_text(cfg_b);
    if (!opening.empty()) {
        A.send_text({{"type", "set_position"}, {"position", opening}});
        B.send_text({{"type", "set_position"}, {"position", opening}});
    }
    // "color" 说的是这一侧玩家执什么，AI 执另一色；执黑那一侧选完边就立刻起手。
    A.send_text({{"type", "choose_side"}, {"color", a_is_black ? "white" : "black"}});
    B.send_text({{"type", "choose_side"}, {"color", a_is_black ? "black" : "white"}});

    Ws* mover = a_is_black ? &A : &B;
    Ws* other = a_is_black ? &B : &A;
    const int step_ms = opt.timeout_ms + kStepSlackMs;
    int last_row = -1, last_col = -1;
    std::string last_by;

    while (res.plies < kMaxPlies) {
        json ev;
        std::string why;
        if (!wait_event(*mover, {"ai_move", "game_over"}, step_ms, &ev, &why,
                        mover == &A ? opt.a_name : opt.b_name)) {
            const std::string failed = mover == &A ? opt.a_name : opt.b_name;
            Ws& silent = mover == &A ? B : A;
            const std::string silent_name = mover == &A ? opt.b_name : opt.a_name;
            if (!drain_end(silent, silent_name, res, a_is_black) &&
                !drain_end(*mover, failed, res, a_is_black)) {
                res.winner = "abort";
                res.reason = why;
                // 作废要指认得出是哪一手：只有「刚递给对面的那个点」能被对面判非法，
                // 而「谁判谁非法」正是这台赛具目前唯一还不知道答案的问题。
                if (last_row >= 0) {
                    res.reason += "（" + last_by + " 落 (" + std::to_string(last_row) + "," +
                        std::to_string(last_col) + ") 之后）";
                }
                // 再问一层：两侧到这一刻的盘到底一不一样。取不到就如实写取不到。
                json ra, rb;
                const bool ok_a = query_replay(A, 4000, &ra);
                const bool ok_b = query_replay(B, 4000, &rb);
                res.drift = (ok_a && ok_b) ? drift_note(ra, rb)
                                           : std::string("(replay 没取到：") +
                                                 (ok_a ? std::string() : "A ") +
                                                 (ok_b ? std::string() : "B ") + ")";
                res.reason += " ｜" + res.drift;
            } else {
                res.reason = failed + " 已不回话，" + res.reason;
            }
            break;
        }
        if (ev["type"] == "game_over") {
            res.winner = winner_side(ev, a_is_black);
            res.reason = ev.value("reason", std::string(""));
            break;
        }
        const int row = ev.value("row", -1);
        const int col = ev.value("col", -1);
        const long long think = ev.value("thinking_time_ms", 0LL);
        // 计时按座位换算回 A/B：mover 执黑 ⇔ (mover==A) == a_is_black。
        const bool mover_is_a = (mover == &A);
        if (mover_is_a) res.ms_a += think; else res.ms_b += think;
        ++res.plies;
        if (row < 0 || col < 0) { res.winner = "abort"; res.reason = "ai_move 没有坐标"; break; }
        last_row = row;
        last_col = col;
        last_by = mover == &A ? opt.a_name : opt.b_name;
        res.moves.push_back(json::array({row, col, last_by, think}));
        if (!other->send_text({{"type", "make_move"}, {"row", row}, {"col", col}})) {
            res.winner = "abort";
            res.reason = "递给对手失败";
            break;
        }
        std::swap(mover, other);
    }
    if (res.winner == "abort" && res.reason.empty()) res.reason = "手数上限";
    return res;
}

// Wilson 区间：小样本下比正态近似诚实，而 384 局这种量正好是它该出场的地方。
void wilson(double p, long n, double* lo, double* hi) {
    const double z = 1.959963984540054;
    if (n <= 0) { *lo = 0.0; *hi = 0.0; return; }
    const double z2 = z * z;
    const double denom = 1.0 + z2 / n;
    const double centre = (p + z2 / (2 * n)) / denom;
    const double half = z * std::sqrt(p * (1 - p) / n + z2 / (4.0 * n * n)) / denom;
    *lo = centre - half;
    *hi = centre + half;
}

struct Tally {
    long games = 0, a_wins = 0, b_wins = 0, draws = 0, aborted = 0;
    long invalid_moves = 0;      // 对面不肯接的那一手：两棵树的禁手判定不同，不是机器抖动
    long black_wins = 0, decided_black = 0;
    double score = 0.0;        // A 的得分率（和棋算半分）
    long long ms_a = 0, ms_b = 0;
};

Tally tally_of(const std::vector<std::pair<json, GameResult>>& rows) {
    Tally t;
    double points = 0.0;
    for (const auto& row : rows) {
        const GameResult& g = row.second;
        ++t.games;
        t.ms_a += g.ms_a;
        t.ms_b += g.ms_b;
        if (g.winner == "abort") {
            ++t.aborted;
            if (g.reason.find("Invalid move") != std::string::npos) ++t.invalid_moves;
            continue;
        }
        const bool a_black = row.first.value("a_black", true);
        ++t.decided_black;
        if (g.winner == "a") { ++t.a_wins; points += 1.0; }
        else if (g.winner == "b") { ++t.b_wins; points += 0.0; }
        else { ++t.draws; points += 0.5; }
        const std::string black_side = a_black ? "a" : "b";
        if (g.winner == black_side) ++t.black_wins;
    }
    const long decided = t.a_wins + t.b_wins + t.draws;
    t.score = decided > 0 ? points / static_cast<double>(decided) : 0.0;
    if (t.decided_black == 0) t.decided_black = 1;
    return t;
}

void print_report(const Tally& t, const Options& opt, const char* title) {
    const long decided = t.a_wins + t.b_wins + t.draws;
    double lo = 0.0, hi = 0.0;
    wilson(t.score, decided, &lo, &hi);
    std::printf("%s\n", title);
    std::printf("  对局=%ld 计入=%ld 作废=%ld (%.1f%%)，其中对面拒收「Invalid move」=%ld 局\n",
                t.games, decided, t.aborted,
                t.games ? 100.0 * t.aborted / t.games : 0.0, t.invalid_moves);
    std::printf("  %s(%s:%d) 得分 %.4f   95%% Wilson 区间 [%.4f, %.4f]\n", opt.a_name.c_str(),
                opt.a_host.c_str(), opt.a_port, t.score, lo, hi);
    std::printf("  胜=%ld 负=%ld 和=%ld   执黑一侧胜率=%.4f (%ld/%ld) 座位不均就是夹具自己的账\n",
                t.a_wins, t.b_wins, t.draws, static_cast<double>(t.black_wins) / t.decided_black,
                t.black_wins, t.decided_black);
    if (decided > 0) {
        std::printf("  每局均耗 %s=%.1f ms / %s=%.1f ms\n", opt.a_name.c_str(),
                    static_cast<double>(t.ms_a) / decided, opt.b_name.c_str(),
                    static_cast<double>(t.ms_b) / decided);
    }
}

// ── 配对报表：同一开局的两个座位合起来算一对 ─────────────────────────────────
// 上面那个分数把「A 执黑赢 + A 执白输」也记成 0.5 —— 这一对没有说出谁更强，它只是噪声的均值。
// 只有两座位同胜的对（rich = A 两胜，reversed = B 两胜）才真的把差值指出来，而判定要的分辨率是
// SE = 0.5/√n_inf，n_inf 正是这两种对的只数。这一格以前由临时脚本在 CSV 外面算，脚本自己错过三次
// （轮次171 的 `--append` 撞号、轮次124 的空日志、轮次129 的锚点脚本），所以挪进仪器里。
struct Pairing {
    long complete = 0;          // 两个座位都跑到的对
    long rich = 0, split = 0, reversed = 0;
    long split_with_draw = 0;   // split 里「有一手是和棋」那一子类
    long incomplete = 0;        // 任一手作废，或只跑到一个座位
    long collisions = 0;        // 同一个 (对, 座位) 出现两次 ⇒ 这份档不止拼了一遍读数
};

Pairing pairing_of(const std::vector<std::pair<json, GameResult>>& rows) {
    Pairing p;
    std::map<long, std::map<bool, long>> seen;      // 对 → 座位 → 只数
    std::map<long, std::map<bool, double>> points;  // 对 → 座位 → A 拿到的分（作废记 -1）
    for (const auto& row : rows) {
        const long pair = row.first.value("game", 0LL) / 2;
        const bool a_black = row.first.value("a_black", true);
        if (++seen[pair][a_black] > 1) ++p.collisions;
        const std::string& w = row.second.winner;
        points[pair][a_black] = w == "abort" ? -1.0 : (w == "a" ? 1.0 : (w == "b" ? 0.0 : 0.5));
    }
    for (const auto& kv : seen) {
        const std::map<bool, long>& seats = kv.second;
        if (!seats.count(true) || !seats.count(false)) {
            ++p.incomplete;   // 只看到一个座位：这一对不能算进任何一边
            continue;
        }
        const double x = points.at(kv.first).at(true);
        const double y = points.at(kv.first).at(false);
        if (x < 0.0 || y < 0.0) {
            ++p.incomplete;
            continue;
        }
        ++p.complete;
        const double a = x + y;   // A 在这一对里拿到的总分
        if (a == 2.0) ++p.rich;
        else if (a == 0.0) ++p.reversed;
        else {
            ++p.split;
            if (x == 0.5 || y == 0.5) ++p.split_with_draw;
        }
    }
    return p;
}

void print_pairs(const Pairing& p) {
    if (p.collisions) {
        std::printf("  配对报表拒绝出数：同一个（开局对，座位）出现了不止一次（撞号 %ld 处）⇒ 这份档里拼了"
                    "不止一遍读数（`--append` 或分片编号会撞），先把它拆开\n", p.collisions);
        return;
    }
    const long n_inf = p.rich + p.reversed;
    const double phat = n_inf > 0 ? static_cast<double>(p.rich) / n_inf : 0.0;
    const double se_inf = n_inf > 0 ? 0.5 / std::sqrt(static_cast<double>(n_inf)) : 0.0;
    const double cmp = p.complete > 0
                           ? (p.rich + 0.5 * p.split) / static_cast<double>(p.complete) : 0.0;
    const double se_cmp = p.complete > 0 ? 0.5 / std::sqrt(static_cast<double>(p.complete)) : 0.0;
    std::printf("  配对（两座位同开局算一对）：完整=%ld 不完整(作废/缺一座位)=%ld   "
                "A 两胜=%ld 各胜一座位=%ld（其中含和=%ld） B 两胜=%ld\n",
                p.complete, p.incomplete, p.rich, p.split, p.split_with_draw, p.reversed);
    std::printf("  有信息对 n_inf=%ld ⇒ p̂(A 两胜 | 有信息)=%.4f SE=%.4f 95%%≈[%.4f, %.4f]；"
                "完整对得分=%.4f SE=%.4f（分数按 0.5 为中性，差过 1.96 SE 才谈方向）\n",
                n_inf, phat, se_inf, phat - 1.96 * se_inf, phat + 1.96 * se_inf, cmp, se_cmp);
}

// 仪器自己的用例：喂一份合成棋谱，钉住四类计数与撞号那一臂。判定要用这把尺子，它自己不能没测过。
// 这里**不用** assert()：`gomoku_tree_match` 不在 CMakeLists:339-348 那个「每个套件都加 -UNDEBUG」的
// 名单里，一旦有人把树配成 Release，assert 版自检会变成永远通过的空过场。
int selfcheck_pairs() {
    int failures = 0;
    auto expect = [&failures](bool ok, const char* what) {
        if (!ok) {
            std::printf("  [selfcheck] 不自洽：%s\n", what);
            ++failures;
        }
    };

    std::vector<std::pair<json, GameResult>> rows;
    auto add = [&rows](long game, bool a_black, const std::string& winner) {
        json rec = {{"game", game}, {"a_black", a_black}};
        GameResult g;
        g.winner = winner;
        rows.emplace_back(rec, g);
    };
    add(0, true, "a");    add(1, false, "a");      // A 两个座位都赢 ⇒ rich
    add(2, true, "b");    add(3, false, "b");      // B 两个座位都赢 ⇒ reversed
    add(4, true, "a");    add(5, false, "b");      // 各胜一座位 ⇒ split
    add(6, true, "draw"); add(7, false, "a");      // 半分 + 一胜 ⇒ split（含和那一子类）
    add(8, true, "a");    add(9, false, "abort");  // 一手作废 ⇒ incomplete
    add(10, true, "a");                            // 只跑到一个座位 ⇒ incomplete 的另一条路
    // winner 记的是**哪一侧**赢（"a"/"b"/"draw"），座位由 a_black 记；一对里两条的座位必须一真一假，
    // 否则那不是「同一开局的两个座位」。上面第一版把 rich 写成了「一条 a 一条 b」，被这条自检当场抓住
    // —— 尺子自己有牙，这正是把它放进 ctest 的理由。

    const Pairing p = pairing_of(rows);
    expect(p.complete == 4, "完整对要 4");
    expect(p.incomplete == 2, "不完整对要 2（一手作废的那对 + 只跑到一个座位的那对）");
    expect(p.rich == 1, "rich 要 1");
    expect(p.reversed == 1, "reversed 要 1");
    expect(p.split == 2, "split 要 2");
    expect(p.split_with_draw == 1, "split 里含和的子类要 1");
    expect(p.collisions == 0, "干净档不许报撞号");

    std::vector<std::pair<json, GameResult>> dup = rows;
    dup.push_back(rows[0]);   // 同一个 (对 0, 执黑座位) 再出现一次
    const Pairing d = pairing_of(dup);
    expect(d.collisions == 1, "撞号那一臂要报 1 处");

    std::printf("[selfcheck] 配对报表\n");
    print_pairs(p);
    std::printf("  钉值：完整 4 / 不完整 2（作废一对 + 缺一座位一对）/ rich 1 / split 2(含和 1) / "
                "reversed 1 / n_inf 2\n");
    std::printf("  撞号那一臂：collisions=%ld（报表已拒绝出数）\n", d.collisions);
    if (failures) {
        std::printf("  [selfcheck] 失败 %d 条\n", failures);
        return 1;
    }
    std::printf("  [selfcheck] 全部自洽\n");
    return 0;
}

bool parse_endpoint(const std::string& spec, std::string* host, int* port) {
    const size_t colon = spec.rfind(':');
    if (colon == std::string::npos) return false;
    *host = spec.substr(0, colon);
    *port = std::atoi(spec.substr(colon + 1).c_str());
    return !host->empty() && *port > 0 && *port < 65536;
}

int run_report(const Options& opt) {
    std::ifstream in(opt.out_path);
    if (!in) { std::fprintf(stderr, "读不到 %s\n", opt.out_path.c_str()); return 2; }
    std::vector<std::pair<json, GameResult>> rows;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        json rec = json::parse(line, nullptr, false);
        if (rec.is_discarded()) continue;
        GameResult g;
        g.winner = rec.value("winner", "abort");
        g.reason = rec.value("reason", std::string());
        g.plies = rec.value("plies", 0);
        g.ms_a = rec.value("ms_a", 0LL);
        g.ms_b = rec.value("ms_b", 0LL);
        rows.emplace_back(rec, g);
    }
    print_report(tally_of(rows), opt, "整树赛读数（来自档）");
    print_pairs(pairing_of(rows));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    bool report_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](int* out) {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s 缺参数\n", arg.c_str()); return false; }
            *out = std::atoi(argv[++i]);
            return true;
        };
        std::string host; int port = 0;
        if (arg == "--a" && i + 1 < argc && parse_endpoint(argv[++i], &host, &port)) {
            opt.a_host = host; opt.a_port = port;
        } else if (arg == "--b" && i + 1 < argc && parse_endpoint(argv[++i], &host, &port)) {
            opt.b_host = host; opt.b_port = port;
        } else if (arg == "--names" && i + 1 < argc) {
            const std::string pair_s = argv[++i];
            const size_t eq = pair_s.find('=');
            if (eq == std::string::npos) { std::fprintf(stderr, "--names 要 a=b\n"); return 1; }
            opt.a_name = pair_s.substr(0, eq);
            opt.b_name = pair_s.substr(eq + 1);
        } else if (arg == "--openings" && i + 1 < argc) {
            opt.openings_path = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            opt.out_path = argv[++i];
        } else if (arg == "--report" && i + 1 < argc) {
            report_only = true;
            opt.out_path = argv[++i];
        } else if (arg == "--games") { if (!next(&opt.games)) return 1;
        } else if (arg == "--timeout-ms") { if (!next(&opt.timeout_ms)) return 1;
        } else if (arg == "--max-depth") { if (!next(&opt.max_depth)) return 1;
        } else if (arg == "--a-max-nodes") { if (!next(&opt.a_max_nodes)) return 1;
        } else if (arg == "--b-max-nodes") { if (!next(&opt.b_max_nodes)) return 1;
        } else if (arg == "--append") {
            opt.append = true;
        } else if (arg == "--shard") { if (!next(&opt.shard)) return 1;
        } else if (arg == "--shards") { if (!next(&opt.shards)) return 1;
        } else if (arg == "--selfcheck-pairs") {
            // 不需要服务器：仪器自己的钉值（进 ctest，见 CMakeLists 末尾那条 add_test）
            return selfcheck_pairs();
        } else if (arg == "--help" || arg == "-h") {
            std::printf("用法: %s --a HOST:PORT --b HOST:PORT [--names new=old]\n"
                        "        [--openings FILE.jsonl] [--games N] [--shard K --shards M]\n"
                        "        [--timeout-ms N] [--max-depth N] [--a-max-nodes N --b-max-nodes N]\n"
                        "        [--out FILE.jsonl [--append] | --report FILE.jsonl]\n"
                        "        %s --selfcheck-pairs   （跑配对报表的钉值用例，不连服务器）\n",
                        argv[0], argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "不认识的参数: %s\n", arg.c_str());
            return 1;
        }
    }
    if (report_only) {
        if (opt.out_path.empty()) opt.out_path = opt.openings_path;
        return run_report(opt);
    }
    if (opt.shards < 1 || opt.shard < 0 || opt.shard >= opt.shards) {
        std::fprintf(stderr, "--shard 要在 [0, --shards) 内\n");
        return 1;
    }
    std::vector<json> openings;
    if (opt.openings_path.empty()) {
        openings.push_back(json::object());   // 空盘 = 一条开局
    } else {
        openings = load_openings(opt.openings_path);
        if (openings.empty()) { std::fprintf(stderr, "开局集是空的\n"); return 2; }
    }
    // 游戏 i 的座位由 i 的奇偶定，分片只取属于自己的那几号 ⇒ 合并后两座位各跑满。
    // 追加模式会把两跑串进同一个文件（轮次171 就被自己这么绊了一次：180 行里混着两次 90 局，
    // 而日志只报最后一跑的读数）。所以默认要求目标文件不存在或为空。
    {
        std::ifstream probe(opt.out_path);
        std::string scratch;
        if (probe && std::getline(probe, scratch) && !scratch.empty() && !opt.append) {
            std::fprintf(stderr,
                         "%s 已经有内容（%zu 字节起头）。两跑串在一档会把读数算错 —— "
                         "要么换个 --out，要么明确加 --append\n", opt.out_path.c_str(),
                         scratch.size());
            return 2;
        }
    }
    std::ofstream out(opt.out_path, opt.append ? std::ios::app : std::ios::trunc);
    if (!out) { std::fprintf(stderr, "写不出 %s\n", opt.out_path.c_str()); return 2; }

    std::vector<std::pair<json, GameResult>> rows;
    const long long t0 = now_ms();
    const long cycle = 2 * static_cast<long>(openings.size());
    if (opt.games % cycle != 0) {
        std::fprintf(stderr,
                     "！局数 %d 不是 2×开局数=%ld 的整数倍 ⇒ 有开局没被两个座位各跑一遍，"
                     "这一份读数的座位差不为零\n", opt.games, cycle);
    }
    for (int g = 0; g < opt.games; ++g) {
        // 分片按「对」切，不是按局切：同一开局的两个座位要落在同一片里，
        // 否则每片各自只看到一个座位，合起来才平 —— 单片的报告就成了假的偏。
        const long pair = g / 2;
        if (opt.shards > 1 && (pair % opt.shards) != opt.shard) continue;
        // 排成「同一开局连着跑两个座位」：开局号 = pair，座位 = g 的奇偶。
        // 用 g % 开局数 会和 g % 2 同周期（开局数是偶数时），那样每条开局只会出现在
        // 同一个座位上 —— 轮次170 的 null 对照就是被这个坑读出 0.167 的。
        const json& opening = openings[static_cast<size_t>(pair) % openings.size()];
        const bool a_is_black = (g % 2) == 0;
        const GameResult r = play_game(opt, opening, a_is_black);
        json rec = {{"game", g}, {"a_black", a_is_black}, {"winner", r.winner},
                    {"reason", r.reason}, {"plies", r.plies}, {"ms_a", r.ms_a},
                    {"ms_b", r.ms_b}, {"timeout_ms", opt.timeout_ms},
                    {"max_depth", opt.max_depth}, {"a_max_nodes", opt.a_max_nodes},
                    {"b_max_nodes", opt.b_max_nodes}, {"a", opt.a_name}, {"b", opt.b_name},
                    {"moves", r.moves}, {"drift", r.drift}};
        out << rec.dump() << "\n" << std::flush;
        rows.emplace_back(std::move(rec), r);
        std::printf("  第 %d 局 座位%s 手数=%d 结果=%s%s 用时=%lldms\n", g,
                    a_is_black ? " A执黑" : " B执黑", r.plies, r.winner.c_str(),
                    (r.winner == "abort" ? (", " + r.reason).c_str() : ""),
                    static_cast<long long>(r.ms_a + r.ms_b));
    }
    const long long spent = now_ms() - t0;
    Tally t = tally_of(rows);
    char title[160];
    std::snprintf(title, sizeof title, "整树赛（分片 %d/%d，%zu 条开局，预算 %d ms/手，深度 %d）",
                  opt.shard, opt.shards, openings.size(), opt.timeout_ms, opt.max_depth);
    print_report(t, opt, title);
    print_pairs(pairing_of(rows));
    std::printf("  本分片墙钟=%lld ms（%.1f ms/局）\n", spent,
                rows.empty() ? 0.0 : static_cast<double>(spent) / rows.size());
    return 0;
}

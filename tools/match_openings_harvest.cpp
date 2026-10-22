// 整树赛开局集的采集器（轮次171，方案 C 的样本量那一半）。
//
// 为什么要有这台仪器：赛具的读数按「有效样本」算力量，而**有效样本的单位是一条开局×座位这一对，
// 不是局**（轮次170 的 null 对照里，两台相同引擎逐局同解 ⇒ 同一条开局反复跑不加信息）。6 条开局
// 撑不起方案 C 的 ≈384，所以要按 docs/ENGINE_REDESIGN.md 保障 1 的口径「模板 + 若干中盘快照」扩集。
// 快照不手工挑，从 `gomoku_train --records` 写的对局记录里按固定手数取，这样开局集和引擎实际会
// 走到的形状是同分布的。
//
// 取出来的开局只收「偶数子、黑先行、盘面还活着」的三件条件，全部不满足的记录逐条计数并报出来 ——
// 赛具那边（tools/tree_match.cpp 的 load_openings）对奇数子开局是直接拒不收的，两头口径要一致。
#include "core/board.h"
#include "core/pattern.h"
#include "core/position_setup.h"
#include "core/types.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using nlohmann::json;
using namespace gomoku;

namespace {

struct Options {
    std::vector<std::string> records_paths;
    std::string out_path;
    std::vector<int> plies;
    int per_band = 8;
    bool verbose = false;
};

std::vector<int> parse_plies(const std::string& text) {
    std::vector<int> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const int v = std::atoi(item.c_str());
        if (v > 0 && v < 225) out.push_back(v);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// 一条记录里取出的候选开局。key 是「同一盘面」的判据：格子按 (r,c,color) 排序拼串，
// 与落子顺序无关 —— 换坐重下的两条线会落到同一个键上，那正是我们要合掉的东西。
struct Candidate {
    std::string key;
    std::string doc;
    int plies = 0;
    unsigned long long tie = 0;
};

unsigned long long mix(const std::string& s) {
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

struct Stats {
    long records = 0, games_bad = 0, ply_hits = 0, dead = 0, parity = 0, dup = 0, kept = 0;
    long no_install = 0;   // 赛具的 set_position 装不进去（position_from_moves 回了 error）
    std::string first_no_install;
};

// 把 moves 数组按顺序摆到盘上；返回 false 表示这条记录本身摆不出合法盘面。
bool replay(const json& moves, Board& board, std::vector<std::pair<int, int>>& order,
            std::vector<std::string>& colors) {
    for (const auto& entry : moves) {
        if (!entry.contains("row") || !entry.contains("col") || !entry.contains("color")) return false;
        const int row = entry["row"].get<int>();
        const int col = entry["col"].get<int>();
        const std::string color = entry["color"].get<std::string>();
        const int c = color == "black" ? BLACK : (color == "white" ? WHITE : EMPTY);
        if (c == EMPTY || !board.is_empty(row, col)) return false;
        if (!board.place_stone(row, col, c)) return false;
        order.emplace_back(row, col);
        colors.push_back(color);
    }
    return true;
}

std::string doc_of(const std::vector<std::pair<int, int>>& prefix) {
    json moves_arr = json::array();
    for (const auto& p : prefix) moves_arr.push_back({p.first, p.second});
    json doc;
    doc["version"] = 1;
    doc["type"] = "gomoku_position";
    doc["first"] = "black";
    doc["moves"] = moves_arr;
    return doc.dump();
}

// 反过来用同一道筛：把一份**已经写好**的开局档过一遍装载契约，报出装不进去的行。
// 采集时筛过一次，档就永远安全了吗 —— 记录会换、棋型识别会改版、档也会手工编辑。
// 所以把判据留成一条能重复跑的命令，而不是留成「当时采出来是干净的」这句话。
// 退出码 0 = 全部可装载，1 = 有装不进的（这样它能直接当门禁）。
int check_openings(const std::string& path) {
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "读不到 %s\n", path.c_str()); return 2; }
    std::string line;
    long idx = 0, bad = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const long row = idx++;
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded()) { std::printf("%ld 非 JSON\n", row); ++bad; continue; }
        if (!j.contains("moves") || !j["moves"].is_array() || j["moves"].empty()) continue;
        std::vector<Position> mv;
        for (const auto& p : j["moves"]) mv.emplace_back(p[0].get<int>(), p[1].get<int>());
        const SetupResult r = position_from_moves(mv, BLACK);
        if (!r.ok()) { std::printf("%ld %s\n", row, r.error.c_str()); ++bad; continue; }
        if (r.side_to_move != BLACK) { std::printf("%ld 轮到白（%zu 子）\n", row, mv.size()); ++bad; }
    }
    std::printf("开局档 %s：行=%ld 不可装=%ld\n", path.c_str(), idx, bad);
    return bad == 0 ? 0 : 1;
}

void harvest_path(const std::string& path, const Options& opt, std::vector<Candidate>& out,
                  std::set<std::string>& seen, Stats& st) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "读不到记录文件 %s\n", path.c_str());
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        ++st.records;
        json rec = json::parse(line, nullptr, false);
        if (rec.is_discarded() || !rec.contains("moves") || !rec["moves"].is_array()) {
            ++st.games_bad;
            continue;
        }
        Board board;
        std::vector<std::pair<int, int>> order;
        std::vector<std::string> colors;
        if (!replay(rec["moves"], board, order, colors)) { ++st.games_bad; continue; }

        for (int ply : opt.plies) {
            if (ply > static_cast<int>(order.size())) continue;
            ++st.ply_hits;
            if (ply % 2 != 0) { ++st.parity; continue; }
            // 黑先行的第 ply 手之后，最后一子是白 ⇒ 现在轮到黑。
            //
            // 这道筛直接用**赛具装机的那条路**（`position_from_moves`），不自己复刻棋规：
            // 快照文档写的是 `{"first":"black","moves":[[r,c]…]}`，服务器收到 set_position 就
            // 是把这份 moves 交给它。轮次177 的 null 里 211 对有 2 对整对作废，报错是
            // 「黑子 (5,6) 落点为禁手（三三 / 四四 / 长连）」，出自 `position_setup.cpp:538-551`
            // —— 它把每颗已落的黑子**拿掉、用剩下的全盘**问一次禁手，比「落子当时」严：
            // (5,6) 在前 18 子上合法，在这张 36 子快照上就不合法了。所以按当时的棋规筛会漏，
            // 按装载契约筛才和服务器判的是同一件事（判定只有一份真源）。
            std::vector<Position> install_moves;
            install_moves.reserve(static_cast<size_t>(ply));
            for (int i = 0; i < ply; ++i) install_moves.emplace_back(order[i].first, order[i].second);
            const SetupResult install = position_from_moves(install_moves, BLACK);
            if (!install.ok()) {
                ++st.no_install;
                if (st.first_no_install.empty()) st.first_no_install = install.error;
                continue;
            }
            // 轮次必须是黑 —— 赛具的循环固定从执黑那一侧读第一帧，轮到白会让它等一帧永远不来的 ai_move。
            if (install.side_to_move != BLACK || install.board.check_winner() != EMPTY) {
                ++st.dead;
                continue;
            }

            std::vector<std::string> cells;
            for (int i = 0; i < ply; ++i) {
                cells.push_back(std::to_string(order[i].first) + "," + std::to_string(order[i].second) +
                    ":" + colors[i]);
            }
            std::sort(cells.begin(), cells.end());
            std::string key;
            for (const auto& c : cells) key += c + ";";
            key += "|ply=" + std::to_string(ply);
            if (!seen.insert(key).second) { ++st.dup; continue; }

            std::vector<std::pair<int, int>> prefix(order.begin(), order.begin() + ply);
            Candidate cand;
            cand.key = key;
            cand.doc = doc_of(prefix);
            cand.plies = ply;
            cand.tie = mix(key);
            out.push_back(std::move(cand));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](std::string* out) {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s 缺参数\n", arg.c_str()); return false; }
            *out = argv[++i];
            return true;
        };
        std::string v;
        if (arg == "--records" && value(&v)) opt.records_paths.push_back(v);
        else if (arg == "--out" && value(&v)) opt.out_path = v;
        else if (arg == "--plies" && value(&v)) opt.plies = parse_plies(v);
        else if (arg == "--per-band") { if (!value(&v)) return 1; opt.per_band = std::atoi(v.c_str()); }
        else if (arg == "--verbose") opt.verbose = true;
        else if (arg == "--check" && value(&v)) {
            PatternRecognizer::init();
            return check_openings(v);
        }
        else if (arg == "--help" || arg == "-h") {
            std::printf("用法: %s --records FILE.jsonl [--records FILE.jsonl ...]\n"
                        "        [--plies 4,6,8,...] [--per-band N] [--out FILE.jsonl] [--verbose]\n"
                        "   或: %s --check 开局档.jsonl   （把装载契约当门禁跑：不可装 ⇒ 退出码 1）\n",
                        argv[0], argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "不认识的参数: %s\n", arg.c_str());
            return 1;
        }
    }
    if (opt.records_paths.empty()) { std::fprintf(stderr, "至少要一份 --records\n"); return 1; }
    if (opt.plies.empty()) opt.plies = parse_plies("4,6,8,10,12,14,16,18,20,24,28");
    // is_forbidden 靠这张表活着；不 init 就是「什么都不禁」，那这道筛就成了摆设。
    PatternRecognizer::init();

    std::vector<Candidate> all;
    std::set<std::string> seen;
    Stats st;
    for (const auto& p : opt.records_paths) harvest_path(p, opt, all, seen, st);

    // 每个手数档里按哈希取前 N 个：确定性的伪随机抽样，既不手工挑局面，也不让开局集被
    // 某一段连续的同源局面占满。
    std::vector<Candidate> kept;
    for (int ply : opt.plies) {
        std::vector<Candidate> band;
        for (auto& c : all) if (c.plies == ply) band.push_back(c);
        std::sort(band.begin(), band.end(), [](const Candidate& a, const Candidate& b) {
            return a.tie < b.tie;
        });
        if (opt.per_band > 0 && static_cast<int>(band.size()) > opt.per_band)
            band.resize(static_cast<size_t>(opt.per_band));
        kept.insert(kept.end(), band.begin(), band.end());
        if (opt.verbose) std::fprintf(stderr, "  档 %2d 子：候选 %zu 取 %zu\n", ply, band.size(), band.size());
    }
    st.kept = static_cast<long>(kept.size());

    std::ofstream out(opt.out_path);
    if (!out) { std::fprintf(stderr, "写不出 %s\n", opt.out_path.c_str()); return 2; }
    for (const auto& c : kept) out << c.doc << "\n";
    std::printf("记录=%ld 摆不出的局=%ld 命中档=%ld 奇数子拒收=%ld 装不进=%ld 已死盘=%ld 重复盘面=%ld ⇒ 开局=%ld 条\n",
                st.records, st.games_bad, st.ply_hits, st.parity, st.no_install, st.dead, st.dup, st.kept);
    if (opt.verbose && !st.first_no_install.empty())
        std::fprintf(stderr, "  第一条装不进的报错：%s\n", st.first_no_install.c_str());
    return 0;
}

// 戊-1（v2 §4.4）的验收仪器：一次「人类 think」的墙钟分布，四档各量一遍。
//
//   off    全停     —— 没有 broker，这一档就是劣化的分母。
//   full   全训     —— broker 按缺省配额把 reserved_cores 之后的核全占满。
//   half   半训     —— 配额砍一半。
//   yield  边下边训 —— broker 在跑，但每一次 think 之前发 `pause`、think 完发 `resume`
//                      （§4.4 的让位那一档；pause 的粒度是任务，所以在飞的那批不会立刻停手，
//                      这正是这一档要暴露的东西）。
//
// 判据（文档写的）：`yield` 档的 p99 相对 `off` 档劣化 ≤ 20 %；超过了说明绑核没生效，
// 不是「机器不够」。所以这台仪器把 think 钉在 reserved_cores 之前的核上（缺省 cpu 1），
// 而 worker 只拿 reserved_cores 之后的核（broker 的 `make_task` 就是这么分配的）。
//
// 一「次 think」= 引擎在一副普查棋盘上按面板那套预算（深度 + 墙钟预算）搜一次。
// 每档都新建一张置换表：TT 里带着上一档的热度比较墙钟是没有意义的。
//
// 用法（cwd 必须是 build 目录，因为这里按 `./gomoku_broker` 与 `./gomoku_train` 找进程）：
//   ./think_latency_probe [--poses N] [--depth D] [--timeout-ms T] [--pin CPU]
//                         [--root DIR] [--regimes off,full,half,yield] [--train-seconds S]
// 输出：每档一行 `n / p50 / p99 / mean / 训练 nps / spawned / requeued / failed`，
//       最后一行是判据的结论。
#include "core/board.h"
#include "core/types.h"
#include "search/search_engine.h"

#include "../tests/pinned_positions.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <spawn.h>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sched.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

struct Args {
    std::string root = "/tmp/gomoku_think_probe";
    std::string broker = "./gomoku_broker";
    std::string worker = "./gomoku_train";
    std::string regimes = "off,full,half,yield";
    int poses = 24;
    int depth = 6;
    int timeout_ms = 1200;
    int pin = 1;              // think 所在的核：必须落在 reserved_cores 之前
    int reserved_cores = 3;   // 轮次163 实测出来的缺省配额
    int train_seconds = 90;
};

int need_int(int& i, int argc, char** argv) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: %s 需要一个数值\n", argv[i]);
        std::exit(2);
    }
    return std::atoi(argv[++i]);
}

std::string need_str(int& i, int argc, char** argv) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: %s 需要一个值\n", argv[i]);
        std::exit(2);
    }
    return argv[++i];
}

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        if (f == "--poses") a.poses = need_int(i, argc, argv);
        else if (f == "--depth") a.depth = need_int(i, argc, argv);
        else if (f == "--timeout-ms") a.timeout_ms = need_int(i, argc, argv);
        else if (f == "--pin") a.pin = need_int(i, argc, argv);
        else if (f == "--reserved-cores") a.reserved_cores = need_int(i, argc, argv);
        else if (f == "--root") a.root = need_str(i, argc, argv);
        else if (f == "--broker") a.broker = need_str(i, argc, argv);
        else if (f == "--worker") a.worker = need_str(i, argc, argv);
        else if (f == "--regimes") a.regimes = need_str(i, argc, argv);
        else if (f == "--train-seconds") a.train_seconds = need_int(i, argc, argv);
        else {
            std::fprintf(stderr, "Error: 未知参数 %s\n", f.c_str());
            std::exit(2);
        }
    }
    if (a.poses <= 0 || a.depth <= 0 || a.timeout_ms <= 0) {
        std::fprintf(stderr, "--poses/--depth/--timeout-ms 必须都是正数\n");
        std::exit(2);
    }
    if (a.pin >= a.reserved_cores) {
        // 这条不是洁癖：think 一旦落在 worker 拿的那些核上，这一档测的就是「抢核」而不是「让位」，
        // 判据的结论会反过来教我们做一件错事。
        std::fprintf(stderr, "--pin 必须落在 reserved_cores(%d) 之前，否则这一档量不到绑核的效果\n",
                     a.reserved_cores);
        std::exit(2);
    }
    return a;
}

long long now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 最近秩（nearest-rank）：p99 取「至少有 99 % 的读数 ≤ 它」的那一个样本。
double percentile(std::vector<double> sorted, double p) {
    if (sorted.empty()) return 0.0;
    double rank = p / 100.0 * static_cast<double>(sorted.size());
    size_t index = static_cast<size_t>(rank);
    if (rank > static_cast<double>(index)) ++index;
    if (index == 0) index = 1;
    if (index > sorted.size()) index = sorted.size();
    return sorted[index - 1];
}

std::string read_file(const std::string& path) {
    std::ifstream file(path);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// 控制面客户端：发一行，能收到回话就收（收不到不是失败，`pause` 那类命令的回话很短）。
bool control(const Args& a, const std::string& line) {
    const std::string path = a.root + "/broker.sock";
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    bool ok = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
    if (ok) {
        const std::string payload = line + "\n";
        ok = ::send(fd, payload.data(), payload.size(), MSG_NOSIGNAL) ==
             static_cast<ssize_t>(payload.size());
        if (ok) {
            timeval timeout{};
            timeout.tv_sec = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            char buffer[2048];
            ::recv(fd, buffer, sizeof(buffer) - 1, 0);
        }
    }
    ::close(fd);
    return ok;
}

pid_t spawn_shell(const std::string& command) {
    pid_t pid = -1;
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    std::vector<char*> argv = {const_cast<char*>("/bin/sh"), const_cast<char*>("-c"),
                               const_cast<char*>(command.c_str()), nullptr};
    const int rc = posix_spawnp(&pid, "/bin/sh", nullptr, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr);
    return rc == 0 ? pid : -1;
}

int status_field(const Args& a, const std::string& key) {
    // 直接从控制面问一份 status，然后按键把那个整数抠出来。仪器不引 JSON 库，
    // 是为了这台仪器在树里只依赖引擎本身。
    const std::string out = a.root + "/probe_status.txt";
    const std::string command = a.broker + " --root '" + a.root + "' --send '{\"cmd\":\"status\"}' > '" +
                                out + "' 2>&1";
    if (std::system(command.c_str()) != 0) return -1;
    const std::string text = read_file(out);
    const size_t found = text.find("\"" + key + "\":");
    if (found == std::string::npos) return -1;
    return std::atoi(text.c_str() + found + key.size() + 3);
}

struct Row {
    std::string name;
    std::vector<double> samples_ms;
    double p50 = 0.0;
    double p99 = 0.0;
    double mean = 0.0;
    int train_nps = 0;
    int spawned = 0;
    int requeued = 0;
    int failed = 0;
};

Row run_regime(const Args& a, const std::string& name, const std::vector<gomoku::Board>& boards,
               int quota, bool yield) {
    Row row;
    row.name = name;
    std::error_code ec;
    pid_t broker = -1;
    if (quota > 0) {
        std::filesystem::remove_all(a.root, ec);
        std::filesystem::create_directories(a.root, ec);
        // 活要够多：整个 regime 期间 worker 都得有任务可跑，否则「全训」其实只训了一半时间。
        const std::string command = a.broker + " --root '" + a.root + "' --worker '" + a.worker +
                                    "' --quota " + std::to_string(quota) + " --reserved-cores " +
                                    std::to_string(a.reserved_cores) +
                                    " --tasks 2 --games-per-task 4 --depth 4 --max-nodes 1500 "
                                    "--seconds " +
                                    std::to_string(a.train_seconds) + " > '" + a.root + "/log.txt' 2>&1";
        broker = spawn_shell(command);
        // 等第一个 worker 真的跑起来：否则前面几次 think 量到的是「还没开始训」。
        for (int waited = 0; waited < 15000; waited += 100) {
            if (read_file(a.root + "/log.txt").find("spawned seq=") != std::string::npos) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } else {
        std::filesystem::remove_all(a.root, ec);
    }

    // 每档一张新表（同一颗帽、同一个深度、同一批局面，只有「旁边有没有训练在抢」这一件事不同）。
    gomoku::SearchEngine engine;
    std::atomic<bool> stop{false};
    for (const gomoku::Board& board : boards) {
        if (yield) control(a, "{\"cmd\":\"pause\"}");
        const long long t0 = now_us();
        const gomoku::SearchResult result = engine.search(board, gomoku::BLACK, a.depth, a.timeout_ms,
                                                            stop);
        const long long t1 = now_us();
        if (yield) control(a, "{\"cmd\":\"resume\"}");
        row.samples_ms.push_back(static_cast<double>(t1 - t0) / 1000.0);
        (void)result;
    }

    if (broker > 0) {
        row.train_nps = status_field(a, "nodes_per_search_second");
        row.spawned = status_field(a, "spawned");
        row.requeued = status_field(a, "requeued");
        row.failed = status_field(a, "failed");
        control(a, "{\"cmd\":\"stop\"}");
        int status = 0;
        for (int waited = 0; waited < 30000; waited += 100) {
            if (::waitpid(broker, &status, WNOHANG) == broker) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ::kill(-broker, SIGKILL);   // 收尾：不让 worker 活到下一档去污染分母
        ::waitpid(broker, &status, 0);
    }

    std::sort(row.samples_ms.begin(), row.samples_ms.end());
    row.p50 = percentile(row.samples_ms, 50.0);
    row.p99 = percentile(row.samples_ms, 99.0);
    double total = 0.0;
    for (double sample : row.samples_ms) total += sample;
    row.mean = row.samples_ms.empty() ? 0.0 : total / static_cast<double>(row.samples_ms.size());
    std::printf("probe  regime=%-6s n=%zu p50_ms=%.1f p99_ms=%.1f mean_ms=%.1f train_nps=%d "
                "spawned=%d requeued=%d failed=%d\n",
                row.name.c_str(), row.samples_ms.size(), row.p50, row.p99, row.mean, row.train_nps,
                row.spawned, row.requeued, row.failed);
    std::fflush(stdout);
    return row;
}

bool wants(const std::string& list, const std::string& name) {
    return list.find(name) != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
    const Args a = parse(argc, argv);
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(a.pin, &set);
    if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
        std::fprintf(stderr, "Error: 把 think 钉在 cpu %d 上失败了：%s\n", a.pin,
                     std::strerror(errno));
        return 2;
    }
    const std::vector<gomoku::Board> all = gomoku::pinned_census_boards();
    if (all.empty()) {
        std::fprintf(stderr, "Error: 普查棋盘是空的\n");
        return 2;
    }
    const size_t count = std::min<size_t>(static_cast<size_t>(a.poses), all.size());
    const std::vector<gomoku::Board> boards(all.begin(), all.begin() + static_cast<long>(count));

    std::printf("probe  pin=cpu%d depth=%d timeout_ms=%d poses=%zu reserved_cores=%d\n", a.pin,
                a.depth, a.timeout_ms, boards.size(), a.reserved_cores);
    std::fflush(stdout);

    Row off;
    Row yield_row;
    bool have_off = false;
    bool have_yield = false;
    // 配额按 broker 的公式自己算一遍，这样「全训」这一档就是它缺省会做的事，而不是仪器替它决定的。
    const long online = ::sysconf(_SC_NPROCESSORS_ONLN);
    const int full_quota =
        std::max(1, static_cast<int>(online > 0 ? online : 1) - a.reserved_cores);
    if (wants(a.regimes, "off")) {
        off = run_regime(a, "off", boards, 0, false);
        have_off = true;
    }
    if (wants(a.regimes, "full")) run_regime(a, "full", boards, full_quota, false);
    if (wants(a.regimes, "half")) run_regime(a, "half", boards, full_quota / 2 > 0 ? full_quota / 2 : 1, false);
    if (wants(a.regimes, "yield")) {
        yield_row = run_regime(a, "yield", boards, full_quota, true);
        have_yield = true;
    }

    if (have_off && have_yield) {
        const double ratio = off.p99 > 0.0 ? yield_row.p99 / off.p99 : 0.0;
        std::printf("verdict 边下边训 p99 / 全停 p99 = %.1f ms / %.1f ms = %.1f %%（判据 ≤ 120 %%）=> %s\n",
                    yield_row.p99, off.p99, ratio * 100.0,
                    ratio <= 1.20 ? "绑核生效" : "劣化超过判据，绑核没有生效");
        std::fflush(stdout);
        return ratio <= 1.20 ? 0 : 1;
    }
    return 0;
}

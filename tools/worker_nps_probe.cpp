// 戊-1（v2 §4 H 进程模型）的成本仪器：一台进程在这台机器上能跑多少节点/秒，同时开 2 / 4 份、
// 每份绑一个核之后每份还剩多少。验收第 1 条写的是「每 worker 的 nps 不低于单进程的 90 %」，
// 这一条先在**这台 6 核 VMware 虚机**上量出来——量不出来的东西不该先建 broker。
//
// 一台进程只做同一件事：在 42 副普查棋盘（`tests/pinned_positions.h`，与乙-1/乙-2 同一批）上，
// 按固定深度与固定节点帽各搜一次，重复 reps 轮，报自己的
// `pid / affinity / 总节点 / 墙钟 / nps / RSS / 答案校验和 / 停因分布`。并发由外面的脚本用
// `taskset` 起多份，本进程只报自己那一份的读数。
//
// 校验和、总节点数、深度与停因分布在任意臂之间必须逐字相同——帽 binding、时限只防死循环
//（§9.2：搜索类读数的超时就是节点帽）。这一条不是装饰：如果绑核或并发改变了答案，那「每份
// 都在做同一件事」这个前提就不成立，nps 的比也就没有意义。
//
// 用法：build/worker_nps_probe [--reps N] [--depth D] [--nodes CAP] [--pin CPU] [--tt-entries E] [--tag STR]
//   --tt-entries E  换一张 E 条目的置换表（0 = 引擎自己那张 2^18 ≈ 7.3 MB）。这一档改的是**工作本身**
//                   ——表小了截断就少，总节点数与校验和都会动，所以它只能用来量「并发掉的那几个点是
//                   不是表的容量」，同一行比较里 E 必须一样。
//   --pin CPU   用 `sched_setaffinity` 把自己绑到那一个核（外面也可以用 taskset 达到同样效果；
//               两条都留着是为了分清「绑核这件事由谁做」——broker 的 worker 是自己在启动时绑，
//               还是由 supervisor 用 taskset 拉起。v2 §4.2 说的是进程级绑核，所以自绑那条路
//               才是这一单元要覆盖的。）
#include "core/board.h"
#include "core/evaluator.h"
#include "core/pattern.h"
#include "core/types.h"
#include "optimization/transposition.h"
#include "search/search_engine.h"

#include "../tests/pinned_positions.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sched.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

struct Args {
    int reps = 1;
    int depth = 4;
    int nodes = 20000;
    int pin = -1;
    int tt_entries = 0;  // 0 = 用引擎自己那张默认表；>0 = 自己建一张这么大表（进程各一张）
    std::string tag = "probe";
};

int need_int(int& i, int argc, char** argv) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "%s 需要一个数值\n", argv[i]);
        std::exit(2);
    }
    return std::atoi(argv[++i]);
}

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        if (f == "--reps") a.reps = need_int(i, argc, argv);
        else if (f == "--depth") a.depth = need_int(i, argc, argv);
        else if (f == "--nodes") a.nodes = need_int(i, argc, argv);
        else if (f == "--tt-entries") a.tt_entries = need_int(i, argc, argv);
        else if (f == "--pin") a.pin = need_int(i, argc, argv);
        else if (f == "--tag") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--tag 需要一个字符串\n");
                std::exit(2);
            }
            a.tag = argv[++i];
        } else {
            std::fprintf(stderr, "未知参数 %s\n", f.c_str());
            std::exit(2);
        }
    }
    if (a.reps <= 0 || a.depth <= 0 || a.nodes <= 0) {
        std::fprintf(stderr, "--reps/--depth/--nodes 必须都是正数\n");
        std::exit(2);
    }
    return a;
}

// 现在真的能用到的核（绑完之后这就是一个数）。
std::string affinity_list() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return "?";
    std::string out;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &set)) continue;
        if (!out.empty()) out += ",";
        out += std::to_string(cpu);
    }
    return out;
}

long rss_kb() {
    std::ifstream file("/proc/self/statm");
    std::string total_pages, resident_pages;
    if (!(file >> total_pages >> resident_pages)) return -1;
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return -1;
    return std::strtol(resident_pages.c_str(), nullptr, 10) * page / 1024;
}

}  // namespace

int main(int argc, char** argv) {
    const Args a = parse(argc, argv);
    gomoku::PatternRecognizer::init();

    if (a.pin >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(a.pin, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0) {
            std::perror("sched_setaffinity");
            return 1;
        }
    }

    const std::vector<gomoku::Board> boards = gomoku::pinned_census_boards();
    if (boards.empty()) {
        std::fprintf(stderr, "普查棋盘集是空的\n");
        return 1;
    }

    // 表要么由引擎自己拥有（缺省 2^18 ≈ 7.3 MB），要么按 `--tt-entries` 换一张那么大的。
    // `shared = false`：一台 worker 进程只有一个线程，加分片锁就变成给这一臂自己添成本。
    std::unique_ptr<gomoku::TranspositionTable> owned_table;
    if (a.tt_entries > 0) {
        owned_table = std::make_unique<gomoku::TranspositionTable>(
            static_cast<size_t>(a.tt_entries), false);
    }
    const gomoku::ThreatCoefficients default_coeffs;
    std::unique_ptr<gomoku::SearchEngine> engine =
        owned_table ? std::make_unique<gomoku::SearchEngine>(default_coeffs, owned_table.get())
                    : std::make_unique<gomoku::SearchEngine>();
    engine->set_book_enabled(false);
    engine->set_max_nodes(a.nodes);
    std::atomic<bool> stop(false);

    long long total_nodes = 0;
    long long total_tt_hits = 0;
    unsigned long long checksum = 1469598103934665603ULL;  // FNV-1a 64 offset basis
    std::map<std::string, int> stop_reasons;
    std::map<int, int> depths;
    const auto t0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < a.reps; ++rep) {
        for (const gomoku::Board& board : boards) {
            const gomoku::SearchResult res =
                engine->search(board, gomoku::BLACK, a.depth, 60000, stop);
            total_nodes += res.nodes_searched;
            total_tt_hits += res.tt_hits;
            stop_reasons[res.stopped]++;
            depths[res.depth]++;
            checksum = (checksum ^ static_cast<unsigned long long>(res.best_move.pos.row)) *
                       1099511628211ULL;
            checksum = (checksum ^ static_cast<unsigned long long>(res.best_move.pos.col)) *
                       1099511628211ULL;
            checksum = (checksum ^ static_cast<unsigned long long>(
                                  static_cast<long long>(res.score) + 1000000000LL)) *
                       1099511628211ULL;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    const double nps = us > 0 ? static_cast<double>(total_nodes) * 1000000.0 / static_cast<double>(us)
                              : 0.0;

    std::printf("probe tag=%s pid=%ld pin=%d affinity=%s reps=%d boards=%zu depth=%d cap=%d tt=%d\n",
                a.tag.c_str(), static_cast<long>(getpid()), a.pin, affinity_list().c_str(), a.reps,
                boards.size(), a.depth, a.nodes, a.tt_entries);
    std::printf("probe  tag=%s nodes=%lld tt_hits=%lld elapsed_ms=%.1f nps=%.0f rss_kb=%ld "
                "checksum=%016llx\n",
                a.tag.c_str(), total_nodes, total_tt_hits, static_cast<double>(us) / 1000.0, nps,
                rss_kb(), checksum);
    for (const auto& entry : stop_reasons) {
        std::printf("probe  tag=%s stopped[%s]=%d\n", a.tag.c_str(), entry.first.c_str(),
                    entry.second);
    }
    for (const auto& entry : depths) {
        std::printf("probe  tag=%s reached_depth[%d]=%d\n", a.tag.c_str(), entry.first,
                    entry.second);
    }
    std::fflush(stdout);
    return 0;
}

// 丁-5 S1 本轮只交仪器与读数（用户裁决：先量再定）。
//
// 要回答的是这一单元的杀线与它的验收仪器：
//   杀线：「`swap` 下仍 > 60 % 开局一方通吃 → 这些开局本身就是必胜局 → 转 S2」；
//   反向证据：「适配度赛钉在 0.5 的比例不降反升」（轮次129 量到 32.5 % → 38.8 %）。
//
// 两个口径必须先分开，否则一个读数能被读成两种意思：
//   座位通吃 = 同一个**座位**赢下这一对两局 → 候选局分恰为 0.5，谁强都看不出来（这就是
//              「钉在 0.5」那一条）；
//   玩家通吃 = 同一个**玩家**赢下两局 → 局分 1.0 / 0.0，这才是判别力。
//
// 四个臂，同一批开局前缀（第 1、3 手由开场方落、第 2 手由另一方落 = swap2 的三面），
// 每对两局只是**谁开场**互换；判定档与轮次159 一致（depth 4、节点帽 1500/手、60 s 不
// binding、max_moves 60、认输开）：
//   A none/同系数   文档指定的对照组。
//   B swap/同系数   文档指定的仪器。三面之后由**非开场方**决定是否换色，依据是它自己那套
//                   系数下的 `evaluate_board(三面盘)`：评估认为黑位好就换到黑位。
//   C none/异强度   把 reference 那侧的 `rush_four` 打到 300（轮次159 实测过这一档能整对翻转）。
//   D swap/异强度   同一批弱侧开局 + 交换判决。这才是交换规则真正要用的场合：两个玩家不等强。
//
// 复现轮次160 那四个读数（约 25 分钟 / 288 局；每臂跑完即打一行汇总，可中断后用
// `--only A|B|C|D` 单独补一臂）：
//   cmake --build build --target opening_rule_screen -j4
//   ./build/opening_rule_screen --openings 48 --weak-openings 24
//
// 丁-6（S2 开局筛选）另用两类读数，同一台仪器：
//   `--repeat R`  同一局面对同一座位连下 R 局，报「结果是否每次都相同」。这一条量的是
//                 **筛选判据能不能成立**：指导书 §4 丁-6 思路 2 的判据是「双方胜率都落在
//                 [0.25, 0.75]」，而引擎是确定性的——同一局面、同一座位的 K 局若逐字相同，
//                 那 K 局的胜率只能是 0 或 1，那条判据永远点不着。先量这个，再谈换判据。
//   `--params-b P` 第二个玩家改为从参数文件读（默认那套 vs `data/training/best_params.json`
//                 = 树里现成的真实候选/冠军对），并汇总「非座位通吃」的开局数 = 按「换判据
//                 后」的口径能留下的开局数。
#include "core/evaluator.h"
#include "core/pattern.h"
#include "search/search_engine.h"
#include "training/self_play.h"
#include "training/trainer.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace gomoku;

namespace {

struct Args {
    int openings = 48;
    int weak_openings = 24;
    int stones = 3;
    int nodes = 1500;
    int depth = 4;
    int max_moves = 60;
    int repeat = 1;
    std::string only = "all";
    std::string params_b;   // 第二个玩家的系数来自参数文件（真实对）
};

int need(int& i, int argc, char** argv) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "%s 需要值\n", argv[i]);
        std::exit(2);
    }
    return std::atoi(argv[++i]);
}

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        if (f == "--openings") a.openings = need(i, argc, argv);
        else if (f == "--weak-openings") a.weak_openings = need(i, argc, argv);
        else if (f == "--stones") a.stones = need(i, argc, argv);
        else if (f == "--nodes") a.nodes = need(i, argc, argv);
        else if (f == "--depth") a.depth = need(i, argc, argv);
        else if (f == "--moves") a.max_moves = need(i, argc, argv);
        else if (f == "--only") a.only = argv[++i];
        else if (f == "--params-b") a.params_b = argv[++i];
        else if (f == "--repeat") a.repeat = need(i, argc, argv);
        else {
            std::fprintf(stderr, "未知参数 %s\n", f.c_str());
            std::exit(2);
        }
    }
    return a;
}

struct Counters {
    int pairs = 0;
    int games = 0;
    int seat_sweep = 0;     // 同座位赢两局 → 局分恰 0.5
    int player_sweep = 0;   // 同玩家赢两局 → 局分 1.0 / 0.0
    int undecided = 0;      // 至少一局未分胜负
    int swapped_pairs = 0;  // 两局里至少发生过一次交换
    int mixed_decision = 0; // 两局的交换判决不一致（座位互换后本该相反）
    long long plies = 0;
};

// 一局：`cand_opens` = 候选方摆第 1、3 手。返回值以**座位**计：+1 黑胜、-1 白胜、0 未分、2 作废。
int play_one(bool cand_opens, bool swap, const CoefficientPair& cand, const CoefficientPair& ref,
             const std::vector<Move>& opening, const SelfPlayConfig& settings, int& plies) {
    // 开场方默认坐黑；交换后开场方坐白。
    const bool cand_is_black = swap ? !cand_opens : cand_opens;
    SearchEngine black(cand_is_black ? cand : ref);
    SearchEngine white(cand_is_black ? ref : cand);
    black.set_book_enabled(false);
    white.set_book_enabled(false);
    SelfPlay play(black, white, settings);
    std::atomic<bool> stop(false);
    const GameRecord rec = play.run(opening, nullptr, &stop);
    plies += static_cast<int>(rec.moves.size());
    if (rec.reason == GameEndReason::INVALID) return 2;
    if (rec.winner == BLACK) return 1;
    if (rec.winner == WHITE) return -1;
    return 0;
}

void run_arm(char tag, const Args& a, int openings, bool use_swap, bool weak_ref,
             const CoefficientPair& cand, const CoefficientPair& ref, const SelfPlayConfig& settings,
             Counters& c) {
    std::printf("=== arm %c: swap=%d weak_ref=%d openings=%d\n", tag, use_swap ? 1 : 0,
                weak_ref ? 1 : 0, openings);
    std::fflush(stdout);
    for (int i = 0; i < openings; ++i) {
        const std::vector<Move> opening = Trainer::random_opening_line(a.stones, 1000 + i);
        if (static_cast<int>(opening.size()) < a.stones) {
            std::printf("%d, SHORT(%zu)\n", i, opening.size());
            continue;
        }
        Board prefix;
        bool ok = true;
        for (const Move& m : opening) {
            if (!prefix.place_stone(m.pos.row, m.pos.col, m.color)) ok = false;
        }
        if (!ok) {
            std::printf("%d, BADPREFIX\n", i);
            continue;
        }

        // 交换判决：**非开场方**用自己的系数评三面盘，评估说黑位好就换到黑位。
        // 第 1 局非开场方是 ref，第 2 局非开场方是 cand。
        const Evaluator ref_eval(ref);
        const Evaluator cand_eval(cand);
        const bool swap1 = use_swap && ref_eval.evaluate_board(prefix, BLACK) > 0;
        const bool swap2 = use_swap && cand_eval.evaluate_board(prefix, BLACK) > 0;

        int plies = 0;
        const int g1 = play_one(true, swap1, cand, ref, opening, settings, plies);
        const int g2 = play_one(false, swap2, cand, ref, opening, settings, plies);
        if (g1 == 2 || g2 == 2) continue;
        ++c.pairs;
        c.games += 2;
        c.plies += plies;
        if (swap1 || swap2) ++c.swapped_pairs;
        if (swap1 != swap2) ++c.mixed_decision;
        if (g1 == 0 || g2 == 0) ++c.undecided;
        if (g1 != 0 && g1 == g2) ++c.seat_sweep;

        // 玩家口径：g1 候选坐（swap1 ? 白 : 黑），g2 候选坐（swap2 ? 黑 : 白）。
        const int cand_g1 = (!swap1) ? g1 : -g1;
        const int cand_g2 = (swap2) ? g2 : -g2;
        if (cand_g1 != 0 && cand_g1 == cand_g2) ++c.player_sweep;

        std::printf("%d, swap(%d,%d), seat(%d,%d), cand(%d,%d)\n", i, swap1 ? 1 : 0,
                    swap2 ? 1 : 0, g1, g2, cand_g1, cand_g2);
        std::fflush(stdout);
    }
    std::printf("# arm %c: 对=%d 局=%d 平均手数=%.1f | 座位通吃=%d (%.1f%%) 玩家通吃=%d (%.1f%%)"
                " | 换过座的对=%d (%.1f%%) 两局判决不同=%d | 含未分=%d\n",
                tag, c.pairs, c.games, c.games ? c.plies / (double)c.games : 0.0, c.seat_sweep,
                c.pairs ? 100.0 * c.seat_sweep / c.pairs : 0.0, c.player_sweep,
                c.pairs ? 100.0 * c.player_sweep / c.pairs : 0.0, c.swapped_pairs,
                c.pairs ? 100.0 * c.swapped_pairs / c.pairs : 0.0, c.mixed_decision, c.undecided);
    std::fflush(stdout);
}

bool wanted(const Args& a, char tag) {
    return a.only == "all" || (a.only.size() == 1 && a.only[0] == tag);
}

// 一个前缀在两局里各自被重复 R 次：确定性引擎下同一座位的 R 局必须逐字相同，
// 这条量的是「同一局面同座位的 K 局能不能凑出 0.25~0.75 之间的胜率」——若 R 局全同，
// 每色的 K 局胜率只能是 0 或 1，指导书 §4 丁-6 思路 2 那条判据就永远点不着。
void run_determinism(const Args& a, const CoefficientPair& cand, const CoefficientPair& ref,
                     const SelfPlayConfig& settings) {
    std::printf("=== determinism: openings=%d repeat=%d\n", a.openings, a.repeat);
    int repeats = 0, identical = 0, differing = 0;
    for (int i = 0; i < a.openings; ++i) {
        const std::vector<Move> opening = Trainer::random_opening_line(a.stones, 1000 + i);
        if (static_cast<int>(opening.size()) < a.stones) continue;
        Board prefix;
        bool ok = true;
        for (const Move& m : opening) {
            if (!prefix.place_stone(m.pos.row, m.pos.col, m.color)) ok = false;
        }
        if (!ok) continue;
        for (int g = 0; g < 2; ++g) {
            std::vector<std::string> signatures;
            for (int r = 0; r < a.repeat; ++r) {
                SearchEngine black(g == 0 ? cand : ref);
                SearchEngine white(g == 0 ? ref : cand);
                black.set_book_enabled(false);
                white.set_book_enabled(false);
                SelfPlay play(black, white, settings);
                std::atomic<bool> stop(false);
                const GameRecord rec = play.run(opening, nullptr, &stop);
                std::string sig = std::to_string(static_cast<int>(rec.winner)) + "/"
                                + std::to_string(rec.moves.size());
                for (const MoveRecord& mr : rec.moves) {
                    sig += " " + std::to_string(mr.move.pos.row) + ","
                         + std::to_string(mr.move.pos.col);
                }
                signatures.push_back(std::move(sig));
                ++repeats;
            }
            bool same = true;
            for (size_t s = 1; s < signatures.size(); ++s) {
                if (signatures[s] != signatures[0]) same = false;
            }
            if (same) ++identical;
            else {
                ++differing;
                std::printf("%d seat%d DIFFERS: %s | %s\n", i, g, signatures[0].c_str(),
                            signatures[1].c_str());
            }
        }
        std::printf("# 开局 %d: 两座位各 %d 次重复，逐字相同的座位组数累计 %d/%d\n", i, a.repeat,
                    identical, repeats / 2);
        std::fflush(stdout);
    }
    std::printf("# determinism: 重复局数=%d 同座位组全部相同=%d 出现不同=%d ⇒ 同座位的 K 局"
                "给出的是同一个数，每色胜率只能取 0 或 1\n",
                repeats, identical, differing);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    const Args a = parse(argc, argv);
    PatternRecognizer::init();
    const CoefficientPair cand;                 // 缺省那一套，两侧同值
    CoefficientPair ref = cand;
    ref.white.rush_four = 300;                  // 弱的一侧：冲四从 1000 打到 300

    SelfPlayConfig settings;
    settings.max_depth = a.depth;
    settings.timeout_ms = 60000;
    settings.max_nodes = a.nodes;
    settings.max_moves = a.max_moves;

    Counters c_a, c_b, c_c, c_d, c_e;
    if (wanted(a, 'A')) run_arm('A', a, a.openings, false, false, cand, cand, settings, c_a);
    if (wanted(a, 'B')) run_arm('B', a, a.openings, true, false, cand, cand, settings, c_b);
    if (wanted(a, 'C')) run_arm('C', a, a.weak_openings, false, true, cand, ref, settings, c_c);
    if (wanted(a, 'D')) run_arm('D', a, a.weak_openings, true, true, cand, ref, settings, c_d);
    // E 臂：把「合成弱侧」换成树里现成的**真实**一对（缺省系数 vs `--params-b` 读来的
    // 那套）。丁-6 的筛集判据要在这对上量才有意义。
    if (wanted(a, 'E')) {
        if (a.params_b.empty()) {
            std::printf("=== arm E skipped: 需要 --params-b <文件>\n");
        } else {
            CoefficientPair loaded;
            std::string error;
            if (!CoefficientVector::load(a.params_b, loaded, error)) {
                std::fprintf(stderr, "--params-b 读不进来: %s\n", error.c_str());
                return 2;
            }
            std::printf("=== arm E 用的第二套系数来自 %s\n", a.params_b.c_str());
            run_arm('E', a, a.weak_openings, false, false, cand, loaded, settings, c_e);
        }
    }
    if (a.repeat > 1) run_determinism(a, cand, cand, settings);
    std::printf("# 杀线口径：座位通吃 > 60 %% → 这些开局本身就是必胜局，换规则不解决问题 → 转 S2\n");
    std::printf("# 反向证据口径：玩家通吃降而座位通吃升 = 局分被钉到 0.5，判别力反而更差\n");
    return 0;
}

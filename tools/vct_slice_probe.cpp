// VCT 的切片烧法与归属探针（轮次181 的读数出处，登记在 REVIEW_LOG 轮次181）。
//
// 问的是两件事，都必须量而不是推测：
//   1)「关掉 VCT 把时间还给 AlphaBeta」这一臂的收益上限。上限写在产品代码里：
//      search_engine.cpp:297-300 用 elapsed_since_start() 从 time_limit_ms 里减出
//      remaining_time ⇒ VCT 烧掉多少毫秒就是 AB 少拿多少毫秒。上限若是几十毫秒，
//      这一臂直接结案，不必花一次赛（一晚上 3 次 × 35 min）。
//   2) VCT 交出的那手棋到底是不是它的。这一问必须走真实次序：VCT 前面压着
//      forced(:144) / greedy_five(:162) / must_block(:205) / book(:219) / vcf(:258)，
//      把它单独拎出来跑会把它「本来轮不到答」的臂也算成它的贡献（本轮第一版就是这么错的）。
//
// 定价照抄线上那一档：max_nodes_==0 且 time_limit_ms>0 ⇒ VCT 拿
//   deadline = search_start + 30 % × timeout_ms（默认 16 000 ms ⇒ 4 800 ms），
//   node_limit_ 保持它自己的默认 500 000（代码在这条分支上不调 set_node_limit）。
// 门也照抄：threat_probe().own_three 决定这一次思考到底进不进 VCT —— 用的是同一个函数，
// 不是我对它的复刻。
//
// 只读工具：不改任何产品行为，读的是 VCTSearch 已有的旋钮与它自己的节点计数
//（src/search/vct.h 上那个 nodes_searched() 就是为它留的观察面）。WIN 的复核另走一条
// 独立算法（自己扫 225 个格，数我方与对面各自的「下一手成五」点），不复用 VCT 的防守
// 枚举，所以它能判 VCT 而不被 VCT 判 —— 但也别忘了一条判据只覆盖它自己数的那件事：
// 只数我方剩余成五点会漏掉「对面先把五做成」，本轮就因此把 28 个伪报读成过「全部成立」。
//
// 跑法：./build/vct_slice_probe --engine-order --out R.csv
//       [--openings FILE] [--slice-ms N] [--node-limit N] [--depth N]
//       [--time-limit-ms N] [--verbose]
// 缺省就是线上那一档（切片 4800 ms / 帽 500 000 / 深度 12 / 16 000 ms / tools/match_openings.jsonl）。
// 逐臂明细在 --out 的 CSV：win_blocks<0 的那些就是「输在赛跑上」的伪报。
#include "core/evaluator.h"
#include "core/pattern.h"
#include "core/position_setup.h"
#include "core/types.h"
#include "search/move_generator.h"
#include "search/search_engine.h"
#include "search/vct.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace gomoku;
using nlohmann::json;

namespace {

struct Options {
    std::string openings = "tools/match_openings.jsonl";
    std::string out;
    int slice_ms = 4800;      // 30 % × 线上默认 timeout_ms=16000
    int node_limit = 500000;  // VCTSearch 自己的默认帽
    int depth = 12;           // vct_max_depth_ 的默认
    int time_limit_ms = 16000;  // 线上默认 timeout_ms（param_schema.cpp:23）
    bool engine_order = false;
    bool verbose = false;
    int rebuild_plies = 0;    // >0 ⇒ 对非 0 节点的 WIN 重建一条 forcing 线（#84 gap②）
};

struct Arm {
    std::string opening;
    int color = BLACK;
    bool legal_side = true;
    bool gated = false;      // threat_probe().own_three ⇒ 线上会进 VCT
    long long ms = 0;
    int nodes = 0;
    const char* result = "skip";
    bool hit_node_cap = false;
    bool hit_deadline = false;
    // WIN 的独立复核（见 audit_win）
    char win_tag = '-';
    int win_blocks = -1;
    int win_row = -1, win_col = -1;
    // 这一句「我赢」是从根扫描哪一条分支回来的（见 root_branch）；'X' = 认不出且节点>0
    char win_branch = '-';
    std::string win_pv;
    // 重放这条开局用的子序列与先手（#84 gap② 的 forcing 线重建要用，不能等 run_arm 之后再摆）
    std::vector<Position> stones;
    int first_color = BLACK;
    // VCT 声称的着法**落下之前**，对面已经攥着几个成五点（见 five_points_of）
    int race_before = 0;
    // 按真实次序（快路径 → VCF → VCT，AlphaBeta 关掉）走一遍完整思考的归属
    std::string eng_method;
    long long eng_ms = 0;
    int eng_score = 0;
    int eng_row = -1, eng_col = -1;
};

// #84 欠的那一格归因：VCT 的根扫描有四条分支会当场交出 WIN，判据逐字照 `src/search/vct.cpp`
// 里那条次序摆：本手成五 'F' → 摆完之后自己有一条活四 'L' → four_count>=2 'D' → 防守表为空 'N'。
// 第四条只能排除法得到：`VCTSearch::generate_defenses` 是私有的，探针调不到它。
// 「0 节点 ⇔ 根扫描答的」这条**捷径是错的**，一次带标签的实验把它证伪了（六条白侧臂里三条被标成
// 递归的其实是根上的 'L' —— 根扫描会先把前面的候选着法送进递归，节点数于是先涨了，走到后面某一
// 手才命中 'L' 并返回）。所以 F/L/D 三条按判据直接认，认不出来的（本应 'N'）在节点>0 时**与真递归
// 分不开**，标成 'X'；只有节点=0 的才敢写死 'N'。要精确到那一层得给 vct.cpp 加一个归因出口，
// 本轮不加（它没有任何读者，而这一层的分辨力对本次的结论不是必需的）。
char root_branch(const Board& board, int color, int row, int col) {
    if (row < 0 || col < 0 || !board.is_empty(row, col)) return '?';
    Board scratch = board;
    if (!scratch.place_stone(row, col, color)) return '?';
    const bool five = scratch.has_five_in_a_row(row, col, color);
    const PositionPattern p =
        PatternRecognizer::recognize_position(scratch, row, col, color);
    scratch.undo_last_move();
    if (five) return 'F';
    for (int d = 0; d < 4; ++d) {
        if (p.directions[d].type == PatternType::LIVE_FOUR) return 'L';
    }
    if (p.four_count >= 2) return 'D';
    return 'N';
}

// 此刻这张盘上，`who` 一方还有几个「下一手成五」的点。audit_win 是在 VCT 声称的着法**之后**
// 数这个数的；这里要在它落子**之前**数一次，因为递归证明（'V' 那一支）根本不经过根的四条分支，
// 拿「之后」那个数没法问「对面本来是不是已经攥着胜着」。
int five_points_of(const Board& board, int who) {
    Board scratch = board;
    int count = 0;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            if (!scratch.is_empty(r, c)) continue;
            if (!scratch.place_stone(r, c, who)) continue;
            if (scratch.has_five_in_a_row(r, c, who)) ++count;
            scratch.undo_last_move();
        }
    }
    return count;
}

// 独立复核 VCT 的一句「我赢」：把它给的着法摆上，只看两个硬事实 ——
//   1) 摆完之后进攻方还剩几个「下一手成五」的点 S；
//   2) 同一张盘面上，防守方自己有几个「下一手成五」的点 R（VCT 的根扫描不看这一项，
//      它的 generate_defenses 只枚举「挡我这条威胁」的点，对面在别处已经能成五它不知道）。
// 编码进 blocks_out：本手直接成五写 5（标签 'F'）；否则 R>=1 写 -(R+1)（赛跑先输），
// 否则写 S。判读：S>=2 且 R==0 ⇒ 这个 WIN 与 VCT 的枚举无关，是真的；S<=1 或 R>=1 ⇒
// 它那句「没有防守」是防守枚举漏了，不是局面如此。
// 'X' = 黑侧这个点是禁手（VCT 的 generate_attacks 本该滤掉，出现即产品缺陷）。'O' = 点已被占。
char audit_win(Board& board, int color, int row, int col, int* blocks_out) {
    *blocks_out = -1;
    if (row < 0 || col < 0 || !board.is_empty(row, col)) return 'O';
    if (color == BLACK && PatternRecognizer::is_forbidden(board, row, col, BLACK)) return 'X';
    if (!board.place_stone(row, col, color)) return 'O';
    const int defender = color == BLACK ? WHITE : BLACK;
    const bool five_now = board.has_five_in_a_row(row, col, color);
    int completions = 0;
    // 对面还剩几个「下一手成五」的点：一个都不剩才算这个 WIN 赢在赛跑上。
    // VCT 的根扫描只看自己那条线（它的 generate_defenses 只枚举「挡我这条威胁」的点），
    // 对面自己在别处 already 有成五点这件事它不看 —— 那类局面 must_block 快路径会先接走。
    int racer = 0;
    if (!five_now) {
        for (int r = 0; r < BOARD_SIZE; ++r) {
            for (int c = 0; c < BOARD_SIZE; ++c) {
                if (!board.is_empty(r, c)) continue;
                if (!board.place_stone(r, c, color)) continue;
                if (board.has_five_in_a_row(r, c, color)) ++completions;
                board.undo_last_move();
            }
        }
        for (int r = 0; r < BOARD_SIZE; ++r) {
            for (int c = 0; c < BOARD_SIZE; ++c) {
                if (!board.is_empty(r, c)) continue;
                if (!board.place_stone(r, c, defender)) continue;
                if (board.has_five_in_a_row(r, c, defender)) ++racer;
                board.undo_last_move();
            }
        }
    }
    board.undo_last_move();
    *blocks_out = five_now ? 5 : (racer > 0 ? -1 - racer : completions);
    return five_now ? 'F' : 'S';
}

void report_audit(const std::string& name, const std::vector<Arm>& arms) {
    std::map<char, int> tag_count;
    std::vector<std::string> suspect, deep;
    int win_arms = 0, zero_node = 0, recursive = 0, fabricated = 0;
    int sound_multi = 0, blocked_one = 0, lost_race = 0, five_now = 0;
    for (const Arm& a : arms) {
        if (std::string(a.result) != "WIN") continue;
        ++win_arms;
        ++tag_count[a.win_tag];
        if (a.win_tag == 'F') ++five_now;
        else if (a.win_blocks < 0) ++lost_race;
        else if (a.win_blocks >= 2) ++sound_multi;
        else ++blocked_one;
        // 只有 0 节点回来的 WIN 才归因得到「本手成五 / 本手之后还剩几个成五点」：
        // 递归证明的第一手本来就不该立刻露出成五点，那类臂交给逐手复验，不在这里定罪。
        if (a.nodes == 0) {
            ++zero_node;
            if (a.win_tag == 'S' && (a.win_blocks < 0 || a.win_blocks <= 1)) {
                ++fabricated;
                suspect.push_back(a.opening + (a.color == BLACK ? " B" : " W") + " (" +
                    std::to_string(a.win_row) + "," + std::to_string(a.win_col) +
                    ") 我方可成五点=" + std::to_string(a.win_blocks) +
                    (a.win_blocks < 0 ? "（对面抢先把五做成）" : "（对面一子就挡住）"));
            }
        } else {
            ++recursive;
            deep.push_back(a.opening + (a.color == BLACK ? " B" : " W") + " (" +
                std::to_string(a.win_row) + "," + std::to_string(a.win_col) +
                ") 节点=" + std::to_string(a.nodes) + " 支=" + a.win_branch +
                " 本手后可成五点=" + std::to_string(a.win_blocks) +
                " 落子前对面已有成五点=" + std::to_string(a.race_before) +
                " 变例=" + (a.win_pv.empty() ? std::string("(空)") : a.win_pv));
        }
    }
    std::printf("\n-- %s 的 WIN 复核 --\n", name.c_str());
    std::printf("  WIN=%d（根扫描 0 节点=%d，进了递归=%d）\n", win_arms, zero_node, recursive);
    std::printf("  复核：本手成五='F'=%d  本手后看威胁='S'=%d  黑侧禁手='X'=%d  点已占='O'=%d\n",
                tag_count['F'], tag_count['S'], tag_count['X'], tag_count['O']);
    std::printf("    落子后「自己剩 >=2 个成五点且对面没有成五点」=%d  「对面一子挡住(<=1)」=%d  "
                "「对面在别处已有成五点 ⇒ 这个 WIN 输在赛跑上」=%d  「本手直接成五」=%d\n",
                sound_multi, blocked_one, lost_race, five_now);
    std::printf("  根扫描里「对面一子就挡得住」的（=VCT 判 WIN 是防守枚举漏了）：%d/%d\n",
                fabricated, zero_node);
    // 分支归因（#84 欠的那一格）：哪一支交回的 WIN，各支的复核结论怎么分布。
    struct BranchRow { int total = 0; int five = 0; int sound = 0; int blocked = 0; int race = 0;
                       int held_before = 0; };
    std::map<char, BranchRow> by_branch;
    for (const Arm& a : arms) {
        if (std::string(a.result) != "WIN") continue;
        BranchRow& row = by_branch[a.win_branch];
        ++row.total;
        if (a.race_before > 0) ++row.held_before;
        if (a.win_tag == 'F') ++row.five;
        else if (a.win_blocks < 0) ++row.race;
        else if (a.win_blocks >= 2) ++row.sound;
        else ++row.blocked;
    }
    std::printf("  分支归因（F=本手成五 L=活四 D=双四 N=防守表空(0 节点) X=认不出 F/L/D 且节点>0：防守表空或递归证明，两者分不开）\n");
    std::printf("    支    合计   本手成五   >=2成五点且对面没有   对面一子挡住   对面先把五做成   落子前对面已有成五点\n");
    for (const char k : {'F', 'L', 'D', 'N', 'X', '?'}) {
        const auto it = by_branch.find(k);
        if (it == by_branch.end()) continue;
        const BranchRow& r = it->second;
        std::printf("    %c  %6d %10d %18d %13d %14d %14d\n",
                    k, r.total, r.five, r.sound, r.blocked, r.race, r.held_before);
    }
    for (size_t i = 0; i < suspect.size() && i < 12; ++i)
        std::printf("    %s\n", suspect[i].c_str());
    std::printf("  非 0 节点交回的 WIN（递归证明，或后面的候选才命中根分支 —— 见分支列）：%zu\n", deep.size());
    for (size_t i = 0; i < deep.size() && i < 24; ++i)
        std::printf("    %s\n", deep[i].c_str());
}

long long percentile(std::vector<long long> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

void report(const std::string& name, const std::vector<Arm>& arms, int slice_ms,
            int node_limit_for_note) {
    std::vector<Arm> gated_arms;
    for (const Arm& a : arms)
        if (a.gated) gated_arms.push_back(a);
    std::vector<long long> ms_gated;
    for (const Arm& a : gated_arms) ms_gated.push_back(a.ms);

    long long sum_gated = 0, sum_unknown = 0;
    long long cap_nodes = 0, cap_ms = 0;
    int unknown_arms = 0;
    int cap_hits = 0, deadline_hits = 0, wins = 0, over_1000 = 0, over_200 = 0;
    for (const Arm& a : gated_arms) {
        sum_gated += a.ms;
        if (std::string(a.result) == "UNKNOWN") {
            sum_unknown += a.ms;
            ++unknown_arms;
        }
        if (a.hit_node_cap) {
            ++cap_hits;
            cap_nodes += a.nodes;
            cap_ms += a.ms;
        }
        if (a.hit_deadline) ++deadline_hits;
        if (std::string(a.result) == "WIN") ++wins;
        if (a.ms > 1000) ++over_1000;
        if (a.ms > 200) ++over_200;
    }
    const size_t n = gated_arms.size();
    std::printf("\n== %s ==\n", name.c_str());
    std::printf("臂=%zu  进门(own_three)=%zu (%.1f%%)  VCT判WIN=%d\n",
                arms.size(), n, arms.empty() ? 0.0 : 100.0 * static_cast<double>(n) / arms.size(), wins);
    if (n == 0) {
        std::printf("  没有一臂进 VCT ⇒ 线上这一档它根本不点火，无时间可回收。\n");
        return;
    }
    std::printf("  进门臂烧的墙钟 ms：均值=%.0f  p50=%lld  p75=%lld  p90=%lld  p99=%lld  最大=%lld\n",
                static_cast<double>(sum_gated) / static_cast<double>(n),
                percentile(ms_gated, 0.50), percentile(ms_gated, 0.75),
                percentile(ms_gated, 0.90), percentile(ms_gated, 0.99),
                *std::max_element(ms_gated.begin(), ms_gated.end()));
    std::printf("  烧法：>200 ms=%d/%zu  >1000 ms=%d/%zu  撞墙钟 deadline=%d  撞节点帽=%d\n",
                over_200, n, over_1000, n, deadline_hits, cap_hits);
    // 撞帽的那些臂同时给出 VCT 的吞吐，是「先撞哪个限」的直接证据。
    if (cap_nodes > 0 && cap_ms > 0) {
        const double nps = 1000.0 * static_cast<double>(cap_nodes) / static_cast<double>(cap_ms);
        std::printf("  撞帽臂吞吐≈%.0f 节点/秒（%lld 节点 / %lld ms）⇒ 跑满 %d 节点帽约需 %.2f s\n",
                    nps, cap_nodes, cap_ms, node_limit_for_note,
                    static_cast<double>(node_limit_for_note) / nps);
    }
    // 「关掉 VCT 能还给 AlphaBeta 多少时间」只算没答出来的那些臂：判 WIN 的臂当场返回，
    // 整手预算都是省下的，不是烧掉的。
    std::printf("  可回收上限（只算 UNKNOWN 臂）：%d 臂合计 %.1f s，摊到每一手 = 16000 ms 预算的 %.2f%%"
                "（= 切片 %d ms 的 %.2f%%）\n",
                unknown_arms, static_cast<double>(sum_unknown) / 1000.0,
                100.0 * static_cast<double>(sum_unknown) /
                    (static_cast<double>(arms.size()) * 16000.0),
                slice_ms,
                100.0 * static_cast<double>(sum_unknown) /
                    (static_cast<double>(arms.size()) * static_cast<double>(slice_ms)));
}

// 真实次序的归属表：孤立地跑 VCT 会把它「本来轮不到答」的臂也算成它的贡献，
// 因为快路径和 VCF 在它前面就会把同样的胜着拿走。这张表把两者对上。
void report_engine_order(const std::string& name, const std::vector<Arm>& arms) {
    std::map<std::string, int> by_method;
    std::map<std::string, long long> ms_by_method;
    for (const Arm& a : arms) {
        if (a.eng_method.empty()) continue;
        ++by_method[a.eng_method];
        ms_by_method[a.eng_method] += a.eng_ms;
    }
    std::printf("\n== %s 按真实次序（快路径 → VCF → VCT，AB 关）的归属 ==\n", name.c_str());
    for (const auto& kv : by_method) {
        std::printf("  %-12s 臂=%3d (%.1f%%)  均值=%lld ms\n", kv.first.c_str(), kv.second,
                    100.0 * static_cast<double>(kv.second) / static_cast<double>(arms.size()),
                    ms_by_method[kv.first] / std::max(1, kv.second));
    }
    // 交叉：VCT 单独跑判 WIN 的那些臂，在真实次序里被谁拿走、还是真由 vct 交出的。
    int vct_win = 0, still_vct = 0, taken_by_vcf = 0, taken_by_fast = 0, fell_through = 0;
    for (const Arm& a : arms) {
        if (std::string(a.result) != "WIN") continue;
        ++vct_win;
        if (a.eng_method == "vct") ++still_vct;
        else if (a.eng_method == "vcf") ++taken_by_vcf;
        else if (a.eng_method == "greedy" || a.eng_method == "alphabeta" ||
                 a.eng_method == "no_legal_move") ++fell_through;
        else ++taken_by_fast;
    }
    std::printf("  交叉：VCT 单独判 WIN 的 %d 臂里，真实次序仍由 vct 交出=%d，"
                "被 VCF 先拿走=%d，被快路径拿走=%d，战术层没答（兜底）=%d\n",
                vct_win, still_vct, taken_by_vcf, taken_by_fast, fell_through);
    int answered = 0, same_move = 0;
    for (const Arm& a : arms) {
        if (std::string(a.result) != "WIN") continue;
        ++answered;
        if (a.eng_row == a.win_row && a.eng_col == a.win_col) ++same_move;
    }
    std::printf("  同一手：这 %d 臂里真实次序交出**同一颗子**的=%d\n", answered, same_move);
}

// 复刻线上那一条分支的定价：不调 set_node_limit 之外的东西，deadline 用同一套毫秒。
void run_arm(Arm& arm, const Board& board, const Evaluator& ev, TranspositionTable& tt,
             const Options& opt) {
    std::atomic<bool> stop{false};
    if (!arm.gated) {
        arm.result = "gated-out";
        return;
    }
    VCTSearch vct(ev, tt);
    vct.set_node_limit(opt.node_limit);
    const auto start = std::chrono::steady_clock::now();
    vct.set_deadline(start + std::chrono::milliseconds(opt.slice_ms));
    VCTMove r = vct.search(board, arm.color, opt.depth, stop);
    const auto end = std::chrono::steady_clock::now();
    arm.ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    arm.nodes = vct.nodes_searched();
    arm.result = r.result == VCTResult::WIN ? "WIN"
                 : r.result == VCTResult::LOSS ? "LOSS" : "UNKNOWN";
    arm.hit_node_cap = arm.nodes > opt.node_limit;
    arm.hit_deadline = std::chrono::steady_clock::now() >=
                       start + std::chrono::milliseconds(opt.slice_ms);
    if (r.result == VCTResult::WIN) {
        arm.win_row = r.best_move.pos.row;
        arm.win_col = r.best_move.pos.col;
        Board scratch = board;
        arm.win_tag = audit_win(scratch, arm.color, arm.win_row, arm.win_col, &arm.win_blocks);
        const char pred = root_branch(board, arm.color, arm.win_row, arm.win_col);
        arm.win_branch = pred != 'N' ? pred : (arm.nodes == 0 ? 'N' : 'X');
        arm.race_before = five_points_of(board, arm.color == BLACK ? WHITE : BLACK);
        for (const Move& m : r.variation) {
            if (!m.pos.is_valid()) continue;
            // 分隔符不能用逗号：这一列直接写进 CSV。
            arm.win_pv += "(" + std::to_string(m.pos.row) + ":" + std::to_string(m.pos.col) +
                          (m.color == BLACK ? "B)" : "W)");
        }
    }
}

// 真实次序那一遍：快路径（forced / greedy_five / must_block / book）→ VCF → VCT，
// 只把 AlphaBeta 关掉。关 AB 不影响「谁在战术层先答出来」，而这一臂要问的正是后者；
// 一个都没答的臂会落到 greedy 兜底，那正好就是「战术层对这手没有归属」。
void run_engine_order(Arm& arm, const Board& board, SearchEngine& eng, const Options& opt) {
    std::atomic<bool> stop{false};
    const auto start = std::chrono::steady_clock::now();
    SearchResult r = eng.search(board, arm.color, 8, opt.time_limit_ms, stop);
    const auto end = std::chrono::steady_clock::now();
    arm.eng_method = r.method;
    arm.eng_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    arm.eng_score = r.score;
    arm.eng_row = r.best_move.pos.row;
    arm.eng_col = r.best_move.pos.col;
}

std::vector<Position> moves_of(const json& rec) {
    std::vector<Position> out;
    if (!rec.contains("moves")) return out;
    for (const auto& m : rec["moves"]) {
        if (m.is_array() && m.size() >= 2) out.emplace_back(m[0].get<int>(), m[1].get<int>());
    }
    return out;
}

// #84 的 gap②：递归证明交回来的 WIN 在树里**不带变例**（`vct.cpp` 那条 `best_score >= 100000`
// 的出口只填 best_move），所以「逐手复验」在库里没有可依之物。这一台重建器不读 VCT 的任何内部
// 状态，只把开局序列摆回去、按局面自己找一条**forcing 线**：进攻方只走「落子后自己的成五点比
// 落子前多 ⇒ 这一手是把四做出来」的着法；防守方要么挡住刚露出的那个点，要么自己成五；两边都查
// 黑侧禁手。找得到一条真的胜线 ⇒ VCT 那句「我赢」成立；找不到 ⇒ 这个 WIN 至少不是forcing的，
// 判不出真赢。「找不到」不等于「证伪」（真正的forcing线可能第一步不当令），那一行要照实写。
struct ForcingLine {
    bool found = false;
    std::string line;
    std::string stall;
};

int five_points_into(const Board& board, int color, Position* out, int cap) {
    int n = 0;
    for (const Position& p : board.active_empty()) {
        Board scratch = board;
        if (!scratch.place_stone(p.row, p.col, color)) continue;
        if (!scratch.has_five_in_a_row(p.row, p.col, color)) continue;
        if (out && n < cap) out[n] = p;
        ++n;
    }
    return n;
}

bool forcing_search(Board& board, int attacker, int ply, int max_plies, std::string& line) {
    if (ply > max_plies) return false;
    const int defender = attacker == BLACK ? WHITE : BLACK;

    Position before[8];
    const int before_count = five_points_into(board, attacker, before, 8);

    for (const Position& p : board.active_empty()) {
        if (!board.place_stone(p.row, p.col, attacker)) continue;
        if (board.check_winner() == attacker) {
            line += "(" + std::to_string(p.row) + ":" + std::to_string(p.col) +
                    (attacker == BLACK ? "B 成五)" : "W 成五)");
            board.undo_last_move();
            return true;
        }
        Position after[8];
        const int after_count = five_points_into(board, attacker, after, 8);
        if (after_count <= before_count) {  // 这一手没做出新的四，不是 forcing 的
            board.undo_last_move();
            continue;
        }
        if (ply + 1 > max_plies) {
            board.undo_last_move();
            continue;
        }
        bool all_blocks_lose = true;
        std::string this_line;
        for (int i = 0; i < after_count && all_blocks_lose; ++i) {
            Board next = board;
            if (!next.place_stone(after[i].row, after[i].col, defender)) continue;
            std::string tail;
            if (next.check_winner() == defender ||
                !forcing_search(next, attacker, ply + 1, max_plies, tail)) {
                all_blocks_lose = false;
            } else {
                this_line += "(" + std::to_string(p.row) + ":" + std::to_string(p.col) +
                             (attacker == BLACK ? "B)" : "W)") +
                             "(" + std::to_string(after[i].row) + ":" +
                             std::to_string(after[i].col) +
                             (defender == BLACK ? "B)" : "W)") + tail;
            }
        }
        if (all_blocks_lose) {
            line = this_line;
            board.undo_last_move();
            return true;
        }
        board.undo_last_move();
    }
    return false;
}

ForcingLine rebuild_forcing_line(const std::vector<Position>& stones, int first_color,
                                 int attacker, int max_plies) {
    ForcingLine out;
    Board board;
    int side = first_color;
    for (const Position& p : stones) {
        if (!board.place_stone(p.row, p.col, side)) {
            out.stall = "开局序列装不下";
            return out;
        }
        side = side == BLACK ? WHITE : BLACK;
    }
    std::string line;
    out.found = forcing_search(board, attacker, 0, max_plies, line);
    out.line = line;
    return out;
}

// 非 0 节点交回来的那几条 WIN 的真伪重建（#84 gap②）。库不给变例，这里用只读重建顶上去。
void report_rebuild(const std::string& name, const std::vector<Arm>& arms, int max_plies) {
    int checked = 0, found = 0;
    for (const Arm& a : arms) {
        if (std::string(a.result) != "WIN" || a.nodes == 0) continue;
        ++checked;
        const ForcingLine line =
            rebuild_forcing_line(a.stones, a.first_color, a.color, max_plies);
        if (line.found) ++found;
        const std::string detail = line.found ? (" 线=" + line.line)
                                              : std::string("（不等于证伪，见 rebuild 上的说明）");
        std::printf("  %s %c (%d,%d) 节点=%d 我方成五点=%d ⇒ forcing 重建：%s%s\n",
                    a.opening.c_str(), a.color == BLACK ? 'B' : 'W',
                    a.win_row, a.win_col, a.nodes, a.win_blocks,
                    line.found ? "找得到一条真的胜线" : "找不到", detail.c_str());
    }
    std::printf("-- %s 的 WIN 逐手重建：查 %d 条（只查非 0 节点那批），找到 %d，进攻深度上限 %d 手 --\n",
                name.c_str(), checked, found, max_plies);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* dflt) {
            return i + 1 < argc ? std::string(argv[++i]) : std::string(dflt);
        };
        if (a == "--openings") opt.openings = next(opt.openings.c_str());
        else if (a == "--slice-ms") opt.slice_ms = std::atoi(next("4800").c_str());
        else if (a == "--node-limit") opt.node_limit = std::atoi(next("500000").c_str());
        else if (a == "--depth") opt.depth = std::atoi(next("12").c_str());
        else if (a == "--time-limit-ms") opt.time_limit_ms = std::atoi(next("16000").c_str());
        else if (a == "--engine-order") opt.engine_order = true;
        else if (a == "--out") opt.out = next("");
        else if (a == "--verbose") opt.verbose = true;
        else if (a == "--rebuild-forcing") opt.rebuild_plies = std::atoi(next("4").c_str());
        else {
            std::printf("用法: %s [--openings F] [--slice-ms N] [--node-limit N] [--depth N]"
                        " [--time-limit-ms N] [--engine-order] [--rebuild-forcing N] [--out F]"
                        " [--verbose]\n", argv[0]);
            return 2;
        }
    }

    PatternRecognizer::init();
    Evaluator ev;
    // 表大小 = 产品默认（TranspositionTable 的 num_entries 缺省 2^18，search_engine
    // 自己持有的那张就是它）；一份表贯穿全部臂 ⇒ 比每手开局时的新表偏乐观，
    // 这个误差方向记在这里，读数时按「VCT 的下界」解释。
    TranspositionTable tt;
    // --engine-order 那一遍用的是**真引擎**（默认系数 = 线上缺省档），只关掉 AB 与开局库：
    // 库是每进程抽签的（opening_book.h:57），留着它归属就不确定。
    SearchEngine eng;
    eng.set_alphabeta_enabled(false);
    eng.set_book_enabled(false);

    std::ifstream in(opt.openings);
    if (!in) {
        std::fprintf(stderr, "打不开开局集 %s\n", opt.openings.c_str());
        return 1;
    }

    std::vector<Arm> black_legal, white_mirror, all;
    std::string line;
    int idx = 0, installed = 0, dead = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        json rec;
        try {
            rec = json::parse(line);
        } catch (...) {
            ++idx;
            continue;
        }
        std::vector<Position> mv = moves_of(rec);
        const int first = rec.value("first", std::string("black")) == "white" ? WHITE : BLACK;
        Board board;
        int side = first;
        if (!mv.empty()) {
            SetupResult s = position_from_moves(mv, first);
            if (!s.ok() || s.board.check_winner() != EMPTY) {
                ++dead;
                continue;
            }
            board = s.board;
            side = s.side_to_move;
        }
        ++installed;

        for (int color : {BLACK, WHITE}) {
            Arm arm;
            arm.opening = "#" + std::to_string(idx);
            arm.color = color;
            arm.legal_side = (color == side);
            arm.first_color = first;
            arm.stones = mv;
            arm.gated = threat_probe(board, color).own_three;
            run_arm(arm, board, ev, tt, opt);
            if (opt.engine_order) run_engine_order(arm, board, eng, opt);
            all.push_back(arm);
            if (color == BLACK) black_legal.push_back(arm);
            else white_mirror.push_back(arm);
            if (opt.verbose && arm.gated) {
                std::printf("%s %c %s nodes=%d ms=%lld\n", arm.opening.c_str(),
                            color == BLACK ? 'B' : 'W', arm.result, arm.nodes, arm.ms);
            }
        }
        ++idx;
    }

    std::printf("开局集=%s  装机成功=%d  装不进/已分胜负=%d  切片=%d ms  节点帽=%d  深度=%d\n",
                opt.openings.c_str(), installed, dead, opt.slice_ms, opt.node_limit, opt.depth);
    report("执黑侧（开局集的自然轮次，棋形与赛具读到的一致）", black_legal, opt.slice_ms, opt.node_limit);
    report("执白侧（同一盘面换个轮次问 —— 只为采样白侧的烧法，不是合法臂）", white_mirror, opt.slice_ms, opt.node_limit);
    report_audit("执黑侧", black_legal);
    report_audit("执白侧", white_mirror);
    if (opt.rebuild_plies > 0) {
        report_rebuild("执黑侧", black_legal, opt.rebuild_plies);
        report_rebuild("执白侧", white_mirror, opt.rebuild_plies);
    }
    if (opt.engine_order) {
        report_engine_order("执黑侧", black_legal);
        report_engine_order("执白侧", white_mirror);
    }

    if (!opt.out.empty()) {
        std::ofstream o(opt.out);
        o << "opening,color,gated,ms,nodes,result,hit_cap,hit_deadline,win_tag,win_blocks,win_row,win_col,"
             "win_branch,win_pv,race_before,eng_method,eng_ms,eng_score,eng_row,eng_col\n";
        for (const Arm& a : all) {
            o << a.opening << ',' << (a.color == BLACK ? 'B' : 'W') << ',' << (a.gated ? 1 : 0)
              << ',' << a.ms << ',' << a.nodes << ',' << a.result << ','
              << (a.hit_node_cap ? 1 : 0) << ',' << (a.hit_deadline ? 1 : 0) << ','
              << a.win_tag << ',' << a.win_blocks << ',' << a.win_row << ',' << a.win_col << ','
              << a.win_branch << ',' << a.win_pv << ',' << a.race_before << ','
              << a.eng_method << ',' << a.eng_ms << ',' << a.eng_score << ','
              << a.eng_row << ',' << a.eng_col << '\n';
        }
    }
    return 0;
}

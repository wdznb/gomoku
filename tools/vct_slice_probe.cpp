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
    // 按真实次序（快路径 → VCF → VCT，AlphaBeta 关掉）走一遍完整思考的归属
    std::string eng_method;
    long long eng_ms = 0;
    int eng_score = 0;
    int eng_row = -1, eng_col = -1;
};

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
                ") 节点=" + std::to_string(a.nodes) + " 本手后可成五点=" + std::to_string(a.win_blocks));
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
    for (size_t i = 0; i < suspect.size() && i < 12; ++i)
        std::printf("    %s\n", suspect[i].c_str());
    std::printf("  递归证明的 WIN（本判据只能确认它「不是立即可见」，真伪要逐手复验）：%zu\n", deep.size());
    for (size_t i = 0; i < deep.size() && i < 8; ++i)
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
        else {
            std::printf("用法: %s [--openings F] [--slice-ms N] [--node-limit N] [--depth N]"
                        " [--time-limit-ms N] [--engine-order] [--out F] [--verbose]\n", argv[0]);
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
    if (opt.engine_order) {
        report_engine_order("执黑侧", black_legal);
        report_engine_order("执白侧", white_mirror);
    }

    if (!opt.out.empty()) {
        std::ofstream o(opt.out);
        o << "opening,color,gated,ms,nodes,result,hit_cap,hit_deadline,win_tag,win_blocks,win_row,win_col,"
             "eng_method,eng_ms,eng_score,eng_row,eng_col\n";
        for (const Arm& a : all) {
            o << a.opening << ',' << (a.color == BLACK ? 'B' : 'W') << ',' << (a.gated ? 1 : 0)
              << ',' << a.ms << ',' << a.nodes << ',' << a.result << ','
              << (a.hit_node_cap ? 1 : 0) << ',' << (a.hit_deadline ? 1 : 0) << ','
              << a.win_tag << ',' << a.win_blocks << ',' << a.win_row << ',' << a.win_col << ','
              << a.eng_method << ',' << a.eng_ms << ',' << a.eng_score << ','
              << a.eng_row << ',' << a.eng_col << '\n';
        }
    }
    return 0;
}

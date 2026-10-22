// 「新根降一层」探针（轮次169 三份读数的出处，登记在 REVIEW_LOG 轮次169）：
// 只读 —— 它不改 src/，也不改任何产品行为，改的只是「下一次思考要求几层」。
//
// 跑法：./build/new_root_depth_probe [--pose N] [--depth D] [--od N] [--ocap N]
//         [--no-cold] [--no-warm] [--no-ponder] [--no-oracle]
//
// 三个读数：
//   1) 上一手在深度 D 付过钱之后，新根（我方落子 + 对手应手 = 两子之后）再要深度 D，
//      热表还剩多少用（对照：同一个新根、空表，要 D 花多少）；
//   2) 若新根只要求 D-1 / D-2，节点数塌到什么程度，答案（着法/分值/证明层）变不变；
//   3) 变了的那些是不是变弱：用 exact_root_score 在更深一层（D+2）给每个候选着法定价。
//
// 盘面 = tests/test_pinned.cpp 的四条钉臂（quiet/pos24 × 两色，d6 自然结束于
// 221774/156428/300347/175088 节点），所以「上一手」的重建与门禁那四个数逐字同源。
// 全部按节点定价（帽 8,000,000，四条 d6 臂的自然结束都在它以下 ⇒ 墙不参与判定）。
// 热表不可克隆 ⇒ 每条热臂都从头重放一遍上一手，重放结果与第一次逐字比对（不确定就报警）。
#include "core/board.h"
#include "core/types.h"
#include "search/search_engine.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace gomoku;

namespace {

constexpr int kStageCap = 8000000;
constexpr int kStageWallMs = 600000;

struct Pose {
    const char* name;
    std::vector<std::array<int, 3>> stones;   // {row, col, color}
    int color;                                 // 根上轮到谁下
};

Board build(const std::vector<std::array<int, 3>>& stones) {
    Board board;
    for (const auto& s : stones) board.place_stone(s[0], s[1], s[2]);
    return board;
}

std::vector<std::array<int, 3>> quiet() {
    return {{7, 7, BLACK}, {7, 8, WHITE}, {8, 8, BLACK}, {6, 6, WHITE}};
}

std::vector<std::array<int, 3>> pos24() {
    return {{9, 11, BLACK}, {0, 14, WHITE}, {10, 2, BLACK},  {6, 6, WHITE},
            {10, 4, BLACK}, {9, 2, WHITE},  {4, 2, BLACK},   {5, 3, WHITE},
            {2, 3, BLACK},  {10, 1, WHITE}, {12, 12, BLACK}, {2, 6, WHITE},
            {1, 8, BLACK},  {14, 8, WHITE}, {4, 1, BLACK},   {13, 4, WHITE},
            {1, 14, BLACK}, {8, 11, WHITE}, {10, 10, BLACK}, {8, 7, WHITE},
            {12, 1, BLACK}, {11, 6, WHITE}, {5, 2, BLACK},   {0, 7, WHITE}};
}

const std::vector<Pose>& poses() {
    static const std::vector<Pose> kAll = {
        {"quiet BLACK", quiet(), BLACK},
        {"quiet WHITE", quiet(), WHITE},
        {"pos24 BLACK", pos24(), BLACK},
        {"pos24 WHITE", pos24(), WHITE},
    };
    return kAll;
}

struct Read {
    bool ok = false;
    long long nodes = 0;
    int score = 0, depth = 0, partial = 0, tt_hits = 0, seldepth = 0;
    Move mv;
    std::string method, stopped;
    std::vector<Move> pv;
};

// 一次完整思考。hint 不接线是故意的：它只改排序（轮次141 量到 ≈1.13x），
// 接进来会把「深度」的功劳和「排序」的功劳混进同一格。
Read think(SearchEngine& engine, const Board& board, int color, int depth) {
    std::atomic<bool> stop(false);
    engine.set_max_nodes(kStageCap);
    engine.clear_hint();
    const SearchResult r = engine.search(board, color, depth, kStageWallMs, stop);
    Read out;
    out.ok = r.best_move.pos.is_valid();
    out.nodes = r.nodes_searched;
    out.score = r.score;
    out.depth = r.depth;
    out.partial = r.partial_depth;
    out.tt_hits = r.tt_hits;
    out.seldepth = r.seldepth;
    out.mv = r.best_move;
    out.method = r.method;
    out.stopped = r.stopped;
    out.pv = r.principal_variation;
    return out;
}

struct Exchange {
    Board new_root;
    Board pred_root;      // 上一手自己预告的那条线（P + 我方子 + PV 第二步），可能无效
    bool has_pred = false;
    Move mine, reply;
    Read first;      // 上一手那一次思考的读数
    bool ok = false;
};

// 在调用方给的引擎上重放「上一手」：深度 D 思考根 P，落我方答案 + 对手应手两子。
// 引擎留在调用方手里 ⇒ 表是热的。variant 0 = PV 第二步（引擎自己预告的应手），
// 1 = 对手用自己的引擎在 P+我方子 上选的最强应手（预判之外的局面）。
Exchange replay(const Pose& p, int d, int variant, SearchEngine& warm) {
    Exchange ex;
    const Board start = build(p.stones);
    ex.first = think(warm, start, p.color, d);
    if (!ex.first.ok) return ex;
    ex.mine = ex.first.mv;
    warm.transposition_table().advance_generation();   // 生产口径：落一子走一格龄

    Board after_me = start;
    if (!after_me.place_stone(ex.mine.pos.row, ex.mine.pos.col, ex.mine.color)) {
        ex.first.ok = false;
        return ex;
    }

    Move reply;
    if (variant == 0) {
        if (ex.first.pv.size() < 2) { ex.first.ok = false; return ex; }
        reply = ex.first.pv[1];
    } else {
        SearchEngine opp;
        const Read r = think(opp, after_me, opponent(p.color), d);
        if (!r.ok) { ex.first.ok = false; return ex; }
        reply = r.mv;
    }
    if (reply.color == EMPTY) reply.color = opponent(p.color);
    ex.reply = reply;

    // 预告盘面 = PV 那条线（预搜要下的根）。variant 0 时它与新根是同一个盘面。
    if (ex.first.pv.size() >= 2) {
        const Move pv_reply = ex.first.pv[1];
        Board pred = after_me;
        Move coloured = pv_reply;
        if (coloured.color == EMPTY) coloured.color = opponent(p.color);
        if (pred.place_stone(coloured.pos.row, coloured.pos.col, coloured.color)) {
            ex.pred_root = pred;
            ex.has_pred = true;
        }
    }

    warm.transposition_table().advance_generation();
    if (!after_me.place_stone(reply.pos.row, reply.pos.col, reply.color)) {
        ex.first.ok = false;
        return ex;
    }
    ex.new_root = after_me;
    ex.ok = true;
    return ex;
}

std::string str(const Move& m) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "(%d,%d)", m.pos.row, m.pos.col);
    return buf;
}

void line(const std::string& tag, const Read& r) {
    char pad[64];
    std::snprintf(pad, sizeof pad, "%-26s", tag.c_str());
    if (!r.ok) {
        std::printf("%s —— 没有答案\n", pad);
        return;
    }
    std::printf("%s nodes=%-8lld tt_hits=%-8d score=%-8d 层=%d/%d sel=%-3d %-7s %s/%s\n",
                pad, r.nodes, r.tt_hits, r.score, r.depth, r.partial, r.seldepth,
                str(r.mv).c_str(), r.method.c_str(), r.stopped.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    int only_pose = -1, depth = 6, oracle_depth = 8, ocap = 2000000;
    bool do_cold = true, do_warm = true, do_oracle = true, do_ponder = true;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--pose") && i + 1 < argc) only_pose = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--depth") && i + 1 < argc) depth = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--od") && i + 1 < argc) oracle_depth = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--ocap") && i + 1 < argc) ocap = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-cold")) do_cold = false;
        else if (!std::strcmp(argv[i], "--no-warm")) do_warm = false;
        else if (!std::strcmp(argv[i], "--no-ponder")) do_ponder = false;
        else if (!std::strcmp(argv[i], "--no-oracle")) do_oracle = false;
        else {
            std::printf("用法: %s [--pose N] [--depth D] [--od N] [--ocap N]"
                        " [--no-cold] [--no-warm] [--no-ponder] [--no-oracle]\n", argv[0]);
            return 1;
        }
    }

    std::printf("# 新根降一层探针  D=%d  阶段帽=%d  定价深度=%d 定价帽=%d\n",
                depth, kStageCap, oracle_depth, ocap);

    for (size_t pi = 0; pi < poses().size(); ++pi) {
        if (only_pose >= 0 && static_cast<int>(pi) != only_pose) continue;
        const Pose& p = poses()[pi];
        std::printf("\n## %s（根上轮到 %s，上一手按 d%d 付过钱）\n", p.name,
                    p.color == BLACK ? "BLACK" : "WHITE", depth);

        for (int variant = 0; variant < 2; ++variant) {
            SearchEngine geom;
            const Exchange ex = replay(p, depth, variant, geom);
            std::printf("\n- 应手来源=%s  上一手 %s 对手应 %s\n",
                        variant == 0 ? "PV 预告" : "对手自选",
                        str(ex.mine).c_str(), ex.ok ? str(ex.reply).c_str() : "(无)");
            if (!ex.ok) { std::printf("  （这条盘面对不上，跳过）\n"); continue; }
            std::printf("  新根手数 move_count=%d（根 %zu 子 + 我方 1 + 对手 1）\n",
                        ex.new_root.move_count(), p.stones.size());
            line("  上一手（重建用）", ex.first);

            std::vector<Move> cands;
            for (int drop = 0; drop <= 2; ++drop) {
                const int ask = depth - drop;
                if (ask < 1) continue;
                char t[32];
                std::snprintf(t, sizeof t, "热·要 d%d", ask);
                if (do_warm) {
                    SearchEngine warm;
                    const Exchange again = replay(p, depth, variant, warm);
                    if (!again.ok || !(again.mine == ex.mine) || !(again.reply == ex.reply)) {
                        std::printf("  ！重放不确定：%s %s vs %s %s —— 这格作废\n",
                                    str(again.mine).c_str(), str(again.reply).c_str(),
                                    str(ex.mine).c_str(), str(ex.reply).c_str());
                        continue;
                    }
                    const Read r = think(warm, ex.new_root, p.color, ask);
                    line(t, r);
                    if (r.ok) {
                        bool seen = false;
                        for (const Move& m : cands) if (m == r.mv) seen = true;
                        if (!seen) cands.push_back(r.mv);
                    }
                }
                if (do_cold) {
                    SearchEngine fresh;
                    std::string ct = std::string("冷·要 d") + std::to_string(ask);
                    line(ct, think(fresh, ex.new_root, p.color, ask));
                }
            }

            // 对照臂：不降深度，改成「空闲时按预告预搜一手」（§2.4.3 ②，线上默认关）。
            // 预搜的帽子用生产那条公式（think_nodes × percent/100，地板 4000、天花板 200000），
            // 然后下一手仍要**全深度 d6** —— 这是「省节点但不肯少算一层」的那一半。
            if (do_ponder) {
                for (int pct : {25, 50}) {
                    SearchEngine warm;
                    const Exchange again = replay(p, depth, variant, warm);
                    if (!again.ok || !again.has_pred) {
                        std::printf("  预搜@%d%%           —— 这条没有 PV 第二步，跳过\n", pct);
                        continue;
                    }
                    long long budget = static_cast<long long>(again.first.nodes) * pct / 100;
                    if (budget < 4000) budget = 4000;
                    if (budget > 200000) budget = 200000;
                    std::atomic<bool> stop(false);
                    const SearchResult pd = warm.ponder_search(again.pred_root, p.color, depth,
                                                               static_cast<int>(budget), stop);
                    char t[64];
                    std::snprintf(t, sizeof t, "预%d%%+要 d6", pct);
                    const Read r = think(warm, ex.new_root, p.color, depth);
                    line(t, r);
                    const bool guessed = ex.has_pred &&
                        again.pred_root.zobrist_hash() == ex.new_root.zobrist_hash();
                    std::printf("    （预搜自身 nodes=%lld 层=%d 停=%s；猜中对手应手=%s；"
                                "预搜+下一手合计=%lld）\n",
                                static_cast<long long>(pd.nodes_searched), pd.depth,
                                pd.stopped.c_str(), guessed ? "是" : "否",
                                static_cast<long long>(pd.nodes_searched) + r.nodes);
                    if (r.ok) {
                        bool seen = false;
                        for (const Move& m : cands) if (m == r.mv) seen = true;
                        if (!seen) cands.push_back(r.mv);
                    }
                }
            }

            if (!do_warm || !do_oracle) continue;
            // 定价：各条热臂的着法各问一次更深一层的精确分，一次一问、一台新引擎。
            if (cands.size() <= 1) {
                std::printf("  定价：各臂同一个着法 %s ⇒ 无价差可测\n",
                            cands.empty() ? "(无)" : str(cands[0]).c_str());
                continue;
            }
            std::printf("  定价（d%d 参考下的根着法精确分，帽 %d）：\n", oracle_depth, ocap);
            for (size_t ci = 0; ci < cands.size(); ++ci) {
                SearchEngine fresh;
                std::atomic<bool> stop(false);
                fresh.set_max_nodes(ocap);
                const int s = fresh.exact_root_score(ex.new_root, p.color, cands[ci],
                                                     oracle_depth, ocap, stop);
                std::printf("    着法 %-8s -> %d\n", str(cands[ci]).c_str(), s);
            }
        }
    }
    return 0;
}

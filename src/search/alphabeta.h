#ifndef GOMOKU_ALPHABETA_H
#define GOMOKU_ALPHABETA_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "../core/pattern.h"
#include "../optimization/transposition.h"
#include "move_generator.h"
#include "search_limits.h"
#include "search_state.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <vector>

namespace gomoku {

struct SearchInfo {
    int depth = 0;
    int nodes_searched = 0;
    int score = 0;
    Move best_move;
    std::vector<Move> principal_variation;
    // §6.3.1 的根着法表。SearchInfo 是 SearchStats 的镜像，所以这里也要有它，
    // 否则 §6.10 的落点在 to_info 处断掉（第一次接线就是这么断的）。
    std::vector<RootMoveScore> root_scores;
    int elapsed_ms = 0;
    // What a hand that ran out of budget looks like. One layer completing is the
    // normal case, and then partial_depth stays 0; when it did not, depth is the
    // last layer that ran to the end, partial_depth the one that was cut, and
    // root_moves_done/total say how far into that layer the cut landed.
    // stopped says why (design doc 2.3.3: an interrupted think must not be
    // reported as a fast path that never searched).
    int partial_depth = 0;
    int root_moves_done = 0;
    int root_moves_total = 0;
    int seldepth = 0;
    // The last field design doc 2.9 :443 promises this row: how many nodes read
    // something out of the table. `int` rather than the counter's own `uint64_t`
    // because a hit is only recorded at a node the search visited, so
    // `tt_hits <= nodes_searched` - the same bound this struct already relies on
    // when it narrows the node count at alphabeta.cpp:480.
    int tt_hits = 0;
    Move partial_best_move;
    StopReason stopped = StopReason::NONE;
};

// A node's value plus whether that value means anything. A score that came out of
// a budget short-circuit is a static evaluation of one position, not a bound on
// the subtree, and the parent must know the difference: it is the difference
// between "this search wrote nothing" (the old single aborted_ flag, which also
// discarded the work of every subtree that had finished) and "this node's own
// value is not a bound, but its finished children still are".
struct NodeResult {
    int score = 0;
    bool sound = true;
};

class AlphaBetaSearch {
public:
    AlphaBetaSearch(const Evaluator& evaluator, TranspositionTable& tt);

    // The legacy shape: this phase's share of the budget in milliseconds, plus an
    // optional absolute deadline, plus the node ceiling from set_node_limit(). It
    // builds a SearchLimits and runs the same layered search as the entry point
    // below, so both report the same accounting.
    Move search(const Board& board, int color, int max_depth,
                int time_limit_ms, std::atomic<bool>& stop_flag);

    // Limits of one think, including the budget split and the soft window that
    // decides whether to start another layer. Returns the move to play; the
    // accounting is in last_stats() / last_search_info().
    Move search(const Board& board, int color, const SearchLimits& limits,
                std::atomic<bool>& stop_flag);

    SearchInfo last_search_info() const { return info_; }
    SearchStats last_stats() const { return stats_; }

    void set_node_limit(int limit) { node_limit_ = limit; }

    // One frame per layer, complete or cut short, so a long think does not go
    // silent between start and return and the client can show which layer the
    // interrupt landed in.
    void set_progress_callback(std::function<void(const SearchStats&)> cb) {
        progress_callback_ = cb;
    }

    // Time source for the whole search. Production reads the monotonic clock; a
    // test hands in a scripted one, which is what turns "in which layer did the
    // deadline cross" from a race into an input.
    void set_clock(const SearchClock* clock) { clock_ = clock; }

    // Absolute end-of-move wall-clock deadline (VCF/VCT get their own slices of
    // the same budget). The time_limit_ms handed to search() is only this
    // phase's share, so without the deadline a search that starts after the
    // budget is spent still gets its whole share.
    void set_deadline(std::chrono::steady_clock::time_point deadline) {
        deadline_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                           deadline.time_since_epoch())
                           .count();
        has_deadline_ = true;
    }

private:
    const Evaluator& evaluator_;
    TranspositionTable& tt_;
    // The tiered candidate source (design doc 2.5.1): which squares a layer is
    // allowed to search is its decision, the search only orders and iterates the
    // result.
    MoveGenerator move_gen_;
    // The layer's own memory of what cut: this ply's killers and, per colour and
    // cell, the history behind the price (design doc 2.4.5 ②/③). It is per-think
    // state, aged between thinks, and it is why a second think of one session is not
    // a repeat of the first.
    MoveHeuristics heuristics_;
    // Design doc 2.4.4's triangular PV table, keyed by the same ply the killers use.
    // Nothing in the search reads it: it is written on an alpha raise and copied out
    // once when a layer commits, which is what keeps the line and the node count two
    // separate questions.
    PvTable pv_;
    // 本层已经问过的根着法及其分值。层开始时清空，层完整提交时排序、截断、搬进
    // stats_.root_scores；被切断的层不搬——那半张表里的数来自短路的子树，和 PV
    // 是同一条纪律。
    std::vector<RootMoveScore> layer_scores_;
    int node_limit_;
    // Only the layered accounting left in the object: the counters and the three
    // ways a think can end live in the context, so the stop test is one function
    // over one set of fields instead of three conditions repeated per node.
    SearchContext ctx_;
    const SearchClock* clock_ = nullptr;
    SearchInfo info_;
    SearchStats stats_;
    std::function<void(const SearchStats&)> progress_callback_;

    long long deadline_ms_ = 0;
    bool has_deadline_ = false;

    // `ply` is how far this node sits from the root of the think. The table needs it
    // to keep a mate score as a distance from that root rather than as the remaining
    // depth of whichever node proved it, which is only comparable inside one
    // iteration of one think. It is also the slot this node's line is written to, and
    // the slot its children read - the triangular PV table of design doc 2.4.4, which
    // is why the same node visited twice in one think never inherits a line from the
    // first visit.
    NodeResult alphabeta(Board& board, int depth, int alpha, int beta, int color,
                         std::atomic<bool>& stop_flag, int ply = 0);

    // Design doc 2.6 step 4's bound on the forcing tail. `qdepth` is the ply count
    // inside quiescence, so this is a bound on one node's chain, not on one think: the
    // node entered at this depth answers with its own static score and opens nothing.
    // `seldepth` records the depth a node was entered at, so the deepest number a think
    // can report is exactly this one.
    static constexpr int kMaxQdepth = 8;

    NodeResult quiescence(Board& board, int alpha, int beta, int color,
                          std::atomic<bool>& stop_flag, int qdepth, uint64_t hash,
                          int32_t tt_eval);

    // The one read of a position's static score (design doc 2.6 step 1). `cached` is
    // what an earlier lookup on this node already found - the record's field, or
    // NO_STATIC_EVAL - and is only asked for when it says nothing, so a node that
    // probed the table for its bound does not walk the bucket again for the score.
    // A miss costs the whole-board scan and leaves its result where the next visit of
    // this layout finds it: on the record this position has, or in a slot of the same
    // bucket that holds nothing else.
    //
    // The stored number is BLACK's side of the evaluation and the returned one is
    // `color`'s, which is the same fact read through the antisymmetry of
    // evaluate_board; the field has no byte for whose side it is and this way it needs
    // none. Every path here returns exactly the integer evaluate_board(board, color)
    // would, so the reuse cannot move a score, a cutoff or a node.
    int static_score(const Board& board, int color, uint64_t hash, int32_t cached);

    // One child of a tree node, in the shape design doc 2.7 calls PVS: the whole
    // window for the first candidate of a node and for every candidate of a node that
    // has no bottom to measure against yet, a one-point probe for each later one, and
    // a full-window re-search only for a probe that lands strictly inside the node's
    // window. `board` already carries this node's move, `color` is the side that
    // played it, and the value comes back in the parent's perspective. The probe is
    // what buys the cut: a candidate that cannot raise this node's alpha is never
    // searched again.
    NodeResult search_child_pvs(Board& board, int depth, int alpha, int beta, int color,
                                 int ply, bool scout, std::atomic<bool>& stop_flag);

    // The tiered candidates this layer may search, in the order the generator put
    // them: Tier0/Tier1 whole, Tier2 down to the layer's width, Tier3 only in a
    // position with no threat. `ply` selects the width (design doc 2.5.1's table),
    // so the root asks for ply 0.
    std::vector<Move> generate_moves(const Board& board, int color, int ply = 0);
    // The three stages of design doc 2.4.5 plus the price it puts last: ① the TT
    // move, pinned at index 0 and left out of everything below, ② this ply's two
    // killers in slot order, then ④ the static price and, under it, ③ the history of
    // the cells this search has already seen cut. ③ and ④ are swapped against the
    // doc's list because that is the order this engine's prices can pay for - the
    // measurement is in alphabeta.cpp's comparator and the table is in
    // REVIEW_LOG's B2 section. The last two keys are the ones the root chooses on, so
    // a layer with nothing learned yet searches exactly the order A6 pinned. `ply` is
    // the node's distance from this think's root, which is what keys the killer slots.
    void sort_moves(std::vector<Move>& moves, const Board& board, int color,
                    const Move& tt_move = Move(), int ply = 0);

    // One layer of the root loop: every root candidate inside one window, searched
    // in order until the window's top is reached or the budget is gone. The
    // returned score is the layer's best and its `sound` half says whether any root
    // child short-circuited; how many candidates were actually searched is in
    // stats_.root_moves_done, which is what distinguishes "the window crossed after
    // the last one" from "it crossed in the middle". The layer's best also writes
    // slot 0 of the PV table - the root is not a tree node, so its line has no node
    // to be written by - and only a layer that both finished and stayed sound commits
    // it into stats_.pv.
    NodeResult search_root_layer(Board& board, const std::vector<Move>& root_moves,
                                 int depth, int color, Move& best_move,
                                 int alpha, int beta, std::atomic<bool>& stop_flag);

    // One layer inside an aspiration window centred on `centre` (design doc 2.4.3
    // ③). A value outside the window says the estimate was wrong, not the
    // position, so the side that failed opens up and the layer re-runs against the
    // bound the previous pass actually proved; once both ends are open the layer is
    // the whole window, which is what a think with no estimate at all runs. A pass
    // the budget cut short is returned as it stands: a wider window cannot finish a
    // layer that ran out of nodes.
    NodeResult search_root_with_aspiration(Board& board,
                                           const std::vector<Move>& root_moves, int depth,
                                           int color, Move& best_move, bool use_aspiration,
                                           int centre, std::atomic<bool>& stop_flag);

    // Copy the live counters into the stats so a frame and the final report read
    // the same numbers.
    void refresh_counters();
    void emit_progress();

    static SearchInfo to_info(const SearchStats& stats);
};

}

#endif

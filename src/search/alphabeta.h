#ifndef GOMOKU_ALPHABETA_H
#define GOMOKU_ALPHABETA_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "../core/pattern.h"
#include "../optimization/transposition.h"
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

    NodeResult alphabeta(Board& board, int depth, int alpha, int beta, int color,
                         std::atomic<bool>& stop_flag);

    NodeResult quiescence(Board& board, int alpha, int beta, int color,
                          std::atomic<bool>& stop_flag, int qdepth);

    std::vector<Move> generate_moves(const Board& board, int color);
    void sort_moves(std::vector<Move>& moves, const Board& board, int color,
                    const Move& tt_move = Move());

    // One layer of the root loop: every root candidate inside one window. The
    // returned score is the layer's best and its `sound` half says whether any
    // root child short-circuited; how many candidates were actually searched is
    // in stats_.root_moves_done, which is what distinguishes "the window crossed
    // after the last one" from "it crossed in the middle".
    NodeResult search_root_layer(Board& board, const std::vector<Move>& root_moves,
                                 int depth, int color, Move& best_move,
                                 std::atomic<bool>& stop_flag);

    // Copy the live counters into the stats so a frame and the final report read
    // the same numbers.
    void refresh_counters();
    void emit_progress();

    static SearchInfo to_info(const SearchStats& stats);
};

}

#endif

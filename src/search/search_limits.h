#ifndef GOMOKU_SEARCH_LIMITS_H
#define GOMOKU_SEARCH_LIMITS_H

#include "../core/types.h"
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace gomoku {

// Why a think ended. The engine used to have no way to say: an aborted search
// and an unfinished depth-0 one both came out as method "greedy", so the client
// could not tell "no time" from "nothing to search" (design doc P1.5).
enum class StopReason {
    NONE,
    TIME_SOFT,  // the soft deadline passed at a layer boundary; a layer did finish
    TIME_HARD,  // the absolute deadline crossed mid-layer
    NODES,      // the node budget ran out
    USER,       // the stop flag was set (重开 / 终止 / 接管)
    MATE        // a forced win or loss was proven, no point searching further
};

const char* stop_reason_name(StopReason reason);

// How far a cell stands from the centre, in the metric the root tie-break orders
// on. Read from the board geometry rather than from
// Evaluator::get_position_weight on purpose: that table is scaled by the tunable
// position_weight_scale, and a coefficient of 0 would silently delete the
// tie-break this is a key of. Every D4 symmetry of the board preserves it.
inline int center_distance(Position pos) {
    const int center = BOARD_SIZE / 2;
    return std::abs(pos.row - center) + std::abs(pos.col - center);
}

// The total order every candidate decision is made on: value, then being nearer
// the centre (center_distance above), then the lower cell index. The last key is
// what makes it a total order - no two cells share an index, so no comparison can
// come back "equal" and leave the answer to whatever order a container happened to
// iterate in - and the second is what makes the engine's choice in a tie be the
// central one instead of an accident of Board::active_empty()'s swap-remove order.
//
// It lives here, next to the distance it reads, because both the search (which
// picks the root answer) and the candidate generator (which decides which
// candidates the width limit keeps) order on it: one definition is what keeps
// "the tier that survived truncation" and "the layer that won" saying the same
// thing about which candidate is better.
struct RootScore {
    int score;
    int center_dist;
    int cell_index;

    RootScore(const Move& m, int s)
        : score(s), center_dist(center_distance(m.pos)), cell_index(m.pos.to_index()) {}

    bool operator>(const RootScore& other) const {
        if (score != other.score) return score > other.score;
        if (center_dist != other.center_dist) return center_dist < other.center_dist;
        return cell_index < other.cell_index;
    }
};

// How one think is allowed to spend its budget.
struct SearchLimits {
    int max_depth = 8;
    int soft_time_ms = 0;   // 0 = no soft window; the whole hard deadline is usable
    int hard_time_ms = 0;   // 0 = no wall-clock limit at all
    int max_nodes = 0;      // 0 = no node limit; this is what training runs on
    bool ponder = false;
    Move hint_move;                 // what the previous think considered best
    int hint_score = 0;             // its score, used to centre the aspiration
    bool has_hint_score = false;
    bool training = false;          // budget is nodes, not time
};

// What one think actually did, including the part of it that was cut short.
struct SearchStats {
    int completed_depth = 0;
    int partial_depth = 0;          // the interrupted layer; 0 = nothing interrupted
    int root_moves_done = 0;
    int root_moves_total = 0;
    int seldepth = 0;               // deepest quiescence reach
    int score = 0;
    Move best_move;                 // the last completed layer's winner
    Move partial_best_move;         // the interrupted layer's, for display only
    std::vector<Move> pv;
    uint64_t nodes = 0;
    uint64_t qnodes = 0;
    uint64_t tt_hits = 0;
    uint64_t tt_cutoffs = 0;
    int elapsed_ms = 0;
    StopReason stopped = StopReason::NONE;
};

// The clock a search reads, injected so that "which layer was it in when the
// deadline crossed" is a number a test sets rather than a race it hopes to lose.
class SearchClock {
public:
    virtual ~SearchClock() = default;
    virtual long long now_ms() const = 0;
};

// Milliseconds on a monotonic clock, for production use.
long long steady_clock_ms();

// The clock a search uses when its owner did not install one. A shared instance
// rather than a fresh subclass per search, so nothing is allocated or virtual
// called per think.
const SearchClock& system_search_clock();

}

#endif

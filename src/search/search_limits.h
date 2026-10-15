#ifndef GOMOKU_SEARCH_LIMITS_H
#define GOMOKU_SEARCH_LIMITS_H

#include "../core/types.h"
#include <cstdint>
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

// How one think is allowed to spend its budget.
struct SearchLimits {
    int max_depth = 8;
    int soft_time_ms = 0;   // 0 = derive from the hard deadline
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

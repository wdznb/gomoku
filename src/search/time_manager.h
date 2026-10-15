#ifndef GOMOKU_TIME_MANAGER_H
#define GOMOKU_TIME_MANAGER_H

#include "../core/board.h"
#include "search_limits.h"

namespace gomoku {

// What one move gets to spend, after the position has had a say in it.
struct MoveBudget {
    int soft_ms = 0;
    int hard_ms = 0;
    int max_nodes = 0;
};

// Turns the caller's request into a budget. The engine used to hand the same
// fixed proportions to VCF (35%) and VCT (55%) whatever the position needed;
// this is where the per-move decisions live instead.
class TimeManager {
public:
    // The side to move is not an input: the candidate set is colour-independent
    // and the forbidden-point difference is already settled by the fast paths
    // above this layer. `prev_move_elapsed_ms` is what the previous move in this
    // session actually cost, or 0 when there is no previous move.
    static MoveBudget allocate(const Board& board, const SearchLimits& limits,
                               int prev_move_elapsed_ms);
};

}

#endif

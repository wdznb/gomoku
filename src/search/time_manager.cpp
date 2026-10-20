#include "time_manager.h"

#include <algorithm>
#include <chrono>

namespace gomoku {

const char* stop_reason_name(StopReason reason) {
    switch (reason) {
        case StopReason::NONE: return "none";
        case StopReason::TIME_SOFT: return "time_soft";
        case StopReason::TIME_HARD: return "time_hard";
        case StopReason::NODES: return "nodes";
        case StopReason::USER: return "user";
        case StopReason::MATE: return "mate";
    }
    return "none";
}

long long steady_clock_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

namespace {

// steady_clock rather than high_resolution_clock because VCF, VCT and the
// end-of-move deadline a SearchEngine installs are all steady_clock time_points.
// Mixing the two would compare time-since-boot against time-since-epoch.
class SystemSearchClock : public SearchClock {
public:
    long long now_ms() const override { return steady_clock_ms(); }
};

const SystemSearchClock kSystemSearchClock;

}

const SearchClock& system_search_clock() {
    return kSystemSearchClock;
}

MoveBudget TimeManager::allocate(const Board& board, const SearchLimits& limits,
                                 int prev_move_elapsed_ms) {
    MoveBudget budget;
    budget.hard_ms = limits.hard_time_ms > 0 ? limits.hard_time_ms : 0;
    budget.max_nodes = limits.max_nodes > 0 ? limits.max_nodes : 0;

    // Training drives itself by nodes: one move then costs the same on a loaded
    // machine and on an idle one, which is the property a time budget cannot give.
    if (limits.training) {
        budget.soft_ms = 0;
        budget.hard_ms = 0;
        return budget;
    }

    if (budget.hard_ms <= 0) {
        budget.soft_ms = 0;
        return budget;
    }

    budget.soft_ms = limits.soft_time_ms > 0
                         ? std::min(limits.soft_time_ms, budget.hard_ms)
                         : budget.hard_ms * 65 / 100;

    // A handful of candidates means a shallow tree: the forcing moves are
    // already taken by SearchEngine's fast paths, so what is left converges
    // quickly and the remaining budget would only be spent re-proving a cut.
    if (board.active_empty().size() <= 6) {
        budget.soft_ms = std::min(budget.soft_ms, budget.hard_ms / 2);
    }

    // The opening is book and pattern work rather than a deep read, and an
    // endgame is the opposite: few stones, long lines, one tempo deciding the
    // result, so it is allowed to run close to the hard limit.
    const int empties = MAX_MOVES - board.move_count();
    if (board.move_count() < 6) {
        budget.soft_ms = std::min(budget.soft_ms, budget.hard_ms / 2);
    } else if (empties < 30) {
        budget.soft_ms = std::min(budget.hard_ms, budget.soft_ms * 5 / 4);
    }

    // A move that already overran 80% of its limit is the sign of a position
    // this engine cannot read quickly; asking the next one for the full soft
    // window is how two consecutive timeouts look to the player.
    if (prev_move_elapsed_ms > budget.hard_ms * 8 / 10) {
        budget.soft_ms = std::min(budget.soft_ms, budget.hard_ms / 2);
    }

    budget.soft_ms = std::max(budget.soft_ms, 1);
    return budget;
}

}

#ifndef GOMOKU_SEARCH_STATE_H
#define GOMOKU_SEARCH_STATE_H

#include "search_limits.h"
#include <atomic>
#include <cstdint>

namespace gomoku {

// Everything one think mutates that is not a result: the budget it is spending,
// the counters that report how much it spent, and why it stopped. Pulling it out
// of AlphaBetaSearch is what turns the stop test into one function over one set
// of fields instead of four conditions repeated at three call sites.
class SearchContext {
public:
    void start(const SearchLimits& limits, const SearchClock& clock,
               std::atomic<bool>& stop_flag) {
        clock_ = &clock;
        stop_flag_ = &stop_flag;
        // One reading, then offsets: a clock that advances when it is asked
        // (which is what makes an interrupt land at a testable layer) would move
        // the hard and soft deadlines apart from each other if each computed its
        // own.
        const long long now = clock.now_ms();
        hard_ms_ = limits.hard_time_ms > 0 ? now + limits.hard_time_ms : 0;
        soft_ms_ = limits.soft_time_ms > 0 ? now + limits.soft_time_ms : 0;
        max_nodes_ = limits.max_nodes > 0 ? static_cast<uint64_t>(limits.max_nodes) : 0;
        start_ms_ = now;
        nodes_ = 0;
        qnodes_ = 0;
        tt_hits_ = 0;
        tt_cutoffs_ = 0;
        seldepth_ = 0;
        check_counter_ = 0;
        reason_ = StopReason::NONE;
    }

    // The mid-tree stop test. The abort flag and the node budget are cheap and
    // read at every node; the clock is read once per 1024 nodes, because a check
    // whose whole purpose is to save time costs more per node than it saves.
    bool hard_stopped() {
        if (reason_ != StopReason::NONE) return true;
        if (stop_flag_->load(std::memory_order_relaxed)) {
            reason_ = StopReason::USER;
            return true;
        }
        if (max_nodes_ != 0 && nodes_ >= max_nodes_) {
            reason_ = StopReason::NODES;
            return true;
        }
        if ((++check_counter_ & 0x3FF) == 0 && hard_ms_ != 0 &&
            clock_->now_ms() >= hard_ms_) {
            reason_ = StopReason::TIME_HARD;
            return true;
        }
        return false;
    }

    // Read at a layer boundary only, where the cost is irrelevant: a soft
    // deadline that has passed with a finished layer below is a reason not to
    // start the next iteration, not a reason to discard the last one.
    bool soft_time_up() const {
        return soft_ms_ != 0 && clock_->now_ms() >= soft_ms_;
    }

    StopReason reason() const { return reason_; }
    // Set only by the root loop, for a result the search stopped on deliberately
    // rather than ran out of budget on: reason_ stays free to mean "aborted".
    void set_reason(StopReason reason) { reason_ = reason; }
    bool stopped_for(StopReason reason) const { return reason_ == reason; }

    long long elapsed_ms() const { return clock_->now_ms() - start_ms_; }

    void count_node() { ++nodes_; }
    void count_quiescence_node() { ++nodes_; ++qnodes_; }
    void count_tt_hit() { ++tt_hits_; }
    void count_tt_cutoff() { ++tt_cutoffs_; }
    void note_quiescence_depth(int depth) { if (depth > seldepth_) seldepth_ = depth; }

    uint64_t nodes() const { return nodes_; }
    uint64_t quiescence_nodes() const { return qnodes_; }
    uint64_t tt_hits() const { return tt_hits_; }
    uint64_t tt_cutoffs() const { return tt_cutoffs_; }
    int seldepth() const { return seldepth_; }

private:
    const SearchClock* clock_ = nullptr;
    std::atomic<bool>* stop_flag_ = nullptr;
    long long start_ms_ = 0;
    long long hard_ms_ = 0;
    long long soft_ms_ = 0;
    uint64_t max_nodes_ = 0;
    uint64_t nodes_ = 0;
    uint64_t qnodes_ = 0;
    uint64_t tt_hits_ = 0;
    uint64_t tt_cutoffs_ = 0;
    int seldepth_ = 0;
    unsigned check_counter_ = 0;
    StopReason reason_ = StopReason::NONE;
};

}

#endif

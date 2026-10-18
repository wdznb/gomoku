#ifndef GOMOKU_SEARCH_STATE_H
#define GOMOKU_SEARCH_STATE_H

#include "../core/types.h"
#include "search_limits.h"
#include <atomic>
#include <cstdint>
#include <vector>

namespace gomoku {

// Two squares are the same square by their cell, not by the record that carries
// them: a killer stored by the side that moved last ply and read back by the side
// that moves this ply must still be found.
inline bool same_cell(const Move& move, const Position& pos) {
    return move.pos.is_valid() && pos.is_valid() &&
           move.pos.to_index() == pos.to_index();
}

// Design doc 2.4.5's stages ② and ③: the two moves that cut this same layer before
// (killers, one slot per ply) and, per colour and cell, how often a move played at
// this depth has produced a cutoff (history, accumulated by depth² and aged).
//
// They exist because the width limit decides *which* squares a layer sees while the
// static score only knows what a square is worth on its own. A square that just cut
// a subtree is worth searching first for a reason the price cannot express, and
// searching it first is what makes the sibling's window fail high on move one
// instead of after all of them.
//
// Both are keyed to one think: `ply` counts from the root the caller started this
// think at, so a killer from the previous think sits at a ply that now means a
// different position. A think therefore begins with empty slots, and history ages
// by half rather than being thrown out - it is the only thing that says "this cell
// keeps cutting" across the two or three thinks that play one move.
class MoveHeuristics {
public:
    // A node deeper than this has no killer slot, so stage ② has nothing to place
    // and its order starts at the price. The cap is the search's own ceiling (design
    // doc: depth 20) plus room for the re-searches, and a killer index that ran past
    // it would write outside the table.
    static const int kMaxPly = 64;
    static const int kKillerSlots = 2;

    void start_think() {
        for (int p = 0; p < kMaxPly; ++p) {
            killer_[p][0] = Move();
            killer_[p][1] = Move();
        }
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < BOARD_SIZE * BOARD_SIZE; ++i) {
                history_[c][i] >>= 1;
            }
        }
    }

    // The move that made this node fail high. Both tables answer for it at once:
    // the layer it happened on, and the depth that was still left below it.
    void record_cutoff(int ply, const Move& move, int color, int depth) {
        history_[color_slot(color)][move.pos.to_index()] += depth * depth;
        if (ply < 0 || ply >= kMaxPly) return;
        if (same_cell(killer_[ply][0], move.pos)) return;
        killer_[ply][1] = killer_[ply][0];
        killer_[ply][0] = move;
    }

    // Ages history between the iterations of one think, which is what 随搜索衰减
    // says: a cutoff found two layers below this one is evidence about a shallower
    // tree, and by the time the search is six layers deep it should not still be
    // deciding which of two equally-priced cells a layer tries first. One shift per
    // layer keeps the freshest iteration dominant and lets old, weakly-supported
    // entries fall to 0 on their own, with no threshold for the search to tune
    // against.
    void start_layer() {
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < BOARD_SIZE * BOARD_SIZE; ++i) {
                history_[c][i] >>= 1;
            }
        }
    }

    // Slot order is rank order: slot 0 cut this layer most recently.
    const Move& killer(int ply, int slot) const {
        static const Move kNone;
        if (ply < 0 || ply >= kMaxPly || slot < 0 || slot >= kKillerSlots) return kNone;
        return killer_[ply][slot];
    }

    int history_value(int color, const Position& pos) const {
        if (!pos.is_valid()) return 0;
        return history_[color_slot(color)][pos.to_index()];
    }

    // For the tests that want to read what a think left behind rather than infer it
    // from an order.
    int killer_count(int ply) const {
        if (ply < 0 || ply >= kMaxPly) return 0;
        int n = 0;
        for (int slot = 0; slot < kKillerSlots; ++slot) {
            if (killer_[ply][slot].pos.is_valid()) ++n;
        }
        return n;
    }

    static int color_slot(int color) { return color == BLACK ? 0 : 1; }

private:
    Move killer_[kMaxPly][kKillerSlots];
    // One entry per cell, indexed exactly the way Position::to_index() numbers the
    // board, so reading a move's history costs no lookup. Zero at construction:
    // start_think() ages the table instead of clearing it, and an aged uninitialised
    // cell would order the very first think of a session on stack noise.
    int history_[2][BOARD_SIZE * BOARD_SIZE] = {};
};

// Design doc 2.4.4's first option: the triangular PV table. One line per ply; a
// node writes its own line when a child raises its alpha, and what it writes is
// `[that move] + line(ply + 1)`.
//
// It exists because the line a layer proved died with the node that proved it:
// `SearchStats::pv` had no writer at all, so the answer the player got was one cell
// and the reply this search expected to the opponent's best reply - which is the
// whole reason that cell was worth playing - was thrown away. The same line is what
// a ponder preset would re-search on the next hand (doc 2.4.3 ②), and what a
// stronger layer would reuse, so it lives in the think's state rather than being
// rebuilt from the result.
//
// Every node clears its own slot on entry. That is what makes copying one ply down
// safe: the slot below is either the child that just returned or empty, never the
// line some earlier sibling left at the same ply. It is also why the reported line
// can end short - at a quiescence leaf or a terminal five - instead of stopping
// wherever the last node that touched this ply happened to stop.
class PvTable {
public:
    // The same ply ceiling as the killers. A node deeper than this has no slot, so
    // the line it produced is simply not extended, and the parent's entry stands as
    // the end of the variation.
    static const int kMaxPly = 64;
    // A line cannot outlive the tree it came from: at most the documented depth
    // ceiling (20) of tree plies below the root, plus the root's own move.
    static const int kMaxLen = 24;

    void start_think() {
        for (int p = 0; p < kMaxPly; ++p) length_[p] = 0;
    }

    // Called at the top of every node, including the paths that return without
    // searching anything.
    void clear(int ply) {
        if (in_range(ply)) length_[ply] = 0;
    }

    // This node's line: the move it is playing, then the line the child one ply
    // deeper left behind.
    void extend(int ply, const Move& move, int child_ply) {
        if (!in_range(ply)) return;
        int n = 0;
        line_[ply][n++] = move;
        if (in_range(child_ply)) {
            const int child_len = length_[child_ply];
            for (int i = 0; i < child_len && n < kMaxLen; ++i) {
                line_[ply][n++] = line_[child_ply][i];
            }
        }
        length_[ply] = n;
    }

    // A line this node cannot attest deeper than one move: the value it is returning
    // came out of the table, so the rest of that variation belongs to whichever node
    // wrote the entry and is not in this think's slots. The move itself is still the
    // one that search proved, which is what makes the parent's line continue rather
    // than stop.
    void set_single(int ply, const Move& move) {
        if (!in_range(ply)) return;
        if (!move.pos.is_valid()) {
            length_[ply] = 0;
            return;
        }
        line_[ply][0] = move;
        length_[ply] = 1;
    }

    int length(int ply) const { return in_range(ply) ? length_[ply] : 0; }

    const Move& at(int ply, int i) const {
        static const Move kNone;
        if (!in_range(ply) || i < 0 || i >= length_[ply]) return kNone;
        return line_[ply][i];
    }

    // Out of the think's state and into the record the caller reads.
    void copy_out(int ply, std::vector<Move>* out) const {
        out->clear();
        if (!in_range(ply)) return;
        for (int i = 0; i < length_[ply]; ++i) out->push_back(line_[ply][i]);
    }

private:
    static bool in_range(int ply) { return ply >= 0 && ply < kMaxPly; }

    Move line_[kMaxPly][kMaxLen];
    // Zero at construction for the same reason history is: the first think of a
    // session must not read a line out of stack noise.
    int length_[kMaxPly] = {};
};

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
        has_hard_ = limits.hard_time_ms > 0;
        hard_ms_ = now + limits.hard_time_ms;
        has_soft_ = limits.soft_time_ms > 0;
        soft_ms_ = now + limits.soft_time_ms;
        max_nodes_ = limits.max_nodes > 0 ? static_cast<uint64_t>(limits.max_nodes) : 0;
        start_ms_ = now;
        nodes_ = 0;
        qnodes_ = 0;
        tt_hits_ = 0;
        tt_cutoffs_ = 0;
        eval_scans_ = 0;
        eval_reuses_ = 0;
        eval_patches_ = 0;
        seldepth_ = 0;
        check_counter_ = 0;
        reason_ = StopReason::NONE;
    }

    // A deadline given as an absolute instant rather than a share of this
    // phase's budget. It has to be expressible separately because an offset of
    // zero means "no limit", while a deadline already in the past means "stop
    // before the first layer" - the two cannot share one field.
    void set_absolute_deadline(long long deadline_ms) {
        has_hard_ = true;
        hard_ms_ = deadline_ms;
    }
    void set_absolute_soft_limit(long long soft_ms) {
        has_soft_ = soft_ms > 0;
        soft_ms_ = soft_ms;
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
        if ((++check_counter_ & 0x3FF) == 0) return deadline_passed();
        return false;
    }

    // The layer-boundary stop test: the same three cheap checks, but the clock is
    // read every time. This is where the engine decides whether to start another
    // layer, and an answer stale by a layer is not an answer.
    bool layer_boundary_stopped() {
        if (reason_ != StopReason::NONE) return true;
        if (stop_flag_->load(std::memory_order_relaxed)) {
            reason_ = StopReason::USER;
            return true;
        }
        if (max_nodes_ != 0 && nodes_ >= max_nodes_) {
            reason_ = StopReason::NODES;
            return true;
        }
        return deadline_passed();
    }

    // Read at a layer boundary only, where the cost is irrelevant: a soft
    // deadline that has passed with a finished layer below is a reason not to
    // start the next iteration, not a reason to discard the last one.
    bool soft_time_up() const {
        return has_soft_ && clock_->now_ms() >= soft_ms_;
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

    // Design doc 2.6 step 1's three cost counters: how many positions a think paid a
    // whole-board scan for, how many it read off the table instead, and how many of
    // those scans found somewhere in the table to leave their score. They are
    // deliberately not part of SearchStats: that struct is a progress frame, and a
    // frame's fields are read by the session archive and the client, while these three
    // numbers describe work the search did not do - a fact about cost, not about the
    // position, and one that a node-neutral reuse changes without changing anything a
    // player sees.
    void count_eval_scan() { ++eval_scans_; }
    void count_eval_reuse() { ++eval_reuses_; }
    void count_eval_patch() { ++eval_patches_; }

    void note_quiescence_depth(int depth) { if (depth > seldepth_) seldepth_ = depth; }

    uint64_t nodes() const { return nodes_; }
    uint64_t quiescence_nodes() const { return qnodes_; }
    uint64_t tt_hits() const { return tt_hits_; }
    uint64_t tt_cutoffs() const { return tt_cutoffs_; }
    uint64_t eval_scans() const { return eval_scans_; }
    uint64_t eval_reuses() const { return eval_reuses_; }
    uint64_t eval_patches() const { return eval_patches_; }
    int seldepth() const { return seldepth_; }

private:
    bool deadline_passed() {
        if (!has_hard_) return false;
        if (clock_->now_ms() >= hard_ms_) {
            reason_ = StopReason::TIME_HARD;
            return true;
        }
        return false;
    }

    const SearchClock* clock_ = nullptr;
    std::atomic<bool>* stop_flag_ = nullptr;
    long long start_ms_ = 0;
    bool has_hard_ = false;
    long long hard_ms_ = 0;
    bool has_soft_ = false;
    long long soft_ms_ = 0;
    uint64_t max_nodes_ = 0;
    uint64_t nodes_ = 0;
    uint64_t qnodes_ = 0;
    uint64_t tt_hits_ = 0;
    uint64_t tt_cutoffs_ = 0;
    uint64_t eval_scans_ = 0;
    uint64_t eval_reuses_ = 0;
    uint64_t eval_patches_ = 0;
    int seldepth_ = 0;
    unsigned check_counter_ = 0;
    StopReason reason_ = StopReason::NONE;
};

}

#endif

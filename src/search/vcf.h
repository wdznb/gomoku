#ifndef GOMOKU_VCF_H
#define GOMOKU_VCF_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "../core/pattern.h"
#include "../optimization/transposition.h"
#include <atomic>
#include <chrono>
#include <vector>

namespace gomoku {

enum class VCFResult {
    WIN,
    LOSS,
    UNKNOWN
};

enum class DefenseResult {
    SURVIVES,
    PROVEN_LOSS,
    UNKNOWN
};

struct VCFMove {
    VCFResult result;
    Move best_move;
    std::vector<Move> variation;

    VCFMove() : result(VCFResult::UNKNOWN) {}
};

class VCFSearch {
public:
    VCFSearch(const Evaluator& evaluator, TranspositionTable& tt);

    VCFMove search(const Board& board, int color, int max_depth,
                   std::atomic<bool>& stop_flag);

    // Proves the position lost for `defender` (who moves first) when every legal
    // reply still leaves the opponent a forced VCF win. Only PROVEN_LOSS is a
    // resignation-grade answer; UNKNOWN means the proof was cut short.
    DefenseResult find_defense(const Board& board, int defender, int max_depth,
                               std::atomic<bool>& stop_flag, Move& defense_move);

    void set_node_limit(int limit) { node_limit_ = limit; }

    // Optional absolute wall-clock deadline shared with the rest of the search
    // (see VCTSearch::set_deadline); without it VCF runs to node_limit_ and can
    // overrun the caller's timeout_ms.
    void set_deadline(std::chrono::steady_clock::time_point deadline) {
        deadline_ = deadline;
        has_deadline_ = true;
    }

private:
    const Evaluator& evaluator_;
    TranspositionTable& tt_;
    int node_limit_;
    int nodes_searched_;
    std::chrono::steady_clock::time_point deadline_{};
    bool has_deadline_ = false;

    bool aborted(std::atomic<bool>& stop_flag) const {
        return stop_flag.load() ||
               (has_deadline_ && std::chrono::steady_clock::now() >= deadline_);
    }

    int vcf_attack(Board& board, int color, int depth, int alpha, int beta,
                   std::atomic<bool>& stop_flag);

    std::vector<Move> generate_rush_fours(const Board& board, int color);
    Move find_block_move(const Board& board, int row, int col, int color);

    // Takes the board it may mutate: the caller hands over a private working
    // copy, so the win-attempt loop can place/undo instead of cloning a board per
    // candidate. find_defense reuses one copy across all of the defender's
    // replies for the same reason.
    VCFMove search_impl(Board& board, int color, int max_depth,
                        std::atomic<bool>& stop_flag);

    // A move that makes five or a live four forces the opponent to answer, which
    // invalidates any win proof that assumed they keep the initiative.
    bool has_decisive_tempo(const Board& board, int row, int col, int color) const;
};

}

#endif

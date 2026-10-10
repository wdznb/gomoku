#ifndef GOMOKU_VCF_H
#define GOMOKU_VCF_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "../core/pattern.h"
#include "../optimization/transposition.h"
#include <atomic>
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

private:
    const Evaluator& evaluator_;
    TranspositionTable& tt_;
    int node_limit_;
    int nodes_searched_;

    int vcf_attack(Board& board, int color, int depth, int alpha, int beta,
                   std::atomic<bool>& stop_flag);

    std::vector<Move> generate_rush_fours(const Board& board, int color);
    Move find_block_move(const Board& board, int row, int col, int color);

    VCFMove search_impl(const Board& board, int color, int max_depth,
                        std::atomic<bool>& stop_flag);

    // A move that makes five or a live four forces the opponent to answer, which
    // invalidates any win proof that assumed they keep the initiative.
    bool has_decisive_tempo(const Board& board, int row, int col, int color) const;
};

}

#endif

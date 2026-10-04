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
};

}

#endif

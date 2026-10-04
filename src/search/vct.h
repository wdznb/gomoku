#ifndef GOMOKU_VCT_H
#define GOMOKU_VCT_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "../core/pattern.h"
#include "../optimization/transposition.h"
#include <atomic>
#include <vector>

namespace gomoku {

enum class VCTResult {
    WIN,
    LOSS,
    UNKNOWN
};

struct VCTMove {
    VCTResult result;
    Move best_move;
    std::vector<Move> variation;

    VCTMove() : result(VCTResult::UNKNOWN) {}
};

class VCTSearch {
public:
    VCTSearch(const Evaluator& evaluator, TranspositionTable& tt);

    VCTMove search(const Board& board, int color, int max_depth,
                   std::atomic<bool>& stop_flag);

    void set_node_limit(int limit) { node_limit_ = limit; }

private:
    const Evaluator& evaluator_;
    TranspositionTable& tt_;
    int node_limit_;
    int nodes_searched_;

    int vct_attack(Board& board, int color, int depth, int alpha, int beta,
                   std::atomic<bool>& stop_flag);

    int vct_defend(Board& board, int color, int depth, int alpha, int beta,
                   std::atomic<bool>& stop_flag);

    std::vector<Move> generate_attacks(const Board& board, int color);
    std::vector<Move> generate_defenses(const Board& board, int color, int attack_row, int attack_col);
};

}

#endif

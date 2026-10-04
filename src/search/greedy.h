#ifndef GOMOKU_GREEDY_H
#define GOMOKU_GREEDY_H

#include "../core/types.h"
#include "../core/board.h"
#include "../core/evaluator.h"
#include <vector>

namespace gomoku {

struct ScoredMove {
    Move move;
    int attack_score;
    int defense_score;
    int total_score;
    
    bool operator<(const ScoredMove& other) const {
        return total_score > other.total_score;
    }
};

class GreedySearch {
public:
    GreedySearch();
    explicit GreedySearch(const ThreatCoefficients& coeffs);
    
    Move search(const Board& board, int color) const;
    std::vector<ScoredMove> search_ranked(const Board& board, int color, int max_results = 10) const;
    
    void set_coefficients(const ThreatCoefficients& coeffs);
    
private:
    Evaluator evaluator_;
};

}

#endif

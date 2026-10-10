#include "greedy.h"
#include <algorithm>
#include <set>

namespace gomoku {

GreedySearch::GreedySearch() {}

GreedySearch::GreedySearch(const ThreatCoefficients& coeffs) : evaluator_(coeffs) {}

void GreedySearch::set_coefficients(const ThreatCoefficients& coeffs) {
    evaluator_.set_coefficients(coeffs);
}

Move GreedySearch::search(const Board& board, int color) const {
    auto ranked = search_ranked(board, color, 1);
    if (ranked.empty()) {
        return Move(BOARD_SIZE / 2, BOARD_SIZE / 2, color);
    }
    return ranked[0].move;
}

std::vector<ScoredMove> GreedySearch::search_ranked(const Board& board, int color, 
                                                      int max_results) const {
    int opp_color = opponent(color);
    const auto& candidates = board.active_empty();
    
    std::vector<ScoredMove> scored_moves;
    scored_moves.reserve(candidates.size());
    
    for (const auto& pos : candidates) {
        if (PatternRecognizer::is_forbidden(board, pos.row, pos.col, color)) {
            continue;
        }
        
        // evaluate_position_both already returns my_score + opp_score, so
        // calling it once with `color` and once with `opp_color` produced
        // identical values (their summands are just swapped). Compute each
        // side separately with evaluate_position so attack_score and
        // defense_score remain distinguishable.
        int attack_score = evaluator_.evaluate_position(board, pos.row, pos.col, color);
        int defense_score = evaluator_.evaluate_position(board, pos.row, pos.col, opp_color);
        
        int total_score = attack_score + defense_score;
        
        ScoredMove sm;
        sm.move = Move(pos, color);
        sm.attack_score = attack_score;
        sm.defense_score = defense_score;
        sm.total_score = total_score;
        
        scored_moves.push_back(sm);
    }
    
    std::sort(scored_moves.begin(), scored_moves.end());
    
    if (static_cast<int>(scored_moves.size()) > max_results) {
        scored_moves.resize(max_results);
    }
    
    return scored_moves;
}

}

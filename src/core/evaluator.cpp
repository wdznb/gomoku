#include "evaluator.h"
#include <cmath>
#include <algorithm>

namespace gomoku {

Evaluator::Evaluator() {
    init_position_weights();
}

Evaluator::Evaluator(const ThreatCoefficients& coeffs) : coeffs_(coeffs) {
    init_position_weights();
}

void Evaluator::set_coefficients(const ThreatCoefficients& coeffs) {
    coeffs_ = coeffs;
    // position_weights_ are built from coeffs_.position_weight_scale at
    // construction; without a rebuild a runtime coefficient change leaves the
    // center-bias table stale and move ordering dominated by old weights.
    init_position_weights();
}

void Evaluator::init_position_weights() {
    int center = BOARD_SIZE / 2;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            int dist_r = std::abs(r - center);
            int dist_c = std::abs(c - center);
            int max_dist = center * 2;
            int dist = dist_r + dist_c;
            position_weights_[r][c] = (max_dist - dist) * coeffs_.position_weight_scale;
        }
    }
}

int Evaluator::get_position_weight(int row, int col) const {
    return position_weights_[row][col];
}

int Evaluator::pattern_score(const PositionPattern& pattern) const {
    int score = 0;

    // FIVE is only counted here, not via has_live_four / four_count, so
    // evaluate_board (which uses pattern_score directly) can see a
    // candidate square that completes five for either side. Without this
    // branch the stand-pat leaf value at every quiescence entry ignores
    // one-ply mate threats and returns an over-optimistic score.
    for (int d = 0; d < 4; ++d) {
        if (pattern.directions[d].type == PatternType::FIVE) {
            score += coeffs_.five;
        }
    }

    if (pattern.has_live_four) {
        score += coeffs_.live_four * pattern.live_four_count;
    }
    
    if (pattern.has_rush_four) {
        score += coeffs_.rush_four * pattern.rush_four_count;
    }
    
    if (pattern.has_live_three) {
        score += coeffs_.live_three * pattern.live_three_count;
    }
    
    for (int d = 0; d < 4; ++d) {
        if (pattern.directions[d].type == PatternType::SLEEPY_THREE) {
            score += coeffs_.sleepy_three;
        } else if (pattern.directions[d].type == PatternType::LIVE_TWO) {
            score += coeffs_.live_two;
        } else if (pattern.directions[d].type == PatternType::SLEEPY_TWO) {
            score += coeffs_.sleepy_two;
        }
    }
    
    if (pattern.has_four_four) {
        score += coeffs_.double_four;
    }
    if (pattern.has_four_three) {
        score += coeffs_.four_three;
    }
    if (pattern.has_three_three) {
        score += coeffs_.double_three;
    }
    
    return score;
}

int Evaluator::evaluate_position(const Board& board, int row, int col, int color) const {
    if (!board.is_empty(row, col)) return 0;

    PositionPattern pattern = PatternRecognizer::recognize_position(board, row, col, color);

    return evaluate_position_with_pattern(board, row, col, pattern);
}

int Evaluator::evaluate_position_with_pattern(const Board& board, int row, int col,
                                              const PositionPattern& pattern) const {
    if (!board.is_empty(row, col)) return 0;

    for (int d = 0; d < 4; ++d) {
        if (pattern.directions[d].type == PatternType::FIVE) {
            return coeffs_.five;
        }
    }

    int score = pattern_score(pattern);
    score += get_position_weight(row, col);

    return score;
}

int Evaluator::evaluate_position_both(const Board& board, int row, int col, int my_color) const {
    if (!board.is_empty(row, col)) return 0;
    
    int opp_color = opponent(my_color);
    
    if (PatternRecognizer::is_forbidden(board, row, col, my_color)) {
        return -coeffs_.forbidden_penalty;
    }
    
    int my_score = evaluate_position(board, row, col, my_color);
    int opp_score = evaluate_position(board, row, col, opp_color);
    
    return my_score + opp_score;
}

int Evaluator::evaluate_board(const Board& board, int my_color) const {
    int opp_color = opponent(my_color);
    long long my_total = 0;
    long long opp_total = 0;
    
    const auto& candidates = board.active_empty();
    
    for (const auto& pos : candidates) {
        PositionPattern my_pattern = PatternRecognizer::recognize_position(board, pos.row, pos.col, my_color);
        my_total += pattern_score(my_pattern);
        
        PositionPattern opp_pattern = PatternRecognizer::recognize_position(board, pos.row, pos.col, opp_color);
        opp_total += pattern_score(opp_pattern);
    }
    
    long long diff = my_total - opp_total;
    // Summing pattern scores over hundreds of candidate squares can exceed the
    // mate threshold (100000) that AlphaBeta uses to detect forced wins and
    // terminate iterative deepening. Static evaluation must never reach a mate
    // score or the root will break early on what is really just a busy board.
    long long cap = static_cast<long long>(coeffs_.five) - 1;
    if (diff > cap) diff = cap;
    if (diff < -cap) diff = -cap;
    return static_cast<int>(diff);
}

}

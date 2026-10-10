#ifndef GOMOKU_EVALUATOR_H
#define GOMOKU_EVALUATOR_H

#include "types.h"
#include "board.h"
#include "pattern.h"

namespace gomoku {

struct ThreatCoefficients {
    int five = 100000;
    int live_four = 10000;
    int rush_four = 1000;
    int live_three = 500;
    int sleepy_three = 100;
    int live_two = 50;
    int sleepy_two = 10;
    
    int double_four = 50000;
    int double_three = 30000;
    int four_three = 20000;
    
    int forbidden_penalty = 50000;
    
    int position_weight_scale = 1;
    
    ThreatCoefficients() = default;
};

class Evaluator {
public:
    Evaluator();
    explicit Evaluator(const ThreatCoefficients& coeffs);
    
    void set_coefficients(const ThreatCoefficients& coeffs);
    const ThreatCoefficients& coefficients() const { return coeffs_; }
    
    int evaluate_position(const Board& board, int row, int col, int color) const;
    // Overload for callers that already ran recognize_position on this square
    // (VCF/VCT/quiescence candidate generators score with the same pattern they
    // used to filter, so re-recognizing inside evaluate_position doubles the
    // direction scans in the engine's hottest loop).
    int evaluate_position_with_pattern(const Board& board, int row, int col,
                                       const PositionPattern& pattern) const;
    int evaluate_position_both(const Board& board, int row, int col, int my_color) const;
    
    int evaluate_board(const Board& board, int my_color) const;
    
    int get_position_weight(int row, int col) const;
    
private:
    ThreatCoefficients coeffs_;
    int position_weights_[BOARD_SIZE][BOARD_SIZE];
    
    void init_position_weights();
    int pattern_score(const PositionPattern& pattern) const;
};

}

#endif

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
    
    // Centre bias per candidate square: the table below runs from 0 at the corner
    // to 14 * this at the centre, so a scale of 8 puts centre and corner 112 points
    // apart - the same order as live_two, which is what makes the preference
    // observable in a comparison of two otherwise equal shapes.
    int position_weight_scale = 8;
    // Multiplier for the stones-on-the-board half of that preference inside
    // evaluate_board. Kept separate from position_weight_scale so training can move
    // the whole-board term without re-tuning the candidate ordering.
    int distance_to_stones = 1;
    // What one closed line end costs: the shape table cannot tell a board edge from
    // an opponent stone, so an edge-hugging 活三 is paid as if it were open. The
    // discount runs over at most four directions times two ends, so the bound that
    // params.h puts on it (0..150) keeps eight closed ends cheaper than the
    // cheapest legal 活四. Against a 冲四 the tighter promise
    // 8 * edge_penalty < rush_four holds at the hand-tuned 100, not for every legal
    // file - a file that trains 冲四 down and the discount up is a parameter set to
    // judge by its games, not one this field can silently absorb.
    int edge_penalty = 100;
    // How many of the "near a stone but no threat there" candidates each layer
    // keeps - the width limit design doc 2.5.1 asks for. Root layer uses
    // cand_width_root, deeper layers cand_width_deep (never more than the root's,
    // which sanitize() enforces); nothing reads them until stage B1's candidate
    // tiers, which is why they are declared here rather than hard-coded there.
    int cand_width_root = 24;
    int cand_width_deep = 8;
    
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
                                       const PositionPattern& pattern, int color) const;
    int evaluate_position_both(const Board& board, int row, int col, int my_color) const;

    // Both sides' candidate scores at one empty square: exactly
    // evaluate_position(board, r, c, color) + evaluate_position(board, r, c,
    // opponent(color)), but from a single shared four-direction pass. Used for
    // move ordering, which always wants both sides of every candidate.
    int evaluate_position_pair(const Board& board, int row, int col, int color) const;

    // The same sum for a caller that already holds both patterns. The tiered
    // candidate generator (design doc 2.5.1) has to look at the patterns anyway to
    // decide which tier a square belongs to, so scoring it must not be a second
    // pass over the four directions.
    int candidate_pair_value(const PositionPattern& mine, const PositionPattern& theirs,
                             int row, int col, int color) const;
    
    int evaluate_board(const Board& board, int my_color) const;
    
    int get_position_weight(int row, int col) const;
    
private:
    ThreatCoefficients coeffs_;
    int position_weights_[BOARD_SIZE][BOARD_SIZE];
    
    void init_position_weights();
    // `row`/`col` are the candidate the pattern was read at: the shape itself does
    // not say where on the board it sits, and a line the edge closes has to be
    // worth less than the same line with room to grow.
    int pattern_score(const PositionPattern& pattern, int row, int col) const;
    // Sum of the centre weights over the stones of one colour, and the price of the
    // ones standing in the outer two rows or columns. Both are read one colour at a
    // time and subtracted from each other by the caller, so the whole-board value
    // stays antisymmetric: eval(W) == -eval(B).
    int position_term(const Board& board, int color) const;
    int edge_penalty_term(const Board& board, int color) const;
    // A recognised candidate pattern's value for its colour: the mate bonus for
    // a five, otherwise the pattern score plus the square's centre bias - except
    // a shape BLACK may not legally play, which is worth nothing.
    int candidate_value(int row, int col, const PositionPattern& pattern, int color) const;
};

}

#endif

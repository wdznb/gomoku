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

// 丁-3（v2 §3.6 S6）：黑白各一套系数。
//
// 指导书 §4 丁-3 实现思路 1 把这个结构写在 `params.h`，这里写在 `evaluator.h`：
// `params.h` 已经 `#include "../core/evaluator.h"`（它要 `ThreatCoefficients` 做基因边界），
// 放在那边会形成一个循环包含，而「一套还是两套系数」是评估器的事实，不是训练器的事实。
//
// 反对称性（`eval(W) == -eval(B)`）是 negamax、TT 的单符号约定与根层比较三处的地基，
// 所以这个结构只允许「按子的颜色取系数」这一种读法：见 `Evaluator::coeffs_for`。
struct CoefficientPair {
    ThreatCoefficients black;
    ThreatCoefficients white;

    CoefficientPair() = default;

    // 两侧同一套 —— 旧文件、旧调用点、以及「等价臂」验收读的都是这一个形状。
    static CoefficientPair shared(const ThreatCoefficients& coeffs) {
        CoefficientPair pair;
        pair.black = coeffs;
        pair.white = coeffs;
        return pair;
    }

    const ThreatCoefficients& for_color(int color) const { return color == BLACK ? black : white; }
    ThreatCoefficients& for_color(int color) { return color == BLACK ? black : white; }

    // 14 个可调量加两个规则常量全部逐字相同。帽、TT 是否要清、以及验收那一口
    // 「等价臂与今天逐字节相同」都问的是这一句，不是「两个对象看着差不多」。
    bool identical() const {
        return black.five == white.five && black.live_four == white.live_four &&
               black.rush_four == white.rush_four && black.live_three == white.live_three &&
               black.sleepy_three == white.sleepy_three && black.live_two == white.live_two &&
               black.sleepy_two == white.sleepy_two && black.double_four == white.double_four &&
               black.double_three == white.double_three && black.four_three == white.four_three &&
               black.forbidden_penalty == white.forbidden_penalty &&
               black.position_weight_scale == white.position_weight_scale &&
               black.distance_to_stones == white.distance_to_stones &&
               black.edge_penalty == white.edge_penalty &&
               black.cand_width_root == white.cand_width_root &&
               black.cand_width_deep == white.cand_width_deep;
    }
};

class Evaluator {
public:
    Evaluator();
    explicit Evaluator(const ThreatCoefficients& coeffs);
    explicit Evaluator(const CoefficientPair& pair);
    
    void set_coefficients(const ThreatCoefficients& coeffs);
    void set_coefficients(const CoefficientPair& pair);
    // 两侧同一套时这就是改之前的那个读数；按色取一律走 `coeffs_for`。
    const ThreatCoefficients& coefficients() const { return pair_.black; }
    const CoefficientPair& coefficient_pair() const { return pair_; }
    // 唯一被允许的按色读法。反对称的支点就在这句与 `pattern_score` 的颜色参数上：
    // 一个形状值多少，取决于**摆它的是哪一色**。
    const ThreatCoefficients& coeffs_for(int color) const { return pair_.for_color(color); }
    
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

    // The two halves of `evaluate_board`, split at the one call it makes per
    // candidate square (design doc 2.6 step 3). A reader that already holds both
    // shapes at a square adds `board_pair_value` for it and finishes with
    // `evaluate_board_from`; `evaluate_board` itself is that same pair of calls with
    // a fresh four-direction pass between them. Neither half is a different
    // evaluation: the loop body, the stone terms and the cap live here and only here,
    // so a cached pass cannot drift from the scan it replaces.
    //
    // Not `candidate_pair_value`, which is the ordering price: that folds in the
    // square's centre weight and pays a flat `coeffs_.five` for a five, while the
    // board sum adds pattern scores - each side judged by the forbidden rule
    // evaluated on its own pattern - and is the number that gets capped.
    int board_pair_value(const PositionPattern& my_pattern, const PositionPattern& opp_pattern,
                         int row, int col, int my_color) const;
    int evaluate_board_from(long long pattern_total, const Board& board, int my_color) const;
    
    // 丁-3：位置项按色可训，所以这张表是两份。旧的两参形式保留成「问行棋方那一侧」，
    // 因为 `candidate_value` 里用它的那一处本来就是在给这一色的候选定价。
    int get_position_weight(int row, int col, int color) const;
    int get_position_weight(int row, int col) const { return get_position_weight(row, col, BLACK); }
    
private:
    CoefficientPair pair_;
    // [colour][row][col]：两侧可以有不同的 `position_weight_scale`，
    // 而这张表在每次换系数时重建，所以它永远不会比 `pair_` 旧。
    // 第一维按**颜色枚举值**索引，而 `BLACK = 1`、`WHITE = 2`（`types.h:12`），
    // 所以它是 3 而不是 2：按 `[2]` 开会让白方那一格写到数组外面去。
    int position_weights_[3][BOARD_SIZE][BOARD_SIZE];
    
    void init_position_weights();
    // `row`/`col` are the candidate the pattern was read at: the shape itself does
    // not say where on the board it sits, and a line the edge closes has to be
    // worth less than the same line with room to grow. `color` is whose stones the
    // pattern describes - that is the one and only place the pair is selected.
    int pattern_score(const PositionPattern& pattern, int row, int col, int color) const;
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

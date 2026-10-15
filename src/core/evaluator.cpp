#include "evaluator.h"
#include <cmath>
#include <algorithm>

namespace gomoku {

namespace {

// How many of a direction's two ends the edge of the board closes at one cell.
// Built at compile time: the answer depends only on the cell, never on the stones.
// An end with room for fewer than two more cells cannot hold the extension the
// shape's price assumes, which is the difference between 贴边活三 and 被封活三 that
// the 18-bit window cannot see (both read the same 2 in the cell beyond the run).
struct BlockedEnds {
    signed char v[BOARD_SIZE][BOARD_SIZE][4];
};

constexpr BlockedEnds build_blocked_ends() {
    BlockedEnds table{};
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            for (int d = 0; d < 4; ++d) {
                const int dr = DIRECTIONS[d][0];
                const int dc = DIRECTIONS[d][1];
                int ends = 0;
                int forward = 0;
                for (int k = 1; k < BOARD_SIZE; ++k) {
                    const int rr = r + dr * k;
                    const int cc = c + dc * k;
                    if (rr < 0 || rr >= BOARD_SIZE || cc < 0 || cc >= BOARD_SIZE) break;
                    ++forward;
                }
                int backward = 0;
                for (int k = 1; k < BOARD_SIZE; ++k) {
                    const int rr = r - dr * k;
                    const int cc = c - dc * k;
                    if (rr < 0 || rr >= BOARD_SIZE || cc < 0 || cc >= BOARD_SIZE) break;
                    ++backward;
                }
                ends = (forward < 2 ? 1 : 0) + (backward < 2 ? 1 : 0);
                table.v[r][c][d] = static_cast<signed char>(ends);
            }
        }
    }
    return table;
}

constexpr BlockedEnds kBlockedEnds = build_blocked_ends();

}

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

int Evaluator::pattern_score(const PositionPattern& pattern, int row, int col) const {
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

    // The shape's price assumes room. A live four / live three / live two is worth
    // what it is because both ends can grow, and the table cannot tell that an end
    // here is the edge of the board rather than a stone, so the correction has to
    // come from where the candidate sits (design doc 2.5.4 - the shape table itself
    // stays untouched, because it is the bitboard/scalar matcher's consistency
    // proof, not a place to put a preference).
    for (int d = 0; d < 4; ++d) {
        const PatternType type = pattern.directions[d].type;
        if (type == PatternType::LIVE_FOUR || type == PatternType::LIVE_THREE ||
            type == PatternType::LIVE_TWO) {
            score -= coeffs_.edge_penalty * kBlockedEnds.v[row][col][d];
        }
    }

    return score;
}

int Evaluator::evaluate_position(const Board& board, int row, int col, int color) const {
    if (!board.is_empty(row, col)) return 0;

    PositionPattern pattern = PatternRecognizer::recognize_position(board, row, col, color);

    return evaluate_position_with_pattern(board, row, col, pattern, color);
}

int Evaluator::evaluate_position_with_pattern(const Board& board, int row, int col,
                                              const PositionPattern& pattern, int color) const {
    if (!board.is_empty(row, col)) return 0;

    return candidate_value(row, col, pattern, color);
}

int Evaluator::candidate_value(int row, int col, const PositionPattern& pattern,
                               int color) const {
    for (int d = 0; d < 4; ++d) {
        if (pattern.directions[d].type == PatternType::FIVE) {
            return coeffs_.five;
        }
    }

    // An exact five is not a forbidden shape (README:27/29), which is why the
    // check above runs first. A double-three / double-four / overline, though, is
    // a stone BLACK can never place, so the threat it describes is worth nothing -
    // the same rule evaluate_board applies to each side of a candidate square.
    if (color == BLACK && PatternRecognizer::is_forbidden_from_pattern(pattern)) return 0;

    int score = pattern_score(pattern, row, col);
    score += get_position_weight(row, col);

    return score;
}

int Evaluator::evaluate_position_pair(const Board& board, int row, int col, int color) const {
    if (!board.is_empty(row, col)) return 0;

    PositionPattern mine, theirs;
    PatternRecognizer::recognize_position_both(board, row, col, color, mine, theirs);

    return candidate_value(row, col, mine, color) +
           candidate_value(row, col, theirs, opponent(color));
}

int Evaluator::evaluate_position_both(const Board& board, int row, int col, int my_color) const {
    if (!board.is_empty(row, col)) return 0;

    if (PatternRecognizer::is_forbidden(board, row, col, my_color)) {
        return -coeffs_.forbidden_penalty;
    }

    return evaluate_position_pair(board, row, col, my_color);
}

int Evaluator::position_term(const Board& board, int color) const {
    // One pass over the stones on the board, not over the candidate list: the
    // preference is about where the army stands, and the candidates are the empty
    // squares around it. move_history() is kept in step by place_stone and
    // undo_last_move, so it is exactly the stones that are there now.
    long long total = 0;
    for (const Move& move : board.move_history()) {
        if (move.color != color) continue;
        total += position_weights_[move.pos.row][move.pos.col];
    }
    return static_cast<int>(total * coeffs_.distance_to_stones);
}

int Evaluator::edge_penalty_term(const Board& board, int color) const {
    long long total = 0;
    for (const Move& move : board.move_history()) {
        if (move.color != color) continue;
        const int row = move.pos.row;
        const int col = move.pos.col;
        int closed = 0;
        if (row <= 1 || row >= BOARD_SIZE - 2) ++closed;
        if (col <= 1 || col >= BOARD_SIZE - 2) ++closed;
        total += closed;
    }
    return static_cast<int>(total * coeffs_.edge_penalty);
}

int Evaluator::evaluate_board(const Board& board, int my_color) const {
    long long my_total = 0;
    long long opp_total = 0;
    
    const auto& candidates = board.active_empty();
    const bool black_is_mine = (my_color == BLACK);

    for (const auto& pos : candidates) {
        // One shared pass over the four directions yields both colours; two
        // recognize_position calls read the same two bitboard lines again.
        PositionPattern my_pattern, opp_pattern;
        PatternRecognizer::recognize_position_both(board, pos.row, pos.col, my_color,
                                                  my_pattern, opp_pattern);

        // pattern_score pays out double_four / double_three / four_three, so a
        // shape BLACK can never legally play would bank points for a stone the
        // move generator refuses to place. Each side is judged on its own pattern:
        // the square may be forbidden for BLACK and a live four for WHITE.
        if (!black_is_mine || !PatternRecognizer::is_forbidden_from_pattern(my_pattern)) {
            my_total += pattern_score(my_pattern, pos.row, pos.col);
        }
        if (black_is_mine || !PatternRecognizer::is_forbidden_from_pattern(opp_pattern)) {
            opp_total += pattern_score(opp_pattern, pos.row, pos.col);
        }
    }

    long long diff = my_total - opp_total;
    // Where the stones stand. The pattern sum only ever speaks about threats a
    // player could still build, so an otherwise quiet board reads as 0 no matter
    // whether the army sits on the centre lines or along the rim - which is why a
    // search with no book move to lean on drifted to the corners. Both terms are
    // taken as a difference between the two colours, so eval(W) == -eval(B) still
    // holds with them in, and the cap below still bounds them.
    diff += position_term(board, my_color);
    diff -= position_term(board, opponent(my_color));
    diff -= edge_penalty_term(board, my_color);
    diff += edge_penalty_term(board, opponent(my_color));
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

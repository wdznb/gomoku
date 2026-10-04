#include "vct.h"
#include <algorithm>
#include <climits>

namespace gomoku {

VCTSearch::VCTSearch(const Evaluator& evaluator, TranspositionTable& tt)
    : evaluator_(evaluator), tt_(tt), node_limit_(500000), nodes_searched_(0) {}

std::vector<Move> VCTSearch::generate_attacks(const Board& board, int color) {
    std::vector<Move> moves;
    const auto& candidates = board.active_empty();

    for (const auto& pos : candidates) {
        if (!board.is_empty(pos.row, pos.col)) continue;

        PositionPattern pattern = PatternRecognizer::recognize_position(board, pos.row, pos.col, color);

        bool is_attack = false;
        for (int d = 0; d < 4; ++d) {
            PatternType t = pattern.directions[d].type;
            if (t == PatternType::RUSH_FOUR || t == PatternType::LIVE_FOUR ||
                t == PatternType::FIVE || t == PatternType::LIVE_THREE) {
                is_attack = true;
                break;
            }
        }

        if (is_attack) {
            moves.push_back(Move(pos.row, pos.col, color));
        }
    }

    std::sort(moves.begin(), moves.end(), [&](const Move& a, const Move& b) {
        int sa = evaluator_.evaluate_position(board, a.pos.row, a.pos.col, color);
        int sb = evaluator_.evaluate_position(board, b.pos.row, b.pos.col, color);
        return sa > sb;
    });

    return moves;
}

std::vector<Move> VCTSearch::generate_defenses(const Board& board, int color,
                                                 int attack_row, int attack_col) {
    std::vector<Move> defenses;
    int opp = opponent(color);
    const auto& candidates = board.active_empty();

    for (const auto& pos : candidates) {
        if (!board.is_empty(pos.row, pos.col)) continue;

        PositionPattern pattern = PatternRecognizer::recognize_position(board, pos.row, pos.col, opp);
        bool is_threat = false;
        for (int d = 0; d < 4; ++d) {
            PatternType t = pattern.directions[d].type;
            if (t == PatternType::FIVE || t == PatternType::LIVE_FOUR || t == PatternType::RUSH_FOUR) {
                is_threat = true;
                break;
            }
        }

        if (is_threat) {
            defenses.push_back(Move(pos.row, pos.col, opp));
        }
    }

    if (defenses.empty()) {
        for (const auto& pos : candidates) {
            if (!board.is_empty(pos.row, pos.col)) continue;
            int dist = std::abs(pos.row - attack_row) + std::abs(pos.col - attack_col);
            if (dist <= 3) {
                defenses.push_back(Move(pos.row, pos.col, opp));
            }
        }
    }

    std::sort(defenses.begin(), defenses.end(), [&](const Move& a, const Move& b) {
        int sa = evaluator_.evaluate_position(board, a.pos.row, a.pos.col, opponent(color));
        int sb = evaluator_.evaluate_position(board, b.pos.row, b.pos.col, opponent(color));
        return sa > sb;
    });

    return defenses;
}

int VCTSearch::vct_defend(Board& board, int color, int depth, int alpha, int beta,
                           std::atomic<bool>& stop_flag) {
    nodes_searched_++;
    if (nodes_searched_ > node_limit_ || stop_flag.load()) return 0;
    if (depth <= 0) return 0;

    auto attacks = generate_attacks(board, color);
    if (attacks.empty()) return 0;

    int best_score = -INT_MAX;

    for (const auto& move : attacks) {
        if (stop_flag.load()) break;

        board.place_stone(move.pos.row, move.pos.col, color);

        if (board.has_five_in_a_row(move.pos.row, move.pos.col, color)) {
            board.remove_stone(move.pos.row, move.pos.col);
            return 100000 + depth;
        }

        PositionPattern pattern = PatternRecognizer::recognize_position(
            board, move.pos.row, move.pos.col, color);
        bool has_live_four = false;
        for (int d = 0; d < 4; ++d) {
            if (pattern.directions[d].type == PatternType::LIVE_FOUR) {
                has_live_four = true;
                break;
            }
        }

        if (has_live_four) {
            board.remove_stone(move.pos.row, move.pos.col);
            return 100000 + depth;
        }

        auto defenses = generate_defenses(board, color, move.pos.row, move.pos.col);
        if (defenses.empty()) {
            board.remove_stone(move.pos.row, move.pos.col);
            return 100000 + depth;
        }

        int worst_for_attacker = INT_MAX;
        for (const auto& def : defenses) {
            board.place_stone(def.pos.row, def.pos.col, def.color);
            int score = vct_attack(board, color, depth - 1, alpha, beta, stop_flag);
            board.remove_stone(def.pos.row, def.pos.col);
            worst_for_attacker = std::min(worst_for_attacker, score);
            beta = std::min(beta, worst_for_attacker);
            if (alpha >= beta) break;
        }

        board.remove_stone(move.pos.row, move.pos.col);

        best_score = std::max(best_score, worst_for_attacker);
        alpha = std::max(alpha, best_score);
        if (alpha >= beta) break;
    }

    return best_score;
}

int VCTSearch::vct_attack(Board& board, int color, int depth, int alpha, int beta,
                           std::atomic<bool>& stop_flag) {
    nodes_searched_++;
    if (nodes_searched_ > node_limit_ || stop_flag.load()) return 0;
    if (depth <= 0) return 0;

    auto attacks = generate_attacks(board, color);
    if (attacks.empty()) return 0;

    int best_score = -INT_MAX;

    for (const auto& move : attacks) {
        if (stop_flag.load()) break;

        board.place_stone(move.pos.row, move.pos.col, color);

        if (board.has_five_in_a_row(move.pos.row, move.pos.col, color)) {
            board.remove_stone(move.pos.row, move.pos.col);
            return 100000 + depth;
        }

        PositionPattern pattern = PatternRecognizer::recognize_position(
            board, move.pos.row, move.pos.col, color);
        bool has_live_four = false;
        for (int d = 0; d < 4; ++d) {
            if (pattern.directions[d].type == PatternType::LIVE_FOUR) {
                has_live_four = true;
                break;
            }
        }

        if (has_live_four) {
            board.remove_stone(move.pos.row, move.pos.col);
            return 100000 + depth;
        }

        auto defenses = generate_defenses(board, color, move.pos.row, move.pos.col);
        if (defenses.empty()) {
            board.remove_stone(move.pos.row, move.pos.col);
            return 100000 + depth;
        }

        int worst_for_attacker = INT_MAX;
        for (const auto& def : defenses) {
            board.place_stone(def.pos.row, def.pos.col, def.color);
            int score = vct_attack(board, color, depth - 1, alpha, beta, stop_flag);
            board.remove_stone(def.pos.row, def.pos.col);
            worst_for_attacker = std::min(worst_for_attacker, score);
            if (worst_for_attacker <= -100000) break;
        }

        board.remove_stone(move.pos.row, move.pos.col);

        if (worst_for_attacker > best_score) {
            best_score = worst_for_attacker;
        }
        alpha = std::max(alpha, best_score);
        if (alpha >= beta) break;
    }

    return best_score;
}

VCTMove VCTSearch::search(const Board& board, int color, int max_depth,
                           std::atomic<bool>& stop_flag) {
    VCTMove result;
    nodes_searched_ = 0;

    Board mutable_board = board;

    auto attacks = generate_attacks(mutable_board, color);
    if (attacks.empty()) {
        result.result = VCTResult::UNKNOWN;
        return result;
    }

    int best_score = -INT_MAX;
    Move best_move;

    for (const auto& move : attacks) {
        if (stop_flag.load()) break;

        mutable_board.place_stone(move.pos.row, move.pos.col, color);

        if (mutable_board.has_five_in_a_row(move.pos.row, move.pos.col, color)) {
            mutable_board.remove_stone(move.pos.row, move.pos.col);
            result.result = VCTResult::WIN;
            result.best_move = move;
            result.variation.push_back(move);
            return result;
        }

        PositionPattern pattern = PatternRecognizer::recognize_position(
            mutable_board, move.pos.row, move.pos.col, color);
        bool has_live_four = false;
        for (int d = 0; d < 4; ++d) {
            if (pattern.directions[d].type == PatternType::LIVE_FOUR) {
                has_live_four = true;
                break;
            }
        }

        if (has_live_four) {
            mutable_board.remove_stone(move.pos.row, move.pos.col);
            result.result = VCTResult::WIN;
            result.best_move = move;
            result.variation.push_back(move);
            return result;
        }

        auto defenses = generate_defenses(mutable_board, color, move.pos.row, move.pos.col);
        if (defenses.empty()) {
            mutable_board.remove_stone(move.pos.row, move.pos.col);
            result.result = VCTResult::WIN;
            result.best_move = move;
            result.variation.push_back(move);
            return result;
        }

        int worst = INT_MAX;
        for (const auto& def : defenses) {
            mutable_board.place_stone(def.pos.row, def.pos.col, def.color);
            int score = vct_attack(mutable_board, color, max_depth - 1,
                                   -INT_MAX, -best_score, stop_flag);
            mutable_board.remove_stone(def.pos.row, def.pos.col);
            worst = std::min(worst, score);
        }

        mutable_board.remove_stone(move.pos.row, move.pos.col);

        if (worst > best_score) {
            best_score = worst;
            best_move = move;
        }
    }

    if (best_score >= 100000) {
        result.result = VCTResult::WIN;
        result.best_move = best_move;
    } else {
        result.result = VCTResult::UNKNOWN;
    }

    return result;
}

}

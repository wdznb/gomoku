#include "vcf.h"
#include <algorithm>
#include <climits>

namespace gomoku {

VCFSearch::VCFSearch(const Evaluator& evaluator, TranspositionTable& tt)
    : evaluator_(evaluator), tt_(tt), node_limit_(1000000), nodes_searched_(0) {}

std::vector<Move> VCFSearch::generate_rush_fours(const Board& board, int color) {
    std::vector<std::pair<int, Move>> scored_moves;
    const auto& candidates = board.active_empty();

    for (const auto& pos : candidates) {
        if (!board.is_empty(pos.row, pos.col)) continue;
        if (color == BLACK && PatternRecognizer::is_forbidden(board, pos.row, pos.col, color)) continue;

        PositionPattern pattern = PatternRecognizer::recognize_position(board, pos.row, pos.col, color);

        bool has_four = false;
        for (int d = 0; d < 4; ++d) {
            PatternType t = pattern.directions[d].type;
            if (t == PatternType::RUSH_FOUR || t == PatternType::LIVE_FOUR || t == PatternType::FIVE) {
                has_four = true;
                break;
            }
        }

        if (has_four) {
            int score = evaluator_.evaluate_position(board, pos.row, pos.col, color);
            scored_moves.emplace_back(score, Move(pos.row, pos.col, color));
        }
    }

    std::sort(scored_moves.begin(), scored_moves.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<Move> moves;
    moves.reserve(scored_moves.size());
    for (auto& p : scored_moves) {
        moves.push_back(std::move(p.second));
    }
    return moves;
}

Move VCFSearch::find_block_move(const Board& board, int row, int col, int color) {
    for (int d = 0; d < 4; ++d) {
        int dr = DIRECTIONS[d][0];
        int dc = DIRECTIONS[d][1];

        int count = 1;
        int empty_r = -1, empty_c = -1;
        int empty_count = 0;

        for (int i = 1; i <= 4; ++i) {
            int r = row + i * dr;
            int c = col + i * dc;
            if (!board.is_valid(r, c)) break;
            if (board.get(r, c) == color) count++;
            else if (board.is_empty(r, c)) {
                empty_r = r;
                empty_c = c;
                empty_count++;
            }
            else break;
        }

        for (int i = 1; i <= 4; ++i) {
            int r = row - i * dr;
            int c = col - i * dc;
            if (!board.is_valid(r, c)) break;
            if (board.get(r, c) == color) count++;
            else if (board.is_empty(r, c)) {
                empty_r = r;
                empty_c = c;
                empty_count++;
            }
            else break;
        }

        if (count == 4 && empty_count == 1) {
            return Move(empty_r, empty_c, opponent(color));
        }
    }

    return Move();
}

int VCFSearch::vcf_attack(Board& board, int color, int depth, int alpha, int beta,
                           std::atomic<bool>& stop_flag) {
    nodes_searched_++;
    if (nodes_searched_ > node_limit_ || stop_flag.load()) return 0;
    if (depth <= 0) return 0;

    auto rush_fours = generate_rush_fours(board, color);
    if (rush_fours.empty()) return 0;

    int best_score = -INT_MAX;

    for (const auto& move : rush_fours) {
        if (stop_flag.load()) break;

        board.place_stone(move.pos.row, move.pos.col, color);

        if (board.has_five_in_a_row(move.pos.row, move.pos.col, color)) {
            board.undo_last_move();
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
            board.undo_last_move();
            return 100000 + depth;
        }

        if (pattern.four_count >= 2) {
            board.undo_last_move();
            return 100000 + depth;
        }

        Move block = find_block_move(board, move.pos.row, move.pos.col, color);
        if (block.pos.row < 0) {
            board.undo_last_move();
            continue;
        }

        if (block.color == BLACK && PatternRecognizer::is_forbidden(board, block.pos.row, block.pos.col, block.color)) {
            board.undo_last_move();
            return 100000 + depth;
        }

        board.place_stone(block.pos.row, block.pos.col, block.color);

        if (board.has_five_in_a_row(block.pos.row, block.pos.col, block.color)) {
            board.undo_last_move();
            board.undo_last_move();
            return -100000 + depth;
        }

        int score = vcf_attack(board, color, depth - 1, alpha, beta, stop_flag);

        board.undo_last_move();
        board.undo_last_move();

        if (score > best_score) {
            best_score = score;
        }
        if (best_score > alpha) {
            alpha = best_score;
        }
        if (alpha >= beta) {
            break;
        }
    }

    return best_score;
}

VCFMove VCFSearch::search(const Board& board, int color, int max_depth,
                           std::atomic<bool>& stop_flag) {
    VCFMove result;
    nodes_searched_ = 0;

    Board mutable_board = board;

    auto rush_fours = generate_rush_fours(mutable_board, color);
    if (rush_fours.empty()) {
        result.result = VCFResult::UNKNOWN;
        return result;
    }

    int best_score = -INT_MAX;
    Move best_move;

    for (const auto& move : rush_fours) {
        if (stop_flag.load()) break;

        mutable_board.place_stone(move.pos.row, move.pos.col, color);

        if (mutable_board.has_five_in_a_row(move.pos.row, move.pos.col, color)) {
            mutable_board.undo_last_move();
            result.result = VCFResult::WIN;
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
            mutable_board.undo_last_move();
            result.result = VCFResult::WIN;
            result.best_move = move;
            result.variation.push_back(move);
            return result;
        }

        if (pattern.four_count >= 2) {
            mutable_board.undo_last_move();
            result.result = VCFResult::WIN;
            result.best_move = move;
            result.variation.push_back(move);
            return result;
        }

        Move block = find_block_move(mutable_board, move.pos.row, move.pos.col, color);
        if (block.pos.row < 0) {
            mutable_board.undo_last_move();
            continue;
        }

        if (block.color == BLACK && PatternRecognizer::is_forbidden(mutable_board, block.pos.row, block.pos.col, block.color)) {
            mutable_board.undo_last_move();
            result.result = VCFResult::WIN;
            result.best_move = move;
            result.variation.push_back(move);
            return result;
        }

        mutable_board.place_stone(block.pos.row, block.pos.col, block.color);

        if (mutable_board.has_five_in_a_row(block.pos.row, block.pos.col, block.color)) {
            mutable_board.undo_last_move();
            mutable_board.undo_last_move();
            continue;
        }

        int score = vcf_attack(mutable_board, color, max_depth - 1,
                                -INT_MAX, INT_MAX, stop_flag);

        mutable_board.undo_last_move();
        mutable_board.undo_last_move();

        if (score > best_score) {
            best_score = score;
            best_move = move;
        }
    }

    if (best_score >= 100000) {
        result.result = VCFResult::WIN;
        result.best_move = best_move;
    } else {
        result.result = VCFResult::UNKNOWN;
    }

    return result;
}

}

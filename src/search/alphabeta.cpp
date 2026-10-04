#include "alphabeta.h"
#include <algorithm>
#include <climits>
#include <chrono>

namespace gomoku {

AlphaBetaSearch::AlphaBetaSearch(const Evaluator& evaluator, TranspositionTable& tt)
    : evaluator_(evaluator), tt_(tt), node_limit_(2000000),
      move_time_limit_ms_(5000), nodes_searched_(0) {}

bool AlphaBetaSearch::is_timeout() const {
    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - start_time_).count();
    return elapsed >= move_time_limit_ms_;
}

std::vector<Move> AlphaBetaSearch::generate_moves(const Board& board, int color) {
    std::vector<Move> moves;
    const auto& candidates = board.active_empty();

    for (const auto& pos : candidates) {
        if (!board.is_empty(pos.row, pos.col)) continue;
        if (PatternRecognizer::is_forbidden(board, pos.row, pos.col, color)) continue;
        moves.push_back(Move(pos.row, pos.col, color));
    }

    return moves;
}

void AlphaBetaSearch::sort_moves(std::vector<Move>& moves, const Board& board, int color,
                                  const Move& tt_move) {
    bool tt_swapped = false;
    if (tt_move.pos.is_valid()) {
        auto it = std::find_if(moves.begin(), moves.end(),
            [&](const Move& m) { return m.pos.row == tt_move.pos.row && m.pos.col == tt_move.pos.col; });
        if (it != moves.begin() && it != moves.end()) {
            std::iter_swap(moves.begin(), it);
            tt_swapped = true;
        }
    }

    std::sort(moves.begin() + (tt_swapped ? 1 : 0), moves.end(),
              [&](const Move& a, const Move& b) {
        int sa = evaluator_.evaluate_position_both(board, a.pos.row, a.pos.col, color);
        int sb = evaluator_.evaluate_position_both(board, b.pos.row, b.pos.col, color);
        return sa > sb;
    });
}

int AlphaBetaSearch::quiescence(Board& board, int alpha, int beta, int color,
                                 std::atomic<bool>& stop_flag) {
    int stand_pat = evaluator_.evaluate_board(board, color);

    if (stand_pat >= beta) return beta;
    if (stand_pat > alpha) alpha = stand_pat;

    const auto& candidates = board.active_empty();
    std::vector<Move> threat_moves;

    for (const auto& pos : candidates) {
        if (!board.is_empty(pos.row, pos.col)) continue;

        PositionPattern pattern = PatternRecognizer::recognize_position(board, pos.row, pos.col, color);
        bool is_threat = false;
        for (int d = 0; d < 4; ++d) {
            PatternType t = pattern.directions[d].type;
            if (t == PatternType::FIVE || t == PatternType::LIVE_FOUR ||
                t == PatternType::RUSH_FOUR || t == PatternType::LIVE_THREE) {
                is_threat = true;
                break;
            }
        }

        if (is_threat) {
            if (color == BLACK && PatternRecognizer::is_forbidden(board, pos.row, pos.col, color)) {
                continue;
            }
            threat_moves.push_back(Move(pos.row, pos.col, color));
        }
    }

    if (threat_moves.empty()) return stand_pat;

    std::sort(threat_moves.begin(), threat_moves.end(), [&](const Move& a, const Move& b) {
        int sa = evaluator_.evaluate_position(board, a.pos.row, a.pos.col, color);
        int sb = evaluator_.evaluate_position(board, b.pos.row, b.pos.col, color);
        return sa > sb;
    });

    for (const auto& move : threat_moves) {
        if (stop_flag.load() || is_timeout()) break;

        board.place_stone(move.pos.row, move.pos.col, color);

        if (board.has_five_in_a_row(move.pos.row, move.pos.col, color)) {
            board.undo_last_move();
            return 100000;
        }

        int score = -quiescence(board, -beta, -alpha, opponent(color), stop_flag);
        board.undo_last_move();

        if (score > alpha) {
            alpha = score;
        }
        if (alpha >= beta) return beta;
    }

    return alpha;
}

int AlphaBetaSearch::alphabeta(Board& board, int depth, int alpha, int beta, int color,
                                std::atomic<bool>& stop_flag) {
    nodes_searched_++;
    if (nodes_searched_ > node_limit_ || stop_flag.load() || is_timeout()) {
        int winner = board.check_winner();
        if (winner == color) return 100000 + depth;
        if (winner == opponent(color)) return -100000 - depth;
        return evaluator_.evaluate_board(board, color);
    }

    uint64_t hash = board.zobrist_hash();
    const TTEntry* entry = tt_.lookup(hash);
    Move tt_move;
    if (entry && entry->depth >= depth) {
        if (entry->flag == TTFlag::EXACT) return entry->value;
        if (entry->flag == TTFlag::LOWERBOUND && entry->value >= beta) return entry->value;
        if (entry->flag == TTFlag::UPPERBOUND && entry->value <= alpha) return entry->value;
    }
    if (entry) {
        tt_move = entry->best_move;
    }

    int winner = board.check_winner();
    if (winner == color) return 100000 + depth;
    if (winner == opponent(color)) return -100000 - depth;
    if (board.is_game_over()) return 0;

    if (depth <= 0) {
        return quiescence(board, alpha, beta, color, stop_flag);
    }

    auto moves = generate_moves(board, color);
    if (moves.empty()) return evaluator_.evaluate_board(board, color);

    sort_moves(moves, board, color, tt_move);

    Move best_move;
    int best_score = -INT_MAX;
    TTFlag flag = TTFlag::UPPERBOUND;

    for (const auto& move : moves) {
        if (stop_flag.load() || is_timeout()) break;

        board.place_stone(move.pos.row, move.pos.col, color);

        int score = -alphabeta(board, depth - 1, -beta, -alpha, opponent(color), stop_flag);

        board.undo_last_move();

        if (score > best_score) {
            best_score = score;
            best_move = move;
        }
        if (score > alpha) {
            alpha = score;
            flag = TTFlag::EXACT;
        }
        if (alpha >= beta) {
            flag = TTFlag::LOWERBOUND;
            break;
        }
    }

    if (best_score > -100000 && best_score < 100000) {
        tt_.store(hash, depth, best_score, best_move, flag);
    }

    return best_score;
}

Move AlphaBetaSearch::search(const Board& board, int color, int max_depth,
                              int time_limit_ms, std::atomic<bool>& stop_flag) {
    move_time_limit_ms_ = time_limit_ms;
    nodes_searched_ = 0;
    start_time_ = std::chrono::high_resolution_clock::now();

    tt_.new_search();

    Move best_move;
    int best_score = 0;

    auto root_moves = generate_moves(board, color);
    if (root_moves.empty()) {
        return Move(BOARD_SIZE / 2, BOARD_SIZE / 2, color);
    }

    if (root_moves.size() == 1) {
        return root_moves[0];
    }

    sort_moves(root_moves, board, color);

    for (int depth = 1; depth <= max_depth; ++depth) {
        if (stop_flag.load() || is_timeout()) break;

        int alpha = -INT_MAX;
        int beta = INT_MAX;
        int depth_best_score = -INT_MAX;
        Move depth_best_move;

        Board mutable_board = board;
        for (const auto& move : root_moves) {
            if (stop_flag.load() || is_timeout()) break;

            mutable_board.place_stone(move.pos.row, move.pos.col, color);

            int score = -alphabeta(mutable_board, depth - 1, -beta, -alpha,
                                   opponent(color), stop_flag);

            mutable_board.undo_last_move();

            if (score > depth_best_score) {
                depth_best_score = score;
                depth_best_move = move;
            }
            if (score > alpha) {
                alpha = score;
            }
        }

        if (!stop_flag.load() && !is_timeout()) {
            best_move = depth_best_move;
            best_score = depth_best_score;
            info_.depth = depth;
            info_.score = best_score;
            info_.best_move = best_move;
        }

        if (best_score >= 100000 || best_score <= -100000) break;
    }

    if (!best_move.pos.is_valid() && !root_moves.empty()) {
        best_move = root_moves[0];
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    info_.elapsed_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time_).count());
    info_.nodes_searched = nodes_searched_;

    return best_move;
}

}

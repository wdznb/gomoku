#include "alphabeta.h"
#include "../core/transforms.h"
#include <algorithm>
#include <climits>
#include <chrono>

namespace gomoku {

AlphaBetaSearch::AlphaBetaSearch(const Evaluator& evaluator, TranspositionTable& tt)
    : evaluator_(evaluator), tt_(tt), node_limit_(2000000),
      move_time_limit_ms_(5000), nodes_searched_(0) {}

bool AlphaBetaSearch::is_timeout() const {
    // The engine shares one absolute budget between VCF, VCT and this phase and
    // hands us only the slice left. Once that deadline has passed the whole
    // move is over budget, so stop instead of running another slice. Without a
    // deadline the per-search limit is all we have.
    if (has_deadline_) {
        return std::chrono::steady_clock::now() >= deadline_;
    }
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
    // The TT move earns its place by the previous search, so it must stay at
    // index 0 and out of the heuristic re-ordering below. Looking for it
    // anywhere but the front used to leave it unpinned when it already was the
    // first move, which is exactly the common case.
    bool tt_swapped = false;
    if (tt_move.pos.is_valid()) {
        auto it = std::find_if(moves.begin(), moves.end(),
            [&](const Move& m) { return m.pos.row == tt_move.pos.row && m.pos.col == tt_move.pos.col; });
        if (it != moves.end()) {
            if (it != moves.begin()) std::iter_swap(moves.begin(), it);
            tt_swapped = true;
        }
    }

    // evaluate_position_both runs four pattern recognitions per call; the
    // comparator used to invoke it O(n log n) times per node. Score once up
    // front and sort the pairs instead.
    //
    // Callers pass a move list that generate_moves has already filtered
    // against is_forbidden for BLACK, so calling evaluate_position_both here
    // would rerun is_forbidden (which does a second recognize_combined) on
    // every node of every ply. Score both sides directly and skip that
    // redundant work.
    int start = tt_swapped ? 1 : 0;
    int count = static_cast<int>(moves.size()) - start;
    if (count <= 1) return;

    std::vector<std::pair<int, size_t>> scored;
    scored.reserve(count);
    for (int i = 0; i < count; ++i) {
        const Move& m = moves[start + i];
        int s = evaluator_.evaluate_position_pair(board, m.pos.row, m.pos.col, color);
        scored.emplace_back(s, static_cast<size_t>(start + i));
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<Move> reordered(moves.size());
    for (size_t i = 0; i < static_cast<size_t>(start); ++i) reordered[i] = moves[i];
    for (size_t i = 0; i < scored.size(); ++i) {
        reordered[start + i] = moves[scored[i].second];
    }
    moves = std::move(reordered);
}

int AlphaBetaSearch::quiescence(Board& board, int alpha, int beta, int color,
                                 std::atomic<bool>& stop_flag) {
    // Quiescence can recurse for tens of moves on a still-live position. Without
    // a node increment here, node_limit_ only bounds the alphabeta tree and a
    // timeout or abort flag set at a shallow depth can still be trapped inside
    // an unbounded quiescence subtree.
    nodes_searched_++;
    if (nodes_searched_ > node_limit_ || stop_flag.load() || is_timeout()) {
        return evaluator_.evaluate_board(board, color);
    }

    int stand_pat = evaluator_.evaluate_board(board, color);

    // Fail-soft on the stand-pat cutoff: returning `beta` (the window edge)
    // would let the parent store that bound as TTFlag::EXACT. Returning
    // stand_pat is the node's true lower value and stays sound when the
    // parent records it after any alpha/beta adjustment.
    if (stand_pat >= beta) return stand_pat;
    if (stand_pat > alpha) alpha = stand_pat;

    const auto& candidates = board.active_empty();
    std::vector<std::pair<int, Move>> scored;

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
            // Score from the pattern we just recognized; calling
            // evaluate_position here would re-run the same four scans.
            int s = evaluator_.evaluate_position_with_pattern(board, pos.row, pos.col, pattern);
            scored.emplace_back(s, Move(pos.row, pos.col, color));
        }
    }

    if (scored.empty()) return stand_pat;

    // Decorate-sort-undecorate: scores are already attached, so the sort does
    // no pattern recognition.
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    for (const auto& entry : scored) {
        const Move& move = entry.second;
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
        // Fail-soft on the beta cutoff. Returning `alpha` (which now holds
        // the best score seen) rather than `beta` avoids poisoning the TT
        // with a value that is only a bound.
        if (alpha >= beta) return alpha;
    }

    return alpha;
}

int AlphaBetaSearch::alphabeta(Board& board, int depth, int alpha, int beta, int color,
                                std::atomic<bool>& stop_flag) {
    nodes_searched_++;
    if (nodes_searched_ > node_limit_ || stop_flag.load() || is_timeout()) {
        // Only the last-placed stone can have produced a five; check_winner()
        // would scan all 225 cells and then is_game_over() would scan them
        // again. This abort path fires very frequently during iterative
        // deepening, so the last-move check is important.
        const Move& lm = board.last_move();
        if (lm.pos.is_valid() &&
            board.has_five_in_a_row(lm.pos.row, lm.pos.col, lm.color)) {
            if (lm.color == color) return 100000 + depth;
            return -100000 - depth;
        }
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

    // Terminal check: only the last move can have produced a five.
    const Move& lm = board.last_move();
    if (lm.pos.is_valid() &&
        board.has_five_in_a_row(lm.pos.row, lm.pos.col, lm.color)) {
        if (lm.color == color) return 100000 + depth;
        return -100000 - depth;
    }
    if (board.move_count() >= MAX_MOVES) return 0;

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
        if (stop_flag.load() || is_timeout() || nodes_searched_ > node_limit_) {
            // Search aborted: siblings went unsearched, so only a LOWERBOUND
            // on the true value is sound. Never store EXACT or UPPERBOUND here.
            flag = TTFlag::LOWERBOUND;
            break;
        }

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
    // Reset the reported info each search. An aborted or early-return search
    // must not hand the caller the previous search's depth/score.
    info_ = SearchInfo{};
    start_time_ = std::chrono::high_resolution_clock::now();
    // Reset the reported info each search. If every iteration is aborted (a
    // client-supplied timeout of 0 makes is_timeout() true immediately) or the
    // single-move / empty-board fast paths return early, the caller still
    // reads last_search_info(); without this it sees the previous search's
    // depth/score or an indeterminate value on the first call.
    info_ = SearchInfo{};

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

    // Symmetric positions are searched once per mirror otherwise. When the
    // board maps onto itself under a D4 variant, every candidate has twins that
    // score alike, branch alike and win alike, so keep only the lowest-index
    // cell of each orbit. Positions with no self-symmetry - almost every
    // midgame - are untouched, which is what keeps the chosen move identical to
    // what the undeduplicated root would have played.
    const std::vector<int> symmetries = self_symmetries(board);
    if (symmetries.size() > 1) {
        root_moves.erase(
            std::remove_if(root_moves.begin(), root_moves.end(),
                           [&](const Move& m) {
                               return !is_symmetry_representative(symmetries, m.pos);
                           }),
            root_moves.end());
    }

    sort_moves(root_moves, board, color);

    for (int depth = 1; depth <= max_depth; ++depth) {
        if (stop_flag.load() || is_timeout()) break;

        int alpha = -INT_MAX;
        int beta = INT_MAX;
        int depth_best_score = -INT_MAX;
        Move depth_best_move;

        // Re-copied per iteration on purpose. undo_last_move() restores the
        // candidate list by swap-remove, so a board that has already played and
        // undone this iteration's root moves holds active_empty() in a different
        // order than a fresh copy, and the score ties in sort_moves break
        // against that order. Reusing one board across depths would quietly
        // change which equal-valued moves get searched first.
        Board mutable_board = board;
        for (const auto& move : root_moves) {
            if (stop_flag.load() || is_timeout() || nodes_searched_ > node_limit_) break;

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

        if (!stop_flag.load() && !is_timeout() && nodes_searched_ <= node_limit_) {
            best_move = depth_best_move;
            best_score = depth_best_score;
            info_.depth = depth;
            info_.score = best_score;
            info_.best_move = best_move;

            // Feed the shallow iteration's winner back into the root order so
            // deeper iterations inherit the cutoff / TT-move effect that makes
            // iterative deepening worth the extra work.
            if (depth_best_move.pos.is_valid()) {
                auto it = std::find_if(root_moves.begin(), root_moves.end(),
                    [&](const Move& m) {
                        return m.pos.row == depth_best_move.pos.row &&
                               m.pos.col == depth_best_move.pos.col;
                    });
                if (it != root_moves.begin() && it != root_moves.end()) {
                    std::iter_swap(root_moves.begin(), it);
                }
            }
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

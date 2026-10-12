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
        // Recognize once and reuse: is_forbidden would run the same 4-direction
        // scan a second time on a cell this loop already needs the pattern for.
        PositionPattern pattern = PatternRecognizer::recognize_position(board, pos.row, pos.col, color);
        if (color == BLACK && PatternRecognizer::is_forbidden_from_pattern(pattern)) continue;

        bool has_four = false;
        for (int d = 0; d < 4; ++d) {
            PatternType t = pattern.directions[d].type;
            if (t == PatternType::RUSH_FOUR || t == PatternType::LIVE_FOUR || t == PatternType::FIVE) {
                has_four = true;
                break;
            }
        }

        if (has_four) {
            int score = evaluator_.evaluate_position_with_pattern(board, pos.row, pos.col, pattern);
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

        // Find forward extent of consecutive stones of `color`
        int fwd_r = row, fwd_c = col;
        for (int i = 1; i <= 4; ++i) {
            int r = row + i * dr;
            int c = col + i * dc;
            if (!board.is_valid(r, c)) break;
            if (board.get(r, c) == color) { count++; fwd_r = r; fwd_c = c; }
            else break;
        }

        // Find backward extent of consecutive stones of `color`
        int bwd_r = row, bwd_c = col;
        for (int i = 1; i <= 4; ++i) {
            int r = row - i * dr;
            int c = col - i * dc;
            if (!board.is_valid(r, c)) break;
            if (board.get(r, c) == color) { count++; bwd_r = r; bwd_c = c; }
            else break;
        }

        if (count >= 4) {
            // Check the two endpoints of the consecutive group
            int check_fwd_r = fwd_r + dr, check_fwd_c = fwd_c + dc;
            int check_bwd_r = bwd_r - dr, check_bwd_c = bwd_c - dc;

            bool fwd_open = board.is_valid(check_fwd_r, check_fwd_c) &&
                            board.is_empty(check_fwd_r, check_fwd_c);
            bool bwd_open = board.is_valid(check_bwd_r, check_bwd_c) &&
                            board.is_empty(check_bwd_r, check_bwd_c);

            if (fwd_open && !bwd_open)
                return Move(check_fwd_r, check_fwd_c, opponent(color));
            if (bwd_open && !fwd_open)
                return Move(check_bwd_r, check_bwd_c, opponent(color));
        } else {
            // A gap four (XXX_X, XX_XX, X_XXX): the stones on the far side of one
            // empty cell make the run four, so that cell is the only point that
            // completes five and therefore the forced block.
            const int ends[2][2] = {{fwd_r, fwd_c}, {bwd_r, bwd_c}};
            const int signs[2] = {1, -1};
            for (int e = 0; e < 2; ++e) {
                const int sign = signs[e];
                const int hole_r = ends[e][0] + sign * dr;
                const int hole_c = ends[e][1] + sign * dc;
                if (!board.is_valid(hole_r, hole_c) || !board.is_empty(hole_r, hole_c)) continue;

                int beyond_r = hole_r + sign * dr;
                int beyond_c = hole_c + sign * dc;
                int beyond = 0;
                while (beyond < 4 && board.is_valid(beyond_r, beyond_c) &&
                       board.get(beyond_r, beyond_c) == color) {
                    ++beyond;
                    beyond_r += sign * dr;
                    beyond_c += sign * dc;
                }
                if (count + beyond == 4) {
                    return Move(hole_r, hole_c, opponent(color));
                }
            }
        }
    }

    return Move();
}

int VCFSearch::vcf_attack(Board& board, int color, int depth, int alpha, int beta,
                           std::atomic<bool>& stop_flag) {
    nodes_searched_++;
    if (nodes_searched_ > node_limit_ || aborted(stop_flag)) return 0;
    if (depth <= 0) return 0;

    auto rush_fours = generate_rush_fours(board, color);
    if (rush_fours.empty()) return 0;

    int best_score = -INT_MAX;

    for (const auto& move : rush_fours) {
        if (aborted(stop_flag)) break;

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

        // The block stone may hand the defender a decisive tempo of their own:
        // a completed five wins outright; a live or rush four lets the defender
        // finish their own attack before the attacker's VCF sequence matures.
        // Because find_block_move returns the unique forced block, this outcome
        // sinks only the current attacker candidate - other attacks may force a
        // different block. Skip this branch and try the next.
        bool defender_has_tempo = false;
        if (board.has_five_in_a_row(block.pos.row, block.pos.col, block.color)) {
            defender_has_tempo = true;
        } else {
            PositionPattern block_pattern = PatternRecognizer::recognize_position(
                board, block.pos.row, block.pos.col, block.color);
            for (int d = 0; d < 4; ++d) {
                PatternType t = block_pattern.directions[d].type;
                if (t == PatternType::LIVE_FOUR || t == PatternType::RUSH_FOUR) {
                    defender_has_tempo = true;
                    break;
                }
            }
            if (!defender_has_tempo && block_pattern.four_count >= 2) {
                defender_has_tempo = true;
            }
        }

        if (defender_has_tempo) {
            board.undo_last_move();
            board.undo_last_move();
            continue;
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
    nodes_searched_ = 0;
    // One copy for the whole search: search_impl plays and unhands its
    // candidates on it, so the win-attempt loop needs no per-candidate clone.
    Board working = board;
    return search_impl(working, color, max_depth, stop_flag);
}

VCFMove VCFSearch::search_impl(Board& board, int color, int max_depth,
                                std::atomic<bool>& stop_flag) {
    VCFMove result;

    auto rush_fours = generate_rush_fours(board, color);
    if (rush_fours.empty()) {
        result.result = VCFResult::UNKNOWN;
        return result;
    }

    int best_score = -INT_MAX;
    Move best_move;

    for (const auto& move : rush_fours) {
        if (aborted(stop_flag)) break;

        board.place_stone(move.pos.row, move.pos.col, color);

        if (board.has_five_in_a_row(move.pos.row, move.pos.col, color)) {
            board.undo_last_move();
            result.result = VCFResult::WIN;
            result.best_move = move;
            result.variation.push_back(move);
            return result;
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
            result.result = VCFResult::WIN;
            result.best_move = move;
            result.variation.push_back(move);
            return result;
        }

        if (pattern.four_count >= 2) {
            board.undo_last_move();
            result.result = VCFResult::WIN;
            result.best_move = move;
            result.variation.push_back(move);
            return result;
        }

        Move block = find_block_move(board, move.pos.row, move.pos.col, color);
        if (block.pos.row < 0) {
            board.undo_last_move();
            continue;
        }

        if (block.color == BLACK && PatternRecognizer::is_forbidden(board, block.pos.row, block.pos.col, block.color)) {
            board.undo_last_move();
            result.result = VCFResult::WIN;
            result.best_move = move;
            result.variation.push_back(move);
            return result;
        }

        board.place_stone(block.pos.row, block.pos.col, block.color);

        // Same tempo check as vcf_attack: a block that hands the defender a
        // five, a live four, a rush four, or a double-four invalidates this
        // attacker candidate; skip and try the next attack.
        bool defender_has_tempo = false;
        if (board.has_five_in_a_row(block.pos.row, block.pos.col, block.color)) {
            defender_has_tempo = true;
        } else {
            PositionPattern block_pattern = PatternRecognizer::recognize_position(
                board, block.pos.row, block.pos.col, block.color);
            for (int d = 0; d < 4; ++d) {
                PatternType t = block_pattern.directions[d].type;
                if (t == PatternType::LIVE_FOUR || t == PatternType::RUSH_FOUR) {
                    defender_has_tempo = true;
                    break;
                }
            }
            if (!defender_has_tempo && block_pattern.four_count >= 2) {
                defender_has_tempo = true;
            }
        }

        if (defender_has_tempo) {
            board.undo_last_move();
            board.undo_last_move();
            continue;
        }

        int score = vcf_attack(board, color, max_depth - 1,
                                -INT_MAX, INT_MAX, stop_flag);

        board.undo_last_move();
        board.undo_last_move();

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

bool VCFSearch::has_decisive_tempo(const Board& board, int row, int col, int color) const {
    // Recognize once and reuse: the forbidden test and the tempo loop below
    // both consume the same 4-direction pattern. The old path ran
    // is_forbidden (a full recognize_position) and then recognize_position
    // again for BLACK - two identical scans per candidate.
    PositionPattern pattern = PatternRecognizer::recognize_position(board, row, col, color);

    if (color == BLACK && PatternRecognizer::is_forbidden_from_pattern(pattern)) {
        return false;
    }

    for (int d = 0; d < 4; ++d) {
        PatternType t = pattern.directions[d].type;
        if (t == PatternType::FIVE || t == PatternType::LIVE_FOUR ||
            t == PatternType::RUSH_FOUR) {
            return true;
        }
    }

    return false;
}

DefenseResult VCFSearch::find_defense(const Board& board, int defender, int max_depth,
                                      std::atomic<bool>& stop_flag, Move& defense_move) {
    nodes_searched_ = 0;
    defense_move = Move();

    int attacker = opponent(defender);
    int legal_moves = 0;

    for (const auto& pos : board.active_empty()) {
        if (!board.is_empty(pos.row, pos.col)) continue;
        if (defender == BLACK &&
            PatternRecognizer::is_forbidden(board, pos.row, pos.col, defender)) {
            continue;
        }

        legal_moves++;

        // Making five wins outright; making a live four forces the attacker to
        // answer it, which invalidates a win proof built on them keeping the
        // initiative. A rush four also grants a winning tempo because the
        // defender can complete the five on their next turn unless the attacker
        // spends a move blocking it. Reporting SURVIVES here is the safe
        // direction: the cost of being wrong is a missed resignation, not a
        // false one.
        if (has_decisive_tempo(board, pos.row, pos.col, defender)) {
            defense_move = Move(pos, defender);
            return DefenseResult::SURVIVES;
        }

        // A fresh copy per reply, not one board played and undone across the
        // loop: undo_last_move() restores the candidate list by swap-remove, so
        // a reused board hands search_impl a different active_empty() order for
        // each reply, and the ties in threat ordering break against that order.
        Board after = board;
        after.place_stone(pos.row, pos.col, defender);

        VCFMove attacker_result = search_impl(after, attacker, max_depth, stop_flag);

        if (aborted(stop_flag) || nodes_searched_ > node_limit_) {
            return DefenseResult::UNKNOWN;
        }

        if (attacker_result.result != VCFResult::WIN) {
            defense_move = Move(pos, defender);
            return DefenseResult::SURVIVES;
        }
    }

    if (legal_moves == 0) {
        return DefenseResult::UNKNOWN;
    }

    return DefenseResult::PROVEN_LOSS;
}

}

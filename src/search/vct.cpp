#include "vct.h"
#include <algorithm>
#include <climits>

namespace gomoku {

VCTSearch::VCTSearch(const Evaluator& evaluator, TranspositionTable& tt)
    : evaluator_(evaluator), tt_(tt), node_limit_(500000), nodes_searched_(0) {}

std::vector<Move> VCTSearch::generate_attacks(const Board& board, int color) {
    std::vector<std::pair<int, Move>> scored_moves;
    const auto& candidates = board.active_empty();

    for (const auto& pos : candidates) {
        if (!board.is_empty(pos.row, pos.col)) continue;
        // Recognize once and reuse: is_forbidden would run the same 4-direction
        // scan a second time on a cell this loop already needs the pattern for.
        PositionPattern pattern = PatternRecognizer::recognize_position(board, pos.row, pos.col, color);
        if (color == BLACK && PatternRecognizer::is_forbidden_from_pattern(pattern)) continue;

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

std::vector<Move> VCTSearch::generate_defenses(const Board& board, int color,
                                                 int attack_row, int attack_col) {
    int opp = opponent(color);
    std::vector<std::pair<int, Move>> scored_defenses;
    const auto& candidates = board.active_empty();

    for (const auto& pos : candidates) {
        if (!board.is_empty(pos.row, pos.col)) continue;
        PositionPattern pattern = PatternRecognizer::recognize_position(board, pos.row, pos.col, opp);
        // The forbidden test reads the same four directions the recognition above
        // already scanned, so it takes the pattern instead of re-running them.
        if (opp == BLACK && PatternRecognizer::is_forbidden_from_pattern(pattern)) continue;

        bool is_threat = false;
        for (int d = 0; d < 4; ++d) {
            PatternType t = pattern.directions[d].type;
            if (t == PatternType::FIVE || t == PatternType::LIVE_FOUR || t == PatternType::RUSH_FOUR) {
                is_threat = true;
                break;
            }
        }

        if (is_threat) {
            int score = evaluator_.evaluate_position_with_pattern(board, pos.row, pos.col, pattern);
            scored_defenses.emplace_back(score, Move(pos.row, pos.col, opp));
        }
    }

    // Merge the proximity blocks into the counter-threat set unconditionally
    // (deduplicated). The old "only when no counter-threat exists" rule let a
    // single anywhere-on-board reply exclude the real blocking squares, so
    // vct_defend/vct_attack minned over an incomplete defense set and proved
    // fabricated 100000+depth wins. Adding candidates can only ever remove
    // such false wins, never create one (same monotone-safe argument as the
    // Chebyshev note below).
    for (const auto& pos : candidates) {
        if (!board.is_empty(pos.row, pos.col)) continue;
        if (opp == BLACK && PatternRecognizer::is_forbidden(board, pos.row, pos.col, opp)) continue;
        // Chebyshev, not Manhattan: the unique completion cell of a four the
        // attacker just threatens is at most 4 steps ALONG the line, and a
        // diagonal line puts that cell at Manhattan distance 8 (e.g. 4 diagonal
        // steps). A Manhattan <= 4 filter drops those diagonal blocks, leaving
        // defenses empty, which vct_defend/vct_attack read as "no defense" and
        // return a 100000+depth win that search_engine then commits without
        // AlphaBeta - a fabricated forced win. Chebyshev <= 4 covers every
        // direction's completion cell while only ever ADDING candidate blocks
        // (monotone-safe: strictly reduces false wins, never invents one).
        int dist = std::max(std::abs(pos.row - attack_row),
                            std::abs(pos.col - attack_col));
        if (dist <= 4) {
            bool already_listed = false;
            for (const auto& d : scored_defenses) {
                if (d.second.pos.row == pos.row && d.second.pos.col == pos.col) {
                    already_listed = true;
                    break;
                }
            }
            if (already_listed) continue;
            int score = evaluator_.evaluate_position(board, pos.row, pos.col, opp);
            scored_defenses.emplace_back(score, Move(pos.row, pos.col, opp));
        }
    }

    std::sort(scored_defenses.begin(), scored_defenses.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<Move> defenses;
    defenses.reserve(scored_defenses.size());
    for (auto& p : scored_defenses) {
        defenses.push_back(std::move(p.second));
    }
    return defenses;
}

int VCTSearch::vct_defend(Board& board, int color, int depth, int alpha, int beta,
                           std::atomic<bool>& stop_flag) {
    nodes_searched_++;
    if (nodes_searched_ > node_limit_ || aborted(stop_flag)) return 0;
    if (depth <= 0) return 0;

    auto attacks = generate_attacks(board, color);
    if (attacks.empty()) return 0;

    // vct_defend is entered after the defender's reply: it is the attacker's
    // turn again, so the outer loop must maximise over attacker candidates
    // (the previous std::min made this node pretend the attacker was picking
    // their worst move).
    int best_score = -INT_MAX;
    bool evaluated_any = false;

    for (const auto& move : attacks) {
        if (aborted(stop_flag)) break;
        evaluated_any = true;

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

        auto defenses = generate_defenses(board, color, move.pos.row, move.pos.col);
        if (defenses.empty()) {
            board.undo_last_move();
            return 100000 + depth;
        }

        int worst_for_attacker = INT_MAX;
        for (const auto& def : defenses) {
            board.place_stone(def.pos.row, def.pos.col, def.color);

            // A defense that hands the defender a five, a live four, a rush
            // four, or a double four takes the initiative back: the attacker
            // has to spend the next move answering rather than continuing the
            // VCF sequence. This sinks only the current attack candidate -
            // other attacks may force a different block, so skip the whole
            // attack rather than the node.
            bool defender_has_tempo =
                board.has_five_in_a_row(def.pos.row, def.pos.col, def.color);
            if (!defender_has_tempo) {
                PositionPattern def_pattern = PatternRecognizer::recognize_position(
                    board, def.pos.row, def.pos.col, def.color);
                for (int d = 0; d < 4 && !defender_has_tempo; ++d) {
                    PatternType t = def_pattern.directions[d].type;
                    if (t == PatternType::LIVE_FOUR || t == PatternType::RUSH_FOUR) {
                        defender_has_tempo = true;
                    }
                }
                if (!defender_has_tempo && def_pattern.four_count >= 2) {
                    defender_has_tempo = true;
                }
            }

            if (defender_has_tempo) {
                board.undo_last_move();
                worst_for_attacker = -100000 - depth;
                break;
            }

            int score = vct_attack(board, color, depth - 1, alpha, beta, stop_flag);
            board.undo_last_move();
            worst_for_attacker = std::min(worst_for_attacker, score);
            if (worst_for_attacker <= -100000) break;
        }

        board.undo_last_move();

        if (worst_for_attacker > best_score) {
            best_score = worst_for_attacker;
        }
        alpha = std::max(alpha, best_score);
        if (alpha >= beta) break;
    }

    if (!evaluated_any) return 0;
    return best_score;
}

int VCTSearch::vct_attack(Board& board, int color, int depth, int alpha, int beta,
                           std::atomic<bool>& stop_flag) {
    nodes_searched_++;
    if (nodes_searched_ > node_limit_ || aborted(stop_flag)) return 0;
    if (depth <= 0) return 0;

    auto attacks = generate_attacks(board, color);
    if (attacks.empty()) return 0;

    int best_score = -INT_MAX;
    bool evaluated_any = false;

    for (const auto& move : attacks) {
        if (aborted(stop_flag)) break;
        evaluated_any = true;

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

        auto defenses = generate_defenses(board, color, move.pos.row, move.pos.col);
        if (defenses.empty()) {
            board.undo_last_move();
            return 100000 + depth;
        }

        int worst_for_attacker = INT_MAX;
        for (const auto& def : defenses) {
            board.place_stone(def.pos.row, def.pos.col, def.color);

            bool defender_has_tempo =
                board.has_five_in_a_row(def.pos.row, def.pos.col, def.color);
            if (!defender_has_tempo) {
                PositionPattern def_pattern = PatternRecognizer::recognize_position(
                    board, def.pos.row, def.pos.col, def.color);
                for (int d = 0; d < 4 && !defender_has_tempo; ++d) {
                    PatternType t = def_pattern.directions[d].type;
                    if (t == PatternType::LIVE_FOUR || t == PatternType::RUSH_FOUR) {
                        defender_has_tempo = true;
                    }
                }
                if (!defender_has_tempo && def_pattern.four_count >= 2) {
                    defender_has_tempo = true;
                }
            }

            // Same rule as vct_defend: a defender tempo invalidates this
            // specific attack move, not the whole node. Other attack
            // candidates may force a different block. Break out of the
            // defense loop, sink this attack, and continue the outer loop.
            if (defender_has_tempo) {
                board.undo_last_move();
                worst_for_attacker = -100000 - depth;
                break;
            }

            int score = vct_defend(board, color, depth - 1, alpha, beta, stop_flag);
            board.undo_last_move();
            worst_for_attacker = std::min(worst_for_attacker, score);
            if (worst_for_attacker <= -100000) break;
        }

        board.undo_last_move();

        if (worst_for_attacker > best_score) {
            best_score = worst_for_attacker;
        }
        alpha = std::max(alpha, best_score);
        if (alpha >= beta) break;
    }

    if (!evaluated_any) return 0;
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
        if (aborted(stop_flag)) break;

        mutable_board.place_stone(move.pos.row, move.pos.col, color);

        if (mutable_board.has_five_in_a_row(move.pos.row, move.pos.col, color)) {
            mutable_board.undo_last_move();
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
            mutable_board.undo_last_move();
            result.result = VCTResult::WIN;
            result.best_move = move;
            result.variation.push_back(move);
            return result;
        }

        if (pattern.four_count >= 2) {
            mutable_board.undo_last_move();
            result.result = VCTResult::WIN;
            result.best_move = move;
            result.variation.push_back(move);
            return result;
        }

        auto defenses = generate_defenses(mutable_board, color, move.pos.row, move.pos.col);
        if (defenses.empty()) {
            mutable_board.undo_last_move();
            result.result = VCTResult::WIN;
            result.best_move = move;
            result.variation.push_back(move);
            return result;
        }

        int worst = INT_MAX;
        for (const auto& def : defenses) {
            mutable_board.place_stone(def.pos.row, def.pos.col, def.color);

            // Same counter-threat check as vct_attack/vct_defend: if the
            // defense gives the defender a five, a live four, a rush four,
            // or a double four the attacker has lost the initiative and this
            // VCT candidate is invalid.
            bool defender_has_tempo =
                mutable_board.has_five_in_a_row(def.pos.row, def.pos.col, def.color);
            if (!defender_has_tempo) {
                PositionPattern def_pattern = PatternRecognizer::recognize_position(
                    mutable_board, def.pos.row, def.pos.col, def.color);
                for (int d = 0; d < 4 && !defender_has_tempo; ++d) {
                    PatternType t = def_pattern.directions[d].type;
                    if (t == PatternType::LIVE_FOUR || t == PatternType::RUSH_FOUR) {
                        defender_has_tempo = true;
                    }
                }
                if (!defender_has_tempo && def_pattern.four_count >= 2) {
                    defender_has_tempo = true;
                }
            }

            if (defender_has_tempo) {
                mutable_board.undo_last_move();
                worst = -100000 - max_depth;
                break;
            }

            int score = vct_defend(mutable_board, color, max_depth - 1,
                                   -INT_MAX, INT_MAX, stop_flag);
            mutable_board.undo_last_move();
            worst = std::min(worst, score);
        }

        mutable_board.undo_last_move();

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

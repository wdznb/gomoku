#include "vct.h"
#include <algorithm>
#include <climits>

namespace gomoku {

VCTSearch::VCTSearch(const Evaluator& evaluator, TranspositionTable& tt)
    : evaluator_(evaluator), tt_(tt), node_limit_(500000), nodes_searched_(0) {}

void VCTSearch::collect_attacks(const Board& board, int color,
                                 std::vector<std::pair<int, Move>>& scored_moves) const {
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
            int score = evaluator_.evaluate_position_with_pattern(board, pos.row, pos.col, pattern,
                                                                  color);
            scored_moves.emplace_back(score, Move(pos.row, pos.col, color));
        }
    }

    std::sort(scored_moves.begin(), scored_moves.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
}

const std::vector<Move>& VCTSearch::generate_attacks(const Board& board, int color, int depth,
                                                      std::vector<Move>& overflow) {
    if (depth < 0 || depth >= kMoveSlotCount) {
        std::vector<std::pair<int, Move>> scored;
        collect_attacks(board, color, scored);
        overflow.clear();
        overflow.reserve(scored.size());
        for (auto& p : scored) {
            overflow.push_back(std::move(p.second));
        }
        return overflow;
    }

    MoveSlots& slot = attack_slots_[depth];
    slot.scored.clear();
    collect_attacks(board, color, slot.scored);
    slot.moves.clear();
    slot.moves.reserve(slot.scored.size());
    for (auto& p : slot.scored) {
        slot.moves.push_back(std::move(p.second));
    }
    return slot.moves;
}

void VCTSearch::collect_defenses(const Board& board, int color, int attack_row, int attack_col,
                                  std::vector<std::pair<int, Move>>& scored_defenses) const {
    int opp = opponent(color);
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
            int score = evaluator_.evaluate_position_with_pattern(board, pos.row, pos.col, pattern,
                                                                  opp);
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
        if (dist > 4) continue;
        bool already_listed = false;
        for (const auto& d : scored_defenses) {
            if (d.second.pos.row == pos.row && d.second.pos.col == pos.col) {
                already_listed = true;
                break;
            }
        }
        if (already_listed) continue;
        // Both tests above are side-effect-free, so the two cheap ones now run
        // before this one: is_forbidden re-scans the four directions per call,
        // and the squares it is asked about are down to the 9x9 window around
        // the attack point instead of every candidate.
        if (opp == BLACK && PatternRecognizer::is_forbidden(board, pos.row, pos.col, opp)) continue;
        int score = evaluator_.evaluate_position(board, pos.row, pos.col, opp);
        scored_defenses.emplace_back(score, Move(pos.row, pos.col, opp));
    }

    std::sort(scored_defenses.begin(), scored_defenses.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
}

const std::vector<Move>& VCTSearch::generate_defenses(const Board& board, int color,
                                                      int attack_row, int attack_col, int depth,
                                                      std::vector<Move>& overflow) {
    if (depth < 0 || depth >= kMoveSlotCount) {
        std::vector<std::pair<int, Move>> scored;
        collect_defenses(board, color, attack_row, attack_col, scored);
        overflow.clear();
        overflow.reserve(scored.size());
        for (auto& p : scored) {
            overflow.push_back(std::move(p.second));
        }
        return overflow;
    }

    MoveSlots& slot = defense_slots_[depth];
    slot.scored.clear();
    collect_defenses(board, color, attack_row, attack_col, slot.scored);
    slot.moves.clear();
    slot.moves.reserve(slot.scored.size());
    for (auto& p : slot.scored) {
        slot.moves.push_back(std::move(p.second));
    }
    return slot.moves;
}

int VCTSearch::vct_defend(Board& board, int color, int depth, int alpha, int beta,
                           std::atomic<bool>& stop_flag, std::vector<Move>& out_line) {
    // #88 的约定：out_line 只在「这一层交回 100000+」时被填，其余任何出口都是空的。
    out_line.clear();
    nodes_searched_++;
    if (nodes_searched_ > node_limit_ || aborted(stop_flag)) return 0;
    if (depth <= 0) return 0;

    // 槽号 = 这里的 depth：递归严格减 depth，父层握着 attacks[d] 时子层只碰 d-1。
    std::vector<Move> overflow;
    const std::vector<Move>& attacks = generate_attacks(board, color, depth, overflow);
    if (attacks.empty()) return 0;

    // vct_defend is entered after the defender's reply: it is the attacker's
    // turn again, so the outer loop must maximise over attacker candidates
    // (the previous std::min made this node pretend the attacker was picking
    // their worst move).
    int best_score = -INT_MAX;
    LineSet ln_overflow;
    LineSet& ln = lines(depth, ln_overflow);
    LineGuard guard(this, &ln);
    std::vector<Move>& best_line = ln.best;
    best_line.clear();
    bool evaluated_any = false;

    for (const auto& move : attacks) {
        if (aborted(stop_flag)) break;
        evaluated_any = true;

        board.place_stone(move.pos.row, move.pos.col, color);

        if (board.has_five_in_a_row(move.pos.row, move.pos.col, color)) {
            board.undo_last_move();
            out_line.push_back(move);
            return 100000 + depth;
        }

        // #84：见 search() 里那条同形的判断 —— 这一层也只看「挡我」的点，不看对面此刻能不能
        // 直接把五做成。丢掉这条候选，不是丢掉整个节点（别的候选可能先把那个点填上）。
        if (defender_can_make_five(board, color)) {
            ++defend_rejections_;
            board.undo_last_move();
            continue;
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
            out_line.push_back(move);
            return 100000 + depth;
        }

        if (pattern.four_count >= 2) {
            board.undo_last_move();
            out_line.push_back(move);
            return 100000 + depth;
        }

        const std::vector<Move>& defenses =
            generate_defenses(board, color, move.pos.row, move.pos.col, depth, overflow);
        if (defenses.empty()) {
            board.undo_last_move();
            out_line.push_back(move);
            return 100000 + depth;
        }

        int worst_for_attacker = INT_MAX;
        std::vector<Move>& worst_line = ln.worst;
        worst_line.clear();
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
                worst_line.clear();
                break;
            }

            std::vector<Move>& child_line = ln.child;
            int score = vct_attack(board, color, depth - 1, alpha, beta, stop_flag, child_line);
            board.undo_last_move();
            // 变例跟着**对防守方最好的那一手**走：这一层交回的分数就是它，线也必须是它的线，
            // 否则交出去的那条线核的是另一条分支。
            if (score < worst_for_attacker) {
                worst_for_attacker = score;
                worst_line.clear();
                worst_line.push_back(def);
                worst_line.insert(worst_line.end(), child_line.begin(), child_line.end());
            }
            if (worst_for_attacker <= -100000) break;
        }

        board.undo_last_move();

        if (worst_for_attacker > best_score) {
            best_score = worst_for_attacker;
            best_line.clear();
            best_line.push_back(move);
            best_line.insert(best_line.end(), worst_line.begin(), worst_line.end());
        }
        alpha = std::max(alpha, best_score);
        if (alpha >= beta) break;
    }

    if (!evaluated_any) return 0;
    if (best_score >= 100000) out_line = best_line;
    return best_score;
}

int VCTSearch::vct_attack(Board& board, int color, int depth, int alpha, int beta,
                           std::atomic<bool>& stop_flag, std::vector<Move>& out_line) {
    out_line.clear();
    nodes_searched_++;
    if (nodes_searched_ > node_limit_ || aborted(stop_flag)) return 0;
    if (depth <= 0) return 0;

    // 槽号 = 这里的 depth：递归严格减 depth，父层握着 attacks[d] 时子层只碰 d-1。
    std::vector<Move> overflow;
    const std::vector<Move>& attacks = generate_attacks(board, color, depth, overflow);
    if (attacks.empty()) return 0;

    int best_score = -INT_MAX;
    LineSet ln_overflow;
    LineSet& ln = lines(depth, ln_overflow);
    LineGuard guard(this, &ln);
    std::vector<Move>& best_line = ln.best;
    best_line.clear();
    bool evaluated_any = false;

    for (const auto& move : attacks) {
        if (aborted(stop_flag)) break;
        evaluated_any = true;

        board.place_stone(move.pos.row, move.pos.col, color);

        if (board.has_five_in_a_row(move.pos.row, move.pos.col, color)) {
            board.undo_last_move();
            out_line.push_back(move);
            return 100000 + depth;
        }

        // #84：同 search()/vct_defend 那条 —— 对面此刻就能把五做成，这条候选不是胜着。
        if (defender_can_make_five(board, color)) {
            ++attack_rejections_;
            board.undo_last_move();
            continue;
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
            out_line.push_back(move);
            return 100000 + depth;
        }

        if (pattern.four_count >= 2) {
            board.undo_last_move();
            out_line.push_back(move);
            return 100000 + depth;
        }

        const std::vector<Move>& defenses =
            generate_defenses(board, color, move.pos.row, move.pos.col, depth, overflow);
        if (defenses.empty()) {
            board.undo_last_move();
            out_line.push_back(move);
            return 100000 + depth;
        }

        int worst_for_attacker = INT_MAX;
        std::vector<Move>& worst_line = ln.worst;
        worst_line.clear();
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
                worst_line.clear();
                break;
            }

            std::vector<Move>& child_line = ln.child;
            int score = vct_defend(board, color, depth - 1, alpha, beta, stop_flag, child_line);
            board.undo_last_move();
            if (score < worst_for_attacker) {
                worst_for_attacker = score;
                worst_line.clear();
                worst_line.push_back(def);
                worst_line.insert(worst_line.end(), child_line.begin(), child_line.end());
            }
            if (worst_for_attacker <= -100000) break;
        }

        board.undo_last_move();

        if (worst_for_attacker > best_score) {
            best_score = worst_for_attacker;
            best_line.clear();
            best_line.push_back(move);
            best_line.insert(best_line.end(), worst_line.begin(), worst_line.end());
        }
        alpha = std::max(alpha, best_score);
        if (alpha >= beta) break;
    }

    if (!evaluated_any) return 0;
    if (best_score >= 100000) out_line = best_line;
    return best_score;
}

VCTMove VCTSearch::search(const Board& board, int color, int max_depth,
                           std::atomic<bool>& stop_flag) {
    VCTMove result;
    nodes_searched_ = 0;
    root_rejections_ = 0;
    defend_rejections_ = 0;
    attack_rejections_ = 0;
    line_peak_ = 0;
    line_collisions_ = 0;
    held_n_ = 0;

    Board mutable_board = board;

    // 根扫描这一层的槽号就是 max_depth；下面的 vct_defend 从 max_depth-1 起，不会撞到根。
    std::vector<Move> overflow;
    const std::vector<Move>& attacks = generate_attacks(mutable_board, color, max_depth, overflow);
    if (attacks.empty()) {
        result.result = VCTResult::UNKNOWN;
        return result;
    }

    int best_score = -INT_MAX;
    Move best_move;
    LineSet ln_overflow;
    LineSet& ln = lines(max_depth, ln_overflow);
    LineGuard guard(this, &ln);
    std::vector<Move>& best_line = ln.best;
    best_line.clear();

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

        // #84：这一手没把对面的成五点填掉，就不是胜着 —— 它不必来挡，先落那一子棋就结束了。
        // 下面三条威胁分支和那条防守循环都只算「挡我这条威胁」的点，看不出这一层。
        if (defender_can_make_five(mutable_board, color)) {
            ++root_rejections_;
            mutable_board.undo_last_move();
            continue;
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

        const std::vector<Move>& defenses =
            generate_defenses(mutable_board, color, move.pos.row, move.pos.col, max_depth,
                              overflow);
        if (defenses.empty()) {
            mutable_board.undo_last_move();
            result.result = VCTResult::WIN;
            result.best_move = move;
            result.variation.push_back(move);
            return result;
        }

        int worst = INT_MAX;
        std::vector<Move>& worst_line = ln.worst;
        worst_line.clear();
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
                worst_line.clear();
                break;
            }

            std::vector<Move>& child_line = ln.child;
            int score = vct_defend(mutable_board, color, max_depth - 1,
                                   -INT_MAX, INT_MAX, stop_flag, child_line);
            mutable_board.undo_last_move();
            // 根上的分数是「对防守方最好的那一手」给的，交出去的变例也必须是那一手的。
            if (score < worst) {
                worst = score;
                worst_line.clear();
                worst_line.push_back(def);
                worst_line.insert(worst_line.end(), child_line.begin(), child_line.end());
            }
        }

        mutable_board.undo_last_move();

        if (worst > best_score) {
            best_score = worst;
            best_move = move;
            best_line.clear();
            best_line.push_back(move);
            best_line.insert(best_line.end(), worst_line.begin(), worst_line.end());
        }
    }

    if (best_score >= 100000) {
        result.result = VCTResult::WIN;
        result.best_move = best_move;
        result.variation = best_line;
    } else {
        result.result = VCTResult::UNKNOWN;
    }

    return result;
}

namespace {
// #84 的门用的是「对面此刻有几个成五点」这个硬事实。扫描走 active_empty() —— 任何成五点
// 都挨着自己的子，而 active_empty() 是「离任何子 Chebyshev<=2」的空格集合，是它的超集。
// place/undo 会改 active_empty_ 本身，所以先把格子抄进这块小缓冲，不能边遍历边落子。
constexpr int kMaxActiveCells = BOARD_SIZE * BOARD_SIZE;
}  // namespace

bool VCTSearch::defender_can_make_five(Board& board, int attacker) const {
    const int defender = attacker == BLACK ? WHITE : BLACK;
    Position cells[kMaxActiveCells];
    int count = 0;
    for (const Position& pos : board.active_empty()) {
        if (count == kMaxActiveCells) break;
        cells[count++] = pos;
    }
    for (int i = 0; i < count; ++i) {
        if (!board.place_stone(cells[i].row, cells[i].col, defender)) continue;
        const bool wins = board.has_five_in_a_row(cells[i].row, cells[i].col, defender);
        board.undo_last_move();
        if (wins) return true;
    }
    return false;
}

}

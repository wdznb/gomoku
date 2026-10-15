#include "alphabeta.h"
#include "../core/transforms.h"
#include <algorithm>
#include <climits>

namespace gomoku {

AlphaBetaSearch::AlphaBetaSearch(const Evaluator& evaluator, TranspositionTable& tt)
    : evaluator_(evaluator), tt_(tt), node_limit_(2000000),
      clock_(&system_search_clock()) {}

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
    // would rerun is_forbidden (which does a second recognize_position) on
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

NodeResult AlphaBetaSearch::quiescence(Board& board, int alpha, int beta, int color,
                                       std::atomic<bool>& stop_flag, int qdepth) {
    // Quiescence can recurse for tens of moves on a still-live position. Without
    // a node increment here, the node budget only bounds the alphabeta tree and a
    // timeout or abort flag set at a shallow depth can still be trapped inside
    // an unbounded quiescence subtree.
    ctx_.count_quiescence_node();
    ctx_.note_quiescence_depth(qdepth);
    if (ctx_.hard_stopped()) {
        // A static evaluation of one position: the number the parent shows the
        // player, and nothing the parent may build a bound out of.
        return {evaluator_.evaluate_board(board, color), false};
    }

    int stand_pat = evaluator_.evaluate_board(board, color);

    // Fail-soft on the stand-pat cutoff: returning `beta` (the window edge)
    // would let the parent store that bound as TTFlag::EXACT. Returning
    // stand_pat is the node's true lower value and stays sound when the
    // parent records it after any alpha/beta adjustment.
    if (stand_pat >= beta) return {stand_pat, true};
    if (stand_pat > alpha) alpha = stand_pat;

    const auto& candidates = board.active_empty();
    std::vector<std::pair<int, Move>> scored;
    scored.reserve(candidates.size());

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
            if (color == BLACK && PatternRecognizer::is_forbidden_from_pattern(pattern)) {
                continue;
            }
            // Score from the pattern we just recognized; calling
            // evaluate_position here would re-run the same four scans.
            int s = evaluator_.evaluate_position_with_pattern(board, pos.row, pos.col, pattern,
                                                              color);
            scored.emplace_back(s, Move(pos.row, pos.col, color));
        }
    }

    if (scored.empty()) return {stand_pat, true};

    // Decorate-sort-undecorate: scores are already attached, so the sort does
    // no pattern recognition.
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    bool node_sound = true;
    for (const auto& entry : scored) {
        const Move& move = entry.second;
        if (ctx_.hard_stopped()) {
            node_sound = false;
            break;
        }

        board.place_stone(move.pos.row, move.pos.col, color);

        if (board.has_five_in_a_row(move.pos.row, move.pos.col, color)) {
            board.undo_last_move();
            // A completed five is a fact about the position, not an estimate, so
            // it stays sound even when the budget is what ended the search.
            return {100000, true};
        }

        NodeResult child = quiescence(board, -beta, -alpha, opponent(color), stop_flag,
                                      qdepth + 1);
        board.undo_last_move();
        if (!child.sound) node_sound = false;

        int score = -child.score;
        if (score > alpha) {
            alpha = score;
        }
        // Fail-soft on the beta cutoff. Returning `alpha` (which now holds
        // the best score seen) rather than `beta` avoids poisoning the TT
        // with a value that is only a bound.
        if (alpha >= beta) return {alpha, node_sound};
    }

    return {alpha, node_sound};
}

NodeResult AlphaBetaSearch::alphabeta(Board& board, int depth, int alpha, int beta, int color,
                                      std::atomic<bool>& stop_flag, int ply) {
    ctx_.count_node();
    if (ctx_.hard_stopped()) {
        // Only the last-placed stone can have produced a five; check_winner()
        // would scan all 225 cells and then is_game_over() would scan them
        // again. This abort path fires very frequently during iterative
        // deepening, so the last-move check is important.
        const Move& lm = board.last_move();
        if (lm.pos.is_valid() &&
            board.has_five_in_a_row(lm.pos.row, lm.pos.col, lm.color)) {
            if (lm.color == color) return {100000 + depth, true};
            return {-100000 - depth, true};
        }
        return {evaluator_.evaluate_board(board, color), false};
    }

    uint64_t hash = board.zobrist_hash();
    // Copied out, not pointed into the table: the store below can land in this very
    // bucket and evict the record this node is still reading.
    TTEntry entry;
    Move tt_move;
    bool node_sound = true;
    if (tt_.probe(hash, entry, ply)) {
        ctx_.count_tt_hit();
        if (entry.depth >= depth) {
            if (entry.flag == TTFlag::EXACT) {
                ctx_.count_tt_cutoff();
                return {entry.value, true};
            }
            if (entry.flag == TTFlag::LOWERBOUND && entry.value >= beta) {
                ctx_.count_tt_cutoff();
                return {entry.value, true};
            }
            if (entry.flag == TTFlag::UPPERBOUND && entry.value <= alpha) {
                ctx_.count_tt_cutoff();
                return {entry.value, true};
            }
        }
        tt_move = entry.best_move;
    }

    // Terminal check: only the last move can have produced a five.
    const Move& lm = board.last_move();
    if (lm.pos.is_valid() &&
        board.has_five_in_a_row(lm.pos.row, lm.pos.col, lm.color)) {
        if (lm.color == color) return {100000 + depth, true};
        return {-100000 - depth, true};
    }
    if (board.move_count() >= MAX_MOVES) return {0, true};

    if (depth <= 0) {
        return quiescence(board, alpha, beta, color, stop_flag, 0);
    }

    auto moves = generate_moves(board, color);
    if (moves.empty()) return {evaluator_.evaluate_board(board, color), true};

    sort_moves(moves, board, color, tt_move);

    Move best_move;
    int best_score = -INT_MAX;
    TTFlag flag = TTFlag::UPPERBOUND;

    for (const auto& move : moves) {
        if (ctx_.hard_stopped()) {
            // The scores that made up best_score came out of short-circuit static
            // evaluations, so not even a LOWERBOUND is sound for this node. It is
            // this node's own record, not the search's: a sibling subtree that
            // finished is sound and has already written its own entries.
            node_sound = false;
            break;
        }

        board.place_stone(move.pos.row, move.pos.col, color);

        NodeResult child = alphabeta(board, depth - 1, -beta, -alpha, opponent(color),
                                     stop_flag, ply + 1);

        board.undo_last_move();
        if (!child.sound) node_sound = false;

        int score = -child.score;
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

    // A sound node's value is a bound on its subtree even when the search as a
    // whole was cut short, so an aborted search keeps its finished work in the
    // table. What it does not keep is this node's own short-circuited number.
    //
    // Mate scores are stored too (design doc 2.4.2): they used to be excluded, so a
    // proven line was recomputed from scratch on every move that walked back into
    // it. The window is wide enough to hold +-100000 plus the ply correction and
    // narrow enough to keep the -INT_MAX nothing-was-searched value out.
    if (node_sound && best_score > -1000000 && best_score < 1000000) {
        tt_.store(hash, depth, best_score, best_move, flag, ply);
    }

    return {best_score, node_sound};
}

SearchInfo AlphaBetaSearch::to_info(const SearchStats& stats) {
    SearchInfo info;
    info.depth = stats.completed_depth;
    info.nodes_searched = static_cast<int>(stats.nodes);
    info.score = stats.score;
    info.best_move = stats.best_move;
    info.principal_variation = stats.pv;
    info.elapsed_ms = stats.elapsed_ms;
    info.partial_depth = stats.partial_depth;
    info.root_moves_done = stats.root_moves_done;
    info.root_moves_total = stats.root_moves_total;
    info.seldepth = stats.seldepth;
    info.partial_best_move = stats.partial_best_move;
    info.stopped = stats.stopped;
    return info;
}

void AlphaBetaSearch::refresh_counters() {
    stats_.nodes = ctx_.nodes();
    stats_.qnodes = ctx_.quiescence_nodes();
    stats_.tt_hits = ctx_.tt_hits();
    stats_.tt_cutoffs = ctx_.tt_cutoffs();
    stats_.seldepth = ctx_.seldepth();
    stats_.elapsed_ms = static_cast<int>(ctx_.elapsed_ms());
}

void AlphaBetaSearch::emit_progress() {
    if (!progress_callback_) return;
    refresh_counters();
    progress_callback_(stats_);
}

Move AlphaBetaSearch::search(const Board& board, int color, int max_depth,
                             int time_limit_ms, std::atomic<bool>& stop_flag) {
    // The legacy shape: this phase's share of the budget as a hard limit and the
    // node ceiling from set_node_limit(). An absolute deadline set through
    // set_deadline() overrides the share below, exactly as it did when is_timeout()
    // knew only about the share.
    SearchLimits limits;
    limits.max_depth = max_depth;
    limits.hard_time_ms = time_limit_ms;
    limits.max_nodes = node_limit_;
    return search(board, color, limits, stop_flag);
}

Move AlphaBetaSearch::search(const Board& board, int color, const SearchLimits& limits,
                             std::atomic<bool>& stop_flag) {
    // Reset the reported accounting each search. An aborted or early-return
    // search must not hand the caller the previous search's depth/score.
    stats_ = SearchStats{};
    info_ = SearchInfo{};

    ctx_.start(limits, *clock_, stop_flag);
    if (has_deadline_) ctx_.set_absolute_deadline(deadline_ms_);

    // No generation step here, on purpose. Ageing the table at the start of a think
    // is what let that think's own shallow records evict the deep ones the previous
    // move had paid for; the owners that commit a stone to a board advance the clock
    // (GameManager, SelfPlay).
    Move best_move;
    int best_score = 0;

    auto root_moves = generate_moves(board, color);
    if (root_moves.empty()) {
        // An invalid move says "nothing to play". The centre square may already
        // be occupied, or be a forbidden point for BLACK.
        return Move();
    }

    if (root_moves.size() == 1) {
        return root_moves[0];
    }

    stats_.root_moves_total = static_cast<int>(root_moves.size());

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
        stats_.root_moves_total = static_cast<int>(root_moves.size());
    }

    sort_moves(root_moves, board, color);

    for (int depth = 1; depth <= limits.max_depth; ++depth) {
        if (ctx_.layer_boundary_stopped()) {
            // The budget was already gone before this layer started: the layer
            // below is the last complete record, and there is nothing partial to
            // report. root_moves_done keeps the previous layer's full count, which
            // is what says "no layer was interrupted" rather than "layer 1 broke
            // after its first candidate".
            stats_.stopped = ctx_.reason();
            refresh_counters();
            break;
        }
        // The soft window is a decision about starting a layer, never a reason to
        // discard a finished one (design doc 2.3.2).
        if (stats_.completed_depth >= 1 && ctx_.soft_time_up()) {
            stats_.stopped = StopReason::TIME_SOFT;
            break;
        }

        // Re-copied per iteration on purpose. undo_last_move() restores the
        // candidate list by swap-remove, so a board that has already played and
        // undone this iteration's root moves holds active_empty() in a different
        // order than a fresh copy, and the score ties in sort_moves break
        // against that order. Reusing one board across depths would quietly
        // change which equal-valued moves get searched first.
        Board mutable_board = board;
        Move layer_move;
        NodeResult layer = search_root_layer(mutable_board, root_moves, depth, color,
                                            layer_move, stop_flag);
        const bool layer_complete =
            stats_.root_moves_done == static_cast<int>(root_moves.size());

        if (layer_complete && layer.sound) {
            best_move = layer_move;
            best_score = layer.score;
            stats_.completed_depth = depth;
            stats_.score = best_score;
            stats_.best_move = best_move;
            // A layer that finished replaces any earlier cut-short record.
            stats_.partial_depth = 0;
            stats_.partial_best_move = Move();
            emit_progress();

            // Feed the shallow iteration's winner back into the root order so
            // deeper iterations inherit the cutoff / TT-move effect that makes
            // iterative deepening worth the extra work.
            if (layer_move.pos.is_valid()) {
                auto it = std::find_if(root_moves.begin(), root_moves.end(),
                    [&](const Move& m) {
                        return m.pos.row == layer_move.pos.row &&
                               m.pos.col == layer_move.pos.col;
                    });
                if (it != root_moves.begin() && it != root_moves.end()) {
                    std::iter_swap(root_moves.begin(), it);
                }
            }

            if (best_score >= 100000 || best_score <= -100000) {
                // A proven result is a deliberate end, not a budget running out.
                stats_.stopped = StopReason::MATE;
                break;
            }
        } else {
            // The interrupted layer is recorded and reported, and not submitted:
            // its scores came partly out of short-circuited subtrees. The caller
            // still plays the last completed layer's move, so a cut think cannot
            // silently become a weaker one.
            stats_.partial_depth = depth;
            stats_.partial_best_move = layer_move;
            stats_.stopped = ctx_.reason();
            emit_progress();
            break;
        }
    }

    // A think that completed no layer must say why. Reaching this with no reason
    // means the layer loop never ran (a request for zero depth) rather than that
    // time or nodes ran out.
    if (stats_.completed_depth == 0 && stats_.stopped == StopReason::NONE &&
        limits.max_depth >= 1) {
        stats_.stopped = ctx_.reason();
        if (stats_.stopped == StopReason::NONE) stats_.stopped = StopReason::USER;
    }

    if (!best_move.pos.is_valid() && !root_moves.empty()) {
        best_move = root_moves[0];
    }

    refresh_counters();
    info_ = to_info(stats_);
    return best_move;
}

NodeResult AlphaBetaSearch::search_root_layer(Board& board,
                                              const std::vector<Move>& root_moves, int depth,
                                              int color, Move& best_move,
                                              std::atomic<bool>& stop_flag) {
    int alpha = -INT_MAX;
    const int beta = INT_MAX;
    int layer_best_score = -INT_MAX;
    Move layer_best_move;
    bool layer_sound = true;
    stats_.root_moves_done = 0;

    for (const auto& move : root_moves) {
        if (ctx_.hard_stopped()) break;

        board.place_stone(move.pos.row, move.pos.col, color);

        // A root child sits one ply from the root, whatever the layer's depth is.
        NodeResult child = alphabeta(board, depth - 1, -beta, -alpha, opponent(color),
                                     stop_flag, 1);

        board.undo_last_move();

        if (!child.sound) layer_sound = false;
        const int score = -child.score;
        if (score > layer_best_score) {
            layer_best_score = score;
            layer_best_move = move;
        }
        if (score > alpha) alpha = score;
        // What the interrupted layer would have played, kept as it is built: the
        // player sees it, the returned move never comes from it.
        stats_.partial_best_move = layer_best_move;
        // Only a candidate whose own subtree ran to the end counts as searched. A
        // short circuit sets the stop reason for the whole rest of the think, so the
        // cut candidate is always the last one attempted and the count is exactly
        // "how far into the layer the interrupt landed" - which is what the client
        // shows as 第 N 层（12/34）.
        if (child.sound) ++stats_.root_moves_done;
    }

    best_move = layer_best_move;
    return {layer_best_score, layer_sound};
}

}

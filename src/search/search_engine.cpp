#include "search_engine.h"
#include <chrono>
#include <climits>

namespace gomoku {

namespace {

// AlphaBeta keeps the tail of the move budget: while VCF/VCT shared the whole
// deadline, an expensive tactical phase left AlphaBeta only the 100 ms floor
// and a depth-0 fallback (README:343 wants deeper searches to get more time).
constexpr int kVcfBudgetPercent = 35;
constexpr int kVctBudgetPercent = 55;

}

SearchEngine::SearchEngine()
    : book_enabled_(true),
      vcf_enabled_(true), vct_enabled_(true), alphabeta_enabled_(true),
      vcf_max_depth_(20), vct_max_depth_(12), alphabeta_max_depth_(8),
      resign_detection_(true), resign_node_limit_(400000) {}

SearchEngine::SearchEngine(const ThreatCoefficients& coeffs)
    : evaluator_(coeffs), greedy_(coeffs),
      book_enabled_(true),
      vcf_enabled_(true), vct_enabled_(true), alphabeta_enabled_(true),
      vcf_max_depth_(20), vct_max_depth_(12), alphabeta_max_depth_(8),
      resign_detection_(true), resign_node_limit_(400000) {}

void SearchEngine::set_coefficients(const ThreatCoefficients& coeffs) {
    evaluator_.set_coefficients(coeffs);
    greedy_.set_coefficients(coeffs);
}

SearchResult SearchEngine::search(const Board& board, int color,
                                   int max_depth, int time_limit_ms,
                                   std::atomic<bool>& stop_flag) {
    auto start = std::chrono::high_resolution_clock::now();
    SearchResult result;
    // The one wall clock the caller is owed: each phase times itself, but the
    // replay panel and ai_move.thinking_time_ms report a single number per move.
    auto elapsed_since_start = [start]() {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start).count());
    };

    auto root_moves = board.active_empty();
    if (root_moves.empty()) {
        // Only a full board gets here: rebuild_active_empty() seeds all 225 cells
        // when move_count_ is 0 (board.cpp:281-293). The centre may well hold a
        // stone, so hand back an invalid move and the caller commits nothing.
        result.method = "no_legal_move";
        result.elapsed_ms = elapsed_since_start();
        return result;
    }

    // Filter the candidate list once up front. The single-move fast path and
    // the must_block scan below both consume root_moves; without this a BLACK
    // player on a nearly-full board could be handed the last empty cell as
    // "forced" even when it is a double-three / double-four / overline.
    if (color == BLACK) {
        std::vector<Position> legal;
        legal.reserve(root_moves.size());
        for (const auto& pos : root_moves) {
            if (PatternRecognizer::is_forbidden(board, pos.row, pos.col, BLACK)) continue;
            legal.push_back(pos);
        }
        root_moves.swap(legal);
        if (root_moves.empty()) {
            // A forbidden shape needs a friendly stone within two cells, so an
            // emptied active list says nothing about the rest of the board: legal
            // points may exist, just outside the window active_empty() covers.
            std::vector<Position> far_legal;
            for (int r = 0; r < BOARD_SIZE; ++r) {
                for (int c = 0; c < BOARD_SIZE; ++c) {
                    if (board.is_empty(r, c) &&
                        !PatternRecognizer::is_forbidden(board, r, c, BLACK)) {
                        far_legal.push_back(Position(r, c));
                    }
                }
            }
            if (!far_legal.empty()) {
                root_moves.swap(far_legal);
            } else if (resign_detection_) {
                result.resigned = true;
                result.method = "resign";
                result.elapsed_ms = elapsed_since_start();
                return result;
            } else {
                // Nothing to play and no resignation asked for: an invalid move is
                // how the caller is told to commit nothing this turn.
                result.method = "no_legal_move";
                result.elapsed_ms = elapsed_since_start();
                return result;
            }
        }
    }

    if (root_moves.size() == 1) {
        result.best_move = Move(root_moves[0], color);
        result.method = "forced";
        result.elapsed_ms = elapsed_since_start();
        return result;
    }

    // Own-win must be checked across every legal root candidate, not just the
    // top greedy move. Greedy ranks by attack + defense, so a purely
    // defensive square (blocking an opponent live-four, say) can score higher
    // than an outright winning five. Without this scan the must_block branch
    // below fires first and the AI throws away a guaranteed win.
    for (const auto& pos : root_moves) {
        if (!board.is_empty(pos.row, pos.col)) continue;
        PositionPattern pattern = PatternRecognizer::recognize_position(
            board, pos.row, pos.col, color);
        for (int d = 0; d < 4; ++d) {
            if (pattern.directions[d].type == PatternType::FIVE) {
                result.best_move = Move(pos, color);
                result.score = 100000;
                result.method = "greedy_five";
                result.elapsed_ms = elapsed_since_start();
                return result;
            }
        }
    }

    int opp = opponent(color);
    for (const auto& pos : root_moves) {
        if (!board.is_empty(pos.row, pos.col)) continue;
        if (color == BLACK && PatternRecognizer::is_forbidden(board, pos.row, pos.col, color)) continue;
        PositionPattern pattern = PatternRecognizer::recognize_position(
            board, pos.row, pos.col, opp);
        for (int d = 0; d < 4; ++d) {
            if (pattern.directions[d].type == PatternType::FIVE) {
                // A second completion point means no single block survives the
                // turn. The scan runs over board.active_empty(), not root_moves:
                // for BLACK the root list is forbidden-filtered, which hides the
                // very second point being looked for.
                bool second_five_point = false;
                for (const auto& other : board.active_empty()) {
                    if (other.row == pos.row && other.col == pos.col) continue;
                    if (!board.is_empty(other.row, other.col)) continue;
                    PositionPattern theirs = PatternRecognizer::recognize_position(
                        board, other.row, other.col, opp);
                    for (int e = 0; e < 4; ++e) {
                        if (theirs.directions[e].type == PatternType::FIVE) {
                            second_five_point = true;
                            break;
                        }
                    }
                    if (second_five_point) break;
                }
                // Two points and resignation switched off still gets the single
                // block: 不打开认输就不要静默认输.
                if (second_five_point && resign_detection_) {
                    result.resigned = true;
                    result.method = "resign";
                    result.elapsed_ms = elapsed_since_start();
                    return result;
                }
                result.best_move = Move(pos, color);
                result.score = 99000;
                result.method = "must_block";
                result.elapsed_ms = elapsed_since_start();
                return result;
            }
        }
    }

    if (book_enabled_ && book_ != nullptr && !book_->empty() &&
        board.move_count() <= book_->max_depth() && !stop_flag.load()) {
        Move book_move;
        if (book_->query(board, color, book_move)) {
            result.best_move = book_move;
            result.score = 0;
            result.depth = 0;
            result.method = "book";
            result.elapsed_ms = elapsed_since_start();
            return result;
        }
    }

    // One absolute wall-clock budget for AlphaBeta and the resign probe; the
    // tactical phases get slices of it below, measured from the same start.
    // VCF/VCT otherwise bound work only by node_limit_ (1M / 500K), so without
    // any deadline they overrun time_limit_ms by seconds before AlphaBeta starts.
    const bool has_deadline = (time_limit_ms > 0);
    const auto search_start = std::chrono::steady_clock::now();
    auto search_deadline = search_start
                           + std::chrono::milliseconds(has_deadline ? time_limit_ms : 0);

    if (vcf_enabled_ && !stop_flag.load()) {
        VCFSearch vcf(evaluator_, tt_alphabeta_);
        if (has_deadline) {
            vcf.set_deadline(search_start + std::chrono::milliseconds(
                static_cast<long long>(time_limit_ms) * kVcfBudgetPercent / 100));
        }
        VCFMove vcf_result = vcf.search(board, color, vcf_max_depth_, stop_flag);
        if (vcf_result.result == VCFResult::WIN) {
            result.best_move = vcf_result.best_move;
            result.score = 100000;
            result.method = "vcf";
            result.principal_variation = vcf_result.variation;
            result.elapsed_ms = elapsed_since_start();
            return result;
        }

        // Note: opponent VCF defense is handled by alphabeta search below.
        // The opponent's VCF best_move is the attacker's own stone position,
        // which cannot be used as our defense move. The alphabeta search
        // will naturally find the best defensive move.
    }

    if (vct_enabled_ && !stop_flag.load()) {
        VCTSearch vct(evaluator_, tt_alphabeta_);
        if (has_deadline) {
            vct.set_deadline(search_start + std::chrono::milliseconds(
                static_cast<long long>(time_limit_ms) * kVctBudgetPercent / 100));
        }
        VCTMove vct_result = vct.search(board, color, vct_max_depth_, stop_flag);
        if (vct_result.result == VCTResult::WIN) {
            result.best_move = vct_result.best_move;
            result.score = 90000;
            result.method = "vct";
            result.principal_variation = vct_result.variation;
            result.elapsed_ms = elapsed_since_start();
            return result;
        }
    }

    if (alphabeta_enabled_ && !stop_flag.load()) {
        AlphaBetaSearch ab(evaluator_, tt_alphabeta_);
        // The absolute end-of-move deadline, not a slice: remaining_time below
        // is only this phase's share of the budget, so on its own it lets the
        // move overshoot by that whole share once the tactics phases spent it.
        if (has_deadline) ab.set_deadline(search_deadline);
        int remaining_time = time_limit_ms;
        auto elapsed = std::chrono::high_resolution_clock::now() - start;
        int used_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
        remaining_time -= used_ms;
        if (remaining_time < 100) remaining_time = 100;
        // Iteration-level frames for the client: the frame at the end of this
        // function only lands once the whole search is over, which is too late
        // for its 终止 button. The member is copied out first so the hook carries
        // its own value. AlphaBeta times only its own phase, so the tactics phases
        // it came after are added on: one clock for the per-depth frames and the
        // terminal frame below, and a constant offset keeps the sequence non-decreasing.
        const ProgressCallback progress_cb = progress_callback_;
        if (progress_cb) {
            ab.set_progress_callback([progress_cb, used_ms](int depth, int nodes, int elapsed_ms) {
                progress_cb(depth, nodes, elapsed_ms + used_ms);
            });
        }

        // Tie the node budget to the wall-clock deadline so that a large
        // timeout actually buys more search. Empirical throughput on this
        // target is roughly 100k–250k nodes/s, so 200 nodes/ms is a safe
        // middle ground: a 30 s move gets up to 6 M nodes, while a 4 s move
        // still has 800 k — far above the old hard-coded 2 M cap. The product
        // is computed in 64-bit and clamped: a huge trainer --timeout-ms made
        // remaining_time * 200 overflow int negative, tripping node_limit_ on
        // the very first node and silently falling back to root_moves[0].
        ab.set_node_limit(static_cast<int>(std::min<long long>(
            static_cast<long long>(remaining_time) * 200LL, INT_MAX)));

        int depth = (max_depth > 0) ? max_depth : alphabeta_max_depth_;
        Move ab_move = ab.search(board, color, depth, remaining_time, stop_flag);

        // A search that found nothing to search returns an invalid move. The
        // fallback takes the first legal candidate root_moves still holds rather
        // than greedy_.search, which reads the same active_empty() window and
        // hands back the centre point when that window has no legal cell.
        if (!ab_move.pos.is_valid() || !board.is_empty(ab_move.pos.row, ab_move.pos.col)) {
            Move fallback;
            for (const auto& pos : root_moves) {
                if (!board.is_empty(pos.row, pos.col)) continue;
                if (color == BLACK &&
                    PatternRecognizer::is_forbidden(board, pos.row, pos.col, BLACK)) continue;
                fallback = Move(pos, color);
                break;
            }
            result.best_move = fallback;
            result.method = fallback.pos.is_valid() ? "greedy" : "no_legal_move";
            result.elapsed_ms = elapsed_since_start();
            return result;
        }

        SearchInfo info = ab.last_search_info();
        result.best_move = ab_move;
        result.score = info.score;
        result.depth = info.depth;
        result.nodes_searched = info.nodes_searched;
        result.method = "alphabeta";
        // Depth 0 means not one iteration finished and AlphaBeta fell back to
        // root_moves[0]; calling that alphabeta showed depth 0 / score 0 in the
        // replay panel (README:530 lists greedy as a method).
        if (info.depth == 0) {
            result.method = "greedy";
            // A renamed fast path is not AlphaBeta: README:531 leaves depth/nodes
            // unset for every other method, and this hand completed no layer.
            result.depth = 0;
            result.nodes_searched = 0;
        }

        // AlphaBeta's own count starts with that phase and covers only it; the
        // caller is owed the whole move, so the measure is taken below - after the
        // resign probe, which spends part of the same budget.

        // AlphaBeta proved the loss itself (mate threshold, side-to-move
        // perspective): resign without waiting for the budget-gated probe
        // below, which rarely fires in real games because AlphaBeta almost
        // always consumes the whole time limit. Non-mate scores are untouched.
        if (resign_detection_ && !stop_flag.load() && info.score <= -100000) {
            result.resigned = true;
            result.method = "resign";
        }

        if (resign_detection_ && !stop_flag.load() && !result.resigned) {
            auto elapsed_after_ab = std::chrono::high_resolution_clock::now() - start;
            int total_used_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_after_ab).count());
            int probe_budget_ms = time_limit_ms - total_used_ms;
            // Reserve enough time for the callback / network commit; without
            // this the resign probe (VCF depth 20, node cap 400k) can run for
            // many additional seconds whenever AlphaBeta finished early and
            // overshoot the per-move budget the client is waiting on.
            if (probe_budget_ms > 500) {
                int usable_ms = probe_budget_ms - 200;
                // Rough throughput ceiling for VCF nodes on this target. The
                // product is computed in 64-bit: a large client timeout made
                // usable_ms * 400 overflow int into a negative cap, which then
                // tripped node_limit_ on the very first node and silently
                // disabled resignation. resign_node_limit_ is the hard ceiling.
                long long time_scaled_nodes =
                    static_cast<long long>(usable_ms) * 400LL;
                int probe_nodes = static_cast<int>(
                    std::min<long long>(resign_node_limit_, time_scaled_nodes));

                VCFSearch loss_probe(evaluator_, tt_alphabeta_);
                loss_probe.set_node_limit(probe_nodes);
                if (has_deadline) loss_probe.set_deadline(search_deadline);

                Move defense;
                DefenseResult dr = loss_probe.find_defense(board, color, vcf_max_depth_,
                                                            stop_flag, defense);
                if (dr == DefenseResult::PROVEN_LOSS) {
                    result.resigned = true;
                    result.method = "resign";
                }
            }
        }

        const int whole_elapsed_ms = elapsed_since_start();
        result.elapsed_ms = whole_elapsed_ms;
        if (progress_callback_) {
            progress_callback_(info.depth, info.nodes_searched, whole_elapsed_ms);
        }

        return result;
    }

    // The full candidate ranking is only paid for when it is actually consumed:
    // every path above returned, and alphabeta never reaches here without one.
    result.best_move = greedy_.search(board, color);
    result.method = "greedy";
    result.elapsed_ms = elapsed_since_start();
    return result;
}

}

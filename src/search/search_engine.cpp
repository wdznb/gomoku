#include "search_engine.h"
#include <chrono>
#include <climits>

namespace gomoku {

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

    auto root_moves = board.active_empty();
    if (root_moves.empty()) {
        result.best_move = Move(BOARD_SIZE / 2, BOARD_SIZE / 2, color);
        result.method = "default";
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
                return result;
            } else {
                // Nothing to play and no resignation asked for: an invalid move is
                // how the caller is told to commit nothing this turn.
                result.method = "no_legal_move";
                return result;
            }
        }
    }

    if (root_moves.size() == 1) {
        result.best_move = Move(root_moves[0], color);
        result.method = "forced";
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
                result.best_move = Move(pos, color);
                result.score = 99000;
                result.method = "must_block";
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
            return result;
        }
    }

    // One absolute wall-clock budget shared by the VCF, VCT and AlphaBeta
    // phases. VCF/VCT otherwise bound work only by node_limit_ (1M / 500K), so
    // they can overrun time_limit_ms by seconds before AlphaBeta even starts;
    // anchoring them to a common deadline keeps the whole move within budget.
    const bool has_deadline = (time_limit_ms > 0);
    auto search_deadline = std::chrono::steady_clock::now()
                           + std::chrono::milliseconds(has_deadline ? time_limit_ms : 0);

    if (vcf_enabled_ && !stop_flag.load()) {
        VCFSearch vcf(evaluator_, tt_alphabeta_);
        if (has_deadline) vcf.set_deadline(search_deadline);
        VCFMove vcf_result = vcf.search(board, color, vcf_max_depth_, stop_flag);
        if (vcf_result.result == VCFResult::WIN) {
            result.best_move = vcf_result.best_move;
            result.score = 100000;
            result.method = "vcf";
            result.principal_variation = vcf_result.variation;
            return result;
        }

        // Note: opponent VCF defense is handled by alphabeta search below.
        // The opponent's VCF best_move is the attacker's own stone position,
        // which cannot be used as our defense move. The alphabeta search
        // will naturally find the best defensive move.
    }

    if (vct_enabled_ && !stop_flag.load()) {
        VCTSearch vct(evaluator_, tt_alphabeta_);
        if (has_deadline) vct.set_deadline(search_deadline);
        VCTMove vct_result = vct.search(board, color, vct_max_depth_, stop_flag);
        if (vct_result.result == VCTResult::WIN) {
            result.best_move = vct_result.best_move;
            result.score = 90000;
            result.method = "vct";
            result.principal_variation = vct_result.variation;
            return result;
        }
    }

    if (alphabeta_enabled_ && !stop_flag.load()) {
        AlphaBetaSearch ab(evaluator_, tt_alphabeta_);
        // Same absolute deadline the VCF/VCT phases were given: remaining_time
        // below is only this phase's slice, so on its own it lets the move
        // overshoot by that whole slice once the tactics phases spent it.
        if (has_deadline) ab.set_deadline(search_deadline);
        int remaining_time = time_limit_ms;
        auto elapsed = std::chrono::high_resolution_clock::now() - start;
        int used_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
        remaining_time -= used_ms;
        if (remaining_time < 100) remaining_time = 100;

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

        SearchInfo info = ab.last_search_info();
        result.best_move = ab_move;
        result.score = info.score;
        result.depth = info.depth;
        result.nodes_searched = info.nodes_searched;
        result.method = "alphabeta";
        result.elapsed_ms = info.elapsed_ms;

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

        if (progress_callback_) {
            progress_callback_(info.depth, info.nodes_searched, info.elapsed_ms);
        }

        return result;
    }

    // The full candidate ranking is only paid for when it is actually consumed:
    // every path above returned, and alphabeta never reaches here without one.
    result.best_move = greedy_.search(board, color);
    result.method = "greedy";
    return result;
}

}

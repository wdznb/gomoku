#ifndef GOMOKU_ALPHABETA_H
#define GOMOKU_ALPHABETA_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "../core/pattern.h"
#include "../optimization/transposition.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <vector>

namespace gomoku {

struct SearchInfo {
    int depth = 0;
    int nodes_searched = 0;
    int score = 0;
    Move best_move;
    std::vector<Move> principal_variation;
    int elapsed_ms = 0;
};

class AlphaBetaSearch {
public:
    AlphaBetaSearch(const Evaluator& evaluator, TranspositionTable& tt);

    Move search(const Board& board, int color, int max_depth,
                int time_limit_ms, std::atomic<bool>& stop_flag);

    SearchInfo last_search_info() const { return info_; }

    void set_node_limit(int limit) { node_limit_ = limit; }

    // Same shape as SearchEngine's ProgressCallback: one frame per completed
    // iteration, so a long think does not go silent between start and return.
    void set_progress_callback(std::function<void(int depth, int nodes, int elapsed_ms)> cb) {
        progress_callback_ = cb;
    }

    // Absolute end-of-move wall-clock deadline (VCF/VCT get their own slices of
    // the same budget). The time_limit_ms handed to search() is only this
    // phase's share, so without the deadline a search that starts after the
    // budget is spent still gets its whole share.
    void set_deadline(std::chrono::steady_clock::time_point deadline) {
        deadline_ = deadline;
        has_deadline_ = true;
    }

private:
    const Evaluator& evaluator_;
    TranspositionTable& tt_;
    int node_limit_;
    int move_time_limit_ms_;
    int nodes_searched_;
    SearchInfo info_;
    std::function<void(int depth, int nodes, int elapsed_ms)> progress_callback_;

    std::chrono::time_point<std::chrono::high_resolution_clock> start_time_;
    std::chrono::steady_clock::time_point deadline_{};
    bool has_deadline_ = false;

    int alphabeta(Board& board, int depth, int alpha, int beta, int color,
                  std::atomic<bool>& stop_flag);

    int quiescence(Board& board, int alpha, int beta, int color,
                   std::atomic<bool>& stop_flag);

    std::vector<Move> generate_moves(const Board& board, int color);
    void sort_moves(std::vector<Move>& moves, const Board& board, int color,
                    const Move& tt_move = Move());

    bool is_timeout() const;
};

}

#endif

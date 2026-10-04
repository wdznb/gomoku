#ifndef GOMOKU_ALPHABETA_H
#define GOMOKU_ALPHABETA_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "../core/pattern.h"
#include "../optimization/transposition.h"
#include <atomic>
#include <chrono>
#include <vector>

namespace gomoku {

struct SearchInfo {
    int depth;
    int nodes_searched;
    int score;
    Move best_move;
    std::vector<Move> principal_variation;
    int elapsed_ms;
};

class AlphaBetaSearch {
public:
    AlphaBetaSearch(const Evaluator& evaluator, TranspositionTable& tt);

    Move search(const Board& board, int color, int max_depth,
                int time_limit_ms, std::atomic<bool>& stop_flag);

    SearchInfo last_search_info() const { return info_; }

    void set_node_limit(int limit) { node_limit_ = limit; }
    void set_move_time_limit(int limit) { move_time_limit_ms_ = limit; }

private:
    const Evaluator& evaluator_;
    TranspositionTable& tt_;
    int node_limit_;
    int move_time_limit_ms_;
    int nodes_searched_;
    SearchInfo info_;

    std::chrono::time_point<std::chrono::high_resolution_clock> start_time_;

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

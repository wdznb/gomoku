#ifndef GOMOKU_VCT_H
#define GOMOKU_VCT_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "../core/pattern.h"
#include "../optimization/transposition.h"
#include <atomic>
#include <chrono>
#include <vector>

namespace gomoku {

enum class VCTResult {
    WIN,
    LOSS,
    UNKNOWN
};

struct VCTMove {
    VCTResult result;
    Move best_move;
    std::vector<Move> variation;

    VCTMove() : result(VCTResult::UNKNOWN) {}
};

class VCTSearch {
public:
    VCTSearch(const Evaluator& evaluator, TranspositionTable& tt);

    VCTMove search(const Board& board, int color, int max_depth,
                   std::atomic<bool>& stop_flag);

    void set_node_limit(int limit) { node_limit_ = limit; }

    // Nodes actually examined by the last search() - the readout tools/vct_slice_probe.cpp
    // prices VCT with. Observability only; nothing in the engine branches on it.
    int nodes_searched() const { return nodes_searched_; }

    // Optional absolute wall-clock deadline. VCT otherwise bounds work only by
    // node_limit_, so it can overrun the caller's timeout_ms by seconds; the
    // search engine sets this once so VCT/VCF/AlphaBeta share a single budget.
    void set_deadline(std::chrono::steady_clock::time_point deadline) {
        deadline_ = deadline;
        has_deadline_ = true;
    }

private:
    const Evaluator& evaluator_;
    TranspositionTable& tt_;
    int node_limit_;
    int nodes_searched_;
    std::chrono::steady_clock::time_point deadline_{};
    bool has_deadline_ = false;

    bool aborted(std::atomic<bool>& stop_flag) const {
        return stop_flag.load() ||
               (has_deadline_ && std::chrono::steady_clock::now() >= deadline_);
    }

    int vct_attack(Board& board, int color, int depth, int alpha, int beta,
                   std::atomic<bool>& stop_flag);

    int vct_defend(Board& board, int color, int depth, int alpha, int beta,
                   std::atomic<bool>& stop_flag);

    std::vector<Move> generate_attacks(const Board& board, int color);
    std::vector<Move> generate_defenses(const Board& board, int color, int attack_row, int attack_col);
};

}

#endif

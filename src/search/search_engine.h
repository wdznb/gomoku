#ifndef GOMOKU_SEARCH_ENGINE_H
#define GOMOKU_SEARCH_ENGINE_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "../search/greedy.h"
#include "../search/vcf.h"
#include "../search/vct.h"
#include "../search/alphabeta.h"
#include "../optimization/transposition.h"
#include <atomic>
#include <string>
#include <functional>

namespace gomoku {

struct SearchResult {
    Move best_move;
    int score;
    int depth;
    int nodes_searched;
    int elapsed_ms;
    std::string method;
    std::vector<Move> principal_variation;
};

using ProgressCallback = std::function<void(int depth, int nodes, int elapsed_ms)>;

class SearchEngine {
public:
    SearchEngine();
    explicit SearchEngine(const ThreatCoefficients& coeffs);

    SearchResult search(const Board& board, int color,
                        int max_depth, int time_limit_ms,
                        std::atomic<bool>& stop_flag);

    void set_progress_callback(ProgressCallback cb) { progress_callback_ = cb; }

    void set_vcf_enabled(bool v) { vcf_enabled_ = v; }
    void set_vct_enabled(bool v) { vct_enabled_ = v; }
    void set_alphabeta_enabled(bool v) { alphabeta_enabled_ = v; }

    void set_vcf_depth(int d) { vcf_max_depth_ = d; }
    void set_vct_depth(int d) { vct_max_depth_ = d; }
    void set_alphabeta_depth(int d) { alphabeta_max_depth_ = d; }

    void set_coefficients(const ThreatCoefficients& coeffs);

    TranspositionTable& transposition_table() { return tt_; }

private:
    Evaluator evaluator_;
    GreedySearch greedy_;
    TranspositionTable tt_;

    bool vcf_enabled_;
    bool vct_enabled_;
    bool alphabeta_enabled_;

    int vcf_max_depth_;
    int vct_max_depth_;
    int alphabeta_max_depth_;

    ProgressCallback progress_callback_;
};

}

#endif

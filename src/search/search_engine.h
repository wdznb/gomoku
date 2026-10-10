#ifndef GOMOKU_SEARCH_ENGINE_H
#define GOMOKU_SEARCH_ENGINE_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "../search/greedy.h"
#include "../search/vcf.h"
#include "../search/vct.h"
#include "../search/alphabeta.h"
#include "../optimization/transposition.h"
#include "../optimization/opening_book.h"
#include <atomic>
#include <string>
#include <functional>

namespace gomoku {

struct SearchResult {
    Move best_move;
    // Default-initialised: the non-AlphaBeta return paths (default, forced,
    // resign, book, vcf, vct, greedy) leave these untouched, and the server
    // callback reads score/elapsed_ms to build the ai_move message. Unset,
    // that read is of an indeterminate value and the client gets garbage.
    int score = 0;
    int depth = 0;
    int nodes_searched = 0;
    int elapsed_ms = 0;
    std::string method;
    std::vector<Move> principal_variation;
    bool resigned = false;
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

    // The book is owned by the caller and must stay alive and unmodified
    // while any search may be running against it.
    void set_opening_book(const OpeningBook* book) { book_ = book; }
    const OpeningBook* opening_book() const { return book_; }
    void set_book_enabled(bool v) { book_enabled_ = v; }

    // Proves the position lost by exhausting every reply against the opponent's
    // forced VCF. Returns quickly on the first reply that survives, so the cost
    // only grows in positions that are genuinely lost. Only ever sets
    // SearchResult.resigned; failing to prove a loss keeps the AI playing.
    void set_resign_detection(bool v) { resign_detection_ = v; }
    void set_resign_node_limit(int n) { resign_node_limit_ = n; }

    TranspositionTable& transposition_table() { return tt_alphabeta_; }

private:
    Evaluator evaluator_;
    GreedySearch greedy_;
    // VCF and VCT receive this table by reference but never read or write it;
    // keeping a single instance avoids allocating ~64 MB per session that is
    // never touched.
    TranspositionTable tt_alphabeta_;
    const OpeningBook* book_ = nullptr;

    bool book_enabled_;

    bool vcf_enabled_;
    bool vct_enabled_;
    bool alphabeta_enabled_;

    int vcf_max_depth_;
    int vct_max_depth_;
    int alphabeta_max_depth_;

    bool resign_detection_;
    int resign_node_limit_;

    ProgressCallback progress_callback_;
};

}

#endif

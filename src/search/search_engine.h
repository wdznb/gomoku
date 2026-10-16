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
    // Default-initialised so no return path can hand back an indeterminate value:
    // the server reads score/elapsed_ms to build the ai_move message. elapsed_ms is
    // the whole call on every path (entry timestamp to hand-back), while depth and
    // nodes_searched stay 0 outside AlphaBeta, which is the only path that fills them.
    int score = 0;
    int depth = 0;
    int nodes_searched = 0;
    int elapsed_ms = 0;
    std::string method;
    std::vector<Move> principal_variation;
    bool resigned = false;
    // AlphaBeta's layered accounting, so a hand that ran out of budget is
    // distinguishable from a hand that had nothing to search: depth is the last
    // layer that ran to the end, partial_depth the one that was cut, and stopped
    // says why ("" is never used - "none" means the think finished). Every fast
    // path that never ran a layered search leaves these at their defaults.
    int partial_depth = 0;
    int root_moves_done = 0;
    int root_moves_total = 0;
    int seldepth = 0;
    std::string stopped = "none";
};

// One frame per AlphaBeta layer, so a long think does not go silent between the
// start and the answer. The frame used to carry three ints and could not tell
// "ran out of budget inside layer 6" from "had nothing to search", which is the
// whole difference the layered accounting exists to show; it now carries that
// accounting and each caller picks what its consumer displays. elapsed_ms is the
// move clock read once per frame by SearchEngine, never a sum of phase slices.
using ProgressCallback = std::function<void(const SearchStats&)>;

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

    // The two budgets the client may name besides the hard wall clock (design doc
    // 2.3.1): a soft deadline that only decides whether another layer starts, and a
    // node ceiling that replaces the time-derived budget outright. Zero on the node
    // ceiling keeps the budget the remaining time derives; zero on the soft deadline
    // means no soft window at all, which is what every caller that never named one
    // had before. A node ceiling is what makes a training think cost the same twice,
    // which a millisecond budget cannot promise on a loaded machine.
    void set_soft_time_ms(int ms) { soft_time_ms_ = ms > 0 ? ms : 0; }
    void set_max_nodes(int nodes) { max_nodes_ = nodes > 0 ? nodes : 0; }

    void set_coefficients(const ThreatCoefficients& coeffs);

    // The book is owned by the caller and must stay alive and unmodified
    // while any search may be running against it.
    void set_opening_book(const OpeningBook* book) { book_ = book; }
    const OpeningBook* opening_book() const { return book_; }
    void set_book_enabled(bool v) { book_enabled_ = v; }

    // What the session's previous think concluded (design doc 2.4.3 ①③): the root
    // order starts from `move` and the aspiration window is centred on `score`.
    // Both are only hints - a stale one costs a re-search of the layers it misled,
    // never a different move - so the owner calls clear_hint() as soon as the board
    // stops being one the number described. Must be called from the thread that
    // searches, before search().
    void set_hint(Move move, int score) {
        hint_move_ = move;
        hint_score_ = score;
        has_hint_ = true;
    }
    void clear_hint() {
        hint_move_ = Move();
        hint_score_ = 0;
        has_hint_ = false;
    }

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

    // The last hint handed in by the session owner, read once per think into the
    // SearchLimits it searches under.
    Move hint_move_;
    int hint_score_ = 0;
    bool has_hint_ = false;

    bool book_enabled_;

    bool vcf_enabled_;
    bool vct_enabled_;
    bool alphabeta_enabled_;

    int vcf_max_depth_;
    int vct_max_depth_;
    int alphabeta_max_depth_;
    int soft_time_ms_ = 0;
    int max_nodes_ = 0;

    bool resign_detection_;
    int resign_node_limit_;

    ProgressCallback progress_callback_;
};

}

#endif

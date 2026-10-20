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
#include <memory>
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
    // §6.3.1：根着法逐个分值，按分值降序、最多 kRootScoreReportLimit 行。只有
    // AlphaBeta 的完整层会填它；快路径与战术层留空（它们的答案不是「比出来的」）。
    std::vector<RootMoveScore> root_scores;
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
    // How many of this think's nodes read a bound or a move out of the table (design
    // doc 2.9 :443, the last field on that row). The number a player's 复盘 needs to tell
    // "this reuse worked" from "this think re-proved everything": `nodes_searched` says
    // how much was searched, `tt_hits` says how much of it was not re-searched.
    int tt_hits = 0;
    // The soft window the AlphaBeta think was given, in milliseconds since the think
    // started (design doc 2.3.4). Zero is honest about three different cases: a think
    // with no clock at all, a think running on a named node budget, and every fast path
    // that never started a layered search. Only AlphaBeta fills it.
    int soft_ms = 0;
    // The wall that window was shaped from - this phase's share of the caller's
    // time_limit_ms after the tactical phases had spent part of it. Reported next to
    // soft_ms because the shaping is a ratio, and a ratio with one number shown is a
    // claim: 1950 says nothing about whether the engine was asked for 3000 or 4000.
    int hard_ms = 0;
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
    // `table` hands the whole engine a transposition table it does not own (design doc
    // 2.8 #1). It belongs on the constructor rather than as a setter because of what
    // the worker that shares one would otherwise do: allocate 7.3MB for itself, then
    // hand it back a moment later - and a block that size freed inside a thread's own
    // arena stays resident there, so the peak never drops and the decoupling this row
    // is about does not happen. nullptr (the default, and every caller that never
    // thought about it) keeps a table of its own, exactly as before.
    // The table must outlive the engine, and it must have been constructed `shared`
    // before more than one thread reaches it - that flag is what decides whether a
    // probe takes a lock.
    explicit SearchEngine(const ThreatCoefficients& coeffs,
                          TranspositionTable* table = nullptr);

    SearchResult search(const Board& board, int color,
                        int max_depth, int time_limit_ms,
                        std::atomic<bool>& stop_flag);

    // Design doc 2.4.3 ②「预搜对手应手」: the position a finished think's own PV
    // predicts, searched ahead on the session's table while nobody is waiting for it,
    // so the next think meets records deep enough to cut on. Deliberately narrower
    // than search(), because a ponder is not a move: no book, no VCF/VCT, no resign
    // probe, no progress frames, no hint, and no wall clock at all - only a node
    // budget, so whoever ends the ponder joins a thread that stops within the
    // 1024-node check instead of a deadline. `elapsed_ms` stays 0 for the same reason;
    // nothing this returns is delivered to anybody.
    SearchResult ponder_search(const Board& board, int color, int max_depth,
                               int node_cap, std::atomic<bool>& stop_flag);

    // §6.3.1 的补充：一个根着法的精确分值。根表里排在后面的着法带的是界，复盘
    // 要判「差多少」时必须付这一次钱。走的是 ponder 那条窄路（无书、无战术层、
    // 无时钟、只有节点帽），因为复盘要的是同一个深度的可比读数，不是一次完整思考。
    int exact_root_score(const Board& board, int color, Move move, int max_depth,
                         int node_cap, std::atomic<bool>& stop_flag);

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
    // What the next think is held to. Only AlphaBeta reads it; the tactical phases
    // and the resign probe still slice the millisecond budget.
    int max_nodes() const { return max_nodes_; }

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

    // Design doc 2.3.4 step 4. The budget the next think gets is shaped by what the
    // last one actually cost: a think that ran past 80 % of its hard limit is a
    // position this engine cannot read quickly, and asking the next one for the full
    // soft window is how two consecutive timeouts look to the player. Only the owner
    // of a sequence of thinks knows that number, so it hands it over; 0 - the
    // default, and every caller that never says - means "no previous think", which
    // leaves the shaping to the position alone.
    void set_prev_move_elapsed_ms(int ms) { prev_move_elapsed_ms_ = ms > 0 ? ms : 0; }
    int prev_move_elapsed_ms() const { return prev_move_elapsed_ms_; }

    // Which regime the caller is in, said out loud rather than guessed from one of the
    // numbers (design doc 2.3.4 step 5: 训练模式 - 时间预算不参与). A named node ceiling
    // already implies it, but `gomoku_train` with its default `--timeout-ms 400` and no
    // `--max-nodes` (self_play.h:15-18) is training too, and shaping a soft window out of
    // that wall would quietly change the depth every collected sample was searched to.
    void set_training(bool training) { training_ = training; }
    bool training() const { return training_; }

    // Whose clock the layered think reads its budget against. nullptr - the default,
    // and every caller in production - keeps the steady clock. This exists for design
    // doc §4's 「注入 mock clock」: the window is *chosen* in this class but only *spent*
    // inside AlphaBeta's root loop, so a clock handed in at the engine's door is what
    // proves the chosen number is the spent one. The tactical phases keep their own
    // slices of the wall clock; the layer-boundary decision this is about lives in
    // AlphaBeta's context.
    void set_clock(const SearchClock* clock) { clock_ = clock; }

    // Proves the position lost by exhausting every reply against the opponent's
    // forced VCF. Returns quickly on the first reply that survives, so the cost
    // only grows in positions that are genuinely lost. Only ever sets
    // SearchResult.resigned; failing to prove a loss keeps the AI playing.
    void set_resign_detection(bool v) { resign_detection_ = v; }
    void set_resign_node_limit(int n) { resign_node_limit_ = n; }

    TranspositionTable& transposition_table() { return *tt_alphabeta_; }

private:
    Evaluator evaluator_;
    GreedySearch greedy_;
    // Null exactly when tt_alphabeta_ points at a table somebody else owns.
    // VCF and VCT receive that pointer by reference but never read or write it;
    // keeping a single instance avoids allocating the extra tables a per-phase
    // instance would cost - 7.3MB each at this size - that are never touched.
    // It is heap-held because a worker that shares the trainer's table has no use
    // for its own copy, and a member object could not be given back.
    std::unique_ptr<TranspositionTable> owned_tt_;
    TranspositionTable* tt_alphabeta_;
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
    int prev_move_elapsed_ms_ = 0;
    bool training_ = false;
    const SearchClock* clock_ = nullptr;

    bool resign_detection_;
    int resign_node_limit_;

    ProgressCallback progress_callback_;
};

}

#endif

#ifndef GOMOKU_GAME_MANAGER_H
#define GOMOKU_GAME_MANAGER_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "../search/search_engine.h"
#include <memory>
#include <atomic>
#include <thread>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace gomoku {

enum class GameStatus {
    WAITING,
    PLAYING,
    GAME_OVER
};

// One ply of the session's own game, kept so the client can replay it with the
// engine's numbers. Human stones carry method "human" and no search data.
// The last five fields are AlphaBeta's layered accounting (search_engine.h
// SearchResult); they travel with the move so a replay can say which layer an
// interrupted think was in, and a record written before they existed reads as a
// think that finished.
struct MoveAnalysis {
    Move move;
    int score = 0;
    int depth = 0;
    int nodes = 0;
    int elapsed_ms = 0;
    std::string method;
    int partial_depth = 0;
    int root_moves_done = 0;
    int root_moves_total = 0;
    int seldepth = 0;
    std::string stopped = "none";
    // Last of the fields, because two sites build this struct by position
    // (`MoveAnalysis{move, score, depth, …}` at game_manager.cpp:209 and :505) and an
    // inserted field would silently re-word every one of them. Design doc 2.9 :443's
    // table-read count, kept so a 复盘 can tell a reused think from a re-proved one.
    int tt_hits = 0;
};

// Everything the replay view needs, captured in one lock so the move list and
// the ending cannot disagree. `move_count` is the board's stone count read in
// that same lock: the session flush compares it against what it last wrote, and
// reading board_ from the io thread outside this snapshot would race the worker.
struct ReplaySnapshot {
    std::vector<MoveAnalysis> moves;
    int winner = EMPTY;
    std::string reason;
    GameStatus status = GameStatus::WAITING;
    int move_count = 0;
    // The stones a hand-set position laid down and that are still on the board,
    // i.e. everything the board holds that `moves` did not play. Empty for a
    // game that started from an empty board, which is also what makes such a
    // record replayable on its own.
    std::vector<Move> base;
};

// A whole session as the store holds it, handed back to restore_game. `base` are
// the stones a hand-set position laid down (ReplaySnapshot::base) and `moves` the
// stones played after them, so the two together rebuild the board exactly. The
// split is what keeps the engine numbers of a played move distinguishable from
// pre-set stones.
struct GameRestore {
    std::vector<Move> base;
    std::vector<MoveAnalysis> moves;
    // EMPTY leaves the current side assignment alone.
    int player_color = EMPTY;
    GameStatus status = GameStatus::WAITING;
    int winner = EMPTY;
    std::string reason;
    bool position_pending = false;
};

class GameManager {
public:
    // The engine's per-layer accounting, handed on unchanged so the frame writer
    // decides what the client sees.
    using ProgressCallback = std::function<void(const SearchStats&)>;
    using MoveCallback = std::function<void(const Move& move, int score, int thinking_time_ms)>;
    using HintCallback = std::function<void(const Move& move, int score)>;
    using GameOverCallback = std::function<void(int winner, const std::string& reason)>;
    
    GameManager();
    ~GameManager();
    
    void reset();
    
    void set_player_color(int color);
    int player_color() const { return player_color_; }
    int ai_color() const { return ai_color_; }
    int current_turn() const;

    // Install a hand-set position without starting a game. The caller has
    // already validated it (position_setup), so this only has to stop any
    // worker, swap the board and drop the replay history: no stones of its own
    // are recorded, and no AI search is armed. The session then sits in
    // `position_pending()` until the player picks a side or plays, which is
    // what keeps choose_side usable on a board that is not empty.
    // The installed stones become base_stones(), which is what a persisted
    // session writes alongside the played moves.
    bool set_position(const Board& board);
    bool position_pending() const { return position_pending_; }

    // Rebuild a session from its persisted record: the board, the per-move engine
    // numbers, who plays which side, and how it ended. No callback fires and no
    // AI search is armed - the caller decides whether the side to move needs one.
    // A move that does not land on an empty square rejects the whole restore, so a
    // half-readable record can never become a playable board.
    bool restore_game(const GameRestore& restore);

    // Stop and join both search workers, discarding any in-flight best move. A
    // socket being detached has to use this before its callbacks are cleared: a
    // worker outliving the socket it writes into would either post onto a closed
    // stream or call through a dangling callback.
    void halt_search();
    
    bool make_move(int row, int col);
    // On success, out_move_count / out_status report the board state captured
    // before the AI worker is re-armed, so the caller never reads board_/status_
    // concurrently with that worker.
    bool undo(int& out_move_count, GameStatus& out_status);
    void resign(int color);
    
    bool request_hint(int timeout_ms, int max_depth);
    void start_ai_thinking(int timeout_ms, int max_depth);
    void start_ai_thinking();
    void stop_thinking();
    
    bool is_ai_thinking() const { return ai_thinking_.load(); }
    
    void set_progress_callback(ProgressCallback cb) { progress_callback_ = cb; }
    void set_move_callback(MoveCallback cb) { move_callback_ = cb; }
    void set_hint_callback(HintCallback cb) { hint_callback_ = cb; }
    void set_game_over_callback(GameOverCallback cb) { game_over_callback_ = cb; }
    
    const Board& board() const { return board_; }
    // The session's table, exposed so the generation a committed stone advances can
    // be read by the tests that own that rule.
    TranspositionTable& transposition_table() { return engine_.transposition_table(); }
    GameStatus status() const { return status_; }
    int winner() const { return winner_; }
    // Copies the session's own game for the replay view. Not const: it takes
    // thinking_mutex_, which the AI worker also holds when it commits a stone.
    ReplaySnapshot replay_snapshot();
    
    void set_coefficients(const ThreatCoefficients& coeffs);
    // 丁-3：按色的两套。两侧同值时与上面那句是同一台引擎、同一个评估器。
    void set_coefficients(const CoefficientPair& coeffs);
    // soft_time_ms / max_nodes are optional overrides: a negative value means
    // "whatever this session already has", so the two older callers and a client
    // that names only the timeout keep the engine's own budget (0 on the soft window
    // means the client named none, which is what lets SearchEngine shape one from the
    // hard wall per design doc 2.3.4; 0 on the node ceiling keeps the time-derived
    // ceiling).
    void set_config(int timeout_ms, int max_depth, int soft_time_ms = -1, int max_nodes = -1);
    int timeout_ms() const { return timeout_ms_; }
    int max_depth() const { return max_depth_; }
    int soft_time_ms() const { return soft_time_ms_; }
    int max_nodes() const { return max_nodes_; }
    // What the last think of this session cost, which is the input the next think's
    // soft window is shaped with. Read for the tests and the UI's own report of why a
    // move came back when it did.
    int prev_think_elapsed_ms() const { return prev_think_elapsed_ms_.load(); }

    // Design doc 2.4.3 ②「预置复用」, the session's side of it. After the AI commits a
    // stone, the idle time before the human's next stone is spent searching the reply
    // the finished think's own PV predicted, on this session's table, so the next think
    // meets records at the depth it will ask for instead of re-proving them (a normal
    // cross-move hit is two plies shallower than the new root needs - see REVIEW_LOG
    // 轮次140 读数 2). `percent` is that pre-search's budget as a share of the think
    // that predicted the reply; 0 - the default - never arms one, because the cost is a
    // second engine thread per waiting session and the doc prices no such thing.
    // The ponder delivers nothing: no frame, no analysis row, no 上一手耗时, and the
    // table's generation only steps for stones that were actually played.
    void set_ponder(int percent);
    int ponder_percent() const { return ponder_percent_; }
    bool ponder_active() const { return ponder_active_.load(); }
    // The two witnesses, read-only: how many pre-searches this session armed since it
    // was opened, and what the last one that ended cost.
    int ponder_arms() const { return ponder_arms_.load(); }
    int ponder_last_nodes() const { return ponder_last_nodes_.load(); }

    void set_opening_book(const OpeningBook* book);
    
private:
    Board board_;
    SearchEngine engine_;
    Evaluator evaluator_;
    
    int player_color_;
    int ai_color_;
    GameStatus status_;
    int winner_;
    // True between "position installed" and "the player chose a side or played"
    // (see set_position). Only ever touched on the io thread and under
    // thinking_mutex_ inside set_position / reset.
    bool position_pending_ = false;
    std::string game_over_reason_;
    std::vector<MoveAnalysis> analyses_;
    
    std::atomic<bool> ai_thinking_;
    std::atomic<bool> stop_flag_;
    std::atomic<bool> hint_active_;
    // The idle pre-search's own stop flag and busy flag: 终止 stops it with the think,
    // while `ai_thinking_` keeps saying only what it said before - that a move is being
    // looked for. `make_move` / `undo` refuse while a think runs but not while a ponder
    // runs, so those two end the ponder by joining it instead.
    std::atomic<bool> ponder_active_;
    std::atomic<bool> ponder_stop_;
    // Witnesses, because 「一个预搜不落子、不进复盘、也不算成上一手耗时」 is a claim
    // about a thread nobody can see: how many pre-searches this session armed, and what
    // the last one that ended cost. A session with --ponder 0 keeps both at 0.
    std::atomic<int> ponder_arms_{0};
    std::atomic<int> ponder_last_nodes_{0};
    // Hard abort: reset / resign / destructor paths flip this before joining
    // the worker. The worker must discard its in-flight best move. Plain
    // stop_thinking leaves it false so the aborted worker still commits the
    // last completed best move and the game does not get stuck with ai_color_
    // to move and no re-arm.
    std::atomic<bool> discard_pending_;
    std::unique_ptr<std::thread> search_thread_;
    std::unique_ptr<std::thread> hint_thread_;
    std::unique_ptr<std::thread> ponder_thread_;
    std::mutex thinking_mutex_;
    
    int timeout_ms_;
    int max_depth_;
    // Named by set_config when a client wants a think to stop before the hard
    // deadline or to cost a fixed number of nodes; 0 on either is what every
    // session had before this existed - no soft window, and the node ceiling the
    // remaining time derives.
    int soft_time_ms_ = 0;
    int max_nodes_ = 0;
    // 0 arms nothing (see set_ponder); otherwise it is the share of the predicting
    // think's node count that the pre-search is allowed to spend.
    int ponder_percent_ = 0;
    // The searching thread writes this after a think hands back its move; the thread
    // that spawns the next one reads it into the Budget it passes by value, which is
    // why it is atomic rather than a plain int like the two above.
    std::atomic<int> prev_think_elapsed_ms_{0};

    // What the session's last completed AlphaBeta think concluded, for the next
    // think of the same colour to reuse as a root candidate and an aspiration
    // centre (design doc 2.4.3 ①③). `hint_color_` is whose perspective
    // `hint_score_` speaks from, and it is the gate: a think for the other colour
    // would read the same number with the sign meaning the opposite. Any path that
    // replaces the board outright - 悔棋, 摆棋, 重开, a restored record - invalidates
    // it, because the number no longer describes a position that exists.
    Move hint_move_;
    int hint_score_ = 0;
    int hint_color_ = EMPTY;
    bool has_hint_ = false;

    // One think's share of the above, read on the thread that spawns the worker.
    struct Reuse {
        Move move;
        int score = 0;
        bool valid = false;
    };
    Reuse take_hint(int for_color) const {
        if (!has_hint_ || hint_color_ != for_color) return Reuse{};
        return Reuse{hint_move_, hint_score_, true};
    }

    // One think's share of the budget config, read on the spawning thread for the
    // same reason as the reuse above: engine_ state is written by the searching
    // thread only, and set_config runs on the io thread while a worker is inside
    // engine_.search.
    struct Budget {
        int soft_time_ms = 0;
        int max_nodes = 0;
        // What the previous think of this session cost, in milliseconds (design doc
        // 2.3.4 step 4). Carried by value for the same reason as the two above: the
        // spawning thread reads it, the searching thread writes it.
        int prev_elapsed_ms = 0;
    };
    Budget take_budget() const {
        return Budget{soft_time_ms_, max_nodes_, prev_think_elapsed_ms_.load()};
    }
    
    ProgressCallback progress_callback_;
    MoveCallback move_callback_;
    HintCallback hint_callback_;
    GameOverCallback game_over_callback_;
    
    void check_game_over();
    // The reuse is what take_hint() handed back on the thread that spawned this
    // worker: the engine's hint is written by the searching thread and read by no
    // other, so the worker never touches the session's own members for it.
    void ai_thinking_worker(int timeout_ms, int max_depth, Reuse reuse, Budget budget);
    // Stop and join the idle pre-search, leaving `ponder_stop_` ready for the next one.
    // Same rule as halt_search: never with thinking_mutex_ held.
    void cancel_ponder();
    // Sets game-over state without joining the search threads, so it is safe to
    // call from ai_thinking_worker itself.
    void resign_now(int color);

  public:
    // #81 的接口，加在类的最后：`game_manager.h:142/188-196/204-208/239-247` 按行号被指认过。
    //
    // 这一次思考的子有没有已经交出去。`ai_move` 是在提交段里发出去的（`thinking_mutex_` 之内），
    // 而 `ai_thinking_` 要到 worker 的最后一行才清 —— 这两件事之间到达的合法子，今天被 :215
    // 一拒就不再有第二次的机会（轮次178 量到 4/3326 局）。这个记号就是「已经被应承过」与
    // 「人在 AI 想完之前提前点了一手」的分界：前者该等，后者照旧拒。
    bool answer_delivered() const { return ai_answer_delivered_.load(); }

    // 思考彻底结束（标志已交回）时的通知。由服务端装上，内容只有一个：把「可以取队列了」
    // post 回 io 线程。**不在 worker 线程上做这件事** —— 被取出的那手棋会调 make_move，
    // 而 make_move 自己就会点下一手思考，`start_ai_thinking` 开头又去 join 上一个 worker，
    // 在 worker 线程上跑它等于让线程 join 自己（轮次183 就是挂死在这里）。
    using ThinkFinishedCallback = std::function<void()>;
    void set_think_finished_callback(ThinkFinishedCallback cb) {
        think_finished_callback_ = std::move(cb);
    }

  private:
    std::atomic<bool> ai_answer_delivered_{false};
    ThinkFinishedCallback think_finished_callback_;
};

}

#endif

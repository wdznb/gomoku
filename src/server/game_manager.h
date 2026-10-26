#ifndef GOMOKU_GAME_MANAGER_H
#define GOMOKU_GAME_MANAGER_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "../search/search_engine.h"
#include "owner_checked_mutex.h"
#include <memory>
#include <atomic>
#include <cassert>
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
    // (`MoveAnalysis{move, score, depth, …}` at game_manager.cpp:264 and :607) and an
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
    
    // #85：回调族的交换以前只靠一句顺序 —— 先 `halt_search()`（真的 join）再动这六个
    // `std::function`，注释写着「Only ever called after the search has been joined」，但那
    // 是当前树的读数，不是约束。这里把它做成机制，形状与轮次199 那把带锁的属主登记型相同（那个类型已在轮次209 删除，机制留在 `OwnerOnlySection` 里）：
    // 第一次交换的线程记成属主，此后每一次交换都要求**同一线程**且**没有在飞的东西**
    //（`ai_thinking_` / `hint_active_` / `ponder_active_` 全假），否则断言当场崩。
    // 没有新锁、没有等待：两个判断都只是原子读。
    void set_progress_callback(ProgressCallback cb) {
        claim_callback_swap();
        progress_callback_ = std::move(cb);
    }
    void set_move_callback(MoveCallback cb) {
        claim_callback_swap();
        move_callback_ = std::move(cb);
    }
    void set_hint_callback(HintCallback cb) {
        claim_callback_swap();
        hint_callback_ = std::move(cb);
    }
    void set_game_over_callback(GameOverCallback cb) {
        claim_callback_swap();
        game_over_callback_ = std::move(cb);
    }

    // 观察面：属主是谁、这一手在这里能不能交换。引擎没有任何地方按它们分支。
    std::thread::id callbacks_owner() const {
        return callbacks_owner_.load(std::memory_order_relaxed);
    }
    bool callbacks_swappable_here() const {
        const std::thread::id recorded = callbacks_owner();
        const bool same_thread =
            recorded == std::thread::id{} || recorded == std::this_thread::get_id();
        return same_thread && !anything_in_flight();
    }
    
    const Board& board() const { return board_; }
    // The session's table, exposed so the generation a committed stone advances can
    // be read by the tests that own that rule.
    TranspositionTable& transposition_table() { return engine_.transposition_table(); }
    GameStatus status() const { return status_; }
    int winner() const { return winner_; }
    // Copies the session's own game for the replay view. Not const: it takes
    // the session lock that used to sit here (`thinking_mutex_`, removed in
    // 轮次205 #113), which the AI worker also held when it commits a stone.
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
    // (see set_position). Only ever touched on the io thread - it used to be
    // additionally fenced by `thinking_mutex_` inside set_position / reset.
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
    // centre (design doc 2.4.3 ①③). `color` is whose perspective
    // `score` speaks from, and it is the gate: a think for the other colour
    // would read the same number with the sign meaning the opposite. Any path that
    // replaces the board outright - 悔棋, 摆棋, 重开, a restored record - invalidates
    // it, because the number no longer describes a position that exists.
    //
    // 轮次205 #113：一份结论分两段走。**发布**在思考线程那一侧（一次原子 exchange，
    // 与 `pending_result_` 同形），**取用与失效**只在属主线程 —— `current_hint_` 因此
    // 只有一个读者一个写者，`thinking_mutex_` 不再为它存在。以前是 worker 抱着锁直接写
    // 那四个字段（本轮的仪器量到该获取点恒有两根线程），所以锁看着像「缓存需要锁」，
    // 其实需要的是「跨线程的那一跳有出口」。
    struct HintConclusion {
        Move move;
        int score = 0;
        int color = EMPTY;
        bool valid = false;
    };
    HintConclusion current_hint_;
    std::atomic<HintConclusion*> published_hint_{nullptr};
    // 只登记、不阻塞：谁第一次取用/失效，谁就是这一局提示缓存的属主线程；换线程取用当场 assert。
    OwnerOnlySection hint_adopter_;

    // One think's share of the above, read on the thread that spawns the worker.
    struct Reuse {
        Move move;
        int score = 0;
        bool valid = false;
    };
    // 取用之前先把别人发布的结论收进属主线程自己的那份 —— 这就是「消息队列回属主」的
    // 那一跳：worker 只管 exchange 一次，谁在属主线程上谁 adopt。
    //
    // 轮次206 #114：这一跳的**方向**得有读者。轮次205 的存活臂证明 TSan 看不见那个窗口
    // （树上每一次取用都在 join 之后，两根线程从不同时在场），所以「races=0」不能当证据；
    // 而 `anything_in_flight()` 也不是那条纪律 —— 读代码看到的：`request_hint` 在 :404 就把
    // `hint_active_` 置真，之后 :416 才取用，用那个谓词断言会**在正常路径上当场假崩**。
    // 能成立的那句话是「取用只在属主线程」，于是就用只登记不阻塞的那一档机制去钉它。
    void adopt_published_hint() {
        const std::lock_guard<OwnerOnlySection> owner(hint_adopter_);
        const std::unique_ptr<HintConclusion> taken(
            published_hint_.exchange(nullptr, std::memory_order_acq_rel));
        if (taken) current_hint_ = *taken;
    }
    // 盘面被整块换掉的那几条路（悔棋 / 摆棋 / 重开 / 恢复棋谱）：属主线程自己的那份作废，
    // 顺带把还没被取用的发布也丢掉 —— 它描述的是已经不存在的局面。
    void discard_hint_cache() {
        const std::lock_guard<OwnerOnlySection> owner(hint_adopter_);
        current_hint_ = HintConclusion{};
        delete published_hint_.exchange(nullptr, std::memory_order_acq_rel);
    }
    bool hint_adoption_thread_is_owner() const { return hint_adopter_.current_thread_is_owner(); }

    Reuse take_hint(int for_color) {
        adopt_published_hint();
        if (!current_hint_.valid || current_hint_.color != for_color) return Reuse{};
        return Reuse{current_hint_.move, current_hint_.score, true};
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
    //
    // 轮次205 #113：盘面快照也归这条线程所有，但**取它的是属主线程** —— 快照在
    // `start_ai_thinking` 里复制好、按值交进来（与求招臂 :405/:422 和预搜臂 :636/:660
    // 同一个形状）。以前是这条线程抱着 `thinking_mutex_` 复制活盘，本轮的仪器在的那个
    // 获取点读到「两根线程」（每一手思考都是一根新线程），那把锁于是非删不可：它守的
    // 不是并发写，而是「跨线程去读别人的盘」。复制份数不变：本来就要拷一次。
    void ai_thinking_worker(Board snapshot, int timeout_ms, int max_depth, Reuse reuse,
                            Budget budget);
    // Stop and join the idle pre-search, leaving `ponder_stop_` ready for the next one.
    // Same rule as halt_search: never with the session's own lock held (there is no
    // lock here any more; the rule survives as "join before you touch").
    void cancel_ponder();
    // Sets game-over state without joining the search threads, so it is safe to
    // call from ai_thinking_worker itself.
    void resign_now(int color);

  public:
    // 轮次193 #106：一局的盘面只有一个写者 —— 属主线程（服务端就是那条 io 线程）。
    //
    // worker 想完只把结论停进一个原子槽（一次 exchange，无锁），再调一次这个回调；回调
    // **只许 post**，不许碰游戏状态、也不许拿 `games_mutex_`（轮次187 的挂死就是 worker
    // 就地拿锁）。落子、`analyses_`、提示、`move_callback_` 与 `check_game_over()` 全在
    // 属主线程的 `commit_pending()` 里做，于是 io 线程读整盘（`state_sync` /
    // `replay_snapshot`）与提交之间不再有第二个写者 —— 轮次193 改前实测那一族 TSan race
    // 共 7 条：主线程 `Board(const Board&)` 读 vs worker 持 `thinking_mutex_` 的写
    // （`board.cpp:64` ×2、`:145` ×2、`:65` ×1，另两条报在 `board.h:45` 的拷贝上）。
    //
    // 不装回调时（单元测试与训练器的无头用法）worker 就地提交，与这一轮之前逐字同形：
    // 那条路上属主线程在 join 之前不读盘，本来就没有第二个读者。
    using OwnerNotify = std::function<void()>;
    // 第六个同族、同一个闸（轮次193 #106 之后 worker 只碰这一位，所以它更需要在飞时禁换）。
    void set_owner_notify(OwnerNotify cb) {
        claim_callback_swap();
        owner_notify_ = std::move(cb);
    }
    // 属主线程：把 worker 停下（或被中止）的那份结论落盘。槽空就是空操作 —— 通知迟到
    // 是正常形状（`halt_search` 会先把槽取走，`exchange` 让两边恰好有一个拿到它）。
    void commit_pending();

    // 轮次190 #105：求招收尾那一句「这张盘还是不是我给它算的那张」抽成一个判据，让它可以
    // 被同步地钉住（并发那一面由 `test_hint_answer_for_a_superseded_position` 盖）。比的必须是
    // 号而不是子数：悔棋两子再落回一手会把子数补回原样，而那张盘已经不是原来那张。
    bool board_matches_hint_snapshot(int snapshot_epoch) const {
        return board_epoch_.load(std::memory_order_acquire) == snapshot_epoch;
    }

  private:
    // 轮次193 #106：结论槽。同一局同一刻只有一手思考，生产者（worker）与消费者
    // （`commit_pending`）从不同时在场，所以一个原子指针就是完整的队列。
    std::atomic<SearchResult*> pending_result_{nullptr};
    // #85：回调族的属主线程（第一次交换时登记）。与轮次199 那把锁的属主槽同一形状：
    // 一次 CAS 登记、若干次 relaxed 读，没有第二把锁。
    std::atomic<std::thread::id> callbacks_owner_{};

    // 在飞 = 有别的线程可能正拿着这六个 function 走。三个旗标各自覆盖一条路：
    // 思考、求招、预搜。
    bool anything_in_flight() const {
        return ai_thinking_.load(std::memory_order_relaxed) ||
               hint_active_.load(std::memory_order_relaxed) ||
               ponder_active_.load(std::memory_order_relaxed);
    }

    void claim_callback_swap() {
        // 两条分开 assert：一条合取只会说「不成立」，说不出是哪一半 —— 而这两半的修法不同。
        const std::thread::id swapper = std::this_thread::get_id();
        const std::thread::id first = callbacks_owner_.load(std::memory_order_relaxed);
        assert((first == std::thread::id{} || first == swapper) &&
               "回调族只能由第一次交换它的那个线程再交换（#85）");
        assert(!anything_in_flight() &&
               "有思考/求招/预搜在飞时不许交换回调（#85）");
        const std::thread::id self = std::this_thread::get_id();
        std::thread::id recorded{};
        callbacks_owner_.compare_exchange_strong(recorded, self, std::memory_order_relaxed);
    }
    OwnerNotify owner_notify_;
    // 两条提交路的同一具身体：无头路径由 worker 自己调，服务端由属主线程调。
    void apply_search_result(SearchResult result);
    // halt_search 用：这一手不要了 —— 槽里的结论扔掉，思考标志照旧交回（worker 以前
    // 是自己走到最后一行清它的；现在它停在 post 那一步就返回了）。
    void drop_pending_result();
    // 轮次190 #105，加在类的最后（上面 #81 那条规矩：这个头文件的 `:142/188-196/204-208/239-247`
    // 都被按行号指认过）。盘面版本号：每一个改 `board_` 的地方 +1，改盘的顺序也由它说话。
    // 求招 worker 判断「这张盘还是不是我给它算的那张」时**只 load 这个号**，不回读 `board_` ——
    // 玩家那两条路（`make_move` / `undo`）改盘不拿 `thinking_mutex_`，只挡
    // `ai_thinking_`，而它们都不挡 `hint_active_`，所以那记回读是一 pair 真 race（TSan 量到）。
    // 与轮次187 那条规矩同形：worker 交回结论/下判断的那一步只看原子号，不碰锁、不碰活盘。
    std::atomic<int> board_epoch_{0};
};

}

#endif

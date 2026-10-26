#include "game_manager.h"
#include "core/pattern.h"
#include <chrono>

namespace gomoku {

namespace {
// timeout_ms is client-controlled (set_config / request_hint) and feeds the
// search deadline, the resign-probe node budget and the worker's wait. An
// unbounded value makes the AI search for effectively forever, and a huge one
// also overflowed the probe-node arithmetic. Clamp to a sane wall-clock range.
// The top is 2 minutes because the page bounds the deadline by the depth it is
// searching to (2x..4x, in seconds) and lets a player ask for depth 20.
int clamp_timeout_ms(int timeout_ms) {
    if (timeout_ms < 100) return 100;
    if (timeout_ms > 120000) return 120000;
    return timeout_ms;
}

// max_depth is client-controlled through the same two doors, and the page's
// 2..20 is only an input rule: iterative deepening runs depth 1..max_depth and
// is stopped by nothing but the deadline, so an unbounded value buys no stronger
// move and a socket client can ask for one. Clamp to what the page can express.
int clamp_max_depth(int max_depth) {
    if (max_depth < 1) return 1;
    if (max_depth > 20) return 20;
    return max_depth;
}

// The soft deadline is the point a think stops starting another layer, so a value
// at or above the hard one asks for nothing the hard one does not already give:
// keep it inside the window it is meant to cut short. Zero stays zero, which is
// the engine's "no soft window" answer rather than a deadline of no length.
int clamp_soft_time_ms(int soft_time_ms, int hard_time_ms) {
    if (soft_time_ms <= 0) return 0;
    if (soft_time_ms > hard_time_ms) return hard_time_ms;
    return soft_time_ms;
}

// A node budget is what makes a think cost the same twice, and unlike the timeout
// it cannot be turned into unbounded work by a big value: the hard deadline still
// applies. Zero means the time-derived ceiling this engine has always used.
int clamp_max_nodes(int max_nodes) {
    return max_nodes > 0 ? max_nodes : 0;
}

// The idle pre-search's share of the think that predicted the reply. Both bounds are
// about what a *ponder* is for: with no floor, a think of a few thousand nodes leaves
// the pre-search nothing to prove and the next think re-proves the whole tree; with no
// ceiling, a session that was allowed two minutes for a move would spend another
// minute of idle CPU on a guess the opponent may not even play. Neither bound is a
// wall clock, so the join that ends a ponder still lands at the next node check.
int ponder_node_budget(int think_nodes, int percent) {
    if (think_nodes <= 0 || percent <= 0) return 0;
    long long budget = static_cast<long long>(think_nodes) * percent / 100;
    if (budget < 4000) budget = 4000;
    if (budget > 200000) budget = 200000;
    return static_cast<int>(budget);
}
}

GameManager::GameManager() 
    : player_color_(BLACK), ai_color_(WHITE),
      status_(GameStatus::WAITING), winner_(EMPTY),
      ai_thinking_(false), stop_flag_(false), hint_active_(false),
      ponder_active_(false), ponder_stop_(false),
      discard_pending_(false),
      timeout_ms_(5000), max_depth_(8) {
    // The engine only fires its own progress hook; forward it to whichever
    // consumer installed a GameManager progress_callback_ so the client's
    // "thinking_progress" messages actually flow.
    engine_.set_progress_callback([this](const SearchStats& stats) {
        if (progress_callback_) {
            progress_callback_(stats);
        }
    });
}

GameManager::~GameManager() {
    halt_search();
}

// Hard abort then join, so no thread is running when this returns. Must be
// called while the search is still in flight: since 轮次205 #113 the worker commits
// nothing at all (it publishes into an atomic slot), and before that it held the session
// lock to do so,
// and joining while holding it would deadlock against the thread being joined.
void GameManager::halt_search() {
    discard_pending_.store(true);
    stop_thinking();
    if (search_thread_ && search_thread_->joinable()) {
        search_thread_->join();
    }
    if (hint_thread_ && hint_thread_->joinable()) {
        hint_thread_->join();
    }
    search_thread_.reset();
    hint_thread_.reset();
    // The join above can leave a fresh ponder behind: the AI worker arms one as its
    // last act, so a halt that arrived while it was still running has to take it off
    // too before anything reads the board or the table this session owns.
    cancel_ponder();
    // 轮次193 #106：worker 也可能已经把结论停在槽里等属主线程来取。halt 的意思是
    // 「这一手不要了」，所以在这里取走扔掉 —— 与 join 一起，这就是「迟到的通知什么
    // 都不做」的保证（通知那边只有一次 exchange，取到空槽就返回）。
    drop_pending_result();
}

// Stop and join the idle pre-search. Never called with the session lock held (there
// hasn't been one since 轮次205 #113) - the rule that mattered was the join order: the
// caller is about to touch engine_ or board_, and the ponder thread is the only other
// thing that touches either of them while it runs.
void GameManager::cancel_ponder() {
    ponder_stop_.store(true);
    if (ponder_thread_ && ponder_thread_->joinable()) {
        ponder_thread_->join();
    }
    ponder_thread_.reset();
    ponder_active_.store(false);
    // Ready for the next arm: the flag a ponder reads is this one, and a stale true
    // would make the next pre-search stop on its very first node.
    ponder_stop_.store(false);
}

void GameManager::set_ponder(int percent) {
    // Outside that range the number means nothing the arm can use: a zero or negative
    // share arms nothing, and a share over 100 % is a pre-search that costs more than
    // the think that predicted it - at which point the honest setting is 100.
    ponder_percent_ = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    if (ponder_percent_ == 0) cancel_ponder();
}

void GameManager::reset() {
    halt_search();
    board_.reset();
    board_epoch_.fetch_add(1, std::memory_order_release);
    status_ = GameStatus::WAITING;
    winner_ = EMPTY;
    game_over_reason_.clear();
    analyses_.clear();
    position_pending_ = false;
    discard_hint_cache();
    // A new game has no previous think, and 上一手耗时 is exactly the kind of number
    // that would otherwise carry across the boundary and damp the first move of the
    // next game by the last move of this one.
    prev_think_elapsed_ms_.store(0);
}

bool GameManager::set_position(const Board& board) {
    // A full board has no side to move, so installing one would leave the
    // session with nothing to do. The loader's own validation rejects it too;
    // this guard is here because set_position is also callable directly by the
    // trainer and the tests.
    if (board.move_count() >= MAX_MOVES) return false;

    // Same hard-abort sequence as reset(): the in-flight worker must not commit
    // a stone onto the board that replaces the one it searched.
    halt_search();

    board_ = board;
    board_epoch_.fetch_add(1, std::memory_order_release);
    status_ = GameStatus::WAITING;
    winner_ = EMPTY;
    game_over_reason_.clear();
    analyses_.clear();
    position_pending_ = true;
    discard_hint_cache();
    return true;
}

bool GameManager::restore_game(const GameRestore& restore) {
    // Same hard-abort sequence as set_position: a worker still searching would
    // commit its stone onto the board being rebuilt.
    halt_search();

    // Rebuilt on a scratch board so a record that goes illegal half way leaves
    // the session's own board untouched and the caller can fall back to a
    // fresh game.
    Board fresh;
    for (const Move& stone : restore.base) {
        if (stone.color != BLACK && stone.color != WHITE) return false;
        if (!fresh.place_stone(stone.pos.row, stone.pos.col, stone.color)) return false;
    }
    std::vector<MoveAnalysis> fresh_analyses;
    for (const MoveAnalysis& entry : restore.moves) {
        const Move& move = entry.move;
        if (move.color != BLACK && move.color != WHITE) return false;
        if (!fresh.place_stone(move.pos.row, move.pos.col, move.color)) return false;
        fresh_analyses.push_back(entry);
    }
    board_ = fresh;
    board_epoch_.fetch_add(1, std::memory_order_release);
    analyses_ = std::move(fresh_analyses);
    if (restore.player_color == BLACK || restore.player_color == WHITE) {
        player_color_ = restore.player_color;
        ai_color_ = opponent(restore.player_color);
    }
    status_ = restore.status;
    winner_ = restore.status == GameStatus::GAME_OVER ? restore.winner : EMPTY;
    game_over_reason_ = restore.status == GameStatus::GAME_OVER ? restore.reason : std::string();
    position_pending_ = restore.position_pending;
    discard_hint_cache();
    return true;
}

void GameManager::set_player_color(int color) {
    player_color_ = color;
    ai_color_ = opponent(color);
    // Picking a side is how a hand-set position becomes a game.
    position_pending_ = false;
}

int GameManager::current_turn() const {
    if (board_.move_count() % 2 == 0) {
        return BLACK;
    }
    return WHITE;
}

bool GameManager::make_move(int row, int col) {
    if (status_ == GameStatus::GAME_OVER) return false;
    if (ai_thinking_.load()) return false;

    // The idle pre-search is a guess about what this stone will be, and the stone has
    // now arrived: end it before this thread touches board_ or advances the table's
    // generation, because the two would otherwise be written from both sides.
    cancel_ponder();

    int turn = current_turn();
    // While a hand-set position is still pending, no side has been claimed yet,
    // so this stone is the claim (README lets 落子 start the game instead of
    // choose_side). It has to happen before the colour guard below, which would
    // otherwise compare the turn against the default player_color_.
    if (position_pending_) {
        player_color_ = turn;
        ai_color_ = opponent(turn);
    }
    // Only the human side may call make_move. If stop_thinking aborted the AI
    // worker before it placed a stone, the board still shows AI's color to
    // move; without this guard the human's next make_move would silently
    // commit a stone for ai_color_ and corrupt the Zobrist parity.
    if (turn != player_color_) return false;
    // Explicit occupancy check before the forbidden scan. is_forbidden on an
    // occupied square early-returns a clean pattern (no stone to test), so
    // the previous path silently reached place_stone with an occupied cell
    // and reported the same generic failure via an unintended branch.
    if (!board_.is_empty(row, col)) return false;
    // Renju forbids BLACK from creating double-three, double-four, or an
    // overline. Every AI search path already rejects these; the human must
    // be bound by the same rules or VCF/resign proofs of the form
    // "BLACK cannot legally play that block" are unsound.
    if (turn == BLACK && PatternRecognizer::is_forbidden(board_, row, col, BLACK)) {
        return false;
    }
    if (!board_.place_stone(row, col, turn)) {
        return false;
    }
    board_epoch_.fetch_add(1, std::memory_order_release);
    // Playing a stone is the other way out of a hand-set position.
    position_pending_ = false;
    // The human's stone ages the table the same way the AI's does: one move, one
    // step, whichever side played it.
    engine_.transposition_table().advance_generation();
    analyses_.push_back(MoveAnalysis{Move(row, col, turn), 0, 0, 0, 0, "human"});
    
    if (status_ == GameStatus::WAITING) {
        status_ = GameStatus::PLAYING;
    }
    
    check_game_over();
    
    if (status_ == GameStatus::PLAYING && current_turn() == ai_color_) {
        start_ai_thinking(timeout_ms_, max_depth_);
    }
    
    return true;
}

bool GameManager::undo(int& out_move_count, GameStatus& out_status) {
    if (ai_thinking_.load()) return false;
    if (position_pending_) return false;
    if (board_.move_count() == 0) return false;
    // Taking a stone back rewrites the board the pre-search is reading a copy of and
    // the table it is writing; end it first, same rule as make_move.
    cancel_ponder();
    // Base stones are not a history: 摆棋 put them there and only another 摆棋
    // takes them off. With no analysed move to give back there is nothing to
    // undo, so a board of nothing but pre-placed stones refuses.
    if (analyses_.empty()) return false;

    int undo_count = 1;
    if (board_.move_count() >= 2 && current_turn() == player_color_) {
        undo_count = 2;
    }
    // Never pop more board stones than the replay list can account for, or the
    // two views of the same history would drift apart.
    if (undo_count > static_cast<int>(analyses_.size())) {
        undo_count = static_cast<int>(analyses_.size());
    }
    
    int undone = 0;
    for (int i = 0; i < undo_count && board_.move_count() > 0; ++i) {
        board_.undo_last_move();
        board_epoch_.fetch_add(1, std::memory_order_release);
        ++undone;
    }
    // The board and the replay list are the same history seen two ways, so a
    // stone the board gave up has to leave the analysis list too.
    for (int i = 0; i < undone && !analyses_.empty(); ++i) {
        analyses_.pop_back();
    }
    // A hint from the position that was just unpicked no longer describes a
    // position the session will search.
    discard_hint_cache();
    
    if (board_.move_count() == 0) {
        status_ = GameStatus::WAITING;
        winner_ = EMPTY;
        game_over_reason_.clear();
    } else if (status_ == GameStatus::GAME_OVER) {
        status_ = GameStatus::PLAYING;
        winner_ = EMPTY;
        game_over_reason_.clear();
    }
    
    // Capture the post-undo state now, before the worker is re-armed below.
    // Once start_ai_thinking() runs, that worker writes board_/status_ under
    // thinking_mutex_ (removed in 轮次205 #113); reading them from the io thread
    // afterwards would have raced.
    out_move_count = board_.move_count();
    out_status = status_;

    // Re-trigger AI if it is now AI's turn to move. Covers two cases:
    //  1) board still non-empty and it's AI's turn (player undid one move)
    //  2) board became empty and AI plays BLACK - the game must reopen
    //     itself, mirroring what handle_choose_side does on empty boards.
    if (current_turn() == ai_color_) {
        start_ai_thinking(timeout_ms_, max_depth_);
    }
    
    return true;
}

ReplaySnapshot GameManager::replay_snapshot() {
    ReplaySnapshot snapshot;
    snapshot.moves = analyses_;
    snapshot.winner = winner_;
    snapshot.reason = game_over_reason_;
    snapshot.status = status_;
    snapshot.move_count = board_.move_count();
    // The board's own stack is base stones then played stones, so whatever the
    // stack holds beyond the analysis list is what a hand-set position put there
    // and no undo has taken back off yet.
    const std::vector<Move>& history = board_.move_history();
    const size_t played = analyses_.size();
    if (history.size() > played) {
        snapshot.base.assign(history.begin(), history.end() - static_cast<std::ptrdiff_t>(played));
    }
    return snapshot;
}

void GameManager::resign(int color) {
    halt_search();

    if (status_ == GameStatus::GAME_OVER) return;
    
    resign_now(color);
}

void GameManager::resign_now(int color) {
    if (status_ == GameStatus::GAME_OVER) return;

    winner_ = opponent(color);
    game_over_reason_ = "resign";
    status_ = GameStatus::GAME_OVER;

    if (game_over_callback_) {
        game_over_callback_(winner_, game_over_reason_);
    }
}

bool GameManager::request_hint(int timeout_ms, int max_depth) {
    // 死局不再求招：其余入口都已在终局处拒绝，放行只会为一局不会再落的棋
    // 烧掉整段 timeout，再回一手无人能落的推荐。
    if (status_ == GameStatus::GAME_OVER) return false;
    if (ai_thinking_.load() || hint_active_.load()) return false;
    timeout_ms = clamp_timeout_ms(timeout_ms);
    max_depth = clamp_max_depth(max_depth);

    if (hint_thread_ && hint_thread_->joinable()) {
        hint_thread_->join();
    }
    // One engine, one thread inside it: the idle pre-search has to be off the table
    // before a 求招 think starts.
    cancel_ponder();

    hint_active_.store(true);
    stop_flag_.store(false);
    // A hint is a fresh delivery, so it has to undo the discard mark the last
    // halt_search left: 摆棋/重开 route through halt_search, and without this the
    // next hint searches to completion and then drops the answer in silence.
    discard_pending_.store(false);
    int turn = current_turn();
    Board board_snapshot = board_;
    // 这一号的值就是「上面那次拷贝发生时的盘面版本」，收尾时拿它比的是同一格的号，而不是
    // 再去读一次活盘（#105：`make_move`/`undo` 改盘不拿 `thinking_mutex_`，也不挡 `hint_active_`）。
    const int snapshot_epoch = board_epoch_.load(std::memory_order_acquire);
    // Captured on this thread and copied into the worker: see start_ai_thinking.
    Reuse reuse = take_hint(turn);

    // The budget is the session's, but 求招 carries its own timeout and that is the
    // hard deadline the soft window has to fit inside.
    Budget budget = take_budget();
    budget.soft_time_ms = clamp_soft_time_ms(budget.soft_time_ms, timeout_ms);

    hint_thread_ = std::make_unique<std::thread>(
        [this, board_snapshot, turn, timeout_ms, max_depth, snapshot_epoch, reuse,
         budget]() mutable {
            engine_.set_soft_time_ms(budget.soft_time_ms);
            engine_.set_max_nodes(budget.max_nodes);
            engine_.set_prev_move_elapsed_ms(budget.prev_elapsed_ms);
            if (reuse.valid) {
                engine_.set_hint(reuse.move, reuse.score);
            } else {
                engine_.clear_hint();
            }
            SearchResult result = engine_.search(board_snapshot, turn, max_depth, timeout_ms, stop_flag_);
            prev_think_elapsed_ms_.store(result.elapsed_ms);

            // A user stop_thinking only raises stop_flag_, so delivering on the
            // discard flag alone still answers a stopped hint and releases the
            // client's 求招 button; reset / resign / the destructor set
            // discard_pending_ first and must stay silent.
            if (!discard_pending_.load() && hint_callback_) {
                // The answer describes board_snapshot. make_move and undo do not
                // wait for an in-flight hint, so a stone landed in the meantime
                // makes it advice about a board that no longer exists. 比的是**盘面版本号**
                // 而不是活盘（#105）：那两条路改盘既不拿 `thinking_mutex_`、也不挡
                // `hint_active_`，所以在锁里读 `board_` 也挡不住它们 —— 只有这个号两边都会动。
                const bool same_board = board_matches_hint_snapshot(snapshot_epoch);
                // The same test the answer is delivered on: when the board is
                // still the position this hint was computed from, the next 求招
                // on it is searching exactly what this one just searched, so it
                // starts from this conclusion.
                // 轮次205 #113：结论只做一次原子发布（与 #106 那个 `pending_result_` 同形），
                // 取用与失效都在属主线程的 `take_hint` / `discard_hint_cache` 里 —— 这条线程
                // 从此不再碰会话的字段，`thinking_mutex_` 也就没了第二根线程。
                if (same_board) {
                    delete published_hint_.exchange(
                        new HintConclusion{result.best_move, result.score, turn,
                                           result.method == "alphabeta"},
                        std::memory_order_acq_rel);
                }
                if (!same_board) {
                    hint_callback_(Move(), 0);
                    hint_active_.store(false);
                    return;
                }
                // Deliver even when best_move is invalid (timeout with no PV):
                // the server distinguishes valid (hint_result) from invalid
                // (error) so the client is never left waiting on silence.
                hint_callback_(result.best_move, result.score);
            }
            hint_active_.store(false);
        });
    return true;
}

void GameManager::start_ai_thinking(int timeout_ms, int max_depth) {
    stop_thinking();
    if (search_thread_ && search_thread_->joinable()) {
        search_thread_->join();
    }
    if (hint_thread_ && hint_thread_->joinable()) {
        hint_thread_->join();
    }
    // Last, not first: the AI worker is the thread that arms a ponder, so its own join
    // is what makes "no thread is inside engine_" true. After this line nothing but the
    // thread armed below touches the engine.
    cancel_ponder();
    // Both workers are joined above, so any hint one of them wrote is visible here.
    Reuse reuse;
    
    // If the game is being opened by the AI (human picked WHITE, or an undo
    // emptied the board while ai_color_ == BLACK), status_ is still WAITING.
    // The worker's commit path checks status_ == PLAYING, so without this
    // promotion the AI's first stone is silently dropped and the human's
    // next make_move is rejected by the turn guard - session lockup.
    // 两条 worker 都在上面 join 掉了（`stop_thinking` → join → `cancel_ponder`），所以这里
    // 没有第二个写者：轮次205 #113 把锁撤了，纪律换成「join 之后属主线程才动这些字段」。
    if (status_ == GameStatus::WAITING) {
        status_ = GameStatus::PLAYING;
    }
    // 快照在**属主线程**上复制好、按值交进去：那条线程读的是自己这份副本，不再读活盘；
    // 复制份数与改前相同（改前是它在锁里拷一份）。提示也是按值交进去的，同一个理由。
    Board snapshot = board_;
    reuse = take_hint(ai_color_);

    stop_flag_.store(false);
    discard_pending_.store(false);
    ai_thinking_.store(true);
    timeout_ms_ = timeout_ms;
    max_depth_ = max_depth;
    
    search_thread_ = std::make_unique<std::thread>(
        &GameManager::ai_thinking_worker, this, std::move(snapshot), timeout_ms, max_depth,
        reuse, take_budget());
}

void GameManager::start_ai_thinking() {
    start_ai_thinking(timeout_ms_, max_depth_);
}

void GameManager::stop_thinking() {
    stop_flag_.store(true);
    // 终止 means "stop working on this position", and the idle pre-search is work on a
    // position too - one the player has not even agreed to. The join that follows at
    // the next arm point is what actually ends it.
    ponder_stop_.store(true);
}

void GameManager::ai_thinking_worker(Board snapshot, int timeout_ms, int max_depth,
                                     Reuse reuse, Budget budget) {

    engine_.set_soft_time_ms(budget.soft_time_ms);
    engine_.set_max_nodes(budget.max_nodes);
    engine_.set_prev_move_elapsed_ms(budget.prev_elapsed_ms);
    if (reuse.valid) {
        engine_.set_hint(reuse.move, reuse.score);
    } else {
        engine_.clear_hint();
    }

    SearchResult result = engine_.search(snapshot, ai_color_, max_depth, timeout_ms, stop_flag_);
    prev_think_elapsed_ms_.store(result.elapsed_ms);

    // 轮次193 #106：这条线程不再改盘。结论停进槽里（一次 release exchange），通知只做
    // 一次 post；落子、`analyses_`、`move_callback_` 与标志交回都在属主线程里做 —— 盘面
    // 因此只有一个写者。回调不装（无头用法）时就地提交，与这一轮之前同一具身体、同一个线程。
    if (owner_notify_) {
        // 同一局同一刻只有一手思考，槽本来就该是空的；真留着上一份只能说明属主线程
        // 没来取过，而那时新一手是点不起来的（make_move / undo 都挡在 ai_thinking_ 上）。
        delete pending_result_.exchange(new SearchResult(std::move(result)),
                                        std::memory_order_acq_rel);
        owner_notify_();
        return;
    }
    apply_search_result(std::move(result));
}

// 本函数就是原来 worker 的提交段：谁调它，谁就是这一局的属主线程。无头路径由 worker
// 自己调；服务端由 io 线程在 commit_pending() 里调。
void GameManager::apply_search_result(SearchResult result) {
    if (result.resigned) {
        // Interruption is not a refutation: the proven loss is the engine's own
        // conclusion, so 终止 must still end the game. Skipping it here would
        // leave the session with ai_color_ to move and no frame coming.
        if (!discard_pending_.load() && status_ == GameStatus::PLAYING) {
            resign_now(ai_color_);
        }
        ai_thinking_.store(false);
        return;
    }

    // What the idle pre-search would work on: decided under the lock below and armed
    // after it is released (design doc 2.4.3 ②). The board is a *prediction* - this
    // think's own variation played one reply further - which is the whole point: a
    // record written at ply 2 of the previous think is two plies shallower than the
    // next root asks for, while a pre-search of that very position writes them at the
    // depth the next think can actually cut on.
    Board ponder_root;
    int ponder_nodes = 0;

    if (move_callback_ && result.best_move.pos.is_valid()) {
        // A user stop_thinking sets stop_flag_ but leaves discard_pending_
        // false: the worker still commits its last completed best move so
        // the game does not get stuck with ai_color_ to move and no re-arm.
        // reset / resign / destructor set discard_pending_ first, so those
        // paths never see a stale AI stone.
        if (!discard_pending_.load() &&
            status_ == GameStatus::PLAYING &&
            current_turn() == ai_color_ &&
            board_.place_stone(result.best_move.pos, ai_color_)) {
            // 这一手改盘要能被别的线程知道，而它们读不到 `board_`（读不得，见求招收尾那段）：
            // 号在这里 +1，与玩家那两条路同一口径。
            board_epoch_.fetch_add(1, std::memory_order_release);
            // The table's clock steps once per committed stone, not once per think:
            // the records this think wrote stay in the table for the moves that
            // follow, which is what lets a later think meet the same position by a
            // different move order and read them. A ponder is not a stone, so it
            // advances nothing here.
            engine_.transposition_table().advance_generation();
            analyses_.push_back(MoveAnalysis{result.best_move, result.score, result.depth,
                                             result.nodes_searched, result.elapsed_ms,
                                             result.method, result.partial_depth,
                                             result.root_moves_done, result.root_moves_total,
                                             result.seldepth, result.stopped,
                                             result.tt_hits});
            // The session remembers this think for the next one of the same colour.
            // Only AlphaBeta's number is a position value: the tactical and forced
            // paths report a sentinel, and centring a window on one of those buys
            // nothing but widened re-searches.
            delete published_hint_.exchange(
                new HintConclusion{result.best_move, result.score, ai_color_,
                                   result.method == "alphabeta"},
                std::memory_order_acq_rel);
            // ai_move 与标志交回之间不再有「窗口」：两件事同在本函数里，而本函数整段
            // 在属主线程上跑 —— io 线程不可能在这中间读一条玩家消息（轮次178 的
            // 4/3326 就是旧形状下那段尾巴；旧补丁按 io 线程排队取件，这里不需要了）。
            move_callback_(result.best_move, result.score, result.elapsed_ms);

            check_game_over();

            // Armed only for the shape this is about: a layered think, a game still
            // alive, a reply left in its own variation, and a human now holding the
            // move. A fast-path answer predicts nothing, and 终止 / 认输 / a finished
            // game leave nothing idle to fill.
            if (ponder_percent_ > 0 && result.method == "alphabeta" &&
                !result.resigned && result.principal_variation.size() >= 2 &&
                status_ == GameStatus::PLAYING && current_turn() == player_color_) {
                const Move reply = result.principal_variation[1];
                Board root = board_;
                if (reply.color == current_turn() &&
                    root.place_stone(reply.pos, reply.color)) {
                    ponder_root = root;
                    ponder_nodes = ponder_node_budget(result.nodes_searched, ponder_percent_);
                }
            }
        }
    }

    if (ponder_nodes > 0) {
        // Nothing should still be running at this point - every path that arms one
        // joins the previous one first - but replacing a joinable std::thread calls
        // std::terminate, and an idle pre-search is not worth that class of bug.
        cancel_ponder();
        ponder_active_.store(true);
        // Counted before the thread exists, so a test that waits for this worker to
        // finish can read the arm without racing the ponder itself.
        ponder_arms_.fetch_add(1);
        const int ponder_color = ai_color_;
        // 旧形状里这里捕的是 worker 那次思考的 `max_depth` 入参；搬进属主线程之后，
        // 同一格读数就是此刻配置的那一档（这次思考正是按它起的）。
        const int ponder_depth = max_depth();
        ponder_thread_ = std::make_unique<std::thread>(
            [this, ponder_root, ponder_color, ponder_nodes, ponder_depth]() {
                const SearchResult pondered = engine_.ponder_search(
                    ponder_root, ponder_color, ponder_depth, ponder_nodes, ponder_stop_);
                ponder_last_nodes_.store(pondered.nodes_searched);
                ponder_active_.store(false);
            });
    }

    ai_thinking_.store(false);
}

void GameManager::commit_pending() {
    SearchResult* parked = pending_result_.exchange(nullptr, std::memory_order_acq_rel);
    if (parked == nullptr) return;
    std::unique_ptr<SearchResult> result(parked);
    apply_search_result(std::move(*result));
}

void GameManager::drop_pending_result() {
    SearchResult* parked = pending_result_.exchange(nullptr, std::memory_order_acq_rel);
    if (parked == nullptr) return;
    delete parked;
    // 结论不要了，但 worker 替这一手攥着的标志必须交回 —— 旧形状里它是自己走到最后
    // 一行清的，现在它停在 post 那一步就返回了。
    ai_thinking_.store(false);
}

void GameManager::check_game_over() {
    int winner = board_.check_winner();
    if (winner != EMPTY) {
        winner_ = winner;
        game_over_reason_ = "five";
        status_ = GameStatus::GAME_OVER;
        
        if (game_over_callback_) {
            game_over_callback_(winner_, game_over_reason_);
        }
        return;
    }
    
    if (board_.move_count() >= MAX_MOVES) {
        winner_ = EMPTY;
        game_over_reason_ = "draw";
        status_ = GameStatus::GAME_OVER;
        
        if (game_over_callback_) {
            game_over_callback_(winner_, game_over_reason_);
        }
    }
}

void GameManager::set_coefficients(const ThreatCoefficients& coeffs) {
    set_coefficients(CoefficientPair::shared(coeffs));
}

void GameManager::set_coefficients(const CoefficientPair& coeffs) {
    // The engine's own setter clears its transposition table (指导书 §4 丁-3 注意 2);
    // this evaluator is the one that prices the panel's per-move numbers, so it has
    // to move to the same pair in the same call.
    evaluator_.set_coefficients(coeffs);
    engine_.set_coefficients(coeffs);
}

void GameManager::set_opening_book(const OpeningBook* book) {
    engine_.set_opening_book(book);
}

void GameManager::set_config(int timeout_ms, int max_depth,
                             int soft_time_ms, int max_nodes) {
    timeout_ms_ = clamp_timeout_ms(timeout_ms);
    max_depth_ = clamp_max_depth(max_depth);
    // An unnamed override keeps what the session already had: the same rule the
    // frame reader applies, so set_config is the one place that decides a budget.
    if (soft_time_ms >= 0) soft_time_ms_ = clamp_soft_time_ms(soft_time_ms, timeout_ms_);
    if (max_nodes >= 0) max_nodes_ = clamp_max_nodes(max_nodes);
}

}

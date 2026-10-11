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
struct MoveAnalysis {
    Move move;
    int score = 0;
    int depth = 0;
    int nodes = 0;
    int elapsed_ms = 0;
    std::string method;
};

// Everything the replay view needs, captured in one lock so the move list and
// the ending cannot disagree.
struct ReplaySnapshot {
    std::vector<MoveAnalysis> moves;
    int winner = EMPTY;
    std::string reason;
    bool game_over = false;
};

class GameManager {
public:
    using ProgressCallback = std::function<void(int depth, int nodes, int elapsed_ms)>;
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
    bool set_position(const Board& board);
    bool position_pending() const { return position_pending_; }
    
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
    GameStatus status() const { return status_; }
    int winner() const { return winner_; }
    // Copies the session's own game for the replay view. Not const: it takes
    // thinking_mutex_, which the AI worker also holds when it commits a stone.
    ReplaySnapshot replay_snapshot();
    
    void set_coefficients(const ThreatCoefficients& coeffs);
    void set_config(int timeout_ms, int max_depth);
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
    // Hard abort: reset / resign / destructor paths flip this before joining
    // the worker. The worker must discard its in-flight best move. Plain
    // stop_thinking leaves it false so the aborted worker still commits the
    // last completed best move and the game does not get stuck with ai_color_
    // to move and no re-arm.
    std::atomic<bool> discard_pending_;
    std::unique_ptr<std::thread> search_thread_;
    std::unique_ptr<std::thread> hint_thread_;
    std::mutex thinking_mutex_;
    
    int timeout_ms_;
    int max_depth_;
    
    ProgressCallback progress_callback_;
    MoveCallback move_callback_;
    HintCallback hint_callback_;
    GameOverCallback game_over_callback_;
    
    void check_game_over();
    void ai_thinking_worker(int timeout_ms, int max_depth);
    // Sets game-over state without joining the search threads, so it is safe to
    // call from ai_thinking_worker itself.
    void resign_now(int color);
};

}

#endif

#ifndef GOMOKU_GAME_MANAGER_H
#define GOMOKU_GAME_MANAGER_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "../search/search_engine.h"
#include "protocol.h"
#include <memory>
#include <atomic>
#include <thread>
#include <functional>
#include <mutex>

namespace gomoku {

enum class GameStatus {
    WAITING,
    PLAYING,
    GAME_OVER
};

class GameManager {
public:
    using ProgressCallback = std::function<void(int depth, int nodes, int elapsed_ms)>;
    using MoveCallback = std::function<void(const Move& move, int score, int thinking_time_ms)>;
    using GameOverCallback = std::function<void(int winner, const std::string& reason)>;
    
    GameManager();
    ~GameManager();
    
    void reset();
    
    void set_player_color(int color);
    int player_color() const { return player_color_; }
    int ai_color() const { return ai_color_; }
    int current_turn() const;
    
    bool make_move(int row, int col);
    bool undo();
    void resign(int color);
    
    void request_hint(int timeout_ms, int max_depth);
    void start_ai_thinking(int timeout_ms, int max_depth);
    void start_ai_thinking();
    void stop_thinking();
    
    bool is_ai_thinking() const { return ai_thinking_.load(); }
    
    void set_progress_callback(ProgressCallback cb) { progress_callback_ = cb; }
    void set_move_callback(MoveCallback cb) { move_callback_ = cb; }
    void set_game_over_callback(GameOverCallback cb) { game_over_callback_ = cb; }
    
    const Board& board() const { return board_; }
    GameStatus status() const { return status_; }
    int winner() const { return winner_; }
    
    void set_coefficients(const ThreatCoefficients& coeffs);
    void set_config(int timeout_ms, int max_depth);
    
private:
    Board board_;
    SearchEngine engine_;
    Evaluator evaluator_;
    
    int player_color_;
    int ai_color_;
    GameStatus status_;
    int winner_;
    std::string game_over_reason_;
    
    std::atomic<bool> ai_thinking_;
    std::atomic<bool> stop_flag_;
    std::atomic<bool> hint_active_;
    std::unique_ptr<std::thread> search_thread_;
    std::mutex thinking_mutex_;
    
    int timeout_ms_;
    int max_depth_;
    
    ProgressCallback progress_callback_;
    MoveCallback move_callback_;
    GameOverCallback game_over_callback_;
    
    void check_game_over();
    void ai_thinking_worker(int timeout_ms, int max_depth);
};

}

#endif

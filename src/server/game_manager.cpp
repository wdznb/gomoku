#include "game_manager.h"
#include <chrono>

namespace gomoku {

GameManager::GameManager() 
    : player_color_(BLACK), ai_color_(WHITE),
      status_(GameStatus::WAITING), winner_(EMPTY),
      ai_thinking_(false), stop_flag_(false), hint_active_(false),
      timeout_ms_(5000), max_depth_(8) {
}

GameManager::~GameManager() {
    stop_thinking();
    if (search_thread_ && search_thread_->joinable()) {
        search_thread_->join();
    }
    if (hint_thread_ && hint_thread_->joinable()) {
        hint_thread_->join();
    }
}

void GameManager::reset() {
    stop_thinking();
    if (search_thread_ && search_thread_->joinable()) {
        search_thread_->join();
    }
    if (hint_thread_ && hint_thread_->joinable()) {
        hint_thread_->join();
    }
    search_thread_.reset();
    hint_thread_.reset();
    board_.reset();
    status_ = GameStatus::WAITING;
    winner_ = EMPTY;
    game_over_reason_.clear();
}

void GameManager::set_player_color(int color) {
    player_color_ = color;
    ai_color_ = opponent(color);
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
    
    int turn = current_turn();
    if (!board_.place_stone(row, col, turn)) {
        return false;
    }
    
    if (status_ == GameStatus::WAITING) {
        status_ = GameStatus::PLAYING;
    }
    
    check_game_over();
    
    if (status_ == GameStatus::PLAYING && current_turn() == ai_color_) {
        start_ai_thinking(timeout_ms_, max_depth_);
    }
    
    return true;
}

bool GameManager::undo() {
    if (ai_thinking_.load()) return false;
    if (board_.move_count() == 0) return false;
    
    int undo_count = 1;
    if (board_.move_count() >= 2 && current_turn() == player_color_) {
        undo_count = 2;
    }
    
    for (int i = 0; i < undo_count && board_.move_count() > 0; ++i) {
        board_.undo_last_move();
    }
    
    if (board_.move_count() == 0) {
        status_ = GameStatus::WAITING;
    }
    
    if (status_ == GameStatus::GAME_OVER) {
        status_ = GameStatus::PLAYING;
        winner_ = EMPTY;
        game_over_reason_.clear();
    }
    
    return true;
}

void GameManager::resign(int color) {
    if (status_ == GameStatus::GAME_OVER) return;
    
    winner_ = opponent(color);
    game_over_reason_ = "resign";
    status_ = GameStatus::GAME_OVER;
    
    if (game_over_callback_) {
        game_over_callback_(winner_, game_over_reason_);
    }
}

void GameManager::request_hint(int timeout_ms, int max_depth) {
    if (ai_thinking_.load() || hint_active_.load()) return;
    
    if (hint_thread_ && hint_thread_->joinable()) {
        hint_thread_->join();
    }
    
    hint_active_.store(true);
    stop_flag_.store(false);
    int turn = current_turn();
    Board board_snapshot = board_;
    
    hint_thread_ = std::make_unique<std::thread>(
        [this, board_snapshot, turn, timeout_ms, max_depth]() mutable {
            SearchResult result = engine_.search(board_snapshot, turn, max_depth, timeout_ms, stop_flag_);
            
            if (move_callback_ && !stop_flag_.load() && result.best_move.pos.is_valid()) {
                move_callback_(result.best_move, result.score, 0);
            }
            hint_active_.store(false);
        });
}

void GameManager::start_ai_thinking(int timeout_ms, int max_depth) {
    if (ai_thinking_.load()) return;
    
    stop_thinking();
    if (search_thread_ && search_thread_->joinable()) {
        search_thread_->join();
    }
    if (hint_thread_ && hint_thread_->joinable()) {
        hint_thread_->join();
    }
    
    stop_flag_.store(false);
    ai_thinking_.store(true);
    timeout_ms_ = timeout_ms;
    max_depth_ = max_depth;
    
    search_thread_ = std::make_unique<std::thread>(
        &GameManager::ai_thinking_worker, this, timeout_ms, max_depth);
}

void GameManager::start_ai_thinking() {
    start_ai_thinking(timeout_ms_, max_depth_);
}

void GameManager::stop_thinking() {
    stop_flag_.store(true);
    ai_thinking_.store(false);
}

void GameManager::ai_thinking_worker(int timeout_ms, int max_depth) {
    SearchResult result = engine_.search(board_, ai_color_, max_depth, timeout_ms, stop_flag_);
    
    if (move_callback_ && !stop_flag_.load() && result.best_move.pos.is_valid()) {
        std::lock_guard<std::mutex> lock(thinking_mutex_);
        if (board_.place_stone(result.best_move.pos, ai_color_)) {
            move_callback_(result.best_move, result.score, result.elapsed_ms);
            
            check_game_over();
        }
    }
    
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
    evaluator_.set_coefficients(coeffs);
    engine_.set_coefficients(coeffs);
}

void GameManager::set_config(int timeout_ms, int max_depth) {
    timeout_ms_ = timeout_ms;
    max_depth_ = max_depth;
}

}

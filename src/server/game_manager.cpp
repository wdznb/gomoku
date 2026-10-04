#include "game_manager.h"
#include <chrono>

namespace gomoku {

GameManager::GameManager() 
    : player_color_(BLACK), ai_color_(WHITE),
      status_(GameStatus::WAITING), winner_(EMPTY),
      ai_thinking_(false), stop_flag_(false),
      timeout_ms_(5000), max_depth_(8) {
}

void GameManager::reset() {
    stop_thinking();
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
    
    board_.undo_last_move();
    
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
    if (ai_thinking_.load()) return;
    
    int turn = current_turn();
    Move hint = search_.search(board_, turn);
    
    if (move_callback_) {
        move_callback_(hint, 0, 0);
    }
}

void GameManager::start_ai_thinking(int timeout_ms, int max_depth) {
    if (ai_thinking_.load()) return;
    
    stop_flag_.store(false);
    ai_thinking_.store(true);
    timeout_ms_ = timeout_ms;
    max_depth_ = max_depth;
    
    thinking_future_ = std::async(std::launch::async, [this, timeout_ms, max_depth]() {
        return search_.search(board_, ai_color_);
    });
    
    std::thread([this, timeout_ms]() {
        ai_thinking_worker(timeout_ms, max_depth_);
    }).detach();
}

void GameManager::stop_thinking() {
    stop_flag_.store(true);
    
    if (thinking_future_.valid()) {
        try {
            thinking_future_.wait_for(std::chrono::milliseconds(100));
        } catch (...) {}
    }
    
    ai_thinking_.store(false);
}

void GameManager::ai_thinking_worker(int timeout_ms, int max_depth) {
    auto start = std::chrono::high_resolution_clock::now();
    
    Move best_move = search_.search(board_, ai_color_);
    
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    ai_thinking_.store(false);
    
    if (move_callback_ && !stop_flag_.load()) {
        board_.place_stone(best_move.pos, ai_color_);
        move_callback_(best_move, 0, static_cast<int>(elapsed));
        
        check_game_over();
        
        if (status_ == GameStatus::PLAYING && current_turn() == ai_color_) {
            start_ai_thinking(timeout_ms_, max_depth_);
        }
    }
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
    search_.set_coefficients(coeffs);
}

}

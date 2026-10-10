#include "game_manager.h"
#include "core/pattern.h"
#include <chrono>

namespace gomoku {

GameManager::GameManager() 
    : player_color_(BLACK), ai_color_(WHITE),
      status_(GameStatus::WAITING), winner_(EMPTY),
      ai_thinking_(false), stop_flag_(false), hint_active_(false),
      discard_pending_(false),
      timeout_ms_(5000), max_depth_(8) {
}

GameManager::~GameManager() {
    discard_pending_.store(true);
    stop_thinking();
    if (search_thread_ && search_thread_->joinable()) {
        search_thread_->join();
    }
    if (hint_thread_ && hint_thread_->joinable()) {
        hint_thread_->join();
    }
}

void GameManager::reset() {
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
    {
        std::lock_guard<std::mutex> lock(thinking_mutex_);
        board_.reset();
        status_ = GameStatus::WAITING;
        winner_ = EMPTY;
        game_over_reason_.clear();
    }
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
        winner_ = EMPTY;
        game_over_reason_.clear();
    } else if (status_ == GameStatus::GAME_OVER) {
        status_ = GameStatus::PLAYING;
        winner_ = EMPTY;
        game_over_reason_.clear();
    }
    
    // Re-trigger AI if it is now AI's turn to move. Covers two cases:
    //  1) board still non-empty and it's AI's turn (player undid one move)
    //  2) board became empty and AI plays BLACK - the game must reopen
    //     itself, mirroring what handle_choose_side does on empty boards.
    if (current_turn() == ai_color_) {
        start_ai_thinking(timeout_ms_, max_depth_);
    }
    
    return true;
}

void GameManager::resign(int color) {
    discard_pending_.store(true);
    stop_thinking();
    if (search_thread_ && search_thread_->joinable()) {
        search_thread_->join();
    }
    if (hint_thread_ && hint_thread_->joinable()) {
        hint_thread_->join();
    }
    
    std::lock_guard<std::mutex> lock(thinking_mutex_);
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
    stop_thinking();
    if (search_thread_ && search_thread_->joinable()) {
        search_thread_->join();
    }
    if (hint_thread_ && hint_thread_->joinable()) {
        hint_thread_->join();
    }
    
    // If the game is being opened by the AI (human picked WHITE, or an undo
    // emptied the board while ai_color_ == BLACK), status_ is still WAITING.
    // The worker's commit path checks status_ == PLAYING, so without this
    // promotion the AI's first stone is silently dropped and the human's
    // next make_move is rejected by the turn guard - session lockup.
    {
        std::lock_guard<std::mutex> lock(thinking_mutex_);
        if (status_ == GameStatus::WAITING) {
            status_ = GameStatus::PLAYING;
        }
    }

    stop_flag_.store(false);
    discard_pending_.store(false);
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
}

void GameManager::ai_thinking_worker(int timeout_ms, int max_depth) {
    Board snapshot;
    {
        std::lock_guard<std::mutex> lock(thinking_mutex_);
        snapshot = board_;
    }

    SearchResult result = engine_.search(snapshot, ai_color_, max_depth, timeout_ms, stop_flag_);

    if (result.resigned) {
        std::lock_guard<std::mutex> lock(thinking_mutex_);
        if (!discard_pending_.load() && !stop_flag_.load() && status_ == GameStatus::PLAYING) {
            resign_now(ai_color_);
        }
        ai_thinking_.store(false);
        return;
    }

    if (move_callback_ && result.best_move.pos.is_valid()) {
        std::lock_guard<std::mutex> lock(thinking_mutex_);
        // A user stop_thinking sets stop_flag_ but leaves discard_pending_
        // false: the worker still commits its last completed best move so
        // the game does not get stuck with ai_color_ to move and no re-arm.
        // reset / resign / destructor set discard_pending_ first, so those
        // paths never see a stale AI stone.
        if (!discard_pending_.load() &&
            status_ == GameStatus::PLAYING &&
            current_turn() == ai_color_ &&
            board_.place_stone(result.best_move.pos, ai_color_)) {
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

void GameManager::set_opening_book(const OpeningBook* book) {
    engine_.set_opening_book(book);
}

void GameManager::set_config(int timeout_ms, int max_depth) {
    timeout_ms_ = timeout_ms;
    max_depth_ = max_depth;
}

}

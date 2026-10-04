#include "controller.h"

namespace gomoku {

SearchController::SearchController() : stop_flag_(false), thinking_(false) {}

SearchController::SearchController(const ThreatCoefficients& coeffs)
    : engine_(coeffs), stop_flag_(false), thinking_(false) {}

SearchController::~SearchController() {
    stop();
    if (search_thread_ && search_thread_->joinable()) {
        search_thread_->join();
    }
}

void SearchController::start_search(const Board& board, int color,
                                     int max_depth, int time_limit_ms) {
    if (thinking_.load()) {
        stop();
        if (search_thread_ && search_thread_->joinable()) {
            search_thread_->join();
        }
    }

    stop_flag_.store(false);
    thinking_.store(true);

    search_thread_ = std::make_unique<std::thread>(
        &SearchController::search_worker, this, board, color, max_depth, time_limit_ms);
}

void SearchController::stop() {
    stop_flag_.store(true);
}

SearchResult SearchController::get_result() const {
    std::lock_guard<std::mutex> lock(result_mutex_);
    return last_result_;
}

void SearchController::set_progress_callback(ProgressCallback cb) {
    progress_callback_ = cb;
    engine_.set_progress_callback(cb);
}

void SearchController::set_coefficients(const ThreatCoefficients& coeffs) {
    engine_.set_coefficients(coeffs);
}

void SearchController::search_worker(Board board, int color,
                                      int max_depth, int time_limit_ms) {
    SearchResult result = engine_.search(board, color, max_depth, time_limit_ms, stop_flag_);

    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_result_ = result;
    }

    thinking_.store(false);

    if (completion_callback_) {
        completion_callback_(result);
    }
}

}

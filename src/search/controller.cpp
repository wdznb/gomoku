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
    delete published_result_.exchange(nullptr, std::memory_order_acq_rel);
}

void SearchController::start_search(const Board& board, int color,
                                     int max_depth, int time_limit_ms) {
    if (thinking_.load()) {
        stop();
    }
    // Join unconditionally before reassigning search_thread_. The previous
    // worker may have finished (thinking_ == false) while the std::thread is
    // still joinable; resetting the unique_ptr over a joinable thread calls
    // std::terminate. Joining an already-finished thread is cheap.
    if (search_thread_ && search_thread_->joinable()) {
        search_thread_->join();
    }

    stop_flag_.store(false);
    thinking_.store(true);

    // One Board copy for the whole hand-off, taken after the join above so the worker
    // that is still running cannot be reading the member. The thread holds a
    // reference_wrapper, not a second Board, so nothing else in this path gets
    // transposed (design doc 2.8 #5).
    search_board_ = board;
    search_thread_ = std::make_unique<std::thread>(
        &SearchController::search_worker, this, std::cref(search_board_), color,
        max_depth, time_limit_ms);
}

void SearchController::stop() {
    stop_flag_.store(true);
}

SearchResult SearchController::get_result() {
    // 取走即消耗：`exchange` 给这根线程独占权，所以复制 `*taken` 期间没有别人能删它，
    // 也不需要一把锁或一套回收机制。槽里没东西（还没思考、或已被读走）就交回默认值。
    const std::unique_ptr<SearchResult> taken(
        published_result_.exchange(nullptr, std::memory_order_acq_rel));
    if (!taken) return SearchResult{};
    return std::move(*taken);
}

void SearchController::set_progress_callback(ProgressCallback cb) {
    progress_callback_ = cb;
    engine_.set_progress_callback(cb);
}

void SearchController::set_coefficients(const ThreatCoefficients& coeffs) {
    engine_.set_coefficients(coeffs);
}

void SearchController::search_worker(const Board& board, int color,
                                     int max_depth, int time_limit_ms) {
    SearchResult result = engine_.search(board, color, max_depth, time_limit_ms, stop_flag_);

    // 发布 = 一次 exchange：上一份要是没被读者取走，由这根线程换下来并删掉，槽里最多一份。
    result_publishes_.fetch_add(1, std::memory_order_relaxed);
    SearchResult* stale = published_result_.exchange(new SearchResult(result),
                                                     std::memory_order_acq_rel);
    delete stale;

    thinking_.store(false);

    if (completion_callback_) {
        completion_callback_(result);
    }
}

}

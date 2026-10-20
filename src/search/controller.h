#ifndef GOMOKU_CONTROLLER_H
#define GOMOKU_CONTROLLER_H

#include "search_engine.h"
#include <atomic>
#include <thread>
#include <future>
#include <mutex>
#include <functional>

namespace gomoku {

class SearchController {
public:
    SearchController();
    explicit SearchController(const ThreatCoefficients& coeffs);
    ~SearchController();

    void start_search(const Board& board, int color,
                      int max_depth, int time_limit_ms);

    void stop();

    bool is_thinking() const { return thinking_.load(); }

    SearchResult get_result() const;

    void set_progress_callback(ProgressCallback cb);
    void set_completion_callback(std::function<void(SearchResult)> cb) {
        completion_callback_ = cb;
    }

    void set_coefficients(const ThreatCoefficients& coeffs);

    SearchEngine& engine() { return engine_; }

private:
    SearchEngine engine_;
    std::atomic<bool> stop_flag_;
    std::atomic<bool> thinking_;

    mutable std::mutex result_mutex_;
    SearchResult last_result_;

    std::unique_ptr<std::thread> search_thread_;

    // The position one think is run from, owned by the controller rather than by the
    // thread's argument list: `search_worker` reads it through a reference, so a think
    // hands over one Board and no second transposition. Writing it is only safe once
    // the previous worker is done, and `start_search` joins unconditionally before it
    // gets here (controller.cpp, round 34 F2) - that join is what makes this member
    // readable by the next worker without racing the last one.
    Board search_board_;

    ProgressCallback progress_callback_;
    std::function<void(SearchResult)> completion_callback_;

    void search_worker(const Board& board, int color, int max_depth, int time_limit_ms);
};

}

#endif

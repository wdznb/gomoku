#ifndef GOMOKU_CONTROLLER_H
#define GOMOKU_CONTROLLER_H

#include "search_engine.h"
#include <atomic>
#include <thread>
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

    // 轮次211 #113：一次思考的结论用**一条消息**交回，不再用「共享字段 + mutex」。读者取走
    // 即消耗（第二次读回的是默认值），这不是省事而是唯一能不引回收机制的做法：`SearchResult`
    // 里有 string 和三个 vector，读者要能在写者还在发布时安全复制，就得要么加锁要么做
    // hazard pointer —— 而这个类在产品里没有使用者（`grep` 只剩测试）。
    SearchResult get_result();

    // 「无锁」在这里是一个数，不是形容词（#93/#114/#210 同一用途）。
    bool result_slot_is_lock_free() const { return published_result_.is_lock_free(); }
    unsigned long long results_published() const {
        return result_publishes_.load(std::memory_order_relaxed);
    }

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

    // 发布/取用都走 `exchange`：每个节点恰好被一根线程拿到，拿到的人负责删。写者发布时把
    // 上一份没被读走的换掉并删除，所以最多一份活在槽里，析构再兜一次（轮次205 的漏账教训）。
    std::atomic<SearchResult*> published_result_{nullptr};
    std::atomic<unsigned long long> result_publishes_{0};

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

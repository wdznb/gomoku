#ifndef GOMOKU_OWNER_CHECKED_MUTEX_H
#define GOMOKU_OWNER_CHECKED_MUTEX_H

#include <atomic>
#include <cassert>
#include <thread>

namespace gomoku {

// 轮次199 #93 立的是这个形状：`games_mutex_` 守的那几张表只许一根线程碰（轮次187 那次挂死
// 就是它的反面 —— io 抱着锁去 join 一个等同一把锁的 worker）。当时做成「属主登记 + assert +
// 一把真 mutex」；轮次205-208 用两台仪器（每个获取点按线程记数、真实赛具的 419 次获取全部
// `distinct=1`）量出**那把真 mutex 从来没挡过第二根线程**，于是三处先后降级成只登记不阻塞的
// `OwnerOnlySection`（`games_mutex_`→`games_owner_`、`end_frame_mutex_`→`end_frame_owner_`、
// 提示缓存的取用点 `hint_adopter_`），带真 mutex 的那个类型随之在产品里失去使用者，
// 已于本轮删除 —— 它的全部断言语义都在 `claim_owner()` 这一半里，删锁体不删机制。
//
// 用哪一种的判据也写在机器上，不写在文档里：
//   * 只有「一个对象恒由一根线程独占触碰」被仪器量到，才允许用 `OwnerOnlySection`；
//   * 真有第二根线程会并发进入的，仍然该用一把真锁（本轮之后 `src/server/` 里没有这种了；
//     `src/` 里剩下的 `std::mutex` 见 REVIEW_LOG 轮次208 的更正段与轮次210：controller /
//     TT 分片两处，各自还欠一次判定；opening_book 那把已在轮次210 用「去掉共享状态」撤掉）。
// 边界照旧：`NDEBUG` 下不拦人 —— 把它做成生产级失败方式仍是一个待点头的取舍（#93 起就没动）。

class OwnerOnlySection {
public:
    void lock() { claim_owner(); }
    void unlock() {}

    std::thread::id owner() const { return owner_.load(std::memory_order_relaxed); }

    bool current_thread_is_owner() const {
        const std::thread::id recorded = owner();
        return !(recorded == std::thread::id{}) && recorded == std::this_thread::get_id();
    }

    bool owner_slot_is_lock_free() const { return owner_.is_lock_free(); }

private:
    void claim_owner() {
        const std::thread::id self = std::this_thread::get_id();
        std::thread::id recorded = std::thread::id{};
        if (owner_.compare_exchange_strong(recorded, self, std::memory_order_relaxed)) return;
        assert(recorded == self &&
               "这一节只许一根线程触碰（#113）：换了线程就不是「串行」，而是有人在跨线程共享它");
    }

    std::atomic<std::thread::id> owner_{};
};

}  // namespace gomoku

#endif  // GOMOKU_OWNER_CHECKED_MUTEX_H

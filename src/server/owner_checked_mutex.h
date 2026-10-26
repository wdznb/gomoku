#ifndef GOMOKU_OWNER_CHECKED_MUTEX_H
#define GOMOKU_OWNER_CHECKED_MUTEX_H

#include <atomic>
#include <cassert>
#include <mutex>
#include <thread>

namespace gomoku {

// #93 的机制。`GameServer::games_mutex_` 守着的那几张表只许一个线程碰（轮次187 那次挂死
// 就是它的反面：io 线程抱着这把锁去 join 一个正在等同一把锁的 worker）。那条规矩以前只写在
// 注释里 —— 全仓确实一处都不拿，但那是**当前树的读数**，不是约束。这个类型把它做成形状：
//
//   * 第一次 `lock()` 的线程被记进属主槽（一次原子 CAS），此后每一次 `lock()` 都对不上就
//     assert 当场崩；`unlock()` 不判（只有持有者能走到那儿）。
//   * 属主槽是一个**可读的数**，所以「这一手是不是属主线程拿的」在测试里能直接核对，
//     不需要把进程搞崩才有证据。
//   * 想绕过它只能写 `std::lock_guard<std::mutex>` 去拿一个不是 `std::mutex` 的成员 ——
//     编译就过不去，这正是「机制」与「口头约定」的分别。
//
// 刻意没做的事：一是把误用做成 release 也 abort（那等于把一次写错变成整台服务器当场死，
// 是要点头的取舍）；二是给它加条件变量或 try_lock 的等待语义 —— 它只需要是 BasicLockable。
// 属主槽必须是原子的：读侧（`owner()` / `current_thread_is_owner()`）可能在别的线程上跑，
// 而这里的原则是「不加第二把锁」。
class OwnerCheckedMutex {
public:
    void lock() {
        claim_owner();
        mu_.lock();
    }

    void unlock() { mu_.unlock(); }

    // 属主槽里的那个线程 id；从没被任何线程拿过锁时是默认构造的 id。
    std::thread::id owner() const { return owner_.load(std::memory_order_relaxed); }

    // 调用线程是不是那个属主。没登记过（属主还是默认 id）时一律为假 —— 这时还没有任何主张
    // 可以违反，第一手拿走锁的线程就是属主。
    bool current_thread_is_owner() const {
        const std::thread::id recorded = owner();
        return !(recorded == std::thread::id{}) && recorded == std::this_thread::get_id();
    }

    // 「最好无锁」在这里是可核对的，不是形容词：这一位必须是真原子。
    bool owner_slot_is_lock_free() const { return owner_.is_lock_free(); }

private:
    void claim_owner() {
        const std::thread::id self = std::this_thread::get_id();
        std::thread::id recorded = std::thread::id{};
        // 第一手：把空槽换成本线程，赢了就直接拿走锁。
        if (owner_.compare_exchange_strong(recorded, self, std::memory_order_relaxed)) return;
        assert(recorded == self &&
               "games_mutex_ 的属主只有一个线程：worker 线程不许拿它（#93）");
    }

    std::mutex mu_;
    std::atomic<std::thread::id> owner_{};
};

}  // namespace gomoku

#endif  // GOMOKU_OWNER_CHECKED_MUTEX_H

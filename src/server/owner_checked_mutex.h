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

// 轮次206 #113 的这一档连那把内部 mutex 都不留：**只登记属主，从不阻塞**。
//
// 它存在的理由是一次量出来的读数，不是一句偏好。`GameServer::end_frame_mutex_` 那四个获取点
// 挂上按线程记数的仪器后，在真实赛具（24 局、两台服务器）与整套服务端用例里读到的都是
// `distinct=1`（唯一没被走到的那一处是「玩家那一手被拒」的收回路径，也在同一根线程上）；
// asio 的 io_context 只有一根线程，所以这四个点从来都是同一根线程串行跑到，锁挡的是一件
// 不会发生的事，而每一次获取都要付一次原子 RMW。
//
// 但「今天只有一根线程」不是约束 —— 轮次199 为 `games_mutex_` 写下的正是这句话，所以这里
// 保留同一件机制：第一次进入的线程登记进属主槽，之后任何**别的**线程进来当场 assert 崩，
// 而观察面（`owner()` / `current_thread_is_owner()` / `owner_slot_is_lock_free()`）让测试
// 不必把进程搞崩也能核对。取舍与 #93 相同：`NDEBUG` 下不拦人。
//
// 它**不是**互斥量：同一根线程重复进入永远成功（这正是它和 `OwnerCheckedMutex` 的分别）。
// 只有在「同一个对象恒由一根线程独占触碰」时才能这样替换，而那句话必须由仪器拿到，不是猜。
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

#ifndef GOMOKU_OPENING_BOOK_H
#define GOMOKU_OPENING_BOOK_H

#include "../core/board.h"
#include "../core/types.h"
#include <atomic>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace gomoku {

class OpeningBook {
public:
    struct BookEntry {
        Position move;
        int weight;
        std::string name;

        BookEntry() : weight(1) {}
        BookEntry(Position p, int w, std::string n) : move(p), weight(w), name(std::move(n)) {}
    };

    // 抽签的可核对观察面（#93/#114 同一用途）：计数器必须是真原子，否则「无锁」只是形容词。
    bool draw_counter_is_lock_free() const { return draw_seq_.is_lock_free(); }
    unsigned long long draws_taken() const {
        return static_cast<unsigned long long>(draw_seq_.load(std::memory_order_relaxed));
    }

    // A full game line; every prefix maps to its following move as a book entry.
    void add_line(const std::vector<Position>& line, int weight, const std::string& name);

    bool load(const std::string& path, std::string& error);
    bool load_or_default(const std::string& path);

    void clear();

    bool query(const Board& board, int color, Move& out_move) const;
    bool query(const std::vector<Move>& history, int color, Move& out_move) const;

    bool has_entry(const std::vector<Move>& history) const;

    size_t entry_count() const { return entries_.size(); }
    size_t line_count() const { return lines_.size(); }
    bool empty() const { return entries_.empty(); }

    // Maximum ply count the book covers; searches take over beyond this.
    int max_depth() const { return max_depth_; }

    static constexpr int SYMMETRY_VARIANTS = 8;
    static Position apply_transform(Position p, int variant);

private:
    std::unordered_map<std::string, std::vector<BookEntry>> entries_;
    std::vector<std::vector<Position>> lines_;
    int max_depth_ = 0;

    // Seeded once per book: the default-constructed engine replays the same
    // sequence in every process, which made the first weighted draw - the common
    // "play one game on a fresh server" case - fully deterministic.
    // 轮次210 #113：抽签原来是「一个共享的 `std::mt19937` + 一把 mutex」。这把锁和
    // `src/server/` 那几把不同 —— 它**真的**被多根线程拿：book 只在服务端用（训练、复盘、
    // 预搜都 `set_book_enabled(false)`），而查询发生在每一手思考自己的 worker 线程里。
    // 所以撤它不能靠「反正只有一根线程」，只能把**共享状态本身去掉**：
    //   * `rng_seed_` 在构造时取一次 `random_device`（注意：抽签从来就没被 `--seed` 钉过，
    //     本轮 grep 过 `rng_` 没有任何再播种的路径 ⇒ 换算法不破坏任何可重复性，因为本来就没有）；
    //   * 每次抽签从原子计数器拿一个序号，喂进 splitmix64 ⇒ 一次 relaxed fetch_add，无锁、无共享可变状态；
    //   * 取模的偏差在这个量级（权重合计远小于 2^32）是 2^-32 级，写在下面代码旁。
    uint64_t rng_seed_ = (static_cast<uint64_t>(std::random_device{}()) << 32) |
                           static_cast<uint64_t>(std::random_device{}());
    mutable std::atomic<uint64_t> draw_seq_{0};

    static std::string make_key(const std::vector<Position>& moves);
    static bool covers_position(const std::vector<Move>& history, Position pos);
};

}

#endif

#ifndef GOMOKU_TRANSPOSITION_H
#define GOMOKU_TRANSPOSITION_H

#include "../core/types.h"
#include <vector>
#include <cstdint>

namespace gomoku {

enum class TTFlag : uint8_t {
    NONE = 0,
    EXACT,
    LOWERBOUND,
    UPPERBOUND
};

struct TTEntry {
    uint64_t hash;
    int depth;
    int value;
    Move best_move;
    TTFlag flag;
    uint8_t age;

    TTEntry() : hash(0), depth(0), value(0), flag(TTFlag::NONE), age(0) {}
};

class TranspositionTable {
public:
    // 2^18 x 32B ~= 8.4MB per table. Every GameManager owns an engine and its
    // table, and sessions are unbounded: the old 2^20 default blew the <100MB
    // memory target with three concurrent games.
    explicit TranspositionTable(size_t num_entries = 1 << 18);

    void store(uint64_t hash, int depth, int value, Move best_move, TTFlag flag);
    const TTEntry* lookup(uint64_t hash) const;

    void new_search();
    void clear();

    size_t size() const { return entries_.size(); }
    int hashfull() const;

private:
    std::vector<TTEntry> entries_;
    uint8_t current_age_;

    size_t index_for(uint64_t hash) const {
        return static_cast<size_t>(hash % entries_.size());
    }
};

}

#endif

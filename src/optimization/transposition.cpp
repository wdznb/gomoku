#include "transposition.h"

namespace gomoku {

TranspositionTable::TranspositionTable(size_t num_entries)
    : entries_(num_entries), current_age_(0) {}

void TranspositionTable::store(uint64_t hash, int depth, int value,
                                Move best_move, TTFlag flag) {
    size_t idx = index_for(hash);
    TTEntry& entry = entries_[idx];

    bool replace = (entry.flag == TTFlag::NONE)
                || (entry.age != current_age_)
                || (depth >= entry.depth);

    if (!replace) return;

    entry.hash = hash;
    entry.depth = depth;
    entry.value = value;
    entry.best_move = best_move;
    entry.flag = flag;
    entry.age = current_age_;
}

const TTEntry* TranspositionTable::lookup(uint64_t hash) const {
    size_t idx = index_for(hash);
    const TTEntry& entry = entries_[idx];

    if (entry.hash == hash && entry.flag != TTFlag::NONE) {
        return &entry;
    }
    return nullptr;
}

void TranspositionTable::new_search() {
    current_age_++;
}

void TranspositionTable::clear() {
    for (auto& e : entries_) {
        e = TTEntry{};
    }
    current_age_ = 0;
}

int TranspositionTable::hashfull() const {
    int count = 0;
    int sample = std::min(static_cast<size_t>(1000), entries_.size());
    for (int i = 0; i < sample; ++i) {
        if (entries_[i].flag != TTFlag::NONE && entries_[i].age == current_age_) {
            count++;
        }
    }
    return count * 1000 / sample;
}

}

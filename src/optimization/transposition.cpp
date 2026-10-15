#include "transposition.h"
#include <algorithm>

namespace gomoku {

namespace {

size_t bucket_bits_for(size_t num_entries) {
    // The index is a shift, so the bucket count is a power of two and the shift is
    // never the undefined 64-bit one. A request that is not a multiple of the way
    // count rounds up, so size() of a tiny table is a little more than asked for.
    size_t buckets = (num_entries + TranspositionTable::kBucketWays - 1) /
                     TranspositionTable::kBucketWays;
    size_t bits = 1;
    while ((static_cast<size_t>(1) << bits) < buckets) ++bits;
    return bits;
}

}

TranspositionTable::TranspositionTable(size_t num_entries)
    : bucket_bits_(bucket_bits_for(num_entries)), generation_(0) {
    buckets_.resize(static_cast<size_t>(1) << bucket_bits_);
}

void TranspositionTable::store(uint64_t hash, int depth, int value, Move best_move,
                               TTFlag flag, int ply, int32_t static_eval) {
    if (flag == TTFlag::NONE) return;

    Bucket& bucket = buckets_[index_for(hash)];
    const uint32_t key = key_for(hash);

    uint8_t victim = 0;
    bool own_entry = false;
    bool empty_found = false;
    for (uint8_t i = 0; i < kBucketWays; ++i) {
        TTEntry& slot = bucket.slots[i];
        if (slot.flag == TTFlag::NONE) {
            if (!empty_found) {
                empty_found = true;
                victim = i;
            }
            continue;
        }
        if (slot.key != key) continue;

        // The same position again. A deeper record of it wins; a shallower one is
        // the same think meeting itself through a transposition and says nothing
        // new. This is the order the design doc asks for - depth first, age only as
        // the tie-break - because the old policy let a fresh shallow record evict a
        // deep one and threw away the work a previous move had paid for.
        own_entry = true;
        victim = i;
        if (depth < slot.depth) return;
        break;
    }

    if (!own_entry && !empty_found) {
        // Full bucket of other positions: the shallowest record goes, and among
        // equals the one from the oldest generation.
        for (uint8_t i = 1; i < kBucketWays; ++i) {
            const TTEntry& candidate = bucket.slots[i];
            const TTEntry& current = bucket.slots[victim];
            if (candidate.depth < current.depth ||
                (candidate.depth == current.depth &&
                 age_older(candidate.age, current.age))) {
                victim = i;
            }
        }
    }

    TTEntry& slot = bucket.slots[victim];
    slot.key = key;
    slot.value = store_value(value, ply);
    slot.static_eval = static_eval;
    slot.depth = static_cast<int16_t>(depth);
    slot.flag = flag;
    slot.age = generation_;
    slot.best_move = best_move;
}

bool TranspositionTable::probe(uint64_t hash, TTEntry& out, int ply) const {
    const Bucket& bucket = buckets_[index_for(hash)];
    const uint32_t key = key_for(hash);

    for (uint8_t i = 0; i < kBucketWays; ++i) {
        const TTEntry& slot = bucket.slots[i];
        if (slot.flag == TTFlag::NONE || slot.key != key) continue;
        out = slot;
        out.value = probe_value(out.value, ply);
        return true;
    }
    return false;
}

void TranspositionTable::advance_generation() {
    ++generation_;
}

void TranspositionTable::clear() {
    for (auto& bucket : buckets_) {
        for (auto& slot : bucket.slots) {
            slot = TTEntry{};
        }
    }
    generation_ = 0;
}

int TranspositionTable::hashfull() const {
    // Per mille of the sampled table holding this generation's records, so a caller
    // can see the table filling up without walking every bucket.
    const size_t sample = std::min<size_t>(100, buckets_.size());
    int count = 0;
    for (size_t i = 0; i < sample; ++i) {
        const Bucket& bucket = buckets_[i];
        for (uint8_t j = 0; j < kBucketWays; ++j) {
            if (bucket.slots[j].flag != TTFlag::NONE && bucket.slots[j].age == generation_) {
                ++count;
            }
        }
    }
    return count * 1000 / static_cast<int>(sample * kBucketWays);
}

}

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
    // A record does not lose the number it already holds just because this writer
    // has none of its own: NO_STATIC_EVAL means "I did not evaluate", not "forget
    // what was evaluated here". Only a slot that is about to describe the same
    // position can keep it - the shallowest victim of a full bucket belongs to
    // another position, and its score goes with it.
    if (static_eval == NO_STATIC_EVAL && slot.key == key) {
        static_eval = slot.static_eval;
    }
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

int32_t TranspositionTable::static_eval_of(uint64_t hash) const {
    const Bucket& bucket = buckets_[index_for(hash)];
    const uint32_t key = key_for(hash);

    int eval_only = -1;
    for (uint8_t i = 0; i < kBucketWays; ++i) {
        const TTEntry& slot = bucket.slots[i];
        if (slot.key != key) continue;
        if (slot.flag != TTFlag::NONE) return slot.static_eval;
        // An untouched empty slot has the default key 0, which a position whose tag
        // happens to be 0 matches; its depth says it holds nothing, and it is skipped.
        if (slot.depth == kEvalOnlyDepth && eval_only < 0) eval_only = i;
    }
    if (eval_only < 0) return NO_STATIC_EVAL;
    return bucket.slots[eval_only].static_eval;
}

bool TranspositionTable::put_static_eval(uint64_t hash, int32_t static_eval) {
    Bucket& bucket = buckets_[index_for(hash)];
    const uint32_t key = key_for(hash);

    int spare = -1;
    for (uint8_t i = 0; i < kBucketWays; ++i) {
        TTEntry& slot = bucket.slots[i];
        if (slot.flag != TTFlag::NONE) {
            if (slot.key != key) continue;
            slot.static_eval = static_eval;
            return true;
        }
        // Prefer a slot that has never held anything over one that already holds
        // another position's score, so a bucket caches as many evaluations as it has
        // ways instead of recycling one. Neither choice moves a bound record: both
        // slots have flag NONE, which is all store() looks at.
        if (spare < 0 || (slot.depth != kEvalOnlyDepth &&
                          bucket.slots[spare].depth == kEvalOnlyDepth)) {
            spare = i;
        }
    }
    if (spare < 0) return false;

    TTEntry& slot = bucket.slots[spare];
    slot = TTEntry{};
    slot.key = key;
    slot.static_eval = static_eval;
    slot.depth = kEvalOnlyDepth;
    return true;
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

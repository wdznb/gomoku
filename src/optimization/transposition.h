#ifndef GOMOKU_TRANSPOSITION_H
#define GOMOKU_TRANSPOSITION_H

#include "../core/types.h"
#include <array>
#include <cstdint>
#include <vector>

namespace gomoku {

enum class TTFlag : uint8_t {
    NONE = 0,
    EXACT,
    LOWERBOUND,
    UPPERBOUND
};

// "this entry carries no static evaluation". INT32_MIN is not a score the
// evaluator can produce: the aggregate is clamped to +-99999.
constexpr int32_t NO_STATIC_EVAL = INT32_MIN;

// A record of one position, kept in a four-way bucket with three of its siblings.
// The table no longer stores the 64-bit hash: a bucket is addressed by a bit slice
// of a mixed hash, so the full key cannot be compared there, and a 32-bit tag of
// the hash's own high half makes a false hit a one-in-four-billion event. The doc's
// 24-byte entry with an int16 value is not reachable - the legal score range runs to
// +-99999 and the engine's mate sentinel is +-100000 - so the value stays 32-bit
// and the entry 28 bytes, which is still smaller than the 32 it replaces.
struct TTEntry {
    uint32_t key;
    int32_t value;
    // Filled in by the search that first evaluates the position (design doc 2.6
    // step 1); until then it carries NO_STATIC_EVAL and readers must ignore it.
    int32_t static_eval;
    int16_t depth;
    TTFlag flag;
    uint8_t age;
    Move best_move;

    TTEntry() : key(0), value(0), static_eval(NO_STATIC_EVAL), depth(0),
                flag(TTFlag::NONE), age(0) {}
};

class TranspositionTable {
public:
    // 2^18 x 28B ~= 7.3MB per table, down from 8.4MB. Every GameManager owns an
    // engine and its table, and sessions are unbounded: the old 2^20 default blew
    // the <100MB memory target with three concurrent games.
    explicit TranspositionTable(size_t num_entries = 1 << 18);

    // `ply` is the distance from the root of the think that wrote the record: mate
    // scores are stored relative to it and read back relative to the probing node's
    // own ply, so an entry found at the edge of one iteration still says the right
    // thing when a later iteration reaches the same position from a different depth.
    void store(uint64_t hash, int depth, int value, Move best_move, TTFlag flag,
               int ply = 0, int32_t static_eval = NO_STATIC_EVAL);

    // Copies the record out and restores `value` for the probing node's ply. The
    // entry is not handed back as a pointer: a store into the same bucket can
    // overwrite the slot a caller is still reading.
    bool probe(uint64_t hash, TTEntry& out, int ply = 0) const;

    // One step of the clock the replacement policy reads. It advances when a stone
    // is committed to a board, not when a think starts: ageing the table at the start
    // of a think is what let that think's own shallow records evict the deep ones the
    // previous move had paid for.
    void advance_generation();

    void clear();

    size_t size() const { return buckets_.size() * kBucketWays; }
    uint8_t generation() const { return generation_; }
    int hashfull() const;

    static constexpr uint8_t kBucketWays = 4;

private:
    // +-100000 is the engine's mate sentinel, the same number the resign threshold
    // and README's proven-loss rule read out of a SearchResult.
    static constexpr int32_t kMateThreshold = 100000;

    struct Bucket {
        std::array<TTEntry, kBucketWays> slots;
    };

    size_t index_for(uint64_t hash) const {
        // High bits of a mixed product: the low bits of a Zobrist hash are what the
        // board's own toggles change most, and a direct modulo put neighbouring
        // generations of positions in the same bucket.
        return static_cast<size_t>((hash * 0x9E3779B97F4A7C15ULL) >> (64 - bucket_bits_));
    }
    // The verification tag is the hash's own high half. bucket_bits_ is at least 1,
    // so the shift above is never the undefined 64-bit one.
    static uint32_t key_for(uint64_t hash) {
        return static_cast<uint32_t>(hash >> 32);
    }
    static int32_t store_value(int32_t value, int ply) {
        if (value >= kMateThreshold) return value + ply;
        if (value <= -kMateThreshold) return value - ply;
        return value;
    }
    static int32_t probe_value(int32_t value, int ply) {
        // Saturating, not exact: undoing the correction must never be able to drop a
        // mate record back under the +-100000 line. A record stored two plies in and
        // read twenty plies down would otherwise restore as 99982 and stop counting
        // as a proven result, and that line is what the mate cutoff, the resign rule
        // and the replay panel's red dot all read.
        if (value >= kMateThreshold) {
            const int32_t restored = value - ply;
            return restored > kMateThreshold ? restored : kMateThreshold;
        }
        if (value <= -kMateThreshold) {
            const int32_t restored = value + ply;
            return restored < -kMateThreshold ? restored : -kMateThreshold;
        }
        return value;
    }
    // Age is a uint8_t ring; the entry furthest behind the current generation is the
    // victim, and a raw comparison gets that backwards once the counter has wrapped.
    static bool age_older(uint8_t a, uint8_t b) {
        return static_cast<int8_t>(a - b) < 0;
    }

    std::vector<Bucket> buckets_;
    // Set by every constructor through bucket_bits_for(), which never returns 0.
    size_t bucket_bits_;
    uint8_t generation_;
};

}

#endif

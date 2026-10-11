#ifndef GOMOKU_TRAINING_JSON_INT_H
#define GOMOKU_TRAINING_JSON_INT_H

#include <nlohmann/json.hpp>
#include <cstdint>
#include <limits>

namespace gomoku {

// JSON keeps integers in 64 bits and get<int> only casts, so a file holding
// 4294967299 reads back as 3: a legal-looking value the writer never meant.
// Anything that decides a position or a parameter must be range-checked instead
// of truncated. Informational counters (search score, node counts, ratings) may
// fall back to a default, since no reader trusts their exact magnitude.
inline bool read_json_int(const nlohmann::json& node, int& out) {
    if (!node.is_number_integer()) return false;
    if (node.is_number_unsigned()) {
        const uint64_t wide = node.get<uint64_t>();
        if (wide > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
        out = static_cast<int>(wide);
        return true;
    }
    const int64_t wide = node.get<int64_t>();
    if (wide < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
        wide > static_cast<int64_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    out = static_cast<int>(wide);
    return true;
}

// Same reasoning for the 64-bit counters (random seed, sample totals): a value
// that does not fit, or that is negative, is refused rather than reinterpreted.
inline bool read_json_uint(const nlohmann::json& node, uint64_t& out) {
    if (!node.is_number_integer()) return false;
    if (node.is_number_unsigned()) {
        out = node.get<uint64_t>();
        return true;
    }
    const int64_t wide = node.get<int64_t>();
    if (wide < 0) return false;
    out = static_cast<uint64_t>(wide);
    return true;
}

}

#endif

#include "opening_book.h"
#include "../core/pattern.h"
#include "../training/json_int.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>

namespace gomoku {

namespace {

constexpr int VARIANT_COUNT = 8;

}

Position OpeningBook::apply_transform(Position p, int variant) {
    constexpr int n = BOARD_SIZE;
    int i = p.row;
    int j = p.col;

    if (variant & 4) std::swap(i, j);
    if (variant & 1) i = n - 1 - i;
    if (variant & 2) j = n - 1 - j;

    return Position(i, j);
}

std::string OpeningBook::make_key(const std::vector<Position>& prefix) {
    std::string key;
    key.reserve(prefix.size() * 4);
    for (const auto& p : prefix) {
        if (!key.empty()) key += ',';
        key += std::to_string(p.to_index());
    }
    return key;
}

bool OpeningBook::covers_position(const std::vector<Move>& history, Position pos) {
    for (const auto& m : history) {
        if (m.pos == pos) return true;
    }
    return false;
}

void OpeningBook::add_line(const std::vector<Position>& line, int weight,
                           const std::string& name) {
    if (line.size() < 2) return;

    for (const auto& pos : line) {
        if (!pos.is_valid()) return;
    }

    lines_.push_back(line);
    max_depth_ = std::max(max_depth_, static_cast<int>(line.size()) - 1);

    for (int variant = 0; variant < VARIANT_COUNT; ++variant) {
        std::vector<Position> transformed;
        transformed.reserve(line.size());
        for (const auto& pos : line) {
            transformed.push_back(apply_transform(pos, variant));
        }

        std::vector<Position> prefix;
        prefix.reserve(transformed.size());
        for (size_t ply = 1; ply < transformed.size(); ++ply) {
            prefix.push_back(transformed[ply - 1]);
            entries_[make_key(prefix)].emplace_back(transformed[ply], weight, name);
        }
    }
}

bool OpeningBook::load(const std::string& path, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "cannot open file: " + path;
        return false;
    }

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(file);
    } catch (const nlohmann::json::exception& e) {
        error = std::string("json parse error: ") + e.what();
        return false;
    }

    if (!root.contains("lines") || !root["lines"].is_array()) {
        error = "missing 'lines' array";
        return false;
    }

    const nlohmann::json& lines = root["lines"];

    for (const auto& entry : lines) {
        if (!entry.contains("moves") || !entry["moves"].is_array()) continue;

        std::vector<Position> line;
        bool valid = true;
        for (const auto& m : entry["moves"]) {
            if (!m.is_array() || m.size() != 2) {
                valid = false;
                break;
            }
            int row = 0;
            int col = 0;
            if (!read_json_int(m[0], row) || !read_json_int(m[1], col)) {
                valid = false;
                break;
            }
            line.push_back(Position(row, col));
        }

        if (!valid || line.size() < 2) continue;

        // A weight that does not fit an int is refused with its line: both the
        // truncated value and a silent fall-back to the default would move the
        // entry inside the sampling distribution the author never meant.
        int weight = 1;
        if (entry.contains("weight") && !read_json_int(entry["weight"], weight)) continue;

        // Clamp the per-entry weight: a malformed book with a huge weight would
        // otherwise overflow the int total_weight sum in query() into UB.
        if (weight < 1) weight = 1;
        if (weight > 1000000) weight = 1000000;
        std::string name = entry.contains("name") && entry["name"].is_string()
                               ? entry["name"].get<std::string>()
                               : "";

        add_line(line, weight, name);
    }

    if (lines_.empty()) {
        error = "no valid lines in book";
        return false;
    }

    return true;
}

bool OpeningBook::load_or_default(const std::string& path) {
    std::string error;
    if (load(path, error)) return true;

    add_line({{7, 7}, {7, 8}, {8, 8}}, 10, "hua_yue");
    add_line({{7, 7}, {7, 8}, {6, 8}}, 10, "pu_yue");
    add_line({{7, 7}, {8, 8}, {6, 8}}, 8, "shu_xing");
    return false;
}

void OpeningBook::clear() {
    entries_.clear();
    lines_.clear();
    max_depth_ = 0;
}

bool OpeningBook::has_entry(const std::vector<Move>& history) const {
    std::vector<Position> prefix;
    prefix.reserve(history.size());
    for (const auto& m : history) {
        prefix.push_back(m.pos);
    }
    return entries_.find(make_key(prefix)) != entries_.end();
}

bool OpeningBook::query(const std::vector<Move>& history, int color, Move& out_move) const {
    // The book is keyed by move sequence only; each stored slot implicitly
    // alternates colors because Renju starts with BLACK. If the caller's
    // (color, history.size()) pair disagrees with that alternation, we are
    // being asked for the wrong player's reply and any returned slot would
    // be stamped with the wrong color. Reject early rather than return a
    // mismatched Move.
    int expected = (history.size() % 2 == 0) ? BLACK : WHITE;
    if (color != expected) return false;

    // The key is only the sequence of landing squares, so a board whose stones
    // were placed out of alternation (a hand-set position) can share a book
    // prefix while the two colours play each other's roles. A book line belongs
    // to the game that opened BLACK-WHITE-BLACK..., so reject any history whose
    // per-stone colours disagree with that.
    for (size_t i = 0; i < history.size(); ++i) {
        const int stone_color = (i % 2 == 0) ? BLACK : WHITE;
        if (history[i].color != stone_color) return false;
    }

    std::vector<Position> prefix;
    prefix.reserve(history.size());
    for (const auto& m : history) {
        prefix.push_back(m.pos);
    }

    auto it = entries_.find(make_key(prefix));
    if (it == entries_.end() || it->second.empty()) return false;

    std::vector<BookEntry> candidates;
    candidates.reserve(it->second.size());
    for (const auto& entry : it->second) {
        if (!entry.move.is_valid()) continue;
        if (covers_position(history, entry.move)) continue;
        candidates.push_back(entry);
    }

    if (candidates.empty()) return false;

    // Accumulate in 64-bit so even a pathological number of entries sharing a
    // key cannot overflow the total the distribution is built from.
    long long total_weight = 0;
    for (const auto& c : candidates) {
        total_weight += std::max(1, c.weight);
    }
    if (total_weight <= 0) return false;

    // 一次 relaxed 递增喂给 splitmix64 —— 没有共享可变状态，所以没有锁。
    // 取模偏差：total_weight 远小于 2^32（权重是按手累加的 int），偏差在 2^-32 级。
    const uint64_t n = draw_seq_.fetch_add(1, std::memory_order_relaxed);
    uint64_t x = rng_seed_ + (n + 1) * 0x9e3779b97f4a7c15ull;
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    x = x ^ (x >> 31);
    long long pick = static_cast<long long>(x % static_cast<uint64_t>(total_weight));

    const BookEntry* chosen = &candidates.back();
    for (const auto& c : candidates) {
        pick -= std::max(1, c.weight);
        if (pick < 0) {
            chosen = &c;
            break;
        }
    }

    out_move = Move(chosen->move, color);
    return true;
}

bool OpeningBook::query(const Board& board, int color, Move& out_move) const {
    if (!query(board.move_history(), color, out_move)) return false;
    if (!board.is_empty(out_move.pos.row, out_move.pos.col)) return false;
    if (color == BLACK &&
        PatternRecognizer::is_forbidden(board, out_move.pos.row, out_move.pos.col, BLACK)) {
        return false;
    }
    return true;
}

}

#ifndef GOMOKU_OPENING_BOOK_H
#define GOMOKU_OPENING_BOOK_H

#include "../core/board.h"
#include "../core/types.h"
#include <cstdint>
#include <mutex>
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

    mutable std::mt19937 rng_;
    mutable std::mutex rng_mutex_;

    static std::string make_key(const std::vector<Position>& moves);
    static bool covers_position(const std::vector<Move>& history, Position pos);
};

}

#endif

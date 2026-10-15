#ifndef GOMOKU_BOARD_H
#define GOMOKU_BOARD_H

#include "types.h"
#include <vector>
#include <cstdint>
#include <random>

namespace gomoku {

// Bitboard line geometry, shared by Board (which maintains the words) and
// PatternRecognizer (which reads them). Every pattern direction is a family of
// parallel lines; one line holds its stones in a single 64-bit word, with the
// stone at distance `pos` from the line's origin at bit pos + LINE_MARGIN.
// Eight margin bits put the 15 real cells at bits 8..22, so a 9-cell window
// always shifts out of one word and the unused bits below the first cell and
// above the last stay clear: cells off the board are recovered from a
// precomputed wall mask instead of a bounds check per window cell.
constexpr int LINE_MARGIN = 8;
constexpr int MAX_LINE_INDEX = 2 * BOARD_SIZE - 1;   // enough for the two diagonal families

inline constexpr int line_index(int dir, int row, int col) {
    switch (dir) {
        case 0: return row;                                     // rows:      (0, 1)
        case 1: return col;                                     // columns:   (1, 0)
        case 2: return row - col + BOARD_SIZE - 1;              // diagonals: (1, 1)
        default: return row + col;                              // anti-diagonals: (1, -1)
    }
}

inline constexpr int line_position(int dir, int row, int col) {
    return dir == 0 ? col : row;
}

class ZobristHash {
public:
    static void init();
    static uint64_t compute_hash(const std::array<int, BOARD_SIZE * BOARD_SIZE>& cells);
    static uint64_t update_hash(uint64_t hash, int pos, int color);
    
private:
    static std::array<std::array<uint64_t, 3>, BOARD_SIZE * BOARD_SIZE> random_table_;
};

class Board {
public:
    Board();
    
    void reset();
    bool place_stone(int row, int col, int color);
    bool place_stone(Position pos, int color);
    void undo_last_move();
    
    int get(int row, int col) const;
    int get(Position pos) const;
    bool is_empty(int row, int col) const;
    bool is_empty(Position pos) const;
    bool is_valid(int row, int col) const;
    bool is_valid(Position pos) const;
    
    int move_count() const { return move_count_; }
    const Move& last_move() const;
    const std::vector<Move>& move_history() const { return moves_; }
    
    int check_winner() const;
    bool is_game_over() const;
    bool has_five_in_a_row(int row, int col, int color) const;
    
    uint64_t zobrist_hash() const { return zobrist_hash_; }

    // Stones of `color` on line `line` of direction family `dir`, as a bitboard.
    uint64_t line_bits(int color, int dir, int line) const {
        return lines_[color][dir][line];
    }

    // Length of the `color` run that passes through (row, col) along `dir`,
    // counting the centre cell itself. Cells of another colour or off the board
    // cut the run short, exactly as the old cell-by-cell walk did.
    int run_length(int dir, int row, int col, int color) const;
    
    const std::vector<Position>& active_empty() const { return active_empty_; }
    void update_active_empty(int row, int col);
    
    std::vector<Position> get_candidate_moves() const;
    
private:
    std::array<int, BOARD_SIZE * BOARD_SIZE> cells_;
    std::vector<Move> moves_;
    int move_count_;
    uint64_t zobrist_hash_;

    // [color][direction family][line] bitboard view of cells_, updated with
    // each place/undo so pattern recognition never walks the board cell by
    // cell. Index 0 (EMPTY) stays zero.
    std::array<std::array<std::array<uint64_t, MAX_LINE_INDEX>, 4>, 3> lines_;

    void set_stone_bits(int row, int col, int color, bool present);
    
    std::array<bool, BOARD_SIZE * BOARD_SIZE> has_neighbor_;
    std::vector<Position> active_empty_;
    // Incremental bookkeeping for the active_empty_ set. neighbor_count_[i]
    // counts non-empty stones within Chebyshev distance 2 of cell i (never
    // including i itself); active_index_[i] is the position of cell i inside
    // active_empty_, or -1 when absent. Together they let undo_last_move
    // restore the candidate list in O(25) via swap-remove instead of a full
    // 225-cell rebuild at every ply.
    std::array<uint8_t, BOARD_SIZE * BOARD_SIZE> neighbor_count_;
    std::array<int16_t, BOARD_SIZE * BOARD_SIZE> active_index_;
    
    void rebuild_active_empty();
    void activate(int index, int row, int col);
    void deactivate(int index);
};

}

#endif

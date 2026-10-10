#ifndef GOMOKU_BOARD_H
#define GOMOKU_BOARD_H

#include "types.h"
#include <vector>
#include <cstdint>
#include <random>

namespace gomoku {

class ZobristHash {
public:
    static void init();
    static uint64_t compute_hash(const std::array<int, BOARD_SIZE * BOARD_SIZE>& cells);
    static uint64_t update_hash(uint64_t hash, int pos, int color);
    
private:
    static std::array<std::array<uint64_t, 3>, BOARD_SIZE * BOARD_SIZE> random_table_;
    static bool initialized_;
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
    
    const std::vector<Position>& active_empty() const { return active_empty_; }
    void update_active_empty(int row, int col);
    
    std::vector<Position> get_candidate_moves() const;
    
private:
    std::array<int, BOARD_SIZE * BOARD_SIZE> cells_;
    std::vector<Move> moves_;
    int move_count_;
    uint64_t zobrist_hash_;
    
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
    
    int count_direction(int row, int col, int dr, int dc, int color) const;
    void rebuild_active_empty();
    void activate(int index, int row, int col);
    void deactivate(int index);
};

}

#endif

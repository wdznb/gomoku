#include "board.h"
#include <algorithm>
#include <cstring>

namespace gomoku {

std::array<std::array<uint64_t, 3>, BOARD_SIZE * BOARD_SIZE> ZobristHash::random_table_;
bool ZobristHash::initialized_ = false;

void ZobristHash::init() {
    if (initialized_) return;
    
    std::mt19937_64 rng(12345);
    for (int i = 0; i < BOARD_SIZE * BOARD_SIZE; ++i) {
        for (int c = 0; c < 3; ++c) {
            random_table_[i][c] = rng();
        }
    }
    initialized_ = true;
}

uint64_t ZobristHash::compute_hash(const std::array<int, BOARD_SIZE * BOARD_SIZE>& cells) {
    uint64_t hash = 0;
    for (int i = 0; i < BOARD_SIZE * BOARD_SIZE; ++i) {
        if (cells[i] != EMPTY) {
            hash ^= random_table_[i][cells[i]];
        }
    }
    return hash;
}

uint64_t ZobristHash::update_hash(uint64_t hash, int pos, int color) {
    return hash ^ random_table_[pos][color];
}

Board::Board() {
    ZobristHash::init();
    reset();
}

void Board::reset() {
    cells_.fill(EMPTY);
    moves_.clear();
    move_count_ = 0;
    zobrist_hash_ = 0;
    has_neighbor_.fill(false);
    active_empty_.clear();
    rebuild_active_empty();
}

bool Board::place_stone(int row, int col, int color) {
    if (!is_valid(row, col) || !is_empty(row, col)) {
        return false;
    }
    
    int index = row * BOARD_SIZE + col;
    cells_[index] = color;
    zobrist_hash_ = ZobristHash::update_hash(zobrist_hash_, index, color);
    
    moves_.push_back(Move(row, col, color));
    move_count_++;
    
    update_active_empty(row, col);
    
    return true;
}

bool Board::place_stone(Position pos, int color) {
    return place_stone(pos.row, pos.col, color);
}

void Board::remove_stone(int row, int col) {
    if (!is_valid(row, col) || is_empty(row, col)) {
        return;
    }
    
    int index = row * BOARD_SIZE + col;
    int color = cells_[index];
    cells_[index] = EMPTY;
    zobrist_hash_ = ZobristHash::update_hash(zobrist_hash_, index, color);
}

void Board::remove_stone(Position pos) {
    remove_stone(pos.row, pos.col);
}

void Board::undo_last_move() {
    if (move_count_ == 0) return;
    
    const Move& last = moves_.back();
    remove_stone(last.pos);
    moves_.pop_back();
    move_count_--;
    
    rebuild_active_empty();
}

int Board::get(int row, int col) const {
    if (!is_valid(row, col)) return -1;
    return cells_[row * BOARD_SIZE + col];
}

int Board::get(Position pos) const {
    return get(pos.row, pos.col);
}

bool Board::is_empty(int row, int col) const {
    if (!is_valid(row, col)) return false;
    return cells_[row * BOARD_SIZE + col] == EMPTY;
}

bool Board::is_empty(Position pos) const {
    return is_empty(pos.row, pos.col);
}

bool Board::is_valid(int row, int col) const {
    return row >= 0 && row < BOARD_SIZE && col >= 0 && col < BOARD_SIZE;
}

bool Board::is_valid(Position pos) const {
    return is_valid(pos.row, pos.col);
}

const Move& Board::last_move() const {
    static Move invalid;
    if (moves_.empty()) return invalid;
    return moves_.back();
}

int Board::check_winner() const {
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            int color = cells_[r * BOARD_SIZE + c];
            if (color != EMPTY && has_five_in_a_row(r, c, color)) {
                return color;
            }
        }
    }
    return EMPTY;
}

bool Board::is_game_over() const {
    if (move_count_ >= MAX_MOVES) return true;
    return check_winner() != EMPTY;
}

bool Board::has_five_in_a_row(int row, int col, int color) const {
    for (int d = 0; d < 4; ++d) {
        int dr = DIRECTIONS[d][0];
        int dc = DIRECTIONS[d][1];
        
        int count = 1;
        count += count_direction(row, col, dr, dc, color);
        count += count_direction(row, col, -dr, -dc, color);
        
        if (count >= 5) return true;
    }
    return false;
}

int Board::count_direction(int row, int col, int dr, int dc, int color) const {
    int count = 0;
    int r = row + dr;
    int c = col + dc;
    
    while (is_valid(r, c) && cells_[r * BOARD_SIZE + c] == color) {
        count++;
        r += dr;
        c += dc;
    }
    
    return count;
}

void Board::update_active_empty(int row, int col) {
    Position new_pos(row, col);
    
    auto it = std::find(active_empty_.begin(), active_empty_.end(), new_pos);
    if (it != active_empty_.end()) {
        active_empty_.erase(it);
    }
    
    int neighbor_range = 2;
    for (int dr = -neighbor_range; dr <= neighbor_range; ++dr) {
        for (int dc = -neighbor_range; dc <= neighbor_range; ++dc) {
            int nr = row + dr;
            int nc = col + dc;
            
            if (!is_valid(nr, nc)) continue;
            
            int nindex = nr * BOARD_SIZE + nc;
            if (cells_[nindex] != EMPTY) continue;
            
            if (!has_neighbor_[nindex]) {
                has_neighbor_[nindex] = true;
                active_empty_.push_back(Position(nr, nc));
            }
        }
    }
}

void Board::rebuild_active_empty() {
    has_neighbor_.fill(false);
    active_empty_.clear();
    
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            int index = r * BOARD_SIZE + c;
            if (cells_[index] != EMPTY) {
                int neighbor_range = 2;
                for (int dr = -neighbor_range; dr <= neighbor_range; ++dr) {
                    for (int dc = -neighbor_range; dc <= neighbor_range; ++dc) {
                        int nr = r + dr;
                        int nc = c + dc;
                        
                        if (!is_valid(nr, nc)) continue;
                        
                        int nindex = nr * BOARD_SIZE + nc;
                        if (cells_[nindex] != EMPTY) continue;
                        
                        if (!has_neighbor_[nindex]) {
                            has_neighbor_[nindex] = true;
                            active_empty_.push_back(Position(nr, nc));
                        }
                    }
                }
            }
        }
    }
    
    if (move_count_ == 0) {
        for (int r = 0; r < BOARD_SIZE; ++r) {
            for (int c = 0; c < BOARD_SIZE; ++c) {
                active_empty_.push_back(Position(r, c));
            }
        }
    }
}

std::vector<Position> Board::get_candidate_moves() const {
    return active_empty_;
}

}

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
    lines_ = {};
    moves_.clear();
    move_count_ = 0;
    zobrist_hash_ = 0;
    has_neighbor_.fill(false);
    neighbor_count_.fill(0);
    active_index_.fill(-1);
    active_empty_.clear();
    rebuild_active_empty();
}

bool Board::place_stone(int row, int col, int color) {
    if (!is_valid(row, col) || !is_empty(row, col)) {
        return false;
    }
    
    int index = row * BOARD_SIZE + col;
    cells_[index] = color;
    set_stone_bits(row, col, color, true);
    zobrist_hash_ = ZobristHash::update_hash(zobrist_hash_, index, color);
    
    moves_.push_back(Move(row, col, color));
    move_count_++;
    
    update_active_empty(row, col);
    
    return true;
}

bool Board::place_stone(Position pos, int color) {
    return place_stone(pos.row, pos.col, color);
}

void Board::undo_last_move() {
    if (move_count_ == 0) return;
    
    const Move& last = moves_.back();
    int row = last.pos.row, col = last.pos.col;
    int removed_index = row * BOARD_SIZE + col;
    int removed_color = cells_[removed_index];

    // Undo bookkeeping in the exact reverse order of update_active_empty,
    // so the state is consistent even if we bail out on a bad cell.
    //
    // 1) The removed stone was on the board, so its own active_empty_
    //    membership was set to "occupied" (index -1). Restore it if any
    //    neighbor is still present.
    // 2) Every 5x5 cell around (row, col) that had its neighbor_count_
    //    incremented at placement time must be decremented now; when a
    //    cell's count reaches zero and it is empty, deactivate it.
    // 3) The placed cell itself is not part of (2)'s increments (it was
    //    occupied then); its count only reflects neighbors that existed
    //    before placement. Do NOT decrement its own count.
    cells_[removed_index] = EMPTY;
    set_stone_bits(row, col, removed_color, false);
    zobrist_hash_ = ZobristHash::update_hash(zobrist_hash_, removed_index, removed_color);
    moves_.pop_back();
    move_count_--;

    if (move_count_ == 0) {
        // Board just became empty: every cell is a candidate again.
        rebuild_active_empty();
        return;
    }

    for (int dr = -2; dr <= 2; ++dr) {
        for (int dc = -2; dc <= 2; ++dc) {
            int nr = row + dr;
            int nc = col + dc;
            if (!is_valid(nr, nc)) continue;
            int nindex = nr * BOARD_SIZE + nc;
            if (nindex == removed_index) continue;
            if (cells_[nindex] != EMPTY) continue;
            if (neighbor_count_[nindex] == 0) continue;
            neighbor_count_[nindex]--;
            if (neighbor_count_[nindex] == 0) {
                has_neighbor_[nindex] = false;
                deactivate(nindex);
            }
        }
    }

    if (neighbor_count_[removed_index] > 0) {
        has_neighbor_[removed_index] = true;
        activate(removed_index, row, col);
    } else {
        has_neighbor_[removed_index] = false;
    }
}

void Board::set_stone_bits(int row, int col, int color, bool present) {
    // The line arrays are indexed by colour, so a stray value here would write
    // outside them rather than just leaving a cell nobody reads.
    if (color < BLACK || color > WHITE) return;
    for (int dir = 0; dir < 4; ++dir) {
        const int line = line_index(dir, row, col);
        const uint64_t bit = uint64_t{1} << (line_position(dir, row, col) + LINE_MARGIN);
        uint64_t& word = lines_[color][dir][line];
        word = present ? (word | bit) : (word & ~bit);
    }
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
        // Renju: BLACK must form exactly five stones to win - an overline of
        // six or more is an illegal shape (already rejected by is_forbidden),
        // and treating it as a win would give BLACK two ways to win.
        // WHITE has no forbidden rules and wins on any run of five or more.
        const int run = run_length(d, row, col, color);
        if (color == BLACK) {
            if (run == 5) return true;
        } else {
            if (run >= 5) return true;
        }
    }
    return false;
}

int Board::run_length(int dir, int row, int col, int color) const {
    if (dir < 0 || dir >= 4 || color < EMPTY || color > WHITE) return 0;
    if (!is_valid(row, col)) return 1;

    const uint64_t word = lines_[color][dir][line_index(dir, row, col)];
    const int bit = line_position(dir, row, col) + LINE_MARGIN;

    // The centre counts even when it holds no stone of this colour, matching the
    // cell-by-cell walk this replaced: callers ask "would playing here make a
    // five", so they pass an empty square. The margin bits below the first cell
    // and above the last are never set, so both runs stop at the board edge
    // without a bounds check per step.
    int run = 1;
    for (int k = bit + 1; k < 64 && ((word >> k) & 1u); ++k) ++run;
    for (int k = bit - 1; k >= 0 && ((word >> k) & 1u); --k) ++run;
    return run;
}

void Board::update_active_empty(int row, int col) {
    int placed_index = row * BOARD_SIZE + col;

    if (move_count_ == 1) {
        // First stone on an empty board: active_empty_ was seeded with all
        // 225 cells. Rebuild the neighborhood view from scratch so cells
        // outside the 5x5 box drop out and neighbor_count_ is correct.
        rebuild_active_empty();
        return;
    }

    // The placed cell is no longer empty; drop it from active_empty_.
    deactivate(placed_index);
    has_neighbor_[placed_index] = false;

    for (int dr = -2; dr <= 2; ++dr) {
        for (int dc = -2; dc <= 2; ++dc) {
            int nr = row + dr;
            int nc = col + dc;
            if (!is_valid(nr, nc)) continue;
            int nindex = nr * BOARD_SIZE + nc;
            if (cells_[nindex] != EMPTY) continue;
            if (neighbor_count_[nindex]++ == 0) {
                has_neighbor_[nindex] = true;
                activate(nindex, nr, nc);
            }
        }
    }
}

void Board::activate(int index, int row, int col) {
    if (active_index_[index] >= 0) return;
    active_index_[index] = static_cast<int16_t>(active_empty_.size());
    active_empty_.push_back(Position(row, col));
}

void Board::deactivate(int index) {
    int16_t pos = active_index_[index];
    if (pos < 0) return;
    int last_i = static_cast<int>(active_empty_.size()) - 1;
    if (pos != last_i) {
        Position moved = active_empty_[last_i];
        active_empty_[pos] = moved;
        active_index_[moved.row * BOARD_SIZE + moved.col] = pos;
    }
    active_empty_.pop_back();
    active_index_[index] = -1;
}

void Board::rebuild_active_empty() {
    has_neighbor_.fill(false);
    neighbor_count_.fill(0);
    active_index_.fill(-1);
    active_empty_.clear();

    if (move_count_ == 0) {
        for (int r = 0; r < BOARD_SIZE; ++r) {
            for (int c = 0; c < BOARD_SIZE; ++c) {
                activate(r * BOARD_SIZE + c, r, c);
            }
        }
        return;
    }

    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            int index = r * BOARD_SIZE + c;
            if (cells_[index] != EMPTY) continue;
            int count = 0;
            for (int dr = -2; dr <= 2; ++dr) {
                for (int dc = -2; dc <= 2; ++dc) {
                    int nr = r + dr;
                    int nc = c + dc;
                    if (!is_valid(nr, nc)) continue;
                    if (cells_[nr * BOARD_SIZE + nc] == EMPTY) continue;
                    count++;
                }
            }
            neighbor_count_[index] = static_cast<uint8_t>(count);
            if (count > 0) {
                has_neighbor_[index] = true;
                activate(index, r, c);
            }
        }
    }
}

std::vector<Position> Board::get_candidate_moves() const {
    return active_empty_;
}

}

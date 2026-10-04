#ifndef GOMOKU_TYPES_H
#define GOMOKU_TYPES_H

#include <cstdint>
#include <array>

namespace gomoku {

constexpr int BOARD_SIZE = 15;
constexpr int MAX_MOVES = BOARD_SIZE * BOARD_SIZE;
constexpr int EMPTY = 0;
constexpr int BLACK = 1;
constexpr int WHITE = 2;

struct Position {
    int row;
    int col;
    
    Position() : row(-1), col(-1) {}
    Position(int r, int c) : row(r), col(c) {}
    
    bool operator==(const Position& other) const {
        return row == other.row && col == other.col;
    }
    
    bool operator!=(const Position& other) const {
        return !(*this == other);
    }
    
    bool operator<(const Position& other) const {
        if (row != other.row) return row < other.row;
        return col < other.col;
    }
    
    bool is_valid() const {
        return row >= 0 && row < BOARD_SIZE && col >= 0 && col < BOARD_SIZE;
    }
    
    int to_index() const {
        return row * BOARD_SIZE + col;
    }
    
    static Position from_index(int index) {
        return Position(index / BOARD_SIZE, index % BOARD_SIZE);
    }
};

struct Move {
    Position pos;
    int color;
    
    Move() : color(EMPTY) {}
    Move(int row, int col, int c) : pos(row, col), color(c) {}
    Move(Position p, int c) : pos(p), color(c) {}
    
    bool operator==(const Move& other) const {
        return pos == other.pos && color == other.color;
    }
};

inline int opponent(int color) {
    return (color == BLACK) ? WHITE : BLACK;
}

constexpr int DIRECTIONS[4][2] = {
    {0, 1},   // horizontal
    {1, 0},   // vertical
    {1, 1},   // diagonal down-right
    {1, -1}   // diagonal down-left
};

}

#endif

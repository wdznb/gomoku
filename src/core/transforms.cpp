#include "transforms.h"
#include <algorithm>

namespace gomoku {

Position transform_position(Position pos, int variant) {
    int r = pos.row;
    int c = pos.col;
    if (variant & 4) std::swap(r, c);
    if (variant & 1) r = BOARD_SIZE - 1 - r;
    if (variant & 2) c = BOARD_SIZE - 1 - c;
    return Position(r, c);
}

Board transform_board(const Board& board, int variant) {
    Board result;
    for (const auto& move : board.move_history()) {
        Position moved = transform_position(move.pos, variant);
        result.place_stone(moved, move.color);
    }
    return result;
}

std::vector<int> self_symmetries(const Board& board) {
    std::vector<int> variants;
    for (int v = 0; v < SYMMETRY_COUNT; ++v) {
        bool preserved = true;
        for (const auto& move : board.move_history()) {
            Position mapped = transform_position(move.pos, v);
            if (board.get(mapped) != move.color) {
                preserved = false;
                break;
            }
        }
        if (preserved) variants.push_back(v);
    }
    return variants;
}

bool is_symmetry_representative(const std::vector<int>& sym, Position pos) {
    const int index = pos.to_index();
    for (const int variant : sym) {
        if (transform_position(pos, variant).to_index() < index) return false;
    }
    return true;
}

int canonical_cell(const Board& board, Position pos) {
    int best = pos.to_index();
    for (int variant : self_symmetries(board)) {
        best = std::min(best, transform_position(pos, variant).to_index());
    }
    return best;
}

}

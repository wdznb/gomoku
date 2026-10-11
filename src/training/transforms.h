#ifndef GOMOKU_TRAINING_TRANSFORMS_H
#define GOMOKU_TRAINING_TRANSFORMS_H

#include "../core/board.h"
#include <vector>

namespace gomoku {

// The eight symmetries of the square (dihedral group D4). Variant encoding
// matches OpeningBook::apply_transform: bit0 reflects rows, bit1 reflects
// columns, bit2 transposes across the main diagonal.
constexpr int SYMMETRY_COUNT = 8;

Position transform_position(Position pos, int variant);
Board transform_board(const Board& board, int variant);

// Variants that map this exact position back onto itself. Used to treat
// mirror-image branch points as already explored during self-play retries.
std::vector<int> self_symmetries(const Board& board);

// The lowest cell index reachable from pos by any self-symmetry of the board,
// so two symmetric moves share one key.
int canonical_cell(const Board& board, Position pos);

}

#endif

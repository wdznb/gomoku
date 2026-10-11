#ifndef GOMOKU_CORE_TRANSFORMS_H
#define GOMOKU_CORE_TRANSFORMS_H

#include "board.h"
#include <vector>

namespace gomoku {

// The eight symmetries of the square (dihedral group D4). Variant encoding
// matches OpeningBook::apply_transform: bit0 reflects rows, bit1 reflects
// columns, bit2 transposes across the main diagonal.
constexpr int SYMMETRY_COUNT = 8;

Position transform_position(Position pos, int variant);
Board transform_board(const Board& board, int variant);

// Variants that map this exact position back onto itself. Self-play retries use
// them to treat mirror-image branch points as already explored, and the search
// uses them to drop root moves that are mirrors of a move it already covers.
std::vector<int> self_symmetries(const Board& board);

// True when pos is the lowest-index cell of its orbit under the variants in
// sym, i.e. the representative of a group of mirror-equivalent squares.
bool is_symmetry_representative(const std::vector<int>& sym, Position pos);

// The lowest cell index reachable from pos by any self-symmetry of the board,
// so two symmetric moves share one key.
int canonical_cell(const Board& board, Position pos);

}

#endif

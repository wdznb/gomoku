#ifndef GOMOKU_POSITION_SETUP_H
#define GOMOKU_POSITION_SETUP_H

#include "board.h"
#include "pattern.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gomoku {

// Which grid token stands for which colour. Tokens are kept as strings so that
// 0/1/2, B/W/., X/O/. and 黑/白/空 are three readings of one grid rather than
// three board types: the position itself is always a Board of EMPTY/BLACK/WHITE.
struct GridEncoding {
    std::string black;
    std::string white;
    std::string empty;

    bool operator==(const GridEncoding& other) const;
    bool operator!=(const GridEncoding& other) const { return !(*this == other); }
};

// One row of grid tokens, already split. BOARD_SIZE of these make a board.
using GridRow = std::vector<std::string>;
using GridCells = std::vector<GridRow>;

// A position read from any supported input form, together with the facts the
// caller has to report: what the tokens were taken to mean, whose turn follows
// from the stone counts, and why a rejected position was rejected.
struct SetupResult {
    Board board;
    // Only the move-list form has an order; grid and bitboard inputs leave it
    // empty, because the stones were never played one after another.
    std::vector<Move> history;
    int black_count = 0;
    int white_count = 0;
    int side_to_move = BLACK;
    // Grid forms: the reading that was applied (always reported, so a human
    // interface can show it back for confirmation). Empty for the other forms.
    GridEncoding encoding;
    std::string error;

    bool ok() const { return error.empty(); }
};

// Bitboard form: bit (row * BOARD_SIZE + col) of word (i / 64), so 4 uint64
// words cover the 225 cells and the engine's internal per-line words - which
// depend on LINE_MARGIN and the direction families - stay an implementation
// detail no input format has to track.
constexpr int BITBOARD_WORDS = 4;
constexpr int BITBOARD_TOP_BITS = MAX_MOVES - (BITBOARD_WORDS - 1) * 64;
constexpr uint64_t BITBOARD_TOP_MASK = (1ull << BITBOARD_TOP_BITS) - 1;

// Split a text grid into cells. Rows of BOARD_SIZE tokens (comma / semicolon /
// tab / whitespace separated, or one ASCII symbol per character) are read first;
// anything else is read flat, as the whole document: 225 separated cells, or a
// continuous run of one symbol per cell — at most three distinct symbols, and
// line breaks may fall anywhere. Blank lines and '#' comment lines are ignored,
// a UTF-8 BOM is tolerated.
bool parse_grid_text(const std::string& text, GridCells& out, std::string& error);

// The documented encodings that cover this grid's tokens, reduced to distinct
// readings and listed in priority order: the first entry is the reading to
// apply. Several alphabets agreeing on every token (three of them use "." as
// empty) collapse into one entry, so this is empty exactly when the grid mixes
// notations or uses a symbol no format defines - a rejection, not a guess.
std::vector<GridEncoding> detect_grid_encodings(const GridCells& cells);

// Build and validate. `forced` overrides detection - that is how the UI sends
// back the mapping the player confirmed, and how a caller with no human in the
// loop states its template. Pass nullptr to detect.
SetupResult position_from_grid(const GridCells& cells, const GridEncoding* forced = nullptr);
SetupResult position_from_bitboard(const std::array<uint64_t, BITBOARD_WORDS>& black,
                                   const std::array<uint64_t, BITBOARD_WORDS>& white);
// Alternating colours from `first_color` (BLACK or WHITE), in list order.
SetupResult position_from_moves(const std::vector<Position>& moves, int first_color = BLACK);

// The legality checks every form shares, also usable on a Board the caller built
// itself (a trainer loading a position, a test setting one up by hand). Sets the
// counts and side_to_move on `out`, or its error. BLACK's first-move obligation
// is what decides the turn: equal counts mean BLACK plays next, one more BLACK
// stone than WHITE means WHITE does, anything else is not a position a game
// reaches. An existing five means the game is already over, and a BLACK stone
// sitting on a forbidden square means the position was never legal to build.
void validate_position(const Board& board, SetupResult& out);

}

#endif

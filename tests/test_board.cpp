#include "core/board.h"
#include <algorithm>
#include <iostream>
#include <random>
#include <vector>
#include <cassert>

using namespace gomoku;

void test_board_init() {
    Board board;
    assert(board.move_count() == 0);
    
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            assert(board.is_empty(r, c));
        }
    }
    std::cout << "test_board_init passed" << std::endl;
}

void test_place_stone() {
    Board board;
    
    assert(board.place_stone(7, 7, BLACK));
    assert(board.get(7, 7) == BLACK);
    assert(board.move_count() == 1);
    
    assert(!board.place_stone(7, 7, WHITE));
    
    assert(board.place_stone(7, 8, WHITE));
    assert(board.get(7, 8) == WHITE);
    assert(board.move_count() == 2);
    
    std::cout << "test_place_stone passed" << std::endl;
}

void test_undo() {
    Board board;
    
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, WHITE);
    
    assert(board.move_count() == 2);
    
    board.undo_last_move();
    assert(board.move_count() == 1);
    assert(board.is_empty(7, 8));
    assert(board.get(7, 7) == BLACK);
    
    board.undo_last_move();
    assert(board.move_count() == 0);
    assert(board.is_empty(7, 7));
    
    std::cout << "test_undo passed" << std::endl;
}

void test_five_in_a_row() {
    Board board;
    
    board.place_stone(7, 3, BLACK);
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    
    assert(!board.has_five_in_a_row(7, 3, BLACK));
    
    board.place_stone(7, 7, BLACK);
    assert(board.has_five_in_a_row(7, 7, BLACK));
    
    assert(board.check_winner() == BLACK);
    
    std::cout << "test_five_in_a_row passed" << std::endl;
}

void test_vertical_five() {
    Board board;
    
    board.place_stone(3, 7, WHITE);
    board.place_stone(4, 7, WHITE);
    board.place_stone(5, 7, WHITE);
    board.place_stone(6, 7, WHITE);
    board.place_stone(7, 7, WHITE);
    
    assert(board.check_winner() == WHITE);
    
    std::cout << "test_vertical_five passed" << std::endl;
}

void test_diagonal_five() {
    Board board;
    
    board.place_stone(3, 3, BLACK);
    board.place_stone(4, 4, BLACK);
    board.place_stone(5, 5, BLACK);
    board.place_stone(6, 6, BLACK);
    board.place_stone(7, 7, BLACK);
    
    assert(board.check_winner() == BLACK);
    
    std::cout << "test_diagonal_five passed" << std::endl;
}

void test_active_empty() {
    Board board;
    
    size_t initial_size = board.active_empty().size();
    assert(initial_size == BOARD_SIZE * BOARD_SIZE);
    
    board.place_stone(7, 7, BLACK);
    
    size_t after_size = board.active_empty().size();
    assert(after_size > 0);
    assert(after_size < BOARD_SIZE * BOARD_SIZE);
    
    std::cout << "test_active_empty passed" << std::endl;
}

void test_active_empty_incremental_undo() {
    // Round 30 rewrote undo_last_move to decrement neighbor counts instead
    // of a full rebuild. Verify it stays consistent with a fresh board
    // built by replaying the same stones, and that undo all the way back
    // to an empty board restores the 225-cell candidate list.
    Board A;
    const int placements[][3] = {
        {7, 7, BLACK}, {8, 8, WHITE}, {6, 6, BLACK}, {7, 9, WHITE},
        {5, 7, BLACK}, {9, 7, WHITE}, {7, 5, BLACK}, {10, 10, WHITE},
    };
    const int n = static_cast<int>(sizeof(placements) / sizeof(placements[0]));

    for (int i = 0; i < n; ++i) {
        A.place_stone(placements[i][0], placements[i][1], placements[i][2]);
    }
    // Snapshot A's candidate set.
    std::vector<Position> a_active = A.active_empty();

    // Independent replay.
    Board B;
    for (int i = 0; i < n; ++i) {
        B.place_stone(placements[i][0], placements[i][1], placements[i][2]);
    }
    std::vector<Position> b_active = B.active_empty();

    // Same set as multisets (order differs under swap-remove, sort to compare).
    auto key = [](const std::vector<Position>& v) {
        std::vector<int> out;
        out.reserve(v.size());
        for (const auto& p : v) out.push_back(p.row * BOARD_SIZE + p.col);
        std::sort(out.begin(), out.end());
        return out;
    };
    assert(key(a_active) == key(b_active));
    assert(A.zobrist_hash() == B.zobrist_hash());

    // Now undo the last four moves on A and rebuild a reference Board from
    // the first n-4 stones; active_empty must match.
    for (int i = 0; i < 4; ++i) A.undo_last_move();
    Board C;
    for (int i = 0; i < n - 4; ++i) {
        C.place_stone(placements[i][0], placements[i][1], placements[i][2]);
    }
    assert(key(A.active_empty()) == key(C.active_empty()));
    assert(A.zobrist_hash() == C.zobrist_hash());

    // Undo everything: A must be a valid empty board with all cells active.
    while (A.move_count() > 0) A.undo_last_move();
    assert(A.move_count() == 0);
    assert(A.active_empty().size() == BOARD_SIZE * BOARD_SIZE);

    std::cout << "test_active_empty_incremental_undo passed" << std::endl;
}

void test_zobrist_hash() {
    Board board1;
    Board board2;
    
    board1.place_stone(7, 7, BLACK);
    board2.place_stone(7, 7, BLACK);
    
    assert(board1.zobrist_hash() == board2.zobrist_hash());
    
    board1.place_stone(7, 8, WHITE);
    assert(board1.zobrist_hash() != board2.zobrist_hash());
    
    board2.place_stone(7, 8, WHITE);
    assert(board1.zobrist_hash() == board2.zobrist_hash());
    
    std::cout << "test_zobrist_hash passed" << std::endl;
}

void test_line_bitboard_tracks_cells() {
    // Phase 4.1 gave the board a bitboard line view per pattern direction, read
    // directly by the pattern recognizer. Interleave placements and undos and
    // check after every step that each stone's bit is set in exactly the line
    // words of its own colour, in all four direction families.
    std::mt19937 rng(20261011);
    Board board;

    auto verify = [](const Board& b) {
        for (int r = 0; r < BOARD_SIZE; ++r) {
            for (int c = 0; c < BOARD_SIZE; ++c) {
                const int cell = b.get(r, c);
                for (int d = 0; d < 4; ++d) {
                    const uint64_t bit = uint64_t{1} << (line_position(d, r, c) + LINE_MARGIN);
                    const int line = line_index(d, r, c);
                    const bool black = (b.line_bits(BLACK, d, line) & bit) != 0;
                    const bool white = (b.line_bits(WHITE, d, line) & bit) != 0;
                    assert(black == (cell == BLACK));
                    assert(white == (cell == WHITE));
                }
            }
        }
    };

    verify(board);
    for (int step = 0; step < 600; ++step) {
        if (step % 3 == 2 && board.move_count() > 0) {
            board.undo_last_move();
        } else {
            const int r = static_cast<int>(rng() % BOARD_SIZE);
            const int c = static_cast<int>(rng() % BOARD_SIZE);
            board.place_stone(r, c, board.move_count() % 2 == 0 ? BLACK : WHITE);
        }
        verify(board);
    }

    // Copies must carry the bitboards too: the search clones the root board.
    Board copy = board;
    verify(copy);
    assert(copy.zobrist_hash() == board.zobrist_hash());

    // Undoing to an empty board and resetting must both leave every word clear.
    while (copy.move_count() > 0) copy.undo_last_move();
    verify(copy);
    board.reset();
    verify(board);

    std::cout << "test_line_bitboard_tracks_cells passed" << std::endl;
}

void test_run_length_matches_walk() {
    // run_length is the bitboard replacement for the old two-sided cell walk and
    // now feeds has_five_in_a_row plus the Renju overline reclassification. Check
    // it against an independent walk over get() on randomised boards, on empty
    // squares included - callers ask "would playing here make a five".
    auto walk = [](const Board& b, int dir, int row, int col, int color) {
        const int dr = DIRECTIONS[dir][0];
        const int dc = DIRECTIONS[dir][1];
        int run = 1;
        for (int i = 1; ; ++i) {
            const int r = row + i * dr;
            const int c = col + i * dc;
            if (!b.is_valid(r, c) || b.get(r, c) != color) break;
            ++run;
        }
        for (int i = 1; ; ++i) {
            const int r = row - i * dr;
            const int c = col - i * dc;
            if (!b.is_valid(r, c) || b.get(r, c) != color) break;
            ++run;
        }
        return run;
    };

    std::mt19937 rng(987654321);
    long long checked = 0;
    for (int game = 0; game < 60; ++game) {
        Board board;
        const int stones = 1 + static_cast<int>(rng() % 45);
        for (int i = 0; i < stones; ++i) {
            const int r = static_cast<int>(rng() % BOARD_SIZE);
            const int c = static_cast<int>(rng() % BOARD_SIZE);
            board.place_stone(r, c, i % 2 == 0 ? BLACK : WHITE);
        }
        for (int r = 0; r < BOARD_SIZE; ++r) {
            for (int c = 0; c < BOARD_SIZE; ++c) {
                for (int d = 0; d < 4; ++d) {
                    for (int side : {BLACK, WHITE}) {
                        assert(board.run_length(d, r, c, side) == walk(board, d, r, c, side));
                        ++checked;
                    }
                }
            }
        }
    }

    // Out-of-board coordinates must not read outside the line arrays.
    assert(Board().run_length(0, -1, 0, BLACK) == 1);
    assert(Board().run_length(3, BOARD_SIZE, BOARD_SIZE, WHITE) == 1);

    assert(checked >= 100000);
    std::cout << "test_run_length_matches_walk passed (" << checked << " runs)" << std::endl;
}

int main() {
    std::cout << "Running board tests..." << std::endl;
    
    test_board_init();
    test_place_stone();
    test_undo();
    test_five_in_a_row();
    test_vertical_five();
    test_diagonal_five();
    test_active_empty();
    test_active_empty_incremental_undo();
    test_line_bitboard_tracks_cells();
    test_run_length_matches_walk();
    test_zobrist_hash();
    
    std::cout << "All board tests passed!" << std::endl;
    return 0;
}

#include "core/board.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <random>
#include <thread>
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

// A8 (design doc 2.5.1): candidate tiering needs the two facts the board already
// maintains privately — how many stones stand within Chebyshev distance 2 of a cell,
// and whether that cell is in the candidate list at all. Both are now readable, so
// the incremental bookkeeping can be differentially tested against a recount for the
// first time; before this round only a full-board rebuild and a hash could be compared.
void test_neighbor_count_at_matches_a_recount() {
    auto recount = [](const Board& board, Position pos) {
        int count = 0;
        for (int dr = -2; dr <= 2; ++dr) {
            for (int dc = -2; dc <= 2; ++dc) {
                const int nr = pos.row + dr, nc = pos.col + dc;
                if (!board.is_valid(nr, nc)) continue;
                if (board.get(nr, nc) != EMPTY) ++count;
            }
        }
        return count;
    };
    auto all_match = [&](const Board& board) {
        for (int r = 0; r < BOARD_SIZE; ++r) {
            for (int c = 0; c < BOARD_SIZE; ++c) {
                const Position p(r, c);
                if (!board.is_empty(p)) continue;
                assert(board.neighbor_count_at(p) == recount(board, p));
            }
        }
    };

    Board board;
    all_match(board);  // empty board: every cell reads zero

    // A cluster in the middle and two stones stranded in the corners, so the
    // 5x5 window is tested at the board edge as well as inside the field.
    const int placements[][3] = {
        {7, 7, BLACK}, {8, 8, WHITE}, {6, 6, BLACK}, {7, 9, WHITE},
        {5, 7, BLACK}, {0, 0, WHITE}, {14, 14, BLACK}, {14, 0, WHITE},
    };
    const int n = static_cast<int>(sizeof(placements) / sizeof(placements[0]));
    for (int i = 0; i < n; ++i) {
        board.place_stone(placements[i][0], placements[i][1], placements[i][2]);
        all_match(board);  // every ply, not just the final position
    }

    // The undo path decrements instead of rebuilding, which is where a stale count
    // would show up.
    while (board.move_count() > 0) {
        board.undo_last_move();
        all_match(board);
    }

    // Out of bounds answers zero rather than reading outside the array.
    assert(board.neighbor_count_at(Position(-1, 3)) == 0);
    assert(board.neighbor_count_at(Position(15, 15)) == 0);

    std::cout << "test_neighbor_count_at_matches_a_recount passed" << std::endl;
}

void test_is_active_agrees_with_the_candidate_list() {
    Board board;
    // Before any stone: the list is the whole board, so membership is total.
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            assert(board.is_active(Position(r, c)));
        }
    }

    const int placements[][3] = {
        {7, 7, BLACK}, {7, 8, WHITE}, {8, 7, BLACK}, {1, 1, WHITE}, {13, 13, BLACK},
    };
    const int n = static_cast<int>(sizeof(placements) / sizeof(placements[0]));
    for (int i = 0; i < n; ++i) {
        board.place_stone(placements[i][0], placements[i][1], placements[i][2]);
    }

    auto in_list = [&](Position p) {
        const std::vector<Position>& list = board.active_empty();
        return std::find(list.begin(), list.end(), p) != list.end();
    };
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            const Position p(r, c);
            assert(board.is_active(p) == in_list(p));
            if (!board.is_empty(p)) assert(!board.is_active(p));  // a stone is never a candidate
            if (board.neighbor_count_at(p) == 0) assert(!board.is_active(p));
        }
    }

    // A corner no stone of this layout stands within 2 of: both rules agree it is
    // out, and neither answer is about the list's order.
    assert(!board.is_active(Position(14, 0)));
    assert(board.neighbor_count_at(Position(14, 0)) == 0);

    // Undo down to the single centre stone. Its 5x5 box minus the stone itself is
    // the candidate set, named independently of anything the board reports.
    while (board.move_count() > 1) board.undo_last_move();
    int expected = 0;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            const bool in_box = std::abs(r - 7) <= 2 && std::abs(c - 7) <= 2;
            const bool is_stone = (r == 7 && c == 7);
            assert(board.is_active(Position(r, c)) == (in_box && !is_stone));
            assert(board.is_active(Position(r, c)) == in_list(Position(r, c)));
            if (in_box && !is_stone) ++expected;
        }
    }
    assert(static_cast<int>(board.active_empty().size()) == expected);  // 24 cells

    assert(!board.is_active(Position(-1, 0)));
    assert(!board.is_active(Position(0, 15)));

    std::cout << "test_is_active_agrees_with_the_candidate_list passed" << std::endl;
}

void test_has_last_move_distinguishes_an_empty_board() {
    Board board;
    // last_move() on an empty board is a default Move whose position is (-1,-1);
    // that is the sentinel this accessor replaces.
    assert(!board.has_last_move());
    assert(!board.last_move().pos.is_valid());

    assert(board.place_stone(0, 0, BLACK));
    assert(board.has_last_move());
    assert(board.last_move().pos == Position(0, 0));

    // Undoing down to the empty board restores the no-last-move answer, and with it
    // the (-1,-1) sentinel — the two states a caller used to have to guess apart.
    while (board.move_count() > 0) board.undo_last_move();
    assert(!board.has_last_move());
    assert(!board.last_move().pos.is_valid());

    // An undo of an empty board is a no-op, not a negative move count.
    board.undo_last_move();
    assert(!board.has_last_move());
    assert(board.move_count() == 0);

    std::cout << "test_has_last_move_distinguishes_an_empty_board passed" << std::endl;
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

void test_zobrist_init_concurrent_first_use() {
    // Round 74: ZobristHash::init used check-then-fill on a plain bool, so two
    // training threads constructing their first Board could refill the shared
    // table at once: readers took half-filled words and the table mixed two
    // generations. The values are unchanged (seed 12345, same fill order), so
    // the canonical table can be rebuilt here from the seed and compared.
    // This test runs first in main(), before any other Board exists, so its
    // threads are the process's first users of the table.
    std::array<std::array<uint64_t, 3>, BOARD_SIZE * BOARD_SIZE> expected;
    {
        std::mt19937_64 rng(12345);
        for (int i = 0; i < BOARD_SIZE * BOARD_SIZE; ++i) {
            for (int c = 0; c < 3; ++c) {
                expected[i][c] = rng();
            }
        }
    }

    const int thread_count = 8;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<uint64_t> hashes(thread_count, 0);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            ready.fetch_add(1);
            while (!go.load()) {}
            Board board;
            board.place_stone(7, 7, BLACK);
            board.place_stone(3, 11, WHITE);
            hashes[t] = board.zobrist_hash();
        });
    }
    while (ready.load() < thread_count) {}
    go.store(true);
    for (auto& th : threads) {
        th.join();
    }

    const uint64_t want = expected[7 * BOARD_SIZE + 7][BLACK] ^
                          expected[3 * BOARD_SIZE + 11][WHITE];
    for (int t = 0; t < thread_count; ++t) {
        assert(hashes[t] == want);
    }

    std::cout << "test_zobrist_init_concurrent_first_use passed" << std::endl;
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
    
    test_zobrist_init_concurrent_first_use();
    test_board_init();
    test_place_stone();
    test_undo();
    test_five_in_a_row();
    test_vertical_five();
    test_diagonal_five();
    test_active_empty();
    test_active_empty_incremental_undo();
    test_neighbor_count_at_matches_a_recount();
    test_is_active_agrees_with_the_candidate_list();
    test_has_last_move_distinguishes_an_empty_board();
    test_line_bitboard_tracks_cells();
    test_run_length_matches_walk();
    test_zobrist_hash();
    
    std::cout << "All board tests passed!" << std::endl;
    return 0;
}

#include "core/board.h"
#include <algorithm>
#include <iostream>
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
    test_zobrist_hash();
    
    std::cout << "All board tests passed!" << std::endl;
    return 0;
}

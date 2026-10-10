#include "core/board.h"
#include "core/pattern.h"
#include <iostream>
#include <cassert>

using namespace gomoku;

void test_pattern_init() {
    PatternRecognizer::init();
    std::cout << "test_pattern_init passed" << std::endl;
}

void test_recognize_five() {
    Board board;
    
    board.place_stone(7, 3, BLACK);
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    
    PositionPattern pattern = PatternRecognizer::recognize_position(board, 7, 7, BLACK);
    
    bool has_five = false;
    for (int d = 0; d < 4; ++d) {
        if (pattern.directions[d].type == PatternType::FIVE) {
            has_five = true;
            break;
        }
    }
    
    assert(has_five);
    std::cout << "test_recognize_five passed" << std::endl;
}

void test_recognize_live_four() {
    Board board;
    
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(7, 7, BLACK);
    
    PositionPattern pattern = PatternRecognizer::recognize_position(board, 7, 4, BLACK);
    
    bool has_live_four = false;
    for (int d = 0; d < 4; ++d) {
        if (pattern.directions[d].type == PatternType::LIVE_FOUR) {
            has_live_four = true;
            break;
        }
    }
    
    assert(has_live_four);
    std::cout << "test_recognize_live_four passed" << std::endl;
}

void test_combined_pattern() {
    Board board;
    
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    
    board.place_stone(5, 7, BLACK);
    board.place_stone(6, 7, BLACK);
    
    CombinedPattern combined = PatternRecognizer::recognize_combined(board, 7, 7, BLACK);
    
    assert(combined.four_three);
    std::cout << "test_combined_pattern passed" << std::endl;
}

void test_forbidden() {
    Board board;
    
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    
    board.place_stone(5, 7, BLACK);
    board.place_stone(6, 7, BLACK);
    
    bool is_forbidden = PatternRecognizer::is_forbidden(board, 7, 7, BLACK);
    
    assert(is_forbidden);
    std::cout << "test_forbidden passed" << std::endl;
}

void test_white_no_forbidden() {
    Board board;
    
    board.place_stone(7, 5, WHITE);
    board.place_stone(7, 6, WHITE);
    
    board.place_stone(5, 7, WHITE);
    board.place_stone(6, 7, WHITE);
    
    bool is_forbidden = PatternRecognizer::is_forbidden(board, 7, 7, WHITE);
    
    assert(!is_forbidden);
    std::cout << "test_white_no_forbidden passed" << std::endl;
}

void test_recognize_live_three() {
    Board board;
    
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    
    PositionPattern pattern = PatternRecognizer::recognize_position(board, 7, 4, BLACK);
    
    bool has_live_three = false;
    for (int d = 0; d < 4; ++d) {
        if (pattern.directions[d].type == PatternType::LIVE_THREE) {
            has_live_three = true;
            break;
        }
    }
    
    assert(has_live_three);
    std::cout << "test_recognize_live_three passed" << std::endl;
}

void test_five_not_forbidden() {
    Board board;
    
    board.place_stone(7, 3, BLACK);
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    
    board.place_stone(5, 7, BLACK);
    board.place_stone(6, 7, BLACK);
    
    bool is_forbidden = PatternRecognizer::is_forbidden(board, 7, 7, BLACK);
    assert(!is_forbidden);
    
    PositionPattern pattern = PatternRecognizer::recognize_position(board, 7, 7, BLACK);
    bool has_five = false;
    for (int d = 0; d < 4; ++d) {
        if (pattern.directions[d].type == PatternType::FIVE) {
            has_five = true;
            break;
        }
    }
    assert(has_five);
    
    std::cout << "test_five_not_forbidden passed" << std::endl;
}

void test_blocked_four_is_none() {
    Board board;
    
    board.place_stone(7, 2, WHITE);
    board.place_stone(7, 3, BLACK);
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(7, 7, WHITE);
    
    PositionPattern pattern = PatternRecognizer::recognize_position(board, 7, 4, BLACK);
    
    bool is_none = true;
    for (int d = 0; d < 4; ++d) {
        if (pattern.directions[d].type != PatternType::NONE) {
            is_none = false;
            break;
        }
    }
    
    assert(is_none);
    std::cout << "test_blocked_four_is_none passed" << std::endl;
}

void test_overline_at_window_edge() {
    // BLACK already holds (7,3)..(7,7). Placing at (7,8) forms six-in-a-row.
    // The 9-cell pattern window at (7,8) only sees offsets -4..+4 (columns
    // 4..12), so without an edge-extension probe match_pattern counts 5 and
    // calls it FIVE. Under Renju this is a long-connect forbidden.
    Board board;

    board.place_stone(7, 3, BLACK);
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(7, 7, BLACK);

    PositionPattern pattern = PatternRecognizer::recognize_position(board, 7, 8, BLACK);

    bool has_long_five = false;
    bool has_plain_five = false;
    for (int d = 0; d < 4; ++d) {
        if (pattern.directions[d].type == PatternType::LONG_FIVE) has_long_five = true;
        if (pattern.directions[d].type == PatternType::FIVE) has_plain_five = true;
    }

    assert(has_long_five);
    assert(!has_plain_five);
    assert(PatternRecognizer::is_forbidden(board, 7, 8, BLACK));
    std::cout << "test_overline_at_window_edge passed" << std::endl;
}

void test_overline_not_win_for_black() {
    // has_five_in_a_row must return false when BLACK has six in a row, so
    // check_winner does not award BLACK a win via an illegal overline.
    Board board;

    board.place_stone(7, 3, BLACK);
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(7, 7, BLACK);

    // Board.place_stone does not itself enforce forbidden rules; place a sixth
    // stone to build the overline shape and check winner detection.
    board.place_stone(7, 8, BLACK);

    assert(!board.has_five_in_a_row(7, 5, BLACK));
    assert(board.check_winner() == EMPTY);
    std::cout << "test_overline_not_win_for_black passed" << std::endl;
}

void test_overline_is_win_for_white() {
    // WHITE has no forbidden rules; six in a row still counts as a win.
    Board board;

    board.place_stone(7, 3, WHITE);
    board.place_stone(7, 4, WHITE);
    board.place_stone(7, 5, WHITE);
    board.place_stone(7, 6, WHITE);
    board.place_stone(7, 7, WHITE);
    board.place_stone(7, 8, WHITE);

    assert(board.has_five_in_a_row(7, 5, WHITE));
    assert(board.check_winner() == WHITE);
    std::cout << "test_overline_is_win_for_white passed" << std::endl;
}

int main() {
    std::cout << "Running pattern tests..." << std::endl;
    
    test_pattern_init();
    test_recognize_five();
    test_recognize_live_four();
    test_recognize_live_three();
    test_combined_pattern();
    test_forbidden();
    test_five_not_forbidden();
    test_white_no_forbidden();
    test_blocked_four_is_none();
    test_overline_at_window_edge();
    test_overline_not_win_for_black();
    test_overline_is_win_for_white();
    
    std::cout << "All pattern tests passed!" << std::endl;
    return 0;
}

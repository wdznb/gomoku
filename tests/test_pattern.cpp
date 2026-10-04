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

int main() {
    std::cout << "Running pattern tests..." << std::endl;
    
    test_pattern_init();
    test_recognize_five();
    test_recognize_live_four();
    test_combined_pattern();
    test_forbidden();
    test_white_no_forbidden();
    
    std::cout << "All pattern tests passed!" << std::endl;
    return 0;
}

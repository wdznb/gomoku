#include "core/board.h"
#include "core/pattern.h"
#include "core/evaluator.h"
#include <iostream>
#include <cassert>

using namespace gomoku;

void test_evaluator_init() {
    Evaluator evaluator;
    std::cout << "test_evaluator_init passed" << std::endl;
}

void test_position_weight() {
    Evaluator evaluator;
    
    int center_weight = evaluator.get_position_weight(7, 7);
    int corner_weight = evaluator.get_position_weight(0, 0);
    int edge_weight = evaluator.get_position_weight(0, 7);
    
    assert(center_weight > edge_weight);
    assert(edge_weight > corner_weight);
    
    std::cout << "test_position_weight passed" << std::endl;
}

void test_evaluate_position() {
    Board board;
    Evaluator evaluator;
    
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    
    int score = evaluator.evaluate_position(board, 7, 7, BLACK);
    
    assert(score > 0);
    std::cout << "test_evaluate_position passed (score: " << score << ")" << std::endl;
}

void test_evaluate_both_perspectives() {
    Board board;
    Evaluator evaluator;
    
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    
    int black_score = evaluator.evaluate_position_both(board, 7, 7, BLACK);
    int white_score = evaluator.evaluate_position_both(board, 7, 7, WHITE);
    
    assert(black_score > 0);
    assert(white_score > 0);
    
    std::cout << "test_evaluate_both_perspectives passed" << std::endl;
    std::cout << "  Black score: " << black_score << std::endl;
    std::cout << "  White score: " << white_score << std::endl;
}

void test_custom_coefficients() {
    ThreatCoefficients coeffs;
    coeffs.five = 200000;
    coeffs.live_four = 20000;
    
    Evaluator evaluator(coeffs);
    
    Board board;
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(7, 7, BLACK);
    
    int score = evaluator.evaluate_position(board, 7, 3, BLACK);
    
    assert(score >= coeffs.live_four);
    
    std::cout << "test_custom_coefficients passed (score: " << score << ")" << std::endl;
}

int main() {
    std::cout << "Running evaluator tests..." << std::endl;
    
    PatternRecognizer::init();
    
    test_evaluator_init();
    test_position_weight();
    test_evaluate_position();
    test_evaluate_both_perspectives();
    test_custom_coefficients();
    
    std::cout << "All evaluator tests passed!" << std::endl;
    return 0;
}

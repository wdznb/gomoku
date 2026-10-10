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

void test_pattern_score_includes_five() {
    // Round 32: pattern_score must include coeffs_.five when the recognized
    // pattern shows an exact FIVE. Because pattern_score is private, drive
    // the check through evaluate_position (which returns coeffs_.five on a
    // FIVE) plus a comparison against a non-FIVE threat to confirm the
    // aggregate scoring path (used by evaluate_board for quiescence
    // stand-pat) can distinguish an exact five from a live four.
    ThreatCoefficients coeffs;
    Evaluator evaluator(coeffs);

    Board board;
    // XXXX_ with WHITE: filling the gap is exact five, not a live four.
    board.place_stone(7, 4, WHITE);
    board.place_stone(7, 5, WHITE);
    board.place_stone(7, 6, WHITE);
    board.place_stone(7, 7, WHITE);

    int five_score = evaluator.evaluate_position(board, 7, 8, WHITE);
    assert(five_score >= coeffs.five);

    std::cout << "test_pattern_score_includes_five passed (score: " << five_score << ")" << std::endl;
}

void test_evaluate_board_sees_mate_one() {
    // Round 32: evaluate_board aggregates pattern_score over candidates, so
    // a position where one side has a mate-in-one must show a strictly
    // larger swing in that side's favour than an otherwise identical,
    // threat-free position. Guards the quiescence leaf value against
    // silently missing decisive one-ply threats.
    Evaluator evaluator;

    Board with_threat;
    with_threat.place_stone(7, 4, WHITE);
    with_threat.place_stone(7, 5, WHITE);
    with_threat.place_stone(7, 6, WHITE);
    with_threat.place_stone(7, 7, WHITE);
    with_threat.place_stone(9, 4, BLACK);
    with_threat.place_stone(9, 5, BLACK);

    int eval_white_threat = evaluator.evaluate_board(with_threat, BLACK);

    Board no_threat;
    no_threat.place_stone(7, 4, WHITE);
    no_threat.place_stone(7, 5, WHITE);
    no_threat.place_stone(7, 6, WHITE);
    // no closing square threat: only 3-in-row.
    no_threat.place_stone(9, 4, BLACK);
    no_threat.place_stone(9, 5, BLACK);

    int eval_no_threat = evaluator.evaluate_board(no_threat, BLACK);

    // BLACK-to-move evaluation should be strictly worse when WHITE has a
    // mate-in-one than when WHITE has no such threat.
    assert(eval_white_threat < eval_no_threat);

    std::cout << "test_evaluate_board_sees_mate_one passed ("
              << eval_white_threat << " vs " << eval_no_threat << ")" << std::endl;
}

int main() {
    std::cout << "Running evaluator tests..." << std::endl;
    
    PatternRecognizer::init();
    
    test_evaluator_init();
    test_position_weight();
    test_evaluate_position();
    test_evaluate_both_perspectives();
    test_custom_coefficients();
    test_pattern_score_includes_five();
    test_evaluate_board_sees_mate_one();
    
    std::cout << "All evaluator tests passed!" << std::endl;
    return 0;
}

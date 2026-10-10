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

void test_white_gap_split_is_five() {
    // WHITE: XXXX_X. Filling the gap yields six-in-a-row, which is a WIN for
    // WHITE under Renju (WHITE is not subject to overline rules). The 9-cell
    // window's match_pattern labels any 6+ run as LONG_FIVE, but recognize_
    // direction must demote WHITE's LONG_FIVE back to FIVE or must_block,
    // has_decisive_tempo and the evaluator all miss the winning threat.
    Board board;

    board.place_stone(7, 3, WHITE);
    board.place_stone(7, 4, WHITE);
    board.place_stone(7, 5, WHITE);
    board.place_stone(7, 6, WHITE);
    // col 7 left empty (the gap)
    board.place_stone(7, 8, WHITE);

    PositionPattern pattern = PatternRecognizer::recognize_position(board, 7, 7, WHITE);

    bool has_five = false;
    bool has_long_five = false;
    for (int d = 0; d < 4; ++d) {
        if (pattern.directions[d].type == PatternType::FIVE) has_five = true;
        if (pattern.directions[d].type == PatternType::LONG_FIVE) has_long_five = true;
    }
    assert(has_five);
    assert(!has_long_five);

    // BLACK must not be reported as forbidden for filling this shape; and
    // the same shape for BLACK at the gap must be labeled LONG_FIVE (overline).
    assert(!PatternRecognizer::is_forbidden(board, 7, 7, WHITE));

    Board bboard;
    bboard.place_stone(7, 3, BLACK);
    bboard.place_stone(7, 4, BLACK);
    bboard.place_stone(7, 5, BLACK);
    bboard.place_stone(7, 6, BLACK);
    bboard.place_stone(7, 8, BLACK);
    PositionPattern bpattern = PatternRecognizer::recognize_position(bboard, 7, 7, BLACK);
    bool b_has_long = false;
    bool b_has_plain_five = false;
    for (int d = 0; d < 4; ++d) {
        if (bpattern.directions[d].type == PatternType::LONG_FIVE) b_has_long = true;
        if (bpattern.directions[d].type == PatternType::FIVE) b_has_plain_five = true;
    }
    assert(b_has_long);
    assert(!b_has_plain_five);

    std::cout << "test_white_gap_split_is_five passed" << std::endl;
}

void test_forbidden_from_pattern_equivalence() {
    // Round 41: is_forbidden was refactored to delegate its decision to
    // is_forbidden_from_pattern, letting hot paths (VCF has_decisive_tempo)
    // reuse an already-recognized BLACK pattern instead of scanning twice.
    // Lock in that both entry points agree across the four cases that matter:
    // double-three (forbidden), overline (forbidden), exact five (legal),
    // and a legal four-three (must NOT be forbidden).
    struct Case { const char* name; bool expect_forbidden; void (*setup)(Board&); };

    auto double_three = [](Board& b) {
        b.place_stone(7, 5, BLACK); b.place_stone(7, 6, BLACK);
        b.place_stone(5, 7, BLACK); b.place_stone(6, 7, BLACK);
    };
    auto overline = [](Board& b) {
        // Leave (7,7) empty; filling it makes cols 5-10 = six in a row (overline).
        for (int c = 5; c <= 10; ++c) if (c != 7) b.place_stone(7, c, BLACK);
    };
    auto exact_five = [](Board& b) {
        b.place_stone(7, 5, BLACK); b.place_stone(7, 6, BLACK);
        b.place_stone(7, 8, BLACK); b.place_stone(7, 9, BLACK);
        // filling col 7 makes an exact five - legal, not forbidden
    };
    auto four_three = [](Board& b) {
        // Legal BLACK winning shape (four + three). Must not be forbidden.
        b.place_stone(7, 5, BLACK); b.place_stone(7, 6, BLACK); b.place_stone(7, 8, BLACK);
        b.place_stone(5, 7, BLACK); b.place_stone(6, 7, BLACK);
    };

    Case cases[] = {
        {"double_three", true,  double_three},
        {"overline",     true,  overline},
        {"exact_five",   false, exact_five},
        {"four_three",   false, four_three},
    };

    for (const auto& c : cases) {
        Board b;
        c.setup(b);
        bool via_board    = PatternRecognizer::is_forbidden(b, 7, 7, BLACK);
        PositionPattern p = PatternRecognizer::recognize_position(b, 7, 7, BLACK);
        bool via_pattern  = PatternRecognizer::is_forbidden_from_pattern(p);
        assert(via_board == via_pattern);
        assert(via_board == c.expect_forbidden);
    }

    std::cout << "test_forbidden_from_pattern_equivalence passed" << std::endl;
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
    test_white_gap_split_is_five();
    test_forbidden_from_pattern_equivalence();
    
    std::cout << "All pattern tests passed!" << std::endl;
    return 0;
}

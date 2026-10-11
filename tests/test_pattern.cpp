#include "core/board.h"
#include "core/pattern.h"
#include <iostream>
#include <random>
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

void test_gap_shapes() {
    // R34 F3: the contiguous scan only counted the run through the placed
    // stone, so every single-hole shape was read one or two grades too weak.
    Board board;

    // XX_XX with the probe closing the right pair: four stones around one hole,
    // so the hole is the only five point - a rush four, not a two.
    Board a;
    a.place_stone(7, 3, BLACK);
    a.place_stone(7, 4, BLACK);
    a.place_stone(7, 6, BLACK);
    assert(PatternRecognizer::recognize_direction(a, 7, 7, BLACK, 0).type == PatternType::RUSH_FOUR);

    // X_XXX with the probe as the lone stone on the far side of the hole.
    Board b;
    b.place_stone(7, 5, BLACK);
    b.place_stone(7, 6, BLACK);
    b.place_stone(7, 7, BLACK);
    assert(PatternRecognizer::recognize_direction(b, 7, 9, BLACK, 0).type == PatternType::RUSH_FOUR);

    // A gap four stays a four even when both outer ends are blocked: the hole
    // completion is unaffected by what sits outside the shape.
    Board c;
    c.place_stone(7, 4, WHITE);
    c.place_stone(7, 5, BLACK);
    c.place_stone(7, 6, BLACK);
    c.place_stone(7, 8, BLACK);
    c.place_stone(7, 10, WHITE);
    PositionPattern blocked = PatternRecognizer::recognize_position(c, 7, 9, BLACK);
    assert(blocked.directions[0].type == PatternType::RUSH_FOUR);
    assert(blocked.rush_four_count == 1 && blocked.has_rush_four);

    // X_XX with room on both sides: filling the hole makes a live four, so this
    // is a live three (the Renju double-three forbidden counts these shapes).
    Board d;
    d.place_stone(7, 5, BLACK);
    d.place_stone(7, 6, BLACK);
    PositionPattern open_three = PatternRecognizer::recognize_position(d, 7, 8, BLACK);
    assert(open_three.directions[0].type == PatternType::LIVE_THREE);
    assert(open_three.live_three_count == 1);

    // Same shape with the far end taken away: the hole fill would only make a
    // blocked four, so it drops to a sleepy three.
    Board e;
    e.place_stone(7, 5, BLACK);
    e.place_stone(7, 6, BLACK);
    e.place_stone(7, 9, WHITE);
    assert(PatternRecognizer::recognize_position(e, 7, 8, BLACK).directions[0].type ==
           PatternType::SLEEPY_THREE);

    // Two stones around a hole are deliberately out of scope: X_X is only worth
    // a three after the hole is filled, so no label is claimed for it.
    Board f;
    f.place_stone(7, 6, BLACK);
    assert(PatternRecognizer::recognize_direction(f, 7, 8, BLACK, 0).type == PatternType::NONE);

    std::cout << "test_gap_shapes passed" << std::endl;
}

void test_overline_locked_four() {
    // R35 F3: for BLACK a four counts only if some completion point makes
    // EXACTLY five. Both cases below look like a four to the window matcher and
    // are not, once the fills are measured on the board.
    Board board;

    // X_XXXX with the probe at the right end of the run: filling left overruns
    // (3,4,5,6,7,8), filling right makes exactly five, so this is a rush four.
    board.place_stone(7, 3, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(7, 7, BLACK);
    assert(PatternRecognizer::recognize_direction(board, 7, 8, BLACK, 0).type ==
           PatternType::RUSH_FOUR);
    // WHITE has no overline rule, so the same shape is a live four for it.
    Board w;
    w.place_stone(7, 3, WHITE);
    w.place_stone(7, 5, WHITE);
    w.place_stone(7, 6, WHITE);
    w.place_stone(7, 7, WHITE);
    assert(PatternRecognizer::recognize_direction(w, 7, 8, WHITE, 0).type ==
           PatternType::LIVE_FOUR);

    // X_XXXX_X: every fill makes six, so BLACK holds no four here at all.
    Board locked;
    locked.place_stone(7, 3, BLACK);
    locked.place_stone(7, 5, BLACK);
    locked.place_stone(7, 6, BLACK);
    locked.place_stone(7, 7, BLACK);
    locked.place_stone(7, 10, BLACK);
    PositionPattern pattern = PatternRecognizer::recognize_position(locked, 7, 8, BLACK);
    assert(pattern.directions[0].type == PatternType::NONE);
    assert(pattern.four_count == 0);

    // A plain live four has two real five points and keeps its label.
    Board live;
    live.place_stone(7, 5, BLACK);
    live.place_stone(7, 6, BLACK);
    live.place_stone(7, 7, BLACK);
    assert(PatternRecognizer::recognize_direction(live, 7, 8, BLACK, 0).type ==
           PatternType::LIVE_FOUR);

    std::cout << "test_overline_locked_four passed" << std::endl;
}

void test_bitboard_matches_scalar() {
    // Phase 4.1 replaced the per-cell base-3 window scan with bitboard line
    // words plus an 18-bit pattern table. The table is generated from
    // match_pattern, so the decode side is equivalent by construction; what has
    // to hold up under testing is the window extraction - line choice, shift
    // alignment and the off-board wall mask for all four directions. Compare
    // every square of every board against the scalar oracle.
    auto compare_all = [](const Board& board, long long& checked) {
        for (int r = 0; r < BOARD_SIZE; ++r) {
            for (int c = 0; c < BOARD_SIZE; ++c) {
                for (int d = 0; d < 4; ++d) {
                    for (int side : {BLACK, WHITE}) {
                        const DirectionPattern fast =
                            PatternRecognizer::recognize_direction(board, r, c, side, d);
                        const DirectionPattern ref =
                            PatternRecognizer::recognize_direction_scalar(board, r, c, side, d);
                        assert(fast.type == ref.type);
                        assert(fast.direction == ref.direction);
                        ++checked;
                    }
                }
            }
        }
    };

    long long checked = 0;

    // Randomised midgame shapes.
    std::mt19937 rng(20261011);
    for (int game = 0; game < 60; ++game) {
        Board board;
        const int stones = 1 + static_cast<int>(rng() % 45);
        for (int i = 0; i < stones; ++i) {
            const int r = static_cast<int>(rng() % BOARD_SIZE);
            const int c = static_cast<int>(rng() % BOARD_SIZE);
            board.place_stone(r, c, i % 2 == 0 ? BLACK : WHITE);
        }
        compare_all(board, checked);
    }

    // Structured boards that put stones along every edge, corner and diagonal,
    // so windows clipped by the board boundary are covered deliberately rather
    // than by chance.
    for (int color = 1; color <= 2; ++color) {
        Board board;
        for (int i = 0; i < BOARD_SIZE; ++i) {
            board.place_stone(0, i, color);
            board.place_stone(i, 0, color);
            board.place_stone(BOARD_SIZE - 1, i, color);
            board.place_stone(i, BOARD_SIZE - 1, color);
            board.place_stone(i, i, color);
            board.place_stone(i, BOARD_SIZE - 1 - i, color);
            board.place_stone(7, i, color == BLACK ? WHITE : BLACK);
        }
        compare_all(board, checked);
    }

    assert(checked >= 100000);
    std::cout << "test_bitboard_matches_scalar passed (" << checked << " windows)" << std::endl;
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
    test_gap_shapes();
    test_overline_locked_four();
    test_forbidden_from_pattern_equivalence();
    test_bitboard_matches_scalar();
    
    std::cout << "All pattern tests passed!" << std::endl;
    return 0;
}

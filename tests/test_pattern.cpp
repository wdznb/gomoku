#include "core/board.h"
#include "core/pattern.h"
#include <array>
#include <iostream>
#include <random>
#include <vector>
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

// recognize_position_both is the shared-load fast path for callers that score
// one candidate for both colours (AlphaBeta's sort_moves, evaluate_board and
// evaluate_position_both). It has to answer exactly what the two single-colour
// calls answer, or move ordering and static evaluation quietly diverge from
// every branch that still recognizes one colour at a time.
static bool patterns_equal(const PositionPattern& a, const PositionPattern& b) {
    for (int d = 0; d < 4; ++d) {
        if (a.directions[d].type != b.directions[d].type) return false;
        if (a.directions[d].direction != b.directions[d].direction) return false;
    }
    return a.four_count == b.four_count &&
           a.three_count == b.three_count &&
           a.live_four_count == b.live_four_count &&
           a.rush_four_count == b.rush_four_count &&
           a.live_three_count == b.live_three_count &&
           a.has_four_four == b.has_four_four &&
           a.has_four_three == b.has_four_three &&
           a.has_three_three == b.has_three_three &&
           a.has_live_four == b.has_live_four &&
           a.has_rush_four == b.has_rush_four &&
           a.has_live_three == b.has_live_three;
}

void test_recognize_position_both_matches_pair() {
    auto compare_all = [](const Board& board, long long& checked) {
        for (int r = 0; r < BOARD_SIZE; ++r) {
            for (int c = 0; c < BOARD_SIZE; ++c) {
                for (int side : {BLACK, WHITE}) {
                    const PositionPattern ref_first =
                        PatternRecognizer::recognize_position(board, r, c, side);
                    const PositionPattern ref_second =
                        PatternRecognizer::recognize_position(board, r, c, opponent(side));

                    PositionPattern first, second;
                    PatternRecognizer::recognize_position_both(board, r, c, side, first, second);

                    assert(patterns_equal(ref_first, first));
                    assert(patterns_equal(ref_second, second));
                    ++checked;
                }
            }
        }
    };

    long long checked = 0;

    std::mt19937 rng(20261012);
    for (int game = 0; game < 40; ++game) {
        Board board;
        const int stones = 1 + static_cast<int>(rng() % 45);
        for (int i = 0; i < stones; ++i) {
            const int r = static_cast<int>(rng() % BOARD_SIZE);
            const int c = static_cast<int>(rng() % BOARD_SIZE);
            board.place_stone(r, c, i % 2 == 0 ? BLACK : WHITE);
        }
        compare_all(board, checked);
    }

    // Rows, columns and both diagonals fully packed, so the shared pass is
    // checked against runs that reach the window's ends and against the
    // colour-specific corrections (WHITE overline demotion, BLACK overline
    // promotion and the locked-four probe) on every square at once.
    for (int color = 1; color <= 2; ++color) {
        Board board;
        for (int i = 0; i < BOARD_SIZE; ++i) {
            board.place_stone(7, i, color);
            board.place_stone(i, 7, color);
            board.place_stone(i, i, color);
            board.place_stone(i, BOARD_SIZE - 1 - i, color);
        }
        compare_all(board, checked);
    }

    assert(checked >= 10000);
    std::cout << "test_recognize_position_both_matches_pair passed (" << checked
              << " squares)" << std::endl;
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

// Asymmetric live three: _BBB_. with ONE outer end blocked is still a live
// three, because the free end can grow a four with both ends free. The old
// matcher demanded both outer cells empty and demoted these to SLEEPY_THREE,
// which also hid the three-three forbidden point built from two such shapes.
void test_asymmetric_live_three() {
    // Black (7,5)(7,6), white (7,9), probe (7,7): the right outer end (7,9) is
    // blocked but (7,3)(7,4) is free - the three can still become a live four.
    Board right_blocked;
    right_blocked.place_stone(7, 5, BLACK);
    right_blocked.place_stone(7, 6, BLACK);
    right_blocked.place_stone(7, 9, WHITE);
    assert(PatternRecognizer::recognize_direction(right_blocked, 7, 7, BLACK, 0).type ==
           PatternType::LIVE_THREE);

    // Mirror image: white blocks (7,3) and the right side is free.
    Board left_blocked;
    left_blocked.place_stone(7, 5, BLACK);
    left_blocked.place_stone(7, 6, BLACK);
    left_blocked.place_stone(7, 3, WHITE);
    assert(PatternRecognizer::recognize_direction(left_blocked, 7, 7, BLACK, 0).type ==
           PatternType::LIVE_THREE);

    // Both outer ends blocked: filling either space makes a four that is dead
    // at both ends, so this must stay a sleepy three.
    Board both_blocked;
    both_blocked.place_stone(7, 5, BLACK);
    both_blocked.place_stone(7, 6, BLACK);
    both_blocked.place_stone(7, 3, WHITE);
    both_blocked.place_stone(7, 9, WHITE);
    assert(PatternRecognizer::recognize_direction(both_blocked, 7, 7, BLACK, 0).type ==
           PatternType::SLEEPY_THREE);

    // Two such asymmetric live threes meeting on one point are a double-three:
    // under the old matcher neither counted as a live three and the point was
    // not forbidden for BLACK.
    Board double_three;
    double_three.place_stone(7, 5, BLACK);
    double_three.place_stone(7, 6, BLACK);
    double_three.place_stone(5, 7, BLACK);
    double_three.place_stone(6, 7, BLACK);
    double_three.place_stone(7, 9, WHITE);
    double_three.place_stone(3, 7, WHITE);
    const PositionPattern pattern =
        PatternRecognizer::recognize_position(double_three, 7, 7, BLACK);
    int live_threes = 0;
    for (int d = 0; d < 4; ++d) {
        if (pattern.directions[d].type == PatternType::LIVE_THREE) ++live_threes;
    }
    assert(live_threes >= 2);
    assert(pattern.has_three_three);
    assert(PatternRecognizer::is_forbidden(double_three, 7, 7, BLACK));
    // The same point as WHITE partakes in no forbidden shape.
    assert(!PatternRecognizer::is_forbidden(double_three, 7, 7, WHITE));

    // The hole-split three through the probe (X_XX) keeps its own label and its
    // one-blocked variant stays sleepy - match_gapped_run is untouched.
    Board gapped;
    gapped.place_stone(7, 5, BLACK);
    gapped.place_stone(7, 6, BLACK);
    assert(PatternRecognizer::recognize_direction(gapped, 7, 8, BLACK, 0).type ==
           PatternType::LIVE_THREE);
    Board gapped_blocked;
    gapped_blocked.place_stone(7, 5, BLACK);
    gapped_blocked.place_stone(7, 6, BLACK);
    gapped_blocked.place_stone(7, 9, WHITE);
    assert(PatternRecognizer::recognize_direction(gapped_blocked, 7, 8, BLACK, 0).type ==
           PatternType::SLEEPY_THREE);

    std::cout << "test_asymmetric_live_three passed" << std::endl;
}

static const char* pattern_name(PatternType type) {
    switch (type) {
        case PatternType::NONE:         return "NONE";
        case PatternType::SLEEPY_TWO:   return "SLEEPY_TWO";
        case PatternType::LIVE_TWO:     return "LIVE_TWO";
        case PatternType::SLEEPY_THREE: return "SLEEPY_THREE";
        case PatternType::LIVE_THREE:   return "LIVE_THREE";
        case PatternType::RUSH_FOUR:    return "RUSH_FOUR";
        case PatternType::LIVE_FOUR:    return "LIVE_FOUR";
        case PatternType::FIVE:         return "FIVE";
        case PatternType::LONG_FIVE:    return "LONG_FIVE";
    }
    return "?";
}

// R77 R1: the phantom four at the window edge. BLACK holds (7,2)..(7,5) with
// (7,6) empty and the probe lands on (7,7). The 9-cell window only sees cols
// 3..5 plus the probe, so the gapped matcher reads XX_X and calls it a rush
// four. The old code skipped the five-point walk whenever the run through the
// probe was not four stones, so the label survived even though filling (7,6)
// connects cols 2..7 - a six, i.e. an overline, not a five. The walk now always
// runs for BLACK fours, and a completion-less "four" is demoted to NONE.
void test_window_edge_gapped_four() {
    // The phantom: (7,2) sits outside the window, so nothing in the window can
    // see that the hole completion overruns.
    Board phantom;
    phantom.place_stone(7, 2, BLACK);
    phantom.place_stone(7, 3, BLACK);
    phantom.place_stone(7, 4, BLACK);
    phantom.place_stone(7, 5, BLACK);
    const DirectionPattern edge =
        PatternRecognizer::recognize_direction(phantom, 7, 7, BLACK, 0);
    std::cout << "  (7,2)(7,3)(7,4)(7,5) probe (7,7) dir (0,1): "
              << pattern_name(edge.type) << std::endl;
    assert(edge.type == PatternType::NONE);
    const PositionPattern whole = PatternRecognizer::recognize_position(phantom, 7, 7, BLACK);
    assert(whole.directions[0].type == PatternType::NONE);
    assert(whole.four_count == 0 && !whole.has_rush_four && !whole.has_live_four);
    assert(whole.has_four_four == false && whole.has_four_three == false);

    // Control: drop the out-of-window stone and the same probe IS a rush four -
    // filling (7,6) now makes exactly five (cols 3..7).
    Board control;
    control.place_stone(7, 3, BLACK);
    control.place_stone(7, 4, BLACK);
    control.place_stone(7, 5, BLACK);
    const DirectionPattern kept =
        PatternRecognizer::recognize_direction(control, 7, 7, BLACK, 0);
    std::cout << "  control (7,3)..(7,5) probe (7,7) dir (0,1): "
              << pattern_name(kept.type) << std::endl;
    assert(kept.type == PatternType::RUSH_FOUR);

    // (a) A true live four keeps both of its five points: filling either end of
    // XXXX_ still makes exactly five.
    Board live;
    live.place_stone(7, 4, BLACK);
    live.place_stone(7, 5, BLACK);
    live.place_stone(7, 6, BLACK);
    const DirectionPattern live_four =
        PatternRecognizer::recognize_direction(live, 7, 7, BLACK, 0);
    std::cout << "  live four (7,4)..(7,6) probe (7,7) dir (0,1): "
              << pattern_name(live_four.type) << std::endl;
    assert(live_four.type == PatternType::LIVE_FOUR);

    // (b) The one-five-point demotion the walk already performed is not lost:
    // with (7,2) added, the left completion overruns (cols 2..7) and only
    // (7,8) still wins, so the same live four becomes a rush four.
    Board one_point;
    one_point.place_stone(7, 2, BLACK);
    one_point.place_stone(7, 4, BLACK);
    one_point.place_stone(7, 5, BLACK);
    one_point.place_stone(7, 6, BLACK);
    const DirectionPattern demoted =
        PatternRecognizer::recognize_direction(one_point, 7, 7, BLACK, 0);
    std::cout << "  live four + (7,2) probe (7,7) dir (0,1): "
              << pattern_name(demoted.type) << std::endl;
    assert(demoted.type == PatternType::RUSH_FOUR);

    // (c) WHITE is not subject to the overline rule, so the same phantom shape
    // is a real winning threat for WHITE and keeps its label.
    Board white;
    white.place_stone(7, 2, WHITE);
    white.place_stone(7, 3, WHITE);
    white.place_stone(7, 4, WHITE);
    white.place_stone(7, 5, WHITE);
    const DirectionPattern white_four =
        PatternRecognizer::recognize_direction(white, 7, 7, WHITE, 0);
    std::cout << "  WHITE (7,2)..(7,5) probe (7,7) dir (0,1): "
              << pattern_name(white_four.type) << std::endl;
    assert(white_four.type == PatternType::RUSH_FOUR);

    std::cout << "test_window_edge_gapped_four passed" << std::endl;
}

// Round 80, B1: a BLACK LIVE_THREE now gets a board-level correction the same way
// a BLACK four already did. The recognition window only reads +/-4 cells, so
// (7,2)(7,5)(7,6)(7,9)(7,10) around the probe (7,7) labels LIVE_THREE even though
// every fill runs past five - and a fabricated second live three turns the point
// into a phantom 三三 forbidden. WHITE is not subject to the overline rule.
static Board b1_board(const std::vector<std::array<int, 3>>& stones) {
    Board board;
    for (const std::array<int, 3>& s : stones) board.place_stone(s[0], s[1], s[2]);
    return board;
}

void test_black_live_three_board_correction() {
    const int B = BLACK, W = WHITE;

    // The counterexample: horizontal shape X_XX_XX_X with the probe in the hole,
    // plus a genuine vertical live three through (6,7)(8,7).
    const Board counter = b1_board({{7, 2, B}, {7, 5, B}, {7, 6, B}, {7, 9, B}, {7, 10, B},
                                    {6, 7, B}, {8, 7, B}});
    const PositionPattern downgraded =
        PatternRecognizer::recognize_position(counter, 7, 7, BLACK);
    // All four directions, not just the total: only the horizontal one is demoted.
    assert(downgraded.directions[0].type == PatternType::SLEEPY_THREE);
    assert(downgraded.directions[1].type == PatternType::LIVE_THREE);
    assert(downgraded.directions[2].type == PatternType::NONE);
    assert(downgraded.directions[3].type == PatternType::NONE);
    assert(downgraded.live_three_count == 1);
    assert(downgraded.has_three_three == false);
    assert(PatternRecognizer::is_forbidden(counter, 7, 7, BLACK) == false);

    // WHITE reads the same shape untouched: the overline is a win there, and the
    // forbidden rules only ever bind BLACK.
    const Board white_same = b1_board({{7, 2, W}, {7, 5, W}, {7, 6, W}, {7, 9, W}, {7, 10, W},
                                       {6, 7, W}, {8, 7, W}});
    const PositionPattern white =
        PatternRecognizer::recognize_position(white_same, 7, 7, WHITE);
    assert(white.directions[0].type == PatternType::LIVE_THREE);
    assert(white.directions[1].type == PatternType::LIVE_THREE);
    assert(white.live_three_count == 2);
    assert(PatternRecognizer::is_forbidden(white_same, 7, 7, WHITE) == false);

    // Positive shapes must not regress: an open _XXX_ on an empty board is still a
    // live three, and one blocked at both ends is still sleepy.
    const Board open_three = b1_board({{7, 5, B}, {7, 6, B}});
    assert(PatternRecognizer::recognize_position(open_three, 7, 7, BLACK)
               .directions[0].type == PatternType::LIVE_THREE);
    const Board dead_three = b1_board({{7, 5, B}, {7, 6, B}, {7, 3, W}, {7, 9, W}});
    assert(PatternRecognizer::recognize_position(dead_three, 7, 7, BLACK)
               .directions[0].type == PatternType::SLEEPY_THREE);

    // Two real live threes sharing the probe are still a forbidden 三三.
    const Board real_double = b1_board({{7, 5, B}, {7, 6, B}, {6, 7, B}, {8, 7, B}});
    const PositionPattern double_three =
        PatternRecognizer::recognize_position(real_double, 7, 7, BLACK);
    assert(double_three.directions[0].type == PatternType::LIVE_THREE);
    assert(double_three.directions[1].type == PatternType::LIVE_THREE);
    assert(double_three.live_three_count == 2);
    assert(double_three.has_three_three == true);
    assert(PatternRecognizer::is_forbidden(real_double, 7, 7, BLACK) == true);

    // The four branch is byte-for-byte the old behaviour.
    const Board live_four = b1_board({{7, 4, B}, {7, 5, B}, {7, 6, B}});
    assert(PatternRecognizer::recognize_position(live_four, 7, 7, BLACK)
               .directions[0].type == PatternType::LIVE_FOUR);
    const Board hole_four = b1_board({{7, 3, B}, {7, 4, B}, {7, 5, B}, {7, 6, B}, {7, 8, B}});
    const PositionPattern hole = PatternRecognizer::recognize_position(hole_four, 7, 7, BLACK);
    assert(hole.directions[0].type == PatternType::LONG_FIVE);
    assert(PatternRecognizer::is_forbidden(hole_four, 7, 7, BLACK) == true);
    const Board gapped_four = b1_board({{7, 4, B}, {7, 5, B}, {7, 7, B}, {7, 8, B}});
    assert(PatternRecognizer::recognize_position(gapped_four, 7, 6, BLACK)
               .directions[0].type == PatternType::FIVE);

    // The essence is that the blocker is outside the window: (7,2) is exactly five
    // cells from the probe (7,7), so no window cell reaches it. Same shape without
    // that stone is a real live three again, because both extensions do make five.
    const Board with_blocker = b1_board({{7, 5, B}, {7, 6, B}, {7, 10, B},
                                         {6, 7, B}, {8, 7, B}, {7, 2, B}});
    const Board without_blocker = b1_board({{7, 5, B}, {7, 6, B}, {7, 10, B},
                                            {6, 7, B}, {8, 7, B}});
    assert(PatternRecognizer::recognize_position(without_blocker, 7, 7, BLACK)
               .directions[0].type == PatternType::LIVE_THREE);
    assert(PatternRecognizer::recognize_position(without_blocker, 7, 7, BLACK)
               .live_three_count == 2);
    assert(PatternRecognizer::is_forbidden(without_blocker, 7, 7, BLACK) == true);
    assert(PatternRecognizer::recognize_position(with_blocker, 7, 7, BLACK)
               .directions[0].type == PatternType::SLEEPY_THREE);
    assert(PatternRecognizer::is_forbidden(with_blocker, 7, 7, BLACK) == false);
    // A blocker six cells out (7,1) is past the reach of any five the shape can
    // make, so the same horizontal group keeps its live label.
    const Board blocker_6 = b1_board({{7, 5, B}, {7, 6, B}, {7, 10, B},
                                      {6, 7, B}, {8, 7, B}, {7, 1, B}});
    assert(PatternRecognizer::recognize_position(blocker_6, 7, 7, BLACK)
               .directions[0].type == PatternType::LIVE_THREE);

    std::cout << "test_black_live_three_board_correction passed" << std::endl;
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
    test_recognize_position_both_matches_pair();
    test_bitboard_matches_scalar();
    test_asymmetric_live_three();
    test_window_edge_gapped_four();
    test_black_live_three_board_correction();
    
    std::cout << "All pattern tests passed!" << std::endl;
    return 0;
}

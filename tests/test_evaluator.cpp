#include "core/board.h"
#include "core/pattern.h"
// pattern_score is private; the forbidden-guard tests below re-derive what the
// pre-round-81 aggregate summed, so the class body has to be readable.
#define private public
#include "core/evaluator.h"
#undef private
#include <iostream>
#include <cassert>

using namespace gomoku;

// What evaluate_board computes, re-derived from the board: every candidate's
// pattern_score on both sides unconditionally (that is, without the per-side
// forbidden guard), then the two positional terms taken as a difference between
// the colours. The forbidden-guard cases below subtract exactly the black term
// they claim to drop, so this has to carry everything else the function does.
static long long naive_board_total(const Board& board, int my_color, const Evaluator& evaluator) {
    long long total = 0;
    for (const Position& pos : board.active_empty()) {
        PositionPattern mine, theirs;
        PatternRecognizer::recognize_position_both(board, pos.row, pos.col, my_color, mine, theirs);
        total += evaluator.pattern_score(mine, pos.row, pos.col) -
                 evaluator.pattern_score(theirs, pos.row, pos.col);
    }
    for (const Move& move : board.move_history()) {
        const int centre = evaluator.get_position_weight(move.pos.row, move.pos.col) *
                           evaluator.coeffs_.distance_to_stones;
        const int closed =
            ((move.pos.row <= 1 || move.pos.row >= BOARD_SIZE - 2) ? 1 : 0) +
            ((move.pos.col <= 1 || move.pos.col >= BOARD_SIZE - 2) ? 1 : 0);
        const long long term =
            static_cast<long long>(centre) - static_cast<long long>(closed) * evaluator.coeffs_.edge_penalty;
        total += (move.color == my_color) ? term : -term;
    }
    return total;
}

// evaluate_board caps the aggregate at five-1 so a busy board cannot pass itself
// off as a mate score. The cap predates the round-81 guard and is untouched by
// it, so a "nothing changed" comparison has to apply it to the raw sum too.
static long long clamp_board_diff(long long diff, const Evaluator& evaluator) {
    const long long cap = static_cast<long long>(evaluator.coeffs_.five) - 1;
    if (diff > cap) return cap;
    if (diff < -cap) return -cap;
    return diff;
}

// Candidates whose BLACK-side pattern is a forbidden shape.
static int black_forbidden_candidates(const Board& board, const Evaluator& evaluator,
                                      int* first_black_ps = nullptr, int* first_white_ps = nullptr,
                                      Position* first_pos = nullptr) {
    int count = 0;
    for (const Position& pos : board.active_empty()) {
        PositionPattern mine, theirs;
        PatternRecognizer::recognize_position_both(board, pos.row, pos.col, BLACK, mine, theirs);
        if (!PatternRecognizer::is_forbidden_from_pattern(mine)) continue;
        if (count == 0) {
            if (first_black_ps) *first_black_ps = evaluator.pattern_score(mine, pos.row, pos.col);
            if (first_white_ps) *first_white_ps = evaluator.pattern_score(theirs, pos.row, pos.col);
            if (first_pos) *first_pos = pos;
        }
        ++count;
    }
    return count;
}

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

void test_set_coefficients_rebuilds_position_weights() {
    // Round 34 F5: set_coefficients must rebuild position_weights_, which are
    // derived from coeffs_.position_weight_scale at construction. Without the
    // rebuild a runtime coefficient change leaves the center-bias table stale.
    ThreatCoefficients base;
    Evaluator evaluator(base);
    int center_before = evaluator.get_position_weight(7, 7);

    ThreatCoefficients scaled = base;
    scaled.position_weight_scale = base.position_weight_scale * 3;
    evaluator.set_coefficients(scaled);

    int center_after = evaluator.get_position_weight(7, 7);
    assert(center_after == center_before * 3);

    std::cout << "test_set_coefficients_rebuilds_position_weights passed" << std::endl;
}

// Round 81, E1: pattern_score pays out double_four / double_three / four_three,
// so a shape BLACK may never legally play banked points for a stone the move
// generator refuses to place. evaluate_board has to zero that one term and
// nothing else - no forbidden_penalty is introduced.
void test_evaluate_board_skips_black_forbidden_point() {
    Evaluator evaluator;
    Board board;
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(5, 7, BLACK);
    board.place_stone(6, 7, BLACK);
    assert(PatternRecognizer::is_forbidden(board, 7, 7, BLACK));

    int black_ps = 0, white_ps = 0;
    Position spot{0, 0};
    assert(black_forbidden_candidates(board, evaluator, &black_ps, &white_ps, &spot) == 1);
    assert(spot.row == 7 && spot.col == 7);
    assert(black_ps == 31000);   // double_three 30000 + two live three 2x500
    assert(white_ps == 0);

    const long long before = naive_board_total(board, BLACK, evaluator);
    // 38300 of shape sum plus the four stones' centre term (96+104+96+104 = 400).
    assert(before == 38700);
    const int after_black = evaluator.evaluate_board(board, BLACK);
    assert(after_black == 7700);
    assert(after_black == static_cast<int>(before - black_ps));
    assert(evaluator.evaluate_board(board, WHITE) == -7700);
    std::cout << "test_evaluate_board_skips_black_forbidden_point passed ("
              << before << " -> " << after_black << ")" << std::endl;
}

void test_evaluate_board_double_four_point() {
    Evaluator evaluator;
    Board board;
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(4, 7, BLACK);
    board.place_stone(5, 7, BLACK);
    board.place_stone(6, 7, BLACK);
    assert(PatternRecognizer::is_forbidden(board, 7, 7, BLACK));

    int black_ps = 0;
    assert(black_forbidden_candidates(board, evaluator, &black_ps) == 1);
    assert(black_ps == 70000);  // double_four 50000 + two live four 2x10000
    const long long before = naive_board_total(board, BLACK, evaluator);
    // 99800 of shape sum plus 576 of centre term. The raw sum runs past the mate
    // cap; only the guarded value below is compared, and it lands under the cap.
    assert(before == 100376);
    const int after_black = evaluator.evaluate_board(board, BLACK);
    assert(after_black == 30376);
    assert(after_black == static_cast<int>(before - black_ps));
    std::cout << "test_evaluate_board_double_four_point passed ("
              << before << " -> " << after_black << ")" << std::endl;
}

// The square can be forbidden for BLACK and a real shape for WHITE: only the
// black side of that candidate drops to zero.
void test_evaluate_board_keeps_white_value_on_forbidden_square() {
    Evaluator evaluator;
    Board board;
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(5, 7, BLACK);
    board.place_stone(6, 7, BLACK);
    board.place_stone(8, 8, WHITE);
    board.place_stone(9, 9, WHITE);

    int black_ps = 0, white_ps = 0;
    assert(PatternRecognizer::is_forbidden(board, 7, 7, BLACK));
    assert(black_forbidden_candidates(board, evaluator, &black_ps, &white_ps) == 1);
    assert(black_ps == 31000);
    assert(white_ps == 500);  // WHITE's own 活三 through (7,7)(8,8)(9,9)

    const long long before = naive_board_total(board, BLACK, evaluator);
    // 35700 of shape sum, plus BLACK's 400 centre term minus WHITE's 176.
    assert(before == 35924);
    const int after_black = evaluator.evaluate_board(board, BLACK);
    assert(after_black == 4924);
    // Exactly the black term vanished; WHITE's 500 is still in the total.
    assert(before - after_black == 31000);
    assert(evaluator.evaluate_board(board, WHITE) == -4924);
    std::cout << "test_evaluate_board_keeps_white_value_on_forbidden_square passed ("
              << before << " -> " << after_black << ", white term kept)" << std::endl;
}

// A position with no black forbidden candidate must score identically to the
// unguarded sum, so the guard changes nothing for ordinary boards.
void test_evaluate_board_unchanged_without_forbidden() {
    Evaluator evaluator;

    Board plain;
    plain.place_stone(7, 7, BLACK);
    plain.place_stone(7, 8, WHITE);
    plain.place_stone(8, 8, BLACK);
    plain.place_stone(6, 6, WHITE);
    plain.place_stone(9, 9, BLACK);
    plain.place_stone(5, 5, WHITE);
    assert(black_forbidden_candidates(plain, evaluator) == 0);
    assert(evaluator.evaluate_board(plain, BLACK) == 1708);
    assert(evaluator.evaluate_board(plain, BLACK) ==
           clamp_board_diff(naive_board_total(plain, BLACK, evaluator), evaluator));
    assert(evaluator.evaluate_board(plain, WHITE) ==
           clamp_board_diff(naive_board_total(plain, WHITE, evaluator), evaluator));

    // The round-32 mate-in-one board, unchanged too: WHITE's four is the threat
    // and BLACK has no forbidden shape anywhere. Its raw sum runs past the mate
    // cap, which is why the comparison below clamps.
    Board threat;
    threat.place_stone(7, 4, WHITE);
    threat.place_stone(7, 5, WHITE);
    threat.place_stone(7, 6, WHITE);
    threat.place_stone(7, 7, WHITE);
    threat.place_stone(9, 4, BLACK);
    threat.place_stone(9, 5, BLACK);
    assert(black_forbidden_candidates(threat, evaluator) == 0);
    const int threat_value = evaluator.evaluate_board(threat, BLACK);
    assert(threat_value ==
           clamp_board_diff(naive_board_total(threat, BLACK, evaluator), evaluator));
    assert(threat_value == -99999);
    std::cout << "test_evaluate_board_unchanged_without_forbidden passed ("
              << 1708 << ", " << threat_value << ")" << std::endl;
}

// WHITE is never judged: the same shape in WHITE's hands keeps its 31000, from
// both perspectives.
void test_evaluate_board_never_judges_white() {
    Evaluator evaluator;
    Board board;
    board.place_stone(7, 5, WHITE);
    board.place_stone(7, 6, WHITE);
    board.place_stone(5, 7, WHITE);
    board.place_stone(6, 7, WHITE);
    assert(!PatternRecognizer::is_forbidden(board, 7, 7, WHITE));
    assert(black_forbidden_candidates(board, evaluator) == 0);

    int white_ps = 0;
    {
        PositionPattern mine, theirs;
        PatternRecognizer::recognize_position_both(board, 7, 7, BLACK, mine, theirs);
        // From BLACK's seat the white shape is the opponent term; it must be
        // worth its full double_three.
        white_ps = evaluator.pattern_score(theirs, 7, 7);
    }
    assert(white_ps == 31000);
    assert(evaluator.evaluate_board(board, WHITE) == 38700);
    assert(evaluator.evaluate_board(board, WHITE) == naive_board_total(board, WHITE, evaluator));
    assert(evaluator.evaluate_board(board, BLACK) == -38700);
    assert(evaluator.evaluate_board(board, BLACK) == naive_board_total(board, BLACK, evaluator));
    std::cout << "test_evaluate_board_never_judges_white passed (white keeps " << white_ps << ")" << std::endl;
}

// Round 89, T1: candidate_value takes the colour now, so the single-square
// entries judge a shape BLACK may never legally play the same way evaluate_board
// already did - and they judge nothing else. The (7,7) centre bias below is 112,
// the (7,8) one is 104, both untouched by the gate.
void test_candidate_value_zeroes_black_forbidden_shape() {
    Evaluator evaluator;

    // Four-four: three black stones in row 7 and three in column 7.
    Board four_four;
    four_four.place_stone(7, 4, BLACK);
    four_four.place_stone(7, 5, BLACK);
    four_four.place_stone(7, 6, BLACK);
    four_four.place_stone(4, 7, BLACK);
    four_four.place_stone(5, 7, BLACK);
    four_four.place_stone(6, 7, BLACK);
    assert(PatternRecognizer::is_forbidden(four_four, 7, 7, BLACK));
    assert(evaluator.evaluate_position(four_four, 7, 7, BLACK) == 0);
    assert(evaluator.evaluate_position(four_four, 7, 7, WHITE) == 112);
    // The pair is the two single-colour reads summed, so the black half leaving
    // takes exactly its own value with it - from either seat.
    assert(evaluator.evaluate_position_pair(four_four, 7, 7, WHITE) == 112);
    assert(evaluator.evaluate_position_pair(four_four, 7, 7, BLACK) == 112);
    assert(evaluator.evaluate_position_pair(four_four, 7, 7, WHITE) ==
           evaluator.evaluate_position(four_four, 7, 7, BLACK) +
               evaluator.evaluate_position(four_four, 7, 7, WHITE));
    // The neighbour is an ordinary candidate on the very same board: 1050 of shape
    // plus the square's own 104 centre bias.
    assert(evaluator.evaluate_position(four_four, 7, 8, BLACK) == 1154);
    assert(evaluator.evaluate_position(four_four, 7, 8, WHITE) == 104);
    assert(evaluator.evaluate_position_pair(four_four, 7, 8, WHITE) == 1258);

    // Double three: two black stones each way.
    Board three_three;
    three_three.place_stone(7, 5, BLACK);
    three_three.place_stone(7, 6, BLACK);
    three_three.place_stone(5, 7, BLACK);
    three_three.place_stone(6, 7, BLACK);
    assert(PatternRecognizer::is_forbidden(three_three, 7, 7, BLACK));
    assert(evaluator.evaluate_position(three_three, 7, 7, BLACK) == 0);
    assert(evaluator.evaluate_position(three_three, 7, 7, WHITE) == 112);

    // Overline: five black stones in row 7, so (7,7) would run past five.
    Board overline;
    overline.place_stone(7, 2, BLACK);
    overline.place_stone(7, 3, BLACK);
    overline.place_stone(7, 4, BLACK);
    overline.place_stone(7, 5, BLACK);
    overline.place_stone(7, 6, BLACK);
    assert(PatternRecognizer::is_forbidden(overline, 7, 7, BLACK));
    assert(evaluator.evaluate_position(overline, 7, 7, BLACK) == 0);
    assert(evaluator.evaluate_position(overline, 7, 7, WHITE) == 112);

    // Three live fours and no five: still forbidden.
    Board three_fours;
    three_fours.place_stone(7, 4, BLACK);
    three_fours.place_stone(7, 5, BLACK);
    three_fours.place_stone(7, 6, BLACK);
    three_fours.place_stone(5, 7, BLACK);
    three_fours.place_stone(6, 7, BLACK);
    three_fours.place_stone(8, 7, BLACK);
    three_fours.place_stone(5, 5, BLACK);
    three_fours.place_stone(6, 6, BLACK);
    three_fours.place_stone(8, 8, BLACK);
    assert(PatternRecognizer::is_forbidden(three_fours, 7, 7, BLACK));
    assert(evaluator.evaluate_position(three_fours, 7, 7, BLACK) == 0);

    // Anti-overreach: a single live four is a legal BLACK shape and keeps its
    // whole price.
    Board live_four;
    live_four.place_stone(7, 4, BLACK);
    live_four.place_stone(7, 5, BLACK);
    live_four.place_stone(7, 6, BLACK);
    assert(!PatternRecognizer::is_forbidden(live_four, 7, 7, BLACK));
    assert(evaluator.evaluate_position(live_four, 7, 7, BLACK) == 10112);
    assert(evaluator.evaluate_position_pair(live_four, 7, 7, BLACK) == 10224);

    std::cout << "test_candidate_value_zeroes_black_forbidden_shape passed ("
              << evaluator.evaluate_position(four_four, 7, 7, BLACK) << ", white keeps "
              << evaluator.evaluate_position(four_four, 7, 7, WHITE) << ")" << std::endl;
}

// The gate sits behind the exact-five early return (README:27/29 - a five is not
// a forbidden shape), so BLACK's winning point still pays coeffs.five. This whole
// case is green on HEAD too: it is the proof the new test did not reprioritise
// the two checks.
void test_exact_five_outranks_the_forbidden_gate() {
    Evaluator evaluator;
    const int five = ThreatCoefficients().five;

    Board plain_five;
    plain_five.place_stone(7, 4, BLACK);
    plain_five.place_stone(7, 5, BLACK);
    plain_five.place_stone(7, 6, BLACK);
    plain_five.place_stone(7, 7, BLACK);
    assert(!PatternRecognizer::is_forbidden(plain_five, 7, 3, BLACK));
    assert(evaluator.evaluate_position(plain_five, 7, 3, BLACK) == five);

    // A square whose own pattern carries the double-four flag *and* an exact
    // five: four in row 7 (the five), plus live fours in column 7 and on the
    // main diagonal. has_four_four is set, yet the point is legal and scores the
    // mate value, never zero.
    Board five_and_double_four;
    five_and_double_four.place_stone(7, 3, BLACK);
    five_and_double_four.place_stone(7, 4, BLACK);
    five_and_double_four.place_stone(7, 5, BLACK);
    five_and_double_four.place_stone(7, 6, BLACK);
    five_and_double_four.place_stone(5, 7, BLACK);
    five_and_double_four.place_stone(6, 7, BLACK);
    five_and_double_four.place_stone(8, 7, BLACK);
    five_and_double_four.place_stone(5, 5, BLACK);
    five_and_double_four.place_stone(6, 6, BLACK);
    five_and_double_four.place_stone(8, 8, BLACK);
    const PositionPattern shape =
        PatternRecognizer::recognize_position(five_and_double_four, 7, 7, BLACK);
    assert(shape.has_four_four);
    assert(!PatternRecognizer::is_forbidden_from_pattern(shape));
    assert(!PatternRecognizer::is_forbidden(five_and_double_four, 7, 7, BLACK));
    assert(evaluator.evaluate_position(five_and_double_four, 7, 7, BLACK) == five);
    assert(evaluator.evaluate_position_pair(five_and_double_four, 7, 7, BLACK) == five + 112);

    std::cout << "test_exact_five_outranks_the_forbidden_gate passed (" << five << ")"
              << std::endl;
}

// Engine redesign A5 (design doc 2.5.2): the centre preference has to be in the
// whole-board value, not only in the candidate ordering. The same three stones on
// the same shape read lower once they stand in the corner.
void test_evaluate_board_prefers_centre_stones() {
    Evaluator evaluator;

    Board centre;
    centre.place_stone(7, 5, BLACK);
    centre.place_stone(7, 6, BLACK);
    centre.place_stone(7, 7, BLACK);

    // The identical shape, shifted six rows up and four columns left: every run,
    // every gap and every flank cell keeps its relation to the stones.
    Board corner;
    corner.place_stone(1, 1, BLACK);
    corner.place_stone(1, 2, BLACK);
    corner.place_stone(1, 3, BLACK);

    const int centre_value = evaluator.evaluate_board(centre, BLACK);
    const int corner_value = evaluator.evaluate_board(corner, BLACK);
    assert(centre_value > corner_value);

    // Which half of the difference is which: the stones themselves are worth less
    // away from the middle, and the corner copy is the one the edge term bites.
    assert(evaluator.position_term(centre, BLACK) > evaluator.position_term(corner, BLACK));
    assert(evaluator.edge_penalty_term(centre, BLACK) == 0);
    assert(evaluator.edge_penalty_term(corner, BLACK) > 0);

    // And the preference runs the other way for the player facing it: the same
    // stones belong to the opponent in WHITE's reading.
    assert(evaluator.evaluate_board(centre, WHITE) < evaluator.evaluate_board(corner, WHITE));

    std::cout << "test_evaluate_board_prefers_centre_stones passed ("
              << centre_value << " vs " << corner_value << ")" << std::endl;
}

// Both new terms are taken as a difference between the colours, so the algebraic
// symmetry the round-81 contract leans on still holds with them in.
void test_evaluate_board_antisymmetric_with_terms() {
    Evaluator evaluator;

    struct Pose {
        const char* name;
        int stones[6][3];
        int count;
    };
    const Pose poses[] = {
        {"edge shape", {{0, 0, BLACK}, {0, 1, BLACK}, {0, 2, BLACK}, {7, 10, WHITE}, {8, 10, WHITE}, {0, 0, 0}}, 5},
        {"forbidden point", {{7, 5, BLACK}, {7, 6, BLACK}, {5, 7, BLACK}, {6, 7, BLACK}, {8, 8, WHITE}, {0, 0, 0}}, 5},
        {"centre fight", {{7, 7, BLACK}, {7, 8, WHITE}, {8, 8, BLACK}, {6, 6, WHITE}, {9, 9, BLACK}, {5, 5, WHITE}}, 6},
    };

    for (const Pose& pose : poses) {
        Board board;
        for (int i = 0; i < pose.count; ++i) {
            assert(board.place_stone(pose.stones[i][0], pose.stones[i][1], pose.stones[i][2]));
        }
        const int black = evaluator.evaluate_board(board, BLACK);
        const int white = evaluator.evaluate_board(board, WHITE);
        // Not a tautology about zeros: each of these boards has stones on it and
        // the positional terms make both readings non-zero.
        assert(black != 0);
        assert(black == -white);
        std::cout << "  " << pose.name << ": " << black << " / " << white << std::endl;
    }

    std::cout << "test_evaluate_board_antisymmetric_with_terms passed" << std::endl;
}

// Engine redesign A5 (design doc 2.5.4): a live shape whose line runs out of board
// inside one cell is discounted - the shape table reads that wall as a blocked
// cell, so the correction comes from where the candidate sits. It stays a
// discount: small enough that it can never promote a 冲四 above a real one.
void test_edge_penalty_below_rush_four() {
    Evaluator evaluator;
    const ThreatCoefficients& c = evaluator.coefficients();

    // 活三 with a single cell of flank before the wall, at (7,1): 100 off one end.
    Board three_wall;
    three_wall.place_stone(7, 2, BLACK);
    three_wall.place_stone(7, 3, BLACK);
    PositionPattern three, unused;
    PatternRecognizer::recognize_position_both(three_wall, 7, 1, BLACK, three, unused);
    assert(evaluator.pattern_score(three, 7, 1) == c.live_three - c.edge_penalty);
    // The very same shape read at the middle of an open row keeps its full price.
    assert(evaluator.pattern_score(three, 7, 7) == c.live_three);

    // 活四 against the same wall loses the same one end.
    Board four_wall;
    four_wall.place_stone(7, 2, BLACK);
    four_wall.place_stone(7, 3, BLACK);
    four_wall.place_stone(7, 4, BLACK);
    PositionPattern four, unused2;
    PatternRecognizer::recognize_position_both(four_wall, 7, 1, BLACK, four, unused2);
    assert(evaluator.pattern_score(four, 7, 1) == c.live_four - c.edge_penalty);

    // A live two deep in an open row is not discounted at all, so the correction
    // is the wall's doing and not a blanket tax on small shapes.
    Board open;
    open.place_stone(7, 8, BLACK);
    PositionPattern two, unused3;
    PatternRecognizer::recognize_position_both(open, 7, 7, BLACK, two, unused3);
    assert(evaluator.pattern_score(two, 7, 7) == c.live_two);

    // The bound that keeps the discount a discount: every candidate has four
    // directions and each direction has two ends, so the most one square can lose
    // is eight ends' worth - and that still has to cost less than one 冲四.
    assert(8 * c.edge_penalty < c.rush_four);
    // And the discounted shape does not change class: 400 is still worth more than
    // a 眠三 and less than a 冲四.
    assert(c.live_three - c.edge_penalty > c.sleepy_three);
    assert(c.live_three - c.edge_penalty < c.rush_four);

    // The calibration those rows assume: one closed end costs 100, and a candidate
    // square's centre bias spans 112 from corner to centre.
    assert(c.position_weight_scale == 8);
    assert(c.edge_penalty == 100);

    std::cout << "test_edge_penalty_below_rush_four passed (discount per end "
              << c.edge_penalty << ", worst per candidate " << 8 * c.edge_penalty
              << ", rush_four " << c.rush_four << ")" << std::endl;
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
    test_set_coefficients_rebuilds_position_weights();
    test_evaluate_board_skips_black_forbidden_point();
    test_evaluate_board_double_four_point();
    test_evaluate_board_keeps_white_value_on_forbidden_square();
    test_evaluate_board_unchanged_without_forbidden();
    test_evaluate_board_never_judges_white();
    test_candidate_value_zeroes_black_forbidden_shape();
    test_exact_five_outranks_the_forbidden_gate();
    test_evaluate_board_prefers_centre_stones();
    test_evaluate_board_antisymmetric_with_terms();
    test_edge_penalty_below_rush_four();
    
    std::cout << "All evaluator tests passed!" << std::endl;
    return 0;
}

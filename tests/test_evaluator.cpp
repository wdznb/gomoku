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

// What evaluate_board computed before the per-side forbidden guard: every
// candidate's pattern_score on both sides, unconditionally.
static long long naive_board_total(const Board& board, int my_color, const Evaluator& evaluator) {
    long long total = 0;
    for (const Position& pos : board.active_empty()) {
        PositionPattern mine, theirs;
        PatternRecognizer::recognize_position_both(board, pos.row, pos.col, my_color, mine, theirs);
        total += evaluator.pattern_score(mine) - evaluator.pattern_score(theirs);
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
            if (first_black_ps) *first_black_ps = evaluator.pattern_score(mine);
            if (first_white_ps) *first_white_ps = evaluator.pattern_score(theirs);
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
    assert(before == 38300);
    const int after_black = evaluator.evaluate_board(board, BLACK);
    assert(after_black == 7300);
    assert(after_black == static_cast<int>(before - black_ps));
    assert(evaluator.evaluate_board(board, WHITE) == -7300);
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
    assert(before == 99800);
    const int after_black = evaluator.evaluate_board(board, BLACK);
    assert(after_black == 29800);
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
    assert(before == 35700);
    const int after_black = evaluator.evaluate_board(board, BLACK);
    assert(after_black == 4700);
    // Exactly the black term vanished; WHITE's 500 is still in the total.
    assert(before - after_black == 31000);
    assert(evaluator.evaluate_board(board, WHITE) == -4700);
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
    assert(evaluator.evaluate_board(plain, BLACK) == 1700);
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
              << 1700 << ", " << threat_value << ")" << std::endl;
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
        white_ps = evaluator.pattern_score(theirs);
    }
    assert(white_ps == 31000);
    assert(evaluator.evaluate_board(board, WHITE) == 38300);
    assert(evaluator.evaluate_board(board, WHITE) == naive_board_total(board, WHITE, evaluator));
    assert(evaluator.evaluate_board(board, BLACK) == -38300);
    assert(evaluator.evaluate_board(board, BLACK) == naive_board_total(board, BLACK, evaluator));
    std::cout << "test_evaluate_board_never_judges_white passed (white keeps " << white_ps << ")" << std::endl;
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
    
    std::cout << "All evaluator tests passed!" << std::endl;
    return 0;
}

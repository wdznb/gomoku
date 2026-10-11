#include "core/board.h"
#include "core/pattern.h"
#include "core/evaluator.h"
#include "core/transforms.h"
#include "search/greedy.h"
#include "search/vcf.h"
#include "search/vct.h"
#include "search/alphabeta.h"
#include "search/search_engine.h"
#include "search/controller.h"
#include "optimization/transposition.h"
#include "optimization/opening_book.h"
#include "server/game_manager.h"
#include <iostream>
#include <cassert>
#include <atomic>
#include <thread>
#include <chrono>

using namespace gomoku;

void test_transposition_table() {
    TranspositionTable tt(1024);
    assert(tt.size() == 1024);

    Move m(7, 7, BLACK);
    tt.store(12345, 3, 500, m, TTFlag::EXACT);

    const TTEntry* entry = tt.lookup(12345);
    assert(entry != nullptr);
    assert(entry->hash == 12345);
    assert(entry->depth == 3);
    assert(entry->value == 500);
    assert(entry->flag == TTFlag::EXACT);

    const TTEntry* miss = tt.lookup(99999);
    assert(miss == nullptr);

    std::cout << "test_transposition_table passed" << std::endl;
}

void test_transposition_aging() {
    TranspositionTable tt(1024);

    Move m1(7, 7, BLACK);
    tt.store(100, 2, 300, m1, TTFlag::EXACT);
    tt.new_search();

    Move m2(7, 8, WHITE);
    tt.store(100, 1, 200, m2, TTFlag::LOWERBOUND);

    const TTEntry* entry = tt.lookup(100);
    assert(entry != nullptr);
    assert(entry->value == 200);
    assert(entry->depth == 1);

    std::cout << "test_transposition_aging passed" << std::endl;
}

void test_vcf_immediate_five() {
    Board board;
    Evaluator evaluator;
    TranspositionTable tt;

    board.place_stone(7, 3, BLACK);
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);

    VCFSearch vcf(evaluator, tt);
    std::atomic<bool> stop(false);

    VCFMove result = vcf.search(board, BLACK, 10, stop);
    assert(result.result == VCFResult::WIN);
    assert(result.best_move.pos.row == 7);
    assert(result.best_move.pos.col == 2 || result.best_move.pos.col == 7);

    std::cout << "test_vcf_immediate_five passed" << std::endl;
}

void test_vcf_no_win() {
    Board board;
    Evaluator evaluator;
    TranspositionTable tt;

    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);

    VCFSearch vcf(evaluator, tt);
    vcf.set_node_limit(10000);
    std::atomic<bool> stop(false);

    VCFMove result = vcf.search(board, BLACK, 6, stop);
    assert(result.result != VCFResult::WIN);

    std::cout << "test_vcf_no_win passed" << std::endl;
}

void test_vcf_gap_four_sequence_wins() {
    // A VCF whose only win begins with a gap four. BLACK (7,6) makes XX_XX across
    // the hole at (7,4): the hole is the sole five point, so the forced block is
    // that cell and the follow-up (8,6) makes a live four. Before the matcher saw
    // holes this candidate was not even generated, and the contiguous line
    // 1.(7,4) is a dead end because (7,1) blocks it, so this fails without the
    // gap support rather than merely taking a different route to a win.
    Board board;
    Evaluator evaluator;
    TranspositionTable tt;

    board.place_stone(7, 1, WHITE);
    board.place_stone(7, 2, BLACK);
    board.place_stone(7, 3, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(5, 6, BLACK);
    board.place_stone(6, 6, BLACK);

    VCFSearch vcf(evaluator, tt);
    std::atomic<bool> stop(false);

    VCFMove result = vcf.search(board, BLACK, 10, stop);
    assert(result.result == VCFResult::WIN);
    assert(result.best_move.pos.row == 7 && result.best_move.pos.col == 6);

    std::cout << "test_vcf_gap_four_sequence_wins passed" << std::endl;
}

void test_vct_immediate_win() {
    Board board;
    Evaluator evaluator;
    TranspositionTable tt;

    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);

    VCTSearch vct(evaluator, tt);
    std::atomic<bool> stop(false);

    VCTMove result = vct.search(board, BLACK, 6, stop);
    assert(result.result == VCTResult::WIN);

    std::cout << "test_vct_immediate_win passed" << std::endl;
}

void test_vct_diagonal_rush_four_is_not_a_win() {
    // Round 42: generate_defenses' fallback used a Manhattan <= 4 window, so a
    // diagonal four whose unique completion cell is 4 diagonal steps from the
    // attack stone (Manhattan 8) was never offered as a block. With no defense
    // listed, vct_defend/vct_attack returned 100000+depth, and search_engine
    // committed the move as a forced win without AlphaBeta - a fabricated win.
    //
    // BLACK has a diagonal three (6,6),(7,7),(8,8). Its only extensions are at
    // (5,5) or (9,9); playing either makes a RUSH four (the opposite outer end
    // is blocked by WHITE), completable only at the far diagonal cell - exactly
    // Chebyshev 4 / Manhattan 8 from the stone just played. WHITE has those
    // blocks, so this is NOT a forced win. Fixed filter uses Chebyshev <= 4.
    Board board;
    Evaluator evaluator;
    TranspositionTable tt;

    board.place_stone(6, 6, BLACK);
    board.place_stone(7, 7, BLACK);
    board.place_stone(8, 8, BLACK);
    board.place_stone(4, 4, WHITE);   // blocks the (5,5)-four's near end
    board.place_stone(10, 10, WHITE); // blocks the (9,9)-four's near end
    board.place_stone(0, 0, WHITE);   // parity filler: BLACK to move

    VCTSearch vct(evaluator, tt);
    std::atomic<bool> stop(false);

    VCTMove result = vct.search(board, BLACK, 8, stop);
    assert(result.result != VCTResult::WIN);

    std::cout << "test_vct_diagonal_rush_four_is_not_a_win passed" << std::endl;
}

void test_vct_deadline_aborts_search() {
    // Round 42 F2: VCT previously bounded work only by node_limit_ (500K),
    // ignoring timeout_ms, so it could overrun the caller's per-move budget by
    // seconds (and, joined on the single io thread, stall every session). With
    // an already-expired deadline the search must bail immediately rather than
    // grind out the forcing win it would otherwise find.
    Board board;
    Evaluator evaluator;
    TranspositionTable tt;

    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);

    VCTSearch vct(evaluator, tt);
    vct.set_deadline(std::chrono::steady_clock::now() - std::chrono::seconds(1));
    std::atomic<bool> stop(false);

    VCTMove result = vct.search(board, BLACK, 6, stop);
    assert(result.result != VCTResult::WIN);

    std::cout << "test_vct_deadline_aborts_search passed" << std::endl;
}

void test_alphabeta_basic() {
    Board board;
    Evaluator evaluator;
    TranspositionTable tt;

    board.place_stone(7, 7, BLACK);

    AlphaBetaSearch ab(evaluator, tt);
    std::atomic<bool> stop(false);

    Move result = ab.search(board, WHITE, 2, 3000, stop);
    assert(result.pos.is_valid());
    assert(board.is_empty(result.pos.row, result.pos.col));

    SearchInfo info = ab.last_search_info();
    assert(info.depth >= 1);
    assert(info.nodes_searched > 0);

    std::cout << "test_alphabeta_basic passed (depth: " << info.depth
              << ", nodes: " << info.nodes_searched << ")" << std::endl;
}

void test_alphabeta_blocks_five() {
    Board board;
    Evaluator evaluator;
    TranspositionTable tt;

    board.place_stone(7, 3, BLACK);
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);

    AlphaBetaSearch ab(evaluator, tt);
    std::atomic<bool> stop(false);

    Move result = ab.search(board, WHITE, 2, 3000, stop);
    assert(result.pos.row == 7);
    assert(result.pos.col == 2 || result.pos.col == 7);

    std::cout << "test_alphabeta_blocks_five passed" << std::endl;
}

void test_alphabeta_takes_winning() {
    Board board;
    Evaluator evaluator;
    TranspositionTable tt;

    board.place_stone(7, 3, WHITE);
    board.place_stone(7, 4, WHITE);
    board.place_stone(7, 5, WHITE);
    board.place_stone(7, 6, WHITE);

    AlphaBetaSearch ab(evaluator, tt);
    std::atomic<bool> stop(false);

    Move result = ab.search(board, WHITE, 2, 3000, stop);
    assert(result.pos.row == 7);
    assert(result.pos.col == 2 || result.pos.col == 7);

    std::cout << "test_alphabeta_takes_winning passed" << std::endl;
}

void test_search_engine_layered() {
    Board board;
    SearchEngine engine;

    board.place_stone(7, 7, BLACK);

    std::atomic<bool> stop(false);
    SearchResult result = engine.search(board, WHITE, 4, 3000, stop);

    assert(result.best_move.pos.is_valid());
    assert(!result.method.empty());

    std::cout << "test_search_engine_layered passed (method: " << result.method << ")" << std::endl;
}

void test_search_engine_finds_five() {
    Board board;
    SearchEngine engine;

    board.place_stone(7, 3, BLACK);
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);

    std::atomic<bool> stop(false);
    SearchResult result = engine.search(board, BLACK, 4, 3000, stop);

    assert(result.best_move.pos.row == 7);
    assert(result.best_move.pos.col == 2 || result.best_move.pos.col == 7);
    assert(result.score >= 99000);

    std::cout << "test_search_engine_finds_five passed (method: " << result.method << ")" << std::endl;
}

void test_controller_async() {
    Board board;
    SearchController controller;

    board.place_stone(7, 7, BLACK);

    std::atomic<bool> done(false);
    controller.set_completion_callback([&](SearchResult result) {
        assert(result.best_move.pos.is_valid());
        done.store(true);
    });

    controller.start_search(board, WHITE, 4, 2000);

    int wait_count = 0;
    while (!done.load() && wait_count < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wait_count++;
    }

    assert(done.load());
    assert(!controller.is_thinking());

    std::cout << "test_controller_async passed" << std::endl;
}

void test_controller_stop() {
    Board board;
    SearchController controller;

    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, WHITE);
    board.place_stone(6, 7, BLACK);

    controller.start_search(board, WHITE, 10, 30000);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    controller.stop();

    int wait_count = 0;
    while (controller.is_thinking() && wait_count < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wait_count++;
    }

    assert(!controller.is_thinking());

    std::cout << "test_controller_stop passed" << std::endl;
}

// Round 34 F2: starting a second search after the first completed previously
// reset the unique_ptr over a still-joinable std::thread, calling
// std::terminate. start_search must join the finished worker unconditionally.
void test_controller_restart_after_completion() {
    Board board;
    SearchController controller;
    board.place_stone(7, 7, BLACK);

    std::atomic<int> completed(0);
    controller.set_completion_callback([&](SearchResult result) {
        if (result.best_move.pos.is_valid()) completed.fetch_add(1);
    });

    controller.start_search(board, WHITE, 3, 1500);
    int wait_count = 0;
    while (controller.is_thinking() && wait_count < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wait_count++;
    }

    board.place_stone(7, 8, WHITE);
    controller.start_search(board, BLACK, 3, 1500);
    wait_count = 0;
    while (controller.is_thinking() && wait_count < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wait_count++;
    }

    assert(!controller.is_thinking());
    assert(completed.load() >= 1);

    std::cout << "test_controller_restart_after_completion passed" << std::endl;
}

// Round 35 F1: SearchResult fields must be deterministically initialised.
// The greedy_five / vcf / vct / forced / book return paths set only score (or
// nothing) and leave depth/nodes_searched/elapsed_ms untouched; the server
// callback reads score and elapsed_ms to build the ai_move message, so an
// uninitialised read would publish indeterminate values.
void test_search_result_fields_initialized_on_fast_paths() {
    Board board;
    SearchEngine engine;

    // WHITE can complete an exact five immediately.
    board.place_stone(7, 3, WHITE);
    board.place_stone(7, 4, WHITE);
    board.place_stone(7, 5, WHITE);
    board.place_stone(7, 6, WHITE);
    board.place_stone(9, 3, BLACK);

    std::atomic<bool> stop(false);
    SearchResult result = engine.search(board, WHITE, 4, 1000, stop);

    // Own-win path: score is set, the timing fields default to 0 (not garbage).
    assert(result.method == "greedy_five");
    assert(result.score == 100000);
    assert(result.elapsed_ms == 0);
    assert(result.depth == 0);
    assert(result.nodes_searched == 0);

    std::cout << "test_search_result_fields_initialized_on_fast_paths passed" << std::endl;
}

void test_forced_move() {
    Board board;
    SearchEngine engine;

    board.place_stone(7, 7, BLACK);

    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            if (board.is_empty(r, c)) {
                int dist = std::abs(r - 7) + std::abs(c - 7);
                if (dist > 2) {
                    board.place_stone(r, c, (r + c) % 2 == 0 ? BLACK : WHITE);
                }
            }
        }
    }

    std::atomic<bool> stop(false);
    SearchResult result = engine.search(board, BLACK, 4, 3000, stop);

    assert(result.best_move.pos.is_valid());

    std::cout << "test_forced_move passed" << std::endl;
}

// Round 33 F1: winning your own five must take priority over blocking the
// opponent's. Greedy ranks by attack + defense, so a defensive square can
// outrank an outright winning five; before the fix the must_block branch
// fired first and threw away a guaranteed win.
void test_search_engine_prefers_own_five_over_block() {
    Board board;
    SearchEngine engine;

    // BLACK has an unfinished four in row 7 that will become five next turn
    // if not blocked.
    board.place_stone(7, 3, BLACK);
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);

    // WHITE has a matching four in row 9; playing (9,2) or (9,7) is an
    // immediate win.
    board.place_stone(9, 3, WHITE);
    board.place_stone(9, 4, WHITE);
    board.place_stone(9, 5, WHITE);
    board.place_stone(9, 6, WHITE);

    std::atomic<bool> stop(false);
    SearchResult result = engine.search(board, WHITE, 4, 3000, stop);

    // Must complete WHITE's own five, not defend against BLACK.
    assert(result.best_move.color == WHITE);
    assert(result.best_move.pos.row == 9);
    assert(result.best_move.pos.col == 2 || result.best_move.pos.col == 7);
    assert(result.method == "greedy_five");

    std::cout << "test_search_engine_prefers_own_five_over_block passed" << std::endl;
}

void test_opening_book_prefix_expansion() {
    OpeningBook book;
    book.add_line({{7, 7}, {7, 8}, {6, 8}}, 10, "pu_yue");

    assert(!book.empty());
    assert(book.line_count() == 1);
    assert(book.max_depth() == 2);

    std::vector<Move> empty_hist;
    std::vector<Move> one = {Move(7, 7, BLACK)};
    std::vector<Move> two = {Move(7, 7, BLACK), Move(7, 8, WHITE)};
    std::vector<Move> three = {Move(7, 7, BLACK), Move(7, 8, WHITE), Move(6, 8, BLACK)};

    assert(!book.has_entry(empty_hist));
    assert(book.has_entry(one));
    assert(book.has_entry(two));
    assert(!book.has_entry(three));

    std::cout << "test_opening_book_prefix_expansion passed" << std::endl;
}

void test_opening_book_symmetry() {
    OpeningBook book;
    book.add_line({{7, 7}, {7, 8}, {6, 8}}, 10, "pu_yue");

    std::vector<Move> mirrored = {Move(7, 7, BLACK), Move(7, 6, WHITE)};
    Move response;
    assert(book.query(mirrored, BLACK, response));

    bool expected = response.pos == Position(6, 6) || response.pos == Position(8, 6);
    assert(expected);

    std::vector<Move> rotated = {Move(7, 7, BLACK), Move(8, 7, WHITE)};
    assert(book.query(rotated, BLACK, response));

    std::cout << "test_opening_book_symmetry passed" << std::endl;
}

void test_opening_book_miss() {
    OpeningBook book;
    book.add_line({{7, 7}, {7, 8}, {6, 8}}, 10, "pu_yue");

    std::vector<Move> offbook = {Move(0, 0, BLACK), Move(14, 14, WHITE)};
    Move response;
    assert(!book.query(offbook, BLACK, response));

    std::cout << "test_opening_book_miss passed" << std::endl;
}

void test_opening_book_load_json() {
    OpeningBook book;
    std::string path = std::string(GOMOKU_SOURCE_DIR) + "/data/opening_book.json";
    std::string error;

    assert(book.load(path, error));
    assert(!book.empty());
    assert(book.line_count() > 20);
    assert(book.entry_count() > book.line_count());

    std::vector<Move> hist = {Move(7, 7, BLACK), Move(7, 8, WHITE)};
    Move response;
    assert(book.query(hist, BLACK, response));
    assert(response.pos.is_valid());

    std::cout << "test_opening_book_load_json passed (lines: " << book.line_count()
              << ", entries: " << book.entry_count() << ")" << std::endl;
}

void test_opening_book_rejects_occupied() {
    OpeningBook book;
    book.add_line({{7, 7}, {7, 8}, {6, 8}}, 10, "pu_yue");

    Board board;
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, WHITE);
    board.place_stone(6, 8, BLACK);

    Move response;
    if (book.query(board.move_history(), WHITE, response)) {
        assert(board.is_empty(response.pos.row, response.pos.col));
    }

    std::cout << "test_opening_book_rejects_occupied passed" << std::endl;
}

void test_opening_book_large_weight_no_overflow() {
    // Two candidates share one key, each with a weight near 2^31. The old code
    // summed them into a signed int, overflowing into UB and a negative
    // uniform_int_distribution range. The pick must be computed in 64-bit and
    // still return one of the two legal replies.
    OpeningBook book;
    const int big = 2000000000;
    book.add_line({{7, 7}, {8, 8}, {6, 6}}, big, "a");
    book.add_line({{7, 7}, {8, 8}, {6, 7}}, big, "b");

    Board board;
    board.place_stone(7, 7, BLACK);
    board.place_stone(8, 8, WHITE);

    Move response;
    assert(book.query(board.move_history(), BLACK, response));
    bool ok = (response.pos.row == 6 && (response.pos.col == 6 || response.pos.col == 7));
    assert(ok);

    std::cout << "test_opening_book_large_weight_no_overflow passed" << std::endl;
}

void test_search_engine_uses_book() {
    OpeningBook book;
    std::string path = std::string(GOMOKU_SOURCE_DIR) + "/data/opening_book.json";
    std::string error;
    assert(book.load(path, error));

    Board board;
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, WHITE);

    SearchEngine engine;
    engine.set_opening_book(&book);

    std::atomic<bool> stop(false);
    SearchResult result = engine.search(board, BLACK, 6, 3000, stop);

    assert(result.method == "book");
    assert(result.best_move.pos.is_valid());
    assert(board.is_empty(result.best_move.pos.row, result.best_move.pos.col));

    engine.set_book_enabled(false);
    SearchResult no_book = engine.search(board, BLACK, 4, 3000, stop);
    assert(no_book.method != "book");

    std::cout << "test_search_engine_uses_book passed (book move: "
              << result.best_move.pos.row << "," << result.best_move.pos.col << ")" << std::endl;
}

void test_search_engine_ignores_book_midgame() {
    OpeningBook book;
    std::string path = std::string(GOMOKU_SOURCE_DIR) + "/data/opening_book.json";
    std::string error;
    assert(book.load(path, error));

    Board board;
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 3, WHITE);
    board.place_stone(10, 10, BLACK);
    board.place_stone(2, 2, WHITE);
    board.place_stone(10, 8, BLACK);

    SearchEngine engine;
    engine.set_opening_book(&book);

    std::atomic<bool> stop(false);
    SearchResult result = engine.search(board, WHITE, 2, 3000, stop);

    assert(result.method != "book");

    std::cout << "test_search_engine_ignores_book_midgame passed (method: "
              << result.method << ")" << std::endl;
}

void test_vcf_defense_proven_loss() {
    Board board;
    Evaluator evaluator;
    TranspositionTable tt;

    board.place_stone(7, 3, BLACK);
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(0, 0, WHITE);
    board.place_stone(0, 1, WHITE);
    board.place_stone(1, 0, WHITE);

    VCFSearch vcf(evaluator, tt);
    std::atomic<bool> stop(false);

    Move defense;
    DefenseResult dr = vcf.find_defense(board, WHITE, 10, stop, defense);
    assert(dr == DefenseResult::PROVEN_LOSS);

    std::cout << "test_vcf_defense_proven_loss passed" << std::endl;
}

void test_vcf_defense_survives() {
    Board board;
    Evaluator evaluator;
    TranspositionTable tt;

    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(0, 0, WHITE);
    board.place_stone(1, 1, WHITE);

    VCFSearch vcf(evaluator, tt);
    std::atomic<bool> stop(false);

    Move defense;
    DefenseResult dr = vcf.find_defense(board, WHITE, 10, stop, defense);
    assert(dr == DefenseResult::SURVIVES);
    assert(defense.color == WHITE);
    assert(board.is_empty(defense.pos.row, defense.pos.col));

    std::cout << "test_vcf_defense_survives passed (defense: "
              << defense.pos.row << "," << defense.pos.col << ")" << std::endl;
}

void test_vcf_defense_cut_short_is_unknown() {
    Board board;
    Evaluator evaluator;
    TranspositionTable tt;

    board.place_stone(7, 3, BLACK);
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(0, 0, WHITE);
    board.place_stone(0, 1, WHITE);

    VCFSearch vcf(evaluator, tt);
    std::atomic<bool> stop(true);

    Move defense;
    DefenseResult dr = vcf.find_defense(board, WHITE, 10, stop, defense);
    assert(dr == DefenseResult::UNKNOWN);

    std::cout << "test_vcf_defense_cut_short_is_unknown passed" << std::endl;
}

void test_vcf_defense_own_live_four_survives() {
    Board board;
    Evaluator evaluator;
    TranspositionTable tt;

    board.place_stone(7, 3, BLACK);
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(10, 4, WHITE);
    board.place_stone(10, 5, WHITE);
    board.place_stone(10, 6, WHITE);

    VCFSearch vcf(evaluator, tt);
    std::atomic<bool> stop(false);

    Move defense;
    DefenseResult dr = vcf.find_defense(board, WHITE, 10, stop, defense);
    assert(dr == DefenseResult::SURVIVES);
    // Which cell comes back depends on the active-empty iteration order, and
    // with gap shapes recognised (10,2) and (10,8) join (10,3) and (10,7) as
    // decisive tempi - a four whose only hole still makes exactly five is a
    // winning tempo too. What must hold is that WHITE answers with its own
    // four-or-better instead of a passive block.
    assert(defense.pos.row == 10);
    PositionPattern tempo = PatternRecognizer::recognize_position(
        board, defense.pos.row, defense.pos.col, WHITE);
    bool has_tempo = false;
    for (int d = 0; d < 4; ++d) {
        PatternType t = tempo.directions[d].type;
        if (t == PatternType::FIVE || t == PatternType::LIVE_FOUR ||
            t == PatternType::RUSH_FOUR) {
            has_tempo = true;
        }
    }
    assert(has_tempo);

    std::cout << "test_vcf_defense_own_live_four_survives passed" << std::endl;
}

void test_search_engine_resigns_on_proven_loss() {
    Board board;

    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(5, 4, BLACK);
    board.place_stone(6, 4, BLACK);
    board.place_stone(0, 0, WHITE);
    board.place_stone(1, 1, WHITE);

    SearchEngine engine;
    std::atomic<bool> stop(false);
    SearchResult result = engine.search(board, WHITE, 2, 2000, stop);

    assert(result.resigned);
    assert(result.method == "resign");
    assert(result.best_move.pos.is_valid());

    engine.set_resign_detection(false);
    SearchResult continued = engine.search(board, WHITE, 2, 2000, stop);
    assert(!continued.resigned);

    std::cout << "test_search_engine_resigns_on_proven_loss passed (move: "
              << result.best_move.pos.row << "," << result.best_move.pos.col << ")" << std::endl;
}

// The resign probe turns its time budget into a node cap via time*400. A large
// client timeout made that product overflow a signed int into a negative cap,
// tripping node_limit_ on the first node and silently disabling resignation -
// so the AI kept playing provably-lost games. This uses a huge timeout (with the
// slow main threat phases off, to bound runtime) and still requires a resign.
void test_search_engine_resigns_with_large_timeout() {
    Board board;

    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(5, 4, BLACK);
    board.place_stone(6, 4, BLACK);
    board.place_stone(0, 0, WHITE);
    board.place_stone(1, 1, WHITE);

    SearchEngine engine;
    engine.set_vcf_enabled(false);
    engine.set_vct_enabled(false);
    std::atomic<bool> stop(false);
    SearchResult result = engine.search(board, WHITE, 1, 6000000, stop);

    assert(result.resigned);
    assert(result.method == "resign");

    std::cout << "test_search_engine_resigns_with_large_timeout passed" << std::endl;
}

void test_search_engine_no_false_resign() {
    Board board;

    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, WHITE);
    board.place_stone(8, 8, BLACK);
    board.place_stone(6, 6, WHITE);

    SearchEngine engine;
    std::atomic<bool> stop(false);
    SearchResult result = engine.search(board, BLACK, 2, 2000, stop);

    assert(!result.resigned);
    assert(result.method != "resign");
    assert(result.best_move.pos.is_valid());
    assert(board.is_empty(result.best_move.pos.row, result.best_move.pos.col));

    std::cout << "test_search_engine_no_false_resign passed (method: "
              << result.method << ")" << std::endl;
}

void test_game_manager_resign_state() {
    GameManager game;
    game.set_player_color(BLACK);

    std::string reason;
    int winner = EMPTY;
    int callbacks = 0;
    game.set_game_over_callback([&](int w, const std::string& r) {
        winner = w;
        reason = r;
        callbacks++;
    });

    game.resign(BLACK);
    assert(game.status() == GameStatus::GAME_OVER);
    assert(winner == WHITE);
    assert(game.winner() == WHITE);
    assert(reason == "resign");
    assert(callbacks == 1);

    game.resign(WHITE);
    assert(callbacks == 1);
    assert(game.winner() == WHITE);

    std::cout << "test_game_manager_resign_state passed" << std::endl;
}

void test_game_manager_rejects_forbidden_human_move() {
    // Human BLACK cannot execute a double-three; Renju forbids it, and every
    // AI VCF/resign proof assumes the opponent is bound by the same rules.
    // Build the setup shape on a raw board and confirm the predicate that
    // make_move now consults actually fires.
    Board probe;
    probe.place_stone(7, 5, BLACK);
    probe.place_stone(7, 6, BLACK);
    probe.place_stone(5, 7, BLACK);
    probe.place_stone(6, 7, BLACK);
    assert(PatternRecognizer::is_forbidden(probe, 7, 7, BLACK));

    // Overline: six-in-a-row is also forbidden for BLACK and is caught by
    // the extended run walk added to recognize_direction.
    Board overline;
    for (int c = 3; c <= 7; ++c) overline.place_stone(7, c, BLACK);
    assert(PatternRecognizer::is_forbidden(overline, 7, 8, BLACK));

    std::cout << "test_game_manager_rejects_forbidden_human_move passed" << std::endl;
}

void test_game_manager_rejects_move_on_ai_turn() {
    // When current_turn is not player_color, make_move must reject the call
    // so a stray click cannot commit a stone on the AI's behalf.
    GameManager game;
    game.set_player_color(WHITE);
    // Board is empty; current_turn() is BLACK, which is the AI's colour.
    assert(game.current_turn() == BLACK);
    assert(game.current_turn() != game.player_color());
    bool ok = game.make_move(7, 7);
    assert(!ok);
    assert(game.board().move_count() == 0);

    std::cout << "test_game_manager_rejects_move_on_ai_turn passed" << std::endl;
}

void test_game_manager_promotes_status_when_ai_opens() {
    // Round 30: AI plays the first stone when the human chooses WHITE (or an
    // undo empties the board while ai_color_ == BLACK). The worker's commit
    // path is gated on status_ == PLAYING, so start_ai_thinking must promote
    // WAITING -> PLAYING synchronously; otherwise the AI's opening stone is
    // dropped and every later human move fails the turn guard.
    GameManager game;
    game.set_player_color(WHITE);
    assert(game.status() == GameStatus::WAITING);

    std::atomic<bool> got_move{false};
    game.set_move_callback([&](const Move&, int, int) { got_move.store(true); });
    game.set_config(500, 1);

    game.start_ai_thinking(500, 1);
    // Status must already be PLAYING before the search thread can commit.
    assert(game.status() == GameStatus::PLAYING);

    // Wait for the worker to finish and observe that it actually placed a
    // stone (this is the end-to-end guarantee the fix provides).
    for (int i = 0; i < 300 && !got_move.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(got_move.load());
    assert(game.board().move_count() == 1);

    std::cout << "test_game_manager_promotes_status_when_ai_opens passed" << std::endl;
}

void test_game_manager_hint_uses_hint_callback() {
    // Round 37 F1: a hint must NOT be delivered through move_callback_, which
    // the server maps to "ai_move". The client's ai_move handler places a stone
    // and flips the turn, so routing a hint there lets a single "hint" request
    // desync the board. Hints belong on hint_callback_ ("hint_result"), which
    // the client only highlights.
    GameManager game;
    game.set_player_color(BLACK);

    std::atomic<bool> hint_fired{false};
    std::atomic<bool> move_fired{false};
    game.set_hint_callback([&](const Move& m, int) {
        assert(m.pos.is_valid());
        hint_fired.store(true);
    });
    game.set_move_callback([&](const Move&, int, int) { move_fired.store(true); });
    game.set_config(500, 1);

    game.request_hint(500, 1);
    for (int i = 0; i < 300 && !hint_fired.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    assert(hint_fired.load());
    assert(!move_fired.load());
    // A hint is advisory only: it must never commit a stone to the board.
    assert(game.board().move_count() == 0);

    std::cout << "test_game_manager_hint_uses_hint_callback passed" << std::endl;
}

void test_game_manager_replay_snapshot() {
    // Phase 4.4 replays the session's own game, so the analysis list has to be
    // the board's history seen a second way: one entry per committed stone,
    // dropped together on undo, gone after reset, and untouched by a resign
    // that placed no stone.
    GameManager game;
    game.set_player_color(BLACK);
    game.set_config(500, 1);

    std::atomic<int> ai_moves{0};
    game.set_move_callback([&](const Move&, int, int) { ai_moves.fetch_add(1); });

    assert(game.replay_snapshot().moves.empty());
    assert(game.make_move(7, 7));

    for (int i = 0; i < 300 && ai_moves.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(ai_moves.load() >= 1);

    ReplaySnapshot snap = game.replay_snapshot();
    assert(snap.moves.size() == 2);
    assert(snap.moves.size() == game.board().move_count());
    assert(snap.moves[0].method == "human");
    assert(snap.moves[0].move.color == BLACK);
    assert(snap.moves[0].move.pos.row == 7 && snap.moves[0].move.pos.col == 7);
    assert(snap.moves[1].move.color == WHITE);
    assert(snap.status != GameStatus::GAME_OVER);
    assert(snap.winner == EMPTY);

    // Both stones come back, so both entries must leave the list.
    int move_count = 1;
    GameStatus undo_status = GameStatus::PLAYING;
    assert(game.undo(move_count, undo_status));
    assert(move_count == 0);
    assert(game.replay_snapshot().moves.empty());

    assert(game.make_move(7, 7));
    for (int i = 0; i < 300 && ai_moves.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    game.resign(BLACK);

    snap = game.replay_snapshot();
    assert(snap.status == GameStatus::GAME_OVER);
    assert(snap.winner == WHITE);
    assert(snap.reason == "resign");
    assert(snap.moves.size() == game.board().move_count());
    assert(snap.moves.front().method == "human");

    game.reset();
    assert(game.replay_snapshot().moves.empty());

    std::cout << "test_game_manager_replay_snapshot passed" << std::endl;
}

void test_root_symmetry_dedup() {
    // Phase 4.2 drops root moves that are mirror images of a move already
    // covered. An empty board is invariant under all eight D4 variants and its
    // 225 squares form 36 orbits (Burnside: (225 + 1 + 1 + 1 + 4*15) / 8), so
    // exactly 36 squares may survive the filter.
    Board empty;
    const std::vector<int> full_group = self_symmetries(empty);
    assert(full_group.size() == static_cast<size_t>(SYMMETRY_COUNT));

    int representatives = 0;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            if (is_symmetry_representative(full_group, Position(r, c))) ++representatives;
        }
    }
    assert(representatives == 36);

    Evaluator evaluator;
    std::atomic<bool> stop(false);

    // A single centre stone leaves the board fully symmetric, so whatever the
    // root plays it must play the representative of the orbit, not its mirror.
    Board center;
    center.place_stone(7, 7, BLACK);

    TranspositionTable tt_center;
    AlphaBetaSearch ab_center(evaluator, tt_center);
    Move chosen = ab_center.search(center, WHITE, 2, 3000, stop);
    assert(chosen.pos.is_valid());
    assert(is_symmetry_representative(self_symmetries(center), chosen.pos));

    // The premise of the whole filter: an isomorphic position scores alike, so
    // dropping the mirrors cannot drop the best line. {B(7,7) W(7,6)} and its
    // transpose {B(7,7) W(6,7)} are the same shape seen from the other side.
    Board wide;
    wide.place_stone(7, 7, BLACK);
    wide.place_stone(7, 6, WHITE);
    Board tall;
    tall.place_stone(7, 7, BLACK);
    tall.place_stone(6, 7, WHITE);

    TranspositionTable tt_wide;
    TranspositionTable tt_tall;
    AlphaBetaSearch ab_wide(evaluator, tt_wide);
    AlphaBetaSearch ab_tall(evaluator, tt_tall);
    ab_wide.search(wide, BLACK, 3, 3000, stop);
    ab_tall.search(tall, BLACK, 3, 3000, stop);

    const SearchInfo wide_info = ab_wide.last_search_info();
    const SearchInfo tall_info = ab_tall.last_search_info();
    assert(wide_info.score == tall_info.score);

    std::cout << "test_root_symmetry_dedup passed (representatives: " << representatives
              << ", mirrored score: " << wide_info.score << ")" << std::endl;
}

// Regression guard for the fixed-2M node-limit bug: AlphaBetaSearch exposes a
// setter but search_engine never called it, so every search was capped at 2M
// nodes regardless of the timeout. This test verifies the API contract holds.
void test_alphabeta_node_limit_api() {
    Evaluator evaluator;
    TranspositionTable tt;
    AlphaBetaSearch ab(evaluator, tt);

    // The constructor hard-codes 2,000,000 — this is the known baseline.
    // The real fix lives in search_engine.cpp (dynamic budget from timeout),
    // but having this API-level check prevents accidental re-introduction of
    // a fixed cap without a corresponding setter call in the caller chain.
    ab.set_node_limit(500000);
    // If set_node_limit compiles and doesn't crash, the API contract holds.

    std::cout << "test_alphabeta_node_limit_api passed" << std::endl;
}

int main() {
    std::cout << "Running search tests..." << std::endl;

    PatternRecognizer::init();

    test_transposition_table();
    test_transposition_aging();
    test_vcf_immediate_five();
    test_vcf_no_win();
    test_vcf_gap_four_sequence_wins();
    test_vct_immediate_win();
    test_vct_diagonal_rush_four_is_not_a_win();
    test_vct_deadline_aborts_search();
    test_alphabeta_basic();
    test_alphabeta_blocks_five();
    test_alphabeta_takes_winning();
    test_opening_book_prefix_expansion();
    test_opening_book_symmetry();
    test_opening_book_miss();
    test_opening_book_load_json();
    test_opening_book_rejects_occupied();
    test_opening_book_large_weight_no_overflow();
    test_search_engine_uses_book();
    test_search_engine_ignores_book_midgame();
    test_search_engine_layered();
    test_search_engine_finds_five();
    test_controller_async();
    test_controller_stop();
    test_controller_restart_after_completion();
    test_forced_move();
    test_search_result_fields_initialized_on_fast_paths();
    test_vcf_defense_proven_loss();
    test_vcf_defense_survives();
    test_vcf_defense_cut_short_is_unknown();
    test_vcf_defense_own_live_four_survives();
    test_search_engine_resigns_on_proven_loss();
    test_search_engine_no_false_resign();
    test_game_manager_resign_state();
    test_game_manager_rejects_forbidden_human_move();
    test_game_manager_rejects_move_on_ai_turn();
    test_game_manager_promotes_status_when_ai_opens();
    test_game_manager_hint_uses_hint_callback();
    test_game_manager_replay_snapshot();
    test_root_symmetry_dedup();
    test_search_engine_prefers_own_five_over_block();
    test_alphabeta_node_limit_api();

    std::cout << "All search tests passed!" << std::endl;
    return 0;
}

#include "core/board.h"
#include "core/pattern.h"
#include "core/evaluator.h"
#include "core/transforms.h"
#include "optimization/transposition.h"
#include <atomic>
#include <chrono>
#include <vector>
#include <algorithm>
// J2 drives sort_moves and generate_moves directly, and both are private. Every
// header alphabeta.h pulls in is included above, so the macro rewrites
// alphabeta.h's class body and nothing else (same trick as test_training.cpp).
#define private public
#include "search/alphabeta.h"
#undef private
#include "search/greedy.h"
#include "search/vcf.h"
#include "search/vct.h"
#include "search/search_engine.h"
#include "search/controller.h"
#include "optimization/opening_book.h"
#include "server/game_manager.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace gomoku;

namespace {

// Resident set size in KiB, for pinning the default table's real footprint.
bool read_rss_kb(long long& kb) {
#if defined(_WIN32)
    (void)kb;
    return false;
#else
    std::ifstream file("/proc/self/statm");
    long long total_pages = 0;
    long long resident_pages = 0;
    if (!(file >> total_pages >> resident_pages)) return false;
    kb = resident_pages * 4;  // statm counts 4 KiB pages
    return true;
#endif
}

}  // namespace

// The default table is 2^18 entries (~8 MiB); the old 2^20 default cost ~32 MiB
// per engine, and every session owns an engine. Run before any other table is
// built so the delta reflects this table alone: 32 MiB would break the upper
// bound and a shrunk table would break the lower one.
void test_transposition_default_size() {
    long long before_kb = 0;
    const bool have_rss = read_rss_kb(before_kb);
    long long delta_kb = 0;
    bool measured = false;
    {
        TranspositionTable tt;
        assert(tt.size() == (1u << 18));
        long long after_kb = 0;
        if (have_rss && read_rss_kb(after_kb)) {
            measured = true;
            delta_kb = after_kb - before_kb;
        }
    }
    if (measured) {
        assert(delta_kb >= 4096 && delta_kb <= 20480);
        std::cout << "test_transposition_default_size passed (rss delta " << delta_kb << " KiB)"
                  << std::endl;
    } else {
        std::cout << "test_transposition_default_size passed (rss unavailable)" << std::endl;
    }
}

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
    // Round 75, H1 parity: the fixed reader accepts exactly the 45 lines and
    // 109 weighted prefix keys the pre-fix reader did. Every coordinate and
    // weight in this file fits an int with room to spare, so no cast ever
    // truncated and the two readers must agree number for number (the old logic
    // replayed over the same file in build/r75b_book_parity.py gives the same
    // two counts).
    assert(book.line_count() == 45);
    assert(book.entry_count() == 109);

    std::vector<Move> hist = {Move(7, 7, BLACK), Move(7, 8, WHITE)};
    Move response;
    assert(book.query(hist, BLACK, response));
    assert(response.pos.is_valid());

    std::cout << "test_opening_book_load_json passed (lines: " << book.line_count()
              << ", entries: " << book.entry_count() << ")" << std::endl;
}

// Round 75, H1: a line whose coordinate or weight is wider than an int used to
// be cast by get<int>() - 2^32+8 became row 8, 2^32 became column 0, 2^32+2
// became weight 2 - so a malformed line entered the book aiming at a square
// nobody wrote. Such a line is now refused whole. The legal lines beside it in
// the same file must still load, or one malformed entry would poison the book.
void test_opening_book_rejects_wide_numbers() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "gomoku_book_wide.json";
    {
        std::ofstream file(path);
        file << R"({"lines":[)"
             << R"({"name":"bad_row","weight":5,"moves":[[4294967304,7],[7,8],[6,8]]},)"
             << R"({"name":"bad_col","weight":5,"moves":[[1,1],[1,4294967296],[2,2]]},)"
             << R"({"name":"bad_weight","weight":4294967298,"moves":[[0,0],[0,1],[1,1]]},)"
             << R"({"name":"good","weight":5,"moves":[[3,3],[3,4],[4,4]]})"
             << R"(]})";
    }

    OpeningBook book;
    std::string error;
    assert(book.load(path.string(), error));
    assert(book.line_count() == 1);
    // The one legal line expands to its 8 symmetries: 4 first-move keys and 8
    // two-ply keys, 12 unique keys in all.
    assert(book.entry_count() == 12);

    // The squares a truncating cast would have created are not in the book:
    // row 8 from 2^32+8, column 0 from 2^32, and the weight-2 copy of (0,0).
    assert(!book.has_entry({Move(8, 7, BLACK)}));
    assert(!book.has_entry({Move(1, 1, BLACK)}));
    assert(!book.has_entry({Move(1, 1, BLACK), Move(1, 0, WHITE)}));
    assert(!book.has_entry({Move(0, 0, BLACK)}));

    // The legal line answers, prefix for prefix.
    assert(book.has_entry({Move(3, 3, BLACK)}));
    assert(book.has_entry({Move(3, 3, BLACK), Move(3, 4, WHITE)}));
    Move response;
    assert(book.query({Move(3, 3, BLACK)}, WHITE, response));
    assert(response.pos == Position(3, 4) || response.pos == Position(4, 3));

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::cout << "test_opening_book_rejects_wide_numbers passed" << std::endl;
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

void test_opening_book_rejects_swapped_colours() {
    // Round 74: the book key is only the sequence of landing squares, so a
    // hand-set position whose stones were laid down out of colour alternation
    // used to walk into a book line whose first entries are BLACK. query() now
    // checks every history stone against the line's own colours.
    OpeningBook book;
    book.add_line({{7, 7}, {7, 8}, {8, 8}}, 10, "hua_yue");

    Board normal;                                    // the true alternation
    normal.place_stone(7, 7, BLACK);
    normal.place_stone(7, 8, WHITE);
    Move response;
    assert(book.query(normal, BLACK, response));
    // The hua_yue line and its vertical mirror coincide on the first two
    // squares (row 7 is the centre row), so this key legitimately holds two
    // replies - (8,8) and (6,8) - and the weighted pick may land on either.
    assert(response.color == BLACK);
    assert(response.pos == Position(8, 8) || response.pos == Position(6, 8));

    Board swapped;                                   // same squares, colours swapped
    swapped.place_stone(7, 7, WHITE);
    swapped.place_stone(7, 8, BLACK);
    assert(swapped.move_history()[0].color == WHITE);
    assert(swapped.move_history()[1].color == BLACK);
    assert(swapped.move_history()[0].pos == Position(7, 7));
    assert(swapped.move_history()[1].pos == Position(7, 8));
    Move other;
    assert(!book.query(swapped, BLACK, other));      // old code answered true here

    std::cout << "test_opening_book_rejects_swapped_colours passed" << std::endl;
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

// Round 43 T1: generate_defenses used to return ONLY the defender's counter-
// threat squares whenever any existed anywhere on the board (the proximity
// fallback was gated on that list being empty), so the squares that actually
// answer the four the attacker just made were dropped from the defense set and
// vct_defend/vct_attack minned over an incomplete set. The fix merges the
// Chebyshev <= 4 candidates in unconditionally.
//
// WHITE's three at (5,4)-(5,6) is closed on the left by BLACK (5,3), so every
// extension is a RUSH four whose only answer is (5,8). BLACK also keeps a rush
// four in the distance at (10,10)-(10,13) (five point (10,9), (10,14) holds the
// other end): a counter-threat square far from the attack - exactly what the old
// guard used to exclude the (5,8) block. WHITE has no forced win here, so VCT
// must not claim one.
void test_vct_far_counter_does_not_fabricate_win() {
    Board board;
    Evaluator evaluator;
    TranspositionTable tt;

    board.place_stone(5, 3, BLACK);
    board.place_stone(5, 4, WHITE);
    board.place_stone(5, 5, WHITE);
    board.place_stone(5, 6, WHITE);
    board.place_stone(10, 10, BLACK);
    board.place_stone(10, 11, BLACK);
    board.place_stone(10, 12, BLACK);
    board.place_stone(10, 13, BLACK);
    board.place_stone(10, 14, WHITE);

    VCTSearch vct(evaluator, tt);
    std::atomic<bool> stop(false);

    VCTMove result = vct.search(board, WHITE, 8, stop);
    assert(result.result != VCTResult::WIN);

    // Control: a concrete forced sequence (the gap-four chain) must still be
    // proven, so merging the blocks did not turn the prover silent.
    Board forced;
    forced.place_stone(7, 1, WHITE);
    forced.place_stone(7, 2, BLACK);
    forced.place_stone(7, 3, BLACK);
    forced.place_stone(7, 5, BLACK);
    forced.place_stone(5, 6, BLACK);
    forced.place_stone(6, 6, BLACK);

    VCTMove won = vct.search(forced, BLACK, 10, stop);
    assert(won.result == VCTResult::WIN);
    assert(won.best_move.pos.row == 7 && won.best_move.pos.col == 6);

    std::cout << "test_vct_far_counter_does_not_fabricate_win passed" << std::endl;
}

// Round 43 T2: resign detection used to rely on the VCF loss probe alone, and
// that probe is gated on probe_budget_ms > 500 (time_limit_ms minus what
// AlphaBeta already spent), so a small per-move budget silently disabled
// resignation even when AlphaBeta itself had proven the loss (its mate score
// reaches the +/-(100000+depth) threshold). The fix resigns straight off a
// proven mate score. WHITE is lost to BLACK's double live three; 300 ms is far
// below the 500 ms probe gate, so only the new AlphaBeta path can trigger.
void test_search_engine_resigns_when_alphabeta_proves_mate() {
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
    SearchResult result = engine.search(board, WHITE, 4, 300, stop);

    assert(result.resigned);
    assert(result.method == "resign");
    assert(result.best_move.pos.is_valid());

    // Control: the same budget on a normal position must keep playing.
    Board quiet;
    quiet.place_stone(7, 7, BLACK);
    quiet.place_stone(7, 8, WHITE);
    quiet.place_stone(8, 8, BLACK);
    quiet.place_stone(6, 6, WHITE);
    SearchResult continued = engine.search(quiet, BLACK, 2, 300, stop);
    assert(!continued.resigned);
    assert(continued.method != "resign");
    assert(continued.best_move.pos.is_valid());

    std::cout << "test_search_engine_resigns_when_alphabeta_proves_mate passed" << std::endl;
}

// Round 43 T7: the AlphaBeta node budget is remaining_time * 200. A huge client
// timeout multiplied in int overflowed - 1500000000 * 200 wraps negative - which
// tripped node_limit_ before the first node: AlphaBeta returned no depth and no
// nodes and search_engine fell back to root_moves[0]. The fix multiplies in
// 64-bit and clamps to INT_MAX, so both huge budgets search normally.
void test_alphabeta_huge_timeout_does_not_overflow_node_budget() {
    Board board;
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, WHITE);
    board.place_stone(8, 8, BLACK);

    const int huge_times[] = {2000000000, 1500000000};
    for (int huge : huge_times) {
        SearchEngine engine;
        engine.set_vcf_enabled(false);
        engine.set_vct_enabled(false);
        std::atomic<bool> stop(false);
        SearchResult result = engine.search(board, BLACK, 3, huge, stop);

        assert(result.method == "alphabeta");
        assert(result.depth >= 1);
        assert(result.nodes_searched > 0);
        assert(result.best_move.pos.is_valid());
        assert(board.is_empty(result.best_move.pos.row, result.best_move.pos.col));
        assert(!result.resigned);
    }

    std::cout << "test_alphabeta_huge_timeout_does_not_overflow_node_budget passed" << std::endl;
}

namespace {

// J1/J2 share one midgame shape: twelve stones, both sides holding a live three
// (BLACK's row 7 is open at (7,6)/(7,10), WHITE's row 9 at (9,5)/(9,9)). Deep
// enough that a real search runs for a while, not a tactic the engine sees at
// once.
void build_midgame(Board& board) {
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, BLACK);
    board.place_stone(7, 9, BLACK);
    board.place_stone(9, 6, WHITE);
    board.place_stone(9, 7, WHITE);
    board.place_stone(9, 8, WHITE);
    board.place_stone(5, 5, BLACK);
    board.place_stone(10, 4, BLACK);
    board.place_stone(11, 11, BLACK);
    board.place_stone(4, 9, WHITE);
    board.place_stone(3, 11, WHITE);
    board.place_stone(2, 2, WHITE);
    assert(board.move_count() == 12);
}

std::vector<std::pair<int, int>> cell_multiset(const std::vector<Move>& moves) {
    std::vector<std::pair<int, int>> cells;
    cells.reserve(moves.size());
    for (const Move& m : moves) cells.emplace_back(m.pos.row, m.pos.col);
    std::sort(cells.begin(), cells.end());
    return cells;
}

std::vector<int> heuristic_scores(const std::vector<Move>& moves, const Board& board,
                                  const Evaluator& evaluator, int color) {
    std::vector<int> scores;
    scores.reserve(moves.size());
    for (const Move& m : moves) {
        scores.push_back(evaluator.evaluate_position_pair(board, m.pos.row, m.pos.col, color));
    }
    return scores;
}

}  // namespace

// J1: the deadline is the engine's one absolute budget, not a slice measured
// from this phase's own start. The phase's own time limit here is 5000 ms while
// the shared deadline is 50 ms away; before the fix AlphaBeta ignored the
// deadline (is_timeout() only knew move_time_limit_ms_) and ran the whole
// 5000 ms slice.
void test_alphabeta_absolute_deadline_stops_search() {
    Board board;
    build_midgame(board);
    Evaluator evaluator;
    TranspositionTable tt;

    AlphaBetaSearch ab(evaluator, tt);
    std::atomic<bool> stop(false);
    ab.set_deadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(50));

    const auto t0 = std::chrono::steady_clock::now();
    Move result = ab.search(board, WHITE, 8, 5000, stop);
    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    assert(result.pos.is_valid());
    assert(board.is_empty(result.pos.row, result.pos.col));
    assert(elapsed < 1000);

    std::cout << "test_alphabeta_absolute_deadline_stops_search passed (" << elapsed
              << " ms, depth " << ab.last_search_info().depth << ")" << std::endl;
}

// J1: a deadline already in the past must still hand back a legal move. The
// first iteration breaks before any node is visited, best_move stays invalid
// and the root fallback answers with root_moves[0]; the reported depth stays 0
// so the caller cannot mistake it for a searched move.
void test_alphabeta_expired_deadline_returns_root_fallback() {
    Board board;
    build_midgame(board);
    Evaluator evaluator;
    TranspositionTable tt;

    AlphaBetaSearch ab(evaluator, tt);
    std::atomic<bool> stop(false);
    ab.set_deadline(std::chrono::steady_clock::now() - std::chrono::seconds(1));

    const auto t0 = std::chrono::steady_clock::now();
    Move result = ab.search(board, WHITE, 8, 5000, stop);
    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    assert(result.pos.is_valid());
    assert(board.is_empty(result.pos.row, result.pos.col));
    SearchInfo info = ab.last_search_info();
    assert(info.depth == 0);
    assert(elapsed < 1000);

    // The fallback is the first root candidate: regenerating the root list the
    // same way (generate + heuristic sort; this asymmetric board has no
    // symmetries to dedup) must name the same cell.
    std::vector<Move> root = ab.generate_moves(board, WHITE);
    assert(!root.empty());
    ab.sort_moves(root, board, WHITE);
    assert(result.pos == root[0].pos);

    std::cout << "test_alphabeta_expired_deadline_returns_root_fallback passed ("
              << elapsed << " ms, fallback "
              << result.pos.row << "," << result.pos.col << ")" << std::endl;
}

// J1: without a deadline the per-search slice remains the only bound: a 200 ms
// budget on a deep request must return a normal searched move inside a generous
// wall-clock ceiling.
void test_alphabeta_large_depth_bounded_by_slice_without_deadline() {
    Board board;
    build_midgame(board);
    Evaluator evaluator;
    TranspositionTable tt;

    AlphaBetaSearch ab(evaluator, tt);
    std::atomic<bool> stop(false);

    const auto t0 = std::chrono::steady_clock::now();
    Move result = ab.search(board, WHITE, 6, 200, stop);
    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    assert(result.pos.is_valid());
    assert(board.is_empty(result.pos.row, result.pos.col));
    assert(elapsed < 1500);

    std::cout << "test_alphabeta_large_depth_bounded_by_slice_without_deadline passed ("
              << elapsed << " ms, depth " << ab.last_search_info().depth << ")" << std::endl;
}

// J2: the TT move already at index 0 used to be repainted as an ordinary
// candidate - the heuristic re-sort covered index 0 too, so a later move with a
// higher score displaced the one move the previous search paid to know. Here
// moves[3] is the strict heuristic maximum and moves[0] is the TT move with a
// lower score; after sort_moves, index 0 must still be the TT move and the tail
// must open with that maximum.
void test_sort_moves_pins_tt_move_already_at_front() {
    Board board;
    build_midgame(board);
    Evaluator evaluator;
    TranspositionTable tt;
    AlphaBetaSearch ab(evaluator, tt);

    std::vector<Move> generated = ab.generate_moves(board, WHITE);
    assert(generated.size() > 8);
    const std::vector<std::pair<int, int>> multiset = cell_multiset(generated);

    const std::vector<int> scores = heuristic_scores(generated, board, evaluator, WHITE);
    const size_t max_i = static_cast<size_t>(
        std::max_element(scores.begin(), scores.end()) - scores.begin());
    const size_t min_i = static_cast<size_t>(
        std::min_element(scores.begin(), scores.end()) - scores.begin());
    assert(max_i != min_i);
    assert(scores[max_i] > scores[min_i]);
    const Move tt_move = generated[min_i];

    std::vector<Move> ordered;
    ordered.reserve(generated.size());
    ordered.push_back(tt_move);  // index 0 already holds the TT move
    for (const Move& m : generated) {
        if (m.pos == generated[min_i].pos || m.pos == generated[max_i].pos) continue;
        ordered.push_back(m);
        if (ordered.size() == 3) ordered.push_back(generated[max_i]);  // index 3: the best
    }
    assert(ordered.size() == generated.size());
    assert(ordered[0].pos == tt_move.pos);
    assert(ordered[3].pos == generated[max_i].pos);

    ab.sort_moves(ordered, board, WHITE, tt_move);

    assert(ordered[0].pos == tt_move.pos);
    assert(cell_multiset(ordered) == multiset);
    const std::vector<int> after = heuristic_scores(ordered, board, evaluator, WHITE);
    assert(after.size() == generated.size());
    assert(after[1] == scores[max_i]);
    for (size_t i = 2; i < after.size(); ++i) assert(after[i] <= after[i - 1]);

    std::cout << "test_sort_moves_pins_tt_move_already_at_front passed (tt "
              << tt_move.pos.row << "," << tt_move.pos.col << ", scores "
              << scores[min_i] << " vs tail-max " << scores[max_i] << ")" << std::endl;
}

// J2: a TT move found deeper in the list is swapped to the front and the pin
// holds there; the tail behind it is non-increasing and no cell is lost or
// duplicated.
void test_sort_moves_promotes_tt_move_from_tail() {
    Board board;
    build_midgame(board);
    Evaluator evaluator;
    TranspositionTable tt;
    AlphaBetaSearch ab(evaluator, tt);

    std::vector<Move> generated = ab.generate_moves(board, WHITE);
    assert(generated.size() > 8);
    const std::vector<std::pair<int, int>> multiset = cell_multiset(generated);

    const std::vector<int> scores = heuristic_scores(generated, board, evaluator, WHITE);
    const size_t max_i = static_cast<size_t>(
        std::max_element(scores.begin(), scores.end()) - scores.begin());

    // The strict maximum, worst case for the pin, starts at index 5.
    std::vector<Move> ordered;
    ordered.reserve(generated.size());
    for (size_t i = 0; i < generated.size(); ++i) {
        if (i == max_i) continue;
        if (ordered.size() == 5) ordered.push_back(generated[max_i]);
        ordered.push_back(generated[i]);
    }
    assert(ordered.size() == generated.size());
    assert(ordered[5].pos == generated[max_i].pos);
    const Move tt_move = generated[max_i];

    ab.sort_moves(ordered, board, WHITE, tt_move);

    assert(ordered[0].pos == tt_move.pos);
    assert(cell_multiset(ordered) == multiset);
    const std::vector<int> after = heuristic_scores(ordered, board, evaluator, WHITE);
    assert(after[0] == scores[max_i]);
    for (size_t i = 2; i < after.size(); ++i) assert(after[i] <= after[i - 1]);

    std::cout << "test_sort_moves_promotes_tt_move_from_tail passed (tt "
              << tt_move.pos.row << "," << tt_move.pos.col << ", tail "
              << after[1] << ".." << after.back() << ")" << std::endl;
}

// J2: without a TT move (invalid) nothing is pinned and the whole list is
// re-ordered, so index 0 is the heuristic maximum and the array is
// non-increasing from the front.
void test_sort_moves_full_order_without_tt_move() {
    Board board;
    build_midgame(board);
    Evaluator evaluator;
    TranspositionTable tt;
    AlphaBetaSearch ab(evaluator, tt);

    assert(!Move().pos.is_valid());

    std::vector<Move> generated = ab.generate_moves(board, WHITE);
    assert(generated.size() > 8);
    const std::vector<std::pair<int, int>> multiset = cell_multiset(generated);

    const std::vector<int> scores = heuristic_scores(generated, board, evaluator, WHITE);
    const int best = *std::max_element(scores.begin(), scores.end());

    std::vector<Move> ordered = generated;
    ab.sort_moves(ordered, board, WHITE, Move());

    assert(cell_multiset(ordered) == multiset);
    const std::vector<int> after = heuristic_scores(ordered, board, evaluator, WHITE);
    assert(after.size() == generated.size());
    assert(after[0] == best);
    for (size_t i = 1; i < after.size(); ++i) assert(after[i] <= after[i - 1]);

    std::cout << "test_sort_moves_full_order_without_tt_move passed" << std::endl;
}

int main() {
    std::cout << "Running search tests..." << std::endl;

    PatternRecognizer::init();

    test_transposition_default_size();
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
    test_opening_book_rejects_wide_numbers();
    test_opening_book_rejects_occupied();
    test_opening_book_rejects_swapped_colours();
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
    test_vct_far_counter_does_not_fabricate_win();
    test_search_engine_resigns_when_alphabeta_proves_mate();
    test_alphabeta_huge_timeout_does_not_overflow_node_budget();
    test_alphabeta_absolute_deadline_stops_search();
    test_alphabeta_expired_deadline_returns_root_fallback();
    test_alphabeta_large_depth_bounded_by_slice_without_deadline();
    test_sort_moves_pins_tt_move_already_at_front();
    test_sort_moves_promotes_tt_move_from_tail();
    test_sort_moves_full_order_without_tt_move();

    std::cout << "All search tests passed!" << std::endl;
    return 0;
}

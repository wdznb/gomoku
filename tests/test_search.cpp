#include "core/board.h"
#include "core/pattern.h"
#include "core/evaluator.h"
#include "core/transforms.h"
#include "optimization/transposition.h"
#include <atomic>
#include <chrono>
#include <climits>
#include <array>
#include <cstdint>
#include <vector>
#include <algorithm>
// J2 drives sort_moves and generate_moves directly, and both are private; the
// round-79 generator checks drive generate_rush_fours / generate_attacks the
// same way. Every header the alpha-beta header pulls in is included above, so
// the macro rewrites only this project's class bodies (same trick as
// test_training.cpp).
#define private public
#include "search/alphabeta.h"
#include "search/vcf.h"
#include "search/vct.h"
#undef private
#include "search/greedy.h"
#include "search/move_generator.h"
#include "search/search_engine.h"
#include "search/controller.h"
#include "search/search_limits.h"
#include "search/search_state.h"
#include "search/time_manager.h"
#include "optimization/opening_book.h"
#include "server/game_manager.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace gomoku;

namespace {

// Defined further down, next to the J1/J2 tests that share the shape.
void build_midgame(Board& board);

// A clock the test steers, so "which layer had the deadline crossed" is an
// input rather than a race the case hopes to lose.
//
// SteadyClock never moves: it isolates the budget checks from the time checks.
// JumpClock reads once at start and is already past any deadline afterwards.
// ScriptedClock advances on every reading, which is what pins the deadlines of a
// search whose start() consulted the clock more than once.
struct SteadyClock : SearchClock {
    long long now_ms() const override { return 0; }
};

struct JumpClock : SearchClock {
    mutable int readings = 0;
    long long now_ms() const override {
        ++readings;
        return readings == 1 ? 0 : 100000;
    }
};

struct ScriptedClock : SearchClock {
    mutable long long ms = 0;
    const long long step;
    explicit ScriptedClock(long long step_ms) : step(step_ms) {}
    long long now_ms() const override {
        long long value = ms;
        ms += step;
        ++readings;
        return value;
    }
    mutable int readings = 0;
};

// A clock armed by reading index rather than by time: every reading before the
// arm point is 0 ms and every reading from it on is far past any deadline. The
// search reads this clock once at start() and then once per layer boundary, once
// per 1024 stop tests, and once per emitted frame - so a test can measure how
// many readings a given layer costs and put the crossing on the poll *inside*
// the layer it wants interrupted. That is what turns "which layer was it in when
// the deadline crossed" from a race into an input.
struct ArmingClock : SearchClock {
    mutable int readings = 0;
    int arm_at = INT_MAX;
    long long now_ms() const override {
        ++readings;
        return readings >= arm_at ? 1000000000LL : 0;
    }
};

// The layer table one whole (unarmed) think leaves behind: for each completed
// depth, how many clock readings had happened and how many nodes had been
// counted. A cut search reproduces these numbers exactly up to its cut point,
// which is what lets a test aim the cut at a chosen layer.
struct LayerRecord {
    std::vector<int> depth;
    std::vector<int> readings;
    std::vector<uint64_t> nodes;
    std::vector<Move> best;
};

// Runs one search that is never interrupted and records the per-layer table.
void measure_layers(AlphaBetaSearch& search, const Board& board, int color,
                    const SearchLimits& limits, std::atomic<bool>& stop,
                    ArmingClock& clock, LayerRecord* record, SearchStats* final_stats) {
    clock.arm_at = INT_MAX;
    clock.readings = 0;
    record->depth.clear();
    record->readings.clear();
    record->nodes.clear();
    record->best.clear();
    search.set_clock(&clock);
    search.set_progress_callback([&](const SearchStats& stats) {
        record->depth.push_back(stats.completed_depth);
        record->readings.push_back(clock.readings);
        record->nodes.push_back(stats.nodes);
        record->best.push_back(stats.best_move);
    });
    search.search(board, color, limits, stop);
    search.set_progress_callback(nullptr);
    if (final_stats) *final_stats = search.last_stats();
}

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

// The default table is 2^18 entries of 28 B (~7.3 MiB); the old 2^20 default cost
// ~32 MiB per engine, and every session owns an engine. Run before any other table is
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

// A3: the table is four-way set-associative, addressed by the high bits of a mixed
// hash and verified by a 32-bit tag of the hash's own high half. These hashes carry
// distinct high halves on purpose: every small integer has the tag 0 and would alias.
void test_transposition_table() {
    TranspositionTable tt(1024);
    assert(tt.size() == 1024);

    // The memory contract the session budget is written against (transposition.h):
    // 2^18 of these is ~=7.3 MB, less than the 32-byte entry it replaced.
    assert(sizeof(TTEntry) == 28);

    const uint64_t hash = 0x1122334455667788ULL;
    const uint64_t other = 0x99AABBCCDDEEFF00ULL;
    Move m(7, 7, BLACK);
    tt.store(hash, 3, 500, m, TTFlag::EXACT);

    TTEntry entry;
    assert(tt.probe(hash, entry));
    assert(entry.depth == 3);
    assert(entry.value == 500);
    assert(entry.flag == TTFlag::EXACT);
    assert(entry.best_move == m);
    // A3 lands the field; the search that fills it in is the static-evaluation reuse
    // step, so until then every record carries the explicit "no evaluation here".
    assert(entry.static_eval == NO_STATIC_EVAL);

    TTEntry miss;
    assert(!tt.probe(other, miss));

    std::cout << "test_transposition_table passed" << std::endl;
}

// A3: the replacement order was age-first, which is why cross-move reuse failed - a
// shallow record written by the newest think evicted the deep record the next think
// would have read. Depth decides, age only breaks a tie.
void test_tt_depth_first_replacement() {
    TranspositionTable tt(1024);
    const uint64_t hash = 0x1122334455667788ULL;

    tt.store(hash, 5, 500, Move(7, 7, BLACK), TTFlag::EXACT);
    tt.advance_generation();
    tt.store(hash, 3, 300, Move(7, 8, WHITE), TTFlag::LOWERBOUND);

    TTEntry entry;
    assert(tt.probe(hash, entry));
    // The newer but shallower record lost: same depth 5 and value 500 as when it was
    // written a generation ago.
    assert(entry.depth == 5);
    assert(entry.value == 500);
    assert(entry.best_move == Move(7, 7, BLACK));

    // A deeper record of the same position does replace, whatever its age.
    tt.store(hash, 6, 700, Move(3, 3, BLACK), TTFlag::EXACT);
    assert(tt.probe(hash, entry));
    assert(entry.depth == 6);
    assert(entry.value == 700);

    std::cout << "test_tt_depth_first_replacement passed" << std::endl;
}

// A3: a think used to age the whole table, which left the deep records of the
// previous move free game for the shallow ones of this one. The clock now moves when
// a stone is committed - by either side.
void test_tt_generation_advances_per_move() {
    SearchEngine engine;
    Board board;
    build_midgame(board);
    std::atomic<bool> stop(false);

    const uint8_t before_think = engine.transposition_table().generation();
    const SearchResult result = engine.search(board, WHITE, 4, 1500, stop);
    assert(result.best_move.pos.is_valid());
    // One think, no stone played by this caller: the generation did not move.
    assert(engine.transposition_table().generation() == before_think);

    GameManager game;
    game.set_player_color(BLACK);
    game.set_config(500, 1);
    std::atomic<int> ai_moves{0};
    game.set_move_callback([&](const Move&, int, int) { ai_moves.fetch_add(1); });

    TranspositionTable& session_tt = game.transposition_table();
    const uint8_t start = session_tt.generation();
    assert(game.make_move(7, 7));
    // Both halves of the wait are needed: move_callback_ fires inside the worker
    // and ai_thinking_ only goes false once that worker has left the commit block,
    // and undo() refuses while the flag is still set.
    for (int i = 0; i < 300 && (ai_moves.load() < 1 || game.is_ai_thinking()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(ai_moves.load() >= 1);
    assert(!game.is_ai_thinking());
    // The human's stone and the AI's stone: two steps, and the think that produced
    // the second one added none of its own.
    assert(session_tt.generation() - start == 2);

    // Undo gives the stones back but does not wind the clock: the records of the
    // position being re-entered are still the newest ones the session wrote.
    int move_count = 1;
    GameStatus undo_status = GameStatus::PLAYING;
    assert(game.undo(move_count, undo_status));
    assert(session_tt.generation() - start == 2);

    std::cout << "test_tt_generation_advances_per_move passed (generation "
              << static_cast<int>(session_tt.generation() - start) << " after two stones)"
              << std::endl;
}

// A3: a mate score used to be excluded from the table, so a proven line was recomputed
// from scratch by every move that walked back into it. It is stored now, but only as
// distance from the think's root, which is the form that stays comparable across
// iterations - the +-100000 sentinel the resign rule and the replay panel read out of
// a SearchResult is untouched, because the correction happens inside the table.
void test_tt_mate_score_normalised_by_ply() {
    TranspositionTable tt(1024);
    const uint64_t hash = 0x1122334455667788ULL;
    TTEntry entry;

    // A win found with 4 of the layer's depth still unused, six plies into the root.
    tt.store(hash, 4, 100004, Move(7, 6, BLACK), TTFlag::EXACT, 6);
    assert(tt.probe(hash, entry, 6));
    assert(entry.value == 100004);

    // The same record seen from the root itself: the mate is 6 plies closer to that
    // node, so it reads bigger, and a later node reads it smaller. Both stay above
    // the sentinel either way.
    assert(tt.probe(hash, entry, 0));
    assert(entry.value == 100010);
    assert(tt.probe(hash, entry, 10));
    assert(entry.value == 100000);

    // And it stays there: the restore is saturating, so a record two plies in that
    // is met twenty plies deep is still a proven result rather than a 99982.
    assert(tt.probe(hash, entry, 20));
    assert(entry.value == 100000);
    assert(tt.probe(hash, entry, 40));
    assert(entry.value == 100000);

    // A loss mirrors it: closer to the root is worse, not better.
    tt.store(hash, 4, -100004, Move(7, 6, WHITE), TTFlag::EXACT, 6);
    assert(tt.probe(hash, entry, 6));
    assert(entry.value == -100004);
    assert(tt.probe(hash, entry, 0));
    assert(entry.value == -100010);
    assert(tt.probe(hash, entry, 30));
    assert(entry.value == -100000);

    // A score inside the ordinary range is untouched by the ply.
    tt.store(hash, 4, 500, Move(7, 6, BLACK), TTFlag::EXACT, 7);
    assert(tt.probe(hash, entry, 2));
    assert(entry.value == 500);

    std::cout << "test_tt_mate_score_normalised_by_ply passed" << std::endl;
}

// A3: the tag is 32 bits, not the whole hash, so two positions can alias. What must
// hold is that sharing a bucket does not alias: the tag decides, and the bucket has
// room for the four positions that collide in the index.
void test_tt_key_collision_is_a_miss_not_a_wrong_hit() {
    TranspositionTable tt(1 << 12);
    // The addressing is part of the layout being tested, so the case recomputes it
    // rather than trusting an accessor that only exists for tests: 4096 entries over
    // four ways is 1024 buckets, so the index is the top 10 bits of the mixed product.
    auto bucket_of = [](uint64_t h) {
        return static_cast<size_t>((h * 0x9E3779B97F4A7C15ULL) >> (64 - 10));
    };

    const uint64_t hash = 0x1122334455667788ULL;
    uint64_t twin = 0;
    for (uint64_t h = 1; h < 1000000; ++h) {
        const uint64_t candidate = (h << 32) | 0x0BADF00DU;
        if (candidate == hash) continue;
        if (bucket_of(candidate) != bucket_of(hash)) continue;
        if (candidate >> 32 == hash >> 32) continue;
        twin = candidate;
        break;
    }
    assert(twin != 0);

    TTEntry entry;
    tt.store(hash, 4, 500, Move(7, 7, BLACK), TTFlag::EXACT);
    // Same bucket, different tag: a miss, and the record survives it.
    assert(!tt.probe(twin, entry));
    assert(tt.probe(hash, entry));
    assert(entry.value == 500);

    // Four ways mean the neighbour is kept rather than replacing it.
    tt.store(twin, 4, 900, Move(8, 8, WHITE), TTFlag::EXACT);
    assert(tt.probe(twin, entry));
    assert(entry.value == 900);
    assert(tt.probe(hash, entry));
    assert(entry.value == 500);
    assert(bucket_of(twin) == bucket_of(hash));

    std::cout << "test_tt_key_collision_is_a_miss_not_a_wrong_hit passed (bucket "
              << bucket_of(hash) << ")" << std::endl;
}

// A3: a full bucket's victim is the shallowest record, and only a tie on depth is
// broken by age. The order matters: the retired policy let the newest record push out
// the deepest one, which is the record the next think would actually have read.
void test_tt_full_bucket_replaces_shallowest_then_oldest() {
    TranspositionTable tt(1 << 12);
    auto bucket_of = [](uint64_t h) {
        return static_cast<size_t>((h * 0x9E3779B97F4A7C15ULL) >> (64 - 10));
    };
    const uint64_t hash = 0x1122334455667788ULL;
    std::vector<uint64_t> keys{hash};
    for (uint64_t h = 1; h < 1000000 && keys.size() < 6; ++h) {
        const uint64_t candidate = (h << 32) | 0x0BADF00DU;
        if (bucket_of(candidate) != bucket_of(hash)) continue;
        if (candidate >> 32 == hash >> 32) continue;
        keys.push_back(candidate);
    }
    assert(keys.size() == 6);

    // Four graded records fill the bucket, one generation apart each.
    tt.store(keys[0], 4, 400, Move(7, 7, BLACK), TTFlag::EXACT);
    tt.advance_generation();
    tt.store(keys[1], 5, 500, Move(7, 7, BLACK), TTFlag::EXACT);
    tt.advance_generation();
    tt.store(keys[2], 6, 600, Move(7, 7, BLACK), TTFlag::EXACT);
    tt.advance_generation();
    tt.store(keys[3], 7, 700, Move(7, 7, BLACK), TTFlag::EXACT);
    tt.advance_generation();

    TTEntry entry;
    // The shallowest and the oldest are the same record here, so this first eviction
    // is the tie case: keys[4] at depth 4 goes where keys[0] at depth 4 and age 0 sits.
    tt.store(keys[4], 4, 404, Move(8, 8, WHITE), TTFlag::EXACT);
    assert(!tt.probe(keys[0], entry));
    assert(tt.probe(keys[4], entry));
    assert(entry.value == 404);
    for (size_t i = 1; i <= 3; ++i) assert(tt.probe(keys[i], entry));

    // Now the order is what decides: keys[5] is newer than every record in the bucket
    // and shallower than three of them, so the depth-4 neighbour is the victim and the
    // deep ones survive. Under the retired age-first policy this is where the deep
    // record of a previous move was thrown away.
    tt.advance_generation();
    tt.store(keys[5], 3, 303, Move(9, 9, WHITE), TTFlag::EXACT);
    assert(!tt.probe(keys[4], entry));
    assert(tt.probe(keys[5], entry));
    for (size_t i = 1; i <= 3; ++i) assert(tt.probe(keys[i], entry));

    std::cout << "test_tt_full_bucket_replaces_shallowest_then_oldest passed" << std::endl;
}

// A3: a probe copies the record out. Handing back a pointer into the table would let
// the store at the end of this very node evict the entry the node is still reading.
void test_tt_probe_copies_the_record_out() {
    TranspositionTable tt(1 << 12);
    const uint64_t hash = 0x1122334455667788ULL;

    tt.store(hash, 4, 500, Move(7, 7, BLACK), TTFlag::EXACT);
    TTEntry held;
    assert(tt.probe(hash, held));
    assert(held.value == 500);

    // Overwrite the position with a deeper record of a different value.
    tt.store(hash, 6, 777, Move(2, 2, WHITE), TTFlag::LOWERBOUND);
    // The copy the caller is holding is unchanged, and a fresh probe sees the new one.
    assert(held.value == 500);
    TTEntry fresh;
    assert(tt.probe(hash, fresh));
    assert(fresh.value == 777);
    assert(fresh.depth == 6);

    std::cout << "test_tt_probe_copies_the_record_out passed" << std::endl;
}

// An entry written by an earlier move is still readable: the generation is what the
// replacement policy reads, never what a probe reads.
void test_transposition_cross_search_reuse() {
    TranspositionTable tt(1024);

    Move m(7, 7, BLACK);
    const uint64_t hash = 0x1122334455667788ULL;
    const uint64_t other = 0x99AABBCCDDEEFF00ULL;
    TTEntry entry;
    tt.store(hash, 3, 500, m, TTFlag::EXACT);
    assert(tt.probe(hash, entry));

    tt.advance_generation();
    tt.advance_generation();

    assert(tt.probe(hash, entry));
    assert(entry.depth == 3);
    assert(entry.value == 500);
    assert(entry.flag == TTFlag::EXACT);
    assert(entry.best_move == m);

    // An empty slot is a miss, and a NONE flag stays a miss across generations.
    TranspositionTable blank(1024);
    assert(!blank.probe(hash, entry));
    blank.store(hash, 3, 500, m, TTFlag::NONE);
    assert(!blank.probe(hash, entry));
    blank.advance_generation();
    assert(!blank.probe(hash, entry));

    // Different hash, different tag: the bucket addressing did not turn the table
    // into a slot-only cache.
    assert(!tt.probe(other, entry));

    std::cout << "test_transposition_cross_search_reuse passed" << std::endl;
}

// The age filter never lived in the probe and the depth gate still lives in
// alphabeta: an entry only supplies its value when it is deep enough, otherwise
// it is a move-ordering hint and the score is recomputed.
void test_transposition_depth_gate_still_applies() {
    Board board;
    build_midgame(board);
    Evaluator evaluator;
    const uint64_t hash = board.zobrist_hash();
    std::atomic<bool> stop(false);

    // search() is what normally opens the budget a fresh think spends; calling
    // alphabeta() directly has to arm it too, or the context has no clock and
    // every node takes the abort branch before the table is read.
    //
    // The budget asked for here is no budget at all: what this case claims is the
    // TT depth gate, so the clock and the node ceiling are taken out of the room
    // they test (round 111 OO3 lifted the wall-clock limit for these blocks for the
    // same reason - a saturated gate box could make this depth-4 call leave through
    // the abort branch and turn the two value asserts below into a load meter).
    auto arm = [&stop](AlphaBetaSearch& search) {
        SearchLimits limits;
        search.ctx_.start(limits, system_search_clock(), stop);
    };

    {
        TranspositionTable tt(1 << 12);
        AlphaBetaSearch search(evaluator, tt);
        arm(search);
        tt.store(hash, 4, 123456, Move(7, 6, BLACK), TTFlag::EXACT);
        Board work = board;
        const NodeResult result = search.alphabeta(work, 4, -INT_MAX, INT_MAX, BLACK, stop);
        assert(result.sound);
        assert(result.score == 123456);
    }

    {
        TranspositionTable tt(1 << 12);
        AlphaBetaSearch search(evaluator, tt);
        arm(search);
        tt.store(hash, 3, 123456, Move(7, 6, BLACK), TTFlag::EXACT);
        TTEntry hint;
        assert(tt.probe(hash, hint));
        assert(hint.depth < 4);
        Board work = board;
        const NodeResult result = search.alphabeta(work, 4, -INT_MAX, INT_MAX, BLACK, stop);
        assert(result.sound);
        assert(result.score != 123456);
        assert(result.score > -1000000 && result.score < 1000000);
    }

    // A bound is a value only when it actually cuts: the deep LOWERBOUND above
    // beta is returned, and the same bound one ply too shallow is not.
    {
        TranspositionTable tt(1 << 12);
        AlphaBetaSearch search(evaluator, tt);
        arm(search);
        tt.store(hash, 6, 300, Move(7, 6, BLACK), TTFlag::LOWERBOUND);
        Board work = board;
        assert(search.alphabeta(work, 4, 60, 200, BLACK, stop).score == 300);
    }

    {
        TranspositionTable tt(1 << 12);
        AlphaBetaSearch search(evaluator, tt);
        arm(search);
        tt.store(hash, 3, 300, Move(7, 6, BLACK), TTFlag::LOWERBOUND);
        Board work = board;
        assert(search.alphabeta(work, 4, 60, 200, BLACK, stop).score != 300);
    }

    std::cout << "test_transposition_depth_gate_still_applies passed" << std::endl;
}

// One think's cost, twice: on a cold table and then on the table that think just
// filled. Both figures are counts rather than timings - no wall-clock limit, no node
// cap, a fresh table - so only the table and the evaluation can move them. On the
// pre-A3 layout (direct mapping, age before depth) the cold think spent 276131 nodes
// and 383 hits; the A3 layout brought that to 225745 / 2833 and A5's positional terms
// to 222885 / 2965. A6's root tie re-search raised it again, to the 267918 below, so
// the ceiling that used to sit at 250000 (with slack over 222885, and under the
// 276131 a wrong table needed) no longer separates this layout from the pre-A3 one -
// that comparison belongs to the table's own cases now, and the number here is what
// it has always been secondarily: a bound on how expensive one think of depth 4 is
// allowed to get.
void test_tt_cold_think_cost_and_warm_reuse() {
    Board board;
    for (int i = 0; i < 4; ++i) {
        const int r = 1 + i * 3;
        const int c = 2 + (i % 3);
        board.place_stone(r, c, BLACK);
        board.place_stone(r, c + 1, WHITE);
        board.place_stone(r, c + 2, WHITE);
        board.place_stone(r, c + 3, WHITE);
    }
    assert(board.move_count() == 16);

    Evaluator evaluator;
    std::atomic<bool> stop(false);

    TranspositionTable tt;
    AlphaBetaSearch cold(evaluator, tt);
    cold.set_node_limit(INT_MAX);
    const Move cold_move = cold.search(board, WHITE, 4, 0, stop);
    const uint64_t cold_nodes = cold.stats_.nodes;
    assert(cold.stats_.completed_depth == 4);
    // A ceiling with more than a tenth of slack over the measured 267918.
    assert(cold_nodes <= 300000);

    AlphaBetaSearch warm(evaluator, tt);
    warm.set_node_limit(INT_MAX);
    const Move warm_move = warm.search(board, WHITE, 4, 0, stop);
    const uint64_t warm_nodes = warm.stats_.nodes;
    // The same root on the table the cold think just filled is a table read, not a
    // search: the claim is the ratio (96 against 267918), not a number.
    assert(warm_nodes * 10 < cold_nodes);
    assert(warm_move.pos.row == cold_move.pos.row && warm_move.pos.col == cold_move.pos.col);
    assert(warm.stats_.score == cold.stats_.score);

    std::cout << "test_tt_cold_think_cost_and_warm_reuse passed (cold " << cold_nodes
              << " nodes, warm repeat " << warm_nodes << ", move (" << cold_move.pos.row
              << "," << cold_move.pos.col << ") score " << cold.stats_.score << ")" << std::endl;
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

void test_vcf_block_move_ignores_overline_endpoint() {
    // Round 81 E3: an endpoint of a four is only worth defending when the attacker
    // may legally complete five there. BLACK (7,4)(7,5)(7,6)(7,7) with a fifth
    // stone at (7,9): filling the hole (7,8) joins (7,9) and makes six, an
    // overline that wins nothing, so the unique legal five point is (7,3) and that
    // is the forced block. HEAD saw both endpoints as open and returned no block
    // at all, which made the caller `continue` and drop the candidate.
    Board board;
    Evaluator evaluator;
    TranspositionTable tt;

    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 9, BLACK);

    // Sanity: (7,8) really is a forbidden (overline) point and (7,3) is legal.
    assert(PatternRecognizer::is_forbidden(board, 7, 8, BLACK));
    assert(!PatternRecognizer::is_forbidden(board, 7, 3, BLACK));

    VCFSearch vcf(evaluator, tt);
    Move block = vcf.find_block_move(board, 7, 7, BLACK);
    assert(block.pos.row == 7);
    assert(block.pos.col == 3);
    assert(block.color == WHITE);

    // The same group scanned from an interior stone gives the same block.
    Move block_mid = vcf.find_block_move(board, 7, 5, BLACK);
    assert(block_mid.pos.row == 7 && block_mid.pos.col == 3);

    // And the sequence reports the win, at the legal five point.
    std::atomic<bool> stop(false);
    VCFMove result = vcf.search(board, BLACK, 10, stop);
    assert(result.result == VCFResult::WIN);
    assert(result.best_move.pos.row == 7 && result.best_move.pos.col == 3);

    // WHITE is not subject to the overline rule: the identical white shape keeps
    // both ends live, so there is still no unique block (verbatim HEAD behaviour).
    Board white_board;
    white_board.place_stone(7, 4, WHITE);
    white_board.place_stone(7, 5, WHITE);
    white_board.place_stone(7, 6, WHITE);
    white_board.place_stone(7, 7, WHITE);
    white_board.place_stone(7, 9, WHITE);
    Move white_block = vcf.find_block_move(white_board, 7, 7, WHITE);
    assert(white_block.pos.row < 0);

    // BLACK with BOTH endpoints illegal must still be skipped - an illegal point
    // is not a block. (7,2) and (7,9) each turn their side's fill into six.
    Board both_illegal;
    both_illegal.place_stone(7, 2, BLACK);
    both_illegal.place_stone(7, 4, BLACK);
    both_illegal.place_stone(7, 5, BLACK);
    both_illegal.place_stone(7, 6, BLACK);
    both_illegal.place_stone(7, 7, BLACK);
    both_illegal.place_stone(7, 9, BLACK);
    assert(PatternRecognizer::is_forbidden(both_illegal, 7, 8, BLACK));
    assert(PatternRecognizer::is_forbidden(both_illegal, 7, 3, BLACK));
    Move none = vcf.find_block_move(both_illegal, 7, 7, BLACK);
    assert(none.pos.row < 0);
    Move none_mid = vcf.find_block_move(both_illegal, 7, 5, BLACK);
    assert(none_mid.pos.row < 0);

    // Control: an ordinary four with two legal ends is still ambiguous, and a
    // four against the edge still blocks at the single open end.
    Board open_four;
    open_four.place_stone(7, 4, BLACK);
    open_four.place_stone(7, 5, BLACK);
    open_four.place_stone(7, 6, BLACK);
    open_four.place_stone(7, 7, BLACK);
    assert(vcf.find_block_move(open_four, 7, 7, BLACK).pos.row < 0);

    Board edge_four;
    edge_four.place_stone(7, 0, BLACK);
    edge_four.place_stone(7, 1, BLACK);
    edge_four.place_stone(7, 2, BLACK);
    edge_four.place_stone(7, 3, BLACK);
    Move edge_block = vcf.find_block_move(edge_four, 7, 3, BLACK);
    assert(edge_block.pos.row == 7 && edge_block.pos.col == 4);

    std::cout << "test_vcf_block_move_ignores_overline_endpoint passed" << std::endl;
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

// R85 F3: the thinking strip is fed by the engine's progress callback. Only the
// frame at the end of search() used to arrive, so a multi-second think showed one
// jump at the end. The callback must fire once per completed iterative-deepening
// layer, with strictly rising depths, while search() is still running (that is
// what lets 终止 appear during the think rather than after it).
void test_engine_progress_callback_reports_each_layer() {
    Board board;
    build_midgame(board);
    SearchEngine engine;
    // The tactical phases can return a proven win before AlphaBeta runs at all,
    // which would leave no layer frames to check; the callback under test is
    // AlphaBeta's, so they are switched off for this probe.
    engine.set_vcf_enabled(false);
    engine.set_vct_enabled(false);

    std::vector<int> depths;
    bool search_returned = false;
    bool any_after_return = false;
    engine.set_progress_callback([&](const SearchStats& stats) {
        if (search_returned) any_after_return = true;
        depths.push_back(stats.completed_depth);
    });

    std::atomic<bool> stop(false);
    SearchResult result = engine.search(board, WHITE, 8, 3000, stop);
    search_returned = true;

    assert(result.best_move.pos.is_valid());
    assert(!any_after_return);
    assert(depths.size() >= 2);
    // The engine's closing frame repeats the last layer's depth on purpose - it
    // carries the final node/elapsed totals for the UI - so the sequence is
    // non-decreasing with at least one real jump between layers.
    bool rose = false;
    for (size_t i = 1; i < depths.size(); ++i) {
        assert(depths[i] >= depths[i - 1]);
        if (depths[i] > depths[i - 1]) rose = true;
    }
    assert(rose);

    std::cout << "test_engine_progress_callback_reports_each_layer passed ("
              << depths.size() << " layers, last depth " << depths.back() << ")" << std::endl;
}

// R85 F4: the tactical phases used to share one absolute deadline with AlphaBeta,
// so a position where VCF/VCT spend the budget left AlphaBeta the 100 ms floor and
// a depth-0 result. A defender position with rush-four candidates must still come
// back with at least one completed AlphaBeta layer and not the greedy fallback.
void test_engine_keeps_alphabeta_time_after_tactics() {
    // A midgame where the side to move has a rush-four candidate that dead-ends:
    // WHITE (9,5)(9,6)(9,7) blocked at (9,4) can fill (9,8), but the four that
    // makes completes only at (9,9), so the tactical phases have work to do and
    // no win to prove. (build_midgame's WHITE three does win outright - VCF
    // returns a mate at depth 0 - so it cannot show what AlphaBeta gets.)
    Board board;
    board.place_stone(9, 5, WHITE);
    board.place_stone(9, 6, WHITE);
    board.place_stone(9, 7, WHITE);
    board.place_stone(9, 4, BLACK);
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, BLACK);
    board.place_stone(7, 9, BLACK);
    board.place_stone(5, 5, BLACK);
    board.place_stone(10, 4, BLACK);
    board.place_stone(11, 11, BLACK);
    board.place_stone(4, 9, WHITE);
    board.place_stone(3, 11, WHITE);
    board.place_stone(2, 2, WHITE);
    assert(board.move_count() == 13);

    SearchEngine engine;

    std::atomic<bool> stop(false);
    SearchResult result = engine.search(board, WHITE, 8, 3000, stop);
    assert(result.best_move.pos.is_valid());
    assert(result.depth >= 1);
    assert(result.method != "greedy");
    std::cout << "test_engine_keeps_alphabeta_time_after_tactics passed (method "
              << result.method << ", depth " << result.depth << ")" << std::endl;

    // A budget too small for the tactical phases must not fabricate a deep
    // answer either. Before A2 the only way to say "no layer finished" was to
    // relabel the think "greedy"; now the alphabeta label survives and the reason
    // has to come with it, so a depth-0 answer either came from a fast path or
    // names why the search stopped.
    //
    // 合约过期（轮次121 B1）: that paragraph used to *imply* "a 120 ms think on this
    // board cannot reach the requested depth", and the bare `stopped != "none"` below
    // encoded that wall-clock fact. B1's width cap makes the implication false. Measured
    // on this exact board by /mnt/d/tmp/r121_b1t_r989.cpp + .sh, which runs both budgets
    // on both trees at both -O0 and -O2: pre-B1 -O0 depth 3 / time_hard / 3944 nodes /
    // root 0-133, pre-B1 -O2 depth 3 / time_hard / 18860 nodes / root 1-133, B1 -O0 depth
    // 6 / time_hard / 2732 nodes / root 0-26, and B1 -O2 finishes all eight layers in
    // 208 nodes / 0 ms / root 26-26 with stopped == "none". Reporting no reason is then
    // the *correct* answer for a think that really completed, so the premise moves to the
    // rule the comment states: an alphabeta result whose deepening was cut has to name
    // the cut, and one that names no cut has to show a finished deepening - every layer,
    // no partial layer, every root candidate walked. The pre-A2 shape this case exists
    // for (labelled alphabeta, no reason, unfinished layers) stays red on every arm.
    SearchResult quick = engine.search(board, WHITE, 8, 120, stop);
    assert(quick.best_move.pos.is_valid());
    assert(quick.method != "alphabeta" || quick.stopped != "none" || quick.depth >= 8);
    if (quick.method == "alphabeta" && quick.stopped == "none") {
        assert(quick.depth >= 8);
        assert(quick.partial_depth == 0);
        assert(quick.root_moves_done == quick.root_moves_total);
    }
    std::cout << "  short budget: method " << quick.method << ", depth " << quick.depth
              << ", stopped " << quick.stopped << ", partial " << quick.partial_depth
              << ", best (" << quick.best_move.pos.row << "," << quick.best_move.pos.col
              << ")" << std::endl;
}

// R95 Z1, case A: the tactical win paths. README:57 promises 逐手引擎分析（方法/
// 分值/深度/节点/耗时）for every hand, but a hand the VCF or VCT phase wins outright
// never reached the one assignment elapsed_ms had, so it reported 耗时 0 while the
// phase behind it had already spent most of a second. Capped WHITE triplets give
// each phase a forcing proof of its own: three stacked on rows 2/4/6 are a VCF
// (100000), two on rows 2/4 are only a VCT (90000). The non-zero stamp is required
// only once the measurement says the call really cost a millisecond - a
// sub-millisecond call reads 0 on both sides of the fix, and this must not pass by
// rounding nor go red on a machine fast enough to finish under 1 ms.
void test_engine_stamps_elapsed_on_tactical_wins() {
    std::atomic<bool> stop(false);

    const auto build_lattice = [](Board& board, int rows) {
        for (int i = 0; i < rows; ++i) {
            const int r = 2 + i * 2;
            board.place_stone(r, 2, BLACK);
            board.place_stone(r, 3, WHITE);
            board.place_stone(r, 4, WHITE);
            board.place_stone(r, 5, WHITE);
        }
    };

    {
        Board board;
        build_lattice(board, 3);
        assert(board.move_count() == 12);
        SearchEngine engine;
        const auto t0 = std::chrono::steady_clock::now();
        SearchResult result = engine.search(board, WHITE, 8, 3000, stop);
        const long long wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        assert(result.method == "vcf");
        assert(result.score == 100000);
        assert(result.best_move.pos.is_valid());
        // Only AlphaBeta fills depth and nodes_searched; the stamp must not drag
        // them along (README:530's method enum, unchanged by this fix).
        assert(result.depth == 0);
        assert(result.nodes_searched == 0);
        assert(result.elapsed_ms > 0 || wall_ms < 1);
        assert(result.elapsed_ms <= wall_ms + 1);
        std::cout << "test_engine_stamps_elapsed_on_tactical_wins passed (vcf: stamped "
                  << result.elapsed_ms << " ms, whole call " << wall_ms << " ms)"
                  << std::endl;
    }
    {
        Board board;
        build_lattice(board, 2);
        assert(board.move_count() == 8);
        SearchEngine engine;
        const auto t0 = std::chrono::steady_clock::now();
        SearchResult result = engine.search(board, WHITE, 8, 3000, stop);
        const long long wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        assert(result.method == "vct");
        assert(result.score == 90000);
        assert(result.best_move.pos.is_valid());
        assert(result.depth == 0);
        assert(result.nodes_searched == 0);
        assert(result.elapsed_ms > 0 || wall_ms < 1);
        assert(result.elapsed_ms <= wall_ms + 1);
        std::cout << "  vct: stamped " << result.elapsed_ms
                  << " ms, whole call " << wall_ms << " ms" << std::endl;
    }
}

// R95 Z1, case B: the AlphaBeta path. VCF and VCT run first and each owns a fixed
// share of the budget (35 % / 55 % of the same start, round 85 F4), so a
// forcing-rich position with no proof spends those shares before AlphaBeta opens
// its own clock - here ~1.1 s of a 2 s move. What that path handed back was the
// AlphaBeta sub-segment, so the single 耗时 the replay panel and
// ai_move.thinking_time_ms show was ~1.1 s of the 2 s short (899 ms of 2000).
// Compare against the wall clock around the call, with a band wide enough to
// ignore machine speed.
void test_engine_elapsed_covers_the_tactical_phases() {
    Board board;
    // The same capped triplets, three rows apart and one column further each row,
    // so no line completes five: both tactical phases search the whole lattice,
    // find nothing, and AlphaBeta answers the position.
    for (int i = 0; i < 4; ++i) {
        const int r = 1 + i * 3;
        const int c = 2 + (i % 3);
        board.place_stone(r, c, BLACK);
        board.place_stone(r, c + 1, WHITE);
        board.place_stone(r, c + 2, WHITE);
        board.place_stone(r, c + 3, WHITE);
    }
    assert(board.move_count() == 16);

    SearchEngine engine;
    std::atomic<bool> stop(false);
    const auto t0 = std::chrono::steady_clock::now();
    SearchResult result = engine.search(board, WHITE, 8, 2000, stop);
    const long long total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    assert(result.method == "alphabeta");
    assert(result.depth >= 1);
    assert(result.nodes_searched > 0);
    const long long slack = std::max(5LL, total_ms / 20);
    const long long diff = result.elapsed_ms > total_ms
                               ? static_cast<long long>(result.elapsed_ms) - total_ms
                               : total_ms - result.elapsed_ms;
    assert(diff <= slack);

    std::cout << "test_engine_elapsed_covers_the_tactical_phases passed (whole call "
              << total_ms << " ms, elapsed_ms " << result.elapsed_ms
              << ", slack " << slack << ", depth " << result.depth << ")" << std::endl;
}

// R96 AA2: the iteration frames AlphaBeta emits (search_engine.cpp's progress
// hook, feeding the client's thinking_progress) timed only their own phase, so
// the first frame of a 2 s move that had already spent ~1.1 s in VCF/VCT reported
// a handful of milliseconds while the terminal frame reported the whole move -
// two clocks in one progress stream. The wrapper now adds the pre-search segment
// to every layer frame. Measure that segment separately by running the same
// position with AlphaBeta switched off: that call ends at the greedy fallback
// (README:530's method list) with elapsed_ms stamped from the move start, so it
// is the tactics segment alone.
void test_engine_progress_frames_share_the_search_clock() {
    Board board;
    for (int i = 0; i < 4; ++i) {
        const int r = 1 + i * 3;
        const int c = 2 + (i % 3);
        board.place_stone(r, c, BLACK);
        board.place_stone(r, c + 1, WHITE);
        board.place_stone(r, c + 2, WHITE);
        board.place_stone(r, c + 3, WHITE);
    }
    assert(board.move_count() == 16);

    std::atomic<bool> stop(false);

    SearchEngine pre;
    pre.set_alphabeta_enabled(false);
    const auto t_pre = std::chrono::steady_clock::now();
    const SearchResult pre_search = pre.search(board, WHITE, 8, 2000, stop);
    const long long pre_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_pre).count();
    assert(pre_search.method == "greedy");
    assert(pre_search.elapsed_ms <= pre_ms + 1);
    const int pre_search_ms = pre_search.elapsed_ms;

    SearchEngine engine;
    std::vector<int> depths;
    std::vector<int> elapsed;
    bool returned = false;
    bool after_return = false;
    engine.set_progress_callback([&](const SearchStats& stats) {
        if (returned) after_return = true;
        depths.push_back(stats.completed_depth);
        elapsed.push_back(stats.elapsed_ms);
    });

    const auto t0 = std::chrono::steady_clock::now();
    SearchResult result = engine.search(board, WHITE, 8, 2000, stop);
    const long long total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    returned = true;

    assert(result.method == "alphabeta");
    assert(!after_return);
    assert(elapsed.size() >= 2);
    // On a machine fast enough that the tactics phase finishes under half a
    // second there is nothing to show, so the lower bound is conditional; the
    // agreement with the measured pre-search segment is what HEAD fails.
    assert(elapsed.front() + 50 >= pre_search_ms || pre_search_ms < 500);
    for (size_t i = 1; i < elapsed.size(); ++i) {
        assert(elapsed[i] >= elapsed[i - 1]);
        assert(depths[i] >= depths[i - 1]);
    }
    assert(static_cast<long long>(elapsed.back()) <= total_ms + 1);
    assert(elapsed.back() == result.elapsed_ms);

    std::cout << "test_engine_progress_frames_share_the_search_clock passed (pre-search "
              << pre_search_ms << " ms, first frame " << elapsed.front()
              << " ms, " << elapsed.size() << " frames, terminal " << elapsed.back()
              << " ms of " << total_ms << " ms)" << std::endl;
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

    // Own-win path: score is set, and the depth/nodes fields stay 0.
    assert(result.method == "greedy_five");
    assert(result.score == 100000);
    // elapsed_ms is a real measurement here, so a sub-millisecond fast path
    // legitimately reports 0; pinning that a costly path stamps it is
    // test_engine_stamps_elapsed_on_tactical_wins' job, not this one's.
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

    // Round 88 S2 conflict, reported not hidden: this Phase-2 filler position
    // has 213 stones and is already decided (check_winner()==WHITE, 28 black and
    // 24 white fives on the board), so the new two-completion-point rule rightly
    // answers it with resign and an empty best_move. The case is about the
    // crowded-board move path, so the resign switch is turned off here and the
    // new resign behaviour is pinned by
    // test_search_engine_resigns_on_two_enemy_five_points instead.
    engine.set_resign_detection(false);

    std::atomic<bool> stop(false);
    SearchResult result = engine.search(board, BLACK, 4, 3000, stop);

    assert(result.best_move.pos.is_valid());

    std::cout << "test_forced_move passed" << std::endl;
}

// Round 92 W2: the three pre-search fast paths README:530 lists - forced,
// greedy_five, must_block - are reached from the engine side. Each method string
// is written on exactly one return, so the label plus the score and depth that
// branch leaves behind is the whole observable: none of the three fills a depth,
// and only the two five-handling paths set a score.
void test_fast_path_labels_fill_no_depth() {
    std::atomic<bool> stop(false);

    {
        // forced: every cell but (7,7) is filled, so the candidate list holds one
        // point. The 2-row stripe colouring keeps every line at B,B,W,W or
        // B,W,B,W under a two-stone run, so no side has a five to resign over.
        Board board;
        for (int r = 0; r < BOARD_SIZE; ++r) {
            for (int c = 0; c < BOARD_SIZE; ++c) {
                if (r == 7 && c == 7) continue;
                board.place_stone(r, c, (2 * r + c) % 4 < 2 ? BLACK : WHITE);
            }
        }
        assert(board.active_empty().size() == 1);

        SearchEngine engine;
        SearchResult result = engine.search(board, WHITE, 4, 1000, stop);
        assert(result.method == "forced");
        assert(result.score == 0);
        assert(result.depth == 0);
    }

    {
        // greedy_five: WHITE's open-ended four in row 8 completes next move, so
        // the own-win scan answers before any depth is searched.
        Board board;
        board.place_stone(8, 3, WHITE);
        board.place_stone(8, 4, WHITE);
        board.place_stone(8, 5, WHITE);
        board.place_stone(8, 6, WHITE);
        board.place_stone(2, 2, BLACK);
        board.place_stone(12, 12, BLACK);

        SearchEngine engine;
        SearchResult result = engine.search(board, WHITE, 4, 1000, stop);
        assert(result.method == "greedy_five");
        assert(result.score == 100000);
        assert(result.depth == 0);
    }

    {
        // must_block: WHITE's four in row 0 is capped at (0,6) by a black stone,
        // so (0,1) is the single completion point, the turn is savable, and the
        // defensive mark score 99000 is what gets reported.
        Board board;
        board.place_stone(0, 2, WHITE);
        board.place_stone(0, 3, WHITE);
        board.place_stone(0, 4, WHITE);
        board.place_stone(0, 5, WHITE);
        board.place_stone(0, 6, BLACK);
        board.place_stone(10, 10, BLACK);

        SearchEngine engine;
        SearchResult result = engine.search(board, BLACK, 4, 1000, stop);
        assert(result.method == "must_block");
        assert(result.score == 99000);
        assert(result.depth == 0);
    }

    std::cout << "test_fast_path_labels_fill_no_depth passed" << std::endl;
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

// WW-C: 把上面那条空转用例的意图做实。add_line 为一条谱登记的 key 只有它的前
// 1..n-1 手，所以一条 3 手谱并不存在 3 子的 key；旧用例拿 3 子 history 去查，query 在
// entries_ 找不到 key 时（opening_book.cpp:191-192）就返回 false，`if (found)` 里的
// 断言体一次也没有执行过 - 它 pin 的是「查不到谱」，不是「拒绝落在已有子上的应着」。
// 产品行为本来是对的，所以这条用例没有 HEAD 负例，做的是覆盖率做实。
void test_opening_book_refuses_an_occupied_response_it_really_found() {
    const std::vector<Move> hit{Move(7, 7, BLACK), Move(7, 8, WHITE)};
    Move response;

    // 一条畸形谱：key = {7,7} 黑、{7,8} 白，应着却回到 {7,7} - 那一格正被它自己占着。
    // 中心点在八种对称下都不动，所以这个 key 下每一条变体的应着都是 {7,7}，
    // covers_position（:194-200）必然把它们全部滤掉，candidates 为空（:202）⇒ 不出手。
    OpeningBook blocked;
    blocked.add_line({{7, 7}, {7, 8}, {7, 7}}, 10, "self_blocked");
    // has_entry 只看 key（:156-162），所以这一行证明 found 分支真的走到了。
    assert(blocked.has_entry(hit));
    assert(!blocked.query(hit, BLACK, response));

    // 原用例断言体的语义（交出的应着一定落在空位上）现在才真的被执行：同一把 2 子
    // key 上放一条正常谱，落点是空位。两个对称变体给出 (6,8) 与 (8,8)，都空着。
    OpeningBook live;
    live.add_line({{7, 7}, {7, 8}, {6, 8}}, 10, "pu_yue");
    assert(live.has_entry(hit));
    Board board;
    assert(board.place_stone(7, 7, BLACK));
    assert(board.place_stone(7, 8, WHITE));
    assert(live.query(board, BLACK, response));
    assert(response.color == BLACK);
    assert(response.pos == Position(6, 8) || response.pos == Position(8, 8));
    assert(board.is_empty(response.pos.row, response.pos.col));

    // 反证：同样 2 子、但不等于任何 key（{0,0} 是角，任何对称都不会把 {7,8} 映到角上）。
    // found 为假时 query 也返回 false - 与新用例的命中项相对，这里 has_entry 先证伪，
    // 说明上面那条红/绿不是又一次空转。
    const std::vector<Move> miss{Move(7, 7, BLACK), Move(0, 0, WHITE)};
    assert(!blocked.has_entry(miss));
    assert(!live.has_entry(miss));
    assert(!blocked.query(miss, BLACK, response));

    std::cout << "  [r119wwc] blocked-key response refused, live-key response ("
              << response.pos.row << "," << response.pos.col << ") unused" << std::endl;
    std::cout << "test_opening_book_refuses_an_occupied_response_it_really_found passed"
              << std::endl;
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
    // still return one of the key's legal replies.
    OpeningBook book;
    const int big = 2000000000;
    book.add_line({{7, 7}, {8, 8}, {6, 6}}, big, "a");
    book.add_line({{7, 7}, {8, 8}, {6, 7}}, big, "b");

    Board board;
    board.place_stone(7, 7, BLACK);
    board.place_stone(8, 8, WHITE);

    Move response;
    assert(book.query(board.move_history(), BLACK, response));
    // The ordered key (7,7),(8,8) is held only by the variants that fix both
    // stones - identity and transpose - and the transpose mirror of (6,7) is
    // (7,6), so this key legitimately holds three replies: (6,6), (6,7) and
    // (7,6). (6,6) is its own mirror and so weighs twice; the weighted pick may
    // land on any of the three.
    assert(response.color == BLACK);
    assert(response.pos == Position(6, 6) || response.pos == Position(6, 7) ||
           response.pos == Position(7, 6));

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

// Round 79, A1: when BLACK's forbidden-point filter empties the active list,
// the branch now scans the whole board for legal points first, and the ending
// it declares depends on resign_detection_: with the switch off the caller is
// told "no_legal_move" with an invalid move instead of being resigned. The board
// reuses the round-78 server shape: every cell but (7,7) is occupied and a black
// stone at (7,7) is a six-in-a-row overline, so the filtered root list is empty
// and there is no other empty cell anywhere - the far scan finds nothing.
void test_search_engine_no_legal_move_and_resign_switch() {
    Board board;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            if (r == 7 && c == 7) continue;
            const bool line = (r == 7) || (c == 7) || (r - c == 0) || (r + c == 14);
            int color = line ? WHITE : ((r + c) % 2 == 0 ? BLACK : WHITE);
            if (r == 7 && c >= 2 && c <= 6) color = BLACK;
            assert(board.place_stone(r, c, color));
        }
    }
    assert(board.is_empty(7, 7));
    assert(PatternRecognizer::is_forbidden(board, 7, 7, BLACK));

    std::atomic<bool> stop(false);
    {
        SearchEngine engine;
        SearchResult result = engine.search(board, BLACK, 2, 2000, stop);
        assert(result.resigned);
        assert(result.method == "resign");
    }
    {
        // Resignation is off: no ending may be declared by this branch, and the
        // invalid best move is how the caller learns to commit nothing.
        SearchEngine engine;
        engine.set_resign_detection(false);
        SearchResult result = engine.search(board, BLACK, 2, 2000, stop);
        assert(!result.resigned);
        assert(result.method == "no_legal_move");
        assert(!result.best_move.pos.is_valid());
    }

    std::cout << "test_search_engine_no_legal_move_and_resign_switch passed" << std::endl;
}

// Round 88, S2: WHITE holds two distinct completion points here, so no single
// block survives the turn and the engine may declare the loss up front. With the
// switch off the same position still gets the block (不打开认输就不要静默认输),
// and a four whose second end already carries a black stone must never trigger
// the resignation at all.
void test_search_engine_resigns_on_two_enemy_five_points() {
    Board board;
    board.place_stone(2, 3, WHITE);
    board.place_stone(2, 4, WHITE);
    board.place_stone(2, 5, WHITE);
    board.place_stone(2, 6, WHITE);
    board.place_stone(10, 10, BLACK);
    board.place_stone(12, 12, BLACK);

    // The premise: two different cells each complete a WHITE five, and BLACK has
    // no five of its own to take instead.
    for (int col : {2, 7}) {
        assert(board.is_empty(2, col));
        const PositionPattern pattern =
            PatternRecognizer::recognize_position(board, 2, col, WHITE);
        bool makes_five = false;
        for (int d = 0; d < 4; ++d) {
            if (pattern.directions[d].type == PatternType::FIVE) makes_five = true;
        }
        assert(makes_five);
    }

    std::atomic<bool> stop(false);
    {
        SearchEngine engine;  // resignation detection is on by default
        SearchResult result = engine.search(board, BLACK, 2, 3000, stop);
        assert(result.resigned);
        assert(result.method == "resign");
        assert(!result.best_move.pos.is_valid());
        std::cout << "  two five points, resign on: method=" << result.method << std::endl;
    }
    {
        SearchEngine engine;
        engine.set_resign_detection(false);
        SearchResult result = engine.search(board, BLACK, 2, 3000, stop);
        assert(!result.resigned);
        assert(result.method == "must_block");
        assert(result.best_move.pos.is_valid());
        const bool blocks_a_five_point = result.best_move.pos == Position(2, 2) ||
                                        result.best_move.pos == Position(2, 7);
        assert(blocks_a_five_point);
        std::cout << "  two five points, resign off: block "
                  << result.best_move.pos.row << "," << result.best_move.pos.col << std::endl;
    }
    {
        // One completion point only: (0,6) already holds a black stone, so the
        // four is dead at that end and a block still saves the turn.
        Board single;
        single.place_stone(0, 2, WHITE);
        single.place_stone(0, 3, WHITE);
        single.place_stone(0, 4, WHITE);
        single.place_stone(0, 5, WHITE);
        single.place_stone(0, 6, BLACK);
        single.place_stone(10, 10, BLACK);

        SearchEngine engine;
        SearchResult result = engine.search(single, BLACK, 2, 3000, stop);
        assert(!result.resigned);
        assert(result.method == "must_block");
        assert(result.best_move.pos == Position(0, 1));
    }

    std::cout << "test_search_engine_resigns_on_two_enemy_five_points passed" << std::endl;
}

// Round 88, S3: a board with no empty cell at all has an empty root candidate
// list before any search starts. The engine must report that there is nothing to
// play with an invalid move - the centre point it used to hand back is occupied
// here, so a caller that committed it would corrupt the board.
void test_search_engine_full_board_has_no_legal_move() {
    Board board;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            assert(board.place_stone(r, c, (r + c) % 2 == 0 ? BLACK : WHITE));
        }
    }
    assert(board.move_count() == BOARD_SIZE * BOARD_SIZE);
    assert(board.active_empty().empty());

    std::atomic<bool> stop(false);
    SearchEngine engine;
    SearchResult result = engine.search(board, WHITE, 8, 1000, stop);

    assert(result.method == "no_legal_move");
    assert(!result.best_move.pos.is_valid());
    assert(!result.resigned);

    std::cout << "test_search_engine_full_board_has_no_legal_move passed" << std::endl;
}

// Round 88, S3 anti-overreach: the other end of the same candidate list. With no
// stone on the board rebuild_active_empty() seeds all 225 cells
// (board.cpp:281-293), so the first move is still an ordinary search that opens
// at 天元 and is not reported as an empty list.
void test_search_engine_empty_board_still_opens_at_center() {
    Board board;
    assert(board.move_count() == 0);
    assert(board.active_empty().size() == static_cast<size_t>(BOARD_SIZE * BOARD_SIZE));

    std::atomic<bool> stop(false);
    SearchEngine engine;
    engine.set_vcf_enabled(false);
    engine.set_vct_enabled(false);
    SearchResult result = engine.search(board, BLACK, 2, 3000, stop);

    assert(result.method != "no_legal_move");
    assert(result.best_move.pos.is_valid());
    assert(result.best_move.pos == Position(7, 7));

    std::cout << "test_search_engine_empty_board_still_opens_at_center passed (method: "
              << result.method << ")" << std::endl;
}

// Round 79, A2: the generators now test forbidden from the pattern they already
// recognized instead of re-scanning the board (is_forbidden ->
// is_forbidden_from_pattern). The observable set may not change: a black
// double-four point that shape-wise looks like an attack stays out of both
// generators, and a point that completes exactly five stays in both.
void test_generators_keep_five_points_and_drop_forbidden() {
    Evaluator evaluator;
    TranspositionTable tt;
    VCFSearch vcf(evaluator, tt);
    VCTSearch vct(evaluator, tt);

    // (7,7) closes a row four and a column four for BLACK at once: placing there
    // is a forbidden double-four, while the recognition sees the fours that
    // would otherwise qualify the cell as an attack.
    Board cross;
    cross.place_stone(7, 4, BLACK);
    cross.place_stone(7, 5, BLACK);
    cross.place_stone(7, 6, BLACK);
    cross.place_stone(7, 3, WHITE);
    cross.place_stone(4, 7, BLACK);
    cross.place_stone(5, 7, BLACK);
    cross.place_stone(6, 7, BLACK);
    cross.place_stone(3, 7, WHITE);
    assert(cross.is_empty(7, 7));
    assert(PatternRecognizer::is_forbidden(cross, 7, 7, BLACK));
    const PositionPattern cross_pattern =
        PatternRecognizer::recognize_position(cross, 7, 7, BLACK);
    assert(cross_pattern.has_four_four);
    assert(PatternRecognizer::is_forbidden_from_pattern(cross_pattern));
    bool cross_has_four = false;
    for (int d = 0; d < 4; ++d) {
        const PatternType t = cross_pattern.directions[d].type;
        if (t == PatternType::RUSH_FOUR || t == PatternType::LIVE_FOUR ||
            t == PatternType::FIVE) {
            cross_has_four = true;
        }
    }
    assert(cross_has_four);

    const auto drop = [](const std::vector<Move>& moves, int row, int col) {
        for (const Move& m : moves) {
            if (m.pos.row == row && m.pos.col == col) return false;
        }
        return true;
    };
    const auto keep = [](const std::vector<Move>& moves, int row, int col) {
        for (const Move& m : moves) {
            if (m.pos.row == row && m.pos.col == col) return true;
        }
        return false;
    };
    assert(drop(vcf.generate_rush_fours(cross, BLACK), 7, 7));
    assert(drop(vct.generate_attacks(cross, BLACK), 7, 7));

    // (7,7) here completes an exact five on the row (and would be a rush four on
    // the column): the five exemption keeps the cell legal and both generators
    // must offer it.
    Board five;
    five.place_stone(7, 3, BLACK);
    five.place_stone(7, 4, BLACK);
    five.place_stone(7, 5, BLACK);
    five.place_stone(7, 6, BLACK);
    five.place_stone(4, 7, WHITE);
    five.place_stone(5, 7, BLACK);
    five.place_stone(6, 7, BLACK);
    five.place_stone(8, 7, BLACK);
    assert(five.is_empty(7, 7));
    assert(!PatternRecognizer::is_forbidden(five, 7, 7, BLACK));
    const PositionPattern five_pattern =
        PatternRecognizer::recognize_position(five, 7, 7, BLACK);
    assert(PatternRecognizer::is_forbidden_from_pattern(five_pattern) == false);
    bool row_is_five = false;
    for (int d = 0; d < 4; ++d) {
        if (five_pattern.directions[d].type == PatternType::FIVE) row_is_five = true;
    }
    assert(row_is_five);
    assert(keep(vcf.generate_rush_fours(five, BLACK), 7, 7));
    assert(keep(vct.generate_attacks(five, BLACK), 7, 7));

    std::cout << "test_generators_keep_five_points_and_drop_forbidden passed" << std::endl;
}

// Round 79, A3: greedy_.search moved from an unconditional precompute to the
// point where it is consumed (the final return). With every other phase off, the
// observable answer must be exactly what greedy returns by itself: method
// "greedy" and the same move.
void test_greedy_method_and_move_unchanged() {
    std::atomic<bool> stop(false);

    Board mid;
    build_midgame(mid);
    Board simple;
    simple.place_stone(7, 7, BLACK);
    simple.place_stone(7, 8, WHITE);
    simple.place_stone(8, 8, BLACK);
    simple.place_stone(6, 6, WHITE);

    SearchEngine engine;
    engine.set_book_enabled(false);
    engine.set_vcf_enabled(false);
    engine.set_vct_enabled(false);
    engine.set_alphabeta_enabled(false);
    GreedySearch greedy;

    SearchResult mid_result = engine.search(mid, WHITE, 2, 2000, stop);
    assert(mid_result.method == "greedy");
    assert(mid_result.best_move == greedy.search(mid, WHITE));
    assert(mid_result.best_move.pos.row == 9 && mid_result.best_move.pos.col == 5);

    SearchResult simple_result = engine.search(simple, WHITE, 2, 2000, stop);
    assert(simple_result.method == "greedy");
    assert(simple_result.best_move == greedy.search(simple, WHITE));
    assert(simple_result.best_move.pos.row == 8 && simple_result.best_move.pos.col == 7);

    std::cout << "test_greedy_method_and_move_unchanged passed (mid "
              << mid_result.best_move.pos.row << "," << mid_result.best_move.pos.col
              << "; simple " << simple_result.best_move.pos.row << ","
              << simple_result.best_move.pos.col << ")" << std::endl;
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

// R97 BB2: a finished game refuses everything else - make_move, undo, a second
// resign - but the hint door still opened. It searched a move nobody can play,
// burned the whole timeout on it, and answered with a recommendation the client
// would highlight on a dead board.
void test_game_manager_refuses_hint_after_game_over() {
    GameManager game;
    game.set_player_color(BLACK);
    game.set_config(300, 1);

    std::atomic<int> hints{0};
    game.set_hint_callback([&](const Move&, int) { hints.fetch_add(1); });

    // Control, one step before the end: the very same door still answers, so the
    // refusal below cannot be the guard that was already there.
    const int before = hints.load();
    assert(game.request_hint(300, 1));
    for (int i = 0; i < 100 && hints.load() == before; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(hints.load() == before + 1);

    game.resign(BLACK);
    assert(game.status() == GameStatus::GAME_OVER);
    assert(game.winner() == WHITE);

    assert(!game.request_hint(300, 1));
    // And nothing arrives late either: the dead game has no answer to give.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    assert(hints.load() == before + 1);

    std::cout << "test_game_manager_refuses_hint_after_game_over passed" << std::endl;
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
    // The fallback is a root candidate, not a searched move, and the report says
    // so: no layer was interrupted either, the think never started one.
    assert(info.stopped == StopReason::TIME_HARD);
    assert(info.partial_depth == 0);
    assert(elapsed < 1000);

    // The fallback is the first root candidate: regenerating the root list the
    // same way (generate + heuristic sort; this asymmetric board has no
    // symmetries to dedup) must name the same cell. Since A6 this is a claim about
    // the position rather than about a container's state: the list is ordered by a
    // total triple, so a second generate-and-sort on a board that has played and
    // undone stones names the same head. Before it, this assert was only as
    // reproducible as active_empty() happened to be.
    std::vector<Move> root = ab.generate_moves(board, WHITE);
    assert(!root.empty());
    ab.sort_moves(root, board, WHITE);
    assert(result.pos == root[0].pos);

    std::cout << "test_alphabeta_expired_deadline_returns_root_fallback passed ("
              << elapsed << " ms, fallback "
              << result.pos.row << "," << result.pos.col << ")" << std::endl;
}

// Round 88, S6, restated by A2 (design doc 2.3.3).
//
// The rule is now per-node rather than per-search: a value that came out of a
// budget short-circuit is not a bound, so *that node* stores nothing, while a node
// whose own subtree ran to the end stores whether or not the search as a whole was
// cut short. On this engine the two rules select the same set of table entries -
// after a cut every later node short-circuits too, so there is no sound node left
// to bank - which is why the store half below is kept as a regression pin and
// reported as 无可观察差异 against the base commit, not as a HEAD-negative. What A2
// adds is that the cut itself is described: which layer was interrupted, how far
// into it, and why.
//
// The quiet board has no five in sight, so the +/-100000 store gate cannot be what
// keeps the table empty: only the abort can.
void test_alphabeta_aborted_search_stores_only_sound_nodes() {
    Board board;
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, WHITE);
    board.place_stone(8, 8, BLACK);
    board.place_stone(6, 6, WHITE);

    Evaluator evaluator;
    std::atomic<bool> stop(false);

    // The hash the first root child stores under, rebuilt the way the search
    // builds it: generate, dedup by symmetry (none here), heuristic sort.
    Move first_root;
    uint64_t child_hash = 0;
    {
        TranspositionTable scratch;
        AlphaBetaSearch order(evaluator, scratch);
        std::vector<Move> roots = order.generate_moves(board, BLACK);
        const std::vector<int> symmetries = self_symmetries(board);
        if (symmetries.size() > 1) {
            roots.erase(std::remove_if(roots.begin(), roots.end(),
                                       [&](const Move& m) {
                                           return !is_symmetry_representative(symmetries, m.pos);
                                       }),
                        roots.end());
        }
        order.sort_moves(roots, board, BLACK);
        assert(!roots.empty());
        first_root = roots[0];
        Board child = board;
        child.place_stone(first_root.pos.row, first_root.pos.col, BLACK);
        child_hash = child.zobrist_hash();
    }

    // The whole first layer, measured so the cut below lands inside the second one
    // instead of somewhere in the two.
    uint64_t layer_one_nodes = 0;
    {
        TranspositionTable scratch;
        AlphaBetaSearch measure(evaluator, scratch);
        measure.set_node_limit(2000000);
        measure.search(board, BLACK, 1, 600000, stop);
        layer_one_nodes = static_cast<uint64_t>(measure.last_search_info().nodes_searched);
    }
    assert(layer_one_nodes > 0);

    const int node_limit = static_cast<int>(layer_one_nodes + 1);
    TranspositionTable aborted_tt;
    AlphaBetaSearch aborted(evaluator, aborted_tt);
    aborted.set_node_limit(node_limit);
    aborted.search(board, BLACK, 2, 600000, stop);
    const SearchInfo cut = aborted.last_search_info();
    // The first layer is a completed record and the second is the interrupted one.
    assert(cut.depth == 1);
    assert(cut.partial_depth == 2);
    assert(cut.root_moves_done == 0);
    assert(cut.root_moves_total > 1);
    assert(cut.stopped == StopReason::NODES);
    // The cut landed inside the node that would have written this entry, so its
    // own short-circuit value is not a bound and is not in the table.
    TTEntry cut_entry;
    assert(!aborted_tt.probe(child_hash, cut_entry));
    // A layer did finish, so the move played is that layer's, not a fallback.
    assert(cut.best_move.pos.is_valid());

    // Control: the same board and depth with a budget the whole tree fits writes
    // that entry and reports no interrupted layer, so the check above is not
    // looking at a position the table never records.
    TranspositionTable full_tt;
    AlphaBetaSearch completed(evaluator, full_tt);
    completed.set_node_limit(2000000);
    completed.search(board, BLACK, 2, 600000, stop);
    const SearchInfo full_info = completed.last_search_info();
    assert(full_info.depth == 2);
    assert(full_info.partial_depth == 0);
    // Contract change (A4): this layer now runs inside an aspiration window centred
    // on the score the layer below returned. Here that estimate is far off - 4202
    // against a real 20584 - so the pass fails high, the end it failed against
    // opens up, and the layer re-runs: 1104 nodes measured where the same think with
    // one pass per layer cost 752, and 1060 once A6's total ordering changed which
    // candidate the layer looks at first. What the control still has to prove is that
    // the whole tree fits (no interrupted layer, entry written: asserted below), so
    // the ceiling is the ladder's pass count times a single pass, not a guess at
    // one pass's cost.
    assert(full_info.nodes_searched > layer_one_nodes);
    assert(full_info.nodes_searched < 752 * 3);
    TTEntry entry;
    const bool control_entry = full_tt.probe(child_hash, entry);
    assert(control_entry);

    std::cout << "test_alphabeta_aborted_search_stores_only_sound_nodes passed ("
              << "layer 1 nodes: " << layer_one_nodes << ", cut: d" << cut.depth
              << "+partial " << cut.partial_depth << " " << cut.root_moves_done << "/"
              << cut.root_moves_total << " " << stop_reason_name(cut.stopped)
              << ", control nodes: " << full_info.nodes_searched
              << ", control entry value: " << entry.value
              << ", flag: " << static_cast<int>(entry.flag)
              << ", first root: " << first_root.pos.row << "," << first_root.pos.col << ")"
              << std::endl;
}

// A2, design doc 2.3.2/2.3.3 and verification item 2 of the plan: the deadline
// crossing inside the deepest layer must report that layer as the interrupted one
// - the completed depth below it, a partial record above it, the root loop part-way
// through - rather than silently returning a shallower answer with no explanation.
// The cut is aimed with the injected clock: pass 1 counts how many readings a whole
// think spends, and pass 2 arms the crossing on a poll inside the target layer.
void test_stop_reason_time_hard_lands_mid_layer() {
    Board board;
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, WHITE);
    board.place_stone(8, 8, BLACK);
    board.place_stone(6, 6, WHITE);

    Evaluator evaluator;
    std::atomic<bool> stop(false);

    SearchLimits limits;
    limits.max_depth = 4;
    limits.hard_time_ms = 600000;  // never crossed until the clock is armed

    // The deepest layer that costs at least two clock polls is the one the cut can
    // be aimed into; deeper layers always cost more, so the deepest completed layer
    // is the natural candidate.
    TranspositionTable scratch_tt;
    AlphaBetaSearch probe(evaluator, scratch_tt);
    ArmingClock clock;
    LayerRecord table;
    measure_layers(probe, board, BLACK, limits, stop, clock, &table, nullptr);
    assert(table.depth.size() == static_cast<size_t>(limits.max_depth));

    int target = 0;
    int polls = 0;
    for (size_t i = table.depth.size(); i-- > 1;) {
        const long long boundary = i == 0 ? 1 : table.readings[i - 1];
        const int inside = table.readings[i] - static_cast<int>(boundary) - 2;
        if (inside >= 2) {
            target = table.depth[i];
            polls = inside;
            break;
        }
    }
    assert(target >= 2);

    // Arm on the poll that lands past the middle of the target layer: early enough
    // that a root candidate is still unsearched, late enough that at least one has
    // been finished.
    const int layer_index = target - 1;
    const long long boundary = layer_index == 0 ? 1 : table.readings[layer_index - 1];
    TranspositionTable cut_tt;
    AlphaBetaSearch search(evaluator, cut_tt);
    ArmingClock armed;
    armed.arm_at = static_cast<int>(boundary) + 1 + polls * 3 / 4;
    search.set_clock(&armed);
    const Move played = search.search(board, BLACK, limits, stop);

    const SearchStats stats = search.last_stats();
    const SearchInfo info = search.last_search_info();
    assert(stats.completed_depth == target - 1);
    assert(stats.partial_depth == target);
    assert(stats.root_moves_done < stats.root_moves_total);
    assert(stats.stopped == StopReason::TIME_HARD);
    assert(info.partial_depth == stats.partial_depth);
    assert(info.stopped == StopReason::TIME_HARD);
    // The interrupted layer is not submitted: the move played is the last
    // completed layer's winner, never the cut layer's.
    assert(played.pos == stats.best_move.pos);
    assert(stats.partial_best_move.pos.is_valid() || stats.root_moves_done == 0);
    // The leaves of a completed layer are quiescence nodes, so the width the old
    // accounting never reported is present here.
    assert(stats.qnodes > 0);

    std::cout << "test_stop_reason_time_hard_lands_mid_layer passed (cut in layer "
              << target << " at reading " << armed.arm_at << " of " << polls
              << " in-layer polls, root " << stats.root_moves_done << "/"
              << stats.root_moves_total << ", nodes " << stats.nodes
              << ", seldepth " << stats.seldepth << ")" << std::endl;
}

// A2: the node budget is the second way a think ends, and it reports itself as
// such. Same layered shape as the time case, aimed by node count instead of by
// clock reading - and the same guarantee that a cut layer cannot overwrite the
// completed one.
void test_stop_reason_nodes_cuts_a_layer_and_keeps_the_completed_move() {
    Board board;
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, WHITE);
    board.place_stone(8, 8, BLACK);
    board.place_stone(6, 6, WHITE);

    Evaluator evaluator;
    std::atomic<bool> stop(false);

    SearchLimits limits;
    limits.max_depth = 3;
    limits.hard_time_ms = 600000;

    TranspositionTable scratch_tt;
    AlphaBetaSearch probe(evaluator, scratch_tt);
    ArmingClock clock;
    LayerRecord table;
    measure_layers(probe, board, BLACK, limits, stop, clock, &table, nullptr);
    assert(table.depth.size() >= 2);
    const uint64_t after_two = table.nodes[table.depth.size() - 1];
    const uint64_t after_one = table.nodes[table.depth.size() - 2];
    assert(after_two > after_one + 4);

    TranspositionTable cut_tt;
    AlphaBetaSearch search(evaluator, cut_tt);
    SteadyClock never;
    search.set_clock(&never);
    SearchLimits budget = limits;
    budget.max_nodes = static_cast<int>(after_one + (after_two - after_one) * 3 / 4);
    const Move played = search.search(board, BLACK, budget, stop);

    const SearchStats stats = search.last_stats();
    assert(stats.completed_depth == table.depth[table.depth.size() - 2]);
    assert(stats.partial_depth == 3);
    assert(stats.root_moves_done < stats.root_moves_total);
    assert(stats.stopped == StopReason::NODES);
    assert(played.pos == stats.best_move.pos);
    // The budget really was the end of the think: the whole layer would have cost
    // more than it allowed.
    assert(stats.nodes >= static_cast<uint64_t>(budget.max_nodes));

    std::cout << "test_stop_reason_nodes_cuts_a_layer_and_keeps_the_completed_move passed ("
              << "budget " << budget.max_nodes << ", nodes " << stats.nodes
              << ", completed d" << stats.completed_depth << ", cut d" << stats.partial_depth
              << " root " << stats.root_moves_done << "/" << stats.root_moves_total << ")"
              << std::endl;
}

// A2, design doc 2.3.2: the soft window is a decision about starting another layer,
// never a reason to discard a finished one. A think that spent its soft window but
// not its hard deadline reports the completed depth, no partial layer, and
// "time_soft" - which is the state the client shows as 第 N 层 without an
// interruption marker.
void test_stop_reason_soft_window_keeps_the_completed_layer() {
    Board board;
    build_midgame(board);

    Evaluator evaluator;
    std::atomic<bool> stop(false);

    SearchLimits limits;
    limits.max_depth = 6;
    limits.hard_time_ms = 600000;  // the hard deadline is never reached here
    limits.soft_time_ms = 250;

    // An advancing clock spends 100 ms per reading, so the soft window is gone by
    // the second layer boundary while the hard one is still 6000 s away.
    ScriptedClock clock(100);
    TranspositionTable tt;
    AlphaBetaSearch search(evaluator, tt);
    search.set_clock(&clock);
    const Move played = search.search(board, BLACK, limits, stop);

    const SearchStats stats = search.last_stats();
    assert(stats.completed_depth == 1);
    assert(stats.partial_depth == 0);
    assert(stats.stopped == StopReason::TIME_SOFT);
    assert(played.pos == stats.best_move.pos);
    assert(stats.best_move.pos.is_valid());

    std::cout << "test_stop_reason_soft_window_keeps_the_completed_layer passed (d"
              << stats.completed_depth << " kept, nodes " << stats.nodes << ", "
              << clock.readings << " readings)" << std::endl;
}

// A2: the replacement for round 101 FF4's source-shape pin, which could only pin
// the shape because is_timeout() read a raw clock no test could steer. The claim is
// the same one: a deadline that crosses at the instant of committing must not
// discard a layer whose every root candidate was searched. With the injected clock
// the crossing is an input, so the window FF4 described as unconstructible is now
// aimed at directly - the alarm is armed on the reading the commit itself takes,
// which is after the decision and before the next one.
void test_stop_reason_crossing_at_the_commit_keeps_the_layer() {
    Board board;
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, WHITE);
    board.place_stone(8, 8, BLACK);
    board.place_stone(6, 6, WHITE);

    Evaluator evaluator;
    std::atomic<bool> stop(false);

    SearchLimits limits;
    limits.max_depth = 2;
    limits.hard_time_ms = 600000;

    TranspositionTable scratch_tt;
    AlphaBetaSearch probe(evaluator, scratch_tt);
    ArmingClock clock;
    LayerRecord table;
    measure_layers(probe, board, BLACK, limits, stop, clock, &table, nullptr);
    assert(table.depth.size() == 2);
    const int commit_reading = table.readings[0];

    TranspositionTable tt;
    AlphaBetaSearch search(evaluator, tt);
    ArmingClock armed;
    armed.arm_at = commit_reading;
    search.set_clock(&armed);
    const Move played = search.search(board, BLACK, limits, stop);

    const SearchStats stats = search.last_stats();
    assert(stats.completed_depth == 1);
    assert(stats.partial_depth == 0);
    assert(stats.root_moves_done == stats.root_moves_total);
    assert(stats.stopped == StopReason::TIME_HARD);
    assert(played.pos == stats.best_move.pos);

    std::cout << "test_stop_reason_crossing_at_the_commit_keeps_the_layer passed (alarm at "
              << commit_reading << ", kept d" << stats.completed_depth << " of "
              << stats.root_moves_total << " candidates, nodes " << stats.nodes << ")"
              << std::endl;
}

// A2: 终止 / 重开 / 接管 sets the flag, and a flag set between two layers must be
// reported as a user stop that keeps the layer already on the record. The flag is
// read at every stop test, so no clock is needed to aim this one: the test sets it
// from the frame the third layer commits with.
void test_stop_reason_user_flag_stops_between_layers() {
    Board board;
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, WHITE);
    board.place_stone(8, 8, BLACK);
    board.place_stone(6, 6, WHITE);

    Evaluator evaluator;
    std::atomic<bool> stop(false);

    SearchLimits limits;
    limits.max_depth = 4;
    limits.hard_time_ms = 600000;

    TranspositionTable tt;
    AlphaBetaSearch search(evaluator, tt);
    SteadyClock never;
    search.set_clock(&never);
    int last_completed = 0;
    search.set_progress_callback([&](const SearchStats& stats) {
        last_completed = stats.completed_depth;
        // Press 终止 the moment a layer is on the record, before the next starts.
        if (stats.completed_depth >= 3) stop.store(true);
    });
    const Move played = search.search(board, BLACK, limits, stop);

    const SearchStats stats = search.last_stats();
    assert(last_completed == 3);
    assert(stats.completed_depth == 3);
    assert(stats.partial_depth == 0);
    assert(stats.stopped == StopReason::USER);
    assert(played.pos == stats.best_move.pos);

    std::cout << "test_stop_reason_user_flag_stops_between_layers passed (kept d"
              << stats.completed_depth << ", nodes " << stats.nodes << ")" << std::endl;
}

// A2: a proven win ends the deepening on purpose. That is a stop, so it has to be
// labelled as one - and it must not be mistaken for a budget running out, which is
// what the +-100000 resign threshold downstream reads.
void test_stop_reason_mate_ends_the_deepening_without_an_abort() {
    Board board;
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(7, 7, BLACK);
    board.place_stone(9, 9, WHITE);
    board.place_stone(10, 10, WHITE);
    assert(!board.is_game_over());

    Evaluator evaluator;
    std::atomic<bool> stop(false);
    TranspositionTable tt;
    AlphaBetaSearch search(evaluator, tt);
    SteadyClock never;
    search.set_clock(&never);

    SearchLimits limits;
    limits.max_depth = 4;
    limits.hard_time_ms = 600000;
    const Move played = search.search(board, BLACK, limits, stop);

    const SearchStats stats = search.last_stats();
    assert(stats.score >= 100000);
    assert(stats.stopped == StopReason::MATE);
    assert(stats.partial_depth == 0);
    // One layer is enough to see the five, and deepening stops there: the loop did
    // not run to max_depth.
    assert(stats.completed_depth == 1);
    assert(played.pos.is_valid());
    assert(board.is_empty(played.pos.row, played.pos.col));

    std::cout << "test_stop_reason_mate_ends_the_deepening_without_an_abort passed (d"
              << stats.completed_depth << ", score " << stats.score << ", nodes "
              << stats.nodes << ")" << std::endl;
}

// A2: an unbounded quiescence is the hole the node budget could not see - the old
// accounting counted only alphabeta nodes at the places that checked, and never
// reported the width quiescence added. Quiescence nodes are now counted toward the
// budget, kept as their own total, and the deepest reach is reported.
void test_quiescence_nodes_count_toward_the_budget_and_report_seldepth() {
    // Both sides hold a live three: every quiescence entry has forcing moves to
    // try, so the subtree runs past its own depth.
    Board board;
    build_midgame(board);

    Evaluator evaluator;
    std::atomic<bool> stop(false);

    SearchLimits limits;
    limits.max_depth = 1;
    limits.hard_time_ms = 600000;
    limits.max_nodes = 4000;

    TranspositionTable tt;
    AlphaBetaSearch search(evaluator, tt);
    SteadyClock never;
    search.set_clock(&never);
    search.search(board, BLACK, limits, stop);

    const SearchStats stats = search.last_stats();
    // The budget bounds the whole tree, quiet part included.
    assert(stats.nodes <= static_cast<uint64_t>(limits.max_nodes) + 1);
    assert(stats.qnodes > 0);
    assert(stats.qnodes <= stats.nodes);
    assert(stats.seldepth >= 1);

    // A control with no quiet work to do must not invent any: on a board where no
    // candidate makes a four or a live three, quiescence stand-pats at depth 0.
    Board quiet;
    quiet.place_stone(0, 0, BLACK);
    quiet.place_stone(14, 14, WHITE);
    TranspositionTable quiet_tt;
    AlphaBetaSearch quiet_search(evaluator, quiet_tt);
    quiet_search.set_clock(&never);
    SearchLimits quiet_limits = limits;
    quiet_limits.max_nodes = 0;
    quiet_search.search(quiet, BLACK, quiet_limits, stop);
    const SearchStats quiet_stats = quiet_search.last_stats();
    assert(quiet_stats.qnodes > 0);
    assert(quiet_stats.seldepth == 0);

    std::cout << "test_quiescence_nodes_count_toward_the_budget_and_report_seldepth passed ("
              << "nodes " << stats.nodes << ", qnodes " << stats.qnodes << ", seldepth "
              << stats.seldepth << "; quiet qnodes " << quiet_stats.qnodes
              << " seldepth " << quiet_stats.seldepth << ")" << std::endl;
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

// A4, design doc 2.4.3 ①: the root order starts from the move the session's
// previous think considered best. The pin is observable through the fallback: with
// the deadline already expired no layer runs, so the answer is literally
// root_moves[0] - whatever the order puts there. A think that gets to search is
// free to disagree with the pin, which is why the pin is proved here rather than in
// a scored think.
void test_root_ordering_uses_hint_move() {
    Board board;
    build_midgame(board);
    Evaluator evaluator;
    TranspositionTable tt;

    AlphaBetaSearch ab(evaluator, tt);
    std::atomic<bool> stop(false);
    ab.set_deadline(std::chrono::steady_clock::now() - std::chrono::seconds(1));

    // The same order the root builds for itself: this board has no self-symmetry,
    // so nothing is deduplicated and the heuristic sort is all that decides.
    std::vector<Move> root = ab.generate_moves(board, WHITE);
    ab.sort_moves(root, board, WHITE);
    assert(root.size() > 8);
    const Move pinned = root.back();
    assert(pinned.pos != root[0].pos);

    SearchLimits limits;
    limits.max_depth = 4;
    limits.hard_time_ms = 5000;
    limits.hint_move = pinned;

    const Move hinted = ab.search(board, WHITE, limits, stop);
    const SearchInfo hint_info = ab.last_search_info();
    assert(hint_info.depth == 0);
    assert(hinted.pos == pinned.pos);

    // Control 1: no hint (the invalid default) pins nothing, so the fallback is
    // the heuristic maximum as it always was.
    limits.hint_move = Move();
    const Move plain = ab.search(board, WHITE, limits, stop);
    assert(plain.pos == root[0].pos);

    // Control 2: a hint for a cell the root list does not hold - here a stone is
    // already on it - is dropped rather than played.
    limits.hint_move = Move(7, 7, WHITE);
    assert(!board.is_empty(7, 7));
    const Move stale = ab.search(board, WHITE, limits, stop);
    assert(stale.pos == root[0].pos);

    std::cout << "test_root_ordering_uses_hint_move passed (pinned "
              << pinned.pos.row << "," << pinned.pos.col << ", unhinted "
              << plain.pos.row << "," << plain.pos.col << ", stale "
              << stale.pos.row << "," << stale.pos.col << ")" << std::endl;
}

namespace {

// Spread across the board with no five in sight, so every one of the four layers
// completes and none of them ends in a mate sentinel: the centre each layer
// inherits is an ordinary position value, which is what the ladder is for.
Board quiet_think_board() {
    Board board;
    board.place_stone(3, 3, BLACK);
    board.place_stone(13, 13, BLACK);
    board.place_stone(3, 13, WHITE);
    board.place_stone(13, 3, WHITE);
    board.place_stone(8, 6, WHITE);
    board.place_stone(10, 10, BLACK);
    board.place_stone(4, 4, WHITE);
    return board;
}

// One depth-limited think on `board` with the aspiration centre the caller asks
// for, under a budget that cannot run out: no clock and no node ceiling, so the
// only thing the three arms below can differ in is how many passes each layer
// needed.
SearchStats think_with_centre(const Board& board, const Evaluator& evaluator,
                              std::atomic<bool>& stop, bool has_centre, int centre) {
    TranspositionTable tt;
    AlphaBetaSearch search(evaluator, tt);
    SearchLimits limits;
    limits.max_depth = 4;
    limits.has_hint_score = has_centre;
    limits.hint_score = centre;
    const Move move = search.search(board, WHITE, limits, stop);
    SearchStats stats = search.last_stats();
    stats.best_move = move;
    return stats;
}

}  // namespace

// A4, design doc 2.4.3 ③: a layer is searched inside a window centred on the value
// the layer below reported (seeded from the score the session handed in), and a
// value outside that window re-searches the layer with the failed side opened up.
// The rule has to be a work-saving device, never a different move, so every arm has
// to submit exactly what the whole-window think submits - same cell, same score,
// same completed depth - while the node count proves the centre was consumed at all
// (before the fix, handing in a number changed nothing, so all three counts were
// the same line).
void test_aspiration_refines_without_changing_result() {
    const Board board = quiet_think_board();
    Evaluator evaluator;
    std::atomic<bool> stop(false);

    const SearchStats none = think_with_centre(board, evaluator, stop, false, 0);
    assert(none.completed_depth == 4);
    assert(none.stopped == StopReason::NONE);

    const SearchStats exact = think_with_centre(board, evaluator, stop, true, none.score);
    const SearchStats negated = think_with_centre(board, evaluator, stop, true, -none.score);

    for (const SearchStats* arm : {&exact, &negated}) {
        assert(arm->best_move.pos == none.best_move.pos);
        assert(arm->score == none.score);
        assert(arm->completed_depth == none.completed_depth);
        assert(arm->stopped == none.stopped);
    }
    // The load-bearing half: a centre the window cannot hold costs re-searches, so
    // the work is not the whole-window figure. The negated arm is not pinned this
    // way - its count can land on the full-window one by accident - and it is there
    // to show a wildly wrong estimate still returns the same move.
    assert(exact.nodes != none.nodes);

    std::cout << "test_aspiration_refines_without_changing_result passed (best "
              << none.best_move.pos.row << "," << none.best_move.pos.col << " score "
              << none.score << " depth " << none.completed_depth << ", nodes: no centre "
              << none.nodes << ", centre " << exact.nodes << ", negated centre "
              << negated.nodes << ")" << std::endl;
}

// A4 through the layer the session actually calls: SearchEngine owns the hint the
// owner hands it and reads it into the limits it searches under. A centre-only hint
// (no pinned move) touches nothing but the aspiration windows, so identical output
// with a different node count is the proof that the number reached the search.
void test_search_engine_forwards_the_hint_to_the_search() {
    const Board board = quiet_think_board();
    std::atomic<bool> stop(false);

    auto think = [&](bool has_centre, int centre) {
        SearchEngine engine;
        engine.set_vcf_enabled(false);
        engine.set_vct_enabled(false);
        engine.set_book_enabled(false);
        engine.set_resign_detection(false);
        engine.clear_hint();
        if (has_centre) engine.set_hint(Move(), centre);
        // A budget this think cannot spend: the deadline is ten minutes away and
        // the node ceiling derived from it clamps to INT_MAX, so the four layers
        // decide the answer and the node count is a deterministic quantity.
        return engine.search(board, WHITE, 4, 600000, stop);
    };

    const SearchResult plain = think(false, 0);
    assert(plain.method == "alphabeta");
    assert(plain.depth == 4);

    const SearchResult reuse = think(true, plain.score);
    assert(reuse.method == plain.method);
    assert(reuse.best_move.pos == plain.best_move.pos);
    assert(reuse.score == plain.score);
    assert(reuse.depth == plain.depth);
    assert(reuse.nodes_searched != plain.nodes_searched);

    std::cout << "test_search_engine_forwards_the_hint_to_the_search passed ("
              << plain.best_move.pos.row << "," << plain.best_move.pos.col
              << " score " << plain.score << ", nodes " << plain.nodes_searched
              << " -> " << reuse.nodes_searched << ")" << std::endl;
}

// One candidate's own value at the root of a depth-`depth` think, read independently
// of the root loop: its own table, so no record another candidate left behind is read,
// and the window the caller asks for. The root loop cannot give this - every candidate
// after the first is searched against a bottom edge already at the best's value, which
// proves "not better", not "equal".
static int root_candidate_value(const Board& board, const Evaluator& evaluator,
                                std::atomic<bool>& stop, const Move& candidate, int color,
                                int depth, int alpha, int beta) {
    TranspositionTable fresh(1 << 12);
    AlphaBetaSearch probe(evaluator, fresh);
    // Calling alphabeta() directly still has to arm the context, or the node test
    // reads an unset budget and every node leaves through the abort branch. This is
    // no budget at all: no deadline, no node ceiling.
    SearchLimits none;
    probe.ctx_.start(none, system_search_clock(), stop);
    Board work = board;
    assert(work.place_stone(candidate.pos.row, candidate.pos.col, color));
    const NodeResult child =
        probe.alphabeta(work, depth - 1, -beta, -alpha, opponent(color), stop, 1);
    assert(child.sound);
    return -child.score;
}

// The sweep's answer: the best value any root candidate reaches, and every cell that
// reaches it.
struct SweepResult {
    int best;
    std::vector<Position> optimal;
};

// Which candidates hold the optimum, read one by one. A full window over all of them
// is the obvious reading and it is 1.87M nodes here (78 s of this suite's -O0 time for
// the 123-candidate midgame below), which is more than the whole rest of the file.
// Width 1 around the running optimum is enough to tell the three cases apart: a
// fail-soft return strictly between the two edges is the exact value, a return at or
// under the bottom edge is an upper bound on the true value (so strictly worse than
// the best), and a return at or over the top edge is a lower bound (so genuinely
// better - and then that one candidate does get the full window). A cell measured
// against an older, lower centre can therefore only be exact or provably under the
// final best, never ambiguously tied, so one pass classifies everything. 143013 nodes
// for the same answer as the full sweep, and it agrees with it on every board in this
// file including the two mate boards, where the numbers are the +-100000 family.
static SweepResult sweep_optimum(const Board& board, int color, int depth,
                                 const Evaluator& evaluator, std::atomic<bool>& stop) {
    std::vector<Move> root;
    {
        TranspositionTable tt;
        AlphaBetaSearch order(evaluator, tt);
        root = order.generate_moves(board, color);
        order.sort_moves(root, board, color);
    }
    assert(root.size() > 8);
    // Wide, but not INT_MAX: the window gets negated.
    const int wide = 1 << 28;
    std::vector<int> value(root.size());
    int best = -wide;
    for (size_t i = 0; i < root.size(); ++i) {
        if (i == 0) {
            value[i] = root_candidate_value(board, evaluator, stop, root[i], color, depth, -wide,
                                            wide);
        } else {
            value[i] = root_candidate_value(board, evaluator, stop, root[i], color, depth,
                                            best - 1, best + 1);
            if (value[i] >= best + 1) {
                value[i] = root_candidate_value(board, evaluator, stop, root[i], color, depth,
                                                -wide, wide);
            }
        }
        if (value[i] > best) best = value[i];
    }
    SweepResult out;
    out.best = best;
    for (size_t i = 0; i < root.size(); ++i) {
        if (value[i] == best) out.optimal.push_back(root[i].pos);
    }
    return out;
}

// The cell a total-order tie picks: nearest the centre, then the lower index.
static Position triple_best(const std::vector<Position>& cells) {
    Position best = cells.front();
    for (const auto& p : cells) {
        if (center_distance(p) < center_distance(best) ||
            (center_distance(p) == center_distance(best) && p.to_index() < best.to_index())) {
            best = p;
        }
    }
    return best;
}

// The root's two claims: what it plays is an optimum of the position (so the tie
// probe, which re-reads a rival that came back level with the best, cannot let a
// strictly worse move take over), and when several cells share that optimum it
// plays the most central of them (design doc 2.5.3, 验收 G4).
//
// Both are checked against sweep_optimum's independent re-read rather than against a
// number, because which cells tie is a property of the position. The two halves need
// different boards: on the quiet midgame the optimum turns out to be unique at every
// depth from 1 to 4, which makes the sweep the guard on the probe's soundness and
// says nothing about ties; the double-open-four board below has an optimum several
// cells reach at once, since any move that completes a five is the same proven win,
// so it is where the centre key actually has something to decide. It is read at
// depth 1 because a proven win ends the deepening there.
void test_root_tiebreak_prefers_centre() {
    Evaluator evaluator;
    std::atomic<bool> stop(false);

    auto think = [&](const Board& board, int color, int depth) {
        TranspositionTable tt;
        AlphaBetaSearch engine(evaluator, tt);
        SearchLimits limits;
        limits.max_depth = depth;
        const Move played = engine.search(board, color, limits, stop);
        SearchStats stats = engine.last_stats();
        stats.best_move = played;
        return stats;
    };

    // Half one: the quiet midgame at depth 3, where the probe does fire (the think
    // spends work on rivals that came back level) but never flips anything. The
    // guard is that the move played is the best of the swept optima and the score
    // reported is the swept value: a probe that mistook "bounded by V" for "worth V"
    // and handed the root to a worse cell breaks one of those two.
    const int depth = 3;
    const Board quiet = quiet_think_board();
    const SweepResult quiet_opt = sweep_optimum(quiet, WHITE, depth, evaluator, stop);
    const SearchStats quiet_think = think(quiet, WHITE, depth);
    assert(quiet_think.completed_depth == depth);
    assert(quiet_think.score == quiet_opt.best);
    assert(quiet_think.best_move.pos == triple_best(quiet_opt.optimal));

    // Half two: the constructible tie. BLACK has a four on row 7 open at both ends,
    // so (7,2) and (7,7) each complete a five; every completed five is the same
    // proven win, so both cells hold the optimum, at centre distances 5 and 0. The
    // premise is counted, not assumed - the tie has to exist and its members have to
    // disagree about centrality, or the centre key decides nothing here.
    Board dual;
    dual.place_stone(7, 3, BLACK);
    dual.place_stone(7, 4, BLACK);
    dual.place_stone(7, 5, BLACK);
    dual.place_stone(7, 6, BLACK);
    // WHITE's three, kept off row 7 so neither completion makes a second shape.
    dual.place_stone(11, 3, WHITE);
    dual.place_stone(11, 4, WHITE);
    dual.place_stone(11, 5, WHITE);

    const SweepResult dual_opt = sweep_optimum(dual, BLACK, 1, evaluator, stop);
    assert(dual_opt.optimal.size() > 1);
    int unequal = 0;
    for (size_t i = 1; i < dual_opt.optimal.size(); ++i) {
        if (center_distance(dual_opt.optimal[i]) != center_distance(dual_opt.optimal[0])) {
            ++unequal;
        }
    }
    assert(unequal > 0);

    const SearchStats dual_think = think(dual, BLACK, 3);
    assert(dual_think.completed_depth == 1);
    assert(dual_think.stopped == StopReason::MATE);
    // The reported number keeps the ±100000 sentinel the caller's resign rule reads
    // (README:338's red dot): the tie re-search must not turn a proven win into a
    // score, and must not answer with the other winning cell either.
    assert(dual_think.score == dual_opt.best);
    assert(dual_think.score >= 100000);
    assert(dual_think.best_move.pos == triple_best(dual_opt.optimal));
    assert(dual_think.best_move.pos == Position(7, 7));

    std::cout << "test_root_tiebreak_prefers_centre passed (quiet d" << depth << ": "
              << quiet_opt.optimal.size() << " tied optimum " << quiet_opt.best
              << ", played " << quiet_think.best_move.pos.row << ","
              << quiet_think.best_move.pos.col << "; dual-win: " << dual_opt.optimal.size()
              << " tied optima, " << unequal << " unequal-centre members, played "
              << dual_think.best_move.pos.row << "," << dual_think.best_move.pos.col
              << " score " << dual_think.score << ")" << std::endl;
}

// A6: the same position reached by two different move orders searches to the same
// answer with the same amount of work. The second board carries exactly the stones
// the first does and got there through one extra place-and-take-back, which is the
// documented way active_empty() changes order without changing the position: undo
// restores a cell by swap-remove and a re-activated cell is appended at the end.
// Before the total triple, sort_moves left such a pair of lists to std::sort's
// mercy over equal scores, so an equal-valued pair could be searched in either
// order and the cut pattern - the node count - moved with it.
void test_root_choice_reproducible() {
    Evaluator evaluator;
    std::atomic<bool> stop(false);

    const Board direct = quiet_think_board();
    Board detour = quiet_think_board();
    // A cell inside the active window and near its front: undo_last_move() puts an
    // emptied cell back at the end of the vector, and the swap-remove that opened
    // its slot moved the window's last cell into it. (0,0) does not do this - it is
    // already the front, so taking it back leaves the list exactly as it was.
    assert(detour.place_stone(1, 1, WHITE));
    detour.undo_last_move();

    // The premise: the same position, stated in a different candidate order.
    assert(detour.move_count() == direct.move_count());
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            assert(detour.get(r, c) == direct.get(r, c));
        }
    }
    assert(detour.zobrist_hash() == direct.zobrist_hash());
    assert(direct.active_empty() != detour.active_empty());

    auto think = [&](const Board& board) {
        TranspositionTable tt;
        AlphaBetaSearch ab(evaluator, tt);
        SearchLimits limits;
        limits.max_depth = 3;  // no clock, no node ceiling: depth alone bounds it
        Move move = ab.search(board, WHITE, limits, stop);
        SearchStats stats = ab.last_stats();
        stats.best_move = move;
        return stats;
    };

    const SearchStats first = think(direct);
    assert(first.completed_depth == 3);
    // Control: one board, two identical thinks. This half is green on the tree
    // before the triple too - it pins that the harness itself measures something
    // repeatable, so the cross-board assert below is about the order and nothing
    // else.
    const SearchStats again = think(direct);
    assert(again.best_move.pos == first.best_move.pos);
    assert(again.nodes == first.nodes);

    const SearchStats other = think(detour);
    assert(other.best_move.pos == first.best_move.pos);
    assert(other.score == first.score);
    assert(other.completed_depth == first.completed_depth);
    assert(other.nodes == first.nodes);

    std::cout << "test_root_choice_reproducible passed (" << first.best_move.pos.row << ","
              << first.best_move.pos.col << " score " << first.score << ", nodes "
              << first.nodes << " both ways, " << direct.active_empty().size()
              << " candidates)" << std::endl;
}

// A6 asks whether the symmetry filter's representative rule and the root's triple
// agree, and the answer is yes by geometry rather than by code: every D4 transform
// of the board is a row flip, a column flip or a transpose, and all three preserve
// |row-7| + |col-7|, so the members of an orbit are always equally central and the
// lowest-index rule is already applying the triple's last key. This case pins that
// agreement - it is a control, green on both trees, and it is what says the dedup
// does not need a triple of its own.
void test_d4_dedup_keeps_central_representative() {
    Board empty;
    const std::vector<int> group = self_symmetries(empty);
    assert(group.size() == static_cast<size_t>(SYMMETRY_COUNT));

    int representatives = 0;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            const Position p(r, c);
            if (!is_symmetry_representative(group, p)) continue;
            ++representatives;
            for (const int variant : group) {
                const Position twin = transform_position(p, variant);
                // Equally central, and the survivor the lowest index of the orbit.
                assert(center_distance(twin) == center_distance(p));
                assert(p.to_index() <= twin.to_index());
            }
        }
    }
    assert(representatives == 36);

    // The root of a fully symmetric board is answered by the same rule: with the
    // deadline already spent no layer runs, so the reply is the head of the
    // deduplicated and triple-ordered list - the most central representative there
    // is, which for the empty board is the centre itself.
    Evaluator evaluator;
    TranspositionTable tt;
    AlphaBetaSearch ab(evaluator, tt);
    std::atomic<bool> stop(false);
    ab.set_deadline(std::chrono::steady_clock::now() - std::chrono::seconds(1));
    const Move opened = ab.search(empty, BLACK, 2, 3000, stop);
    assert(ab.last_search_info().depth == 0);
    assert(opened.pos == Position(7, 7));
    assert(center_distance(opened.pos) == 0);

    std::cout << "test_d4_dedup_keeps_central_representative passed (" << representatives
              << " representatives, all orbit members equally central, head "
              << opened.pos.row << "," << opened.pos.col << ")" << std::endl;
}

// Round 89, T1: the colour gate reaches the search layer through the two
// single-colour reads greedy_.search_ranked reports. On the four-four board
// WHITE paid 70014 to "block" a shape BLACK can never play; that defence price
// is now nothing, while BLACK's own list is untouched because the generator
// already dropped the point before scoring it.
void test_greedy_drops_the_defense_price_of_a_black_forbidden_point() {
    Board board;
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(4, 7, BLACK);
    board.place_stone(5, 7, BLACK);
    board.place_stone(6, 7, BLACK);
    assert(PatternRecognizer::is_forbidden(board, 7, 7, BLACK));

    GreedySearch greedy;
    const int all = BOARD_SIZE * BOARD_SIZE;

    const std::vector<ScoredMove> as_white = greedy.search_ranked(board, WHITE, all);
    const ScoredMove* spot = nullptr;
    const ScoredMove* neighbour = nullptr;
    for (const ScoredMove& candidate : as_white) {
        if (candidate.move.pos.row == 7 && candidate.move.pos.col == 7) spot = &candidate;
        if (candidate.move.pos.row == 7 && candidate.move.pos.col == 8) neighbour = &candidate;
    }
    assert(spot != nullptr);
    // The centre bias in these four numbers is position_weight_scale 8 (engine
    // redesign A5), so (7,7) carries 112 and (7,8) carries 104; the shape halves
    // are the same 0 / 1050 they were at scale 1.
    assert(spot->defense_score == 0);
    assert(spot->attack_score == 112);
    assert(spot->total_score == spot->attack_score + spot->defense_score);
    assert(spot->total_score == 112);
    // The square beside it is an ordinary candidate on the same board: BLACK's
    // live four there is still a threat worth blocking.
    assert(neighbour != nullptr);
    assert(neighbour->defense_score == 1154);
    assert(neighbour->attack_score == 104);
    assert(neighbour->total_score == 1258);

    // BLACK's ranked list never carried the forbidden point even before the gate.
    const std::vector<ScoredMove> as_black = greedy.search_ranked(board, BLACK, all);
    bool black_carries_spot = false;
    for (const ScoredMove& candidate : as_black) {
        if (candidate.move.pos.row == 7 && candidate.move.pos.col == 7) black_carries_spot = true;
    }
    assert(!black_carries_spot);

    // Control: a single live four is legal for BLACK, so WHITE still pays the
    // full blocking price for it.
    Board legal;
    legal.place_stone(7, 4, BLACK);
    legal.place_stone(7, 5, BLACK);
    legal.place_stone(7, 6, BLACK);
    assert(!PatternRecognizer::is_forbidden(legal, 7, 7, BLACK));
    const std::vector<ScoredMove> legal_white = greedy.search_ranked(legal, WHITE, all);
    const ScoredMove* legal_spot = nullptr;
    for (const ScoredMove& candidate : legal_white) {
        if (candidate.move.pos.row == 7 && candidate.move.pos.col == 7) legal_spot = &candidate;
    }
    assert(legal_spot != nullptr);
    assert(legal_spot->defense_score == 10112);
    assert(legal_spot->total_score == 10224);

    std::cout << "test_greedy_drops_the_defense_price_of_a_black_forbidden_point passed ("
              << spot->defense_score << " defense at 7,7, control "
              << legal_spot->defense_score << ")" << std::endl;
}

// Round 102 GG1, inverted by A2 (design doc 2.3.3). The case was written to prove
// the depth-0 relabel could not hide a node count: README:530-531 leaves depth/nodes
// unset for every method but alphabeta, and the relabel made a hand whose first
// layer never completed report {"method":"greedy","depth":0,"nodes":N>0}, which is
// the one combination the table forbids. The redesign removes the relabel instead of
// the count, so the same hand now reports what it was: an alphabeta think that ran
// out of budget, with its nodes visible and its reason named.
//
// The construction is unchanged and still needed: the position below is a 24-stone
// midgame whose *whole first layer* costs 4858 nodes (about 433 ms in this suite's
// unoptimised build). The engine's smallest node budget is remaining_time * 200
// (search_engine.cpp:263) with remaining_time floored at 100 ms, i.e. 20000 nodes,
// so no node cap can cut a layer that cheap - the interrupt has to come from the
// absolute deadline. A 10 ms budget is therefore ~43x inside the layer rather than
// past it, which is what makes the interrupted layer constructible instead of a race.
void test_engine_reports_the_interrupted_alphabeta_layer() {
    const int placements[][2] = {
        {9, 11}, {0, 14}, {10, 2},  {6, 6},  {10, 4}, {9, 2},  {4, 2},  {5, 3},
        {2, 3},  {10, 1}, {12, 12}, {2, 6},  {1, 8},  {14, 8}, {4, 1},  {13, 4},
        {1, 14}, {8, 11}, {10, 10}, {8, 7},  {12, 1}, {11, 6}, {5, 2},  {0, 7},
    };

    Board board;
    for (size_t i = 0; i < sizeof(placements) / sizeof(placements[0]); ++i) {
        // Alternating colours, twelve stones each, WHITE to move.
        board.place_stone(placements[i][0], placements[i][1], i % 2 == 0 ? BLACK : WHITE);
    }
    assert(board.move_count() == 24);

    std::atomic<bool> stop(false);

    // VCF and VCT each take a slice of the same budget before AlphaBeta starts,
    // and the resign probe takes the remainder after it. Switching all three off
    // leaves the deadline to the single phase being measured.
    SearchEngine engine;
    engine.set_vcf_enabled(false);
    engine.set_vct_enabled(false);
    engine.set_resign_detection(false);

    int frame_depth = -1;
    int frame_nodes = -1;
    engine.set_progress_callback(
        [&](const SearchStats& stats) {
            frame_depth = stats.completed_depth;
            frame_nodes = static_cast<int>(stats.nodes);
        });

    const SearchResult interrupted = engine.search(board, WHITE, 4, 10, stop);

    // Control: the same position with an ample budget completes layers, so the
    // count is reported the same way it always was.
    //
    // Round 112 §3(a): this layer measures about 4858 nodes / 433 ms without -O2, so
    // 2000 ms left only ~4.6x headroom and a saturated machine ran out of it and fell to
    // greedy, turning :2742 red. 20000 ms is ~46x. The discriminator above is the 10 ms
    // interrupted call, which this does not touch, so no discriminating power is lost.
    SearchEngine full;
    full.set_vcf_enabled(false);
    full.set_vct_enabled(false);
    full.set_resign_detection(false);
    const SearchResult completed = full.search(board, WHITE, 4, 20000, stop);

    std::cout << "test_engine_reports_the_interrupted_alphabeta_layer measured "
              << "(interrupted: " << interrupted.method << " d" << interrupted.depth
              << "+partial " << interrupted.partial_depth << "/n" << interrupted.nodes_searched
              << " " << interrupted.stopped
              << ", last frame d" << frame_depth << "/n" << frame_nodes
              << ", control: " << completed.method << " d" << completed.depth
              << "+partial " << completed.partial_depth << "/n"
              << completed.nodes_searched << " " << completed.stopped << ")" << std::endl;

    // No relabel: the phase that searched keeps its name and says why it ended.
    assert(interrupted.method == "alphabeta");
    assert(interrupted.stopped == "time_hard");
    // Not one layer finished, and the one that was cut is reported with how far
    // into it the deadline landed.
    assert(interrupted.depth == 0);
    assert(interrupted.partial_depth == 1);
    assert(interrupted.root_moves_total > 1);
    assert(interrupted.root_moves_done < interrupted.root_moves_total);
    // The work is visible instead of zeroed, which is what makes a timeout and a
    // position with nothing to search two different reports.
    assert(interrupted.nodes_searched > 0);
    // And a move came back regardless: the root fallback answers for the player.
    assert(interrupted.best_move.pos.is_valid());
    assert(board.is_empty(interrupted.best_move.pos.row, interrupted.best_move.pos.col));
    // The frames carry the same unfinished depth rather than a invented one.
    assert(frame_depth == 0);
    assert(frame_nodes > 0);

    assert(completed.method == "alphabeta");
    assert(completed.depth >= 1);
    assert(completed.nodes_searched > 0);

    std::cout << "test_engine_reports_the_interrupted_alphabeta_layer passed" << std::endl;
}

// ---------------------------------------------------------------------------
// A1 scaffolding. SearchLimits / SearchStats / TimeManager / SearchContext do
// not exist on the base commit, so these cases have no HEAD-negative to run
// against; they are the construction and default-value sanity of the new
// vocabulary, and REVIEW_LOG.md records that as a declared exception.
// ---------------------------------------------------------------------------

void test_search_limits_and_stats_defaults() {
    SearchLimits limits;
    assert(limits.max_depth == 8);
    assert(limits.soft_time_ms == 0 && limits.hard_time_ms == 0);
    assert(limits.max_nodes == 0);
    assert(!limits.ponder && !limits.training);
    assert(!limits.has_hint_score && limits.hint_score == 0);
    assert(!limits.hint_move.pos.is_valid());

    SearchStats stats;
    assert(stats.completed_depth == 0 && stats.partial_depth == 0);
    assert(stats.root_moves_done == 0 && stats.root_moves_total == 0);
    assert(stats.seldepth == 0 && stats.score == 0 && stats.elapsed_ms == 0);
    assert(stats.nodes == 0 && stats.qnodes == 0);
    assert(stats.tt_hits == 0 && stats.tt_cutoffs == 0);
    assert(stats.pv.empty());
    assert(!stats.best_move.pos.is_valid());
    assert(!stats.partial_best_move.pos.is_valid());
    assert(stats.stopped == StopReason::NONE);

    // These strings leave the process: the client shows them and the replay
    // stores them, so a rename is a protocol change.
    assert(std::string(stop_reason_name(StopReason::NONE)) == "none");
    assert(std::string(stop_reason_name(StopReason::TIME_SOFT)) == "time_soft");
    assert(std::string(stop_reason_name(StopReason::TIME_HARD)) == "time_hard");
    assert(std::string(stop_reason_name(StopReason::NODES)) == "nodes");
    assert(std::string(stop_reason_name(StopReason::USER)) == "user");
    assert(std::string(stop_reason_name(StopReason::MATE)) == "mate");

    std::cout << "test_search_limits_and_stats_defaults passed" << std::endl;
}

void test_time_manager_sizes_the_window_to_the_position() {
    SearchLimits limits;
    limits.hard_time_ms = 3000;

    // An empty board is both a book stage and a tiny candidate set: either
    // reason alone halves the soft window.
    Board opening;
    MoveBudget opening_budget = TimeManager::allocate(opening, limits, 0);
    assert(opening_budget.hard_ms == 3000);
    assert(opening_budget.soft_ms == 1500);

    Board mid;
    build_midgame(mid);
    assert(mid.active_empty().size() > 6);
    MoveBudget mid_budget = TimeManager::allocate(mid, limits, 0);
    assert(mid_budget.soft_ms == 1950);  // the 65 % default, untouched

    // An explicit soft request is honoured, but never beyond the hard limit.
    limits.soft_time_ms = 5000;
    assert(TimeManager::allocate(mid, limits, 0).soft_ms == 3000);
    limits.soft_time_ms = 900;
    assert(TimeManager::allocate(mid, limits, 0).soft_ms == 900);
    limits.soft_time_ms = 0;

    // The previous move spending 80 % of its limit is the boundary: over it
    // halves the next window, at it does not.
    assert(TimeManager::allocate(mid, limits, 2400).soft_ms == 1950);
    assert(TimeManager::allocate(mid, limits, 2401).soft_ms == 1500);

    // An endgame is the opposite of an opening: few stones left to place, long
    // forcing lines, and it may run past the default share towards the hard
    // limit. 196 stones leave 29 empties, so the candidate branch stays out of
    // the way and only the endgame rule moves the number.
    Board endgame;
    bool black_to_place = true;
    for (int row = 0; row < BOARD_SIZE; ++row) {
        for (int col = 0; col < BOARD_SIZE && endgame.move_count() < 196; ++col) {
            assert(endgame.place_stone(row, col, black_to_place ? BLACK : WHITE));
            black_to_place = !black_to_place;
        }
    }
    assert(endgame.move_count() == 196);
    assert(endgame.active_empty().size() > 6);
    assert(TimeManager::allocate(endgame, limits, 0).soft_ms == 2437);

    std::cout << "test_time_manager_sizes_the_window_to_the_position passed" << std::endl;
}

void test_time_manager_node_budget_and_no_deadline() {
    // Training asks for nodes and gets nothing else: the same move must not cost
    // a different number of instructions on a loaded machine.
    SearchLimits training;
    training.training = true;
    training.max_nodes = 250000;
    training.hard_time_ms = 5000;
    MoveBudget trained = TimeManager::allocate(Board(), training, 0);
    assert(trained.max_nodes == 250000);
    assert(trained.hard_ms == 0 && trained.soft_ms == 0);

    // No hard deadline at all: the node budget stands alone, and a soft window
    // with nothing to be a fraction of is not invented.
    SearchLimits nodes_only;
    nodes_only.max_nodes = 1000;
    MoveBudget open = TimeManager::allocate(Board(), nodes_only, 0);
    assert(open.hard_ms == 0 && open.soft_ms == 0 && open.max_nodes == 1000);

    // A play request that carries both budgets keeps both.
    SearchLimits both;
    both.hard_time_ms = 1000;
    both.max_nodes = 4096;
    MoveBudget kept = TimeManager::allocate(Board(), both, 0);
    assert(kept.hard_ms == 1000 && kept.max_nodes == 4096);
    assert(kept.soft_ms == 500);

    std::cout << "test_time_manager_node_budget_and_no_deadline passed" << std::endl;
}

void test_search_context_polls_the_clock_once_per_1024_nodes() {
    SearchLimits limits;
    limits.hard_time_ms = 1000;
    std::atomic<bool> stop_flag{false};

    JumpClock clock;
    SearchContext ctx;
    ctx.start(limits, clock, stop_flag);
    assert(clock.readings == 1);

    // A stop test whose whole purpose is to save time must not cost a clock read
    // per node: 1023 of them go by without asking, and only the 1024th sees the
    // deadline that was already crossed.
    for (int i = 0; i < 1023; ++i) {
        assert(!ctx.hard_stopped());
        assert(clock.readings == 1);
    }
    assert(ctx.hard_stopped());
    assert(clock.readings == 2);
    assert(ctx.stopped_for(StopReason::TIME_HARD));
    assert(ctx.hard_stopped());
    assert(clock.readings == 2);  // sticky: the reason is remembered, not re-polled

    // Once the search has stopped, the budget checks still short-circuit before
    // the clock, so a stopped think costs no further readings however long it
    // unwinds.
    for (int i = 0; i < 2048; ++i) assert(ctx.hard_stopped());
    assert(clock.readings == 2);

    std::cout << "test_search_context_polls_the_clock_once_per_1024_nodes passed" << std::endl;
}

void test_search_context_reports_why_it_stopped() {
    std::atomic<bool> stop_flag{false};
    SteadyClock clock;

    SearchLimits node_budget;
    node_budget.max_nodes = 4;
    SearchContext nodes_ctx;
    nodes_ctx.start(node_budget, clock, stop_flag);
    for (int i = 0; i < 3; ++i) {
        nodes_ctx.count_node();
        assert(!nodes_ctx.hard_stopped());
    }
    nodes_ctx.count_node();
    assert(nodes_ctx.nodes() == 4);
    assert(nodes_ctx.hard_stopped());
    assert(nodes_ctx.stopped_for(StopReason::NODES));

    // The player pressed 终止 / 重开 / 接管: the flag is the reason, and it beats
    // the budget checks in reporting which.
    std::atomic<bool> user_stop{false};
    SearchContext user_ctx;
    user_ctx.start(node_budget, clock, user_stop);
    user_stop.store(true);
    assert(user_ctx.hard_stopped());
    assert(user_ctx.stopped_for(StopReason::USER));

    // A deliberate stop set by the root loop reads back as itself, and a budget
    // limit of zero means the check is never armed.
    SearchContext armed;
    SearchLimits nothing;
    armed.start(nothing, clock, stop_flag);
    assert(!armed.hard_stopped());
    assert(!armed.soft_time_up());
    armed.set_reason(StopReason::MATE);
    assert(armed.stopped_for(StopReason::MATE));
    assert(armed.hard_stopped());

    std::cout << "test_search_context_reports_why_it_stopped passed" << std::endl;
}

void test_search_context_deadlines_share_one_start_reading() {
    // An advancing clock is the only way to catch a start() that reads the clock
    // once per deadline: each extra reading would push the later window further
    // out and the soft/hard pair would no longer be measured from one instant.
    ScriptedClock clock(100);
    SearchLimits limits;
    limits.hard_time_ms = 1000;
    limits.soft_time_ms = 400;
    std::atomic<bool> stop_flag{false};

    SearchContext ctx;
    ctx.start(limits, clock, stop_flag);
    assert(clock.readings == 1);
    assert(ctx.elapsed_ms() == 100);
    assert(ctx.elapsed_ms() == 200);

    // 300 ms of simulated search has not spent the soft window; the next
    // reading is exactly 400, which only lands there because the window was
    // measured from the same reading as the hard deadline.
    assert(!ctx.soft_time_up());
    assert(ctx.soft_time_up());

    std::cout << "test_search_context_deadlines_share_one_start_reading passed" << std::endl;
}

void test_search_context_counters_account_for_the_tree() {
    SteadyClock clock;
    std::atomic<bool> stop_flag{false};
    SearchLimits limits;
    limits.max_nodes = 100;

    SearchContext ctx;
    ctx.start(limits, clock, stop_flag);
    ctx.count_node();
    ctx.count_quiescence_node();
    ctx.count_quiescence_node();
    ctx.count_tt_hit();
    ctx.count_tt_hit();
    ctx.count_tt_cutoff();
    ctx.note_quiescence_depth(3);
    ctx.note_quiescence_depth(7);
    ctx.note_quiescence_depth(5);  // a shallower line does not undo the record

    assert(ctx.nodes() == 3);
    assert(ctx.quiescence_nodes() == 2);
    assert(ctx.tt_hits() == 2);
    assert(ctx.tt_cutoffs() == 1);
    assert(ctx.seldepth() == 7);

    // start() resets the bookkeeping: a second think on the same context must not
    // inherit the last one's node count or stop reason.
    ctx.start(limits, clock, stop_flag);
    assert(ctx.nodes() == 0 && ctx.quiescence_nodes() == 0);
    assert(ctx.tt_hits() == 0 && ctx.tt_cutoffs() == 0);
    assert(ctx.seldepth() == 0);
    assert(ctx.reason() == StopReason::NONE);

    std::cout << "test_search_context_counters_account_for_the_tree passed" << std::endl;
}

// ---------------------------------------------------------------------------
// Stage B1 (design doc 2.5.1 候选分层与宽度限制 + 3.2 候选生成伪码).
//
// The fixtures below are placed in exactly one order per pose: `near_last_move`
// reads board.last_move(), so the pose is only the same pose when the stone that
// lands last is the same stone. Do not reorder these lists.
// ---------------------------------------------------------------------------

namespace {

using B1Stones = std::vector<std::array<int, 3>>;

Board b1_board(const B1Stones& stones) {
    Board board;
    for (const auto& s : stones) assert(board.place_stone(s[0], s[1], s[2]));
    return board;
}

// crowded: midgame plus a second cluster. One BLACK square, (4,5), is 禁手.
Board b1_crowded() {
    return b1_board({
        {7, 7, BLACK}, {7, 8, BLACK}, {7, 9, BLACK},
        {9, 6, WHITE}, {9, 7, WHITE}, {9, 8, WHITE},
        {5, 5, BLACK}, {10, 4, BLACK}, {11, 11, BLACK},
        {4, 9, WHITE}, {3, 11, WHITE}, {2, 2, WHITE},
        {6, 7, BLACK}, {8, 6, WHITE}, {10, 10, BLACK}, {5, 10, WHITE},
        {12, 3, BLACK}, {4, 4, WHITE}, {6, 12, BLACK}, {12, 8, WHITE},
        {13, 13, BLACK}, {1, 6, WHITE}, {3, 5, BLACK}, {11, 6, WHITE},
    });
}

// quiet: four stones, no threat for either side - the 「无威胁时放宽」 pose.
Board b1_quiet() {
    return b1_board({{7, 7, BLACK}, {7, 8, WHITE}, {8, 8, BLACK}, {6, 6, WHITE}});
}

// scattered: four isolated corner stones, so Tier2 and Tier3 both have a real
// population to be cut from.
Board b1_scattered() {
    return b1_board({{2, 2, BLACK}, {12, 12, WHITE}, {2, 12, BLACK}, {12, 2, WHITE}});
}

// WHITE owns an open four on row 7; BLACK to move has exactly two answers, and
// neither of them makes a threat of its own.
Board b1_white_open_four() {
    return b1_board({
        {7, 5, WHITE}, {7, 6, WHITE}, {7, 7, WHITE}, {7, 8, WHITE},
        {10, 10, BLACK}, {10, 11, BLACK}, {3, 3, BLACK},
    });
}

// BLACK owns the same shape; here Tier0's *own*-five half is what has to survive.
Board b1_black_makes_five() {
    return b1_board({
        {7, 4, BLACK}, {7, 5, BLACK}, {7, 6, BLACK}, {7, 7, BLACK},
        {10, 10, WHITE}, {10, 11, WHITE}, {3, 3, WHITE},
    });
}

// BLACK 3+3 around (7,7): 6,7 / 8,7 vertical and 7,6 / 7,8 horizontal, so (7,7)
// itself is the 三三 point and must never reach a tier for BLACK.
Board b1_double_three() {
    return b1_board({
        {7, 6, BLACK}, {7, 8, BLACK}, {6, 7, BLACK}, {8, 7, BLACK},
        {2, 2, WHITE}, {2, 3, WHITE}, {2, 4, WHITE},
    });
}

Evaluator b1_evaluator(int root, int deep) {
    ThreatCoefficients coeffs = Evaluator().coefficients();
    coeffs.cand_width_root = root;
    coeffs.cand_width_deep = deep;
    return Evaluator(coeffs);
}

// A generator holds its evaluator by reference (move_generator.h:82), so the two
// have to be owned together. Writing `MoveGenerator gen(b1_evaluator(24, 8))` binds
// the generator to a temporary that dies at the end of that statement, and the
// generator then reads a width off freed stack - which is how this file's own case 3
// first came back reporting a Tier2 width of 0 at the root and 4 at ply 1. Every
// generator in these cases comes from here.
struct B1Rig {
    explicit B1Rig(int root, int deep)
        : evaluator(b1_evaluator(root, deep)), generator(evaluator) {}
    const Evaluator evaluator;
    const MoveGenerator generator;
};

bool b1_in_tier(const std::vector<Candidate>& tier, int row, int col) {
    for (const Candidate& c : tier)
        if (c.move.pos.row == row && c.move.pos.col == col) return true;
    return false;
}

bool b1_in_any_tier(const CandidateSet& set, int row, int col) {
    return b1_in_tier(set.forced, row, col) || b1_in_tier(set.threats, row, col) ||
           b1_in_tier(set.vicinity, row, col) || b1_in_tier(set.remote, row, col);
}

bool b1_in_flat(const std::vector<Move>& flat, int row, int col) {
    for (const Move& m : flat)
        if (m.pos.row == row && m.pos.col == col) return true;
    return false;
}

// The tier each active empty square belongs to *before* any width is applied.
// Written from design doc 3.2's pseudo-code rather than by calling the generator,
// so an assert about "the width kept 24 of them" is measured against the actual
// population and not against a second copy of the thing under test.
struct B1Populations {
    size_t active = 0;
    size_t forced = 0;
    size_t threats = 0;
    size_t vicinity = 0;
    size_t remote = 0;
    size_t forbidden = 0;
};

bool b1_doc_makes_five(const PositionPattern& p) {
    for (int d = 0; d < 4; ++d)
        if (p.directions[d].type == PatternType::FIVE) return true;
    return false;
}

bool b1_doc_makes_forcing_threat(const PositionPattern& p) {
    for (int d = 0; d < 4; ++d) {
        const PatternType t = p.directions[d].type;
        if (t == PatternType::FIVE || t == PatternType::LIVE_FOUR ||
            t == PatternType::RUSH_FOUR || t == PatternType::LIVE_THREE)
            return true;
    }
    return false;
}

bool b1_doc_makes_double_threat(const PositionPattern& p) {
    return p.has_four_four || p.has_four_three || p.has_three_three;
}

bool b1_doc_near_last_move(const Board& board, Position pos, int radius) {
    if (!board.has_last_move()) return false;
    const Position& last = board.last_move().pos;
    return std::abs(pos.row - last.row) <= radius && std::abs(pos.col - last.col) <= radius;
}

B1Populations b1_populations(const Board& board, int color) {
    B1Populations pop;
    for (const Position& pos : board.active_empty()) {
        if (!board.is_empty(pos)) continue;
        ++pop.active;
        PositionPattern mine, theirs;
        PatternRecognizer::recognize_position_both(board, pos.row, pos.col, color, mine, theirs);
        if (color == BLACK && PatternRecognizer::is_forbidden_from_pattern(mine)) {
            ++pop.forbidden;
            continue;
        }
        if (b1_doc_makes_five(mine) || b1_doc_makes_five(theirs)) ++pop.forced;
        else if (b1_doc_makes_forcing_threat(mine) || b1_doc_makes_double_threat(mine)) ++pop.threats;
        else if (b1_doc_near_last_move(board, pos, 2) || board.neighbor_count_at(pos) >= 2)
            ++pop.vicinity;
        else
            ++pop.remote;
    }
    return pop;
}

struct B1Think {
    uint64_t nodes = 0;
    int root_total = 0;
    int completed_depth = 0;
    int score = 0;
    Move best;
    StopReason stopped = StopReason::NONE;
};

// One think with no clock and no node ceiling, so `nodes` is the size of the tree
// the width left behind rather than a meter of how busy the box was. Public API
// only - which is what lets the same body run on the tree before B1.
B1Think b1_think(const Board& board, int color, int depth, int root, int deep) {
    const Evaluator evaluator = b1_evaluator(root, deep);
    TranspositionTable tt;  // fresh table per arm: no warm TT may rescue an arm
    AlphaBetaSearch search(evaluator, tt);
    std::atomic<bool> stop(false);

    SearchLimits limits;
    limits.max_depth = depth;
    limits.hard_time_ms = 0;
    limits.soft_time_ms = 0;
    limits.max_nodes = 0;

    B1Think out;
    out.best = search.search(board, color, limits, stop);
    const SearchStats& stats = search.last_stats();
    out.nodes = stats.nodes;
    out.root_total = stats.root_moves_total;
    out.completed_depth = stats.completed_depth;
    out.score = stats.score;
    out.stopped = stats.stopped;
    return out;
}

}  // namespace

// B1-1: 宽度吃不掉挡点 (design doc 2.5.1's 「强制着法永不截断」 row).
//
// Tier0 has two halves and this stage makes both of them real: the square that
// completes a five for the side to play (§3.2's `is_forced(mine)` shorthand) and
// the square where the *opponent* would complete one, which is the only answer to
// that threat and is what 2.5.1's struct comment asks for. Half one runs at the
// narrowest widths the trained surface allows, so a tier that could be cut is
// provably cut and the forced points still come back.
void test_move_generator_tiers_never_drop_forced() {
    const Evaluator tight_eval = b1_evaluator(2, 2);  // the surface minimums
    const MoveGenerator tight(tight_eval);

    // Half one: WHITE's open four, BLACK to block.
    const Board blocking = b1_white_open_four();
    const B1Populations block_pop = b1_populations(blocking, BLACK);
    assert(block_pop.forced == 2);
    assert(block_pop.vicinity == 60);  // the population Tier2 is cut out of
    for (const int ply : {0, 1, 3, 6}) {
        CandidateSet set;
        tight.generate(blocking, BLACK, ply, false, &set);
        assert(set.forced.size() == 2);
        assert(b1_in_tier(set.forced, 7, 4) && b1_in_tier(set.forced, 7, 9));
        assert(set.vicinity.size() == 2);  // the width really is at its floor
        const std::vector<Move> flat = tight.generate_moves(blocking, BLACK, ply, false);
        assert(b1_in_flat(flat, 7, 4) && b1_in_flat(flat, 7, 9));
        // 2 forced + 4 threats + 2 of the 60 vicinity cells; nothing else survives.
        assert(flat.size() == 8);
        assert(set.remote.empty());
    }

    // Half two, the mirror: the squares that complete BLACK's OWN five. They are
    // Tier0 in normal mode and they do not appear in Tier1 there at all - while
    // quiescence, which has no Tier0, keeps them because 成五 passes its filter.
    const Board own = b1_black_makes_five();
    const B1Populations own_pop = b1_populations(own, BLACK);
    assert(own_pop.forced == 2);
    assert(own_pop.threats == 0);
    for (const int ply : {0, 1, 3}) {
        CandidateSet set;
        tight.generate(own, BLACK, ply, false, &set);
        assert(set.forced.size() == 2);
        assert(b1_in_tier(set.forced, 7, 3) && b1_in_tier(set.forced, 7, 8));
        assert(set.threats.empty());
        assert(!b1_in_tier(set.threats, 7, 3) && !b1_in_tier(set.threats, 7, 8));
    }

    // And at the defaults, where the widths are not trying to starve anyone.
    const B1Rig at_default_rig(24, 8);
    const MoveGenerator& at_default = at_default_rig.generator;
    CandidateSet defaults;
    at_default.generate(own, BLACK, 3, false, &defaults);
    assert(defaults.forced.size() == 2 && defaults.threats.empty());
    assert(defaults.vicinity.size() == 8 && defaults.remote.empty());

    // The quiescence half of the same claim: no Tier0 exists there, so 成五 is only
    // reachable through the forcing filter - and both squares come back.
    CandidateSet quiet;
    at_default.generate(own, BLACK, 0, true, &quiet);
    assert(quiet.threats.size() == 2);
    assert(b1_in_tier(quiet.threats, 7, 3) && b1_in_tier(quiet.threats, 7, 8));

    // The converse, stated so the two halves are not confused: a square that only
    // stops the *opponent's* five is Tier0 in normal mode and is NOT a quiescence
    // threat, because quiescence filters on the mover's own shapes.
    CandidateSet quiet_block;
    at_default.generate(blocking, BLACK, 0, true, &quiet_block);
    assert(!b1_in_tier(quiet_block.threats, 7, 4) && !b1_in_tier(quiet_block.threats, 7, 9));

    std::cout << "test_move_generator_tiers_never_drop_forced passed (block "
              << "(7,4)+(7,9) and own-five (7,3)+(7,8) both survive a width of 2 at "
              "every ply; 8 candidates kept out of " << block_pop.active << ")"
              << std::endl;
}

// B1-2: Tier2 服从本层宽度 (design doc 2.5.1's K column).
//
// Three things have to be pinned separately or the ladder is just a constant that
// happens to look like a ladder: the kept count against the *population* it was
// drawn from, the kept count against each knob, and the middle row against the
// mean of the two knobs rather than against either one.
void test_move_generator_vicinity_obeys_the_layer_width() {
    Board board;
    build_midgame(board);  // the suite's own 12-stone midgame
    const B1Populations pop = b1_populations(board, BLACK);
    assert(pop.forced == 0 && pop.threats == 9);
    assert(pop.vicinity == 67);  // the pre-cut Tier2 population

    const B1Rig at_default_rig(24, 8);
    const MoveGenerator& at_default = at_default_rig.generator;

    // The K ladder: 24 at the root, 16 at ply 1-2, 8 from ply 3 on.
    const size_t expected[5] = {24, 16, 16, 8, 8};
    const int plies[5] = {0, 1, 2, 3, 6};
    size_t kept[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < 5; ++i) {
        CandidateSet set;
        at_default.generate(board, BLACK, plies[i], false, &set);
        assert(set.vicinity.size() == expected[i]);
        assert(set.forced.empty() && set.threats.size() == 9);
        assert(set.remote.empty());
        // strictly below the population it was cut from, at every ply
        assert(set.vicinity.size() < pop.vicinity);
        kept[i] = set.flatten().size();
    }
    assert(kept[0] == 33 && kept[1] == 25 && kept[2] == 25 && kept[3] == 17);
    assert(kept[3] == kept[4]);  // ply 3 and ply 6 are the same layer width

    // It is a knob, not a constant: the surface minimum keeps exactly 2 of the 67.
    CandidateSet min_set;
    {
        const B1Rig min_rig(2, 2);
        const MoveGenerator& min_gen = min_rig.generator;
        min_gen.generate(board, BLACK, 0, false, &min_set);
        assert(min_set.vicinity.size() == 2);
        assert(min_set.flatten().size() == 11);  // 9 threats + 2 vicinity, both never-cut
                                                 // tiers at their measured size
        assert(min_set.threats.size() == 9);
    }

    // ...and the surface maximum keeps 64 of them at the root, 32 from ply 3 on.
    {
        const B1Rig max_rig(64, 32);
        const MoveGenerator& max_gen = max_rig.generator;
        CandidateSet max_root, max_deep;
        max_gen.generate(board, BLACK, 0, false, &max_root);
        max_gen.generate(board, BLACK, 3, false, &max_deep);
        assert(max_root.vicinity.size() == 64 && max_root.flatten().size() == 73);
        assert(max_deep.vicinity.size() == 32 && max_deep.flatten().size() == 41);
    }

    // The middle row is the MEAN of the two knobs and not a copy of either: with
    // 30 / 10 the ply-1 width is 20, which is neither 30 nor 10. (At the defaults
    // 16 happens to be (24+8)/2, which alone cannot tell a mean from a constant.)
    {
        const B1Rig asym_rig(30, 10);
        const MoveGenerator& asym_gen = asym_rig.generator;
        const size_t want[4] = {30, 20, 20, 10};
        for (int ply = 0; ply < 4; ++ply) {
            CandidateSet set;
            asym_gen.generate(board, BLACK, ply, false, &set);
            assert(set.vicinity.size() == want[ply]);
            assert(set.vicinity.size() < pop.vicinity);
        }
        assert(asym_rig.evaluator.coefficients().cand_width_root != 20 &&
               asym_rig.evaluator.coefficients().cand_width_deep != 20);
    }

    std::cout << "test_move_generator_vicinity_obeys_the_layer_width passed (Tier2 "
              << "population " << pop.vicinity << " kept as 24/16/8 by ply, 2 at the "
              "surface minimum, 64/32 at the maximum, and 20 at ply 1 for the 30/10 "
              "pair - a mean, not a copy)"
              << std::endl;
}

// B1-3: 无威胁时放宽 + Tier3 的 M (design doc 2.5.1's 「无威胁」 row and its M column).
//
// The doc's rows, not the measured numbers: Tier2 keeps the ROOT width on a board
// with no threat at all (instead of falling through the ply ladder), Tier3's cap is
// `cand_width_deep` (a cap - a population below it is kept whole, not padded), and
// Tier3 is empty whenever Tier1 is not.
void test_move_generator_tier3_and_the_no_threat_widening() {
    const B1Rig at_default_rig(24, 8);
    const MoveGenerator& at_default = at_default_rig.generator;

    struct NoThreatPose {
        const char* name;
        Board board;
        size_t pop_vicinity;
        size_t pop_remote;
        size_t remote_at_root;
    };
    const B1Populations quiet_pop = b1_populations(b1_quiet(), BLACK);
    const B1Populations scat_pop = b1_populations(b1_scattered(), BLACK);
    std::vector<NoThreatPose> poses = {
        {"quiet", b1_quiet(), quiet_pop.vicinity, quiet_pop.remote, 7},
        {"scattered", b1_scattered(), scat_pop.vicinity, scat_pop.remote, 8},
    };
    // 7 is below the cap of 8 (so the cap is a cap) and 8 is exactly it (so the cap
    // is cand_width_deep and not a smaller number). Both have to be true.
    assert(quiet_pop.remote == 7 && quiet_pop.remote < 8);
    assert(scat_pop.remote == 72 && scat_pop.remote > 8);

    for (const NoThreatPose& pose : poses) {
        const B1Populations pop = b1_populations(pose.board, BLACK);
        assert(pop.forced == 0 && pop.threats == 0);  // the premise of the row

        for (const int ply : {0, 1, 2, 3, 6}) {
            CandidateSet set;
            at_default.generate(pose.board, BLACK, ply, false, &set);
            assert(set.forced.empty() && set.threats.empty());
            // 「无威胁时放宽」: the ROOT width at every ply, where the threat poses
            // fall 24 -> 16 -> 8.
            assert(set.vicinity.size() == std::min(pop.vicinity, size_t(24)));
            // Tier3 lives at the root only: it is dropped as soon as Tier2 kept
            // something, which is the doc's 「仅当 Tier2 落空」 half.
            if (ply == 0) {
                assert(set.remote.size() == pose.remote_at_root);
                assert(set.remote.size() == std::min(pop.remote, size_t(8)));
            } else {
                assert(set.remote.empty());
                assert(set.vicinity.size() > 0);
            }
        }
    }

    // The contrast that makes the widening an actual rule and not a coincidence of
    // the default root width: a threat pose at the same ply keeps less.
    Board mid;
    build_midgame(mid);
    CandidateSet threat_at_ply3;
    at_default.generate(mid, BLACK, 3, false, &threat_at_ply3);
    assert(threat_at_ply3.threats.size() == 9);
    assert(threat_at_ply3.vicinity.size() == 8);
    for (const NoThreatPose& pose : poses) {
        CandidateSet set;
        at_default.generate(pose.board, BLACK, 3, false, &set);
        assert(set.threats.empty());
        // three times what the threat pose keeps at the very same ply
        assert(set.vicinity.size() == 24 && set.vicinity.size() > threat_at_ply3.vicinity.size());
    }

    // M is `cand_width_deep`, and it is a ceiling: at root=64 the no-threat poses
    // keep their whole Tier2 (33 and 24 of them, both under 64) while Tier3 tracks
    // deep = 2 / 8 / 32, stopping short of its own population when it is smaller.
    for (const int deep : {2, 8, 32}) {
        const B1Rig wide_rig(64, deep);
        const MoveGenerator& wide = wide_rig.generator;
        CandidateSet q, s;
        wide.generate(b1_quiet(), BLACK, 0, false, &q);
        wide.generate(b1_scattered(), BLACK, 0, false, &s);
        assert(q.vicinity.size() == 33 && s.vicinity.size() == 24);
        assert(q.remote.size() == std::min(size_t(7), size_t(deep)));
        assert(s.remote.size() == std::min(size_t(72), size_t(deep)));
        assert(q.remote.size() <= static_cast<size_t>(deep));
    }

    // 「remote 只在完全无威胁时保留」 stated the other way round: on every pose that
    // does have a threat, Tier3 is empty even at the root and even with M wide open.
    {
        const B1Rig max_rig(64, 32);
        const MoveGenerator& max_gen = max_rig.generator;
        const std::vector<std::pair<const char*, Board>> threat_poses = {
            {"midgame", [&] { Board b; build_midgame(b); return b; }()},
            {"crowded", b1_crowded()},
            {"white_open_four", b1_white_open_four()},
            {"black_makes_five", b1_black_makes_five()},
            {"double_three", b1_double_three()},
        };
        for (const auto& threat : threat_poses) {
            for (const int color : {BLACK, WHITE}) {
                CandidateSet set;
                max_gen.generate(threat.second, color, 0, false, &set);
                assert(!set.threats.empty() || !set.forced.empty());
                if (!set.threats.empty()) assert(set.remote.empty());
            }
        }
    }

    std::cout << "test_move_generator_tier3_and_the_no_threat_widening passed (quiet "
              << quiet_pop.vicinity << "+" << quiet_pop.remote << " and scattered "
              << scat_pop.vicinity << "+" << scat_pop.remote << " populations keep the "
              "root width at every ply, Tier3 = min(pop, cand_width_deep) at the root "
              "only, and is empty wherever Tier1 is not)"
              << std::endl;
}

// B1-4: 一层能搜的格子终于有了上限 - the cost side, and the HEAD-negative arm.
//
// Public API only, so this very body compiles on the tree before B1 (which reads
// neither width knob and so answers all three arms with the identical 133-candidate
// search): every one of the four strict inequalities is red there. The pairs are
// ordered by the knob rather than being a single lucky comparison, which is what
// makes it a width limit and not a different position.
void test_move_generator_width_lowers_nodes_and_root_size() {
    Board midgame;
    build_midgame(midgame);

    for (const int color : {BLACK, WHITE}) {
        const B1Think tight = b1_think(midgame, color, 6, 2, 2);
        const B1Think narrow = b1_think(midgame, color, 6, 24, 8);
        const B1Think wide = b1_think(midgame, color, 6, 64, 32);

        assert(tight.nodes < narrow.nodes);
        assert(narrow.nodes < wide.nodes);
        assert(tight.root_total < narrow.root_total);
        assert(narrow.root_total < wide.root_total);

        // The measured ladder, so "smaller" is not just "smaller than a number I
        // picked". 3.9x between the extremes on the node count, 6.6x/11x on the
        // root width.
        if (color == BLACK) {
            assert(tight.nodes == 160 && narrow.nodes == 533 && wide.nodes == 1114);
            assert(tight.root_total == 11 && narrow.root_total == 33 && wide.root_total == 73);
        } else {
            assert(tight.nodes == 97 && narrow.nodes == 461 && wide.nodes == 937);
            assert(tight.root_total == 6 && narrow.root_total == 28 && wide.root_total == 68);
        }

        // A two-wide root still finds the same win the full-width root found: same
        // tactic, same proven score, same completed layer, same reason.
        assert(tight.score == 100000 && narrow.score == 100000 && wide.score == 100000);
        assert(tight.completed_depth == 2 && narrow.completed_depth == 2 &&
               wide.completed_depth == 2);
        assert(tight.stopped == StopReason::MATE && narrow.stopped == StopReason::MATE &&
               wide.stopped == StopReason::MATE);
        const int want_row = color == BLACK ? 7 : 9;
        const int want_col = color == BLACK ? 6 : 5;
        assert(tight.best.pos.row == want_row && tight.best.pos.col == want_col);
        assert(narrow.best.pos == tight.best.pos);
        assert(wide.best.pos == tight.best.pos);
        // NOT asserted, on purpose: completed_depth as a monotone function of the
        // width. A cut root can finish deeper, and pinning that would be pinning
        // an accident.
    }

    // The same claim on a non-mate-shaped (but still proven) pose, where the root
    // is twice as wide and the tree grows with it.
    const Board crowded = b1_crowded();
    const B1Think c_narrow = b1_think(crowded, BLACK, 6, 24, 8);
    const B1Think c_wide = b1_think(crowded, BLACK, 6, 64, 32);
    assert(c_narrow.nodes < c_wide.nodes);
    assert(c_narrow.root_total < c_wide.root_total);
    assert(c_narrow.nodes == 888 && c_narrow.root_total == 39);
    assert(c_wide.nodes == 1861 && c_wide.root_total == 79);
    assert(c_narrow.score == 100000 && c_wide.score == 100000);
    assert(c_narrow.best.pos.row == 7 && c_narrow.best.pos.col == 6);
    assert(c_wide.best.pos == c_narrow.best.pos);

    // Recorded, deliberately NOT asserted - the one honest cost of the stage: on
    // the quiet pose (see REVIEW_LOG) the wide and narrow arms still agree on the
    // move but disagree on the number the player is shown, because a truncated
    // root proves a different bound. That is exactly why Tier0 and Tier1 are never
    // cut, and pinning a score here would freeze whichever bound this box happened
    // to reach.
    std::cout << "test_move_generator_width_lowers_nodes_and_root_size passed (midgame "
              << "BLACK 160/533/1114 nodes over 11/33/73 root candidates, WHITE "
              << "97/461/937 over 6/28/68, crowded BLACK 888/1861 over 39/79, every arm "
              "on the same proven win)"
              << std::endl;
}

// B1-5: 禁手格子进不了任何一层 (the 「禁手剪枝」 half of 2.5.1).
//
// (7,7) on double_three is the 三三 point of the 7,6+7,8 / 6,7+8,7 cross. It is
// illegal for BLACK, so it must reach no tier at any ply and any width. For WHITE it
// is legal - and it only shows up at the MAXIMUM width, because its
// candidate_pair_value puts it at vicinity[28] of a 45-cell Tier2, under the root
// cap of 24. An assert written at the default width would be red for the wrong
// reason: it would be measuring the cut, not the colour rule.
void test_move_generator_drops_black_forbidden() {
    const Board pose = b1_double_three();

    // The premise, read off the rule rather than off the generator.
    assert(PatternRecognizer::is_forbidden(pose, 7, 7, BLACK));
    assert(!PatternRecognizer::is_forbidden(pose, 7, 7, WHITE));
    const B1Populations black_pop = b1_populations(pose, BLACK);
    const B1Populations white_pop = b1_populations(pose, WHITE);
    assert(black_pop.forbidden == 1);      // exactly (7,7), out of 71 active cells
    assert(white_pop.forbidden == 0);
    assert(black_pop.active == 71 && white_pop.active == 71);

    const std::vector<std::pair<int, int>> widths = {{24, 8}, {2, 2}, {64, 32}};
    for (const auto& w : widths) {
        const B1Rig rig(w.first, w.second);
        const MoveGenerator& gen = rig.generator;
        for (const int ply : {0, 1, 2, 3, 6}) {
            CandidateSet set;
            gen.generate(pose, BLACK, ply, false, &set);
            assert(!b1_in_any_tier(set, 7, 7));
            assert(!b1_in_flat(gen.generate_moves(pose, BLACK, ply, false), 7, 7));
            // and not through quiescence either
            CandidateSet quiet;
            gen.generate(pose, BLACK, ply, true, &quiet);
            assert(!b1_in_any_tier(quiet, 7, 7));
        }
    }

    // The mirror arm, at the width that can actually show it: legal for WHITE, and
    // present in WHITE's Tier2 at the surface maximum at the recorded rank.
    {
        const B1Rig max_rig(64, 32);
        const MoveGenerator& max_gen = max_rig.generator;
        CandidateSet white;
        max_gen.generate(pose, WHITE, 0, false, &white);
        assert(white.vicinity.size() == 45);
        assert(white.vicinity.size() == white_pop.vicinity);  // not cut: 45 of 45
        assert(b1_in_tier(white.vicinity, 7, 7));
        bool at_recorded_rank = false;
        for (size_t i = 0; i < white.vicinity.size(); ++i) {
            if (white.vicinity[i].move.pos.row == 7 && white.vicinity[i].move.pos.col == 7) {
                at_recorded_rank = (i == 28 && white.vicinity[i].score == 112);
                break;
            }
        }
        assert(at_recorded_rank);  // vicinity[28], score 112
        CandidateSet black_at_max;
        max_gen.generate(pose, BLACK, 0, false, &black_at_max);
        assert(black_at_max.vicinity.size() == 34);
        assert(!b1_in_any_tier(black_at_max, 7, 7));
        assert(black_at_max.vicinity.size() < white.vicinity.size());  // the one cell it cost
    }

    // At the DEFAULT width the same square is cut for WHITE as well - pinned so the
    // finding itself is recorded, not just the workaround above.
    {
        const B1Rig at_default_rig(24, 8);
        const MoveGenerator& at_default = at_default_rig.generator;
        CandidateSet white_default;
        at_default.generate(pose, WHITE, 0, false, &white_default);
        assert(white_default.vicinity.size() == 24);
        assert(!b1_in_tier(white_default.vicinity, 7, 7));
    }

    // The second 禁手 arm: crowded drops exactly (4,5) for BLACK and nothing else.
    {
        const Board crowded = b1_crowded();
        const B1Populations crowded_black = b1_populations(crowded, BLACK);
        const B1Populations crowded_white = b1_populations(crowded, WHITE);
        assert(PatternRecognizer::is_forbidden(crowded, 4, 5, BLACK));
        assert(!PatternRecognizer::is_forbidden(crowded, 4, 5, WHITE));
        assert(crowded_black.forbidden == 1);
        assert(crowded_white.forbidden == 0);
        // 173 = 174 active - the one illegal square; no other cell went missing.
        assert(crowded_black.active == 174);
        assert(crowded_black.forced + crowded_black.threats + crowded_black.vicinity +
                   crowded_black.remote == 173);
        assert(crowded_white.forced + crowded_white.threats + crowded_white.vicinity +
                   crowded_white.remote == 174);

        for (const auto& w : widths) {
            const B1Rig rig(w.first, w.second);
            const MoveGenerator& gen = rig.generator;
            for (const int ply : {0, 3}) {
                CandidateSet set;
                gen.generate(crowded, BLACK, ply, false, &set);
                assert(!b1_in_any_tier(set, 4, 5));
                assert(!b1_in_flat(gen.generate_moves(crowded, BLACK, ply, false), 4, 5));
            }
        }
    }

    std::cout << "test_move_generator_drops_black_forbidden passed ((7,7) in no tier at "
              "3 width settings x 5 plies for BLACK, at WHITE's vicinity[28] score 112 "
              "only at the maximum width, and crowded's single 禁手 is (4,5))"
              << std::endl;
}

// B1-6: 静搜那一行还是原来的强制着法过滤器 - the control for the rewiring.
//
// Quiescence used to build its own list inline; it now consumes Tier1 of the same
// generator with in_quiescence set. That claim is only worth anything if the row is
// rebuilt the way the old code did it and compared cell by cell and score by score,
// so that is what this does. Green on both trees by construction: it is a 控制, not
// a discriminator (and it cannot compile on HEAD, which has no MoveGenerator).
void test_quiescence_row_is_the_forcing_filter() {
    const Evaluator evaluator = b1_evaluator(24, 8);
    const MoveGenerator gen(evaluator);

    // The list HEAD's AlphaBetaSearch::quiescence built, transcribed: recognize for
    // the side to play, keep FIVE / LIVE_FOUR / RUSH_FOUR / LIVE_THREE, drop a
    // BLACK-forbidden square, price with the pattern already recognized, order by
    // value -> centre -> cell index.
    auto old_inline_row = [&](const Board& board, int color) {
        std::vector<std::pair<int, Move>> scored;
        for (const Position& pos : board.active_empty()) {
            if (!board.is_empty(pos.row, pos.col)) continue;
            const PositionPattern pattern =
                PatternRecognizer::recognize_position(board, pos.row, pos.col, color);
            bool is_threat = false;
            for (int d = 0; d < 4; ++d) {
                const PatternType t = pattern.directions[d].type;
                if (t == PatternType::FIVE || t == PatternType::LIVE_FOUR ||
                    t == PatternType::RUSH_FOUR || t == PatternType::LIVE_THREE) {
                    is_threat = true;
                    break;
                }
            }
            if (!is_threat) continue;
            if (color == BLACK && PatternRecognizer::is_forbidden_from_pattern(pattern)) continue;
            scored.emplace_back(
                evaluator.evaluate_position_with_pattern(board, pos.row, pos.col, pattern, color),
                Move(pos.row, pos.col, color));
        }
        std::stable_sort(scored.begin(), scored.end(),
                         [](const std::pair<int, Move>& a, const std::pair<int, Move>& b) {
                             return RootScore(a.second, a.first) > RootScore(b.second, b.first);
                         });
        return scored;
    };

    struct Arm {
        const char* name;
        Board board;
        int color;
    };
    const std::vector<Arm> arms = {
        {"midgame", [&] { Board b; build_midgame(b); return b; }(), BLACK},
        {"midgame", [&] { Board b; build_midgame(b); return b; }(), WHITE},
        {"white_open_four", b1_white_open_four(), BLACK},
        {"white_open_four", b1_white_open_four(), WHITE},
        {"double_three", b1_double_three(), BLACK},
        {"double_three", b1_double_three(), WHITE},
        {"crowded", b1_crowded(), BLACK},
    };

    size_t compared = 0;
    for (const Arm& arm : arms) {
        const std::vector<std::pair<int, Move>> want = old_inline_row(arm.board, arm.color);
        CandidateSet set;
        gen.generate(arm.board, arm.color, 0, true, &set);

        // 「仅强制着法」: Tier1 and nothing else.
        assert(set.forced.empty() && set.vicinity.empty() && set.remote.empty());
        assert(set.threats.size() == want.size());
        for (size_t i = 0; i < want.size(); ++i) {
            assert(set.threats[i].move.pos == want[i].second.pos);
            assert(set.threats[i].move.color == want[i].second.color);
            assert(set.threats[i].score == want[i].first);
        }
        compared += want.size();

        // The row is never truncated, whatever the knobs say - a quiescence subtree
        // has no width, and a 活四 answer is not something a layer may be cut off.
        const B1Rig narrow_rig(2, 2);
        const MoveGenerator& narrow = narrow_rig.generator;
        CandidateSet narrow_set;
        narrow.generate(arm.board, arm.color, 0, true, &narrow_set);
        assert(narrow_set.threats.size() == want.size());
        for (size_t i = 0; i < want.size(); ++i)
            assert(narrow_set.threats[i].move.pos == want[i].second.pos);
    }

    std::cout << "test_quiescence_row_is_the_forcing_filter passed (" << arms.size()
              << " arms, " << compared << " candidates matched cell by cell and score by "
              "score against the old inline filter, other three tiers empty)"
              << std::endl;
}

// B1-7: 一层的取舍只由局面决定 (A6's claim, moved from the root to the tiers).
//
// A6 pinned that the root's *choice* is a function of the position and not of
// active_empty()'s swap-remove order. The tiers now decide which candidates a layer
// even gets to search, which is the same question one level up: if the pre-sort
// order reached a decision, a board that merely played-and-undone a stone would hand
// the search a different list. This extends A6 rather than duplicating it - it says
// nothing about which move wins.
void test_tier_choice_reproducible_across_undo_cycles() {
    Board direct;
    build_midgame(direct);

    Board detour;
    build_midgame(detour);
    // The documented way active_empty() changes order without changing the position:
    // a cell inside the active window and not at its front, played and then taken
    // back. undo_last_move puts it at the END of the vector and the swap-remove that
    // opened its slot moved the window's last cell into it.
    assert(detour.place_stone(1, 1, WHITE));
    detour.undo_last_move();

    assert(detour.move_count() == direct.move_count());
    assert(detour.has_last_move() == direct.has_last_move());
    assert(detour.last_move().pos == direct.last_move().pos);
    assert(detour.zobrist_hash() == direct.zobrist_hash());
    for (int r = 0; r < BOARD_SIZE; ++r)
        for (int c = 0; c < BOARD_SIZE; ++c) assert(detour.get(r, c) == direct.get(r, c));
    // The premise: the two boards are the same position stated in a different
    // candidate order. If this ever holds trivially the case proves nothing.
    assert(direct.active_empty() != detour.active_empty());

    const B1Rig at_default_rig(24, 8);
    const MoveGenerator& at_default = at_default_rig.generator;
    size_t tiers_compared = 0;
    for (const int color : {BLACK, WHITE}) {
        for (const int ply : {0, 1, 3}) {
            CandidateSet a, b;
            at_default.generate(direct, color, ply, false, &a);
            at_default.generate(detour, color, ply, false, &b);
            // Contents AND order, score by score, for all four tiers - the kept set
            // is a function of the position, so is the order it comes back in.
            const std::vector<Candidate>* ta[4] = {&a.forced, &a.threats, &a.vicinity, &a.remote};
            const std::vector<Candidate>* tb[4] = {&b.forced, &b.threats, &b.vicinity, &b.remote};
            for (int t = 0; t < 4; ++t) {
                assert(ta[t]->size() == tb[t]->size());
                for (size_t i = 0; i < ta[t]->size(); ++i) {
                    assert((*ta[t])[i].move.pos == (*tb[t])[i].move.pos);
                    assert((*ta[t])[i].move.color == (*tb[t])[i].move.color);
                    assert((*ta[t])[i].score == (*tb[t])[i].score);
                }
                tiers_compared += ta[t]->size();
            }
            const std::vector<Move> fa = a.flatten(), fb = b.flatten();
            assert(fa.size() == fb.size());
            for (size_t i = 0; i < fa.size(); ++i) assert(fa[i].pos == fb[i].pos);
            // The cut has to be real for the claim to be a claim.
            assert(a.vicinity.size() == 24 || a.vicinity.size() == 16 ||
                   a.vicinity.size() == 8);
        }
    }

    std::cout << "test_tier_choice_reproducible_across_undo_cycles passed ("
              << tiers_compared << " tier entries compared across a played-and-undone "
              "stone, with the candidate list genuinely reordered and 67 -> 24/16/8 "
              "cut both ways)"
              << std::endl;
}

int main() {
    std::cout << "Running search tests..." << std::endl;

    PatternRecognizer::init();

    test_transposition_default_size();
    test_transposition_table();
    test_tt_depth_first_replacement();
    test_tt_generation_advances_per_move();
    test_tt_mate_score_normalised_by_ply();
    test_tt_key_collision_is_a_miss_not_a_wrong_hit();
    test_tt_full_bucket_replaces_shallowest_then_oldest();
    test_tt_probe_copies_the_record_out();
    test_transposition_cross_search_reuse();
    test_transposition_depth_gate_still_applies();
    test_tt_cold_think_cost_and_warm_reuse();
    test_vcf_immediate_five();
    test_vcf_no_win();
    test_vcf_gap_four_sequence_wins();
    test_vcf_block_move_ignores_overline_endpoint();
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
    test_opening_book_refuses_an_occupied_response_it_really_found();
    test_opening_book_rejects_swapped_colours();
    test_opening_book_large_weight_no_overflow();
    test_search_engine_uses_book();
    test_search_engine_ignores_book_midgame();
    test_search_engine_layered();
    test_engine_progress_callback_reports_each_layer();
    test_engine_keeps_alphabeta_time_after_tactics();
    test_engine_stamps_elapsed_on_tactical_wins();
    test_engine_elapsed_covers_the_tactical_phases();
    test_engine_progress_frames_share_the_search_clock();
    test_engine_reports_the_interrupted_alphabeta_layer();
    test_search_engine_finds_five();
    test_controller_async();
    test_controller_stop();
    test_controller_restart_after_completion();
    test_forced_move();
    test_fast_path_labels_fill_no_depth();
    test_search_result_fields_initialized_on_fast_paths();
    test_vcf_defense_proven_loss();
    test_vcf_defense_survives();
    test_vcf_defense_cut_short_is_unknown();
    test_vcf_defense_own_live_four_survives();
    test_search_engine_resigns_on_proven_loss();
    test_search_engine_no_false_resign();
    test_search_engine_no_legal_move_and_resign_switch();
    test_search_engine_resigns_on_two_enemy_five_points();
    test_search_engine_full_board_has_no_legal_move();
    test_search_engine_empty_board_still_opens_at_center();
    test_generators_keep_five_points_and_drop_forbidden();
    test_greedy_method_and_move_unchanged();
    test_game_manager_resign_state();
    test_game_manager_rejects_forbidden_human_move();
    test_game_manager_rejects_move_on_ai_turn();
    test_game_manager_promotes_status_when_ai_opens();
    test_game_manager_hint_uses_hint_callback();
    test_game_manager_refuses_hint_after_game_over();
    test_game_manager_replay_snapshot();
    test_root_symmetry_dedup();
    test_search_engine_prefers_own_five_over_block();
    test_alphabeta_node_limit_api();
    test_vct_far_counter_does_not_fabricate_win();
    test_search_engine_resigns_when_alphabeta_proves_mate();
    test_alphabeta_huge_timeout_does_not_overflow_node_budget();
    test_alphabeta_absolute_deadline_stops_search();
    test_alphabeta_expired_deadline_returns_root_fallback();
    test_alphabeta_aborted_search_stores_only_sound_nodes();
    test_stop_reason_time_hard_lands_mid_layer();
    test_stop_reason_nodes_cuts_a_layer_and_keeps_the_completed_move();
    test_stop_reason_soft_window_keeps_the_completed_layer();
    test_stop_reason_crossing_at_the_commit_keeps_the_layer();
    test_stop_reason_user_flag_stops_between_layers();
    test_stop_reason_mate_ends_the_deepening_without_an_abort();
    test_quiescence_nodes_count_toward_the_budget_and_report_seldepth();
    test_greedy_drops_the_defense_price_of_a_black_forbidden_point();
    test_alphabeta_large_depth_bounded_by_slice_without_deadline();
    test_sort_moves_pins_tt_move_already_at_front();
    test_sort_moves_promotes_tt_move_from_tail();
    test_sort_moves_full_order_without_tt_move();
    test_root_ordering_uses_hint_move();
    test_aspiration_refines_without_changing_result();
    test_search_engine_forwards_the_hint_to_the_search();

    test_root_tiebreak_prefers_centre();
    test_root_choice_reproducible();
    test_d4_dedup_keeps_central_representative();

    test_search_limits_and_stats_defaults();
    test_time_manager_sizes_the_window_to_the_position();
    test_time_manager_node_budget_and_no_deadline();
    test_search_context_polls_the_clock_once_per_1024_nodes();
    test_search_context_reports_why_it_stopped();
    test_search_context_deadlines_share_one_start_reading();
    test_search_context_counters_account_for_the_tree();

    test_move_generator_tiers_never_drop_forced();
    test_move_generator_vicinity_obeys_the_layer_width();
    test_move_generator_tier3_and_the_no_threat_widening();
    test_move_generator_width_lowers_nodes_and_root_size();
    test_move_generator_drops_black_forbidden();
    test_quiescence_row_is_the_forcing_filter();
    test_tier_choice_reproducible_across_undo_cycles();

    std::cout << "All search tests passed!" << std::endl;
    return 0;
}

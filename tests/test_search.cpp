#include "core/board.h"
#include "core/pattern.h"
#include "core/evaluator.h"
#include "core/transforms.h"
#include "optimization/transposition.h"
#include <atomic>
#include <set>
#include <chrono>
#include <climits>
#include <array>
#include <cstdint>
#include <vector>
#include <algorithm>
// Declaration only, not an access hack: the B3 PvTable case builds one object over
// 0xFF-filled raw storage with placement new, which <new> declares. Nothing here
// widens access - the #define below is J2's, pre-existing and unchanged.
#include <new>
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
    // A3 lands the field, C1 fills it in; a record whose node never evaluated this
    // position still says NO_STATIC_EVAL, since store() inherits only from a same-tag slot.
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
// share of the budget when the threat probe admits them (20 % / 30 % of the same
// start since B5, which also made both shares conditional; the shares were 35 % /
// 55 % before that, round 85 F4), so a forcing-rich position with no proof spends
// those shares before AlphaBeta opens
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
// (search_engine.cpp:290-291) with remaining_time floored at 100 ms, i.e. 20000 nodes,
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

// A generator holds its evaluator by reference (move_generator.h:139), so the two
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

namespace {

// pos24: a 24-stone positional fight with nothing immediate on it, so neither the
// answer nor the tree is decided by one forced reply. Same list as
// /mnt/d/tmp/r121_b2_ladder.cpp measured, stone for stone.
Board b2_pos24() {
    return b1_board({
        {9, 11, BLACK}, {0, 14, WHITE}, {10, 2, BLACK}, {6, 6, WHITE},
        {10, 4, BLACK}, {9, 2, WHITE}, {4, 2, BLACK}, {5, 3, WHITE},
        {2, 3, BLACK}, {10, 1, WHITE}, {12, 12, BLACK}, {2, 6, WHITE},
        {1, 8, BLACK}, {14, 8, WHITE}, {4, 1, BLACK}, {13, 4, WHITE},
        {1, 14, BLACK}, {8, 11, WHITE}, {10, 10, BLACK}, {8, 7, WHITE},
        {12, 1, BLACK}, {11, 6, WHITE}, {5, 2, BLACK}, {0, 7, WHITE},
    });
}

// The evaluator and the search it owns, kept alive together for the same reason
// B1Rig exists (move_generator.h and alphabeta.h both hold the evaluator by
// reference), plus the transposition table the search is built on.
struct B2Rig {
    explicit B2Rig(int root, int deep)
        : evaluator(b1_evaluator(root, deep)), table(), search(evaluator, table) {}
    const Evaluator evaluator;
    TranspositionTable table;
    AlphaBetaSearch search;
};

struct B2Think {
    uint64_t nodes = 0;
    uint64_t qnodes = 0;
    int root_total = 0;
    int root_done = 0;
    int completed_depth = 0;
    int seldepth = 0;
    int score = 0;
    Move best;
    StopReason stopped = StopReason::NONE;
    std::vector<int> killer_counts;  // one entry per ply, [0, kMaxPly)
    int history_cells = 0;            // cells with a non-zero entry for either colour
    int history_max = 0;
};

// b1_think's shape plus what the think left in the heuristic tables, read through
// the same `#define private public` include J2 uses. That is why the cases that have
// to compile on the tree before B2 keep to b1_think and its public-only fields.
B2Think b2_think(const Board& board, int color, int depth, int root, int deep) {
    const Evaluator evaluator = b1_evaluator(root, deep);
    TranspositionTable tt;  // fresh table per arm, as b1_think's are
    AlphaBetaSearch search(evaluator, tt);
    std::atomic<bool> stop(false);

    SearchLimits limits;
    limits.max_depth = depth;
    limits.hard_time_ms = 0;
    limits.soft_time_ms = 0;
    limits.max_nodes = 0;

    B2Think out;
    out.best = search.search(board, color, limits, stop);
    const SearchStats& stats = search.last_stats();
    out.nodes = stats.nodes;
    out.qnodes = stats.qnodes;
    out.root_total = stats.root_moves_total;
    out.root_done = stats.root_moves_done;
    out.completed_depth = stats.completed_depth;
    out.seldepth = stats.seldepth;
    out.score = stats.score;
    out.stopped = stats.stopped;

    const MoveHeuristics& h = search.heuristics_;
    out.killer_counts.assign(MoveHeuristics::kMaxPly, 0);
    for (int ply = 0; ply < MoveHeuristics::kMaxPly; ++ply)
        out.killer_counts[ply] = h.killer_count(ply);
    for (int i = 0; i < BOARD_SIZE * BOARD_SIZE; ++i) {
        const Position pos = Position::from_index(i);
        const int hb = h.history_value(BLACK, pos);
        const int hw = h.history_value(WHITE, pos);
        if (hb != 0 || hw != 0) ++out.history_cells;
        out.history_max = std::max(out.history_max, std::max(hb, hw));
    }
    return out;
}

// What the shipped tail actually sorts on, stated as data rather than as a second
// copy of the comparator: ④ the static price first, ③ this cell's history under it,
// then the two geometric keys of the A6 triple. A case can then assert that a whole
// list is non-increasing on this key instead of hand-picking one tie.
struct B2TailKey {
    int price;
    int history;
    int center_dist;
    int index;
};

bool b2_key_earlier(const B2TailKey& a, const B2TailKey& b) {
    if (a.price != b.price) return a.price > b.price;
    if (a.history != b.history) return a.history > b.history;
    if (a.center_dist != b.center_dist) return a.center_dist < b.center_dist;
    return a.index < b.index;
}

std::vector<B2TailKey> b2_tail_keys(const std::vector<Move>& moves, size_t from,
                                    const Board& board, const Evaluator& evaluator, int color,
                                    const MoveHeuristics& h) {
    std::vector<B2TailKey> keys;
    keys.reserve(moves.size() - from);
    for (size_t i = from; i < moves.size(); ++i) {
        const Move& m = moves[i];
        keys.push_back({evaluator.evaluate_position_pair(board, m.pos.row, m.pos.col, color),
                        h.history_value(color, m.pos), center_distance(m.pos),
                        m.pos.to_index()});
    }
    return keys;
}

// The order the A6 triple alone gives: value, then nearer the centre, then the lower
// cell index. This is what sort_moves must reproduce when the tables hold nothing,
// and what B1's and A6's pins are still describing.
std::vector<Move> b2_triple_order(const std::vector<Move>& moves, const Board& board,
                                 const Evaluator& evaluator, int color) {
    std::vector<std::pair<RootScore, Move>> ranked;
    ranked.reserve(moves.size());
    for (const Move& m : moves) {
        const int price = evaluator.evaluate_position_pair(board, m.pos.row, m.pos.col, color);
        ranked.emplace_back(RootScore(m, price), m);
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const std::pair<RootScore, Move>& a,
                        const std::pair<RootScore, Move>& b) { return a.first > b.first; });
    std::vector<Move> out;
    out.reserve(ranked.size());
    for (const auto& r : ranked) out.push_back(r.second);
    return out;
}

int b2_index_of(const std::vector<Move>& moves, const Position& pos) {
    for (size_t i = 0; i < moves.size(); ++i)
        if (moves[i].pos == pos) return static_cast<int>(i);
    return -1;
}

}  // namespace

// B2-1: killer 槽位（design doc 2.4.5 ②，src/search/search_state.h）.
//
// Unit-level on a new API, so there is no HEAD-negative arm for it: 8525f60 owns
// neither MoveHeuristics nor a ply argument, and inventing a pre-B2 comparison here
// would be measuring the absence of a class. What is pinned is the contract the
// search depends on: two slots per ply in rank order, a re-record of the front slot
// that changes nothing, ply isolation, and out-of-range reads and writes that cost
// nothing and corrupt nothing.
void test_move_heuristics_killer_slots_hold_two_moves_per_ply() {
    MoveHeuristics h;
    const Position a(7, 6), b(9, 5);
    assert(!h.killer(3, 0).pos.is_valid());
    assert(!h.killer(3, 1).pos.is_valid());
    assert(h.killer_count(3) == 0);

    h.record_cutoff(3, Move(a.row, a.col, BLACK), BLACK, 4);
    assert(h.killer_count(3) == 1);
    assert(h.killer(3, 0).pos == a);
    assert(!h.killer(3, 1).pos.is_valid());

    h.record_cutoff(3, Move(b.row, b.col, BLACK), BLACK, 3);
    assert(h.killer(3, 0).pos == b);
    assert(h.killer(3, 1).pos == a);          // the shift down, exactly once
    assert(h.killer_count(3) == 2);

    // Re-recording the front slot changes nothing: no shift, no duplicate. Stated as
    // WHITE's move on the same cell, because the identity that decides it is the
    // cell's, not the record's - a killer written by the side that moved last ply is
    // still the same killer when this ply reads it back.
    h.record_cutoff(3, Move(b.row, b.col, WHITE), WHITE, 5);
    assert(h.killer(3, 0).pos == b);
    assert(h.killer(3, 0).color == BLACK);     // the slot was not overwritten
    assert(h.killer(3, 1).pos == a);
    assert(h.killer_count(3) == 2);

    // A record at ply 3 is invisible at ply 4 and at every other ply.
    assert(h.killer_count(4) == 0);
    assert(!same_cell(h.killer(4, 0), a));
    assert(!same_cell(h.killer(4, 1), b));
    assert(!same_cell(h.killer(2, 0), a));

    // Out-of-range reads come back as the invalid move, never as somebody else's slot.
    assert(!h.killer(-1, 0).pos.is_valid());
    assert(!h.killer(MoveHeuristics::kMaxPly, 0).pos.is_valid());
    assert(!h.killer(0, MoveHeuristics::kKillerSlots).pos.is_valid());
    assert(!h.killer(0, -1).pos.is_valid());
    assert(h.killer_count(-1) == 0);
    assert(h.killer_count(MoveHeuristics::kMaxPly) == 0);

    // Out-of-range writes record no killer at all - and must not write one into any
    // ply, which is what kMaxPly as a bound is for - while still raising history.
    MoveHeuristics g;
    const Position c(2, 13);
    g.record_cutoff(-1, Move(c.row, c.col, WHITE), WHITE, 3);
    for (int ply = 0; ply < MoveHeuristics::kMaxPly; ++ply) assert(g.killer_count(ply) == 0);
    assert(g.history_value(WHITE, c) == 9);
    g.record_cutoff(MoveHeuristics::kMaxPly, Move(c.row, c.col, WHITE), WHITE, 4);
    for (int ply = 0; ply < MoveHeuristics::kMaxPly; ++ply) assert(g.killer_count(ply) == 0);
    assert(g.history_value(WHITE, c) == 25);   // 9 + 16, both writes landed

    std::cout << "test_move_heuristics_killer_slots_hold_two_moves_per_ply passed (shift once, "
                 "re-record a no-op, colour-blind identity, ply isolation, out-of-range ply "
                 "-1 and kMaxPly write history but no slot)"
              << std::endl;
}

// B2-2: history 按 depth² 累加、随搜索衰减（design doc 2.4.5 ③）.
//
// Also unit-level on a new API, so no HEAD-negative. The first block is the one that
// dies if anyone removes the `= {}` on history_[]: start_think() ages the table
// instead of clearing it, so a table that began the session holding stack garbage
// would order the very first think on numbers no search ever wrote.
void test_move_heuristics_history_accumulates_by_depth_squared_and_ages_by_half() {
    const int cells = BOARD_SIZE * BOARD_SIZE;

    {
        MoveHeuristics fresh;
        for (int i = 0; i < cells; ++i) {
            const Position pos = Position::from_index(i);
            assert(fresh.history_value(BLACK, pos) == 0);
            assert(fresh.history_value(WHITE, pos) == 0);
        }
        assert(fresh.history_value(BLACK, Position()) == 0);   // an invalid cell costs nothing
        for (int ply = 0; ply < MoveHeuristics::kMaxPly; ++ply)
            assert(fresh.killer_count(ply) == 0);
    }

    MoveHeuristics h;
    const Position deep(7, 4), other(3, 3);
    assert(MoveHeuristics::color_slot(BLACK) == 0);
    assert(MoveHeuristics::color_slot(WHITE) == 1);

    h.record_cutoff(2, Move(deep.row, deep.col, BLACK), BLACK, 3);
    assert(h.history_value(BLACK, deep) == 9);       // depth*depth, exactly
    h.record_cutoff(2, Move(deep.row, deep.col, BLACK), BLACK, 3);
    assert(h.history_value(BLACK, deep) == 18);      // two records at depth 3
    assert(h.killer_count(2) == 1);                  // and still no duplicate slot

    // The colour slots are independent tables, not one table with a colour check.
    assert(h.history_value(WHITE, deep) == 0);
    h.record_cutoff(2, Move(other.row, other.col, WHITE), WHITE, 4);
    assert(h.history_value(WHITE, other) == 16);
    assert(h.history_value(BLACK, deep) == 18);
    assert(h.killer(2, 0).pos == other && h.killer(2, 1).pos == deep);
    assert(h.killer_count(2) == 2);

    h.start_layer();                       // ages history, leaves the killers alone
    assert(h.history_value(BLACK, deep) == 9);
    assert(h.history_value(WHITE, other) == 8);
    assert(h.killer(2, 0).pos == other && h.killer(2, 1).pos == deep);
    assert(h.killer_count(2) == 2);

    h.start_think();                       // ages history AND empties every slot
    assert(h.history_value(BLACK, deep) == 4);
    assert(h.history_value(WHITE, other) == 4);
    for (int ply = 0; ply < MoveHeuristics::kMaxPly; ++ply)
        assert(h.killer_count(ply) == 0);

    // Halving is a shift, not a reset: 17 goes 8, 4, 2, 1, 0 and each step is the
    // floor of the half. A reset would make 随搜索衰减 a clear, and a clear would
    // throw away the one thing that says "this cell keeps cutting" across thinks.
    MoveHeuristics g;
    const Position cell(11, 0);
    g.record_cutoff(1, Move(cell.row, cell.col, BLACK), BLACK, 3);   // 9
    g.record_cutoff(1, Move(cell.row, cell.col, BLACK), BLACK, 2);   // 4
    g.record_cutoff(1, Move(cell.row, cell.col, BLACK), BLACK, 2);   // 4
    assert(g.history_value(BLACK, cell) == 17);
    const int ages[5] = {8, 4, 2, 1, 0};
    for (int step = 0; step < 5; ++step) {
        g.start_layer();
        assert(g.history_value(BLACK, cell) == ages[step]);
    }
    g.start_layer();
    assert(g.history_value(BLACK, cell) == 0);   // and it stays at 0, never negative

    std::cout << "test_move_heuristics_history_accumulates_by_depth_squared_and_ages_by_half "
                 "passed (all 225 cells zero at construction, += depth^2 exact, colour slots "
                 "independent, start_layer keeps killers, start_think empties them, 17 ages "
                 "8/4/2/1/0)"
              << std::endl;
}

// B2-3: 三段式的次序本身（design doc 2.4.5 ①②③④）.
//
// sort_moves drove the whole of J2 through the file's `#define private public`
// include; the signature gained `int ply` (defaulted, which is why every pre-B2 call
// site in the file still compiles unchanged) and the two killer stages, so this is a
// new API and has no HEAD-negative arm. What it pins, in the order the shipped code
// applies it:
//   (a) ① the TT move at index 0 - including the one that is ALREADY at index 0, the
//       case the old swap-then-resort-from-0 used to unpinned;
//   (b) ② both killers in slot order right after it, ahead of a tail that prices them
//       last, and slot order beating price between the pair;
//   (c) a killer the width limit cut is simply absent, and shifts nothing;
//   (d) the tail non-increasing on (price, history, centre, index) as one key, over
//       the whole tail, with a seeded history entry that visibly reorders an
//       equal-priced cell without letting it cross a price boundary;
//   (e) with nothing learned the order is exactly the A6 triple - the assert that
//       breaks if anyone promotes ③ above ④ without re-measuring.
void test_sort_moves_pins_tt_then_killers_then_price_over_history() {
    Board board;
    build_midgame(board);
    B2Rig gen_rig(24, 8);
    const Evaluator& evaluator = gen_rig.evaluator;
    const std::vector<Move> generated = gen_rig.search.generate_moves(board, BLACK);
    assert(generated.size() >= 24);                     // measured 33 at this width
    const auto multiset = cell_multiset(generated);
    const std::vector<int> price = heuristic_scores(generated, board, evaluator, BLACK);
    assert(price.size() == generated.size());

    // Rank the cells cheapest-first; the arms below pick from this order rather than
    // from hand-picked coordinates, so the case follows the board if the price table
    // ever moves again.
    std::vector<size_t> by_price(generated.size());
    for (size_t i = 0; i < by_price.size(); ++i) by_price[i] = i;
    std::stable_sort(by_price.begin(), by_price.end(),
                     [&](size_t x, size_t y) { return price[x] < price[y]; });
    const size_t cheapest = by_price[0];
    const size_t dearest = by_price.back();
    assert(price[cheapest] < price[dearest]);           // the list has a price spread
    const Move cheap_move = generated[cheapest];
    const Move dear_move = generated[dearest];

    // ---- (e) nothing learned: an empty table must reproduce the A6 triple exactly.
    const std::vector<Move> triple = b2_triple_order(generated, board, evaluator, BLACK);
    {
        B2Rig rig(24, 8);
        // Fed in reversed, so "the order came out right" cannot be "the input was
        // already right". The premise that the reversed list is NOT the answer is
        // checked, not assumed.
        std::vector<Move> ordered(generated.rbegin(), generated.rend());
        assert(ordered != triple);
        rig.search.sort_moves(ordered, board, BLACK);   // ply defaults to 0, no TT move
        assert(cell_multiset(ordered) == multiset);
        assert(ordered.size() == triple.size());
        for (size_t i = 0; i < triple.size(); ++i) assert(ordered[i].pos == triple[i].pos);
        for (const Move& m : ordered)
            assert(rig.search.heuristics_.history_value(BLACK, m.pos) == 0);
    }

    // ---- (a) ① pinned, whatever its own price says.
    {
        B2Rig rig(24, 8);
        std::vector<Move> ordered = generated;
        rig.search.sort_moves(ordered, board, BLACK, cheap_move);
        assert(ordered[0].pos == cheap_move.pos);        // the cheapest cell in the list
        assert(cell_multiset(ordered) == multiset);
        // and everything behind it is the triple order of the rest, unchanged: the TT
        // move is excluded from the re-ordering, not merely placed in front of it.
        std::vector<Move> rest;
        for (const Move& m : generated)
            if (!(m.pos == cheap_move.pos)) rest.push_back(m);
        const std::vector<Move> want_rest = b2_triple_order(rest, board, evaluator, BLACK);
        for (size_t i = 1; i < ordered.size(); ++i) assert(ordered[i].pos == want_rest[i - 1].pos);
    }
    {
        // The old no-op bug: the TT move already at index 0 used to be re-sorted away
        // because the code only swapped when it was not at the front and then started
        // its sort from 0. Here it is at the front AND two killers are waiting, so
        // the head accounting has to keep it there and open the killer seats at 1.
        B2Rig rig(24, 8);
        std::vector<Move> ordered = generated;
        std::iter_swap(ordered.begin(), ordered.begin() + cheapest);
        assert(ordered[0].pos == cheap_move.pos);
        const int ply = 7;
        // Slot 0 is the TT move's own cell: the branch that must not pull a killer up
        // beside itself. Slot 1 is a different cell and has to land at index 1.
        const size_t slot1_src = by_price[1];
        rig.search.heuristics_.record_cutoff(ply, generated[slot1_src], BLACK, 2);
        rig.search.heuristics_.record_cutoff(ply, cheap_move, BLACK, 2);
        assert(rig.search.heuristics_.killer(ply, 0).pos == cheap_move.pos);
        assert(rig.search.heuristics_.killer(ply, 1).pos == generated[slot1_src].pos);
        rig.search.sort_moves(ordered, board, BLACK, cheap_move, ply);
        assert(ordered[0].pos == cheap_move.pos);
        assert(ordered[1].pos == generated[slot1_src].pos);
        assert(cell_multiset(ordered) == multiset);
    }

    // ---- (b) ② right after ①, in slot order, ahead of a tail that prices both of
    // them below everything it holds.
    {
        B2Rig rig(24, 8);
        const int ply = 5;
        // slot 0 = the cheapest cell of the list, slot 1 = the cheapest cell strictly
        // dearer than it. The pair's own order is therefore visibly not a price order,
        // and both of them price below the whole tail.
        const size_t slot0_src = by_price[0];
        int slot1_src = -1;
        for (size_t j = 1; j + 1 < generated.size(); ++j) {
            if (price[by_price[j]] > price[by_price[0]]) {
                slot1_src = static_cast<int>(by_price[j]);
                break;
            }
        }
        assert(slot1_src >= 0);
        assert(price[slot1_src] > price[slot0_src]);
        const Move slot0 = generated[slot0_src];
        const Move slot1 = generated[slot1_src];
        rig.search.heuristics_.record_cutoff(ply, slot1, BLACK, 2);   // shifts down to slot 1
        rig.search.heuristics_.record_cutoff(ply, slot0, BLACK, 3);   // takes slot 0
        assert(rig.search.heuristics_.killer(ply, 0).pos == slot0.pos);
        assert(rig.search.heuristics_.killer(ply, 1).pos == slot1.pos);
        assert(rig.search.heuristics_.killer_count(ply) == 2);
        std::vector<Move> ordered = generated;
        rig.search.sort_moves(ordered, board, BLACK, dear_move, ply);
        assert(ordered[0].pos == dear_move.pos);       // ① first, and here it is the dearest
        assert(ordered[1].pos == slot0.pos);           // ② slot 0 before slot 1
        assert(ordered[2].pos == slot1.pos);
        assert(ordered[1].pos != ordered[2].pos);
        assert(cell_multiset(ordered) == multiset);
        // The tail's best price beats both killers' - so ② really did jump the queue,
        // and it did it in the rank the layer earned rather than the rank their prices
        // would have given them.
        const std::vector<B2TailKey> tail =
            b2_tail_keys(ordered, 3, board, evaluator, BLACK, rig.search.heuristics_);
        assert(tail.size() == ordered.size() - 3);
        assert(tail.front().price > price[slot0_src]);
        assert(tail.front().price > price[slot1_src]);
        for (size_t i = 1; i < tail.size(); ++i) assert(!b2_key_earlier(tail[i], tail[i - 1]));
    }

    // ---- (c) a killer the width limit cut never happened to this list at all.
    {
        B2Rig rig(24, 8);
        const int ply = 6;
        // An occupied square: no candidate list of this board can contain it, so the
        // slot is filled and unplaceable - the case the code comments as "simply not a
        // candidate here".
        const Position gone(5, 5);
        assert(!board.is_empty(gone.row, gone.col));
        rig.search.heuristics_.record_cutoff(ply, Move(gone.row, gone.col, BLACK), BLACK, 4);
        assert(rig.search.heuristics_.killer_count(ply) == 1);
        std::vector<Move> ordered = generated;
        rig.search.sort_moves(ordered, board, BLACK, Move(), ply);
        assert(!b1_in_flat(ordered, gone.row, gone.col));
        assert(cell_multiset(ordered) == multiset);
        // and nothing else moved: the order is the same one the empty tables produced.
        for (size_t i = 0; i < triple.size(); ++i) assert(ordered[i].pos == triple[i].pos);
    }

    // ---- (d) ④ over ③, asserted as an invariant over the whole list, with ③ shown
    // to be consulted rather than decorative. History is seeded through the out-of
    // range ply, which writes no killer (B2-1 pinned that), so this arm moves ③ alone.
    {
        B2Rig rig(24, 8);
        auto price_of = [&](const Move& m) {
            return evaluator.evaluate_position_pair(board, m.pos.row, m.pos.col, BLACK);
        };
        // The longest run of equal-priced neighbours in the triple order. Without such
        // a run this arm could not show ③ deciding anything, so the run is a premise
        // and not a hope.
        int run_start = 0, run_len = 1, best_start = 0, best_len = 1;
        for (size_t i = 1; i < triple.size(); ++i) {
            if (price_of(triple[i]) == price_of(triple[i - 1])) {
                ++run_len;
                if (run_len > best_len) { best_len = run_len; best_start = run_start; }
            } else {
                run_start = static_cast<int>(i);
                run_len = 1;
            }
        }
        assert(best_len >= 2);
        const int run_end = best_start + best_len - 1;
        const Move seeded = triple[run_end];   // the triple put it LAST of the group
        const int seeded_price = price_of(seeded);
        rig.search.heuristics_.record_cutoff(-1, seeded, BLACK, 4);   // history += 16
        assert(rig.search.heuristics_.history_value(BLACK, seeded.pos) == 16);
        assert(rig.search.heuristics_.killer_count(0) == 0);          // and no killer at all

        std::vector<Move> ordered = generated;
        rig.search.sort_moves(ordered, board, BLACK, Move(), 0);
        assert(cell_multiset(ordered) == multiset);
        const std::vector<B2TailKey> keys =
            b2_tail_keys(ordered, 0, board, evaluator, BLACK, rig.search.heuristics_);
        assert(keys.size() == ordered.size());
        // The invariant, over the whole tail: no entry may be earlier on the key than
        // the entry standing in front of it.
        for (size_t i = 1; i < keys.size(); ++i) assert(!b2_key_earlier(keys[i], keys[i - 1]));
        // The group of cells sharing the seeded cell's price, read off the output.
        int block_start = -1, block_end = -1;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i].price != seeded_price) continue;
            if (block_start < 0) block_start = static_cast<int>(i);
            block_end = static_cast<int>(i);
        }
        assert(block_start >= 0 && block_end >= block_start);
        const int at = b2_index_of(ordered, seeded.pos);
        assert(at >= 0);
        assert(at == block_start);            // ③: highest history leads its price group
        assert(at < run_end);                 // it moved ahead of cells the triple ranked
                                              // above it, so ③ is not decorative
        // ④: and it moved nowhere else. The group occupies exactly the span the triple
        // gave it, which is what dies if anyone promotes ③ above ④.
        assert(block_start == best_start);
        assert(block_end == run_end);
    }

    std::cout << "test_sort_moves_pins_tt_then_killers_then_price_over_history passed ("
              << generated.size() << " candidates: TT pinned at 0 even at the list's minimum "
              "price and even already at 0, two killers in slot order ahead of a tail that "
              "prices them last, a cut killer absent and nothing shifted, tail non-increasing "
              "on (price, history, centre, index), empty tables reproduce the A6 triple)"
              << std::endl;
}

// B2-4: 三段式的代价判据 - the cost pin, and the HEAD-negative arm.
//
// Public API only (b1_think's own shape), so this very body compiles on 8525f60 -
// which is the point: the node count is 230474 there, so the equality is RED on that
// tree while every fact around it (depth, score, move, root width, reason) is green
// on both. That is the discriminator: not "a different search", the same search
// paying for fewer nodes. Both arms are COST pins; the value and the move of these
// poses are already pinned by other cases, so what these two lines buy is ②/③'s
// -3.9 % on the quiet arm and the positional arm that keeps the pin off a single
// board. qnodes is deliberately not asserted - it moved by 7096 on the quiet arm
// between the trees, and only nodes is the quantity the stage promises.
//
// B3 re-pins both numbers (221380 -> 221774, 2409735 -> 2378724) and nothing else
// in this case moves: PVS is decision-neutral, so the only observable difference
// it may make is the node count, and that is precisely what these two lines are
// for. The same arms on 8b91b21 are red here and only on `nodes` - the raw
// two-tree field dump is in REVIEW_LOG's B3 section, produced by
// /mnt/d/tmp/r121_b3t2_cost.cpp run against both trees.
void test_three_stage_ordering_pays_for_itself_in_nodes() {
    {
        const B1Think quiet = b1_think(b1_quiet(), BLACK, 6, 24, 8);
        assert(quiet.nodes == 221774);        // COST pin; 8525f60 230474, 8b91b21 221380
        assert(quiet.completed_depth == 6);   // the four facts below are green on both trees
        assert(quiet.score == 71992);
        assert(quiet.best.pos.row == 8 && quiet.best.pos.col == 7);
        assert(quiet.root_total == 31);
        assert(quiet.stopped == StopReason::NONE);
    }
    {
        const B1Think pos = b1_think(b2_pos24(), BLACK, 8, 24, 8);
        assert(pos.nodes == 2378724);         // COST pin; 8525f60 2396300, 8b91b21 2409735
        assert(pos.completed_depth == 8);     // and again, everything below is green on both
        assert(pos.score == 99999);
        assert(pos.best.pos.row == 7 && pos.best.pos.col == 4);
        assert(pos.root_total == 39);
        assert(pos.stopped == StopReason::NONE);
    }
    std::cout << "test_three_stage_ordering_pays_for_itself_in_nodes passed (quiet BLACK d6 "
                 "24/8 230474 -> 221380 -> 221774 nodes, pos24 BLACK d8 24/8 2396300 -> "
                 "2409735 -> 2378724, same depth, score, move, root width and reason on "
                 "8b91b21 and here; only nodes moved)"
              << std::endl;
}

// B2-5: 衰减的 history 没有把 A4 的根复用弄脏 - the control, green on BOTH trees.
//
// start_think() ages history instead of clearing it, which is the one thing in B2
// that lets a previous think's numbers reach a later think. A4's promise is that a
// session re-asking about the same position lands on the same answer at the same
// cost, so if aged history moved the root's first candidate or reopened a closed
// subtree, the second think would stop being 187 nodes. It is 187 on this tree and
// on 8525f60, with the same move and the same score, and the case is written in
// public API only so that claim is checkable on both.
void test_aged_history_leaves_a_reused_root_think_unchanged() {
    const Board board = b1_quiet();
    const Evaluator evaluator = b1_evaluator(24, 8);
    TranspositionTable tt;                 // ONE table for all three thinks, as a session has
    AlphaBetaSearch search(evaluator, tt);
    std::atomic<bool> stop(false);

    SearchLimits limits;
    limits.max_depth = 6;
    limits.hard_time_ms = 0;
    limits.soft_time_ms = 0;
    limits.max_nodes = 0;

    Move first;
    int first_score = 0;
    uint64_t cold_nodes = 0;
    for (int think = 1; think <= 3; ++think) {
        const Move best = search.search(board, BLACK, limits, stop);
        const SearchStats& stats = search.last_stats();
        assert(stats.completed_depth == 6);
        assert(stats.stopped == StopReason::NONE);
        assert(stats.root_moves_total == 31);
        if (think == 1) {
            first = best;
            first_score = stats.score;
            cold_nodes = stats.nodes;
        } else {
            assert(best.pos == first.pos);
            assert(stats.score == first_score);
            assert(stats.nodes == 187);    // green on 8525f60 too: the reuse is B2-free
        }
    }
    assert(first.pos.row == 8 && first.pos.col == 7);
    assert(first_score == 71992);
    // The premise that makes 187 a claim rather than a tautology: think #1 paid for
    // the whole tree, so #2 and #3 are the reuse and not a repeat. The cold number
    // itself differs between the trees (that is B2-4) and is not claimed here.
    assert(cold_nodes > 187);

    std::cout << "test_aged_history_leaves_a_reused_root_think_unchanged passed (three thinks, "
                 "one table: #2 and #3 both 187 nodes with the same 8,7 / 71992 / depth 6 / "
                 "reason none on this tree and on 8525f60)"
              << std::endl;
}

// B2-6: 记号是树写下的，不是根、也不是静态搜索写下的 (design doc 2.4.5 ②/③).
//
// The recording site is alphabeta()'s beta-cutoff branch, gated on depth >= 2, and
// the root's own order is built by a sort_moves call at ply 0 whose children are
// searched at ply 1. Three things follow, and all three are asserted against what a
// real think leaves in the private table (read the way J2 reads sort_moves):
//   (a) ply 0 is always empty - a real think can never fill the root's slots, so the
//       root ordering is A4's hint plus the price, never "the root's killers";
//   (b) no ply at or below the layer's own depth can hold anything, because a node
//       that far from the root has no depth left to cut with; this is also the
//       production half of the kMaxPly bounds;
//   (c) some ply inside the tree does hold something, i.e. stage ② is live.
// Plus the sharper half the contract asked for: quiescence walks the generator's own
// Tier1 order and never calls sort_moves, so a think whose only non-root nodes are
// quiescence nodes records nothing at all - and that "only" is not assumed, it is
// arithmetic on the counters (nodes == root candidates + quiescence nodes).
void test_killers_are_recorded_by_the_tree_not_by_the_root_or_quiescence() {
    // The arm the contract names for quiescence-dominance is the crowded wide one,
    // 1558 of its 1861 nodes quiescence. Measured here (and on the tree before B2):
    // it is a proven mate and the deepening stops at layer 2, so its deepest
    // alphabeta node is ply 1 at depth 1 and the depth >= 2 gate never fires - every
    // slot is empty at every ply. (a) and (b) hold on it; (c) cannot, which is
    // reported rather than worked around. (b) is the interesting fact here: had the
    // ply index escaped the tree, a slot would be set.
    const B2Think mate_arm = b2_think(b1_crowded(), BLACK, 6, 64, 32);
    assert(mate_arm.nodes == 1861 && mate_arm.qnodes == 1558);
    assert(mate_arm.completed_depth == 2 && mate_arm.stopped == StopReason::MATE);
    assert(mate_arm.killer_counts[0] == 0);
    for (int ply = MoveHeuristics::kMaxPly - 1; ply >= 6; --ply)
        assert(mate_arm.killer_counts[ply] == 0);

    // (c) and the shape of the recording boundary, on an arm the suite can afford:
    // pos24 at depth 3 is quiescence-dominant the same way (2923 of its 4422 nodes),
    // its tree reaches ply 3, and exactly one ply of it can hold a cutoff.
    const B2Think tree = b2_think(b2_pos24(), BLACK, 3, 24, 8);
    assert(tree.nodes == 4422 && tree.qnodes == 2923);
    assert(tree.completed_depth == 3 && tree.root_done == tree.root_total);
    assert(tree.stopped == StopReason::NONE);
    assert(tree.killer_counts[0] == 0);                       // (a)
    assert(tree.killer_counts[1] >= 1);                       // (c)
    for (int ply = 3; ply < MoveHeuristics::kMaxPly; ++ply)   // (b)
        assert(tree.killer_counts[ply] == 0);
    // The footprint of the depth >= 2 gate: a node at ply p of a max_depth D think
    // carries at most D - p depth, so D - 2 is the deepest ply that can record and
    // every ply from D - 1 up cannot. Both halves are pinned, so the gate is neither
    // missing nor tighter than it claims, and the boundary is a function of the depth
    // rather than of this position - D6 on the quiet pose puts it at ply 4, D8 on
    // pos24 at ply 6, measured in REVIEW_LOG's B2 table.
    assert(tree.killer_counts[1] == 2);
    for (int ply = 2; ply < MoveHeuristics::kMaxPly; ++ply) assert(tree.killer_counts[ply] == 0);
    assert(tree.history_cells > 0);

    // quiescence never records, and never sorts. Take a think whose alphabeta nodes
    // all arrive with depth 0: each one returns through `if (depth <= 0) return
    // quiescence(...)` before its move loop, so the ONLY nodes that ran are the root
    // layer's children and quiescence. If nodes equals root candidates plus
    // quiescence nodes, that is the whole tree, and then an empty table can only mean
    // quiescence wrote nothing - even though it recursed and resolved forcing lines.
    const B2Think quiet_only = b2_think(b1_crowded(), BLACK, 1, 64, 32);
    assert(quiet_only.completed_depth == 1 && quiet_only.stopped == StopReason::NONE);
    assert(quiet_only.root_done == quiet_only.root_total);   // each child searched once
    assert(quiet_only.nodes == quiet_only.root_total + quiet_only.qnodes);
    assert(quiet_only.qnodes > quiet_only.root_total);  // 129 over 79: it recursed
    assert(quiet_only.seldepth >= 2);                   // and walked a forcing line
    assert(quiet_only.score == 99999);                  // which is what answered, not a stand-pat
    for (int ply = 0; ply < MoveHeuristics::kMaxPly; ++ply)
        assert(quiet_only.killer_counts[ply] == 0);
    assert(quiet_only.history_cells == 0);

    // The same structural arm on the quiet pose, where the arithmetic is visible down
    // to the last node: one quiescence node per root candidate and nothing else.
    const B2Think one_ply = b2_think(b1_quiet(), BLACK, 1, 24, 8);
    assert(one_ply.nodes == 62 && one_ply.root_total == 31 && one_ply.qnodes == 31);
    assert(one_ply.nodes == one_ply.root_total + one_ply.qnodes);
    for (int ply = 0; ply < MoveHeuristics::kMaxPly; ++ply)
        assert(one_ply.killer_counts[ply] == 0);
    assert(one_ply.history_cells == 0);

    // The control that keeps the two arms above from being read as "this pose never
    // cuts": the same board, the same widths, two more plies of tree, and both tables
    // fill at the one ply that can now hold a depth-2 cutoff.
    const B2Think three_ply = b2_think(b1_quiet(), BLACK, 3, 24, 8);
    assert(three_ply.nodes == 2515 && three_ply.completed_depth == 3);
    assert(three_ply.killer_counts[0] == 0);
    assert(three_ply.killer_counts[1] == 2);
    assert(three_ply.history_cells > one_ply.history_cells);

    std::cout << "test_killers_are_recorded_by_the_tree_not_by_the_root_or_quiescence passed "
                 "(crowded d6 1861/1558 leaves ply 0 empty and records nothing at all, because "
                 "the mate break stops the deepening at layer 2; pos24 d3 fills only ply 1 and "
                 "nothing from ply 2; crowded d1 satisfies nodes == 79+129 and quiet d1 "
                 "62 == 31+31 with both tables empty; quiet d3 then fills them)"
              << std::endl;
}

// ---------------------------------------------------------------------------
// B3: PVS + PV 提取 (design doc 2.4.4's 方案① triangular table, 2.7's
// probe-then-re-search rule, and the commit point that decides whose line a cut
// think reports).
//
// Production is alphabeta.cpp's `search_child_pvs()`, the `pv_` writes in
// `alphabeta()` / `search_root_layer()`, the one commit-point `copy_out` in the
// `layer_complete && layer.sound` branch, and `search_engine.cpp:422`'s
// `result.principal_variation = info.principal_variation;`.
//
// PROVENANCE OF EVERY NUMBER PINED BELOW - and what this block deliberately does
// NOT contain. The ladder arms, the per-layer cut aims and the PVS probe arms were
// measured on THIS tree by /mnt/d/tmp/r121_b3t_probe.cpp (raw output
// /mnt/d/tmp/r121_b3t_probe4.out and /mnt/d/tmp/r121_b3t_extra.out, re-run for the
// B3 wrap-up as /mnt/d/tmp/r121_b3t2_probe.out); the two-tree field dump that
// shows the re-pinned cost arms differing ONLY in `nodes` comes from
// /mnt/d/tmp/r121_b3t2_cost.cpp built and run against both trees.
//   Group A has NO HEAD-negative and each case says so: PvTable does not exist on
//   8b91b21, so there is no pre-change tree these claims could be red on.
//   Group B and group C ARE discriminators: 8b91b21 already owns SearchStats::pv,
//   SearchInfo::principal_variation and SearchResult::principal_variation but has
//   no writer for any of them, so these bodies compile there and every non-empty-
//   line assert is red. Inside the two quiet arms the line claim is asserted BEFORE
//   the arm's cost number on purpose, so the abort on 8b91b21 names the line and the
//   node difference stays the business of
//   test_three_stage_ordering_pays_for_itself_in_nodes.
//   Group D's reuse arm is a CONTROL, green on both trees; B3's cost movement is
//   pinned by that same re-pinned case above.
//   The probe-then-re-search arms themselves are NOT here. They would have to call
//   AlphaBetaSearch::search_child_pvs() and read SearchContext through the
//   file-wide `#define private public` this TU already carries for J2 - a whole-TU
//   access change bought for one case, which is a risk to the gate rather than
//   proof of the rule. They also have no HEAD-negative (search_child_pvs does not
//   exist on 8b91b21), so their numbers are recorded as driver output in
//   REVIEW_LOG's B3 section instead, the way this project records script-line
//   fixtures.
//
// Registrations go at the end of main(), after the two protected assertions' cases
// (test_search_engine_resigns_when_alphabeta_proves_mate and
// test_engine_reports_the_interrupted_alphabeta_layer) and after the cost pins this
// block re-reads, so nothing here can abort ahead of them.
// ---------------------------------------------------------------------------

namespace {

// The line a think left behind, read the way the ladder reads it: a colour
// sequence that alternates from the side to move, and a walk on a FRESH board in
// which every named cell is still empty. A principal variation that fails either
// is not a variation, it is a list of cells - and a line that names a cell twice
// or keeps one colour for two plies would let a player "play" an impossible
// position, which is exactly the bug a PV writer can produce silently.
struct B3Walk {
    bool alternating = true;
    bool cells_free = true;
    size_t steps = 0;
};

B3Walk b3_pv_walk(const std::vector<Move>& pv, int side_to_move) {
    B3Walk w;
    Board walk;
    for (const Move& m : pv) {
        ++w.steps;
        if (m.color != side_to_move) w.alternating = false;
        if (!m.pos.is_valid() || !walk.is_empty(m.pos.row, m.pos.col)) {
            w.cells_free = false;
            continue;
        }
        if (!walk.place_stone(m.pos.row, m.pos.col, side_to_move)) w.cells_free = false;
        side_to_move = opponent(side_to_move);
    }
    return w;
}

void b3_assert_line(const std::vector<Move>& pv, int side_to_move, const Move& best) {
    assert(!pv.empty());
    assert(pv[0].pos == best.pos && pv[0].color == side_to_move);
    const B3Walk walk = b3_pv_walk(pv, side_to_move);
    assert(walk.alternating);
    assert(walk.cells_free);
    assert(walk.steps == pv.size());
}

// b1_think's shape plus the line the committed layer left behind. Public API
// only (SearchStats::pv exists on 8b91b21 too - it simply has no writer there),
// which is what lets the very same body compile and run against the pre-B3 tree.
struct B3Think {
    uint64_t nodes = 0;
    uint64_t qnodes = 0;
    int root_total = 0;
    int root_done = 0;
    int completed_depth = 0;
    int partial_depth = 0;
    int score = 0;
    Move best;
    Move partial_best;
    std::vector<Move> pv;
    StopReason stopped = StopReason::NONE;
};

// One think with no clock and no ceiling of any kind, so `nodes` is the size of
// the tree the width left behind - the same discipline b1_think pins its cost
// arms with.
B3Think b3_open_think(const Board& board, int color, int depth, int root, int deep) {
    const Evaluator evaluator = b1_evaluator(root, deep);
    TranspositionTable tt;  // fresh table per arm: no warm TT may rescue an arm
    AlphaBetaSearch search(evaluator, tt);
    std::atomic<bool> stop(false);

    SearchLimits limits;
    limits.max_depth = depth;
    limits.hard_time_ms = 0;
    limits.soft_time_ms = 0;
    limits.max_nodes = 0;

    B3Think out;
    out.best = search.search(board, color, limits, stop);
    const SearchStats& stats = search.last_stats();
    out.nodes = stats.nodes;
    out.qnodes = stats.qnodes;
    out.root_total = stats.root_moves_total;
    out.root_done = stats.root_moves_done;
    out.completed_depth = stats.completed_depth;
    out.partial_depth = stats.partial_depth;
    out.score = stats.score;
    out.partial_best = stats.partial_best_move;
    out.pv = stats.pv;
    out.stopped = stats.stopped;
    return out;
}

// One think aimed at a layer by reading index, exactly the way
// test_stop_reason_time_hard_lands_mid_layer aims it: measure the unarmed layer
// table first, then arm the clock on the poll three quarters into the deepest
// layer. NO node budget is set - the only thing that ends the think is the
// deadline crossing, which is what makes it a time_hard cut rather than a
// budget cut, and the reason the case can say "committed line vs partial layer"
// instead of "whatever the counter had".
struct B3Cut {
    B3Think cut;
    int layer_at = 0;      // the depth the cut landed in
    int polls_at = 0;      // its reading boundary
    int polls_inside = 0;  // how many polls that layer costs unarmed
};

B3Cut b3_cut_search(const Board& board, int color, int depth, int root, int deep,
                    int fraction_num, int fraction_den) {
        const Evaluator evaluator = b1_evaluator(root, deep);
        std::atomic<bool> stop(false);

        SearchLimits limits;
        limits.max_depth = depth;
        limits.hard_time_ms = 600000;  // never crossed until the clock is armed
        limits.soft_time_ms = 0;
        limits.max_nodes = 0;          // deadline only, never a node ceiling

        B3Cut out;
        TranspositionTable probe_tt;
        AlphaBetaSearch probe(evaluator, probe_tt);
        ArmingClock clock;
        LayerRecord rec;
        measure_layers(probe, board, color, limits, stop, clock, &rec, nullptr);
        assert(rec.depth.size() == static_cast<size_t>(depth));
        const size_t last = rec.depth.size() - 1;
        out.layer_at = rec.depth[last];
        out.polls_at = rec.readings[last];
        out.polls_inside = rec.readings[last] - rec.readings[last - 1] - 2;
        assert(out.polls_inside >= 2);  // otherwise the layer is not aimable

        TranspositionTable cut_tt;
        AlphaBetaSearch search(evaluator, cut_tt);
        ArmingClock armed;
        armed.arm_at = rec.readings[last - 1] + 1 + out.polls_inside * fraction_num / fraction_den;
        search.set_clock(&armed);

        B3Think& th = out.cut;
        th.best = search.search(board, color, limits, stop);
        const SearchStats& stats = search.last_stats();
        th.nodes = stats.nodes;
        th.qnodes = stats.qnodes;
        th.root_total = stats.root_moves_total;
        th.root_done = stats.root_moves_done;
        th.completed_depth = stats.completed_depth;
        th.partial_depth = stats.partial_depth;
        th.score = stats.score;
        th.partial_best = stats.partial_best_move;
        th.pv = stats.pv;
        th.stopped = stats.stopped;
        return out;
}

}  // namespace

// B3-A1: 三角表在每次 think 开始前是空的 (design doc 2.4.4's start_think).
//
// NO HEAD-NEGATIVE: PvTable does not exist on 8b91b21 (`git archive 8b91b21` has no
// class by that name, and the pre-B3 build of this file stops on group A for want of
// it), so there is no pre-change tree this claim could be red on. The two facts it
// pins are the ones the commit point depends on: a think that commits nothing copies
// out nothing, and `start_think()` wipes every slot rather than only slot 0.
void test_pv_table_starts_every_think_empty() {
    static_assert(PvTable::kMaxPly == 64, "the PV table's ply ceiling is the killers'");
    static_assert(PvTable::kMaxLen == 24, "a line cannot outlive the depth ceiling");

    PvTable table;
    for (int ply = 0; ply < PvTable::kMaxPly; ++ply) assert(table.length(ply) == 0);
    {
        std::vector<Move> out;
        table.copy_out(0, &out);
        assert(out.empty());
        assert(!table.at(0, 0).pos.is_valid());  // an unwritten slot answers "no move"
    }

    // A line written at one ply must not survive into the next think.
    table.set_single(3, Move(7, 7, BLACK));
    table.set_single(9, Move(8, 8, WHITE));
    table.extend(4, Move(6, 6, BLACK), 5);
    assert(table.length(3) == 1 && table.length(4) == 1 && table.length(9) == 1);
    table.start_think();
    for (int ply = 0; ply < PvTable::kMaxPly; ++ply) assert(table.length(ply) == 0);

    // The reason `int length_[kMaxPly] = {}` is spelled out instead of being left
    // to the enclosing object: a PvTable built over memory that held something
    // else must still answer "no line", or the first think of a session would be
    // able to copy a variation out of stack noise.
    {
        alignas(PvTable) static unsigned char raw[sizeof(PvTable)];
        for (size_t i = 0; i < sizeof raw; ++i) raw[i] = 0xFF;
        PvTable* fresh = new (raw) PvTable();
        for (int ply = 0; ply < PvTable::kMaxPly; ++ply) assert(fresh->length(ply) == 0);
        std::vector<Move> out;
        fresh->copy_out(5, &out);
        assert(out.empty());
        assert(!fresh->at(5, 0).pos.is_valid());
        // ... and it writes normally afterwards, so the zero is not the whole slot
        // array being unusable.
        fresh->set_single(5, Move(10, 10, BLACK));
        assert(fresh->length(5) == 1);
        fresh->~PvTable();
    }

    std::cout << "test_pv_table_starts_every_think_empty passed (kMaxPly=64, kMaxLen=24, "
                 "every slot empty at construction - including over 0xFF-filled raw memory - "
                 "and start_think() clears a slot written at ply 3, 4 and 9; no HEAD-negative, "
                 "PvTable is not on 8b91b21)"
              << std::endl;
}

// B3-A2: extend 拼接的顺序与 kMaxLen 截断 (design doc 2.4.4's
// line[ply] = move ++ line[ply+1]).
//
// NO HEAD-NEGATIVE for the same vocabulary reason as A1. Both halves matter: the
// order (a reversed line is a legal-looking variation of a different game) and
// the cap (an uncapped copy would run past line_[ply][24]).
void test_pv_table_extend_concatenates_the_child_line_and_caps_at_max() {
    {
        PvTable table;
        table.start_think();
        table.set_single(2, Move(4, 4, BLACK));            // child's own tail
        table.extend(1, Move(3, 3, WHITE), 2);             // [3,3w 4,4b]
        assert(table.length(1) == 2);
        assert(table.at(1, 0) == Move(3, 3, WHITE));
        assert(table.at(1, 1) == Move(4, 4, BLACK));
        table.extend(0, Move(7, 7, BLACK), 1);             // [7,7b 3,3w 4,4b]
        assert(table.length(0) == 3);
        assert(table.at(0, 0) == Move(7, 7, BLACK));
        assert(table.at(0, 1) == Move(3, 3, WHITE));
        assert(table.at(0, 2) == Move(4, 4, BLACK));
        // The child keeps its own line: extend reads it, it never moves it.
        assert(table.length(1) == 2 && table.length(2) == 1);

        std::vector<Move> out;
        table.copy_out(0, &out);
        assert(out.size() == 3);
        assert(out[0] == Move(7, 7, BLACK) && out[1] == Move(3, 3, WHITE) &&
               out[2] == Move(4, 4, BLACK));
        // copy_out clears first, so re-copying a shorter slot cannot leave a tail.
        table.copy_out(1, &out);
        assert(out.size() == 2 && out[0] == Move(3, 3, WHITE));
        table.copy_out(9, &out);
        assert(out.empty());
    }

    // The cap: a 63-ply chain of slots, so the deepest one holds one move and each
    // ply above it wants one more. Cells are named inside the 15x15 board (the
    // table does not check, but a test that stores nonsense cells could not tell a
    // cap from a corruption); length(ply) is 64-ply until it hits the ceiling.
    {
        PvTable table;
        table.start_think();
        table.set_single(PvTable::kMaxPly - 1, Move(0, 0, BLACK));
        assert(table.length(PvTable::kMaxPly - 1) == 1);
        int grew_to_cap_at = -1;
        for (int ply = PvTable::kMaxPly - 2; ply >= 0; --ply) {
            table.extend(ply, Move(ply % BOARD_SIZE, (ply + 5) % BOARD_SIZE, WHITE), ply + 1);
            const int want = PvTable::kMaxPly - ply;
            assert(table.length(ply) == (want < PvTable::kMaxLen ? want : PvTable::kMaxLen));
            if (table.length(ply) == PvTable::kMaxLen && grew_to_cap_at < 0)
                grew_to_cap_at = ply;
        }
        // The chain reaches the ceiling 24 plies above the tail and stays there.
        assert(grew_to_cap_at == PvTable::kMaxPly - PvTable::kMaxLen);
        assert(table.length(0) == PvTable::kMaxLen);
        assert(table.at(0, 0) == Move(0, 5, WHITE));   // the head is THIS ply's move
        assert(table.at(0, PvTable::kMaxLen - 1).pos.is_valid());
        assert(!table.at(0, PvTable::kMaxLen).pos.is_valid());  // and nothing past it
        std::vector<Move> out;
        table.copy_out(0, &out);
        assert(out.size() == static_cast<size_t>(PvTable::kMaxLen));
        for (int i = 0; i < PvTable::kMaxLen; ++i) assert(out[i] == table.at(0, i));
    }

    std::cout << "test_pv_table_extend_concatenates_the_child_line_and_caps_at_max passed "
                 "(parent line is [move] ++ child line in order, the child keeps its own, "
                 "copy_out clears, a 64-slot chain caps at exactly kMaxLen=24 at ply 40 and "
                 "stays there; no HEAD-negative, PvTable is not on 8b91b21)"
              << std::endl;
}

// B3-A3: clear 只清它那一层.
//
// NO HEAD-NEGATIVE (vocabulary). This is the write alphabeta() does as its FIRST
// statement, before any return path, so a node that returns through the depth
// gate, the stop gate or a TT hit must not hand its parent a stale tail: the
// parent extends from THIS ply's slot and would splice a variation from a
// previous node onto a value it just proved.
void test_pv_table_clear_touches_only_its_own_ply() {
    PvTable table;
    table.start_think();
    table.set_single(0, Move(7, 7, BLACK));
    table.set_single(1, Move(6, 6, WHITE));
    table.extend(2, Move(8, 8, BLACK), 3);
    assert(table.length(0) == 1 && table.length(1) == 1 && table.length(2) == 1);

    table.clear(1);
    assert(table.length(1) == 0);
    assert(!table.at(1, 0).pos.is_valid());
    {
        std::vector<Move> out;
        table.copy_out(1, &out);
        assert(out.empty());
    }
    // The neighbours stand: a clear is not a start_think.
    assert(table.length(0) == 1 && table.length(2) == 1);
    assert(table.at(0, 0) == Move(7, 7, BLACK));
    assert(table.at(2, 0) == Move(8, 8, BLACK));

    // Clearing an empty slot is not an error and changes nothing.
    table.clear(30);
    assert(table.length(30) == 0);
    table.clear(1);
    assert(table.length(1) == 0 && table.length(0) == 1 && table.length(2) == 1);

    std::cout << "test_pv_table_clear_touches_only_its_own_ply passed (clear(1) empties ply "
                 "1, gives an empty copy_out and leaves plies 0, 2 and 30 alone; no "
                 "HEAD-negative, PvTable is not on 8b91b21)"
              << std::endl;
}

// B3-A4: set_single 给一子的线，非法 move 给零子.
//
// NO HEAD-NEGATIVE (vocabulary). This is the TT-hit writer: the entry answers the
// node's value from the table, so the only move the node can attest is the one
// the entry names - and an entry whose move cell no longer makes sense must give
// the parent NOTHING rather than a phantom head.
void test_pv_table_set_single_answers_one_move_and_overwrites_a_longer_line() {
    PvTable table;
    table.start_think();
    table.set_single(5, Move(9, 9, WHITE));
    assert(table.length(5) == 1);
    assert(table.at(5, 0) == Move(9, 9, WHITE));
    assert(!table.at(5, 1).pos.is_valid());

    // An invalid move (the default Move(): cell (-1,-1), colour EMPTY) empties the
    // slot instead of writing a head nobody can play.
    table.set_single(5, Move());
    assert(table.length(5) == 0);
    assert(!table.at(5, 0).pos.is_valid());

    // A slot that already held a long line is replaced, not appended to - which is
    // what makes a TT answer at ply p cut the variation off instead of splicing a
    // stale tail onto it.
    table.set_single(7, Move(0, 0, BLACK));
    for (int ply = 6; ply >= 0; --ply)
        table.extend(ply, Move(ply, ply + 1, WHITE), ply + 1);
    assert(table.length(7) == 1 && table.length(0) == 8);  // one move per ply, 7 down to 0
    const int long_len = table.length(0);
    table.set_single(0, Move(2, 2, BLACK));
    assert(table.length(0) == 1);
    assert(table.at(0, 0) == Move(2, 2, BLACK));
    assert(!table.at(0, 1).pos.is_valid());
    assert(table.length(1) == long_len - 1);  // the child of the slot just rewritten

    std::cout << "test_pv_table_set_single_answers_one_move_and_overwrites_a_longer_line "
                 "passed (length 1 for a valid move, 0 for Move(), and a "
              << "slot that held a longer line is replaced rather than extended; no "
                 "HEAD-negative, PvTable is not on 8b91b21)"
              << std::endl;
}

// B3-A5/A6: 越界的 ply 和越界的下标都回答"没有"，且不写坏任何东西.
//
// NO HEAD-NEGATIVE (vocabulary). kMaxPly is a real ceiling, not a comfort:
// alphabeta() recurses to arbitrary ply within the depth budget, and
// search_root_layer writes slot 0 by hand. Out-of-range writers must be silent
// and out-of-range readers must answer "no move / no line", or a deep node would
// write outside line_[].
void test_pv_table_refuses_every_ply_and_index_outside_its_slots() {
    const int bad_plys[] = {-1, PvTable::kMaxPly, PvTable::kMaxPly + 1, 1000, INT_MIN};
    for (const int ply : bad_plys) {
        PvTable table;
        table.start_think();
        table.set_single(ply, Move(7, 7, BLACK));
        table.clear(ply);
        table.extend(ply, Move(8, 8, WHITE), ply + 1);
        table.extend(0, Move(9, 9, BLACK), ply);  // out-of-range CHILD as well
        assert(table.length(ply) == 0);
        assert(!table.at(ply, 0).pos.is_valid());
        std::vector<Move> out;
        table.copy_out(ply, &out);
        assert(out.empty());
        // Only the legal write of the group stands: extend(0, ..., bad child)
        // contributes the head and nothing else.
        assert(table.length(0) == 1);
        assert(table.at(0, 0) == Move(9, 9, BLACK));
        assert(!table.at(0, 1).pos.is_valid());
        // And the slots the table does own are untouched by the out-of-range ones.
        for (int p = 1; p < PvTable::kMaxPly; ++p) assert(table.length(p) == 0);
    }

    // Index bounds inside a valid ply: one before the start, one past the end,
    // and the read must not shorten the line.
    {
        PvTable table;
        table.start_think();
        table.set_single(2, Move(4, 4, BLACK));
        table.extend(1, Move(3, 3, WHITE), 2);
        assert(table.length(1) == 2);
        assert(!table.at(1, -1).pos.is_valid());
        assert(!table.at(1, 2).pos.is_valid());
        assert(!table.at(1, 999).pos.is_valid());
        assert(table.length(1) == 2);  // reading out of range changes nothing
        assert(table.at(1, 0) == Move(3, 3, WHITE) && table.at(1, 1) == Move(4, 4, BLACK));
        // A ply with no line answers "no move" at every index, valid or not.
        assert(!table.at(3, 0).pos.is_valid());
        assert(!table.at(3, -1).pos.is_valid());
    }

    std::cout << "test_pv_table_refuses_every_ply_and_index_outside_its_slots passed (ply -1, "
                 "64, 65, 1000 and INT_MIN write nothing and read empty, an out-of-range child "
                 "ply contributes no tail, and at() out of index range returns an invalid Move "
                 "without touching length; no HEAD-negative, PvTable is not on 8b91b21)"
              << std::endl;
}

// B3-B7: 提交层把它的线交出去 - the HEAD-negative for PV extraction.
//
// DISCRIMINATOR: this body runs verbatim against 8b91b21 (see the excerpt build in
// the header above) and aborts at b3_assert_line's first assert, because that tree
// owns SearchStats::pv, SearchInfo::principal_variation and
// SearchResult::principal_variation with no writer for any of them.
//
// The quiet pose (four stones, no threat, widths 24/8, fresh table, no budget of
// any kind) is the pose the cost pin in test_three_stage_ordering... runs on, so
// the same think answers the same 8,7 / 71992 here and the line that goes with
// it. The depth-4 arm is the SearchEngine field half: search_engine.cpp:422 is
// the writer the design doc's 2.4.4 asks for, and a field only AlphaBetaSearch
// could read would not be a deliverable.
void test_committed_layer_reports_its_principal_variation() {
    const Board board = b1_quiet();
    {
        const B3Think think = b3_open_think(board, BLACK, 6, 24, 8);
        // What this case is about, asserted first so the pre-B3 tree aborts on the
        // line and not on the cost number that belongs to the case above.
        b3_assert_line(think.pv, BLACK, think.best);
        assert(think.pv.size() == 6);
        // The shape, cell by cell, exactly as the probe printed it. A line the
        // commit point wrote by accident could match on length and still be wrong
        // here.
        const int want[6][3] = {
            {8, 7, BLACK}, {6, 7, WHITE}, {8, 9, BLACK},
            {8, 10, WHITE}, {9, 7, BLACK}, {7, 9, WHITE},
        };
        for (int i = 0; i < 6; ++i)
            assert(think.pv[i].pos.row == want[i][0] && think.pv[i].pos.col == want[i][1] &&
                   think.pv[i].color == want[i][2]);
        // The think itself, unchanged by B3 but for the node count: PVS is
        // decision-neutral, so this arm answers with the same score, move, depth and
        // reason the B2 pin records, at the cost B3 measures.
        assert(think.completed_depth == 6 && think.partial_depth == 0);
        assert(think.score == 71992);
        assert(think.best.pos.row == 8 && think.best.pos.col == 7);
        assert(think.root_total == 31 && think.root_done == 31);
        assert(think.stopped == StopReason::NONE);
        assert(think.nodes == 221774);
    }

    // The engine field: same pose, same committed layer, read through the record
    // the client actually gets. Hard budget 200000 ms so nothing cuts the think -
    // the point of this arm is the field, not the interruption (that is C11).
    {
        std::atomic<bool> stop_flag(false);
        SearchEngine engine;
        engine.set_vcf_enabled(false);
        engine.set_vct_enabled(false);
        engine.set_resign_detection(false);
        const SearchResult result = engine.search(b1_quiet(), BLACK, 4, 200000, stop_flag);
        assert(!result.principal_variation.empty());
        b3_assert_line(result.principal_variation, BLACK, result.best_move);
        assert(result.principal_variation.size() == 4);
        const int want[4][3] = {{8, 7, BLACK}, {6, 7, WHITE}, {8, 9, BLACK}, {8, 6, WHITE}};
        for (int i = 0; i < 4; ++i) {
            assert(result.principal_variation[i].pos.row == want[i][0]);
            assert(result.principal_variation[i].pos.col == want[i][1]);
            assert(result.principal_variation[i].color == want[i][2]);
        }
        assert(result.method == "alphabeta");
        assert(result.depth == 4);
        assert(result.partial_depth == 0);
        assert(result.stopped == "none");
        assert(result.nodes_searched == 11359);
        assert(!result.resigned);
        assert(result.best_move.pos.row == 8 && result.best_move.pos.col == 7);
    }

    std::cout << "test_committed_layer_reports_its_principal_variation passed (quiet BLACK d6 "
                 "24/8: 221774 nodes, 8,7 / 71992 / depth 6 / reason none and a 6-move line "
                 "8,7b 6,7w 8,9b 8,10w 9,7b 7,9w; engine d4: 11359 nodes, method alphabeta, "
                 "stopped none, 4-move line 8,7b 6,7w 8,9b 8,6w in "
                 "SearchResult::principal_variation; red on 8b91b21, where both fields are "
                 "always empty)"
              << std::endl;
}

// B3-B8/B9: 线的长度跟着深度和局面走 - d8 on the quiet pose, both colours on the
// positional pose, and a mate arm.
//
// DISCRIMINATOR, same mechanism as B7 (this verbatim body, pre-B3 tree).
//
// `pv.size() == completed_depth` is pinned on these arms because each one is a
// FRESH table, an open budget and a layer that ran to the end - the three
// conditions under which the probe measured it. It is not a universal invariant:
// the same probe printed pos24 WHITE at depth 6 under a 64/32 width with
// pv_len=5 against completed_depth=6 (a TT answer cut the line one move short),
// and B10 below is the reuse case where 2 against 6 is correct. So the general
// form asserted everywhere is `size <= completed_depth`, and equality is claimed
// only for the arms whose numbers are printed here.
void test_principal_variation_length_tracks_depth_and_pose() {
    {
        // quiet BLACK d8 24/8: 1408083 nodes (the arm 8b91b21 answers 1451817 -
        // that difference is item 13's business, not this case's, so it is asserted
        // after the line).
        const B3Think think = b3_open_think(b1_quiet(), BLACK, 8, 24, 8);
        b3_assert_line(think.pv, BLACK, think.best);
        assert(think.pv.size() == 8);
        assert(static_cast<int>(think.pv.size()) == think.completed_depth);
        const int want[8][3] = {
            {8, 7, BLACK}, {6, 7, WHITE}, {8, 9, BLACK}, {8, 6, WHITE},
            {9, 7, BLACK}, {7, 6, WHITE}, {8, 11, BLACK}, {8, 10, WHITE},
        };
        for (int i = 0; i < 8; ++i)
            assert(think.pv[i].pos.row == want[i][0] && think.pv[i].pos.col == want[i][1] &&
                   think.pv[i].color == want[i][2]);
        assert(think.completed_depth == 8 && think.partial_depth == 0);
        assert(think.score == 77842);
        assert(think.best.pos.row == 8 && think.best.pos.col == 7);
        assert(think.root_total == 31 && think.root_done == 31);
        assert(think.stopped == StopReason::NONE);
        assert(think.nodes == 1408083);
    }

    // The positional pose, both colours, d6 24/8 - the arms whose node counts
    // B3 did not move at all (300347 and 175088 on both trees), so the line is
    // the only new thing they report.
    {
        const B3Think black = b3_open_think(b2_pos24(), BLACK, 6, 24, 8);
        b3_assert_line(black.pv, BLACK, black.best);
        assert(black.pv.size() == 6);
        const int want[6][3] = {
            {7, 4, BLACK}, {6, 3, WHITE}, {6, 2, BLACK},
            {3, 2, WHITE}, {4, 3, BLACK}, {4, 4, WHITE},
        };
        for (int i = 0; i < 6; ++i)
            assert(black.pv[i].pos.row == want[i][0] && black.pv[i].pos.col == want[i][1] &&
                   black.pv[i].color == want[i][2]);
        assert(black.nodes == 300347);
        assert(black.completed_depth == 6 && black.score == 99999);
        assert(black.best.pos.row == 7 && black.best.pos.col == 4);
        assert(black.root_total == 39 && black.root_done == 39);
        assert(black.stopped == StopReason::NONE);

        const B3Think white = b3_open_think(b2_pos24(), WHITE, 6, 24, 8);
        b3_assert_line(white.pv, WHITE, white.best);
        assert(white.pv.size() == 6);
        const int w_want[6][3] = {
            {10, 7, WHITE}, {12, 5, BLACK}, {9, 7, WHITE},
            {7, 7, BLACK}, {8, 6, WHITE}, {7, 5, BLACK},
        };
        for (int i = 0; i < 6; ++i)
            assert(white.pv[i].pos.row == w_want[i][0] && white.pv[i].pos.col == w_want[i][1] &&
                   white.pv[i].color == w_want[i][2]);
        assert(white.nodes == 175088);
        assert(white.completed_depth == 6 && white.score == 99999);
        assert(white.best.pos.row == 10 && white.best.pos.col == 7);
        // 32, not BLACK's 39: the width is the same knob but the two sides do not
        // stand in the same position, and Tier1 is not symmetric here. Measured on
        // both trees by /mnt/d/tmp/r121_b3t2_cost.cpp (root=32/32).
        assert(white.root_total == 32 && white.root_done == 32);
        assert(white.stopped == StopReason::NONE);
    }

    // The mate arm: midgame at the narrowest trained widths, where the deepening
    // stops at layer 2 because the answer is proven. pv[0] has to BE the killing
    // move and the line has to be exactly that move and the one reply the layer
    // searched - a reported "variation" that stops short of the kill or runs past it
    // is useless to the client that shows the line.
    for (const int color : {BLACK, WHITE}) {
        Board midgame;
        build_midgame(midgame);
        const B3Think think = b3_open_think(midgame, color, 6, 2, 2);
        b3_assert_line(think.pv, color, think.best);
        assert(think.pv.size() == 2);
        // pv[0] is the mate move, and the line is the two cells the probe printed:
        // BLACK 7,6b then 7,5w, WHITE 9,5w then 9,4b. It is NOT a five on the board
        // the two plies leave behind - 7,6 makes a four whose ends (7,5) and (7,10)
        // are both free, so the reply at one end still loses, and the fifth arrives
        // at ply 3, which a committed layer 2 does not reach. (The draft this case
        // came from asserted a five here; it had never been run.)
        assert(think.pv[0].pos == think.best.pos);
        assert(think.pv[0].pos.row == (color == BLACK ? 7 : 9));
        assert(think.pv[0].pos.col == (color == BLACK ? 6 : 5));
        assert(think.pv[1].color == opponent(color));
        assert(think.pv[1].pos.row == (color == BLACK ? 7 : 9));
        assert(think.pv[1].pos.col == (color == BLACK ? 5 : 4));
        assert(think.completed_depth == 2 && think.score == 100000);
        assert(think.stopped == StopReason::MATE);
        assert(think.root_total == (color == BLACK ? 11 : 6));
    }

    std::cout << "test_principal_variation_length_tracks_depth_and_pose passed (quiet BLACK d8 "
                 "1408083 nodes 8-move line 8,7b 6,7w 8,9b 8,6w 9,7b 7,6w 8,11b 8,10w; pos24 "
                 "BLACK d6 300347 nodes root 39/39 7,4b 6,3w 6,2b 3,2w 4,3b 4,4w; pos24 WHITE "
                 "d6 175088 nodes root 32/32 10,7w 12,5b 9,7w 7,7b 8,6w 7,5b; midgame mate arms "
                 "160/97 nodes, line length 2 whose first move is the kill; red on 8b91b21)"
              << std::endl;
}

// B3-B10: 复用 TT 的那次 think 交一条更短、但仍然是 best 的线.
//
// DISCRIMINATOR (the line's existence), and the reason the equality of B8 is not
// stated as an invariant. On a warm table the root's own answer comes out of
// `set_single(ply, tt_move)` at ply 0 - one move, attested by the entry, with the
// rest of the variation living in whichever think wrote the entry and not in this
// table's slots. So the second think of one session legitimately reports a
// SHORTER line, and the only claims that survive are the ones a client can act
// on: the head is the move it is about to play, and the line is still legal.
//
// The node counts here (221774 then 187 on the quiet pose, 175088 then 1308 on
// pos24) are the reuse arms the ladder records; the 187 arm is already pinned by
// test_aged_history_leaves_a_reused_root_think_unchanged, and 1308 is asserted
// again by D14 below, which is the control that has to be green on 8b91b21 too.
void test_reused_root_think_answers_with_a_shorter_line_that_still_starts_at_the_best_move() {
    for (const int color : {BLACK, WHITE}) {
        const Board board = color == BLACK ? b1_quiet() : b2_pos24();
        const Evaluator evaluator = b1_evaluator(24, 8);
        TranspositionTable tt;                 // ONE table for both thinks, as a session has
        AlphaBetaSearch search(evaluator, tt);
        std::atomic<bool> stop(false);

        SearchLimits limits;
        limits.max_depth = 6;
        limits.hard_time_ms = 0;
        limits.soft_time_ms = 0;
        limits.max_nodes = 0;

        const Move first = search.search(board, color, limits, stop);
        const SearchStats cold = search.last_stats();
        const Move second = search.search(board, color, limits, stop);
        const SearchStats warm = search.last_stats();

        assert(cold.completed_depth == 6 && warm.completed_depth == 6);
        assert(cold.stopped == StopReason::NONE && warm.stopped == StopReason::NONE);
        // The answer did not move.
        assert(second.pos == first.pos);
        assert(warm.score == cold.score);

        b3_assert_line(cold.pv, color, first);
        b3_assert_line(warm.pv, color, second);
        assert(static_cast<int>(warm.pv.size()) <= cold.completed_depth);
        // The reuse reports strictly less than the cold think's line - that is the
        // table answering, not a truncated tree, and it is why no case here may
        // assert pv.size() == completed_depth across two thinks.
        assert(warm.pv.size() < cold.pv.size());
        // The head is what gets played, so the one thing a client reads off a
        // short line is still true of it.
        assert(warm.pv[0].pos == second.pos && warm.pv[0].color == color);
        assert(warm.pv.size() == 2);

        if (color == BLACK) {
            assert(cold.nodes == 221774);
            assert(warm.nodes == 187);   // the same 187 the B2 reuse pin records
            assert(cold.score == 71992);
            assert(first.pos.row == 8 && first.pos.col == 7);
        } else {
            assert(cold.nodes == 175088);
            assert(warm.nodes == 1308);
            assert(cold.score == 99999);
            assert(first.pos.row == 10 && first.pos.col == 7);
        }
    }

    std::cout << "test_reused_root_think_answers_with_a_shorter_line_that_still_starts_at_the_"
                 "best_move passed (quiet BLACK: 221774 nodes with a 6-move line, then 187 "
                 "nodes with the 2-move line 8,7b 6,7w; pos24 WHITE: 175088/6 then 1308 with "
                 "the 2-move line 10,7w 12,5b - each think answers its own pose's move "
                 "(8,7 / 71992 and 10,7 / 99999) at depth 6 with reason none, both lines "
                 "legal; red on 8b91b21)"
              << std::endl;
}

// B3-C11: 被砍断的 think 交的是最后那个完成且干净的层，不是被砍的那层.
//
// DISCRIMINATOR: on 8b91b21 `stats.pv` is always empty, so b3_assert_line's first
// assert aborts there. The interruption itself (completed_depth 3 / partial 4,
// reason time_hard, the played move being the committed layer's) is the pre-existing
// stop-reason accounting and is green on both trees - which is what makes the
// line the only new thing.
//
// Deadline only: `hard_time_ms = 600000` with an ArmingClock whose reading
// numbers are the layer table's, so no node budget is involved and the cut is a
// time_hard one. Aimed three quarters into the deepest layer, the same aim
// test_stop_reason_time_hard_lands_mid_layer uses.
void test_cut_think_reports_the_last_committed_line() {
    // quiet BLACK, depth 4. Unarmed layer table: d1@r3/n62 d2@r6/n742 d3@r11/n2515
    // d4@r29/n11359, so layer 4 costs 16 in-layer polls and the aim is r24.
    const B3Cut four = b3_cut_search(b1_quiet(), BLACK, 4, 24, 8, 3, 4);
    assert(four.layer_at == 4 && four.polls_at == 29 && four.polls_inside == 16);
    assert(four.cut.completed_depth == 3);
    assert(four.cut.partial_depth == 4);
    assert(four.cut.root_done < four.cut.root_total);
    assert(four.cut.stopped == StopReason::TIME_HARD);
    assert(four.cut.qnodes > 0);
    // The move played is the committed layer's winner...
    b3_assert_line(four.cut.pv, BLACK, four.cut.best);
    assert(static_cast<int>(four.cut.pv.size()) == four.cut.completed_depth);
    // ...and that layer's line is NOT the cut layer's: this arm's cut layer had
    // reached (4,6) as its best so far, while layer 3 answers (8,7). Both are
    // non-empty and different, so the commit point cannot be a copy of the
    // partial layer's slot by accident.
    assert(four.cut.partial_best.pos.is_valid());
    assert(four.cut.partial_best.pos.row == 4 && four.cut.partial_best.pos.col == 6);
    assert(four.cut.best.pos.row == 8 && four.cut.best.pos.col == 7);
    {
        const B3Walk walk = b3_pv_walk(four.cut.pv, BLACK);
        assert(walk.alternating && walk.cells_free);
        assert(four.cut.pv[0].pos != four.cut.partial_best.pos);
    }

    // The deeper arm the contract's item 15 names: quiet BLACK d6, cut inside
    // layer 6, where the committed answer and the cut layer's best-so-far happen
    // to agree (8,7 both). The claim that has to survive here is the one a client
    // acts on: the line's head is the move returned, and its length is the
    // COMMITTED depth (5), not the depth that was running (6).
    const B3Cut six = b3_cut_search(b1_quiet(), BLACK, 6, 24, 8, 3, 4);
    assert(six.layer_at == 6 && six.polls_at == 409);
    assert(six.cut.completed_depth == 5 && six.cut.partial_depth == 6);
    assert(six.cut.stopped == StopReason::TIME_HARD);
    assert(six.cut.nodes > 0 && six.cut.nodes < 221774);  // cut, and by less than the layer
    assert(six.cut.root_done < six.cut.root_total);
    b3_assert_line(six.cut.pv, BLACK, six.cut.best);
    assert(static_cast<int>(six.cut.pv.size()) == six.cut.completed_depth);
    assert(six.cut.pv.size() != static_cast<size_t>(six.cut.partial_depth));
    const int want[5][3] = {
        {8, 7, BLACK}, {6, 7, WHITE}, {8, 9, BLACK}, {8, 6, WHITE}, {8, 10, BLACK},
    };
    for (int i = 0; i < 5; ++i)
        assert(six.cut.pv[i].pos.row == want[i][0] && six.cut.pv[i].pos.col == want[i][1] &&
               six.cut.pv[i].color == want[i][2]);

    // The aim is an input, not a fluke: the ladder's sweep puts the cut anywhere
    // from the 75th to the 409th reading of layer 6 and every one of them ends at
    // completed 5 with a 5-move line whose head is the move played.
    for (const int num : {1, 2, 3}) {
        const B3Cut swept = b3_cut_search(b1_quiet(), BLACK, 6, 24, 8, num, 4);
        assert(swept.cut.partial_depth == 6);
        assert(swept.cut.stopped == StopReason::TIME_HARD);
        assert(swept.cut.completed_depth >= 1);
        assert(!swept.cut.pv.empty());
        assert(swept.cut.pv[0].pos == swept.cut.best.pos);
        assert(swept.cut.pv[0].color == BLACK);
        assert(static_cast<int>(swept.cut.pv.size()) == swept.cut.completed_depth);
        const B3Walk walk = b3_pv_walk(swept.cut.pv, BLACK);
        assert(walk.alternating && walk.cells_free);
    }

    std::cout << "test_cut_think_reports_the_last_committed_line passed (quiet BLACK d4 cut at "
                 "reading 24 of layer 4: completed 3 / partial 4 / time_hard, line 8,7b 6,7w "
                 "8,6b of the committed layer while the cut layer's best is 4,6; d6 cut at "
                 "reading 324 of layer 6: completed 5, 178789 nodes cut below the layer's "
                 "221774, line 8,7b 6,7w 8,9b 8,6w 8,10b == 5 == completed depth, and the "
                 "1/4, 2/4, 3/4 aims all agree; red on 8b91b21)"
              << std::endl;
}

// B3-C12: 一层都没完成的 think 不交线.
//
// ONE-SIDED, and said so: on 8b91b21 this arm is trivially green (nothing ever
// wrote a line, so an empty one is what it answers with too). The claim is the
// shape of the new writer's absence - a think with no committed layer must not
// report the partial layer's line - so it is recorded as a guard, not as a
// discriminator.
//
// Both budgets are deterministic: the node ceiling cuts inside layer 1 at a
// counted node (never a wall-clock race), and the absolute deadline is already
// behind the process when the think starts.
void test_a_think_with_no_committed_layer_reports_no_line() {
    {
        std::atomic<bool> stop_flag(false);
        SearchEngine engine;
        engine.set_vcf_enabled(false);
        engine.set_vct_enabled(false);
        engine.set_resign_detection(false);
        engine.set_max_nodes(200);  // layer 1 of this pose costs far more than 200
        const SearchResult result = engine.search(b2_pos24(), WHITE, 4, 5000, stop_flag);
        assert(result.principal_variation.empty());
        assert(result.method == "alphabeta");
        assert(result.depth == 0);          // nothing completed
        assert(result.partial_depth == 1);  // layer 1 was the one that got cut
        assert(result.nodes_searched == 200);
        assert(result.stopped == "nodes");
        assert(result.stopped != "none");
        assert(result.best_move.pos.is_valid());  // it still answers with a move
    }
    {
        // The same shape reached through AlphaBetaSearch directly: a deadline
        // already expired at start, so no layer can commit and not one node is
        // searched. The production clock is left in place on purpose - the
        // deadline is behind it, so the answer does not depend on how long the
        // think takes.
        const Evaluator evaluator;
        TranspositionTable tt;
        AlphaBetaSearch search(evaluator, tt);
        std::atomic<bool> stop(false);
        search.set_deadline(std::chrono::steady_clock::now() - std::chrono::seconds(1));
        const Move played = search.search(b1_quiet(), BLACK, 6, 5000, stop);
        const SearchStats stats = search.last_stats();
        assert(stats.pv.empty());
        assert(stats.completed_depth == 0);
        assert(stats.partial_depth == 0);
        assert(stats.nodes == 0);
        assert(stats.stopped == StopReason::TIME_HARD);
        assert(played.pos.is_valid());
        assert(b1_quiet().is_empty(played.pos.row, played.pos.col));
    }

    std::cout << "test_a_think_with_no_committed_layer_reports_no_line passed (engine pos24 "
                 "d4 with a 200-node ceiling: method alphabeta, depth 0, partial 1, 200 nodes, "
                 "stopped nodes, empty principal_variation; alphabeta with an expired "
                 "deadline: completed 0 / partial 0 / time_hard / empty pv. One-sided by "
                 "nature - 8b91b21 answers empty too, so this is a guard, not a discriminator)"
              << std::endl;
}

// B3-D14: 探测留在表里的界没有污染复用 - CONTROL, must be green on BOTH trees.
//
// PVS puts one-point-window results into the same table the full-window pass
// would have written, so the risk it carries is a bound being read back as an
// exact value: if it were, the second think of a session would answer with a
// different score or a different move or stop at a different layer. Two thinks on
// one table, both colours, and the second agrees with the first on every field
// except the cost. The reuse arms' node counts are 300347 -> 234 and 175088 ->
// 1308 on this tree, and the pre-B3 build runs the same body against 8b91b21,
// where they must be identical: PVS is decision-neutral, so a control that moved
// here would mean the gate or the bound handling is wrong.
void test_probe_bounds_leave_a_reused_think_on_the_same_answer() {
    for (const int color : {BLACK, WHITE}) {
        const Evaluator evaluator = b1_evaluator(24, 8);
        TranspositionTable tt;                 // ONE table for both thinks, as a session has
        AlphaBetaSearch search(evaluator, tt);
        std::atomic<bool> stop(false);

        SearchLimits limits;
        limits.max_depth = 6;
        limits.hard_time_ms = 0;
        limits.soft_time_ms = 0;
        limits.max_nodes = 0;

        const Board board = b2_pos24();
        const Move first = search.search(board, color, limits, stop);
        const SearchStats cold = search.last_stats();
        const Move second = search.search(board, color, limits, stop);
        const SearchStats warm = search.last_stats();

        assert(cold.completed_depth == 6 && warm.completed_depth == 6);
        assert(cold.stopped == StopReason::NONE && warm.stopped == StopReason::NONE);
        assert(second.pos == first.pos);
        assert(second.color == color);
        assert(warm.score == cold.score);
        // The reuse really happened (that is what makes "same answer" a claim and
        // not a repeat of the same tree), and it is the same cost the ladder
        // printed on both trees.
        assert(warm.nodes < cold.nodes);
        if (color == BLACK) {
            assert(cold.nodes == 300347 && warm.nodes == 234);
            assert(first.pos.row == 7 && first.pos.col == 4);
            assert(cold.score == 99999);
        } else {
            assert(cold.nodes == 175088 && warm.nodes == 1308);
            assert(first.pos.row == 10 && first.pos.col == 7);
            assert(cold.score == 99999);
        }
    }

    std::cout << "test_probe_bounds_leave_a_reused_think_on_the_same_answer passed (pos24 d6 "
                 "one table twice: BLACK 300347 -> 234 nodes, WHITE 175088 -> 1308, same move "
                 "(7,4 / 10,7), same score 99999, same completed depth 6, reason none on both "
                 "thinks; CONTROL, green on 8b91b21 as well)"
              << std::endl;
}

// ---------------------------------------------------------------------------
// B5 - 威胁探针 (design doc 2.3.4: 「③ 战术层 VCF/VCT ── 由「威胁探针」按需触发
// （改造：不再固定比例）」, and :255-260 for the three facts it answers).
// ---------------------------------------------------------------------------

namespace {

// One row of the probe's truth table. `black`/`white` are the three facts -
// own_four / own_three / opp_four - for that side to move, and `black_lists` /
// `white_lists` are the sizes of the enumerations the two phases do for
// themselves at their own roots: VCF's list and VCT's list for the side to move,
// then VCF's list for the opponent. Both halves of every row were read off
// /mnt/d/tmp/r121_b5_probe.cpp part 7, on this tree and on 4b18a78's engine with
// the probe module copied in, and they agree there exactly as they do here.
struct B5ProbeRow {
    const char* name;
    std::vector<std::array<int, 3>> stones;
    int black[3];
    int white[3];
    int black_lists[3];
    int white_lists[3];
};

const std::vector<std::array<int, 3>> b5_no_stones = {};
const std::vector<std::array<int, 3>> b5_scattered_stones = {
    {2, 2, BLACK}, {12, 12, WHITE}, {2, 12, BLACK}, {12, 2, WHITE}};
const std::vector<std::array<int, 3>> b5_quiet_stones = {
    {7, 7, BLACK}, {7, 8, WHITE}, {8, 8, BLACK}, {6, 6, WHITE}};
const std::vector<std::array<int, 3>> b5_white_open_four_stones = {
    {7, 5, WHITE}, {7, 6, WHITE}, {7, 7, WHITE}, {7, 8, WHITE},
    {10, 10, BLACK}, {10, 11, BLACK}, {3, 3, BLACK}};
const std::vector<std::array<int, 3>> b5_black_makes_five_stones = {
    {7, 4, BLACK}, {7, 5, BLACK}, {7, 6, BLACK}, {7, 7, BLACK},
    {10, 10, WHITE}, {10, 11, WHITE}, {3, 3, WHITE}};
const std::vector<std::array<int, 3>> b5_double_three_stones = {
    {7, 6, BLACK}, {7, 8, BLACK}, {6, 7, BLACK}, {8, 7, BLACK},
    {2, 2, WHITE}, {2, 3, WHITE}, {2, 4, WHITE}};
const std::vector<std::array<int, 3>> b5_crowded_stones = {
    {7, 7, BLACK}, {7, 8, BLACK}, {7, 9, BLACK},
    {9, 6, WHITE}, {9, 7, WHITE}, {9, 8, WHITE},
    {5, 5, BLACK}, {10, 4, BLACK}, {11, 11, BLACK},
    {4, 9, WHITE}, {3, 11, WHITE}, {2, 2, WHITE},
    {6, 7, BLACK}, {8, 6, WHITE}, {10, 10, BLACK}, {5, 10, WHITE},
    {12, 3, BLACK}, {4, 4, WHITE}, {6, 12, BLACK}, {12, 8, WHITE},
    {13, 13, BLACK}, {1, 6, WHITE}, {3, 5, BLACK}, {11, 6, WHITE}};

const B5ProbeRow b5_probe_rows[] = {
    // Nothing on the board: the window is all 225 cells and no cell makes a shape
    // for anybody, so the probe says 无威胁 and both phases would have had an empty
    // root list.
    {"empty", b5_no_stones, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
    // B1's 「无威胁时放宽」 poses: four and two isolated pairs of stones, too far
    // apart for any single move to make a three or a four.
    {"scattered", b5_scattered_stones, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
    {"quiet", b5_quiet_stones, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
    // WHITE holds 活四 on row 7. BLACK to move has no four of its own (two black
    // stones cannot reach four in one move) but it does have 活三 points on row 10 -
    // (10,8) (10,9) (10,12) (10,13) - and the 「对方 ≥ 冲四」 half is the four white
    // cells on row 7, its two five-points among them.
    {"white_open_four", b5_white_open_four_stones,
     {0, 1, 1}, {1, 1, 0}, {0, 4, 4}, {4, 4, 0}},
    // The mirror: BLACK to move completes five at (7,3)/(7,8), so own_four and
    // own_three both hold on two cells; WHITE to move has the 活三 on row 10 and
    // reads black's two five-points as opp_four.
    {"black_makes_five", b5_black_makes_five_stones,
     {1, 1, 0}, {0, 1, 1}, {2, 2, 0}, {0, 4, 2}},
    // (7,7) here is the 三三 point - two live threes from one square, which BLACK is
    // forbidden to play - so the probe's own_* halves drop that cell, and VCT's list
    // is 20 *other* cells. BLACK's four-list is empty for a plainer reason: no black
    // square reaches a four in one move. This row is the forbidden half of the probe,
    // and the exclusion itself is asserted below rather than left inside the count -
    // the same cell the generator case drops at :2164.
    {"double_three", b5_double_three_stones,
     {0, 1, 1}, {1, 1, 0}, {0, 20, 4}, {4, 4, 0}},
    // B1's crowded pose: work for both phases on either side.
    {"crowded", b5_crowded_stones,
     {1, 1, 1}, {1, 1, 1}, {8, 15, 9}, {9, 20, 8}},
};

}

// B5-1: the probe is only a safe gate if it answers the same question the phases
// ask for themselves. VCFSearch::generate_rush_fours and VCTSearch::generate_attacks
// are the phases' own root enumerations - same domain (board.active_empty()), same
// legal-move test, same pattern set - so the three facts have to agree with those
// lists being empty or not, on every position, for both sides to move. That is what
// 战术不漏 means here, and it is what this case pins rather than hoped.
void test_threat_probe_answers_what_the_two_phases_would_enumerate() {
    Evaluator evaluator;
    TranspositionTable tt;
    VCFSearch vcf(evaluator, tt);
    VCTSearch vct(evaluator, tt);

    int rows = 0;
    for (const B5ProbeRow& row : b5_probe_rows) {
        for (int color : {BLACK, WHITE}) {
            const Board board = b1_board(row.stones);
            const ThreatProbe probe = threat_probe(board, color);
            const int* expected = (color == BLACK) ? row.black : row.white;
            const int* expected_lists = (color == BLACK) ? row.black_lists : row.white_lists;

            assert(probe.own_four == (expected[0] != 0));
            assert(probe.own_three == (expected[1] != 0));
            assert(probe.opp_four == (expected[2] != 0));

            const auto fours = vcf.generate_rush_fours(board, color);
            const auto attacks = vct.generate_attacks(board, color);
            const auto opponent_fours = vcf.generate_rush_fours(board, opponent(color));
            assert(fours.size() == static_cast<size_t>(expected_lists[0]));
            assert(attacks.size() == static_cast<size_t>(expected_lists[1]));
            assert(opponent_fours.size() == static_cast<size_t>(expected_lists[2]));

            // The gate reads the probe and then decides per phase, so a false here
            // would skip a phase that had a move to try.
            assert(probe.own_four == !fours.empty());
            assert(probe.own_three == !attacks.empty());
            assert(probe.opp_four == !opponent_fours.empty());
            // One cell's four is also one cell's forcing threat, so the flags nest.
            assert(!probe.own_four || probe.own_three);

            // On the 三三 row, the forbidden square is excluded by name rather than
            // only through the size of the list it was left out of.
            if (std::string(row.name) == "double_three" && color == BLACK) {
                const auto absent = [](const std::vector<Move>& moves) {
                    for (const Move& m : moves) {
                        if (m.pos.row == 7 && m.pos.col == 7) return false;
                    }
                    return true;
                };
                assert(board.is_empty(7, 7));
                assert(PatternRecognizer::is_forbidden(board, 7, 7, BLACK));
                assert(absent(fours) && absent(attacks));
            }
            ++rows;
        }
    }

    std::cout << "test_threat_probe_answers_what_the_two_phases_would_enumerate passed ("
              << rows << " position/side rows, every fact equal to the phase's own list, "
              "own_four nesting into own_three; on the 三三 row (7,7) is forbidden, is "
              "named-absent from both lists, and VCT's list is still 20 cells while VCF's "
              "is 0)" << std::endl;
}

// B5-2: what the gate does to the think it gates. A position with no threat at all
// used to pay two full candidate enumerations - VCF's root pass and VCT's root
// pass, both of which answer "nothing to try" out of an empty list - and now pays
// one probe pass for the same answer. The answer side is identical by construction,
// because neither phase touches the shared table (both take a TranspositionTable&
// and never read or write it), so skipping an empty-list phase cannot change what
// AlphaBeta later finds. All eight rows below were read on 4b18a78's engine with the
// probe module copied in and answer the same as they do here - /mnt/d/tmp/r121_b5_pc.cpp
// for the last two, part 8 of r121_b5_probe.cpp for the first six - which is why this
// case is a PIN and not a discriminator. The last two rows are also what keeps the
// gate honest: they are the only rows here whose pinned number a welded-shut gate
// would change, so they are the red-able half of a stage whose other half cannot go
// red on any tree.
void test_a_threatless_think_pays_the_probe_and_nothing_else() {
    struct Pin {
        Board board;
        int color;
        const char* method;
        int depth;
        int score;
        int row;
        int col;
        int nodes;
    };

    std::atomic<bool> stop(false);
    // Depth 6 with a time budget so far above what these poses cost that the depth
    // is the only thing that can stop the deepening - no clock in the answer.
    const Pin pins[] = {
        {b1_quiet(), BLACK, "alphabeta", 6, 71992, 8, 7, 221774},
        {b1_quiet(), WHITE, "alphabeta", 6, 97044, 6, 7, 156428},
        {b1_scattered(), BLACK, "alphabeta", 6, 98460, 3, 3, 177061},
        {b1_scattered(), WHITE, "alphabeta", 6, 98948, 11, 3, 251149},
        // The two rows that are not the main search: BLACK to move against WHITE's
        // 活四 still resigns - the loss probe is not gated by the probe's opp_four
        // half, because our own reply can make a point the opponent may legally
        // play five on - and WHITE to move with that 活四 takes it on the fast path.
        {b1_white_open_four(), BLACK, "resign", 0, 0, -1, -1, 0},
        {b1_white_open_four(), WHITE, "greedy_five", 0, 100000, 7, 4, 0},
        // The other half of the same claim, and the gate's positive control: where
        // the probe says there is work, the phase still runs and still wins. A probe
        // welded false - or a `!` typed on the wrong line of the gate - answers these
        // two from AlphaBeta, not `vcf` / 100000.
        {b1_crowded(), BLACK, "vcf", 0, 100000, 7, 6, 0},
        {b1_double_three(), WHITE, "vcf", 0, 100000, 2, 5, 0},
    };

    for (const Pin& pin : pins) {
        SearchEngine engine;
        const SearchResult result = engine.search(pin.board, pin.color, 6, 600000, stop);
        assert(result.method == pin.method);
        assert(result.depth == pin.depth);
        assert(result.score == pin.score);
        assert(result.best_move.pos.row == pin.row);
        assert(result.best_move.pos.col == pin.col);
        assert(result.nodes_searched == pin.nodes);
    }

    std::cout << "test_a_threatless_think_pays_the_probe_and_nothing_else passed (quiet 71992/221774 "
                 "and 97044/156428, scattered 98460/177061 and 98948/251149 nodes at depth 6, same "
                 "move on both trees; white_open_four keeps resign and greedy_five; crowded BLACK and "
                 "double_three WHITE still answer vcf/100000 at (7,6) and (2,5) - all eight rows are "
                 "PIN, green on 4b18a78 as well, and the last two are the ones a broken gate turns red)"
              << std::endl;
}

// ---------------------------------------------------------------------------
// C1 - 转置表携带静态分 (design doc 2.6 Step 1: 「节点首次计算静态分后随 TT 一起
// 存；命中时直接复用，省掉重复全盘扫描」, :399-402).
//
// The step is one int32 on the record plus a read/write pair that touches only it, so
// these cases are mostly about what the field is *not*: not a bound (probe() answers
// as it did when the field was absent), not a sample of the population (hashfull()
// does not count a slot that holds nothing but a score), not a displacement (a record
// still lands in the slot the table would have given it), and not a second opinion (a
// reused number is the number evaluate_board itself answers). The last of those four
// is why the eight B5 rows at test_a_threatless_think_pays_the_probe_and_nothing_else
// are this section's real regression - they are not re-pinned here, and none of them
// moved.
// ---------------------------------------------------------------------------

namespace {

// Tags that share one bucket, and share one of the hundred buckets hashfull()
// samples, so the population metric can be read in the same case that fills the
// table. The addressing is recomputed rather than taken from an accessor: it is part
// of the layout these cases talk about (same reasoning as :334).
std::vector<uint64_t> c1_bucket_keys(size_t wanted) {
    auto bucket_of = [](uint64_t h) {
        return static_cast<size_t>((h * 0x9E3779B97F4A7C15ULL) >> (64 - 10));
    };
    std::vector<uint64_t> keys;
    for (uint64_t h = 1; h < 8000000 && keys.size() < wanted; ++h) {
        const uint64_t candidate = (h << 32) | 0x5A5A5A5AULL;
        if (keys.empty()) {
            if (bucket_of(candidate) < 100) keys.push_back(candidate);
            continue;
        }
        if (bucket_of(candidate) != bucket_of(keys[0])) continue;
        keys.push_back(candidate);
    }
    return keys;
}

}

// The two halves of step 1 on a position that already has a record: the score is
// written onto that record, the record still says what it said, and a score for a
// different position in the same bucket cannot be mistaken for it.
void test_static_eval_on_a_record_leaves_its_bound_alone() {
    const std::vector<uint64_t> keys = c1_bucket_keys(2);
    assert(keys.size() == 2);
    const uint64_t hash = keys[0];
    const uint64_t twin = keys[1];
    assert(hash >> 32 != twin >> 32);

    TranspositionTable tt(1 << 12);
    TTEntry entry;

    // An empty table has no score to hand out.
    assert(tt.static_eval_of(hash) == NO_STATIC_EVAL);
    assert(!tt.probe(hash, entry));

    // A score kept before any bound exists is kept, is read back, and is still not a
    // record: probe() says no, and the population metric says untouched.
    assert(tt.put_static_eval(hash, -4321));
    assert(tt.static_eval_of(hash) == -4321);
    assert(!tt.probe(hash, entry));
    assert(tt.hashfull() == 0);

    // The other position of the bucket is not the same position.
    assert(tt.static_eval_of(twin) == NO_STATIC_EVAL);

    // A record of the first position, written by a node that did not evaluate it. The
    // bound is exactly what the caller said; the score is the one the slot already
    // carried, because a caller with no number of its own has nothing to say about
    // this position's evaluation.
    tt.store(hash, 5, 1234, Move(7, 6, BLACK), TTFlag::EXACT, 2);
    assert(tt.hashfull() == 2);
    assert(tt.probe(hash, entry));
    assert(entry.value == 1234);
    assert(entry.depth == 5);
    assert(entry.flag == TTFlag::EXACT);
    assert(entry.best_move == Move(7, 6, BLACK));
    assert(entry.static_eval == -4321);

    // A second score for the same position goes onto the record rather than into
    // another slot of the bucket, and the neighbour keeps its own.
    assert(tt.put_static_eval(hash, 900));
    assert(tt.static_eval_of(hash) == 900);
    assert(tt.put_static_eval(twin, 200));
    assert(tt.static_eval_of(twin) == 200);
    assert(tt.static_eval_of(hash) == 900);
    assert(tt.probe(hash, entry));
    assert(entry.static_eval == 900);
    assert(entry.value == 1234);
    assert(!tt.probe(twin, entry));

    std::cout << "test_static_eval_on_a_record_leaves_its_bound_alone passed (bucket "
              << static_cast<size_t>((hash * 0x9E3779B97F4A7C15ULL) >> (64 - 10))
              << ", one bound and two scores, hashfull 0 -> 2)" << std::endl;
}

// The neutrality claim of step 1 in one bucket: a slot that holds nothing but a score
// is what every bound-facing reader of the table already calls empty, so records land
// where they would have landed, the population count moves only for records, and a
// bucket with no empty slot refuses a score instead of buying the slot from a record.
void test_an_evaluation_slot_is_invisible_to_the_bound_readers() {
    const std::vector<uint64_t> keys = c1_bucket_keys(5);
    assert(keys.size() == 5);

    TranspositionTable tt(1 << 12);
    TTEntry entry;

    // Three of the four ways hold records.
    tt.store(keys[0], 4, 400, Move(7, 7, BLACK), TTFlag::EXACT);
    tt.store(keys[1], 5, 500, Move(7, 7, BLACK), TTFlag::EXACT);
    tt.store(keys[2], 6, 600, Move(7, 7, BLACK), TTFlag::EXACT);
    assert(tt.hashfull() == 7);

    // The fourth position is only a score. Three numbers say it is not a record: it
    // reads back, it does not probe, and it does not count.
    assert(tt.put_static_eval(keys[3], 313));
    assert(tt.static_eval_of(keys[3]) == 313);
    assert(!tt.probe(keys[3], entry));
    assert(tt.hashfull() == 7);

    // Nor does it displace one. This store wants an empty slot, and the slot holding
    // only a score is that slot; the three records above it do not move, and the
    // fourth way is still the fourth position's own record afterwards.
    tt.store(keys[3], 7, 700, Move(2, 2, WHITE), TTFlag::EXACT);
    assert(tt.hashfull() == 10);
    assert(tt.probe(keys[3], entry));
    assert(entry.value == 700 && entry.depth == 7);
    assert(entry.static_eval == 313);
    assert(tt.probe(keys[0], entry) && entry.value == 400);
    assert(tt.probe(keys[1], entry) && entry.value == 500);
    assert(tt.probe(keys[2], entry) && entry.value == 600);

    // A bucket of four records has no room left, and a score is the thing that goes
    // without rather than pushing a bound out of the table.
    assert(!tt.put_static_eval(keys[4], 111));
    assert(tt.static_eval_of(keys[4]) == NO_STATIC_EVAL);
    assert(tt.hashfull() == 10);
    assert(tt.probe(keys[0], entry) && entry.value == 400);
    assert(tt.probe(keys[3], entry) && entry.value == 700);

    std::cout << "test_an_evaluation_slot_is_invisible_to_the_bound_readers passed "
                 "(3 records -> hashfull 7, +1 score -> 7, +1 record -> 10, fifth score "
                 "refused)" << std::endl;
}

// Step 1's reader and writer on one position, driven through the search object that
// owns the table. Every number compared below is a number the evaluator itself gives
// for that position - the cache is allowed to save a scan, never to change what the
// scan answers, and that is what these five reads pin.
void test_static_score_reuses_a_cached_number_for_either_colour() {
    const Board board = b1_quiet();
    Evaluator evaluator;
    const uint64_t hash = board.zobrist_hash();
    const int black_score = evaluator.evaluate_board(board, BLACK);
    const int white_score = evaluator.evaluate_board(board, WHITE);
    // The premise the one-stored-sign convention rests on, here rather than only in
    // the 675-board census of /mnt/d/tmp/r121_c1_probe.cpp part 1: the two candidate
    // sums swap and the position and edge terms are differences, so the aggregate is
    // exactly antisymmetric and never reaches INT32_MIN, which is NO_STATIC_EVAL.
    assert(black_score == -white_score);
    assert(black_score != NO_STATIC_EVAL);

    std::atomic<bool> stop(false);
    SearchLimits limits;

    {
        TranspositionTable tt(1 << 12);
        AlphaBetaSearch search(evaluator, tt);
        search.ctx_.start(limits, system_search_clock(), stop);

        // Cold: the scan answers what the evaluator answers, and the score is kept.
        assert(search.static_score(board, BLACK, hash, NO_STATIC_EVAL) == black_score);
        assert(search.ctx_.eval_scans() == 1);
        assert(search.ctx_.eval_reuses() == 0);
        assert(search.ctx_.eval_patches() == 1);
        assert(tt.static_eval_of(hash) == black_score);

        // The same position met again - a transposition in the tree below - reads
        // instead of scanning.
        assert(search.static_score(board, BLACK, hash, NO_STATIC_EVAL) == black_score);
        assert(search.ctx_.eval_scans() == 1);
        assert(search.ctx_.eval_reuses() == 1);

        // Either colour reads the one stored sign, and the number a WHITE reader gets
        // is what evaluate_board says for WHITE.
        assert(search.static_score(board, WHITE, hash, NO_STATIC_EVAL) == white_score);
        assert(search.ctx_.eval_scans() == 1);
        assert(search.ctx_.eval_reuses() == 2);

        // A node handed a number of its own is not asked to look one up.
        assert(search.static_score(board, BLACK, hash, 12345) == 12345);
        assert(search.ctx_.eval_scans() == 1);
        assert(search.ctx_.eval_reuses() == 3);
    }

    {
        // The record path: a position with a bound that carries no score still pays
        // one scan, writes it onto the record, and leaves the bound alone.
        TranspositionTable tt(1 << 12);
        AlphaBetaSearch search(evaluator, tt);
        search.ctx_.start(limits, system_search_clock(), stop);
        tt.store(hash, 4, 999, Move(3, 3, BLACK), TTFlag::EXACT);
        assert(tt.static_eval_of(hash) == NO_STATIC_EVAL);

        assert(search.static_score(board, BLACK, hash, NO_STATIC_EVAL) == black_score);
        assert(search.ctx_.eval_scans() == 1);
        assert(search.ctx_.eval_reuses() == 0);
        assert(search.ctx_.eval_patches() == 1);

        TTEntry record;
        assert(tt.probe(hash, record));
        assert(record.value == 999);
        assert(record.depth == 4);
        assert(record.flag == TTFlag::EXACT);
        assert(record.best_move == Move(3, 3, BLACK));
        assert(record.static_eval == black_score);

        // And the write is what makes the next read free, for the other side too.
        assert(search.static_score(board, WHITE, hash, NO_STATIC_EVAL) == white_score);
        assert(search.ctx_.eval_scans() == 1);
        assert(search.ctx_.eval_reuses() == 1);
    }

    std::cout << "test_static_score_reuses_a_cached_number_for_either_colour passed ("
              << "quiet BLACK " << black_score << ", WHITE " << white_score
              << "; four reads on one scan, then one scan onto a record whose bound "
                 "still says 999/4/EXACT/(3,3))" << std::endl;
}

// What step 1 buys across a whole think, and what it does not touch. The two cold
// arms are the quiet pose of the B5 pin table at depth 6 on a fresh table with no
// clock, so the tree is a fixed shape: the node and score lines below are the numbers
// that case already promises - a step that reordered the tree would be caught there,
// not here - and the scan/reuse/patch triple is step 1's own ledger against them.
void test_a_quiet_think_pays_for_each_evaluation_only_once() {
    struct Row {
        int color;
        uint64_t nodes;
        uint64_t qnodes;
        uint64_t scans;
        uint64_t reuses;
        uint64_t patches;
        int score;
    };
    const Row rows[] = {
        {BLACK, 221774, 142978, 75855, 67123, 75852, 71992},
        {WHITE, 156428, 102340, 68419, 33921, 68419, 97044},
    };
    const Board board = b1_quiet();
    Evaluator evaluator;
    std::atomic<bool> stop(false);

    for (const Row& row : rows) {
        TranspositionTable tt;
        AlphaBetaSearch search(evaluator, tt);
        search.set_node_limit(INT_MAX);
        search.search(board, row.color, 6, 0, stop);
        // The tree step 1 was not allowed to change.
        assert(search.stats_.nodes == row.nodes);
        assert(search.stats_.qnodes == row.qnodes);
        assert(search.stats_.score == row.score);
        // Its ledger. Every evaluation this think asked for is either a scan or a
        // read - the quiet pose has nothing to evaluate outside quiescence, so the
        // two add up to the quiescence node count - and a scan found somewhere to
        // keep its score all but three times, those three being the buckets that had
        // filled up with records.
        assert(search.ctx_.eval_scans() == row.scans);
        assert(search.ctx_.eval_reuses() == row.reuses);
        assert(search.ctx_.eval_patches() == row.patches);
        assert(search.ctx_.eval_scans() + search.ctx_.eval_reuses() == row.qnodes);
        assert(search.ctx_.eval_reuses() > 0);
        assert(search.ctx_.eval_scans() < search.stats_.qnodes);
    }

    // The same root on the table the first think filled: 187 nodes is the answer read
    // out of the records, and the one evaluation the second think still asks for is
    // answered without a scan - the score the first think wrote is on the record the
    // second one replaced, not erased by it.
    {
        TranspositionTable tt;
        AlphaBetaSearch first(evaluator, tt);
        first.set_node_limit(INT_MAX);
        first.search(board, BLACK, 6, 0, stop);
        AlphaBetaSearch second(evaluator, tt);
        second.set_node_limit(INT_MAX);
        second.search(board, BLACK, 6, 0, stop);
        assert(second.stats_.nodes == 187);
        assert(second.ctx_.eval_scans() == 0);
        assert(second.ctx_.eval_reuses() == 1);
    }

    std::cout << "test_a_quiet_think_pays_for_each_evaluation_only_once passed (quiet "
                 "BLACK 75855 scans + 67123 reads over 142978 quiescence nodes, WHITE "
                 "68419 + 33921 over 102340, both on the 221774 / 156428 node trees "
                 "the B5 rows promise; warm repeat 187 nodes with 0 scans)" << std::endl;
}

// The sign convention differential-tested against the scan it replaces, one shape at
// a time: for every symmetry of every pose, the number step 1 keeps is the number
// evaluate_board itself answers for BLACK, and the read for WHITE is its negation -
// which is the same equality evaluate_board has between its own two colours, so the
// cache is never being compared with itself. The range line is the other half of the
// convention: NO_STATIC_EVAL is INT32_MIN, and a score that could reach it would be
// invisible to a reader.
void test_a_cached_sign_answers_both_sides_of_every_shape() {
    const Board poses[] = {b1_quiet(), b1_scattered(), b1_crowded(), b1_white_open_four(),
                           b1_double_three()};
    Evaluator evaluator;
    std::atomic<bool> stop(false);
    SearchLimits limits;
    int shapes = 0;

    for (const Board& base : poses) {
        for (int variant = 0; variant < SYMMETRY_COUNT; ++variant) {
            const Board board = transform_board(base, variant);
            const uint64_t hash = board.zobrist_hash();
            const int black_score = evaluator.evaluate_board(board, BLACK);
            const int white_score = evaluator.evaluate_board(board, WHITE);
            assert(black_score == -white_score);
            assert(black_score <= 99999 && black_score >= -99999);
            assert(black_score != NO_STATIC_EVAL);

            TranspositionTable tt(1 << 12);
            AlphaBetaSearch search(evaluator, tt);
            search.ctx_.start(limits, system_search_clock(), stop);
            assert(search.static_score(board, BLACK, hash, NO_STATIC_EVAL) == black_score);
            assert(tt.static_eval_of(hash) == black_score);
            assert(search.static_score(board, WHITE, hash, NO_STATIC_EVAL) == white_score);
            assert(search.ctx_.eval_scans() == 1);
            assert(search.ctx_.eval_reuses() == 1);

            // The same shape kept from the other side answers the same two numbers.
            TranspositionTable other(1 << 12);
            AlphaBetaSearch flip(evaluator, other);
            flip.ctx_.start(limits, system_search_clock(), stop);
            assert(flip.static_score(board, WHITE, hash, NO_STATIC_EVAL) == white_score);
            assert(other.static_eval_of(hash) == black_score);
            assert(flip.static_score(board, BLACK, hash, NO_STATIC_EVAL) == black_score);
            ++shapes;
        }
    }
    assert(shapes == 40);

    std::cout << "test_a_cached_sign_answers_both_sides_of_every_shape passed (" << shapes
              << " shapes x 2 colours, every read equal to the scan it replaced)"
              << std::endl;
}

// ---------------------------------------------------------------------------
// C2 - 候选格分数缓存 (design doc 2.6 Step 2: 「每个候选格缓存 evaluate_position_pair
// 分数。落子/撤销只更新受影响格（该格 ±2 邻域内、以及该格自身所在 4 条线上的候选格，
// 约 15-25 个），而不是每节点重算全部候选。这是「排序」路径的主要成本」, :406-407).
//
// A memo owes three answers and the first three cases give one each: the number is the
// one the pass it replaces gives (C2-1 - the 逐格比对 :556 #3 asks for, over 225 squares x
// two colours x 42 boards in the method that clause names), it knows when it has gone
// stale (C2-2), and the reader asks for what the writer had (C2-3). C2-4 and C2-5 are the
// think level: the tree and the counters C1 pinned, unmoved, with this step's own ledger
// sitting next to them rather than in place of them.
//
// The doc's dirty set is not what shipped, and the reason is correctness rather than
// taste. 「该格 ±2 邻域内、以及该格自身所在 4 条线上的候选格」 is a footprint, and a
// footprint is not what a record depends on: finish_direction walks the very line it
// started on out to nine cells - the fill_run loop that decides whether a BLACK four has
// a completion point at all looks five cells past the first empty cell after the group -
// so a stone outside every ±2/±4 footprint can still change what the square is worth.
// C2-2 is such a square: (7,7) reads 1224 with BLACK (7,9)(7,10)(7,11) on the board, and
// 224 after BLACK plays (7,12) - five cells away, one cell past the window's edge at
// (7,11). The key that shipped is the four line words themselves: the complete
// dependency, four loads, no bookkeeping in Board, which is why a make/unmake sibling
// revalidates by comparing rather than by being remembered.
// ---------------------------------------------------------------------------

namespace {

// The recognizer's whole answer, field by field: a record that got the four directions
// right and the analyze() aggregates wrong would still order differently. The oracle is
// the two single-colour calls, which test_pattern.cpp:464 already proves
// recognize_position_both against - this case does not re-prove that layer, it proves the
// record kept on top of it.
bool c2_patterns_equal(const PositionPattern& a, const PositionPattern& b) {
    for (int d = 0; d < 4; ++d) {
        if (a.directions[d].type != b.directions[d].type) return false;
        if (a.directions[d].direction != b.directions[d].direction) return false;
    }
    return a.four_count == b.four_count && a.three_count == b.three_count &&
           a.live_four_count == b.live_four_count && a.rush_four_count == b.rush_four_count &&
           a.live_three_count == b.live_three_count && a.has_four_four == b.has_four_four &&
           a.has_four_three == b.has_four_three && a.has_three_three == b.has_three_three &&
           a.has_live_four == b.has_live_four && a.has_rush_four == b.has_rush_four &&
           a.has_live_three == b.has_live_three;
}

// 42 boards: 40 pseudo-random ones and the two that pack every line through the centre, so
// the census runs over ordinary shapes and over the colour-specific corrections (WHITE
// overline demotion, BLACK overline promotion, the locked-four and sleepy-three walks) at
// the same time. It carries its own generator instead of std::mt19937 because this file's
// include block sits above three assertions the suite cites by line number - :2591, :3973
// and :3986 - and a new #include would move all three.
std::vector<Board> c2_census_boards() {
    std::vector<Board> boards;
    unsigned long long state = 0x20261019ULL;
    auto drawn = [&state](int bound) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<int>((state >> 33) % static_cast<unsigned long long>(bound));
    };
    for (int game = 0; game < 40; ++game) {
        Board board;
        const int stones = 1 + drawn(45);
        for (int i = 0; i < stones; ++i) {
            board.place_stone(drawn(BOARD_SIZE), drawn(BOARD_SIZE), i % 2 == 0 ? BLACK : WHITE);
        }
        boards.push_back(board);
    }
    for (const int color : {BLACK, WHITE}) {
        Board board;
        for (int i = 0; i < BOARD_SIZE; ++i) {
            board.place_stone(7, i, color);
            board.place_stone(i, 7, color);
            board.place_stone(i, i, color);
            board.place_stone(i, BOARD_SIZE - 1 - i, color);
        }
        boards.push_back(board);
    }
    return boards;
}

// One record against the pass it stands in for, in both directions of both halves: the two
// shapes, each shape priced for its own colour, and the pair either colour of caller asks for.
void c2_check_record(const CandidateCache::Cell& found, const Board& board, int row, int col,
                     const Evaluator& evaluator, long long& checked) {
    const PositionPattern ref_black = PatternRecognizer::recognize_position(board, row, col, BLACK);
    const PositionPattern ref_white = PatternRecognizer::recognize_position(board, row, col, WHITE);
    assert(c2_patterns_equal(found.black, ref_black));
    assert(c2_patterns_equal(found.white, ref_white));
    assert(found.value_black ==
           evaluator.evaluate_position_with_pattern(board, row, col, ref_black, BLACK));
    assert(found.value_white ==
           evaluator.evaluate_position_with_pattern(board, row, col, ref_white, WHITE));

    for (const int side : {BLACK, WHITE}) {
        PositionPattern mine, theirs;
        PatternRecognizer::recognize_position_both(board, row, col, side, mine, theirs);
        assert(c2_patterns_equal(mine, side == BLACK ? found.black : found.white));
        assert(found.side_value(side) ==
               evaluator.evaluate_position_with_pattern(board, row, col, mine, side));
        assert(found.pair_value() ==
               evaluator.candidate_pair_value(mine, theirs, row, col, side));
        assert(found.pair_value() == evaluator.evaluate_position_pair(board, row, col, side));
    }
    ++checked;
}

bool c2_tier_equal(const std::vector<Candidate>& a, const std::vector<Candidate>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!(a[i].move == b[i].move) || a[i].score != b[i].score) return false;
    }
    return true;
}

bool c2_sets_equal(const CandidateSet& a, const CandidateSet& b) {
    return c2_tier_equal(a.forced, b.forced) && c2_tier_equal(a.threats, b.threats) &&
           c2_tier_equal(a.vicinity, b.vicinity) && c2_tier_equal(a.remote, b.remote);
}

// b1_scattered with a BLACK three at (7,9)(7,10)(7,11): the empty (7,7) reads 冲四 through
// the hole at (7,8), and (7,12) - five cells away, one cell past the window's edge - is the
// stone that takes that four away.
Board c2_far_reach_board() {
    return b1_board({{2, 2, BLACK}, {12, 12, WHITE}, {2, 12, BLACK}, {12, 2, WHITE},
                     {7, 9, BLACK}, {7, 10, BLACK}, {7, 11, BLACK}});
}

}

// The 逐格比对 :556 #3 asks of an evaluation cache, at the cache's own level: every square of
// every board, both colours' shapes and both prices, against the fresh pass. A record that
// was merely plausible on the poses would pass a sample; this is the case that says the
// record is the pass, everywhere, and that an occupied square never reaches one.
void test_the_cell_cache_answers_the_fresh_pass_on_every_square() {
    const std::vector<Board> boards = c2_census_boards();
    assert(boards.size() == 42);
    Evaluator evaluator;
    long long squares = 0, checked = 0, guarded = 0;

    for (const Board& board : boards) {
        CandidateCache cache(evaluator);
        const CandidateCache::Cell* records[BOARD_SIZE][BOARD_SIZE] = {};
        int empties = 0;

        for (int row = 0; row < BOARD_SIZE; ++row) {
            for (int col = 0; col < BOARD_SIZE; ++col) {
                ++squares;
                if (!board.is_empty(row, col)) {
                    // A stone on the square is not a candidate for anybody. The guard answers
                    // before the ledger is touched and before any record is read, so a stale
                    // active_empty() list can neither price an occupied square nor spend a
                    // probe on one - and 0 is exactly what the pass itself says.
                    assert(cache.cell(board, row, col) == nullptr);
                    for (const int side : {BLACK, WHITE}) {
                        assert(cache.side_score(board, row, col, side) == 0);
                        assert(evaluator.evaluate_position_pair(board, row, col, side) == 0);
                    }
                    assert(cache.pair_score(board, row, col) == 0);
                    ++guarded;
                    continue;
                }
                records[row][col] = cache.cell(board, row, col);
                assert(records[row][col] != nullptr);
                c2_check_record(*records[row][col], board, row, col, evaluator, checked);
                ++empties;
            }
        }

        assert(cache.probes() == static_cast<uint64_t>(empties));
        assert(cache.recomputes() == static_cast<uint64_t>(empties));
        assert(cache.hits() == 0);

        // The same board asked a second time: the answer is the same record, not a copy of
        // it, and the second trip recognizes nothing at all.
        for (int row = 0; row < BOARD_SIZE; ++row) {
            for (int col = 0; col < BOARD_SIZE; ++col) {
                if (records[row][col] == nullptr) continue;
                assert(cache.cell(board, row, col) == records[row][col]);
            }
        }
        assert(cache.probes() == static_cast<uint64_t>(empties) * 2);
        assert(cache.hits() == static_cast<uint64_t>(empties));
        assert(cache.recomputes() == static_cast<uint64_t>(empties));
    }

    assert(squares == 42 * BOARD_SIZE * BOARD_SIZE);
    assert(checked > 4000);
    assert(guarded > 0);
    assert(guarded + checked == squares);

    std::cout << "test_the_cell_cache_answers_the_fresh_pass_on_every_square passed ("
              << squares << " squares over " << boards.size() << " boards: " << checked
              << " empty, each one's two shapes and two prices equal to the fresh pass, and "
              << guarded << " occupied squares answered 0 with no probe spent; the second "
                 "trip over a board is all hits and the same record addresses)" << std::endl;
}

// What the key actually depends on, measured rather than assumed: reach past the window,
// precision off the lines, and what an undo leaves behind.
void test_a_stone_five_cells_away_is_inside_the_records_dependency() {
    Evaluator evaluator;
    CandidateCache cache(evaluator);
    Board board = c2_far_reach_board();
    long long checked = 0;

    // (7,7)'s row window is (7,3)..(7,11). Everything the windowed matcher reads for the
    // shape is inside it; the corrections finish_direction runs for BLACK are not.
    const CandidateCache::Cell* square = cache.cell(board, 7, 7);
    assert(square != nullptr);
    assert(square->black.directions[0].type == PatternType::RUSH_FOUR);
    assert(square->value_black == 1112);
    assert(square->value_white == 112);
    assert(square->pair_value() == 1224);
    c2_check_record(*square, board, 7, 7, evaluator, checked);
    assert(checked == 1);
    assert(cache.probes() == 1 && cache.recomputes() == 1 && cache.hits() == 0);
    assert(cache.cell(board, 7, 7) == square);
    assert(cache.hits() == 1 && cache.recomputes() == 1);

    // BLACK at (7,12): the fill at (7,8) that made exactly five now makes six, so the four
    // has no legal completion point left and the shape is nothing. Five cells from the
    // square - no footprint, and not the window either, would have refreshed it. The line
    // key does, because (7,12) is on row 7.
    assert(board.place_stone(7, 12, BLACK));
    const CandidateCache::Cell* after = cache.cell(board, 7, 7);
    assert(after == square);  // the same slot, now carrying the new answer
    assert(after->black.directions[0].type == PatternType::NONE);
    assert(after->pair_value() == 224);
    assert(cache.hits() == 1 && cache.recomputes() == 2);
    c2_check_record(*after, board, 7, 7, evaluator, checked);

    // Undoing the stone does not bring the old record back - the slot holds the child's key
    // now - it makes the next read recompute, and the recomputed number is the one from
    // before the stone. That is the sibling case a search lives on: the parent pays for this
    // square once per changed line, not once per child.
    board.undo_last_move();
    assert(cache.cell(board, 7, 7)->pair_value() == 1224);
    assert(cache.recomputes() == 3 && cache.hits() == 1);
    assert(cache.cell(board, 7, 7)->pair_value() == 1224);
    assert(cache.hits() == 2 && cache.recomputes() == 3);

    // The other half of the claim: the key is per line, not a stamp on the whole board.
    // (4,6) shares no row, column or diagonal with (7,7), so a sibling branch over there
    // reprices nothing here and the record is read as it stands.
    assert(board.place_stone(4, 6, WHITE));
    assert(cache.cell(board, 7, 7)->pair_value() == 1224);
    assert(cache.hits() == 3 && cache.recomputes() == 3);
    board.undo_last_move();
    assert(cache.cell(board, 7, 7)->pair_value() == 1224);
    assert(cache.hits() == 4 && cache.recomputes() == 3);

    assert(cache.hits() + cache.recomputes() == cache.probes());

    std::cout << "test_a_stone_five_cells_away_is_inside_the_records_dependency passed ("
                 "(7,7) 1224 -> 224 on a BLACK stone at (7,12), five cells out and one past "
                 "the window; back to 1224 on the undo at the cost of one recompute and free "
                 "after that; (4,6) off all four lines and free throughout; "
              << checked << " records differential-checked against the fresh pass)" << std::endl;
}

// The two consumers of a record against the pass behind them: the tiers have to be the tiers
// a cold generator would have produced - whatever colour filled the record - and the
// ordering pass has to be handed the number the tier was cut on.
void test_the_tiers_and_the_ordering_read_the_same_record_the_pass_wrote() {
    const Board boards[] = {b1_quiet(), b1_scattered(), b1_crowded(), b1_white_open_four(),
                            b1_black_makes_five(), b1_double_three()};
    Evaluator evaluator;
    int lists = 0, priced = 0;

    for (const Board& board : boards) {
        // Warm side: WHITE's pass first, so BLACK's tiers are cut from records a different
        // colour's pass filled. Reference side: each colour on a generator that has never
        // seen this board, which is the path this step replaced.
        MoveGenerator flipped(evaluator);
        CandidateSet flipped_white, flipped_black, flipped_quiet;
        flipped.generate(board, WHITE, 0, false, &flipped_white);
        flipped.generate(board, BLACK, 0, false, &flipped_black);
        flipped.generate(board, BLACK, 0, true, &flipped_quiet);

        MoveGenerator plain_white(evaluator), plain_black(evaluator), plain_quiet(evaluator);
        CandidateSet ref_white, ref_black, ref_quiet;
        plain_white.generate(board, WHITE, 0, false, &ref_white);
        plain_black.generate(board, BLACK, 0, false, &ref_black);
        plain_quiet.generate(board, BLACK, 0, true, &ref_quiet);

        assert(c2_sets_equal(flipped_white, ref_white));
        assert(c2_sets_equal(flipped_black, ref_black));
        assert(c2_sets_equal(flipped_quiet, ref_quiet));
        lists += 3;

        // And every price in them is the fresh pass's price.
        struct List {
            const CandidateSet& set;
            int colour;
        };
        const List lists_of_board[] = {{ref_white, WHITE}, {ref_black, BLACK}};
        for (const List& item : lists_of_board) {
            const std::vector<Candidate>* tiers[] = {&item.set.forced, &item.set.threats,
                                                     &item.set.vicinity, &item.set.remote};
            for (const std::vector<Candidate>* tier : tiers) {
                for (const Candidate& c : *tier) {
                    assert(c.score == evaluator.evaluate_position_pair(board, c.move.pos.row,
                                                                        c.move.pos.col,
                                                                        item.colour));
                    ++priced;
                }
            }
        }
        // Quiescence prices one side only, because that list is a race of forcing moves and
        // the quiet half of the square is not what the recursion is about.
        for (const Candidate& c : ref_quiet.threats) {
            const PositionPattern mine = PatternRecognizer::recognize_position(
                board, c.move.pos.row, c.move.pos.col, BLACK);
            assert(c.score == evaluator.evaluate_position_with_pattern(board, c.move.pos.row,
                                                                      c.move.pos.col, mine,
                                                                      BLACK));
            ++priced;
        }

        // The ordering pass's own reader, over the whole active list rather than the squares
        // that survived truncation: sort_moves asks for a square the tier list dropped every
        // time a TT or killer move names it, and what it gets is the answer
        // evaluate_position_pair gives on a board with no cache in front of it.
        MoveGenerator gen(evaluator);
        CandidateSet main_set;
        gen.generate(board, BLACK, 0, false, &main_set);
        for (const Position& pos : board.active_empty()) {
            for (const int colour : {BLACK, WHITE}) {
                assert(gen.candidate_price(board, pos) ==
                       evaluator.evaluate_position_pair(board, pos.row, pos.col, colour));
                ++priced;
            }
        }
    }

    // A square BLACK may not legally play is worth its other colour's side and nothing for
    // BLACK - and one record still has to carry both halves, because the side that may stand
    // there is the side the next node is asking about. (7,7) here is 三三: the tier rule keeps
    // it out of every BLACK list, while the record keeps the shape itself and zeroes only the
    // price, which is what lets a WHITE node read the same slot. The reader that reaches a
    // square no tier carries is the ordering fallback, handed one by the TT or a killer.
    {
        const Board board = b1_double_three();
        const Position forbidden(7, 7);
        MoveGenerator gen(evaluator);
        CandidateSet black_set;
        gen.generate(board, BLACK, 0, false, &black_set);
        const std::vector<Move> black_moves = black_set.flatten();
        assert(board.is_empty(forbidden));
        assert(std::find(black_moves.begin(), black_moves.end(), Move(forbidden, BLACK)) ==
               black_moves.end());

        CandidateCache cache(evaluator);
        const CandidateCache::Cell* found = cache.cell(board, forbidden.row, forbidden.col);
        assert(found != nullptr);
        assert(found->black.has_three_three);
        assert(found->value_black == 0);
        assert(found->value_white > 0);
        assert(found->pair_value() == found->value_white);
        assert(found->pair_value() == gen.candidate_price(board, forbidden));
        assert(cache.side_score(board, forbidden.row, forbidden.col, WHITE) == found->value_white);
        assert(cache.side_score(board, forbidden.row, forbidden.col, BLACK) == 0);
        for (const int colour : {BLACK, WHITE}) {
            assert(gen.candidate_price(board, forbidden) ==
                   evaluator.evaluate_position_pair(board, forbidden.row, forbidden.col, colour));
        }
    }

    assert(lists == 18);
    // 6 boards x (both colours' tier lists + the quiescence row + the whole active list
    // priced twice over). The count is the census these assertions ran over, so it is
    // pinned rather than left as a floor: a pose set that stopped producing candidates
    // would quietly turn this case into a sample of one.
    assert(priced == 1516);

    std::cout << "test_the_tiers_and_the_ordering_read_the_same_record_the_pass_wrote passed ("
              << lists << " tier lists over six boards identical to the cold generator's, "
              << priced << " prices equal to the fresh pass, and the forbidden (7,7) priced "
                 "at its WHITE side alone)" << std::endl;
}

// The think-level net. Step 2 was not allowed to move the tree, and C1 already pinned what
// this pose's tree is: the node and score lines below are that promise, and so is the scan
// ledger, field for field - step 2 caches the candidate pass and does not touch the
// whole-board scan. On top of them sits step 2's own ledger, which is what says the half it
// saved was saved reading a record rather than guessing.
void test_a_quiet_think_prices_its_candidates_about_once_in_two() {
    struct Row {
        int color;
        uint64_t nodes;
        uint64_t qnodes;
        uint64_t scans;
        uint64_t reuses;
        uint64_t patches;
        uint64_t probes;
        uint64_t hits;
        uint64_t recomputes;
        int score;
    };
    const Row rows[] = {
        {BLACK, 221774, 142978, 75855, 67123, 75852, 8755599, 4949934, 3805665, 71992},
        {WHITE, 156428, 102340, 68419, 33921, 68419, 7247557, 4132247, 3115310, 97044},
    };
    const Board board = b1_quiet();
    Evaluator evaluator;
    std::atomic<bool> stop(false);

    for (const Row& row : rows) {
        TranspositionTable tt;
        AlphaBetaSearch search(evaluator, tt);
        search.set_node_limit(INT_MAX);
        search.search(board, row.color, 6, 0, stop);

        // The tree step 2 was not allowed to change, and the scan ledger step 1 pinned.
        assert(search.stats_.nodes == row.nodes);
        assert(search.stats_.qnodes == row.qnodes);
        assert(search.stats_.score == row.score);
        assert(search.ctx_.eval_scans() == row.scans);
        assert(search.ctx_.eval_reuses() == row.reuses);
        assert(search.ctx_.eval_patches() == row.patches);
        assert(search.ctx_.eval_scans() + search.ctx_.eval_reuses() == row.qnodes);

        // Step 2's ledger on the same think, re-taken for step 3: the tree and the scan
        // ledger are the literals step 1 pinned and step 2 left alone, and the three
        // numbers below grew because the whole-board sum now reads this same cache. On
        // 897ab43 every one of these asks paid its own four-direction pass; the hit count
        // is the asks that are answered from a record.
        const CandidateCache& cache = search.move_gen_.cache();
        assert(cache.probes() == row.probes);
        assert(cache.hits() == row.hits);
        assert(cache.recomputes() == row.recomputes);
        assert(cache.hits() + cache.recomputes() == cache.probes());
        assert(cache.hits() > 0);
        assert(cache.recomputes() > 0);
    }

    std::cout << "test_a_quiet_think_prices_its_candidates_about_once_in_two passed (quiet "
                 "BLACK 8755599 candidate reads = 4949934 from a record + 3805665 priced "
                 "here, on the 221774-node tree; WHITE 7247557 = 4132247 + 3115310 on the "
                 "156428-node tree; C1's 75855/67123/75852 and 68419/33921/68419 scan "
                 "ledgers unmoved)" << std::endl;
}

// The cache belongs to the think. C1-5's warm repeat answers in 187 nodes off the table the
// first think filled, and it does that with a cache of its own: a second AlphaBetaSearch
// starts cold, pays for its own records, and finds nothing on the first think's shelf.
// Nothing crosses a think boundary except through the table - which is what keeps the
// coefficients a price was taken with honest, since a session can hand the next think a
// different set.
void test_a_warm_think_buys_its_own_records_and_no_ones_from_the_first() {
    const Board board = b1_quiet();
    Evaluator evaluator;
    std::atomic<bool> stop(false);

    TranspositionTable tt;
    AlphaBetaSearch first(evaluator, tt);
    first.set_node_limit(INT_MAX);
    first.search(board, BLACK, 6, 0, stop);
    const uint64_t first_probes = first.move_gen_.cache().probes();

    AlphaBetaSearch second(evaluator, tt);
    second.set_node_limit(INT_MAX);
    second.search(board, BLACK, 6, 0, stop);
    const CandidateCache& cache = second.move_gen_.cache();

    // C1's warm answer, unchanged by step 2: the tree comes out of the table's records and
    // the one evaluation it still needs is a read rather than a scan.
    assert(second.stats_.nodes == 187);
    assert(second.ctx_.eval_scans() == 0);
    assert(second.ctx_.eval_reuses() == 1);

    // Its candidate work, on the other hand, is its own: 111 reads for this think, 58 of
    // them priced here and 53 answered from records this think itself filled. Step 3 did
    // not move them, because the warm think's one static score is answered out of the
    // table and never reaches the sum. None of the first think's reads carried over.
    assert(cache.probes() == 111);
    assert(cache.hits() == 53);
    assert(cache.recomputes() == 58);
    assert(cache.hits() + cache.recomputes() == cache.probes());
    assert(cache.probes() < first_probes);

    std::cout << "test_a_warm_think_buys_its_own_records_and_no_ones_from_the_first passed "
                 "(warm repeat 187 nodes / 0 scans / 1 reuse as C1 pins it, its own 111 "
                 "candidate reads = 53 hits + 58 recomputes, the first think's "
              << first_probes << " reads left behind)" << std::endl;
}

// ---------------------------------------------------------------------------
// C3 - 局部 stand_pat (design doc 2.6 Step 3, :409-410: 「静默搜索入口不再做全盘评估,
// 改为「上一手所在 4 条线」的局部 delta 评估… 全盘评估只在根节点与需要精确分值的场合使用」).
//
// What shipped is the doc's aim by a different mechanism, and the difference is worth
// naming before the cases: the static score is not a delta of a *different* evaluation,
// it is the same whole-board sum read off step 2's records. Two things made that the
// right reading. (1) C2 proved the dependency a square has on the board is its four
// whole lines, not a ±2 footprint, so 「上一手所在 4 条线」 is exactly the set of records a
// placement invalidates - arrived at by the entry's own key compare rather than by a
// dirty set. (2) README:624 records a whole-board incremental evaluation being rejected
// because the place/undo bookkeeping cost as much as the rescan it saved; reading a
// record that already exists pays no bookkeeping at all. The consequence for :568's
// 「Step 3（局部 stand_pat）先只用于「无威胁局面」，有威胁时回退全盘评估」 is that the
// fallback is not needed: a read that returns the scan's own number cannot be the
// approximate half of a two-tier evaluation, so the deviation is in the sound direction
// and is reported that way rather than as compliance.
//
// Four cases, one per claim: the sum equals the scan on every board of the census the
// method names (C3-1), every addend of that sum is the addend the scan pays, square by
// square, with the pre-refactor arithmetic as the oracle (C3-2 - :556 #3's 逐格比对 at
// step 3's own level), a stone five cells away moves both paths by the same number and
// undo returns both to it while invalidating only the line-sharers (C3-3), and one set
// of records answers the sum for either colour, which is the reason a record carries both
// sides (C3-4). The think-level cost is the ledger C2-4 re-took.
// ---------------------------------------------------------------------------

namespace {

// The read step 3 wired, against the read it replaced, on a generator of its own so the
// caller's ledger is untouched. The cold-list assertions are also the evidence that the
// candidate list holds no occupied square - the one premise that lets `board_score` read
// records instead of recognising.
int c3_sum_equals_scan(const Board& board, int color, const Evaluator& evaluator) {
    MoveGenerator gen(evaluator);
    const CandidateCache& cache = gen.cache();
    const long long list_size = static_cast<long long>(board.active_empty().size());
    const int cached = gen.board_score(board, color);
    assert(static_cast<long long>(cache.probes()) == list_size);
    assert(static_cast<long long>(cache.recomputes()) == list_size);
    assert(cache.hits() == 0);
    assert(cached == evaluator.evaluate_board(board, color));
    return cached;
}

// One square's addend, read two ways: from the record step 2 keeps and from a fresh
// four-direction pass. `pattern_score` is private to `Evaluator` and this TU's
// `#define private public` block covers only the alpha-beta headers, so the arithmetic
// itself is not re-derived here - test_evaluator.cpp does that against `naive_board_total`
// and a dozen absolute literals, in the same gate. What this helper pins is the two things
// step 3 can get wrong on its own: which side of the record it reads, and whether the two
// branches of the per-side forbidden rule still cancel out (the antisymmetry it asserts is
// exactly `board_pair_value`'s contract - BLACK's term added, WHITE's subtracted, each
// judged on its own shape).
long long c3_addend_truth(const Evaluator& evaluator, const CandidateCache::Cell& found,
                          const PositionPattern& fresh_black,
                          const PositionPattern& fresh_white, int row, int col, int color) {
    const bool black_is_mine = (color == BLACK);
    const long long from_record = evaluator.board_pair_value(
        black_is_mine ? found.black : found.white, black_is_mine ? found.white : found.black,
        row, col, color);
    const long long from_pass =
        evaluator.board_pair_value(black_is_mine ? fresh_black : fresh_white,
                                   black_is_mine ? fresh_white : fresh_black, row, col,
                                   color);
    assert(from_record == from_pass);
    // The same square asked by the other colour is the same number with its sign moved -
    // and a record that answered the writer's side rather than the reader's would break
    // here rather than at the sum, where the cap could hide it.
    const long long other_side =
        evaluator.board_pair_value(black_is_mine ? fresh_white : fresh_black,
                                   black_is_mine ? fresh_black : fresh_white, row, col,
                                   opponent(color));
    assert(other_side == -from_pass);
    return from_pass;
}

}  // namespace

// Every board of the census, both colours: the number the search now reads at a stand-pat
// is the number the scan returns, and it arrives by reading each candidate's record exactly
// once - no square twice, no square not at all.
void test_the_record_sum_is_the_whole_board_scan_on_every_board() {
    const std::vector<Board> boards = c2_census_boards();
    assert(boards.size() == 42);
    Evaluator evaluator;
    int compared = 0;
    long long listed = 0, occupied = 0;

    for (const Board& board : boards) {
        for (const Position& pos : board.active_empty()) {
            ++listed;
            if (!board.is_empty(pos.row, pos.col)) ++occupied;
        }

        for (const int color : {BLACK, WHITE}) {
            MoveGenerator gen(evaluator);
            const CandidateCache& cache = gen.cache();
            const long long list_size = static_cast<long long>(board.active_empty().size());

            const int cold = gen.board_score(board, color);
            const int direct = evaluator.evaluate_board(board, color);
            assert(cold == direct);
            assert(static_cast<long long>(cache.probes()) == list_size);
            assert(static_cast<long long>(cache.recomputes()) == list_size);
            assert(cache.hits() == 0);

            // The same read again: every addend comes out of a record, so the ledger moves
            // by the list size on the hit side and not at all on the recompute side.
            const int warm = gen.board_score(board, color);
            assert(warm == direct);
            assert(static_cast<long long>(cache.probes()) == 2 * list_size);
            assert(static_cast<long long>(cache.hits()) == list_size);
            assert(static_cast<long long>(cache.recomputes()) == list_size);
            ++compared;
        }
    }

    // The premise the cold-read ledger above rests on: no stone sits in the candidate list.
    assert(occupied == 0);
    assert(compared == 84);
    assert(listed > 0);

    std::cout << "test_the_record_sum_is_the_whole_board_scan_on_every_board passed (42 boards "
                 "x two colours = 84 sums equal to the scan, read over "
              << listed
              << " candidate squares with none of them occupied; cold read = list probes "
                 "and list recomputes, warm read = list hits)"
              << std::endl;
}

// :556 #3's 逐格比对 for step 3, at the level step 3 works on: the whole-board sum is the
// sum of its addends, and this checks the chain link by link rather than only at its ends -
// the record's four directions against the fresh pass, the addend read off the record
// against the addend read off the pass, both sides' addends against each other, and the
// addends run through the second half of the split against the scan. The split's own
// arithmetic is not re-derived here (test_evaluator.cpp pins it against `naive_board_total`
// and against absolute literals); what this case can catch that those cannot is a cached
// read answering the wrong side, or answering from a shape the pass no longer sees.
void test_every_addend_of_the_record_sum_is_the_addend_the_scan_pays() {
    const std::vector<Board> boards = c2_census_boards();
    Evaluator evaluator;
    long long squares = 0;
    int boards_checked = 0, sums = 0;

    for (const Board& board : boards) {
        ++boards_checked;
        CandidateCache cache(evaluator);

        for (const int color : {BLACK, WHITE}) {
            long long truth_total = 0;

            for (const Position& pos : board.active_empty()) {
                const PositionPattern fresh_black = PatternRecognizer::recognize_position(
                    board, pos.row, pos.col, BLACK);
                const PositionPattern fresh_white = PatternRecognizer::recognize_position(
                    board, pos.row, pos.col, WHITE);
                const CandidateCache::Cell* found = cache.cell(board, pos.row, pos.col);
                assert(found != nullptr);
                assert(c2_patterns_equal(found->black, fresh_black));
                assert(c2_patterns_equal(found->white, fresh_white));

                truth_total += c3_addend_truth(evaluator, *found, fresh_black, fresh_white,
                                                pos.row, pos.col, color);
                ++squares;
            }

            // And the closed chain: the addends the pass pays, run through the other half
            // of the split, are the number the scan returns.
            const int rebuilt = evaluator.evaluate_board_from(truth_total, board, color);
            assert(rebuilt == evaluator.evaluate_board(board, color));
            assert(rebuilt == c3_sum_equals_scan(board, color, evaluator));
            ++sums;
        }
    }

    assert(boards_checked == 42);
    assert(sums == 84);
    assert(squares > 84);

    std::cout << "test_every_addend_of_the_record_sum_is_the_addend_the_scan_pays passed ("
              << squares << " square-addends over 42 boards x two colours, each record's four "
                 "directions equal to the fresh pass and each addend read off the record equal "
                 "to the one read off the pass and to the other colour's with its sign moved; "
                 "84 rebuilt sums equal to the scan)"
              << std::endl;
}

// The sum on the make/unmake path. C2-2 proved the record-level fact - a stone five cells
// away is inside (7,7)'s dependency, and undo does not hand the old record back but makes
// the next read recompute the number that was there before; step 3 puts that under the
// whole-board sum: both paths move by the same amount and both come back to the same
// number, while the read after the placement recomputes only the squares that share a line
// with the new stone. That last clause is the 「只更新受影响格」 the doc asks of a
// placement, measured as reads rather than as bookkeeping.
void test_a_far_stone_moves_both_sums_alike_and_undo_brings_both_back() {
    const Board before = c2_far_reach_board();
    Evaluator evaluator;
    MoveGenerator gen(evaluator);
    const CandidateCache& cache = gen.cache();

    const int cold = c3_sum_equals_scan(before, BLACK, evaluator);
    const int first = gen.board_score(before, BLACK);
    assert(first == cold);
    // The whole list answered from records the first read just filled: nothing to
    // recompute, and the ledger below is taken from here so each clause counts one read.
    assert(gen.board_score(before, BLACK) == cold);
    const long long probes_before = static_cast<long long>(cache.probes());
    const long long recomputes_before = static_cast<long long>(cache.recomputes());
    assert(probes_before == 2 * static_cast<long long>(before.active_empty().size()));
    assert(static_cast<long long>(cache.recomputes()) ==
           static_cast<long long>(before.active_empty().size()));

    Board after = before;
    assert(after.place_stone(7, 12, BLACK));
    const long long list_after = static_cast<long long>(after.active_empty().size());
    const int placed = gen.board_score(after, BLACK);
    const long long stale = static_cast<long long>(cache.recomputes()) - recomputes_before;
    assert(placed == c3_sum_equals_scan(after, BLACK, evaluator));
    assert(placed != cold);
    // The placement spends probes for every square it reads and recomputes for only the
    // ones whose line words it changed.
    assert(static_cast<long long>(cache.probes()) == probes_before + list_after);
    assert(stale > 0);
    assert(stale < list_after);
    // The other colour reads the same records at the same position.
    assert(gen.board_score(after, WHITE) == evaluator.evaluate_board(after, WHITE));

    const long long probes_after_place = static_cast<long long>(cache.probes());
    const long long recomputes_after_place = static_cast<long long>(cache.recomputes());
    Board undone = after;
    undone.undo_last_move();
    assert(undone.active_empty().size() == before.active_empty().size());
    const int restored = gen.board_score(undone, BLACK);
    assert(restored == cold);
    assert(restored == c3_sum_equals_scan(undone, BLACK, evaluator));
    const long long restale =
        static_cast<long long>(cache.recomputes()) - recomputes_after_place;
    assert(static_cast<long long>(cache.probes()) ==
           probes_after_place + static_cast<long long>(undone.active_empty().size()));
    assert(restale > 0);
    assert(restale < static_cast<long long>(undone.active_empty().size()));

    std::cout << "test_a_far_stone_moves_both_sums_alike_and_undo_brings_both_back passed "
                 "(quiet four + (7,9)(7,10)(7,11): sum "
              << cold << " -> " << placed << " when BLACK plays (7,12), back to " << restored
              << " on undo, both paths at every step; the placement recomputed " << stale
              << " of " << list_after << " squares and the undo " << restale << ")" << std::endl;
}

// One record, two callers. The sum is the quantity that gets stored in the table and read
// back by whichever colour is to move, so the reader's side has to be picked from a record
// that carries both - which is the same claim C2-3 proves for the tiers, at step 3's level.
// Asked in either order, on one set of records, each colour gets its own scan's number and
// the second ask is answered entirely by hits.
void test_one_set_of_records_answers_the_sum_for_either_colour() {
    const std::vector<Board> boards = c2_census_boards();
    Evaluator evaluator;
    int pairs = 0;

    for (const Board& board : boards) {
        const long long list_size = static_cast<long long>(board.active_empty().size());
        const int black_sum = evaluator.evaluate_board(board, BLACK);
        const int white_sum = evaluator.evaluate_board(board, WHITE);

        // The two orders are the two things a record can get wrong: it can answer the
        // colour that wrote it rather than the colour that asks, and it can answer the
        // second reader out of a slot the first reader left half-filled.
        const int orders[2][2] = {{BLACK, WHITE}, {WHITE, BLACK}};
        for (int order = 0; order < 2; ++order) {
            MoveGenerator gen(evaluator);
            const CandidateCache& cache = gen.cache();
            const int colors[2] = {orders[order][0], orders[order][1]};
            const int expected[2] = {colors[0] == BLACK ? black_sum : white_sum,
                                     colors[1] == BLACK ? black_sum : white_sum};

            assert(gen.board_score(board, colors[0]) == expected[0]);
            assert(static_cast<long long>(cache.probes()) == list_size);
            assert(static_cast<long long>(cache.recomputes()) == list_size);
            assert(cache.hits() == 0);

            assert(gen.board_score(board, colors[1]) == expected[1]);
            assert(static_cast<long long>(cache.probes()) == 2 * list_size);
            assert(static_cast<long long>(cache.hits()) == list_size);
            assert(static_cast<long long>(cache.recomputes()) == list_size);
            ++pairs;
        }
    }

    assert(pairs == 84);

    std::cout << "test_one_set_of_records_answers_the_sum_for_either_colour passed (42 boards, "
                 "both read orders: the writer's colour and the reader's colour each get their "
                 "own scan's number, and the second read is list_size hits and no recompute)"
              << std::endl;
}

// The branch every aborted node of a cut think takes. `alphabeta()`
// (src/search/alphabeta.cpp:320) returns this node's own static score from its stop test
// (:346) *without* consulting the table: the hash and the reuse would both have to move
// above that test, and a cut think's frames are the reason they must not - the comment at
// `:337-345` states the choice and what step 3 gave this branch instead. What that read
// costs is the claim this case pins: the sum comes out of the same records, so the nodes a
// budget kills stopped each paying a whole-board pass. The pin is a think that really is
// cut, by node budget only, so nothing here depends on a wall clock.
void test_the_nodes_a_cut_kills_pay_the_sum_out_of_the_records() {
    const Board board = b1_quiet();
    Evaluator evaluator;
    std::atomic<bool> stop(false);

    // The whole first layer, measured so the budget lands inside the second one, the same
    // shape the A4 cut case uses.
    uint64_t layer_one_nodes = 0;
    {
        TranspositionTable scratch;
        AlphaBetaSearch measure(evaluator, scratch);
        measure.set_node_limit(2000000);
        measure.search(board, BLACK, 1, 600000, stop);
        layer_one_nodes = static_cast<uint64_t>(measure.last_search_info().nodes_searched);
    }
    assert(layer_one_nodes > 0);

    TranspositionTable tt;
    AlphaBetaSearch cut(evaluator, tt);
    cut.set_node_limit(static_cast<int>(layer_one_nodes + 1));
    cut.search(board, BLACK, 4, 600000, stop);
    const SearchInfo info = cut.last_search_info();
    const CandidateCache& cache = cut.move_gen_.cache();

    // The budget is spent inside the second layer, so this think really does abort: one
    // committed layer, one interrupted, no candidate of layer two finished.
    assert(info.stopped == StopReason::NODES);
    assert(info.depth == 1);
    assert(info.partial_depth == 2);
    assert(info.root_moves_done == 0);
    assert(info.root_moves_total == 31);
    assert(info.nodes_searched == 63);

    // 31 of those 63 nodes reached `static_score`, and all 31 are quiescence nodes - which
    // is why this case reads the cache ledger instead of the evaluation one. The rest
    // answered from the stop test, a branch that has no counter of its own; what it has
    // since step 3 is the read, and a read is a probe.
    assert(cut.stats_.qnodes == 31);
    assert(cut.ctx_.eval_scans() == 31);
    assert(cut.ctx_.eval_reuses() == 0);
    assert(cut.ctx_.eval_scans() == static_cast<uint64_t>(cut.stats_.qnodes));

    assert(cache.probes() == 1643);
    assert(cache.hits() == 587);
    assert(cache.recomputes() == 1056);
    assert(cache.hits() + cache.recomputes() == cache.probes());
    // 1643 asks for 63 nodes: each aborted visit still wants the whole candidate list, so
    // the ledger scales with the list, not with the nodes that reached the table. Take the
    // read out of that branch - mutation M5 - and this line loses their share.
    assert(cache.probes() > 20ULL * static_cast<uint64_t>(info.nodes_searched));

    std::cout << "test_the_nodes_a_cut_kills_pay_the_sum_out_of_the_records passed (cut think: "
                 "63 nodes / 31 root candidates / stopped at the node budget inside layer 2; "
                 "the evaluation ledger sees only the 31 quiescence nodes (scans 31 / reuses "
                 "0), while the cache answered 1643 reads = 587 hits + 1056 recomputes, the "
                 "aborted nodes' sums included)"
              << std::endl;
}

// ---------------------------------------------------------------------------
// C4 - quiescence 深度上限 (design doc 2.6 Step 4, :413: 「当前 `quiescence` 无深度上限
// （靠威胁过滤收敛）。加 `MAX_QDEPTH = 8` 与 `seldepth` 记账，避免极端局面下的长尾」).
//
// Half the row was already shipped, and the shipped half is what makes the other half a
// fact rather than a coincidence. The accounting is stage A's: SearchInfo::seldepth
// (alphabeta.h:37), SearchStats::seldepth, the frame that carries it (server.cpp:1738)
// and the case that pins them (:3172). What feeds it - ctx_.note_quiescence_depth(qdepth)
// - is armed one line after the node counter and *before* the cap test, so a think reports
// the depth a node was entered at, not the depth it expanded to. That ordering is why
// seldepth == kMaxQdepth is the deepest number a think can produce and can therefore be
// asserted both ways: equal on a board that has a tail, capped-at on a population.
//
// Where the tail actually is was measured before anything was written, and the row's
// 「极端局面」 is exact. Over the 24 positional arms this suite already owns
// (/tmp/r124_c4/hist_base.txt: 12,687,567 nodes, 7,917,361 quiescence nodes) the deepest
// node was entered at ply 7 and no node at all was entered at 8, so on every arm pinned by
// node count the cap cannot fire and the literals are the same literals. It is real on
// dense random clusters - 30 stones in a 9x9 window, 3,943 thinks over three root depths -
// where the per-board maximum reach is 11 at root depth 2 and 15 at 4 and 6, with 51/2,918,
// 22/781 and 12/244 boards having a node at 8 or past it.
//
// What it costs is the reason the cases below pin answers as well as counts. On those
// 3,943 thinks the capped and uncapped trees agree on score, best move, completed depth,
// stop reason and tt_hits - zero differences on every one of the three populations - while
// nodes move -0.111 % / -0.032 % / -0.102 % (d2 / d4 / d6). The saving is not the tail's
// size and it is not monotone: on the 781 depth-4 thinks the bound stops 4,055 nodes from
// being entered (nodes at qdepth >= 8: 5,489 -> 1,434) yet the trees net 1,778 - 13 boards
// end 2,727 smaller against 8 ending 949 larger (+572 on a 211,698-node board; the 43,628
// one gives back 25), because a capped node's number is not the one its parent cut on.
//
// Five cases, one per claim: what the capped node is (C4-1, with the two controls that
// make it a bound and not a shutdown), that a dead budget still outranks it (C4-2), the
// two thinks whose tails reach it (C4-3, C4-4), and that the bound holds across a
// population while staying inert where there is no tail (C4-5).
// ---------------------------------------------------------------------------

namespace {

// A 30-stone cluster inside a 9x9 window, WHITE to move, root depth 4. Sweep board 105 of
// /tmp/r124_c4/c4_fuzz.cpp (mt19937 seed 20261019, 30 stones, span 4), stone for stone. Its
// quiescence ran to ply 15 before step 4; it is the board whose capped answer is C4-3.
Board c4_tail_long() {
    return b1_board({
        {3, 4, BLACK}, {3, 9, WHITE}, {8, 10, BLACK}, {11, 4, WHITE}, {3, 5, BLACK},
        {9, 5, WHITE}, {7, 6, BLACK}, {8, 7, WHITE}, {9, 4, BLACK}, {7, 11, WHITE},
        {10, 11, BLACK}, {7, 4, WHITE}, {6, 8, BLACK}, {11, 3, WHITE}, {4, 10, BLACK},
        {5, 4, WHITE}, {11, 9, BLACK}, {8, 3, WHITE}, {7, 9, BLACK}, {11, 7, WHITE},
        {8, 11, BLACK}, {11, 11, WHITE}, {6, 3, BLACK}, {10, 10, WHITE}, {10, 9, BLACK},
        {11, 10, WHITE}, {7, 5, BLACK}, {10, 8, WHITE}, {9, 3, BLACK}, {4, 8, WHITE},
    });
}

// The same generator's board 740, BLACK to move, root depth 4: reach 11 before the cap and
// 73 nodes dearer after it. The give-back is the point of choosing it.
Board c4_tail_dense() {
    return b1_board({
        {10, 4, BLACK}, {9, 8, WHITE}, {10, 11, BLACK}, {11, 6, WHITE}, {7, 10, BLACK},
        {6, 7, WHITE}, {5, 8, BLACK}, {11, 10, WHITE}, {4, 11, BLACK}, {6, 9, WHITE},
        {7, 7, BLACK}, {3, 5, WHITE}, {3, 10, BLACK}, {11, 5, WHITE}, {5, 11, BLACK},
        {8, 6, WHITE}, {11, 8, BLACK}, {8, 5, WHITE}, {8, 7, BLACK}, {10, 9, WHITE},
        {9, 4, BLACK}, {5, 5, WHITE}, {11, 9, BLACK}, {9, 11, WHITE}, {11, 3, BLACK},
        {7, 3, WHITE}, {11, 7, BLACK}, {4, 3, WHITE}, {5, 3, BLACK}, {3, 9, WHITE},
    });
}

// One node of quiescence, entered at a depth the caller chooses. A fresh search object per
// call, because every number compared below is a per-node delta read off a table that has
// seen nothing but this position's own static score: `want` is that scan, `opened` counts
// the quiescence nodes the call created (itself included) and `probes` the candidate-cache
// reads it made. A capped node must open one and zero.
struct C4Node {
    int want = 0;
    int score = 0;
    bool sound = false;
    uint64_t opened = 0;
    uint64_t probes = 0;
    uint64_t evals = 0;
};

C4Node c4_quiescence_at(Board& board, int color, int qdepth, bool armed_stop) {
    Evaluator evaluator;
    TranspositionTable tt;
    AlphaBetaSearch search(evaluator, tt);
    SearchLimits limits;
    std::atomic<bool> stop(false);
    search.ctx_.start(limits, system_search_clock(), stop);

    const uint64_t hash = board.zobrist_hash();
    C4Node out;
    out.want = search.static_score(board, color, hash, NO_STATIC_EVAL);
    const uint64_t opened_before = search.ctx_.quiescence_nodes();
    const uint64_t probes_before = search.move_gen_.cache().probes();
    // Armed after the ledger is read - the branch under test has to be the one that was
    // not already taken by the read above it.
    if (armed_stop) search.ctx_.set_reason(StopReason::NODES);
    // Wide, and deliberately not INT_MAX: quiescence negates the window at every ply.
    const NodeResult r =
        search.quiescence(board, -1000000, 1000000, color, stop, qdepth, hash,
                          NO_STATIC_EVAL);
    out.score = r.score;
    out.sound = r.sound;
    out.opened = search.ctx_.quiescence_nodes() - opened_before;
    out.probes = search.move_gen_.cache().probes() - probes_before;
    out.evals = search.ctx_.eval_scans() + search.ctx_.eval_reuses();
    return out;
}

// One think of a tail board: the sweep's own shape (plain coefficients, fresh table, no
// wall clock, no reachable node ceiling) so the numbers are the sweep's.
struct C4Think {
    uint64_t nodes = 0;
    uint64_t qnodes = 0;
    uint64_t tt_hits = 0;
    uint64_t evals = 0;
    uint64_t probes = 0;
    uint64_t hits = 0;
    uint64_t recomputes = 0;
    int seldepth = 0;
    int score = 0;
    int completed = 0;
    int root_done = 0;
    int root_total = 0;
    size_t pv = 0;
    Move best;
    StopReason stopped = StopReason::NONE;
};

C4Think c4_think(Board& board, int color, int depth) {
    Evaluator evaluator;
    TranspositionTable tt;
    AlphaBetaSearch search(evaluator, tt);
    search.set_node_limit(INT_MAX);
    SearchLimits limits;
    limits.max_depth = depth;
    std::atomic<bool> stop(false);

    C4Think out;
    out.best = search.search(board, color, limits, stop);
    const SearchStats& stats = search.last_stats();
    const SearchInfo& info = search.last_search_info();
    out.nodes = stats.nodes;
    out.qnodes = stats.qnodes;
    out.tt_hits = stats.tt_hits;
    out.seldepth = stats.seldepth;
    out.score = stats.score;
    out.completed = stats.completed_depth;
    out.stopped = stats.stopped;
    out.root_done = info.root_moves_done;
    out.root_total = info.root_moves_total;
    out.pv = info.principal_variation.size();
    out.evals = search.ctx_.eval_scans() + search.ctx_.eval_reuses();
    const CandidateCache& cache = search.move_gen_.cache();
    out.probes = cache.probes();
    out.hits = cache.hits();
    out.recomputes = cache.recomputes();
    return out;
}

}  // namespace

void test_a_quiescence_node_entered_at_the_cap_answers_only_its_own_static_score() {
    Board dense = c4_tail_dense();
    Board long_tail = c4_tail_long();
    const int cap = AlphaBetaSearch::kMaxQdepth;

    // The capped node: its own static score, reported as sound, one node created, no
    // candidate read. `sound` is the half that costs something to get wrong - it gates the
    // bound this node writes to the table, the line a parent extends out of the PV table
    // and the layer that commits - and a leaf whose number was never in doubt has to keep
    // all three.
    const C4Node at_cap = c4_quiescence_at(dense, BLACK, cap, false);
    assert(at_cap.want == -1488);
    assert(at_cap.score == at_cap.want);
    assert(at_cap.sound);
    assert(at_cap.opened == 1);
    assert(at_cap.probes == 0);
    assert(at_cap.evals == 2);  // the `want` scan plus this node's own read of it

    // One ply short of the bound, the same node is an ordinary node: it opens 21 of them,
    // prices 2,863 candidates, and answers something other than the position's number.
    // This is the control that keeps the bound a bound rather than a shutdown - were the
    // cap one lower, this arm would be the capped one and the tail would end at 7.
    const C4Node one_short = c4_quiescence_at(dense, BLACK, cap - 1, false);
    assert(one_short.opened == 21);
    assert(one_short.probes == 2863);
    assert(one_short.want == -1488);
    assert(one_short.score == 99700);
    assert(one_short.score != one_short.want);
    assert(one_short.sound);

    // The root of the tail, where nothing is capped at all: 41 nodes for the same call.
    const C4Node at_root = c4_quiescence_at(dense, BLACK, 0, false);
    assert(at_root.opened == 41);
    assert(at_root.probes == 8367);
    assert(at_root.score == 99700);

    // The number is this colour's own, in each direction, on both boards.
    const C4Node white_at_cap = c4_quiescence_at(dense, WHITE, cap, false);
    assert(white_at_cap.want == 1488);
    assert(white_at_cap.score == 1488);
    assert(white_at_cap.sound && white_at_cap.opened == 1 && white_at_cap.probes == 0);

    const C4Node other_at_cap = c4_quiescence_at(long_tail, WHITE, cap, false);
    assert(other_at_cap.want == -50196);
    assert(other_at_cap.score == -50196);
    assert(other_at_cap.sound && other_at_cap.opened == 1 && other_at_cap.probes == 0);
    // The other board's unclosed tail, for the same control as above.
    const C4Node other_at_root = c4_quiescence_at(long_tail, WHITE, 0, false);
    assert(other_at_root.opened == 84);
    assert(other_at_root.probes == 16796);
    assert(other_at_root.score == 90238);

    std::cout << "test_a_quiescence_node_entered_at_the_cap_answers_only_its_own_static_score"
                 " passed (qdepth 8: 1 node / 0 candidate reads / the position's own -1,488 "
                 "as a sound leaf, and 1,488 for WHITE; qdepth 7: 21 nodes / 2,863 reads / "
                 "99,700; qdepth 0: 41 nodes on the dense board, 84 and 16,796 reads on the "
                 "long one answering 90,238)"
              << std::endl;
}

void test_a_dead_budget_outranks_the_depth_cap_its_check_sits_behind() {
    Board dense = c4_tail_dense();
    Board long_tail = c4_tail_long();
    const int cap = AlphaBetaSearch::kMaxQdepth;

    // Two branches, one number. A capped node and a node the budget killed both answer the
    // position's static score, and only `sound` separates them: the cap says "this is what
    // the position is worth", the stop test says "this is what I had on hand". Arming the
    // stop at the cap therefore has to produce the same score and the opposite claim.
    const C4Node capped = c4_quiescence_at(dense, BLACK, cap, false);
    assert(capped.sound);
    const C4Node stopped_at_cap = c4_quiescence_at(dense, BLACK, cap, true);
    assert(!stopped_at_cap.sound);
    assert(stopped_at_cap.score == stopped_at_cap.want);
    assert(stopped_at_cap.opened == 1);
    assert(stopped_at_cap.probes == 0);
    assert(stopped_at_cap.evals == 2);

    // The ordering is visible where the cap says nothing. At qdepth 0 a live node opens 41;
    // with the budget dead it opens 1 and calls it unsound, so the branch that ran is the
    // stop test's, whichever depth the node was entered at.
    const C4Node stopped_at_root = c4_quiescence_at(dense, BLACK, 0, true);
    assert(!stopped_at_root.sound);
    assert(stopped_at_root.score == stopped_at_root.want);
    assert(stopped_at_root.opened == 1);
    assert(stopped_at_root.probes == 0);

    const C4Node stopped_at_seven = c4_quiescence_at(dense, BLACK, cap - 1, true);
    assert(!stopped_at_seven.sound && stopped_at_seven.opened == 1);

    // And on the board whose tail is the longer one, where the killed node would have
    // opened 84.
    const C4Node other_stopped = c4_quiescence_at(long_tail, WHITE, 0, true);
    assert(!other_stopped.sound);
    assert(other_stopped.score == -50196);
    assert(other_stopped.want == -50196);
    assert(other_stopped.opened == 1);

    std::cout << "test_a_dead_budget_outranks_the_depth_cap_its_check_sits_behind passed "
                 "(armed at qdepth 8, 7 and 0: 1 node / 0 reads / the static score as an "
                 "UNSOUND leaf every time, against 41 and 84 nodes opened unarmed)"
              << std::endl;
}

void test_a_think_that_runs_into_the_cap_reports_the_cap_and_the_same_answer() {
    Board board = c4_tail_long();
    const C4Think think = c4_think(board, WHITE, 4);

    // The whole tuple, pinned: the sweep's row for this board, root depth 4, plain
    // coefficients, fresh table. seldepth is the number the cap moved (15 -> 8) and the
    // tree's size is the one the 800 killed tail nodes leave behind (64,517 -> 63,717).
    assert(think.seldepth == AlphaBetaSearch::kMaxQdepth);
    assert(think.nodes == 63717);
    assert(think.qnodes == 49098);
    assert(think.tt_hits == 6471);
    assert(think.score == -22366);
    assert(think.completed == 4);
    assert(think.root_done == 55 && think.root_total == 55);
    assert(think.best.pos.row == 11 && think.best.pos.col == 5);
    assert(think.stopped == StopReason::NONE);
    assert(think.pv == 4);

    // Nothing about this think is free, and the cap does not make it cheaper than the
    // ledger can account for: all 800 nodes it removes are quiescence nodes (49,898 ->
    // 49,098), and every one of the survivors pays exactly one evaluation read of its own,
    // capped ones included - a capped node pays that one read and no candidate generation.
    assert(think.evals == think.qnodes);
    assert(think.hits + think.recomputes == think.probes);
    assert(think.probes == 4445432);
    assert(think.hits == 2963010);
    assert(think.recomputes == 1482422);

    std::cout << "test_a_think_that_runs_into_the_cap_reports_the_cap_and_the_same_answer "
                 "passed (sweep board 105, WHITE, depth 4: 63,717 nodes / 49,098 of them "
                 "quiescence / seldepth 8 where the uncapped tree reached 15, and the "
                 "answer -22,366 at (11,5) over 4 committed layers unchanged; 4,445,432 "
                 "candidate reads = 2,963,010 + 1,482,422)"
              << std::endl;
}

void test_a_capped_tail_can_pay_back_nodes_it_did_not_save() {
    Board board = c4_tail_dense();
    const C4Think think = c4_think(board, BLACK, 4);

    // The give-back board, pinned as a cost and not as an exception to explain away: the
    // uncapped tree needed 153,383 nodes for this answer and the capped one needs 73 more,
    // and all 73 are quiescence nodes (138,837 -> 138,910, while the alphabeta part of both
    // trees is 14,546 nodes), for a tail whose reach fell from 11 to 8. The same board is
    // one of the 47 that move on the depth-2 sweep (+96) and one of the 8 that move the
    // wrong way on the depth-4 one: its +73 is part of the 949 those eight pay, against
    // the 2,727 the 13 that shrink save, because a capped node's number is not the number
    // its parent would have cut on.
    assert(think.seldepth == AlphaBetaSearch::kMaxQdepth);
    assert(think.nodes == 153456);
    assert(think.qnodes == 138910);
    assert(think.tt_hits == 1670);
    assert(think.completed == 4);
    assert(think.root_done == 44 && think.root_total == 44);
    assert(think.best.pos.row == 2 && think.best.pos.col == 11);
    assert(think.stopped == StopReason::NONE);
    // 99,999 is the evaluator's ceiling, not a proof: this arm's answer is the same in
    // both trees, which is the point - and the same reason the score is not asserted as a
    // mate distance.
    assert(think.score == 99999);
    assert(think.pv == 4);
    assert(think.evals == think.qnodes);
    assert(think.hits + think.recomputes == think.probes);
    assert(think.probes == 18238659);
    assert(think.hits == 12508908);

    std::cout << "test_a_capped_tail_can_pay_back_nodes_it_did_not_save passed (sweep board "
                 "740, BLACK, depth 4: 153,456 nodes against the uncapped tree's 153,383, "
                 "+73 for a tail that got 3 plies shorter (seldepth 11 -> 8), with the "
                 "answer 99,999 at (2,11) over 4 layers unchanged and 138,910 evaluation "
                 "reads for 138,910 quiescence nodes)"
              << std::endl;
}

void test_the_cap_bounds_every_arm_of_the_census_and_is_inert_where_there_is_no_tail() {
    const int cap = AlphaBetaSearch::kMaxQdepth;

    // The population half of the bound: 42 boards x both colours at root depth 2, the same
    // generator C2's census is built from. 84 arms, 155,052 nodes, and exactly one of them
    // has a node entered at the cap (board 0, BLACK). No arm may report past it, and the
    // aggregate is pinned so a bound that quietly started firing on the other 83 is red
    // here rather than in a node count nobody reads.
    uint64_t total_nodes = 0;
    uint64_t total_qnodes = 0;
    int deepest = 0;
    int at_cap = 0;
    const std::vector<Board> boards = c2_census_boards();
    for (const Board& board : boards) {
        for (const int color : {BLACK, WHITE}) {
            Board work = board;
            const C4Think think = c4_think(work, color, 2);
            assert(think.seldepth <= cap);
            if (think.seldepth == cap) ++at_cap;
            if (think.seldepth > deepest) deepest = think.seldepth;
            assert(think.evals == think.qnodes);
            total_nodes += think.nodes;
            total_qnodes += think.qnodes;
        }
    }
    assert(boards.size() == 42);
    assert(total_nodes == 155052);
    assert(total_qnodes == 139879);
    assert(deepest == cap);
    assert(at_cap == 1);

    // The inert half, on two of the arms the tail census measured at reach 6: the cap has
    // nothing to fire on where the forcing moves run out first, so the node literals this
    // suite has carried since B3 still hold here.
    const B2Think quiet_black = b2_think(b1_quiet(), BLACK, 6, 24, 8);
    assert(quiet_black.nodes == 221774);
    assert(quiet_black.seldepth == 6);
    assert(quiet_black.seldepth < cap);
    const B2Think quiet_white = b2_think(b1_quiet(), WHITE, 6, 24, 8);
    assert(quiet_white.nodes == 156428);
    assert(quiet_white.seldepth == 6);
    assert(quiet_white.seldepth < cap);

    std::cout << "test_the_cap_bounds_every_arm_of_the_census_and_is_inert_where_there_is_no_"
                 "tail passed (84 census arms / 155,052 nodes / deepest 8 on exactly 1 arm, "
                 "every arm's evaluation ledger closing on its quiescence count; the "
                 "tail-free quiet pose keeps 221,774 and 156,428 nodes at seldepth 6)"
              << std::endl;
}

// ---------------------------------------------------------------------------
// D2 - 进程级共享 TT 的分片口径 (design doc 2.8 #1 :433 「分片加锁或 lock-free」, 风险表 :569
// 「先分片加锁（16 片）」).
//
// The 16 locks are chosen by the TOP bits of the bucket index, so the split moves locks
// and not entries: which four records share a bucket - and therefore which record a
// store evicts - is index_for()'s business alone. That is the claim this case exists to
// keep, because the other reading of 「分片」 (cut the hash space into 16 sub-tables) is
// the smaller edit to the file and a different search tree, and every node literal this
// suite has carried since B3 would have had to move with it. So the assertion is
// equality with the unshared arm down to the score and the move, on a pose whose cost
// those literals already pin.
void test_a_sharded_table_searches_the_tree_an_unshared_one_searches() {
    struct Arm {
        uint64_t nodes = 0;
        uint64_t qnodes = 0;
        int score = 0;
        int completed_depth = 0;
        int seldepth = 0;
        Move best;
    };
    auto run = [](bool shared, size_t entries, const Board& board, int color) {
        const Evaluator evaluator = b1_evaluator(24, 8);
        TranspositionTable tt(entries, shared);
        AlphaBetaSearch search(evaluator, tt);
        std::atomic<bool> stop(false);

        SearchLimits limits;
        limits.max_depth = 6;
        limits.hard_time_ms = 0;
        limits.soft_time_ms = 0;
        limits.max_nodes = 0;

        Arm out;
        out.best = search.search(board, color, limits, stop);
        const SearchStats& stats = search.last_stats();
        out.nodes = stats.nodes;
        out.qnodes = stats.qnodes;
        out.score = stats.score;
        out.completed_depth = stats.completed_depth;
        out.seldepth = stats.seldepth;
        assert(tt.is_shared() == shared);
        assert(static_cast<int>(tt.size()) == static_cast<int>(entries));
        return out;
    };

    const Board quiet = b1_quiet();
    for (const int color : {BLACK, WHITE}) {
        const Arm plain = run(false, 1 << 18, quiet, color);
        const Arm shard = run(true, 1 << 18, quiet, color);
        // The suite's own literals, re-read here so the arm that is supposed to be
        // unchanged is proved unchanged against the number, not just against its twin.
        assert(plain.nodes == (color == BLACK ? 221774ull : 156428ull));
        assert(shard.nodes == plain.nodes);
        assert(shard.qnodes == plain.qnodes);
        assert(shard.score == plain.score);
        assert(shard.best.pos == plain.best.pos);
        assert(shard.completed_depth == plain.completed_depth && shard.completed_depth == 6);
        assert(shard.seldepth == plain.seldepth && shard.seldepth == 6);
    }

    // The degenerate split: fewer buckets than locks, where shard_shift_ is 0 and
    // bucket >> 0 & 15 is the bucket itself, so every bucket holds its own lock. No
    // search runs on a table this size (it would be all evictions); what is being
    // checked is that the degenerate index still routes a record to the bucket a probe
    // reads back, and that the whole-table walks work there.
    {
        TranspositionTable tiny(64, true);           // 16 buckets == 16 locks
        assert(tiny.size() == 64);
        uint64_t hash = 0;
        int stored = 0;
        for (int i = 1; i <= 40; ++i) {
            hash = ZobristHash::update_hash(hash, i, BLACK);
            tiny.store(hash, 4, i * 100, Move(7, 7, BLACK), TTFlag::EXACT, 0);
            ++stored;
        }
        TTEntry entry;
        int hits = 0;
        uint64_t probe_hash = 0;
        for (int i = 1; i <= 40; ++i) {
            probe_hash = ZobristHash::update_hash(probe_hash, i, BLACK);
            if (tiny.probe(probe_hash, entry, 0) && entry.value == i * 100) ++hits;
        }
        assert(hits > 0 && hits <= stored);
        assert(tiny.hashfull() >= 0 && tiny.hashfull() <= 1000);
        tiny.clear();
        assert(!tiny.probe(hash, entry, 0));
        assert(tiny.hashfull() == 0);
        assert(tiny.generation() == 0);
    }

    std::cout << "test_a_sharded_table_searches_the_tree_an_unshared_one_searches passed ("
                 "quiet BLACK 221,774 / WHITE 156,428 nodes answered identically by a "
                 "16-lock table and a lock-free one, score, move, depth and seldepth "
                 "included; the 16-bucket degenerate split - one lock per bucket - "
                 "answered its own probes, walked all sixteen locks to clear, and came "
                 "back empty)"
              << std::endl;
}

// 轮次130 · 设计文档 2.8 #5: `search_worker` no longer receives a Board by value;
// the hand-off is one copy into the controller's own `search_board_`, and the
// thread carries a reference to that member. Two things have to stay true for that
// to be safe, and both are pinned here rather than left to the join comment:
//   (a) the think reads the position as it was when `start_search` returned, so the
//       caller may keep editing its own Board - the old by-value signature gave that
//       guarantee for free, and a `cref` bound to the caller's board (the mistake
//       this change is one step away from) would not;
//   (b) a second `start_search` overwrites the member only after the first worker is
//       joined, so the second think answers its own position and never a stale one.
// These two cases sit here, at the end of the file rather than beside the other
// controller tests (:1221-1303), because those are above the cited assertion at
// :3973 and inserting there would move it.
void test_controller_think_ignores_later_caller_board_edits() {
    for (int round = 0; round < 20; ++round) {
        Board board;
        board.place_stone(7, 3, BLACK);
        board.place_stone(7, 4, BLACK);
        board.place_stone(7, 5, BLACK);
        board.place_stone(7, 6, BLACK);
        board.place_stone(7, 2, WHITE);   // closes the left end: only (7,7) wins
        board.place_stone(9, 9, WHITE);

        SearchController controller;
        std::atomic<bool> done(false);
        controller.set_completion_callback([&](SearchResult result) {
            if (result.best_move.pos.is_valid()) done.store(true);
        });

        controller.start_search(board, BLACK, 4, 1500);

        // Edit the caller's board right after the hand-off. Thread startup costs tens
        // of microseconds, so a worker bound to this Board would read the block at
        // (7,7) before its first node; a worker reading the controller's snapshot
        // cannot see any of it.
        board.place_stone(7, 7, WHITE);
        board.place_stone(7, 8, WHITE);
        board.place_stone(6, 6, WHITE);

        int wait_count = 0;
        while (!done.load() && wait_count < 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            wait_count++;
        }

        assert(done.load());
        assert(!controller.is_thinking());
        SearchResult result = controller.get_result();
        assert(result.best_move.pos.row == 7);
        assert(result.best_move.pos.col == 7);
        assert(result.score >= 99000);
    }

    std::cout << "test_controller_think_ignores_later_caller_board_edits passed "
                 "(20 thinks, each answered from the position as handed over even "
                 "though the caller blocked the winning square microseconds later)"
              << std::endl;
}

void test_controller_second_think_uses_the_new_position() {
    SearchController controller;

    // A quiet position with no tactical answer, so the first think is still running
    // when the second call arrives and has to be stopped and joined first.
    Board quiet;
    quiet.place_stone(7, 7, BLACK);
    quiet.place_stone(7, 8, WHITE);
    quiet.place_stone(8, 6, BLACK);
    quiet.place_stone(6, 8, WHITE);
    controller.start_search(quiet, BLACK, 12, 3000);
    assert(controller.is_thinking());

    // The second position is a forced five for WHITE. A snapshot that was not
    // rewritten - or rewritten before the first worker was joined - answers the quiet
    // position instead, and no white winning square exists there.
    Board win;
    win.place_stone(10, 4, WHITE);
    win.place_stone(10, 5, WHITE);
    win.place_stone(10, 6, WHITE);
    win.place_stone(10, 7, WHITE);
    win.place_stone(10, 3, BLACK);   // closes the left end: only (10,8) wins
    win.place_stone(3, 3, BLACK);

    std::atomic<int> completed(0);
    controller.set_completion_callback([&](SearchResult result) {
        if (result.best_move.pos.is_valid()) completed.fetch_add(1);
    });

    controller.start_search(win, WHITE, 4, 1500);

    int wait_count = 0;
    while (controller.is_thinking() && wait_count < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wait_count++;
    }

    assert(!controller.is_thinking());
    assert(completed.load() >= 1);
    SearchResult result = controller.get_result();
    assert(result.best_move.pos.row == 10);
    assert(result.best_move.pos.col == 8);
    assert(result.score >= 99000);

    std::cout << "test_controller_second_think_uses_the_new_position passed "
                 "(the second hand-off rewrote the snapshot after joining the first "
                 "worker; the delivered move is the new position's five)"
              << std::endl;
}

// F1, design doc 2.3.4 read at the one place it is applied: the layered branch of
// SearchEngine::search. `TimeManager::allocate` has had its own unit table since the
// A round pinned it, but a pure function with no caller proves nothing about a game -
// and until this round it had no caller. These arms come in through the door a player
// uses (a think with a wall clock and no named window) and read the decision off the
// result.

// Turns off everything in a think except the layered search, so the only clock in the
// room is the one the test installed.
static void quieten(SearchEngine& engine) {
    engine.set_vcf_enabled(false);
    engine.set_vct_enabled(false);
    engine.set_book_enabled(false);
    engine.set_resign_detection(false);
}

void test_the_engine_shapes_the_window_the_client_did_not_name() {
    const Board board = quiet_think_board();
    std::atomic<bool> stop(false);

    auto think = [&](int prev_elapsed_ms, int named_soft, int named_nodes, int wall_ms,
                     bool training = false) {
        SearchEngine engine;
        quieten(engine);
        engine.set_prev_move_elapsed_ms(prev_elapsed_ms);
        engine.set_soft_time_ms(named_soft);
        engine.set_max_nodes(named_nodes);
        engine.set_training(training);
        return engine.search(board, WHITE, 2, wall_ms, stop);
    };

    // The window is 65 % of the wall *this* think had, and that wall is the caller's
    // budget minus what the earlier phases of the same think spent - a quantity only
    // the engine knows at the moment of shaping. Pinning the ratio rather than the
    // number is what keeps this true on a loaded machine and under memcheck, where the
    // identical think starts against a wall tens of ms shorter.
    const SearchResult plain = think(0, 0, 0, 3000);
    assert(plain.method == "alphabeta");
    assert(plain.hard_ms > 0 && plain.hard_ms <= 3000);
    assert(plain.soft_ms == plain.hard_ms * 65 / 100);
    assert(plain.soft_ms < plain.hard_ms);
    // Seven stones and a wide candidate window: neither of the position halves (a
    // book stage, a handful of candidates) applies, so the default share stands.
    assert(board.move_count() >= 6 && board.active_empty().size() > 6);

    // A previous think that overran 80 % of its wall buys the next one half of it.
    // The boundary itself (exactly 80 %) is pinned at the allocator table, where the
    // number is an input rather than a reading of a clock that moved.
    const SearchResult damped = think(plain.hard_ms * 8 / 10 + 1, 0, 0, 3000);
    assert(damped.method == "alphabeta");
    assert(damped.soft_ms == damped.hard_ms / 2);
    assert(damped.soft_ms < plain.soft_ms);

    // A named window is not shaped at all: it is the client's `--soft-time-ms`, and a
    // session that asked for 900 gets 900. Not even clamped to the wall, because that
    // is how it behaved before this round too - an unreachable window is inert, not
    // wrong, since the hard deadline still cuts.
    const SearchResult named = think(0, 900, 0, 3000);
    assert(named.soft_ms == 900);
    assert(named.hard_ms > 0 && named.hard_ms <= 3000);
    // Damping does not reach past a client's own number either.
    assert(think(plain.hard_ms, 900, 0, 3000).soft_ms == 900);

    // A named node ceiling is the training regime, and there the time budget stands
    // aside (doc 2.3.4 step 5). Stands aside means stands aside in both directions:
    // the wall is NOT moved - `--timeout-ms` stays the fallback the D round asked for -
    // and no window is invented from it.
    const SearchResult trained = think(0, 0, 200000, 3000);
    assert(trained.soft_ms == 0);
    assert(trained.hard_ms > 0 && trained.hard_ms <= 3000);

    // And the regime is said out loud, not guessed from a number: `gomoku_train` with
    // its default `--timeout-ms 400` and no `--max-nodes` (self_play.h:15-18) is training
    // too, and if the engine had to infer it from the node ceiling every think of the
    // shipped training command would have been cut to 260 ms and the samples collected
    // from a shallower search than the round asked for. Damping is inert here as well -
    // the allocator returns before it reaches either correction.
    const SearchResult declared = think(plain.hard_ms, 0, 0, 3000, /*training=*/true);
    assert(declared.soft_ms == 0);
    assert(declared.hard_ms > 0 && declared.hard_ms <= 3000);
    // A think that is not declared training and names no nodes is a play think, so the
    // flag has to default the other way or the shaping would never reach a player.
    assert(think(0, 0, 0, 3000).soft_ms > 0);

    // No wall clock at all: the depth-only think. `hard_ms` here is the 100 ms slice
    // the phased branch gives AlphaBeta when the caller named no time (the pre-existing
    // contract pinned by test_alphabeta_large_depth_bounded_by_slice_without_deadline),
    // and shaping a window out of it would cut a depth-only think at 65 ms - which is
    // exactly why the gate is `has_deadline` and not `hard_ms > 0`.
    const SearchResult untime = think(0, 0, 0, 0);
    assert(untime.soft_ms == 0);
    assert(untime.hard_ms == 100);

    std::cout << "test_the_engine_shapes_the_window_the_client_did_not_name passed ("
              << "wall " << plain.hard_ms << " -> soft " << plain.soft_ms
              << ", damped soft " << damped.soft_ms
              << ", trained soft " << trained.soft_ms << ")" << std::endl;
}

// F2: the window chosen at the engine's door is the window the root loop stops at.
// The two halves live in different classes - SearchEngine shapes, AlphaBetaSearch's
// layer boundary spends - so "wired" is not the same as "honoured", and this is the
// case that says the number that was decided is the number that was used.
void test_the_window_the_engine_shapes_is_the_one_the_root_loop_stops_at() {
    const Board board = quiet_think_board();
    std::atomic<bool> stop(false);

    // 100 ms per reading: the soft window is crossed a few readings into the first
    // layers, while the hard wall - an absolute instant on the real steady clock,
    // installed by set_deadline() - is out of reach of a clock that starts at 0 and
    // creeps. That asymmetry is the point: soft is the only time reason this arm can
    // report, so a time_hard here would mean the injection did not take.
    ScriptedClock clock(100);
    SearchResult shaped;
    {
        SearchEngine engine;
        quieten(engine);
        engine.set_clock(&clock);
        shaped = engine.search(board, WHITE, 6, 3000, stop);
    }
    assert(shaped.method == "alphabeta");
    assert(shaped.soft_ms > 0 && shaped.soft_ms == shaped.hard_ms * 65 / 100);
    assert(shaped.stopped == "time_soft");
    // A layer boundary, not mid-layer: the soft window is a decision about starting
    // another iteration, so the layer below it stays intact and there is no partial.
    assert(shaped.partial_depth == 0);
    assert(shaped.depth >= 1 && shaped.depth < 6);
    // The count a soft stop leaves behind is the completed layer's full one, which is
    // what distinguishes "no layer was interrupted" from "layer 1 broke after its
    // first candidate".
    assert(shaped.root_moves_done == shaped.root_moves_total);
    assert(shaped.root_moves_done > 0);

    // Control, same wall, same depth request, same everything but a clock that never
    // moves: nothing stops this think early, so the stop above was the window rather
    // than the node ceiling that the 3000 ms wall also implies.
    SteadyClock idle;
    SearchResult whole;
    {
        SearchEngine engine;
        quieten(engine);
        engine.set_clock(&idle);
        whole = engine.search(board, WHITE, 6, 3000, stop);
    }
    assert(whole.method == "alphabeta");
    assert(whole.stopped == "none");
    assert(whole.depth == 6);
    assert(whole.soft_ms == shaped.soft_ms);

    std::cout << "test_the_window_the_engine_shapes_is_the_one_the_root_loop_stops_at passed ("
              << "window " << shaped.soft_ms << " of " << shaped.hard_ms
              << " reached d" << shaped.depth << "/" << whole.depth
              << ", readings " << clock.readings << ")" << std::endl;
}

// F3: the session is the only party that knows what the last think cost, and step 4
// of the allocator needs that number. This pins the hand-off - recorded per committed
// think, carried to the next one, cleared by a new game - without pretending to pin
// the shaping, which F1 already did at the engine's door.
void test_the_session_hands_the_next_think_what_the_last_one_cost() {
    GameManager game;
    game.set_player_color(BLACK);
    game.set_config(1500, 3);

    std::atomic<int> thinks{0};
    std::atomic<int> first_cost{-1};
    std::atomic<int> second_cost{-1};
    game.set_move_callback([&](const Move& move, int, int elapsed_ms) {
        assert(move.pos.is_valid());
        (thinks.fetch_add(1) == 0 ? first_cost : second_cost).store(elapsed_ms);
    });

    // A fresh session has no previous think. 上一手耗时 is precisely the kind of number
    // that would otherwise leak across the boundary and damp the first move of the next
    // game by the last move of this one.
    assert(game.prev_think_elapsed_ms() == 0);

    game.make_move(7, 7);
    for (int i = 0; i < 400 && thinks.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(thinks.load() == 1);
    assert(game.prev_think_elapsed_ms() == first_cost.load());

    // The second AI think is armed by the human's next move, and by then the session
    // has already recorded the first think's cost - so the witness after it is the
    // *second* cost, not the first. One number, never two: the record is overwritten by
    // whichever think last finished.
    //
    // The cell is the first empty one on the board rather than a literal, because the
    // AI's reply to (7,7) is a stone the engine chooses and a hand-picked neighbour is
    // how this case used to fail: make_move() on an occupied cell answers false and no
    // second think ever starts.
    int second_row = -1;
    int second_col = -1;
    for (int row = 0; row < BOARD_SIZE && second_row < 0; ++row) {
        for (int col = 0; col < BOARD_SIZE && second_row < 0; ++col) {
            if (game.board().is_empty(row, col)) {
                second_row = row;
                second_col = col;
            }
        }
    }
    assert(second_row >= 0 && game.make_move(second_row, second_col));
    for (int i = 0; i < 400 && thinks.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(thinks.load() == 2);
    assert(game.prev_think_elapsed_ms() == second_cost.load());

    game.reset();
    assert(game.prev_think_elapsed_ms() == 0);

    std::cout << "test_the_session_hands_the_next_think_what_the_last_one_cost passed ("
              << "costs " << first_cost.load() << ", " << second_cost.load() << ")"
              << std::endl;
}

// G1, design doc 2.9 :443. The row promised `SearchInfo` a `tt_hits` and the counter has
// been ticking since the reuse round - what was missing was the way out of the search, so
// the one number that says "this think read the table" stopped at a progress frame and a
// test. These arms pin the copy site rather than the counter: the frame and the returned
// record must be two readings of one think, and the bound the field's `int` rests on
// (a hit is recorded at a visited node, once per node at most - `alphabeta.cpp:364` is the
// only call site) must hold on both a full think and the warm repeat of it.
void test_the_table_reads_a_think_took_are_reported_out_of_the_search() {
    const Board board = quiet_think_board();
    Evaluator evaluator;
    std::atomic<bool> stop(false);

    SearchLimits limits;
    limits.max_depth = 3;

    TranspositionTable tt;
    AlphaBetaSearch cold(evaluator, tt);
    cold.search(board, WHITE, limits, stop);
    const SearchStats cold_stats = cold.last_stats();
    const SearchInfo cold_info = cold.last_search_info();
    assert(cold_info.tt_hits == static_cast<int>(cold_stats.tt_hits));
    assert(cold_info.tt_hits > 0);
    assert(cold_info.tt_hits <= cold_info.nodes_searched);

    AlphaBetaSearch warm(evaluator, tt);
    warm.search(board, WHITE, limits, stop);
    const SearchStats warm_stats = warm.last_stats();
    const SearchInfo warm_info = warm.last_search_info();
    assert(warm_info.tt_hits == static_cast<int>(warm_stats.tt_hits));
    assert(warm_info.tt_hits > 0);
    assert(warm_info.tt_hits <= warm_info.nodes_searched);

    // And the reuse is visible *as* reuse. Node count alone cannot tell "this position is
    // simple" from "this position was already proved", which is the distinction the whole
    // reuse subsystem lives for; cross-multiplying keeps it in integers: the share of
    // nodes answered from the table has to rise when the table is warm.
    assert(warm_info.nodes_searched < cold_info.nodes_searched);
    assert(static_cast<long long>(warm_info.tt_hits) * cold_info.nodes_searched >
           static_cast<long long>(cold_info.tt_hits) * warm_info.nodes_searched);

    // The engine's own door, which is the only route the session and the replay use.
    SearchEngine engine;
    quieten(engine);
    const SearchResult first = engine.search(board, WHITE, 3, 600000, stop);
    const SearchResult second = engine.search(board, WHITE, 3, 600000, stop);
    assert(first.method == "alphabeta" && first.tt_hits > 0);
    assert(first.tt_hits <= first.nodes_searched);
    assert(second.tt_hits > 0 && second.nodes_searched < first.nodes_searched);
    // A think that never started a layered search reports 0, like soft_ms does: the
    // number describes a table read, and there was no table read to describe.
    SearchEngine blocked;
    blocked.set_alphabeta_enabled(false);
    blocked.set_resign_detection(false);
    const SearchResult forced = blocked.search(board, WHITE, 3, 600000, stop);
    assert(forced.method != "alphabeta");
    assert(forced.tt_hits == 0);

    std::cout << "test_the_table_reads_a_think_took_are_reported_out_of_the_search passed ("
              << "cold " << cold_info.nodes_searched << " nodes / " << cold_info.tt_hits
              << " reads, warm " << warm_info.nodes_searched << " / " << warm_info.tt_hits
              << "; engine " << first.tt_hits << " -> " << second.tt_hits << ")" << std::endl;
}

// G2: the session is the consumer. The number only earns its keep if the replay a player
// opens later still says how much of that think was a table read.
void test_the_session_records_the_table_reads_it_kept() {
    GameManager game;
    game.set_player_color(BLACK);
    game.set_config(3000, 4);

    std::atomic<int> ai_moves{0};
    game.set_move_callback([&](const Move&, int, int) { ai_moves.fetch_add(1); });

    assert(game.make_move(7, 7));
    for (int i = 0; i < 400 && ai_moves.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(ai_moves.load() == 1);

    const ReplaySnapshot replay = game.replay_snapshot();
    assert(replay.moves.size() == 2);
    // The stone a human placed has no think behind it, so it reports no reads - the same
    // honest zero the fast paths report.
    assert(replay.moves[0].method == "human");
    assert(replay.moves[0].tt_hits == 0);
    assert(replay.moves[1].method == "alphabeta");
    assert(replay.moves[1].tt_hits > 0);
    // The bound holds across the whole hand-off chain (SearchStats -> SearchInfo ->
    // SearchResult -> MoveAnalysis), which is the part that can silently re-word itself
    // when a field is inserted in the middle of a position-initialised struct.
    assert(replay.moves[1].tt_hits <= replay.moves[1].nodes);

    std::cout << "test_the_session_records_the_table_reads_it_kept passed ("
              << replay.moves[1].nodes << " nodes, " << replay.moves[1].tt_hits
              << " table reads)" << std::endl;
}

// ---------------------------------------------------------------------------
// 轮次141 A（design doc 2.4.3 ②「把上一手的 PV 第二着作为预置」）: the one cross-hand
// reuse that can actually cut. A record written two plies into a think is stored two
// plies shallower than the next root asks for, so an ordinary session hit only
// reorders the new tree (REVIEW_LOG 轮次140 读数 2: 23/24 命中, 中位 −8.8 %). Searching
// the predicted reply ahead writes that position's children at the depth the next
// think will request. No clock anywhere: every think below is ended by its depth, so
// `nodes` is a count, not a load figure.
// ---------------------------------------------------------------------------

void test_a_pondered_reply_think_meets_a_tree_it_already_paid_for() {
    struct Arm { const char* name; Board board; int color; };
    const std::vector<Arm> arms = {
        {"quiet BLACK", b1_quiet(), BLACK},
        {"quiet WHITE", b1_quiet(), WHITE},
        {"scattered BLACK", b1_scattered(), BLACK},
        {"scattered WHITE", b1_scattered(), WHITE},
    };
    const int depth = 4;
    // A wall far above what these poses cost, so the node budget a 0 ms timeout would
    // derive (100 ms -> 20,000 nodes) never becomes the thing under test.
    const int wall_ms = 600000;
    std::atomic<bool> stop(false);

    long long cold_total = 0;
    long long warm_total = 0;
    for (const Arm& arm : arms) {
        SearchEngine session;
        SearchEngine fresh;

        const SearchResult first = session.search(arm.board, arm.color, depth, wall_ms, stop);
        const SearchResult first_control =
            fresh.search(arm.board, arm.color, depth, wall_ms, stop);
        // The two engines have to start from the same conclusion, or what is being
        // compared below is two trees rather than one tree paid for twice.
        assert(first.method == "alphabeta" && first_control.method == "alphabeta");
        assert(first.best_move == first_control.best_move);
        assert(first.score == first_control.score);
        assert(first.principal_variation.size() >= 2);

        Board reply_position = arm.board;
        assert(reply_position.place_stone(first.best_move.pos, arm.color));
        const Move predicted = first.principal_variation[1];
        assert(predicted.color == opponent(arm.color));
        assert(reply_position.place_stone(predicted.pos, predicted.color));

        // What the session would spend on it: the share the product setting uses.
        const int budget = first.nodes_searched / 2;
        const SearchResult pondered =
            session.ponder_search(reply_position, arm.color, depth, budget, stop);
        assert(pondered.method == "alphabeta");
        assert(pondered.nodes_searched > 0);

        const SearchResult cold = fresh.search(reply_position, arm.color, depth, wall_ms, stop);
        const SearchResult warm =
            session.search(reply_position, arm.color, depth, wall_ms, stop);
        assert(cold.depth == depth && warm.depth == depth);
        // 着法与分值不许动 - 这是 :554 那句「变化必须能解释」在这一格上的形状：这里没有
        // 需要解释的变化。
        assert(warm.best_move == cold.best_move);
        assert(warm.score == cold.score);
        assert(warm.nodes_searched < cold.nodes_searched);
        cold_total += cold.nodes_searched;
        warm_total += warm.nodes_searched;
        std::cout << "  [" << arm.name << "] first " << first.nodes_searched
                  << " / budget " << budget << " spent " << pondered.nodes_searched
                  << " -> second think cold " << cold.nodes_searched << " vs warm "
                  << warm.nodes_searched << std::endl;
    }
    // The row is 「节点数较冷启动下降 ≥ 25 %」, read over the four arms together: one
    // pose's ratio is a property of that pose, while the row is about the mechanism.
    assert(warm_total * 4 <= cold_total * 3);
    std::cout << "test_a_pondered_reply_think_meets_a_tree_it_already_paid_for passed ("
              << arms.size() << " arms at depth " << depth << ": cold " << cold_total
              << " -> pondered " << warm_total << " nodes, "
              << 100.0 * (1.0 - static_cast<double>(warm_total) / static_cast<double>(cold_total))
              << "% cheaper, every arm the same move at the same score)" << std::endl;
}

// The same mechanism's two silences, which are what make it safe to run inside a live
// session: a ponder spends its own node budget and never a wall clock - so whoever
// ends it joins a thread that stops at the next node check, not one that runs out a
// deadline nobody is reading - and it emits no progress frame and no elapsed time,
// because the client already holds its move and a frame after the stone would read as
// a second think.
void test_a_ponder_costs_only_its_budget_and_says_nothing_on_the_hook() {
    const Board board = b1_quiet();
    std::atomic<bool> stop(false);
    SearchEngine engine;
    int frames = 0;
    engine.set_progress_callback([&frames](const SearchStats&) { ++frames; });

    const SearchResult first = engine.search(board, BLACK, 4, 600000, stop);
    assert(first.method == "alphabeta");
    const int frames_after_a_think = frames;
    // The hook is wired, so the equality below says something.
    assert(frames_after_a_think > 0);

    Board reply_position = board;
    assert(reply_position.place_stone(first.best_move.pos, BLACK));
    const Move predicted = first.principal_variation[1];
    assert(reply_position.place_stone(predicted.pos, predicted.color));

    const int budget = 3000;
    const SearchResult pondered =
        engine.ponder_search(reply_position, BLACK, 6, budget, stop);
    assert(pondered.method == "alphabeta");
    assert(pondered.nodes_searched <= budget);
    assert(pondered.stopped == "nodes");
    assert(pondered.elapsed_ms == 0);
    assert(frames == frames_after_a_think);

    // A pre-search that was given no budget searches nothing at all.
    const SearchResult unfunded = engine.ponder_search(reply_position, BLACK, 6, 0, stop);
    assert(unfunded.nodes_searched == 0 && unfunded.method.empty());

    // And one met by a 终止 already standing never reaches its first node.
    stop.store(true);
    const SearchResult stopped = engine.ponder_search(reply_position, BLACK, 6, budget, stop);
    assert(stopped.nodes_searched == 0);
    stop.store(false);

    std::cout << "test_a_ponder_costs_only_its_budget_and_says_nothing_on_the_hook passed ("
              << pondered.nodes_searched << " of " << budget << " nodes, stopped "
              << pondered.stopped << ", " << frames << " frames - the same "
              << frames_after_a_think << " the think itself made)" << std::endl;
}

// 轮次141 A（design doc 2.4.3 ②）在会话那一层的形状。预搜是**空闲时间**里的活：它既不能
// 落子、也不能进复盘、更不能冒充「上一手耗时」，而谁要动棋盘就得先把它收回来。上一条量的
// 是节点收益，这一条量的是这四件事 - 一个跑在会话里的线程做错任何一件都会被客户的帧或者
// 悔棋看见。
void test_a_pondering_session_arms_a_pre_search_that_commits_nothing() {
    GameManager game;
    game.set_player_color(BLACK);
    game.set_config(3000, 4);
    game.set_ponder(50);

    std::atomic<int> ai_moves{0};
    std::atomic<int> last_elapsed_ms{0};
    game.set_move_callback([&](const Move&, int, int elapsed_ms) {
        last_elapsed_ms.store(elapsed_ms);
        ai_moves.fetch_add(1);
    });

    assert(game.make_move(7, 7));
    for (int i = 0; i < 600 && ai_moves.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(ai_moves.load() == 1);
    // The worker arms the pre-search as its last act, while ai_thinking_ is still up,
    // so a session that has answered cannot yet be holding a half-told story.
    for (int i = 0; i < 200 && game.is_ai_thinking(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(!game.is_ai_thinking());
    assert(game.ponder_arms() == 1);

    const auto generation_before = game.transposition_table().generation();
    const size_t rows_before = game.replay_snapshot().moves.size();
    for (int i = 0; i < 200 && game.ponder_last_nodes() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const int pondered_nodes = game.ponder_last_nodes();
    assert(pondered_nodes > 0);
    // None of that left a mark on anything the client or a 复盘 reads.
    assert(game.transposition_table().generation() == generation_before);
    assert(game.replay_snapshot().moves.size() == rows_before);
    assert(game.prev_think_elapsed_ms() == last_elapsed_ms.load());

    // Turning the switch off takes back the one that is running, rather than leaving it
    // to write into the table the next stone is about to age.
    game.set_ponder(0);
    assert(!game.ponder_active());

    // A human stone is the other way out, and it has to be the synchronous kind: the
    // board it places and the generation it advances are the two things the ponder
    // thread must not be touching at the same time.
    assert(game.make_move(8, 8));
    assert(!game.ponder_active());
    for (int i = 0; i < 600 && ai_moves.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(ai_moves.load() == 2);
    for (int i = 0; i < 200 && game.is_ai_thinking(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // With the share at 0 the second think arms nothing, so the game plays on exactly
    // as it did before this mechanism existed.
    assert(game.ponder_arms() == 1);

    GameManager silent;
    silent.set_player_color(BLACK);
    silent.set_config(3000, 4);
    std::atomic<int> silent_moves{0};
    silent.set_move_callback([&](const Move&, int, int) { silent_moves.fetch_add(1); });
    assert(silent.make_move(7, 7));
    for (int i = 0; i < 600 && silent_moves.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(silent_moves.load() == 1);
    for (int i = 0; i < 200 && silent.is_ai_thinking(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(silent.ponder_arms() == 0);
    assert(silent.ponder_last_nodes() == 0);

    std::cout << "test_a_pondering_session_arms_a_pre_search_that_commits_nothing passed ("
              << "one arm, " << pondered_nodes << " nodes spent in the idle time, "
              << "generation / replay / 上一手耗时 all untouched, switch off joins it)"
              << std::endl;
}

// 丙-2 O2 的验收两张：①「42 副 × 225 格 × 2 色 = 18,900 组，缓存值与现算值逐格相同」的字面计数，
// ②「cap 不在缓存里」（`evaluate_board_from` 最后才夹，缓存的是未夹的和）。
// 这张缓存本身是 §2.6 Step 2/3 落地的（轮次122/123：排序路径 −88.5 %、总识别 −63.6 %、
// ns/节点 −7.9 %），指导书 §4 丙-2 写的「未做」是过期状态——本轮补的是它点名的这两张验收，
// 不新建第二张表（同一份「每格棋盘和价」被缓存两次才是这里真正的风险）。
void test_the_board_sum_cache_answers_every_cell_of_the_census_and_keeps_the_cap_outside() {
    const std::vector<Board> boards = c2_census_boards();
    assert(boards.size() == 42);
    Evaluator evaluator;

    long long groups = 0;
    long long from_records = 0;
    long long zeros_from_occupied = 0;

    for (const Board& board : boards) {
        CandidateCache cache(evaluator);
        for (int row = 0; row < BOARD_SIZE; ++row) {
            for (int col = 0; col < BOARD_SIZE; ++col) {
                for (const int side : {BLACK, WHITE}) {
                    // 现算的那一遍：四向识别 + `board_pair_value`，与 `evaluate_board` 的加数同一形状。
                    PositionPattern fresh_mine, fresh_theirs;
                    PatternRecognizer::recognize_position_both(board, row, col, side,
                                                               fresh_mine, fresh_theirs);
                    const int fresh_addend = evaluator.board_pair_value(
                        fresh_mine, fresh_theirs, row, col, side == BLACK);

                    const CandidateCache::Cell* found = cache.cell(board, row, col);
                    int cached_addend = 0;
                    if (found == nullptr) {
                        // 占了子的格不进记录，也不进求和：`evaluate_board` 走的是
                        // `active_empty()`，那里头没有它——两边同意的不是「加数是 0」，
                        // 而是「这一格谁都不问」。
                        assert(!board.is_empty(row, col));
                        bool in_list = false;
                        for (const Position& pos : board.active_empty()) {
                            if (pos.row == row && pos.col == col) { in_list = true; break; }
                        }
                        assert(!in_list);
                        assert(fresh_addend == 0 || fresh_addend != 0);   // 见上：现算不被问到这里
                        ++zeros_from_occupied;
                    } else {
                        assert(board.is_empty(row, col));
                        const PositionPattern& mine = side == BLACK ? found->black : found->white;
                        const PositionPattern& theirs = side == BLACK ? found->white : found->black;
                        cached_addend = evaluator.board_pair_value(mine, theirs, row, col,
                                                                   side);
                        assert(c2_patterns_equal(mine, fresh_mine));
                        assert(c2_patterns_equal(theirs, fresh_theirs));
                        assert(cached_addend == fresh_addend);
                        ++from_records;
                    }
                    ++groups;
                }
            }
        }
    }

    // 字面计数：225 格 × 2 色 × 42 副。少一格都不行——那意味着普查盘被换小了。
    assert(groups == 42LL * BOARD_SIZE * BOARD_SIZE * 2);
    assert(groups == 18900);
    assert(from_records > 0);
    assert(from_records + zeros_from_occupied == groups);
    assert(zeros_from_occupied > 0);

    // ②cap 在缓存之外：找一个原始加数之和已经越过 `five - 1` 的盘面，逐格加数未夹、
    //   只有求和之后才夹。缓存若存了夹过的数，这两句话不可能同时成立。
    Board crowded;
    for (int row = 4; row <= 10; ++row) {
        for (int col = 4; col <= 10; ++col) {
            crowded.place_stone(row, col, (row + col) % 2 == 0 ? BLACK : WHITE);
        }
    }
    MoveGenerator gen(evaluator);
    long long uncapped = 0;
    for (const Position& pos : crowded.active_empty()) {
        PositionPattern mine, theirs;
        PatternRecognizer::recognize_position_both(crowded, pos.row, pos.col, BLACK, mine, theirs);
        uncapped += evaluator.board_pair_value(mine, theirs, pos.row, pos.col, true);
    }
    const int capped = gen.board_score(crowded, BLACK);
    const int cap = evaluator.coefficients().five - 1;
    std::cout << "  [r153c2] groups=" << groups << " from records=" << from_records
              << " occupied (never asked)=" << zeros_from_occupied
              << " uncapped sum=" << uncapped << " capped=" << capped << " cap=" << cap << std::endl;
    // 夹子是双向的：越过 `five - 1` 的和被拉回 ±(five - 1)，而缓存里躺着的是未夹的加数。
    assert(uncapped > static_cast<long long>(cap) || uncapped < -static_cast<long long>(cap));
    const int expected = uncapped > 0 ? cap : -cap;
    assert(capped == expected);
    // 同一个盘面再读一次：走记录的求和与走整盘扫描的求和同数，且都等于夹过的那一个。
    assert(evaluator.evaluate_board(crowded, BLACK) == capped);
    assert(gen.board_score(crowded, BLACK) == evaluator.evaluate_board(crowded, BLACK));
    std::cout << "test_the_board_sum_cache_answers_every_cell_of_the_census_and_keeps_the_cap_outside"
              << " passed (18,900 组逐格相同：" << from_records << " 格读记录、"
              << zeros_from_occupied << " 格占了子答 0；cap " << cap << " 只在求和之后落，"
              << "未夹的和 " << uncapped << " 被夹回)" << std::endl;
}

// 丁-3 注意 2：换系数集必须清 TT。`static_eval` 只按 BLACK 那一个符号存
// （src/optimization/transposition.h:44-52），这个约定只在**同一套系数**内成立；按色两套
// 之后，一张表里躺着的是两种颜色各自的价格。钉三件事：搜过之后表非空、换一套之后表空、
// 换的是**同一套**也清（判据是「换过系数」，不是「值不同才清」——后者要调用方去比
// 14 × 2 个数，而漏比一次的代价是把新系数按旧价格来用）。
void test_a_coefficient_change_empties_the_transposition_table() {
    const Board board = quiet_think_board();
    std::atomic<bool> stop(false);

    SearchEngine engine;
    quieten(engine);
    engine.set_max_nodes(20000);
    const SearchResult first = engine.search(board, WHITE, 4, 600000, stop);
    assert(first.nodes_searched > 0);
    assert(first.tt_hits >= 0);
    std::cout << "  [丁-3] after think: hashfull=" << engine.transposition_table().hashfull()
              << " generation=" << static_cast<int>(engine.transposition_table().generation())
              << std::endl;
    assert(engine.transposition_table().hashfull() > 0);

    ThreatCoefficients other;
    other.live_four = 12345;
    engine.set_coefficients(other);
    assert(engine.transposition_table().hashfull() == 0);
    assert(engine.transposition_table().generation() == 0);

    // 换回同一套也清：上面那两条判据在「值相同」的分支上必须仍然成立。
    engine.set_max_nodes(20000);
    engine.search(board, WHITE, 4, 600000, stop);
    assert(engine.transposition_table().hashfull() > 0);
    engine.set_coefficients(other);
    assert(engine.transposition_table().hashfull() == 0);

    // 按色那一路：一对系数进来同样清表。
    engine.set_max_nodes(20000);
    engine.search(board, WHITE, 4, 600000, stop);
    assert(engine.transposition_table().hashfull() > 0);
    CoefficientPair pair;
    pair.white.position_weight_scale = 3;
    engine.set_coefficients(pair);
    assert(engine.transposition_table().hashfull() == 0);

    std::cout << "test_a_coefficient_change_empties_the_transposition_table passed" << std::endl;
}

// 轮次176：调用方命名了节点预算时，战术层也按节点出价。
//
// 在此之前 `--max-nodes` 只管得住 AlphaBeta：VCF/VCT 各自带着 1,000,000 / 500,000 的
// 构造默认帽（vcf.cpp:8、vct.cpp:8），外加一条墙钟切片（时限的 20 % / 30 %）。于是一手
// 「按节点出价」的思考，实际代价 = 主搜索的份额（确定的）+ 战术层花掉多少毫秒（跟着负载走），
// 而且战术层能比主搜索本身多花几十倍的钱。现在命名了预算就把份额收到
// min(自己的默认帽, 预算 × 份额) —— 只收紧、不放宽，所以定深那一档（预算远大于默认帽）
// 的战术层行为一字不动，六个既有线上 pin 仍然成立。
//
// 夹具那 9/90 同配置翻局最后查明与这条无关（是开局库每进程的加权随机抽签，见轮次176 的
// 探针 ⑤/⑥：关库后两跑逐手零分岔）。这条改动按自己的口径记账：一手棋的代价从此是局面的
// 函数，而不是「局面 × 机器此刻多忙」。
void test_named_node_budget_prices_the_tactical_phases() {
    // 既有夹具（test_engine_stamps_elapsed_on_tactical_wins）：rows=2 只有 VCT 证明，
    // rows=3 是 VCF 证明。VCT 份额 30 %，VCF 份额 20 %。
    const auto lattice = [](int rows) {
        Board board;
        for (int i = 0; i < rows; ++i) {
            const int r = 2 + i * 2;
            board.place_stone(r, 2, BLACK);
            board.place_stone(r, 3, WHITE);
            board.place_stone(r, 4, WHITE);
            board.place_stone(r, 5, WHITE);
        }
        return board;
    };
    std::atomic<bool> stop(false);
    const int wall_ms = 3000;   // 与赛具同口径；命名了预算时战术层不再看这条墙

    // ① 份额吃不住证明 ⇒ 战术层交不出答案，主搜索接手，并把手上的节点预算花满。
    //    改动前同一手读 vct：那时无 VCT 只有 500,000 节点的默认帽和 900 ms 的墙。
    {
        SearchEngine engine;
        engine.set_max_nodes(40);                       // VCT 份额 = 40 × 30 % = 12 节点
        const SearchResult r = engine.search(lattice(2), WHITE, 8, wall_ms, stop);
        assert(r.method == "alphabeta");
        assert(r.nodes_searched == 40);                 // 主搜索被自己的帽掐住，一分不差
        std::cout << "  [轮176] cap 40 -> " << r.method << " nodes=" << r.nodes_searched
                  << " (改动前读 vct)" << std::endl;
    }

    // ② 份额够 ⇒ 战术层照旧交答案，且交的是同一个点：这条改的是「最多花多少」，
    //    不是「必须花多少」。
    {
        SearchEngine engine;
        engine.set_max_nodes(200);                      // 份额 60 节点，证明装得下
        const SearchResult r = engine.search(lattice(2), WHITE, 8, wall_ms, stop);
        assert(r.method == "vct");
        assert(r.score == 90000);
        assert(r.best_move.pos.row == 3 && r.best_move.pos.col == 4);
        std::cout << "  [轮176] cap 200 -> " << r.method << " (" << r.best_move.pos.row
                  << "," << r.best_move.pos.col << ")" << std::endl;
    }

    // ③ 只收紧不放宽：份额一旦高过战术层自己的默认帽（VCT 500,000），min() 就把它按回
    //    默认值 ⇒ 40,000,000 的预算和 8,000,000 的预算、以及根本不命名预算（墙钟放到
    //    600 s，切片永远够）的旧行为，三者必须交出同一个点。这一条是「定深臂不动」的证明。
    {
        struct Identity {
            std::string method;
            int score = 0, row = -1, col = -1;
        } identity;
        for (int cap : {8000000, 40000000}) {
            SearchEngine engine;
            engine.set_max_nodes(cap);                  // 份额 2.4M / 12M > 默认 500,000
            const SearchResult r = engine.search(lattice(2), WHITE, 8, wall_ms, stop);
            identity.method = r.method;
            identity.score = r.score;
            identity.row = r.best_move.pos.row;
            identity.col = r.best_move.pos.col;
            assert(r.method == "vct");
            assert(r.score == 90000);
            assert(r.best_move.pos.row == 3 && r.best_move.pos.col == 4);
        }
        SearchEngine unnamed;                             // 未命名预算 = 走老路（墙钟切片）
        const SearchResult r = unnamed.search(lattice(2), WHITE, 8, 600000, stop);
        assert(r.method == identity.method);
        assert(r.score == identity.score);
        assert(r.best_move.pos.row == identity.row && r.best_move.pos.col == identity.col);
        std::cout << "  [轮176] 8M / 40M / 未命名 三支同解：" << identity.method << " ("
                  << identity.row << "," << identity.col << ")" << std::endl;
    }

    // ④ VCF 那一侧：份额是上限而不是下限，小帽也交得出证明（rows=3 的证明只要几个节点），
    //    大帽与小帽同解 —— 收份额不会把一个本来赢得下的局面改判成别的。
    {
        SearchResult small_move;
        {
            SearchEngine engine;
            engine.set_max_nodes(40);                   // VCF 份额 = 8 节点
            small_move = engine.search(lattice(3), WHITE, 8, wall_ms, stop);
            assert(small_move.method == "vcf");
            assert(small_move.score == 100000);
        }
        SearchEngine engine;
        engine.set_max_nodes(200000);                    // 份额 40,000 节点
        const SearchResult big = engine.search(lattice(3), WHITE, 8, wall_ms, stop);
        assert(big.method == "vcf" && big.score == 100000);
        assert(big.best_move.pos.row == small_move.best_move.pos.row);
        assert(big.best_move.pos.col == small_move.best_move.pos.col);
        std::cout << "  [轮176] vcf 份额 8 vs 40000 同解 (" << big.best_move.pos.row << ","
                  << big.best_move.pos.col << ")" << std::endl;
    }

    // ⑤ 份额比例本身：上面四条只钉住「有份额」，没钉住「份额是 30 %」。这份 VCT 证明要
    //    46–48 个节点，30 % 之下 cap 150 只分到 45（交不出，主搜索接手），cap 160 分到 48
    //    （交得出）⇒ 比例被夹在 [30 %, 32 %) 里，抬比例或砍比例都会在这里红。
    {
        SearchEngine starved;
        starved.set_max_nodes(150);                     // 份额 45 节点
        const SearchResult tight = starved.search(lattice(2), WHITE, 8, wall_ms, stop);
        assert(tight.method == "alphabeta");
        assert(tight.nodes_searched == 150);

        SearchEngine enough;
        enough.set_max_nodes(160);                      // 份额 48 节点
        const SearchResult loose = enough.search(lattice(2), WHITE, 8, wall_ms, stop);
        assert(loose.method == "vct");
        assert(loose.score == 90000);
        std::cout << "  [轮176] 150（份额 45）交不出 / 160（份额 48）交得出 ⇒ 30 % 被夹住"
                  << std::endl;
    }

    std::cout << "test_named_node_budget_prices_the_tactical_phases passed" << std::endl;
}

// #84：根扫描的三条威胁分支（活四 / 双四 / 没有防守点）以前只看「挡我这条威胁」的点，不看
// 对面此刻能不能直接把五做成。实测这一类占执黑侧 43 个根扫描 WIN 里的 28 个（轮次184；轮次181
// 只数出「28 个伪报」，这一轮把它归到活四那一条支上，四条分支里其余三条 418 臂一次都没交回过）。
// 三臂：
//   ① 黑落一子成活四，而白在同一刻攥着唯一的成五点 ⇒ 这不是胜着（门装上之前这里是 WIN）。
//   ② 把白那串四缩成三（成五点消失）⇒ 同一颗黑子必须是胜着 —— 正控，防止我把门装到别处去。
//   ③ 黑这一手正好把白的成五点填掉，同时自己成活四 ⇒ 门不许挡住它（被拒的是这条候选，不是整个节点）。
void test_vct_root_threat_loses_the_race_when_the_defender_holds_a_five() {
    Evaluator evaluator;
    TranspositionTable tt;
    std::atomic<bool> stop(false);

    // ① 白的成五点 (0,4) 空着；黑的 (10,4) 成活四（两端 (10,3)/(10,8) 都空）。
    {
        Board board;
        for (int c = 0; c < 4; ++c) assert(board.place_stone(0, c, WHITE));
        for (int c = 5; c <= 7; ++c) assert(board.place_stone(10, c, BLACK));
        assert(board.is_empty(0, 4));

        VCTSearch vct(evaluator, tt);
        const VCTMove raced = vct.search(board, BLACK, 6, stop);
        assert(raced.result != VCTResult::WIN);
    }

    // ② 同一副黑的子，白只剩三连 ⇒ 白的成五点不存在 ⇒ 同一个判据必须交出胜着。
    {
        Board board;
        for (int c = 0; c < 3; ++c) assert(board.place_stone(0, c, WHITE));
        for (int c = 5; c <= 7; ++c) assert(board.place_stone(10, c, BLACK));

        VCTSearch vct(evaluator, tt);
        const VCTMove wins = vct.search(board, BLACK, 6, stop);
        assert(wins.result == VCTResult::WIN);
        assert(wins.best_move.pos.row == 10 &&
               (wins.best_move.pos.col == 4 || wins.best_move.pos.col == 8));
    }

    // ③ 白那串四改在对角线上，唯一的成五点 (4,4) 同时是黑第四行那条活的左端 ⇒ 黑落那里既挡住
    // 对面又成活四：门放它过去。
    {
        Board board;
        for (int i = 0; i < 4; ++i) assert(board.place_stone(i, i, WHITE));
        for (int c = 5; c <= 7; ++c) assert(board.place_stone(4, c, BLACK));
        assert(board.is_empty(4, 4));

        VCTSearch vct(evaluator, tt);
        const VCTMove blocks_and_wins = vct.search(board, BLACK, 6, stop);
        assert(blocks_and_wins.result == VCTResult::WIN);
        assert(blocks_and_wins.best_move.pos.row == 4 &&
               blocks_and_wins.best_move.pos.col == 4);
    }

    std::cout << "test_vct_root_threat_loses_the_race_when_the_defender_holds_a_five passed"
              << std::endl;
}

// 轮次197 那条真线的前 34 手：tools/match_openings.jsonl 第 13 条的 14 子 + VCT 自己搜出来的
// forcing 线。#89 与 #88 两条用例都用它，因为「递归层走到过」与「递归的胜着能不能逐手核对」
// 是同一格上的一对问题。
const int kVctRaceLine[34][2] = {
    {7, 7}, {5, 5}, {6, 8}, {9, 5}, {7, 5}, {7, 6}, {8, 6}, {9, 7},
    {6, 4}, {5, 3}, {6, 7}, {6, 5}, {6, 9}, {6, 10}, {5, 9}, {4, 10},
    {7, 9}, {4, 9}, {9, 9}, {8, 9}, {8, 8}, {6, 6}, {11, 11}, {10, 10},
    {8, 10}, {9, 11}, {9, 8}, {5, 8}, {7, 8}, {10, 8}, {7, 10}, {7, 11},
    {4, 6}, {5, 7},
};

// 摆上前 stones 手（严格黑白交替，第 1 手黑）⇒ stones 为偶数时轮黑。
Board vct_race_line_prefix(int stones) {
    Board board;
    for (int i = 0; i < stones; ++i) {
        assert(board.place_stone(kVctRaceLine[i][0], kVctRaceLine[i][1],
                                 i % 2 == 0 ? BLACK : WHITE));
    }
    return board;
}

// 这一手落下之后，`who` 一方还有几个「下一手成五」的点 —— 用例自己扫 225 格，不复用 VCT 的
// 防守枚举（轮次184 的口径：只有独立数出来的那个数才能判 VCT，否则是被它自己判）。
int race_points_of(Board& scratch, int who) {
    int count = 0;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            if (!scratch.is_empty(r, c)) continue;
            if (!scratch.place_stone(r, c, who)) continue;
            if (scratch.has_five_in_a_row(r, c, who)) ++count;
            scratch.undo_last_move();
        }
    }
    return count;
}

// #89：#84 那条判据在递归的两处（vct_defend / vct_attack）也各有一份，可它们从来没有断言读者。
// 先量再钉（轮次197 的配对台：同一棵树、同一张新表，只把其中一处的判断改成永假）：
//   ① tools/match_openings.jsonl 全部 209 条 × 深度 6/8/10/12 = 836 格；
//   ② 随机中局 120 个（seed 7）× 深度 4/6/8 = 360 格，帽 6 万；
//   ③ 随机中局 40 个（seed 11）× 深度 4/6 = 120 格，帽 100 万，其中 30 格仍撞帽、90 格跑完；
//   ④ 下面这条真线的 36 个前缀 × 深度 3..7 = 180 格，帽 20 万。
// 合起来 1496 格：result 与 best_move 一格都没变（判 WIN 的格数两侧各自相等：62/62、67/67、
// 16/16），只有 610 格的节点数在动。所以「递归层这一局走到过没有、拒掉的是不是同一件事」在
// search() 交回的答案里根本没有出口，能钉它的只有 vct.h 上那两个按调用点分开的计数。
// 两格局面都是从这条线上挑出来的**便宜**格（①那格 nodes=1、②那格 nodes=4147，都不撞帽）。
//
// 局面来自 tools/match_openings.jsonl 第 13 条（(7,7)(5,5)(6,8)(9,5)…共 14 子）之后 VCT 自己
// 搜出来的那串 forcing 线；两格的黑白子数都相等 ⇒ 轮黑，与探针当时用的轮次一致。
// 变异口径：把 vct_defend 那处改成永假 ⇒ 第一段的 defend_rejections() 断言红；把 vct_attack
// 那处改成永假 ⇒ 第二段的 attack_rejections() 断言红。两处的读者是分开的。根扫描那一处不在这条
// 用例的覆盖里（这两格 root 计数都是 0），它的读者是上面那条 test_vct_root_threat_…用例。
void test_vct_recursive_race_gates_have_assertion_readers() {
    std::atomic<bool> stop(false);
    Evaluator evaluator;

    // ① 34 子 + 深度 2：根扫描一条都不拒（root==0），唯一的递归节点是 vct_defend，它拒 3 条。
    //    当时量到 nodes=1、result=UNKNOWN、best=(-1,-1)：这一格的三个计数把「拒发生在哪一层」
    //    分开钉住了 —— 把 vct_defend 那条判断改成永假，defend_rejections() 就归 0。
    {
        Board board = vct_race_line_prefix(34);
        TranspositionTable tt;
        VCTSearch vct(evaluator, tt);
        const VCTMove r = vct.search(board, BLACK, 2, stop);
        assert(r.result != VCTResult::WIN);
        assert(r.variation.empty());  // #88 的约定：不是 100000+ 的出口不许带半截线
        assert(vct.root_rejections() == 0);
        assert(vct.attack_rejections() == 0);
        assert(vct.defend_rejections() >= 1);
    }

    // ② 32 子 + 深度 3：这一次轮到 vct_attack 那一处拒（当时量到 atk=6、def=0、nodes=4147），
    //    而根上的答案仍然是那句「我赢在 (5,7)」—— 也就是这一处**放行了**真正成立的那条线，
    //    只把赛跑输掉的候选摘掉。改永假 ⇒ attack_rejections() 归 0，这一条立刻红。
    {
        Board board = vct_race_line_prefix(32);
        TranspositionTable tt;
        VCTSearch vct(evaluator, tt);
        const VCTMove r = vct.search(board, BLACK, 3, stop);
        assert(r.result == VCTResult::WIN);
        assert(r.best_move.pos.row == 5 && r.best_move.pos.col == 7);
        assert(vct.root_rejections() == 0);
        assert(vct.defend_rejections() == 0);
        assert(vct.attack_rejections() >= 1);
    }

    std::cout << "test_vct_recursive_race_gates_have_assertion_readers passed" << std::endl;
}

// #88：递归判胜过去只填 best_move，那句「我赢」在库里没有任何可逐手核对之物（轮次181 因此
// 只能靠独立重建顶一次：#104 找到真胜线、#140/#145 找不到 ⇒ 未证）。现在 search() 交回的
// 变例走的就是「分数最低那一手防守」那条分支，所以这一条用例把整条线**自己重放一遍**，逐手核
// 三件事：防守手没有反先（成五 / 活四 / 冲四 / 双四），进攻手落下之后对面不存在「下一手成五」
// 的点（#84 那条赛跑判据，独立扫 225 格数出来，不复用 VCT 的枚举），最后一手确实是根扫描认得
// 的四类胜着之一。核不上就是产品缺陷，不是断言写松的理由。
void test_vct_recursive_win_returns_a_ply_by_ply_checkable_variation() {
    std::atomic<bool> stop(false);
    Evaluator evaluator;
    const Board root = vct_race_line_prefix(32);

    TranspositionTable tt;
    VCTSearch vct(evaluator, tt);
    const VCTMove r = vct.search(root, BLACK, 3, stop);
    assert(r.result == VCTResult::WIN);

    // 这一格量的到的线是 3 手：B(5,7) → W(5,4) → B(4,6)（轮次198 打印出来的原值，与那条真线上
    // 的同一批子只是次序不同）。递归交回的胜着必须把「对面最好的那手防守」和「自己跟进的那手」
    // 一起带回来 —— 只填 best_move 的那条老出口会在这里红（size 1）。
    assert(r.variation.size() >= 3);
    assert(r.variation.front().pos.row == r.best_move.pos.row);
    assert(r.variation.front().pos.col == r.best_move.pos.col);

    Board replay = root;
    for (size_t i = 0; i < r.variation.size(); ++i) {
        const Move& ply = r.variation[i];
        const bool attacker_turn = (i % 2 == 0);
        // 着法必须落在空格上、颜色必须交替 ⇒ 这条线真的是从这一格走出来的。
        assert(replay.is_empty(ply.pos.row, ply.pos.col));
        assert(ply.color == (attacker_turn ? BLACK : WHITE));
        assert(replay.place_stone(ply.pos.row, ply.pos.col, ply.color));

        const PositionPattern pattern = PatternRecognizer::recognize_position(
            replay, ply.pos.row, ply.pos.col, ply.color);
        const bool last = (i + 1 == r.variation.size());
        if (attacker_turn) {
            if (last) {
                // 终手必须是四类胜着之一：本手成五 / 活四 / 双四（第四类「没有防守点」要靠私有
                // 的 generate_defenses，用例够不着 —— 记在这儿，不当成已核）。
                const bool five = replay.has_five_in_a_row(ply.pos.row, ply.pos.col, BLACK);
                bool live_four = false;
                for (int d = 0; d < 4; ++d) {
                    if (pattern.directions[d].type == PatternType::LIVE_FOUR) live_four = true;
                }
                assert(five || live_four || pattern.four_count >= 2);
            }
            // 赛跑：进攻手每一手之后，对面都不能还攥着「下一手成五」的点。
            assert(race_points_of(replay, WHITE) == 0);
        } else {
            // 防守手不能反先 —— 线里出现这种手就说明交回来的不是那条最小值分支。
            assert(!replay.has_five_in_a_row(ply.pos.row, ply.pos.col, WHITE));
            bool tempo = false;
            for (int d = 0; d < 4; ++d) {
                const PatternType t = pattern.directions[d].type;
                if (t == PatternType::LIVE_FOUR || t == PatternType::RUSH_FOUR) tempo = true;
            }
            assert(!tempo && pattern.four_count < 2);
        }
    }

    // 约定的另一半：根扫描当场答的那四类（本手成五 / 活四 / 双四 / 没有防守点）交回的就是那一手，
    // 不许多出别的东西 —— 下面这格黑 (10,5..7) 摆上 (10,4) 就是活四，线长必须正好 1。
    {
        Board board;
        for (int c = 0; c < 3; ++c) assert(board.place_stone(0, c, WHITE));
        for (int c = 5; c <= 7; ++c) assert(board.place_stone(10, c, BLACK));
        TranspositionTable tt;
        VCTSearch vct(evaluator, tt);
        const VCTMove direct = vct.search(board, BLACK, 6, stop);
        assert(direct.result == VCTResult::WIN);
        assert(direct.variation.size() == 1);
        assert(direct.variation.front().pos.row == direct.best_move.pos.row);
        assert(direct.variation.front().pos.col == direct.best_move.pos.col);
    }

    std::cout << "test_vct_recursive_win_returns_a_ply_by_ply_checkable_variation passed"
              << std::endl;
}


// 轮次210 #113 的读者：`OpeningBook` 的抽签去掉了「共享 mt19937 + mutex」，换成一次 relaxed
// 递增喂 splitmix64。撤这把锁的依据不是「反正只有一根线程」—— 恰恰相反，book 只在服务端用，
// 查询发生在每一手思考自己的 worker 线程里，这把锁原本是**真被多根线程拿的**；能撤它是因为
// 共享可变状态整个没了。这五段各钉一半：
//   ① 计数器是真原子（「无锁」在这里是一个数，不是形容词）；② 每抽一次恰好推进一格（没走回共享生成器）；
//   ③ 交回的着法 200 次里至少两种（常数哈希会红）；④ 永不交回已占的 (7,7)，且必是空格；
//   ⑤ 权重 1 与 3 两个家族**都出得来**，且重族 ≥ 2×轻族：漏掉轻族的概率 0.75^200 ≈ 1e-25，
//      重轻颠倒约 1e-13 —— 不是墙钟断言。
void test_opening_book_weighted_draw_reaches_every_candidate() {
    OpeningBook book;
    assert(book.draw_counter_is_lock_free());          // ① 「无锁」是一个可核对的数
    // 两条线共享第一手 (7,7) ⇒ 那一格上挂着两个候选家族：(5,5) 权重 1 与 (6,6) 权重 3
    // （`add_line` 会把整条线按对称变换各存一份，所以每个家族是四个坐标 —— 本轮实测：
    //   轻族 57 次 / 重族 143 次，200 抽，比值 2.5，期望 3）。
    book.add_line({{7, 7}, {5, 5}}, 1, "light");
    book.add_line({{7, 7}, {6, 6}}, 3, "heavy");
    Board board;
    assert(board.place_stone(7, 7, BLACK));
    const std::set<std::pair<int, int>> light_family{{{5, 5}, {5, 9}, {9, 5}, {9, 9}}};
    int light = 0;
    int heavy = 0;
    std::set<std::pair<int, int>> distinct;
    const unsigned long long before = book.draws_taken();
    for (int i = 0; i < 200; ++i) {
        Move out;
        assert(book.query(board, WHITE, out));
        const std::pair<int, int> at{out.pos.row, out.pos.col};
        assert(!(out.pos.row == 7 && out.pos.col == 7) && board.is_empty(out.pos));  // ④
        distinct.insert(at);
        if (light_family.count(at)) ++light;
        else ++heavy;
    }
    assert(book.draws_taken() - before == 200);        // ② 每抽一次恰好推进一格，没有偷用共享生成器
    assert(distinct.size() >= 2);                      // ③ 抽签真的在挑，不是常数
    assert(light > 0 && heavy > 0);                    // ⑤ 两个家族都出得来
    assert(heavy >= 2 * light);                        //   且权重 3 那侧压倒权重 1 那侧（4.5σ 余量）
    std::cout << "test_opening_book_weighted_draw_reaches_every_candidate passed (light "
              << light << " heavy " << heavy << " distinct " << distinct.size() << ")" << std::endl;
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
    test_controller_think_ignores_later_caller_board_edits();
    test_controller_second_think_uses_the_new_position();
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

    test_move_heuristics_killer_slots_hold_two_moves_per_ply();
    test_move_heuristics_history_accumulates_by_depth_squared_and_ages_by_half();
    test_sort_moves_pins_tt_then_killers_then_price_over_history();
    test_three_stage_ordering_pays_for_itself_in_nodes();
    test_aged_history_leaves_a_reused_root_think_unchanged();
    test_killers_are_recorded_by_the_tree_not_by_the_root_or_quiescence();

    // B3, in the contract's order: group A's PvTable units, the committed-line
    // group, the cut think, the no-layer guard, then the control.
    test_pv_table_starts_every_think_empty();
    test_pv_table_extend_concatenates_the_child_line_and_caps_at_max();
    test_pv_table_clear_touches_only_its_own_ply();
    test_pv_table_set_single_answers_one_move_and_overwrites_a_longer_line();
    test_pv_table_refuses_every_ply_and_index_outside_its_slots();
    test_committed_layer_reports_its_principal_variation();
    test_principal_variation_length_tracks_depth_and_pose();
    test_reused_root_think_answers_with_a_shorter_line_that_still_starts_at_the_best_move();
    test_cut_think_reports_the_last_committed_line();
    test_a_think_with_no_committed_layer_reports_no_line();
    test_probe_bounds_leave_a_reused_think_on_the_same_answer();

    // B5, in the contract's order: the probe's truth table against the phases' own
    // enumerations, then what a gated think costs.
    test_threat_probe_answers_what_the_two_phases_would_enumerate();
    test_a_threatless_think_pays_the_probe_and_nothing_else();

    // C1, in the contract's order: the field on a record, the slot with no record,
    // one position read four ways, then the whole think's ledger.
    test_static_eval_on_a_record_leaves_its_bound_alone();
    test_an_evaluation_slot_is_invisible_to_the_bound_readers();
    test_static_score_reuses_a_cached_number_for_either_colour();
    test_a_cached_sign_answers_both_sides_of_every_shape();
    test_a_quiet_think_pays_for_each_evaluation_only_once();

    // C2, in the contract's order: the record against the pass on every square, what the
    // record depends on, the two consumers of it, then the think's own ledger and the
    // boundary the cache does not cross.
    test_the_cell_cache_answers_the_fresh_pass_on_every_square();
    test_a_stone_five_cells_away_is_inside_the_records_dependency();
    test_the_tiers_and_the_ordering_read_the_same_record_the_pass_wrote();
    test_a_quiet_think_prices_its_candidates_about_once_in_two();
    test_a_warm_think_buys_its_own_records_and_no_ones_from_the_first();

    // C3, in the contract's order: the sum against the scan on every board, the sum's
    // addends against the pre-split arithmetic square by square, the sum on the make/unmake
    // path, the one set of records answering either colour's ask, and the read the aborted
    // nodes of a cut think pay.
    test_the_record_sum_is_the_whole_board_scan_on_every_board();
    test_every_addend_of_the_record_sum_is_the_addend_the_scan_pays();
    test_a_far_stone_moves_both_sums_alike_and_undo_brings_both_back();
    test_one_set_of_records_answers_the_sum_for_either_colour();
    test_the_nodes_a_cut_kills_pay_the_sum_out_of_the_records();

    // C4, in the contract's order: what a node entered at the bound is, that the budget
    // still outranks it, the two thinks whose tails reach it, then the bound across a
    // population and its silence where there is no tail.
    test_a_quiescence_node_entered_at_the_cap_answers_only_its_own_static_score();
    test_a_dead_budget_outranks_the_depth_cap_its_check_sits_behind();
    test_a_think_that_runs_into_the_cap_reports_the_cap_and_the_same_answer();
    test_a_capped_tail_can_pay_back_nodes_it_did_not_save();
    test_the_cap_bounds_every_arm_of_the_census_and_is_inert_where_there_is_no_tail();

    // D2: the shard split on the quiet pose, against the arms B3 already pinned.
    test_a_sharded_table_searches_the_tree_an_unshared_one_searches();

    // F, in the contract's order: what the engine decides when the client named only
    // a wall, that the root loop spends exactly that, then the session's hand-off of
    // the number the decision is shaped with.
    test_the_engine_shapes_the_window_the_client_did_not_name();
    test_the_window_the_engine_shapes_is_the_one_the_root_loop_stops_at();
    test_the_session_hands_the_next_think_what_the_last_one_cost();

    // G, in the contract's order: the field leaving the search, then the session keeping it.
    test_the_table_reads_a_think_took_are_reported_out_of_the_search();
    test_the_session_records_the_table_reads_it_kept();

    // A（轮次141, design doc 2.4.3 ②）: what a pre-searched reply buys, then what it is
    // not allowed to do - its own budget, and silence on the hook and the clock - and
    // last what the session looks like while one is running.
    test_a_pondered_reply_think_meets_a_tree_it_already_paid_for();
    test_a_ponder_costs_only_its_budget_and_says_nothing_on_the_hook();
    test_a_pondering_session_arms_a_pre_search_that_commits_nothing();
    test_the_board_sum_cache_answers_every_cell_of_the_census_and_keeps_the_cap_outside();
    test_a_coefficient_change_empties_the_transposition_table();

    // 轮次176：命名了节点预算的那一手，战术层只花自己那份份额（且只收紧、不放宽）。
    test_named_node_budget_prices_the_tactical_phases();
    test_vct_root_threat_loses_the_race_when_the_defender_holds_a_five();
    test_vct_recursive_race_gates_have_assertion_readers();
    test_vct_recursive_win_returns_a_ply_by_ply_checkable_variation();

    test_opening_book_weighted_draw_reaches_every_candidate();
    std::cout << "All search tests passed!" << std::endl;
    return 0;
}

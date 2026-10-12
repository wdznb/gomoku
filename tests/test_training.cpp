#include "core/board.h"
#include "core/pattern.h"
#include "core/evaluator.h"
#include "search/search_engine.h"
#include "training/params.h"
#include "core/transforms.h"
#include "training/game_record.h"
#include "training/elo.h"
#include "training/self_play.h"
#include "training/data_collector.h"
#include "training/genetic.h"
#include "training/hill_climbing.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

// Trainer::play_game() is private, and the games_played counter can only be
// observed first-hand by playing one. Every header trainer.h pulls in is
// included above, so the macro rewrites trainer.h's class body and nothing else.
#define private public
#include "training/trainer.h"
#undef private

using namespace gomoku;

namespace {

std::filesystem::path temp_path(const std::string& name) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("gomoku_training_" + name);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

// The optimizers are tested against a synthetic objective with a known best
// point, because engine self-play fitness would make the suite take hours.
ThreatCoefficients target_coefficients() {
    ThreatCoefficients coeffs;
    coeffs.live_four = 20000;
    coeffs.rush_four = 8000;
    coeffs.live_three = 2000;
    coeffs.sleepy_three = 500;
    coeffs.live_two = 200;
    coeffs.sleepy_two = 40;
    coeffs.double_four = 60000;
    coeffs.double_three = 40000;
    coeffs.four_three = 30000;
    coeffs.position_weight_scale = 2;
    return coeffs;
}

double deviation_score(const ThreatCoefficients& coeffs) {
    const std::vector<int> genes = CoefficientVector::to_vector(coeffs);
    const std::vector<int> target = CoefficientVector::to_vector(target_coefficients());
    double total = 0.0;
    for (size_t i = 0; i < genes.size() && i < target.size(); ++i) {
        const double span = target[i] > 0 ? static_cast<double>(target[i]) : 1.0;
        const double difference = (genes[i] - target[i]) / span;
        total += difference * difference;
    }
    return 1.0 / (1.0 + total);
}

SelfPlayConfig quick_play_config() {
    SelfPlayConfig settings;
    settings.max_depth = 2;
    settings.timeout_ms = 120;
    settings.max_moves = 8;
    return settings;
}

std::vector<Move> moves_of(const GameRecord& record) {
    std::vector<Move> moves;
    moves.reserve(record.moves.size());
    for (const auto& entry : record.moves) moves.push_back(entry.move);
    return moves;
}

// A training game must be a real game: alternating colours from BLACK, no cell
// used twice, nothing off the board, and no forbidden BLACK shape.
void assert_playable_line(const std::vector<Move>& moves) {
    Board board;
    for (size_t i = 0; i < moves.size(); ++i) {
        const Move& move = moves[i];
        const int expected = i % 2 == 0 ? BLACK : WHITE;
        assert(move.color == expected);
        assert(move.pos.is_valid());
        assert(board.is_empty(move.pos));
        if (move.color == BLACK) {
            assert(!PatternRecognizer::is_forbidden(board, move.pos.row, move.pos.col, BLACK));
        }
        board.place_stone(move.pos, move.color);
    }
}

void assert_playable(const GameRecord& record) {
    assert_playable_line(moves_of(record));
}

}

void test_transform_group() {
    // Variant 0 is the identity, and every variant is a bijection of the board.
    for (int variant = 0; variant < SYMMETRY_COUNT; ++variant) {
        std::set<int> seen;
        for (int index = 0; index < BOARD_SIZE * BOARD_SIZE; ++index) {
            const Position moved = transform_position(Position::from_index(index), variant);
            assert(moved.is_valid());
            seen.insert(moved.to_index());
        }
        assert(seen.size() == static_cast<size_t>(BOARD_SIZE * BOARD_SIZE));
    }
    assert(transform_position(Position(3, 11), 0) == Position(3, 11));
    assert(transform_position(Position(3, 11), 4) == Position(11, 3));

    // Applying every variant to one cell yields its symmetry orbit, whose size
    // depends on how much of the group fixes the cell: the centre is fixed by all
    // eight, a corner by four, a general cell by none.
    auto orbit_size = [](Position pos) {
        std::set<int> orbit;
        for (int variant = 0; variant < SYMMETRY_COUNT; ++variant) {
            orbit.insert(transform_position(pos, variant).to_index());
        }
        return orbit.size();
    };
    assert(orbit_size(Position(7, 7)) == 1);
    assert(orbit_size(Position(0, 0)) == 4);
    assert(orbit_size(Position(6, 7)) == 4);
    assert(orbit_size(Position(3, 5)) == static_cast<size_t>(SYMMETRY_COUNT));

    std::cout << "test_transform_group passed" << std::endl;
}

void test_self_symmetries_and_canonical_cells() {
    Board board;
    board.place_stone(7, 7, BLACK);

    // A single centre stone is unchanged by all eight symmetries.
    assert(self_symmetries(board).size() == static_cast<size_t>(SYMMETRY_COUNT));

    const std::vector<Move> candidates = legal_moves(board, WHITE);
    assert(candidates.size() == 24);

    // The 24 neighbours of the centre fall into 5 orbits: edge-adjacent,
    // two-away-straight, diagonal-adjacent, knight-shape, diagonal-two.
    std::set<int> classes;
    for (const auto& candidate : candidates) {
        classes.insert(canonical_cell(board, candidate.pos));
    }
    assert(classes.size() == 5);

    // A stone off every symmetry axis (middle row, middle column, both diagonals)
    // leaves only the identity, so each cell becomes its own representative.
    board.place_stone(5, 8, WHITE);
    assert(self_symmetries(board).size() == 1);
    assert(canonical_cell(board, Position(3, 4)) == Position(3, 4).to_index());

    // A stone on the middle row is still moved by the row reflection, which fixes
    // the centre cell itself.
    Board axial;
    axial.place_stone(7, 7, BLACK);
    axial.place_stone(7, 8, WHITE);
    assert(self_symmetries(axial).size() == 2);
    assert(canonical_cell(axial, Position(0, 0)) == canonical_cell(axial, Position(14, 0)));

    std::cout << "test_self_symmetries_and_canonical_cells passed" << std::endl;
}

void test_transform_board() {
    Board board;
    board.place_stone(7, 7, BLACK);
    board.place_stone(6, 7, WHITE);

    // Transposing across the main diagonal moves the white stone to (7, 6).
    const Board transposed = transform_board(board, 4);
    assert(transposed.get(7, 7) == BLACK);
    assert(transposed.get(7, 6) == WHITE);
    assert(transposed.get(6, 7) == EMPTY);
    assert(transposed.move_count() == 2);

    // Reflecting rows is a symmetry of the board, so stone counts survive.
    const Board reflected = transform_board(board, 1);
    assert(reflected.get(8, 7) == WHITE);
    assert(reflected.move_history().size() == 2);

    std::cout << "test_transform_board passed" << std::endl;
}

void test_game_record_roundtrip() {
    GameRecord record;
    record.winner = BLACK;
    record.reason = GameEndReason::FIVE;
    record.black_profile = 3;
    record.white_profile = 1;
    record.total_nodes = 12345;
    record.max_depth = 4;
    for (int i = 0; i < 3; ++i) {
        MoveRecord entry;
        entry.move = Move(7 + i, 7, i % 2 == 0 ? BLACK : WHITE);
        entry.score = 100 * i;
        entry.depth = 2 + i;
        entry.nodes = 500 + i;
        entry.elapsed_ms = 40 + i;
        entry.method = i == 0 ? "greedy_five" : "alphabeta";
        record.moves.push_back(entry);
    }

    std::string error;
    GameRecord restored;
    assert(GameRecord::from_json(record.to_json(), restored, error));
    assert(restored.moves.size() == record.moves.size());
    assert(restored.winner == BLACK);
    assert(restored.reason == GameEndReason::FIVE);
    assert(restored.black_profile == 3);
    assert(restored.white_profile == 1);
    assert(restored.total_nodes == 12345);
    assert(restored.max_depth == 4);
    assert(restored.moves[2].move == record.moves[2].move);
    assert(restored.moves[2].method == "alphabeta");
    assert(restored.moves[2].nodes == 502);
    assert(restored.is_decisive());
    assert(restored.outcome_for(BLACK) == 1.0f);
    assert(restored.outcome_for(WHITE) == -1.0f);

    const std::filesystem::path path = temp_path("record.json");
    assert(record.save(path.string(), error));
    GameRecord loaded;
    assert(GameRecord::load(path.string(), loaded, error));
    assert(loaded.moves.size() == 3);

    // JSONL appends instead of rewriting, so a long run accumulates games.
    const std::filesystem::path jsonl = temp_path("records.jsonl");
    assert(record.append_to_jsonl(jsonl.string(), error));
    assert(record.append_to_jsonl(jsonl.string(), error));
    std::ifstream file(jsonl);
    std::string line;
    int lines = 0;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        GameRecord parsed;
        assert(GameRecord::from_json(line, parsed, error));
        assert(parsed.moves.size() == 3);
        lines++;
    }
    assert(lines == 2);

    // A record with an off-board move is unusable, so it must be rejected rather
    // than replayed into a crash.
    GameRecord broken;
    broken.reason = GameEndReason::FIVE;
    broken.moves.push_back(MoveRecord{});
    broken.moves[0].move = Move(99, 0, BLACK);
    GameRecord unused;
    assert(!GameRecord::from_json(broken.to_json(), unused, error));

    std::cout << "test_game_record_roundtrip passed" << std::endl;
}

// Round 78, M3: winner and reason are outcomes, not metadata. Defaulting a
// missing one turned a win into a draw in every sample derived from the record;
// an unknown name would read as INVALID and quietly drop a decided game. Both
// are refused, "invalid" itself stays a legal reason, and a rejected parse must
// leave the caller's record untouched.
void test_game_record_requires_outcome() {
    std::string error;

    GameRecord sentinel;
    sentinel.winner = WHITE;
    sentinel.reason = GameEndReason::RESIGN;

    const char* rejected[] = {
        R"({"moves":[]})",                                    // no winner at all
        R"({"moves":[],"reason":"five"})",                    // reason but no winner
        R"({"moves":[],"winner":"black"})",                   // winner but no reason
        R"({"moves":[],"winner":1,"reason":"five"})",         // winner is not a string
        R"({"moves":[],"winner":"black","reason":1})",        // reason is not a string
        R"({"moves":[],"winner":"grey","reason":"five"})",    // winner name not written
        R"({"moves":[],"winner":"black","reason":"fiv"})",    // reason name not written
        R"({"moves":[],"winner":"black","reason":"fiveinarow"})",
    };
    for (const char* text : rejected) {
        GameRecord out = sentinel;
        error.clear();
        assert(!GameRecord::from_json(text, out, error));
        assert(!error.empty());
        // The failed parse may not leave half of itself in the caller's record.
        assert(out.winner == WHITE && out.reason == GameEndReason::RESIGN);
        assert(out.moves.empty());
        std::cout << "  rejected " << text << " -> " << error << std::endl;
    }

    // The three refusal messages are distinct, so a caller can tell "missing"
    // from "misspelled".
    GameRecord out;
    error.clear();
    assert(!GameRecord::from_json(R"({"moves":[]})", out, error));
    assert(error.find("must state winner") != std::string::npos);
    error.clear();
    assert(!GameRecord::from_json(R"({"moves":[],"winner":"black"})", out, error));
    assert(error.find("must state reason") != std::string::npos);
    error.clear();
    assert(!GameRecord::from_json(R"({"moves":[],"winner":"grey","reason":"five"})", out, error));
    assert(error.find("unknown winner colour") != std::string::npos);
    error.clear();
    assert(!GameRecord::from_json(R"({"moves":[],"winner":"black","reason":"fiv"})", out, error));
    assert(error.find("unknown end reason") != std::string::npos);

    // Legal: "invalid" is itself one of the written reason names, and every
    // written name round-trips through the writer unchanged.
    error.clear();
    assert(GameRecord::from_json(
        R"({"moves":[],"winner":"draw","reason":"invalid"})", out, error));
    assert(out.winner == EMPTY && out.reason == GameEndReason::INVALID);

    const GameEndReason reasons[] = {
        GameEndReason::FIVE,    GameEndReason::FORBIDDEN, GameEndReason::RESIGN,
        GameEndReason::BOARD_FULL, GameEndReason::MOVE_LIMIT, GameEndReason::INVALID,
    };
    const int winners[] = {BLACK, WHITE, WHITE, EMPTY, EMPTY, EMPTY};
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        GameRecord written;
        written.winner = winners[i];
        written.reason = reasons[i];
        GameRecord back;
        error.clear();
        assert(GameRecord::from_json(written.to_json(), back, error));
        assert(back.winner == winners[i]);
        assert(back.reason == reasons[i]);
    }

    // A record a real game produces round-trips whole through the same door.
    SearchEngine engine;
    SelfPlay play(engine, engine, quick_play_config());
    const GameRecord played = play.run(std::vector<Move>(), nullptr, nullptr);
    assert(played.reason != GameEndReason::INVALID);
    GameRecord restored;
    error.clear();
    assert(GameRecord::from_json(played.to_json(), restored, error));
    assert(restored.winner == played.winner && restored.reason == played.reason);
    assert(restored.moves.size() == played.moves.size());

    std::cout << "test_game_record_requires_outcome passed" << std::endl;
}

void test_elo_system() {
    EloSystem elo(1500.0, 32.0);
    assert(EloSystem::expected_score(1500, 1500) == 0.5);
    assert(std::abs(EloSystem::expected_score(1700, 1500) - 0.7597) < 0.001);
    assert(std::abs(EloSystem::expected_score(1500, 1700) +
                    EloSystem::expected_score(1700, 1500) - 1.0) < 1e-12);

    const int a = elo.add_player("v0");
    const int b = elo.add_player("v1");
    assert(a == 0 && b == 1);
    // Registering a known name is an alias lookup, not a second player.
    assert(elo.add_player("v1") == b);
    assert(elo.players().size() == 2);

    assert(elo.k_factor(elo.player(a)) == 32.0);

    elo.record_result(a, b, 1.0);
    assert(std::abs(elo.player(a).elo - 1516.0) < 1e-9);
    assert(std::abs(elo.player(b).elo - 1484.0) < 1e-9);
    assert(elo.player(a).wins == 1 && elo.player(b).losses == 1);
    assert(elo.player(a).games == 1);

    // A draw pulls the leader down and the trailer up: the gap shrinks but the
    // leader stays ahead.
    const double gap_before = elo.player(a).elo - elo.player(b).elo;
    elo.record_result(a, b, 0.5);
    assert(elo.player(a).draws == 1);
    assert(elo.player(a).elo < 1516.0);
    assert(elo.player(b).elo > 1484.0);
    const double gap_after = elo.player(a).elo - elo.player(b).elo;
    assert(gap_after > 0.0 && gap_after < gap_before);

    EloSystem matured(1500.0, 32.0);
    const int veteran = matured.add_player("veteran");
    const int newcomer = matured.add_player("newcomer");
    for (int i = 0; i < 40; ++i) matured.record_result(veteran, newcomer, 1.0);
    assert(matured.k_factor(matured.player(veteran)) == 24.0);

    EloSystem experienced(1500.0, 32.0);
    const int settled = experienced.add_player("settled");
    const int rival = experienced.add_player("rival");
    for (int i = 0; i < 120; ++i) experienced.record_result(settled, rival, 1.0);
    assert(experienced.k_factor(experienced.player(settled)) == 16.0);

    std::vector<EloSystem::Player> standings = elo.standings();
    assert(standings.size() == 2);
    assert(standings[0].name == "v0");

    std::string error;
    EloSystem restored;
    assert(restored.load_json(elo.to_json(), error));
    assert(restored.players().size() == 2);
    assert(std::abs(restored.player(0).elo - elo.player(0).elo) < 1e-9);
    assert(restored.player(1).wins == elo.player(1).wins);

    const std::filesystem::path path = temp_path("elo.json");
    assert(elo.save(path.string(), error));
    EloSystem from_file;
    assert(from_file.load(path.string(), error));
    assert(from_file.has_player("v1"));

    // An id that was never registered must throw instead of reading past the end.
    bool threw = false;
    try {
        elo.player(7);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(threw);

    std::cout << "test_elo_system passed" << std::endl;
}

void test_coefficient_vector() {
    ThreatCoefficients base;
    const std::vector<int> genes = CoefficientVector::to_vector(base);
    assert(genes.size() == static_cast<size_t>(TUNABLE_COUNT));
    assert(genes[static_cast<int>(TunableParam::LIVE_FOUR)] == 10000);

    const ThreatCoefficients rebuilt = CoefficientVector::from_vector(genes);
    assert(CoefficientVector::to_vector(rebuilt) == genes);
    // The untunable fields keep their built-in values.
    assert(rebuilt.five == 100000);
    assert(rebuilt.forbidden_penalty == 50000);

    ThreatCoefficients wild;
    wild.live_four = 500000;
    wild.rush_four = 90000;
    wild.live_three = 1;
    wild.double_four = 10;
    wild.position_weight_scale = -5;
    CoefficientVector::sanitize(wild);
    assert(wild.live_four == 40000);
    assert(wild.rush_four == 15000);
    // Out-of-bound repair is clamped, not reordered: a live three of 1 stays below
    // its own minimum only after the bound is applied.
    assert(wild.live_three == 150);
    assert(wild.double_four >= wild.live_four);
    assert(wild.position_weight_scale == 0);
    assert(wild.five == 100000);

    ThreatCoefficients inverted;
    inverted.live_four = 2000;
    inverted.rush_four = 1900;
    inverted.live_three = 6000;
    CoefficientVector::sanitize(inverted);
    assert(inverted.live_three <= inverted.rush_four);
    assert(inverted.rush_four <= inverted.live_four);

    std::string error;
    ThreatCoefficients parsed;
    // five above the mate threshold would desynchronise AlphaBeta's win detection,
    // so the file is refused outright.
    assert(!CoefficientVector::from_json_string("{\"coefficients\":{\"five\":200000}}",
                                                parsed, error));
    assert(!error.empty());
    assert(!CoefficientVector::from_json_string("{\"coefficients\":{\"live_four\":\"many\"}}",
                                                parsed, error));
    assert(!CoefficientVector::from_json_string("not json", parsed, error));
    assert(CoefficientVector::from_json_string("{\"coefficients\":{\"five\":90000,"
                                               "\"live_four\":5000}}", parsed, error));
    assert(parsed.five == 90000);
    assert(parsed.live_four == 5000);

    const std::filesystem::path path = temp_path("params.json");
    ThreatCoefficients champion = target_coefficients();
    assert(CoefficientVector::save(path.string(), champion, error));
    ThreatCoefficients loaded;
    assert(CoefficientVector::load(path.string(), loaded, error));
    assert(CoefficientVector::to_vector(loaded) == CoefficientVector::to_vector(champion));
    assert(!CoefficientVector::load((path.string() + ".missing").c_str(), loaded, error));

    std::cout << "test_coefficient_vector passed" << std::endl;
}

void test_self_play_records_legal_game() {
    SearchEngine black;
    SearchEngine white;
    SelfPlay play(black, white, quick_play_config());

    std::vector<PlyChoices> choices;
    GameRecord record = play.run(std::vector<Move>(), &choices, nullptr);

    assert(!record.moves.empty());
    assert(record.moves.size() <= 8);
    assert_playable(record);
    // A capped training game is undecided, never credited to either side.
    if (record.reason == GameEndReason::MOVE_LIMIT) {
        assert(record.winner == EMPTY);
        assert(!record.is_decisive());
        assert(record.outcome_for(BLACK) == 0.0f);
    } else {
        assert(record.reason == GameEndReason::FIVE ||
               record.reason == GameEndReason::RESIGN);
        assert(record.winner != EMPTY);
    }
    assert(record.total_nodes > 0);
    assert(choices.size() == record.moves.size());

    // Every ply records the alternatives the engine could have chosen, and no
    // alternative repeats the move that was played or a symmetric twin of it.
    for (size_t ply = 0; ply < choices.size(); ++ply) {
        std::set<int> keys;
        keys.insert(choices[ply].chosen_key);
        assert(choices[ply].chosen_key >= 0);
        assert(choices[ply].alternatives.size() == choices[ply].alternative_keys.size());
        for (size_t j = 0; j < choices[ply].alternatives.size(); ++j) {
            assert(!(choices[ply].alternatives[j] == choices[ply].chosen));
            assert(keys.insert(choices[ply].alternative_keys[j]).second);
        }
    }

    std::cout << "test_self_play_records_legal_game passed" << std::endl;
}

void test_self_play_replays_prefix() {
    std::vector<Move> prefix;
    prefix.emplace_back(7, 7, BLACK);
    prefix.emplace_back(8, 8, WHITE);

    SearchEngine black;
    SearchEngine white;
    SelfPlay play(black, white, quick_play_config());

    std::vector<PlyChoices> choices;
    GameRecord record = play.run(prefix, &choices, nullptr);
    assert(record.moves.size() >= 2);
    assert(record.moves[0].move == Move(7, 7, BLACK));
    assert(record.moves[0].method == "replay");
    // Replayed plies carry no engine decision, so choices start after them.
    assert(choices.size() == record.moves.size() - 2);
    assert_playable(record);

    // A prefix that breaks the rules makes the whole game unusable instead of
    // producing a corrupted record.
    std::vector<Move> bad_colour;
    bad_colour.emplace_back(7, 7, WHITE);
    GameRecord rejected = play.run(bad_colour, nullptr, nullptr);
    assert(rejected.reason == GameEndReason::INVALID);
    assert(rejected.winner == EMPTY);
    assert(rejected.moves.empty());

    std::vector<Move> occupied;
    occupied.emplace_back(7, 7, BLACK);
    occupied.emplace_back(7, 7, WHITE);
    assert(play.run(occupied, nullptr, nullptr).reason == GameEndReason::INVALID);

    // A prefix that already contains five ends the game without any search, so a
    // finished line can be replayed as data.
    std::vector<Move> five_line;
    five_line.emplace_back(7, 3, BLACK);
    five_line.emplace_back(0, 0, WHITE);
    five_line.emplace_back(7, 4, BLACK);
    five_line.emplace_back(0, 1, WHITE);
    five_line.emplace_back(7, 5, BLACK);
    five_line.emplace_back(0, 2, WHITE);
    five_line.emplace_back(7, 6, BLACK);
    five_line.emplace_back(0, 3, WHITE);
    five_line.emplace_back(7, 7, BLACK);
    GameRecord decided = play.run(five_line, nullptr, nullptr);
    assert(decided.reason == GameEndReason::FIVE);
    assert(decided.winner == BLACK);
    assert(decided.moves.size() == five_line.size());
    assert_playable(decided);

    std::cout << "test_self_play_replays_prefix passed" << std::endl;
}

void test_self_play_stop_flag_is_invalid() {
    SearchEngine black;
    SearchEngine white;
    SelfPlayConfig settings = quick_play_config();
    settings.max_moves = 40;
    SelfPlay play(black, white, settings);

    std::atomic<bool> stop(true);
    GameRecord record = play.run(std::vector<Move>(), nullptr, &stop);
    // Nothing was decided, so the record must not be readable as a result.
    assert(record.reason == GameEndReason::INVALID);
    assert(record.winner == EMPTY);
    assert(record.moves.empty());

    std::cout << "test_self_play_stop_flag_is_invalid passed" << std::endl;
}

void test_retry_explorer_diverges() {
    SearchEngine black;
    SearchEngine white;
    RetryExplorer explorer(black, white, quick_play_config());

    std::vector<std::vector<Move>> lines;
    for (int game = 0; game < 3; ++game) {
        GameRecord record;
        assert(explorer.explore(record, nullptr));
        assert(record.reason != GameEndReason::INVALID);
        assert_playable(record);
        const std::vector<Move> line = moves_of(record);
        // No two retried games may follow the identical line.
        assert(std::find(lines.begin(), lines.end(), line) == lines.end());
        lines.push_back(line);
    }
    assert(explorer.games_played() == 3);

    // Retry-from-undo keeps the earliest moves identical and changes only the tail.
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::vector<Move>& previous = lines[i - 1];
        const std::vector<Move>& current = lines[i];
        assert(current.size() >= 2);
        assert(!(current == previous));
        size_t shared = 0;
        while (shared < current.size() && shared < previous.size() &&
               current[shared] == previous[shared]) {
            shared++;
        }
        assert(shared + 1 == current.size() || shared < previous.size());
    }

    std::cout << "test_retry_explorer_diverges passed" << std::endl;
}

void test_retry_explorer_forced_line() {
    // Starting from an opening line: the first two plies must be the given moves,
    // and the divergence still happens at the last decision point.
    std::vector<Move> opening;
    opening.emplace_back(7, 7, BLACK);
    opening.emplace_back(7, 8, WHITE);

    SearchEngine black;
    SearchEngine white;
    RetryExplorer explorer(black, white, quick_play_config());
    explorer.set_start_line(opening);

    GameRecord first;
    assert(explorer.explore(first, nullptr));
    assert(first.moves.size() >= 2);
    assert(first.moves[0].move == Move(7, 7, BLACK));
    assert(first.moves[1].move == Move(7, 8, WHITE));
    assert_playable(first);

    GameRecord second;
    assert(explorer.explore(second, nullptr));
    assert(second.moves[0].move == Move(7, 7, BLACK));
    assert(second.moves[1].move == Move(7, 8, WHITE));
    assert(moves_of(first) != moves_of(second));
    assert_playable(second);

    // reset() forgets the tree, so the next game starts from the line again.
    explorer.reset();
    GameRecord third;
    assert(explorer.explore(third, nullptr));
    assert(third.moves[0].move == Move(7, 7, BLACK));

    std::cout << "test_retry_explorer_forced_line passed" << std::endl;
}

void test_feature_extraction() {
    Board board;
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, WHITE);
    board.place_stone(6, 6, BLACK);
    board.place_stone(8, 9, WHITE);
    board.place_stone(9, 9, BLACK);

    DataCollector collector;
    const std::vector<float> features = collector.extract_features(board, BLACK);
    assert(static_cast<int>(features.size()) == DataCollector::FEATURE_COUNT);
    assert(features[DataCollector::FEATURE_SIDE] == 1.0f);
    assert(features[DataCollector::FEATURE_MY_STONES] > features[DataCollector::FEATURE_OPP_STONES]);
    assert(features[DataCollector::FEATURE_PLY] > 0.0f);
    assert(features[DataCollector::FEATURE_ACTIVE] > 0.0f);
    // The last stone placed is black at (9, 9).
    assert(std::abs(features[DataCollector::FEATURE_LAST_ROW] -
                    (9 - BOARD_SIZE / 2) / 7.0f) < 1e-6);
    assert(std::abs(features[DataCollector::FEATURE_LAST_COL] -
                    (9 - BOARD_SIZE / 2) / 7.0f) < 1e-6);
    // BLACK holds its own last-move window centre.
    assert(features[DataCollector::FEATURE_WINDOW + 12] == 1.0f);

    // Shape counts are normalised, so they stay bounded as the board fills.
    for (int i = 0; i < DataCollector::PATTERN_SLOTS * 2; ++i) {
        assert(features[i] >= 0.0f);
        assert(features[i] < 1.0f);
    }

    // The orientation-invariant block must not change under a symmetry, while the
    // window around the last move must, otherwise the augmentation is a no-op.
    const Board rotated = transform_board(board, 4);
    const std::vector<float> rotated_features = collector.extract_features(rotated, BLACK);
    for (int i = 0; i < DataCollector::FEATURE_LAST_ROW; ++i) {
        assert(std::abs(features[i] - rotated_features[i]) < 1e-4);
    }
    bool window_changed = false;
    for (int i = DataCollector::FEATURE_WINDOW; i < DataCollector::FEATURE_COUNT; ++i) {
        if (std::abs(features[i] - rotated_features[i]) > 1e-6) window_changed = true;
    }
    assert(window_changed);

    std::cout << "test_feature_extraction passed" << std::endl;
}

void test_data_collector_samples() {
    SearchEngine black;
    SearchEngine white;
    SelfPlayConfig settings = quick_play_config();
    settings.max_moves = 10;
    SelfPlay play(black, white, settings);

    GameRecord record = play.run(std::vector<Move>(), nullptr, nullptr);
    assert(record.moves.size() >= 4);

    DataCollector collector;

    std::vector<TrainingSample> plain = collector.collect(record, 1000, false);
    assert(!plain.empty());
    for (const auto& sample : plain) {
        assert(static_cast<int>(sample.features.size()) == DataCollector::FEATURE_COUNT);
        assert(sample.game_id == 1000);
        assert(sample.move_index >= 0);
        assert(sample.move.pos.is_valid());
        assert(sample.value == record.outcome_for(record.moves[sample.move_index].move.color));
    }

    const std::vector<TrainingSample> augmented = collector.collect(record, 1000, true);
    assert(augmented.size() >= plain.size());
    assert(augmented.size() <= plain.size() * SYMMETRY_COUNT);

    // Augmentation multiplies one ply into at most eight orientations, and the
    // symmetric ones are dropped, so no ply contributes more than SYMMETRY_COUNT.
    std::map<int, int> per_ply;
    for (const auto& sample : augmented) per_ply[sample.move_index]++;
    assert(per_ply.size() == plain.size());
    for (const auto& entry : per_ply) {
        assert(entry.second >= 1);
        assert(entry.second <= SYMMETRY_COUNT);
    }

    // Shallow searches carry no signal about the coefficients, so depth filtering
    // must reduce the sample count.
    collector.set_min_search_depth(99);
    assert(collector.collect(record, 1000, false).empty());
    collector.set_min_search_depth(1);

    // An invalid record yields nothing rather than garbage samples.
    GameRecord broken = record;
    broken.reason = GameEndReason::INVALID;
    assert(collector.collect(broken, 7, true).empty());

    const std::filesystem::path path = temp_path("samples.jsonl");
    std::string error;
    assert(collector.write_jsonl(augmented, path.string(), error));
    std::vector<TrainingSample> loaded;
    assert(DataCollector::read_jsonl(path.string(), loaded, error));
    assert(loaded.size() == augmented.size());
    assert(loaded[0].game_id == augmented[0].game_id);
    assert(loaded[0].move == augmented[0].move);
    assert(loaded[0].move.color == augmented[0].move.color);
    assert(static_cast<int>(loaded[0].features.size()) == DataCollector::FEATURE_COUNT);

    // A corrupt line is reported, not skipped, so a truncated file cannot quietly
    // shrink the dataset.
    std::ofstream damage(path, std::ios::app);
    damage << "{\"features\":[{\"broken\":1}]}\n";
    damage.close();
    std::vector<TrainingSample> ignored;
    assert(!DataCollector::read_jsonl(path.string(), ignored, error));
    assert(error.find("line") != std::string::npos);

    std::cout << "test_data_collector_samples passed" << std::endl;
}

void test_hill_climbing() {
    ThreatCoefficients base;
    const double initial = deviation_score(base);

    HillClimbingConfig settings;
    settings.max_iterations = 80;
    settings.stagnation_limit = 60;

    HillClimbingOptimizer optimizer(deviation_score, settings, 4242);
    int iterations = 0;
    double best = 0.0;
    const ThreatCoefficients result = optimizer.run(base, iterations, best);

    assert(best > initial);
    assert(deviation_score(result) == best);
    assert(iterations >= 1 && iterations <= settings.max_iterations);
    assert(optimizer.evaluations() == iterations + 1);
    assert(optimizer.best_history().size() == static_cast<size_t>(iterations) + 1);
    assert(optimizer.best_history().front() <= optimizer.best_history().back());

    // Same seed, same walk.
    HillClimbingOptimizer repeat(deviation_score, settings, 4242);
    int repeat_iterations = 0;
    double repeat_best = 0.0;
    const ThreatCoefficients repeat_result = repeat.run(base, repeat_iterations, repeat_best);
    assert(CoefficientVector::to_vector(repeat_result) ==
           CoefficientVector::to_vector(result));

    // A neighbour step rescales exactly one coordinate, and the result is still a
    // legal parameter set.
    const std::vector<int> genes = CoefficientVector::to_vector(base);
    const std::vector<int> grown = HillClimbingOptimizer::neighbor(genes, 0, 1.6);
    assert(grown.size() == genes.size());
    assert(grown[0] == 16000);
    for (size_t i = 1; i < genes.size(); ++i) assert(grown[i] == genes[i]);

    // Scaling the smallest threat down cannot go to zero or below its bound.
    const std::vector<int> shrunk =
        HillClimbingOptimizer::neighbor(genes, static_cast<size_t>(TUNABLE_COUNT) - 2, 0.001);
    assert(shrunk[static_cast<size_t>(TUNABLE_COUNT) - 2] >= 1);
    // An out-of-range coordinate is left alone instead of reading past the end.
    assert(HillClimbingOptimizer::neighbor(genes, genes.size(), 2.0) == genes);

    ThreatCoefficients from_neighbour = CoefficientVector::from_vector(grown);
    CoefficientVector::sanitize(from_neighbour);
    assert(CoefficientVector::to_vector(from_neighbour) == grown);

    // Stagnation ends the search early when nothing improves.
    HillClimbingConfig stuck_settings;
    stuck_settings.max_iterations = 200;
    stuck_settings.stagnation_limit = 5;
    HillClimbingOptimizer stuck([](const ThreatCoefficients&) { return 1.0; },
                                stuck_settings, 99);
    int stuck_iterations = 0;
    double stuck_best = 0.0;
    stuck.run(base, stuck_iterations, stuck_best);
    assert(stuck.converged());
    assert(stuck_iterations == stuck_settings.stagnation_limit);
    assert(std::abs(stuck_best - 1.0) < 1e-12);

    std::cout << "test_hill_climbing passed" << std::endl;
}

void test_genetic() {
    ThreatCoefficients base;
    const double initial = deviation_score(base);

    GeneticConfig settings;
    settings.population_size = 10;
    settings.max_generations = 12;
    settings.stagnation_generations = 20;

    GeneticOptimizer optimizer(deviation_score, settings, 20261011);
    int generations = 0;
    double best = 0.0;
    const ThreatCoefficients result = optimizer.run(base, generations, best);

    assert(best > initial);
    assert(deviation_score(result) >= best - 1e-12);
    assert(generations >= 1 && generations <= settings.max_generations);
    assert(optimizer.population().size() == static_cast<size_t>(settings.population_size));
    assert(optimizer.evaluations() ==
           settings.population_size * (generations + 1));
    // Elitism: the best member of the final population is the returned answer.
    double strongest = 0.0;
    for (const auto& member : optimizer.population()) {
        strongest = std::max(strongest, member.fitness);
        // Every member is a legal parameter set, bounds and ladder included.
        ThreatCoefficients check = member.coefficients;
        CoefficientVector::sanitize(check);
        assert(CoefficientVector::to_vector(check) == member.genes);
    }
    assert(std::abs(strongest - best) < 1e-12);

    GeneticOptimizer repeat(deviation_score, settings, 20261011);
    int repeat_generations = 0;
    double repeat_best = 0.0;
    const ThreatCoefficients repeat_result = repeat.run(base, repeat_generations, repeat_best);
    assert(CoefficientVector::to_vector(repeat_result) == CoefficientVector::to_vector(result));

    // Crossover keeps the vector length and stays within the convex hull of the
    // two parents before sanitising.
    std::mt19937_64 rng(7);
    const std::vector<int> lhs = CoefficientVector::to_vector(base);
    const std::vector<int> rhs = CoefficientVector::to_vector(target_coefficients());
    const std::vector<int> child = GeneticOptimizer::crossover(lhs, rhs, 0.5, rng);
    assert(child.size() == lhs.size());
    for (size_t i = 0; i < child.size(); ++i) {
        assert(child[i] >= std::min(lhs[i], rhs[i]));
        assert(child[i] <= std::max(lhs[i], rhs[i]));
    }

    // A flat fitness cannot improve, so the run reports convergence through
    // stagnation instead of looping to max_generations.
    GeneticConfig flat_settings = settings;
    flat_settings.stagnation_generations = 2;
    GeneticOptimizer flat([](const ThreatCoefficients&) { return 0.25; }, flat_settings, 5);
    int flat_generations = 0;
    double flat_best = 0.0;
    flat.run(base, flat_generations, flat_best);
    assert(flat.converged());
    assert(std::abs(flat_best - 0.25) < 1e-12);
    assert(flat_generations == flat_settings.stagnation_generations);

    std::cout << "test_genetic passed" << std::endl;
}

void test_random_opening_line() {
    const std::vector<Move> line = Trainer::random_opening_line(6, 12345);
    assert(line.size() == 6);
    // The empty board has no near-neighbour candidates, so the opening is forced.
    assert(line[0] == Move(7, 7, BLACK));
    assert_playable_line(line);

    assert(Trainer::random_opening_line(0, 1).empty());
    assert(Trainer::random_opening_line(1, 1).size() == 1);

    // Same seed, same line; the seed selects the branch, so different seeds can
    // differ.
    assert(Trainer::random_opening_line(6, 12345) == line);
    bool differs = false;
    for (uint64_t seed = 1; seed < 12 && !differs; ++seed) {
        if (Trainer::random_opening_line(6, seed) != line) differs = true;
    }
    assert(differs);

    std::cout << "test_random_opening_line passed" << std::endl;
}

TrainingConfig fast_training_config() {
    TrainingConfig config;
    config.rounds = 1;
    config.optimizer = OptimizerKind::HILL_CLIMBING;
    config.hill_climbing_iterations = 40;
    config.stagnation_generations = 6;
    config.collect_data = false;
    config.fitness_games = 2;
    config.verification_games = 2;
    config.accept_score = 0.56;
    return config;
}

void test_trainer_round_promotes() {
    TrainingConfig config = fast_training_config();
    config.params_path = temp_path("champion_params.json").string();
    config.checkpoint_path = temp_path("trainer_checkpoint.json").string();

    Trainer trainer(ThreatCoefficients(), config);
    trainer.set_fitness_override(deviation_score);

    const ThreatCoefficients before = trainer.champion();
    const TrainerRound round = trainer.run_round();

    assert(round.round == 1);
    assert(trainer.versions().size() == 2);
    assert(trainer.ratings().players().size() == 2);
    assert(trainer.rounds_completed() == 1);
    assert(trainer.games_played() == 0);
    assert(trainer.samples_collected() == 0);

    // The candidate beat the champion on the injected objective, so it is kept.
    assert(round.accepted);
    assert(round.verification_score >= config.accept_score);
    assert(deviation_score(trainer.champion()) > deviation_score(before));
    assert(trainer.ratings().player(0).elo < 1500.0);
    assert(trainer.ratings().player(1).elo > 1500.0);

    std::string error;
    ThreatCoefficients from_file;
    assert(CoefficientVector::load(config.params_path, from_file, error));
    assert(CoefficientVector::to_vector(from_file) == CoefficientVector::to_vector(trainer.champion()));

    std::ifstream checkpoint(config.checkpoint_path);
    assert(checkpoint.is_open());

    // A fresh trainer can resume exactly where this one stopped.
    Trainer resumed(ThreatCoefficients(), config);
    assert(resumed.load_checkpoint(config.checkpoint_path, error));
    assert(resumed.rounds_completed() == trainer.rounds_completed());
    assert(resumed.games_played() == trainer.games_played());
    assert(resumed.versions().size() == trainer.versions().size());
    assert(CoefficientVector::to_vector(resumed.champion()) ==
           CoefficientVector::to_vector(trainer.champion()));
    assert(std::abs(resumed.ratings().player(1).elo -
                    trainer.ratings().player(1).elo) < 1e-9);

    std::cout << "test_trainer_round_promotes passed" << std::endl;
}

void test_trainer_round_rejects_regression() {
    TrainingConfig config = fast_training_config();
    config.params_path = temp_path("rejected_params.json").string();
    config.checkpoint_path = temp_path("rejected_checkpoint.json").string();

    Trainer trainer(ThreatCoefficients(), config);
    // A constant objective: no candidate can score above the champion, so nothing
    // may be promoted and no parameter file may appear.
    trainer.set_fitness_override([](const ThreatCoefficients&) { return 0.4; });

    const ThreatCoefficients before = trainer.champion();
    const TrainerRound round = trainer.run_round();

    assert(!round.accepted);
    assert(round.verification_score == 0.5);
    assert(trainer.versions().size() == 2);
    assert(CoefficientVector::to_vector(trainer.champion()) ==
           CoefficientVector::to_vector(before));
    assert(trainer.ratings().player(1).games == 1);
    assert(trainer.ratings().player(1).draws == 1);

    std::ifstream params_file(config.params_path);
    assert(!params_file.is_open());

    std::string error;
    Trainer resumed(ThreatCoefficients(), config);
    assert(resumed.load_checkpoint(config.checkpoint_path, error));
    assert(resumed.rounds_completed() == 1);
    assert(CoefficientVector::to_vector(resumed.champion()) ==
           CoefficientVector::to_vector(before));

    std::cout << "test_trainer_round_rejects_regression passed" << std::endl;
}

void test_trainer_keeps_rule_constants_through_promotion() {
    // Round 74: the optimisers rebuild candidates from a default-constructed
    // struct, so before the fix a promoted candidate lost the rule constants
    // the run started from - the written parameter file carried the built-in
    // five (100000) and forbidden_penalty (50000) instead of the loaded ones.
    TrainingConfig config = fast_training_config();
    config.params_path = temp_path("constant_params.json").string();
    config.checkpoint_path = temp_path("constant_checkpoint.json").string();

    ThreatCoefficients base;
    base.five = 88888;
    base.forbidden_penalty = 77777;

    Trainer trainer(base, config);
    trainer.set_fitness_override(deviation_score);

    const TrainerRound round = trainer.run_round();
    assert(round.accepted);                       // the injected objective promotes
    assert(round.candidate.five == 88888);
    assert(round.candidate.forbidden_penalty == 77777);
    assert(trainer.champion().five == 88888);
    assert(trainer.champion().forbidden_penalty == 77777);

    std::string error;
    ThreatCoefficients from_file;
    assert(CoefficientVector::load(config.params_path, from_file, error));
    assert(from_file.five == 88888);
    assert(from_file.forbidden_penalty == 77777);

    std::ifstream checkpoint(config.checkpoint_path);
    assert(checkpoint.is_open());

    std::cout << "test_trainer_keeps_rule_constants_through_promotion passed" << std::endl;
}

void test_trainer_genetic_round_and_status() {
    TrainingConfig config = fast_training_config();
    config.optimizer = OptimizerKind::GENETIC;
    config.population_size = 6;
    config.generations = 3;
    config.stagnation_generations = 2;
    config.rounds = 2;
    config.checkpoint_path = temp_path("genetic_checkpoint.json").string();

    std::vector<std::string> log_lines;
    Trainer trainer(ThreatCoefficients(), config);
    trainer.set_fitness_override(deviation_score);
    trainer.set_log_callback([&log_lines](const std::string& line) {
        log_lines.push_back(line);
    });

    const std::vector<TrainerRound> history = trainer.run(config.rounds);
    assert(history.size() == static_cast<size_t>(config.rounds));
    assert(trainer.versions().size() == static_cast<size_t>(config.rounds) + 1);
    assert(trainer.rounds_completed() == config.rounds);
    assert(!log_lines.empty());
    bool mentions_genetic = false;
    for (const auto& line : log_lines) {
        if (line.find("genetic") != std::string::npos) mentions_genetic = true;
    }
    assert(mentions_genetic);
    assert(trainer.status_text().find("rounds=2") != std::string::npos);
    assert(trainer.status_text().find("games=0") != std::string::npos);

    // One verification match per round, and a match moves exactly two ratings.
    int total_games = 0;
    const std::vector<EloSystem::Player> standings = trainer.ratings().standings();
    for (size_t i = 0; i < standings.size(); ++i) {
        total_games += standings[i].games;
        if (i > 0) assert(standings[i - 1].elo >= standings[i].elo);
    }
    assert(total_games == 2 * config.rounds);

    std::cout << "test_trainer_genetic_round_and_status passed" << std::endl;
}

void test_trainer_rejects_bad_checkpoint() {
    TrainingConfig config = fast_training_config();
    const std::filesystem::path path = temp_path("broken_checkpoint.json");
    {
        std::ofstream file(path);
        file << "{\"versions\":[]}";
    }

    Trainer trainer(ThreatCoefficients(), config);
    std::string error;
    assert(!trainer.load_checkpoint(path.string(), error));
    assert(!error.empty());

    {
        std::ofstream file(path);
        file << "nonsense";
    }
    assert(!trainer.load_checkpoint(path.string(), error));

    {
        std::ofstream file(path);
        file << "{\"versions\":[],\"rounds_completed\":3}";
    }
    assert(!trainer.load_checkpoint(path.string(), error));

    {
        std::ofstream file(path);
        file << "{\"best_version\":5,\"versions\":["
             << CoefficientVector::to_json_string(ThreatCoefficients()) << "]}";
    }
    assert(!trainer.load_checkpoint(path.string(), error));
    assert(error.find("range") != std::string::npos);

    // A checkpoint whose ratings do not match its versions would attribute results
    // to the wrong parameter set.
    {
        std::ofstream file(path);
        file << "{\"versions\":[" << CoefficientVector::to_json_string(ThreatCoefficients())
             << "],\"elo\":" << "{\"players\":[{\"name\":\"v0\"},{\"name\":\"v1\"}]}" << "}";
    }
    assert(!trainer.load_checkpoint(path.string(), error));
    assert(error.find("disagree") != std::string::npos);

    // Round 78, M5: equal lengths are not agreement. Names shifted by one
    // attribute the results to another version, so the pair is checked name by
    // name, index by index.
    {
        std::ofstream file(path);
        file << "{\"versions\":[" << CoefficientVector::to_json_string(ThreatCoefficients())
             << "," << CoefficientVector::to_json_string(ThreatCoefficients())
             << "],\"elo\":{\"players\":[{\"name\":\"v1\"},{\"name\":\"v0\"}]}}";
    }
    error.clear();
    assert(!trainer.load_checkpoint(path.string(), error));
    assert(error.find("disagree") != std::string::npos);
    assert(trainer.versions().size() == 1);

    // Control: the same shape with the names in index order loads, so the
    // refusal above is the shift and not the file shape.
    {
        std::ofstream file(path);
        file << "{\"versions\":[" << CoefficientVector::to_json_string(ThreatCoefficients())
             << "," << CoefficientVector::to_json_string(ThreatCoefficients())
             << "],\"elo\":{\"players\":[{\"name\":\"v0\"},{\"name\":\"v1\"}]}}";
    }
    assert(trainer.load_checkpoint(path.string(), error));
    assert(trainer.versions().size() == 2);
    assert(trainer.ratings().players().size() == 2);

    assert(!trainer.load_checkpoint((path.string() + ".missing").c_str(), error));

    std::cout << "test_trainer_rejects_bad_checkpoint passed" << std::endl;
}

// --seed takes a full 64-bit unsigned, so a checkpoint can carry a seed above
// 2^63-1; the counters around it must round-trip and stay sane.
void test_trainer_checkpoint_seed_and_counters() {
    TrainingConfig config = fast_training_config();
    config.seed = 0xFFFFFFFFFFFFFFFFull;
    Trainer trainer(ThreatCoefficients(), config);

    const std::filesystem::path path = temp_path("seed_checkpoint.json");
    std::string error;
    assert(trainer.save_checkpoint(path.string(), error));

    Trainer resumed(ThreatCoefficients(), config);
    assert(resumed.load_checkpoint(path.string(), error));
    assert(resumed.config().seed == config.seed);
    assert(resumed.versions().size() == 1);

    // Impossible counters are rejected, and a rejected checkpoint must not leave
    // the trainer running on that file's versions.
    {
        std::ofstream file(path);
        file << "{\"versions\":["
             << CoefficientVector::to_json_string(ThreatCoefficients()) << ","
             << CoefficientVector::to_json_string(ThreatCoefficients())
             << "],\"rounds_completed\":-2}";
    }
    assert(!resumed.load_checkpoint(path.string(), error));
    assert(error.find("negative") != std::string::npos);
    assert(resumed.rounds_completed() == 0);
    assert(resumed.versions().size() == 1);

    std::cout << "test_trainer_checkpoint_seed_and_counters passed" << std::endl;
}

// JSON stores integers far wider than int and get<int> only casts, so a reader
// must either refuse the value or fall back — never accept a truncated one.
void test_readers_reject_out_of_range_numbers() {
    std::string error;

    ThreatCoefficients coefficients;
    assert(!CoefficientVector::from_json_string(
        "{\"coefficients\":{\"five\":99999999999}}", coefficients, error));
    assert(error.find("32-bit") != std::string::npos);

    // Coordinates decide the position, so an unusable one rejects the whole record
    // instead of replaying whatever the low half of the number points at. The
    // record states its outcome so the off-board check is the one that fires.
    GameRecord record;
    assert(!GameRecord::from_json(
        "{\"moves\":[{\"row\":99999999999,\"col\":3,\"color\":\"black\"}],"
        "\"winner\":\"black\",\"reason\":\"five\"}", record, error));
    assert(error.find("off the board") != std::string::npos);

    TrainingSample sample;
    assert(!TrainingSample::from_json(
        "{\"features\":[],\"move\":{\"row\":4294967299,\"col\":3,\"color\":\"black\"}}",
        sample, error));
    assert(error.find("range") != std::string::npos);
    assert(!TrainingSample::from_json(
        "{\"features\":[],\"move\":{\"row\":99,\"col\":3,\"color\":\"black\"}}", sample, error));
    assert(error.find("off the board") != std::string::npos);

    // Metadata decides nothing, so it falls back to its default instead.
    GameRecord metadata;
    assert(GameRecord::from_json(
        "{\"moves\":[],\"winner\":\"draw\",\"reason\":\"invalid\",\"total_nodes\":99999999999}",
        metadata, error));
    assert(metadata.total_nodes == 0);

    EloSystem elo;
    assert(elo.load_json("{\"players\":[{\"name\":\"v0\",\"games\":99999999999}]}", error));
    assert(elo.player(0).games == 0);

    std::cout << "test_readers_reject_out_of_range_numbers passed" << std::endl;
}

// Round 72: play_game() discards an INVALID game, and the discarded game must
// not inflate games_played_ - every checkpoint and status line would otherwise
// overstate the work done. The counter is private and only alive through
// play_game(), so the test drives it directly and pins the ordinary counting.
//
// Round 78, M2: play_match() counts its games through the same `played`
// out-parameter, so a discarded game leaves the denominator instead of
// counting as a half. An INVALID record cannot be produced deterministically
// through this door: play_game passes an empty prefix and a null stop flag
// (trainer.cpp:59), SelfPlay::run only reports INVALID for an illegal prefix
// (self_play.cpp:32-42), an external stop (:122-131) or an engine illegal move
// (:71-82), and every move generator filters forbidden points. So the out-
// parameter is pinned on the valid side here and through the denominator
// identity below; the negative proof for the exclusion is at compile level.
void test_play_game_counts_only_valid_games() {
    TrainingConfig config = fast_training_config();
    config.search_depth = 1;
    config.timeout_ms = 100;
    config.max_moves = 1;

    Trainer trainer(ThreatCoefficients(), config);
    SearchEngine black;
    SearchEngine white;

    // One stone then the move cap: a MOVE_LIMIT game, not INVALID, so it counts
    // once, and an unfinished game scores half.
    assert(trainer.games_played() == 0);
    bool played = false;
    assert(trainer.play_game(black, white, 1, &played) == 0.5);
    assert(played);
    assert(trainer.games_played() == 1);
    SearchEngine black_b;
    SearchEngine white_b;
    assert(trainer.play_game(black_b, white_b, 2) == 0.5);
    assert(trainer.games_played() == 2);

    // An explicit null out-parameter is not a crash and not a behaviour change.
    SearchEngine black_c;
    SearchEngine white_c;
    assert(trainer.play_game(black_c, white_c, 1, nullptr) == 0.5);
    assert(trainer.games_played() == 3);

    // Every game in a match is undecided (MOVE_LIMIT), so the average is 0.5
    // and cannot cross the promotion line: an undecided game must never read
    // as a win.
    Trainer matcher(ThreatCoefficients(), config);
    const double match = matcher.play_match(ThreatCoefficients(), ThreatCoefficients(), 2);
    assert(match == 0.5);
    assert(match < config.accept_score);
    assert(matcher.games_played() == 2);

    // The injected-objective fast path is untouched by the counting change:
    // win, loss and draw are still decided by the two scores.
    Trainer fast(ThreatCoefficients(), config);
    fast.set_fitness_override([](const ThreatCoefficients& c) { return c.live_four > 15000 ? 1.0 : 0.0; });
    ThreatCoefficients strong;
    strong.live_four = 20000;
    ThreatCoefficients weak;
    weak.live_four = 5000;
    assert(fast.play_match(strong, weak, 2) == 1.0);
    assert(fast.play_match(weak, strong, 2) == 0.0);
    assert(fast.play_match(strong, strong, 2) == 0.5);
    assert(fast.games_played() == 0);

    std::cout << "test_play_game_counts_only_valid_games passed" << std::endl;
}

// Round 72: CoefficientVector::save() writes a temporary and renames it over
// the target, so a failed save must not destroy the parameters already on disk
// and a successful one must replace them wholesale.
void test_parameter_save_replaces_atomically() {
    const std::filesystem::path path = temp_path("atomic_params.json");
    const std::string tmp = path.string() + ".tmp";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::string error;

    // A successful save leaves the target in place and no temporary behind.
    assert(CoefficientVector::save(path.string(), target_coefficients(), error));
    assert(error.empty());
    assert(std::filesystem::exists(path));
    assert(!std::filesystem::exists(tmp));

    // A second save replaces the first outright: the reader sees the second
    // content, never a half-rewritten mixture under the old name.
    const ThreatCoefficients second;
    assert(CoefficientVector::save(path.string(), second, error));
    assert(!std::filesystem::exists(tmp));
    ThreatCoefficients loaded;
    assert(CoefficientVector::load(path.string(), loaded, error));
    assert(CoefficientVector::to_vector(loaded) == CoefficientVector::to_vector(second));

    // A temporary that cannot be created aborts the save before the rename, so
    // the previous file survives the failure instead of being truncated.
    std::filesystem::create_directory(tmp, ec);
    assert(!ec);
    assert(!CoefficientVector::save(path.string(), target_coefficients(), error));
    assert(!error.empty());
    ThreatCoefficients still;
    assert(CoefficientVector::load(path.string(), still, error));
    assert(CoefficientVector::to_vector(still) == CoefficientVector::to_vector(second));

    std::filesystem::remove_all(tmp, ec);
    std::filesystem::remove(path, ec);
    std::cout << "test_parameter_save_replaces_atomically passed" << std::endl;
}

// Round 72: Trainer::save_checkpoint() gets the same write-then-rename
// treatment, because a truncated checkpoint is what load_checkpoint refuses.
void test_checkpoint_save_replaces_atomically() {
    TrainingConfig config = fast_training_config();
    config.params_path = temp_path("atomic_ckpt_params.json").string();

    const std::filesystem::path path = temp_path("atomic_checkpoint.json");
    const std::string tmp = path.string() + ".tmp";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::string error;

    Trainer first(ThreatCoefficients(), config);
    assert(first.save_checkpoint(path.string(), error));
    assert(error.empty());
    assert(!std::filesystem::exists(tmp));

    // A second trainer with a different champion overwrites the checkpoint; a
    // resume reads the second state, not a mixture of the two.
    Trainer second(target_coefficients(), config);
    assert(second.save_checkpoint(path.string(), error));
    assert(!std::filesystem::exists(tmp));
    Trainer resumed(ThreatCoefficients(), config);
    assert(resumed.load_checkpoint(path.string(), error));
    assert(CoefficientVector::to_vector(resumed.champion()) ==
           CoefficientVector::to_vector(second.champion()));

    // A blocked temporary makes the save fail before the rename, so the old
    // checkpoint stays loadable instead of being left half-written.
    std::filesystem::create_directory(tmp, ec);
    assert(!ec);
    Trainer third(target_coefficients(), config);
    assert(!third.save_checkpoint(path.string(), error));
    assert(!error.empty());
    Trainer again(ThreatCoefficients(), config);
    assert(again.load_checkpoint(path.string(), error));
    assert(CoefficientVector::to_vector(again.champion()) ==
           CoefficientVector::to_vector(second.champion()));

    std::filesystem::remove_all(tmp, ec);
    std::filesystem::remove(path, ec);
    std::cout << "test_checkpoint_save_replaces_atomically passed" << std::endl;
}

// Round 73, rewind_to_next_branch: a branch point one ply short of the move cap
// replays the whole game and lets the engine decide nothing, so the rewind must
// skip such plies. Every explored game keeps at least one engine-decided ply,
// and the cap itself still produces MOVE_LIMIT endings rather than an error.
void test_retry_rewind_keeps_engine_decisions() {
    SearchEngine black;
    SearchEngine white;
    black.set_resign_detection(false);
    white.set_resign_detection(false);
    SelfPlayConfig settings = quick_play_config();
    settings.max_moves = 5;

    RetryExplorer explorer(black, white, settings);
    std::vector<Move> opening;
    opening.emplace_back(7, 7, BLACK);
    explorer.set_start_line(opening);

    int explored = 0;
    for (int i = 0; i < 6; ++i) {
        GameRecord record;
        if (!explorer.explore(record, nullptr)) break;
        int engine_moves = 0;
        for (const auto& entry : record.moves) {
            if (entry.method != "replay") ++engine_moves;
        }
        assert(engine_moves >= 1);
        // Five plies cannot finish a five, so the cap is what ends every game.
        assert(record.reason == GameEndReason::MOVE_LIMIT);
        assert(record.winner == EMPTY);
        assert_playable(record);
        ++explored;
    }
    assert(explored >= 3);
    assert(explorer.games_played() == explored);

    std::cout << "test_retry_rewind_keeps_engine_decisions passed" << std::endl;
}

// Round 73, generate_data_games: with neither --records nor --samples there is
// nothing to write, so no game may be played at all. The old code paid the full
// self-play cost and dropped every result at merge. One output path restores
// the collection.
void test_generate_data_games_requires_outputs() {
    TrainingConfig config = fast_training_config();
    config.search_depth = 2;
    config.timeout_ms = 120;
    config.max_moves = 8;
    config.data_opening_plies = 2;
    config.data_threads = 2;
    config.seed = 98765;
    config.records_path.clear();
    config.samples_path.clear();

    Trainer trainer(ThreatCoefficients(), config);
    assert(trainer.games_played() == 0);
    assert(trainer.generate_data_games(2, 2) == 0);
    assert(trainer.games_played() == 0);
    assert(trainer.samples_collected() == 0);

    // With the records path present the games are played, counted and appended.
    const std::filesystem::path records = temp_path("data_records.jsonl");
    trainer.config_.records_path = records.string();
    assert(trainer.generate_data_games(2, 2) > 0);
    assert(trainer.games_played() >= 2);
    std::ifstream records_file(records);
    std::string line;
    int record_lines = 0;
    while (std::getline(records_file, line)) {
        if (!line.empty()) ++record_lines;
    }
    assert(record_lines >= 2);

    std::cout << "test_generate_data_games_requires_outputs passed" << std::endl;
}

// Round 73, game_id_base: ids follow the games_played_ high-water mark, so a
// second batch - in the same run or after a kill and resume - can never reissue
// a game_id the first batch already used. The old rounds_completed_-based base
// repeated ids whenever a batch was collected again in the same round, exactly
// what a resume after a kill does, and two different games then shared one
// (game_id, move_index) identity in samples.jsonl. Note move_index alone cannot
// name a row: collection augments every ply into SYMMETRY_COUNT oriented
// variants that share the ply's index, so the identity under test is the
// game_id itself.
void test_data_game_ids_unique_across_batches() {
    TrainingConfig config = fast_training_config();
    config.search_depth = 2;
    config.timeout_ms = 120;
    config.max_moves = 8;
    config.data_opening_plies = 2;
    config.seed = 24680;
    config.records_path.clear();
    const std::filesystem::path samples = temp_path("ids_samples.jsonl");
    config.samples_path = samples.string();
    config.checkpoint_path = temp_path("ids_checkpoint.json").string();

    Trainer trainer(ThreatCoefficients(), config);
    assert(trainer.generate_data_games(2, 1) > 0);
    assert(trainer.games_played() >= 2);

    std::vector<TrainingSample> first;
    std::string error;
    assert(DataCollector::read_jsonl(samples.string(), first, error));
    assert(!first.empty());
    std::set<int> first_ids;
    for (const auto& sample : first) first_ids.insert(sample.game_id);
    assert(first_ids.size() >= 2);

    // The second batch is collected by a trainer resumed from the checkpoint,
    // the shape a run killed between the data append and the next round takes.
    assert(trainer.save_checkpoint(config.checkpoint_path, error));
    Trainer resumed(ThreatCoefficients(), config);
    assert(resumed.load_checkpoint(config.checkpoint_path, error));
    assert(resumed.games_played() == trainer.games_played());
    assert(resumed.generate_data_games(2, 1) > 0);

    std::vector<TrainingSample> all;
    assert(DataCollector::read_jsonl(samples.string(), all, error));
    assert(all.size() > first.size());

    // Every id in the file is either from the first batch or fresh; the old
    // code would have collapsed the second batch back onto the first ids and
    // left fresh_ids empty.
    std::set<int> all_ids;
    for (const auto& sample : all) all_ids.insert(sample.game_id);
    std::set<int> fresh_ids;
    for (int id : all_ids) {
        if (first_ids.count(id) == 0) fresh_ids.insert(id);
    }
    assert(fresh_ids.size() == first_ids.size());
    assert(*fresh_ids.begin() > *first_ids.rbegin());
    assert(all_ids.size() == first_ids.size() + fresh_ids.size());

    std::cout << "test_data_game_ids_unique_across_batches passed" << std::endl;
}

// Round 73, play_match: an odd game request rounds UP to the next even count -
// every pairing plays both colours - and the score is normalised by the games
// actually played: --fitness-games 3 means four games, not two.
void test_play_match_rounds_up_to_even() {
    TrainingConfig config = fast_training_config();
    config.search_depth = 2;
    config.timeout_ms = 120;
    config.max_moves = 8;
    config.resign_enabled = false;

    Trainer trainer(ThreatCoefficients(), config);
    const int before = trainer.games_played();
    const double score = trainer.play_match(ThreatCoefficients(), target_coefficients(), 3);
    assert(trainer.games_played() - before == 4);
    // Eight plies cannot finish a five, so every game is a MOVE_LIMIT draw:
    // four halves over four games pins the normalisation to exactly 0.5.
    assert(std::abs(score - 0.5) < 1e-12);

    const int before_single = trainer.games_played();
    trainer.play_match(ThreatCoefficients(), target_coefficients(), 1);
    assert(trainer.games_played() - before_single == 2);

    std::cout << "test_play_match_rounds_up_to_even passed" << std::endl;
}

// Round 73: write failures on params, checkpoint, records and samples must
// reach std::cerr even with no log callback installed. --quiet only silences
// progress lines; data loss has to stay visible.
void test_write_failures_report_to_stderr() {
    const std::filesystem::path blocker = temp_path("stderr_blocker");
    {
        std::ofstream file(blocker);
        file << "not a directory";
    }

    // run_round: an accepted candidate whose params and checkpoint live under a
    // regular file reports both failed saves.
    TrainingConfig config = fast_training_config();
    config.accept_score = 0.0;
    config.params_path = (blocker / "params.json").string();
    config.checkpoint_path = (blocker / "checkpoint.json").string();
    Trainer trainer(ThreatCoefficients(), config);
    trainer.set_fitness_override([](const ThreatCoefficients&) { return 0.5; });
    std::ostringstream round_log;
    std::streambuf* previous = std::cerr.rdbuf(round_log.rdbuf());
    const TrainerRound round = trainer.run_round();
    std::cerr.rdbuf(previous);
    assert(round.accepted);
    assert(round_log.str().find("could not save parameters") != std::string::npos);
    assert(round_log.str().find("could not save checkpoint") != std::string::npos);

    // Data collection: an unwritable records path reports the failed append,
    // and the samples the round did produce are still returned.
    TrainingConfig record_config = fast_training_config();
    record_config.search_depth = 2;
    record_config.timeout_ms = 120;
    record_config.max_moves = 8;
    record_config.seed = 13579;
    record_config.records_path = (blocker / "records.jsonl").string();
    record_config.samples_path.clear();
    Trainer record_trainer(ThreatCoefficients(), record_config);
    std::ostringstream record_log;
    previous = std::cerr.rdbuf(record_log.rdbuf());
    const int record_samples = record_trainer.generate_data_games(2, 1);
    std::cerr.rdbuf(previous);
    assert(record_samples > 0);
    assert(record_log.str().find("could not append record") != std::string::npos);

    // ... and an unwritable samples path reports its own failure.
    TrainingConfig sample_config = record_config;
    sample_config.records_path.clear();
    sample_config.samples_path = (blocker / "samples.jsonl").string();
    Trainer sample_trainer(ThreatCoefficients(), sample_config);
    std::ostringstream sample_log;
    previous = std::cerr.rdbuf(sample_log.rdbuf());
    const int sample_samples = sample_trainer.generate_data_games(2, 1);
    std::cerr.rdbuf(previous);
    assert(sample_samples > 0);
    assert(sample_log.str().find("could not append samples") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(blocker, ec);
    std::cout << "test_write_failures_report_to_stderr passed" << std::endl;
}

// Round 75, E6: the six file.flush() sites. A buffered stream reports a full
// disk only when its buffer leaves for the device, so a good() read right after
// the last << could call a write that reached nothing a success: the record was
// silently dropped and the parameter file was renamed over by a truncated .tmp.
// /dev/full fails every write with ENOSPC, which is the cheapest full disk
// there is.
void test_buffered_write_failures_are_caught() {
    if (!std::filesystem::exists("/dev/full")) {
        std::cout << "test_buffered_write_failures_are_caught skipped: /dev/full missing"
                  << std::endl;
        return;
    }
    auto file_bytes = [](const std::string& p) {
        std::ifstream in(p, std::ios::binary);
        std::string content;
        char buffer[512];
        while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
            content.append(buffer, static_cast<size_t>(in.gcount()));
        }
        return content;
    };
    std::string error;

    // Control experiment for the mechanism the fix addresses: one small write to
    // /dev/full still reads as good() until the buffer is flushed; the flush is
    // what the six sites gained, and what makes the failures below observable.
    {
        std::ofstream probe("/dev/full", std::ios::binary | std::ios::trunc);
        probe << "x";
        const bool good_before_flush = probe.good();
        probe.flush();
        std::cout << "  /dev/full probe: good() before flush="
                  << (good_before_flush ? "true" : "false") << " after="
                  << (probe.good() ? "true" : "false") << std::endl;
        assert(!probe.good());
    }

    // append_to_jsonl: the server marks a game exported on this return value, so
    // a record that never left the buffer must come back as a failure.
    GameRecord record;
    record.winner = WHITE;
    record.reason = GameEndReason::FIVE;
    error.clear();
    assert(!record.append_to_jsonl("/dev/full", error));
    assert(!error.empty());
    std::cout << "  append_to_jsonl -> " << error << std::endl;

    // The single-file writers report their failure the same way.
    error.clear();
    assert(!record.save("/dev/full", error));
    assert(!error.empty());
    EloSystem elo;
    error.clear();
    assert(!elo.save("/dev/full", error));
    assert(!error.empty());
    const std::vector<TrainingSample> one_sample(1);
    DataCollector collector;
    error.clear();
    assert(!collector.write_jsonl(one_sample, "/dev/full", error));
    assert(!error.empty());
    std::cout << "  record/elo/collector saves -> " << error << std::endl;

    // The two rename guards: point the temporary at /dev/full and the old file
    // has to survive byte for byte, with the failed temporary still in place.
    std::error_code ec;
    const std::filesystem::path params = temp_path("flush_params.json");
    const std::string params_tmp = params.string() + ".tmp";
    std::filesystem::remove_all(params_tmp, ec);
    error.clear();
    assert(CoefficientVector::save(params.string(), target_coefficients(), error));
    const std::string params_before = file_bytes(params.string());
    std::filesystem::create_symlink("/dev/full", params_tmp, ec);
    assert(!ec);
    error.clear();
    assert(!CoefficientVector::save(params.string(), ThreatCoefficients(), error));
    assert(!error.empty());
    assert(!std::filesystem::is_symlink(params));
    assert(file_bytes(params.string()) == params_before);
    assert(std::filesystem::is_symlink(params_tmp));
    ThreatCoefficients loaded;
    assert(CoefficientVector::load(params.string(), loaded, error));
    assert(CoefficientVector::to_vector(loaded) ==
           CoefficientVector::to_vector(target_coefficients()));
    std::cout << "  params save with a poisoned temporary -> " << error << std::endl;
    std::filesystem::remove(params_tmp, ec);
    std::filesystem::remove(params, ec);

    TrainingConfig ckpt_config = fast_training_config();
    ckpt_config.params_path = temp_path("flush_ckpt_params.json").string();
    const std::filesystem::path ckpt = temp_path("flush_checkpoint.json");
    const std::string ckpt_tmp = ckpt.string() + ".tmp";
    std::filesystem::remove_all(ckpt_tmp, ec);
    Trainer trainer(ThreatCoefficients(), ckpt_config);
    error.clear();
    assert(trainer.save_checkpoint(ckpt.string(), error));
    const std::string ckpt_before = file_bytes(ckpt.string());
    std::filesystem::create_symlink("/dev/full", ckpt_tmp, ec);
    assert(!ec);
    error.clear();
    assert(!trainer.save_checkpoint(ckpt.string(), error));
    assert(!error.empty());
    assert(!std::filesystem::is_symlink(ckpt));
    assert(file_bytes(ckpt.string()) == ckpt_before);
    assert(std::filesystem::is_symlink(ckpt_tmp));
    std::cout << "  checkpoint save with a poisoned temporary -> " << error << std::endl;
    std::filesystem::remove(ckpt_tmp, ec);
    std::filesystem::remove(ckpt, ec);

    std::cout << "test_buffered_write_failures_are_caught passed" << std::endl;
}

// Round 75, F1: a present `coefficients` that is not an object is a malformed
// parameter file, not an absent one. The old reader fell through to the root,
// found none of the twelve names there and reported a successful load of pure
// defaults - the server then printed "Parameters loaded" for a file it had
// never honored. A file that carries only part of the keys is still legal:
// absent keys keep their defaults and are not an error.
void test_params_rejects_non_object_coefficients() {
    ThreatCoefficients parsed;
    std::string error;
    const char* malformed[] = {R"({"coefficients":[]})", R"({"coefficients":"x"})",
                               R"({"coefficients":null})"};

    for (const char* text : malformed) {
        error.clear();
        assert(!CoefficientVector::from_json_string(text, parsed, error));
        assert(error == "coefficients must be an object");
    }

    // Through the file door as well: load() refuses the same three shapes.
    for (const char* text : malformed) {
        const std::filesystem::path path = temp_path("bad_coefficients.json");
        { std::ofstream(path) << text; }
        error.clear();
        assert(!CoefficientVector::load(path.string(), parsed, error));
        assert(error == "coefficients must be an object");
    }

    // Control 1: an object holding only some of the twelve keys loads, and the
    // keys it does not name keep their defaults.
    error.clear();
    assert(CoefficientVector::from_json_string(R"({"coefficients":{"live_four":12345}})",
                                               parsed, error));
    assert(error.empty());
    assert(parsed.live_four == 12345);
    const ThreatCoefficients defaults;
    assert(parsed.rush_four == defaults.rush_four);
    assert(parsed.five == defaults.five);

    // Control 2: the partial file form works through load() too.
    const std::filesystem::path partial = temp_path("partial_coefficients.json");
    { std::ofstream(partial) << R"({"coefficients":{"double_four":54321}})"; }
    error.clear();
    assert(CoefficientVector::load(partial.string(), parsed, error));
    assert(parsed.double_four == 54321);
    assert(parsed.live_four == defaults.live_four);

    // Control 3: the bare root form (no `coefficients` wrapper) is unchanged.
    error.clear();
    assert(CoefficientVector::from_json_string(R"({"live_three":321})", parsed, error));
    assert(parsed.live_three == 321);

#ifndef _WIN32
    // The CLI doors: a malformed --params file makes both binaries exit 1 with
    // their own refusal, and neither may print the success line (README:154:
    // the server exits rather than falling back to defaults).
    if (std::filesystem::exists("gomoku_server") &&
        std::filesystem::exists("gomoku_train")) {
        const std::filesystem::path bad = temp_path("params_bad_cli.json");
        { std::ofstream(bad) << R"({"coefficients":[]})"; }
        const std::filesystem::path out = temp_path("params_cli_stdout.txt");
        const std::filesystem::path err = temp_path("params_cli_stderr.txt");
        auto read_all = [](const std::filesystem::path& p) {
            std::ifstream in(p);
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        };
        auto run = [&](const std::string& command) {
            const int status = std::system(command.c_str());
            assert(status != -1);
            return WEXITSTATUS(status);
        };
        const std::string redirect = " > '" + out.string() + "' 2> '" + err.string() + "'";
        std::error_code ec;
        std::filesystem::remove(out, ec);

        assert(run("./gomoku_server --params '" + bad.string() + "' --no-book" + redirect) == 1);
        assert(read_all(out).find("Parameters loaded") == std::string::npos);
        const std::string server_err = read_all(err);
        assert(server_err.find("Cannot load --params") != std::string::npos);
        std::cout << "  server --params: "
                  << server_err.substr(0, server_err.find('\n')) << std::endl;

        assert(run("./gomoku_train --params '" + bad.string() + "'" + redirect) == 1);
        assert(read_all(out).find("Starting from") == std::string::npos);
        const std::string trainer_err = read_all(err);
        assert(trainer_err.find("cannot use --params") != std::string::npos);
        std::cout << "  trainer --params: "
                  << trainer_err.substr(0, trainer_err.find('\n')) << std::endl;

#if defined(__linux__)
        // Positive control: the same reader accepts a partial object file and
        // the server says so before `timeout` ends it, so the absence of the
        // line above is a refusal, not an inability to ever print it.
        const std::filesystem::path good = temp_path("params_good_cli.json");
        { std::ofstream(good) << R"({"coefficients":{"live_four":12345}})"; }
        std::filesystem::remove(out, ec);
        std::system(("timeout 3 ./gomoku_server --params '" + good.string() +
                     "' --no-book --port 8123 > '" + out.string() + "' 2>&1").c_str());
        assert(read_all(out).find("Parameters loaded:") != std::string::npos);
        std::cout << "  server --params control: the partial file prints Parameters loaded"
                  << std::endl;
#endif
    } else {
        std::cout << "  CLI --params probes skipped: binaries not in the working directory"
                  << std::endl;
    }
#endif

    std::cout << "test_params_rejects_non_object_coefficients passed" << std::endl;
}

// R77 L1: the two rule constants behind every mate score are checked at the
// door. five <= 0 collapses the evaluation scale (ordinary scores are capped at
// five - 1), and forbidden_penalty <= 0 turns the forbidden-move cost into a
// bonus; at the 100000 mate threshold AlphaBeta reads our own penalty as a
// proven loss and resigns. A rejected file must leave `out` untouched.
void test_params_rejects_broken_rule_constants() {
    ThreatCoefficients sentinel;
    sentinel.five = 31337;
    sentinel.forbidden_penalty = 4242;

    const char* rejected[] = {
        R"({"coefficients":{"five":0}})",
        R"({"coefficients":{"five":-1}})",
        R"({"coefficients":{"five":100001}})",
        R"({"coefficients":{"forbidden_penalty":0}})",
        R"({"coefficients":{"forbidden_penalty":-5}})",
        R"({"coefficients":{"forbidden_penalty":100000}})",
        R"({"coefficients":{"forbidden_penalty":100001}})",
        R"({"five":0})",
    };
    std::string error;
    for (const char* text : rejected) {
        ThreatCoefficients out = sentinel;
        error.clear();
        assert(!CoefficientVector::from_json_string(text, out, error));
        assert(!error.empty());
        assert(out.five == sentinel.five);
        assert(out.forbidden_penalty == sentinel.forbidden_penalty);
        std::cout << "  rejected " << text << " -> " << error << std::endl;
    }

    // Accepted: a file that names a known key but keeps the two constants out
    // of it (round 78, M1: a file naming no known key at all is now refused as
    // the wrong file - see test_params_rejects_unrecognised_keys), the boundary
    // values, and a round-trip through the writer.
    const ThreatCoefficients defaults;
    ThreatCoefficients parsed;
    error.clear();
    assert(CoefficientVector::from_json_string(R"({"live_four":16000})", parsed, error));
    assert(parsed.live_four == 16000);
    assert(parsed.five == defaults.five);
    assert(parsed.forbidden_penalty == defaults.forbidden_penalty);

    error.clear();
    assert(CoefficientVector::from_json_string(
        R"({"coefficients":{"five":100000,"forbidden_penalty":50000}})", parsed, error));
    assert(parsed.five == 100000);
    assert(parsed.forbidden_penalty == 50000);
    std::cout << "  accepted the 100000/50000 boundary" << std::endl;

    ThreatCoefficients round_trip;
    round_trip.five = 88888;
    round_trip.forbidden_penalty = 77777;
    error.clear();
    assert(CoefficientVector::from_json_string(
        CoefficientVector::to_json_string(round_trip), parsed, error));
    assert(parsed.five == 88888);
    assert(parsed.forbidden_penalty == 77777);
    std::cout << "  round-tripped 88888/77777 through the writer" << std::endl;

    std::cout << "test_params_rejects_broken_rule_constants passed" << std::endl;
}

// Round 78, M1: a parameter file that names none of the twelve coefficients is
// the wrong file (or a wholesale rename), and the old reader loaded it as a
// successful load of pure defaults - training ran against parameters the file
// never stated. An unrecognised key next to real ones is a typo (live_fourx)
// that would silently leave its coefficient at the default. Both are refused,
// the second naming the key; a partial but well-spelled file stays legal.
void test_params_rejects_unrecognised_keys() {
    std::string error;
    ThreatCoefficients parsed;

    const char* no_known[] = {
        R"({"live_fourx":1})",
        R"({"foo":1,"bar":2})",
        R"({"coefficients":{}})",
        R"({"coefficients":{"live_fourx":1500,"rush_fourx":300}})",
    };
    for (const char* text : no_known) {
        error.clear();
        assert(!CoefficientVector::from_json_string(text, parsed, error));
        assert(error == "no known coefficient key found in the file");
        std::cout << "  rejected " << text << " -> " << error << std::endl;
    }

    // A known key that is also out of range must not mask the unknown-key
    // report: the file is checked for shape before its values are trusted.
    error.clear();
    assert(!CoefficientVector::from_json_string(
        R"({"coefficients":{"five":1000,"junk":1}})", parsed, error));
    assert(error == "unknown coefficient key: junk");
    std::cout << "  rejected {\"coefficients\":{\"five\":1000,\"junk\":1}} -> " << error
              << std::endl;

    error.clear();
    assert(!CoefficientVector::from_json_string(
        R"({"coefficients":{"live_four":16000,"live_fourx":1}})", parsed, error));
    assert(error == "unknown coefficient key: live_fourx");

    // Controls: the writer's own output still loads whole, a partial file naming
    // real keys still loads with the rest at their defaults, and the bare-root
    // form is unchanged.
    ThreatCoefficients written;
    written.live_four = 20000;
    written.five = 32411;
    error.clear();
    assert(CoefficientVector::from_json_string(
        CoefficientVector::to_json_string(written), parsed, error));
    assert(parsed.live_four == 20000 && parsed.five == 32411);

    const ThreatCoefficients defaults;
    error.clear();
    assert(CoefficientVector::from_json_string(R"({"live_three":321})", parsed, error));
    assert(parsed.live_three == 321);
    assert(parsed.live_four == defaults.live_four);
    assert(parsed.five == defaults.five);

    std::cout << "test_params_rejects_unrecognised_keys passed" << std::endl;
}

// R77 L2: stod() parses "nan"/"inf" with used == text.size(), and a NaN then
// sails through every range guard below it (all comparisons are false), so
// --accept-score nan used to start a training run whose promotion threshold no
// score can clear. The reader must refuse non-finite numbers like any other
// malformed text, and the ordinal threshold must still parse.
void test_trainer_cli_rejects_non_finite_accept_score() {
#ifndef _WIN32
    if (!std::filesystem::exists("gomoku_train")) {
        std::cout << "  trainer CLI --accept-score probes skipped: binary not in the"
                     " working directory" << std::endl;
        return;
    }
    const std::filesystem::path out = temp_path("accept_cli_stdout.txt");
    const std::filesystem::path err = temp_path("accept_cli_stderr.txt");
    auto read_all = [](const std::filesystem::path& p) {
        std::ifstream in(p);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    };
    auto run = [&](const std::string& command) {
        const int status = std::system(command.c_str());
        assert(status != -1);
        return WEXITSTATUS(status);
    };
    const std::string redirect = " > '" + out.string() + "' 2> '" + err.string() + "'";
    std::error_code ec;

    for (const char* value : {"nan", "inf", "-inf"}) {
        std::filesystem::remove(out, ec);
        std::filesystem::remove(err, ec);
        assert(run(std::string("./gomoku_train --accept-score ") + value + redirect) == 1);
        const std::string message = read_all(err);
        assert(message.find("invalid number for --accept-score") != std::string::npos);
        std::cout << "  --accept-score " << value << ": "
                  << message.substr(0, message.find('\n')) << std::endl;
    }

    // Positive control: a finite value parses, so --help still runs and prints
    // the usage text - the refusals above are about the value, not the flag.
    std::filesystem::remove(out, ec);
    assert(run("./gomoku_train --accept-score 0.9 --help" + redirect) == 0);
    assert(read_all(out).find("Usage:") != std::string::npos);
    std::cout << "  --accept-score 0.9: parsed, --help printed the usage text" << std::endl;
#endif
    std::cout << "test_trainer_cli_rejects_non_finite_accept_score passed" << std::endl;
}

int main() {
    std::cout << "Running training tests..." << std::endl;

    PatternRecognizer::init();

    test_transform_group();
    test_self_symmetries_and_canonical_cells();
    test_transform_board();
    test_game_record_roundtrip();
    test_game_record_requires_outcome();
    test_elo_system();
    test_coefficient_vector();
    test_self_play_records_legal_game();
    test_self_play_replays_prefix();
    test_self_play_stop_flag_is_invalid();
    test_retry_explorer_diverges();
    test_retry_explorer_forced_line();
    test_feature_extraction();
    test_data_collector_samples();
    test_hill_climbing();
    test_genetic();
    test_random_opening_line();
    test_trainer_round_promotes();
    test_trainer_round_rejects_regression();
    test_trainer_keeps_rule_constants_through_promotion();
    test_trainer_genetic_round_and_status();
    test_trainer_rejects_bad_checkpoint();
    test_trainer_checkpoint_seed_and_counters();
    test_readers_reject_out_of_range_numbers();
    test_play_game_counts_only_valid_games();
    test_parameter_save_replaces_atomically();
    test_checkpoint_save_replaces_atomically();
    test_retry_rewind_keeps_engine_decisions();
    test_generate_data_games_requires_outputs();
    test_data_game_ids_unique_across_batches();
    test_play_match_rounds_up_to_even();
    test_write_failures_report_to_stderr();
    test_buffered_write_failures_are_caught();
    test_params_rejects_non_object_coefficients();
    test_params_rejects_broken_rule_constants();
    test_params_rejects_unrecognised_keys();
    test_trainer_cli_rejects_non_finite_accept_score();

    std::cout << "All training tests passed" << std::endl;
    return 0;
}

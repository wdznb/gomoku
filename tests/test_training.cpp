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
    // instead of replaying whatever the low half of the number points at.
    GameRecord record;
    assert(!GameRecord::from_json(
        "{\"moves\":[{\"row\":99999999999,\"col\":3,\"color\":\"black\"}]}", record, error));
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
    assert(GameRecord::from_json("{\"moves\":[],\"total_nodes\":99999999999}", metadata, error));
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
    assert(trainer.play_game(black, white, 1) == 0.5);
    assert(trainer.games_played() == 1);
    SearchEngine black_b;
    SearchEngine white_b;
    assert(trainer.play_game(black_b, white_b, 2) == 0.5);
    assert(trainer.games_played() == 2);

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

int main() {
    std::cout << "Running training tests..." << std::endl;

    PatternRecognizer::init();

    test_transform_group();
    test_self_symmetries_and_canonical_cells();
    test_transform_board();
    test_game_record_roundtrip();
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

    std::cout << "All training tests passed" << std::endl;
    return 0;
}

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
#include "training/replay_analyzer.h"
#include "training/tactics_card.h"
#include "training/genetic.h"
#include "training/hill_climbing.h"
#include "training/sprt.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
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
#include <thread>
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

// 丁-3：优化器看到的候选是一对系数，所以两侧各按同一把尺子量，取平均。只数黑侧的
// 话白侧那一半没有梯度，`best > initial` 就成了运气而不是测量。
double deviation_score(const CoefficientPair& pair) {
    return 0.5 * (deviation_score(pair.black) + deviation_score(pair.white));
}

// 基因向量按色展开：0..13 是黑侧、14..27 是白侧。测试按名字取坐标，不把 14 写死。
size_t black_gene(TunableParam gene) { return static_cast<size_t>(gene); }
size_t white_gene(TunableParam gene) {
    return static_cast<size_t>(gene) + static_cast<size_t>(TUNABLE_COUNT);
}

// 把上面那个读数当**回调**交出去时要一个没有歧义的名字：`deviation_score` 现在有两个
// 重载，重载函数名进不了 `std::function` 的模板实参推导。
double pair_deviation_score(const CoefficientPair& pair) { return deviation_score(pair); }

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

// The board after a line, folded to its smallest symmetry variant: two openings
// that differ only by a rotation are the same position for a symmetric engine, so a
// set is only as diverse as the number of these keys it produces.
std::string symmetry_key_of(const std::vector<Move>& line) {
    Board board;
    for (const auto& move : line) board.place_stone(move.pos, move.color);
    std::string best;
    for (int variant = 0; variant < SYMMETRY_COUNT; ++variant) {
        const Board turned = transform_board(board, variant);
        std::string cells;
        for (int row = 0; row < BOARD_SIZE; ++row) {
            for (int col = 0; col < BOARD_SIZE; ++col) {
                cells += std::to_string(static_cast<char>('0' + turned.get(row, col)));
            }
        }
        if (best.empty() || cells < best) best = cells;
    }
    return best;
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

// Round 101 FF1: the bracket behind the rating table disagreed with the promotion
// rule it sits next to. README:198 makes a verification match 8 games, so a 5-3
// candidate scores 0.625, which trainer.cpp:408 promotes (>= the 0.56 bar, open match)
// while the old `>= 0.75 / <= 0.25` test booked *both* sides a draw - the table
// printed a match somebody had clearly taken as nobody winning. The bracket now
// follows what a match score means: above half is a win, below half a loss, exactly
// half the only draw. The two scores the stricter test already called correctly
// (0.75 from round 99's D6, 0.25) stay correct, and the rating itself is computed
// from score_a before any bracketing, so every arm pins the Elo delta too - the
// bookkeeping is the only thing that changed.
void test_elo_scores_a_majority_match_as_a_win() {
    struct Arm {
        double score;
        int wins_a;
        int losses_a;
        int draws_a;
    };
    const Arm arms[] = {
        {1.000, 1, 0, 0},
        {0.750, 1, 0, 0},   // 3-1 of 4: win before and after (round 99, D6)
        {0.625, 1, 0, 0},   // 5-3 of 8: the score that used to print as a draw
        {0.500, 0, 0, 1},   // exactly half is still the only draw
        {0.375, 0, 1, 0},   // 3-5 of 8: the mirror of the arm above
        {0.250, 0, 1, 0},
        {0.000, 0, 1, 0},
    };

    for (const Arm& arm : arms) {
        EloSystem elo(1500.0, 32.0);
        const int a = elo.add_player("candidate");
        const int b = elo.add_player("champion");
        elo.record_result(a, b, arm.score);

        std::cout << "  [r101ff1] score " << arm.score << " -> candidate w/l/d "
                  << elo.player(a).wins << "/" << elo.player(a).losses << "/"
                  << elo.player(a).draws << ", champion w/l/d " << elo.player(b).wins << "/"
                  << elo.player(b).losses << "/" << elo.player(b).draws << ", elo "
                  << elo.player(a).elo << std::endl;

        assert(elo.player(a).wins == arm.wins_a);
        assert(elo.player(a).losses == arm.losses_a);
        assert(elo.player(a).draws == arm.draws_a);
        // The opponent is scored on the same match, so its counters mirror a's.
        assert(elo.player(b).wins == arm.losses_a);
        assert(elo.player(b).losses == arm.wins_a);
        assert(elo.player(b).draws == arm.draws_a);
        assert(elo.player(a).games == 1 && elo.player(b).games == 1);

        // Equal starting ratings make the expected score exactly half, so the
        // rating move is k * (score - 0.5) whichever bracket the score lands in.
        const double delta = 32.0 * (arm.score - 0.5);
        assert(std::abs(elo.player(a).elo - (1500.0 + delta)) < 1e-9);
        assert(std::abs(elo.player(b).elo - (1500.0 - delta)) < 1e-9);
    }

    std::cout << "test_elo_scores_a_majority_match_as_a_win passed" << std::endl;
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

// R86 Q3: collect() with augmentation evaluates all eight board orientations,
// but slots 0..24 are read off the whole board and are identical under every
// symmetry - the fix hoists that part out of the variant loop and recomputes
// only 25..51 per orientation. The reorder must be behaviour-preserving: every
// kept sample has to equal extract_features(transform_board(board_i, v), side)
// for one of the eight v, and the identity orientation must still be kept.
void test_augmented_samples_match_manual_transforms() {
    // Six plies on purpose: the board is asymmetric from ply 1 on ((7,8) and
    // (7,6) are mirror images, but (8,8) and (5,5) are not), so the late plies
    // really carry all eight orientations, not a deduplicated subset.
    GameRecord record;
    record.winner = BLACK;
    record.reason = GameEndReason::FIVE;
    const Position line[6] = {{7, 7}, {7, 8}, {8, 8}, {6, 6}, {5, 5}, {7, 6}};
    for (int i = 0; i < 6; ++i) {
        MoveRecord entry;
        entry.move = Move(line[i].row, line[i].col, i % 2 == 0 ? BLACK : WHITE);
        entry.score = 0;
        entry.depth = 4;
        entry.nodes = 100;
        entry.elapsed_ms = 5;
        entry.method = "alphabeta";
        record.moves.push_back(entry);
    }
    record.total_nodes = 600;
    record.max_depth = 4;

    // boards[i] is the position before record.moves[i] is played.
    std::vector<Board> boards(record.moves.size() + 1);
    for (size_t i = 0; i < record.moves.size(); ++i) {
        boards[i + 1] = boards[i];
        boards[i + 1].place_stone(record.moves[i].move.pos.row, record.moves[i].move.pos.col,
                                  record.moves[i].move.color);
    }

    DataCollector collector;
    const std::vector<TrainingSample> augmented = collector.collect(record, 9090, true);
    assert(!augmented.empty());

    std::map<int, std::vector<std::vector<float>>> kept;
    for (const auto& sample : augmented) {
        assert(sample.move_index >= 0 && sample.move_index < static_cast<int>(record.moves.size()));
        assert(static_cast<int>(sample.features.size()) == DataCollector::FEATURE_COUNT);
        kept[sample.move_index].push_back(sample.features);
    }
    assert(static_cast<int>(kept.size()) == static_cast<int>(record.moves.size()));

    for (const auto& entry : kept) {
        const int side = record.moves[entry.first].move.color;
        std::vector<std::vector<float>> want;
        for (int v = 0; v < SYMMETRY_COUNT; ++v) {
            want.push_back(collector.extract_features(transform_board(boards[entry.first], v), side));
        }
        assert(entry.second.size() >= 1 && entry.second.size() <= static_cast<size_t>(SYMMETRY_COUNT));
        // Every kept sample is one of the eight orientations, bit for bit.
        for (const auto& features : entry.second) {
            assert(std::find(want.begin(), want.end(), features) != want.end());
        }
        // The identity orientation is the board itself: it must be kept, and
        // every distinct orientation must survive as a sample of its own.
        assert(std::find(entry.second.begin(), entry.second.end(), want[0]) != entry.second.end());
        for (const auto& features : want) {
            assert(std::find(entry.second.begin(), entry.second.end(), features) != entry.second.end());
        }
    }

    // Anti-overreach: without augmentation there is exactly one sample per ply,
    // equal to the untransformed board's full 52-slot vector.
    const std::vector<TrainingSample> plain = collector.collect(record, 9091, false);
    assert(plain.size() == record.moves.size());
    for (const auto& sample : plain) {
        assert(sample.features == collector.extract_features(boards[sample.move_index], record.moves[sample.move_index].move.color));
    }

    std::cout << "test_augmented_samples_match_manual_transforms passed" << std::endl;
}

void test_hill_climbing() {
    CoefficientPair base;  // 两侧同值，就是改之前那一套默认系数
    const double initial = deviation_score(base);

    HillClimbingConfig settings;
    settings.max_iterations = 80;
    settings.stagnation_limit = 60;

    HillClimbingOptimizer optimizer(pair_deviation_score, settings, 4242);
    int iterations = 0;
    double best = 0.0;
    const CoefficientPair result = optimizer.run(base, iterations, best);

    assert(best > initial);
    assert(deviation_score(result) == best);
    assert(iterations >= 1 && iterations <= settings.max_iterations);
    assert(optimizer.evaluations() == iterations + 1);
    assert(optimizer.best_history().size() == static_cast<size_t>(iterations) + 1);
    assert(optimizer.best_history().front() <= optimizer.best_history().back());

    // Same seed, same walk.
    HillClimbingOptimizer repeat(pair_deviation_score, settings, 4242);
    int repeat_iterations = 0;
    double repeat_best = 0.0;
    const CoefficientPair repeat_result = repeat.run(base, repeat_iterations, repeat_best);
    assert(CoefficientVector::to_vector(repeat_result) ==
           CoefficientVector::to_vector(result));

    // A neighbour step rescales exactly one coordinate, and the result is still a
    // legal parameter set. Probed on a *positional* gene on purpose: the threat
    // ladder has a cross-side band (指导书 §4 丁-3 注意 5), so a probe there moves the
    // other colour too, and that is the next arm down.
    const std::vector<int> genes = CoefficientVector::to_vector(base);
    assert(genes.size() == static_cast<size_t>(PAIR_GENE_COUNT));
    const std::vector<int> grown =
        HillClimbingOptimizer::neighbor(genes, black_gene(TunableParam::POSITION_WEIGHT_SCALE), 1.6);
    assert(grown.size() == genes.size());
    assert(grown[black_gene(TunableParam::POSITION_WEIGHT_SCALE)] == 13);
    for (size_t i = 0; i < genes.size(); ++i) {
        if (i == black_gene(TunableParam::POSITION_WEIGHT_SCALE)) continue;
        assert(grown[i] == genes[i]);
    }

    // 一条威胁阶梯上的探针会拖动另一侧：白侧被拉进「与黑侧 ±20 % 同阶」的那条带里，
    // 因为带是按黑侧为基准算的（丁-3 让「白方是另一个引擎」这件事不可能被训练出来）。
    const std::vector<int> dragged =
        HillClimbingOptimizer::neighbor(genes, black_gene(TunableParam::LIVE_FOUR), 1.6);
    assert(dragged[black_gene(TunableParam::LIVE_FOUR)] == 16000);
    assert(dragged[white_gene(TunableParam::LIVE_FOUR)] == 12800);

    // Scaling the smallest threat down cannot go to zero or below its bound. Named
    // by gene rather than by offset from the end: the surface grew, and the last
    // two coordinates are candidate widths now, not threats.
    const std::vector<int> shrunk = HillClimbingOptimizer::neighbor(
        genes, black_gene(TunableParam::SLEEPY_TWO), 0.001);
    assert(shrunk[black_gene(TunableParam::SLEEPY_TWO)] >= 1);
    // An out-of-range coordinate is left alone instead of reading past the end.
    assert(HillClimbingOptimizer::neighbor(genes, genes.size(), 2.0) == genes);

    CoefficientPair from_neighbour = CoefficientVector::pair_from_vector(grown);
    CoefficientVector::sanitize(from_neighbour);
    assert(CoefficientVector::to_vector(from_neighbour) == grown);

    // Stagnation ends the search early when nothing improves.
    HillClimbingConfig stuck_settings;
    stuck_settings.max_iterations = 200;
    stuck_settings.stagnation_limit = 5;
    HillClimbingOptimizer stuck([](const CoefficientPair&) { return 1.0; },
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
    CoefficientPair base;  // 两侧同值 = 改之前的默认一套系数
    const double initial = deviation_score(base);

    GeneticConfig settings;
    settings.population_size = 10;
    settings.max_generations = 12;
    settings.stagnation_generations = 20;

    GeneticOptimizer optimizer(pair_deviation_score, settings, 20261011);
    int generations = 0;
    double best = 0.0;
    const CoefficientPair result = optimizer.run(base, generations, best);

    assert(best > initial);
    assert(deviation_score(result) >= best - 1e-12);
    assert(generations >= 1 && generations <= settings.max_generations);
    assert(optimizer.population().size() == static_cast<size_t>(settings.population_size));
    assert(optimizer.evaluations() ==
           settings.population_size + generations * (settings.population_size - 1));
    // Elitism: the best member of the final population is the returned answer.
    double strongest = 0.0;
    for (const auto& member : optimizer.population()) {
        strongest = std::max(strongest, member.fitness);
        // Every member is a legal parameter set, bounds, both colours' ladders
        // and the cross-side band included.
        CoefficientPair check = member.coefficients;
        CoefficientVector::sanitize(check);
        assert(CoefficientVector::to_vector(check) == member.genes);
    }
    assert(std::abs(strongest - best) < 1e-12);

    GeneticOptimizer repeat(pair_deviation_score, settings, 20261011);
    int repeat_generations = 0;
    double repeat_best = 0.0;
    const CoefficientPair repeat_result = repeat.run(base, repeat_generations, repeat_best);
    assert(CoefficientVector::to_vector(repeat_result) == CoefficientVector::to_vector(result));

    // Crossover keeps the vector length and stays within the convex hull of the
    // two parents before sanitising.
    std::mt19937_64 rng(7);
    const std::vector<int> lhs = CoefficientVector::to_vector(base);
    const std::vector<int> rhs =
        CoefficientVector::to_vector(CoefficientPair::shared(target_coefficients()));
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
    GeneticOptimizer flat([](const CoefficientPair&) { return 0.25; }, flat_settings, 5);
    int flat_generations = 0;
    double flat_best = 0.0;
    flat.run(base, flat_generations, flat_best);
    assert(flat.converged());
    assert(std::abs(flat_best - 0.25) < 1e-12);
    assert(flat_generations == flat_settings.stagnation_generations);

    std::cout << "test_genetic passed" << std::endl;
}

// R87: one half of the seeded population is redrawn across each bound's whole
// box. The old draw was min + noise * span with noise in (-0.5, 0.5), so every
// reseeded gene landed in the lower half of its range - for double_four the old
// maximum is 50000 - 1, and the top half of every box was unreachable at
// seeding time, which is exactly the region a wider search of these bounds
// needs to start from.
void test_genetic_seed_covers_the_whole_box() {
    CoefficientPair base;  // 两侧同值的默认一套，就是改之前的那个 base
    const std::vector<int> base_genes = CoefficientVector::to_vector(base);

    GeneticConfig settings;
    settings.population_size = 12;
    // Zero generations leaves run() with nothing to evolve, so population() is
    // exactly what seed_population() wrote.
    settings.max_generations = 0;

    const std::vector<uint64_t> seeds = {202, 505};
    // Both halves of every gene's box, visited by the reseeded half of the
    // population across both seeds. The version this replaced asked the question
    // about double_four only - the one gene the R87 bug pinned to its lower half -
    // and paired it with "no draw may land exactly on min", a truncation
    // coincidence the two seeds were swept to avoid. That pairing stopped being
    // neutral once the surface grew a bound four values wide: a quarter of that
    // gene's draws hit min by arithmetic, and hitting min is not a failure to
    // reach the end of the box. Half-coverage is the property, and it is now
    // checked for every gene - which still guards R87, because the formula that
    // round deleted left every gene inside its own lower half, not double_four
    // alone.
    //
    // 丁-3 asks the question of the **reference** side only (genes 0..13). The white
    // half's threat ladder is band-limited to ±20 % of black's, so its whole box is
    // not reachable by construction - that is the invariant, not a shortfall.
    bool low_seen[TUNABLE_COUNT] = {};
    bool high_seen[TUNABLE_COUNT] = {};

    // The most one inner step can add to a gene is span / 8 (noise is bounded
    // by 0.5 and then the 0.25 factor halves it again), and a nudge member
    // spends one step per gene. Ladder repairs only move a value towards
    // another gene that is itself within this budget of its own base.
    long nudge_budget = 0;
    for (int g = 0; g < TUNABLE_COUNT; ++g) {
        const ParamBound bound = CoefficientVector::bound_for(static_cast<TunableParam>(g));
        nudge_budget += (bound.max_value - bound.min_value) / 8;
    }
    std::cout << "[r87r] nudge_budget=" << nudge_budget << std::endl;

    for (uint64_t seed : seeds) {
        GeneticOptimizer optimizer([](const CoefficientPair&) { return 0.0; },
                                   settings, seed);
        int generations = -1;
        double best = -1.0;
        optimizer.run(base, generations, best);
        assert(generations == 0);
        assert(optimizer.population().size() ==
               static_cast<size_t>(settings.population_size));

        // Seat 0 is the founder: the hand-tuned base itself, gene for gene.
        assert(optimizer.population()[0].genes == base_genes);

        for (int i = 1; i < settings.population_size; ++i) {
            const std::vector<int>& genes = optimizer.population()[i].genes;
            const bool reseeded = i % 2 == 1;
            std::cout << "[r87r] seed=" << seed << " member[" << i << "]"
                      << (reseeded ? " reseed:" : " nudge: ");
            for (int v : genes) std::cout << ' ' << v;
            std::cout << std::endl;
            if (reseeded) {
                for (int g = 0; g < TUNABLE_COUNT; ++g) {
                    const ParamBound bound =
                        CoefficientVector::bound_for(static_cast<TunableParam>(g));
                    assert(genes[g] >= bound.min_value && genes[g] <= bound.max_value);
                    const int middle =
                        bound.min_value + (bound.max_value - bound.min_value) / 2;
                    if (genes[g] < middle) low_seen[g] = true;
                    if (genes[g] > middle) high_seen[g] = true;
                }
            } else {
                for (int g = 0; g < TUNABLE_COUNT; ++g) {
                    assert(std::abs(genes[g] - base_genes[g]) <= nudge_budget);
                }
            }
        }
    }
    for (int g = 0; g < TUNABLE_COUNT; ++g) {
        if (!low_seen[g] || !high_seen[g]) {
            std::cout << "[r87r] gene " << g << " missed low=" << low_seen[g]
                      << " high=" << high_seen[g] << std::endl;
        }
        assert(low_seen[g]);
        assert(high_seen[g]);
    }

    std::cout << "test_genetic_seed_covers_the_whole_box passed" << std::endl;
}

// B6: empty steps (candidate == current) must neither count as evaluations nor
// trigger stagnation. A multiplier of 1.0 with sanitize makes every neighbor a
// no-op for the base genes.
void test_hill_climbing_empty_steps() {
    CoefficientPair base;
    const std::vector<int> genes = CoefficientVector::to_vector(base);
    for (size_t g = 0; g < genes.size(); ++g) {
        assert(HillClimbingOptimizer::neighbor(genes, g, 1.0) == genes);
    }

    HillClimbingConfig settings;
    settings.max_iterations = 40;
    settings.stagnation_limit = 12;
    settings.multipliers = {1.0};
    HillClimbingOptimizer optimizer([](const CoefficientPair&) { return 0.0; },
                                    settings, 4242);
    int iterations = 0;
    double best = 0.0;
    optimizer.run(base, iterations, best);

    assert(iterations == 0);
    assert(optimizer.evaluations() == 1);
    assert(optimizer.converged() == false);
    assert(optimizer.best_history().size() == 1);

    std::cout << "test_hill_climbing_empty_steps passed" << std::endl;
}

// B6: invariant iterations_run == evaluations - 1 == best_history.size() - 1
// holds when some steps are empty (mixed multipliers 1.0 and 1.6).
void test_hill_climbing_effective_steps() {
    CoefficientPair base;
    HillClimbingConfig settings;
    settings.max_iterations = 40;
    settings.stagnation_limit = 1000;
    settings.multipliers = {1.0, 1.6};
    HillClimbingOptimizer optimizer([](const CoefficientPair& c) {
        long sum = 0;
        for (int v : CoefficientVector::to_vector(c)) sum += v;
        return static_cast<double>(sum);
    }, settings, 7);
    int iterations = 0;
    double best = 0.0;
    optimizer.run(base, iterations, best);

    // Effective steps < raw loop iterations (some 1.0 multipliers produced no-ops).
    assert(iterations < settings.max_iterations);
    // Pin the invariant.
    assert(iterations == optimizer.evaluations() - 1);
    assert((size_t)iterations == optimizer.best_history().size() - 1);
    assert(optimizer.converged() == false);

    std::cout << "test_hill_climbing_effective_steps passed" << std::endl;
}

// B7: elitism means the incumbent is appended after evaluate(), so the elite
// slot itself never gets scored a second time. Scoring by gene-vector was
// dropped: a produced child can coincide with a vector that is already in the
// population, and that child is scored again - that is what the current
// implementation does (measured on c14854a: seed=3 a non-winner vector was
// scored 4 times, seed=11 the founder vector 3 times, seed=4242 the winning
// vector 2 times), so calls-per-vector measured vector uniqueness rather than
// elite re-scoring, and it was not independent of the seeding distribution.
// What stays observable here is the elite slot and the evaluation count.
void test_genetic_elite_single_evaluation() {
    CoefficientPair base;
    GeneticConfig settings;
    settings.population_size = 8;
    settings.max_generations = 1;
    settings.stagnation_generations = 50;
    settings.crossover_ratio = 0.85;
    settings.mutation_rate = 0.0;

    auto sum_fitness = [](const CoefficientPair& c) -> double {
        long sum = 0;
        for (int v : CoefficientVector::to_vector(c)) sum += v;
        return static_cast<double>(sum);
    };

    GeneticOptimizer optimizer(sum_fitness, settings, 3);
    int generations = 0;
    double best = 0.0;
    CoefficientPair winner = optimizer.run(base, generations, best);

    // Population is full (elite appended, not pre-filled before evaluate).
    assert(optimizer.population().size() == static_cast<size_t>(settings.population_size));

    // The last slot is the incumbent carried across the generation: it holds the
    // fitness run() reports as best, and its coefficients are the returned
    // champion, without having been scored again by the append.
    assert(optimizer.population().back().fitness == best);
    assert(CoefficientVector::to_vector(optimizer.population().back().coefficients) ==
           CoefficientVector::to_vector(winner));

    // Total evaluations = population_size + generations * (population_size - 1).
    assert(optimizer.evaluations() ==
           settings.population_size + generations * (settings.population_size - 1));

    std::cout << "test_genetic_elite_single_evaluation passed" << std::endl;
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
    trainer.set_fitness_override(pair_deviation_score);

    const CoefficientPair before = trainer.champion();
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
    CoefficientPair from_file;
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
    trainer.set_fitness_override([](const CoefficientPair&) { return 0.4; });

    const CoefficientPair before = trainer.champion();
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
    trainer.set_fitness_override(pair_deviation_score);

    const TrainerRound round = trainer.run_round();
    assert(round.accepted);                       // the injected objective promotes
    // 丁-3：规则常量按色带过去——两侧都是启动那一份，因为帽取的是两侧 `five` 的
    // 较小者，一个「黑方 88888 / 白方 100000」的冠军会把将杀带切成两种。
    assert(round.candidate.black.five == 88888);
    assert(round.candidate.black.forbidden_penalty == 77777);
    assert(round.candidate.white.five == 88888);
    assert(round.candidate.white.forbidden_penalty == 77777);
    assert(trainer.champion().black.five == 88888);
    assert(trainer.champion().black.forbidden_penalty == 77777);
    assert(trainer.champion().white.five == 88888);
    assert(trainer.champion().white.forbidden_penalty == 77777);

    std::string error;
    CoefficientPair from_file;
    assert(CoefficientVector::load(config.params_path, from_file, error));
    assert(from_file.black.five == 88888);
    assert(from_file.black.forbidden_penalty == 77777);
    assert(from_file.white.five == 88888);
    assert(from_file.white.forbidden_penalty == 77777);

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
    trainer.set_fitness_override(pair_deviation_score);
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

// Round 79, A5: TrainingSample::from_json now requires the label (move object,
// numeric value, numeric weight) and the 52-long feature vector, so a damaged
// line is refused whole instead of training on a half-read sample. The writer's
// own payload must still read back.
void test_sample_from_json_rejects_damaged() {
    const auto features = [](int count) {
        std::string out = "[";
        for (int i = 0; i < count; ++i) {
            if (i) out += ",";
            out += "0";
        }
        return out + "]";
    };
    const int n = DataCollector::FEATURE_COUNT;
    const std::string move = "\"move\":{\"row\":7,\"col\":7,\"color\":\"black\"}";

    TrainingSample sample;
    std::string error;
    const std::string ok =
        "{\"features\":" + features(n) + ",\"value\":0.5,\"weight\":1.0," + move + "}";
    assert(TrainingSample::from_json(ok, sample, error) && error.empty());
    assert(static_cast<int>(sample.features.size()) == n);
    assert(sample.move == Move(7, 7, BLACK));
    assert(sample.value == 0.5f && sample.weight == 1.0f);

    // No move at all: the features describe a label that is missing.
    error.clear();
    assert(!TrainingSample::from_json(
        "{\"features\":" + features(n) + ",\"value\":0.5,\"weight\":1.0}", sample, error));
    assert(error.find("move") != std::string::npos);

    // move present but not an object.
    error.clear();
    assert(!TrainingSample::from_json(
        "{\"features\":" + features(n) + ",\"value\":0.5,\"weight\":1.0,\"move\":5}", sample, error));
    assert(error.find("move") != std::string::npos);

    // Missing value / weight, and their non-numeric shapes.
    error.clear();
    assert(!TrainingSample::from_json(
        "{\"features\":" + features(n) + ",\"weight\":1.0," + move + "}", sample, error));
    assert(error.find("value") != std::string::npos);
    assert(!TrainingSample::from_json(
        "{\"features\":" + features(n) + ",\"value\":\"high\",\"weight\":1.0," + move + "}",
        sample, error));
    assert(error.find("value") != std::string::npos);
    error.clear();
    assert(!TrainingSample::from_json(
        "{\"features\":" + features(n) + ",\"value\":0.5," + move + "}", sample, error));
    assert(error.find("weight") != std::string::npos);
    assert(!TrainingSample::from_json(
        "{\"features\":" + features(n) + ",\"value\":0.5,\"weight\":null," + move + "}",
        sample, error));
    assert(error.find("weight") != std::string::npos);

    // A feature vector of the wrong length is another model layout.
    error.clear();
    assert(!TrainingSample::from_json(
        "{\"features\":" + features(n - 1) + ",\"value\":0.5,\"weight\":1.0," + move + "}",
        sample, error));
    assert(error.find("51 features, needs 52") != std::string::npos);
    assert(!TrainingSample::from_json(
        "{\"features\":" + features(n + 1) + ",\"value\":0.5,\"weight\":1.0," + move + "}",
        sample, error));
    assert(error.find("53 features, needs 52") != std::string::npos);

    // Positive round trip: the writer's own payload reads back whole.
    TrainingSample written;
    written.features.assign(n, 0.25f);
    written.value = -1.0f;
    written.weight = 0.5f;
    written.move = Move(3, 12, WHITE);
    written.search_score = 42;
    written.search_depth = 3;
    written.nodes = 99;
    written.game_id = 7;
    written.move_index = 5;
    TrainingSample back;
    error.clear();
    assert(TrainingSample::from_json(written.to_json(), back, error) && error.empty());
    assert(static_cast<int>(back.features.size()) == n);
    assert(back.features[0] == 0.25f && back.features[n - 1] == 0.25f);
    assert(back.move == Move(3, 12, WHITE));
    assert(back.value == -1.0f && back.weight == 0.5f);
    assert(back.search_score == 42 && back.search_depth == 3 && back.nodes == 99);
    assert(back.game_id == 7 && back.move_index == 5);

    std::cout << "test_sample_from_json_rejects_damaged passed" << std::endl;
}

// R86 P3: the move colour is the label's side, so only the writer's own two
// spellings may read back. Anything else - "BLACK", "b", the empty string, a
// CJK word - used to fall through to WHITE and load a sample whose label says
// the wrong player moved. The coordinate gates above the colour read are
// pinned alongside, so a fix cannot trade one for another.
void test_sample_move_color_must_be_exact() {
    const auto features = [](int count) {
        std::string out = "[";
        for (int i = 0; i < count; ++i) {
            if (i) out += ",";
            out += "0";
        }
        return out + "]";
    };
    const int n = DataCollector::FEATURE_COUNT;
    const auto sample_text = [&](const std::string& color_json) {
        return "{\"features\":" + features(n) + ",\"value\":0.5,\"weight\":1.0," +
               "\"move\":{\"row\":7,\"col\":7,\"color\":" + color_json + "}}";
    };

    TrainingSample sample;
    std::string error;
    assert(TrainingSample::from_json(sample_text("\"black\""), sample, error) && error.empty());
    assert(sample.move == Move(7, 7, BLACK));
    assert(sample.move.color == BLACK);
    error.clear();
    assert(TrainingSample::from_json(sample_text("\"white\""), sample, error) && error.empty());
    assert(sample.move == Move(7, 7, WHITE));
    assert(sample.move.color == WHITE);

    for (const char* bad : {"\"BLACK\"", "\"b\"", "\"\"", "\"\\u767d\""}) {
        error.clear();
        assert(!TrainingSample::from_json(sample_text(bad), sample, error));
        assert(!error.empty());
    }

    // The coordinate gates stay where they were: an off-board row and a 64-bit
    // row a truncating read would fold onto the board both still refuse.
    error.clear();
    assert(!TrainingSample::from_json(
        "{\"features\":" + features(n) +
            ",\"value\":0.5,\"weight\":1.0,"
            "\"move\":{\"row\":15,\"col\":7,\"color\":\"black\"}}",
        sample, error));
    error.clear();
    assert(!TrainingSample::from_json(
        "{\"features\":" + features(n) +
            ",\"value\":0.5,\"weight\":1.0,"
            "\"move\":{\"row\":4294967303,\"col\":7,\"color\":\"black\"}}",
        sample, error));

    std::cout << "test_sample_move_color_must_be_exact passed" << std::endl;
}

// R97 BB3: game_id and move_index are the sample's identity, and samples.jsonl
// leans on (game_id, move_index) being unique. The reader used to fall back to
// the -1 default whenever the key was written but unreadable, so a batch of
// such lines collapsed into one phantom game carrying the same index. A key that
// is absent stays legal - the writer simply has no id to give - but a key that
// is present is a claim and must read back.
void test_sample_refuses_unreadable_identity_keys() {
    const auto features = [](int count) {
        std::string out = "[";
        for (int i = 0; i < count; ++i) {
            if (i) out += ",";
            out += "0";
        }
        return out + "]";
    };
    const int n = DataCollector::FEATURE_COUNT;
    const std::string move = "\"move\":{\"row\":7,\"col\":7,\"color\":\"black\"}";
    const auto sample_with = [&](const std::string& tail) {
        return "{\"features\":" + features(n) + ",\"value\":0.5,\"weight\":1.0," +
               move + "," + tail + "}";
    };

    TrainingSample sample;
    std::string error;

    // 7.0 is a float, "7" a string, null nothing, 4294967297 an integer that
    // does not fit an int: all four state an identity the reader cannot carry.
    const char* bad[] = {
        "\"game_id\":7.0",
        "\"game_id\":\"7\"",
        "\"game_id\":null",
        "\"game_id\":4294967297",
        "\"move_index\":5.0",
        "\"move_index\":\"five\"",
        "\"move_index\":null",
        "\"move_index\":-2147483649",
    };
    for (const char* key : bad) {
        error.clear();
        const bool ok = TrainingSample::from_json(sample_with(key), sample, error);
        assert(!ok);
        assert(!error.empty());
    }

    // Both keys absent stays the ordinary case: a legal sample, and the -1
    // default for each identity field.
    error.clear();
    assert(TrainingSample::from_json(
        "{\"features\":" + features(n) + ",\"value\":0.5,\"weight\":1.0," + move + "}",
        sample, error) && error.empty());
    assert(sample.game_id == -1 && sample.move_index == -1);

    // One readable key next to a broken one still refuses the line, and the
    // readable id does not smuggle the row through.
    error.clear();
    assert(!TrainingSample::from_json(
        sample_with("\"game_id\":7,\"move_index\":3.5"), sample, error));
    error.clear();
    assert(!TrainingSample::from_json(
        sample_with("\"game_id\":7.0,\"move_index\":3"), sample, error));

    // The positive side of the same door: readable integers land on their own
    // values, including the -1 the writer emits for an unlabelled sample.
    error.clear();
    assert(TrainingSample::from_json(
        sample_with("\"game_id\":7,\"move_index\":5"), sample, error) && error.empty());
    assert(sample.game_id == 7 && sample.move_index == 5);
    error.clear();
    assert(TrainingSample::from_json(
        sample_with("\"game_id\":-1,\"move_index\":-1"), sample, error) && error.empty());
    assert(sample.game_id == -1 && sample.move_index == -1);

    // The engine numbers keep the fallback behaviour they always had: a broken
    // search_score is not an identity claim, so it stays a default.
    error.clear();
    assert(TrainingSample::from_json(
        sample_with("\"search_score\":\"loud\""), sample, error) && error.empty());
    assert(sample.search_score == 0);

    std::cout << "test_sample_refuses_unreadable_identity_keys passed" << std::endl;
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
// (trainer.cpp:69), SelfPlay::run only reports INVALID for an illegal prefix
// (self_play.cpp:38-48), an external stop (:137-143) or an engine illegal move
// (:77-88), and every move generator filters forbidden points. So the out-
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
    assert(trainer.play_game(black, white, 1, {}, &played) == 0.5);
    assert(played);
    assert(trainer.games_played() == 1);
    SearchEngine black_b;
    SearchEngine white_b;
    assert(trainer.play_game(black_b, white_b, 2, {}) == 0.5);
    assert(trainer.games_played() == 2);

    // An explicit null out-parameter is not a crash and not a behaviour change.
    SearchEngine black_c;
    SearchEngine white_c;
    assert(trainer.play_game(black_c, white_c, 1, {}, nullptr) == 0.5);
    assert(trainer.games_played() == 3);

    // Every game in a match is undecided (MOVE_LIMIT), so the average is 0.5
    // and cannot cross the promotion line: an undecided game must never read
    // as a win.
    Trainer matcher(ThreatCoefficients(), config);
    const double match = matcher.play_match(CoefficientPair(), CoefficientPair(), 2);
    assert(match == 0.5);
    assert(match < config.accept_score);
    assert(matcher.games_played() == 2);

    // The injected-objective fast path is untouched by the counting change:
    // win, loss and draw are still decided by the two scores.
    Trainer fast(ThreatCoefficients(), config);
    fast.set_fitness_override([](const CoefficientPair& c) { return c.black.live_four > 15000 ? 1.0 : 0.0; });
    ThreatCoefficients strong_set;
    strong_set.live_four = 20000;
    ThreatCoefficients weak_set;
    weak_set.live_four = 5000;
    const CoefficientPair strong = CoefficientPair::shared(strong_set);
    const CoefficientPair weak = CoefficientPair::shared(weak_set);
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

// Round 112 PP1: the id a data game got used to be games_played_ + 1 + t * 1000, i.e. a
// fixed offset off a counter every round advances (trainer.cpp:74 and :318 add each
// merged record, plus the fitness and verification games). A long run therefore
// re-reaches the offset legitimately: on the round where games_played_ is 1000, worker
// 0's base is exactly the base worker 1 used on the games_played_ = 0 round, so two
// different games are written under one game_id, and samples.jsonl loses the
// (game_id, move_index) identity README:181's append stream needs. The fix claims every
// id from one atomic counter, so claims equal merged records and no id is handed out
// twice.
//
// The same stride also overlaps INSIDE one call once a worker's own count passes 1000:
// games=2001, threads=2 gives per_worker=1000 and remainder=1, so t=0 plays 1001 games
// and its 1001st lands on games_played_ + 1001, the very id t=1's first game uses. That
// arm is deliberately not built here - it needs 2001 games really played - so the
// arithmetic stays in this comment and the case below covers the cross-round reissue,
// which is what a long run actually hits.
//
// The claim is pinned as two disjoint id sets rather than "distinct ids == games played":
// generate_data_games answers with the sample count, not the game count, and a recorded
// game whose plies all fall under the collector's search-depth gate claims an id without
// contributing a row, so the file's id count is a lower bound on the games.
void test_data_game_ids_do_not_repeat_across_rounds() {
    TrainingConfig config = fast_training_config();
    config.search_depth = 2;
    config.timeout_ms = 120;
    config.max_moves = 8;
    config.data_opening_plies = 2;
    config.seed = 13579;
    config.records_path.clear();
    const std::filesystem::path samples = temp_path("pp1_samples.jsonl");
    config.samples_path = samples.string();

    // One ply is owed up to SYMMETRY_COUNT rows: collection augments it into oriented
    // variants that all carry that ply's move_index (see the per-ply counts in
    // test_collector_keeps_the_variant_that_moved_the_last_stone). So a (game_id,
    // move_index) pair repeats by design - what it may never do is name two different
    // games, which is exactly what one id handed out twice amounts to. Judge the pair by
    // its row count, and the two batches by whether they share an id.
    const auto busiest_pair = [](const std::vector<TrainingSample>& rows) {
        std::map<std::pair<int, int>, int> counts;
        for (const auto& row : rows) counts[{row.game_id, row.move_index}] += 1;
        int busiest = 0;
        for (const auto& entry : counts) if (entry.second > busiest) busiest = entry.second;
        return busiest;
    };
    const auto ids_of = [](const std::vector<TrainingSample>& rows) {
        std::set<int> ids;
        for (const auto& row : rows) ids.insert(row.game_id);
        return ids;
    };

    Trainer trainer(ThreatCoefficients(), config);
    assert(trainer.generate_data_games(4, 2) > 0);

    std::vector<TrainingSample> first_batch;
    std::string error;
    assert(DataCollector::read_jsonl(samples.string(), first_batch, error));
    assert(!first_batch.empty());

    // Control, green on both trees: inside one call the two workers still sit 1000
    // apart, so no id is shared and no pair leaves its ply.
    assert(busiest_pair(first_batch) <= SYMMETRY_COUNT);
    const std::set<int> first_ids = ids_of(first_batch);
    assert(first_ids.size() >= 2);
    const size_t first_rows = first_batch.size();

    // A real long-run state, not an invented one: games_played_ only ever climbs, and
    // this is the value where HEAD's t=1 offset from the very first round becomes t=0's
    // offset for this round.
    trainer.games_played_ = 1000;
    assert(trainer.generate_data_games(4, 2) > 0);

    std::vector<TrainingSample> all;
    assert(DataCollector::read_jsonl(samples.string(), all, error));
    assert(all.size() > first_rows);

    // The second batch's rows are exactly what the append added after the first read.
    const std::vector<TrainingSample> second_batch(all.begin() +
                                                       static_cast<long>(first_rows),
                                                   all.end());
    const std::set<int> second_ids = ids_of(second_batch);
    assert(!second_ids.empty());

    // Discriminator: HEAD numbers this batch's worker 0 from 1000 + 1, which is where
    // the first batch's worker 1 already started, so the same game_id names two
    // different games and their plies pile up under one pair.
    for (int id : second_ids) assert(first_ids.count(id) == 0);
    assert(ids_of(all).size() == first_ids.size() + second_ids.size());
    assert(busiest_pair(all) <= SYMMETRY_COUNT);

    std::cout << "test_data_game_ids_do_not_repeat_across_rounds passed" << std::endl;
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
    const double score = trainer.play_match(CoefficientPair(),
                                            CoefficientPair::shared(target_coefficients()), 3);
    assert(trainer.games_played() - before == 4);
    // Eight plies cannot finish a five, so every game is a MOVE_LIMIT draw:
    // four halves over four games pins the normalisation to exactly 0.5.
    assert(std::abs(score - 0.5) < 1e-12);

    const int before_single = trainer.games_played();
    trainer.play_match(CoefficientPair(), CoefficientPair::shared(target_coefficients()), 1);
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
    trainer.set_fitness_override([](const CoefficientPair&) { return 0.5; });
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
    // has to survive byte for byte. The params guard now also drops the failed
    // temporary; the checkpoint guard still documents one being left behind.
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
    // Round 88, S8: a write that died at flush no longer leaves its temporary
    // behind, so the poisoned symlink the test planted is gone. The rename never
    // ran, which the two asserts above still prove. The checkpoint writer below
    // was not given the same cleanup, so it still keeps its failed temporary.
    assert(!std::filesystem::is_symlink(params_tmp));
    assert(!std::filesystem::exists(params_tmp));
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
    // their own refusal, and neither may print the success line (README:155:
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

// Round 100 EE1: the guard behind that boundary stopped one step short. Evaluator
// clamps the aggregate at five - 1 (evaluator.cpp:175-177), so five == 1 is not a
// smaller scale but a cap of zero - evaluate_board answers 0 for every board, the
// engine plays blind, and the server still prints the file as loaded. The other
// side of the boundary (five == 2) must stay legal, and a rejected file must leave
// the caller's coefficients untouched.
void test_params_refuses_a_five_that_flattens_the_scale() {
    ThreatCoefficients sentinel;
    sentinel.five = 31337;
    sentinel.forbidden_penalty = 4242;

    std::string error;
    ThreatCoefficients out = sentinel;
    error.clear();
    assert(!CoefficientVector::from_json_string(R"({"coefficients":{"five":1}})", out, error));
    assert(!error.empty());
    assert(out.five == sentinel.five);
    assert(out.forbidden_penalty == sentinel.forbidden_penalty);
    std::cout << "  [r100ee1] rejected five:1" << std::endl;

    ThreatCoefficients parsed;
    error.clear();
    assert(CoefficientVector::from_json_string(R"({"coefficients":{"five":2}})", parsed, error));
    assert(parsed.five == 2);
    assert(error.empty());
    std::cout << "  [r100ee1] accepted the five:2 boundary" << std::endl;

    // What the guard buys: the same black live four scored with a cap of 0 and a
    // cap of 1. The flattening is reproduced by handing the Evaluator the
    // coefficients directly - the loader no longer lets five == 1 through.
    Board board;
    board.place_stone(7, 4, BLACK);
    board.place_stone(7, 5, BLACK);
    board.place_stone(7, 6, BLACK);
    board.place_stone(7, 7, BLACK);
    board.place_stone(0, 0, WHITE);
    ThreatCoefficients flat;
    flat.five = 1;
    ThreatCoefficients working;
    working.five = 2;
    const int flattened = Evaluator(flat).evaluate_board(board, BLACK);
    const int scored = Evaluator(working).evaluate_board(board, BLACK);
    std::cout << "  [r100ee1] evaluate_board: five=1 -> " << flattened
              << ", five=2 -> " << scored << std::endl;
    assert(flattened == 0);
    assert(scored != 0);

    std::cout << "test_params_refuses_a_five_that_flattens_the_scale passed" << std::endl;
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

// R84 E3: a parameter file is an instruction, so an out-of-range tunable must be
// refused with the key and its allowed range named - sanitize() clamping the
// value and reporting a successful load let a file ask for 500000 and silently
// get 40000. The clamps stay for the optimizers, whose moves are always inside
// the bounds, which is why sanitize_vector / from_vector are pinned below.
void test_params_rejects_out_of_range_tunables() {
    std::string error;
    ThreatCoefficients parsed;

    const struct {
        const char* text;
        const char* key;
        const char* range;
    } cases[] = {
        {R"({"live_four":500000})", "live_four", "[1500, 40000]"},
        {R"({"live_three":-1})", "live_three", "[150, 8000]"},
        {R"({"coefficients":{"rush_four":99999}})", "rush_four", "[300, 15000]"},
        {R"({"position_weight_scale":-50})", "position_weight_scale", "[0, 20]"},
        {R"({"coefficients":{"sleepy_two":0}})", "sleepy_two", "[1, 500]"},
        {R"({"edge_penalty":200})", "edge_penalty", "[0, 150]"},
        {R"({"coefficients":{"cand_width_deep":100}})", "cand_width_deep", "[2, 32]"},
    };
    for (const auto& c : cases) {
        error.clear();
        const bool ok = CoefficientVector::from_json_string(c.text, parsed, error);
        assert(!ok);
        assert(error.find(c.key) != std::string::npos);
        assert(error.find(c.range) != std::string::npos);
        std::cout << "  rejected " << c.text << " -> " << error << std::endl;
    }

    // The boundary values themselves are inside the range and still load.
    error.clear();
    assert(CoefficientVector::from_json_string(R"({"live_four":40000})", parsed, error));
    assert(parsed.live_four == 40000);
    error.clear();
    assert(CoefficientVector::from_json_string(R"({"live_four":1500})", parsed, error));
    assert(parsed.live_four == 1500);

    // A whole valid file (the writer's own output) is unaffected.
    ThreatCoefficients normal;
    normal.live_four = 12000;
    normal.live_three = 480;
    error.clear();
    assert(CoefficientVector::from_json_string(CoefficientVector::to_json_string(normal),
                                               parsed, error));
    assert(parsed.live_four == normal.live_four);
    assert(parsed.live_three == normal.live_three);

    // Anti-regression: sanitize_vector still clamps to the bounds (the genetic
    // and hill-climbing paths depend on it), and from_vector still accepts an
    // out-of-range vector instead of throwing it away.
    std::vector<int> values = CoefficientVector::to_vector(ThreatCoefficients());
    values[static_cast<int>(TunableParam::LIVE_FOUR)] = 500000;
    values[static_cast<int>(TunableParam::LIVE_THREE)] = -9;
    CoefficientVector::sanitize_vector(values);
    assert(values[static_cast<int>(TunableParam::LIVE_FOUR)] == 40000);
    assert(values[static_cast<int>(TunableParam::LIVE_THREE)] == 150);

    std::vector<int> wild = CoefficientVector::to_vector(ThreatCoefficients());
    wild[static_cast<int>(TunableParam::LIVE_FOUR)] = -1;
    const ThreatCoefficients clamped = CoefficientVector::from_vector(wild);
    assert(clamped.live_four == 1500);

    std::cout << "test_params_rejects_out_of_range_tunables passed" << std::endl;
}

// R85 F1: a file's stated values must survive the load. from_json_string used to
// check each key against its own bound and then call sanitize(), whose two
// cross-key repairs (the strength ladder and the double_*/four_three floor)
// silently rewrote file values inside their bounds: live_two=1000 loaded as 30,
// double_four=10000 under live_four=40000 loaded as 40000. The reader must refuse
// such a file with the offending keys named instead.
void test_params_rejects_cross_key_rewrites() {
    std::string error;
    ThreatCoefficients parsed;

    // A. Every adjacent pair of the strength ladder, inverted one at a time, all
    // values inside their own bounds so only the cross-key rule can catch them.
    // The refusal names the first adjacent pair the ladder finds broken, so the
    // only key pinned here is the one this file asks too much of - the sentence
    // around it, and the counterpart it is compared against, are free to change.
    const struct {
        const char* text;
        const char* weaker;
        int asked;
        int neighbour;
    } ladder[] = {
        {R"({"live_four":1500,"rush_four":2000})", "rush_four", 2000, 1500},
        {R"({"rush_four":300,"live_three":8000})", "live_three", 8000, 300},
        {R"({"live_three":150,"sleepy_three":2000})", "sleepy_three", 2000, 150},
        {R"({"sleepy_three":30,"live_two":1000})", "live_two", 1000, 30},
        {R"({"live_two":10,"sleepy_two":500})", "sleepy_two", 500, 10},
    };
    for (const auto& c : ladder) {
        error.clear();
        const bool ok = CoefficientVector::from_json_string(c.text, parsed, error);
        assert(!ok);
        assert(!error.empty());
        assert(error.find(c.weaker) != std::string::npos);
        const std::string asked = std::to_string(c.asked);
        const std::string neighbour = std::to_string(c.neighbour);
        assert(error.find(asked) != std::string::npos ||
               error.find(neighbour) != std::string::npos);
        std::cout << "  refused " << c.text << " -> " << error << std::endl;
    }

    // B. A combination floor below live_four: the file asks for a pair worth less
    // than the single threat it contains, which sanitize() would raise for it.
    const struct {
        const char* text;
        const char* key;
    } floors[] = {
        {R"({"live_four":40000,"double_four":10000})", "double_four"},
        {R"({"live_four":40000,"double_three":5000})", "double_three"},
        {R"({"live_four":40000,"double_four":50000,"double_three":40000,)"
         R"("four_three":5000})", "four_three"},
    };
    for (const auto& c : floors) {
        error.clear();
        const bool ok = CoefficientVector::from_json_string(c.text, parsed, error);
        assert(!ok);
        assert(!error.empty());
        assert(error.find(c.key) != std::string::npos);
        assert(error.find("live_four") != std::string::npos);
        std::cout << "  refused " << c.text << " -> " << error << std::endl;
    }

    // F1b: the refusal is narrowed to the end sanitize would actually rewrite. A
    // written key the defaults would clamp is refused; a default a written key
    // would raise is repaired in place instead - the file stated nothing about it.
    error.clear();
    assert(CoefficientVector::from_json_string(R"({"live_four":40000})", parsed, error));
    assert(parsed.live_four == 40000);
    assert(parsed.double_three == 40000);  // default 30000, raised to live_four
    assert(parsed.four_three == 40000);    // default 20000, raised to live_four
    assert(parsed.double_four == 50000);   // already above, untouched

    const struct {
        const char* text;
        const char* written;
    } written_violations[] = {
        {R"({"live_two":1000})", "live_two"},        // default sleepy_three=100 pins it to 100
        {R"({"rush_four":15000})", "rush_four"},     // default live_four=10000 pins it to 10000
        {R"({"sleepy_two":500})", "sleepy_two"},     // default live_two=50 pins it to 50
        {R"({"double_four":1500})", "double_four"},  // default live_four=10000 raises it
    };
    for (const auto& c : written_violations) {
        error.clear();
        const bool ok = CoefficientVector::from_json_string(c.text, parsed, error);
        assert(!ok);
        assert(error.find(c.written) != std::string::npos);
        std::cout << "  refused " << c.text << " -> " << error << std::endl;
    }

    // Written values the defaults leave alone still load: the line is "would
    // sanitize change what the file wrote", not "does a neighbour differ".
    error.clear();
    assert(CoefficientVector::from_json_string(R"({"live_two":50})", parsed, error));
    assert(parsed.live_two == 50);
    error.clear();
    assert(CoefficientVector::from_json_string(R"({"sleepy_two":10})", parsed, error));
    assert(parsed.sleepy_two == 10);
    error.clear();
    assert(CoefficientVector::from_json_string(R"({"rush_four":1500})", parsed, error));
    assert(parsed.rush_four == 1500);

    // C. Not an over-reach: a file whose ladder holds (several adjacent pairs
    // exactly equal) and whose combinations clear live_four loads with every
    // value byte for byte as written.
    error.clear();
    assert(CoefficientVector::from_json_string(
        R"({"live_four":20000,"rush_four":5000,"live_three":5000,)"
        R"("sleepy_three":500,"live_two":500,"sleepy_two":500,)"
        R"("double_four":60000,"double_three":60000,"four_three":60000})",
        parsed, error));
    assert(parsed.live_four == 20000);
    assert(parsed.rush_four == 5000);
    assert(parsed.live_three == 5000);
    assert(parsed.sleepy_three == 500);
    assert(parsed.live_two == 500);
    assert(parsed.sleepy_two == 500);
    assert(parsed.double_four == 60000);
    assert(parsed.double_three == 60000);
    assert(parsed.four_three == 60000);

    // D. A partial file is still a file: the defaults behind the written keys
    // already satisfy both invariants.
    const ThreatCoefficients defaults;
    error.clear();
    assert(CoefficientVector::from_json_string(R"({"live_four":20000})", parsed, error));
    assert(parsed.live_four == 20000);
    assert(parsed.rush_four == defaults.rush_four);
    assert(parsed.double_four == defaults.double_four);
    assert(parsed.sleepy_two == defaults.sleepy_two);

    error.clear();
    const std::string default_text = CoefficientVector::to_json_string(defaults);
    assert(CoefficientVector::from_json_string(default_text, parsed, error));
    assert(CoefficientVector::to_vector(parsed) == CoefficientVector::to_vector(defaults));

    // E. Anti-regression: the optimizers' silent repairs stay in place - they feed
    // candidate vectors, not files, and have to be clamped rather than refused.
    ThreatCoefficients inverted;
    inverted.live_two = 1000;
    inverted.sleepy_three = 30;
    CoefficientVector::sanitize(inverted);
    assert(inverted.live_two == 30);
    assert(inverted.sleepy_three == 30);

    std::vector<int> values = CoefficientVector::to_vector(ThreatCoefficients());
    values[static_cast<int>(TunableParam::LIVE_TWO)] = 1000;
    values[static_cast<int>(TunableParam::SLEEPY_THREE)] = 30;
    CoefficientVector::sanitize_vector(values);
    assert(values[static_cast<int>(TunableParam::LIVE_TWO)] == 30);

    std::vector<int> raised = CoefficientVector::to_vector(ThreatCoefficients());
    raised[static_cast<int>(TunableParam::LIVE_FOUR)] = 40000;
    raised[static_cast<int>(TunableParam::DOUBLE_FOUR)] = 10000;
    const ThreatCoefficients clamped = CoefficientVector::from_vector(raised);
    assert(clamped.double_four == 40000);
    assert(clamped.double_three >= 40000);
    assert(clamped.four_three >= 40000);

    // F. Round trip: what to_json_string writes is exactly what from_json_string
    // reads back, for the legal file above and for the built-in defaults.
    ThreatCoefficients legal;
    legal.live_four = 20000;
    legal.rush_four = 5000;
    legal.live_three = 5000;
    legal.sleepy_three = 500;
    legal.live_two = 500;
    legal.sleepy_two = 500;
    legal.double_four = 60000;
    legal.double_three = 60000;
    legal.four_three = 60000;
    const std::string legal_text = CoefficientVector::to_json_string(legal);
    error.clear();
    assert(CoefficientVector::from_json_string(legal_text, parsed, error));
    assert(CoefficientVector::to_vector(parsed) == CoefficientVector::to_vector(legal));
    assert(CoefficientVector::to_json_string(parsed) == legal_text);
    assert(CoefficientVector::to_json_string(
               CoefficientVector::from_vector(CoefficientVector::to_vector(legal))) == legal_text);

    std::cout << "test_params_rejects_cross_key_rewrites passed" << std::endl;
}

// 设计文档 2.5.1 / 2.5.2 的参数面：edge_penalty 与 distance_to_stones 是 A5 就
// 存在的两个字段，但它们既不在枚举里也不在键表里，所以 trained 值出不了
// params.save 这一步；两枚宽度是阶段 B1 的候选分层要读的。四条新键都是可选的，
// 老文件照旧加载。
void test_params_legacy_12_key_file_still_loads() {
    static const char* legacy =
        R"({"coefficients":{"five":100000,"live_four":10000,"rush_four":1000,)"
        R"("live_three":500,"sleepy_three":100,"live_two":50,"sleepy_two":10,)"
        R"("double_four":50000,"double_three":30000,"four_three":20000,)"
        R"("forbidden_penalty":50000,"position_weight_scale":8}})";
    ThreatCoefficients parsed;
    std::string error;
    assert(CoefficientVector::from_json_string(legacy, parsed, error));
    assert(error.empty());
    const ThreatCoefficients defaults;
    assert(parsed.live_four == 10000);
    assert(parsed.position_weight_scale == 8);
    assert(parsed.edge_penalty == defaults.edge_penalty);
    assert(parsed.distance_to_stones == defaults.distance_to_stones);
    assert(parsed.cand_width_root == defaults.cand_width_root);
    assert(parsed.cand_width_deep == defaults.cand_width_deep);

    // And the writer still names every one of the old twelve, so a reader that is
    // not this one keeps finding them.
    const std::string written = CoefficientVector::to_json_string(defaults);
    for (const char* key : {"five", "live_four", "rush_four", "live_three", "sleepy_three",
                            "live_two", "sleepy_two", "double_four", "double_three",
                            "four_three", "forbidden_penalty", "position_weight_scale"}) {
        assert(written.find(key) != std::string::npos);
    }

    std::cout << "test_params_legacy_12_key_file_still_loads passed" << std::endl;
}

// The four genes survive the writer, the reader and the vector view. On the tree
// before this change the file is refused outright ("unknown coefficient key:
// edge_penalty"), and a params.save of trained position terms silently wrote
// nothing about them, so the next load restored the compiled-in 100 / 1.
void test_params_position_and_width_genes_round_trip() {
    ThreatCoefficients trained;
    trained.edge_penalty = 150;  // the top of its bound
    trained.distance_to_stones = 4;
    trained.cand_width_root = 40;
    trained.cand_width_deep = 16;

    std::string error;
    const std::string text = CoefficientVector::to_json_string(trained);
    ThreatCoefficients parsed;
    assert(CoefficientVector::from_json_string(text, parsed, error));
    assert(parsed.edge_penalty == 150);
    assert(parsed.distance_to_stones == 4);
    assert(parsed.cand_width_root == 40);
    assert(parsed.cand_width_deep == 16);

    const std::vector<int> genes = CoefficientVector::to_vector(trained);
    assert(genes.size() == static_cast<size_t>(TUNABLE_COUNT));
    assert(genes[static_cast<int>(TunableParam::EDGE_PENALTY)] == 150);
    assert(genes[static_cast<int>(TunableParam::DISTANCE_TO_STONES)] == 4);
    assert(genes[static_cast<int>(TunableParam::CAND_WIDTH_ROOT)] == 40);
    assert(genes[static_cast<int>(TunableParam::CAND_WIDTH_DEEP)] == 16);
    const ThreatCoefficients back = CoefficientVector::from_vector(genes);
    assert(back.edge_penalty == 150);
    assert(back.distance_to_stones == 4);
    assert(back.cand_width_root == 40);
    assert(back.cand_width_deep == 16);

    // Neither new pair joins the strength ladder: a discount or a width is not a
    // threat value, and capping it against live_four would delete it.
    ThreatCoefficients ladder;
    ladder.edge_penalty = 120;
    ladder.distance_to_stones = 3;
    ladder.cand_width_root = 12;
    ladder.cand_width_deep = 5;
    ladder.live_two = 1000;  // past its own neighbour, so the ladder does fire
    CoefficientVector::sanitize(ladder);
    assert(ladder.edge_penalty == 120);
    assert(ladder.distance_to_stones == 3);
    assert(ladder.cand_width_root == 12);
    assert(ladder.cand_width_deep == 5);
    assert(ladder.live_two == ladder.sleepy_three);

    // The bound is set by a structural promise, not by the factor-of-four rule of
    // thumb: eight closed ends (four directions, two ends each) at the largest
    // legal discount still cost less than the cheapest legal 活四. Against 冲四 the
    // promise only holds at the calibrated defaults (test_evaluator.cpp's
    // 8 * edge_penalty < rush_four), because a file may train rush_four down to
    // 300 - and a discount that outprices the smallest legal 冲四 is a parameter
    // set worth reporting, not one the bound can silently absorb.
    const ParamBound edge = CoefficientVector::bound_for(TunableParam::EDGE_PENALTY);
    const ParamBound live_four = CoefficientVector::bound_for(TunableParam::LIVE_FOUR);
    assert(8 * edge.max_value < live_four.min_value);

    // sanitize caps a deep width that out-widens the root; the root is the one
    // layer that pays for the neighbourhood only once.
    ThreatCoefficients wide;
    wide.cand_width_root = 4;
    wide.cand_width_deep = 32;
    CoefficientVector::sanitize(wide);
    assert(wide.cand_width_deep == 4);

    std::cout << "test_params_position_and_width_genes_round_trip passed" << std::endl;
}

// The width relation follows the rule the ladder already obeys: refuse when the
// repair would overwrite a value the file stated, repair in place when the other
// end is an inherited default.
void test_params_refuses_only_stated_width_conflicts() {
    ThreatCoefficients parsed;
    std::string error;

    // A. Both ends stated and the deeper one is wider: refused, both names in it.
    error.clear();
    assert(!CoefficientVector::from_json_string(
        R"({"cand_width_root":10,"cand_width_deep":20})", parsed, error));
    assert(error.find("cand_width_deep") != std::string::npos);
    assert(error.find("cand_width_root") != std::string::npos);
    std::cout << "  refused both-stated -> " << error << std::endl;

    // B. Only the end sanitize would cap is stated: refused as well, because the
    // default root would silently rewrite what the file wrote.
    error.clear();
    assert(!CoefficientVector::from_json_string(R"({"cand_width_deep":30})", parsed, error));
    assert(error.find("cand_width_deep") != std::string::npos);
    assert(error.find("（对端是默认值）") != std::string::npos);

    // C. Only the root is stated, and it is the narrow end: the default deep is
    // what gets capped, and a file that said nothing about deep is not
    // over-reaching. The inherited value is repaired, not refused.
    error.clear();
    assert(CoefficientVector::from_json_string(R"({"cand_width_root":6})", parsed, error));
    assert(parsed.cand_width_root == 6);
    assert(parsed.cand_width_deep == 6);  // default 8, capped to the stated root

    // D. Equal widths are inside the relation, not outside it.
    error.clear();
    assert(CoefficientVector::from_json_string(
        R"({"cand_width_root":32,"cand_width_deep":32})", parsed, error));
    assert(parsed.cand_width_deep == 32);

    // E. Out of range is its own refusal, with the range named (control: the two
    // widths have bounds, so a file asking for 100000 is not silently clamped).
    error.clear();
    assert(!CoefficientVector::from_json_string(R"({"cand_width_root":100})", parsed, error));
    assert(error.find("cand_width_root") != std::string::npos);
    assert(error.find("[2, 64]") != std::string::npos);

    std::cout << "test_params_refuses_only_stated_width_conflicts passed" << std::endl;
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

// Round 90, U1: the optimisers hand candidates back as gene vectors, and
// from_vector default-constructs on purpose, so a candidate arrived at the
// objective carrying the compiled-in five and forbidden_penalty while the
// champion carried this run's. Every pairing measure_fitness makes has to be
// rule-identical on both sides - which is observable as the constants the
// injected objective is called with.
void test_measure_fitness_flattens_rule_constants_for_candidates() {
    TrainingConfig config = fast_training_config();
    config.params_path = temp_path("u1_flat_params.json").string();
    config.checkpoint_path = temp_path("u1_flat_checkpoint.json").string();

    ThreatCoefficients base;
    base.five = 90000;
    base.forbidden_penalty = 40000;
    // The point of the two numbers above is that they are not the defaults.
    assert(base.five != ThreatCoefficients().five);
    assert(base.forbidden_penalty != ThreatCoefficients().forbidden_penalty);

    std::vector<std::pair<int, int>> seen;  // five / forbidden_penalty per call
    Trainer trainer(base, config);
    trainer.set_fitness_override([&seen](const CoefficientPair& coeffs) {
        // 丁-3：两侧各自带着冠军的规则常量进来（`run_round` 逐色搬），所以这里记的
        // 还是「一套」——但要钉住两侧一致，否则将杀带会随颜色变。
        assert(coeffs.white.five == coeffs.black.five);
        assert(coeffs.white.forbidden_penalty == coeffs.black.forbidden_penalty);
        seen.emplace_back(coeffs.black.five, coeffs.black.forbidden_penalty);
        return deviation_score(coeffs);
    });

    trainer.run_round();

    // run_round reads the champion itself (that call is not a candidate and is
    // the same on both trees), then the walk evaluates at least its starting
    // point, so there is more than one pairing to look at.
    assert(seen.size() >= 2);
    for (const std::pair<int, int>& call : seen) {
        assert(call.first == 90000);
        assert(call.second == 40000);
    }

    std::cout << "test_measure_fitness_flattens_rule_constants_for_candidates passed ("
              << seen.size() << " pairings, all " << seen.front().first << "/"
              << seen.front().second << ")" << std::endl;
}

// Anti-overreach: a run that starts from the compiled defaults has nothing to
// flatten, so the pairing stays exactly what it was - defaults in, defaults out.
void test_measure_fitness_leaves_default_rule_constants_alone() {
    TrainingConfig config = fast_training_config();
    config.params_path = temp_path("u1_default_params.json").string();
    config.checkpoint_path = temp_path("u1_default_checkpoint.json").string();

    const ThreatCoefficients defaults;
    std::vector<std::pair<int, int>> seen;
    Trainer trainer(defaults, config);
    trainer.set_fitness_override([&seen](const CoefficientPair& coeffs) {
        assert(coeffs.white.five == coeffs.black.five);
        assert(coeffs.white.forbidden_penalty == coeffs.black.forbidden_penalty);
        seen.emplace_back(coeffs.black.five, coeffs.black.forbidden_penalty);
        return deviation_score(coeffs);
    });

    trainer.run_round();

    assert(seen.size() >= 2);
    for (const std::pair<int, int>& call : seen) {
        assert(call.first == defaults.five);
        assert(call.second == defaults.forbidden_penalty);
    }

    std::cout << "test_measure_fitness_leaves_default_rule_constants_alone passed ("
              << seen.size() << " pairings, all " << defaults.five << "/"
              << defaults.forbidden_penalty << ")" << std::endl;
}

// R91 V1: the per-ply dedup key has to carry the last stone's cell as well as
// the played one. Slots 0-24 are invariant, but 25-51 are read off the oriented
// board's last move - so a symmetry that fixes the played cell while moving that
// stone yields a genuinely different sample, and dropping it loses data.
//
// A record whose leading plies are replays (collect() ignores those) plus one
// searched ply that is the only sampled one.
static GameRecord sampled_record(const std::vector<Move>& placement, Move sampled) {
    GameRecord record;
    record.winner = BLACK;
    record.reason = GameEndReason::FIVE;
    for (const Move& m : placement) {
        MoveRecord entry;
        entry.move = m;
        entry.method = "replay";
        record.moves.push_back(entry);
    }
    MoveRecord entry;
    entry.move = sampled;
    entry.score = 120;
    entry.depth = 4;
    entry.nodes = 500;
    entry.elapsed_ms = 9;
    entry.method = "alphabeta";
    record.moves.push_back(entry);
    record.total_nodes = 500;
    record.max_depth = 4;
    return record;
}

// The board the sampled ply is played on: every ply but the last.
static Board sampled_prefix(const GameRecord& record) {
    Board board;
    for (size_t i = 0; i + 1 < record.moves.size(); ++i) {
        board.place_stone(record.moves[i].move.pos, record.moves[i].move.color);
    }
    return board;
}

// The part augmentation is supposed to multiply: slots 25-51 plus the move.
static std::vector<float> orientation_key(const TrainingSample& sample) {
    std::vector<float> key(sample.features.begin() + DataCollector::FEATURE_LAST_ROW,
                           sample.features.end());
    key.push_back(static_cast<float>(sample.move.pos.to_index()));
    key.push_back(static_cast<float>(sample.move.color));
    return key;
}

// The number of samples whose (25-51, move) key is unique in the batch.
static int distinct_orientation_keys(const std::vector<TrainingSample>& samples) {
    std::set<std::vector<float>> seen;
    for (const TrainingSample& sample : samples) seen.insert(orientation_key(sample));
    return static_cast<int>(seen.size());
}

void test_collector_keeps_the_variant_that_moved_the_last_stone() {
    // The main-diagonal mirror (variant 4) maps this placement back onto itself:
    // (7,7) holds, the black pair (6,8)/(8,6) swaps, the white pair (7,6)/(6,7)
    // swaps. It also fixes the played cell (5,5) - but it moves the last stone.
    const std::vector<Move> placement{Move(7, 7, BLACK), Move(6, 8, BLACK), Move(8, 6, BLACK),
                                      Move(7, 6, WHITE), Move(6, 7, WHITE)};
    const GameRecord record = sampled_record(placement, Move(5, 5, BLACK));
    const Board before = sampled_prefix(record);

    // The fixture really is self-symmetric, really fixes the played cell, and
    // really relocates the last move - otherwise the count below proves nothing.
    const std::vector<int> symmetries = self_symmetries(before);
    assert(symmetries.size() == 2);
    assert(std::find(symmetries.begin(), symmetries.end(), 4) != symmetries.end());
    assert(transform_position(Position(5, 5), 4) == Position(5, 5));
    const Position last = before.last_move().pos;
    assert(last == Position(6, 7));
    assert(transform_position(last, 4) != last);
    assert(transform_board(before, 4).zobrist_hash() == before.zobrist_hash());
    assert(transform_board(before, 4).last_move().pos == Position(7, 6));

    DataCollector collector;
    const std::vector<TrainingSample> augmented = collector.collect(record, 7101, true);

    // Eight orientations, four distinct placements, two last-stone cells each:
    // none of them is a duplicate of another.
    assert(augmented.size() == static_cast<size_t>(SYMMETRY_COUNT));
    assert(distinct_orientation_keys(augmented) == SYMMETRY_COUNT);

    // The identity reading and the main-diagonal reading share the played cell
    // (5,5) and the placement hash - they are the pair the old key threw away.
    // Both must be present, anchored at the two different last-move cells.
    int at_played_cell = 0;
    bool anchored_before = false;
    bool anchored_after = false;
    for (const TrainingSample& sample : augmented) {
        if (sample.move.pos != Position(5, 5)) continue;
        ++at_played_cell;
        const std::vector<float>& f = sample.features;
        const int row = static_cast<int>(f[DataCollector::FEATURE_LAST_ROW] * 7.0f + 7.0f + 0.5f);
        const int col = static_cast<int>(f[DataCollector::FEATURE_LAST_COL] * 7.0f + 7.0f + 0.5f);
        if (row == 6 && col == 7) anchored_before = true;
        if (row == 7 && col == 6) anchored_after = true;
    }
    assert(at_played_cell == 2);
    assert(anchored_before && anchored_after);

    // One orientation only ever means one sample for this ply.
    const std::vector<TrainingSample> plain = collector.collect(record, 7102, false);
    assert(plain.size() == 1);
    assert(plain[0].move.pos == Position(5, 5));

    std::cout << "test_collector_keeps_the_variant_that_moved_the_last_stone passed ("
              << augmented.size() << " augmented, " << plain.size() << " plain)" << std::endl;
}

// Anti-overreach for V1: when the symmetry fixes the last stone too the two
// variants ARE one and the same sample, and a fully asymmetric placement has
// eight distinct readings to begin with. Both answers must stay as they were.
void test_collector_still_drops_genuine_symmetry_duplicates() {
    DataCollector collector;

    // (i) Both the last stone (9,9) and the played cell (5,5) sit on the main
    // diagonal, so variant 4 reproduces the identity sample exactly.
    const std::vector<Move> diagonal{Move(7, 7, BLACK), Move(6, 8, BLACK), Move(8, 6, BLACK),
                                     Move(6, 6, WHITE), Move(9, 9, WHITE)};
    const GameRecord fixed_record = sampled_record(diagonal, Move(5, 5, BLACK));
    const Board fixed_before = sampled_prefix(fixed_record);
    assert(self_symmetries(fixed_before).size() == 2);
    assert(transform_position(fixed_before.last_move().pos, 4) == fixed_before.last_move().pos);
    const std::vector<TrainingSample> fixed = collector.collect(fixed_record, 7103, true);
    assert(fixed.size() == 4);
    assert(distinct_orientation_keys(fixed) == 4);

    // (ii) No self-symmetry at all: all eight readings are different boards, so
    // all eight are kept.
    const std::vector<Move> asymmetric{Move(7, 7, BLACK), Move(6, 8, BLACK), Move(8, 6, BLACK),
                                       Move(7, 6, WHITE), Move(4, 9, WHITE)};
    const GameRecord loose_record = sampled_record(asymmetric, Move(5, 5, BLACK));
    const Board loose_before = sampled_prefix(loose_record);
    assert(self_symmetries(loose_before).size() == 1);
    const std::vector<TrainingSample> loose = collector.collect(loose_record, 7104, true);
    assert(loose.size() == SYMMETRY_COUNT);
    assert(distinct_orientation_keys(loose) == SYMMETRY_COUNT);

    std::cout << "test_collector_still_drops_genuine_symmetry_duplicates passed ("
              << fixed.size() << " duplicate-keeping, " << loose.size() << " asymmetric)"
              << std::endl;
}

// Round 99 DD5: an empty flag value is nearly always an unexpanded shell variable,
// and for a path it is destructive - `--resume ""` used to parse as "no resume" and
// the run then wrote the default checkpoint as if nothing had been asked for.
// require_value now throws on an empty value and the existing catch answers with
// exit 1. Each probe carries `--rounds 0` so a tree that still accepts the empty
// value stops at the round-count guard instead of starting a training run: what is
// under test is which of the two refusals the parser produces, and that is read off
// the exit code and the flag name in stderr - no wording is asserted.
void test_trainer_refuses_an_empty_flag_value() {
#ifndef _WIN32
    if (!std::filesystem::exists("gomoku_train")) {
        std::cout << "  trainer empty-value probes skipped: gomoku_train is not in the "
                     "working directory" << std::endl;
        std::cout << "test_trainer_refuses_an_empty_flag_value passed" << std::endl;
        return;
    }
    const std::filesystem::path err = temp_path("r99_dd5_stderr.txt");
    auto read_all = [](const std::filesystem::path& p) {
        std::ifstream in(p);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    };
    std::string last_err;
    auto run = [&](const std::string& arguments) {
        const std::string command = "./gomoku_train " + arguments + " > /dev/null 2> '" +
                                    err.string() + "'";
        const int status = std::system(command.c_str());
        assert(status != -1);
        last_err = read_all(err);
        return WEXITSTATUS(status);
    };

    for (const char* flag : {"--resume", "--params", "--out"}) {
        const int status = run(std::string(flag) + " '' --rounds 0");
        std::cout << "  [r99dd5] " << flag << " with an empty value => exit " << status << ": "
                  << last_err.substr(0, last_err.find('\n')) << std::endl;
        assert(status == 1);
        // The empty value was refused, ahead of the round-count guard behind it.
        assert(last_err.find(flag) != std::string::npos);
        assert(last_err.find("--rounds") == std::string::npos);
    }

    // Control: the same flags with a stated value still reach the guard that
    // follows the parser, so the refusal above is about emptiness, not the flag.
    const int stated = run("--out '/tmp/r99_dd5_out.json' --rounds 0");
    std::cout << "  [r99dd5] --out with a value => exit " << stated << ": "
              << last_err.substr(0, last_err.find('\n')) << std::endl;
    assert(stated == 1);
    assert(last_err.find("--rounds") != std::string::npos);

    std::cout << "test_trainer_refuses_an_empty_flag_value passed" << std::endl;
#endif
}

// Round 99 DD6, labelled as what it is: a help-text assert. Only the usage
// entries for --stagnation and --hill-iterations were rewritten, and the two
// claims they now make are the code's existing behaviour - the 3x probe budget is
// trainer.cpp:374 and the N+1 ceiling is hill_climbing.cpp skipping a step whose
// neighbour is identical. No call path changes, so there is nothing else to pin.
void test_trainer_help_states_the_probe_budget() {
#ifndef _WIN32
    if (!std::filesystem::exists("gomoku_train")) {
        std::cout << "  trainer help probe skipped: gomoku_train is not in the working "
                     "directory" << std::endl;
        std::cout << "test_trainer_help_states_the_probe_budget passed" << std::endl;
        return;
    }
    const std::filesystem::path out = temp_path("r99_dd6_usage.txt");
    const int status = std::system(("./gomoku_train --help > '" + out.string() +
                                    "' 2>&1").c_str());
    assert(status != -1);
    std::ifstream usage(out);
    std::ostringstream text;
    text << usage.rdbuf();
    const std::string help = text.str();
    std::cout << "  [r99dd6] --help => exit " << WEXITSTATUS(status) << ", " << help.size()
              << " bytes of usage" << std::endl;
    assert(WEXITSTATUS(status) == 0);
    assert(help.find("--stagnation") != std::string::npos);
    assert(help.find("3x") != std::string::npos);
    assert(help.find("N+1") != std::string::npos);

    std::cout << "test_trainer_help_states_the_probe_budget passed" << std::endl;
#endif
}

// Round 100 EE2: --no-sessions next to a directory the caller actually wrote down
// used to mean "the directory you named is dropped on the floor" - the same
// ambiguous instruction `--sessions ""` is refused for (README:169-172's rationale).
// The parser now answers exit 1 naming both sides. Every probe also carries a
// nonexistent --web-dir, so a tree without the conflict guard stops at the web-dir
// refusal (main.cpp:122) instead of binding a port, and a tree with it never gets
// that far: the --port value below is therefore never bound in either. What is
// under test is which refusal the process produces, read off the exit code and the
// flag names in stderr - no wording is asserted.
void test_server_refuses_no_sessions_with_a_stated_path() {
#ifndef _WIN32
    if (!std::filesystem::exists("gomoku_server")) {
        std::cout << "  --no-sessions conflict probes skipped: gomoku_server is not in the "
                     "working directory" << std::endl;
        std::cout << "test_server_refuses_no_sessions_with_a_stated_path passed" << std::endl;
        return;
    }
    const std::filesystem::path err = temp_path("r100_ee2_stderr.txt");
    const std::string missing_web = temp_path("r100_ee2_no_such_web").string();
    auto read_all = [](const std::filesystem::path& p) {
        std::ifstream in(p);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    };
    std::string last_err;
    auto run = [&](const std::string& arguments) {
        const std::string command = "./gomoku_server " + arguments + " --web-dir '" +
                                    missing_web + "' --port 59321 > /dev/null 2> '" +
                                    err.string() + "'";
        const int status = std::system(command.c_str());
        assert(status != -1);
        last_err = read_all(err);
        return WEXITSTATUS(status);
    };

    const char* const conflicts[] = {
        "--no-sessions --sessions '/tmp/r100_ee2_sessions'",
        "--sessions '/tmp/r100_ee2_sessions' --no-sessions",
        "--no-sessions --records '/tmp/r100_ee2_records.jsonl'",
    };
    for (const char* arguments : conflicts) {
        const int status = run(arguments);
        std::cout << "  [r100ee2] " << arguments << " => exit " << status << ": "
                  << last_err.substr(0, last_err.find('\n')) << std::endl;
        assert(status == 1);
        // The contradiction is named, and it is named instead of the web-dir
        // refusal that every one of these commands would otherwise reach.
        assert(last_err.find("--no-sessions") != std::string::npos);
        assert(last_err.find("--web-dir") == std::string::npos);
    }

    // Controls: each half on its own still parses, so the refusal above is about
    // the combination rather than either flag. Both fall through to the web-dir
    // guard, which is what keeps these runs from ever listening.
    const int alone = run("--no-sessions");
    std::cout << "  [r100ee2] --no-sessions alone => exit " << alone << ": "
              << last_err.substr(0, last_err.find('\n')) << std::endl;
    assert(alone == 1);
    assert(last_err.find("--web-dir") != std::string::npos);
    assert(last_err.find("--no-sessions") == std::string::npos);

    const int stated = run("--sessions '/tmp/r100_ee2_sessions'");
    std::cout << "  [r100ee2] --sessions alone => exit " << stated << ": "
              << last_err.substr(0, last_err.find('\n')) << std::endl;
    assert(stated == 1);
    assert(last_err.find("--web-dir") != std::string::npos);

    std::cout << "test_server_refuses_no_sessions_with_a_stated_path passed" << std::endl;
#endif
}

// Round 101 FF2: `--resume CK --seed N` used to start, parse N, and then let the
// resume overwrite it (trainer.cpp:628 assigns the seed the checkpoint recorded, and
// trainer.cpp:486 always writes one), so the reproducibility the caller asked for was
// not the run that actually happened. The parser now refuses the pair, the same class
// as the --params/--resume contradiction it sits next to.
//
// Arms that must merely *run* use the cheapest knobs the guards allow (one round, one
// generation, depth 1, a 1 ms slice, two moves per game, two verification games,
// --no-data), which takes tens of milliseconds, and every artefact is a temp path so
// data/ is never written. Judgement is the exit code, which flag names appear in
// stderr, and the seed number the checkpoint ends up carrying - no wording is
// asserted. NOTE for the next CLI probe here: run a full `make -j4` first, or an
// unchanged gomoku_train from before the fix is exec'd and the arms read as failures.
void test_trainer_refuses_a_seed_that_a_resume_would_drop() {
#ifndef _WIN32
    if (!std::filesystem::exists("gomoku_train")) {
        std::cout << "  --seed/--resume probes skipped: gomoku_train is not in the working "
                     "directory" << std::endl;
        std::cout << "test_trainer_refuses_a_seed_that_a_resume_would_drop passed" << std::endl;
        return;
    }
    const std::string base =
        "--rounds 1 --population 2 --generations 1 --depth 1 --timeout-ms 1 --max-moves 2 "
        "--match-opening-plies 0 "
        "--fitness-games 1 --verification-games 2 --no-data --quiet";
    const std::filesystem::path err = temp_path("r101_ff2_stderr.txt");
    auto read_all = [](const std::filesystem::path& p) {
        std::ifstream in(p);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    };
    // The number the writer put behind the "seed" key, read without assuming any
    // particular spacing or field order.
    auto seed_in = [&read_all](const std::filesystem::path& p) -> long long {
        const std::string text = read_all(p);
        const size_t key = text.find("\"seed\"");
        if (key == std::string::npos) return -1;
        size_t i = text.find_first_of("0123456789", key + 6);
        if (i == std::string::npos) return -1;
        long long value = 0;
        for (; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); ++i) {
            value = value * 10 + (text[i] - '0');
        }
        return value;
    };
    std::string last_err;
    auto run = [&](const std::string& arguments) {
        const std::string command = "./gomoku_train " + base + " " + arguments +
                                    " > /dev/null 2> '" + err.string() + "'";
        const int status = std::system(command.c_str());
        assert(status != -1);
        last_err = read_all(err);
        return WEXITSTATUS(status);
    };

    // Setup, and the both-absent control: a run that never mentions --seed writes a
    // checkpoint carrying a seed of its own.
    const std::filesystem::path params = temp_path("r101_ff2_params.json");
    const std::filesystem::path ck = temp_path("r101_ff2_ck.json");
    std::error_code ec;
    std::filesystem::remove(ck, ec);
    const int setup = run("--out '" + params.string() + "' --checkpoint '" + ck.string() + "'");
    const long long stored_seed = seed_in(ck);
    std::cout << "  [r101ff2] neither flag => exit " << setup << ", checkpoint seed "
              << stored_seed << std::endl;
    assert(setup == 0);
    assert(std::filesystem::exists(ck));
    assert(stored_seed != 7);

    // Both given, in either order: refused, naming both flags.
    const std::string quoted_ck = "'" + ck.string() + "'";
    for (const std::string arguments : {"--resume " + quoted_ck + " --seed 7",
                                        "--seed 7 --resume " + quoted_ck}) {
        const int status = run(arguments);
        std::cout << "  [r101ff2] " << arguments << " => exit " << status << ": "
                  << last_err.substr(0, last_err.find('\n')) << std::endl;
        assert(status == 1);
        assert(last_err.find("--seed") != std::string::npos);
        assert(last_err.find("--resume") != std::string::npos);
    }
    // The refusal happens before the run, so the checkpoint it named is untouched.
    assert(seed_in(ck) == stored_seed);

    // Control: --resume on its own still resumes (a fresh copy, because the run
    // advances the file it read).
    const std::filesystem::path resume_copy = temp_path("r101_ff2_resume_copy.json");
    std::filesystem::copy_file(ck, resume_copy, std::filesystem::copy_options::overwrite_existing);
    const int resume_only = run("--resume '" + resume_copy.string() + "'");
    std::cout << "  [r101ff2] --resume alone => exit " << resume_only << ", stderr "
              << last_err.size() << " bytes, checkpoint seed " << seed_in(resume_copy)
              << std::endl;
    assert(resume_only == 0);
    assert(last_err.empty());
    assert(seed_in(resume_copy) == stored_seed);

    // Control: --seed on its own takes effect, which is the whole point of the
    // refusal above - the value is only dropped when a resume is in play.
    const std::filesystem::path seed_params = temp_path("r101_ff2_seed_params.json");
    const std::filesystem::path seed_ck = temp_path("r101_ff2_seed_ck.json");
    std::filesystem::remove(seed_ck, ec);
    const int seed_only = run("--seed 7 --out '" + seed_params.string() + "' --checkpoint '" +
                              seed_ck.string() + "'");
    std::cout << "  [r101ff2] --seed 7 alone => exit " << seed_only << ", checkpoint seed "
              << seed_in(seed_ck) << std::endl;
    assert(seed_only == 0);
    assert(seed_in(seed_ck) == 7);

    std::cout << "test_trainer_refuses_a_seed_that_a_resume_would_drop passed" << std::endl;
#endif
}

// Round 106 JJ2: --out and --checkpoint name the two files a run owns - the champion
// parameters it writes at the end and the resume state it writes every round. Pointing both
// at one path meant the closing champion write replaced the checkpoint, so the next
// --resume read coefficients where it expected a round counter. README:204 records the
// refusal, which sits with the --params/--resume and --seed/--resume ones ahead of every
// file operation.
//
// The judgement is the exit code plus what the named path ends up holding. `--rounds 0`
// cannot be used here: the round-count guard is ahead of this refusal in the same parser, so
// both trees would answer 1 and nothing would discriminate. The arms therefore run the
// cheapest legal round round 101 FF2 established, and every artefact is a temp path so
// data/ is never written. No wording is asserted beyond the two flag names.
void test_trainer_refuses_one_file_for_both_out_and_checkpoint() {
#ifndef _WIN32
    if (!std::filesystem::exists("gomoku_train")) {
        std::cout << "  --out/--checkpoint probes skipped: gomoku_train is not in the working "
                     "directory" << std::endl;
        std::cout << "test_trainer_refuses_one_file_for_both_out_and_checkpoint passed" << std::endl;
        return;
    }
    const std::string base =
        "--rounds 1 --population 2 --generations 1 --depth 1 --timeout-ms 1 --max-moves 2 "
        "--match-opening-plies 0 "
        "--fitness-games 1 --verification-games 2 --no-data --quiet";
    const std::filesystem::path err = temp_path("r106_jj2_stderr.txt");
    auto read_all = [](const std::filesystem::path& p) {
        std::ifstream in(p);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    };
    std::string last_err;
    auto run = [&](const std::string& arguments) {
        const std::string command = "./gomoku_train " + base + " " + arguments +
                                    " > /dev/null 2> '" + err.string() + "'";
        const int status = std::system(command.c_str());
        assert(status != -1);
        last_err = read_all(err);
        return WEXITSTATUS(status);
    };

    // Control: the same cheapest run with two distinct paths finishes and writes both.
    const std::filesystem::path out = temp_path("r106_jj2_out.json");
    const std::filesystem::path ck = temp_path("r106_jj2_ck.json");
    const int apart = run("--out '" + out.string() + "' --checkpoint '" + ck.string() + "'");
    std::cout << "  [r106jj2] two paths => exit " << apart << ", stderr " << last_err.size()
              << " bytes" << std::endl;
    assert(apart == 0);
    assert(last_err.empty());
    assert(std::filesystem::exists(out));
    assert(std::filesystem::exists(ck));

    // Discriminator: one path for both roles is refused, and nothing is created.
    const std::filesystem::path one = temp_path("r106_jj2_one.json");
    const int refused = run("--out '" + one.string() + "' --checkpoint '" + one.string() + "'");
    std::cout << "  [r106jj2] --out == --checkpoint => exit " << refused << ": "
              << last_err.substr(0, last_err.find('\n')) << std::endl;
    assert(refused == 1);
    assert(last_err.find("--out") != std::string::npos);
    assert(last_err.find("--checkpoint") != std::string::npos);
    // Ahead of ensure_parent_directory and both writers, so the name it refused is absent.
    assert(!std::filesystem::exists(one));

    // The stronger half: a checkpoint already sitting at that name survives the attempt
    // byte for byte. Replacing it is exactly the loss the refusal exists to prevent.
    const std::filesystem::path kept = temp_path("r106_jj2_kept.json");
    {
        std::ofstream writer(kept);
        writer << "{\"seed\":12345,\"rounds_completed\":7}\n";
    }
    const std::string before = read_all(kept);
    const int kept_status =
        run("--out '" + kept.string() + "' --checkpoint '" + kept.string() + "'");
    const std::string after = read_all(kept);
    std::cout << "  [r106jj2] one existing file => exit " << kept_status << ", bytes "
              << before.size() << " -> " << after.size() << std::endl;
    assert(kept_status == 1);
    assert(after == before);
    assert(after.find("rounds_completed") != std::string::npos);

    std::cout << "test_trainer_refuses_one_file_for_both_out_and_checkpoint passed" << std::endl;
#endif
}

// Round 107 KK1: run() clears everything that describes *this* run - generations_run,
// converged_, best_history_ - and it did not clear the stagnation counter
// `generations_since_improvement_`. genetic.h:39-41 states the contract as "evolves until
// the best result stops improving", i.e. this run's own improvement history, so a second run
// on the same object inherited the first one's count and, with fitness that cannot improve,
// hit `generations_since_improvement_ >= stagnation_generations` inside generation 1 and
// reported generations_run == 1. The trainer builds one optimizer per round loop, so every
// round after the first of a stagnating search was billed as a single generation.
//
// `evaluations_` is intentionally left alone (genetic.h:50-51 - it is the caller's cumulative
// cost counter), which is why the accumulation is pinned in the same test: a future sweep
// that zeroed it too would silently under-report the training budget. A constant fitness
// makes the outcome independent of the RNG draws - no generation can beat the incumbent, so
// the only exit is stagnation - and population_size == 2 means each generation scores
// exactly one new member (the incumbent carries over unscored), fixing a run's cost at
// population_size + generations * (population_size - 1).
void test_genetic_stagnation_count_is_per_run() {
    CoefficientPair base;

    GeneticConfig settings;
    settings.population_size = 2;
    settings.stagnation_generations = 2;
    settings.max_generations = 2;

    auto flat = [](const CoefficientPair&) { return 1.0; };

    GeneticOptimizer optimizer(flat, settings, 20261015);
    int first_generations = 0;
    double first_best = 0.0;
    optimizer.run(base, first_generations, first_best);
    const int first_cost = optimizer.evaluations();
    std::cout << "  [r107kk1] run 1 => generations " << first_generations << ", converged "
              << optimizer.converged() << ", evaluations " << first_cost << std::endl;
    assert(first_generations == settings.stagnation_generations);
    assert(optimizer.converged());
    assert(std::abs(first_best - 1.0) < 1e-12);
    assert(first_cost ==
           settings.population_size + first_generations * (settings.population_size - 1));

    // Discriminator: a second run on the same object owes the caller its own stagnation
    // history, so the flat fitness again takes stagnation_generations generations.
    int second_generations = 0;
    double second_best = 0.0;
    optimizer.run(base, second_generations, second_best);
    std::cout << "  [r107kk1] run 2 on the same object => generations " << second_generations
              << ", evaluations " << optimizer.evaluations() << std::endl;
    assert(second_generations == settings.stagnation_generations);
    assert(optimizer.converged());
    assert(std::abs(second_best - 1.0) < 1e-12);

    // Control: a fresh object with the same config and seed needs the same number of
    // generations, which is exactly what the second run above was owed.
    GeneticOptimizer fresh(flat, settings, 20261015);
    int fresh_generations = 0;
    double fresh_best = 0.0;
    fresh.run(base, fresh_generations, fresh_best);
    std::cout << "  [r107kk1] a brand-new object => generations " << fresh_generations
              << std::endl;
    assert(fresh_generations == first_generations);
    assert(std::abs(fresh_best - 1.0) < 1e-12);

    // The opposite invariant, and it must stay green: the cost counter keeps counting across
    // runs. Written as a delta it holds whichever way the second run ended, and it goes red
    // the moment run() resets evaluations_.
    const int second_cost = optimizer.evaluations();
    std::cout << "  [r107kk1] evaluations accumulate " << first_cost << " -> " << second_cost
              << std::endl;
    assert(second_cost > first_cost);
    assert(second_cost - first_cost ==
           settings.population_size + second_generations * (settings.population_size - 1));

    std::cout << "test_genetic_stagnation_count_is_per_run passed" << std::endl;
}

// Round 110 NN1: README:204 refuses --out when it names the same file as --checkpoint, and
// README:206 requires --checkpoint to name the same file as --resume. HEAD compared the two
// command-line *strings*, so 同一个文件 written two ways went the wrong way in both
// directions: `目录/ck.json` beside `目录/./ck.json` slipped past README:204 and the closing
// champion write replaced a real checkpoint, while the same pair beside --resume was refused
// as a mismatch even though README:206 was satisfied. The guards now compare
// std::filesystem::weakly_canonical results - the file, not its spelling.
//
// Both arms therefore judge exit codes and what the named files end up holding, never the
// wording of a message beyond the flag names the refusal must print. Every path is absolute,
// temp_path-built, with the "." segment injected through parent_path()/"."/filename() so no
// arm depends on anything but ./gomoku_train, and data/ is never opened. --rounds 0 still
// cannot discriminate (the round-count guard sits ahead of both refusals), so each arm runs
// the cheapest legal round round 106 JJ2 and round 101 FF2 established.
void test_trainer_refuses_one_file_written_two_ways_for_out_and_checkpoint() {
#ifndef _WIN32
    if (!std::filesystem::exists("gomoku_train")) {
        std::cout << "  --out/--checkpoint spelling probes skipped: gomoku_train is not in the "
                     "working directory" << std::endl;
        std::cout << "test_trainer_refuses_one_file_written_two_ways_for_out_and_checkpoint passed"
                  << std::endl;
        return;
    }
    const std::string base =
        "--rounds 1 --population 2 --generations 1 --depth 1 --timeout-ms 1 --max-moves 2 "
        "--match-opening-plies 0 "
        "--fitness-games 1 --verification-games 2 --no-data --quiet";
    const std::filesystem::path err = temp_path("r110_nn1a_stderr.txt");
    auto read_all = [](const std::filesystem::path& p) {
        std::ifstream in(p);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    };
    std::string last_err;
    auto run = [&](const std::string& arguments) {
        const std::string command = "./gomoku_train " + base + " " + arguments +
                                    " > /dev/null 2> '" + err.string() + "'";
        const int status = std::system(command.c_str());
        assert(status != -1);
        last_err = read_all(err);
        return WEXITSTATUS(status);
    };

    // A real checkpoint to protect: a run that owns two differently named files leaves a
    // resume state on disk, and its bytes are what README:204's refusal exists to keep.
    const std::filesystem::path out = temp_path("r110_nn1a_out.json");
    const std::filesystem::path ck = temp_path("r110_nn1a_ck.json");
    const int setup = run("--out '" + out.string() + "' --checkpoint '" + ck.string() + "'");
    const std::string before = read_all(ck);
    std::cout << "  [r110nn1a] two distinct names => exit " << setup << ", checkpoint "
              << before.size() << " bytes" << std::endl;
    assert(setup == 0);
    assert(!before.empty());
    assert(std::filesystem::exists(out));

    // Discriminator: 目录/ck.json and 目录/./ck.json are one file. HEAD compares the
    // spellings, sees two, runs the round and overwrites the checkpoint with the champion -
    // exit 0 and a different file.
    const std::filesystem::path dotted = ck.parent_path() / "." / ck.filename();
    const int refused = run("--out '" + ck.string() + "' --checkpoint '" + dotted.string() + "'");
    const std::string after = read_all(ck);
    std::cout << "  [r110nn1a] --out 目录/ck.json vs --checkpoint 目录/./ck.json => exit "
              << refused << ", bytes " << before.size() << " -> " << after.size() << std::endl;
    assert(refused == 1);
    assert(after == before);
    assert(last_err.find("--out") != std::string::npos);
    assert(last_err.find("--checkpoint") != std::string::npos);

    // Control, green before and after: the identical spelling was already refused, so
    // resolving the paths did not loosen README:204's real case.
    const std::filesystem::path kept = temp_path("r110_nn1a_kept.json");
    const std::string kept_before = [&kept, &before] {
        std::ofstream writer(kept);
        writer << before;
        return before;
    }();
    const int same_spelling =
        run("--out '" + kept.string() + "' --checkpoint '" + kept.string() + "'");
    std::cout << "  [r110nn1a] --out == --checkpoint, one spelling => exit " << same_spelling
              << ", bytes " << kept_before.size() << " -> " << read_all(kept).size() << std::endl;
    assert(same_spelling == 1);
    assert(read_all(kept) == kept_before);

    // Control: two genuinely different files still train and still write both - the
    // canonicalizing guard must not turn the ordinary run into a refusal.
    const std::filesystem::path apart_out = temp_path("r110_nn1a_apart_out.json");
    const std::filesystem::path apart_ck = temp_path("r110_nn1a_apart_ck.json");
    const int apart =
        run("--out '" + apart_out.string() + "' --checkpoint '" + apart_ck.string() + "'");
    std::cout << "  [r110nn1a] two genuinely different files => exit " << apart << ", stderr "
              << last_err.size() << " bytes" << std::endl;
    assert(apart == 0);
    assert(last_err.empty());
    assert(std::filesystem::exists(apart_out));
    assert(std::filesystem::exists(apart_ck));

    std::cout
        << "test_trainer_refuses_one_file_written_two_ways_for_out_and_checkpoint passed"
        << std::endl;
#endif
}

// Round 110 NN1, the other direction: --resume and --checkpoint may name one file in two
// spellings (README:206). HEAD's string comparison called `目录/ck.json` and `目录/./ck.json`
// different, so a legal resume aborted with exit 1 and the run never started.
//
// The same-cheapest-round driver as the case above, and the guard ordering is what makes the
// arms readable: the --resume/--checkpoint comparison sits ahead of the --params, --seed and
// --out refusals and ahead of every file operation, so --out here deliberately names a third
// file - pointing it at the checkpoint would answer 1 for the README:204 reason and mask this
// guard. The control that matters most is the two-genuinely-different-files arm: it must stay
// red for the caller on both trees, i.e. the fix did not soften README:207's real refusal.
void test_trainer_accepts_a_resume_and_checkpoint_that_differ_only_by_a_dot_segment() {
#ifndef _WIN32
    if (!std::filesystem::exists("gomoku_train")) {
        std::cout << "  --resume/--checkpoint spelling probes skipped: gomoku_train is not in "
                     "the working directory" << std::endl;
        std::cout << "test_trainer_accepts_a_resume_and_checkpoint_that_differ_only_by_a_dot"
                     "_segment passed" << std::endl;
        return;
    }
    const std::string base =
        "--rounds 1 --population 2 --generations 1 --depth 1 --timeout-ms 1 --max-moves 2 "
        "--match-opening-plies 0 "
        "--fitness-games 1 --verification-games 2 --no-data --quiet";
    const std::filesystem::path err = temp_path("r110_nn1b_stderr.txt");
    auto read_all = [](const std::filesystem::path& p) {
        std::ifstream in(p);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    };
    std::string last_err;
    auto run = [&](const std::string& arguments) {
        const std::string command = "./gomoku_train " + base + " " + arguments +
                                    " > /dev/null 2> '" + err.string() + "'";
        const int status = std::system(command.c_str());
        assert(status != -1);
        last_err = read_all(err);
        return WEXITSTATUS(status);
    };

    // A checkpoint a resume can legitimately read: written here by a real run, so
    // load_checkpoint() accepts it and an exit 1 can only come from the guards.
    const std::filesystem::path ck = temp_path("r110_nn1b_ck.json");
    const std::filesystem::path setup_out = temp_path("r110_nn1b_setup_out.json");
    const int setup =
        run("--out '" + setup_out.string() + "' --checkpoint '" + ck.string() + "'");
    std::cout << "  [r110nn1b] setup => exit " << setup << ", checkpoint "
              << read_all(ck).size() << " bytes" << std::endl;
    assert(setup == 0);
    assert(std::filesystem::exists(ck));

    // Discriminator: one file, two spellings. README:206 is satisfied, so the run should
    // proceed; HEAD refuses it and writes nothing.
    const std::filesystem::path dotted = ck.parent_path() / "." / ck.filename();
    const std::filesystem::path resume_out = temp_path("r110_nn1b_resume_out.json");
    const int accepted = run("--resume '" + ck.string() + "' --checkpoint '" + dotted.string() +
                             "' --out '" + resume_out.string() + "'");
    std::cout << "  [r110nn1b] --resume 目录/ck.json --checkpoint 目录/./ck.json => exit "
              << accepted << ", stderr " << last_err.size() << " bytes, --out "
              << (std::filesystem::exists(resume_out) ? "written" : "absent") << std::endl;
    assert(accepted == 0);
    assert(last_err.empty());
    assert(std::filesystem::exists(resume_out));

    // Control, green on both trees: the identical spelling was never refused, so the fix
    // keeps the run it already allowed.
    const std::filesystem::path same_out = temp_path("r110_nn1b_same_out.json");
    const int same_spelling = run("--resume '" + ck.string() + "' --checkpoint '" + ck.string() +
                                  "' --out '" + same_out.string() + "'");
    std::cout << "  [r110nn1b] --resume == --checkpoint, one spelling => exit " << same_spelling
              << ", stderr " << last_err.size() << " bytes" << std::endl;
    assert(same_spelling == 0);
    assert(last_err.empty());
    assert(std::filesystem::exists(same_out));

    // Control, the one that must stay red for a loosened guard: two genuinely different
    // files are still refused before anything is opened (README:207).
    const std::filesystem::path other_ck = temp_path("r110_nn1b_other_ck.json");
    const std::filesystem::path refusal_out = temp_path("r110_nn1b_refusal_out.json");
    const int refused = run("--resume '" + ck.string() + "' --checkpoint '" + other_ck.string() +
                            "' --out '" + refusal_out.string() + "'");
    std::cout << "  [r110nn1b] --resume and --checkpoint two files => exit " << refused
              << ", checkpoint " << other_ck << " exists "
              << (std::filesystem::exists(other_ck) ? "yes" : "no") << std::endl;
    assert(refused == 1);
    assert(last_err.find("--resume") != std::string::npos);
    assert(last_err.find("--checkpoint") != std::string::npos);
    assert(!std::filesystem::exists(other_ck));
    assert(!std::filesystem::exists(refusal_out));

    std::cout
        << "test_trainer_accepts_a_resume_and_checkpoint_that_differ_only_by_a_dot_segment passed"
        << std::endl;
#endif
}

// Round 126, D-1 (docs/ENGINE_REDESIGN.md :434, :592): a training ply may be priced
// in nodes instead of milliseconds. SelfPlayConfig::max_nodes is armed on both
// engines by the SelfPlay constructor, because search() names only a depth and a
// millisecond limit and the engine field is the only route the ceiling has into the
// next think. The assertions below are exact node counts, which is the property the
// knob exists to buy: the cut point is a counter read at every node, so it lands on
// the same number under load, under valgrind, and on a machine that is half asleep -
// unlike the 10 ms wall-clock arm REVIEW_LOG records for test_search.
void test_self_play_node_budget_bounds_every_think() {
    SearchEngine black;
    SearchEngine white;
    black.set_resign_detection(false);
    white.set_resign_detection(false);
    SelfPlayConfig settings;
    settings.max_depth = 4;
    // Five seconds: an unstarted depth-4 think costs a few thousand nodes here, so a
    // ceiling of 1000 is what must cut every ply, never the clock.
    settings.timeout_ms = 5000;
    settings.max_nodes = 1000;
    settings.max_moves = 8;

    SelfPlay play(black, white, settings);
    GameRecord record = play.run(std::vector<Move>(), nullptr, nullptr);
    assert(record.reason != GameEndReason::INVALID);
    assert(record.moves.size() == 8);
    assert_playable(record);
    for (const auto& entry : record.moves) {
        assert(entry.method == "alphabeta");
        assert(entry.stopped == "nodes");
        assert(entry.nodes == settings.max_nodes);
    }
    // The record's own sum is the ledger the training run is measured by.
    assert(record.total_nodes == 8 * settings.max_nodes);

    // A second, independent pair on the same ceiling replays the identical line and
    // reports the identical work ply for ply: with the budget in nodes, "how much
    // this game cost" is a property of the position, not of the run.
    SearchEngine black_b;
    SearchEngine white_b;
    black_b.set_resign_detection(false);
    white_b.set_resign_detection(false);
    SelfPlay again(black_b, white_b, settings);
    GameRecord second = again.run(std::vector<Move>(), nullptr, nullptr);
    assert(second.moves.size() == record.moves.size());
    for (size_t i = 0; i < second.moves.size(); ++i) {
        assert(second.moves[i].move == record.moves[i].move);
        assert(second.moves[i].nodes == record.moves[i].nodes);
        assert(second.moves[i].stopped == record.moves[i].stopped);
    }

    std::cout << "test_self_play_node_budget_bounds_every_think passed" << std::endl;
}

// The doc keeps --timeout-ms as 安全兜底 (docs :434), so a named node ceiling must not
// disarm the wall clock: with a ceiling too large to reach, the think still stops for
// time. The reason is asserted rather than a duration, because a duration is what a
// loaded machine cannot promise. Nothing in this tree could remove that fallback
// through a flag: TimeManager::allocate's training branch (time_manager.cpp:51) zeroes
// the soft/hard slice of a MoveBudget that no production caller reads - allocate() is
// only ever called from the A1 scaffolding tests - and SearchEngine arms the clock
// twice over, through limits.hard_time_ms and through ab.set_deadline
// (search_engine.cpp:295, :260).
void test_self_play_ms_budget_backs_up_a_generous_node_ceiling() {
    SearchEngine black;
    SearchEngine white;
    black.set_resign_detection(false);
    white.set_resign_detection(false);
    SelfPlayConfig settings;
    // Eight layers: no machine finishes that inside 60 ms on these positions, so the
    // wall clock is certain to be the thing that cuts, while the 20M-node ceiling is
    // certain never to be reached.
    settings.max_depth = 8;
    settings.timeout_ms = 60;
    settings.max_nodes = 20000000;
    settings.max_moves = 4;

    SelfPlay play(black, white, settings);
    GameRecord record = play.run(std::vector<Move>(), nullptr, nullptr);
    assert(record.reason != GameEndReason::INVALID);
    assert(record.moves.size() == 4);
    assert_playable(record);
    int thought = 0;
    int done = 0;
    for (const auto& entry : record.moves) {
        if (entry.method != "alphabeta") continue;
        ++thought;
        // A think that finished every layer it was asked for stops for "none"; that
        // is still the clock's answer, not the ceiling's.
        assert(entry.stopped != "nodes");
        assert(entry.nodes < settings.max_nodes);
        if (entry.stopped == "time_hard") ++done;
    }
    // A game whose every ply answered off a fast path would prove nothing, because
    // those paths report 0 nodes whatever the budgets say.
    assert(thought == 4);
    std::cout << "  [r126] fallback arm: " << done << "/4 plies cut by the ms budget"
              << std::endl;
    assert(done >= 1);

    std::cout << "test_self_play_ms_budget_backs_up_a_generous_node_ceiling passed" << std::endl;
}

// 0 is the default and the old behavior: no ceiling is armed, so nothing can stop for
// want of one. This is also what keeps every existing SelfPlay caller - the
// quick_play_config() tests and RetryExplorer's data side - unchanged by the new field.
void test_self_play_zero_ceiling_arms_no_node_budget() {
    SearchEngine black;
    SearchEngine white;
    black.set_resign_detection(false);
    white.set_resign_detection(false);
    SelfPlayConfig settings = quick_play_config();
    assert(settings.max_nodes == 0);
    assert(black.max_nodes() == 0);

    SelfPlay play(black, white, settings);
    assert(black.max_nodes() == 0);
    assert(white.max_nodes() == 0);
    GameRecord record = play.run(std::vector<Move>(), nullptr, nullptr);
    assert(record.reason != GameEndReason::INVALID);
    assert(!record.moves.empty());
    assert_playable(record);
    for (const auto& entry : record.moves) {
        assert(entry.stopped != "nodes");
    }

    std::cout << "test_self_play_zero_ceiling_arms_no_node_budget passed" << std::endl;
}

// The regime has to be said out loud rather than read off one of the numbers. The F
// round's budget allocator is the first thing inside the engine that treats a wall clock
// as a quantity to divide, and `--timeout-ms` with no `--max-nodes` is the shipped
// training default (self_play.h:15-18; README.md:186 names neither). Shaping that wall
// would have cut every think of the default run to 65 % of it and collected its samples
// from a depth the round never asked for - the one change to the training pipeline that
// would have been invisible in every existing cost reading.
void test_self_play_declares_training_so_the_clock_does_not_shape_it() {
    SearchEngine black;
    SearchEngine white;
    black.set_resign_detection(false);
    white.set_resign_detection(false);
    // An engine that no game owns is a play engine: the flag defaults off, which is what
    // keeps the shaping reachable for a player.
    assert(!black.training() && !white.training());

    SelfPlayConfig settings = quick_play_config();
    assert(settings.max_nodes == 0);
    assert(settings.timeout_ms > 0);
    SelfPlay play(black, white, settings);
    assert(black.training() && white.training());

    // The observable half of the same fact: a think that was shaped by the wall can stop
    // for the window it was given, and a training think must never report that reason.
    GameRecord record = play.run(std::vector<Move>(), nullptr, nullptr);
    assert(!record.moves.empty());
    for (const auto& entry : record.moves) {
        assert(entry.stopped != "time_soft");
    }

    std::cout << "test_self_play_declares_training_so_the_clock_does_not_shape_it passed ("
              << record.moves.size() << " moves, no soft stop)" << std::endl;
}

// A one-node ceiling is not a broken think: the interrupted layer hands back the root
// move it had, so the game keeps going with a legal - if shallow - line. Zero completed
// layers with a reported cut is the accounting design doc 2.3.3 asks for, and this is
// the training-side caller that has to keep working under it.
void test_self_play_one_node_ceiling_still_answers_legally() {
    SearchEngine black;
    SearchEngine white;
    black.set_resign_detection(false);
    white.set_resign_detection(false);
    SelfPlayConfig settings;
    settings.max_depth = 4;
    settings.timeout_ms = 5000;
    settings.max_nodes = 1;
    settings.max_moves = 6;

    SelfPlay play(black, white, settings);
    GameRecord record = play.run(std::vector<Move>(), nullptr, nullptr);
    assert(record.reason == GameEndReason::MOVE_LIMIT);
    assert(record.moves.size() == 6);
    assert_playable(record);
    for (const auto& entry : record.moves) {
        assert(entry.method == "alphabeta");
        assert(entry.nodes == 1);
        assert(entry.stopped == "nodes");
        // Nothing finished, and the layer that was cut is reported as cut.
        assert(entry.depth == 0);
        assert(entry.partial_depth == 1);
    }

    std::cout << "test_self_play_one_node_ceiling_still_answers_legally passed" << std::endl;
}

// The two copies TrainingConfig::max_nodes has to make - fitness games go through
// play_game, data games through the worker lambda - are separate lines, and a run can
// be half budgeted if only one of them is wired. The fitness side is read back off the
// engines; the data side owns its engines inside the worker, so it is read off the
// record file it writes.
void test_trainer_config_node_budget_reaches_both_game_paths() {
    TrainingConfig config = fast_training_config();
    config.search_depth = 4;
    config.timeout_ms = 5000;
    config.max_nodes = 1000;
    config.max_moves = 2;

    Trainer trainer(ThreatCoefficients(), config);
    SearchEngine black;
    SearchEngine white;
    bool played = false;
    assert(trainer.play_game(black, white, 1, {}, &played) == 0.5);
    assert(played);
    assert(black.max_nodes() == config.max_nodes);
    assert(white.max_nodes() == config.max_nodes);

    // The same engines are the only thing a config that names no ceiling can leave
    // armed: 0 has to disarm, not inherit whatever the last caller set.
    TrainingConfig off = fast_training_config();
    off.search_depth = 4;
    off.timeout_ms = 5000;
    off.max_moves = 2;
    Trainer unarmed(ThreatCoefficients(), off);
    SearchEngine black_b;
    SearchEngine white_b;
    black_b.set_max_nodes(9000);
    white_b.set_max_nodes(9000);
    assert(unarmed.play_game(black_b, white_b, 1, {}, nullptr) == 0.5);
    assert(black_b.max_nodes() == 0);
    assert(white_b.max_nodes() == 0);

    const std::filesystem::path records = temp_path("r126_node_budget_records.jsonl");
    TrainingConfig data = config;
    data.max_moves = 6;
    data.data_threads = 1;
    data.data_opening_plies = 2;
    data.seed = 4242;
    data.records_path = records.string();
    data.samples_path.clear();
    Trainer collector(ThreatCoefficients(), data);
    assert(collector.generate_data_games(1, 1) > 0);

    std::ifstream records_file(records);
    std::string line;
    int games = 0;
    int capped = 0;
    std::string error;
    while (std::getline(records_file, line)) {
        if (line.empty()) continue;
        GameRecord record;
        assert(GameRecord::from_json(line, record, error));
        ++games;
        for (const auto& entry : record.moves) {
            // The two opening plies are replayed, not thought, so they carry no budget.
            if (entry.method == "replay") continue;
            assert(entry.nodes <= data.max_nodes);
            if (entry.stopped == "nodes") ++capped;
        }
    }
    std::cout << "  [r126] data games=" << games << " node-capped plies=" << capped << std::endl;
    assert(games >= 1);
    // Uncapped this arm's depth-4 plies cost several thousand nodes each (measured for
    // this round), so a single capped ply can only mean the ceiling reached the worker.
    assert(capped >= 1);

    std::error_code ec;
    std::filesystem::remove(records, ec);
    std::cout << "test_trainer_config_node_budget_reaches_both_game_paths passed" << std::endl;
}

// The CLI half: --max-nodes parses, 0 is its own answer rather than a rejected count,
// and the negative and missing-value arms are refused before a game is played.
void test_trainer_cli_max_nodes_probes() {
#ifndef _WIN32
    if (!std::filesystem::exists("gomoku_train")) {
        std::cout << "  --max-nodes probes skipped: gomoku_train is not in the working "
                     "directory" << std::endl;
        std::cout << "test_trainer_cli_max_nodes_probes passed" << std::endl;
        return;
    }
    const std::string base =
        "--rounds 1 --population 2 --generations 1 --depth 1 --timeout-ms 1 --max-moves 2 "
        "--match-opening-plies 0 "
        "--fitness-games 1 --verification-games 2 --no-data --quiet";
    const std::filesystem::path err = temp_path("r126_max_nodes_stderr.txt");
    const std::filesystem::path out_path = temp_path("r126_max_nodes_out.json");
    auto read_all = [](const std::filesystem::path& p) {
        std::ifstream in(p);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    };
    auto run = [&](const std::string& arguments) {
        const std::string command = "./gomoku_train " + base + " --out '" + out_path.string() +
                                    "' " + arguments + " > /dev/null 2> '" + err.string() + "'";
        const int status = std::system(command.c_str());
        assert(status != -1);
        return WEXITSTATUS(status);
    };

    const int zero = run("--max-nodes 0");
    const int named = run("--max-nodes 5000");
    std::string zero_err = read_all(err);
    std::cout << "  [r126] --max-nodes 0 => exit " << zero << ", --max-nodes 5000 => exit "
              << named << std::endl;
    assert(zero == 0);
    assert(zero_err.empty());
    assert(named == 0);

    const int negative = run("--max-nodes -1");
    const std::string negative_err = read_all(err);
    std::cout << "  [r126] --max-nodes -1 => exit " << negative << ", stderr says "
              << (negative_err.find("--max-nodes") != std::string::npos ? "the flag" : "nothing")
              << std::endl;
    assert(negative == 1);
    assert(negative_err.find("--max-nodes") != std::string::npos);

    const int missing = run("--max-nodes");
    assert(missing == 1);
    const int junk = run("--max-nodes 1e3");
    assert(junk == 1);

    const std::filesystem::path usage = temp_path("r126_max_nodes_usage.txt");
    assert(std::system(("./gomoku_train --help > '" + usage.string() + "' 2>&1").c_str()) != -1);
    const std::string help = read_all(usage);
    std::cout << "  [r126] --help mentions --max-nodes: "
              << (help.find("--max-nodes") != std::string::npos ? "yes" : "no") << std::endl;
    assert(help.find("--max-nodes") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(out_path, ec);
    std::cout << "test_trainer_cli_max_nodes_probes passed" << std::endl;
#endif
}

// ---------------------------------------------------------------------------
// D2 - 进程级共享 TT (design doc 2.8 #1 :433 「自对弈的所有引擎共享一张大表…分片加锁…内存与
// 并发局数**解耦**」, 测试表 :596 「共享 TT 并发安全」).
//
// What the batch does with the one table is the whole claim, and it splits three ways:
// every worker reaches it (not just the first), a record left in it does not survive into
// the next batch (the coefficients a score was computed under get replaced between them),
// and a thread reading what another thread wrote reads a whole record. The memory half is
// not asserted here - it is a resident-set measurement, and the round's log carries it.

TrainingConfig d2_data_config(const std::string& tag, int games, int threads) {
    TrainingConfig config = fast_training_config();
    config.search_depth = 4;
    config.timeout_ms = 400;
    config.max_nodes = 4000;
    config.max_moves = 6;
    config.collect_data = true;
    config.data_games = games;
    config.data_threads = threads;
    config.data_opening_plies = 2;
    config.seed = 4242;
    config.records_path = temp_path(tag + "_records.jsonl").string();
    config.samples_path.clear();
    return config;
}

// 一 worker 一盘 makes the attribution arithmetic close: one engine alone cannot have
// advanced the table past its own game, so a generation above the longest single game
// says a second worker wrote here too. The hits say the same thing from the content side
// - a worker left on its own table leaves its positions absent from the one the trainer
// can see.
void test_one_shared_table_serves_every_worker_of_a_batch() {
    TrainingConfig config = d2_data_config("r127_shared", 4, 4);
    Trainer trainer(ThreatCoefficients(), config);
    assert(trainer.shared_transposition_table() == nullptr);  // built at the batch head
    const int samples = trainer.generate_data_games(4, 4);
    TranspositionTable* tt = trainer.shared_transposition_table();
    assert(tt != nullptr);
    assert(tt->is_shared());
    assert(tt->size() == static_cast<size_t>(1 << 18));
    assert(samples > 0);

    std::ifstream file(config.records_path);
    std::string line;
    int games = 0, games_with_hits = 0, max_thought = 0, total_thought = 0;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        GameRecord record;
        std::string error;
        assert(GameRecord::from_json(line, record, error));
        ++games;
        Board board;
        int thought = 0, hits = 0;
        for (const auto& entry : record.moves) {
            board.place_stone(entry.move.pos.row, entry.move.pos.col, entry.move.color);
            // The replayed opening plies were never thought, so no node stored them.
            if (entry.method == "replay") continue;
            ++thought;
            TTEntry found;
            if (tt->probe(board.zobrist_hash(), found, 0)) ++hits;
        }
        if (hits > 0) ++games_with_hits;
        max_thought = std::max(max_thought, thought);
        total_thought += thought;
    }
    std::cout << "  [r127] games=" << games << " thought plies=" << total_thought
              << " longest game=" << max_thought << " games answered by the table="
              << games_with_hits << " generation=" << static_cast<int>(tt->generation())
              << std::endl;
    assert(games == 4);
    assert(games_with_hits == games);
    assert(static_cast<int>(tt->generation()) > max_thought);

    std::error_code ec;
    std::filesystem::remove(config.records_path, ec);
    std::cout << "test_one_shared_table_serves_every_worker_of_a_batch passed" << std::endl;
}

// A record is a score computed under the coefficients that were live when it was written,
// and the champion can be replaced between batches, so the batch head empties the table
// (design doc 轮次126's 失效口径). depth 30 is deeper than anything a --depth 4 search
// writes, so the replacement policy cannot be what removes this record: only the clear can.
void test_the_next_batch_clears_the_shared_table() {
    TrainingConfig config = d2_data_config("r127_clear", 1, 1);
    config.max_nodes = 2000;
    Trainer trainer(ThreatCoefficients(), config);
    assert(trainer.generate_data_games(1, 1) > 0);
    TranspositionTable* tt = trainer.shared_transposition_table();
    assert(tt != nullptr);

    const uint64_t sentinel = 0x0123456789ABCDEFULL;
    tt->store(sentinel, 30, 4242, Move(7, 7, BLACK), TTFlag::EXACT, 0);
    TTEntry found;
    assert(tt->probe(sentinel, found, 0));
    assert(found.depth == 30 && found.value == 4242);

    assert(trainer.generate_data_games(1, 1) > 0);
    const bool survived = tt->probe(sentinel, found, 0);
    std::cout << "  [r127] sentinel after the next batch: " << (survived ? "STILL THERE"
                                                                         : "cleared")
              << ", generation back to " << static_cast<int>(tt->generation()) << std::endl;
    assert(!survived);
    // The slot is usable again afterwards - a clear is an empty table, not a broken one.
    tt->store(sentinel, 30, 4242, Move(7, 7, BLACK), TTFlag::EXACT, 0);
    assert(tt->probe(sentinel, found, 0));

    std::error_code ec;
    std::filesystem::remove(config.records_path, ec);
    std::cout << "test_the_next_batch_clears_the_shared_table passed" << std::endl;
}

// --shared-tt 0 is the doc's own layout (N workers, N tables), and the borrowed form has
// to be able to answer a move: a worker that shares is not a worker that does not search.
void test_sharing_off_leaves_every_engine_its_own_table() {
    TrainingConfig config = d2_data_config("r127_off", 2, 2);
    config.shared_tt_entries = 0;
    Trainer trainer(ThreatCoefficients(), config);
    assert(trainer.generate_data_games(2, 2) > 0);
    assert(trainer.shared_transposition_table() == nullptr);

    SearchEngine a;
    SearchEngine b;
    assert(&a.transposition_table() != &b.transposition_table());
    assert(!a.transposition_table().is_shared());

    TranspositionTable shared(1 << 12, true);
    SearchEngine borrower(ThreatCoefficients(), &shared);
    assert(&borrower.transposition_table() == &shared);
    assert(borrower.transposition_table().is_shared());

    Board board;
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, WHITE);
    std::atomic<bool> stop(false);
    const SearchResult answer = borrower.search(board, BLACK, 2, 50, stop);
    const bool legal = answer.best_move.color != EMPTY &&
                       board.is_empty(answer.best_move.pos.row, answer.best_move.pos.col);
    std::cout << "  [r127] borrowed engine answered "
              << answer.best_move.pos.row << "," << answer.best_move.pos.col
              << " (legal: " << (legal ? "yes" : "no") << ")" << std::endl;
    assert(legal);

    std::error_code ec;
    std::filesystem::remove(config.records_path, ec);
    std::cout << "test_sharing_off_leaves_every_engine_its_own_table passed" << std::endl;
}

// 并发安全. Four writers on one 1024-bucket table, eight hashes they all share, so every
// store lands in a slot somebody else is reading. A record is written from one place and
// read from another, and the four fields the reader gets back are only mutually true if
// the write that produced them was not interleaved with another: value V always comes with
// static_eval 3V+7, depth 7 and the move V decodes to. Anything else is a torn record, and
// the count has to be exactly zero. One walker takes all sixteen locks per pass
// (hashfull()'s whole-table walk) and turns the shared generation clock while they run.
void test_one_shared_table_survives_four_concurrent_writers() {
    TranspositionTable tt(1 << 12, true);
    const int kWriters = 4;
    const int kIterations = 20000;
    uint64_t pool[8];
    for (int i = 0; i < 8; ++i) {
        pool[i] = 0x9E3779B97F4A7C15ULL * (static_cast<uint64_t>(i) + 1);
    }

    struct Counters {
        long stores = 0;
        long hits = 0;
        long torn = 0;
    };
    std::vector<Counters> counters(kWriters);
    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    for (int tid = 0; tid < kWriters; ++tid) {
        writers.emplace_back([&, tid]() {
            Counters& mine = counters[tid];
            TTEntry found;
            for (int i = 0; i < kIterations; ++i) {
                const uint64_t hash = pool[i % 8];
                const int value = tid * 10000 + (i % 10000);
                tt.store(hash, 7, value, Move(value % 15, (value / 15) % 15, BLACK),
                         TTFlag::EXACT, 0, value * 3 + 7);
                ++mine.stores;
                if (tt.probe(hash, found, 0)) {
                    ++mine.hits;
                    if (found.flag != TTFlag::EXACT ||
                        found.static_eval != found.value * 3 + 7 ||
                        found.best_move.pos.row != found.value % 15 ||
                        found.best_move.pos.col != (found.value / 15) % 15 ||
                        found.best_move.color != BLACK) {
                        ++mine.torn;
                    }
                }
            }
        });
    }

    std::thread walker([&tt]() {
        for (int i = 0; i < 2000; ++i) {
            tt.hashfull();
            tt.advance_generation();
        }
    });

    for (auto& writer : writers) writer.join();
    walker.join();

    long stores = 0, hits = 0, torn = 0;
    for (const Counters& c : counters) {
        stores += c.stores;
        hits += c.hits;
        torn += c.torn;
    }
    std::cout << "  [r127] " << stores << " stores / " << hits << " reads on one 4096-slot "
              << "table from " << kWriters << " threads, torn reads=" << torn
              << ", generation=" << static_cast<int>(tt.generation()) << std::endl;
    assert(stores == static_cast<long>(kWriters) * kIterations);
    assert(hits > 0);
    assert(torn == 0);

    tt.clear();
    TTEntry found;
    assert(!tt.probe(pool[0], found, 0));

    std::cout << "test_one_shared_table_survives_four_concurrent_writers passed" << std::endl;
}

// The CLI half: the flag parses, 0 is a real answer (the doc's own per-engine layout), a
// negative and a size past the ceiling are refused before anything is allocated, and the
// help text names it.
void test_trainer_cli_shared_tt_probes() {
#ifndef _WIN32
    if (!std::filesystem::exists("gomoku_train")) {
        std::cout << "  --shared-tt probes skipped: gomoku_train is not in the working "
                     "directory" << std::endl;
        std::cout << "test_trainer_cli_shared_tt_probes passed" << std::endl;
        return;
    }
    const std::string base =
        "--rounds 1 --population 2 --generations 1 --depth 1 --timeout-ms 1 --max-moves 2 "
        "--match-opening-plies 0 "
        "--fitness-games 1 --verification-games 2 --no-data --quiet";
    const std::filesystem::path err = temp_path("r127_shared_tt_stderr.txt");
    const std::filesystem::path out_path = temp_path("r127_shared_tt_out.json");
    auto read_all = [](const std::filesystem::path& p) {
        std::ifstream in(p);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    };
    auto run = [&](const std::string& arguments) {
        const std::string command = "./gomoku_train " + base + " --out '" + out_path.string() +
                                    "' " + arguments + " > /dev/null 2> '" + err.string() + "'";
        const int status = std::system(command.c_str());
        assert(status != -1);
        return WEXITSTATUS(status);
    };

    const int off = run("--shared-tt 0");
    const std::string off_err = read_all(err);
    const int named = run("--shared-tt 1024");
    const int ceiling = run("--shared-tt 4194304");
    std::cout << "  [r127] --shared-tt 0 => exit " << off << " (stderr "
              << (off_err.empty() ? "empty" : "not empty") << "), 1024 => exit " << named
              << ", 4194304 => exit " << ceiling << std::endl;
    assert(off == 0);
    assert(off_err.empty());
    assert(named == 0);
    assert(ceiling == 0);

    const int negative = run("--shared-tt -1");
    const std::string negative_err = read_all(err);
    const int oversized = run("--shared-tt 4194305");
    const std::string oversized_err = read_all(err);
    std::cout << "  [r127] --shared-tt -1 => exit " << negative << ", 4194305 => exit "
              << oversized << " ("
              << (oversized_err.find("at most") != std::string::npos ? "names the ceiling"
                                                                      : "does not")
              << ")" << std::endl;
    assert(negative == 1);
    assert(negative_err.find("--shared-tt") != std::string::npos);
    assert(oversized == 1);
    assert(oversized_err.find("--shared-tt") != std::string::npos);

    assert(run("--shared-tt") == 1);
    assert(run("--shared-tt 1e3") == 1);

    const std::filesystem::path usage = temp_path("r127_shared_tt_usage.txt");
    assert(std::system(("./gomoku_train --help > '" + usage.string() + "' 2>&1").c_str()) != -1);
    const std::string help = read_all(usage);
    std::cout << "  [r127] --help mentions --shared-tt: "
              << (help.find("--shared-tt") != std::string::npos ? "yes" : "no") << std::endl;
    assert(help.find("--shared-tt") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(out_path, ec);
    std::cout << "test_trainer_cli_shared_tt_probes passed" << std::endl;
#endif
}

// Design doc 2.8 #3, the arithmetic first - pinned without a board so the numbers
// below are the formula and nothing else. At the default effect_elo of 300 the two
// terms are ln(p1 / 0.5) and ln((1 - p1) / 0.5) for p1 = 1/(1 + 10^-0.75) =
// 0.8490204, which is +0.5294752 for a win and -1.1974637 for a loss, and the
// bounds at the 5% risks are +/-ln(19) = +/-2.9444390. So six wins (+3.1768510)
// close the test as an accept and four losses (-4.7898546) as a reject, while a
// match that splits every pair drifts -0.6679885 a pair and -2.6719539 over the
// shipped 8-game ceiling - short of the bound, which is what keeps a
// colour-decided board in the hands of --accept-score. A draw scores the average
// of the two terms (-0.3339942), so "no result" is evidence against a 300 Elo gap
// rather than neutral, and that is the whole reason an unfinished game cannot
// promote on the test's word alone.
void test_sprt_closes_only_at_a_pair_boundary() {
    SprtConfig settings;
    assert(settings.effect_elo == 300);
    assert(std::abs(settings.alpha - 0.05) < 1e-12);
    assert(std::abs(settings.beta - 0.05) < 1e-12);

    const double bound = 2.9444390;
    {
        SprtTest fresh(settings);
        assert(std::abs(fresh.upper_bound() - bound) < 1e-6);
        assert(std::abs(fresh.lower_bound() + bound) < 1e-6);
        assert(fresh.verdict() == SprtVerdict::OPEN);
        assert(!fresh.stop_requested());
        assert(fresh.games() == 0);
        assert(fresh.llr() == 0.0);
    }

    // One colour moved by a whole term and the test still refuses to decide: a
    // single game measures who moved first as much as who plays better.
    {
        SprtTest one(settings);
        one.add_game(1.0);
        assert(std::abs(one.llr() - 0.5294752) < 1e-6);
        assert(one.games() == 1);
        assert(!one.stop_requested());
        assert(one.verdict() == SprtVerdict::OPEN);
        // Even past the bound on one game, end_pair is the only door: this ratio
        // (+7.6) is above any bound, and the match is still open until both
        // colours have been played.
        SprtTest blown(SprtConfig{2000, 0.05, 0.05});
        blown.add_game(0.0);
        assert(blown.llr() < -bound);
        assert(!blown.stop_requested());
        assert(blown.verdict() == SprtVerdict::OPEN);
        blown.add_game(0.0);
        blown.end_pair();
        assert(blown.stop_requested());
        assert(blown.verdict() == SprtVerdict::REJECT);
        assert(blown.games() == 2);
    }

    auto play_pair = [](SprtTest& test, double first, double second) {
        test.add_game(first);
        test.add_game(second);
        test.end_pair();
    };

    {
        SprtTest sweep(settings);
        play_pair(sweep, 1.0, 1.0);
        assert(std::abs(sweep.llr() - 1.0589504) < 1e-6);
        assert(sweep.verdict() == SprtVerdict::OPEN);
        play_pair(sweep, 1.0, 1.0);
        assert(std::abs(sweep.llr() - 2.1179008) < 1e-6);
        assert(!sweep.stop_requested());
        play_pair(sweep, 1.0, 1.0);
        assert(std::abs(sweep.llr() - 3.1768510) < 1e-6);
        assert(sweep.verdict() == SprtVerdict::ACCEPT);
        assert(sweep.stop_requested());
        assert(sweep.games() == 6);
        // A verdict, once given, is not revisited - a caller that kept playing
        // after the close still reports the decision it stopped for.
        play_pair(sweep, 0.0, 0.0);
        assert(sweep.verdict() == SprtVerdict::ACCEPT);
        assert(std::abs(sweep.llr() - 0.7819236) < 1e-6);
        // The case that needs the latch rather than the assignment: three more
        // losing pairs carry the ratio to -6.40, clean through the LOWER bound,
        // and the match still reports the accept it stopped for. Without the
        // latch the second crossing overwrites the first verdict, so a match
        // decided at six games would turn into a rejection on its fourteenth.
        play_pair(sweep, 0.0, 0.0);
        play_pair(sweep, 0.0, 0.0);
        play_pair(sweep, 0.0, 0.0);
        assert(sweep.verdict() == SprtVerdict::ACCEPT);
        assert(sweep.stop_requested());
        assert(std::abs(sweep.llr() + 6.4028583) < 1e-6);
        assert(sweep.games() == 14);
    }

    {
        SprtTest worse(settings);
        play_pair(worse, 0.0, 0.0);
        assert(std::abs(worse.llr() + 2.3949273) < 1e-6);
        assert(worse.verdict() == SprtVerdict::OPEN);
        play_pair(worse, 0.0, 0.0);
        assert(std::abs(worse.llr() + 4.7898546) < 1e-6);
        assert(worse.verdict() == SprtVerdict::REJECT);
        assert(worse.games() == 4);
    }

    {
        SprtTest split(settings);
        SprtTest drawn(settings);
        for (int i = 0; i < 4; ++i) {
            play_pair(split, 1.0, 0.0);
            play_pair(drawn, 0.5, 0.5);
        }
        assert(split.verdict() == SprtVerdict::OPEN);
        assert(!split.stop_requested());
        assert(split.games() == 8);
        // The identity the model is built on: a pair of halves is exactly a pair
        // of one win and one loss.
        assert(std::abs(split.llr() - drawn.llr()) < 1e-12);
        assert(std::abs(split.llr() + 2.6719539) < 1e-6);
    }

    // The off switch: no terms, no bounds, no decision - only the game count, so
    // a caller can still report how many games a match took.
    {
        SprtTest off(SprtConfig{0, 0.05, 0.05});
        for (int i = 0; i < 4; ++i) play_pair(off, 0.0, 0.0);
        assert(off.llr() == 0.0);
        assert(off.verdict() == SprtVerdict::OPEN);
        assert(!off.stop_requested());
        assert(off.games() == 8);
    }

    // The clamp, not a crash: an Elo this side of ten thousand rounds p1 to
    // exactly 1.0 in a double, and ln(0) would hand every ratio below a NaN that
    // no bound can compare against.
    {
        SprtTest absurd(SprtConfig{1000000, 0.05, 0.05});
        absurd.add_game(1.0);
        absurd.add_game(0.0);
        absurd.end_pair();
        assert(std::isfinite(absurd.llr()));
        assert(absurd.verdict() == SprtVerdict::REJECT);
    }

    std::cout << "test_sprt_closes_only_at_a_pair_boundary passed (bounds +/-ln(19) = "
                 "+/-2.9444390, a win worth +0.5294752 and a loss -1.1974637 at the default "
                 "300 Elo, accept at 6 games, reject at 4, an all-split 8-game match left "
                 "open at -2.6719539, one game never deciding, and the accept still standing "
                 "after the record crossed the other bound at -6.4028583)"
              << std::endl;
}

// The wiring, on real games whose result is fixed by construction rather than by
// the engine's mood: --max-moves 1 makes every game unfinished, so an eight-game
// match is eight halves and the ratio can only be the draw term times eight.
// Three ceilings of the same request: the shipped default plays all eight, an
// absurd 1000 Elo gap ends it after the first pair, and --sprt-elo 0 plays all
// eight with no ratio at all.
void test_a_closed_sprt_ends_the_match_early() {
    TrainingConfig config = fast_training_config();
    config.search_depth = 1;
    config.timeout_ms = 100;
    config.max_moves = 1;
    config.resign_enabled = false;

    Trainer at_default(ThreatCoefficients(), config);
    const int before_default = at_default.games_played();
    const MatchOutcome open =
        at_default.score_match(CoefficientPair(), CoefficientPair(), 8);
    assert(at_default.games_played() - before_default == 8);
    assert(open.games == 8);
    assert(open.verdict == SprtVerdict::OPEN);
    assert(std::abs(open.llr + 2.6719539) < 1e-6);
    assert(std::abs(open.score - 0.5) < 1e-12);

    config.sprt_elo = 1000;
    Trainer strict(ThreatCoefficients(), config);
    const int before_strict = strict.games_played();
    const MatchOutcome closed =
        strict.score_match(CoefficientPair(), CoefficientPair(), 8);
    // Six games the ceiling allowed are never played, and the two that were are a
    // full pair: the ratio crossed the bound on the first game of the pair, and
    // the match still played the second colour.
    assert(strict.games_played() - before_strict == 2);
    assert(closed.games == 2);
    assert(closed.verdict == SprtVerdict::REJECT);
    assert(std::abs(closed.llr + 4.3764829) < 1e-6);
    assert(std::abs(closed.score - 0.5) < 1e-12);

    config.sprt_elo = 0;
    Trainer fixed(ThreatCoefficients(), config);
    const int before_fixed = fixed.games_played();
    const MatchOutcome played =
        fixed.score_match(CoefficientPair(), CoefficientPair(), 8);
    assert(fixed.games_played() - before_fixed == 8);
    assert(played.games == 8);
    assert(played.verdict == SprtVerdict::OPEN);
    assert(played.llr == 0.0);
    assert(std::abs(played.score - 0.5) < 1e-12);

    // play_match is the score-only door the optimisers use, and it now reports
    // the same number the outcome's score field does - the fitness signal did not
    // move with this change.
    Trainer wrapper(ThreatCoefficients(), config);
    assert(std::abs(wrapper.play_match(CoefficientPair(), CoefficientPair(), 8) -
                     played.score) < 1e-12);

    std::cout << "  [r128] one 8-game request: default 300 played 8 and stayed open, "
              << "1000 stopped after 2 (reject), 0 played 8 with llr 0" << std::endl;
    std::cout << "test_a_closed_sprt_ends_the_match_early passed" << std::endl;
}

// The verdict is the promotion rule whenever the test has one. An unfinished-game
// match scores 0.5, and with --accept-score pulled below half the bar alone would
// promote it - the sequential test says no, and the round keeps the champion.
// With the test off the same config promotes: that difference is the whole claim,
// and nothing else about the round is allowed to change between the two arms.
void test_a_reject_verdict_keeps_a_candidate_the_bar_would_promote() {
    TrainingConfig config = fast_training_config();
    config.search_depth = 1;
    config.timeout_ms = 100;
    config.max_moves = 1;
    config.resign_enabled = false;
    config.accept_score = 0.4;
    config.verification_games = 4;
    config.hill_climbing_iterations = 1;

    config.sprt_elo = 1000;
    Trainer strict(ThreatCoefficients(), config);
    const TrainerRound stopped = strict.run_round();
    assert(stopped.round == 1);
    assert(std::abs(stopped.verification_score - 0.5) < 1e-12);
    assert(stopped.verification_verdict == SprtVerdict::REJECT);
    assert(stopped.verification_match_games == 2);
    assert(!stopped.accepted);
    assert(strict.rounds_completed() == 1);
    assert(CoefficientVector::to_vector(strict.champion()) ==
           CoefficientVector::to_vector(CoefficientPair::shared(ThreatCoefficients())));
    assert(strict.status_text().find("champion=v0") != std::string::npos);

    config.sprt_elo = 0;
    Trainer lenient(ThreatCoefficients(), config);
    const TrainerRound promoted = lenient.run_round();
    assert(std::abs(promoted.verification_score - 0.5) < 1e-12);
    assert(promoted.verification_verdict == SprtVerdict::OPEN);
    assert(promoted.verification_match_games == 4);
    assert(promoted.accepted);
    assert(lenient.status_text().find("champion=v1") != std::string::npos);

    std::cout << "  [r128] score 0.5 against a 0.4 bar: reject after "
              << stopped.verification_match_games
              << " games kept the champion, the same match open after "
              << promoted.verification_match_games << " games promoted" << std::endl;
    std::cout << "test_a_reject_verdict_keeps_a_candidate_the_bar_would_promote passed"
              << std::endl;
}

// The CLI half: the flag parses, 0 is its own answer (so it is refused nowhere a
// count would be), --no-sprt means the same thing, a negative is refused by name,
// and the usage text carries both. NOTE for the next probe here: two of these arms
// assert the summary line's *wording*, which every other CLI arm in this file
// avoids - here it is the contract, because that line is the only place a run
// shows what the test decided and how many games it needed. --max-moves 2 gives
// each side one stone, which cannot be a five, so every game is unfinished and the
// early stop is deterministic rather than a matter of the engine's mood.
void test_trainer_cli_sprt_probes() {
#ifndef _WIN32
    if (!std::filesystem::exists("gomoku_train")) {
        std::cout << "  --sprt-elo probes skipped: gomoku_train is not in the working "
                     "directory" << std::endl;
        std::cout << "test_trainer_cli_sprt_probes passed" << std::endl;
        return;
    }
    const std::string base =
        "--rounds 1 --method hill --hill-iterations 1 --depth 1 --timeout-ms 1 "
        "--max-moves 2 --match-opening-plies 0 --fitness-games 1 "
        "--verification-games 4 --no-data --quiet";
    const std::filesystem::path err = temp_path("r128_sprt_stderr.txt");
    const std::filesystem::path out_path = temp_path("r128_sprt_out.json");
    const std::filesystem::path ck = temp_path("r128_sprt_ck.json");
    auto read_all = [](const std::filesystem::path& p) {
        std::ifstream in(p);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    };
    auto run = [&](const std::string& arguments) {
        const std::string command = "./gomoku_train " + base + " --out '" + out_path.string() +
                                    "' --checkpoint '" + ck.string() + "' " + arguments +
                                    " > /dev/null 2> '" + err.string() + "'";
        const int status = std::system(command.c_str());
        assert(status != -1);
        return WEXITSTATUS(status);
    };
    // temp_path() deletes what it names, so the log path is taken once: reading
    // it back through a second call would delete the run's own output first.
    const std::filesystem::path log = temp_path("r128_sprt_stdout.txt");
    auto stdout_of = [&](const std::string& arguments) {
        const std::string command = "./gomoku_train " + base + " --out '" + out_path.string() +
                                    "' --checkpoint '" + ck.string() + "' " + arguments +
                                    " > '" + log.string() + "' 2> '" + err.string() + "'";
        assert(std::system(command.c_str()) != -1);
        return read_all(log);
    };

    std::error_code ec;
    std::filesystem::remove(out_path, ec);
    std::filesystem::remove(ck, ec);

    const int off = run("--sprt-elo 0");
    const int named = run("--sprt-elo 300");
    const int flag_off = run("--no-sprt");
    std::cout << "  [r128] --sprt-elo 0 => exit " << off << ", 300 => exit " << named
              << ", --no-sprt => exit " << flag_off << std::endl;
    assert(off == 0);
    assert(named == 0);
    assert(flag_off == 0);

    const int negative = run("--sprt-elo -1");
    const std::string negative_err = read_all(err);
    std::cout << "  [r128] --sprt-elo -1 => exit " << negative << ", stderr says "
              << (negative_err.find("--sprt-elo") != std::string::npos ? "the flag" : "nothing")
              << std::endl;
    assert(negative == 1);
    assert(negative_err.find("--sprt-elo") != std::string::npos);
    assert(run("--sprt-elo") == 1);
    assert(run("--sprt-elo 1e3") == 1);
    assert(run("--no-sprt 1") == 1);

    // The stop itself, through the door a user runs: eight unfinished games ask
    // for four pairs, a 1000 Elo test closes it on the first pair, and the same
    // match with the test off plays all four.
    const std::string stopped = stdout_of("--sprt-elo 1000");
    const std::string whole = stdout_of("--no-sprt");
    std::cout << "  [r128] --sprt-elo 1000 line: "
              << (stopped.find("reject after 2 games") != std::string::npos ? "stopped early"
                                                                            : "played everything")
              << "; --no-sprt line: "
              << (whole.find("open after 4 games") != std::string::npos ? "played everything"
                                                                        : "unexpected")
              << std::endl;
    assert(stopped.find("reject after 2 games") != std::string::npos);
    assert(whole.find("open after 4 games") != std::string::npos);
    assert(whole.find("llr 0") != std::string::npos);

    const std::filesystem::path usage = temp_path("r128_sprt_usage.txt");
    assert(std::system(("./gomoku_train --help > '" + usage.string() + "' 2>&1").c_str()) != -1);
    const std::string help = read_all(usage);
    std::cout << "  [r128] --help mentions --sprt-elo: "
              << (help.find("--sprt-elo") != std::string::npos ? "yes" : "no")
              << ", --no-sprt: " << (help.find("--no-sprt") != std::string::npos ? "yes" : "no")
              << std::endl;
    assert(help.find("--sprt-elo") != std::string::npos);
    assert(help.find("--no-sprt") != std::string::npos);

    std::filesystem::remove(out_path, ec);
    std::filesystem::remove(ck, ec);
    std::filesystem::remove(temp_path("r128_sprt_stdout.txt"), ec);
    std::filesystem::remove(temp_path("r128_sprt_stderr.txt"), ec);
    std::filesystem::remove(temp_path("r128_sprt_usage.txt"), ec);
    std::cout << "test_trainer_cli_sprt_probes passed" << std::endl;
#endif
}

// D-4, design doc 2.8 #4. The set a match pair is played from: fixed by index, so
// two candidates measured in one round are measured on the same positions; diverse,
// so a match asks eight questions instead of the same one twice; and short enough to
// replay legally, because a line the replay refuses would leave the match with no
// denominator rather than a smaller one.
void test_match_opening_set_is_fixed_and_diverse() {
    // The shipped default is OFF, and this is the door that keeps it off: the round's
    // mirror screen found 47 of 48 four-ply lines already won by one colour, so a
    // replayed opening pins its pair at 0.5 instead of testing the coefficients. The
    // mechanism below is what a named count arms.
    assert(TrainingConfig().match_opening_plies == 0);

    // Plies 0 is the pre-#4 contract and the arm the readings compare against.
    assert(Trainer::match_opening_line(0, 0).empty());
    assert(Trainer::match_opening_line(3, 0).empty());
    assert(Trainer::match_opening_line(-2, 0).empty());

    // One ply is forced - an empty board has no near-neighbour candidates - so every
    // member opens with the same BLACK stone and the set's diversity lives after it.
    std::set<std::string> keys;
    for (int i = 0; i < Trainer::kMatchOpeningSet; ++i) {
        const std::vector<Move> line = Trainer::match_opening_line(i, 4);
        assert(line.size() == 4);
        assert(line[0] == Move(BOARD_SIZE / 2, BOARD_SIZE / 2, BLACK));
        assert(line[1].color == WHITE);
        assert(line[2].color == BLACK);
        assert(line[3].color == WHITE);
        assert_playable_line(line);
        keys.insert(symmetry_key_of(line));
    }
    // Distinct as positions, not merely as move sequences: a mirror image of an
    // opening asks a symmetric engine the same question, so counting the fold is the
    // only way to know the set really has eight members.
    std::cout << "  [r129] opening set: " << Trainer::kMatchOpeningSet << " indices of 4 "
                 "plies reach " << keys.size() << " positions after folding the "
              << SYMMETRY_COUNT << " symmetries" << std::endl;
    assert(keys.size() == static_cast<size_t>(Trainer::kMatchOpeningSet));

    // Why four plies is the floor when the mechanism is armed and not a guess:
    // legal_moves() only walks the cells near a stone, so with one stone on the board
    // the reply is chosen from a handful of neighbourhood cells, and eight indices
    // cannot yield eight positions. The reading is printed, not asserted equal to the
    // set size - the ceiling is the reason four is the smallest useful count.
    std::set<std::string> two_ply_keys;
    for (int i = 0; i < Trainer::kMatchOpeningSet; ++i) {
        const std::vector<Move> line = Trainer::match_opening_line(i, 2);
        assert(line.size() == 2);
        assert_playable_line(line);
        two_ply_keys.insert(symmetry_key_of(line));
    }
    std::cout << "  [r129] the same 8 indices at 2 plies reach only " << two_ply_keys.size()
              << " positions - four is the floor when the flag is armed" << std::endl;
    assert(two_ply_keys.size() < static_cast<size_t>(Trainer::kMatchOpeningSet));

    // Fixed means bounded: past the end of the set the rotation reuses the same lines
    // instead of inventing an unbounded number of them, and asking twice answers twice
    // the same way.
    assert(Trainer::match_opening_line(Trainer::kMatchOpeningSet, 4) ==
           Trainer::match_opening_line(0, 4));
    assert(Trainer::match_opening_line(Trainer::kMatchOpeningSet + 3, 4) ==
           Trainer::match_opening_line(3, 4));
    assert(Trainer::match_opening_line(-Trainer::kMatchOpeningSet, 4) ==
           Trainer::match_opening_line(0, 4));
    assert(Trainer::match_opening_line(2, 4) == Trainer::match_opening_line(2, 4));
    assert(Trainer::match_opening_line(2, 6).size() == 6);

    // A longer request stays legal, not merely longer: BLACK's next stone is picked
    // out of legal_moves(), which is the same filter the replay validates against.
    for (int i = 0; i < Trainer::kMatchOpeningSet; ++i) {
        assert_playable_line(Trainer::match_opening_line(i, 6));
    }

    std::cout << "test_match_opening_set_is_fixed_and_diverse passed" << std::endl;
}

// The wiring. A prefix play_game does not hand to SelfPlay cannot be seen from
// outside, but an illegal prefix is loud: it discards the game, so the discard is
// what proves the line travelled - and it is also the failure mode that matters,
// because a generator that produced an illegal opening would silently eat the
// match's denominator instead of failing.
void test_match_pair_replays_its_opening_and_discards_an_illegal_one() {
    TrainingConfig config = fast_training_config();
    config.search_depth = 1;
    config.timeout_ms = 50;
    config.max_moves = 6;
    Trainer trainer(ThreatCoefficients(), config);
    const int before = trainer.games_played();

    // Every member of the set replays: eight games played, eight games counted.
    bool played = false;
    for (int i = 0; i < Trainer::kMatchOpeningSet; ++i) {
        SearchEngine black;
        SearchEngine white;
        const double score =
            trainer.play_game(black, white, 1, Trainer::match_opening_line(i, 4), &played);
        assert(played);
        assert(score == 0.0 || score == 0.5 || score == 1.0);
    }
    assert(trainer.games_played() - before == Trainer::kMatchOpeningSet);

    // A cell used twice is not a draw and not a half: it leaves the denominator,
    // which is what keeps a broken opening from scoring as a survived game.
    std::vector<Move> twice;
    twice.emplace_back(3, 3, BLACK);
    twice.emplace_back(3, 3, WHITE);
    SearchEngine black_b;
    SearchEngine white_b;
    played = true;
    assert(trainer.play_game(black_b, white_b, 1, twice, &played) == 0.0);
    assert(!played);

    // And the colour order is checked, not assumed: WHITE opening on an empty board
    // fails the same way, so a set that stopped alternating would show up here.
    std::vector<Move> wrong_colour;
    wrong_colour.emplace_back(7, 7, WHITE);
    SearchEngine black_c;
    SearchEngine white_c;
    played = true;
    assert(trainer.play_game(black_c, white_c, 2, wrong_colour, &played) == 0.0);
    assert(!played);
    assert(trainer.games_played() - before == Trainer::kMatchOpeningSet);

    std::cout << "  [r129] " << Trainer::kMatchOpeningSet
              << " openings replayed and counted, 2 illegal lines discarded without "
                 "touching the counter"
              << std::endl;
    std::cout << "test_match_pair_replays_its_opening_and_discards_an_illegal_one passed"
              << std::endl;
}

// The rotation through the door the optimisers use. A match with the sequential test
// off spends every pair it asks for, so the game count is the contract: four pairs
// means eight games, all of them replayed from the round's openings and none of them
// discarded. Deeper openings keep the same promise.
void test_a_match_rotates_through_the_opening_set() {
    TrainingConfig config = fast_training_config();
    config.search_depth = 1;
    config.timeout_ms = 50;
    config.max_moves = 6;
    config.sprt_elo = 0;
    config.max_moves = 8;
    config.match_opening_plies = 4;

    Trainer shallow(ThreatCoefficients(), config);
    const int before_shallow = shallow.games_played();
    const MatchOutcome pairs =
        shallow.score_match(CoefficientPair(), CoefficientPair(), 8);
    std::cout << "  [r129] 8-game match at 4 opening plies: " << pairs.games
              << " games counted, score " << pairs.score << std::endl;
    assert(pairs.games == 8);
    assert(shallow.games_played() - before_shallow == 8);

    config.match_opening_plies = 6;
    Trainer deep(ThreatCoefficients(), config);
    const int before_deep = deep.games_played();
    const MatchOutcome deeper = deep.score_match(CoefficientPair(), CoefficientPair(), 8);
    std::cout << "  [r129] 8-game match at 6 opening plies: " << deeper.games
              << " games counted, score " << deeper.score << std::endl;
    assert(deeper.games == 8);
    assert(deep.games_played() - before_deep == 8);

    // Plies 0 is the arm the readings compare against: the same match, from the
    // empty board, still spends all eight games.
    config.match_opening_plies = 0;
    Trainer empty_start(ThreatCoefficients(), config);
    const int before_empty = empty_start.games_played();
    const MatchOutcome old =
        empty_start.score_match(CoefficientPair(), CoefficientPair(), 8);
    assert(old.games == 8);
    assert(empty_start.games_played() - before_empty == 8);

    std::cout << "test_a_match_rotates_through_the_opening_set passed" << std::endl;
}

// The flag at the CLI door: 0 is its own answer and parses, a negative is refused by
// name, a count that reaches the ply cap is refused because it would leave the game
// with nothing searched, and the usage text carries the row. --max-moves 2 is the
// shipped short-game probe, so the ceiling is one ply away and both sides of the
// guard are reachable.
void test_trainer_cli_match_opening_probes() {
#ifndef _WIN32
    if (!std::filesystem::exists("gomoku_train")) {
        std::cout << "  --match-opening-plies probes skipped: gomoku_train is not in the "
                     "working directory" << std::endl;
        std::cout << "test_trainer_cli_match_opening_probes passed" << std::endl;
        return;
    }
    const std::string base =
        "--rounds 1 --method hill --hill-iterations 1 --depth 1 --timeout-ms 1 "
        "--max-moves 2 --fitness-games 1 --verification-games 2 --no-data --quiet";
    const std::filesystem::path out_path = temp_path("r129_open_out.json");
    const std::filesystem::path ck = temp_path("r129_open_ck.json");
    const std::filesystem::path err = temp_path("r129_open_stderr.txt");
    auto read_all = [](const std::filesystem::path& p) {
        std::ifstream in(p);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    };
    auto run = [&](const std::string& arguments) {
        const std::string command = "./gomoku_train " + base + " --out '" + out_path.string() +
                                    "' --checkpoint '" + ck.string() + "' " + arguments +
                                    " > /dev/null 2> '" + err.string() + "'";
        const int status = std::system(command.c_str());
        assert(status != -1);
        return WEXITSTATUS(status);
    };

    std::error_code ec;
    std::filesystem::remove(out_path, ec);
    std::filesystem::remove(ck, ec);

    // One replayed ply under a two-ply cap is still a searched game: accepted. The
    // same request at the cap is refused, and 0 - no opening at all - is accepted.
    const int one = run("--match-opening-plies 1");
    const int at_cap = run("--match-opening-plies 2");
    // Read the refusal before the next arm overwrites the log: the arm that exits 0
    // writes nothing to it.
    const std::string cap_err = read_all(err);
    const int off = run("--match-opening-plies 0");
    // Asking for nothing is the shipped default, and under --max-moves 2 the default
    // has to clear the guard - which is only true because it is 0.
    const int unnamed = run("");
    std::cout << "  [r129] --match-opening-plies 1 => exit " << one << ", 2 with "
                 "--max-moves 2 => exit "
              << at_cap << ", 0 => exit " << off << ", unnamed => exit " << unnamed
              << std::endl;
    assert(one == 0);
    assert(at_cap == 1);
    assert(off == 0);
    assert(unnamed == 0);
    // The refusal names both flags: a run that dies on this combination has to be
    // told which of the two to move.
    assert(cap_err.find("--match-opening-plies") != std::string::npos);
    assert(cap_err.find("--max-moves") != std::string::npos);

    assert(run("--match-opening-plies -1") == 1);
    assert(run("--match-opening-plies") == 1);
    assert(run("--match-opening-plies 1e1") == 1);
    assert(run("--match-opening-plies ''") == 1);

    const std::filesystem::path usage = temp_path("r129_open_usage.txt");
    assert(std::system(("./gomoku_train --help > '" + usage.string() + "' 2>&1").c_str()) != -1);
    const std::string help = read_all(usage);
    std::cout << "  [r129] --help mentions --match-opening-plies: "
              << (help.find("--match-opening-plies") != std::string::npos ? "yes" : "no")
              << std::endl;
    assert(help.find("--match-opening-plies") != std::string::npos);

    std::filesystem::remove(out_path, ec);
    std::filesystem::remove(ck, ec);
    std::filesystem::remove(err, ec);
    std::filesystem::remove(usage, ec);
    std::cout << "test_trainer_cli_match_opening_probes passed" << std::endl;
#endif
}


// G3, design doc 2.9 :443 along its last leg. The table-read count only earns its keep if
// it survives the two archives it is written into, and if a record written before the key
// existed still reads back - as "no reads recorded", not as a broken file.
void test_game_record_keeps_the_table_read_count() {
    GameRecord record;
    record.winner = BLACK;
    record.reason = GameEndReason::FIVE;
    record.max_depth = 3;
    for (int i = 0; i < 3; ++i) {
        MoveRecord entry;
        entry.move = Move(7 + i, 7, i % 2 == 0 ? BLACK : WHITE);
        entry.score = 50 * i;
        entry.depth = 3;
        entry.nodes = 4000 + i;
        entry.elapsed_ms = 30 + i;
        entry.method = "alphabeta";
        entry.seldepth = 4 + i;
        entry.stopped = "none";
        entry.tt_hits = 900 + i;
        record.moves.push_back(entry);
    }

    const std::string text = record.to_json();
    assert(text.find("\"tt_hits\"") != std::string::npos);

    std::string error;
    GameRecord restored;
    assert(GameRecord::from_json(text, restored, error));
    assert(restored.moves.size() == 3);
    for (int i = 0; i < 3; ++i) {
        assert(restored.moves[i].tt_hits == 900 + i);
        // The neighbours of the new key must not have shifted: an inserted field in a
        // field-by-field copy is how a round-trip starts reading `seldepth` as `tt_hits`.
        assert(restored.moves[i].seldepth == 4 + i);
        assert(restored.moves[i].stopped == "none");
        assert(restored.moves[i].nodes == 4000 + i);
    }

    // The same file as the round before the key existed: the reader has to default it,
    // the way it already defaults `partial_depth` and friends for a pre-accounting record.
    std::string legacy = text;
    const std::string needle = ",\"tt_hits\":";
    size_t cut = legacy.find(needle);
    assert(cut != std::string::npos);
    while (cut != std::string::npos) {
        size_t end = cut + needle.size();
        while (end < legacy.size() && std::isdigit(static_cast<unsigned char>(legacy[end]))) ++end;
        legacy.erase(cut, end - cut);
        cut = legacy.find(needle);
    }
    assert(legacy.find("\"tt_hits\"") == std::string::npos);

    GameRecord from_legacy;
    assert(GameRecord::from_json(legacy, from_legacy, error));
    assert(from_legacy.moves.size() == 3);
    for (const auto& entry : from_legacy.moves) {
        assert(entry.tt_hits == 0);
        assert(entry.seldepth != 0);
    }

    std::cout << "test_game_record_keeps_the_table_read_count passed (3 plies kept, "
              << "and a key-less record read 0)" << std::endl;
}

// ---- 设计文档 §6.2 段 A：零搜索筛查 ----
// 这六个用例钉的是「段 A 只做筛查」这件事的每一条边界：摆幅的算术、开局判据、
// 摆幅判据、flat_game、读不回来的记录、以及深度塌陷与将杀带穿越这两条容易被
// 前一条判据抢走的。段 B 的妙手 / 俗手判定**故意**没有用例——它要 root_scores。

void test_replay_swing_reads_from_the_mover_side() {
    GameRecord record;
    record.winner = BLACK;
    record.reason = GameEndReason::FIVE;
    const int scores[4] = {100, 200, 50, 0};
    for (int i = 0; i < 4; ++i) {
        MoveRecord entry;
        entry.move = Move(7, 7 + i, i % 2 == 0 ? BLACK : WHITE);
        entry.score = scores[i];
        entry.depth = 4;
        entry.method = "alphabeta";
        record.moves.push_back(entry);
    }

    // 黑走完第一手，白报 +200，所以黑掉了 300。
    assert(ReplayAnalyzer::swing_at(record, 0) == -300);
    assert(ReplayAnalyzer::swing_at(record, 1) == -250);
    assert(ReplayAnalyzer::swing_at(record, 2) == -50);
    // 最后一手没有下一手；越界的两种取法也一样。
    assert(ReplayAnalyzer::swing_at(record, 3) == 0);
    assert(ReplayAnalyzer::swing_at(record, -1) == 0);
    assert(ReplayAnalyzer::swing_at(record, 4) == 0);

    // 反过来：对手报 -8000，说明自己这一手赚了 8000（再减掉自己当时那 100）。
    record.moves[1].score = -8000;
    assert(ReplayAnalyzer::swing_at(record, 0) == 7900);
}

void test_replay_screen_drops_the_opening_but_keeps_a_forced_reply() {
    GameRecord record;
    record.winner = WHITE;
    record.reason = GameEndReason::FIVE;
    for (int i = 0; i < 8; ++i) {
        MoveRecord entry;
        entry.move = Move(7, 7 + (i % 3), i % 2 == 0 ? BLACK : WHITE);
        entry.score = 0;
        entry.depth = 4;
        entry.method = "alphabeta";
        record.moves.push_back(entry);
    }

    ScreenConfig config;
    config.opening_plies = 6;
    ReplayAnalyzer analyzer(config);
    GameScreen screen = analyzer.screen(record);
    assert(screen.total_plies == 8);
    assert(screen.opening == 6);
    assert(screen.candidates == 0);
    assert(screen.flat == 1);  // §6.4：候选手少于 2 手的一局不产题卡
    assert(screen.plies[0].reason == ScreenReason::OPENING);
    assert(screen.plies[7].reason == ScreenReason::NOISE);

    // 开局里的一手如果是必挡，它就不是「无意义的前期手」：位置判据放过它。
    record.moves[3].method = "must_block";
    screen = analyzer.screen(record);
    assert(screen.opening == 5);
    assert(screen.plies[3].candidate);
    assert(screen.plies[3].reason == ScreenReason::METHOD_SHIFT);
}

void test_replay_screen_flags_a_big_swing() {
    GameRecord record;
    record.winner = BLACK;
    record.reason = GameEndReason::FIVE;
    const int scores[3] = {0, -8000, 0};
    for (int i = 0; i < 3; ++i) {
        MoveRecord entry;
        entry.move = Move(7, 7 + i, i % 2 == 0 ? BLACK : WHITE);
        entry.score = scores[i];
        entry.depth = 4;
        entry.method = "alphabeta";
        record.moves.push_back(entry);
    }

    ScreenConfig config;
    config.opening_plies = 0;  // 关掉位置判据，只留摆幅
    ReplayAnalyzer analyzer(config);
    const GameScreen screen = analyzer.screen(record);
    assert(screen.plies[0].swing == 8000);
    assert(screen.plies[0].candidate);
    assert(screen.plies[0].reason == ScreenReason::BIG_SWING);
    // 最后一手没有摆幅，所以它落回 NOISE 而不是被当成「没事发生」的候选。
    assert(screen.plies[2].reason == ScreenReason::NOISE);
}

void test_replay_screen_calls_a_flat_game_flat() {
    GameRecord record;
    record.winner = BLACK;
    record.reason = GameEndReason::FIVE;
    for (int i = 0; i < 8; ++i) {
        MoveRecord entry;
        entry.move = Move(7, 7 + (i % 3), i % 2 == 0 ? BLACK : WHITE);
        entry.score = 0;
        entry.depth = 4;
        entry.method = "alphabeta";
        record.moves.push_back(entry);
    }

    ScreenConfig config;
    config.opening_plies = 0;
    ReplayAnalyzer analyzer(config);
    GameScreen screen = analyzer.screen(record);
    assert(screen.candidates == 0);
    assert(screen.noise == 8);
    assert(screen.flat == 1);

    // 一手大摆幅就够让它不再是 flat：§6.4 的门槛是 2 手，这里正好落在边界上。
    record.moves[2].score = -9000;
    screen = analyzer.screen(record);
    assert(screen.candidates == 2);
    assert(screen.flat == 0);
}

void test_replay_screen_refuses_an_invalid_record() {
    GameRecord record;
    record.winner = BLACK;
    record.reason = GameEndReason::INVALID;
    MoveRecord entry;
    entry.move = Move(7, 7, BLACK);
    entry.score = 50000;
    entry.method = "alphabeta";
    record.moves.push_back(entry);

    ReplayAnalyzer analyzer;
    const GameScreen screen = analyzer.screen(record);
    assert(screen.total_plies == 1);
    assert(screen.plies.empty());  // 一手都不产
    assert(screen.candidates == 0);
    assert(screen.flat == 1);
}

void test_replay_screen_sees_a_depth_dip_and_a_mate_crossing() {
    // 深度塌陷：四手都平，但第 2 手只搜到 3 层（中位 6）-> 引擎当时不确定。
    GameRecord record;
    record.winner = BLACK;
    record.reason = GameEndReason::FIVE;
    const int depths[4] = {6, 6, 3, 6};
    for (int i = 0; i < 4; ++i) {
        MoveRecord entry;
        entry.move = Move(7, 7 + i, i % 2 == 0 ? BLACK : WHITE);
        entry.score = 0;
        entry.depth = depths[i];
        entry.method = "alphabeta";
        record.moves.push_back(entry);
    }

    ScreenConfig config;
    config.opening_plies = 0;
    ReplayAnalyzer analyzer(config);
    GameScreen screen = analyzer.screen(record);
    assert(screen.plies[2].reason == ScreenReason::DEPTH_DIP);

    // 将杀带穿越：分数从 95000 掉到 100。MATE_CROSS 现在排在 BIG_SWING 之前
    // （具体判据压过泛化判据），所以不用再抬阈值才能看见它。
    record.moves[0].score = 95000;
    record.moves[1].score = 100;
    record.moves[2].depth = 6;
    ScreenConfig mate_config;
    mate_config.opening_plies = 0;
    mate_config.swing_threshold = 200000;
    ReplayAnalyzer mate_analyzer(mate_config);
    screen = mate_analyzer.screen(record);
    assert(screen.plies[0].reason == ScreenReason::MATE_CROSS);
}

void test_replay_screen_keeps_tactical_plies_out_of_the_generic_judgments() {
    // 三条守卫都是从第一份真实记录里读出来的（REVIEW_LOG 轮次144），一条一个断言：
    //   1) 战术手的 depth 是 0，那是结构性的，不许落进 DEPTH_DIP；
    //   2) 将杀带内两侧的摆幅是饱和分的假差值（实测 -199000），不许落进 BIG_SWING；
    //   3) 具体判据（连续必挡 / 战术层接管）压过泛化判据（摆幅大）。
    ScreenConfig config;
    config.opening_plies = 0;
    ReplayAnalyzer analyzer(config);

    // 守卫 1：vcf 的 depth 0 比中位浅 4 层，但战术手不参与深度判据。
    GameRecord guard1;
    guard1.winner = BLACK;
    guard1.reason = GameEndReason::FIVE;
    const int depths1[4] = {4, 4, 0, 0};
    const char* methods1[4] = {"alphabeta", "alphabeta", "vcf", "vcf"};
    for (int i = 0; i < 4; ++i) {
        MoveRecord entry;
        entry.move = Move(7, 7 + i, i % 2 == 0 ? BLACK : WHITE);
        entry.score = 0;
        entry.depth = depths1[i];
        entry.method = methods1[i];
        guard1.moves.push_back(entry);
    }
    GameScreen screen = analyzer.screen(guard1);
    assert(screen.plies[2].reason == ScreenReason::NONE);

    // 守卫 2：两侧都在将杀带内，摆幅 -199000，但这不是形势的摆幅。
    guard1.moves[0].score = 100000;
    guard1.moves[1].score = 99000;
    screen = analyzer.screen(guard1);
    assert(screen.plies[0].reason != ScreenReason::BIG_SWING);
    assert(screen.plies[0].reason == ScreenReason::NONE);

    // 守卫 3：连续两手必挡，第二手同时带着 -199000 的摆幅，标签必须是 must_block_run。
    GameRecord guard3;
    guard3.winner = BLACK;
    guard3.reason = GameEndReason::FIVE;
    const char* methods3[4] = {"alphabeta", "must_block", "must_block", "vcf"};
    const int scores3[4] = {0, 99000, 100000, 0};
    const int depths3[4] = {4, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        MoveRecord entry;
        entry.move = Move(7, 7 + i, i % 2 == 0 ? BLACK : WHITE);
        entry.score = scores3[i];
        entry.depth = depths3[i];
        entry.method = methods3[i];
        guard3.moves.push_back(entry);
    }
    screen = analyzer.screen(guard3);
    assert(screen.plies[0].reason == ScreenReason::METHOD_SHIFT);
    assert(screen.plies[1].reason == ScreenReason::MUST_BLOCK_RUN);

    // 守卫 4：一手安静的 alphabeta 紧接一手 vcf。摆幅 0，但「战术层在这里接管」
    // 本身就是信号，不许被 NOISE 吃掉。两侧都在将杀带内，所以摆幅确实是 0。
    GameRecord guard4;
    guard4.winner = BLACK;
    guard4.reason = GameEndReason::FIVE;
    const char* methods4[4] = {"alphabeta", "alphabeta", "vcf", "vcf"};
    const int scores4[4] = {0, -100000, 100000, 100000};
    for (int i = 0; i < 4; ++i) {
        MoveRecord entry;
        entry.move = Move(7, 7 + i, i % 2 == 0 ? BLACK : WHITE);
        entry.score = scores4[i];
        entry.depth = 4;
        entry.method = methods4[i];
        guard4.moves.push_back(entry);
    }
    screen = analyzer.screen(guard4);
    assert(screen.plies[1].swing == 0);
    assert(screen.plies[1].reason == ScreenReason::METHOD_SHIFT);
    assert(screen.plies[1].candidate);
}

// ---- 设计文档 §6.5–§6.7：段 B / 段 C / 题卡 ----
// 这六个用例钉的是「判定与格式」，不是「棋力」：段 B 的正确性在真实记录上量
// （REVIEW_LOG 轮次145），这里只保证每条判据的算术与序列化不会悄悄变形。

void test_tactics_card_round_trips_every_field() {
    TacticsCard card;
    card.id = "g3-p17";
    card.layer = "normal";
    card.kind = "blunder";
    card.side = WHITE;
    card.ply = 17;
    card.stones = {Move(7, 7, BLACK), Move(7, 8, WHITE), Move(8, 8, BLACK)};
    card.last_move = card.stones.back();
    card.answer = Move(8, 9, WHITE);
    card.alt_answers = {Move(6, 6, WHITE)};
    card.best_score = 3100;
    card.played_score = -1900;
    card.second_score = -1100;
    card.gap = 4200;
    card.loss = 5000;
    card.budget = 20000;
    card.depth = 8;
    card.played = Move(9, 6, WHITE);
    card.second = Move(5, 5, WHITE);
    card.method = "alphabeta";
    card.solve_budget = 16000;
    card.failed_at = 8000;
    card.coeffs_sha = "a3f1b2c4";
    card.source_game = "run_x/records.jsonl";
    card.source_index = 42;

    const std::string text = card.to_json();
    TacticsCard back;
    std::string error;
    assert(TacticsCard::from_json(text, back, error));
    assert(back.id == card.id);
    assert(back.layer == card.layer);
    assert(back.kind == card.kind);
    assert(back.side == card.side);
    assert(back.ply == card.ply);
    assert(back.stones.size() == 3);
    assert(back.stones[1].pos.row == 7 && back.stones[1].pos.col == 8);
    assert(back.stones[1].color == WHITE);
    assert(back.answer.pos.row == 8 && back.answer.pos.col == 9);
    assert(back.alt_answers.size() == 1);
    assert(back.best_score == 3100 && back.played_score == -1900);
    assert(back.gap == 4200 && back.loss == 5000);
    assert(back.played.pos.col == 6 && back.second.pos.row == 5);
    assert(back.method == "alphabeta");
    assert(back.solve_budget == 16000 && back.failed_at == 8000);
    assert(back.coeffs_sha == "a3f1b2c4");
    assert(back.source_game == card.source_game && back.source_index == 42);

    // 缺字段要拒绝，不许静默补默认值：一张没有正解的卡不能进题库。
    TacticsCard broken;
    assert(!TacticsCard::from_json("{\"id\":\"x\"}", broken, error));
}

void test_tactics_layer_boundaries_match_the_doc() {
    // §6.5：<= 8k easy、<= 64k normal、其余 hard；0（梯子跑完也没解出）算 hard。
    assert(TacticsCard::layer_for(0) == "hard");
    assert(TacticsCard::layer_for(1000) == "easy");
    assert(TacticsCard::layer_for(8000) == "easy");
    assert(TacticsCard::layer_for(8001) == "normal");
    assert(TacticsCard::layer_for(64000) == "normal");
    assert(TacticsCard::layer_for(64001) == "hard");
    assert(TacticsCard::layer_at_least("hard", "normal"));
    assert(!TacticsCard::layer_at_least("easy", "normal"));
    assert(TacticsCard::layer_at_least("normal", "normal"));
}

void test_coefficient_fingerprint_is_stable_and_sensitive() {
    ThreatCoefficients a;
    const std::string first = coefficients_fingerprint(a);
    assert(first.size() == 8);
    assert(coefficients_fingerprint(a) == first);  // 稳定

    ThreatCoefficients b = a;
    b.live_three += 1;
    assert(coefficients_fingerprint(b) != first);  // 对改动敏感
}

void test_replay_reviewer_refuses_an_illegal_replay() {
    GameRecord record;
    record.winner = BLACK;
    record.reason = GameEndReason::FIVE;
    MoveRecord first;
    first.move = Move(7, 7, BLACK);
    record.moves.push_back(first);
    MoveRecord second;
    second.move = Move(7, 7, WHITE);  // 同一格
    record.moves.push_back(second);

    ReplayReviewer reviewer(ThreatCoefficients{});
    Board board;
    assert(reviewer.position_before(record, 1, board));   // 只摆第一手
    assert(!reviewer.position_before(record, 2, board));  // 第二手撞格
}

void test_replay_reviewer_scores_its_own_choice_as_lossless() {
    // 段 B 的精确复算路径：先问引擎自己走哪一手，再把那一手当「实际走的那手」
    // 交给复核。loss 必须是 0，且不许被判成俗手——这条抓的是「拿界当精确分」
    // 那一类错误（第一版 12 手全 loss 0/gap 0，看着像对，其实是没算）。
    const Position opening[6] = {{7, 7}, {7, 8}, {8, 8}, {6, 6}, {9, 9}, {8, 7}};
    Board board;
    for (int i = 0; i < 6; ++i) {
        board.place_stone(opening[i].row, opening[i].col, i % 2 == 0 ? BLACK : WHITE);
    }
    SearchEngine engine(ThreatCoefficients{});
    engine.set_book_enabled(false);
    engine.set_max_nodes(20000);
    std::atomic<bool> stop{false};
    const SearchResult think = engine.search(board, BLACK, 6, 60000, stop);
    assert(think.best_move.pos.is_valid());
    assert(!think.root_scores.empty());
    assert(think.root_scores.front().exact);  // 第一手被问时下界是 -INF，所以必精确

    GameRecord record;
    record.winner = BLACK;
    record.reason = GameEndReason::FIVE;
    for (int i = 0; i < 6; ++i) {
        MoveRecord entry;
        entry.move = Move(opening[i].row, opening[i].col, i % 2 == 0 ? BLACK : WHITE);
        entry.method = "replay";
        record.moves.push_back(entry);
    }
    MoveRecord played;
    played.move = think.best_move;
    played.method = "alphabeta";
    record.moves.push_back(played);

    ReplayReviewer reviewer(ThreatCoefficients{});
    const PlyReview review = reviewer.review(record, 6);
    assert(review.root_moves > 0);
    assert(review.kind != "blunder");
    assert(review.kind != "unlisted");
    assert(review.loss == 0);
}

void test_replay_reviewer_prices_a_difficulty_ladder() {
    // 段 C：同一个局面，预算越大越可能解出正解；解出的那一档必须 >= 上一次失败的档。
    const Position opening[4] = {{7, 7}, {7, 8}, {8, 8}, {6, 6}};
    Board board;
    for (int i = 0; i < 4; ++i) {
        board.place_stone(opening[i].row, opening[i].col, i % 2 == 0 ? BLACK : WHITE);
    }
    SearchEngine engine(ThreatCoefficients{});
    engine.set_book_enabled(false);
    engine.set_max_nodes(50000);
    std::atomic<bool> stop{false};
    const SearchResult think = engine.search(board, BLACK, 6, 60000, stop);
    assert(think.best_move.pos.is_valid());

    ReviewConfig config;
    config.ladder_min = 1000;
    config.ladder_max = 64000;
    ReplayReviewer reviewer(ThreatCoefficients{}, config);
    const LadderResult ladder = reviewer.difficulty(board, BLACK, think.best_move);
    assert(ladder.solve_budget > 0);          // 正解是它自己走出来的，必然解得出
    assert(ladder.solve_budget <= 64000);
    assert(ladder.failed_at < ladder.solve_budget);
}


// ---- 设计文档 6.7 a：样本的 teacher_move ----
// 两件事：label 与 teacher 必须分开记；没有老师的样本必须与旧格式逐字节相同。

void test_training_sample_keeps_a_teacher_move() {
    TrainingSample sample;
    sample.features.assign(DataCollector::FEATURE_COUNT, 0.0f);
    sample.value = 1.0f;
    sample.move = Move(9, 6, WHITE);
    sample.teacher_move = Move(8, 9, WHITE);
    sample.teacher_source = "replay";

    const std::string text = sample.to_json();
    TrainingSample back;
    std::string error;
    assert(TrainingSample::from_json(text, back, error));
    assert(back.teacher_move.pos.row == 8 && back.teacher_move.pos.col == 9);
    assert(back.teacher_move.color == WHITE);
    assert(back.teacher_source == "replay");
    // 老师不许覆盖 label：move 仍是当时走的那一手。
    assert(back.move.pos.row == 9 && back.move.pos.col == 6);
}

void test_training_sample_without_a_teacher_keeps_the_old_shape() {
    TrainingSample sample;
    sample.features.assign(DataCollector::FEATURE_COUNT, 0.0f);
    sample.value = 0.0f;
    sample.move = Move(7, 7, BLACK);

    const std::string text = sample.to_json();
    // 没有老师就不许写键：否则旧读法会把「有老师但为空」当成新样本。
    assert(text.find("teacher_move") == std::string::npos);
    assert(text.find("teacher_source") == std::string::npos);

    TrainingSample back;
    std::string error;
    assert(TrainingSample::from_json(text, back, error));
    assert(!back.teacher_move.pos.is_valid());
    assert(back.teacher_source.empty());
}


// 丁-1（v2 §2.1 S3）第一口：连续分量的三个读数各给什么值，全部手算得出。
// 底版是一局九手的黑胜：黑棋最后一手报 100,000（将杀带里），白棋最后一手报 900，
// 分差 99,100 落在 20,000 的尺度上 ⇒ terminal 饱和到 1.0；九手 / max_moves 60 ⇒ speed 0.85；
// 不是认输 ⇒ depth 中性 0.5。合成 = 0.5·1.0 + 0.3·0.85 + 0.2·0.5 = 0.855。
void test_game_margin_answers_its_three_readings_by_value() {
    const int kMaxMoves = 60;
    auto push = [](GameRecord& record, int row, int col, int color, int depth, int score) {
        MoveRecord entry;
        entry.move = Move(row, col, color);
        entry.depth = depth;
        entry.score = score;
        entry.method = "alphabeta";
        record.moves.push_back(entry);
    };

    GameRecord win;
    win.winner = BLACK;
    win.reason = GameEndReason::FIVE;
    const int black_cols[5] = {3, 4, 5, 6, 7};
    const int white_cells[4][2] = {{8, 3}, {8, 8}, {9, 3}, {10, 10}};
    for (int i = 0; i < 4; ++i) {
        push(win, 7, black_cols[i], BLACK, 4, 1000);
        push(win, white_cells[i][0], white_cells[i][1], WHITE, 4, 900);
    }
    push(win, 7, black_cols[4], BLACK, 4, 100000);
    assert(win.moves.size() == 9u);

    const double win_margin = Trainer::game_margin(win, kMaxMoves);
    std::cout << "  five win plies=" << win.moves.size() << " margin=" << win_margin << std::endl;
    assert(std::abs(win_margin - 0.855) < 1e-12);

    // 1) 未分胜负与作废记录都是中性 0.5：连续分量不许替棋盘编一个优势。
    GameRecord draw = win;
    draw.winner = EMPTY;
    draw.reason = GameEndReason::MOVE_LIMIT;
    assert(Trainer::game_margin(draw, kMaxMoves) == 0.5);
    GameRecord broken = win;
    broken.reason = GameEndReason::INVALID;
    assert(Trainer::game_margin(broken, kMaxMoves) == 0.5);

    // 2) 重放与开局库的手不含决策（§6.4 筛题卡同一条纪律）：全是它们时取不到分差 ⇒ 中性。
    GameRecord replayed = win;
    for (auto& entry : replayed.moves) entry.method = "replay";
    assert(Trainer::game_margin(replayed, kMaxMoves) == 0.5);

    // 3) terminal 的尺度真的在起作用：分差只有 2,000 时 terminal = 0.6，不是饱和的 1.0。
    GameRecord quiet_win = win;
    quiet_win.moves.back().score = 2900;
    const double quiet_margin = Trainer::game_margin(quiet_win, kMaxMoves);
    std::cout << "  small score gap 2000, margin=" << quiet_margin << std::endl;
    assert(std::abs(quiet_margin - (0.5 * 0.6 + 0.3 * 0.85 + 0.2 * 0.5)) < 1e-12);

    // 4) 手数那一项：同一终局，赛制越宽，同样的九手显得越「慢」。
    //    max_moves 60 → 30 的差是 0.3·(9/60 - 9/30) = -0.045。
    const double tight_margin = Trainer::game_margin(win, 30);
    assert(std::abs((win_margin - tight_margin) - 0.3 * (9.0 / 30.0 - 9.0 / 60.0)) < 1e-12);

    // 5) 只有认输才有深度差：赢家最后深 8、输家最后深 4 ⇒ 0.2·clamp01(0.5 + 4/8) = 0.2，
    //    比中性多 0.1；反向就是罚 0.1。
    GameRecord resign = win;
    resign.reason = GameEndReason::RESIGN;
    for (auto& entry : resign.moves) {
        if (entry.move.color == WHITE) entry.depth = 4;
    }
    resign.moves.back().depth = 8;
    const double resign_margin = Trainer::game_margin(resign, kMaxMoves);
    assert(std::abs(resign_margin - (win_margin + 0.1)) < 1e-12);

    GameRecord shallow = resign;
    for (auto& entry : shallow.moves) {
        entry.depth = entry.move.color == BLACK ? 4 : 8;
    }
    assert(std::abs(Trainer::game_margin(shallow, kMaxMoves) - (win_margin - 0.1)) < 1e-12);

    // 6) 深度差被尺度截断：差 20 层与差 8 层给同一个顶，「多想四层」拿不到线性奖励。
    GameRecord huge_gap = resign;
    for (auto& entry : huge_gap.moves) {
        entry.depth = entry.move.color == BLACK ? 24 : 4;
    }
    assert(std::abs(Trainer::game_margin(huge_gap, kMaxMoves) - resign_margin) < 1e-12);

    // 7) 战术手报的 depth 是 0（结构性的不是「没想」）：全 0 时深度项回到中性，认输不受罚。
    GameRecord tactical = resign;
    for (auto& entry : tactical.moves) entry.depth = 0;
    assert(std::abs(Trainer::game_margin(tactical, kMaxMoves) - win_margin) < 1e-12);

    std::cout << "test_game_margin_answers_its_three_readings_by_value passed" << std::endl;
}

// 第二口：混合本身。w = 0 必须是**逐字**回到胜负（早退，不是算出来像），
// w >= 1 是纯连续分量，中间是凸组合。
void test_the_margin_blend_is_identity_at_zero_weight() {
    assert(Trainer::blend_fitness(1.0, 0.845, 0.0) == 1.0);
    assert(Trainer::blend_fitness(0.0, 0.845, 0.0) == 0.0);
    assert(Trainer::blend_fitness(0.5, 0.845, 0.0) == 0.5);
    assert(Trainer::blend_fitness(1.0, 0.3, 1.0) == 0.3);
    assert(Trainer::blend_fitness(0.0, 0.7, 1.0) == 0.7);
    assert(std::abs(Trainer::blend_fitness(1.0, 0.5, 0.4) - 0.8) < 1e-12);
    assert(std::abs(Trainer::blend_fitness(0.0, 0.5, 0.4) - 0.2) < 1e-12);
    // 负权重与非有限权重都按「关掉」处理：适配度不能因为一个坏参数就换轴。
    assert(Trainer::blend_fitness(1.0, 0.2, -0.5) == 1.0);
    std::cout << "test_the_margin_blend_is_identity_at_zero_weight passed" << std::endl;
}

// 第三口（旧行为不变 + 新行为）：真下一场两局赛。w = 0 的分数必须还在那张格子上
// {0, 0.5, 1}（两局、每局 0/0.5/1）；同一场赛在 w = 0.4 下被换成
// 0.6 · 胜负 + 0.4 · 连续分量，所以它必须落在 `[0.6·s0, 0.6·s0 + 0.4]` 这条带里
// ——连续分量本身在 [0,1]，这条界是算得出的，不依赖具体棋局走成什么样。
// 两次跑用同一台仪器：全新的 Trainer（rounds_completed_ = 0 ⇒ 同一批开局线）、
// 同一对系数、同一颗节点帽（800/手，定得住，`--max-nodes` 的口径）。
void test_a_margin_weight_blends_the_real_match_score_off_the_lattice() {
    PatternRecognizer::init();
    TrainingConfig config;
    config.rounds = 1;
    config.search_depth = 2;
    config.timeout_ms = 5000;
    config.max_nodes = 800;
    config.max_moves = 14;
    config.fitness_games = 2;
    config.verification_games = 2;
    config.sprt_elo = 0;        // 不打断：两局都要真下完，格点判定才有意义
    config.resign_enabled = false;
    config.collect_data = false;
    config.params_path = temp_path("margin_blend_params.json").string();
    config.checkpoint_path = temp_path("margin_blend_checkpoint.json").string();

    const CoefficientPair candidate = CoefficientPair::shared(ThreatCoefficients());
    const CoefficientPair reference = CoefficientPair::shared(target_coefficients());

    Trainer plain(ThreatCoefficients(), config);
    const MatchOutcome binary = plain.score_match(candidate, reference, 2, 0.0);
    assert(binary.games == 2);
    const double s0 = binary.score;
    // 两局、每局 {0, 0.5, 1} ⇒ 平均分只能落在 0.25 的格子上（轮次128 记的那 5 档）。
    const bool on_lattice = std::abs(s0 * 4.0 - std::round(s0 * 4.0)) < 1e-9;
    std::cout << "  w=0 score " << s0 << " games " << binary.games << std::endl;
    assert(on_lattice);

    Trainer blended(ThreatCoefficients(), config);
    const MatchOutcome outcome = blended.score_match(candidate, reference, 2, 0.4);
    const double s = outcome.score;
    std::cout << "  w=0.4 score " << s << std::endl;
    assert(outcome.games == 2);
    assert(s >= 0.0 && s <= 1.0);
    // 分数被换成 0.6·胜负 + 0.4·连续分量，而连续分量在 [0,1]：这两条界是算得出的，
    // 不依赖这局棋具体走成什么样。
    assert(s >= 0.6 * s0 - 1e-12 && s <= 0.6 * s0 + 0.4 + 1e-12);
    assert(std::abs(s - s0) <= 0.4 + 1e-12);
    // 这一场里至少有一局是分出胜负的（s0 = 0.25 只能是「一和一滴」），
    // 所以 w = 0.4 之后分数不可能还在那 5 档格点上——这就是「档数从 5 涨成连续」
    // 在单元层能钉到的那一句（另一句在 CLI 读数里）。
    const bool still_on_lattice = std::abs(s * 4.0 - std::round(s * 4.0)) < 1e-9;
    assert(!still_on_lattice);

    std::cout << "test_a_margin_weight_blends_the_real_match_score_off_the_lattice passed"
              << std::endl;
}

// 丁-2（v2 §3.4 S4）第一口：`MoveRecord::teacher_*` 的往返，以及「没有老师时逐字节不变」。
// 这四个键是**可选**的：缺省时一条都不写，所以蒸馏关着的每一行 JSONL 与 S4 之前同形；
// 写的时候老师的颜色跟着这一手走（老师与学生在同一个局面上下同一色），不必重复存。
void test_teacher_fields_round_trip_and_stay_absent_when_off() {
    GameRecord record;
    record.winner = BLACK;
    record.reason = GameEndReason::FIVE;

    MoveRecord ply;
    ply.move = Move(7, 7, BLACK);
    ply.score = 1234;
    ply.depth = 2;
    ply.method = "alphabeta";
    record.moves.push_back(ply);

    // 1) 没有老师：整行里不该出现 "teacher" 这个子串（轮次147 给样本钉的是同一条纪律）。
    const std::string untaught = record.to_json();
    assert(untaught.find("teacher") == std::string::npos);
    GameRecord untaught_back;
    std::string error;
    assert(GameRecord::from_json(untaught, untaught_back, error));
    assert(!untaught_back.moves[0].teacher_move.pos.is_valid());
    assert(untaught_back.moves[0].teacher_depth == 0);
    assert(untaught_back.moves[0].teacher_score == 0);

    // 2) 有老师：四个键写出来，读回来格号与分值都在，颜色继承这一手的颜色。
    record.moves[0].teacher_move = Move(8, 8, WHITE);   // 故意给错颜色，读回来必须仍是 BLACK
    record.moves[0].teacher_depth = 6;
    record.moves[0].teacher_score = -4200;
    const std::string taught = record.to_json();
    assert(taught.find("\"teacher_depth\":6") != std::string::npos);
    assert(taught.find("\"teacher_score\":-4200") != std::string::npos);
    GameRecord taught_back;
    assert(GameRecord::from_json(taught, taught_back, error));
    assert(taught_back.moves[0].teacher_move.pos.row == 8 &&
           taught_back.moves[0].teacher_move.pos.col == 8);
    assert(taught_back.moves[0].teacher_move.color == BLACK);
    assert(taught_back.moves[0].teacher_depth == 6);
    assert(taught_back.moves[0].teacher_score == -4200);
    // 学生的账一字不动：老师是**多记的一列**，不是被改写的读数。
    assert(taught_back.moves[0].depth == 2 && taught_back.moves[0].score == 1234);

    std::cout << "test_teacher_fields_round_trip_and_stay_absent_when_off passed" << std::endl;
}

// 第二口：SelfPlay 真的在落子之前问了老师，而且**没有改变学生下的那一手**。
// 后半句是 §4 丁-2 注意 1 的实质：老师的条目若写进学生的表，随后的学生搜索会直接切在
// 更深的条目上，`depth`/`nodes` 就不是「当时那一手搜了多深」。这里同一所（同一份系数、
// 同一对引擎形状）下两遍——一遍关老师、一遍开老师（老师用的是另一张表）——
// 逐手比对学生的着法、深度、节点与分值。两边都把时限放到远在水位之上、只认节点帽，
// 所以「一字不动」这条断言不由机器快慢决定（§9.2：定深臂用节点帽当超时）。
void test_self_play_records_a_teacher_answer_without_touching_the_student() {
    PatternRecognizer::init();
    const ThreatCoefficients coeffs = ThreatCoefficients();

    SelfPlayConfig plain_settings;
    plain_settings.max_depth = 2;
    plain_settings.timeout_ms = 20000;
    plain_settings.max_nodes = 2500;
    plain_settings.max_moves = 10;

    SelfPlayConfig taught_settings = plain_settings;
    taught_settings.teacher_depth = 4;
    taught_settings.teacher_max_nodes = 20000;

    GameRecord plain;
    {
        SearchEngine student(coeffs);
        student.set_book_enabled(false);
        SelfPlay play(student, student, plain_settings);
        plain = play.run({}, nullptr, nullptr);
    }

    GameRecord taught;
    {
        SearchEngine student(coeffs);
        student.set_book_enabled(false);
        TranspositionTable teacher_table(1 << 18, true);
        SearchEngine teacher_engine(coeffs, &teacher_table);
        teacher_engine.set_book_enabled(false);
        teacher_engine.set_resign_detection(false);
        assert(&student.transposition_table() != &teacher_engine.transposition_table());

        SelfPlay play(student, student, taught_settings);
        play.set_teacher(&teacher_engine);
        taught = play.run({}, nullptr, nullptr);
    }

    assert(plain.reason != GameEndReason::INVALID);
    assert(taught.reason != GameEndReason::INVALID);
    assert(!plain.moves.empty() && !taught.moves.empty());

    // 1) 每一个引擎手都带回了老师的答案，深度记的是请求的那一档（注意 3）。
    //    「在落子之前搜」钉成可核的形式：把记录重放一遍，老师答的那一格在**那一手之前**
    //    必须是空的（注意 2）——落子之后再搜就永远不可能说出这一格。
    Board replay;
    int same_as_student = 0;
    for (const MoveRecord& entry : taught.moves) {
        assert(entry.teacher_depth == 4);
        assert(entry.teacher_move.pos.is_valid());
        assert(replay.is_empty(entry.teacher_move.pos));
        if (entry.teacher_move.pos == entry.move.pos) ++same_as_student;
        replay.place_stone(entry.move.pos, entry.move.color);
    }
    assert(same_as_student > 0);   // 师生一致的那几手就是「先搜后落」的物证
    // 2) 关着的时候一个老师字段都不写。
    for (const MoveRecord& entry : plain.moves) {
        assert(entry.teacher_depth == 0);
        assert(!entry.teacher_move.pos.is_valid());
    }
    // 3) 学生的读数一字不动。
    assert(plain.moves.size() == taught.moves.size());
    for (size_t i = 0; i < plain.moves.size(); ++i) {
        if (!(plain.moves[i].move == taught.moves[i].move) ||
            plain.moves[i].depth != taught.moves[i].depth ||
            plain.moves[i].nodes != taught.moves[i].nodes ||
            plain.moves[i].score != taught.moves[i].score) {
            std::cout << "  ply " << i << " student moved: (" << plain.moves[i].move.pos.row
                      << "," << plain.moves[i].move.pos.col << ") d" << plain.moves[i].depth
                      << " n" << plain.moves[i].nodes << " vs ("
                      << taught.moves[i].move.pos.row << "," << taught.moves[i].move.pos.col
                      << ") d" << taught.moves[i].depth << " n" << taught.moves[i].nodes
                      << std::endl;
        }
        assert(plain.moves[i].move == taught.moves[i].move);
        assert(plain.moves[i].depth == taught.moves[i].depth);
        assert(plain.moves[i].nodes == taught.moves[i].nodes);
        assert(plain.moves[i].score == taught.moves[i].score);
    }

    std::cout << "test_self_play_records_a_teacher_answer_without_touching_the_student passed ("
              << taught.moves.size() << " plies distilled)" << std::endl;
}

// 第三口：蒸馏让战术层的 ply 第一次进得了样本。
// `DataCollector` 的门槛是 `entry.depth >= min_search_depth_`（缺省 1），而 vcf / vct /
// must_block 报的 depth 是 0——轮次125 那句「14/28/62 → 112/224/496 条样本，差的就是这几类 ply」
// 就是这个门槛造成的。现在「这一手有老师的答案」也算数，但 `search_depth` 仍然记学生自己的
// 0（拿老师的深度去补它同样是污染，只是换个方向）。
void test_a_taught_tactical_ply_becomes_a_sample() {
    GameRecord record;
    record.winner = BLACK;
    record.reason = GameEndReason::FIVE;
    const int black_cols[5] = {6, 7, 8, 9, 5};
    const int white_cells[4][2] = {{8, 6}, {8, 7}, {8, 8}, {6, 6}};
    for (int i = 0; i < 4; ++i) {
        MoveRecord black;
        black.move = Move(7, black_cols[i], BLACK);
        black.score = 500;
        black.depth = 2;
        black.method = "alphabeta";
        record.moves.push_back(black);
        MoveRecord white;
        white.move = Move(white_cells[i][0], white_cells[i][1], WHITE);
        white.score = 480;
        white.depth = 2;
        white.method = "alphabeta";
        record.moves.push_back(white);
    }
    MoveRecord finish;
    finish.move = Move(7, black_cols[4], BLACK);   // (7,5)：补上 7 行 5..9 的整五
    finish.score = 100000;
    finish.depth = 0;                                // 战术层不回填深度
    finish.method = "vcf";
    record.moves.push_back(finish);
    assert(record.moves.size() == 9u);

    DataCollector collector;
    const std::vector<TrainingSample> before = collector.collect(record, 1, false);
    // 九手里只有最后那一手是 depth 0 ⇒ 被门槛挡掉，前面八手各出一条。
    assert(static_cast<int>(before.size()) == 8);
    for (const TrainingSample& sample : before) assert(sample.teacher_source.empty());

    record.moves.back().teacher_move = Move(7, 4, BLACK);
    record.moves.back().teacher_depth = 5;
    record.moves.back().teacher_score = 100000;
    const std::vector<TrainingSample> after = collector.collect(record, 1, false);
    assert(static_cast<int>(after.size()) == 9);

    const TrainingSample& taught = after.back();
    assert(taught.move_index == 8);
    assert(taught.teacher_source == "distill");
    assert(taught.teacher_move.pos.row == 7 && taught.teacher_move.pos.col == 4);
    assert(taught.teacher_move.color == BLACK);
    assert(taught.move.pos.row == 7 && taught.move.pos.col == 5);
    assert(taught.search_depth == 0);   // 学生的深度不被老师的深度改写
    assert(taught.value == 1.0f);

    // 增广的每一个朝向都要跟着转：老师与学生在同一个局面上，样本换了镜像而老师不换，
    // 等于给同一个位置配了另一个位置的答案。
    const std::vector<TrainingSample> wide = collector.collect(record, 2, true);
    int taught_variants = 0;
    for (const TrainingSample& sample : wide) {
        if (sample.move_index != 8) continue;
        ++taught_variants;
        assert(sample.teacher_source == "distill");
        assert(sample.teacher_move.pos.is_valid());
        assert(!(sample.teacher_move.pos == sample.move.pos));
    }
    assert(taught_variants > 1);

    std::cout << "test_a_taught_tactical_ply_becomes_a_sample passed (" << taught_variants
              << " oriented samples for the taught ply)" << std::endl;
}

// 第四口（§4 丁-2 注意 4 的那个顺手 bug）：`reset()` 以前连起始线一起清掉，
// worker 每轮只设一次开局，于是重试的那几局都从空盘开始——「悔棋再战」退到的是另一棵树。
// 钉两件事：reset 之后当前线仍然是那条开局；树被丢掉（下一局从这条线重新分叉）。
void test_reset_returns_to_the_start_line_not_the_empty_board() {
    PatternRecognizer::init();
    const ThreatCoefficients coeffs = ThreatCoefficients();
    SearchEngine engine(coeffs);
    engine.set_book_enabled(false);

    SelfPlayConfig settings = quick_play_config();
    settings.max_nodes = 2000;
    settings.timeout_ms = 20000;

    RetryExplorer explorer(engine, engine, settings);
    const std::vector<Move> line = {Move(7, 7, BLACK), Move(7, 8, WHITE), Move(6, 6, BLACK)};
    explorer.set_start_line(line);
    assert(explorer.current_line().size() == 3u);

    GameRecord record;
    std::atomic<bool> never(false);
    assert(explorer.explore(record, &never));
    // 退到分叉点之后当前线可能变短，但它永远不该掉回空盘。
    assert(explorer.current_line().size() >= 1u);

    explorer.reset();
    assert(explorer.current_line().size() == 3u);
    assert(explorer.current_line()[0].pos.row == 7 && explorer.current_line()[0].pos.col == 7);
    assert(explorer.current_line()[2].pos.row == 6 && explorer.current_line()[2].pos.col == 6);

    std::cout << "test_reset_returns_to_the_start_line_not_the_empty_board passed" << std::endl;
}

// 丁-3（v2 §3.4 S6）验收 4 —— 序列化兼容。指导书 §4 丁-3 注意 3：「单份文件读成两侧
// 同值，不要造新格式」。钉三件事：两侧同值的那一对写出的字节与旧的一套**逐字相同**；
// 旧文件读回来两侧同值；两侧真的分开了才多一个 "white" 子对象，而只会读单套的那条路
// 遇到这种文件必须**拒绝**——把黑侧当成两套会静默丢掉 丁-4 的补偿。
void test_d3_flat_file_reads_as_shared_and_a_split_file_adds_one_object() {
    const ThreatCoefficients set = target_coefficients();
    std::string error;

    // 1) 旧格式 → 两侧同值。
    const std::string path = temp_path("d3_flat_params.json").string();
    assert(CoefficientVector::save(path, set, error));
    CoefficientPair loaded;
    assert(CoefficientVector::load(path, loaded, error));
    assert(loaded.identical());
    assert(CoefficientVector::to_vector(loaded.black) == CoefficientVector::to_vector(set));
    assert(CoefficientVector::to_vector(loaded.white) == CoefficientVector::to_vector(set));

    // 2) 两侧同值写出来的字节 = 改之前的字节（checkpoint 的 versions[] 就是这个形状）。
    const std::string flat_bytes = CoefficientVector::to_json_string(set);
    assert(CoefficientVector::to_json_string(loaded) == flat_bytes);
    assert(flat_bytes.find("white") == std::string::npos);

    // 3) 两侧分开：多一个子对象，读回来各是各的。
    CoefficientPair split = CoefficientPair::shared(set);
    split.white.edge_penalty = 40;
    split.white.position_weight_scale = 1;
    const std::string paired_bytes = CoefficientVector::to_json_string(split);
    assert(paired_bytes.find("\"white\"") != std::string::npos);
    CoefficientPair back;
    assert(CoefficientVector::pair_from_json_string(paired_bytes, back, error));
    assert(back.white.edge_penalty == 40);
    assert(back.black.edge_penalty == set.edge_penalty);
    assert(CoefficientVector::to_vector(back) == CoefficientVector::to_vector(split));

    // 4) 单套读法拒绝按色的文件（而不是拿黑侧糊过去）。
    ThreatCoefficients single;
    assert(!CoefficientVector::from_json_string(paired_bytes, single, error));
    assert(error.find("pair") != std::string::npos);
    std::cout << "  [丁-3] flat " << flat_bytes.size() << " bytes, paired "
              << paired_bytes.size() << " bytes, refusal: " << error << std::endl;

    std::cout << "test_d3_flat_file_reads_as_shared_and_a_split_file_adds_one_object passed"
              << std::endl;
}

// 丁-3 注意 6 —— 基因数从 14 变 28，`to_vector/from_vector` 跟着改。钉的是索引口径
// （`side * TUNABLE_COUNT + gene`，黑 0..13 / 白 14..27）、宽度守恒（14 进来 14 出去，
// 28 进来 28 出去）与边界查表也按同一个口径。
void test_d3_gene_vector_is_side_major_and_keeps_its_width() {
    assert(PAIR_GENE_COUNT == 28);
    assert(TUNABLE_COUNT == 14);

    CoefficientPair pair;
    pair.white.cand_width_root = 30;
    const std::vector<int> genes = CoefficientVector::to_vector(pair);
    assert(genes.size() == static_cast<size_t>(PAIR_GENE_COUNT));
    const std::vector<int> black_genes = CoefficientVector::to_vector(pair.black);
    const std::vector<int> white_genes = CoefficientVector::to_vector(pair.white);
    assert(black_genes.size() == static_cast<size_t>(TUNABLE_COUNT));
    for (int g = 0; g < TUNABLE_COUNT; ++g) {
        assert(genes[black_gene(static_cast<TunableParam>(g))] == black_genes[g]);
        assert(genes[white_gene(static_cast<TunableParam>(g))] == white_genes[g]);
    }

    // 宽度守恒：一个扁平基因组仍然是扁平的（旧 checkpoint 与旧基因组都走这条路），
    // 不许被「配对化」撑成 28 个。
    std::vector<int> flat = CoefficientVector::to_vector(ThreatCoefficients());
    assert(flat.size() == static_cast<size_t>(TUNABLE_COUNT));
    CoefficientVector::sanitize_pair_vector(flat);
    assert(flat.size() == static_cast<size_t>(TUNABLE_COUNT));
    const CoefficientPair from_flat = CoefficientVector::pair_from_vector(flat);
    assert(from_flat.identical());

    // sanitize_vector 按宽度决定它洗的是什么：同一个 28 wide 的向量交给它，结果与
    // sanitize_pair_vector 一致（否则优化器有一半的基因是被错的规则修的）。
    std::vector<int> wide = genes;
    wide[white_gene(TunableParam::CAND_WIDTH_ROOT)] = 999;   // 越界，被夹回 64
    wide[black_gene(TunableParam::SLEEPY_TWO)] = 9000;        // 比活四还高，被阶梯压回
    std::vector<int> via_vector = wide;
    CoefficientVector::sanitize_vector(via_vector);
    std::vector<int> via_pair = wide;
    CoefficientVector::sanitize_pair_vector(via_pair);
    assert(via_vector == via_pair);
    assert(via_vector[white_gene(TunableParam::CAND_WIDTH_ROOT)] == 64);
    assert(via_vector[black_gene(TunableParam::SLEEPY_TWO)] ==
           via_vector[black_gene(TunableParam::LIVE_TWO)]);

    // 边界查表也是同一个口径：第 20 号基因就是白侧的第 6 号。
    assert(CoefficientVector::bound_for_gene(20).min_value ==
           CoefficientVector::bound_for(static_cast<TunableParam>(20 - TUNABLE_COUNT)).min_value);
    assert(CoefficientVector::bound_for_gene(0).max_value ==
           CoefficientVector::bound_for(TunableParam::LIVE_FOUR).max_value);

    std::cout << "test_d3_gene_vector_is_side_major_and_keeps_its_width passed" << std::endl;
}

// 丁-3 注意 4 + 5 —— 两套系数各洗各的（`sanitize` 与 `CrossRule` 逐色跑一遍），
// 再叠一条「威胁阶梯两侧必须同阶（±20 %）」：越带的是**白侧**被拉回带内（黑侧是基准），
// 位置项没有带。文件那一侧同一条判据是**拒绝**，不是修。
void test_d3_ladder_band_moves_white_and_the_reader_refuses() {
    const ThreatCoefficients base;
    CoefficientPair pair = CoefficientPair::shared(base);
    pair.white.live_four = 40000;     // 黑侧 10000，超带三倍
    pair.white.rush_four = 9000;      // 黑侧 1000
    pair.white.edge_penalty = 140;    // 位置项，没有带
    CoefficientVector::sanitize(pair);
    // 带顶 = 10000 × 1.2；先落带，再按白侧自己的阶梯把 冲四 压到 活四 之下。
    assert(pair.white.live_four == 12000);
    assert(pair.white.rush_four == 1200);
    assert(pair.black.live_four == 10000);
    assert(pair.white.edge_penalty == 140);
    // 幂等：再洗一次一个字都不动（优化器每步都洗，不幂等的话冠军会自己漂走）。
    const std::vector<int> once = CoefficientVector::to_vector(pair);
    CoefficientVector::sanitize(pair);
    assert(CoefficientVector::to_vector(pair) == once);

    // 文件那一侧：同样一个越带的白侧，读它必须失败。白侧自己的阶梯要先合法，否则
    // 报出来的是「组合分低于活四」那条单色判据，±20 % 这一条根本没被跑到。
    CoefficientPair wild = CoefficientPair::shared(ThreatCoefficients());
    wild.white.live_four = 40000;
    wild.white.rush_four = 1000;
    wild.white.live_three = 900;
    wild.white.sleepy_three = 100;
    wild.white.live_two = 50;
    wild.white.sleepy_two = 10;
    wild.white.double_four = 50000;
    wild.white.double_three = 45000;
    wild.white.four_three = 42000;
    std::string error;
    assert(!CoefficientVector::pair_from_json_string(
        CoefficientVector::to_json_string(wild), wild, error));
    assert(error.find("live_four") != std::string::npos);
    // 钉的是「哪一条判据把这份文件拦下来的」：带，不是单色的阶梯。
    assert(error.find("away from black") != std::string::npos);
    std::cout << "  [丁-3] band refusal: " << error << std::endl;

    // 规则常量两侧必须同值：`five` 是将杀阈值、帽取两侧较小者，一个「黑 88888 白 100000」
    // 的文件会把将杀带切成两种颜色。
    CoefficientPair rulesplit = CoefficientPair::shared(ThreatCoefficients());
    rulesplit.white.five = 90000;
    assert(!CoefficientVector::pair_from_json_string(
        CoefficientVector::to_json_string(rulesplit), rulesplit, error));
    assert(error.find("both colours") != std::string::npos);

    std::cout << "test_d3_ladder_band_moves_white_and_the_reader_refuses passed" << std::endl;
}

// 丁-6（v2 §3.2 S2）第一条：判据本身。指导书思路 2 写的是「双方胜率落在 [0.25,0.75]」，
// 轮次160 实测那条点不着（同一座位的 K 局逐字相同 ⇒ 每色胜率只能是 0 或 1），所以按注意 2
// 「换判据而不是放宽阈值」换成：**两局由不同座位赢 = 座位不决定结果 = 这一对能区分玩家**。
// 这里不打对局，直接摆读数——判据是纯函数，摆出来才钉得住每一个分支。
void test_d6_keeps_only_the_pairs_the_seat_does_not_decide() {
    OpeningScreenReport report;
    report.config.candidates = 5;
    report.config.path = temp_path("d6_openings.json").string();

    auto add = [&](int as_black, int as_white, int games) {
        OpeningReading reading;
        reading.line.push_back(Move(7, 7, BLACK));
        reading.line.push_back(Move(7, 8, WHITE));
        reading.line.push_back(Move(8, 7, BLACK));
        reading.as_candidate_black = as_black;
        reading.as_candidate_white = as_white;
        reading.games = games;
        reading.plies = 20;
        reading.valid = games == 2 && as_black != 2 && as_white != 2 && as_black != 0 &&
                        as_white != 0;
        reading.seat_decided = !reading.discriminates();
        reading.kept = reading.discriminates();
        report.readings.push_back(reading);
    };
    add(1, -1, 2);   // 黑座位赢第一局、白座位赢第二局 ⇒ 玩家决定 ⇒ 留
    add(-1, 1, 2);   // 同上，方向反过来 ⇒ 留
    add(1, 1, 2);    // 黑座位通吃 ⇒ 这一对恒为 0.5 ⇒ 剔
    add(-1, -1, 2);  // 白座位通吃 ⇒ 剔
    add(0, 1, 2);    // 有一局未分胜负 ⇒ 不判、也不计进「筛前那一档」
    add(2, 1, 0);    // 有一局作废 ⇒ 同上

    assert(report.lines().size() == 2u);
    assert(report.readings[0].discriminates() && report.readings[1].discriminates());
    assert(!report.readings[2].discriminates() && !report.readings[3].discriminates());
    assert(!report.readings[4].valid && !report.readings[5].valid);

    // 落盘 → 读回：计数从读数重算，`reused` 标出「一份局都没跑」。
    std::string error;
    report.set_sha = "hand-built";   // save 只写不带校验的字段，load 要比的是调用方给的 sha
    assert(OpeningScreen::save(report, error));
    OpeningScreenReport back;
    back.config = report.config;
    assert(OpeningScreen::load(report.config.path, "hand-built", back, error));
    assert(back.reused);
    assert(back.readings.size() == 6u);
    assert(back.kept == 2);
    assert(back.swept_before == 2);   // 两条被座位通吃的；未分/作废的不算
    assert(back.swept_after == 0);   // 留下的里面一个通吃都不该有（判据正确性的那一句）
    assert(back.lines().size() == 2u);
    assert(back.set_sha == "hand-built");

    // sha 不匹配 = 系数或档位换了：旧集不作数，错误里两个键都要能看见。
    OpeningScreenReport stale;
    stale.config = report.config;
    assert(!OpeningScreen::load(report.config.path, "other-key", stale, error));
    assert(error.find("hand-built") != std::string::npos);
    assert(error.find("other-key") != std::string::npos);
    std::cout << "  [丁-6] kept " << back.kept << "/6 readings, seat-decided before "
              << back.swept_before << ", after " << back.swept_after << std::endl;

    std::filesystem::remove(report.config.path);
    std::cout << "test_d6_keeps_only_the_pairs_the_seat_does_not_decide passed" << std::endl;
}

// 丁-6 第二条：`coeffs_sha` 是真的键。四半套系数（两个玩家 × 两侧）与档位四项，动任何
// 一个都必须换键——否则 丁-4/丁-6 换过系数以后，旧筛选集会静默冒充新那一代的结果。
void test_d6_the_screen_key_moves_with_either_player_and_with_the_regime() {
    const CoefficientPair same;
    OpeningScreenConfig cfg;
    cfg.candidates = 8;
    const std::string base = OpeningScreen::set_sha(same, same, cfg);
    assert(base == OpeningScreen::set_sha(same, same, cfg));   // 稳定：同输入同键
    // 键的开头确实是那四个指纹按顺序拼起来的（不是自己另起一套哈希），后面才是档位。
    const std::string wanted_prefix = coefficients_fingerprint(same.black) + "-" +
                                      coefficients_fingerprint(same.white) + "-" +
                                      coefficients_fingerprint(same.black) + "-" +
                                      coefficients_fingerprint(same.white);
    assert(base.substr(0, wanted_prefix.size()) == wanted_prefix);
    const std::string regime = base.substr(wanted_prefix.size());
    assert(regime == "-s" + std::to_string(cfg.stones) + "d" + std::to_string(cfg.depth) + "n" +
                     std::to_string(cfg.max_nodes) + "c" + std::to_string(cfg.candidates) + "k" +
                     std::to_string(cfg.seed_stride));

    CoefficientPair a_black_changed = same;
    a_black_changed.black.rush_four = 900;
    assert(OpeningScreen::set_sha(a_black_changed, same, cfg) != base);
    CoefficientPair a_white_changed = same;
    a_white_changed.white.position_weight_scale = 5;   // 丁-3 之后白侧也能单独不同
    assert(OpeningScreen::set_sha(a_white_changed, same, cfg) != base);
    CoefficientPair b_black_changed = same;
    b_black_changed.black.live_three = 400;
    assert(OpeningScreen::set_sha(same, b_black_changed, cfg) != base);
    CoefficientPair b_white_changed = same;
    b_white_changed.white.edge_penalty = 140;
    assert(OpeningScreen::set_sha(same, b_white_changed, cfg) != base);

    // 两侧同值的两套系数与「一套系数」是同一件事，但两个玩家互换不是：键里前四半是
    // 候选、后四半是参照，反过来就是另一个键（筛选问的是「这一对」谁能分开）。
    CoefficientPair shifted = same;
    shifted.white.cand_width_root = 20;
    assert(OpeningScreen::set_sha(shifted, same, cfg) != OpeningScreen::set_sha(same, shifted, cfg));

    OpeningScreenConfig fewer = cfg;
    fewer.candidates = 7;
    assert(OpeningScreen::set_sha(same, same, fewer) != base);
    OpeningScreenConfig deeper = cfg;
    deeper.depth = 5;
    assert(OpeningScreen::set_sha(same, same, deeper) != base);
    OpeningScreenConfig wider = cfg;
    wider.max_nodes = 2000;
    assert(OpeningScreen::set_sha(same, same, wider) != base);
    OpeningScreenConfig longer = cfg;
    longer.stones = 4;
    assert(OpeningScreen::set_sha(same, same, longer) != base);
    OpeningScreenConfig other_seeds = cfg;
    other_seeds.seed_stride = 2000;   // 换一批候选开局 = 换键，不能拿旧集的读数冒充
    assert(OpeningScreen::set_sha(same, same, other_seeds) != base);

    std::cout << "  [丁-6] key " << base << std::endl;
    std::cout << "test_d6_the_screen_key_moves_with_either_player_and_with_the_regime passed"
              << std::endl;
}

// 丁-6 第三条（旧行为不变）：缺省关着时，开局一处都不动。`match_opening_for` 在没有筛出
// 可用集时必须逐字回到 `match_opening_line`——这条不是「大概一样」，是三档 × 16 个索引
// 的逐手比对。
void test_d6_a_closed_screen_leaves_the_fixed_rotation_byte_for_byte() {
    TrainingConfig config;
    assert(config.opening_screen_candidates == 0);   // 缺省 = 关
    Trainer trainer(ThreatCoefficients(), config);

    const int plies[] = {0, 2, 4};
    for (const int p : plies) {
        config.match_opening_plies = p;
        Trainer scoped(ThreatCoefficients(), config);
        for (int i = 0; i < 16; ++i) {
            const std::vector<Move> today = scoped.match_opening_line(i, p);
            const std::vector<Move> routed = scoped.match_opening_for(i);
            assert(routed.size() == today.size());
            for (size_t s = 0; s < today.size(); ++s) {
                assert(routed[s].pos.row == today[s].pos.row &&
                       routed[s].pos.col == today[s].pos.col && routed[s].color == today[s].color);
            }
        }
    }
    // 关着的时候既不落盘也不跑局：`openings_screen_done_` 翻起来了，集合仍是空的。
    assert(trainer.screened_openings_.empty());
    assert(trainer.games_played() == 0);
    std::filesystem::path cache = std::filesystem::temp_directory_path() /
                                  "gomoku_training_d6_openings.json";
    std::error_code ec;
    assert(!std::filesystem::exists(cache, ec));

    // 反过来：筛出 ≥ `kMatchOpeningSet` 条之后，配对局真的从这套里取，而且取法是
    // 「(轮次 + 对序) mod 池大小」——同一轮里候选与冠军必须拿到同一批局面，否则配对
    // 就不是配对。这里直接把池子注入进去（`openings_screen_done_` 置真 = 这一代已经筛过），
    // 量的是「用没用 + 怎么取」，不掺对局，也不会往 data/ 落盘。
    TrainingConfig armed;
    armed.match_opening_plies = 0;
    armed.opening_screen_candidates = 0;
    armed.openings_path = temp_path("d6_unused_cache.json").string();
    Trainer with_pool(ThreatCoefficients(), armed);
    with_pool.openings_screen_done_ = true;
    for (int i = 0; i < 9; ++i) {
        std::vector<Move> marker;
        marker.push_back(Move(0, i, BLACK));
        marker.push_back(Move(1, i, WHITE));
        marker.push_back(Move(2, i, BLACK));
        with_pool.screened_openings_.push_back(marker);
    }
    assert(Trainer::kMatchOpeningSet == 8);
    for (int i = 0; i < 20; ++i) {
        const std::vector<Move> picked = with_pool.match_opening_for(i);
        assert(picked.size() == 3u);
        assert(picked[0].pos.row == 0 && picked[0].pos.col == i % 9);   // 用的是池子，不是天元开局
        assert(picked[1].color == WHITE && picked[2].color == BLACK);
        // 同一索引两次取到同一条：配对的两局必须同一局面，否则颜色差又被算进读数里。
        assert(with_pool.match_opening_for(i)[1].pos.col == picked[1].pos.col);
    }
    assert(with_pool.games_played() == 0);   // 注入的这一代一局都没跑
    // 池子不够（< 8）时退回固定轮换：这一支是杀线后半段的出口，不能静默用半套。
    Trainer short_pool(ThreatCoefficients(), armed);
    short_pool.openings_screen_done_ = true;
    for (int i = 0; i < 7; ++i) {
        std::vector<Move> marker;
        marker.push_back(Move(0, i, BLACK));
        short_pool.screened_openings_.push_back(marker);
    }
    assert(short_pool.match_opening_for(3) == Trainer::match_opening_line(3, 0));

    std::cout << "test_d6_a_closed_screen_leaves_the_fixed_rotation_byte_for_byte passed"
              << std::endl;
}

// 丁-6 第四条（成本，也就是这一单元的杀线口径）：筛 N 条候选要付 2N 局，且同一代第二次
// 用必须从盘上读回来、一局不跑。档取训练真用的那一档（depth 4、节点帽 1500/手、max_moves 60）——
// 判定用节点帽，所以这四个读数是确定的（§9.2：搜索类断言的超时就是节点帽）。
void test_d6_two_candidates_cost_four_games_and_the_second_use_reads_the_cache() {
    OpeningScreenConfig cfg;
    cfg.candidates = 2;
    cfg.stones = 3;
    cfg.depth = 4;
    cfg.max_nodes = 1500;
    cfg.max_moves = 60;
    cfg.path = temp_path("d6_screen_cache.json").string();

    CoefficientPair pair;
    pair.white.rush_four = 300;   // 轮次160 实测「能动这一读的只有威胁阶梯」那一项
    std::string error;
    const OpeningScreenReport first = OpeningScreen::run(pair, CoefficientPair(), cfg, error);
    assert(error.empty());
    assert(first.reused == false);
    assert(first.readings.size() == 2u);
    assert(first.games_played == 4);           // 2 条候选 × 两局，一分不多
    // 两条候选各是一个分支：第 0 条黑座位通吃（剔），第 1 条两座位各赢一局（留）。
    assert(first.readings[0].as_candidate_black == 1 && first.readings[0].as_candidate_white == 1);
    assert(first.readings[0].valid && !first.readings[0].kept);
    assert(first.readings[1].as_candidate_black == 1 && first.readings[1].as_candidate_white == -1);
    assert(first.readings[1].kept);
    assert(first.kept == 1);
    assert(first.swept_before == 1);
    assert(first.swept_after == 0);            // 留下的里面没有一条还被座位决定
    assert(first.lines().size() == 1u);
    assert(first.lines()[0].size() == 3u);
    std::cout << "  [丁-6] screen of 2: kept " << first.kept << " swept " << first.swept_before
              << " games " << first.games_played << std::endl;

    // 存下来再读：同一代不重筛（`games_played == 0` 就是「一局都没跑」的凭据）。
    assert(OpeningScreen::save(first, error));
    OpeningScreenReport again;
    again.config = cfg;
    assert(OpeningScreen::load(cfg.path, first.set_sha, again, error));
    assert(again.reused);
    assert(again.games_played == 0);
    assert(again.readings.size() == 2u);
    assert(again.kept == 1 && again.swept_before == 1 && again.swept_after == 0);
    assert(again.set_sha == first.set_sha);
    assert(again.lines().size() == 1u);

    // 换一代系数（白侧 `rush_four` 改回去）→ 键变了 → 旧集必须拒绝，而不是被当成筛过。
    OpeningScreenReport stale;
    stale.config = cfg;
    CoefficientPair moved;
    const std::string moved_sha = OpeningScreen::set_sha(moved, CoefficientPair(), cfg);
    assert(moved_sha != first.set_sha);
    assert(!OpeningScreen::load(cfg.path, moved_sha, stale, error));
    assert(error.find("was screened under") != std::string::npos);

    std::filesystem::remove(cfg.path);
    std::cout << "test_d6_two_candidates_cost_four_games_and_the_second_use_reads_the_cache passed"
              << std::endl;
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
    test_elo_scores_a_majority_match_as_a_win();
    test_coefficient_vector();
    test_self_play_records_legal_game();
    test_self_play_replays_prefix();
    test_self_play_stop_flag_is_invalid();
    test_retry_explorer_diverges();
    test_retry_explorer_forced_line();
    test_feature_extraction();
    test_data_collector_samples();
    test_augmented_samples_match_manual_transforms();
    test_hill_climbing();
    test_genetic();
    test_genetic_seed_covers_the_whole_box();
    test_hill_climbing_empty_steps();
    test_hill_climbing_effective_steps();
    test_genetic_elite_single_evaluation();
    test_random_opening_line();
    test_trainer_round_promotes();
    test_trainer_round_rejects_regression();
    test_trainer_keeps_rule_constants_through_promotion();
    test_trainer_genetic_round_and_status();
    test_trainer_rejects_bad_checkpoint();
    test_trainer_checkpoint_seed_and_counters();
    test_readers_reject_out_of_range_numbers();
    test_sample_from_json_rejects_damaged();
    test_sample_move_color_must_be_exact();
    test_sample_refuses_unreadable_identity_keys();
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
    test_params_refuses_a_five_that_flattens_the_scale();
    test_params_rejects_unrecognised_keys();
    test_params_rejects_out_of_range_tunables();
    test_params_rejects_cross_key_rewrites();
    test_trainer_cli_rejects_non_finite_accept_score();
    test_measure_fitness_flattens_rule_constants_for_candidates();
    test_measure_fitness_leaves_default_rule_constants_alone();
    test_collector_keeps_the_variant_that_moved_the_last_stone();
    test_collector_still_drops_genuine_symmetry_duplicates();
    test_trainer_refuses_an_empty_flag_value();
    test_trainer_help_states_the_probe_budget();
    test_server_refuses_no_sessions_with_a_stated_path();
    test_trainer_refuses_a_seed_that_a_resume_would_drop();
    test_trainer_refuses_one_file_for_both_out_and_checkpoint();
    test_genetic_stagnation_count_is_per_run();
    test_trainer_refuses_one_file_written_two_ways_for_out_and_checkpoint();
    test_trainer_accepts_a_resume_and_checkpoint_that_differ_only_by_a_dot_segment();
    test_data_game_ids_do_not_repeat_across_rounds();
    test_params_legacy_12_key_file_still_loads();
    test_params_position_and_width_genes_round_trip();
    test_params_refuses_only_stated_width_conflicts();
    test_self_play_node_budget_bounds_every_think();
    test_self_play_ms_budget_backs_up_a_generous_node_ceiling();
    test_self_play_zero_ceiling_arms_no_node_budget();
    test_self_play_declares_training_so_the_clock_does_not_shape_it();
    test_game_record_keeps_the_table_read_count();
    test_self_play_one_node_ceiling_still_answers_legally();
    test_trainer_config_node_budget_reaches_both_game_paths();
    test_trainer_cli_max_nodes_probes();

    // D2, in the contract's order: the one table every worker reaches, the batch head that
    // empties it, what --shared-tt 0 leaves alone, then the concurrent read.
    test_one_shared_table_serves_every_worker_of_a_batch();
    test_the_next_batch_clears_the_shared_table();
    test_sharing_off_leaves_every_engine_its_own_table();
    test_one_shared_table_survives_four_concurrent_writers();
    test_trainer_cli_shared_tt_probes();

    // D3, in the contract's order: the test's own arithmetic with no board in it,
    // the match that stops on it, the verdict that overrides the score bar, then
    // the flags at the CLI door.
    test_sprt_closes_only_at_a_pair_boundary();
    test_a_closed_sprt_ends_the_match_early();
    test_a_reject_verdict_keeps_a_candidate_the_bar_would_promote();
    test_trainer_cli_sprt_probes();

    // D4, in the contract's order: the set itself, the door a line goes through,
    // the rotation over a match's pairs, then the flag at the CLI.
    test_match_opening_set_is_fixed_and_diverse();
    test_match_pair_replays_its_opening_and_discards_an_illegal_one();
    test_a_match_rotates_through_the_opening_set();
    test_trainer_cli_match_opening_probes();

    // J 阶段段 A（设计文档 6.2）：摆幅算术、开局判据、摆幅判据、flat_game、
    // 坏记录、深度塌陷与将杀穿越。
    test_replay_swing_reads_from_the_mover_side();
    test_replay_screen_drops_the_opening_but_keeps_a_forced_reply();
    test_replay_screen_flags_a_big_swing();
    test_replay_screen_calls_a_flat_game_flat();
    test_replay_screen_refuses_an_invalid_record();
    test_replay_screen_sees_a_depth_dip_and_a_mate_crossing();
    test_replay_screen_keeps_tactical_plies_out_of_the_generic_judgments();
    test_tactics_card_round_trips_every_field();
    test_tactics_layer_boundaries_match_the_doc();
    test_coefficient_fingerprint_is_stable_and_sensitive();
    test_replay_reviewer_refuses_an_illegal_replay();
    test_replay_reviewer_scores_its_own_choice_as_lossless();
    test_replay_reviewer_prices_a_difficulty_ladder();
    test_training_sample_keeps_a_teacher_move();
    test_training_sample_without_a_teacher_keeps_the_old_shape();
    test_game_margin_answers_its_three_readings_by_value();
    test_the_margin_blend_is_identity_at_zero_weight();
    test_a_margin_weight_blends_the_real_match_score_off_the_lattice();
    test_teacher_fields_round_trip_and_stay_absent_when_off();
    test_self_play_records_a_teacher_answer_without_touching_the_student();
    test_a_taught_tactical_ply_becomes_a_sample();
    test_reset_returns_to_the_start_line_not_the_empty_board();
    test_d3_flat_file_reads_as_shared_and_a_split_file_adds_one_object();
    test_d3_gene_vector_is_side_major_and_keeps_its_width();
    test_d3_ladder_band_moves_white_and_the_reader_refuses();
    // 丁-6，按契约顺序：判据本身 → 键 → 关着时旧行为不变 → 成本与复用。
    test_d6_keeps_only_the_pairs_the_seat_does_not_decide();
    test_d6_the_screen_key_moves_with_either_player_and_with_the_regime();
    test_d6_a_closed_screen_leaves_the_fixed_rotation_byte_for_byte();
    test_d6_two_candidates_cost_four_games_and_the_second_use_reads_the_cache();

    std::cout << "All training tests passed" << std::endl;
    return 0;
}

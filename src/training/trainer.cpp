#include "trainer.h"
#include "json_int.h"
#include "self_play.h"
#include "../core/transforms.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <map>
#include <thread>
#include <algorithm>
#include <cmath>
#include <atomic>

using json = nlohmann::json;

namespace gomoku {

namespace {

std::string version_name(size_t index) {
    return "v" + std::to_string(index);
}

}

Trainer::Trainer(const ThreatCoefficients& base, const TrainingConfig& config)
    : config_(config), elo_(1500.0, 32.0) {
    versions_.push_back(base);
    elo_.add_player(version_name(0));
}

void Trainer::set_fitness_override(CoefficientFitness fitness) {
    fitness_override_ = std::move(fitness);
}

double Trainer::measure_fitness(const ThreatCoefficients& coefficients) {
    // Rule constants are not genes (README:607-608): from_vector default-constructs
    // on purpose because sanitize relies on those defaults, so a candidate arrives
    // carrying the compiled-in five and forbidden_penalty while the champion carries
    // the parameter file's. Flatten the baseline before comparing - the override
    // included, so a test hook measures the same pairing the games do.
    ThreatCoefficients scored = coefficients;
    scored.five = champion().five;
    scored.forbidden_penalty = champion().forbidden_penalty;
    if (fitness_override_) return fitness_override_(scored);
    return play_match(scored, champion(), config_.fitness_games);
}

SearchEngine& Trainer::prepare(SearchEngine& engine) const {
    // Training matches must measure the coefficients, not a stored opening: the
    // book would hand both sides the same memorised reply and hide the difference
    // being optimised.
    engine.set_book_enabled(false);
    engine.set_resign_detection(config_.resign_enabled);
    return engine;
}

double Trainer::play_game(SearchEngine& black, SearchEngine& white, int black_owner,
                          const std::vector<Move>& opening, bool* played) {
    SelfPlayConfig settings;
    settings.max_depth = config_.search_depth;
    settings.timeout_ms = config_.timeout_ms;
    settings.max_nodes = config_.max_nodes;
    settings.max_moves = config_.max_moves;

    SelfPlay play(black, white, settings);
    GameRecord record = play.run(opening, nullptr, nullptr);
    const bool valid = record.reason != GameEndReason::INVALID;
    if (played) *played = valid;
    // Same yardstick as the data side: an INVALID game is discarded, so it
    // must not inflate games_played_ in the checkpoint or the progress line.
    if (valid) games_played_++;
    if (!valid) return 0.0;

    if (record.winner == EMPTY) {
        // Unfinished games score half: they must not hand either side a result
        // the board does not show.
        return 0.5;
    }
    const int winner_owner = record.winner == BLACK ? black_owner : 3 - black_owner;
    return winner_owner == 1 ? 1.0 : 0.0;
}

double Trainer::play_match(const ThreatCoefficients& candidate,
                           const ThreatCoefficients& reference,
                           int games) {
    return score_match(candidate, reference, games).score;
}

MatchOutcome Trainer::score_match(const ThreatCoefficients& candidate,
                                  const ThreatCoefficients& reference,
                                  int games) {
    MatchOutcome outcome;
    if (fitness_override_) {
        const double candidate_score = fitness_override_(candidate);
        const double reference_score = fitness_override_(reference);
        if (candidate_score > reference_score + 1e-12) outcome.score = 1.0;
        else if (candidate_score < reference_score - 1e-12) outcome.score = 0.0;
        else outcome.score = 0.5;
        // An injected objective plays no games, so there is no record to test and
        // the --accept-score bar stays the only rule that can promote.
        return outcome;
    }

    // Fresh engines per evaluation, not a reused pair: a transposition table entry
    // records a score computed under whatever coefficients were active when it was
    // written, so carrying one across a candidate change would score the new
    // parameters with the old ones.
    SearchEngine candidate_engine(candidate);
    SearchEngine reference_engine(reference);
    prepare(candidate_engine);
    prepare(reference_engine);

    SprtConfig settings;
    settings.effect_elo = config_.sprt_elo;
    SprtTest test(settings);

    // Every pairing runs twice with the colours swapped, so a candidate cannot
    // pass on the strength of having the first move. Odd requests round UP to
    // the next even count, as the config comment promises; and since design doc
    // 2.8 #3 both counts are ceilings, because the test below can end the match
    // at a pair boundary before they are spent.
    const int pairs = std::max(1, (games + 1) / 2);
    double score = 0.0;
    int played_games = 0;
    for (int i = 0; i < pairs; ++i) {
        // One position per pair, replayed identically for both colours: the pair
        // then measures the same opening once from each side, so what that opening
        // gives the first mover cancels inside the pair instead of deciding it.
        // Rotating the position by round and pair index is what stops every match
        // of a run from asking the same question about the same two stones
        // (design doc 2.8 #4).
        const std::vector<Move> opening =
            match_opening_line(rounds_completed_ + i, config_.match_opening_plies);
        bool valid = false;
        const double first =
            play_game(candidate_engine, reference_engine, 1, opening, &valid);
        if (valid) {
            score += first;
            ++played_games;
            test.add_game(first);
        }
        valid = false;
        const double second =
            play_game(reference_engine, candidate_engine, 2, opening, &valid);
        if (valid) {
            score += second;
            ++played_games;
            test.add_game(second);
        }
        // Polled between pairs, never between the two games of one pair: one
        // game measures who moved first as much as who plays better, and a test
        // allowed to stop on it would promote the colour advantage README's
        // 颜色通吃 record is about - the reason the bar sits above half at all.
        test.end_pair();
        if (test.stop_requested()) break;
    }
    outcome.games = played_games;
    outcome.llr = test.llr();
    outcome.verdict = test.verdict();
    // A discarded game has no result to average, so it leaves the denominator
    // rather than counting as a half; one that never got played cannot promote.
    outcome.score = played_games == 0 ? 0.0 : score / played_games;
    return outcome;
}

std::vector<Move> Trainer::random_opening_line(int plies, uint64_t seed) {
    std::vector<Move> line;
    if (plies <= 0) return line;

    // An empty board has no active candidates, so the opening stone is fixed and
    // the randomness starts from the reply.
    const Move opening(BOARD_SIZE / 2, BOARD_SIZE / 2, BLACK);
    line.push_back(opening);
    if (plies == 1) return line;

    Board board;
    board.place_stone(opening.pos, BLACK);

    std::mt19937_64 rng(seed);
    while (static_cast<int>(line.size()) < plies && board.move_count() < MAX_MOVES) {
        const int color = board.move_count() % 2 == 0 ? BLACK : WHITE;
        const std::vector<Move> candidates = legal_moves(board, color);
        if (candidates.empty()) break;

        // One representative per symmetry class: the branches that differ only by
        // rotation would otherwise absorb the whole random budget.
        std::map<int, Move> classes;
        for (const auto& candidate : candidates) {
            classes.emplace(canonical_cell(board, candidate.pos), candidate);
        }

        std::vector<Move> representatives;
        representatives.reserve(classes.size());
        for (const auto& entry : classes) representatives.push_back(entry.second);

        std::uniform_int_distribution<size_t> pick(0, representatives.size() - 1);
        const Move chosen = representatives[pick(rng)];
        board.place_stone(chosen.pos, color);
        line.push_back(chosen);
    }

    return line;
}

std::vector<Move> Trainer::match_opening_line(int index, int plies) {
    if (plies <= 0) return {};
    // Fixed by index, not by --seed: two candidates measured in the same round have
    // to be handed the same positions, and a set that moved with the run's seed
    // would make the pairing depend on a number the optimiser does not care about.
    // Wrapping is what makes this a set at all - past kMatchOpeningSet the rotation
    // reuses the same lines instead of inventing an unbounded number of them.
    const int wrapped = ((index % kMatchOpeningSet) + kMatchOpeningSet) % kMatchOpeningSet;
    return random_opening_line(plies, static_cast<uint64_t>(wrapped) + 1);
}

int Trainer::generate_data_games(int games, int threads) {
    if (games <= 0 || fitness_override_) return 0;
    // Nothing to write means nothing to play for: without either output path
    // the games would only burn the round's budget and be dropped at merge.
    if (config_.records_path.empty() && config_.samples_path.empty()) {
        if (log_) log_("skipping data games this round: no --records or --samples "
                       "output path was given");
        return 0;
    }

    const int worker_count = std::max(1, std::min(threads, games));
    const int per_worker = games / worker_count;
    const int remainder = games % worker_count;

    struct Local {
        std::vector<GameRecord> records;
        std::vector<TrainingSample> samples;
    };
    std::vector<Local> locals(worker_count);
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    // One id per game that actually yields samples, claimed from this counter
    // instead of computed from the worker number: the claim count equals the merged
    // record count, so the games_played_ advance after join and the ids already
    // handed out stay exactly aligned. A fixed 1000 stride makes two workers overlap
    // once a worker's count passes 1000, and its offset relative to games_played_ is
    // legitimately re-reached in later rounds; both show up as a repeated
    // (game_id, move_index) in samples.jsonl - the identity of the training data,
    // which README:181's append stream has to keep separable.
    std::atomic<int> next_data_id{games_played_ + 1};
    DataCollector collector;
    const ThreatCoefficients current_champion = champion();

    // Design doc 2.8 #1. Built here rather than in the constructor so a --no-data run
    // never pays for a table it would not touch, and filled before any worker exists so
    // the shared flag is never written once threads are running.
    if (config_.shared_tt_entries > 0 && !shared_tt_) {
        shared_tt_ = std::make_unique<TranspositionTable>(
            static_cast<size_t>(config_.shared_tt_entries), true);
    }
    // The 失效口径 the round before this one asked for: a record is a score computed
    // under the coefficients that were active when it was written, and the champion may
    // have been replaced since the last batch. Emptying the table at the head of each
    // batch is the one rule that needs no stamp on the entry and no comparison of
    // coefficient sets - and it is cheap, because a batch is a round and the table is
    // 7.3MB.
    if (shared_tt_) shared_tt_->clear();

    for (int t = 0; t < worker_count; ++t) {
        // t is captured by value: the loop advances while the workers are already
        // running, so a reference capture would hand several workers the same
        // slot and the same engine settings.
        workers.emplace_back([&, t]() {
            Local& local = locals[t];
            const uint64_t seed = config_.seed ^
                                  (0x9E3779B97F4A7C15ull *
                                   (static_cast<uint64_t>(rounds_completed_ + 1) * 131 + t + 1));

            // Every worker of this batch prices positions with current_champion, so one
            // table between them is the same information each of them would have paid
            // 7.3MB to hold on its own. It is named on the constructor rather than set
            // afterwards, because a worker that allocated its own table first would
            // still have allocated it - see SearchEngine's note on the peak. The
            // fitness side (play_match) keeps per-engine tables on purpose: there the
            // two sides hold two different coefficient sets at once, and sharing that
            // would score a candidate with the reference's numbers.
            SearchEngine engine(current_champion, shared_tt_.get());
            prepare(engine);

            SelfPlayConfig settings;
            settings.max_depth = config_.search_depth;
            settings.timeout_ms = config_.timeout_ms;
            settings.max_nodes = config_.max_nodes;
            settings.max_moves = config_.max_moves;

            RetryExplorer explorer(engine, engine, settings);
            explorer.set_start_line(random_opening_line(config_.data_opening_plies, seed));

            const int count = per_worker + (t < remainder ? 1 : 0);
            // Data-game ids use the games_played_ high-water mark (persisted with
            // the checkpoint) rather than rounds_completed_: they then number the
            // played games densely, and INVALID games - skipped below - never
            // consume one. It is not crash protection: the count advances here,
            // but the checkpoint is only written at the end of the round, so a
            // kill between the data append and the checkpoint still reissues this
            // round's ids on --resume. Real protection needs an export marker and
            // is not built yet. Workers only read games_played_; each id is claimed
            // from next_data_id, so several threads claiming at once is atomic and
            // not a data race.
            for (int i = 0; i < count; ++i) {
                GameRecord record;
                if (!explorer.explore(record, nullptr)) {
                    explorer.reset();
                    if (!explorer.explore(record, nullptr)) break;
                }

                // INVALID games are junk: keep them out of local.records too, so
                // neither games_played_ nor the JSONL stream counts or writes a
                // discarded game.
                if (record.reason == GameEndReason::INVALID) continue;
                local.records.push_back(record);

                std::vector<TrainingSample> samples = collector.collect(
                    record, next_data_id.fetch_add(1), true);
                for (auto& sample : samples) local.samples.push_back(std::move(sample));
            }
        });
    }

    for (auto& worker : workers) worker.join();

    std::vector<GameRecord> records;
    std::vector<TrainingSample> samples;
    for (auto& local : locals) {
        for (auto& record : local.records) records.push_back(std::move(record));
        for (auto& sample : local.samples) samples.push_back(std::move(sample));
    }
    // Counted from the merged records instead of inside the workers: a shared
    // non-atomic counter touched by several threads at once is a data race.
    games_played_ += static_cast<int>(records.size());
    samples_collected_ += samples.size();

    std::string error;
    if (!config_.records_path.empty() && !records.empty()) {
        for (const auto& record : records) {
            // Write failures go to stderr regardless of --quiet: silence is
            // for progress lines, not for lost data.
            if (!record.append_to_jsonl(config_.records_path, error)) {
                std::cerr << "could not append record: " << error << std::endl;
                break;
            }
        }
    }
    if (!config_.samples_path.empty() && !samples.empty()) {
        if (!collector.write_jsonl(samples, config_.samples_path, error)) {
            std::cerr << "could not append samples: " << error << std::endl;
        }
    }

    return static_cast<int>(samples.size());
}

TrainerRound Trainer::run_round() {
    TrainerRound result;
    result.round = rounds_completed_ + 1;
    result.champion = champion();
    result.champion_fitness = fitness_override_ ? fitness_override_(champion()) : 0.5;

    CoefficientFitness fitness = [this](const ThreatCoefficients& coefficients) {
        return measure_fitness(coefficients);
    };

    const uint64_t round_seed = config_.seed + static_cast<uint64_t>(rounds_completed_) * 7919;
    ThreatCoefficients candidate;

    if (config_.optimizer == OptimizerKind::GENETIC) {
        GeneticConfig settings;
        settings.population_size = config_.population_size;
        settings.max_generations = config_.generations;
        settings.stagnation_generations = config_.stagnation_generations;

        GeneticOptimizer optimizer(fitness, settings, round_seed);
        int generations_run = 0;
        double best_fitness = 0.0;
        candidate = optimizer.run(result.champion, generations_run, best_fitness);
        result.candidate_fitness = best_fitness;
        if (log_) {
            log_("round " + std::to_string(result.round) + ": genetic ran " +
                 std::to_string(generations_run) + " generations, " +
                 std::to_string(optimizer.evaluations()) + " evaluations, best fitness " +
                 std::to_string(best_fitness));
        }
    } else {
        HillClimbingConfig settings;
        settings.max_iterations = config_.hill_climbing_iterations;
        settings.stagnation_limit = config_.stagnation_generations * 3;

        HillClimbingOptimizer optimizer(fitness, settings, round_seed);
        int iterations_run = 0;
        double best_fitness = 0.0;
        candidate = optimizer.run(result.champion, iterations_run, best_fitness);
        result.candidate_fitness = best_fitness;
        if (log_) {
            log_("round " + std::to_string(result.round) + ": hill climbing ran " +
                 std::to_string(iterations_run) + " iterations, " +
                 std::to_string(optimizer.evaluations()) + " evaluations, best fitness " +
                 std::to_string(best_fitness));
        }
    }
    // Rule constants are not genes: the optimisers rebuild candidates from a
    // default-constructed struct, so carry the champion's values over or the
    // verification match compares two different rule sets and a promotion would
    // rewrite the parameter file's constants with defaults.
    candidate.five = result.champion.five;
    candidate.forbidden_penalty = result.champion.forbidden_penalty;
    result.candidate = candidate;

    const MatchOutcome check =
        score_match(candidate, result.champion, config_.verification_games);
    result.verification_score = check.score;
    result.verification_verdict = check.verdict;
    result.verification_llr = check.llr;
    result.verification_match_games = check.games;
    // The test's answer is the promotion rule whenever it has one: an ACCEPT is
    // the same evidence the score bar was going to ask for, reached in fewer
    // games, and a REJECT says the record is worse than the fraction it averaged
    // to. Only an open match - the ceiling spent with the ratio still inside both
    // bounds - falls back to --accept-score, which is the bar README's colour
    // advantage record is measured against.
    result.accepted = check.verdict == SprtVerdict::ACCEPT ||
                      (check.verdict == SprtVerdict::OPEN &&
                       check.score >= config_.accept_score);

    const size_t candidate_index = versions_.size();
    const int candidate_id = elo_.add_player(version_name(candidate_index));
    elo_.record_result(candidate_id, static_cast<int>(best_version_), result.verification_score);
    versions_.push_back(candidate);

    if (result.accepted) {
        best_version_ = candidate_index;
        if (!config_.params_path.empty()) {
            std::string error;
            if (!CoefficientVector::save(config_.params_path, champion(), error)) {
                std::cerr << "could not save parameters: " << error << std::endl;
            }
        }
    }

    result.champion_elo = elo_.player(static_cast<int>(best_version_)).elo;
    result.candidate_elo = elo_.player(candidate_id).elo;
    result.games_played = games_played_;
    result.samples_collected = static_cast<int>(samples_collected_);

    if (config_.collect_data) {
        const int samples = generate_data_games(config_.data_games, config_.data_threads);
        result.samples_collected = static_cast<int>(samples_collected_);
        if (log_) log_("round " + std::to_string(result.round) + ": collected " +
                       std::to_string(samples) + " samples");
    }

    rounds_completed_++;
    if (log_) {
        log_("round " + std::to_string(result.round) + ": verification score " +
             std::to_string(result.verification_score) + " (sprt " +
             sprt_verdict_name(result.verification_verdict) + ", llr " +
             std::to_string(result.verification_llr) + ", " +
             std::to_string(result.verification_match_games) + " games) -> " +
             (result.accepted ? "promoted" : "kept champion") + ", champion elo " +
             std::to_string(result.champion_elo));
    }

    if (!config_.checkpoint_path.empty()) {
        std::string error;
        if (save_checkpoint(config_.checkpoint_path, error)) {
            if (log_) log_("round " + std::to_string(result.round) +
                           ": checkpoint saved to " + config_.checkpoint_path);
        } else {
            std::cerr << "could not save checkpoint: " << error << std::endl;
        }
    }

    return result;
}

std::vector<TrainerRound> Trainer::run(int rounds) {
    std::vector<TrainerRound> history;
    history.reserve(rounds);
    for (int i = 0; i < rounds; ++i) history.push_back(run_round());
    return history;
}

std::string Trainer::status_text() const {
    std::ostringstream out;
    out << "rounds=" << rounds_completed_
        << " versions=" << versions_.size()
        << " champion=" << version_name(best_version_)
        << " games=" << games_played_
        << " samples=" << samples_collected_;
    return out.str();
}

bool Trainer::save_checkpoint(const std::string& path, std::string& error) const {
    json root;
    root["rounds_completed"] = rounds_completed_;
    root["games_played"] = games_played_;
    root["samples_collected"] = static_cast<long long>(samples_collected_);
    root["best_version"] = static_cast<long long>(best_version_);
    root["seed"] = config_.seed;

    json entries = json::array();
    for (const auto& coefficients : versions_) {
        entries.push_back(json::parse(CoefficientVector::to_json_string(coefficients)));
    }
    root["versions"] = entries;
    root["elo"] = json::parse(elo_.to_json());

    // Write-then-rename, mirroring save_session: a kill or power loss mid-write
    // must leave the previous checkpoint intact, not a truncated file that
    // load_checkpoint cannot parse.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file.is_open()) {
            error = "cannot write " + tmp;
            return false;
        }
        file << root.dump(2);
        // The rename below is only safe once the bytes are really on the device:
        // a buffered failure would surface at close, after the good() check and
        // after the truncated .tmp had replaced a working checkpoint.
        file.flush();
        if (!file.good()) {
            error = "failed while writing " + tmp;
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        error = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

bool Trainer::load_checkpoint(const std::string& path, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();

    json root;
    try {
        root = json::parse(buffer.str());
    } catch (const json::exception& e) {
        error = std::string("checkpoint is not valid JSON: ") + e.what();
        return false;
    }
    if (!root.is_object() || !root.contains("versions") || !root["versions"].is_array() ||
        root["versions"].empty()) {
        error = "checkpoint has no parameter versions";
        return false;
    }

    std::vector<ThreatCoefficients> restored_versions;
    for (const auto& entry : root["versions"]) {
        ThreatCoefficients coefficients;
        if (!CoefficientVector::from_json_string(entry.dump(), coefficients, error)) {
            error = "checkpoint version is malformed: " + error;
            return false;
        }
        restored_versions.push_back(coefficients);
    }

    // Every counter is range-checked: get<int> only casts, so a checkpoint holding
    // a value wider than the field would resume the run on a truncated total.
    int best = 0;
    if (root.contains("best_version") && !read_json_int(root["best_version"], best)) {
        error = "checkpoint champion index is out of range";
        return false;
    }
    if (best < 0 || best >= static_cast<int>(restored_versions.size())) {
        error = "checkpoint champion index is out of range";
        return false;
    }

    EloSystem restored_elo;
    if (root.contains("elo")) {
        if (!restored_elo.load_json(root["elo"].dump(), error)) {
            error = "checkpoint ratings are malformed: " + error;
            return false;
        }
    } else {
        for (size_t i = 0; i < restored_versions.size(); ++i) restored_elo.add_player(version_name(i));
    }
    // Player ids are used as version indices, so the two lists must agree by name
    // and not merely by length: equal lengths with the names shifted would
    // attribute results to another version, and every later checkpoint would then
    // be unreadable.
    if (restored_elo.players().size() != restored_versions.size()) {
        error = "checkpoint ratings and versions disagree";
        return false;
    }
    for (size_t i = 0; i < restored_versions.size(); ++i) {
        if (restored_elo.players()[i].name != version_name(i)) {
            error = "checkpoint ratings and versions disagree";
            return false;
        }
    }

    // Everything is read into locals and only assigned once the whole file
    // validates, so a checkpoint rejected halfway cannot leave this trainer
    // running on another run's versions and ratings.
    int restored_rounds = 0;
    int restored_games = 0;
    uint64_t restored_samples = 0;
    uint64_t restored_seed = config_.seed;
    auto read_int = [&root](const char* key, int& out) {
        return !root.contains(key) || read_json_int(root[key], out);
    };
    if (!read_int("rounds_completed", restored_rounds) ||
        !read_int("games_played", restored_games)) {
        error = "checkpoint counters are out of range";
        return false;
    }
    if (restored_rounds < 0 || restored_games < 0) {
        error = "checkpoint counters must not be negative";
        return false;
    }
    if (root.contains("samples_collected") &&
        !read_json_uint(root["samples_collected"], restored_samples)) {
        error = "checkpoint sample total is out of range";
        return false;
    }
    if (root.contains("seed") && !read_json_uint(root["seed"], restored_seed)) {
        error = "checkpoint seed is out of range";
        return false;
    }

    versions_ = restored_versions;
    elo_ = restored_elo;
    best_version_ = static_cast<size_t>(best);
    rounds_completed_ = restored_rounds;
    games_played_ = restored_games;
    samples_collected_ = static_cast<size_t>(restored_samples);
    config_.seed = restored_seed;
    return true;
}

}

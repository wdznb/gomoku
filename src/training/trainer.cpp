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
    : Trainer(CoefficientPair::shared(base), config) {}

Trainer::Trainer(const CoefficientPair& base, const TrainingConfig& config)
    : config_(config), elo_(1500.0, 32.0) {
    versions_.push_back(base);
    elo_.add_player(version_name(0));
}

void Trainer::set_fitness_override(CoefficientFitness fitness) {
    fitness_override_ = std::move(fitness);
}

double Trainer::measure_fitness(const CoefficientPair& coefficients) {
    // Rule constants are not genes (README:607-608): from_vector default-constructs
    // on purpose because sanitize relies on those defaults, so a candidate arrives
    // carrying the compiled-in five and forbidden_penalty while the champion carries
    // the parameter file's. Flatten the baseline before comparing - the override
    // included, so a test hook measures the same pairing the games do. Each side
    // keeps its own constants; the evaluation cap is the smaller of the two fives,
    // so a pair whose sides disagree still has one colour-independent ceiling.
    CoefficientPair scored = coefficients;
    scored.black.five = champion().black.five;
    scored.black.forbidden_penalty = champion().black.forbidden_penalty;
    scored.white.five = champion().white.five;
    scored.white.forbidden_penalty = champion().white.forbidden_penalty;
    if (fitness_override_) return fitness_override_(scored);
    return play_match(scored, champion(), config_.fitness_games,
                      config_.fitness_margin_weight);
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
                          const std::vector<Move>& opening, bool* played,
                          double margin_weight) {
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
    const double binary = winner_owner == 1 ? 1.0 : 0.0;
    if (margin_weight <= 0.0) {
        // 默认形状：胜负对原样返回。这一句早退是「w = 0 与今天逐字节一致」那条
        // 要求（指导书 §4 丁-1 实现思路 3）的执行处，不是省几行浮点。
        return binary;
    }
    const double margin = game_margin(record, settings.max_moves);
    return blend_fitness(binary, margin, margin_weight);
}

double Trainer::blend_fitness(double binary, double margin, double weight) {
    if (weight <= 0.0) return binary;
    if (weight >= 1.0) return margin;
    return (1.0 - weight) * binary + weight * margin;
}

namespace {

double clamp_unit(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

}  // namespace

// 丁-1 的连续分量。三个读数都从**已有产物**取，不新增基础设施：
//   1. 终局分差——赢方最后一手与输方最后一手各自报出的搜索分；
//   2. 先成五一方用了多少手——手数越短越高；
//   3. 认输时两侧的搜索深度差——只有 `RESIGN` 才有这一项，其余给中性 0.5。
// 尺度是写死的常数，不是可拧的旋钮：可调的只有外面那个权重 w（指导书要求「必须可关」，
// 旋钮越多，「同 seed 不许翻面」那条判据要量的面就越大）。
double Trainer::game_margin(const GameRecord& record, int max_moves) {
    constexpr double kScoreScale = 20000.0;  // 两手报告的搜索分差拉满所需的量级
    constexpr double kDepthScale = 8.0;      // 深度差 8 层算拉满

    if (record.reason == GameEndReason::INVALID || record.winner == EMPTY) return 0.5;

    // 终局分差用**记录里已经写下的搜索分**，而不是把终局盘再评一遍：
    // `Evaluator::evaluate_board` 扫的是空格的「还能长出什么」（`active_empty()` 的循环），
    // 盘面上已经摆成的五连不在它的评价里——本轮实测过，一个黑棋成五的终局它给 -10，
    // 所以那个读数会系统性地把「赢定了」评成「没有优势」。而一手棋的 `score` 正是
    // 当时那一方自己报的分，成五的手在将杀带里。重放与开局库的手不含决策（score 是 0
    // 或上一手的残值），与 §6.4 筛题卡时同一条纪律，跳过。
    int winner_score = 0;
    int loser_score = 0;
    bool winner_seen = false;
    bool loser_seen = false;
    for (const MoveRecord& entry : record.moves) {
        if (entry.method == "replay" || entry.method == "book") continue;
        if (entry.move.color == record.winner) {
            winner_score = entry.score;
            winner_seen = true;
        } else {
            loser_score = entry.score;
            loser_seen = true;
        }
    }
    if (!winner_seen || !loser_seen) return 0.5;
    const double gap = static_cast<double>(winner_score) - static_cast<double>(loser_score);
    const double terminal = clamp_unit(0.5 + gap / kScoreScale);

    // 用了多少手：同样的赢，赢得快就是更高。
    const double plies = static_cast<double>(record.moves.size());
    const double speed =
        clamp_unit(1.0 - plies / static_cast<double>(std::max(1, max_moves)));

    // 深度差只有认输那一局才有意义：认输是「我算清了这盘救不回来」，
    // 所以两侧最后各搜了多深值得进信号。战术手（vcf / must_block）报 depth = 0 是
    // 结构性的不是「没想」，取最后一个非零深度；取不到就中性。
    double depth_term = 0.5;
    if (record.reason == GameEndReason::RESIGN) {
        int winner_depth = 0;
        int loser_depth = 0;
        for (const MoveRecord& entry : record.moves) {
            if (entry.depth <= 0) continue;
            if (entry.move.color == record.winner) winner_depth = entry.depth;
            else loser_depth = entry.depth;
        }
        if (winner_depth > 0 || loser_depth > 0) {
            depth_term = clamp_unit(0.5 + (winner_depth - loser_depth) / kDepthScale);
        }
    }
    return 0.5 * terminal + 0.3 * speed + 0.2 * depth_term;
}

double Trainer::play_match(const CoefficientPair& candidate,
                           const CoefficientPair& reference,
                           int games, double margin_weight) {
    return score_match(candidate, reference, games, margin_weight).score;
}

MatchOutcome Trainer::score_match(const CoefficientPair& candidate,
                                  const CoefficientPair& reference,
                                  int games, double margin_weight) {
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
        const std::vector<Move> opening = match_opening_for(rounds_completed_ + i);
        bool valid = false;
        const double first =
            play_game(candidate_engine, reference_engine, 1, opening, &valid, margin_weight);
        if (valid) {
            score += first;
            ++played_games;
            test.add_game(first);
        }
        valid = false;
        const double second =
            play_game(reference_engine, candidate_engine, 2, opening, &valid, margin_weight);
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
    const CoefficientPair current_champion = champion();

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

    // 丁-2：老师的那张表。同样是「开在任何 worker 之前、每轮批首清空」，
    // 也同样是 `shared = true`——几个 worker 的老师读的是同一份系数下的条目，
    // 共享它省的是 7.3 MB × (--threads)，与注意 1 要防的「学生的表被老师写深了」是两件事。
    if (config_.teacher_depth > 0 && config_.shared_tt_entries > 0 && !teacher_tt_) {
        teacher_tt_ = std::make_unique<TranspositionTable>(
            static_cast<size_t>(config_.shared_tt_entries), true);
    }
    if (teacher_tt_) teacher_tt_->clear();

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
            settings.teacher_depth = config_.teacher_depth;
            settings.teacher_max_nodes = config_.teacher_max_nodes;

            // 丁-2：每一个 worker 一个老师（`SearchEngine` 不是线程安全的），
            // 系数与这一批的学生相同，表是 `teacher_tt_` 而不是 `shared_tt_`。
            // 认输判定关掉：认输回来是不带 best_move 的，那样这一手就没有答案可记。
            // 开局库同样关掉，与 `prepare` 给学生的理由一样——蒸馏要的是这套系数
            // 在这个局面下搜出来的答案，不是背下来的那一步。
            std::unique_ptr<SearchEngine> teacher;
            if (config_.teacher_depth > 0) {
                teacher = std::make_unique<SearchEngine>(current_champion, teacher_tt_.get());
                teacher->set_book_enabled(false);
                teacher->set_resign_detection(false);
                teacher->set_training(true);
            }

            RetryExplorer explorer(engine, engine, settings);
            if (teacher) explorer.set_teacher(teacher.get());
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

    CoefficientFitness fitness = [this](const CoefficientPair& coefficients) {
        return measure_fitness(coefficients);
    };

    const uint64_t round_seed = config_.seed + static_cast<uint64_t>(rounds_completed_) * 7919;
    CoefficientPair candidate;

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
    // rewrite the parameter file's constants with defaults. Each side carries its
    // own - `five` is what that colour's threats are capped against, and the
    // evaluation cap is the smaller of the two, so this stays colour-independent.
    candidate.black.five = result.champion.black.five;
    candidate.black.forbidden_penalty = result.champion.black.forbidden_penalty;
    candidate.white.five = result.champion.white.five;
    candidate.white.forbidden_penalty = result.champion.white.forbidden_penalty;
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

    std::vector<CoefficientPair> restored_versions;
    for (const auto& entry : root["versions"]) {
        CoefficientPair coefficients;
        if (!CoefficientVector::pair_from_json_string(entry.dump(), coefficients, error)) {
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

// `tactics_card.h:66` 的那个符号在这里重述一次声明，而不是往文件顶部加 `#include`：
// 加一行 include 会把本文件被按行号指认的那几处（`measure_fitness` 41-57、`score_match`
// 180-257、checkpoint 的 `versions[]` 626-629）整体推走。
std::string coefficients_fingerprint(const ThreatCoefficients& coeffs);

namespace {

// 开局棋谱的 JSON 形状与题卡一致（`[[r,c,"black"], …]`）。没有共用 `tactics_card.cpp`
// 里那对同名助手，是因为要把它们导出来就得在 `tactics_card.h` 里加声明，而那行的下面
// 压着被按行号指认的 `coefficients_fingerprint`。
const char* opening_color_name(int color) {
    return color == BLACK ? "black" : (color == WHITE ? "white" : "none");
}

int opening_color_from_name(const std::string& name) {
    if (name == "b" || name == "black") return BLACK;
    if (name == "w" || name == "white") return WHITE;
    return EMPTY;
}

// 一局：`candidate_is_black` 决定候选坐哪一侧，两侧各持自己那一套系数。返回值以**座位**计。
int play_opening_screen_game(const std::vector<Move>& line, bool candidate_is_black,
                             const CoefficientPair& candidate, const CoefficientPair& reference,
                             const SelfPlayConfig& settings, int& plies) {
    SearchEngine black(candidate_is_black ? candidate : reference);
    SearchEngine white(candidate_is_black ? reference : candidate);
    black.set_book_enabled(false);
    white.set_book_enabled(false);
    black.set_resign_detection(true);
    white.set_resign_detection(true);
    SelfPlay play(black, white, settings);
    std::atomic<bool> stop(false);
    const GameRecord rec = play.run(line, nullptr, &stop);
    plies += static_cast<int>(rec.moves.size());
    if (rec.reason == GameEndReason::INVALID) return 2;
    if (rec.winner == BLACK) return 1;
    if (rec.winner == WHITE) return -1;
    return 0;
}

// 三个计数都从读数重算：跑出来的与读回来的走同一条口径。
void recount_openings(OpeningScreenReport& report) {
    report.swept_before = 0;
    report.swept_after = 0;
    report.kept = 0;
    for (const OpeningReading& reading : report.readings) {
        if (reading.valid && !reading.discriminates()) report.swept_before++;
        if (reading.kept) {
            report.kept++;
            if (reading.seat_decided) report.swept_after++;
        }
    }
}

}  // namespace

std::string OpeningScreen::set_sha(const CoefficientPair& candidate,
                                   const CoefficientPair& reference,
                                   const OpeningScreenConfig& config) {
    return coefficients_fingerprint(candidate.black) + "-" +
           coefficients_fingerprint(candidate.white) + "-" +
           coefficients_fingerprint(reference.black) + "-" +
           coefficients_fingerprint(reference.white) +
           "-s" + std::to_string(config.stones) + "d" + std::to_string(config.depth) + "n" +
           std::to_string(config.max_nodes) + "c" + std::to_string(config.candidates) + "k" +
           std::to_string(config.seed_stride);
}

bool OpeningScreen::load(const std::string& path, const std::string& sha, OpeningScreenReport& out,
                         std::string& error) {
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
        error = std::string("opening set is not valid JSON: ") + e.what();
        return false;
    }
    if (!root.is_object() || !root.contains("set_sha") || !root["set_sha"].is_string()) {
        error = "opening set has no set_sha";
        return false;
    }
    const std::string stored = root["set_sha"].get<std::string>();
    if (stored != sha) {
        // 换系数或换档：旧筛选留下的是**另一对玩家**下有胜机的开局，留着比没有更糟。
        error = "opening set was screened under " + stored + ", this pair needs " + sha;
        return false;
    }
    if (!root.contains("readings") || !root["readings"].is_array()) {
        error = "opening set has no readings";
        return false;
    }

    OpeningScreenReport report;
    report.config = out.config;   // 档位由调用方给，它也是 sha 的来源
    report.set_sha = stored;
    for (const json& item : root["readings"]) {
        if (!item.is_object()) {
            error = "opening reading is not an object";
            return false;
        }
        OpeningReading reading;
        if (!item.contains("line") || !item["line"].is_array()) {
            error = "opening reading has no line";
            return false;
        }
        for (const json& stone : item["line"]) {
            if (!stone.is_array() || stone.size() < 3 || !stone[2].is_string()) {
                error = "opening reading has an unreadable stone";
                return false;
            }
            reading.line.push_back(Move(stone[0].get<int>(), stone[1].get<int>(),
                                        opening_color_from_name(stone[2].get<std::string>())));
        }
        for (const char* key : {"as_candidate_black", "as_candidate_white", "plies", "games"}) {
            if (!item.contains(key) || !item[key].is_number_integer()) {
                error = std::string("opening reading is missing ") + key;
                return false;
            }
        }
        reading.as_candidate_black = item["as_candidate_black"].get<int>();
        reading.as_candidate_white = item["as_candidate_white"].get<int>();
        reading.plies = item["plies"].get<int>();
        reading.games = item["games"].get<int>();
        reading.valid = reading.games == 2 && reading.as_candidate_black != 2 &&
                        reading.as_candidate_white != 2 && reading.as_candidate_black != 0 &&
                        reading.as_candidate_white != 0;
        reading.seat_decided = !reading.discriminates();
        reading.kept = reading.discriminates();
        report.readings.push_back(reading);
    }
    recount_openings(report);
    report.reused = true;
    out = report;
    return true;
}

bool OpeningScreen::save(const OpeningScreenReport& report, std::string& error) {
    json root;
    root["schema"] = "openings/1";
    root["set_sha"] = report.set_sha;
    root["games_played"] = report.games_played;
    root["swept_before"] = report.swept_before;
    root["swept_after"] = report.swept_after;
    root["kept"] = report.kept;
    root["candidate_count"] = static_cast<int>(report.readings.size());
    root["stones"] = report.config.stones;
    root["depth"] = report.config.depth;
    root["max_nodes"] = report.config.max_nodes;

    json readings = json::array();
    for (const OpeningReading& reading : report.readings) {
        json stones = json::array();
        for (const Move& stone : reading.line) {
            stones.push_back(
                json::array({stone.pos.row, stone.pos.col, opening_color_name(stone.color)}));
        }
        json item;
        item["line"] = std::move(stones);
        item["as_candidate_black"] = reading.as_candidate_black;
        item["as_candidate_white"] = reading.as_candidate_white;
        item["plies"] = reading.plies;
        item["games"] = reading.games;
        item["kept"] = reading.kept;
        readings.push_back(std::move(item));
    }
    root["readings"] = std::move(readings);

    // 与参数文件、checkpoint 同一个写法：先写临时文件再改名，半写的一份不会顶掉能用的那份。
    const std::string tmp = report.config.path + ".tmp";
    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file.is_open()) {
            error = "cannot write " + tmp;
            return false;
        }
        file << root.dump(2);
        file.flush();
        if (!file.good()) {
            error = "failed while writing " + tmp;
            std::error_code remove_ec;
            std::filesystem::remove(tmp, remove_ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, report.config.path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
        error = "cannot replace " + report.config.path + ": " + ec.message();
        return false;
    }
    return true;
}

OpeningScreenReport OpeningScreen::run(const CoefficientPair& candidate,
                                       const CoefficientPair& reference,
                                       const OpeningScreenConfig& config, std::string& error) {
    OpeningScreenReport report;
    report.config = config;
    report.set_sha = set_sha(candidate, reference, config);
    if (config.candidates <= 0) return report;

    SelfPlayConfig settings;
    settings.max_depth = config.depth;
    // 时限只防死循环：判定用节点帽，这样一条开局在空机与满载机上给出同一个答案（§9.2）。
    settings.timeout_ms = 60000;
    settings.max_nodes = config.max_nodes;
    settings.max_moves = config.max_moves;

    for (int i = 0; i < config.candidates; ++i) {
        OpeningReading reading;
        reading.line = Trainer::random_opening_line(config.stones,
                                                    static_cast<uint64_t>(config.seed_stride) + i);
        if (static_cast<int>(reading.line.size()) < config.stones) continue;
        Board prefix;
        bool placeable = true;
        for (const Move& stone : reading.line) {
            if (!prefix.place_stone(stone.pos.row, stone.pos.col, stone.color)) placeable = false;
        }
        if (!placeable) continue;

        reading.as_candidate_black =
            play_opening_screen_game(reading.line, true, candidate, reference, settings,
                                     reading.plies);
        reading.as_candidate_white =
            play_opening_screen_game(reading.line, false, candidate, reference, settings,
                                     reading.plies);
        reading.games = (reading.as_candidate_black == 2 || reading.as_candidate_white == 2) ? 0 : 2;
        report.games_played += reading.games;
        reading.valid = reading.games == 2 && reading.as_candidate_black != 0 &&
                        reading.as_candidate_white != 0;
        reading.seat_decided = !reading.discriminates();
        reading.kept = reading.discriminates();
        report.readings.push_back(reading);
    }
    recount_openings(report);
    (void)error;
    return report;
}

std::vector<std::vector<Move>> OpeningScreenReport::lines() const {
    std::vector<std::vector<Move>> kept_lines;
    for (const OpeningReading& reading : readings) {
        if (reading.kept) kept_lines.push_back(reading.line);
    }
    return kept_lines;
}

std::string OpeningScreen::summarize(const OpeningScreenReport& report) {
    const int judged = report.swept_before + report.kept;
    std::ostringstream out;
    out << "opening screen: kept " << report.kept << " of " << report.readings.size()
        << " candidates (" << judged << " judged, "
        << (judged ? 100.0 * report.swept_before / judged : 0.0)
        << "% seat-decided before, " << report.swept_after << " after; " << report.games_played
        << " games, sha " << report.set_sha
        << (report.reused ? " (reused from disk)" : " (screened now)");
    return out.str();
}

std::vector<Move> Trainer::match_opening_for(int index) {
    ensure_openings_screened();
    const int pool = static_cast<int>(screened_openings_.size());
    if (pool >= kMatchOpeningSet) {
        // 与固定轮换同一个取法：按 (轮次 + 对序) 取模，同一轮里候选与冠军拿到同一批局面。
        const int wrapped = ((index % pool) + pool) % pool;
        return screened_openings_[static_cast<size_t>(wrapped)];
    }
    return match_opening_line(index, config_.match_opening_plies);
}

void Trainer::ensure_openings_screened() {
    if (openings_screen_done_) return;
    openings_screen_done_ = true;
    if (config_.opening_screen_candidates <= 0 || fitness_override_) return;

    // 「这两个玩家」取的是**本轮的起点系数 vs 当前冠军**：这是一整轮里唯一固定的那对，
    // 也是适配度赛真正要比的那对——候选是谁并不影响「这条开局能不能把两个不同的
    // 系数集分开」。两侧同值（v0 就是冠军，比如没换参数的续训）时这条判据只会留下
    // 0 条，下面的门槛会把这一代退回固定轮换，不会拿一个空集去训。
    OpeningScreenConfig screen;
    screen.candidates = config_.opening_screen_candidates;
    screen.path = config_.openings_path;
    screen.depth = config_.search_depth;
    screen.max_nodes = config_.max_nodes > 0 ? config_.max_nodes : screen.max_nodes;
    screen.max_moves = config_.max_moves;

    const CoefficientPair base = versions_.empty() ? champion() : versions_[0];
    const CoefficientPair reference = champion();
    const std::string sha = OpeningScreen::set_sha(base, reference, screen);

    OpeningScreenReport report;
    std::string error;
    report.config = screen;
    if (OpeningScreen::load(config_.openings_path, sha, report, error)) {
        if (log_) log_(OpeningScreen::summarize(report));
    } else {
        // 「盘上没有」与「盘上那份是别的系数筛的」都不是故障，重筛就是了；
        // 只有后者值得让读日志的人看见（它说明键真的在管换代）。
        if (log_ && error.find("cannot open") == std::string::npos) {
            log_("opening set on disk not reusable: " + error);
        }
        error.clear();
        report = OpeningScreen::run(base, reference, screen, error);
        if (!error.empty() && log_) log_("opening screen failed: " + error);
        if (!report.readings.empty()) {
            std::string save_error;
            if (!OpeningScreen::save(report, save_error) && log_) {
                log_("opening set not saved: " + save_error);
            }
        }
        if (log_) log_(OpeningScreen::summarize(report));
    }
    screened_openings_ = report.lines();
    if (static_cast<int>(screened_openings_.size()) < kMatchOpeningSet && log_) {
        log_("opening screen kept " + std::to_string(screened_openings_.size()) +
             " < " + std::to_string(kMatchOpeningSet) +
             ": 筛不出可用的集，本轮起退回固定 8 条轮换（指导书 §4 丁-6 杀线的后半段）");
        screened_openings_.clear();
    }
}

}

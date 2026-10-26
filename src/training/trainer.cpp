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
                          double margin_weight, VerificationGame* out) {
    SelfPlayConfig settings;
    settings.max_depth = config_.search_depth;
    settings.timeout_ms = config_.timeout_ms;
    settings.max_nodes = config_.max_nodes;
    settings.max_moves = config_.max_moves;

    // 轮次189 #102：一场配对局从**空表**开始。镜像对的第二局要是带着第一局烘热的置换表，
    // 「候选 ≡ 冠军」就不读 0.5 了：188 那批 190 轮里同版候选的验证赛 **179 次读 1.000000**
    // （SPRT 6 局就 accept），把下面两行拆掉则本轮的用例当场红在 `copy.score == 0.5` 上。
    // 上面 `score_match` 那对引擎是为「跨候选」才新建的，这两句管的是**跨局**。
    black.transposition_table().clear();
    white.transposition_table().clear();
    SelfPlay play(black, white, settings);
    GameRecord record = play.run(opening, nullptr, nullptr);
    const bool valid = record.reason != GameEndReason::INVALID;
    if (played) *played = valid;
    // 逐场台账在**任何返回之前**填好，包括被丢弃那一条：`pair_index / seat / score`
    // 是调用方（`score_match`）才有的信息，这里只交回这一场自己的事实。
    if (out) {
        out->valid = valid;
        out->plies = static_cast<int>(record.moves.size());
        out->winner = record.winner;
        out->reason = record.reason_name();
        out->score = 0.0;  // 覆盖调用方写进去的分，只在下面正常路径上再赋值
    }
    // Same yardstick as the data side: an INVALID game is discarded, so it
    // must not inflate games_played_ in the checkpoint or the progress line.
    if (valid) { games_played_++; note_opening_game(record, opening); }
    if (!valid) return 0.0;

    double result;
    if (record.winner == EMPTY) {
        // Unfinished games score half: they must not hand either side a result
        // the board does not show.
        result = 0.5;
    } else {
        const int winner_owner = record.winner == BLACK ? black_owner : 3 - black_owner;
        const double binary = winner_owner == 1 ? 1.0 : 0.0;
        result = margin_weight <= 0.0
                     // 默认形状：胜负对原样返回。这一句早退是「w = 0 与今天逐字节一致」那条
                     // 要求（指导书 §4 丁-1 实现思路 3）的执行处，不是省几行浮点。
                     ? binary
                     : blend_fitness(binary, game_margin(record, settings.max_moves), margin_weight);
    }
    if (out) out->score = result;
    return result;
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
        VerificationGame first_row;
        first_row.pair_index = i;
        first_row.seat = 1;  // 这一场候选执黑（`black_owner` 就是 1）
        const double first =
            play_game(candidate_engine, reference_engine, 1, opening, &valid, margin_weight,
                      &first_row);
        first_row.score = first;
        outcome.game_log.push_back(first_row);
        if (valid) {
            score += first;
            ++played_games;
            test.add_game(first);
        }
        valid = false;
        VerificationGame second_row;
        second_row.pair_index = i;
        second_row.seat = 2;  // 同一开局换边：这一场候选执白
        const double second =
            play_game(reference_engine, candidate_engine, 2, opening, &valid, margin_weight,
                      &second_row);
        second_row.score = second;
        outcome.game_log.push_back(second_row);
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
        // #104：逐次评估的读数口。以前一轮只留一行「best fitness」，那 183/190 次「晋的是
        // 复制体」只能靠外面的脚本从 checkpoint 的 versions 指纹里拼出来 —— 这条把每次评估
        // 的分数、有没有被接受、评的是不是冠军本体直接落到同一份日志里。
        if (log_) {
            const int logged_round = result.round;
            optimizer.set_evaluation_observer(
                [this, logged_round](const HillClimbingOptimizer::EvaluationRecord& e) {
                    log_("round " + std::to_string(logged_round) + ": eval " +
                         std::to_string(e.step) + " fitness " + std::to_string(e.fitness) +
                         (e.accepted ? " accepted" : " kept") +
                         (e.same_as_start ? " champion-itself" : ""));
                });
        }
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
    result.verification_games = check.game_log;
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
        // #110：聚合行之后逐场各一行。上面那行今天能说「6 games、score 1.000000」，
        // 说不出的是这 6 场里候选两侧各赢几场、有没有 INVALID 被剔出分母、SPRT 停在
        // 第几对 —— 而那正是核对内部尺与外部赛具读数（0.500 对 1.000）唯一能核的东西。
        // 被丢弃的场也印（valid=0），一条场都不许只留在分母里。
        for (const VerificationGame& game : result.verification_games) {
            log_("round " + std::to_string(result.round) + ": verif pair " +
                 std::to_string(game.pair_index) + " seat " + (game.seat == 1 ? "black" : "white") +
                 " score " + std::to_string(game.score) + " valid " +
                 std::to_string(game.valid ? 1 : 0) + " plies " + std::to_string(game.plies) +
                 " reason " + game.reason);
        }
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
// 186-260、checkpoint 的 `versions[]` 632-635）整体推走。
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

// 一局跑完之后由 `Trainer::play_game` 递过来的**座位**赢家。
int opening_seat_winner(const GameRecord& record) {
    if (record.winner == BLACK) return 1;
    if (record.winner == WHITE) return -1;
    return 0;
}

bool same_opening_line(const std::vector<Move>& a, const std::vector<Move>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].pos.row != b[i].pos.row || a[i].pos.col != b[i].pos.col ||
            a[i].color != b[i].color) {
            return false;
        }
    }
    return true;
}

// 两个计数都从读数重算：写下去的与读回来的走同一条口径。
void recount_ledger(OpeningLedgerState& state) {
    state.observed_pairs = 0;
    state.retired = 0;
    for (const OpeningLedgerEntry& entry : state.entries) {
        state.observed_pairs += entry.pairs;
        if (entry.retired) state.retired++;
    }
}

}  // namespace

std::string OpeningLedger::set_sha(const CoefficientPair& base,
                                   const OpeningLedgerConfig& config) {
    // 键是**这次 run 起点那一套系数**（`versions_[0]`）的黑白两个半套指纹，加上产生这些证据的
    // 档位。冠军换代不动它——已经花掉的对局证据不该被清空；起点换了（续训、换参数文件）才作废。
    return coefficients_fingerprint(base.black) + "-" +
           coefficients_fingerprint(base.white) +
           "-s" + std::to_string(config.stones) + "d" + std::to_string(config.depth) + "n" +
           std::to_string(config.max_nodes) + "m" + std::to_string(config.max_moves) + "p" +
           std::to_string(config.pool) + "q" + std::to_string(config.min_pairs);
}

OpeningLedgerState OpeningLedger::fresh(const OpeningLedgerConfig& config,
                                        const std::string& sha) {
    OpeningLedgerState state;
    state.config = config;
    state.set_sha = sha;
    for (int i = 0; i < config.pool; ++i) {
        OpeningLedgerEntry entry;
        entry.line = Trainer::match_opening_line(i, config.stones);
        // 生成器一路都走 `legal_moves`，唯一可能的短是盘面提前满了；那种开局不进池子。
        if (static_cast<int>(entry.line.size()) != config.stones) continue;
        Board prefix;
        bool placeable = true;
        for (const Move& stone : entry.line) {
            if (!prefix.place_stone(stone.pos.row, stone.pos.col, stone.color)) placeable = false;
        }
        if (placeable) state.entries.push_back(std::move(entry));
    }
    recount_ledger(state);
    return state;
}

bool OpeningLedger::load(const std::string& path, const std::string& sha, OpeningLedgerState& out,
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
        error = std::string("opening ledger is not valid JSON: ") + e.what();
        return false;
    }
    if (!root.is_object() || !root.contains("set_sha") || !root["set_sha"].is_string()) {
        error = "opening ledger has no set_sha";
        return false;
    }
    const std::string stored = root["set_sha"].get<std::string>();
    if (stored != sha) {
        // 换起点系数或换档：那本账记的是**另一对玩家**在哪一档下被座位钉死，留着比没有更糟。
        error = "opening ledger was kept under " + stored + ", this run needs " + sha;
        return false;
    }
    if (!root.contains("entries") || !root["entries"].is_array()) {
        error = "opening ledger has no entries";
        return false;
    }

    OpeningLedgerState state;
    state.config = out.config;   // 档位由调用方给，它也是 sha 的来源
    state.set_sha = stored;
    for (const json& item : root["entries"]) {
        if (!item.is_object()) {
            error = "opening ledger entry is not an object";
            return false;
        }
        if (!item.contains("line") || !item["line"].is_array()) {
            error = "opening ledger entry has no line";
            return false;
        }
        OpeningLedgerEntry entry;
        for (const json& stone : item["line"]) {
            if (!stone.is_array() || stone.size() < 3 || !stone[2].is_string()) {
                error = "opening ledger entry has an unreadable stone";
                return false;
            }
            entry.line.push_back(Move(stone[0].get<int>(), stone[1].get<int>(),
                                      opening_color_from_name(stone[2].get<std::string>())));
        }
        for (const char* key : {"pairs", "pinned_pairs"}) {
            if (!item.contains(key) || !item[key].is_number_integer()) {
                error = std::string("opening ledger entry is missing ") + key;
                return false;
            }
        }
        entry.pairs = item["pairs"].get<int>();
        entry.pinned_pairs = item["pinned_pairs"].get<int>();
        // 判决本身也从读数重算，不信文件里写的那个布尔：一本「pairs 够、全被钉死」的账
        // 必须退役，写反了的文件改不过来它。
        entry.retired = entry.pairs >= state.config.min_pairs &&
                        entry.pairs > 0 && entry.pinned_pairs == entry.pairs;
        // 半途的那一局不落盘：读回来时它总是空的，下次跑到同一开局会重新凑一对。
        state.entries.push_back(std::move(entry));
    }
    recount_ledger(state);
    state.reused = true;
    out = state;
    return true;
}

bool OpeningLedger::save(const OpeningLedgerState& state, std::string& error) {
    json root;
    root["schema"] = "openings/2";
    root["set_sha"] = state.set_sha;
    root["observed_pairs"] = state.observed_pairs;
    root["retired"] = state.retired;
    root["pool"] = static_cast<int>(state.entries.size());
    root["min_pairs"] = state.config.min_pairs;
    root["stones"] = state.config.stones;
    root["depth"] = state.config.depth;
    root["max_nodes"] = state.config.max_nodes;
    root["max_moves"] = state.config.max_moves;

    json entries = json::array();
    for (const OpeningLedgerEntry& entry : state.entries) {
        json stones = json::array();
        for (const Move& stone : entry.line) {
            stones.push_back(
                json::array({stone.pos.row, stone.pos.col, opening_color_name(stone.color)}));
        }
        json item;
        item["line"] = std::move(stones);
        item["pairs"] = entry.pairs;
        item["pinned_pairs"] = entry.pinned_pairs;
        item["retired"] = entry.retired;
        entries.push_back(std::move(item));
    }
    root["entries"] = std::move(entries);

    // 与参数文件、checkpoint 同一个写法：先写临时文件再改名，半写的一份不会顶掉能用的那份。
    const std::string tmp = state.config.path + ".tmp";
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
    std::filesystem::rename(tmp, state.config.path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
        error = "cannot replace " + state.config.path + ": " + ec.message();
        return false;
    }
    return true;
}

int OpeningLedger::record_game(OpeningLedgerState& state, const std::vector<Move>& line,
                               int seat_winner) {
    if (state.set_sha.empty() || state.config.pool <= 0) return 0;
    OpeningLedgerEntry* target = nullptr;
    for (OpeningLedgerEntry& entry : state.entries) {
        if (same_opening_line(entry.line, line)) {
            target = &entry;
            break;
        }
    }
    // 池外的开局不记：账本只对自己那几条负责，固定轮换兜底时用的就是同一批线，正常都在池里。
    if (target == nullptr) return 0;

    if (!target->has_pending) {   // 半个对子先站着，等它的姊妹局
        target->has_pending = true;
        target->pending_seat = seat_winner;
        return 0;
    }
    const int first = target->pending_seat;
    const int second = seat_winner;
    target->has_pending = false;
    target->pending_seat = 0;

    target->pairs++;
    state.observed_pairs++;
    // 「被座位钉死」要求两局**都分了胜负**且赢家是同一个座位。含未分胜负的对不算证据：
    // 它既没被钉死，也没证明这条开局能把两个人分开，两种意思都不能由它说。
    const bool pinned = first != 0 && second != 0 && first == second;
    if (pinned) target->pinned_pairs++;

    // 判决是读数的纯函数：够数且**每一对**都被钉死才退役，出现过一对能区分的就恢复。
    const bool should_retire = target->pairs >= state.config.min_pairs &&
                               target->pinned_pairs == target->pairs;
    if (should_retire && !target->retired) {
        target->retired = true;
        state.retired++;
        return 1;
    }
    if (!should_retire && target->retired) {
        target->retired = false;
        state.retired--;
        return -1;
    }
    return 0;
}

std::vector<std::vector<Move>> OpeningLedgerState::active_lines() const {
    std::vector<std::vector<Move>> active;
    for (const OpeningLedgerEntry& entry : entries) {
        if (!entry.retired) active.push_back(entry.line);
    }
    return active;
}

std::string OpeningLedger::summarize(const OpeningLedgerState& state) {
    std::ostringstream out;
    out << "opening ledger: " << state.entries.size() << " lines, " << state.observed_pairs
        << " pairs observed, " << state.retired << " retired (min_pairs "
        << state.config.min_pairs << ", sha " << state.set_sha
        << (state.reused ? "; reused from disk)" : "; kept in this run)");
    return out.str();
}

std::vector<Move> Trainer::match_opening_for(int index) {
    ensure_openings_loaded();
    const std::vector<std::vector<Move>> active = openings_.active_lines();
    const int pool = static_cast<int>(active.size());
    if (pool >= kMatchOpeningSet) {
        // 与固定轮换同一个取法：按 (轮次 + 对序) 取模，同一轮里候选与冠军拿到同一批局面。
        const int wrapped = ((index % pool) + pool) % pool;
        return active[static_cast<size_t>(wrapped)];
    }
    return match_opening_line(index, config_.match_opening_plies);
}

void Trainer::note_opening_game(const GameRecord& record, const std::vector<Move>& opening) {
    if (config_.opening_pool <= 0 || fitness_override_) return;
    ensure_openings_loaded();
    if (openings_.set_sha.empty()) return;

    const int change = OpeningLedger::record_game(openings_, opening, opening_seat_winner(record));
    if (change == 0) return;
    // 只有判决翻转才写盘：账本小（≤ pool 条），但一局一写会把适配度赛变成磁盘赛。
    std::string error;
    if (!OpeningLedger::save(openings_, error) && log_) {
        log_("opening ledger not saved: " + error);
    }
    if (log_) {
        log_(change > 0 ? "opening retired: 这一对两局都由同一座位赢下，累计看满 " +
                              std::to_string(openings_.config.min_pairs) +
                              " 对都是这样 ⇒ 这条开局出局（§4 丁-6 的事后退役）"
                        : "opening restored: 退役过的开局出现一对能区分两个玩家 ⇒ 回到轮换里");
    }
}

void Trainer::ensure_openings_loaded() {
    if (openings_loaded_) return;
    openings_loaded_ = true;
    if (config_.opening_pool <= 0 || fitness_override_) return;

    OpeningLedgerConfig ledger;
    ledger.pool = config_.opening_pool;
    ledger.min_pairs = config_.opening_min_pairs > 0 ? config_.opening_min_pairs : 1;
    ledger.path = config_.openings_path;
    ledger.stones = config_.match_opening_plies;
    ledger.depth = config_.search_depth;
    ledger.max_nodes = config_.max_nodes;
    ledger.max_moves = config_.max_moves;

    // 「这一本账是谁的」取的是**这次 run 起点的系数**（`versions_[0]`），不是当前冠军：证据是从
    // 适配度赛里顺手记的，冠军换代不该把已经花掉的对局清空；起点变了才该重记。
    const CoefficientPair base = versions_.empty() ? champion() : versions_[0];
    const std::string sha = OpeningLedger::set_sha(base, ledger);

    OpeningLedgerState state;
    state.config = ledger;
    std::string error;
    if (OpeningLedger::load(config_.openings_path, sha, state, error)) {
        if (log_) log_(OpeningLedger::summarize(state));
    } else {
        // 「盘上没有」与「盘上那本是别的起点记的」都不是故障，重开一本就是了；
        // 只有后者值得让读日志的人看见（它说明键真的在管换代）。
        if (log_ && error.find("cannot open") == std::string::npos) {
            log_("opening ledger on disk not reusable: " + error);
        }
        state = OpeningLedger::fresh(ledger, sha);
        std::string save_error;
        if (!OpeningLedger::save(state, save_error) && log_) {
            log_("opening ledger not saved: " + save_error);
        }
        if (log_) log_(OpeningLedger::summarize(state));
    }
    openings_ = std::move(state);
    if (static_cast<int>(openings_.active_lines().size()) < kMatchOpeningSet && log_) {
        log_("opening ledger leaves " +
             std::to_string(openings_.active_lines().size()) + " < " +
             std::to_string(kMatchOpeningSet) +
             ": 可用的开局不够一轮轮换，退回固定 8 条（指导书 §4 丁-6 杀线的后半段）");
    }
}

}

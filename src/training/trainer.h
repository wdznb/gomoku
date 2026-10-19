#ifndef GOMOKU_TRAINER_H
#define GOMOKU_TRAINER_H

#include "../core/evaluator.h"
#include "../search/search_engine.h"
#include "data_collector.h"
#include "elo.h"
#include "game_record.h"
#include "genetic.h"
#include "hill_climbing.h"
#include "sprt.h"
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace gomoku {

enum class OptimizerKind {
    GENETIC,
    HILL_CLIMBING,
};

struct TrainingConfig {
    // Rounds to run when the CLI is not given --rounds. Each round optimises and
    // then verifies one candidate.
    int rounds = 3;

    // How hard the engines think during training matches. Everything in a
    // training round is multiplied out by population x games x plies, so these
    // are the knobs that decide how long a run takes.
    int search_depth = 4;
    int timeout_ms = 400;
    // Nodes one think may spend during training matches. Zero keeps today's cost
    // model, where the ceiling is what the remaining milliseconds derive. A named
    // value makes a ply cost the same on an idle and a loaded machine, and
    // timeout_ms then only backs the think up if it runs long.
    int max_nodes = 0;
    int max_moves = 60;
    bool resign_enabled = true;

    OptimizerKind optimizer = OptimizerKind::GENETIC;
    int population_size = 8;
    int generations = 4;
    int stagnation_generations = 3;
    int hill_climbing_iterations = 12;

    // Games played to score one candidate, and games played before a candidate
    // is allowed to replace the champion. Both are rounded up to an even count
    // because every pairing is played once with each side.
    int fitness_games = 2;
    int verification_games = 8;
    // A candidate must beat the champion by this score to be promoted.
    double accept_score = 0.56;
    // The Elo gap a match's results are tested against (design doc 2.8 #3). Both
    // doors above are ceilings now: a match stops at a pair boundary as soon as
    // the sequential test leaves the bounds, so an eight-game verification costs
    // four games when the candidate loses both colours twice. 0 plays every pair
    // the counts ask for, which is what both flags meant before. sprt.h carries
    // the arithmetic that makes 300 the smallest default the ceiling can reach.
    int sprt_elo = 300;

    // Self-play games generated per round purely to collect training data.
    int data_games = 4;
    int data_threads = 2;
    int data_opening_plies = 2;
    bool collect_data = true;
    // Slots in the one transposition table every data worker shares (design doc 2.8
    // #1's 进程级共享 TT；0 turns sharing off and each worker keeps its own). The
    // default is a worker's own table size, 2^18 x 28B ~= 7.3MB, because that is what
    // makes the claim true rather than bigger: sharing at this size costs 7.3MB once
    // instead of 7.3MB per --threads, and the doc's 2^22 example would need 16
    // concurrent workers before it beat the tables it replaces. Naming a larger number
    // buys hit rate on a machine with the memory for it.
    int shared_tt_entries = 1 << 18;

    uint64_t seed = 20261011;

    std::string records_path;
    std::string samples_path;
    std::string params_path;
    // Written at the end of every round so an interrupted run can be resumed.
    std::string checkpoint_path;
};

struct TrainerRound {
    int round = 0;
    ThreatCoefficients champion;
    ThreatCoefficients candidate;
    double champion_fitness = 0.0;
    double candidate_fitness = 0.0;
    double verification_score = 0.0;
    // What the sequential test had to say about the match that produced that
    // score, and how many games it needed to say it. OPEN means the ceiling was
    // reached with no decision and --accept-score was the rule that fired; the
    // game count is the match's own, not the run's running total.
    SprtVerdict verification_verdict = SprtVerdict::OPEN;
    double verification_llr = 0.0;
    int verification_match_games = 0;
    bool accepted = false;
    int games_played = 0;
    int samples_collected = 0;
    double champion_elo = 0.0;
    double candidate_elo = 0.0;
};

// One match's answer. `score` is what the optimisers rank candidates on and what
// the Elo table books; the three fields after it are the sequential test's own
// reading of the same games, kept so a round can show *why* a candidate was
// promoted instead of only the fraction that cleared the bar.
struct MatchOutcome {
    double score = 0.0;
    int games = 0;
    SprtVerdict verdict = SprtVerdict::OPEN;
    double llr = 0.0;
};

// 3.6 training main loop: optimise coefficients, prove the result over the table
// by playing it against the champion, keep the ratings, and bank games plus
// samples for later analysis.
class Trainer {
public:
    using LogFn = std::function<void(const std::string& line)>;

    Trainer(const ThreatCoefficients& base, const TrainingConfig& config);

    // Score coefficient sets without playing: used by the test suite and by any
    // tool that optimises a different objective. When set, verification becomes
    // a direct comparison instead of a match.
    void set_fitness_override(CoefficientFitness fitness);
    void set_log_callback(LogFn callback) { log_ = std::move(callback); }

    TrainerRound run_round();
    std::vector<TrainerRound> run(int rounds);

    const ThreatCoefficients& champion() const { return versions_[best_version_]; }
    const std::vector<ThreatCoefficients>& versions() const { return versions_; }
    const EloSystem& ratings() const { return elo_; }
    size_t samples_collected() const { return samples_collected_; }
    int games_played() const { return games_played_; }
    int rounds_completed() const { return rounds_completed_; }
    const TrainingConfig& config() const { return config_; }

    // The table the data workers are handed, or nullptr when --shared-tt 0 left each
    // worker with its own. Reading it from outside is how a test tells a shared table
    // from a table that merely happens to be the same size.
    TranspositionTable* shared_transposition_table() const { return shared_tt_.get(); }

    // One-line progress summary, used by the CLI and by resume messages.
    std::string status_text() const;

    // A short random opening so parallel workers do not all replay one line.
    static std::vector<Move> random_opening_line(int plies, uint64_t seed);

    bool save_checkpoint(const std::string& path, std::string& error) const;
    bool load_checkpoint(const std::string& path, std::string& error);

private:
    double measure_fitness(const ThreatCoefficients& coefficients);
    MatchOutcome score_match(const ThreatCoefficients& candidate,
                             const ThreatCoefficients& reference, int games);
    double play_match(const ThreatCoefficients& candidate, const ThreatCoefficients& reference,
                      int games);
    // Score for whoever holds the stones passed as `black_owner` (1 = the
    // candidate, 2 = the reference). `played` says whether the game happened at
    // all: a discarded INVALID one is not a draw and leaves the denominator.
    double play_game(SearchEngine& black, SearchEngine& white, int black_owner,
                     bool* played = nullptr);
    SearchEngine& prepare(SearchEngine& engine) const;
    int generate_data_games(int games, int threads);

    TrainingConfig config_;
    std::vector<ThreatCoefficients> versions_;
    size_t best_version_ = 0;
    EloSystem elo_;
    CoefficientFitness fitness_override_;
    LogFn log_;
    // Built once for the whole Trainer when --shared-tt names a size, so the flag that
    // decides whether a probe takes a shard lock is set before any worker exists.
    std::unique_ptr<TranspositionTable> shared_tt_;
    int games_played_ = 0;
    size_t samples_collected_ = 0;
    int rounds_completed_ = 0;
};

}

#endif

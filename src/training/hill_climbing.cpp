#include "hill_climbing.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace gomoku {

HillClimbingOptimizer::HillClimbingOptimizer(CoefficientFitness fitness,
                                             const HillClimbingConfig& config,
                                             uint64_t seed)
    : fitness_(std::move(fitness)), config_(config), rng_(seed) {
    if (config_.multipliers.empty()) config_.multipliers = {1.6, 0.625};
    config_.max_iterations = std::max(1, config_.max_iterations);
    config_.stagnation_limit = std::max(1, config_.stagnation_limit);
}

std::vector<int> HillClimbingOptimizer::neighbor(const std::vector<int>& genes, size_t index,
                                                 double multiplier) {
    std::vector<int> candidate = genes;
    if (index >= candidate.size()) return candidate;

    const double scaled = candidate[index] * multiplier;
    const double rounded = std::llround(scaled);
    // Never collapse a positive coefficient to zero or let it wrap past its own
    // bound by rounding; sanitize_vector finishes the job. It reads the width to
    // decide what it is sanitizing - 14 genes is one set, 28 is a pair - so a
    // probe into the white half cannot be repaired with black's rules alone.
    candidate[index] = rounded <= 0 ? 1 : static_cast<int>(rounded);
    CoefficientVector::sanitize_vector(candidate);
    return candidate;
}

CoefficientPair HillClimbingOptimizer::run(const CoefficientPair& base,
                                            int& iterations_run,
                                            double& best_fitness) {
    iterations_run = 0;
    converged_ = false;
    stagnation_ = 0;
    best_history_.clear();

    std::vector<int> current = CoefficientVector::to_vector(base);
    const std::vector<int> start = current;   // 「候选 ≡ 冠军」就是与这一套逐字相同
    CoefficientPair current_coefficients = CoefficientVector::pair_from_vector(current);
    double current_fitness = fitness_(current_coefficients);
    evaluations_++;
    auto emit = [this](const EvaluationRecord& record) {
        if (observer_) observer_(record);
    };
    {
        EvaluationRecord baseline;
        baseline.step = 0;
        baseline.fitness = current_fitness;
        baseline.same_as_start = true;
        emit(baseline);
    }

    CoefficientPair best_coefficients = current_coefficients;
    best_fitness = current_fitness;
    best_history_.push_back(best_fitness);

    std::uniform_int_distribution<size_t> pick_index(0, current.size() - 1);

    // Only probes that actually differ are steps: iterations_run, evaluations and
    // best_history_ all count those, not the raw loop index.
    int steps = 0;

    for (int iteration = 1; iteration <= config_.max_iterations; ++iteration) {
        const size_t index = CoefficientVector::perturbable_slot(pick_index(rng_), current.size());
        const size_t slot = rng_() % config_.multipliers.size();
        const std::vector<int> candidate = neighbor(current, index, config_.multipliers[slot]);
        // A rescale can round back to the same integer, and a step that changed
        // nothing is not a failed probe: counting it as stagnation converges the
        // walk before it has tried a single different point.
        if (candidate == current) continue;
        ++steps;
        const CoefficientPair candidate_coefficients =
            CoefficientVector::pair_from_vector(candidate);
        const double candidate_fitness = fitness_(candidate_coefficients);
        evaluations_++;
        // 接受与否先算成一条判据，读数与走子用的是**同一个** bool —— 否则日志里那列
        // accepted 可能与真正走的那一步不一致。
        const bool accepted = candidate_fitness > current_fitness + 1e-9;
        EvaluationRecord record;
        record.step = steps;
        record.fitness = candidate_fitness;
        record.accepted = accepted;
        record.same_as_start = (candidate == start);
        record.gene_index = index;
        record.multiplier = config_.multipliers[slot];

        // Accept only strict improvements over the point we are standing on, so
        // the walk climbs instead of drifting sideways through ties.
        if (accepted) {
            current = candidate;
            current_coefficients = candidate_coefficients;
            current_fitness = candidate_fitness;
            stagnation_ = 0;
            if (candidate_fitness > best_fitness) {
                best_coefficients = candidate_coefficients;
                best_fitness = candidate_fitness;
            }
        } else {
            stagnation_++;
            if (stagnation_ >= config_.stagnation_limit) {
                converged_ = true;
                iterations_run = steps;
                best_history_.push_back(best_fitness);
                emit(record);   // 收敛这一支直接 return，读数必须在 return 之前发出去
                return best_coefficients;
            }
        }

        emit(record);
        best_history_.push_back(best_fitness);
        iterations_run = steps;
    }

    return best_coefficients;
}

}

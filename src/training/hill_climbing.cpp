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
    // bound by rounding; sanitize_vector finishes the job.
    candidate[index] = rounded <= 0 ? 1 : static_cast<int>(rounded);
    CoefficientVector::sanitize_vector(candidate);
    return candidate;
}

ThreatCoefficients HillClimbingOptimizer::run(const ThreatCoefficients& base,
                                              int& iterations_run,
                                              double& best_fitness) {
    iterations_run = 0;
    converged_ = false;
    stagnation_ = 0;
    best_history_.clear();

    std::vector<int> current = CoefficientVector::to_vector(base);
    ThreatCoefficients current_coefficients = CoefficientVector::from_vector(current);
    double current_fitness = fitness_(current_coefficients);
    evaluations_++;

    ThreatCoefficients best_coefficients = current_coefficients;
    best_fitness = current_fitness;
    best_history_.push_back(best_fitness);

    std::uniform_int_distribution<size_t> pick_index(0, current.size() - 1);

    for (int iteration = 1; iteration <= config_.max_iterations; ++iteration) {
        const size_t index = pick_index(rng_);
        const size_t slot = rng_() % config_.multipliers.size();
        const std::vector<int> candidate = neighbor(current, index, config_.multipliers[slot]);
        const ThreatCoefficients candidate_coefficients = CoefficientVector::from_vector(candidate);
        const double candidate_fitness = fitness_(candidate_coefficients);
        evaluations_++;

        // Accept only strict improvements over the point we are standing on, so
        // the walk climbs instead of drifting sideways through ties.
        if (candidate_fitness > current_fitness + 1e-9) {
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
                iterations_run = iteration;
                best_history_.push_back(best_fitness);
                return best_coefficients;
            }
        }

        best_history_.push_back(best_fitness);
        iterations_run = iteration;
    }

    return best_coefficients;
}

}

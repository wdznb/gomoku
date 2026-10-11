#ifndef GOMOKU_HILL_CLIMBING_H
#define GOMOKU_HILL_CLIMBING_H

#include "../core/evaluator.h"
#include "params.h"
#include <functional>
#include <random>
#include <vector>
#include "genetic.h"

namespace gomoku {

struct HillClimbingConfig {
    int max_iterations = 40;
    // Each probe rescales one coefficient by one of these factors, so the step
    // size stays sensible whether the coefficient is 10 or 50000.
    std::vector<double> multipliers = {1.6, 0.625};
    // The search stops once this many consecutive probes fail to improve.
    int stagnation_limit = 12;
};

class HillClimbingOptimizer {
public:
    HillClimbingOptimizer(CoefficientFitness fitness, const HillClimbingConfig& config,
                          uint64_t seed);

    // Coordinate-wise greedy walk: rescale one coefficient, keep it if the score
    // rises. Cheaper per step than the genetic search, and it only ever moves
    // away from a set that the evaluation function already likes.
    ThreatCoefficients run(const ThreatCoefficients& base,
                           int& iterations_run,
                           double& best_fitness);

    bool converged() const { return converged_; }
    int evaluations() const { return evaluations_; }
    const std::vector<double>& best_history() const { return best_history_; }

    // One rescaled coordinate, already sanitized. Public for direct testing.
    static std::vector<int> neighbor(const std::vector<int>& genes, size_t index,
                                     double multiplier);

private:
    CoefficientFitness fitness_;
    HillClimbingConfig config_;
    mutable std::mt19937_64 rng_;
    std::vector<double> best_history_;
    int evaluations_ = 0;
    int stagnation_ = 0;
    bool converged_ = false;
};

}

#endif

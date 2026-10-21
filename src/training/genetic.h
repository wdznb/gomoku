#ifndef GOMOKU_GENETIC_H
#define GOMOKU_GENETIC_H

#include "../core/evaluator.h"
#include "params.h"
#include <functional>
#include <random>
#include <vector>

namespace gomoku {

// Score of a candidate. Higher is stronger; the trainer fills this in with
// self-play results, and tests fill it in with arithmetic. 丁-3: a candidate is a
// *pair* of coefficient sets, because that is what an engine is built from once
// the two colours may hold different numbers - scoring half a pair would score a
// set no player ever sits down with.
using CoefficientFitness = std::function<double(const CoefficientPair&)>;

struct GeneticConfig {
    int population_size = 8;
    // Fraction of each new generation produced from two parents; the rest are
    // copies of a tournament-selected parent.
    double crossover_ratio = 0.6;
    double mutation_rate = 0.5;
    // Standard deviation of the multiplicative noise applied to a mutated gene.
    double mutation_sigma = 0.2;
    int tournament_size = 3;
    int stagnation_generations = 4;
    int max_generations = 20;
};

class GeneticOptimizer {
public:
    struct Individual {
        // Side-major: genes 0..13 are the black set and 14..27 the white one
        // (CoefficientVector::PAIR_GENE_COUNT).
        std::vector<int> genes;
        CoefficientPair coefficients;
        double fitness = 0.0;
    };

    GeneticOptimizer(CoefficientFitness fitness, const GeneticConfig& config, uint64_t seed);

    // Evolves until the best result stops improving or max_generations is
    // reached. `generations_run` and `best_fitness` report what actually
    // happened; the caller owns the decision to keep training.
    CoefficientPair run(const CoefficientPair& base,
                        int& generations_run,
                        double& best_fitness);

    bool converged() const { return converged_; }
    const std::vector<Individual>& population() const { return population_; }
    const std::vector<double>& best_history() const { return best_history_; }

    // Fitness evaluations consumed - training cost is dominated by this number.
    int evaluations() const { return evaluations_; }

    // Public for direct testing; run() is what uses them in sequence.
    int select_parent_index() const;
    static std::vector<int> crossover(const std::vector<int>& lhs,
                                      const std::vector<int>& rhs,
                                      double blend,
                                      std::mt19937_64& rng);
    std::vector<int> mutate(const std::vector<int>& genes) const;

private:
    void seed_population(const CoefficientPair& base);
    void evaluate(std::vector<Individual>& generation);

    CoefficientFitness fitness_;
    GeneticConfig config_;
    mutable std::mt19937_64 rng_;
    std::vector<Individual> population_;
    std::vector<double> best_history_;
    int evaluations_ = 0;
    int generations_since_improvement_ = 0;
    double best_fitness_ = 0.0;
    bool converged_ = false;
};

}

#endif

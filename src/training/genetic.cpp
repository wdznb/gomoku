#include "genetic.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace gomoku {

namespace {

double clamp_double(double value, double low, double high) {
    return std::max(low, std::min(high, value));
}

}

GeneticOptimizer::GeneticOptimizer(CoefficientFitness fitness, const GeneticConfig& config,
                                   uint64_t seed)
    : fitness_(std::move(fitness)), config_(config), rng_(seed) {
    config_.population_size = std::max(2, config_.population_size);
    config_.tournament_size = std::max(2, std::min(config_.tournament_size,
                                                   config_.population_size));
    config_.crossover_ratio = clamp_double(config_.crossover_ratio, 0.0, 0.9);
    config_.mutation_rate = clamp_double(config_.mutation_rate, 0.0, 1.0);
    config_.mutation_sigma = clamp_double(config_.mutation_sigma, 0.01, 1.0);
}

void GeneticOptimizer::seed_population(const ThreatCoefficients& base) {
    const std::vector<int> base_genes = CoefficientVector::to_vector(base);

    population_.clear();
    population_.reserve(config_.population_size);

    Individual founder;
    founder.genes = base_genes;
    founder.coefficients = base;
    population_.push_back(founder);

    std::uniform_real_distribution<double> noise(-0.5, 0.5);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    for (int i = 1; i < config_.population_size; ++i) {
        std::vector<int> genes = base_genes;
        for (int g = 0; g < static_cast<int>(genes.size()); ++g) {
            const ParamBound bound = CoefficientVector::bound_for(static_cast<TunableParam>(g));
            const double span = static_cast<double>(bound.max_value - bound.min_value);
            // Half the members keep the human shape and get one randomly placed
            // nudge per gene, the rest are redrawn across the whole box.
            if (i % 2 == 0) {
                const size_t target = rng_() % genes.size();
                genes[target] += static_cast<int>(noise(rng_) * span * 0.25);
            } else {
                genes[g] = static_cast<int>(bound.min_value + uniform(rng_) * span);
            }
        }
        CoefficientVector::sanitize_vector(genes);
        Individual member;
        member.genes = genes;
        member.coefficients = CoefficientVector::from_vector(genes);
        population_.push_back(member);
    }
}

void GeneticOptimizer::evaluate(std::vector<Individual>& generation) {
    for (auto& member : generation) {
        member.fitness = fitness_(member.coefficients);
        evaluations_++;
    }
}

int GeneticOptimizer::select_parent_index() const {
    std::uniform_int_distribution<size_t> pick(0, population_.size() - 1);

    int best_index = static_cast<int>(pick(rng_));
    double best_fitness = population_[best_index].fitness;

    for (int i = 1; i < config_.tournament_size; ++i) {
        const int candidate = static_cast<int>(pick(rng_));
        if (population_[candidate].fitness > best_fitness) {
            best_index = candidate;
            best_fitness = population_[candidate].fitness;
        }
    }
    return best_index;
}

std::vector<int> GeneticOptimizer::crossover(const std::vector<int>& lhs,
                                             const std::vector<int>& rhs,
                                             double blend,
                                             std::mt19937_64& rng) {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::vector<int> child;
    child.reserve(lhs.size());

    for (size_t g = 0; g < lhs.size(); ++g) {
        if (unit(rng) < blend) {
            // Arithmetic recombination can reach values neither parent carries.
            const double weight = unit(rng);
            const double mixed = lhs[g] * (1.0 - weight) + rhs[g] * weight;
            child.push_back(static_cast<int>(std::llround(mixed)));
        } else {
            child.push_back(unit(rng) < 0.5 ? lhs[g] : rhs[g]);
        }
    }
    return child;
}

std::vector<int> GeneticOptimizer::mutate(const std::vector<int>& genes) const {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::normal_distribution<double> gaussian(0.0, config_.mutation_sigma);

    std::vector<int> result = genes;
    for (size_t g = 0; g < result.size(); ++g) {
        if (unit(rng_) >= 0.5) continue;
        const double perturbed = result[g] * (1.0 + gaussian(rng_));
        result[g] = static_cast<int>(std::llround(perturbed));
    }
    CoefficientVector::sanitize_vector(result);
    return result;
}

ThreatCoefficients GeneticOptimizer::run(const ThreatCoefficients& base,
                                         int& generations_run,
                                         double& best_fitness) {
    generations_run = 0;
    best_fitness = 0.0;
    converged_ = false;
    best_history_.clear();
    // Stagnation is measured inside one run: a second run on this object would
    // otherwise inherit the previous one's count and stop after a single
    // generation. evaluations_ is the caller's cost counter and stays cumulative.
    generations_since_improvement_ = 0;

    seed_population(base);
    evaluate(population_);

    auto strongest = std::max_element(population_.begin(), population_.end(),
                                      [](const Individual& lhs, const Individual& rhs) {
                                          return lhs.fitness < rhs.fitness;
                                      });
    best_fitness_ = strongest->fitness;
    ThreatCoefficients best_coefficients = strongest->coefficients;
    best_history_.push_back(best_fitness_);
    best_fitness = best_fitness_;

    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const int crossover_slots = static_cast<int>(config_.crossover_ratio *
                                                  (config_.population_size - 1));

    for (int generation = 1; generation <= config_.max_generations; ++generation) {
        std::vector<Individual> next;
        next.reserve(config_.population_size);

        int produced_by_crossover = 0;
        // One slot stays free for the incumbent, whose fitness is already known,
        // so evaluate() only ever scores the members this generation produced.
        while (static_cast<int>(next.size()) + 1 < config_.population_size) {
            std::vector<int> child;
            if (produced_by_crossover < crossover_slots) {
                const int lhs = select_parent_index();
                const int rhs = select_parent_index();
                child = crossover(population_[lhs].genes, population_[rhs].genes, 0.35, rng_);
                produced_by_crossover++;
            } else {
                child = population_[select_parent_index()].genes;
            }

            if (unit(rng_) < config_.mutation_rate) child = mutate(child);
            CoefficientVector::sanitize_vector(child);

            Individual member;
            member.genes = child;
            member.coefficients = CoefficientVector::from_vector(child);
            next.push_back(member);
        }

        evaluate(next);
        // Elitism: the incumbent is never lost to a bad recombination.
        next.push_back(*strongest);
        population_ = next;

        strongest = std::max_element(population_.begin(), population_.end(),
                                     [](const Individual& lhs, const Individual& rhs) {
                                         return lhs.fitness < rhs.fitness;
                                     });
        if (strongest->fitness > best_fitness_ + 1e-9) {
            best_fitness_ = strongest->fitness;
            best_coefficients = strongest->coefficients;
            generations_since_improvement_ = 0;
        } else {
            generations_since_improvement_++;
            if (generations_since_improvement_ >= config_.stagnation_generations) {
                converged_ = true;
            }
        }
        best_history_.push_back(best_fitness_);

        generations_run = generation;
        best_fitness = best_fitness_;
        if (converged_) break;
    }

    return best_coefficients;
}

}

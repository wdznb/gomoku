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
    // #104 的读数口：以前只有「hill climbing ran N iterations, M evaluations, best fitness X」
    // 这一行，逐次评估的分数、有没有被接受、评的是不是冠军本体，日志和 checkpoint 里都没有
    // —— 轮次189/191 那两张「183 次晋的是复制体」的表是拿 versions 指纹 + 日志正则在外面拼
    // 出来的，仪器不在树里、读数不可复算。这里把每一次评估（含起步那一评）交给一个观察者。
    // 观察者只在 run() 的那一根线程上被调用（优化器由调用方独占），所以没有锁也没有队列。
    struct EvaluationRecord {
        int step = 0;               // 本轮第几次「真的评了」；起步那一评是 0
        double fitness = 0.0;       // 这一次评估的分数本身，不是历史最好
        bool accepted = false;      // 严格改进才走进去（与 current_fitness + 1e-9 比）
        bool same_as_start = false; // 这套系数 == 这一轮起步的那套 ⇒ 候选就是冠军本体
        size_t gene_index = 0;      // 被 rescale 的那个坐标（起步那一评是 0，且别的行为不看它）
        double multiplier = 0.0;    // 乘了哪一档（起步那一评是 0）
    };
    using EvaluationObserver = std::function<void(const EvaluationRecord&)>;

    HillClimbingOptimizer(CoefficientFitness fitness, const HillClimbingConfig& config,
                          uint64_t seed);

    void set_evaluation_observer(EvaluationObserver observer) {
        observer_ = std::move(observer);
    }

    // Coordinate-wise greedy walk: rescale one coefficient, keep it if the score
    // rises. Cheaper per step than the genetic search, and it only ever moves
    // away from a set that the evaluation function already likes.
    CoefficientPair run(const CoefficientPair& base,
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
    EvaluationObserver observer_;
};

}

#endif

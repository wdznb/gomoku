#ifndef GOMOKU_TRAINING_PARAMS_H
#define GOMOKU_TRAINING_PARAMS_H

#include "../core/evaluator.h"
#include <string>
#include <vector>

namespace gomoku {

// Coefficients the trainer is allowed to move.
//
// `five` is deliberately excluded: AlphaBeta compares raw scores against a
// hard-coded 100000 to recognise forced wins, and Evaluator caps static
// evaluation at five - 1. Retuning `five` would make those two disagree and
// turn mate detection into a coin flip. `forbidden_penalty` is excluded for a
// different reason - it encodes a rule of the game, not playing strength.
enum class TunableParam {
    LIVE_FOUR,
    RUSH_FOUR,
    LIVE_THREE,
    SLEEPY_THREE,
    LIVE_TWO,
    SLEEPY_TWO,
    DOUBLE_FOUR,
    DOUBLE_THREE,
    FOUR_THREE,
    POSITION_WEIGHT_SCALE,
    COUNT
};

constexpr int TUNABLE_COUNT = static_cast<int>(TunableParam::COUNT);

struct ParamBound {
    int min_value;
    int max_value;
};

// Threat coefficients viewed as a fixed-length integer vector, which is what
// the population-based and local-search optimizers mutate.
class CoefficientVector {
public:
    static std::vector<int> to_vector(const ThreatCoefficients& coeffs);
    static ThreatCoefficients from_vector(const std::vector<int>& values);

    // Clamp every tunable to its bound and re-establish the threat hierarchy:
    // a weaker shape may never outscore a stronger one, and the combination
    // bonuses may never fall below a bare live four. Idempotent.
    static void sanitize(ThreatCoefficients& coeffs);
    static void sanitize_vector(std::vector<int>& values);

    static ParamBound bound_for(TunableParam param);

    // JSON text form, so a coefficient set can be embedded in checkpoints as well
    // as written on its own.
    static std::string to_json_string(const ThreatCoefficients& coeffs);
    static bool from_json_string(const std::string& text, ThreatCoefficients& out,
                                 std::string& error);

    static bool load(const std::string& path, ThreatCoefficients& out, std::string& error);
    static bool save(const std::string& path, const ThreatCoefficients& coeffs, std::string& error);
};

}

#endif

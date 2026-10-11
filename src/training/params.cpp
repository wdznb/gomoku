#include "params.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>

using json = nlohmann::json;

namespace gomoku {

namespace {

// Bounds bracket the hand-tuned defaults by roughly a factor of four in each
// direction. Wide enough that the search can actually find better ratios, narrow
// enough that no tunable can cross the 100000 mate threshold on its own.
ParamBound default_bound_for(TunableParam param) {
    switch (param) {
        case TunableParam::LIVE_FOUR:             return {1500, 40000};
        case TunableParam::RUSH_FOUR:             return {300, 15000};
        case TunableParam::LIVE_THREE:            return {150, 8000};
        case TunableParam::SLEEPY_THREE:          return {30, 2000};
        case TunableParam::LIVE_TWO:              return {10, 1000};
        case TunableParam::SLEEPY_TWO:            return {1, 500};
        case TunableParam::DOUBLE_FOUR:           return {10000, 90000};
        case TunableParam::DOUBLE_THREE:          return {5000, 90000};
        case TunableParam::FOUR_THREE:            return {5000, 90000};
        case TunableParam::POSITION_WEIGHT_SCALE: return {0, 20};
        default:                                  return {1, 100};
    }
}

int& field_of(ThreatCoefficients& coeffs, TunableParam param) {
    switch (param) {
        case TunableParam::LIVE_FOUR:             return coeffs.live_four;
        case TunableParam::RUSH_FOUR:             return coeffs.rush_four;
        case TunableParam::LIVE_THREE:            return coeffs.live_three;
        case TunableParam::SLEEPY_THREE:          return coeffs.sleepy_three;
        case TunableParam::LIVE_TWO:              return coeffs.live_two;
        case TunableParam::SLEEPY_TWO:            return coeffs.sleepy_two;
        case TunableParam::DOUBLE_FOUR:           return coeffs.double_four;
        case TunableParam::DOUBLE_THREE:          return coeffs.double_three;
        case TunableParam::FOUR_THREE:            return coeffs.four_three;
        case TunableParam::POSITION_WEIGHT_SCALE: return coeffs.position_weight_scale;
        default:                                  return coeffs.position_weight_scale;
    }
}

}

std::vector<int> CoefficientVector::to_vector(const ThreatCoefficients& coeffs) {
    ThreatCoefficients copy = coeffs;
    std::vector<int> values;
    values.reserve(TUNABLE_COUNT);
    for (int i = 0; i < TUNABLE_COUNT; ++i) {
        values.push_back(field_of(copy, static_cast<TunableParam>(i)));
    }
    return values;
}

ThreatCoefficients CoefficientVector::from_vector(const std::vector<int>& values) {
    ThreatCoefficients coeffs;
    for (int i = 0; i < TUNABLE_COUNT && i < static_cast<int>(values.size()); ++i) {
        field_of(coeffs, static_cast<TunableParam>(i)) = values[i];
    }
    sanitize(coeffs);
    return coeffs;
}

void CoefficientVector::sanitize(ThreatCoefficients& coeffs) {
    for (int i = 0; i < TUNABLE_COUNT; ++i) {
        TunableParam param = static_cast<TunableParam>(i);
        ParamBound bound = default_bound_for(param);
        int& value = field_of(coeffs, param);
        value = std::max(bound.min_value, std::min(bound.max_value, value));
    }

    // A weaker threat must never outscores a stronger one, or move ordering
    // picks a live two over a live four. Repairing by capping the weaker entry
    // keeps the stronger ones (and their bounds) untouched.
    TunableParam ladder[] = {TunableParam::LIVE_FOUR, TunableParam::RUSH_FOUR,
                             TunableParam::LIVE_THREE, TunableParam::SLEEPY_THREE,
                             TunableParam::LIVE_TWO, TunableParam::SLEEPY_TWO};
    for (int i = 1; i < static_cast<int>(sizeof(ladder) / sizeof(ladder[0])); ++i) {
        int& weaker = field_of(coeffs, ladder[i]);
        const int stronger = field_of(coeffs, ladder[i - 1]);
        weaker = std::min(weaker, stronger);
    }

    // A combination of two threats is worth at least as much as the single best
    // one it contains.
    const int live_four = coeffs.live_four;
    coeffs.double_four = std::max(coeffs.double_four, live_four);
    coeffs.double_three = std::max(coeffs.double_three, live_four);
    coeffs.four_three = std::max(coeffs.four_three, live_four);
}

void CoefficientVector::sanitize_vector(std::vector<int>& values) {
    // Route through the struct so the vector and struct views can never drift
    // apart on what a legal parameter set means.
    values = to_vector(from_vector(values));
}

ParamBound CoefficientVector::bound_for(TunableParam param) {
    return default_bound_for(param);
}

std::string CoefficientVector::to_json_string(const ThreatCoefficients& coeffs) {
    json root;
    root["coefficients"] = {
        {"five", coeffs.five},
        {"live_four", coeffs.live_four},
        {"rush_four", coeffs.rush_four},
        {"live_three", coeffs.live_three},
        {"sleepy_three", coeffs.sleepy_three},
        {"live_two", coeffs.live_two},
        {"sleepy_two", coeffs.sleepy_two},
        {"double_four", coeffs.double_four},
        {"double_three", coeffs.double_three},
        {"four_three", coeffs.four_three},
        {"forbidden_penalty", coeffs.forbidden_penalty},
        {"position_weight_scale", coeffs.position_weight_scale},
    };
    return root.dump();
}

bool CoefficientVector::from_json_string(const std::string& text, ThreatCoefficients& out,
                                         std::string& error) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::exception& e) {
        error = std::string("coefficient JSON is not valid: ") + e.what();
        return false;
    }
    if (!root.is_object()) {
        error = "coefficient JSON must be an object";
        return false;
    }

    const json* body = &root;
    if (root.contains("coefficients") && root["coefficients"].is_object()) {
        body = &root["coefficients"];
    }

    ThreatCoefficients coeffs;
    static const char* names[] = {"five", "live_four", "rush_four", "live_three",
                                  "sleepy_three", "live_two", "sleepy_two",
                                  "double_four", "double_three", "four_three",
                                  "forbidden_penalty", "position_weight_scale"};
    int* fields[] = {&coeffs.five, &coeffs.live_four, &coeffs.rush_four, &coeffs.live_three,
                     &coeffs.sleepy_three, &coeffs.live_two, &coeffs.sleepy_two,
                     &coeffs.double_four, &coeffs.double_three, &coeffs.four_three,
                     &coeffs.forbidden_penalty, &coeffs.position_weight_scale};

    for (int i = 0; i < 12; ++i) {
        if (!body->contains(names[i])) continue;
        const json& value = (*body)[names[i]];
        if (!value.is_number_integer()) {
            error = std::string(names[i]) + " must be an integer";
            return false;
        }
        *fields[i] = value.get<int>();
    }

    // `five` is load-bearing for mate detection, so a file that moves it above the
    // threshold is rejected rather than silently applied.
    if (coeffs.five > 100000) {
        error = "five must not exceed the 100000 mate threshold";
        return false;
    }

    sanitize(coeffs);
    out = coeffs;
    return true;
}

bool CoefficientVector::load(const std::string& path, ThreatCoefficients& out, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return from_json_string(buffer.str(), out, error);
}

bool CoefficientVector::save(const std::string& path, const ThreatCoefficients& coeffs,
                             std::string& error) {
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        error = "cannot write " + path;
        return false;
    }
    file << json::parse(to_json_string(coeffs)).dump(2);
    if (!file.good()) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

}

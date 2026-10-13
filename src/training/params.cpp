#include "params.h"
#include "json_int.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
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

    // A present `coefficients` that is not an object is a malformed file, not an
    // absent one: falling through to the root would find none of the twelve names
    // there and report a successful load of nothing but the defaults.
    const json* body = &root;
    if (root.contains("coefficients")) {
        if (!root["coefficients"].is_object()) {
            error = "coefficients must be an object";
            return false;
        }
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

    int matched = 0;
    // The keys the file states itself; the cross-key checks below refuse to
    // rewrite a value the file wrote, never a default (present[] follows names[]).
    bool present[12] = {};
    for (int i = 0; i < 12; ++i) {
        if (!body->contains(names[i])) continue;
        ++matched;
        present[i] = true;
        const json& value = (*body)[names[i]];
        if (!value.is_number_integer()) {
            error = std::string(names[i]) + " must be an integer";
            return false;
        }
        // Cast-not-throw: an integer wider than 32 bits must be rejected, because
        // the truncated low half would be a value the file never stated.
        int parsed = 0;
        if (!read_json_int(value, parsed)) {
            error = std::string(names[i]) + " is outside the range of a 32-bit integer";
            return false;
        }
        *fields[i] = parsed;
    }

    // A file that names none of the twelve is not a subset of the defaults, it is
    // the wrong file; loading it would report success while training nothing.
    if (matched == 0) {
        error = "no known coefficient key found in the file";
        return false;
    }
    // An unrecognised key is a typo (live_fourx) that would otherwise be silently
    // ignored while the real coefficient kept its default.
    for (auto it = body->begin(); it != body->end(); ++it) {
        bool known = false;
        for (int i = 0; i < 12; ++i) {
            if (it.key() == names[i]) {
                known = true;
                break;
            }
        }
        if (!known) {
            error = "unknown coefficient key: " + it.key();
            return false;
        }
    }

    // `five` is load-bearing for mate detection, so a file that moves it above the
    // threshold is rejected rather than silently applied.
    if (coeffs.five > 100000) {
        error = "five must not exceed the 100000 mate threshold";
        return false;
    }
    // The evaluator caps ordinary scores at five - 1, so a non-positive five
    // flattens them or turns the whole static evaluation negative.
    if (coeffs.five <= 0) {
        error = "five must stay above zero or the evaluation scale collapses";
        return false;
    }
    // forbidden_penalty is read only by evaluate_position_both, and no production
    // path calls that entry. The range guard stays because a params file can write
    // zero or a negative here, which turns a forbidden move into a free one.
    if (coeffs.forbidden_penalty <= 0 || coeffs.forbidden_penalty >= 100000) {
        error = "forbidden_penalty must stay between zero and the 100000 mate threshold";
        return false;
    }

    // The file is an instruction, not a suggestion: sanitize() would clamp an
    // out-of-range tunable into its bound and the loader would still report
    // success (README:155 wants a refusal). The clamping stays for the
    // optimizers, whose own moves are always generated inside the bounds.
    static const char* tunable_names[] = {"live_four", "rush_four", "live_three",
                                          "sleepy_three", "live_two", "sleepy_two",
                                          "double_four", "double_three", "four_three",
                                          "position_weight_scale"};
    for (int i = 0; i < TUNABLE_COUNT; ++i) {
        TunableParam param = static_cast<TunableParam>(i);
        ParamBound bound = default_bound_for(param);
        const int value = field_of(coeffs, param);
        if (value < bound.min_value || value > bound.max_value) {
            error = std::string(tunable_names[i]) + " " + std::to_string(value) +
                    " is outside the allowed range [" + std::to_string(bound.min_value) +
                    ", " + std::to_string(bound.max_value) + "]";
            return false;
        }
    }

    // Files state these outright; sanitize()'s repairs are for optimizer
    // candidates. Only a value the file actually wrote can be "rewritten": a
    // relation whose other end was never named is left to sanitize(), which is
    // what keeps a partial file legal.
    const bool wrote_live_four = present[1];
    const bool wrote_rush_four = present[2];
    const bool wrote_live_three = present[3];
    const bool wrote_sleepy_three = present[4];
    const bool wrote_live_two = present[5];
    const bool wrote_sleepy_two = present[6];
    const bool wrote_double_four = present[7];
    const bool wrote_double_three = present[8];
    const bool wrote_four_three = present[9];
    if (coeffs.rush_four > coeffs.live_four && wrote_rush_four) {
        error = std::string("rush_four ") + std::to_string(coeffs.rush_four) +
                " must not exceed live_four " + std::to_string(coeffs.live_four) +
                (wrote_live_four ? "" : "（对端是默认值）");
        return false;
    }
    if (coeffs.live_three > coeffs.rush_four && wrote_live_three) {
        error = std::string("live_three ") + std::to_string(coeffs.live_three) +
                " must not exceed rush_four " + std::to_string(coeffs.rush_four) +
                (wrote_rush_four ? "" : "（对端是默认值）");
        return false;
    }
    if (coeffs.sleepy_three > coeffs.live_three && wrote_sleepy_three) {
        error = std::string("sleepy_three ") + std::to_string(coeffs.sleepy_three) +
                " must not exceed live_three " + std::to_string(coeffs.live_three) +
                (wrote_live_three ? "" : "（对端是默认值）");
        return false;
    }
    if (coeffs.live_two > coeffs.sleepy_three && wrote_live_two) {
        error = std::string("live_two ") + std::to_string(coeffs.live_two) +
                " must not exceed sleepy_three " + std::to_string(coeffs.sleepy_three) +
                (wrote_sleepy_three ? "" : "（对端是默认值）");
        return false;
    }
    if (coeffs.sleepy_two > coeffs.live_two && wrote_sleepy_two) {
        error = std::string("sleepy_two ") + std::to_string(coeffs.sleepy_two) +
                " must not exceed live_two " + std::to_string(coeffs.live_two) +
                (wrote_live_two ? "" : "（对端是默认值）");
        return false;
    }
    if (coeffs.double_four < coeffs.live_four && wrote_double_four) {
        error = std::string("double_four ") + std::to_string(coeffs.double_four) +
                " must be at least live_four " + std::to_string(coeffs.live_four) +
                (wrote_live_four ? "" : "（对端是默认值）");
        return false;
    }
    if (coeffs.double_three < coeffs.live_four && wrote_double_three) {
        error = std::string("double_three ") + std::to_string(coeffs.double_three) +
                " must be at least live_four " + std::to_string(coeffs.live_four) +
                (wrote_live_four ? "" : "（对端是默认值）");
        return false;
    }
    if (coeffs.four_three < coeffs.live_four && wrote_four_three) {
        error = std::string("four_three ") + std::to_string(coeffs.four_three) +
                " must be at least live_four " + std::to_string(coeffs.live_four) +
                (wrote_live_four ? "" : "（对端是默认值）");
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
    // Write-then-rename, mirroring save_session: the trained parameters are
    // only replaced once the whole new file is on disk, so an interrupted
    // write cannot leave a half-written params file behind.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file.is_open()) {
            error = "cannot write " + tmp;
            return false;
        }
        file << json::parse(to_json_string(coeffs)).dump(2);
        // The rename may only replace a good file once these bytes left the
        // stream buffer: an error that surfaces at close would be reported as a
        // successful save of a truncated parameter file.
        file.flush();
        if (!file.good()) {
            error = "failed while writing " + tmp;
            std::error_code remove_ec;
            std::filesystem::remove(tmp, remove_ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
        error = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

}

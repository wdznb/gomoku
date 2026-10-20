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

// One row per key a parameter file may name, and the only place a key's name,
// its field, its tunable gene and its range are stated. The five parallel
// surfaces this replaced (the writer's key list, the reader's names[]/fields[]/
// present[], the bound switch, the field_of switch) had to be edited in step or
// drift silently: field_of's `default:` aliased an unlisted gene onto
// position_weight_scale, so a parameter added to the enum but not to the switch
// was trainable, savable under the wrong name, and unloadable.
//
// Bounds bracket the hand-tuned defaults by roughly a factor of four in each
// direction. Wide enough that the search can actually find better ratios, narrow
// enough that no tunable can cross the 100000 mate threshold on its own.
// edge_penalty's ceiling is set by a structural promise rather than by that rule
// of thumb: eight closed ends (four directions, two ends each) at the largest
// legal value still cost less than the cheapest legal 活四, so the discount
// cannot push a 活四 below a 眠三 for any parameter set, trained or hand-tuned.
// Against 冲四 the same promise holds only at the calibrated defaults, where a
// file may not train rush_four down to its 300 floor.
//
// The two rule constants carry the range their explicit guards below enforce;
// tunable == TunableParam::COUNT is what keeps them out of the vector view.
struct ParamSpec {
    const char* name;
    int ThreatCoefficients::*field;
    TunableParam tunable;
    ParamBound bound;
};

constexpr ParamSpec kParams[] = {
    {"five", &ThreatCoefficients::five, TunableParam::COUNT, {2, 100000}},
    {"live_four", &ThreatCoefficients::live_four, TunableParam::LIVE_FOUR, {1500, 40000}},
    {"rush_four", &ThreatCoefficients::rush_four, TunableParam::RUSH_FOUR, {300, 15000}},
    {"live_three", &ThreatCoefficients::live_three, TunableParam::LIVE_THREE, {150, 8000}},
    {"sleepy_three", &ThreatCoefficients::sleepy_three, TunableParam::SLEEPY_THREE, {30, 2000}},
    {"live_two", &ThreatCoefficients::live_two, TunableParam::LIVE_TWO, {10, 1000}},
    {"sleepy_two", &ThreatCoefficients::sleepy_two, TunableParam::SLEEPY_TWO, {1, 500}},
    {"double_four", &ThreatCoefficients::double_four, TunableParam::DOUBLE_FOUR, {10000, 90000}},
    {"double_three", &ThreatCoefficients::double_three, TunableParam::DOUBLE_THREE, {5000, 90000}},
    {"four_three", &ThreatCoefficients::four_three, TunableParam::FOUR_THREE, {5000, 90000}},
    {"forbidden_penalty", &ThreatCoefficients::forbidden_penalty, TunableParam::COUNT, {1, 99999}},
    {"position_weight_scale", &ThreatCoefficients::position_weight_scale,
     TunableParam::POSITION_WEIGHT_SCALE, {0, 20}},
    {"distance_to_stones", &ThreatCoefficients::distance_to_stones,
     TunableParam::DISTANCE_TO_STONES, {0, 4}},
    {"edge_penalty", &ThreatCoefficients::edge_penalty, TunableParam::EDGE_PENALTY, {0, 150}},
    {"cand_width_root", &ThreatCoefficients::cand_width_root, TunableParam::CAND_WIDTH_ROOT,
     {2, 64}},
    {"cand_width_deep", &ThreatCoefficients::cand_width_deep, TunableParam::CAND_WIDTH_DEEP,
     {2, 32}},
};

constexpr int kParamCount = static_cast<int>(sizeof(kParams) / sizeof(kParams[0]));

// Index loops rather than range-for: these run during constant evaluation, and
// an older toolchain's constexpr support stops short of the relaxation that
// allows range-for there.
constexpr int rows_for_gene(int gene) {
    int found = 0;
    for (int i = 0; i < kParamCount; ++i) {
        if (static_cast<int>(kParams[i].tunable) == gene) ++found;
    }
    return found;
}

constexpr bool tunable_table_complete() {
    for (int gene = 0; gene < static_cast<int>(TunableParam::COUNT); ++gene) {
        if (rows_for_gene(gene) != 1) return false;
    }
    return true;
}

static_assert(tunable_table_complete(),
              "every TunableParam needs exactly one row in kParams");

const ParamSpec& spec_for(TunableParam param) {
    for (const ParamSpec& spec : kParams) {
        if (spec.tunable == param) return spec;
    }
    // Unreachable: the static_assert above proves every gene has a row.
    return kParams[0];
}

ParamBound default_bound_for(TunableParam param) {
    return spec_for(param).bound;
}

int& field_of(ThreatCoefficients& coeffs, TunableParam param) {
    return coeffs.*(spec_for(param).field);
}

const char* name_of(TunableParam param) {
    return spec_for(param).name;
}

}

std::vector<int> CoefficientVector::to_vector(const ThreatCoefficients& coeffs) {
    std::vector<int> values;
    values.reserve(TUNABLE_COUNT);
    for (int i = 0; i < TUNABLE_COUNT; ++i) {
        values.push_back(coeffs.*(spec_for(static_cast<TunableParam>(i)).field));
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

    // A deeper layer may not widen past the root: the root sees the whole
    // neighbourhood once, and every ply below it pays for that width again. Capped
    // from the top so a trained root width of 4 pulls the whole ladder with it.
    coeffs.cand_width_deep = std::min(coeffs.cand_width_deep, coeffs.cand_width_root);
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
    json body;
    for (const ParamSpec& spec : kParams) {
        body[spec.name] = coeffs.*(spec.field);
    }
    json root;
    root["coefficients"] = std::move(body);
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
    // absent one: falling through to the root would find none of the names kParams
    // states and report a successful load of nothing but the defaults.
    const json* body = &root;
    if (root.contains("coefficients")) {
        if (!root["coefficients"].is_object()) {
            error = "coefficients must be an object";
            return false;
        }
        body = &root["coefficients"];
    }

    ThreatCoefficients coeffs;
    int matched = 0;
    // The keys the file states itself; the cross-key checks below refuse to
    // rewrite a value the file wrote, never a default (present[] follows kParams).
    bool present[kParamCount] = {};
    for (int i = 0; i < kParamCount; ++i) {
        const ParamSpec& spec = kParams[i];
        if (!body->contains(spec.name)) continue;
        ++matched;
        present[i] = true;
        const json& value = (*body)[spec.name];
        if (!value.is_number_integer()) {
            error = std::string(spec.name) + " must be an integer";
            return false;
        }
        // Cast-not-throw: an integer wider than 32 bits must be rejected, because
        // the truncated low half would be a value the file never stated.
        int parsed = 0;
        if (!read_json_int(value, parsed)) {
            error = std::string(spec.name) + " is outside the range of a 32-bit integer";
            return false;
        }
        coeffs.*(spec.field) = parsed;
    }
    // Did the file state this gene's value, or is it the compiled-in default?
    auto stated = [&](TunableParam param) {
        for (int i = 0; i < kParamCount; ++i) {
            if (kParams[i].tunable == param) return present[i];
        }
        return false;
    };

    // A file that names none of them is not a subset of the defaults, it is
    // the wrong file; loading it would report success while training nothing.
    if (matched == 0) {
        error = "no known coefficient key found in the file";
        return false;
    }
    // An unrecognised key is a typo (live_fourx) that would otherwise be silently
    // ignored while the real coefficient kept its default.
    for (auto it = body->begin(); it != body->end(); ++it) {
        bool known = false;
        for (const ParamSpec& spec : kParams) {
            if (it.key() == spec.name) {
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
    // The evaluator caps ordinary scores at five - 1, so that cap must stay
    // positive: at five == 1 every board flattens to zero, and a non-positive five
    // turns the whole static evaluation negative.
    if (coeffs.five <= 1) {
        error = "five must stay above 1 or the evaluation scale collapses";
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
    for (const ParamSpec& spec : kParams) {
        if (spec.tunable == TunableParam::COUNT) continue;
        const int value = coeffs.*(spec.field);
        if (value < spec.bound.min_value || value > spec.bound.max_value) {
            error = std::string(spec.name) + " " + std::to_string(value) +
                    " is outside the allowed range [" + std::to_string(spec.bound.min_value) +
                    ", " + std::to_string(spec.bound.max_value) + "]";
            return false;
        }
    }

    // Files state these outright; sanitize()'s repairs are for optimizer
    // candidates. Only a value the file actually wrote can be "rewritten": a
    // relation whose other end was never named is left to sanitize(), which is
    // what keeps a partial file legal. `must_not_exceed` picks which end
    // sanitize() moves - the ladder caps the weaker threat, the combination
    // floors raise the pair, and the width cap lowers the deeper layer.
    struct CrossRule {
        TunableParam asked;
        TunableParam against;
        bool must_not_exceed;
    };
    const CrossRule rules[] = {
        {TunableParam::RUSH_FOUR, TunableParam::LIVE_FOUR, true},
        {TunableParam::LIVE_THREE, TunableParam::RUSH_FOUR, true},
        {TunableParam::SLEEPY_THREE, TunableParam::LIVE_THREE, true},
        {TunableParam::LIVE_TWO, TunableParam::SLEEPY_THREE, true},
        {TunableParam::SLEEPY_TWO, TunableParam::LIVE_TWO, true},
        {TunableParam::DOUBLE_FOUR, TunableParam::LIVE_FOUR, false},
        {TunableParam::DOUBLE_THREE, TunableParam::LIVE_FOUR, false},
        {TunableParam::FOUR_THREE, TunableParam::LIVE_FOUR, false},
        {TunableParam::CAND_WIDTH_DEEP, TunableParam::CAND_WIDTH_ROOT, true},
    };
    for (const CrossRule& rule : rules) {
        if (!stated(rule.asked)) continue;
        const int asked = field_of(coeffs, rule.asked);
        const int other = field_of(coeffs, rule.against);
        if (rule.must_not_exceed ? (asked <= other) : (asked >= other)) continue;
        error = std::string(name_of(rule.asked)) + " " + std::to_string(asked) +
                (rule.must_not_exceed ? " must not exceed " : " must be at least ") +
                name_of(rule.against) + " " + std::to_string(other) +
                (stated(rule.against) ? "" : "（对端是默认值）");
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

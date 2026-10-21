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

// The genes whose two sides must stay the same shape of engine (指导书 §4 丁-3 注意 5):
// the threat values. The positional genes - centre preference, stone distance,
// edge discount and the two candidate widths - are what 丁-4's balance controller
// is allowed to move on one side alone, so they carry no cross-side band.
constexpr TunableParam kThreatGenes[] = {
    TunableParam::LIVE_FOUR, TunableParam::RUSH_FOUR, TunableParam::LIVE_THREE,
    TunableParam::SLEEPY_THREE, TunableParam::LIVE_TWO, TunableParam::SLEEPY_TWO,
    TunableParam::DOUBLE_FOUR, TunableParam::DOUBLE_THREE, TunableParam::FOUR_THREE,
};

constexpr int kThreatGeneCount =
    static_cast<int>(sizeof(kThreatGenes) / sizeof(kThreatGenes[0]));

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

// The nearest value to `reference` that is within LADDER_MATCH_PERCENT of it.
// Returns `value` untouched when it already sits in the band, which is what makes
// this double as the reader's check: a file whose white ladder is out of band is
// one where `pull_into_band` moves a value the file stated outright.
int pull_into_band(int value, int reference) {
    if (reference <= 0) return value;
    const long long high = static_cast<long long>(reference) * (100 + LADDER_MATCH_PERCENT) / 100;
    const long long low = static_cast<long long>(reference) * (100 - LADDER_MATCH_PERCENT) / 100;
    if (value > high) return static_cast<int>(high);
    if (value < low) return static_cast<int>(low);
    return value;
}

// Pull every white threat gene into band with the black one, moving only the
// white side (black is the reference the pair is measured against).
void pull_white_into_band(CoefficientPair& pair) {
    for (int i = 0; i < kThreatGeneCount; ++i) {
        const TunableParam gene = kThreatGenes[i];
        int& value = field_of(pair.white, gene);
        value = pull_into_band(value, field_of(pair.black, gene));
    }
}

// Pull the white threat ladder into band with the black one, re-establish white's
// own hierarchy, and pull again. The two repairs feed each other: the band lowers
// a white combo below white's live four, and the hierarchy raises it back out of
// band. The loop runs to a fixed point and always ends on the band, so a pair that
// cannot converge leaves a legal-but-tightened white side rather than a silent
// band violation; the pass cap is there so that case stops instead of spinning.
void enforce_ladder_match(CoefficientPair& pair) {
    for (int pass = 0; pass < 8; ++pass) {
        std::vector<int> before = CoefficientVector::to_vector(pair.white);
        pull_white_into_band(pair);
        CoefficientVector::sanitize(pair.white);
        pull_white_into_band(pair);
        if (CoefficientVector::to_vector(pair.white) == before) return;
    }
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
    // A genome wider than one set is a pair, and the optimizers hand the same
    // helper both shapes: routing by length keeps a 28-gene candidate from being
    // sanitized as if its white half were black's.
    if (static_cast<int>(values.size()) > TUNABLE_COUNT) {
        sanitize_pair_vector(values);
        return;
    }
    // Route through the struct so the vector and struct views can never drift
    // apart on what a legal parameter set means.
    values = to_vector(from_vector(values));
}

std::vector<int> CoefficientVector::to_vector(const CoefficientPair& pair) {
    std::vector<int> values;
    values.reserve(PAIR_GENE_COUNT);
    for (int side = 0; side < 2; ++side) {
        const std::vector<int> one_side =
            to_vector(pair.for_color(side == 0 ? BLACK : WHITE));
        values.insert(values.end(), one_side.begin(), one_side.end());
    }
    return values;
}

CoefficientPair CoefficientVector::pair_from_vector(const std::vector<int>& values) {
    // A flat genome is a shared pair: the shape every old checkpoint, every
    // hand-written parameter set and every test that names 14 genes already uses.
    if (static_cast<int>(values.size()) <= TUNABLE_COUNT) {
        return CoefficientPair::shared(from_vector(values));
    }
    // A short first half fills the rest from the defaults, exactly as
    // from_vector does; a short second half does the same for the white side.
    const int white_end = std::min(static_cast<int>(values.size()), PAIR_GENE_COUNT);
    const std::vector<int> black_genes(values.begin(), values.begin() + TUNABLE_COUNT);
    const std::vector<int> white_genes(values.begin() + TUNABLE_COUNT,
                                       values.begin() + white_end);
    CoefficientPair pair;
    pair.black = from_vector(black_genes);
    pair.white = from_vector(white_genes);
    enforce_ladder_match(pair);
    return pair;
}

void CoefficientVector::sanitize(CoefficientPair& pair) {
    sanitize(pair.black);
    sanitize(pair.white);
    enforce_ladder_match(pair);
}

void CoefficientVector::sanitize_pair_vector(std::vector<int>& values) {
    // The width a caller handed in is the width it gets back: a flat genome stays
    // flat (its two sides are the same set), so a 14-gene caller never finds its
    // vector grown past the genes it named.
    const bool wide = static_cast<int>(values.size()) > TUNABLE_COUNT;
    const CoefficientPair pair = pair_from_vector(values);
    values = wide ? to_vector(pair) : to_vector(pair.black);
}

ParamBound CoefficientVector::bound_for_gene(int gene) {
    if (gene < 0 || gene >= PAIR_GENE_COUNT) return ParamBound{0, 0};
    return default_bound_for(static_cast<TunableParam>(gene % TUNABLE_COUNT));
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

std::string CoefficientVector::to_json_string(const CoefficientPair& pair) {
    json body;
    for (const ParamSpec& spec : kParams) {
        body[spec.name] = pair.black.*(spec.field);
    }
    // The shared pair writes today's bytes: the second sub-object appears only
    // when there is a second, different set to state.
    if (!pair.identical()) {
        json white_body;
        for (const ParamSpec& spec : kParams) {
            white_body[spec.name] = pair.white.*(spec.field);
        }
        body["white"] = std::move(white_body);
    }
    json root;
    root["coefficients"] = std::move(body);
    return root.dump();
}

bool CoefficientVector::pair_from_json_string(const std::string& text, CoefficientPair& out,
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
    if (root.contains("coefficients")) {
        if (!root["coefficients"].is_object()) {
            error = "coefficients must be an object";
            return false;
        }
        body = &root["coefficients"];
    }
    if (!body->contains("white")) {
        ThreatCoefficients shared_coeffs;
        if (!from_json_string(text, shared_coeffs, error)) return false;
        out = CoefficientPair::shared(shared_coeffs);
        return true;
    }
    if (!(*body)["white"].is_object()) {
        error = "white must be an object";
        return false;
    }

    // Each half goes through the single-set reader unchanged, so every range,
    // every cross-key relation and every unknown-key refusal is checked once per
    // colour rather than re-implemented here (指导书 §4 丁-3 注意 4).
    json black_body = *body;
    black_body.erase("white");
    ThreatCoefficients black_coeffs, white_coeffs;
    if (!from_json_string(black_body.dump(), black_coeffs, error)) {
        error = "black side: " + error;
        return false;
    }
    if (!from_json_string((*body)["white"].dump(), white_coeffs, error)) {
        error = "white side: " + error;
        return false;
    }

    // Rule constants are not genes, and a per-colour rule is not a strength
    // difference: `five` is the threshold AlphaBeta compares raw scores against and
    // the evaluation cap is taken from it, so two colours holding two different
    // values would put two mate thresholds inside one search.
    if (black_coeffs.five != white_coeffs.five) {
        error = "five must be the same for both colours";
        return false;
    }
    if (black_coeffs.forbidden_penalty != white_coeffs.forbidden_penalty) {
        error = "forbidden_penalty must be the same for both colours";
        return false;
    }

    CoefficientPair pair;
    pair.black = black_coeffs;
    pair.white = white_coeffs;
    // A file is an instruction, so the cross-side band is a refusal here and a
    // repair only in sanitize(), which the optimizers use.
    for (int i = 0; i < kThreatGeneCount; ++i) {
        const TunableParam gene = kThreatGenes[i];
        const int value = field_of(pair.white, gene);
        const int pulled = pull_into_band(value, field_of(pair.black, gene));
        if (pulled == value) continue;
        error = std::string("white ") + name_of(gene) + " " + std::to_string(value) +
                " is more than " + std::to_string(LADDER_MATCH_PERCENT) +
                "% away from black " + std::to_string(pulled) +
                "（两侧必须是同一个引擎）";
        return false;
    }
    out = pair;
    return true;
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
    // A per-colour file is not a single-set file with a spare key in it. Reading
    // the black half and applying it to both colours would throw away the second
    // side's coefficients and report success, so name the condition instead.
    if (body->contains("white")) {
        error = "this file carries a second colour's coefficients; load it as a pair";
        return false;
    }
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

bool CoefficientVector::load(const std::string& path, CoefficientPair& out, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return pair_from_json_string(buffer.str(), out, error);
}

bool CoefficientVector::save(const std::string& path, const ThreatCoefficients& coeffs,
                             std::string& error) {
    // One set is the pair whose sides match, and the paired writer emits the flat
    // object in exactly that case - so this is the same file, byte for byte.
    return save(path, CoefficientPair::shared(coeffs), error);
}

bool CoefficientVector::save(const std::string& path, const CoefficientPair& pair,
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
        file << json::parse(to_json_string(pair)).dump(2);
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

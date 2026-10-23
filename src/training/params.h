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
    // Design doc 2.5.2: the centre preference and the edge discount should be a
    // learned preference, not a hard-coded one. Both scale a term of
    // evaluate_board / pattern_score, and neither is a threat value, so neither
    // joins the strength ladder sanitize() repairs.
    EDGE_PENALTY,
    DISTANCE_TO_STONES,
    // Design doc 2.5.1: the candidate width limit is a playing-strength knob too -
    // how much of the near-a-stone neighbourhood each layer is allowed to search.
    CAND_WIDTH_ROOT,
    CAND_WIDTH_DEEP,
    COUNT
};

constexpr int TUNABLE_COUNT = static_cast<int>(TunableParam::COUNT);
// 丁-3（v2 §3.4 S6）：一套系数有了第二个维度，基因表却仍是一基因一行——按色只在
// 向量与 JSON 这两层边界上展开。索引口径 `side * TUNABLE_COUNT + gene`，0..13 是黑方、
// 14..27 是白方；两侧同一套时前 14 个与改之前逐字节相同，后 14 个只是它的镜像。
constexpr int PAIR_GENE_COUNT = 2 * TUNABLE_COUNT;

struct ParamBound {
    int min_value;
    int max_value;
};

// How far one side's threat ladder may drift from the other's before the pair
// stops being the same engine wearing a different colour (指导书 §4 丁-3 注意 5).
constexpr int LADDER_MATCH_PERCENT = 20;

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

    // The same two views for a pair (丁-3). A 14-wide vector is a shared pair, so
    // the flat form an old file or an old checkpoint states is the one shape every
    // reader already accepts; 28 is a pair whose sides differ. `sanitize(pair)`
    // runs the single-set repair once per side and then the cross-side ladder
    // guard, which moves only the white side - black is the reference.
    static std::vector<int> to_vector(const CoefficientPair& pair);
    static CoefficientPair pair_from_vector(const std::vector<int>& values);
    static void sanitize(CoefficientPair& pair);
    static void sanitize_pair_vector(std::vector<int>& values);

    // Bound of a side-major gene index (0..27). The optimizers walk the wide
    // vector, so the bound lookup has to follow the same indexing.
    static ParamBound bound_for_gene(int gene);

    // Is this pair gene worth probing? BLACK's double_four / double_three have no
    // payout point: both pattern_score call sites for BLACK sit behind
    // is_forbidden_from_pattern (evaluator.cpp:192, :267/:270) and that gate is
    // exactly what reads has_three_three / has_four_four (pattern.cpp:594), so a
    // black pattern carrying either field is scored as nothing - with one exception
    // the gate itself shows: is_forbidden returns false the moment the pattern holds
    // an exact five (pattern.cpp:592), so a square where BLACK makes five *and* a
    // double-four at once does read the gene. Such a square wins the game outright,
    // which is why 轮次174 still measured the two as dead: measured gene-by-gene with
    // tools/gene_reach_probe (45 盘面 × 全空格 × 两色, 基线 4,718,908 逐字不动).
    // The optimizers therefore stop drawing them; they stay loadable, savable and
    // ladder-repaired (sanitize still raises them to live_four, and white is still
    // banded against black's value as the reference).
    static bool gene_is_perturbable(int gene);

    // Map an arbitrary draw onto a perturbable gene of a genome this wide, so a
    // picker keeps its draw count identical while never landing on a dead gene.
    // A genome that is not pair-wide (a shared 14-gene set feeds both colours, where
    // these two values *do* pay out for WHITE) is returned unchanged.
    static size_t perturbable_slot(size_t draw, size_t genome_width);

    static ParamBound bound_for(TunableParam param);

    // JSON text form, so a coefficient set can be embedded in checkpoints as well
    // as written on its own.
    static std::string to_json_string(const ThreatCoefficients& coeffs);
    static bool from_json_string(const std::string& text, ThreatCoefficients& out,
                                 std::string& error);

    // A pair's text form is the flat object it always was, with one addition: a
    // `"white"` sub-object that appears only when the two sides differ. So the
    // bytes a shared pair writes are today's bytes, an old file reads as a shared
    // pair, and a reader that only knows the flat keys still finds the black side
    // - which is why the single-set reader *refuses* a paired file instead of
    // quietly training one colour's coefficients for both.
    static std::string to_json_string(const CoefficientPair& pair);
    static bool pair_from_json_string(const std::string& text, CoefficientPair& out,
                                      std::string& error);

    static bool load(const std::string& path, ThreatCoefficients& out, std::string& error);
    static bool save(const std::string& path, const ThreatCoefficients& coeffs, std::string& error);
    static bool load(const std::string& path, CoefficientPair& out, std::string& error);
    static bool save(const std::string& path, const CoefficientPair& pair, std::string& error);
};

}

#endif

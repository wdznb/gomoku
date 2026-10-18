#include "move_generator.h"
#include "../core/pattern.h"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <vector>

namespace gomoku {

namespace {

// Everything a tier that must not be cut needs as a "keep" argument.
constexpr size_t kKeepAll = std::numeric_limits<size_t>::max();

// The square completes five for whoever plays it.
bool makes_five(const PositionPattern& pattern) {
    for (int d = 0; d < 4; ++d) {
        if (pattern.directions[d].type == PatternType::FIVE) return true;
    }
    return false;
}

// The square makes a shape the opponent has to answer: 成五, 活四, 冲四 or 活三.
// This is the filter the quiescence subtree has always used, and design doc
// 2.5.1's quiescence row keeps it as the only one ("仅强制着法"). It is also
// VCTSearch::generate_attacks' own membership test, and so the 「≥ 活三」 row of
// the threat probe below.
bool makes_forcing_threat(const PositionPattern& pattern) {
    for (int d = 0; d < 4; ++d) {
        const PatternType t = pattern.directions[d].type;
        if (t == PatternType::FIVE || t == PatternType::LIVE_FOUR ||
            t == PatternType::RUSH_FOUR || t == PatternType::LIVE_THREE) {
            return true;
        }
    }
    return false;
}

// The square makes a four: 成五, 活四 or 冲四, which is design doc 2.3.4's
// 「己方 ≥ 冲四」 and VCFSearch::generate_rush_fours' own membership test. Every
// four is also a forcing threat, so `makes_four` ⊂ `makes_forcing_threat`.
bool makes_four(const PositionPattern& pattern) {
    for (int d = 0; d < 4; ++d) {
        const PatternType t = pattern.directions[d].type;
        if (t == PatternType::FIVE || t == PatternType::LIVE_FOUR ||
            t == PatternType::RUSH_FOUR) {
            return true;
        }
    }
    return false;
}

// The square makes two threats at once (四四 / 四三 / 三三), which is the shape a
// single answer cannot stop. PositionPattern has already counted it, so promoting
// it to Tier1 - design doc 2.7's 「双威胁前置识别」 row - costs one branch here and
// buys the tactical point never being cut by a width.
bool makes_double_threat(const PositionPattern& pattern) {
    return pattern.has_four_four || pattern.has_four_three || pattern.has_three_three;
}

// Within `radius` Chebyshev steps of the stone just played. The last move is the
// place the game is being decided at, and a square next to it has a neighbour
// count of only 1 while still being the answer to it.
bool near_last_move(const Board& board, Position pos, int radius) {
    if (!board.has_last_move()) return false;
    const Position& last = board.last_move().pos;
    return std::abs(pos.row - last.row) <= radius && std::abs(pos.col - last.col) <= radius;
}

// Tier2's width for one layer (design doc 2.5.1's K column). The trained surface
// carries two knobs while the table has three K values, so the ply 1-2 row is the
// mean of the two - 16 at the defaults, which is the table's number.
int tier2_width(const ThreatCoefficients& coeffs, int ply) {
    if (ply <= 0) return coeffs.cand_width_root;
    if (ply <= 2) return (coeffs.cand_width_root + coeffs.cand_width_deep) / 2;
    return coeffs.cand_width_deep;
}

// Order one tier by the total triple and keep the first `keep` of it. Sorting
// before cutting is what makes the cut mean "the width kept the best of the tier"
// rather than "the width kept whatever the candidate list happened to start
// with", and the two keys under the score are what make the kept set a function of
// the position alone.
void order_and_cut(std::vector<Candidate>& tier, size_t keep) {
    std::stable_sort(tier.begin(), tier.end(), [](const Candidate& a, const Candidate& b) {
        return RootScore(a.move, a.score) > RootScore(b.move, b.score);
    });
    if (tier.size() > keep) tier.resize(keep);
}

}

std::vector<Move> CandidateSet::flatten() const {
    std::vector<Move> out;
    out.reserve(forced.size() + threats.size() + vicinity.size() + remote.size());
    for (const auto& entry : forced) out.push_back(entry.move);
    for (const auto& entry : threats) out.push_back(entry.move);
    for (const auto& entry : vicinity) out.push_back(entry.move);
    for (const auto& entry : remote) out.push_back(entry.move);
    return out;
}

ThreatProbe threat_probe(const Board& board, int color) {
    ThreatProbe probe;
    const int opp = opponent(color);

    for (const Position& pos : board.active_empty()) {
        if (!board.is_empty(pos)) continue;

        // One pass gives both sides' shapes at this square, which is what makes the
        // probe O(候选数) rather than two O(候选数) passes.
        PositionPattern mine, theirs;
        PatternRecognizer::recognize_position_both(board, pos.row, pos.col, color, mine, theirs);

        // A shape is a threat only where the side that would make it may legally
        // play it; that is the same test VCFSearch::generate_rush_fours and
        // VCTSearch::generate_attacks run before they admit a square, which is what
        // makes own_four / own_three answer exactly "is that phase's root list
        // non-empty".
        const bool mine_legal = !(color == BLACK &&
                                  PatternRecognizer::is_forbidden_from_pattern(mine));
        const bool theirs_legal = !(opp == BLACK &&
                                    PatternRecognizer::is_forbidden_from_pattern(theirs));

        if (mine_legal && !probe.own_four) {
            if (makes_four(mine)) {
                probe.own_four = true;
                probe.own_three = true;
            } else if (makes_forcing_threat(mine)) {
                probe.own_three = true;
            }
        }

        // opp_four is the doc's 「对方 ≥ 冲四（必须防）」 read on the *current* board.
        // Nothing in stage B gates a search on it: a reply we play can change what
        // the opponent is forbidden to play, so a false here is not proof that no
        // four-point exists one move later. Stage C's 「无威胁局面」 test is the
        // consumer the doc names.
        if (theirs_legal && !probe.opp_four) probe.opp_four = makes_four(theirs);
    }

    return probe;
}

void MoveGenerator::generate(const Board& board, int color, int ply, bool in_quiescence,
                             CandidateSet* out) const {
    *out = CandidateSet{};
    const ThreatCoefficients& coeffs = evaluator_.coefficients();

    for (const Position& pos : board.active_empty()) {
        // The candidate set is maintained by place_stone / undo_last_move, so an
        // occupied cell should never be in it; the search has always re-checked, and
        // a tier that trusted a stale list would search a square that has a stone on
        // it.
        if (!board.is_empty(pos)) continue;

        PositionPattern mine, theirs;
        PatternRecognizer::recognize_position_both(board, pos.row, pos.col, color, mine, theirs);

        // A shape BLACK may not legally play is not a candidate at all, whichever
        // tier it would have landed in. Read off the pattern above: this is the same
        // test is_forbidden() runs, without its second four-direction scan.
        if (color == BLACK && PatternRecognizer::is_forbidden_from_pattern(mine)) continue;

        if (in_quiescence) {
            // Quiescence is a race of forcing moves, so it prices a candidate by
            // what it does for the side to play. The opponent's shapes at this
            // square are what the next recursion of the subtree sees, not a reason
            // to cut it out of this one.
            if (makes_forcing_threat(mine)) {
                out->threats.push_back(
                    {Move(pos, color),
                     evaluator_.evaluate_position_with_pattern(board, pos.row, pos.col, mine,
                                                               color)});
            }
            continue;
        }

        // Both sides of the square, from the patterns the tier test already used.
        const int score =
            evaluator_.candidate_pair_value(mine, theirs, pos.row, pos.col, color);
        const Candidate candidate{Move(pos, color), score};

        // Tier0's second half: a square where the *opponent* would complete five is
        // the only answer to that threat, and design doc 2.5.1 lists it as a forced
        // point. It reads the same pattern the score above was taken from.
        if (makes_five(mine) || makes_five(theirs)) out->forced.push_back(candidate);
        else if (makes_forcing_threat(mine) || makes_double_threat(mine))
            out->threats.push_back(candidate);
        else if (near_last_move(board, pos, 2) || board.neighbor_count_at(pos) >= 2)
            out->vicinity.push_back(candidate);
        else
            out->remote.push_back(candidate);
    }

    if (in_quiescence) {
        order_and_cut(out->threats, kKeepAll);
        return;
    }

    // A position with no threat on it at all is the one the widths have to relax
    // for: there is nothing to answer, so the layer is choosing where to build, and
    // the choice needs the widest look at the board it can get (design doc 2.5.1's
    // 「无威胁时放宽」).
    const bool no_threats = out->forced.empty() && out->threats.empty();

    order_and_cut(out->forced, kKeepAll);
    order_and_cut(out->threats, kKeepAll);

    size_t width = static_cast<size_t>(tier2_width(coeffs, ply));
    if (no_threats) {
        width = std::max(width, static_cast<size_t>(coeffs.cand_width_root));
    }
    order_and_cut(out->vicinity, width);

    // Tier3's width (design doc 2.5.1's M column): the table gives it a number only
    // on the root row, and only for a position with no threat. 3.2 keeps it in
    // addition when nothing else survived, which is what stops a board of isolated
    // stones from handing a layer an empty list. The number itself is
    // cand_width_deep, whose default is the doc's literal 8.
    size_t remote_width = 0;
    if (no_threats && (ply <= 0 || out->vicinity.empty())) {
        remote_width = static_cast<size_t>(coeffs.cand_width_deep);
    }
    order_and_cut(out->remote, remote_width);
}

std::vector<Move> MoveGenerator::generate_moves(const Board& board, int color, int ply,
                                                bool in_quiescence) const {
    CandidateSet set;
    generate(board, color, ply, in_quiescence, &set);
    return set.flatten();
}

}

#ifndef GOMOKU_MOVE_GENERATOR_H
#define GOMOKU_MOVE_GENERATOR_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "search_limits.h"
#include "search_state.h"
#include <vector>

namespace gomoku {

// One candidate, and the value it was placed into its tier by. The tier test and
// the price read the same four-direction pass, because the ordering loop this
// replaces paid a recognition every time its comparator asked which of two
// candidates is better - O(n log n) times per node, on the same handful of squares.
// A generator that recognized twice would re-introduce the cost the tiering is
// meant to pay for.
//
// Design doc 3.2 names this ScoredMove; greedy.h already owns that name for the
// greedy layer's attack/defense pair, so the tier record is named for the set it
// belongs to.
struct Candidate {
    Move move;
    int score;
};

// The four tiers of design doc 2.5.1, each already ordered by the total triple
// (value, centre, cell index) and truncated to the width its layer is allowed.
// The tiers are separate vectors because they answer different questions: which
// squares a layer may not skip, and which squares it may.
struct CandidateSet {
    // Tier0: the squares that end the game - the ones that complete five for the
    // side to play, and the ones where the opponent would complete five, which are
    // the only answer to that threat. Never truncated: a layer that could not play
    // these would be a layer that loses on the spot.
    std::vector<Candidate> forced;
    // Tier1: the squares where the side to play makes 活四, 冲四 or 活三, or a double
    // threat (四四 / 四三 / 三三). Never truncated either - the width is what Tier2
    // and Tier3 are cut to, and cutting a tactical answer is the failure mode the
    // design doc's risk table calls out by name.
    std::vector<Candidate> threats;
    // Tier2: the squares that sit in the fighting area without making a threat
    // themselves: within two of the last stone, or with at least two stones already
    // within two of them. Kept down to the layer's width.
    std::vector<Candidate> vicinity;
    // Tier3: everything else the board still considers active. Kept only when the
    // position has no threat for either side at all - design doc 2.5.1 gives Tier3
    // a width only on the 「无威胁」 row - which is what stops a board of isolated
    // stones from handing a layer an empty list.
    std::vector<Candidate> remote;

    // The tiers in order, as the flat list the search iterates.
    std::vector<Move> flatten() const;
};

// Design doc 2.3.4's 「威胁探针」: the one O(候选数) pass that answers, before a think
// pays for a tactical search, whether this position has tactical work in it at all.
// The three facts are the doc's three bullets, read off the same four-direction
// pattern the tiers already use:
//   own_four   - a square where `color` makes 成五 / 活四 / 冲四 (「己方 ≥ 冲四」),
//   own_three  - a square where it makes 活三 or better (「或 ≥ 活三」),
//   opp_four   - a square where the *opponent* makes 成五 / 活四 / 冲四 (「对方 ≥ 冲四，
//                必须防」).
// Each half is filtered by what that side may legally play, because a shape a side
// is forbidden to make is not a threat it can make: `own_*` use the same
// forbidden test VCF and VCT apply to their own root lists, and `opp_four` the
// opponent's. `own_four` implies `own_three`.
//
// The pass runs over board.active_empty() - the domain VCFSearch and VCTSearch
// enumerate - which is what makes `own_four` answer exactly "is VCF's root list
// non-empty" and `own_three` exactly "is VCT's root list non-empty". A think that
// skips a phase on a false therefore cannot skip one that had a move to try; that
// equivalence is 战术不漏, and it is pinned as an invariant rather than hoped.
struct ThreatProbe {
    bool own_four = false;
    bool own_three = false;
    bool opp_four = false;
};

// The pass itself. `color` is the side to move; the probe is not a candidate list,
// so it does not depend on a ply or on a width.
ThreatProbe threat_probe(const Board& board, int color);

// Where the candidate squares of one layer come from (design doc 2.5.1, 3.2).
//
// This replaces the old "every active empty cell, no width" list, which made the
// branch factor of a midgame node 60-100 and is why a depth-6 layer could not be
// finished in the time a think gets.
//
// The tier of a square and the price it is ordered by both come off one
// four-direction pass, and design doc 2.6 step 2 says that pass must not be the
// per-node cost of every candidate: the shapes and prices this generator works
// out are kept in a CandidateCache it owns, and the search's ordering pass reads
// the same records rather than recognizing a second time.
class MoveGenerator {
public:
    explicit MoveGenerator(const Evaluator& evaluator)
        : evaluator_(evaluator), cache_(evaluator) {}

    // Fill `out` with the tiers of this position for `color` at distance `ply`
    // from the root of the think. `ply` picks the width: the root keeps
    // cand_width_root of Tier2, the two plies after it the mean of the two
    // configured widths, and anything deeper cand_width_deep.
    //
    // With in_quiescence only Tier1 is filled, with the forcing moves of the
    // subtree (成五 / 活四 / 冲四 / 活三) priced by what they do for the side to
    // play: a quiet leaf's value comes from the stand-pat, and the moves that can
    // change it are the ones that force an answer. `ply` is not used there,
    // because a quiescence subtree has no width.
    void generate(const Board& board, int color, int ply, bool in_quiescence,
                  CandidateSet* out) const;

    // generate() and then flatten().
    std::vector<Move> generate_moves(const Board& board, int color, int ply,
                                     bool in_quiescence) const;

    // What one square is worth to both sides at once: exactly
    // `evaluate_position_pair(board, row, col, colour)` for either colour, read
    // through the cache rather than by a fresh four-direction pass. This is the
    // price design doc 2.4.5 puts at ④, and the ordering pass is its only reader
    // that does not also need the shapes.
    int candidate_price(const Board& board, const Position& pos) const {
        return cache_.pair_score(board, pos.row, pos.col);
    }

    // Step 2's ledger, for the cases that want to say how much of a think's
    // candidate work was shared rather than redone.
    const CandidateCache& cache() const { return cache_; }

private:
    const Evaluator& evaluator_;
    // The tiers answer a question about the position, and a node looking at a
    // position does not change it, so `generate()` stays const and this is the
    // scratch state that lets it be: a record is reused while the four lines
    // through its square are unchanged, whatever board asked.
    mutable CandidateCache cache_;
};

}

#endif

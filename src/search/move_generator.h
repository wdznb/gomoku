#ifndef GOMOKU_MOVE_GENERATOR_H
#define GOMOKU_MOVE_GENERATOR_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "search_limits.h"
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

// Where the candidate squares of one layer come from (design doc 2.5.1, 3.2).
//
// This replaces the old "every active empty cell, no width" list, which made the
// branch factor of a midgame node 60-100 and is why a depth-6 layer could not be
// finished in the time a think gets.
class MoveGenerator {
public:
    explicit MoveGenerator(const Evaluator& evaluator) : evaluator_(evaluator) {}

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

private:
    const Evaluator& evaluator_;
};

}

#endif

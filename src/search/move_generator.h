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

// 轮次213 #118：自底向上的**稳定归并**，工作区由调用方常驻。它替换的是 `std::stable_sort`
// —— 后者每次调用自己借一块临时缓冲（实测一次中盘思考 77,681 次
// `operator new(std::nothrow)`，调用者是 `std::__detail::__get_temporary_buffer<T>`）。
// 这里的次序要求是「相等键保持原相对次序」，所以不能退成 `std::sort`（见
// `alphabeta.cpp` 的 tie 说明与 `order_and_cut` 头上的注释）：**换的是谁借缓冲，不是稳定性**。
// 等价性先在树外验过：4000 个「键值域 8、长度 0-59、大量相等键」的随机向量上与原
// `std::stable_sort` 逐项相同（`/tmp/r218/merge_sort_proto.cpp`）。
template <typename T, typename Less>
void stable_merge_in_place(std::vector<T>& data, std::vector<T>& scratch, Less less) {
    const size_t n = data.size();
    if (n < 2) return;
    if (scratch.size() < n) scratch.resize(n);  // 只有第一次碰到更长的输入才扩容
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t start = 0; start < n; start += 2 * width) {
            const size_t mid = std::min(start + width, n);
            const size_t end = std::min(start + 2 * width, n);
            size_t i = start;
            size_t j = mid;
            size_t k = start;
            while (i < mid && j < end) {
                // 相等时两边都不「更小」⇒ 取左边：稳定性就落在这个方向上。
                scratch[k++] = less(data[j], data[i]) ? data[j++] : data[i++];
            }
            while (i < mid) scratch[k++] = data[i++];
            while (j < end) scratch[k++] = data[j++];
            for (size_t p = start; p < end; ++p) data[p] = scratch[p];
        }
    }
}

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

    // 轮次213 #118：层内排序要一块**同生共死**的归并缓冲区。`std::stable_sort` 每次调用
    // 自己借临时缓冲（一次中盘思考实测 77,681 次 `operator new(std::nothrow)`，调用者是
    // `std::__detail::__get_temporary_buffer<Candidate>`），而这里的次序**必须**是稳定的
    // （相等分数留谁由位置决定，见 `alphabeta.cpp:113` 那条 tie 的说明），所以换的是
    // 「谁借缓冲」而不是「稳不稳定」。跟着这一层走，缓冲区容量就随同一块 scratch 活着。
    std::vector<Candidate> merge_scratch;

    // 只清四层的**长度**，容量留着复用 —— 赋一个空 `CandidateSet{}` 会把容量丢掉，
    // 于是每个节点都从零增长（实测每次 `generate()` 约 8.7 次分配）。`merge_scratch`
    // 不清：它只是工作区，长度没有语义。
    void clear_tiers() {
        forced.clear();
        threats.clear();
        vicinity.clear();
        remote.clear();
    }

    // The tiers in order, as the flat list the search iterates.
    std::vector<Move> flatten() const;
    // 同一个结果写进调用方的缓冲（`clear` 保容量）—— 每节点一次摊平不该配一次新分配。
    void flatten_into(std::vector<Move>& out) const;
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

    // The whole-board static score over the same records: design doc 2.6 step 3's
    // stand-pat entry. A node that asks this and then asks `generate()` recognises
    // each square once between the two, whichever order they come in, and the number
    // is the one `Evaluator::evaluate_board` returns for the same board.
    int board_score(const Board& board, int color) const {
        return cache_.board_score(board, color);
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

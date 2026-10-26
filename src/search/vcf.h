#ifndef GOMOKU_VCF_H
#define GOMOKU_VCF_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "../core/pattern.h"
#include "../optimization/transposition.h"
#include <array>
#include <atomic>
#include <chrono>
#include <utility>
#include <vector>

namespace gomoku {

enum class VCFResult {
    WIN,
    LOSS,
    UNKNOWN
};

enum class DefenseResult {
    SURVIVES,
    PROVEN_LOSS,
    UNKNOWN
};

struct VCFMove {
    VCFResult result;
    Move best_move;
    std::vector<Move> variation;

    VCFMove() : result(VCFResult::UNKNOWN) {}
};

class VCFSearch {
public:
    VCFSearch(const Evaluator& evaluator, TranspositionTable& tt);

    VCFMove search(const Board& board, int color, int max_depth,
                   std::atomic<bool>& stop_flag);

    // Proves the position lost for `defender` (who moves first) when every legal
    // reply still leaves the opponent a forced VCF win. Only PROVEN_LOSS is a
    // resignation-grade answer; UNKNOWN means the proof was cut short.
    DefenseResult find_defense(const Board& board, int defender, int max_depth,
                               std::atomic<bool>& stop_flag, Move& defense_move);

    void set_node_limit(int limit) { node_limit_ = limit; }

    // Optional absolute wall-clock deadline shared with the rest of the search
    // (see VCTSearch::set_deadline); without it VCF runs to node_limit_ and can
    // overrun the caller's timeout_ms.
    void set_deadline(std::chrono::steady_clock::time_point deadline) {
        deadline_ = deadline;
        has_deadline_ = true;
    }

private:
    // #118 档二：这一层曾经是「每个节点建两张表、返回即弃」——打分对表靠几何扩容
    // （实测 `_M_realloc_append<pair<int,Move>>` 一次自对弈 77,619 次 = 全部堆分配的
    // 68.0 %），输出表每次调用再 reserve 一次（14,489 次 = 12.7 %）。两张表按递归深度
    // 分槽常驻：递归严格减 depth，父节点握着 slots[d] 时子节点只会碰 slots[d-1]，
    // 所以不存在「父循环还在迭代、子调用改了同一张表」。深度越出槽数就退回调用方的
    // 局部表（`overflow`）：只是不池化，语义一个字节没变。
    static constexpr int kMoveSlotCount = 24;

    struct MoveSlots {
        std::vector<std::pair<int, Move>> scored;
        std::vector<Move> moves;
    };

    const Evaluator& evaluator_;
    TranspositionTable& tt_;
    int node_limit_;
    int nodes_searched_;
    std::chrono::steady_clock::time_point deadline_{};
    bool has_deadline_ = false;
    std::array<MoveSlots, kMoveSlotCount> slots_{};

    bool aborted(std::atomic<bool>& stop_flag) const {
        return stop_flag.load() ||
               (has_deadline_ && std::chrono::steady_clock::now() >= deadline_);
    }

    int vcf_attack(Board& board, int color, int depth, int alpha, int beta,
                   std::atomic<bool>& stop_flag);

    // Fills `scored` with the (score, move) pairs worth trying, in the same order
    // and with the same comparator the callers already depended on.
    void collect_rush_fours(const Board& board, int color,
                            std::vector<std::pair<int, Move>>& scored) const;

    // Reference into `slots[depth]`, or into `overflow` when depth is out of range.
    // The reference stays valid until the next generate at the same depth.
    const std::vector<Move>& generate_rush_fours(const Board& board, int color, int depth,
                                                 std::vector<Move>& overflow);

    Move find_block_move(const Board& board, int row, int col, int color);

    // Takes the board it may mutate: the caller hands over a private working
    // copy, so the win-attempt loop can place/undo instead of cloning a board per
    // candidate. find_defense reuses one copy across all of the defender's
    // replies for the same reason.
    VCFMove search_impl(Board& board, int color, int max_depth,
                        std::atomic<bool>& stop_flag);

    // A move that makes five or a live four forces the opponent to answer, which
    // invalidates any win proof that assumed they keep the initiative.
    bool has_decisive_tempo(const Board& board, int row, int col, int color) const;
};

}

#endif

#ifndef GOMOKU_VCT_H
#define GOMOKU_VCT_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "../core/pattern.h"
#include "../optimization/transposition.h"
#include <atomic>
#include <chrono>
#include <vector>

namespace gomoku {

enum class VCTResult {
    WIN,
    LOSS,
    UNKNOWN
};

struct VCTMove {
    VCTResult result;
    Move best_move;
    // #88：WIN 必须带着**能逐手核对的那条线**回来。约定只有一条 —— 分数不是 100000+ 的那些
    // 出口（撞帽、超时、depth<=0、只到根扫描第一手的四条分支）都不会往里塞东西，所以
    // 「一句没法核对的主胜着」不可能带半截线回来；根扫描当场答的那四类，变例就是那一手。
    std::vector<Move> variation;

    VCTMove() : result(VCTResult::UNKNOWN) {}
};

class VCTSearch {
public:
    VCTSearch(const Evaluator& evaluator, TranspositionTable& tt);

    VCTMove search(const Board& board, int color, int max_depth,
                   std::atomic<bool>& stop_flag);

    void set_node_limit(int limit) { node_limit_ = limit; }

    // Nodes actually examined by the last search() - the readout tools/vct_slice_probe.cpp
    // prices VCT with. Observability only; nothing in the engine branches on it.
    int nodes_searched() const { return nodes_searched_; }

    // #89 的观察面：上一次 search() 里「对面此刻已经攥着成五点」这条判据（#84 立的规矩）
    // 拒掉了几条候选，按那三个调用点分开数：search() 的根扫描、vct_defend、vct_attack。
    // 根扫描那一处的行为早有结论级用例钉着；递归两处实测只动工作量、不动 result 与 best_move
    //（轮次197 的配对台 1496 格里 610 格节点数不同、结论分岔 0；批次、帽与口径的账记在
    // REVIEW_LOG 与 tests/test_search.cpp 那条用例的头上），所以「这一局到底走到过递归层没有」
    // 除了这三个计数没有别的读者 —— 用例钉的就是它们：哪一处改成永假，对应那个计数就归 0。
    int root_rejections() const { return root_rejections_; }
    int defend_rejections() const { return defend_rejections_; }
    int attack_rejections() const { return attack_rejections_; }

    // Optional absolute wall-clock deadline. VCT otherwise bounds work only by
    // node_limit_, so it can overrun the caller's timeout_ms by seconds; the
    // search engine sets this once so VCT/VCF/AlphaBeta share a single budget.
    void set_deadline(std::chrono::steady_clock::time_point deadline) {
        deadline_ = deadline;
        has_deadline_ = true;
    }

private:
    const Evaluator& evaluator_;
    TranspositionTable& tt_;
    int node_limit_;
    int nodes_searched_;
    int root_rejections_ = 0;
    int defend_rejections_ = 0;
    int attack_rejections_ = 0;
    std::chrono::steady_clock::time_point deadline_{};
    bool has_deadline_ = false;

    bool aborted(std::atomic<bool>& stop_flag) const {
        return stop_flag.load() ||
               (has_deadline_ && std::chrono::steady_clock::now() >= deadline_);
    }

    int vct_attack(Board& board, int color, int depth, int alpha, int beta,
                   std::atomic<bool>& stop_flag, std::vector<Move>& out_line);

    int vct_defend(Board& board, int color, int depth, int alpha, int beta,
                   std::atomic<bool>& stop_flag, std::vector<Move>& out_line);

    std::vector<Move> generate_attacks(const Board& board, int color);
    std::vector<Move> generate_defenses(const Board& board, int color, int attack_row, int attack_col);

    // #84：对面此刻有没有「下一手成五」的点。一条威胁主张（活四/双四/无防守点）只有在答案
    // 是「没有」时才成立 —— 否则防守方不必来挡，先落那一子棋就结束了。根扫描与递归两层原来
    // 都不看这一项：`generate_defenses` 只枚举「挡我这条威胁」的点，对面在别处已经能成五它
    // 不知道（实测 209 条开局 × 执黑侧 43 个根扫描 WIN 里 28 个属于这一类，轮次184）。
    // 只在三条进攻候选循环里调用（根扫描与 vct_attack/vct_defend 各一处），每条候选一次；扫描走
    // active_empty()（任何成五点都挨着子），不是全 225 格。
    bool defender_can_make_five(Board& board, int attacker) const;
};

}

#endif

#ifndef GOMOKU_REPLAY_ANALYZER_H
#define GOMOKU_REPLAY_ANALYZER_H

#include "../core/board.h"
#include "../core/evaluator.h"
#include "game_record.h"
#include <string>
#include <vector>

namespace gomoku {

// 设计文档 docs/ENGINE_REDESIGN_V2.md §6.2 的「段 A：零搜索筛查」。
//
// 段 A 只读 GameRecord 的逐手字段（score / depth / method / stopped），不碰棋盘、
// 不起搜索，所以它的成本是一次 JSON 解析加每手几次比较。它只回答一个问题：
// 「这一局里哪几手值得花段 B 的定预算复核」。
//
// 段 A 不做的事，写在这里免得下一轮当成缺陷：
//   * 不判妙手 / 俗手。那需要 root_scores（§6.3.1 是段 B 的能力），段 A 只有引擎
//     当时报的那一个 score，判不出「这一步排第几、差多少」；
//   * 不判 §6.4 判据 2 的「信息量」那一半（gap < 500），同一个原因。段 A 用的是
//     现在就能判的替代：摆幅小 ＋ 没有战术层参与 ＋ 深度不低于本局中位；
//   * 最后一手没有下一手，因此没有摆幅。它由段 B 的终局判定处理，不在段 A 里。
enum class ScreenReason {
    NONE,           // 既没被剔除，也没命中任何一条筛子
    BIG_SWING,      // |swing| >= swing_threshold
    METHOD_SHIFT,   // 相邻两手之间战术层状态变了（alphabeta <-> vcf/vct/必挡）
    MUST_BLOCK_RUN, // 连续两手及以上都是必挡
    DEPTH_DIP,      // 比本局中位深度浅 depth_dip 层以上
    MATE_CROSS,     // 将杀带（|score| >= mate_band）的进或出
    NOISE,          // 被剔除：摆幅小、深度不低于中位、且没有战术层参与
    OPENING,        // 被剔除：开局手，且不是战术手
    // 被剔除：replay / book。这一手不是引擎想出来的——replay 是重放的开局前缀
    // （RetryExplorer 每轮回退后重放的那些手），book 是查表。它们身上没有可复盘的
    // 决策，摆幅再大也不是候选。第一份真实记录里 86 手有 46 手是 replay，其中一手
    // 还被标成了 method_shift，这条剔除就是它逼出来的（REVIEW_LOG 轮次144）。
    REPLAYED,
};

// 直方图的下标上界。写成常量而不是往 enum 里塞 COUNT，是为了让下面那个
// reason_name 的 switch 保持「覆盖每一个真实取值」——多一个哨兵会立刻触发 -Wswitch。
constexpr int kScreenReasonCount = 9;

struct PlyScreen {
    int ply = 0;
    int side = EMPTY;       // BLACK / WHITE，取自记录里那一手自己的颜色
    int score = 0;          // 引擎在 ply 报的分，行棋方视角
    int next_score = 0;     // 下一手的分，对手视角；最后一手为 0
    int swing = 0;          // (-next_score) - score，正数 = 走完这手自己变好了
    int depth = 0;
    std::string method;
    ScreenReason reason = ScreenReason::NONE;
    bool candidate = false;
};

struct GameScreen {
    std::vector<PlyScreen> plies;
    int total_plies = 0;
    int candidates = 0;
    int noise = 0;
    int opening = 0;
    int replayed = 0;
    // §6.4 的 flat_game 计数：候选手少于 2 手的一局不产题卡，但这个计数本身有用
    // ——它占比上升说明训练对局变得没内容了。
    int flat = 0;
    std::vector<int> reasons;  // 下标 = static_cast<int>(ScreenReason)
};

struct ScreenConfig {
    int swing_threshold = 5000;  // §5.3 analyze_swing 的默认
    int opening_plies = 6;       // §5.3 analyze_opening_plies 的默认
    int quiet_band = 500;        // §6.4 的「无信息」带
    int depth_dip = 2;
    int mate_band = 90000;       // VCT 报 90000、VCF 报 100000，取低的那一个
};

class ReplayAnalyzer {
public:
    explicit ReplayAnalyzer(ScreenConfig config = ScreenConfig{});

    GameScreen screen(const GameRecord& record) const;
    const ScreenConfig& config() const { return config_; }

    // §6.3.2：swing_i = (-score_{i+1}) - score_i。
    // score_i 是第 i 手行棋方视角，而 score_{i+1} 是对方的视角，所以「走完这一手
    // 之后自己的形势」= -score_{i+1}。越界的 ply 返回 0（没有下一手就没有摆幅）。
    static int swing_at(const GameRecord& record, int ply);

    // 五个快路径与战术层。它们的手不是「引擎想出来的」，是「局面逼出来的」，
    // 所以既不该被开局判据剔除，也不该被「无信息」判据剔除。
    static bool is_tactical_method(const std::string& method);
    static bool is_forced_method(const std::string& method);

    static const char* reason_name(ScreenReason reason);

private:
    ScreenConfig config_;
};

// §6.7 段 B 的复核配置。默认值就是 §5.3 的 analyze_* 默认，一个不多一个不少。
struct ReviewConfig {
    int budget = 20000;         // analyze_budget：定节点，不定时钟
    int max_depth = 8;          // analyze_depth
    int blunder_swing = 5000;   // analyze_swing：失分到多少算俗手
    int brilliant_gap = 3000;   // analyze_gap：领先次优多少算妙手
    int forced_margin = 500;    // 「不输」的容差，用来数唯一应手
    int ladder_min = 1000;      // §6.5 的几何阶梯
    int ladder_max = 256000;
};

// 段 B 对一手的复核结果。`kind` 就是 §6.3.2 那六种判定加一个 unlisted。
struct PlyReview {
    int ply = 0;
    int side = EMPTY;
    Move played;
    Move best;
    Move second;
    int best_score = 0;
    int played_score = 0;
    int second_score = 0;
    int loss = 0;      // best - played
    int gap = 0;       // best - second
    int root_moves = 0;
    int budget = 0;
    int depth = 0;
    std::string method;
    std::string kind = "none";
    bool unlisted = false;
};

// §6.5 的难度：从最小预算往上问「现在能解出正解了吗」。
struct LadderResult {
    int solve_budget = 0;  // 第一个解出的预算；0 = 跑到顶也没解出
    int failed_at = 0;     // 解出前最后一次失败的预算，构成区间 (failed_at, solve_budget]
};

// 段 B 与段 C。它要跑真搜索，所以和段 A（纯记录解析）分开：段 A 每局微秒级，
// 段 B 每手一次定预算搜索，段 C 每张卡 3–5 次。
class ReplayReviewer {
public:
    ReplayReviewer(const ThreatCoefficients& coeffs, ReviewConfig config = ReviewConfig{});

    // 把记录摆到第 ply 手落子之前。非法着法返回 false（那条记录不可复盘）。
    bool position_before(const GameRecord& record, int ply, Board& out) const;

    // 段 B：对第 ply 手做定预算复核。
    PlyReview review(const GameRecord& record, int ply) const;

    // 段 C：最小能解出 answer 的节点预算。
    LadderResult difficulty(const Board& board, int side, Move answer) const;

private:
    PlyReview review_board(const Board& board, int side, Move played, int record_score) const;

    ThreatCoefficients coeffs_;
    ReviewConfig config_;
};

}

#endif

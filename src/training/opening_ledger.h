#ifndef GOMOKU_OPENING_LEDGER_H
#define GOMOKU_OPENING_LEDGER_H

#include "../core/types.h"

#include <string>
#include <vector>

namespace gomoku {

struct CoefficientPair;

// v2 §3.2 S2（指导书 §4 丁-6）：开局集按双方胜机筛选——**事后退役版**。
//
// 一句话：训练赛的一局棋要能**区分两个玩家**才值得其位。一条开局在两局里（候选分别坐黑、
// 坐白）如果都是同一个座位赢，那一对的局分恒等于 0.5——谁强看不出来；这种开局进得多，适配度
// 读到的就只是「谁拿到了好座位」。
//
// 为什么不是指导书思路 1/2 那种「跑之前先筛」：两半都在轮次161 量过。
//   (a) 判据的形状。思路 2 那句「双方胜率都落在 [0.25, 0.75]」量的是同一座位跑 K 局的胜率，
//       而 `tools/opening_rule_screen.cpp --repeat 2` 实测 32 局**逐字相同**（确定性引擎），
//       每色的 K 局胜率只能取 0 或 1 ⇒ 那条判据永远点不着。注意 2 给的出口是「筛选判据选错时，
//       **换判据而不是放宽阈值**」，换完就是下面那本账。
//   (b) 成本。事前筛 N 条要付 2N 局：48 条 = 96 局 > 一轮训练的 ~28 局（population 8 × fitness 2
//       ＋ 验证赛 8），而且键含冠军系数 ⇒ 每次晋升重付一遍。杀线「筛选成本 > 省下的训练局数」
//       在数值上被击中，于是改成**事后**：证据全部来自本来就要跑的适配度赛与验证赛，新增 0 局。
//
// 账本口径（每条开局一本账）：
//   一对观察 = 同一条开局的两局，候选各坐一次色（`Trainer::score_match` 的配对形状）。
//   两局都跑完且**都分了胜负**、赢家是同一个座位 ⇒ 这一对「被座位钉死」；含未分胜负的对不算
//   证据——它既没被钉死，也没证明这条开局能把两个人分开。
//   被看满 `min_pairs` 对且每一对都被钉死 ⇒ 退役；只要出现过一对能区分的，这条开局永不退役。
//
// 键（`set_sha`）覆盖**这次 run 起点那一套系数**（`versions_[0]`）的黑白两个半套指纹，加上产生
// 这些证据的档位（子数、深度、节点帽、手数上限、池子、门槛）。冠军换代不该把已经花掉的对局证据
// 清空——起点没变，账还是那本账；起点换了（续训、换参数文件），旧账作废。
//
// 起跑线（轮次161 E 臂实测：缺省系数 vs `data/training/best_params.json`，两者只差
// `cand_width_root` 24/15）：48 条三面开局里 38 对被座位钉死（79.2 %）、9 条留下（18.8 %）、
// 1 对含未分胜负；同一批在 24 条上是 19/5，留下率同形 ⇒ 不是抽样噪声。
//
// `pool = 0` 是缺省，也就是**关**：一条额外的局都不跑、一个字节都不多写，开局仍是训练那套固定
// 8 条轮换（`Trainer::match_opening_line`）。
struct OpeningLedgerConfig {
    int pool = 0;          // 账本跟踪几条开局；0 = 关
    int min_pairs = 2;     // 一条开局要被看满几对才允许退役
    int stones = 3;        // 开局几颗子，与 `match_opening_plies` 同一口径（进 sha）
    int depth = 4;         // 以下三项是**产生证据的那一档**，换档 = 换一批证据
    int max_nodes = 1500;
    int max_moves = 60;
    std::string path = "data/training/openings.json";
};

struct OpeningLedgerEntry {
    std::vector<Move> line;
    int pairs = 0;         // 有效对数（两局都跑完的）
    int pinned_pairs = 0;  // 其中被座位钉死的对数
    bool retired = false;  // pairs >= min_pairs && pinned_pairs == pairs

    // 半个对子：一局来了先记在这里，等它的姊妹局。落盘不写这一对字段——账本只在判决翻转
    // 时写，半途的一局下次跑到自然会补齐。
    bool has_pending = false;
    int pending_seat = 0;
};

struct OpeningLedgerState {
    OpeningLedgerConfig config;
    std::string set_sha;             // 空 = 这一代没有账本（关着，或键不合）
    std::vector<OpeningLedgerEntry> entries;
    int observed_pairs = 0;          // 全部开局的有效对数之和
    int retired = 0;                 // 退役条数
    bool reused = false;             // true = 账本是读回来的，不是这次从零记的

    // 未退役的那些开局，按池内顺序。少于 `Trainer::kMatchOpeningSet` 条时调用方退回固定轮换
    //（指导书 §4 丁-6 杀线的后半段：筛不出可用的集）。
    std::vector<std::vector<Move>> active_lines() const;
};

class OpeningLedger {
public:
    // 指纹 = 这次 run 起点系数的黑白两个半套 + 档位六项。换起点系数或换档，旧证据就不作数。
    static std::string set_sha(const CoefficientPair& base, const OpeningLedgerConfig& config);

    // 从零建池子：第 i 条开局 = `Trainer::match_opening_line(i, config.stones)`。
    static OpeningLedgerState fresh(const OpeningLedgerConfig& config, const std::string& sha);

    // sha 不合时返回 false 并在 `error` 里写清「是哪一套键记的 vs 现在这套」——换起点系数
    // 就该重记，留着别人的账比没有更糟。
    static bool load(const std::string& path, const std::string& sha, OpeningLedgerState& out,
                     std::string& error);
    static bool save(const OpeningLedgerState& state, std::string& error);

    // 记一局：`seat_winner` 是这一局**座位**（颜色）的赢家，1 黑、-1 白、0 未分胜负。同一开局
    // 的两局凑成一对。返回值 = 这一局之后判决有没有翻转：1 刚退役、-1 被恢复、0 没变。
    static int record_game(OpeningLedgerState& state, const std::vector<Move>& line,
                           int seat_winner);

    // 一句人话摘要，进训练日志。
    static std::string summarize(const OpeningLedgerState& state);
};

}  // namespace gomoku

#endif

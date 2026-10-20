#ifndef GOMOKU_TACTICS_CARD_H
#define GOMOKU_TACTICS_CARD_H

#include "../core/types.h"
#include "../core/evaluator.h"
#include <string>
#include <vector>

namespace gomoku {

// 设计文档 docs/ENGINE_REDESIGN_V2.md §6.6 的题卡。
//
// 一张卡 = 一个局面 ＋ 一个正解 ＋ 一份证据 ＋ 一个难度。它是段 A（筛哪几手）、
// 段 B（判成什么）、段 C（有多难）三段的唯一产物，也是三个消费方（训练样本、
// 摆棋测试、回归门禁）唯一认识的格式。
//
// `stones` 存全量棋谱而不是哈希：卡片必须自包含——能直接摆到棋盘上、能进
// position_setup、能在原始对局文件丢了以后独立复现。
//
// `coeffs_sha` 不是元数据，是正确性的一部分：判断题的正解依赖评估系数，换一套
// 系数同一个局面的「正解」可能变。没有这个字段，题库会在系数换代后静默变成一份
// 错的答案（§6.6 的 proven / judged 分表就是为了这件事）。
struct TacticsCard {
    std::string schema = "tactics/1";
    std::string id;                       // "<game>-p<ply>"
    std::string layer = "normal";         // easy / normal / hard，由段 C 定
    std::string kind;                     // blunder / brilliant / forced / counterexample / missed_mate
    int side = EMPTY;                     // 行棋方
    int ply = 0;
    std::vector<Move> stones;             // 全量棋谱（含最后一手）
    Move last_move;                       // stones.back()
    Move answer;                          // 正解
    std::vector<Move> alt_answers;        // 唯一应手题的其它等价解（通常为空）

    // 证据：段 B 复核那一次的读数，原样存，不改写。
    int best_score = 0;
    int played_score = 0;
    int second_score = 0;
    int gap = 0;                          // best - second
    int loss = 0;                         // best - played
    int budget = 0;                       // 复核用的节点预算
    int depth = 0;                        // 复核完成到的层
    Move played;
    Move second;
    std::string method;                   // 复核那次的 method（alphabeta / vcf / ...）

    // 难度是区间不是点：(failed_at, solve_budget]
    int solve_budget = 0;
    int failed_at = 0;

    std::string coeffs_sha;
    std::string generator = "analyze/1";
    std::string source_game;
    int source_index = 0;

    std::string to_json() const;
    static bool from_json(const std::string& text, TacticsCard& out, std::string& error);

    // §6.5 的分层：<= 8k 是 easy，<= 64k 是 normal，其余（含解不出）是 hard。
    static std::string layer_for(int solve_budget);
    static bool layer_at_least(const std::string& layer, const std::string& floor);
};

// 系数指纹：14 个可调量按 FNV-1a 压成 8 位十六进制。它只用来回答一个问题——
// 「这张卡是在哪套系数下判出来的」，所以不需要抗碰撞，只需要稳定。
std::string coefficients_fingerprint(const ThreatCoefficients& coeffs);

}

#endif

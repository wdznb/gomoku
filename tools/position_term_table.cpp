// 位置项 / 贴边项的逐格差分表（设计文档 v2 §7 回归保障 3，v1.0 的 :556 欠的那张表）。
// 42 副普查棋盘 × 225 格 × 2 色 = 18,900 行，每行把 Evaluator 的两个成员函数与一份
// 「按定义从头求和」的独立实现摆在一起；两者必须逐格相等，否则这行 match=0、工具非零退出。
//
// 这一项不改任何生产代码：它的价值是以后动评估器时，有一张表能立刻指出哪一格变了。
//
// 用法：build/position_term_table [--out data/diff/position_term.csv]
#define private public
#include "core/evaluator.h"
#undef private
#include "core/board.h"
#include "core/types.h"
#include "../tests/pinned_positions.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace gomoku;

namespace {

// 定义本身：中心是 BOARD_SIZE/2，权重按曼哈顿距离线性衰减；贴边看的是「外两行/列」，
// 一行一列各算一次，所以角上那一格是 2。这两段是照着 §2.5.2 的定义写的，
// 不调用 Evaluator 的任何成员函数——差分要是有意义，两边就必须是两个来源。
int definition_weight(int row, int col, const ThreatCoefficients& coeffs) {
    const int center = BOARD_SIZE / 2;
    const int max_dist = center * 2;
    const int dist = std::abs(row - center) + std::abs(col - center);
    return (max_dist - dist) * coeffs.position_weight_scale;
}

int definition_closed_edges(int row, int col) {
    int closed = 0;
    if (row <= 1 || row >= BOARD_SIZE - 2) ++closed;
    if (col <= 1 || col >= BOARD_SIZE - 2) ++closed;
    return closed;
}

int definition_position_term(const Board& board, int color, const ThreatCoefficients& coeffs) {
    long long total = 0;
    for (const Move& move : board.move_history()) {
        if (move.color != color) continue;
        total += definition_weight(move.pos.row, move.pos.col, coeffs);
    }
    return static_cast<int>(total * coeffs.distance_to_stones);
}

int definition_edge_term(const Board& board, int color, const ThreatCoefficients& coeffs) {
    long long total = 0;
    for (const Move& move : board.move_history()) {
        if (move.color != color) continue;
        total += definition_closed_edges(move.pos.row, move.pos.col);
    }
    return static_cast<int>(total * coeffs.edge_penalty);
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_path = "data/diff/position_term.csv";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--out") out_path = argv[i + 1];
    }

    PatternRecognizer::init();
    const Evaluator evaluator;
    const ThreatCoefficients coeffs = evaluator.coefficients();
    const std::vector<Board> boards = pinned_census_boards();
    if (boards.size() != 42) {
        std::fprintf(stderr, "census is %zu boards, expected 42\n", boards.size());
        return 2;
    }

    std::ofstream out(out_path);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", out_path.c_str());
        return 2;
    }
    out << "board,row,col,color,position_term,edge_penalty_term,"
           "recomputed_position_term,recomputed_edge_penalty_term,match\n";

    long long rows = 0;
    long long mismatches = 0;
    for (size_t b = 0; b < boards.size(); ++b) {
        for (int row = 0; row < BOARD_SIZE; ++row) {
            for (int col = 0; col < BOARD_SIZE; ++col) {
                for (const int color : {BLACK, WHITE}) {
                    // 每一格真的放一颗子：差分测的是「这一格对这两项的边际」，
                    // 整盘求和重复 225 次的话，改一格的权重根本不会让任何一行变红。
                    Board probe = boards[b];
                    probe.place_stone(row, col, color);   // 占了就按原盘比，行仍然有效
                    const int impl_position = evaluator.position_term(probe, color);
                    const int impl_edge = evaluator.edge_penalty_term(probe, color);
                    const int want_position = definition_position_term(probe, color, coeffs);
                    const int want_edge = definition_edge_term(probe, color, coeffs);
                    const int match = (impl_position == want_position && impl_edge == want_edge) ? 1 : 0;
                    if (!match) ++mismatches;
                    out << b << ',' << row << ',' << col << ',' << (color == BLACK ? "black" : "white")
                        << ',' << impl_position << ',' << impl_edge << ','
                        << want_position << ',' << want_edge << ',' << match << '\n';
                    ++rows;
                }
            }
        }
    }
    out.close();

    std::printf("position_term_table: rows=%lld boards=%zu mismatches=%lld -> %s\n",
                rows, boards.size(), mismatches, out_path.c_str());
    if (rows != 42LL * BOARD_SIZE * BOARD_SIZE * 2) {
        std::fprintf(stderr, "expected %lld rows, got %lld\n", 42LL * BOARD_SIZE * BOARD_SIZE * 2, rows);
        return 2;
    }
    if (mismatches != 0) {
        std::fprintf(stderr, "MISMATCH: %lld of %lld rows disagree with the by-definition sum\n",
                     mismatches, rows);
        return 1;
    }
    std::printf("all %lld rows match\n", rows);
    return 0;
}

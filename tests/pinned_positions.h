// 常驻的棋盘集（设计文档 v2 §7 回归保障 1 与 3，指导书乙-1／乙-2）。
// 两处都读这同一份：`tools/position_term_table` 要 42 副普查棋盘做逐格差分，
// `test_pinned` 要 pos24 与两副 d8 臂做选点基准。test_search.cpp 里另有一份
// `c2_census_boards()`，那是**故意**留着的：它的 include 块压在三条被按行号引用的
// 断言（:2591、:3973、:3986）之上，加一个 #include 会把三处引用一起推走。
// 两份生成器逐位相同（同一个 seed、同一个 LCG、同一个循环顺序），改了这里就要改那里。
#ifndef GOMOKU_TESTS_PINNED_POSITIONS_H
#define GOMOKU_TESTS_PINNED_POSITIONS_H

#include "core/board.h"
#include "core/types.h"

#include <vector>

namespace gomoku {

// 42 副：40 副伪随机 ＋ 两副把每一条过中心的线都占满的，于是普查既跑在普通形状上，
// 也跑在那些按色分叉的修正上（WHITE 的长连降级、BLACK 的长连升级、锁四与困三的走法）。
inline std::vector<Board> pinned_census_boards() {
    std::vector<Board> boards;
    unsigned long long state = 0x20261019ULL;
    auto drawn = [&state](int bound) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<int>((state >> 33) % static_cast<unsigned long long>(bound));
    };
    for (int game = 0; game < 40; ++game) {
        Board board;
        const int stones = 1 + drawn(45);
        for (int i = 0; i < stones; ++i) {
            board.place_stone(drawn(BOARD_SIZE), drawn(BOARD_SIZE), i % 2 == 0 ? BLACK : WHITE);
        }
        boards.push_back(board);
    }
    for (const int color : {BLACK, WHITE}) {
        Board board;
        for (int i = 0; i < BOARD_SIZE; ++i) {
            board.place_stone(7, i, color);
            board.place_stone(i, 7, color);
            board.place_stone(i, i, color);
            board.place_stone(i, BOARD_SIZE - 1 - i, color);
        }
        boards.push_back(board);
    }
    return boards;
}

// 24 子的 positional 局，盘面上没有任何一步是逼出来的——正解与树都不是一步棋决定的。
inline Board pinned_pos24() {
    Board board;
    const int stones[][3] = {
        {9, 11, BLACK}, {0, 14, WHITE}, {10, 2, BLACK}, {6, 6, WHITE},
        {10, 4, BLACK}, {9, 2, WHITE}, {4, 2, BLACK}, {5, 3, WHITE},
        {2, 3, BLACK}, {10, 1, WHITE}, {12, 12, BLACK}, {2, 6, WHITE},
        {1, 8, BLACK}, {14, 8, WHITE}, {4, 1, BLACK}, {13, 4, WHITE},
        {1, 14, BLACK}, {8, 11, WHITE}, {10, 10, BLACK}, {8, 7, WHITE},
        {12, 1, BLACK}, {11, 6, WHITE}, {5, 2, BLACK}, {0, 7, WHITE},
    };
    for (const auto& s : stones) board.place_stone(s[0], s[1], s[2]);
    return board;
}

}  // namespace gomoku

#endif

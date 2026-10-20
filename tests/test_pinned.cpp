// 乙-2：把选点一致性基准从 REVIEW_LOG 的 /tmp 读数搬成树内常驻臂（v2 §7 回归保障 1，v1.0 :554）。
// 单独一个套件，不塞进 test_search：d8 两臂在 -O2 上各要 6 s 与 10 s，而人一旦开始跳过
// 变慢的 ctest，门禁就死了（指导书 §4 乙-2 的注意）。
//
// 每臂只钉三件事：着法的格号、分值、节点数（v1.0 :554 那句「着法变化必须能解释」的形状）。
#include "core/board.h"
#include "core/pattern.h"
#include "core/types.h"
#include "search/search_engine.h"
#include "pinned_positions.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>
#include <string>
#include <vector>

using namespace gomoku;

namespace {

struct Arm {
    const char* name;
    Board board;
    int color;
    int depth;
    const char* method;
    int score;
    int row;
    int col;
    int nodes;
    const char* source;   // 这一行的读数是从哪来的：历史出厂针，还是本轮实测
};

// 与 test_search.cpp:6841 那张表同一台仪器：默认候选宽度（24/8）、定深、时限远在水位之上，
// 所以停在depth 的不是钟。节点帽给一个不参与判定的宽值（第三口 case 证明它不 binding）。
Board quiet_pose() {
    Board board;
    board.place_stone(7, 7, BLACK);
    board.place_stone(7, 8, WHITE);
    board.place_stone(8, 8, BLACK);
    board.place_stone(6, 6, WHITE);
    return board;
}

std::vector<Arm> pinned_arms() {
    const Board pos24 = pinned_pos24();
    return {
        // 前两行是 REVIEW_LOG 轮次135/138 记过的出厂针：`quiet BLACK d8 = 1,408,083`、
        // `pos24 BLACK d8 = 2,378,724`，本轮在 -O2 上逐位复现（着法 8,7 / 7,4、分值 77,842 / 99,999）。
        {"quiet BLACK d8", quiet_pose(), BLACK, 8, "alphabeta", 77842, 8, 7, 1408083,
         "轮次138 出厂针，本轮复现"},
        {"pos24 BLACK d8", pos24, BLACK, 8, "alphabeta", 99999, 7, 4, 2378724,
         "轮次138 出厂针，本轮复现"},
        // 这四行是浅的那几臂，与 test_search.cpp:6841 的 depth-6 表同一批读数（前两行逐字相同）。
        {"quiet BLACK d6", quiet_pose(), BLACK, 6, "alphabeta", 71992, 8, 7, 221774,
         "与 :6841 表同行"},
        {"quiet WHITE d6", quiet_pose(), WHITE, 6, "alphabeta", 97044, 6, 7, 156428,
         "与 :6841 表同行"},
        {"pos24 BLACK d6", pos24, BLACK, 6, "alphabeta", 99999, 7, 4, 300347,
         "本轮实测（-O2）"},
        {"pos24 WHITE d6", pos24, WHITE, 6, "alphabeta", 99999, 10, 7, 175088,
         "本轮实测（-O2）"},
    };
}

SearchResult run(const Board& board, int color, int depth, int node_cap) {
    std::atomic<bool> stop(false);
    SearchEngine engine;
    engine.set_max_nodes(node_cap);
    // 600 s 的时限不参与判定，只为防死循环（指导书 §9.2：定深臂用节点帽当超时）。
    return engine.search(board, color, depth, 600000, stop);
}

// 一：钉住每一臂的答案本身。
void test_the_pinned_choice_arms_answer_their_recorded_move() {
    for (const Arm& arm : pinned_arms()) {
        const SearchResult result = run(arm.board, arm.color, arm.depth, 8000000);
        assert(result.method == std::string(arm.method));
        assert(result.depth == arm.depth);
        assert(result.score == arm.score);
        assert(result.best_move.pos.row == arm.row);
        assert(result.best_move.pos.col == arm.col);
        assert(result.nodes_searched == arm.nodes);
        std::printf("  PIN %-15s %d,%d score %d nodes %d (%s)\n",
                    arm.name, arm.row, arm.col, arm.score, arm.nodes, arm.source);
    }
    std::puts("test_the_pinned_choice_arms_answer_their_recorded_move passed");
}

// 二：边界——钉的是棋盘集本身。副数、pos24 的子数、以及那两副过中心的满线盘，
// 改了它们就等于改了臂，这里先让「改盘面」比「改答案」更早变红。
void test_the_pinned_board_set_is_the_one_the_arms_were_read_on() {
    const std::vector<Board> census = pinned_census_boards();
    assert(census.size() == 42);
    assert(census[0].move_count() >= 1);
    // 第 41、42 副是那两副满线盘：每条过中心的线上都摆满了同一色。
    for (size_t b = 40; b < 42; ++b) {
        // 四条线在中心相交，也两两共享角格，所以摆上去的子比 4×15 少：这里钉的是
        // 「四条线每一格都有子」加上「子数正好等于四条线的并」，改任一条线都会红。
        std::vector<std::pair<int, int>> union_cells;
        for (int i = 0; i < BOARD_SIZE; ++i) {
            assert(!census[b].is_empty(7, i));
            assert(!census[b].is_empty(i, 7));
            assert(!census[b].is_empty(i, i));
            assert(!census[b].is_empty(i, BOARD_SIZE - 1 - i));
            const int candidates[4][2] = {{7, i}, {i, 7}, {i, i}, {i, BOARD_SIZE - 1 - i}};
            for (const auto& cell : candidates) {
                bool seen = false;
                for (const auto& held : union_cells) {
                    if (held.first == cell[0] && held.second == cell[1]) { seen = true; break; }
                }
                if (!seen) union_cells.push_back({cell[0], cell[1]});
            }
        }
        assert(census[b].move_count() == static_cast<int>(union_cells.size()));
    }
    const Board pos24 = pinned_pos24();
    assert(pos24.move_count() == 24);
    assert(pos24.move_history().size() == 24);
    assert(quiet_pose().move_count() == 4);
    std::printf("  PIN census=42 boards, pos24=24 stones, quiet=4 stones\n");
    std::puts("test_the_pinned_board_set_is_the_one_the_arms_were_read_on passed");
}

// 三：旧行为不变 ＋ 节点帽不是装饰。宽帽与「不给帽」必须逐字同答；把帽压到 20,000，
// 同一臂就必须以 nodes 停下来并把节点数压在帽下——否则这条臂上的「帽」只是一个数字。
void test_the_node_cap_is_a_control_and_not_part_of_the_answer() {
    const Board board = quiet_pose();

    SearchEngine no_cap_engine;   // 默认：没有节点帽
    std::atomic<bool> stop(false);
    const SearchResult uncapped = no_cap_engine.search(board, BLACK, 6, 600000, stop);
    const SearchResult capped = run(board, BLACK, 6, 8000000);
    assert(uncapped.nodes_searched == 221774);
    assert(capped.best_move.pos == uncapped.best_move.pos);
    assert(capped.score == uncapped.score);
    assert(capped.nodes_searched == uncapped.nodes_searched);

    const SearchResult tight = run(board, BLACK, 6, 20000);
    assert(tight.stopped == "nodes");
    assert(tight.nodes_searched <= 20000);
    assert(tight.depth < 6);
    std::printf("  PIN cap 8000000 == no cap (221774 nodes), cap 20000 stops at %d nodes at depth %d\n",
                tight.nodes_searched, tight.depth);
    std::puts("test_the_node_cap_is_a_control_and_not_part_of_the_answer passed");
}

}  // namespace

int main() {
    std::puts("Running pinned choice arms (乙-2)...");
    PatternRecognizer::init();

    test_the_pinned_choice_arms_answer_their_recorded_move();
    test_the_pinned_board_set_is_the_one_the_arms_were_read_on();
    test_the_node_cap_is_a_control_and_not_part_of_the_answer();

    std::puts("All pinned choice arms passed!");
    return 0;
}

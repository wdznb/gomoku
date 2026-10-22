// 基因可达性探针（轮次174 的读数出处）：改一个系数，看它在评估里到底读不读得到。
//
// 推理是代码级的：pattern_score 只在 evaluate_position_pair 的两个调用点出账
// （evaluator.cpp:267 += 自己、:270 -= 对手），两处都被 `my_color != BLACK ||
// !is_forbidden_from_pattern(my_pattern)` 这一类条件包着；而 has_three_three /
// has_four_four 恰恰就是 is_forbidden_from_pattern 判黑违规的那两个字段
// （pattern.cpp:558,594）。⇒ 只要该颜色是黑且形状是三三或四四，整个 pattern_score
// 被跳过，black.double_three 与 black.double_four 没有可以出账的地方。
//
// 这里做两件事：① 在黑走满所有空格 × 一批盘面上量「改系数前后的总分」；
// ② 同一次跑里用 **白侧同名字段** 作正控 —— 白不受禁手，改动必须看见。
#include "core/board.h"
#include "core/evaluator.h"
#include "core/pattern.h"
#include "core/types.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using nlohmann::json;
using namespace gomoku;

namespace {

long long total_over(const Evaluator& ev, const std::vector<Board>& boards) {
    long long total = 0;
    for (const Board& board : boards) {
        for (int row = 0; row < BOARD_SIZE; ++row) {
            for (int col = 0; col < BOARD_SIZE; ++col) {
                if (!board.is_empty(row, col)) continue;
                total += ev.evaluate_position_pair(board, row, col, BLACK);
                total += ev.evaluate_position_pair(board, row, col, WHITE);
            }
            total += ev.evaluate_board(board, BLACK);
            total += ev.evaluate_board(board, WHITE);
        }
    }
    return total;
}

Board board_of(const json& doc) {
    Board board;
    if (!doc.contains("moves") || !doc["moves"].is_array()) return board;
    int colour = BLACK;
    if (doc.value("first", std::string("black")) == "white") colour = WHITE;
    for (const auto& mv : doc["moves"]) {
        if (!mv.is_array() || mv.size() != 2) break;
        if (!board.place_stone(mv[0].get<int>(), mv[1].get<int>(), colour)) break;
        colour = opponent(colour);
    }
    return board;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string openings_path = argc > 1 ? argv[1] : "tools/match_openings.jsonl";
    std::vector<Board> boards;
    std::ifstream in(openings_path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        json doc = json::parse(line, nullptr, false);
        if (doc.is_discarded()) continue;
        boards.push_back(board_of(doc));
    }
    std::printf("盘面=%zu 个（来自 %s）\n", boards.size(), openings_path.c_str());

    CoefficientPair base = CoefficientPair::shared(ThreatCoefficients{});
    const long long base_total = total_over(Evaluator(base), boards);
    std::printf("基线总分 = %lld\n", base_total);

    struct Arm { const char* name; int which; int field; int value; };
    // which: 0=black 1=white；field: 0=double_three 1=double_four 2=four_three
    const std::vector<Arm> arms = {
        {"黑 double_three → 1",     0, 0, 1},
        {"黑 double_three → 200000", 0, 0, 200000},
        {"黑 double_four  → 1",     0, 1, 1},
        {"黑 double_four  → 200000", 0, 1, 200000},
        {"白 double_three → 1",     1, 0, 1},
        {"白 double_four  → 200000", 1, 1, 200000},
        {"黑 four_three   → 1（正控：合法形状，必须看得见）", 0, 2, 1},
    };
    for (const Arm& arm : arms) {
        CoefficientPair p = base;
        ThreatCoefficients& t = arm.which == 0 ? p.black : p.white;
        if (arm.field == 0) t.double_three = arm.value;
        else if (arm.field == 1) t.double_four = arm.value;
        else t.four_three = arm.value;
        const long long v = total_over(Evaluator(p), boards);
        std::printf("  %-46s 总分=%lld  差=%lld  ⇒ %s\n", arm.name, v, v - base_total,
                    v == base_total ? "读不到（惯性基因）" : "读得到");
    }
    return 0;
}

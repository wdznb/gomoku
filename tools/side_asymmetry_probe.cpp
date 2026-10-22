// 色别不对称探针（轮次172 的读数出处；只读，不改 src/ 也不改任何产品行为）。
//
// 要回答的是三个数，不是假设：
//   A. 禁手到底吃掉黑方多少候选 —— 在棋盘自己的候选域上数 `is_forbidden`，并报出
//      「黑无路可走」（候选全被禁手吃掉）的局面数；
//   B. 同一张盘面上，搜索**真正拿到的**分支宽度黑与白差多少（走真的 MoveGenerator，
//      所以这个宽度差是线上那套分层/宽度帽的结果，不是理想棋盘的想象）；
//   C. 自对弈记录里按色别的胜负与结束原因构成，以及白胜的那些局，黑在最后几手时
//      被禁手挤掉多少候选（「禁手致负」的代理读数）。
//
// 输入两类：开局集（tools/match_openings.jsonl 那种 position 文档）与对局记录
// （gomoku_train --records 的 GameRecord JSONL，moves[] 带 row/col/color）。
// 全部按盘面确定性统计，不跑搜索 ⇒ 秒级出数。
#include "core/board.h"
#include "core/evaluator.h"
#include "core/pattern.h"
#include "core/types.h"
#include "search/move_generator.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using nlohmann::json;
using namespace gomoku;

namespace {

struct Bands {
    // 按手数分档：0-9 / 10-19 / 20-29 / 30-39 / 40+
    static int index_of(int plies) {
        if (plies < 10) return 0;
        if (plies < 20) return 1;
        if (plies < 30) return 2;
        if (plies < 40) return 3;
        return 4;
    }
};

struct Aggregate {
    long positions = 0;
    long cand_sum = 0;
    long forb_black_sum = 0;     // 「黑若在此行棋会被吃掉多少候选」
    long forb_mover_sum = 0;     // 「行棋方自己被吃掉多少」——白侧按定义恒为 0，那是控制
    long no_move_positions = 0;  // 候选域非空但全被禁手吃掉（黑无路可走）
    long width_black = 0, width_white = 0;
    long width_black_empty = 0, width_white_empty = 0;
    long band_positions[5] = {0, 0, 0, 0, 0};
    long band_forb[5] = {0, 0, 0, 0, 0};
    long band_cand[5] = {0, 0, 0, 0, 0};
};

class Prober {
public:
    explicit Prober(const Evaluator& ev) : gen_(ev) {}

    // 在 board 上对 `color` 执黑/执白两种身份都量一遍，但只在 `side_to_measure` 那一侧
    // 计禁手（禁手只判黑，白侧的禁手数按定义就是 0，那是控制而不是发现）。
    void measure(const Board& board, int side_to_measure, Aggregate* agg) const {
        std::vector<Position> domain = board.get_candidate_moves();
        long forb_black = 0;
        for (const Position& p : domain) {
            if (PatternRecognizer::is_forbidden(board, p.row, p.col, BLACK)) ++forb_black;
        }
        // 行棋方自己被吃掉的数：白侧恒为 0（is_forbidden 的第一行就是 color != BLACK 就返回），
        // 那一格是「规则确实单边」的控制，不是发现。
        const long forb_mover = side_to_measure == BLACK ? forb_black : 0;
        const bool black_to_move = side_to_measure == BLACK;
        const long width_own = static_cast<long>(gen_.generate_moves(board, side_to_measure, 0, false).size());
        const long width_other = static_cast<long>(gen_.generate_moves(board, opponent(side_to_measure), 0, false).size());

        ++agg->positions;
        agg->cand_sum += static_cast<long>(domain.size());
        agg->forb_black_sum += forb_black;
        agg->forb_mover_sum += forb_mover;
        if (!domain.empty() && forb_mover == static_cast<long>(domain.size())) ++agg->no_move_positions;
        if (black_to_move) { agg->width_black += width_own; agg->width_white += width_other;
                             if (width_own == 0) ++agg->width_black_empty;
                             if (width_other == 0) ++agg->width_white_empty; }
        else { agg->width_white += width_own; agg->width_black += width_other;
               if (width_own == 0) ++agg->width_white_empty;
               if (width_other == 0) ++agg->width_black_empty; }

        const int band = Bands::index_of(board.move_count());
        ++agg->band_positions[band];
        agg->band_forb[band] += forb_black;
        agg->band_cand[band] += static_cast<long>(domain.size());
    }

private:
    MoveGenerator gen_;
};

void report(const char* title, const Aggregate& a, const Evaluator& ev) {
    (void)ev;
    if (a.positions == 0) { std::printf("%s：没有样本\n", title); return; }
    const double per = static_cast<double>(a.positions);
    std::printf("%s\n", title);
    std::printf("  局面=%ld 候选域均值=%.2f 黑若行棋被吃掉=%.2f (%.2f%%) 行棋方自己被吃掉=%.2f (%.2f%%)\n",
                a.positions, a.cand_sum / per, a.forb_black_sum / per,
                a.cand_sum ? 100.0 * a.forb_black_sum / a.cand_sum : 0.0,
                a.forb_mover_sum / per,
                a.cand_sum ? 100.0 * a.forb_mover_sum / a.cand_sum : 0.0);
    std::printf("  黑无路可走（候选全禁手）= %ld 个（%.2f%%）\n", a.no_move_positions,
                100.0 * a.no_move_positions / per);
    std::printf("  搜索真正拿到的宽度：黑=%.2f 白=%.2f ⇒ 黑/白 = %.3f（空列表：黑 %ld / 白 %ld）\n",
                a.width_black / per, a.width_white / per,
                a.width_white ? static_cast<double>(a.width_black) / a.width_white : 0.0,
                a.width_black_empty, a.width_white_empty);
    static const char* kBandName[5] = {" 4- 9 子", "10-19 子", "20-29 子", "30-39 子", " 40+ 子"};
    std::printf("  按手数分档的禁手挤压率：\n");
    for (int b = 0; b < 5; ++b) {
        if (a.band_positions[b] == 0) continue;
        std::printf("    %s：%3ld 个局面  候选 %ld 被吃 %ld ⇒ %.2f%%\n", kBandName[b],
                    a.band_positions[b], a.band_cand[b], a.band_forb[b],
                    a.band_cand[b] ? 100.0 * a.band_forb[b] / a.band_cand[b] : 0.0);
    }
}

Board board_of(const json& doc, bool* ok) {
    Board board;
    *ok = true;
    if (doc.contains("moves") && doc["moves"].is_array()) {
        int colour = BLACK;
        if (doc.value("first", std::string("black")) == "white") colour = WHITE;
        for (const auto& mv : doc["moves"]) {
            if (!mv.is_array() || mv.size() != 2) { *ok = false; break; }
            if (!board.place_stone(mv[0].get<int>(), mv[1].get<int>(), colour)) { *ok = false; break; }
            colour = opponent(colour);
        }
    }
    return board;
}

struct Outcome {
    long games = 0, black_wins = 0, white_wins = 0, draws = 0, unknown = 0;
    long reason_five = 0, reason_resign = 0, reason_draw = 0, reason_other = 0;
    long white_wins_with_late_no_move = 0;
    Aggregate black_plies, white_plies;
};

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> record_paths;
    std::string openings_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--records" && i + 1 < argc) record_paths.push_back(argv[++i]);
        else if (arg == "--openings" && i + 1 < argc) openings_path = argv[++i];
        else if (arg == "--help" || arg == "-h") {
            std::printf("用法: %s [--openings FILE.jsonl] [--records FILE.jsonl ...]\n", argv[0]);
            return 0;
        } else { std::fprintf(stderr, "不认识的参数: %s\n", arg.c_str()); return 1; }
    }

    Evaluator ev;
    Prober prober(ev);

    if (!openings_path.empty()) {
        Aggregate agg;
        std::ifstream in(openings_path);
        std::string line;
        long bad = 0, total = 0;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            ++total;
            json doc = json::parse(line, nullptr, false);
            if (doc.is_discarded()) { ++bad; continue; }
            bool ok = false;
            const Board board = board_of(doc, &ok);
            if (!ok) { ++bad; continue; }
            // 开局集按口径只收偶数子黑先行，所以「行棋方」恒为黑。
            prober.measure(board, BLACK, &agg);
        }
        char title[128];
        std::snprintf(title, sizeof title,
                      "A. 开局集的根（读 %ld 条，量到 %ld 个黑先行的盘面，解析/摆盘失败 %ld 条）",
                      total, agg.positions, bad);
        report(title, agg, ev);
    }

    Outcome oc;
    for (const auto& path : record_paths) {
        std::ifstream in(path);
        if (!in) { std::fprintf(stderr, "读不到 %s\n", path.c_str()); continue; }
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            json rec = json::parse(line, nullptr, false);
            if (rec.is_discarded() || !rec.contains("moves") || !rec["moves"].is_array()) continue;
            ++oc.games;
            const std::string winner = rec.value("winner", std::string(""));
            const std::string reason = rec.value("reason", std::string(""));
            if (winner == "black") ++oc.black_wins;
            else if (winner == "white") ++oc.white_wins;
            else if (winner == "draw") ++oc.draws;
            else ++oc.unknown;
            if (reason == "five") ++oc.reason_five;
            else if (reason == "resign") ++oc.reason_resign;
            else if (reason == "draw") ++oc.reason_draw;
            else ++oc.reason_other;

            Board board;
            std::vector<int> late_no_move_black;   // 每一手黑棋时的「候选全禁手」记 1
            int plies = 0;
            for (const auto& mv : rec["moves"]) {
                if (!mv.contains("row") || !mv.contains("color")) break;
                const std::string c = mv.value("color", std::string(""));
                const int colour = c == "black" ? BLACK : (c == "white" ? WHITE : EMPTY);
                if (colour == EMPTY) break;
                // 先在这手之前量行棋方，再落子。
                Aggregate* target = colour == BLACK ? &oc.black_plies : &oc.white_plies;
                const long before = target->no_move_positions;
                prober.measure(board, colour, target);
                if (colour == BLACK && target->no_move_positions > before)
                    late_no_move_black.push_back(plies);
                if (!board.place_stone(mv.value("row", -1), mv.value("col", -1), colour)) break;
                ++plies;
            }
            if (winner == "white" && !late_no_move_black.empty()) ++oc.white_wins_with_late_no_move;
        }
    }

    if (oc.games) {
        std::printf("\nB/C. 自对弈记录（%ld 局）\n", oc.games);
        std::printf("  胜方：黑 %ld / 白 %ld / 和 %ld / 其它 %ld ⇒ 黑胜率 %.4f（含和按半分=%.4f）\n",
                    oc.black_wins, oc.white_wins, oc.draws, oc.unknown,
                    oc.games ? static_cast<double>(oc.black_wins) / (oc.black_wins + oc.white_wins + 1e-9) : 0.0,
                    oc.games ? (oc.black_wins + 0.5 * oc.draws) / oc.games : 0.0);
        std::printf("  结束原因：成五 %ld / 认输 %ld / 和棋 %ld / 其它 %ld\n",
                    oc.reason_five, oc.reason_resign, oc.reason_draw, oc.reason_other);
        report("  黑方行棋的那一手（落子前盘面）：", oc.black_plies, ev);
        report("  白方行棋的那一手（落子前盘面）：", oc.white_plies, ev);
        std::printf("  白胜且该局里出现过「黑候选全被禁手吃掉」= %ld 局\n",
                    oc.white_wins_with_late_no_move);
    }
    return 0;
}

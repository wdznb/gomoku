#include "replay_analyzer.h"

#include "../search/search_engine.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>

namespace gomoku {

namespace {

// 本局的中位深度：段 A 用「比中位浅多少层」当「引擎当时不确定」的代理。
// 只用 depth > 0 的手 —— 0 是快路径（成五 / 挡五 / 开局库）的深度，把它们算进
// 中位数会把每一局的中位拉到 0，深度判据就永远不触发。
int median_depth(const GameRecord& record) {
    std::vector<int> depths;
    depths.reserve(record.moves.size());
    for (const MoveRecord& move : record.moves) {
        if (move.depth > 0) depths.push_back(move.depth);
    }
    if (depths.empty()) return 0;
    std::sort(depths.begin(), depths.end());
    return depths[depths.size() / 2];
}

int non_negative(int value) { return value < 0 ? 0 : value; }

// 将杀带的判定单独成函数，是因为它同时被三条判据用到（MATE_CROSS 要「穿越」，
// BIG_SWING 要「两侧都不在带内」）。写在判据里会让那条 if 长到读不懂。
bool in_mate_band(int score, int band) { return std::abs(score) >= band; }

bool mate_band_crossed(int from, int to, int band) {
    return in_mate_band(from, band) != in_mate_band(to, band);
}

}  // namespace

ReplayAnalyzer::ReplayAnalyzer(ScreenConfig config) : config_(config) {
    // 四个阈值都从命令行来（§5.3 的 analyze_*），负值在这里一次夹掉，免得
    // 「阈值 -1」这种输入让每条判据都恒真。
    config_.swing_threshold = non_negative(config_.swing_threshold);
    config_.opening_plies = non_negative(config_.opening_plies);
    config_.quiet_band = non_negative(config_.quiet_band);
    config_.depth_dip = non_negative(config_.depth_dip);
    config_.mate_band = non_negative(config_.mate_band);
}

const char* ReplayAnalyzer::reason_name(ScreenReason reason) {
    switch (reason) {
        case ScreenReason::NONE:           return "none";
        case ScreenReason::BIG_SWING:      return "big_swing";
        case ScreenReason::METHOD_SHIFT:   return "method_shift";
        case ScreenReason::MUST_BLOCK_RUN: return "must_block_run";
        case ScreenReason::DEPTH_DIP:      return "depth_dip";
        case ScreenReason::MATE_CROSS:     return "mate_cross";
        case ScreenReason::NOISE:          return "noise";
        case ScreenReason::OPENING:        return "opening";
        case ScreenReason::REPLAYED:       return "replayed";
    }
    return "none";
}

bool ReplayAnalyzer::is_tactical_method(const std::string& method) {
    return method == "must_block" || method == "forced" || method == "greedy_five" ||
           method == "vcf" || method == "vct";
}

bool ReplayAnalyzer::is_forced_method(const std::string& method) {
    return method == "must_block" || method == "forced";
}

int ReplayAnalyzer::swing_at(const GameRecord& record, int ply) {
    const int last = static_cast<int>(record.moves.size()) - 1;
    if (ply < 0 || ply >= last) return 0;
    return -record.moves[ply + 1].score - record.moves[ply].score;
}

GameScreen ReplayAnalyzer::screen(const GameRecord& record) const {
    GameScreen out;
    out.reasons.assign(kScreenReasonCount, 0);
    out.total_plies = static_cast<int>(record.moves.size());

    // 一条读不回来的记录连胜负都不可信（game_record.h 的 is_decisive 是同一条
    // 纪律），从它身上筛出来的手只会污染题库，所以一手都不产。
    if (record.reason == GameEndReason::INVALID) {
        out.flat = 1;
        return out;
    }

    const int median = median_depth(record);
    const int last = out.total_plies - 1;

    for (int i = 0; i < out.total_plies; ++i) {
        const MoveRecord& move = record.moves[i];
        const bool has_next = (i < last);

        PlyScreen ply;
        ply.ply = i;
        ply.side = move.move.color;
        ply.score = move.score;
        ply.depth = move.depth;
        ply.method = move.method;
        ply.next_score = has_next ? record.moves[i + 1].score : 0;
        ply.swing = swing_at(record, i);

        const bool tactical = is_tactical_method(move.method);
        const bool forced = is_forced_method(move.method);
        // 下一手是不是战术手。它要参与「无信息」的判定：一手安静的 alphabeta 紧接
        // 一手 vcf，摆幅是 0（两侧都在将杀带内），但那一刻正是战术层接管的位置，
        // 不能因为摆幅小就被判成没信息（REVIEW_LOG 轮次144 的第五个发现）。
        const bool next_tactical = has_next && is_tactical_method(record.moves[i + 1].method);

        // 判据的顺序：先剔除，再命中；命中里「具体的」压过「量大的」。
        //
        // 这个顺序不是拍出来的，是第一份真实记录逼出来的（REVIEW_LOG 轮次144）：
        // 一局 21 手里 vcf 与 must_block 各 4 手，而报告写着 method_shift 0 /
        // must_block_run 0 —— 因为战术手同时带着巨大摆幅，全被 BIG_SWING 抢走了
        // 标签。标签不是装饰：§6.3.2 给每种判定的训练价值不同，下游按 kind 加权。
        if (move.method == "replay" || move.method == "book") {
            // 重放的前缀与开局库都不含决策：前者是 RetryExplorer 为了走到某个分支
            // 重摆的，后者是查表。它们的 score 是 0 或上一手的残值，拿它算摆幅只会
            // 制造假阳性。
            ply.reason = ScreenReason::REPLAYED;
        } else if (i < config_.opening_plies && !tactical) {
            ply.reason = ScreenReason::OPENING;
        } else if (!tactical && !next_tactical && std::abs(ply.swing) < config_.quiet_band &&
                   (median == 0 || ply.depth >= median)) {
            // 三件事同时成立才算「无意义」：摆幅落在静默带里、没有战术层参与、
            // 深度不低于本局中位。第三条是段 A 对 §6.4 判据 2 那个 gap 的替代
            // ——gap 要 root_scores，那是段 B 的事。
            ply.reason = ScreenReason::NOISE;
        } else if (has_next && forced && is_forced_method(record.moves[i + 1].method)) {
            ply.reason = ScreenReason::MUST_BLOCK_RUN;
        } else if (has_next && tactical != is_tactical_method(record.moves[i + 1].method)) {
            ply.reason = ScreenReason::METHOD_SHIFT;
        } else if (has_next && mate_band_crossed(ply.score, ply.next_score, config_.mate_band)) {
            ply.reason = ScreenReason::MATE_CROSS;
        } else if (has_next && !in_mate_band(ply.score, config_.mate_band) &&
                   !in_mate_band(ply.next_score, config_.mate_band) &&
                   std::abs(ply.swing) >= config_.swing_threshold) {
            // 两侧都在将杀带内时，摆幅是「饱和分减饱和分」的假差值（实测见到过
            // -199000：100000 -> 99000），不是形势的摆幅，所以这条判据不许响。
            ply.reason = ScreenReason::BIG_SWING;
        } else if (!tactical && median > 0 && ply.depth + config_.depth_dip <= median) {
            // 战术手不参与深度判据：vcf / must_block 的 depth 是 0，那是结构性的
            // （它们根本不进 alphabeta），不是「引擎当时不确定」。
            ply.reason = ScreenReason::DEPTH_DIP;
        }

        ply.candidate = ply.reason != ScreenReason::NONE &&
                        ply.reason != ScreenReason::NOISE &&
                        ply.reason != ScreenReason::OPENING &&
                        ply.reason != ScreenReason::REPLAYED;
        if (ply.reason == ScreenReason::NOISE) ++out.noise;
        if (ply.reason == ScreenReason::OPENING) ++out.opening;
        if (ply.reason == ScreenReason::REPLAYED) ++out.replayed;
        if (ply.candidate) ++out.candidates;
        ++out.reasons[static_cast<int>(ply.reason)];
        out.plies.push_back(ply);
    }

    // §6.4 的 flat_game：候选手少于 2 手的一局不产题卡。0 手与 1 手都算平。
    out.flat = out.candidates < 2 ? 1 : 0;
    return out;
}

namespace {

bool same_cell(const Move& a, const Move& b) {
    return a.pos.is_valid() && b.pos.is_valid() && a.pos.row == b.pos.row && a.pos.col == b.pos.col;
}

}  // namespace

ReplayReviewer::ReplayReviewer(const ThreatCoefficients& coeffs, ReviewConfig config)
    : coeffs_(coeffs), config_(config) {
    if (config_.budget <= 0) config_.budget = 20000;
    if (config_.max_depth < 1) config_.max_depth = 8;
    if (config_.ladder_min < 1000) config_.ladder_min = 1000;
    if (config_.ladder_max < config_.ladder_min) config_.ladder_max = config_.ladder_min;
}

bool ReplayReviewer::position_before(const GameRecord& record, int ply, Board& out) const {
    out = Board();
    const int limit = std::min(ply, static_cast<int>(record.moves.size()));
    for (int i = 0; i < limit; ++i) {
        const Move& move = record.moves[i].move;
        if (!move.pos.is_valid() || !out.is_empty(move.pos)) return false;
        out.place_stone(move.pos.row, move.pos.col, move.color);
    }
    return true;
}

PlyReview ReplayReviewer::review(const GameRecord& record, int ply) const {
    PlyReview review;
    review.ply = ply;
    if (ply < 0 || ply >= static_cast<int>(record.moves.size())) return review;

    Board board;
    if (!position_before(record, ply, board)) return review;

    const Move played = record.moves[ply].move;
    int side = played.color;
    if (side == EMPTY) side = (ply % 2 == 0) ? BLACK : WHITE;
    review.ply = ply;
    return review_board(board, side, played, record.moves[ply].score);
}

PlyReview ReplayReviewer::review_board(const Board& board, int side, Move played,
                                       int record_score) const {
    PlyReview review;
    review.side = side;
    review.played = played;
    review.budget = config_.budget;

    SearchEngine engine(coeffs_);
    engine.set_book_enabled(false);
    engine.set_max_nodes(config_.budget);
    std::atomic<bool> stop{false};
    // 时限必须给一个不 binding 的值：轮次121 B5 量到 time_limit_ms = 0 会被兜到
    // 100 ms，那时层切割点跟着机器忙闲走，两次复核就不可比了。
    const SearchResult result = engine.search(board, side, config_.max_depth, 60000, stop);

    review.method = result.method;
    review.depth = result.depth;
    review.root_moves = static_cast<int>(result.root_scores.size());
    if (result.root_scores.empty()) {
        // 快路径与战术层不给根表：它们的答案不是「比出来的」，段 B 对它们没话说。
        review.kind = "none";
        return review;
    }

    const RootMoveScore& top = result.root_scores.front();
    review.best = top.move;

    const RootMoveScore* played_entry = nullptr;
    for (const RootMoveScore& entry : result.root_scores) {
        if (same_cell(entry.move, played)) {
            played_entry = &entry;
            break;
        }
    }
    if (played_entry == nullptr) {
        // 走的那一手不在根表里：被候选宽度裁掉、或根本是禁手。它不是「差多少」，
        // 是「引擎当时没考虑」，所以单独一个 kind，不许并进俗手。
        review.unlisted = true;
        review.kind = "unlisted";
        review.best_score = top.score;
        return review;
    }

    // 精确化（§6.3.1 的补充）：根表里排在后面的着法带的是界，不是真值——第一版
    // 直接拿表里的分数做差，结果 6 手全是 gap 0、loss 0，因为失败低返回的界正好
    // 等于当时的 alpha。判「差多少」之前，最好的与走的那手各付一次精确复算。
    auto exact_of = [&](const RootMoveScore& entry) {
        if (entry.exact) return entry.score;
        return engine.exact_root_score(board, side, entry.move, config_.max_depth,
                                       config_.budget, stop);
    };
    review.best_score = exact_of(top);
    review.played_score = exact_of(*played_entry);
    review.loss = review.best_score - review.played_score;

    // 次优只在「有精确值」的着法里找。拿界当次优会让 gap 恒等于 0，而 gap 是妙手
    // 的唯一证据；没有证据就不宣称妙手（§6.3.2）。
    int safe_moves = 1;  // 最好的那一手自己安全
    for (const RootMoveScore& entry : result.root_scores) {
        if (same_cell(entry.move, top.move)) continue;
        if (!entry.exact) continue;
        if (!review.second.pos.is_valid()) {
            review.second = entry.move;
            review.second_score = entry.score;
        }
        if (review.best_score - entry.score < config_.forced_margin) ++safe_moves;
    }
    review.gap = review.second.pos.is_valid() ? review.best_score - review.second_score : 0;

    const bool mate = std::abs(review.best_score) >= 90000;
    if (mate && !same_cell(played, review.best)) {
        review.kind = "missed_mate";
    } else if (record_score >= 20000 && review.best_score <= -90000) {
        // 引擎当时说自己 +20000，复核说这局面是输的：反例的价值最高（§6.3.2）。
        review.kind = "counterexample";
    } else if (review.loss >= config_.blunder_swing) {
        review.kind = "blunder";
    } else if (same_cell(played, review.best) && review.gap >= config_.brilliant_gap &&
               safe_moves > 1) {
        // 「不是唯一应手」这一条不能省：只有一步能走的局面里走对了叫被迫正确，
        // 算进妙手会让题库塞满 must_block，solve_rate 立刻饱和（§6.3.2）。
        review.kind = "brilliant";
    } else if (safe_moves == 1 && review.second.pos.is_valid()) {
        // 「唯一应手」要有证据：至少一手别的着法有精确分、且它不安全。没有证据时
        // safe_moves 恒为 1（根表尾部只有界），那样每条都会变成 forced —— 第一版
        // 就是这么错的，12 手全判成了唯一应手。
        review.kind = "forced";
    } else {
        review.kind = "none";
    }
    return review;
}

LadderResult ReplayReviewer::difficulty(const Board& board, int side, Move answer) const {
    LadderResult ladder;
    int last_failed = 0;
    for (int budget = config_.ladder_min;; budget *= 2) {
        SearchEngine engine(coeffs_);
        engine.set_book_enabled(false);
        engine.set_max_nodes(budget);
        std::atomic<bool> stop{false};
        const SearchResult result = engine.search(board, side, config_.max_depth, 60000, stop);
        const bool solved = !result.root_scores.empty() &&
                            same_cell(result.root_scores.front().move, answer);
        if (solved) {
            ladder.solve_budget = budget;
            ladder.failed_at = last_failed;
            return ladder;
        }
        last_failed = budget;
        if (budget >= config_.ladder_max) break;
    }
    ladder.solve_budget = 0;
    ladder.failed_at = last_failed;
    return ladder;
}

}

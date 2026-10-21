#include "self_play.h"
#include "../core/pattern.h"
#include "../core/transforms.h"

namespace gomoku {

std::vector<Move> legal_moves(const Board& board, int color) {
    std::vector<Move> moves;
    for (const auto& pos : board.active_empty()) {
        if (!board.is_empty(pos)) continue;
        if (color == BLACK && PatternRecognizer::is_forbidden(board, pos.row, pos.col, BLACK)) {
            continue;
        }
        moves.emplace_back(pos, color);
    }
    return moves;
}

SelfPlay::SelfPlay(SearchEngine& black_engine, SearchEngine& white_engine,
                   const SelfPlayConfig& config)
    : black_engine_(black_engine), white_engine_(white_engine), config_(config) {
    // search() names only a depth and a millisecond limit, so the engine field is
    // the only route a node ceiling has into the next think. A non-positive value
    // clamps to 0 inside the setter, which is the previous behavior.
    black_engine_.set_max_nodes(config_.max_nodes);
    white_engine_.set_max_nodes(config_.max_nodes);
    // Design doc 2.3.4 step 5 says the time budget does not participate in 训练模式, and
    // this is the one place every training think passes - data games, verification pairs
    // and fitness games all come through here. The node ceiling alone could not say it:
    // `--timeout-ms` with no `--max-nodes` is the default (self_play.h:15-18), and a soft
    // window shaped from that wall would leave each collected sample searched to a depth
    // the round did not ask for.
    black_engine_.set_training(true);
    white_engine_.set_training(true);
}

GameRecord SelfPlay::run(const std::vector<Move>& prefix,
                         std::vector<PlyChoices>* out_choices,
                         std::atomic<bool>* stop_flag) {
    std::atomic<bool> unused_stop(false);
    std::atomic<bool>& flag = stop_flag ? *stop_flag : unused_stop;
    // 丁-2：训练口径由这一处声明，不靠引擎自己猜（与上面给学生的那两句同一条理由）——
    // 老师的 think 也发生在训练对局里，它不该被软窗口塑形，只认自己的节点帽与硬时限。
    if (teacher_ != nullptr && config_.teacher_depth > 0) teacher_->set_training(true);

    Board board;
    GameRecord record;

    for (const auto& move : prefix) {
        const int expected = board.move_count() % 2 == 0 ? BLACK : WHITE;
        const bool illegal = move.color != expected ||
                             !board.is_empty(move.pos) ||
                             (move.color == BLACK &&
                              PatternRecognizer::is_forbidden(board, move.pos.row, move.pos.col, BLACK));
        if (illegal) {
            record.winner = EMPTY;
            record.reason = GameEndReason::INVALID;
            return record;
        }

        board.place_stone(move.pos, move.color);
        MoveRecord entry;
        entry.move = move;
        entry.method = "replay";
        record.moves.push_back(entry);

        if (board.has_five_in_a_row(move.pos.row, move.pos.col, move.color)) {
            record.winner = move.color;
            record.reason = GameEndReason::FIVE;
            return record;
        }
    }

    while (!flag.load() && record.moves.size() < static_cast<size_t>(config_.max_moves) &&
           board.move_count() < MAX_MOVES) {
        const int color = board.move_count() % 2 == 0 ? BLACK : WHITE;
        SearchEngine& engine = color == BLACK ? black_engine_ : white_engine_;

        SearchResult result = engine.search(board, color, config_.max_depth,
                                           config_.timeout_ms, flag);

        if (result.resigned) {
            record.winner = opponent(color);
            record.reason = GameEndReason::RESIGN;
            return record;
        }

        const Move chosen = result.best_move;
        const bool illegal = chosen.color != color ||
                             !board.is_empty(chosen.pos) ||
                             (color == BLACK &&
                              PatternRecognizer::is_forbidden(board, chosen.pos.row, chosen.pos.col, BLACK));
        if (illegal) {
            // Playing an illegal move would corrupt every sample after it, so the
            // game is discarded instead of patched with a filler move.
            record.winner = EMPTY;
            record.reason = GameEndReason::INVALID;
            return record;
        }

        if (out_choices) {
            PlyChoices choices;
            choices.chosen = chosen;
            choices.chosen_key = canonical_cell(board, chosen.pos);

            const std::vector<Move> candidates = legal_moves(board, color);
            std::set<int> seen;
            seen.insert(choices.chosen_key);
            for (const auto& candidate : candidates) {
                const int key = canonical_cell(board, candidate.pos);
                if (!seen.insert(key).second) continue;
                choices.alternatives.push_back(candidate);
                choices.alternative_keys.push_back(key);
            }
            out_choices->push_back(choices);
        }

        // 丁-2：老师在**落子之前**搜同一个局面（§4 丁-2 注意 2——落子后局面就变了），
        // 用调用方交来的独立引擎与独立置换表（注意 1）。它的答案只多记三个字段，
        // 不参与这一局下哪一手，也不进学生的节点/深度账。
        Move teacher_answer;
        int teacher_depth = 0;
        int teacher_score = 0;
        if (teacher_ != nullptr && config_.teacher_depth > 0) {
            teacher_->set_max_nodes(config_.teacher_max_nodes);
            // 同一只 stop flag：整轮被叫停时老师不能自己再跑完一次深搜。
            const SearchResult taught = teacher_->search(board, color, config_.teacher_depth,
                                                        config_.timeout_ms, flag);
            // 老师不做认输判定：认输会不带 best_move 回来，那样这一手就没有答案可记。
            if (!taught.resigned && taught.best_move.pos.is_valid()) {
                teacher_answer = taught.best_move;
                // 注意 3：记的是请求的深度。vcf / vct / must_block 的 `result.depth`
                // 是 0，那是结构性的，不是「老师只看了 0 层」。
                teacher_depth = config_.teacher_depth;
                teacher_score = taught.score;
            }
        }

        board.place_stone(chosen.pos, color);
        // Per committed ply, not per think: the records this ply's search wrote stay
        // in the table for the rest of the game, so a transposition later in the same
        // game can still find them.
        engine.transposition_table().advance_generation();

        MoveRecord entry;
        entry.move = chosen;
        entry.score = result.score;
        entry.depth = result.depth;
        entry.nodes = result.nodes_searched;
        entry.elapsed_ms = result.elapsed_ms;
        entry.method = result.method;
        entry.partial_depth = result.partial_depth;
        entry.root_moves_done = result.root_moves_done;
        entry.root_moves_total = result.root_moves_total;
        entry.seldepth = result.seldepth;
        entry.stopped = result.stopped;
        entry.tt_hits = result.tt_hits;
        entry.teacher_move = teacher_answer;
        entry.teacher_depth = teacher_depth;
        entry.teacher_score = teacher_score;
        record.moves.push_back(entry);

        record.total_nodes += result.nodes_searched;
        if (result.depth > record.max_depth) record.max_depth = result.depth;

        if (board.has_five_in_a_row(chosen.pos.row, chosen.pos.col, color)) {
            record.winner = color;
            record.reason = GameEndReason::FIVE;
            return record;
        }
    }

    record.winner = EMPTY;
    if (board.move_count() >= MAX_MOVES) {
        record.reason = GameEndReason::BOARD_FULL;
    } else if (flag.load()) {
        // Stopped mid-game: the record is not a finished line, so nothing may read
        // an outcome out of it.
        record.reason = GameEndReason::INVALID;
    } else {
        record.reason = GameEndReason::MOVE_LIMIT;
    }
    return record;
}

RetryExplorer::RetryExplorer(SearchEngine& black_engine, SearchEngine& white_engine,
                             const SelfPlayConfig& config)
    : self_play_(black_engine, white_engine, config) {}

void RetryExplorer::reset() {
    // 丁-2 注意 4：以前这里把 prefix_ 一起清空，于是 worker 只在每轮第一次设过的开局
    // 只作用于第一局——重试的那几局是从空盘开始的，「悔棋再战」退到的其实是另一棵树。
    // 现在 reset 回到起始线，树照样丢（它属于上一条线）。
    prefix_ = start_line_;
    choices_.clear();
    tried_.clear();
    exhausted_ = false;
}

void RetryExplorer::set_start_line(const std::vector<Move>& line) {
    start_line_ = line;
    prefix_ = line;
    choices_.clear();
    tried_.clear();
    exhausted_ = false;
}

bool RetryExplorer::explore(GameRecord& out, std::atomic<bool>* stop_flag) {
    if (exhausted_) return false;

    const size_t replayed = prefix_.size();
    std::vector<PlyChoices> tail;
    GameRecord record = self_play_.run(prefix_, &tail, stop_flag);
    games_played_++;

    if (record.reason == GameEndReason::INVALID) {
        // Either the engine misbehaved or the external stop was raised; retrying
        // the same line would just produce the same unusable record.
        exhausted_ = true;
        out = record;
        return true;
    }

    for (size_t k = 0; k < tail.size(); ++k) {
        const size_t ply = replayed + k;
        if (choices_.size() <= ply) {
            choices_.resize(ply + 1);
            tried_.resize(ply + 1);
        }
        choices_[ply] = tail[k];
        tried_[ply].insert(tail[k].chosen_key);
    }

    std::vector<Move> played;
    played.reserve(record.moves.size());
    for (const auto& entry : record.moves) played.push_back(entry.move);

    out = record;
    if (!stop_flag || !stop_flag->load()) {
        if (!rewind_to_next_branch(played)) exhausted_ = true;
    }
    return true;
}

// Retreat to the latest ply that still has a branch nobody has walked, keeping
// everything before it. Plies after the divergence belong to a different
// position, so their records are dropped rather than reused.
bool RetryExplorer::rewind_to_next_branch(const std::vector<Move>& played) {
    const int max_moves = self_play_.config().max_moves;
    for (int ply = static_cast<int>(played.size()) - 1; ply >= 0; --ply) {
        if (static_cast<size_t>(ply) >= choices_.size()) continue;
        // A branch point one ply short of the move cap replays a whole game
        // and lets the engine decide nothing - skip it and go shallower.
        if (ply + 3 > max_moves) continue;
        const PlyChoices& choices = choices_[ply];
        std::set<int>& already_tried = tried_[ply];

        for (size_t j = 0; j < choices.alternatives.size(); ++j) {
            if (already_tried.count(choices.alternative_keys[j]) != 0) continue;

            already_tried.insert(choices.alternative_keys[j]);
            prefix_.assign(played.begin(), played.begin() + ply);
            prefix_.push_back(choices.alternatives[j]);
            choices_.resize(ply + 1);
            tried_.resize(ply + 1);
            return true;
        }
    }
    return false;
}

}

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
    : black_engine_(black_engine), white_engine_(white_engine), config_(config) {}

GameRecord SelfPlay::run(const std::vector<Move>& prefix,
                         std::vector<PlyChoices>* out_choices,
                         std::atomic<bool>* stop_flag) {
    std::atomic<bool> unused_stop(false);
    std::atomic<bool>& flag = stop_flag ? *stop_flag : unused_stop;

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

        board.place_stone(chosen.pos, color);

        MoveRecord entry;
        entry.move = chosen;
        entry.score = result.score;
        entry.depth = result.depth;
        entry.nodes = result.nodes_searched;
        entry.elapsed_ms = result.elapsed_ms;
        entry.method = result.method;
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
    prefix_.clear();
    choices_.clear();
    tried_.clear();
    exhausted_ = false;
}

void RetryExplorer::set_start_line(const std::vector<Move>& line) {
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
    for (int ply = static_cast<int>(played.size()) - 1; ply >= 0; --ply) {
        if (static_cast<size_t>(ply) >= choices_.size()) continue;
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

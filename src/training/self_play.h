#ifndef GOMOKU_SELF_PLAY_H
#define GOMOKU_SELF_PLAY_H

#include "../core/board.h"
#include "../search/search_engine.h"
#include "game_record.h"
#include <atomic>
#include <set>
#include <vector>

namespace gomoku {

struct SelfPlayConfig {
    int max_depth = 4;
    int timeout_ms = 400;
    // Nodes one think may spend, armed on both engines when a SelfPlay is built.
    // 0 leaves each think on the ceiling the remaining time derives.
    int max_nodes = 0;
    // A training game usually ends long before the board fills; without a cap a
    // mirrored, never-progressing pair could run for 225 plies per sample.
    int max_moves = 80;
};

// Every move `color` may legally play in `board`: empty cells near the live
// area, with BLACK's forbidden shapes removed.
std::vector<Move> legal_moves(const Board& board, int color);

// A single decision point. The alternative keys are canonical cell indices, so
// two mirror-image replies count as one branch.
struct PlyChoices {
    Move chosen;
    int chosen_key = -1;
    std::vector<Move> alternatives;
    std::vector<int> alternative_keys;
};

// Plays complete games between two engine configurations and writes them out as
// records. The engines are used by reference and are NOT thread safe - give each
// training thread its own pair.
class SelfPlay {
public:
    SelfPlay(SearchEngine& black_engine, SearchEngine& white_engine,
             const SelfPlayConfig& config);

    // Replays `prefix`, then lets the engines decide the rest. `out_choices`,
    // when supplied, receives one PlyChoices per ply the engines decided, in
    // order, starting at index prefix.size().
    GameRecord run(const std::vector<Move>& prefix,
                   std::vector<PlyChoices>* out_choices,
                   std::atomic<bool>* stop_flag);

    const SelfPlayConfig& config() const { return config_; }

private:
    SearchEngine& black_engine_;
    SearchEngine& white_engine_;
    SelfPlayConfig config_;
};

// 悔棋再战. Repeatedly plays games that differ as late as possible: after each
// game it retreats to the deepest ply that still has an untried alternative,
// keeps every move before it, and continues from there. Symmetric branches are
// treated as already explored, so the retry budget goes to genuinely different
// positions.
class RetryExplorer {
public:
    RetryExplorer(SearchEngine& black_engine, SearchEngine& white_engine,
                  const SelfPlayConfig& config);

    // Plays one game from the current line. Returns false when the tree under
    // this line has no untried branch left and `out` is untouched.
    bool explore(GameRecord& out, std::atomic<bool>* stop_flag);

    bool exhausted() const { return exhausted_; }
    int games_played() const { return games_played_; }
    const std::vector<Move>& current_line() const { return prefix_; }

    // Begin from a given legal opening instead of the empty board. The recorded
    // branch tree belongs to the previous line, so it is dropped with it.
    void set_start_line(const std::vector<Move>& line);

    // Forgets the tree, so the next explore() starts a fresh game.
    void reset();

private:
    bool rewind_to_next_branch(const std::vector<Move>& played);

    SelfPlay self_play_;
    std::vector<Move> prefix_;
    std::vector<PlyChoices> choices_;
    std::vector<std::set<int>> tried_;
    bool exhausted_ = false;
    int games_played_ = 0;
};

}

#endif

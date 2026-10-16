#ifndef GOMOKU_GAME_RECORD_H
#define GOMOKU_GAME_RECORD_H

#include "../core/types.h"
#include <string>
#include <vector>

namespace gomoku {

// Why a recorded game stopped. Mirrors the reasons the server reports, plus the
// two endings only a training run produces.
enum class GameEndReason {
    FIVE,          // a player made five in a row
    FORBIDDEN,     // BLACK played a forbidden shape
    RESIGN,        // a player proved the position lost and resigned
    BOARD_FULL,
    MOVE_LIMIT,    // the game was cut off while still undecided
    INVALID,       // the record could not be replayed legally
};

// One ply as it was searched. The last five fields are the layered accounting a
// think reports (how far it got before it stopped, and why); a record written
// before they existed has no keys for them and reads back as a think that
// finished at the depth and node count it states.
struct MoveRecord {
    Move move;
    int score = 0;
    int depth = 0;
    int nodes = 0;
    int elapsed_ms = 0;
    std::string method;
    int partial_depth = 0;
    int root_moves_done = 0;
    int root_moves_total = 0;
    int seldepth = 0;
    std::string stopped = "none";
};

struct GameRecord {
    std::vector<MoveRecord> moves;
    int winner = EMPTY;
    GameEndReason reason = GameEndReason::INVALID;
    int black_profile = -1;
    int white_profile = -1;
    int total_nodes = 0;
    int max_depth = 0;

    // A record that cannot be replayed carries no readable result, even if a
    // winner survived in the file, so nothing downstream can score it.
    bool is_decisive() const {
        return winner != EMPTY && reason != GameEndReason::MOVE_LIMIT &&
               reason != GameEndReason::INVALID;
    }

    // Outcome seen by `color`: +1 won, -1 lost, 0 undecided.
    float outcome_for(int color) const {
        if (!is_decisive()) return 0.0f;
        return winner == color ? 1.0f : -1.0f;
    }

    std::string reason_name() const;
    static GameEndReason reason_from_name(const std::string& name);

    std::string to_json() const;
    static bool from_json(const std::string& text, GameRecord& out, std::string& error);

    bool save(const std::string& path, std::string& error) const;
    static bool load(const std::string& path, GameRecord& out, std::string& error);

    // One JSON object per line, appended, so a long training run can keep
    // accumulating records without rewriting the file.
    bool append_to_jsonl(const std::string& path, std::string& error) const;
};

}

#endif

#ifndef GOMOKU_SESSION_STORE_H
#define GOMOKU_SESSION_STORE_H

#include "../training/game_record.h"
#include <string>
#include <vector>

namespace gomoku {

// One server session as it sits on disk: the game itself plus what is needed to
// hand it back to the same browser tab, or to a server that restarted in between.
// The record is reused from the training side verbatim, so a saved session is
// also a training sample with no conversion step.
struct SessionSnapshot {
    std::string token;
    long long created_ms = 0;
    long long updated_ms = 0;
    // "" while nobody has picked a side yet.
    std::string player_color;
    std::string status;  // waiting / playing / game_over
    std::string winner;  // "" while the game is undecided
    std::string reason;
    bool position_pending = false;
    int timeout_ms = 0;
    int max_depth = 0;
    GameRecord record;
    // Stones a hand-set position installed that are still on the board, which
    // record.moves does not cover. Empty for a game that started from an empty
    // board - and only such a game has a record that can be replayed on its own,
    // which is what decides whether it is worth exporting for training.
    std::vector<Move> base;
};

// Tokens double as file names, so they are restricted to lower-case hex. Anything
// else could walk out of the session directory once it is concatenated.
bool valid_token(const std::string& token);

std::string session_path(const std::string& dir, const std::string& token);

std::string session_to_json(const SessionSnapshot& snap);
bool session_from_json(const std::string& text, SessionSnapshot& out, std::string& error);

// Writes <dir>/<token>.json through a temporary file and a rename, so a process
// killed mid-write leaves the previous snapshot readable rather than half a file.
bool save_session(const std::string& dir, const SessionSnapshot& snap, std::string& error);
bool load_session(const std::string& dir, const std::string& token, SessionSnapshot& out,
                  std::string& error);
// Paths of every <dir>/*.json, sorted, so a restart can re-admit live sessions.
std::vector<std::string> list_sessions(const std::string& dir);
bool delete_session(const std::string& dir, const std::string& token, std::string& error);

}  // namespace gomoku

#endif

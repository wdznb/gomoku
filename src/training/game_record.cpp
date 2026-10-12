#include "game_record.h"
#include "json_int.h"
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

namespace gomoku {

namespace {

const char* color_name(int color) {
    if (color == BLACK) return "black";
    if (color == WHITE) return "white";
    return "draw";
}

bool color_from_name(const std::string& name, int& out) {
    if (name == "black") { out = BLACK; return true; }
    if (name == "white") { out = WHITE; return true; }
    if (name == "draw")  { out = EMPTY; return true; }
    return false;
}

}

std::string GameRecord::reason_name() const {
    switch (reason) {
        case GameEndReason::FIVE:        return "five";
        case GameEndReason::FORBIDDEN:   return "forbidden";
        case GameEndReason::RESIGN:      return "resign";
        case GameEndReason::BOARD_FULL:  return "board_full";
        case GameEndReason::MOVE_LIMIT:  return "move_limit";
        case GameEndReason::INVALID:     break;
    }
    return "invalid";
}

GameEndReason GameRecord::reason_from_name(const std::string& name) {
    if (name == "five")       return GameEndReason::FIVE;
    if (name == "forbidden")  return GameEndReason::FORBIDDEN;
    if (name == "resign")     return GameEndReason::RESIGN;
    if (name == "board_full") return GameEndReason::BOARD_FULL;
    if (name == "move_limit") return GameEndReason::MOVE_LIMIT;
    return GameEndReason::INVALID;
}

std::string GameRecord::to_json() const {
    json root;
    root["winner"] = color_name(winner);
    root["reason"] = reason_name();
    root["black_profile"] = black_profile;
    root["white_profile"] = white_profile;
    root["total_nodes"] = total_nodes;
    root["max_depth"] = max_depth;

    json move_array = json::array();
    for (const auto& record : moves) {
        move_array.push_back({
            {"row", record.move.pos.row},
            {"col", record.move.pos.col},
            {"color", color_name(record.move.color)},
            {"score", record.score},
            {"depth", record.depth},
            {"nodes", record.nodes},
            {"elapsed_ms", record.elapsed_ms},
            {"method", record.method},
        });
    }
    root["moves"] = move_array;
    return root.dump();
}

bool GameRecord::from_json(const std::string& text, GameRecord& out, std::string& error) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::exception& e) {
        error = std::string("invalid game record JSON: ") + e.what();
        return false;
    }

    if (!root.is_object() || !root.contains("moves") || !root["moves"].is_array()) {
        error = "game record must be an object with a moves array";
        return false;
    }

    GameRecord record;
    // Winner and end reason are outcomes, not metadata: defaulting them would
    // turn a win into a draw in every sample derived from the record.
    if (!root.contains("winner") || !root["winner"].is_string()) {
        error = "game record must state winner as black, white or draw";
        return false;
    }
    if (!color_from_name(root["winner"].get<std::string>(), record.winner)) {
        error = "unknown winner colour in game record";
        return false;
    }
    if (!root.contains("reason") || !root["reason"].is_string()) {
        error = "game record must state reason as an end reason";
        return false;
    }
    const std::string reason = root["reason"].get<std::string>();
    record.reason = reason_from_name(reason);
    // reason_from_name maps any unmatched name to INVALID, so a miss has to be
    // told apart from "invalid", which is itself one of the written names.
    if (record.reason == GameEndReason::INVALID && reason != "invalid") {
        error = "unknown end reason in game record: " + reason;
        return false;
    }
    auto read_metadata = [&root](const char* key, int fallback) {
        int value = fallback;
        return root.contains(key) && read_json_int(root[key], value) ? value : fallback;
    };
    record.black_profile = read_metadata("black_profile", record.black_profile);
    record.white_profile = read_metadata("white_profile", record.white_profile);
    record.total_nodes = read_metadata("total_nodes", record.total_nodes);
    record.max_depth = read_metadata("max_depth", record.max_depth);

    for (const auto& entry : root["moves"]) {
        if (!entry.is_object()) {
            error = "game record move entry is not an object";
            return false;
        }
        auto read_int = [&entry](const char* key, int fallback) {
            int value = fallback;
            return entry.contains(key) && read_json_int(entry[key], value) ? value : fallback;
        };

        MoveRecord move_record;
        // Unusable coordinates fall back to -1 and are rejected below, rather than
        // being truncated into some other square on the board.
        move_record.move = Move(read_int("row", -1), read_int("col", -1), EMPTY);
        if (!move_record.move.pos.is_valid()) {
            error = "game record move is off the board";
            return false;
        }
        std::string colour = entry.contains("color") && entry["color"].is_string()
                                 ? entry["color"].get<std::string>() : "";
        if (!color_from_name(colour, move_record.move.color) ||
            move_record.move.color == EMPTY) {
            error = "game record move has an unknown colour";
            return false;
        }
        move_record.score = read_int("score", 0);
        move_record.depth = read_int("depth", 0);
        move_record.nodes = read_int("nodes", 0);
        move_record.elapsed_ms = read_int("elapsed_ms", 0);
        move_record.method = entry.contains("method") && entry["method"].is_string()
                                 ? entry["method"].get<std::string>() : "";
        record.moves.push_back(move_record);
    }

    out = record;
    return true;
}

bool GameRecord::save(const std::string& path, std::string& error) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        error = "cannot write " + path;
        return false;
    }
    file << to_json();
    // Buffered writes report a full disk only at close, after this check would
    // have called the file saved.
    file.flush();
    if (!file.good()) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

bool GameRecord::load(const std::string& path, GameRecord& out, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "cannot open " + path;
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    return from_json(text, out, error);
}

bool GameRecord::append_to_jsonl(const std::string& path, std::string& error) const {
    std::ofstream file(path, std::ios::app);
    if (!file.is_open()) {
        error = "cannot append to " + path;
        return false;
    }
    file << to_json() << "\n";
    // The server marks the game exported on this return value, so a failure that
    // the buffer only reveals at close would silently drop the record.
    file.flush();
    if (!file.good()) {
        error = "failed while appending to " + path;
        return false;
    }
    return true;
}

}

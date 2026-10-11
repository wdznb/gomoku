#include "session_store.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

using nlohmann::json;

namespace gomoku {
namespace {

const char* kTmpSuffix = ".tmp";

const char* color_name(int color) {
    return color == BLACK ? "black" : color == WHITE ? "white" : "empty";
}

// "black" / "white", or EMPTY for anything else so the caller can reject it.
int color_of(const std::string& name) {
    if (name == "black") return BLACK;
    if (name == "white") return WHITE;
    return EMPTY;
}

bool read_file(const std::string& path, std::string& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

bool valid_token(const std::string& token) {
    if (token.empty() || token.size() > 64) return false;
    for (char ch : token) {
        const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        if (!hex) return false;
    }
    return true;
}

std::string session_path(const std::string& dir, const std::string& token) {
    if (!valid_token(token)) return std::string();
    return dir + "/" + token + ".json";
}

std::string session_to_json(const SessionSnapshot& snap) {
    json root;
    root["version"] = 1;
    root["type"] = "gomoku_session";
    root["token"] = snap.token;
    root["created_ms"] = snap.created_ms;
    root["updated_ms"] = snap.updated_ms;
    root["player_color"] = snap.player_color;
    root["status"] = snap.status;
    root["winner"] = snap.winner;
    root["reason"] = snap.reason;
    root["position_pending"] = snap.position_pending;
    root["timeout_ms"] = snap.timeout_ms;
    root["max_depth"] = snap.max_depth;
    // GameRecord owns its own JSON; embedding the parsed object keeps one schema
    // for both the session file and the training record it also produces.
    root["record"] = json::parse(snap.record.to_json());
    json base_array = json::array();
    for (const Move& stone : snap.base) {
        base_array.push_back({{"row", stone.pos.row},
                              {"col", stone.pos.col},
                              {"color", color_name(stone.color)}});
    }
    root["base"] = base_array;
    return root.dump(2) + "\n";
}

bool session_from_json(const std::string& text, SessionSnapshot& out, std::string& error) {
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded()) {
        error = "会话文件不是合法 JSON";
        return false;
    }
    if (!root.is_object() || !root.contains("version") || !root["version"].is_number_integer() ||
        root["version"].get<int>() != 1) {
        error = "会话文件版本不认识";
        return false;
    }
    if (!root.contains("token") || !root["token"].is_string() ||
        !valid_token(root["token"].get<std::string>())) {
        error = "会话文件缺少合法 token";
        return false;
    }
    if (!root.contains("record") || !root["record"].is_object()) {
        error = "会话文件没有 record 对象";
        return false;
    }

    SessionSnapshot snap;
    snap.token = root["token"].get<std::string>();
    auto read_string = [&root](const char* key, std::string& dst) {
        if (root.contains(key) && root[key].is_string()) dst = root[key].get<std::string>();
    };
    auto read_number = [&root](const char* key, long long& dst) {
        if (root.contains(key) && root[key].is_number_integer()) dst = root[key].get<long long>();
    };
    auto read_int = [&root](const char* key, int& dst) {
        if (root.contains(key) && root[key].is_number_integer()) dst = root[key].get<int>();
    };
    read_string("player_color", snap.player_color);
    read_string("status", snap.status);
    read_string("winner", snap.winner);
    read_string("reason", snap.reason);
    read_number("created_ms", snap.created_ms);
    read_number("updated_ms", snap.updated_ms);
    read_int("timeout_ms", snap.timeout_ms);
    read_int("max_depth", snap.max_depth);
    if (root.contains("position_pending") && root["position_pending"].is_boolean()) {
        snap.position_pending = root["position_pending"].get<bool>();
    }

    std::string record_error;
    if (!GameRecord::from_json(root["record"].dump(), snap.record, record_error)) {
        error = "会话里的对局记录读不出来: " + record_error;
        return false;
    }
    if (snap.status != "waiting" && snap.status != "playing" && snap.status != "game_over") {
        error = "会话状态不认识: " + snap.status;
        return false;
    }
    // Validate config values to prevent corrupted files from loading extreme
    // values that bypass the clamp_timeout_ms/clamp_max_depth guards applied at
    // message boundaries (set_config/request_hint). These bounds match those
    // clamps: [100, 120000] ms for timeout, [1, 20] for depth.
    if (snap.timeout_ms < 0 || snap.timeout_ms > 120000) {
        error = "会话里的超时时间不合法";
        return false;
    }
    if (snap.max_depth < 0 || snap.max_depth > 20) {
        error = "会话里的搜索深度不合法";
        return false;
    }
    // Absent means no hand-set position, which is the common case. A present but
    // malformed base is refused rather than skipped: silently dropping those
    // stones would resume the player into a different position than they left.
    if (root.contains("base")) {
        if (!root["base"].is_array()) {
            error = "会话里的 base 不是数组";
            return false;
        }
        for (const auto& entry : root["base"]) {
            if (!entry.is_object() || !entry.contains("row") || !entry.contains("col") ||
                !entry["row"].is_number_integer() || !entry["col"].is_number_integer() ||
                entry["row"].get<int>() < 0 || entry["row"].get<int>() >= BOARD_SIZE ||
                entry["col"].get<int>() < 0 || entry["col"].get<int>() >= BOARD_SIZE) {
                error = "会话里的摆棋棋子读不出来";
                return false;
            }
            int color = EMPTY;
            if (entry.contains("color") && entry["color"].is_string()) {
                color = color_of(entry["color"].get<std::string>());
            }
            if (color == EMPTY) {
                error = "会话里的摆棋棋子没有颜色";
                return false;
            }
            snap.base.emplace_back(entry["row"].get<int>(), entry["col"].get<int>(), color);
        }
    }
    out = std::move(snap);
    return true;
}

bool save_session(const std::string& dir, const SessionSnapshot& snap, std::string& error) {
    const std::string path = session_path(dir, snap.token);
    if (path.empty()) {
        error = "非法 token，无法落盘";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = "无法创建会话目录 " + dir + ": " + ec.message();
        return false;
    }

    const std::string tmp = path + kTmpSuffix;
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "无法写入 " + tmp;
            return false;
        }
        file << session_to_json(snap);
        file.flush();
        if (!file.good()) {
            error = "写入 " + tmp + " 中途失败";
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        error = "无法替换 " + path + ": " + ec.message();
        return false;
    }
    return true;
}

bool load_session(const std::string& dir, const std::string& token, SessionSnapshot& out,
                  std::string& error) {
    const std::string path = session_path(dir, token);
    if (path.empty()) {
        error = "非法 token";
        return false;
    }
    std::string text;
    if (!read_file(path, text, error)) return false;
    return session_from_json(text, out, error);
}

std::vector<std::string> list_sessions(const std::string& dir) {
    std::vector<std::string> found;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return found;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        found.push_back(entry.path().string());
    }
    std::sort(found.begin(), found.end());
    return found;
}

bool delete_session(const std::string& dir, const std::string& token, std::string& error) {
    const std::string path = session_path(dir, token);
    if (path.empty()) {
        error = "非法 token";
        return false;
    }
    // Separate error slots: remove() resets ec for a file that is simply
    // absent, so one shared code let the .tmp sweep overwrite a real failure
    // on the main file and report success with the session still on disk.
    std::error_code main_ec;
    std::filesystem::remove(path, main_ec);
    if (main_ec) {
        error = "无法删除 " + path + ": " + main_ec.message();
        return false;
    }
    // The stray .tmp is best-effort: its absence is not an error, and neither
    // is failing to sweep it.
    std::error_code tmp_ec;
    std::filesystem::remove(path + kTmpSuffix, tmp_ec);
    return true;
}

}  // namespace gomoku

#include "position_json.h"

#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace gomoku {

namespace {

SetupResult rejected(const std::string& error) {
    SetupResult result;
    result.error = error;
    return result;
}

// One grid value as a token: numbers become their own text, so 0/1/2 arrays and
// "0"/"1"/"2" arrays read identically and a stated encoding can mix them.
bool token_of(const json& value, std::string& out, std::string& error) {
    if (value.is_number_integer()) {
        out = std::to_string(value.get<long long>());
        return true;
    }
    if (value.is_string()) {
        out = value.get<std::string>();
        return true;
    }
    error = "棋盘格值必须是数字或标志字符串";
    return false;
}

bool read_encoding(const json& doc, GridEncoding& out, std::string& error) {
    if (!doc["encoding"].is_object()) {
        error = "encoding 必须是 {\"black\":…, \"white\":…, \"empty\":…} 对象";
        return false;
    }
    const json& enc = doc["encoding"];
    for (const char* key : {"black", "white", "empty"}) {
        if (!enc.contains(key)) {
            error = std::string("encoding 缺少 \"") + key + "\"";
            return false;
        }
        std::string token;
        if (!token_of(enc[key], token, error)) return false;
        if (std::string(key) == "black") out.black = token;
        else if (std::string(key) == "white") out.white = token;
        else out.empty = token;
    }
    return true;
}

// A 64-bit word of the bitboard form. A string is parsed with the usual 0x /
// decimal prefixes; a JSON number is fine too, but only when it is integral,
// because a float would silently round a 64-bit mask.
bool word_of(const json& value, uint64_t& out, std::string& error) {
    // Unsigned first: is_number_integer() also covers values above INT64_MAX,
    // and get<long long>() throws on those.
    if (value.is_number_unsigned()) {
        out = value.get<uint64_t>();
        return true;
    }
    if (value.is_number_integer()) {
        const long long signed_value = value.get<long long>();
        if (signed_value < 0) {
            error = "位棋盘的字不能为负";
            return false;
        }
        out = static_cast<uint64_t>(signed_value);
        return true;
    }
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        try {
            size_t consumed = 0;
            out = std::stoull(text, &consumed, 0);
            // Trailing junk would otherwise read as a different mask than the
            // file says ("0xFF " and "0xFF0" both parse to something).
            while (consumed < text.size() && std::isspace(static_cast<unsigned char>(text[consumed]))) ++consumed;
            if (consumed != text.size()) {
                error = "位棋盘的字不是合法整数: \"" + text + "\"";
                return false;
            }
            return true;
        } catch (const std::exception&) {
            error = "位棋盘的字不是合法整数: \"" + text + "\"";
            return false;
        }
    }
    error = "位棋盘的字必须是整数或整数字符串";
    return false;
}

bool read_words(const json& list, std::array<uint64_t, BITBOARD_WORDS>& out, const char* side,
                std::string& error) {
    if (!list.is_array() || list.size() != BITBOARD_WORDS) {
        error = std::string("bitboard.") + side + " 需要 " + std::to_string(BITBOARD_WORDS) + " 个字";
        return false;
    }
    for (int i = 0; i < BITBOARD_WORDS; ++i) {
        if (!word_of(list[i], out[i], error)) {
            error = std::string("bitboard.") + side + ": " + error;
            return false;
        }
    }
    return true;
}

bool position_of(const json& entry, Position& out, size_t index, std::string& error) {
    int row = -1;
    int col = -1;
    if (entry.is_array() && entry.size() == 2 && entry[0].is_number_integer() && entry[1].is_number_integer()) {
        row = entry[0].get<int>();
        col = entry[1].get<int>();
    } else if (entry.is_object() && entry.contains("row") && entry.contains("col") &&
               entry["row"].is_number_integer() && entry["col"].is_number_integer()) {
        row = entry["row"].get<int>();
        col = entry["col"].get<int>();
    } else {
        error = "第 " + std::to_string(index + 1) + " 个坐标需为 [row, col] 或 {\"row\":…,\"col\":…}";
        return false;
    }
    out = Position(row, col);
    return true;
}

SetupResult board_form(const json& doc, const GridEncoding* override) {
    const json& value = doc["board"];
    GridCells cells;
    if (value.is_string()) {
        std::string error;
        if (!parse_grid_text(value.get<std::string>(), cells, error)) return rejected(error);
        return position_from_grid(cells, override);
    }
    if (!value.is_array()) return rejected("board 必须是 15x15 数组、225 元数组或文本棋盘字符串");

    std::string error;
    if (value.size() == static_cast<size_t>(MAX_MOVES)) {
        // Flat row-major list, accepted because coordinate lists dumped from an
        // array-typed language arrive this way.
        cells.resize(BOARD_SIZE);
        for (int i = 0; i < MAX_MOVES; ++i) {
            std::string token;
            if (!token_of(value[i], token, error)) return rejected(error);
            cells[i / BOARD_SIZE].push_back(token);
        }
    } else if (value.size() == static_cast<size_t>(BOARD_SIZE)) {
        for (const json& row_json : value) {
            if (!row_json.is_array() || row_json.size() != static_cast<size_t>(BOARD_SIZE)) {
                return rejected("board 的每一行必须是 " + std::to_string(BOARD_SIZE) + " 元数组");
            }
            GridRow row;
            for (const json& cell : row_json) {
                std::string token;
                if (!token_of(cell, token, error)) return rejected(error);
                row.push_back(token);
            }
            cells.push_back(std::move(row));
        }
    } else {
        return rejected("board 必须是 15 行、225 元或文本棋盘");
    }
    return position_from_grid(cells, override);
}

SetupResult bitboard_form(const json& doc) {
    const json& value = doc["bitboard"];
    if (!value.is_object() || !value.contains("black") || !value.contains("white")) {
        return rejected("bitboard 必须是 {\"black\":[4 字], \"white\":[4 字]}");
    }
    std::array<uint64_t, BITBOARD_WORDS> black{};
    std::array<uint64_t, BITBOARD_WORDS> white{};
    std::string error;
    if (!read_words(value["black"], black, "black", error)) return rejected(error);
    if (!read_words(value["white"], white, "white", error)) return rejected(error);
    return position_from_bitboard(black, white);
}

SetupResult moves_form(const json& doc) {
    const json& value = doc["moves"];
    if (!value.is_array()) return rejected("moves 必须是坐标数组");

    int first = BLACK;
    if (doc.contains("first")) {
        if (!doc["first"].is_string()) return rejected("first 必须是 \"black\" 或 \"white\"");
        const std::string side = doc["first"].get<std::string>();
        if (side != "black" && side != "white") {
            return rejected("first 只能是 black 或 white，收到 \"" + side + "\"");
        }
        first = side == "black" ? BLACK : WHITE;
    }

    std::vector<Position> moves;
    moves.reserve(value.size());
    size_t index = 0;
    for (const json& entry : value) {
        Position pos;
        std::string error;
        if (!position_of(entry, pos, index, error)) return rejected(error);
        moves.push_back(pos);
        ++index;
    }
    return position_from_moves(moves, first);
}

}  // namespace

SetupResult position_from_json(const json& doc) {
    if (!doc.is_object()) return rejected("局面文档必须是 JSON 对象");
    if (doc.contains("version")) {
        if (!doc["version"].is_number_integer() || doc["version"].get<int>() != 1) {
            return rejected("不支持的局面版本，当前只认 version 1");
        }
    }

    GridEncoding override;
    if (doc.contains("encoding")) {
        std::string error;
        if (!read_encoding(doc, override, error)) return rejected(error);
    }

    const int forms = (doc.contains("board") ? 1 : 0) + (doc.contains("bitboard") ? 1 : 0) +
                      (doc.contains("moves") ? 1 : 0);
    if (forms != 1) {
        return rejected("局面文档需要 board / bitboard / moves 三者之一，且只能选一个");
    }
    if (doc.contains("first") && !doc.contains("moves")) {
        return rejected("first 只能与 moves 同时出现");
    }

    if (doc.contains("board")) {
        return board_form(doc, doc.contains("encoding") ? &override : nullptr);
    }
    if (doc.contains("bitboard")) return bitboard_form(doc);
    return moves_form(doc);
}

SetupResult position_from_text(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return rejected("内容为空");
    // A JSON document starts with either brace, so a bare array reports the
    // parser's complaint rather than a confusing grid-width error.
    const char opening = text[first];
    if (opening != '{' && opening != '[') {
        GridCells cells;
        std::string error;
        if (!parse_grid_text(text, cells, error)) return rejected(error);
        return position_from_grid(cells);
    }
    // do_not_throw: a truncated upload is a normal occurrence, not a crash.
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded()) return rejected("JSON 解析失败，请检查括号与逗号");
    return position_from_json(doc);
}

bool load_position_file(const std::string& path, SetupResult& out, std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "无法打开局面文件: " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out = position_from_text(buffer.str());
    if (!out.ok()) error = out.error;
    return out.ok();
}

std::string position_to_json(const Board& board) {
    json doc;
    doc["version"] = 1;
    doc["type"] = "gomoku_position";
    doc["encoding"] = {{"black", BLACK}, {"white", WHITE}, {"empty", EMPTY}};
    json rows = json::array();
    for (int r = 0; r < BOARD_SIZE; ++r) {
        json row = json::array();
        for (int c = 0; c < BOARD_SIZE; ++c) row.push_back(board.get(r, c));
        rows.push_back(std::move(row));
    }
    doc["board"] = std::move(rows);
    return doc.dump(2);
}

}

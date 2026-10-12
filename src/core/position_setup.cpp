#include "position_setup.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

namespace gomoku {

bool GridEncoding::operator==(const GridEncoding& other) const {
    return black == other.black && white == other.white && empty == other.empty;
}

namespace {

enum class Token : int { BLACK = 0, WHITE = 1, EMPTY = 2, UNKNOWN = 3 };

std::string trim(const std::string& raw) {
    const char* spaces = " \t\r\n";
    size_t b = raw.find_first_not_of(spaces);
    if (b == std::string::npos) return {};
    size_t e = raw.find_last_not_of(spaces);
    return raw.substr(b, e - b + 1);
}

// ASCII letters fold to upper case so "b"/"w"/"o" read the same as their
// capitals; multi-byte tokens (黑) pass through untouched, since casting a byte
// above 127 to unsigned char keeps std::toupper a no-op on them.
std::string normalize(const std::string& raw) {
    std::string t = trim(raw);
    for (char& ch : t) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return t;
}

struct Alphabet {
    std::vector<std::string> black;
    std::vector<std::string> white;
    std::vector<std::string> empty;
};

// The documented token sets, in priority order. The first is the engine's own
// cell numbering (EMPTY=0, BLACK=1, WHITE=2), which is what board_to_json emits
// and therefore what a round-tripped position carries; the second is the same
// numbering with -1 for WHITE, as array dumps from numpy-style datasets write
// it. The canonical symbol for a colour is listed first in its alias vector.
const std::vector<Alphabet>& alphabets() {
    static const std::vector<Alphabet> kTable = {
        Alphabet{{"1"}, {"2"}, {"0"}},
        Alphabet{{"1"}, {"-1"}, {"0"}},
        Alphabet{{"B"}, {"W"}, {".", "_", "-", "·", ""}},
        Alphabet{{"X"}, {"O"}, {".", "_", "-", "·", ""}},
        Alphabet{{"黑"}, {"白"}, {"空", ".", "_", "-", "·", ""}},
    };
    return kTable;
}

bool in_list(const std::vector<std::string>& list, const std::string& token) {
    return std::find(list.begin(), list.end(), token) != list.end();
}

Token classify(const Alphabet& alphabet, const std::string& token) {
    if (in_list(alphabet.black, token)) return Token::BLACK;
    if (in_list(alphabet.white, token)) return Token::WHITE;
    if (in_list(alphabet.empty, token)) return Token::EMPTY;
    return Token::UNKNOWN;
}

const char* token_name(Token t) {
    switch (t) {
        case Token::BLACK: return "black";
        case Token::WHITE: return "white";
        case Token::EMPTY: return "empty";
        default: return "unknown";
    }
}

// The distinct tokens of a grid, normalized and sorted.
std::vector<std::string> grid_tokens(const GridCells& cells) {
    std::map<std::string, bool> seen;
    for (const GridRow& row : cells) {
        for (const std::string& cell : row) seen[normalize(cell)] = true;
    }
    std::vector<std::string> tokens;
    for (const auto& entry : seen) tokens.push_back(entry.first);
    return tokens;
}

// Every form ends here: build the board, then run the shared legality checks.
void finish(SetupResult& result) {
    validate_position(result.board, result);
}

}  // namespace

std::vector<GridEncoding> detect_grid_encodings(const GridCells& cells) {
    const std::vector<std::string> tokens = grid_tokens(cells);
    std::vector<GridEncoding> readings;
    std::vector<std::string> signatures;

    for (const Alphabet& alphabet : alphabets()) {
        std::map<Token, std::string> representative;
        std::string signature;
        bool covered = true;
        for (const std::string& token : tokens) {
            const Token kind = classify(alphabet, token);
            if (kind == Token::UNKNOWN) {
                covered = false;
                break;
            }
            // Prefer a token the file actually uses, so the confirmation dialog
            // can echo back the symbol the player wrote.
            if (!representative.count(kind)) representative[kind] = token;
            signature += token + "=" + token_name(kind) + ";";
        }
        if (!covered) continue;

        auto symbol_for = [&alphabet, &representative](Token kind, const std::vector<std::string>& aliases) {
            auto found = representative.find(kind);
            // An empty token is only worth echoing when it is the alphabet's own
            // canonical empty symbol - reporting "" as the empty mark in a dialog
            // is useless to a reader.
            if (found != representative.end() && !(kind == Token::EMPTY && found->second.empty())) {
                return found->second;
            }
            return aliases[0];
        };

        GridEncoding encoding;
        encoding.black = symbol_for(Token::BLACK, alphabet.black);
        encoding.white = symbol_for(Token::WHITE, alphabet.white);
        encoding.empty = symbol_for(Token::EMPTY, alphabet.empty);

        // Two alphabets that read every present token the same way are one
        // reading, not an ambiguity - "." alone is empty under three alphabets.
        if (std::find(signatures.begin(), signatures.end(), signature) == signatures.end()) {
            signatures.push_back(signature);
            readings.push_back(encoding);
        }
    }
    return readings;
}

namespace {

// One line's cells: a plain 15-character ASCII row reads as one symbol per cell,
// anything else splits on the usual separators.
GridRow split_row(const std::string& stripped) {
    const bool has_separator = stripped.find_first_of(",;\t") != std::string::npos;
    const bool one_symbol_per_cell = !has_separator && stripped.size() == BOARD_SIZE &&
                                     std::all_of(stripped.begin(), stripped.end(), [](char ch) {
                                         return static_cast<unsigned char>(ch) < 0x80;
                                     });
    GridRow row;
    if (one_symbol_per_cell) {
        for (char ch : stripped) row.push_back(std::string(1, ch));
        return row;
    }
    std::string current;
    auto flush = [&row, &current]() {
        if (current.empty()) return;
        row.push_back(current);
        current.clear();
    };
    for (char ch : stripped) {
        if (ch == ',' || ch == ';' || ch == '\t' || ch == ' ') flush();
        else current += ch;
    }
    flush();
    return row;
}

// The codepoints of a block, so an unseparated 黑/白/空 run explodes by symbol
// and not by byte. A lead byte whose continuation bytes are missing or malformed
// yields itself alone, which then matches no alphabet and is reported as an
// unknown mark rather than silently corrupting a neighbour.
std::vector<std::string> codepoints_of(const std::string& block) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < block.size()) {
        const unsigned char lead = static_cast<unsigned char>(block[i]);
        size_t width = 1;
        if (lead >= 0xF0) width = 4;
        else if (lead >= 0xE0) width = 3;
        else if (lead >= 0xC0) width = 2;
        bool well_formed = width > 1 && i + width <= block.size();
        for (size_t k = 1; k < width && well_formed; ++k) {
            if ((static_cast<unsigned char>(block[i + k]) & 0xC0) != 0x80) well_formed = false;
        }
        if (!well_formed) width = 1;
        out.push_back(block.substr(i, width));
        i += width;
    }
    return out;
}

GridCells reshape(const std::vector<std::string>& cells) {
    GridCells rows;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        rows.push_back(GridRow(cells.begin() + r * BOARD_SIZE,
                               cells.begin() + (r + 1) * BOARD_SIZE));
    }
    return rows;
}

}  // namespace

bool parse_grid_text(const std::string& text, GridCells& out, std::string& error) {
    out.clear();
    std::string body = text;
    // UTF-8 BOM.
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF) {
        body.erase(0, 3);
    }

    std::istringstream stream(body);
    std::string line;
    GridCells rows;
    std::vector<std::string> blocks;  // every cell of the document, row layout ignored
    std::string row_error;
    bool ragged = false;
    bool past_rows = false;
    bool past_cells = false;
    while (std::getline(stream, line)) {
        const std::string stripped = trim(line);
        if (stripped.empty() || stripped[0] == '#') continue;

        GridRow row = split_row(stripped);
        for (const std::string& cell : row) blocks.push_back(cell);
        if (static_cast<int>(row.size()) != BOARD_SIZE) {
            ragged = true;
            if (row_error.empty()) {
                row_error = "第 " + std::to_string(rows.size() + 1) + " 行有 " +
                            std::to_string(row.size()) + " 格，需要 " + std::to_string(BOARD_SIZE) + " 格";
            }
        }
        rows.push_back(std::move(row));

        // Tokenising the whole upload before refusing it cost 12 s and 439 MB on a
        // 16 MB text, on the single io thread and without a session. A grid wider
        // than 15 rows and any run past 225 cells can no longer become a board, so
        // reading stops there and the complaint carries the count reached so far.
        if (!ragged && static_cast<int>(rows.size()) > BOARD_SIZE) {
            past_rows = true;
            break;
        }
        if (blocks.size() > static_cast<size_t>(BOARD_SIZE * BOARD_SIZE)) {
            past_cells = true;
            break;
        }
    }

    if (!ragged && static_cast<int>(rows.size()) == BOARD_SIZE) {
        out = std::move(rows);
        return true;
    }
    if (row_error.empty()) {
        row_error = past_rows
            ? "棋盘有超过 " + std::to_string(BOARD_SIZE) + " 行（读到 " +
              std::to_string(rows.size()) + " 行），需要 " + std::to_string(BOARD_SIZE) + " 行"
            : "棋盘有 " + std::to_string(rows.size()) + " 行，需要 " +
              std::to_string(BOARD_SIZE) + " 行";
    }

    // Flat form: the whole document as one long run, row layout ignored - 225
    // separated cells, or as many symbols as the separators happen to leave. A
    // dataset dumped with -1 for WHITE therefore has to keep its separators, since
    // an unseparated run is read one symbol per character by definition.
    if (blocks.size() == static_cast<size_t>(BOARD_SIZE * BOARD_SIZE)) {
        out = reshape(blocks);
        return true;
    }
    std::vector<std::string> symbols;
    for (const std::string& block : blocks) {
        for (const std::string& symbol : codepoints_of(block)) symbols.push_back(symbol);
    }
    // A fourth kind of symbol means the text cannot be a board however long it is,
    // so it is reported before the cell count: a truncated dump and a file that
    // mixes alphabets need different fixes.
    std::vector<std::string> kinds;
    for (const std::string& symbol : symbols) {
        const std::string token = normalize(symbol);
        if (std::find(kinds.begin(), kinds.end(), token) == kinds.end()) kinds.push_back(token);
    }
    if (kinds.size() > 3) {
        std::string seen;
        for (const std::string& kind : kinds) seen += (seen.empty() ? "" : "/") + kind;
        error = "连续棋盘含 " + std::to_string(kinds.size()) + " 种符号 (" + seen +
                ")，只能表示黑/白/空三种";
        return false;
    }
    if (symbols.size() == static_cast<size_t>(BOARD_SIZE * BOARD_SIZE)) {
        out = reshape(symbols);
        return true;
    }

    // Rows that are all 15 cells wide but the wrong number of them: say so, that
    // is the honest complaint. Otherwise the document was meant to be flat, so
    // count its cells instead of blaming a row width.
    if (!ragged) {
        error = row_error;
        return false;
    }
    const size_t seen_count = std::max(blocks.size(), symbols.size());
    if (seen_count > static_cast<size_t>(BOARD_SIZE)) {
        error = past_cells
            ? "棋盘有超过 " + std::to_string(BOARD_SIZE * BOARD_SIZE) + " 格（读到 " +
              std::to_string(seen_count) + " 格），需要 " +
              std::to_string(BOARD_SIZE * BOARD_SIZE) + " 格（15x15，分隔或连写皆可）"
            : "棋盘有 " + std::to_string(seen_count) + " 格，需要 " +
              std::to_string(BOARD_SIZE * BOARD_SIZE) + " 格（15x15，分隔或连写皆可）";
        return false;
    }
    error = row_error;
    return false;
}

namespace {

bool same_sign(const std::string& a, const std::string& b) { return normalize(a) == normalize(b); }

bool dims_ok(const GridCells& cells, std::string& error) {
    if (static_cast<int>(cells.size()) != BOARD_SIZE) {
        error = "棋盘有 " + std::to_string(cells.size()) + " 行，需要 " + std::to_string(BOARD_SIZE) + " 行";
        return false;
    }
    for (size_t r = 0; r < cells.size(); ++r) {
        if (static_cast<int>(cells[r].size()) != BOARD_SIZE) {
            error = "第 " + std::to_string(r + 1) + " 行有 " + std::to_string(cells[r].size()) +
                    " 格，需要 " + std::to_string(BOARD_SIZE) + " 格";
            return false;
        }
    }
    return true;
}

}  // namespace

SetupResult position_from_grid(const GridCells& cells, const GridEncoding* forced) {
    SetupResult result;
    std::string error;
    if (!dims_ok(cells, error)) {
        result.error = error;
        return result;
    }

    GridEncoding encoding;
    if (forced) {
        encoding = *forced;
        encoding.black = normalize(encoding.black);
        encoding.white = normalize(encoding.white);
        encoding.empty = normalize(encoding.empty);
        // The three tokens have to name three different things: black == white
        // reads every white stone as black, and an empty that collides with either
        // makes that side's stones vanish.
        if (encoding.black == encoding.white) {
            result.error = "指定的标志里黑和白共用 \"" + encoding.black + "\"，无法分出双方";
            return result;
        }
        if (encoding.black == encoding.empty) {
            result.error = "指定的标志里黑和空共用 \"" + encoding.black + "\"，黑子会被读成空位";
            return result;
        }
        if (encoding.white == encoding.empty) {
            result.error = "指定的标志里白和空共用 \"" + encoding.white + "\"，白子会被读成空位";
            return result;
        }
        // An explicitly supplied mapping is trusted as stated, but every token
        // still has to be covered by it - otherwise a stray symbol would be read
        // as empty and quietly delete a stone.
        for (const GridRow& row : cells) {
            for (const std::string& cell : row) {
                const std::string token = normalize(cell);
                if (!same_sign(token, encoding.black) && !same_sign(token, encoding.white) &&
                    !same_sign(token, encoding.empty)) {
                    result.error = "指定的标志不含 \"" + token + "\"，请确认黑/白/空分别对应哪个符号";
                    return result;
                }
            }
        }
    } else {
        std::vector<GridEncoding> readings = detect_grid_encodings(cells);
        if (readings.empty()) {
            std::string seen;
            for (const std::string& token : grid_tokens(cells)) {
                if (token.empty()) continue;
                seen += (seen.empty() ? "" : "/") + token;
            }
            result.error = "无法识别棋盘标志 " + (seen.empty() ? "" : "(" + seen + ")") +
                           "，支持 0/1/2、1/-1/0、B/W/.、X/O/.、黑/白/空";
            return result;
        }
        encoding = readings[0];
    }
    result.encoding = encoding;

    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            const std::string token = normalize(cells[r][c]);
            int color = EMPTY;
            if (same_sign(token, encoding.black)) {
                color = BLACK;
            } else if (same_sign(token, encoding.white)) {
                color = WHITE;
            }
            if (color != EMPTY && !result.board.place_stone(r, c, color)) {
                result.error = "落子失败 (" + std::to_string(r) + "," + std::to_string(c) + ")";
                return result;
            }
        }
    }
    finish(result);
    return result;
}

SetupResult position_from_bitboard(const std::array<uint64_t, BITBOARD_WORDS>& black,
                                   const std::array<uint64_t, BITBOARD_WORDS>& white) {
    SetupResult result;
    for (int w = 0; w < BITBOARD_WORDS; ++w) {
        const uint64_t stray = w == BITBOARD_WORDS - 1 ? ~BITBOARD_TOP_MASK : 0ull;
        if ((black[w] | white[w]) & stray) {
            result.error = "位棋盘第 " + std::to_string(w) + " 个字含超出 " +
                           std::to_string(MAX_MOVES) + " 格的位";
            return result;
        }
        if (black[w] & white[w]) {
            result.error = "位棋盘的 black 与 white 在第 " + std::to_string(w) + " 个字上重叠";
            return result;
        }
    }

    for (int i = 0; i < MAX_MOVES; ++i) {
        const int word = i / 64;
        const uint64_t bit = 1ull << (i % 64);
        int color = EMPTY;
        if (black[word] & bit) color = BLACK;
        if (white[word] & bit) color = WHITE;
        if (color == EMPTY) continue;
        const Position pos = Position::from_index(i);
        if (!result.board.place_stone(pos, color)) {
            result.error = "落子失败 (" + std::to_string(pos.row) + "," + std::to_string(pos.col) + ")";
            return result;
        }
    }
    finish(result);
    return result;
}

SetupResult position_from_moves(const std::vector<Position>& moves, int first_color) {
    SetupResult result;
    if (first_color != BLACK && first_color != WHITE) {
        result.error = "首手颜色必须是黑或白";
        return result;
    }
    for (size_t i = 0; i < moves.size(); ++i) {
        const Position pos = moves[i];
        if (!pos.is_valid()) {
            result.error = "第 " + std::to_string(i + 1) + " 个坐标 (" + std::to_string(pos.row) + "," +
                           std::to_string(pos.col) + ") 超出棋盘";
            return result;
        }
        if (!result.board.is_empty(pos)) {
            result.error = "第 " + std::to_string(i + 1) + " 个坐标 (" + std::to_string(pos.row) + "," +
                           std::to_string(pos.col) + ") 与前面的落子重复";
            return result;
        }
        const int color = (i % 2 == 0) ? first_color : opponent(first_color);
        if (!result.board.place_stone(pos, color)) {
            result.error = "落子失败 (" + std::to_string(pos.row) + "," + std::to_string(pos.col) + ")";
            return result;
        }
        result.history.emplace_back(pos, color);
    }
    finish(result);
    return result;
}

void validate_position(const Board& board, SetupResult& result) {
    int black = 0;
    int white = 0;
    std::vector<Position> black_stones;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            const int color = board.get(r, c);
            if (color == BLACK) {
                ++black;
                black_stones.emplace_back(r, c);
            } else if (color == WHITE) {
                ++white;
            }
        }
    }
    result.black_count = black;
    result.white_count = white;

    if (board.move_count() >= MAX_MOVES) {
        result.error = "棋盘已满，无子可落";
        return;
    }
    // BLACK's first-move obligation is what makes the counts carry turn
    // information at all: equal counts mean BLACK is due, one extra BLACK stone
    // means WHITE is, and anything else was never a position in a game.
    if (black == white) {
        result.side_to_move = BLACK;
    } else if (black == white + 1) {
        result.side_to_move = WHITE;
    } else {
        result.error = "黑 " + std::to_string(black) + " 子、白 " + std::to_string(white) +
                       " 子：黑先手，黑白差只能是 0 或 1";
        return;
    }

    const int winner = board.check_winner();
    if (winner != EMPTY) {
        result.error = std::string(winner == BLACK ? "黑" : "白") + "方已有五连，此局面已结束";
        return;
    }

    // Renju forbids the *move*, so a BLACK stone already on the board is legal
    // only if the square it sits on was not a forbidden landing square given the
    // rest of the position: remove it, ask is_forbidden, put it back. This also
    // catches 长连, which check_winner deliberately does not treat as a win.
    for (const Position& stone : black_stones) {
        Board without;
        for (int r = 0; r < BOARD_SIZE; ++r) {
            for (int c = 0; c < BOARD_SIZE; ++c) {
                if (r == stone.row && c == stone.col) continue;
                const int color = board.get(r, c);
                if (color != EMPTY) without.place_stone(r, c, color);
            }
        }
        if (PatternRecognizer::is_forbidden(without, stone.row, stone.col, BLACK)) {
            result.error = "黑子 (" + std::to_string(stone.row) + "," + std::to_string(stone.col) +
                           ") 落点为禁手（三三 / 四四 / 长连）";
            return;
        }
    }
}

}

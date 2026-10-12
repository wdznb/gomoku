#include "core/position_setup.h"
#include "server/position_json.h"
#include "server/game_manager.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace gomoku;

namespace {

const std::vector<Position> kBlack{{7, 7}, {8, 8}, {9, 9}};
const std::vector<Position> kWhite{{6, 6}, {7, 8}};

std::string token_for(const std::vector<Position>& black, const std::vector<Position>& white,
                      int r, int c, const char* b, const char* w, const char* e) {
    const int index = r * BOARD_SIZE + c;
    for (const Position& p : black) {
        if (p.to_index() == index) return b;
    }
    for (const Position& p : white) {
        if (p.to_index() == index) return w;
    }
    return e;
}

// A grid written with the given tokens. `delim` separates cells: "" writes one
// symbol per cell (ASCII only), "," or " " write a separated grid, which is how
// multi-byte tokens such as 黑 have to be written.
std::string grid_text(const std::vector<Position>& black, const std::vector<Position>& white,
                      const char* b, const char* w, const char* e, const char* delim) {
    std::string out;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            out += token_for(black, white, r, c, b, w, e);
            if (c + 1 < BOARD_SIZE) out += delim;
        }
        out += '\n';
    }
    return out;
}

std::vector<int> cells_of(const Board& board) {
    std::vector<int> cells;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) cells.push_back(board.get(r, c));
    }
    return cells;
}

bool same_cells(const SetupResult& a, const SetupResult& b) {
    return cells_of(a.board) == cells_of(b.board);
}

// The same position as one long run rather than 15 rows: `delim` goes between
// every two cells ("" for an unseparated run) and a line break every `wrap`
// cells, where wrap != 0. The line breaks deliberately ignore row boundaries.
std::string flat_text(const std::vector<Position>& black, const std::vector<Position>& white,
                      const char* b, const char* w, const char* e, const char* delim, int wrap) {
    std::string out;
    for (int i = 0; i < MAX_MOVES; ++i) {
        if (i && *delim) out += delim;
        out += token_for(black, white, i / BOARD_SIZE, i % BOARD_SIZE, b, w, e);
        if (wrap && (i + 1) % wrap == 0 && i + 1 < MAX_MOVES) out += '\n';
    }
    return out;
}

}  // namespace

void test_grid_parsing() {
    GridCells cells;
    std::string error;

    assert(parse_grid_text(grid_text(kBlack, kWhite, "1", "2", "0", ","), cells, error));
    assert(cells.size() == BOARD_SIZE && cells[0].size() == BOARD_SIZE);
    assert(cells[7][7] == "1" && cells[7][8] == "2" && cells[0][0] == "0");

    // One symbol per cell, with a BOM, comments and blank lines to look past.
    assert(parse_grid_text("\xEF\xBB\xBF# trainer dump\n\n" +
                               grid_text(kBlack, kWhite, "B", "W", ".", ""),
                           cells, error));
    assert(cells[7][7] == "B" && cells[7][8] == "W");

    // Semicolon and space separate cells just as well as comma.
    const std::string semi = grid_text(kBlack, kWhite, "1", "2", "0", ";");
    assert(parse_grid_text(semi, cells, error));
    assert(cells[9][9] == "1");
    const std::string spaced = grid_text(kBlack, kWhite, "1", "2", "0", " ");
    assert(parse_grid_text(spaced, cells, error));
    assert(cells[6][6] == "2");

    GridCells nope;
    assert(!parse_grid_text("1,2,3\n", nope, error));
    assert(error.find("15 格") != std::string::npos);
    assert(!parse_grid_text(grid_text(kBlack, kWhite, "1", "2", "0", ",").substr(0, 60), nope, error));
    assert(error.find("15 行") != std::string::npos);
    assert(!parse_grid_text("", nope, error));
    assert(!parse_grid_text("# only a comment\n", nope, error));

    std::cout << "test_grid_parsing passed" << std::endl;
}

void test_flat_strings() {
    const SetupResult rows = position_from_text(grid_text(kBlack, kWhite, "B", "W", ".", ""));
    assert(rows.ok());

    // One unseparated run of 225 ASCII symbols is the same board.
    SetupResult run = position_from_text(flat_text(kBlack, kWhite, "B", "W", ".", "", 0));
    assert(run.ok() && same_cells(rows, run));
    assert(run.encoding.black == "B");

    SetupResult numeric = position_from_text(flat_text(kBlack, kWhite, "1", "2", "0", "", 0));
    assert(numeric.ok() && same_cells(rows, numeric));

    // 225 multi-byte symbols are 675 bytes: the run has to explode by codepoint,
    // and this is the form the row parser used to refuse outright.
    SetupResult chinese = position_from_text(flat_text(kBlack, kWhite, "黑", "白", "空", "", 0));
    assert(chinese.ok() && same_cells(rows, chinese));
    assert(chinese.encoding.black == "黑");

    // Separated flat dumps, on one line or with breaks that ignore row
    // boundaries. 1/-1/0 only works here: an unseparated run would read the "-"
    // and the "1" as two different marks.
    SetupResult sep = position_from_text(flat_text(kBlack, kWhite, "1", "-1", "0", ",", 0));
    assert(sep.ok() && same_cells(rows, sep));
    assert(sep.encoding.white == "-1");
    SetupResult spaced = position_from_text(flat_text(kBlack, kWhite, "1", "2", "0", " ", 50));
    assert(spaced.ok() && same_cells(rows, spaced));
    SetupResult tabs = position_from_text(flat_text(kBlack, kWhite, "1", "2", "0", "\t;", 50));
    assert(tabs.ok() && same_cells(rows, tabs));

    // A fourth symbol is a refusal, not a guess, whatever the run's length is: the
    // symbol kinds are checked before the cell count.
    std::string four = flat_text(kBlack, kWhite, "B", "W", ".", "", 0);
    four[0] = '4';
    SetupResult four_marks = position_from_text(four);
    assert(!four_marks.ok() && four_marks.error.find("三种") != std::string::npos);
    SetupResult long_four = position_from_text(four + "BWB.WB");
    assert(!long_four.ok() && long_four.error.find("三种") != std::string::npos);
    SetupResult short_four = position_from_text(four.substr(0, 200));
    assert(!short_four.ok() && short_four.error.find("三种") != std::string::npos);

    // Three symbols and the wrong number of cells is a length complaint.
    const std::string run225 = flat_text(kBlack, kWhite, "1", "2", "0", "", 0);
    SetupResult short_run = position_from_text(run225.substr(0, 224));
    assert(!short_run.ok() && short_run.error.find("224 格") != std::string::npos);
    SetupResult long_run = position_from_text(run225 + "1");
    assert(!long_run.ok() && long_run.error.find("226 格") != std::string::npos);
    SetupResult long_sep = position_from_text(flat_text(kBlack, kWhite, "1", "2", "0", ",", 0) + ",1");
    assert(!long_sep.ok() && long_sep.error.find("226 格") != std::string::npos);
    SetupResult empty_run = position_from_text(std::string(300, '.'));
    assert(!empty_run.ok() && empty_run.error.find("300 格") != std::string::npos);

    // Rows that are all 15 wide but the wrong number of them are still reported
    // as rows, and a three-cell line still reports its width: the flat reading is
    // a fallback, not a rewrite of the existing diagnostics.
    const std::string numeric_rows = grid_text(kBlack, kWhite, "1", "2", "0", ",");
    SetupResult sixteen = position_from_text(numeric_rows + numeric_rows.substr(0, numeric_rows.find('\n') + 1));
    assert(!sixteen.ok() && sixteen.error.find("16 行") != std::string::npos);
    SetupResult three = position_from_text("1,2,3\n");
    assert(!three.ok() && three.error.find("15 格") != std::string::npos);

    std::cout << "test_flat_strings passed" << std::endl;
}

void test_encoding_detection() {
    // Each supported alphabet is recognised from the file's own tokens.
    struct Case { const char* b; const char* w; const char* e; };
    const Case cases[] = {{"1", "2", "0"}, {"B", "W", "."}, {"X", "O", "."},
                          {"黑", "白", "."}};
    for (const Case& c : cases) {
        const char* delim = std::string(c.b).size() > 1 ? "," : "";
        SetupResult r = position_from_text(grid_text(kBlack, kWhite, c.b, c.w, c.e, delim));
        assert(r.ok());
        assert(r.encoding.black == c.b && r.encoding.white == c.w && r.encoding.empty == c.e);
        assert(r.black_count == 3 && r.white_count == 2 && r.side_to_move == WHITE);
    }
    // 1 / -1 / 0 is the other numeric convention.
    SetupResult signed_form =
        position_from_text(grid_text(kBlack, kWhite, "1", "-1", "0", ","));
    assert(signed_form.ok());
    assert(signed_form.encoding.white == "-1");
    assert(signed_form.black_count == 3 && signed_form.white_count == 2);

    // Underscore and dash are ordinary empty markers too.
    assert(position_from_text(grid_text(kBlack, kWhite, "B", "W", "_", "")).ok());
    assert(position_from_text(grid_text(kBlack, kWhite, "B", "W", "-", "")).ok());

    // Lower case folds to the same reading.
    SetupResult lower = position_from_text(grid_text(kBlack, kWhite, "b", "w", ".", ""));
    assert(lower.ok() && lower.encoding.black == "B");

    // Mixing notations is not a reading anyone should guess: reject it.
    SetupResult mixed = position_from_text(grid_text(kBlack, kWhite, "B", "2", ".", ""));
    assert(!mixed.ok());
    assert(mixed.error.find("无法识别") != std::string::npos);

    // A stated encoding overrides detection, including the opposite reading.
    json doc;
    doc["board"] = grid_text({{7, 7}, {8, 8}}, {{6, 6}}, "o", "x", ".", "");
    doc["encoding"] = {{"black", "O"}, {"white", "X"}, {"empty", "."}};
    SetupResult forced = position_from_json(doc);
    assert(forced.ok());
    assert(forced.encoding.black == "O");
    assert(forced.black_count == 2 && forced.white_count == 1);
    assert(forced.side_to_move == WHITE);
    // Without that statement the same file reads the other way round, and the
    // counts come out impossible: this is exactly what the confirmation step is
    // for.
    assert(!position_from_text(grid_text({{7, 7}, {8, 8}}, {{6, 6}}, "o", "x", ".", "")).ok());

    // ...and a stated mapping must still cover every token the file uses.
    doc["encoding"] = {{"black", "O"}, {"white", "X"}, {"empty", "_"}};
    SetupResult uncovered = position_from_json(doc);
    assert(!uncovered.ok());
    assert(uncovered.error.find("指定的标志") != std::string::npos);

    std::cout << "test_encoding_detection passed" << std::endl;
}

void test_bitboard_form() {
    std::array<uint64_t, BITBOARD_WORDS> black{};
    std::array<uint64_t, BITBOARD_WORDS> white{};
    for (const Position& p : kBlack) black[p.to_index() / 64] |= 1ull << (p.to_index() % 64);
    for (const Position& p : kWhite) white[p.to_index() / 64] |= 1ull << (p.to_index() % 64);

    SetupResult r = position_from_bitboard(black, white);
    assert(r.ok());
    assert(r.black_count == 3 && r.white_count == 2 && r.side_to_move == WHITE);
    assert(r.board.get(9, 9) == BLACK && r.board.get(7, 8) == WHITE);

    std::array<uint64_t, BITBOARD_WORDS> overlap_white = white;
    overlap_white[0] |= 1ull;  // cell (0,0), already BLACK below
    std::array<uint64_t, BITBOARD_WORDS> overlap_black = black;
    overlap_black[0] |= 1ull;
    assert(!position_from_bitboard(overlap_black, overlap_white).ok());

    std::array<uint64_t, BITBOARD_WORDS> stray_black = black;
    stray_black[3] |= 1ull << BITBOARD_TOP_BITS;  // the first cell past index 224
    SetupResult stray = position_from_bitboard(stray_black, white);
    assert(!stray.ok() && stray.error.find("225") != std::string::npos);
    // The last real cell is still accepted.
    std::array<uint64_t, BITBOARD_WORDS> edge_white = white;
    edge_white[3] |= 1ull << (BITBOARD_TOP_BITS - 1);
    assert(position_from_bitboard(black, edge_white).ok());

    // The words arrive as decimals or hex strings; a string keeps full 64-bit
    // precision, which a JSON double would round away. Cells (7,7)=112,
    // (8,8)=128, (9,9)=144 for black and (6,6)=96, (7,8)=113 for white.
    SetupResult from_json = position_from_text(
        R"({"bitboard":{"black":["0x0","0x0001000000000000","0x10001","0x0"],
                        "white":["0x0","0x0002000100000000","0x0","0x0"]}})");
    assert(from_json.ok());
    assert(same_cells(r, from_json));

    std::cout << "test_bitboard_form passed" << std::endl;
}

void test_moves_form() {
    SetupResult r = position_from_moves({{7, 7}, {6, 6}, {8, 8}, {7, 8}, {9, 9}});
    assert(r.ok());
    assert(r.black_count == 3 && r.white_count == 2 && r.side_to_move == WHITE);
    assert(r.history.size() == 5);
    assert(r.history[0].color == BLACK && r.history[1].color == WHITE);
    assert(r.history[4].pos.row == 9 && r.history[4].pos.col == 9);

    // The list may open with either colour; the turn follows the counts.
    SetupResult white_first = position_from_moves({{7, 7}, {6, 6}, {8, 8}, {9, 9}}, WHITE);
    assert(white_first.ok());
    assert(white_first.black_count == 2 && white_first.white_count == 2);
    assert(white_first.side_to_move == BLACK);
    assert(white_first.history[0].color == WHITE);

    assert(!position_from_moves({{7, 7}, {8, 8}, {7, 7}}).ok());
    assert(!position_from_moves({{7, 7}, {15, 0}}).ok());
    assert(!position_from_moves({{7, 7}}, 0).ok());

    // An empty list is a legal position: nothing played, BLACK to move.
    SetupResult empty_list = position_from_moves({});
    assert(empty_list.ok() && empty_list.side_to_move == BLACK);

    std::cout << "test_moves_form passed" << std::endl;
}

void test_moves_array_size_is_capped() {
    // R85 G3: a moves array is an instruction from the client, and one far larger
    // than the board must be refused on its size - not walked entry by entry
    // until a coordinate happens to repeat. The cap is MAX_MOVES (the board's own
    // cell count, core/types.h), so the refusal has to name it.
    json doc;
    doc["version"] = 1;
    doc["type"] = "gomoku_position";
    json moves = json::array();
    for (int i = 0; i < 300000; ++i) {
        // Every entry is a legal coordinate, so only the count can reject this.
        moves.push_back(json::array({i % BOARD_SIZE, (i / BOARD_SIZE) % BOARD_SIZE}));
    }
    doc["moves"] = std::move(moves);

    SetupResult oversized = position_from_json(doc);
    assert(!oversized.ok());
    assert(!oversized.error.empty());
    assert(oversized.error.find(std::to_string(MAX_MOVES)) != std::string::npos);

    // A normal twenty-stone document still goes through untouched.
    json small;
    small["version"] = 1;
    json small_moves = json::array();
    for (int i = 0; i < 20; ++i) {
        small_moves.push_back(json::array({i % BOARD_SIZE, i / BOARD_SIZE}));
    }
    small["moves"] = std::move(small_moves);
    SetupResult normal = position_from_json(small);
    assert(normal.ok());
    assert(normal.black_count == 10 && normal.white_count == 10);

    std::cout << "test_moves_array_size_is_capped passed" << std::endl;
}

void test_validation_rules() {
    // Black opens, so the counts may only ever differ by one in black's favour.
    SetupResult skewed =
        position_from_text(grid_text({{0, 0}, {0, 1}, {0, 2}, {0, 3}}, {{1, 1}}, "1", "2", "0", ","));
    assert(!skewed.ok());
    assert(skewed.error.find("黑白差") != std::string::npos);

    SetupResult white_ahead =
        position_from_text(grid_text({{0, 0}}, {{1, 1}, {1, 2}}, "1", "2", "0", ","));
    assert(!white_ahead.ok());

    // A position that already contains a five is a finished game, not a start.
    SetupResult five = position_from_text(grid_text(
        {{0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}, {7, 7}},
        {{14, 0}, {14, 1}, {14, 3}, {14, 4}, {13, 13}}, "1", "2", "0", ","));
    assert(!five.ok());
    assert(five.error.find("五连") != std::string::npos);

    // WHITE wins on six or more, BLACK may not: an overline is a forbidden
    // length, so it is the forbidden scan that rejects it, not the winner scan.
    SetupResult overline = position_from_text(grid_text(
        {{0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}},
        {{14, 0}, {14, 1}, {14, 3}, {14, 4}, {13, 13}}, "1", "2", "0", ","));
    assert(!overline.ok());
    assert(overline.error.find("禁手") != std::string::npos);
    assert(overline.error.find("五连") == std::string::npos);

    // A BLACK stone sitting on a double-three square could never have been
    // played, so the position was never legal to build.
    SetupResult forbidden = position_from_text(grid_text(
        {{7, 6}, {7, 8}, {7, 7}, {6, 7}, {8, 7}},
        {{0, 0}, {1, 0}, {3, 0}, {4, 0}}, "1", "2", "0", ","));
    assert(!forbidden.ok());
    assert(forbidden.error.find("禁手") != std::string::npos);
    assert(forbidden.error.find("7,7") != std::string::npos);

    // The same frame without the centre is a legal position to hand over.
    SetupResult legal = position_from_text(grid_text(
        {{7, 6}, {7, 8}, {6, 7}, {8, 7}},
        {{0, 0}, {1, 0}, {3, 0}}, "1", "2", "0", ","));
    assert(legal.ok());
    assert(legal.side_to_move == WHITE);

    // A full board has no side to move.
    GridCells full(BOARD_SIZE, GridRow(BOARD_SIZE, "0"));
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) full[r][c] = (r + c) % 2 ? "1" : "2";
    }
    SetupResult full_board = position_from_grid(full);
    assert(!full_board.ok());
    assert(full_board.error.find("棋盘已满") != std::string::npos);

    // Ragged grids are refused before anything is read.
    GridCells ragged(BOARD_SIZE, GridRow(BOARD_SIZE, "0"));
    ragged[3].pop_back();
    assert(!position_from_grid(ragged).ok());
    GridCells short_board(14, GridRow(BOARD_SIZE, "0"));
    assert(!position_from_grid(short_board).ok());

    // validate_position also serves a board someone assembled by hand.
    Board hand;
    hand.place_stone(7, 7, BLACK);
    hand.place_stone(7, 8, BLACK);
    SetupResult two_black;
    validate_position(hand, two_black);
    assert(!two_black.ok());

    Board fair;
    fair.place_stone(7, 7, BLACK);
    fair.place_stone(7, 8, WHITE);
    SetupResult fair_result;
    validate_position(fair, fair_result);
    assert(fair_result.ok());
    assert(fair_result.black_count == 1 && fair_result.white_count == 1);
    assert(fair_result.side_to_move == BLACK);

    std::cout << "test_validation_rules passed" << std::endl;
}

void test_canonical_document() {
    // All three forms describe one position, and the exported document reads
    // back as that same position: that is what makes the template shareable
    // between the UI, the trainer and the tests.
    SetupResult grid = position_from_text(grid_text(kBlack, kWhite, "1", "2", "0", ","));
    assert(grid.ok());
    SetupResult from_moves =
        position_from_text(R"({"moves":[[7,7],[6,6],[8,8],[7,8],[9,9]]})");
    assert(from_moves.ok());
    assert(same_cells(grid, from_moves));

    std::string exported = position_to_json(grid.board);
    SetupResult back = position_from_text(exported);
    assert(back.ok());
    assert(same_cells(grid, back));
    assert(back.black_count == 3 && back.white_count == 2 && back.side_to_move == WHITE);
    assert(back.encoding.black == "1" && back.encoding.white == "2" && back.encoding.empty == "0");

    // The flat 225-value spelling of `board` is the same position again, and so
    // is the same grid handed over as text inside a document.
    json flat;
    flat["board"] = json::array();
    for (int value : cells_of(grid.board)) flat["board"].push_back(value);
    SetupResult from_flat = position_from_json(flat);
    assert(from_flat.ok() && same_cells(grid, from_flat));

    json as_text;
    as_text["board"] = grid_text(kBlack, kWhite, "B", "W", ".", "");
    SetupResult from_text_board = position_from_json(as_text);
    assert(from_text_board.ok() && same_cells(grid, from_text_board));

    // Malformed documents are refused with a reason, never guessed at.
    assert(!position_from_text(R"({"version":2,"moves":[[7,7]]})").ok());
    assert(!position_from_text(R"({"board":[[0]],"moves":[[7,7]]})").ok());
    assert(!position_from_text(R"({"version":1})").ok());
    assert(!position_from_text(R"({"moves":[[7,7]],"first":"middle"})").ok());
    assert(!position_from_text(R"({"board":[[0]],"first":"black"})").ok());
    assert(!position_from_text(R"({"bitboard":{"black":[0,0,0],"white":[0,0,0,0]}})").ok());
    assert(!position_from_text(R"({"bitboard":{"black":[0,"0x1ZZ",0,0],"white":[0,0,0,0]}})").ok());
    assert(!position_from_text(R"({"moves":[[7,7,7]]})").ok());
    assert(!position_from_text(R"({"moves":[[7,7]],})").ok());
    assert(!position_from_text(R"([1,2,3])").ok());
    assert(!position_from_text("   \n").ok());
    assert(position_from_text("1,2,3\n").error.find("15 格") != std::string::npos);

    std::cout << "test_canonical_document passed" << std::endl;
}

void test_shipped_templates() {
#ifdef GOMOKU_SOURCE_DIR
    const std::string root = GOMOKU_SOURCE_DIR;
    const char* names[] = {"data/position_template.json", "data/position_template.csv",
                           "data/position_template.txt", "data/position_bitboard_template.json",
                           "data/position_moves_template.json", "data/position_flat_template.txt",
                           "data/position_flat_template.csv"};
    for (const char* name : names) {
        SetupResult loaded;
        std::string error;
        assert(load_position_file(root + "/" + name, loaded, error));
        assert(loaded.ok());
        assert(loaded.black_count == 3 && loaded.white_count == 2);
        assert(loaded.side_to_move == WHITE);
        assert(same_cells(loaded, position_from_text(
                                   grid_text(kBlack, kWhite, "1", "2", "0", ","))));
    }
    SetupResult missing;
    std::string error;
    assert(!load_position_file(root + "/data/no_such_position.json", missing, error));
    assert(error.find("无法打开") != std::string::npos);
    std::cout << "test_shipped_templates passed (7 templates)" << std::endl;
#else
    std::cout << "test_shipped_templates skipped (no GOMOKU_SOURCE_DIR)" << std::endl;
#endif
}

void test_game_manager_position() {
    SetupResult pos = position_from_text(grid_text(kBlack, kWhite, "1", "2", "0", ","));
    assert(pos.ok());

    GameManager gm;
    bool committed = false;
    gm.set_move_callback([&committed](const Move&, int, int) { committed = true; });
    gm.set_config(300, 4);

    assert(gm.set_position(pos.board));
    assert(gm.position_pending());
    assert(gm.status() == GameStatus::WAITING);
    assert(gm.board().move_count() == 5);
    // The turn comes from the stone counts, exactly as the loader decided.
    assert(gm.current_turn() == WHITE);
    // Setting a position starts nothing: no worker, no replay entries.
    assert(!gm.is_ai_thinking());
    assert(gm.replay_snapshot().moves.empty());

    // A loaded position may be replaced again while it is still pending.
    assert(gm.set_position(pos.board));
    assert(gm.board().move_count() == 5);

    // Undo walks the loaded stones back off the board, one round at a time.
    int move_count = -1;
    GameStatus status = GameStatus::PLAYING;
    // In position_pending, undo is refused outright to protect the hand-set
    // board from being eaten stone by stone.
    assert(!gm.undo(move_count, status));
    assert(gm.position_pending());
    assert(gm.board().move_count() == 5);

    // A full board is refused outright: it has no side to move.
    Board full;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) full.place_stone(r, c, (r + c) % 2 ? BLACK : WHITE);
    }
    assert(full.move_count() == MAX_MOVES);
    assert(!gm.set_position(full));

    // Taking the side to move turns the position into a game, and the AI
    // answers the player's next stone.
    assert(gm.set_position(pos.board));
    gm.set_player_color(WHITE);
    assert(!gm.position_pending());
    assert(gm.make_move(5, 5));
    assert(gm.board().move_count() == 6);
    for (int i = 0; i < 200 && gm.is_ai_thinking(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(committed);
    assert(gm.board().move_count() == 7);
    assert(gm.current_turn() == WHITE);
    assert(gm.replay_snapshot().moves.size() == 2);

    // reset() drops the loaded position along with everything else.
    gm.reset();
    assert(!gm.position_pending());
    assert(gm.board().move_count() == 0);

    std::cout << "test_game_manager_position passed" << std::endl;
}

MoveAnalysis analysis(int row, int col, int color, int score = 0,
                      const char* method = "alphabeta") {
    MoveAnalysis entry;
    entry.move = Move(row, col, color);
    entry.score = score;
    entry.depth = 6;
    entry.nodes = 1234;
    entry.elapsed_ms = 5;
    entry.method = method;
    return entry;
}

void test_restore_game() {
    GameManager gm;
    gm.set_config(300, 4);

    // An empty record rebuilds an empty board but keeps the side choice, so a
    // session that was interrupted before the first stone still resumes as a
    // game rather than as a hand-set position.
    GameRestore opened;
    opened.player_color = BLACK;
    opened.status = GameStatus::PLAYING;
    assert(gm.restore_game(opened));
    assert(gm.player_color() == BLACK && gm.ai_color() == WHITE);
    assert(gm.board().move_count() == 0);
    assert(gm.status() == GameStatus::PLAYING);
    assert(!gm.position_pending());

    const std::vector<MoveAnalysis> moves{
        analysis(7, 7, BLACK, 120),
        analysis(8, 8, WHITE, -120, "book"),
        analysis(6, 6, BLACK, 180),
    };

    GameRestore plain;
    plain.moves = moves;
    plain.player_color = BLACK;
    plain.status = GameStatus::PLAYING;
    assert(gm.restore_game(plain));
    assert(gm.board().move_count() == 3);
    assert(gm.board().get(7, 7) == BLACK);
    assert(gm.board().get(8, 8) == WHITE);
    assert(gm.current_turn() == WHITE);
    // The replay numbers come back with the stones, so a resumed game still has
    // a curve to show.
    ReplaySnapshot snap = gm.replay_snapshot();
    assert(snap.moves.size() == 3);
    assert(snap.moves[1].method == "book" && snap.moves[1].score == -120);
    assert(snap.moves[0].depth == 6 && snap.moves[0].nodes == 1234);
    assert(snap.status != GameStatus::GAME_OVER && snap.winner == EMPTY);
    // A game that started from nothing has a self-contained record.
    assert(snap.base.empty() && snap.move_count == 3);
    // Restoring never arms a search on its own.
    assert(!gm.is_ai_thinking());

    // A finished game keeps its ending.
    GameRestore over = plain;
    over.status = GameStatus::GAME_OVER;
    over.winner = BLACK;
    over.reason = "five";
    assert(gm.restore_game(over));
    assert(gm.status() == GameStatus::GAME_OVER);
    assert(gm.winner() == BLACK);
    snap = gm.replay_snapshot();
    assert(snap.status == GameStatus::GAME_OVER && snap.winner == BLACK && snap.reason == "five");
    assert(snap.moves.size() == 3);

    // An empty player colour leaves the earlier choice alone: a session saved
    // before anyone picked a side keeps whichever side the process already had.
    gm.reset();
    gm.set_player_color(WHITE);
    GameRestore unchosen = plain;
    unchosen.player_color = EMPTY;
    assert(gm.restore_game(unchosen));
    assert(gm.player_color() == WHITE);

    // Records that cannot be replayed are refused, and the board from the
    // previous restore survives them.
    GameRestore overlap = plain;
    overlap.moves = {analysis(7, 7, BLACK), analysis(7, 7, WHITE)};
    assert(!gm.restore_game(overlap));
    assert(gm.board().move_count() == 3);
    GameRestore no_colour = plain;
    no_colour.moves = {analysis(9, 9, EMPTY)};
    assert(!gm.restore_game(no_colour));
    GameRestore off_board = plain;
    off_board.moves = {analysis(BOARD_SIZE, 0, BLACK)};
    assert(!gm.restore_game(off_board));
    GameRestore bad_base = plain;
    bad_base.base = {Move(7, 7, BLACK)};
    assert(!gm.restore_game(bad_base));
    assert(gm.board().move_count() == 3);

    // A pending position rebuilds as pending, so choose_side stays usable.
    GameRestore pending = plain;
    pending.player_color = EMPTY;
    pending.status = GameStatus::WAITING;
    pending.position_pending = true;
    assert(gm.restore_game(pending));
    assert(gm.position_pending());
    assert(gm.status() == GameStatus::WAITING);

    std::cout << "test_restore_game passed" << std::endl;
}

// The save side of the same contract: what a session writes has to be enough to
// rebuild the board, including the stones a hand-set position laid down that no
// played move accounts for.
void test_snapshot_round_trip() {
    GameManager gm;
    SetupResult pos = position_from_text(grid_text(kBlack, kWhite, "1", "2", "0", ","));
    assert(pos.ok());
    assert(gm.set_position(pos.board));

    ReplaySnapshot snap = gm.replay_snapshot();
    assert(snap.move_count == 5);
    // Nothing has been played, so every stone on the board is a base stone.
    assert(snap.moves.empty() && snap.base.size() == 5);

    // Base stones are not a history and cannot be undone: a record that is
    // nothing but base stones has no analysed move to give back, so the undo is
    // refused and the setup stays exactly as it was installed. Status is PLAYING
    // on purpose - the position_pending_ guard would mask the refusal.
    GameManager pure;
    GameRestore base_only;
    base_only.base = snap.base;
    base_only.player_color = BLACK;
    base_only.status = GameStatus::PLAYING;
    assert(pure.restore_game(base_only));
    assert(!pure.position_pending());
    const std::vector<int> installed = cells_of(pure.board());
    int refused_count = 12345;
    GameStatus refused_status = GameStatus::GAME_OVER;
    assert(!pure.undo(refused_count, refused_status));
    assert(refused_count == 12345 && refused_status == GameStatus::GAME_OVER);
    assert(pure.board().move_count() == 5);
    assert(cells_of(pure.board()) == installed);
    snap = pure.replay_snapshot();
    assert(snap.move_count == 5 && snap.moves.empty() && snap.base.size() == 5);

    // The player took the position up as BLACK and the AI answered one stone, so
    // the session is five installed stones plus one played move - and only the
    // played move carries engine numbers.
    GameRestore restored;
    restored.base = snap.base;
    restored.moves = {analysis(5, 5, WHITE, 42, "alphabeta")};
    restored.player_color = BLACK;
    restored.status = GameStatus::PLAYING;
    GameManager second;
    assert(second.restore_game(restored));
    assert(second.board().move_count() == 6);
    assert(second.board().get(5, 5) == WHITE);
    assert(second.board().get(9, 9) == BLACK);
    snap = second.replay_snapshot();
    assert(snap.move_count == 6);
    assert(snap.moves.size() == 1 && snap.base.size() == 5);

    // Undo takes back the analysed move that was played and nothing else: with
    // only one analysed stone behind five installed ones, the clamping keeps the
    // pop on that stone and the split does not grow past it. move_count returns
    // to the installed count, and feeding the record back rebuilds five stones.
    second.set_config(60000, 8);
    int move_count = -1;
    GameStatus status = GameStatus::WAITING;
    assert(second.undo(move_count, status));
    // The undone AI stone re-arms the worker for the square it just lost; halt
    // it before asserting so it cannot commit a replacement under the checks.
    second.halt_search();
    assert(move_count == 5 && status == GameStatus::PLAYING && !second.is_ai_thinking());
    snap = second.replay_snapshot();
    assert(snap.move_count == 5);
    assert(snap.moves.empty() && snap.base.size() == 5);
    for (const Move& stone : snap.base) {
        assert(second.board().get(stone.pos.row, stone.pos.col) == stone.color);
    }
    // Feeding that record back in rebuilds exactly the board it came from.
    GameRestore again;
    again.base = snap.base;
    again.moves = snap.moves;
    again.player_color = BLACK;
    GameManager third;
    assert(third.restore_game(again));
    assert(third.board().move_count() == 5);
    assert(third.board().get(5, 5) == EMPTY);
    assert(third.board().get(9, 9) == BLACK);
    assert(third.replay_snapshot().base.size() == 5);

    std::cout << "test_snapshot_round_trip passed" << std::endl;
}

// A hand-set position nobody has taken up yet is the player's setup, not a
// game: an undo must never eat its base stones.
void test_undo_rejected_in_position_pending() {
    Board placed;
    assert(placed.place_stone(7, 7, BLACK));
    assert(placed.place_stone(6, 6, WHITE));
    assert(placed.place_stone(8, 8, BLACK));
    assert(placed.place_stone(7, 8, WHITE));
    assert(placed.place_stone(9, 9, BLACK));

    GameManager gm;
    assert(gm.set_position(placed));
    assert(gm.position_pending());
    const int before = gm.board().move_count();
    assert(before == 5);

    int move_count = -1;
    GameStatus status = GameStatus::PLAYING;
    const bool accepted = gm.undo(move_count, status);

    std::cout << "undo in position_pending: returned=" << (accepted ? "true" : "false")
              << ", board move_count " << before << " -> " << gm.board().move_count()
              << ", out_move_count=" << move_count << std::endl;
    if (accepted && move_count != before) {
        std::cout << "FAIL: undo in position_pending was accepted and took "
                  << (before - move_count) << " base stone(s) off the board" << std::endl;
    }

    // Expected: the undo is refused outright. Should a fix instead accept it as
    // a no-op, the board still has to be exactly the setup that was loaded.
    assert(!accepted || move_count == before);
    assert(gm.board().move_count() == before);
    assert(cells_of(gm.board()) == cells_of(placed));
    assert(gm.position_pending());

    std::cout << "test_undo_rejected_in_position_pending passed" << std::endl;
}

// Round 79, A7: a forced encoding used to be trusted as stated, so a mapping that
// names the same token twice read every white stone as black (black == white) or
// quietly deleted a side (an empty that collides with either colour). The three
// tokens have to be three different things before the coverage test runs, and the
// message has to say which symbol clashed.
void test_forced_encoding_rejects_collisions() {
    GridCells cells;
    std::string error;
    assert(parse_grid_text(grid_text(kBlack, kWhite, "1", "2", "0", ","), cells, error));

    // black == white: the clash is reported before the grid's own "1"/"2"/"0"
    // tokens get a chance to turn it into a coverage error.
    {
        GridEncoding forced;
        forced.black = "X";
        forced.white = "X";
        forced.empty = ".";
        SetupResult r = position_from_grid(cells, &forced);
        assert(!r.ok());
        assert(r.error.find("X") != std::string::npos);
        assert(r.error.find("黑和白") != std::string::npos);
    }
    // black == empty.
    {
        GridEncoding forced;
        forced.black = "*";
        forced.white = "2";
        forced.empty = "*";
        SetupResult r = position_from_grid(cells, &forced);
        assert(!r.ok());
        assert(r.error.find("*") != std::string::npos);
        assert(r.error.find("黑和空") != std::string::npos);
    }
    // white == empty.
    {
        GridEncoding forced;
        forced.black = "1";
        forced.white = "#";
        forced.empty = "#";
        SetupResult r = position_from_grid(cells, &forced);
        assert(!r.ok());
        assert(r.error.find("#") != std::string::npos);
        assert(r.error.find("白和空") != std::string::npos);
    }
    // The clash is the normalized reading, not the typed one: "b" against "B" is
    // one token, and the message shows the folded form.
    {
        GridEncoding forced;
        forced.black = "b";
        forced.white = "B";
        forced.empty = ".";
        SetupResult r = position_from_grid(cells, &forced);
        assert(!r.ok());
        assert(r.error.find("B") != std::string::npos);
        assert(r.error.find("黑和白") != std::string::npos);
    }
    // Three distinct tokens keep behaving exactly as before, including the
    // opposite reading detection would not pick.
    {
        GridEncoding forced;
        forced.black = "1";
        forced.white = "2";
        forced.empty = "0";
        SetupResult r = position_from_grid(cells, &forced);
        assert(r.ok());
        assert(r.black_count == 3 && r.white_count == 2);
        assert(r.board.get(9, 9) == BLACK && r.board.get(7, 8) == WHITE);
    }
    // And a stated mapping that is internally distinct but does not cover the
    // grid still gives the coverage error, not a clash error.
    {
        GridEncoding forced;
        forced.black = "B";
        forced.white = "W";
        forced.empty = ".";
        SetupResult r = position_from_grid(cells, &forced);
        assert(!r.ok());
        assert(r.error.find("不含") != std::string::npos);
    }

    std::cout << "test_forced_encoding_rejects_collisions passed" << std::endl;
}

// Round 79, A8: a bitboard word was read with std::stoull's base 0, which takes a
// leading zero to mean octal, so "010" became 8 and every stone landed on another
// square while the position still validated. Only 0x / 0X is hexadecimal now;
// everything else is decimal.
void test_bitboard_words_are_decimal() {
    // 10 decimal sets cells (0,1) and (0,3); octal 010 is 8, which sets only
    // (0,3) - the stone at (0,1) is what the old read dropped.
    std::array<uint64_t, BITBOARD_WORDS> black{};
    std::array<uint64_t, BITBOARD_WORDS> white{};
    black[0] = 10;
    white[0] = 5;
    SetupResult direct = position_from_bitboard(black, white);
    assert(direct.ok());

    SetupResult padded = position_from_json(json::parse(
        R"({"bitboard":{"black":["010","0","0","0"],"white":["5","0","0","0"]}})"));
    assert(padded.ok());
    assert(padded.board.get(0, 1) == BLACK);
    assert(padded.board.get(0, 3) == BLACK);
    assert(padded.board.get(0, 0) == WHITE);
    assert(padded.board.get(0, 2) == WHITE);
    assert(same_cells(direct, padded));

    // A longer zero pad lands nowhere near the octal reading: 0016 is decimal 16,
    // cell (0,4), where octal 0016 would be 14 - cells (0,1), (0,2) and (0,3).
    std::array<uint64_t, BITBOARD_WORDS> one{};
    one[0] = 16;
    std::array<uint64_t, BITBOARD_WORDS> none{};
    SetupResult as_sixteen = position_from_bitboard(one, none);
    assert(as_sixteen.ok());
    SetupResult zero_padded = position_from_json(json::parse(
        R"({"bitboard":{"black":["0016","0","0","0"],"white":["0","0","0","0"]}})"));
    assert(zero_padded.ok());
    assert(zero_padded.board.get(0, 4) == BLACK);
    assert(zero_padded.board.is_empty(0, 1));
    assert(zero_padded.board.is_empty(0, 2));
    assert(zero_padded.board.is_empty(0, 3));
    assert(same_cells(as_sixteen, zero_padded));

    // 0x and 0X still mean hexadecimal, and both spellings of one mask agree.
    SetupResult lower_hex = position_from_json(json::parse(
        R"({"bitboard":{"black":["0x10","0","0","0"],"white":["0","0","0","0"]}})"));
    SetupResult upper_hex = position_from_json(json::parse(
        R"({"bitboard":{"black":["0X10","0","0","0"],"white":["0","0","0","0"]}})"));
    assert(lower_hex.ok() && upper_hex.ok());
    assert(same_cells(as_sixteen, lower_hex));
    assert(same_cells(as_sixteen, upper_hex));

    // An unadorned decimal is unchanged.
    SetupResult plain = position_from_json(json::parse(
        R"({"bitboard":{"black":["10","0","0","0"],"white":["5","0","0","0"]}})"));
    assert(plain.ok());
    assert(same_cells(direct, plain));

    // The zero-padded mask moves no stone into a neighbour on the last word
    // either: word 3 covers the final rows, and reading it as octal would both
    // drop (13,1) and collide a white stone with a black one.
    std::array<uint64_t, BITBOARD_WORDS> high_black{};
    std::array<uint64_t, BITBOARD_WORDS> high_white{};
    high_black[3] = 10;   // cells 193 and 195
    high_white[3] = 16;   // cell 196
    SetupResult high_direct = position_from_bitboard(high_black, high_white);
    assert(high_direct.ok());
    SetupResult high_padded = position_from_json(json::parse(
        R"({"bitboard":{"black":["0","0","0","000010"],"white":["0","0","0","000016"]}})"));
    assert(high_padded.ok());
    assert(high_padded.board.get(12, 13) == BLACK);
    assert(high_padded.board.get(13, 0) == BLACK);
    assert(high_padded.board.get(13, 1) == WHITE);
    assert(same_cells(high_direct, high_padded));

    std::cout << "test_bitboard_words_are_decimal passed" << std::endl;
}

// Round 90, U3: fifteen rows of which one is the wrong width is a layout slip on
// a grid, and 第 N 行有 M 格 names the line to fix. Until now the row complaint
// was only kept when nothing was ragged, so this document fell through to the
// flat reading and came back as a cell total - which cannot tell one short line
// apart from a truncated dump.
void test_ragged_row_in_a_fifteen_row_grid_names_the_row() {
    GridCells cells;
    std::string error;

    // The third line loses its last cell: 224 cells over 15 rows.
    std::string short_row;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            if (r == 2 && c == BOARD_SIZE - 1) continue;
            if (c) short_row += ",";
            short_row += "0";
        }
        short_row += '\n';
    }
    assert(!parse_grid_text(short_row, cells, error));
    assert(cells.empty());
    assert(error.find("第 3 行有 14 格") != std::string::npos);

    // The mirror image, one cell too many on the same line, is named the same
    // way: the reading is about the row, not about how far the total is off.
    std::string long_row;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE + (r == 2 ? 1 : 0); ++c) {
            if (c) long_row += ",";
            long_row += "0";
        }
        long_row += '\n';
    }
    assert(!parse_grid_text(long_row, cells, error));
    assert(cells.empty());
    assert(error.find("第 3 行有 16 格") != std::string::npos);

    std::cout << "test_ragged_row_in_a_fifteen_row_grid_names_the_row passed ("
              << error << ")" << std::endl;
}

int main() {
    std::cout << "Running position setup tests..." << std::endl;
    test_grid_parsing();
    test_flat_strings();
    test_encoding_detection();
    test_forced_encoding_rejects_collisions();
    test_bitboard_form();
    test_bitboard_words_are_decimal();
    test_moves_form();
    test_moves_array_size_is_capped();
    test_validation_rules();
    test_canonical_document();
    test_shipped_templates();
    test_game_manager_position();
    test_restore_game();
    test_snapshot_round_trip();
    test_undo_rejected_in_position_pending();
    test_ragged_row_in_a_fifteen_row_grid_names_the_row();
    std::cout << "All position setup tests passed!" << std::endl;
    return 0;
}

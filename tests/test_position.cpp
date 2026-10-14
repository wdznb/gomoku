#include "core/position_setup.h"
#include "server/position_json.h"
#include "server/game_manager.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <sys/resource.h>
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

// R91 V2: the array form writes an empty point as "". Detection reads that as
// empty and echoes back its family's canonical symbol, so the confirmed mapping
// a client sends back (README:539) has to cover the blanks it was handed -
// otherwise re-reading the same cells under the echo it produced is refused.
void test_blank_cells_reread_under_the_echoed_encoding() {
    // 15 rows of 15 tokens with one black stone, one white stone and the rest
    // spelled `blank`, which is how a UI round-trip actually looks.
    auto rows = [](const char* black, const char* white, const char* blank) {
        json grid = json::array();
        for (int r = 0; r < BOARD_SIZE; ++r) {
            json row = json::array();
            for (int c = 0; c < BOARD_SIZE; ++c) {
                if (r == 7 && c == 7) row.push_back(black);
                else if (r == 6 && c == 6) row.push_back(white);
                else row.push_back(blank);
            }
            grid.push_back(std::move(row));
        }
        return grid;
    };

    struct Family { const char* black; const char* white; const char* blank; const char* echoed; };
    // The two canonical empty symbols the echo can carry for a blank-filled
    // array: "." for the letter families, "空" for the Chinese one.
    const Family families[] = {{"B", "W", "", "."}, {"黑", "白", "", "空"}};

    for (const Family& family : families) {
        json doc;
        doc["version"] = 1;
        doc["board"] = rows(family.black, family.white, family.blank);

        const SetupResult detected = position_from_json(doc);
        assert(detected.ok());
        assert(detected.encoding.black == family.black);
        assert(detected.encoding.white == family.white);
        assert(detected.encoding.empty == family.echoed);
        assert(detected.black_count == 1 && detected.white_count == 1);

        // Hand that very echo back as the confirmed mapping over the same cells.
        doc["encoding"] = {{"black", detected.encoding.black},
                           {"white", detected.encoding.white},
                           {"empty", detected.encoding.empty}};
        const SetupResult forced = position_from_json(doc);
        assert(forced.ok());
        assert(same_cells(detected, forced));
        assert(forced.black_count == 1 && forced.white_count == 1);
    }

    std::cout << "test_blank_cells_reread_under_the_echoed_encoding passed" << std::endl;
}

// Anti-overreach for V2: the blank only counts as covered when the stated empty
// symbol is one of the spellings the detector treats as empty. Under a numeric
// mapping, and for a symbol no alphabet defines at all, the coverage guard still
// has to refuse the grid instead of quietly dropping a stone.
void test_blank_cell_is_stray_outside_the_letter_families() {
    auto rows = [](const char* black, const char* white, const char* blank, const char* stray) {
        json grid = json::array();
        for (int r = 0; r < BOARD_SIZE; ++r) {
            json row = json::array();
            for (int c = 0; c < BOARD_SIZE; ++c) {
                if (r == 7 && c == 7) row.push_back(black);
                else if (r == 6 && c == 6) row.push_back(white);
                else if (r == 3 && c == 3 && stray) row.push_back(stray);
                else row.push_back(blank);
            }
            grid.push_back(std::move(row));
        }
        return grid;
    };

    // 0/1/2 is a legal mapping, and "" is not one of its spellings for anything.
    json numeric;
    numeric["version"] = 1;
    numeric["board"] = rows("1", "2", "", nullptr);
    numeric["encoding"] = {{"black", "1"}, {"white", "2"}, {"empty", "0"}};
    const SetupResult refused = position_from_json(numeric);
    assert(!refused.ok());
    assert(!refused.error.empty());

    // The same blank grid under the mapping that does cover it is accepted, so
    // the refusal above is about the numeric flags, not about the document.
    json numeric_blank;
    numeric_blank["version"] = 1;
    numeric_blank["board"] = rows("1", "2", "0", nullptr);
    numeric_blank["encoding"] = {{"black", "1"}, {"white", "2"}, {"empty", "0"}};
    const SetupResult accepted = position_from_json(numeric_blank);
    assert(accepted.ok());

    // A symbol no alphabet defines stays a stray token even under a mapping that
    // does cover blanks.
    json stray;
    stray["version"] = 1;
    stray["board"] = rows("B", "W", "", "?");
    stray["encoding"] = {{"black", "B"}, {"white", "W"}, {"empty", "."}};
    const SetupResult rejected = position_from_json(stray);
    assert(!rejected.ok());
    assert(!rejected.error.empty());

    // Same cells, stated symbol instead of the blank: the letter mapping reads it.
    json clean;
    clean["version"] = 1;
    clean["board"] = rows("B", "W", ".", nullptr);
    clean["encoding"] = {{"black", "B"}, {"white", "W"}, {"empty", "."}};
    const SetupResult read = position_from_json(clean);
    assert(read.ok());
    assert(same_cells(read, accepted));

    std::cout << "test_blank_cell_is_stray_outside_the_letter_families passed" << std::endl;
}

// Round 98 CC1: the flat form used to be exploded into one std::string per
// codepoint before its length was judged, so a run far past 225 cells paid for
// the whole container before it could be refused. The scan is single-pass now -
// the total keeps counting, symbols are only kept while they can still become a
// board, and a fourth kind of mark stops the run at once. The bounds below are
// deliberately loose: they pin the cost class (one pass over the text, no
// per-symbol container), not a speed, and no wording is asserted.
void test_oversized_flat_run_is_scanned_in_one_pass() {
    const size_t bytes = 16 * 1024 * 1024;  // one 16 MB line of dots, no separators
    const std::string huge(bytes, '.');

    rusage before{};
    getrusage(RUSAGE_SELF, &before);
    const auto start = std::chrono::steady_clock::now();
    GridCells cells;
    std::string error;
    const bool ok = parse_grid_text(huge, cells, error);
    const long long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    rusage after{};
    getrusage(RUSAGE_SELF, &after);
    // ru_utime is charged by the kernel in quanta of CPU, so unlike the wall clock above it
    // is not a meter of how many other processes wanted this core.
    const long long cpu_ms =
        (after.ru_utime.tv_sec - before.ru_utime.tv_sec) * 1000LL +
        (after.ru_utime.tv_usec - before.ru_utime.tv_usec) / 1000LL;

    assert(!ok);
    assert(!error.empty());
    // The complaint is the cell count, so it carries the 225 it wanted.
    assert(error.find("225") != std::string::npos);
    assert(cells.empty());
    std::cout << "  [r98cc1] 16 MB run refused in " << elapsed_ms
              << " ms wall / " << cpu_ms << " ms cpu, peak RSS "
              << (after.ru_maxrss / 1024) << " MB" << std::endl;
    // Round 111 OO3: the wall clock above used to be a *pass* condition, and a wall clock
    // is a load meter - on a saturated 12-core box this same one-pass scan read 27602 ms.
    // CPU time is not the same meter (measured here: 6947 ms idle, 13324 ms under the same
    // 24-way saturation) but it cannot carry the claim either: the gap it would have to
    // straddle is 6.9 s now against ~13 s for the per-symbol container it replaced, so no
    // threshold is both >= 3x this scan and below 13 s - and a saturated box reaches that
    // number on its own. The cost class is therefore carried by the one bound that does
    // separate the two versions: 1155 MB peak for the container against the 135 MB here, an
    // order of magnitude with no clock in it. Both timings above stay printed as evidence,
    // asserted on nothing.
    // ru_maxrss is the process high-water mark in KB. The text itself is 16 MB and
    // the readers copy it a few times; a vector of one string per codepoint lands
    // an order of magnitude above that.
    assert(after.ru_maxrss < 250LL * 1024);

    // Priority is unchanged: three legal marks plus a fourth is the symbol-kind
    // complaint however long the run is, and that complaint names no cell count.
    std::string four_kinds;
    for (int i = 0; i < 300; ++i) {
        four_kinds += (i % 3 == 0) ? "黑" : (i % 3 == 1) ? "白" : "空";
    }
    four_kinds += "?";
    std::string kind_error;
    assert(!parse_grid_text(four_kinds, cells, kind_error));
    assert(!kind_error.empty());
    assert(kind_error.find("三种") != std::string::npos);
    assert(kind_error.find("225") == std::string::npos);

    // The same 300 symbols without the stray mark are read as a length problem.
    const std::string three_kinds = four_kinds.substr(0, four_kinds.size() - 1);
    std::string count_error;
    assert(!parse_grid_text(three_kinds, cells, count_error));
    assert(count_error.find("225") != std::string::npos);

    std::cout << "test_oversized_flat_run_is_scanned_in_one_pass passed" << std::endl;
}

// Round 98 CC2: a UTF-8 BOM is not content. Left at the front it is the first
// character the shape test reads, so a legal JSON document was handed to the grid
// reader and refused for the wrong cell count. The reader now steps over the
// three bytes before deciding the form and starts the JSON parse past them, and a
// payload shorter than three bytes must not read past its end.
void test_bom_prefix_does_not_change_the_shape() {
    const std::string bom = "\xEF\xBB\xBF";

    json matrix = json::array();
    for (int r = 0; r < BOARD_SIZE; ++r) {
        json row = json::array();
        for (int c = 0; c < BOARD_SIZE; ++c) row.push_back(0);
        matrix.push_back(std::move(row));
    }
    matrix[7][7] = 1;  // black
    matrix[8][8] = 1;  // black
    matrix[6][6] = 2;  // white
    json doc;
    doc["board"] = matrix;
    doc["encoding"] = {{"black", "1"}, {"white", "2"}, {"empty", "0"}};
    const std::string text = doc.dump();

    const SetupResult plain = position_from_text(text);
    const SetupResult prefixed = position_from_text(bom + text);
    const SetupResult bom_newline = position_from_text(bom + "\n" + text);
    assert(plain.ok());
    assert(prefixed.ok());
    assert(bom_newline.ok());
    assert(same_cells(plain, prefixed));
    assert(same_cells(plain, bom_newline));
    // Stated the way the preview states it: stones of each colour and the side to
    // move, so a BOM that slipped into the board would show up here too.
    assert(prefixed.black_count == plain.black_count && prefixed.black_count == 2);
    assert(prefixed.white_count == plain.white_count && prefixed.white_count == 1);
    assert(prefixed.side_to_move == plain.side_to_move);
    assert(bom_newline.black_count == plain.black_count);
    assert(bom_newline.side_to_move == plain.side_to_move);

    // The grid branch keeps working with a BOM in front of it: that reader already
    // stripped one, and the text it is handed still carries it.
    const std::string grid = grid_text(kBlack, kWhite, "1", "2", "0", ",");
    const SetupResult grid_plain = position_from_text(grid);
    const SetupResult grid_bom = position_from_text(bom + grid);
    assert(grid_plain.ok() && grid_bom.ok());
    assert(same_cells(grid_plain, grid_bom));

    // Short payloads are clean refusals, not an out-of-bounds read.
    GridCells nope;
    const std::string cases[] = {"", "\xEF", "\xEF\xBB", bom};
    for (const std::string& payload : cases) {
        const SetupResult short_read = position_from_text(payload);
        assert(!short_read.ok());
        assert(!short_read.error.empty());
        nope.clear();
        std::string error;
        assert(!parse_grid_text(payload, nope, error));
        assert(!error.empty());
    }

    std::cout << "test_bom_prefix_does_not_change_the_shape passed" << std::endl;
}

// Round 99 DD4: the flat scan used to leave the run as soon as it had seen a
// fourth kind of mark, and the complaint printed the count it stopped at - so a
// run that mixes six or eight alphabets was reported as a four-symbol board and
// only its first four marks were named. That is the contract round 98 CC1
// recorded wrongly. The scan now visits every codepoint and only the *storing* of
// symbols stays capped at the 225 a board needs; the priority between the two
// complaints is unchanged (README:259-260, :305). The chosen stray marks contain
// no digits, so the only ASCII digits in the message are the count itself, and
// the list's completeness is read off the marks it names - no wording is asserted.
void test_every_symbol_kind_of_an_oversized_run_is_named() {
    GridCells cells;

    // Three marks the table defines and two it never does, cycled past 225 cells:
    // five kinds over 400 codepoints.
    std::string five_kinds;
    for (int i = 0; i < 80; ++i) five_kinds += "黑白空?%";
    std::string error5;
    assert(!parse_grid_text(five_kinds, cells, error5));
    assert(!error5.empty());
    assert(cells.empty());
    std::cout << "  [r99dd4] five-kind report: " << error5 << std::endl;
    // The scan ran to the end, so the fifth mark is named and the count is 5.
    assert(error5.find("?") != std::string::npos);
    assert(error5.find("%") != std::string::npos);
    assert(error5.find("5") != std::string::npos);
    // The symbol-kind complaint still precedes the cell count, which it never
    // states: 400 cells and no 225 in the message.
    assert(error5.find("225") == std::string::npos);

    // Eight kinds over 320 codepoints: the list tracks the input instead of
    // stopping at four.
    std::string eight_kinds;
    for (int i = 0; i < 40; ++i) eight_kinds += "黑白空?!@&%";
    std::string error8;
    assert(!parse_grid_text(eight_kinds, cells, error8));
    std::cout << "  [r99dd4] eight-kind report: " << error8 << std::endl;
    assert(error8.find("!") != std::string::npos);
    assert(error8.find("@") != std::string::npos);
    assert(error8.find("&") != std::string::npos);
    assert(error8.find("%") != std::string::npos);
    assert(error8.find("8") != std::string::npos);
    assert(error8.find("225") == std::string::npos);

    std::cout << "test_every_symbol_kind_of_an_oversized_run_is_named passed" << std::endl;
}

// Round 109 MM1: a grid cell written as a JSON number above INT64_MAX is a value no
// document ever named as a mark. token_of() sent every is_number_integer() cell through
// get<long long>(), and nlohmann's arithmetic conversion for a number_unsigned is a bare
// static_cast - it does not throw - so 18446744073709551615 arrived as the token "-1",
// which README:240 lists as a legitimate WHITE symbol. A stone the file never asked for.
// README:215 is the house rule against reading a 64-bit integer through a truncating cast,
// and README:245-246 says an unrecognized symbol is refused and *shown back*, never guessed.
// The fix branches is_number_unsigned() first, exactly as word_of() already did at :69.
void test_cell_number_beyond_int64_is_never_read_as_minus_one() {
    // 2^64-1. Over INT64_MAX, so the signed reader used for every JSON integer truncates it.
    const uint64_t beyond_int64 = 18446744073709551615ULL;
    // A 15x15 array carrying the three given cell values: `white` on (7,7), `black` on
    // (6,6) and `blank` everywhere else. Numbers stay numbers and strings stay strings,
    // which is what separates the two paths this case is about.
    auto board_of = [](const json& black, const json& white, const json& blank) {
        json rows_json = json::array();
        for (int r = 0; r < BOARD_SIZE; ++r) {
            json row = json::array();
            for (int c = 0; c < BOARD_SIZE; ++c) {
                if (r == 7 && c == 7) row.push_back(white);
                else if (r == 6 && c == 6) row.push_back(black);
                else row.push_back(blank);
            }
            rows_json.push_back(std::move(row));
        }
        return rows_json;
    };
    const json blank = "0";
    const json oversized = beyond_int64;

    json doc;
    doc["version"] = 1;
    doc["board"] = board_of(json(0), oversized, json(0));
    const SetupResult refused = position_from_json(doc);
    // The refusal every reader needs: the grid holds a symbol nothing defines.
    assert(!refused.ok());
    // No stone at all survives it. On HEAD a WHITE sits right here, and the count guard
    // then rejects that document for a *different* reason (黑 0 子、白 1 子), so !ok()
    // alone proves nothing - the empty board is the observable difference.
    assert(refused.black_count == 0 && refused.white_count == 0);
    assert(refused.board.get(7, 7) == EMPTY);
    // README:246: 拒绝并列出看到的符号. The number it echoes is the number the file
    // stated, not the one the cast invented.
    std::cout << "  [r109mm1] refusal names: " << refused.error << std::endl;
    assert(refused.error.find("18446744073709551615") != std::string::npos);

    // The same oversized cell with a legal black companion: on HEAD the truncated token
    // is covered by README:240's second row (1 / -1 / 0), the counts balance, and the
    // whole document *loads* - a phantom white stone placed as if it had been written.
    json paired;
    paired["version"] = 1;
    paired["board"] = board_of(json(1), oversized, json(0));
    const SetupResult paired_refused = position_from_json(paired);
    std::cout << "  [r109mm1] paired ok=" << (paired_refused.error.empty() ? "true" : "false")
              << " 黑" << paired_refused.black_count << " 白" << paired_refused.white_count
              << std::endl;
    assert(!paired_refused.ok());
    assert(paired_refused.black_count == 0 && paired_refused.white_count == 0);
    assert(paired_refused.board.get(6, 6) == EMPTY && paired_refused.board.get(7, 7) == EMPTY);

    // Control, green before and after: the documented 1 / -1 / 0 convention written as
    // text really does place a white stone. The guard above must not touch it. (It needs
    // the black companion too - a lone white is not a position a game reaches, and the
    // count rule would reject the document for the wrong reason.)
    json signed_form;
    signed_form["version"] = 1;
    signed_form["board"] = board_of(json("1"), json("-1"), blank);
    const SetupResult signed_read = position_from_json(signed_form);
    assert(signed_read.ok());
    assert(signed_read.black_count == 1 && signed_read.white_count == 1);
    assert(signed_read.encoding.white == "-1");
    assert(signed_read.board.get(7, 7) == WHITE);

    // Control: the ordinary small-integer grid is unchanged by the unsigned branch, with
    // and without a stated encoding - 0/1/2 all sit below INT64_MAX either way.
    json numeric;
    numeric["version"] = 1;
    numeric["board"] = board_of(json(1), json(2), json(0));
    const SetupResult detected = position_from_json(numeric);
    assert(detected.ok());
    assert(detected.black_count == 1 && detected.white_count == 1);

    json stated = numeric;
    stated["encoding"] = {{"black", 1}, {"white", 2}, {"empty", 0}};
    const SetupResult forced = position_from_json(stated);
    assert(forced.ok());
    assert(same_cells(detected, forced));
    assert(forced.black_count == 1 && forced.white_count == 1);

    // A stated encoding does not need the fix to refuse the oversized cell: the coverage
    // guard rejects an uncovered token whatever text produced it. Both trees refuse with
    // no stone placed, so this is a control - the reports differ only in the symbol they
    // name, which is printed above rather than asserted.
    json stated_oversized;
    stated_oversized["version"] = 1;
    stated_oversized["board"] = board_of(json(1), oversized, json(0));
    stated_oversized["encoding"] = {{"black", 1}, {"white", 2}, {"empty", 0}};
    const SetupResult oversized_forced = position_from_json(stated_oversized);
    std::cout << "  [r109mm1] stated-encoding refusal names: " << oversized_forced.error
              << std::endl;
    assert(!oversized_forced.ok());
    assert(oversized_forced.black_count == 0 && oversized_forced.white_count == 0);

    std::cout << "test_cell_number_beyond_int64_is_never_read_as_minus_one passed" << std::endl;
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
    test_blank_cells_reread_under_the_echoed_encoding();
    test_blank_cell_is_stray_outside_the_letter_families();
    test_oversized_flat_run_is_scanned_in_one_pass();
    test_bom_prefix_does_not_change_the_shape();
    test_every_symbol_kind_of_an_oversized_run_is_named();
    test_cell_number_beyond_int64_is_never_read_as_minus_one();
    std::cout << "All position setup tests passed!" << std::endl;
    return 0;
}

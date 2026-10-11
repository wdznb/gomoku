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
    int previous = 5;
    int undos = 0;
    while (gm.undo(move_count, status) && undos < 10) {
        assert(move_count < previous);
        assert(gm.board().move_count() == move_count);
        previous = move_count;
        ++undos;
    }
    assert(undos > 0 && previous == 0);
    // Undo only removes stones; choosing a side is the sole thing that turns a
    // hand-set position into a game, so the flag survives an empty board.
    assert(gm.position_pending());
    assert(gm.replay_snapshot().moves.empty());

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

    // Undo walks the played stone and then a base stone off the board, and the
    // split follows it: two rounds take back the AI answer and one installed
    // stone, so the record of the undone session rebuilds four stones, not the
    // five that were installed.
    int move_count = -1;
    GameStatus status = GameStatus::PLAYING;
    assert(second.undo(move_count, status));
    assert(move_count == 4 && !second.is_ai_thinking());
    snap = second.replay_snapshot();
    assert(snap.move_count == 4);
    assert(snap.moves.empty() && snap.base.size() == 4);
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
    assert(third.board().move_count() == 4);
    assert(third.board().get(5, 5) == EMPTY);
    assert(third.replay_snapshot().base.size() == 4);

    std::cout << "test_snapshot_round_trip passed" << std::endl;
}

int main() {
    std::cout << "Running position setup tests..." << std::endl;
    test_grid_parsing();
    test_flat_strings();
    test_encoding_detection();
    test_bitboard_form();
    test_moves_form();
    test_validation_rules();
    test_canonical_document();
    test_shipped_templates();
    test_game_manager_position();
    test_restore_game();
    test_snapshot_round_trip();
    std::cout << "All position setup tests passed!" << std::endl;
    return 0;
}

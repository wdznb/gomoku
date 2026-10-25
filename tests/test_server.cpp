// Round-72 white-box tests over GameServer's session bookkeeping: the exported
// stamp that undo / set_position reset, the residue delete when a board empties,
// and the saved_payload stamp that may only move after a successful write.
// Round 77 adds GameManager's discard mark (K2), read directly by its test.
// All of it lives behind private handlers and private maps, so the classes are
// un-privated for this translation unit only: every header server.h pulls in is
// pre-included first, so only this project's classes are affected. No io thread
// is started - handlers are called directly and sends just queue on the
// io_context that never runs.
#include "core/board.h"
#include "core/pattern.h"
#include "core/evaluator.h"
#include "core/types.h"
#include "optimization/transposition.h"
#include "optimization/opening_book.h"
#include "search/greedy.h"
#include "search/vcf.h"
#include "search/vct.h"
#include "search/alphabeta.h"
#include "search/search_engine.h"
#include "training/game_record.h"
#include "training/tactics_card.h"
#include "server/session_store.h"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/signal_set.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <ios>
#include <iostream>
#include <istream>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <ostream>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#define private public
// game_manager.h is included inside the macro on purpose: the K2 test below
// reads GameManager's discard_pending_ directly, and its include guard means
// the class body is only rewriteable here, on first inclusion. Everything it
// pulls in is pre-included above.
#include "server/game_manager.h"
#include "server/server.h"
#include "server/param_schema.h"  // 戊-2：清单就是被测对象
#include "server/preview_arms.h"
#include "pinned_positions.h"       // 预览针臂与乙-2 那批定格盘面的对照
#undef private

using namespace gomoku;

namespace {

std::filesystem::path scratch(const char* name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() /
                ("gomoku_server_" + std::string(name) + "_" + std::to_string(stamp));
    std::filesystem::remove_all(path);
    return path;
}

long long records_lines(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string line;
    long long count = 0;
    while (std::getline(file, line)) {
        if (!line.empty()) ++count;
    }
    return count;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

// Run the handler sends that were posted onto the io_context (the one thread
// the tests never start), then read back every frame the socket was handed
// since the previous call. The write side of an unconnected socket fails, and
// the failed write leaves its frame in the queue, so frames accumulate and
// `seen` is the read cursor.
std::vector<std::string> drain_frames(GameServer& server,
                                      const std::shared_ptr<WebSocketSession>& socket,
                                      size_t& seen) {
    server.ioc_.restart();
    for (int i = 0; i < 8; ++i) {
        if (server.ioc_.poll() == 0) break;
    }
    std::queue<std::string> queue = socket->send_queue_;
    std::vector<std::string> all;
    while (!queue.empty()) {
        all.push_back(std::move(queue.front()));
        queue.pop();
    }
    std::vector<std::string> fresh(all.begin() + static_cast<std::ptrdiff_t>(seen), all.end());
    seen = all.size();
    return fresh;
}

// A finished game the manager can rebuild exactly: alternating colours, no
// repeats, and the last stone completing a five on row 7 for `winner`
// (black's line takes nine plies, white's ten).
GameRestore five_restore(int winner) {
    GameRestore restore;
    restore.player_color = BLACK;
    restore.status = GameStatus::GAME_OVER;
    restore.winner = winner;
    restore.reason = "five";
    const Position black_line[9] = {{7, 3}, {0, 0}, {7, 4}, {0, 1}, {7, 5},
                                    {0, 2}, {7, 6}, {0, 3}, {7, 7}};
    const Position white_line[10] = {{0, 0}, {7, 3}, {0, 2}, {7, 4}, {0, 4},
                                     {7, 5}, {0, 6}, {7, 6}, {0, 8}, {7, 7}};
    const int count = winner == WHITE ? 10 : 9;
    for (int i = 0; i < count; ++i) {
        const Position cell = winner == WHITE ? white_line[i] : black_line[i];
        MoveAnalysis analysis;
        analysis.move = Move(cell.row, cell.col, i % 2 == 0 ? BLACK : WHITE);
        analysis.method = "human";
        restore.moves.push_back(analysis);
    }
    return restore;
}

GameRestore playing_restore(int moves) {
    GameRestore restore;
    restore.player_color = BLACK;
    restore.status = GameStatus::PLAYING;
    const Position cells[2] = {{7, 7}, {8, 8}};
    for (int i = 0; i < moves; ++i) {
        MoveAnalysis analysis;
        analysis.move = Move(cells[i].row, cells[i].col, i % 2 == 0 ? BLACK : WHITE);
        analysis.method = "human";
        restore.moves.push_back(analysis);
    }
    return restore;
}

// The document the client's 确认 sends for a two-stone hand-set position: one
// black stone and one white, so the position is legal and BLACK is to move.
json hand_set_position_doc() {
    json matrix = json::array();
    for (int r = 0; r < BOARD_SIZE; ++r) {
        json row = json::array();
        for (int c = 0; c < BOARD_SIZE; ++c) row.push_back(0);
        matrix.push_back(std::move(row));
    }
    matrix[7][3] = 1;  // black
    matrix[7][4] = 2;  // white
    return json{{"version", 1},
                {"type", "gomoku_position"},
                {"encoding", {{"black", 1}, {"white", 2}, {"empty", 0}}},
                {"board", matrix}};
}

// The shape a 摆棋 → 执黑 → 五连 game leaves behind: two base stones on the board
// and a finished, decisive record. base is non-empty, so the game stays in its
// session file and out of records.jsonl.
GameRestore hand_set_finished() {
    GameRestore restore;
    restore.base = {Move(7, 3, BLACK), Move(7, 8, WHITE)};
    restore.player_color = BLACK;
    restore.status = GameStatus::GAME_OVER;
    restore.winner = BLACK;
    restore.reason = "five";
    const Position line[7] = {{7, 4}, {0, 1}, {7, 5}, {0, 2}, {7, 6}, {0, 3}, {7, 7}};
    for (int i = 0; i < 7; ++i) {
        MoveAnalysis analysis;
        analysis.move = Move(line[i].row, line[i].col, i % 2 == 0 ? BLACK : WHITE);
        analysis.method = "human";
        restore.moves.push_back(analysis);
    }
    return restore;
}

// One GameServer with one socket and one fresh session, wired exactly the way
// ensure_attached wires them after a handshake.
struct Harness {
    GameServer server;
    std::shared_ptr<WebSocketSession> socket;
    std::shared_ptr<GameServer::TrackedGame> entry;

    Harness(const std::filesystem::path& sessions_dir, const std::filesystem::path& records_path)
        : server(0, std::string(), std::string(), nullptr, sessions_dir.string(),
                 records_path.empty() ? std::string() : records_path.string()) {
        socket = std::make_shared<WebSocketSession>(tcp::socket(server.ioc_), std::string());
        entry = server.open_session(nullptr);
        server.attach_socket(socket, entry);
    }

    std::filesystem::path session_file() const {
        return std::filesystem::path(server.sessions_dir_) / (entry->token + ".json");
    }
};

// 轮次141 方案A 的接线：`--ponder <percent>` 只有走进每一局的会话才算数，而它是
// open_session 里的一行，所以这一条钉的是「会话拿到的是服务器那一份」，不是默认值。
void test_the_ponder_share_reaches_the_sessions_it_opens() {
    const auto dir = scratch("r141_ponder_sessions");
    {
        // 服务器没被要求预搜：每一局都是 0，也就是这棵树在没有 --ponder 时的老形状。
        GameServer off(0, std::string(), std::string(), nullptr, dir.string(), std::string());
        auto entry = off.open_session(nullptr);
        assert(entry->game->ponder_percent() == 0);
        assert(!entry->game->ponder_active());
    }
    {
        // 要求 50 %：新建的会话一出生就带着它，不需要客户端再说什么。
        GameServer on(0, std::string(), std::string(), nullptr, dir.string(), std::string(), 50);
        auto entry = on.open_session(nullptr);
        assert(entry->game->ponder_percent() == 50);
        // 越界的百分比在构造函数那一格就被折回 0..100，会话里读到的是折回后的数。
        GameServer wide(0, std::string(), std::string(), nullptr, dir.string(), std::string(), 180);
        assert(wide.open_session(nullptr)->game->ponder_percent() == 100);
    }
    std::cout << "test_the_ponder_share_reaches_the_sessions_it_opens passed" << std::endl;
}

// Round 72, handle_undo: an undo gives the token back to the undecided game, so
// the finished game that was already exported must be exportable again once the
// position finishes a second time.
void test_undo_reopens_export() {
    const auto dir = scratch("undo_sessions");
    const auto records = scratch("undo_records.jsonl");
    {
        Harness h(dir, records);
        assert(h.entry->game->restore_game(five_restore(BLACK)));
        h.server.flush_entry(h.entry);
        assert(records_lines(records) == 1);
        assert(h.entry->exported);
        const std::string first_payload = h.entry->saved_payload;

        h.server.handle_undo(h.socket);
        assert(!h.entry->exported);
        assert(h.entry->game->status() == GameStatus::PLAYING);

        // The half game lands on disk but exports nothing.
        h.server.flush_entry(h.entry);
        assert(records_lines(records) == 1);
        assert(h.entry->saved_payload != first_payload);

        // Finish again: black completes a five with a different stone, and the
        // second ending is owed its own line in records.jsonl.
        h.server.handle_make_move(h.socket, json{{"row", 7}, {"col", 2}});
        assert(h.entry->game->status() == GameStatus::GAME_OVER);
        assert(!h.entry->game->is_ai_thinking());
        h.server.flush_entry(h.entry);
        assert(records_lines(records) == 2);
        assert(h.entry->exported);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_undo_reopens_export passed" << std::endl;
}

// Round 72, handle_set_position: the position starts a fresh game, so the
// export stamp of the finished one is cleared and the next decisive game on the
// new token is exported again. An empty hand-set position is the shape that
// keeps the token exportable - base stones suspend the export by design.
//
// R85 G1: a session whose game already ended keeps its token and file, and the
// new position is handed a token of its own - so the state checks below re-read
// the socket's entry instead of the Harness's construction-time one.
void test_set_position_reopens_export() {
    const auto dir = scratch("setup_sessions");
    const auto records = scratch("setup_records.jsonl");
    {
        Harness h(dir, records);
        assert(h.entry->game->restore_game(five_restore(BLACK)));
        h.server.flush_entry(h.entry);
        assert(records_lines(records) == 1);
        assert(h.entry->exported);
        const std::string before = read_text(h.session_file());

        h.server.handle_set_position(
            h.socket, json{{"position", {{"version", 1}, {"moves", json::array()}}}});
        const std::shared_ptr<GameServer::TrackedGame> after =
            h.server.entry_locked(h.socket.get());
        assert(after);
        assert(after->game->position_pending());
        assert(!after->exported);
        assert(!after->side_chosen);
        assert(after->token != h.entry->token);

        // G1 keeps the finished game's file on purpose: the new position rides
        // on a token of its own, so nothing overwrote the old one.
        assert(std::filesystem::exists(h.session_file()));
        assert(read_text(h.session_file()) == before);

        // A zero-stone session is not kept: the new token gets no file of its own.
        h.server.flush_entry(after);
        const std::filesystem::path new_file =
            std::filesystem::path(h.server.sessions_dir_) / (after->token + ".json");
        assert(!std::filesystem::exists(new_file));

        // A second, different game finishing on the new token exports again.
        assert(after->game->restore_game(five_restore(WHITE)));
        h.server.flush_entry(after);
        assert(records_lines(records) == 2);
        assert(after->exported);
        assert(std::filesystem::exists(new_file));
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_set_position_reopens_export passed" << std::endl;
}

// Round 72, flush_entry's empty branch: undo back to an empty board must take
// the session file of the previous game with it, or a restart would resurrect
// the undone game.
void test_emptied_board_deletes_residue() {
    const auto dir = scratch("residue_sessions");
    const auto records = scratch("residue_records.jsonl");
    {
        Harness h(dir, records);
        assert(h.entry->game->restore_game(playing_restore(1)));
        h.server.flush_entry(h.entry);
        assert(std::filesystem::exists(h.session_file()));

        h.server.handle_undo(h.socket);
        assert(h.entry->game->status() == GameStatus::WAITING);
        assert(!h.entry->game->is_ai_thinking());

        // A blocked directory (a path component is a file) makes the delete
        // itself fail: the dedup stamp must not move and the next tick retries.
        const auto blocker = scratch("residue_blocker");
        { std::ofstream file(blocker); file << "not a directory"; }
        const std::string good_dir = h.server.sessions_dir_;
        const std::string stale_payload = h.entry->saved_payload;
        h.server.sessions_dir_ = (blocker / "sub").string();
        h.server.flush_entry(h.entry);
        assert(h.entry->saved_payload == stale_payload);
        assert(std::filesystem::exists(
            std::filesystem::path(good_dir) / (h.entry->token + ".json")));

        h.server.sessions_dir_ = good_dir;
        h.server.flush_entry(h.entry);
        assert(!std::filesystem::exists(h.session_file()));
        assert(records_lines(records) == 0);
        std::error_code cleanup;
        std::filesystem::remove(blocker, cleanup);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_emptied_board_deletes_residue passed" << std::endl;
}

// Round 72, flush_entry's save branch: a failed write must leave saved_payload
// where it was so the next tick retries, and an unchanged session must still be
// skipped by the content comparison.
void test_failed_flush_retries_and_keeps_dedup() {
    const auto dir = scratch("retry_sessions");
    const auto records = scratch("retry_records.jsonl");
    {
        Harness h(dir, records);
        assert(h.entry->game->restore_game(playing_restore(1)));
        h.server.flush_entry(h.entry);
        const std::string first_payload = h.entry->saved_payload;
        assert(!first_payload.empty());
        const std::string good_dir = h.server.sessions_dir_;

        // A regular file sits where the session directory should be, so the
        // failed attempt sees a write error, not a silent success.
        const auto blocker = scratch("retry_blocker");
        { std::ofstream file(blocker); file << "not a directory"; }
        h.server.sessions_dir_ = (blocker / "sub").string();

        assert(h.entry->game->restore_game(playing_restore(2)));
        h.server.flush_entry(h.entry);
        assert(h.entry->saved_payload == first_payload);

        // The readable copy still holds the first game.
        std::string error;
        SessionSnapshot on_disk;
        assert(load_session(good_dir, h.entry->token, on_disk, error));
        assert(on_disk.record.moves.size() == 1);

        // Once the directory is usable again the next tick retries and lands.
        h.server.sessions_dir_ = good_dir;
        h.server.flush_entry(h.entry);
        assert(h.entry->saved_payload != first_payload);
        assert(load_session(good_dir, h.entry->token, on_disk, error));
        assert(on_disk.record.moves.size() == 2);

        // Nothing changed: the content comparison must keep the file untouched.
        const std::string before = read_text(h.session_file());
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        h.server.flush_entry(h.entry);
        assert(read_text(h.session_file()) == before);

        std::error_code ec;
        std::filesystem::remove(blocker, ec);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_failed_flush_retries_and_keeps_dedup passed" << std::endl;
}

// Round 73, handle_message: check_position is a dry run and must not open a
// session for the socket that asks. The old code ran ensure_attached for every
// non-ping message, which answered a preview probe with a fresh session token
// and burned an engine; any other game message must still attach.
void test_check_position_skips_session_creation() {
    const auto dir = scratch("preview_sessions");
    {
        GameServer server(0, std::string(), std::string(), nullptr, dir.string(),
                          std::string());
        auto socket =
            std::make_shared<WebSocketSession>(tcp::socket(server.ioc_), std::string());
        const json preview = {{"type", "check_position"},
                              {"position", {{"version", 1}, {"moves", json::array()}}}};
        server.handle_message(socket, preview);
        for (int i = 0; i < 8; ++i) {
            if (server.ioc_.poll() == 0) break;
        }

        // No session, no game, no token binding: the probe was answered without
        // anyone being handed a new game.
        assert(server.games_.empty());
        assert(server.sessions_.empty());
        assert(server.socket_tokens_.empty());
        assert(socket->send_queue_.size() == 1);
        const json reply = json::parse(socket->send_queue_.front());
        assert(reply["type"] == "position_preview");
        assert(reply["ok"] == true);
        assert(reply.contains("board"));

        // The control: any other game message still attaches on demand, and the
        // attach announces itself with the session frame.
        server.handle_message(socket, json{{"type", "undo"}});
        // asio marks the context stopped once a poll() drains it, so the sends
        // posted by the undo path only run after an explicit restart().
        server.ioc_.restart();
        for (int i = 0; i < 8; ++i) {
            if (server.ioc_.poll() == 0) break;
        }
        assert(server.sessions_.size() == 1);
        assert(server.games_.size() == 1);
        assert(server.socket_tokens_.size() == 1);
        bool saw_session_frame = false;
        std::queue<std::string> frames = socket->send_queue_;
        while (!frames.empty()) {
            if (json::parse(frames.front())["type"] == "session") saw_session_frame = true;
            frames.pop();
        }
        assert(saw_session_frame);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_check_position_skips_session_creation passed" << std::endl;
}

// Round 73, flush_entry: a failed records.jsonl append must leave saved_payload
// where it was, or the dedup check reads the finished game as already written
// and it is lost from the training stream forever. The session file itself
// keeps working: the completed game is on disk, and the retry appends exactly
// one line without rewriting or corrupting the session file.
void test_failed_export_retries_records_append() {
    const auto dir = scratch("export_sessions");
    const auto records = scratch("export_records.jsonl");
    const auto blocker = scratch("export_blocker");
    std::error_code ec;
    {
        std::ofstream file(blocker);
        file << "not a directory";
    }
    {
        Harness h(dir, records);
        assert(h.entry->game->restore_game(five_restore(BLACK)));
        h.server.flush_entry(h.entry);
        assert(records_lines(records) == 1);
        assert(h.entry->exported);

        // A second decisive game on the same token: undo reopens the export,
        // then black finishes with a different stone.
        h.server.handle_undo(h.socket);
        assert(!h.entry->exported);
        h.server.flush_entry(h.entry);
        const std::string stamped = h.entry->saved_payload;
        assert(!stamped.empty());
        h.server.handle_make_move(h.socket, json{{"row", 7}, {"col", 2}});
        assert(h.entry->game->status() == GameStatus::GAME_OVER);

        // The records path now points through a regular file, so the append
        // fails after the session file was already written.
        h.server.records_path_ = (blocker / "records.jsonl").string();
        h.server.flush_entry(h.entry);
        assert(h.entry->saved_payload == stamped);
        assert(!h.entry->exported);
        assert(records_lines(records) == 1);

        // The failed attempt still left a loadable session file holding the
        // finished game: nothing about the session bookkeeping was corrupted.
        SessionSnapshot on_disk;
        std::string error;
        assert(load_session(dir.string(), h.entry->token, on_disk, error));
        assert(on_disk.record.moves.size() == 9);
        assert(on_disk.record.is_decisive());

        // Restored: the retry appends exactly the one owed line, and a further
        // flush must not duplicate it.
        h.server.records_path_ = records.string();
        h.server.flush_entry(h.entry);
        assert(records_lines(records) == 2);
        assert(h.entry->exported);
        assert(h.entry->saved_payload != stamped);
        h.server.flush_entry(h.entry);
        assert(records_lines(records) == 2);
        assert(load_session(dir.string(), h.entry->token, on_disk, error));
        assert(on_disk.record.moves.size() == 9);
    }
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::filesystem::remove(blocker, ec);
    std::cout << "test_failed_export_retries_records_append passed" << std::endl;
}

// Round 104 II, flush_all: the shutdown sweep appends BEFORE it lets a session
// file say the game is over. README:572 flushes the table on the exit signal and
// README:575 owes every decisive empty-board game its line in records.jsonl, so a
// failed append there may not leave a finished-looking file behind: a cold rebuild
// would read the game as decided, the exported stamp would be set for a record that
// never landed, and the next real ending would be the last chance to export it.
// flush_entry keeps its own order (file first, append second) - that tick path is
// pinned verbatim by test_failed_export_retries_records_append above.
void test_shutdown_flush_keeps_the_file_behind_a_failed_append() {
    const auto dir = scratch("shutdown_sessions");
    const auto records = scratch("shutdown_records.jsonl");
    const auto blocker = scratch("shutdown_blocker");
    std::error_code ec;
    {
        std::ofstream file(blocker);
        file << "not a directory";
    }
    {
        Harness h(dir, records);
        assert(h.entry->game->restore_game(five_restore(BLACK)));
        h.server.flush_all();
        assert(records_lines(records) == 1);
        assert(h.entry->exported);

        // Undo reopens the export; the sweep then writes the undecided game, so
        // the file on disk is the pre-ending state this case compares against.
        h.server.handle_undo(h.socket);
        assert(!h.entry->exported);
        h.server.flush_all();
        const std::string stamped = h.entry->saved_payload;
        assert(!stamped.empty());
        SessionSnapshot on_disk;
        std::string error;
        assert(load_session(dir.string(), h.entry->token, on_disk, error));
        assert(on_disk.record.moves.size() == 8);
        assert(!on_disk.record.is_decisive());

        // Black finishes again with a different stone (nine plies), and the records
        // path now points through a regular file, so the append fails.
        h.server.handle_make_move(h.socket, json{{"row", 7}, {"col", 2}});
        assert(h.entry->game->status() == GameStatus::GAME_OVER);
        h.server.records_path_ = (blocker / "records.jsonl").string();
        h.server.flush_all();

        // Nothing owed, nothing paid: no line, no stamp, no dedup commit - and the
        // session file is still the undecided one, not this finished game.
        assert(records_lines(records) == 1);
        assert(!h.entry->exported);
        assert(h.entry->saved_payload == stamped);
        assert(load_session(dir.string(), h.entry->token, on_disk, error));
        assert(on_disk.record.moves.size() < 9);
        assert(!on_disk.record.is_decisive());

        // Restored: the next sweep pays exactly the one owed line and the file
        // finally advances to the decided game.
        h.server.records_path_ = records.string();
        h.server.flush_all();
        assert(records_lines(records) == 2);
        assert(h.entry->exported);
        assert(h.entry->saved_payload != stamped);
        assert(load_session(dir.string(), h.entry->token, on_disk, error));
        assert(on_disk.record.moves.size() == 9);
        assert(on_disk.record.is_decisive());
    }
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::filesystem::remove(blocker, ec);
    std::cout << "test_shutdown_flush_keeps_the_file_behind_a_failed_append passed" << std::endl;
}

// Control for the reordered sweep: with records writable, running it twice - the
// shape of a Ctrl+C that lands after a tick already flushed - must advance the
// file once and append once. The exported flag and the dedup stamp both survive
// the call, so a finished game cannot be counted into the training stream twice.
void test_shutdown_flush_appends_once_per_ending() {
    const auto dir = scratch("twice_sessions");
    const auto records = scratch("twice_records.jsonl");
    std::error_code ec;
    {
        Harness h(dir, records);
        assert(h.entry->game->restore_game(five_restore(BLACK)));
        h.server.flush_all();
        assert(records_lines(records) == 1);
        h.server.flush_all();
        assert(records_lines(records) == 1);
        assert(h.entry->exported);

        // A second ending on the same token owes its own single line.
        h.server.handle_undo(h.socket);
        h.server.flush_all();
        assert(records_lines(records) == 1);
        h.server.handle_make_move(h.socket, json{{"row", 7}, {"col", 2}});
        h.server.flush_all();
        h.server.flush_all();
        assert(records_lines(records) == 2);
        assert(h.entry->exported);

        SessionSnapshot on_disk;
        std::string error;
        assert(load_session(dir.string(), h.entry->token, on_disk, error));
        assert(on_disk.record.moves.size() == 9);
        assert(on_disk.record.is_decisive());
    }
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_shutdown_flush_appends_once_per_ending passed" << std::endl;
}

// Round 108 LL, flush_all: the shutdown sweep must decide what counts as a game by the same
// content rule flush_entry applies to the session file. README:571 states 一子未落的空会话不写,
// and away from 摆棋 the one way to end a game with no stone anywhere is 执黑 and then 认输 on
// move zero - handle_resign (:1078) accepts `side_chosen` as "started", and choosing BLACK
// leaves BLACK to move, so no AI worker is posted and the board really does stay empty.
// Without the guard that game reached export_game_record, whose only content test is
// `base.empty() && record.is_decisive()`, so records.jsonl gained a decisive line with zero
// moves - a record that describes no game, stamped exported, in the stream the trainer reads.
void test_shutdown_flush_keeps_a_zero_stone_resign_out_of_the_records() {
    const auto dir = scratch("zero_stone_sessions");
    const auto records = scratch("zero_stone_records.jsonl");
    std::error_code ec;
    {
        Harness h(dir, records);
        h.server.handle_choose_side(h.socket, json{{"color", "black"}});
        h.server.handle_resign(h.socket);
        assert(h.entry->game->status() == GameStatus::GAME_OVER);
        assert(h.entry->game->board().move_count() == 0);
        h.server.flush_all();

        // The pose this case is about: no line in the record stream. The session file is
        // gone as well, but that half is flush_entry's own rule and holds on both trees.
        assert(records_lines(records) == 0);
        assert(!std::filesystem::exists(h.session_file()));
    }

    // Control, in a sweep of its own so it is green on both trees: the guard is about the
    // absence of stones, not about shutdown dropping real games. The same Harness shape with
    // a finished nine-ply record appends its one line, stamps the export, keeps its file.
    {
        const auto kept_dir = scratch("kept_stone_sessions");
        const auto kept_records = scratch("kept_stone_records.jsonl");
        Harness h(kept_dir, kept_records);
        assert(h.entry->game->restore_game(five_restore(BLACK)));
        h.server.flush_all();
        assert(records_lines(kept_records) == 1);
        assert(h.entry->exported);
        assert(std::filesystem::exists(h.session_file()));
        std::filesystem::remove_all(kept_dir, ec);
        std::filesystem::remove(kept_records, ec);
    }
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_shutdown_flush_keeps_a_zero_stone_resign_out_of_the_records passed"
              << std::endl;
}

// Round 104 II, the `--no-sessions` shape of the reordered sweep: with both paths
// cleared (main.cpp:100-103), flush_all now reaches export_game_record without
// passing flush_entry's first-line short-circuit. The helper's own path test is
// what keeps that harmless - a decided game in the table at Ctrl+C must not create
// a record file, a session file or anything else on disk.
void test_shutdown_flush_with_persistence_off_writes_nothing() {
    const auto root = scratch("off_root");
    const auto records = root / "records.jsonl";
    std::error_code ec;
    {
        GameServer server(0, std::string(), std::string(), nullptr, std::string(), std::string());
        auto socket = std::make_shared<WebSocketSession>(tcp::socket(server.ioc_), std::string());
        const auto entry = server.open_session(nullptr);
        server.attach_socket(socket, entry);
        assert(entry->game->restore_game(five_restore(BLACK)));
        assert(server.snapshot_of(*entry).record.is_decisive());

        server.flush_all();
        assert(!std::filesystem::exists(records));
        assert(!std::filesystem::exists(root));
        assert(entry->saved_payload.empty());
        assert(entry->game->status() == GameStatus::GAME_OVER);
    }
    std::filesystem::remove_all(root, ec);
    std::cout << "test_shutdown_flush_with_persistence_off_writes_nothing passed" << std::endl;
}

// R77 K2: halt_search (reset / set_position / restore / resign / the destructor)
// raises discard_pending_ so an in-flight worker drops its best move, and only
// start_ai_thinking used to lower it again. A hint asked for right after any of
// those paths was accepted and really searched, but the worker dropped the
// answer at the discard gate - the client's 求招 waited forever. request_hint
// is a fresh delivery and must clear the mark before it starts.
void test_request_hint_clears_stale_discard() {
    Board setup;
    setup.place_stone(7, 7, BLACK);
    setup.place_stone(6, 6, WHITE);

    auto probe = [](const char* path, GameManager& game) {
        assert(game.discard_pending_.load());
        assert(game.request_hint(300, 1));
        assert(!game.discard_pending_.load());
        game.halt_search();
        std::cout << "  request_hint clears the discard mark after " << path << std::endl;
    };

    GameManager after_reset;
    after_reset.reset();
    probe("reset", after_reset);

    GameManager after_position;
    assert(after_position.set_position(setup));
    probe("set_position", after_position);

    // Round 97 BB2 replaced this posture: it used to resign and then ask for a
    // hint, but request_hint now refuses a finished game by design, so "the mark
    // is raised and a hint is still accepted" cannot hold there. restore_game is
    // the fourth path through halt_search (:102, the same one set_position uses)
    // and leaves the game playing, which is what this probe needs.
    GameManager after_restore;
    assert(after_restore.restore_game(playing_restore(2)));
    assert(after_restore.status() == GameStatus::PLAYING);
    probe("restore", after_restore);

    // Control: the other clearing door, start_ai_thinking, behaves the same.
    GameManager after_arm;
    after_arm.reset();
    assert(after_arm.discard_pending_.load());
    after_arm.start_ai_thinking(300, 1);
    assert(!after_arm.discard_pending_.load());
    after_arm.halt_search();

    std::cout << "test_request_hint_clears_stale_discard passed" << std::endl;
}

// R78 N1, read_position_payload: `position` accepts the canonical document or
// the board as text, and a board matrix serialised as a string at the sender is
// an array. The old code copied any parsed JSON into doc and then merged the
// top-level `encoding` into it - doc["encoding"] on an array throws
// type_error.305, the throw escaped handle_check_position / handle_set_position
// and the read loop swallowed it: the socket went silent, no preview and no
// error. The array is now wrapped as {"board": [...]} before the merge.
void test_position_array_string_payload_answers() {
    json matrix = json::array();
    for (int r = 0; r < BOARD_SIZE; ++r) {
        json row = json::array();
        for (int c = 0; c < BOARD_SIZE; ++c) row.push_back(0);
        matrix.push_back(std::move(row));
    }
    matrix[7][3] = 1;  // black
    matrix[7][4] = 2;  // white
    const std::string payload = matrix.dump();
    const json encoding = {{"black", 1}, {"white", 2}, {"empty", 0}};

    const auto dir = scratch("n1_sessions");
    {
        Harness h(dir, std::string());
        size_t seen = 0;

        // (a) The array-as-string with the top-level encoding the UI states:
        // exactly one position_preview frame, not silence.
        h.server.handle_check_position(
            h.socket, json{{"position", payload}, {"encoding", encoding}});
        std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        const json preview = json::parse(frames[0]);
        assert(preview["type"] == "position_preview");
        assert(preview["ok"] == true);
        assert(preview["black_count"] == 1);
        assert(preview["white_count"] == 1);
        assert(preview["side_to_move"] == "black");
        assert(preview["encoding"]["black"] == "1");
        assert(preview["encoding"]["white"] == "2");
        assert(preview["encoding"]["empty"] == "0");
        assert(preview["board"][7][3] == 1);
        assert(preview["board"][7][4] == 2);

        // (b) Same payload without the encoding: the wrapped board still reads
        // (tokens detected as the 0/1/2 alphabet), not "局面文档必须是 JSON 对象".
        h.server.handle_check_position(h.socket, json{{"position", payload}});
        frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        const json detected = json::parse(frames[0]);
        assert(detected["type"] == "position_preview");
        assert(detected["ok"] == true);
        assert(detected["black_count"] == 1);
        assert(detected["side_to_move"] == "black");

        // (c) Control: the same board as a document string is untouched.
        json doc;
        doc["board"] = matrix;
        doc["encoding"] = encoding;
        h.server.handle_check_position(h.socket, json{{"position", doc.dump()}});
        frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        const json from_doc = json::parse(frames[0]);
        assert(from_doc["type"] == "position_preview");
        assert(from_doc["ok"] == true);
        assert(from_doc["black_count"] == 1);
        assert(from_doc["white_count"] == 1);

        // (d) Control: the text grid path is untouched.
        std::string grid;
        for (int r = 0; r < BOARD_SIZE; ++r) {
            for (int c = 0; c < BOARD_SIZE; ++c) {
                grid += (r == 7 && c == 3) ? "1" : (r == 7 && c == 4) ? "2" : "0";
                grid += (c == BOARD_SIZE - 1) ? '\n' : ' ';
            }
        }
        h.server.handle_check_position(h.socket, json{{"position", grid}});
        frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        const json from_grid = json::parse(frames[0]);
        assert(from_grid["type"] == "position_preview");
        assert(from_grid["ok"] == true);
        assert(from_grid["black_count"] == 1);
        assert(from_grid["white_count"] == 1);

        // (e) set_position on the same payload: the position is installed, one
        // position_set frame, and the session is in the pending state.
        h.server.handle_set_position(
            h.socket, json{{"position", payload}, {"encoding", encoding}});
        frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        const json installed = json::parse(frames[0]);
        assert(installed["type"] == "position_set");
        assert(installed["position_pending"] == true);
        assert(installed["current_turn"] == "black");
        assert(installed["black_count"] == 1);
        assert(installed["white_count"] == 1);
        assert(h.entry->game->position_pending());
        assert(h.entry->game->board().move_count() == 2);
        assert(h.entry->game->board().get(7, 3) == BLACK);
        assert(h.entry->game->board().get(7, 4) == WHITE);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_position_array_string_payload_answers passed" << std::endl;
}

// R78 N2, make_move's pending-position claim + handle_make_move's colour read:
// README lets 落子 start a hand-set game instead of choose_side ("玩家随后选择
// 执子颜色或直接落子"). The old make_move compared the turn against the default
// player_color_ (BLACK) before any claim, so a white-to-move position could
// never be started by playing; and handle_make_move read player_color() before
// the move, which would report the stale colour once the claim moved into
// make_move. This drives the whole thing through the socket handlers.
void test_pending_position_direct_move_claims_side() {
    const auto dir = scratch("n2_sessions");
    {
        // (A) White to move (3 black / 2 white), no choose_side: the white
        // stone claims WHITE for the player and hands BLACK to the AI.
        Harness h(dir, std::string());
        size_t seen = 0;
        h.server.handle_set_position(
            h.socket, json{{"position", {{"version", 1},
                                         {"moves", {{0, 0}, {14, 0}, {0, 2}, {14, 2}, {0, 4}}}}}});
        std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        assert(json::parse(frames[0])["type"] == "position_set");
        assert(h.entry->game->position_pending());
        assert(h.entry->game->status() == GameStatus::WAITING);
        assert(h.entry->game->current_turn() == WHITE);
        assert(h.entry->game->board().move_count() == 5);

        // (D) undo is still refused while a hand-set position is pending.
        h.server.handle_undo(h.socket);
        frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        assert(json::parse(frames[0])["type"] == "error");
        assert(h.entry->game->position_pending());
        assert(h.entry->game->board().move_count() == 5);

        // A long budget so the armed black worker cannot finish and commit
        // during the assertions; it is halted before the board is read again.
        h.entry->game->set_config(120000, 20);
        h.server.handle_make_move(h.socket, json{{"row", 7}, {"col", 7}});
        const bool armed = h.entry->game->is_ai_thinking();
        h.entry->game->halt_search();

        frames = drain_frames(h.server, h.socket, seen);
        int move_frames = 0;
        for (const std::string& frame : frames) {
            const json parsed = json::parse(frame);
            if (parsed["type"] == "move_made") {
                ++move_frames;
                assert(parsed["row"] == 7);
                assert(parsed["col"] == 7);
                assert(parsed["color"] == "white");
            }
            assert(parsed["type"] != "error");
        }
        assert(move_frames == 1);

        assert(h.entry->game->player_color() == WHITE);
        assert(h.entry->game->ai_color() == BLACK);
        assert(!h.entry->game->position_pending());
        assert(h.entry->game->status() == GameStatus::PLAYING);
        assert(h.entry->side_chosen);
        assert(h.entry->game->board().move_count() == 6);
        // Black is to move and was the side the AI got.
        assert(h.entry->game->current_turn() == BLACK);
        assert(armed);
        // The stopped worker committed nothing: the claim stone is the 6th.
        assert(h.entry->game->board().get(7, 7) == WHITE);
    }
    {
        // (B) Reverse control: choose_side first clears the pending flag, and a
        // white point in a session whose player is BLACK is still refused by
        // the colour guard.
        Harness h(dir, std::string());
        size_t seen = 0;
        h.server.handle_set_position(
            h.socket, json{{"position", {{"version", 1},
                                         {"moves", {{0, 0}, {14, 0}, {0, 2}, {14, 2}, {0, 4}}}}}});
        h.entry->game->set_config(120000, 20);
        h.server.handle_choose_side(h.socket, json{{"color", "black"}});
        assert(h.entry->game->is_ai_thinking());
        h.entry->game->halt_search();
        assert(!h.entry->game->position_pending());
        assert(h.entry->game->player_color() == BLACK);
        assert(h.entry->game->ai_color() == WHITE);
        assert(h.entry->game->board().move_count() == 5);
        drain_frames(h.server, h.socket, seen);

        h.server.handle_make_move(h.socket, json{{"row", 7}, {"col", 7}});
        const std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        const json refused = json::parse(frames[0]);
        assert(refused["type"] == "error");
        assert(refused["message"] == "Invalid move");
        assert(h.entry->game->board().move_count() == 5);
        assert(h.entry->game->board().is_empty(7, 7));
    }
    {
        // (C) Regression: a black-to-move pending position started by playing
        // black behaved this way before the fix too (the default player colour
        // was already BLACK); the claim must not change any of it.
        Harness h(dir, std::string());
        size_t seen = 0;
        h.server.handle_set_position(
            h.socket, json{{"position", {{"version", 1},
                                         {"moves", {{0, 0}, {14, 0}, {0, 2}, {14, 2}}}}}});
        drain_frames(h.server, h.socket, seen);
        assert(h.entry->game->position_pending());
        assert(h.entry->game->current_turn() == BLACK);
        h.entry->game->set_config(120000, 20);

        h.server.handle_make_move(h.socket, json{{"row", 7}, {"col", 7}});
        const bool armed = h.entry->game->is_ai_thinking();
        h.entry->game->halt_search();

        const std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
        int move_frames = 0;
        for (const std::string& frame : frames) {
            const json parsed = json::parse(frame);
            if (parsed["type"] == "move_made") ++move_frames;
            assert(parsed["type"] != "error");
        }
        assert(move_frames == 1);
        assert(h.entry->game->player_color() == BLACK);
        assert(h.entry->game->ai_color() == WHITE);
        assert(!h.entry->game->position_pending());
        assert(h.entry->game->status() == GameStatus::PLAYING);
        assert(h.entry->game->board().move_count() == 5);
        assert(h.entry->game->current_turn() == WHITE);
        assert(armed);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_pending_position_direct_move_claims_side passed" << std::endl;
}

// R78 N3, ai_thinking_worker's resigned branch: the old gate was
// !discard_pending_ && !stop_flag_ && PLAYING, so a user 终止 (stop_thinking only
// raises stop_flag_) turned the engine's proven loss - search() returning
// resigned=true because every remaining cell is forbidden for BLACK - into
// silence: the game stayed PLAYING with ai_color_ to move and no frame coming,
// and every later make_move was rejected by the colour guard. The stop term is
// gone; only the discard paths (reset / set_position / resign / destructor)
// may suppress the ending. The board below makes that loss unavoidable and
// stop-independent: (7,7) is the last empty cell and a black stone there is a
// six-in-a-row (overline), so the black candidate filter empties root_moves
// before any stop check and search() resigns by itself.
void test_resign_survives_interruption() {
    Board full;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            if (r == 7 && c == 7) continue;
            const bool line = (r == 7) || (c == 7) || (r - c == 0) || (r + c == 14);
            int color = line ? WHITE : ((r + c) % 2 == 0 ? BLACK : WHITE);
            if (r == 7 && c >= 2 && c <= 6) color = BLACK;
            assert(full.place_stone(r, c, color));
        }
    }
    assert(full.move_count() == MAX_MOVES - 1);
    assert(full.is_empty(7, 7));
    assert(PatternRecognizer::is_forbidden(full, 7, 7, BLACK));

    int frames = 0;
    int frame_winner = EMPTY;
    std::string frame_reason;
    {
        GameManager game;
        game.set_player_color(WHITE);  // the AI plays BLACK
        assert(game.ai_color() == BLACK);
        assert(game.set_position(full));
        // The state a game armed by start_ai_thinking has: playing, not
        // pending, no discard mark.
        game.status_ = GameStatus::PLAYING;
        game.position_pending_ = false;
        game.discard_pending_.store(false);
        game.set_game_over_callback([&](int winner, const std::string& reason) {
            ++frames;
            frame_winner = winner;
            frame_reason = reason;
        });

        // 终止 arrived: stop_flag_ is up when the worker decides, and the
        // worker is driven directly so the window is deterministic. A4 put the
        // session's reuse record in the worker's arguments and A7 the budget with
        // it; this game has neither, so both stay at the engine's own defaults.
        game.stop_flag_.store(true);
        std::thread worker(&GameManager::ai_thinking_worker, &game, 2000, 2,
                           GameManager::Reuse{}, GameManager::Budget{});
        worker.join();

        assert(game.status() == GameStatus::GAME_OVER);
        assert(game.winner() == WHITE);
        assert(frames == 1);
        assert(frame_winner == WHITE);
        assert(frame_reason == "resign");
        // The game is over, so the human's next move is refused by the
        // game-over status rather than by the colour guard in a game that
        // still looks playable.
        assert(!game.make_move(7, 7));
    }

    // Control: the discard path (reset / set_position / resign / destructor)
    // must stay silent - a game the user threw away does not end itself.
    {
        GameManager game;
        game.set_player_color(WHITE);
        assert(game.set_position(full));
        game.status_ = GameStatus::PLAYING;
        game.position_pending_ = false;
        game.discard_pending_.store(true);
        game.set_game_over_callback([&](int, const std::string&) { ++frames; });
        game.stop_flag_.store(true);
        std::thread worker(&GameManager::ai_thinking_worker, &game, 2000, 2,
                           GameManager::Reuse{}, GameManager::Budget{});
        worker.join();
        assert(game.status() == GameStatus::PLAYING);
        assert(frames == 1);  // unchanged: no second callback
    }
    std::cout << "test_resign_survives_interruption passed" << std::endl;
}

// Round 79, A6 (contract updated in R85; disk facts only since R96 AA4): a session
// file whose record cannot be rebuilt is handed a NEW token (README:476-477), so
// what has to survive is the old file on disk - the entry no longer owns it. The
// unreadable_record flag flush_entry used to consult is gone: the fresh game takes
// the ordinary empty-board branch, and deleting a file its own token has never
// written is a success, not a case to guard.
void test_unreadable_record_file_survives() {
    const auto dir = scratch("unreadable_sessions");
    const auto records = scratch("unreadable_records.jsonl");
    std::filesystem::create_directories(dir);

    // A valid envelope (so load_saved_sessions admits the token) whose record
    // repeats a cell, which restore_game's replay rejects.
    const std::string damaged =
        R"({"version":1,"type":"gomoku_session","token":"deadbeef","status":"playing",)"
        R"("record":{"moves":[{"row":7,"col":7,"color":"black","method":"human"},)"
        R"({"row":7,"col":7,"color":"white","method":"human"}],)"
        R"("winner":"draw","reason":"invalid"}})";
    { std::ofstream file(dir / "deadbeef.json"); file << damaged; }

    {
        Harness h(dir, records);
        h.server.load_saved_sessions();  // start() does this; handlers are driven directly here
        h.server.handle_resume(h.socket, json{{"token", "deadbeef"}});

        const std::shared_ptr<GameServer::TrackedGame> resumed =
            h.server.entry_locked(h.socket.get());
        assert(resumed);
        assert(resumed->token != "deadbeef");
        assert(!resumed->rebuilt);
        // The caller gets an empty game, not a half-replayed one.
        assert(resumed->game->replay_snapshot().move_count == 0);

        const std::filesystem::path own_file =
            std::filesystem::path(h.server.sessions_dir_) / (resumed->token + ".json");
        // The new token has written nothing, and the unreadable record still sits
        // under the old token, byte for byte.
        assert(!std::filesystem::exists(own_file));
        assert(read_text(dir / "deadbeef.json") == damaged);

        // Only a side is chosen - no stone is placed, so the board stays empty and
        // the flush takes the empty-board branch on THIS token's own (absent) file.
        h.server.handle_choose_side(h.socket, json{{"color", "black"}});
        assert(resumed->side_chosen);
        h.server.flush_entry(resumed);
        assert(!std::filesystem::exists(own_file));
        assert(read_text(dir / "deadbeef.json") == damaged);

        // AA4's new consequence: content is what puts the file on disk, and undoing
        // back to an empty board has to take it away again - a restart must not
        // resurrect a wiped game. Both writes stay off the damaged record.
        assert(resumed->game->restore_game(playing_restore(1)));
        h.server.flush_entry(resumed);
        assert(std::filesystem::exists(own_file));
        assert(read_text(dir / "deadbeef.json") == damaged);

        h.server.handle_undo(h.socket);
        assert(resumed->game->status() == GameStatus::WAITING);
        h.server.flush_entry(resumed);
        assert(!std::filesystem::exists(own_file));
        assert(read_text(dir / "deadbeef.json") == damaged);

        h.server.detach_socket(h.socket.get());  // flushes the session on the way out
        assert(read_text(dir / "deadbeef.json") == damaged);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_unreadable_record_file_survives passed" << std::endl;
}

// R96 AA5: on_flush_tick may not shelve a game whose file it could not write.
// flush_entry now answers whether it settled, and only 过期且已落盘 leaves the table
// for cold_sessions_: shelving a stale file means the token resumes from it and the
// plies that failed to land are gone for good. A path component that is a regular
// file makes save_session fail for real, and the same entry is settled once the
// directory is usable again.
void test_failed_flush_keeps_the_overdue_entry_hot() {
    const auto dir = scratch("aa5_sessions");
    const auto records = scratch("aa5_records.jsonl");
    const auto wall_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    };
    {
        Harness h(dir, records);
        assert(h.entry->game->restore_game(playing_restore(1)));
        h.server.detach_socket(h.socket.get());  // flushes: the one-stone game lands
        const std::string token = h.entry->token;
        assert(std::filesystem::exists(dir / (token + ".json")));

        // Past the 30 minute reattach window, with a second ply still owed a write.
        assert(h.entry->game->restore_game(playing_restore(2)));
        h.entry->detached_ms = wall_ms() - 30LL * 60LL * 1000LL - 10;

        const auto blocker = scratch("aa5_blocker");
        { std::ofstream file(blocker); file << "not a directory"; }
        const std::string good_dir = h.server.sessions_dir_;
        h.server.sessions_dir_ = (blocker / "sub").string();

        h.server.on_flush_tick();

        // The write failed, so the entry still owes a retry: it stays hot, and the
        // cold shelf - which a resume would read - stays empty for it.
        assert(h.server.sessions_.count(token) == 1);
        assert(h.server.cold_sessions_.count(token) == 0);
        assert(h.server.sessions_.at(token)->game != nullptr);

        // The readable copy is still the one-stone game: nothing half-written.
        std::string error;
        SessionSnapshot on_disk;
        h.server.sessions_dir_ = good_dir;
        assert(load_session(good_dir, token, on_disk, error));
        assert(on_disk.record.moves.size() == 1);

        // Writable again: the next tick settles the file and evicts as usual.
        h.entry->detached_ms = wall_ms() - 30LL * 60LL * 1000LL - 10;
        h.server.on_flush_tick();
        assert(h.server.sessions_.count(token) == 0);
        assert(h.server.cold_sessions_.count(token) == 1);
        assert(load_session(good_dir, token, on_disk, error));
        assert(on_disk.record.moves.size() == 2);

        std::error_code cleanup;
        std::filesystem::remove(blocker, cleanup);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_failed_flush_keeps_the_overdue_entry_hot passed" << std::endl;
}

// Round 112 PP2: an overdue entry used to go onto cold_sessions_ whatever it held, and
// a snapshot of a game with no stone anywhere was shelved too - flush_entry's
// empty-content branch had already deleted its file, so the shelf kept the only copy of
// a token that describes nothing. A resume then rebuilt from that snapshot under the
// SAME token and answered resumed:false, while README:487-488 says a game the server
// cannot come back to opens a new one: resumed:false plus a NEW token. Dropping the
// contentless entry instead leaves the token unknown both on disk and in memory, so the
// resume falls through to open_session(nullptr) like any unknown token does.
void test_a_shelved_contentless_session_does_not_keep_its_token() {
    const auto dir = scratch("pp2_sessions");
    const auto records = scratch("pp2_records.jsonl");
    const auto wall_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    };
    {
        Harness h(dir, records);
        assert(h.entry->game->restore_game(playing_restore(1)));  // one stone, WHITE to move
        h.server.flush_entry(h.entry);
        const std::string token = h.entry->token;
        const auto file = dir / (token + ".json");
        // The game really was on disk before the undo took it back.
        assert(std::filesystem::exists(file));

        h.server.handle_undo(h.socket);
        assert(h.entry->game->replay_snapshot().move_count == 0);
        assert(h.server.flush_entry(h.entry));  // empty branch: delete the residue
        assert(!std::filesystem::exists(file));

        // Past the reattach window without sleeping 30 minutes: detach is what stamps
        // detached_ms, and the tick's own clock decides it is overdue.
        h.server.detach_socket(h.socket.get());
        h.entry->detached_ms = wall_ms() - 30LL * 60LL * 1000LL - 10;
        h.server.on_flush_tick();

        // Nothing contentless was shelved, and the table no longer holds it.
        assert(h.server.cold_sessions_.count(token) == 0);
        assert(h.server.sessions_.count(token) == 0);

        auto second = std::make_shared<WebSocketSession>(tcp::socket(h.server.ioc_), std::string());
        size_t seen = 0;
        h.server.handle_resume(second, json{{"token", token}});
        json frame;
        for (const std::string& text : drain_frames(h.server, second, seen)) {
            const json parsed = json::parse(text, nullptr, false);
            if (!parsed.is_discarded() && parsed.value("type", "") == "session") frame = parsed;
        }
        assert(frame.is_object());
        assert(frame["resumed"] == false);
        // Discriminator: the dead token is not handed back - the answer names a new one.
        assert(frame["token"] != token);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_a_shelved_contentless_session_does_not_keep_its_token passed" << std::endl;
}

// Round 112 PP2 control: the same tick still shelves a game that has stones on it, and
// its token still comes back - README:565-566, past 30 minutes the resume rebuilds from
// disk and the token stays valid. Without this arm the guard above would be indistinguishable
// from "cold resume is broken".
void test_a_shelved_played_session_keeps_its_token() {
    const auto dir = scratch("pp2c_sessions");
    const auto records = scratch("pp2c_records.jsonl");
    const auto wall_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    };
    {
        Harness h(dir, records);
        assert(h.entry->game->restore_game(playing_restore(2)));
        h.server.detach_socket(h.socket.get());  // flushes: the two-stone game lands
        const std::string token = h.entry->token;
        assert(std::filesystem::exists(dir / (token + ".json")));

        h.entry->detached_ms = wall_ms() - 30LL * 60LL * 1000LL - 10;
        h.server.on_flush_tick();
        assert(h.server.cold_sessions_.count(token) == 1);
        assert(h.server.sessions_.count(token) == 0);

        auto second = std::make_shared<WebSocketSession>(tcp::socket(h.server.ioc_), std::string());
        size_t seen = 0;
        h.server.handle_resume(second, json{{"token", token}});
        json frame;
        for (const std::string& text : drain_frames(h.server, second, seen)) {
            const json parsed = json::parse(text, nullptr, false);
            if (!parsed.is_discarded() && parsed.value("type", "") == "session") frame = parsed;
        }
        assert(frame.is_object());
        assert(frame["token"] == token);
        assert(frame["resumed"] == true);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_a_shelved_played_session_keeps_its_token passed" << std::endl;
}

// Round 81, C1: handle_resign's "started" guard must count played plies, not
// stones on the board. A hand-set position installs its base stones as the board
// itself (game_manager.cpp:90), so replay_snapshot().move_count is non-zero while
// analyses_ is still empty - and undo already refuses to give those stones back
// for the same reason. Settling such a position handed the opponent 白棋胜（认输）
// over zero moves on the default player_color_(BLACK).
void test_resign_needs_a_played_ply_not_base_stones() {
    const auto dir = scratch("c1_sessions");
    const auto records = scratch("c1_records.jsonl");

    json matrix = json::array();
    for (int r = 0; r < BOARD_SIZE; ++r) {
        json row = json::array();
        for (int c = 0; c < BOARD_SIZE; ++c) row.push_back(0);
        matrix.push_back(std::move(row));
    }
    matrix[7][3] = 1;  // black
    matrix[7][4] = 2;  // white
    const json encoding = {{"black", 1}, {"white", 2}, {"empty", 0}};
    // The document form the client's 确认 sends: a bare matrix as a JSON value is
    // refused outright by read_position_payload (only a *string* may be a bare
    // grid), so nothing would be installed and the guard could not be tested.
    const json position = {{"version", 1},
                           {"type", "gomoku_position"},
                           {"encoding", encoding},
                           {"board", matrix}};

    {
        // (1) 摆子 only, no side chosen: the position is pending, two stones sit
        // on the board, and 认输 has to be refused without settling anything.
        Harness h(dir, records);
        size_t seen = 0;
        h.server.handle_set_position(h.socket, json{{"position", position}});
        drain_frames(h.server, h.socket, seen);
        assert(h.entry->game->position_pending());
        assert(!h.entry->side_chosen);
        assert(h.entry->game->board().move_count() == 2);
        assert(h.entry->game->replay_snapshot().move_count == 2);
        assert(h.entry->game->replay_snapshot().moves.empty());
        assert(h.entry->game->status() == GameStatus::WAITING);

        h.server.handle_resign(h.socket);
        const std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        const json refused = json::parse(frames[0]);
        assert(refused["type"] == "error");
        assert(refused["message"] == "Game not started");
        assert(h.entry->game->status() == GameStatus::WAITING);
        assert(h.entry->game->winner() == EMPTY);
        assert(h.entry->game->game_over_reason_.empty());
        assert(h.entry->game->board().move_count() == 2);
        h.server.flush_entry(h.entry);
        assert(records_lines(records) == 0);
    }
    {
        // (2) 摆子 then choose a side and play: a real ply is on the board, so
        // 认输 settles the game exactly as it did before.
        Harness h(dir, records);
        size_t seen = 0;
        h.server.handle_set_position(h.socket, json{{"position", position}});
        drain_frames(h.server, h.socket, seen);
        assert(h.entry->game->position_pending());
        h.entry->game->set_config(120000, 20);
        h.server.handle_choose_side(h.socket, json{{"color", "black"}});
        assert(h.entry->side_chosen);
        assert(h.entry->game->player_color() == BLACK);
        h.entry->game->halt_search();
        drain_frames(h.server, h.socket, seen);

        h.server.handle_make_move(h.socket, json{{"row", 9}, {"col", 9}});
        h.entry->game->halt_search();
        std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
        int move_frames = 0;
        for (const std::string& frame : frames) {
            const json parsed = json::parse(frame);
            assert(parsed["type"] != "error");
            if (parsed["type"] == "move_made") ++move_frames;
        }
        assert(move_frames == 1);
        assert(!h.entry->game->replay_snapshot().moves.empty());

        h.server.handle_resign(h.socket);
        assert(h.entry->game->status() == GameStatus::GAME_OVER);
        assert(h.entry->game->winner() == WHITE);
        assert(h.entry->game->game_over_reason_ == "resign");
        h.server.handle_resign(h.socket);  // a finished game resigns nothing further
        assert(h.entry->game->winner() == WHITE);
        frames = drain_frames(h.server, h.socket, seen);
        for (const std::string& frame : frames) {
            assert(json::parse(frame)["type"] != "error");
        }
    }
    {
        // (3) Control, ordinary game: the first stone from an empty board starts
        // it, and 认输 behaves verbatim as before the guard change.
        Harness h(dir, records);
        size_t seen = 0;
        h.entry->game->set_config(120000, 20);
        h.server.handle_make_move(h.socket, json{{"row", 7}, {"col", 7}});
        h.entry->game->halt_search();
        drain_frames(h.server, h.socket, seen);
        assert(h.entry->game->board().move_count() == 1);
        assert(h.entry->game->replay_snapshot().moves.size() == 1);

        h.server.handle_resign(h.socket);
        assert(h.entry->game->status() == GameStatus::GAME_OVER);
        assert(h.entry->game->winner() == WHITE);
        assert(h.entry->game->game_over_reason_ == "resign");
        h.server.flush_entry(h.entry);
        // 认输 from an empty start is a decisive game, so it still reaches the
        // training stream.
        assert(records_lines(records) == 1);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_resign_needs_a_played_ply_not_base_stones passed" << std::endl;
}

// R85 G1: 摆棋 replaces the game a session is playing, but a game that already
// ended is finished business: its file must survive it and the new position must
// start on a token of its own, which the client is told. HEAD kept the entry - and
// its token - so the next flush wrote the new position over the finished game's
// file, destroying the only copy of a game that base stones keep out of
// records.jsonl.
void test_set_position_rotates_token_after_finished_game() {
    const auto dir = scratch("rotate_sessions");
    const auto records = scratch("rotate_records.jsonl");
    {
        Harness h(dir, records);
        assert(h.entry->game->restore_game(hand_set_finished()));
        h.server.flush_entry(h.entry);

        const std::string old_token = h.entry->token;
        const std::filesystem::path old_file =
            std::filesystem::path(h.server.sessions_dir_) / (old_token + ".json");
        assert(std::filesystem::exists(old_file));
        const std::string before = read_text(old_file);
        SessionSnapshot old_snap;
        std::string error;
        assert(load_session(h.server.sessions_dir_, old_token, old_snap, error));
        assert(!old_snap.base.empty());
        const size_t old_moves = old_snap.record.moves.size();
        assert(old_moves == hand_set_finished().moves.size());

        size_t seen = 0;
        h.server.handle_set_position(h.socket, json{{"position", hand_set_position_doc()}});
        const std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);

        std::string told_token;
        for (const std::string& frame : frames) {
            const json parsed = json::parse(frame);
            if (parsed["type"] == "session") {
                told_token = parsed["token"].get<std::string>();
            }
        }
        assert(!told_token.empty());     // the page must learn where the new game is
        assert(told_token != old_token); // and it is not the finished game's token

        const std::shared_ptr<GameServer::TrackedGame> after =
            h.server.entry_locked(h.socket.get());
        assert(after && after->token == told_token);
        h.server.flush_entry(after);

        // The finished game's file is intact, stone for stone.
        assert(std::filesystem::exists(old_file));
        assert(read_text(old_file) == before);
        SessionSnapshot reread;
        assert(load_session(h.server.sessions_dir_, old_token, reread, error));
        assert(!reread.base.empty());
        assert(reread.record.moves.size() == old_moves);

        // And the new position landed on the new token, with its own base stones.
        SessionSnapshot fresh_snap;
        assert(load_session(h.server.sessions_dir_, told_token, fresh_snap, error));
        assert(fresh_snap.base.size() == 2);
        assert(fresh_snap.record.moves.empty());
        assert(records_lines(records) == 0);  // a base game never exports
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_set_position_rotates_token_after_finished_game passed" << std::endl;
}

// R85 G4: the timestamps in a session file say when that file's game began and was
// last touched. Replacing the game with a hand-set position must re-stamp
// created_ms - keeping the replaced game's birth time would date the new position
// from a game that is over.
void test_set_position_restamps_session_created_ms() {
    const auto dir = scratch("created_sessions");
    const auto records = scratch("created_records.jsonl");
    {
        Harness h(dir, records);
        assert(h.entry->game->restore_game(hand_set_finished()));
        h.server.flush_entry(h.entry);

        SessionSnapshot old_snap;
        std::string error;
        assert(load_session(h.server.sessions_dir_, h.entry->token, old_snap, error));
        assert(old_snap.created_ms > 0);

        const long long placed_ms = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());

        h.server.handle_set_position(h.socket, json{{"position", hand_set_position_doc()}});
        const std::shared_ptr<GameServer::TrackedGame> after =
            h.server.entry_locked(h.socket.get());
        assert(after);
        h.server.flush_entry(after);

        SessionSnapshot fresh_snap;
        assert(load_session(h.server.sessions_dir_, after->token, fresh_snap, error));
        assert(fresh_snap.created_ms != old_snap.created_ms);
        assert(fresh_snap.created_ms >= old_snap.created_ms);
        // Within the minute the position was placed in, not last week's game.
        assert(fresh_snap.created_ms <= placed_ms + 60000);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_set_position_restamps_session_created_ms passed" << std::endl;
}

// R85 G2: a token whose file cannot be rebuilt is not a game to come back to. The
// resume must answer resumed:false AND hand the client a different token
// (README:476-477), leaving the unreadable file on disk untouched - HEAD answered
// with the same token, so the next flush overwrote the file with the fresh game.
void test_unreadable_resume_gets_a_fresh_token() {
    const auto dir = scratch("freshtoken_sessions");
    const auto records = scratch("freshtoken_records.jsonl");
    std::filesystem::create_directories(dir);

    // Schema-valid envelope whose record cannot be replayed: cell (7,7) twice.
    const std::string damaged =
        R"({"version":1,"type":"gomoku_session","token":"deadbeef","status":"playing",)"
        R"("record":{"moves":[{"row":7,"col":7,"color":"black","method":"human"},)"
        R"({"row":7,"col":7,"color":"white","method":"human"}],)"
        R"("winner":"draw","reason":"invalid"}})";
    { std::ofstream file(dir / "deadbeef.json"); file << damaged; }

    {
        Harness h(dir, records);
        h.server.load_saved_sessions();

        size_t seen = 0;
        h.server.handle_resume(h.socket, json{{"token", "deadbeef"}});
        const std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);

        bool saw_unresumed = false;
        std::string told_token;
        for (const std::string& frame : frames) {
            const json parsed = json::parse(frame);
            if (parsed["type"] != "session") continue;
            assert(parsed["resumed"] == false);  // it could not be rebuilt
            saw_unresumed = true;
            told_token = parsed["token"].get<std::string>();
        }
        assert(saw_unresumed);
        assert(told_token != "deadbeef");  // a token that names nothing usable
        assert(read_text(dir / "deadbeef.json") == damaged);

        // Playing a stone on the fresh game must not write over the file either.
        h.server.handle_make_move(h.socket, json{{"row", 9}, {"col", 9}});
        const std::shared_ptr<GameServer::TrackedGame> live =
            h.server.entry_locked(h.socket.get());
        assert(live);
        h.server.flush_entry(live);
        assert(read_text(dir / "deadbeef.json") == damaged);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_unreadable_resume_gets_a_fresh_token passed" << std::endl;
}

// H1: two tabs on one token fight over it, and the loser used to be cut off
// without a word. A bare close is indistinguishable from a dead network, so the
// loser reconnects, resumes the same token and takes the game back - once a
// second, with every handover halting the AI search. The notice has to reach the
// socket being replaced, before or with the close.
void test_resume_takeover_notifies_the_loser() {
    const auto dir = scratch("takeover_sessions");
    const auto records = scratch("takeover_records.jsonl");
    {
        // The frame the server sends the socket it is replacing. One constant:
        // a rename on the server side is a one-line change here.
        const std::string kTakeoverFrame = "session_taken_over";

        Harness h(dir, records);  // socket A, attached to the token
        const std::string token = h.entry->token;

        auto second = std::make_shared<WebSocketSession>(tcp::socket(h.server.ioc_), std::string());
        size_t seen_second = 0;
        h.server.handle_resume(second, json{{"token", token}});

        const std::vector<std::string> second_frames = drain_frames(h.server, second, seen_second);
        bool second_resumed = false;
        for (const std::string& frame : second_frames) {
            const json parsed = json::parse(frame);
            if (parsed["type"] == "session") second_resumed = (parsed["resumed"] == true);
        }
        assert(second_resumed);  // the newcomer took the session

        // The replaced socket gets exactly one notice, and it is in its queue
        // although the socket has already been shut down: the notice was handed
        // to the writer before the close, which is what makes it observably
        // different from a dropped connection.
        size_t seen_first = 0;
        const std::vector<std::string> first_frames = drain_frames(h.server, h.socket, seen_first);
        int notices = 0;
        for (const std::string& frame : first_frames) {
            const json parsed = json::parse(frame);
            if (parsed["type"] == kTakeoverFrame) {
                ++notices;
                if (parsed.contains("token")) assert(parsed["token"] == token);
            }
        }
        assert(notices == 1);
        assert(!h.socket->ws_.next_layer().socket().is_open());  // and it is closed

        const auto entry_it = h.server.sessions_.find(token);
        assert(entry_it != h.server.sessions_.end());
        assert(entry_it->second->attached == second.get());
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_resume_takeover_notifies_the_loser passed" << std::endl;
}

// Round 88, S1: a hint answers the board as it looked when 求招 was pressed.
// make_move and undo do not wait for the in-flight search, so a stone that lands
// while the engine thinks makes its result advice about a position that no longer
// exists. The delivery slot then carries an invalid Move, which is exactly the
// shape server.cpp turns into the error frame instead of a hint_result the client
// would highlight on a stale board.
// 一个快路径接不住的中局：8 子、分散、双方都还没成活三。开局库与强制手都不会
// 替它答，所以 `request_hint` 必然进 alphabeta —— 这是这条用例的前提能被构造出来、
// 而不是靠「睡 100 ms 赌它还没答完」的原因。
GameRestore hint_pose_restore() {
    GameRestore restore;
    restore.player_color = BLACK;
    restore.status = GameStatus::PLAYING;
    const Position cells[8] = {{3, 3}, {11, 4}, {4, 10}, {13, 12},
                               {7, 2}, {2, 12}, {12, 7}, {8, 9}};
    for (int i = 0; i < 8; ++i) {
        MoveAnalysis analysis;
        analysis.move = Move(cells[i].row, cells[i].col, i % 2 == 0 ? BLACK : WHITE);
        analysis.method = "human";
        restore.moves.push_back(analysis);
    }
    return restore;
}

void test_hint_answer_for_a_superseded_position() {
    const auto dir = scratch("stale_hint_sessions");
    const auto records = scratch("stale_hint_records.jsonl");
    {
        Harness h(dir, records);
        GameManager& game = *h.entry->game;
        assert(game.restore_game(hint_pose_restore()));   // 8 子，BLACK 应手
        assert(game.current_turn() == BLACK);
        const int armed_at = game.board().move_count();
        assert(armed_at == 8);

        size_t seen = 0;
        h.server.handle_request_hint(h.socket, json{{"timeout_ms", 3000}, {"max_depth", 8}});
        assert(game.hint_active_.load());

        // **前提要直接看，不能拿墙钟当代理。**以前这里是「睡 100 ms → 悔棋 → 断言
        // 总耗时 > 500 ms」，而 memcheck 下那 500 ms 很可能是悔棋自己花掉的：答案早在
        // 悔棋之前就合法送达了，于是后面那条 `hint_results == 0` 就成了在怪产品「丢了
        // 一个合法推荐」。现在改成：悔棋之前先把已经回来的帧收干净，亲眼确认里面没有
        // hint_result —— 有，就是**前提没成立**，报这一条，而不是让不变量去背锅。
        int early_results = 0;
        for (const std::string& frame : drain_frames(h.server, h.socket, seen)) {
            const json parsed = json::parse(frame, nullptr, false);
            if (!parsed.is_discarded() && parsed.contains("type") &&
                parsed["type"] == "hint_result") ++early_results;
        }
        assert(early_results == 0 &&
               "前提没成立：悔棋之前 hint_result 就已合法送达，这条用例测不到「被取代」");

        int undone_count = -1;
        GameStatus undone_status = GameStatus::WAITING;
        assert(game.undo(undone_count, undone_status));
        assert(undone_count == armed_at - 2 &&
               "悔棋要退两子，快照的 move_count 才不再等于盘面");
        // The undo must not have armed an AI worker either, or the answer would
        // be dropped at the discard gate rather than at the stale-position check.
        assert(!game.is_ai_thinking());

        for (int i = 0; i < 300 && game.hint_active_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        assert(!game.hint_active_.load());
        // 只作参考，不参与判定：这一手到底跑了多久不是前提，前提已经在上面看过了。
        std::cout << "  [r168] 被取代的 hint：悔棋后仍在跑的这段搜索交付前盘面 = "
                  << armed_at << "，悔棋后 = " << undone_count << std::endl;

        int hint_results = 0;
        std::string answer_type;
        std::string answer_message;
        for (const std::string& frame : drain_frames(h.server, h.socket, seen)) {
            const json parsed = json::parse(frame, nullptr, false);
            if (parsed.is_discarded() || !parsed.contains("type")) continue;
            const std::string type = parsed["type"].get<std::string>();
            if (type != "hint_result" && type != "error") continue;
            if (type == "hint_result") ++hint_results;
            answer_type = type;
            if (parsed.contains("message") && parsed["message"].is_string()) {
                answer_message = parsed["message"].get<std::string>();
            }
        }
        assert(hint_results == 0 &&
               "被取代的 hint 不许交付 hint_result（前提已在悔棋前亲眼查过）");
        assert(answer_type == "error");
        assert(answer_message == "No hint available");
        std::cout << "  [r168] stale hint: " << answer_type << " / " << answer_message
                  << "（盘面从 " << armed_at << " 子退到 " << undone_count
                  << " 子之后才有答案）" << std::endl;

        // Anti-overreach: with nothing touching the board the next hint is still a
        // hint_result on a legal point.
        h.server.handle_request_hint(h.socket, json{{"timeout_ms", 3000}, {"max_depth", 8}});
        assert(game.hint_active_.load());
        for (int i = 0; i < 300 && game.hint_active_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        assert(!game.hint_active_.load());

        int clean_results = 0;
        int clean_errors = 0;
        int clean_row = -1;
        int clean_col = -1;
        for (const std::string& frame : drain_frames(h.server, h.socket, seen)) {
            const json parsed = json::parse(frame, nullptr, false);
            if (parsed.is_discarded() || !parsed.contains("type")) continue;
            const std::string type = parsed["type"].get<std::string>();
            if (type == "error") ++clean_errors;
            if (type != "hint_result") continue;
            ++clean_results;
            clean_row = parsed.value("row", -1);
            clean_col = parsed.value("col", -1);
        }
        assert(clean_errors == 0);
        assert(clean_results == 1);
        assert(clean_row >= 0 && clean_row < BOARD_SIZE);
        assert(clean_col >= 0 && clean_col < BOARD_SIZE);
        assert(game.board().is_empty(clean_row, clean_col));
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_hint_answer_for_a_superseded_position passed" << std::endl;
}

// R97 BB2: the server's own door. handle_request_hint forwards whatever
// request_hint answers, and until the finished game was refused the answer was
// true - so 求招 on a dead board bought a full search and then a hint_result
// highlighting a point nobody will play. With the gate the request is refused
// where it stands: the existing error frame, no worker, and none of the timeout
// spent.
void test_finished_game_answers_a_hint_with_the_error_frame() {
    const auto dir = scratch("dead_hint_sessions");
    const auto records = scratch("dead_hint_records.jsonl");
    {
        Harness h(dir, records);
        GameManager& game = *h.entry->game;
        assert(game.restore_game(playing_restore(1)));  // one black stone, WHITE to move
        assert(game.status() == GameStatus::PLAYING);

        size_t seen = 0;
        // Control, one step before the end: the same door gives a real hint.
        h.server.handle_request_hint(h.socket, json{{"timeout_ms", 400}, {"max_depth", 1}});
        assert(game.hint_active_.load());
        for (int i = 0; i < 200 && game.hint_active_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        assert(!game.hint_active_.load());
        int live_results = 0;
        int live_errors = 0;
        for (const std::string& frame : drain_frames(h.server, h.socket, seen)) {
            const json parsed = json::parse(frame, nullptr, false);
            if (parsed.is_discarded() || !parsed.contains("type")) continue;
            const std::string type = parsed["type"].get<std::string>();
            if (type == "hint_result") ++live_results;
            if (type == "error") ++live_errors;
        }
        assert(live_results == 1);
        assert(live_errors == 0);

        // End the game the way the client does.
        h.server.handle_resign(h.socket);
        assert(game.status() == GameStatus::GAME_OVER);

        const auto t0 = std::chrono::steady_clock::now();
        h.server.handle_request_hint(h.socket, json{{"timeout_ms", 3000}, {"max_depth", 8}});
        const long long refused_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        // No worker was armed, and the answer did not cost the 3 s it asked for.
        assert(!game.hint_active_.load());
        assert(refused_ms < 500);

        int dead_results = 0;
        int dead_errors = 0;
        std::string dead_message;
        for (const std::string& frame : drain_frames(h.server, h.socket, seen)) {
            const json parsed = json::parse(frame, nullptr, false);
            if (parsed.is_discarded() || !parsed.contains("type")) continue;
            const std::string type = parsed["type"].get<std::string>();
            if (type == "hint_result") ++dead_results;
            if (type != "error") continue;
            ++dead_errors;
            if (parsed.contains("message") && parsed["message"].is_string()) {
                dead_message = parsed["message"].get<std::string>();
            }
        }
        assert(dead_errors == 1);
        assert(dead_results == 0);
        assert(dead_message == "No hint available");

        // And nothing arrives late either: wait past the timeout the request
        // asked for and the dead game still has no recommendation to deliver.
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        for (const std::string& frame : drain_frames(h.server, h.socket, seen)) {
            const json parsed = json::parse(frame, nullptr, false);
            if (parsed.is_discarded() || !parsed.contains("type")) continue;
            if (parsed["type"].get<std::string>() != "hint_result") continue;
            ++dead_results;
        }
        assert(dead_results == 0);

        std::cout << "test_finished_game_answers_a_hint_with_the_error_frame passed "
                  << "(refused in " << refused_ms << " ms of a asked 3000)" << std::endl;
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
}

// Round 98 CC2: a BOM at the head of a string payload states the file's encoding,
// it is not document content. It used to reach json::parse, which discarded the
// text, and the fallback then handed the very same characters to the grid reader -
// so a perfectly stated board was refused for the shape of its characters.
void test_check_position_reads_a_bom_prefixed_document() {
    const std::string bom = "\xEF\xBB\xBF";
    json matrix = json::array();
    for (int r = 0; r < BOARD_SIZE; ++r) {
        json row = json::array();
        for (int c = 0; c < BOARD_SIZE; ++c) row.push_back(0);
        matrix.push_back(std::move(row));
    }
    matrix[7][7] = 1;
    matrix[8][8] = 1;
    matrix[6][6] = 2;
    matrix[9][9] = 2;
    json doc;
    doc["board"] = matrix;
    doc["encoding"] = {{"black", "1"}, {"white", "2"}, {"empty", "0"}};
    const std::string text = doc.dump();

    std::string grid;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            const bool black = (r == 7 && c == 7) || (r == 8 && c == 8);
            const bool white = (r == 6 && c == 6) || (r == 9 && c == 9);
            grid += black ? "1" : white ? "2" : "0";
            grid += (c + 1 < BOARD_SIZE) ? "," : "\n";
        }
    }

    const auto dir = scratch("r98cc2_sessions");
    {
        Harness h(dir, std::string());
        size_t seen = 0;
        auto preview = [&h, &seen](const std::string& payload) {
            h.server.handle_check_position(h.socket, json{{"position", payload}});
            const std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
            assert(frames.size() == 1);
            const json parsed = json::parse(frames[0]);
            assert(parsed["type"] == "position_preview");
            return parsed;
        };

        const json plain = preview(text);
        assert(plain["ok"] == true);
        assert(plain["black_count"] == 2);
        assert(plain["white_count"] == 2);
        assert(plain["side_to_move"] == "black");

        // The same document with the mark in front of it, and with a line break
        // between the mark and the brace: one frame each, and the same board.
        const std::string prefixed_payloads[] = {bom + text, bom + "\n" + text};
        for (const std::string& payload : prefixed_payloads) {
            const json read = preview(payload);
            assert(read["ok"] == true);
            assert(read["black_count"] == plain["black_count"]);
            assert(read["white_count"] == plain["white_count"]);
            assert(read["side_to_move"] == plain["side_to_move"]);
            assert(read["board"] == plain["board"]);
        }

        // The grid branch already stripped a BOM, and it still does.
        const json grid_plain = preview(grid);
        const json grid_bom = preview(bom + grid);
        assert(grid_plain["ok"] == true);
        assert(grid_bom["ok"] == true);
        assert(grid_bom["board"] == grid_plain["board"]);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_check_position_reads_a_bom_prefixed_document passed" << std::endl;
}

// Round 98 CC3: a human stone that finishes the game answered with the ending
// first and the stone second, because make_move fires the game_over callback on
// the way in while handle_make_move queued move_made only afterwards. The client
// paints the winning stone from move_made, so 终局 landed on a board one stone
// short of the five. The AI path already sends its stone before the end
// (game_manager.cpp:424-426); the human path now holds the end frame back until
// the stone has gone out.
void test_human_move_answers_the_stone_before_the_end() {
    json matrix = json::array();
    for (int r = 0; r < BOARD_SIZE; ++r) {
        json row = json::array();
        for (int c = 0; c < BOARD_SIZE; ++c) row.push_back(0);
        matrix.push_back(std::move(row));
    }
    // Four black in a row, both ends open, four white far away: black to move and
    // (7,11) is the five.
    for (int c = 7; c <= 10; ++c) matrix[7][c] = 1;
    matrix[5][5] = 2;
    matrix[6][6] = 2;
    matrix[9][12] = 2;
    matrix[10][13] = 2;
    json four;
    four["board"] = matrix;
    four["encoding"] = {{"black", "1"}, {"white", "2"}, {"empty", "0"}};

    auto types_of = [](const std::vector<std::string>& frames) {
        std::string joined;
        for (const std::string& frame : frames) {
            const json parsed = json::parse(frame, nullptr, false);
            if (parsed.is_discarded() || !parsed.contains("type")) continue;
            joined += (joined.empty() ? "" : ",") + parsed["type"].get<std::string>();
        }
        return joined;
    };

    const auto dir = scratch("r98cc3_sessions");
    {
        // (1) No side chosen: the stone claims black and ends the game, and the
        // two frames owe their documented order.
        Harness h(dir, std::string());
        size_t seen = 0;
        h.server.handle_set_position(h.socket, json{{"position", four}});
        std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        assert(json::parse(frames[0])["type"] == "position_set");
        assert(h.entry->game->current_turn() == BLACK);

        h.server.handle_make_move(h.socket, json{{"row", 7}, {"col", 11}});
        frames = drain_frames(h.server, h.socket, seen);
        std::cout << "  [r98cc3] frames after the finishing stone: " << types_of(frames)
                  << std::endl;
        assert(h.entry->game->status() == GameStatus::GAME_OVER);
        assert(types_of(frames) == "move_made,game_over");
        const json stone = json::parse(frames[0]);
        assert(stone["row"] == 7 && stone["col"] == 11);
        assert(stone["color"] == "black");
        const json end = json::parse(frames[1]);
        assert(end["winner"] == "black");
        assert(end["reason"] == "five");
    }
    {
        // (2) Control: a stone that finishes nothing owes no ending at all. The
        // long budget keeps the armed worker from answering during the drain.
        Harness h(dir, std::string());
        size_t seen = 0;
        h.entry->game->set_config(120000, 20);
        h.server.handle_make_move(h.socket, json{{"row", 7}, {"col", 7}});
        h.entry->game->halt_search();
        const std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
        assert(types_of(frames) == "move_made");
    }
    {
        // (3) The same finish after 执黑: still stone-then-end, and the player is
        // owed no ai_move for a game that stopped with the stone.
        Harness h(dir, std::string());
        size_t seen = 0;
        h.server.handle_set_position(h.socket, json{{"position", four}});
        h.entry->game->set_config(120000, 20);
        h.server.handle_choose_side(h.socket, json{{"color", "black"}});
        assert(h.entry->game->player_color() == BLACK);
        assert(!h.entry->game->is_ai_thinking());
        drain_frames(h.server, h.socket, seen);

        h.server.handle_make_move(h.socket, json{{"row", 7}, {"col", 11}});
        const std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
        assert(types_of(frames) == "move_made,game_over");
        assert(!h.entry->game->is_ai_thinking());
    }
    {
        // (4) 认输 is not the deferred path: the ending goes straight out, once.
        Harness h(dir, std::string());
        size_t seen = 0;
        h.entry->game->set_config(120000, 20);
        h.server.handle_make_move(h.socket, json{{"row", 7}, {"col", 7}});
        h.entry->game->halt_search();
        drain_frames(h.server, h.socket, seen);
        assert(h.entry->game->player_color() == BLACK);

        h.server.handle_resign(h.socket);
        assert(h.entry->game->status() == GameStatus::GAME_OVER);
        const std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
        assert(types_of(frames) == "game_over");
        assert(json::parse(frames[0])["winner"] == "white");
        assert(json::parse(frames[0])["reason"] == "resign");
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_human_move_answers_the_stone_before_the_end passed" << std::endl;
}

// Round 99 DD3: the reject branch of handle_make_move gave the deferred_end_for_
// slot back but left whatever a worker had parked in deferred_end_frame_ sitting
// there. That ending reached neither this session - its own stone was refused, so
// no success path ever drained the slot - nor anybody else, until the NEXT
// session played a legal stone and was handed a game_over for a game it is not
// part of. The window is real: server.cpp:941-944 arms the slot, and an AI worker
// committing a finishing stone under it fires the callback that parks the frame
// (game_manager.cpp:424-426, server.cpp:1626-1634). Two statements cannot be hit
// by timing in a test, so the slot is armed exactly the way that block arms it
// and the worker is then run to completion: the state entering the reject branch
// is the state the race produces, and the parked frame is the server's own
// callback output rather than one written by this test.
void test_a_rejected_stone_delivers_the_end_a_worker_parked() {
    GameRestore white_finishing;
    white_finishing.player_color = BLACK;
    white_finishing.status = GameStatus::PLAYING;
    // Nine alternating plies, so WHITE (the AI) is to move, and its four on row 7
    // has both ends free: the worker's stone finishes the game.
    const Position black_cells[5] = {{0, 0}, {0, 2}, {0, 4}, {2, 0}, {4, 0}};
    const Position white_cells[4] = {{7, 7}, {7, 8}, {7, 9}, {7, 10}};
    for (int i = 0; i < 9; ++i) {
        MoveAnalysis analysis;
        analysis.move = i % 2 == 0
            ? Move(black_cells[i / 2].row, black_cells[i / 2].col, BLACK)
            : Move(white_cells[i / 2].row, white_cells[i / 2].col, WHITE);
        analysis.method = "human";
        white_finishing.moves.push_back(analysis);
    }

    auto types_of = [](const std::vector<std::string>& frames) {
        std::string joined;
        for (const std::string& frame : frames) {
            const json parsed = json::parse(frame, nullptr, false);
            if (parsed.is_discarded() || !parsed.contains("type")) continue;
            joined += (joined.empty() ? "" : ",") + parsed["type"].get<std::string>();
        }
        return joined;
    };
    auto index_of = [](const std::vector<std::string>& frames, const char* type) {
        for (size_t i = 0; i < frames.size(); ++i) {
            const json parsed = json::parse(frames[i], nullptr, false);
            if (!parsed.is_discarded() && parsed.value("type", std::string()) == type) {
                return static_cast<long>(i);
            }
        }
        return -1L;
    };

    const auto dir = scratch("r99dd3_sessions");
    {
        Harness h(dir, std::string());
        size_t seen = 0;
        assert(h.entry->game->restore_game(white_finishing));
        assert(h.entry->game->current_turn() == WHITE);
        h.entry->game->set_config(300, 1);

        {
            std::lock_guard<std::mutex> lock(h.server.end_frame_mutex_);
            h.server.deferred_end_for_ = h.socket.get();
        }
        h.entry->game->start_ai_thinking();
        // Wait on the event itself rather than on a clock: the frame the worker's
        // callback parks under the armed slot. Round 111 OO3 widened the ceiling from
        // 2000 x 5 ms (10 s) to 60 s: the wait is a bound on a worker that is doing real
        // search on a shared box, so under a saturated gate it simply loses the race - the
        // assertion below is unchanged, a frame that never arrives is still a failure.
        bool parked = false;
        for (int spin = 0; spin < 12000 && !parked; ++spin) {
            // 轮次193 #106：worker 想完只 post 一句，落子与 game_over 都在属主（这里就是
            // 测试自己充当的那条 io 线程）上跑 —— 不泵队列，那个回调永远不会发生。
            h.server.ioc_.restart();
            h.server.ioc_.poll();
            {
                std::lock_guard<std::mutex> lock(h.server.end_frame_mutex_);
                parked = !h.server.deferred_end_frame_.is_null();
            }
            if (!parked) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!parked) {
            std::lock_guard<std::mutex> lock(h.server.end_frame_mutex_);
            std::cout << "  [r99dd3] TIMEOUT after 60 s: ai_thinking="
                      << h.entry->game->ai_thinking_.load()
                      << " status=" << static_cast<int>(h.entry->game->status_)
                      << " deferred_end_for=" << static_cast<const void*>(h.server.deferred_end_for_)
                      << " session=" << static_cast<const void*>(h.socket.get())
                      << " same=" << (h.server.deferred_end_for_ == h.socket.get() ? "yes" : "no")
                      << std::endl;
        }
        assert(parked);

        // (13,13) is empty; the stone is refused by the game-over guard at
        // game_manager.cpp:148, which is what the worker's five leaves behind.
        h.server.handle_make_move(h.socket, json{{"row", 13}, {"col", 13}});
        const std::vector<std::string> refused_stone = drain_frames(h.server, h.socket, seen);

        // A second session on the same server plays a legal stone. Its success path
        // is what drains a frame this session should have been told about.
        auto other_socket =
            std::make_shared<WebSocketSession>(tcp::socket(h.server.ioc_), std::string());
        const std::shared_ptr<GameServer::TrackedGame> other = h.server.open_session(nullptr);
        h.server.attach_socket(other_socket, other);
        size_t seen_other = 0;
        assert(other->game->restore_game(playing_restore(2)));
        other->game->set_config(120000, 20);
        h.server.handle_make_move(other_socket, json{{"row", 5}, {"col", 5}});
        other->game->halt_search();
        const std::vector<std::string> next_stone =
            drain_frames(h.server, other_socket, seen_other);

        std::cout << "  [r99dd3] refused stone: " << types_of(refused_stone)
                  << " | next session's stone: " << types_of(next_stone) << std::endl;

        const long refusal = index_of(refused_stone, "error");
        const long ending = index_of(refused_stone, "game_over");
        assert(refusal >= 0);
        assert(ending > refusal);
        const json end = json::parse(refused_stone[static_cast<size_t>(ending)]);
        assert(end["winner"] == "white");
        assert(end["reason"] == "five");

        {
            std::lock_guard<std::mutex> lock(h.server.end_frame_mutex_);
            assert(h.server.deferred_end_for_ == nullptr);
            assert(h.server.deferred_end_frame_.is_null());
        }
        assert(types_of(next_stone) == "move_made");
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_a_rejected_stone_delivers_the_end_a_worker_parked passed" << std::endl;
}

// Round 88, S5: the two halves of a resumed session's search config stand on their
// own. A file that states max_depth but never states timeout_ms - 0 is the
// round-86 P2 sentinel for "key absent" - used to lose both at once, because one
// absent key skipped set_config entirely and the game silently kept its defaults.
void test_resume_restores_each_config_half() {
    const auto dir = scratch("config_half_sessions");
    {
        GameManager untouched;
        const int default_timeout = untouched.timeout_ms();
        const int default_depth = untouched.max_depth();
        std::cout << "  [r88s] default_config=" << default_timeout << "/"
                  << default_depth << std::endl;

        GameServer server(0, std::string(), std::string(), nullptr, dir.string(),
                          std::string());

        // Build each document with the writer itself, then patch the two config
        // keys, so the record schema is never re-implemented here. A json null in
        // the patch erases the key; the writer's own 0 is the round-86 P2
        // "never declared" sentinel.
        auto resume_with = [&server](const char* token, const json& stated,
                                     int& timeout, int& depth) {
            SessionSnapshot writable;
            writable.token = token;
            writable.player_color = "black";
            writable.status = "playing";
            json doc = json::parse(session_to_json(writable));
            for (auto it = stated.begin(); it != stated.end(); ++it) {
                if (it.value().is_null()) doc.erase(it.key());
                else doc[it.key()] = it.value();
            }
            SessionSnapshot snap;
            std::string error;
            const std::string text = doc.dump();
            if (!session_from_json(text, snap, error)) {
                std::cout << "  [r88s] " << token << " rejected: " << error << std::endl;
            }
            assert(session_from_json(text, snap, error));
            const std::shared_ptr<GameServer::TrackedGame> entry = server.open_session(&snap);
            assert(entry->rebuilt);
            timeout = entry->game->timeout_ms();
            depth = entry->game->max_depth();
        };

        int timeout = 0;
        int depth = 0;

        // Only the depth is stated, with the timeout left at the writer's 0.
        resume_with("aa01", {{"max_depth", 3}}, timeout, depth);
        assert(timeout == default_timeout);
        assert(depth == 3);

        // Only the timeout is stated.
        resume_with("aa02", {{"timeout_ms", 7000}}, timeout, depth);
        assert(timeout == 7000);
        assert(depth == default_depth);

        // Anti-overreach: neither stated, in both of the ways a file can say
        // that - the 0 sentinel and the key being absent outright. The absent
        // half must be fed the value this game already has, never a 0 that the
        // clamps would turn into 100 ms and depth 1.
        resume_with("aa03", json::object(), timeout, depth);
        assert(timeout == default_timeout);
        assert(depth == default_depth);
        resume_with("aa05", {{"timeout_ms", nullptr}, {"max_depth", nullptr}},
                    timeout, depth);
        assert(timeout == default_timeout);
        assert(depth == default_depth);

        // Both stated: both come back.
        resume_with("aa04", {{"timeout_ms", 7000}, {"max_depth", 4}}, timeout, depth);
        assert(timeout == 7000);
        assert(depth == 4);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_resume_restores_each_config_half passed" << std::endl;
}

// Round 90, U2: 摆棋 with zero stones is not content. README:541-543 counts a
// session as worth keeping by 落子 / 摆子 / 选边, and position_pending() - which
// hand-setting any position raises until someone takes a side - is none of those
// three. It used to sit in the reuse-vs-rotate test, so a page that loaded an
// empty board twice was handed a new token every click.
void test_zero_stone_position_keeps_the_token() {
    const auto dir = scratch("zero_stone_sessions");
    {
        Harness h(dir, std::filesystem::path());
        const json empty_position = {
            {"position", {{"version", 1}, {"moves", json::array()}}}};

        h.server.handle_set_position(h.socket, empty_position);
        const std::shared_ptr<GameServer::TrackedGame> first =
            h.server.entry_locked(h.socket.get());
        assert(first);
        assert(first->game->position_pending());
        assert(!first->side_chosen);
        // The board this session started on had nothing to keep either, so the
        // position landed on the token the socket already had.
        assert(first->token == h.entry->token);
        const std::string token = first->token;

        // A second 摆棋 of the same nothing, with the pending mark up: same entry,
        // same token.
        h.server.handle_set_position(h.socket, empty_position);
        const std::shared_ptr<GameServer::TrackedGame> again =
            h.server.entry_locked(h.socket.get());
        assert(again);
        assert(again.get() == first.get());
        assert(again->token == token);
        assert(again->game->position_pending());

        // 重开 answers the same way on the same untouched board.
        h.server.handle_restart(h.socket);
        const std::shared_ptr<GameServer::TrackedGame> restarted =
            h.server.entry_locked(h.socket.get());
        assert(restarted);
        assert(restarted.get() == first.get());
        assert(restarted->token == token);
        assert(!restarted->game->position_pending());

        // And none of this wrote a session file: an empty game is not kept.
        assert(!std::filesystem::exists(dir / (token + ".json")));
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_zero_stone_position_keeps_the_token passed" << std::endl;
}

// Round 94, Y1: a transient accept failure used to be the last thing the listener
// ever did - the completion handler returned without re-arming, so the port went
// unserved for the rest of the process while the sessions already open kept
// playing. The retry timer paces the re-armed loop, and operation_aborted, which
// is the shape a shutdown takes, stays the only error that ends it.
void test_transient_accept_failure_rearms_and_abort_ends() {
    using clock = std::chrono::steady_clock;
    beast::error_code ec;
    {
        // A closed acceptor completes async_accept with a descriptor error. That
        // is transient, not a shutdown, so every round has to leave another
        // attempt scheduled behind it.
        GameServer server(0, std::string(), std::string(), nullptr, std::string(),
                          std::string());
        server.acceptor_.close(ec);
        server.do_accept();

        auto previous = server.accept_retry_timer_.expiry();
        auto shortest_gap = clock::duration::max();
        int rearms = 0;
        for (int i = 0; i < 4; ++i) {
            server.ioc_.restart();
            server.ioc_.run_for(std::chrono::milliseconds(150));
            const auto deadline = server.accept_retry_timer_.expiry();
            if (deadline > previous) {
                ++rearms;
                shortest_gap = std::min(shortest_gap, deadline - previous);
            }
            previous = deadline;
        }
        assert(rearms >= 2);
        // The wait is what paces the retry: an inline re-accept in the same
        // handler stack would move the deadline by ~0 instead of ~100 ms.
        assert(shortest_gap >= std::chrono::milliseconds(50));

        server.accept_retry_timer_.cancel();
        server.ioc_.restart();
        server.ioc_.poll();
    }
    {
        // The shutdown shape: an accept that is still pending when the acceptor
        // closes completes aborted, and that on its own must end the loop.
        GameServer server(0, std::string(), std::string(), nullptr, std::string(),
                          std::string());
        const auto baseline = server.accept_retry_timer_.expiry();
        server.do_accept();
        server.acceptor_.close(ec);
        server.ioc_.restart();
        server.ioc_.run_for(std::chrono::milliseconds(300));
        assert(server.accept_retry_timer_.expiry() == baseline);
    }
    std::cout << "test_transient_accept_failure_rearms_and_abort_ends passed" << std::endl;
}

}  // namespace

// Round 111 OO1: one port, two protocols - README:294 is the browser opening
// http://localhost:8080, which is a plain GET for index.html/app.js/style.css before the
// WebSocket upgrade. handle_http_request() never read req_.method(), so it answered every
// verb as a GET: a POST to an existing path got 200 plus the file's own bytes, telling the
// sender its write had been accepted when nothing was written anywhere, and a HEAD - a
// request that by definition asks for headers only - got a body anyway. The handler now
// branches the verb: GET and HEAD share resolve_file and the status code, only GET fills
// the body, and any other method gets 405 with an Allow naming the two reads it knows.
//
// What is asserted: the status code, whether the body is the file's bytes, and the
// presence of the Content-Type / Allow fields. No message wording. One deliberate
// non-assertion: prepare_payload() writes Content-Length from the body it is handed, so a
// HEAD's is 0 - that is this round's semantics, so no arm pins it as the file size.
void test_http_answers_a_read_with_the_file_and_a_write_with_a_refusal() {
    const auto dir = scratch("r111_oo1_web");
    std::filesystem::create_directories(dir);
    // A sentinel no page in web/ contains, so "the body is the file" is a statement about
    // this response rather than about index.html.
    const std::string sentinel = "<!doctype html>r111-oo1-sentinel-index-bytes</html>\n";
    {
        std::ofstream file(dir / "index.html", std::ios::binary);
        file << sentinel;
    }

    net::io_context ioc;   // never run: a write on an unconnected socket just stays queued
    auto ask = [&dir, &ioc](http::verb method, const std::string& target) {
        auto session = std::make_shared<WebSocketSession>(tcp::socket(ioc), dir.string());
        session->req_.method(method);
        session->req_.target(target);
        session->handle_http_request();
        return session->res_;
    };
    auto has = [](const std::shared_ptr<http::response<http::string_body>>& res,
                  http::field name) {
        return res->find(name) != res->end();
    };

    // Discriminator 1: POST is not a read this handler can serve, so the file stays
    // unoffered and the answer says which methods are. HEAD's tree has no verb branch at
    // all and hands back 200 with the file.
    const auto posted = ask(http::verb::post, "/index.html");
    std::cout << "  [r111oo1] POST /index.html => " << static_cast<int>(posted->result())
              << ", body " << posted->body().size() << " bytes, allow "
              << (has(posted, http::field::allow)
                      ? std::string(posted->find(http::field::allow)->value())
                      : std::string("<absent>"))
              << std::endl;
    assert(posted->result() == http::status::method_not_allowed);
    assert(has(posted, http::field::allow));
    assert(posted->find(http::field::allow)->value().find("GET") != std::string::npos);
    assert(posted->body().find(sentinel) == std::string::npos);

    // Discriminator 2: HEAD asks for the headers of that same file. The status and the
    // Content-Type are the GET's, the body is not.
    const auto head = ask(http::verb::head, "/index.html");
    std::cout << "  [r111oo1] HEAD /index.html => " << static_cast<int>(head->result())
              << ", body " << head->body().size() << " bytes, content-type present "
              << (has(head, http::field::content_type) ? "yes" : "no") << std::endl;
    assert(head->result() == http::status::ok);
    assert(head->body().empty());
    assert(has(head, http::field::content_type));

    // Control, green before and after: the GET the browser actually sends is unchanged,
    // byte for byte, and a miss is still a 404.
    const auto got = ask(http::verb::get, "/index.html");
    std::cout << "  [r111oo1] GET /index.html => " << static_cast<int>(got->result())
              << ", body " << got->body().size() << " bytes, HEAD agrees on content-type "
              << (got->find(http::field::content_type)->value() ==
                          head->find(http::field::content_type)->value()
                      ? "yes"
                      : "no") << std::endl;
    assert(got->result() == http::status::ok);
    assert(got->body() == sentinel);
    assert(has(got, http::field::content_type));
    const auto missed = ask(http::verb::get, "/nope.html");
    assert(missed->result() == http::status::not_found);

    // The same branch, two more verbs: 405 is what a non-read gets, not what POST in
    // particular gets. HEAD's handler has no verb branch at all, so these arms are red
    // there too - they widen the discriminator, they are not both-trees-green controls.
    const http::verb methods[] = {http::verb::put, http::verb::delete_};
    for (const http::verb other : methods) {
        const auto refused = ask(other, "/index.html");
        std::cout << "  [r111oo1] " << (other == http::verb::put ? "PUT" : "DELETE")
                  << " /index.html => " << static_cast<int>(refused->result()) << ", allow "
                  << (has(refused, http::field::allow) ? "set" : "absent") << std::endl;
        assert(refused->result() == http::status::method_not_allowed);
        assert(has(refused, http::field::allow));
        assert(refused->body().find(sentinel) == std::string::npos);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout
        << "test_http_answers_a_read_with_the_file_and_a_write_with_a_refusal passed"
        << std::endl;
}

// Round 111 OO2: resolve_file() walked the target segment by segment and refused any
// segment that was not a name, so a single `.` was rejected with `..`. They are not the
// same request: `..` asks for a file outside web_dir_ (README:139 is the directory being
// rooted in), and `.` asks for this one - `/./index.html` *is* index.html. HEAD's tree
// answers that with a 404. The guard now refuses only `..` by name and lets
// weakly_canonical plus the root-prefix check at server.cpp:450-459 decide, which is what
// actually holds the line: every control below was red-proof before and stays green now.
void test_resolve_file_reads_a_single_dot_segment_and_still_refuses_every_escape() {
    const auto dir = scratch("r111_oo2_web");
    std::filesystem::create_directories(dir / "a");
    const std::string index = "<!doctype html>r111-oo2-index</html>\n";
    {
        std::ofstream file(dir / "index.html", std::ios::binary);
        file << index;
    }
    {
        std::ofstream file(dir / "a" / "b.css", std::ios::binary);
        file << "body{margin:0}\n";
    }

    net::io_context ioc;
    auto session = std::make_shared<WebSocketSession>(tcp::socket(ioc), dir.string());
    auto resolved = [&session](const std::string& target) {
        return session->resolve_file(target).string();
    };

    // Discriminator: one file, two spellings.
    const std::string plain = resolved("/index.html");
    const std::string dotted = resolved("/./index.html");
    std::cout << "  [r111oo2] /index.html hit=" << (plain.empty() ? "no" : "yes")
              << ", /./index.html hit=" << (dotted.empty() ? "no" : "yes") << std::endl;
    assert(!plain.empty());
    assert(dotted == plain);
    // An intermediate `.` segment is the same no-op.
    assert(resolved("/a/./b.css") == resolved("/a/b.css"));
    assert(!resolved("/a/b.css").empty());

    // The same answer through the handler, not just the resolver: a browser that spells
    // its own root with a dot still gets the page instead of a 404.
    session->req_.method(http::verb::get);
    session->req_.target("/./index.html");
    session->handle_http_request();
    std::cout << "  [r111oo2] GET /./index.html => "
              << static_cast<int>(session->res_->result()) << ", body "
              << session->res_->body().size() << " bytes" << std::endl;
    assert(session->res_->result() == http::status::ok);
    assert(session->res_->body() == index);

    // Controls: the escapes are still refused, and each is refused for the reason the
    // comment above names - `..` by name, the rest by the entry check or the root prefix.
    const char* const escapes[] = {"/../index.html", "/x/../../etc/passwd",
                                   "/./../index.html", "/a/../index.html",
                                   "/%2e%2e/index.html", "/a//b", "/..", "index.html",
                                   "/\\..\\index.html"};
    for (const char* target : escapes) {
        const std::string hit = resolved(target);
        std::cout << "  [r111oo2] " << target << " => " << (hit.empty() ? "refused"
                                                                        : "served")
                  << std::endl;
        assert(hit.empty());
    }

    // Control: `.` alone names the directory, and a directory is not a regular file, so
    // reading the root itself is still a miss - the dot is allowed, not the listing.
    assert(resolved("/.").empty());
    assert(resolved("/./").empty());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_resolve_file_reads_a_single_dot_segment_and_still_refuses_every_escape"
                 " passed" << std::endl;
}

// VV1g/VV1h: the same rule the position layer enforces, driven over the socket so the
// path is the one the 摆棋 弹窗 actually takes. README:536 keeps `encoding` inside the
// board form, so a bitboard upload that carries a half-written `encoding` at the top
// level is still a position - on HEAD the field was read before the form was chosen and
// the whole document came back as an error frame.
void test_a_half_written_encoding_does_not_veto_a_bitboard_upload() {
    const auto dir = scratch("vv1_sessions");
    {
        Harness h(dir, std::string());
        size_t seen = 0;

        json words;
        words["black"] = json::array({"0x0", "0x0001000000000000", "0x10001", "0x0"});
        words["white"] = json::array({"0x0", "0x0002000100000000", "0x0", "0x0"});
        json bitboard;
        bitboard["version"] = 1;
        bitboard["bitboard"] = std::move(words);

        // VV1g: the top-level encoding names only black, and the bitboard lands.
        h.server.handle_set_position(
            h.socket, json{{"position", bitboard},
                           {"encoding", {{"black", 1}}}});
        std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        const json installed = json::parse(frames[0]);
        std::cout << "  [r118vv1g] frame type: " << installed.value("type", std::string())
                  << std::endl;
        assert(installed["type"] == "position_set");
        assert(installed["position_pending"] == true);
        assert(installed["black_count"] == 3);
        assert(installed["white_count"] == 2);
        assert(installed["current_turn"] == "white");
        assert(h.entry->game->position_pending());
        assert(h.entry->game->board().move_count() == 5);
        assert(h.entry->game->board().get(7, 7) == BLACK);
        assert(h.entry->game->board().get(8, 8) == BLACK);
        assert(h.entry->game->board().get(9, 9) == BLACK);
        assert(h.entry->game->board().get(6, 6) == WHITE);
        assert(h.entry->game->board().get(7, 8) == WHITE);

        // VV1h control: the same bitboard with no encoding at all is unchanged. A
        // second socket, because re-setting on a board that already has stones rotates
        // the session and puts an extra frame in front of the reply.
        Harness plain(dir, std::string());
        size_t plain_seen = 0;
        plain.server.handle_set_position(plain.socket, json{{"position", bitboard}});
        frames = drain_frames(plain.server, plain.socket, plain_seen);
        assert(frames.size() == 1);
        const json answer = json::parse(frames[0]);
        assert(answer["type"] == "position_set");
        assert(answer["position_pending"] == true);
        assert(answer["black_count"] == 3);
        assert(answer["white_count"] == 2);
        assert(answer["current_turn"] == "white");
        assert(plain.entry->game->board().move_count() == 5);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_a_half_written_encoding_does_not_veto_a_bitboard_upload passed"
              << std::endl;
}

// A7 (design doc 2.3.3, G2): the progress frame has to say which layer broke and how
// far into it the think got, because `depth` alone cannot tell "ran out of budget
// inside layer 6" from "had nothing to search". The accounting itself is proved in
// test_search against an injected clock; what is proved here is the wire - the five
// new keys, each `stopped` reason reaching the client by name, and the old three
// keeping the meanings they had when the frame carried only those.
void test_thinking_progress_carries_partial_fields() {
    const auto dir = scratch("progress_a7_sessions");
    {
        Harness h(dir, std::filesystem::path());
        size_t seen = 0;

        SearchStats cut;
        cut.completed_depth = 5;
        cut.partial_depth = 6;
        cut.root_moves_done = 12;
        cut.root_moves_total = 34;
        cut.seldepth = 7;
        cut.nodes = 123456;
        cut.elapsed_ms = 4800;
        cut.stopped = StopReason::TIME_HARD;
        h.entry->game->progress_callback_(cut);
        std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        const json hard = json::parse(frames[0]);
        std::cout << "  [r121a7] progress frame: " << hard.dump() << std::endl;
        assert(hard["type"] == "thinking_progress");
        assert(hard["depth"] == 5);
        assert(hard["nodes"] == 123456);
        assert(hard["elapsed_ms"] == 4800);
        assert(hard["partial_depth"] == 6);
        assert(hard["root_moves_done"] == 12);
        assert(hard["root_moves_total"] == 34);
        assert(hard["seldepth"] == 7);
        assert(hard["stopped"] == "time_hard");

        // Control: a think stopped at a layer boundary interrupted nothing, so the
        // same keys answer 0 rather than going missing on the client.
        SearchStats at_boundary;
        at_boundary.completed_depth = 8;
        at_boundary.nodes = 987654;
        at_boundary.elapsed_ms = 1200;
        at_boundary.seldepth = 9;
        at_boundary.stopped = StopReason::TIME_SOFT;
        h.entry->game->progress_callback_(at_boundary);
        frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        const json soft = json::parse(frames[0]);
        assert(soft["depth"] == 8);
        assert(soft["partial_depth"] == 0);
        assert(soft["root_moves_done"] == 0);
        assert(soft["root_moves_total"] == 0);
        assert(soft["seldepth"] == 9);
        assert(soft["stopped"] == "time_soft");

        // Every reason arrives as a name, and "none" is the only one the page stays
        // silent on. The three the page cannot produce by itself (a user 终止, a
        // proven mate, an exhausted node budget) are named like the rest.
        const StopReason reasons[] = {StopReason::NONE, StopReason::TIME_SOFT,
                                      StopReason::TIME_HARD, StopReason::NODES,
                                      StopReason::USER, StopReason::MATE};
        const char* const names[] = {"none", "time_soft", "time_hard", "nodes",
                                     "user", "mate"};
        for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
            SearchStats stats;
            stats.stopped = reasons[i];
            h.entry->game->progress_callback_(stats);
            frames = drain_frames(h.server, h.socket, seen);
            assert(frames.size() == 1);
            const json answer = json::parse(frames[0]);
            std::cout << "  [r121a7] stopped " << names[i] << " => "
                      << answer.value("stopped", std::string()) << std::endl;
            assert(answer["stopped"] == names[i]);
            // A frame that says nothing else must still be a whole frame: the old
            // three keys are always present, at zero, never absent.
            assert(answer["depth"] == 0);
            assert(answer["nodes"] == 0);
            assert(answer["elapsed_ms"] == 0);
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_thinking_progress_carries_partial_fields passed" << std::endl;
}

// A7: the accounting is only worth having if it survives the three hops it is used
// for - into the `replay` frame 回放 reads, into the session file and back out of
// it, and out of a file written before the keys existed, which reads as "every layer
// finished" (README:531).
void test_replay_roundtrip_keeps_new_fields() {
    const auto dir = scratch("replay_a7_sessions");
    const auto records = scratch("replay_a7_records.jsonl");
    const auto wall_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    };
    {
        Harness h(dir, records);
        GameRestore restore = playing_restore(2);
        MoveAnalysis& broken = restore.moves[1];
        broken.method = "alphabeta";
        broken.score = 500;
        broken.depth = 5;
        broken.nodes = 123456;
        broken.elapsed_ms = 4800;
        broken.partial_depth = 6;
        broken.root_moves_done = 12;
        broken.root_moves_total = 34;
        broken.seldepth = 7;
        broken.stopped = "time_hard";
        assert(h.entry->game->restore_game(restore));
        const std::string token = h.entry->token;

        // 1. The frame the client reads.
        size_t seen = 0;
        h.server.handle_request_replay(h.socket);
        std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        json replay = json::parse(frames[0]);
        assert(replay["type"] == "replay");
        assert(replay["moves"].size() == 2);
        std::cout << "  [r121a7] replay ply 2: " << replay["moves"][1].dump() << std::endl;
        assert(replay["moves"][1]["partial_depth"] == 6);
        assert(replay["moves"][1]["root_moves_done"] == 12);
        assert(replay["moves"][1]["root_moves_total"] == 34);
        assert(replay["moves"][1]["seldepth"] == 7);
        assert(replay["moves"][1]["stopped"] == "time_hard");
        // Control: the neighbour ply was never cut and does not borrow the answer.
        assert(replay["moves"][0]["partial_depth"] == 0);
        assert(replay["moves"][0]["stopped"] == "none");

        // 2. The file on disk, written by the session's own writer.
        h.server.flush_entry(h.entry);
        SessionSnapshot on_disk;
        std::string error;
        assert(load_session(dir.string(), token, on_disk, error));
        assert(on_disk.record.moves.size() == 2);
        assert(on_disk.record.moves[1].partial_depth == 6);
        assert(on_disk.record.moves[1].root_moves_done == 12);
        assert(on_disk.record.moves[1].root_moves_total == 34);
        assert(on_disk.record.moves[1].seldepth == 7);
        assert(on_disk.record.moves[1].stopped == "time_hard");

        // 3. The same file with the five keys erased - what a pre-A7 session looks
        // like. It is a readable record with no answer to "which layer broke", not a
        // broken one, so the reader's job is to say 0 / "none" and still build.
        json legacy = json::parse(read_text(h.session_file()));
        legacy["token"] = "a7000000000000f7";
        for (json& entry : legacy["record"]["moves"]) {
            for (const char* key : {"partial_depth", "root_moves_done",
                                    "root_moves_total", "seldepth", "stopped"}) {
                entry.erase(key);
            }
        }
        const std::string legacy_text = legacy.dump();
        assert(legacy_text.find("partial_depth") == std::string::npos);
        SessionSnapshot old_format;
        assert(session_from_json(legacy_text, old_format, error));
        assert(old_format.record.moves[1].partial_depth == 0);
        assert(old_format.record.moves[1].root_moves_done == 0);
        assert(old_format.record.moves[1].seldepth == 0);
        assert(old_format.record.moves[1].stopped == "none");
        const std::shared_ptr<GameServer::TrackedGame> rebuilt =
            h.server.open_session(&old_format);
        assert(rebuilt->rebuilt);
        const ReplaySnapshot rebuilt_replay = rebuilt->game->replay_snapshot();
        assert(rebuilt_replay.moves.size() == 2);
        assert(rebuilt_replay.moves[1].depth == 5);
        assert(rebuilt_replay.moves[1].stopped == "none");
        assert(rebuilt_replay.moves[1].partial_depth == 0);

        // 4. The whole takeover path: shelved past the reattach window, rebuilt from
        // disk by the second socket, and the frame it is handed still names layer 6.
        h.server.detach_socket(h.socket.get());
        h.entry->detached_ms = wall_ms() - 30LL * 60LL * 1000LL - 10;
        h.server.on_flush_tick();
        assert(h.server.cold_sessions_.count(token) == 1);

        auto second = std::make_shared<WebSocketSession>(tcp::socket(h.server.ioc_), std::string());
        size_t second_seen = 0;
        h.server.handle_resume(second, json{{"token", token}});
        h.server.handle_request_replay(second);
        for (const std::string& text : drain_frames(h.server, second, second_seen)) {
            const json parsed = json::parse(text, nullptr, false);
            if (!parsed.is_discarded() && parsed.value("type", "") == "replay") {
                replay = parsed;
            }
        }
        std::cout << "  [r121a7] resumed replay ply 2: " << replay["moves"][1].dump()
                  << std::endl;
        assert(replay["moves"][1]["partial_depth"] == 6);
        assert(replay["moves"][1]["root_moves_done"] == 12);
        assert(replay["moves"][1]["root_moves_total"] == 34);
        assert(replay["moves"][1]["seldepth"] == 7);
        assert(replay["moves"][1]["stopped"] == "time_hard");
        assert(replay["moves"][0]["stopped"] == "none");
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_replay_roundtrip_keeps_new_fields passed" << std::endl;
}

// A7's other half: the two budgets a client may name. README:345 fixes the three
// rules - a frame that names a key only changes that key, `soft_time_ms` is kept
// inside the hard deadline it is meant to cut short, and 0 asks for the pre-A7
// behavior (no soft window, node ceiling derived from the time). Neither is
// persisted, so a resume hands back the defaults.
void test_set_config_keeps_the_two_optional_budgets() {
    const auto dir = scratch("budget_a7_sessions");
    {
        Harness h(dir, std::filesystem::path());
        // Two stones, so the session below has something to write: a zero-stone
        // game takes flush_entry's delete-the-residue branch instead.
        assert(h.entry->game->restore_game(playing_restore(2)));
        auto apply = [&](const json& frame, int& soft, int& nodes) {
            h.server.handle_set_config(h.socket, frame);
            soft = h.entry->game->soft_time_ms();
            nodes = h.entry->game->max_nodes();
        };
        int soft = -1;
        int nodes = -1;

        apply(json{{"timeout_ms", 8000}, {"max_depth", 6}}, soft, nodes);
        std::cout << "  [r121a7] no budget named => " << soft << "/" << nodes << std::endl;
        assert(soft == 0);
        assert(nodes == 0);

        // Both named, inside the window: both taken.
        apply(json{{"soft_time_ms", 3000}, {"max_nodes", 50000}}, soft, nodes);
        assert(soft == 3000);
        assert(nodes == 50000);

        // A soft deadline past the hard one asks for nothing the hard one does not
        // already give, so it lands on the deadline instead of beyond it.
        apply(json{{"timeout_ms", 8000}, {"soft_time_ms", 20000}}, soft, nodes);
        std::cout << "  [r121a7] soft 20000 over hard 8000 => " << soft << std::endl;
        assert(soft == 8000);

        // Control: naming one key still leaves the other alone - the same per-key
        // rule the round-88 config halves are held to.
        apply(json{{"max_nodes", 1234}}, soft, nodes);
        assert(soft == 8000);
        assert(nodes == 1234);

        // 0 on either is a request for the derived budget, not a budget of nothing.
        apply(json{{"soft_time_ms", 0}, {"max_nodes", 0}}, soft, nodes);
        assert(soft == 0);
        assert(nodes == 0);

        // A non-positive node count below zero is not a budget either.
        apply(json{{"max_nodes", -5}}, soft, nodes);
        assert(nodes == 0);

        // The two budgets are not part of the session file, unlike the timeout and
        // the depth (README:345): the file says what the game is, not what a client
        // last asked a single think to cost.
        apply(json{{"timeout_ms", 9000}, {"max_depth", 7},
                   {"soft_time_ms", 3000}, {"max_nodes", 50000}},
              soft, nodes);
        assert(soft == 3000);
        assert(nodes == 50000);
        h.server.flush_entry(h.entry);
        const std::string file_text = read_text(h.session_file());
        std::cout << "  [r121a7] session file names soft_time_ms="
                  << (file_text.find("soft_time_ms") != std::string::npos)
                  << ", max_nodes=" << (file_text.find("max_nodes") != std::string::npos)
                  << std::endl;
        assert(file_text.find("soft_time_ms") == std::string::npos);
        assert(file_text.find("max_nodes") == std::string::npos);
        SessionSnapshot on_disk;
        std::string error;
        assert(load_session(dir.string(), h.entry->token, on_disk, error));
        assert(on_disk.timeout_ms == 9000);
        assert(on_disk.max_depth == 7);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_set_config_keeps_the_two_optional_budgets passed" << std::endl;
}


// ── 甲-3 复盘面板：协议侧（设计文档 v2 §6.9，指导书甲-3 的验收三条）────────────
// 一局「读得回来」的记录才有一手可复盘：段 A 对 reason==INVALID 直接返回零候选
// （replay_analyzer.cpp:86），而 snapshot_of 只在 status==GAME_OVER 时才写下胜负，
// 所以这一组用例都从 five_restore 出发——这与 §6.9 第 1 句「一局结束后」是同一件事。
GameRestore analyze_restore() {
    GameRestore restore;
    restore.player_color = BLACK;
    restore.status = GameStatus::GAME_OVER;
    restore.winner = WHITE;
    restore.reason = "five";
    // 双方都差一子成五，黑先走：黑 (9,2)-(9,5)、白 (7,2)-(7,5)。第 8 手黑棋没有收自己的
    // (9,6) 而是走了个安静的 (0,12)，白棋第 9 手 (7,6) 成五——所以段 B 在这一手看到的
    // 正解是 (9,6)（自己成五），失分是「+100000 对 -100000」。第 6、7 手两侧都在收自己的
    // 四，没有「该做而不做」的手。
    const Position cells[10] = {{9, 2}, {7, 2}, {9, 3}, {7, 3}, {9, 4},
                                {7, 4}, {9, 5}, {7, 5}, {0, 12}, {7, 6}};
    // 段 A 的摆幅按 (-score_{i+1}) - score_i 算，两侧都不在将杀带里才有资格响，所以
    // 记录里第 8、9 手的分值写成 5,000 / 5,000：黑棋以为自己是 +5,000，白棋接着也是
    // +5,000（它的视角），摆幅 -10,000 就是 BIG_SWING。开局六手之内按 §6.4 判据 1 剔除。
    const int scores[10] = {100, 120, 150, 200, 300, 600, 1500, 1400, 5000, 5000};
    for (int i = 0; i < 10; ++i) {
        MoveAnalysis analysis;
        analysis.move = Move(cells[i].row, cells[i].col, i % 2 == 0 ? BLACK : WHITE);
        analysis.method = "alphabeta";
        analysis.score = scores[i];
        analysis.depth = 6;
        analysis.nodes = 20000;
        restore.moves.push_back(analysis);
    }
    return restore;
}

void test_analyze_progress_then_exactly_one_result_per_ply() {
    const auto dir = scratch("r148_analyze_sessions");
    Harness h(dir, std::string());
    assert(h.entry->game->restore_game(analyze_restore()));
    size_t seen = 0;

    h.server.handle_analyze_request(h.socket,
                                    json{{"depth", 1}, {"budget", 1000}, {"max_candidates", 1}});

    int progress = 0;
    json result;
    bool got_result = false;
    // 等的是「一条线程跑完一次定预算搜索」，按 §9.2 这一臂只能用墙钟；240 s 的帽
    // 在裸跑时是 40 倍余量，在 memcheck 下也不至于误杀。
    for (int i = 0; i < 2400 && !got_result; ++i) {
        for (const std::string& frame : drain_frames(h.server, h.socket, seen)) {
            const json parsed = json::parse(frame, nullptr, false);
            if (parsed.is_discarded() || !parsed.contains("type")) continue;
            const std::string type = parsed["type"].get<std::string>();
            if (type == "analyze_progress") {
                ++progress;
                assert(parsed["total"] == 2 &&
                       parsed["done"] == progress);
            } else if (type == "analyze_result") {
                got_result = true;
                result = parsed;
            } else {
                std::cout << "  [r148a3] unexpected frame during a review: " << type << std::endl;
                assert(false);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    assert(got_result);
    // 每手一项，与非候选手一起：前端的标记是按下标贴到曲线上的，缺一项就贴错手。
    assert(result["moves"].size() == 10);
    // 进度帧的 total 就是结果帧的 candidates，而 reviewed == 实际复核过的手数。
    assert(result["candidates"] == result["reviewed"]);
    assert(result["reviewed"] == progress);
    assert(result["coeffs_sha"] == coefficients_fingerprint(ThreatCoefficients{}));
    int marked = 0;
    for (const json& item : result["moves"]) {
        assert(item.contains("ply") && item.contains("kind") && item.contains("played"));
        assert(item.contains("best") && item.contains("loss") && item.contains("gap"));
        assert(item.contains("layer"));
        const std::string kind = item["kind"].get<std::string>();
        if (kind != "none") ++marked;
        if (item["ply"] < 6) assert(kind == "none");   // 开局六手之内是剔除区，不贴标记
        if (item["ply"] == 8) {
            // 钉的是「段 B 对快路径的手没话说」这一条（§6.3.1 的根表规则，轮次145 落进
            // replay_analyzer.cpp:216 那句 return）：第 8 手黑棋盘上就有成五的着法，
            // SearchEngine 的快路径直接答，root_scores 是空的，所以判定不许是俗手，
            // layer 也不许是难度层——它俩都只能是「没说过话」的那一个值。
            assert(kind == "none");
            assert(item["layer"] == "unrated");
            assert(item["played"][0] == 0 && item["played"][1] == 12);
        }
        if (kind == "none") assert(item["layer"] == "unrated" || item["layer"] == "easy" ||
                                    item["layer"] == "normal" || item["layer"] == "hard");
    }
    assert(marked == 0);
    assert(result["cards"] == 0);
    std::cout << "test_analyze_progress_then_exactly_one_result_per_ply passed (" << progress
              << " progress frames, 1 result, " << result["moves"].size() << " plies, "
              << result["candidates"] << " candidates, " << marked << " marked)" << std::endl;

    // 跑完的会话还能继续下：复盘不留任何状态在 entry 上。
    h.server.handle_analyze_request(h.socket, json{{"depth", 1}, {"budget", 1000}});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    h.server.handle_undo(h.socket);
    assert(h.entry->game->status() == GameStatus::PLAYING);
    h.server.stop_analyze(*h.entry);
}

void test_analyze_is_refused_while_the_ai_is_thinking() {
    const auto dir = scratch("r148_analyze_busy");
    Harness h(dir, std::string());
    // 三子的 quiet 局面 + 长预算：AI 那一边一定还在想（没有五、没有必挡，快路径不响）。
    h.entry->game->set_config(120000, 20);
    assert(h.entry->game->restore_game(playing_restore(2)));
    size_t seen = 0;
    assert(h.server.entry_locked(h.socket.get()) != nullptr);
    h.server.handle_make_move(h.socket, json{{"row", 5}, {"col", 5}});
    const bool busy = h.entry->game->is_ai_thinking();
    std::cout << "  [r148a3] busy=" << (busy ? "yes" : "no") << std::endl;
    assert(busy);

    // 思考中的复盘请求：回一帧 error，且不点分析线程——一条 analyze_* 帧都不许有。
    h.server.handle_analyze_request(h.socket, json{{"depth", 2}, {"budget", 1000}});
    const std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
    int analyze_frames = 0;
    int errors = 0;
    std::string kinds;
    for (const std::string& frame : frames) {
        const json parsed = json::parse(frame, nullptr, false);
        if (parsed.is_discarded() || !parsed.contains("type")) continue;
        const std::string type = parsed["type"].get<std::string>();
        kinds += (kinds.empty() ? "" : ",") + type;
        if (type.rfind("analyze_", 0) == 0) ++analyze_frames;
        if (type == "error") ++errors;
    }
    std::cout << "  [r148a3] frames while busy: " << kinds << std::endl;
    assert(errors == 1);
    assert(analyze_frames == 0);
    h.entry->game->halt_search();
    h.server.stop_analyze(*h.entry);
    drain_frames(h.server, h.socket, seen);
}

void test_analyze_refuses_the_positions_it_cannot_rebuild_and_the_games_it_cannot_read() {
    const auto dir = scratch("r148_analyze_empty");
    {
        // 一子未落：没有可复盘的手，回 error，不发 analyze_*。
        Harness h(dir, std::string());
        size_t seen = 0;
        h.server.handle_analyze_request(h.socket, json{{"depth", 2}, {"budget", 1000}});
        const std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        const json reply = json::parse(frames[0]);
        assert(reply["type"] == "error");
        assert(h.entry->game->status() == GameStatus::WAITING);
    }
    {
        // 摆棋开的局：base 子不在手数里，段 B 从空盘一手一手摆不回这一局（README:575
        // 同一条：这种局只活在会话文件里，不进训练流）。这里照同一口径拒绝。
        Harness h(dir, std::string());
        GameRestore restored = analyze_restore();
        restored.base = {Move(3, 3, BLACK), Move(11, 11, WHITE)};
        restored.moves.clear();
        const Position tail[2] = {{4, 4}, {5, 5}};
        for (int i = 0; i < 2; ++i) {
            MoveAnalysis analysis;
            analysis.move = Move(tail[i].row, tail[i].col, i % 2 == 0 ? BLACK : WHITE);
            analysis.method = "alphabeta";
            analysis.score = 8000;
            analysis.depth = 6;
            restored.moves.push_back(analysis);
        }
        restored.status = GameStatus::GAME_OVER;
        assert(h.entry->game->restore_game(restored));
        size_t seen = 0;
        h.server.handle_analyze_request(h.socket, json{{"depth", 2}, {"budget", 1000}});
        const std::vector<std::string> frames = drain_frames(h.server, h.socket, seen);
        assert(frames.size() == 1);
        const json reply = json::parse(frames[0]);
        assert(reply["type"] == "error");
        assert(reply["message"].get<std::string>().find("摆棋") != std::string::npos);
    }
    std::cout << "test_analyze_refuses_the_positions_it_cannot_rebuild_and_the_games_it_cannot_read"
              << " passed (empty game and 摆棋 game both answer one error frame, no analyze_*)"
              << std::endl;
}
// 断言带一句「这条钉的是哪句话」：宏在调用点展开，所以 assert 报的 file:line 就是这一臂自己的行，
// 而消息是人话（既有用例只 assert 条件；新加的这几条要能被 mutation 臂按行指认）。
#define CHECK_MESSAGE(cond, msg) assert((msg) && (cond))
#define CHECK_EQ(a, b) assert((((a) == (b)) ? 1 : 0) && #a " == " #b)

// ── 戊-2 参数单一真源 + 网页控制面：协议侧（v2 §5、§4.5.2；指导书 §4 戊-2 验收三条）────
//
// 三条口径各有一臂：清单里每条都必须带满面板要的那几个字段（少一个前端就得回去硬编码）；
// `config_set` 越界是**拒绝并点名**，不是截断（旧的那条 `set_config` 保留 clamp，两边互不影响）；
// 预览跑固定针臂、**不写任何状态**，所以同一份参数预览两次必须给出同样的节点/深度/着法。

namespace {

std::vector<json> config_frames(GameServer& server, std::shared_ptr<WebSocketSession> socket,
                                size_t& seen) {
    std::vector<json> out;
    for (const std::string& frame : drain_frames(server, socket, seen)) {
        json parsed = json::parse(frame, nullptr, false);
        if (!parsed.is_discarded() && parsed.contains("type")) out.push_back(parsed);
    }
    return out;
}

const json& only_frame(const std::vector<json>& frames, const std::string& type) {
    static const json empty;
    for (const json& frame : frames) {
        if (frame["type"] == type) return frame;
    }
    return empty;
}

}  // namespace

void test_config_schema_carries_the_fields_the_panel_needs() {
    const auto dir = scratch("r165_config_schema");
    Harness h(dir, std::string());
    size_t seen = 0;
    h.server.handle_config_frame(h.socket, json{{"type", "config_schema"}});
    const std::vector<json> frames = config_frames(h.server, h.socket, seen);
    CHECK_EQ(frames.size(), 1u);
    const json& schema = only_frame(frames, "config_schema");
    assert(!schema.is_null());
    assert(schema.contains("params") && schema["params"].is_array());
    const json& params = schema["params"];
    assert(params.size() >= 8u);

    const char* const kRequired[] = {"key",  "group",  "type",     "min",    "max",
                                     "default", "step", "when_live", "unit"};
    int live = 0;
    int next_think = 0;
    int restart = 0;
    for (const json& entry : params) {
        for (const char* field : kRequired) {
            // 「每条都有那 9 个字段」这一条不是排版要求：前端就是靠它渲染，缺一个就得回去硬编码。
            const std::string missing =
                std::string(entry.value("key", std::string("?"))) + " 少了字段 " + field;
            CHECK_MESSAGE(entry.contains(field), missing.c_str());
        }
        CHECK_MESSAGE(entry.contains("desc_plain") && entry.contains("effect_note") &&
                                entry.contains("suggest"),
                            "白话/效果/建议档是面板的三行说明，缺一条就等于把解释工作推回前端");
        const std::string when = entry["when_live"].get<std::string>();
        if (when == "live") ++live;
        else if (when == "next_think") ++next_think;
        else if (when == "restart") ++restart;
        else assert(false);
        const std::string type = entry["type"].get<std::string>();
        if (type == "int" || type == "double") {
            const double lo = entry["min"].get<double>();
            const double hi = entry["max"].get<double>();
            const double def = entry["default"].get<double>();
            CHECK_MESSAGE(lo < hi, "区间是空的");
            CHECK_MESSAGE(def >= lo && def <= hi, "默认值落在自己的区间之外");
        }
    }
    // 三类 when_live 各至少一例（指导书 戊-2 验收第 1 条的最后一项）。
    CHECK_MESSAGE(live > 0 && next_think > 0 && restart > 0,
                        "三类 when_live 必须都能出现在清单里，否则面板学不到「改了要重启」这件事");

    // 单一真源的反证：默认值必须是代码此刻真的在用的那个数，不是文档里的数。
    const ParamSpec* depth = ParamRegistry::find("max_depth");
    assert(depth != nullptr);
    CHECK_MESSAGE(h.entry->game->max_depth() == static_cast<int>(depth->default_value),
                        "面板缺省与服务端实际缺省不一致 ⇒ 清册只是装饰");
    CHECK_MESSAGE(h.server.analyze_depth_default_ ==
                            static_cast<int>(ParamRegistry::find("analyze_depth")->default_value),
                        "复盘缺省与清册漂移了");
    std::cout << "  [r165] schema rows=" << params.size() << " live/next/restart=" << live << "/"
              << next_think << "/" << restart << std::endl;
    std::cout << "test_config_schema_carries_the_fields_the_panel_needs passed" << std::endl;
}

void test_config_set_refuses_out_of_range_without_touching_the_session() {
    const auto dir = scratch("r165_config_reject");
    Harness h(dir, std::string());
    assert(h.entry->game->restore_game(playing_restore(2)));
    size_t seen = 0;
    const int depth_before = h.entry->game->max_depth();
    const int timeout_before = h.entry->game->timeout_ms();

    auto reject_for = [&](const json& frame, const std::string& expect_key) {
        h.server.handle_config_frame(h.socket, frame);
        const std::vector<json> frames = config_frames(h.server, h.socket, seen);
        const json& reject = only_frame(frames, "config_reject");
        CHECK_MESSAGE(!reject.is_null(), "越界必须回 config_reject，不能沉默");
        CHECK_MESSAGE(reject["key"] == expect_key,
                            "拒绝要点名是哪一条参数");
        CHECK_MESSAGE(reject.contains("why") && !reject["why"].get<std::string>().empty(),
                            "拒绝要带一句能给人看的话");
        CHECK_MESSAGE(only_frame(frames, "config_ack").is_null(),
                            "同一条不能既拒绝又确认");
        return reject["why"].get<std::string>();
    };

    // 超过上限：话术里带着区间，而且没有把值截到 20 再用（指导书 戊-2 注意 3）。
    const std::string over = reject_for(json{{"type", "config_set"}, {"key", "max_depth"},
                                             {"value", 99}},
                                        "max_depth");
    std::cout << "  [r165] max_depth 99 => " << over << std::endl;
    CHECK_MESSAGE(over.find("[2, 20]") != std::string::npos, "拒绝话术要带区间");
    // 小数、负数、字符串、不存在的键、缺 value：五样都被拒，且都不改会话。
    const std::string fractional = reject_for(json{{"type", "config_set"}, {"key", "max_depth"},
                                                   {"value", 6.5}}, "max_depth");
    std::cout << "  [r165] max_depth 6.5 => " << fractional << std::endl;
    reject_for(json{{"type", "config_set"}, {"key", "timeout_ms"}, {"value", 50}}, "timeout_ms");
    reject_for(json{{"type", "config_set"}, {"key", "max_nodes"}, {"value", "many"}}, "max_nodes");
    const std::string unknown = reject_for(json{{"type", "config_set"}, {"key", "no_such_knob"},
                                                {"value", 1}}, "no_such_knob");
    CHECK_MESSAGE(unknown.find("没有这条参数") != std::string::npos,
                        "「这条不存在」不能混报成「越界」");
    reject_for(json{{"type", "config_set"}, {"key", "max_depth"}}, "max_depth");
    // 要重启才生效的那一条也是拒绝，而不是「收了再说改了」：面板据此禁用。
    const std::string reboot = reject_for(json{{"type", "config_set"}, {"key", "shared_tt_bits"},
                                               {"value", 20}}, "shared_tt_bits");
    CHECK_MESSAGE(reboot.find("重启") != std::string::npos, "restart 那一档要写明要重启");

    CHECK_MESSAGE(h.entry->game->max_depth() == depth_before, "被拒的值不许改变会话");
    CHECK_MESSAGE(h.entry->game->timeout_ms() == timeout_before, "被拒的值不许改变会话");
    std::cout << "test_config_set_refuses_out_of_range_without_touching_the_session passed"
              << std::endl;
}

void test_config_set_applies_each_live_class_and_acks_it() {
    const auto dir = scratch("r165_config_apply");
    Harness h(dir, std::string());
    assert(h.entry->game->restore_game(playing_restore(2)));
    size_t seen = 0;

    auto apply = [&](const std::string& key, const json& value) {
        h.server.handle_config_frame(h.socket,
                                     json{{"type", "config_set"}, {"key", key}, {"value", value}});
        const std::vector<json> frames = config_frames(h.server, h.socket, seen);
        const json& ack = only_frame(frames, "config_ack");
        CHECK_MESSAGE(!ack.is_null(), "合法的值要回 config_ack");
        CHECK_MESSAGE(ack["key"] == key, "ack 要指认改的是哪一条");
        CHECK_MESSAGE(ack["value"] == value, "ack 要回显生效的值");
        CHECK_MESSAGE(only_frame(frames, "config_reject").is_null(), "不该同时被拒");
        return ack["when_live"].get<std::string>();
    };

    CHECK_MESSAGE(apply("timeout_ms", 5000) == "live", "timeout_ms 是 live 那一档");
    CHECK_MESSAGE(h.entry->game->timeout_ms() == 5000, "ack 说生效就得真生效");
    CHECK_MESSAGE(apply("max_depth", 12) == "next_think", "max_depth 是 next_think 那一档");
    CHECK_MESSAGE(h.entry->game->max_depth() == 12, "ack 说生效就得真生效");
    // 改一条不许顺手把另一条打回缺省（与旧 `set_config` 同一条 per-key 规则）。
    CHECK_MESSAGE(h.entry->game->timeout_ms() == 5000, "改 max_depth 不许动 timeout_ms");
    apply("soft_time_ms", 3000);
    CHECK_MESSAGE(h.entry->game->soft_time_ms() == 3000, "soft_time_ms 要落地");
    apply("max_nodes", 60000);
    CHECK_MESSAGE(h.entry->game->max_nodes() == 60000, "max_nodes 要落地");
    CHECK_MESSAGE(h.entry->game->timeout_ms() == 5000, "后两条不许把 timeout 打回缺省");
    // 复盘那一组落在服务端状态上：下一次 analyze 没点名 depth 时读的就是这个数。
    apply("analyze_depth", 10);
    CHECK_MESSAGE(h.server.analyze_depth_default_ == 10, "复盘缺重要能被改");
    std::cout << "test_config_set_applies_each_live_class_and_acks_it passed" << std::endl;
}

void test_config_preview_is_repeatable_and_commits_nothing() {
    const auto dir = scratch("r165_preview");
    Harness h(dir, std::string());
    assert(h.entry->game->restore_game(playing_restore(2)));
    size_t seen = 0;
    const int depth_before = h.entry->game->max_depth();
    const int timeout_before = h.entry->game->timeout_ms();

    auto preview = [&](const std::string& key, const json& value) {
        h.server.handle_config_frame(h.socket, json{{"type", "config_preview"},
                                                    {"key", key}, {"value", value}});
        const std::vector<json> frames = config_frames(h.server, h.socket, seen);
        const json& out = only_frame(frames, "config_preview");
        CHECK_MESSAGE(!out.is_null(), "预览要回一帧 config_preview");
        return out;
    };

    const json first = preview("max_depth", 4);
    CHECK_MESSAGE(first["committed"] == false, "预览不许写进任何状态");
    CHECK_MESSAGE(first["previewable"] == true, "深度是针臂能表达的");
    CHECK_EQ(first["arms"].size(), 5u);
    const json second = preview("max_depth", 4);
    // 两次预览逐字段对得上：针臂按节点定价，所以时间不参与（指导书 §9.2）。
    CHECK_MESSAGE(first["checksum"] == second["checksum"], "同一份参数的两次预览校验和不同");
    CHECK_MESSAGE(first["total_nodes"] == second["total_nodes"], "两次预览的节点数不同");
    for (size_t i = 0; i < first["arms"].size(); ++i) {
        const json& a = first["arms"][i];
        const json& b = second["arms"][i];
        CHECK_MESSAGE(a["depth"] == b["depth"] && a["nodes"] == b["nodes"] &&
                                a["move"] == b["move"] && a["score"] == b["score"],
                            "两次预览的某一臂读数不同");
    }
    // 换一档参数必须真的换个读数，否则这台仪器什么都没量。
    const json deeper = preview("max_depth", 8);
    CHECK_MESSAGE(deeper["checksum"] != first["checksum"] ||
                            deeper["total_nodes"] != first["total_nodes"],
                        "预览对不同的深度给出同一份读数 ⇒ 针臂没在跑");
    std::cout << "  [r165] preview nodes d4=" << first["total_nodes"]
              << " d14=" << deeper["total_nodes"] << std::endl;
    // 针臂表达不了的参数直说「不可预览」并给理由，而不是演一遍像样的读数。
    const json soft = preview("soft_time_ms", 2000);
    CHECK_MESSAGE(soft["previewable"] == false, "软时限不该被说成可预览");
    CHECK_MESSAGE(!soft["why_not"].get<std::string>().empty(), "不可预览要给理由");
    const json ponder = preview("ponder_percent", 50);
    CHECK_MESSAGE(ponder["previewable"] == false, "要重启的那一条不该有预览读数");
    CHECK_MESSAGE(h.entry->game->max_depth() == depth_before &&
                            h.entry->game->timeout_ms() == timeout_before,
                        "预览改动了会话状态");
    std::cout << "test_config_preview_is_repeatable_and_commits_nothing passed" << std::endl;
}

void test_config_preview_is_refused_while_the_ai_is_thinking() {
    const auto dir = scratch("r165_preview_busy");
    Harness h(dir, std::string());
    h.entry->game->set_config(120000, 20);
    assert(h.entry->game->restore_game(playing_restore(2)));
    size_t seen = 0;
    h.server.handle_make_move(h.socket, json{{"row", 5}, {"col", 5}});
    CHECK_MESSAGE(h.entry->game->is_ai_thinking(), "这一臂的前提：AI 正在想");

    h.server.handle_config_frame(h.socket,
                                 json{{"type", "config_preview"}, {"key", "max_depth"},
                                      {"value", 12}});
    const std::vector<json> frames = config_frames(h.server, h.socket, seen);
    CHECK_MESSAGE(only_frame(frames, "config_preview").is_null(),
                        "玩家那手正在思考时不许跑预览");
    CHECK_MESSAGE(!only_frame(frames, "error").is_null(), "要回一帧 error 说明为什么不做");
    h.server.handle_stop_thinking(h.socket);
    std::cout << "test_config_preview_is_refused_while_the_ai_is_thinking passed" << std::endl;
}

void test_preview_arms_are_the_pinned_poses() {
    const std::vector<std::pair<Board, int>>& arms = preview_arms();
    CHECK_EQ(arms.size(), 5u);
    // 臂序就是清单里写的那个：quiet×{黑,白}、scattered×{黑,白}、pos24×黑。
    CHECK_MESSAGE(arms[0].second == BLACK && arms[1].second == BLACK &&
                            arms[2].second == WHITE && arms[3].second == WHITE &&
                            arms[4].second == BLACK,
                        "针臂的执子顺序变了，读数就不再多可比重");
    const Board pinned = pinned_pos24();
    for (int row = 0; row < BOARD_SIZE; ++row) {
        for (int col = 0; col < BOARD_SIZE; ++col) {
            // 生产码不引测试头（那等于把夹具链进服务端），所以两边各有一份定义，靠这条钉住。
            CHECK_MESSAGE(arms[4].first.get(row, col) == pinned.get(row, col),
                                "预览的 pos24 与 tests/pinned_positions.h 的那一份不是同一摆法");
        }
    }
    CHECK_EQ(arms[4].first.move_count(), 24);
    CHECK_MESSAGE(arms[0].first.move_count() < arms[4].first.move_count() &&
                            arms[2].first.move_count() > 0,
                        "针臂的三副盘面空了");
    std::cout << "test_preview_arms_are_the_pinned_poses passed" << std::endl;
}


// ── 悔棋重下同一手要不要重新思考（用户提出的预期：不应该从头重想）────────────────
// 三次读数的形状是同一件事：答案不许变，成本要塌。塌下来的那部分全部来自置换表跨悔棋存活
// ——`GameManager::undo` 不清表，只 `advance_generation()`（那是**替换顺序**的时钟，
// 不是有效性：`transposition.cpp` 的 probe 不参与代际判断）。所以第二次思考仍然进
// `engine_.search`，只是它几乎全程在读第一次写下的记录。
//
// 节点数在这里是**量**：`set_config(120000, 4, -1, 40000)` 给一个远在水位之上的墙钟 +
// 一个不参与判定也不会先到的节点帽，深度 4 自然跑完（4312 / 10375 都远小于 40000），
// 于是同一棵树在空机与满载机上给出同一个数（指导书 §9.2）。
namespace {

// 等 make_move 自己起的那条 worker 跑完：join 而不是轮询——memcheck 下墙钟会放大
// ~20 倍，任何「等 N 毫秒」的写法都会变成假失败，而 join 没有时限。
// 轮次193 #106：worker 想完只把结论停进原子槽再 post 一句；盘要由属主线程落。测试里没有
// 在跑的 io 线程，join 完之后由这一行充当属主（无头用法没装通知 ⇒ 槽本来就是空的，空操作）。
void join_think(GameManager& game) {
    if (game.search_thread_ && game.search_thread_->joinable()) game.search_thread_->join();
    game.commit_pending();
    assert(!game.is_ai_thinking());
}

struct Think {
    int row = -1, col = 0, score = 0, depth = 0, nodes = 0, tt_hits = 0;
    std::string method;
};

Think last_ai_think(const GameManager& game) {
    Think t;
    for (const MoveAnalysis& a : game.analyses_) {
        if (a.method == "human") continue;
        t.row = a.move.pos.row; t.col = a.move.pos.col; t.score = a.score;
        t.depth = a.depth; t.nodes = a.nodes; t.tt_hits = a.tt_hits; t.method = a.method;
    }
    return t;
}

bool same_answer(const Think& a, const Think& b) {
    return a.row == b.row && a.col == b.col && a.score == b.score && a.depth == b.depth &&
           a.method == b.method;
}

// 一个新会话：人执黑，深度 4，墙钟远在水位之上，节点帽只是护栏。
void arm_session(GameManager& game) {
    game.reset();
    game.set_move_callback([](const Move&, int, int) {});
    game.set_game_over_callback([](int, const std::string&) {});
    game.set_player_color(BLACK);
    game.set_config(120000, 4, -1, 40000);
}

}  // namespace

// 一：悔棋重下同一手 ⇒ 答案逐字相同、成本塌到零头。这是新 behaviour。
void test_undo_then_the_same_stone_reuses_the_table_across_the_takeback() {
    // (a) 一手就悔：空盘 → 人 (7,7) → AI → 悔 → 再人 (7,7) → AI。
    GameManager once; arm_session(once);
    assert(once.make_move(7, 7)); join_think(once);
    const Think first = last_ai_think(once);
    CHECK_EQ(first.nodes, 4312);            // 出厂针：该位置第一次思考的节点数
    CHECK_EQ(first.tt_hits, 299);
    CHECK_MESSAGE(first.method == "alphabeta", "第一次思考走的是分层搜索，不是快路");

    int moved = 0;
    GameStatus status = GameStatus::PLAYING;
    assert(once.undo(moved, status));
    // 注意 `undo` 的出参是**退回之后盘上剩几子**，不是「退掉几子」——空盘开局悔一手，
    // 剩 0。把它读成「退掉的个数」会永远判错，所以这里按契约钉住值而不是名字。
    CHECK_EQ(moved, 0);
    CHECK_EQ(status, GameStatus::WAITING);   // 退到空盘 ⇒ 这一局回到「还没开」
    assert(once.make_move(7, 7)); join_think(once);
    const Think replayed = last_ai_think(once);
    std::cout << "  [r167] 一手后悔棋：nodes " << first.nodes << " -> " << replayed.nodes
              << ", tt_hits " << first.tt_hits << " -> " << replayed.tt_hits << std::endl;
    CHECK_MESSAGE(same_answer(first, replayed), "悔棋重下同一手不许换个答案");
    CHECK_EQ(replayed.nodes, 1268);         // 同一位置的第二次：读表，不重想
    CHECK_MESSAGE(replayed.nodes < first.nodes / 3, "重下同一手要付不到第一次的三分之一");

    // (b) 真实用法：下了两回合再悔（表已经被两次思考灌满）。
    GameManager fresh_session; arm_session(fresh_session);
    assert(fresh_session.make_move(7, 7)); join_think(fresh_session);
    assert(fresh_session.make_move(8, 8)); join_think(fresh_session);
    const Think cold_at_pair = last_ai_think(fresh_session);
    CHECK_EQ(cold_at_pair.nodes, 10375);

    GameManager warm_session; arm_session(warm_session);
    assert(warm_session.make_move(7, 7)); join_think(warm_session);
    assert(warm_session.make_move(8, 8)); join_think(warm_session);
    int moved2 = 0;
    GameStatus status2 = GameStatus::PLAYING;
    assert(warm_session.undo(moved2, status2));
    assert(warm_session.make_move(8, 8)); join_think(warm_session);
    const Think replayed_at_pair = last_ai_think(warm_session);
    std::cout << "  [r167] 两回合后悔棋：nodes " << cold_at_pair.nodes << " -> "
              << replayed_at_pair.nodes << "（省 "
              << (100.0 - 100.0 * replayed_at_pair.nodes / cold_at_pair.nodes) << " %）"
              << std::endl;
    CHECK_MESSAGE(same_answer(cold_at_pair, replayed_at_pair), "悔棋重下同一手不许换个答案");
    CHECK_EQ(replayed_at_pair.nodes, 144);  // 1.4 % —— 实际上就是「没有重想」
    std::cout << "test_undo_then_the_same_stone_reuses_the_table_across_the_takeback passed"
              << std::endl;
}

// 二：反证——同一张热表，悔棋之后**换一手**下，成本必须回到正常量级。
// 没有这一条，上面那个「144」也可以只是「搜索根本没跑 / 直接吐了个缓存着法」。
void test_a_different_stone_after_the_takeback_pays_a_fresh_think_again() {
    GameManager game; arm_session(game);
    assert(game.make_move(7, 7)); join_think(game);
    assert(game.make_move(8, 8)); join_think(game);
    CHECK_EQ(last_ai_think(game).nodes, 10375);   // 位置 P 的第一次：全额
    int moved0 = 0;
    GameStatus status0 = GameStatus::PLAYING;
    assert(game.undo(moved0, status0));
    assert(game.make_move(8, 8)); join_think(game);
    const Think warm_same = last_ai_think(game);   // 位置 P 的第二次：读表
    CHECK_EQ(warm_same.nodes, 144);

    int moved = 0;
    GameStatus status = GameStatus::PLAYING;
    assert(game.undo(moved, status));
    assert(game.make_move(0, 0)); join_think(game);
    const Think other = last_ai_think(game);
    std::cout << "  [r167] 悔棋后换一手：(8,8) 重下 " << warm_same.nodes << " vs 换到 (0,0) "
              << other.nodes << std::endl;
    // 表一样热（前两手的思考完全一样），位置一变就要重新证 ⇒ 省法确实来自位置相同。
    CHECK_MESSAGE(other.nodes > warm_same.nodes * 10,
                        "换个位置还这么便宜 ⇒ 省法不是位置复用");
    CHECK_EQ(other.nodes, 7167);            // 同一张热表，换个位置就是正常量级
    std::cout << "test_a_different_stone_after_the_takeback_pays_a_fresh_think_again passed"
              << std::endl;
}

// 三：旧 behaviour 不变——悔棋仍然精确退回原位、仍然不并发，而且反复悔/下不抖。
void test_the_takeback_lands_on_the_same_position_and_a_repeat_stays_stable() {
    GameManager game; arm_session(game);
    assert(game.make_move(7, 7)); join_think(game);
    const unsigned long long at_humans_hand = game.board_.zobrist_hash();
    CHECK_MESSAGE(at_humans_hand != 0,
                        "拿空盘那个恒 0 的哈希来比相等，等于什么都没证（本轮踩过）");
    CHECK_MESSAGE(!game.is_ai_thinking(), "人在应手时不该有 worker 还在写棋盘");

    assert(game.make_move(8, 8)); join_think(game);
    int moved = 0;
    GameStatus status = GameStatus::PLAYING;
    assert(game.undo(moved, status));
    CHECK_EQ(game.board_.zobrist_hash(), at_humans_hand);   // 精确退回，不是「大致退回」
    CHECK_EQ(moved, 2);
    CHECK_MESSAGE(!game.is_ai_thinking(), "悔棋之后不许留一条还在想的 worker");

    // 同一手再反复悔/反复下三次：读数必须逐字相同。进循环之前该位置已经想过两次，
    // 所以三次都是 144 —— 反复悔棋不会把表挤空，也不会一次比一次贵。
    std::vector<int> reps;
    for (int round = 0; round < 3; ++round) {
        assert(game.make_move(8, 8)); join_think(game);
        reps.push_back(last_ai_think(game).nodes);
        int m = 0;
        GameStatus s = GameStatus::PLAYING;
        assert(game.undo(m, s));
    }
    std::cout << "  [r167] 同一位置连想三次：";
    for (const int n : reps) std::cout << n << " ";
    std::cout << std::endl;
    CHECK_EQ(reps.size(), 3u);
    CHECK_EQ(reps[1], reps[2]);            // 到表复用上限之后不再抖
    CHECK_EQ(reps[2], 144);
    CHECK_MESSAGE(reps[0] >= reps[1], "第二次不许比第一次更贵（表被挤掉了就是回归）");
    std::cout << "test_the_takeback_lands_on_the_same_position_and_a_repeat_stays_stable passed"
              << std::endl;
}


// 轮次184 #81 那张「尾窗存手」表在轮次193 #106 里删掉了，连同它的用例：ai_move 与标志交回
// 现在同在一个 io 任务里（`GameManager::commit_pending`），一收一放之间没有第二个 handler 能
// 插进来 —— 尾巴不存在，所以「存手」和「取件单」都没有了存在的形状。轮次178 量到的 4/3326
// 吞子窗口是**结构性消失**，下面第三条直接把「看到子就立刻回一手」钉住，不再靠撑宽尾巴。
//
// #106 搬家的三面各一条：
//   ① 喊话那一刻盘还没动，提交段（move_callback）跑在喊话者之外的线程上；
//   ② 属主决定不要这一手时（`halt_search`），槽里的结论被扔掉、标志交回，迟到的提交是空操作；
//   ③ 服务端那条路上，读到 ai_move 的人一定读到已经交回的标志 ⇒ 回手不会被吞。
void test_the_ai_commit_runs_on_the_owner_thread() {
    GameManager game;
    game.set_player_color(BLACK);
    game.set_config(120000, 4, -1, 40000);

    std::atomic<int> notified{0};
    std::atomic<std::thread::id> notifier{std::thread::id()};
    game.set_owner_notify([&notified, &notifier] {
        notifier.store(std::this_thread::get_id());
        notified.fetch_add(1);
    });
    std::atomic<std::thread::id> committed{std::thread::id()};
    std::atomic<int> commit_calls{0};
    game.set_move_callback([&committed, &commit_calls](const Move&, int, int) {
        committed.store(std::this_thread::get_id());
        commit_calls.fetch_add(1);
    });
    game.set_game_over_callback([](int, const std::string&) {});

    const std::thread::id owner = std::this_thread::get_id();
    assert(game.make_move(7, 7));       // 人落一子；make_move 自己把 AI 那一手点起来
    for (int spin = 0; spin < 12000 && notified.load() == 0; ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // 前提直接看，不拿墙钟当代理：worker 想完了，把结论停进了槽里。
    CHECK_MESSAGE(notified.load() == 1, "worker 想完必须喊一声（而且只喊一次）");
    CHECK_MESSAGE(notifier.load() != owner, "前提：喊话的是 worker，不是属主线程");
    CHECK_MESSAGE(game.board().move_count() == 1, "喊话那一刻 AI 的子还不许落下");
    CHECK_MESSAGE(commit_calls.load() == 0, "喊话那一刻不该已经有提交段跑过");
    CHECK_MESSAGE(game.is_ai_thinking(), "标志要等属主提交才交回 —— worker 不再自己收尾");

    game.commit_pending();              // 测试在替那条 io 线程做事
    CHECK_MESSAGE(commit_calls.load() == 1, "提交段要跑，而且只跑一次");
    CHECK_MESSAGE(committed.load() == owner, "提交段（move_callback）必须在属主线程跑");
    CHECK_MESSAGE(game.board().move_count() == 2, "属主提交之后 AI 那子才上盘");
    CHECK_MESSAGE(!game.is_ai_thinking(), "提交完标志要交回");
    // 再提交一次什么都不许发生。注意这条断言**不单独钉 exchange**：把槽换成
    // 「load + 拷贝、槽不清」的突变体（M4a）也照样绿，因为第二份结论会撞上
    // `apply_search_result` 里的 `current_turn() == ai_color_` —— 轮到人的时候落子
    // 直接被挡回。exchange 那一项的看门人是所有权语义（memcheck），不是这条。
    game.commit_pending();
    CHECK_MESSAGE(commit_calls.load() == 1 && game.board().move_count() == 2,
                  "再提交一次什么都不许发生（槽已空；即便重放也被人那一手的轮次挡住）");
    game.halt_search();
    std::cout << "  [r193] 提交段线程 = 属主（与喊话者不同）；再提交一次不重复提交"
              << std::endl;
    std::cout << "test_the_ai_commit_runs_on_the_owner_thread passed" << std::endl;
}

void test_a_parked_result_is_dropped_when_thinking_is_stopped() {
    GameManager game;
    game.set_player_color(BLACK);
    game.set_config(120000, 4, -1, 40000);

    std::atomic<int> notified{0};
    game.set_owner_notify([&notified] { notified.fetch_add(1); });
    std::atomic<int> commit_calls{0};
    game.set_move_callback([&commit_calls](const Move&, int, int) { commit_calls.fetch_add(1); });
    game.set_game_over_callback([](int, const std::string&) {});

    assert(game.make_move(7, 7));
    for (int spin = 0; spin < 12000 && notified.load() == 0; ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_MESSAGE(notified.load() == 1 && game.is_ai_thinking(),
                  "前提：结论已停在槽里，等属主来取");
    const int before = game.board().move_count();

    // 认输 / 悔棋 / 断开走的那条路（#105 的 stale 判据是另一头）：这一手不要了。
    game.halt_search();
    CHECK_MESSAGE(!game.is_ai_thinking(),
                  "停止之后标志必须交回 —— worker 停在 post 那一步，不再自己清它");
    CHECK_MESSAGE(game.board().move_count() == before && commit_calls.load() == 0,
                  "被丢弃的结论不许落子、也不许回调");
    // 通知已经排进队列但没有属主去取：迟到的提交是空操作（槽早被 halt_search 换走了）。
    game.commit_pending();
    CHECK_MESSAGE(game.board().move_count() == before && commit_calls.load() == 0 &&
                          !game.is_ai_thinking(),
                  "迟到的提交不许把被丢弃的那一手落回来");
    std::cout << "  [r193] 停在槽里的一手被 halt_search 丢掉：盘 " << before << " 子没变、"
              << "回调 0 次、迟到的提交空操作" << std::endl;
    std::cout << "test_a_parked_result_is_dropped_when_thinking_is_stopped passed" << std::endl;
}

void test_the_reply_after_ai_move_is_never_swallowed() {
    const auto dir = scratch("r193_fast_reply");
    Harness h(dir, std::string());
    GameManager& game = *h.entry->game;
    game.set_config(120000, 4, -1, 40000);   // 按节点帽定价：一手上界可复现
    size_t seen = 0;

    h.server.handle_make_move(h.socket, json{{"row", 7}, {"col", 7}});
    bool saw_ai_move = false;
    for (int spin = 0; spin < 12000 && !saw_ai_move; ++spin) {
        for (const std::string& frame : drain_frames(h.server, h.socket, seen)) {
            const json parsed = json::parse(frame, nullptr, false);
            if (!parsed.is_discarded() && parsed.value("type", "") == "ai_move") {
                saw_ai_move = true;
            }
        }
        if (!saw_ai_move) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_MESSAGE(saw_ai_move, "前提：AI 那子要真的交付");
    // 这条是结构性的：帧离开 socket 与标志交回在同一段提交里，所以读到帧的人一定读到已交回。
    CHECK_MESSAGE(!game.is_ai_thinking(),
                  "ai_move 已在手上时标志必须已经交回（这一段没有第二条尾巴）");

    int row = -1, col = -1;
    for (int r = 0; r < BOARD_SIZE && row < 0; ++r) {
        for (int c = 0; c < BOARD_SIZE && row < 0; ++c) {
            if (game.board().is_empty(r, c)) { row = r; col = c; }
        }
    }
    assert(row >= 0);
    // 玩家看到子就立刻回一手 —— 轮次178 量到的尾巴里，这一手会被 `Invalid move` 吞掉。
    h.server.handle_make_move(h.socket, json{{"row", row}, {"col", col}});
    int errors = 0;
    int stones = 0;
    for (const std::string& frame : drain_frames(h.server, h.socket, seen)) {
        const json parsed = json::parse(frame, nullptr, false);
        if (parsed.is_discarded()) continue;
        const std::string type = parsed.value("type", "");
        if (type == "error") ++errors;
        if (type == "move_made") ++stones;
    }
    CHECK_MESSAGE(errors == 0, "看到 ai_move 之后的回手不许被吞成 Invalid move");
    CHECK_MESSAGE(stones == 1, "回手要落，而且只落一次");
    CHECK_MESSAGE(!game.board().is_empty(row, col), "回手真的在盘上");
    game.halt_search();
    std::cout << "  [r193] 看到 ai_move 就回 ( " << row << "," << col << " )：error 0、"
              << "move_made 1" << std::endl;
    std::cout << "test_the_reply_after_ai_move_is_never_swallowed passed" << std::endl;
}


// #91：detach 下来的会话原本要留满 30 分钟才下架（断线续局要的就是这段时间）。一场赛具每局开两条
// 新链接 ⇒ 每局堆两个会话，实测 +14 MB/局，约 250 个/台就开始握手超时（轮次185 量的）。名额把
// 「窗口」换成「名额」：越过就把**最老的 detach 会话**照 overdue 那条路走 —— 先结算落盘，再上架成
// cold。丢的是引擎，不是局：token 仍可续，只是从文件重建。六臂：
//   ① 越过名额：老的那条被结算并下架，新的留下；
//   ② 被下架的 token 仍可 resume（resumed:true，且带回了那一局的子）；
//   ③ 没超名额 ⇒ 一条都不动；
//   ④ 缺省（0 = 不限）⇒ 今天的行为，两条都还热；
//   ⑤ 名额只管 detach 的：还在被 socket 用的那条不许被动；
//   ⑥ 文件还欠着写 ⇒ 这条不许上架，但它也不许把名额机制钉死 —— 跳过它，名额让给下一条文件本来就新的。
void test_a_detached_session_over_the_cap_is_settled_and_shelved() {
    const auto dir = scratch("r186_session_cap");
    const auto records = scratch("r186_cap_records.jsonl");
    const auto wall_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    };
    // 一条会话：开局、落 n 子、然后断开（detach 会把 detached_ms 盖上戳）。
    GameServer server(0, std::string(), std::string(), nullptr, dir.string(), records.string());
    std::vector<std::shared_ptr<WebSocketSession>> held;
    auto make_detached = [&server, &held](int stones) {
        auto socket = std::make_shared<WebSocketSession>(tcp::socket(server.ioc_), std::string());
        auto entry = server.open_session(nullptr);
        server.attach_socket(socket, entry);
        assert(entry->game->restore_game(playing_restore(stones)));
        server.detach_socket(socket.get());
        held.push_back(socket);
        return entry;
    };

    // ④ 缺省 = 不限：两条 detach 的会话都没过期，tick 之后都还热。
    {
        auto first = make_detached(1);
        auto second = make_detached(1);
        server.set_max_sessions(0);
        server.on_flush_tick();
        assert(server.sessions_.count(first->token) == 1);
        assert(server.sessions_.count(second->token) == 1);
        assert(server.cold_sessions_.empty());
    }

    // ③ 有名额但没超：cap=2、正好两条 ⇒ 仍然都不动。
    {
        server.set_max_sessions(2);
        server.on_flush_tick();
        assert(server.sessions_.size() == 2);
        assert(server.cold_sessions_.empty());
    }

    // ⑤ 名额只管 detach 的：再来一条挂在 socket 上的（活的），它不该是受害者。
    auto live_socket = std::make_shared<WebSocketSession>(tcp::socket(server.ioc_), std::string());
    auto live = server.open_session(nullptr);
    server.attach_socket(live_socket, live);
    assert(live->game->restore_game(playing_restore(2)));

    // ① 超名额：此刻有两条 detach + 一条活的，cap=1 ⇒ 最老的那条 detach 先下架，而且它是
    //    **先结算落盘**才走的。受害者按内容挑（map 的序是 token 序，不是插入序，begin() 可能就是
    //    那条活的）；「最老」用显式盖戳定死 —— 同一毫秒里两条谁先谁后不可判。
    std::shared_ptr<GameServer::TrackedGame> victim = nullptr;
    for (auto& kv : server.sessions_) {
        if (kv.first == live->token) continue;
        if (!victim || kv.second->detached_ms < victim->detached_ms) victim = kv.second;
    }
    assert(victim);
    const std::string victim_token = victim->token;
    victim->detached_ms = wall_ms() - 60LL * 1000LL;
    server.set_max_sessions(1);
    server.on_flush_tick();

    assert(server.sessions_.count(victim_token) == 0);
    assert(server.cold_sessions_.count(victim_token) == 1);
    assert(server.sessions_.count(live->token) == 1);   // ⑤ 活着的那条没被动
    // 落下的那一条已经不在内存里了 —— 被丢掉的是它的引擎（那张置换表），不是那局棋：② 会用旧 token 续回来。
    // 文件必须是它最后一次结算的样子：一局 1 子。
    std::string error;
    SessionSnapshot on_disk;
    assert(load_session(dir, victim_token, on_disk, error));
    assert(on_disk.record.moves.size() == 1);
    assert(server.sessions_.size() == 2);               // live + 剩下那条 detach

    // ⑥ 欠写的会话不许上架，但它**不该把名额机制钉死**：再造一条 detach 的，让它成为「最老的那条」
    //    并且带着没落盘的改动（文件里还是 1 子，盘上已经 2 子）。目录换成不可写之后 tick 应当
    //    跳过它（不许上架）、把名额让给下一条文件本来就是新的会话。
    auto third = make_detached(1);
    assert(third->game->restore_game(playing_restore(2)));
    third->detached_ms = wall_ms() - 120LL * 1000LL;
    const std::string third_token = third->token;
    const auto blocker = scratch("r186_blocker");
    { std::ofstream file(blocker); file << "not a directory"; }
    const std::string good_dir = server.sessions_dir_;
    server.sessions_dir_ = (blocker / "sub").string();
    const size_t cold_before = server.cold_sessions_.size();
    server.on_flush_tick();
    assert(server.sessions_.count(third_token) == 1);           // 欠写的这条没被搬走
    assert(server.cold_sessions_.count(third_token) == 0);
    assert(server.cold_sessions_.size() == cold_before + 1);    // 名额由另一条兑现了
    server.sessions_dir_ = good_dir;
    std::string dirty_error;
    SessionSnapshot dirty_file;
    assert(load_session(good_dir, third_token, dirty_file, dirty_error));
    assert(dirty_file.record.moves.size() == 1);                // 那第二手没有进文件
    std::error_code cleanup;
    std::filesystem::remove(blocker, cleanup);

    // ② 被下架的那条仍可续：新 socket 拿旧 token resume ⇒ resumed:true 且那一局的子回来了。
    auto second_socket = std::make_shared<WebSocketSession>(tcp::socket(server.ioc_), std::string());
    size_t seen = 0;
    server.handle_resume(second_socket, json{{"token", victim_token}});
    json frame;
    for (const std::string& text : drain_frames(server, second_socket, seen)) {
        const json parsed = json::parse(text, nullptr, false);
        if (!parsed.is_discarded() && parsed.value("type", "") == "session") frame = parsed;
    }
    assert(frame.is_object());
    assert(frame["resumed"] == true);
    assert(frame.contains("board"));

    std::cout << "  [r186] 名额 1：最老的 detach 会话被结算并上架，token 仍可续；live 与欠写的都没动"
              << std::endl;
    std::cout << "test_a_detached_session_over_the_cap_is_settled_and_shelved passed" << std::endl;
}

namespace {
// 轮次187 的挂死探针留下的三件东西故意不释放，见下面那段注释。
Harness* g_wedge_probe = nullptr;
std::thread* g_wedge_joiner = nullptr;
std::atomic<bool> g_wedge_returned{false};
}  // namespace

// 断开一条**正在思考**的链接，会不会把整台服务器钉死。
//
// 形状：on_disconnect（server.cpp:695）先拿 games_mutex_，detach_socket 在它里面 halt_search
// （:1422）—— 那一步是「join 掉正在跑的那条 worker」。而 worker 交回思考标志的最后一步
// （setup_game_callbacks 装的 think-finished，:1809）也要拿同一把 games_mutex_。两边各等对方，
// 谁都不会先撒手；而 io 线程是这台服务器唯一的反应器，所以挂住的不是这一局，是整个进程：
// 夹具上实测到 SIGINT 之后 7 分钟不退出、ping 也不回，/proc 里 io 线程与 worker 都停在
// futex_do_wait，那条 worker 的 utime=0 —— 它一睁眼就撞在锁上。
//
// 判法只能在「真挂住也出得来」的形状里做：拿锁那一端放到一根临时线程上，主线程只等**有界的工作量**；
// 一旦没回来，这两条卡住的线程连同 Harness 一起留在文件域指针里**不析构** —— 析构它会走到第二次
// 挂死（~GameManager 也要 join 那条卡住的 worker），而留在 static 里 valgrind 记 still reachable，
// 不是 definite leak。用例通过的那一支（修好之后）三件都是正常收尾的。
//
// 等待上界按「一整次有界搜索」计时来定，不写死秒数：memcheck 下整进程慢 ~20 倍、TSan ~3 倍，任何按
// 秒画的界线都会变成假失败（轮次168 被这条坑过一次）。收口那条路要做的工作不可能超过一手完整搜索 ——
// 停止旗一置，worker 在下一个节点就撒手 —— 所以 8 个单位是余量。
void test_a_socket_that_closes_while_the_ai_is_thinking_does_not_wedge_the_io_thread() {
    // 对照臂先跑，它有两个用处：证明「思考已经跑完的断开」本来就一直是对的，以及给出上面那个单位。
    Harness settled(scratch("r187_disconnect_settled"), std::string());
    GameManager& calm = *settled.entry->game;
    calm.set_config(120000, 6, -1, 40000);
    const auto unit_t0 = std::chrono::steady_clock::now();
    assert(calm.make_move(7, 7));
    join_think(calm);
    const long long unit_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - unit_t0).count();
    settled.server.on_disconnect(settled.socket.get());
    CHECK_MESSAGE(settled.entry->attached == nullptr, "没在飞的断开同样要收干净（对照臂）");
    const int ticks = static_cast<int>(std::max<long long>(2000, unit_ms * 8) / 10) + 1;

    auto* h = new Harness(scratch("r187_disconnect_while_thinking"), std::string());
    g_wedge_probe = h;
    GameManager& game = *h->entry->game;
    game.set_config(1200000, 8, -1, 400000);   // 一次思考要跑一阵，断开才赶得上它
    assert(game.make_move(7, 7));               // 人落一子：make_move 自己把 AI 那一手点起来
    CHECK_MESSAGE(game.is_ai_thinking(), "断开之前那条 worker 必须还在飞");

    g_wedge_returned.store(false);
    g_wedge_joiner = new std::thread([h] {
        h->server.on_disconnect(h->socket.get());   // io 线程在那一刻做的事：拿锁 + join worker
        g_wedge_returned.store(true);
    });
    for (int tick = 0; tick < ticks && !g_wedge_returned.load(); ++tick) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const bool came_back = g_wedge_returned.load();
    if (!came_back) {
        std::printf("  [r187] 断开挂住：io 线程抱着 games_mutex_ 等 worker，worker 等那把锁\n");
    }
    CHECK_MESSAGE(came_back,
                  "断开一条正在思考的会话必须自己收回来 —— 否则整台服务器连 ping 都不回");

    // 收回来了才收线程。挂住的那一支要继续留在文件域里不析构（上面注释写的那次二次挂死），
    // 而过了的这一支没理由把一条已完成的线程漏着 —— 轮次190 的 TSan 就把它记成了
    // `thread leak`（1 条，创建点正是这条用例在 main 里那一行）。
    if (came_back) {
        g_wedge_joiner->join();
        delete g_wedge_joiner;
        g_wedge_joiner = nullptr;
    }

    // 「收回来」不许是靠少做事换来的：这一条要变成 detach、标志要交回、盘要落进会话文件。
    CHECK_MESSAGE(h->entry->attached == nullptr, "断开之后这一条要挂成 detach");
    CHECK_MESSAGE(!game.is_ai_thinking(), "思考标志要交回来");
    CHECK_MESSAGE(std::filesystem::exists(h->session_file()), "断开那一刻的盘要落进会话文件");

    std::cout << "  [r187] 断开一条在飞的思考：join 回来了、detach 落盘了、标志交回了（单位 = 一手完整搜索 "
              << unit_ms << " ms，上界 " << ticks * 10 << " ms）" << std::endl;
    std::cout << "test_a_socket_that_closes_while_the_ai_is_thinking_does_not_wedge_the_io_thread passed"
              << std::endl;
}

// 轮次190 #105：求招收尾那句「这张盘还是不是我算的那张」现在比的是**盘面版本号**，不是子数。
// 这条钉两件事：(1) 每一处改 `board_` 的路都要把号 +1（号漏了，判据就会把动过的盘当没动）；
// (2) 「悔棋退两子、再落一手把子数补回原样」这一格 —— 旧判据（比 move_count）在这里会说
// 「还是那张盘」，把一份算在别的顺序上的推荐照样投递；号在这里说「不是」。**这条不需要线程，
// 所以前提是能直接看的**（并发那一面由 :1710 那条盖）。
void test_the_board_epoch_distinguishes_a_refilled_position_from_the_snapsotted_one() {
    Harness h(scratch("r190_board_epoch"), std::string());
    GameManager& game = *h.entry->game;
    // restore_game 也要算一次：它是「会话从记录里被重建」那一条改盘路。
    const int epoch_at_open = game.board_epoch_.load();
    assert(game.restore_game(hint_pose_restore()));      // 8 子，BLACK 应手
    assert(game.board_epoch_.load() == epoch_at_open + 1);
    const int armed_at = game.board().move_count();
    assert(armed_at == 8);

    // (1) 每一格改盘都算一次：号在 restore_game 之后已经动过，这里逐路量增量。
    int epoch = game.board_epoch_.load();
    assert(game.make_move(0, 0));                        // 人落一子（也会点起 AI 那一手）
    assert(game.board_epoch_.load() == epoch + 1);
    epoch = game.board_epoch_.load();
    join_think(game);                                    // AI 提交的那一子
    assert(game.board_epoch_.load() == epoch + 1);
    epoch = game.board_epoch_.load();
    int undone_count = -1;
    GameStatus undone_status = GameStatus::WAITING;
    assert(game.undo(undone_count, undone_status));
    // 悔一次退两子 ⇒ 号走两格，而不是一格
    assert(game.board_epoch_.load() == epoch + 2);
    epoch = game.board_epoch_.load();
    game.reset();
    assert(game.board_epoch_.load() == epoch + 1);
    epoch = game.board_epoch_.load();
    Board blank;
    blank.place_stone(7, 7, BLACK);
    assert(game.set_position(blank));
    assert(game.board_epoch_.load() == epoch + 1);

    // (2) 把子数补回原样的那一格：先立一个快照号，再动盘，动完子数一样、号不一样。
    assert(game.restore_game(hint_pose_restore()));
    assert(game.board().move_count() == armed_at);
    const int snapshot_epoch = game.board_epoch_.load();
    assert(game.board_matches_hint_snapshot(snapshot_epoch));   // 没动过 ⇒ 就是那张盘

    assert(!game.is_ai_thinking());
    assert(game.undo(undone_count, undone_status) && undone_count == armed_at - 2);
    assert(game.make_move(0, 0));                               // 落回一手，盘面 7 子
    join_think(game);                                           // AI 应一手 ⇒ 8 子，回到快照的大小
    assert(game.board().move_count() == armed_at);              // 旧判据在这一步会说「还是那张盘」
    assert(!game.board_matches_hint_snapshot(snapshot_epoch));  // 号说的是：不是

    std::cout << "  [r190] 盘面号：restore/reset/set_position/人手/AI 手各 +1、悔棋一次 +2；"
              << "子数补回 " << armed_at << " 的那一格旧判据会放行，号拦住了" << std::endl;
    std::cout << "test_the_board_epoch_distinguishes_a_refilled_position_from_the_snapsotted_one passed"
              << std::endl;
}

// 轮次190 的**测量**用例，登记为 #106。轮次193 修完之后它变成「改后应当无声」的取证：
// 跑法 `GOMOKU_R190_MEASURE=1 ./build-tsan/test_server`，TSan 应报 0 条 race。
// 循环里「拷贝整盘」与「跑掉已 post 的提交任务」都在同一根线程上 —— 这正是产品里 io 线程
// 做 `state_sync`（`server.cpp:825` 的形状）与 `commit_pending` 的形状；worker 已经不再写
// `board_`。改前（轮次193 开工时的 HEAD）同一跑法实测 7 条：全是主线程
// `Board(const Board&)` 读 vs worker 持 `thinking_mutex_` 的写 —— `board.cpp:64` ×2、
// `:145` ×2、`:65` ×1，另两条报在 `board.h:45` 的拷贝上。
// 不带变量 ⇒ 缺省门禁不跑它（它是要开着 TSan 才读得出东西的仪器）。
void test_the_io_thread_board_copy_overlaps_the_ai_commit() {
    if (std::getenv("GOMOKU_R190_MEASURE") == nullptr) {
        std::cout << "  [#106] 跳过（要跑它请给 GOMOKU_R190_MEASURE=1）" << std::endl;
        return;
    }
    Harness h(scratch("r190_board_overlap_measure"), std::string());
    GameManager& game = *h.entry->game;
    assert(game.restore_game(hint_pose_restore()));
    game.set_config(1200000, 12, -1, 20000000);   // 一手要跑很久，读数窗才够宽
    assert(game.make_move(3, 10));                 // 人落一子 ⇒ AI 那一手在飞
    assert(game.is_ai_thinking());                 // 前提直接看，不拿墙钟当代理
    long reads = 0;
    while (game.is_ai_thinking()) {
        // 属主那条路：这一泵跑的可能正是 worker 停下时 post 的那次提交。
        h.server.ioc_.restart();
        h.server.ioc_.poll();
        Board copy = game.board();                 // handle_state 读的就是这一格
        (void)copy.move_count();
        ++reads;
    }
    join_think(game);
    std::cout << "  [#106] 思考在飞期间的整盘拷贝 = " << reads
              << " 次；拷贝与提交都在同一根线程上，worker 不写 `board_`（TSan 应 0 条）"
              << std::endl;
    std::cout << "test_the_io_thread_board_copy_overlaps_the_ai_commit passed" << std::endl;
}
// 轮次191 #103：未完局不进 records。这条把轮次188 那两次同版 null「418 个会话文件、records 各只有 1 行」的成因钉住：
// 418 个会话里只有 2 个是空 base（开局集第 1 行那条 `{}` ⇒ 夹具不发 set_position），其中一局（CSV 第 0 局）本台执黑赢下、
// 31 手之后认输的是对面那台 —— `resign` 只出现在轮到落子那一侧的链接上（`tree_match.cpp:409-445`）⇒ 本台会话停在
// `status:"playing"` + `reason:"invalid"`，非决定性、按 `export_game_record`（`server.cpp:1594-1606`）本来就不该落；
// 另一局本台执白认输 ⇒ 落 1 行。⇒ 代码与实测自洽，188 记的「两局都是空 base + 决定性结束」是错的（B 段改判）。
void test_an_unfinished_game_stays_out_of_the_record_stream_until_it_ends() {
    const auto dir = scratch("r191_unfinished_sessions");
    const auto records = scratch("r191_unfinished_records.jsonl");
    {
        Harness h(dir, records);
        // 黑四子 {7,3}..{7,6}、白四子交错，BLACK 应手：差一手成五，正是「跑了一阵还没完」的形状
        GameRestore mid;
        mid.player_color = BLACK;
        mid.status = GameStatus::PLAYING;
        const Position cells[8] = {{7, 3}, {0, 0}, {7, 4}, {0, 1},
                                   {7, 5}, {0, 2}, {7, 6}, {0, 3}};
        for (int i = 0; i < 8; ++i) {
            MoveAnalysis analysis;
            analysis.move = Move(cells[i].row, cells[i].col, i % 2 == 0 ? BLACK : WHITE);
            analysis.method = "human";
            mid.moves.push_back(analysis);
        }
        assert(h.entry->game->restore_game(mid));
        assert(h.entry->game->status() == GameStatus::PLAYING);
        assert(!h.entry->game->is_ai_thinking());

        h.server.flush_entry(h.entry);
        assert(records_lines(records) == 0);              // 未完局：records 一行都不许多
        assert(!h.entry->exported);
        assert(std::filesystem::exists(h.session_file()));   // 但会话文件要落：复盘与续局的料
        std::ifstream in(h.session_file());
        const json saved = json::parse(in, nullptr, false);
        assert(!saved.is_discarded());
        assert(saved["status"] == "playing");
        assert(saved["record"]["reason"] == "invalid");   // 与 188 那份现场同一个读数
        assert(saved["record"]["moves"].size() == 8);

        // 把这局下完：黑补上 {7,7} 成五 —— 同一局到这一刻才有资格进 records
        h.server.handle_make_move(h.socket, json{{"row", 7}, {"col", 7}});
        assert(h.entry->game->status() == GameStatus::GAME_OVER);
        assert(!h.entry->game->is_ai_thinking());
        h.server.flush_entry(h.entry);
        assert(records_lines(records) == 1);
        assert(h.entry->exported);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "  [r191] 未完局：records 0 行而会话文件写 playing/invalid；补上第五子之后同一局才落 1 行"
              << std::endl;
    std::cout << "test_an_unfinished_game_stays_out_of_the_record_stream_until_it_ends passed"
              << std::endl;
}

int main() {
    std::cout << "Running server tests..." << std::endl;

    PatternRecognizer::init();

    test_undo_reopens_export();

    test_the_ponder_share_reaches_the_sessions_it_opens();
    test_set_position_reopens_export();
    test_emptied_board_deletes_residue();
    test_failed_flush_retries_and_keeps_dedup();
    test_failed_flush_keeps_the_overdue_entry_hot();
    test_check_position_skips_session_creation();
    test_failed_export_retries_records_append();
    test_shutdown_flush_keeps_the_file_behind_a_failed_append();
    test_shutdown_flush_appends_once_per_ending();
    test_shutdown_flush_with_persistence_off_writes_nothing();
    test_shutdown_flush_keeps_a_zero_stone_resign_out_of_the_records();
    test_request_hint_clears_stale_discard();
    test_position_array_string_payload_answers();
    test_pending_position_direct_move_claims_side();
    test_resign_survives_interruption();
    test_unreadable_record_file_survives();
    test_resign_needs_a_played_ply_not_base_stones();
    test_set_position_rotates_token_after_finished_game();
    test_set_position_restamps_session_created_ms();
    test_unreadable_resume_gets_a_fresh_token();
    test_resume_takeover_notifies_the_loser();
    test_hint_answer_for_a_superseded_position();
    test_finished_game_answers_a_hint_with_the_error_frame();
    test_check_position_reads_a_bom_prefixed_document();
    test_human_move_answers_the_stone_before_the_end();
    test_a_rejected_stone_delivers_the_end_a_worker_parked();
    test_resume_restores_each_config_half();
    test_zero_stone_position_keeps_the_token();
    test_transient_accept_failure_rearms_and_abort_ends();
    test_http_answers_a_read_with_the_file_and_a_write_with_a_refusal();
    test_resolve_file_reads_a_single_dot_segment_and_still_refuses_every_escape();
    test_a_shelved_contentless_session_does_not_keep_its_token();
    test_a_shelved_played_session_keeps_its_token();
    test_a_half_written_encoding_does_not_veto_a_bitboard_upload();
    test_thinking_progress_carries_partial_fields();
    test_replay_roundtrip_keeps_new_fields();
    test_set_config_keeps_the_two_optional_budgets();
    test_config_schema_carries_the_fields_the_panel_needs();
    test_config_set_refuses_out_of_range_without_touching_the_session();
    test_config_set_applies_each_live_class_and_acks_it();
    test_config_preview_is_repeatable_and_commits_nothing();
    test_config_preview_is_refused_while_the_ai_is_thinking();
    test_preview_arms_are_the_pinned_poses();
    test_undo_then_the_same_stone_reuses_the_table_across_the_takeback();
    test_a_different_stone_after_the_takeback_pays_a_fresh_think_again();
    test_the_takeback_lands_on_the_same_position_and_a_repeat_stays_stable();
    test_the_ai_commit_runs_on_the_owner_thread();
    test_a_parked_result_is_dropped_when_thinking_is_stopped();
    test_the_reply_after_ai_move_is_never_swallowed();
    test_a_detached_session_over_the_cap_is_settled_and_shelved();
    test_a_socket_that_closes_while_the_ai_is_thinking_does_not_wedge_the_io_thread();
    test_the_board_epoch_distinguishes_a_refilled_position_from_the_snapsotted_one();
    test_the_io_thread_board_copy_overlaps_the_ai_commit();
    test_an_unfinished_game_stays_out_of_the_record_stream_until_it_ends();

    test_analyze_progress_then_exactly_one_result_per_ply();
    test_analyze_is_refused_while_the_ai_is_thinking();
    test_analyze_refuses_the_positions_it_cannot_rebuild_and_the_games_it_cannot_read();


    std::cout << "All server tests passed!" << std::endl;
    return 0;
}

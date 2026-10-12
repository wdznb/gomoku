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

    GameManager after_resign;
    after_resign.set_player_color(BLACK);
    after_resign.resign(BLACK);
    assert(after_resign.status() == GameStatus::GAME_OVER);
    probe("resign", after_resign);

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
        // worker is driven directly so the window is deterministic.
        game.stop_flag_.store(true);
        std::thread worker(&GameManager::ai_thinking_worker, &game, 2000, 2);
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
        std::thread worker(&GameManager::ai_thinking_worker, &game, 2000, 2);
        worker.join();
        assert(game.status() == GameStatus::PLAYING);
        assert(frames == 1);  // unchanged: no second callback
    }
    std::cout << "test_resign_survives_interruption passed" << std::endl;
}

// Round 79, A6 (contract updated in R85): a session file whose record cannot be
// rebuilt (open_session falls back to a fresh empty game) must stay on disk byte
// for byte while the game is still stone-less: choosing a side alone changes the
// flushed payload, which used to send flush_entry's empty-board branch into
// delete_session - the very file the token names. The game the player gets is not
// the damaged token's any more (README:476-477 hands out a fresh one), so the
// entry is found through the socket and the old file is what must survive.
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
        assert(resumed->unreadable_record);
        assert(!resumed->rebuilt);
        // The caller gets an empty game, not a half-replayed one.
        assert(resumed->game->replay_snapshot().move_count == 0);

        // Only a side is chosen - no stone is placed, so the board stays empty
        // while the payload the next flush would write has changed.
        h.server.handle_choose_side(h.socket, json{{"color", "black"}});
        assert(resumed->side_chosen);

        h.server.detach_socket(h.socket.get());  // flushes the session on the way out

        assert(std::filesystem::exists(dir / "deadbeef.json"));
        assert(read_text(dir / "deadbeef.json") == damaged);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(records, ec);
    std::cout << "test_unreadable_record_file_survives passed" << std::endl;
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

}  // namespace

int main() {
    std::cout << "Running server tests..." << std::endl;

    PatternRecognizer::init();

    test_undo_reopens_export();
    test_set_position_reopens_export();
    test_emptied_board_deletes_residue();
    test_failed_flush_retries_and_keeps_dedup();
    test_check_position_skips_session_creation();
    test_failed_export_retries_records_append();
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

    std::cout << "All server tests passed!" << std::endl;
    return 0;
}

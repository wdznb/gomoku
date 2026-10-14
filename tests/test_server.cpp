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
void test_hint_answer_for_a_superseded_position() {
    const auto dir = scratch("stale_hint_sessions");
    const auto records = scratch("stale_hint_records.jsonl");
    {
        Harness h(dir, records);
        GameManager& game = *h.entry->game;
        assert(game.restore_game(playing_restore(1)));  // one black stone, WHITE to move
        assert(game.current_turn() == WHITE);
        assert(game.board().move_count() == 1);

        size_t seen = 0;
        const auto requested = std::chrono::steady_clock::now();
        h.server.handle_request_hint(h.socket, json{{"timeout_ms", 3000}, {"max_depth", 8}});
        assert(game.hint_active_.load());

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int undone_count = -1;
        GameStatus undone_status = GameStatus::WAITING;
        assert(game.undo(undone_count, undone_status));
        assert(undone_count == 0);
        // The undo must not have armed an AI worker either, or the answer would
        // be dropped at the discard gate rather than at the stale-position check.
        assert(!game.is_ai_thinking());

        for (int i = 0; i < 300 && game.hint_active_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        assert(!game.hint_active_.load());
        const long long delivery_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - requested).count();
        // The premise of this case: the stone was given back while the search was
        // still running, not after it had already answered.
        assert(delivery_ms > 500);

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
        assert(hint_results == 0);
        assert(answer_type == "error");
        assert(answer_message == "No hint available");
        std::cout << "  stale hint: " << answer_type << " / " << answer_message
                  << " after " << delivery_ms << " ms" << std::endl;

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

int main() {
    std::cout << "Running server tests..." << std::endl;

    PatternRecognizer::init();

    test_undo_reopens_export();
    test_set_position_reopens_export();
    test_emptied_board_deletes_residue();
    test_failed_flush_retries_and_keeps_dedup();
    test_failed_flush_keeps_the_overdue_entry_hot();
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
    test_hint_answer_for_a_superseded_position();
    test_resume_restores_each_config_half();
    test_zero_stone_position_keeps_the_token();
    test_transient_accept_failure_rearms_and_abort_ends();

    std::cout << "All server tests passed!" << std::endl;
    return 0;
}

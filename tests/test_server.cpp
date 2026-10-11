// Round-72 white-box tests over GameServer's session bookkeeping: the exported
// stamp that undo / set_position reset, the residue delete when a board empties,
// and the saved_payload stamp that may only move after a successful write.
// All three live behind private handlers and private maps, so the class is
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
#include "server/game_manager.h"
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
// same token is exported again. An empty hand-set position is the shape that
// keeps the token exportable - base stones suspend the export by design.
void test_set_position_reopens_export() {
    const auto dir = scratch("setup_sessions");
    const auto records = scratch("setup_records.jsonl");
    {
        Harness h(dir, records);
        assert(h.entry->game->restore_game(five_restore(BLACK)));
        h.server.flush_entry(h.entry);
        assert(records_lines(records) == 1);
        assert(h.entry->exported);

        h.server.handle_set_position(
            h.socket, json{{"position", {{"version", 1}, {"moves", json::array()}}}});
        assert(h.entry->game->position_pending());
        assert(!h.entry->exported);
        assert(!h.entry->side_chosen);

        // A zero-stone session is not kept: the finished game's file goes with it.
        h.server.flush_entry(h.entry);
        assert(!std::filesystem::exists(h.session_file()));

        // A second, different game finishing on the same token exports again.
        assert(h.entry->game->restore_game(five_restore(WHITE)));
        h.server.flush_entry(h.entry);
        assert(records_lines(records) == 2);
        assert(h.entry->exported);
        assert(std::filesystem::exists(h.session_file()));
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

    std::cout << "All server tests passed!" << std::endl;
    return 0;
}

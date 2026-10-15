#ifndef GOMOKU_SERVER_H
#define GOMOKU_SERVER_H

#include "game_manager.h"
#include "session_store.h"
#include "../optimization/opening_book.h"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/signal_set.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace gomoku {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace http = beast::http;
using tcp = net::ip::tcp;
using json = nlohmann::json;

class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
public:
    WebSocketSession(tcp::socket&& socket, const std::string& web_dir);
    
    void start();
    void send(const json& msg);
    void close();
    
    std::function<void(std::shared_ptr<WebSocketSession>, const json&)> on_message;
    std::function<void(std::shared_ptr<WebSocketSession>)> on_close;
    // Invoked only after a successful WebSocket handshake, so a plain HTTP
    // request (a page or asset load) never creates a game/engine.
    std::function<void(std::shared_ptr<WebSocketSession>)> on_open;
    
private:
    websocket::stream<beast::tcp_stream> ws_;
    std::string web_dir_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    std::shared_ptr<http::response<http::string_body>> res_;
    // Queue access and write initiation all run on ws_'s executor (posted
    // from send()), so no separate mutex is needed.
    std::queue<std::string> send_queue_;
    // Set by close() when the queue was not empty: the shutdown waits for the
    // drain in do_write() so a frame sent just before close() still leaves.
    bool close_after_drain_ = false;
    
    void do_read();
    void do_write();
    // TCP shutdown without a handshake; close() reaches it only once the write
    // queue is empty.
    void drop_socket();
    // Slide the socket's idle deadline forward. Beast's tcp_stream expiry is one
    // absolute deadline shared by every later operation, so it has to be re-armed
    // by hand or it caps the connection's lifetime instead of its silence.
    void arm_idle();
    void on_http_read(beast::error_code ec, std::size_t bytes);
    void run_websocket();
    void handle_http_request();
    void send_http_response();
    // Resolve a request target to a regular file strictly inside web_dir_.
    // Returns an empty path on any traversal / miss so the caller sends 404.
    std::filesystem::path resolve_file(const std::string& target);
    std::string get_content_type(const std::string& path);
};

class GameServer {
public:
    // `coefficients`, when non-null, replaces the built-in threat values for every
    // game. Only read during construction, so a temporary from main() is safe.
    // `sessions_dir` is where each session's own file lives and `records_path` the
    // JSONL the finished games are appended to for the trainer; both may be empty
    // to run without persistence.
    GameServer(int port = 8080, const std::string& web_dir = "web",
               const std::string& book_path = "data/opening_book.json",
               const ThreatCoefficients* coefficients = nullptr,
               const std::string& sessions_dir = "data/sessions",
               const std::string& records_path = "data/training/records.jsonl");

    void start();
    void stop();

private:
    // A game plus what the server needs to hand it back after its socket went
    // away. The game outlives the WebSocket it was played on: `attached` is null
    // while nobody holds it, and the entry itself stays until a client resumes
    // the token or the reattach window closes.
    struct TrackedGame {
        std::string token;
        std::shared_ptr<GameManager> game;
        WebSocketSession* attached = nullptr;
        long long created_ms = 0;
        long long updated_ms = 0;
        // Zero while attached. How long the socket has been gone decides when the
        // entry is dropped back to disk.
        long long detached_ms = 0;
        // The player picked a side, so the session has one rather than the
        // default. Only a chosen side is written as player_color.
        bool side_chosen = false;
        // This session was rebuilt from a saved record rather than opened empty,
        // which is what makes a `resume` answer `true` instead of a new token.
        bool rebuilt = false;
        // Already appended to records_path, so a resumed game does not export a
        // second copy of the same ending.
        bool exported = false;
        // The last flushed payload with its timestamps zeroed: unchanged means
        // nothing to write, which is what keeps a quiet session off the disk.
        std::string saved_payload;
    };

    net::io_context ioc_;
    tcp::acceptor acceptor_;
    net::signal_set signals_;
    std::string web_dir_;
    OpeningBook book_;
    // Trained coefficients from --params. Applied to every new game because each
    // GameManager owns its own engine; the file is read once here, never again.
    ThreatCoefficients coefficients_;
    bool use_loaded_coefficients_ = false;
    std::map<WebSocketSession*, std::shared_ptr<GameManager>> games_;
    std::map<std::string, std::shared_ptr<TrackedGame>> sessions_;
    std::map<WebSocketSession*, std::string> socket_tokens_;
    // Sessions read from disk that no socket has claimed yet. Kept as snapshots,
    // not GameManagers, because each engine carries its own transposition table:
    // a restarted server can therefore remember every game it saved without
    // paying for a board per game.
    std::map<std::string, SessionSnapshot> cold_sessions_;
    std::string sessions_dir_;
    std::string records_path_;
    net::steady_timer flush_timer_;
    // Retries do_accept() after a transient accept failure. Without it one bad
    // accept (descriptor exhaustion, a client resetting mid-accept) would leave no
    // pending operation and the port would stop being served for the rest of the run.
    net::steady_timer accept_retry_timer_;
    // Guards every map above. The io thread is the only thread that touches them
    // today, so the lock is the record of that rule rather than a requirement.
    std::mutex games_mutex_;
    // Only the io thread ever assigns deferred_end_for_, but an AI worker can report
    // a settlement for the same session pointer concurrently, so this pair needs its
    // own lock and nothing else may be locked while holding it.
    std::mutex end_frame_mutex_;
    WebSocketSession* deferred_end_for_ = nullptr;
    json deferred_end_frame_;
    
    void do_accept();
    void on_connect(std::shared_ptr<WebSocketSession> session);
    void on_open(std::shared_ptr<WebSocketSession> session);
    void on_disconnect(WebSocketSession* session);
    void on_message(std::shared_ptr<WebSocketSession> session, const json& msg);
    
    void handle_message(std::shared_ptr<WebSocketSession> session, const json& msg);
    void send_message(std::shared_ptr<WebSocketSession> session, const json& msg);
    void send_error(std::shared_ptr<WebSocketSession> session, const std::string& message);
    
    void handle_choose_side(std::shared_ptr<WebSocketSession> session, const json& msg);
    void handle_check_position(std::shared_ptr<WebSocketSession> session, const json& msg);
    void handle_set_position(std::shared_ptr<WebSocketSession> session, const json& msg);
    void handle_make_move(std::shared_ptr<WebSocketSession> session, const json& msg);
    void handle_undo(std::shared_ptr<WebSocketSession> session);
    void handle_request_replay(std::shared_ptr<WebSocketSession> session);
    void handle_request_hint(std::shared_ptr<WebSocketSession> session, const json& msg);
    void handle_resign(std::shared_ptr<WebSocketSession> session);
    void handle_stop_thinking(std::shared_ptr<WebSocketSession> session);
    void handle_set_config(std::shared_ptr<WebSocketSession> session, const json& msg);
    void handle_restart(std::shared_ptr<WebSocketSession> session);
    void handle_resume(std::shared_ptr<WebSocketSession> session, const json& msg);
    
    json board_to_json(const Board& board);
    
    void setup_game_callbacks(std::shared_ptr<WebSocketSession> session, GameManager& game);
    static void clear_game_callbacks(GameManager& game);

    // Sessions on disk the process has not been told about yet.
    void load_saved_sessions();
    // Give the socket a game to play in, opening a new session if it has not
    // resumed one. Returns null only if the socket is already gone.
    std::shared_ptr<TrackedGame> ensure_attached(std::shared_ptr<WebSocketSession> session);
    // The session this socket plays in, or null. games_mutex_ must already be
    // held: every handler takes it for its whole body, and the flush tick holds it
    // while it walks the table.
    std::shared_ptr<TrackedGame> entry_locked(WebSocketSession* session);
    std::shared_ptr<TrackedGame> open_session(const SessionSnapshot* from);
    void attach_socket(const std::shared_ptr<WebSocketSession>& session,
                       const std::shared_ptr<TrackedGame>& entry);
    void send_session_state(const std::shared_ptr<WebSocketSession>& session,
                            const std::shared_ptr<TrackedGame>& entry, bool resumed);

    SessionSnapshot snapshot_of(const TrackedGame& entry) const;
    // Writes the session file when the game changed, and appends the record once
    // the game is over and replayable on its own. False means the write did not
    // settle, so the entry still owes a retry and must not be shelved as cold.
    bool flush_entry(const std::shared_ptr<TrackedGame>& entry);
    void flush_all();
    // Releases a game its socket no longer holds, keeping it in memory until the
    // reattach window runs out. games_mutex_ must already be held; on_disconnect
    // takes it.
    void detach_socket(WebSocketSession* session);
    void schedule_flush();
    void on_flush_tick();
};

}

#endif

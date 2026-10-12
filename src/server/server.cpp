#include "server.h"
#include "position_json.h"
#include "training/json_int.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <csignal>
#include <chrono>
#include <cctype>
#include <atomic>
#include <random>

namespace gomoku {

namespace {
// json::value(key, default) only falls back to the default when the key is
// absent; a present-but-wrong-type value (e.g. {"row":"7"}) throws
// type_error, which unwinds past the handler's own validation into do_read's
// generic catch and leaves the client with no reply. Read integers defensively
// so a malformed field is treated as the default and the handler still
// validates and responds. The same reasoning covers width: get<int> only casts,
// so {"row":4294967304} would arrive as 8 and pass the 0-14 square check, while
// read_json_int refuses anything that does not fit an int and the fallback lets
// the handler reject the move instead of playing a truncated square.
int get_int(const json& msg, const char* key, int fallback) {
    int value = 0;
    if (!msg.contains(key) || !read_json_int(msg[key], value)) return fallback;
    return value;
}

// Config fields are per-message overrides: one the frame does not carry keeps the
// value the session already has, so a client that sends only max_depth does not
// have the timeout it chose earlier reset under it. A value that is present but
// not a representable integer counts as not sent, which is also what stops the
// 64-bit truncation get_int defends against from reaching the game.
int get_int_or(const json& msg, const char* key, int current) {
    int value = 0;
    if (!msg.contains(key) || !read_json_int(msg[key], value)) return current;
    return value;
}

// Pull the position payload out of a check_position / set_position message.
// `position` is either the canonical document or the board as text, so a file
// upload and a paste box send the same shape; a top-level `encoding` is merged
// into the document because it is the mapping the caller is stating.
bool read_position_payload(const json& msg, json& doc, std::string& error) {
    if (!msg.contains("position")) {
        error = "缺少 position 字段";
        return false;
    }
    const json& value = msg["position"];
    if (value.is_object()) {
        doc = value;
    } else if (value.is_string()) {
        const std::string text = value.get<std::string>();
        json parsed = json::parse(text, nullptr, false);
        if (parsed.is_object()) {
            doc = std::move(parsed);
        } else if (parsed.is_array()) {
            // A bare array is the board matrix, not a document: merging
            // `encoding` into it would index an array with a string and throw out
            // of the handler before any frame goes back.
            doc = json::object();
            doc["board"] = std::move(parsed);
        } else {
            // Not a document, so a grid: CSV, txt, or one long flat run of
            // symbols. A numeric grid parses as a JSON *number* ("100...0" is a
            // 225-digit integer), and taking that value would report "局面文档必须
            // 是 JSON 对象" for a perfectly readable board.
            doc = json::object();
            doc["board"] = text;
        }
    } else {
        error = "position 必须是局面对象或棋盘文本";
        return false;
    }
    if (msg.contains("encoding")) doc["encoding"] = msg["encoding"];
    return true;
}

const char* side_name(int color) {
    return color == BLACK ? "black" : "white";
}

// EMPTY for anything that is not a side, which is how an unstarted session and a
// damaged colour string both end up meaning "nobody has chosen yet".
int side_of(const std::string& name) {
    if (name == "black") return BLACK;
    if (name == "white") return WHITE;
    return EMPTY;
}

const char* status_name(GameStatus status) {
    switch (status) {
        case GameStatus::PLAYING: return "playing";
        case GameStatus::GAME_OVER: return "game_over";
        case GameStatus::WAITING: break;
    }
    return "waiting";
}

GameStatus status_of(const std::string& name) {
    if (name == "playing") return GameStatus::PLAYING;
    if (name == "game_over") return GameStatus::GAME_OVER;
    return GameStatus::WAITING;
}

// How long a session may stay silent in both directions before it is reaped. The
// browser pings well inside this window, so it only fires for a peer that is
// really gone; a player thinking over the board is covered by that ping rather
// than by widening this value.
constexpr int kIdleTimeoutSeconds = 120;

// How often the session table is written to disk, and how long a game whose
// socket went away stays in memory waiting for it to come back. Past the window
// the game is dropped back to its file: the token keeps working, but resuming it
// costs a rebuild instead of a reattach.
constexpr int kFlushIntervalSeconds = 10;
constexpr int kReattachWindowSeconds = 30 * 60;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Make a directory the server writes into and say plainly when it cannot. Both
// the session files and the record stream are created on demand, so a missing
// parent is a startup problem, not something to discover once per flush.
bool prepare_dir(const std::string& path, const char* what) {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec && !std::filesystem::is_directory(path)) {
        std::cerr << what << " " << path << " 建不出来: " << ec.message() << std::endl;
        return false;
    }
    return true;
}

std::string make_token() {
    // Mixed from the system entropy source, the clock and a per-process counter,
    // so two sessions opened in the same millisecond still differ and a token is
    // not guessable from the file listing.
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    const std::uint64_t mixed = (static_cast<std::uint64_t>(rd()) << 32) ^
                                static_cast<std::uint64_t>(rd()) ^
                                static_cast<std::uint64_t>(now_ms()) ^
                                (static_cast<std::uint64_t>(++counter) << 20);
    static const char digits[] = "0123456789abcdef";
    std::string token(16, '0');
    for (int i = 0; i < 16; ++i) {
        token[i] = digits[(mixed >> (60 - 4 * i)) & 0xf];
    }
    return token;
}

// The server states an ending in its own words and the training record has its
// own enum; "draw" is the board filling up, which is the one ending the record
// calls something else.
GameEndReason end_reason_of(const std::string& reason) {
    if (reason == "five") return GameEndReason::FIVE;
    if (reason == "resign") return GameEndReason::RESIGN;
    if (reason == "draw") return GameEndReason::BOARD_FULL;
    if (reason == "forbidden") return GameEndReason::FORBIDDEN;
    return GameEndReason::INVALID;
}

const char* server_reason_of(GameEndReason reason) {
    switch (reason) {
        case GameEndReason::FIVE: return "five";
        case GameEndReason::RESIGN: return "resign";
        case GameEndReason::BOARD_FULL: return "draw";
        case GameEndReason::FORBIDDEN: return "forbidden";
        default: break;
    }
    return "";
}

MoveRecord record_of(const MoveAnalysis& analysis) {
    MoveRecord entry;
    entry.move = analysis.move;
    entry.score = analysis.score;
    entry.depth = analysis.depth;
    entry.nodes = analysis.nodes;
    entry.elapsed_ms = analysis.elapsed_ms;
    entry.method = analysis.method;
    return entry;
}

MoveAnalysis analysis_of(const MoveRecord& entry) {
    MoveAnalysis analysis;
    analysis.move = entry.move;
    analysis.score = entry.score;
    analysis.depth = entry.depth;
    analysis.nodes = entry.nodes;
    analysis.elapsed_ms = entry.elapsed_ms;
    analysis.method = entry.method;
    return analysis;
}

// The board a session reports is the one its record describes, not the one the
// GameManager is holding. A resumed game may already have a search running (a
// second tab taking over a live session), and reading board_ from the io thread
// next to that worker is the race every other reply here avoids by taking the
// snapshot under thinking_mutex_.
Board board_of(const ReplaySnapshot& replay) {
    Board board;
    for (const Move& stone : replay.base) {
        board.place_stone(stone.pos.row, stone.pos.col, stone.color);
    }
    for (const MoveAnalysis& entry : replay.moves) {
        board.place_stone(entry.move.pos.row, entry.move.pos.col, entry.move.color);
    }
    return board;
}
}  // namespace


WebSocketSession::WebSocketSession(tcp::socket&& socket, const std::string& web_dir)
    : ws_(std::move(socket)), web_dir_(web_dir) {
}

void WebSocketSession::start() {
    // The server multiplexes one port: a browser first issues a plain GET for
    // index.html/app.js/style.css (served from web_dir_), then opens the
    // WebSocket on the same origin. Read the HTTP request headers before
    // deciding which path to take; the WebSocket handshake reuses the request
    // we just parsed, so nothing is consumed twice.
    ws_.next_layer().expires_after(std::chrono::seconds(kIdleTimeoutSeconds));
    http::async_read(
        ws_.next_layer(), buffer_, req_,
        [self = shared_from_this()](beast::error_code ec, std::size_t bytes) {
            self->on_http_read(ec, bytes);
        });
}

// Re-arm rather than replace: the expiry set once at accept would cap how long a
// connection may live, not how long it may be silent, and every session would
// drop 60 s after the page loaded no matter how busy the game was.
void WebSocketSession::arm_idle() {
    ws_.next_layer().expires_after(std::chrono::seconds(kIdleTimeoutSeconds));
}

void WebSocketSession::on_http_read(beast::error_code ec, std::size_t) {
    if (ec) {
        // Not a well-formed HTTP request (e.g. a raw TCP probe); drop it. The
        // game was never created, so there is nothing to unregister.
        beast::error_code ignored;
        ws_.next_layer().socket().close(ignored);
        return;
    }

    if (websocket::is_upgrade(req_)) {
        run_websocket();
    } else {
        handle_http_request();
    }
}

void WebSocketSession::run_websocket() {
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res) {
            res.set(boost::beast::http::field::server, "GomokuAI");
        }));

    ws_.async_accept(
        req_,
        [self = shared_from_this()](beast::error_code ec) {
            if (ec) {
                std::cerr << "WebSocket accept error: " << ec.message() << std::endl;
                if (self->on_close) {
                    self->on_close(self);
                }
                // No game was created (on_open only runs on success), so close
                // the socket here rather than rely on the destructor's timing,
                // matching the malformed-request path in on_http_read.
                beast::error_code ignored;
                self->ws_.next_layer().socket().close(ignored);
                return;
            }
            // The handshake succeeded, so this session is a real game
            // connection; only now does the server build its GameManager.
            if (self->on_open) {
                self->on_open(self);
            }
            self->do_read();
        });
}

void WebSocketSession::do_read() {
    arm_idle();
    ws_.async_read(
        buffer_,
        [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
            if (ec) {
                if (ec != websocket::error::closed) {
                    std::cerr << "WebSocket read error: " << ec.message() << std::endl;
                }
                if (self->on_close) {
                    self->on_close(self);
                }
                return;
            }
            
            std::string text = beast::buffers_to_string(self->buffer_.data());
            self->buffer_.consume(bytes_transferred);
            
            try {
                json msg = json::parse(text);
                if (self->on_message) {
                    self->on_message(self, msg);
                }
            } catch (const std::exception& e) {
                std::cerr << "JSON parse error: " << e.what() << std::endl;
            }
            
            self->do_read();
        });
}

void WebSocketSession::send(const json& msg) {
    std::string payload = msg.dump();
    // send() is called from the AI search worker and hint threads (via the
    // GameManager move / game-over / progress callbacks) as well as the io
    // thread. websocket::stream is not thread-safe and must have every
    // operation, including async_write initiation, run on its executor;
    // calling do_write() inline here would start a write on the worker thread
    // concurrently with the io thread's outstanding async_read. Post onto the
    // stream executor so enqueue + write are serialized with the read side.
    net::post(
        ws_.get_executor(),
        [self = shared_from_this(), payload = std::move(payload)]() mutable {
            bool write_in_progress = !self->send_queue_.empty();
            self->send_queue_.push(std::move(payload));
            if (!write_in_progress) {
                self->do_write();
            }
        });
}

void WebSocketSession::do_write() {
    if (send_queue_.empty()) {
        return;
    }
    // Traffic in either direction counts as liveness: an AI search that takes
    // tens of seconds leaves the client silent, and the server's own progress
    // messages should not be the thing that gets the session reaped.
    arm_idle();

    ws_.async_write(
        net::buffer(send_queue_.front()),
        [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "WebSocket write error: " << ec.message() << std::endl;
                return;
            }
            self->send_queue_.pop();
            self->do_write();
        });
}

void WebSocketSession::close() {
    // Not Beast's closing handshake: that waits for the peer to answer, and this
    // runs from another socket's handler on the one io thread, so a peer that is
    // already gone or never answers would stop the server for every game on it.
    // Dropping the connection makes the outstanding read come back as an error,
    // and that handler releases the session like any other disconnect.
    beast::error_code ec;
    auto& socket = ws_.next_layer().socket();
    socket.shutdown(tcp::socket::shutdown_both, ec);
    socket.close(ec);
}

// Resolve a request target to a regular file that lives strictly under
// web_dir_. weakly_canonical folds any ".." lexically and resolves symlinks on
// existing prefixes, and the final string-prefix check drops every path that
// escapes the root, so "/../../etc/passwd" and percent-encoded variants return
// empty (-> 404). Absolute targets, backslashes, '%' and duplicate slashes are
// rejected up front as defense in depth.
std::filesystem::path WebSocketSession::resolve_file(const std::string& target) {
    namespace fs = std::filesystem;

    std::string p = target;
    auto frag = p.find_first_of("?#");
    if (frag != std::string::npos) p = p.substr(0, frag);
    if (p.empty() || p == "/") p = "/index.html";

    if (p.front() != '/' ||
        p.find('\\') != std::string::npos ||
        p.find('%') != std::string::npos ||
        p.find("//") != std::string::npos) {
        return {};
    }

    std::string rel = p.substr(1);
    size_t start = 0;
    while (start <= rel.size()) {
        size_t slash = rel.find('/', start);
        std::string part = rel.substr(start,
            slash == std::string::npos ? std::string::npos : slash - start);
        if (part == ".." || part == ".") return {};
        if (slash == std::string::npos) break;
        start = slash + 1;
    }

    std::error_code ec;
    fs::path root = fs::weakly_canonical(web_dir_, ec);
    if (ec) return {};
    fs::path full = fs::weakly_canonical(root / rel, ec);
    if (ec) return {};

    std::string rs = root.string();
    const char sep = static_cast<char>(fs::path::preferred_separator);
    if (!rs.empty() && rs.back() != sep) rs.push_back(sep);
    if (full.string().rfind(rs, 0) != 0) return {};

    if (!fs::is_regular_file(full, ec) || ec) return {};
    return full;
}

std::string WebSocketSession::get_content_type(const std::string& path) {
    auto dot = path.find_last_of('.');
    std::string ext = (dot == std::string::npos)
        ? std::string() : path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "html") return "text/html; charset=utf-8";
    if (ext == "css")  return "text/css; charset=utf-8";
    if (ext == "js")   return "application/javascript; charset=utf-8";
    if (ext == "json") return "application/json";
    if (ext == "ico")  return "image/x-icon";
    if (ext == "png")  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "svg")  return "image/svg+xml";
    return "application/octet-stream";
}

void WebSocketSession::handle_http_request() {
    res_ = std::make_shared<http::response<http::string_body>>();

    std::filesystem::path file = resolve_file(std::string(req_.target()));
    std::ifstream in(file, std::ios::binary);
    if (!file.empty() && in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        res_->body() = ss.str();
        res_->result(http::status::ok);
        res_->set(http::field::content_type, get_content_type(file.string()));
    } else {
        res_->result(http::status::not_found);
        res_->set(http::field::content_type, "text/plain; charset=utf-8");
        res_->body() = "Not found";
    }

    res_->version(req_.version());
    res_->set(http::field::server, "GomokuAI");
    res_->set(http::field::connection, "close");
    res_->prepare_payload();

    send_http_response();
}

void WebSocketSession::send_http_response() {
    http::async_write(
        ws_.next_layer(), *res_,
        [self = shared_from_this()](beast::error_code, std::size_t) {
            beast::error_code ec;
            self->ws_.next_layer().socket().shutdown(tcp::socket::shutdown_both, ec);
            self->ws_.next_layer().socket().close(ec);
        });
}

GameServer::GameServer(int port, const std::string& web_dir,
                       const std::string& book_path,
                       const ThreatCoefficients* coefficients,
                       const std::string& sessions_dir,
                       const std::string& records_path)
    : ioc_(), acceptor_(net::make_strand(ioc_)),
      signals_(ioc_, SIGINT, SIGTERM), web_dir_(web_dir),
      sessions_dir_(sessions_dir), records_path_(records_path), flush_timer_(ioc_) {
    if (coefficients) {
        coefficients_ = *coefficients;
        use_loaded_coefficients_ = true;
    }
    std::string error;
    if (book_path.empty()) {
        std::cout << "Opening book disabled" << std::endl;
    } else if (book_.load(book_path, error)) {
        std::cout << "Opening book loaded: " << book_path
                  << " (" << book_.line_count() << " lines, "
                  << book_.entry_count() << " entries)" << std::endl;
    } else {
        std::cout << "Opening book unavailable (" << error
                  << "); search will handle all positions" << std::endl;
    }

    tcp::endpoint endpoint(tcp::v4(), port);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(net::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(net::socket_base::max_listen_connections);
}

void GameServer::start() {
    std::cout << "Server started on port " << acceptor_.local_endpoint().port() << std::endl;
    std::cout << "Open http://localhost:" << acceptor_.local_endpoint().port() << " in your browser" << std::endl;
    if (!sessions_dir_.empty() && !prepare_dir(sessions_dir_, "会话目录")) {
        sessions_dir_.clear();
        records_path_.clear();
    }
    // A records path with no directory part is relative to the working directory,
    // which needs nothing created for it.
    const std::string records_dir = records_path_.empty()
        ? std::string() : std::filesystem::path(records_path_).parent_path().string();
    if (!records_dir.empty() && !prepare_dir(records_dir, "记录目录")) {
        records_path_.clear();
    }
    if (!sessions_dir_.empty()) {
        std::cout << "Sessions saved under " << sessions_dir_
                  << ", finished games appended to " << records_path_ << std::endl;
    } else {
        std::cout << "Session persistence disabled" << std::endl;
    }

    // SIGINT/SIGTERM are handled on the io thread instead of a raw std::signal
    // handler, which cannot safely touch server state (and whose handler would
    // race with ioc_.run()). Stopping the acceptor rejects new connections and
    // ioc_.stop() releases run() even with a client still connected.
    signals_.async_wait([this](const beast::error_code& ec, int) {
        if (ec) return;
        std::cout << "\nShutting down..." << std::endl;
        stop();
    });

    load_saved_sessions();
    schedule_flush();
    do_accept();
    ioc_.run();
}

void GameServer::stop() {
    // Whatever the ten-second tick has not written yet goes out now, so a
    // Ctrl+C between two moves does not lose the move that was already played.
    flush_all();
    beast::error_code ec;
    acceptor_.close(ec);
    flush_timer_.cancel();
    signals_.cancel();
    ioc_.stop();
}

void GameServer::do_accept() {
    acceptor_.async_accept(
        net::make_strand(ioc_),
        [this](beast::error_code ec, tcp::socket socket) {
            if (ec) {
                if (ec != net::error::operation_aborted) {
                    std::cerr << "Accept error: " << ec.message() << std::endl;
                }
                return;
            }
            
            auto session = std::make_shared<WebSocketSession>(std::move(socket), web_dir_);
            on_connect(session);
            session->start();
            do_accept();
        });
}

void GameServer::on_connect(std::shared_ptr<WebSocketSession> session) {
    session->on_message = [this](std::shared_ptr<WebSocketSession> s, const json& msg) {
        on_message(s, msg);
    };
    session->on_close = [this](std::shared_ptr<WebSocketSession> s) {
        on_disconnect(s.get());
    };
    // A plain HTTP request never reaches this callback, so static asset loads
    // (index.html/app.js/style.css, plus favicon probes) do not each allocate
    // a GameManager and its multi-tens-of-MB transposition table.
    session->on_open = [this](std::shared_ptr<WebSocketSession> s) {
        on_open(s);
    };
}

// A handshake alone is not a game: the browser opens a socket before it knows
// whether it is resuming one, so the GameManager is built by the first message
// that needs it (or by `resume`), not here. Otherwise every reconnect, every
// duplicate tab and every page that only probes the port would allocate an
// engine and its transposition table.
void GameServer::on_open(std::shared_ptr<WebSocketSession> session) {
    (void)session;
}

// The socket going away no longer ends the game. It stops the search that was
// writing into this socket, flushes what the game has, and leaves the session in
// the table so `resume` with the same token can hand it to a new socket.
void GameServer::on_disconnect(WebSocketSession* session) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    detach_socket(session);
}

void GameServer::on_message(std::shared_ptr<WebSocketSession> session, const json& msg) {
    handle_message(session, msg);
}

void GameServer::handle_message(std::shared_ptr<WebSocketSession> session, const json& msg) {
    if (!msg.contains("type") || !msg["type"].is_string()) {
        send_error(session, "Missing or invalid message type");
        return;
    }
    
    std::string type = msg["type"];

    if (type == "resume") {
        handle_resume(session, msg);
        return;
    }
    if (type != "ping" && type != "check_position") {
        // Every game message is played in a session: the socket's own if it
        // already has one, a new one otherwise. `ping` is excluded because a
        // keepalive is not a player, and `resume` because it brings its own.
        // `check_position` is excluded because it is a dry run: opening a
        // session for it would hand the client a fresh token and lose the
        // game it was about to set a position in.
        if (!ensure_attached(session)) return;
    }

    if (type == "choose_side") {
        handle_choose_side(session, msg);
    } else if (type == "check_position") {
        handle_check_position(session, msg);
    } else if (type == "set_position") {
        handle_set_position(session, msg);
    } else if (type == "make_move") {
        handle_make_move(session, msg);
    } else if (type == "undo") {
        handle_undo(session);
    } else if (type == "request_replay") {
        handle_request_replay(session);
    } else if (type == "request_hint") {
        handle_request_hint(session, msg);
    } else if (type == "resign") {
        handle_resign(session);
    } else if (type == "stop_thinking") {
        handle_stop_thinking(session);
    } else if (type == "set_config") {
        handle_set_config(session, msg);
    } else if (type == "ping") {
        // Keepalive. Receiving anything already slid the session's idle deadline
        // forward, so the reply exists only to let the client tell a live socket
        // from one whose peer has quietly gone.
        json reply;
        reply["type"] = "pong";
        send_message(session, reply);
    } else if (type == "restart") {
        handle_restart(session);
    } else {
        send_error(session, "Unknown message type: " + type);
    }
}

void GameServer::send_message(std::shared_ptr<WebSocketSession> session, const json& msg) {
    session->send(msg);
}

void GameServer::send_error(std::shared_ptr<WebSocketSession> session, const std::string& message) {
    json msg;
    msg["type"] = "error";
    msg["message"] = message;
    send_message(session, msg);
}

void GameServer::handle_choose_side(std::shared_ptr<WebSocketSession> session, const json& msg) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    if (it == games_.end()) return;
    
    auto& game = it->second;
    // A hand-set position is the one case where stones are already on the board
    // and the side choice is still open: set_position deliberately leaves the
    // game unstarted, so picking a side is how the player starts it.
    if (game->is_ai_thinking() || (game->board().move_count() > 0 && !game->position_pending())) {
        send_error(session, "Cannot choose side after the game has started");
        return;
    }
    // A malformed or unknown color must not silently mean WHITE: the client
    // would get a "playing" state_sync with inverted board colors and no error.
    // And it must not touch the board either: validating before the reset below
    // keeps a rejected "red" from wiping a finished game's state.
    if (!msg.contains("color") || !msg["color"].is_string()) {
        send_error(session, "choose_side requires a \"color\" of \"black\" or \"white\"");
        return;
    }
    std::string color_str = msg["color"].get<std::string>();
    if (color_str != "black" && color_str != "white") {
        send_error(session, "Invalid color: " + color_str);
        return;
    }
    // An empty board can still be in GAME_OVER status if the previous game
    // was resigned before any move was made. Without clearing that status
    // here, make_move would reject every subsequent player move (its status
    // guard short-circuits first) and the AI worker would also refuse to
    // commit its move, so the session would lock up immediately.
    if (game->status() == GameStatus::GAME_OVER) {
        game->reset();
        // The export stamp describes the game that just ended - a cold resume sets
        // it from the saved record - and this entry keeps living, so leaving it true
        // would keep the next finished game out of records.jsonl.
        if (auto entry = entry_locked(session.get())) entry->exported = false;
    }
    int color = (color_str == "black") ? BLACK : WHITE;
    
    game->set_player_color(color);
    // A session with no chosen side has no player colour worth saving: the
    // GameManager always has a default, and writing that out would resume the
    // player into a game they never claimed a side in.
    if (auto entry = entry_locked(session.get())) entry->side_chosen = true;
    
    json response;
    response["type"] = "state_sync";
    response["board"] = board_to_json(game->board());
    response["current_turn"] = game->current_turn() == BLACK ? "black" : "white";
    response["game_status"] = "playing";
    send_message(session, response);
    
    if (game->current_turn() == game->ai_color()) {
        game->start_ai_thinking();
    }
}

// A dry run: recognise and validate a position without touching the session's
// game. The reply carries ok / counts / side to move / the encoding that was
// read, which is what the UI shows the player for the confirmation step. A
// rejected position answers with ok:false and the reason rather than an error
// message, so a form can display it inline.
void GameServer::handle_check_position(std::shared_ptr<WebSocketSession> session, const json& msg) {
    json doc;
    std::string error;
    json reply;
    reply["type"] = "position_preview";
    if (!read_position_payload(msg, doc, error)) {
        reply["ok"] = false;
        reply["error"] = error;
        send_message(session, reply);
        return;
    }

    SetupResult result = position_from_json(doc);
    reply["ok"] = result.ok();
    if (!result.ok()) {
        reply["error"] = result.error;
        send_message(session, reply);
        return;
    }
    reply["black_count"] = result.black_count;
    reply["white_count"] = result.white_count;
    reply["side_to_move"] = side_name(result.side_to_move);
    // Only a grid has symbols to echo back; a bitboard or move list carries no
    // mapping, and empty strings there would read as "the marks are blank".
    if (doc.contains("board")) {
        reply["encoding"] = {{"black", result.encoding.black},
                             {"white", result.encoding.white},
                             {"empty", result.encoding.empty}};
    }
    reply["board"] = board_to_json(result.board);
    send_message(session, reply);
}

// Install the position as the session's board. Deliberately does not start the
// game: the client stays in the waiting state, no AI search is armed, and the
// player then chooses a side (or plays the side to move) to begin.
void GameServer::handle_set_position(std::shared_ptr<WebSocketSession> session, const json& msg) {
    json doc;
    std::string error;
    if (!read_position_payload(msg, doc, error)) {
        send_error(session, error);
        return;
    }
    // The second confirmation the UI owes a board file. Symbols are the only
    // form where the reading can disagree with what the player meant, so a
    // grid-shaped document has to state its mapping - either in the file
    // itself, which is why the shipped templates carry an `encoding`, or in the
    // message after the player confirms the one check_position reported.
    if (doc.contains("board") && !doc.contains("encoding")) {
        send_error(session, "棋盘文件需要先确认黑/白/空标志：请先用 check_position 取得识别结果，"
                            "再在 set_position 里带上 encoding");
        return;
    }

    SetupResult result = position_from_json(doc);
    if (!result.ok()) {
        send_error(session, result.error);
        return;
    }

    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    if (it == games_.end()) return;

    auto& game = it->second;
    if (game->is_ai_thinking()) {
        send_error(session, "AI 正在思考，请先停止再摆棋");
        return;
    }
    // Settle the previous game before this position wipes it: the 10s tick and
    // the detach may both land after the reset, losing the finished game from
    // the session file and from records.jsonl.
    if (auto entry = entry_locked(session.get())) flush_entry(entry);
    if (!game->set_position(result.board)) {
        send_error(session, "无法摆上此局面");
        return;
    }

    // No worker exists now (set_position joined both), so reading the board and
    // the turn here cannot race a commit.
    if (auto entry = entry_locked(session.get())) {
        // The client clears its chosen side when a position lands, and the loaded
        // position has no side until someone takes one: keeping the old claim in
        // the session file would resume the player into a colour they no longer
        // hold.
        entry->side_chosen = false;
        // Same for the export stamp: the game that was appended to
        // records.jsonl is over, and the position starts a fresh one.
        entry->exported = false;
    }
    json reply;
    reply["type"] = "position_set";
    reply["board"] = board_to_json(game->board());
    reply["current_turn"] = side_name(game->current_turn());
    reply["game_status"] = "waiting";
    reply["position_pending"] = true;
    reply["black_count"] = result.black_count;
    reply["white_count"] = result.white_count;
    send_message(session, reply);
}

void GameServer::handle_make_move(std::shared_ptr<WebSocketSession> session, const json& msg) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    if (it == games_.end()) return;
    
    auto& game = it->second;
    int row = get_int(msg, "row", -1);
    int col = get_int(msg, "col", -1);
    
    if (row < 0 || row >= 15 || col < 0 || col >= 15) {
        send_error(session, "Invalid move position");
        return;
    }

    // make_move only ever commits a stone for player_color_ (it rejects any call
    // when current_turn() != player_color_), and on a pending position that call
    // is also what claims the side, so the colour is read once the move is in.
    // current_turn() is still not read here: it would touch board state outside
    // make_move's ai_thinking_ handshake, racing the AI worker's board write under
    // thinking_mutex_.
    if (!game->make_move(row, col)) {
        send_error(session, "Invalid move");
        return;
    }
    const int placed_color = game->player_color();
    // A played stone claims a side even without choose_side: resume/takeover
    // guards and snapshots key off side_chosen, and a game the AI is actually
    // playing in must be re-armed after a reconnect.
    if (auto entry = entry_locked(session.get())) entry->side_chosen = true;

    json response;
    response["type"] = "move_made";
    response["row"] = row;
    response["col"] = col;
    response["color"] = placed_color == BLACK ? "black" : "white";
    send_message(session, response);
}

void GameServer::handle_undo(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    if (it == games_.end()) return;
    
    auto& game = it->second;
    int undo_move_count = 0;
    GameStatus undo_status = GameStatus::WAITING;
    if (game->undo(undo_move_count, undo_status)) {
        // The exported flag describes the game that just gave back a stone:
        // whatever follows this board is a new game, and a second finished
        // game on the same token owes records.jsonl a second export.
        if (auto entry = entry_locked(session.get())) entry->exported = false;
        json response;
        response["type"] = "undo_success";
        response["move_count"] = undo_move_count;
        if (undo_status == GameStatus::GAME_OVER) {
            response["game_status"] = "game_over";
        } else if (undo_status == GameStatus::WAITING) {
            response["game_status"] = "waiting";
        } else {
            response["game_status"] = "playing";
        }
        send_message(session, response);
    } else {
        send_error(session, "Cannot undo");
    }
}

void GameServer::handle_request_replay(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    if (it == games_.end()) return;

    const ReplaySnapshot snapshot = it->second->replay_snapshot();

    json moves_json = json::array();
    for (const MoveAnalysis& analysis : snapshot.moves) {
        json entry;
        entry["row"] = analysis.move.pos.row;
        entry["col"] = analysis.move.pos.col;
        entry["color"] = analysis.move.color == BLACK ? "black" : "white";
        entry["score"] = analysis.score;
        entry["depth"] = analysis.depth;
        entry["nodes"] = analysis.nodes;
        entry["elapsed_ms"] = analysis.elapsed_ms;
        entry["method"] = analysis.method;
        moves_json.push_back(entry);
    }
    // The stones a hand-set position laid down. Without them the move list is not
    // a board: a client rebuilding its display from `replay` alone would drop the
    // position the game was played on.
    json base_json = json::array();
    for (const Move& stone : snapshot.base) {
        base_json.push_back({{"row", stone.pos.row},
                             {"col", stone.pos.col},
                             {"color", side_name(stone.color)}});
    }

    json response;
    response["type"] = "replay";
    response["moves"] = moves_json;
    response["base"] = base_json;
    const bool game_over = snapshot.status == GameStatus::GAME_OVER;
    response["game_over"] = game_over;
    if (!game_over) {
        response["winner"] = "none";
    } else {
        response["winner"] = snapshot.winner == EMPTY ? "draw"
                           : (snapshot.winner == BLACK ? "black" : "white");
    }
    response["reason"] = snapshot.reason;
    send_message(session, response);
}

void GameServer::handle_request_hint(std::shared_ptr<WebSocketSession> session, const json& msg) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    if (it == games_.end()) return;
    
    auto& game = it->second;
    const int timeout_ms = get_int_or(msg, "timeout_ms", game->timeout_ms());
    const int max_depth = get_int_or(msg, "max_depth", game->max_depth());

    if (!game->request_hint(timeout_ms, max_depth)) {
        send_error(session, "No hint available");
    }
}

void GameServer::handle_resign(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    if (it == games_.end()) return;

    // A game that was never started has no side to resign: the opening actions
    // are choosing a colour or playing the first stone, and settling an untouched
    // board would hand the opponent a win over zero moves. Only the analysis list
    // counts as a start - the board's stone count also covers 摆棋 base stones,
    // which undo refuses to give back for the same reason.
    const std::shared_ptr<TrackedGame> entry = entry_locked(session.get());
    const bool started = entry && (entry->side_chosen ||
                                   !entry->game->replay_snapshot().moves.empty());
    if (!started) {
        send_error(session, "Game not started");
        return;
    }

    auto& game = it->second;
    game->resign(game->player_color());
}

void GameServer::handle_stop_thinking(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    if (it == games_.end()) return;
    
    it->second->stop_thinking();
}

void GameServer::handle_set_config(std::shared_ptr<WebSocketSession> session, const json& msg) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    if (it == games_.end()) return;
    
    auto& game = it->second;
    const int timeout_ms = get_int_or(msg, "timeout_ms", game->timeout_ms());
    const int max_depth = get_int_or(msg, "max_depth", game->max_depth());
    game->set_config(timeout_ms, max_depth);
}

void GameServer::handle_restart(std::shared_ptr<WebSocketSession> session) {
    std::shared_ptr<TrackedGame> fresh;
    {
        std::lock_guard<std::mutex> lock(games_mutex_);
        const std::shared_ptr<TrackedGame> previous = entry_locked(session.get());
        const ReplaySnapshot replay =
            previous ? previous->game->replay_snapshot() : ReplaySnapshot();
        // 重开 on a board nobody has touched needs no new session: the token and
        // the file already describe nothing, so reusing them keeps a page that
        // presses the button twice from leaving a trail of empty sessions.
        const bool nothing_to_keep = replay.move_count == 0 && replay.base.empty() &&
                                     previous && !previous->side_chosen &&
                                     !previous->game->position_pending();
        if (nothing_to_keep) {
            previous->game->reset();
            // Back to "nobody has claimed a side", which is also what the client's
            // resetGame does with its own colour.
            previous->side_chosen = false;
        } else {
            // The game being left behind goes to disk before this socket stops
            // being able to reach it: detach_socket flushes, and the entry stays
            // resumable by its token for the rest of the reattach window.
            if (previous) detach_socket(session.get());
            fresh = open_session(nullptr);
            attach_socket(session, fresh);
        }
    }

    json response;
    // Emit game_restarted, not state_sync: the client routes state_sync to
    // handleStateSync, which hard-codes gameStatus='playing' and would clobber
    // the reset. game_restarted drives resetGame() (side buttons re-enabled,
    // waiting state); the next choose_side re-arms the AI, so a BLACK-AI
    // restart does not need an inline start_ai_thinking here.
    response["type"] = "game_restarted";
    response["game_status"] = "waiting";
    send_message(session, response);
    if (fresh) send_session_state(session, fresh, false);
}

// The session this socket plays in, opened on demand so a client that never
// learns about tokens still gets a game the moment it asks for one.
std::shared_ptr<GameServer::TrackedGame> GameServer::ensure_attached(
    std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    if (const std::shared_ptr<TrackedGame> existing = entry_locked(session.get())) {
        return existing;
    }
    std::shared_ptr<TrackedGame> entry = open_session(nullptr);
    attach_socket(session, entry);
    send_session_state(session, entry, false);
    return entry;
}

std::shared_ptr<GameServer::TrackedGame> GameServer::entry_locked(WebSocketSession* session) {
    const auto token_it = socket_tokens_.find(session);
    if (token_it == socket_tokens_.end()) return nullptr;
    const auto entry_it = sessions_.find(token_it->second);
    if (entry_it == sessions_.end()) return nullptr;
    return entry_it->second;
}

// Build a session, either empty or from a snapshot that came off disk. The
// caller holds games_mutex_ and attaches the socket afterwards.
std::shared_ptr<GameServer::TrackedGame> GameServer::open_session(const SessionSnapshot* from) {
    auto entry = std::make_shared<TrackedGame>();
    entry->created_ms = now_ms();
    entry->updated_ms = entry->created_ms;

    auto game = std::make_shared<GameManager>();
    if (use_loaded_coefficients_) game->set_coefficients(coefficients_);
    game->set_opening_book(&book_);

    bool unreadable_record = false;
    if (from) {
        GameRestore restore;
        restore.base = from->base;
        restore.moves.reserve(from->record.moves.size());
        for (const MoveRecord& record : from->record.moves) {
            restore.moves.push_back(analysis_of(record));
        }
        restore.player_color = side_of(from->player_color);
        restore.status = status_of(from->status);
        restore.winner = side_of(from->winner);
        restore.reason = from->reason;
        restore.position_pending = from->position_pending;
        // A record the board cannot be rebuilt from is not a game worth
        // resuming; the caller still gets a playable session either way, and the
        // file stays on disk untouched for a human to look at.
        if (!game->restore_game(restore)) {
            std::cerr << "会话 " << from->token << " 的记录摆不出来，改为新开一局" << std::endl;
            game->reset();
            // A new game on a fresh clock: the file's own creation time belongs
            // to the record that could not be rebuilt, not to this one.
            unreadable_record = true;
        } else {
            entry->side_chosen = !from->player_color.empty();
            entry->exported = from->record.is_decisive();
            entry->rebuilt = true;
            // The file keeps its own birth time across a cold rebuild, otherwise
            // every resume would rewrite the header of the record it just read.
            if (from->created_ms > 0) entry->created_ms = from->created_ms;
            if (from->timeout_ms > 0 && from->max_depth > 0) {
                game->set_config(from->timeout_ms, from->max_depth);
            }
            SessionSnapshot probe = *from;
            probe.created_ms = 0;
            probe.updated_ms = 0;
            entry->saved_payload = session_to_json(probe);
        }
    }
    entry->game = game;

    // Tokens name files, so a collision would overwrite another session. A
    // session rebuilt from disk keeps the token it was read under: the caller
    // already proved that token is not in sessions_, and the player holding the
    // old URL must land on this game rather than a blank one.
    if (from) {
        entry->token = from->token;
    } else {
        std::string token = make_token();
        while (sessions_.count(token) || cold_sessions_.count(token)) token = make_token();
        entry->token = std::move(token);
    }
    sessions_[entry->token] = entry;
    if (unreadable_record) {
        entry->unreadable_record = true;
        // Stamp the fresh empty game with exactly what a flush would write for
        // it, so flush_entry's dedup returns early instead of taking its
        // empty-board branch - that branch deletes the residue file, and this
        // token's file is the unreadable record a human is meant to be able to
        // open. Once the player actually moves here the payloads differ again and
        // the session is written as usual.
        SessionSnapshot empty_probe = snapshot_of(*entry);
        empty_probe.created_ms = 0;
        empty_probe.updated_ms = 0;
        entry->saved_payload = session_to_json(empty_probe);
    }
    return entry;
}

void GameServer::attach_socket(const std::shared_ptr<WebSocketSession>& session,
                              const std::shared_ptr<TrackedGame>& entry) {
    entry->attached = session.get();
    entry->detached_ms = 0;
    games_[session.get()] = entry->game;
    socket_tokens_[session.get()] = entry->token;
    setup_game_callbacks(session, *entry->game);
}

void GameServer::detach_socket(WebSocketSession* session) {
    games_.erase(session);
    const auto token_it = socket_tokens_.find(session);
    if (token_it == socket_tokens_.end()) return;
    const std::string token = token_it->second;
    socket_tokens_.erase(token_it);

    const auto entry_it = sessions_.find(token);
    if (entry_it == sessions_.end()) return;
    TrackedGame& entry = *entry_it->second;
    // A tab that was taken over keeps landing here when its close arrives, and
    // the game now belongs to the newer socket.
    if (entry.attached != session) return;
    entry.attached = nullptr;
    entry.detached_ms = now_ms();
    // The player's voice goes with the socket, and a search still running is
    // writing into it, so it is stopped and joined before the callbacks drop.
    // The game itself is left exactly where the last committed stone put it:
    // whoever resumes finds the same position, and re-arms the AI if it was the
    // AI's move.
    entry.game->halt_search();
    clear_game_callbacks(*entry.game);
    flush_entry(entry_it->second);
}

void GameServer::send_session_state(const std::shared_ptr<WebSocketSession>& session,
                                   const std::shared_ptr<TrackedGame>& entry, bool resumed) {
    const ReplaySnapshot replay = entry->game->replay_snapshot();
    // A chosen side is a started game: choose_side's own reply already says so, and
    // the manager only promotes its status once a stone lands or the AI is armed,
    // so a game the human opens - still empty, nothing armed - would otherwise be
    // reported as waiting and the page would refuse the first click.
    GameStatus reported = replay.status;
    if (entry->side_chosen && reported == GameStatus::WAITING) {
        reported = GameStatus::PLAYING;
    }
    json reply;
    reply["type"] = "session";
    reply["token"] = entry->token;
    reply["resumed"] = resumed;
    reply["player_color"] = entry->side_chosen ? side_name(entry->game->player_color())
                                               : std::string();
    reply["game_status"] = status_name(reported);
    reply["current_turn"] = side_name(replay.move_count % 2 == 0 ? BLACK : WHITE);
    reply["position_pending"] = entry->game->position_pending();
    reply["move_count"] = replay.move_count;
    reply["winner"] = replay.status == GameStatus::GAME_OVER
                          ? (replay.winner == EMPTY ? "draw" : side_name(replay.winner))
                          : std::string();
    reply["reason"] = replay.reason;
    reply["timeout_ms"] = entry->game->timeout_ms();
    reply["max_depth"] = entry->game->max_depth();
    reply["board"] = board_to_json(board_of(replay));
    send_message(session, reply);
}

void GameServer::handle_resume(std::shared_ptr<WebSocketSession> session, const json& msg) {
    std::string token;
    if (msg.contains("token") && msg["token"].is_string()) {
        token = msg["token"].get<std::string>();
    }
    std::shared_ptr<TrackedGame> entry;
    bool resumed = false;
    {
        std::lock_guard<std::mutex> lock(games_mutex_);
        // A socket that already played a game and is now asking for another one
        // leaves that game where it was: flushed, and open to whoever holds its
        // token.
        detach_socket(session.get());

        if (valid_token(token)) {
            const auto hot = sessions_.find(token);
            if (hot != sessions_.end()) {
                entry = hot->second;
                resumed = true;
                if (entry->attached && entry->attached != session.get()) {
                    // Last claim wins, and the tab that loses one gets an error on
                    // its next read rather than a silent freeze: closing it here is
                    // what produces that. Its own detach lands later and leaves the
                    // session alone, because the entry is about to name this socket.
                    WebSocketSession* previous = entry->attached;
                    entry->attached = nullptr;
                    entry->detached_ms = now_ms();
                    games_.erase(previous);
                    socket_tokens_.erase(previous);
                    previous->close();
                    // The search that tab left behind is stopped before this
                    // socket takes over its callbacks: the worker reads them while
                    // it searches, so swapping them mid-flight would be a race, and
                    // the re-armed search below is what makes the AI pick the
                    // position back up.
                    entry->game->halt_search();
                }
            } else {
                const auto cold = cold_sessions_.find(token);
                if (cold != cold_sessions_.end()) {
                    const SessionSnapshot& saved = cold->second;
                    // A file with nothing in it is not a game to come back to.
                    const bool has_game = !saved.base.empty() ||
                                          !saved.record.moves.empty() ||
                                          !saved.player_color.empty();
                    entry = open_session(&saved);
                    cold_sessions_.erase(cold);
                    resumed = has_game && entry->rebuilt;
                }
            }
        }
        if (!entry) {
            entry = open_session(nullptr);
            resumed = false;
        }
        attach_socket(session, entry);
    }

    // A game whose AI was mid-search when the socket went was stopped with it, so
    // the position it comes back to still has the AI to move. Re-arming here is
    // what makes a refresh land on "AI 思考中" rather than a board waiting on a
    // side that never plays. However, a session where nobody has chosen a side
    // yet must not start searching: set_position deliberately leaves status
    // WAITING until side choice, and arming the AI would violate that invariant.
    GameManager& game = *entry->game;
    if (resumed && entry->side_chosen && !game.position_pending() &&
        !game.is_ai_thinking() && game.status() != GameStatus::GAME_OVER &&
        game.current_turn() == game.ai_color()) {
        game.start_ai_thinking();
    }
    send_session_state(session, entry, resumed);
}

void GameServer::load_saved_sessions() {
    if (sessions_dir_.empty()) return;
    std::lock_guard<std::mutex> lock(games_mutex_);
    for (const std::string& path : list_sessions(sessions_dir_)) {
        const std::string token = std::filesystem::path(path).stem().string();
        SessionSnapshot snap;
        std::string error;
        if (!valid_token(token) || !load_session(sessions_dir_, token, snap, error)) {
            std::cerr << "忽略读不出来的会话文件 " << path << ": " << error << std::endl;
            continue;
        }
        cold_sessions_[token] = std::move(snap);
    }
    if (!cold_sessions_.empty()) {
        std::cout << "Loaded " << cold_sessions_.size() << " saved session(s) from "
                  << sessions_dir_ << std::endl;
    }
}

SessionSnapshot GameServer::snapshot_of(const TrackedGame& entry) const {
    SessionSnapshot snap;
    snap.token = entry.token;
    snap.created_ms = entry.created_ms;
    snap.updated_ms = entry.updated_ms;
    GameManager& game = *entry.game;
    const ReplaySnapshot replay = game.replay_snapshot();
    snap.status = status_name(replay.status);
    snap.position_pending = game.position_pending();
    snap.timeout_ms = game.timeout_ms();
    snap.max_depth = game.max_depth();
    snap.player_color = entry.side_chosen ? side_name(game.player_color()) : std::string();
    if (replay.status == GameStatus::GAME_OVER) {
        snap.winner = replay.winner == EMPTY ? "draw" : side_name(replay.winner);
    }
    snap.reason = replay.reason;
    snap.base = replay.base;

    GameRecord& record = snap.record;
    record.moves.clear();
    record.moves.reserve(replay.moves.size());
    record.total_nodes = 0;
    record.max_depth = 0;
    for (const MoveAnalysis& analysis : replay.moves) {
        record.total_nodes += analysis.nodes;
        if (analysis.depth > record.max_depth) record.max_depth = analysis.depth;
        record.moves.push_back(record_of(analysis));
    }
    record.winner = replay.winner;
    record.reason = replay.status == GameStatus::GAME_OVER ? end_reason_of(replay.reason)
                                                           : GameEndReason::INVALID;
    return snap;
}

void GameServer::flush_entry(const std::shared_ptr<TrackedGame>& entry) {
    if (sessions_dir_.empty()) return;
    const SessionSnapshot snap = snapshot_of(*entry);
    // The timestamps say when the write happened, not what changed, so the
    // comparison is made against a copy with them cleared: a session nobody has
    // moved in gets rewritten once rather than every tick.
    SessionSnapshot probe = snap;
    probe.created_ms = 0;
    probe.updated_ms = 0;
    const std::string payload = session_to_json(probe);
    if (payload == entry->saved_payload) return;
    // Content decides: a game with no stone anywhere - base or played - is not
    // kept, even if a side was chosen and the game resigned at move zero. A
    // settled previous game on restart always has stones, so it still lands.
    if (snap.base.empty() && snap.record.moves.empty()) {
        // A token that opened on an unreadable record keeps that file until this
        // game has content: choosing a side is not content, and the stamp stops
        // the next tick from re-entering here.
        if (entry->unreadable_record) {
            entry->saved_payload = payload;
            return;
        }
        // And a file from an earlier game on this token must not outlive the
        // wipe: undo back to an empty board deletes the residue, or a restart
        // would resurrect the undone game. Only commit the dedup stamp once the
        // delete actually succeeded, so a failure retries on the next tick.
        std::string remove_error;
        if (!delete_session(sessions_dir_, entry->token, remove_error)) {
            std::cerr << "会话 " << entry->token << " 残留文件删除失败: "
                      << remove_error << std::endl;
            return;
        }
        entry->saved_payload = payload;
        return;
    }

    SessionSnapshot to_write = snap;
    to_write.created_ms = entry->created_ms;
    to_write.updated_ms = now_ms();
    entry->updated_ms = to_write.updated_ms;

    std::string error;
    if (!save_session(sessions_dir_, to_write, error)) {
        // saved_payload keeps its old value: the next tick sees the difference
        // and retries, instead of treating a failed write as saved forever.
        std::cerr << "会话 " << entry->token << " 落盘失败: " << error << std::endl;
        return;
    }
    // Only a game that started from an empty board is a training record on its
    // own: one that began from a hand-set position needs its `base` stones too,
    // which the trainer's reader has no way to place. So those stay in the
    // session file, for review and for resuming, and out of the record stream.
    if (!entry->exported && to_write.base.empty() && to_write.record.is_decisive()) {
        if (!records_path_.empty()) {
            if (!to_write.record.append_to_jsonl(records_path_, error)) {
                std::cerr << "对局记录追加失败: " << error << std::endl;
                return;
            }
        }
        entry->exported = true;
    }
    // Commit the dedup stamp only once both the file write and the export are
    // done: committing it before the append meant a failed records.jsonl write
    // was never retried, and the game vanished from the training stream.
    entry->saved_payload = payload;
}

void GameServer::flush_all() {
    std::lock_guard<std::mutex> lock(games_mutex_);
    for (const auto& token_entry : sessions_) {
        flush_entry(token_entry.second);
    }
}

void GameServer::schedule_flush() {
    flush_timer_.expires_after(std::chrono::seconds(kFlushIntervalSeconds));
    flush_timer_.async_wait([this](const beast::error_code& ec) {
        if (ec) return;
        on_flush_tick();
        schedule_flush();
    });
}

void GameServer::on_flush_tick() {
    std::lock_guard<std::mutex> lock(games_mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        flush_entry(it->second);
        TrackedGame& entry = *it->second;
        const bool overdue = !entry.attached && entry.detached_ms > 0 &&
                             now_ms() - entry.detached_ms >= kReattachWindowSeconds * 1000LL;
        if (overdue) {
            // Back onto the shelf. The file is current (flush_entry just ran), so
            // what is dropped is the engine, not the game: the token still
            // resumes, by rebuilding from that file.
            cold_sessions_[entry.token] = snapshot_of(entry);
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

json GameServer::board_to_json(const Board& board) {
    json board_json = json::array();
    for (int i = 0; i < 15; ++i) {
        json row = json::array();
        for (int j = 0; j < 15; ++j) {
            row.push_back(board.get(i, j));
        }
        board_json.push_back(row);
    }
    return board_json;
}

// The game outlives the socket, but its voice does not: a worker that posts into
// a closed stream is at best noise in the log. Only ever called after the
// search has been joined, so no thread is reading these while they are cleared.
void GameServer::clear_game_callbacks(GameManager& game) {
    game.set_progress_callback(nullptr);
    game.set_move_callback(nullptr);
    game.set_hint_callback(nullptr);
    game.set_game_over_callback(nullptr);
}

void GameServer::setup_game_callbacks(std::shared_ptr<WebSocketSession> session, GameManager& game) {
    game.set_progress_callback([this, session](int depth, int nodes, int elapsed_ms) {
        json msg;
        msg["type"] = "thinking_progress";
        msg["depth"] = depth;
        msg["nodes"] = nodes;
        msg["elapsed_ms"] = elapsed_ms;
        send_message(session, msg);
    });
    
    game.set_move_callback([this, session](const Move& move, int score, int thinking_time_ms) {
        json msg;
        msg["type"] = "ai_move";
        msg["row"] = move.pos.row;
        msg["col"] = move.pos.col;
        msg["color"] = move.color == BLACK ? "black" : "white";
        msg["score"] = score;
        msg["thinking_time_ms"] = thinking_time_ms;
        send_message(session, msg);
    });
    
    game.set_hint_callback([this, session](const Move& move, int score) {
        json msg;
        if (move.pos.is_valid()) {
            msg["type"] = "hint_result";
            msg["row"] = move.pos.row;
            msg["col"] = move.pos.col;
            msg["score"] = score;
        } else {
            msg["type"] = "error";
            msg["message"] = "No hint available";
        }
        send_message(session, msg);
    });

    game.set_game_over_callback([this, session](int winner, const std::string& reason) {
        json msg;
        msg["type"] = "game_over";
        if (winner == EMPTY) {
            msg["winner"] = "draw";
        } else {
            msg["winner"] = winner == BLACK ? "black" : "white";
        }
        msg["reason"] = reason;
        send_message(session, msg);
        // Settle the game the moment it ends, not at the next 10s tick or a
        // detach: a set_position in between would otherwise wipe the result
        // from both the session file and records.jsonl. This callback can run
        // while the worker holds thinking_mutex_ (or, on resign, while the io
        // thread holds games_mutex_), so the flush is posted to the io thread
        // where games_mutex_ is taken first. flush_entry is content-deduped and
        // respects the exported flag, so tick/detach stay cheap.
        net::post(ioc_, [this, session] {
            std::lock_guard<std::mutex> lock(games_mutex_);
            if (auto entry = entry_locked(session.get())) flush_entry(entry);
        });
    });
}

}

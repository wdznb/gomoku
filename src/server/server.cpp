#include "server.h"
#include "position_json.h"
#include "training/json_int.h"
#include "training/tactics_card.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <csignal>
#include <chrono>
#include <cctype>
#include <algorithm>
#include <atomic>
#include <map>
#include <random>
#include "param_schema.h"  // 戊-2：参数清册就是这一份表（占掉原来那行空行，为了不把下面被按行号指认的那些行推走）
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
        std::string text = value.get<std::string>();
        // Same rule as position_from_text: the three bytes at the very start are a
        // BOM, not content, and leaving them makes json::parse discard a legal
        // document, which then takes the grid branch below.
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF) {
            text.erase(0, 3);
        }
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
    entry.partial_depth = analysis.partial_depth;
    entry.root_moves_done = analysis.root_moves_done;
    entry.root_moves_total = analysis.root_moves_total;
    entry.seldepth = analysis.seldepth;
    entry.stopped = analysis.stopped;
    entry.tt_hits = analysis.tt_hits;
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
    analysis.partial_depth = entry.partial_depth;
    analysis.root_moves_done = entry.root_moves_done;
    analysis.root_moves_total = entry.root_moves_total;
    analysis.seldepth = entry.seldepth;
    analysis.stopped = entry.stopped;
    analysis.tt_hits = entry.tt_hits;
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
// drop one idle timeout after the page loaded no matter how busy the game was.
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
    // The cap on a buffered inbound message is stated here rather than left to
    // whatever the Boost build happens to default it to, because do_read()
    // accumulates a fragmented message whole before any handler runs - the 225-stone
    // and timeout checks downstream come too late. The number is the frame size the
    // position loader already assumes on this wire (position_json.cpp's "The wire
    // allows a 16 MiB frame"); past it Beast fails the read with message_too_big and
    // the existing error path drops the socket.
    ws_.read_message_max(16u * 1024u * 1024u);

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
            if (self->send_queue_.empty() && self->close_after_drain_) {
                self->drop_socket();
                return;
            }
            self->do_write();
        });
}

void WebSocketSession::close() {
    // Deferred behind the write queue: a frame sent just before this (the
    // takeover notice) must leave first. A plain TCP drop here would discard
    // the queue, and Beast's close handshake cannot order it either - its
    // close op takes the write lock the moment the in-flight write ends and
    // runs before the write completion handler (write.hpp unlocks wr_block and
    // invokes op_close first), preempting everything still queued.
    net::post(ws_.get_executor(), [self = shared_from_this()] {
        if (!self->send_queue_.empty()) {
            self->close_after_drain_ = true;
            return;
        }
        self->drop_socket();
    });
}

void WebSocketSession::drop_socket() {
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
    // `..` is refused by name because it asks for a file outside web_dir_. A
    // single `.` is the opposite: it names this directory, so `/./index.html`
    // is index.html and must be served. It is left in place - weakly_canonical
    // below collapses it - and the canonical path is then required to start
    // with web_dir_, which is what actually decides the matter.
    size_t start = 0;
    while (start <= rel.size()) {
        size_t slash = rel.find('/', start);
        std::string part = rel.substr(start,
            slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..") return {};
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

    // One port, two protocols: a browser GETs index.html/app.js/style.css from
    // web_dir_ and then upgrades the WebSocket, so a plain request is only ever
    // a request for a static file. A HEAD asks for the headers of that same
    // file and is answered with no body; any other method is not a read this
    // handler knows how to serve, and handing back the file would tell the
    // client its write had been accepted.
    const bool is_get = req_.method() == http::verb::get;
    const bool is_head = req_.method() == http::verb::head;

    if (is_get || is_head) {
        std::filesystem::path file = resolve_file(std::string(req_.target()));
        std::ifstream in(file, std::ios::binary);
        if (!file.empty() && in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            if (is_get) res_->body() = ss.str();
            res_->result(http::status::ok);
            res_->set(http::field::content_type, get_content_type(file.string()));
        } else {
            res_->result(http::status::not_found);
            res_->set(http::field::content_type, "text/plain; charset=utf-8");
            if (is_get) res_->body() = "Not found";
        }
    } else {
        res_->result(http::status::method_not_allowed);
        res_->set(http::field::content_type, "text/plain; charset=utf-8");
        res_->set(http::field::allow, "GET, HEAD");
        res_->body() = "Method Not Allowed";
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
                       const CoefficientPair* coefficients,
                       const std::string& sessions_dir,
                       const std::string& records_path,
                       int ponder_percent)
    : ioc_(), acceptor_(net::make_strand(ioc_)),
      signals_(ioc_, SIGINT, SIGTERM), web_dir_(web_dir),
      sessions_dir_(sessions_dir), records_path_(records_path), flush_timer_(ioc_),
      accept_retry_timer_(ioc_) {
    // Assigned in the body rather than the init list: the member is declared ahead of
    // the two timers, and an init list that runs out of declaration order is a warning
    // the next reader would have to reason about for no benefit.
    ponder_percent_ = ponder_percent < 0 ? 0 : (ponder_percent > 100 ? 100 : ponder_percent);
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
    // 退出前把每一局的复盘线程收回来。线程只在两手之间读停旗，所以这里最多等一手的
    // 一次定预算搜索；不等它就把 sessions_ 拆掉，等于在 joinable 的 std::thread 上
    // 析构，那是 std::terminate —— 整个进程，不是这一局。
    {
        std::lock_guard<std::mutex> lock(games_mutex_);
        for (const auto& pair : sessions_) stop_analyze(*pair.second);
    }
    beast::error_code ec;
    acceptor_.close(ec);
    flush_timer_.cancel();
    accept_retry_timer_.cancel();
    signals_.cancel();
    ioc_.stop();
}

void GameServer::do_accept() {
    acceptor_.async_accept(
        net::make_strand(ioc_),
        [this](beast::error_code ec, tcp::socket socket) {
            if (ec) {
                // Only a shutdown ends the accept loop. Everything else is
                // transient - no_descriptors and network_down come and go, and a
                // client that resets mid-accept is one connection, not a dead
                // server - so the loop re-arms itself rather than leaving the
                // listening socket with nothing waiting on it. The timer paces the
                // retry so a failure that does NOT go away cannot spin the loop.
                if (ec == net::error::operation_aborted) return;
                std::cerr << "Accept error: " << ec.message() << std::endl;
                accept_retry_timer_.expires_after(std::chrono::milliseconds(100));
                accept_retry_timer_.async_wait([this](beast::error_code wait_ec) {
                    if (!wait_ec) do_accept();
                });
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
    // 槽位按裸指针索引；这一句不清，下一个复用了同一块地址的 socket 会替上一个把子落下。
    drop_pending_move_locked(session);
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
    if (type != "ping" && type != "check_position" && type != "config_schema") {
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
    } else if (type == "analyze_request") {
        // 复盘面板（设计文档 v2 §6.9）：会话级帧，所以它走的是上面那条
        // ensure_attached 的路径，不进 :705 那个「不需要会话」的白名单。
        handle_analyze_request(session, msg);
    } else if (type == "set_config") {
        handle_set_config(session, msg);
    } else if (type == "config_schema" || type == "config_set" || type == "config_preview") { handle_config_frame(session, msg);
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

    if (it->second->is_ai_thinking()) {
        send_error(session, "AI 正在思考，请先停止再摆棋");
        return;
    }
    // Settle the previous game before this position wipes it: the 10s tick and
    // the detach can both land after the reset, and a game with stones is its
    // own only copy - a hand-set base keeps its record out of records.jsonl, so
    // <token>.json is all there is. Rotating it onto its own token first is the
    // same reuse-vs-rotate decision handle_restart makes; README:541-543 counts
    // content as 落子/摆子/选边, so a board with none of the three - which includes
    // a position hand-set with zero stones - has nothing to keep and is merely
    // flushed in place.
    if (const std::shared_ptr<TrackedGame> previous = entry_locked(session.get())) {
        const ReplaySnapshot replay = previous->game->replay_snapshot();
        const bool nothing_to_keep = replay.move_count == 0 && replay.base.empty() &&
                                     !previous->side_chosen;
        if (nothing_to_keep) {
            flush_entry(previous);
        } else {
            // detach_socket flushes the old game under its own token; the fresh
            // token goes out right away, or the client keeps resuming the
            // settled record with the token it still holds.
            detach_socket(session.get());
            const std::shared_ptr<TrackedGame> fresh = open_session(nullptr);
            attach_socket(session, fresh);
            send_session_state(session, fresh, false);
        }
    }
    // The rotation above replaces this socket's game, so the lookup is redone
    // before the position is installed.
    it = games_.find(session.get());
    if (it == games_.end()) return;
    auto& game = it->second;
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
        // This position is a new game on a fresh clock: the old creation time
        // belongs to the record that was just replaced, and leaving it would
        // date the new file's header before its own board. The cold-rebuild
        // path resets the same pair.
        entry->created_ms = now_ms();
        entry->updated_ms = entry->created_ms;
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
    const int row = get_int(msg, "row", -1);
    const int col = get_int(msg, "col", -1);

    // #81 尾窗：AI 的子已经交出（`ai_move` 就在前面几行发出去了）、但 `ai_thinking_` 还没
    // 交回 —— 这一段里到达的合法子今天被 make_move 的守卫一拒就再无机会。这里把它存一手，
    // 等 worker 交回标志之后由 io 线程取走。
    //
    // 判「要不要存货」的这一段不能抱着 games_mutex_ 走出函数：当场执行那条路要进
    // do_make_move，而它自己拿那把锁。
    bool kept = false;   // 已经进队列，回帧由执行的那一刻补
    bool full = false;   // 队列里已经有一手，第二手是真拒绝
    unsigned long long drain_now = 0;   // 非 0 ⇒ 入队正赶上标志交回，当场自己补一次取件
    {
        std::lock_guard<std::mutex> lock(games_mutex_);
        auto it = games_.find(session.get());
        if (it == games_.end()) return;
        if (row >= 0 && row < 15 && col >= 0 && col < 15 &&
            it->second->is_ai_thinking() && it->second->answer_delivered()) {
            if (pending_moves_.count(session.get())) {
                full = true;
            } else {
                pending_moves_[session.get()] = PendingMove{
                    [this, session, row, col]() { do_make_move(session, row, col); },
                    ++pending_seq_};
                kept = true;
                // 交回标志可能正好落在「我读标志」与「我入队」之间 —— 那一次的取件单已经跑完，
                // 不自己补一次取件就再没人来取。锁下重读一次标志，真错过了就当场补。
                if (!it->second->is_ai_thinking()) drain_now = ++pending_seq_;
            }
        }
    }

    if (row < 0 || row >= 15 || col < 0 || col >= 15) {
        send_error(session, "Invalid move position");
        return;
    }
    if (full) {
        send_error(session, "Invalid move");
        return;
    }
    if (drain_now) {
        apply_pending_move(session.get(), drain_now);
        return;
    }
    if (kept) return;
    do_make_move(session, row, col);
}

void GameServer::apply_pending_move(WebSocketSession* key, unsigned long long stamp) {
    std::function<void()> intent;
    {
        std::lock_guard<std::mutex> lock(games_mutex_);
        auto pending = pending_moves_.find(key);
        if (pending == pending_moves_.end()) return;
        // 迟到的取件单：这一手是它之后才存进来的，交给下一次的取件单。
        if (pending->second.stamp > stamp) return;
        intent = std::move(pending->second.intent);
        pending_moves_.erase(pending);
    }
    // io 线程，锁已放下：这一句会走到 make_move，而 make_move 自己就点下一手思考。
    intent();
}

void GameServer::drop_pending_move_locked(WebSocketSession* key) {
    pending_moves_.erase(key);
}

void GameServer::do_make_move(std::shared_ptr<WebSocketSession> session, int row, int col) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    // 排队期间这一局可能已经被拿走（断开、重开）；没有局就没有欠帧。
    if (it == games_.end()) return;
    
    auto& game = it->second;

    // make_move only ever commits a stone for player_color_ (it rejects any call
    // when current_turn() != player_color_), and on a pending position that call
    // is also what claims the side, so the colour is read once the move is in.
    // current_turn() is still not read here: it would touch board state outside
    // make_move's ai_thinking_ handshake, racing the AI worker's board write under
    // thinking_mutex_.
    // The stone can end the game inside make_move, whose callback would then queue
    // game_over ahead of the move_made below; the AI path sends ai_move first, so
    // hold the ending until this frame has gone out.
    {
        std::lock_guard<std::mutex> lock(end_frame_mutex_);
        deferred_end_for_ = session.get();
    }
    if (!game->make_move(row, col)) {
        // Nothing landed, so nothing is owed ahead of a move_made: release the slot
        // and take back whatever a worker parked in it while the move was rejected,
        // so that ending goes out to this session as it did before, not to nobody.
        json held;
        {
            std::lock_guard<std::mutex> lock(end_frame_mutex_);
            deferred_end_for_ = nullptr;
            held.swap(deferred_end_frame_);
        }
        send_error(session, "Invalid move");
        if (!held.is_null()) send_message(session, held);
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
    json deferred;
    {
        std::lock_guard<std::mutex> lock(end_frame_mutex_);
        deferred_end_for_ = nullptr;
        deferred.swap(deferred_end_frame_);
    }
    if (!deferred.is_null()) send_message(session, deferred);
}

void GameServer::handle_undo(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    if (it == games_.end()) return;
    drop_pending_move_locked(session.get());   // 悔棋之后这一手的对象已经不存在了
    
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
        entry["partial_depth"] = analysis.partial_depth;
        entry["root_moves_done"] = analysis.root_moves_done;
        entry["root_moves_total"] = analysis.root_moves_total;
        entry["seldepth"] = analysis.seldepth;
        entry["stopped"] = analysis.stopped;
        entry["tt_hits"] = analysis.tt_hits;
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
    drop_pending_move_locked(session.get());

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
    
    drop_pending_move_locked(session.get());   // 终止 = 这一手不必再落了
    it->second->stop_thinking();
}

void GameServer::handle_set_config(std::shared_ptr<WebSocketSession> session, const json& msg) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    if (it == games_.end()) return;
    
    auto& game = it->second;
    const int timeout_ms = get_int_or(msg, "timeout_ms", game->timeout_ms());
    const int max_depth = get_int_or(msg, "max_depth", game->max_depth());
    // Optional since A7: a frame that names neither keeps the session's budget, and
    // a 0 asks the engine for the behavior every session had before - the node
    // ceiling derived from the remaining time, and no soft window at all.
    const int soft_time_ms = get_int_or(msg, "soft_time_ms", game->soft_time_ms());
    const int max_nodes = get_int_or(msg, "max_nodes", game->max_nodes());
    game->set_config(timeout_ms, max_depth, soft_time_ms, max_nodes);
}

void GameServer::handle_restart(std::shared_ptr<WebSocketSession> session) {
    std::shared_ptr<TrackedGame> fresh;
    {
        std::lock_guard<std::mutex> lock(games_mutex_);
        drop_pending_move_locked(session.get());   // 新局不欠旧局那一手
        const std::shared_ptr<TrackedGame> previous = entry_locked(session.get());
        const ReplaySnapshot replay =
            previous ? previous->game->replay_snapshot() : ReplaySnapshot();
        // 重开 on a board nobody has touched needs no new session: the token and
        // the file already describe nothing, so reusing them keeps a page that
        // presses the button twice from leaving a trail of empty sessions. Nothing
        // touched is README:541-543's three-way test - no stone played, no stone
        // hand-set, no side chosen - and a position hand-set empty is precisely
        // that, so it stays on its own token instead of rotating one per click.
        const bool nothing_to_keep = replay.move_count == 0 && replay.base.empty() &&
                                     previous && !previous->side_chosen;
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
    game->set_ponder(ponder_percent_);

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
            // 0 means the file never stated the key (round-86 P2 sentinel), so each
            // half is restored on its own. set_config clamps both arguments, so the
            // absent half is fed the value this game already has, never 0.
            const int restored_timeout = from->timeout_ms > 0 ? from->timeout_ms : game->timeout_ms();
            const int restored_depth = from->max_depth > 0 ? from->max_depth : game->max_depth();
            game->set_config(restored_timeout, restored_depth);
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
    // old URL must land on this game rather than a blank one. The two cases only
    // diverge when the rebuild FAILED: then the token's file is the unreadable
    // record itself, which stays on disk for a human to look at and must not be
    // written over by this fresh game's flushes - so that one gets a new token,
    // and resumed:false with a new token is what README:487 promises for a
    // record nobody could read.
    if (from && !unreadable_record) {
        entry->token = from->token;
    } else {
        std::string token = make_token();
        while (sessions_.count(token) || cold_sessions_.count(token)) token = make_token();
        entry->token = std::move(token);
    }
    sessions_[entry->token] = entry;
    if (unreadable_record) {
        // Stamp the fresh empty game with exactly what a flush would write for
        // it, so flush_entry's dedup returns early instead of taking its
        // empty-board branch - that branch deletes the file named by the token,
        // and this game has not created one yet. The unreadable record itself
        // stays on disk under its own, now unused, token.
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
    // 复盘线程发的是「这一局我看完了」，而这句话现在属于另一个标签页（或者谁都不属于），
    // 所以在停 AI 之前先把它收回来。它从不取 games_mutex_，因此这里 join 不会与 io 线程互等。
    stop_analyze(entry);
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
                    // Last claim wins. The loser is told why before it is dropped:
                    // without the notice its client reconnects and resumes the
                    // same token, and the two tabs evict each other in a loop,
                    // every takeover re-running halt_search and a flush. It then
                    // gets an error on its next read rather than a silent freeze.
                    // Its own detach lands later and leaves the session alone,
                    // because the entry is about to name this socket.
                    WebSocketSession* previous = entry->attached;
                    entry->attached = nullptr;
                    entry->detached_ms = now_ms();
                    games_.erase(previous);
                    socket_tokens_.erase(previous);
                    json taken_over;
                    taken_over["type"] = "session_taken_over";
                    taken_over["token"] = entry->token;
                    previous->send(taken_over);
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

// The export decision, in one place for both flush paths: the record stream is
// appended before a session file is allowed to say the game is over. False means
// the append failed, so the caller must leave the session file where it was.
static bool export_game_record(bool& exported, const SessionSnapshot& to_write,
                               const std::string& records_path, std::string& error) {
    if (!exported && to_write.base.empty() && to_write.record.is_decisive()) {
        if (!records_path.empty()) {
            if (!to_write.record.append_to_jsonl(records_path, error)) {
                std::cerr << "对局记录追加失败: " << error << std::endl;
                return false;
            }
        }
        exported = true;
    }
    return true;
}

bool GameServer::flush_entry(const std::shared_ptr<TrackedGame>& entry) {
    if (sessions_dir_.empty()) return true;
    const SessionSnapshot snap = snapshot_of(*entry);
    // The timestamps say when the write happened, not what changed, so the
    // comparison is made against a copy with them cleared: a session nobody has
    // moved in gets rewritten once rather than every tick.
    SessionSnapshot probe = snap;
    probe.created_ms = 0;
    probe.updated_ms = 0;
    const std::string payload = session_to_json(probe);
    if (payload == entry->saved_payload) return true;
    // Content decides: a game with no stone anywhere - base or played - is not
    // kept, even if a side was chosen and the game resigned at move zero. A
    // settled previous game on restart always has stones, so it still lands.
    if (snap.base.empty() && snap.record.moves.empty()) {
        // And a file from an earlier game on this token must not outlive the
        // wipe: undo back to an empty board deletes the residue, or a restart
        // would resurrect the undone game. Only commit the dedup stamp once the
        // delete actually succeeded, so a failure retries on the next tick.
        // A game that opened on an unreadable record takes this branch too and
        // is safe: it holds a NEW token (see open_session), so the file named
        // here is its own, while the record nobody could read stays under the old
        // token - and deleting a file that is simply not there reports success.
        std::string remove_error;
        if (!delete_session(sessions_dir_, entry->token, remove_error)) {
            std::cerr << "会话 " << entry->token << " 残留文件删除失败: "
                      << remove_error << std::endl;
            return false;
        }
        entry->saved_payload = payload;
        return true;
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
        return false;
    }
    // Only a game that started from an empty board is a training record on its
    // own: one that began from a hand-set position needs its `base` stones too,
    // which the trainer's reader has no way to place. So those stay in the
    // session file, for review and for resuming, and out of the record stream.
    if (!export_game_record(entry->exported, to_write, records_path_, error)) return false;
    // Commit the dedup stamp only once both the file write and the export are
    // done: committing it before the append meant a failed records.jsonl write
    // was never retried, and the game vanished from the training stream.
    entry->saved_payload = payload;
    return true;
}

// Shutdown exports first: a failed append leaves the session file at its
// pre-ending state, so the next start can still write that record.
void GameServer::flush_all() {
    std::lock_guard<std::mutex> lock(games_mutex_);
    for (const auto& token_entry : sessions_) {
        // This snapshot is only the append's; flush_entry takes its own for the
        // file, and a game decided here is decided there - the record cannot grow
        // after the ending, while an undo resets the exported flag.
        const SessionSnapshot snap = snapshot_of(*token_entry.second);
        // The same content-decides rule flush_entry applies to the file: a game with
        // no stone anywhere is not kept, so shutdown must not append it to the record
        // stream either (README:571). flush_entry is still what removes the residue.
        if (snap.base.empty() && snap.record.moves.empty()) {
            flush_entry(token_entry.second);
            continue;
        }
        std::string error;
        if (!export_game_record(token_entry.second->exported, snap, records_path_, error)) {
            continue;
        }
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
        const bool settled = flush_entry(it->second);
        TrackedGame& entry = *it->second;
        const bool overdue = !entry.attached && entry.detached_ms > 0 &&
                             now_ms() - entry.detached_ms >= kReattachWindowSeconds * 1000LL;
        if (!overdue || !settled) {
            // A failed write leaves the entry in the table, so the next tick retries it
            // instead of shelving a game whose file is stale.
            ++it;
            continue;
        }
        const SessionSnapshot shelved = snapshot_of(entry);
        if (shelved.base.empty() && shelved.record.moves.empty() &&
            shelved.player_color.empty()) {
            // Nothing was ever played here, and flush_entry's empty-content branch
            // already deleted this token's file. Shelving the snapshot would only let
            // the old token come back at resume as a resumed:false frame naming the
            // SAME token, while README:487 asks for a new game with a new token;
            // dropping the entry leaves the token unknown both on disk and in memory,
            // so the next resume falls through to open_session(nullptr).
            stop_analyze(entry);
            it = sessions_.erase(it);
            continue;
        }
        // Back onto the shelf. The file is current (flush_entry just settled
        // it), so what is dropped is the engine, not the game: the token still
        // resumes, by rebuilding from that file.
        stop_analyze(entry);
        cold_sessions_[entry.token] = shelved;
        it = sessions_.erase(it);
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
    // 一起清：留着它，一个已经交还出去的 socket 会被别人的思考结果 post 醒一次。
    game.set_think_finished_callback(nullptr);
}

void GameServer::setup_game_callbacks(std::shared_ptr<WebSocketSession> session, GameManager& game) {
    // depth / nodes / elapsed_ms keep the meanings they had when the frame carried
    // only these three. The rest says how far the think got: partial_depth is the
    // layer that was cut (0 = none), the two root counts are how far into it it got,
    // seldepth the deepest quiescence reach, and stopped why it ended - so "ran out
    // of budget inside layer 6" and "had nothing to search" are different frames
    // (design doc 2.3.3, G2).
    game.set_progress_callback([this, session](const SearchStats& stats) {
        json msg;
        msg["type"] = "thinking_progress";
        msg["depth"] = stats.completed_depth;
        msg["nodes"] = static_cast<int>(stats.nodes);
        msg["elapsed_ms"] = stats.elapsed_ms;
        msg["partial_depth"] = stats.partial_depth;
        msg["root_moves_done"] = stats.root_moves_done;
        msg["root_moves_total"] = stats.root_moves_total;
        msg["seldepth"] = stats.seldepth;
        msg["stopped"] = stop_reason_name(stats.stopped);
        send_message(session, msg);
    });
    
    game.set_think_finished_callback([this, session] {
        // 在 worker 线程上被调用：这里只许做一次 post。取队列的那一步必须在 io 线程上跑
        // ——它要调 make_move，而 make_move 会点下一手思考，那一步开头 join 上一个 worker。
        unsigned long long stamp = 0;
        {
            std::lock_guard<std::mutex> lock(games_mutex_);
            stamp = pending_seq_;
        }
        net::post(ioc_, [this, key = session.get(), stamp] { apply_pending_move(key, stamp); });
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
        // A human stone that ends the game is answered by handle_make_move, which
        // arms this deferral and sends the held frame after move_made; every other
        // source (AI, resign, 判负) goes straight out as before.
        bool held = false;
        {
            std::lock_guard<std::mutex> lock(end_frame_mutex_);
            if (deferred_end_for_ == session.get()) {
                deferred_end_frame_ = std::move(msg);
                held = true;
            }
        }
        if (!held) send_message(session, msg);
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

// ── 甲-3 复盘面板：协议侧（设计文档 v2 §6.9，指导书甲-3）──────────────────
namespace {
// 三档默认值就是 §5.3 的 analyze_depth / analyze_budget / analyze_max_candidates，
// 一个不多一个不少。越界按本仓库别处的老规矩折回可表达的范围而不是报错：这三只数字
// 没有「猜错客户端意图」的代价，客户端要 99 层和要 20 层想的是同一件事——往深里想。
constexpr int kAnalyzeDefaultDepth = 8;
constexpr int kAnalyzeDefaultBudget = 20000;
constexpr int kAnalyzeDefaultCandidates = 12;

int clamp_analyze_depth(int depth) {
    if (depth < 1) return 1;
    if (depth > 20) return 20;      // 页面能表达的最大深度
    return depth;
}

int clamp_analyze_budget(int budget) {
    if (budget < 1000) return 1000;
    if (budget > 256000) return 256000;   // §6.5 难度阶梯的顶
    return budget;
}

int clamp_analyze_candidates(int candidates) {
    if (candidates < 0) return 0;
    if (candidates > 64) return 64;
    return candidates;
}

// 后台线程发一帧。socket 已经走了就什么都不做——复盘是「看一眼」的东西，不是玩家
// 点了就欠他的结果，所以失联不报错也不重试。
void send_analysis_frame(const std::weak_ptr<WebSocketSession>& socket, const json& frame) {
    if (auto session = socket.lock()) session->send(frame);
}
}  // namespace

void GameServer::handle_analyze_request(std::shared_ptr<WebSocketSession> session, const json& msg) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    const std::shared_ptr<TrackedGame> entry = entry_locked(session.get());
    if (!entry) return;

    // 复盘要跑真搜索，一局一条线程，所以「AI 正在想这一手」时不接（§6.9 第 1 条写的
    // 就是「一局结束后」）。不排队：面板一次只展示一份复盘，排队等于同一局搜两遍。
    if (entry->game->is_ai_thinking()) {
        send_error(session, "AI 正在思考这一手，复盘会偷走它的时钟；等它落定再点复盘。");
        return;
    }
    if (entry->analyzing) {
        send_error(session, "这一局的复盘还在跑，等它回来再点。");
        return;
    }

    // 摆棋开的局读不出同一个盘面：段 B 的 position_before 从空盘一手一手摆回去，而
    // 摆棋的 base 子不在手数里（README:575 就是这条：这样的局只活在会话文件里，不进
    // 训练流）。复盘用的正是同一条摆法，所以照同一口径拒绝，不猜。
    const SessionSnapshot snap = snapshot_of(*entry);
    if (!snap.base.empty()) {
        send_error(session, "摆棋开的局不能复盘：从空盘摆不回这一局。");
        return;
    }
    if (snap.record.moves.empty()) {
        send_error(session, "这一局还没有可以复盘的手。");
        return;
    }

    AnalysisRequest request;
    request.record = snap.record;
    // 复盘与筛题按一套系数定价，取的是黑方那一半：`coeffs_sha` 报的就是这个数，
    // 缓存键与算它的那套数字因此始终一致。按色的复盘不是丁-3 的范围（指导书 §4
    // 丁-3 实现思路只把「下棋」的一侧交给 `CoefficientPair`），两侧同值时这里与改之前
    // 取到的是同一套。
    request.coeffs = use_loaded_coefficients_ ? coefficients_.black : ThreatCoefficients{};
    request.coeffs_sha = coefficients_fingerprint(request.coeffs);
    request.review.max_depth = clamp_analyze_depth(get_int_or(msg, "depth", analyze_depth_default_));
    request.review.budget = clamp_analyze_budget(get_int_or(msg, "budget", analyze_budget_default_));
    request.max_candidates =
        clamp_analyze_candidates(get_int_or(msg, "max_candidates", analyze_candidates_default_));

    entry->analyze_stop->store(false);
    entry->analyzing = true;
    // 上一份必然已经 join（analyzing 只在 join 时清），所以这里赋值不会撞到一个
    // joinable 的 std::thread 上——那会直接 std::terminate。
    entry->analyze_thread = std::thread(&GameServer::run_analysis,
                                       std::weak_ptr<WebSocketSession>(session),
                                       entry->analyze_stop,
                                       std::move(request));
}

void GameServer::run_analysis(std::weak_ptr<WebSocketSession> socket,
                              std::shared_ptr<std::atomic<bool>> stop_flag,
                              AnalysisRequest request) {
    const ReplayAnalyzer analyzer(request.screen);
    const ReplayReviewer reviewer(request.coeffs, request.review);
    const GameScreen screen = analyzer.screen(request.record);

    // 段 B 只复核段 A 挑出来的手；一帧一手，为的是让人看到「第 23 / 60 手」而不是
    // 一个转圈的图标（§6.9 第 1 条）。
    std::vector<int> candidates;
    for (const PlyScreen& ply : screen.plies) {
        if (ply.candidate) candidates.push_back(ply.ply);
    }
    const int total = static_cast<int>(candidates.size());

    std::map<int, PlyReview> reviewed;
    int done = 0;
    for (const int ply : candidates) {
        if (stop_flag->load()) return;
        PlyReview result = reviewer.review(request.record, ply);
        // 段 C 只给产题的那几手量难度：一手最多六次定预算搜索，全都量等于把一局棋的
        // 复盘变成十分钟。max_candidates 就是 §5.3 那一格，按失分从大到小排。
        reviewed[ply] = std::move(result);
        ++done;
        json frame;
        frame["type"] = "analyze_progress";
        frame["done"] = done;
        frame["total"] = total;
        send_analysis_frame(socket, frame);
    }

    // 难度层只给「值得出题」的那些手，且不超过 max_candidates 手；其余手不跑阶梯，
    // layer 记 unrated，而不是假称它简单。
    std::vector<std::pair<int, int>> ladder_order;   // (loss, ply)，从掉得最狠的开始
    for (const auto& entry : reviewed) {
        const std::string& kind = entry.second.kind;
        if (kind == "none" || kind == "unlisted") continue;
        ladder_order.emplace_back(std::abs(entry.second.loss), entry.first);
    }
    std::sort(ladder_order.begin(), ladder_order.end(),
              [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                  return a.first > b.first;
              });
    int layered = 0;
    std::map<int, int> solve_budgets;
    for (const auto& ranked : ladder_order) {
        if (layered >= request.max_candidates) break;
        if (stop_flag->load()) return;
        const PlyReview& result = reviewed[ranked.second];
        Board board;
        if (reviewer.position_before(request.record, result.ply, board)) {
            // 段 C 问的是「这道题要多少节点才解得出来」，答案按正解（本手最好的那着）算。
            solve_budgets[result.ply] =
                reviewer.difficulty(board, result.side, result.best).solve_budget;
        }
        ++layered;
    }

    // 每手一项，与非候选手一起发回去：前端的标记是按下标贴到胜率曲线上的，缺一项就
    // 会贴错手（指导书甲-3 第 3 条把几何钉死了，这里就把对齐交给帧本身）。
    const PlyReview unreviewed;
    json moves = json::array();
    int cards = 0;
    for (size_t ply = 0; ply < request.record.moves.size(); ++ply) {
        const MoveRecord& played = request.record.moves[ply];
        const auto found = reviewed.find(static_cast<int>(ply));
        const bool judged = found != reviewed.end();
        const PlyReview& result = judged ? found->second : unreviewed;
        const auto budget_it = solve_budgets.find(static_cast<int>(ply));
        const int solve_budget = budget_it == solve_budgets.end() ? 0 : budget_it->second;

        json item;
        item["ply"] = static_cast<int>(ply);
        item["kind"] = judged ? result.kind : std::string("none");
        item["played"] = json::array({played.move.pos.row, played.move.pos.col});
        item["best"] = judged && result.best.pos.is_valid()
                           ? json::array({result.best.pos.row, result.best.pos.col})
                           : json::array({-1, -1});
        item["loss"] = judged ? result.loss : 0;
        item["gap"] = judged ? result.gap : 0;
        // unrated 是诚实的那一个：没跑阶梯的手不假装它简单。
        item["layer"] = !judged ? std::string("unrated")
                                : (solve_budget > 0 ? TacticsCard::layer_for(solve_budget)
                                                    : std::string("unrated"));
        if (judged && result.kind != "none" && result.kind != "unlisted") ++cards;
        moves.push_back(std::move(item));
    }

    json frame;
    frame["type"] = "analyze_result";
    frame["moves"] = std::move(moves);
    frame["coeffs_sha"] = request.coeffs_sha;
    frame["cards"] = cards;
    frame["total_plies"] = screen.total_plies;
    frame["candidates"] = total;
    frame["reviewed"] = static_cast<int>(reviewed.size());
    send_analysis_frame(socket, frame);
}

void GameServer::stop_analyze(TrackedGame& entry) {
    entry.analyze_stop->store(true);
    if (entry.analyze_thread.joinable()) {
        entry.analyze_thread.join();
    }
    entry.analyzing = false;
    // Ready for the next request: the flag the thread pokes is the one it already
    // holds a copy of, and a stale true would make the next review stop on its very
    // first ply.
    entry.analyze_stop->store(false);
}

}

#include "preview_arms.h"  // 针臂的定义在本文件末尾（戊-2）
namespace gomoku {
// 戊-2（v2 §4.5.2、§5；指导书 §4 戊-2 思路 2）：`config_schema` / `config_set` /
// `config_preview` 三条帧都从这一条函数进。分派写在 handle_message 的那一行里而不是另起分支，
// 是因为 `server.cpp` 里 :939-942 与 :1626-1634 两处被 test_server 按行号指认——多一行就会
// 把它们推走（本轮的行号指针见 REVIEW_LOG 对应轮次）。

// 预览的固定针臂：quiet / scattered 各跑两侧，pos24 再跑一次黑方，一共 5 臂。
// 三副盘面都刻意选「没有一手是逼出来的」那种：只有这样才能量出深度/节点预算改变的是什么，
// 而不是让一个杀棋把三条臂的答案压成同一个数。
//
// pos24 这一副与 `tests/pinned_positions.h` 的 `pinned_pos24()` 是同一摆法。两份定义不能各说各话，
// 所以 test_server 里有一条臂逐子比对二者（生产码不引测试头，那等于把压测夹具链进服务端）。
const std::vector<std::pair<Board, int>>& preview_arms() {
    static const std::vector<std::pair<Board, int>> arms = [] {
        const int quiet[][3] = {{7, 7, BLACK}, {7, 8, WHITE}, {8, 8, BLACK},
                                {6, 6, WHITE}, {8, 6, BLACK}, {6, 8, WHITE}};
        const int scattered[][3] = {{3, 3, BLACK}, {11, 4, WHITE}, {4, 10, BLACK}, {13, 12, WHITE},
                                    {7, 2, BLACK}, {2, 12, WHITE}, {12, 7, BLACK}, {8, 9, WHITE}};
        const int pos24[][3] = {
            {9, 11, BLACK}, {0, 14, WHITE}, {10, 2, BLACK}, {6, 6, WHITE},
            {10, 4, BLACK}, {9, 2, WHITE},  {4, 2, BLACK},  {5, 3, WHITE},
            {2, 3, BLACK},  {10, 1, WHITE}, {12, 12, BLACK}, {2, 6, WHITE},
            {1, 8, BLACK},  {14, 8, WHITE}, {4, 1, BLACK},  {13, 4, WHITE},
            {1, 14, BLACK}, {8, 11, WHITE}, {10, 10, BLACK}, {8, 7, WHITE},
            {12, 1, BLACK}, {11, 6, WHITE}, {5, 2, BLACK},   {0, 7, WHITE},
        };
        auto build = [](const int stones[][3], size_t count) {
            Board board;
            for (size_t i = 0; i < count; ++i) {
                board.place_stone(stones[i][0], stones[i][1], stones[i][2]);
            }
            return board;
        };
        const Board quiet_board = build(quiet, sizeof(quiet) / sizeof(quiet[0]));
        const Board scattered_board = build(scattered, sizeof(scattered) / sizeof(scattered[0]));
        const Board pos24_board = build(pos24, sizeof(pos24) / sizeof(pos24[0]));
        std::vector<std::pair<Board, int>> out;
        for (const int side : {BLACK, WHITE}) {
            out.emplace_back(quiet_board, side);
            out.emplace_back(scattered_board, side);
        }
        out.emplace_back(pos24_board, BLACK);
        return out;
    }();
    return arms;
}

// 针臂的节点帽。预览要在同一台机器上跑两遍给出同样的数字，所以按节点定价而不是按时钟
// （指导书 §9.2：搜索类读数的超时就是节点帽）；时钟留着当防死循环的兜底。


void GameServer::handle_config_frame(std::shared_ptr<WebSocketSession> session, const json& msg) {
    const std::string type = msg.value("type", std::string());
    if (type == "config_schema") {
        // 清单是服务端给的，不是前端抄的：前端只按这一份渲染（指导书 戊-2 注意 1 的「零硬编码」）。
        json reply;
        reply["type"] = "config_schema";
        reply["params"] = ParamRegistry::schema_json()["params"];
        send_message(session, reply);
        return;
    }

    std::lock_guard<std::mutex> lock(games_mutex_);
    const std::shared_ptr<TrackedGame> entry = entry_locked(session.get());
    if (!entry) return;
    std::shared_ptr<GameManager> game = entry->game;

    const std::string key = msg.contains("key") && msg["key"].is_string()
                                ? msg["key"].get<std::string>()
                                : std::string();
    json reject;
    reject["type"] = "config_reject";
    reject["key"] = key;
    const ParamSpec* spec = ParamRegistry::find(key);
    if (!spec) {
        // 「这条参数不存在」与「这条越界」必须分开报：前端收到后者会去检查一个根本没有的旋钮。
        reject["why"] = key.empty() ? "config_set 需要点名一条参数"
                                    : ("代码里没有这条参数：" + key);
        send_message(session, reject);
        return;
    }
    ParamCheck checked;
    if (!msg.contains("value")) {
        reject["why"] = std::string("`") + spec->key + "` 需要给一个值";
        send_message(session, reject);
        return;
    }
    if (!ParamRegistry::check(*spec, msg["value"], checked)) {
        reject["why"] = checked.problem;
        send_message(session, reject);
        return;
    }

    if (type == "config_preview") {
        if (game->is_ai_thinking()) {
            // 与复盘同一口径：预览也跑真搜索，玩家那手的时钟不能分给它（指导书 §7 那一行）。
            send_error(session, "AI 正在思考这一手，预览会偷走它的时钟；等它落定再预览。");
            return;
        }
        json preview;
        preview["type"] = "config_preview";
        preview["key"] = spec->key;
        preview["value"] = msg["value"];
        // 只有「针臂真的能表达」的参数可预览：软时限改的是收尾时机、ponder 与表容量要重启，
        // 拿针臂跑它们只会给一个看起来像结论的噪音。
        const bool expressible = std::string(spec->key) == "max_depth" ||
                                 std::string(spec->key) == "timeout_ms" ||
                                 std::string(spec->key) == "max_nodes";
        preview["previewable"] = expressible;
        if (!expressible) {
            preview["why_not"] = "这条参数改变不了固定针臂的读数（" +
                                 std::string(spec->when_live == WhenLive::Restart
                                                    ? "它要重启，进程没起来之前针臂跑不到它"
                                                    : "它只影响收尾时机，而针臂是按节点帽定价的") +
                                 "）";
            send_message(session, preview);
            return;
        }
        int depth = game->max_depth();
        int cap = game->max_nodes();
        if (spec->key == std::string("max_depth")) depth = static_cast<int>(checked.numeric);
        if (spec->key == std::string("max_nodes")) cap = static_cast<int>(checked.numeric);
        if (cap <= 0) cap = kPreviewNodeCap;
        SearchEngine engine(coefficients_);
        engine.set_max_nodes(cap);
        if (use_loaded_coefficients_) engine.set_book_enabled(false);
        std::atomic<bool> stop{false};
        json arms = json::array();
        long long total_nodes = 0;
        long long total_ms = 0;
        long long checksum = 0;
        for (const auto& arm : preview_arms()) {
            const SearchResult result = engine.search(arm.first, arm.second, depth, kPreviewWallMs,
                                                      stop);
            json one;
            one["side"] = arm.second == BLACK ? "black" : "white";
            one["depth"] = result.depth;
            one["nodes"] = result.nodes_searched;
            one["elapsed_ms"] = result.elapsed_ms;
            one["nps"] = result.elapsed_ms > 0
                            ? static_cast<double>(result.nodes_searched) * 1000.0 /
                                  static_cast<double>(result.elapsed_ms)
                            : 0.0;
            one["move"] = std::to_string(result.best_move.pos.row) + "," +
                          std::to_string(result.best_move.pos.col);
            one["score"] = result.score;
            one["stopped"] = result.stopped;
            arms.push_back(one);
            total_nodes += result.nodes_searched;
            total_ms += result.elapsed_ms;
            checksum = checksum * 131 + result.depth * 7 + result.nodes_searched +
                       result.score * 3 + result.best_move.pos.row * 100 + result.best_move.pos.col;
        }
        preview["arms"] = arms;
        preview["total_nodes"] = total_nodes;
        // 墙钟与 nps 只作参考：这台机器的绝对时间不可复现（轮次163 的吞吐表），
        // 所以「两次预览一致」这条判据比的是节点、深度、着法、分值与校验和。
        preview["total_ms"] = total_ms;
        preview["checksum"] = checksum;
        preview["committed"] = false;   // 预览不改任何状态：参数没写进会话，棋局也没动
        send_message(session, preview);
        return;
    }

    // 到这里就是 `config_set`。
    if (spec->when_live == WhenLive::Restart) {
        reject["why"] = std::string("`") + spec->key + "` 要重启进程才生效（" + spec->effect_note +
                        "），面板该把它禁掉而不是改了再告诉你没用";
        send_message(session, reject);
        return;
    }
    const std::string group = spec->group;
    if (group == "play") {
        const int numeric = static_cast<int>(checked.numeric);
        game->set_config(spec->key == std::string("timeout_ms") ? numeric : game->timeout_ms(),
                         spec->key == std::string("max_depth") ? numeric : game->max_depth(),
                         spec->key == std::string("soft_time_ms") ? numeric : game->soft_time_ms(),
                         spec->key == std::string("max_nodes") ? numeric : game->max_nodes());
    } else if (group == "analyze") {
        const int numeric = static_cast<int>(checked.numeric);
        if (spec->key == std::string("analyze_depth")) analyze_depth_default_ = numeric;
        else if (spec->key == std::string("analyze_budget")) analyze_budget_default_ = numeric;
        else analyze_candidates_default_ = numeric;
    } else {
        reject["why"] = std::string("`") + spec->key + "` 这一组服务端不代收（" + group + "）";
        send_message(session, reject);
        return;
    }
    json ack;
    ack["type"] = "config_ack";
    ack["key"] = spec->key;
    ack["value"] = msg["value"];
    ack["when_live"] = ParamRegistry::when_live_name(spec->when_live);
    send_message(session, ack);
}

}  // namespace gomoku

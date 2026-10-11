#include "server.h"
#include "position_json.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <csignal>
#include <chrono>
#include <cctype>

namespace gomoku {

namespace {
// json::value(key, default) only falls back to the default when the key is
// absent; a present-but-wrong-type value (e.g. {"row":"7"}) throws
// type_error, which unwinds past the handler's own validation into do_read's
// generic catch and leaves the client with no reply. Read integers defensively
// so a malformed field is treated as the default and the handler still
// validates and responds.
int get_int(const json& msg, const char* key, int fallback) {
    if (!msg.contains(key) || !msg[key].is_number_integer()) return fallback;
    return msg[key].get<int>();
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
        if (parsed.is_discarded()) {
            // Not JSON, so a plain-text grid: CSV, txt or one symbol per cell.
            doc = json::object();
            doc["board"] = text;
        } else {
            doc = std::move(parsed);
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
}

WebSocketSession::WebSocketSession(tcp::socket&& socket, const std::string& web_dir)
    : ws_(std::move(socket)), web_dir_(web_dir) {
}

void WebSocketSession::start() {
    // The server multiplexes one port: a browser first issues a plain GET for
    // index.html/app.js/style.css (served from web_dir_), then opens the
    // WebSocket on the same origin. Read the HTTP request headers before
    // deciding which path to take; the WebSocket handshake reuses the request
    // we just parsed, so nothing is consumed twice.
    ws_.next_layer().expires_after(std::chrono::seconds(60));
    http::async_read(
        ws_.next_layer(), buffer_, req_,
        [self = shared_from_this()](beast::error_code ec, std::size_t bytes) {
            self->on_http_read(ec, bytes);
        });
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
    beast::error_code ec;
    ws_.close(websocket::close_code::normal, ec);
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
                       const ThreatCoefficients* coefficients)
    : ioc_(), acceptor_(net::make_strand(ioc_)),
      signals_(ioc_, SIGINT, SIGTERM), web_dir_(web_dir) {
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

    // SIGINT/SIGTERM are handled on the io thread instead of a raw std::signal
    // handler, which cannot safely touch server state (and whose handler would
    // race with ioc_.run()). Stopping the acceptor rejects new connections and
    // ioc_.stop() releases run() even with a client still connected.
    signals_.async_wait([this](const beast::error_code& ec, int) {
        if (ec) return;
        std::cout << "\nShutting down..." << std::endl;
        stop();
    });

    do_accept();
    ioc_.run();
}

void GameServer::stop() {
    beast::error_code ec;
    acceptor_.close(ec);
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

void GameServer::on_open(std::shared_ptr<WebSocketSession> session) {
    auto game = std::make_shared<GameManager>();
    if (use_loaded_coefficients_) game->set_coefficients(coefficients_);
    game->set_opening_book(&book_);
    {
        std::lock_guard<std::mutex> lock(games_mutex_);
        games_[session.get()] = game;
    }
    setup_game_callbacks(session, *game);
}

void GameServer::on_disconnect(WebSocketSession* session) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session);
    if (it != games_.end()) {
        it->second->stop_thinking();
        games_.erase(it);
    }
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
    // An empty board can still be in GAME_OVER status if the previous game
    // was resigned before any move was made. Without clearing that status
    // here, make_move would reject every subsequent player move (its status
    // guard short-circuits first) and the AI worker would also refuse to
    // commit its move, so the session would lock up immediately.
    if (game->status() == GameStatus::GAME_OVER) {
        game->reset();
    }
    // A malformed or unknown color must not silently mean WHITE: the client
    // would get a "playing" state_sync with inverted board colors and no error.
    if (!msg.contains("color") || !msg["color"].is_string()) {
        send_error(session, "choose_side requires a \"color\" of \"black\" or \"white\"");
        return;
    }
    std::string color_str = msg["color"].get<std::string>();
    if (color_str != "black" && color_str != "white") {
        send_error(session, "Invalid color: " + color_str);
        return;
    }
    int color = (color_str == "black") ? BLACK : WHITE;
    
    game->set_player_color(color);
    
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
    if (!game->set_position(result.board)) {
        send_error(session, "无法摆上此局面");
        return;
    }

    // No worker exists now (set_position joined both), so reading the board and
    // the turn here cannot race a commit.
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

    // make_move only ever commits a stone for player_color_ (it rejects any
    // call when current_turn() != player_color_). Reading player_color_ avoids
    // touching board state here: current_turn() would read move_count on the
    // io thread outside make_move's ai_thinking_ handshake, racing the AI
    // worker's board write under thinking_mutex_.
    int placed_color = game->player_color();
    if (!game->make_move(row, col)) {
        send_error(session, "Invalid move");
        return;
    }

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

    json response;
    response["type"] = "replay";
    response["moves"] = moves_json;
    response["game_over"] = snapshot.game_over;
    if (!snapshot.game_over) {
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
    int timeout_ms = get_int(msg, "timeout_ms", 5000);
    int max_depth = get_int(msg, "max_depth", 6);
    
    if (!game->request_hint(timeout_ms, max_depth)) {
        send_error(session, "No hint available");
    }
}

void GameServer::handle_resign(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    if (it == games_.end()) return;
    
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
    int timeout_ms = get_int(msg, "timeout_ms", 5000);
    int max_depth = get_int(msg, "max_depth", 8);
    game->set_config(timeout_ms, max_depth);
}

void GameServer::handle_restart(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    if (it == games_.end()) return;
    
    auto& game = it->second;
    game->stop_thinking();
    game->reset();
    
    json response;
    // Emit game_restarted, not state_sync: the client routes state_sync to
    // handleStateSync, which hard-codes gameStatus='playing' and would clobber
    // the reset. game_restarted drives resetGame() (side buttons re-enabled,
    // waiting state); the next choose_side re-arms the AI, so a BLACK-AI
    // restart does not need an inline start_ai_thinking here.
    response["type"] = "game_restarted";
    response["game_status"] = "waiting";
    send_message(session, response);
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
    });
}

}

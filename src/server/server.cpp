#include "server.h"
#include <iostream>
#include <fstream>
#include <sstream>

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
}

WebSocketSession::WebSocketSession(tcp::socket&& socket, const std::string& web_dir)
    : ws_(std::move(socket)), web_dir_(web_dir) {
}

void WebSocketSession::start() {
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res) {
            res.set(boost::beast::http::field::server, "GomokuAI");
        }));
    
    ws_.async_accept(
        [self = shared_from_this()](beast::error_code ec) {
            if (ec) {
                std::cerr << "WebSocket accept error: " << ec.message() << std::endl;
                // The GameManager was already registered in games_ before
                // start() was invoked, so run the close callback here to
                // erase it; otherwise plain TCP probes that never complete
                // the WS handshake leak a session (and its ~96 MB of TTs)
                // each time.
                if (self->on_close) {
                    self->on_close(self);
                }
                return;
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

GameServer::GameServer(int port, const std::string& web_dir,
                       const std::string& book_path)
    : ioc_(), acceptor_(net::make_strand(ioc_)), web_dir_(web_dir) {
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
    do_accept();
    ioc_.run();
}

void GameServer::stop() {
    acceptor_.close();
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
    
    auto game = std::make_shared<GameManager>();
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
    } else if (type == "make_move") {
        handle_make_move(session, msg);
    } else if (type == "undo") {
        handle_undo(session);
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
    if (game->is_ai_thinking() || game->board().move_count() > 0) {
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
    // Defensive read: a present-but-non-string "color" would otherwise throw
    // out of this handler with no reply. Default to black.
    std::string color_str = (msg.contains("color") && msg["color"].is_string())
                                ? msg["color"].get<std::string>()
                                : std::string("black");
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
    if (game->undo()) {
        json response;
        response["type"] = "undo_success";
        response["move_count"] = game->board().move_count();
        if (game->status() == GameStatus::GAME_OVER) {
            response["game_status"] = "game_over";
        } else if (game->status() == GameStatus::WAITING) {
            response["game_status"] = "waiting";
        } else {
            response["game_status"] = "playing";
        }
        send_message(session, response);
    } else {
        send_error(session, "Cannot undo");
    }
}

void GameServer::handle_request_hint(std::shared_ptr<WebSocketSession> session, const json& msg) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    if (it == games_.end()) return;
    
    auto& game = it->second;
    int timeout_ms = get_int(msg, "timeout_ms", 5000);
    int max_depth = get_int(msg, "max_depth", 6);
    
    game->request_hint(timeout_ms, max_depth);
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
    response["type"] = "state_sync";
    response["board"] = board_to_json(game->board());
    response["current_turn"] = "black";
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

json GameServer::move_to_json(const Move& move) {
    json move_json;
    move_json["row"] = move.pos.row;
    move_json["col"] = move.pos.col;
    move_json["color"] = move.color == BLACK ? "black" : "white";
    return move_json;
}

json GameServer::game_state_json(const GameManager& game) {
    json state;
    state["board"] = board_to_json(game.board());
    state["current_turn"] = game.current_turn() == BLACK ? "black" : "white";
    
    switch (game.status()) {
        case GameStatus::WAITING:
            state["game_status"] = "waiting";
            break;
        case GameStatus::PLAYING:
            state["game_status"] = "playing";
            break;
        case GameStatus::GAME_OVER:
            state["game_status"] = "game_over";
            if (game.winner() == EMPTY) {
                state["winner"] = "draw";
            } else {
                state["winner"] = game.winner() == BLACK ? "black" : "white";
            }
            break;
    }
    
    return state;
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
        msg["type"] = "hint_result";
        msg["row"] = move.pos.row;
        msg["col"] = move.pos.col;
        msg["score"] = score;
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

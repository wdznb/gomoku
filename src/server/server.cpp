#include "server.h"
#include <iostream>
#include <fstream>
#include <sstream>

namespace gomoku {

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
    std::lock_guard<std::mutex> lock(send_mutex_);
    bool write_in_progress = !send_queue_.empty();
    send_queue_.push(msg.dump());
    if (!write_in_progress) {
        do_write();
    }
}

void WebSocketSession::do_write() {
    if (send_queue_.empty()) {
        is_writing_ = false;
        return;
    }
    
    is_writing_ = true;
    
    ws_.async_write(
        net::buffer(send_queue_.front()),
        [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "WebSocket write error: " << ec.message() << std::endl;
                return;
            }
            std::lock_guard<std::mutex> lock(self->send_mutex_);
            self->send_queue_.pop();
            self->do_write();
        });
}

void WebSocketSession::close() {
    beast::error_code ec;
    ws_.close(websocket::close_code::normal, ec);
}

GameServer::GameServer(int port, const std::string& web_dir)
    : ioc_(), acceptor_(net::make_strand(ioc_)), web_dir_(web_dir) {
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
    if (!msg.contains("type")) {
        send_error(session, "Missing message type");
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
    std::string color_str = msg.value("color", "black");
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
    int row = msg.value("row", -1);
    int col = msg.value("col", -1);
    
    if (row < 0 || row >= 15 || col < 0 || col >= 15) {
        send_error(session, "Invalid move position");
        return;
    }
    
    if (!game->make_move(row, col)) {
        send_error(session, "Invalid move");
        return;
    }
    
    if (game->status() == GameStatus::GAME_OVER) {
        json response;
        response["type"] = "game_over";
        response["winner"] = game->winner() == BLACK ? "black" : "white";
        response["reason"] = "five";
        send_message(session, response);
    } else if (game->current_turn() == game->ai_color()) {
        game->start_ai_thinking();
    }
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
    int timeout_ms = msg.value("timeout_ms", 5000);
    int max_depth = msg.value("max_depth", 6);
    
    game->request_hint(timeout_ms, max_depth);
}

void GameServer::handle_resign(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(games_mutex_);
    auto it = games_.find(session.get());
    if (it == games_.end()) return;
    
    auto& game = it->second;
    game->resign(game->player_color());
    
    json response;
    response["type"] = "game_over";
    response["winner"] = game->ai_color() == BLACK ? "black" : "white";
    response["reason"] = "resign";
    send_message(session, response);
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
    int timeout_ms = msg.value("timeout_ms", 5000);
    int max_depth = msg.value("max_depth", 8);
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
    response["game_status"] = "playing";
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
            state["winner"] = game.winner() == BLACK ? "black" : "white";
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
    
    game.set_game_over_callback([this, session](int winner, const std::string& reason) {
        json msg;
        msg["type"] = "game_over";
        msg["winner"] = winner == BLACK ? "black" : "white";
        msg["reason"] = reason;
        send_message(session, msg);
    });
}

}

#ifndef GOMOKU_SERVER_H
#define GOMOKU_SERVER_H

#include "game_manager.h"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <nlohmann/json.hpp>
#include <memory>
#include <map>
#include <mutex>
#include <queue>
#include <string>

namespace gomoku {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
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
    
private:
    websocket::stream<beast::tcp_stream> ws_;
    std::string web_dir_;
    beast::flat_buffer buffer_;
    std::queue<std::string> send_queue_;
    bool is_writing_ = false;
    
    void do_read();
    void do_write();
    void handle_http_request(beast::http::request<beast::http::string_body>& req);
    std::string get_content_type(const std::string& path);
    std::string load_file(const std::string& path);
};

class GameServer {
public:
    GameServer(int port = 8080, const std::string& web_dir = "web");
    
    void start();
    void stop();
    
private:
    net::io_context ioc_;
    tcp::acceptor acceptor_;
    std::string web_dir_;
    std::map<WebSocketSession*, std::shared_ptr<GameManager>> games_;
    std::mutex games_mutex_;
    
    void do_accept();
    void on_connect(std::shared_ptr<WebSocketSession> session);
    void on_disconnect(WebSocketSession* session);
    void on_message(std::shared_ptr<WebSocketSession> session, const json& msg);
    
    void handle_message(std::shared_ptr<WebSocketSession> session, const json& msg);
    void send_message(std::shared_ptr<WebSocketSession> session, const json& msg);
    void send_error(std::shared_ptr<WebSocketSession> session, const std::string& message);
    
    void handle_choose_side(std::shared_ptr<WebSocketSession> session, const json& msg);
    void handle_make_move(std::shared_ptr<WebSocketSession> session, const json& msg);
    void handle_undo(std::shared_ptr<WebSocketSession> session);
    void handle_request_hint(std::shared_ptr<WebSocketSession> session, const json& msg);
    void handle_resign(std::shared_ptr<WebSocketSession> session);
    void handle_stop_thinking(std::shared_ptr<WebSocketSession> session);
    void handle_set_config(std::shared_ptr<WebSocketSession> session, const json& msg);
    void handle_restart(std::shared_ptr<WebSocketSession> session);
    
    json board_to_json(const Board& board);
    json move_to_json(const Move& move);
    json game_state_json(const GameManager& game);
    
    void setup_game_callbacks(std::shared_ptr<WebSocketSession> session, GameManager& game);
};

}

#endif

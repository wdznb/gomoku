#ifndef GOMOKU_SERVER_H
#define GOMOKU_SERVER_H

#include "game_manager.h"
#include "../optimization/opening_book.h"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/signal_set.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <queue>
#include <string>

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
    
    void do_read();
    void do_write();
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
    GameServer(int port = 8080, const std::string& web_dir = "web",
               const std::string& book_path = "data/opening_book.json",
               const ThreatCoefficients* coefficients = nullptr);

    void start();
    void stop();

private:
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
    std::mutex games_mutex_;
    
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
    
    json board_to_json(const Board& board);
    
    void setup_game_callbacks(std::shared_ptr<WebSocketSession> session, GameManager& game);
};

}

#endif

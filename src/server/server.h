#ifndef GOMOKU_SERVER_H
#define GOMOKU_SERVER_H

#include "game_manager.h"
#include "owner_checked_mutex.h"
#include "session_store.h"
#include "../optimization/opening_book.h"
#include "../training/game_record.h"
#include "../training/replay_analyzer.h"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/signal_set.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cassert>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
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
               const CoefficientPair* coefficients = nullptr,
               const std::string& sessions_dir = "data/sessions",
               const std::string& records_path = "data/training/records.jsonl",
               // Design doc 2.4.3 ②, server-wide policy: the share of a finished think
               // that its session may spend pre-searching the reply it predicted while
               // the human is thinking. 0 (the default) never arms one - this is a
               // second engine thread per waiting session, which no document prices.
               int ponder_percent = 0);

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
        // 复盘面板（设计文档 v2 §6.9，指导书甲-3）：一局一条后台分析线程。
        // `analyze_stop` is shared rather than a member because the thread reads it
        // through a copy: even if an exit path ever forgot to join, the flag it pokes
        // stays alive, so the worst case is a late frame, not a dangling write.
        std::shared_ptr<std::atomic<bool>> analyze_stop = std::make_shared<std::atomic<bool>>(false);
        std::thread analyze_thread;
        // Set by the io thread when it arms a review, cleared at the join point. A
        // second `analyze_request` while it is up is refused, not queued: the panel
        // shows one review at a time, and a queue would let a tab that clicks twice
        // search the same game twice.
        bool analyzing = false;

        // Destroying a TrackedGame with a live analysis thread left on it would run
        // ~std::thread on a joinable thread, which is std::terminate - the whole
        // process, not this session. Every exit path also calls stop_analyze earlier so
        // the frames stop, but this is the one that cannot be forgotten by a future
        // path.
        ~TrackedGame() {
            analyze_stop->store(true);
            if (analyze_thread.joinable()) analyze_thread.join();
        }
    };

    net::io_context ioc_;
    tcp::acceptor acceptor_;
    net::signal_set signals_;
    std::string web_dir_;
    OpeningBook book_;
    // Trained coefficients from --params. Applied to every new game because each
    // GameManager owns its own engine; the file is read once here, never again.
    // 丁-3：一对——按色的参数文件要能真的按下色去玩。两侧同值时这就是原来那一份。
    CoefficientPair coefficients_;
    bool use_loaded_coefficients_ = false;
    // --ponder, handed to every session the server opens (see the constructor).
    int ponder_percent_ = 0; int analyze_depth_default_ = 8; int analyze_budget_default_ = 20000; int analyze_candidates_default_ = 12;
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
    // Guards every map above. The io thread is the only thread that touches them,
    // and since 轮次199 that is no longer just a comment: this type records the
    // thread that first locks it and asserts every later lock comes from the same
    // one, so a worker-side acquisition fails a check (or the compile) instead of
    // deadlocking the whole server the way 轮次187's did.
    OwnerCheckedMutex games_mutex_;
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
    void handle_resume(std::shared_ptr<WebSocketSession> session, const json& msg); void handle_config_frame(std::shared_ptr<WebSocketSession> session, const json& msg);
    // 复盘面板的请求帧（§6.9）：读参数、取整局快照、起一条后台分析线程。玩家正在
    // 让 AI 思考时直接拒绝，不排队——复盘要跑真搜索，而那一局的时间属于这一手。
    void handle_analyze_request(std::shared_ptr<WebSocketSession> session, const json& msg);
    
    json board_to_json(const Board& board);
    
    // 轮次193 #106：装回调时把「结论回属主线程」的通知也装上。它要一份共享所有权（那
    // 条通知可能排在 detach 之后才跑），所以收的是 entry 而不是 `GameManager&`。
    void setup_game_callbacks(std::shared_ptr<WebSocketSession> session,
                             const std::shared_ptr<TrackedGame>& entry);
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

    // Everything the analysis thread needs, copied out by the io thread before it
    // arms the thread: the record is a snapshot of the replay list, the coefficients
    // are the ones this session plays with, and the two configs are the frame's own
    // numbers. Nothing here points back at the game or the server.
    struct AnalysisRequest {
        GameRecord record;
        ThreatCoefficients coeffs;
        ScreenConfig screen;
        ReviewConfig review;
        int max_candidates = 12;   // §5.3 analyze_max_candidates：段 C 最多给几手定难度
        std::string coeffs_sha;
    };

    // 段 A 筛候选 → 段 B 逐手复核（一手一帧 analyze_progress）→ 段 C 给产题的那几手
    // 定难度层 → 一帧 analyze_result。它从不取 games_mutex_，也不碰 GameManager，所以
    // io 线程可以在持有这把锁时 join 它；它只在两手之间读停旗，一手就是一次定节点预算
    // 的搜索（§9.2：节点帽就是超时，复盘这一路没有墙钟）。
    static void run_analysis(std::weak_ptr<WebSocketSession> socket,
                             std::shared_ptr<std::atomic<bool>> stop_flag,
                             AnalysisRequest request);

    // Stop and join this session's analysis thread. Called with games_mutex_ held by
    // whoever is taking the socket or the entry away: detach_socket, the idle sweep and
    // stop(). Leaving it armed would let a thread keep sending into a socket that
    // already belongs to another tab.
    static void stop_analyze(TrackedGame& entry);
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

    // 今天 handle_make_move 的主体（拒因、帧序、`side_chosen` 落账）。自己拿 `games_mutex_`，
    // 所以调用它的 io 线程那一条路不得抱着那把锁。
    void do_make_move(std::shared_ptr<WebSocketSession> session, int row, int col);

  public:
    // #91：那面内存墙不在「重开/摆棋」那条路上，而在「detach 下来的会话要留满 30 分钟」这条设计上：
    // 每局两条新链接 ⇒ 每局两个会话，实测 +14 MB/局（轮次185），约 250 个会话/台就开始握手超时。
    // 这个名额把「窗口」变成「名额」：越过就把最老的 detach 会话照 overdue 的同一条路走 —— 先结算
    // 落盘，再上架成 cold（token 仍可续，只是从文件重建，引擎丢掉）。
    // 缺省 0 = 不限 = 今天的行为；只有显式 `--max-sessions` 会改它，负数按 0 收。
    void set_max_sessions(int limit) { max_sessions_ = limit < 0 ? 0 : limit; }

  private:
    // 把一条已经**落盘结算好**的会话从内存里摘掉：要么放上冷架（token 之后从文件重建），要么在
    // 「这一局什么都没下过」时直接删掉。调用方负责先 flush —— 没结算成功的不许走到这里。
    // **调用方必须已持 games_mutex_**（on_flush_tick 持着）。
    void shelve_locked(const std::shared_ptr<TrackedGame>& entry);
    int max_sessions_ = 0;
};

}

#endif

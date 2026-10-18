// Boost.Beast WebSocket capacity probe for gomoku_server.
//
// Implements the constraints in docs/PROMPT_STRESS_TEST.md: case A keeps every
// connection alive and paces one {"type":"ping"} per second per connection (S3),
// reconnects only through exponential backoff (S4), re-checks its own four circuit
// breaker conditions every 5 s (S5), drives every connection from one io_context on
// min(cores, 8) threads (S7) and pins a fixed read buffer per connection (S8). It
// changes no system setting (S9) and unwinds every socket before exiting (S10).
//
// Built by build_stress.sh, deliberately outside CMakeLists.txt so the probe is not
// part of the production target list.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using clock_type = std::chrono::steady_clock;

namespace {

constexpr double kBackoffBaseMs = 200.0;   // S4: base 200 ms
constexpr double kBackoffCapMs = 30000.0;  // S4: cap 30 s
constexpr double kBackoffJitter = 0.2;     // S4: +/-20 %
constexpr int kMaxTier = 500;              // S2: hard ceiling, higher needs a human first
constexpr double kMaxOpenPerSecond = 100;  // S3: bound on handshake churn while ramping
constexpr double kMaxHandshakeWindowS = 60;  // S3: 用例 B duration cap
constexpr double kMaxHandshakes = kMaxOpenPerSecond * kMaxHandshakeWindowS;
constexpr double kWindDownGraceS = 15.0;   // a breaker trip must still clean up

struct Options {
    std::string host = "127.0.0.1";
    int port = 8080;
    int tier = 1;
    double ramp_up_s = 10.0;
    double hold_s = 60.0;
    double interval_ms = 1000.0;
    double connect_deadline_s = 5.0;
    double io_deadline_s = 5.0;
    int threads = 0;
    std::size_t read_buffer_bytes = 16 * 1024;
    int max_consecutive_fail = 20;
    double breaker_fail_rate = 0.05;
    long breaker_timewait = 8000;
    double breaker_p99_connect_s = 5.0;
    long breaker_rss_mb = 2048;
    long server_pid = 0;
    bool smoke = false;
    bool handshake_only = false;
    std::string csv_path;
};

struct Metrics {
    std::mutex m;
    uint64_t connect_ok = 0;
    uint64_t connect_fail = 0;
    uint64_t msg_ok = 0;
    uint64_t msg_err = 0;
    int max_consecutive_fail = 0;
    int established_peak = 0;
    std::vector<double> rtt_ms;
    std::vector<double> connect_ms;
    std::vector<double> win_connect_ms;  // 5 s window for the breaker
    uint64_t win_ok = 0;
    uint64_t win_fail = 0;
    // Peak, not the end-of-tier reading: by report time the tier's sockets are gone.
    long server_rss_kb_peak = -1;
    std::size_t server_fds_peak = 0;
    std::string stop_reason;
};

struct Ctx {
    Options opt;
    asio::io_context ioc;
    tcp::endpoint endpoint;
    Metrics metrics;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> opening{false};
    std::atomic<bool> closing{false};
    std::atomic<int> live_conns{0};
    std::atomic<int> established{0};
    // The window the opener actually spent issuing connects, so the 用例 B rate is a
    // measurement rather than ramp_up_s restated as a promise.
    std::atomic<int64_t> first_open_ns{0};
    std::atomic<int64_t> last_open_ns{0};

    void request_stop(const std::string& reason) {
        bool expected = false;
        if (stop_requested.compare_exchange_strong(expected, true)) {
            std::lock_guard<std::mutex> lock(metrics.m);
            if (metrics.stop_reason.empty()) metrics.stop_reason = reason;
        }
    }
};

// A steady timer wants an integral duration; the plan is written in fractional
// seconds, so every wait goes through these two.
std::chrono::nanoseconds seconds_dur(double s) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(s));
}

std::chrono::nanoseconds millis_dur(double ms) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double, std::milli>(ms));
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = (p / 100.0) * static_cast<double>(v.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(idx);
    const std::size_t hi = std::min(lo + 1, v.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

// /proc is the only source read here: no helper process is ever forked while the
// system is under load (S10).
long read_vm_rss_kb(long pid) {
    const std::string path =
        pid > 0 ? "/proc/" + std::to_string(pid) + "/status" : "/proc/self/status";
    std::ifstream in(path);
    std::string line;
    long kb = -1;
    while (std::getline(in, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream parts(line.substr(6));
            if (parts >> kb) break;
        }
    }
    return kb;
}

std::size_t count_open_fds(long pid) {
    if (pid <= 0) return 0;
    const std::string path = "/proc/" + std::to_string(pid) + "/fd";
    DIR* dir = opendir(path.c_str());
    if (!dir) return 0;
    std::size_t n = 0;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) ++n;
    }
    closedir(dir);
    return n;
}

// "TCP: inuse N orphan N tw N alloc N" - tw is the TIME_WAIT the breaker watches (S5).
long read_timewait() {
    std::ifstream in("/proc/net/sockstat");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("TCP:", 0) != 0) continue;
        std::istringstream parts(line);
        std::string token;
        while (parts >> token) {
            if (token == "tw") {
                long value = 0;
                if (parts >> value) return value;
            }
        }
    }
    return -1;
}

class Conn : public std::enable_shared_from_this<Conn> {
public:
    explicit Conn(Ctx& ctx)
        : ctx_(ctx), ws_(ctx.ioc), pace_(ws_.get_executor()), pause_(ws_.get_executor()) {
        read_buf_.reserve(ctx_.opt.read_buffer_bytes);
        // Over budget is an error, not a reason to grow (S8).
        read_buf_.max_size(ctx_.opt.read_buffer_bytes);
        set_deadline(ctx_.opt.connect_deadline_s);
    }

    void start() { attempt_connect(); }

private:
    void set_deadline(double seconds) {
        beast::get_lowest_layer(ws_).expires_after(seconds_dur(seconds));
    }

    // The tcp_stream deadline is one absolute expiry shared by every operation on the
    // socket, so it is armed per round trip: a leftover expiry would kill the next read.
    void clear_deadline() {
        beast::get_lowest_layer(ws_).expires_never();
    }

    // A connection only ever has one outstanding operation, so its handlers cannot
    // interleave across threads and no per-socket strand is needed. Nothing outside
    // a connection ever touches its socket: stopping happens by flag, and each
    // connection unwinds itself at its next handler.
    void attempt_connect() {
        if (ctx_.stop_requested.load() || ctx_.closing.load()) {
            finish();
            return;
        }
        set_deadline(ctx_.opt.connect_deadline_s);
        attempt_started_ = clock_type::now();
        auto self = shared_from_this();
        beast::get_lowest_layer(ws_).async_connect(
            ctx_.endpoint, [self](beast::error_code ec) { self->on_connect(ec); });
    }

    void on_connect(beast::error_code ec) {
        if (ctx_.stop_requested.load() || ctx_.closing.load()) {
            record_connect(false);
            finish();
            return;
        }
        if (ec) {
            record_connect(false);
            schedule_backoff();
            return;
        }
        set_deadline(ctx_.opt.io_deadline_s);
        auto self = shared_from_this();
        ws_.async_handshake(ctx_.endpoint.address().to_string() + ":" +
                                std::to_string(ctx_.opt.port),
                            "/",
                            [self](beast::error_code eco) { self->on_handshake(eco); });
    }

    void on_handshake(beast::error_code ec) {
        if (ctx_.stop_requested.load() || ctx_.closing.load()) {
            record_connect(!ec);
            drop_socket();
            finish();
            return;
        }
        if (ec) {
            record_connect(false);
            drop_socket();
            schedule_backoff();
            return;
        }
        record_connect(true);
        established_ = true;
        consecutive_fail_ = 0;
        backoff_ms_ = kBackoffBaseMs;
        clear_deadline();
        const int now = ++ctx_.established;
        {
            std::lock_guard<std::mutex> lock(ctx_.metrics.m);
            ctx_.metrics.established_peak = std::max(ctx_.metrics.established_peak, now);
        }
        // 用例 B: accept + handshake is the path under test, so the socket is closed
        // gracefully the moment it is established and no message is ever sent.
        if (ctx_.opt.handshake_only) {
            shutdown_normal();
            return;
        }
        send_ping();
    }

    void send_ping() {
        if (ctx_.stop_requested.load()) {
            shutdown_normal();
            return;
        }
        if (ctx_.closing.load()) {
            shutdown_normal();
            return;
        }
        ping_ = R"({"type":"ping"})";
        sent_at_ = clock_type::now();
        // One window for the whole round trip: the write and the read that answers it.
        set_deadline(ctx_.opt.io_deadline_s);
        auto self = shared_from_this();
        ws_.async_write(asio::buffer(ping_),
                        [self](beast::error_code ec, std::size_t) { self->on_write(ec); });
    }

    void on_write(beast::error_code ec) {
        if (ec) {
            fail_round_trip();
            return;
        }
        auto self = shared_from_this();
        ws_.async_read(read_buf_,
                       [self](beast::error_code eco, std::size_t) { self->on_read(eco); });
    }

    void on_read(beast::error_code ec) {
        if (ec) {
            fail_round_trip();
            return;
        }
        const std::string payload = beast::buffers_to_string(read_buf_.data());
        read_buf_.consume(read_buf_.size());
        const bool is_pong = payload.find("\"pong\"") != std::string::npos;
        const double rtt =
            std::chrono::duration<double, std::milli>(clock_type::now() - sent_at_).count();
        {
            std::lock_guard<std::mutex> lock(ctx_.metrics.m);
            if (is_pong) {
                ++ctx_.metrics.msg_ok;
                ctx_.metrics.rtt_ms.push_back(rtt);
            } else {
                ++ctx_.metrics.msg_err;
            }
        }
        if (!is_pong) {
            fail_round_trip();
            return;
        }
        // One message per second per connection: paced, never saturating. In smoke mode
        // the hold timer, not a ping count, ends the connection, so the single socket is
        // required to live past every deadline the client arms.
        clear_deadline();
        auto self = shared_from_this();
        pace_.expires_after(millis_dur(ctx_.opt.interval_ms));
        pace_.async_wait([self](beast::error_code eco) {
            if (!eco) self->send_ping();
        });
    }

    void fail_round_trip() {
        {
            std::lock_guard<std::mutex> lock(ctx_.metrics.m);
            ++ctx_.metrics.msg_err;
        }
        drop_socket();
        if (ctx_.stop_requested.load() || ctx_.closing.load()) {
            finish();
            return;
        }
        // A live socket that went away comes back only through the same backoff, so
        // recovery can never become the zero-delay reconnect loop of 2026-10-16 (S4).
        schedule_backoff();
    }

    void schedule_backoff() {
        ++consecutive_fail_;
        {
            std::lock_guard<std::mutex> lock(ctx_.metrics.m);
            ctx_.metrics.max_consecutive_fail =
                std::max(ctx_.metrics.max_consecutive_fail, consecutive_fail_);
        }
        if (ctx_.stop_requested.load() || ctx_.closing.load()) {
            finish();
            return;
        }
        if (consecutive_fail_ >= ctx_.opt.max_consecutive_fail) {
            ctx_.request_stop("connect_failed_" + std::to_string(consecutive_fail_) +
                              "_in_a_row");
            finish();
            return;
        }
        const double delay = std::min(backoff_ms_, kBackoffCapMs);
        backoff_ms_ = std::min(backoff_ms_ * 2.0, kBackoffCapMs);
        std::uniform_real_distribution<double> spread(1.0 - kBackoffJitter,
                                                      1.0 + kBackoffJitter);
        const double jittered = delay * spread(rng_);
        auto self = shared_from_this();
        pause_.expires_after(millis_dur(jittered));
        pause_.async_wait([self](beast::error_code ec) {
            if (!ec) self->attempt_connect();
        });
    }

    void record_connect(bool ok) {
        const double ms =
            ok ? std::chrono::duration<double, std::milli>(clock_type::now() - attempt_started_)
                       .count()
               : 0.0;
        std::lock_guard<std::mutex> lock(ctx_.metrics.m);
        if (ok) {
            ++ctx_.metrics.connect_ok;
            ++ctx_.metrics.win_ok;
            ctx_.metrics.connect_ms.push_back(ms);
            ctx_.metrics.win_connect_ms.push_back(ms);
        } else {
            ++ctx_.metrics.connect_fail;
            ++ctx_.metrics.win_fail;
        }
    }

    void shutdown_normal() {
        established_ = false;
        --ctx_.established;
        clear_deadline();
        beast::error_code ignored;
        ws_.close(websocket::close_code::normal, ignored);
        drop_socket();
        finish();
    }

    void drop_socket() {
        clear_deadline();
        if (established_) {
            established_ = false;
            --ctx_.established;
        }
        beast::error_code ec;
        ws_.close(websocket::close_code::normal, ec);
        auto& sock = beast::get_lowest_layer(ws_).socket();
        sock.shutdown(tcp::socket::shutdown_both, ec);
        sock.close(ec);
        read_buf_.consume(read_buf_.size());
    }

    void finish() {
        if (done_) return;
        done_ = true;
        pace_.cancel();
        pause_.cancel();
        --ctx_.live_conns;
    }

    Ctx& ctx_;
    websocket::stream<beast::tcp_stream> ws_;
    asio::steady_timer pace_;
    asio::steady_timer pause_;
    beast::flat_buffer read_buf_;
    std::string ping_;
    clock_type::time_point attempt_started_ = clock_type::now();
    clock_type::time_point sent_at_ = clock_type::now();
    int consecutive_fail_ = 0;
    double backoff_ms_ = kBackoffBaseMs;
    bool established_ = false;
    bool done_ = false;
    std::mt19937 rng_{std::random_device{}()};
};

// S5: the client brakes itself. Every 5 s it re-reads the four trip conditions and
// stops the whole run rather than waiting to be told.
struct Breaker {
    Ctx& ctx;
    asio::steady_timer timer;

    explicit Breaker(Ctx& c) : ctx(c), timer(c.ioc) {}

    void start() { arm(); }

    void arm() {
        timer.expires_after(std::chrono::seconds(5));
        timer.async_wait([this](beast::error_code ec) {
            if (ec) return;
            sample();
            if (!ctx.stop_requested.load() && !ctx.closing.load()) arm();
        });
    }

    void sample() {
        double fail_rate = 0.0;
        double p99_connect_s = 0.0;
        std::vector<double> window;
        {
            std::lock_guard<std::mutex> lock(ctx.metrics.m);
            const uint64_t ok = ctx.metrics.win_ok;
            const uint64_t fail = ctx.metrics.win_fail;
            window.swap(ctx.metrics.win_connect_ms);
            ctx.metrics.win_ok = 0;
            ctx.metrics.win_fail = 0;
            const uint64_t attempts = ok + fail;
            if (attempts > 0)
                fail_rate = static_cast<double>(fail) / static_cast<double>(attempts);
            p99_connect_s = percentile(window, 99) / 1000.0;
        }
        const long tw = read_timewait();
        const long rss_mb = read_vm_rss_kb(0) / 1024;
        if (ctx.opt.server_pid > 0) {
            const long s_rss = read_vm_rss_kb(ctx.opt.server_pid);
            const std::size_t s_fds = count_open_fds(ctx.opt.server_pid);
            std::lock_guard<std::mutex> lock(ctx.metrics.m);
            if (s_rss > ctx.metrics.server_rss_kb_peak) ctx.metrics.server_rss_kb_peak = s_rss;
            if (s_fds > ctx.metrics.server_fds_peak) ctx.metrics.server_fds_peak = s_fds;
        }
        std::ostringstream why;
        if (fail_rate > ctx.opt.breaker_fail_rate) {
            why << "breaker_connect_fail_rate_" << static_cast<int>(fail_rate * 1000) << "/1000";
        } else if (tw > ctx.opt.breaker_timewait) {
            why << "breaker_time_wait_" << tw;
        } else if (!window.empty() && p99_connect_s > ctx.opt.breaker_p99_connect_s) {
            why << "breaker_p99_connect_ms_" << static_cast<int>(p99_connect_s * 1000);
        } else if (rss_mb > ctx.opt.breaker_rss_mb) {
            why << "breaker_client_rss_mb_" << rss_mb;
        }
        if (!why.str().empty()) ctx.request_stop(why.str());
    }
};

// Opens one connection per step until the tier is filled, never faster than S3's
// 100 handshakes per second.
struct Opener {
    Ctx& ctx;
    asio::steady_timer timer;
    double step_s = 0.2;
    int opened = 0;

    explicit Opener(Ctx& c) : ctx(c), timer(c.ioc) {}

    void start() {
        step_s = std::max(ctx.opt.ramp_up_s / static_cast<double>(ctx.opt.tier),
                          1.0 / kMaxOpenPerSecond);
        timer.expires_after(std::chrono::milliseconds(1));
        timer.async_wait([this](beast::error_code ec) {
            if (!ec) tick();
        });
    }

    void tick() {
        if (ctx.stop_requested.load() || !ctx.opening.load()) return;
        if (opened >= ctx.opt.tier) {
            ctx.opening = false;
            return;
        }
        ++opened;
        const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   clock_type::now().time_since_epoch())
                                   .count();
        int64_t expected_first = 0;
        ctx.first_open_ns.compare_exchange_strong(expected_first, now_ns);
        ctx.last_open_ns.store(now_ns);
        auto conn = std::make_shared<Conn>(ctx);
        ++ctx.live_conns;
        conn->start();
        if (opened >= ctx.opt.tier) {
            ctx.opening = false;
            return;
        }
        timer.expires_after(seconds_dur(step_s));
        timer.async_wait([this](beast::error_code ec) {
            if (!ec) tick();
        });
    }
};

// Ends the run once every connection has unwound, or when the grace period after the
// planned finish passes - a breaker trip must never leave a socket behind (S10).
struct Reaper {
    Ctx& ctx;
    asio::steady_timer timer;
    clock_type::time_point hard_stop;

    Reaper(Ctx& c, clock_type::time_point deadline)
        : ctx(c), timer(c.ioc), hard_stop(deadline) {}

    void start() { arm(); }

    void arm() {
        timer.expires_after(std::chrono::milliseconds(200));
        timer.async_wait([this](beast::error_code ec) {
            if (ec) return;
            tick();
        });
    }

    void tick() {
        const bool drained = !ctx.opening.load() && ctx.live_conns.load() == 0;
        if (drained || clock_type::now() >= hard_stop) {
            ctx.opening = false;
            ctx.closing = true;
            ctx.ioc.stop();
            return;
        }
        arm();
    }
};

struct Report {
    int tier = 0;
    int peak = 0;
    uint64_t connect_ok = 0;
    uint64_t connect_fail = 0;
    double fail_rate = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    uint64_t msg_ok = 0;
    uint64_t msg_err = 0;
    double throughput = 0.0;
    long client_rss_kb = -1;
    long server_rss_kb = -1;
    std::size_t server_fds = 0;
    std::string reason;
    double seconds = 0.0;
    double open_window_s = 0.0;
};

Report collect(Ctx& ctx, double held_seconds) {
    Report r;
    std::vector<double> rtts;
    {
        std::lock_guard<std::mutex> lock(ctx.metrics.m);
        r.tier = ctx.opt.tier;
        r.peak = ctx.metrics.established_peak;
        r.connect_ok = ctx.metrics.connect_ok;
        r.connect_fail = ctx.metrics.connect_fail;
        r.msg_ok = ctx.metrics.msg_ok;
        r.msg_err = ctx.metrics.msg_err;
        rtts = ctx.metrics.rtt_ms;
        r.server_rss_kb = ctx.metrics.server_rss_kb_peak;
        r.server_fds = ctx.metrics.server_fds_peak;
        r.reason = ctx.metrics.stop_reason;
        if (ctx.metrics.max_consecutive_fail > 0) {
            r.reason +=
                " max_consecutive_fail=" + std::to_string(ctx.metrics.max_consecutive_fail);
        }
    }
    const uint64_t attempts = r.connect_ok + r.connect_fail;
    r.fail_rate =
        attempts ? static_cast<double>(r.connect_fail) / static_cast<double>(attempts) : 0.0;
    // 用例 B sends no message at all, so the only latency that exists is the connect.
    const std::vector<double>& latencies =
        ctx.opt.handshake_only ? ctx.metrics.connect_ms : rtts;
    r.p50 = percentile(latencies, 50);
    r.p95 = percentile(latencies, 95);
    r.p99 = percentile(latencies, 99);
    r.throughput = held_seconds > 0 ? static_cast<double>(r.msg_ok) / held_seconds : 0.0;
    r.client_rss_kb = read_vm_rss_kb(0);
    if (ctx.opt.server_pid > 0) {
        // A tier shorter than the 5 s sample cycle has no peak yet.
        if (r.server_rss_kb < 0) r.server_rss_kb = read_vm_rss_kb(ctx.opt.server_pid);
        if (r.server_fds == 0) r.server_fds = count_open_fds(ctx.opt.server_pid);
    }
    r.seconds = held_seconds;
    const int64_t first_open = ctx.first_open_ns.load();
    const int64_t last_open = ctx.last_open_ns.load();
    r.open_window_s = last_open > first_open
                          ? static_cast<double>(last_open - first_open) / 1e9
                          : held_seconds;
    return r;
}

void print_report(const Report& r, const Ctx& ctx) {
    std::cout << std::fixed << std::setprecision(2) << "TIER " << r.tier
              << " smoke=" << (ctx.opt.smoke ? 1 : 0) << " conns_target=" << ctx.opt.tier
              << " conns_peak=" << r.peak << " connect_ok=" << r.connect_ok
              << " connect_fail=" << r.connect_fail << " fail_rate=" << (r.fail_rate * 100.0)
              << "% open_rate_per_s="
              << (r.open_window_s > 0
                      ? static_cast<double>(r.connect_ok + r.connect_fail) / r.open_window_s
                      : 0.0)
              << " open_window_s=" << r.open_window_s
              << " latency=" << (ctx.opt.handshake_only ? "connect" : "rtt")
              << " p50_ms=" << r.p50 << " p95_ms=" << r.p95 << " p99_ms=" << r.p99
              << " msg_ok=" << r.msg_ok << " msg_err=" << r.msg_err
              << " throughput_mps=" << r.throughput
              << " client_rss_mb=" << r.client_rss_kb / 1024
              << " server_rss_peak_mb=" << (r.server_rss_kb >= 0 ? r.server_rss_kb / 1024 : -1)
              << " server_fds_peak=" << r.server_fds << " time_wait=" << read_timewait()
              << " seconds=" << r.seconds
              << " reason=" << (r.reason.empty() ? "none" : r.reason) << "\n";
}

void append_csv(const Options& opt, const Report& r, int threads) {
    if (opt.csv_path.empty()) return;
    const bool fresh = !std::ifstream(opt.csv_path).good();
    std::ofstream out(opt.csv_path, std::ios::app);
    if (!out) return;
    if (fresh) {
        out << "ts,tier,conns_target,conns_peak,connect_ok,connect_fail,fail_rate_pct,p50_ms,"
               "p95_ms,p99_ms,msg_ok,msg_err,throughput_mps,client_rss_mb,server_rss_peak_mb,"
               "server_fds_peak,time_wait,threads,buffer_bytes_per_conn,seconds,reason\n";
    }
    out << std::time(nullptr) << ',' << r.tier << ',' << opt.tier << ',' << r.peak << ','
        << r.connect_ok << ',' << r.connect_fail << ',' << std::fixed << std::setprecision(3)
        << r.fail_rate * 100.0 << ',' << r.p50 << ',' << r.p95 << ',' << r.p99 << ',' << r.msg_ok
        << ',' << r.msg_err << ',' << r.throughput << ',' << r.client_rss_kb / 1024 << ','
        << (r.server_rss_kb >= 0 ? r.server_rss_kb / 1024 : -1) << ',' << r.server_fds << ','
        << read_timewait() << ',' << threads << ',' << opt.read_buffer_bytes << ',' << r.seconds
        << ',' << (r.reason.empty() ? "none" : r.reason) << '\n';
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto take_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(4);
            }
            return argv[++i];
        };
        auto as_num = [&](const std::string& name) {
            const std::string value = take_value(name.c_str());
            try {
                return std::stod(value);
            } catch (...) {
                std::cerr << "bad number for " << name << ": " << value << "\n";
                std::exit(4);
            }
        };
        if (arg == "--host") {
            opt.host = take_value(arg.c_str());
        } else if (arg == "--port") {
            opt.port = static_cast<int>(as_num(arg));
        } else if (arg == "--tier") {
            opt.tier = static_cast<int>(as_num(arg));
        } else if (arg == "--ramp-up") {
            opt.ramp_up_s = as_num(arg);
        } else if (arg == "--hold") {
            opt.hold_s = as_num(arg);
        } else if (arg == "--interval-ms") {
            opt.interval_ms = as_num(arg);
        } else if (arg == "--threads") {
            opt.threads = static_cast<int>(as_num(arg));
        } else if (arg == "--read-buffer") {
            opt.read_buffer_bytes = static_cast<std::size_t>(as_num(arg));
        } else if (arg == "--server-pid") {
            opt.server_pid = static_cast<long>(as_num(arg));
        } else if (arg == "--max-consecutive-fail") {
            opt.max_consecutive_fail = static_cast<int>(as_num(arg));
        } else if (arg == "--csv") {
            opt.csv_path = take_value(arg.c_str());
        } else if (arg == "--handshake-only") {
            opt.handshake_only = true;
        } else if (arg == "--smoke") {
            opt.smoke = true;
            opt.tier = 1;
            opt.ramp_up_s = 1.0;
            // Long enough to outlive every deadline the client arms, so a socket that
            // survives a 3-ping handshake but dies on a timer cannot pass this gate.
            opt.hold_s = 12.0;
        } else {
            std::cerr << "unknown option " << arg << "\n";
            return false;
        }
    }
    if (opt.handshake_only) {
        // 用例 B: --tier counts total handshakes issued over --ramp-up, not sockets held
        // at once, so S2's 500 does not apply. S3's pair of limits does: <= 100/s and
        // <= 60s, which together bound the run at 6000 connects and therefore at ~6000
        // client-side TIME_WAIT entries.
        if (opt.tier < 1) {
            std::cerr << "--handshake-only needs --tier >= 1 (total connects)\n";
            return false;
        }
        if (opt.ramp_up_s > kMaxHandshakeWindowS) {
            std::cerr << "--handshake-only window must be <=" << kMaxHandshakeWindowS
                      << "s (S3)\n";
            return false;
        }
        if (static_cast<double>(opt.tier) > kMaxHandshakes) {
            std::cerr << "--handshake-only total connects must be <=" << kMaxHandshakes
                      << " (S3: 100/s x 60s)\n";
            return false;
        }
        const double rate = static_cast<double>(opt.tier) / opt.ramp_up_s;
        if (rate > kMaxOpenPerSecond + 0.001) {
            std::cerr << "--handshake-only rate " << rate
                      << "/s exceeds the declared 100/s cap (S3)\n";
            return false;
        }
        return true;
    }
    if (opt.tier < 1 || opt.tier > kMaxTier) {
        std::cerr << "tier must be 1.." << kMaxTier
                  << " (S2 caps concurrency at 500; anything above needs a human first)\n";
        return false;
    }
    if (opt.ramp_up_s < 1.0 || opt.hold_s < 1.0) {
        std::cerr << "--ramp-up and --hold must be at least 1 second\n";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        std::cerr
            << "usage: ws_stress --tier N [--host H] [--port P] [--ramp-up S] [--hold S] "
               "[--interval-ms MS] [--threads N] [--read-buffer BYTES] [--server-pid PID] "
               "[--max-consecutive-fail N] [--csv PATH] [--smoke] [--handshake-only]\n";
        return 4;
    }

    int threads = opt.threads > 0 ? opt.threads
                                  : static_cast<int>(std::thread::hardware_concurrency());
    if (threads <= 0) threads = 1;
    threads = std::min(threads, 8);  // S7: never thread-per-client, never more than 8

    Ctx ctx;
    ctx.opt = opt;

    tcp::resolver resolver(ctx.ioc);
    beast::error_code ec;
    auto results = resolver.resolve(opt.host, std::to_string(opt.port), ec);
    if (ec || results.empty()) {
        std::cerr << "resolve failed for " << opt.host << ":" << opt.port << ": "
                  << ec.message() << "\n";
        return 4;
    }
    ctx.endpoint = *results.begin();
    resolver.cancel();

    std::cout << std::fixed << std::setprecision(2) << "PLAN tier=" << opt.tier
              << " ramp_up_s=" << opt.ramp_up_s << " hold_s=" << opt.hold_s
              << " interval_ms=" << opt.interval_ms << " threads=" << threads
              << " buffer_bytes_per_conn=" << opt.read_buffer_bytes << " pinned_client_mem_mb="
              << (static_cast<double>(opt.tier) * opt.read_buffer_bytes) / (1024.0 * 1024.0)
              << " max_open_per_s=" << kMaxOpenPerSecond
              << " backoff=200ms_x2_cap30s_jitter_pm20pct breaker{fail_rate_pct=5,time_wait="
              << opt.breaker_timewait << ",p99_connect_s=5,rss_mb=" << opt.breaker_rss_mb
              << "} max_consecutive_fail=" << opt.max_consecutive_fail
              << " smoke=" << (opt.smoke ? 1 : 0)
              << " mode=" << (opt.handshake_only ? "handshake" : "long") << "\n";

    const auto started = clock_type::now();
    const auto hold_at = started + seconds_dur(opt.ramp_up_s + opt.hold_s);

    ctx.opening = true;
    Breaker breaker(ctx);
    Opener opener(ctx);
    Reaper reaper(ctx, hold_at + seconds_dur(kWindDownGraceS));
    asio::steady_timer hold_timer(ctx.ioc);

    breaker.start();
    opener.start();
    hold_timer.expires_at(hold_at);
    hold_timer.async_wait([&ctx](beast::error_code e) {
        if (e) return;
        ctx.closing = true;
        ctx.opening = false;
    });

    asio::signal_set signals(ctx.ioc, SIGINT, SIGTERM);
    signals.async_wait([&ctx](beast::error_code, int) { ctx.request_stop("signal"); });

    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    for (int i = 0; i < threads; ++i) pool.emplace_back([&ctx] { ctx.ioc.run(); });
    reaper.start();
    for (auto& t : pool) t.join();

    const double held = std::chrono::duration<double>(
                              std::min(hold_at, clock_type::now()) -
                              (started + seconds_dur(opt.ramp_up_s)))
                              .count();
    Report r = collect(ctx, std::max(1.0, held));
    print_report(r, ctx);
    append_csv(opt, r, threads);

    // A steady single long connection over 12s makes exactly one connect and about
    // twelve round trips. More connects than one, or a missing reply, is churn: the
    // ladder must not start on a client that cannot hold a socket open.
    if (opt.smoke) {
        const bool clean = r.msg_ok >= 8 && r.msg_err == 0 && r.connect_ok <= 1 &&
                           r.connect_fail == 0 && r.reason.empty();
        return clean ? 0 : 1;
    }
    if (r.reason.rfind("breaker", 0) == 0 || r.reason == "signal") return 2;
    if (!r.reason.empty()) return 3;
    return 0;
}

// gomoku_broker：v2 §4.1 的 supervisor。四件事——派活、收账、看门、让位。
//
//   配额  workers = max(1, 在线 CPU − reserved_cores)，reserved_cores 缺省 **3**：这不是抄文档的
//         2，是轮次163 在这台 6 核虚机上量出来的（2/3 份 ≈ 100/96 % 单进程 nps，4 份只有 88 %）。
//   派活  每个任务写一个任务文件（`src/training/run_spool.h` 那份契约），再 `posix_spawn` 一个
//         `gomoku_train --worker <task>`；同时在飞的不超过配额，多出来的排在队列里。
//   收账  只 tail 这个 run 自己的文件；一个 attempt 只有在**收尸之后**才判账——完成标记存在、整份读
//         干净（半行/断号/坏 JSON 都不并）、且行数与标记自己声明的条数逐条对上，才并进来。
//         所以「文件账」永远不会带脏行，也不存在「还在飞的活被读成截断」这一说。
//   看门  子进程退出码非 0、或没有完成标记、或心跳超时（缺省 5 s）⇒ 作废这一 attempt（丢弃它留下的
//         残行）并以 `attempt + 1` 重排；重排次数有上限，超了就报失败退出而不是无限重试。
//   让位  一个 UDS 监听（`<root>/broker.sock`），一行一条 JSON 命令：`pause` / `resume` / `stop` /
//         `quota N` / `status` / `beat`。pause 之后不再派新任务（粒度是任务不是进程，§4.4），在飞的跑完即止；
//         **pause 不是退出**——那手棋下完还要 resume，所以循环继续转。真停要下 `stop`。
//         每个 worker 一条连接，往这个 socket 每 200 ms 写一行心跳，broker 用它算「谁还活着」。
//
// 用法：build/gomoku_broker --root <dir> [--tasks N] [--games-per-task N] [--reserved-cores 1..4]
//       [--quota N] [--worker <path/gomoku_train>] [--params P] [--seconds S] [--hb-timeout S]
//       [--depth D] [--max-nodes N] [--timeout-ms N]
//       或作为控制客户端：build/gomoku_broker --root <dir> --send '{"cmd":"pause"}'
// 输出：--root 下的 tasks/task-<seq>-<attempt>.json、spool/w<seq>.jsonl、done/w<seq>-<attempt>.json、
//       merged/records.jsonl、merged/samples.jsonl、broker_status.json，以及 stdout 的状态行。
#include "training/json_int.h"
#include "training/params.h"
#include "training/run_spool.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

using json = nlohmann::json;
using namespace gomoku;

namespace {

constexpr int kMaxRequeues = 3;

struct Args {
    std::string root = "data/training/broker";
    std::string worker_binary = "./build/gomoku_train";
    std::string params_path;
    int tasks = 4;
    int games_per_task = 2;
    int reserved_cores = 3;    // 轮次163 的实测缺省，见文件头
    int quota = 0;             // 0 = 按公式算
    int first_game_id = 0;     // id 的高水位：跨任务不重号从这里起
    int hb_timeout_s = 5;
    int seconds = 0;           // >0：跑这么久（让位那一档要持续供活），0 = 派完 tasks 个就收
    int depth = 4;
    int max_nodes = 1500;
    int max_moves = 60;
    int timeout_ms = 400;
    int data_opening_plies = 4;
    int teacher_depth = 0;
    int teacher_max_nodes = 0;
    // 控制命令的一次性发送：`--send '{"cmd":"pause"}'` 连上这个 root 的 broker.sock 发一行、
    // 把回话打到 stdout 然后退出。仪器和面板都用这一条通道，不再开第二个监听。
    std::string send;
};

int need_int(int& i, int argc, char** argv) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: %s 需要一个数值\n", argv[i]);
        std::exit(2);
    }
    return std::atoi(argv[++i]);
}

std::string need_str(int& i, int argc, char** argv) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: %s 需要一个值\n", argv[i]);
        std::exit(2);
    }
    return argv[++i];
}

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        if (f == "--root") a.root = need_str(i, argc, argv);
        else if (f == "--worker") a.worker_binary = need_str(i, argc, argv);
        else if (f == "--params") a.params_path = need_str(i, argc, argv);
        else if (f == "--tasks") a.tasks = need_int(i, argc, argv);
        else if (f == "--games-per-task") a.games_per_task = need_int(i, argc, argv);
        else if (f == "--reserved-cores") a.reserved_cores = need_int(i, argc, argv);
        else if (f == "--quota") a.quota = need_int(i, argc, argv);
        else if (f == "--first-game-id") a.first_game_id = need_int(i, argc, argv);
        else if (f == "--hb-timeout") a.hb_timeout_s = need_int(i, argc, argv);
        else if (f == "--seconds") a.seconds = need_int(i, argc, argv);
        else if (f == "--depth") a.depth = need_int(i, argc, argv);
        else if (f == "--max-nodes") a.max_nodes = need_int(i, argc, argv);
        else if (f == "--max-moves") a.max_moves = need_int(i, argc, argv);
        else if (f == "--timeout-ms") a.timeout_ms = need_int(i, argc, argv);
        else if (f == "--data-opening-plies") a.data_opening_plies = need_int(i, argc, argv);
        else if (f == "--teacher-depth") a.teacher_depth = need_int(i, argc, argv);
        else if (f == "--teacher-max-nodes") a.teacher_max_nodes = need_int(i, argc, argv);
        else if (f == "--send") a.send = need_str(i, argc, argv);
        else {
            std::fprintf(stderr, "Error: 未知参数 %s\n", f.c_str());
            std::exit(2);
        }
    }
    if (a.tasks <= 0 || a.games_per_task <= 0) {
        std::fprintf(stderr, "Error: --tasks 与 --games-per-task 必须为正\n");
        std::exit(2);
    }
    if (a.reserved_cores < 1 || a.reserved_cores > 4) {
        // 范围照 v2 §7 那张参数表（1-4，默认 2）；缺省值本身按实测取 3。
        std::fprintf(stderr, "Error: --reserved-cores must be in [1, 4]\n");
        std::exit(2);
    }
    if (a.hb_timeout_s <= 0) {
        std::fprintf(stderr, "Error: --hb-timeout must be at least 1 second\n");
        std::exit(2);
    }
    return a;
}

int online_cpus() {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<int>(n) : 1;
}

std::chrono::steady_clock::time_point now() { return std::chrono::steady_clock::now(); }

struct InFlight {
    pid_t pid = -1;
    int seq = 0;
    int attempt = 1;
    std::string spool_path;
    std::string done_path;
    // 只有「这一份任务确实被给了心跳通道」才接受看门的死刑：单跑一个 worker（没有 broker）
    // 时任务里没有 control_socket，它一条心跳也不会发，拿「没收到心跳」杀它就是凭空杀好活。
    bool beating = false;
    std::chrono::steady_clock::time_point started = now();
    std::chrono::steady_clock::time_point last_beat = now();
};

class Broker {
public:
    explicit Broker(Args args) : args_(std::move(args)) {
        run_id_ = std::filesystem::path(args_.root).filename().string();
        if (run_id_.empty()) run_id_ = "broker-run";
        next_id_ = args_.first_game_id;
        cores_ = online_cpus();
        quota_ = args_.quota > 0 ? args_.quota : std::max(1, cores_ - args_.reserved_cores);
        const std::filesystem::path root(args_.root);
        tasks_dir_ = (root / "tasks").string();
        spool_dir_ = (root / "spool").string();
        done_dir_ = (root / "done").string();
        merged_dir_ = (root / "merged").string();
        socket_path_ = (root / "broker.sock").string();
        std::error_code ec;
        std::filesystem::create_directories(tasks_dir_, ec);
        std::filesystem::create_directories(spool_dir_, ec);
        std::filesystem::create_directories(done_dir_, ec);
        std::filesystem::create_directories(merged_dir_, ec);
        std::filesystem::remove(socket_path_, ec);
        for (int seq = 1; seq <= args_.tasks; ++seq) {
            Job job;
            job.seq = seq;
            jobs_.emplace(seq, job);
            queue_.push_back(seq);
        }
        last_seq_ = args_.tasks;
    }

    ~Broker() {
        if (listen_fd_ >= 0) ::close(listen_fd_);
        std::error_code ec;
        std::filesystem::remove(socket_path_, ec);
    }

    int quota() const { return quota_; }
    int cores() const { return cores_; }

    bool start(std::string& error) {
        // 监听这一侧必须是非阻塞的：`accept4(..., SOCK_NONBLOCK)` 的 flag 只作用于**返回的那个新
        // socket**，监听 fd 本身还阻塞的话，第一次 accept 就会把整个主循环卡在「等第一个连接」上
        // ——这一条是本机实测踩出来的（轮次163：一个任务文件都没写出去，进程睡在 socket 等待里）。
        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (listen_fd_ < 0) {
            error = std::string("socket: ") + std::strerror(errno);
            return false;
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            error = std::string("bind ") + socket_path_ + ": " + std::strerror(errno);
            return false;
        }
        // 队列按配额给，再加几个位置：面板也走这条通道，不能被一次连接把 worker 的心跳挡在门外。
        if (::listen(listen_fd_, 16) != 0) {
            error = std::string("listen: ") + std::strerror(errno);
            return false;
        }
        return true;
    }

    // 主循环：派活 → 收尸 → 心跳检查 → tail 收账，直到任务排空、或收到 `stop`、或 `--seconds` 到点。
    //
    // `pause` **不是**「跑完就退出」：§4.4 要的是玩家那手期间 worker 让位、那手结束再回来，
    // 所以暂停期间循环必须继续转（收尸、收账、听控制面都要），只是不再派新活。
    // 于是暂停后想真停，得由控制面下发 `stop`。
    int run() {
        const auto deadline = args_.seconds > 0 ? now() + std::chrono::seconds(args_.seconds)
                                                : std::chrono::steady_clock::time_point::max();
        std::string error;
        while (true) {
            reap_children();
            accept_connections();
            poll_sockets();
            spawn_ready();
            check_heartbeats();
            tail_all();
            const bool drained = inflight_.empty();
            if (stop_ && drained) break;
            if (!paused_ && !stop_ && queue_.empty() && drained && pending_.empty() &&
                args_.seconds == 0) {
                break;
            }
            if (now() >= deadline) {
                std::printf("broker: ran out of time with %zu queued, %zu in flight\n",
                            queue_.size(), inflight_.size());
                break;
            }
            // 持续供活那一档：只有「没让位、没叫停、手上没活」时才续一批。
            if (args_.seconds > 0 && !paused_ && !stop_ && queue_.empty() && drained) refill();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!error.empty()) std::cerr << "broker: " << error << "\n";
        write_status();
        const int unfinished = static_cast<int>(queue_.size()) + static_cast<int>(pending_.size());
        // 任何一条被放弃的任务都必须让进程非 0 退出，否则「任务账与文件账对齐」这条验收读数
        // 会被一个绿色的退出码掩盖掉。让位不是坏账，但让位之后还压着没派出去的活是。
        if (failed_ > 0) return failed_;
        return unfinished == 0 ? 0 : 1;
    }

private:
    struct Job {
        int seq = 0;
        int attempt = 1;
        int requeued = 0;
    };

    std::string task_path(int seq, int attempt) const {
        return tasks_dir_ + "/task-" + std::to_string(seq) + "-" + std::to_string(attempt) + ".json";
    }
    std::string spool_path(int seq) const { return spool_dir_ + "/w" + std::to_string(seq) + ".jsonl"; }
    std::string done_path(int seq, int attempt) const {
        return done_dir_ + "/w" + std::to_string(seq) + "-" + std::to_string(attempt) + ".json";
    }

    RunTask make_task(const Job& job, int first_game_id) const {
        RunTask task;
        task.run_id = std::filesystem::path(args_.root).filename().string();
        task.seq = job.seq;
        task.attempt = job.attempt;
        task.games = args_.games_per_task;
        task.first_game_id = static_cast<uint64_t>(first_game_id);
        task.depth = args_.depth;
        task.max_nodes = args_.max_nodes;
        task.max_moves = args_.max_moves;
        task.timeout_ms = args_.timeout_ms;
        task.teacher_depth = args_.teacher_depth;
        task.teacher_max_nodes = args_.teacher_max_nodes;
        task.data_opening_plies = args_.data_opening_plies;
        // 绑核：只用 reserved_cores 之后的那几个，0 与 1 永远留给 asio/GameManager 与玩家那手
        // （§4.4）。任务号对可用核取模——同一时刻一个核上不会有两个在飞的任务，因为派活的
        // 时候 in-flight 数不超过配额。
        const int available = std::max(1, cores_ - args_.reserved_cores);
        task.pin_cpu = args_.reserved_cores + ((job.seq - 1) % available);
        task.params_path = args_.params_path;
        task.spool_path = spool_path(job.seq);
        task.done_path = done_path(job.seq, job.attempt);
        task.control_socket = socket_path_;   // 心跳通道：worker 用它告诉看门「我还在算」
        return task;
    }

    // 续一批任务：数量按配额，够把在飞的补满就行。
    void refill() {
        const int batch = std::max(1, quota_);
        for (int i = 0; i < batch; ++i) {
            Job job;
            job.seq = ++last_seq_;
            jobs_.emplace(job.seq, job);
            queue_.push_back(job.seq);
        }
    }

    void spawn_ready() {
        while (!paused_ && !stop_ && static_cast<int>(inflight_.size()) < quota_ && !queue_.empty()) {
            const Job job = jobs_.at(queue_.front());
            queue_.erase(queue_.begin());
            std::string error;
            const int base = next_id_;
            next_id_ += args_.games_per_task;   // 派活的一方预留这一批的号，跨任务不重号
            const RunTask task = make_task(job, base);
            if (!write_task_file(task_path(job.seq, job.attempt), task, error)) {
                std::cerr << "broker: task file failed: " << error << "\n";
                failed_++;
                continue;
            }
            InFlight flight;
            flight.seq = job.seq;
            flight.attempt = job.attempt;
            flight.spool_path = task.spool_path;
            flight.done_path = task.done_path;
            flight.beating = !task.control_socket.empty();
            flight.started = now();
            flight.last_beat = now();
            if (!spawn(task, &flight)) {
                std::cerr << "broker: spawn failed for seq " << job.seq << "\n";
                requeue(job.seq, "spawn failed");
                continue;
            }
            inflight_.push_back(flight);
            spawned_++;
            std::printf("broker: spawned seq=%d attempt=%d pid=%ld pin=%d games=%d id_base=%d\n",
                        job.seq, job.attempt, static_cast<long>(flight.pid), task.pin_cpu,
                        task.games, base);
            std::fflush(stdout);
        }
    }

    bool spawn(const RunTask& task, InFlight* flight) {
        const std::string path = task_path(task.seq, task.attempt);
        std::vector<std::string> argv = {args_.worker_binary, "--worker", path};
        std::vector<char*> argvp;
        for (auto& arg : argv) argvp.push_back(arg.data());
        argvp.push_back(nullptr);
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        // worker 死了不该把 broker 一起带走：新进程组，看门要杀就整组杀。
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
        pid_t pid = -1;
        const int rc = posix_spawnp(&pid, argvp[0], nullptr, &attr, argvp.data(), environ);
        posix_spawnattr_destroy(&attr);
        if (rc != 0) {
            std::cerr << "broker: posix_spawnp: " << std::strerror(rc) << "\n";
            return false;
        }
        flight->pid = pid;
        return true;
    }

    // 重排 = 同一个 seq 的下一次 attempt：号在前、旧的排后，且 attempt 记在 jobs_ 里，
    // 这样「谁被重排了几次」不依赖某个 Job 对象还活着（它早就从队列里出去了）。
    void requeue(int seq, const std::string& reason) {
        Job& job = jobs_[seq];
        if (job.attempt >= kMaxRequeues + 1) {
            std::printf("broker: seq=%d gave up after %d attempts (%s)\n", seq, job.attempt,
                        reason.c_str());
            std::fflush(stdout);
            failed_++;
            return;
        }
        job.attempt++;
        job.requeued++;
        requeued_++;
        std::printf("broker: requeue seq=%d as attempt=%d (%s)\n", seq, job.attempt,
                    reason.c_str());
        std::fflush(stdout);
        queue_.insert(queue_.begin(), seq);
    }

    // 一个跑完的 attempt 先进「待判」名单，判账由 tail_all 在**收尸之后**做。
    // 为什么必须在收尸之后：worker 还活着的时候它的 spool 是**正在被写**的文件，这时候读到的
    // 「末尾半行」根本不能算截断（§4.3 的判据本来是「这一 attempt 结束了却没跑完」）；
    // 上一版就是踩在这里——它按目录扫标记，于是把一个还在飞的 attempt 判脏、删了它的标记，
    // 结果同一个 seq 同时在飞两份，被杀那一半的退出又被当成第二次重排。
    struct Finished {
        int seq = 0;
        int attempt = 1;
        std::string spool_path;
        std::string marker_path;
        int declared_lines = -1;   // 标记自己声明的条数：文件账必须与它逐条对上
    };

    void reap_children() {
        while (true) {
            int status = 0;
            const pid_t pid = ::waitpid(-1, &status, WNOHANG);
            if (pid <= 0) return;
            for (size_t i = 0; i < inflight_.size(); ++i) {
                if (inflight_[i].pid != pid) continue;
                const InFlight flight = inflight_[i];
                inflight_.erase(inflight_.begin() + static_cast<long>(i));
                std::error_code ec;
                const bool marker = std::filesystem::exists(flight.done_path, ec);
                const bool clean_exit = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                if (!marker || !clean_exit) {
                    const int code = WIFSIGNALED(status) ? -WTERMSIG(status) : WEXITSTATUS(status);
                    // 两种形状分开报：带标记却非 0 退出是「写完标记又崩了」（几乎不可能是崩，
                    // 更可能是我们的退出码被改），没标记才是「这一批根本没跑完」。
                    const std::string reason =
                        marker ? ("exit " + std::to_string(code) + " despite a done marker")
                               : ("exit " + std::to_string(code) + " without a done marker");
                    drop_attempt(flight);
                    requeue(flight.seq, reason);
                } else {
                    Finished finished;
                    finished.seq = flight.seq;
                    finished.attempt = flight.attempt;
                    finished.spool_path = flight.spool_path;
                    finished.marker_path = flight.done_path;
                    finished.declared_lines = marker_declared_lines(flight.done_path);
                    pending_.push_back(finished);
                }
                break;
            }
        }
    }

    static int marker_declared_lines(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) return -1;
        try {
            const json root = json::parse(std::string((std::istreambuf_iterator<char>(file)),
                                                       std::istreambuf_iterator<char>()));
            int lines = -1;
            if (root.is_object() && root.contains("lines") && read_json_int(root["lines"], lines)) {
                return lines;
            }
        } catch (const json::exception&) {
            return -1;
        }
        return -1;
    }

    // 重排之前把这一 attempt 留下的东西清掉：残行不能留在文件里冒充上一批的续篇。
    void drop_attempt(const InFlight& flight) {
        std::ofstream truncate(flight.spool_path, std::ios::trunc);
        truncate.close();
        std::error_code ec;
        std::filesystem::remove(flight.done_path, ec);
    }

    void drop_finished(const Finished& finished) {
        std::ofstream truncate(finished.spool_path, std::ios::trunc);
        truncate.close();
        std::error_code ec;
        std::filesystem::remove(finished.marker_path, ec);
    }

    void check_heartbeats() {
        for (size_t i = 0; i < inflight_.size();) {
            InFlight& flight = inflight_[i];
            if (!flight.beating) {
                ++i;
                continue;
            }
            const auto idle = std::chrono::duration_cast<std::chrono::seconds>(now() -
                                                                               flight.last_beat)
                                  .count();
            if (idle < args_.hb_timeout_s) {
                ++i;
                continue;
            }
            std::printf("broker: heartbeat stale %lld s for seq=%d, killing pid=%ld\n",
                        static_cast<long long>(idle), flight.seq, static_cast<long>(flight.pid));
            ::kill(flight.pid, SIGKILL);
            const InFlight copy = flight;
            inflight_.erase(inflight_.begin() + static_cast<long>(i));
            drop_attempt(copy);
            requeue(copy.seq, "heartbeat timeout");
        }
    }

    void accept_connections() {
        sockaddr_un addr{};
        socklen_t len = sizeof(addr);
        const int fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len,
                                 SOCK_NONBLOCK);   // 没有新连接时立刻返回，主循环不能被 accept 卡住
        if (fd < 0) return;
        connections_.push_back(fd);
    }

    void poll_sockets() {
        for (size_t i = 0; i < connections_.size();) {
            const int fd = connections_[i];
            char buffer[512];
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
            if (n == 0) {
                ::close(fd);
                connections_.erase(connections_.begin() + static_cast<long>(i));
                continue;
            }
            if (n < 0) {
                ++i;
                continue;
            }
            buffer[n] = '\0';
            handle_command(fd, std::string(buffer, static_cast<size_t>(n)));
            ++i;
        }
    }

    void handle_command(int fd, const std::string& line) {
        for (const std::string& one : split_lines(line)) {
            json msg;
            try {
                msg = json::parse(one);
            } catch (const json::exception&) {
                std::printf("broker: ignored an unparsable control line\n");
                continue;
            }
            if (!msg.is_object() || !msg.contains("cmd") || !msg["cmd"].is_string()) continue;
            const std::string cmd = msg["cmd"].get<std::string>();
            // 每一条被认下来的命令都回一句 ack：控制面要能区分「下发了」和「生效了」。
            // 不回话的那一侧只能靠超时猜，而猜出来的状态不能进面板。
            json ack;
            ack["cmd"] = "ack";
            ack["applied"] = cmd;
            if (cmd == "pause") {
                paused_ = true;
                std::printf("broker: pause accepted, %zu in flight will drain\n", inflight_.size());
            } else if (cmd == "resume") {
                paused_ = false;
                std::printf("broker: resume accepted\n");
            } else if (cmd == "stop") {
                // 收摊：不再派新活，等在飞的把这一批跑完（任务粒度，不杀进程），然后写状态退出。
                stop_ = true;
                paused_ = true;
                std::printf("broker: stop accepted, dropping %zu queued tasks\n", queue_.size());
                queue_.clear();
            } else if (cmd == "quota") {
                int wanted = 0;
                if (msg.contains("workers") && read_json_int(msg["workers"], wanted) && wanted >= 1) {
                    quota_ = wanted;
                    std::printf("broker: quota set to %d\n", quota_);
                }
            } else if (cmd == "beat") {
                int seq = 0;
                if (msg.contains("seq") && read_json_int(msg["seq"], seq)) {
                    bool matched = false;
                    for (InFlight& flight : inflight_) {
                        if (flight.seq != seq) continue;
                        flight.last_beat = now();
                        matched = true;
                    }
                    // 计数的是「认出来的心跳」，不是收到的条数：一个不认识这个 seq 的 beat 说明
                    // 对面那个 worker 已经被重排或作废了，把它算进活着的凭据会让状态读数说谎。
                    if (matched) ++beats_;
                }
            } else if (cmd == "status") {
                // 回给问的那一方（面板/仪器走这条），同时打一行到 broker 的 stdout 便于人看。
                const std::string reply = status_json().dump() + "\n";
                send_line(fd, reply);
                std::printf("%s", reply.c_str());
            } else {
                // 认不出的命令不能沉默：沉默让下发的一方以为它生效了。
                json reject;
                reject["cmd"] = "reject";
                reject["applied"] = cmd;
                reject["why"] = "unknown control command";
                send_line(fd, reject.dump() + "\n");
                std::printf("broker: rejected control command %s\n", cmd.c_str());
                continue;
            }
            // 心跳不吃 ack：一秒五拍 ×N 个 worker 的应答只是把控制面变成噪音。
            if (cmd != "beat" && cmd != "status") {
                ack["paused"] = paused_;
                ack["stopped"] = stop_;
                ack["quota"] = quota_;
                send_line(fd, ack.dump() + "\n");
            }
            std::fflush(stdout);
        }
    }

    void send_line(int fd, const std::string& text) {
        const ssize_t sent = ::send(fd, text.data(), text.size(), MSG_NOSIGNAL);
        if (sent != static_cast<ssize_t>(text.size())) {
            std::cerr << "broker: control reply failed: " << std::strerror(errno) << "\n";
        }
    }

    static std::vector<std::string> split_lines(const std::string& blob) {
        std::vector<std::string> out;
        std::string current;
        for (char c : blob) {
            if (c == '\n') {
                if (!current.empty()) out.push_back(current);
                current.clear();
                continue;
            }
            current.push_back(c);
        }
        if (!current.empty()) out.push_back(current);
        return out;
    }

    // 判账：一个 attempt 只有在「进程已收尸 + 标记在 + 整份读干净 + 行数与标记声明逐条对上」之后才入账。
    // 判的时候写的一侧已经关了文件，所以这是一次性从头读完，没有增量偏移要维护
    // （「只往前不回头」那条口径由 `SpoolTail` 自己的臂钉着）。
    void tail_all() {
        while (!pending_.empty()) {
            const Finished finished = pending_.front();
            pending_.erase(pending_.begin());
            judge(finished);
        }
    }

    void judge(const Finished& finished) {
        std::vector<SpoolLine> lines;
        TailReport report;
        std::string error;
        SpoolTail tail(run_id_);
        if (!tail.read_file(finished.spool_path, lines, report, error)) {
            std::cerr << "broker: tail " << finished.spool_path << ": " << error << "\n";
            drop_finished(finished);
            requeue(finished.seq, "spool could not be read");
            return;
        }
        // 标记自己声明的条数与文件里数到的行数必须相等，这是「任务账与文件账对齐」的正面读数：
        // 标记说 66 行而文件里只有 65 行完整行，就是断号，和半行同罪。
        const bool count_mismatch = finished.declared_lines != static_cast<int>(lines.size());
        if (report.needs_requeue() || count_mismatch) {
            const std::string why = count_mismatch && report.first_drop.empty()
                                        ? ("the marker declares " +
                                           std::to_string(finished.declared_lines) +
                                           " lines, the file has " + std::to_string(lines.size()))
                                        : report.first_drop;
            std::printf("broker: spool %s is dirty (%s) -> the task is requeued\n",
                        finished.spool_path.c_str(), why.c_str());
            std::fflush(stdout);
            drop_finished(finished);
            requeue(finished.seq, "dirty spool");
            return;
        }
        append_payloads(finished.spool_path, lines);
        ++done_;
        std::printf("broker: done seq=%d attempt=%d\n", finished.seq, finished.attempt);
        std::fflush(stdout);
    }

    // 「每秒节点数」这一栏不另开一条 IPC：并进来的每条记录本来就带每手的 `nodes` 与 `elapsed_ms`，
    // 加起来就是这个 run 真正做过的搜索工作量。分母是**搜索时间**之和，不是墙钟——本机墙钟不可复现
    // （轮次163 的吞吐表里同一个节点帽在两份进程之间能差 30 %），所以这一栏的名字就写明它是搜索秒。
    void count_record_work(const std::string& payload) {
        json record;
        try {
            record = json::parse(payload);
        } catch (const json::exception&) {
            return;
        }
        if (!record.is_object() || !record.contains("moves") || !record["moves"].is_array()) return;
        for (const json& move : record["moves"]) {
            int nodes = 0;
            if (move.contains("nodes") && read_json_int(move["nodes"], nodes)) merged_nodes_ += nodes;
            int elapsed = 0;
            if (move.contains("elapsed_ms") && read_json_int(move["elapsed_ms"], elapsed)) {
                merged_search_ms_ += elapsed;
            }
        }
    }

    static int count_kind(const std::vector<SpoolLine>& lines, const std::string& kind) {
        int n = 0;
        for (const SpoolLine& line : lines) {
            if (line.kind == kind) ++n;
        }
        return n;
    }

    void append_payloads(const std::string& path, const std::vector<SpoolLine>& lines) {
        std::ofstream records(merged_dir_ + "/records.jsonl", std::ios::app);
        std::ofstream samples(merged_dir_ + "/samples.jsonl", std::ios::app);
        if (!records.is_open() || !samples.is_open()) {
            std::cerr << "broker: cannot open the merged files under " << merged_dir_ << "\n";
            return;
        }
        for (const SpoolLine& line : lines) {
            // 并进来的是 payload 本身：一行字节，不重新序列化。
            if (line.kind == "game") {
                records << line.payload << "\n";
                count_record_work(line.payload);   // 每秒节点数就从这些行数出来
            } else if (line.kind == "sample") {
                samples << line.payload << "\n";
            }
        }
        records.flush();
        samples.flush();
        merged_records_ += count_kind(lines, "game");
        merged_samples_ += count_kind(lines, "sample");
        std::printf("broker: merged %zu lines from %s\n", lines.size(), path.c_str());
        std::fflush(stdout);
    }

    json status_json() const {
        json root;
        root["run_id"] = run_id_;
        root["cores"] = cores_;
        root["reserved_cores"] = args_.reserved_cores;
        root["quota"] = quota_;
        root["paused"] = paused_;
        root["stopped"] = stop_;
        root["spawned"] = spawned_;
        root["done"] = done_;
        root["queued"] = static_cast<int>(queue_.size());
        root["in_flight"] = static_cast<int>(inflight_.size());
        root["pending_judgement"] = static_cast<int>(pending_.size());
        root["requeued"] = requeued_;
        root["failed"] = failed_;
        root["beats"] = beats_;
        root["merged_records"] = merged_records_;
        root["merged_samples"] = merged_samples_;
        root["merged_nodes"] = merged_nodes_;
        root["merged_search_ms"] = merged_search_ms_;
        root["nodes_per_search_second"] =
            merged_search_ms_ > 0 ? merged_nodes_ * 1000 / merged_search_ms_ : 0;
        // 「各 worker 心跳」这一项是 §4.5.2 那条 `train_status` 帧要的：面板要能指认哪个 pid 在哪个核上、
        // 上一次心跳是几毫秒之前。缺了这一项，「已让出 N 个 worker」那句话就没有凭据。
        json workers = json::array();
        for (const InFlight& flight : inflight_) {
            json entry;
            entry["seq"] = flight.seq;
            entry["attempt"] = flight.attempt;
            entry["pid"] = static_cast<long long>(flight.pid);
            entry["beating"] = flight.beating;
            entry["beat_age_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        now() - flight.last_beat)
                                        .count();
            entry["age_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(now() -
                                                                                   flight.started)
                                  .count();
            workers.push_back(entry);
        }
        root["workers"] = workers;
        return root;
    }

    void write_status() {
        std::ofstream file(args_.root + "/broker_status.json", std::ios::trunc);
        if (!file.is_open()) {
            std::cerr << "broker: cannot write " << args_.root << "/broker_status.json\n";
            return;
        }
        file << status_json().dump(2) << "\n";
    }

    Args args_;
    std::string run_id_{"broker-run"};
    std::string tasks_dir_;
    std::string spool_dir_;
    std::string done_dir_;
    std::string merged_dir_;
    std::string socket_path_;
    int listen_fd_ = -1;
    std::vector<int> connections_;
    std::vector<int> queue_;
    int last_seq_ = 0;   // 发出去过的最大任务号；`--seconds` 续批时从这里往后接
    std::map<int, Job> jobs_;
    std::vector<InFlight> inflight_;
    std::vector<Finished> pending_;   // 已收尸、还没判账的 attempt
    int cores_ = 1;
    int quota_ = 1;
    int next_id_ = 0;
    bool paused_ = false;
    bool stop_ = false;
    int spawned_ = 0;
    int done_ = 0;
    int beats_ = 0;
    int requeued_ = 0;
    int failed_ = 0;
    int merged_records_ = 0;
    int merged_samples_ = 0;
    long long merged_nodes_ = 0;
    long long merged_search_ms_ = 0;
};

}  // namespace

// 一次性控制客户端：连上 `<root>/broker.sock`，发一行，把回话原样打到 stdout。
// 它不开任何监听（§4.3：新 IPC 不许再开 TCP 端口，控制面也只有那一个 UDS），
// 所以面板之外的一根探针和面板本身看到的是同一个状态。
int send_control(const std::string& root, const std::string& line) {
    const std::string path = (std::filesystem::path(root) / "broker.sock").string();
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "Error: socket: " << std::strerror(errno) << "\n";
        return 2;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "Error: connect " << path << ": " << std::strerror(errno) << "\n";
        ::close(fd);
        return 2;
    }
    const std::string payload = line + "\n";
    if (::send(fd, payload.data(), payload.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(payload.size())) {
        std::cerr << "Error: send: " << std::strerror(errno) << "\n";
        ::close(fd);
        return 2;
    }
    char buffer[4096];
    // 这一头不能无限等：`--send` 也用来发那些不需要回话的命令，等不到就把它挂住，
    // 而一个挂住的探针在仪器里读起来跟「服务端死了」一模一样（轮次163 的第一次冒烟就是这样）。
    timeval timeout{};
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer) - 1, 0);
    ::close(fd);
    if (n <= 0) {
        // 没有回话（超时或对方收尾）：`quota` 这一类只改状态的命令允许这样，
        // 但探针要用「随后发一次 status」自己确认，而不是猜。
        return 0;
    }
    std::cout.write(buffer, n);
    if (buffer[n - 1] != '\n') std::cout << "\n";
    return 0;
}

int main(int argc, char** argv) {
    Args args = parse(argc, argv);
    if (!args.send.empty()) return send_control(args.root, args.send);
    Broker broker(args);
    std::string error;
    if (!broker.start(error)) {
        std::cerr << "Error: " << error << "\n";
        return 2;
    }
    std::printf("broker: cores=%d reserved=%d quota=%d root=%s\n", broker.cores(),
                args.reserved_cores, broker.quota(), args.root.c_str());
    std::fflush(stdout);
    return broker.run();
}

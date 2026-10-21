#include "worker_mode.h"

#include "params.h"
#include "trainer.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <sched.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace gomoku {

// 协议就两个整数，所以这里手写这一行而不是过一遍 JSON 库：字节形状是被测试钉住的东西，
// 少一层依赖少一处「库的格式化改了，协议也跟着改」。
std::string worker_beat_line(int seq, int attempt) {
    return "{\"cmd\":\"beat\",\"seq\":" + std::to_string(seq) + ",\"attempt\":" +
           std::to_string(attempt) + "}\n";
}

namespace {

constexpr int kBeatIntervalMs = 200;   // 看门缺省 5 s 无心跳就杀；这一档要能留出 25 倍的余量

// 把一份 JSONL 的每一行**原样**搬进 spool：payload 就是那一行字节，不重新序列化，
// 所以 records / samples 的既有读者在收账之后仍然能直接读它。
bool republish_lines(const std::string& path, const std::string& kind, SpoolWriter& writer,
                     int& count, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "cannot read " + path;
        return false;
    }
    count = 0;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        if (!writer.append(kind, line, error)) return false;
        ++count;
    }
    return true;
}

#ifndef _WIN32
// 一条连接用到底（v2 §4.3：每 worker 一条 UDS 连接）。发不出去就返回 false，
// 由心跳线程停下去——被看门杀掉然后重排，比假装还活着更便宜。
class BeatChannel {
public:
    ~BeatChannel() { shutdown(); }
    bool open(const std::string& path, std::string& error) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) {
            error = std::string("socket: ") + std::strerror(errno);
            return false;
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() + 1 >= sizeof(addr.sun_path)) {
            error = "control socket path is too long for a UDS address: " + path;
            shutdown();
            return false;
        }
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            error = "connect " + path + ": " + std::strerror(errno);
            shutdown();
            return false;
        }
        return true;
    }
    bool beat(int seq, int attempt, std::string& error) {
        const std::string line = worker_beat_line(seq, attempt);
        const ssize_t sent = ::send(fd_, line.data(), line.size(), MSG_NOSIGNAL);
        if (sent != static_cast<ssize_t>(line.size())) {
            error = "beat send: " + std::string(std::strerror(errno));
            return false;
        }
        ++beats_;
        return true;
    }
    int beats() const { return beats_; }

private:
    void shutdown() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    int fd_ = -1;
    int beats_ = 0;
};
#endif

// 把心跳线程停下来。放在这里是因为 `run_worker_mode` 的每一条失败路径都要经过它：
// 线程在收集返回之前不 join，早退的那几条路径会把捕获到的栈上对象留在原地。
struct BeatStopper {
    std::atomic<bool>& beating;
    std::thread& heartbeat;
    ~BeatStopper() {
        beating.store(false);
        if (heartbeat.joinable()) heartbeat.join();
    }
};

}  // namespace

uint64_t worker_seed_for(const RunTask& task) {
    // 任务文件里没有「种子」这一项是有意的：同一个任务重跑必须给出同一批局，而 id 的分配权
    // 在派活的一方（`first_game_id`）。所以种子由任务自己推出来，且 `attempt` 参与——重排的
    // 那一遍要跑出不与上一遍重合的一批局，否则被杀掉的那半轮只是在原地重放。
    uint64_t mixed = task.first_game_id * 0x9E3779B97F4A7C15ull;
    mixed ^= static_cast<uint64_t>(task.seq) * 131ull;
    mixed ^= static_cast<uint64_t>(task.attempt) * 0x100000001B3ull;
    return mixed;
}

bool run_worker_mode(const std::string& task_path, std::string& error, WorkerOutcome* out) {
    RunTask task;
    if (!read_task_file(task_path, task, error)) return false;

    // 进程级绑核：这一半在 worker 自己手里（v2 §4.2 的第四条理由与指导书思路 4 就是这个形状——
    // 外面用 taskset 拉起的做法管不到动态 spawn 的 worker，所以绑核必须是启动时自绑）。
#ifndef _WIN32
    if (task.pin_cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(task.pin_cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0) {
            error = "sched_setaffinity to cpu " + std::to_string(task.pin_cpu) + " failed";
            return false;
        }
    }
#else
    if (task.pin_cpu >= 0) {
        // 杀线（v2 §4.2）：进程模型只在 POSIX 侧实现，Windows 保持线程模式。这里不写一份
        // 没有仪器覆盖的 `SetProcessAffinityMask`，而是明说这一档在 Windows 上不成立。
        error = "worker 的进程级绑核只在 POSIX 侧实现（Windows 保持线程模式）";
        return false;
    }
#endif

    CoefficientPair coefficients;
    if (!task.params_path.empty() && !CoefficientVector::load(task.params_path, coefficients, error)) {
        return false;
    }

    TrainingConfig config;
    config.search_depth = task.depth;
    config.max_nodes = task.max_nodes;
    config.max_moves = task.max_moves;
    config.timeout_ms = task.timeout_ms;
    config.teacher_depth = task.teacher_depth;
    config.teacher_max_nodes = task.teacher_max_nodes;
    config.data_opening_plies = task.data_opening_plies;
    config.seed = worker_seed_for(task);
    // 一个进程一张表：D-2 的分片锁解决的是同一地址空间里几个 worker 共用一张表，跨进程没有
    // 这件事（v2 §4.2 第四条：进程边界与「TT 本来就每引擎一张」天然对齐）。
    config.shared_tt_entries = 0;
    // 产出先落在任务自己的临时文件里，再由下面这一段包成 spool 行。这样数据采集那一步
    // 用的是与线程模式**完全相同**的写盘代码，worker 只多了一层信封。
    config.records_path = task.spool_path + ".records";
    config.samples_path = task.spool_path + ".samples";
    std::error_code ec;
    if (!std::filesystem::path(config.records_path).parent_path().empty()) {
        std::filesystem::create_directories(
            std::filesystem::path(config.records_path).parent_path(), ec);
    }
    std::filesystem::remove(config.records_path, ec);
    std::filesystem::remove(config.samples_path, ec);

    Trainer trainer(coefficients, config);
    std::atomic<bool> beating{false};
    std::thread heartbeat;
    const BeatStopper stopper{beating, heartbeat};
    std::string beat_error;
#ifndef _WIN32
    BeatChannel channel;
    if (!task.control_socket.empty()) {
        if (!channel.open(task.control_socket, error)) return false;
        // 第一拍在干活之前同步发出去：通道连不上就说明这一份任务没有活着的通知路径，
        // 与其让看门在 5 s 后把它当僵尸杀掉，不如现在退出并重排（一条局都不浪费）。
        if (!channel.beat(task.seq, task.attempt, error)) return false;
        beating.store(true);
        heartbeat = std::thread([&task, &channel, &beating, &beat_error] {
            while (beating.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kBeatIntervalMs));
                if (!beating.load()) break;
                if (!channel.beat(task.seq, task.attempt, beat_error)) break;
            }
        });
    }
#else
    if (!task.control_socket.empty()) {
        error = "worker 的 UDS 心跳只在 POSIX 侧实现（Windows 保持线程模式）";
        return false;
    }
#endif
    trainer.collect_data_games(task.games, 1, static_cast<int>(task.first_game_id));

    SpoolWriter writer;
    if (!writer.open(task, error)) return false;
    int records = 0;
    int samples = 0;
    if (!republish_lines(config.records_path, "game", writer, records, error)) return false;
    if (!republish_lines(config.samples_path, "sample", writer, samples, error)) return false;
    if (!writer.finish(error)) return false;
    stopper.beating.store(false);
    if (heartbeat.joinable()) heartbeat.join();

    // 中间文件在标记之后才删：完成标记落下之前，盘上这份原始产物就是这一任务的凭据。
    std::filesystem::remove(config.records_path, ec);
    std::filesystem::remove(config.samples_path, ec);

    if (out != nullptr) {
        out->pin_cpu = task.pin_cpu;
        out->records = records;
        out->samples = samples;
        out->games_played = trainer.games_played();
        out->spool_path = task.spool_path;
#ifndef _WIN32
        out->beats = channel.beats();
#endif
    }
    return true;
}

}  // namespace gomoku

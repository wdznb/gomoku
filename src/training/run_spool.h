#ifndef GOMOKU_RUN_SPOOL_H
#define GOMOKU_RUN_SPOOL_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gomoku {

// v2 §4.3（指导书 §4 戊-1）：任务文件与结果 spool。
//
// 三条口径，每一条下面都有一臂盯着（§2 的「新机制必须有杀线」）：
//
// 1. 每行自带 `run_id` / `seq` / `attempt` / `index`。收账的一方只从**自己上次读到的偏移**往后读，
//    绝不回读已经入账的字节（文档写的「永不 seek」取的是这个意思：账推进了就不回去）。
// 2. 一个 worker 只写**它自己那一个文件**。于是「半行」只可能来自被杀在一次 append 中间的那一下，
//    不需要跨进程协调写——v2 那张图里「tail 各 worker 的 JSONL」就是这个形状，N 个写者共用一个文件
//    才是需要协调的那种形状，这里刻意不做。
// 3. 读到的行若截断、不解析、或 `(seq, attempt, index)` 不是期望的下一个 ⇒ **丢弃并重排任务**，
//    绝不把一行被截断的 JSON 补完再读：那是数据污染，不是健壮性。谁判出这件事由
//    `TailReport::needs_requeue()` 说，重排的**决定**由 broker 下（它下一轮就写一个新 attempt）。
//
// 为什么行里要带 `attempt`：重排之后同一个 `seq` 会再跑一遍，新的那些行必须能和上一 attempt 留下的
// 残骸分清，否则「账对齐」这条没有可对齐的东西。第几次尝试不是策略，是记账的必要字段。
struct RunTask {
    std::string run_id;
    int seq = 0;                 // 任务号，一个 run 内单调
    int attempt = 1;             // 第几次尝试；重排由派活的一方递增
    int games = 0;               // 这一个任务跑几局数据采集
    uint64_t first_game_id = 0;  // 上一个任务用到的最后一个 game_id：这一批从它 +1 起，由派活的一方给
    int depth = 4;
    int max_nodes = 0;           // 0 = 由剩余时间推上限（与训练缺省同口径）
    int max_moves = 60;
    int pin_cpu = -1;            // -1 = 不绑，由调度器决定
    // 下面四项是**数据采集那一步真正吃到的档位**。它们进契约是因为不等价就没有意义：
    // worker 与线程模式必须跑同一套深度、同一套时间/节点预算、同一套开局长度与同一个老师，
    // 否则「换到进程模式不改变数据」这句话只能对一半成立（丁-2 的蒸馏就是这样被漏掉的）。
    int timeout_ms = 400;        // `max_nodes = 0` 时它才是预算，与训练缺省同口径
    int teacher_depth = 0;       // 丁-2：0 = 关
    int teacher_max_nodes = 0;
    int data_opening_plies = 4;  // 每局数据采集的开局手数（`Trainer::random_opening_line`）
    std::string params_path;     // 学生系数所在文件；空 = 用缺省那一套
    std::string spool_path;      // 结果落点（这个 worker 自己的文件）
    std::string done_path;       // 完成标记落点
    // 心跳走 UDS（v2 §4.3：控制面每 worker 一条连接，一行一条 JSON）。空 = 这条契约里没有人收心跳，
    // 于是派活的一方也不能因为「没收到心跳」杀它——只有 broker 起了监听才填。
    std::string control_socket;
};

bool write_task_file(const std::string& path, const RunTask& task, std::string& error);
bool read_task_file(const std::string& path, RunTask& out, std::string& error);

// 一行 spool。`payload` 是**原样的一段 JSON 文本**——就是一条对局记录或一个训练样本本来的样子，
// 存成字符串而不是子对象，为的是换到进程模式之后数据本身的形状一个字都不变，收账的一方可以把它
// 直接交回给既有的 `GameRecord::from_json` / `TrainingSample::from_json`。
struct SpoolLine {
    std::string run_id;
    int seq = 0;
    int attempt = 1;
    int index = 0;      // 这个 (seq, attempt) 里的第几条，1 起
    std::string kind;   // "game" / "sample"
    std::string payload;
};

std::string spool_line_to_string(const SpoolLine& line);
bool parse_spool_line(const std::string& text, SpoolLine& out, std::string& error);

// 写者：一次构造对应一个 (run_id, seq, attempt) 的一个文件。每条写完立刻 flush——被 SIGKILL 打断时
// 最多损失正在写的那一条，而且它一定是没有换行结尾的那一条（下面的读者就按这个判「截断」）。
class SpoolWriter {
public:
    bool open(const RunTask& task, std::string& error);
    bool append(const std::string& kind, const std::string& payload, std::string& error);
    int written() const { return written_; }
    // 写完成标记（带这一尝试声明的条数）。声明的条数是账对齐的另一半：读者数到的完整行数少于它，
    // 就是这一尝试没跑完，重排。
    bool finish(std::string& error);

private:
    std::string spool_path_;
    std::string done_path_;
    std::string run_id_;
    int seq_ = 0;
    int attempt_ = 1;
    int written_ = 0;
};

// 一文件的读取报告：入账的行、三类丢弃，以及末尾那条还没写完的截断行（它既不入账也不推进偏移）。
struct TailReport {
    int accepted = 0;
    int dropped_unparsable = 0;
    int dropped_sequence = 0;
    int dropped_other_run = 0;
    bool truncated_tail = false;
    std::string first_drop;  // 人话一句，进日志

    bool needs_requeue() const {
        return dropped_unparsable > 0 || dropped_sequence > 0 || truncated_tail;
    }
};

class SpoolTail {
public:
    explicit SpoolTail(std::string run_id) : run_id_(std::move(run_id)) {}

    // 从上次偏移往后读；`lines` 只收这次新入账的行。
    bool read_file(const std::string& path, std::vector<SpoolLine>& lines, TailReport& report,
                   std::string& error);
    std::size_t offset(const std::string& path) const;
    int expected_index(const std::string& path, int seq, int attempt) const;

private:
    std::string run_id_;
    std::map<std::string, std::size_t> offsets_;
    // 每个 (文件, seq, attempt) 各自的序号游标：重排之后一个文件里会有两个 attempt，
    // 它们的 index 都是从 1 起的独立序列。
    std::map<std::string, int> next_index_;
};

}  // namespace gomoku

#endif

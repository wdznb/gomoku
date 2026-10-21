#ifndef GOMOKU_WORKER_MODE_H
#define GOMOKU_WORKER_MODE_H

#include "run_spool.h"

#include <cstdint>
#include <string>

namespace gomoku {

// 戊-1（v2 §4.1、指导书 §4 思路 2）：`gomoku_train --worker <task.json>` 的那一半。
//
// 一个进程跑一个任务，四步：绑核 → 用**同一条数据采集路**（`Trainer::collect_data_games`，
// 也就是 `run_round` 里那一步：同一个 `RetryExplorer`、同一个 `DataCollector`、同一处落盘）
// 跑 N 局 → 把产出的每一行**原样**装进 spool 信封写进这个任务自己的文件 → 写完成标记。
// 训练逻辑一行都不复制，这是指导书思路 2 那句「复用现有 Trainer，不复制训练逻辑」的执行处。
//
// 为什么要 spool 这一层：v2 §4.3 要求「每行自带 run_id / seq」，而 records/samples 的行形状
// 是既有的（改它就等于同时改两套读者）。所以信封包在外面，payload 里是那一段字节本身——
// 收账的一方把 payload 交回 `GameRecord::from_json` / `TrainingSample::from_json` 就能读。
//
// 心跳：任务里带了 `control_socket` 时，worker 在干活期间**用同一条 UDS 连接**每 200 ms 发一行
// 心跳（§4.3 的「每 worker 一条 UDS 连接，消息是单行 JSON」）。发不出去就当场失败——
// 一个没有心跳通道的 worker 只会被看门当僵尸杀掉重排，早死比晚死省一批局。
struct WorkerOutcome {
    int pin_cpu = -1;        // 实际绑到的核（-1 = 没绑）
    int records = 0;         // 写进 spool 的记录行数
    int samples = 0;         // 写进 spool 的样本行数
    int games_played = 0;    // `Trainer::games_played()`：真正跑完并保留的局数
    int beats = 0;           // 往控制面发出去的心跳条数（0 = 这一份任务根本没有控制面）
    std::string spool_path;
};

// 一行心跳的字节形状：`{"cmd":"beat","seq":N,"attempt":A}` 加一个换行。
// 把它单独抽出来是因为它是协议，而协议要能被独立钉住——改一个字段名，钉它的那一臂就得红。
std::string worker_beat_line(int seq, int attempt);

// 任务里没有一个「种子」字段是有意的：id 的分配权在派活的一方（`first_game_id`），而 RNG 的
// 种子由任务本身推出来——同一个任务文件重跑两次必须产出逐字相同的两行流，这也是等价性的凭据。
uint64_t worker_seed_for(const RunTask& task);

// 跑一个任务。返回 false 时 `error` 非空且**不写完成标记**（被杀的与失败的形状一致：
// 缺标记 = 重排）。`out` 可空。
bool run_worker_mode(const std::string& task_path, std::string& error, WorkerOutcome* out = nullptr);

}  // namespace gomoku

#endif

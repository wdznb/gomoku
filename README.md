# 五子棋 AI 深度思考系统

一个高性能的五子棋AI系统，具备深度思考、自我训练和参数优化能力。

## 版本

`master` 是基线分支；每个阶段完成时在 `master` 上打一个带备注的标签，备注逐条列出该阶段实现的计划功能。

| 标签 | 阶段 | 实现的计划功能 |
| --- | --- | --- |
| `v0.1.0-phase1` | 阶段1 基础引擎 | 15x15 棋盘与棋型识别、黑棋禁手检测、贪心搜索、WebSocket 服务器与 Canvas 前端 |
| `v0.2.3-phase2` | 阶段2 高级搜索 | VCF / VCT 威胁空间搜索、AlphaBeta + 迭代加深 + quiescence、置换表、开局库、强制失败证明与自动认输、异步思考控制 |
| `v0.3.2-phase3` | 阶段3 自我训练 | 对局记录、自我对弈 + 悔棋再战、训练数据收集、遗传 + 爬山优化、Elo 评分、训练主循环与断点续训、`gomoku_server --params` |

阶段3 的三个标签：`v0.3.0-phase3` 是功能基线，`v0.3.1-phase3` 是训练模块审查（`REVIEW_LOG.md` 轮次 49–50）后的修复版本，`v0.3.2-phase3` 是阶段收尾（文档与版本记录）后的 `master` 基线。

## 特性

### 阶段1
- ✅ 15x15 棋盘，支持标准五子棋规则
- ✅ 棋型识别（活四、冲四、活三、眠三等）
- ✅ 组合棋型检测（四四、四三、三三）
- ✅ 黑棋禁手检测（三三禁手、四四禁手、长连禁手）
- ✅ 贪心搜索算法
- ✅ WebSocket 实时通信
- ✅ 纯前端 Canvas 棋盘渲染
- ✅ 可配置威胁系数

### 阶段2
- ✅ VCF / VCT 威胁空间搜索
- ✅ AlphaBeta + 迭代加深 + quiescence（fail-soft）
- ✅ 置换表（Zobrist 哈希 + 代龄老化）
- ✅ 开局库（8 对称匹配 + 加权随机）
- ✅ 强制失败证明（loss proof）与 AI 自动认输
- ✅ 异步控制（AI 独立线程思考 + 多会话并发 + 超时中断）

### 阶段3
- ✅ 对局记录系统（JSON / JSONL）
- ✅ 自我对弈 + 悔棋再战（RetryExplorer，对称类去重）
- ✅ 训练数据收集（52 维特征 + 8 方向增强）
- ✅ 遗传算法 + 爬山法参数优化
- ✅ Elo 评分（自适应 K）
- ✅ 训练主循环 + 断点续训（checkpoint / resume）
- ✅ 训练参数落地：`gomoku_server --params`

### 计划功能
- ⏳ 位棋盘（bitboard）加速
- ⏳ 搜索侧对称去重（训练侧已实现）
- ⏳ 前端增强与对局回放分析

## 系统要求

- **操作系统**: Linux 或 WSL (Windows Subsystem for Linux)
- **编译器**: g++ 7+ (支持 C++17)
- **构建工具**: CMake 3.14+ (可选)
- **依赖库**:
  - Boost (system, thread)
  - websocketpp (自动下载)
  - nlohmann/json (自动下载)

## 安装依赖

### Ubuntu/Debian (包括 WSL)

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libboost-all-dev git
```

### 验证安装

```bash
g++ --version      # 应该 >= 7.0
cmake --version    # 应该 >= 3.14
```

## 构建

### 方法1：使用构建脚本（推荐）

```bash
chmod +x build.sh
./build.sh
```

### 方法2：使用 CMake

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 方法3：手动编译

```bash
# 下载依赖
git clone --depth 1 --branch 0.8.2 https://github.com/zaphoyd/websocketpp.git deps/websocketpp
git clone --depth 1 --branch v3.11.2 https://github.com/nlohmann/json.git deps/json

# 编译
g++ -std=c++17 -O2 -Wall -Wextra \
    -Isrc -Ideps/websocketpp -Ideps/json/include \
    src/main.cpp \
    src/core/board.cpp \
    src/core/pattern.cpp \
    src/core/evaluator.cpp \
    src/search/greedy.cpp \
    src/server/server.cpp \
    src/server/game_manager.cpp \
    -o build/gomoku_server \
    -lboost_system -lboost_thread -lpthread

# 复制前端文件
cp -r web build/
```

## 运行

```bash
cd build
./gomoku_server
```

服务器默认在端口 **8080** 启动。

### 自定义端口

```bash
./gomoku_server --port 9090
```

### 自定义前端目录

```bash
./gomoku_server --web-dir /path/to/web
```

### 开局库

```bash
./gomoku_server --book /path/to/opening_book.json   # 指定开局库文件
./gomoku_server --no-book                          # 完全禁用开局库
```

### 使用训练出的参数

```bash
./gomoku_server --params data/training/best_params.json
```

参数文件加载失败（文件缺失、字段类型错误、`five` 超过 100000 将杀阈值）时服务器**直接退出**而不是退回默认值：正在被替换的正是棋力本身，静默忽略会让人误以为训练生效了。

## 自我训练（阶段3）

训练器 `gomoku_train` 用自我对弈评估威胁系数，每轮的流程是：

1. 用遗传算法或爬山法在系数空间中搜索一个候选参数集；
2. 让候选与当前冠军对战（每对组合执黑白各一次），得到验证分；
3. 验证分 ≥ `--accept-score`（默认 0.56）才晋升为新冠军，并写入 `--out`；
4. 生成若干盘自我对弈作为训练数据（对局记录 + 特征样本，JSONL 追加）；
5. 保存 checkpoint，中断后可 `--resume` 继续。

```bash
cd build
./gomoku_train --rounds 20 --method genetic --population 8
```

常用选项（完整列表见 `--help`）：

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `--method` | `genetic` | `genetic` 或 `hill`（爬山法） |
| `--rounds` | 3 | 训练轮数；配合 `--resume` 表示**额外**轮数 |
| `--depth` / `--timeout-ms` | 4 / 400 | 训练对局的搜索深度与单步思考预算 |
| `--max-moves` | 60 | 单盘步数上限，未分胜负记为和 |
| `--fitness-games` | 2 | 评估一个候选所用的对局数 |
| `--verification-games` | 8 | 晋升前的验证对局数 |
| `--accept-score` | 0.56 | 晋升门槛（必须 > 0.5，即仅靠“不输”不能晋升） |
| `--data-games` / `--threads` | 4 / 2 | 每轮采集数据的自我对弈盘数与并发线程 |
| `--no-data` | — | 只调参，不采集数据 |
| `--no-resign` | — | 训练对局中禁止认输（认输会截断数据） |
| `--params` | 内置默认 | 起始参数文件 |
| `--out` | `data/training/best_params.json` | 冠军参数输出位置 |
| `--records` / `--samples` | 空 | 对局记录 / 训练样本 JSONL |
| `--checkpoint` | `data/training/checkpoint.json` | 断点文件 |
| `--resume` | — | 从断点继续训练 |

产物目录 `data/training/` 已被 `.gitignore` 忽略，训练不会污染仓库。

训练期间开局库始终关闭：比赛要衡量的是系数的差别，而开局库会把双方都引到同一着记忆着法上，把这个差别掩盖掉。

实测默认配置（`--depth 4 --timeout-ms 400 --max-moves 60`）下自我对弈是会分胜负的（常见 27 手内黑棋成五），但**先手优势很明显**：同一套系数互换黑白通常一胜一负，验证分正好落在 0.5。所以候选必须**执白取胜**才能越过 `--accept-score`——这是交换先手配对刻意为之，否则先手优势会被误当成参数提升。副作用是 `--fitness-games 2`（只有 1 对）时优化器能看到的信号只有 0 / 0.5 / 1 三档，几乎是个平台；要更有区分度就调大 `--fitness-games`，代价是每轮对局数成倍增加。

参数文件里的整数会做**范围校验**：JSON 用 64 位存整数而 `get<int>` 只做截断，`five: 4294967396` 会被读成 `100`，因此这类文件直接拒绝而不是套用截断值；坐标同理，只有元数据（节点数、搜索深度）允许回落到默认值。

## 使用

1. 启动服务器后，在浏览器中打开 `http://localhost:8080`
2. 选择执子颜色（黑棋先行或白棋后行）
3. 点击棋盘落子
4. AI 会自动应答

### 功能按钮

- **悔棋**: 撤销上一步（会同时撤销AI的回应）
- **求招**: 让AI给出当前最佳落子建议
- **认输**: 认输结束对局
- **终止思考**: 中断AI的当前思考
- **重新开始**: 重置棋盘开始新对局

### AI 设置

- **超时时间**: AI思考的最大时间（毫秒）
- **搜索深度**: AI搜索的最大深度

## 项目结构

```
gomoku/
├── src/
│   ├── core/           # 核心模块
│   │   ├── types.h         # 基础类型定义
│   │   ├── board.h/cpp     # 棋盘管理 + Zobrist 哈希
│   │   ├── pattern.h/cpp   # 棋型识别 + 禁手判定
│   │   └── evaluator.h/cpp # 评估函数与威胁系数
│   ├── search/         # 搜索算法
│   │   ├── greedy.h/cpp    # 贪心 / 强制着法
│   │   ├── vcf.h/cpp       # VCF 搜索 + 失败证明（find_defense）
│   │   ├── vct.h/cpp       # VCT 搜索
│   │   ├── alphabeta.h/cpp # AlphaBeta + 迭代加深 + quiescence
│   │   ├── search_engine.h/cpp # 分层调度
│   │   └── controller.h/cpp    # 异步思考控制
│   ├── optimization/   # 搜索加速
│   │   ├── transposition.h/cpp # 置换表
│   │   └── opening_book.h/cpp  # 开局库
│   ├── training/       # 自我训练（阶段3）
│   │   ├── params.h/cpp        # 可调系数向量、取值域与序列化
│   │   ├── json_int.h          # 32/64 位整数的取值范围校验（读文件用）
│   │   ├── transforms.h/cpp    # D4 八对称与规范格去重
│   │   ├── game_record.h/cpp   # 对局记录（3.1）
│   │   ├── self_play.h/cpp     # 自我对弈 + 悔棋再战（3.2）
│   │   ├── data_collector.h/cpp# 特征提取与样本采集（3.3）
│   │   ├── genetic.h/cpp       # 遗传算法（3.4）
│   │   ├── hill_climbing.h/cpp # 爬山法（3.4）
│   │   ├── elo.h/cpp           # Elo 评分（3.5）
│   │   ├── trainer.h/cpp       # 训练主循环 + checkpoint（3.6）
│   │   └── main_trainer.cpp    # gomoku_train 命令行入口
│   ├── server/         # 服务器
│   │   ├── server.h/cpp    # WebSocket + HTTP 服务器
│   │   ├── game_manager.h/cpp # 游戏管理
│   │   └── protocol.h      # 消息协议
│   └── main.cpp        # gomoku_server 入口
├── web/                # 前端文件
│   ├── index.html
│   ├── style.css
│   └── app.js
├── data/               # 开局库与训练产物（data/training/ 被忽略）
├── tests/              # 单元测试
├── CMakeLists.txt      # CMake配置
├── build.sh            # 构建脚本
└── README.md
```

## 测试

```bash
cd build
ctest --output-on-failure
```

或手动运行测试：

```bash
./test_board
./test_pattern
./test_evaluator
./test_search
./test_training
```

## WebSocket 消息协议

### 客户端 → 服务器

```json
{"type": "choose_side", "color": "black"}
{"type": "make_move", "row": 7, "col": 7}
{"type": "undo"}
{"type": "request_hint", "timeout_ms": 5000, "max_depth": 8}
{"type": "resign"}
{"type": "stop_thinking"}
{"type": "restart"}
```

### 服务器 → 客户端

```json
{"type": "ai_move", "row": 7, "col": 8, "color": "white", "score": 1234}
{"type": "thinking_progress", "depth": 4, "nodes": 50000}
{"type": "game_over", "winner": "black", "reason": "five"}
{"type": "error", "message": "Invalid move"}
```

## 开发计划

### 阶段2：高级搜索
- ✅ VCF（连续冲四胜）搜索
- ✅ VCT（连续活三胜）搜索
- ✅ AlphaBeta + Minimax 剪枝（迭代加深 + quiescence + fail-soft）
- ✅ 置换表（Zobrist哈希）
- ✅ 开局库
- ✅ 异步控制（AI 独立线程思考 + 多会话并发）

> 说明：搜索为**迭代加深的单线程** AlphaBeta；“多线程”指异步思考控制（每次 AI 落子起一个工作线程，
> 支持多会话并发与超时中断），并非同一着棋的并行搜索。

### 阶段3：自我训练
- ✅ 对局记录系统（3.1）
- ✅ 自我对弈 + 悔棋再战（3.2）
- ✅ 训练数据收集（3.3）
- ✅ 遗传算法优化 + 爬山法（3.4）
- ✅ Elo评分系统（3.5）
- ✅ 训练主循环与断点续训（3.6）

> 说明：训练对象是**威胁系数**，`five` 与 `forbidden_penalty` 不参与调参——前者是 AlphaBeta
> 的将杀阈值（硬编码 100000）与静态评估上限的公共基准，后者是规则而非棋力。
> “悔棋再战”从最靠近终局、且还有未尝试分支的那一步重新展开，配合 D4 对称类去重，
> 使重复的镜像变化不占用探索预算。

### 阶段4：优化
- 位棋盘优化
- 对称去重（训练侧已在分支探索与样本增强中实现，搜索侧待做）
- 前端增强
- 对局回放分析

## 性能目标

- 搜索速度: >100,000 节点/秒
- 内存占用: <100MB
- 响应时间: <5秒

## 许可证

MIT License

## 贡献

欢迎提交 Issue 和 Pull Request！

## 联系方式

如有问题或建议，请提交 Issue。

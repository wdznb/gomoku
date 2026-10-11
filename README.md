# 五子棋 AI 深度思考系统

一个高性能的五子棋AI系统，具备深度思考、自我训练和参数优化能力。

## 版本

`master` 是基线分支；每个阶段完成时在 `master` 上打一个带备注的标签，备注逐条列出该阶段实现的计划功能。

| 标签 | 阶段 | 实现的计划功能 |
| --- | --- | --- |
| `v0.1.0-phase1` | 阶段1 基础引擎 | 15x15 棋盘与棋型识别、黑棋禁手检测、贪心搜索、WebSocket 服务器与 Canvas 前端 |
| `v0.2.3-phase2` | 阶段2 高级搜索 | VCF / VCT 威胁空间搜索、AlphaBeta + 迭代加深 + quiescence、置换表、开局库、强制失败证明与自动认输、异步思考控制 |
| `v0.3.2-phase3` | 阶段3 自我训练 | 对局记录、自我对弈 + 悔棋再战、训练数据收集、遗传 + 爬山优化、Elo 评分、训练主循环与断点续训、`gomoku_server --params` |
| `v0.4.0-phase4` | 阶段4 优化（部分） | 位棋盘表示与形状查表（4.1）、搜索根节点对称去重（4.2）；该标签不含 4.3 前端增强与 4.4 对局回放分析 |

阶段3 的三个标签：`v0.3.0-phase3` 是功能基线，`v0.3.1-phase3` 是训练模块审查（`REVIEW_LOG.md` 轮次 49–50）后的修复版本，`v0.3.2-phase3` 是阶段收尾（文档与版本记录）后的 `master` 基线。

## 特性

### 阶段1
- ✅ 15x15 棋盘，支持标准五子棋规则
- ✅ 棋型识别（活四、冲四、活三、眠三等），含缺口棋型：`XXX_X`/`XX_XX`/`X_XXX` 识为冲四（唯一五点即缺口），`X_XX`/`XX_X` 识为跳三
- ✅ 组合棋型检测（四四、四三、三三）
- ✅ 黑棋禁手检测（三三禁手、四四禁手、长连禁手），并已修正“只能成长连的四”：黑棋的四必须有恰好成五的冲点才算威胁（`XXXX_X` 是冲四而非活四，`X_XXXX_X` 不算四）
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

### 阶段4
- ✅ 位棋盘（bitboard）加速：行/列/斜线各压成一个 64 位字，棋型识别走 2^18 形状查表
- ✅ 双方棋型单次扫描：`recognize_position_both` 用同一对线位推出黑白两色的窗口，走法排序与全盘评估不再重复解码（搜索吞吐 +19~23%，节点数与选点不变）
- ✅ 对称去重：训练侧分支探索 + 搜索侧根节点镜像候选
- ✅ 对局回放分析：逐手引擎分析（方法/分值/深度/节点/耗时）+ 回放导航 + 评估曲线
- ✅ 前端增强：落子记录可点击跳转回放、终局原因显示、AI 设置真正下发到服务器
- ✅ 摆设棋局：自定义局面（15x15 数组 / 分隔或连写的长串 / 位棋盘 / 落子序列）可从文件、报文或前端编辑器载入，自动识别黑白空标志并校验手数差、禁手与已有五连

### 计划功能
阶段1–4 的计划功能已全部落地。`REVIEW_LOG.md` 的 “Deliberately deferred” 清单也已全部关闭：
缺子棋型与连冲四判定已实现（轮次 54），搜索性能重构两项实现、一项实测后否决（轮次 55），
io 线程上的工作线程 `join` 经实测确认被单个搜索节点的耗时界定（最深 32 层 / 30 秒预算下最差
0.31 ms，且回调经 `net::post` 投递到 session executor，不存在死锁），改动风险大于收益故保持不变
（轮次 56）。

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

## 摆设棋局（自定义开局）

除了从空盘开始对局，还可以直接把一个自定义局面摆上桌：后端判断它是否合法（黑白手数差、
黑方禁手、是否已有五连），并据此算出下一步轮到谁（黑先手，因此手数相等是黑行棋，黑多一子是白行棋）。

### 三种输入形式

| 形式 | 字段 | 说明 |
| --- | --- | --- |
| 完整棋盘 | `board` | 15×15 嵌套数组、225 元行优先数组，或文本棋盘字符串：分隔（逗号 / 空格 / 制表符 / 分号 / 换行）或**连写的 225 字符长串**，一行或整个文件都行 |
| 位棋盘 | `bitboard` | `{"black":[4 个 uint64], "white":[4 个 uint64]}`，第 `row*15+col` 位表示该格，只允许 `< 225` 的位；字可以是十进制整数，也可以是带 `0x` 前缀的字符串（字符串避免 JSON 浮点丢掉 64 位精度） |
| 落子序列 | `moves` | `[row, col]` 或 `{"row":…,"col":…}` 的列表，按黑白交替自动着色，`first` 指定首手颜色（默认 `black`） |

三者只能出现一个，同时出现或全部缺失都会被拒绝。位棋盘与落子序列没有符号歧义，因此不需要
标志确认；棋盘形式必须说明黑/白/空对应哪些标志（见下）。

### 标志识别

`board` 里的每个符号按下面顺序尝试匹配，第一个能覆盖**全部**出现的符号的读法被采用：

| 黑 | 白 | 空 |
| --- | --- | --- |
| `1` | `2` | `0` |
| `1` | `-1` | `0` |
| `B` | `W` | `.` / `_` / `-` / `·` |
| `X` | `O` | `.` / `_` / `-` / `·` |
| `黑` | `白` | `空` / `.` / `_` / `-` / `·` |

字母大小写等价（`b`/`w`/`o` 同大写）。识别不了就**直接拒绝**并列出看到的符号，绝不猜：一套
棋盘里混用 `1/2/0` 与 `B/W/.` 会被拒，而不是把认不出的符号当成空、悄悄把棋子抹掉。
前端文本载入需要先确认这次读法（`check_position` 会回显识别到的标志），自动化训练与测试因为
双方共用同一套算法或同一份模板，不需要这一步——模板里写明 `encoding` 即可直接载入。

### 分隔与连写

棋盘文本先按“15 行 × 15 格”解析；行数或某行格数不对时再按整盘的**连续长串**解析，两种写法
都支持，文件和报文里都一样：

- **有分隔**：225 个用逗号 / 空格 / 制表符 / 分号分开的格子，可以换行也可以挤在一行；
- **无分隔**：一整串只含黑/白/空三种符号的字符（例如 `...BW....` 连写 225 个），按字符逐个解析，
  中文标志（`黑/白/空`）同样可以连写。连写串按 UTF-8 码点切分，不会把一个汉字拆成三格。

落到长串解析时，出现**第四种**符号会先报“连续棋盘含 N 种符号 (…)”而不是格数不对——符号种类
比长度更是问题所在；`1/-1/0` 这套标志无法连写，`1-10` 之类根本分不出格，这类数据集必须带分隔符。
长度不对时报“棋盘有 N 格，需要 225 格（15x15，分隔或连写皆可）”，而行数或行内格数错的棋盘仍按
原来的“第 1 行有 3 格，需要 15 格”提示，方便定位是排版问题还是长度问题。

### 标准模板

`data/` 下的七个模板就是给训练、复盘与测试用的标准格式，都是可被后端直接识别的合法局面
（3 黑 2 白，轮到白方）：

- `position_template.json` —— 规范文档：`version` / `type` / `encoding` / `board`
- `position_template.csv` —— 逗号分隔的 0/1/2 棋盘
- `position_template.txt` —— 每格一符号的 `B`/`W`/`.` 棋盘
- `position_flat_template.txt` —— 同样 225 格连写成一行（`B`/`W`/`.`，无分隔符）
- `position_flat_template.csv` —— 一行 225 个逗号分隔的 `1`/`-1`/`0`
- `position_bitboard_template.json` —— 4×uint64 位棋盘对
- `position_moves_template.json` —— 落子坐标序列 + `first`

导出用的规范文档由 `position_to_json(board)` 产生，前端“导出”按钮写出的是同一种形状，因此
导出的文件可以原样再载入。

### 代码入口

不依赖 JSON 与网络的核心解析层是 `src/core/position_setup.h`：`parse_grid_text`、
`detect_grid_encodings`、`position_from_grid`、`position_from_bitboard`、`position_from_moves`、
`validate_position`。JSON / 文件层在 `src/server/position_json.h`：`position_from_json`、
`position_from_text`、`load_position_file`、`position_to_json`。训练与测试可直接调用它们摆出
指定局面；服务器则用 `GameManager::set_position` 把局面装上会话的棋盘。

装上之后**只摆局面、不开新局**：状态回到 `waiting`，已有的思考线程被 join 并丢弃，回放分析清空，
AI 不会自动落子。玩家随后选择执子颜色或直接落子，这局才开始（选择颜色/落子也是唯一会清除“摆棋待定”
标记的两个动作，因此空盘之外也仍能选边）。

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

### 回放分析

- **载入本局分析**: 取回本会话逐手的引擎数据（方法、分值、深度、节点、耗时），
  之后可用 ⏮ ◀ ▶  或 **▶ 自动** 逐手回看，**退出回放** 回到当前局面
- 落子记录里的每一手都可以直接点击跳转；有引擎分析后，每条记录会附带其分值与搜索信息
- 评估曲线按**黑棋视角**绘制：引擎的分值属于刚落子的一方，因此白棋的分值取反后入图；
  红点表示该手分值超出曲线量程（已证胜负）
- 悔棋会使已载入的分析失效（手数与索引都对不上），需要重新载入

### AI 设置

- **超时时间**: AI思考的最大时间（毫秒），服务器限制在 100–30000
- **搜索深度**: AI搜索的最大深度
- 两项改动会通过 `set_config` 立即下发，对本局后续的 AI 落子生效

### 摆棋

- **选择文件**: 载入 `data/position_template.*` 那种格式的局面文件（json / csv / txt 皆可）
- **文本框**: 直接粘贴棋盘（分行、带分隔符，或 225 格连写成一串）或局面对象，配合 **识别** 使用
- **识别**: 干跑一次解析，回显黑白手数、轮到谁以及识别到的黑/白/空标志；识别成功后勾选
  “确认标志”才能 **载入**（导出数组等非文本形式无需勾选）
- **载入**: 把局面摆上服务器棋盘，状态回到“等待选边”，AI 不会自动落子
- **手动摆棋**: 点棋盘按 空→黑→白→空 循环刷格，本地计数即可看出是否合法，**识别** 会做完整校验
- **清空** / **导出**: 清掉本地编辑盘；导出规范 JSON 文档（`position.json` 下载），可原样再载入

## 项目结构

```
gomoku/
├── src/
│   ├── core/           # 核心模块
│   │   ├── types.h         # 基础类型定义
│   │   ├── board.h/cpp     # 棋盘管理 + 位棋盘行 + Zobrist 哈希
│   │   ├── pattern.h/cpp   # 棋型识别（位窗口 + 形状查表）+ 禁手判定
│   │   ├── transforms.h/cpp# D4 八对称、自对称集合与规范格去重
│   │   ├── position_setup.h/cpp # 局面载入：标志识别、棋盘/位棋盘/落子序列、合法性校验
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
│   │   ├── position_json.h/cpp # 局面文档 / 文件读写与规范导出
│   │   └── game_manager.h/cpp # 游戏管理 + 回放快照 + set_position
│   └── main.cpp        # gomoku_server 入口
├── web/                # 前端文件
│   ├── index.html
│   ├── style.css
│   └── app.js
├── data/               # 开局库、局面模板与训练产物（data/training/ 被忽略）
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
./test_position
./test_training
```

`test_position` 覆盖局面载入的三层：文本/数组/位棋盘/落子序列的解析、分隔与连写两种长串、
五种标志表与混合符号拒绝、手数差 / 禁手 / 已有五连 / 满盘校验、`data/` 里七个模板文件都能载入、
以及 `set_position` 之后不开新局、可选边、可替换、可悔到空盘的行为。

## WebSocket 消息协议

### 客户端 → 服务器

```json
{"type": "choose_side", "color": "black"}
{"type": "make_move", "row": 7, "col": 7}
{"type": "undo"}
{"type": "request_hint", "timeout_ms": 5000, "max_depth": 8}
{"type": "request_replay"}
{"type": "set_config", "timeout_ms": 5000, "max_depth": 8}
{"type": "resign"}
{"type": "stop_thinking"}
{"type": "restart"}
{"type": "check_position", "position": {"board": [[...]], "encoding": {"black": 1, "white": 2, "empty": 0}}}
{"type": "check_position", "position": "文本棋盘或局面对象字符串", "encoding": {"black": "B", "white": "W", "empty": "."}}
{"type": "set_position", "position": {"bitboard": {"black": [0, 281474976710656, 65537, 0], "white": [0, 562954248388608, 0, 0]}}}
```

`position` 可以是局面对象，也可以是文本（粘贴的棋盘——分行、带分隔符或 225 格连写成一串，
或整份 JSON 文档字符串）；顶层 `encoding` 会并入文档，作为调用方声明的标志映射。

### 服务器 → 客户端

```json
{"type": "ai_move", "row": 7, "col": 8, "color": "white", "score": 1234, "thinking_time_ms": 5000}
{"type": "thinking_progress", "depth": 4, "nodes": 50000}
{"type": "game_over", "winner": "black", "reason": "five"}
{"type": "replay", "game_over": true, "winner": "white", "reason": "resign",
 "moves": [{"row": 7, "col": 7, "color": "black", "method": "human",
            "score": 0, "depth": 0, "nodes": 0, "elapsed_ms": 0}]}
{"type": "error", "message": "Invalid move"}
{"type": "position_preview", "ok": true, "black_count": 3, "white_count": 2, "side_to_move": "white",
 "encoding": {"black": "1", "white": "2", "empty": "0"}, "board": [[...]]}
{"type": "position_preview", "ok": false, "error": "黑 3 子、白 0 子：黑先手，黑白差只能是 0 或 1"}
{"type": "position_set", "board": [[...]], "current_turn": "white", "game_status": "waiting",
 "position_pending": true, "black_count": 3, "white_count": 2}
```

`replay` 是本会话对局的逐手分析：`method` 为 `human` 表示玩家落子（无搜索数据），
其余为引擎返回路径（`book` / `vcf` / `vct` / `alphabeta` / `greedy`）；
`score` 是**刚落子一方**视角的分值，非 AlphaBeta 路径不填 `depth` / `nodes`。

`position_preview` 是干跑结果，不会改动会话状态；识别失败也用 `ok:false` + `error` 回答而不是
`error` 消息，方便表单内联显示。`encoding` 只在棋盘形式下出现（位棋盘与落子序列没有标志可言）。
`set_position` 失败才走 `error`，成功回 `position_set`，其中 `position_pending: true` 表示局面已
摆好但尚未开局：此时 `choose_side` 仍然可用，AI 不会自己落子。带 `board` 的文档必须说明 `encoding`
（文件里写好，或 `check_position` 之后由玩家确认再带上），否则会被拒绝。

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
- ✅ 位棋盘优化（4.1）
- ✅ 对称去重（4.2，训练侧 + 搜索侧根节点）
- ✅ 前端增强（4.3）
- ✅ 对局回放分析（4.4）

> 说明：4.1 把每条线（行/列/两族斜线）压成一个 64 位字，9 格窗口用 `(own << 9) | blocked`
> 索引一张 2^18 的形状表；表在首次使用时由既有的窗口解码器跑完全部键位生成（约 4.7ms，
> 常驻 256KB），因此形状语义与原实现按构造等价，标量解码只保留为测试对照。
> 同一颗格子的黑方窗口与白方窗口读的是同两条线（黑子即白方的堵点），所以走法排序与
> 全盘评估改为一次算完两色（`recognize_position_both`）；新增的差分测试逐字段比对单次
> 扫描与两次单色识别，全 225 格 × 两色 × 42 个棋盘一致，节点数与选点也逐条一致。
> 增量式全盘评估做过量化评估后放弃：一手棋影响 4 条线上 ±4 格共约 15 个候选格
> （12 子局面占候选集 27%），落子+撤销的代价与节点上那次全量重扫基本持平。
> 4.2 只在根节点丢弃镜像重复的候选着法：棋盘在某个 D4 变体下映射回自身时，同一轨道只保留
> 索引最小的格子；无自对称的中局（绝大多数局面）完全不受影响，选点与不含去重的根搜索一致。
> 4.4 由 `GameManager` 逐手记录引擎分析（落子与悔棋与棋盘同步），前端用
> `request_replay` 取回后驱动回放与评估曲线；4.3 顺带修好了“AI 设置”只作用于求招、
> 从未下发给搜索引擎的问题（现通过 `set_config` 下发）。

## 性能目标

- 搜索速度: >100,000 节点/秒
- 内存占用: <100MB
- 响应时间: <5秒

### 实测（阶段4，WSL gcc -O2，单线程）

| 指标 | 位棋盘前 | 位棋盘后 |
| --- | --- | --- |
| 单方向棋型识别 | 216 ns | 48 ns |
| 全盘静态评估 `evaluate_board` | 29.5 µs | 5.2 µs |
| 根节点吞吐 | 35,000 节点/秒 | 207,000 节点/秒 |

对称开局位置的根搜索节点数从 14,838 降到 5,241（约 2.8×），最佳着法与分值不变。

## 许可证

MIT License

## 贡献

欢迎提交 Issue 和 Pull Request！

## 联系方式

如有问题或建议，请提交 Issue。

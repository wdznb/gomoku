# 五子棋 AI 深度思考系统

一个高性能的五子棋AI系统，具备深度思考、自我训练和参数优化能力。

## 特性

### 阶段1（当前）
- ✅ 15x15 棋盘，支持标准五子棋规则
- ✅ 棋型识别（活四、冲四、活三、眠三等）
- ✅ 组合棋型检测（四四、四三、三三）
- ✅ 黑棋禁手检测（三三禁手、四四禁手、长连禁手）
- ✅ 贪心搜索算法
- ✅ WebSocket 实时通信
- ✅ 纯前端 Canvas 棋盘渲染
- ✅ 可配置威胁系数

### 计划功能
- ✅ VCF/VCT 威胁空间搜索
- ✅ AlphaBeta + Minimax 剪枝（迭代加深 + 静态/quiescence 搜索）
- ✅ 置换表（Zobrist哈希，含代龄老化）
- ✅ 开局库（对称匹配 + 加权随机）
- ✅ 强制失败证明 + 自动认输
- ✅ 异步控制（AI 独立线程思考 + 多会话并发；非并行搜索）
- ⏳ 自我训练系统
- ⏳ 参数优化（遗传算法）

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
│   │   ├── board.h/cpp     # 棋盘管理
│   │   ├── pattern.h/cpp   # 棋型识别
│   │   └── evaluator.h/cpp # 评估函数
│   ├── search/         # 搜索算法
│   │   └── greedy.h/cpp    # 贪心搜索
│   ├── server/         # 服务器
│   │   ├── server.h/cpp    # WebSocket服务器
│   │   ├── game_manager.h/cpp # 游戏管理
│   │   └── protocol.h      # 消息协议
│   └── main.cpp        # 入口
├── web/                # 前端文件
│   ├── index.html
│   ├── style.css
│   └── app.js
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
- 对局记录系统
- 悔棋再战机制
- 训练数据收集
- 遗传算法优化
- Elo评分系统

### 阶段4：优化
- 位棋盘优化
- 对称去重
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

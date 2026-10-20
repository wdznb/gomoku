# 五子棋 AI 搜索引擎架构评审与重构设计文档

| 项 | 内容 |
| --- | --- |
| 文档版本 | v1.0 |
| 评审对象 | `gomoku` 仓库（阶段 4 完成基线 `v0.5.0-phase4`） |
| 评审范围 | `src/search`、`src/core`、`src/optimization`、`src/training` 与二者之间的接口契约 |
| 评审目标 | 定位「超时/深度不可知、结果不复用、偏好边界、资源过高」四类缺陷的架构根因，并给出可落地的重构设计 |
| 结论 | 四类缺陷**同源**：当前搜索引擎是「单层贪心排序 + 全盘静态评估 + 无宽度限制的迭代加深」结构，缺少现代对弈引擎必备的**时间管理、增量复用、候选裁剪**三个子系统。建议按 4 个阶段重构，其中 A 阶段（可观测性 + 根节点复用 + 中心偏好）为低风险高收益项，应优先落地。 |

---

## 1. 现状架构评审

### 1.1 模块全景

```
                    ┌──────────────────────────────────────────────┐
   web/app.js ──WS──▶│ server/server.cpp  (Beast WS + token 会话表)  │
                    └───────────────────┬──────────────────────────┘
                                        │
                             ┌──────────▼───────────┐
                             │ server/game_manager  │  每会话一个实例
                             │  Board board_        │  ├─ board.h  (cells_ + 4×3 线位棋盘 + Zobrist)
                             │  SearchEngine engine_│  └─ search_engine.h
                             └──────────┬───────────┘
                                        │ search(board, color, depth, time, stop_flag)
                    ┌───────────────────▼───────────────────────────┐
                    │            SearchEngine（分层调度）             │
                    │  1. 候选过滤 / 单一着法快路径 / 己方成五 / 挡五  │
                    │  2. 开局库                                      │
                    │  3. VCF  (预算 35%)                            │
                    │  4. VCT  (预算 55%)                            │
                    │  5. AlphaBeta (剩余预算)  ──▶ TranspositionTable │
                    │  6. 认输探针 (VCF find_defense)                 │
                    └───────────────────┬───────────────────────────┘
                                        │
                    ┌───────────────────▼───────────────────────────┐
                    │ AlphaBetaSearch：迭代加深 + quiescence          │
                    │  generate_moves ← Board::active_empty()        │
                    │  sort_moves     ← Evaluator::evaluate_position_pair
                    │  leaf value     ← Evaluator::evaluate_board    │
                    └───────────────────────────────────────────────┘

   训练侧：training/trainer.cpp ──▶ SelfPlay（每线程一对 SearchEngine，各自持有 TT）
```

**关键观察**：评估函数 `evaluate_board` 是**唯一**决定搜索价值的函数，而它只累加棋型分（`evaluator.cpp:144-179`）；位置偏好只存在于走法排序路径（`evaluator.cpp:104-122`）。这条「排序有中心偏好、评估没有中心偏好」的错位，是「选边界」问题的结构性根源。

### 1.2 现状设计中被保留的优点

重构不应推倒重来，以下设计经实测有效，应保留：

- **位棋盘 + 形状查表**：每条线压成 64 位字、9 格窗口走 2^18 查表，单方向识别 216ns → 48ns，全盘评估 29.5µs → 5.2µs（README 实测表）。
- **`recognize_position_both` 单次扫描出双方棋型**：避免了走法排序与全盘评估的重复解码，吞吐 +19~23%。
- **候选集增量维护**：`active_index_` + `neighbor_count_` 让 `undo_last_move` 以 O(25) 恢复候选集，而不是 225 格重建（`board.cpp:99-108`）。
- **失败证明与自动认输**：`find_defense` 只在真正劣势的局面里增长成本，是很好的「按需付费」设计。
- **异步思考控制 + 会话与 socket 解耦**：`SearchController` 独立线程 + token 会话表，边界清晰，重构时不应破坏。
- **D4 对称去重**：根节点镜像候选去重，对称局面节点数 14838 → 5241。

### 1.3 四类缺陷的根因分析

#### P1｜思考时间过久（深度 6 即超时），且超时时无法得知中断于第几层

**现象**：深度 6 超时；超时后只能看到「上次完成深度」，看不到「本次中断在哪一层、搜索了多少」。

**根因链**：

1. **候选点无宽度限制**。`generate_moves` 把 `board.active_empty()` 全量返回（`alphabeta.cpp:27-38`）。`active_empty()` 是「任一棋子 Chebyshev 距离 ≤ 2 的空格」，中盘常见 60–120 个。无 Tier 分层、无 top-K 截断，分支因子 b ≈ 60–100。深度 6 在理想排序下也需 ≈ b³ 节点，实际排序质量远达不到理想。
2. **单节点成本高**。静默搜索入口先做一次 `evaluate_board`（全盘扫描 + 每候选 `recognize_position_both`，5.2µs），再对每个候选做 `recognize_position` 过滤威胁（`alphabeta.cpp:99-136`）。实测吞吐 ≈ 200k 节点/秒，12 秒预算 ≈ 240 万节点，**结构上不可能**覆盖深度 6。
3. **排序质量弱 → 剪枝效率低**。`sort_moves` 只有「一层贪心打分」（`alphabeta.cpp:40-85`），没有杀手启发、历史启发、PVS、aspiration、LMR。AlphaBeta 的节点数对排序质量极其敏感，弱排序使有效分支因子接近 b 而非 √b。
4. **战术层抢占预算**。VCF 固定吃掉 35%、VCT 固定吃掉 55%（`search_engine.cpp:12-13, 194-218`），AlphaBeta 只拿剩余；认输探针还会再花一段（`search_engine.cpp:318-351`）。**无论局面是否需要 VCF/VCT，预算都按固定比例被划走**。
5. **中断语义丢失（可观测性缺陷）**：
   - `AlphaBetaSearch` 的 `aborted_` 是**搜索级**标志：任一节点超时即置位（`alphabeta.cpp:94-97, 177-190`），此后所有层的成果一律丢弃（`alphabeta.cpp:352` 的 `!aborted_` 条件），所以 `info_.depth` 永远停在「最后一个完整层」。
   - 若**第 1 层**就超时，`info_` 保持默认值 → `depth == 0` → `SearchEngine` 把 `method` 改写成 `"greedy"`（`search_engine.cpp:297-303`），**「超时」与「没搜」在协议上不可区分**。
   - `SearchInfo` / `SearchResult` **没有** `stopped` / `partial_depth` / `seldepth` / `root_moves_searched` 字段；`thinking_progress` 只在完整层发出（`alphabeta.cpp:361-366`），所以客户端在超时期间收不到任何「第 6 层搜到一半」的信号。
   - 中断检查放在**每个节点**（`alphabeta.cpp:177`、`227`、`331`），每节点一次 `steady_clock::now()`，在高频路径上自身也是成本。

#### P2｜每次思考无法复用上次结果，或未优先考虑上次结果

**根因链**：

1. **根节点排序不传 TT 走法**。`alphabeta.cpp:310` 调用 `sort_moves(root_moves, board, color)` **没有传 `tt_move`**，而函数签名是支持该参数的（`alphabeta.cpp:40-41`）。上一手搜索在根节点存下的最佳着法，在本次根排序里被完全忽略。
2. **TT 替换策略把「代龄」置于「深度」之上**。`new_search()` 每次搜索 `current_age_++`（`transposition.cpp:37-39`），`store` 的替换条件是 `entry.age != current_age_ || depth >= entry.depth`（`transposition.cpp:13-17`）——只要代龄不同就允许覆盖，**本次搜索 depth=1 迭代的浅表项可以覆盖上一手 depth=8 的深表项**。这与「深度优先 + 代龄次之」的正确策略正好相反。
3. **单槽表 + 取模索引**。每个 index 只放一条记录，2^18 槽（`transposition.h:33, 44-51`），无 bucket、无 depth-preferred 子槽；索引 `hash % entries_.size()` 只利用哈希低位。实际搜索访问的位置数常达数百万，冲突率高，命中率被结构性压低。
4. **中断搜索一律不写表**。`aborted_` 置位后整场搜索不再写 TT（`alphabeta.cpp:255-257`）。真实对局中**几乎每次思考都会因时限被中止**，于是「上次思考」的成果基本全部丢弃——这正是「无法复用」的最大来源。正确做法是只禁止「由静态评估短路产生的返回值」入库（节点级标记），而不是全局禁用。
5. **没有 PV 可复用**。`SearchInfo::principal_variation` 在 AlphaBeta 中**从未被填充**（只有 VCF/VCT 填 `SearchResult.principal_variation`）。因此连「上次的主变」这个复用对象都不存在，更谈不上「预置对手应手 / ponder」。
6. **无 aspiration window**。根节点每次都是全窗口 `(-INT_MAX, INT_MAX)`（`alphabeta.cpp:315-316`），无法借助上次分数收窄窗口来省掉大量节点。

#### P3｜候选点充裕时倾向选择边界而非中心

**根因链**（这是四类缺陷里最"反直觉"的一条，根因不在搜索深度，而在**价值函数缺项 + 顺序不确定**）：

1. **位置偏好只进排序，不进评估**。`position_weights_` 仅被 `candidate_value` 使用（`evaluator.cpp:104-122`），而叶节点与静默节点使用的是 `evaluate_board`（`evaluator.cpp:144-179`）——**后者只累加 `pattern_score`，完全没有位置项**。也就是说：搜索在"价值"层面认为中心与边界等价，中心偏好只在"先试哪个"上起作用。
2. **偏好量级过小**。`position_weights_[r][c] = (14 - |r-7| - |c-7|) * position_weight_scale`，默认 `scale = 1`（`evaluator.cpp:23-34`、`evaluator.h:25`）→ 权重区间仅 `[0, 14]`。而 `sleepy_two = 10`、`live_two = 50`、`live_three = 500`。14 分只够做排序内的同分微调，**任何一条眠二都会压过整个中心偏好**；而它在评估里根本不存在。
3. **候选列表顺序天然从边界开始**。`rebuild_active_empty` 按 row-major 填充（`board.cpp:296-316`），`(0,0)` 在最前；`undo_last_move` 用 swap-remove 打乱顺序（`board.cpp:268-279`）。`sort_moves` 用 `std::sort`（**不稳定排序**）处理同分项，根节点又用严格大于保留先到者（`alphabeta.cpp:343`）→ **同分时"谁在候选列表前面谁赢"**，而列表顺序本身不确定、不可复现。
4. **TT 的 `tt_move` 可能是边界着法，并形成正反馈**。一旦某次浅层搜索把边界着法写进表，后续节点会把它提到最前（`alphabeta.cpp:47-54`），于是它被反复优先搜索，同分时反复胜出。
5. **兜底路径直接取 `root_moves[0]`**（`search_engine.cpp:273-286`、`alphabeta.cpp:386-388`），而 `root_moves[0]` 不具备任何中心语义。
6. **棋型识别不考虑"可用连线空间"**。贴边的活三实际价值低于腹地的活三（一侧空间被棋盘边界封死），但当前 `pattern_score` 对两者给同样的分。

#### P4｜思考资源消耗高，无法长时间自我训练

**根因链**：

1. **每节点全盘评估**。`evaluate_board` 5.2µs / 次，且在每个静默节点作为 `stand_pat` 调用（`alphabeta.cpp:99`）；`quiescence` 内还对每个候选做 `recognize_position`。单线程 ~200k nps 是这一结构的直接结果。
2. **候选分数每次节点重算**。`sort_moves` 在每个节点对全部候选重新调用 `evaluate_position_pair`（`alphabeta.cpp:69-78`），父节点已经算过的信息没有传递，候选格分数也没有缓存。
3. **Board 被反复按值复制**。`SearchController::search_worker(Board board, ...)` 按值传参（`controller.cpp:55-57`）；`AlphaBetaSearch::search` 内每层迭代再复制一次（`alphabeta.cpp:329`）。Board 内含 `vector`、多个 `array`，复制不便宜。
4. **每个引擎独立 TT，内存线性膨胀**。`SearchEngine` 内嵌 `TranspositionTable tt_alphabeta_`（`search_engine.h:77`），默认 2^18 × 32B ≈ 8.4MB；`GameManager` 每会话持有一个 `SearchEngine`（`game_manager.h:142`）。训练侧：每轮建 `candidate_engine` + `reference_engine`（`trainer.cpp:101-102`），每个数据采集 worker 再建一个（`trainer.cpp:215`）——一次自对弈同时存在 2 张表，多线程 × N。
5. **时间预算不可预测 → 训练时长不可控**。自对弈用 `timeout_ms`（`self_play.h:15`）而非节点预算，机器负载波动直接放大训练时长；且时间预算被 VCF/VCT 固定切走，实际深度浮动。
6. **优化信号弱 → 需要更多对局**。README 已记录：验证赛「颜色通吃」，`--fitness-games 2` 时优化器只能看到 `{0, 0.5, 1}` 三档，几乎是个平台；要提升区分度只能加大对局数，**资源需求随对局数线性上升**。根因是评估函数缺少位置项与形状空间修正，导致黑白胜负由先手决定而非由系数决定。

---

## 2. 重构设计

### 2.1 设计目标与非目标

**目标**

| 编号 | 目标 | 可验收指标 |
| --- | --- | --- |
| G1 | 深度 6 在 2s 硬时限内完成 | 中盘典型局面 depth 6 完成率 ≥ 95% |
| G2 | 超时可观测 | 每次思考都上报「已完成深度 / 中断深度 / 已搜根着法数 / 中止原因」 |
| G3 | 跨手复用 | 同一会话内第二手起的根节点命中率 > 30%，节点数较冷启动下降 ≥ 25% |
| G4 | 中心偏好确定 | 无战术差异时 100% 选择更靠中心的点；选点可复现（同局面同配置结果一致） |
| G5 | 吞吐提升 | 单线程 ≥ 1M nps（当前 ~200k） |
| G6 | 训练可长时间运行 | 单轮耗时下降 ≥ 40%，内存峰值与并发局数解耦（共享 TT） |

**非目标**（本期不做）

- 不改规则层（禁手判定、胜负判定、局面载入）。
- 不改前端布局与 WebSocket 帧的既有字段语义（只增字段，不改含义）。
- 不引入神经网络评估（保持纯搜索 + 手工评估，训练仍优化威胁系数）。

### 2.2 目标架构

```
┌─────────────────────────────────────────────────────────────────────┐
│ SearchEngine（分层调度，保留）                                        │
│  ① 快路径：强制着法 / 成五 / 挡五 / 开局库        （保留）            │
│  ② TimeManager ── 计算 soft/hard 时限 + 节点预算  （新增）            │
│  ③ 战术层 VCF/VCT ── 由「威胁探针」按需触发       （改造：不再固定比例）│
│  ④ AlphaBeta ── 迭代加深 + 候选裁剪 + 启发排序     （改造）            │
│  ⑤ 结果汇总：SearchStats（含中断语义）            （新增）            │
└────────────┬──────────────────────────────────┬─────────────────────┘
             │                                  │
   ┌─────────▼──────────┐            ┌──────────▼──────────────────┐
   │ MoveGenerator      │            │ TranspositionTable v2       │
   │ （新增）            │            │  4-way bucket / depth-first │
   │  Tier 分层 + top-K │            │  + static_eval 字段          │
   │  威胁着法提取       │            │  + mate 距离归一化           │
   └─────────┬──────────┘            │  + 高位混合索引              │
             │                       └──────────┬──────────────────┘
   ┌─────────▼──────────┐            ┌──────────▼──────────────────┐
   │ SearchState        │            │ SearchHeuristics            │
   │ （新增）            │            │ （新增）                     │
   │  killer[ply][2]    │            │  PV 提取 / aspiration        │
   │  history[color][i] │            │  根节点上一手最佳着法复用     │
   │  候选分数缓存       │            │  ponder 预置                 │
   └────────────────────┘            └─────────────────────────────┘

   ┌─────────────────────────────────────────────────────────────────┐
   │ Evaluator v2：pattern_score + position_term（新增）+ edge_term  │
   └─────────────────────────────────────────────────────────────────┘

  训练侧：共享只读 TT（进程级）+ 节点预算 + SPRT 自适应对局数
```

### 2.3 时间管理子系统（对应 P1 / G1 / G2）

#### 2.3.1 接口

```cpp
// src/search/search_limits.h （新增）
struct SearchLimits {
    int  max_depth      = 8;      // 目标最大深度
    int  soft_time_ms   = 0;      // 软时限：仅用于"是否继续下一层"的决策
    int  hard_time_ms   = 0;      // 硬时限：绝对截止，保证 UI 响应上限
    int  max_nodes      = 0;      // 节点预算（0 = 不限）；训练用此项替代时间预算
    bool ponder         = false;  // 是否启用预置复用
    Move hint_move;               // 上一手搜索给出的预置着法（ponder / 复用）
    int  hint_score     = 0;      // 上一手分数，用于 aspiration 中心
    bool has_hint_score = false;
};

enum class StopReason { NONE, TIME_SOFT, TIME_HARD, NODES, USER, MATE };

struct SearchStats {                 // 对外可观测的完整结果
    int  completed_depth   = 0;      // 最后一个完整跑完的层
    int  partial_depth     = 0;      // 被中断的那一层（0 = 未发生中断）
    int  root_moves_done   = 0;      // 中断层已搜索的根着法数
    int  root_moves_total  = 0;      // 中断层的根着法总数
    int  seldepth          = 0;      // 最大静默深度
    int  score             = 0;
    Move best_move;                  // completed_depth 的最优着法
    Move partial_best_move;          // 中断层的当前最优着法（供展示/诊断）
    std::vector<Move> pv;            // 主变例（新增，AlphaBeta 也填充）
    uint64_t nodes = 0, qnodes = 0;
    uint64_t tt_hits = 0, tt_cutoffs = 0;
    int  elapsed_ms = 0;
    StopReason stopped = StopReason::NONE;
};
```

#### 2.3.2 时间检查频率

把「每节点一次 `now()`」改为**每 1024 节点 + 层边界**检查一次：

```cpp
inline bool SearchContext::should_stop() {
    if (stop_flag_.load(std::memory_order_relaxed)) { reason_ = StopReason::USER; return true; }
    if (max_nodes_ && nodes_ >= max_nodes_)          { reason_ = StopReason::NODES; return true; }
    if ((++check_counter_ & 0x3FF) == 0 && deadline_ &&
        steady_clock::now() >= *deadline_)           { reason_ = StopReason::TIME_HARD; return true; }
    return false;
}
```

层边界（根节点每层开始时）**强制**检查一次，用于 soft 时限决策：`if (elapsed >= soft_time_ms && completed_depth >= min_depth) break;`

#### 2.3.3 中断语义（核心修复）

**原则**：中断不再"全局作废"，而是**分层记账**。

- `aborted_` 拆成两级：
  - `node_unsound_`：**节点级**，只表示"本次返回值来自静态评估短路，不是可信边界"，作用域限于该节点及其父节点当次 `store` 决策；
  - `search_stopped_`：搜索级，只用于控制"不再开始新工作"，**不再禁止写 TT**。
- 中断层保留 `partial_best_move`：即使该层未搜完，已完成的根着法分数仍是有效下界，可用于诊断与（在 `root_moves_done >= 1` 时）作为**备选返回**——由调用方策略决定是否采用（默认不采用，保证棋力不退化；日志/UI 展示）。
- `completed_depth == 0` 时，`stopped != NONE` 必须显式上报，禁止改写成 `"greedy"` 而不留痕迹：`method` 保留 `"alphabeta"`，另加 `stopped` 字段。
- `thinking_progress` 帧扩展为：

```json
{"type":"thinking_progress","depth":5,"partial_depth":6,
 "root_moves_done":12,"root_moves_total":34,
 "nodes":812345,"seldepth":9,"elapsed_ms":1832,
 "stopped":"none"}
```

超时收尾再发一帧 `stopped:"time_hard"`，前端即可显示「第 6 层中断（12/34），采用第 5 层结果 H8」。

#### 2.3.4 预算分配器

```cpp
struct MoveBudget { int soft_ms, hard_ms, max_nodes; };

MoveBudget TimeManager::allocate(const Board& b, int color, const SearchLimits& l) {
    // 1) 起始预算：由客户端给出 hard_ms；soft = hard * 0.65
    // 2) 局面修正：候选数越少 → 预算越短（强制着法已由快路径处理）
    // 3) 阶段修正：开局（move_count < 6）走开局库/短搜；中盘全预算；残局（空位 < 30）加长
    // 4) 稳定性修正：上一手实际耗时 > 0.8*hard 时下调 soft（避免连续超时）
    // 5) 训练模式：直接返回 {0,0,max_nodes}，时间预算不参与
}
```

**VCF/VCT 由固定比例改为按需触发**：先跑一次 O(候选数) 的「威胁探针」，判断是否存在

- 己方 ≥ 冲四 或 ≥ 活三 的候选点（进攻机会），或
- 对方 ≥ 冲四（必须防）的候选点。

只有命中才启动对应战术搜索，并给予**独立上限**（如 hard 的 20%/30%）而不是 35%/55%。无威胁局面直接进 AlphaBeta，把全部预算给主搜索。

### 2.4 复用子系统（对应 P2 / G3）

#### 2.4.1 置换表 v2

```cpp
// src/optimization/transposition.h
struct TTEntry {                     // 32B → 24B（去掉冗余 hash 高位，保留 16 位校验）
    uint32_t key16;                  // hash 高 16 位校验
    int16_t  value;                  // 归一化分值（mate 用 ±(32000-depth)）
    int16_t  static_eval;            // 新增：该局面静态分，供剪枝与 stand_pat 复用
    int16_t  depth;                  // 8 位够用，留 16 位对齐
    Move     best_move;
    uint8_t  flag;                   // EXACT / LOWER / UPPER / NONE
    uint8_t  age;
};

class TranspositionTable {
    static constexpr int kBucketWays = 4;      // 4 路组相联
    std::vector<std::array<TTEntry, kBucketWays>> buckets_;
    // 替换：同 key16 命中 → 按 depth/age 更新；未命中 → 选 (age 最老, depth 最小) 的槽
    size_t index_for(uint64_t h) const {
        return (h * 0x9E3779B97F4A7C15ULL) >> (64 - bucket_bits_);  // 高位混合
    }
};
```

**替换策略修正（关键）**：优先级改为 `depth 优先，age 次之，同深度比 age`。当前实现把 age 放在 depth 之前，是「跨手复用失效」的直接原因。同时**降低 `new_search()` 的代龄推进频率**——建议代龄按「一手棋」推进，而不是按「一次 search 调用」推进（当前 VCF/VCT/AlphaBeta 与认输探针共用一张表，一次思考内多次 `new_search()` 会让整张表在本手内反复老化）。

#### 2.4.2 mate 分数入库

当前 `best_score > -100000 && < 100000` 才写表（`alphabeta.cpp:255`），将杀分数一律不入库 → 将杀路径每手重算。改为：入库时把 `100000 ± depth` 归一化为「距根的步数」，读出时按当前 ply 还原，并按 ply 校正 mate 距离比较。

#### 2.4.3 根节点复用（三处具体修复）

```cpp
// ① 根排序带上"上一手的最佳着法"（当前漏传 tt_move）
sort_moves(root_moves, board, color, limits_.hint_move);

// ② 把上一手的 PV 第二着作为 ponder 预置（对手真走了这一手 → 整棵子树已在本手前被搜索过，TT 直接命中）
if (limits_.ponder && limits_.hint_move.pos.is_valid()) { /* 预搜对手应手 */ }

// ③ aspiration window：以 hint_score 为中心
int delta = 50;
int alpha = has_hint ? hint_score - delta : -INF, beta = has_hint ? hint_score + delta : INF;
// 失败 → delta *= 4 重搜本层（PVS 风格）
```

#### 2.4.4 PV 提取

新增 PV 提取（两种实现任选）：

- **三角表（triangular PV table）**：`pv_table[ply][...]`，节点在 alpha 提升时把 `[move] + pv_table[ply+1]` 拷到本层。实现简单，拷贝量 O(depth²)。
- **TT 回溯**：从根沿 `best_move` 递归查表直到无命中或成环。

产出填入 `SearchInfo::pv`（当前完全为空），并用于下一手的 ponder 预置。

#### 2.4.5 启发式排序（三段式）

```
① TT move（命中即置顶）
② 杀手着法 killer[ply][0..1]（同层剪枝过的着法）
③ 历史启发 history[color][cell]（按 (depth²) 累加，随搜索衰减）
④ 静态分 evaluate_position_pair（现有逻辑，作为最终兜底）
```

杀手/历史是提升 AlphaBeta 剪枝效率最廉价的手段，也是「深度 6 能否完成」的决定性因素之一。

### 2.5 候选点裁剪与中心偏好（对应 P3 / G4 / G1）

#### 2.5.1 候选分层与宽度限制

```cpp
struct CandidateSet {
    std::vector<Move> forced;     // Tier0：己方成五 / 对方成五的挡点 / VCF-VCT 强制着法
    std::vector<Move> threats;    // Tier1：能形成 冲四 / 活三 / 双威胁 的点（宽度不截断）
    std::vector<Move> vicinity;   // Tier2：邻域点（按静态分排序后取前 K）
    std::vector<Move> remote;     // Tier3：其余（仅在无威胁局面取前 M）
};
```

宽度策略（可配置，纳入训练参数）：

| 层 | Tier0 | Tier1 | Tier2 (K) | Tier3 (M) |
| --- | --- | --- | --- | --- |
| 根（ply 0） | 全 | 全 | 24 | 8（仅无威胁） |
| ply 1–2 | 全 | 全 | 16 | 0 |
| ply ≥ 3 | 全 | 全 | 8 | 0 |
| quiescence | — | 全（仅强制着法） | 0 | 0 |

**无威胁剪枝**：若局面中双方均无 ≥ 活三的威胁，则只搜索「上一手 2 格邻域 ∪ 已有棋子的 2 格邻域」内静态分前 M 的点，并允许提前返回静态分（futility pruning）。

预期效果：中盘有效分支因子从 ~60 降到 ~10–16，深度 6 的节点数下降 1–2 个数量级。

#### 2.5.2 位置偏好进入评估函数（核心修复）

```cpp
// evaluator.cpp：evaluate_board 增加位置项
int Evaluator::evaluate_board(const Board& board, int my_color) const {
    long long my = 0, opp = 0;
    // ... 现有 pattern_score 累加（保留） ...
    // 新增：棋子位置项 —— 对盘上每一颗子按中心权重累加
    my  += position_term(board, my_color);
    opp += position_term(board, opponent(my_color));
    // 新增：贴边惩罚 —— 边缘两行/两列的棋子，形状空间被封死，价值打折
    my  -= edge_penalty_term(board, my_color);
    opp -= edge_penalty_term(board, opponent(my_color));
    // ... 原有 cap 逻辑（保留，但 cap 需覆盖新增项）...
}
```

**权重标定要求**（关键，否则修复无效）：

- 中心项量级需与 `live_two`（50）同阶，例如 `position_weight_scale` 默认从 `1` 提升到 `8~16`，使中心-边角差达到 100–200 分，**在评估里可观测**；
- 边界惩罚对 `row/col ∈ {0,1,13,14}` 显式扣分；
- 两项都**纳入训练参数**（`TunableParam` 增加 `EDGE_PENALTY`、`DISTANCE_TO_STONES`），由训练器调优——这样「不选边界」不再靠硬编码，而是可学习的偏好。

#### 2.5.3 确定性 tie-break

根节点选择改为三元组比较，消除对 `std::sort` 稳定性与候选列表顺序的依赖：

```cpp
struct RootScore { int score; int center_dist; int cell_index; };
auto better = [](const RootScore& a, const RootScore& b) {
    if (a.score != b.score) return a.score > b.score;              // 分值优先
    if (a.center_dist != b.center_dist) return a.center_dist < b.center_dist;  // 同分 → 更靠中心
    return a.cell_index < b.cell_index;                            // 再同 → 最小格号，保证可复现
};
```

同时修复两处顺序依赖：
- `sort_moves` 改用 `std::stable_sort`，或显式在比较器里带 `cell_index` 兜底；
- 兜底路径 `root_moves[0]` 改为「按上述三元组取最优」，而不是列表首元素。

#### 2.5.4 棋型识别考虑连线空间

在 `pattern_score` 中对贴边棋型打折（活三/活四的一侧被棋盘边界封死时，实际威胁等级下降）。最小改动版：形状查表在生成时即区分「一侧靠边」与「两侧开阔」，返回不同的 `PatternType` 或附加一个 `open_ends` 字段。

### 2.6 评估性能子系统（对应 P4 / G5）

按「小步、可验证」顺序推进（README 已记录一次全盘增量评估被否决，因此不再一次性重写）：

**Step 1｜TT 携带 `static_eval`**
节点首次计算静态分后随 TT 一起存；命中时直接复用，省掉重复全盘扫描。低风险、立即可测。

**Step 2｜候选格分数缓存**
每个候选格缓存 `evaluate_position_pair` 分数。落子/撤销只更新受影响格（该格 ±2 邻域内、以及该格自身所在 4 条线上的候选格，约 15–25 个），而不是每节点重算全部候选。这是「排序」路径的主要成本。

**Step 3｜局部 stand_pat**
静默搜索入口不再做全盘评估，改为「上一手所在 4 条线」的局部 delta 评估（`last_move` 的 4 条线位棋盘已在 `board.lines_` 中，读取成本极低）。全盘评估只在根节点与需要精确分值的场合使用。

**Step 4｜quiescence 深度上限**
当前 `quiescence` 无深度上限（靠威胁过滤收敛）。加 `MAX_QDEPTH = 8` 与 `seldepth` 记账，避免极端局面下的长尾。

预期：Step 1+2 使排序路径成本下降 ~60%，Step 3 使静默节点成本下降 ~80%，合计吞吐 ≥ 1M nps 可期。

### 2.7 搜索算法增强（对应 G1 / G5）

按性价比排序，建议全部实现（均为 AlphaBeta 的标准增强）：

| 增强 | 收益 | 风险 |
| --- | --- | --- |
| PVS（主变例搜索 + zero-window 侦察） | 节点数 −20~35% | 低 |
| 三段式启发排序（TT/killer/history） | 节点数 −30~50% | 低 |
| Aspiration window | 节点数 −15~30% | 低（需失败重搜） |
| LMR（后排着法减深） | 节点数 −15~25% | 中（需 re-search 校验） |
| 威胁延伸（冲四/活三 +1 层） | 战术不漏，深度等效 +1~2 | 低 |
| null-move | 五子棋语义特殊，**本期不做** | 高 |
| 双威胁前置识别（四三/双三提为 Tier1） | 直接命中五子棋的核心战术 | 低 |

### 2.8 自训练资源子系统（对应 P4 / G6）

1. **进程级共享只读 TT**：自对弈的所有引擎共享一张大表（如 2^22 槽 ≈ 96MB），分片加锁或 lock-free；内存与并发局数**解耦**。当前是「每引擎一张 8.4MB 表」，N 个并发 → N × 8.4MB。
2. **节点预算替代时间预算**：训练配置从 `--timeout-ms` 改为 `--max-nodes`（保留 `--timeout-ms` 作为安全兜底）。每盘成本可预测，训练时长可控，且不受机器负载影响。
3. **SPRT 自适应对局数**：验证赛改为序贯检验（Sequential Probability Ratio Test），在统计显著时提前终止，而不是固定 8 局。解决 README 记录的「fitness 只有 0/0.5/1 三档」问题的一半（另一半靠评估函数的位置项修复）。
4. **开局多样化**：从开局库按权重采样不同开局（当前训练期开局库被完全关闭，导致"颜色通吃"）。改为「固定一组多样化开局 + 交换黑白」，让胜负由系数而非先手决定。
5. **去掉不必要的 Board 复制**：`SearchController::search_worker` 改为接收 `const Board&` 并在 worker 内自行构造可变副本（或直接在 worker 内 copy 一次），避免「传参一次 + 每层一次」的重复复制。

### 2.9 接口契约变更（向后兼容）

| 结构 | 变更 | 兼容性 |
| --- | --- | --- |
| `SearchInfo` | 新增 `partial_depth`、`seldepth`、`stopped`、`pv`、`tt_hits`、`root_moves_done/total` | 只增不改 |
| `SearchResult` | 新增 `stopped`、`partial_depth`、`interrupted` | 只增不改；`depth` 语义明确为「已完成深度」 |
| `thinking_progress` 帧 | 新增 `partial_depth`、`root_moves_done`、`root_moves_total`、`seldepth`、`stopped` | 只增不改，前端可渐进消费 |
| `set_config` 帧 | 新增可选 `soft_time_ms`、`max_nodes` | 缺省沿用现有行为 |
| 新增 `src/search/search_limits.h` | `SearchLimits` / `SearchStats` / `StopReason` | 新文件 |

---

## 3. 关键算法伪码

### 3.1 迭代加深 + 分层记账

```cpp
SearchStats AlphaBetaSearch::search(const Board& b, int color, const SearchLimits& lim,
                                    std::atomic<bool>& stop) {
    ctx_.reset(lim, stop);                       // 含 deadline / max_nodes / stop 标志
    stats_ = SearchStats{};
    Move hint = lim.hint_move;
    auto root_moves = generate_root_moves(b, color, hint);   // 分层 + top-K + tt/hint 置顶

    int score = 0; bool have_score = lim.has_hint_score;
    for (int depth = 1; depth <= lim.max_depth; ++depth) {
        if (ctx_.hard_stop()) break;                          // 层边界强制检查
        stats_.root_moves_total = root_moves.size();
        stats_.root_moves_done  = 0;

        int delta = 50, alpha = have_score ? score - delta : -INF,
                            beta  = have_score ? score + delta :  INF;
        int layer_best = -INF; Move layer_move; bool layer_ok = false;

        while (true) {                                        // aspiration 失败重搜
            layer_ok = search_root(b, root_moves, depth, alpha, beta, color,
                                   &layer_best, &layer_move, &stats_);
            if (layer_ok || ctx_.hard_stop()) break;
            if (layer_best <= alpha) alpha = -INF;            // 失败低 → 放宽
            else if (layer_best >= beta) beta = INF;
            else break;
        }

        if (layer_ok) {                                       // 完整层 → 提交
            score = layer_best; have_score = true;
            stats_.completed_depth = depth;
            stats_.score = score; stats_.best_move = layer_move;
            extract_pv(b, depth, stats_.pv);                  // 三角表或 TT 回溯
            root_moves = promote(root_moves, layer_move);      // 下次迭代先搜它
            emit_progress(/*completed=*/depth, /*partial=*/0, /*stopped=*/StopReason::NONE);
            if (is_mate(score)) { stats_.stopped = StopReason::MATE; break; }
        } else {                                              // 中断层 → 记录但不提交
            stats_.partial_depth     = depth;
            stats_.partial_best_move = layer_move;
            stats_.stopped           = ctx_.reason();          // TIME_HARD / NODES / USER
            emit_progress(depth, depth, stats_.stopped);        // 关键：让客户端看见中断
            break;
        }
    }
    if (stats_.completed_depth == 0 && stats_.stopped == StopReason::NONE)
        stats_.stopped = StopReason::USER;                     // 不允许"无声失败"
    return stats_;
}
```

### 3.2 候选生成（分层 + 宽度）

```cpp
std::vector<Move> MoveGenerator::generate(const Board& b, int color, int ply, bool in_quiescence) {
    std::vector<ScoredMove> threats, vicinity, remote;
    for (Position p : b.active_empty()) {
        if (!b.is_empty(p)) continue;
        if (color == BLACK && PatternRecognizer::is_forbidden(b, p, color)) continue;

        PositionPattern mine, theirs;
        PatternRecognizer::recognize_position_both(b, p, color, mine, theirs);
        int s = evaluator_.candidate_value(p, mine, color) + evaluator_.candidate_value(p, theirs, opponent(color));

        if (in_quiescence) { if (is_forced(mine)) threats.push_back({s,p}); continue; }
        if (is_forced(mine) || is_double_threat(mine)) { threats.push_back({s,p}); continue; }  // Tier0+Tier1
        if (near_last_moves(b, p, 2) || b.neighbor_count_at(p) >= 2) vicinity.push_back({s,p});
        else remote.push_back({s,p});
    }
    sort_desc(threats);                                    // Tier0/Tier1 全保留
    size_t K = width_for(ply, threats.empty());            // 根 24 / ply≥3 8 / 无威胁时放宽
    sort_desc(vicinity); truncate(vicinity, K);
    sort_desc(remote);   truncate(remote, threats.empty() && vicinity.empty() ? 8 : 0);

    std::vector<Move> out;
    append(out, threats, vicinity, remote);
    return out;
}
```

### 3.3 根节点确定性选择

```cpp
// 替换 alphabeta.cpp:343 的 "if (score > depth_best_score)"
RootScore cand{score, center_distance(move.pos), move.pos.to_index()};
if (better(cand, best)) best = cand;      // 分值 → 中心距离 → 格号，三元组严格全序
```

---

## 4. 实施路线

| 阶段 | 内容 | 覆盖问题 | 验收标准 | 风险 |
| --- | --- | --- | --- | --- |
| **A（可观测 + 复用 + 偏好）** | ① `SearchLimits`/`SearchStats`/`StopReason` 落地；② 中断分层记账 + `thinking_progress` 扩展 + 前端展示；③ 根排序传 `hint_move`；④ TT 替换改为 depth 优先、代龄按手推进；⑤ 位置项进入 `evaluate_board`（标定权重量级）+ 边界惩罚；⑥ 根节点三元组 tie-break + `stable_sort`；⑦ 兜底路径不再取 `root_moves[0]` | P1（可观测）、P2（部分）、P3（全部） | depth 6 超时时 UI 显示「中断于第 6 层（12/34）」；无战术差异局面 100% 选更靠中心的点且可复现；`tests/test_search.cpp` 新增「中断深度上报」「中心偏好」「选点可复现」用例 | 低 |
| **B（搜索质量）** | 候选分层 + 宽度限制 + 无威胁剪枝；三段式启发排序（TT/killer/history）；PVS；aspiration；PV 提取；威胁延伸；双威胁前置识别；VCF/VCT 按需触发 | P1（时限）、P2（PV 复用）、P3（候选顺序） | 中盘典型局面 depth 6 在 2s 内完成率 ≥ 95%；节点数较 A 阶段下降 ≥ 50% | 中（选点变化，需棋力回归） |
| **C（评估性能）** | TT 携带 `static_eval`；候选格分数增量缓存；局部 stand_pat；quiescence 深度上限 + `seldepth` | P4（吞吐） | 单线程 ≥ 1M nps；depth 8 在 4s 内完成 | 中（增量评估正确性，需差分测试） |
| **D（自训练）** | 共享只读 TT（分片）；`--max-nodes` 节点预算；SPRT 自适应对局数；开局多样化采样；去掉多余 Board 复制 | P4（训练资源） | 单轮训练耗时下降 ≥ 40%；内存峰值与并发局数解耦；`--fitness-games 2` 下 fitness 不再只有三档 | 中（多线程 TT 数据竞争） |

**每阶段的回归保障**（必须有，否则选点变化无法评估）：

1. **选点一致性基准**：固定一组局面（`data/` 下的模板 + 若干中盘快照），记录「着法 / 分值 / 节点数 / 深度」，阶段间对比。允许分值变化，但着法变化必须能解释。
2. **棋力回归**：用现有 `gomoku_train` 跑「新引擎 vs 旧引擎」的验证赛（互换黑白），以 Elo/胜率确认没有退化。
3. **差分测试**：评估函数改动（位置项、增量缓存）必须与旧实现逐格比对（沿用 `recognize_position_both` 那次「全 225 格 × 两色 × 42 棋盘一致」的测试方法）。
4. **时间语义测试**：注入 mock clock，验证 `partial_depth` / `root_moves_done` / `stopped` 在软硬时限、节点上限、用户中止三种情况下的取值。

---

## 5. 风险与权衡

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 候选裁剪改变选点，棋力可能短期下降 | 中 | 宽度参数纳入训练参数；A/B 阶段各跑一次 Elo 回归；先只对 ply ≥ 3 裁剪，根节点保守 |
| 位置项权重标定不当，出现"为靠中心而弃要着" | 高 | 位置项只作为**同分/近似同分**的偏好（量级控制在 live_two 同阶、远低于 live_three）；边界惩罚必须小于冲四价值；纳入训练可调 |
| TT 复用引入"陈旧但深度更高"的错误结果 | 中 | 保留 `key16` 校验；mate 分数按 ply 归一化；代龄按手推进而非按 search 推进 |
| 增量评估正确性（README 已有一次否决记录） | 高 | 按 Step 1→4 小步推进，每步都做差分测试；Step 3（局部 stand_pat）先只用于"无威胁局面"，有威胁时回退全盘评估 |
| 多线程共享 TT 的数据竞争 | 中 | 先分片加锁（16 片），再考虑 lock-free；训练侧先单线程验证正确性 |
| 时间语义与前端 `深度 × 2` 秒规则冲突 | 低 | 前端规则改为「目标深度 + 软/硬时限」，保留旧字段兼容；`set_config` 新增可选字段 |
| 四阶段改动量较大 | 中 | A 阶段独立可交付（纯可观测 + 偏好修复），B/C/D 各自可独立回滚；每阶段打 tag（沿用现有 `v0.x-phaseN` 惯例） |

---

## 6. 附录：文件级改动清单

| 文件 | 改动 |
| --- | --- |
| `src/search/search_limits.h` | **新增** `SearchLimits` / `SearchStats` / `StopReason` |
| `src/search/alphabeta.h/.cpp` | 拆分 `aborted_` 为节点级/搜索级；分层记账；PV 提取；aspiration；PVS；根三元组 tie-break；层边界时间检查；`stable_sort` |
| `src/search/move_generator.h/.cpp` | **新增** 候选分层、宽度限制、无威胁剪枝、威胁/双威胁识别 |
| `src/search/search_state.h` | **新增** killer 表、history 表、候选分数缓存、PV 三角表 |
| `src/search/search_engine.h/.cpp` | 接入 `SearchLimits`/`SearchStats`；VCF/VCT 按需触发；`TimeManager` 预算分配；`depth==0` 不再静默改写为 `greedy`；根节点传 `hint_move`；兜底路径确定性化 |
| `src/search/time_manager.h/.cpp` | **新增** 预算分配（软/硬时限、节点预算、阶段修正） |
| `src/search/controller.h/.cpp` | `search_worker` 不再按值收 `Board`；透传 `SearchLimits` 与上一手 `hint` |
| `src/optimization/transposition.h/.cpp` | 4 路 bucket；depth 优先替换；高位混合索引；`static_eval` 字段；mate 归一化；代龄按手推进 |
| `src/core/evaluator.h/.cpp` | `evaluate_board` 加入 `position_term` / `edge_penalty_term`；权重标定；`pattern_score` 考虑连线空间 |
| `src/core/board.h/.cpp` | 暴露 `neighbor_count_at(pos)` 供候选分层使用；`rebuild_active_empty` 顺序不再被下游依赖 |
| `src/training/params.h/.cpp` | 新增可调参数：`EDGE_PENALTY`、`DISTANCE_TO_STONES`、`CAND_WIDTH_ROOT`、`CAND_WIDTH_DEEP` |
| `src/training/self_play.cpp` | 节点预算；共享 TT；开局多样化 |
| `src/training/trainer.cpp` | SPRT 自适应对局数；共享 TT；引擎复用 |
| `src/training/main_trainer.cpp` | 新增 `--max-nodes`；保留 `--timeout-ms` 为兜底 |
| `src/server/game_manager.cpp` | 透传上一手 `hint_move`/`hint_score`；`thinking_progress` 新字段下发 |
| `web/app.js` | 展示「中断于第 N 层（x/y）」；思考进度条按 `partial_depth` 更新 |
| `tests/test_search.cpp` | 新增：中断深度上报、中心偏好、选点可复现、候选宽度、TT 跨手复用、差分评估 |
| `tests/test_training.cpp` | 新增：节点预算、共享 TT 并发安全、SPRT 终止 |

---

## 文档状态：已被 v2.0 取代（2026-10-20 续写）

**后续评审与演进设计见 `docs/ENGINE_REDESIGN_V2.md`。** 该文件承接本版四阶段（A/B/C/D）落地后的实测读数（REVIEW_LOG 轮次121–141），
针对用户新提出的三个问题给出新设计：① 单线程 1M nps 达不到（实测 228,000 nps）时 CPU 内核还能改什么、GPU 该放哪；
② 深度 6 以上「谁先手谁赢」的根因与修法（规则级交换 + 连续训练信号 + 深搜蒸馏，而不是加深搜索）；
③ 自我训练多进程化（broker + worker）、绑核治理与网页参数控制面（单一真源 + 预览效果）。

本文件保留为 v1.0 原稿：A/B/C/D 四阶段的机制清单与逐条落点以本文件的章节号为准
（`src/` 与 `tests/` 的注释按 `ENGINE_REDESIGN.md :NNN` 绝对行号引用本文件，**因此本文件不再增删行**）。

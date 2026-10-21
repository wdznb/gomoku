# 实现指导书：ENGINE_REDESIGN_V2 剩余单元

> 读者：接手实现的 AI / 工程师。
> 规格来源：`docs/ENGINE_REDESIGN_V2.md`（v2.1，1051 行）。本文件**不替代**它——设计意图看那份，动手细节看这份。
> 基线提交：`origin/master` = **`a9dfeb1`**（轮次147）。开始前先 `git pull` 并确认 `git log --oneline -1` 是这个 hash 或它的后代。

---

## 0. 怎么用这份文档

1. 先读 §1（环境）与 §2（纪律）。**§2 的六条不是建议，是这个仓库付过学费的规矩。**
2. 按 §3 的单元顺序做，一个单元一次 commit + push。
3. 每个单元都给了「状态 / 目标效果 / 实现思路 / 注意 / 验收」。**验收里带「杀线」的，做不到就回收该单元，不许落半截。**
4. 任何数字必须标「实测」或「未测/预估」。这是本仓库的文档纪律，写报告时会被检查。
5. 卡住时先看 §5「已知坑清单」，那里是前几轮真踩过的。
6. **有多种实现路线的地方看 §8**：每条岔路都写了首选、为什么只能选它、什么读数出现才换；§8.2 是「已被实测否掉的路线」，提这些路线之前先读出处。
7. **每个单元都要自带回归用例，且每个用例都要有超时**，写法与逐单元清单见 §9。

---

## 1. 环境

| 项 | 值 |
| --- | --- |
| 主机 | **就是你现在所在的这台 Linux 主机**；代码在 `~/Desktop/workspace/gomoku` |
| 主机 | Ubuntu，kernel 7.0.0-38，**6 vCPU**，VMware 虚拟机（只有 SVGA 虚拟卡，**AMD 卡未直通，GPU 路径不可验证**） |
| 工具链 | cmake 4.2.3、g++ 15.2.0、node v22、无 ninja |
| 构建 | `cmake --build build -j6 --target <目标>`；`build/` 已配好离线 JSON（`FETCHCONTENT_FULLY_DISCONNECTED=ON` ＋ `build/_deps/json-src`），**不要重新 configure 成联网模式** |
| 测试 | `ctest --test-dir build`（8 个套件，**约 500 s**，`test_search` 占 470 s） |
| 推送 | origin 是 SSH（`git@github.com:wdznb/gomoku.git`），主机 `~/.ssh/id_ed25519` 认到 `wdznb`。**HTTPS 无凭据，别用** |
| **实现者位置** | **你就在这台 Linux 主机上**：直接编辑 `~/Desktop/workspace/gomoku`，直接构建、直接 `git push`。**不需要 ssh，也不需要 tar 同步** |

### 1.1 改代码的正确姿势

**你就在这台主机上**，直接编辑 `~/Desktop/workspace/gomoku` 里的文件、直接 `cmake --build`、直接 `git push`。没有同步环节，所以不会出现「本地改了但远端没同步」这类问题。

仍然要守三条：

1. **一次只改一个文件（或一对必须同时改的 .h/.cpp），改完立刻编译**。多文件一起改，编译错误会掩盖真正的失败点，定位成本翻倍。
2. **不要用 `sed -i` 或脚本做多锚点替换**：含转义的锚点容易不匹配，行级替换会命中同名子串——本仓库曾因此把 `analyze_main.cpp` 打坏两次，只能 `git checkout` 回滚。用编辑器精确改，或改完立刻编译验证。
3. **批量脚本一律用 `&&` 串联**：脚本中途失败时，后面的命令不能照跑（尤其是「改完就 commit」这类链）。

> 如果你是从 Windows 侧编辑副本（另一种工作方式），才需要 `本地改 → tar cf - <文件> | ssh ... tar xf -` 这条链路。那种方式下**命令必须用 `&&`**，否则脚本断言失败后同步照跑，会把半成品推到远端。

---

## 2. 六条纪律

| # | 规矩 | 为什么 |
| --- | --- | --- |
| 1 | **一次一个文件，编译通过再下一个** | 多文件一起改，编译错误会掩盖真正的失败点 |
| 2 | **命令串联一律 `&&`** | 脚本中途失败时，后面的命令不能照跑 |
| 3 | 编译失败 → `git checkout -- <该文件>` 立即回滚 | 已验证这是最快恢复路径 |
| 4 | 新增用例至少一条**变异臂**：改错一条断言 → 期望非零退出并报出行号，然后还原 | 纯 `assert` 的用例不验证一遍，等于没写（轮次144 起执行） |
| 5 | 改 `src/` → 必须跑完整 `ctest`；只改 `tools/`、`web/`、`docs/` → 可以不跑，但必须在提交信息里写明理由 | `ctest` 里没有 `tools/` 目标 |
| 6 | 每个新机制必须带**杀线**（什么读数出现就作废） | 先例：威胁延伸、LMR、futility 三轮都是这样回收的 |

### 2.1 每次提交前的固定门禁

```bash
node tests/readme_anchor_check.js     # 期望：120 citations, 0 problems
node tests/web_frontend_check.js      # 期望：105/105 passed
ctest --test-dir build                # 期望：8/8 passed（改 src/ 时）
git push origin master                # 然后核对 git log --oneline -1 origin/master
```

> `readme_anchor_check` 扫描 `src/`、`tests/`、`web/` 里所有 `README:NNN` 形式的绝对行号引用。**给 README 插入一行会让后面所有引用位移**，所以要么追加到文件末尾，要么改完立刻重跑这支检查并按报错修正。

---

## 3. 当前状态

### 3.1 已完成（不要重做）

| 交付 | 提交 | 内容 |
| --- | --- | --- |
| 复盘漏斗段 A | `37b0fbd` | `ReplayAnalyzer`：零搜索筛查（摆幅/方法迁移/必挡连跑/深度塌陷/将杀穿越），两条剔除（开局、无信息）＋ 第三条（`replay`/`book` 不是决策） |
| 段 B / 段 C | `37b0fbd` | `ReplayReviewer`：定预算复核、精确复算（`SearchEngine::exact_root_score`）、六种判定、难度阶梯 |
| 题卡 | `37b0fbd` | `TacticsCard`（§6.6 全部字段）、JSON 往返、系数指纹 |
| CLI | `37b0fbd` / `59e3dca` | `gomoku_analyze --replay / --review / --cards / --cards-dir / --bench / --layer / --params` |
| 题卡分表 | `59e3dca` | `proven/`（杀棋题，与系数无关）vs `judged/`（判断题）＋ `index.json` |
| 样本 teacher | `a9dfeb1` | `TrainingSample::teacher_move` / `teacher_source`；`--export-samples` |
| `root_scores` | `37b0fbd` | `RootMoveScore{move, score, exact}`，穿过 `SearchStats → SearchInfo → SearchResult` |

已有数据：`data/tactics/proven/hard.jsonl`（2 张漏杀）、`data/tactics/judged/normal.jsonl`（2 张俗手）、`data/tactics/index.json`。

### 3.2 待做（17 个单元）

甲-3、甲-4、乙-1、乙-2、丙-1..4、丁-1..6、戊-1..3。见 §4。

### 3.3 明确不做

- **L 阶段全部**（`WavefrontBatch`、`BatchEvaluator`、GPU 后端）——构建主机是 VMware 虚拟机，没有真 GPU，**没有仪器就不开工**。
- v1.0 已判不落地的三条（威胁延伸、LMR、futility）不复活。
- 神经网络评估不在范围。

---

## 4. 单元说明

### 甲-3 复盘面板（协议 ＋ 前端）

**状态**：未开始。协议层没有任何 `analyze_*` 帧；前端只有回放（`replay` 帧）与胜率曲线，没有判定标记。

**目标效果**：一局结束后（或手动点「复盘」），服务端对整局跑一遍段 A＋段 B，边跑边推「第几手 / 共几手」，跑完推每手的判定；前端在**已有的胜率曲线**上打标记（俗手红点 / 妙手绿星 / 唯一应手灰线 / 反例紫方块），点标记跳到那一手，并给出「摆棋测试」入口。**不新增页面、不新增端口。**

**实现思路**

1. **协议（服务端）**
   - `src/server/server.h` 的 handler 声明区（约 `:184` 之后）加：
     `void handle_analyze_request(std::shared_ptr<WebSocketSession> session, const json& msg);`
   - `src/server/server.cpp` 的 `handle_message` 分发链（`:715-743`）加一个 `else if (type == "analyze_request")`。**它是会话级帧，所以不要动 `:705` 那个白名单**（白名单只管 `ping`/`check_position` 这类不需要会话的帧）。
   - 新 handler 里：
     - 用 `get_int_or(msg, "depth", 8)` / `get_int_or(msg, "budget", 20000)` 读参数（`server.cpp:25-40` 的两个 helper 已防 `{"row":"7"}` 这类异常）；
     - 从 `GameManager::replay_snapshot()`（`game_manager.cpp:334-351`，`thinking_mutex_` 下拷贝）取整局；把 `MoveAnalysis` 逐字段转成 `GameRecord::moves`（`row/col/color/score/depth/nodes/method/...` 一一对应）；
     - **起一个后台线程**跑 `ReplayAnalyzer` ＋ `ReplayReviewer`（都在 `src/training/replay_analyzer.h`），每完成一手 `send_message(session, analyze_progress)`，最后发一帧 `analyze_result`；
     - 玩家正在思考（`ai_thinking()`）时**直接拒绝**，回一帧 `error` 说明「AI 正在思考，复盘会偷走它的时钟」，不排队。
   - 线程安全：`WebSocketSession::send` 内部走 `net::post`（`server.cpp:352-370`），**后台线程可以直接调**；但 socket 关闭后不能再写，所以要么在 `clear_game_callbacks`（`:1660`）里 join/置停，要么用 `weak_ptr` ＋ `expired()` 检查。

2. **协议（帧形状）**

   | 方向 | 帧 | 字段 |
   | --- | --- | --- |
   | C→S | `analyze_request` | `depth`（可选，默认 8）、`budget`（可选，默认 20000）、`max_candidates`（可选，默认 12） |
   | S→C | `analyze_progress` | `done`（已复核手数）、`total`（候选手数） |
   | S→C | `analyze_result` | `moves[]`：每项 `{ply, kind, best:{row,col}, played:{row,col}, loss, gap, layer}`；`coeffs_sha`；`cards`（本次写出的卡数，若启用） |

   `kind` 取值：`blunder` / `brilliant` / `forced` / `counterexample` / `missed_mate` / `unlisted` / `none`。

3. **前端**
   - `web/app.js` 的 `switch (msg.type)`（`:1693-1751`）加两个 `case`：`analyze_progress` → 更新状态行「复盘 12/34」；`analyze_result` → 把结果写进 `this.analysis[i].verdict`（`this.analysis` 与 `this.moveHistory` 下标对齐，见 `constructor:78`）。
   - `drawEvalGraph`（`:905-961`）里**只追加标记绘制**：在 `forEach` 里把 `verdict` 挂到 `points` 上，在既有的红点绘制（`:953-960`）之后按类别换色/换形状。
   - 点击：`canvas` 已有 click 处理（`:963`），按 x 反算下标后调 `showReplayMove(index)`（`:781-793`）。
   - 面板 DOM：`web/index.html` 的 `#region-left`（`:23-35`）里加一个 `<section id="analyze-panel" hidden>`，内含「复盘」「摆棋测试」两个按钮与一个结果列表；`web/style.css` 末尾加 `.panel / .panel-title / .mark-*`，复用既有 `.field`、`.btn-tiny`。

**注意（逐条都会让你返工）**

1. **`drawEvalGraph` 的几何一位都不能动**：`limit = 2000`、`x = width * (i + 0.5) / n`、`y = height / 2 - (clamped / limit) * (height / 2 - 6)`、`|score| >= 99999` 才画点。`tests/web_frontend_check.js` 有两条用例逐点校验（约 `:1138-1195` 与 `:1679-1721`）。**加标记只能新增绘制**。
2. 前端测试桩**没有 `clientWidth/clientHeight`**，走 `200×64` 回退分支；新画的任何东西都要能在回退尺寸下算出来。
3. `resumeApplied` 为真时不会自动进回放视角（`app.js:739-742`），面板要「开局即复盘」就必须显式调 `showReplayMove(0)`。
4. `web_frontend_check.js` 的 105 项**全是行为断言**（元素桩属性、实际发出的帧、canvas 调用坐标），不是源码字符串匹配。新用例**只许追加到文件末尾**，并且不要依赖用例之间的全局元素状态（元素桩是单例，需要先显式清空，参考 `:3733-3735` 的注释）。
5. `tt_hits` 服务端已经下发（`server.cpp:1068`）但前端丢弃了，做面板时可以直接消费，**不需要改协议**。
6. 不要为复盘新开端口。仓库有过一次压测把临时端口池打满导致机器硬重启的记录（`docs/PROMPT_STRESS_TEST.md`）。

**验收**

- `tests/test_server.cpp` 用既有 `Harness`（`:218-234`）＋ `drain_frames` 加两条：
  - 发 `analyze_request` 后，**至少收到 1 帧 `analyze_progress` 与恰好 1 帧 `analyze_result`**，且 `analyze_result.moves` 长度 == 对局手数；
  - AI 思考中发 `analyze_request` → 收到 `error`，且**没有** `analyze_*` 帧。
- `tests/web_frontend_check.js` 末尾追加 ≥3 条：面板默认隐藏；收到 `analyze_result` 后标记数 == 带判定的手数；点击标记后 `replayIndex` 变成对应下标。
- **杀线**：`drawEvalGraph` 的折线坐标或点阈值改一位 → 既有 105 项里必须变红。跑一次验证：故意把 `limit` 改成 2001，确认红，再改回。

---

### 甲-4 题库门禁接进流程

**状态**：`tools/tactics_bench.sh` 已存在并能跑，但没接进任何流程，`--cards` 的注释也过时了。

**目标效果**：`build.sh` 之后能一条命令跑三档 `solve_rate`，README 有一行指引，人和 AI 都知道这道门禁存在。

**实现思路**

1. `tools/tactics_bench.sh` 的注释里 `--cards data/tactics/judged/normal.jsonl` 改成 `--cards-dir data/tactics`（甲-1 已改 CLI，脚本注释没跟上）。
2. `build.sh` 末尾加一段**注释**说明「构建完成后建议跑 `tools/tactics_bench.sh`」（不要真的塞进 `build.sh` 执行——构建脚本不该跑 8 秒的门禁）。
3. `README.md` **追加到文件末尾**（不要插在中间）：一行说明题库门禁的命令与判据。

**注意**

- README 的行号被 `readme_anchor_check` 扫描，**插入行会让所有 `README:NNN` 引用位移**。所以只能追加到末尾，改完立刻跑 `node tests/readme_anchor_check.js`。
- 门禁判据是「与上一 tag 的读数比，任何一档掉 > 2 个百分点即红」；0 %/100 % 的告警**不算红**（那是预算选错了，换一档再读）。

**验收**

- `tools/tactics_bench.sh data/tactics` 输出三档 `solve_rate`（当前基线：`@4000 0/4`、`@8000 0/4`、`@32000 0/4`，实测）。
- `node tests/readme_anchor_check.js` → 0 problems。

---

### 乙-1 D：位置项差分表（225 × 2 × 42）

**状态**：未做。这是 v1.0 `:556` 欠下的账：A5 位置项落地时没有交差分表。

**目标效果**：证明「位置项 / 贴边项」的实现与「按定义逐子重算」在 42 副棋盘 × 225 格 × 2 色 = **18,900 组**上完全一致，产出 `data/diff/position_term.csv` 存档。

**实现思路**

1. 新 `tools/position_term_table.cpp`，CMake 加一个目标（照 `gomoku_analyze` 的写法，链 `GOMOKU_CORE_SOURCES`）。
2. 棋盘集从 `tests/pinned_positions.h`（乙-2 建）取 42 副；没有就先在工具里内联 42 副（`data/position_*.json` 已有模板可参考）。
3. 对每副棋盘、每一格、每一色输出一行：`board,row,col,color,position_term,edge_penalty_term,recomputed_position_term,recomputed_edge_penalty_term,match`。
4. 「重算」= 直接遍历 `move_history()`，按 `position_weights_[r][c]` 与「外两行/列」的定义求和，**不复用增量路径**。

**注意**

- `position_term` / `edge_penalty_term` 是 `Evaluator` 的 **private** 成员函数（`evaluator.h:117-118`）。仓库既有先例是用 `#define private public` 在测试 TU 里打开（`tests/test_search.cpp` 就这么做），工具里也可以，但**只能在工具/测试里用**，不要污染 `src/`。
- 这一项**不改任何生产代码**，它只是把已有的实现钉住。所以它不会让任何读数变好——它的价值是「以后改评估器时有一张表能比对」。
- 输出文件放 `data/diff/`，不要让 `build` 每次生成（会污染工作区）；用命令行参数指定输出路径。

**验收**

- CSV 恰好 **18,900 行数据**（不含表头），`match` 列全为 1。
- 变异臂：把 `edge_penalty_term` 的「外两行/列」改成「外三行/列」→ 表里必须出现 `match=0`。

---

### 乙-2 F：选点基准搬进树内

**状态**：未做。选点一致性基准目前只存在于 `REVIEW_LOG` 的 `/tmp` 读数里，树内没有常驻臂。

**目标效果**：把 pos24 与两副 d8 臂做成树内常驻用例，任何人改引擎都能立刻看出「着法/分值/节点数」有没有变。

**实现思路**

1. 新 `tests/pinned_positions.h`：42 副普查棋盘 ＋ pos24 ＋ 两副 d8 臂（用 `std::array<Position, N>` 或直接一串 `Move`）。
2. **单独一个 test 目标 `test_pinned`**（不要塞进 `test_search`）：这些臂是定深搜索，d8 一副要数秒，塞进 `test_search` 会让本来 470 s 的套件更慢，而人一旦开始跳过 `ctest`，门禁就死了。
3. 用例只断言三件事：`best_move` 的格号、`score`、`nodes`。**分值允许变化时必须写明理由**，着法变化必须能解释（v1.0 `:554`）。

**注意**

- 这些读数来自 `REVIEW_LOG` 轮次137–141，**搜索前必须 `set_max_nodes` ＋ 给一个不 binding 的时限**。`time_limit_ms = 0` 会被 `search_engine.cpp:256-260` 兜到 100 ms，层切割点跟着机器忙闲走，读数就不可比了（轮次121 B5 踩过）。
- `test_pinned` 要加进 `CMakeLists.txt` 末尾那个 `foreach(... -UNDEBUG)` 列表，否则 Release 构建下 `assert` 会被编译掉。

**验收**

- `./build/test_pinned` 全绿，且打印每副棋盘的 `move/score/nodes`。
- 变异臂：把某一副的期望 `nodes` 改 ±1 → 必须红。

---

### 丙-1 O5：战术证明入库（`ProofTable`）

**状态**：未做。依据是一条实测：**`VCFSearch` / `VCTSearch` 完全不读写共享置换表**（轮次121 B5 为论证门控等价性顺带量到的，`vcf.cpp` / `vct.cpp` 里没有任何一次 probe/store）。后果是同一份「这里能杀」的证明被买了三次。

**目标效果**：同一次思考里，VCF 证明过的杀，AlphaBeta 不再重爬；下一手同一会话里，已证明的局面直接命中。**答案一个字都不许变。**

**实现思路**

1. 新 `src/search/proof_table.h`：`2^16` 项 × 24 B ≈ 1.5 MB，条目 `{uint32 key16, uint8 verdict, uint8 distance, Move best_move}`，`verdict ∈ {WIN_FOR_side, LOSS_FOR_side}`，**没有 UNKNOWN 这个存储态**。
2. 写点两处：
   - `VCFSearch::search_impl` 返回 `VCFResult::WIN` 时写 `WIN`，距离 = `variation.size()`；
   - `find_defense` 返回 `DefenseResult::PROVEN_LOSS` 时写 `LOSS`。
   - **`UNKNOWN` 一律不写**——「没证明」不是信息。
3. 读点两处：
   - `VCFSearch::vcf_attack` 进节点先查，命中 `WIN` 直接返回 `100000 + dist`；
   - `AlphaBetaSearch::alphabeta` 在 TT 探测之后、候选生成之前查一次，命中就整节点短路。
4. 代龄按手推进（与 A3 给 TT 定的规则同口径），`new_search()` 不老化它。

**注意**

- **命中必须返回 `±(100000 + dist)`**，不能返回 `±100000`：`search_engine.cpp:349` 的认输判定与 `transposition.h:98` 的距离还原都依赖这个约定。
- **任何一臂出现着法或分值变化 → 立即回滚**。证明表的语义是「同一个答案换一条路径」，出现新答案就是 bug，不是收益。
- `ProofTable` 每会话一张，跟着 `SearchEngine` 走；训练侧每 worker 一张（与 TT 同生命周期）。
- 这张表**不进 `SearchResult`、不进帧**。

**验收**

- 在同一批根上（轮次121 B5 的 275 根臂）跑加表前后：`nodes` 必须下降，`method` 分布与 `score`/`best_move` 必须逐根相同。
- 报两个数：**命中占比**（`alphabeta` 命中 `ProofTable` 的节点数 ÷ 总节点数）与**节点降幅**。
- **杀线**：命中 < 3 % → 作废（1.5 MB 换不到可测的节点数）；命中达标但节点降 < 2 % → 也作废（说明命中的节点本来就被 TT 或杀手剪掉了）。

---

### 丙-2 O2：`BoardValueCache`（**文档原文写错了，按这里做**）

**状态**：未做。**注意：`ENGINE_REDESIGN_V2.md` §2.2 O2 的前提是错的**——它说「`pattern_score` 对每颗已占子调用」，实际 `pattern_score` 是对**空候选格**调的（`evaluator.cpp:279/289`），已占子只进 `position_term` / `edge_penalty_term`（`:208-233`，那里不调 `pattern_score`）。所以「已占子键缓存」打不到那 20.85 %。**轮次153 实测：这一半已经在树里**——`CandidateCache::board_score`（§2.6 Step 2/3，轮次122-123）就是这份每格棋盘和价缓存，键是四条线字、失效判据是线字键比对，命中由 `test_search` 的 C2-4 钉着（quiet BLACK d6：8,755,599 probes／4,949,934 命中 = 56.5 %）；状态应为**已做**，本轮交的是点名而树里缺的两张验收（18,900 组逐格差分 ＋ cap 不进缓存）。

**目标效果**：把 `evaluate_board` 里每个空候选格的**棋盘和价**（`board_pair_value`）缓存起来，键不变就复用，把 20.85 % 的 `pattern_score` 大部分省掉。

**实现思路**

1. 照 `CandidateCache` 的形状（`src/search/search_state.h:226-350`）：每格一份记录，键是**四族线字**（`uint64_t key[4]`，黑白各 32 位拼成一个字，见 `line_key` `:339-345`）＋ `valid`。
2. 缓存的是 `board_pair_value(my_pattern, opp_pattern, row, col, black_is_mine)` 的返回值——**注意它与 `candidate_pair_value` 是两个数**：前者不含中心权重、含禁手判定、会被 cap；后者是排序价。
3. `evaluate_board`（`evaluator.cpp:276-293`）改成读穿缓存：键相同用旧值，键不同重算并更新。
4. 缓存挂在 `SearchContext` / `SearchState` 上，**跟着一次思考生死**，不要放进 `Board`。

**注意**

- **cap 不在缓存里**：`evaluate_board_from` 最后才 cap（`:270-272`），缓存的是未 cap 的和。缓存 cap 后的值会在系数变化时静默出错。
- 失效判据必须是**线字键**，不是「±2 邻域」：实测过 (7,7) 会因 (7,12) 落子而变（轮次122），邻域判据会漏。
- 键比对成本是 4 × `uint64` × 候选数；若「命中省的钱不比键贵」，本项作废。

**验收**

- 42 副棋盘 × 225 格 × 2 色 = 18,900 组，缓存值与现算值**逐格相同**（复用乙-1 的棋盘集）。
- 报 `Ir/节点` 与 `ns/节点` 前后对比；固定针臂的着法/分值/节点数**一个不许变**（这是纯成本改动）。
- **杀线**：`Ir/节点` 降幅 < 5 % → 作废。

---

### 丙-3 O3：排序分与威胁分分离

**状态**：轮次154 实测判**不落地**。前提先纠正：排序路径**不重识别**——`sort_moves` 的 ③/④ 段（`alphabeta.cpp:88-98`）读的是丙-2 那条缓存的记录（`cell()` 命中就是一次加法），识别与两色求值只发生在失算那一次，`CandidateCache::cell` 9.50 % ＋ `recognize_position_both` 8.66 % 是失算的成本、不是每节点排序的成本。机制按本单元原样量过（威胁等级键）：六臂每一次宽度截断的 kept 集合有 **99.45 %~100 %** 与今天不同（每份被截的 tier 只有 3.6~4.0 个等级，宽度却要留 8-16 个），杀线直接命中；成本侧「`cell()` 填两色价」整条路实测 ≈ 13.56 % Ir，够不到本单元 `≥ 30 %` 那一格（需要 354.5 `Ir/候选格`，整条路只有 160.2），而文档 `ENGINE_REDESIGN_V2.md:187` 自己的估值是 6-8 %。读数、口径与未测项见 REVIEW_LOG 轮次154。

**目标效果**：排序路径改成 O(1) 线字判定，两色全量配对分只保留在叶子（`stand_pat` / `evaluate_board`）。

**实现思路**

1. `MoveGenerator::generate`（`move_generator.cpp:145+`）已经为了分 Tier 跑过一次 `recognize_position_both`，**先看它能不能直接给出「行棋方四线的成五/成四/活三」布尔量**，而不是再算一次 `candidate_pair_value`。
2. 排序键改成「Tier → 线字威胁等级 → 中心距离 → 格号」的四元全序，与 A6 的三元组保持同一个方向。
3. `candidate_pair_value` 保留，但只被叶子调用。

**注意**

- **这不只是顺序改动**：`generate` 同时用配对分决定 Tier 归属与宽度截断。改它会影响**哪些格子被保留**，进而改着法。所以验收必须是「着法与分值不变」，而不是「节点数下降」。
- 仓库有三次「机制建好后判不落地」的先例（威胁延伸、LMR、futility）。这一项若着法变了且解释不了，直接判不落地。
- 以 `Ir/候选格` 单一读数决生死（轮次140 的口径）。

**验收**

- 固定针臂（乙-2 的 pos24 ＋ 两副 d8）着法/分值/深度**全同**；`Ir/候选格` 下降 ≥ 30 %。
- **杀线**：任何一副针臂换答案 → 作废。

---

### 丙-4 O1：`finish_direction` 升格为线段局部 LUT

**状态**：轮次155 实测判**不落地**（表建对了，等价性四条全过：75,600 组、18,900 格聚合字段、531,441 键 × 窗外 8 变体全枚举、六臂着法/分值/节点一字未动——但成本是倒退）。本单元的杀线「取窗指令数超估值 2 倍」命中：查询里 13 格 → 三进制下标实测 **100.0 Ir/次**（`line_key_at` self 14.65 % ÷ 397,312,421 次），而 §2.2 O1 给的是「约 10 条指令」；整节点 Ir 四臂 d6 **+5.86 %**（79,147,997,595 → 83,786,118,514）、六臂 **+7.20 %**，黑棋连基础窗型都不查的最省形式仍 **+5.21 %**。根因是「占 14.45 % Ir」被当成了可替换的稳态成本，实测它是 777,032,656 次 × **50.1 Ir/次** 的累积——绝大多数调用在 `color == BLACK` 那两道门前就返回了，走线根本不跑。建表成本另记：-O2 80.5 ms／-O0 708.3 ms。读数、六支变异臂与还原过程见 REVIEW_LOG 轮次155。

**目标效果**：把「13 格三态 → 修正后的 `PatternType`」做成一张表，把走线变成一次表读。

**实现思路**

1. 键 = 以候选格为中心、沿 `dir` 的 **13 格三态**（EMPTY / BLACK / WHITE，出界按 blocked），`3^13 = 1,594,323` 项，`uint8`（基础类型 3 位 ｜ 黑棋修正类型 3 位）≈ 1.6 MB。**中心格恒为黑**（`pattern.cpp:350` 的 `own |= 1u << kCenterBit`），所以只有 `3^12` 个不同键。
2. **建表复用现有 `finish_direction` 的纯函数副本**，跑满一次生成——不要重写判断逻辑，否则会有第二份语义。
3. 查询：从 `board.line_bits(color, dir, line)`（`board.h:76-77`）＋ `LINE_MARGIN = 8`（`board.h:19`）取位 8..22，shift + mask 出 13 格，再一次表读。
4. 建表成本单独记进启动开销（文档 O4），不许藏在吞吐数字里。

**注意（三条前提，缺一条就做不成）**

1. **必须把两处无界 `while` 与 `run_length` 在 ±6 截断**：`pattern.cpp:421-432`（`five_points` 的走线）、`:465-467`（`painted` 的扩张）、`:384`（`run_length() >= 6`）。截断必须证明不改答案——骨架：穿过探针的连续段 ≤ 4（否则窗型已是 FIVE/LONG_FIVE），凑成五只需看 `4 - f` 格，判定格 ≤ ±5；`painted` 只需知道「连续段超过 4」，±6 足够。
2. **键必须是三态，不能把 EMPTY 和 WHITE 合并**：`five_points` 与活三段用的是 `== EMPTY`，而 `fill_run` / `painted` 把两者都当阻断——两种用法不同，合并会错。
3. **基础 `PatternType` 不进键**：它由键中央 9 个 trit 唯一决定（`pattern_lut_`，`pattern.cpp:353`），进键只会让表大 6 倍。

**验收**

- **42 副棋盘 × 225 格 × 4 方向 × 2 色 = 75,600 组**，LUT 值与现函数值**逐组相同**，1 组不一致即弃。
- 同一棋盘集上再逐格比 `recognize_position_both` 的聚合字段，确认误差没从另一条路径渗回来。
- 报该函数的 Ir 降幅与整节点成本降幅（目标 11–13 %，**预估**）。
- **杀线**：75,600 组有 1 组不一致 → 作废；取窗指令数超估值 2 倍（省下的只是函数调用开销）→ 作废。

---

### 丁-1 S3：训练信号从二值胜负换成连续量

**状态**：轮次156 **已落地**（缺省 `w = 0`，等同今天的形状）。`TrainingConfig::fitness_margin_weight` + `--fitness-margin-weight`，`Trainer::game_margin`（`trainer.cpp:113-165`）三个读数全从 `GameRecord` 现算，`Trainer::blend_fitness`（`:95-101`）执行 `w = 0` 的逐字恒等。**实现思路 2 的第一条按 §711 改过来源**：`Evaluator::evaluate_board` 扫的是空格的潜力，盘面上已成五的终局实测只给 −1,814／+304，用它当「终局优势」会把「赢定了」评成「没有优势」；现改成赢方最后一手与输方最后一手各自报出的搜索分差（尺度 20,000）。读数：单元层 0.855／0.655／±0.1 全是手算钉值，真赛 0.25（格点上）→ 0.325（格点外）；CLI 层 `--seed 20261011 --fitness-games 2` 下 w=0 读 1.0/0.5、w=0.35 读 0.879687/0.556875，同 seed 两遍逐字节一致，晋升结论一字未动（杀线未触发）；`--seed 4242` 那组（3 轮 × 20 邻域、depth 3）证明换轴确实改变选择——三个 w 的冠军系数互不相同，w=0 三次晋升、w=0.35 一次。**W 只进适配度，验证赛的 SPRT 序列永远是纯胜负。**ctest 9/9（755.03 s）、memcheck test_training clean（reports=0，544 s）、变异臂六支各断在钉值上。

**目标效果**：给适配度加连续分量，让「赢得多漂亮」也进入信号，而不只是「赢没赢」。

**实现思路**

1. 改 `Trainer::measure_fitness`（`trainer.cpp:38-49`）：现在它返回 `play_match(...)` 的胜负对分数；改成「胜负对 × (1-w) ＋ 连续分量 × w」。
2. 三个分量全部从**现有产物**取，不加新基础设施：
   - 终局静态分差：终局局面 `Evaluator::evaluate_board` 的两色差，归一化到 [0,1]；
   - 先成五一方用了多少手：`GameRecord::moves.size()` 归一化；
   - 认输时的深度差：`MoveRecord.depth`。
3. 权重是可训练参数（`TrainingConfig` 加字段），并且**必须可关**（`w = 0` 时与今天逐字节一致）。

**注意**

- **同 seed 结论不许翻面**。轮次126 有先例：`--max-nodes` 把每手搜浅之后，同 seed 的验证赛结论从 0.875 晋升翻成 0.375 不晋升。翻面 → 权重下调直到消失；下调到 0 仍翻 → 作废。
- `measure_fitness` 里要把 `five` / `forbidden_penalty` 对齐到冠军（`trainer.cpp:50-53`），新分量不能破坏这一步。

**验收**

- `--fitness-games 2` 下适配度取值**档数从 5 涨成连续**（报直方图或唯一值个数）。
- 同 seed 跑两轮，晋升结论一致。
- **杀线**：同 seed 翻面且权重降到 0 仍翻 → 作废。

---

### 丁-2 S4：深搜蒸馏（`teacher_move` 进 `MoveRecord`）

**状态**：轮次157 **已落地**，缺省 `--teacher-depth 0` = 关（关着时记录与样本逐字节是 S4 之前的形状，四个老师键是可选键）。四条注意全部照做并有断言守着：老师用 `teacher_tt_`（不是 `shared_tt_`，`trainer.cpp:352-359`）、在 `board.place_stone` 之前搜（重放记录可核：它答的那一格当时必须为空）、`teacher_depth` 记请求值而不是 `result.depth`、`RetryExplorer::reset()` 改成回到起始线。读数：`--data-games 8 --threads 2 --depth 2 --max-nodes 1500 --seed 20261011` 下样本 **456 → 672**（战术层 ply 第一次进来，+47 %），整轮墙钟 134 → 147 s（+9.7 %），每轮对局数不变 ⇒ 杀线不触发；师生一致率 67.9 %，随轮次下降（74.5 % → 59.5 %）。五支变异臂各断在钉值上，其中「老师改用学生的表」那一支复现了真实污染（`ply 1 student moved: (6,7) d2 n76 vs (6,6) d2 n10`）。ctest 9/9（765.13 s）、memcheck test_training clean（reports=0，752 s）、TSan 3 worker 共享老师表 0 条 race。师生**棋力**差没有量过（不在本单元单子里）。落地位置与读数详见 REVIEW_LOG 轮次157。

**目标效果**：每手棋由「当时的引擎」和「一个更贵的老师」各给一个答案，两者分开记；样本因此第一次覆盖战术层 ply。

**实现思路**

1. `src/training/game_record.h` 的 `MoveRecord` 加 `Move teacher_move` / `int teacher_depth` / `int teacher_score`（只增字段，放在 `tt_hits` 之后），`game_record.cpp` 的 `to_json` / `from_json` 各加三个可选键（旧记录读回为空 = 没有老师）。
2. `SelfPlayConfig` 加 `teacher_depth` / `teacher_max_nodes`（0 = 关）；`SelfPlay::run` 在 `engine.search`（`self_play.cpp:76`）之后、`place_stone`（`:115`）之前，用**独立引擎 ＋ 独立 TT** 再搜一次，写进 `entry.teacher_*`。
3. `DataCollector::collect`（`data_collector.cpp:155-166`）把 `entry.teacher_move` 搬进 `sample.teacher_move`，`teacher_source = "distill"`。

**注意**

1. **教师必须用独立 TT**。worker 与 `shared_tt_` 共用一张表（`trainer.cpp:286`），教师若在同一张表上写 d6 条目，随后的 d4 搜索会直接切在 d6 条目上，`entry.depth` 与 `search_depth` 失真，样本口径被污染。
2. **教师必须在落子前搜**，否则局面已变。
3. **vcf/vct/must_block 不回填 `depth`**（`search_engine.cpp:179/225-250`），教师深度要显式记 `config.teacher_depth`，不要指望 `result.depth`。
4. **顺手修一个 bug**：`RetryExplorer::reset()`（`self_play.cpp:163-168`）会清掉 `set_start_line` 设的开局（`trainer.cpp:296` 只设一次），所以重试那一局是从空盘开始的。改成保留 start line。

**验收**

- 样本里出现 `teacher_source == "distill"` 的记录；**样本覆盖 vcf / must_block 的 ply**（今天这些 ply 全丢）。
- 报「teacher 与实际一致的比例」随轮次的变化。
- **杀线**：教师搜索贵 k 倍，若每轮对局数塌到 1/4 以下 → 只对 `--fitness-games` 的对局做蒸馏。

---

### 丁-3 S6：黑白分离的系数（`CoefficientPair`）

**状态**：已落地（轮次158）。`Evaluator` 改持 `CoefficientPair`（`evaluator.h:157`），每一处系数按摆子的那一色取；两侧同值时整棵树与改之前逐字节相同（`test_pinned` 六臂一字未动，实测 246.49 s）。基因 14 → 28（`params.h:45 PAIR_GENE_COUNT`，索引 `side * 14 + gene`），旧文件读成两侧同值、只有分开才多写一个 `"white"` 子对象，换系数集清 TT 已进 `SearchEngine::set_coefficients`。**没给出来的那一项见下面注意 5 的括注：`kParams` 里没有「防守权重」这个基因。**

**目标效果**：黑白各一套系数（14 个可调量 × 2），且**反对称性不许破**：`eval(W) == -eval(B)` 是 negamax、TT 单符号约定、根层比较三处的地基。

**实现思路**

1. `params.h` 加 `struct CoefficientPair { ThreatCoefficients black, white; }`，附 `static CoefficientPair shared(const ThreatCoefficients&)`；旧文件（单份系数）读进来时两侧同值。
2. `Evaluator` 改持 `CoefficientPair`，并新增 `set_coefficients(const CoefficientPair&)`；旧签名保留为 `shared(c)` 的薄包装（**旧调用点一行不用改**）。
3. 签名加 `color` 的三处：
   - `pattern_score(const PositionPattern&, int row, int col, int color)`（私有）
   - `get_position_weight(int row, int col, int color)`
   - `board_pair_value(..., int my_color)`（把原来的 `bool black_is_mine` 换掉）
4. `position_weights_` 变 `[2][BOARD_SIZE][BOARD_SIZE]`，`init_position_weights()` 跑两遍。
5. `evaluate_board_from` 的帽改成 `min(pair.black.five, pair.white.five) - 1`——**帽必须与颜色无关**，否则反对称在帽处就破了。

**注意（这是全项目最容易破不变量的一项）**

1. **反对称的唯一支点是 `evaluator.cpp:267/270`**：两侧必须按「**子的颜色**」取系数（`my_color` / `opponent(my_color)`），不能按「我方/对方」取同一套。`eval(W) = S_W − S_B`、`eval(B) = S_B − S_W`，这样才恒等取负。
2. **换系数集必须 `tt.clear()`**：TT 的 `static_eval` 只存 BLACK 符号（`transposition.h:44-52`），这个约定只在同一套系数内成立。
3. **序列化要兼容旧文件**：`kParams` 是一基因一行（`params.cpp:42-63`，落地后仍是 14 行，按色只在向量与 JSON 这两层边界展开）、`to_json_string` 是扁平结构（`:280-288`）、checkpoint 的 `versions[]` 就是那个扁平对象（`trainer.cpp:626-629`）。单份文件读成「两侧同值」，不要造新格式。
4. **`sanitize` 与 `CrossRule` 必须逐色各跑一遍**（`params.cpp:180-211`、`:518-544`），否则一套合法、另一套越界的文件会被静默接受。
5. **威胁阶梯两侧必须同阶（±20 %）**：否则「白方」就是另一个引擎，Elo 失去意义。允许两侧不同的是位置项 / 贴边 / 候选宽度 / 防守权重——**注意：`kParams` 里没有「防守权重」这一基因**（14 个可调量是 9 个威胁 + `position_weight_scale` / `distance_to_stones` / `edge_penalty` / `cand_width_root` / `cand_width_deep`），所以这一项在这一版是**给不出来的**，丁-4 只能在四项里挑。
6. 基因数从 14 变 28，`TUNABLE_COUNT` 与 `CoefficientVector::to_vector/from_vector` 都要跟着改；`measure_fitness`（`trainer.cpp:41-57`）与 `score_match`（`:180-257`）假定「候选 = 一套系数」，要重定义为「候选 = 一对」。

**验收**

- **等价臂先行**：`pair.black == pair.white` 时，整棵树的着法/分值/节点数与今天**逐字节相同**。
- `eval(W) == -eval(B)` 在 42 副棋盘 × 225 格上成立（`tests/test_evaluator.cpp:549-554` 那条断言的加强版）。
- 旧参数文件 / 旧 checkpoint 能读进来，读成两侧同值。
- **预期红灯（落地时已逐条改，未删一条断言）**：变红的是 `tests/test_evaluator.cpp` 的三个辅助函数与两处用例——`naive_board_total` 与 `black_forbidden_candidates` 改成按色调 `pattern_score` / `get_position_weight`，`clamp_board_diff` 的帽取两侧 `five` 的较小者，`test_evaluate_board_never_judges_white` 与 `test_edge_penalty_below_rush_four` 里共五处补上颜色实参。（旧版这里列的是改之前那份文件里的行号区间，行号随新用例漂移，改为按**函数名**指认。）

---

### 丁-4 S6 后半：平衡控制器

**状态**：轮次159 实测判**不落地**，代码一行未写（`src/` 与 `tests/` 未动，HEAD 仍是丁-3 的 `f6084ef`）。两半各自量过：**判据那一半**是算术——穷举 95 % Wilson 区间，每色 n ≤ 3 局**永远**不含 0.5（所以 `fitness_games = 2` 那侧结构上是哑的），每色 4 局（= 今天 8 局换色验证赛）只有 **4-0** 才点火、且 balanced 过程误点火率 **12.5 %**，「8 局的噪声不该触发」只在**每色 8 局**那一行成立。**动作那一半**是实测：null fixture（两侧同一套系数）在缺省档 32 局里白座位**全胜**、16 个换色对全部同座位通吃，关认输与提到 depth 6 都一样；把它允许动的四项拧到**超过 5 步封顶 10~30 倍**的量级（白侧 `position_weight_scale` 8 → 0 与 8 → 20、`cand_width_root` 24 → 2，后者连平均手数都从 22 掉到 16）六臂读数**一字不变**；而白侧 `rush_four` 1000 → 300（威胁阶梯，丁-3 的 ±20 % 带与丁-4 注意 1 双重不许动）一读就把 16 局全白胜翻成 **16 局全黑胜**。⇒ 判据每轮点火、点的是座位差，动作空间推不动它，本单元验收句「否则回对称系数并作废」与思路 4「到 5 步仍偏 → 交给 S1」在第一轮就会触发，而 S1 是丁-5。读数、Wilson 表与工具（`/tmp/d4/{wilson,null_match,lever}.cpp`）见 REVIEW_LOG 轮次159；建议丁-5 之后回头重测，届时那六臂就是现成的对照基线。

**目标效果**：每轮训练后读一次**按色胜率**，只对落后那一侧走一步有界修正，让训练赛不一边倒。

**实现思路**

1. 新 `src/training/balance.h/.cpp`：`BalanceController`，输入 `{wr_black, wr_white, games}`，输出「改哪一项、改多少」或「不动」。
2. 判据用 **Wilson 区间**与 0.5 比较，只有区间不含 0.5 才算「偏了」——8 局的噪声不该触发动作。
3. 动作：只改**落后那一侧**的 `position_weight_scale` / `edge_penalty` / `cand_width_*` 中的一个，步长 ±10 %，**每轮最多一步**，累计 5 步封顶。
4. 到 5 步仍偏 → 停手，把问题交给 S1（规则级补偿），并在报告里写明「系数补偿到头了」。
5. `solve_rate` 掉 2 个点 → 回滚该步并冻结本机制。

**注意**

- **威胁阶梯两侧不动**（丁-3 的约束），这里只动位置/贴边/宽度/防守权重。
- 按色胜率会**被这套机制本身改变**，所以强度读数只认题库 `solve_rate` 与整树赛 Elo，**不认按色胜率**。
- 默认关（`train_balance_step = 0`）。

**验收**

- 三步内按色胜率差必须缩小；否则回对称系数并作废。
- 每一步都要有一条 `train_status` 记录「改了哪一项、从多少到多少、读数变化」。

---

### 丁-5 S1：开局规则参数化（swap / soosyrv）

**状态**：未做。**这一项改的是五子棋规则本身**，不是引擎内部。**轮次159 把丁-4 的量交给这里**：null fixture（两侧同一套系数）在训练缺省档 16/16 局由座位定胜负，系数侧四项拧到 −100 %/+150 % 推不动一格——所以「让训练赛不一边倒」的正确杠杆是开局规则，不是系数。做完这一项之后再回头看丁-4 值不值得重做（轮次159 的六臂是现成的对照基线）。

**目标效果**：`--opening-rule none|swap|soosyrv`，默认 `none`（与今天一致）。让先手优势有**规则级**补偿，而不是靠搜索补偿。

**实现思路**

1. `TrainingConfig` 与 `GameManager` 各加一个 `opening_rule` 字段，**两者读同一份定义**（否则网页上换了规则、训练器不认）。
2. `swap`（swap2）：黑下第 1、3 手，白在第 2 手后决定是否交换颜色；`soosyrv`（Soosõrv-8 简化）：黑第 1 手天元、白第 2 手、黑第 3 手，白再决定是否交换。
3. `game_manager` 要暴露「交换回合」给前端（一个新的 C→S 帧 `choose_swap`），训练器侧在 `score_match` 的开局阶段走同一套判定。

**注意**

- **老记录不可比**：换规则前后的对局不能放在一起算胜率。换规则前导出旧记录，读数另标规则名。
- 训练侧的 `--match-opening-plies` 与这个规则是**两件事**（前者是重放固定开局，后者是交换颜色），不要混。
- 默认必须 `none`，且 `none` 下整条链路与今天逐字节相同。

**验收**

- 同一套系数下跑 48 条开局，报两个数：**「一方通吃比例」**与**「颜色决定的对局占比」**，对照组是 `none` 规则的同 seed 结果。
- **杀线**：`swap` 下仍 > 60 % 开局一方通吃 → 说明该强度下这些开局本身就是必胜局，换规则不解决问题 → 转 S2；若「适配度赛钉在 0.5 的比例」不降反升（轮次129 读到过 32.5 % → 38.8 %）→ 记为反向证据，权重下调而不是硬上。

---

### 丁-6 S2：开局集按双方胜机筛选（无 GPU 版）

**状态**：未做。文档原本把这一项放在 GPU 批处理上，但 **L 阶段已取消**，所以退回顺序循环。

**目标效果**：开场前对每条候选开局做「黑白各自下的胜机评估」，只保留双方都有胜机的开局进训练集；结果落盘复用，不在线筛。

**实现思路**

1. 新函数（放 `trainer.cpp` 或新 `src/training/opening_screen.h/.cpp`）：对每条候选开局，黑白各跑 K 次短局（低深度、定节点），读「成五手数」与「终局分差」。
2. 判据：双方胜率都落在 `[0.25, 0.75]` 才算「都有胜机」；否则剔除。
3. 结果写 `data/training/openings.json`（含 `coeffs_sha`，系数换代要重筛）。

**注意**

- **顺序循环就是全部并行度**：没有 GPU、没有 `WavefrontBatch`（L 阶段取消）。K 与候选数要小，否则筛选成本超过省下的训练局数。
- 筛选判据选错时，**换判据而不是放宽阈值**。
- 落盘文件要带 `coeffs_sha`：换系数后旧筛选结果失效。

**验收**

- 筛前 / 筛后各报一遍「一方通吃比例」，筛后应压到 **1/3 以下**。
- **杀线**：筛选成本 > 省下的训练局数 → 降 K 并复用落盘；两者都救不回来 → 作废，退回「固定 8 条轮换」。

---

### 戊-1 H：进程模型（`gomoku_broker` ＋ `gomoku_train --worker`）

**状态**：未做。现在是**进程内 `std::thread`**（`trainer.cpp:268-327`），没有 broker、没有 worker 模式、没有绑核。

**目标效果**：supervisor 进程（broker）派活给 N 个 worker 进程，每个 worker 绑一个物理核；玩家下棋时 worker 让位；崩溃只死一个任务。

**实现思路**

1. 新 `tools/broker_main.cpp` → `gomoku_broker`：读配额、写任务文件、tail 各 worker 的 JSONL、汇总状态、看门（退出码非 0 重排）、在轮末跑 `BalanceController`。
2. `main_trainer.cpp` 加 `--worker <root>` 模式：从任务文件读活、跑完写记录、退出。**复用现有 `Trainer`，不复制训练逻辑**。
3. IPC：**文件 spool ＋ UDS，不新开 TCP 端口**。大流量（记录/样本/逐手统计）追加写文件，每行带 `run_id`/`seq`；控制（起停/心跳/参数版本）每 worker 一条 UDS。
4. 绑核：`sched_setaffinity` 绑 `reserved_cores … n-1`；`reserved_cores` 默认 2（1 给 asio/GameManager，1 给玩家那手搜索）。
5. 玩家一开始思考 → broker 把部分 worker 置 `PAUSED`（不是杀），玩家那手结束再 `RESUME`。

**注意**

1. **不许新开 TCP 监听**：仓库有过一次压测把临时端口池打满、导致机器硬重启（`docs/PROMPT_STRESS_TEST.md`；TIME_WAIT 峰值 5011 = 熔断线 8000 的 63 %）。
2. **半行写要丢弃并重排任务，不许续行**——续一行被截断的 JSON 是数据污染，不是健壮性。
3. `--threads` 的语义已经是糊涂账（不带数据采集时是惰性的：user 56.50 vs wall 57.06，三份 RSS 只差 212 kB，轮次125 实测）。进程模型要把资源契约写成显式的「N 个 worker 进程」。
4. **Windows 回退**：`CreateProcess` ＋ Job Object ＋ `SetProcessAffinityMask`；两者都不可用 → 退回线程模式并**显式上报**「本机为线程模式」。**不允许「两边都有但一边没测」的状态进树**。
5. TT 本来就是每引擎一张（轮次127 实测共享表平坦，+71 kB/worker），进程边界与共享表边界天然对齐。

**验收**

- 1 / 2 / 4 worker：报每秒节点数与每轮墙钟；**每 worker 的 nps ≥ 单进程的 90 %**（绑核生效、没有互相踩 cache）。
- `SIGKILL` 一臂 ＋ 半行写一臂：broker 的任务账与文件账在 N 秒内对齐。
- 三档（全训 / 半训 / 边下边训）：报一次人类 think 的 p50/p99；**边下边训档 p99 相对全停档劣化 ≤ 20 %**（超过说明绑核没生效，不是机器不够）。

---

### 戊-2 I：参数单一真源 ＋ 网页控制面 ＋ 预览

**状态**：未做。参数现在散在三处：`src/training/params.h`（可训练系数）、`src/main.cpp`（启动旗标）、`web/app.js` 的 `set_config`（只下发 `timeout_ms` / `max_depth`，`app.js:646-651`）。

**目标效果**：一份 `ParamSpec` 定义同时喂服务端、训练器、前端；前端零硬编码；面板能改、能预览、能解释。

**实现思路**

1. 新 `src/server/param_schema.h`：`ParamSpec{ key, group, type, min, max, default, step, unit, when_live, desc_plain, effect_note, suggest }`；`ParamRegistry` 是它的单例。
2. 帧：
   - C→S `config_schema`（拉清单）、`config_set`（改一个，带 `preview` 布尔）、`config_preview`（跑固定针臂，不回填）；
   - S→C `config_schema`（清单）、`config_ack` / `config_reject`（改参结果，越界要点名）。
3. 前端：拿到 schema 后**动态渲染**表单，`min`/`max`/`default`/`suggest` 全部来自服务端；每次开新对局、起训练、开复盘面板都先拉一次 schema，**不缓存默认值**。
4. 三类 `when_live`：`live`（立即生效）/ `next_think`（下一手/下一轮）/ `restart`（要重启，面板禁用并写明）。
5. 预览：跑固定针臂（quiet / scattered × 两色 ＋ pos24），回填「完成深度 / 节点数 / 墙钟 / nps / 着法 / 分值 / `solve_rate@8k`」；**预览不改任何状态**，玩家思考时直接拒绝。

**注意**

- **前端零硬编码**是一条纪律，要写一条用例守住：`web_frontend_check` 里加「`app.js` 不含默认参数硬编码」。
- 参数清册见 `docs/ENGINE_REDESIGN_V2.md` §5（四组：人类对局 / 训练 / 复盘题库 / 系统资源），每条都要能查到同一行；**表格改而代码不改等于没有单一真源**。
- 服务端对每个 `config_set` 做范围校验，越界 `config_reject`，**不截断、不静默改**（沿用轮次74/86 的哨兵风格）。

**验收**

- `test_server`：`config_schema` 形状（每条都有 8 个字段）、越界拒绝、三类 `when_live` 各一例。
- `web_frontend_check` 追加：面板由 schema 渲染（改 schema 的 `max`，输入框 `max` 跟着变）、不含默认参数硬编码。
- 预览两次读数一致；预览前后不改参数时，正式 think 的读数与预览同形（允许时间项抖动）。

---

### 戊-3 K：一键训练（五相状态机）

**状态**：未做。CLI 现在只有「从头跑 N 轮」或「从 checkpoint 续 N 轮」（`main_trainer.cpp:420`），没有分相入口。

**目标效果**：`train_autopilot` 把六个要人手串的动作串成一个**可中断、可恢复、可解释**的循环。

**实现思路**

| 相 | 做什么 | 出口 | 失败时 |
| --- | --- | --- | --- |
| `PREPARE` | 读 checkpoint、生成/复用开局集、起 worker、绑核 | 开局集就绪 ＋ 心跳齐 | 缺开局集 → 现场筛；筛失败 → 退回固定 8 条 |
| `COLLECT` | 自对弈产 records/samples（含 teacher） | 局数或节点预算到 | 单 worker 死 → 重排该任务，不重跑整相 |
| `OPTIMIZE` | 遗传/爬山在适配度上搜 | 代数/停滞代数到 | 适配度全等 → 记「饱和」并跳 `BALANCE` |
| `VERIFY` | 候选 vs 冠军：SPRT ＋ 按色胜率 ＋ 题库解题率 | SPRT 出界或局数到顶 | 解题率掉 > 2 点 → 拒绝晋升（即使胜率好看） |
| `BALANCE` | `BalanceController` 走一步或不动 | 一步走完 | 见丁-4 杀线 |

**注意**

- 状态机放 **broker**，不放前端：关掉浏览器不能停训练。
- `train_pause` 在**相边界**生效（不打断一手棋）；`train_stop` 写 checkpoint 后退出，`train_start` 从 checkpoint 续。
- 每相结束发一帧 `train_status`，带「这一相花了多久、产出了什么、下一步为什么是它」——这是一键不变成黑箱的唯一办法。
- **一键的边界**：只自动做上面五相，不改代码、不换算法、不调机器。

**验收**

- **同 seed 跑两次，相序列、每相出口条件、最终 checkpoint 的系数逐字节相同**（除时间字段）。这条比任何胜率读数都硬。
- 中途 `train_pause` 后再 `train_start`，能从断点续且结果与不中断一致。

---

## 5. 已知坑清单（前几轮真踩过的）

| # | 坑 | 现象 | 处置 |
| --- | --- | --- | --- |
| 1 | 用 python/sed 打多锚点补丁 | 含转义的锚点会不匹配（heredoc 还会吃掉转义），改一半就退出 | 用编辑器精确改；脚本里每个锚点 `assert count == 1` |
| 2 | 行级 find/replace | `cards written` 命中了同名子串，把 `analyze_main.cpp` 打坏 | 用唯一锚点，改完立刻编译 |
| 3 | 换行串联命令 | 前一条失败后，后一条照跑（曾把半成品推到远端） | 一律 `&&` |
| 4 | 写 dotfile 存脚本 | 某些沙箱/工具下 dotfile 不落盘，脚本「成功」但文件不存在 | 用普通文件名（如 `patch_j.py`） |
| 5 | `readme_anchor_check` | 给 README 插一行，后面所有 `README:NNN` 位移 | 只追加到末尾，或改完立刻重跑 |
| 6 | `web_frontend_check` | 105 项是**行为断言**，`drawEvalGraph` 几何被逐点校验 | 加标记只追加绘制 |
| 7 | `time_limit_ms = 0` | 会被兜到 100 ms，层切割点跟机器忙闲走 | 复盘/定深臂一律 `set_max_nodes` ＋ 不 binding 的时限 |
| 8 | `RetryExplorer` | 记录里 53 % 的 ply 是 `replay`（回退重放），不是决策 | 段 A 已剔除；做样本/统计时也要排除 |
| 9 | `ponder_node_budget` | 是**节点帽**（4000–200000）不是墙钟 | 定价按节点算，不按时钟算 |
| 10 | 根表尾部是**界**不是分 | 第一版段 B 拿表里的分数做差，12 手全 `loss 0` | 已加 `RootMoveScore::exact` ＋ `exact_root_score()`；新判据要记住这条 |
| 11 | `ctest` 要 8.5 分钟 | 改 `src/` 必须跑，改 `tools/` 可以不跑 | 提交信息里写明跑没跑、为什么 |
| 12 | 构建主机的「本机」口径 | 是 6 vCPU 的 VMware 虚拟机，不是 12 线程物理机 | 报告里别写「本机 12 逻辑核」 |

---

## 6. 验收命令模板

```bash
# 0) 开工前（你已经在主机上，直接进目录）
cd ~/Desktop/workspace/gomoku && git pull && git log --oneline -1   # 期望 a9dfeb1 或其后代

# 1) 改完一个文件
cmake --build build -j6 --target <受影响目标>          # 期望 rc=0，零新增 warning

# 2) 跑该单元的专用验证（见 §4 每个单元的「验收」）

# 3) 固定门禁
ctest --test-dir build                                  # 期望 8/8（改 src/ 时）
node tests/readme_anchor_check.js                       # 期望 120 citations, 0 problems
node tests/web_frontend_check.js                        # 期望 105/105

# 4) 变异臂（新增用例必做）
#    改错一条断言 → 期望非零退出并报出 test_xxx.cpp:行号 → 还原 → 复跑全绿

# 5) 提交并推送
git add -A && git commit -F - <<'MSG'
feat: <一句话> — 轮次NNN

<规格章节 / 落点 / 读数 / 杀线 / 门禁>
MSG
git push origin master && git log --oneline -1 origin/master
```

---

## 7. 交接检查清单

开工前确认：

- [ ] `git log --oneline -1` 是 `a9dfeb1` 或其后代，`git status` 干净
- [ ] `cmake --build build -j6` 能过（不重新 configure）
- [ ] `ctest --test-dir build` 8/8（基线）
- [ ] `node tests/readme_anchor_check.js` 120/0、`node tests/web_frontend_check.js` 105/105
- [ ] 读过 `docs/ENGINE_REDESIGN_V2.md` 的 §2.2（O1–O4）、§3.2（S1–S4）、§3.3/§3.4（S5/S6）、§4（进程模型）、§6（复盘漏斗）
- [ ] 知道 **L 阶段不做**、GPU 不可用

每个单元交付时确认：

- [ ] 一个 commit，一条 `push`，`origin/master` hash 已核对
- [ ] 提交信息里有「规格章节 / 落点 / 读数 / 杀线 / 门禁」
- [ ] 新机制带杀线，读数标了「实测」或「未测」
- [ ] 新增用例有变异臂证据（报出行号）
- [ ] 新增用例都有超时：搜索臂用节点帽，I/O/进程臂用 `ctest TIMEOUT` 或脚本 `timeout`
- [ ] 新测试目标已加进 `CMakeLists.txt`、`foreach(... -UNDEBUG)` 与 `set_tests_properties(... TIMEOUT ...)`
- [ ] 若走杀线：写清读数与理由，回收代码，**继续下一个单元**

---

## 8. 方案选择：多路线时的取舍

这一章回答三个问题：**首选哪个方案、为什么只能选它、什么读数出现时才换另一个**。
凡是「只有一条路」的地方也写明理由——因为下一个人会想试第二条。

### 8.0 三条通则（先看这三条，再看逐单元的表）

**通则一：先算账，再动手。**
`1M nps` 就是这条的标本。目标要求 Ir/节点从 42,353.5 降到 ≤ 9,660（4.4 倍），而实测评估族已占节点成本 69.19 %——**把评估族整个做到 0，也只有 3.23 倍**（`1/(1−0.6919)`）。这是算术，不是工程态度。所以结论不是「再加一层优化」，而是**改口径**：G5 拆成 G5a（单线程 ≥ 45 万）＋ G5b（整机 ≥ 100 万）。
**凡是目标里出现「提升 N 倍」而现有占比算不出这个 N，先停下来算，别开工。**

**通则二：有实测就按实测改口径，不按愿望加机制。**
同类先例还有两个：`:124` 的 ponder 从「默认关」改成「默认 25 %」，依据是「省下 ÷ 花掉 = 1.273，总节点净 −4 %」而不是「45.8 % 降幅」；段 B 的复核强度，20k/6 产出 0 张卡、200k/8 产出 4 张，于是新增「复核者必须比被复核者强」这条约束。**口径是结论的一部分，不是包装。**

**通则三：机制建好后读数不达标，回收，不「再调调」。**
先例：威胁延伸、LMR、futility 三轮都是建完、量完、判不落地、代码回收；丙-1、丙-3、丙-4 同样，而**丁-4 是第一次「量完就知道不用建」**——判据的算术（每色 4 局的 95 % Wilson 只有 4-0 才点火）与动作的实测（四项旋钮拧到 −100 %/+150 % 推不动一格胜负）都不需要对局就能算清，先例记在轮次159。**不许留一个「暂时关着但代码还在」的半成品**——那会让下一个人以为它在跑。

### 8.1 逐单元决策表

| 单元 | 岔路口 | 首选 | 为什么只能这样 / 为什么它更好 | 什么情况下换 |
| --- | --- | --- | --- | --- |
| 甲-3 | 分析跑在哪 | 独立 `std::thread` | 同步跑会阻塞 io 线程（一条复盘要数秒到数十秒）；`SearchController` 是「一次思考」的模型，装不下「多手串行」 | 若将来要并发多个会话的复盘，才考虑引入任务队列 |
| 甲-3 | 结果怎么推 | 进度帧按「每 N 手」＋ 结果帧一次推完 | 60 手 × 每手 20 B ≈ 1.2 KB，一帧够；但分析本身耗时长，必须有进度 | 若单局分析 > 30 s，改成「只分析候选手」＋ 分段推送 |
| 甲-3 | 何时触发 | 手动按钮为主，自动为可选（默认关） | 终局瞬间自动跑会立刻吃 CPU，玩家往往想直接开下一局 | 若用户明确要「下完自动出报告」，把默认改成开 |
| 甲-3 | 分析时玩家在思考 | 直接拒绝（回 `error`） | 复盘会偷走玩家那手的时钟，且两者的 TT 会互相污染 | 不换。要并行就等戊-1 的进程隔离 |
| 甲-4 | 门禁放哪 | 独立脚本，**不进 `ctest`** | `ctest` 已 500 s，再塞一个 8 s 的臂，人会开始跳过 `ctest`，门禁就死了 | 不换 |
| 乙-1 | 棋盘集从哪来 | `tests/pinned_positions.h`（乙-2 建） | 一处定义，两处使用（差分表 ＋ 常驻臂） | 乙-2 未做时先内联 42 副，做完再合并 |
| 乙-1 | 怎么访问 private | `#define private public`（只在工具/测试 TU） | 仓库既有先例，不动生产接口 | 若该函数要被多个工具长期使用，才加公开只读接口 |
| 乙-2 | 放哪个 test 目标 | **新 `test_pinned`** | `test_search` 已 470 s，d8 臂会再拖数秒 | 不换 |
| 乙-2 | 期望值怎么来 | 硬编码读数（来自 `REVIEW_LOG`） | 透明、可 review、变了一眼看得出 | 臂数 > 50 时才考虑 golden 文件 |
| 丙-1 | 证明存哪 | **独立 `ProofTable`**，不写进 TT | TT 的替换是 depth 优先、且每批 `clear()`——证明会被淘汰；证明必须比一次思考活得久 | 不换。若坚持写 TT，等于让「已证明」和「搜得深」抢同一个槽 |
| 丙-1 | `UNKNOWN` 存不存 | **不存** | 「没证明」不是信息；存了就是把未证明当证明用 | 不换 |
| 丙-1 | 命中返回什么 | `±(100000 + dist)` | 认输判定（`search_engine.cpp:349`）与 TT 距离还原都依赖这个约定 | 不换 |
| 丙-2 | 缓存挂哪 | `SearchState`（跟一次思考生死） | `Board` 要可复制、要能跨思考复用，挂上去会带来失效地狱 | 不换 |
| 丙-2 | 键用什么 | 四 `uint64` 线字 | 「±2 邻域」会漏：实测 (7,7) 会因 (7,12) 落子而变（轮次122） | 不换。键成本 > 收益时是**作废**，不是换键 |
| 丙-3 | 威胁等级从哪来 | 复用 `MoveGenerator` 已有的一次 `recognize` | 那里已经为分 Tier 扫过一遍，再算就是第三次 | 不换。轮次154 补：布尔量**给得出**（等级 = 行棋方四方向 `PatternType` 最大档），分辨率**给不够**（每份被截的 tier 只有 3.6-4.0 档，宽度却要留 8-16 个），所以不存在「再写细一点」的第二方案 |
| 丙-3 | 着法变了怎么办 | **不换方法，直接作废** | 这一项改的是「哪些格子被保留」，着法变了说明 Tier 判定被改了，不是优化 | 不换 |
| 丙-4 | 键宽 | 3^13（实现时用 3^12 压缩索引） | 中心格恒黑（`pattern.cpp:350`），只有 3^12 个不同键，省 6 倍内存 | 不换 |
| 丙-4 | 建表方式 | **复用现有 `finish_direction` 的纯函数副本** | 重写会有第二份语义，两份迟早漂移 | 不换 |
| 丙-4 | LUT 做不成怎么办 | 退到**「只对白棋查表、黑棋继续走线」** | 黑棋特判（长连、活四是否真活、活三是否真活）正是走线的主因，白棋分支是纯函数 | 差分失败时走这条 fallback，仍然要 75,600 组验证 |
| 丁-1 | 连续分量从哪取 | 从 `GameRecord` 现算 | 不加新基础设施，样本/记录已经带着需要的一切 | 若某分量在记录里取不到，才考虑训练时顺带记 |
| 丁-2 | `teacher_move` 落在哪 | **`MoveRecord`**，不是样本层 | `collect()` 只见 `GameRecord`；落在样本层会让样本无法从记录复现 | 不换 |
| 丁-2 | 教师用哪张 TT | **独立 TT** | worker 与 `shared_tt_` 共用一张表，d6 条目会让 d4 的 `search_depth` 失真 | 不换 |
| 丁-2 | 教师按深度还是节点 | 按**节点**（成本可预测） | 与 `--max-nodes` 同一条纪律：机器忙闲不该改变样本 | 需要「深度差」分量时才额外记深度 |
| 丁-3 | 系数怎么拆 | `Evaluator` 持 `CoefficientPair` | 两个 `Evaluator` 会让 TT/缓存分裂；每次传 color 会让改动面爆炸 | 不换 |
| 丁-3 | cap 用谁 | `min(five_b, five_w) − 1` | 帽若随颜色变，反对称在帽处就破了 | 不换 |
| 丁-3 | 威胁阶梯能不能两侧不同 | **不能（±20 % 内）** | 否则「白方」就是另一个引擎，Elo 失去意义 | 不换。允许不同的是位置/贴边/宽度/防守权重 |
| 丁-3 | 等价臂不逐字节相同怎么办 | **回滚重做** | 说明某处破坏了不变量，不是「差一点」 | 不换 |
| 丁-4 | 判据用原始胜率还是区间 | **Wilson 区间** | 8 局的噪声不该触发动作 | 局数 ≥ 200 时才可以直接比点估计 |
| 丁-4 | 一次改几项 | **一项，一步** | 多项同时改无法归因 | 不换 |
| 丁-5 | 规则实现在哪 | 引擎内，且 `game_manager` 与训练器**读同一份定义** | 两处各写一份，网页上换了规则训练器不认 | 不换 |
| 丁-6 | 并行度 | **顺序循环** | L 阶段取消（无 GPU、无 `WavefrontBatch`），顺序就是全部并行度 | 若将来恢复 L，才换成跨根批处理 |
| 戊-1 | 进程还是线程 | **进程** | 崩溃隔离（assert 在 memcheck 下 abort 过两次）、`--threads` 语义已糊涂、玩家与训练不能共核、TT 本来就每引擎一张 | 平台不支持时退回线程，并**显式上报** |
| 戊-1 | IPC 用什么 | 文件 spool ＋ UDS | 端口池被打满过（硬重启）、TIME_WAIT 峰值 5011 | 不换。共享内存只在 spool 成为瓶颈时才考虑 |
| 戊-1 | 绑核怎么绑 | 进程内 `sched_setaffinity` | 外部 `taskset` 管不到动态起的 worker | 不换 |
| 戊-2 | schema 存哪 | 代码里的 `ParamSpec` 表 | 编译期可检查、与训练器共用同一份 | 参数需要热加载时才改 JSON 文件 |
| 戊-2 | 预览跑什么局面 | **固定针臂** | 必须可复现、可比；用玩家当前局面则两次读数不可比 | 不换 |
| 戊-3 | 状态机放哪 | **broker** | 放前端：关浏览器就停训练；放 trainer 进程内：worker 崩了状态机跟着死 | 戊-1 未做时暂放 trainer 进程内，但必须在代码注释里标「临时」 |

### 8.2 已被实测否掉的路线（不要再试）

| 路线 | 为什么否掉 | 出处 |
| --- | --- | --- |
| 单线程 1M nps | 评估族清零也只值 3.23 倍，节点侧全开只值 1.34–2.03 倍且要牺牲 17 个必胜 think | 轮次135–138；`docs/ENGINE_REDESIGN_V2.md` §1.2–§1.3 |
| GPU 进单次思考 | 一次节点 4.4 µs，而 kernel 往返 5–20 µs；每节点只有 44.94 次候选评估，撑不满占用率 | 轮次142；§2.1 |
| 单根 VCF 波前扩展 | 每层宽 1–3、层间严格依赖、丢掉早退；每层工作量 ≈ 6.78 µs，小于一次往返 | 轮次143；§2.4.1 |
| 整条静默尾巴关掉 | 只省 15.4 %，32 景上 27/64 臂换答案，17 个 think 掉出 99,000 | 轮次138 |
| 跨节点格子分表记忆化 | 重复率 28.6 %，上限 1.25 倍；无限表要 1.04 GB | 轮次136 |
| 全盘增量评估 | make/undo delta 的成本约等于重扫 | README 已记录那次否决 |
| 威胁延伸 / LMR / futility | 建完量完判不落地，代码已回收 | 轮次121 B4、131、134 |
| 按色胜率平衡控制器（丁-4） | 判据每轮都点火（每色 4 局只有 4-0 才不含 0.5，误点火 12.5 %），而它动的四项拧到 −100 %/+150 % 推不动一格胜负；能动的是被双重禁止的威胁阶梯 | 轮次159 |
| 用「已占子键缓存」打 `pattern_score` | 前提错了：`pattern_score` 对空候选格调，已占子不进它 | 本文件 §4 丙-2 |

> 这张表的用法：**任何人提出其中一条，先让他读对应出处**。不是不允许重新讨论，而是重新讨论必须带来新读数。

---

## 9. 测试与超时（每个单元自带回归用例）

要求两条：**每个单元自己写回归用例**（不是验证一次就算），**每个用例都有超时**（挂住的测试比失败的测试更糟，它会把整条门禁拖死）。

### 9.1 三层超时（当前只缺第一层，先补上）

| 层 | 现在有没有 | 该怎么做 |
| --- | --- | --- |
| `ctest` 层（进程级） | **一个都没有**——`CMakeLists.txt` 里没有任何 `set_tests_properties(... TIMEOUT ...)`，死循环会让 `ctest` 永远挂着 | 见下面的片段，**第一个单元顺手补上** |
| 测试进程内（搜索臂） | 部分有（`ScriptedClock` 是给「哪一层被截止」用的，不是兜底） | 搜索臂一律 `set_max_nodes(N)` ＋ 不 binding 的时限；**节点帽就是超时** |
| 脚本层 | `tools/tactics_bench.sh` 没有 | 每个 budget 包一层 `timeout 300` |

**`CMakeLists.txt` 末尾补这一段**（放在 `enable_testing()` 之后、`foreach` 之前）：

```cmake
# 每个测试都要有超时。没有 TIMEOUT 的 ctest 遇到死循环会永远挂着，
# 而挂住的门禁比红掉的门禁更糟：人看不到红，只会看到「还在跑」。
# 值取实测的 2 倍左右：既不会被一台忙机器误杀，也不让真 hang 拖十分钟。
set_tests_properties(test_board     PROPERTIES TIMEOUT 120)
set_tests_properties(test_pattern   PROPERTIES TIMEOUT 120)
set_tests_properties(test_evaluator PROPERTIES TIMEOUT 120)
set_tests_properties(test_search    PROPERTIES TIMEOUT 900)   # 实测 470 s
set_tests_properties(test_position  PROPERTIES TIMEOUT 120)
set_tests_properties(test_session   PROPERTIES TIMEOUT 60)
set_tests_properties(test_server    PROPERTIES TIMEOUT 180)
set_tests_properties(test_training  PROPERTIES TIMEOUT 300)   # 实测 13 s，memcheck 下 166 s
```

再兜一层：跑门禁时用 `ctest --timeout 900 --test-dir build`。

### 9.2 超时值怎么定

| 情形 | 用什么当超时 | 为什么 |
| --- | --- | --- |
| 搜索/评估类断言 | **节点帽**（`set_max_nodes`） | 墙钟会让读数随机器忙闲变化，同一断言今天绿明天红 |
| 定深臂（d8 等） | 节点帽 ＋ 一个**不 binding** 的时限（如 60000 ms） | 只防死循环，不参与判定 |
| I/O / 进程 / IPC 臂 | 墙钟（`std::chrono` 自检 或 `ctest` TIMEOUT） | 这类没有「节点」可数 |
| 脚本门禁 | `timeout N cmd` | 脚本里任何一步都可能挂 |

**超时了怎么办**：先分清「机器慢」还是「死循环」。
- 机器慢 → 提高 `TIMEOUT`，但**不许提高判定用的时限**（那会改变读数）；
- 死循环 → **加节点帽或加迭代上限**，不许只调大时间。把死循环包在一层更长的超时里，等于把 bug 藏起来。

### 9.3 回归用例的写法（六条）

1. **每个单元至少 3 条**：新增行为 1 条 ＋ 边界 1 条 ＋ **旧行为不变 1 条**（后者最容易被漏，也最重要）。
2. **命名**：`test_<单元>_<钉的是什么>`，函数上方用中文注释写明它钉的是哪条规格（章节号或 `REVIEW_LOG` 轮次）。
3. **钉值，不钉存在**：`assert(review.loss == 55268)` 而不是 `assert(review.loss > 0)`。本仓库的用例风格是硬数值，变了就要解释。
4. **不依赖执行顺序与全局状态**：元素桩、全局单例用完要显式清空（`web_frontend_check` 的用例之间共享 `el()` 单例）。
5. **失败信息要能定位**：`assert(cond)` 之外，复杂断言先算出 `got` 再比较，报错时能看出差多少。
6. **必须有一条变异臂**：改错一条断言 → 期望非零退出并报出 `文件:行号` → 还原 → 复跑全绿。**没有变异臂证据的新用例，不算交付。**

### 9.4 逐单元回归用例清单

| 单元 | 落在哪 | 至少钉什么 | 超时 |
| --- | --- | --- | --- |
| 甲-3 复盘面板 | `test_server` ＋ `web_frontend_check.js` | 帧序列（≥1 progress、恰好 1 result）、思考中拒绝、面板默认隐藏、标记数、点击跳手 | 180 s / 脚本 |
| 甲-4 门禁接线 | `tools/tactics_bench.sh` ＋ `readme_anchor_check` | 三档输出、anchor 0 problems | 脚本 `timeout 300` |
| 乙-1 差分表 | 新 `tools/position_term_table` | 18,900 行、`match` 全 1；变异臂改「外三行」→ 出现 0 | 工具自检 ＋ 脚本 |
| 乙-2 选点基准 | 新 `test_pinned` | 每副的 `move/score/nodes`；变异臂改 ±1 → 红 | 900 s（d8 慢） |
| 丙-1 `ProofTable` | `test_search` | `UNKNOWN` 不入库、命中返回 `±(100000+dist)`、275 根等价 | 900 s |
| 丙-2 `BoardValueCache` | `test_evaluator` | 18,900 组逐格等价、cap 不被缓存 | 120 s |
| 丙-3 排序/威胁分离 | `test_search` | 针臂着法/分值/深度全同 | 900 s |
| 丙-4 `finish_direction` LUT | `test_pattern` | 75,600 组逐组相同（1 组不一致即红） | 300 s（差分跑得久） |
| 丁-1 S3 连续信号 | `test_training` | `w=0` 与今天逐字节相同、档数 > 5、同 seed 一致 | 300 s |
| 丁-2 S4 蒸馏 | `test_training` | `teacher_*` 往返、旧记录读回为空、教师用独立 TT（`search_depth` 不被污染） | 300 s |
| 丁-3 S6 按色系数 | `test_evaluator` | 等价臂（两侧同值 → 整树逐字节）、`eval(W) == -eval(B)`、旧文件读成同值 | 120 s |
| 丁-4 平衡控制器 | `test_training` | Wilson 判据、一步一项、5 步封顶、回滚条件 | 300 s |
| 丁-5 S1 交换规则 | `test_training` ＋ `test_server` | `none` 与今天逐字节、`swap` 的交换回合、非法规则被拒 | 300 s / 180 s |
| 丁-6 S2 开局筛选 | `test_training` | 判据边界、落盘复用（同 `coeffs_sha` 不重筛） | 300 s |
| 戊-1 H 进程模型 | 新 `test_broker`（或 shell 臂） | 半行写丢弃并重排、`SIGKILL` 后账对齐、绑核生效 | 脚本 `timeout 300` |
| 戊-2 I 控制面 | `test_server` ＋ `web_frontend_check.js` | schema 形状、越界拒绝、三类 `when_live`、前端零硬编码 | 180 s |
| 戊-3 K 一键训练 | `test_training` | 同 seed 两次相序列与 checkpoint 逐字节相同 | 300 s |

> 新增测试目标（`test_pinned`、`test_broker`）记得做三件事：加进 `CMakeLists.txt`、加进末尾那个 `foreach(... -UNDEBUG)` 列表、**加一条 `set_tests_properties(... TIMEOUT ...)`**。

---

## 附：单元与设计文档的对应

| 单元 | 设计文档章节 | v1.0 出处 |
| --- | --- | --- |
| 甲-3 复盘面板 | §6.9、§6.10 | — |
| 甲-4 门禁接线 | §6.7 c | — |
| 乙-1 位置项差分表 | §7 回归保障 3 | `:556` |
| 乙-2 选点基准 | §7 回归保障 1 | `:554` |
| 丙-1 O5 证明入库 | §2.3 | — |
| 丙-2 O2 `BoardValueCache` | §2.2 O2（**前提已更正**） | — |
| 丙-3 O3 排序/威胁分离 | §2.2 O3 | — |
| 丙-4 O1 `finish_direction` LUT | §2.2 O1 | — |
| 丁-1 S3 连续信号 | §3.2 S3 | `:435` |
| 丁-2 S4 蒸馏 | §3.2 S4 | — |
| 丁-3 S6 按色系数 | §3.4 | — |
| 丁-4 平衡控制器 | §3.4.3 | — |
| 丁-5 S1 交换规则 | §3.2 S1 | — |
| 丁-6 S2 开局筛选 | §3.2 S2 | `:436` |
| 戊-1 H 进程模型 | §4.1–§4.4 | `:433` |
| 戊-2 I 控制面 | §4.5–§4.6、§5 | — |
| 戊-3 K 一键训练 | §4.8 | — |

> `docs/ENGINE_REDESIGN.md`（v1.0）**不要改**：`:434` / `:592` 被 `tests/test_training.cpp` 按绝对行号引用。

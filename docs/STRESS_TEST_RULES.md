````text
【Valgrind / 竞争检测使用约束 · 必须遵守】

前提 Valgrind 为最新版（>= 3.24），GCC 支持 -fsanitize=thread。

━━━ 一、铁律：Valgrind 与压测互斥 ━━━

跑 Valgrind 时不许开压测；压测时不许把被测进程放进 Valgrind。
这不是"会更慢"的问题，是会改变被测对象的性质、并且显著提高把主机拖进假死的风险。

━━━ 二、为什么互斥（四个量级）━━━

1. CPU：memcheck 下程序慢 20–50 倍 → 吞吐量塌陷，测出的 QPS 是 Valgrind 的 QPS，不是服务的。
2. 内存：放大 2–5 倍（每块分配都记元数据）→  只有 16GB，很容易吃满 → vmmem 膨胀 → 主机可用内存下降。
3. 延迟：P99 放大一个数量级以上 → 客户端容易判定超时。
4. accept 速率：骤降 → 服务端来不及 accept，SYN 队列堆积。

最危险的一条：服务端在 Valgrind 下慢 20–50 倍后，客户端建连会大量超时。
如果客户端此时"失败就重连"，就形成 超时 → 重连 → 更超时 的正反馈。
这正是 2026-10-16 整机假死的机制。在 Valgrind 下压测，比正常运行更容易触发端口耗尽。

━━━ 三、工具分工（不要拿 DRD 当第一道筛子）━━━

- 内存错误 / 泄漏 / 越界  →  valgrind memcheck（保留为提交门禁）
- 数据竞争                →  TSan（-fsanitize=thread），比 DRD 快 10–20 倍且更准
- 死锁                    →  TSan（TSAN_OPTIONS 里开 second_deadlock_stack=1）
- 指令数（性能对比）      →  callgrind
- 堆峰值                  →  massif

━━━ 四、为什么不用 DRD 筛数据竞争 ━━━

版本新 ≠ DRD 结果可信。以下两类是工具模型的结构性限制，不是版本 bug：

- DRD / Helgrind 对 asio strand 必误报。strand 是"逻辑串行化"而非锁，
  工具只能看到多个线程碰同一块内存，看不到 strand 保证的互斥。
  凡是 handler 里访问 GameManager 状态的测试，DRD 都会报"并发访问"。
- DRD 对无锁数据结构必误报。共享置换表若用 atomic load/store + relaxed ordering，
  DRD 的 happens-before 模型无法证明其正确性，会给出大量假阳性。

对这两类场景，DRD 的假阳性率高到没有筛选价值。
现有 tools/valgrind_check.sh 是"DRD 先筛 → TSan 确认"，请把顺序反过来：
TSan 先跑（快、准），DRD 只在需要交叉验证时用。

━━━ 五、TSan 落地（不需要修改 CMakeLists）━━━

第一步，内核 6.x 必做，否则 TSan 直接失败：

    sudo sysctl vm.mmap_rnd_bits=28
    # 不设会报：FATAL: ThreadSanitizer: unexpected memory mapping 0x...
    # 这条重启失效；确认生效用 cat /proc/sys/vm/mmap_rnd_bits

第二步，独立构建目录，不要污染 build/ 和 build-vg/：

    cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
          -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -O1"
    cmake --build build-tsan -j"$(nproc)"

第三步，跑的时候让一次收集全部竞争，而不是第一个就退出：

    TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1" ./build-tsan/test_training
    TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1" ./build-tsan/test_server

━━━ 六、三条硬约束 ━━━

1. TSan 与 Valgrind 不能同时用——两者都改写内存布局，叠加会崩溃。
2. TSan 构建下禁止压测——-O1 加插桩同样慢 5–15 倍，触发的是同一套"变慢 → 超时 → 重连"正反馈。
3. vm.mmap_rnd_bits=28 是临时设置，重启失效，不要写进脚本假设它一直有效。

━━━ 七、本项目优先用 TSan 覆盖的三处 ━━━

1. 共享只读置换表——无锁并发读写，DRD 会大面积误报，只有 TSan 能给出可用结论。
2. GameManager 的 ai_thinking_worker——与 io_context 线程共享棋局状态。
3. 训练参数 / 检查点的原子替换——文件写入与读取竞争。

注：valgrind_check.sh 用法示例里提到 test_shared_tt_concurrent_probe_store，
但该测试当前不存在，transposition.h 里也还没有 atomic / mutex 成员。
共享只读 TT 属重设计未落地部分，实现后必须补这个用例。

━━━ 八、正确时序 ━━━

1. 功能验证：Valgrind memcheck / TSan 跑测试（不采基线、不压测）
2. 基线采集：关掉 Valgrind 和 TSan，采 2 分钟空载基线
3. 容量压测：关掉 Valgrind 和 TSan，开 watch-resources 采样，分档压测
4. 事故复现：绝对不要同时开 Valgrind / TSan

━━━ 九、汇报要求 ━━━

- memcheck：报 definite leak 数量、invalid read/write 数量、是否新增（对比基线）
- TSan：报 data race 数量、涉及的函数对、是否与 strand/atomic 相关
- 两者都必须说明跑在哪个构建目录、是否开了 -UNDEBUG
- 若判定某条报告是误报，必须给出依据（是 strand、是 atomic、还是已知的第三方库），不能只说"看着像误报"
````

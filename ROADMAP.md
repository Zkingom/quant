# ROADMAP：实施顺序与验收门槛

当前日期基准：2026-10-19。核心功能冻结：**2026-11-24**。正式交付：**2026-12-14**。
优先级：正确性 > 可验证性 > 可维护性 > 性能优化 > 扩展功能。

## 1. 模块实施顺序（按依赖）

| 顺序 | 模块 | 计划窗口 | 前置 | 交付物 |
|---|---|---|---|---|
| 0 | 工程骨架 | 10-19（✅） | — | CMake/Presets、警告、Sanitizer、GTest 集成、`scripts/check.sh` |
| 1 | M1 core | 10-19 → 10-21（✅ 10-19 完成） | 0 | 类型、命令、拒单码、配置约束、校验、溢出检查 |
| 2 | M2 book | 10-22 → 10-28（✅ 提前于 10-19 → 10-20 完成） | M1 | 价格层 + FIFO + ID 索引；`find_invariant_violation()`；结构单测 |
| 3 | M3 engine | 10-20（✅ 提前完成） | M2 | 撮合、撤单、事件、序号；语义单测；**引擎参考模型、事件审计器、差分测试、故障注入**（D-023） |
| 4 | M4 testkit | 10-20（✅ 提前完成） | M3 | 把生成器 / 参考引擎 / 审计器整理为可复用的 `lob::testkit`（供 M5、M6 使用）；更大规模长跑（更多种子、更长序列）（D-023 调整） |
| 5 | M5 replay / snapshot / recovery | 10-20 → 10-21（✅ 提前完成） | M3、M4 | 文本格式 v1、日志与提交会话、快照与恢复、`lob_replay`、Golden、崩溃测试 |
| 6 | M6 bench 基线 | 11-19 → 11-23（✅ 提前于 10-21 → 10-22 完成并验收） | M4 | 固定负载基准、吞吐 + p50/p95/p99、方法学文档；profiling 与一项有假设的优化实验（E1，D-042） |
| — | **L1 冻结评审** | **11-24**（✅ 10-24 提前评审并通过，见 [MODULE_NOTES/L1_REVIEW.md](MODULE_NOTES/L1_REVIEW.md)） | 全部 | 见 §2.1 |
| 7 | L2 性能工程 | 11-25 → 12-07 | L1 通过 | perf 剖析、≥1 项有假设的优化实验、前后对比（大部分已由 M6 交付，现状见 §2.2） |
| 8 | L3 网关（有条件） | 12-08 → 12-10 | L2 通过 | 单客户端 TCP 网关 |
| 9 | 交付收尾 | 12-11 → 12-14 | — | 文档终审、AI_USAGE 定稿、复现脚本 |

M4 排在 M5 之前：差分测试是比 Golden Test 更强的正确性证据，应尽早对 M3 生效；
Golden Test 依赖文本格式，格式稳定后再固化。

**进度受阻时的削减顺序**（先砍前面的）：L3 网关 → L2 第二项实验 → M5 中的 CLI 便利功能 →
（不可削减）差分测试、Sanitizer 构建、Golden Tests、不变量检查。

## 2. 验收门槛

所有门槛以 `scripts/check.sh` 的实际输出为证据，结果记录在对应 `MODULE_NOTES/` 中；
例外是差分测试门槛（§2.1），其规模超出 CTest 的合理运行时间，证据来源为 `scripts/soak.sh`（由 `scripts/evidence_m6.sh` 运行，
输出在 `MODULE_NOTES/evidence/M6/regression/soak/`）。
任何未能在当前环境执行的检查都必须列为"未验证项"并附复验命令。

### 2.1 Level 1（核心系统）—— 必须全部满足

> 2026-10-24 评审结论：全部满足、无阻断项（[MODULE_NOTES/L1_REVIEW.md](MODULE_NOTES/L1_REVIEW.md)）；同日作者确认 **L1 通过**。

**构建**
- [x] `gcc-debug`、`gcc-release`、`gcc-asan`、`clang-debug`、`clang-release`、`clang-asan` 六个预设全部零警告（`-Werror`）构建。
- [x] 六个预设下 CTest 全部通过；Sanitizer 构建使用 `-fno-sanitize-recover=all`，任何 UB 即失败。

**语义覆盖**（每项至少一个具名测试）
- [x] 价格优先：更优价格先成交。
- [x] 同价 FIFO：同价位按到达顺序成交。
- [x] 部分成交（taker 部分、maker 部分）与完全成交。
- [x] 跨多个价格层的一次性扫单，成交价为 maker 价格。
- [x] 撤单：撤活跃单、撤部分成交单、撤未知 ID、撤已成交 ID、重复撤单。
- [x] 重复 ID（与活跃订单冲突）、无效订单（每种拒单原因）。
- [x] 边界值：价格带上下界 ±1、数量 0/1/max/max+1、`INT64_MIN/MAX`、`UINT64_MAX` ID。
- [x] 容量上限（`max_live_orders`）拒单。

**不变量与守恒**
- [x] 每条命令处理后 `find_invariant_violation()` 无违例（测试与差分驱动中逐步检查）。
- [x] 数量守恒：Σ 接受数量 = 2·Σ 成交数量 + Σ 剩余挂单 + Σ 撤单剩余。
- [x] 订单簿永不交叉：处理后 best_bid < best_ask（两侧非空时）。

**确定性**
- [x] 同一输入回放两次，事件流与快照哈希一致。
- [x] GCC 与 Clang 构建对同一输入产生相同快照哈希。
- [x] Golden Tests：≥ 5 个手写场景 + ≥ 1 个固定种子大场景（规模：≥ 2 000 条命令，2026-10-24 由作者确定），输出与提交的期望文件逐字节一致。

**差分测试**
- [x] 固定种子（≥ 20 个种子 × ≥ 10⁴ 命令）下，引擎与朴素参考模型的事件流与快照完全一致。
  证据来源：`scripts/soak.sh`（经 `scripts/evidence_m6.sh`），不是 `check.sh`；CTest 中的差分（20 × 5 000，逐条全量检查）作为每次构建的快速回归。

**基准**
- [x] `lob_bench` 可运行，输出吞吐与 p50/p95/p99，负载与环境写入 `BENCHMARK.md`。

**文档**
- [x] README / ARCHITECTURE / DESIGN / DECISIONS / BENCHMARK / MODULE_NOTES / AI_USAGE 与代码一致。

### 2.2 Level 2（性能工程）—— L1 通过后

> 现状（2026-10-24）：前三项已由 M6 满足（BENCHMARK §7–§10、MODULE_NOTES/M6）；前后对比与等价性由 L2 补齐；其余见各项括注。

- [x] 固定种子、固定负载、固定参数的基线结果（≥ 5 次重复，报告中位数与离散度）。（M6：BENCHMARK §7、§10）
- [x] 至少一份剖析证据（`perf record/report` 或替代工具，见风险 R1）定位热点。（M6 §5：perf cpu-clock + callgrind；无 PMU）
- [x] 至少一项优化实验：写明假设 → 改动 → 测量 → 结论（含"假设不成立"的可能）。（M6：E1 采纳；P1 未检出收益）
- [x] 前后对比：吞吐、延迟分位、内存占用、代码复杂度、工程代价。（BENCHMARK §12：吞吐与延迟来自 §9 的同会话 A/B；峰值 RSS 实测，每个构建 5 次中位数；代码与工程代价按提交统计）
- [ ] 基线实现保留（编译期或运行期可选），实验结果可用一条命令复现。（部分：编译期选项 `LOB_BASELINE` 已保留基线实现，D-045；等价性可一条命令复现（`scripts/l2_equivalence.sh`），性能对比尚未改为基于两种构建的一条命令）
- [x] 优化版本通过全部 L1 测试，且与基线的事件流/快照哈希一致。（L1 测试：两种构建各 197/197；哈希：`scripts/l2_equivalence.sh`，16 个输入的事件流、日志、state_hash、快照文件全部一致，见 [MODULE_NOTES/L2_EQUIVALENCE.md](MODULE_NOTES/L2_EQUIVALENCE.md)）

### 2.3 Level 3（系统扩展）—— 仅 L1、L2 均通过且时间允许

- [ ] 单客户端 TCP 网关，网络层与撮合核心通过 `Command` 与事件输出接口解耦（引擎实际接口为 `process(Command, std::vector<Event>&)`，D-022；`EventSink` 已被否决）。
- [ ] 网关端到端测试：经 TCP 输入的命令序列与文件回放产生相同快照哈希。
- [ ] 仅当有测量证据表明阻塞 I/O 成为瓶颈时才引入 epoll。

## 3. 已知风险

| ID | 风险 | 影响 | 缓解 |
|---|---|---|---|
| R1 | WSL2 内核无对应 `linux-tools`，`perf` 可能不可用或缺少硬件计数器 | L2 剖析证据 | 尝试 `/usr/lib/linux-tools/*/perf` 或源码编译 perf；退路为 `valgrind --tool=callgrind` + 采样计时；如实记录 |
| R2 | WSL2 虚拟化、CPU 频率与调度噪声影响延迟分位 | 基准可信度 | 多次重复、报告离散度、固定负载与核亲和（`taskset`），在 BENCHMARK.md 中声明限制 |
| R3 | 源码位于 `/mnt/g`（9P 文件系统），构建 I/O 较慢 | 开发效率 | 可接受；必要时把构建目录放到 WSL 本地文件系统 |

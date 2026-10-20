# M4 testkit —— 技术说明

状态：✅ 2026-10-20 验收通过（证据见 §7、§9 与 [evidence/M4/](evidence/M4/)）。生产代码（`include/`、`src/`、`examples/`）在 M4 中零改动（`git diff main -- include src examples` 为空）。

## 1. 职责与边界

**负责**：把 M2/M3 中分散的测试工具整理为职责清晰、可复用、不依赖 GoogleTest 的测试基础设施；
设计覆盖不同簿深度、价格分布、撤单比例、成交密度、容量边界、ID 生命周期与数值上界的工作负载画像；
执行更大规模的确定性长跑与长序列故障注入，并记录时间、资源、种子与复现方法；对 M1–M3 完整回归；
审查异常语义、事件顺序、生命周期与资源清理中尚未覆盖的风险；为 M5 建立可执行的契约。

**不负责**：任何生产功能；性能测量（M6）；文本格式与回放工具（M5）。

## 2. 文件组织

```
testkit/include/lob/testkit/
  rng.hpp                确定性随机数                       (自 tests/support 迁移)
  reference_engine.hpp   朴素参考引擎 + best_price/drop_resting (自 tests/engine 迁移并扩展)
  event_auditor.hpp      A1–A6 审计器，返回 optional<string>，128 位累计 (自 tests/engine 迁移并改造)
  workload.hpp           WorkloadProfile、WorkloadGenerator、10 个画像   (新：自 M3 Driver 抽出)
  differential.hpp       run_differential、Coverage、unmet_targets、FaultInjector (新：自 M3 Driver 抽出)
  describe.hpp           事件/命令/簿的调试文本                (自 tests/engine 抽出)
  alloc_fault.hpp        分配故障注入接口 + Injector 适配器     (自 tests/support 迁移)
testkit/src/             describe, event_auditor, workload, differential, alloc_fault(OBJECT 库)
testkit/tools/soak.cpp   lob_soak 长跑 CLI
tests/engine/test_replay_contract.cpp   M5 契约 C1–C6（新）
scripts/soak.sh, scripts/evidence_m4.sh
```

## 3. 重构与"未削弱"的证明

### 3.1 迁移映射

| 原位置 | 新位置 | 变化 |
|---|---|---|
| `tests/support/rng.hpp` | `testkit/.../rng.hpp` | 命名空间 `lob::testkit` |
| `tests/support/alloc_fault.{hpp,cpp}` | `testkit/.../alloc_fault.hpp` + OBJECT 库 | 增加 `Injector` 适配器 |
| `tests/engine/reference_engine.hpp` | `testkit/.../reference_engine.hpp` | 增加 `best_price`（O(n)，替代排序）与 `drop_resting` |
| `tests/engine/event_auditor.hpp` | `testkit/.../event_auditor.{hpp,cpp}` | 返回 `optional<string>`；累计量 128 位；检查与消息文本不变 |
| M3 `Driver`（`test_engine_differential.cpp` 内） | `workload.cpp` + `differential.cpp` | 常数变为画像参数；增加全量检查间隔、状态指标、故障注入 |
| `engine_test_util.hpp` 中的 `describe/dump` | `describe.{hpp,cpp}` | `PrintTo` 留在 tests（GoogleTest 胶水） |

文件用 `git mv` 迁移，历史可追溯。M2 订单簿参考模型留在 `tests/book/`（只有一个消费者，D-026）。

### 3.2 证据

| 证明对象 | 方法 | 结果 |
|---|---|---|
| M3 差分命令流未变 | `mixed` 画像 20 种子 × 5,000 命令，22 个计数与 M3 验收日志逐一比较 | 全部相等（`MixedProfileReproducesTheM3Stream`） |
| M2 差分未变 | 订单簿差分的覆盖计数与 M2 验收日志比较 | 逐字相同（28056 / 11865 / 3920 / …） |
| 新增生成器参数在默认值下中性 | 每次修改生成器后重跑上一项 | 始终相等 |
| 检出能力未降 | M2、M3 变异表全部重跑，逐条与 M3 验收时的结果比较 | 见 §9.4 |
| 测试未被删除 | 用例数 130 → 147：+1 差分（拆为 2 个）、+1 长序列故障、+5 契约、+10 画像冒烟 | 无删除 |

## 4. 接口与调用关系

```cpp
const std::vector<WorkloadProfile>& standard_profiles();
RunReport run_differential(const WorkloadProfile&, const RunOptions&);   // {seed, commands, full_check_every, fault_per_mille, injector}
std::vector<std::string> unmet_targets(const WorkloadProfile&, const Coverage&);
EventAuditor::audit(command, events) / matches(book) -> std::optional<std::string>
```

`run_differential` 的一条命令（[differential.cpp](../testkit/src/differential.cpp)，`Runner::one_command`）：

```
cmd = generator.next(step, ref)                  // 读取参考模型的最优价、在簿订单
[若武装故障] k = U[0,5]；50% 先清空 events 容量；injector.arm(k)
engine.process(cmd, events)                      // 捕获 bad_alloc
[若武装] injected = injector.injected(); disarm
   注入了但没抛 → 失败（吞掉了失败）；抛了但没注入 → 失败
抛出 → handle_fault: PREPARE（events 空且 last_seq 未变）/ RESIDUAL（与参考变换后的事件比较，drop_resting，审计）→ 全量检查
否则 → expected = ref.process(cmd); events == expected; auditor.audit(cmd, events); record()
每 K 条 → full_check(): 两侧簿逐单比较、auditor.matches、引擎不变量、全深度采样
```

**具体案例**（契约 C2，`test_replay_contract.cpp`）：簿中卖 #1 50×5（seq 1、2）。`BUY #10 50×8`，事件缓冲区已预留容量，故准备阶段无分配；
第 0 次分配即 `OrderBook::add`（挂剩余 3）→ 注入失败。观察到：`#3 ACCEPTED`、`#4 TRADE maker=1 5@50 taker_rem=3`、
`#5 CANCELLED id=10 3 INSUFFICIENT_RESOURCES`，`last_seq` 2→5，簿为空，`bad_alloc` 传出。随后不注入地重试同一命令：
`#6 ACCEPTED`、`#7 RESTED id=10 8@50`——客户实际得到 5 成交 + 8 挂单，而不是 5 + 3。

## 5. 长跑设计

画像与目标见 DESIGN §4.4，检查节奏见 DESIGN §4.2，长序列故障注入见 DESIGN §4.3。长跑矩阵（`scripts/soak.sh`）：

| 模式 | 构建 | 每画像 | 目的 |
|---|---|---|---|
| long | gcc-release | 种子 1–10 × 200,000 命令，每 1000 条全量检查，`--require-targets` | 规模与边界 |
| faults | gcc-release | 种子 11–15 × 50,000 命令，2% 命令武装故障，`--require-targets` | 反复失败后继续运行 |
| xcheck | gcc-release 与 clang-release | 种子 21–23 × 50,000 命令 | 跨编译器确定性：覆盖/状态/故障行逐字比较 |
| asan | gcc-asan（ASan+UBSan+LSan） | 种子 31–32 × 20,000 命令，2% 故障 | 内存错误、未定义行为、泄漏（含失败路径） |

**复现方法**：任何失败都打印 `[repro] lob_soak --profile P --seed S --commands N --check-every 1 --fault-per-mille F`。
命令流只由 (画像, 种子) 决定，故障由派生自种子的独立随机流决定，`--check-every` 不影响两者（有测试），所以复现是精确的。

## 6. 画像校准记录

初版画像在不检查目标时全部"通过"，但其中 4 个没有到达声称的边界。处理原则：修改画像，不放宽目标。

| 画像 | 初版实测 | 原因 | 处理 |
|---|---|---|---|
| deep_book | 单侧 804 档（目标 1000）；单命令最多扫 15 档（目标 20） | 被动价偏移上限 600 限制了档位数；扫单规模与每档挂单量相当 | 偏移 1800；扫单量 3000–5000 |
| single_price | 最长队列 28（目标 200） | 单一价格下对手单立即成交，同侧队列只像 ±1 随机游走增长（~√n） | 新增按阶段交替的侧向偏置（80/20） |
| aggressive_thin | 84% 成交在价格带边缘；每单成交 < 1 | 以 mid 为锚的激进单剩余挂在远处，市场随机游走到边缘 | 新增"以对手最优价为锚"与"固定参考价"选项 |
| extreme_quantity | 最大档位总量 5.9×10¹⁷（目标 2.3×10¹⁸） | 64 个价位分散；且**档位指标只在全量检查时采样，漏掉峰值** | 两个价位；改为每条受理命令后采样最优档 |

`aggressive_thin` 的目标修订（唯一一次修改目标）：先验目标"成交数 ≥ 受理数"在稳态下处于上限附近——每次 maker 完全成交都移除一个
由某个受理订单留下的挂单，而部分成交每个 taker 至多一次——因此它度量的是负载的平衡，不是密度。改用"到达即成交的受理订单比例 ≥ 70%"
（`mixed` 为 38%）。随后发现"多数订单成交"需要大 maker，而"频繁扫空一侧"需要小 maker，二者冲突，于是拆分为 `high_density` 与 `thin_book` 两个画像，
两者都使用原先确定的目标。

## 7. 长跑结果（实际执行）

所有数字均摘自 [evidence/M4/soak/](evidence/M4/soak/) 中的日志（汇总见 `soak_summary.log`，整个矩阵墙钟 113 秒）。
环境：WSL2 Ubuntu 24.04.1，GCC 13.3.0 / Clang 18.1.3，详见 `environment.txt`。"测试吞吐"是整个差分工具链（引擎 + 参考引擎 + 审计器 + 生成器）的速度，**不是引擎性能**，不得用于性能结论（性能属于 M6）。

### 7.1 长跑（gcc-release，每画像 10 个种子 × 200,000 条命令，每 1000 条全量检查，`--require-targets`）

| 画像 | 命令 | 受理 | 成交 | 秒 | 测试吞吐（命令/秒） | 峰值 RSS（MiB） | 单侧最多档位 | 最长队列 | 最大档位总量 | 单命令最多扫档 | 扫空一侧 | 目标 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| mixed | 2,000,000 | 1,373,939 | 1,088,688 | 1.60 | 1,247,439 | 9.8 | 15 | 33 | 1,216 | 9 | 38,209 | 达成 |
| deep_book | 2,000,000 | 1,581,685 | 1,366,809 | 42.44 | 47,121 | 13.5 | 1,542 | 2,545 | 256,514 | 56 | 13,343 | 达成 |
| single_price | 2,000,000 | 1,477,244 | 1,171,644 | 2.55 | 784,042 | 10.6 | 1 | 400 | 13,648 | 1 | 2,916 | 达成 |
| high_density | 2,000,000 | 1,679,712 | 1,473,476 | 1.08 | 1,859,452 | 10.7 | 13 | 31 | 339 | 9 | 62,874 | 达成 |
| thin_book | 2,000,000 | 1,679,826 | 1,437,308 | 1.13 | 1,763,149 | 10.8 | 12 | 83 | 637 | 5 | 216,229 | 达成 |
| cancel_heavy | 2,000,000 | 870,747 | 427,308 | 1.37 | 1,458,830 | 7.7 | 9 | 64 | 1,124 | 8 | 14,547 | 达成 |
| id_churn | 2,000,000 | 1,044,002 | 733,767 | 1.05 | 1,899,930 | 3.6 | 16 | 32 | 836 | 9 | 37,178 | 达成 |
| capacity_edge | 2,000,000 | 982,398 | 667,083 | 0.66 | 3,027,910 | 8.0 | 4 | 4 | 338 | 4 | 132,777 | 达成 |
| extreme_quantity | 2,000,000 | 1,366,063 | 1,095,931 | 1.38 | 1,446,444 | 9.8 | 2 | 63 | 6,919,747,252,664,114,099 | 1 | 41,037 | 达成 |
| extreme_price | 2,000,000 | 1,373,188 | 1,087,794 | 1.53 | 1,309,417 | 9.8 | 15 | 30 | 955 | 11 | 38,128 | 达成 |

合计 20,000,000 条命令、54.8 秒，全部种子无失败，10 个画像目标全部达成。
`extreme_quantity` 的最大档位总量 6,919,747,252,664,114,099 ≈ 0.75 × INT64_MAX，累计受理量 135,550,104,246,963,548,803,181 ≈ 1.5 × 10⁴ × INT64_MAX——后者在 `int64` 记账下会溢出（D-029 的实证）。
峰值 RSS 约 10 MiB 主要来自测试工具自身（执行器记录见过的 ID 的集合随命令数增长；`id_churn` 的 ID 回绕，因此只有 3.6 MiB），不是引擎内存。

### 7.2 长序列故障注入（gcc-release，每画像 5 个种子 × 50,000 条命令，2% 命令武装，`--require-targets`）

| 画像 | 命令（不含故障步） | PREPARE | RESIDUAL | 武装但未触发 | 秒 | 目标 |
|---|---|---|---|---|---|---|
| mixed | 248,521 | 453 | 1,026 | 3,569 | 0.21 | 达成 |
| deep_book | 248,202 | 443 | 1,355 | 3,250 | 6.23 | 达成 |
| single_price | 248,814 | 448 | 738 | 3,862 | 0.32 | 达成 |
| high_density | 248,774 | 451 | 775 | 3,822 | 0.14 | 达成 |
| thin_book | 248,343 | 446 | 1,211 | 3,391 | 0.15 | 达成 |
| cancel_heavy | 248,817 | 452 | 731 | 3,865 | 0.19 | 达成 |
| id_churn | 248,778 | 448 | 774 | 3,826 | 0.14 | 达成 |
| capacity_edge | 248,658 | 452 | 890 | 3,706 | 0.09 | 达成 |
| extreme_quantity | 248,829 | 447 | 724 | 3,877 | 0.18 | 达成 |
| extreme_price | 248,509 | 452 | 1,039 | 3,557 | 0.20 | 达成 |

合计 13,755 次实际注入的失败（PREPARE 4,492，RESIDUAL 9,263），每次之后运行继续，并在后续全部命令上与对齐后的参考引擎一致。"武装但未触发"指所选的第 k 次分配不存在（该命令分配次数少于 k+1），属正常情况。

### 7.3 Sanitizer（gcc-asan：ASan + UBSan + LSan，每画像 2 个种子 × 20,000 条命令，2% 故障）

| 画像 | 命令 | PREPARE | RESIDUAL | 秒 | 峰值 RSS（MiB） | 报告 |
|---|---|---|---|---|---|---|
| mixed | 39,764 | 81 | 155 | 1.37 | 142.7 | 无 |
| deep_book | 39,714 | 79 | 207 | 23.68 | 610.8 | 无 |
| single_price | 39,821 | 80 | 99 | 2.54 | 328.6 | 无 |
| high_density | 39,797 | 79 | 124 | 1.05 | 87.0 | 无 |
| thin_book | 39,740 | 81 | 179 | 1.15 | 93.1 | 无 |
| cancel_heavy | 39,808 | 79 | 113 | 1.32 | 195.0 | 无 |
| id_churn | 39,800 | 80 | 120 | 0.99 | 105.0 | 无 |
| capacity_edge | 39,785 | 79 | 136 | 0.78 | 55.2 | 无 |
| extreme_quantity | 39,792 | 81 | 127 | 1.33 | 171.1 | 无 |
| extreme_price | 39,779 | 81 | 140 | 1.35 | 146.9 | 无 |

ASan 下的 RSS 主要是 ASan 的隔离区与影子内存，与 Release 不可比。`lsan_canary.log` 证明 LeakSanitizer 在本环境中确实生效（故意泄漏 64 字节被报告、退出码 1），因此"无报告"包括失败路径上的资源释放。

### 7.4 跨编译器确定性

10 个画像各 3 个种子 × 50,000 条命令，分别用 gcc-release 与 clang-release 运行，`[coverage]`、`[state]`、`[faults]` 行逐字比较：**10/10 完全相同**（`soak_summary.log` 末尾）。两种编译器使用同一个 libstdc++，因此这验证的是编译器与优化差异，不是标准库差异。

## 8. 风险审查与 M5 必须解决的契约

### 8.1 审查结果

| # | 风险 | 状态 | 依据 |
|---|---|---|---|
| R1 | 部分成交后剩余挂单失败，调用方重试 → 重复执行 | **M5 必须处理** | C2 演示 |
| R2 | 有资源失败的运行无法通过命令日志重放复现 | **M5 必须处理（架构级约束）** | C3 演示 |
| R3 | `logic_error`（计划与执行不一致、M2 契约违反）路径下，taker 的生命周期在事件流中没有终结事件 | 未测试（正常代码不可达，只由变异触发）；**M5 必须把它当作致命错误停止处理** | 代码审查：`matching_engine.cpp` 在抛出前已追加 Accepted/Trade |
| R4 | 引擎没有从快照恢复的接口（簿 + `last_seq`） | **M5 必须新增生产接口** | 现有 API 无法设置 seq 或批量载入簿 |
| R5 | `process` 返回后、事件持久化前进程崩溃 → 引擎状态领先于持久化日志 | **M5 必须设计写入顺序** | C5 提供命令 → seq 区间的对应 |
| R6 | 测试预言机自身的整数溢出（审计器累计量） | 已发现并修复 | D-029；`extreme_quantity` 累计 > INT64_MAX |
| R7 | 指标采样盲区使"到达边界"的结论失真 | 已发现并修复 | D-030 |
| R8 | 失败路径上的资源泄漏 | 已覆盖 | LSan 探针证明泄漏检测在本环境有效；全部故障注入在 ASan 预设与 asan 长跑中运行 |
| R9 | 被移动对象、析构、大簿销毁 | 已覆盖 | M2/M3 生命周期测试；deep_book（5000 单）在 ASan 下销毁无报告 |
| R10 | 事件缓冲区 `reserve` 的 `length_error` | 未测试（需 `max_size` 量级的缓冲区）；属于准备阶段，按设计为强保证 | 代码审查 |
| R11 | seq 在长跑中耗尽 | 仅单元测试（需白盒设置） | M3 `EngineSeqExhaustion` |
| R12 | 引擎非线程安全 | 设计如此（单线程）；**M5/L3 网关必须串行化调用** | ARCHITECTURE §1 |
| R13 | libc++ 未测试 | 未验证 | 跨编译器一致性只在 libstdc++ 上验证 |

**架构层面的结论**（不是 M1–M3 的缺陷，而是 M5 必须接受的约束）：系统的确定性保证是"无资源失败时，相同命令日志 ⇒ 相同事件日志"（C6）。
一旦提交阶段发生资源失败，结果取决于运行环境，命令日志不再是完整的输入。因此恢复的真源应是事件日志（C4），而不是命令日志（D-032）。

### 8.2 M5 必须解决的接口契约

1. **重试判定**：调用方必须在 `process` 前记录 `last_seq()`；抛出后若 `last_seq()` 未变则可重试（C1），若已变则**禁止重试**，
   且必须持久化已追加的事件（C2）。
2. **事件持久化顺序**：明确"处理 → 持久化事件 → 确认"的顺序与崩溃窗口；用 C5（命令 ↔ seq 区间）在恢复时判定哪些命令已生效。
3. **命令日志与事件日志的关系**：命令日志需要能标注"该命令以资源失败结束"，或者恢复只使用事件日志；无论哪种，都必须有测试证明恢复后的事件流与原运行一致。
4. **快照与恢复**：新增从（簿 + `last_seq`）构造引擎的生产接口；契约：恢复后的引擎对后续命令产生与原引擎逐字节相同的事件（可复用 C6 的方法测试）。
5. **重复执行防护（幂等）**：回放驱动在重启后不得把已生效的命令再送入引擎；需要以 seq 或命令序号为键的去重规则。
6. **致命错误**：`logic_error` 与非注入的异常一律停止处理并保留现场；不得在其后继续接受命令（R3）。
7. **文本格式**：事件与命令的稳定文本格式（拒单原因、撤单原因等字符串已在 M1/M3 固定，Golden 文件将依赖它们）。

## 9. 验证证据

### 9.1 全量回归

`scripts/check.sh`，删除 `build/` 后全新构建：六个预设全部通过，每个 **147/147**，`-Werror` 下零警告，墙钟 404 秒（[evidence/M4/check_all_presets.log](evidence/M4/check_all_presets.log)）。

### 9.2 回归锚点与契约（[evidence/M4/anchors_and_contract.log](evidence/M4/anchors_and_contract.log)）

- `MixedProfileReproducesTheM3Stream`：22 个计数与 M3 验收证据逐一相等。
- `CheckIntervalDoesNotChangeTheStream`：10 个画像通过。
- M2 订单簿差分的覆盖计数与 M2 验收证据逐字相同。
- `LongRunsWithRepeatedFaultsStayConsistent`：10 个画像 × 2 种子 × 4,000 条命令、5% 武装，PREPARE 412、RESIDUAL 873，均通过。
- 契约 C1–C6 全部通过（C2、C3 通过即表示所演示的风险真实存在）。

### 9.3 资源清理

LSan 探针：故意泄漏的 64 字节被 LeakSanitizer 报告，退出码 1（[evidence/M4/lsan_canary.log](evidence/M4/lsan_canary.log)）。

### 9.4 变异测试回归（M2 / M3 变异表通过重构后的测试树重跑）

与 M3 验收时的结果逐条比较（"失败测试数"为杀死该变异的测试数量，M3 验收时 → M4）：

| 模块 | 变异 | 结果 | 失败测试数 |
|---|---|---|---|
| M3 | trade executes at the taker's limit instead of the maker's price | 杀死 | 10 → 20 |
| M3 | equal prices do not trade | 杀死 | 18 → 34 |
| M3 | priority taken from the RESTED seq instead of the ACCEPTED seq | 杀死 | 31 → 44 |
| M3 | BOOK_FULL ignores slots freed by complete fills | 杀死 | 2 → 9 |
| M3 | BOOK_FULL check removed | 杀死 | 4 → 14 |
| M3 | duplicate-id check removed | 杀死 | 3 → 17 |
| M3 | remainder never rests | 杀死 | 50 → 65 |
| M3 | fill quantity is the maker's whole remaining | 杀死 | 8 → 23 |
| M3 | plan keeps counting after the order is filled | 杀死 | 3 → 17 |
| M3 | resource failure does not report the cancelled remainder | 杀死 | 4 → 17 |
| M3 | event capacity not reserved before the commit | 杀死 | 4 → 16 |
| M3 | RESTED reuses the last trade's seq | 杀死 | 50 → 64 |
| M3 | unknown cancel reported as INVALID_ORDER_ID | 杀死 | 5 → 17 |
| M3 | rejection does not advance last_seq | 杀死 | 6 → 19 |
| M2 | FIFO: new order joins the front of the queue | 杀死 | 31 → 44 |
| M2 | cancel leaves an empty level behind | 杀死 | 15 → 29 |
| M2 | fill_front forgets to reduce the level total | 杀死 | 10 → 22 |
| M2 | crossing check allows a locked book (>= becomes >) | 杀死 | 2 → 2 |
| M2 | fill_front rejects an exact complete fill | 杀死 | 26 → 43 |
| M2 | rollback: new level not erased when the list insert throws | 杀死 | 6 → 17 |
| M2 | rollback: list node not removed when the index insert throws | 杀死 | 3 → 15 |
| M2 | move constructor leaves the source index populated | 存活 | 0 → 0 |
| M2 | depth returns one level too many | 杀死 | 1 → 1 |
| M2 | invariant checker skips the FIFO seq check | 杀死 | 1 → 1 |
| M2 | for_each_order_while ignores the stop signal | 杀死 | 4 → 18 |

全部 25 个变异的结局与 M3 验收时相同（M3 14/14 杀死；M2 10/11，存活者为 D-013 记录的等价变异）；对每个被杀死的变异，杀死它的测试数量都不减少，
多数因长跑冒烟与长序列故障测试的加入而增加。"M2 交叉检查 `>=` 改 `>`"仍只有 2 个测试能发现：引擎从不向订单簿提交交叉订单，只有订单簿层的测试能观察到它。

**过程记录**：第一次完整证据运行中，变异执行器的源码复制列表没有包含新增的 `testkit/` 目录，CMake 配置失败，两份变异日志只有配置错误——
而证据脚本当时没有检查变异结果，仍然报告成功。修复：执行器复制 `testkit/`；证据脚本对 check、soak、变异三个阶段都在失败或未完成时退出非零。
表中的变异日志是修复后单独重跑得到的（被测代码与其余阶段完全相同；修复只涉及 `scripts/`）。

### 9.5 未验证项

- libc++（两种编译器使用同一个 libstdc++）。
- 执行器检查的独立自检（D-033）。
- R3、R10、R11（见 §8.1）。
- 长跑中的种子数量（每画像 10 + 5 + 3 + 2 个）有限；确定性保证使任何失败都可精确复现，但不能证明未覆盖的种子没有问题。

## 10. 修改任务（供独立练习；只给需求与验收，不给实现）

**T4-1 失败用例最小化。** 为 `lob_soak` 增加 `--minimize`：在失败时自动缩短命令序列（例如逐段删除命令并重跑），输出最短的仍然失败的命令序列。
验收：用 M3 变异"BOOK_FULL 忽略腾出的位置"制造失败，最小化后的序列不超过 10 条命令，且对正确引擎通过；最小化过程本身是确定性的。

**T4-2 新画像：价格带边缘的扫单。** 设计一个画像，使激进单频繁在价格带的上下边缘被截断（clamp），并验证截断后的订单语义。
验收：在 `unmet_targets` 中给出可度量的目标（例如"被截断的激进单占比 ≥ 10%"，需要新增计数器），长跑达标；不得修改已有画像的随机数调用顺序（`mixed` 锚点测试保持通过）。

**T4-3 执行器自检。** 目前执行器的比较逻辑没有独立自检（D-033）。设计一种不改动生产类型的方法，证明"关闭事件比较"会被某个测试发现。
验收：给出方法与代价的书面比较；若实现，新增测试在执行器的事件比较被禁用时失败，在正常时通过。

**T4-4 恢复契约草案测试。** 在不修改生产代码的前提下，用 `ReferenceEngine` 作为"快照载体"写出 M5 契约 4 的测试草案：从事件日志重建簿后，
后续命令在参考引擎上的事件与原运行一致。
验收：测试通过；在 MODULE_NOTES 中说明该草案在 M5 新增生产恢复接口后应如何改写。

**T4-5 资源使用的回归门槛。** 把 §7 中 deep_book 的峰值内存记录为基线，增加一个只在 Release 下运行的检查：峰值 RSS 超过基线 1.5 倍即失败。
验收：说明 RSS 的测量方法与 WSL2 下的噪声；门槛的选择有数据依据。

## 11. 总结

- **已验证能力**：见 §7、§9。
- **遗留问题**：见 §8.1 中"未测试/未验证"各项；执行器检查没有独立自检（D-033）；只在 libstdc++ 上验证。
- **对 M5 的交付**：§8.2 的 7 条契约，其中 C1–C6 已有可执行测试；M5 的设计必须让这些测试继续通过，并为第 3–5 条补充测试。

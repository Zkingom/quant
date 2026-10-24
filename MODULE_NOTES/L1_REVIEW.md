# L1 冻结评审（ROADMAP §2.1）

- 评审日期：2026-10-24（计划窗口 11-24，提前进行）
- 被评审版本：`main` `90dbfdf`（M6 合并后）
- 证据来源：`MODULE_NOTES/evidence/M6/regression/`。该证据在树 `7e2b7c9` 上生成；`7e2b7c9` 到 `90dbfdf` 之间
  `src/ include/ tests/ testkit/ apps/ bench/ cmake/ scripts/`、`CMakeLists.txt`、`CMakePresets.json` 均无改动
  （`git diff 7e2b7c9 90dbfdf -- …` 为空），因此证据适用于被评审版本。本次评审没有重新运行测试。
- 范围：只核对，不开发新功能，不修改代码与测试。发现的问题列入 §3；其中文档类问题按作者决定修正，见 §3.1。

复验命令（在 WSL/Linux 中，仓库根目录）：

```bash
scripts/evidence_m6.sh
```

## 1. 结论

**L1 的全部门槛在实质上满足，没有阻断项。** 有 1 项需要作者裁定证据来源（G1），1 项需要裁定"大场景"的规模（G6），
其余缺口都是文档与代码不一致或文档过时（G2–G5），不影响行为。

| 类别 | 结果 |
|---|---|
| 构建 | ✅ |
| 语义覆盖（8 项） | ✅ 每项都有具名测试 |
| 不变量与守恒（3 项） | ✅ |
| 确定性（3 项） | ✅（"大场景"规模待裁定，G6） |
| 差分测试 | ✅ 由长跑矩阵满足；`check.sh` 自身只有 20 × 5 000（G1） |
| 基准 | ✅ |
| 文档与代码一致 | ⚠️ 有不一致（G2–G5） |

## 2. 逐项核对

### 2.1 构建

| 门槛 | 结果 | 证据 |
|---|---|---|
| 六个预设零警告（`-Werror`）构建 | ✅ | `LOB_WARNINGS_AS_ERRORS` 默认 `ON`（`CMakeLists.txt:23`），`-Werror` 由 `cmake/CompilerWarnings.cmake:23` 加入；`regression/check_all_presets.log` 中 `warning:` 出现 0 次 |
| 六个预设 CTest 全部通过；Sanitizer 使用 `-fno-sanitize-recover=all` | ✅ | 同一日志：六个预设各 197/197；`cmake/Sanitizers.cmake:13`；asan 预设为 `address;undefined`（`CMakePresets.json:18`） |

### 2.2 语义覆盖

| 门槛 | 结果 | 具名测试 |
|---|---|---|
| 价格优先 | ✅ | `EngineTest.BetterPriceTradesFirst` |
| 同价 FIFO | ✅ | `EngineTest.SamePriceTradesInArrivalOrder`、`PriorityIsTheAcceptanceSeq`；`OrderBookTest.SamePriceQueuesInArrivalOrder` |
| 部分成交（taker / maker）与完全成交 | ✅ | `EngineTest.TakerPartialFillRestsRemainder`、`MakerPartialFillKeepsQueuePosition`、`FullFillExecutesAtMakerPrice` |
| 跨多层扫单，成交价为 maker 价 | ✅ | `EngineTest.SweepStopsAtLimitAndRestsRemainder`（逐笔断言成交价 100、101、101）、`SellSweepAcrossBidLevels`、`SweepEmptiesWholeSideThenRests` |
| 撤单：活跃 / 部分成交 / 未知 ID / 已成交 ID / 重复撤单 | ✅ | `EngineTest.CancelRestingOrder`、`CancelAfterPartialFillReturnsRemaining`、`CancelRejections`（ID 0、已成交的 1、未知的 77、撤两次的 3） |
| 重复 ID、每种拒单原因 | ✅ | `EngineTest.DuplicateLiveIdIsRejectedEvenIfItWouldTrade`；`StatelessRejectionsLeaveBookUnchanged` 覆盖 5 种无状态原因；`UNKNOWN_ORDER_ID` 见上；`BOOK_FULL` 见 `EngineCapacityTest.*`——8 种 `RejectReason` 全部覆盖 |
| 边界值：价格带 ±1、数量 0/1/max/max+1、`INT64_MIN/MAX`、`UINT64_MAX` ID | ✅ | `ValidateNewOrder.PriceBandBoundaries`（min−1、min、max、max+1、INT64_MIN、INT64_MAX）、`QuantityBoundaries`（INT64_MIN、−1、0、1、max、max+1、INT64_MAX）、`OrderIdBoundaries`（0、1、UINT64_MAX）、`ValidateCancelOrder.Boundaries` |
| 容量上限拒单 | ✅ | `EngineCapacityTest.OrderThatWouldRestIsRejectedWhenFull` 等 4 个；`OrderBookCapacity.AddRejectsWhenFullAndRecoversAfterRemoval` |

### 2.3 不变量与守恒

| 门槛 | 结果 | 证据 |
|---|---|---|
| 每条命令后不变量检查通过 | ✅ | 引擎单元测试的 `Harness::run` 每条命令调用 `find_invariant_violation()`、审计器与 `matches()`（`tests/engine/test_matching_engine.cpp:75-83`）；`EngineDifferential.MixedProfileReproducesTheM3Stream` 以 `full_check_every = 1` 运行，并断言检查次数 = 100 000 + 20。长跑矩阵为每 1 000 条一次（事件流仍逐条比较） |
| 数量守恒 Σ接受 = 2·Σ成交 + Σ挂单 + Σ撤单剩余 | ✅ | `EventAuditor::check_conservation()` 在每次 `audit()` 末尾执行（`testkit/src/event_auditor.cpp:60-61`），`matches()` 再与引擎订单簿核对；审计器自身由 `AuditorTest.*`（15 个）验证 |
| 订单簿永不交叉 | ✅ | 不变量 6（`include/lob/book/order_book.hpp` 中 `find_invariant_violation` 的说明）；`TamperTest.DetectsCrossedBook` 证明检查有效；`OrderBookContractTest.AddRejectsCrossingOrLockingPrice` |

### 2.4 确定性

| 门槛 | 结果 | 证据 |
|---|---|---|
| 同一输入回放两次，事件流与快照一致 | ✅ | `ReplayContract.C6FailureFreeCommandReplayIsExact`（两个引擎、两种缓冲方式，事件与订单簿相同）；`Golden.*` 每次运行与提交的期望文件逐字节比较（日志与带 FNV-1a 64 校验的快照） |
| GCC 与 Clang 产生相同快照哈希 | ✅ | `Golden.*` 在 clang-debug / clang-release / clang-asan 中同样逐字节通过；长跑矩阵的 xcheck 阶段 10 个画像 gcc/clang 输出一致（`regression/soak_summary.log`） |
| Golden ≥ 5 个手写 + ≥ 1 个固定种子大场景 | ✅（G6） | `tests/golden/`：basic、priority、sweep_partial、rejections、id_reuse + seeded_mixed_2000（种子 1，2 000 条，命令文件本身也被固定） |

### 2.5 差分测试

| 门槛 | 结果 | 证据 |
|---|---|---|
| ≥ 20 个种子 × ≥ 10⁴ 条命令，事件流与快照一致 | ✅（G1） | 长跑矩阵（`scripts/soak.sh`，由 `evidence_m6.sh` 运行）：10 个画像 × 每画像 20 个不同种子（1–10 各 200 000 条；11–15 各 50 000 条且注入分配失败；21–23 各 50 000 条，gcc 与 clang 各一次；31–32 各 20 000 条，ASan），共 230 次运行全部 `ok=1`、0 次失败，全部 ≥ 10⁴ 条。事件流逐条比较，订单簿逐单比较（每 1 000 条与结束时） |

### 2.6 基准

| 门槛 | 结果 | 证据 |
|---|---|---|
| `lob_bench` 可运行，输出吞吐与 p50/p95/p99，负载与环境写入 BENCHMARK | ✅ | BENCHMARK §1–§11；`bench.smoke` 在六个预设中通过 |

### 2.7 文档

| 门槛 | 结果 | 证据 |
|---|---|---|
| README / ARCHITECTURE / DESIGN / DECISIONS / BENCHMARK / MODULE_NOTES / AI_USAGE 与代码一致 | ⚠️ | 见 G2–G5。BENCHMARK、MODULE_NOTES/M6 在 M6 验收时已逐项核对 |

## 3. 缺口清单

| ID | 严重度 | 描述 | 位置 | 建议的处理（未执行） |
|---|---|---|---|---|
| G1 | 需裁定 | ROADMAP §2 规定"所有门槛以 `scripts/check.sh` 的实际输出为证据"，但 `check.sh`（CTest）中的差分测试只有 20 个种子 × 5 000 条；≥ 20 × 10⁴ 的门槛只由长跑矩阵 `soak.sh` 满足，而它不在 `check.sh` 中 | ROADMAP §2；`tests/engine/test_engine_differential.cpp:47` | 二选一：在 ROADMAP 中把 `soak.sh` / `evidence_m6.sh` 列为该门槛的证据来源；或在 CTest 中加入一个满足门槛的差分用例（代价：每个预设多数十秒，ASan 下更多） |
| G2 | 低 | 文档称不变量检查为 `check_invariants()`，实际接口是 `find_invariant_violation()` | ROADMAP §1（M2 行）与 §2.1；DECISIONS 第 32 行 | 改为实际名称 |
| G3 | 低 | 数据流图仍是 M0 时的"目标形态"，写的是 `EventSink`；实际为追加到调用方的 `std::vector<Event>`（D-022 已否决 `EventSink`）。ROADMAP §2.3 的 L3 条目同样写 `EventSink` | ARCHITECTURE §3；ROADMAP §2.3 | 更新数据流图为实际形态；L3 条目改写为实际接口或注明需重新设计 |
| G4 | 低 | ARCHITECTURE §4"当前已实现部分"有 M1–M5 小节，没有 M6 | ARCHITECTURE §4 | 增加 M6 小节（`bench/`、脚本、证据目录） |
| G5 | 低 | ROADMAP 的进度表与 §2.2 已过时：计划窗口仍是原始日期（如 M2 10-22 → 10-28，实际 10-19 → 10-20 完成）；§2.2 L2 的各项全部未勾选，但 M6 已交付其中大部分 | ROADMAP §1、§2.2 | 更新实际日期；按 M6 证据逐项核对 L2（见 §4） |
| G6 | 需裁定 | "固定种子大场景"没有规定规模；现有 Golden 为 2 000 条命令。是否算"大"由作者决定 | ROADMAP §2.1；`tests/golden/seeded_mixed_2000.*` | 接受 2 000 条并在 ROADMAP 写明规模；或增加更大的固定种子 Golden（代价：仓库中增加期望文件体积） |

不是缺口但需要知道的事项：

- m2 变异表有 1 个存活者（"移动构造不清空源"），是 D-013 记录的等价变异（libstdc++ 的容器移动本就清空源），M4、M5、M6 中结果相同。
- 长跑矩阵中订单簿与不变量每 1 000 条检查一次（事件流逐条比较）；"逐条检查"由 CTest 中的单元与差分测试承担。

### 3.1 处理记录（2026-10-24，作者决定；只改文档，不改代码与测试）

| ID | 作者决定 | 处理 |
|---|---|---|
| G1 | 差分门槛以 `soak.sh` / `evidence_m6.sh` 为证据来源 | ROADMAP §2 的证据规则写明该例外；§2.1 差分条目注明证据来源，CTest 差分定位为快速回归 |
| G2 | 修正 | ROADMAP §1、§2.1 与 DECISIONS D-004 改为 `find_invariant_violation()` |
| G3 | 修正 | ARCHITECTURE §3 改为按代码绘制的实际数据流（Session → 引擎 → 批次编码 → ByteSink → 确认；恢复、verify、快照）；ROADMAP §2.3 的 L3 条目改为实际接口并注明 `EventSink` 已被否决（D-022） |
| G4 | 修正 | ARCHITECTURE §4 增加 M6 小节。评审中另发现 M3 小节仍指向已不存在的 `tests/support/`（M4 已并入 `lob::testkit`），一并更正 |
| G5 | 修正 | ROADMAP §1 补实际完成日期与 L1 评审链接；§2.2 按 M6 证据标注现状（3 项满足、3 项部分满足或未满足，括注原因） |
| G6 | 接受 2 000 条 | ROADMAP §2.1 写明"固定种子大场景"规模为 ≥ 2 000 条命令 |

ROADMAP §2.1 的 L1 复选框保持未勾选，在作者确认 L1 通过后勾选。

## 4. 对 L2（ROADMAP §2.2）的现状说明（不属于 L1 评审，供后续决定）

| L2 门槛 | 现状 |
|---|---|
| 固定参数的基线（≥ 5 次重复，中位数与离散度） | M6 已满足（BENCHMARK §7、§10） |
| 剖析证据定位热点 | M6 已满足（perf cpu-clock、callgrind；无 PMU，见风险 R1） |
| ≥ 1 项"假设 → 改动 → 测量 → 结论"实验 | M6 已满足（E1；P1 为空结果） |
| 前后对比：吞吐、延迟、内存、复杂度、工程代价 | 部分满足：内存只有分析值与分配字节数，**没有 RSS 测量** |
| 基线实现保留（编译期或运行期可选），一条命令复现 | **未满足**：基线只以 git 标签 `m6-baseline` 保留，复现需 `scripts/bench_compare.sh m6-baseline main …`，不是编译期/运行期开关 |
| 优化版本通过全部 L1 测试，且与基线的事件流/快照哈希一致 | 测试已满足（§2）；Golden 文件在 E1 前后未改变即说明日志与快照一致，但没有专门的"基线 vs 优化"哈希比较记录 |

## 5. 未验证项（环境所限，与 M5/M6 相同）

- 掉电与操作系统崩溃下的持久性（只测了进程崩溃）；9P 上 `fsync` 的语义。
- libc++；裸机 Linux；硬件计数器。

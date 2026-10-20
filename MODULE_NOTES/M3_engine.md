# M3 engine —— 技术说明

状态：✅ 实现完成，2026-10-20 在 6 个预设下验收通过（证据见 §9 与 [evidence/M3/](evidence/M3/)）。

## 1. 职责与边界

**负责**：把命令（`NewOrder` / `CancelOrder`）变成事件，并维持订单簿的价格-时间优先状态。具体包括：
全部输入校验与拒单语义（无状态 + 有状态）、价格-时间优先撮合（maker 价成交、部分/完全成交、跨层扫单）、
剩余量挂单、撤单、全局事件序号、准备/提交两阶段与异常语义。

**不负责**：文本格式与回放（M5）、快照（M5）、性能测量（M6）、并发。订单簿结构的一致性由 M2 保证，M3 只通过 M2 的公共接口操作它。

**依赖**：M1（类型、命令、`validate_new_order/validate_cancel_order`、`checked_add`）、M2（`OrderBook`）。
M3 对 M2 的唯一改动是新增只读查询 `for_each_order_while`（D-018）。

## 2. 文件组织

```
include/lob/engine/events.hpp           5 种事件、CommandType、CancelReason、seq_of、稳定字符串
include/lob/engine/matching_engine.hpp  MatchingEngine
src/engine/matching_engine.cpp          全部实现
tests/engine/engine_test_util.hpp       事件打印（PrintTo）、构造器、dump()
tests/engine/event_auditor.hpp          事件审计器（A1–A6）
tests/engine/reference_engine.hpp       朴素参考引擎
tests/engine/test_matching_engine.cpp   单元测试（30 个）
tests/engine/test_event_auditor.cpp     审计器自检（15 个）
tests/engine/test_engine_differential.cpp  差分测试（1 个，10 万条命令）
tests/engine/test_engine_alloc_failure.cpp 分配失败注入（7 个，独立可执行文件）
tests/support/rng.hpp, alloc_fault.{hpp,cpp}  M2/M3 共用的测试支撑
examples/engine_demo.cpp
scripts/mutation/{mutate,m2_book,m3_engine}.py, scripts/evidence_m3.sh
```

## 3. 接口契约

| 函数 | 契约 |
|---|---|
| `MatchingEngine(config)` | 非法配置抛 `std::invalid_argument` |
| `process(cmd, events)` | 把本命令的全部事件**追加**到 `events`；不读取、不修改已有元素 |
| 异常：`bad_alloc` / `length_error`（预留容量）、`overflow_error`（seq 将回绕） | 准备阶段抛出：引擎状态与 `events` 内容都不变（强） |
| 异常：`bad_alloc`（挂剩余量） | 已执行成交保留；`events` 以 `Cancelled(InsufficientResources)` 结束该订单；不变量成立；重抛（基本 + 审计） |
| 异常：`logic_error` | 只可能来自程序错误（计划与执行不一致、M2 契约违反），从未在测试中出现 |
| `book()` / `last_seq()` / `config()` | 只读视图 |
| `find_invariant_violation()` | M2 六条不变量 + "簿中每单 seq ≤ last_seq" |

## 4. 关键实现

### 4.1 `process(NewOrder)`（[matching_engine.cpp:44](../src/engine/matching_engine.cpp:44)）

```
准备（只读）
  validate_new_order ──失败──▶ reject()
  book_.contains(id) ──是──▶ reject(DUPLICATE)
  plan = plan_match(order)                      // for_each_order_while，O(k)
  rests = plan.fill < qty
  rests && count − plan.makers_filled ≥ max ──▶ reject(BOOK_FULL)
  n = 1 + plan.trades + rests
  seq = first_seq_of_block(n)                   // overflow_error，不修改
  ensure_capacity(events, n)                    // 可能 bad_alloc，不修改内容
提交
  push Accepted(seq)                            // 容量已有 → 不分配
  while remaining > 0:
     maker = front(对手侧); 不存在或不可成交 → break
     q = min(remaining, maker.remaining)
     fill_front(对手侧, q)                       // 不分配（M2 实测）
     push Trade(++seq, …)
  trades/remaining 与 plan 不一致 → logic_error  // D-025
  remaining == 0 → return
  ++seq
  try   book_.add(剩余, accepted_seq)            // 唯一可能失败点：bad_alloc
  catch bad_alloc → push Cancelled(seq, InsufficientResources); 重抛
  push Rested(seq)
```

要点：

- **为什么 `add` 只可能抛 `bad_alloc`**：M2 `add` 的五个前置条件都已由构造保证——通过无状态校验；ID 不在簿中（准备阶段已查，
  撮合只会移除订单不会加入）；容量（按成交后数量判定）；`accepted_seq` 大于此前所有 seq；不交叉（循环耗尽了全部可成交流动性）。
- **`last_seq_` 逐事件更新**：如果在提交中途抛出，`last_seq_` 恰好等于已追加的最后一个事件，不变量 I2 与 A1 都继续成立。
- **`Accepted` 先于成交写出**：订单一旦通过校验即被受理；之后发生的任何事都由后续事件描述。

### 4.2 `plan_match`（[matching_engine.cpp:133](../src/engine/matching_engine.cpp:133)）

按优先级遍历对手侧，遇到第一个不可成交的价格即停止（之后的都更差），或在剩余量归零时停止。
对每个被触及的 maker 计 `q = min(剩余, maker 剩余)`、成交数 +1、若 `q == maker 剩余` 则完全成交数 +1。
它与提交阶段的循环计算的是同一序列：两者之间簿没有被修改，且两者都按 M2 的优先级顺序访问 maker。

### 4.3 `process(CancelOrder)`（[matching_engine.cpp:154](../src/engine/matching_engine.cpp:154)）

`id == 0` → `INVALID_ORDER_ID`；不在簿中 → `UNKNOWN_ORDER_ID`；否则 seq 检查与容量预留后调用 `noexcept` 的 `book_.cancel`，
追加 `Cancelled(Requested)`。成功路径在预留之后不可能失败，所以撤单是强保证。

### 4.4 事件容量 `ensure_capacity`（[matching_engine.cpp:25](../src/engine/matching_engine.cpp:25)）

需要扩容时取 `max(所需, 2×当前容量)`（并以 `max_size()` 为上限）。只在准备阶段调用。
精确 `reserve(size+n)` 会让一个从不清空缓冲的调用方每条命令都重新分配一次，总复制量 O(N²)。

## 5. 撮合语义与边界条件

| 情形 | 行为 | 测试 |
|---|---|---|
| 无对手流动性 | `Accepted, Rested` | `OrderWithoutLiquidityRests` |
| 限价不可成交 | 挂单，不成交 | `NonCrossingLimitDoesNotTrade` |
| 买价高于卖价 | 以 **maker 价**成交 | `FullFillExecutesAtMakerPrice`、`SellTakerExecutesAtMakerBidPrice` |
| 等价 | 成交 | `EqualPriceTrades` |
| taker 部分成交 | 剩余以原价、受理 seq 挂单 | `TakerPartialFillRestsRemainder`、`PriorityIsTheAcceptanceSeq` |
| maker 部分成交 | 保持队列位置 | `MakerPartialFillKeepsQueuePosition` |
| 价格优先 / 同价 FIFO | 按价、按受理 seq | `BetterPriceTradesFirst`、`SamePriceTradesInArrivalOrder` |
| 跨层扫单至限价 | 逐层成交，剩余挂在限价 | `SweepStopsAtLimitAndRestsRemainder`、`SellSweepAcrossBidLevels` |
| 扫空整侧（50 层） | 50 笔成交后挂剩余 | `SweepEmptiesWholeSideThenRests` |
| 每种无状态拒单 | 簿不变，消耗一个 seq | `StatelessRejectionsLeaveBookUnchanged` |
| 重复活跃 ID（即使可成交） | 拒绝，无成交 | `DuplicateLiveIdIsRejectedEvenIfItWouldTrade` |
| 无状态校验先于重复检查 | 报 `INVALID_QUANTITY` | `StatelessCheckPrecedesDuplicateCheck` |
| 已成交/已撤 ID 复用 | 允许 | `IdsOfFilledAndCancelledOrdersMayBeReused` |
| 撤单：活跃 / 部分成交后 / 0 / 已成交 / 未知 / 重复 | 见 §4.3 | `CancelRestingOrder`、`CancelAfterPartialFillReturnsRemaining`、`CancelRejections` |
| 簿满：需挂单 / 完全成交 / 成交腾位后挂单 / 最后一个空位 | 拒 / 受理 / 受理 / 受理后再拒 | `EngineCapacityTest.*` |
| seq 将回绕 | `overflow_error`，状态与输出不变；恰好剩 1 个 seq 时拒单仍可执行 | `EngineSeqExhaustion.*` |
| 事件追加、不清空调用方内容 | — | `EventsAreAppendedAndSeqIsContiguous` |
| 移动 | 簿与 seq 转移；源对象空、保留 seq、独立可用 | `EngineLifecycle.MoveTransfersBookAndSeq` |

## 6. 一个完整的调用链案例（`examples/engine_demo` 实际输出摘录）

簿：卖 101×5（#1）、101×3（#2）、102×4（#3）；买 99×10（#4）。命令：`NEW id=5 BUY 102x10`。

1. 准备：无状态校验通过；ID 5 不在簿中；`plan_match` 按优先级访问 #1（101，q=5，完全）、#2（101，q=3，完全）、#3（102，q=2，部分），
   剩余归零停止 → `plan = {fill=10, trades=3, makers_filled=2}`；无剩余，不触发容量判定；`n = 1 + 3 = 4`；预留容量。
2. 提交：`#9 ACCEPTED`；`front(卖)` = #1 → `fill_front(卖,5)` → `#10 TRADE maker=1 5@101`；#2 → `#11 TRADE 3@101`；
   #3 → `#12 TRADE 2@102 (maker left 2)`；剩余 0，返回。成交价均为 maker 价（101、102），而 taker 限价是 102。
3. 随后 `NEW id=6 SELL 98x12`：与买 99（#4）成交 10，剩余 2 以卖 98 挂单（`#15 RESTED`）——卖单挂在低于原买价的位置，
   因为对手侧已无可成交的买单。

完整输出：[evidence/M3/engine_demo.out](evidence/M3/engine_demo.out)。

## 7. 对象生命周期

`MatchingEngine` 拥有一个 `OrderBook` 和一个 `SeqNum`。不可拷贝；移动构造/赋值为 `= default` 且 `noexcept`
（`OrderBook` 的移动是 `noexcept`，并把源簿清空，见 D-013）。被移动后的引擎：簿为空、`last_seq()` 保持，
因此继续使用时 seq 仍单调。引擎不持有调用方 `events` 的任何引用：`process` 返回后不再访问它。

## 8. 不变量与验证手段

| 不变量 | 检查者 | 位置 |
|---|---|---|
| I1 M2 结构不变量 | `MatchingEngine::find_invariant_violation` | 每条命令后（单元、差分、注入） |
| I2 挂单 seq ≤ last_seq | 同上 | 同上 |
| A1 seq 从 1 连续 | 审计器 | 同上 |
| A2 生命周期状态机 | 审计器 | 同上 |
| A3 价格-时间优先、maker 价、`q = min`、剩余量 | 审计器（用自己的重建簿选出"应当成交的 maker"） | 同上 |
| A4 不穿价挂单 | 审计器 | 同上 |
| A5 数量守恒（针对引擎簿） | 审计器 `matches()` | 同上 |
| A6 重建簿 = 引擎簿（含 seq） | 审计器 `matches()` | 同上 |
| `BOOK_FULL` 只拒绝不可成交的订单 | 差分测试逐次断言 | 每次 `BOOK_FULL` |
| 与独立实现逐事件一致 | 差分测试 | 每条命令 |

审计器的局限：A5 在审计器**自己**的重建上是构造上成立的（每个数量都已逐事件核对），独立证据在于针对引擎簿的形式；
`BOOK_FULL` 是否"该拒"审计器不判定，交给差分测试。

## 9. 验证证据（2026-10-20 实际执行）

（数字均来自 [evidence/M3/](evidence/M3/) 中的日志；环境见 `environment.txt`。）

### 9.1 六预设

`scripts/check.sh`，从删除 `build/` 后的全新构建开始：六个预设全部通过，每个 **130/130**（M1 29 + M2 48 + M3 53），`-Werror` 下零警告。

### 9.2 差分测试（20 个种子 × 5,000 条命令）

```
[coverage] seeds=20 commands=100000 accepted=68558 trades=54005 cancels=13555
[coverage] multi_maker=12191 multi_level=8145 taker_full=19829 taker_partial_rested=5924 rested_no_trade=42805 maker_partial=18970
[coverage] accepted_while_full=8518 reused_dead_id=3601 max_orders=40
[coverage] rejects: invalid_id=1203 invalid_side=1200 price=1273 qty_invalid=1189 qty_large=1213 duplicate=136 unknown=5571 book_full=6102
```

每条命令后比较：完整事件向量、两侧完整簿（id、价、剩余、seq）、审计器 A1–A6、引擎不变量。
生成器交替"建簿"（被动单、少撤单，直到满容量）与"搅动"（扫单、撤单、非法输入）两个 500 条命令的阶段。
上述计数在 gcc-debug、gcc-asan、clang-release 上完全相同。

生成器调整记录（未放宽任何断言）：初版生成器从未让簿达到容量上限（最高 30/40），覆盖断言因此失败、`BOOK_FULL` 零次；
加入"建簿"阶段后又出现过度饱和（`BOOK_FULL` 占 38%），改为满容量时自动切回"搅动"。

### 9.3 分配失败注入

```
[alloc] random stream: 150 commands x 2 buffer modes, 596 injection points: 150 PREPARE (strong), 446 RESIDUAL (basic + audit)
```

| 场景 | 结果 |
|---|---|
| 无成交挂单，空缓冲 | 两类都出现：容量扩张失败 → PREPARE；`add` 失败 → RESIDUAL |
| 无成交挂单，预留缓冲 | 只有 RESIDUAL（准备阶段无分配） |
| 跨层扫单后挂剩余 | PREPARE + RESIDUAL；RESIDUAL 时成交保留、剩余以 `INSUFFICIENT_RESOURCES` 撤销，与参考引擎逐事件一致 |
| 跨层完全成交，预留缓冲 | 分配次数 **0**（提交阶段不分配的实证） |
| 撤单 / 未知撤单 / 重复 ID / 非法数量，预留缓冲 | 分配次数 0；空缓冲时只有 PREPARE |
| `BOOK_FULL` | 只有 PREPARE（强） |
| 随机流 150 条命令 × 两种缓冲 | 每个注入点都是 PREPARE 或 RESIDUAL 之一，无第三类结果 |

### 9.4 变异测试

```
[KILLED] trade executes at the taker's limit instead of the maker's price: 10 failing: EngineTest.FullFillExecutesAtMakerPrice, EngineTest.SellTakerExecutesA...
[KILLED] equal prices do not trade: 18 failing: EngineTest.EqualPriceTrades, EngineTest.TakerPartialFillRestsRemainder, EngineTest.MakerPartialFillKeepsQueue...
[KILLED] priority taken from the RESTED seq instead of the ACCEPTED seq: 31 failing: EngineTest.OrderWithoutLiquidityRests, EngineTest.NonCrossingLimitDoesNo...
[KILLED] BOOK_FULL ignores slots freed by complete fills: 2 failing: EngineCapacityTest.FillsFreeSlotsBeforeTheRemainderRests, EngineDifferential.MatchesRefe...
[KILLED] BOOK_FULL check removed: 4 failing: EngineCapacityTest.OrderThatWouldRestIsRejectedWhenFull, EngineCapacityTest.LastFreeSlotCanBeUsed, EngineDiffere...
[KILLED] duplicate-id check removed: 3 failing: EngineTest.DuplicateLiveIdIsRejectedEvenIfItWouldTrade, EngineDifferential.MatchesReferenceEngineOnFixedSeeds...
[KILLED] remainder never rests: 50 failing: EngineTest.OrderWithoutLiquidityRests, EngineTest.NonCrossingLimitDoesNotTrade, EngineTest.FullFillExecutesAtMake...
[KILLED] fill quantity is the maker's whole remaining: 8 failing: EngineTest.MakerPartialFillKeepsQueuePosition, EngineTest.SamePriceTradesInArrivalOrder, En...
[KILLED] plan keeps counting after the order is filled: 3 failing: EngineTest.MakerPartialFillKeepsQueuePosition, EngineDifferential.MatchesReferenceEngineOn...
[KILLED] resource failure does not report the cancelled remainder: 4 failing: EngineAllocFailure.RestWithoutTradeEmptyBuffer, EngineAllocFailure.RestWithoutT...
[KILLED] event capacity not reserved before the commit: 4 failing: EngineAllocFailure.RestWithoutTradeEmptyBuffer, EngineAllocFailure.MultiLevelSweepThenRest...
[KILLED] RESTED reuses the last trade's seq: 50 failing: EngineTest.OrderWithoutLiquidityRests, EngineTest.NonCrossingLimitDoesNotTrade, EngineTest.FullFillE...
[KILLED] unknown cancel reported as INVALID_ORDER_ID: 5 failing: EngineTest.CancelRejections, EngineSeqExhaustion.RefusesToWrapAndChangesNothing, EngineDiffe...
[KILLED] rejection does not advance last_seq: 6 failing: EngineTest.StatelessRejectionsLeaveBookUnchanged, EngineTest.CancelRejections, EngineCapacityTest.Or...
survivors: 0/14
```

另外，M2 变异表在 M3 代码就位后重跑（[evidence/M3/mutations_m2_regression.log](evidence/M3/mutations_m2_regression.log)）：10/11 被杀死，
存活者仍是 D-013 记录的等价变异；新增的"`for_each_order_while` 忽略停止信号"被 M2 单元测试与 M3 测试共同杀死，
两个 M2 回滚缺陷现在也会被 M3 的分配注入测试检出。

注意：表中"RESTED seq 作为优先级"的变异最初被编译器杀死（`-Werror` 下 `accepted_seq` 未使用），这不构成测试证据；
已改写为能通过编译的形式重跑。两个只有分配注入测试能检出的变异（"不预留事件容量"、"资源失败不报告剩余"）说明注入测试不可省略。

### 9.5 未验证项与风险

- **libc++ 未测试**（与 M2 相同）；GCC 与 Clang 都使用 libstdc++。
- **`logic_error` 路径**（计划与执行不一致）只由变异测试触发过，正常代码中不可达。
- **剩余挂单失败后调用方的处理**：引擎保证状态一致并重抛；调用方（M5 回放）如何处理（终止 / 记录后继续）尚未实现。
- 差分测试的价格带（1..60）、容量（40）都很小，这是为了制造冲突；大规模长跑留给 M4。
- 性能未测量：每个新订单比单遍撮合多一次 O(k) 只读遍历，代价待 M6 测量（D-019）。

## 10. 修改任务（供独立练习；只给需求与验收，不给实现）

**T3-1 IOC 订单。** 给 `NewOrder` 增加时效（GTC / IOC）。IOC 的剩余量不挂单，而是以新的 `CancelReason::Unfilled`（自定）撤销。
验收：GTC 行为与现有全部测试一致；IOC 完全成交时与 GTC 事件相同；IOC 有剩余时以撤销事件结束且簿不变；
IOC 永远不会被 `BOOK_FULL` 拒绝（论证并测试）；参考引擎与审计器同步扩展，差分测试加入 IOC 并有覆盖计数 > 0；
分配注入测试证明 IOC 的提交阶段在预留缓冲下分配次数为 0（即 IOC 具有强保证）。

**T3-2 自成交保护（STP）。** 给订单增加 `owner`，同 owner 的 taker 与 maker 不成交，采用"撤销 maker"（cancel resting）策略。
验收：写出 STP 与价格-时间优先的交互规则（被撤 maker 后面的同价订单是否仍可成交），并为每条规则写单元测试；
撤销 maker 产生的事件有独立原因；数量守恒在审计器中仍成立；差分测试覆盖 STP 触发次数 > 0。

**T3-3 单遍撮合实验（D-019 的备选）。** 实现一个不预演的撮合版本（编译期开关），比较两种实现的事件流是否一致。
验收：在不触发 `BOOK_FULL` 的负载上两种实现的事件流逐字节一致；书面说明单遍版本的异常保证降到哪一级、`BOOK_FULL` 规则必须如何改变；
在 M6 完成后补充两者的实测性能对比（无数据前不得下结论）。

**T3-4 拒单的 seq 策略。** 把"拒单不消耗 seq"作为配置选项实现。
验收：默认行为不变；开启后 seq 只分配给 Accepted/Trade/Rested/Cancelled，审计器 A1 相应调整；
在 DECISIONS 中比较两种策略对回放丢失检测的影响。

**T3-5 撤单的强保证证明。** 目前撤单在空缓冲时有一次容量分配。写一个测试，证明在任意分配失败下撤单要么完全没有发生，
要么完全发生（不存在"簿已删除订单但没有事件"的结果）。
验收：测试使用 `tests/support/alloc_fault`，扫描所有注入点；用变异"先调用 `book_.cancel` 再预留容量"证明该测试能检出顺序错误。

## 11. 总结

- **已验证能力**：价格-时间优先撮合；maker 价成交；taker/maker 的部分与完全成交；跨层扫单（含扫空整侧）；剩余以受理 seq 挂单；
  撤单全部情形；8 种拒单原因及其判定顺序；`BOOK_FULL` 精确规则及其推论；连续 seq 与溢出保护；事件追加与几何扩容；
  准备阶段失败的强保证、剩余挂单失败的基本保证 + 完整审计（逐个分配点实证）；与独立参考引擎在 10 万条命令上逐事件一致；
  六预设零警告。
- **遗留问题**：见 §9.5。
- **对后续模块的接口承诺**：M5 回放直接调用 `process(Command, events)`，事件流是状态变化的完整描述（可据此重建簿）；
  M4/M6 复用 `tests/engine` 中的生成器、参考引擎与审计器（届时整理为 `lob::testkit`）。

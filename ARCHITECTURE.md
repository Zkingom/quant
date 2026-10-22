# ARCHITECTURE

> 状态标记：✅ 已实现并通过测试 · 🚧 进行中 · 📝 已设计、未实现。
> 本文件只把 ✅ 部分当作事实描述；其余部分是设计意图，会随实现修订。

## 1. 系统定位

单线程、确定性、正确性优先的单标的限价订单簿（LOB）与撮合引擎。
"确定性"的含义：**给定相同的配置与相同的输入命令序列，输出事件序列与最终快照逐字节一致**，
与运行次数、编译器（GCC/Clang）、优化级别无关。

确定性的来源（设计约束，而非事后测试）：

1. 单线程处理，命令严格按输入顺序处理。
2. 不读取墙钟时间；时间优先由引擎内部单调序号（`SeqNum`）表达。
3. 价格、数量全部为整数，不使用浮点。
4. 输出与快照只遍历有序结构（有序价格层 + FIFO 队列），绝不遍历哈希表。
5. 不依赖指针值、地址顺序或未指定的求值顺序。

## 2. 分层与模块

```
            ┌──────────────────────────────────────────────┐
  apps/     │ lob_replay (CLI)   lob_bench   [L3] gateway  │
            └──────┬───────────────────┬───────────────────┘
                   │                   │
  M5 io     ┌──────▼──────┐     ┌──────▼──────┐
            │ parser /    │     │ M6 bench    │
            │ writer /    │     │ harness     │
            │ snapshot    │     └──────┬──────┘
            └──────┬──────┘            │
                   │      ┌────────────▼───────────┐
                   │      │ M4 testkit: workload   │
                   │      │ generator + reference  │
                   │      │ model (naive book)     │
                   │      └────────────┬───────────┘
            ┌──────▼───────────────────▼──────┐
  M3 engine │ MatchingEngine: validate →      │
            │ match → rest/cancel → events    │
            └──────────────┬──────────────────┘
            ┌──────────────▼──────────────────┐
  M2 book   │ OrderBook: price levels, FIFO,  │
            │ order-id index, invariants      │
            └──────────────┬──────────────────┘
            ┌──────────────▼──────────────────┐
  M1 core   │ types, commands, reject codes,  │
            │ config, validation, checked int │
            └─────────────────────────────────┘
```

依赖只允许自上而下；每个模块是独立的 CMake 目标，拥有独立测试可执行文件。

| 模块 | CMake 目标 | 职责 | 依赖 | 状态 |
|---|---|---|---|---|
| M1 core | `lob::core` | 基本类型、命令、拒单原因、配置约束、无状态校验、溢出检查算术 | 无 | ✅ |
| M2 book | `lob::book` | 订单簿数据结构：价格层、同价 FIFO、ID 索引、不变量检查；**不含撮合逻辑** | core | ✅ |
| M3 engine | `lob::engine` | 命令处理：有状态校验（重复 ID、未知 ID、容量）、价格-时间优先撮合、事件生成、序号 | core, book | ✅ |
| M4 testkit | `lob::testkit`（仅测试） | 参考引擎、事件审计器、工作负载画像与生成器、差分执行器、分配故障注入、长跑工具 `lob_soak` | core, book, engine | ✅ |
| M5 io | `lob::io` + `lob_replay` | 文本格式 v1、日志（批次 + 校验和）、快照文件、恢复（事件折叠）、提交会话、verify；`lob_replay` CLI；Golden 与崩溃测试 | core, engine | ✅ |
| M6 bench | `bench/`（`lob_bench`，位于 `bench/` 而不是 `apps/`，只在启用测试时构建） | Book / Engine / Replay / Session 四层的吞吐与延迟分布（p50/p95/p99/p99.9） | book, engine, io, testkit（只用于计时区外的生成） | ✅（2026-10-22 验收） |
| L3 gateway | `lob_gateway` | 单客户端 TCP 网关（有条件执行） | engine, io | 📝 |

模块划分理由：

- **book 与 engine 分离**：book 只保证结构不变量（无空价格层、层总量 = 订单剩余量之和、索引一致），
  engine 负责业务语义（撮合、拒单）。这样可以独立测试数据结构，也可以在 Level 2 替换 book 实现而不改撮合语义。
- **testkit 独立于 io**：差分测试只需要内存中的命令序列，不应被文本格式的变化牵连；
  基准测试复用同一工作负载生成器，保证"测的就是测过正确性的负载"。
- **io 在 engine 之上**：引擎对文本格式一无所知，网关（L3）可复用同一引擎接口。

## 3. 数据流（目标形态）

```
命令文件 ──parser──▶ Command(variant) ──▶ MatchingEngine::process()
                                              │ 1. 无状态校验 (M1 validate_*)
                                              │ 2. 有状态校验 (重复/未知 ID、容量)
                                              │ 3. 撮合：与对手方最优价逐层、逐单成交
                                              │ 4. 剩余挂单 / 撤单
                                              ▼
                                         EventSink (Accepted/Rejected/Trade/Rested/Cancelled)
                                              │
                         ┌────────────────────┼─────────────────────┐
                         ▼                    ▼                     ▼
                   事件文本写出         差分比较(参考模型)        统计/基准
                                              
回放结束 ──▶ 规范快照(文本) + FNV-1a 64 哈希
```

## 4. 当前已实现部分（✅）

### M1 core

- 头文件：`include/lob/core/{types,checked,command,reject_reason,config,validation}.hpp`
- 源文件：`src/core/config.cpp`（仅配置校验，抛异常；其余为 `constexpr` 内联）
- 数据流：`NewOrder` / `CancelOrder` → `validate_new_order` / `validate_cancel_order`
  → `std::optional<RejectReason>`（空表示通过无状态校验）。
- 详见 [MODULE_NOTES/M1_core.md](MODULE_NOTES/M1_core.md)。

### M2 book

- 头文件：`include/lob/book/order_book.hpp`；源文件：`src/book/order_book.cpp`。
- 对上（M3）提供三个修改操作与一组查询：
  `add(NewOrder, SeqNum)`（挂剩余量）、`fill_front(maker 侧, 数量)`（与对手方最优单成交）、`cancel(id)`；
  `best_price / front / find / quantity_at / depth / for_each_order`。撮合循环将由 M3 用
  `front(对手侧)` + `fill_front(对手侧, min(剩余, maker 剩余))` 组合实现，订单簿不感知 taker。
- 内部数据流：`add` → 前置检查 → `try_emplace` 价格层 → 队尾入链表 → 写索引；
  `fill_front` / `cancel` → 减量 → 必要时同时删除节点与索引项 → 层空即删层。
- 测试结构：`lob_book_tests`（单元、Tamper、与参考模型的差分）+ `lob_book_alloc_tests`（独立进程，分配失败注入）。
- 详见 [MODULE_NOTES/M2_book.md](MODULE_NOTES/M2_book.md)。

### M3 engine

- 头文件：`include/lob/engine/{events,matching_engine}.hpp`；源文件：`src/engine/matching_engine.cpp`。
- 对外：`process(Command, std::vector<Event>&)` 把事件追加到调用方缓冲；`book()`、`last_seq()`、`find_invariant_violation()`。
- 数据流：`process(NewOrder)` → 准备（无状态校验 → 重复 ID → `plan_match` 只读预演 → `BOOK_FULL` → seq 区间检查 → 预留事件容量）
  → 提交（`Accepted` → 循环 `front` + `fill_front` 产生 `Trade` → `add` 剩余并发 `Rested`）。
  `process(CancelOrder)` → 校验 → `book.cancel` → `Cancelled` / `Rejected`。
- 只经 M2 公共接口访问订单簿；M2 为此新增只读 `for_each_order_while`（D-018）。
- 测试结构：`lob_engine_tests`（单元、审计器自检、与参考引擎的差分）+ `lob_engine_alloc_tests`（独立进程，分配失败注入）；
  测试支撑代码在 `tests/support/`（`rng.hpp`、`alloc_fault.{hpp,cpp}`，M2/M3 共用）。
- 详见 [MODULE_NOTES/M3_engine.md](MODULE_NOTES/M3_engine.md)。

### M5 io（生产）与 lob_replay

```
include/lob/engine/snapshot.hpp   EngineSnapshot、find_snapshot_violation（引擎新增：snapshot()/restore()）
include/lob/io/  checksum · text_format · journal · sink · snapshot_file · recovery · session · verify
src/io/          对应实现（file_sink.cpp 为 POSIX 文件 I/O）
apps/lob_replay.cpp   run / recover / verify
```

- 依赖方向：`lob_replay → lob::io → lob::engine`；`lob::io` 不依赖 testkit。测试通过 `ByteSink` 接口注入写失败，通过 testkit 注入分配失败。
- 数据流（run）：命令文件 → `parse_command_file` → `recover(日志字节, 快照?, 配置)`（扫描批次、折叠事件、校验边界）
  → `FileSink(path, valid_bytes)`（截断残缺尾部）→ `Session::submit`（去重 → 执行 → 写批次 [+fsync] → 确认）→ 周期性原子快照。
- 恢复状态机、提交协议与保证边界见 DESIGN §6；模块说明见 [MODULE_NOTES/M5_replay.md](MODULE_NOTES/M5_replay.md)。

### M4 testkit（仅测试，不进入生产目标）

```
testkit/include/lob/testkit/   rng · reference_engine · event_auditor · workload · differential · describe · alloc_fault
testkit/src/                   对应实现；alloc_fault.cpp 为 OBJECT 库（替换全局 operator new/delete）
testkit/tools/soak.cpp         lob_soak：画像 × 种子区间的长跑 CLI
```

- 依赖方向：`lob::testkit → lob::engine → lob::book → lob::core`；`tests/` 与 `lob_soak` 依赖 testkit；生产目标从不依赖它。
- 数据流：`WorkloadGenerator(画像, 种子)` 读取 `ReferenceEngine` 状态生成命令 → 同一命令送入 `MatchingEngine` 与 `ReferenceEngine`
  → 事件逐条比较、`EventAuditor` 逐条审计 → 周期性全量检查（簿、守恒、不变量）→ `Coverage` 与边界目标判定。
- 故障注入通过 `FaultInjector` 接口接入执行器，实现在 `lob_testkit_alloc_fault`，只链接进 `*_alloc_tests` 与 `lob_soak`。
- 详见 [MODULE_NOTES/M4_testkit.md](MODULE_NOTES/M4_testkit.md)。

## 5. 构建拓扑

- 构建系统：CMake ≥ 3.25 + Ninja，`CMakePresets.json` 定义 GCC/Clang × Debug/Release/ASan+UBSan 六个预设。
- 开发环境：Windows 宿主 + WSL2 Ubuntu 24.04（构建与测试均在 Linux 内执行）。
- 统一入口：`scripts/check.sh [preset...]`（配置 → 构建 → CTest）。

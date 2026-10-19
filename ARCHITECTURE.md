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
| M2 book | `lob::book` | 订单簿数据结构：价格层、同价 FIFO、ID 索引、不变量检查；**不含撮合逻辑** | core | 📝 |
| M3 engine | `lob::engine` | 命令处理：有状态校验（重复 ID、未知 ID、容量）、价格-时间优先撮合、事件生成、序号 | core, book | 📝 |
| M4 testkit | `lob::testkit` | 固定种子工作负载生成器、朴素参考模型、差分测试驱动 | core, engine | 📝 |
| M5 io | `lob::io` | 文本命令格式解析、事件序列化、规范快照与哈希、回放驱动；`lob_replay` CLI；Golden Tests | core, engine | 📝 |
| M6 bench | `lob_bench` | 吞吐与延迟分布（p50/p95/p99）测量 | engine, testkit | 📝 |
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

## 5. 构建拓扑

- 构建系统：CMake ≥ 3.25 + Ninja，`CMakePresets.json` 定义 GCC/Clang × Debug/Release/ASan+UBSan 六个预设。
- 开发环境：Windows 宿主 + WSL2 Ubuntu 24.04（构建与测试均在 Linux 内执行）。
- 统一入口：`scripts/check.sh [preset...]`（配置 → 构建 → CTest）。

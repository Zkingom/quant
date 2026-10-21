# Deterministic Limit Order Book & Matching Engine

C++20 实现的单线程、确定性、正确性优先的单标的限价订单簿与撮合引擎。
目标：价格-时间优先撮合、可复现回放与快照、严格测试（差分 / Golden / Sanitizer）、可审计的基准。

## 当前状态（2026-10-21）

| 模块 | 状态 |
|---|---|
| M0 工程骨架（CMake、Presets、警告、Sanitizer、GTest） | ✅ |
| M1 core：类型、命令、拒单码、配置约束、无状态校验 | ✅ 29 个测试，6 个预设通过 |
| M2 book：价格层 + FIFO + ID 索引、不变量检查、强异常保证 | ✅ 48 个测试（含 10 万步差分测试与分配失败注入），6 个预设通过 |
| M3 engine：价格-时间优先撮合、拒单语义、事件流、两阶段异常语义 | ✅ 53 个测试（含事件审计器、10 万条命令的参考引擎差分、逐分配点故障注入），6 个预设通过 |
| M4 testkit：可复用测试基础设施、10 个工作负载画像、长跑与长序列故障注入、M5 契约测试 | ✅ 147 个 CTest 用例（含 10 个画像冒烟长跑）；长跑矩阵见 MODULE_NOTES/M4 |
| M5 replay：文本格式 v1、带校验日志、快照与恢复、提交会话、`lob_replay` | ✅ 见 MODULE_NOTES/M5（含 Golden、逐字节截断、写失败与分配注入、`kill -9` 崩溃测试） |
| M6 bench | 📝 未开始 |

撮合引擎、回放与恢复已可用；尚无性能数据（M6）。掉电持久性未验证，见 DESIGN §6.6。 计划与验收门槛见 [ROADMAP.md](ROADMAP.md)。

## 构建与测试

要求：Linux（开发环境为 WSL2 Ubuntu 24.04）、CMake ≥ 3.25、Ninja、GCC ≥ 13 或 Clang ≥ 18、GoogleTest（缺失时自动下载 v1.14.0）。

```bash
sudo apt-get install -y cmake ninja-build g++ clang libgtest-dev
```

单个预设：

```bash
cmake --preset gcc-debug && cmake --build --preset gcc-debug && ctest --preset gcc-debug
```

全部六个预设（GCC/Clang × Debug/Release/ASan+UBSan）：

```bash
scripts/check.sh
```

示例：

```bash
./build/gcc-debug/examples/core_validation_demo
```

```bash
./build/gcc-debug/examples/book_demo
```

```bash
./build/gcc-debug/examples/engine_demo
```

变异测试（在 `/tmp` 的隔离副本上逐一注入缺陷并重跑全部测试）：

```bash
python3 scripts/mutation/m2_book.py "$PWD"
```

```bash
python3 scripts/mutation/m3_engine.py "$PWD"
```

重新生成 M3 的全部验收证据（全新构建、六预设、差分/注入日志、Demo、变异测试）：

```bash
scripts/evidence_m3.sh
```

长跑（列出画像；运行一个画像；运行完整矩阵）：

```bash
./build/gcc-release/testkit/lob_soak --list
```

```bash
./build/gcc-release/testkit/lob_soak --profile deep_book --seeds 1-10 --commands 200000 --check-every 1000 --require-targets
```

```bash
scripts/soak.sh /tmp/soak
```

重新生成 M4 的全部验收证据（全新构建、回归锚点、LSan 探针、长跑矩阵、变异回归）：

```bash
scripts/evidence_m4.sh
```

## 回放与恢复（M5）

```bash
./build/gcc-release/apps/lob_replay run --commands tests/golden/priority.cmd --journal /tmp/j.log --snapshot /tmp/s.snap --echo
```

```bash
./build/gcc-release/apps/lob_replay verify --journal /tmp/j.log --commands tests/golden/priority.cmd
```

```bash
./build/gcc-release/apps/lob_replay recover --journal /tmp/j.log --snapshot /tmp/s.snap
```

同一命令再次运行 `run` 时，已提交的命令按 `cmd_seq` 去重、不会重复执行；崩溃后重启即用同一命令续跑。`--durable` 在确认前 `fsync`。

重新生成 M5 的全部验收证据：

```bash
scripts/evidence_m5.sh
```

## 文档

- [ARCHITECTURE.md](ARCHITECTURE.md) — 模块划分、依赖与数据流
- [DESIGN.md](DESIGN.md) — 数据结构、撮合语义、复杂度
- [DECISIONS.md](DECISIONS.md) — 工程决策与权衡
- [ROADMAP.md](ROADMAP.md) — 实施顺序与 L1/L2/L3 验收门槛
- [BENCHMARK.md](BENCHMARK.md) — 基准方法与实测结果
- [MODULE_NOTES/](MODULE_NOTES/) — 各模块技术说明与修改任务
- [AI_USAGE.md](AI_USAGE.md) — AI 参与情况披露

## 目录结构

```
include/lob/<module>/   公共头文件
src/<module>/           实现
tests/<module>/         GoogleTest 测试（每模块一个可执行文件）
examples/               可运行示例
cmake/                  警告与 Sanitizer 配置
scripts/check.sh        全预设构建 + 测试
scripts/mutation/       模块变异测试脚本
apps/                   lob_replay（回放 / 恢复 / 校验 CLI）
testkit/                测试基础设施（仅测试构建）、长跑工具 lob_soak、命令生成器 lob_gen_commands
tests/golden/           Golden 场景（命令、日志、快照）
```

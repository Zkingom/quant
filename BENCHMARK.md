# BENCHMARK

> 本文件先写测量方案（§1–§6，在任何测量之前写定），再写实测结果（§7 起）。
> 所有数字只适用于 §1 的机器、构建模式与负载；不代表裸机或生产环境性能。

## 1. 环境审计（2026-10-21，测量前）

| 项 | 实际情况 | 对测量的影响 |
|---|---|---|
| 平台 | Windows 11 宿主 + WSL2（Hyper-V）Ubuntu 24.04.1，内核 6.6.87.2-microsoft-standard-WSL2 | 所有结论限于该虚拟化环境 |
| CPU | Intel i7-12700H（混合架构 6P + 8E，20 线程）；WSL 报告为统一的"10 核 × 2 线程" | 无法从虚拟机内识别 P/E 核 |
| vCPU 映射 | 20 个 vCPU 上同一整数循环的速度一致（1.478–1.500 ns/次） | `taskset` 只固定 **vCPU**；宿主可在 P/E 核之间迁移 vCPU——不可控噪声源 |
| 频率控制 | 无 cpufreq（虚拟机内不可见） | 不能固定频率或关闭睿频 |
| 计时器 | `std::chrono::steady_clock` 连续两次读取：p50 17 ns，p99 18–25 ns，分辨率约 15 ns，最大 12–53 µs | 逐操作计时有约 17 ns 下限；p99.9 / max 包含虚拟机中断与抢占 |
| 硬件性能计数器 | 内核："no PMU driver, software events only"；`perf stat` 的 cycles / instructions / cache / branch 均为 `<not supported>` | **没有硬件计数器数据**；缓存与分支只能用 cachegrind **模拟** |
| Profiler | `perf 6.8.12`（linux-tools-generic，非本内核配套）：软件事件与 `cpu-clock` 定时采样可用；valgrind 3.22（callgrind / cachegrind） | 热点用 `perf record -e cpu-clock -g`；精确指令归因用 callgrind |
| 文件系统 | `/tmp`：ext4（WSL 虚拟磁盘）；源码树 `/mnt/g`：9P（到 Windows NTFS 的桥接） | 持久化测量分别标注；9P 结果不代表任何原生文件系统 |
| 编译器 / 标准库 | GCC 13.3.0 + libstdc++（主测）；Clang 18.1.3 + 同一 libstdc++（对照） | 未测 libc++ |
| 构建 | `gcc-release` 预设：`-O3 -DNDEBUG`（CMake Release），无 Sanitizer；订单簿契约检查在 Release 中仍开启（D-012） | Sanitizer 结果不用于性能 |

## 2. 被测层次与场景

| 层 | 场景 | 测什么 |
|---|---|---|
| **Book**（M2） | `book.add_cancel@N`：深度 N ∈ {1 000, 100 000} 的稳态，交替"新增一个不交叉的挂单 / 撤销一个随机在簿订单" | `add`、`cancel` 各自的延迟与吞吐 |
| | `book.fill_front`：单一价位 100 000 单的 FIFO 队列，逐个完全成交（含删层） | `fill_front` |
| | `book.query@N`：深度 N 下 `best_price`、`front`、随机价格 `quantity_at` | 查询 |
| **Engine**（M3） | 7 个 testkit 画像的预生成命令流：`mixed`（常规混合）、`deep_book`（深簿）、`single_price`（长 FIFO 队列）、`cancel_heavy`（高撤单）、`high_density`、`thin_book`（频繁跨层/扫空）、`extreme_quantity`（极端数量） | 完整 `process()`：校验、预演、撮合、事件、挂剩余；延迟另按"本命令成交笔数"分组（0 / 1 / 2–4 / ≥5） |
| **Replay**（M5） | `mixed` 流：命令文件解析、事件格式化、内存会话回放（`Session` + `MemorySink`）、`recover`（扫描 + 折叠）、`verify_journal` | 文本与日志处理的吞吐；回放的逐命令延迟 |
| **Session**（M5） | `mixed` 流：`MemorySink`；`FileSink` 于 ext4 非持久；ext4 持久（每批 `fsync`）；9P 持久 | `submit()` 延迟与吞吐；区分内存执行、写日志、`fsync` |

命令数：引擎每画像 200 000 条；回放 200 000 条；会话非持久 200 000 条、持久模式 5 000 条（受 `fsync` 支配）。种子固定（见 §5）。

## 3. 测量协议

- **数据生成与校验在计时区外**：命令流、订单簿操作序列（每一步用哪个 ID、哪个价格）全部预先生成到数组；计时循环只执行。
  计时结束后再做正确性检查（不同重复之间状态哈希必须相同——确定性也顺带被验证）。
- **两种独立的计时方式**，在不同的重复中进行，互不污染：
  - 吞吐：整个循环前后各读一次时钟，循环内无计时；
  - 延迟：每个操作前后各读一次时钟，记录全部样本，精确排序求分位数（不做直方图近似）。
  延迟样本**包含**约 17 ns 的时钟开销，不做扣除；每次运行同时测量并报告时钟开销。
- **预热**：每个场景先完整执行一次（不计时），以触发首次缺页、填充分配器空闲链表与代码缓存；随后执行计时重复。
- **重复**：每个进程内每个场景 1 次预热 + 5 次吞吐重复 + 1 次延迟重复（延迟重复样本数足够大：引擎 200 000 个/画像）；
  进程间独立运行 **5 次**（`scripts/bench.sh`），每次 `taskset -c 2` 固定到同一个 vCPU。
- **报告**：吞吐取每个进程 5 次重复的中位数，再取 5 个进程的中位数，并给出进程间的最小–最大范围；
  延迟报告 p50 / p95 / p99 / p99.9 / max（各进程的值取中位数，并给出 p99 的进程间范围）。样本少于 10 000 时不报 p99.9。
- **防止被优化掉**：每个结果（事件数、成交量、状态哈希）累加到一个最终输出的校验值中。
- **原始数据**：每次运行输出 CSV 行（`场景,指标,值`），全部保存在 `MODULE_NOTES/evidence/M6/`；汇总表由脚本从原始数据生成。

## 4. 开销归属

| 层 | 包含 | 不包含 |
|---|---|---|
| Book | `OrderBook` 调用本身（含 Release 中仍开启的契约检查） | 操作生成、ID 选择 |
| Engine | `MatchingEngine::process`（含事件追加到复用的缓冲区） | 命令生成（testkit 参考模型）、审计、差分比较 |
| Replay | 解析 / 格式化 / 会话 / 恢复本身 | 文件读写（内存中进行） |
| Session | `submit()`：去重摘要、执行、批次编码、`write`（+ `fsync`） | 命令解析；确认输出 |

testkit（参考引擎、审计器）**不出现在任何计时区内**。文件系统开销只出现在 Session 层的文件模式中，并按 ext4 / 9P 分开报告。

## 5. 固定参数

- 生成器：`testkit::WorkloadGenerator`，每个画像种子 = 1；`text_representable` 不用于引擎层（只用于需要文本的回放层）。
- 订单簿微基准：自带的确定性生成（`testkit::Rng`，种子 7），价格带 [1, 100 000]，数量 [1, 100]。
- 配置：引擎层使用各画像自身的 `EngineConfig`；订单簿微基准 `max_live_orders` = 2 × N。

## 6. Profiling 方案

1. `perf record -e cpu-clock -F 4999 -g`（软件定时采样）对每个引擎画像的吞吐循环采样，`perf report --no-children` 得到自身时间热点。
2. `valgrind --tool=callgrind` 对 `mixed` 与 `deep_book` 的较短流（20 000 条）做**精确指令数（Ir）**归因，按函数分解：预演、撮合、`OrderBook` 增删、`std::map` / `std::list` / `unordered_map`、分配器。
3. `valgrind --tool=cachegrind --cache-sim=yes --branch-sim=yes`：**模拟的**缓存与分支行为（不是硬件数据，仅用于相对比较）。
4. Session 层：`perf trace` 不可用时用 `strace -c` 统计系统调用次数与耗时。
5. 任何不可用的能力如实记录，不推测缺失的数据。

优化假设只在 §7 基线与上述 profiling 结果之后提出。

## 7. 基线结果（m6-baseline 核心 + 测量工具 `dbf9acf`，gcc-release）

条件：2026-10-21 11:03–12:23 UTC；5 个独立进程，均固定 vCPU 2；系统负载 0.51 → 1.26（宿主有桌面应用，未关闭）；
9P 目录为 `/mnt/g/…/quant/build`。全部 5 个进程的校验值相同。原始数据：`MODULE_NOTES/evidence/M6/baseline_gcc/`
（`run_*.csv`、`run_info.txt`；`report.md` 由 `scripts/bench_report.py` 生成）。吞吐 = 进程中位数（进程间范围）；
延迟为单次延迟重复的分位数、取 5 个进程的中位数（单位 ns，含约 17 ns 时钟开销）。

| 场景 | 吞吐 | 范围 | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|
| book.add_cancel@1000（add / cancel 各算一次操作） | 7.21 M/s | 6.61–7.99 M | 144 / 146 | 194 / 193 | 228 / 226 | 396 / 343 |
| book.add_cancel@100000（≈ 63 000 个价格层） | 1.90 M/s | 1.66–2.16 M | 613 / 550 | 1097 / 1376 | 1777 / 2133 | 4675 / 6580 |
| book.fill_front（10 万单队列） | 44.8 M/s | 42.4–48.6 M | 48 | 54 | 69 | 171 |
| book.query@1000 | 48.4 M/s | 46.7–54.5 M | 38 | 89 | 109 | 235 |
| book.query@100000 | 11.3 M/s | 8.9–12.3 M | 37 | 610 | 870 | 1902 |
| engine.mixed | 11.3 M/s | 9.9–11.8 M | 89 | 247 | 411 | 640 |
| engine.deep_book | 7.59 M/s | 6.94–8.46 M | 114 | 262 | 1308 | 3442 |
| engine.single_price | 19.9 M/s | 18.2–20.8 M | 61 | 134 | 364 | 760 |
| engine.cancel_heavy | 16.0 M/s | 14.9–16.5 M | 75 | 166 | 339 | 566 |
| engine.high_density | 14.8 M/s | 13.9–16.1 M | 70 | 191 | 322 | 585 |
| engine.thin_book | 12.2 M/s | 11.7–12.9 M | 95 | 182 | 252 | 394 |
| engine.extreme_quantity | 18.4 M/s | 17.4–19.7 M | 70 | 131 | 183 | 351 |
| replay.parse（命令/s） | 7.10 M/s | 6.97–8.01 M | | | | |
| replay.format_events（事件/s） | 6.78 M/s | 6.04–7.82 M | | | | |
| replay.session_memory | **4.6 k/s** | 4.2–4.7 k | 150 867 | 620 888 | 1 061 870 | 1 437 460 |
| replay.recover | 1.02 M/s | 0.99–1.03 M | | | | |
| replay.verify | 5.83 M/s | 5.35–6.05 M | | | | |
| session.memory | **5.7 k/s** | 4.8–5.9 k | | | | |
| session.ext4_buffered | **5.6 k/s** | 5.3–5.9 k | 158 574 | 403 834 | 960 928 | 1 441 800 |
| session.ext4_fsync（5 000 条） | 798 /s | **194**–806 | 1 199 830 | 1 763 530 | 2 099 220 | — |
| session.9p_fsync（5 000 条） | 1.0 k/s | 0.97–1.0 k | 890 828 | 1 366 950 | 1 657 300 | — |

按成交笔数分组的延迟（完整表见 `report.md`）：成交 ≥ 5 笔的命令 p50 在 251–1 237 ns，deep_book 最高（p99 4 154 ns）；
无成交的命令 p50 在 65–122 ns。尾延迟随单条命令的成交笔数明显上升；这是分组统计的相关性，未做逐样本归因。

读数时需注意：

- **三个 Session 非持久场景的 4.6–5.7 k/s 主要是一个缺陷的成本，不是引擎或 I/O 的成本**（§8、M6 笔记 §5.1）：
  每条命令复制全部已提交记录，200 000 条的流上每条平均要复制约 10 万条记录。
- session.ext4_fsync 有一个进程只有 194 /s（其余 4 个为 780–806 /s），原因未知（WSL 虚拟磁盘 / 宿主 I/O），未剔除；
  9P 上的 `fsync` 反而比 ext4 快，与 §1 的探测一致，9P 的 `fsync` 实际做了什么无法从虚拟机内确认。
- 进程间范围普遍为中位数的 ±10% 左右：在该环境下小于约 10% 的差异不能视为可分辨。

## 8. Profiling 摘要

详见 [M6 技术笔记 §5](MODULE_NOTES/M6_performance.md)，原始输出在 `MODULE_NOTES/evidence/M6/profile/`，命令在 `scripts/profile.sh`。

| 层 | 证据 | 结论 |
|---|---|---|
| Session | 吞吐随流长度 ≈ 1/n 下降；`perf` 95% 样本在 `submit` 的 32 字节步长复制循环 | `committed_.reserve(size + 1)` 导致 O(n²) |
| Engine | callgrind：分配器 ≈ 25%（mixed）/ 35%（deep_book）的 Ir；`plan_match` ≈ 2% | 节点分配是最大单项；预演不是瓶颈 |
| Book@100k | callgrind 模拟：分配器 ≈ 41% Ir；`std::map` 查找占模拟 D1 读缺失约 63%、误预测约 71%（add + cancel） | 深簿下红黑树查找是缓存与分支行为的主要来源（模拟值） |
| Session 文件 | `perf trace`：每批一次 `write`；持久模式每批一次 `fsync` | fsync 场景受 `fsync` 支配 |

与 §6 方案的偏差：（1）整进程 `perf` 采样对引擎场景不可用（样本被测试工具的生成阶段占据，DWARF 帧不一致），引擎分解只用 callgrind；
（2）缓存/分支模拟用 callgrind 的 `--cache-sim --branch-sim` 代替 cachegrind（同一模拟模型，callgrind 可限定收集范围）；
（3）`perf trace` 可用，未用 `strace`（未安装）。

## 9. 优化实验与探针

| 编号 | 对象 | 改动 | 结论 |
|---|---|---|---|
| E1 | `lob::io::Session`（M5 代码） | 提交记录 `committed_` 由精确 `reserve(size + 1)` 改为几何扩容（D-042） | **采纳**；非持久 Session 场景显著更快，fsync 场景与引擎对照在噪声内 |
| P1 | glibc 分配器（环境，不改代码） | `GLIBC_TUNABLES=glibc.malloc.tcache_count=65535` | 分配器慢路径 Ir −8.9%，墙钟在噪声内；**未检出可分辨的收益**，不作为产品改动 |
| — | 订单索引 `index_.reserve` | 未实现 | 证据不支持（M6 笔记 §6.2） |

E1 对照（`scripts/bench_compare.sh`，A = `9dab8ca`，B = `1456fed`，交替 5 对，vCPU 2，2026-10-21 12:46–14:02 UTC，
每进程 3 次吞吐重复；`MODULE_NOTES/evidence/M6/compare_e1_session/`、`compare_e1_engine_control/`）：

| 场景 | A | B | 判定 |
|---|---|---|---|
| replay.session_memory | 4.5 k/s（3.8–4.9 k） | 1.07 M/s（0.93–1.11 M） | 更快 |
| session.memory | 4.3 k/s（1.9–4.8 k） | 1.09 M/s（0.94–1.11 M） | 更快 |
| session.ext4_buffered | 4.6 k/s（1.9–4.7 k） | 713 k/s（612–734 k） | 更快 |
| session.ext4_fsync（5 000 条） | 772 /s（590–782） | 786 /s（727–805） | 噪声范围内 |
| engine.mixed（对照） | 11.25 M/s（11.08–11.60 M） | 11.23 M/s（10.41–11.58 M） | 噪声范围内 |

**归因（必须这样读）**：

- E1 修复的是 M5 `Session` 实现中的一个**复杂度缺陷**（每条命令 O(n) 的复制，整个会话 O(n²)），不是撮合算法、订单簿或引擎的优化。
  E1 没有改动 `src/book/`、`src/engine/`；引擎对照场景在噪声内；撮合引擎本身的性能就是 §7 的 engine.* 数字，M6 没有改变它。
- 倍数（约 150–250 倍）是 200 000 条命令这一流长度的产物：缺陷的成本随流长度平方增长，在 20 000 条的流上基线已有约 105 k/s。
  因此倍数不能外推到其他长度，也不能称为"系统加速了 N 倍"。正确的表述是：Session 每条命令的成本由随已提交命令数线性增长变为常数，
  在本环境中约 0.9 µs（内存）/ 1.4 µs（ext4 非持久）。
- 持久模式（每批 `fsync`，约 1.2–1.3 ms）不受影响：其成本由 `fsync` 支配。

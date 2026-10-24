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

## 10. 最终结果（含 E1 的最终代码）

代码树 `7e2b7c9`（与之后的 `a84271c` 只差证据文件；`run_info.txt` 中记录的 rev 是运行时的 HEAD `a84271c`）；
二进制为 `scripts/evidence_m6.sh` 检查阶段构建的 `build/gcc-release`、`build/clang-release`；2026-10-22 05:57 UTC 起，
gcc 5 个进程后 clang 3 个进程（**未交替**），vCPU 2，负载 0.87 → 1.43；9P 目录同 §7。每个编译器内全部进程的校验值相同。
原始数据：`MODULE_NOTES/evidence/M6/final_gcc/`、`final_clang/`。

| 场景 | gcc 吞吐（范围） | gcc p50 / p99 / p99.9 ns | clang 吞吐（范围） | clang p99 ns |
|---|---|---|---|---|
| book.add_cancel@1000 | 7.98 M/s（7.79–8.66） | add 136 / 214 / 347 | 8.11 M/s（7.94–8.79） | add 211 |
| book.add_cancel@100000 | 2.34 M/s（2.07–2.65） | add 545 / 1 267 / 3 295；cancel 514 / 1 630 / 3 969 | 2.72 M/s（2.25–2.75） | add 1 429 |
| book.fill_front | 49.7 M/s（46.3–51.0） | 46 / 62 / 184 | 56.0 M/s（51.5–58.4） | 60 |
| book.query@1000 | 55.1 M/s（54.8–60.3） | 35 / 95 / 138 | 56.3 M/s（53.7–58.0） | 86 |
| book.query@100000 | 13.6 M/s（11.1–14.9） | 35 / 825 / 1 246 | 19.8 M/s（19.5–21.9） | 813 |
| engine.mixed | 11.6 M/s（11.0–12.5） | 91 / 410 / 611 | 11.9 M/s（11.3–12.5） | 386 |
| engine.deep_book | 8.66 M/s（8.33–9.56） | 103 / 1 163 / 3 105 | 8.92 M/s（8.62–8.99） | 1 125 |
| engine.single_price | 21.3 M/s（20.5–23.2） | 56 / 368 / 762 | 22.1 M/s（21.7–22.2） | 339 |
| engine.cancel_heavy | 16.3 M/s（16.1–17.9） | 73 / 322 / 530 | 18.5 M/s（17.7–18.5） | 304 |
| engine.high_density | 15.4 M/s（15.3–16.7） | 65 / 297 / 531 | 15.5 M/s（13.5–16.4） | 296 |
| engine.thin_book | 12.5 M/s（11.7–14.0） | 91 / 242 / 356 | 13.4 M/s（12.8–13.5） | 236 |
| engine.extreme_quantity | 19.7 M/s（19.0–21.5） | 67 / 173 / 308 | 20.7 M/s（20.3–21.0） | 169 |
| replay.parse | 7.98 M/s（7.65–8.55） | | 8.13 M/s（7.96–8.48） | |
| replay.format_events | 8.01 M/s（7.39–8.86） | | 7.66 M/s（7.58–7.69） | |
| replay.session_memory | 1.11 M/s（0.98–1.22） | 754 / 2 501 / 9 703（max 13.9 ms） | 1.09 M/s（1.07–1.09） | 2 629 |
| replay.recover | 1.05 M/s（0.94–1.12） | | 1.10 M/s（1.10–1.13） | |
| replay.verify | 6.12 M/s（5.42–6.42） | | 5.98 M/s（5.69–6.00） | |
| session.memory | 1.11 M/s（1.00–1.13） | | 1.10 M/s（1.07–1.11） | |
| session.ext4_buffered | 665 k/s（618–748 k） | 1 163 / 3 551 / 19 620 | 742 k/s（708–762 k） | 3 278 |
| session.ext4_fsync（5 000 条） | 769 /s（**196**–829） | 1 228 870 / 2 013 800 / — | 779 /s（769–845） | 1 891 980 |
| session.9p_fsync（5 000 条） | 1.1 k/s（1.0–1.1 k） | 841 657 / 1 488 590 / — | 1.0 k/s（1.0–1.1 k） | 1 715 520 |

**与 §7 基线比较时必须注意（跨会话漂移）**：

- 自 `m6-baseline` 以来，`src/` 与 `include/` 中唯一的改动是 `src/io/session.cpp`（E1）；`src/book/`、`src/engine/`、`src/core/` 逐字节未变，
  编译器与标志相同。然而 §10 的 book / engine 数字普遍比 §7 高 2–23%（例如 book.add_cancel@100000 1.90 → 2.34 M/s，
  engine.deep_book 7.59 → 8.66 M/s）。**这些差异不是任何代码改动的结果**，而是两次测量会话（2026-10-21 与 10-22，宿主状态与负载不同；具体原因未查明）之间的环境漂移。它说明：跨会话的数字对比不能用来判断优化效果；E1 的结论只来自同一会话内交替运行的 §9。
- Session 非持久场景（4.6–5.7 k/s → 0.67–1.11 M/s）的差异远超漂移幅度，且已由 §9 的同会话 A/B 确认归因于 E1。
- session.ext4_fsync 再次出现一个约 196 /s 的进程（§7 中也有一次 194 /s）：两次会话中 5 个进程各有 1 个，原因未知（T6-5）。

**gcc 与 clang**：两者未交替运行（clang 在 gcc 之后），因此只是参考。两边范围不重叠的只有 book.query@100000（clang 19.8 vs gcc 13.6 M/s）与
book.fill_front（56.0 vs 49.7 M/s）；其余场景的范围重叠。未分析原因（未比较生成代码），不据此推荐编译器。

## 11. 结论与适用范围

- 所有数字只适用于 §1 的 WSL2 虚拟机（i7-12700H 宿主、无 PMU、无频率控制）、Release 构建与单线程、预生成命令流；不代表裸机、其他 CPU 或生产负载。
- 单线程撮合引擎在 7 个画像上为每秒数百万到两千万条命令量级，p99 约 0.2–1.2 µs（含约 17 ns 计时开销）；深簿与单条命令多笔成交时尾延迟最高。
  M6 **没有**改变撮合引擎的性能；E1 修复的是 Session 的复杂度缺陷（§9）。
- 持久模式的吞吐由 `fsync` 决定（本环境约 0.8–1.1 k 批/s），与引擎速度无关；9P 与 ext4 的 `fsync` 语义与成本不可外推到原生文件系统。
- 约 ±10% 以内的差异在本环境中不可分辨；跨会话的比较不可用于归因。
- 未验证：硬件计数器下的缓存与分支行为；裸机；libc++；多核或网络负载。

## 12. L2 前后对比：基线实现 vs 优化实现（`LOB_BASELINE`）

两种实现由同一源码树的编译期选项选择（D-045），只在 Session 的一个源文件上不同（E1）。

| 维度 | 基线（`LOB_BASELINE=ON`） | 优化（`OFF`） | 来源 |
|---|---|---|---|
| 吞吐 | 非持久 Session 4.3–4.6 k/s（200 000 条） | 0.71–1.09 M/s | §9（同会话交替 5 对） |
| 延迟（replay.session_memory p50 / p99 / p99.9） | 152 µs / 1.11 ms / 1.52 ms | 0.78 / 2.56 / 10.2 µs | §9 |
| 撮合引擎、订单簿 | 不变 | 不变 | §9 对照在噪声内；源码相同 |
| **峰值 RSS**（实测，见下） | mixed 31 948 KiB；deep_book 35 948 KiB | mixed 30 068 KiB；deep_book 30 484 KiB | `evidence/L2/rss/rss.csv` |
| 事件流 / 快照 | 与优化版逐字节相同 | 与基线逐字节相同 | [MODULE_NOTES/L2_EQUIVALENCE.md](MODULE_NOTES/L2_EQUIVALENCE.md)（16 个输入） |
| 代码复杂度 | — | `src/io/session.cpp` +7 / −2 行（一个条件 + 一次 `reserve`），无新类型、无新接口 | 提交 `1456fed` |
| 工程代价 | — | 回归测试 +24 行（分配字节数守卫），testkit 分配钩子 +5 行（字节计数），M5 变异表 +2 个变异；基线副本作为冻结代码保留（D-045） | `1456fed`、`a2b6375` |

**峰值 RSS 的测量**（`scripts/l2_rss.sh`，2026-10-24 13:14 UTC，树 `419dec9`，gcc 13.3 Release，WSL2）：

- 每次运行为 `lob_replay run --commands 输入 --journal 日志`（非持久，日志在 ext4），由 GNU `time -f %M` 取子进程的 `ru_maxrss`；
  测量下限（同一方法下 `/bin/true`）为 1 120 KiB。两个构建交替运行，各 5 次，取中位数。
- 输入：`lob_gen_commands` 种子 1、200 000 条；mixed 5 046 206 字节、deep_book 5 475 030 字节（`lob_replay` 把整个命令文件读入内存，两个构建相同）。

| 画像 | 优化 中位数（范围） | 基线 中位数（范围） | 差值 |
|---|---|---|---|
| mixed | 30 068 KiB（30 064–30 068） | 31 948 KiB（31 912–31 952） | 优化低 1 880 KiB（−5.9%） |
| deep_book | 30 484 KiB（30 484–30 488） | 35 948 KiB（35 792–35 948） | 优化低 5 464 KiB（−15.2%） |

- 实测与此前的估算**方向相反**：M6 只按最终容量估算（200 000 条时优化版 `committed_` 容量 262 144 条 ≈ 8.4 MB，
  基线精确容量 ≈ 6.4 MB），推断优化版占用更多。估算没有包含基线在每次重新分配时新旧两块同时存在、以及大块反复分配释放对堆的影响；
  这是可能的解释，**未验证**。deep_book 的差值为何远大于 mixed（两者的提交记录数相同）也**未解释**。
- 同一构建 5 次之间的差异 ≤ 156 KiB，远小于两种实现之间的差值。
- 墙钟时间仅作背景（不是基准结果）：优化 0.30 / 0.34 s，基线 36.95 / 36.35 s（mixed / deep_book 中位数）。
- 只适用于上述输入长度与非持久模式；RSS 随流长度的变化未测。

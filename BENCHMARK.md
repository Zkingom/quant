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

## 7. 基线结果

（待测量）

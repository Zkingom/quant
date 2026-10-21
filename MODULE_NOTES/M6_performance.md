# M6 Benchmark, Profiling & Performance Engineering —— 技术说明

状态：🚧 进行中。测量方法见 [BENCHMARK.md](../BENCHMARK.md) §1–§6（测量前写定）。

## 1. 职责与边界

**负责**：可复现的性能测量体系（四个层次）；基于证据的瓶颈识别；至少一项有明确假设的优化对照实验；
优化后的完整正确性与恢复回归；如实的结论与适用范围。

**不负责**：达到任何预设吞吐或延迟数字；网络层（L3）；多线程。

**约束**：基线冻结为标签 `m6-baseline`（= `main` `7e9396b`，M5 验收状态）；测量工具提交 `dbf9acf` 只新增 `bench/` 与脚本，
`include/ src/ apps/` 与基线逐字节相同（`git diff m6-baseline -- include src apps` 为空）。基线测量完成前不修改撮合核心。

## 2. 文件组织

```
bench/bench_common.hpp   计时、精确分位数、CSV 输出、校验值、吞吐测量模板
bench/bench_book.cpp     Book 层：add/cancel（深度 1k / 100k）、fill_front（10 万单队列）、查询
bench/bench_engine.cpp   Engine 层：7 个画像的完整 process()，延迟按成交笔数分组
bench/bench_io.cpp       Replay 层（解析、格式化、内存回放、恢复、verify）与 Session 层（内存 / ext4 / ext4+fsync / 9P+fsync）
bench/main.cpp           参数、环境行、时钟开销
scripts/bench.sh         独立进程、固定 vCPU、保存原始 CSV
scripts/bench_compare.sh 两个修订在独立 worktree 中构建、交替运行
scripts/bench_report.py  汇总：运行中位数、范围、噪声判定
```

## 3. 测量设计的关键选择

- **计时区内只有被测调用**：命令流与订单簿操作序列预先生成；testkit 参考引擎只用于生成，从不进入计时区。
- **吞吐与延迟分开测**：逐操作计时本身约 17 ns（BENCHMARK §1），会显著放大订单簿微操作的成本；吞吐循环内不读时钟。
- **每次重复都从同一初始状态开始**（`setup` 不计时），并在计时后比对最终状态哈希——确定性因此在每次测量中被顺带检查。
- **A/B 交替运行**：基线与候选版本在同一台机器上交替执行，抵消缓慢的环境漂移；区间重叠判为"噪声范围内"。

## 4. 基线结果

（待测量完成后从原始数据生成）

## 5. Profiling

环境限制（BENCHMARK §1）：WSL2 不暴露 PMU，`perf` 只有软件事件（`cpu-clock`），没有 cycles / instructions /
cache-misses / branch-misses。因此：墙钟热点用 `perf record -e cpu-clock`；指令构成用 callgrind（**指令数，不是时间**）；
缓存与分支行为只有 callgrind 的**模拟**（`--cache-sim --branch-sim`，模拟的是 valgrind 假定的 I1/D1/LL 结构，
不是这颗 CPU 的真实缓存与预测器，只用来比较函数之间的相对份额）。全部命令见 `scripts/profile.sh`，输出在
`evidence/M6/profile/`。Profiling 构建：Release `-O3 -g`（与基线同一编译器和标志，只多调试信息），位于 WSL ext4。

### 5.1 Session：每条命令的成本随已提交命令数线性增长（证据充分）

| 流长度 n（session.memory，同一构建） | 吞吐 |
|---|---|
| 20 000 | 104 579 /s |
| 40 000 | 32 675 /s |
| 80 000 | 15 997 /s |
| 160 000 | 4 669 /s |

n 翻倍、吞吐降为约 1/2～1/3：总成本 O(n²)。`perf record`（cpu-clock，n = 80 000）：95.2% 的样本在
`Session::submit` 自身；`perf annotate` 显示这些样本集中在 `operator new` 之后一个步长 0x20（32 字节）的
`movdqu/movups` 复制循环。来源是 `session.cpp` 中的 `committed_.reserve(committed_.size() + 1)`：
**精确** reserve 使每次 submit 都重新分配 `committed_` 并复制全部已有提交记录（每条 32 字节）。
这条 reserve 本身是 M5 为"提交点之后不分配"引入的（D-038），方向正确，但增长策略错误——
与 M3 已在 D-022 记录并修正过的事件缓冲区问题完全相同，M5 没有沿用。

因此基线中 replay.session_memory / session.memory / session.ext4_buffered（200 000 条，约 4.6–5.7 k/s）
测到的主要是这个复制，而不是引擎、编码或文件系统。

### 5.2 Session 文件层：系统调用（`perf trace -s`，计数可信，耗时含跟踪开销、不作结果）

- ext4 buffered，3 次会话 × 20 000 条：`write` 59 915 次（≈ 每条命令一次）、`fsync` 3 次（每次会话结束 `sync()`）。
  即"buffered"指不 fsync，但每个批次仍是一次 `write` 系统调用（`FileSink` 不在用户态缓冲）。
- ext4 fsync，3 × 5 000 条：`fsync` 15 006 次（≈ 每条命令一次）、`write` 14 979 次。
  `write` 比命令数少 21 次而 `fsync` 多 6 次，原因未查明（可能是 `perf trace` 丢事件），只记录、不解释。

### 5.3 Engine：`MatchingEngine::process` 的指令构成（callgrind，20 000 条 × 3 遍）

收集只在 `process(std::variant…)` 内开启（`--toggle-collect` 对**每个**匹配函数的进入/退出都切换，
第一次用 `process*` 匹配到了嵌套的重载，导致收集被关掉、结果无效；已删除重做，见 §10）。

| 份额（self Ir） | mixed（≈ 727 Ir / 条） | deep_book（≈ 1 130 Ir / 条） |
|---|---|---|
| 分配器合计（malloc / _int_malloc / free / _int_free / operator new） | ≈ 25% | ≈ 35% |
| `process(NewOrder)` 自身 | 11.6% | — |
| `unordered_map` 插入（订单索引） | 3.6% | 3.3% |
| `std::map` 插入 / 再平衡（价格层） | ≈ 3–4% | ≈ 4–5% |
| `plan_match`（撮合预演） | 2.5% | 1.7% |
| 事件缓冲 `ensure_capacity` | ≈ 3.6% | — |

结论：引擎内最大的单项是节点分配与释放（`std::list` 订单节点、`unordered_map` 节点、`std::map` 层节点）；
撮合预演 `plan_match` 只占约 2%，不是瓶颈。

### 5.4 Book：深度 100k 的 add/cancel（callgrind 缓存 / 分支**模拟**）

收集开启于 `OrderBook::add` 与 `OrderBook::cancel`（包含每次重复前 10 万单初始建簿的 add）。共 947 M Ir：

- 分配器合计 ≈ 41% Ir（_int_malloc 11.3%、malloc 10.5%、_int_free 8.2%、free 4.2%、operator new 3.7%、consolidate 2.2%、unlink 1.2%）；
- 两侧 `std::map` 查找（`insert` 中的 `lower_bound` 路径）：约 11% Ir，但占模拟 **D1 读缺失的 50%**、**条件分支误预测的 55%**；
  `cancel` 中的 map 查找/删除再占 D1 读缺失 13%、误预测 16%。约 63 000 个价格层的红黑树深度约 16，每层一次依赖加载；
- `unordered_map` 插入 5.1% + `_M_need_rehash` 1.5%；
- 模拟 LL 读缺失的 98% 落在 `add` 读取输入 `NewOrder`（基准预生成的 20 万条命令数组，顺序冷读）——这是测试工具成本，不是订单簿成本。

### 5.5 没做到 / 不可用

- 没有硬件计数器：无法给出真实 IPC、缓存缺失率与分支误预测率；§5.4 的数字是模拟值。
- 整进程 `perf` 采样对 engine 场景不可用：样本被测试工具的工作负载生成（参考引擎）占据，DWARF 展开给出不一致的内联帧
  （`evidence/M6/profile/perf_engine_deep_book.txt`）。引擎分解因此只依赖 callgrind。
- 尾延迟与成交密度的关系只有基线中按成交笔数分组的分位数（§4），没有逐样本的原因归因。

## 6. 优化实验

### 6.1 实验 E1：Session 提交记录几何扩容（假设先于实现写定）

**假设**：`committed_.reserve(size + 1)` 使每条命令 O(n) 复制（§5.1）。改为与 `ensure_capacity`（D-022）
相同的策略——只在容量不足时 reserve `max(size + 1, 2 × capacity)`——后：

1. replay.session_memory / session.memory / session.ext4_buffered 的吞吐提高一个数量级以上，且与流长度无关
   （session_scaling 各 n 下每条成本接近常数）；
2. 剩余成本由引擎、批次编码与（buffered 时）每批一次 `write` 组成；
3. session.ext4_fsync / 9p_fsync（5 000 条，由 fsync 主导）变化在噪声范围内；
4. engine / book / parse / recover / verify 场景不受影响（代码路径不经过 Session），作为对照。

**保持不变的语义**：reserve 仍在任何效果之前；失败仍返回 `NOT_APPLIED` 且无痕迹；提交点之后仍不分配
（`push_back` 落在已保证的容量内）；日志字节、事件、恢复结果逐字节相同。

**代价**：`committed_` 容量最多为已提交命令数的 2 倍（每条 32 字节）；一次扩容的复制仍是 O(n)，但摊还 O(1)，
单次扩容会带来一次尖峰延迟（与引擎事件缓冲相同的取舍）。在内存紧张时，翻倍可能在"只多一条"本可成功时失败
（返回 `NOT_APPLIED`，调用方可重试）——语义安全，但比精确 reserve 更早拒绝。

**备选**：（a）翻倍失败时退回精确 reserve：消除上述更早拒绝，多一条分支；（b）分块存储 / `std::deque`：
无整体复制，但 `std::deque` 无法预留下一块，提交点之后的 `push_back` 可能分配，违反 D-038；（c）只存最近 K 条：
改变重复检测语义，否决。

**测量计划**：`scripts/bench_compare.sh` 交替 5 对，过滤为 `replay.session_memory`、`session.memory`、
`session.ext4_buffered`、`session.ext4_fsync`，并附 `engine.mixed` 作对照；另测 session_scaling。

## 7. 正确性与恢复回归

## 8. 结论与适用范围

## 9. 修改任务

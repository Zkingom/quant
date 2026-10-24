# M6 Benchmark, Profiling & Performance Engineering —— 技术说明

状态：✅ 已验收（2026-10-22）。测量方法见 [BENCHMARK.md](../BENCHMARK.md) §1–§6（测量前写定）。

## 1. 职责与边界

**负责**：可复现的性能测量体系（四个层次）；基于证据的瓶颈识别；至少一项有明确假设的优化对照实验；
优化后的完整正确性与恢复回归；如实的结论与适用范围。

**不负责**：达到任何预设吞吐或延迟数字；网络层（L3）；多线程。

**约束**：基线冻结为标签 `m6-baseline`（= `main` `7e9396b`，M5 验收状态）；测量工具提交 `dbf9acf` 只新增 `bench/` 与脚本，
`include/ src/ apps/` 与基线逐字节相同（基线测量时 `git diff m6-baseline -- include src apps` 为空）。基线测量完成前不修改撮合核心；
此后整个 M6 中 `src/` 与 `include/` 唯一的改动是 E1 的 `src/io/session.cpp`。

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
scripts/bench_env_compare.sh 同一二进制在两种环境变量下交替运行（P1）
scripts/profile.sh       §5 的全部 profiling 步骤
scripts/evidence_m6.sh   优化后的完整正确性与恢复回归（复用 evidence_m5.sh）
```

## 3. 测量设计的关键选择

- **计时区内只有被测调用**：命令流与订单簿操作序列预先生成；testkit 参考引擎只用于生成，从不进入计时区。
- **吞吐与延迟分开测**：逐操作计时本身约 17 ns（BENCHMARK §1），会显著放大订单簿微操作的成本；吞吐循环内不读时钟。
- **每次重复都从同一初始状态开始**（`setup` 不计时），并在计时后比对最终状态哈希——确定性因此在每次测量中被顺带检查。
- **A/B 交替运行**：基线与候选版本在同一台机器上交替执行，抵消缓慢的环境漂移；区间重叠判为"噪声范围内"。

## 4. 基线结果

完整表格见 [BENCHMARK §7](../BENCHMARK.md)（原始数据 `evidence/M6/baseline_gcc/`）。要点（gcc-release，WSL2，5 个进程中位数）：

- Book：深度 1 000 时 add/cancel 7.2 M/s；深度 100 000（约 63 000 个价格层）1.9 M/s，p99 约 1.8–2.1 µs。
- Engine：7 个画像 7.6–19.9 M/s；deep_book 最慢，其成交 ≥ 5 笔的命令 p50 约 1.2 µs。
- Replay：解析 7.1 M 命令/s，恢复 1.0 M 命令/s。
- Session：非持久 4.6–5.7 k/s——由 §5.1 的缺陷主导；持久（每批 `fsync`）约 0.8–1.0 k/s。
- 进程间范围约 ±10%：本环境下更小的差异不可分辨。

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

n 翻倍、吞吐降为约 1/2～1/3.4：总成本 O(n²)。`perf record`（cpu-clock，n = 80 000）：95.2% 的样本在
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
第一次用 `process*` 匹配到了嵌套的重载，导致收集被关掉、结果无效；已删除重做，见 AI_USAGE M6 第 2 条）。

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

**改动**（`m6-exp1-session-growth`，`1456fed`）：`session.cpp` 中一处——只在 `size == capacity` 时
`reserve(max(size + 1, min(2 × capacity, max_size)))`；另加回归测试 `IoAllocFailure.CommitRecordStorageDoesNotGrowQuadratically`
（4 000 条命令的分配字节数 < 64 MB；实测基线 260.9 MB、E1 5.1 MB，测试在基线代码上失败），
testkit 分配钩子增加字节计数，M5 变异表增加两个变异（精确 reserve；完全不 reserve）。

**结果**（`scripts/bench_compare.sh`，A = `m6-perf` `9dab8ca`，B = E1 `1456fed`，同一机器交替 5 对，vCPU 2，
2026-10-21 12:46–14:02 UTC，每进程 3 次吞吐重复；原始数据 `evidence/M6/compare_e1_session/`、`compare_e1_engine_control/`）：

| 场景 | A 吞吐（范围） | B 吞吐（范围） | 判定 | A p50 / p99 / p99.9 ns | B p50 / p99 / p99.9 ns |
|---|---|---|---|---|---|
| replay.session_memory | 4.5 k/s（3.8–4.9 k） | 1.07 M/s（0.93–1.11 M） | 更快，约 236× | 152 493 / 1 111 690 / 1 517 430 | 776 / 2 560 / 10 217 |
| session.memory | 4.3 k/s（1.9–4.8 k） | 1.09 M/s（0.94–1.11 M） | 更快，约 250× | — | — |
| session.ext4_buffered | 4.6 k/s（1.9–4.7 k） | 713 k/s（612–734 k） | 更快，约 155× | 151 472 / 1 131 140 / 1 626 530 | 1 169 / 3 326 / 10 301 |
| session.ext4_fsync（5 000 条） | 772 /s（590–782） | 786 /s（727–805） | 噪声范围内（+1.8%） | 1 303 630 / 2 083 020 / — | 1 217 790 / 1 956 200 / — |
| engine.mixed（对照） | 11.25 M/s（11.08–11.60 M） | 11.23 M/s（10.41–11.58 M） | 噪声范围内（−0.2%） | 90 / 425 / 676 | 88 / 414 / 635 |

session_scaling（E1，`evidence/M6/profile/session_scaling_e1.txt`，同一构建、单次运行）：n = 20 000 / 40 000 / 80 000 / 160 000 时
1.07 / 1.10 / 1.11 / 1.10 M/s——每条命令成本不再随 n 增长（§5.1 中同样的 n 为 105 k → 4.7 k/s）。

**评估**：

- 假设 1、3、4 成立：三个非持久 Session 场景提高两个多数量级，fsync 场景与引擎对照在噪声内。
  吞吐倍数取决于流长度（O(n²) → O(n)），**不是**一个可外推的"加速比"；在 20 000 条的流上基线本来就有约 105 k/s。
- 假设 2：E1 后 session.memory ≈ 1.09 M/s，而 engine.mixed ≈ 11.2 M/s——剩余约 90% 的每条成本在引擎之外
  （批次编码、摘要、MemorySink 追加）；ext4_buffered 比 memory 每条多约 0.5 µs（每批一次 `write`）。这部分未再细分。
- **最大延迟没有改善**：replay.session_memory 的 max 在 A 为 14.3–20.9 ms、B 为 14.3–16.3 ms。
  有限诊断（`bench_io.cpp` 的 `max_latency_attribution`，只改测试工具；`evidence/M6/profile/max_latency_attribution.txt`，3 个进程）：
  - 3 次的最大值都落在第 171 305 条命令，且该次 `submit` 中 **MemorySink 的 `std::string` 重新分配**（约 24 MB 的日志缓冲区扩容）——
    这是**测试工具的内存日志存储**，不是 Session 或引擎的成本；ext4 文件模式没有这个缓冲区，其 max 为 0.44–0.48 ms（5 个进程之一为 4.8 ms，未归因）；
  - 排除 MemorySink 扩容后 max 为 0.52–0.59 ms，与 `committed_` 翻倍（E1 预期的代价）一致；
  - 两者都排除后 max 为 52–101 µs，**未归因**（可能是虚拟机中断或抢占，未验证）。
  - 尚未确认的是：扩容尖峰中复制与缺页各占多少（T6-8 的剩余部分）。
- 内存（L2 实测，替换此前的容量估算）：`lob_replay run` 处理 200 000 条命令的峰值 RSS（`ru_maxrss`，5 次中位数）——mixed 优化 30 068 KiB / 基线 31 948 KiB，
  deep_book 优化 30 484 KiB / 基线 35 948 KiB；优化版更低，与按最终容量的估算（优化 8.4 MB vs 基线 6.4 MB）方向相反，原因未验证（BENCHMARK §12）。分配字节总量见上面的回归测试。
- 方差：B 的进程间范围与 A 的范围同量级（相对值）；A 的 session.memory / buffered 各有一个进程降到 1.9 k/s，原因未查。

**结论**：采纳（D-042）。归因：E1 修复的是 M5 Session 实现中的复杂度缺陷，不是撮合算法、订单簿或引擎的加速；
`src/book/`、`src/engine/` 未改动，engine.mixed 对照在噪声内。引擎性能仍是 §4 / BENCHMARK §7 的数字。

### 6.2 实现前即放弃的候选：预留订单索引桶（`index_.reserve(max_live_orders)`）

§5.4 中 `_M_need_rehash` 占 1.5%，看上去指向哈希表扩容。但该函数是**每次插入都调用**的检查，
reserve 之后仍然每次调用；真正的 rehash（`_M_rehash`）在两份 profile 中都没有进入列表。证据不支持
"预留能省下可测的成本"，而预留会让构造函数按 `max_live_orders` 一次性分配（大配置下可能在构造时 `bad_alloc`）。
因此不实现、不测量，只记录。

### 6.3 探针 P1：分配器慢路径的敏感度（不改代码，假设先于测量写定）

§5.3/§5.4 的分配器份额（25–41% Ir）是否意味着"减少分配"有同等收益，取决于成本落在 malloc/free 的快路径
（tcache 命中，几十条指令）还是慢路径（bin 管理、`malloc_consolidate`、`unlink_chunk`）。在改动 M2 的容器或分配器
（风险见 D-044、T6-2）之前，先用 glibc 可调参数测这个敏感度，**不改任何代码**：

- A：同一二进制，默认环境；B：`GLIBC_TUNABLES=glibc.malloc.tcache_count=65535`（每个大小类的线程缓存容量
  从默认 7 增大，释放的节点几乎都留在 tcache，下一次同尺寸分配直接命中）。
- **假设**：若慢路径是分配器成本的主要部分，B 在 book.add_cancel@100000、engine.deep_book、engine.mixed 上吞吐明显更高
  （超出 A/B 进程间范围）；若在噪声内，则分配器成本主要是快路径本身，减少分配次数（池化）才是唯一途径，
  且其收益上限只能由真正的代码实验确定。
- 这只是对环境的探针：结论不作为产品改动，不改变默认配置。
- 测量：`scripts/bench_env_compare.sh`，E1 构建，交替 5 对，`--filter book.add_cancel@100000`、`engine.deep_book`、`engine.mixed` 各一组。

**结果**（2026-10-22 UTC，原始数据 `evidence/M6/probe_p1_tcache/`）：

| 场景 | A（默认） | B（tcache_count = 65535） | 判定 |
|---|---|---|---|
| book.add_cancel@100000 | 2.03 M/s（0.84–2.49 M） | 1.97 M/s（0.78–2.26 M） | 噪声范围内（−2.7%） |
| engine.deep_book | 8.42 M/s（7.35–8.82 M） | 8.26 M/s（8.16–8.92 M） | 噪声范围内（−2.0%） |
| engine.mixed | 11.06 M/s（9.96–11.58 M） | 11.38 M/s（11.08–12.05 M） | 噪声范围内（+2.9%） |

可调参数确实生效（`callgrind_allocator_ir.txt`，book.add_cancel@100000 的 add/cancel 内）：总 Ir 947 M → 862 M（−8.9%），
`_int_malloc` −50%，`malloc_consolidate` −84%，`unlink_chunk` 消失；`malloc` / `free` / `operator new` 的快路径指令数不变。

**解读**：慢路径约占这些场景指令的 9%，去掉它在墙钟上不可分辨（该环境下进程间范围约 ±10%，book@100k 的 A、B 各有一个进程
跌到约 0.8 M/s）。因此**不能**说"分配器慢路径无影响"，只能说"其影响小于本环境的分辨力"。分配器的其余份额是快路径本身
（每次分配/释放的固定指令），只有减少分配次数才能去掉——这是 T6-2 的问题，M6 内不做（D-044）。

## 7. 正确性与恢复回归

`scripts/evidence_m6.sh`（= M5 的完整证据脚本，输出到 `evidence/M6/regression/`），树 `7e2b7c9`（含 E1），2026-10-22 04:51 UTC 起：

| 阶段 | 结果 |
|---|---|
| 六个预设（gcc/clang × debug/release/asan，含 UBSan） | 每个预设 197/197 通过（M5 验收时 195 个 + E1 的分配字节数测试 + `bench.smoke`） |
| io / 分配注入 / 引擎差分 | `lob_io_tests` 43、`lob_io_alloc_tests` 4、`EngineDifferential` 2，全部通过；其中逐分配点扫描覆盖每条命令直到不再命中 |
| 进程崩溃（Release，40 次 `kill -9`，30 000 条） | 40/40 次落在运行中，最终日志与快照和不中断的运行逐字节相同 |
| CLI Golden | 通过 |
| M4 长跑矩阵 | 通过，gcc/clang 结果一致 |
| 变异 m5 / m3 / m2 | 0/17、0/14、1/11 存活；存活者为 D-013 记录的等价变异（与 M4、M5 相同） |

**E1 改变分配点后的语义核对**（逐项对应 D-038）：

- 提交前准备：reserve 仍在执行之前；扩容失败返回 `NOT_APPLIED`、日志不变、重试提交与无故障时相同的批次——由逐分配点扫描对每条命令验证
  （E1 后多数命令在准备阶段少一次分配，扫描点随之变化，扫描本身"注入到不再命中"，不依赖固定点数）。
- 提交点之后不分配：变异"完全不 reserve"被扫描杀死（`push_back` 在容量满的命令上分配，注入后异常逃逸 → 测试失败）。
- 去重 / 内容核对 / 写失败中毒 / fsync：对应变异全部被杀死，杀死它们的测试与 M5 相同。
- 复杂度：变异"精确 reserve"只被新的分配字节数测试杀死——M5 的全部正确性测试都不能发现 O(n²)，这是 E1 新增该测试的原因。

性能证据与正确性证据分开存放：本节只引用 `regression/`；性能数字只来自 `baseline_*`、`final_*`、`compare_*`、`probe_*`、`profile/`。

## 8. 结论与适用范围

- **可复现的基线**：BENCHMARK §7（`dbf9acf`）与 §10（最终代码，gcc 5 + clang 3 个进程），原始 CSV、环境与命令全部在 `evidence/M6/`。
- **有证据的瓶颈**：Session 的 O(n²) 提交记录复制（§5.1，perf + 规模实验 + 回归测试三方面证据）；引擎内最大单项是节点分配（§5.3/§5.4，指令数与模拟值）。
- **完整的假设驱动实验**：E1（§6.1），假设先于实现写定；同会话交替 A/B；完整回归（§7）。采纳。
  归因：Session 实现缺陷的修复，**不是**撮合引擎的加速；引擎性能未被 M6 改变。
- **如实的负/空结果**：P1（§6.3）——分配器慢路径指令 −8.9%，墙钟未检出可分辨的变化；预留索引桶在实现前即被证据否定（§6.2）。
- **未解释 / 部分解释**：replay 最大延迟约 15 ms 已定位到测试工具 MemorySink 的缓冲区扩容，剩余 52–101 µs 未归因（T6-8）；
  fsync 场景中每次会话约有一个进程低至约 195 /s（T6-5）；`perf trace` 的计数差异（T6-7）；跨会话 2–23% 的漂移原因（BENCHMARK §10）。
- **适用范围**：只适用于 WSL2 虚拟机、无 PMU、Release、单线程、预生成负载；不推广到裸机或生产环境。

## 9. 修改任务

只给出任务与验收要求，不给答案。

- **T6-1 不依赖时间的性能回归守卫**：E1 的回归测试用"分配字节数"而不是耗时。为引擎设计一个同类守卫
  （例如每条命令的分配次数或 callgrind 指令数上限），要求在 Debug / ASan 预设下也稳定、不随机器变化，
  并用一个变异证明它能捕获 O(n²) 或"每条命令多一次分配"的回退。
- **T6-2 订单节点池化实验**：按 §6 的格式（假设、证据、备选、生命周期与移动语义影响、最小改动、同环境 A/B、完整回归）
  做一次分配减少实验。必须先回答：`OrderBook` 的移动赋值（D-013）在池属于各自订单簿时如何保持 Locator 有效；
  M2 的分配失败测试如何在不减弱的前提下仍覆盖每个分配点。
- **T6-3 持久模式的组提交**：设计"多个命令共享一次 `fsync`"的协议，写清确认时机、崩溃后哪些命令可能丢失、
  与 D-038 / D-040 的关系，再测量。不得让任何已确认命令在进程崩溃后丢失。
- **T6-4 非持久模式的用户态缓冲**：`FileSink` 每批一次 `write`（§5.2）。评估在用户态缓冲的语义代价
  （进程崩溃时丢失的范围、`sync()` 的含义），只有在语义写清后才测量。
- **T6-5 解释 fsync 离群运行**：session.ext4_fsync 在基线（194 /s）与最终测量（196 /s）中各有一个进程远低于其余（约 800 /s）。设计能区分
  "宿主 I/O 干扰"与"虚拟磁盘行为"的测量，而不是剔除该运行。
- **T6-6 硬件计数器对照**：在有 PMU 的裸机 Linux 上重跑 §5.4，比较 callgrind 模拟的 D1 缺失 / 误预测份额与
  真实计数器，写出模拟结论中哪些成立、哪些不成立。
- **T6-7 `perf trace` 计数差异**：§5.2 中 `write` 比命令数少 21 次、`fsync` 多 6 次。查明原因（工具丢事件还是代码路径），
  不允许在未查明前修改代码。
- **T6-8 回放最大延迟的剩余归因**：§6.1 已把约 15 ms 的 max 定位到 MemorySink 缓冲区扩容、约 0.5 ms 定位到 `committed_` 翻倍。
  剩余问题：扩容尖峰中复制与缺页各占多少；排除两类扩容后仍有的 52–101 µs 来自哪里。给出证据，再决定是否值得处理。

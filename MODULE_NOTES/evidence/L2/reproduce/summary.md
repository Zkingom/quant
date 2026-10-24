# L2 复现汇总（基线 vs 优化）

由 `scripts/l2_reproduce.sh` 生成。

- 时间：2026-10-24T14:51:04Z，耗时 4375 s；源码 `21cc273`
- Linux 6.6.87.2-microsoft-standard-WSL2 x86_64；12th Gen Intel(R) Core(TM) i7-12700H；编译器 g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0；Release
- 构建：基线 `build/l2-baseline`（`LOB_BASELINE=ON`）、优化 `build/l2-optimized`（`OFF`）；基准固定 vCPU 2；每项 5 次（交替）

## 1. 等价性（`equivalence.md`）

**结论：16 个输入全部一致**——两种实现产生逐字节相同的事件流、日志与快照。

## 2. 吞吐与延迟（A = 基线，B = 优化；`bench_*/report.md`，原始 CSV 在 `bench_*/A|B/`）

### session（`--filter session --reps 3`）

| scenario | A throughput (range) | B throughput (range) | B vs A | verdict | A p50/p99/p99.9 ns | B p50/p99/p99.9 ns |
|---|---|---|---|---|---|---|
| replay.session_memory | 4.7 k/s (4.4 k/s..5.2 k/s) | 1.11 M/s (1.10 M/s..1.19 M/s) | +23537.2% | faster | 139913/1161720/1561560 | 750/2481/11811 |
| session.memory | 4.9 k/s (3.9 k/s..5.2 k/s) | 1.13 M/s (1.05 M/s..1.19 M/s) | +22839.1% | faster | –/–/– | –/–/– |
| session.ext4_buffered | 4.9 k/s (4.7 k/s..5.1 k/s) | 700.1 k/s (679.8 k/s..794.2 k/s) | +14193.7% | faster | 140505/1182010/1614210 | 1081/3131/10083 |
| session.ext4_fsync | 768 /s (711 /s..834 /s) | 785 /s (192 /s..804 /s) | +2.3% | within noise | 1290040/2471920/– | 1216560/1990160/– |

### engine.mixed 对照（撮合引擎代码在两种构建中相同）

| scenario | A throughput (range) | B throughput (range) | B vs A | verdict | A p50/p99/p99.9 ns | B p50/p99/p99.9 ns |
|---|---|---|---|---|---|---|
| engine.mixed | 11.86 M/s (11.63 M/s..12.75 M/s) | 12.12 M/s (11.00 M/s..12.21 M/s) | +2.2% | within noise | 87/403/614 | 85/398/610 |

## 3. 峰值 RSS（`rss/rss.csv`，KiB，中位数（范围））

| 画像 | 基线 | 优化 | 优化 − 基线 |
|---|---|---|---|
| mixed | 31948 (31948–31952) | 30064 (29768–30068) | -1884 (-5.9%) |
| deep_book | 35948 (35872–35948) | 30484 (30324–30488) | -5464 (-15.2%) |

数字只适用于本次运行的机器、构建与负载；解读见 BENCHMARK §9、§12。

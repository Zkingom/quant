# L2 等价性：优化实现 vs 基线实现

由 `scripts/l2_equivalence.sh` 生成，不要手工编辑表格；重新生成：

```bash
scripts/l2_equivalence.sh build/l2-optimized build/l2-baseline MODULE_NOTES/L2_EQUIVALENCE.md 100000
```

## 条件

- 生成时间：2026-10-24T13:09:12Z；源码：`a2b6375`
- 优化构建：`build/l2-optimized`（`LOB_BASELINE=OFF`，编译 `src/io/session.cpp`）
- 基线构建：`build/l2-baseline`（`LOB_BASELINE=ON`，编译 `src/io/baseline/session.cpp` = 标签 `m6-baseline` 的 `src/io/session.cpp`）
- 编译器：c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 / c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0；构建类型：Release / Release
- 运行方式：`lob_replay run --commands 输入 --journal J --snapshot S`（非持久模式），两个构建使用同一份输入文件
- 输入：`tests/golden/*.cmd`（手写场景与固定种子场景），以及每个 testkit 画像一份生成的命令文件（种子 1，100000 条）

## 比较的内容

- **事件流哈希**：日志中事件行（ACCEPTED / REJECTED / TRADE / RESTED / CANCELLED）的 SHA-256，前 16 位十六进制；
- **日志哈希**：整个日志文件的 SHA-256（事件 + 批次框架 + 每批 FNV-1a 64 校验），前 16 位；
- **快照哈希**：`lob_replay run` 结束行 `[run]` 中的 `state_hash`（规范快照文本的 FNV-1a 64），以及快照文件的 SHA-256 前 16 位。
- 每个构建的日志还被单独 `lob_replay recover` 一次，恢复出的 `state_hash` 必须等于运行结束时的值；任何哈希为空或运行失败，脚本直接报错退出。

## 结果

| 输入 | 命令数 | 输入 SHA-256 | 事件流 优化 / 基线 | 日志 优化 / 基线 | state_hash 优化 / 基线 | 快照文件 优化 / 基线 | 结论 |
|---|---|---|---|---|---|---|---|
| `gen_cancel_heavy_s1_n100000` | 100000 | `d51a84489290c4d3` | `89598d5570609016` / `89598d5570609016` | `5693c584e2af38c6` / `5693c584e2af38c6` | `1ff2c67aa063f5a1` / `1ff2c67aa063f5a1` | `d1084ea6c5f6cd55` / `d1084ea6c5f6cd55` | 一致 |
| `gen_capacity_edge_s1_n100000` | 100000 | `2be399e840b6eadb` | `805fedf256734c11` / `805fedf256734c11` | `b2b10fcabe4443db` / `b2b10fcabe4443db` | `186d725f633a9d15` / `186d725f633a9d15` | `ff38a0f69b2336a1` / `ff38a0f69b2336a1` | 一致 |
| `gen_deep_book_s1_n100000` | 100000 | `6d7f395c0416f983` | `544f695212a952f1` / `544f695212a952f1` | `cadadc1a0345a844` / `cadadc1a0345a844` | `85697967e4756cc9` / `85697967e4756cc9` | `37ac8263a0fd4278` / `37ac8263a0fd4278` | 一致 |
| `gen_extreme_price_s1_n100000` | 100000 | `fb41acd5210869d0` | `5a87866ce4b676d0` / `5a87866ce4b676d0` | `3cfdbdf62ff5e194` / `3cfdbdf62ff5e194` | `89b85cac3c188dde` / `89b85cac3c188dde` | `c95da0ee8543e332` / `c95da0ee8543e332` | 一致 |
| `gen_extreme_quantity_s1_n100000` | 100000 | `04ae453e31895378` | `a132f7a22d1eefb8` / `a132f7a22d1eefb8` | `a19fbdc12fa2560c` / `a19fbdc12fa2560c` | `014230d5d3bbbf56` / `014230d5d3bbbf56` | `2abc7b80d2e1797b` / `2abc7b80d2e1797b` | 一致 |
| `gen_high_density_s1_n100000` | 100000 | `f1b9ebd72e8aefa1` | `b81d5364466c519d` / `b81d5364466c519d` | `dfb563d3e8d565a3` / `dfb563d3e8d565a3` | `a83e8f5e3f3bb177` / `a83e8f5e3f3bb177` | `600cf5f9070a9833` / `600cf5f9070a9833` | 一致 |
| `gen_id_churn_s1_n100000` | 100000 | `74d30a6c49f618dd` | `5fa81b49dd33a7f1` / `5fa81b49dd33a7f1` | `35ab20ecc568e600` / `35ab20ecc568e600` | `eb25942d5a42cfd5` / `eb25942d5a42cfd5` | `df555bcfff8ea683` / `df555bcfff8ea683` | 一致 |
| `gen_mixed_s1_n100000` | 100000 | `12f7975187b0b3df` | `6031086a46482878` / `6031086a46482878` | `0a6b38f3de8c3867` / `0a6b38f3de8c3867` | `2aab09123a5b00f2` / `2aab09123a5b00f2` | `92777482ae8374f3` / `92777482ae8374f3` | 一致 |
| `gen_single_price_s1_n100000` | 100000 | `bf9648b869078cb3` | `698d0c523ce3d122` / `698d0c523ce3d122` | `4cdccc0c1799d67f` / `4cdccc0c1799d67f` | `90abfdaf61f0c4ce` / `90abfdaf61f0c4ce` | `cb5deaa49c545c75` / `cb5deaa49c545c75` | 一致 |
| `gen_thin_book_s1_n100000` | 100000 | `8834b73399a43af1` | `b56433693a5c837b` / `b56433693a5c837b` | `1b2275c2e10e9c9a` / `1b2275c2e10e9c9a` | `83572f2b57dcd864` / `83572f2b57dcd864` | `c6701966de2e534d` / `c6701966de2e534d` | 一致 |
| `golden_basic` | 5 | `0e861cb9577ad6fc` | `f4311f66ebdafe9a` / `f4311f66ebdafe9a` | `c9c10d026c1eab5b` / `c9c10d026c1eab5b` | `834eb856693ac544` / `834eb856693ac544` | `ade94360f75cce25` / `ade94360f75cce25` | 一致 |
| `golden_id_reuse` | 5 | `ea37d3b3a585d059` | `96033db3a28f74e9` / `96033db3a28f74e9` | `d0b769e3aa3be9b6` / `d0b769e3aa3be9b6` | `62ec2e3c57828ae2` / `62ec2e3c57828ae2` | `9a03e52cbc666340` / `9a03e52cbc666340` | 一致 |
| `golden_priority` | 5 | `c3573b6397ce7a8e` | `d0289859d26a525f` / `d0289859d26a525f` | `34ce655ceb8bd1ca` / `34ce655ceb8bd1ca` | `1272a4385011051a` / `1272a4385011051a` | `83ae81d3d306f6ea` / `83ae81d3d306f6ea` | 一致 |
| `golden_rejections` | 15 | `8bea140ef31a91af` | `63a8c134071c3968` / `63a8c134071c3968` | `6aa740f5581a9d7f` / `6aa740f5581a9d7f` | `71131cd9ff0ca564` / `71131cd9ff0ca564` | `e9b48633cc275d9f` / `e9b48633cc275d9f` | 一致 |
| `golden_seeded_mixed_2000` | 2000 | `52bea0ef9463eae5` | `567c1ba3b2838037` / `567c1ba3b2838037` | `42753ab5b93b7400` / `42753ab5b93b7400` | `999521b601e4b5f6` / `999521b601e4b5f6` | `16e53af5f6756192` / `16e53af5f6756192` | 一致 |
| `golden_sweep_partial` | 6 | `2f6c15e29b5817d8` | `91a3db236e465eb7` / `91a3db236e465eb7` | `cd9d26b0ee552682` / `cd9d26b0ee552682` | `773dc0bb41cf87d1` / `773dc0bb41cf87d1` | `e9ce3bc09ea3d0e3` / `e9ce3bc09ea3d0e3` | 一致 |

**结论：16 个输入全部一致**——两种实现产生逐字节相同的事件流、日志与快照。

## 脚本自检（负对照）

用故意引入单个变异的基线构建运行本脚本，脚本都没有报告等价（`MODULE_NOTES/evidence/L2/equivalence_negative_control.txt`）：
M2 "新单入队首"与 M3 "成交价取 taker 限价"被恢复自检拒绝（退出码 2）；M3 "未知撤单报 INVALID_ORDER_ID"产生自洽日志，
由比较表检出（16 个输入中 13 个不一致，退出码 1；一致的 3 个恰好不含未知 ID 的撤单）。

## 适用范围

- 只覆盖上述输入与非持久模式；持久模式（`--durable`）只改变是否逐批 `fsync`，不改变写入的字节，未单独比较。
- 不覆盖分配失败下的行为：两种实现在内存不足时的分配点不同（M6 §7），其等价性由各自通过同一套分配注入测试保证，而不是由本脚本比较。

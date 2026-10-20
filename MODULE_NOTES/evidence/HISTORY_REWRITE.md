# 发布前的历史改写记录

2026-10-20，在首次推送到 GitHub（`Zkingom/quant`）之前，用 `git filter-branch --env-filter` 把全部 10 个提交的作者与提交者
从 `Wulinbhk <个人邮箱>` 改为 `Zkingom <228309269+Zkingom@users.noreply.github.com>`（GitHub noreply 地址），以免个人信息公开。
**只改了姓名与邮箱**：时间、提交说明与每个提交的文件内容（tree）都不变；四个分支改写后的 tree 与改写前逐一相同。
由于作者信息是提交对象的一部分，所有提交哈希都变了。（中间曾有一次改写为另一账号的 noreply 地址的版本，未推送，已丢弃。）

`MODULE_NOTES/evidence/` 中的日志在改写前生成，其中记录的提交哈希（例如 `environment.txt` 中的 "base commit"）是**改写前**的值。
对照表（按提交顺序；每行新旧两个提交的 tree 哈希与提交说明都相同，已逐行校验）：

| 改写前 | 改写后 | 提交说明 |
|---|---|---|
| `5558c54` | `d0cc4fd` | Project skeleton, architecture and acceptance gates |
| `618afa0` | `aad6758` | M1 core: types, commands, reject reasons, config, validation |
| `3ee06a4` | `9c9e66c` | WIP(M2): order book skeleton and unit tests -- NOT ACCEPTED |
| `c82a592` | `20fed4b` | M2 book: order book with invariants, differential and fault-injection tests |
| `32d242c` | `f543850` | Merge M2 book (accepted 2026-10-20) |
| `fa0b56f` | `0c082ab` | M3 engine: price-time matching with two-phase exception semantics |
| `128aacc` | `5092526` | Merge M3 engine (accepted 2026-10-20) |
| `ad4b9dc` | `a20be20` | Remove committed Python bytecode cache; ignore __pycache__ |
| `05949de` | `a1c6722` | M4 testkit: reusable test infrastructure, long runs, M5 contract |
| `e516a46` | `7063f7b` | Merge M4 testkit (accepted 2026-10-20) |

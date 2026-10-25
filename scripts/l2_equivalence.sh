#!/usr/bin/env bash
# L2: runs the SAME command files through an optimized build (LOB_BASELINE=OFF)
# and a baseline build (LOB_BASELINE=ON) of lob_replay, and compares
#   - the event stream: SHA-256 of the journal's event lines only, and of the
#     whole journal (events + batch framing + per-batch checksums);
#   - the final state: the engine state hash printed by `lob_replay run`
#     (FNV-1a 64 of the canonical snapshot) and SHA-256 of the snapshot file.
# Writes a Markdown report and exits non-zero if any input differs.
#
# Usage: scripts/l2_equivalence.sh OPT_BUILD_DIR BASE_BUILD_DIR OUT_MD [COMMANDS_PER_PROFILE]
#   e.g. scripts/l2_equivalence.sh build/l2-optimized build/l2-baseline MODULE_NOTES/L2_EQUIVALENCE.md
# Inputs: every tests/golden/*.cmd, plus one generated file per testkit
# profile (seed 1, COMMANDS_PER_PROFILE commands, default 100000).
set -u
OPT=$1; BASE=$2; OUT=$3; N=${4:-100000}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d "$HOME/lob_l2_equivalence.XXXXXX")
die() { echo "l2_equivalence: $*" >&2; rm -rf "$W"; exit 2; }

# Each build must be what it claims to be.
flag() { sed -n 's/^LOB_BASELINE:BOOL=//p' "$1/CMakeCache.txt"; }
[ "$(flag "$OPT")" = "OFF" ] || die "$OPT is not a LOB_BASELINE=OFF build"
[ "$(flag "$BASE")" = "ON" ] || die "$BASE is not a LOB_BASELINE=ON build"
for b in "$OPT" "$BASE"; do
  [ -x "$b/apps/lob_replay" ] || die "missing $b/apps/lob_replay"
done
GEN=$OPT/testkit/lob_gen_commands
[ -x "$GEN" ] || die "missing $GEN"

# Inputs.
mkdir -p "$W/in"
for f in "$ROOT"/tests/golden/*.cmd; do cp "$f" "$W/in/golden_$(basename "$f")"; done
for p in $("$OPT/testkit/lob_soak" --list | awk '{print $1}'); do
  "$GEN" --profile "$p" --seed 1 --commands "$N" > "$W/in/gen_${p}_s1_n${N}.cmd" || die "generator failed for $p"
done

sha() { sha256sum "$1" | cut -c1-16; }
events_sha() { grep -vE '^(LOB-JOURNAL|CONFIG|BATCH|COMMIT) ' "$1" | sha256sum | cut -c1-16; }

# run BUILD INPUT TAG: sets R_EVENTS R_JOURNAL R_STATE R_SNAPSHOT (all must be
# non-empty). R_STATE is the state_hash of the FINAL "[run]" line — `run` also
# prints a "[recover]" line with the (empty) starting state, which must not be
# taken. The journal is then recovered on its own and must reach the same hash.
# lob_replay writes these status lines to STDERR (stdout carries only --echo).
run() {
  local j="$W/$3.journal" s="$W/$3.snap" recovered
  rm -f "$j" "$s"
  "$1/apps/lob_replay" run --commands "$2" --journal "$j" --snapshot "$s" > /dev/null 2> "$W/$3.run" \
    || die "lob_replay run failed ($3): $(cat "$W/$3.run")"
  R_STATE=$(sed -n 's/^\[run\] .* state_hash=\([0-9a-f]*\).*/\1/p' "$W/$3.run")
  "$1/apps/lob_replay" recover --journal "$j" > /dev/null 2> "$W/$3.recover" \
    || die "lob_replay recover failed ($3): $(cat "$W/$3.recover")"
  recovered=$(sed -n 's/^\[recover\] .* state_hash=\([0-9a-f]*\).*/\1/p' "$W/$3.recover")
  [ "$recovered" = "$R_STATE" ] || die "$3: recovering the journal gives state_hash '$recovered', the run ended with '$R_STATE'"
  [ -s "$j" ] && [ -s "$s" ] || die "$3: journal or snapshot missing"
  R_EVENTS=$(events_sha "$j"); R_JOURNAL=$(sha "$j"); R_SNAPSHOT=$(sha "$s")
  for v in "$R_EVENTS" "$R_JOURNAL" "$R_STATE" "$R_SNAPSHOT"; do
    [ ${#v} -eq 16 ] || die "$3: empty or malformed hash '$v'"
  done
}

rows=""; all_equal=1; count=0
for f in "$W"/in/*.cmd; do
  name=$(basename "$f" .cmd)
  cmds=$(grep -cE '^(NEW|CANCEL) ' "$f")
  run "$OPT" "$f" "opt_$name"; oe=$R_EVENTS; oj=$R_JOURNAL; oh=$R_STATE; os=$R_SNAPSHOT
  run "$BASE" "$f" "base_$name"; be=$R_EVENTS; bj=$R_JOURNAL; bh=$R_STATE; bs=$R_SNAPSHOT
  if [ "$oe" = "$be" ] && [ "$oj" = "$bj" ] && [ "$oh" = "$bh" ] && [ "$os" = "$bs" ]; then
    verdict="一致"
  else
    verdict="**不一致**"; all_equal=0
  fi
  count=$((count + 1))
  rows+="| \`$name\` | $cmds | \`$(sha "$f")\` | \`$oe\` / \`$be\` | \`$oj\` / \`$bj\` | \`$oh\` / \`$bh\` | \`$os\` / \`$bs\` | $verdict |"$'\n'
done

{
  echo "# L2 等价性：优化实现 vs 基线实现"
  echo
  echo "由 \`scripts/l2_equivalence.sh\` 生成，不要手工编辑表格；重新生成："
  echo
  echo '```bash'
  echo "scripts/l2_equivalence.sh $OPT $BASE $OUT $N"
  echo '```'
  echo
  echo "## 条件"
  echo
  echo "- 生成时间：$(date -u +%Y-%m-%dT%H:%M:%SZ)；源码：\`$(git -C "$ROOT" rev-parse --short HEAD)\`$(git -C "$ROOT" diff --quiet HEAD -- src include apps testkit || echo '（工作区有未提交改动）')"
  echo "- 优化构建：\`$OPT\`（\`LOB_BASELINE=OFF\`，编译 \`src/io/session.cpp\`）"
  echo "- 基线构建：\`$BASE\`（\`LOB_BASELINE=ON\`，编译 \`src/io/baseline/session.cpp\` = 标签 \`m6-baseline\` 的 \`src/io/session.cpp\`）"
  echo "- 编译器：$("$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$OPT/CMakeCache.txt")" --version | head -1) / $("$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$BASE/CMakeCache.txt")" --version | head -1)；构建类型：$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$OPT/CMakeCache.txt") / $(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$BASE/CMakeCache.txt")"
  echo "- 运行方式：\`lob_replay run --commands 输入 --journal J --snapshot S\`（非持久模式），两个构建使用同一份输入文件"
  echo "- 输入：\`tests/golden/*.cmd\`（手写场景与固定种子场景），以及每个 testkit 画像一份生成的命令文件（种子 1，$N 条）"
  echo
  echo "## 比较的内容"
  echo
  echo "- **事件流哈希**：日志中事件行（ACCEPTED / REJECTED / TRADE / RESTED / CANCELLED）的 SHA-256，前 16 位十六进制；"
  echo "- **日志哈希**：整个日志文件的 SHA-256（事件 + 批次框架 + 每批 FNV-1a 64 校验），前 16 位；"
  echo "- **快照哈希**：\`lob_replay run\` 结束行 \`[run]\` 中的 \`state_hash\`（规范快照文本的 FNV-1a 64），以及快照文件的 SHA-256 前 16 位。"
  echo "- 每个构建的日志还被单独 \`lob_replay recover\` 一次，恢复出的 \`state_hash\` 必须等于运行结束时的值；任何哈希为空或运行失败，脚本直接报错退出。"
  echo
  echo "## 结果"
  echo
  echo "| 输入 | 命令数 | 输入 SHA-256 | 事件流 优化 / 基线 | 日志 优化 / 基线 | state_hash 优化 / 基线 | 快照文件 优化 / 基线 | 结论 |"
  echo "|---|---|---|---|---|---|---|---|"
  printf '%s' "$rows"
  echo
  if [ $all_equal -eq 1 ]; then
    echo "**结论：$count 个输入全部一致**——两种实现产生逐字节相同的事件流、日志与快照。"
  else
    echo "**结论：存在不一致的输入**（见上表）。"
  fi
  echo
  echo "## 脚本自检（负对照）"
  echo
  echo "用故意引入单个变异的基线构建运行本脚本，脚本都没有报告等价（\`MODULE_NOTES/evidence/L2/equivalence_negative_control.txt\`）："
  echo "M2 \"新单入队首\"与 M3 \"成交价取 taker 限价\"被恢复自检拒绝（退出码 2）；M3 \"未知撤单报 INVALID_ORDER_ID\"产生自洽日志，"
  echo "由比较表检出（16 个输入中 13 个不一致，退出码 1；一致的 3 个恰好不含未知 ID 的撤单）。"
  echo
  echo "## 适用范围"
  echo
  echo "- 只覆盖上述输入与非持久模式；持久模式（\`--durable\`）只改变是否逐批 \`fsync\`，不改变写入的字节，未单独比较。"
  echo "- 不覆盖分配失败下的行为：两种实现在内存不足时的分配点不同（M6 §7），其等价性由各自通过同一套分配注入测试保证，而不是由本脚本比较。"
} > "$OUT"

rm -rf "$W"
echo "l2_equivalence: $count inputs, all_equal=$all_equal -> $OUT"
[ $all_equal -eq 1 ]

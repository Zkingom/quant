#!/usr/bin/env bash
# CLI golden test: lob_replay run on every tests/golden/*.cmd must write the
# committed .journal and .snap byte for byte; verify must pass; running the
# same command file again must commit nothing (all duplicates) and leave the
# journal unchanged.
# Usage: golden_cli.sh LOB_REPLAY GOLDEN_DIR WORK_DIR
set -u
REPLAY=$1; GOLDEN=$2; WORK=$3
rm -rf "$WORK"; mkdir -p "$WORK"
fail=0
for cmd in "$GOLDEN"/*.cmd; do
  name=$(basename "$cmd" .cmd)
  j="$WORK/$name.journal"; s="$WORK/$name.snap"
  if ! "$REPLAY" run --commands "$cmd" --journal "$j" --snapshot "$s" 2> "$WORK/$name.run.err"; then
    echo "FAIL $name: run exited non-zero"; cat "$WORK/$name.run.err"; fail=1; continue
  fi
  cmp -s "$j" "$GOLDEN/$name.journal" || { echo "FAIL $name: journal differs"; fail=1; }
  cmp -s "$s" "$GOLDEN/$name.snap" || { echo "FAIL $name: snapshot differs"; fail=1; }
  "$REPLAY" verify --journal "$j" --commands "$cmd" 2> "$WORK/$name.verify.err" \
    || { echo "FAIL $name: verify"; cat "$WORK/$name.verify.err"; fail=1; }
  cp "$j" "$j.before"
  "$REPLAY" run --commands "$cmd" --journal "$j" --snapshot "$s" 2> "$WORK/$name.rerun.err" \
    || { echo "FAIL $name: rerun"; fail=1; }
  grep -q " committed=0 " "$WORK/$name.rerun.err" || { echo "FAIL $name: rerun committed commands"; fail=1; }
  cmp -s "$j" "$j.before" || { echo "FAIL $name: rerun changed the journal"; fail=1; }
  [ $fail -eq 0 ] && echo "ok $name"
done
exit $fail

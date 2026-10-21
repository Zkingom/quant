#!/usr/bin/env bash
# Process-crash test (DESIGN.md §6.6). A durable `lob_replay run` with
# periodic snapshots is killed with SIGKILL at random moments, then restarted
# with the same command file, again and again; finally it is allowed to
# finish. Checked after EVERY kill:
#   - recovery succeeds (snapshot + journal, torn tail tolerated);
#   - every command acknowledged before the kill (ACK lines, printed only
#     after fsync) is in the recovered history (durable => no acked loss);
# and at the end:
#   - the journal is byte-identical to an uninterrupted run, and the final
#     snapshot is identical too (no command executed twice, none lost).
# This tests process crashes on the file system holding WORK_DIR; it does
# not simulate power loss.
# Usage: crash_restart.sh LOB_REPLAY LOB_GEN_COMMANDS WORK_DIR [KILLS]
set -u
REPLAY=$1; GEN=$2; WORK=$3; KILLS=${4:-12}
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"
"$GEN" --profile mixed --seed 7 --commands 4000 > cmds.txt

"$REPLAY" run --commands cmds.txt --journal ref.journal --snapshot ref.snap --durable 2> ref.err \
  || { echo "reference run failed"; cat ref.err; exit 1; }

RANDOM=4242  # reproducible kill delays (the process timing itself is not)
kills_done=0; max_acked=0; fail=0
for attempt in $(seq 1 "$KILLS"); do
  "$REPLAY" run --commands cmds.txt --journal j.journal --snapshot j.snap --snapshot-every 250 \
      --durable --echo > "out.$attempt" 2> "err.$attempt" &
  pid=$!
  sleep "0.$(( (RANDOM % 9) + 1 ))$(( RANDOM % 10 ))"
  if kill -9 "$pid" 2> /dev/null; then
    kills_done=$((kills_done + 1))
  fi
  wait "$pid" 2> /dev/null
  acked=$(grep -E '^ACK ' "out.$attempt" | tail -1 | awk '{print $2}')
  [ -n "$acked" ] && [ "$acked" -gt "$max_acked" ] && max_acked=$acked
  if [ ! -s j.journal ]; then
    # Killed before anything was written: no history (and no acknowledgement).
    recovered=0; tail_state=EMPTY
  elif ! "$REPLAY" recover --journal j.journal --snapshot j.snap 2> "recover.$attempt"; then
    echo "FAIL: recovery after kill $attempt"; cat "recover.$attempt"; fail=1; break
  else
    recovered=$(sed -n 's/.* last_cmd=\([0-9]*\).*/\1/p' "recover.$attempt")
    tail_state=$(sed -n 's/.* tail=\([A-Z]*\).*/\1/p' "recover.$attempt")
  fi
  echo "kill $attempt: max_acked=$max_acked recovered_last_cmd=$recovered tail=$tail_state"
  if [ "$recovered" -lt "$max_acked" ]; then
    echo "FAIL: acknowledged command $max_acked lost (recovered $recovered)"; fail=1; break
  fi
done
[ $fail -eq 0 ] || exit 1

"$REPLAY" run --commands cmds.txt --journal j.journal --snapshot j.snap --snapshot-every 250 --durable \
  2> final.err || { echo "final run failed"; cat final.err; exit 1; }
cmp -s j.journal ref.journal || { echo "FAIL: journal differs from the uninterrupted run"; exit 1; }
cmp -s j.snap ref.snap || { echo "FAIL: final snapshot differs from the uninterrupted run"; exit 1; }
echo "ok: $kills_done kills (of $KILLS attempts), max acknowledged before a kill=$max_acked;" \
     "final journal and snapshot identical to the uninterrupted run"

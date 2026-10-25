#!/usr/bin/env bash
# L2 step 2: massif at 200k commands (the length of BENCHMARK §12), both builds, mixed and deep_book.
set -u
cd "/mnt/g/学习资料/quant"
OPT=build/l2-optimized; BASE=build/l2-baseline; M=~/l2m
for p in mixed deep_book; do
  build/l2-optimized/testkit/lob_gen_commands --profile $p --seed 1 --commands 200000 > $M/$p.200k.cmd
  for b in opt base; do d=$OPT; [ $b = base ] && d=$BASE
    rm -f $M/j
    /usr/bin/time -o $M/massif200k_${p}_$b.time -f "%e s" valgrind --tool=massif --massif-out-file=$M/massif200k_${p}_$b.out \
      --threshold=0.5 --peak-inaccuracy=0.0 "$d/apps/lob_replay" run --commands $M/$p.200k.cmd --journal $M/j > /dev/null 2> $M/massif200k_${p}_$b.err
    echo "massif $p $b rc=$? $(date -u +%H:%M:%S) $(cat $M/massif200k_${p}_$b.time)"
  done
done
echo ALLDONE

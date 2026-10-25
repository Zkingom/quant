#!/usr/bin/env bash
# L2 step 2, check A: RSS at 50k and massif heap profiles of both builds (deep_book, seed 1). Outputs in ~/l2m.
set -u
cd "/mnt/g/学习资料/quant"
OPT=build/l2-optimized; BASE=build/l2-baseline; M=~/l2m
build/l2-optimized/testkit/lob_gen_commands --profile deep_book --seed 1 --commands 50000 > $M/d50000.cmd
rss() { rm -f $M/j; /usr/bin/time -o $M/t -f "%M" "$1/apps/lob_replay" run --commands "$2" --journal $M/j > /dev/null 2>&1; cat $M/t; }
echo "build,n,run,max_rss_kib" > $M/rss50k.csv
for i in 1 2 3; do for b in opt base; do d=$OPT; [ $b = base ] && d=$BASE
  echo "$b,50000,$i,$(rss $d $M/d50000.cmd)" >> $M/rss50k.csv; done; done
echo "rss50k done $(date -u +%H:%M:%S)"
for b in opt base; do d=$OPT; [ $b = base ] && d=$BASE
  rm -f $M/j
  /usr/bin/time -o $M/massif_$b.time -f "%e s, valgrind max_rss %M KiB" valgrind --tool=massif --massif-out-file=$M/massif_$b.out \
    --threshold=0.5 --peak-inaccuracy=0.0 "$d/apps/lob_replay" run --commands $M/d50000.cmd --journal $M/j > /dev/null 2> $M/massif_$b.err
  echo "massif $b rc=$? $(date -u +%H:%M:%S) $(cat $M/massif_$b.time)"
done
echo ALLDONE

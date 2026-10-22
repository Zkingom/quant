A runs: 5, B runs: 5

| scenario | A throughput (range) | B throughput (range) | B vs A | verdict | A p50/p99/p99.9 ns | B p50/p99/p99.9 ns |
|---|---|---|---|---|---|---|
| engine.mixed | 11.25 M/s (11.08 M/s..11.60 M/s) | 11.23 M/s (10.41 M/s..11.58 M/s) | -0.2% | within noise | 90/425/676 | 88/414/635 |
| engine.mixed[trades=0] | – | – | – | – | 87/219/467 | 85/205/435 |
| engine.mixed[trades=1] | – | – | – | – | 61/275/531 | 62/263/459 |
| engine.mixed[trades=2-4] | – | – | – | – | 175/439/678 | 174/431/598 |
| engine.mixed[trades>=5] | – | – | – | – | 357/726/– | 354/693/– |

A runs: 5, B runs: 5

| scenario | A throughput (range) | B throughput (range) | B vs A | verdict | A p50/p99/p99.9 ns | B p50/p99/p99.9 ns |
|---|---|---|---|---|---|---|
| engine.deep_book | 8.42 M/s (7.35 M/s..8.82 M/s) | 8.26 M/s (8.16 M/s..8.92 M/s) | -2.0% | within noise | 104/1205/3227 | 110/1320/3482 |
| engine.deep_book[trades=0] | – | – | – | – | 111/285/579 | 117/276/475 |
| engine.deep_book[trades=1] | – | – | – | – | 45/242/406 | 50/259/395 |
| engine.deep_book[trades=2-4] | – | – | – | – | 168/479/– | 174/476/– |
| engine.deep_book[trades>=5] | – | – | – | – | 1138/4393/– | 1240/4314/– |

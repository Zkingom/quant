A runs: 5, B runs: 5

| scenario | A throughput (range) | B throughput (range) | B vs A | verdict | A p50/p99/p99.9 ns | B p50/p99/p99.9 ns |
|---|---|---|---|---|---|---|
| book.add_cancel@100000 | 2.03 M/s (841.5 k/s..2.49 M/s) | 1.97 M/s (780.6 k/s..2.26 M/s) | -2.7% | within noise | –/–/– | –/–/– |
| book.add_cancel@100000.add | – | – | – | – | 573/1457/3587 | 590/1510/3828 |
| book.add_cancel@100000.cancel | – | – | – | – | 526/1814/4659 | 573/1918/5345 |

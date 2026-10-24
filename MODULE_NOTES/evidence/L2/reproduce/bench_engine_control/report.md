A runs: 5, B runs: 5

| scenario | A throughput (range) | B throughput (range) | B vs A | verdict | A p50/p99/p99.9 ns | B p50/p99/p99.9 ns |
|---|---|---|---|---|---|---|
| engine.mixed | 11.86 M/s (11.63 M/s..12.75 M/s) | 12.12 M/s (11.00 M/s..12.21 M/s) | +2.2% | within noise | 87/403/614 | 85/398/610 |
| engine.mixed[trades=0] | – | – | – | – | 84/197/423 | 82/196/404 |
| engine.mixed[trades=1] | – | – | – | – | 58/257/420 | 58/253/421 |
| engine.mixed[trades=2-4] | – | – | – | – | 170/417/604 | 167/415/605 |
| engine.mixed[trades>=5] | – | – | – | – | 344/690/– | 339/681/– |

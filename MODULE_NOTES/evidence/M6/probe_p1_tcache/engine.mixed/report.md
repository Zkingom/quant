A runs: 5, B runs: 5

| scenario | A throughput (range) | B throughput (range) | B vs A | verdict | A p50/p99/p99.9 ns | B p50/p99/p99.9 ns |
|---|---|---|---|---|---|---|
| engine.mixed | 11.06 M/s (9.96 M/s..11.58 M/s) | 11.38 M/s (11.08 M/s..12.05 M/s) | +2.9% | within noise | 90/422/645 | 92/415/616 |
| engine.mixed[trades=0] | – | – | – | – | 87/207/405 | 88/199/440 |
| engine.mixed[trades=1] | – | – | – | – | 63/266/429 | 65/269/445 |
| engine.mixed[trades=2-4] | – | – | – | – | 179/437/655 | 177/425/610 |
| engine.mixed[trades>=5] | – | – | – | – | 363/713/– | 358/681/– |

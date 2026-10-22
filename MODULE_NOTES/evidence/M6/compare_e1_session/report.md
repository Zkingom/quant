A runs: 5, B runs: 5

| scenario | A throughput (range) | B throughput (range) | B vs A | verdict | A p50/p99/p99.9 ns | B p50/p99/p99.9 ns |
|---|---|---|---|---|---|---|
| replay.session_memory | 4.5 k/s (3.8 k/s..4.9 k/s) | 1.07 M/s (932.2 k/s..1.11 M/s) | +23664.5% | faster | 152493/1111690/1517430 | 776/2560/10217 |
| session.memory | 4.3 k/s (1.9 k/s..4.8 k/s) | 1.09 M/s (939.9 k/s..1.11 M/s) | +25035.1% | faster | –/–/– | –/–/– |
| session.ext4_buffered | 4.6 k/s (1.9 k/s..4.7 k/s) | 713.3 k/s (611.7 k/s..734.4 k/s) | +15403.2% | faster | 151472/1131140/1626530 | 1169/3326/10301 |
| session.ext4_fsync | 772 /s (590 /s..782 /s) | 786 /s (727 /s..805 /s) | +1.8% | within noise | 1303630/2083020/– | 1217790/1956200/– |

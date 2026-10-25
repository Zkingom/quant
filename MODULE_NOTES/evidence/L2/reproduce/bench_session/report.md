A runs: 5, B runs: 5

| scenario | A throughput (range) | B throughput (range) | B vs A | verdict | A p50/p99/p99.9 ns | B p50/p99/p99.9 ns |
|---|---|---|---|---|---|---|
| replay.session_memory | 4.7 k/s (4.4 k/s..5.2 k/s) | 1.11 M/s (1.10 M/s..1.19 M/s) | +23537.2% | faster | 139913/1161720/1561560 | 750/2481/11811 |
| session.memory | 4.9 k/s (3.9 k/s..5.2 k/s) | 1.13 M/s (1.05 M/s..1.19 M/s) | +22839.1% | faster | –/–/– | –/–/– |
| session.ext4_buffered | 4.9 k/s (4.7 k/s..5.1 k/s) | 700.1 k/s (679.8 k/s..794.2 k/s) | +14193.7% | faster | 140505/1182010/1614210 | 1081/3131/10083 |
| session.ext4_fsync | 768 /s (711 /s..834 /s) | 785 /s (192 /s..804 /s) | +2.3% | within noise | 1290040/2471920/– | 1216560/1990160/– |

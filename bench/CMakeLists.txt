# M6: benchmark harness. Needs the testkit only to GENERATE workloads before
# timing; no production target depends on it.
add_executable(lob_bench
  main.cpp
  bench_book.cpp
  bench_engine.cpp
  bench_io.cpp)
target_link_libraries(lob_bench PRIVATE lob::io lob::testkit lob_project_options)

# Smoke run (tiny sizes, one repetition): keeps the harness compiling and
# running on every preset. Its numbers are meaningless and not reported.
add_test(NAME bench.smoke COMMAND lob_bench --quick)

# L3' stage 2/3: pipeline latency waterfall on a synthetic ITCH stream.
add_executable(lob_itch_bench itch_bench.cpp)
target_link_libraries(lob_itch_bench PRIVATE lob::sim lob::testkit lob_project_options)
add_test(NAME itch_bench.smoke COMMAND lob_itch_bench --quick)

# L3' stage 4: fixed workloads for the cachegrind instruction-count gate.
add_executable(lob_itch_icount itch_icount.cpp)
target_link_libraries(lob_itch_icount PRIVATE lob::sim lob::testkit lob_project_options)

# Real-data validation (D-054): one-pass survey of an ITCH stream on stdin
# (scripts/itch_real_survey.sh; results in MODULE_NOTES/L3.md).
add_executable(lob_itch_survey itch_survey.cpp)
target_link_libraries(lob_itch_survey PRIVATE lob::md lob_project_options)

# L4.6: tick-to-trade (in-process / loopback TCP / round trip), BENCHMARK §15.
# Smoke run keeps it compiling and running; its numbers are not reported.
add_executable(lob_t2t tick_to_trade.cpp)
target_link_libraries(lob_t2t PRIVATE lob::netstage lob::testkit lob_project_options)
add_test(NAME t2t.smoke COMMAND lob_t2t --messages 3000)

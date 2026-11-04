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
target_link_libraries(lob_itch_bench PRIVATE lob::pipeline lob::testkit lob_project_options)
add_test(NAME itch_bench.smoke COMMAND lob_itch_bench --quick)

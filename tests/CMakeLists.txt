find_package(GTest 1.11 QUIET)
if(NOT GTest_FOUND)
  message(STATUS "System GoogleTest not found; fetching v1.14.0")
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
    SYSTEM)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

# One test executable per module: lob_<module>_tests.
function(lob_add_test_suite name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;LABELS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest_main lob_project_options)
  if(ARG_LABELS)
    # Labels select test subsets, e.g. the TSan presets run only "concurrency".
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST PROPERTIES LABELS "${ARG_LABELS}")
  else()
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
  endif()
endfunction()

lob_add_test_suite(lob_core_tests
  SOURCES
    core/test_types.cpp
    core/test_checked.cpp
    core/test_config.cpp
    core/test_validation.cpp
  LIBS lob::core)

lob_add_test_suite(lob_book_tests
  SOURCES
    book/test_order_book.cpp
    book/test_order_book_tamper.cpp
    book/test_order_book_differential.cpp
  LIBS lob::book lob::testkit)

lob_add_test_suite(lob_engine_tests
  SOURCES
    engine/test_matching_engine.cpp
    engine/test_event_auditor.cpp
    engine/test_engine_differential.cpp
  LIBS lob::engine lob::testkit)

lob_add_test_suite(lob_io_tests
  SOURCES
    io/test_text_format.cpp
    io/test_journal.cpp
    io/test_snapshot.cpp
    io/test_recovery.cpp
    io/test_session.cpp
    io/test_golden.cpp
    io/test_file_sink.cpp
  LIBS lob::io lob::testkit)
target_compile_definitions(lob_io_tests PRIVATE LOB_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

# L3' 1a: SPSC queue (semantics, differential vs std::queue, two-thread stress).
lob_add_test_suite(lob_concurrent_tests
  SOURCES
    concurrent/test_spsc_queue.cpp
  LIBS lob::concurrent lob::testkit
  LABELS concurrency)

# L3' md: ITCH decoder, framing, book reconstruction, feed state.
lob_add_test_suite(lob_md_tests
  SOURCES
    md/test_itch_decoder.cpp
    md/test_md_book.cpp
    md/test_feed.cpp
    md/test_md_robustness.cpp
    md/test_trading_phase.cpp
    md/test_resume_grace.cpp
  LIBS lob::md lob::testkit)
target_compile_definitions(lob_md_tests PRIVATE LOB_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

# L3' stage 2: pipeline equivalence, latency recorder, sources, async logger.
lob_add_test_suite(lob_pipeline_tests
  SOURCES
    pipeline/test_pipeline.cpp
    pipeline/test_market.cpp
  LIBS lob::pipeline lob::testkit
  LABELS concurrency)
target_compile_definitions(lob_pipeline_tests PRIVATE LOB_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

# L3' stage 3: risk (one named test per check), strategy, fill models,
# replay == pipeline for orders / fills / positions.
lob_add_test_suite(lob_trading_tests
  SOURCES
    trading/test_risk.cpp
    trading/test_trading.cpp
  LIBS lob::sim lob::testkit
  LABELS concurrency)

# L4.1: OUCH 4.2 subset codec (literals, truncation, fields, mutation fuzz).
lob_add_test_suite(lob_ouch_tests
  SOURCES
    ouch/test_ouch_codec.cpp
  LIBS lob::ouch lob::testkit)
target_compile_definitions(lob_ouch_tests PRIVATE LOB_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

# L4.2: SoupBinTCP framing and session state machines (one named test per transition).
lob_add_test_suite(lob_soup_tests
  SOURCES
    soup/test_soup.cpp
  LIBS lob::soup lob::testkit)

# L4.3: exchange core (sans-IO) and the epoll server over loopback TCP (threads: TSan too).
lob_add_test_suite(lob_net_tests
  SOURCES
    net/test_ouch_exchange.cpp
  LIBS lob::net lob::testkit)
lob_add_test_suite(lob_net_loopback_tests
  SOURCES
    net/test_net_loopback.cpp
  LIBS lob::net lob::testkit
  LABELS concurrency)

# libFuzzer entry (L3' 1e): opt-in, Clang only, not part of CTest. The md
# sources are compiled into the fuzzer directly so that they carry coverage
# instrumentation. Run with scripts/fuzz_itch.sh.
option(LOB_FUZZ "Build the libFuzzer target for the ITCH/feed path (Clang)" OFF)
if(LOB_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "LOB_FUZZ requires Clang")
  endif()
  add_executable(lob_fuzz_itch_feed
    fuzz/fuzz_itch_feed.cpp
    ${PROJECT_SOURCE_DIR}/src/md/itch.cpp
    ${PROJECT_SOURCE_DIR}/src/md/book.cpp
    ${PROJECT_SOURCE_DIR}/src/md/feed.cpp)
  target_include_directories(lob_fuzz_itch_feed PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_options(lob_fuzz_itch_feed PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(lob_fuzz_itch_feed PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(lob_fuzz_itch_feed PRIVATE lob_project_options)

  # L4.1: OUCH codec (both directions; Decoded must re-encode identically).
  add_executable(lob_fuzz_ouch
    fuzz/fuzz_ouch.cpp
    ${PROJECT_SOURCE_DIR}/src/ouch/ouch.cpp)
  target_include_directories(lob_fuzz_ouch PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_options(lob_fuzz_ouch PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(lob_fuzz_ouch PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(lob_fuzz_ouch PRIVATE lob_project_options)
endif()

if(LOB_REPLACE_OPERATOR_NEW)
# Separate executables: lob_testkit_alloc_fault replaces the global
# operator new/delete of the binary it is linked into.
lob_add_test_suite(lob_book_alloc_tests
  SOURCES
    book/test_order_book_alloc_failure.cpp
  LIBS lob::book lob::testkit lob_testkit_alloc_fault)

lob_add_test_suite(lob_engine_alloc_tests
  SOURCES
    engine/test_engine_alloc_failure.cpp
    engine/test_replay_contract.cpp
  LIBS lob::engine lob::testkit lob_testkit_alloc_fault)

lob_add_test_suite(lob_io_alloc_tests
  SOURCES
    io/test_io_faults.cpp
  LIBS lob::io lob::testkit lob_testkit_alloc_fault)

# L3' stage 2: the hot path must not allocate (operator new replaced).
lob_add_test_suite(lob_pipeline_alloc_tests
  SOURCES
    pipeline/test_pipeline_alloc.cpp
  LIBS lob::pipeline lob::testkit lob_testkit_alloc_guard)

# D-058: market-wide mode with pre-created instruments must not allocate.
lob_add_test_suite(lob_market_alloc_tests
  SOURCES
    pipeline/test_market_alloc.cpp
  LIBS lob::pipeline lob::testkit lob_testkit_alloc_guard)

# L3' stage 3: strategy and risk must not allocate.
lob_add_test_suite(lob_trading_alloc_tests
  SOURCES
    trading/test_trading_alloc.cpp
  LIBS lob::strategy lob::risk lob::testkit lob_testkit_alloc_guard)
endif()

# CLI-level tests (Linux shell; they run the real binaries).
add_test(NAME cli.golden
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/golden_cli.sh $<TARGET_FILE:lob_replay>
                 ${CMAKE_CURRENT_SOURCE_DIR}/golden ${CMAKE_CURRENT_BINARY_DIR}/cli_golden)
# Process crashes: SIGKILL at random moments, restart, compare. Runs in /tmp
# (a local Linux file system), not under the source tree.
add_test(NAME cli.crash_restart
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/crash_restart.sh $<TARGET_FILE:lob_replay>
                 $<TARGET_FILE:lob_gen_commands> /tmp/lob_crash_${CMAKE_BUILD_TYPE}_${CMAKE_CXX_COMPILER_ID})

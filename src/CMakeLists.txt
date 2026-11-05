# L3' concurrency primitives (header-only): SPSC ring buffer.
add_library(lob_concurrent INTERFACE)
add_library(lob::concurrent ALIAS lob_concurrent)
target_include_directories(lob_concurrent INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(lob_concurrent INTERFACE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(lob_concurrent INTERFACE Threads::Threads)

# M1 core: types, commands, reject reasons, config, stateless validation.
add_library(lob_core
  core/config.cpp)
add_library(lob::core ALIAS lob_core)
target_include_directories(lob_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(lob_core PUBLIC cxx_std_20)
target_link_libraries(lob_core PRIVATE lob_project_options)

# M2 book: price levels, FIFO queues, order-id index, invariant checking.
add_library(lob_book
  book/order_book.cpp)
add_library(lob::book ALIAS lob_book)
target_link_libraries(lob_book PUBLIC lob::core PRIVATE lob_project_options)

# M3 engine: validation, price-time matching, events.
add_library(lob_engine
  engine/matching_engine.cpp
  engine/snapshot.cpp)
add_library(lob::engine ALIAS lob_engine)
target_link_libraries(lob_engine PUBLIC lob::book PRIVATE lob_project_options)

# M5 io: text formats, journal, snapshot files, recovery, commit session.
# LOB_BASELINE selects the Session implementation: io/session.cpp (optimized,
# M6 E1) or io/baseline/session.cpp, a byte-identical copy of src/io/session.cpp
# at tag m6-baseline (exact reserve of the commit record). Nothing else differs.
if(LOB_BASELINE)
  set(lob_session_source io/baseline/session.cpp)
  message(STATUS "LOB_BASELINE=ON: building the m6-baseline Session implementation")
else()
  set(lob_session_source io/session.cpp)
endif()
add_library(lob_io
  io/text_format.cpp
  io/journal.cpp
  io/snapshot_file.cpp
  io/recovery.cpp
  ${lob_session_source}
  io/verify.cpp
  io/file_sink.cpp)
add_library(lob::io ALIAS lob_io)
target_link_libraries(lob_io PUBLIC lob::engine PRIVATE lob_project_options)
# Visible to everything that links lob::io (tests, bench), so they can tell
# which implementation they run against.
target_compile_definitions(lob_io PUBLIC LOB_BASELINE=$<BOOL:${LOB_BASELINE}>)

# L3' md: ITCH 5.0 subset decoder and framing, book reconstruction, feed
# handler (filter, gap detection, recovery state machine), MoldUDP64 framing.
add_library(lob_md
  md/itch.cpp
  md/book.cpp
  md/feed.cpp
  md/market.cpp)
add_library(lob::md ALIAS lob_md)
target_include_directories(lob_md PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(lob_md PUBLIC cxx_std_20)
target_link_libraries(lob_md PRIVATE lob_project_options)

# L3' pipeline: decoder thread -> SPSC -> book thread, latency recorder,
# chunk sources, asynchronous logger.
add_library(lob_pipeline
  pipeline/pipeline.cpp
  pipeline/async_logger.cpp)
add_library(lob::pipeline ALIAS lob_pipeline)
target_link_libraries(lob_pipeline PUBLIC lob::md lob::concurrent PRIVATE lob_project_options)

# L3' stage 3: strategy interface + market-maker example, pre-trade risk,
# simulated exchange on the (unchanged) L1 matching engine.
add_library(lob_strategy
  strategy/market_maker.cpp
  strategy/virtual_market_maker.cpp)
add_library(lob::strategy ALIAS lob_strategy)
target_link_libraries(lob_strategy PUBLIC lob::md PRIVATE lob_project_options)

add_library(lob_risk
  risk/risk.cpp)
add_library(lob::risk ALIAS lob_risk)
target_link_libraries(lob_risk PUBLIC lob::md PRIVATE lob_project_options)

# L4.1: OUCH 4.2 subset codec (no dependencies).
add_library(lob_ouch
  ouch/ouch.cpp)
add_library(lob::ouch ALIAS lob_ouch)
target_include_directories(lob_ouch PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(lob_ouch PUBLIC cxx_std_20)
target_link_libraries(lob_ouch PRIVATE lob_project_options)

add_library(lob_sim
  sim/sim_exchange.cpp)
add_library(lob::sim ALIAS lob_sim)
target_link_libraries(lob_sim PUBLIC lob::engine lob::md lob::strategy lob::risk lob::pipeline
                      PRIVATE lob_project_options)

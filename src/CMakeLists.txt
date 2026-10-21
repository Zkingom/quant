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
add_library(lob_io
  io/text_format.cpp
  io/journal.cpp
  io/snapshot_file.cpp
  io/recovery.cpp
  io/session.cpp
  io/verify.cpp
  io/file_sink.cpp)
add_library(lob::io ALIAS lob_io)
target_link_libraries(lob_io PUBLIC lob::engine PRIVATE lob_project_options)

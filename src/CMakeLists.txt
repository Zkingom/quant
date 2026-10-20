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

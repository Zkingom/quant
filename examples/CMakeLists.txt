add_executable(core_validation_demo core_validation_demo.cpp)
target_link_libraries(core_validation_demo PRIVATE lob::core lob_project_options)

add_executable(book_demo book_demo.cpp)
target_link_libraries(book_demo PRIVATE lob::book lob_project_options)

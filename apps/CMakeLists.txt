add_executable(lob_replay lob_replay.cpp)
target_link_libraries(lob_replay PRIVATE lob::io lob_project_options)

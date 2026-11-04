add_executable(lob_replay lob_replay.cpp)
target_link_libraries(lob_replay PRIVATE lob::io lob_project_options)

# L3': ITCH file replay for one instrument (streams; bounded memory).
add_executable(lob_itch lob_itch.cpp)
target_link_libraries(lob_itch PRIVATE lob::pipeline lob_project_options)

# Interface target carrying the project's warning policy and debug-only
# library hardening. Linked PRIVATE by every first-party target so that
# third-party code (GoogleTest) is not compiled with these flags.

add_library(lob_project_options INTERFACE)

target_compile_options(lob_project_options INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wconversion
  -Wsign-conversion
  -Wold-style-cast
  -Wcast-qual
  -Wnon-virtual-dtor
  -Woverloaded-virtual
  -Wdouble-promotion
  -Wformat=2
  -Wimplicit-fallthrough
  -Wundef
  $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond -Wduplicated-branches -Wlogical-op>
  $<$<BOOL:${LOB_WARNINGS_AS_ERRORS}>:-Werror>)

# Bounds-checked operator[] / front() / back() etc. in libstdc++ for Debug.
target_compile_definitions(lob_project_options INTERFACE
  $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)

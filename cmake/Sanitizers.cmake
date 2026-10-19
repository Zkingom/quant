# LOB_SANITIZERS is a semicolon-separated list, e.g. "address;undefined".
# Applied globally (including test executables) so that instrumentation
# covers every first-party translation unit. Recovery is disabled: any
# sanitizer report aborts the process and therefore fails the test.

set(LOB_SANITIZERS "" CACHE STRING "Sanitizers to enable, e.g. address;undefined")

if(LOB_SANITIZERS)
  list(JOIN LOB_SANITIZERS "," _lob_sanitizer_list)
  message(STATUS "Sanitizers enabled: ${_lob_sanitizer_list}")
  add_compile_options(
    -fsanitize=${_lob_sanitizer_list}
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${_lob_sanitizer_list})
endif()

# M4 testkit: reusable, GoogleTest-free test infrastructure. Test-only: built
# with LOB_BUILD_TESTS, never linked into production targets.
add_library(lob_testkit STATIC
  src/describe.cpp
  src/event_auditor.cpp
  src/workload.cpp
  src/differential.cpp)
add_library(lob::testkit ALIAS lob_testkit)
target_include_directories(lob_testkit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob_testkit PUBLIC lob::engine PRIVATE lob_project_options)

# Replaces the global operator new/delete of every executable that links it.
add_library(lob_testkit_alloc_fault OBJECT src/alloc_fault.cpp)
target_include_directories(lob_testkit_alloc_fault PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob_testkit_alloc_fault PUBLIC lob::engine PRIVATE lob_project_options)

add_executable(lob_soak tools/soak.cpp)
target_link_libraries(lob_soak PRIVATE lob::testkit lob_testkit_alloc_fault lob_project_options)

# Smoke runs of every profile (short; fault injection on). Long runs are
# executed with scripts/soak.sh and recorded under MODULE_NOTES/evidence/M4.
foreach(profile mixed deep_book single_price high_density thin_book cancel_heavy id_churn
                capacity_edge extreme_quantity extreme_price)
  add_test(NAME soak_smoke.${profile}
           COMMAND lob_soak --profile ${profile} --seeds 1-2 --commands 3000
                    --check-every 100 --fault-per-mille 20)
endforeach()

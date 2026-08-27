set(MOG_SAMPLE_PATH ${CMAKE_SOURCE_DIR}/data/sample.itch)

add_executable(mog-smoke smoke.cpp)
target_link_libraries(mog-smoke PRIVATE mog_flags mog_alloc)

add_executable(mog-golden parser_golden.cpp)
target_compile_definitions(mog-golden PRIVATE MOG_SAMPLE_PATH="${MOG_SAMPLE_PATH}")
target_link_libraries(mog-golden PRIVATE mog_flags)

if(MOG_HAS_EMBED)
  configure_file(${CMAKE_SOURCE_DIR}/cmake/SampleEmbed.hpp.in
    ${CMAKE_BINARY_DIR}/generated/mog/SampleEmbed.hpp @ONLY)
endif()

add_executable(mog-diff parser_diff.cpp)
target_include_directories(mog-diff PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(mog-diff PRIVATE mog_flags)

add_executable(mog-isa isa_dispatch.cpp)
target_include_directories(mog-isa PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(mog-isa PRIVATE mog_flags)

add_executable(mog-book-diff book_diff.cpp)
target_include_directories(mog-book-diff PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(mog-book-diff PRIVATE mog_flags mog_alloc)

add_executable(mog-book-e2e book_e2e.cpp)
target_compile_definitions(mog-book-e2e PRIVATE MOG_SAMPLE_PATH="${MOG_SAMPLE_PATH}")
target_include_directories(mog-book-e2e PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(mog-book-e2e PRIVATE mog_flags mog_alloc)

add_test(NAME smoke COMMAND mog-smoke)
add_test(NAME golden COMMAND mog-golden)
add_test(NAME diff COMMAND mog-diff)
set_tests_properties(diff PROPERTIES ENVIRONMENT "MOG_DIFF_N=200000")
add_test(NAME isa COMMAND mog-isa)
add_test(NAME bookdiff COMMAND mog-book-diff)
set_tests_properties(bookdiff PROPERTIES TIMEOUT 900 ENVIRONMENT "MOG_BOOK_OPS=2000000")
add_test(NAME booke2e COMMAND mog-book-e2e)
set_tests_properties(booke2e PROPERTIES TIMEOUT 300)

add_executable(mog-sched-diff scheduler_diff.cpp)
target_link_libraries(mog-sched-diff PRIVATE mog_flags mog_alloc)
add_test(NAME scheddiff COMMAND mog-sched-diff)
set_tests_properties(scheddiff PROPERTIES TIMEOUT 300)

add_executable(mog-sim-scenarios sim_scenarios.cpp)
target_link_libraries(mog-sim-scenarios PRIVATE mog_flags mog_alloc)
add_test(NAME simscen COMMAND mog-sim-scenarios)
set_tests_properties(simscen PROPERTIES TIMEOUT 120)

add_executable(mog-sim-diff sim_diff.cpp)
target_link_libraries(mog-sim-diff PRIVATE mog_flags mog_alloc)
add_test(NAME simdiff COMMAND mog-sim-diff)
set_tests_properties(simdiff PROPERTIES TIMEOUT 600 ENVIRONMENT "MOG_SIM_OPS=200000")

add_executable(mog-timetravel timetravel_e2e.cpp)
target_link_libraries(mog-timetravel PRIVATE mog_flags mog_alloc)
add_test(NAME timetravel COMMAND mog-timetravel)
set_tests_properties(timetravel PROPERTIES TIMEOUT 120)

add_executable(mog-replay-e2e replay_e2e.cpp)
target_include_directories(mog-replay-e2e PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(mog-replay-e2e PRIVATE mog_flags mog_alloc)
target_compile_definitions(mog-replay-e2e PRIVATE MOG_TEST_SIMRUN=1)
add_test(NAME replayer COMMAND mog-replay-e2e)
set_tests_properties(replayer PROPERTIES TIMEOUT 300)

add_executable(mog-trades-e2e trades_e2e.cpp)
target_include_directories(mog-trades-e2e PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(mog-trades-e2e PRIVATE mog_flags mog_alloc)
add_test(NAME trades COMMAND mog-trades-e2e)
set_tests_properties(trades PROPERTIES TIMEOUT 300)

add_executable(mog-calendar calendar_e2e.cpp)
target_link_libraries(mog-calendar PRIVATE mog_flags)
add_test(NAME calendar COMMAND mog-calendar)
set_tests_properties(calendar PROPERTIES TIMEOUT 60)

add_executable(mog-session session_e2e.cpp)
target_link_libraries(mog-session PRIVATE mog_flags mog_alloc)
add_test(NAME session COMMAND mog-session)
set_tests_properties(session PROPERTIES TIMEOUT 120)

add_executable(mog-simrun simrun_e2e.cpp)
target_link_libraries(mog-simrun PRIVATE mog_flags mog_alloc)
add_test(NAME simrunner COMMAND mog-simrun)
set_tests_properties(simrunner PROPERTIES TIMEOUT 120)

add_executable(mog-g8 g8_e2e.cpp)
target_link_libraries(mog-g8 PRIVATE mog_flags mog_alloc)
add_test(NAME g8 COMMAND mog-g8)
set_tests_properties(g8 PROPERTIES TIMEOUT 120)

add_executable(mog-tearsheet tearsheet_e2e.cpp)
target_link_libraries(mog-tearsheet PRIVATE mog_flags)
add_test(NAME tearsheet COMMAND mog-tearsheet)
set_tests_properties(tearsheet PROPERTIES TIMEOUT 60)

add_executable(mog-orchestrate orchestrate_e2e.cpp)
target_link_libraries(mog-orchestrate PRIVATE mog_flags mog_alloc)
add_test(NAME orchestrate COMMAND mog-orchestrate)
set_tests_properties(orchestrate PROPERTIES TIMEOUT 120)

add_executable(mog-matching-modelcheck matching_modelcheck.cpp)
target_link_libraries(mog-matching-modelcheck PRIVATE mog_flags mog_alloc)
add_test(NAME matchingmodelcheck COMMAND mog-matching-modelcheck)
set_tests_properties(matchingmodelcheck PROPERTIES TIMEOUT 120)

add_executable(mog-fees fees_e2e.cpp)
target_link_libraries(mog-fees PRIVATE mog_flags mog_alloc)
add_test(NAME fees COMMAND mog-fees)
set_tests_properties(fees PROPERTIES TIMEOUT 120)

find_package(Threads REQUIRED)
add_executable(mog-strategy-e2e strategy_e2e.cpp)
target_link_libraries(mog-strategy-e2e PRIVATE mog_flags mog_alloc Threads::Threads)
add_test(NAME strategye2e COMMAND mog-strategy-e2e)
set_tests_properties(strategye2e PROPERTIES TIMEOUT 120)

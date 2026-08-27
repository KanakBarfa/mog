add_executable(mog-gen-sample main.cpp)
target_include_directories(mog-gen-sample PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(mog-gen-sample PRIVATE mog_flags)

# Example binaries; each is self-verifying and registered with CTest so a
# broken example breaks the build gate like any other test.
foreach(example crtp_strategy queue_inspect)
  add_executable(mog-example-${example} ${example}.cpp)
  target_link_libraries(mog-example-${example} PRIVATE mog_flags mog_alloc)
  add_test(NAME example-${example} COMMAND mog-example-${example})
endforeach()

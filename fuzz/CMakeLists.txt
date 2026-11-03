# libFuzzer targets; configure with the fuzz preset (clang, ASan+UBSan).
foreach(target pcap_fuzz packet_fuzz)
  add_executable(${target} ${target}.cpp $<TARGET_OBJECTS:avl_core>)
  target_link_libraries(${target} PRIVATE avl_core avl_warnings)
  target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
  target_link_options(${target} PRIVATE -fsanitize=fuzzer)
endforeach()
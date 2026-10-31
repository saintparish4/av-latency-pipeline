add_executable(avl_dump avl_dump.cpp $<TARGET_OBJECTS:avl_core>)
target_link_libraries(avl_dump PRIVATE avl_core avl_warnings)

foreach(tool roofline_probe kernel_bench udp_rx udp_tx)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${tool}.cpp)
    add_executable(${tool} ${tool}.cpp $<TARGET_OBJECTS:avl_core>)
    target_link_libraries(${tool} PRIVATE avl_core hdr_histogram_static avl_warnings)
  endif()
endforeach()

# Rung 6. Configure with -DAVL_CUDA=ON on a machine with the CUDA toolkit.
# The host-side library (avl_core) is still built by clang; only the .cu file
# goes through nvcc, which is why the top-level flags are C/C++-only.
enable_language(CUDA)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
add_executable(cuda_decode_bench decode_bench.cu $<TARGET_OBJECTS:avl_core>)
target_link_libraries(cuda_decode_bench PRIVATE avl_core)
set_target_properties(cuda_decode_bench PROPERTIES CUDA_ARCHITECTURES native)
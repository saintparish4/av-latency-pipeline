# avl: a deliberately small lidar-packet-to-brake-decision pipeline whose
# purpose is to be measured, not to be feature-complete.
#
# Every subdirectory is optional and added only if it exists, so each phase of
# directive/ can be built and tested before the next phase's files are typed.
# Third-party code is fetched by URL + SHA256 so a stranger gets byte-identical
# dependencies; nothing is taken from the system except the compiler.
cmake_minimum_required(VERSION 3.28)
project(avl LANGUAGES C CXX) 

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # no modules: avoids needing clang-scan-deps

option(AVL_TESTS "Build unit, scene and differential tests" ON)
option(AVL_FUZZ "Build libFuzzer targets (clang only)" OFF)
option(AVL_COZ "Compile Coz progress points into the replay driver" OFF)
option(AVL_CUDA "Build the CUDA decode benchmark (needs nvcc)" OFF)
# Target ISA, pinned so results never depend on -march=native. x86-64-v3 is
# AVX2+FMA; AES and CLMUL are added because Highway's AVX2 target requires
# them and otherwise silently falls back to SSSE3 (directive/p3.md, Gaps).
# On aarch64 use e.g. -DAVL_ISA="-march=armv8.2-a".
set(AVL_ISA "-march=x86-64-v3;-maes;-mpclmul" CACHE STRING "Target ISA flags (list)")
set(AVL_SANITIZE "" CACHE STRING "Sanitizer list passed to -fsanitize= (empty = none)")
set(AVL_LINK_SEED "0" CACHE STRING "Nonzero: shuffle object link order with this seed")

include(cmake/shuffle.cmake)

# Applied to everything, including dependencies: a sanitizer or ISA that only
# covers our code gives false negatives (TSan) or ABI mismatches (march).
# C/C++ only: nvcc (directive/p3.md, rung 6) rejects these spellings.
set(AVL_HOST_LANG "$<COMPILE_LANGUAGE:C,CXX>")
if(AVL_ISA)
  add_compile_options("$<${AVL_HOST_LANG}:${AVL_ISA}>")
endif()
# Frame pointers make perf/Coz call stacks trustworthy for ~1% cost.
# fp-contract=off keeps float results independent of whether the optimizer
# happened to fuse a multiply-add, so every rung can be compared bit for bit.
add_compile_options("$<${AVL_HOST_LANG}:-fno-omit-frame-pointer;-ffp-contract=off>")
if(AVL_SANITIZE)
  add_compile_options(-fsanitize=${AVL_SANITIZE} -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${AVL_SANITIZE})
endif()
# Coverage feedback must come from the code under test, not just the harness.
if(AVL_FUZZ)
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

# SYSTEM: dependency headers are -isystem, so our -Werror never fires on them.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(hdr_histogram
  URL https://github.com/HdrHistogram/HdrHistogram_c/archive/refs/tags/0.11.8.tar.gz
  URL_HASH SHA256=bb95351a6a8b242dc9be1f28562761a84d4cf0a874ffc90a9b630770a6468e94
  SYSTEM)
set(HDR_LOG_REQUIRED OFF CACHE BOOL "" FORCE)
set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(HDR_HISTOGRAM_BUILD_SHARED OFF CACHE BOOL "" FORCE)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  SYSTEM)

FetchContent_Declare(highway
  URL https://github.com/google/highway/archive/refs/tags/1.2.0.tar.gz
  URL_HASH SHA256=7e0be78b8318e8bdbf6fa545d2ecb4c90f947df03f7aadc42c1967f019e63343
  SYSTEM)
set(HWY_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_CONTRIB OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(googletest
  URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  SYSTEM)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(hdr_histogram nlohmann_json highway)
if(AVL_TESTS)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
endif()

# Warnings are ours alone; dependencies are not held to -Werror.
add_library(avl_warnings INTERFACE)
target_compile_options(avl_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)

foreach(dir src bench tools)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt)
    add_subdirectory(${dir})
  endif()
endforeach()
if(AVL_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
  add_subdirectory(tests)
endif()
if(AVL_FUZZ AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/CMakeLists.txt)
  add_subdirectory(fuzz)
endif()
if(AVL_CUDA AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/cuda/CMakeLists.txt)
  add_subdirectory(cuda)
endif()
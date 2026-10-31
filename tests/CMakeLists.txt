# One test binary from every .cpp here, so a phase adds tests by adding files.
# AVL_DATA_DIR points the tests at the pinned captures fetched by
# scripts/fetch_data.sh; tests that need them skip (not fail) if absent.
file(GLOB AVL_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_executable(avl_tests ${AVL_TEST_SOURCES} $<TARGET_OBJECTS:avl_core>)
target_link_libraries(avl_tests PRIVATE avl_core GTest::gtest_main avl_warnings)
target_compile_definitions(avl_tests PRIVATE AVL_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
include(GoogleTest)
gtest_discover_tests(avl_tests DISCOVERY_TIMEOUT 30)
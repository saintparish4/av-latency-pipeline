# avl_core is an OBJECT library so every pipeline' static Registrar is
# linked into every executable (see pipeline.hpp). Sources are globbed so a
# new ladder rung is one new file; CONFIGURE_DEPENDS re-globs on each build.
file(GLOB AVL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
list(SORT AVL_SOURCES)
if(NOT AVL_LINK_SEED STREQUAL "0")
    avl_shuffle(AVL_SOURCES ${AVL_LINK_SEED} ${AVL_SOURCES})
    message(STATUS "avl: link order seed ${AVL_LINK_SEED}")
endif()

add_library(avl_core OBJECT ${AVL_SOURCES})
target_include_directories(avl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(avl_core PUBLIC nlohmann_json::nlohmann_json hwy PRIVATE avl_warnings)
target_compile_definitions(avl_core PUBLIC AVL_LINK_SEED=${AVL_LINK_SEED})
find_package(Threads REQUIRED)
target_link_libraries(avl_core PUBLIC Threads::Threads)
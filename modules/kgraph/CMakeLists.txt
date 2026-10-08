# kgraph: a scene's pass graph, as data and running. Needs the framework.
add_library(kgraph STATIC src/graph.cpp src/catalog.cpp src/runtime.cpp src/entities.cpp)
add_library(KoralEngine::kgraph ALIAS kgraph)
target_include_directories(kgraph PUBLIC include)
target_link_libraries(kgraph PUBLIC kecs Koral::Koral)
target_compile_options(kgraph PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)

if(KORAL_ENGINE_BUILD_TESTS)
    add_executable(kgraph_tests tests/test_graph.cpp tests/test_runtime.cpp tests/test_entities.cpp tests/support.cpp)
    target_link_libraries(kgraph_tests PRIVATE kgraph GTest::gtest GTest::gtest_main)
    target_compile_definitions(kgraph_tests PRIVATE KGRAPH_TEST_SHADERS="${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders")
    gtest_discover_tests(kgraph_tests DISCOVERY_MODE PRE_TEST)
endif()

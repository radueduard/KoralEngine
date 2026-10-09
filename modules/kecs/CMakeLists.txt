# kecs: the engine's ECS. Stands alone — no GPU, no framework — so it is tested without either.
add_library(kecs STATIC src/world.cpp src/transform.cpp src/serialize.cpp src/json.cpp)
add_library(KoralEngine::kecs ALIAS kecs)
target_include_directories(kecs PUBLIC include)
target_compile_options(kecs PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>)

if(KORAL_ENGINE_BUILD_TESTS)
    add_executable(kecs_tests
            tests/test_world.cpp
            tests/test_queries.cpp
            tests/test_commands_hierarchy.cpp
            tests/test_reshape.cpp
            tests/test_serialize.cpp
            tests/test_scale.cpp)
    target_link_libraries(kecs_tests PRIVATE kecs GTest::gtest GTest::gtest_main)
    gtest_discover_tests(kecs_tests DISCOVERY_MODE PRE_TEST)
endif()

# koral-engine: the engine as one shared library, with its C interface and its scenes. What a C# or Kotlin program
# loads beside the framework.
add_library(koral-engine SHARED src/capi.cpp src/scenes.cpp)
add_library(KoralEngine::koral-engine ALIAS koral-engine)
target_include_directories(koral-engine PUBLIC include)
target_compile_definitions(koral-engine PRIVATE KENGINE_BUILD)
target_link_libraries(koral-engine PRIVATE keditor kgraph kscript kecs PUBLIC Koral::Koral)
target_compile_options(koral-engine PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)

if(KORAL_ENGINE_BUILD_TESTS)
    # Only what a program in another language has: the framework, and this library through its C interface.
    add_executable(kengine_tests tests/test_capi.cpp tests/test_shadow.cpp)
    target_link_libraries(kengine_tests PRIVATE koral-engine Koral::Koral GTest::gtest GTest::gtest_main)
    target_compile_definitions(kengine_tests PRIVATE
        KENGINE_LIBRARY="$<TARGET_FILE:koral-engine>"
        KEDITOR_TEST_SHADERS="${CMAKE_SOURCE_DIR}/modules/keditor/tests/shaders"
        KENGINE_SHADOW_SHADERS="${CMAKE_CURRENT_SOURCE_DIR}/tests/shadow/shaders"
        KENGINE_SHADOW_PROJECT="${CMAKE_BINARY_DIR}/shadow-project")
    gtest_discover_tests(kengine_tests DISCOVERY_MODE PRE_TEST)
endif()

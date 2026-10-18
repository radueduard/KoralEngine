# kscript: scripts as the engine sees them: components with functions called on each. Needs kecs alone.
add_library(kscript STATIC src/scripts.cpp src/declarations.cpp)
add_library(KoralEngine::kscript ALIAS kscript)
target_include_directories(kscript PUBLIC include)
target_link_libraries(kscript PUBLIC kecs)
# The engine's own scripts, which every project has: where the engine is built from, unless KORAL_ENGINE_PACKAGE says.
target_compile_definitions(kscript PRIVATE KORAL_ENGINE_PACKAGE="${PROJECT_SOURCE_DIR}/packages/koral")
target_compile_options(kscript PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)

if(KORAL_ENGINE_BUILD_TESTS)
    add_executable(kscript_tests tests/test_scripts.cpp tests/test_declarations.cpp)
    target_link_libraries(kscript_tests PRIVATE kscript GTest::gtest GTest::gtest_main)
    gtest_discover_tests(kscript_tests)
endif()

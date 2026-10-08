# kscript: scripts as the engine sees them: components with functions called on each. Needs kecs alone.
add_library(kscript STATIC src/scripts.cpp)
add_library(KoralEngine::kscript ALIAS kscript)
target_include_directories(kscript PUBLIC include)
target_link_libraries(kscript PUBLIC kecs)
target_compile_options(kscript PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)

if(KORAL_ENGINE_BUILD_TESTS)
    add_executable(kscript_tests tests/test_scripts.cpp)
    target_link_libraries(kscript_tests PRIVATE kscript GTest::gtest GTest::gtest_main)
    gtest_discover_tests(kscript_tests)
endif()

# kasset: every asset of a project, by an ID that stays when its file moves; what is beside each file in its meta.
# Stands alone — no GPU, no framework — so a build's tools can use it, and it is tested without either.
add_library(kasset STATIC src/id.cpp src/meta.cpp src/database.cpp src/mesh.cpp src/cache.cpp src/artifact.cpp src/texture.cpp src/parameters.cpp src/rewrite.cpp src/beside.cpp src/manifest.cpp)
add_library(KoralEngine::kasset ALIAS kasset)
target_include_directories(kasset PUBLIC include)
target_link_libraries(kasset PRIVATE kecs)   # kecs::json, for metas
target_compile_options(kasset PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>)

if(KORAL_ENGINE_BUILD_TESTS)
    add_executable(kasset_tests tests/test_id.cpp tests/test_meta.cpp tests/test_database.cpp tests/test_cache.cpp tests/test_parameters.cpp tests/test_rewrite.cpp tests/test_manifest.cpp)
    target_link_libraries(kasset_tests PRIVATE kasset GTest::gtest GTest::gtest_main)
    gtest_discover_tests(kasset_tests DISCOVERY_MODE PRE_TEST)
endif()

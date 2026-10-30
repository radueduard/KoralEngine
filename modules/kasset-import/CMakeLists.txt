# kasset-import: the importers — a model's, a picture's — as producers whose artifacts kasset keeps. Needs the framework's
# importers, and nothing of the device.
add_library(kasset-import STATIC src/model.cpp src/cook.cpp)
add_library(KoralEngine::kasset-import ALIAS kasset-import)
target_include_directories(kasset-import PUBLIC include)
target_link_libraries(kasset-import PUBLIC kasset PUBLIC Koral::koral-image-import PRIVATE kecs Koral::Koral Koral::koral-model-import Koral::koral-image-compress)
target_compile_options(kasset-import PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)

if(KORAL_ENGINE_BUILD_TESTS)
    add_executable(kasset_import_tests tests/test_model.cpp tests/test_cook.cpp)
    target_link_libraries(kasset_import_tests PRIVATE kasset-import GTest::gtest GTest::gtest_main)
    koral_engine_runtime(kasset_import_tests)
    gtest_discover_tests(kasset_import_tests DISCOVERY_MODE PRE_TEST)
endif()

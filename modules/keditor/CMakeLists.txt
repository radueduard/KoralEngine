# keditor: a scene being worked on, the project it is in, and the editor over them. Needs the framework and its
# interface modules.
add_library(keditor STATIC src/document.cpp src/project.cpp src/play.cpp src/editor.cpp)
add_library(KoralEngine::keditor ALIAS keditor)
target_include_directories(keditor PUBLIC include)
target_link_libraries(keditor PUBLIC kgraph kecs Koral::Koral Koral::koral-ui Koral::koral-gui-extras)
target_compile_options(keditor PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wno-missing-field-initializers>)

if(KORAL_ENGINE_BUILD_TESTS)
    add_executable(keditor_tests tests/test_document.cpp tests/test_editor.cpp ${CMAKE_SOURCE_DIR}/modules/kgraph/tests/support.cpp)
    target_include_directories(keditor_tests PRIVATE ${CMAKE_SOURCE_DIR}/modules/kgraph/tests)
    target_link_libraries(keditor_tests PRIVATE keditor GTest::gtest GTest::gtest_main)
    target_compile_definitions(keditor_tests PRIVATE
        KGRAPH_TEST_SHADERS="${CMAKE_SOURCE_DIR}/modules/kgraph/tests/shaders"
        KEDITOR_TEST_SHADERS="${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders")
    gtest_discover_tests(keditor_tests DISCOVERY_MODE PRE_TEST)
endif()

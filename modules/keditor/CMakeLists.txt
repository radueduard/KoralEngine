# keditor: a scene being worked on, the project it is in, and the editor over them. Needs the framework and its
# interface modules.
# The standard shaders, as text in the library: what a new project's shaders folder is given.
set(KEDITOR_SHADER_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/standard_shaders.h)
set(KEDITOR_SHADER_TEXT "#pragma once\n\nnamespace keditor::detail\n{\n")
foreach(shader engineCommon lit sky outline)
    set(path ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene/${shader}.slang)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${path})
    file(READ ${path} source)
    string(SUBSTRING ${shader} 0 1 first)
    string(TOUPPER ${first} first)
    string(SUBSTRING ${shader} 1 -1 rest)
    string(APPEND KEDITOR_SHADER_TEXT "    inline constexpr const char* ${first}${rest}Slang = R\"koralslang(${source})koralslang\";\n")
endforeach()
string(APPEND KEDITOR_SHADER_TEXT "}\n")
file(GENERATE OUTPUT ${KEDITOR_SHADER_HEADER} CONTENT "${KEDITOR_SHADER_TEXT}")

add_library(keditor STATIC src/document.cpp src/project.cpp src/play.cpp src/editor.cpp src/starter.cpp)
target_include_directories(keditor PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_library(KoralEngine::keditor ALIAS keditor)
target_include_directories(keditor PUBLIC include)
target_link_libraries(keditor PUBLIC kgraph kecs kscript Koral::Koral Koral::koral-ui Koral::koral-gui-extras)
target_compile_options(keditor PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wno-missing-field-initializers>)

if(KORAL_ENGINE_BUILD_TESTS)
    add_executable(keditor_tests tests/test_document.cpp tests/test_editor.cpp ${CMAKE_SOURCE_DIR}/modules/kgraph/tests/support.cpp)
    target_include_directories(keditor_tests PRIVATE ${CMAKE_SOURCE_DIR}/modules/kgraph/tests)
    target_link_libraries(keditor_tests PRIVATE keditor GTest::gtest GTest::gtest_main)
    target_compile_definitions(keditor_tests PRIVATE
        KGRAPH_TEST_SHADERS="${CMAKE_SOURCE_DIR}/modules/kgraph/tests/shaders"
        KEDITOR_TEST_SHADERS="${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders")
    # Freed memory is filled with a pattern, and a test that never ends is stopped: an edit that frees what a frame
    # is still drawing with reads what it freed, and shows as a crash or a wait that never ends rather than as luck.
    gtest_discover_tests(keditor_tests DISCOVERY_MODE PRE_TEST PROPERTIES ENVIRONMENT "MALLOC_PERTURB_=165" TIMEOUT 180)
endif()

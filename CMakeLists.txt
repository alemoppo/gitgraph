# CMakeLists.txt - build di gitgraph.
#
# Fase 1: libreria "core" (util, config, gitdata, history) senza dipendenze
# esterne, strumento di dump testuale e test automatici (ctest).
# La GUI SDL3/SDL3_ttf verra' aggiunta in una fase successiva.
cmake_minimum_required(VERSION 3.16)
project(gitgraph C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- core: nessuna dipendenza da SDL -----------------------------------------
add_library(gg_core STATIC
    src/util.c
    src/config.c
    src/gitdata.c
    src/history.c
)
target_include_directories(gg_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(MSVC)
    target_compile_options(gg_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(gg_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- strumento di ispezione testuale -----------------------------------------
add_executable(gitgraph_dump src/dump_main.c)
target_link_libraries(gitgraph_dump PRIVATE gg_core)

# --- test automatici (senza finestra) ----------------------------------------
enable_testing()

add_executable(test_core tests/test_core.c)
target_link_libraries(test_core PRIVATE gg_core)

# Repository di prova generato prima dei test (fixture ctest). La generazione
# passa da uno script CMake che invoca lo script di piattaforma: ctest non
# lancia mai "cmd /c" direttamente (quoting di cmd poco controllabile).
set(GG_TEST_REPO "${CMAKE_CURRENT_BINARY_DIR}/test_repo"
    CACHE PATH "Repository Git generato per i test")
add_test(NAME make_test_repo
         COMMAND "${CMAKE_COMMAND}"
                 "-DGG_TEST_REPO=${GG_TEST_REPO}"
                 "-DGG_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                 "-P"
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_test_repo_fixture.cmake")
set_tests_properties(make_test_repo PROPERTIES FIXTURES_SETUP testrepo)

add_test(NAME core COMMAND test_core "${GG_TEST_REPO}")
set_tests_properties(core PROPERTIES FIXTURES_REQUIRED testrepo)

add_test(NAME dump_smoke COMMAND gitgraph_dump "${GG_TEST_REPO}")
set_tests_properties(dump_smoke PROPERTIES FIXTURES_REQUIRED testrepo)

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

add_executable(test_scale tests/test_scale.c)
target_link_libraries(test_scale PRIVATE gg_core)

# Generatore di repository sintetici: scrive su stdout uno stream per
# "git fast-import" (nessuna dipendenza dal core, serve solo la libc).
add_executable(scale_repo_gen tests/make_scale_repo.c)

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

# --- test di scala ------------------------------------------------------------
# Due repository sintetici (vedi tests/make_scale_repo.c) con gli obiettivi di
# progetto: ~350 riferimenti / 10.000 commit entro 3 s e ~1.100 riferimenti /
# 50.000 commit entro 30 s. La generazione e' una fixture separata: il budget
# misura solo lettura del repository e costruzione del modello.
set(GG_SCALE_SMALL "${CMAKE_CURRENT_BINARY_DIR}/scale_small"
    CACHE PATH "Repository sintetico piccolo per i test di scala")
set(GG_SCALE_LARGE "${CMAKE_CURRENT_BINARY_DIR}/scale_large"
    CACHE PATH "Repository sintetico grande per i test di scala")

function(gg_add_scale_test name repo commits feature_every sub_every budget timeout sub_index)
    add_test(NAME make_${name}
             COMMAND "${CMAKE_COMMAND}"
                     "-DGG_SCALE_GEN=$<TARGET_FILE:scale_repo_gen>"
                     "-DGG_SCALE_REPO=${repo}"
                     "-DGG_SCALE_COMMITS=${commits}"
                     "-DGG_SCALE_FEATURE=${feature_every}"
                     "-DGG_SCALE_SUB=${sub_every}"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_scale_repo.cmake")
    set_tests_properties(make_${name} PROPERTIES FIXTURES_SETUP ${name}
                                                TIMEOUT ${timeout})
    add_test(NAME ${name} COMMAND test_scale "${repo}" "${budget}" "${sub_index}")
    set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED ${name}
                                         TIMEOUT ${timeout})
endfunction()

gg_add_scale_test(scale_small "${GG_SCALE_SMALL}" 10000 30 5 3 300 4)
gg_add_scale_test(scale_large "${GG_SCALE_LARGE}" 50000 50 5 30 600 4)

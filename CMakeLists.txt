# CMakeLists.txt - build di gitgraph.
#
# Fase 1: libreria "core" (util, config, gitdata, history) senza dipendenze
# esterne, strumento di dump testuale e test automatici (ctest).
# La GUI SDL3/SDL3_ttf verra' aggiunta in una fase successiva.
#
# PIATTAFORMA: solo Windows. src/gitdata.c usa le API Win32 (CreateProcessW,
# pipe, thread) e i test chiamano cmd.exe per generare il repository di prova:
# portare il progetto altrove richiederebbe un altro strato di esecuzione dei
# processi, quindi qui il tentativo di compilare viene fermato subito e con un
# messaggio chiaro invece di errori a cascata.
cmake_minimum_required(VERSION 3.16)
project(gitgraph C)

if(NOT WIN32)
    message(FATAL_ERROR
            "gitgraph e' supportato solo su Windows (usa le API Win32 in "
            "src/gitdata.c e cmd.exe negli script di test).")
endif()

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
    # gcc/clang su Windows (MSYS2 UCRT64 e simili)
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

add_executable(test_config tests/test_config.c)
target_link_libraries(test_config PRIVATE gg_core)

# Generatore di repository sintetici: scrive su stdout uno stream per
# "git fast-import" (nessuna dipendenza dal core, serve solo la libc).
add_executable(scale_repo_gen tests/make_scale_repo.c)

# Finto git.exe per il test di sicurezza: scrive un file sentinella e fallisce.
# Serve a verificare che gitgraph non lo esegua al posto del vero git.
add_executable(fake_git tests/fake_git.c)

# Repository di prova generato prima dei test (fixture ctest). La generazione
# passa da uno script CMake che invoca make_test_repo.bat: ctest non lancia
# mai "cmd /c" direttamente (quoting di cmd poco controllabile).
#
# GG_TEST_ROOT e' la radice autorizzata dei repository di test: e' l'unica
# destinazione in cui lo script puo' creare o cancellare qualcosa, quindi viene
# creata qui e passata allo script. GG_TEST_REPO deve stare dentro questa
# radice e il suo nome finale deve essere uno di quelli ammessi dallo script.
set(GG_TEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/test_repos"
    CACHE PATH "Radice autorizzata dei repository di test")
file(MAKE_DIRECTORY "${GG_TEST_ROOT}")
set(GG_TEST_REPO "${GG_TEST_ROOT}/gitgraph_test_repo"
    CACHE PATH "Repository Git generato per i test")
add_test(NAME make_test_repo
         COMMAND "${CMAKE_COMMAND}"
                 "-DGG_TEST_REPO=${GG_TEST_REPO}"
                 "-DGG_TEST_ROOT=${GG_TEST_ROOT}"
                 "-DGG_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                 "-P"
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_test_repo_fixture.cmake")
set_tests_properties(make_test_repo PROPERTIES FIXTURES_SETUP testrepo)

# Protezioni dello script di generazione: destinazione fuori dalla radice
# autorizzata, prefisso simile alla radice, directory estranea con il marcatore,
# marcatore con testo diverso, file estranei, percorsi con "." e "..", radice del
# filesystem, argomento vuoto, rigenerazione e percorsi con forme diverse
# (separatori, maiuscole, relativi). Tutto avviene in una sandbox sotto la
# build: nessun dato reale viene toccato.
add_test(NAME scripts_guard
         COMMAND "${CMAKE_COMMAND}"
                 "-DGG_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                 "-DGG_GUARD_DIR=${CMAKE_CURRENT_BINARY_DIR}/guard_sandbox"
                 "-P"
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scripts_guard.cmake")
set_tests_properties(scripts_guard PROPERTIES TIMEOUT 900)

# Riceve anche il percorso del finto git.exe, per il test di sicurezza sulla
# ricerca del programma.
add_test(NAME core COMMAND test_core "${GG_TEST_REPO}" "$<TARGET_FILE:fake_git>")
set_tests_properties(core PROPERTIES FIXTURES_REQUIRED testrepo)

# graph.cfg: sintassi, wildcard, validazione e uso end-to-end sul repo di prova
# (il test scrive e rimuove graph.cfg dentro quel repository).
add_test(NAME config COMMAND test_config "${CMAKE_CURRENT_BINARY_DIR}" "${GG_TEST_REPO}")
set_tests_properties(config PROPERTIES FIXTURES_REQUIRED testrepo)

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
set(GG_SCALE_DEEP_REPO "${CMAKE_CURRENT_BINARY_DIR}/scale_deep"
    CACHE PATH "Repository con gerarchia profonda per i test di scala")

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

# Gerarchia profonda: ogni feature nasce dal feature precedente (catena lunga),
# per verificare che il controllo delle relazioni circolari resti lineare.
function(gg_add_scale_deep_test name repo commits feature_every budget timeout)
    add_test(NAME make_${name}
             COMMAND "${CMAKE_COMMAND}"
                     "-DGG_SCALE_GEN=$<TARGET_FILE:scale_repo_gen>"
                     "-DGG_SCALE_REPO=${repo}"
                     "-DGG_SCALE_COMMITS=${commits}"
                     "-DGG_SCALE_FEATURE=${feature_every}"
                     "-DGG_SCALE_SUB=0"
                     "-DGG_SCALE_DEEP=1"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_scale_repo.cmake")
    set_tests_properties(make_${name} PROPERTIES FIXTURES_SETUP ${name}
                                                TIMEOUT ${timeout})
    add_test(NAME ${name} COMMAND test_scale "${repo}" "${budget}" "" 200)
    set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED ${name}
                                         TIMEOUT ${timeout})
endfunction()

gg_add_scale_test(scale_small "${GG_SCALE_SMALL}" 10000 30 5 3 300 4)
gg_add_scale_test(scale_large "${GG_SCALE_LARGE}" 50000 50 5 30 600 4)
gg_add_scale_deep_test(scale_deep "${GG_SCALE_DEEP_REPO}" 20000 20 15 300)

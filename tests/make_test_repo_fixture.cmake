# make_test_repo_fixture.cmake - generazione del repository di prova per ctest.
#
# Eseguito da ctest come:
#   cmake -DGG_TEST_REPO=<dir> -DGG_TEST_ROOT=<cartella> -DGG_BUILD_ROOT=<build>
#         -DGG_SOURCE_DIR=<repo> -P questo-file
#
# La pulizia la fa questa fixture e non lo script: viene rimossa la sola
# cartella dei repository di test dentro la directory di build, poi
# make_test_repo.bat crea il repository (che non cancella mai nulla).
# Passare da uno script CMake evita del tutto le bizzarrie di quoting di
# "cmd /c" quando ctest lancia direttamente un file .bat. Gitgraph e' supportato
# solo su Windows: fuori da qui la generazione non e' prevista.
#
# Prima di cancellare si verifica il percorso: repo e cartella dei test devono
# stare *dentro la build*, sia come scritti sia come percorsi reali (una
# junction puo' portare fuori), e il repo dentro la cartella dei test. Vedi
# tests/cleanup_guard.cmake.

foreach(v GG_TEST_REPO GG_TEST_ROOT GG_BUILD_ROOT GG_SOURCE_DIR)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "${v} non definita")
    endif()
endforeach()

if(NOT WIN32)
    # gitgraph e' supportato solo su Windows (src/gitdata.c usa le API Win32):
    # non c'e' un percorso di generazione del repository di prova altrove.
    message(FATAL_ERROR "gitgraph supporta solo Windows")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cleanup_guard.cmake")

gg_test_dir_is_safe("${GG_TEST_ROOT}" "${GG_BUILD_ROOT}" root_ok root_motivo)
if(NOT root_ok)
    message(FATAL_ERROR
            "GG_TEST_ROOT (${GG_TEST_ROOT}) non e' una cartella di test dentro "
            "la build (${GG_BUILD_ROOT}): ${root_motivo}; niente viene cancellato")
endif()
gg_test_dir_is_safe("${GG_TEST_REPO}" "${GG_BUILD_ROOT}" repo_ok repo_motivo)
if(NOT repo_ok)
    message(FATAL_ERROR
            "GG_TEST_REPO (${GG_TEST_REPO}) non e' una cartella di test dentro "
            "la build (${GG_BUILD_ROOT}): ${repo_motivo}; niente viene cancellato")
endif()
gg_dir_inside("${GG_TEST_ROOT}" "${GG_TEST_REPO}" repo_in_root)
if(NOT repo_in_root)
    message(FATAL_ERROR
            "GG_TEST_REPO (${GG_TEST_REPO}) non e' dentro GG_TEST_ROOT "
            "(${GG_TEST_ROOT}): niente viene cancellato")
endif()

file(REMOVE_RECURSE "${GG_TEST_REPO}")

# cmd non accetta le barre normali in rmdir/mkdir: percorso nativo
file(TO_NATIVE_PATH "${GG_TEST_REPO}" repo_native)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "GG_TEST_REPO=${repo_native}"
            cmd /c "${GG_SOURCE_DIR}/tests/make_test_repo.bat"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR
            "creazione del repository di prova fallita (rc=${rc})\n"
            "output: ${out}\n"
            "errori: ${err}")
endif()
# make_test_repo_fixture.cmake - generazione del repository di prova per ctest.
#
# Eseguito da ctest come:
#   cmake -DGG_TEST_REPO=<dir> -DGG_TEST_ROOT=<cartella> -DGG_SOURCE_DIR=<repo>
#         -P questo-file
#
# La pulizia la fa questa fixture e non lo script: viene rimossa la sola
# cartella dei repository di test dentro la directory di build, poi
# make_test_repo.bat crea il repository (che non cancella mai nulla).
# Passare da uno script CMake evita del tutto le bizzarrie di quoting di
# "cmd /c" quando ctest lancia direttamente un file .bat. Gitgraph e' supportato
# solo su Windows: fuori da qui la generazione non e' prevista.

foreach(v GG_TEST_REPO GG_TEST_ROOT GG_SOURCE_DIR)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "${v} non definita")
    endif()
endforeach()

if(NOT WIN32)
    # gitgraph e' supportato solo su Windows (src/gitdata.c usa le API Win32):
    # non c'e' un percorso di generazione del repository di prova altrove.
    message(FATAL_ERROR "gitgraph supporta solo Windows")
endif()

# la pulizia e' limitata alla cartella dei repository di test: se la cache
# CMake l'ha spostata fuori dalla build, non si cancella nulla
cmake_path(IS_PREFIX GG_TEST_ROOT "${GG_TEST_REPO}" NORMALIZE repo_in_root)
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
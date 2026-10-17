# make_test_repo_fixture.cmake - generazione del repository di prova per ctest.
#
# Eseguito da ctest come:
#   cmake -DGG_TEST_REPO=<dir> -DGG_TEST_ROOT=<radice> -DGG_SOURCE_DIR=<repo>
#         -P questo-file
#
# Lancia tests/make_test_repo.bat passando la directory del repo di prova
# (GG_TEST_REPO) e la radice autorizzata dei repository di test (GG_TEST_ROOT),
# che e' l'unica destinazione in cui lo script puo' creare o cancellare.
# Passare da uno script CMake evita del tutto le bizzarrie di quoting di
# "cmd /c" quando ctest lancia direttamente un file .bat. Gitgraph e' supportato
# solo su Windows: fuori da qui la generazione del repository di prova non e'
# prevista.

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

# il repository di prova deve stare dentro la radice autorizzata: se qualcuno
# ha spostato GG_TEST_REPO altrove, lo script lo rifiuterebbe e conviene
# fermarsi qui con un messaggio chiaro
cmake_path(IS_PREFIX GG_TEST_ROOT "${GG_TEST_REPO}" NORMALIZE is_inside)
if(NOT is_inside)
    message(FATAL_ERROR
            "GG_TEST_REPO (${GG_TEST_REPO}) non e' dentro GG_TEST_ROOT "
            "(${GG_TEST_ROOT}): lo script rifiuterebbe di cancellarla")
endif()

# cmd non accetta le barre normali in rmdir/mkdir: percorso nativo
file(TO_NATIVE_PATH "${GG_TEST_REPO}" repo_native)
file(TO_NATIVE_PATH "${GG_TEST_ROOT}" root_native)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "GG_TEST_REPO=${repo_native}"
            "GG_TEST_ROOT=${root_native}"
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

# make_test_repo_fixture.cmake - generazione del repository di prova per ctest.
#
# Eseguito da ctest come:
#   cmake -DGG_BUILD_ROOT=<build> -DGG_SOURCE_DIR=<repo> -P questo-file
#
# La pulizia la fa questa fixture e non lo script: viene rimosso il solo
# percorso atteso del repository di prova, <build>/test-repos/gitgraph_test_repo,
# scritto qui e ricavato dalla directory di build (non da un parametro), poi
# make_test_repo.bat crea il repository (che non cancella mai nulla). Passare da
# uno script CMake evita del tutto le bizzarrie di quoting di "cmd /c" quando
# ctest lancia direttamente un file .bat. Gitgraph e' supportato solo su
# Windows: fuori da qui la generazione non e' prevista.

foreach(v GG_BUILD_ROOT GG_SOURCE_DIR)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "${v} non definita")
    endif()
endforeach()

if(NOT WIN32)
    # gitgraph e' supportato solo su Windows (src/gitdata.c usa le API Win32):
    # non c'e' un percorso di generazione del repository di prova altrove.
    message(FATAL_ERROR "gitgraph supporta solo Windows")
endif()

# si cancella solo il percorso atteso, scritto qui sotto e dentro la build
set(gg_repo "${GG_BUILD_ROOT}/test-repos/gitgraph_test_repo")
file(REMOVE_RECURSE "${gg_repo}")

# cmd non accetta le barre normali in rmdir/mkdir: percorso nativo
file(TO_NATIVE_PATH "${gg_repo}" repo_native)
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

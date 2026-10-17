# make_test_repo_fixture.cmake - generazione del repository di prova per ctest.
#
# Eseguito da ctest come:
#   cmake -DGG_TEST_REPO=<dir> -DGG_SOURCE_DIR=<repo> -P questo-file
#
# Lancia lo script di piattaforma (tests/make_test_repo.bat su Windows,
# tests/make_test_repo.sh altrove) passando la directory del repo di prova
# tramite la variabile d'ambiente GG_TEST_REPO. Passare da uno script CMake
# evita del tutto le bizzarrie di quoting di "cmd /c" quando ctest lancia
# direttamente un file .bat.

foreach(v GG_TEST_REPO GG_SOURCE_DIR)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "${v} non definita")
    endif()
endforeach()

if(WIN32)
    # cmd non accetta le barre normali in rmdir/mkdir: percorso nativo
    file(TO_NATIVE_PATH "${GG_TEST_REPO}" repo_native)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "GG_TEST_REPO=${repo_native}"
                cmd /c "${GG_SOURCE_DIR}/tests/make_test_repo.bat"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
else()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "GG_TEST_REPO=${GG_TEST_REPO}"
                sh "${GG_SOURCE_DIR}/tests/make_test_repo.sh"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
endif()

if(NOT rc EQUAL 0)
    message(FATAL_ERROR
            "creazione del repository di prova fallita (rc=${rc})\n"
            "output: ${out}\n"
            "errori: ${err}")
endif()

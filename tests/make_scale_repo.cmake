# make_scale_repo.cmake - generazione di un repository sintetico per i test
# di scala, eseguito da ctest come:
#   cmake -DGG_SCALE_GEN=<generatore> -DGG_SCALE_REPO=<dir>
#         -DGG_SCALE_NAME=<nome> -DGG_SCALE_COMMITS=<n>
#         -DGG_SCALE_FEATURE=<ogni-n> -DGG_SCALE_SUB=<ogni-n>
#         [-DGG_SCALE_DEEP=<ogni-n>] -DGG_BUILD_ROOT=<build> -P questo-file
#
# Il generatore (tests/make_scale_repo.c) scrive su stdout uno stream per
# "git fast-import": N commit su main, un branch feature ogni
# GG_SCALE_FEATURE commit, un sotto-branch ogni GG_SCALE_SUB feature (mergiato
# nel feature) e un merge --no-ff in main ogni due feature. Con
# GG_SCALE_DEEP ogni N-esimo feature nasce dal feature precedente: catena di
# branch a profondi' crescente.
#
# Lo stream viene scritto su file e passato a fast-import su stdin: nessuna
# shell. Il repository risultante e' identico a quello creato a mano con gli
# stessi parametri.
#
# GG_SCALE_REPO (percorso atteso sotto <build>/test-repos, scritto in
# CMakeLists.txt) viene verificato prima di ogni scrittura o cancellazione con
# tests/cleanup_guard.cmake: sia il repository sia il file .stream si toccano
# solo se il percorso e' quello atteso e resta dentro la build.

foreach(v GG_SCALE_GEN GG_SCALE_REPO GG_SCALE_NAME GG_SCALE_COMMITS
        GG_SCALE_FEATURE GG_SCALE_SUB GG_BUILD_ROOT)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "${v} non definita")
    endif()
endforeach()
if(NOT DEFINED GG_SCALE_DEEP)
    set(GG_SCALE_DEEP 0)
endif()

# protezione condivisa: si cancellano/scrivono solo i percorsi attesi dentro la
# build, e prima di qualunque operazione distruttiva.
include("${CMAKE_CURRENT_LIST_DIR}/cleanup_guard.cmake")
set(gg_expected_repo "${GG_BUILD_ROOT}/test-repos/${GG_SCALE_NAME}")
gg_cleanup_is_safe("${GG_SCALE_REPO}" "${gg_expected_repo}" "${GG_BUILD_ROOT}"
                   gg_safe gg_motivo)
if(NOT gg_safe)
    message(FATAL_ERROR "pulizia del repository di scala rifiutata: ${gg_motivo}")
endif()

set(stream "${GG_SCALE_REPO}.stream")
gg_cleanup_is_safe("${stream}" "${gg_expected_repo}.stream" "${GG_BUILD_ROOT}"
                   gg_safe gg_motivo)
if(NOT gg_safe)
    message(FATAL_ERROR "scrittura dello stream rifiutata: ${gg_motivo}")
endif()

# --- stream per fast-import ---------------------------------------------------
file(REMOVE "${stream}")
execute_process(
    COMMAND "${GG_SCALE_GEN}" "${GG_SCALE_COMMITS}" "${GG_SCALE_FEATURE}" "${GG_SCALE_SUB}" "${GG_SCALE_DEEP}"
    OUTPUT_FILE "${stream}"
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "generazione dello stream fallita (rc=${rc})")
endif()

# --- repository vuoto ---------------------------------------------------------
file(REMOVE_RECURSE "${GG_SCALE_REPO}")
execute_process(
    COMMAND git init -q -b main "${GG_SCALE_REPO}"
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "git init fallito (rc=${rc})")
endif()

# --- import ------------------------------------------------------------------
# execute_process INPUT_FILE (CMake >= 3.14) passa lo stream su stdin: nessuna
# shell e nessun reindirizzamento da interpretare.
execute_process(
    COMMAND git -C "${GG_SCALE_REPO}" fast-import --quiet
    INPUT_FILE "${stream}"
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "git fast-import fallito (rc=${rc})")
endif()

file(REMOVE "${stream}")

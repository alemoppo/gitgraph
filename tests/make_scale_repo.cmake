# make_scale_repo.cmake - generazione di un repository sintetico per i test
# di scala, eseguito da ctest come:
#   cmake -DGG_SCALE_GEN=<generatore> -DGG_SCALE_REPO=<dir>
#         -DGG_SCALE_COMMITS=<n> -DGG_SCALE_FEATURE=<ogni-n>
#         -DGG_SCALE_SUB=<ogni-n> -DGG_BUILD_ROOT=<build>
#         [-DGG_SCALE_DEEP=<ogni-n>] -P questo-file
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
# GG_SCALE_REPO viene cancellato: prima di ogni cosa si verifica che sia una
# directory di test dentro la build, sia come scritta sia come percorso reale
# (una junction puo' portare fuori). Vedi tests/cleanup_guard.cmake.

foreach(v GG_SCALE_GEN GG_SCALE_REPO GG_SCALE_COMMITS GG_SCALE_FEATURE GG_SCALE_SUB
          GG_BUILD_ROOT)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "${v} non definita")
    endif()
endforeach()
if(NOT DEFINED GG_SCALE_DEEP)
    set(GG_SCALE_DEEP 0)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cleanup_guard.cmake")
gg_test_dir_is_safe("${GG_SCALE_REPO}" "${GG_BUILD_ROOT}" repo_ok repo_motivo)
if(NOT repo_ok)
    message(FATAL_ERROR
            "GG_SCALE_REPO (${GG_SCALE_REPO}) non e' una cartella di test "
            "dentro la build (${GG_BUILD_ROOT}): ${repo_motivo}; niente viene "
            "cancellato")
endif()

set(stream "${GG_SCALE_REPO}.stream")

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

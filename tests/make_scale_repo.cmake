# make_scale_repo.cmake - generazione di un repository sintetico per i test
# di scala, eseguito da ctest come:
#   cmake -DGG_SCALE_GEN=<generatore> -DGG_SCALE_NAME=<nome>
#         -DGG_SCALE_COMMITS=<n> -DGG_SCALE_FEATURE=<ogni-n>
#         -DGG_SCALE_SUB=<ogni-n> [-DGG_SCALE_DEEP=<ogni-n>]
#         -DGG_BUILD_ROOT=<build> -P questo-file
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
# Il repository e il file .stream stanno sotto <build>/test-repos/<nome>, con
# il nome scritto in CMakeLists.txt e passato qui: si cancellano e si scrivono
# solo quei percorsi attesi.

foreach(v GG_SCALE_GEN GG_SCALE_NAME GG_SCALE_COMMITS
        GG_SCALE_FEATURE GG_SCALE_SUB GG_BUILD_ROOT)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "${v} non definita")
    endif()
endforeach()
if(NOT DEFINED GG_SCALE_DEEP)
    set(GG_SCALE_DEEP 0)
endif()

set(gg_repo "${GG_BUILD_ROOT}/test-repos/${GG_SCALE_NAME}")
set(stream "${gg_repo}.stream")

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
file(REMOVE_RECURSE "${gg_repo}")
execute_process(
    COMMAND git init -q -b main "${gg_repo}"
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "git init fallito (rc=${rc})")
endif()

# --- import ------------------------------------------------------------------
# execute_process INPUT_FILE (CMake >= 3.14) passa lo stream su stdin: nessuna
# shell e nessun reindirizzamento da interpretare.
execute_process(
    COMMAND git -C "${gg_repo}" fast-import --quiet
    INPUT_FILE "${stream}"
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "git fast-import fallito (rc=${rc})")
endif()

file(REMOVE "${stream}")

# test_cleanup_guard.cmake - test della protezione della pulizia.
#
# Verifica che tests/cleanup_guard.cmake rifiuti: i percorsi esterni alla
# directory di build, quelli che la scavalcano con "..", quelli con un prefisso
# simile ma non antenato, quelli diversi dal percorso atteso, la radice di un
# drive, i percorsi relativi, quelli vuoti e quelli che attraversano una
# junction (o un symlink). Verifica inoltre che gli script di generazione
# (fixture e scala) rifiutino un GG_*_REPO esterno prima di cancellare o
# scrivere qualsiasi cosa, e che un percorso sicuro si cancelli davvero.
#
# Le directory "esterne" sono finte: stanno dentro la build di questo progetto
# (sotto cleanup_guard_tmp), cosi' un difetto del controllo non puo' cancellare
# dati veri e nessun caso tocca file fuori dalla build. I test negativi non
# eseguono mai una rimozione: provano solo il verdetto, quindi non cancellano
# nulla nemmeno se il controllo fosse sbagliato.
#
# Uso: cmake -DGG_BUILD_ROOT=<build> -DGG_GUARD=<...> -DGG_FIXTURE_SCRIPT=<...>
#            -DGG_SCALE_SCRIPT=<...> -DGG_SOURCE_DIR=<repo>
#            -P tests/test_cleanup_guard.cmake

foreach(v GG_BUILD_ROOT GG_GUARD GG_FIXTURE_SCRIPT GG_SCALE_SCRIPT GG_SOURCE_DIR)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "test_cleanup_guard: ${v} non definita")
    endif()
endforeach()
if(NOT WIN32)
    message(FATAL_ERROR "test_cleanup_guard: solo Windows")
endif()

include("${GG_GUARD}")

# toglie una junction con rmdir: cosi' si rimuove il collegamento e non il
# contenuto della destinazione (REMOVE_RECURSE non si usa su un collegamento)
function(gg_unlink p)
    if(EXISTS "${p}" OR IS_SYMLINK "${p}")
        file(TO_NATIVE_PATH "${p}" nat)
        execute_process(COMMAND cmd /c rmdir "${nat}"
                        RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
    endif()
endfunction()

set(gg_sandbox "${GG_BUILD_ROOT}/cleanup_guard_tmp")
set(gg_fake "${gg_sandbox}/fakebuild")
set(gg_ext "${gg_sandbox}/esterno")
set(gg_link "${gg_fake}/test-repos/link")
set(gg_sent "${gg_ext}/sentinella.txt")

# una esecuzione precedente interrotta: si toglie prima la junction, se c'e'
gg_unlink("${gg_link}")
file(REMOVE_RECURSE "${gg_sandbox}")

file(MAKE_DIRECTORY "${gg_fake}/test-repos")
file(MAKE_DIRECTORY "${gg_ext}/repo")
file(WRITE "${gg_sent}" "dato da non cancellare\n")

# percorsi attesi (ricavati dalla build root come negli script veri)
set(gg_expected_fixture "${gg_fake}/test-repos/gitgraph_test_repo")
set(gg_expected_scale "${gg_fake}/test-repos/scale_prova")

# ---- verdetto del controllo --------------------------------------------------

# atteso: TRUE o FALSE
function(gg_atteso path expected build atteso etichetta)
    gg_cleanup_is_safe("${path}" "${expected}" "${build}" ok motivo)
    if(ok STREQUAL atteso)
        message("  ok: ${etichetta} -> ${ok}")
        return()
    endif()
    message(FATAL_ERROR
            "cleanup_guard: ${etichetta}: atteso ${atteso}, trovato ${ok} (${motivo})")
endfunction()

message("cleanup_guard: verdetti")
gg_atteso("${gg_expected_fixture}" "${gg_expected_fixture}" "${gg_fake}"
          TRUE "percorso atteso sotto la build")
gg_atteso("${gg_expected_scale}" "${gg_expected_scale}" "${gg_fake}"
          TRUE "secondo percorso atteso sotto la build")
gg_atteso("${gg_fake}/test-repos/nuovo" "${gg_fake}/test-repos/nuovo"
          "${gg_fake}" TRUE "percorso non ancora esistente sotto la build")
gg_atteso("${gg_fake}" "${gg_fake}" "${gg_fake}" FALSE "la build stessa non si cancella")
gg_atteso("${gg_ext}/repo" "${gg_expected_fixture}" "${gg_fake}"
          FALSE "percorso esterno alla build")
gg_atteso("${gg_expected_fixture}" "${gg_expected_scale}" "${gg_fake}"
          FALSE "percorso diverso da quello atteso")
gg_atteso("${gg_sandbox}/fakebuild_extra/repo" "${gg_expected_fixture}"
          "${gg_fake}" FALSE "prefisso simile ma non antenato")
gg_atteso("${gg_fake}/../esterno/repo" "${gg_fake}/../esterno/repo" "${gg_fake}"
          FALSE "percorso che scavalca la build con ..")
gg_atteso("C:/qualcosa" "C:/qualcosa" "C:/" FALSE "radice di un drive come build")
gg_atteso("test-repos/repo" "test-repos/repo" "${gg_fake}" FALSE "percorso relativo")
gg_atteso("" "${gg_expected_fixture}" "${gg_fake}" FALSE "percorso vuoto")
gg_atteso("${gg_expected_fixture}" "" "${gg_fake}" FALSE "percorso atteso vuoto")
gg_atteso("${gg_expected_fixture}" "${gg_expected_fixture}" "" FALSE "build vuota")

# ---- percorso che attraversa una junction -----------------------------------
#
# fakebuild/test-repos/link e' una junction verso l'esterno: il percorso scritto
# sta sotto la build, quello reale no. Il controllo deve guardare il reale, e
# anche il caso in cui il percorso da cancellare non esiste ancora e la
# junction sta su un suo antenato.
file(TO_NATIVE_PATH "${gg_link}" gg_link_native)
file(TO_NATIVE_PATH "${gg_ext}" gg_ext_native)
execute_process(COMMAND cmd /c mklink /J "${gg_link_native}" "${gg_ext_native}"
                RESULT_VARIABLE gg_jrc OUTPUT_VARIABLE gg_jout ERROR_VARIABLE gg_jerr)
if(gg_jrc EQUAL 0)
    message("cleanup_guard: junction creata (${gg_link} -> ${gg_ext})")
    gg_atteso("${gg_link}" "${gg_link}" "${gg_fake}" FALSE "la junction stessa")
    gg_atteso("${gg_link}/gitgraph_test_repo" "${gg_link}/gitgraph_test_repo"
              "${gg_fake}" FALSE "percorso non esistente dentro una junction")
    # una junction che resta *dentro* la build non e' un problema: il percorso
    # reale resta dentro e il controllo non rifiuta i collegamenti in se'
    set(gg_dentro "${gg_fake}/test-repos/dentro")
    set(gg_link2 "${gg_fake}/test-repos/link2")
    file(MAKE_DIRECTORY "${gg_dentro}")
    file(TO_NATIVE_PATH "${gg_link2}" gg_link2_native)
    file(TO_NATIVE_PATH "${gg_dentro}" gg_dentro_native)
    execute_process(COMMAND cmd /c mklink /J "${gg_link2_native}" "${gg_dentro_native}"
                    RESULT_VARIABLE gg_jrc2 OUTPUT_QUIET ERROR_QUIET)
    if(gg_jrc2 EQUAL 0)
        gg_atteso("${gg_link2}/repo" "${gg_link2}/repo" "${gg_fake}"
                  TRUE "junction interna alla build")
    endif()
else()
    message("cleanup_guard: junction non creabile in questo ambiente "
            "(mklink rc=${gg_jrc}): caso saltato")
    message("  dettaglio: ${gg_jerr}")
endif()

# ---- gli script di generazione rifiutano e non cancellano --------------------

# fixture del repository di prova: GG_TEST_REPO esterno (il percorso atteso e'
# ricavato da GG_BUILD_ROOT, che qui e' la build finta)
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            "-DGG_TEST_REPO=${gg_ext}/repo"
            "-DGG_BUILD_ROOT=${gg_fake}"
            "-DGG_SOURCE_DIR=${GG_SOURCE_DIR}"
            -P "${GG_FIXTURE_SCRIPT}"
    RESULT_VARIABLE gg_rc OUTPUT_VARIABLE gg_out ERROR_VARIABLE gg_err)
if(gg_rc EQUAL 0)
    message(FATAL_ERROR "cleanup_guard: la fixture ha accettato un GG_TEST_REPO esterno")
endif()
if(NOT EXISTS "${gg_sent}")
    message(FATAL_ERROR "cleanup_guard: la fixture ha cancellato fuori dalla build")
endif()
message("  ok: fixture con GG_TEST_REPO esterno -> rifiutata (rc=${gg_rc})")

# script di scala: GG_SCALE_REPO esterno (il generatore non serve: il controllo
# e' prima di ogni scrittura). Non deve comparire nemmeno il file .stream.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            "-DGG_SCALE_GEN=${CMAKE_COMMAND}"
            "-DGG_SCALE_REPO=${gg_ext}/repo"
            "-DGG_SCALE_NAME=scale_prova"
            "-DGG_SCALE_COMMITS=1"
            "-DGG_SCALE_FEATURE=1"
            "-DGG_SCALE_SUB=0"
            "-DGG_BUILD_ROOT=${gg_fake}"
            -P "${GG_SCALE_SCRIPT}"
    RESULT_VARIABLE gg_rc OUTPUT_VARIABLE gg_out ERROR_VARIABLE gg_err)
if(gg_rc EQUAL 0)
    message(FATAL_ERROR
            "cleanup_guard: lo script di scala ha accettato un GG_SCALE_REPO esterno")
endif()
if(NOT EXISTS "${gg_sent}")
    message(FATAL_ERROR "cleanup_guard: lo script di scala ha cancellato fuori dalla build")
endif()
if(EXISTS "${gg_ext}/repo.stream")
    message(FATAL_ERROR
            "cleanup_guard: lo script di scala ha scritto lo stream prima del controllo")
endif()
message("  ok: script di scala con GG_SCALE_REPO esterno -> rifiutato (rc=${gg_rc})")

# ---- il percorso sicuro si cancella davvero ---------------------------------

file(WRITE "${gg_expected_fixture}/dentro.txt" "x\n")
gg_cleanup_is_safe("${gg_expected_fixture}" "${gg_expected_fixture}"
                   "${gg_fake}" ok motivo)
if(NOT ok)
    message(FATAL_ERROR "cleanup_guard: percorso sicuro rifiutato (${motivo})")
endif()
file(REMOVE_RECURSE "${gg_expected_fixture}")
if(EXISTS "${gg_expected_fixture}")
    message(FATAL_ERROR "cleanup_guard: percorso sicuro non rimosso")
endif()
message("  ok: percorso sicuro rimosso")

# ---- pulizia del test --------------------------------------------------------

gg_unlink("${gg_link}")
gg_unlink("${gg_fake}/test-repos/link2")
if(NOT EXISTS "${gg_sent}")
    message(FATAL_ERROR "cleanup_guard: la sentinella e' sparita")
endif()
file(REMOVE_RECURSE "${gg_sandbox}")
if(EXISTS "${gg_sandbox}")
    message(FATAL_ERROR "cleanup_guard: la cartella di prova non e' stata rimossa")
endif()

message("cleanup_guard: tutti i controlli superati")

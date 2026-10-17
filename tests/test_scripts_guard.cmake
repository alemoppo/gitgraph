# test_scripts_guard.cmake - verifica delle protezioni di make_test_repo.bat
#
# Eseguito da ctest come:
#   cmake -DGG_SOURCE_DIR=<radice> -DGG_GUARD_DIR=<sandbox> -P questo-file
#
# Tutto avviene dentro GG_GUARD_DIR (sottodirectory della build): nessun test
# tocca directory reali ne' la directory temporanea di Windows. Per ogni caso
# vengono controllati il codice di uscita dello script, il messaggio di rifiuto
# atteso e, quando il caso deve fallire, che i file sentinella della directory
# siano ancora presenti e inalterati (non basta il codice di uscita).
#
# La radice autorizzata e' GG_GUARD_DIR/test_repos, creata dal test: e' l'unica
# destinazione in cui lo script puo' creare o cancellare.

foreach(v GG_SOURCE_DIR GG_GUARD_DIR)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "${v} non definita")
    endif()
endforeach()

if(NOT WIN32)
    message(FATAL_ERROR "scripts_guard richiede Windows")
endif()

file(TO_NATIVE_PATH "${GG_SOURCE_DIR}" src_native)
set(script "${src_native}\\tests\\make_test_repo.bat")
file(REMOVE_RECURSE "${GG_GUARD_DIR}")
set(root "${GG_GUARD_DIR}/test_repos")
file(MAKE_DIRECTORY "${root}")
file(TO_NATIVE_PATH "${root}" root_native)

# gg_run_script(<0|1> ARGS <argomenti...> [ROOT <radice>] [NO_ROOT]
#                              [GUARD_FILE <file>]
#                              [GUARD_TEXT <testo>] [EXPECT_TEXT <testo>]
#                              [WARN_TEXT <testo>] [CHECK_FILE <file>])
# 0 = lo script deve riuscire, 1 = lo script deve rifiutare.
#   ROOT         radice passata in GG_TEST_ROOT (predefinito: la sandbox)
#   NO_ROOT      GG_TEST_ROOT viene passata vuota, come se non fosse impostata
#   EXPECT_TEXT  con 1: deve comparire fra i messaggi (un rifiuto per il motivo
#                sbagliato non passa)
#   WARN_TEXT    con 1: non deve comparire
#   GUARD_FILE   con 1: file sentinella che deve esistere e restare identico
#   CHECK_FILE   con 0: file che deve esistere dopo l'operazione
function(gg_run_script expect_ok)
    cmake_parse_arguments(R "NO_ROOT" "ROOT;GUARD_FILE;GUARD_TEXT;EXPECT_TEXT;WARN_TEXT;CHECK_FILE" "ARGS" ${ARGN})
    set(root_env "${root_native}")
    if(R_NO_ROOT)
        set(root_env "")
    elseif(DEFINED R_ROOT)
        set(root_env "${R_ROOT}")
    endif()
    set(R_ENV_REPO "")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "GG_TEST_REPO=${R_ENV_REPO}"
                "GG_TEST_ROOT=${root_env}"
                cmd /c "${script}" ${R_ARGS}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    set(messages "${out}${err}")
    if(expect_ok EQUAL 0)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR
                    "lo script doveva riuscire ma e' fallito (rc=${rc})\n"
                    "root: \$\{root_env\}\\nargs: ${R_ARGS}\noutput: ${out}\nerrori: ${err}")
        endif()
        if(R_CHECK_FILE AND NOT EXISTS "${R_CHECK_FILE}")
            message(FATAL_ERROR
                    "dopo il comando manca ${R_CHECK_FILE}\nargs: ${R_ARGS}")
        endif()
    else()
        if(rc EQUAL 0)
            message(FATAL_ERROR
                    "lo script doveva rifiutare il percorso ma e' riuscito\n"
                    "root: \$\{root_env\}\\nargs: ${R_ARGS}")
        endif()
        if(R_EXPECT_TEXT AND NOT messages MATCHES "${R_EXPECT_TEXT}")
            message(FATAL_ERROR
                    "rifiuto senza il messaggio atteso '${R_EXPECT_TEXT}'\n"
                    "root: \$\{root_env\}\\nargs: ${R_ARGS}\noutput: ${out}\nerrori: ${err}")
        endif()
        if(R_WARN_TEXT AND messages MATCHES "${R_WARN_TEXT}")
            message(FATAL_ERROR
                    "rifiuto per il motivo sbagliato ('${R_WARN_TEXT}' presente)\n"
                    "root: \$\{root_env\}\\nargs: ${R_ARGS}\noutput: ${out}")
        endif()
        if(R_GUARD_FILE)
            if(NOT EXISTS "${R_GUARD_FILE}")
                message(FATAL_ERROR
                        "lo script ha rimosso un file da proteggere: ${R_GUARD_FILE}\n"
                        "root: \$\{root_env\}\\nargs: ${R_ARGS}\noutput: ${out}")
            endif()
            file(READ "${R_GUARD_FILE}" content)
            if(NOT content STREQUAL R_GUARD_TEXT)
                message(FATAL_ERROR
                        "il file da proteggere e' stato modificato: ${R_GUARD_FILE}\n"
                        "root: \$\{root_env\}\\nargs: ${R_ARGS}")
            endif()
        endif()
    endif()
endfunction()

# Crea una directory pronta per essere cancellata: marcatore valido e file
# sentinella, restituendo il percorso del sentinella.
function(gg_make_prepared dir)
    file(MAKE_DIRECTORY "${dir}")
    file(WRITE "${dir}/.gitgraph_test_repo" "gitgraph test repository")
    file(WRITE "${dir}/conservami.txt" "dati importanti")
endfunction()

# --- 1. directory valida dentro la radice: creata e rigenerabile ------------
gg_run_script(0 ARGS "${root}/gitgraph_test_repo" CHECK_FILE "${root}/gitgraph_test_repo/.git")
gg_run_script(0 ARGS "${root}/gitgraph_test_repo" CHECK_FILE "${root}/gitgraph_test_repo/.git")
# suffisso di scenario ammesso
gg_run_script(0 ARGS "${root}/gitgraph_test_repo_guard" CHECK_FILE "${root}/gitgraph_test_repo_guard/.git")

# percorsi con forme diverse ma dentro la radice: devono essere accettati
# (separatori "/" e barra finale, maiuscole, percorso relativo alla radice)
gg_run_script(0 ARGS "${root}/gitgraph_test_repo_slash/" CHECK_FILE "${root}/gitgraph_test_repo_slash/.git")
gg_run_script(0 ARGS "${root}/GITGRAPH_TEST_REPO_CASO" CHECK_FILE "${root}/GITGRAPH_TEST_REPO_CASO/.git")

# --- 2. nome valido e marcatore valido, ma FUORI dalla radice: rifiutato ---
set(outside "${GG_GUARD_DIR}/fuori_radice/gitgraph_test_repo")
gg_make_prepared("${outside}")
gg_run_script(1 ARGS "${outside}" GUARD_FILE "${outside}/conservami.txt"
             GUARD_TEXT "dati importanti"
             EXPECT_TEXT "non e' dentro la radice autorizzata")

# --- 2b. nella radice ma non al suo primo livello (sottodirectory) ---------
set(nested "${root}/annidato/gitgraph_test_repo")
gg_make_prepared("${nested}")
gg_run_script(1 ARGS "${nested}" GUARD_FILE "${nested}/conservami.txt"
             GUARD_TEXT "dati importanti"
             EXPECT_TEXT "non e' dentro la radice autorizzata")

# --- 3. marcatore con testo aggiuntivo: rifiutato e intatto ------------------
set(marker_extra "${root}/gitgraph_test_repo_marker_extra")
file(MAKE_DIRECTORY "${marker_extra}")
file(WRITE "${marker_extra}/.gitgraph_test_repo" "gitgraph test repository\nextra\n")
file(WRITE "${marker_extra}/conservami.txt" "dati importanti")
gg_run_script(1 ARGS "${marker_extra}" GUARD_FILE "${marker_extra}/conservami.txt"
             GUARD_TEXT "dati importanti"
             EXPECT_TEXT "non e' una directory di test di gitgraph"
             WARN_TEXT "non e' dentro la radice")

# --- 3b. marcatore con testo prima del previsto: rifiutato -------------------
set(marker_pre "${root}/gitgraph_test_repo_marker_pre")
file(MAKE_DIRECTORY "${marker_pre}")
file(WRITE "${marker_pre}/.gitgraph_test_repo" "questo file gitgraph test repository\n")
file(WRITE "${marker_pre}/conservami.txt" "dati importanti")
gg_run_script(1 ARGS "${marker_pre}" GUARD_FILE "${marker_pre}/conservami.txt"
             GUARD_TEXT "dati importanti"
             EXPECT_TEXT "non e' una directory di test di gitgraph")

# --- 4. marcatore valido ma file estraneo nella directory: rifiutato --------
set(extra_file "${root}/gitgraph_test_repo_con_extra")
file(MAKE_DIRECTORY "${extra_file}")
file(WRITE "${extra_file}/.gitgraph_test_repo" "gitgraph test repository")
file(WRITE "${extra_file}/conservami.txt" "dati importanti")
gg_run_script(1 ARGS "${extra_file}" GUARD_FILE "${extra_file}/conservami.txt"
             GUARD_TEXT "dati importanti"
             EXPECT_TEXT "non e' una directory di test di gitgraph")

# --- 5. uscita dalla radice con "..": rifiutato ------------------------------
gg_run_script(1 ARGS "${root}/gitgraph_test_repo/../fuori_radice/gitgraph_test_repo"
             EXPECT_TEXT "risoluzione"
             GUARD_FILE "${outside}/conservami.txt" GUARD_TEXT "dati importanti")
# tentativo di risalire fino alla radice del filesystem
gg_run_script(1 ARGS "${root}/../../gitgraph_test_repo" EXPECT_TEXT "risoluzione")

# --- 6. directory sorella con prefisso simile alla radice: non e' dentro -----
set(sibling "${GG_GUARD_DIR}/test_repos_extra/gitgraph_test_repo")
gg_make_prepared("${sibling}")
gg_run_script(1 ARGS "${sibling}" GUARD_FILE "${sibling}/conservami.txt"
             GUARD_TEXT "dati importanti"
             EXPECT_TEXT "non e' dentro la radice autorizzata")
# la sorella e' rimasta intatta: nessuna traccia della cancellazione
if(NOT EXISTS "${sibling}/.gitgraph_test_repo")
    message(FATAL_ERROR "la directory sorella e' stata modificata")
endif()

# --- 6b. destinazione che e' una giunzione verso l'esterno: rifiutata -------
# cmd permette di creare una giunzione; la si usa per verificare che un
# collegamento non venga seguito dalla cancellazione ricorsiva. mklink va
# chiamato con percorsi nativi: con le barre in avanti interpreta "/Users" come
# un'opzione.
file(TO_NATIVE_PATH "${root}/gitgraph_test_repo_junction" junction_native)
file(TO_NATIVE_PATH "${GG_GUARD_DIR}/fuori_radice" target_native)
execute_process(COMMAND cmd /c mklink /J "${junction_native}" "${target_native}"
                RESULT_VARIABLE rc_mklink OUTPUT_QUIET ERROR_QUIET)
if(rc_mklink EQUAL 0)
    file(WRITE "${GG_GUARD_DIR}/fuori_radice/conservami.txt" "dati importanti")
    gg_run_script(1 ARGS "${root}/gitgraph_test_repo_junction"
                 EXPECT_TEXT "collegamento")
    if(NOT EXISTS "${GG_GUARD_DIR}/fuori_radice/conservami.txt")
        message(FATAL_ERROR "il contenuto della giunzione e' stato toccato")
    endif()
else()
    message("scripts_guard: mklink non disponibile, caso della giunzione saltato")
endif()

# --- 7. radice non impostata: rifiutato --------------------------------------
gg_run_script(1 NO_ROOT ARGS "${root}/gitgraph_test_repo"
             EXPECT_TEXT "GG_TEST_ROOT")

# --- 8. radice inesistente: rifiutato ----------------------------------------
gg_run_script(1 ROOT "${GG_GUARD_DIR}/radice_inesistente"
             ARGS "${GG_GUARD_DIR}/radice_inesistente/gitgraph_test_repo"
             EXPECT_TEXT "GG_TEST_ROOT")

# --- 9. casi pericolosi gia' coperti: restano rifiutati --------------------
# nome non ammesso, dentro la radice
gg_run_script(1 ARGS "${root}/altro_repo" EXPECT_TEXT "ammessa")
# nome non ammesso e fuori dalla radice: il controllo di contenimento e' il
# primo filtro e rifiuta prima ancora, quindi non si arriva alla whitelist
gg_run_script(1 ARGS "${GG_GUARD_DIR}/repo_del_mio"
             EXPECT_TEXT "non e' dentro la radice")
# percorso con "." finale
gg_run_script(1 ARGS "${root}/gitgraph_test_repo/." EXPECT_TEXT "risoluzione")
# radice del filesystem
string(SUBSTRING "${root_native}" 0 2 drive)
gg_run_script(1 ARGS "${drive}/" EXPECT_TEXT "")
# home utente e directory temporanea
if(DEFINED ENV{USERPROFILE})
    gg_run_script(1 ARGS "$ENV{USERPROFILE}" EXPECT_TEXT "")
endif()

# --- 10. argomento vuoto: si usa GG_TEST_REPO, che deve stare nella radice --
# Il default in %TEMP% non esiste piu': senza percorso e senza GG_TEST_REPO lo
# script si rifiuta, quindi qui non puo' accadere nulla fuori dalla radice.
gg_run_script(1 ARGS "" EXPECT_TEXT "vuoto")

message("protezioni di make_test_repo.bat: tutti i casi attesi")
# test_scripts_guard.cmake - verifica delle protezioni di make_test_repo.bat
#
# Eseguito da ctest come:
#   cmake -DGG_SOURCE_DIR=<radice> -DGG_GUARD_DIR=<sandbox> -P questo-file
#
# Tutto avviene dentro GG_GUARD_DIR (sottodirectory della build): nessun test
# tocca directory reali. Per ogni caso vengono controllati il codice di uscita
# dello script, il messaggio di rifiuto atteso e, quando il caso dovrebbe
# fallire, che i dati "da proteggere" siano ancora al loro posto.

foreach(v GG_SOURCE_DIR GG_GUARD_DIR)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "${v} non definita")
    endif()
endforeach()

if(NOT WIN32)
    message(FATAL_ERROR "testScriptsGuard richiede Windows")
endif()

file(TO_NATIVE_PATH "${GG_SOURCE_DIR}" src_native)
file(TO_NATIVE_PATH "${GG_GUARD_DIR}" guard_native)
set(script "${src_native}\\tests\\make_test_repo.bat")

# gg_run_script(<0|1> ARGS <argomenti...> [ENV_REPO <valore di GG_TEST_REPO>]
#                              [GUARD_FILE <file>] [GUARD_TEXT <testo>]
#                              [EXPECT_TEXT <testo>] [WARN_TEXT <testo>])
# 0 = lo script deve riuscire, 1 = lo script deve rifiutare. EXPECT_TEXT (usato
# con 1) deve comparire fra i messaggi: evita che un test passi per un rifiuto
# sbagliato. ENV_REPO vuoto (predefinito) lascia GG_TEST_REPO impostata a "",
# cosi' l'argomento vuoto non puo' ricadere sul default %TEMP%.
function(gg_run_script expect_ok)
    cmake_parse_arguments(R "" "GUARD_FILE;GUARD_TEXT;EXPECT_TEXT;WARN_TEXT;ENV_REPO" "ARGS" ${ARGN})
    if(NOT DEFINED R_ENV_REPO)
        set(R_ENV_REPO "")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "GG_TEST_REPO=${R_ENV_REPO}"
                cmd /c "${script}" ${R_ARGS}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    set(messages "${out}${err}")
    if(expect_ok EQUAL 0)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR
                    "lo script doveva riuscire ma e' fallito (rc=${rc})\n"
                    "args: ${R_ARGS}\noutput: ${out}\nerrori: ${err}")
        endif()
    else()
        if(rc EQUAL 0)
            message(FATAL_ERROR
                    "lo script doveva rifiutare il percorso ma e' e' riuscito\n"
                    "args: ${R_ARGS}")
        endif()
        if(R_EXPECT_TEXT AND NOT messages MATCHES "${R_EXPECT_TEXT}")
            message(FATAL_ERROR
                    "rifiuto senza il messaggio atteso '${R_EXPECT_TEXT}'\n"
                    "args: ${R_ARGS}\noutput: ${out}\nerrori: ${err}")
        endif()
        if(R_WARN_TEXT AND messages MATCHES "${R_WARN_TEXT}")
            message(FATAL_ERROR
                    "rifiuto per il motivo sbagliato ('${R_WARN_TEXT}' presente)\n"
                    "args: ${R_ARGS}\noutput: ${out}")
        endif()
        if(R_GUARD_FILE)
            if(NOT EXISTS "${R_GUARD_FILE}")
                message(FATAL_ERROR
                        "lo script ha rimosso un file da proteggere: ${R_GUARD_FILE}\n"
                        "args: ${R_ARGS}\noutput: ${out}")
            endif()
            file(READ "${R_GUARD_FILE}" content)
            if(NOT content STREQUAL R_GUARD_TEXT)
                message(FATAL_ERROR
                        "il file da proteggere e' stato modificato: ${R_GUARD_FILE}\n"
                        "args: ${R_ARGS}")
            endif()
        endif()
    endif()
endfunction()

file(REMOVE_RECURSE "${GG_GUARD_DIR}")
file(MAKE_DIRECTORY "${GG_GUARD_DIR}")

# --- 1. nome non ammesso, directory inesistente: rifiutato e non creata ------
gg_run_script(1 ARGS "${guard_native}\\altro_repo"
             EXPECT_TEXT "non e' un directory|non e' una directory di test ammessa"
             WARN_TEXT "ERRORE durante la creazione")
if(EXISTS "${GG_GUARD_DIR}/altro_repo")
    message(FATAL_ERROR "lo script ha creato una directory con nome non ammesso")
endif()

# --- 2. nome ammesso, directory inesistente: creata --------------------------
gg_run_script(0 ARGS "${guard_native}\\gitgraph_test_repo")
if(NOT EXISTS "${GG_GUARD_DIR}/gitgraph_test_repo/.git")
    message(FATAL_ERROR "il repository di prova non e' stato creato")
endif()
# rigenerazione sulla stessa directory: deve riuscire (rimuove e ricrea)
gg_run_script(0 ARGS "${guard_native}\\gitgraph_test_repo")

# --- 3. nome con suffisso di scenario: ammesso -------------------------------
gg_run_script(0 ARGS "${guard_native}\\gitgraph_test_repo_guard")

# --- 4. directory estranea che contiene il marcatore: non viene toccata ------
#     (nome non ammesso: il marcatore, da solo, non basta comunque)
set(foreign "${GG_GUARD_DIR}/repo_del_mio")
file(MAKE_DIRECTORY "${foreign}")
file(WRITE "${foreign}/.gitgraph_test_repo" "gitgraph test repository")
file(WRITE "${foreign}/conservami.txt" "dati importanti")
gg_run_script(1 ARGS "${foreign}" GUARD_FILE "${foreign}/conservami.txt"
             GUARD_TEXT "dati importanti"
             EXPECT_TEXT "directory di test ammessa")

# --- 5. marcatore con contenuto diverso (nome ammesso): non viene toccata ---
set(fake_marker "${GG_GUARD_DIR}/gitgraph_test_repo_marcatore_falso")
file(MAKE_DIRECTORY "${fake_marker}")
file(WRITE "${fake_marker}/.gitgraph_test_repo" "file di qualcun altro")
file(WRITE "${fake_marker}/conservami.txt" "dati importanti")
gg_run_script(1 ARGS "${fake_marker}" GUARD_FILE "${fake_marker}/conservami.txt"
             GUARD_TEXT "dati importanti"
             EXPECT_TEXT "non contiene il testo|non e' una directory di test di gitgraph")

# --- 6. directory di test con file estranei: non viene toccata ---------------
set(with_extra "${GG_GUARD_DIR}/gitgraph_test_repo_con_extra")
file(MAKE_DIRECTORY "${with_extra}")
file(WRITE "${with_extra}/.gitgraph_test_repo" "gitgraph test repository")
file(WRITE "${with_extra}/conservami.txt" "dati importanti")
gg_run_script(1 ARGS "${with_extra}" GUARD_FILE "${with_extra}/conservami.txt"
             GUARD_TEXT "dati importanti"
             EXPECT_TEXT "non e' una directory di test di gitgraph")

# --- 7. percorso con "." : rifiutato, niente cancellato ----------------------
set(dots "${GG_GUARD_DIR}/dots")
file(MAKE_DIRECTORY "${dots}")
file(WRITE "${dots}/conservami.txt" "dati importanti")
gg_run_script(1 ARGS "${guard_native}\\dots\\." GUARD_FILE "${dots}/conservami.txt"
             GUARD_TEXT "dati importanti"
             EXPECT_TEXT "risoluzione")

# --- 8. percorso con ".." : rifiutato, niente cancellato ---------------------
gg_run_script(1 ARGS "${guard_native}\\gitgraph_test_repo\\..\\repo_del_mio"
             GUARD_FILE "${foreign}/conservami.txt" GUARD_TEXT "dati importanti"
             EXPECT_TEXT "risoluzione")

# --- 9. radice del filesystem: rifiutata -------------------------------------
string(SUBSTRING "${guard_native}" 0 2 drive)
gg_run_script(1 ARGS "${drive}/" EXPECT_TEXT "radice")

# --- 10. argomento vuoto: si usa GG_TEST_REPO -------------------------------
# Il default (%TEMP%\gitgraph_test_repo) non viene esercitato di proposito: i
# test restano dentro la sandbox e non toccano la directory temporanea.
set(env_dir "${GG_GUARD_DIR}/gitgraph_test_repo_da_env")
gg_run_script(0 ARGS "" ENV_REPO "${env_dir}")
if(NOT EXISTS "${env_dir}/.git")
    message(FATAL_ERROR "con argomento vuoto lo script avrebbe dovuto usare GG_TEST_REPO")
endif()

message("protezioni di make_test_repo.bat: tutti i casi attesi")
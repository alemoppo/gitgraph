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
#
# Se mklink non e' disponibile i casi che richiedono una giunzione vengono
# dichiarati SALTATI (con messaggio), mai superati.

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
                    "root: ${root_env}\nargs: ${R_ARGS}\noutput: ${out}\nerrori: ${err}")
        endif()
        if(R_CHECK_FILE AND NOT EXISTS "${R_CHECK_FILE}")
            message(FATAL_ERROR
                    "dopo il comando manca ${R_CHECK_FILE}\nargs: ${R_ARGS}")
        endif()
    else()
        if(rc EQUAL 0)
            message(FATAL_ERROR
                    "lo script doveva rifiutare il percorso ma e' riuscito\n"
                    "root: ${root_env}\nargs: ${R_ARGS}")
        endif()
        if(R_EXPECT_TEXT AND NOT messages MATCHES "${R_EXPECT_TEXT}")
            message(FATAL_ERROR
                    "rifiuto senza il messaggio atteso '${R_EXPECT_TEXT}'\n"
                    "root: ${root_env}\nargs: ${R_ARGS}\noutput: ${out}\nerrori: ${err}")
        endif()
        if(R_WARN_TEXT AND messages MATCHES "${R_WARN_TEXT}")
            message(FATAL_ERROR
                    "rifiuto per il motivo sbagliato ('${R_WARN_TEXT}' presente)\n"
                    "root: ${root_env}\nargs: ${R_ARGS}\noutput: ${out}")
        endif()
        if(R_GUARD_FILE)
            if(NOT EXISTS "${R_GUARD_FILE}")
                message(FATAL_ERROR
                        "lo script ha rimosso un file da proteggere: ${R_GUARD_FILE}\n"
                        "root: ${root_env}\nargs: ${R_ARGS}\noutput: ${out}")
            endif()
            file(READ "${R_GUARD_FILE}" content)
            if(NOT content STREQUAL R_GUARD_TEXT)
                message(FATAL_ERROR
                        "il file da proteggere e' stato modificato: ${R_GUARD_FILE}\n"
                        "root: ${root_env}\nargs: ${R_ARGS}")
            endif()
        endif()
    endif()
endfunction()

# Crea una directory pronta per essere cancellata: marcatore valido e file
# sentinella.
function(gg_make_prepared dir)
    file(MAKE_DIRECTORY "${dir}")
    file(WRITE "${dir}/.gitgraph_test_repo" "gitgraph test repository")
    file(WRITE "${dir}/conservami.txt" "dati importanti")
endfunction()

# Crea una giunzione. Se il collegamento non e' disponibile non viene creato
# nulla e il chiamante lo verifica con EXISTS, dichiarando il caso saltato.
# mklink va chiamato con percorsi nativi: con le barre in avanti interpreta
# "/Users" come un'opzione.
function(gg_make_junction link target)
    file(TO_NATIVE_PATH "${link}" link_native)
    file(TO_NATIVE_PATH "${target}" target_native)
    execute_process(COMMAND cmd /c mklink /J "${link_native}" "${target_native}"
                    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
    if(NOT rc EQUAL 0)
        file(REMOVE "${link_native}")
    endif()
endfunction()

# Scrive un marcatore con i byte esatti richiesti: il testo
# "gitgraph test repository" (24 byte) seguito dal terminatore indicato
# ("none", "lf", "crlf", "crlf_lf", "lf_crlf"). I byte sono prodotti da
# PowerShell con WriteAllBytes, perche' file(WRITE) non garantisce la
# dimensione esatta; la dimensione viene comunque verificata, cosi' un caso non
# puo' passare (o fallire) per un motivo sbagliato.
function(gg_write_marker dir terminator expected_size)
    file(MAKE_DIRECTORY "${dir}")
    file(TO_NATIVE_PATH "${dir}/.gitgraph_test_repo" marker_native)
    # codice PowerShell su una sola riga (separatori ";", nessuno switch):
    # dentro una funzione CMake un set() a piu' argomenti unisce con ";", che
    # qui romperebbe la sintassi
    set(ps_code "$t=$env:GG_MARKER_TEXT; $k=$env:GG_MARKER_TERM; $p=$env:GG_MARKER_PATH;")
    string(APPEND ps_code " if($k -eq 'lf'){$t+=[char]10};")
    string(APPEND ps_code " if($k -eq 'crlf'){$t+=[char]13+[char]10};")
    string(APPEND ps_code " if($k -eq 'crlf_lf'){$t+=[char]13+[char]10+[char]10};")
    string(APPEND ps_code " if($k -eq 'lf_crlf'){$t+=[char]10+[char]13+[char]10};")
    string(APPEND ps_code " [IO.File]::WriteAllBytes($p,[Text.Encoding]::ASCII.GetBytes($t))")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "GG_MARKER_TEXT=gitgraph test repository"
                "GG_MARKER_TERM=${terminator}"
                "GG_MARKER_PATH=${marker_native}"
                powershell -NoProfile -ExecutionPolicy Bypass -Command "${ps_code}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR
                "scrittura del marcatore di prova fallita (rc=${rc}): ${err}")
    endif()
    file(SIZE "${dir}/.gitgraph_test_repo" size)
    if(NOT size EQUAL expected_size)
        message(FATAL_ERROR
                "marcatore di prova di ${size} byte invece di ${expected_size}: "
                "il caso non testerebbe nulla")
    endif()
endfunction()

# --- 1. directory valida dentro la radice: creata e rigenerabile ------------
gg_run_script(0 ARGS "${root}/gitgraph_test_repo" CHECK_FILE "${root}/gitgraph_test_repo/.git")
gg_run_script(0 ARGS "${root}/gitgraph_test_repo" CHECK_FILE "${root}/gitgraph_test_repo/.git")
# suffisso di scenario ammesso
gg_run_script(0 ARGS "${root}/gitgraph_test_repo_guard" CHECK_FILE "${root}/gitgraph_test_repo_guard/.git")
# percorsi con forme diverse ma dentro la radice: devono essere accettati
# (separatori "/" e barra finale, maiuscole)
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
gg_write_marker("${marker_extra}" "crlf_lf" 27)
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
gg_run_script(1 ARGS "${root}/../../gitgraph_test_repo" EXPECT_TEXT "risoluzione")

# --- 6. directory sorella con prefisso simile alla radice: non e' dentro -----
set(sibling "${GG_GUARD_DIR}/test_repos_extra/gitgraph_test_repo")
gg_make_prepared("${sibling}")
gg_run_script(1 ARGS "${sibling}" GUARD_FILE "${sibling}/conservami.txt"
             GUARD_TEXT "dati importanti"
             EXPECT_TEXT "non e' dentro la radice autorizzata")
if(NOT EXISTS "${sibling}/.gitgraph_test_repo")
    message(FATAL_ERROR "la directory sorella e' stata modificata")
endif()

# --- 6b. destinazione che e' una giunzione verso l'esterno: rifiutata -------
gg_make_junction("${root}/gitgraph_test_repo_junction" "${GG_GUARD_DIR}/fuori_radice")
if(EXISTS "${root}/gitgraph_test_repo_junction")
    file(WRITE "${GG_GUARD_DIR}/fuori_radice/conservami.txt" "dati importanti")
    gg_run_script(1 ARGS "${root}/gitgraph_test_repo_junction"
                 EXPECT_TEXT "collegamento")
    if(NOT EXISTS "${GG_GUARD_DIR}/fuori_radice/conservami.txt")
        message(FATAL_ERROR "il contenuto della giunzione e' stato toccato")
    endif()
else()
    message("scripts_guard: mklink non disponibile, giunzione come DESTINAZIONE SALTATO")
endif()

# --- 6c. radice autorizzata essa stessa una giunzione verso l'esterno ------
# Il confronto testuale dei percorsi passerebbe (il padre della destinazione e'
# la radice, parola per parola), ma la radice punta fuori: senza il controllo
# sui collegamenti la cancellazione ricorsiva raggiungerebbe contenuti esterni.
set(root_target "${GG_GUARD_DIR}/radice_fisica")
set(dest_in_root "${root_target}/gitgraph_test_repo")
gg_make_prepared("${dest_in_root}")
gg_make_junction("${GG_GUARD_DIR}/radice_giunzione" "${root_target}")
if(EXISTS "${GG_GUARD_DIR}/radice_giunzione")
    gg_run_script(1 ROOT "${GG_GUARD_DIR}/radice_giunzione"
                 ARGS "${GG_GUARD_DIR}/radice_giunzione/gitgraph_test_repo"
                 EXPECT_TEXT "radice autorizzata")
    # la destinazione fisica e i suoi file sono rimasti intatti
    if(NOT EXISTS "${root_target}/gitgraph_test_repo/conservami.txt")
        message(FATAL_ERROR
                "la destinazione fisica sotto la radice-giunzione e' stata toccata")
    endif()
    if(NOT EXISTS "${root_target}/gitgraph_test_repo/.gitgraph_test_repo")
        message(FATAL_ERROR "il marcatore della destinazione fisica e' stato toccato")
    endif()
else()
    message("scripts_guard: mklink non disponibile, giunzione come RADICE SALTATO")
endif()

# --- 6d. marcatore di 27 byte: rifiutato anche se la prima riga coincide ---
# Il byte in piu' viene dopo il terminatore di riga, quindi non cambia la prima
# riga: un controllo che leggesse solo quella la accetterebbe.
set(marker27 "${root}/gitgraph_test_repo_marker_27")
gg_write_marker("${marker27}" "crlf_lf" 27)
file(WRITE "${marker27}/conservami.txt" "dati importanti")
gg_run_script(1 ARGS "${marker27}" GUARD_FILE "${marker27}/conservami.txt"
             GUARD_TEXT "dati importanti"
             EXPECT_TEXT "non e' una directory di test di gitgraph")

# --- 6e. marcatore con terminatore misto (LF poi CRLF): rifiutato -----------
set(marker_mix "${root}/gitgraph_test_repo_marker_mix")
gg_write_marker("${marker_mix}" "lf_crlf" 27)
file(WRITE "${marker_mix}/conservami.txt" "dati importanti")
gg_run_script(1 ARGS "${marker_mix}" GUARD_FILE "${marker_mix}/conservami.txt"
             GUARD_TEXT "dati importanti"
             EXPECT_TEXT "non e' una directory di test di gitgraph")

# --- 6f. marcatori ammessi: 24, 25 e 26 byte vengono accettati -------------
# Ogni caso rigenera la directory, quindi l'accettazione si osserva nella
# ricreazione del repository (.git presente) e nel marcatore riscritto.
foreach(size_tex 24 25 26)
    if(size_tex EQUAL 24)
        set(terminator "none")
    elseif(size_tex EQUAL 25)
        set(terminator "lf")
    else()
        set(terminator "crlf")
    endif()
    set(dir "${root}/gitgraph_test_repo_marker_${size_tex}")
    gg_write_marker("${dir}" "${terminator}" ${size_tex})
    gg_run_script(0 ARGS "${dir}" CHECK_FILE "${dir}/.git")
    # dopo la rigenerazione il marcatore e' quello scritto dallo script (CRLF)
    file(SIZE "${dir}/.gitgraph_test_repo" size_after)
    if(NOT size_after EQUAL 26)
        message(FATAL_ERROR
                "marcatore di ${size_after} byte dopo la rigenerazione (attesi 26)")
    endif()
endforeach()

# --- 7. radice non impostata: rifiutato --------------------------------------
gg_run_script(1 NO_ROOT ARGS "${root}/gitgraph_test_repo"
             EXPECT_TEXT "GG_TEST_ROOT")

# --- 8. radice inesistente: rifiutato ----------------------------------------
gg_run_script(1 ROOT "${GG_GUARD_DIR}/radice_inesistente"
             ARGS "${GG_GUARD_DIR}/radice_inesistente/gitgraph_test_repo"
             EXPECT_TEXT "GG_TEST_ROOT")

# --- 9. casi pericolosi gia' coperti: restano rifiutati --------------------
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
# home utente
if(DEFINED ENV{USERPROFILE})
    gg_run_script(1 ARGS "$ENV{USERPROFILE}" EXPECT_TEXT "")
endif()

# --- 10. argomento vuoto: si usa GG_TEST_REPO, che deve stare nella radice --
# Il default in %TEMP% non esiste piu': senza percorso e senza GG_TEST_REPO lo
# script si rifiuta, quindi qui non puo' accadere nulla fuori dalla radice.
gg_run_script(1 ARGS "" EXPECT_TEXT "vuoto")

message("protezioni di make_test_repo.bat: tutti i casi attesi")
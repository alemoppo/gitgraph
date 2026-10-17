@echo off
rem ---------------------------------------------------------------------------
rem make_test_repo.bat [percorso]
rem
rem Crea un repository Git isolato e ripetibile per i test di gitgraph.
rem Il percorso arriva da: argomento 1, variabile GG_TEST_REPO, oppure
rem %TEMP%\gitgraph_test_repo.
rem
rem SICUREZZA: lo script cancella una directory solo se e' una directory di test
rem dedicata: il nome finale deve essere "gitgraph_test_repo" oppure
rem "gitgraph_test_repo_<scenario>", il percorso non puo' contenere "." o "..",
rem non puo' essere la radice, la directory corrente, la home o quella
rem temporanea e, se la directory esiste, deve contenere il marcatore
rem .gitgraph_test_repo con il testo esatto "gitgraph test repository" e nessun
rem file diverso da quelli creati qui. Altrimenti lo script fallisce senza
rem toccare nulla: il marcatore da solo non autorizza nessuna cancellazione.
rem
rem Contenuto: main(A,B,C), branch1 da B, branch2 da B2, merge --no-ff,
rem fast-forward, branch cancellato dopo il merge, gruppo di alias (piu' branch
rem sullo stesso commit) e un branch figlio ambiguo, gruppo isolato con date
rem come spareggio (oA/oC/oB da R), remote-tracking con punte diverse, branch
rem nato dal secondo genitore di un merge, merge ripetuti, storia ribasata,
rem ramo senza antenati (genealogia non determinabile), branch con slash,
rem piu' riferimenti sullo stesso commit, messaggio con accenti e piu' righe,
rem uno stash (che il grafo non deve includere).
rem ---------------------------------------------------------------------------
setlocal enableextensions
set "REPO=%~1"
if "%REPO%"=="" set "REPO=%GG_TEST_REPO%"
if "%REPO%"=="" set "REPO=%TEMP%\gitgraph_test_repo"
set "MARKER=.gitgraph_test_repo"
set "MARKERTEXT=gitgraph test repository"
set "BASENAME=gitgraph_test_repo"

rem --- SICUREZZA: cosa puo' essere cancellato --------------------------------
rem Lo script cancella una directory solo se tutte queste condizioni valgono:
rem   1. il percorso non e' vuoto e non contiene "." o ".." (nessuna risoluzione
rem      ambigua: i percorsi con puntini vengono rifiutati, non normalizzati);
rem   2. il nome finale e' esattamente %BASENAME% oppure %BASENAME%_<scenario>
rem      (whitelist: nessuna directory arbitraria puo' essere cancellata);
rem   3. il percorso non e' la radice del filesystem, la directory corrente,
rem      la home o la directory temporanea;
rem   4. se la directory esiste, contiene il marcatore %MARKER% con il testo
rem      esatto %MARKERTEXT% e non contiene altri file oltre quelli creati qui
rem      (il marcatore da solo non autorizza nessuna cancellazione).
rem Altrimenti lo script fallisce senza toccare nulla.
if "%REPO%"=="" goto :unsafe_empty
rem ".." in qualsiasi posizione e una "\." finale: percorsi non risolvibili
set "LAST2=%REPO:~-2%"
if "%LAST2%"=="\." goto :unsafe_dots
echo "%REPO%" | findstr /r /c:"\.\." >nul && goto :unsafe_dots
for %%I in ("%REPO%") do set "REPORES=%%~fI"
if "%REPORES%"=="\" goto :unsafe_root
set "DRIVEONLY=%SystemDrive%\"
if /i "%REPORES%"=="%DRIVEONLY%" goto :unsafe_root
rem gli helper leggono REPORES dal%: passare il percorso come argomento di
rem "call" e' fragile quando termina con "\" (rompe il parsing di cmd)
call :check_name || goto :unsafe_name
if /i "%REPORES%"=="%CD%" goto :unsafe_same
if /i "%REPORES%"=="%USERPROFILE%" goto :unsafe_home
if /i "%REPORES%"=="%TEMP%" goto :unsafe_temp
if /i "%REPORES%"=="%TMP%" goto :unsafe_temp
if exist "%REPORES%\" goto :exists
mkdir "%REPORES%" || goto :err
goto :created

:exists
if not exist "%REPORES%\%MARKER%" goto :unsafe_foreign
set "MARKEROK="
for /f "usebackq delims=" %%L in ("%REPORES%\%MARKER%") do if /i "%%L"=="%MARKERTEXT%" set "MARKEROK=1"
if not defined MARKEROK goto :unsafe_foreign
call :check_entries || goto :unsafe_foreign
rmdir /s /q "%REPORES%" || goto :err
mkdir "%REPORES%" || goto :err

:created
rem marcatore: da qui in poi la directory e' riconoscibile come nostra
> "%REPORES%\%MARKER%" echo %MARKERTEXT%
set "REPO=%REPORES%"

set "GIT_AUTHOR_NAME=Test User"
set "GIT_AUTHOR_EMAIL=test@example.com"
set "GIT_COMMITTER_NAME=Test User"
set "GIT_COMMITTER_EMAIL=test@example.com"

git -C "%REPO%" init -q -b main || goto :err
git -C "%REPO%" config commit.gpgsign false || goto :err
git -C "%REPO%" config core.autocrlf false || goto :err

rem --- main: A, B, C ---------------------------------------------------------
echo hello > "%REPO%\hello.txt"
git -C "%REPO%" add hello.txt || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T10:00:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q -m "A: prima revisione" || goto :err

set "GIT_AUTHOR_DATE=2026-01-02T10:02:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "B: seconda revisione" || goto :err

rem C: messaggio con accenti e piu' righe (scritto via PowerShell in UTF-8,
rem sorgente ASCII per non dipendere dalla codepage del .bat)
set "MSGFILE=%REPO%\msg-c.txt"
powershell -NoProfile -Command "$nl=[char]10; $m='terza revisione: '+[char]0xE0+'ccenti e pi'+[char]0xF9+' righe'+$nl+$nl+'corpo del messaggio, con '+[char]0xFC+'n'+[char]0xEF+'code: '+[char]0x2713; [IO.File]::WriteAllText($env:MSGFILE, $m+$nl, (New-Object Text.UTF8Encoding $false))" || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T10:04:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -F "%MSGFILE%" || goto :err
del "%MSGFILE%"

rem piu' riferimenti sullo stesso commit (C)
git -C "%REPO%" tag v1.0-lw || goto :err
set "GIT_COMMITTER_DATE=2026-01-02T11:40:00+0000"
git -C "%REPO%" tag -a v1.0 -m "versione 1.0" || goto :err

rem --- feature/x da C --------------------------------------------------------
git -C "%REPO%" checkout -q -b feature/x main || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T10:06:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "FX1: lavoro su feature/x" || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T10:08:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "FX2: altro lavoro su feature/x" || goto :err

rem --- branch1 da B ----------------------------------------------------------
git -C "%REPO%" checkout -q -b branch1 main~1 || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T10:10:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "B1: inizio su branch1" || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T10:12:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "B2: branch2 nascera' da qui" || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T10:14:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "B3: fine di branch1" || goto :err
git -C "%REPO%" tag b1-final branch1 || goto :err

rem --- branch2 da B2 ---------------------------------------------------------
git -C "%REPO%" checkout -q -b branch2 branch1~1 || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T10:20:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "C1: inizio su branch2" || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T10:22:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "C2: fine di branch2" || goto :err

rem --- caso ambiguo: ambbase (X), amb1 e amb2 su X, ambchild da X -------------
rem X poggia sulla radice A, per non interferire con branch1/branch2
git -C "%REPO%" checkout -q -b ambbase main~2 || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T10:30:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "X: punto di diramazione ambiguo" || goto :err
git -C "%REPO%" checkout -q -b amb1 ambbase || goto :err
git -C "%REPO%" checkout -q -b amb2 ambbase || goto :err
git -C "%REPO%" checkout -q -b ambchild ambbase || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T10:32:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "AC1: da X, padre non univoco" || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T10:34:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "AC2: fine di ambchild" || goto :err

rem --- merge --no-ff di branch1 in main --------------------------------------
git -C "%REPO%" checkout -q main || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T10:40:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" merge -q --no-ff branch1 -m "Merge branch 'branch1' (--no-ff)" || goto :err
rem hash del merge, ricordato per costruire un branch dal secondo genitore
set "M1="
for /f "delims=" %%h in ('git -C "%REPO%" rev-parse HEAD') do set "M1=%%h"
rem basi usate dagli scenari di genealogia
git -C "%REPO%" tag base-b main~2 || goto :err
git -C "%REPO%" tag base-c main~1 || goto :err

rem --- branch temp: merge --no-ff poi cancellazione --------------------------
git -C "%REPO%" checkout -q -b temp main || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T10:50:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "T1: lavoro del branch temp" || goto :err
git -C "%REPO%" checkout -q main || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T11:00:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" merge -q --no-ff temp -m "Merge branch 'temp' (--no-ff)" || goto :err
git -C "%REPO%" branch -D temp >nul || goto :err

rem --- fast-forward: ff con F1, main si allinea, poi D -----------------------
git -C "%REPO%" checkout -q -b ff main || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T11:10:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "F1: commit di ff" || goto :err
git -C "%REPO%" checkout -q main || goto :err
git -C "%REPO%" merge -q --ff-only ff || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T11:20:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "D: tip di main" || goto :err

rem piu' riferimenti sullo stesso commit (D)
git -C "%REPO%" tag tip-lw || goto :err
set "GIT_COMMITTER_DATE=2026-01-02T11:50:00+0000"
git -C "%REPO%" tag -a v2.0 -m "versione 2.0" || goto :err

rem --- gruppo isolato con date come spareggio (oA, oC, oB figli di R) -------
rem R e' la radice del gruppo; oA il piu' vecchio (12:02), oC 12:04, oB 12:06.
rem Serve a verificare lo spareggio per data e i messaggi che lo dichiarano.
git -C "%REPO%" checkout -q --orphan oA || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T12:00:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "R: radice del gruppo isolato" || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T12:02:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "OA: primo ramo isolato" || goto :err
git -C "%REPO%" checkout -q -b oC HEAD~1 || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T12:04:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "OC: secondo ramo isolato" || goto :err
git -C "%REPO%" checkout -q -b oB HEAD~1 || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T12:06:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "OB: terzo ramo isolato" || goto :err
git -C "%REPO%" checkout -q main || goto :err

rem --- genealogia: ramo nato dal secondo genitore di un merge ----------------
rem sp parte da %M1%^2, cioe' dal secondo genitore del merge (B3).
git -C "%REPO%" checkout -q -b sp "%M1%^2" || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T12:20:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "SP1: nato dal secondo genitore del merge" || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T12:22:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "SP2: altro lavoro su sp" || goto :err

rem --- genealogia: merge ripetuti dello stesso branch in main ----------------
git -C "%REPO%" checkout -q main || goto :err
git -C "%REPO%" checkout -q -b rm base-c || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T12:30:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "RM1: lavoro su rm" || goto :err
git -C "%REPO%" checkout -q main || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T12:40:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" merge -q --no-ff rm -m "main: primo merge di rm" || goto :err
git -C "%REPO%" checkout -q rm || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T12:50:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "RM2: altro lavoro su rm" || goto :err
git -C "%REPO%" checkout -q main || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T13:00:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" merge -q --no-ff rm -m "main: secondo merge di rm" || goto :err

rem --- genealogia: storia ribasata ------------------------------------------
rem rb nasce su B, il commit viene poi riapplicato su C: gli hash cambiano,
rem il fork deve seguire la storia (C), non il nome del branch. rb-old
rem conserva il commit originale, cosi' l'ignoto resta referenziato.
git -C "%REPO%" checkout -q -b rb base-b || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T13:10:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "RB1: lavoro che verra' ribasato" || goto :err
git -C "%REPO%" branch -q rb-old || goto :err
git -C "%REPO%" rebase -q base-c || goto :err

rem --- genealogia non determinabile: ramo senza antenati condivisi ------------
git -C "%REPO%" checkout -q main || goto :err
git -C "%REPO%" checkout -q --orphan orf || goto :err
set "GIT_AUTHOR_DATE=2026-01-02T14:00:00+0000"
set "GIT_COMMITTER_DATE=%GIT_AUTHOR_DATE%"
git -C "%REPO%" commit -q --allow-empty -m "ORF1: radice senza antenati" || goto :err
git -C "%REPO%" checkout -q main || goto :err

rem --- stash: non deve comparire nel grafo -----------------------------------
echo lavoro in corso > "%REPO%\wip.txt"
git -C "%REPO%" add wip.txt || goto :err
git -C "%REPO%" stash push -q -m "lavoro in corso" || goto :err

rem --- remote-tracking: stessi nomi canonici, punte diverse --------------------
rem main punta a D; origin/main e' indietro di un commit (F1); upstream/main
rem e' indietro di tre (M1). Stesso nome canonico di main, punte diverse:
rem nessuno specchio, nessuna relazione certa dai soli nomi.
git -C "%REPO%" update-ref refs/remotes/origin/main main~1 || goto :err
git -C "%REPO%" update-ref refs/remotes/upstream/main main~3 || goto :err
rem --- specchio provato da Git: stesso nome canonico e stesso commit ---------
rem origin/branch1 punta esattamente alla punta di branch1.
git -C "%REPO%" update-ref refs/remotes/origin/branch1 branch1 || goto :err
rem --- alias: nome diverso, stessa punta di feature/x ------------------------
git -C "%REPO%" update-ref refs/remotes/origin/feature-x feature/x || goto :err

echo OK: %REPO%
exit /b 0

rem --- helper: nome finale ammesso (whitelist) -------------------------------
rem lavora su %REPORES%: %1 non e' usato perche' un percorso che termina con
rem "\" rompe il parsing di "call".
:check_name
for %%F in ("%REPORES%") do set "CHECKBASE=%%~nxF"
if /i "%CHECKBASE%"=="%BASENAME%" exit /b 0
if /i not "%CHECKBASE:~0,19%"=="%BASENAME%_" exit /b 1
if "%CHECKBASE:~19%"=="" exit /b 1
exit /b 0

rem --- helper: la directory contiene solo i file che lo script crea ----------
rem %REPORES% non deve contenere entry impreviste.
:check_entries
for %%D in ("%REPORES%") do set "CHECKDIR=%%~fD"
set "UNEXPECTED="
for /f "delims=" %%E in ('dir /b /a "%CHECKDIR%" 2^>nul') do (
    if /i not "%%E"=="%MARKER%" if /i not "%%E"==".git" if /i not "%%E"=="hello.txt" if /i not "%%E"=="wip.txt" if /i not "%%E"=="msg-c.txt" set "UNEXPECTED=%%E"
)
if defined UNEXPECTED exit /b 1
exit /b 0

:unsafe_empty
echo ERRORE: percorso del repository di prova vuoto & exit /b 1
:unsafe_dots
echo ERRORE: "%REPO%" contiene "." o "..": rifiutato perche' la risoluzione & exit /b 1
echo         del percorso sarebbe ambigua. Usa il percorso finale gia' risolto. & exit /b 1
:unsafe_name
echo ERRORE: "%REPORES%" non e' una directory di test ammessa. & exit /b 1
echo         Il nome deve essere "%BASENAME%" o "%BASENAME%_<scenario>". & exit /b 1
:unsafe_root
echo ERRORE: "%REPORES%" e' la radice del filesystem: rifiutato. & exit /b 1
:unsafe_same
echo ERRORE: "%REPORES%" e' la directory corrente: rifiutato. & exit /b 1
:unsafe_home
echo ERRORE: "%REPORES%" e' la home utente: rifiutato. & exit /b 1
:unsafe_temp
echo ERRORE: "%REPORES%" e' la directory temporanea: rifiutato. & exit /b 1
:unsafe_foreign
echo ERRORE: "%REPORES%" esiste ma non e' una directory di test di gitgraph. & exit /b 1
echo         Servono il marcatore %MARKER% con il testo esatto e nessun altro & exit /b 1
echo         file: niente viene cancellato. & exit /b 1

:err
echo ERRORE durante la creazione del repository di test
exit /b 1

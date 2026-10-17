@echo off
rem ---------------------------------------------------------------------------
rem make_test_repo.bat [percorso]
rem
rem Crea un repository Git isolato e ripetibile per i test di gitgraph.
rem Il percorso arriva da: argomento 1, variabile GG_TEST_REPO, oppure
rem %TEMP%\gitgraph_test_repo. Non tocca altri repository.
rem
rem Contenuto: main(A,B,C), branch1 da B, branch2 da B2, merge --no-ff,
rem fast-forward, branch cancellato dopo il merge, caso ambiguo (due branch
rem sullo stesso commit + un terzo creato da li'), gruppo isolato con date
rem come spareggio (oA/oC/oB da R), branch con slash, piu' riferimenti sullo
rem stesso commit, messaggio con accenti e piu' righe, uno stash (che il
rem grafo non deve includere), un remote-tracking speculare a main
rem (origin/main).
rem ---------------------------------------------------------------------------
setlocal enableextensions
set "REPO=%~1"
if "%REPO%"=="" set "REPO=%GG_TEST_REPO%"
if "%REPO%"=="" set "REPO=%TEMP%\gitgraph_test_repo"

if exist "%REPO%" rmdir /s /q "%REPO%"
mkdir "%REPO%" || goto :err

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

rem --- stash: non deve comparire nel grafo -----------------------------------
echo lavoro in corso > "%REPO%\wip.txt"
git -C "%REPO%" add wip.txt || goto :err
git -C "%REPO%" stash push -q -m "lavoro in corso" || goto :err

rem --- remote-tracking speculare (stessa punta di main: specchio) -----------
git -C "%REPO%" update-ref refs/remotes/origin/main main || goto :err

echo OK: %REPO%
exit /b 0

:err
echo ERRORE durante la creazione del repository di test
exit /b 1

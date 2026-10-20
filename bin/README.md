# gitgraph - eseguibile precompilato (Windows x64)

Questa cartella contiene `gitgraph.exe` (GUI, fase 6) e le DLL necessarie:
SDL3, SDL3_ttf e le dipendenze transitive (FreeType, HarfBuzz, GLib, ...).
Build MSYS2 UCRT64, gcc, Release, commit `59348d7` del 2026-10-20.

## Requisiti

- Windows 10/11 a 64 bit (il runtime UCRT fa parte del sistema).
- **Git nel PATH**: l'applicazione legge la storia tramite `git`; Git non e'
  incluso in questa cartella.
- Un font di sistema (Segoe UI va bene); senza font il grafo si vede ma le
  etichette no.

## Avvio

Doppio clic su `gitgraph.exe`, oppure:

    gitgraph.exe [opzioni] [percorso-repo]

La cartella dell'eseguibile contiene le DLL: non serve installare SDL. Per
provare senza aprire una finestra:

    gitgraph.exe --headless --screenshot prova.bmp [percorso-repo]

## Perche' le DLL e non un solo .exe

Staticizzare SDL3, SDL3_ttf, FreeType e HarfBuzz richiederebbe una build
statica custom delle dipendenze (alcune sono LGPL); il bundling delle DLL e'
il modo standard e piu' semplice. Le DLL sono versionate insieme
all'eseguibile. `gitgraph.exe` non e' firmato: al primo avvio SmartScreen puo'
chiedere conferma.

## Licenze

I testi delle licenze dei componenti di terze parti sono nella cartella
`licenses/`. Il codice di gitgraph non e' incluso in questa cartella: il
repository e' la fonte.

# gitgraph

Visualizzatore **in sola lettura** della storia di un repository Git,
ispirato al Revision Graph di TortoiseSVN. C11 + SDL3/SDL3_ttf, Windows.

Il programma non scrive mai sul repository: legge la storia tramite processi
`git` esterni (senza shell, senza libgit2) e non accede alla rete.

## Stato

- **Fase 1 (completata)**: lettura della storia, modello dei branch
  (padre/punto di diramazione/stato/motivazione), test automatici.
- Fasi successive: layout del grafo, disegno con SDL3/SDL3_ttf, interazione.

## Struttura

    src/util.h/.c      helper (allocazione, stringhe, esecuzione processi)
    src/gitdata.h/.c   lettura in sola lettura: commit e riferimenti
    src/config.h/.c    configurazione (long-lived, override; parsing in fase 2)
    src/history.h/.c   modello dei branch dedotto dalla storia
    src/dump_main.c    strumento di ispezione testuale (gitgraph_dump)
    tests/             generatori del repo di prova e test automatici
    CMakeLists.txt     build con CMake

`util`, `config`, `gitdata`, `history` non dipendono da SDL.

## Requisiti

- Git nel PATH.
- Un compilatore C11 e CMake >= 3.16.
- Per la GUI (dalle fasi successive): SDL3 e SDL3_ttf.

### MSYS2 UCRT64 (configurazione verificata)

    pacman -S mingw-w64-ucrt-x86_64-gcc \
              mingw-w64-ucrt-x86_64-cmake \
              mingw-w64-ucrt-x86_64-ninja

### MSVC (alternativa documentata)

Con [vcpkg](https://vcpkg.io) per le dipendenze future:

    vcpkg install sdl3 sdl3-ttf
    cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
    cmake --build build --config Release

## Compilazione

Da MSYS2 UCRT64 (o da un prompt con gcc/cmake/ninja nel PATH):

    cmake -S . -B build -G Ninja
    cmake --build build

Eseguibili prodotti: `build/gitgraph_dump` (fase 1) e, nelle fasi
successive, la GUI.

## Uso

    build/gitgraph_dump.exe [--full] [percorso-repo]

Stampa commit (ordine topologico), riferimenti e modello dei branch con
padre, stato (`certo`/`dedotto`/`ambiguo`/`nessuno`) e motivazione.
`--full` mostra i messaggi di commit completi.

## Test

    ctest --test-dir build --output-on-failure

I test usano un repository di prova isolato e ripetibile generato da
`tests/make_test_repo.bat` (Windows) o `tests/make_test_repo.sh`.
Il repository di prova contiene: merge `--no-ff`, fast-forward, branch
cancellato, caso ambiguo (piu' branch sullo stesso commit), branch con
slash, tag annotati e leggeri multipli, uno stash (da escludere), un
remote-tracking speculare e un messaggio UTF-8 multi-riga.

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
padre, stato e motivazione. `--full` mostra i messaggi di commit completi.

## Modello dei branch (sintesi)

- **Punto di diramazione**: il primo commit, risalendo la catena dei primi
  genitori dalla punta del branch, che appartiene alla *regione propria* di
  un'altra famiglia di branch. Non e' la raggiungibilita' generica: dopo un
  merge i commit del branch sono raggiungibili da altri, ma la catena dei primi
  genitori non li attraversa, quindi il fork resta quello vero.
- **Primo commit proprio** (`first_own_commit`): il piu' vecchio commit
  proprio del branch, cioe' il figlio del punto di diramazione; serve a
  ordinare i fratelli nel layout. Assente per i branch assorbiti o
  fast-forward.
- **Stati del padre**: `certo` (fatto ricavato da Git: remote-tracking di un
  branch locale), `configurato` (dichiarato in `[parents]` di `graph.cfg`, non
  un fatto dedotto), `dedotto` (una sola ipotesi plausibile), `ambiguo`
  (piu' ipotesi indistinguibili: non si inventa un padre), `nessuno`.
- **Gruppi di alias**: piu' branch sulla stessa punta formano un unico gruppo
  (stessa catena, quindi stesso padre dedotto); l'ordine interno tra loro non
  e' determinabile e la motivazione lo dichiara.
- **Long-lived**: sono il tronco o le linee principali; non hanno padre
  dedotto, ne' punto di diramazione, ne' candidati.
- **Date**: usate solo come spareggio quando la struttura non decide (per
  esempio fra diramazioni nate dallo stesso commit); ogni decisione basata su
  una data dichiara nella motivazione che e' poco affidabile con rebase.
- Dettagli e dimostrazione dei filtri: `src/history.h` e l'intestazione di
  `src/history.c`.

### Complessita'

Con `C` commit, `R` riferimenti, `G <= R` famiglie di branch e `L_g` la
lunghezza della catena dei primi genitori della famiglia `g`:

- indici dei genitori e mappa hash -> indice: `O(C + genitori)`, una volta;
- appartenenza dei commit e punti di diramazione: `O(S L_g)` (una sola
  passata sulle catene);
- matrice di relazioni fra famiglie: `O(S L_g + G^2)`;
- bitset di raggiungibilita': `O(S |raggiungibili|)`;
- deduzione dei padri: una sola passata `O(G^2 + S L_g)`.

Nessun cammino viene ripercorso per ogni coppia di branch: il costo cresce
linearmente con la dimensione del repository, non con il prodotto
`riferimenti x lunghezza delle catene`. Misure su questa macchina (Release,
gcc 16.2 MSYS2 UCRT64, `gitgraph_dump` su repository sintetici identici a
quelli dei test di scala): 22 riferimenti/392 commit 0,29 s, 84/1561 0,28 s,
266/4035 0,31 s, 363/10.000 0,37 s, 1130/50.000 1,02 s (di cui 0,35 s di
costruzione del modello).

## Test

    ctest --test-dir build --output-on-failure

Sette test, nessuno con finestra:

1. `make_test_repo` genera il repository di prova;
2. `core` verifica parsing, topologia, modello dei branch, alias, spareggio
   per data, override, long-lived e determinismo;
3. `dump_smoke` esegue lo strumento di dump;
4. `make_scale_small` + `scale_small`: ~363 riferimenti / 10.000 commit entro
   3 s;
5. `make_scale_large` + `scale_large`: ~1130 riferimenti / 50.000 commit entro
   30 s (determinismo verificato anche sul repository grande).

I repository di scala sono sintetici e deterministici: `tests/make_scale_repo.c`
genera su stdout uno stream per `git fast-import` (commit su `main`, un
`feature/<i>` ogni N commit, un `sub/<i>` mergiato nel proprio feature e un
merge `--no-ff` in `main` ogni due feature), `tests/make_scale_repo.cmake` lo
importa con `git fast-import`. Il budget misura solo lettura del repository e
costruzione del modello, non la generazione.

Il repository di prova contiene: merge `--no-ff`, fast-forward, branch
cancellato, gruppo di alias (piu' branch sullo stesso commit) e un branch
figlio ambiguo tra i membri del gruppo, gruppo isolato con spareggio per data,
branch con slash, tag annotati e leggeri multipli, uno stash (da escludere), un
remote-tracking speculare e un messaggio UTF-8 multi-riga.

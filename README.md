# gitgraph

Visualizzatore **in sola lettura** della storia di un repository Git,
ispirato al Revision Graph di TortoiseSVN. C11 + SDL3/SDL3_ttf, Windows.

Il programma non scrive mai sul repository: legge la storia tramite processi
`git` esterni (senza shell, senza libgit2) e non accede alla rete.

## Stato

- **Fase 1 (completata)**: lettura della storia, modello dei branch
  (padre/punto di diramazione/stato/motivazione), test automatici.
- **Fase 2 (completata)**: parser di `graph.cfg` con wildcard e validazione.
- Fasi successive: layout del grafo, disegno con SDL3/SDL3_ttf, interazione.

## Struttura

    src/util.h/.c      helper (allocazione, stringhe, esecuzione processi)
    src/gitdata.h/.c   lettura in sola lettura: commit e riferimenti
    src/config.h/.c    configurazione: parser di graph.cfg (wildcard, validazione)
    src/history.h/.c   modello dei branch dedotto dalla storia
    src/dump_main.c    strumento di ispezione testuale (gitgraph_dump)
    tests/             generatori del repo di prova e test automatici
    CMakeLists.txt     build con CMake

`util`, `config`, `gitdata`, `history` non dipendono da SDL.

## Requisiti

**Piattaforma: solo Windows.** `src/gitdata.c` usa le API Win32
(`CreateProcessW`, pipe, thread) e gli script di test invocano `cmd.exe`:
non c'è un porting per Linux/macOS e il tentativo di compilare altrove viene
fermato subito da CMake con un messaggio esplicito.

- Windows con Git nel PATH.
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

## Configurazione: graph.cfg

Il file è **facoltativo** e si legge da `<radice-repository>/graph.cfg`.
Se non c'è vale il tronco implicito (`main`, altrimenti `master`).
Commenti su righe dedicate (`#` o `;`), sezioni nell'ordine che si preferisce,
` chiave = valore ` con spazi attorno all'`=` ammessi:

```ini
# trunk e linee principali, nell'ordine di gerarchia
[long-lived]
main
# con piu' righe che corrispondono allo stesso branch vale la prima
release/hotfix-*
# '*' = qualsiasi sequenza di caratteri, '/' compreso
release/*

[parents]
# dichiarazioni esplicite, senza wildcard
feature/x = main
bugfix/1 = release/1.0
```

- **Commenti**: solo su righe dedicate, con `#` o `;` a inizio riga. Dentro
  una riga di dati `#` e `;` fanno parte del valore, perché i nomi di
  riferimento Git possono contenerli: `feat#1 = main` vale come override di
  `feat#1`. Di conseguenza `main # nota` **non** vale come `main`: viene
  segnalato come nome non valido, senza troncare il valore.
- **`[long-lived]`**: righe di nomi o di pattern, nell'ordine di gerarchia
  usato dal modello e dal layout. L'unico carattere speciale è `*`, che sta
  per "qualsiasi sequenza di caratteri": nei nomi dei branch la barra non è
  un separatore di significato, quindi `release/*` arriva anche a
  `release/2026/q1`. Non ci sono altre espressioni (niente `?`, niente
  espressioni regolari).
  - i nomi raccolti da una riga sono ordinati **naturalmente**: le sequenze
    di cifre si confrontano come numeri, così `release/1.0`, `release/2.0`,
    `release/10.0` (non `1.0`, `10.0`, `2.0`);
  - se un branch corrisponde a più righe **vale la prima**: con
    `release/hotfix-*` prima di `release/*`, `release/hotfix-3` resta dove
    l'ha messo la prima riga e le successive non lo spostano né lo duplicano;
  - un riferimento remoto conta per il suo **nome canonico**:
    `origin/release/2.0` corrisponde a `release/2.0` (è lo stesso ramo di
    `release/2.0`); nel modello un remoto con la stessa punta del branch
    locale è uno **specchio** (relazione certa), mentre un remoto con punta
    diversa resta autonomo e ne conserva la genealogia dedotta: dal solo nome
    non si deduce una relazione di tracking.
- **`[parents]`**: override manuali con precedenza sull'euristica; stato
  `configurato`, non un fatto ricavato da Git. Niente wildcard.

**Validazione**: un errore rifiuta il file per intero (niente
configurazione a metà) e lo strumento ripiega sul tronco implicito; un
avviso lascia applicare il file. Anche un file **presente ma non
leggibile** (permessi, una directory al suo posto, errore di lettura) viene
rifiutato con un messaggio che lo dice: un file assente, invece, non è un
errore e non produce diagnostica. Sono errori: sezione sconosciuta, riga fuori
da una sezione, intestazione malformata, `=` dentro `[long-lived]`, riga senza
`=` o con un nome mancante in `[parents]`, wildcard in `[parents]`, nome non
valido come riferimento Git, padre uguale al branch, doppio override per lo
stesso branch. Sono avvisi: nome che non esiste nel repository, pattern senza
corrispondenze, voce già long-lived, ciclo di override (ogni ciclo è
segnalato una volta sola, con i nomi dei branch coinvolti; in fase di modello
i branch coinvolti diventano ambigui).

`gitgraph_dump` stampa in testa l'ordine long-lived e gli override applicati,
e la diagnostica su stderr.

## Uso

    build/gitgraph_dump.exe [--full] [percorso-repo]

Stampa commit (ordine topologico), riferimenti e modello dei branch con
padre, stato e motivazione. `--full` mostra i messaggi di commit completi.

## Modello dei branch (sintesi)

- **Punto di diramazione**: il primo commit, risalendo la catena dei primi
  genitori dalla punta del branch, che appartiene alla *regione propria* di
  un'altra famiglia di branch. Non e' la raggiungibilita' generica: dopo un
  merge i commit del branch sono raggiungibili da altri, ma la catena dei primi
  genitori non li attraversa, quindi il fork resta quello vero. Il calcolo non
  dipende dall'ordine dei nomi: un branch che incontra la **punta** di un
  altro ramo si e' diramato li'; i tronchi (long-lived) proseguono sempre, per
 che non possono essere una diramazione di un altro ramo.
- **Ogni riferimento conserva la sua punta**: due riferimenti con lo stesso
  nome canonico ma punte diverse (`main` e `origin/main` indietro di un
  commit) restano due riferimenti distinti, ciascuno con la propria punta; lo
  **specchio** si dichiara solo quando la punta coincide davvero, ed e' l'unico
  caso di relazione `certo` fra riferimenti.
- **Primo commit proprio** (`first_own_commit`): il piu' vecchio commit
  proprio del branch, cioe' il figlio del punto di diramazione; serve a
  ordinare i fratelli nel layout. Assente per i branch assorbiti o
  fast-forward.
- **Genealogia non determinabile**: quando nessun branch puo' essere il padre
  (nessun ramo piu' antico condiviso, o nessun candidato sopravvive ai
  filtri), il modello lo dichiara: stato `nessuno`, messaggio che lo dice e
  `genealogy_undetermined` a `true` nel modello.
- **Stati del padre**: `certo` (fatto ricavato da Git: uno specchio che punta
  allo stesso commit di un branch locale), `configurato` (dichiarato in
  `[parents]` di `graph.cfg`, non un fatto dedotto), `dedotto` (una sola
  ipotesi plausibile; quando il punto di diramazione appartiene alla regione
  propria di un solo candidato, e' quello il padre), `ambiguo` (piu' ipotesi
  indistinguibili: non si inventa un padre), `nessuno`.
- **Gruppi di alias**: piu' riferimenti sulla stessa punta formano una
  famiglia (stessa catena, quindi stesso padre dedotto); fra piu' branch
  **locali** sulla stessa punta resta ambiguo quale sia il nome del padre, e
  la motivazione lo dichiara.
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
- deduzione dei padri: una sola passata `O(G^2 + S L_g)`;
- controllo delle relazioni circolari: `O(R)` sui grafi aciclici (mappa
  nome->nodo e memoizzazione dei nodi gia' verificati).

Nessun cammino viene ripercorso per ogni coppia di branch: il costo cresce
linearmente con la dimensione del repository, non con il prodotto
`riferimenti x lunghezza delle catene`. Misure su questa macchina (Release,
gcc 16.2 MSYS2 UCRT64, `gitgraph_dump` su repository sintetici identici a
quelli dei test di scala): 22 riferimenti/392 commit 0,29 s, 84/1561 0,28 s,
266/4035 0,31 s, 363/10.000 0,37 s, 1130/50.000 1,02 s (di cui 0,35 s di
costruzione del modello).

## Test

    ctest --test-dir build --output-on-failure

Undici test, nessuno con finestra:

1. `make_test_repo` genera il repository di prova;
2. `scripts_guard` verifica le protezioni di `make_test_repo.bat`: creazione e
   rigenerazione dentro la radice autorizzata (comprese forme diverse del
   percorso: barre in avanti, barra finale, maiuscole, suffisso di scenario),
   rifiuto di una directory valida fuori dalla radice, di una sorella con nome
   simile (`test_repos_extra`), di una **radice che è una giunzione** verso
   l'esterno (con la destinazione fisica verificata intatta), di una
   destinazione che è una giunzione, dei marcatori non ammessi (27 byte con la
   prima riga corretta, terminatore misto, testo aggiuntivo) e di quelli ammessi
   (24, 25 e 26 byte: devono essere accettati e rigenerati), di directory con
   file estranei, dei tentativi di uscire con `..`, della radice del filesystem
   e della home, di nome non ammesso, di radice non impostata o inesistente e
   di argomento vuoto. Ogni caso negativo verifica anche che i file sentinella
   siano ancora presenti e inalterati; se `mklink` non è disponibile i casi che
   richiedono una giunzione sono dichiarati **saltati**, mai superati. Tutto
   avviene in una sandbox sotto `build/`;
3. `core` verifica parsing, topologia, modello dei branch, alias, spareggio
   per data, override, long-lived, determinismo, quoting degli argomenti e
   cattura di stdout/stderr senza blocco (pipe riempite oltre la capacita',
   entrambe le pipe attive contemporaneamente, uscita non zero e fallimento
   della creazione dei thread di lettura);
4. `config` verifica `graph.cfg`: sezioni, commenti su righe dedicate, `#` e
   `;` dentro i nomi, BOM/CRLF, wildcard con ordine naturale, barra
   attraversata, priorita' della prima riga che corrisponde, riferimenti remoti
   per nome canonico, tronco implicito, validazione dei nomi di riferimento
   (comprese le lunghezze attorno al suffisso `.lock`), file assente/illeggibile
   (directory al suo posto)/vuoto, casi di errore che rifiutano il file, avvisi
   che lo lasciano valido, cicli di override (nessuno, due nodi, catena lunga,
   cicli distinti, ciascuno segnalato una volta) e uso end-to-end dentro il
   repository di prova (override → stato `configurato`, long-lived senza fork
   né candidati, remoto con punta diversa che mantiene la genealogia dedotta);
5. `dump_smoke` esegue lo strumento di dump;
6. `make_scale_small` + `scale_small`: ~363 riferimenti / 10.000 commit entro
   3 s;
7. `make_scale_large` + `scale_large`: ~1130 riferimenti / 50.000 commit entro
   30 s (determinismo verificato anche sul repository grande);
8. `make_scale_deep` + `scale_deep`: ~890 riferimenti / 20.000 commit entro
   15 s, con ogni branch nato dal precedente (catena profonda): verifica il
   controllo delle relazioni circolari e l'ordinamento dei padri.

I repository di scala sono sintetici e deterministici: `tests/make_scale_repo.c`
genera su stdout uno stream per `git fast-import` (commit su `main`, un
`feature/<i>` ogni N commit, un `sub/<i>` mergiato nel proprio feature e un
merge `--no-ff` in `main` ogni due feature), `tests/make_scale_repo.cmake` lo
importa con `git fast-import`. Il budget misura solo lettura del repository e
costruzione del modello, non la generazione.

Il repository di prova contiene: merge `--no-ff`, fast-forward, branch
cancellato, gruppo di alias (piu' branch sullo stesso commit) e un branch
figlio ambiguo tra i membri del gruppo, gruppo isolato con spareggio per data,
riferimenti remoti con punte diverse (`origin/main`, `upstream/main`) piu' uno
specchio provato e un alias con nome diverso, ramo nato dal **secondo
genitore** di un merge, merge ripetuti dello stesso branch, storia **ribasata**
(con il commit originale conservato in un altro branch), ramo **senza antenati**
condivisi (genealogia non determinabile), branch con slash, tag annotati e
leggeri multipli, uno stash (da escludere) e un messaggio UTF-8 multi-riga.

Lo script `make_test_repo.bat` cancella qualcosa **solo** dentro la radice dei
repository di test, quella indicata da `GG_TEST_ROOT` (impostata da CMake, che
crea anche la directory: `build/test_repos` per configurazione predefinita).
Il percorso della destinazione arriva dall'argomento 1 o da `GG_TEST_REPO`; se
entrambi mancano lo script si rifiuta, senza alcun percorso predefinito.
Tutte queste condizioni devono valere, altrimenti lo script fallisce senza
toccare nulla:

- `GG_TEST_ROOT` è impostata, esiste come directory e **non è un collegamento**:
  il controllo guarda gli attributi del filesystem e sale fino alla radice del
  filesystem, perché il confronto testuale dei percorsi non risolve i
  collegamenti (una radice-giunzione verso l'esterno passerebbe il confronto con
  il padre della destinazione). In caso di dubbio (controllo non eseguibile,
  permessi, PowerShell non disponibile) la cancellazione viene rifiutata;
- il percorso non contiene `.` o `..` e non termina con `.` (nessuna
  risoluzione ambigua);
- una volta risolto (`/` e `\` normalizzati, percorso completo), il **padre**
  della destinazione coincide **esattamente** con la radice: il confronto è
  per uguaglianza, mai per prefisso, quindi una directory sorella con nome
  simile (`test_repos_extra`) non è considerata dentro;
- il nome finale è `gitgraph_test_repo` oppure `gitgraph_test_repo_<scenario>`
  (whitelist del nome);
- il percorso non è la radice del filesystem, la directory corrente, la home o
  quella temporanea;
- se la directory esiste: coincide con radice + nome, **non è un collegamento**
  (giunzione o simbolico, che porterebbero la cancellazione fuori dalla radice;
  per la destinazione è sufficiente il percorso stesso, perché i suoi antenati
  sono la radice, già verificata), contiene il marcatore `.gitgraph_test_repo`
  con **esattamente** il testo `gitgraph test repository` e nessun altro byte:
  il confronto è fatto sui byte (24 senza terminatore, 25 con LF, 26 con CRLF),
  perché leggere solo la prima riga accetterebbe contenuti aggiuntivi che non
  la modificano; e nessun file diverso da quelli creati dallo script (`.git`,
  `hello.txt`, `wip.txt`, `msg-c.txt`).

Il nome della directory e il marcatore, da soli, **non** autorizzano mai una
cancellazione: sono due delle condizioni, non un suffisso sufficiente. Tutte le
verifiche sono coperte dal test `scripts_guard`, che lavora solo dentro
`build/guard_sandbox` e verifica, per ogni caso negativo, sia il rifiuto sia
l'integrità dei file sentinella.

I percorsi possono contenere spazi e caratteri che `cmd.exe` tratta in modo
speciale (`&` è un separatore di comandi): lo script non assembla mai
frammenti di riga di comando dentro variabili e passa ogni percorso ai
programmi esterni come argomento quotato sul posto, quindi una build situata,
per esempio, in `C:\dev\repo & build` continua a funzionare.

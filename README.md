# gitgraph

Visualizzatore **in sola lettura** della storia di un repository Git,
ispirato al Revision Graph di TortoiseSVN. C11 + SDL3/SDL3_ttf, Windows.

Il programma non scrive mai sul repository: legge la storia tramite processi
`git` esterni (senza shell, senza libgit2) e non accede alla rete.

## Stato

- **Fase 1 (completata)**: lettura della storia, modello dei branch
  (padre/punto di diramazione/stato/motivazione), test automatici.
- **Fase 2 (completata)**: parser di `graph.cfg` con wildcard e validazione.
- **Fase 3 (completata)**: disposizione del grafo (layout headless): nodi per
  branch, corsie, righe e archi padre/suggerito; nessuna finestra.
- **Fase 4 (completata)**: layout dei commit (riga, corsia, proprietario,
  archi tra commit), etichette dei riferimenti remoti sulla corsia del locale,
  corsia anonima; `gitgraph_dump --layout`.
- Fasi successive: disegno con SDL3/SDL3_ttf, interazione.

## Struttura

    src/util.h/.c      helper (allocazione, stringhe, esecuzione processi)
    src/gitdata.h/.c   lettura in sola lettura: commit e riferimenti
    src/config.h/.c    configurazione: parser di graph.cfg (wildcard, validazione)
    src/history.h/.c   modello dei branch dedotto dalla storia
    src/layout.h/.c    disposizione del grafo (corsie, righe, archi; commit)
    src/dump_main.c    strumento di ispezione testuale (gitgraph_dump)
    tests/             generatori del repo di prova e test automatici
    CMakeLists.txt     build con CMake

`util`, `config`, `gitdata`, `history`, `layout` non dipendono da SDL.

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

    build/gitgraph_dump.exe [--full] [--layout] [percorso-repo]

Stampa commit (ordine topologico), riferimenti, modello dei branch con
padre, stato e motivazione, e la disposizione del grafo (corsie, righe e
archi). `--full` mostra i messaggi di commit completi. `--layout` stampa solo
la tabella dei commit (riga, corsia, proprietario) e gli archi tra commit.

## Modello dei branch (sintesi)

- **Punto di diramazione**: il primo commit, risalendo la catena dei primi
  genitori dalla punta del branch, in cui un'altra famiglia e' un plausibile
  padre. I candidati sono le famiglie che contengono quel commit, che non stanno
  dentro il branch (discendenti o continuazioni) e che non sono figli suoi (nate
  dalla sua catena). Non e' la raggiungibilita' generica: dopo un merge i
  commit del branch sono raggiungibili da altri, ma la catena dei primi
  genitori non li attraversa, quindi il fork resta quello vero. Il calcolo non
  dipende dall'ordine dei nomi: un branch che incontra la **punta** di un
  altro ramo si e' diramato li'; i tronchi (long-lived) proseguono sempre, per
  che non possono essere una diramazione di un altro ramo.
- **Proprietario dei commit**: ogni commit del log ha un proprietario, uno
  solo, esportato dal modello (`History.commit_owner`). I tronchi reclamano la
  loro catena dei primi genitori per primi, nell'ordine di `[long-lived]`; poi
  ogni altro ramo reclama, risalendo dalla punta, i commit non ancora
  assegnati, fermandosi al primo gia' assegnato (il suo punto di diramazione).
  Un padre reclama sempre prima dei figli, cosi' il figlio si ferma al fork e
  non risale oltre. L'ordine fra rami dello stesso gruppo usa la struttura e
  l'eta' dei rami, mai il nome. Un commit che nessun ramo reclama (la catena di
  un ramo cancellato dopo un merge: e' raggiungibile, ma non appartiene piu' a
  nessuno) resta senza proprietario e il layout lo mette nella **corsia
  anonima**, in coda a tutte le altre.
- **Il nome non decide niente**: l'ordine alfabetico non entra in nessuna
  regola; i confronti fra famiglie usano la struttura dei commit e, quando
  serve, l'orario dei commit (dato di Git). Il test `invariance` permuta i
  nomi e verifica che stato, padre, candidati e padre suggerito restino
  identici in ogni permutazione.
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
  ipotesi plausibile), `ambiguo` (piu' ipotesi indistinguibili: non si inventa
  un padre), `nessuno`. Si sceglie in quest'ordine: se il punto di diramazione
  e' la punta di un'altra famiglia, quella famiglia e' il padre (lo decide la
  struttura); se un candidato ha integrato il branch con un merge (la punta del
  branch sta nella sua storia), e' il padre (lo dice la struttura; vale anche se
  e' nato dallo stesso punto, perche' il merge e' un fatto), ma **solo se la
  prova identifica un unico candidato**: con piu' candidati che hanno integrato
  il branch la struttura non distingue e non si sceglie il primo trovato
  (l'ordine dei riferimenti non e' un fatto di Git), quindi si prosegue con le
  regole seguenti; se fra i candidati
  c'e' un tronco, e' il padre (un long-lived non si deduce, e' dichiarato); se
  resta un solo candidato, e' il padre dedotto. Il tronco e il candidato unico
  decidono pero' solo quando sul punto non ci sono fratelli. Due rami nati
  dallo **stesso punto di diramazione** (nessuno
  contenente l'altro) sono sempre `ambigui` l'uno con l'altro, anche se il
  candidato e' uno solo: senza ordine di nascita la struttura non distingue
  chi e' nato prima, e l'unico modo per dirlo e' dichiararlo in `[parents]`.
  Il test `invariance` copre anche il caso di due candidati che hanno integrato
  lo stesso branch con un merge: resta `ambiguo` e la motivazione lo dichiara.
- **Padre suggerito** (`suggested_parent`, `suggestion_basis`): nei casi
  ambigui il modello indica un suggerimento, **non** un padre: non e' uno stato
  "certo" ne' "dedotto". Il criterio e' documentato e verificato dal test: fra
  i candidati ambigui e' il piu' vecchio (col primo commit proprio piu'
  vecchio del branch), a parita' di date e' il nome, e la motivazione dichiara
  la base del suggerimento ("padre suggerito per data, poco affidabile con
  rebase", oppure lo spareggio sul nome). Se nessun fratello e' piu' antico del
  branch (il ramo e' il piu' vecchio del gruppo), il suggerito e' il primo
  antenato non fratello sotto il punto di diramazione, cioe' il ramo da cui il
  gruppo si e' staccato (di solito il tronco): base `SUGGESTION_ANCESTOR` e
  motivazione "nessun fratello piu' vecchio: padre suggerito '...'". Solo i veri
  radice (nessun fratello piu' vecchio e nessun antenato non fratello sotto il
  punto) restano senza suggerimento. Il grafo dei padri suggeriti non ha cicli.
  Serve al layout per piazzare il branch in modo tratteggiato finche' il padre
  non e' dichiarato.
- **Gli ambigui si risolvono con `[parents]`**: finche' il padre non e'
  dichiarato in `graph.cfg`, il caso resta ambiguo e il layout mostra il
  suggerimento come tratteggio. Un override valido ha precedenza assoluta e
  rende il padre `configurato`. Esempio: nel repository di prova `branch2` e'
  nato da un commit (B2) che sta sia sulla catena di `branch1` sia su quella di
  `sp`, quindi e' ambiguo fra i due e il suggerimento e' `branch1`;
  `parents = branch2 branch1` lo rende certo. Due fratelli nati dallo stesso
  punto (`feature/login` e `bugfix/crash` nel repository di riproduzione mk3)
  si dichiarano allo stesso modo.
- **Gruppi di alias**: piu' riferimenti sulla stessa punta formano una
  famiglia (stessa catena, quindi stesso padre dedotto); fra piu' branch
  **locali** sulla stessa punta resta ambiguo quale sia il nome del padre, e
  la motivazione lo dichiara.
- **Long-lived**: sono il tronco o le linee principali; non hanno padre
  dedotto, ne' punto di diramazione, ne' candidati.
- **Date**: non scelgono mai il padre. Non esistono piu' ne' lo scarto di un
  candidato per data (un ramo nato dopo non e' "figlio": senza ordine di
  nascita, due rami nati dallo stesso punto restano fratelli) ne' l'uso della
  data per decidere il padre. L'eta' decide solo il **suggerimento**: fra
  candidati ambigui, il padre suggerito e' quello col primo commit proprio
  piu' vecchio. Quindi la motivazione non contiene piu' uno "scarto dei
  candidati per data", mentre la frase "padre suggerito per data, poco
  affidabile con rebase" resta, dichiarata.
- Dettagli e dimostrazione dei filtri: `src/history.h` e l'intestazione di
  `src/history.c`.

### Complessita'

Con `C` commit, `R` riferimenti, `G <= R` famiglie di branch e `L_g` la
lunghezza della catena dei primi genitori della famiglia `g`:

- indici dei genitori e mappa hash -> indice: `O(C + genitori)`, una volta;
- quante famiglie hanno ogni commit sulla catena (`nchain`) e punto di
  diramazione di ogni famiglia: `O(S L_g)` (una sola passata sulle catene);
- matrici per commit: il tronco che possiede il commit, la famiglia che
  contiene la punta di un'altra, i pari sui punti di diramazione: `O(S L_g)`,
  con memoria legata al numero di riferimenti (le liste dei pari coprono solo
  i punti di diramazione e le punte, non tutti i commit condivisi);
- bitset di raggiungibilita': `O(S |raggiungibili|)`;
- deduzione dei padri: una sola passata `O(G^2 + S L_g)`;
- controllo delle relazioni circolari: `O(R)` sui grafi aciclici (mappa
  nome->nodo e memoizzazione dei nodi gia' verificati).

Nessun cammino viene ripercorso per ogni coppia di branch: il costo cresce
linearmente con la dimensione del repository, non con il prodotto
`riferimenti x lunghezza delle catene`. Misure su questa macchina (Release,
gcc 16.2 MSYS2 UCRT64, `gitgraph_dump` su repository sintetici identici a
quelli dei test di scala): 22 riferimenti/392 commit 0,29 s, 84/1561 0,28 s,
266/4035 0,31 s, 363/10.000 1,0 s, 1130/50.000 4,6 s (di cui 2,9 s di
costruzione del modello: le regole nuove percorrono le catene piu' volte del
vecchio contatore di proprietari, e le liste dei pari sostituiscono la matrice
di relazioni `G x G`). I budget dei test sono 3 s, 30 s e 15 s.

## Disposizione del grafo (layout)

`gitgraph_dump` stampa anche la disposizione: un nodo per branch, con la corsia
(colonna) e la riga (posizione topologica della punta, 0 = commit piu'
recente), piu' gli archi. `--layout` mostra invece la disposizione a livello di
commit: una riga per commit, con la corsia del proprietario e il proprietario,
e gli archi tra commit.

- **Nodi**: un nodo per riferimento, alla riga della sua punta. I riferimenti
  con la stessa punta (alias) condividono riga e corsia: sono lo stesso ramo e
  non finiscono in corsie diverse.
- **Corsie**: i tronchi (long-lived) prendono le prime corsie, nell'ordine di
  `[long-lived]`: il tronco e' la corsia 0. Gli altri rami seguono accanto al
  proprio padre; fra fratelli viene prima il ramo nato prima (il primo commit
  proprio piu' vecchio), senza guardare i nomi. Le famiglie che la visita in
  profondita' non raggiunge (un ciclo di paternita', possibile solo in un
  modello artificiale) ricevono comunque una corsia, in coda.
- **Etichette remote**: un riferimento remoto la cui punta sta sulla catena dei
  primi genitori del branch **locale con lo stesso nome** (in anticipo o in
  ritardo) non ha una corsia propria: e' un'etichetta del locale e ne condivide
  la corsia (`label_of` nel nodo). Un remoto senza locale resta un ramo normale.
  In `origin/main` indietro di un commit, `origin/main` sta sulla corsia di
  `main`; se invece e' in anticipo, i suoi commit sono suoi ma la corsia resta
  quella di `main`. E' l'unico caso in cui due nodi con la stessa riga possono
  stare in corsie diverse (l'etichetta e l'eventuale altro ramo sulla stessa
  punta). Non c'e' ancora riuso delle corsie: la corsia liberata da
  un'etichetta non viene riassegnata.
- **Archi fra branch**: continui verso il padre certo, configurato o dedotto;
  tratteggiati verso il padre **suggerito** di un caso ambiguo, da mostrare
  tratteggiato finche' `[parents]` non dichiara il padre vero. Le etichette
  remote e i riferimenti della stessa famiglia non producono un arco: sono lo
  stesso ramo.
- **Commit**: ogni commit ha una riga (la sua posizione topologica, la stessa
  del dump) e una corsia: quella del ramo che lo possiede, oppure la corsia
  anonima in coda se non lo possiede nessuno. Gli archi tra commit hanno un
  tipo esplicito: **primo genitore** (la catena), **genitore di merge** (secondo
  genitore e successivi) e **diramazione** (dal primo commit proprio di un ramo
  al suo punto di fork). La catena dei primi genitori di un ramo resta sulla
  sua corsia finche' il proprietario non cambia: il cambio e' il fork.
- **Nessun nome nelle scelte**: corsie, righe e proprietari non cambiano se si
  rinominano i riferimenti. Il test `invariance` confronta la disposizione
  (branch e commit) in ogni permutazione, insieme al resto del modello.
- **Solo numeri, nessuna finestra**: la fase 3 calcola le posizioni; il disegno
  con SDL3/SDL3_ttf resta alla fase successiva.

## Test

    ctest --test-dir build --output-on-failure

Tredici test, nessuno con finestra:

1. `make_test_repo` genera il repository di prova (la fixture rimuove prima il
   solo percorso `<build>/test-repos/gitgraph_test_repo`);
2. `core` verifica parsing, topologia, modello dei branch, alias, motivazioni
   (in particolare il padre suggerito e la motivazione che lo dichiara),
   override, long-lived, determinismo, quoting degli argomenti,
   scelta delle voci del PATH per `git.exe` (vuota, `.`, `..`, relativa, con
   virgolette, UNC, assoluta: provata sulla funzione, non sulla ricerca, che
   vale una volta sola per processo; per ogni voce è verificata anche la
   costruzione del candidato `...\git.exe`: virgolette e spazi ai bordi
   tolti, barre finali in stile Unix riscritte come barra Windows, radice
   del drive senza barra replicata, `.` e le voci relative senza candidato),
   cattura di stdout/stderr senza blocco (pipe riempite oltre la capacità,
   entrambe le pipe attive contemporaneamente, uscita non zero e fallimento
   della creazione dei thread di lettura), conversione dei percorsi UTF-16 in
   UTF-8 (un percorso non ASCII che in UTF-8 supera `MAX_PATH` byte non viene
   troncato: la lunghezza necessaria e' dichiarata e il buffer e' dimensionato
   sul caso peggiore) e sicurezza della ricerca di git:
   un `git.exe` finto nella directory corrente non deve mai essere eseguito al
   posto del vero;
3. `config` verifica `graph.cfg`: sezioni, commenti su righe dedicate, `#` e
   `;` dentro i nomi, BOM/CRLF, wildcard con ordine naturale, barra
   attraversata, priorita' della prima riga che corrisponde, riferimenti remoti
   per nome canonico, tronco implicito, validazione dei nomi di riferimento
   (comprese le lunghezze attorno al suffisso `.lock`), file assente/illeggibile
   (directory al suo posto)/vuoto, casi di errore che rifiutano il file, avvisi
   che lo lasciano valido, cicli di override (nessuno, due nodi, catena lunga,
   cicli distinti, ciascuno segnalato una volta) e uso end-to-end dentro il
   repository di prova (override → stato `configurato`, long-lived senza fork
   né candidati, remoto con punta diversa che mantiene la genealogia dedotta) e
   motivazioni che non cominciano mai con un separatore "; " senza testo prima;
4. `layout` verifica la disposizione del grafo (fase 3): un nodo per branch
   alla riga della sua punta, il tronco in corsia 0, i long-lived nelle prime
   corsie secondo `[long-lived]`, gli alias sulla stessa punta nella stessa
   corsia, le **etichette remote** (`origin/main`, `upstream/main` sulla
   corsia di `main`; `origin/branch1` etichetta di `branch1`), archi continui
   verso il padre e tratteggiati verso il suggerito (`branch1` -> `main`,
   `branch2` -> `branch1`), corsie contigue 0..lanes-1. Verifica inoltre la
   **disposizione dei commit**: una cella per commit con riga e proprietario
   attesi (`A`,`B`,`C` a `main`; `B1`..`B3` a `branch1`; `C1`,`C2` a
   `branch2`; il merge a `main`), la catena dei primi genitori del tronco
   sulla sua corsia, l'arco di merge verso il secondo genitore, l'arco di
   diramazione, il commit `T1` (branch cancellato) nella corsia anonima in
   coda e il determinismo (nodi, archi, commit). Su un repository sintetico
   costruito dal test (main `A`,`B`,`C`; `branch1` `B1`,`B2`,`B3` da `B`;
   `branch2` `C1`,`C2` da `B2`; merge `--no-ff`; `origin/main` in anticipo e
   `upstream/main` in ritardo) confronta una **tabella scritta a mano**. Infine
   un modello artificiale con un ciclo di paternita' verifica che le famiglie
   che la visita in profondita' non raggiunge ricevano comunque una corsia. Le
   stesse regole sono ripetute su ogni modello dalla proprieta' condivisa
   `gg_check_layout`;
5. `dump_smoke` esegue lo strumento di dump;
6. `invariance` verifica che la genealogia non dipenda dall'ordine alfabetico:
   per ogni gruppo di branch con nidificazione o fratelli (nel repository di
   prova `branch1`/`branch2`, `branch1`/`sp`, `oA`/`oB`/`oC`,
   `amb1`/`amb2`/`ambbase`, `feature-x`/`rb`/`rm`) costruisce tutte le
   permutazioni dei nomi (fino a 6 per gruppo) e confronta stato, padre,
   candidati, padre suggerito e disposizione (corsie, righe, archi, **celle
   dei commit e archi tra commit**) **per punta di commit**; costruisce inoltre
   due repository piccoli nella directory temporanea (nidificazione e tre rami
   dallo stesso commit, anche con tutti i commit alla stessa ora, come nei
   repository generati in blocco) e li rimuove alla fine. L'unica cosa che non
   viene confrontata e' lo spareggio dichiarato sul nome a parita' di date:
   li' si verifica la base dello spareggio, non la scelta. Per il caso di **due
   candidati che hanno integrato lo stesso branch con un merge** costruisce un
   repository dedicato e confronta, oltre a tutte le permutazioni dei nomi,
   anche tre ordini diversi dell'array dei riferimenti (identita', invertito,
   ruotato): lo stato deve restare `ambiguo` senza padre, con entrambi i
   candidati e la motivazione che dichiara il merge. Il test verifica anche che
   il grafo dei padri suggeriti non contenga cicli in nessuna permutazione, e
   costruisce i quattro casi di riproduzione mk3 (due rami nati dallo stesso
   punto, modalita' A e B con i nomi in entrambi gli ordini): tutti devono
   risultare `ambigui` con l'altro come unico candidato e un solo padre
   suggerito per data;
7. `make_scale_small` + `scale_small`: ~363 riferimenti / 10.000 commit entro
   3 s; il test stampa il numero di corsie rispetto ai rami (senza remoti nel
   generatore sono uguali) e verifica che non ci sia una corsia anonima;
8. `make_scale_large` + `scale_large`: ~1130 riferimenti / 50.000 commit entro
   30 s (determinismo verificato anche sul repository grande);
9. `make_scale_deep` + `scale_deep`: ~890 riferimenti / 20.000 commit entro
   15 s, con ogni branch nato dal precedente (catena profonda): verifica il
   controllo delle relazioni circolari e l'ordinamento dei padri;
10. `encoding` verifica che i file in `src/` e `tests/` siano **ASCII**: fallisce
    se un file contiene un byte oltre `0x7F` (quindi anche i byte `C3 82` /
    `C3 83` del mojibake). Script CMake puro, senza compilare.

I repository di scala sono sintetici e deterministici: `tests/make_scale_repo.c`
genera su stdout uno stream per `git fast-import` (commit su `main`, un
`feature/<i>` ogni N commit, un `sub/<i>` mergiato nel proprio feature e un
merge `--no-ff` in `main` ogni due feature), `tests/make_scale_repo.cmake` lo
importa con `git fast-import`. Il budget misura solo lettura del repository e
costruzione del modello, non la generazione.

Il repository di prova contiene: merge `--no-ff`, fast-forward, branch
cancellato, gruppo di alias (piu' branch sullo stesso commit), gruppo isolato
(tre rami dallo stesso commit: tutti ambigui fra loro, con il padre
suggerito per data dichiarato solo sul ramo nato dopo), ramo nato da
un commit interno di un altro ramo (che resta ambiguo fra i due rami che lo
contengono),
riferimenti remoti con punte diverse (`origin/main`, `upstream/main`) piu' uno
specchio provato e un alias con nome diverso, ramo nato dal **secondo
genitore** di un merge, merge ripetuti dello stesso branch, storia **ribasata**
(con il commit originale conservato in un altro branch), ramo **senza antenati**
condivisi (genealogia non determinabile), branch con slash, tag annotati e
leggeri multipli, uno stash (da escludere) e un messaggio UTF-8 multi-riga.

Lo script `make_test_repo.bat` crea il repository e **non cancella mai nulla**:
se la directory di destinazione non esiste o e' vuota la usa, altrimenti esce
con errore. La pulizia prima di ogni esecuzione la fa CMake
(`tests/make_test_repo_fixture.cmake`), che rimuove con `file(REMOVE_RECURSE ...)`
il solo percorso `<build>/test-repos/gitgraph_test_repo` (scritto in
`CMakeLists.txt`). Anche i repository di scala stanno sotto `<build>/test-repos/`
e la loro pulizia cancella solo il percorso indicato. Non c'e' nessun controllo
di giunzioni: si cancella solo una cartella di test dentro la directory di
build, con un percorso noto, e CMake non segue i collegamenti in
`REMOVE_RECURSE`.

I percorsi possono contenere spazi e caratteri che `cmd.exe` tratta in modo
speciale (`&` e' un separatore di comandi): i percorsi arrivano tramite variabili
d'ambiente, che `cmd.exe` non viene reinterpretato, e quelli passati in linea di
comando sono quotati sul posto. Una build situata, per esempio, in
`C:\dev\repo & build` continua a funzionare.

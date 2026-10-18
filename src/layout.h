/* layout.h - disposizione del grafo dei branch (nessuna dipendenza da SDL).
 *
 * Fase 3: calcolo headless delle posizioni. Il disegno resta alla fase
 * successiva; qui si producono solo numeri verificabili.
 *
 * NODI = BRANCH. C'e' un nodo per ogni BranchNode di History, nello stesso
 * ordine. Ogni nodo ha:
 *   - una RIGA: la posizione topologica del commit che lo ancora (la punta
 *     del branch), con la stessa convenzione del dump (0 = commit piu'
 *     recente, indici crescenti verso il passato). Una punta fuori dal log
 *     (riferimento non raggiungibile) lascia la riga a SIZE_MAX;
 *   - una CORSIA (colonna): 0 = la piu' a sinistra. I tronchi (long-lived)
 *     occupano le prime corsie, nell'ordine di [long-lived] (il tronco e' la
 *     corsia 0); gli altri rami seguono, accanto al proprio padre.
 *
 * ARCHI = relazioni padre fra branch:
 *   - LAYOUT_EDGE_PARENT (continuo): padre certo, configurato o dedotto;
 *   - LAYOUT_EDGE_SUGGESTED (tratteggiato): padre suggerito di un caso
 *     ambiguo, cioe' una ipotesi da mostrare tratteggiata finche' [parents]
 *     non dichiara il padre vero.
 *
 * FAMIGLIE. I riferimenti con la stessa punta sono la stessa famiglia (stessa
 * genealogia, stessa corsia): gli alias condividono la posizione, cosi' il
 * disegno non li sovrappone in corsie diverse.
 *
 * ETICHETTE REMOTE. Un riferimento remoto la cui punta sta sulla catena dei
 * primi genitori del branch locale con lo stesso nome (in anticipo o in
 * ritardo) non ha una corsia propria: e' un'etichetta del locale e ne
 * condivide la corsia (label_of). Un remoto senza locale resta un ramo
 * normale. La corsia del remoto puo' quindi differire da quella degli altri
 * riferimenti sulla stessa punta: e' l'unico caso in cui due nodi con la
 * stessa riga non stanno nella stessa corsia.
 *
 * COMMIT. Oltre ai nodi-branch, la disposizione assegna una RIGA e una CORSIA
 * a ogni commit del log: la riga e' la sua posizione topologica, la corsia e'
 * quella del ramo proprietario (History.commit_owner).
 *
 * CORSIE ANONIME. I commit che nessun ramo reclama (la catena di un ramo
 * sparito dopo un merge) sono raggruppati in CATENE: componenti connesse di
 * commit anonimi unite dai primi genitori. Il blocco delle corsie anonime sta
 * subito a destra dei tronchi, prima dei rami con nome: i commit senza
 * proprietario restano cosi' accanto al tronco da cui pendono, senza spostare
 * fuori i rami con nome. Le corsie del blocco si assegnano per intervalli di
 * righe (colorazione greedy, in ordine di riga): l'intervallo di una catena va
 * dalla riga del suo commit piu' recente a quella del genitore del suo commit
 * piu' vecchio; due catene con intervalli sovrapposti stanno in corsie diverse,
 * due no possono condividerne una. Cosi' nessun arco passa sopra il nodo di
 * un'altra catena nella stessa corsia, ed e' il caso normale che N rami
 * creati, mergiati e cancellati in sequenza usino una sola corsia anonima.
 *
 * ARCHI TRA COMMIT. Per ogni commit e ogni suo genitore c'e' un solo arco:
 *   - LAYOUT_CEDGE_FIRST_PARENT: il primo genitore nella STESSA corsia (la
 *     catena dei primi genitori che resta sulla corsia);
 *   - LAYOUT_CEDGE_BRANCH: il primo genitore in un'ALTRA corsia. E' la
 *     diramazione: parte dal commit proprietario piu' vecchio di un ramo (o
 *     dal fondo di una catena anonima) e arriva al suo primo genitore. Il
 *     punto di diramazione e' il primo genitore reale, non il campo fork del
 *     modello; l'arco non esiste se il genitore e' nella stessa corsia, dove
 *     duplicherebbe l'arco di catena;
 *   - LAYOUT_CEDGE_MERGE_PARENT: il secondo genitore e i successivi di un
 *     merge.
 *
 * INVARIANZA PER RINOMINA. La disposizione non dipende dai nomi: gli
 * ordinamenti usano solo la struttura (posizioni nel log, ordine di
 * [long-lived]); a parita' di struttura i confronti restano deterministici.
 * Permutando i nomi dei riferimenti, righe e corsie per punta di commit
 * restano identiche (gli alias indistinguibili possono scambiarsi fra loro,
 * come nel resto del modello).
 */
#ifndef GG_LAYOUT_H
#define GG_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>

#include "history.h"

typedef enum {
    LAYOUT_EDGE_PARENT,   /* arco continuo: padre certo/configurato/dedotto */
    LAYOUT_EDGE_SUGGESTED /* arco tratteggiato: padre suggerito (ambiguo) */
} LayoutEdgeKind;

typedef struct {
    size_t branch;   /* indice in History.branches */
    size_t row;      /* riga (posizione topologica della punta), SIZE_MAX se fuori dal log */
    size_t lane;     /* corsia (0 = la piu' a sinistra) */
    size_t label_of; /* remoto etichetta di un locale: indice del locale,
                        SIZE_MAX se ha una corsia propria */
} LayoutNode;

typedef struct {
    size_t child;  /* indice del branch figlio in History.branches */
    size_t parent; /* indice del branch padre in History.branches */
    LayoutEdgeKind kind;
} LayoutEdge;

typedef enum {
    LAYOUT_CEDGE_FIRST_PARENT, /* primo genitore nella stessa corsia */
    LAYOUT_CEDGE_MERGE_PARENT, /* secondo genitore (o successivo) di un merge */
    LAYOUT_CEDGE_BRANCH        /* primo genitore in un'altra corsia (fork) */
} LayoutCommitEdgeKind;

typedef struct {
    size_t commit; /* indice in GitData.commits */
    size_t row;    /* riga (posizione topologica, 0 = piu' recente) */
    size_t lane;   /* corsia del proprietario, o una del blocco anonimo */
    size_t owner;  /* indice del BranchNode proprietario, SIZE_MAX se anonimo */
} LayoutCommit;

typedef struct {
    size_t from; /* commit di partenza (il figlio) */
    size_t to;   /* commit di arrivo (genitore, o punto di diramazione) */
    LayoutCommitEdgeKind kind;
} LayoutCommitEdge;

typedef struct {
    LayoutNode *nodes; /* uno per ogni branch, nello stesso ordine */
    size_t node_count;
    LayoutEdge *edges;
    size_t edge_count;
    size_t rows;  /* numero di righe disponibili (commit del log) */

    LayoutCommit *commits; /* uno per ogni commit, indice = riga */
    size_t commit_count;
    LayoutCommitEdge *cedges;
    size_t cedge_count;
    size_t anon_lane;       /* prima corsia del blocco anonimo, SIZE_MAX se
                               nessun commit e' senza proprietario */
    size_t anon_lane_count; /* corsie del blocco anonimo (la sua larghezza) */

    size_t lanes; /* numero di corsie usate (branch e commit) */
} Layout;

/* Costruisce la disposizione dal modello dei branch. Azzera `out` e ne prende
 * possesso il chiamante, che lo rilascia con layout_free(). */
void layout_build(Layout *out, const History *h);

void layout_free(Layout *l);

/* Nodo del branch di indice `branch_index` (SIZE_MAX se fuori intervallo). */
const LayoutNode *layout_node(const Layout *l, size_t branch_index);

#endif /* GG_LAYOUT_H */

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
 * Gli specchi (remote-tracking che punta allo stesso commit di un branch
 * locale) sono nella stessa famiglia del locale, quindi nella stessa corsia:
 * non producono un arco.
 *
 * FAMIGLIE. I riferimenti con la stessa punta sono la stessa famiglia (stessa
 * genealogia, stessa corsia): gli alias e gli specchi condividono la
 * posizione, cosi' il disegno non li sovrappone in corsie diverse.
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
    size_t branch; /* indice in History.branches */
    size_t row;    /* riga (posizione topologica della punta), SIZE_MAX se fuori dal log */
    size_t lane;   /* corsia (0 = la piu' a sinistra) */
} LayoutNode;

typedef struct {
    size_t child;  /* indice del branch figlio in History.branches */
    size_t parent; /* indice del branch padre in History.branches */
    LayoutEdgeKind kind;
} LayoutEdge;

typedef struct {
    LayoutNode *nodes; /* uno per ogni branch, nello stesso ordine */
    size_t node_count;
    LayoutEdge *edges;
    size_t edge_count;
    size_t rows;  /* numero di righe disponibili (commit del log) */
    size_t lanes; /* numero di corsie usate */
} Layout;

/* Costruisce la disposizione dal modello dei branch. Azzera `out` e ne prende
 * possesso il chiamante, che lo rilascia con layout_free(). */
void layout_build(Layout *out, const History *h);

void layout_free(Layout *l);

/* Nodo del branch di indice `branch_index` (SIZE_MAX se fuori intervallo). */
const LayoutNode *layout_node(const Layout *l, size_t branch_index);

#endif /* GG_LAYOUT_H */

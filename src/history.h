/* history.h - modello dei branch dedotto dalla storia reale.
 *
 * Non dipende da SDL. Non modifica mai il grafo reale: produce solo una
 * descrizione dei branch con padre, punto di diramazione, stato e motivo.
 *
 * Stati del padre:
 *  - CERTO:       fatto ricavato da Git: remote-tracking di un branch locale
 *                 ("stesso branch").
 *  - CONFIGURATO: dichiarato in [parents] di graph.cfg (precedenza assoluta;
 *                 non e' un fatto dedotto dalla storia).
 *  - DEDOTTO:     una sola ipotesi plausibile secondo l'euristica.
 *  - AMBIGUO:     piu' ipotesi non distinguibili (incluso il caso di piu'
 *                 branch sulla stessa punta): non si inventa un padre. Con
 *                 piu' candidati viene indicato un suggerimento
 *                 (suggested_parent + suggestion_basis), che il layout puo'
 *                 mostrare tratteggiato; [parents] in graph.cfg dichiara il
 *                 padre vero e rende il caso CONFIGURATO.
 *  - NESSUNO:     nessun branch padre individuabile.
 */
#ifndef GG_HISTORY_H
#define GG_HISTORY_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "gitdata.h"

typedef enum {
    PARENT_CERTAIN,
    PARENT_CONFIGURED,
    PARENT_DEDUCED,
    PARENT_AMBIGUOUS,
    PARENT_NONE
} ParentState;

/* Perche' un padre e' stato solo suggerito fra i candidati (stato ambiguo): il
 * suggerimento non e' un fatto dedotto, e' un aiuto per il layout. */
typedef enum {
    SUGGESTION_NONE,     /* nessun suggerimento (padre deciso o nessun padre) */
    SUGGESTION_DATE,     /* il candidato col primo commit proprio piu' vecchio */
    SUGGESTION_NAME_TIE, /* a parita' di date: il nome (spareggio dichiarato) */
    SUGGESTION_ANCESTOR  /* nessun fratello piu' vecchio: il primo antenato non
                            fratello sotto il punto di diramazione (il ramo da
                            cui il gruppo di fratelli si e' staccato) */
} SuggestionBasis;

typedef struct {
    char *name;          /* nome breve del branch ("main", "origin/main") */
    char *canonical;     /* nome canonico (per un remoto: nome senza remote) */
    char *mirror_of;     /* remote-tracking che punta allo STESSO commit di un
                             altro branch ("origin/main" == "main"); nomi
                             uguali con punte diverse non bastano */
    size_t ref_index;    /* indice in GitData.refs */
    size_t tip_pos;      /* indice del commit realmente puntato, SIZE_MAX se
                             fuori dal log: ogni riferimento conserva la sua
                             punta, anche quando i nomi coincidono */
    bool is_remote;      /* true per i branch remoti */
    bool is_head;        /* branch corrente */
    bool is_long_lived;
    int long_lived_rank; /* posizione in [long-lived], -1 se assente */

    /* Padre: solo se state != NESSUNO/AMBIGUO; per AMBIGUO resta NULL e i
     * candidati sono elencati in `candidates`/motivation. */
    char *parent;         /* nome del branch padre, o NULL */
    ParentState state;
    char *motivation;     /* spiegazione leggibile, mai NULL */
    bool genealogy_undetermined; /* nessun branch padre deducibile: la
                                     genealogia non e' ricavabile dalla
                                     storia (dichiarato anche nel motivo) */

    char *fork_hash;      /* hash completo del punto di diramazione, o NULL */
    size_t fork_pos;      /* posizione topologica del fork (0 = piu' recente),
                             SIZE_MAX se assente */
    char *first_own_commit; /* primo commit proprio (figlio del punto di
                               diramazione sulla catena dei primi genitori;
                               per l'ordinamento dei figli nel layout), o NULL */
    size_t first_own_pos;   /* posizione topologica, SIZE_MAX se nessuno */

    char **alias_members; /* altri riferimenti (nomi) sulla stessa punta: la
                             stessa genealogia, quindi lo stesso padre dedotto;
                             in ordine alfabetico */
    size_t alias_count;

    char **candidates;    /* candidati (ordinati), solo informativo */
    size_t candidate_count;

    /* Padre suggerito: valido solo con stato AMBIGUO. Non e' un padre
     * dedotto ne' certo: e' il candidato ambiguo piu' antico (col primo
     * commit proprio piu' vecchio del branch, data; a parita', il nome),
     * dichiarato nella motivazione. Per il ramo piu' antico di un gruppo di
     * fratelli, che fra i suoi fratelli non trova nessuno piu' antico, e' il
     * primo antenato non fratello sotto il punto di diramazione (basis
     * SUGGESTION_ANCESTOR): solo i veri radice (nessun antenato non fratello
     * sotto il punto) restano senza suggerimento. Il grafo dei padri suggeriti
     * non ha cicli (ogni salto scende nell'ordine data-nome, e un tronco non
     * ha suggeritori propri). Serve a piazzare il branch (tratteggiato) finche'
     * [parents] non dichiara il padre vero. */
    char *suggested_parent;
    SuggestionBasis suggestion_basis;
} BranchNode;

typedef struct {
    BranchNode *branches;
    size_t branch_count;
    const GitData *data;  /* prestito */
} History;

/* Costruisce il modello dei branch. `cfg` puo' essere NULL (nessun
 * long-lived, nessun override). Ritorna false solo per gravi incoerenze
 * interne (messaggio in err). */
bool history_build(History *out, const GitData *data, const GraphConfig *cfg,
                   char *err, size_t err_size);
void history_free(History *h);

/* Indice del branch con quel nome, o SIZE_MAX se assente. */
size_t history_find_branch(const History *h, const char *name);

#endif /* GG_HISTORY_H */

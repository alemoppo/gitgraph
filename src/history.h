/* history.h - modello dei branch dedotto dalla storia reale.
 *
 * Non dipende da SDL. Non modifica mai il grafo reale: produce solo una
 * descrizione dei branch con padre, punto di diramazione, stato e motivo.
 *
 * Stati del padre:
 *  - CERTO:     dichiarato in [parents] di graph.cfg (o remote-tracking di un
 *               branch locale: "stesso branch").
 *  - DEDOTTO:   una sola ipotesi plausibile secondo l'euristica.
 *  - AMBIGUO:   piu' ipotesi non distinguibili, oppure due branch puntano allo
 *               stesso commit: non si inventa un padre.
 *  - NESSUNO:   nessun altro branch condivide la storia (radice indipendente).
 */
#ifndef GG_HISTORY_H
#define GG_HISTORY_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "gitdata.h"

typedef enum {
    PARENT_CERTAIN,
    PARENT_DEDUCED,
    PARENT_AMBIGUOUS,
    PARENT_NONE
} ParentState;

typedef struct {
    char *name;          /* nome breve del branch ("main", "origin/main") */
    char *canonical;     /* nome canonico del gruppo (es. "feature/x") */
    char *mirror_of;     /* se remote-tracking di un altro branch, il suo nome */
    size_t ref_index;    /* indice in GitData.refs */
    bool is_remote;      /* true per i branch remoti */
    bool is_head;        /* branch corrente */
    bool is_long_lived;
    int long_lived_rank; /* posizione in [long-lived], -1 se assente */

    /* Padre: solo se state != NESSUNO/AMBIGUO; per AMBIGUO resta NULL e i
     * candidati sono elencati in `candidates`/motivation. */
    char *parent;         /* nome del branch padre, o NULL */
    ParentState state;
    char *motivation;     /* spiegazione leggibile, mai NULL */

    char *fork_hash;      /* hash completo del punto di diramazione, o NULL */
    size_t fork_pos;      /* posizione topologica del fork (0 = piu' recente),
                             SIZE_MAX se assente */
    char *first_exclusive_hash; /* primo commit esclusivo (per l'ordinamento
                                   dei figli in layout), o NULL */
    size_t first_exclusive_pos; /* posizione topologica, SIZE_MAX se nessuno */

    char **candidates;    /* candidati (ordinati), solo informativo */
    size_t candidate_count;
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

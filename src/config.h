/* config.h - configurazione del grafo (graph.cfg).
 *
 * Fase 1: solo le strutture dati e i default impliciti. Il parsing e la
 * validazione del file graph.cfg sono implementati in config.c.
 */
#ifndef GG_CONFIG_H
#define GG_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *branch; /* nome del branch (nessuna wildcard) */
    char *parent; /* nome del branch padre dichiarato */
} ParentOverride;

typedef struct {
    /* [long-lived]: lista ordinata; le voci possono contenere wildcard '*'
     * (gia' espanse sui branch esistenti, nell'ordine della lista). */
    char **long_lived;
    size_t long_lived_count;

    /* [parents]: override manuali, con precedenza sull'euristica. */
    ParentOverride *overrides;
    size_t override_count;
} GraphConfig;

/* Rilascia il contenuto e azzera la struttura. */
void config_free(GraphConfig *cfg);

/* Default impliciti: se la lista [long-lived] manca, il tronco e' `main`
 * oppure `master` (in quest'ordine), se esiste tra `branch_names`. */
void config_set_implicit_trunk(GraphConfig *cfg, char *const *branch_names,
                               size_t branch_count);

/* Numero di posizione (0-based) del branch long-lived, o -1 se non lo e'. */
int config_long_lived_rank(const GraphConfig *cfg, const char *branch_name);

#endif /* GG_CONFIG_H */

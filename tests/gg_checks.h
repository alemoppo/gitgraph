/* gg_checks.h - controlli di proprieta' condivisi dai test automatici.
 *
 * Da includere DOPO la definizione della macro CHECK(cond, ...): la
 * proprieta' la esegue usando la macro del test chiamante, cosi' ogni test
 * riporta i fallimenti nel proprio formato e con il proprio conteggio.
 *
 * PROPRIETA' DELLA GENEALOGIA (invariante dei fratelli): ogni ramo che ha un
 * tronco fra gli antenati - un commit della propria catena dei primi
 * genitori che sta anche sulla catena di un long-lived - ha un padre o un
 * padre suggerito. La catena dei padri suggeriti, da un ramo cosi' nato,
 * senza cicli finisce su un ramo la cui genealogia e' gia' risolta (padre
 * assegnato o tronco): un ramo con antenato in un tronco non resta mai
 * senza puntamento. I veri radice (nessun antenato in un tronco, per
 * esempio un gruppo di fratelli isolato dalla nascita) restano senza
 * suggerimento: non c'e' nessun tronco a cui arrivare.
 */
#ifndef GG_CHECKS_H
#define GG_CHECKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* genitore sulla catena dei primi genitori, SIZE_MAX se non c'e' */
static size_t gg_primo_genitore(const GitData *d, size_t cur)
{
    const GitCommit *c = &d->commits[cur];
    return c->parent_count ? c->parent_idx[0] : (size_t)-1;
}

static void gg_check_trunk_ancestry(const History *h, const GitData *d,
                                    const char *etichetta)
{
    size_t cc = d->commit_count;
    uint32_t *tronco = (uint32_t *)malloc((cc ? cc : 1) * sizeof(uint32_t));
    if (!tronco) {
        CHECK(false, "%s: memoria esaurita nella proprieta' del tronco",
              etichetta);
        return;
    }
    memset(tronco, 0, (cc ? cc : 1) * sizeof(uint32_t));

    /* marchia le catene dei primi genitori di ogni tronco: se un commit di
     * una catena di ramo e' marchiato, il ramo discende da quel tronco */
    uint32_t gen = 0;
    for (size_t i = 0; i < h->branch_count; i++) {
        const BranchNode *t = &h->branches[i];
        if (!t->is_long_lived || t->tip_pos == (size_t)-1)
            continue;
        gen++;
        for (size_t cur = t->tip_pos; cur != (size_t)-1;
             cur = gg_primo_genitore(d, cur))
            tronco[cur] = gen;
    }

    for (size_t i = 0; i < h->branch_count; i++) {
        const BranchNode *b = &h->branches[i];
        if (b->is_long_lived || b->tip_pos == (size_t)-1)
            continue;
        bool ha_tronco = false;
        for (size_t cur = b->tip_pos; cur != (size_t)-1 && !ha_tronco;
             cur = gg_primo_genitore(d, cur)) {
            if (tronco[cur])
                ha_tronco = true;
        }
        if (!ha_tronco)
            continue; /* vero radice: nessun antenato in un tronco */

        CHECK(b->parent != NULL || b->suggested_parent != NULL,
              "%s: '%s' ha un tronco fra gli antenati ma non ha ne' padre "
              "ne' padre suggerito (stato %d)",
              etichetta, b->name, (int)b->state);
        if (!b->suggested_parent)
            continue;

        /* dai suggeriti si deve arrivare a genealogia gia' risolta, senza
         * cicli: ogni passo va su un candidato piu' antico */
        size_t cur = i;
        size_t passi = 0;
        while (passi < h->branch_count && h->branches[cur].suggested_parent) {
            size_t next =
                history_find_branch(h, h->branches[cur].suggested_parent);
            if (next == (size_t)-1) {
                CHECK(false,
                      "%s: '%s' suggerisce '%s' che non esiste fra i branch",
                      etichetta, h->branches[cur].name,
                      h->branches[cur].suggested_parent);
                break;
            }
            cur = next;
            passi++;
        }
        CHECK(passi < h->branch_count,
              "%s: ciclo nella catena dei padri suggeriti partendo da '%s'",
              etichetta, b->name);
        if (passi < h->branch_count) {
            const BranchNode *term = &h->branches[cur];
            CHECK(term->is_long_lived || term->parent != NULL,
                  "%s: la catena dei suggeriti di '%s' finisce su '%s' senza "
                  "genealogia risolta (stato %d)",
                  etichetta, b->name, term->name, (int)term->state);
        }
    }
    free(tronco);
}

#endif

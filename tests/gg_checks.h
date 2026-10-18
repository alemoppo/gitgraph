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

#include "layout.h"

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

/* PROPRIETA' DELLA DISPOSIZIONE (fase 3): il layout e' coerente col modello.
 *  - un nodo per branch, nella stessa riga della punta (o primo commit
 *    proprio se la punta e' fuori dal log);
 *  - corsia dentro [0, lanes); i riferimenti sulla stessa punta (stessa
 *    famiglia) condividono riga e corsia;
 *  - ogni branch ha al piu' un arco uscente: continuo verso il padre se il
 *    padre e' un altro ramo, tratteggiato verso il suggerito se il caso e'
 *    ambiguo, nessuno altrimenti (gli specchi sono lo stesso ramo);
 *  - il grafo degli archi non ha cicli. */
static void gg_check_layout(const Layout *l, const History *h, const char *etichetta)
{
    size_t bc = h->branch_count;
    CHECK(l->node_count == bc, "%s: layout con %zu nodi per %zu branch",
          etichetta, l->node_count, bc);
    if (!h->data)
        return;
    CHECK(l->rows == h->data->commit_count, "%s: layout.rows = %zu, commit = %zu",
          etichetta, l->rows, h->data->commit_count);

    for (size_t i = 0; i < l->node_count; i++) {
        const LayoutNode *nd = &l->nodes[i];
        const BranchNode *b = &h->branches[i];
        size_t row = b->tip_pos != (size_t)-1
                         ? b->tip_pos
                         : (b->first_own_pos != (size_t)-1 ? b->first_own_pos
                                                           : (size_t)-1);
        CHECK(nd->branch == i, "%s: nodo %zu riferisce branch %zu", etichetta, i,
              nd->branch);
        CHECK(nd->row == row, "%s: '%s' riga %zu, attesa %zu", etichetta, b->name,
              nd->row, row);
        CHECK(l->lanes == 0 || nd->lane < l->lanes,
              "%s: '%s' corsia %zu fuori da %zu", etichetta, b->name, nd->lane,
              l->lanes);
        /* stessa punta = stessa famiglia = stessa posizione */
        for (size_t k = 0; k < i; k++) {
            if (b->tip_pos != (size_t)-1 && h->branches[k].tip_pos == b->tip_pos)
                CHECK(l->nodes[k].lane == nd->lane &&
                          l->nodes[k].row == nd->row,
                      "%s: '%s' e '%s' (stessa punta) in posizioni diverse",
                      etichetta, b->name, h->branches[k].name);
        }
    }

    size_t *par = (size_t *)malloc((bc ? bc : 1) * sizeof(size_t));
    if (!par) {
        CHECK(false, "%s: memoria esaurita nella proprieta' del layout",
              etichetta);
        return;
    }
    for (size_t i = 0; i < bc; i++)
        par[i] = (size_t)-1;

    for (size_t i = 0; i < bc; i++) {
        const BranchNode *b = &h->branches[i];
        const char *want = NULL;
        LayoutEdgeKind want_kind = LAYOUT_EDGE_PARENT;
        if (b->parent) {
            want = b->parent;
        } else if (b->state == PARENT_AMBIGUOUS && b->suggested_parent) {
            want = b->suggested_parent;
            want_kind = LAYOUT_EDGE_SUGGESTED;
        }
        size_t out = 0;
        bool ok = false;
        for (size_t e = 0; e < l->edge_count; e++) {
            if (l->edges[e].child != i)
                continue;
            out++;
            par[i] = l->edges[e].parent;
            size_t j = want ? history_find_branch(h, want) : (size_t)-1;
            if (j != (size_t)-1 && j != i && l->edges[e].parent == j &&
                l->edges[e].kind == want_kind)
                ok = true;
        }
        CHECK(out <= 1, "%s: '%s' ha %zu archi uscenti", etichetta, b->name, out);
        if (!want) {
            CHECK(out == 0, "%s: '%s' senza padre ma con un arco", etichetta,
                  b->name);
            continue;
        }
        size_t j = history_find_branch(h, want);
        bool stesso_ramo = (j != (size_t)-1 && j != i &&
                            b->tip_pos != (size_t)-1 &&
                            h->branches[j].tip_pos == b->tip_pos);
        if (j == (size_t)-1 || j == i || stesso_ramo)
            CHECK(out == 0, "%s: '%s' arco verso un padre che e' lo stesso ramo",
                  etichetta, b->name);
        else
            CHECK(ok, "%s: '%s' privo dell'arco %s verso '%s'", etichetta, b->name,
                  want_kind == LAYOUT_EDGE_SUGGESTED ? "suggerito" : "continuo",
                  want);
    }

    /* nessun ciclo fra gli archi */
    for (size_t i = 0; i < bc; i++) {
        size_t cur = i;
        size_t passi = 0;
        while (par[cur] != (size_t)-1 && passi <= bc) {
            cur = par[cur];
            passi++;
        }
        CHECK(passi <= bc, "%s: ciclo negli archi del layout partendo dal "
                           "branch %zu",
              etichetta, i);
    }
    free(par);
}

#endif

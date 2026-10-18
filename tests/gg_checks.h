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

/* CATENE ANONIME. I commit senza proprietario formano catene (componenti
 * connesse unite dai primi genitori). In una stessa corsia del blocco anonimo i
 * loro intervalli di righe non devono sovrapporsi: cosi' nessun arco passa
 * sopra il nodo di un'altra catena. */
static void gg_check_anon_chains(const Layout *l, const GitData *d,
                                 const char *etichetta)
{
    size_t cc = d->commit_count;
    if (l->anon_lane_count == 0 || l->anon_lane == (size_t)-1 || cc == 0 ||
        !l->commits)
        return;
    size_t na = 0;
    for (size_t ci = 0; ci < cc; ci++)
        if (l->commits[ci].owner == (size_t)-1)
            na++;
    if (na == 0)
        return;
    size_t *alist = (size_t *)malloc(na * sizeof(size_t));
    size_t *uf = (size_t *)malloc(cc * sizeof(size_t));
    size_t *lo = (size_t *)malloc(cc * sizeof(size_t));
    size_t *hi = (size_t *)malloc(cc * sizeof(size_t));
    if (!alist || !uf || !lo || !hi) {
        free(alist);
        free(uf);
        free(lo);
        free(hi);
        CHECK(false,
              "%s: memoria esaurita nella proprieta' delle catene anonime",
              etichetta);
        return;
    }
    for (size_t ci = 0; ci < cc; ci++) {
        uf[ci] = ci;
        lo[ci] = cc;
        hi[ci] = 0;
    }
    size_t k = 0;
    for (size_t ci = 0; ci < cc; ci++) {
        if (l->commits[ci].owner != (size_t)-1)
            continue;
        alist[k++] = ci;
        if (d->commits[ci].parent_count == 0)
            continue;
        size_t p = d->commits[ci].parent_idx[0];
        if (p == (size_t)-1 || p >= cc || l->commits[p].owner != (size_t)-1)
            continue;
        size_t a = p;
        while (uf[a] != a) {
            uf[a] = uf[uf[a]];
            a = uf[a];
        }
        size_t b = ci;
        while (uf[b] != b) {
            uf[b] = uf[uf[b]];
            b = uf[b];
        }
        if (a != b)
            uf[a] = b;
    }
    /* intervallo di ogni catena: dal commit piu' recente (minimo) al genitore
     * del piu' vecchio */
    for (size_t i = 0; i < na; i++) {
        size_t ci = alist[i];
        size_t r = ci;
        while (uf[r] != r) {
            uf[r] = uf[uf[r]];
            r = uf[r];
        }
        if (ci < lo[r])
            lo[r] = ci;
        size_t e = ci;
        if (d->commits[ci].parent_count > 0) {
            size_t p = d->commits[ci].parent_idx[0];
            if (p != (size_t)-1 && p < cc)
                e = p;
        }
        if (e > hi[r])
            hi[r] = e;
    }
    for (size_t i = 0; i < na; i++) {
        size_t ci = alist[i];
        size_t ri = ci;
        while (uf[ri] != ri) {
            uf[ri] = uf[uf[ri]];
            ri = uf[ri];
        }
        for (size_t j = i + 1; j < na; j++) {
            size_t cj = alist[j];
            if (l->commits[cj].lane != l->commits[ci].lane)
                continue;
            size_t rj = cj;
            while (uf[rj] != rj) {
                uf[rj] = uf[uf[rj]];
                rj = uf[rj];
            }
            if (ri == rj)
                continue;
            CHECK(hi[ri] < lo[rj] || hi[rj] < lo[ri],
                  "%s: due catene anonime nella corsia %zu con intervalli "
                  "sovrapposti ([%zu,%zu] e [%zu,%zu])",
                  etichetta, l->commits[ci].lane, lo[ri], hi[ri], lo[rj],
                  hi[rj]);
        }
    }
    free(alist);
    free(uf);
    free(lo);
    free(hi);
}

/* PROPRIETA' DELLA DISPOSIZIONE (fase 3/4): il layout e' coerente col modello.
 *  - un nodo per branch, nella stessa riga della punta (o primo commit
 *    proprio se la punta e' fuori dal log);
 *  - corsia dentro [0, lanes); i riferimenti sulla stessa punta (stessa
 *    famiglia) condividono riga e corsia, tranne un'etichetta remota che sta
 *    sulla corsia del locale che etichetta (label_of);
 *  - ogni branch ha al piu' un arco uscente: continuo verso il padre se il
 *    padre e' un altro ramo, tratteggiato verso il suggerito se il caso e'
 *    ambiguo, nessuno altrimenti (gli specchi sono lo stesso ramo);
 *  - il grafo degli archi non ha cicli;
 *  - ogni commit ha una cella (riga = posizione topologica; corsia del
 *    proprietario, o del blocco anonimo se nessun ramo lo reclama) e un solo
 *    arco per genitore: di catena se il genitore e' nella stessa corsia, di
 *    diramazione se salta in un'altra corsia (e in tal caso il genitore e' il
 *    primo genitore reale), di merge dal secondo genitore in poi;
 *  - le catene anonime di una stessa corsia hanno intervalli di righe
 *    disgiunti: nessun arco passa sopra il nodo di un'altra catena. */
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
        /* un'etichetta remota condivide la corsia del locale che etichetta */
        if (nd->label_of != (size_t)-1) {
            CHECK(nd->label_of < bc, "%s: '%s' etichetta del branch %zu inesistente",
                  etichetta, b->name, nd->label_of);
            if (nd->label_of < bc)
                CHECK(l->nodes[nd->label_of].lane == nd->lane,
                      "%s: '%s' etichetta di '%s' ma in corsia %zu invece di %zu",
                      etichetta, b->name, h->branches[nd->label_of].name,
                      nd->lane, l->nodes[nd->label_of].lane);
        }
        /* stessa punta = stessa riga; la stessa corsia vale per tutti tranne
         * un'etichetta remota, che sta sulla corsia del locale etichettato */
        for (size_t k = 0; k < i; k++) {
            if (b->tip_pos != (size_t)-1 && h->branches[k].tip_pos == b->tip_pos) {
                CHECK(l->nodes[k].row == nd->row,
                      "%s: '%s' e '%s' (stessa punta) in righe diverse",
                      etichetta, b->name, h->branches[k].name);
                if (nd->label_of == (size_t)-1 && l->nodes[k].label_of == (size_t)-1)
                    CHECK(l->nodes[k].lane == nd->lane,
                          "%s: '%s' e '%s' (stessa punta) in corsie diverse",
                          etichetta, b->name, h->branches[k].name);
            }
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
        /* stesso ramo = stessa famiglia: stessa punta, oppure (etichetta
         * remota) la stessa corsia del locale etichettato */
        bool stesso_ramo =
            (j != (size_t)-1 && j != i &&
             ((b->tip_pos != (size_t)-1 && h->branches[j].tip_pos == b->tip_pos) ||
              l->nodes[i].lane == l->nodes[j].lane));
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

    /* COMMIT: una cella per commit, riga = posizione topologica, corsia del
     * proprietario (o del blocco anonimo); gli archi tra commit hanno i tre
     * tipi dichiarati, uno solo per genitore, coerenti col grafo reale. */
    const GitData *d = h->data;
    CHECK(l->commit_count == d->commit_count,
          "%s: layout con %zu commit per %zu nel log", etichetta,
          l->commit_count, d->commit_count);
    size_t fp_attesi = 0, mp_attesi = 0, br_attesi = 0;
    for (size_t ci = 0; ci < d->commit_count; ci++) {
        const GitCommit *gc = &d->commits[ci];
        for (size_t k = 0; k < gc->parent_count; k++) {
            size_t p = gc->parent_idx[k];
            if (p == (size_t)-1 || p >= d->commit_count)
                continue;
            if (k > 0)
                mp_attesi++;
            else if (l->commits[ci].lane == l->commits[p].lane)
                fp_attesi++;
            else
                br_attesi++;
        }
    }
    size_t fp = 0, mp = 0, br = 0;
    for (size_t ci = 0; ci < l->commit_count && ci < d->commit_count; ci++) {
        const LayoutCommit *cell = &l->commits[ci];
        CHECK(cell->commit == ci,
              "%s: cella %zu riferisce il commit %zu", etichetta, ci, cell->commit);
        CHECK(cell->row == ci, "%s: commit %zu alla riga %zu, attesa %zu",
              etichetta, ci, cell->row, ci);
        CHECK(l->lanes == 0 || cell->lane < l->lanes,
              "%s: commit %zu in corsia %zu fuori da %zu", etichetta, ci,
              cell->lane, l->lanes);
        if (h->commit_owner)
            CHECK(cell->owner == h->commit_owner[ci],
                  "%s: commit %zu proprietario %zu, atteso %zu", etichetta, ci,
                  cell->owner, h->commit_owner[ci]);
        if (cell->owner == (size_t)-1) {
            CHECK(l->anon_lane != (size_t)-1 &&
                      cell->lane >= l->anon_lane &&
                      cell->lane < l->anon_lane + l->anon_lane_count,
                  "%s: commit %zu senza proprietario in corsia %zu, fuori dal "
                  "blocco anonimo",
                  etichetta, ci, cell->lane);
        } else {
            CHECK(cell->owner < bc, "%s: commit %zu proprietario %zu inesistente",
                  etichetta, ci, cell->owner);
            if (cell->owner < bc)
                CHECK(l->nodes[cell->owner].lane == cell->lane,
                      "%s: commit %zu in corsia %zu ma il proprietario '%s' in "
                      "%zu", etichetta, ci, cell->lane,
                      h->branches[cell->owner].name, l->nodes[cell->owner].lane);
            /* nessun nodo-branch sta in una corsia del blocco anonimo */
            CHECK(!(cell->lane >= l->anon_lane &&
                    cell->lane < l->anon_lane + l->anon_lane_count),
                  "%s: il commit %zu (di '%s') usa una corsia anonima %zu",
                  etichetta, ci, h->branches[cell->owner].name, cell->lane);
        }
        /* la catena dei primi genitori resta sulla stessa corsia: finche' il
         * proprietario non cambia, o finche' la catena resta anonima */
        if (d->commits[ci].parent_count > 0) {
            size_t p = d->commits[ci].parent_idx[0];
            if (p != (size_t)-1 && p < d->commit_count) {
                bool stessa = cell->owner != (size_t)-1 &&
                              l->commits[p].owner == cell->owner;
                if (cell->owner == (size_t)-1 && l->commits[p].owner == (size_t)-1)
                    stessa = true;
                if (stessa)
                    CHECK(l->commits[p].lane == cell->lane,
                          "%s: commit %zu e primo genitore %zu con lo stesso "
                          "proprietario ma corsie %zu e %zu", etichetta, ci, p,
                          cell->lane, l->commits[p].lane);
            }
        }
    }
    for (size_t ei = 0; ei < l->cedge_count; ei++) {
        const LayoutCommitEdge *ce = &l->cedges[ei];
        CHECK(ce->from < d->commit_count && ce->to < d->commit_count,
              "%s: arco tra commit fuori dal log (%zu -> %zu)", etichetta,
              ce->from, ce->to);
        if (ce->from >= d->commit_count || ce->to >= d->commit_count)
            continue;
        const GitCommit *gc = &d->commits[ce->from];
        if (ce->kind == LAYOUT_CEDGE_FIRST_PARENT) {
            fp++;
            CHECK(gc->parent_count > 0 && gc->parent_idx[0] == ce->to,
                  "%s: arco di primo genitore %zu -> %zu non e' il primo "
                  "genitore", etichetta, ce->from, ce->to);
            CHECK(l->commits[ce->from].lane == l->commits[ce->to].lane,
                  "%s: arco di primo genitore %zu -> %zu fra corsie diverse "
                  "(%zu e %zu)", etichetta, ce->from, ce->to,
                  l->commits[ce->from].lane, l->commits[ce->to].lane);
        } else if (ce->kind == LAYOUT_CEDGE_MERGE_PARENT) {
            mp++;
            bool trovato = false;
            for (size_t k = 1; k < gc->parent_count && !trovato; k++)
                if (gc->parent_idx[k] == ce->to)
                    trovato = true;
            CHECK(trovato, "%s: arco di merge %zu -> %zu non e' un secondo "
                           "genitore", etichetta, ce->from, ce->to);
        } else {
            br++;
            CHECK(gc->parent_count > 0 && gc->parent_idx[0] == ce->to,
                  "%s: arco di diramazione %zu -> %zu non e' il primo genitore",
                  etichetta, ce->from, ce->to);
            CHECK(l->commits[ce->from].lane != l->commits[ce->to].lane,
                  "%s: arco di diramazione %zu -> %zu nella stessa corsia %zu",
                  etichetta, ce->from, ce->to, l->commits[ce->from].lane);
        }
    }
    CHECK(fp == fp_attesi, "%s: %zu archi di primo genitore, attesi %zu",
          etichetta, fp, fp_attesi);
    CHECK(mp == mp_attesi, "%s: %zu archi di merge, attesi %zu", etichetta, mp,
          mp_attesi);
    CHECK(br == br_attesi, "%s: %zu archi di diramazione, attesi %zu", etichetta,
          br, br_attesi);
    gg_check_anon_chains(l, d, etichetta);
}

#endif

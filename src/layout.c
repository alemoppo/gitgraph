/* layout.c - vedi layout.h
 *
 * Tutto lavora su indici precalcolati: nessuna scansione di nomi nei passi
 * interni (history_find_branch compare solo quando si risolve il nome di un
 * padre, una volta per famiglia, e per l'etichetta remota). Gli ordinamenti
 * usano chiavi strutturali (posizioni nel log, ordine di [long-lived]), mai
 * l'ordine alfabetico, cosi' la disposizione non cambia se si rinominano i
 * riferimenti.
 */
#include "layout.h"

#include "util.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define NO_IDX ((size_t)-1)

/* Una famiglia: i riferimenti con la stessa genealogia (stessa corsia). Per
 * la disposizione bastano il rappresentante e i dati strutturali. */
typedef struct {
    size_t *members;   /* indici in History.branches */
    size_t count;
    size_t rep;        /* primo indice della famiglia (deterministico) */
    bool trunk;
    int rank;          /* posizione in [long-lived], INT_MAX se non tronco */
    size_t row;        /* riga del nodo (posizione della punta) */
    size_t birth;      /* primo commit proprio (o punta): ordina i fratelli */
    size_t parent;     /* famiglia padre, NO_IDX se radice */
    LayoutEdgeKind kind;
    size_t lane;
    bool assigned;
} Fam;

/* Le famiglie sono ordinate con qsort, che non ha un contesto: il vettore e'
 * tenuto in una variabile di file. La disposizione si costruisce in un solo
 * thread, quindi non serve altro. */
static const Fam *g_fams;

/* Fratelli: prima il piu' "vecchio" del gruppo. Le posizioni nel log crescono
 * verso il passato, quindi un indice maggiore e' un ramo nato prima. */
static int cmp_sibling(const void *pa, const void *pb)
{
    size_t a = *(const size_t *)pa;
    size_t b = *(const size_t *)pb;
    const Fam *fa = &g_fams[a];
    const Fam *fb = &g_fams[b];
    if (fa->birth != fb->birth)
        return fa->birth > fb->birth ? -1 : 1;
    if (fa->row != fb->row)
        return fa->row > fb->row ? -1 : 1;
    if (fa->rep != fb->rep)
        return fa->rep < fb->rep ? -1 : 1;
    return 0;
}

/* Radici: i tronchi per primi (nell'ordine di [long-lived]), poi gli altri
 * rami senza padre (dal piu' recente). */
static int cmp_root(const void *pa, const void *pb)
{
    size_t a = *(const size_t *)pa;
    size_t b = *(const size_t *)pb;
    const Fam *fa = &g_fams[a];
    const Fam *fb = &g_fams[b];
    if (fa->trunk != fb->trunk)
        return fa->trunk ? -1 : 1;
    if (fa->trunk && fa->rank != fb->rank)
        return fa->rank < fb->rank ? -1 : 1;
    if (fa->row != fb->row)
        return fa->row < fb->row ? -1 : 1;
    if (fa->rep != fb->rep)
        return fa->rep < fb->rep ? -1 : 1;
    return 0;
}

/* Riga del nodo di un branch: la punta; se la punta e' fuori dal log, il
 * primo commit proprio; altrimenti resta NO_IDX (nessuna posizione). */
static size_t branch_row(const BranchNode *b)
{
    if (b->tip_pos != NO_IDX)
        return b->tip_pos;
    if (b->first_own_pos != NO_IDX)
        return b->first_own_pos;
    return NO_IDX;
}

/* Cammino a soli primi genitori da `from` a `to` (in una delle due direzioni
 * si prova invertendo). Serve all'etichetta remota: la punta del remoto e
 * quella del locale stanno sulla stessa linea. */
static bool chain_reaches(const GitData *d, size_t from, size_t to)
{
    if (!d || from == NO_IDX || to == NO_IDX || from >= d->commit_count ||
        to >= d->commit_count)
        return false;
    for (size_t cur = from; cur != NO_IDX && cur < d->commit_count;) {
        if (cur == to)
            return true;
        const GitCommit *c = &d->commits[cur];
        cur = c->parent_count ? c->parent_idx[0] : NO_IDX;
    }
    return false;
}

/* ---------- catene di commit senza proprietario --------------------------- */

/* Catena anonima: intervallo di righe [top, end] (dal commit piu' recente al
 * genitore del piu' vecchio) e corsia assegnata nel blocco anonimo. */
typedef struct {
    size_t top;
    size_t end;
    size_t lane;
} AnonChain;

static const AnonChain *g_anon; /* per qsort (qsort non ha un contesto) */

/* In ordine di riga del commit piu' recente; `top` e' l'indice minimo della
 * catena, quindi la chiave e' unica e l'ordine e' totale. */
static int cmp_anon_top(const void *pa, const void *pb)
{
    size_t a = *(const size_t *)pa;
    size_t b = *(const size_t *)pb;
    if (g_anon[a].top != g_anon[b].top)
        return g_anon[a].top < g_anon[b].top ? -1 : 1;
    return 0;
}

static size_t uf_find(size_t *uf, size_t x)
{
    while (uf[x] != x) {
        uf[x] = uf[uf[x]];
        x = uf[x];
    }
    return x;
}

/* Raggruppa i commit senza proprietario in catene (componenti connesse dai
 * primi genitori) e assegna le corsie del blocco anonimo con colorazione di
 * intervalli, in ordine di riga. Riempie lane_of[ci] con la corsia relativa al
 * blocco (indice 0..n-1) per i commit anonimi, NO_IDX per gli altri, e ritorna
 * il numero di corsie usate.
 *
 * Complessita': O(commit + catene * corsie). Le catene anonime sono rare (un
 * ramo cancellato dopo il merge) e le corsie del blocco restano poche, quindi
 * la ricerca lineare della prima corsia libera e' adeguata. */
static size_t anon_chain_lanes(const GitData *d, size_t cc, const size_t *owners,
                               size_t *lane_of)
{
    for (size_t ci = 0; ci < cc; ci++)
        lane_of[ci] = NO_IDX;
    if (!d || !owners || cc == 0)
        return 0;
    bool any = false;
    for (size_t ci = 0; ci < cc && !any; ci++)
        if (owners[ci] == NO_IDX)
            any = true;
    if (!any)
        return 0;

    /* unione dei commit anonimi consecutivi nel primo genitore */
    size_t *uf = xmalloc(cc * sizeof(size_t));
    for (size_t ci = 0; ci < cc; ci++)
        uf[ci] = ci;
    for (size_t ci = 0; ci < cc; ci++) {
        if (owners[ci] != NO_IDX || d->commits[ci].parent_count == 0)
            continue;
        size_t p = d->commits[ci].parent_idx[0];
        if (p == NO_IDX || p >= cc || owners[p] != NO_IDX)
            continue;
        size_t a = uf_find(uf, ci);
        size_t b = uf_find(uf, p);
        if (a != b)
            uf[a] = b;
    }

    /* intervalli: il primo commit incontrato in ordine crescente e' il piu'
     * recente (indice minimo); la fine e' la riga del genitore del piu' vecchio
     * (o la riga stessa se il genitore non c'e' o e' fuori dal log) */
    AnonChain *ch = xmalloc(cc * sizeof(AnonChain));
    size_t *slot = xmalloc(cc * sizeof(size_t));
    for (size_t ci = 0; ci < cc; ci++)
        slot[ci] = NO_IDX;
    size_t nch = 0;
    for (size_t ci = 0; ci < cc; ci++) {
        if (owners[ci] != NO_IDX)
            continue;
        size_t r = uf_find(uf, ci);
        if (slot[r] == NO_IDX) {
            slot[r] = nch;
            ch[nch].top = ci;
            ch[nch].end = ci;
            ch[nch].lane = NO_IDX;
            nch++;
        }
        size_t e = ci;
        if (d->commits[ci].parent_count > 0) {
            size_t p = d->commits[ci].parent_idx[0];
            if (p != NO_IDX && p < cc)
                e = p;
        }
        if (e > ch[slot[r]].end)
            ch[slot[r]].end = e;
    }

    size_t *order = xmalloc(nch * sizeof(size_t));
    for (size_t k = 0; k < nch; k++)
        order[k] = k;
    g_anon = ch;
    qsort(order, nch, sizeof(size_t), cmp_anon_top);

    /* greedy: la prima corsia il cui ultimo intervallo finisce prima che questa
     * catena cominci (corsie con intervalli sovrapposti restano distinte) */
    size_t *lane_end = xmalloc(nch * sizeof(size_t));
    size_t used = 0;
    for (size_t k = 0; k < nch; k++) {
        AnonChain *c = &ch[order[k]];
        size_t lane = NO_IDX;
        for (size_t j = 0; j < used; j++) {
            if (lane_end[j] < c->top) {
                lane = j;
                break;
            }
        }
        if (lane == NO_IDX)
            lane = used++;
        lane_end[lane] = c->end;
        c->lane = lane;
    }
    for (size_t ci = 0; ci < cc; ci++) {
        if (owners[ci] != NO_IDX)
            continue;
        lane_of[ci] = ch[slot[uf_find(uf, ci)]].lane;
    }

    free(uf);
    free(slot);
    free(order);
    free(lane_end);
    free(ch);
    return used;
}

void layout_build(Layout *out, const History *h)
{
    memset(out, 0, sizeof *out);
    out->anon_lane = NO_IDX;
    out->anon_lane_count = 0;
    size_t n = h->branch_count;
    out->rows = h->data ? h->data->commit_count : 0;
    out->node_count = n;
    out->nodes = xcalloc(n ? n : 1, sizeof(LayoutNode));
    if (n == 0)
        return;

    size_t cc = out->rows;
    const size_t *owners = h->data ? h->commit_owner : NULL;

    /* --- famiglie: stessa genealogia, stessa corsia ----------------------- */
    size_t *fam_of = xmalloc(n * sizeof(size_t));
    size_t *label_of = xmalloc(n * sizeof(size_t));
    for (size_t i = 0; i < n; i++)
        label_of[i] = NO_IDX;
    size_t *seen = xmalloc((cc ? cc : 1) * sizeof(size_t));
    for (size_t k = 0; k < cc; k++)
        seen[k] = NO_IDX;

    Fam *fams = xmalloc(n * sizeof(Fam));
    size_t fam_count = 0;
    /* due passate: prima i rami locali (creano le famiglie), poi i remoti,
     * che possono essere solo etichette di un locale */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < n; i++) {
            const BranchNode *b = &h->branches[i];
            if ((pass == 0) == b->is_remote)
                continue;
            size_t tip = b->tip_pos;
            size_t f = NO_IDX;
            if (b->is_remote) {
                /* etichetta: il locale con lo stesso nome, se la punta del
                 * remoto sta sulla sua catena dei primi genitori */
                size_t li = history_find_branch(h, b->canonical);
                if (li != NO_IDX && li != i && !h->branches[li].is_remote &&
                    (chain_reaches(h->data, tip, h->branches[li].tip_pos) ||
                     chain_reaches(h->data, h->branches[li].tip_pos, tip))) {
                    f = fam_of[li];
                    label_of[i] = li;
                }
            }
            if (f == NO_IDX) {
                if (tip != NO_IDX && tip < cc && seen[tip] != NO_IDX) {
                    f = seen[tip];
                } else {
                    f = fam_count++;
                    memset(&fams[f], 0, sizeof fams[f]);
                    fams[f].rep = i;
                    fams[f].rank = INT_MAX;
                    fams[f].parent = NO_IDX;
                    fams[f].kind = LAYOUT_EDGE_PARENT;
                    if (tip != NO_IDX && tip < cc)
                        seen[tip] = f;
                }
            }
            fam_of[i] = f;
            fams[f].members =
                xrealloc(fams[f].members, (fams[f].count + 1) * sizeof(size_t));
            fams[f].members[fams[f].count++] = i;
            if (b->is_long_lived) {
                fams[f].trunk = true;
                if (b->long_lived_rank >= 0 && b->long_lived_rank < fams[f].rank)
                    fams[f].rank = b->long_lived_rank;
            }
        }
    }

    /* dati strutturali e padre "esterno" (un membro il cui padre esce dalla
     * famiglia: gli specchi indicano un locale della stessa famiglia) */
    for (size_t f = 0; f < fam_count; f++) {
        const BranchNode *b = &h->branches[fams[f].rep];
        fams[f].row = branch_row(b);
        fams[f].birth = (b->first_own_pos != NO_IDX) ? b->first_own_pos
                                                     : fams[f].row;
        size_t ext = NO_IDX;
        size_t sug = NO_IDX;
        for (size_t k = 0; k < fams[f].count && ext == NO_IDX; k++) {
            const BranchNode *m = &h->branches[fams[f].members[k]];
            if (!m->parent)
                continue;
            size_t j = history_find_branch(h, m->parent);
            if (j != NO_IDX && fam_of[j] != f)
                ext = fam_of[j];
        }
        if (ext == NO_IDX) {
            for (size_t k = 0; k < fams[f].count && sug == NO_IDX; k++) {
                const BranchNode *m = &h->branches[fams[f].members[k]];
                if (m->state != PARENT_AMBIGUOUS || !m->suggested_parent)
                    continue;
                size_t j = history_find_branch(h, m->suggested_parent);
                if (j != NO_IDX && fam_of[j] != f)
                    sug = fam_of[j];
            }
        }
        if (ext != NO_IDX) {
            fams[f].parent = ext;
            fams[f].kind = LAYOUT_EDGE_PARENT;
        } else if (sug != NO_IDX) {
            fams[f].parent = sug;
            fams[f].kind = LAYOUT_EDGE_SUGGESTED;
        }
    }

    /* --- figli e radici --------------------------------------------------- */
    size_t *child_count = xcalloc(fam_count, sizeof(size_t));
    size_t *roots = xmalloc(fam_count * sizeof(size_t));
    size_t root_count = 0;
    for (size_t f = 0; f < fam_count; f++) {
        if (fams[f].parent == NO_IDX)
            roots[root_count++] = f;
        else
            child_count[fams[f].parent]++;
    }
    size_t *child_start = xmalloc((fam_count + 1) * sizeof(size_t));
    child_start[0] = 0;
    for (size_t f = 0; f < fam_count; f++)
        child_start[f + 1] = child_start[f] + child_count[f];
    size_t *child_items = xmalloc((fam_count ? fam_count : 1) * sizeof(size_t));
    size_t *child_cur = xmalloc(fam_count * sizeof(size_t));
    memcpy(child_cur, child_start, fam_count * sizeof(size_t));
    for (size_t f = 0; f < fam_count; f++)
        if (fams[f].parent != NO_IDX)
            child_items[child_cur[fams[f].parent]++] = f;
    free(child_count);
    free(child_cur);

    g_fams = fams;
    qsort(roots, root_count, sizeof(size_t), cmp_root);
    for (size_t f = 0; f < fam_count; f++)
        qsort(child_items + child_start[f], child_start[f + 1] - child_start[f],
              sizeof(size_t), cmp_sibling);

    /* --- catene anonime e corsie: tronchi, blocco anonimo, rami con nome -- */
    size_t *anon_lane_of = xmalloc((cc ? cc : 1) * sizeof(size_t));
    size_t anon_count =
        anon_chain_lanes(h->data, cc, owners, anon_lane_of);

    size_t next = 0;
    for (size_t k = 0; k < root_count; k++) {
        Fam *r = &fams[roots[k]];
        if (r->trunk && !r->assigned) {
            r->lane = next++;
            r->assigned = true;
        }
    }
    if (anon_count > 0) {
        out->anon_lane = next; /* il blocco anonimo sta subito dopo i tronchi */
        out->anon_lane_count = anon_count;
    }
    next += anon_count;
    size_t *stack = xmalloc(fam_count * sizeof(size_t));
    size_t sp = 0;
    for (size_t k = root_count; k-- > 0;)
        stack[sp++] = roots[k];
    while (sp > 0) {
        size_t f = stack[--sp];
        if (!fams[f].assigned) {
            fams[f].lane = next++;
            fams[f].assigned = true;
        }
        for (size_t k = child_start[f + 1]; k-- > child_start[f];)
            stack[sp++] = child_items[k];
    }
    /* famiglie che la visita non raggiunge (ciclo di paternita' in un modello
     * artificiale): comunque una corsia, in coda */
    for (size_t f = 0; f < fam_count; f++) {
        if (!fams[f].assigned) {
            fams[f].lane = next++;
            fams[f].assigned = true;
        }
    }
    out->lanes = next;

    /* --- nodi ------------------------------------------------------------- */
    for (size_t i = 0; i < n; i++) {
        out->nodes[i].branch = i;
        out->nodes[i].lane = fams[fam_of[i]].lane;
        out->nodes[i].row = branch_row(&h->branches[i]);
        out->nodes[i].label_of = label_of[i];
    }

    /* --- archi tra branch: un arco per branch che ha un padre (o un
     * suggerito) --------------------------------------------------------- */
    out->edges = xmalloc((n ? 2 * n : 1) * sizeof(LayoutEdge));
    size_t e = 0;
    for (size_t i = 0; i < n; i++) {
        const BranchNode *b = &h->branches[i];
        const char *target = NULL;
        LayoutEdgeKind kind = LAYOUT_EDGE_PARENT;
        if (b->parent) {
            target = b->parent;
        } else if (b->state == PARENT_AMBIGUOUS && b->suggested_parent) {
            target = b->suggested_parent;
            kind = LAYOUT_EDGE_SUGGESTED;
        }
        if (!target)
            continue;
        size_t j = history_find_branch(h, target);
        /* niente archi verso se stessi ne' dentro la stessa famiglia (uno
         * specchio o un alias non e' un padre: e' lo stesso ramo) */
        if (j == NO_IDX || j == i || fam_of[j] == fam_of[i])
            continue;
        out->edges[e].child = i;
        out->edges[e].parent = j;
        out->edges[e].kind = kind;
        e++;
    }
    out->edge_count = e;

    /* --- commit: riga, corsia, proprietario ------------------------------- */
    out->commit_count = cc;
    out->commits = xcalloc(cc ? cc : 1, sizeof(LayoutCommit));
    for (size_t ci = 0; ci < cc; ci++) {
        LayoutCommit *cell = &out->commits[ci];
        size_t owner = owners ? owners[ci] : NO_IDX;
        cell->commit = ci;
        cell->row = ci;
        if (owner != NO_IDX && owner < n) {
            cell->owner = owner;
            cell->lane = fams[fam_of[owner]].lane;
        } else {
            cell->owner = NO_IDX;
            cell->lane = (anon_lane_of[ci] != NO_IDX)
                             ? out->anon_lane + anon_lane_of[ci]
                             : out->anon_lane;
        }
    }

    /* --- archi tra commit -------------------------------------------------- */
    /* un solo arco per genitore: di catena se resta sulla stessa corsia, di
     * diramazione se salta in un'altra, di merge dal secondo genitore in poi */
    size_t cap = 0;
    if (h->data) {
        for (size_t ci = 0; ci < cc; ci++) {
            const GitCommit *gc = &h->data->commits[ci];
            for (size_t k = 0; k < gc->parent_count; k++)
                if (gc->parent_idx[k] != NO_IDX && gc->parent_idx[k] < cc)
                    cap++;
        }
    }
    out->cedges = xmalloc((cap ? cap : 1) * sizeof(LayoutCommitEdge));
    size_t ce = 0;
    if (h->data) {
        for (size_t ci = 0; ci < cc; ci++) {
            const GitCommit *gc = &h->data->commits[ci];
            for (size_t k = 0; k < gc->parent_count; k++) {
                size_t p = gc->parent_idx[k];
                if (p == NO_IDX || p >= cc)
                    continue;
                LayoutCommitEdgeKind kind;
                if (k > 0)
                    kind = LAYOUT_CEDGE_MERGE_PARENT;
                else if (out->commits[ci].lane == out->commits[p].lane)
                    kind = LAYOUT_CEDGE_FIRST_PARENT;
                else
                    kind = LAYOUT_CEDGE_BRANCH;
                out->cedges[ce].from = ci;
                out->cedges[ce].to = p;
                out->cedges[ce].kind = kind;
                ce++;
            }
        }
    }
    out->cedge_count = ce;

    for (size_t f = 0; f < fam_count; f++)
        free(fams[f].members);
    free(fams);
    free(fam_of);
    free(label_of);
    free(seen);
    free(roots);
    free(child_start);
    free(child_items);
    free(stack);
    free(anon_lane_of);
}

void layout_free(Layout *l)
{
    if (!l)
        return;
    free(l->nodes);
    free(l->edges);
    free(l->commits);
    free(l->cedges);
    memset(l, 0, sizeof *l);
}

const LayoutNode *layout_node(const Layout *l, size_t branch_index)
{
    if (!l || branch_index >= l->node_count)
        return NULL;
    return &l->nodes[branch_index];
}

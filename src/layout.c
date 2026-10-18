/* layout.c - vedi layout.h
 *
 * Tutto lavora su indici precalcolati: nessuna scansione di nomi nei passi
 * interni (history_find_branch compare solo quando si risolve il nome di un
 * padre, una volta per famiglia). Gli ordinamenti usano chiavi strutturali
 * (posizioni nel log, ordine di [long-lived]), mai l'ordine alfabetico, cosi'
 * la disposizione non cambia se si rinominano i riferimenti.
 */
#include "layout.h"

#include "util.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define NO_IDX ((size_t)-1)

/* Una famiglia: i riferimenti con la stessa punta (stessa genealogia, stessa
 * corsia). Per la disposizione bastano il rappresentante e i dati strutturali
 * della famiglia. */
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

void layout_build(Layout *out, const History *h)
{
    memset(out, 0, sizeof *out);
    size_t n = h->branch_count;
    out->rows = h->data ? h->data->commit_count : 0;
    out->node_count = n;
    out->nodes = xcalloc(n ? n : 1, sizeof(LayoutNode));
    if (n == 0)
        return;

    size_t cc = out->rows;

    /* --- famiglie: stessa punta, stessa corsia ---------------------------- */
    size_t *fam_of = xmalloc(n * sizeof(size_t));
    size_t *seen = xmalloc((cc ? cc : 1) * sizeof(size_t));
    for (size_t k = 0; k < cc; k++)
        seen[k] = NO_IDX;

    Fam *fams = xmalloc(n * sizeof(Fam));
    size_t fam_count = 0;
    for (size_t i = 0; i < n; i++) {
        const BranchNode *b = &h->branches[i];
        size_t tip = b->tip_pos;
        size_t f;
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

    /* --- corsie: tronchi nelle prime, poi visita in profondita' ----------- */
    size_t next = 0;
    for (size_t k = 0; k < root_count; k++) {
        Fam *r = &fams[roots[k]];
        if (r->trunk && !r->assigned) {
            r->lane = next++;
            r->assigned = true;
        }
    }
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
    out->lanes = next;

    /* --- nodi ------------------------------------------------------------- */
    for (size_t i = 0; i < n; i++) {
        out->nodes[i].branch = i;
        out->nodes[i].lane = fams[fam_of[i]].lane;
        out->nodes[i].row = branch_row(&h->branches[i]);
    }

    /* --- archi: un arco per branch che ha un padre (o un suggerito) ------- */
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

    for (size_t f = 0; f < fam_count; f++)
        free(fams[f].members);
    free(fams);
    free(fam_of);
    free(seen);
    free(roots);
    free(child_start);
    free(child_items);
    free(stack);
}

void layout_free(Layout *l)
{
    if (!l)
        return;
    free(l->nodes);
    free(l->edges);
    memset(l, 0, sizeof *l);
}

const LayoutNode *layout_node(const Layout *l, size_t branch_index)
{
    if (!l || branch_index >= l->node_count)
        return NULL;
    return &l->nodes[branch_index];
}

/* render.c - vedi render.h */
#include "render.h"

#include "util.h"

#include <stdlib.h>
#include <string.h>

#define NO_IDX ((size_t)-1)

/* Palette delle corsie: 16 colori distinti e leggibili su fondo scuro. */
static const uint8_t g_palette[RENDER_PALETTE_SIZE][3] = {
    { 0xF0, 0x6A, 0x6A }, { 0xF0, 0xA6, 0x3A }, { 0xE8, 0xD1, 0x4A },
    { 0x8F, 0xD1, 0x4A }, { 0x4F, 0xC1, 0x6A }, { 0x3A, 0xC1, 0xA6 },
    { 0x46, 0xB1, 0xE0 }, { 0x5A, 0x9B, 0xF0 }, { 0x8A, 0x7A, 0xF0 },
    { 0xB0, 0x6A, 0xE0 }, { 0xE0, 0x6A, 0xC1 }, { 0xE0, 0x7A, 0x8A },
    { 0xC0, 0xA0, 0x80 }, { 0x90, 0xA0, 0xB0 }, { 0x6A, 0xC0, 0xA0 },
    { 0xA0, 0xC0, 0x60 },
};

void render_lane_color(size_t lane, uint8_t out[3])
{
    const uint8_t *c = g_palette[lane % RENDER_PALETTE_SIZE];
    out[0] = c[0];
    out[1] = c[1];
    out[2] = c[2];
}

/* Riempie una polilinea secondo la convenzione della scena. Ogni riga ha un
 * solo commit, quindi un tratto orizzontale all'altezza di una riga non
 * attraversa mai un nodo estraneo:
 *  - stessa corsia: verticale di 2 punti;
 *  - diramazione (RENDER_LINE_BRANCH): verticale nella corsia del figlio fino
 *    alla riga del genitore, poi orizzontale fino alla corsia del genitore;
 *  - merge (e suggerito): orizzontale alla riga del figlio fino alla corsia del
 *    genitore, poi verticale in quella corsia fino al genitore. */
static size_t route_line(RenderPt pts[RENDER_MAX_POINTS], RenderPt a, RenderPt b,
                         RenderLineKind kind)
{
    pts[0] = a;
    if (a.x == b.x) {
        pts[1] = b;
        return 2;
    }
    if (kind == RENDER_LINE_BRANCH) {
        pts[1].x = a.x;
        pts[1].y = b.y;
        pts[2] = b;
        return 3;
    }
    pts[1].x = b.x;
    pts[1].y = a.y;
    pts[2] = b;
    return 3;
}

/* Voce di etichetta: un riferimento (branch o tag) alla sua punta. */
typedef struct {
    size_t commit;
    RenderLabelKind kind;
    const char *name;
    bool is_head;
    bool is_remote;
} LabelEntry;

/* ordine totale per il raggruppamento: commit, tipo, nome */
static int cmp_entry(const void *pa, const void *pb)
{
    const LabelEntry *a = pa;
    const LabelEntry *b = pb;
    if (a->commit != b->commit)
        return a->commit < b->commit ? -1 : 1;
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : 1;
    return strcmp(a->name, b->name);
}

/* Costruisce le etichette: una per (commit puntato, tipo). I riferimenti di un
 * branch con punta fuori dal log non producono etichetta (non c'e' una cella a
 * cui ancorarli). */
static void build_labels(Render *out, const GitData *data, const History *hist,
                         const Layout *lay)
{
    size_t cap = (hist ? hist->branch_count : 0) +
                 (data ? data->ref_count : 0);
    LabelEntry *ents = xmalloc((cap ? cap : 1) * sizeof(LabelEntry));
    size_t ne = 0;
    if (hist) {
        for (size_t i = 0; i < hist->branch_count; i++) {
            size_t row = lay->nodes[i].row;
            if (row == NO_IDX || row >= lay->rows)
                continue;
            ents[ne].commit = row;
            ents[ne].kind = RENDER_LABEL_BRANCH;
            ents[ne].name = hist->branches[i].name;
            ents[ne].is_head = hist->branches[i].is_head;
            ents[ne].is_remote = hist->branches[i].is_remote;
            ne++;
        }
    }
    if (data) {
        for (size_t i = 0; i < data->ref_count; i++) {
            if (data->refs[i].kind != REF_TAG)
                continue;
            size_t ci = gitdata_find_commit(data, data->refs[i].target);
            if (ci == NO_IDX || ci >= lay->rows)
                continue;
            ents[ne].commit = ci;
            ents[ne].kind = RENDER_LABEL_TAG;
            ents[ne].name = data->refs[i].name;
            ents[ne].is_head = false;
            ents[ne].is_remote = false;
            ne++;
        }
    }
    if (ne == 0) {
        free(ents);
        return;
    }
    qsort(ents, ne, sizeof(LabelEntry), cmp_entry);

    out->anchors = xcalloc(ne, sizeof(RenderAnchor));
    size_t gi = 0;
    for (size_t i = 0; i < ne;) {
        size_t commit = ents[i].commit;
        size_t slot = 0;
        while (i < ne && ents[i].commit == commit) {
            RenderLabelKind kind = ents[i].kind;
            bool is_head = false, has_remote = false;
            StrBuf sb;
            sb_init(&sb);
            while (i < ne && ents[i].commit == commit && ents[i].kind == kind) {
                if (sb.len)
                    sb_append(&sb, ", ");
                sb_append(&sb, ents[i].name);
                is_head = is_head || ents[i].is_head;
                has_remote = has_remote || ents[i].is_remote;
                i++;
            }
            RenderAnchor *an = &out->anchors[gi++];
            an->p.x = (double)lay->commits[commit].lane;
            an->p.y = (double)commit;
            an->row = commit;
            an->commit = commit;
            an->slot = slot++;
            an->kind = kind;
            an->text = sb.data; /* il buffer passa alla scena */
            an->is_head = is_head;
            an->has_remote = has_remote;
        }
    }
    out->anchor_count = gi;
    free(ents);
}

void render_build(Render *out, const GitData *data, const History *hist,
                  const Layout *lay)
{
    memset(out, 0, sizeof *out);
    if (!lay)
        return;
    out->lane_count = lay->lanes;
    out->rows = lay->rows;

    size_t head_ci = NO_IDX;
    if (data && data->head_hash)
        head_ci = gitdata_find_commit(data, data->head_hash);

    /* punti dei commit: uno per cella, alla corsia e riga del layout */
    out->dot_count = lay->commit_count;
    out->dots = xcalloc(out->dot_count ? out->dot_count : 1, sizeof(RenderDot));
    for (size_t ci = 0; ci < lay->commit_count; ci++) {
        const LayoutCommit *cell = &lay->commits[ci];
        RenderDot *d = &out->dots[ci];
        d->p.x = (double)cell->lane;
        d->p.y = (double)cell->row;
        d->lane = cell->lane;
        d->row = cell->row;
        d->commit = ci;
        d->head = (ci == head_ci);
        d->anon = (cell->owner == NO_IDX);
        d->merge_target = false;
    }

    /* archi: uno per ogni arco tra commit. La diramazione di un ramo con padre
     * ambiguo si disegna tratteggiata (e' l'ipotesi del layout). */
    size_t cap = lay->cedge_count;
    out->lines = xcalloc(cap ? cap : 1, sizeof(RenderLine));
    size_t nl = 0;

    for (size_t e = 0; e < lay->cedge_count; e++) {
        const LayoutCommitEdge *ce = &lay->cedges[e];
        if (ce->from >= lay->commit_count || ce->to >= lay->commit_count)
            continue;
        RenderLine *ln = &out->lines[nl];
        RenderPt a = { (double)lay->commits[ce->from].lane, (double)ce->from };
        RenderPt b = { (double)lay->commits[ce->to].lane, (double)ce->to };
        ln->from = ce->from;
        ln->to = ce->to;
        switch (ce->kind) {
        case LAYOUT_CEDGE_FIRST_PARENT:
            ln->kind = RENDER_LINE_FIRST_PARENT;
            break;
        case LAYOUT_CEDGE_MERGE_PARENT:
            ln->kind = RENDER_LINE_MERGE_PARENT;
            break;
        default:
            ln->kind = RENDER_LINE_BRANCH;
            break;
        }
        ln->n = route_line(ln->pts, a, b, ln->kind);
        ln->lane = lay->commits[ce->from].lane;
        ln->dashed = false;
        if (ln->kind == RENDER_LINE_MERGE_PARENT)
            out->dots[ce->to].merge_target = true;
        if (ln->kind == RENDER_LINE_BRANCH && hist) {
            size_t owner = lay->commits[ce->from].owner;
            if (owner != NO_IDX && owner < hist->branch_count &&
                hist->branches[owner].state == PARENT_AMBIGUOUS)
                ln->dashed = true;
        }
        nl++;
    }
    out->line_count = nl;

    /* etichette: un gruppo per (commit puntato, tipo), alla corsia del commit.
     * I nomi dello stesso gruppo si uniscono; i gruppi dello stesso commit si
     * impilano con uno slot crescente (branch prima dei tag). */
    build_labels(out, data, hist, lay);
}

void render_free(Render *r)
{
    if (!r)
        return;
    for (size_t i = 0; i < r->anchor_count; i++)
        free(r->anchors[i].text);
    free(r->dots);
    free(r->lines);
    free(r->anchors);
    memset(r, 0, sizeof *r);
}

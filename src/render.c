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
    }

    /* archi: tutti gli archi tra commit piu' le diramazioni suggerite (a
     * livello di branch). Il vettore e' dimensionato sul caso peggiore, poi la
     * lunghezza effettiva tiene conto degli archi suggeriti scartati (una punta
     * fuori dal log non ha posizione). */
    size_t suggested = 0;
    for (size_t e = 0; e < lay->edge_count; e++)
        if (lay->edges[e].kind == LAYOUT_EDGE_SUGGESTED)
            suggested++;
    size_t cap = lay->cedge_count + suggested;
    out->lines = xcalloc(cap ? cap : 1, sizeof(RenderLine));
    size_t nl = 0;

    for (size_t e = 0; e < lay->cedge_count; e++) {
        const LayoutCommitEdge *ce = &lay->cedges[e];
        if (ce->from >= lay->commit_count || ce->to >= lay->commit_count)
            continue;
        RenderLine *ln = &out->lines[nl];
        RenderPt a = { (double)lay->commits[ce->from].lane, (double)ce->from };
        RenderPt b = { (double)lay->commits[ce->to].lane, (double)ce->to };
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
        nl++;
    }

    for (size_t e = 0; e < lay->edge_count; e++) {
        if (lay->edges[e].kind != LAYOUT_EDGE_SUGGESTED)
            continue;
        const LayoutNode *cn = &lay->nodes[lay->edges[e].child];
        const LayoutNode *pn = &lay->nodes[lay->edges[e].parent];
        if (cn->row == NO_IDX || pn->row == NO_IDX)
            continue;
        RenderLine *ln = &out->lines[nl];
        RenderPt a = { (double)cn->lane, (double)cn->row };
        RenderPt b = { (double)pn->lane, (double)pn->row };
        ln->kind = RENDER_LINE_SUGGESTED;
        ln->n = route_line(ln->pts, a, b, ln->kind);
        ln->lane = cn->lane;
        ln->dashed = true;
        nl++;
    }
    out->line_count = nl;

    /* ancore delle etichette: una per branch, alla posizione del suo nodo */
    if (hist && lay->nodes) {
        out->anchor_count = hist->branch_count;
        out->anchors =
            xcalloc(out->anchor_count ? out->anchor_count : 1, sizeof(RenderAnchor));
        for (size_t i = 0; i < hist->branch_count; i++) {
            const LayoutNode *nd = &lay->nodes[i];
            RenderAnchor *an = &out->anchors[i];
            an->p.x = (double)nd->lane;
            an->p.y = (nd->row == NO_IDX) ? 0.0 : (double)nd->row;
            an->row = nd->row;
            an->branch = i;
            an->is_head = hist->branches[i].is_head;
            an->is_remote = hist->branches[i].is_remote;
        }
    }
}

void render_free(Render *r)
{
    if (!r)
        return;
    free(r->dots);
    free(r->lines);
    free(r->anchors);
    memset(r, 0, sizeof *r);
}

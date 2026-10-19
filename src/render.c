/* render.c - vedi render.h */
#include "render.h"

#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    size_t branch; /* indice del branch (SIZE_MAX per un tag) */
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
            ents[ne].branch = i;
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
            ents[ne].branch = NO_IDX;
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
            size_t rep = NO_IDX;
            bool rep_remote = true;
            StrBuf sb;
            sb_init(&sb);
            while (i < ne && ents[i].commit == commit && ents[i].kind == kind) {
                if (sb.len)
                    sb_append(&sb, ", ");
                sb_append(&sb, ents[i].name);
                if (kind == RENDER_LABEL_BRANCH &&
                    (rep == NO_IDX || (rep_remote && !ents[i].is_remote))) {
                    rep = ents[i].branch;
                    rep_remote = ents[i].is_remote;
                }
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
            an->branch = rep;
            an->w = RENDER_LABEL_PAD +
                    RENDER_LABEL_CHAR * (double)strlen(sb.data);
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

/* ---- selezione e dettagli (fase 6) ---------------------------------------- */

size_t render_commit_at(const Render *r, double x, double y, double rx,
                        double ry)
{
    if (!r || rx <= 0 || ry <= 0)
        return NO_IDX;
    size_t best = NO_IDX;
    double best_d = 1.0;
    for (size_t i = 0; i < r->dot_count; i++) {
        const RenderDot *d = &r->dots[i];
        double dx = (x - d->p.x) / rx;
        double dy = (y - d->p.y) / ry;
        double dist = dx * dx + dy * dy;
        if (dist <= 1.0 && (best == NO_IDX || dist < best_d)) {
            best = d->commit;
            best_d = dist;
        }
    }
    return best;
}

size_t render_label_at(const Render *r, double x, double y)
{
    if (!r)
        return NO_IDX;
    /* dal fondo: l'ultimo gruppo disegnato e' quello col slot piu' alto */
    for (size_t i = r->anchor_count; i > 0; i--) {
        const RenderAnchor *a = &r->anchors[i - 1];
        double x0 = a->p.x + RENDER_LABEL_DX;
        double y0 = a->p.y + (double)a->slot - RENDER_LABEL_H / 2.0;
        if (x >= x0 && x <= x0 + a->w && y >= y0 &&
            y <= y0 + RENDER_LABEL_H)
            return i - 1;
    }
    return NO_IDX;
}

static const char *parent_state_name(ParentState st)
{
    switch (st) {
    case PARENT_CERTAIN:
        return "certo";
    case PARENT_CONFIGURED:
        return "configurato";
    case PARENT_DEDUCED:
        return "dedotto";
    case PARENT_AMBIGUOUS:
        return "ambiguo";
    default:
        return "nessuno";
    }
}

static void format_date(int64_t unix_time, char out[32])
{
    time_t t = (time_t)unix_time;
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    strftime(out, 32, "%Y-%m-%d %H:%MZ", &tmv);
}

/* Aggiunge il testo mandando a capo a `width` colonne, spezzando sulle spazi
 * quando c'e' posto; si ferma dopo `max_lines` righe. */
static void append_wrapped(StrBuf *sb, const char *text, size_t width,
                           size_t max_lines)
{
    if (width < 8)
        width = 8;
    size_t lines = 0;
    const char *p = text ? text : "";
    while (*p && lines < max_lines) {
        const char *nl = strchr(p, '\n');
        size_t seg = nl ? (size_t)(nl - p) : strlen(p);
        while (seg > 0 && p[seg - 1] == '\r')
            seg--;
        size_t off = 0;
        while (off < seg && lines < max_lines) {
            size_t take = seg - off;
            if (take > width) {
                take = width;
                size_t br = take;
                while (br > 0 && p[off + br] != ' ')
                    br--;
                if (br > 0)
                    take = br;
            }
            size_t s = off, e = off + take;
            while (s < e && p[s] == ' ')
                s++;
            while (e > s && p[e - 1] == ' ')
                e--;
            sb_appendn(sb, p + s, e - s);
            sb_append(sb, "\n");
            lines++;
            off += take;
            while (off < seg && p[off] == ' ')
                off++;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
}

char *render_details_text(const GitData *data, const History *hist,
                          const Layout *lay, size_t commit, size_t branch,
                          size_t width)
{
    if (!data || commit >= data->commit_count)
        return NULL;
    const GitCommit *c = &data->commits[commit];
    StrBuf sb;
    sb_init(&sb);
    if (branch != NO_IDX && hist && branch < hist->branch_count)
        sb_appendf(&sb, "selezione: branch %s\n\n",
                   hist->branches[branch].name);
    sb_appendf(&sb, "commit: %s\n", c->hash ? c->hash : "-");
    sb_appendf(&sb, "autore: %s\n", (c->author && c->author[0]) ? c->author
                                                                 : "-");
    char date[32];
    format_date(c->date, date);
    sb_appendf(&sb, "data:   %s\n", date);

    sb_append(&sb, "messaggio:\n");
    append_wrapped(&sb, c->message, width > 2 ? width - 2 : width, 12);

    sb_append(&sb, "genitori:");
    if (c->parent_count == 0) {
        sb_append(&sb, " (nessuno)");
    } else {
        for (size_t k = 0; k < c->parent_count; k++) {
            char sh[16];
            gitdata_short_hash(c->parents[k], 8, sh);
            sb_appendf(&sb, " %s", sh);
        }
        if (c->parent_count > 1)
            sb_append(&sb, "  (merge)");
    }
    sb_append(&sb, "\n");

    sb_append(&sb, "riferimenti:");
    {
        bool any = false;
        for (size_t i = 0; i < data->ref_count; i++) {
            if (!data->refs[i].target ||
                strcmp(data->refs[i].target, c->hash) != 0)
                continue;
            sb_appendf(&sb, " %s%s", data->refs[i].name,
                       data->refs[i].is_head ? " (HEAD)" : "");
            any = true;
        }
        if (!any)
            sb_append(&sb, " (nessuno)");
    }
    sb_append(&sb, "\n");

    size_t owner = NO_IDX;
    if (branch != NO_IDX && hist && branch < hist->branch_count) {
        owner = branch;
    } else if (lay && commit < lay->commit_count) {
        owner = lay->commits[commit].owner;
    }
    if (owner == NO_IDX || !hist || owner >= hist->branch_count) {
        sb_append(&sb, "ramo: anonimo\n");
    } else {
        const BranchNode *b = &hist->branches[owner];
        sb_appendf(&sb, "ramo: %s  (padre: %s)\n", b->name,
                   b->parent ? b->parent : "-");
        sb_appendf(&sb, "stato: %s\n", parent_state_name(b->state));
        sb_appendf(&sb, "motivo: %s\n",
                   (b->motivation && b->motivation[0]) ? b->motivation : "-");
        if (b->state == PARENT_AMBIGUOUS && b->suggested_parent)
            sb_appendf(&sb, "padre suggerito: %s\n", b->suggested_parent);
    }
    return sb.data;
}

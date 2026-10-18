/* test_render.c - test automatici della geometria della scena (fase 5).
 *
 * Uso: test_render <percorso-repo-di-test>
 *
 * Verifica, su un layout costruito a mano (coordinate attese esatte) e sul
 * repository di prova, che la scena prodotta da render_build sia coerente col
 * layout e deterministica:
 *  - un punto per ogni commit, alla corsia e riga del layout, con l'indice del
 *    commit; il commit di HEAD e' marcato; i commit anonimi sono marcati;
 *  - un arco per ogni arco tra commit, con gli estremi sulle celle dei commit;
 *    il primo genitore e' un segmento verticale di 2 punti, gli archi che
 *    cambiano corsia sono spezzate a gomito di 4 punti con la mezzeria esatta;
 *  - un arco tratteggiato per ogni diramazione suggerita con entrambe le punte
 *    nel log; le punte fuori dal log non producono archi;
 *  - un'ancora per branch, alla posizione del suo nodo;
 *  - colore di corsia deterministico (palette ciclica di 16);
 *  - due costruzioni successive identiche (memcmp);
 *  - un layout vuoto produce una scena vuota e render_free la rilascia.
 *
 * Esce con 0 se tutti i test passano, 1 se qualcuno fallisce, 2 su errore di
 * preparazione.
 */
#include "config.h"
#include "gitdata.h"
#include "history.h"
#include "layout.h"
#include "render.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NO_IDX ((size_t)-1)

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, ...)                                                \
    do {                                                                \
        if (cond) {                                                     \
            g_pass++;                                                   \
        } else {                                                        \
            g_fail++;                                                   \
            fprintf(stderr, "FALLITO %s:%d: ", __FILE__, __LINE__);     \
            fprintf(stderr, __VA_ARGS__);                               \
            fputc('\n', stderr);                                        \
        }                                                               \
    } while (0)

#define REQUIRE(cond, msg)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "PREREQUISITO FALLITO: %s\n", msg);         \
            return 2;                                                   \
        }                                                               \
    } while (0)

static bool pt_eq(RenderPt p, double x, double y)
{
    return p.x == x && p.y == y;
}

/* ---- layout costruito a mano: coordinate attese esatte -------------------- */

static void test_hand_built(void)
{
    Layout lay;
    memset(&lay, 0, sizeof lay);

    LayoutCommit commits[3];
    memset(commits, 0, sizeof commits);
    commits[0].commit = 0;
    commits[0].row = 0;
    commits[0].lane = 0;
    commits[0].owner = 0;
    commits[1].commit = 1;
    commits[1].row = 1;
    commits[1].lane = 0;
    commits[1].owner = 0;
    commits[2].commit = 2;
    commits[2].row = 2;
    commits[2].lane = 1;
    commits[2].owner = NO_IDX;

    LayoutCommitEdge ce[3];
    memset(ce, 0, sizeof ce);
    ce[0].from = 0;
    ce[0].to = 1;
    ce[0].kind = LAYOUT_CEDGE_FIRST_PARENT;
    ce[1].from = 1;
    ce[1].to = 2;
    ce[1].kind = LAYOUT_CEDGE_BRANCH;
    ce[2].from = 2;
    ce[2].to = 0;
    ce[2].kind = LAYOUT_CEDGE_MERGE_PARENT;

    lay.commits = commits;
    lay.commit_count = 3;
    lay.cedges = ce;
    lay.cedge_count = 3;
    lay.lanes = 2;
    lay.rows = 3;

    Render sc;
    render_build(&sc, NULL, NULL, &lay);

    CHECK(sc.dot_count == 3, "punti: attesi 3, ottenuti %zu", sc.dot_count);
    CHECK(sc.line_count == 3, "archi: attesi 3, ottenuti %zu", sc.line_count);
    CHECK(sc.anchor_count == 0, "ancore: attese 0, ottenute %zu", sc.anchor_count);
    CHECK(sc.lane_count == 2, "corsie: attese 2, ottenute %zu", sc.lane_count);
    CHECK(sc.rows == 3, "righe: attese 3, ottenute %zu", sc.rows);

    if (sc.dot_count == 3) {
        CHECK(pt_eq(sc.dots[0].p, 0, 0), "punto 0: atteso (0,0)");
        CHECK(pt_eq(sc.dots[1].p, 0, 1), "punto 1: atteso (0,1)");
        CHECK(pt_eq(sc.dots[2].p, 1, 2), "punto 2: atteso (1,2)");
        CHECK(sc.dots[0].commit == 0 && sc.dots[1].commit == 1 &&
                  sc.dots[2].commit == 2,
              "indici dei commit");
        CHECK(!sc.dots[0].head && !sc.dots[0].anon, "punto 0: non HEAD, non anonimo");
        CHECK(sc.dots[2].anon, "punto 2 anonimo");
        CHECK(!sc.dots[2].head, "punto 2: non HEAD");
    }

    if (sc.line_count == 3) {
        const RenderLine *l0 = &sc.lines[0];
        const RenderLine *l1 = &sc.lines[1];
        const RenderLine *l2 = &sc.lines[2];
        CHECK(l0->kind == RENDER_LINE_FIRST_PARENT && l0->n == 2 &&
                  !l0->dashed && l0->lane == 0,
              "arco 0: primo genitore, verticale, corsia 0");
        CHECK(l0->n == 2 && pt_eq(l0->pts[0], 0, 0) && pt_eq(l0->pts[1], 0, 1),
              "arco 0: da (0,0) a (0,1)");
        CHECK(l1->kind == RENDER_LINE_BRANCH && l1->n == 4 && !l1->dashed &&
                  l1->lane == 0,
              "arco 1: diramazione, gomito, corsia 0");
        CHECK(l1->n == 4 && pt_eq(l1->pts[0], 0, 1) && pt_eq(l1->pts[1], 0, 1.5) &&
                  pt_eq(l1->pts[2], 1, 1.5) && pt_eq(l1->pts[3], 1, 2),
              "arco 1: gomito da (0,1) a (1,2)");
        CHECK(l2->kind == RENDER_LINE_MERGE_PARENT && l2->n == 4 && !l2->dashed &&
                  l2->lane == 1,
              "arco 2: merge, gomito, corsia 1");
        CHECK(l2->n == 4 && pt_eq(l2->pts[0], 1, 2) && pt_eq(l2->pts[1], 1, 1) &&
                  pt_eq(l2->pts[2], 0, 1) && pt_eq(l2->pts[3], 0, 0),
              "arco 2: gomito da (1,2) a (0,0)");
    }

    render_free(&sc);
}

/* ---- layout vuoto --------------------------------------------------------- */

static void test_empty(void)
{
    Layout lay;
    memset(&lay, 0, sizeof lay);
    Render sc;
    render_build(&sc, NULL, NULL, &lay);
    CHECK(sc.dot_count == 0 && sc.line_count == 0 && sc.anchor_count == 0,
          "scena vuota");
    CHECK(sc.lane_count == 0 && sc.rows == 0, "corsie e righe a zero");
    render_free(&sc);

    /* render_build con lay NULL non deve toccare nulla di pericoloso */
    Render sc2;
    render_build(&sc2, NULL, NULL, NULL);
    CHECK(sc2.dot_count == 0 && sc2.line_count == 0, "lay NULL: scena vuota");
    render_free(&sc2);
}

/* ---- palette -------------------------------------------------------------- */

static void test_palette(void)
{
    uint8_t a[3], b[3], c[3];
    render_lane_color(3, a);
    render_lane_color(3, b);
    CHECK(memcmp(a, b, 3) == 0, "colore deterministico");
    render_lane_color(3 + RENDER_PALETTE_SIZE, c);
    CHECK(memcmp(a, c, 3) == 0, "palette ciclica di %d", RENDER_PALETTE_SIZE);
    render_lane_color(0, a);
    render_lane_color(1, b);
    CHECK(memcmp(a, b, 3) != 0, "corsie adiacenti con colori diversi");
}

/* ---- repository di prova -------------------------------------------------- */

static int test_real_repo(const char *repo)
{
    char err[1024];
    GitData data;
    REQUIRE(gitdata_load(&data, repo, err, sizeof err), err);

    ConfigBranch *branches =
        xmalloc((data.ref_count ? data.ref_count : 1) * sizeof(ConfigBranch));
    size_t nb = 0;
    for (size_t i = 0; i < data.ref_count; i++) {
        if (data.refs[i].kind == REF_TAG)
            continue;
        const char *name = data.refs[i].name;
        const char *canonical = name;
        if (data.refs[i].kind == REF_REMOTE_BRANCH) {
            const char *slash = strchr(name, '/');
            if (slash && slash[1])
                canonical = slash + 1;
        }
        branches[nb].name = name;
        branches[nb].canonical = canonical;
        nb++;
    }
    char *cfg_path = config_path_in_repo(data.repo_path);
    ConfigReport report;
    GraphConfig cfg;
    char cfg_err[1024];
    config_load(&cfg, cfg_path, branches, nb, &report, cfg_err, sizeof cfg_err);
    free(cfg_path);
    config_report_free(&report);
    free(branches);

    History hist;
    REQUIRE(history_build(&hist, &data, &cfg, err, sizeof err), err);

    Layout lay;
    layout_build(&lay, &hist);

    Render sc;
    render_build(&sc, &data, &hist, &lay);

    size_t head_ci = data.head_hash ? gitdata_find_commit(&data, data.head_hash)
                                    : NO_IDX;

    /* punti: uno per commit, alla sua cella */
    CHECK(sc.dot_count == lay.commit_count, "punti == commit (%zu vs %zu)",
          sc.dot_count, lay.commit_count);
    CHECK(sc.lane_count == lay.lanes, "corsie == layout");
    CHECK(sc.rows == lay.rows, "righe == layout");
    for (size_t ci = 0; ci < sc.dot_count && ci < lay.commit_count; ci++) {
        const RenderDot *d = &sc.dots[ci];
        const LayoutCommit *cell = &lay.commits[ci];
        if (d->lane != cell->lane || d->row != cell->row || d->commit != ci ||
            !pt_eq(d->p, (double)cell->lane, (double)cell->row)) {
            CHECK(false, "punto %zu non coerente con la cella", ci);
            break;
        }
        if (d->anon != (cell->owner == NO_IDX) || d->head != (ci == head_ci)) {
            CHECK(false, "punto %zu: anonimo/head errati", ci);
            break;
        }
        if (ci + 1 == sc.dot_count)
            CHECK(true, "punti coerenti con le celle");
    }

    /* archi: tanti quanti gli archi tra commit piu' le diramazioni suggerite
     * con entrambe le punte nel log */
    size_t expected_lines = 0;
    for (size_t e = 0; e < lay.cedge_count; e++)
        if (lay.cedges[e].from < lay.commit_count &&
            lay.cedges[e].to < lay.commit_count)
            expected_lines++;
    size_t expected_sugg = 0;
    for (size_t e = 0; e < lay.edge_count; e++) {
        if (lay.edges[e].kind != LAYOUT_EDGE_SUGGESTED)
            continue;
        if (lay.nodes[lay.edges[e].child].row != NO_IDX &&
            lay.nodes[lay.edges[e].parent].row != NO_IDX)
            expected_sugg++;
    }
    CHECK(sc.line_count == expected_lines + expected_sugg,
          "archi: attesi %zu, ottenuti %zu", expected_lines + expected_sugg,
          sc.line_count);

    size_t solid = 0, dashed = 0;
    for (size_t i = 0; i < sc.line_count; i++) {
        const RenderLine *ln = &sc.lines[i];
        if (ln->n != 2 && ln->n != 4) {
            CHECK(false, "arco %zu: %zu punti", i, ln->n);
            break;
        }
        if (ln->dashed) {
            dashed++;
            CHECK(ln->kind == RENDER_LINE_SUGGESTED, "arco %zu: tratteggiato non suggerito", i);
        } else {
            solid++;
            CHECK(ln->kind != RENDER_LINE_SUGGESTED, "arco %zu: suggerito non tratteggiato", i);
        }
        /* gli estremi sono celle intere: la prima e' il figlio (riga minore o
         * uguale), l'ultima il genitore */
        if (ln->pts[0].x != (double)(size_t)ln->pts[0].x ||
            ln->pts[0].y != (double)(size_t)ln->pts[0].y ||
            ln->pts[ln->n - 1].x != (double)(size_t)ln->pts[ln->n - 1].x ||
            ln->pts[ln->n - 1].y != (double)(size_t)ln->pts[ln->n - 1].y) {
            CHECK(false, "arco %zu: estremi non interi", i);
            break;
        }
        if (ln->n == 4) {
            double mid = (ln->pts[0].y + ln->pts[3].y) / 2.0;
            if (ln->pts[1].y != mid || ln->pts[2].y != mid ||
                ln->pts[1].x != ln->pts[0].x || ln->pts[2].x != ln->pts[3].x) {
                CHECK(false, "arco %zu: gomito non coerente", i);
                break;
            }
        }
        if (i + 1 == sc.line_count) {
            CHECK(solid == expected_lines, "archi pieni: attesi %zu, ottenuti %zu",
                  expected_lines, solid);
            CHECK(dashed == expected_sugg, "archi suggeriti: attesi %zu, ottenuti %zu",
                  expected_sugg, dashed);
        }
    }

    /* ancore: una per branch */
    CHECK(sc.anchor_count == hist.branch_count, "ancore == branch (%zu vs %zu)",
          sc.anchor_count, hist.branch_count);
    for (size_t i = 0; i < sc.anchor_count && i < hist.branch_count; i++) {
        const RenderAnchor *a = &sc.anchors[i];
        const LayoutNode *nd = &lay.nodes[i];
        if (a->branch != i || a->row != nd->row || a->p.x != (double)nd->lane ||
            a->is_head != hist.branches[i].is_head ||
            a->is_remote != hist.branches[i].is_remote) {
            CHECK(false, "ancora %zu non coerente col nodo", i);
            break;
        }
        if (i + 1 == sc.anchor_count)
            CHECK(true, "ancore coerenti coi nodi");
    }

    /* determinismo: seconda costruzione identica */
    Render sc2;
    render_build(&sc2, &data, &hist, &lay);
    CHECK(sc2.dot_count == sc.dot_count && sc2.line_count == sc.line_count &&
              sc2.anchor_count == sc.anchor_count,
          "seconda costruzione: stesse lunghezze");
    if (sc.dot_count == sc2.dot_count && sc.dot_count > 0)
        CHECK(memcmp(sc.dots, sc2.dots, sc.dot_count * sizeof(RenderDot)) == 0,
              "punti identici fra due costruzioni");
    if (sc.line_count == sc2.line_count && sc.line_count > 0)
        CHECK(memcmp(sc.lines, sc2.lines, sc.line_count * sizeof(RenderLine)) == 0,
              "archi identici fra due costruzioni");
    if (sc.anchor_count == sc2.anchor_count && sc.anchor_count > 0)
        CHECK(memcmp(sc.anchors, sc2.anchors,
                     sc.anchor_count * sizeof(RenderAnchor)) == 0,
              "ancore identiche fra due costruzioni");
    render_free(&sc2);

    render_free(&sc);
    layout_free(&lay);
    history_free(&hist);
    config_free(&cfg);
    gitdata_free(&data);
    return 0;
}

int main(int argc, char **argv)
{
    test_hand_built();
    test_empty();
    test_palette();

    if (argc < 2) {
        fprintf(stderr, "uso: test_render <percorso-repo-di-test>\n");
        return 2;
    }
    int rc = test_real_repo(argv[1]);
    if (rc != 0)
        return rc;

    printf("render: %d controlli passati, %d falliti\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

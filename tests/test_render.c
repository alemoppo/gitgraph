/* test_render.c - test automatici della geometria della scena (fase 5).
 *
 * Uso: test_render <percorso-repo-di-test>
 *
 * Verifica, su un layout costruito a mano (coordinate attese esatte), sul
 * repository di prova e su repository sintetici costruiti qui, che la scena
 * prodotta da render_build sia coerente col layout e deterministica.
 *
 * Percorso degli archi (ogni riga ha un solo commit: il gomito sta su una
 * riga, mai su una mezzeria):
 *  - nessun punto sta strettamente dentro un tratto orizzontale;
 *  - nessun punto di una catena diversa (proprietario diverso) sta dentro un
 *    tratto verticale.
 * Le due proprieta' sono provate sul repository di prova, sul caso x/y/z, su
 * 30 rami mergiati e cancellati in sequenza e su un repository pseudo-casuale
 * (seme fisso) con rami che si sovrappongono.
 *
 * Verifica inoltre:
 *  - un punto per ogni commit, alla corsia e riga del layout, con l'indice del
 *    commit; il commit di HEAD e' marcato; i commit anonimi sono marcati;
 *  - un arco per ogni arco tra commit, con gli estremi sulle celle dei commit;
 *    il primo genitore e' un segmento verticale di 2 punti; diramazione e
 *    merge sono spezzate di 3 punti con il gomito sulla riga giusta;
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

#ifdef _WIN32
#include <windows.h>
#endif

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

/* ---- invarianti dei percorsi ---------------------------------------------- */

static bool strictly_between(double v, double a, double b)
{
    double lo = a < b ? a : b;
    double hi = a < b ? b : a;
    return v > lo && v < hi;
}

/* proprietario del commit (SIZE_MAX se anonimo) */
static size_t commit_owner(const Layout *lay, size_t ci)
{
    return (ci < lay->commit_count) ? lay->commits[ci].owner : NO_IDX;
}

/* Le due proprieta' del percorso. `only_solid` limita il controllo agli archi
 * pieni: gli archi suggeriti a livello di branch (che non sono archi tra commit)
 * si appoggiano alla riga di un nodo-branch, dove puo' stare anche un'etichetta
 * remota con corsia diversa. */
static void check_paths(const Render *sc, const Layout *lay, const char *tag,
                        bool only_solid)
{
    for (size_t i = 0; i < sc->line_count; i++) {
        const RenderLine *ln = &sc->lines[i];
        if (only_solid && ln->dashed)
            continue;
        size_t from = (size_t)ln->pts[0].y;
        size_t to = (size_t)ln->pts[ln->n - 1].y;
        size_t owner_from = commit_owner(lay, from);
        size_t owner_to = commit_owner(lay, to);
        for (size_t s = 0; s + 1 < ln->n; s++) {
            double x0 = ln->pts[s].x, y0 = ln->pts[s].y;
            double x1 = ln->pts[s + 1].x, y1 = ln->pts[s + 1].y;
            if (y0 == y1) {
                for (size_t d = 0; d < sc->dot_count; d++) {
                    const RenderDot *dot = &sc->dots[d];
                    if (dot->p.y == y0 && strictly_between(dot->p.x, x0, x1)) {
                        CHECK(false,
                              "%s: il punto %zu sta dentro il tratto "
                              "orizzontale dell'arco %zu",
                              tag, d, i);
                        goto next_line;
                    }
                }
            } else if (x0 == x1) {
                /* la catena del tratto verticale e' quella dell'estremo che
                 * giace in questa corsia (per un merge, il genitore) */
                size_t seg_owner = (ln->pts[0].x == x0) ? owner_from : owner_to;
                for (size_t d = 0; d < sc->dot_count; d++) {
                    const RenderDot *dot = &sc->dots[d];
                    if (dot->p.x == x0 && strictly_between(dot->p.y, y0, y1) &&
                        commit_owner(lay, dot->commit) != seg_owner) {
                        CHECK(false,
                              "%s: il punto %zu (catena %zu) sta dentro il "
                              "tratto verticale dell'arco %zu (catena %zu)",
                              tag, d, commit_owner(lay, dot->commit), i,
                              seg_owner);
                        goto next_line;
                    }
                }
            }
        }
    next_line:;
    }
    CHECK(true, "%s: percorsi senza nodi estranei", tag);
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
        /* diramazione: verticale nella corsia del figlio fino alla riga del
         * genitore, poi orizzontale fino alla corsia del genitore */
        CHECK(l1->kind == RENDER_LINE_BRANCH && l1->n == 3 && !l1->dashed &&
                  l1->lane == 0,
              "arco 1: diramazione, 3 punti, corsia 0");
        CHECK(l1->n == 3 && pt_eq(l1->pts[0], 0, 1) && pt_eq(l1->pts[1], 0, 2) &&
                  pt_eq(l1->pts[2], 1, 2),
              "arco 1: (0,1) -> (0,2) -> (1,2)");
        /* merge: orizzontale alla riga del commit fino alla corsia del
         * genitore, poi verticale in quella corsia fino al genitore */
        CHECK(l2->kind == RENDER_LINE_MERGE_PARENT && l2->n == 3 &&
                  !l2->dashed && l2->lane == 1,
              "arco 2: merge, 3 punti, corsia 1");
        CHECK(l2->n == 3 && pt_eq(l2->pts[0], 1, 2) && pt_eq(l2->pts[1], 0, 2) &&
                  pt_eq(l2->pts[2], 0, 0),
              "arco 2: (1,2) -> (0,2) -> (0,0)");
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

/* ---- caricamento comune --------------------------------------------------- */

static bool build_layout(const char *repo, GitData *data, GraphConfig *cfg,
                         History *hist, Layout *lay)
{
    char err[1024];
    if (!gitdata_load(data, repo, err, sizeof err)) {
        CHECK(false, "repository '%s' non caricabile: %s", repo, err);
        return false;
    }
    /* configurazione implicita (tronco main): i repository sintetici non hanno
     * graph.cfg, e sul repository di prova la sezione [parents] non cambia il
     * percorso degli archi tra commit (segue sempre i genitori reali). */
    char **names = xmalloc((data->ref_count ? data->ref_count : 1) *
                           sizeof(char *));
    size_t nn = 0;
    for (size_t i = 0; i < data->ref_count; i++)
        if (data->refs[i].kind == REF_LOCAL_BRANCH)
            names[nn++] = data->refs[i].name;
    memset(cfg, 0, sizeof *cfg);
    config_set_implicit_trunk(cfg, names, nn);
    free(names);
    if (!history_build(hist, data, cfg, err, sizeof err)) {
        CHECK(false, "history_build su '%s': %s", repo, err);
        config_free(cfg);
        gitdata_free(data);
        return false;
    }
    layout_build(lay, hist);
    return true;
}

static void free_all(GitData *data, GraphConfig *cfg, History *hist,
                     Layout *lay)
{
    layout_free(lay);
    history_free(hist);
    config_free(cfg);
    gitdata_free(data);
}

/* ---- controlli comuni su una scena --------------------------------------- */

/* Numero atteso di archi pieni: un arco per ogni arco tra commit con entrambe
 * le punte nel log. */
static size_t expected_solid(const Layout *lay)
{
    size_t n = 0;
    for (size_t e = 0; e < lay->cedge_count; e++)
        if (lay->cedges[e].from < lay->commit_count &&
            lay->cedges[e].to < lay->commit_count)
            n++;
    return n;
}

static size_t expected_suggested(const Layout *lay)
{
    size_t n = 0;
    for (size_t e = 0; e < lay->edge_count; e++) {
        if (lay->edges[e].kind != LAYOUT_EDGE_SUGGESTED)
            continue;
        if (lay->nodes[lay->edges[e].child].row != NO_IDX &&
            lay->nodes[lay->edges[e].parent].row != NO_IDX)
            n++;
    }
    return n;
}

/* Forma dell'arco: verticale di 2 punti, oppure 3 punti con il gomito su una
 * riga (diramazione: verticale poi orizzontale; merge/suggerito: orizzontale
 * poi verticale). */
static void check_line_shape(const Render *sc, const Layout *lay, const char *tag)
{
    for (size_t i = 0; i < sc->line_count; i++) {
        const RenderLine *ln = &sc->lines[i];
        if (ln->n != 2 && ln->n != 3) {
            CHECK(false, "%s: arco %zu con %zu punti", tag, i, ln->n);
            return;
        }
        if (ln->pts[0].x != (double)(size_t)ln->pts[0].x ||
            ln->pts[0].y != (double)(size_t)ln->pts[0].y ||
            ln->pts[ln->n - 1].x != (double)(size_t)ln->pts[ln->n - 1].x ||
            ln->pts[ln->n - 1].y != (double)(size_t)ln->pts[ln->n - 1].y) {
            CHECK(false, "%s: arco %zu con estremi non interi", tag, i);
            return;
        }
        if (ln->lane != lay->commits[(size_t)ln->pts[0].y].lane) {
            CHECK(false, "%s: arco %zu con corsia diversa dal figlio", tag, i);
            return;
        }
        if (ln->n == 2) {
            if (ln->pts[0].x != ln->pts[1].x) {
                CHECK(false, "%s: arco %zu verticale con corsie diverse", tag, i);
                return;
            }
        } else if (ln->kind == RENDER_LINE_BRANCH) {
            if (ln->pts[0].x != ln->pts[1].x ||
                ln->pts[1].y != ln->pts[2].y) {
                CHECK(false, "%s: arco %zu di diramazione mal formato", tag, i);
                return;
            }
        } else {
            if (ln->pts[0].y != ln->pts[1].y ||
                ln->pts[1].x != ln->pts[2].x) {
                CHECK(false, "%s: arco %zu di merge mal formato", tag, i);
                return;
            }
        }
    }
    CHECK(true, "%s: forma degli archi coerente", tag);
}

/* Controlla una scena completa su un repository. */
static void check_scene(const char *repo, const char *tag)
{
    GitData data;
    GraphConfig cfg;
    History hist;
    Layout lay;
    if (!build_layout(repo, &data, &cfg, &hist, &lay)) {
        CHECK(false, "%s: repository non costruibile", tag);
        return;
    }
    Render sc;
    render_build(&sc, &data, &hist, &lay);

    /* punti: uno per commit, alla sua cella */
    CHECK(sc.dot_count == lay.commit_count, "%s: punti == commit (%zu vs %zu)",
          tag, sc.dot_count, lay.commit_count);
    CHECK(sc.lane_count == lay.lanes, "%s: corsie == layout", tag);
    CHECK(sc.rows == lay.rows, "%s: righe == layout", tag);
    for (size_t ci = 0; ci < sc.dot_count; ci++) {
        const RenderDot *d = &sc.dots[ci];
        const LayoutCommit *cell = &lay.commits[ci];
        if (d->lane != cell->lane || d->row != cell->row || d->commit != ci ||
            !pt_eq(d->p, (double)cell->lane, (double)cell->row) ||
            d->anon != (cell->owner == NO_IDX)) {
            CHECK(false, "%s: punto %zu non coerente con la cella", tag, ci);
            break;
        }
    }

    size_t solid = expected_solid(&lay);
    size_t sugg = expected_suggested(&lay);
    CHECK(sc.line_count == solid + sugg,
          "%s: archi: attesi %zu, ottenuti %zu", tag, solid + sugg,
          sc.line_count);
    check_line_shape(&sc, &lay, tag);
    check_paths(&sc, &lay, tag, true);

    /* ancore: una per branch, alla posizione del suo nodo */
    CHECK(sc.anchor_count == hist.branch_count, "%s: ancore == branch (%zu vs %zu)",
          tag, sc.anchor_count, hist.branch_count);
    for (size_t i = 0; i < sc.anchor_count && i < hist.branch_count; i++) {
        const RenderAnchor *a = &sc.anchors[i];
        const LayoutNode *nd = &lay.nodes[i];
        if (a->branch != i || a->row != nd->row || a->p.x != (double)nd->lane ||
            a->is_head != hist.branches[i].is_head ||
            a->is_remote != hist.branches[i].is_remote) {
            CHECK(false, "%s: ancora %zu non coerente col nodo", tag, i);
            break;
        }
    }

    /* determinismo: seconda costruzione identica */
    Render sc2;
    render_build(&sc2, &data, &hist, &lay);
    CHECK(sc2.dot_count == sc.dot_count && sc2.line_count == sc.line_count &&
              sc2.anchor_count == sc.anchor_count,
          "%s: seconda costruzione: stesse lunghezze", tag);
    if (sc.dot_count == sc2.dot_count && sc.dot_count > 0)
        CHECK(memcmp(sc.dots, sc2.dots, sc.dot_count * sizeof(RenderDot)) == 0,
              "%s: punti identici fra due costruzioni", tag);
    if (sc.line_count == sc2.line_count && sc.line_count > 0)
        CHECK(memcmp(sc.lines, sc2.lines, sc.line_count * sizeof(RenderLine)) == 0,
              "%s: archi identici fra due costruzioni", tag);
    if (sc.anchor_count == sc2.anchor_count && sc.anchor_count > 0)
        CHECK(memcmp(sc.anchors, sc2.anchors,
                     sc.anchor_count * sizeof(RenderAnchor)) == 0,
              "%s: ancore identiche fra due costruzioni", tag);
    render_free(&sc2);

    printf("%s: %zu righe, %zu corsie, %zu archi, %zu etichette\n", tag,
           sc.rows, sc.lane_count, sc.line_count, sc.anchor_count);
    render_free(&sc);
    free_all(&data, &cfg, &hist, &lay);
}

/* ---- repository sintetici ------------------------------------------------- */

static char g_work[MAX_PATH];

static bool run_git(const char *dir, const char *const *args, char *err,
                    size_t err_size)
{
    SetCurrentDirectoryA(dir);
    StrBuf so, se;
    sb_init(&so);
    sb_init(&se);
    int code = -1;
    const char *argv[16];
    size_t n = 0;
    argv[n++] = "git";
    for (size_t i = 0; args[i] && n < 15; i++)
        argv[n++] = args[i];
    argv[n] = NULL;
    bool ok = gitdata_capture(argv, &so, &se, &code, err, err_size);
    if (!ok || code != 0) {
        fprintf(stderr, "  [git] in %s, uscita %d | errore: %s\n", dir, code,
                se.data ? se.data : "(vuoto)");
    }
    sb_free(&so);
    sb_free(&se);
    return ok && code == 0;
}

static void back_to(const char *dir)
{
    SetCurrentDirectoryA(dir);
}

static void remove_tree(const char *path)
{
    char temp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof temp, temp);
    if (n == 0 || n >= sizeof temp)
        return;
    size_t tl = strlen(temp);
    while (tl > 0 && (temp[tl - 1] == '\\' || temp[tl - 1] == '/'))
        temp[--tl] = '\0';
    if (strncmp(path, temp, tl) != 0)
        return;
    if (strstr(path, "gg_render") == NULL)
        return;
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
                continue;
            char child[MAX_PATH];
            snprintf(child, sizeof child, "%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                SetFileAttributesA(child, FILE_ATTRIBUTE_DIRECTORY);
                remove_tree(child);
            } else {
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(child);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path);
}

static bool commit_at(const char *path, long long t, const char *msg, char *err,
                      size_t err_size)
{
    char date[40];
    snprintf(date, sizeof date, "@%lld +0000", t);
    SetEnvironmentVariableA("GIT_AUTHOR_DATE", date);
    SetEnvironmentVariableA("GIT_COMMITTER_DATE", date);
    const char *cm[] = {"-c", "user.name=T", "-c", "user.email=t@t", "commit",
                        "-q", "--allow-empty", "-m", msg, NULL};
    bool ok = run_git(path, cm, err, err_size);
    SetEnvironmentVariableA("GIT_AUTHOR_DATE", NULL);
    SetEnvironmentVariableA("GIT_COMMITTER_DATE", NULL);
    return ok;
}

static bool merge_at(const char *path, long long t, const char *branch,
                     const char *msg, char *err, size_t err_size)
{
    char date[40];
    snprintf(date, sizeof date, "@%lld +0000", t);
    SetEnvironmentVariableA("GIT_AUTHOR_DATE", date);
    SetEnvironmentVariableA("GIT_COMMITTER_DATE", date);
    const char *mg[] = {"-c", "user.name=T", "-c", "user.email=t@t", "merge",
                        "-q", "--no-ff", branch, "-m", msg, NULL};
    bool ok = run_git(path, mg, err, err_size);
    SetEnvironmentVariableA("GIT_AUTHOR_DATE", NULL);
    SetEnvironmentVariableA("GIT_COMMITTER_DATE", NULL);
    return ok;
}

/* main A; x (X1,X2) e y (Y1,Y2) da A; M1 su main; merge --no-ff di x e di y;
 * z (Z1,Z2) dal merge di y; merge --no-ff di z; x, y e z cancellati. X e Y si
 * sovrappongono (corsie anonime distinte), Z riusa una corsia. */
static bool make_xy_repo(const char *path, char *err, size_t err_size)
{
    back_to(g_work);
    remove_tree(path);
    CreateDirectoryA(path, NULL);
    const char *init[] = {"init", "-b", "main", "-q", NULL};
    if (!run_git(path, init, err, err_size))
        return false;
    long long t = 1773000000;
    if (!commit_at(path, t, "A", err, err_size))
        return false;
    const char *cx[] = {"checkout", "-q", "-b", "x", "main", NULL};
    if (!run_git(path, cx, err, err_size))
        return false;
    if (!commit_at(path, t + 60, "X1", err, err_size) ||
        !commit_at(path, t + 120, "X2", err, err_size))
        return false;
    const char *cy[] = {"checkout", "-q", "-b", "y", "main", NULL};
    if (!run_git(path, cy, err, err_size))
        return false;
    if (!commit_at(path, t + 180, "Y1", err, err_size) ||
        !commit_at(path, t + 240, "Y2", err, err_size))
        return false;
    const char *cm[] = {"checkout", "-q", "main", NULL};
    if (!run_git(path, cm, err, err_size))
        return false;
    if (!commit_at(path, t + 300, "M1", err, err_size) ||
        !merge_at(path, t + 360, "x", "MX", err, err_size) ||
        !merge_at(path, t + 420, "y", "MY", err, err_size))
        return false;
    const char *cz[] = {"checkout", "-q", "-b", "z", "main", NULL};
    if (!run_git(path, cz, err, err_size))
        return false;
    if (!commit_at(path, t + 480, "Z1", err, err_size) ||
        !commit_at(path, t + 540, "Z2", err, err_size))
        return false;
    if (!run_git(path, cm, err, err_size))
        return false;
    if (!merge_at(path, t + 600, "z", "MZ", err, err_size))
        return false;
    const char *dx[] = {"branch", "-D", "x", NULL};
    const char *dy[] = {"branch", "-D", "y", NULL};
    const char *dz[] = {"branch", "-D", "z", NULL};
    return run_git(path, dx, err, err_size) &&
           run_git(path, dy, err, err_size) && run_git(path, dz, err, err_size);
}

/* main A; n rami b1..bn, ognuno creato dalla punta corrente di main con un
 * commit, poi mergiato --no-ff e cancellato: gli n commit restano anonimi ma
 * i loro intervalli non si sovrappongono (una sola corsia anonima). */
static bool make_many_deleted_repo(const char *path, int n, char *err,
                                   size_t err_size)
{
    back_to(g_work);
    remove_tree(path);
    CreateDirectoryA(path, NULL);
    const char *init[] = {"init", "-b", "main", "-q", NULL};
    if (!run_git(path, init, err, err_size))
        return false;
    long long t = 1774000000;
    if (!commit_at(path, t, "A", err, err_size))
        return false;
    const char *cm[] = {"checkout", "-q", "main", NULL};
    for (int i = 1; i <= n; i++) {
        char br[16], subj[16], msg[16];
        snprintf(br, sizeof br, "b%d", i);
        snprintf(subj, sizeof subj, "C%d", i);
        snprintf(msg, sizeof msg, "M%d", i);
        const char *cb[] = {"checkout", "-q", "-b", br, "main", NULL};
        if (!run_git(path, cb, err, err_size))
            return false;
        if (!commit_at(path, t + 60LL * i, subj, err, err_size))
            return false;
        if (!run_git(path, cm, err, err_size))
            return false;
        if (!merge_at(path, t + 60LL * i + 30, br, msg, err, err_size))
            return false;
        const char *db[] = {"branch", "-D", br, NULL};
        if (!run_git(path, db, err, err_size))
            return false;
    }
    return true;
}

/* Generatore pseudo-casuale deterministico (seme fisso): la riproduzione e'
 * sempre la stessa. */
static unsigned g_rng;

static unsigned rnd(unsigned n)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (g_rng >> 16) % (n ? n : 1);
}

/* Repository con rami che si sovrappongono: rami creati da commit diversi di
 * main, alcuni mergiati e cancellati (catene anonime sovrapposte), altri
 * mergiati e conservati (rami con nome). Seme fisso. */
static bool make_random_repo(const char *path, char *err, size_t err_size)
{
    back_to(g_work);
    remove_tree(path);
    CreateDirectoryA(path, NULL);
    const char *init[] = {"init", "-b", "main", "-q", NULL};
    if (!run_git(path, init, err, err_size))
        return false;
    long long t = 1776000000;
    unsigned depth = 0;
    for (int i = 0; i < 3; i++) {
        char msg[16];
        snprintf(msg, sizeof msg, "A%d", i);
        if (!commit_at(path, t, msg, err, err_size))
            return false;
        t += 60;
        depth++;
    }
    const char *cm[] = {"checkout", "-q", "main", NULL};
    g_rng = 0xC0FFEEu;
    int kept = 0, deleted = 0;
    for (int i = 0; i < 18; i++) {
        unsigned back = rnd(depth < 3 ? depth : 3); /* main~back */
        char base[32], br[16];
        snprintf(base, sizeof base, "main~%u", back);
        snprintf(br, sizeof br, "r%d", i);
        const char *cb[] = {"checkout", "-q", "-b", br, base, NULL};
        if (!run_git(path, cb, err, err_size))
            return false;
        int nc = 1 + (int)rnd(2);
        for (int j = 0; j < nc; j++) {
            char msg[24];
            snprintf(msg, sizeof msg, "R%d_%d", i, j);
            if (!commit_at(path, t, msg, err, err_size))
                return false;
            t += 60;
        }
        if (!run_git(path, cm, err, err_size))
            return false;
        char merge_msg[24];
        snprintf(merge_msg, sizeof merge_msg, "MR%d", i);
        if (!merge_at(path, t, br, merge_msg, err, err_size))
            return false;
        t += 60;
        depth++;
        if (rnd(2) == 0) {
            const char *db[] = {"branch", "-D", br, NULL};
            if (!run_git(path, db, err, err_size))
                return false;
            deleted++;
        } else {
            kept++;
        }
    }
    printf("casuale: %d rami conservati, %d cancellati\n", kept, deleted);
    return kept > 0 && deleted > 0;
}

/* ---- pytest-style entry points ------------------------------------------- */

static int g_rc = 0;

static void run_synthetic_repos(void)
{
    char err[1024];
    DWORD wn = GetCurrentDirectoryA(sizeof g_work, g_work);
    if (wn == 0 || wn >= sizeof g_work) {
        fprintf(stderr, "directory corrente non leggibile\n");
        g_rc = 2;
        return;
    }
    char root[MAX_PATH];
    DWORD rn = GetTempPathA(sizeof root, root);
    if (rn == 0 || rn >= sizeof root) {
        fprintf(stderr, "percorso temporaneo non ottenibile\n");
        g_rc = 2;
        return;
    }
    size_t rl = strlen(root);
    while (rl > 0 && (root[rl - 1] == '\\' || root[rl - 1] == '/'))
        root[--rl] = '\0';

    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\gg_render_xy", root);
    if (!make_xy_repo(path, err, sizeof err)) {
        CHECK(false, "creazione del repo x/y/z fallita: %s", err);
    } else {
        check_scene(path, "x/y/z");
    }
    back_to(g_work);
    remove_tree(path);

    snprintf(path, sizeof path, "%s\\gg_render_many", root);
    if (!make_many_deleted_repo(path, 30, err, sizeof err)) {
        CHECK(false, "creazione del repo con 30 rami fallita: %s", err);
    } else {
        check_scene(path, "30 rami cancellati");
    }
    back_to(g_work);
    remove_tree(path);

    snprintf(path, sizeof path, "%s\\gg_render_random", root);
    if (!make_random_repo(path, err, sizeof err)) {
        CHECK(false, "creazione del repo casuale fallita: %s", err);
    } else {
        check_scene(path, "casuale");
    }
    back_to(g_work);
    remove_tree(path);
}

int main(int argc, char **argv)
{
    test_hand_built();
    test_empty();
    test_palette();

    if (argc >= 2) {
        check_scene(argv[1], "repo di prova");
    } else {
        fprintf(stderr, "avviso: nessun repository di prova: uso i soli sintetici\n");
    }

    run_synthetic_repos();
    if (g_rc != 0)
        return g_rc;

    printf("render: %d controlli passati, %d falliti\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

/* test_layout.c - test automatici della disposizione del grafo (fase 3/4).
 *
 * Uso: test_layout <percorso-repo-di-test>
 *
 * Verifica, sul repository di prova e su repository sintetici costruiti qui,
 * che la disposizione sia coerente col modello e non dipenda dai nomi:
 *  - un nodo per branch, con la riga della punta e la corsia della famiglia;
 *  - il tronco (long-lived) nella corsia 0, i long-lived nelle prime corsie;
 *  - i riferimenti sulla stessa punta (alias) nella stessa corsia; un remoto
 *    la cui punta sta sulla catena dei primi genitori del locale con lo stesso
 *    nome e' un'etichetta e non ha una corsia propria (label_of);
 *  - un arco tratteggiato verso il padre suggerito dei casi ambigui noti
 *    (branch1 -> main, branch2 -> branch1);
 *  - corsie contigue 0..lanes-1; il blocco anonimo (una o piu' corsie) sta
 *    subito dopo i tronchi e nessun nodo-branch lo usa;
 *  - ogni commit con riga, corsia del proprietario (o del blocco anonimo) e
 *    proprietario; la catena dei primi genitori di un tronco resta sulla sua
 *    corsia; l'arco di merge punta al secondo genitore; l'arco di diramazione
 *    va dal commit proprietario piu' vecchio (o dal fondo di una catena
 *    anonima) al suo primo genitore, in un'altra corsia;
 *  - tabella scritta a mano (commit e archi tra commit) su un repository
 *    sintetico (main A,B,C; branch1 B1,B2,B3 da B; branch2 C1,C2 da B2;
 *    merge --no-ff; origin/main in anticipo e upstream/main in ritardo) e
 *    tabella degli archi scritta a mano per il repository di prova;
 *  - catene anonime: su un repository con due rami cancellati che si
 *    sovrappongono (x e y) piu' un terzo creato dopo i merge (z) le catene
 *    stanno in due corsie anonime (X e Y distinte, Z riusa); su un repository
 *    con 30 rami creati, mergiati e cancellati in sequenza una sola corsia;
 *  - un modello artificiale con un ciclo di paternita': le famiglie che la
 *    visita in profondita' non raggiunge ricevono comunque una corsia;
 *  - determinismo: due costruzioni successive identiche.
 *
 * La proprieta' condivisa gg_check_layout ripete le regole su ogni modello
 * (anche in test_core, test_config, test_scale, test_invariance).
 *
 * Esce con 0 se tutti i test passano, 1 se qualcuno fallisce, 2 su errore di
 * preparazione.
 */
#include "config.h"
#include "gitdata.h"
#include "history.h"
#include "layout.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

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

#define NO_TIP ((size_t)-1)

/* proprieta' condivisa: serve la macro CHECK definita sopra */
#include "gg_checks.h"

static const LayoutNode *node_named(const History *h, const Layout *l,
                                    const char *name)
{
    size_t i = history_find_branch(h, name);
    return i == (size_t)-1 ? NULL : &l->nodes[i];
}

static bool edge_exists(const Layout *l, size_t child, size_t parent,
                        LayoutEdgeKind kind)
{
    for (size_t e = 0; e < l->edge_count; e++)
        if (l->edges[e].child == child && l->edges[e].parent == parent &&
            l->edges[e].kind == kind)
            return true;
    return false;
}

static bool cedge_exists(const Layout *l, size_t from, size_t to,
                         LayoutCommitEdgeKind kind)
{
    for (size_t e = 0; e < l->cedge_count; e++)
        if (l->cedges[e].from == from && l->cedges[e].to == to &&
            l->cedges[e].kind == kind)
            return true;
    return false;
}

/* Indice dell'`occ`-esima occorrenza (0 = la piu' recente) di un commit la cui
 * prima riga di messaggio e' `subject` (o comincia con `subject` seguito da
 * ':'): i soggetti del repository di prova non sono prefissi l'uno dell'altro,
 * tranne le due "RB1" (rb e rb-old) che si distinguono per occorrenza. */
static size_t commit_subject_n(const GitData *d, const char *subject,
                               unsigned occ)
{
    size_t n = strlen(subject);
    for (size_t i = 0; i < d->commit_count; i++) {
        const char *m = d->commits[i].message;
        if (strncmp(m, subject, n) == 0 &&
            (m[n] == '\0' || m[n] == '\n' || m[n] == '\r' || m[n] == ':')) {
            if (occ == 0)
                return i;
            occ--;
        }
    }
    return NO_TIP;
}

static size_t commit_subject(const GitData *d, const char *subject)
{
    return commit_subject_n(d, subject, 0);
}

/* Carica un repository e ne costruisce modello e disposizione con la sola
 * configurazione implicita (tronco main). Segnala l'errore e ritorna false. */
static bool build_layout(const char *repo, GitData *data, GraphConfig *cfg,
                         History *hist, Layout *lay)
{
    char err[1024];
    if (!gitdata_load(data, repo, err, sizeof err)) {
        CHECK(false, "repository '%s' non caricabile: %s", repo, err);
        return false;
    }
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

static void free_layout(GitData *data, GraphConfig *cfg, History *hist,
                        Layout *lay)
{
    layout_free(lay);
    history_free(hist);
    config_free(cfg);
    gitdata_free(data);
}

/* ---------- repository sintetico: tabella attesa scritta a mano ---------- */

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
    if (strstr(path, "gg_layout") == NULL)
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

/* main A,B,C; branch1 (B1,B2,B3) da B; branch2 (C1,C2) da B2; merge --no-ff
 * di branch2 in main (M); origin/main in ANTICIPO su main (X, un commit oltre
 * la punta, solo il remoto lo referenzia) e upstream/main in RITARDO (B). */
static bool make_table_repo(const char *path, char *err, size_t err_size)
{
    back_to(g_work);
    remove_tree(path);
    CreateDirectoryA(path, NULL);
    const char *init[] = {"init", "-b", "main", "-q", NULL};
    if (!run_git(path, init, err, err_size))
        return false;
    long long t = 1772000000;
    if (!commit_at(path, t, "A", err, err_size) ||
        !commit_at(path, t + 60, "B", err, err_size) ||
        !commit_at(path, t + 120, "C", err, err_size))
        return false;
    const char *co_b1[] = {"checkout", "-q", "-b", "branch1", "main~1", NULL};
    if (!run_git(path, co_b1, err, err_size))
        return false;
    if (!commit_at(path, t + 180, "B1", err, err_size) ||
        !commit_at(path, t + 240, "B2", err, err_size) ||
        !commit_at(path, t + 300, "B3", err, err_size))
        return false;
    const char *co_b2[] = {"checkout", "-q", "-b", "branch2", "branch1~1", NULL};
    if (!run_git(path, co_b2, err, err_size))
        return false;
    if (!commit_at(path, t + 360, "C1", err, err_size) ||
        !commit_at(path, t + 420, "C2", err, err_size))
        return false;
    const char *co_main[] = {"checkout", "-q", "main", NULL};
    if (!run_git(path, co_main, err, err_size))
        return false;
    if (!merge_at(path, t + 480, "branch2", "M", err, err_size))
        return false;
    const char *co_tmp[] = {"checkout", "-q", "-b", "tmp", "main", NULL};
    if (!run_git(path, co_tmp, err, err_size))
        return false;
    if (!commit_at(path, t + 540, "X", err, err_size))
        return false;
    if (!run_git(path, co_main, err, err_size))
        return false;
    const char *om[] = {"update-ref", "refs/remotes/origin/main", "tmp", NULL};
    const char *um[] = {"update-ref", "refs/remotes/upstream/main", "main~2", NULL};
    const char *dt[] = {"branch", "-D", "tmp", NULL};
    return run_git(path, om, err, err_size) &&
           run_git(path, um, err, err_size) &&
           run_git(path, dt, err, err_size);
}

/* Tabella attesa del repository sintetico, scritta a mano: riga (posizione
 * topologica), corsia del proprietario, proprietario. */
static void check_table(const char *repo)
{
    char err[1024];
    GitData data;
    if (!gitdata_load(&data, repo, err, sizeof err)) {
        CHECK(false, "repository sintetico non caricabile: %s", err);
        return;
    }
    char **names = xmalloc((data.ref_count ? data.ref_count : 1) * sizeof(char *));
    size_t nn = 0;
    for (size_t i = 0; i < data.ref_count; i++)
        if (data.refs[i].kind == REF_LOCAL_BRANCH)
            names[nn++] = data.refs[i].name;
    GraphConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    config_set_implicit_trunk(&cfg, names, nn);
    free(names);

    History hist;
    if (!history_build(&hist, &data, &cfg, err, sizeof err)) {
        CHECK(false, "history_build sul repository sintetico: %s", err);
        config_free(&cfg);
        gitdata_free(&data);
        return;
    }
    Layout lay;
    layout_build(&lay, &hist);
    gg_check_layout(&lay, &hist, "repository sintetico");

    const struct {
        const char *soggetto;
        size_t riga;
        size_t corsia;
        const char *proprietario;
    } attesa[] = {
        {"X", 0, 0, "origin/main"},
        {"M", 1, 0, "main"},
        {"C2", 2, 2, "branch2"},
        {"C1", 3, 2, "branch2"},
        {"C", 4, 0, "main"},
        {"B3", 5, 1, "branch1"},
        {"B2", 6, 1, "branch1"},
        {"B1", 7, 1, "branch1"},
        {"B", 8, 0, "main"},
        {"A", 9, 0, "main"},
    };
    CHECK(lay.commit_count == 10, "sintetico: %zu commit, attesi 10",
          lay.commit_count);
    CHECK(lay.lanes == 3, "sintetico: %zu corsie, attese 3", lay.lanes);
    CHECK(lay.anon_lane == NO_TIP, "sintetico: corsia anonima non attesa");
    unsigned char *visti = xcalloc(lay.commit_count ? lay.commit_count : 1, 1);
    for (size_t k = 0; k < sizeof attesa / sizeof attesa[0]; k++) {
        size_t ci = commit_subject(&data, attesa[k].soggetto);
        CHECK(ci != NO_TIP, "sintetico: commit '%s' assente", attesa[k].soggetto);
        if (ci == NO_TIP)
            continue;
        CHECK(!visti[ci], "sintetico: commit '%s' compare due volte",
              attesa[k].soggetto);
        visti[ci] = 1;
        const LayoutCommit *cell = &lay.commits[ci];
        CHECK(cell->row == attesa[k].riga,
              "sintetico: '%s' riga %zu, attesa %zu", attesa[k].soggetto,
              cell->row, attesa[k].riga);
        CHECK(cell->lane == attesa[k].corsia,
              "sintetico: '%s' corsia %zu, attesa %zu", attesa[k].soggetto,
              cell->lane, attesa[k].corsia);
        const char *owner = cell->owner == NO_TIP
                                ? "-"
                                : hist.branches[cell->owner].name;
        CHECK(cell->owner != NO_TIP &&
                  strcmp(owner, attesa[k].proprietario) == 0,
              "sintetico: '%s' proprietario '%s', atteso '%s'",
              attesa[k].soggetto, owner, attesa[k].proprietario);
    }
    for (size_t ci = 0; ci < lay.commit_count; ci++)
        CHECK(visti[ci], "sintetico: commit in riga %zu non compare nella "
                         "tabella attesa", ci);
    free(visti);

    /* la catena dei primi genitori di main resta sulla corsia del tronco */
    size_t mi = history_find_branch(&hist, "main");
    CHECK(mi != NO_TIP, "sintetico: branch main assente");
    if (mi == NO_TIP) {
        layout_free(&lay);
        history_free(&hist);
        config_free(&cfg);
        gitdata_free(&data);
        return;
    }
    size_t m = commit_subject(&data, "M");
    while (m != NO_TIP) {
        CHECK(lay.commits[m].owner == mi,
              "sintetico: il commit in riga %zu non e' di main", m);
        CHECK(lay.commits[m].lane == lay.nodes[mi].lane,
              "sintetico: il commit in riga %zu ha lasciato la corsia di main", m);
        m = data.commits[m].parent_count ? data.commits[m].parent_idx[0] : NO_TIP;
    }

    /* l'arco di merge punta al secondo genitore; l'arco di diramazione al fork */
    size_t mm = commit_subject(&data, "M");
    size_t c2 = commit_subject(&data, "C2");
    CHECK(cedge_exists(&lay, mm, c2, LAYOUT_CEDGE_MERGE_PARENT),
          "sintetico: manca l'arco di merge M -> C2 (secondo genitore)");
    size_t x = commit_subject(&data, "X");
    CHECK(cedge_exists(&lay, x, mm, LAYOUT_CEDGE_FIRST_PARENT) &&
              lay.commits[x].owner != NO_TIP &&
              strcmp(hist.branches[lay.commits[x].owner].name, "origin/main") == 0,
          "sintetico: X (in anticipo) non e' di origin/main o non e' figlio di M");

    /* tabella scritta a mano degli archi tra commit: un arco per genitore, col
     * tipo dedotto dalle corsie. B1 e C1 sono i commit proprietari piu' vecchi
     * di branch1 e branch2: il loro primo genitore sta in un'altra corsia, e
     * l'arco di diramazione va li' (B1 -> B, C1 -> B2), non al fork dedotto del
     * modello. */
    {
        static const struct {
            const char *from;
            const char *to;
            LayoutCommitEdgeKind kind;
        } attesi[] = {
            {"X", "M", LAYOUT_CEDGE_FIRST_PARENT},
            {"M", "C", LAYOUT_CEDGE_FIRST_PARENT},
            {"M", "C2", LAYOUT_CEDGE_MERGE_PARENT},
            {"C2", "C1", LAYOUT_CEDGE_FIRST_PARENT},
            {"C1", "B2", LAYOUT_CEDGE_BRANCH},
            {"C", "B", LAYOUT_CEDGE_FIRST_PARENT},
            {"B3", "B2", LAYOUT_CEDGE_FIRST_PARENT},
            {"B2", "B1", LAYOUT_CEDGE_FIRST_PARENT},
            {"B1", "B", LAYOUT_CEDGE_BRANCH},
            {"B", "A", LAYOUT_CEDGE_FIRST_PARENT},
        };
        size_t n_attesi = sizeof attesi / sizeof attesi[0];
        CHECK(lay.cedge_count == n_attesi,
              "sintetico: %zu archi tra commit, attesi %zu", lay.cedge_count,
              n_attesi);
        for (size_t k = 0; k < n_attesi; k++) {
            size_t from = commit_subject(&data, attesi[k].from);
            size_t to = commit_subject(&data, attesi[k].to);
            CHECK(from != NO_TIP && to != NO_TIP,
                  "sintetico: commit '%s' o '%s' assente", attesi[k].from,
                  attesi[k].to);
            if (from == NO_TIP || to == NO_TIP)
                continue;
            CHECK(cedge_exists(&lay, from, to, attesi[k].kind),
                  "sintetico: manca l'arco '%s' -> '%s' (tipo %d)",
                  attesi[k].from, attesi[k].to, (int)attesi[k].kind);
        }
    }

    printf("sintetico: %zu righe, %zu corsie, %zu archi tra commit\n",
           lay.commit_count, lay.lanes, lay.cedge_count);

    layout_free(&lay);
    history_free(&hist);
    config_free(&cfg);
    gitdata_free(&data);
}

/* ---------- catene anonime: corsie con riuso ------------------------------ */

/* main A; x (X1,X2) e y (Y1,Y2) da A; M1 su main; merge --no-ff di x e di y;
 * z (Z1,Z2) dal merge di y; merge --no-ff di z; x, y e z cancellati. I sei
 * commit di x/y/z restano anonimi: X e Y (stesso punto di diramazione A) si
 * sovrappongono e stanno in corsie diverse, Z (creato dopo i due merge) non si
 * sovrappone a nessuno dei due e riusa una corsia. */
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

static void check_xy_repo(const char *repo)
{
    GitData data;
    GraphConfig cfg;
    History hist;
    Layout lay;
    if (!build_layout(repo, &data, &cfg, &hist, &lay))
        return;
    gg_check_layout(&lay, &hist, "x/y cancellati");

    size_t x1 = commit_subject(&data, "X1");
    size_t y1 = commit_subject(&data, "Y1");
    size_t z1 = commit_subject(&data, "Z1");
    CHECK(x1 != NO_TIP && y1 != NO_TIP && z1 != NO_TIP, "x/y: commit mancanti");
    CHECK(lay.anon_lane != NO_TIP, "x/y: nessun blocco anonimo");
    CHECK(lay.anon_lane_count == 2,
          "x/y: %zu corsie anonime, attese 2 (X e Y sovrapposte, Z riusa)",
          lay.anon_lane_count);
    if (x1 != NO_TIP && y1 != NO_TIP && z1 != NO_TIP) {
        CHECK(lay.commits[x1].owner == NO_TIP &&
                  lay.commits[y1].owner == NO_TIP &&
                  lay.commits[z1].owner == NO_TIP,
              "x/y: x, y o z hanno un proprietario");
        size_t lx = lay.commits[x1].lane;
        size_t ly = lay.commits[y1].lane;
        size_t lz = lay.commits[z1].lane;
        CHECK(lx != ly, "x/y: X e Y nella stessa corsia %zu", lx);
        CHECK(lz == lx || lz == ly,
              "x/y: Z in corsia %zu non riusa ne' X (%zu) ne' Y (%zu)", lz, lx,
              ly);
        /* X2 -> X1 resta nella corsia anonima della catena */
        size_t x2 = commit_subject(&data, "X2");
        if (x2 != NO_TIP)
            CHECK(lay.commits[x2].lane == lx &&
                      cedge_exists(&lay, x2, x1, LAYOUT_CEDGE_FIRST_PARENT),
                  "x/y: X2 non resta nella corsia di X1");
        /* il fondo di ogni catena si dirama verso main (corsia 0) */
        size_t a = commit_subject(&data, "A");
        if (a != NO_TIP)
            CHECK(cedge_exists(&lay, x1, a, LAYOUT_CEDGE_BRANCH) &&
                      cedge_exists(&lay, y1, a, LAYOUT_CEDGE_BRANCH),
                  "x/y: X1 o Y1 senza arco di diramazione verso A");
        size_t my = commit_subject(&data, "MY");
        if (my != NO_TIP)
            CHECK(cedge_exists(&lay, z1, my, LAYOUT_CEDGE_BRANCH),
                  "x/y: Z1 senza arco di diramazione verso MY");
    }
    printf("x/y/z cancellati: %zu corsie anonime (X e Y distinte, Z riusa)\n",
           lay.anon_lane_count);

    free_layout(&data, &cfg, &hist, &lay);
}

/* main A; n rami b1..bn, ognuno creato dalla punta corrente di main con un
 * commit, poi mergiato --no-ff e cancellato. Gli n commit restano anonimi ma i
 * loro intervalli non si sovrappongono: una sola corsia anonima. */
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

static void check_many_deleted(const char *repo, int n)
{
    GitData data;
    GraphConfig cfg;
    History hist;
    Layout lay;
    if (!build_layout(repo, &data, &cfg, &hist, &lay))
        return;
    gg_check_layout(&lay, &hist, "rami cancellati in sequenza");

    CHECK(lay.anon_lane != NO_TIP, "sequenza: nessun blocco anonimo");
    CHECK(lay.anon_lane_count == 1,
          "%d rami mergiati e cancellati: %zu corsie anonime, attesa 1", n,
          lay.anon_lane_count);
    for (int i = 1; i <= n; i++) {
        char subj[16];
        snprintf(subj, sizeof subj, "C%d", i);
        size_t ci = commit_subject(&data, subj);
        CHECK(ci != NO_TIP, "sequenza: commit '%s' assente", subj);
        if (ci == NO_TIP)
            continue;
        CHECK(lay.commits[ci].owner == NO_TIP,
              "sequenza: '%s' ha un proprietario", subj);
        CHECK(lay.commits[ci].lane == lay.anon_lane,
              "sequenza: '%s' in corsia %zu, attesa l'anonima %zu", subj,
              lay.commits[ci].lane, lay.anon_lane);
    }
    printf("%d rami cancellati: %zu corsia anonima su %zu corsie totali\n", n,
           lay.anon_lane_count, lay.lanes);

    free_layout(&data, &cfg, &hist, &lay);
}

/* ---------- modello artificiale: ciclo di paternita' ---------------------- */

/* Due famiglie che si indicano a vicenda: nessuna e' radice, quindi la visita
 * in profondita' non le raggiunge. Devono comunque ricevere una corsia, in
 * coda, diversa l'una dall'altra. */
static void check_unassigned_families(void)
{
    History h;
    memset(&h, 0, sizeof h);
    h.branch_count = 2;
    h.branches = xcalloc(2, sizeof(BranchNode));
    for (size_t i = 0; i < 2; i++) {
        BranchNode *b = &h.branches[i];
        b->name = xstrdup(i == 0 ? "giro-a" : "giro-b");
        b->canonical = xstrdup(b->name);
        b->motivation = xstrdup("modello artificiale");
        b->tip_pos = NO_TIP;
        b->fork_pos = NO_TIP;
        b->first_own_pos = NO_TIP;
        b->parent = xstrdup(i == 0 ? "giro-b" : "giro-a");
        b->state = PARENT_DEDUCED;
    }
    Layout lay;
    layout_build(&lay, &h);
    CHECK(lay.lanes == 2,
          "artificiale: %zu corsie, attese 2 (le famiglie non raggiunte)"
          , lay.lanes);
    CHECK(lay.node_count == 2, "artificiale: %zu nodi, attesi 2", lay.node_count);
    if (lay.node_count == 2) {
        CHECK(lay.nodes[0].lane != lay.nodes[1].lane,
              "artificiale: famiglie non raggiunte nella stessa corsia (%zu)",
              lay.nodes[0].lane);
        CHECK(lay.nodes[0].lane < lay.lanes && lay.nodes[1].lane < lay.lanes,
              "artificiale: corsia fuori intervallo");
    }
    CHECK(lay.commit_count == 0 && lay.anon_lane == NO_TIP,
          "artificiale: nessun commit atteso");
    layout_free(&lay);
    history_free(&h);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) {
        fprintf(stderr, "uso: test_layout <percorso-repo-di-test>\n");
        return 2;
    }

    /* il modello artificiale non ha bisogno di un repository */
    check_unassigned_families();

    GitData data;
    char err[1024];
    if (!gitdata_load(&data, argv[1], err, sizeof err)) {
        fprintf(stderr, "PREREQUISITO FALLITO: %s\n", err);
        return 2;
    }

    /* configurazione implicita: tronco main */
    char **names = xmalloc((data.ref_count ? data.ref_count : 1) * sizeof(char *));
    size_t nn = 0;
    for (size_t i = 0; i < data.ref_count; i++)
        if (data.refs[i].kind == REF_LOCAL_BRANCH)
            names[nn++] = data.refs[i].name;
    GraphConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    config_set_implicit_trunk(&cfg, names, nn);
    free(names);

    History hist;
    REQUIRE(history_build(&hist, &data, &cfg, err, sizeof err),
            "history_build fallita");

    Layout lay;
    layout_build(&lay, &hist);
    gg_check_layout(&lay, &hist, "fixture principale");

    /* forma generale */
    CHECK(lay.node_count == hist.branch_count,
          "nodi = %zu, attesi %zu (un nodo per branch)", lay.node_count,
          hist.branch_count);
    CHECK(lay.rows == data.commit_count, "righe = %zu, commit = %zu", lay.rows,
          data.commit_count);
    CHECK(lay.commit_count == data.commit_count,
          "commit nel layout = %zu, commit = %zu", lay.commit_count,
          data.commit_count);

    /* corsie contigue 0..lanes-1: usate dai nodi o dalla corsia anonima */
    if (lay.lanes > 0) {
        unsigned char *usata = xcalloc(lay.lanes, 1);
        for (size_t i = 0; i < lay.node_count; i++) {
            CHECK(lay.nodes[i].lane < lay.lanes, "'%s' corsia fuori intervallo",
                  hist.branches[i].name);
            if (lay.nodes[i].lane < lay.lanes)
                usata[lay.nodes[i].lane] = 1;
        }
        for (size_t k = 0; k < lay.anon_lane_count && lay.anon_lane != NO_TIP;
             k++) {
            size_t lane = lay.anon_lane + k;
            CHECK(lane < lay.lanes, "corsia anonima %zu fuori intervallo", lane);
            if (lane < lay.lanes)
                usata[lane] = 1;
        }
        for (size_t k = 0; k < lay.lanes; k++)
            CHECK(usata[k], "corsia %zu non usata da nessuno", k);
        free(usata);
    }

    /* il tronco (main, long-lived, primo in [long-lived]) e' la corsia 0 */
    const LayoutNode *mn = node_named(&hist, &lay, "main");
    REQUIRE(mn != NULL, "branch main assente");
    CHECK(mn->lane == 0, "main: corsia %zu, attesa 0 (tronco)", mn->lane);

    /* i long-lived occupano le prime corsie, una per rank (qui solo main, ma la
     * regola resta: la corsia di un long-lived e' il suo rango) */
    for (size_t i = 0; i < hist.branch_count; i++) {
        const BranchNode *b = &hist.branches[i];
        if (!b->is_long_lived || b->long_lived_rank < 0)
            continue;
        CHECK(lay.nodes[i].lane == (size_t)b->long_lived_rank,
              "'%s' long-lived: corsia %zu, atteso il rango %d", b->name,
              lay.nodes[i].lane, b->long_lived_rank);
    }

    /* sulla stessa punta (alias) stessa riga e stessa corsia: origin/branch1
     * e' specchio provato di branch1; origin/feature-x e' alias con nome
     * diverso */
    const LayoutNode *b1 = node_named(&hist, &lay, "branch1");
    const LayoutNode *ob1 = node_named(&hist, &lay, "origin/branch1");
    REQUIRE(b1 != NULL, "branch1 assente");
    REQUIRE(ob1 != NULL, "origin/branch1 assente");
    CHECK(b1->lane == ob1->lane && b1->row == ob1->row,
          "origin/branch1 (specchio) in posizione diversa da branch1");
    const LayoutNode *fx = node_named(&hist, &lay, "feature/x");
    const LayoutNode *ofx = node_named(&hist, &lay, "origin/feature-x");
    REQUIRE(fx != NULL, "feature/x assente");
    REQUIRE(ofx != NULL, "origin/feature-x assente");
    CHECK(fx->lane == ofx->lane && fx->row == ofx->row,
          "origin/feature-x (alias) in posizione diversa da feature/x");

    /* etichette remote: origin/main e upstream/main hanno la punta sulla
     * catena dei primi genitori di main (indietro) e non hanno una corsia
     * propria */
    const LayoutNode *om = node_named(&hist, &lay, "origin/main");
    const LayoutNode *um = node_named(&hist, &lay, "upstream/main");
    REQUIRE(om != NULL, "origin/main assente");
    REQUIRE(um != NULL, "upstream/main assente");
    CHECK(om->lane == mn->lane, "origin/main: corsia %zu, attesa quella di "
                                "main (%zu)", om->lane, mn->lane);
    CHECK(um->lane == mn->lane, "upstream/main: corsia %zu, attesa quella di "
                                "main (%zu)", um->lane, mn->lane);
    size_t omi = history_find_branch(&hist, "origin/main");
    size_t umi = history_find_branch(&hist, "upstream/main");
    size_t nmi = history_find_branch(&hist, "main");
    CHECK(lay.nodes[omi].label_of == nmi, "origin/main: etichetta di main "
                                          "attesa");
    CHECK(lay.nodes[umi].label_of == nmi, "upstream/main: etichetta di main "
                                          "attesa");
    CHECK(lay.nodes[history_find_branch(&hist, "origin/branch1")].label_of ==
              (size_t)history_find_branch(&hist, "branch1"),
          "origin/branch1: etichetta di branch1 attesa");

    /* archi tratteggiati dei casi ambigui noti */
    size_t bi = history_find_branch(&hist, "branch1");
    size_t mi = history_find_branch(&hist, "main");
    const BranchNode *b1n = &hist.branches[bi];
    REQUIRE(b1n->suggested_parent != NULL &&
                strcmp(b1n->suggested_parent, "main") == 0,
            "branch1: suggerito atteso main");
    CHECK(edge_exists(&lay, bi, mi, LAYOUT_EDGE_SUGGESTED),
          "branch1: manca l'arco tratteggiato verso main (il suo suggerito)");
    CHECK(!edge_exists(&lay, bi, mi, LAYOUT_EDGE_PARENT),
          "branch1: arco continuo verso main mentre il padre e' solo suggerito");

    size_t b2i = history_find_branch(&hist, "branch2");
    REQUIRE(b2i != (size_t)-1, "branch2 assente");
    REQUIRE(hist.branches[b2i].suggested_parent != NULL &&
                strcmp(hist.branches[b2i].suggested_parent, "branch1") == 0,
            "branch2: suggerito atteso branch1");
    CHECK(edge_exists(&lay, b2i, bi, LAYOUT_EDGE_SUGGESTED),
          "branch2: manca l'arco tratteggiato verso branch1");

    /* commit: proprietario atteso per i casi noti del repository di prova */
    struct {
        const char *soggetto;
        const char *proprietario;
    } owners[] = {
        {"A", "main"},       {"B", "main"},         {"terza revisione", "main"},
        {"B1", "branch1"},   {"B2", "branch1"},     {"B3", "branch1"},
        {"C1", "branch2"},   {"C2", "branch2"},
        {"Merge branch 'branch1' (--no-ff)", "main"},
    };
    for (size_t k = 0; k < sizeof owners / sizeof owners[0]; k++) {
        size_t ci = commit_subject(&data, owners[k].soggetto);
        CHECK(ci != NO_TIP, "commit '%s' assente", owners[k].soggetto);
        if (ci == NO_TIP)
            continue;
        const LayoutCommit *cell = &lay.commits[ci];
        const char *owner =
            cell->owner == NO_TIP ? "-" : hist.branches[cell->owner].name;
        CHECK(cell->owner != NO_TIP &&
                  strcmp(owner, owners[k].proprietario) == 0,
              "commit '%s': proprietario '%s', atteso '%s'",
              owners[k].soggetto, owner, owners[k].proprietario);
    }

    /* T1 sopravvive solo nel merge del branch temp (cancellato): nessun ramo
     * lo reclama, e' una catena anonima di un solo commit nel blocco anonimo,
     * subito dopo il tronco */
    size_t t1 = commit_subject(&data, "T1");
    REQUIRE(t1 != NO_TIP, "commit T1 assente");
    CHECK(lay.commits[t1].owner == NO_TIP,
          "T1 (branch cancellato) ha un proprietario");
    CHECK(lay.anon_lane != NO_TIP && lay.anon_lane_count == 1 &&
              lay.commits[t1].lane == lay.anon_lane,
          "T1 non e' nella corsia anonima (blocco da %zu corsie in %zu)",
          lay.anon_lane_count, lay.anon_lane);
    CHECK(lay.anon_lane == 1,
          "il blocco anonimo (%zu) non e' subito dopo il tronco (corsia 1)",
          lay.anon_lane);
    /* nessun nodo-branch usa una corsia del blocco anonimo */
    for (size_t i = 0; i < lay.node_count; i++)
        CHECK(lay.anon_lane == NO_TIP ||
                  lay.nodes[i].lane < lay.anon_lane ||
                  lay.nodes[i].lane >= lay.anon_lane + lay.anon_lane_count,
              "'%s' (branch) usa la corsia anonima %zu", hist.branches[i].name,
              lay.nodes[i].lane);
    /* l'arco di diramazione della catena anonima va al genitore in un'altra
     * corsia (non e' un arco di catena) */
    {
        size_t p = data.commits[t1].parent_count
                       ? data.commits[t1].parent_idx[0]
                       : NO_TIP;
        CHECK(p != NO_TIP &&
                  cedge_exists(&lay, t1, p, LAYOUT_CEDGE_BRANCH),
              "T1: manca l'arco di diramazione verso il suo primo genitore");
    }

    /* la catena dei primi genitori di main resta sulla corsia di main, dal
     * primo commit al fondo */
    {
        size_t cur = mn->row;
        while (cur != NO_TIP) {
            CHECK(lay.commits[cur].owner == mi,
                  "commit %zu sulla catena di main senza main come proprietario",
                  cur);
            CHECK(lay.commits[cur].lane == mn->lane,
                  "commit %zu sulla catena di main in corsia %zu", cur,
                  lay.commits[cur].lane);
            cur = data.commits[cur].parent_count ? data.commits[cur].parent_idx[0]
                                                 : NO_TIP;
        }
    }

    /* l'arco di merge punta al secondo genitore; l'arco di diramazione parte
     * dal commit proprietario piu' vecchio del ramo e va al suo primo genitore
     * reale, in un'altra corsia (non usa il campo fork del modello) */
    {
        size_t m1 = commit_subject(&data, "Merge branch 'branch1' (--no-ff)");
        size_t second = data.commits[m1].parent_idx[1];
        CHECK(cedge_exists(&lay, m1, second, LAYOUT_CEDGE_MERGE_PARENT),
              "manca l'arco di merge verso il secondo genitore");
        size_t oldest = NO_TIP;
        for (size_t ci = 0; ci < lay.commit_count; ci++)
            if (lay.commits[ci].owner == bi)
                oldest = ci; /* riga crescente = verso il passato */
        CHECK(oldest != NO_TIP, "branch1 non possiede commit");
        if (oldest != NO_TIP) {
            size_t p = data.commits[oldest].parent_count
                           ? data.commits[oldest].parent_idx[0]
                           : NO_TIP;
            CHECK(p != NO_TIP &&
                      lay.commits[oldest].lane != lay.commits[p].lane &&
                      cedge_exists(&lay, oldest, p, LAYOUT_CEDGE_BRANCH),
                  "branch1: manca l'arco di diramazione dal commit piu' vecchio "
                  "(riga %zu) al suo primo genitore", oldest);
        }
    }

    /* tabella scritta a mano degli archi tra commit del repository di prova:
     * un arco per genitore, col tipo dedotto dalle corsie. `occ` distingue le
     * due "RB1" (rb piu' recente, rb-old piu' vecchio). */
    {
        static const struct {
            const char *from;
            unsigned occ;
            const char *to;
            LayoutCommitEdgeKind kind;
        } attesi[] = {
            {"RB1", 0, "terza revisione", LAYOUT_CEDGE_BRANCH},
            {"RB1", 1, "B", LAYOUT_CEDGE_BRANCH},
            {"main: secondo merge di rm", 0, "main: primo merge di rm",
             LAYOUT_CEDGE_FIRST_PARENT},
            {"main: secondo merge di rm", 0, "RM2", LAYOUT_CEDGE_MERGE_PARENT},
            {"RM2", 0, "RM1", LAYOUT_CEDGE_FIRST_PARENT},
            {"main: primo merge di rm", 0, "D", LAYOUT_CEDGE_FIRST_PARENT},
            {"main: primo merge di rm", 0, "RM1", LAYOUT_CEDGE_MERGE_PARENT},
            {"RM1", 0, "terza revisione", LAYOUT_CEDGE_BRANCH},
            {"D", 0, "F1", LAYOUT_CEDGE_FIRST_PARENT},
            {"F1", 0, "Merge branch 'temp' (--no-ff)",
             LAYOUT_CEDGE_FIRST_PARENT},
            {"Merge branch 'temp' (--no-ff)", 0,
             "Merge branch 'branch1' (--no-ff)", LAYOUT_CEDGE_FIRST_PARENT},
            {"Merge branch 'temp' (--no-ff)", 0, "T1",
             LAYOUT_CEDGE_MERGE_PARENT},
            {"T1", 0, "Merge branch 'branch1' (--no-ff)", LAYOUT_CEDGE_BRANCH},
            {"Merge branch 'branch1' (--no-ff)", 0, "terza revisione",
             LAYOUT_CEDGE_FIRST_PARENT},
            {"Merge branch 'branch1' (--no-ff)", 0, "B3",
             LAYOUT_CEDGE_MERGE_PARENT},
            {"SP2", 0, "SP1", LAYOUT_CEDGE_FIRST_PARENT},
            {"SP1", 0, "B3", LAYOUT_CEDGE_BRANCH},
            {"B3", 0, "B2", LAYOUT_CEDGE_FIRST_PARENT},
            {"OB", 0, "R", LAYOUT_CEDGE_BRANCH},
            {"OC", 0, "R", LAYOUT_CEDGE_BRANCH},
            {"OA", 0, "R", LAYOUT_CEDGE_FIRST_PARENT},
            {"AC2", 0, "AC1", LAYOUT_CEDGE_FIRST_PARENT},
            {"AC1", 0, "X", LAYOUT_CEDGE_BRANCH},
            {"X", 0, "A", LAYOUT_CEDGE_BRANCH},
            {"C2", 0, "C1", LAYOUT_CEDGE_FIRST_PARENT},
            {"C1", 0, "B2", LAYOUT_CEDGE_BRANCH},
            {"B2", 0, "B1", LAYOUT_CEDGE_FIRST_PARENT},
            {"B1", 0, "B", LAYOUT_CEDGE_BRANCH},
            {"FX2", 0, "FX1", LAYOUT_CEDGE_FIRST_PARENT},
            {"FX1", 0, "terza revisione", LAYOUT_CEDGE_BRANCH},
            {"terza revisione", 0, "B", LAYOUT_CEDGE_FIRST_PARENT},
            {"B", 0, "A", LAYOUT_CEDGE_FIRST_PARENT},
        };
        size_t n_attesi = sizeof attesi / sizeof attesi[0];
        CHECK(lay.cedge_count == n_attesi,
              "fixture: %zu archi tra commit, attesi %zu", lay.cedge_count,
              n_attesi);
        for (size_t k = 0; k < n_attesi; k++) {
            size_t from =
                commit_subject_n(&data, attesi[k].from, attesi[k].occ);
            size_t to = commit_subject(&data, attesi[k].to);
            CHECK(from != NO_TIP, "fixture: commit '%s' assente",
                  attesi[k].from);
            CHECK(to != NO_TIP, "fixture: commit '%s' assente", attesi[k].to);
            if (from == NO_TIP || to == NO_TIP)
                continue;
            CHECK(cedge_exists(&lay, from, to, attesi[k].kind),
                  "fixture: manca l'arco '%s' -> '%s' (tipo %d)",
                  attesi[k].from, attesi[k].to, (int)attesi[k].kind);
        }
    }

    /* determinismo: due costruzioni identiche (nodi, archi e commit) */
    Layout lay2;
    layout_build(&lay2, &hist);
    CHECK(lay2.node_count == lay.node_count && lay2.edge_count == lay.edge_count &&
              lay2.lanes == lay.lanes && lay2.rows == lay.rows &&
              lay2.commit_count == lay.commit_count &&
              lay2.cedge_count == lay.cedge_count &&
              lay2.anon_lane == lay.anon_lane,
          "la disposizione non e' deterministica (dimensioni)");
    for (size_t i = 0; i < lay.node_count && i < lay2.node_count; i++)
        CHECK(lay.nodes[i].branch == lay2.nodes[i].branch &&
                  lay.nodes[i].lane == lay2.nodes[i].lane &&
                  lay.nodes[i].row == lay2.nodes[i].row &&
                  lay.nodes[i].label_of == lay2.nodes[i].label_of,
              "la disposizione non e' deterministica al nodo %zu", i);
    for (size_t e = 0; e < lay.edge_count && e < lay2.edge_count; e++)
        CHECK(lay.edges[e].child == lay2.edges[e].child &&
                  lay.edges[e].parent == lay2.edges[e].parent &&
                  lay.edges[e].kind == lay2.edges[e].kind,
              "la disposizione non e' deterministica all'arco %zu", e);
    for (size_t ci = 0; ci < lay.commit_count && ci < lay2.commit_count; ci++)
        CHECK(lay.commits[ci].commit == lay2.commits[ci].commit &&
                  lay.commits[ci].lane == lay2.commits[ci].lane &&
                  lay.commits[ci].owner == lay2.commits[ci].owner,
              "la disposizione non e' deterministica al commit %zu", ci);
    for (size_t e = 0; e < lay.cedge_count && e < lay2.cedge_count; e++)
        CHECK(lay.cedges[e].from == lay2.cedges[e].from &&
                  lay.cedges[e].to == lay2.cedges[e].to &&
                  lay.cedges[e].kind == lay2.cedges[e].kind,
              "la disposizione non e' deterministica all'arco commit %zu", e);
    layout_free(&lay2);

    printf("layout: %zu branch, %zu corsie, %zu righe, %zu archi, %zu archi "
           "tra commit\n",
           lay.node_count, lay.lanes, lay.rows, lay.edge_count, lay.cedge_count);

    layout_free(&lay);
    history_free(&hist);
    config_free(&cfg);
    gitdata_free(&data);

    /* repository sintetico con la tabella scritta a mano */
    DWORD wn = GetCurrentDirectoryA(sizeof g_work, g_work);
    REQUIRE(wn > 0 && wn < sizeof g_work, "directory corrente non leggibile");
    char root[MAX_PATH];
    DWORD rn = GetTempPathA(sizeof root, root);
    REQUIRE(rn > 0 && rn < sizeof root, "percorso temporaneo non ottenibile");
    size_t rl = strlen(root);
    while (rl > 0 && (root[rl - 1] == '\\' || root[rl - 1] == '/'))
        root[--rl] = '\0';
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\gg_layout_table", root);
    REQUIRE(make_table_repo(path, err, sizeof err),
            "creazione del repository sintetico fallita");
    check_table(path);
    back_to(g_work);
    remove_tree(path);

    /* catene anonime sovrapposte (x, y) e riuso della corsia (z) */
    snprintf(path, sizeof path, "%s\\gg_layout_xy", root);
    REQUIRE(make_xy_repo(path, err, sizeof err), "creazione del repo x/y fallita");
    check_xy_repo(path);
    back_to(g_work);
    remove_tree(path);

    /* 30 rami creati, mergiati e cancellati: una sola corsia anonima */
    snprintf(path, sizeof path, "%s\\gg_layout_many", root);
    REQUIRE(make_many_deleted_repo(path, 30, err, sizeof err),
            "creazione del repo con 30 rami fallita");
    check_many_deleted(path, 30);
    back_to(g_work);
    remove_tree(path);

    printf("%d test superati, %d falliti\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}

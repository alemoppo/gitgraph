/* test_invariance.c - la genealogia non deve dipendere dai nomi dei branch.
 *
 * Il difetto che questo test copre: le famiglie venivano visitate in ordine
 * alfabetico e "chi arriva prima rivendica il commit", quindi con gli stessi
 * commit e nomi diversi la genealogia cambiava (per esempio feature/login
 * diventava figlio di bugfix/crash, che in ordine alfabetico arriva prima).
 *
 * Il test non muove i riferimenti di Git: rinomina i riferimenti in memoria
 * (stessi commit, stesse punte) e ricostruisce il modello. Se il modello
 * dipendesse dai nomi, le due costruzioni darebbero risultati diversi. Il
 * confronto e' fatto per punta di commit, quindi i nomi non compaiono: stato,
 * padre, candidati e padre suggerito devono coincidere in ogni permutazione.
 *
 * L'unica eccezione dichiarata e' il pareggio sul nome fra candidati con la
 * stessa data (suggestion_basis = SUGGESTION_NAME_TIE): li' il nome e' la
 * regola, quindi il confronto verifica solo la base dello spareggio e non la
 * scelta.
 */
#include "config.h"
#include "gitdata.h"
#include "history.h"
#include "layout.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

static int g_pass;
static int g_fail;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (cond) {                                                           \
            g_pass++;                                                         \
        } else {                                                               \
            g_fail++;                                                         \
            printf("FALLITO %s:%d: ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

/* "nessun commit": come in history.c, l'assenza si indica con SIZE_MAX */
#define NO_TIP ((size_t)-1)

#define REQUIRE(cond, ...)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_fail++;                                                         \
            printf("FALLITO %s:%d: ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            return 2;                                                            \
        }                                                                      \
    } while (0)

/* proprieta' condivisa: serve la macro CHECK definita sopra */
#include "gg_checks.h"

/* ---------- descrizione del modello, per punta ---------- */

static size_t tip_of(const History *h, const GitData *d, const char *name)
{
    size_t node = history_find_branch(h, name);
    if (node == SIZE_MAX)
        return NO_TIP;
    return gitdata_find_commit(d, d->refs[h->branches[node].ref_index].target);
}

/* Hash abbreviato di un commit, "-" se assente. */
static const char *tip_tag(const GitData *d, size_t pos, char *buf, size_t buf_size)
{
    if (pos == NO_TIP || pos >= d->commit_count) {
        snprintf(buf, buf_size, "-");
        return buf;
    }
    snprintf(buf, buf_size, "%.8s", d->commits[pos].hash);
    return buf;
}

static int cmp_line(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Descrizione canonica: una riga per famiglia (per punta), ordinata per punta.
 * I nomi non compaiono, cosi' due permutazioni con nomi diversi si confrontano
 * riga per riga. */
static char *describe(const History *h, const GitData *d)
{
    size_t n = h->branch_count;
    char **lines = (char **)calloc(n ? n : 1, sizeof(char *));
    size_t count = 0;
    char a[32], b[32], c[32];

    for (size_t i = 0; i < n; i++) {
        size_t tip = gitdata_find_commit(d, d->refs[h->branches[i].ref_index].target);
        if (tip == NO_TIP)
            continue;
        /* una riga per famiglia: la famiglia e' l'insieme dei riferimenti con
         * la stessa punta, quindi basta la prima voce incontrata */
        bool gia = false;
        for (size_t k = 0; k < i && !gia; k++) {
            size_t t2 = gitdata_find_commit(d, d->refs[h->branches[k].ref_index].target);
            if (t2 == tip)
                gia = true;
        }
        if (gia)
            continue;

        StrBuf sb;
        sb_init(&sb);
        sb_appendf(&sb, "%s stato=%d padre=", tip_tag(d, tip, a, sizeof a),
                   (int)h->branches[i].state);
        if (h->branches[i].parent)
            sb_appendf(&sb, "%s", tip_tag(d, tip_of(h, d, h->branches[i].parent), b,
                                          sizeof b));
        else
            sb_append(&sb, "-");
        if (h->branches[i].fork_hash)
            sb_appendf(&sb, " fork=%.8s", h->branches[i].fork_hash);
        else
            sb_append(&sb, " fork=-");
        /* candidati, come punte, in ordine */
        char **tags = (char **)calloc(h->branches[i].candidate_count + 1,
                                      sizeof(char *));
        size_t nt = 0;
        for (size_t k = 0; k < h->branches[i].candidate_count; k++) {
            tags[nt++] = xstrdup(tip_tag(d, tip_of(h, d, h->branches[i].candidates[k]), c,
                                          sizeof c));
        }
        qsort(tags, nt, sizeof(char *), cmp_line);
        sb_append(&sb, " candidati=[");
        for (size_t k = 0; k < nt; k++) {
            if (k)
                sb_append(&sb, ",");
            sb_append(&sb, tags[k]);
            free(tags[k]);
        }
        free(tags);
        sb_append(&sb, "]");
        /* padre suggerito: per data e' una scelta confrontabile; a parita' di
         * date e' il nome (regola dichiarata) e quindi non confrontabile */
        if (h->branches[i].suggestion_basis == SUGGESTION_NAME_TIE)
            sb_append(&sb, " suggerito=parita-di-nome");
        else if (h->branches[i].suggested_parent)
            sb_appendf(&sb, " suggerito=%s",
                       tip_tag(d, tip_of(h, d, h->branches[i].suggested_parent), b,
                               sizeof b));
        else
            sb_append(&sb, " suggerito=-");
        lines[count++] = sb.data ? sb.data : xstrdup("");
    }
    qsort(lines, count, sizeof(char *), cmp_line);

    StrBuf out;
    sb_init(&out);
    for (size_t i = 0; i < count; i++) {
        sb_append(&out, lines[i]);
        sb_append(&out, "\n");
        free(lines[i]);
    }
    free(lines);
    return out.data;
}

/* Descrizione canonica della disposizione, per famiglia (per punta): corsia,
 * riga e archi uscenti. Come describe(), i nomi non compaiono: due permutazioni
 * con nomi diversi si confrontano riga per riga. */
static char *describe_layout(const History *h, const GitData *d, const Layout *l)
{
    size_t n = h->branch_count;
    char **lines = (char **)calloc(n ? n : 1, sizeof(char *));
    size_t count = 0;
    char a[32], b[32];

    for (size_t i = 0; i < n; i++) {
        size_t tip =
            gitdata_find_commit(d, d->refs[h->branches[i].ref_index].target);
        if (tip == NO_TIP)
            continue;
        bool gia = false;
        for (size_t k = 0; k < i && !gia; k++) {
            size_t t2 =
                gitdata_find_commit(d, d->refs[h->branches[k].ref_index].target);
            if (t2 == tip)
                gia = true;
        }
        if (gia)
            continue;

        StrBuf sb;
        sb_init(&sb);
        sb_appendf(&sb, "%s corsia=%zu riga=%zu archi=[",
                   tip_tag(d, tip, a, sizeof a), l->nodes[i].lane,
                   l->nodes[i].row);
        char **tags = (char **)calloc(l->edge_count + 1, sizeof(char *));
        size_t nt = 0;
        for (size_t e = 0; e < l->edge_count; e++) {
            if (l->edges[e].child >= n)
                continue;
            size_t ctip = gitdata_find_commit(
                d, d->refs[h->branches[l->edges[e].child].ref_index].target);
            if (ctip != tip)
                continue;
            size_t ptip = gitdata_find_commit(
                d, d->refs[h->branches[l->edges[e].parent].ref_index].target);
            tags[nt] = xmalloc(64);
            snprintf(tags[nt], 64, "%s:%s", tip_tag(d, ptip, b, sizeof b),
                     l->edges[e].kind == LAYOUT_EDGE_SUGGESTED ? "suggerito"
                                                               : "continuo");
            nt++;
        }
        qsort(tags, nt, sizeof(char *), cmp_line);
        bool first = true;
        for (size_t k = 0; k < nt; k++) {
            if (k && strcmp(tags[k], tags[k - 1]) == 0)
                continue; /* archi identici (alias sulla stessa punta) */
            if (!first)
                sb_append(&sb, ",");
            first = false;
            sb_append(&sb, tags[k]);
        }
        for (size_t k = 0; k < nt; k++)
            free(tags[k]);
        free(tags);
        sb_append(&sb, "]");
        lines[count++] = sb.data ? sb.data : xstrdup("");
    }
    qsort(lines, count, sizeof(char *), cmp_line);

    StrBuf out;
    sb_init(&out);
    for (size_t i = 0; i < count; i++) {
        sb_append(&out, lines[i]);
        sb_append(&out, "\n");
        free(lines[i]);
    }
    free(lines);
    return out.data;
}

/* ---------- permutazioni dei nomi ---------- */

/* Copia di GitData con i nomi del gruppo permutati: stessi commit, stesse
 * punte, solo le stringhe dei riferimenti cambiano. */
static void apply_perm(const GitData *base, const char **names, size_t n,
                       const size_t *perm, GitData *out)
{
    *out = *base;
    out->refs = (GitRef *)calloc(base->ref_count ? base->ref_count : 1,
                                 sizeof(GitRef));
    out->ref_count = 0;
    for (size_t i = 0; i < base->ref_count; i++) {
        const GitRef *src = &base->refs[i];
        if (src->kind == REF_TAG) {
            out->refs[out->ref_count++] = *src;
            continue;
        }
        const char *nuovo = src->name;
        for (size_t k = 0; k < n; k++) {
            if (strcmp(src->name, names[k]) == 0) {
                nuovo = names[perm[k]];
                break;
            }
        }
        GitRef *dst = &out->refs[out->ref_count++];
        *dst = *src;
        dst->name = xstrdup(nuovo);
        char full[512];
        snprintf(full, sizeof full, "%s%s",
                 src->kind == REF_REMOTE_BRANCH ? "refs/remotes/" : "refs/heads/",
                 nuovo);
        dst->full_name = xstrdup(full);
    }
}

static void free_perm(GitData *d)
{
    for (size_t i = 0; i < d->ref_count; i++)
        if (d->refs[i].kind != REF_TAG) {
            free(d->refs[i].name);
            free(d->refs[i].full_name);
        }
    free(d->refs);
}

/* Tutte le permutazioni di 0..n-1 (n <= 4: 24 al massimo). */
/* Tutte le permutazioni di 0..n-1 (n <= 4, quindi al massimo 24). */
static size_t permutations(size_t n, size_t perm[][4], size_t max)
{
    size_t p[4];
    for (size_t i = 0; i < n; i++)
        p[i] = i;
    size_t total = 1;
    for (size_t i = 2; i <= n; i++)
        total *= i;
    size_t count = 0;
    while (count < total && count < max) {
        for (size_t i = 0; i < n; i++)
            perm[count][i] = p[i];
        count++;
        if (count == total)
            break;
        /* prossima permutazione lessicografica */
        size_t i = n;
        while (i > 1 && p[i - 2] >= p[i - 1])
            i--;
        if (i <= 1)
            break;
        size_t j = n;
        while (p[j - 1] <= p[i - 2])
            j--;
        size_t tmp = p[i - 2];
        p[i - 2] = p[j - 1];
        p[j - 1] = tmp;
        for (size_t l = i - 1, r = n - 1; l < r; l++, r--) {
            size_t x = p[l];
            p[l] = p[r];
            p[r] = x;
        }
    }
    return count;
}
/* ---------- aciclicita' dei padri suggeriti ---------- */

/* I padri suggeriti formano un grafo con un arco per ogni stato 'ambiguo' con
 * suggerimento: se contenessero un ciclo, il layout non potrebbe seguirli e
 * la "catena dei padri" non avrebbe senso. La selezione del suggerito scende
 * nell'ordine (data, nome), quindi un ciclo non puo' chiudersi: qui si
 * verifica a posteriori su ogni modello costruito. */
static void check_suggestions_acyclic(const History *h, const char *etichetta)
{
    size_t count = h->branch_count;
    for (size_t i = 0; i < count; i++) {
        if (!h->branches[i].suggested_parent)
            continue;
        /* senza cicli la catena dei padri suggeriti termina in un branch
         * senza suggerimento: altrimenti un nodo tornerebbe su se stesso e
         * il cammino non finirebbe */
        size_t cur = i;
        size_t steps = 0;
        while (steps < count &&
               h->branches[cur].suggested_parent != NULL) {
            size_t nxt = history_find_branch(h, h->branches[cur].suggested_parent);
            if (nxt == (size_t)-1) {
                CHECK(false, "%s: suggerito '%s' non risolvito", etichetta,
                      h->branches[cur].suggested_parent);
                break;
            }
            cur = nxt;
            steps++;
        }
        CHECK(steps < count || !h->branches[cur].suggested_parent,
              "%s: ciclo nel grafo dei padri suggeriti partendo da '%s'",
              etichetta, h->branches[i].name);
    }
}

/* ---------- confronto su tutti i gruppi ---------- */

static void check_group(const GitData *base, const char **names, size_t n,
                        const char *etichetta, int *permutazioni_totali)
{
    size_t perm[24][4];
    size_t count = permutations(n, perm, 24);
    char *riferimento = NULL;
    char *rif_layout = NULL;

    for (size_t k = 0; k < count; k++) {
        GitData permuted;
        apply_perm(base, names, n, perm[k], &permuted);

        /* stesso impianto degli altri test: tronco implicito (main/master) */
        char **names = (char **)calloc(permuted.ref_count + 1, sizeof(char *));
        size_t nn = 0;
        for (size_t i = 0; i < permuted.ref_count; i++)
            if (permuted.refs[i].kind == REF_LOCAL_BRANCH)
                names[nn++] = permuted.refs[i].name;
        GraphConfig cfg;
        memset(&cfg, 0, sizeof cfg);
        config_set_implicit_trunk(&cfg, names, nn);

        History hist;
        char err[512];
        bool ok = history_build(&hist, &permuted, &cfg, err, sizeof err);
        if (!ok) {
            CHECK(false, "%s: history_build fallita (%s)", etichetta, err);
            free_perm(&permuted);
            free(names);
            config_free(&cfg);
            continue;
        }
        char *desc = describe(&hist, &permuted);
        /* il grafo dei padri suggeriti non deve chiudere in ciclo, in
         * nessuna permutazione */
        check_suggestions_acyclic(&hist, etichetta);
        /* e ogni ramo con un tronco fra gli antenati deve avere un padre o
         * un padre suggerito che arriva al tronco */
        gg_check_trunk_ancestry(&hist, &permuted, etichetta);
        /* la disposizione (fase 3) non deve dipendere dai nomi */
        Layout lay;
        layout_build(&lay, &hist);
        gg_check_layout(&lay, &hist, etichetta);
        char *dl = describe_layout(&hist, &permuted, &lay);
        layout_free(&lay);
        if (k == 0) {
            riferimento = desc;
            rif_layout = dl;
            printf("%s: %zu permutazioni, modello di riferimento:\n%s", etichetta,
                   count, riferimento);
        } else {
            CHECK(strcmp(desc, riferimento) == 0,
                  "%s: la permutazione %zu cambia il modello\n--- atteso ---\n%s"
                  "--- trovato ---\n%s",
                  etichetta, k, riferimento, desc);
            free(desc);
            CHECK(strcmp(dl, rif_layout) == 0,
                  "%s: la permutazione %zu cambia la disposizione\n"
                  "--- atteso ---\n%s--- trovato ---\n%s",
                  etichetta, k, rif_layout, dl);
            free(dl);
        }
        (*permutazioni_totali)++;
        history_free(&hist);
        free_perm(&permuted);
        free(names);
        config_free(&cfg);
    }
    free(riferimento);
    free(rif_layout);
}

/* ---------- repository di prova costruiti al volato ---------- */

static char g_work[MAX_PATH];

static bool run_git_in(const char *dir, const char *const *args, char *err,
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
        /* diagnostica: quale comando e' fallito e con quale uscita */
        printf("  [git] in %s, uscita %d: git", dir, code);
        for (size_t i = 1; argv[i]; i++)
            printf(" %s", argv[i]);
        printf(" | errore: %s", se.data ? se.data : "(vuoto)");
        if (so.data)
            printf(" | uscita: %s", so.data);
        printf("\n");
    }
    sb_free(&so);
    sb_free(&se);
    return ok && code == 0;
}

/* toglie la directory di lavoro per non lasciare il processo fermo li' */
static void back_to(const char *dir)
{
    SetCurrentDirectoryA(dir);
}

/* Crea un repository piccolo e leggibile: base, poi rami che nascono da un
 * punto scelto. Le date sono esplicite e crescenti, cosi' le date non
 * confondono il confronto fra nomi diversi. */
static void remove_tree(const char *path);

static bool make_repo(const char *path, const char *scenario, bool stesso_orario,
                    char *err, size_t err_size)
{
    /* directory pulita: un tentativo precedente interrotto non deve lasciare
     * rami vecchi, altrimenti git init riuscirebbe ma i comandi fallirebbero */
    remove_tree(path);
    CreateDirectoryA(path, NULL);
    const char *init[] = {"init", "-b", "base", "-q", NULL};
    if (!run_git_in(path, init, err, err_size))
        return false;
    /* orari espliciti e crescenti: le date non devono confondere il confronto
     * fra nomi diversi, e ogni commit ha un orario proprio */
    static int clock = 1700000000;
    char date[32];
    const char *args[16];
    size_t k = 0;
    const char *radice[] = {"-c", "user.name=T", "-c", "user.email=t@t", "commit",
                            "-q", "--allow-empty", "-m", "R", NULL};
    snprintf(date, sizeof date, "@%d +0000", clock++);
    SetEnvironmentVariableA("GIT_AUTHOR_DATE", date);
    SetEnvironmentVariableA("GIT_COMMITTER_DATE", date);
    for (k = 0; radice[k] && k < 15; k++)
        args[k] = radice[k];
    args[k] = NULL;
    if (!run_git_in(path, args, err, err_size))
        return false;

    if (strcmp(scenario, "nidificazione") == 0) {
        /* base: R, S ; poi un ramo che nasce da R (2 commit) e uno che nasce
         * dal primo di quei due: struttura annidata */
        clock += 60;
        snprintf(date, sizeof date, "@%d +0000", clock);
        SetEnvironmentVariableA("GIT_AUTHOR_DATE", date);
        SetEnvironmentVariableA("GIT_COMMITTER_DATE", date);
        const char *s[] = {"-c", "user.name=T", "-c", "user.email=t@t", "commit",
                           "-q", "--allow-empty", "-m", "S", NULL};
        if (!run_git_in(path, s, err, err_size))
            return false;
        const char *co[] = {"checkout", "-q", "-b", "outer", "base~1", NULL};
        if (!run_git_in(path, co, err, err_size))
            return false;
        for (int i = 0; i < 2; i++) {
            clock += 60;
            snprintf(date, sizeof date, "@%d +0000", clock);
            SetEnvironmentVariableA("GIT_AUTHOR_DATE", date);
            SetEnvironmentVariableA("GIT_COMMITTER_DATE", date);
            char msg[8];
            snprintf(msg, sizeof msg, "O%d", i + 1);
            const char *cm[] = {"-c", "user.name=T", "-c", "user.email=t@t",
                                "commit", "-q", "--allow-empty", "-m", msg, NULL};
            if (!run_git_in(path, cm, err, err_size))
                return false;
        }
        clock += 60;
        snprintf(date, sizeof date, "@%d +0000", clock);
        SetEnvironmentVariableA("GIT_AUTHOR_DATE", date);
        SetEnvironmentVariableA("GIT_COMMITTER_DATE", date);
        const char *ci[] = {"checkout", "-q", "-b", "inner", "outer~1", NULL};
        if (!run_git_in(path, ci, err, err_size))
            return false;
        const char *cmi[] = {"-c", "user.name=T", "-c", "user.email=t@t",
                             "commit", "-q", "--allow-empty", "-m", "I1", NULL};
        if (!run_git_in(path, cmi, err, err_size))
            return false;
        const char *back[] = {"checkout", "-q", "base", NULL};
        return run_git_in(path, back, err, err_size);
    }

    /* tre rami dallo stesso punto interno (hub ha un commit sopra il punto) */
    if (stesso_orario) {
        snprintf(date, sizeof date, "@%d +0000", 1700000000);
        SetEnvironmentVariableA("GIT_AUTHOR_DATE", date);
        SetEnvironmentVariableA("GIT_COMMITTER_DATE", date);
    }
    const char *ch[] = {"checkout", "-q", "-b", "hub", "base", NULL};
    if (!run_git_in(path, ch, err, err_size))
        return false;
    for (int i = 0; i < 2; i++) {
        clock += 60;
        snprintf(date, sizeof date, "@%d +0000", clock);
        SetEnvironmentVariableA("GIT_AUTHOR_DATE", date);
        SetEnvironmentVariableA("GIT_COMMITTER_DATE", date);
        char msg[8];
        snprintf(msg, sizeof msg, "H%d", i + 1);
        const char *cm[] = {"-c", "user.name=T", "-c", "user.email=t@t", "commit",
                            "-q", "--allow-empty", "-m", msg, NULL};
        if (!run_git_in(path, cm, err, err_size))
            return false;
    }
    const char *gruppi[2][2] = {{"hub2", "hub"}, {"zeta", "hub"}};
    for (int g = 0; g < 2; g++) {
        /* il punto di diramazione e' il primo commit di hub ("hub~1") */
        char da[64];
        snprintf(da, sizeof da, "hub~1");
        const char *co[8];
        size_t c = 0;
        co[c++] = "checkout";
        co[c++] = "-q";
        co[c++] = "-b";
        co[c++] = gruppi[g][0];
        co[c++] = da;
        co[c] = NULL;
        if (!run_git_in(path, co, err, err_size))
            return false;
        clock += 60;
        snprintf(date, sizeof date, "@%d +0000", clock);
        SetEnvironmentVariableA("GIT_AUTHOR_DATE", date);
        SetEnvironmentVariableA("GIT_COMMITTER_DATE", date);
        char msg[8];
        snprintf(msg, sizeof msg, "%c1", gruppi[g][0][0]);
        const char *cm[] = {"-c", "user.name=T", "-c", "user.email=t@t", "commit",
                            "-q", "--allow-empty", "-m", msg, NULL};
        if (!run_git_in(path, cm, err, err_size))
            return false;
    }
    const char *back[] = {"checkout", "-q", "base", NULL};
    return run_git_in(path, back, err, err_size);
}

/* Rimuove un albero (solo sotto la directory temporanea creata dal test). */
static void remove_tree(const char *path)
{
    char temp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof temp, temp);
    if (n == 0 || n >= sizeof temp)
        return;
    size_t tl = strlen(temp);
    while (tl > 0 && (temp[tl - 1] == '\\' || temp[tl - 1] == '/'))
        temp[--tl] = '\0';
    /* il percorso deve stare sotto la temporanea e avere il nostro prefisso */
    if (strncmp(path, temp, tl) != 0)
        return;
    if (strstr(path, "gg_invarianza") == NULL)
        return;
    /* il percorso da elencare e' la directory con il carattere jolly: con la
     * directory nuda FindFirstFile restituirebbe lei stessa */
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

/* ---------- i casi di riproduzione mk3: due fratelli sullo stesso punto ---- */

/* Un commit vuoto con l'istante voluto. Lo script di riproduzione mk3.sh usa
 * una data per riga (`cm N`): qui l'ordine relativo delle date resta lo
 * stesso, con istanti separati per non introdurre pareggi di data. */
static bool commit_at(const char *path, long long istante, const char *msg,
                      char *err, size_t err_size)
{
    char date[40];
    snprintf(date, sizeof date, "@%lld +0000", istante);
    SetEnvironmentVariableA("GIT_AUTHOR_DATE", date);
    SetEnvironmentVariableA("GIT_COMMITTER_DATE", date);
    const char *cm[] = {"-c", "user.name=T", "-c", "user.email=t@t", "commit",
                        "-q", "--allow-empty", "-m", msg, NULL};
    bool ok = run_git_in(path, cm, err, err_size);
    SetEnvironmentVariableA("GIT_AUTHOR_DATE", NULL);
    SetEnvironmentVariableA("GIT_COMMITTER_DATE", NULL);
    return ok;
}

/* Lo stesso storico dello script di riproduzione mk3.sh: main (A, B), il
 * genitore P (L1, L2) nato da main~1, il figlio C che nasce a L2 e il
 * genitore che prosegue con L3 prima (modalita' A) o dopo (modalita' B) del
 * lavoro di C. In entrambi i casi il punto di diramazione di entrambi i rami
 * e' L2: due fratelli nati dallo stesso punto. */
static bool make_mk3(const char *path, const char *p_name, const char *c_name,
                     char mode, char *err, size_t err_size)
{
    back_to(g_work);
    remove_tree(path);
    CreateDirectoryA(path, NULL);
    const char *init[] = {"init", "-b", "main", "-q", NULL};
    if (!run_git_in(path, init, err, err_size))
        return false;
    long long t = 1771000000; /* base fissa: il repo e' ripetibile */
    if (!commit_at(path, t, "A", err, err_size) ||
        !commit_at(path, t + 1, "B", err, err_size))
        return false;
    const char *co_p[] = {"checkout", "-q", "-b", p_name, "main~1", NULL};
    if (!run_git_in(path, co_p, err, err_size))
        return false;
    if (!commit_at(path, t + 2, "L1", err, err_size) ||
        !commit_at(path, t + 3, "L2", err, err_size))
        return false;
    if (mode == 'A') {
        /* il genitore prosegue (L3), poi il figlio nasce da P~1 (= L2) */
        if (!commit_at(path, t + 4, "L3", err, err_size))
            return false;
        char da[MAX_PATH];
        snprintf(da, sizeof da, "%s~1", p_name);
        const char *co_c[] = {"checkout", "-q", "-b", c_name, da, NULL};
        if (!run_git_in(path, co_c, err, err_size))
            return false;
        if (!commit_at(path, t + 5, "C1", err, err_size))
            return false;
    } else {
        /* il figlio nasce dalla punta del genitore (= L2), poi il genitore
         * prosegue (L3) */
        const char *co_c[] = {"checkout", "-q", "-b", c_name, p_name, NULL};
        if (!run_git_in(path, co_c, err, err_size))
            return false;
        if (!commit_at(path, t + 4, "C1", err, err_size))
            return false;
        const char *back[] = {"checkout", "-q", p_name, NULL};
        if (!run_git_in(path, back, err, err_size))
            return false;
        if (!commit_at(path, t + 5, "L3", err, err_size))
            return false;
    }
    const char *fine[] = {"checkout", "-q", "main", NULL};
    return run_git_in(path, fine, err, err_size);
}

/* Verita' nota dei casi mk3 (regola dei fratelli): i due rami nascono dallo
 * stesso punto e nessuno contiene l'altro, quindi sono sempre ambigui
 * l'uno con l'altro, qualunque sia il nome. Il ramo piu' antico ha anche
 * il tronco main fra i candidati (il primo antenato non fratello sotto il
 * punto di diramazione) e lo suggerisce: nessun fratello e' piu' vecchio di
 * lui. Il ramo nato dopo suggerisce il piu' antico del gruppo, per data. */
static void check_mk3(const GitData *data, const char *p_name,
                      const char *c_name, bool p_piu_vecchio,
                      const char *etichetta)
{
    char **names = (char **)calloc(data->ref_count + 1, sizeof(char *));
    size_t nn = 0;
    for (size_t i = 0; i < data->ref_count; i++)
        if (data->refs[i].kind == REF_LOCAL_BRANCH)
            names[nn++] = data->refs[i].name;
    GraphConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    config_set_implicit_trunk(&cfg, names, nn);
    History hist;
    char err[512];
    if (!history_build(&hist, data, &cfg, err, sizeof err)) {
        CHECK(false, "%s: history_build fallita (%s)", etichetta, err);
        free(names);
        config_free(&cfg);
        return;
    }
    /* il ramo nato dopo dichiara il suggerimento verso il piu' antico */
    const char *nome_nato_prima = p_piu_vecchio ? p_name : c_name;
    const char *nome_nato_dopo = p_piu_vecchio ? c_name : p_name;
    for (size_t i = 0; i < hist.branch_count; i++) {
        const BranchNode *b = &hist.branches[i];
        bool e_prima = strcmp(b->name, nome_nato_prima) == 0;
        bool e_dopo = strcmp(b->name, nome_nato_dopo) == 0;
        if (!e_prima && !e_dopo)
            continue; /* solo i due fratelli; main e' coperto dall'aciclicita' */
        const char *altro = e_prima ? nome_nato_dopo : nome_nato_prima;
        CHECK(b->state == PARENT_AMBIGUOUS && b->parent == NULL,
              "%s: '%s' atteso ambiguo senza padre, trovato %s (stato %d)",
              etichetta, b->name, b->parent ? b->parent : "-", (int)b->state);
        bool c_altro = false, c_main = false;
        for (size_t k = 0; k < b->candidate_count && b->candidates; k++) {
            if (strcmp(b->candidates[k], altro) == 0)
                c_altro = true;
            if (strcmp(b->candidates[k], "main") == 0)
                c_main = true;
        }
        if (e_prima) {
            /* il piu' antico del gruppo: il fratello e il tronco da cui il
             * gruppo si e' staccato (primo antenato non fratello sotto il
             * punto), entrambi in lizza, pero' resta ambiguo */
            CHECK(b->candidate_count == 2 && c_altro && c_main,
                  "%s: '%s' attesi i candidati '%s' e 'main', trovati %zu",
                  etichetta, b->name, altro, b->candidate_count);
        } else {
            CHECK(b->candidate_count == 1 && c_altro,
                  "%s: '%s' atteso con unico candidato '%s', trovati %zu",
                  etichetta, b->name, altro, b->candidate_count);
        }
        if (e_dopo) {
            CHECK(b->suggested_parent &&
                      strcmp(b->suggested_parent, nome_nato_prima) == 0,
                  "%s: '%s' atteso col suggerito '%s', trovato %s", etichetta,
                  b->name, nome_nato_prima,
                  b->suggested_parent ? b->suggested_parent : "-");
            CHECK(b->suggestion_basis == SUGGESTION_DATE,
                  "%s: '%s' base del suggerimento %d, attesa per data",
                  etichetta, b->name, (int)b->suggestion_basis);
            CHECK(b->motivation &&
                      strstr(b->motivation, "padre suggerito per data") &&
                      strstr(b->motivation, "poco affidabile con rebase"),
                  "%s: '%s' motivazione senza la dichiarazione del "
                  "suggerimento per data: %s",
                  etichetta, b->name, b->motivation ? b->motivation : "-");
        } else {
            /* il piu' antico: nessun fratello vecchio, quindi suggerisce il
             * primo antenato non fratello sotto il punto (qui il tronco main) */
            CHECK(b->suggested_parent &&
                      strcmp(b->suggested_parent, "main") == 0,
                  "%s: '%s' atteso col suggerito 'main', trovato %s", etichetta,
                  b->name,
                  b->suggested_parent ? b->suggested_parent : "-");
            CHECK(b->suggestion_basis == SUGGESTION_ANCESTOR,
                  "%s: '%s' base del suggerimento %d, attesa da antenato",
                  etichetta, b->name, (int)b->suggestion_basis);
            CHECK(b->motivation &&
                      strstr(b->motivation,
                             "nessun fratello piu' vecchio: padre suggerito "
                             "'main'"),
                  "%s: '%s' motivazione senza la dichiarazione del suggerito "
                  "antenato: %s",
                  etichetta, b->name, b->motivation ? b->motivation : "-");
        }
    }
    check_suggestions_acyclic(&hist, etichetta);
    gg_check_trunk_ancestry(&hist, data, etichetta);
    {
        Layout lay;
        layout_build(&lay, &hist);
        gg_check_layout(&lay, &hist, etichetta);
        layout_free(&lay);
    }
    printf("%s: verificati '%s' e '%s' (fratelli, ambigui)\n", etichetta,
           nome_nato_prima, nome_nato_dopo);
    history_free(&hist);
    free(names);
    config_free(&cfg);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 3) {
        fprintf(stderr, "uso: test_invariance <directory-di-lavoro> <percorso-repo>\n");
        return 2;
    }
    /* percorso di lavoro assoluto: il test cambia directory mentre esegue git,
     * e un percorso relativo non basterebbe a tornare indietro */
    char work[MAX_PATH];
    DWORD wn = GetFullPathNameA(argv[1], (DWORD)sizeof work, work, NULL);
    REQUIRE(wn > 0 && wn < sizeof work, "percorso di lavoro non assolutizzato");
    snprintf(g_work, sizeof g_work, "%s", work);
    const char *repo = argv[2];

    GitData base;
    char err[1024];
    REQUIRE(gitdata_load(&base, repo, err, sizeof err),
            "caricamento del repo di prova fallito: %s", err);

    int permutazioni = 0;
    /* gruppi con nidificazione o fratelli nel repository di prova */
    const char *g1[] = {"branch1", "branch2"};
    const char *g2[] = {"branch1", "sp"};
    const char *g3[] = {"oA", "oB", "oC"};
    const char *g4[] = {"amb1", "amb2", "ambbase"};
    const char *g5[] = {"feature/x", "rb", "rm"};
    check_group(&base, g1, 2, "fixture: branch1/branch2 (nidificazione)", &permutazioni);
    check_group(&base, g2, 2, "fixture: branch1/sp (nidificazione)", &permutazioni);
    check_group(&base, g3, 3, "fixture: oA/oB/oC (fratelli)", &permutazioni);
    check_group(&base, g4, 3, "fixture: amb1/amb2/ambbase (alias)", &permutazioni);
    check_group(&base, g5, 3, "fixture: feature-x/rb/rm (fratelli)", &permutazioni);
    gitdata_free(&base);

    /* i due casi di riproduzione, costruiti qui con date esplicite */
    back_to(g_work);
    char root[MAX_PATH];
    DWORD rn = GetTempPathA(sizeof root, root);
    REQUIRE(rn > 0 && rn < sizeof root, "percorso temporaneo non ottenibile");
    size_t rl = strlen(root);
    while (rl > 0 && (root[rl - 1] == '\\' || root[rl - 1] == '/'))
        root[--rl] = '\0';
    char path1[MAX_PATH], path2[MAX_PATH];
    snprintf(path1, sizeof path1, "%s\\gg_invarianza_nid", root);
    snprintf(path2, sizeof path2, "%s\\gg_invarianza_frat", root);
    REQUIRE(make_repo(path1, "nidificazione", false, err, sizeof err),
            "creazione del repository di nidificazione fallita: %s", err);
    REQUIRE(gitdata_load(&base, path1, err, sizeof err),
            "caricamento del repository di nidificazione fallito: %s", err);
    const char *r1[] = {"outer", "inner"};
    check_group(&base, r1, 2, "riproduzione: outer/inner (nidificazione)",
                &permutazioni);
    gitdata_free(&base);

    REQUIRE(make_repo(path2, "fratelli", false, err, sizeof err),
            "creazione del repository di fratelli fallita: %s", err);
    REQUIRE(gitdata_load(&base, path2, err, sizeof err),
            "caricamento del repository di fratelli fallito: %s", err);
    const char *r2[] = {"hub", "hub2", "zeta"};
    check_group(&base, r2, 3, "riproduzione: hub/hub2/zeta (fratelli)", &permutazioni);
    gitdata_free(&base);

    /* stesso caso con tutti i commit alla stessa ora: e' come nascono i
     * repository generati in blocco (fast-import), dove le date non distinguono
     * nulla e il modello deve restare uguale comunque */
    char path3[MAX_PATH];
    snprintf(path3, sizeof path3, "%s\\gg_invarianza_stessi", root);
    REQUIRE(make_repo(path3, "fratelli", true, err, sizeof err),
            "creazione del repository a orari uguali fallita: %s", err);
    REQUIRE(gitdata_load(&base, path3, err, sizeof err),
            "caricamento del repository a orari uguali fallito: %s", err);
    const char *r3[] = {"hub", "hub2", "zeta"};
    check_group(&base, r3, 3, "riproduzione: hub/hub2/zeta (orari uguali)",
                &permutazioni);
    gitdata_free(&base);
    SetEnvironmentVariableA("GIT_AUTHOR_DATE", NULL);
    SetEnvironmentVariableA("GIT_COMMITTER_DATE", NULL);
    back_to(g_work); /* prima di cancellare: la directory corrente non puo' rimanere li' */
    remove_tree(path1);
    remove_tree(path2);
    remove_tree(path3);

    /* i quattro casi di riproduzione mk3: modalita' A e B con i nomi in
     * entrambi gli ordini. La verita' attesa (fratelli sempre ambigui, un
     * solo suggerimento per data) non dipende dai nomi. */
    const struct {
        const char *p, *c;
        char mode;
        bool p_vecchio;
        const char *et;
    } casi_mk3[] = {
        {"feature/login", "bugfix/crash", 'A', true,
         "mk3: modalita' A, genitore feature/login"},
        {"bugfix/crash", "feature/login", 'A', true,
         "mk3: modalita' A, nomi invertiti"},
        {"feature/login", "bugfix/crash", 'B', false,
         "mk3: modalita' B, genitore feature/login"},
        {"bugfix/crash", "feature/login", 'B', false,
         "mk3: modalita' B, nomi invertiti"},
    };
    for (size_t m = 0; m < sizeof casi_mk3 / sizeof casi_mk3[0]; m++) {
        char path_mk[MAX_PATH];
        snprintf(path_mk, sizeof path_mk, "%s\\gg_invarianza_mk3%zu", root, m);
        REQUIRE(make_mk3(path_mk, casi_mk3[m].p, casi_mk3[m].c,
                         casi_mk3[m].mode, err, sizeof err),
                "creazione del repository %s fallita: %s", casi_mk3[m].et, err);
        REQUIRE(gitdata_load(&base, path_mk, err, sizeof err),
                "caricamento del repository %s fallito: %s", casi_mk3[m].et,
                err);
        check_mk3(&base, casi_mk3[m].p, casi_mk3[m].c, casi_mk3[m].p_vecchio,
                  casi_mk3[m].et);
        gitdata_free(&base);
        back_to(g_work);
        remove_tree(path_mk);
    }

    printf("%d controlli superati, %d falliti (%d permutazioni confrontate)\n",
           g_pass, g_fail, permutazioni);
    return g_fail > 0 ? 1 : 0;
}
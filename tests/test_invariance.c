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
/* ---------- confronto su tutti i gruppi ---------- */

static void check_group(const GitData *base, const char **names, size_t n,
                        const char *etichetta, int *permutazioni_totali)
{
    size_t perm[24][4];
    size_t count = permutations(n, perm, 24);
    char *riferimento = NULL;

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
        if (k == 0) {
            riferimento = desc;
            printf("%s: %zu permutazioni, modello di riferimento:\n%s", etichetta,
                   count, riferimento);
        } else {
            CHECK(strcmp(desc, riferimento) == 0,
                  "%s: la permutazione %zu cambia il modello\n--- atteso ---\n%s"
                  "--- trovato ---\n%s",
                  etichetta, k, riferimento, desc);
            free(desc);
        }
        (*permutazioni_totali)++;
        history_free(&hist);
        free_perm(&permuted);
        free(names);
        config_free(&cfg);
    }
    free(riferimento);
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

    printf("%d controlli superati, %d falliti (%d permutazioni confrontate)\n",
           g_pass, g_fail, permutazioni);
    return g_fail > 0 ? 1 : 0;
}
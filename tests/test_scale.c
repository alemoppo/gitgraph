/* test_scale.c - verifica degli obiettivi di scala e del determinismo.
 *
 * Uso: test_scale <percorso-repo> <budget_secondi> [indice_del_sotto_branch]
 *
 * Misura il caricamento (git come processo esterno) e la costruzione del
 * modello dei branch, e fallisce se la somma supera il budget: obiettivo di
 * progetto 300 riferimenti / 10.000 commit entro 3 s e 1.000 riferimenti /
 * 50.000 commit entro 30 s (Release, senza interfaccia grafica).
 *
 * Verifica inoltre che il modello sia deterministico (due costruzioni
 * successive identiche) e che i casi noti del generatore siano risolti:
 * main long-lived senza padre ne' candidati, feature su main, sotto-branch
 * sul proprio feature.
 *
 * Esce con 0 se tutto passa, 1 se un controllo fallisce, 2 su errore di
 * preparazione (repo assente, git non disponibile, argomenti errati).
 */
#include "config.h"
#include "gitdata.h"
#include "history.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail = 0;

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FALLITO ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    g_fail++;
}

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        if (!(cond))                                                      \
            fail(__VA_ARGS__);                                            \
    } while (0)

static double seconds_since(clock_t start)
{
    return (double)(clock() - start) / (double)CLOCKS_PER_SEC;
}

static const BranchNode *branch_named(const History *h, const char *name)
{
    size_t i = history_find_branch(h, name);
    return i == (size_t)-1 ? NULL : &h->branches[i];
}

/* Due modelli equivalenti: stesso esito => deterministico. */
static bool histories_equal(const History *a, const History *b)
{
    if (a->branch_count != b->branch_count)
        return false;
    for (size_t i = 0; i < a->branch_count; i++) {
        const BranchNode *x = &a->branches[i];
        const BranchNode *y = &b->branches[i];
        if (strcmp(x->name, y->name) != 0 || x->state != y->state)
            return false;
        if ((x->parent == NULL) != (y->parent == NULL))
            return false;
        if (x->parent && strcmp(x->parent, y->parent) != 0)
            return false;
        if (strcmp(x->motivation, y->motivation) != 0)
            return false;
        if ((x->fork_hash == NULL) != (y->fork_hash == NULL))
            return false;
        if (x->fork_hash && strcmp(x->fork_hash, y->fork_hash) != 0)
            return false;
        if ((x->first_own_commit == NULL) != (y->first_own_commit == NULL))
            return false;
        if (x->first_own_commit && strcmp(x->first_own_commit, y->first_own_commit) != 0)
            return false;
        if (x->candidate_count != y->candidate_count || x->alias_count != y->alias_count)
            return false;
    }
    return true;
}

static size_t local_branch_count(const GitData *d)
{
    size_t n = 0;
    for (size_t i = 0; i < d->ref_count; i++)
        if (d->refs[i].kind != REF_TAG)
            n++;
    return n;
}

static void check_deep_chain(const History *h, unsigned long depth)
{
    /* Il generatore con profondi' 1 crea una catena: ogni feature nasce dal
     * feature precedente. Serve a verificare che il controllo delle relazioni
     * circolari regga su gerarchie profonde. */
    for (unsigned long i = 1; i <= depth; i++) {
        char cur[64], prev[64];
        snprintf(cur, sizeof cur, "feature/%lu", i);
        snprintf(prev, sizeof prev, "feature/%lu", i - 1);
        const BranchNode *n = branch_named(h, cur);
        if (!n) {
            fail("catena profonda: '%s' assente", cur);
            continue;
        }
        if (!n->parent || strcmp(n->parent, prev) != 0) {
            fail("catena profonda: '%s' ha padre '%s', atteso '%s'", cur,
                 n->parent ? n->parent : "-", prev);
            continue;
        }
        if (n->state != PARENT_DEDUCED)
            fail("catena profonda: '%s' stato %d, atteso dedotto", cur, (int)n->state);
        if (!n->fork_hash)
            fail("catena profonda: '%s' senza punto di diramazione", cur);
    }
}

static void check_model(const History *h, const char *sub_index, unsigned long deep)
{
    for (size_t i = 0; i < h->branch_count; i++) {
        const BranchNode *b = &h->branches[i];
        CHECK(b->motivation && b->motivation[0] != '\0',
              "motivazione mancante per '%s'", b->name);
        CHECK(b->state != PARENT_CONFIGURED,
              "'%s': nessun override configurato, stato non atteso", b->name);
        if (b->state == PARENT_AMBIGUOUS)
            CHECK(b->parent == NULL && b->candidate_count > 0,
                  "'%s': ambiguo senza candidati", b->name);
        if (b->state == PARENT_DEDUCED)
            CHECK(b->parent != NULL, "'%s': dedotto senza padre", b->name);
        /* ogni riferimento conserva la sua punta reale */
        CHECK(b->tip_pos != (size_t)-1, "'%s': punta non risolta", b->name);
    }

    const BranchNode *main_b = branch_named(h, "main");
    CHECK(main_b != NULL, "branch 'main' assente");
    if (main_b) {
        CHECK(main_b->state == PARENT_NONE && main_b->parent == NULL,
              "main: atteso nessun padre");
        CHECK(main_b->fork_hash == NULL, "main: atteso nessun fork");
        CHECK(main_b->candidate_count == 0, "main: attesi nessun candidato");
        CHECK(main_b->is_long_lived, "main: atteso long-lived");
    }

    /* primo feature (non mergiato) e secondo (mergiato): entrambi su main
     * (nel caso della catena profonda la parentela e' verificata a parte) */
    if (deep == 0) {
        const char *features[] = {"feature/0", "feature/1"};
        for (int i = 0; i < 2; i++) {
            const BranchNode *f = branch_named(h, features[i]);
            CHECK(f != NULL, "branch '%s' assente", features[i]);
            if (f)
                CHECK(f->state == PARENT_DEDUCED && f->parent &&
                          strcmp(f->parent, "main") == 0,
                      "'%s': padre atteso 'main', trovato %s", features[i],
                      f->parent ? f->parent : "-");
        }

        if (sub_index) {
            char sub[64];
            snprintf(sub, sizeof sub, "sub/%s", sub_index);
            const BranchNode *s = branch_named(h, sub);
            CHECK(s != NULL, "branch '%s' assente", sub);
            if (s) {
                char parent[64];
                snprintf(parent, sizeof parent, "feature/%s", sub_index);
                CHECK(s->state == PARENT_DEDUCED && s->parent &&
                          strcmp(s->parent, parent) == 0,
                      "'%s': padre atteso '%s', trovato %s", sub, parent,
                      s->parent ? s->parent : "-");
            }
        }
    }

    if (deep > 0)
        check_deep_chain(h, deep);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "Uso: test_scale <percorso-repo> <budget_secondi> [indice_sub] [profondita]\n");
        return 2;
    }
    double budget = atof(argv[2]);
    const char *sub_index = (argc > 3) ? argv[3] : NULL;
    unsigned long deep = (argc > 4) ? strtoul(argv[4], NULL, 10) : 0;

    char err[1024];
    clock_t t_all = clock();

    GitData data;
    clock_t t0 = clock();
    if (!gitdata_load(&data, argv[1], err, sizeof err)) {
        fprintf(stderr, "PREREQUISITO FALLITO: %s\n", err);
        return 2;
    }
    double t_load = seconds_since(t0);

    /* tronco implicito: main */
    char **names = xmalloc((data.ref_count ? data.ref_count : 1) * sizeof(char *));
    size_t nn = 0;
    for (size_t i = 0; i < data.ref_count; i++)
        if (data.refs[i].kind == REF_LOCAL_BRANCH)
            names[nn++] = data.refs[i].name;
    GraphConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    config_set_implicit_trunk(&cfg, names, nn);
    free(names);

    History h1;
    t0 = clock();
    if (!history_build(&h1, &data, &cfg, err, sizeof err)) {
        fprintf(stderr, "PREREQUISITO FALLITO: history_build: %s\n", err);
        gitdata_free(&data);
        return 2;
    }
    double t_build = seconds_since(t0);

    /* seconda costruzione: determinismo */
    History h2;
    t0 = clock();
    if (!history_build(&h2, &data, &cfg, err, sizeof err)) {
        fprintf(stderr, "PREREQUISITO FALLITO: seconda history_build: %s\n", err);
        history_free(&h1);
        gitdata_free(&data);
        return 2;
    }
    double t_build2 = seconds_since(t0);

    printf("scala: %zu commit, %zu riferimenti (%zu branch)\n", data.commit_count,
           data.ref_count, local_branch_count(&data));
    printf("tempi: caricamento %.2f s, costruzione %.2f s, ricostruzione %.2f s, "
           "totale (prima costruzione) %.2f s, budget %.1f s\n",
           t_load, t_build, t_build2, t_load + t_build, budget);

    CHECK(h1.branch_count == local_branch_count(&data),
          "modello con %zu branch, repository con %zu riferimenti non-tag",
          h1.branch_count, local_branch_count(&data));
    CHECK(histories_equal(&h1, &h2), "il modello non e' deterministico");
    check_model(&h1, sub_index, deep);
    CHECK(t_load + t_build <= budget,
          "budget superato: %.2f s > %.1f s", t_load + t_build, budget);

    history_free(&h1);
    history_free(&h2);
    config_free(&cfg);
    gitdata_free(&data);

    (void)t_all;
    printf("totale processo (inclusa ricostruzione): %.2f s\n",
           seconds_since(t_all));
    if (g_fail > 0) {
        printf("%d controlli falliti\n", g_fail);
        return 1;
    }
    printf("tutti i controlli superati\n");
    return 0;
}

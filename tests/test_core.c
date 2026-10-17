/* test_core.c - test automatici della fase 1 (senza finestra).
 *
 * Uso: test_core <percorso-repo-di-test>
 *
 * Copre: parsing di git log/for-each-ref, ordine dei genitori, ordine
 * topologico, esclusione dello stash, tag annotati dereferenziati, regole del
 * modello dei branch (padre, stato, fork, primo commit esclusivo, casi
 * ambigui), precedenza degli override, long-lived, determinismo.
 *
 * Esce con 0 se tutti i test passano, 1 se qualcuno fallisce, 2 su errore
 * di preparazione (repo assente o malformato).
 */
#include "config.h"
#include "gitdata.h"
#include "history.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* Prerequisito: se manca, il repo di test non e' quello atteso. */
#define REQUIRE(cond, msg)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "PREREQUISITO FALLITO: %s\n", msg);         \
            return 2;                                                   \
        }                                                               \
    } while (0)

/* ---------- helper di ricerca ---------- */

static size_t commit_with_prefix(const GitData *d, const char *prefix)
{
    size_t plen = strlen(prefix);
    for (size_t i = 0; i < d->commit_count; i++) {
        if (strncmp(d->commits[i].message, prefix, plen) == 0)
            return i;
    }
    return (size_t)-1;
}

static const char *hash_of(const GitData *d, const char *prefix)
{
    size_t i = commit_with_prefix(d, prefix);
    return i == (size_t)-1 ? NULL : d->commits[i].hash;
}

static const GitRef *ref_named(const GitData *d, const char *name)
{
    for (size_t i = 0; i < d->ref_count; i++) {
        if (strcmp(d->refs[i].name, name) == 0)
            return &d->refs[i];
    }
    return NULL;
}

static const BranchNode *branch_named(const History *h, const char *name)
{
    size_t i = history_find_branch(h, name);
    return i == (size_t)-1 ? NULL : &h->branches[i];
}

static bool candidates_contain(const BranchNode *n, const char *name)
{
    for (size_t i = 0; i < n->candidate_count; i++) {
        if (strcmp(n->candidates[i], name) == 0)
            return true;
    }
    return false;
}

static bool hash_is_hex40(const char *h)
{
    if (strlen(h) != 40)
        return false;
    for (const char *p = h; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
            return false;
    }
    return true;
}

/* Confronta due modelli: stesso esito => determinismo. */
static void compare_histories(const History *a, const History *b)
{
    CHECK(a->branch_count == b->branch_count,
          "determinismo: branch_count %zu != %zu", a->branch_count,
          b->branch_count);
    for (size_t i = 0; i < a->branch_count && i < b->branch_count; i++) {
        const BranchNode *x = &a->branches[i];
        const BranchNode *y = &b->branches[i];
        CHECK(strcmp(x->name, y->name) == 0, "determinismo: nome %zu", i);
        CHECK(x->state == y->state, "determinismo: stato di '%s'", x->name);
        CHECK((x->parent == NULL) == (y->parent == NULL),
              "determinismo: padre di '%s'", x->name);
        if (x->parent && y->parent)
            CHECK(strcmp(x->parent, y->parent) == 0,
                  "determinismo: padre di '%s'", x->name);
        CHECK(strcmp(x->motivation, y->motivation) == 0,
              "determinismo: motivazione di '%s'", x->name);
        CHECK((x->fork_hash == NULL) == (y->fork_hash == NULL),
              "determinismo: fork di '%s'", x->name);
        if (x->fork_hash && y->fork_hash)
            CHECK(strcmp(x->fork_hash, y->fork_hash) == 0,
                  "determinismo: fork di '%s'", x->name);
        CHECK(x->candidate_count == y->candidate_count,
              "determinismo: candidati di '%s'", x->name);
    }
}

/* ---------- test ---------- */

static int test_gitdata(const GitData *d)
{
    /* il repo di prova: 18 commit, 15 riferimenti (11 branch incl.
     * origin/main, 4 tag) */
    CHECK(d->commit_count == 18, "commit_count = %zu, atteso 18",
          d->commit_count);
    CHECK(d->ref_count == 15, "ref_count = %zu, atteso 15", d->ref_count);
    REQUIRE(d->head_branch != NULL, "HEAD scollegata nel repo di test");
    CHECK(strcmp(d->head_branch, "main") == 0, "head_branch = %s",
          d->head_branch);

    /* nessun duplicato, hash ben formati */
    for (size_t i = 0; i < d->commit_count; i++) {
        CHECK(hash_is_hex40(d->commits[i].hash), "hash non valido: %s",
              d->commits[i].hash);
        for (size_t j = i + 1; j < d->commit_count; j++) {
            CHECK(strcmp(d->commits[i].hash, d->commits[j].hash) != 0,
                  "commit duplicato: %s", d->commits[i].hash);
        }
    }

    /* ordine topologico: ogni genitore compare dopo il figlio */
    for (size_t i = 0; i < d->commit_count; i++) {
        const GitCommit *c = &d->commits[i];
        for (size_t k = 0; k < c->parent_count; k++) {
            size_t p = gitdata_find_commit(d, c->parents[k]);
            CHECK(p != (size_t)-1, "genitore mancante di %s", c->hash);
            CHECK(p > i, "ordine topologico violato: %s (pos %zu) prima di %s",
                  c->parents[k], p, c->hash);
        }
    }

    /* ordine dei genitori di un merge: prima il lato main, poi il mergiato */
    size_t m1 = commit_with_prefix(d, "Merge branch 'branch1'");
    REQUIRE(m1 != (size_t)-1, "commit di merge branch1 non trovato");
    const GitCommit *mer = &d->commits[m1];
    CHECK(mer->parent_count == 2, "merge branch1: %zu genitori",
          mer->parent_count);
    if (mer->parent_count == 2) {
        const char *c_hash = hash_of(d, "terza revisione");
        const char *b3_hash = hash_of(d, "B3:");
        CHECK(c_hash && strcmp(mer->parents[0], c_hash) == 0,
              "merge branch1: primo genitore non e' C");
        CHECK(b3_hash && strcmp(mer->parents[1], b3_hash) == 0,
              "merge branch1: secondo genitore non e' B3");
    }

    /* fast-forward: D ha un solo genitore (F1) */
    size_t di = commit_with_prefix(d, "D: tip di main");
    REQUIRE(di != (size_t)-1, "commit D non trovato");
    const GitCommit *dc = &d->commits[di];
    CHECK(dc->parent_count == 1, "D: %zu genitori", dc->parent_count);
    if (dc->parent_count == 1) {
        const char *f1 = hash_of(d, "F1:");
        CHECK(f1 && strcmp(dc->parents[0], f1) == 0, "D: genitore != F1");
    }
    CHECK(dc->date != 0, "data di D assente");
    {
        time_t t = (time_t)dc->date;
        struct tm tmv;
#ifdef _WIN32
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        CHECK(tmv.tm_year + 1900 == 2026 && tmv.tm_mon == 0 &&
                  tmv.tm_mday == 2 && tmv.tm_hour == 11 && tmv.tm_min == 20,
              "data di D inattesa");
    }
    CHECK(strcmp(dc->author, "Test User") == 0, "autore di D = %s", dc->author);

    /* messaggio UTF-8 multi-riga intatto */
    size_t ci = commit_with_prefix(d, "terza revisione");
    REQUIRE(ci != (size_t)-1, "commit C non trovato");
    const char *msg = d->commits[ci].message;
    CHECK(strstr(msg, "\xC3\xA0" "ccenti") != NULL,
          "accents perduti nel messaggio");
    CHECK(strchr(msg, '\n') != NULL, "messaggio multi-riga appiattito");

    /* stash escluso: nessun commit "WIP", nessun ref "stash" */
    for (size_t i = 0; i < d->commit_count; i++)
        CHECK(strncmp(d->commits[i].message, "WIP", 3) != 0,
              "commit dello stash incluso: %s", d->commits[i].hash);
    for (size_t i = 0; i < d->ref_count; i++)
        CHECK(strcmp(d->refs[i].name, "stash") != 0 &&
                  strcmp(d->refs[i].full_name, "refs/stash") != 0,
              "ref dello stash incluso: %s", d->refs[i].full_name);

    /* tag annotati dereferenziati al commit */
    const GitRef *v10 = ref_named(d, "v1.0");
    const GitRef *v10lw = ref_named(d, "v1.0-lw");
    const GitRef *v20 = ref_named(d, "v2.0");
    const GitRef *tip = ref_named(d, "tip-lw");
    REQUIRE(v10 && v10lw && v20 && tip, "tag attesi assenti");
    CHECK(v10->kind == REF_TAG && v20->kind == REF_TAG, "tipo tag");
    const char *ch = hash_of(d, "terza revisione");
    const char *dh = hash_of(d, "D: tip di main");
    CHECK(ch && strcmp(v10->target, ch) == 0, "v1.0 non punta a C");
    CHECK(ch && strcmp(v10lw->target, ch) == 0, "v1.0-lw non punta a C");
    CHECK(dh && strcmp(v20->target, dh) == 0, "v2.0 non punta a D");
    CHECK(dh && strcmp(tip->target, dh) == 0, "tip-lw non punta a D");
    CHECK(strcmp(v10->full_name, "refs/tags/v1.0") == 0, "full_name di v1.0");

    /* remote-tracking speculare a main */
    const GitRef *om = ref_named(d, "origin/main");
    REQUIRE(om != NULL, "origin/main assente");
    CHECK(om->kind == REF_REMOTE_BRANCH, "origin/main: tipo errato");
    CHECK(dh && strcmp(om->target, dh) == 0, "origin/main non punta a D");
    return 0;
}

static int test_history_default(const GitData *data)
{
    /* configurazione implicita: tronco main */
    char **names = xmalloc((data->ref_count ? data->ref_count : 1) *
                           sizeof(char *));
    size_t n = 0;
    for (size_t i = 0; i < data->ref_count; i++) {
        if (data->refs[i].kind == REF_LOCAL_BRANCH)
            names[n++] = data->refs[i].name;
    }
    GraphConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    config_set_implicit_trunk(&cfg, names, n);
    free(names);
    CHECK(cfg.long_lived_count == 1 && strcmp(cfg.long_lived[0], "main") == 0,
          "tronco implicito: atteso main");

    History hist;
    char err[1024];
    REQUIRE(history_build(&hist, data, &cfg, err, sizeof err),
            "history_build fallita");

    /* nodi: 9 branch locali + origin/main */
    CHECK(hist.branch_count == 10, "branch_count = %zu, atteso 10",
          hist.branch_count);

    /* ogni branch ha una motivazione non vuota */
    for (size_t i = 0; i < hist.branch_count; i++) {
        CHECK(hist.branches[i].motivation != NULL &&
                  hist.branches[i].motivation[0] != '\0',
              "motivazione mancante per '%s'", hist.branches[i].name);
    }

    const BranchNode *main_b = branch_named(&hist, "main");
    REQUIRE(main_b != NULL, "branch main assente nel modello");
    CHECK(main_b->state == PARENT_NONE, "main: stato non 'nessuno'");
    CHECK(main_b->parent == NULL, "main: padre inventato");
    CHECK(main_b->is_head, "main: non marcato HEAD");
    CHECK(main_b->is_long_lived, "main: non long-lived");

    /* specchio remote-tracking */
    const BranchNode *om = branch_named(&hist, "origin/main");
    REQUIRE(om != NULL, "origin/main assente nel modello");
    CHECK(om->is_remote, "origin/main: non remoto");
    CHECK(om->mirror_of && strcmp(om->mirror_of, "main") == 0,
          "origin/main: specchio di %s", om->mirror_of ? om->mirror_of : "-");
    CHECK(om->state == PARENT_CERTAIN && om->parent &&
              strcmp(om->parent, "main") == 0,
          "origin/main: padre certo atteso 'main'");
    CHECK(om->is_long_lived, "origin/main: long-lived come main");

    const char *b2h = hash_of(data, "B2:");
    const char *b3h = hash_of(data, "B3:");
    const char *c1h = hash_of(data, "C1:");
    const char *ch = hash_of(data, "terza revisione");

    /* branch1: mergiato in main; padre dedotto, nessun commit esclusivo */
    const BranchNode *b1 = branch_named(&hist, "branch1");
    REQUIRE(b1 != NULL, "branch1 assente");
    CHECK(b1->state == PARENT_DEDUCED && b1->parent &&
              strcmp(b1->parent, "main") == 0,
          "branch1: padre %s (stato %d)", b1->parent ? b1->parent : "-",
          (int)b1->state);
    CHECK(b1->first_exclusive_hash == NULL,
          "branch1: atteso nessun commit esclusivo");
    CHECK(b1->fork_hash && b3h && strcmp(b1->fork_hash, b3h) == 0,
          "branch1: fork atteso su B3");

    /* branch2: diramato da B2 lungo branch1 */
    const BranchNode *b2 = branch_named(&hist, "branch2");
    REQUIRE(b2 != NULL, "branch2 assente");
    CHECK(b2->state == PARENT_DEDUCED && b2->parent &&
              strcmp(b2->parent, "branch1") == 0,
          "branch2: padre %s", b2->parent ? b2->parent : "-");
    CHECK(b2->fork_hash && b2h && strcmp(b2->fork_hash, b2h) == 0,
          "branch2: fork atteso su B2");
    CHECK(b2->first_exclusive_hash && c1h &&
              strcmp(b2->first_exclusive_hash, c1h) == 0,
          "branch2: primo commit esclusivo atteso C1");

    /* feature/x: da C su main */
    const BranchNode *fx = branch_named(&hist, "feature/x");
    REQUIRE(fx != NULL, "feature/x assente");
    CHECK(fx->state == PARENT_DEDUCED && fx->parent &&
              strcmp(fx->parent, "main") == 0,
          "feature/x: padre %s", fx->parent ? fx->parent : "-");
    CHECK(fx->fork_hash && ch && strcmp(fx->fork_hash, ch) == 0,
          "feature/x: fork atteso su C");

    /* ff: assorbito con fast-forward in main */
    const BranchNode *ffb = branch_named(&hist, "ff");
    REQUIRE(ffb != NULL, "ff assente");
    CHECK(ffb->state == PARENT_DEDUCED && ffb->parent &&
              strcmp(ffb->parent, "main") == 0,
          "ff: padre %s", ffb->parent ? ffb->parent : "-");

    /* caso ambiguo: ambchild non sceglie tra amb1/amb2/ammbase */
    const BranchNode *ac = branch_named(&hist, "ambchild");
    REQUIRE(ac != NULL, "ambchild assente");
    CHECK(ac->state == PARENT_AMBIGUOUS, "ambchild: stato %d", (int)ac->state);
    CHECK(ac->parent == NULL, "ambchild: padre inventato");
    CHECK(ac->candidate_count == 3, "ambchild: %zu candidati",
          ac->candidate_count);
    CHECK(candidates_contain(ac, "amb1") && candidates_contain(ac, "amb2") &&
              candidates_contain(ac, "ambbase"),
          "ambchild: candidati inattesi");

    const char *amb[] = {"amb1", "amb2", "ambbase"};
    for (int i = 0; i < 3; i++) {
        const BranchNode *b = branch_named(&hist, amb[i]);
        REQUIRE(b != NULL, "branch ambiguo assente");
        CHECK(b->state == PARENT_AMBIGUOUS && b->parent == NULL,
              "%s: atteso ambiguo senza padre", amb[i]);
    }

    /* determinismo: una seconda costruzione da' lo stesso modello */
    History hist2;
    REQUIRE(history_build(&hist2, data, &cfg, err, sizeof err),
            "seconda history_build fallita");
    compare_histories(&hist, &hist2);
    history_free(&hist2);

    history_free(&hist);
    config_free(&cfg);
    return 0;
}

static int test_overrides(const GitData *data)
{
    char err[1024];

    /* override valido: branch2 <- ff (con precedenza sull'euristica) */
    ParentOverride ov[2] = {{"branch2", "ff"}, {NULL, NULL}};
    GraphConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.overrides = ov;
    cfg.override_count = 1;

    History hist;
    REQUIRE(history_build(&hist, data, &cfg, err, sizeof err),
            "history_build con override fallita");
    const BranchNode *b2 = branch_named(&hist, "branch2");
    REQUIRE(b2 != NULL, "branch2 assente");
    CHECK(b2->state == PARENT_CERTAIN && b2->parent &&
              strcmp(b2->parent, "ff") == 0,
          "override: branch2 padre %s (stato %d)",
          b2->parent ? b2->parent : "-", (int)b2->state);
    history_free(&hist);

    /* override verso un branch inesistente: ignorato */
    ov[0].branch = "branch2";
    ov[0].parent = "non-esiste";
    REQUIRE(history_build(&hist, data, &cfg, err, sizeof err),
            "history_build con override invalido fallita");
    b2 = branch_named(&hist, "branch2");
    REQUIRE(b2 != NULL, "branch2 assente (2)");
    CHECK(b2->state == PARENT_DEDUCED && b2->parent &&
              strcmp(b2->parent, "branch1") == 0,
          "override invalido: branch2 padre %s",
          b2->parent ? b2->parent : "-");
    history_free(&hist);

    /* override incrociati (ciclo): nessun blocco, nodo minore ambiguo */
    ov[0].branch = "branch2";
    ov[0].parent = "ff";
    ov[1].branch = "ff";
    ov[1].parent = "branch2";
    cfg.override_count = 2;
    REQUIRE(history_build(&hist, data, &cfg, err, sizeof err),
            "history_build con ciclo fallita");
    b2 = branch_named(&hist, "branch2");
    const BranchNode *ffb = branch_named(&hist, "ff");
    REQUIRE(b2 && ffb, "branch assenti nel ciclo");
    CHECK(b2->state == PARENT_AMBIGUOUS && b2->parent == NULL,
          "ciclo: branch2 doveva diventare ambiguo (minore del ciclo)");
    CHECK(ffb->state == PARENT_CERTAIN && ffb->parent &&
              strcmp(ffb->parent, "branch2") == 0,
          "ciclo: ff mantiene l'override");
    history_free(&hist);
    return 0;
}

static int test_long_lived_custom(const GitData *data)
{
    /* l'utente dichiara branch1 come long-lived: niente padre euristica */
    char *ll[] = {"branch1"};
    GraphConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.long_lived = ll;
    cfg.long_lived_count = 1;

    char err[1024];
    History hist;
    REQUIRE(history_build(&hist, data, &cfg, err, sizeof err),
            "history_build con long-lived fallita");

    const BranchNode *b1 = branch_named(&hist, "branch1");
    REQUIRE(b1 != NULL, "branch1 assente");
    CHECK(b1->is_long_lived, "branch1: non marcato long-lived");
    CHECK(b1->state == PARENT_NONE && b1->parent == NULL,
          "branch1 long-lived: padre non dedotto");

    const BranchNode *main_b = branch_named(&hist, "main");
    REQUIRE(main_b != NULL, "main assente");
    CHECK(!main_b->is_long_lived, "main: non e' long-lived in questa config");

    /* branch1 resta un candidato valido per chi vi si dirama */
    const BranchNode *b2 = branch_named(&hist, "branch2");
    REQUIRE(b2 != NULL, "branch2 assente");
    CHECK(b2->parent && strcmp(b2->parent, "branch1") == 0,
          "branch2: padre %s", b2->parent ? b2->parent : "-");

    history_free(&hist);
    return 0;
}

static void test_implicit_trunk_helper(void)
{
    char *names[] = {"dev", "topic"};
    GraphConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    config_set_implicit_trunk(&cfg, names, 2);
    CHECK(cfg.long_lived_count == 0,
          "senza main/master non deve esserci tronco implicito");
    config_free(&cfg);

    char *names2[] = {"dev", "master"};
    memset(&cfg, 0, sizeof cfg);
    config_set_implicit_trunk(&cfg, names2, 2);
    CHECK(cfg.long_lived_count == 1 && strcmp(cfg.long_lived[0], "master") == 0,
          "tronco implicito: atteso master");
    config_free(&cfg);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) {
        fprintf(stderr, "uso: test_core <percorso-repo-di-test>\n");
        return 2;
    }

    GitData data;
    char err[1024];
    if (!gitdata_load(&data, argv[1], err, sizeof err)) {
        fprintf(stderr, "PREREQUISITO FALLITO: %s\n", err);
        return 2;
    }

    /* determinismo del caricamento */
    GitData data2;
    if (!gitdata_load(&data2, argv[1], err, sizeof err)) {
        fprintf(stderr, "PREREQUISITO FALLITO (secondo load): %s\n", err);
        gitdata_free(&data);
        return 2;
    }
    CHECK(data.commit_count == data2.commit_count,
          "load non deterministico: commit_count");
    for (size_t i = 0; i < data.commit_count && i < data2.commit_count; i++) {
        CHECK(strcmp(data.commits[i].hash, data2.commits[i].hash) == 0,
              "load non deterministico alla posizione %zu", i);
    }
    gitdata_free(&data2);

    int rc = 0;
    rc |= test_gitdata(&data);
    rc |= test_history_default(&data);
    rc |= test_overrides(&data);
    rc |= test_long_lived_custom(&data);
    test_implicit_trunk_helper();

    gitdata_free(&data);

    printf("%d test superati, %d falliti\n", g_pass, g_fail);
    if (g_fail > 0)
        return 1;
    return rc == 0 ? 0 : 2;
}

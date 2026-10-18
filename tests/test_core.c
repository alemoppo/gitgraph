/* test_core.c - test automatici della fase 1 (senza finestra).
 *
 * Uso: test_core <percorso-repo-di-test>
 *
 * Copre: parsing di git log/for-each-ref, ordine dei genitori, ordine
 * topologico, esclusione dello stash, tag annotati dereferenziati, regole del
 * modello dei branch (padre, stato, fork, primo commit proprio, gruppo di
 * alias, casi ambigui, motivazioni sulle date), precedenza degli
 * override (stato 'configurato'), long-lived, determinismo.
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
        CHECK((x->first_own_commit == NULL) == (y->first_own_commit == NULL),
              "determinismo: primo commit proprio di '%s'", x->name);
        if (x->first_own_commit && y->first_own_commit)
            CHECK(strcmp(x->first_own_commit, y->first_own_commit) == 0,
                  "determinismo: primo commit proprio di '%s'", x->name);
        CHECK(x->alias_count == y->alias_count,
              "determinismo: alias di '%s'", x->name);
        for (size_t k = 0; k < x->alias_count && k < y->alias_count; k++)
            CHECK(strcmp(x->alias_members[k], y->alias_members[k]) == 0,
                  "determinismo: alias di '%s'", x->name);
        CHECK(x->tip_pos == y->tip_pos, "determinismo: punta di '%s'", x->name);
        CHECK(x->genealogy_undetermined == y->genealogy_undetermined,
              "determinismo: genealogia di '%s'", x->name);
    }
}

/* ---------- test ---------- */

static int test_gitdata(const GitData *d)
{
    /* il repo di prova: 31 commit, 28 riferimenti (21 branch, 7 tag; lo stash e' escluso) */
    CHECK(d->commit_count == 31, "commit_count = %zu, atteso 31",
          d->commit_count);
    CHECK(d->ref_count == 28, "ref_count = %zu, atteso 28", d->ref_count);
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

    /* remote-tracking con punte diverse (stesso nome canonico di main) */
    const GitRef *om = ref_named(d, "origin/main");
    const GitRef *um = ref_named(d, "upstream/main");
    const GitRef *ob1 = ref_named(d, "origin/branch1");
    const GitRef *ofx = ref_named(d, "origin/feature-x");
    REQUIRE(om && um && ob1 && ofx, "riferimenti remoti assenti");
    CHECK(om->kind == REF_REMOTE_BRANCH, "origin/main: tipo errato");
    CHECK(dh && strcmp(om->target, dh) != 0, "origin/main: punta diversa da main");
    CHECK(um->kind == REF_REMOTE_BRANCH, "upstream/main: tipo errato");
    CHECK(dh && strcmp(um->target, dh) != 0,
          "upstream/main: punta diversa da main");
    const GitRef *local_main = ref_named(d, "main");
    const GitRef *local_b1 = ref_named(d, "branch1");
    const GitRef *local_fx = ref_named(d, "feature/x");
    CHECK(om->target != NULL && um->target != NULL && om->target[0] &&
              strcmp(om->target, um->target) != 0,
          "origin/main e upstream/main: punte diverse fra loro");
    if (local_b1)
        CHECK(strcmp(ob1->target, local_b1->target) == 0,
              "origin/branch1: stessa punta di branch1");
    if (local_fx)
        CHECK(strcmp(ofx->target, local_fx->target) == 0,
              "origin/feature-x: stessa punta di feature/x");
    (void)local_main;
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

    /* nodi: 17 branch locali + 4 remote-tracking */
    CHECK(hist.branch_count == 21, "branch_count = %zu, atteso 21",
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

    /* origin/main non e' piu' uno specchio: punta diversa da main (vedi i
     * controlli sui riferimenti remoti piu' avanti) */
    const BranchNode *om = branch_named(&hist, "origin/main");
    REQUIRE(om != NULL, "origin/main assente nel modello");
    CHECK(om->is_remote, "origin/main: non remoto");
    CHECK(om->mirror_of == NULL, "origin/main: non deve essere specchio di main");

    const char *b2h = hash_of(data, "B2:");
    const char *b3h = hash_of(data, "B3:");
    const char *b1h = hash_of(data, "B1:");
    const char *bh = hash_of(data, "B:");
    const char *c1h = hash_of(data, "C1:");
    const char *ch = hash_of(data, "terza revisione");
    const char *rhh = hash_of(data, "R:");

    /* main long-lived: nessun padre, nessun fork, nessun candidato */
    CHECK(main_b->fork_hash == NULL, "main: fork mostrato su un long-lived");
    CHECK(main_b->candidate_count == 0, "main: candidati non attesi");

    /* branch1: partito da B, poi mergiato in main. branch2 nasce da B2 che sta
     * sulla sua catena: su B2 arrivano due figli (B3 di branch1 e C1 di
     * branch2) e la struttura non dice chi e' nato prima: con l'ordine di
     * nascita non disponibile, i due sono fratelli per natura (regola 1) e il
     * punto di diramazione di branch1 si ferma a B2, dove resta in lizza solo
     * branch2 (sp e' nato piu' in alto, su B3). Nessun padre: lo stato e'
     * ambiguo con un solo candidato e il primo commit proprio e' il piu'
     * vecchio sopra il fork, B3. */
    const BranchNode *b1 = branch_named(&hist, "branch1");
    REQUIRE(b1 != NULL, "branch1 assente");
    CHECK(b1->state == PARENT_AMBIGUOUS && b1->parent == NULL,
          "branch1: padre %s (stato %d)", b1->parent ? b1->parent : "-",
          (int)b1->state);
    CHECK(b1->fork_hash && b2h && strcmp(b1->fork_hash, b2h) == 0,
          "branch1: fork atteso su B2 (branch2 nasce li')");
    CHECK(b1->first_own_commit && b3h &&
              strcmp(b1->first_own_commit, b3h) == 0,
          "branch1: primo commit proprio atteso B3");
    CHECK(b1->candidate_count == 1 && candidates_contain(b1, "branch2"),
          "branch1: candidato atteso branch2, trovati %zu",
          b1->candidate_count);
    CHECK(!candidates_contain(b1, "main") && !candidates_contain(b1, "sp"),
          "branch1: main e sp non possono essere candidati qui (la discesa si "
          "ferma al primo punto con un plausibile padre)");
    CHECK(b1->suggested_parent == NULL && b1->suggestion_basis == SUGGESTION_NONE,
          "branch1: e' il ramo piu' antico della diramazione, nessun "
          "suggerimento atteso (%s)",
          b1->suggested_parent ? b1->suggested_parent : "-");
    CHECK(b1->motivation && strstr(b1->motivation, "nessun padre suggerito"),
          "branch1: la motivazione deve dichiarare che non c'e' un padre "
          "suggerito: %s",
          b1->motivation ? b1->motivation : "-");

    /* branch2: diramato da B2 lungo branch1 */
    const BranchNode *b2 = branch_named(&hist, "branch2");
    REQUIRE(b2 != NULL, "branch2 assente");
    /* branch2 e' nato da B2, che sta sulla catena di branch1 e anche su quella
     * di sp: la struttura non distingue, quindi nessun padre e' dedotto. I
     * candidati sono elencati e il suggerimento (non un padre) e' il candidato
     * col primo commit proprio piu' vecchio: B1 (branch1) precede SP1 (sp). */
    CHECK(b2->state == PARENT_AMBIGUOUS && b2->parent == NULL,
          "branch2: stato %d con padre %s, atteso ambiguo senza padre",
          (int)b2->state, b2->parent ? b2->parent : "-");
    CHECK(candidates_contain(b2, "branch1") && candidates_contain(b2, "sp"),
          "branch2: candidati attesi branch1 e sp");
    /* tre nomi, due famiglie: origin/branch1 e' lo specchio di branch1 */
    CHECK(b2->candidate_count == 3 && candidates_contain(b2, "origin/branch1"),
          "branch2: candidati attesi branch1, origin/branch1 e sp, trovati %zu",
          b2->candidate_count);
    CHECK(b2->suggested_parent && strcmp(b2->suggested_parent, "branch1") == 0,
          "branch2: padre suggerito %s, atteso branch1",
          b2->suggested_parent ? b2->suggested_parent : "-");
    CHECK(b2->suggestion_basis == SUGGESTION_DATE,
          "branch2: base del suggerimento %d, atteso SUGGESTION_DATE",
          (int)b2->suggestion_basis);
    CHECK(b2->motivation && strstr(b2->motivation, "padre suggerito per data") &&
              strstr(b2->motivation, "poco affidabile con rebase"),
          "branch2: la motivazione deve dichiarare il suggerimento per data: %s",
          b2->motivation ? b2->motivation : "-");
    CHECK(b2->fork_hash && b2h && strcmp(b2->fork_hash, b2h) == 0,
          "branch2: fork atteso su B2");
    CHECK(b2->first_own_commit && c1h &&
              strcmp(b2->first_own_commit, c1h) == 0,
          "branch2: primo commit proprio atteso C1");

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

    /* caso ambiguo: amb1/amb2/ambbase sono un unico gruppo di alias sulla
     * stessa punta: stesso padre dedotto, ordine interno non determinabile */
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
    const char *ah = hash_of(data, "A:");
    const char *xh = hash_of(data, "X:");
    for (int i = 0; i < 3; i++) {
        const BranchNode *b = branch_named(&hist, amb[i]);
        REQUIRE(b != NULL, "branch ambiguo assente");
        CHECK(b->state == PARENT_DEDUCED && b->parent &&
                  strcmp(b->parent, "main") == 0,
              "%s: atteso padre dedotto 'main', trovato %s (stato %d)", amb[i],
              b->parent ? b->parent : "-", (int)b->state);
        CHECK(b->fork_hash && ah && strcmp(b->fork_hash, ah) == 0,
              "%s: fork atteso su A", amb[i]);
        CHECK(b->first_own_commit && xh && strcmp(b->first_own_commit, xh) == 0,
              "%s: primo commit proprio atteso X", amb[i]);
        CHECK(b->alias_count == 2, "%s: attesi 2 alias, trovati %zu", amb[i],
              b->alias_count);
        CHECK(b->motivation && strstr(b->motivation, "stessa punta di"),
              "%s: la motivazione deve dichiarare la stessa punta", amb[i]);
    }

    /* oB e oC nascono come oA dallo stesso punto R: i tre sono fratelli per
     * natura (regola 1) e nessuno contiene l'altro, quindi nessun padre e'
     * dedotto. Per oB e per oC resta in lizza oA, che ha il primo commit
     * proprio piu' vecchio (OA precede OC e OB): il suggerito e' oA, per data. */
    const BranchNode *oc = branch_named(&hist, "oC");
    REQUIRE(oc != NULL, "oC assente");
    /* oC e' nato da R come oA e come oB: due fratelli pari, due candidati
     * (non piu' un solo candidato "dedotto": la simmetria va dichiarata). */
    CHECK(oc->state == PARENT_AMBIGUOUS && oc->parent == NULL,
          "oC: atteso ambiguo senza padre, trovato %s (stato %d)",
          oc->parent ? oc->parent : "-", (int)oc->state);
    CHECK(oc->fork_hash && rhh && strcmp(oc->fork_hash, rhh) == 0,
          "oC: fork atteso su R");
    CHECK(oc->candidate_count == 2 && candidates_contain(oc, "oA") &&
              candidates_contain(oc, "oB"),
          "oC: candidati attesi oA e oB, trovati %zu", oc->candidate_count);
    CHECK(oc->suggested_parent && strcmp(oc->suggested_parent, "oA") == 0,
          "oC: padre suggerito %s, atteso oA",
          oc->suggested_parent ? oc->suggested_parent : "-");
    CHECK(oc->suggestion_basis == SUGGESTION_DATE,
          "oC: base del suggerimento %d, atteso SUGGESTION_DATE",
          (int)oc->suggestion_basis);
    CHECK(oc->motivation && strstr(oc->motivation, "padre suggerito per data") &&
              strstr(oc->motivation, "poco affidabile con rebase"),
          "oC: la motivazione deve dichiarare il suggerimento per data: %s",
          oc->motivation ? oc->motivation : "-");

    const BranchNode *ob = branch_named(&hist, "oB");
    REQUIRE(ob != NULL, "oB assente");
    CHECK(ob->state == PARENT_AMBIGUOUS && ob->parent == NULL,
          "oB: stato %d, atteso ambiguo (stessa base di oC)", (int)ob->state);
    CHECK(candidates_contain(ob, "oA") && candidates_contain(ob, "oC"),
          "oB: candidati attesi oA e oC");
    CHECK(ob->suggested_parent && strcmp(ob->suggested_parent, "oA") == 0,
          "oB: padre suggerito %s, atteso oA",
          ob->suggested_parent ? ob->suggested_parent : "-");
    CHECK(ob->suggestion_basis == SUGGESTION_DATE,
          "oB: base del suggerimento %d, atteso SUGGESTION_DATE",
          (int)ob->suggestion_basis);

    /* La frase "basato sulla data" e' quella dello scarto fra candidati due a
     * due, rimossa: il suggerimento usa altre parole e va solo dove c'e' una
     * scelta da dichiarare. */
    for (size_t i = 0; i < hist.branch_count; i++) {
        CHECK(hist.branches[i].motivation &&
                  strstr(hist.branches[i].motivation, "basato sulla data") == NULL,
              "%s: nessuno scarto per data atteso, trovato: %s",
              hist.branches[i].name,
              hist.branches[i].motivation ? hist.branches[i].motivation : "-");
    }
    /* oA: e' il ramo piu' antico del gruppo isolato: i suoi due candidati
     * (oB, oC) sono fratelli nati dopo di lui, quindi nessuno e' eleggibile
     * come padre suggerito e il caso resta ambiguo, dichiarato. */
    const BranchNode *oa = branch_named(&hist, "oA");
    REQUIRE(oa != NULL, "oA assente");
    CHECK(oa->state == PARENT_AMBIGUOUS && oa->parent == NULL,
          "oA: atteso ambiguo senza padre, trovato %s (stato %d)",
          oa->parent ? oa->parent : "-", (int)oa->state);
    CHECK(oa->fork_hash && rhh && strcmp(oa->fork_hash, rhh) == 0,
          "oA: fork atteso su R");
    CHECK(oa->candidate_count == 2 && candidates_contain(oa, "oB") &&
              candidates_contain(oa, "oC"),
          "oA: candidati attesi oB e oC, trovati %zu", oa->candidate_count);
    CHECK(oa->suggested_parent == NULL && oa->suggestion_basis == SUGGESTION_NONE,
          "oA: nessun padre suggerito atteso (e' il piu' antico del gruppo)");
    CHECK(oa->motivation && strstr(oa->motivation, "nessun padre suggerito"),
          "oA: la motivazione deve dichiarare che non c'e' un padre"
          " suggerito: %s",
          oa->motivation ? oa->motivation : "-");

    /* --- riferimenti locali e remote-tracking con punte diverse -------------- */
    /* origin/branch1 punta allo stesso commit di branch1: specchio provato */
    const BranchNode *ob1 = branch_named(&hist, "origin/branch1");
    REQUIRE(ob1 != NULL, "origin/branch1 assente");
    CHECK(ob1->mirror_of && strcmp(ob1->mirror_of, "branch1") == 0,
          "origin/branch1: specchio atteso di branch1, trovato %s",
          ob1->mirror_of ? ob1->mirror_of : "-");
    CHECK(ob1->state == PARENT_CERTAIN, "origin/branch1: stato certo atteso");
    CHECK(ob1->tip_pos == b1->tip_pos,
          "origin/branch1: stessa punta di branch1");

    /* origin/main e upstream/main hanno lo stesso nome canonico di main ma
     * punte diverse: nessuno specchio, nessuna relazione certa */
    const BranchNode *om2 = branch_named(&hist, "origin/main");
    const BranchNode *um = branch_named(&hist, "upstream/main");
    const BranchNode *mb = branch_named(&hist, "main");
    REQUIRE(om2 && um && mb, "riferimenti remoti assenti");
    CHECK(om2->mirror_of == NULL,
          "origin/main: non e' uno specchio (punta diversa da main)");
    CHECK(um->mirror_of == NULL,
          "upstream/main: non e' uno specchio (punta diversa da main)");
    CHECK(om2->state != PARENT_CERTAIN && um->state != PARENT_CERTAIN,
          "nessuna relazione certa sui remoti indipendenti");
    CHECK(om2->tip_pos != mb->tip_pos && um->tip_pos != mb->tip_pos,
          "i remoti devono conservare la propria punta reale");
    CHECK(om2->parent && strcmp(om2->parent, "main") == 0,
          "origin/main: padre dedotto main atteso, trovato %s",
          om2->parent ? om2->parent : "-");
    CHECK(um->parent && strcmp(um->parent, "main") == 0,
          "upstream/main: padre dedotto main atteso, trovato %s",
          um->parent ? um->parent : "-");
    /* ogni riferimento conserva la punta del proprio riferimento Git */
    for (size_t i = 0; i < hist.branch_count; i++) {
        const BranchNode *n = &hist.branches[i];
        const GitRef *r = &data->refs[n->ref_index];
        if (n->tip_pos != (size_t)-1)
            CHECK(strcmp(data->commits[n->tip_pos].hash, r->target) == 0,
                  "%s: la punta non coincide con il riferimento", n->name);
    }

    /* --- genealogia: secondo genitore di un merge, merge ripetuti, rebase -- */
    const char *c1h2 = hash_of(data, "SP1:");
    const BranchNode *sp = branch_named(&hist, "sp");
    REQUIRE(sp != NULL, "sp assente");
    CHECK(sp->state == PARENT_DEDUCED && sp->parent &&
              strcmp(sp->parent, "branch1") == 0,
          "sp: padre dedotto branch1 atteso, trovato %s",
          sp->parent ? sp->parent : "-");
    CHECK(sp->fork_hash && b3h && strcmp(sp->fork_hash, b3h) == 0,
          "sp: fork atteso su B3 (secondo genitore del merge)");
    CHECK(sp->first_own_commit && c1h2 && strcmp(sp->first_own_commit, c1h2) == 0,
          "sp: primo commit proprio atteso SP1");

    const char *rm1h = hash_of(data, "RM1:");
    const BranchNode *rm = branch_named(&hist, "rm");
    REQUIRE(rm != NULL, "rm assente");
    CHECK(rm->state == PARENT_DEDUCED && rm->parent &&
              strcmp(rm->parent, "main") == 0,
          "rm (merge ripetuti): padre dedotto main atteso");
    CHECK(rm->fork_hash && ch && strcmp(rm->fork_hash, ch) == 0,
          "rm: fork atteso su C, non sui commit dei merge");
    CHECK(rm->first_own_commit && rm1h && strcmp(rm->first_own_commit, rm1h) == 0,
          "rm: primo commit proprio atteso RM1");

    const BranchNode *rb = branch_named(&hist, "rb");
    const BranchNode *rbo = branch_named(&hist, "rb-old");
    REQUIRE(rb && rbo, "branch ribasati assenti");
    CHECK(rb->fork_hash && ch && strcmp(rb->fork_hash, ch) == 0,
          "rb (ribasato su C): il fork segue la storia, non il nome");
    CHECK(rb->first_own_commit != NULL && rbo->first_own_commit != NULL &&
              strcmp(rb->first_own_commit, rbo->first_own_commit) != 0,
          "rb e rb-old: hash dei commit proprio diversi (rebase effettuato)");
    CHECK(rb->fork_hash && bh && strcmp(rbo->fork_hash, bh) == 0,
          "rb-old: fork atteso su B");

    /* ramo senza antenati condivisi: genealogia non determinabile */
    const char *or1h = hash_of(data, "ORF1:");
    const BranchNode *orf = branch_named(&hist, "orf");
    REQUIRE(orf != NULL, "orf assente");
    CHECK(orf->state == PARENT_NONE && orf->parent == NULL,
          "orf: nessun padre (storia senza antenati condivisi)");
    CHECK(orf->genealogy_undetermined, "orf: genealogia non determinabile");
    CHECK(orf->first_own_commit && or1h && strcmp(orf->first_own_commit, or1h) == 0,
          "orf: primo commit proprio atteso ORF1");

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
    CHECK(b2->state == PARENT_CONFIGURED && b2->parent &&
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
    CHECK(b2->state == PARENT_AMBIGUOUS && b2->parent == NULL &&
              b2->suggested_parent && strcmp(b2->suggested_parent, "branch1") == 0,
          "override invalido: branch2 resta ambiguo con suggerimento %s",
          b2->suggested_parent ? b2->suggested_parent : "-");
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
    CHECK(ffb->state == PARENT_CONFIGURED && ffb->parent &&
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
    CHECK(b1->fork_hash == NULL && b1->candidate_count == 0,
          "branch1 long-lived: nessun fork ne' candidati");

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

/* ---------- quoting e cattura delle pipe (nessun blocco) ---------- */

/* Fallimento simulato della creazione del thread di lettura. */
static void *reader_thread_always_fails(void *reader)
{
    (void)reader;
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

/* File di riempimento (~300 KB, ben oltre la capacita' di una pipe). */
static bool write_padding_file(const char *path, char filler)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    char buf[4096];
    memset(buf, filler, sizeof buf);
    for (int i = 0; i < 75; i++) { /* 75 * 4096 = 307200 byte */
        if (fwrite(buf, 1, sizeof buf, f) != sizeof buf) {
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

static void test_cmdline_quoting(void)
{
    char line[512];
    const char *plain[] = { "git", "log", "--branches", NULL };
    gitdata_build_cmdline(plain, line, sizeof line);
    CHECK(strcmp(line, "git log --branches") == 0,
          "argomenti semplici: '%s'", line);

    /* spazi: l'intero argomento viene quotato */
    const char *spaced[] = { "git", "rev-parse", "--git-dir=C:/Users/test utente/x",
                             NULL };
    gitdata_build_cmdline(spaced, line, sizeof line);
    CHECK(strcmp(line, "git rev-parse \"--git-dir=C:/Users/test utente/x\"") == 0,
          "percorso con spazi non quotato correttamente: '%s'", line);

    /* argomento vuoto: due apici, per non farlo sparire */
    const char *empty[] = { "git", "log", "", NULL };
    gitdata_build_cmdline(empty, line, sizeof line);
    CHECK(strcmp(line, "git log \"\"") == 0, "argomento vuoto: '%s'", line);

    /* argomento quotato che termina con una barra: la barra va raddoppiata,
     * altrimenti la virgoletta di chiusura viene "mangiata" dal runtime */
    const char *slash[] = { "git", "log", "--dir=C:/cartella con spazio\\", NULL };
    gitdata_build_cmdline(slash, line, sizeof line);
    CHECK(strcmp(line, "git log \"--dir=C:/cartella con spazio\\\\\"") == 0,
          "barra finale non raddoppiata: '%s'", line);

    /* virgolette incorporate: diventano \" */
    const char *quoted[] = { "git", "log", "--format=ciao \"%s\"", NULL };
    gitdata_build_cmdline(quoted, line, sizeof line);
    CHECK(strstr(line, "\\\"%s\\\"") != NULL,
          "virgolette incorporate non protette: '%s'", line);
}

/* Un figlio che riempie una pipe non deve poter bloccare chi legge: la
 * lettura avviene con un thread per pipe, quindi nessuna delle due puo'
 *Fermare l'altra. Qui si riempiono entrambe, ben oltre la capacita' della pipe. */
static void test_pipe_capture(void)
{
    /* stdout: ~250 KB */
    const char *big_out[] = { "cmd.exe", "/c",
                              "for /L %i in (1,1,40000) do @echo out-%i", NULL };
    StrBuf out, errout;
    sb_init(&out);
    sb_init(&errout);
    int code = -1;
    char err[512];
    bool ok = gitdata_capture(big_out, &out, &errout, &code, err, sizeof err);
    CHECK(ok, "cattura stdout fallita: %s", err);
    CHECK(out.len > 200000, "stdout catturato troppo corto: %zu byte", out.len);
    CHECK(strstr(out.data, "out-40000") != NULL, "stdout incompleto");
    CHECK(code == 0, "codice di uscita atteso 0, trovato %d", code);
    sb_clear(&out);
    sb_clear(&errout);

    /* stderr: ~300 KB, mentre stdout resta vuota */
    const char *big_err[] = { "cmd.exe", "/c",
                              "for /L %i in (1,1,40000) do @echo err-%i 1>&2", NULL };
    ok = gitdata_capture(big_err, &out, &errout, &code, err, sizeof err);
    CHECK(ok, "cattura stderr fallita: %s", err);
    CHECK(errout.len > 200000, "stderr catturato troppo corto: %zu byte",
          errout.len);
    CHECK(strstr(errout.data, "err-40000") != NULL, "stderr incompleto");
    sb_free(&out);
    sb_free(&errout);

    /* comando che non esiste: fallisce senza crash e con messaggio */
    const char *missing[] = { "gitgraph_programma_inesistente", NULL };
    StrBuf o2, e2;
    sb_init(&o2);
    sb_init(&e2);
    err[0] = '\0';
    ok = gitdata_capture(missing, &o2, &e2, &code, err, sizeof err);
    CHECK(!ok, "un programma inesistente deve far fallire la cattura");
    CHECK(err[0] != '\0', "il fallimento di avvio deve lasciare un messaggio");
    CHECK(code == -1, "senza processo non c'e' un codice di uscita (atteso -1, trovato %d)",
          code);
    sb_free(&o2);
    sb_free(&e2);

    /* uscita non zero: non e' un errore, il codice viene riportato */
    const char *failing[] = { "cmd.exe", "/c", "echo prima & exit /b 3", NULL };
    StrBuf o3, e3;
    sb_init(&o3);
    sb_init(&e3);
    err[0] = '\0';
    ok = gitdata_capture(failing, &o3, &e3, &code, err, sizeof err);
    CHECK(ok, "un codice di uscita non zero non deve far fallire la cattura");
    CHECK(code == 3, "codice di uscita atteso 3, trovato %d", code);
    CHECK(strstr(o3.data, "prima") != NULL, "l'output prima dell'uscita deve arrivare");
    sb_free(&o3);
    sb_free(&e3);

    /* entrambe le pipe attive *contemporaneamente*: il comando scrive un file
     * su stdout e, tramite un processo figlio che eredita la pipe di errore,
     * un altro su stderr. Entrambi i file superano di molto la capacita' di
     * una pipe, quindi senza lettura concorrente il figlio si bloccherebbe. */
    char out_file[MAX_PATH], err_file[MAX_PATH], temp_dir[MAX_PATH];
    DWORD tlen = GetTempPathA(sizeof temp_dir, temp_dir);
    bool tmp_ok = (tlen > 0 && tlen < sizeof temp_dir);
    CHECK(tmp_ok, "percorso temporaneo non ottenibile");
    if (!tmp_ok)
        return;
    snprintf(out_file, sizeof out_file, "%sgg_pipe_out.txt", temp_dir);
    snprintf(err_file, sizeof err_file, "%sgg_pipe_err.txt", temp_dir);
    bool wrote = write_padding_file(out_file, 'o') && write_padding_file(err_file, 'e');
    CHECK(wrote, "scrittura dei file di prova fallita");
    if (!wrote)
        return;
    if (strchr(temp_dir, ' ')) {
        /* i path con spazi andrebbero quotati dentro il comando, ma le
         * virgolette annidate non sono ammissibili nella riga di /c: in quel
         * caso il caso viene saltato (e dichiarato) invece di fallire */
        CHECK(false, "saltato: la directory temporanea contiene spazi ('%s')",
              temp_dir);
        remove(out_file);
        remove(err_file);
        return;
    }

    char both_cmd[1024];
    snprintf(both_cmd, sizeof both_cmd, "start /b cmd /c type %s 1>&2 & type %s",
             err_file, out_file);
    const char *both[] = { "cmd.exe", "/c", both_cmd, NULL };
    StrBuf o4, e4;
    sb_init(&o4);
    sb_init(&e4);
    err[0] = '\0';
    ok = gitdata_capture(both, &o4, &e4, &code, err, sizeof err);
    CHECK(ok, "cattura con entrambe le pipe attive: %s", err);
    CHECK(o4.len > 250000, "stdout con entrambe le pipe attive troppo corto: %zu byte",
          o4.len);
    CHECK(e4.len > 250000, "stderr con entrambe le pipe attive troppo corto: %zu byte",
          e4.len);
    CHECK(code == 0, "codice di uscita atteso 0, trovato %d", code);
    sb_free(&o4);
    sb_free(&e4);

    /* stesso comando, ma senza i thread di lettura: la cattura deve fallire in
     * modo controllato (figlio terminato e raccolto), non restare bloccata */
    gitdata_set_reader_thread_hook(reader_thread_always_fails);
    StrBuf o5, e5;
    sb_init(&o5);
    sb_init(&e5);
    err[0] = '\0';
    ok = gitdata_capture(both, &o5, &e5, &code, err, sizeof err);
    CHECK(!ok, "senza thread di lettura la cattura deve fallire");
    CHECK(strstr(err, "thread") != NULL,
          "il messaggio deve parlare del thread di lettura: '%s'", err);
    CHECK(strstr(err, "terminato") != NULL,
          "il messaggio deve dire che il processo e' stato terminato: '%s'", err);
    sb_free(&o5);
    sb_free(&e5);
    gitdata_set_reader_thread_hook(NULL);
    remove(out_file);
    remove(err_file);

    /* aggancio rimosso: la cattura torna a funzionare */
    sb_init(&out);
    sb_init(&errout);
    const char *simple[] = { "cmd.exe", "/c", "echo di nuovo", NULL };
    ok = gitdata_capture(simple, &out, &errout, &code, err, sizeof err);
    CHECK(ok, "dopo aver rimosso l'aggancio la cattura deve riuscire: %s", err);
    CHECK(strstr(out.data, "di nuovo") != NULL, "output atteso");
    sb_free(&out);
    sb_free(&errout);
}

/* ---------- sicurezza: ricerca di git.exe ---------- */

/* Un git.exe nella directory corrente non deve mai essere eseguito: con
 * lpApplicationName NULL Windows lo cerca anche li', e il repository
 * predefinito e' proprio la directory corrente.
 *
 * Il test mette un finto git.exe (fake_git, che scrive un file sentinella)
 * nella directory corrente e ricarica il repository: il caricamento deve
 * riuscire e il sentinella non deve comparire. */
static void test_git_lookup_safety(const char *fake_git_path, const char *repo_path)
{
    if (!fake_git_path || !*fake_git_path) {
        printf("saltato: percorso del finto git.exe non fornito\n");
        return;
    }
    char temp[MAX_PATH];
    DWORD tn = GetTempPathA(sizeof temp, temp);
    bool temp_ok = (tn > 0 && tn < sizeof temp);
    CHECK(temp_ok, "percorso temporaneo non ottenibile");
    if (!temp_ok)
        return;
    char work[MAX_PATH];
    snprintf(work, sizeof work, "%sgg_git_lookup", temp);
    char fake[MAX_PATH], sentinel[MAX_PATH];
    snprintf(fake, sizeof fake, "%s\\git.exe", work);
    snprintf(sentinel, sizeof sentinel, "%s\\git_runs.txt", work);
    remove(sentinel);
    CreateDirectoryA(work, NULL);
    bool copied = CopyFileA(fake_git_path, fake, TRUE) != 0;
    CHECK(copied, "copia del finto git.exe fallita in %s", work);
    if (!copied)
        return;

    char cwd[MAX_PATH];
    DWORD cn = GetCurrentDirectoryA(sizeof cwd, cwd);
    CHECK(cn > 0 && cn < sizeof cwd, "directory corrente non ottenibile");
    /* il percorso del repository deve essere assoluto: dopo il cambio di
     * directory uno relativo non vale piu' */
    char repo_abs[MAX_PATH];
    DWORD rn = GetFullPathNameA(repo_path, (DWORD)sizeof repo_abs, repo_abs, NULL);
    CHECK(rn > 0 && rn < sizeof repo_abs, "percorso del repository non assolutizzato");
    if (rn == 0 || rn >= sizeof repo_abs) {
        remove(fake);
        RemoveDirectoryA(work);
        return;
    }
    CHECK(SetCurrentDirectoryA(work) != 0, "cambio di directory fallito");

    /* La ricerca avviene solo nelle voci assolute del PATH: qui la directory
     * corrente non e' nel PATH, come nella situazione reale (il pericolo e'
     * che Windows la consideri comunque). */
    GitData d;
    char err[1024];
    bool ok = gitdata_load(&d, repo_abs, err, sizeof err);
    SetCurrentDirectoryA(cwd);
    CHECK(ok, "caricamento con git.exe finto nella cwd fallito: %s", err);
    if (!ok) {
        remove(sentinel);
        remove(fake);
        RemoveDirectoryA(work);
        return;
    }
    CHECK(d.commit_count > 0, "caricamento con git.exe finto: nessun commit");
    gitdata_free(&d);

    CHECK(GetFileAttributesA(sentinel) == INVALID_FILE_ATTRIBUTES,
          "il finto git.exe e' stato eseguito: sentinella %s presente", sentinel);

    /* controllo positivo, con un sentinella tutto suo: il finto git.exe, eseguito
     * davvero, lascia il segno. Dimostra che il test non e' vacuo, cioe' che il
     * file sopra verrebbe scritto se il finto programma venisse eseguito al
     * posto del vero git. */
    char positive[MAX_PATH];
    snprintf(positive, sizeof positive, "%s\\controllo_positivo.txt", work);
    remove(positive);
    SetEnvironmentVariableA("GG_FAKE_GIT_SENTINEL", positive);
    StrBuf so, se;
    sb_init(&so);
    sb_init(&se);
    int code = -1;
    const char *fake_argv[] = { fake, NULL };
    bool ran = gitdata_capture(fake_argv, &so, &se, &code, err, sizeof err);
    CHECK(ran, "il finto git.exe non e' nemmeno partito: %s", err);
    CHECK(GetFileAttributesA(positive) != INVALID_FILE_ATTRIBUTES,
          "controllo positivo fallito: il finto git.exe non ha scritto nulla");
    SetEnvironmentVariableA("GG_FAKE_GIT_SENTINEL", NULL);
    sb_free(&so);
    sb_free(&se);

    remove(sentinel);
    remove(positive);
    remove(fake);
    RemoveDirectoryA(work);
}

/* Scelta delle voci del PATH per git.exe.
 *
 * Le funzioni sono provate qui direttamente: la ricerca vera e' memorizzata
 * dopo la prima chiamata (il processo non deve ripeterla), quindi un test che
 * la richiamasse non potrebbe provarle piu' di una volta. */
static void check_entry(const wchar_t *entry, bool atteso)
{
    bool ok = git_path_entry_is_usable(entry, wcslen(entry));
    CHECK(ok == atteso, "voce '%ls': atteso %s, trovato %s", entry,
          atteso ? "utilizzabile" : "scartata", ok ? "utilizzabile" : "scartata");
}

static void test_path_entry_selection(void)
{
    /* scartate: vuota, punto, punto due, voce relativa */
    check_entry(L"", false);
    check_entry(L".", false);
    check_entry(L"..", false);
    check_entry(L"rel\\dir", false);
    check_entry(L"bin", false);
    /* non sono assolute: drive senza barra */
    check_entry(L"C:", false);
    check_entry(L"C:rel\\dir", false);
    /* assolute */
    check_entry(L"C:\\", true);
    check_entry(L"C:\\Program Files\\Git\\cmd", true);
    check_entry(L"D:/tools/git", true);
    /* UNC: assoluta anche senza lettera di drive */
    check_entry(L"\\\\server\\share", true);
    check_entry(L"\\\\server\\share\\", true);
    /* virgolette ai bordi: alcuni installer le mettono, non sono parte del
     * percorso e la voce resta utilizzabile */
    check_entry(L"\"C:\\Program Files\\Git\\cmd\"", true);
    check_entry(L"\"\\\\server\\share\"", true);
    /* virgolette ma voce non assoluta: resta scartata */
    check_entry(L"\".\"", false);
    check_entry(L"\"rel\\dir\"", false);
    /* solo virgolette: voce vuota */
    check_entry(L"\"\"", false);
}

/* Da voce del PATH a nome del file da cercare: la costruzione parte sempre
 * dalla voce originale (con virgolette e spazi ai bordi, che non sono parte
 * del percorso) e aggiunge "\\git.exe": se la voce termina con una barra
 * quella non va replicata, se e' la radice del drive (C:\) la barra resta. */
static void check_candidate(const wchar_t *entry, const wchar_t *atteso)
{
    wchar_t buf[MAX_PATH];
    bool ok = git_path_candidate(entry, wcslen(entry), buf, MAX_PATH);
    CHECK((ok && atteso != NULL) || (!ok && atteso == NULL),
          "candidato della voce '%ls': atteso %s", entry,
          atteso ? "un candidato" : "nessun candidato");
    if (ok && atteso)
        CHECK(wcscmp(buf, atteso) == 0,
              "candidato della voce '%ls': atteso '%ls', trovato '%ls'", entry,
              atteso, buf);
}

static void test_path_candidate(void)
{
    /* i due casi richiesti: virgolette ai bordi e barra finale */
    check_candidate(L"\"C:\\Program Files\\Git\\cmd\"",
                    L"C:\\Program Files\\Git\\cmd\\git.exe");
    check_candidate(L"C:\\Git\\cmd\\", L"C:\\Git\\cmd\\git.exe");
    /* le spaziature ai bordi non sono parte del percorso, quelle interne si' */
    check_candidate(L" \"C:\\Program Files\\Git\\cmd\" ",
                    L"C:\\Program Files\\Git\\cmd\\git.exe");
    check_candidate(L"C:\\Program Files\\Git\\cmd/",
                    L"C:\\Program Files\\Git\\cmd\\git.exe");
    /* radice del drive: la barra finale resta */
    check_candidate(L"C:\\", L"C:\\git.exe");
    check_candidate(L"D:/", L"D:\\git.exe");
    /* scartate: nessun candidato (e out non toccato) */
    check_candidate(L".", NULL);
    check_candidate(L"..", NULL);
    check_candidate(L"\".\"", NULL);
    check_candidate(L"rel\\dir", NULL);
    check_candidate(L"\"C:rel\\dir\"", NULL);
    check_candidate(L"C:", NULL);
    /* buffer piccolo: il candidato non entra */
    wchar_t piccolo[8];
    CHECK(!git_path_candidate(L"C:\\Git\\cmd", 10, piccolo,
                              sizeof piccolo / sizeof piccolo[0]),
          "buffer piccolo: il candidato doveva essere rifiutato");
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) {
        fprintf(stderr, "uso: test_core <percorso-repo-di-test> [percorso-finto-git]\n");
        return 2;
    }
    const char *fake_git = (argc > 2) ? argv[2] : NULL;

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
    test_cmdline_quoting();
    test_pipe_capture();
    test_path_entry_selection();
    test_path_candidate();
    test_git_lookup_safety(fake_git, argv[1]);
    test_implicit_trunk_helper();

    gitdata_free(&data);

    printf("%d test superati, %d falliti\n", g_pass, g_fail);
    if (g_fail > 0)
        return 1;
    return rc == 0 ? 0 : 2;
}

/* test_config.c - test automatici del parsing di graph.cfg (senza finestra).
 *
 * Uso: test_config <directory-di-lavoro> <percorso-repo-di-test>
 *
 * <directory-di-lavoro> serve solo per i file di prova che il test scrive.
 *
 * Copre: sezioni e commenti, wildcard in [long-lived] (espansione e ordine),
 * tronco implicito, override in [parents], errori che rifiutano il file,
 * avvisi che lo lasciano valido, cicli, assenza del file e uso end-to-end
 * con graph.cfg dentro il repository di prova (override -> stato
 * 'configurato', long-lived senza fork ne' candidati).
 *
 * Esce con 0 se tutti i test passano, 1 se qualcuno fallisce, 2 su errore di
 * preparazione.
 */
#include "config.h"
#include "gitdata.h"
#include "history.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
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
            printf("FALLITO %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
        }                                                               \
    } while (0)

/* Fallimento di preparazione: il test non ha senso a continuare. */
#define REQUIRE(cond, ...)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            g_fail++;                                                   \
            printf("FALLITO %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
            exit(2);                                                    \
        }                                                               \
        g_pass++;                                                       \
    } while (0)

static char g_work[512];
static const char *g_repo = NULL;

/* Branch finti del repository: i pattern vengono espansi su questi. */
static char *g_fake_names[] = {"main", "release/1.0", "release/1.1", "release/1.0/hotfix",
                               "origin/main", "feature/x", "bugfix/1"};
#define FAKE_COUNT (sizeof(g_fake_names) / sizeof(g_fake_names[0]))

/* Scrive un graph.cfg di prova e lo carica. */
static bool load_text(const char *name, const char *text, GraphConfig *cfg,
                      ConfigReport *report)
{
    char path[768];
    snprintf(path, sizeof path, "%s/%s", g_work, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    char err[512];
    return config_load(cfg, path, g_fake_names, FAKE_COUNT, report, err, sizeof err);
}

static bool report_has(const ConfigReport *r, const char *needle)
{
    for (size_t i = 0; i < r->count; i++) {
        if (strstr(r->items[i].text, needle) != NULL)
            return true;
    }
    return false;
}

static bool ll_contains(const GraphConfig *cfg, const char *name)
{
    for (size_t i = 0; i < cfg->long_lived_count; i++)
        if (strcmp(cfg->long_lived[i], name) == 0)
            return true;
    return false;
}

static const char *override_parent(const GraphConfig *cfg, const char *branch)
{
    for (size_t i = 0; i < cfg->override_count; i++)
        if (strcmp(cfg->overrides[i].branch, branch) == 0)
            return cfg->overrides[i].parent;
    return NULL;
}

/* ---------- parsing di base e wildcard ---------- */

static void test_parse_and_wildcards(void)
{
    /* file assente: nessun errore, tronco implicito main */
    {
        GraphConfig cfg;
        ConfigReport rep;
        char err[512];
        bool ok = config_load(&cfg, "percorso/inesistente.cfg", g_fake_names,
                              FAKE_COUNT, &rep, err, sizeof err);
        CHECK(ok, "file assente: config_load dovrebbe riuscire");
        CHECK(rep.count == 0, "file assente: nessun messaggio");
        CHECK(cfg.long_lived_count == 1 && strcmp(cfg.long_lived[0], "main") == 0,
              "file assente: atteso tronco implicito main");
        CHECK(!cfg.long_lived_explicit, "file assente: sezione non esplicita");
        config_report_free(&rep);
        config_free(&cfg);
    }

    /* sezione, commenti, wildcard, spazi, CRLF, BOM */
    {
        GraphConfig cfg;
        ConfigReport rep;
        const char *text =
            "\xef\xbb\xbf# configurazione di esempio\r\n"
            "\r\n"
            "[long-lived]\r\n"
            "main\r\n"
            "; i rami di release sono trunk secondari\r\n"
            "release/*\r\n"
            "\r\n"
            "[parents]\r\n"
            "  feature/x  =  main  \r\n"
            "bugfix/1 = release/1.0\r\n";
        bool ok = load_text("ok.cfg", text, &cfg, &rep);
        CHECK(ok, "file valido: config_load fallita (%s)", ok ? "" : "errore");
        CHECK(!report_has(&rep, "errore"), "file valido: errori imprevisti");
        CHECK(cfg.long_lived_explicit, "[long-lived] esplicita non riconosciuta");
        /* '*' non attraversa '/': release/1.0/hotfix non deve entrare */
        CHECK(cfg.long_lived_count == 3, "attesi 3 long-lived, trovati %zu",
              cfg.long_lived_count);
        CHECK(ll_contains(&cfg, "main") && ll_contains(&cfg, "release/1.0") &&
                  ll_contains(&cfg, "release/1.1"),
              "espansione del pattern release/* incompleta");
        CHECK(!ll_contains(&cfg, "release/1.0/hotfix"),
              "'release/*' non deve attraversare '/'");
        CHECK(config_long_lived_rank(&cfg, "main") == 0,
              "main deve avere rank 0");
        CHECK(config_long_lived_rank(&cfg, "release/1.0") == 1,
              "release/1.0 deve avere rank 1");
        CHECK(config_long_lived_rank(&cfg, "feature/x") == -1,
              "feature/x non deve essere long-lived");
        CHECK(cfg.override_count == 2, "attesi 2 override, trovati %zu",
              cfg.override_count);
        CHECK(override_parent(&cfg, "feature/x") &&
                  strcmp(override_parent(&cfg, "feature/x"), "main") == 0,
              "override feature/x = main non riconosciuto");
        CHECK(override_parent(&cfg, "bugfix/1") &&
                  strcmp(override_parent(&cfg, "bugfix/1"), "release/1.0") == 0,
              "override bugfix/1 = release/1.0 non riconosciuto");
        config_report_free(&rep);
        config_free(&cfg);
    }

    /* l'ordine della lista e' l'ordine di gerarchia; duplicati: avviso */
    {
        GraphConfig cfg;
        ConfigReport rep;
        const char *text =
            "[long-lived]\n"
            "release/*\n"
            "release/1.0\n"
            "main\n";
        bool ok = load_text("ordine.cfg", text, &cfg, &rep);
        CHECK(ok, "config con duplicati: caricamento fallito");
        CHECK(cfg.long_lived_count == 3 && strcmp(cfg.long_lived[0], "release/1.0") == 0,
              "l'ordine della lista deve essere rispettato, trovati %zu",
              cfg.long_lived_count);
        CHECK(report_has(&rep, "gia' long-lived"), "duplicato non segnalato");
        CHECK(config_report_error_count(&rep) == 0, "il duplicato e' un avviso");
        config_report_free(&rep);
        config_free(&cfg);
    }

    /* [long-lived] presente ma senza corrispondenze: niente tronco implicito */
    {
        GraphConfig cfg;
        ConfigReport rep;
        const char *text = "[long-lived]\nniente/*\n";
        bool ok = load_text("vuoto.cfg", text, &cfg, &rep);
        CHECK(ok, " caricamento fallito");
        CHECK(cfg.long_lived_count == 0, "lista vuota attesa");
        CHECK(cfg.long_lived_explicit, "la sezione esplicita deve contare");
        CHECK(report_has(&rep, "non corrisponde"), "pattern senza corrispondenze");
        config_report_free(&rep);
        config_free(&cfg);
    }
}

/* ---------- validazione ---------- */

/* Un errore rifiuta il file per intero: cfg resta vuota. */
static void test_errors_reject_file(void)
{
    struct {
        const char *name;
        const char *text;
        const char *needle;
    } cases[] = {
        {"sezione ignota", "[long-lived]\nmain\n\n[colori]\nrosso\n", "sezione sconosciuta"},
        {"riga fuori sezione", "main\n", "fuori da una sezione"},
        {"long-lived con =", "[long-lived]\nmain = altro\n", "senza '='"},
        {"parents senza =", "[parents]\nfeature/x main\n", "branch = padre"},
        {"parents senza padre", "[parents]\nfeature/x =\n", "manca il nome del padre"},
        {"parents senza branch", "[parents]\n = main\n", "manca il nome del branch"},
        {"wildcard in parents", "[parents]\nfeature/* = main\n", "wildcard"},
        {"padre di se stesso", "[parents]\nfeature/x = feature/x\n", "padre di se stesso"},
        {"nome con spazio", "[parents]\nfeature x = main\n", "riferimento valido"},
        {"doppio override", "[parents]\nfeature/x = main\nfeature/x = ff\n", "gia' dichiarato"},
        {"intestazione rotta", "[long-lived\nmain\n", "malformata"},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        GraphConfig cfg;
        ConfigReport rep;
        char path[768];
        snprintf(path, sizeof path, "%s/err_%zu.cfg", g_work, i);
        FILE *f = fopen(path, "wb");
        REQUIRE(f != NULL, "scrittura di %s fallita", path);
        fwrite(cases[i].text, 1, strlen(cases[i].text), f);
        fclose(f);

        char err[512];
        bool ok = config_load(&cfg, path, g_fake_names, FAKE_COUNT, &rep, err,
                              sizeof err);
        CHECK(!ok, "%s: il file doveva essere rifiutato", cases[i].name);
        CHECK(report_has(&rep, cases[i].needle), "%s: messaggio atteso '%s'",
              cases[i].name, cases[i].needle);
        CHECK(config_report_error_count(&rep) >= 1, "%s: nessun errore registrato",
              cases[i].name);
        /* niente configurazione a meta': resta solo il tronco implicito */
        CHECK(cfg.override_count == 0 && !cfg.long_lived_explicit,
              "%s: configurazione parziale nonostante l'errore", cases[i].name);
        CHECK(cfg.long_lived_count == 1 && strcmp(cfg.long_lived[0], "main") == 0,
              "%s: atteso il tronco implicito come ripiego", cases[i].name);
        config_report_free(&rep);
        config_free(&cfg);
        remove(path);
    }
}

/* Avvisi: il file resta valido e applicato. */
static void test_warnings_keep_file(void)
{
    GraphConfig cfg;
    ConfigReport rep;
    const char *text =
        "[long-lived]\n"
        "main\n"
        "non-esiste\n"
        "\n"
        "[parents]\n"
        "fantasma = main\n"
        "feature/x = branch-che-non-esiste\n";
    bool ok = load_text("avvisi.cfg", text, &cfg, &rep);
    CHECK(ok, "gli avvisi non devono far fallire il caricamento");
    CHECK(config_report_error_count(&rep) == 0, "nessun errore atteso");
    CHECK(report_has(&rep, "non esiste"), "branch inesistente non segnalato");
    CHECK(ll_contains(&cfg, "main"), "main doveva restare long-lived");
    CHECK(override_parent(&cfg, "feature/x"), "override mantenuto nonostante l'avviso");
    config_report_free(&rep);
    config_free(&cfg);
}

/* Ciclo di override: avviso (in fase di modello i nodi diventano ambigui). */
static void test_cycle_warning(void)
{
    GraphConfig cfg;
    ConfigReport rep;
    const char *text =
        "[parents]\n"
        "a = b\n"
        "b = c\n"
        "c = a\n";
    bool ok = load_text("ciclo.cfg", text, &cfg, &rep);
    CHECK(ok, "un ciclo non deve rifiutare il file");
    CHECK(report_has(&rep, "ciclo"), "ciclo non segnalato");
    CHECK(config_report_error_count(&rep) == 0, "il ciclo e' un avviso");
    CHECK(cfg.override_count == 3, "i tre override devono restare");
    config_report_free(&rep);
    config_free(&cfg);
}

/* ---------- end-to-end con il repository di prova ---------- */

static const BranchNode *branch_named(const History *h, const char *name)
{
    size_t i = history_find_branch(h, name);
    return i == (size_t)-1 ? NULL : &h->branches[i];
}

/* Scrive graph.cfg nel repository di prova, costruisce il modello, controlla
 * e rimuove il file. */
static void test_end_to_end(void)
{
    char *cfg_path = config_path_in_repo(g_repo);
    REQUIRE(cfg_path != NULL, "config_path_in_repo fallito");
    const char *text =
        "# long-lived esplicito: niente tronco implicito, ordine per gerarchia\n"
        "[long-lived]\n"
        "main\n"
        "\n"
        "[parents]\n"
        "feature/x = branch1\n";
    FILE *f = fopen(cfg_path, "wb");
    REQUIRE(f != NULL, "scrittura di graph.cfg nel repo di prova fallita");
    fwrite(text, 1, strlen(text), f);
    fclose(f);

    GitData data;
    char err[1024];
    bool loaded = gitdata_load(&data, g_repo, err, sizeof err);
    REQUIRE(loaded, "caricamento del repo di prova fallito: %s", err);

    char **names = xmalloc((data.ref_count ? data.ref_count : 1) * sizeof(char *));
    size_t n = 0;
    for (size_t i = 0; i < data.ref_count; i++) {
        if (data.refs[i].kind != REF_TAG)
            names[n++] = data.refs[i].name;
    }
    GraphConfig cfg;
    ConfigReport rep;
    bool cfg_ok = config_load(&cfg, cfg_path, names, n, &rep, err, sizeof err);
    CHECK(cfg_ok, "graph.cfg del repo di prova non valido");
    CHECK(config_report_error_count(&rep) == 0, "nessun errore atteso sul repo di prova");

    History hist;
    REQUIRE(history_build(&hist, &data, &cfg, err, sizeof err),
            "history_build con graph.cfg fallita");

    /* override: stato 'configurato', padre dichiarato */
    const BranchNode *fx = branch_named(&hist, "feature/x");
    REQUIRE(fx != NULL, "feature/x assente");
    CHECK(fx->state == PARENT_CONFIGURED, "override: stato %d", (int)fx->state);
    CHECK(fx->parent && strcmp(fx->parent, "branch1") == 0,
          "override: padre %s", fx->parent ? fx->parent : "-");

    /* long-lived: nessun padre, nessun fork, nessun candidato */
    const BranchNode *main_b = branch_named(&hist, "main");
    REQUIRE(main_b != NULL, "main assente");
    CHECK(main_b->state == PARENT_NONE && main_b->parent == NULL,
          "main: nessun padre");
    CHECK(main_b->fork_hash == NULL && main_b->candidate_count == 0,
          "main: nessun fork ne' candidati");

    /* il resto del modello non cambia */
    const BranchNode *b1 = branch_named(&hist, "branch1");
    REQUIRE(b1 != NULL, "branch1 assente");
    CHECK(b1->state == PARENT_DEDUCED && b1->parent &&
              strcmp(b1->parent, "main") == 0,
          "branch1: padre dedotto main atteso");
    const BranchNode *b2 = branch_named(&hist, "branch2");
    REQUIRE(b2 != NULL, "branch2 assente");
    CHECK(b2->state == PARENT_DEDUCED && b2->parent &&
              strcmp(b2->parent, "branch1") == 0,
          "branch2: padre dedotto branch1 atteso");

    /* long-lived dichiarato col nome remoto vale come il branch locale */
    history_free(&hist);
    config_report_free(&rep);
    config_free(&cfg);
    remove(cfg_path);
    const char *remote_text = "[long-lived]\norigin/main\n";
    f = fopen(cfg_path, "wb");
    fwrite(remote_text, 1, strlen(remote_text), f);
    fclose(f);
    REQUIRE(config_load(&cfg, cfg_path, names, n, &rep, err, sizeof err),
            "config_load con long-lived remoto fallita");
    CHECK(config_long_lived_rank(&cfg, "origin/main") == 0,
          "origin/main doveva essere long-lived di rango 0");
    REQUIRE(history_build(&hist, &data, &cfg, err, sizeof err),
            "history_build con long-lived remoto fallita");
    main_b = branch_named(&hist, "main");
    CHECK(main_b && main_b->is_long_lived,
          "main doveva essere long-lived tramite origin/main");
    history_free(&hist);

    remove(cfg_path);
    config_report_free(&rep);
    config_free(&cfg);
    free(names);
    gitdata_free(&data);
    free(cfg_path);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 3) {
        fprintf(stderr, "uso: test_config <directory-di-lavoro> <percorso-repo>\n");
        return 2;
    }
    snprintf(g_work, sizeof g_work, "%s", argv[1]);
    g_repo = argv[2];

    test_parse_and_wildcards();
    test_errors_reject_file();
    test_warnings_keep_file();
    test_cycle_warning();
    test_end_to_end();

    printf("%d test superati, %d falliti\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}

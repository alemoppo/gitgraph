/* test_config.c - test automatici del parsing di graph.cfg (senza finestra).
 *
 * Uso: test_config <directory-di-lavoro> <percorso-repo-di-test>
 *
 * <directory-di-lavoro> serve solo per i file di prova che il test scrive.
 *
 * Copre: sezioni e commenti (solo su righe dedicate), wildcard in [long-lived]
 * (espansione, ordine naturale, priorita' della prima riga), tronco implicito,
 * override in [parents], validazione dei nomi di riferimento (comprese le
 * lunghezze attorno al suffisso ".lock"), errori che rifiutano il file, file
 * assente/illeggibile/vuoto, avvisi che lo lasciano valido, cicli di override
 * (nessuno, due nodi, catena lunga, cicli distinti) e uso end-to-end con
 * graph.cfg dentro il repository di prova (override -> stato 'configurato',
 * long-lived senza fork ne' candidati).
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

/* proprieta' condivisa: serve la macro CHECK definita sopra */
#include "gg_checks.h"

static char g_work[512];
static const char *g_repo = NULL;

/* Riferimenti finti del repository: nome e nome canonico (per i remoti il
 * canonico e' quello senza il prefisso). */
static ConfigBranch g_fake[] = {
    { "main", "main" },
    { "release/1.0", "release/1.0" },
    { "release/2.0", "release/2.0" },
    { "release/10.0", "release/10.0" },
    { "release/2026/q1", "release/2026/q1" },
    { "release/hotfix-3", "release/hotfix-3" },
    { "origin/main", "main" },
    { "origin/release/2.0", "release/2.0" },
    { "feature/x", "feature/x" },
    { "bugfix/1", "bugfix/1" },
    /* nomi con caratteri che la grammatica deve non toccare */
    { "feat#1", "feat#1" },
    { "feat;x", "feat;x" },
    /* nomi brevi, per la validazione dei riferimenti */
    { "a", "a" },
    { "abc", "abc" },
    { "abcd", "abcd" },
    { "abcde", "abcde" },
    { "abcdef", "abcdef" },
    { "catena/x", "catena/x" },
    { "catena/y", "catena/y" },
    { "catena/z", "catena/z" },
    { "catena/w", "catena/w" },
};
#define FAKE_COUNT (sizeof(g_fake) / sizeof(g_fake[0]))

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
    return config_load(cfg, path, g_fake, FAKE_COUNT, report, err, sizeof err);
}

/* Crea una directory vuota (per provare il caso "il percorso esiste ma non e'
 * un file leggibile"). */
static bool make_dir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0777) == 0;
#endif
}

static void remove_dir(const char *path)
{
#ifdef _WIN32
    RemoveDirectoryA(path);
#else
    rmdir(path);
#endif
}

static bool report_has(const ConfigReport *r, const char *needle)
{
    for (size_t i = 0; i < r->count; i++) {
        if (strstr(r->items[i].text, needle) != NULL)
            return true;
    }
    return false;
}

/* Quante volte compare `needle` nella diagnostica (serve a verificare che un
 * ciclo venga segnalato una volta sola). */
static size_t report_count(const ConfigReport *r, const char *needle)
{
    size_t n = 0;
    for (size_t i = 0; i < r->count; i++)
        if (strstr(r->items[i].text, needle) != NULL)
            n++;
    return n;
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
        bool ok = config_load(&cfg, "percorso/inesistente.cfg", g_fake,
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
        /* la barra non e' un separatore: release/* arriva anche a
         * release/2026/q1 e release/hotfix-3 */
        CHECK(cfg.long_lived_count == 6, "attesi 6 long-lived, trovati %zu",
              cfg.long_lived_count);
        CHECK(ll_contains(&cfg, "main") && ll_contains(&cfg, "release/1.0") &&
                  ll_contains(&cfg, "release/2.0"),
              "espansione del pattern release/* incompleta");
        CHECK(ll_contains(&cfg, "release/2026/q1") &&
                  ll_contains(&cfg, "release/hotfix-3"),
              "il wildcard deve attraversare '/'");
        /* ordine naturale: 1.0, 2.0, 10.0 (2.0 prima di 10.0) */
        CHECK(config_long_lived_rank(&cfg, "release/1.0") == 1 &&
                  config_long_lived_rank(&cfg, "release/2.0") == 2 &&
                  config_long_lived_rank(&cfg, "release/10.0") == 3,
              "ordine naturale dei numeri non rispettato");
        CHECK(config_long_lived_rank(&cfg, "main") == 0, "main deve avere rank 0");
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
        /* release/* raccoglie i cinque rami release; la riga release/1.0 non
         * sposta nulla (valge la prima) e main chiude la lista */
        CHECK(cfg.long_lived_count == 6 && strcmp(cfg.long_lived[0], "release/1.0") == 0,
              "l'ordine della lista deve essere rispettato, trovati %zu",
              cfg.long_lived_count);
        CHECK(strcmp(cfg.long_lived[5], "main") == 0,
              "main deve restare nell'ordine dichiarato");
        CHECK(report_has(&rep, "gia' long-lived"), "duplicato non segnalato");
        CHECK(config_report_error_count(&rep) == 0, "il duplicato e' un avviso");
        config_report_free(&rep);
        config_free(&cfg);
    }

    /* [long-lived] presente ma senza corrispondenze: niente tronco implicito */
    {
        GraphConfig cfg;
        ConfigReport rep;
        const char *text = "[long-lived]\nniente/qui\n";
        bool ok = load_text("vuoto.cfg", text, &cfg, &rep);
        CHECK(ok, " caricamento fallito");
        CHECK(cfg.long_lived_count == 0, "lista vuota attesa");
        CHECK(cfg.long_lived_explicit, "la sezione esplicita deve contare");
        CHECK(report_has(&rep, "non esiste"),
              "un nome senza corrispondenze deve essere segnalato");
        config_report_free(&rep);
        config_free(&cfg);
    }
}

/* Regole di espansione: ordine naturale, priorita' della prima riga,
 * riferimenti remoti contati per nome canonico, barra attraversata. */
static void test_wildcard_rules(void)
{
    /* ordine naturale: 1.0, 2.0, 10.0 (non 1.0, 10.0, 2.0) */
    {
        GraphConfig cfg;
        ConfigReport rep;
        const char *text =
            "[long-lived]\n"
            "release/*\n";
        bool ok = load_text("naturale.cfg", text, &cfg, &rep);
        CHECK(ok, "caricamento fallito");
        /* il pattern attraversa anche '/' e raccoglie tutto il prefisso */
        const char *attesi[] = { "release/1.0", "release/2.0", "release/10.0",
                                 "release/2026/q1", "release/hotfix-3" };
        size_t attesi_count = sizeof attesi / sizeof attesi[0];
        CHECK(cfg.long_lived_count == attesi_count, "attesi %zu long-lived, trovati %zu",
              attesi_count, cfg.long_lived_count);
        for (size_t i = 0; i < attesi_count && i < cfg.long_lived_count; i++)
            CHECK(strcmp(cfg.long_lived[i], attesi[i]) == 0,
                  "posizione %zu: atteso '%s', trovato '%s'", i, attesi[i],
                  cfg.long_lived[i]);
        config_report_free(&rep);
        config_free(&cfg);
    }

    /* la barra non e' speciale: il pattern attraversa anche le sottocartelle */
    {
        GraphConfig cfg;
        ConfigReport rep;
        const char *text =
            "[long-lived]\n"
            "release/*\n";
        bool ok = load_text("barra.cfg", text, &cfg, &rep);
        CHECK(ok, "caricamento fallito");
        CHECK(ll_contains(&cfg, "release/2026/q1"),
              "il wildcard deve attraversare '/' (release/2026/q1)");
        config_report_free(&rep);
        config_free(&cfg);
    }

    /* priorita': fra piu' righe che corrispondono vale la prima */
    {
        GraphConfig cfg;
        ConfigReport rep;
        const char *text =
            "[long-lived]\n"
            "release/hotfix-*\n"
            "release/*\n";
        bool ok = load_text("priorita.cfg", text, &cfg, &rep);
        CHECK(ok, "caricamento fallito");
        /* release/hotfix-3 viene dalla prima riga, quindi mantiene quella
         * posizione; release/2.0 e gli altri vengono dalla seconda */
        CHECK(ll_contains(&cfg, "release/hotfix-3"), "hotfix-3 assente");
        CHECK(cfg.long_lived_count > 0 &&
                  strcmp(cfg.long_lived[0], "release/hotfix-3") == 0,
              "la prima riga che corrisponde deve valere");
        CHECK(report_has(&rep, "gia' long-lived"),
              "la riga successiva non deve spostare il branch");
        config_report_free(&rep);
        config_free(&cfg);
    }

    /* riferimenti remoti: origin/release/2.0 conta come release/2.0 */
    {
        GraphConfig cfg;
        ConfigReport rep;
        const char *text =
            "[long-lived]\n"
            "release/2.0\n";
        bool ok = load_text("remoto.cfg", text, &cfg, &rep);
        CHECK(ok, "caricamento fallito");
        CHECK(cfg.long_lived_count == 1 &&
                  strcmp(cfg.long_lived[0], "release/2.0") == 0,
              "il riferimento remoto deve contare per nome canonico");
        CHECK(config_report_error_count(&rep) == 0,
              "riconoscere un remoto non e' un errore");
        config_report_free(&rep);
        config_free(&cfg);
    }

    /* tronco implicite fra i nomi canonici: basta origin/main */
    {
        GraphConfig cfg;
        ConfigReport rep;
        ConfigBranch solo_remoto[] = { { "origin/main", "main" } };
        char err[512];
        CHECK(config_load(&cfg, "inesistente.cfg", solo_remoto, 1, &rep, err,
                          sizeof err),
              "caricamento con solo remoto fallito");
        CHECK(cfg.long_lived_count == 1 && strcmp(cfg.long_lived[0], "main") == 0,
              "il tronco implicito deve considerare i nomi canonici");
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
        bool ok = config_load(&cfg, path, g_fake, FAKE_COUNT, &rep, err,
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

/* ---------- validazione dei nomi di riferimento ---------- */

/* Il confronto del suffisso ".lock" non deve leggere fuori dal buffer sui nomi
 * brevi: si esercitano le lunghezze attorno a quella del suffisso (1, 3, 4, 5,
 * 6 caratteri) e i casi con e senza ".lock". */
static void test_refname_lengths(void)
{
    /* validi: nessuno dei casi deve essere respinto come nome non valido */
    CHECK(config_refname_syntax_ok("a"), "nome di 1 carattere: valido");
    CHECK(config_refname_syntax_ok("abc"), "nome di 3 caratteri: valido");
    CHECK(config_refname_syntax_ok("abcd"), "nome di 4 caratteri: valido");
    CHECK(config_refname_syntax_ok("abcde"), "nome di 5 caratteri: valido");
    CHECK(config_refname_syntax_ok("abcdef"), "nome di 6 caratteri: valido");
    CHECK(config_refname_syntax_ok("lock"), "'lock' senza punto: valido");
    CHECK(config_refname_syntax_ok("x.locked"), "suffisso simile a .lock: valido");
    CHECK(config_refname_syntax_ok("olock"), "'lock' iniziale: valido");
    CHECK(config_refname_syntax_ok("release/1.0"), "nome con barra: valido");

    /* non validi */
    CHECK(!config_refname_syntax_ok(""), "nome vuoto: non valido");
    CHECK(!config_refname_syntax_ok("@"), "'@': non valido");
    CHECK(!config_refname_syntax_ok(".lock"), "solo '.lock': non valido (5 caratteri)");
    CHECK(!config_refname_syntax_ok("a.lock"), "'a.lock': non valido (6 caratteri)");
    CHECK(!config_refname_syntax_ok("abcde.lock"), "suffisso .lock: non valido");
    CHECK(!config_refname_syntax_ok("release/1.0.lock"), ".lock dopo barra: non valido");
    CHECK(!config_refname_syntax_ok(".abcd"), "iniziale con punto: non valido");
    CHECK(!config_refname_syntax_ok("abc."), "finale con punto: non valido");
    CHECK(!config_refname_syntax_ok("/abc"), "iniziale con barra: non valido");
    CHECK(!config_refname_syntax_ok("abc/"), "finale con barra: non valido");
    CHECK(!config_refname_syntax_ok("a b"), "spazio: non valido");
    CHECK(!config_refname_syntax_ok("a..b"), "doppio punto: non valido");
    CHECK(!config_refname_syntax_ok("a//b"), "doppia barra: non valido");
    CHECK(!config_refname_syntax_ok("a~b"), "tilde: non valido");
    CHECK(!config_refname_syntax_ok("a:b"), "due punti: non valido");
    CHECK(!config_refname_syntax_ok("a[b"), "parentesi: non valido");
    CHECK(!config_refname_syntax_ok("a@{0}"), "sequenza @{: non valido");
    CHECK(!config_refname_syntax_ok("a\\b"), "barra rovescia: non valido");

    /* gli stessi nomi brevi passano dal percorso completo del caricamento:
     * devono risultare "non esistenti" (sintassi valida), non "non validi" */
    {
        GraphConfig cfg;
        ConfigReport rep;
        const char *text =
            "[long-lived]\n"
            "a\n"
            "abc\n"
            "abcd\n"
            "abcde\n"
            "abcdef\n";
        bool ok = load_text("brevi.cfg", text, &cfg, &rep);
        CHECK(ok, "nomi brevi: il file deve restare valido");
        CHECK(!report_has(&rep, "non e' un nome di riferimento valido"),
              "nomi brevi: nessuno deve essere respinto come sintassi");
        CHECK(cfg.long_lived_count == 5, "nomi brevi: tutti long-lived");
        config_report_free(&rep);
        config_free(&cfg);
    }
    /* "a.lock" e' invece un nome non valido: la diagnostica deve dirlo */
    {
        GraphConfig cfg;
        ConfigReport rep;
        bool ok = load_text("lock.cfg", "[long-lived]\na.lock\n", &cfg, &rep);
        CHECK(ok, "un nome non valido e' un avviso, non un errore");
        CHECK(report_has(&rep, "non e' un nome di riferimento valido"),
              "il suffisso .lock deve essere respinto come nome non valido");
        CHECK(cfg.long_lived_count == 0, "nessun long-lived da un nome non valido");
        config_report_free(&rep);
        config_free(&cfg);
    }
}

/* ---------- file assente, illeggibile, vuoto ---------- */

static void test_load_failures(void)
{
    /* 1. file assente: nessun errore, nessun messaggio, solo il tronco implicito */
    {
        GraphConfig cfg;
        ConfigReport rep;
        char err[512];
        CHECK(config_load(&cfg, "percorso/inesistente.cfg", g_fake, FAKE_COUNT, &rep,
                          err, sizeof err),
              "file assente: deve riuscire");
        CHECK(rep.count == 0, "file assente: nessun messaggio");
        CHECK(config_long_lived_rank(&cfg, "main") == 0,
              "file assente: tronco implicito main");
        config_report_free(&rep);
        config_free(&cfg);
    }

    /* 2. percorso non valido (directory inesistente come file): ENOENT resta
     *    non errore, perche' il file non esiste nemmeno come directory */
    {
        GraphConfig cfg;
        ConfigReport rep;
        char err[512];
        char path[768];
        snprintf(path, sizeof path, "%s/nessuna_directory/graph.cfg", g_work);
        CHECK(config_load(&cfg, path, g_fake, FAKE_COUNT, &rep, err, sizeof err),
              "directory inesistente: nessun file da leggere");
        CHECK(rep.count == 0, "directory inesistente: nessun messaggio");
        config_report_free(&rep);
        config_free(&cfg);
    }

    /* 3. il percorso esiste ma e' una directory: file presente e non leggibile
     *    => errore esplicito, non un silenzio come per un file assente */
    {
        GraphConfig cfg;
        ConfigReport rep;
        char err[512];
        char dir[768];
        snprintf(dir, sizeof dir, "%s/directory.cfg", g_work);
        REQUIRE(make_dir(dir), "creazione della directory di prova fallita");
        bool ok = config_load(&cfg, dir, g_fake, FAKE_COUNT, &rep, err, sizeof err);
        CHECK(!ok, "una directory al posto del file deve fallire");
        CHECK(rep.count > 0, "una directory non leggibile deve produrre una diagnostica");
        CHECK(report_has(&rep, "impossibile aprire") || report_has(&rep, "lettura"),
              "la diagnostica deve distinguere apertura/lettura dal file assente");
        CHECK(config_report_error_count(&rep) == 1,
              "il rifiuto per directory deve produrre un solo errore");
        CHECK(err[0] != '\0', "il messaggio sintetico deve essere compilato");
        /* rifiuto completo: resta solo il tronco implicito */
        CHECK(cfg.long_lived_count == 1 && config_long_lived_rank(&cfg, "main") == 0,
              "file rifiutato: solo il tronco implicito");
        config_report_free(&rep);
        config_free(&cfg);
        remove_dir(dir);
    }

    /* 4. file vuoto: valido, nessun messaggio, tronco implicito */
    {
        GraphConfig cfg;
        ConfigReport rep;
        bool ok = load_text("vuoto_vuoto.cfg", "", &cfg, &rep);
        CHECK(ok, "file vuoto: deve restare valido");
        CHECK(rep.count == 0, "file vuoto: nessun messaggio");
        CHECK(config_long_lived_rank(&cfg, "main") == 0, "file vuoto: tronco implicito");
        config_report_free(&rep);
        config_free(&cfg);
    }

    /* 5. solo commenti: valido, nessun messaggio, tronco implicito */
    {
        GraphConfig cfg;
        ConfigReport rep;
        const char *text =
            "# solo commenti\n"
            "; anche con punto e virgola\n"
            "\n"
            "   \n"
            "# [long-lived] dentro un commento non vale come sezione\n";
        bool ok = load_text("solo_commenti.cfg", text, &cfg, &rep);
        CHECK(ok, "solo commenti: deve restare valido");
        CHECK(rep.count == 0, "solo commenti: nessun messaggio");
        CHECK(config_long_lived_rank(&cfg, "main") == 0,
              "solo commenti: tronco implicito");
        config_report_free(&rep);
        config_free(&cfg);
    }
}

/* ---------- commenti solo su righe dedicate ---------- */

/* La sintassi scelta e' "commenti su righe dedicate": un '#' o un ';' dentro
 * una riga di dati fa parte del valore e non tronca nulla. */
static void test_comment_syntax(void)
{
    /* '#' dentro un nome: il nome resta intero e viene applicato */
    {
        GraphConfig cfg;
        ConfigReport rep;
        const char *text =
            "[parents]\n"
            "feat#1 = main\n";
        bool ok = load_text("hash.cfg", text, &cfg, &rep);
        CHECK(ok, "'#' dentro un nome non deve rifiutare il file");
        CHECK(config_report_error_count(&rep) == 0, "'#' nel nome: nessun errore");
        CHECK(cfg.override_count == 1, "'#' nel nome: l'override deve restare");
        CHECK(override_parent(&cfg, "feat#1") != NULL,
              "'#' nel nome: l'override deve valere, non essere troncato");
        config_report_free(&rep);
        config_free(&cfg);
    }
    /* ';' dentro un nome: stessa cosa */
    {
        GraphConfig cfg;
        ConfigReport rep;
        const char *text =
            "[parents]\n"
            "feat;x = main\n";
        bool ok = load_text("punto_virgola.cfg", text, &cfg, &rep);
        CHECK(ok, "';' dentro un nome non deve rifiutare il file");
        CHECK(override_parent(&cfg, "feat;x") != NULL,
              "';' nel nome: l'override deve valere");
        config_report_free(&rep);
        config_free(&cfg);
    }
    /* commento in coda a un nome [long-lived]: non e' un commento, quindi il
     * valore ("main # trunk") e' un nome non valido e viene segnalato, senza
     * essere troncato a "main" */
    {
        GraphConfig cfg;
        ConfigReport rep;
        const char *text =
            "[long-lived]\n"
            "main # il tronco\n";
        bool ok = load_text("inline_ll.cfg", text, &cfg, &rep);
        CHECK(ok, "un nome con spazi e' un avviso, non un errore");
        CHECK(report_has(&rep, "non e' un nome di riferimento valido"),
              "un commento in coda deve essere segnalato come nome non valido");
        CHECK(!ll_contains(&cfg, "main"),
              "il valore non deve essere troncato a 'main'");
        config_report_free(&rep);
        config_free(&cfg);
    }
    /* commento in coda a un override: errore esplicito */
    {
        GraphConfig cfg;
        ConfigReport rep;
        const char *text =
            "[parents]\n"
            "feature/x = main # override\n";
        bool ok = load_text("inline_parents.cfg", text, &cfg, &rep);
        CHECK(!ok, "un padre con spazi deve rifiutare il file");
        CHECK(config_report_error_count(&rep) == 1, "un solo errore atteso");
        CHECK(cfg.override_count == 0, "l'override non deve essere applicato");
        config_report_free(&rep);
        config_free(&cfg);
    }
    /* commenti su righe dedicate: restano il modo supportato */
    {
        GraphConfig cfg;
        ConfigReport rep;
        const char *text =
            "# questo e' un commento\n"
            "[long-lived]\n"
            "main\n"
            "; anche questo\n"
            "release/*\n";
        bool ok = load_text("dedicated.cfg", text, &cfg, &rep);
        CHECK(ok, "commenti su righe dedicate: il file deve restare valido");
        CHECK(rep.count == 0, "commenti su righe dedicate: nessun messaggio");
        CHECK(config_long_lived_rank(&cfg, "main") == 0, "main long-lived");
        config_report_free(&rep);
        config_free(&cfg);
    }
}

/* ---------- cicli di override ---------- */

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
    /* ogni ciclo viene segnalato una volta sola */
    CHECK(report_count(&rep, "ciclo di override") == 1,
          "il ciclo deve essere segnalato una volta sola, trovati %zu messaggi",
          report_count(&rep, "ciclo di override"));
    config_report_free(&rep);
    config_free(&cfg);
}

/* Nessun ciclo: nessun avviso spurio. */
static void test_cycle_no_false_positive(void)
{
    GraphConfig cfg;
    ConfigReport rep;
    const char *text =
        "[parents]\n"
        "feature/x = main\n"
        "bugfix/1 = feature/x\n"
        "release/1.0 = main\n";
    bool ok = load_text("no_ciclo.cfg", text, &cfg, &rep);
    CHECK(ok, "override senza cicli: il file deve restare valido");
    CHECK(!report_has(&rep, "ciclo di override"), "override senza cicli: nessun avviso di ciclo");
    CHECK(cfg.override_count == 3, "i tre override devono restare");
    config_report_free(&rep);
    config_free(&cfg);
}

/* Ciclo di due nodi: segnalato una volta. */
static void test_cycle_two_nodes(void)
{
    GraphConfig cfg;
    ConfigReport rep;
    const char *text =
        "[parents]\n"
        "a = b\n"
        "b = a\n";
    bool ok = load_text("ciclo2.cfg", text, &cfg, &rep);
    CHECK(ok, "un ciclo di due nodi non deve rifiutare il file");
    CHECK(report_count(&rep, "ciclo di override") == 1, "ciclo di due nodi: un solo avviso");
    CHECK(config_report_error_count(&rep) == 0, "il ciclo e' un avviso");
    config_report_free(&rep);
    config_free(&cfg);
}

/* Catena lunga con ciclo in fondo: il ciclo e' segnalato una volta sola e i
 * nodi precedenti non generano avvisi duplicati. */
static void test_cycle_long_chain(void)
{
    GraphConfig cfg;
    ConfigReport rep;
    StrBuf text;
    sb_init(&text);
    sb_append(&text, "[parents]\n");
    /* catena di 40 nodi che richiude su se stessa */
    for (int i = 0; i < 40; i++)
        sb_appendf(&text, "catena/n%d = catena/n%d\n", i, i + 1);
    sb_appendf(&text, "catena/n40 = catena/n7\n");
    char path[768];
    snprintf(path, sizeof path, "%s/catena_lunga.cfg", g_work);
    FILE *f = fopen(path, "wb");
    REQUIRE(f != NULL, "scrittura del file di prova fallita");
    fwrite(text.data, 1, text.len, f);
    fclose(f);
    char err[512];
    bool ok = config_load(&cfg, path, g_fake, FAKE_COUNT, &rep, err, sizeof err);
    sb_free(&text);
    CHECK(ok, "catena con ciclo: il file deve restare valido");
    CHECK(report_count(&rep, "ciclo di override") == 1,
          "catena lunga: un solo avviso di ciclo, trovati %zu", report_count(&rep, "ciclo di override"));
    CHECK(cfg.override_count == 41, "tutti i 41 override devono restare");
    config_report_free(&rep);
    config_free(&cfg);
}

/* Due cicli distinti: due avvisi, uno per ciclo, senza duplicati. */
static void test_cycle_multiple(void)
{
    GraphConfig cfg;
    ConfigReport rep;
    const char *text =
        "[parents]\n"
        "a = b\n"
        "b = a\n"
        "catena/x = catena/y\n"
        "catena/y = catena/z\n"
        "catena/z = catena/x\n"
        "catena/w = main\n";
    bool ok = load_text("cicli_multipli.cfg", text, &cfg, &rep);
    CHECK(ok, "cicli distinti: il file deve restare valido");
    CHECK(report_count(&rep, "ciclo di override") == 2,
          "due cicli distinti: due avvisi, trovati %zu", report_count(&rep, "ciclo di override"));
    /* il ramo che non chiude non deve generare avvisi */
    CHECK(!report_has(&rep, "catena/w"), "un ramo aperto non e' un ciclo");
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

    ConfigBranch *branches =
        xmalloc((data.ref_count ? data.ref_count : 1) * sizeof(ConfigBranch));
    size_t n = 0;
    for (size_t i = 0; i < data.ref_count; i++) {
        if (data.refs[i].kind == REF_TAG)
            continue;
        const char *nm = data.refs[i].name;
        const char *canon = nm;
        if (data.refs[i].kind == REF_REMOTE_BRANCH) {
            const char *slash = strchr(nm, '/');
            if (slash && slash[1])
                canon = slash + 1;
        }
        branches[n].name = nm;
        branches[n].canonical = canon;
        n++;
    }
    GraphConfig cfg;
    ConfigReport rep;
    bool cfg_ok = config_load(&cfg, cfg_path, branches, n, &rep, err, sizeof err);
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

    /* branch1, senza l'ordine di nascita, e' ambiguo con branch2 (fratelli
     * sul commit B2) e non ha piu' main come padre dedotto: main resta pero'
     * fra i candidati come primo antenato non fratello sotto il punto (branch1
     * e' il piu' antico della diramazione) e diventa il padre suggerito. Il
     * resto del modello non cambia: branch2 resta ambiguo con suggerito per
     * data branch1, come nel test core */
    const BranchNode *b1 = branch_named(&hist, "branch1");
    REQUIRE(b1 != NULL, "branch1 assente");
    bool b1_c_b2 = false, b1_c_main = false;
    for (size_t k = 0; k < b1->candidate_count && b1->candidates; k++) {
        if (strcmp(b1->candidates[k], "branch2") == 0)
            b1_c_b2 = true;
        if (strcmp(b1->candidates[k], "main") == 0)
            b1_c_main = true;
    }
    CHECK(b1->state == PARENT_AMBIGUOUS && b1->parent == NULL &&
              b1->candidate_count == 2 && b1_c_b2 && b1_c_main,
          "branch1: atteso ambiguo con branch2 e main in lizza (padre %s, "
          "candidati %zu)",
          b1->parent ? b1->parent : "-", b1->candidate_count);
    CHECK(b1->suggested_parent && strcmp(b1->suggested_parent, "main") == 0 &&
              b1->suggestion_basis == SUGGESTION_ANCESTOR,
          "branch1: suggerito atteso main (antenato), trovato %s (base %d)",
          b1->suggested_parent ? b1->suggested_parent : "-",
          (int)b1->suggestion_basis);
    /* invariante: ogni ramo con un tronco fra gli antenati ha un padre o un
     * padre suggerito che arriva al tronco */
    gg_check_trunk_ancestry(&hist, &data, "end-to-end con graph.cfg");
    const BranchNode *b2 = branch_named(&hist, "branch2");
    REQUIRE(b2 != NULL, "branch2 assente");
    /* branch2 e' nato da B2, che sta sulla catena di branch1 e anche su quella
     * di sp: nessun padre e' dedotto, i candidati sono elencati e il
     * suggerimento (per data) e' branch1. [parents] dichiara il padre vero. */
    CHECK(b2->state == PARENT_AMBIGUOUS && b2->parent == NULL &&
              b2->suggested_parent && strcmp(b2->suggested_parent, "branch1") == 0,
          "branch2: ambiguo con suggerimento %s",
          b2->suggested_parent ? b2->suggested_parent : "-");

    /* long-lived dichiarato col nome remoto vale come il branch locale */
    history_free(&hist);
    config_report_free(&rep);
    config_free(&cfg);
    remove(cfg_path);
    /* long-lived dichiarato col nome remoto vale come il branch locale: la
     * lista contiene il nome canonico, quindi "origin/main" e "main" sono lo
     * stesso ramo */
    const char *remote_text = "[long-lived]\norigin/main\n";
    f = fopen(cfg_path, "wb");
    fwrite(remote_text, 1, strlen(remote_text), f);
    fclose(f);
    REQUIRE(config_load(&cfg, cfg_path, branches, n, &rep, err, sizeof err),
            "config_load con long-lived remoto fallita");
    CHECK(cfg.long_lived_count == 1 && strcmp(cfg.long_lived[0], "main") == 0,
          "il riferimento remoto deve valere come nome canonico");
    CHECK(config_long_lived_rank(&cfg, "main") == 0, "main deve avere rango 0");
    REQUIRE(history_build(&hist, &data, &cfg, err, sizeof err),
            "history_build con long-lived remoto fallita");
    main_b = branch_named(&hist, "main");
    CHECK(main_b && main_b->is_long_lived,
          "main doveva essere long-lived tramite origin/main");
    const BranchNode *om3 = branch_named(&hist, "origin/main");
    CHECK(om3 && !om3->is_long_lived,
          "origin/main staccato: nessuna relazione di tracking dai soli nomi");
    CHECK(om3 && om3->parent && strcmp(om3->parent, "main") == 0,
          "origin/main: genealogia dedotta verso main");
    /* uno specchio invece eredita il ruolo di tronco dal ramo che rispecchia */
    history_free(&hist);

    remove(cfg_path);
    config_report_free(&rep);
    config_free(&cfg);
    free(branches);
    gitdata_free(&data);
    free(cfg_path);
}

/* Una motivazione non deve mai cominciare con "; ": il separatore non
 * separa niente e la riga si legge come se ci fosse un testo prima.
 *
 * Il caso e' reale: con "[long-lived] ff" la famiglia di ff diventa tronco,
 * quindi deduce non viene chiamata e la motivazione resta vuota; upstream/main
 * e' nella stessa famiglia e riceve solo la nota sugli alias. */
static void test_motivation_separator(void)
{
    char *cfg_path = config_path_in_repo(g_repo);
    REQUIRE(cfg_path != NULL, "config_path_in_repo fallito");
    const char *text = "[long-lived]\nff\nmain\n";
    FILE *f = fopen(cfg_path, "wb");
    REQUIRE(f != NULL, "scrittura di graph.cfg nel repo di prova fallita");
    fwrite(text, 1, strlen(text), f);
    fclose(f);

    GitData data;
    char err[1024];
    REQUIRE(gitdata_load(&data, g_repo, err, sizeof err),
            "caricamento del repo di prova fallito: %s", err);

    ConfigBranch *branches =
        xmalloc((data.ref_count ? data.ref_count : 1) * sizeof(ConfigBranch));
    size_t n = 0;
    for (size_t i = 0; i < data.ref_count; i++) {
        if (data.refs[i].kind == REF_TAG)
            continue;
        const char *nm = data.refs[i].name;
        const char *canon = nm;
        if (data.refs[i].kind == REF_REMOTE_BRANCH) {
            const char *slash = strchr(nm, '/');
            if (slash && slash[1])
                canon = slash + 1;
        }
        branches[n].name = nm;
        branches[n].canonical = canon;
        n++;
    }
    GraphConfig cfg;
    ConfigReport rep;
    REQUIRE(config_load(&cfg, cfg_path, branches, n, &rep, err, sizeof err),
            "config_load di graph.cfg fallita: %s", err);

    History hist;
    REQUIRE(history_build(&hist, &data, &cfg, err, sizeof err),
            "history_build con graph.cfg fallita");

    /* nessuna motivazione comincia con "; " (ne' contiene "; ; ") */
    for (size_t i = 0; i < hist.branch_count; i++) {
        const char *mot = hist.branches[i].motivation;
        CHECK(mot != NULL, "%s: motivazione assente", hist.branches[i].name);
        if (!mot)
            continue;
        CHECK(mot[0] != ';', "%s: la motivazione comincia con '; '",
              hist.branches[i].name);
        CHECK(strstr(mot, "; ;") == NULL,
              "%s: separatore ripetuto nella motivazione: %s", hist.branches[i].name,
              mot);
    }

    /* il caso concreto: la nota sugli alias e' l'unico testo, quindi non ha
     * bisogno di separatore */
    const BranchNode *um = branch_named(&hist, "upstream/main");
    REQUIRE(um != NULL, "upstream/main assente");
    const char *atteso =
        "stessa punta di 'ff' (stesso padre dedotto; il nome interno e' ambiguo)";
    CHECK(um->motivation && strcmp(um->motivation, atteso) == 0,
          "upstream/main: motivazione attesa \"%s\", trovata \"%s\"", atteso,
          um->motivation ? um->motivation : "-");

    history_free(&hist);
    config_report_free(&rep);
    config_free(&cfg);
    free(branches);
    gitdata_free(&data);
    remove(cfg_path);
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
    test_wildcard_rules();
    test_refname_lengths();
    test_load_failures();
    test_comment_syntax();
    test_errors_reject_file();
    test_warnings_keep_file();
    test_cycle_warning();
    test_cycle_no_false_positive();
    test_cycle_two_nodes();
    test_cycle_long_chain();
    test_cycle_multiple();
    test_motivation_separator();
    test_end_to_end();

    printf("%d test superati, %d falliti\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}

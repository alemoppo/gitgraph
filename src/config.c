/* config.c - vedi config.h.
 *
 * Parser di graph.cfg: sezioni [long-lived] e [parents], commenti su righe
 * dedicate, valori `chiave = valore`, wildcard '*' in [long-lived] (espansi
 * sui branch esistenti conservando l'ordine della lista) e validazione con
 * diagnostica (errori e avvisi). Nessuna dipendenza esterna, nessuna scrittura:
 * il file viene solo letto.
 *
 * Contratto: un errore rifiuta il file per intero; gli avvisi restano
 * applicati e vengono elencati. Il nome del file e' sempre incluso nei
 * messaggi, con il numero di riga, cosi' l'utente sa dove intervenire.
 */
#include "config.h"

#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- diagnostica ---------- */

void config_report_free(ConfigReport *report)
{
    if (!report)
        return;
    for (size_t i = 0; i < report->count; i++)
        free(report->items[i].text);
    free(report->items);
    memset(report, 0, sizeof *report);
}

size_t config_report_error_count(const ConfigReport *report)
{
    size_t n = 0;
    if (!report)
        return 0;
    for (size_t i = 0; i < report->count; i++)
        if (report->items[i].level == CONFIG_DIAG_ERROR)
            n++;
    return n;
}

void config_report_add(ConfigReport *report, ConfigDiagLevel level,
                       const char *fmt, ...)
{
    if (!report)
        return;
    report->items = xrealloc(report->items, (report->count + 1) * sizeof(ConfigDiag));
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    report->items[report->count].level = level;
    report->items[report->count].text = xstrdup(msg);
    report->count++;
    if (level == CONFIG_DIAG_ERROR)
        report->has_errors = true;
}

/* ---------- rilascio ---------- */

void config_free(GraphConfig *cfg)
{
    if (!cfg)
        return;
    for (size_t i = 0; i < cfg->long_lived_count; i++)
        free(cfg->long_lived[i]);
    free(cfg->long_lived);
    for (size_t i = 0; i < cfg->override_count; i++) {
        free(cfg->overrides[i].branch);
        free(cfg->overrides[i].parent);
    }
    free(cfg->overrides);
    memset(cfg, 0, sizeof *cfg);
}

static bool name_in_list(char *const *names, size_t count, const char *name)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0)
            return true;
    }
    return false;
}

void config_set_implicit_trunk(GraphConfig *cfg, char *const *branch_names,
                               size_t branch_count)
{
    if (cfg->long_lived_explicit || cfg->long_lived_count > 0)
        return; /* [long-lived] esplicito: nessun default */
    const char *trunk = NULL;
    if (name_in_list(branch_names, branch_count, "main"))
        trunk = "main";
    else if (name_in_list(branch_names, branch_count, "master"))
        trunk = "master";
    if (!trunk)
        return;
    cfg->long_lived = xmalloc(sizeof(char *));
    cfg->long_lived[0] = xstrdup(trunk);
    cfg->long_lived_count = 1;
}

int config_long_lived_rank(const GraphConfig *cfg, const char *branch_name)
{
    if (!cfg)
        return -1;
    for (size_t i = 0; i < cfg->long_lived_count; i++) {
        if (strcmp(cfg->long_lived[i], branch_name) == 0)
            return (int)i;
    }
    return -1;
}

char *config_path_in_repo(const char *repo_path)
{
    if (!repo_path)
        return NULL;
    size_t n = strlen(repo_path);
    bool slash = n > 0 && (repo_path[n - 1] == '/' || repo_path[n - 1] == '\\');
    size_t extra = sizeof("/graph.cfg");
    char *p = xmalloc(n + (slash ? 0 : 1) + extra);
    memcpy(p, repo_path, n);
    size_t pos = n;
    if (!slash)
        p[pos++] = '/';
    memcpy(p + pos, "graph.cfg", sizeof("graph.cfg"));
    return p;
}

/* ---------- pattern ---------- */

/* Confronto con wildcard: l'unico carattere speciale e' '*', che sta per
 * "qualsiasi sequenza di caratteri", '/' compreso (nei nomi dei branch la
 * barra non e' un separatore di significato: il pattern "release/" con
 * stella finale copre anche "release/2026/q1"). Non ci sono altre espressioni. */
static bool glob_match(const char *pat, const char *name)
{
    const char *p = pat, *n = name;
    const char *star = NULL;
    const char *retry = NULL;
    while (*n) {
        if (*p == '*') {
            star = ++p;
            retry = n;
            continue;
        }
        if (*p == *n) {
            p++;
            n++;
            continue;
        }
        if (star) {
            retry++;
            p = star;
            n = retry;
            continue;
        }
        return false;
    }
    while (*p == '*')
        p++;
    return *p == '\0';
}

/* Confronto "naturale": le sequenze di cifre si confrontano come numeri, cosi'
 * release/2.0 viene prima di release/10.0. A parita' decide l'ordinamento
 * lessicografico, cosi' il risultato e' deterministico. */
static int natural_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            const char *sa = a, *sb = b;
            while (*a == '0')
                a++;
            while (*b == '0')
                b++;
            const char *da = a, *db = b;
            while (isdigit((unsigned char)*a))
                a++;
            while (isdigit((unsigned char)*b))
                b++;
            size_t la = (size_t)(a - da), lb = (size_t)(b - db);
            if (la != lb)
                return la < lb ? -1 : 1;
            int c = strncmp(da, db, la);
            if (c != 0)
                return c < 0 ? -1 : 1;
            /* cifre equivalenti ("01" == "1"): si prosegue col resto */
            (void)sa;
            (void)sb;
            continue;
        }
        if (*a != *b)
            return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        a++;
        b++;
    }
    if (*a == *b)
        return 0;
    return *a == '\0' ? -1 : 1;
}

static int cmp_natural_str(const void *a, const void *b)
{
    return natural_cmp(*(char *const *)a, *(char *const *)b);
}

/* Sintassi di un nome di riferimento Git (sottinsieme delle regole che
 * contano qui: niente spazi, caratteri vietati, "..", "@{", "/"/"." ai
 * bordi, suffisso ".lock"). */
static bool refname_syntax_ok(const char *name)
{
    if (!name || !*name)
        return false;
    if (strcmp(name, "@") == 0)
        return false;
    size_t n = strlen(name);
    if (name[0] == '/' || name[0] == '.' || name[n - 1] == '/' || name[n - 1] == '.')
        return false;
    size_t lock_at = n - 5;
    if (lock_at > 0 && strcmp(name + lock_at, ".lock") == 0)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c <= 0x20 || c == 0x7f)
            return false;
        if (strchr("~^:?*[\\", (char)c) != NULL)
            return false;
        if (name[i] == '/' && (i + 1 < n && name[i + 1] == '/'))
            return false;
        if (name[i] == '.' && i + 1 < n && name[i + 1] == '.')
            return false;
        if (name[i] == '@' && i + 1 < n && name[i + 1] == '{')
            return false;
    }
    return true;
}

static bool has_wildcard(const char *s)
{
    return strchr(s, '*') != NULL;
}

/* ---------- stato del parser ---------- */

typedef enum { SEC_NONE, SEC_LONG_LIVED, SEC_PARENTS } Section;

typedef struct {
    const char *file;
    const ConfigBranch *branches;
    size_t branch_count;
    ConfigReport *report;
    bool failed;
    Section section;
    GraphConfig *cfg;
} Parser;

static void diag(Parser *p, ConfigDiagLevel level, size_t line, const char *fmt, ...)
{
    char msg[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (level == CONFIG_DIAG_ERROR)
        p->failed = true;
    if (line == 0) /* messaggio non riferibile a una riga (es. cicli) */
        config_report_add(p->report, level, "%s: %s", p->file, msg);
    else
        config_report_add(p->report, level, "%s:%zu: %s", p->file, line, msg);
}

static bool canonical_present(const GraphConfig *cfg, const char *name)
{
    for (size_t i = 0; i < cfg->long_lived_count; i++)
        if (strcmp(cfg->long_lived[i], name) == 0)
            return true;
    return false;
}

/* Espande una riga di [long-lived].
 *
 *  - un branch corrisponde se il pattern e' uguale al suo nome o al suo nome
 *    canonico ("origin/release/2.0" corrisponde a "release/2.0");
 *  - i nomi raccolti sono in ordine naturale (release/2.0 prima di release/10.0);
 *  - fra piu' righe che corrispondono allo stesso branch vale la prima: le
 *    righe successive non lo spostano e non lo duplicano. */
static void add_long_lived(Parser *p, size_t line, const char *pattern)
{
    size_t cap = p->branch_count ? p->branch_count : 1;
    char **hits = xmalloc(cap * sizeof(char *));
    size_t n = 0;
    for (size_t i = 0; i < p->branch_count; i++) {
        const ConfigBranch *b = &p->branches[i];
        if (!glob_match(pattern, b->name) && !glob_match(pattern, b->canonical))
            continue;
        bool dup = false;
        for (size_t k = 0; k < n; k++) {
            if (strcmp(hits[k], b->canonical) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup)
            hits[n++] = xstrdup(b->canonical);
    }
    qsort(hits, n, sizeof(char *), cmp_natural_str);

    for (size_t k = 0; k < n; k++) {
        if (canonical_present(p->cfg, hits[k])) {
            diag(p, CONFIG_DIAG_WARNING, line,
                 "'%s' e' gia' long-lived: vale la riga che l'ha dichiarato prima",
                 hits[k]);
            continue;
        }
        p->cfg->long_lived = xrealloc(p->cfg->long_lived,
                                      (p->cfg->long_lived_count + 1) * sizeof(char *));
        p->cfg->long_lived[p->cfg->long_lived_count++] = hits[k]; /* ownership */
        hits[k] = NULL;
    }
    if (n == 0) {
        if (has_wildcard(pattern))
            diag(p, CONFIG_DIAG_WARNING, line,
                 "il pattern '%s' non corrisponde a nessun branch", pattern);
        else if (!refname_syntax_ok(pattern))
            diag(p, CONFIG_DIAG_WARNING, line,
                 "'%s' non e' un nome di riferimento valido", pattern);
        else
            diag(p, CONFIG_DIAG_WARNING, line,
                 "il branch '%s' non esiste in questo repository", pattern);
    }
    for (size_t k = 0; k < n; k++)
        free(hits[k]);
    free(hits);
}

/* Un nome (di override o di riferimento) esiste fra i riferimenti noti? Si
 * confronta con il nome e con il nome canonico, cosi' "release/2.0" riconosce
 * anche "origin/release/2.0". */
static bool branch_present(const Parser *p, const char *name)
{
    for (size_t i = 0; i < p->branch_count; i++) {
        const ConfigBranch *b = &p->branches[i];
        if (strcmp(b->name, name) == 0 || strcmp(b->canonical, name) == 0)
            return true;
    }
    return false;
}

static void add_override(Parser *p, size_t line, const char *branch, const char *parent)
{
    if (!refname_syntax_ok(branch)) {
        diag(p, CONFIG_DIAG_ERROR, line,
             "'%s' non e' un nome di riferimento valido", branch);
        return;
    }
    if (!refname_syntax_ok(parent)) {
        diag(p, CONFIG_DIAG_ERROR, line,
             "'%s' non e' un nome di riferimento valido", parent);
        return;
    }
    if (strcmp(branch, parent) == 0) {
        diag(p, CONFIG_DIAG_ERROR, line,
             "'%s' non puo' essere padre di se stesso", branch);
        return;
    }
    for (size_t i = 0; i < p->cfg->override_count; i++) {
        if (strcmp(p->cfg->overrides[i].branch, branch) == 0) {
            diag(p, CONFIG_DIAG_ERROR, line,
                 "'%s' e' gia' dichiarato alla riga precedente (padre '%s')", branch,
                 p->cfg->overrides[i].parent);
            return;
        }
    }
    if (!branch_present(p, branch))
        diag(p, CONFIG_DIAG_WARNING, line,
             "il branch '%s' non esiste in questo repository: l'override verra' ignorato",
             branch);
    if (!branch_present(p, parent))
        diag(p, CONFIG_DIAG_WARNING, line,
             "il branch padre '%s' non esiste in questo repository: l'override verra' ignorato",
             parent);
    p->cfg->overrides = xrealloc(p->cfg->overrides,
                                  (p->cfg->override_count + 1) * sizeof(ParentOverride));
    p->cfg->overrides[p->cfg->override_count].branch = xstrdup(branch);
    p->cfg->overrides[p->cfg->override_count].parent = xstrdup(parent);
    p->cfg->override_count++;
}

/* Cerca cicli fra gli override dichiarati: in fase di modello un ciclo
 * renderebbe ambigui i nodi coinvolti, quindi e' un avviso, non un errore. */
static void warn_cycles(Parser *p)
{
    size_t n = p->cfg->override_count;
    for (size_t i = 0; i < n; i++) {
        const char *cur = p->cfg->overrides[i].parent;
        for (size_t step = 0; step < n; step++) {
            if (!cur)
                break;
            const char *next = NULL;
            for (size_t k = 0; k < n; k++) {
                if (strcmp(p->cfg->overrides[k].branch, cur) == 0) {
                    next = p->cfg->overrides[k].parent;
                    break;
                }
            }
            if (!next)
                break;
            if (strcmp(next, p->cfg->overrides[i].branch) == 0) {
                diag(p, CONFIG_DIAG_WARNING, 0,
                     "'%s' e' in un ciclo di override con '%s': i branch coinvolti "
                     "resteranno ambigui",
                     p->cfg->overrides[i].branch, next);
                break;
            }
            cur = next;
        }
    }
}

/* ---------- tokenizzazione ---------- */

static char *trim_dup(const char *s, size_t n)
{
    while (n > 0 && isspace((unsigned char)*s)) {
        s++;
        n--;
    }
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    return xstrndup(s, n);
}

/* ---------- caricamento ---------- */

static bool parse_text(Parser *p, const char *text)
{
    const char *cur = text;
    size_t line_no = 0;
    /* salta un eventuale BOM UTF-8 all'inizio del file */
    if ((unsigned char)cur[0] == 0xef && (unsigned char)cur[1] == 0xbb &&
        (unsigned char)cur[2] == 0xbf)
        cur += 3;

    while (*cur) {
        line_no++;
        const char *nl = strchr(cur, '\n');
        size_t len = nl ? (size_t)(nl - cur) : strlen(cur);
        const char *line = cur;
        size_t line_len = len;
        cur = nl ? nl + 1 : cur + len;

        /* fine riga CRLF */
        if (line_len > 0 && line[line_len - 1] == '\r')
            line_len--;

        /* spazi iniziali: la riga vuota o di commento si salta */
        size_t skip = 0;
        while (skip < line_len && isspace((unsigned char)line[skip]))
            skip++;
        if (skip == line_len)
            continue;
        if (line[skip] == '#' || line[skip] == ';')
            continue;

        if (line[skip] == '[') {
            const char *close = memchr(line + skip, ']', line_len - skip);
            if (!close || close + 1 != line + line_len) {
                diag(p, CONFIG_DIAG_ERROR, line_no, "intestazione di sezione malformata");
                continue;
            }
            char *name = trim_dup(line + skip + 1, (size_t)(close - (line + skip)) - 1);
            if (strcmp(name, "long-lived") == 0) {
                p->section = SEC_LONG_LIVED;
                p->cfg->long_lived_explicit = true;
            } else if (strcmp(name, "parents") == 0) {
                p->section = SEC_PARENTS;
            } else {
                /* sezione sconosciuta: le righe seguenti non appartengono a
                 * nessuna sezione valida (niente eredita' dalla precedente) */
                p->section = SEC_NONE;
                diag(p, CONFIG_DIAG_ERROR, line_no,
                     "sezione sconosciuta '[%s]' (ammesse: [long-lived], [parents])",
                     name);
            }
            free(name);
            continue;
        }

        switch (p->section) {
        case SEC_NONE:
            diag(p, CONFIG_DIAG_ERROR, line_no,
                 "riga fuori da una sezione: attesa un'intestazione [long-lived] o [parents]");
            break;
        case SEC_LONG_LIVED: {
            const char *eq = memchr(line + skip, '=', line_len - skip);
            if (eq) {
                diag(p, CONFIG_DIAG_ERROR, line_no,
                     "in [long-lived] ogni riga e' un nome (senza '=')");
                break;
            }
            char *pattern = trim_dup(line + skip, line_len - skip);
            if (!*pattern) {
                diag(p, CONFIG_DIAG_ERROR, line_no, "voce vuota in [long-lived]");
                free(pattern);
                break;
            }
            add_long_lived(p, line_no, pattern);
            free(pattern);
            break;
        }
        case SEC_PARENTS: {
            const char *eq = memchr(line + skip, '=', line_len - skip);
            if (!eq) {
                diag(p, CONFIG_DIAG_ERROR, line_no,
                     "in [parents] ogni riga ha la forma 'branch = padre'");
                break;
            }
            char *branch = trim_dup(line + skip, (size_t)(eq - (line + skip)));
            char *parent = trim_dup(eq + 1, line_len - (size_t)(eq + 1 - line));
            if (!*branch)
                diag(p, CONFIG_DIAG_ERROR, line_no,
                     "in [parents] manca il nome del branch");
            else if (!*parent)
                diag(p, CONFIG_DIAG_ERROR, line_no,
                     "in [parents] manca il nome del padre di '%s'", branch);
            else if (has_wildcard(branch) || has_wildcard(parent)) {
                diag(p, CONFIG_DIAG_ERROR, line_no,
                     "i wildcard sono ammessi solo in [long-lived]");
            } else {
                add_override(p, line_no, branch, parent);
            }
            free(branch);
            free(parent);
            break;
        }
        }
    }
    return !p->failed;
}

bool config_load(GraphConfig *cfg, const char *path,
                 const ConfigBranch *branches, size_t branch_count,
                 ConfigReport *report, char *err, size_t err_size)
{
    memset(cfg, 0, sizeof *cfg);
    if (report) {
        report->items = NULL;
        report->count = 0;
        report->has_errors = false;
    }

    Parser p;
    memset(&p, 0, sizeof p);
    p.file = path ? path : "";
    p.branches = branches;
    p.branch_count = branch_count;
    p.report = report;
    p.cfg = cfg;
    bool ok = true;

    /* Assenza del file: nessun errore, resta solo il tronco implicito. */
    if (!path)
        goto implicit_trunk;
    FILE *f = fopen(path, "rb");
    if (!f)
        goto implicit_trunk; /* graph.cfg facoltativo: si prosegue coi default */
    {
        StrBuf text;
        sb_init(&text);
        char chunk[8192];
        size_t got;
        while ((got = fread(chunk, 1, sizeof chunk, f)) > 0)
            sb_appendn(&text, chunk, got);
        bool read_error = ferror(f) != 0;
        fclose(f);
        if (read_error) {
            sb_free(&text);
            if (err)
                snprintf(err, err_size, "lettura di '%s' fallita", path);
            return false;
        }
        sb_append(&text, "\n"); /* riga finale senza newline */
        ok = parse_text(&p, text.data);
        sb_free(&text);
        if (!ok) {
            /* il file e' rifiutato per intero: niente configurazione a meta' */
            config_free(cfg);
            if (err)
                snprintf(err, err_size, "graph.cfg non valido: %zu errore/i",
                         config_report_error_count(report));
        }
    }

    if (ok && cfg->override_count > 0)
        warn_cycles(&p);

implicit_trunk:
    /* anche con file rifiutato il tronco implicito resta applicato; il tronco
     * e' cercato fra i nomi canonici ("main" vale per main e origin/main) */
    {
        char **names = xmalloc((branch_count ? branch_count : 1) * sizeof(char *));
        size_t nn = 0;
        for (size_t i = 0; i < branch_count; i++)
            names[nn++] = xstrdup(branches[i].canonical);
        config_set_implicit_trunk(cfg, names, nn);
        for (size_t i = 0; i < nn; i++)
            free(names[i]);
        free(names);
    }
    return ok;
}

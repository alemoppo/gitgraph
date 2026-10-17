/* gitdata.c - vedi gitdata.h
 *
 * Implementazione Windows: git viene eseguito con CreateProcessW (nessuna
 * shell), stdout/stderr catturati tramite pipe lette in modo non bloccante.
 * Prototipo: adatto a repository di test e uso interattivo.
 */
#ifdef _WIN32

#include "gitdata.h"

#include "util.h"

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GG_ERR_MAX 1024

/* ---------- esecuzione di processi ---------- */

/* Accoda un argomento alla command line, applicando le regole di quoting
 * del runtime C di Windows: le barre inverse prima di una virgoletta (o prima
 * della virgoletta di chiusura) vengono raddoppiate, cosi' un percorso che
 * termina con "\" non "mangia" la virgoletta. Gli argomenti vuoti sono
 * quotati; il resto passa invariato. */
static void append_arg(StrBuf *sb, const char *arg)
{
    if (sb->len)
        sb_append(sb, " ");
    bool need_quote = (*arg == '\0');
    for (const char *p = arg; *p && !need_quote; p++) {
        if (*p == ' ' || *p == '\t' || *p == '"')
            need_quote = true;
    }
    if (!need_quote) {
        sb_append(sb, arg);
        return;
    }
    sb_append(sb, "\"");
    const char *p = arg;
    while (*p) {
        size_t bs = 0;
        while (*p == '\\') {
            bs++;
            p++;
        }
        if (*p == '"' || *p == '\0') {
            size_t n = bs * 2 + (*p == '"' ? 1 : 0);
            for (size_t i = 0; i < n; i++)
                sb_appendn(sb, "\\", 1);
            if (*p == '"') {
                sb_appendn(sb, "\"", 1);
                p++;
            }
        } else {
            for (size_t i = 0; i < bs; i++)
                sb_appendn(sb, "\\", 1);
            sb_appendn(sb, p, 1);
            p++;
        }
    }
    sb_append(sb, "\"");
}

void gitdata_build_cmdline(const char *const *argv, char *out, size_t out_size)
{
    StrBuf sb;
    sb_init(&sb);
    for (size_t i = 0; argv[i]; i++)
        append_arg(&sb, argv[i]);
    snprintf(out, out_size, "%s", sb.data);
    sb_free(&sb);
}

static wchar_t *utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *w = xmalloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *wide_to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    char *s = xmalloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* Thread di lettura: consuma una pipe fino alla chiusura. Con un thread per
 * pipe non esiste attesa reciproca: se una pipe si riempie mentre l'altra
 * viene letta, l'altra la drena subito, quindi il figlio non si blocca mai in
 * scrittura (il classico deadlock delle due pipe).
 *
 * `failed` distingue la fine normale (pipe chiusa o rotta, cioe' lo scrittore
 * e' uscito) da un vero errore di lettura: nel secondo caso l'output raccolto
 * sarebbe incompleto e va segnalato. */
typedef struct {
    HANDLE pipe;
    StrBuf *out;
    bool failed;
} PipeReader;

static DWORD WINAPI pipe_reader(LPVOID param)
{
    PipeReader *r = (PipeReader *)param;
    for (;;) {
        char tmp[65536];
        DWORD got = 0;
        if (!ReadFile(r->pipe, tmp, sizeof tmp, &got, NULL)) {
            DWORD e = GetLastError();
            if (e != ERROR_BROKEN_PIPE && e != ERROR_HANDLE_EOF)
                r->failed = true; /* lettura fallita: output incompleto */
            break;                /* pipe chiusa o rotta: fine */
        }
        if (got == 0)
            break; /* scrittore chiuso: EOF */
        sb_appendn(r->out, tmp, got);
    }
    return 0;
}

/* Aggancio per i test: se impostato sostituisce la creazione del thread di
 * lettura, cosi' da esercitare il percorso in cui la lettura concorrente non e'
 * disponibile (ritornando NULL si simula il fallimento di CreateThread).
 * NULL ripristina il comportamento normale. */
static GitdataReaderThreadHook g_reader_hook = NULL;

void gitdata_set_reader_thread_hook(GitdataReaderThreadHook hook)
{
    g_reader_hook = hook;
}

static HANDLE start_reader(PipeReader *r)
{
    if (g_reader_hook)
        return (HANDLE)g_reader_hook(r);
    return CreateThread(NULL, 0, pipe_reader, r, 0, NULL);
}

/* Messaggio d'errore (err puo' essere NULL). */
static void set_err(char *err, size_t err_size, const char *fmt, ...)
{
    if (!err || err_size == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_size, fmt, ap);
    va_end(ap);
}

/* Chiude un handle se valido (usata sui percorsi di errore). */
static void close_if(HANDLE h)
{
    if (h && h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

/* Raccoglie il processo e chiude tutti gli handle: usata su ogni percorso di
 * uscita, cosi' nessun handle resta aperto e il figlio non resta attivo. */
static void reap_child(PROCESS_INFORMATION *pi)
{
    if (pi->hProcess) {
        WaitForSingleObject(pi->hProcess, INFINITE);
        CloseHandle(pi->hThread);
        CloseHandle(pi->hProcess);
        pi->hThread = NULL;
        pi->hProcess = NULL;
    }
}

/* Esegue `argv` (argv[0] = programma, es. "git"; terminato da NULL).
 * Cattura stdout e stderr.
 *
 * Ritorna true quando l'output e' stato raccolto per intero (il codice di
 * uscita del processo finisce in exit_code, anche se diverso da zero).
 * Ritorna false, senza lasciare processi sospesi ne' handle aperti, se:
 *  - il processo non e' partito;
 *  - non si e' potuto creare un thread per pipe: in questo caso il figlio viene
 *    terminato e raccolto, le pipe svuotate fino alla fine e l'errore
 *    segnalato, invece di attendere una lettura che non puo' avvenire;
 *  - la lettura di una pipe e' fallita (output incompleto). */
bool gitdata_capture(const char *const *argv, StrBuf *out, StrBuf *errout,
                     int *exit_code, char *err, size_t err_size)
{
    if (exit_code)
        *exit_code = -1;

    StrBuf cmdline;
    sb_init(&cmdline);
    for (size_t i = 0; argv[i]; i++)
        append_arg(&cmdline, argv[i]);
    wchar_t *wcmd = utf8_to_wide(cmdline.data);
    sb_free(&cmdline);
    if (!wcmd) {
        set_err(err, err_size, "conversione della riga di comando fallita");
        return false;
    }

    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    HANDLE out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 0) ||
        !CreatePipe(&err_r, &err_w, &sa, 0)) {
        DWORD e = GetLastError();
        set_err(err, err_size, "CreatePipe fallita (errore %lu)", (unsigned long)e);
        /* chiude anche le pipe create solo in parte */
        close_if(out_r);
        close_if(out_w);
        close_if(err_r);
        close_if(err_w);
        free(wcmd);
        return false;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &sa, OPEN_EXISTING, 0, NULL);

    STARTUPINFOW si = {0};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul_in != INVALID_HANDLE_VALUE ? nul_in : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_w;
    si.hStdError = err_w;

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DWORD spawn_err = ok ? 0 : GetLastError();
    free(wcmd);
    CloseHandle(out_w);
    CloseHandle(err_w);
    close_if(nul_in);

    if (!ok) {
        close_if(out_r);
        close_if(err_r);
        if (spawn_err == ERROR_FILE_NOT_FOUND || spawn_err == ERROR_PATH_NOT_FOUND)
            set_err(err, err_size, "eseguibile 'git' non trovato nel PATH di sistema");
        else
            set_err(err, err_size, "avvio di git fallito (errore %lu)",
                    (unsigned long)spawn_err);
        return false;
    }

    /* Un thread per pipe: nessuna attesa reciproca, quindi nessun blocco
     * quando una delle due pipe si riempie. */
    PipeReader ro = { out_r, out, false };
    PipeReader re = { err_r, errout, false };
    HANDLE to = start_reader(&ro);
    HANDLE te = start_reader(&re);

    if (!to || !te) {
        /* Senza lettura concorrente non si puo' attendere: il figlio potrebbe
         * restare bloccato su una pipe piena mentre l'altra non viene letta.
         * Viene quindi terminato e raccolto, le due pipe vengono svuotate fino
         * alla fine (il produttore e' morto, quindi non possono piu' bloccare)
         * e l'errore viene segnalato: meglio un errore esplicito che un
         * output troncato usato come se fosse completo. */
        DWORD thread_err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (to)
            WaitForSingleObject(to, INFINITE); /* il figlio e' morto: arriva a EOF */
        else
            pipe_reader(&ro);
        if (te)
            WaitForSingleObject(te, INFINITE);
        else
            pipe_reader(&re);
        close_if(to);
        close_if(te);
        close_if(out_r);
        close_if(err_r);
        reap_child(&pi);
        set_err(err, err_size,
                "lettura di git non disponibile (thread di lettura non creato, "
                "errore %lu): il processo e' stato terminato",
                (unsigned long)thread_err);
        return false;
    }

    HANDLE both[2] = { to, te };
    bool wait_ok = true;
    if (WaitForMultipleObjects(2, both, TRUE, INFINITE) == WAIT_FAILED) {
        /* i thread vanno comunque raccolti prima di chiudere le pipe: attesa
         * individuale e segnalazione del fallimento */
        WaitForSingleObject(to, INFINITE);
        WaitForSingleObject(te, INFINITE);
        wait_ok = false;
    }
    close_if(to);
    close_if(te);
    close_if(out_r);
    close_if(err_r);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exit_code)
        *exit_code = (int)code;
    reap_child(&pi);

    if (!wait_ok) {
        set_err(err, err_size, "attesa dei thread di lettura fallita (errore %lu)",
                (unsigned long)GetLastError());
        return false;
    }
    if (ro.failed || re.failed) {
        set_err(err, err_size, "lettura dell'output di git incompleta");
        return false;
    }
    return true;
}

/* Esegue `git`. args = argomenti dopo "git". */
static bool run_git(const char *const *args, StrBuf *out, StrBuf *errout,
                    int *exit_code, char *err, size_t err_size)
{
    const char *argv[64];
    size_t n = 0;
    argv[n++] = "git";
    for (size_t i = 0; args[i] && n < 62; i++)
        argv[n++] = args[i];
    argv[n] = NULL;
    return gitdata_capture(argv, out, errout, exit_code, err, err_size);
}

/* Prima riga non vuota di un buffer, ripulita per il messaggio d'errore. */
static void first_line(const char *s, char *dst, size_t dst_size)
{
    size_t i = 0;
    while (s && s[i] && s[i] != '\r' && s[i] != '\n' && i + 1 < dst_size) {
        dst[i] = s[i];
        i++;
    }
    dst[i] = '\0';
    if (i == 0)
        snprintf(dst, dst_size, "errore sconosciuto");
}

/* ---------- parsing ---------- */

static void strip_trailing_newlines(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

static bool is_hex_hash(const char *s, size_t len)
{
    if (len < 7 || len > 128)
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

static bool parse_commits(const StrBuf *raw, GitData *d, char *err, size_t err_size)
{
    /* Il flusso e' una sequenza di record di 5 campi separati da NUL
     * (H, P, an, ct, B), ciascun record terminato da un NUL aggiuntivo (-z).
     * Il messaggio %B puo' contenere NUL? No: git non ammette NUL nei commit. */
    const char *p = raw->data;
    const char *end = raw->data + raw->len;

    size_t cap = 64;
    d->commits = xmalloc(cap * sizeof(GitCommit));
    d->commit_count = 0;

    while (p < end) {
        const char *fields[5];
        size_t lens[5];
        for (int f = 0; f < 5; f++) {
            fields[f] = p;
            const char *nul = memchr(p, '\0', (size_t)(end - p));
            if (!nul) {
                /* l'ultimo record puo' non avere il NUL di terminazione */
                if (f == 4) {
                    lens[f] = (size_t)(end - p);
                    p = end;
                    break;
                }
                snprintf(err, err_size, "output di git log troncato (campo %d)", f);
                return false;
            }
            lens[f] = (size_t)(nul - p);
            p = nul + 1;
        }
        if (lens[0] == 0 || !is_hex_hash(fields[0], lens[0])) {
            /* puo' capitare solo per l'eventuale NUL finale: salta */
            if (lens[0] == 0 && p >= end)
                break;
            snprintf(err, err_size, "output di git log inatteso (campo hash non esadecimale)");
            return false;
        }
        if (d->commit_count == cap) {
            cap *= 2;
            d->commits = xrealloc(d->commits, cap * sizeof(GitCommit));
        }
        GitCommit *c = &d->commits[d->commit_count++];
        memset(c, 0, sizeof *c);
        c->hash = xstrndup(fields[0], lens[0]);
        c->author = xstrndup(fields[2], lens[2]);
        c->date = (int64_t)strtoll(fields[3], NULL, 10);
        c->message = xstrndup(fields[4], lens[4]);
        strip_trailing_newlines(c->message);

        /* genitori: hash separati da spazi */
        size_t pcap = 4;
        c->parents = xmalloc(pcap * sizeof(char *));
        const char *q = fields[1];
        const char *qend = fields[1] + lens[1];
        while (q < qend) {
            while (q < qend && *q == ' ')
                q++;
            const char *tok = q;
            while (q < qend && *q != ' ')
                q++;
            if (q > tok) {
                if (c->parent_count == pcap) {
                    pcap *= 2;
                    c->parents = xrealloc(c->parents, pcap * sizeof(char *));
                }
                c->parents[c->parent_count++] = xstrndup(tok, (size_t)(q - tok));
            }
        }
    }
    return true;
}

static bool parse_refs(const StrBuf *raw, GitData *d, char *err, size_t err_size)
{
    /* Una riga per ref, campi separati da NUL:
     * refname, symref, objecttype, objectname, *objectname */
    size_t cap = 32;
    d->refs = xmalloc(cap * sizeof(GitRef));
    d->ref_count = 0;

    const char *p = raw->data;
    const char *end = raw->data + raw->len;
    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end)
            line_end = end;
        const char *fields[5] = {NULL, NULL, NULL, NULL, NULL};
        size_t lens[5] = {0, 0, 0, 0, 0};
        const char *q = p;
        for (int f = 0; f < 5 && q <= line_end; f++) {
            fields[f] = q;
            const char *nul = memchr(q, '\0', (size_t)(line_end - q));
            if (!nul) {
                /* ultimo campo: termina a fine riga, non con NUL */
                lens[f] = (size_t)(line_end - q);
                break;
            }
            lens[f] = (size_t)(nul - q);
            q = nul + 1;
        }
        p = line_end < end ? line_end + 1 : end;

        char refname[512] = {0};
        if (!fields[0] || lens[0] == 0 || lens[0] >= sizeof refname)
            continue;
        memcpy(refname, fields[0], lens[0]);

        bool has_symref = fields[1] && lens[1] > 0;
        const char *objtype = fields[2] ? fields[2] : "";
        size_t objtype_len = lens[2];
        const char *objname = fields[3];
        size_t objname_len = lens[3];
        const char *peeled = fields[4];
        size_t peeled_len = lens[4];

        RefKind kind;
        const char *short_name = NULL;
        if (strncmp(refname, "refs/heads/", 11) == 0) {
            kind = REF_LOCAL_BRANCH;
            short_name = refname + 11;
        } else if (strncmp(refname, "refs/remotes/", 13) == 0) {
            if (has_symref)
                continue; /* es. origin/HEAD: riferimento simbolico */
            kind = REF_REMOTE_BRANCH;
            short_name = refname + 13;
        } else if (strncmp(refname, "refs/tags/", 10) == 0) {
            kind = REF_TAG;
            short_name = refname + 10;
        } else {
            continue;
        }
        if (!objname || objname_len == 0 || !is_hex_hash(objname, objname_len))
            continue;

        /* Per i tag annotati vale l'oggetto dereferenziato; se il tag punta a
         * qualcosa che non e' un commit lo si ignora (fuori dal grafo). */
        const char *target = objname;
        size_t target_len = objname_len;
        if (kind == REF_TAG) {
            if (objtype_len == 3 && strncmp(objtype, "tag", 3) == 0) {
                if (!peeled || peeled_len == 0 || !is_hex_hash(peeled, peeled_len))
                    continue;
                target = peeled;
                target_len = peeled_len;
            } else if (objtype_len == 6 && strncmp(objtype, "commit", 6) == 0) {
                /* tag leggero */
            } else {
                continue; /* tree/blob */
            }
        }

        if (d->ref_count == cap) {
            cap *= 2;
            d->refs = xrealloc(d->refs, cap * sizeof(GitRef));
        }
        GitRef *r = &d->refs[d->ref_count++];
        memset(r, 0, sizeof *r);
        r->name = xstrdup(short_name);
        r->full_name = xstrdup(refname);
        r->target = xstrndup(target, target_len);
        r->kind = kind;
        r->is_head = false;
    }
    (void)err;
    (void)err_size;
    return true;
}

/* ---------- API ---------- */

/* Costruisce la mappa hash->indice e risolve gli hash dei genitori in indici,
 * una sola volta dopo il parsing: la deduzione dei branch lavora poi solo su
 * indici (nessuna scansione lineare). */
static void build_indexes(GitData *d)
{
    d->by_hash = strmap_new();
    for (size_t i = 0; i < d->commit_count; i++)
        strmap_put(d->by_hash, d->commits[i].hash, (void *)(uintptr_t)(i + 1));
    for (size_t i = 0; i < d->commit_count; i++) {
        GitCommit *c = &d->commits[i];
        c->parent_idx = xmalloc((c->parent_count ? c->parent_count : 1) * sizeof(size_t));
        for (size_t k = 0; k < c->parent_count; k++) {
            void *v = strmap_get(d->by_hash, c->parents[k]);
            c->parent_idx[k] = v ? (size_t)(uintptr_t)v - 1 : (size_t)-1;
        }
    }
}

size_t gitdata_find_commit(const GitData *d, const char *hash)
{
    if (!d->by_hash || !hash)
        return (size_t)-1;
    void *v = strmap_get(d->by_hash, hash);
    return v ? (size_t)(uintptr_t)v - 1 : (size_t)-1;
}

void gitdata_short_hash(const char *hash, size_t n, char out[16])
{
    size_t len = strlen(hash);
    if (n > 12)
        n = 12;
    if (n > len)
        n = len;
    memcpy(out, hash, n);
    out[n] = '\0';
}

static char *repo_argument(const char *path)
{
    wchar_t *win = NULL;
    if (path && *path) {
        win = utf8_to_wide(path);
    } else {
        DWORD need = GetCurrentDirectoryW(0, NULL);
        wchar_t *cwd = xmalloc((size_t)need * sizeof(wchar_t));
        GetCurrentDirectoryW(need, cwd);
        win = cwd;
    }
    if (!win)
        return NULL;
    DWORD need = GetFullPathNameW(win, 0, NULL, NULL);
    wchar_t *full = xmalloc((size_t)need * sizeof(wchar_t));
    GetFullPathNameW(win, need, full, NULL);
    free(win);
    char *utf8 = wide_to_utf8(full);
    free(full);
    return utf8;
}

static void free_partial(GitData *d)
{
    for (size_t i = 0; i < d->commit_count; i++) {
        GitCommit *c = &d->commits[i];
        free(c->hash);
        for (size_t k = 0; k < c->parent_count; k++)
            free(c->parents[k]);
        free(c->parents);
        free(c->parent_idx);
        free(c->author);
        free(c->message);
    }
    free(d->commits);
    for (size_t i = 0; i < d->ref_count; i++) {
        free(d->refs[i].name);
        free(d->refs[i].full_name);
        free(d->refs[i].target);
    }
    free(d->refs);
    free(d->head_branch);
    free(d->head_hash);
    free(d->repo_path);
    strmap_free(d->by_hash);
    memset(d, 0, sizeof *d);
}

bool gitdata_load(GitData *out, const char *path, char *err, size_t err_size)
{
    memset(out, 0, sizeof *out);
    char errbuf[GG_ERR_MAX] = {0};
    char firstl[256];

    char *repo = repo_argument(path);
    if (!repo) {
        snprintf(err, err_size, "percorso del repository non valido");
        return false;
    }
    out->repo_path = repo;

    StrBuf so, se;
    sb_init(&so);
    sb_init(&se);
    int code = 0;

    /* 1. validazione: il percorso e' un repository git? */
    {
        const char *args[] = {"-C", repo, "rev-parse", "--git-dir", NULL};
        sb_clear(&so);
        sb_clear(&se);
        if (!run_git(args, &so, &se, &code, errbuf, sizeof errbuf)) {
            snprintf(err, err_size, "%s", errbuf);
            goto fail;
        }
        if (code != 0) {
            first_line(se.data, firstl, sizeof firstl);
            snprintf(err, err_size, "'%s' non e' un repository git valido: %s",
                     repo, firstl);
            goto fail;
        }
    }

    /* 2. branch corrente (assente se HEAD e' scollegata) */
    {
        const char *args[] = {"-C", repo, "symbolic-ref", "-q", "--short", "HEAD", NULL};
        sb_clear(&so);
        sb_clear(&se);
        if (!run_git(args, &so, &se, &code, errbuf, sizeof errbuf)) {
            snprintf(err, err_size, "%s", errbuf);
            goto fail;
        }
        if (code == 0 && so.len > 0) {
            strip_trailing_newlines(so.data);
            out->head_branch = xstrdup(so.data);
        }
    }

    /* 3. hash di HEAD (assente in repo senza commit) */
    {
        const char *args[] = {"-C", repo, "rev-parse", "--verify", "--quiet", "HEAD", NULL};
        sb_clear(&so);
        sb_clear(&se);
        if (!run_git(args, &so, &se, &code, errbuf, sizeof errbuf)) {
            snprintf(err, err_size, "%s", errbuf);
            goto fail;
        }
        if (code == 0 && so.len > 0) {
            strip_trailing_newlines(so.data);
            out->head_hash = xstrdup(so.data);
        }
    }

    /* 4. riferimenti */
    {
        const char *args[] = {
            "-C", repo, "for-each-ref",
            "--format=%(refname)%00%(symref)%00%(objecttype)%00%(objectname)%00%(*objectname)",
            "refs/heads", "refs/remotes", "refs/tags", NULL};
        sb_clear(&so);
        sb_clear(&se);
        if (!run_git(args, &so, &se, &code, errbuf, sizeof errbuf)) {
            snprintf(err, err_size, "%s", errbuf);
            goto fail;
        }
        if (code != 0) {
            first_line(se.data, firstl, sizeof firstl);
            snprintf(err, err_size, "git for-each-ref fallito: %s", firstl);
            goto fail;
        }
        if (!parse_refs(&so, out, err, err_size))
            goto fail;
    }

    /* 5. commit: git log --branches --tags --remotes HEAD (senza --all,
     *    che includerebbe refs/stash), ordinati topologicamente. */
    {
        const char *args[16];
        size_t n = 0;
        args[n++] = "--no-optional-locks";
        args[n++] = "-c";
        args[n++] = "i18n.logOutputEncoding=UTF-8";
        args[n++] = "-C";
        args[n++] = repo;
        args[n++] = "log";
        args[n++] = "--branches";
        args[n++] = "--tags";
        args[n++] = "--remotes";
        if (out->head_hash)
            args[n++] = "HEAD";
        args[n++] = "--topo-order";
        args[n++] = "-z";
        args[n++] = "--pretty=format:%H%x00%P%x00%an%x00%ct%x00%B";
        args[n] = NULL;

        sb_clear(&so);
        sb_clear(&se);
        if (!run_git(args, &so, &se, &code, errbuf, sizeof errbuf)) {
            snprintf(err, err_size, "%s", errbuf);
            goto fail;
        }
        if (code != 0) {
            first_line(se.data, firstl, sizeof firstl);
            snprintf(err, err_size, "git log fallito: %s", firstl);
            goto fail;
        }
        if (!parse_commits(&so, out, err, err_size))
            goto fail;
        build_indexes(out);
    }

    /* 6. marcatura HEAD sui branch locali */
    if (out->head_branch) {
        for (size_t i = 0; i < out->ref_count; i++) {
            GitRef *r = &out->refs[i];
            if (r->kind == REF_LOCAL_BRANCH && strcmp(r->name, out->head_branch) == 0)
                r->is_head = true;
        }
    }

    sb_free(&so);
    sb_free(&se);
    return true;

fail:
    sb_free(&so);
    sb_free(&se);
    free_partial(out);
    if (err && err_size)
        err[err_size - 1] = '\0';
    return false;
}

void gitdata_free(GitData *d)
{
    free_partial(d);
}

#else /* !_WIN32 */

#error "gitgraph: prototipo Windows-only (gitdata.c richiede Win32)"

#endif

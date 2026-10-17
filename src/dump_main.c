/* dump_main.c - strumento di ispezione testuale (fase 1).
 *
 * Uso: gitgraph_dump [--full] [--help] [percorso-repo]
 *   --full   stampa i messaggi di commit completi
 *
 * Stampa commit, riferimenti e modello dei branch (padre, stato, motivo).
 * Nessuna finestra grafica: serve ai test e alla verifica manuale.
 */
#include "util.h"
#include "config.h"
#include "gitdata.h"
#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

static void format_date(int64_t unix_time, char out[32])
{
    time_t t = (time_t)unix_time;
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    strftime(out, 32, "%Y-%m-%d %H:%MZ", &tmv);
}

static const char *state_name(ParentState st)
{
    switch (st) {
    case PARENT_CERTAIN:
        return "certo";
    case PARENT_CONFIGURED:
        return "configurato";
    case PARENT_DEDUCED:
        return "dedotto";
    case PARENT_AMBIGUOUS:
        return "ambiguo";
    default:
        return "nessuno";
    }
}

static const char *kind_name(RefKind k)
{
    switch (k) {
    case REF_LOCAL_BRANCH:
        return "branch";
    case REF_REMOTE_BRANCH:
        return "remote";
    default:
        return "tag";
    }
}

/* Prima riga del messaggio, ripulita. */
static void subject_line(const char *message, char *out, size_t out_size)
{
    size_t i = 0;
    while (message[i] && message[i] != '\n' && message[i] != '\r' && i + 1 < out_size) {
        out[i] = message[i];
        i++;
    }
    out[i] = '\0';
}

static void print_help(void)
{
    printf("uso: gitgraph_dump [--full] [--help] [percorso-repo]\n"
           "  --full   stampa i messaggi di commit completi\n"
           "  percorso-repo  default: directory corrente\n");
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const char *repo = NULL;
    bool full = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--full") == 0)
            full = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "opzione sconosciuta: %s\n", argv[i]);
            print_help();
            return 2;
        } else {
            repo = argv[i];
        }
    }

    char err[1024];
    GitData data;
    if (!gitdata_load(&data, repo, err, sizeof err)) {
        fprintf(stderr, "errore: %s\n", err);
        return 1;
    }

    /* configurazione: graph.cfg nella radice del repository, se presente;
     * altrimenti il solo tronco implicito (main/master) */
    {
        ConfigBranch *branches =
            xmalloc((data.ref_count ? data.ref_count : 1) * sizeof(ConfigBranch));
        size_t n = 0;
        for (size_t i = 0; i < data.ref_count; i++) {
            if (data.refs[i].kind == REF_TAG)
                continue;
            const char *name = data.refs[i].name;
            const char *canonical = name;
            if (data.refs[i].kind == REF_REMOTE_BRANCH) {
                const char *slash = strchr(name, '/');
                if (slash && slash[1])
                    canonical = slash + 1;
            }
            branches[n].name = name;
            branches[n].canonical = canonical;
            n++;
        }
        char *cfg_path = config_path_in_repo(data.repo_path);
        ConfigReport report;
        char cfg_err[1024];
        GraphConfig cfg;
        bool cfg_ok = config_load(&cfg, cfg_path, branches, n, &report, cfg_err,
                                  sizeof cfg_err);
        for (size_t i = 0; i < report.count; i++)
            fprintf(stderr, "%s: %s\n",
                    report.items[i].level == CONFIG_DIAG_ERROR ? "errore"
                                                                : "avviso",
                    report.items[i].text);
        if (!cfg_ok)
            fprintf(stderr, "%s: configurazione ignorata\n", cfg_err);
        free(cfg_path);
        config_report_free(&report);
        free(branches);

        History hist;
        if (!history_build(&hist, &data, &cfg, err, sizeof err)) {
            fprintf(stderr, "errore: %s\n", err);
            config_free(&cfg);
            gitdata_free(&data);
            return 1;
        }

        printf("repo: %s\n", data.repo_path);
        if (data.head_branch)
            printf("branch corrente: %s\n", data.head_branch);
        else
            printf("branch corrente: (HEAD scollegata)\n");
        printf("commit: %zu  riferimenti: %zu\n", data.commit_count, data.ref_count);
        if (cfg.long_lived_count > 0) {
            printf("long-lived (ordine di gerarchia):");
            for (size_t i = 0; i < cfg.long_lived_count; i++)
                printf(" %s", cfg.long_lived[i]);
            printf("%s\n", cfg.long_lived_explicit ? "" : "  (tronco implicito)");
        }
        for (size_t i = 0; i < cfg.override_count; i++)
            printf("override [parents]: %s = %s\n", cfg.overrides[i].branch,
                   cfg.overrides[i].parent);
        printf("\n");

        printf("RIFERIMENTI\n");
        for (size_t i = 0; i < data.ref_count; i++) {
            const GitRef *r = &data.refs[i];
            char sh[16];
            gitdata_short_hash(r->target, 8, sh);
            printf("  %-6s %-30s %s%s\n", kind_name(r->kind), r->name, sh,
                   r->is_head ? "  (HEAD)" : "");
        }
        printf("\n");

        printf("COMMIT (ordine topologico, 0 = piu' recente)\n");
        /* riferimenti per commit in O(commit + riferimenti): indici precalcolati */
        size_t ncommits = data.commit_count;
        size_t *ref_counts = xcalloc(ncommits ? ncommits : 1, sizeof(size_t));
        for (size_t ri = 0; ri < data.ref_count; ri++) {
            size_t ci = gitdata_find_commit(&data, data.refs[ri].target);
            if (ci != (size_t)-1)
                ref_counts[ci]++;
        }
        size_t *ref_off = xmalloc((ncommits + 1) * sizeof(size_t));
        ref_off[0] = 0;
        for (size_t i = 0; i < ncommits; i++)
            ref_off[i + 1] = ref_off[i] + ref_counts[i];
        size_t *ref_items = xmalloc((data.ref_count ? data.ref_count : 1) * sizeof(size_t));
        size_t *ref_cur = xmalloc((ncommits ? ncommits : 1) * sizeof(size_t));
        memcpy(ref_cur, ref_off, ncommits * sizeof(size_t));
        for (size_t ri = 0; ri < data.ref_count; ri++) {
            size_t ci = gitdata_find_commit(&data, data.refs[ri].target);
            if (ci != (size_t)-1)
                ref_items[ref_cur[ci]++] = ri;
        }
        size_t head_idx = data.head_hash ? gitdata_find_commit(&data, data.head_hash)
                                         : (size_t)-1;

        for (size_t i = 0; i < data.commit_count; i++) {
            const GitCommit *c = &data.commits[i];
            char sh[16], date[32], subj[96];
            gitdata_short_hash(c->hash, 8, sh);
            format_date(c->date, date);
            subject_line(c->message, subj, sizeof subj);
            printf("  %4zu  %s  genitori:", i, sh);
            if (c->parent_count == 0)
                printf(" (nessuno)");
            for (size_t k = 0; k < c->parent_count; k++) {
                char ph[16];
                gitdata_short_hash(c->parents[k], 8, ph);
                printf(" %s%s", ph, k == 0 ? "" : "");
            }
            printf("  %s  %-14.14s  %s", date, c->author, subj);
            /* riferimenti su questo commit */
            bool first = true;
            for (size_t k = ref_off[i]; k < ref_off[i + 1]; k++) {
                printf("%s[%s]", first ? "  " : " ", data.refs[ref_items[k]].name);
                first = false;
            }
            if (i == head_idx)
                printf("%sHEAD", first ? "  " : " ");
            printf("\n");
            if (full) {
                /* messaggio completo, indentato */
                const char *p = c->message;
                while (*p) {
                    const char *nl = strchr(p, '\n');
                    size_t len = nl ? (size_t)(nl - p) : strlen(p);
                    printf("         | %.*s\n", (int)len, p);
                    if (!nl)
                        break;
                    p = nl + 1;
                }
            }
        }
        free(ref_counts);
        free(ref_off);
        free(ref_items);
        free(ref_cur);
        printf("\n");

        printf("BRANCH (padre, stato, motivazione)\n");
        for (size_t i = 0; i < hist.branch_count; i++) {
            const BranchNode *b = &hist.branches[i];
            char fork[16] = "-";
            if (b->fork_hash)
                gitdata_short_hash(b->fork_hash, 8, fork);
            printf("  %-20s %s\n", b->name,
                   b->is_head ? "(HEAD)" : (b->mirror_of ? "(specchio)" : ""));
            char tip[16] = "-";
            if (b->tip_pos != (size_t)-1)
                gitdata_short_hash(data.commits[b->tip_pos].hash, 8, tip);
            printf("      tipo: %s%s   punta: %-8s padre: %-16s stato: %-11s fork: %s\n",
                   b->is_remote ? "remote" : "locale",
                   b->is_long_lived ? " long-lived" : "", tip,
                   b->parent ? b->parent : "-",
                   state_name(b->state), fork);
            if (b->first_own_commit) {
                char fh[16];
                gitdata_short_hash(b->first_own_commit, 8, fh);
                printf("      primo commit proprio: %s\n", fh);
            }
            if (b->alias_count > 0) {
                printf("      stessa punta di:");
                for (size_t k = 0; k < b->alias_count; k++)
                    printf(" %s", b->alias_members[k]);
                printf("\n");
            }
            if (b->genealogy_undetermined)
                printf("      genealogia: NON DETERMINABILE dalla storia\n");
            printf("      motivo: %s\n", b->motivation);
            if (b->candidate_count > 0) {
                printf("      candidati:");
                for (size_t k = 0; k < b->candidate_count; k++)
                    printf(" %s", b->candidates[k]);
                printf("\n");
            }
        }

        history_free(&hist);
        config_free(&cfg);
    }

    gitdata_free(&data);
    return 0;
}

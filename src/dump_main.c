/* dump_main.c - strumento di ispezione testuale.
 *
 * Uso: gitgraph_dump [--full] [--layout] [--summary] [--suggest-parents]
 *                    [--help] [percorso-repo]
 *   --full             stampa i messaggi di commit completi
 *   --layout           stampa solo la tabella dei commit (riga, corsia,
 *                      proprietario) e gli archi tra commit
 *   --summary          una riga di totali: commit, branch locali e remoti,
 *                      tag, corsie e rami per stato del padre
 *   --suggest-parents  per ogni branch ambiguo con un padre suggerito stampa
 *                      un commento con la motivazione e la riga "ramo = padre"
 *                      pronta per [parents] di graph.cfg (non scrive file)
 *
 * Senza --layout stampa commit, riferimenti, modello dei branch (padre, stato,
 * motivo) e la disposizione del grafo (corsie, righe, archi). Nessuna finestra
 * grafica: serve ai test e alla verifica manuale.
 */
#include "util.h"
#include "config.h"
#include "gitdata.h"
#include "history.h"
#include "layout.h"

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

static const char *cedge_kind_name(LayoutCommitEdgeKind k)
{
    switch (k) {
    case LAYOUT_CEDGE_FIRST_PARENT:
        return "primo genitore";
    case LAYOUT_CEDGE_MERGE_PARENT:
        return "genitore di merge";
    default:
        return "diramazione";
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
    printf("uso: gitgraph_dump [--full] [--layout] [--summary]\n"
           "                   [--suggest-parents] [--help] [percorso-repo]\n"
           "  --full     stampa i messaggi di commit completi\n"
           "  --layout   stampa la tabella dei commit (riga, corsia,\n"
           "             proprietario) e gli archi tra commit\n"
           "  --summary  una riga di totali (commit, branch locali/remoti,\n"
           "             tag, corsie, rami certi/dedotti/ambigui/configurati/\n"
           "             senza padre)\n"
           "  --suggest-parents  per ogni branch ambiguo con padre suggerito,\n"
           "             commento con la motivazione e riga per [parents]\n"
           "  percorso-repo  default: directory corrente\n");
}

/* Tabella dei commit: riga (posizione topologica), corsia del proprietario e
 * proprietario; i commit senza proprietario stanno nella corsia anonima. */
static void print_commit_layout(const GitData *data, const History *hist,
                                const Layout *lay)
{
    printf("\nCOMMIT LAYOUT (riga, corsia, proprietario)\n");
    printf("  %4s  %6s  %-9s  %s\n", "riga", "corsia", "commit", "proprietario");
    for (size_t ci = 0; ci < lay->commit_count; ci++) {
        const LayoutCommit *cell = &lay->commits[ci];
        char sh[16];
        gitdata_short_hash(data->commits[ci].hash, 8, sh);
        if (cell->owner == (size_t)-1)
            printf("  %4zu  %6zu  %-9s  -\n", cell->row, cell->lane, sh);
        else
            printf("  %4zu  %6zu  %-9s  %s\n", cell->row, cell->lane, sh,
                   hist->branches[cell->owner].name);
    }
    printf("\nARCHI DEI COMMIT (figlio -> genitore; tipo)\n");
    for (size_t e = 0; e < lay->cedge_count; e++) {
        const LayoutCommitEdge *ce = &lay->cedges[e];
        char a[16], b[16];
        gitdata_short_hash(data->commits[ce->from].hash, 8, a);
        gitdata_short_hash(data->commits[ce->to].hash, 8, b);
        printf("  %s -> %s  %s\n", a, b, cedge_kind_name(ce->kind));
    }
    printf("\nlayout: %zu commit, %zu corsie", lay->commit_count, lay->lanes);
    if (lay->anon_lane != (size_t)-1)
        printf(" (corsie anonime: %zu..%zu)", lay->anon_lane,
               lay->anon_lane + lay->anon_lane_count - 1);
    printf(", %zu archi tra commit\n", lay->cedge_count);
}

/* Una riga sola: i totali del repository e la ripartizione dei rami per stato
 * del padre. */
static void print_summary(const GitData *data, const History *hist,
                          const Layout *lay)
{
    size_t local = 0, remote = 0, tags = 0;
    size_t certain = 0, deduced = 0, ambiguous = 0, configured = 0, none = 0;
    for (size_t i = 0; i < hist->branch_count; i++) {
        if (hist->branches[i].is_remote)
            remote++;
        else
            local++;
        switch (hist->branches[i].state) {
        case PARENT_CERTAIN:
            certain++;
            break;
        case PARENT_DEDUCED:
            deduced++;
            break;
        case PARENT_AMBIGUOUS:
            ambiguous++;
            break;
        case PARENT_CONFIGURED:
            configured++;
            break;
        default:
            none++;
            break;
        }
    }
    for (size_t i = 0; i < data->ref_count; i++)
        if (data->refs[i].kind == REF_TAG)
            tags++;
    printf("riepilogo: %zu commit, %zu branch locali, %zu remoti, %zu tag, "
           "%zu corsie; rami: %zu certi, %zu dedotti, %zu ambigui, %zu "
           "configurati, %zu senza padre\n",
           data->commit_count, local, remote, tags, lay->lanes, certain, deduced,
           ambiguous, configured, none);
}

/* Per ogni branch ambiguo con un suggerimento: commento con la motivazione e
 * riga gia' pronta per la sezione [parents] di graph.cfg. Non scrive nulla. */
static void print_suggested_parents(const History *hist)
{
    for (size_t i = 0; i < hist->branch_count; i++) {
        const BranchNode *b = &hist->branches[i];
        if (b->state != PARENT_AMBIGUOUS || !b->suggested_parent)
            continue;
        printf("# %s: %s\n", b->name, b->motivation ? b->motivation : "-");
        printf("%s = %s\n", b->name, b->suggested_parent);
    }
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const char *repo = NULL;
    bool full = false;
    bool layout_only = false;
    bool summary = false;
    bool suggest = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--full") == 0)
            full = true;
        else if (strcmp(argv[i], "--layout") == 0)
            layout_only = true;
        else if (strcmp(argv[i], "--summary") == 0)
            summary = true;
        else if (strcmp(argv[i], "--suggest-parents") == 0)
            suggest = true;
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

        Layout lay;
        layout_build(&lay, &hist);

        if (summary) {
            print_summary(&data, &hist, &lay);
            layout_free(&lay);
            history_free(&hist);
            config_free(&cfg);
            gitdata_free(&data);
            return 0;
        }
        if (suggest) {
            print_suggested_parents(&hist);
            layout_free(&lay);
            history_free(&hist);
            config_free(&cfg);
            gitdata_free(&data);
            return 0;
        }

        printf("repo: %s\n", data.repo_path);
        if (data.head_branch)
            printf("branch corrente: %s\n", data.head_branch);
        else
            printf("branch corrente: (HEAD scollegata)\n");
        printf("commit: %zu  riferimenti: %zu\n", data.commit_count, data.ref_count);

        if (layout_only) {
            print_commit_layout(&data, &hist, &lay);
            layout_free(&lay);
            history_free(&hist);
            config_free(&cfg);
            gitdata_free(&data);
            return 0;
        }

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

        /* disposizione del grafo (fase 3): corsie e righe dei nodi, archi */
        printf("\nLAYOUT (branch: corsia, riga; corsia 0 = piu' a sinistra, "
               "riga 0 = commit piu' recente)\n");
        for (size_t i = 0; i < hist.branch_count; i++) {
            const LayoutNode *nd = &lay.nodes[i];
            if (nd->row == (size_t)-1)
                printf("  %-20s corsia %4zu  riga    -", hist.branches[i].name,
                       nd->lane);
            else
                printf("  %-20s corsia %4zu  riga %4zu", hist.branches[i].name,
                       nd->lane, nd->row);
            if (nd->label_of != (size_t)-1)
                printf("  (etichetta di %s)", hist.branches[nd->label_of].name);
            printf("\n");
        }
        if (lay.anon_lane != (size_t)-1)
            printf("  %-20s corsie %zu..%zu  (commit senza proprietario)\n",
                   "(blocco anonimo)", lay.anon_lane,
                   lay.anon_lane + lay.anon_lane_count - 1);
        printf("\nARCHI (figlio -> padre; tratteggiato = padre suggerito)\n");
        for (size_t e = 0; e < lay.edge_count; e++) {
            printf("  %-20s -> %-20s %s\n",
                   hist.branches[lay.edges[e].child].name,
                   hist.branches[lay.edges[e].parent].name,
                   lay.edges[e].kind == LAYOUT_EDGE_SUGGESTED ? "tratteggiato"
                                                              : "continuo");
        }
        printf("\nlayout: %zu corsie, %zu righe, %zu archi (%zu commit, %zu archi "
               "tra commit)\n",
               lay.lanes, lay.rows, lay.edge_count, lay.commit_count,
               lay.cedge_count);
        if (lay.anon_lane != (size_t)-1)
            printf("corsie anonime: %zu..%zu (%zu corsia%s, subito dopo i "
                   "tronchi)\n",
                   lay.anon_lane, lay.anon_lane + lay.anon_lane_count - 1,
                   lay.anon_lane_count, lay.anon_lane_count == 1 ? "" : "e");
        layout_free(&lay);

        history_free(&hist);
        config_free(&cfg);
    }

    gitdata_free(&data);
    return 0;
}

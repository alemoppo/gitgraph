/* make_scale_repo.c - genera su stdout uno stream per `git fast-import`.
 *
 * Uso: make_scale_repo <commit_totali> <ogni_n_feature> <ogni_n_sub> [epoca]
 *
 * Struttura generata (deterministica, nessun file: solo commit vuoti):
 *  - commit lineari su refs/heads/main;
 *  - ogni `ogni_n_feature` commit di main nasce refs/heads/feature/<i>;
 *  - ogni `ogni_n_sub` feature nasce refs/heads/sub/<i>, mergiato subito
 *    nel feature con un commit di merge (--no-ff);
 *  - ogni due feature, la seconda viene mergiata in main (--no-ff).
 * Tutti i ref restano: il numero di riferimenti cresce con i commit.
 *
 * Autore/committer fissi e date crescenti: lo stream e' ripetibile e gli
 * hash sono deterministici a parita' di parametri.
 *
 * Esempio: make_scale_repo 10000 30 5 | git -C repo fast-import
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static unsigned long long marks = 0;
static long long timestamp = 1700000000;

static unsigned long long next_mark(void)
{
    return ++marks;
}

/* Scrive un commit; from=0 nessun genitore; merge=0 nessun secondo genitore.
 * Ritorna il mark del commit. */
static unsigned long long put_commit(const char *ref, unsigned long long from,
                                     unsigned long long merge, const char *msg)
{
    unsigned long long m = next_mark();
    printf("commit %s\n", ref);
    printf("mark :%llu\n", m);
    printf("committer Scale Repo <scale@example.com> %lld +0000\n", timestamp++);
    printf("data %zu\n%s\n", strlen(msg), msg);
    if (from)
        printf("from :%llu\n", from);
    if (merge)
        printf("merge :%llu\n", merge);
    return m;
}

int main(int argc, char **argv)
{
    if (argc < 4 || argc > 5) {
        fprintf(stderr,
                "uso: make_scale_repo <commit_totali> <ogni_n_feature> <ogni_n_sub> [epoca]\n");
        return 2;
    }
    unsigned long long total = strtoull(argv[1], NULL, 10);
    unsigned long feature_every = (unsigned long)strtoul(argv[2], NULL, 10);
    unsigned long sub_every = (unsigned long)strtoul(argv[3], NULL, 10);
    if (argc == 5)
        timestamp = strtoll(argv[4], NULL, 10);
    if (total == 0 || feature_every < 2 || sub_every == 0) {
        fprintf(stderr, "parametri non validi\n");
        return 2;
    }

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    unsigned long long made = 0;
    unsigned long long main_mark = 0;
    unsigned long long since_feature = 0;
    unsigned long feature_index = 0;

    printf("# gitgraph: repo sintetico per test di scala\n");
    while (made < total) {
        char msg[64];
        snprintf(msg, sizeof msg, "main %llu", main_mark + 1);
        main_mark = put_commit("refs/heads/main", main_mark, 0, msg);
        made++;
        since_feature++;
        if (since_feature < feature_every)
            continue;
        since_feature = 0;

        unsigned long fi = feature_index++;
        char fref[64], sref[80];
        snprintf(fref, sizeof fref, "refs/heads/feature/%lu", fi);

        snprintf(msg, sizeof msg, "feature %lu: inizio", fi);
        unsigned long long fm = put_commit(fref, main_mark, 0, msg);
        made++;
        snprintf(msg, sizeof msg, "feature %lu: lavoro", fi);
        fm = put_commit(fref, fm, 0, msg);
        made++;

        if ((fi + 1) % sub_every == 0) {
            snprintf(sref, sizeof sref, "refs/heads/sub/%lu", fi);
            snprintf(msg, sizeof msg, "sub %lu: inizio", fi);
            unsigned long long sm = put_commit(sref, fm, 0, msg);
            made++;
            snprintf(msg, sizeof msg, "sub %lu: lavoro", fi);
            sm = put_commit(sref, sm, 0, msg);
            made++;
            snprintf(msg, sizeof msg, "feature %lu: merge del sub", fi);
            fm = put_commit(fref, fm, sm, msg);
            made++;
        }

        if (fi % 2 == 1) {
            snprintf(msg, sizeof msg, "main: merge di feature %lu", fi);
            main_mark = put_commit("refs/heads/main", main_mark, fm, msg);
            made++;
        }
    }
    printf("done\n");
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "scrittura dello stream fallita\n");
        return 1;
    }
    return 0;
}

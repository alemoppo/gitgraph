/* test_layout.c - test automatici della disposizione del grafo (fase 3).
 *
 * Uso: test_layout <percorso-repo-di-test>
 *
 * Verifica, sul repository di prova, che la disposizione sia coerente col
 * modello e non dipenda dai nomi:
 *  - un nodo per branch, con la riga della punta e la corsia della famiglia;
 *  - il tronco (long-lived) nella corsia 0, i long-lived nelle prime corsie;
 *  - i riferimenti sulla stessa punta (specchio/alias) nella stessa corsia;
 *  - un arco tratteggiato verso il padre suggerito dei casi ambigui noti
 *    (branch1 -> main, branch2 -> branch1);
 *  - corsie contigue 0..lanes-1, tante quante le famiglie;
 *  - determinismo: due costruzioni successive identiche.
 *
 * La proprieta' condivisa gg_check_layout ripete le stesse regole su ogni
 * modello (anche in test_core, test_config, test_scale, test_invariance).
 *
 * Esce con 0 se tutti i test passano, 1 se qualcuno fallisce, 2 su errore di
 * preparazione.
 */
#include "config.h"
#include "gitdata.h"
#include "history.h"
#include "layout.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define REQUIRE(cond, msg)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "PREREQUISITO FALLITO: %s\n", msg);         \
            return 2;                                                   \
        }                                                               \
    } while (0)

/* proprieta' condivisa: serve la macro CHECK definita sopra */
#include "gg_checks.h"

static const LayoutNode *node_named(const History *h, const Layout *l,
                                    const char *name)
{
    size_t i = history_find_branch(h, name);
    return i == (size_t)-1 ? NULL : &l->nodes[i];
}

static bool edge_exists(const Layout *l, size_t child, size_t parent,
                        LayoutEdgeKind kind)
{
    for (size_t e = 0; e < l->edge_count; e++)
        if (l->edges[e].child == child && l->edges[e].parent == parent &&
            l->edges[e].kind == kind)
            return true;
    return false;
}

static size_t distinct_families(const History *h)
{
    size_t count = 0;
    for (size_t i = 0; i < h->branch_count; i++) {
        size_t tip = h->branches[i].tip_pos;
        bool gia = false;
        if (tip != (size_t)-1) {
            for (size_t k = 0; k < i && !gia; k++)
                if (h->branches[k].tip_pos == tip)
                    gia = true;
        }
        if (!gia)
            count++;
    }
    return count;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) {
        fprintf(stderr, "uso: test_layout <percorso-repo-di-test>\n");
        return 2;
    }

    GitData data;
    char err[1024];
    if (!gitdata_load(&data, argv[1], err, sizeof err)) {
        fprintf(stderr, "PREREQUISITO FALLITO: %s\n", err);
        return 2;
    }

    /* configurazione implicita: tronco main */
    char **names = xmalloc((data.ref_count ? data.ref_count : 1) * sizeof(char *));
    size_t nn = 0;
    for (size_t i = 0; i < data.ref_count; i++)
        if (data.refs[i].kind == REF_LOCAL_BRANCH)
            names[nn++] = data.refs[i].name;
    GraphConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    config_set_implicit_trunk(&cfg, names, nn);
    free(names);

    History hist;
    REQUIRE(history_build(&hist, &data, &cfg, err, sizeof err),
            "history_build fallita");

    Layout lay;
    layout_build(&lay, &hist);
    gg_check_layout(&lay, &hist, "fixture principale");

    /* forma generale */
    CHECK(lay.node_count == hist.branch_count,
          "nodi = %zu, attesi %zu (un nodo per branch)", lay.node_count,
          hist.branch_count);
    CHECK(lay.rows == data.commit_count, "righe = %zu, commit = %zu", lay.rows,
          data.commit_count);
    CHECK(lay.lanes == distinct_families(&hist),
          "corsie = %zu, famiglie (punte distinte) = %zu", lay.lanes,
          distinct_families(&hist));

    /* corsie contigue 0..lanes-1, tutte usate */
    if (lay.lanes > 0) {
        unsigned char *usata = xcalloc(lay.lanes, 1);
        for (size_t i = 0; i < lay.node_count; i++) {
            CHECK(lay.nodes[i].lane < lay.lanes, "'%s' corsia fuori intervallo",
                  hist.branches[i].name);
            if (lay.nodes[i].lane < lay.lanes)
                usata[lay.nodes[i].lane] = 1;
        }
        for (size_t k = 0; k < lay.lanes; k++)
            CHECK(usata[k], "corsia %zu non usata da nessun branch", k);
        free(usata);
    }

    /* il tronco (main, long-lived, primo in [long-lived]) e' la corsia 0 */
    const LayoutNode *mn = node_named(&hist, &lay, "main");
    REQUIRE(mn != NULL, "branch main assente");
    CHECK(mn->lane == 0, "main: corsia %zu, attesa 0 (tronco)", mn->lane);

    /* i long-lived occupano le prime corsie, una per rank (qui solo main, ma la
     * regola resta: la corsia di un long-lived e' il suo rango) */
    for (size_t i = 0; i < hist.branch_count; i++) {
        const BranchNode *b = &hist.branches[i];
        if (!b->is_long_lived || b->long_lived_rank < 0)
            continue;
        CHECK(lay.nodes[i].lane == (size_t)b->long_lived_rank,
              "'%s' long-lived: corsia %zu, atteso il rango %d", b->name,
              lay.nodes[i].lane, b->long_lived_rank);
    }

    /* i riferimenti sulla stessa punta (specchio/alias) condividono la
     * posizione: origin/branch1 e' specchio provato di branch1 */
    const LayoutNode *b1 = node_named(&hist, &lay, "branch1");
    const LayoutNode *ob1 = node_named(&hist, &lay, "origin/branch1");
    REQUIRE(b1 != NULL, "branch1 assente");
    REQUIRE(ob1 != NULL, "origin/branch1 assente");
    CHECK(b1->lane == ob1->lane && b1->row == ob1->row,
          "origin/branch1 (specchio) in posizione diversa da branch1: "
          "(%zu,%zu) vs (%zu,%zu)",
          ob1->lane, ob1->row, b1->lane, b1->row);

    /* archi tratteggiati dei casi ambigui noti */
    size_t bi = history_find_branch(&hist, "branch1");
    size_t mi = history_find_branch(&hist, "main");
    const BranchNode *b1n = &hist.branches[bi];
    REQUIRE(b1n->suggested_parent != NULL &&
                strcmp(b1n->suggested_parent, "main") == 0,
            "branch1: suggerito atteso main");
    CHECK(edge_exists(&lay, bi, mi, LAYOUT_EDGE_SUGGESTED),
          "branch1: manca l'arco tratteggiato verso main (il suo suggerito)");
    CHECK(!edge_exists(&lay, bi, mi, LAYOUT_EDGE_PARENT),
          "branch1: arco continuo verso main mentre il padre e' solo suggerito");

    size_t b2i = history_find_branch(&hist, "branch2");
    REQUIRE(b2i != (size_t)-1, "branch2 assente");
    REQUIRE(hist.branches[b2i].suggested_parent != NULL &&
                strcmp(hist.branches[b2i].suggested_parent, "branch1") == 0,
            "branch2: suggerito atteso branch1");
    CHECK(edge_exists(&lay, b2i, bi, LAYOUT_EDGE_SUGGESTED),
          "branch2: manca l'arco tratteggiato verso branch1");

    /* determinismo: due costruzioni identiche */
    Layout lay2;
    layout_build(&lay2, &hist);
    CHECK(lay2.node_count == lay.node_count && lay2.edge_count == lay.edge_count &&
              lay2.lanes == lay.lanes && lay2.rows == lay.rows,
          "la disposizione non e' deterministica (dimensioni)");
    for (size_t i = 0; i < lay.node_count && i < lay2.node_count; i++)
        CHECK(lay.nodes[i].branch == lay2.nodes[i].branch &&
                  lay.nodes[i].lane == lay2.nodes[i].lane &&
                  lay.nodes[i].row == lay2.nodes[i].row,
              "la disposizione non e' deterministica al nodo %zu", i);
    for (size_t e = 0; e < lay.edge_count && e < lay2.edge_count; e++)
        CHECK(lay.edges[e].child == lay2.edges[e].child &&
                  lay.edges[e].parent == lay2.edges[e].parent &&
                  lay.edges[e].kind == lay2.edges[e].kind,
              "la disposizione non e' deterministica all'arco %zu", e);
    layout_free(&lay2);

    printf("layout: %zu branch, %zu corsie, %zu righe, %zu archi\n",
           lay.node_count, lay.lanes, lay.rows, lay.edge_count);

    layout_free(&lay);
    history_free(&hist);
    config_free(&cfg);
    gitdata_free(&data);

    printf("%d test superati, %d falliti\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}

/* history.c - vedi history.h
 *
 * Euristica (documentata, deterministica). Tutto lavora su indici di commit
 * precalcolati una sola volta: nessuna scansione di hash nei passi interni.
 *
 * 1. Riferimenti e famiglie. Ogni riferimento (locale o remoto) e' un nodo e
 *    conserva la sua punta reale: due riferimenti con lo stesso nome canonico
 *    ma punte diverse NON vengono fusi, e non si dichiara alcuno specchio.Uno
 *    specchio si ha solo quando un remoto punta allo stesso commit di un
 *    branch locale: e' un fatto ricavabile da Git. I riferimenti che puntano
 *    allo stesso commit formano una famiglia: stessa genealogia, quindi stesso
 *    padre dedotto; resta ambiguo solo il nome da usare fra loro.
 *
 * 2. Appartenenza dei commit. Le famiglie, nell'ordine deterministico
 *    (long-lived prima, poi nome), rivendicano i commit della propria catena
 *    dei primi genitori non ancora rivendicati: ogni commit appartiene alla
 *    "regione propria" della prima famiglia che lo attraversa. La scelta fra
 *    diramazioni sorelle (stesso punto di partenza) e' arbitraria ma stabile:
 *    la storia non registra quale sia nata prima.
 *
 * 3. Punto di diramazione f(b): il primo commit che, risalendo la catena dei
 *    primi genitori dalla punta di b, appartiene alla regione propria di
 *    un'altra famiglia. Non e' la raggiungibilita' generica: dopo un merge i
 *    commit di b sono raggiungibili da altri, ma la catena dei primi genitori
 *    non li attraversa, quindi il fork resta quello vero.
 *
 * 4. Primo commit proprio: il piu' vecchio commit rivendicato da b sulla
 *    catena (il figlio di f(b)); assente se la punta e' gia' rivendicata da
 *    altri (branch assorbito o fast-forward).
 *
 * 5. Candidati padre b di b, con filtri O(1) grazie alla matrice di relazioni
 *    precalcolata sulle catene dei primi genitori:
 *      D1  la punta di b giace sulla catena di b          -> b discende da b;
 *      D2  f(b) sta sulla catena di b, piu' in alto di f(b) -> b e' figlio;
 *      D3  esistenza: f(b) sta sotto f(b) sulla catena di b -> b non esisteva;
 *      D4  assorbito: b senza commit propri e punta su un'altra catena, e non
 *          proprio il punto di diramazione -> escluso (FF/alias non fanno da
 *          padre); i long-lived sono esentati (il tronco esiste da sempre);
 *      D5  fork uguali: si usa la data del primo commit proprio come
 *          spareggio quando la struttura non decide (scarto del candidato
 *          piu' recente di b); la motivazione lo dichiara sempre.
 *      D6  contenimento: f(b) deve giacere sulla catena di b.
 *
 * 6. Scelta del padre: se un solo candidato possiede il punto di diramazione
 *    (cioe' quel commit e' nella sua regione propria) e' il padre; se c'e' un
 *    solo candidato e' il padre; altrimenti l'unico candidato a monte di tutti
 *    gli altri; altrimenti ambiguo. Nessun candidato -> fallback: contenitori
 *    della punta (raggiungibilita'), altrimenti nessun padre e la genealogia
 *    viene dichiarata non determinabile. I long-lived non hanno padre, fork ne'
 *    candidati.
 *
 * 7. Complessita'. Siano C i commit, R i riferimenti, G <= R le famiglie e
 *    L_g la lunghezza della catena dei primi genitori della famiglia g.
 *    Precalcolo: O(C + genitori) per gli indici (in gitdata), O(S L_g) per
 *    l'appartenenza dei commit e i punti di diramazione, O(S L_g + GÂ²) per la
 *    matrice di relazioni, O(S |raggiungibili|) per i bitset di
 *    raggiungibilita'. Deduzione: una sola passata O(GÂ² + S L_g). Il controllo
 *    dei cicli usa una mappa nome->nodo: O(R) per gli aciclici. Nessun
 *    percorso viene rifatto per coppia di branch e nulla viene riallocato per
 *    commit: il costo cresce linearmente con il repository.
 */
#include "history.h"

#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NO_POS ((size_t)-1)

typedef struct {
    size_t *nodes;       /* riferimenti con la stessa punta */
    size_t node_count;
    size_t rep_node;     /* rappresentante: preferisce un nodo locale, poi
                            il primo in ordine di nome */
    size_t tip_idx;
    int ll_rank;         /* rank minimo dei membri long-lived, -1 se nessuno */
    bool trunk;           /* il rappresentante e' long-lived: famiglia di tronco */
    size_t f;            /* punto di diramazione, NO_POS se assente */
    size_t fork_dec;     /* punto di diramazione usato per la scelta del padre */
    size_t first_own;    /* primo commit proprio, NO_POS se assente */
    size_t *cand;        /* indici famiglia candidati */
    size_t cand_count;
    BitSet *inside;      /* famiglie la cui catena contiene la sua punta */
    BitSet *reach;       /* commit raggiungibili dalla punta */
} Family;

typedef struct {
    const GitData *d;
    const GraphConfig *cfg;
    History *out;
    Family *families;
    size_t family_count;
    size_t *node_family;    /* nodo -> famiglia */
    size_t *family_order;   /* famiglie in ordine deterministico */
    size_t *nchain;         /* commit -> quante famiglie lo hanno in catena */
    size_t *list_start;     /* commit -> inizio in list_items */
    size_t *list_count;     /* commit -> quante famiglie lo hanno in catena */
    size_t *list_fill;      /* uso interno durante il riempimento */
    size_t *list_items;     /* famiglie per commit, blocchi contigui */
    size_t *fork_head;      /* commit -> prima famiglia che si stacca li' */
    size_t *tip_at;         /* commit -> famiglia la cui punta e' questo commit */
    size_t *trunk_at;       /* commit -> tronco che lo possiede (NO_POS se nessuno) */
    uint32_t *mark_gen;     /* generazione del marchio sulla catena corrente */
    uint32_t mark;
    size_t *fork_next;      /* famiglia -> successiva con lo stesso fork */

    size_t *stack;          /* buffer riutilizzato per i DFS e i cammini */
    StrMap *name_map;       /* nome nodo -> (indice nodo + 1) */
} Ctx;

/* ---------- helper su commit ---------- */

static const GitCommit *commit_at(const Ctx *ctx, size_t idx)
{
    if (idx == NO_POS || idx >= ctx->d->commit_count)
        return NULL;
    return &ctx->d->commits[idx];
}

/* Primo genitore (indice), oppure NO_POS. */
static size_t first_parent(const Ctx *ctx, size_t idx)
{
    const GitCommit *c = commit_at(ctx, idx);
    if (!c || c->parent_count == 0)
        return NO_POS;
    return c->parent_idx[0];
}

static int64_t date_at(const Ctx *ctx, size_t idx)
{
    const GitCommit *c = commit_at(ctx, idx);
    return c ? c->date : 0;
}

static const BranchNode *node_at(const Ctx *ctx, size_t i)
{
    return &ctx->out->branches[i];
}

static int cmp_str_ptr(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* ---------- ordine deterministico ---------- */

/* Chiave di ordinamento: long-lived prima (nell'ordine della configurazione),
 * poi per nome. */
typedef struct {
    size_t idx;
    int rank;          /* -1 se non long-lived */
    const char *name;
} OrderKey;

static int cmp_order_key(const void *A, const void *B)
{
    const OrderKey *a = A, *b = B;
    bool la = a->rank >= 0, lb = b->rank >= 0;
    if (la != lb)
        return la ? -1 : 1;
    if (la && a->rank != b->rank)
        return a->rank < b->rank ? -1 : 1;
    return strcmp(a->name, b->name);
}

/* ---------- nodi ---------- */

static size_t add_node(History *out, const GitRef *r, const char *canonical)
{
    out->branches = xrealloc(out->branches, (out->branch_count + 1) * sizeof(BranchNode));
    BranchNode *n = &out->branches[out->branch_count];
    memset(n, 0, sizeof *n);
    n->name = xstrdup(r->name);
    n->canonical = xstrdup(canonical);
    n->ref_index = (size_t)(r - out->data->refs);
    n->is_remote = (r->kind == REF_REMOTE_BRANCH);
    n->is_head = r->is_head;
    n->long_lived_rank = -1;
    n->fork_pos = NO_POS;
    n->first_own_pos = NO_POS;
    n->tip_pos = NO_POS;
    return out->branch_count++;
}

static void set_parent(BranchNode *n, const char *parent, ParentState st,
                       const char *motivation)
{
    free(n->parent);
    free(n->motivation);
    n->parent = parent ? xstrdup(parent) : NULL;
    n->state = st;
    n->motivation = xstrdup(motivation);
    n->genealogy_undetermined = false;
}

/* Padre suggerito fra i candidati (stato ambiguo): niente padre dedotto, solo
 * un'indicazione con la sua base, cosi' il layout puo' tratteggiarla. */
static void set_suggestion(BranchNode *n, const char *suggested, SuggestionBasis basis)
{
    free(n->suggested_parent);
    n->suggested_parent = suggested ? xstrdup(suggested) : NULL;
    n->suggestion_basis = basis;
}

static void set_fork(BranchNode *n, const Ctx *ctx, size_t fork_idx)
{
    free(n->fork_hash);
    n->fork_hash = NULL;
    n->fork_pos = NO_POS;
    const GitCommit *c = commit_at(ctx, fork_idx);
    if (c) {
        n->fork_hash = xstrdup(c->hash);
        n->fork_pos = fork_idx;
    }
}

static void set_first_own(BranchNode *n, const Ctx *ctx, size_t idx)
{
    free(n->first_own_commit);
    n->first_own_commit = NULL;
    n->first_own_pos = NO_POS;
    const GitCommit *c = commit_at(ctx, idx);
    if (c) {
        n->first_own_commit = xstrdup(c->hash);
        n->first_own_pos = idx;
    }
}

static void clear_lineage(BranchNode *n)
{
    free(n->fork_hash);
    n->fork_hash = NULL;
    n->fork_pos = NO_POS;
    free(n->first_own_commit);
    n->first_own_commit = NULL;
    n->first_own_pos = NO_POS;
}

static void clear_candidates(BranchNode *n)
{
    for (size_t k = 0; k < n->candidate_count; k++)
        free(n->candidates[k]);
    free(n->candidates);
    n->candidates = NULL;
    n->candidate_count = 0;
}

/* ---------- costruzione di nodi, specchi e famiglie ---------- */

static void family_free(Family *f)
{
    free(f->nodes);
    free(f->cand);
    bitset_free(f->inside);
    bitset_free(f->reach);
}

static void create_nodes(Ctx *ctx, const GitData *data)
{
    History *out = ctx->out;
    for (size_t i = 0; i < data->ref_count; i++) {
        const GitRef *r = &data->refs[i];
        if (r->kind == REF_TAG)
            continue;
        const char *canonical = r->name;
        if (r->kind == REF_REMOTE_BRANCH) {
            const char *slash = strchr(r->name, '/');
            if (slash && slash[1])
                canonical = slash + 1;
        }
        size_t node = add_node(out, r, canonical);
        strmap_put(ctx->name_map, out->branches[node].name,
                   (void *)(uintptr_t)(node + 1));
        /* la punta reale del riferimento, sempre conservata */
        out->branches[node].tip_pos = gitdata_find_commit(data, r->target);
    }
}

/* Punta reale, rango long-lived e specchi. Lo specchio richiede lo STESSO
 * commit: un remoto che ha solo lo stesso nome canonico ma una punta diversa
 * resta un riferimento autonomo (la relazione di tracking non e' deducibile
 * dai soli nomi) e non eredita il rango long-lived del branch locale. */
static void finalize_nodes(Ctx *ctx)
{
    History *out = ctx->out;
    StrMap *locals = strmap_new();
    for (size_t i = 0; i < out->branch_count; i++) {
        BranchNode *n = &out->branches[i];
        if (!n->is_remote)
            strmap_put(locals, n->canonical, (void *)(uintptr_t)(i + 1));
    }
    for (size_t i = 0; i < out->branch_count; i++) {
        BranchNode *n = &out->branches[i];
        n->long_lived_rank = config_long_lived_rank(ctx->cfg, n->name);
        if (n->is_remote) {
            void *v = strmap_get(locals, n->canonical);
            const BranchNode *ln = v ? node_at(ctx, (size_t)(uintptr_t)v - 1) : NULL;
            if (ln && ln->tip_pos != NO_POS && ln->tip_pos == n->tip_pos) {
                /* specchio provato: stesso nome canonico e stesso commit */
                n->mirror_of = xstrdup(ln->name);
                if (n->long_lived_rank < 0)
                    n->long_lived_rank = ln->long_lived_rank;
            } else if (!ln && n->long_lived_rank < 0) {
                /* esiste solo il riferimento remoto: vale il nome canonico */
                n->long_lived_rank = config_long_lived_rank(ctx->cfg, n->canonical);
            }
            /* ramo locale presente ma con punta diversa: nessuna relazione di
             * tracking e' deducibile dai soli nomi, quindi il remoto non
             * eredita il ruolo di tronco e mantiene la genealogia dedotta */
        }
        n->is_long_lived = (n->long_lived_rank >= 0);
    }
    strmap_free(locals);
}

/* Famiglie: riferimenti con la stessa punta. */
static void build_families(Ctx *ctx)
{
    size_t n = ctx->d->commit_count;
    size_t b = ctx->out->branch_count;
    ctx->node_family = xmalloc((b ? b : 1) * sizeof(size_t));
    ctx->families = xcalloc(b ? b : 1, sizeof(Family));
    ctx->family_count = 0;

    size_t *fam_of_tip = xmalloc((n ? n : 1) * sizeof(size_t));
    for (size_t i = 0; i < n; i++)
        fam_of_tip[i] = NO_POS;

    for (size_t i = 0; i < b; i++) {
        size_t tip = ctx->out->branches[i].tip_pos;
        size_t fi;
        if (tip != NO_POS) {
            fi = fam_of_tip[tip];
            if (fi == NO_POS) {
                fi = ctx->family_count++;
                ctx->families[fi].tip_idx = tip;
                fam_of_tip[tip] = fi;
            }
        } else {
            fi = ctx->family_count++; /* punta fuori dal log: famiglia isolata */
            ctx->families[fi].tip_idx = NO_POS;
        }
        Family *fam = &ctx->families[fi];
        fam->nodes = xrealloc(fam->nodes, (fam->node_count + 1) * sizeof(size_t));
        fam->nodes[fam->node_count++] = i;
        ctx->node_family[i] = fi;
    }
    free(fam_of_tip);

    for (size_t fi = 0; fi < ctx->family_count; fi++) {
        Family *fam = &ctx->families[fi];
        fam->rep_node = fam->nodes[0];
        for (size_t k = 0; k < fam->node_count; k++) {
            size_t node = fam->nodes[k];
            if (!node_at(ctx, node)->is_remote && node_at(ctx, fam->rep_node)->is_remote)
                fam->rep_node = node; /* il rappresentante e' il branch locale */
        }
        /* Il tronco e' deciso dal rappresentante: una famiglia che contiene un
         * long-lived "in coda" (per esempio uno specchio remoto) nonPromuove
         * il branch locale a tronco. */
        const BranchNode *rep = node_at(ctx, fam->rep_node);
        fam->ll_rank = rep->long_lived_rank;
        fam->trunk = rep->is_long_lived;
        fam->f = NO_POS;
        fam->fork_dec = NO_POS;
        fam->first_own = NO_POS;
    }
}

static void build_family_order(Ctx *ctx)
{
    size_t n = ctx->family_count;
    ctx->family_order = xmalloc((n ? n : 1) * sizeof(size_t));
    OrderKey *keys = xmalloc((n ? n : 1) * sizeof(OrderKey));
    for (size_t i = 0; i < n; i++) {
        keys[i].idx = i;
        keys[i].rank = ctx->families[i].ll_rank;
        keys[i].name = ctx->out->branches[ctx->families[i].rep_node].name;
    }
    qsort(keys, n, sizeof(OrderKey), cmp_order_key);
    for (size_t i = 0; i < n; i++)
        ctx->family_order[i] = keys[i].idx;
    free(keys);
}

/* ---------- precalcoli sulle catene dei primi genitori ---------- */

/* Punti di diramazione, regioni proprie e parenti: tutto dalla sola struttura.
 *
 * Nessuna regola dipende dall'ordine di visita delle famiglie: i nomi non
 * decidono niente (l'unico ordinamento rimasto, quello dei tronchi, e' quello
 * esplicito di [long-lived] in graph.cfg).
 *
 * - nchain[c]: quante famiglie hanno il commit c sulla catena dei primi
 *   genitori. Un commit su due o piu' catene non-tronco, senza un tronco che
 *   lo contenga, e' condiviso: non ha un proprietario.
 * - f(b): per una famiglia non-tronco, il primo commit scendendo dalla catena
 *   di b che e' condiviso oppure che e' la punta di un'altra famiglia (D1, D2,
 *   D6 della vecchia euristica valgono ancora, ma valgono per definizione e non
 *   per il confronto con chi e' passato prima). I tronchi non hanno un punto di
 *   diramazione: sono la base.
 * - first_own(b): il primo commit proprio di b, cioe' il primo scendendo che
 *   nessun'altra famiglia raggiunge; se la famiglia non ha commit proprio
 *   (fast-forward, specchio lontano, alias) non ha nemmeno una base: fork
 *   assente, resta il fallback sui contenitori.
 * - figli: X e' figlio di b se f(X) sta sulla catena di b e X e' piu' recente
 *   di b. E' la relazione che evita di scambiare un ramo con il proprio figlio
 *   e risolve il caso dei due rami nati dallo stesso commit (il piu' vecchio
 *   dei due e' il padre dell'altro).
 *
 * Il resto (quali famiglie stanno su un commit, quindi i candidati) e' costruito
 * con due passate sulle catene: la prima conta, la seconda riempisce.
 */
static void compute_forks(Ctx *ctx)
{
    size_t n = ctx->d->commit_count;
    size_t g = ctx->family_count;
    ctx->nchain = xcalloc(n ? n : 1, sizeof(size_t));

    /* 1. quante famiglie raggiungono ogni commit */
    for (size_t fi = 0; fi < g; fi++) {
        for (size_t cur = ctx->families[fi].tip_idx; cur != NO_POS; cur = first_parent(ctx, cur))
            ctx->nchain[cur]++;
    }

    /* 1b. il tronco che possiede ciascun commit: un long-lived possiede tutto
     * quello che incontra, senza condizioni, e fra due tronchi vale il primo
     * nell'ordine della configurazione (quello esplicito in graph.cfg) */
    ctx->trunk_at = xmalloc((n ? n : 1) * sizeof(size_t));
    for (size_t i = 0; i < n; i++)
        ctx->trunk_at[i] = NO_POS;
    for (size_t oi = 0; oi < g; oi++) {
        size_t fi = ctx->family_order[oi];
        if (!ctx->families[fi].trunk)
            continue;
        for (size_t cur = ctx->families[fi].tip_idx; cur != NO_POS; cur = first_parent(ctx, cur)) {
            if (ctx->trunk_at[cur] == NO_POS)
                ctx->trunk_at[cur] = fi;
        }
    }

    /* 2. punto di diramazione e primo commit proprio */
    for (size_t fi = 0; fi < g; fi++) {
        Family *fam = &ctx->families[fi];
        fam->f = NO_POS;
        fam->first_own = NO_POS;
        if (fam->tip_idx == NO_POS || fam->trunk)
            continue; /* un tronco e' la base: niente padre da dedurre */
        for (size_t cur = fam->tip_idx; cur != NO_POS; cur = first_parent(ctx, cur)) {
            if (ctx->nchain[cur] >= 2) {
                fam->f = cur;
                break;
            }
        }
    }
}

/* Raggiungibilita' (qualsiasi catena) della punta di una famiglia. */
static void reach_build(Ctx *ctx, size_t fi, BitSet *set)
{
    size_t tip = ctx->families[fi].tip_idx;
    if (tip == NO_POS)
        return;
    size_t sp = 0;
    ctx->stack[sp++] = tip;
    bitset_set(set, tip);
    while (sp > 0) {
        size_t cur = ctx->stack[--sp];
        const GitCommit *c = &ctx->d->commits[cur];
        for (size_t k = 0; k < c->parent_count; k++) {
            size_t p = c->parent_idx[k];
            if (p == NO_POS || bitset_test(set, p))
                continue;
            bitset_set(set, p);
            ctx->stack[sp++] = p;
        }
    }
}
/* Famiglie che si staccano da ciascun commit (liste corte: una famiglia ha un
 * solo punto di diramazione, quindi ogni lista ha al massimo G elementi). */
static void build_fork_index(Ctx *ctx)
{
    size_t n = ctx->d->commit_count;
    size_t g = ctx->family_count;
    ctx->fork_head = xmalloc((n ? n : 1) * sizeof(size_t));
    ctx->fork_next = xmalloc((g ? g : 1) * sizeof(size_t));
    for (size_t i = 0; i < n; i++)
        ctx->fork_head[i] = NO_POS;
    for (size_t fi = 0; fi < g; fi++) {
        size_t f = ctx->families[fi].f;
        ctx->fork_next[fi] = NO_POS;
        if (f == NO_POS)
            continue;
        ctx->fork_next[fi] = ctx->fork_head[f];
        ctx->fork_head[f] = fi;
    }
}

/* Famiglie presenti su ciascun commit condiviso: blocchi contigui, in ordine di
 * famiglia. Il totale e' la somma delle condivisioni, cioe' quanto history ha
 * gia' percorso le catene: nessun costo quadratico in piu' rispetto ai
 * riferimenti. */
/* Famiglie presenti sui commit che possono essere un punto di diramazione: i
 * punti di diramazione delle famiglie e le punte delle famiglie. Gli altri
 * commit condivisi stanno dentro un ramo comune (li' non si stacca nessuno e non
 * finisce nessuna punta: e' il caso di un commit su un tronco, che basta da solo
 * a decidere) e non viene osservato.
 *
 * Restringere le liste a questi commit tiene il memoria legato al numero di
 * riferimenti e non alla lunghezza delle catene: su un repository grande i troni
 * condividono decine di migliaia di commit e le liste diventerebbero enormi.
 * Il risultato sono blocchi contigui, in ordine di famiglia. */
static void build_peer_index(Ctx *ctx)
{
    size_t n = ctx->d->commit_count;
    size_t g = ctx->family_count;
    uint8_t *interesting = xcalloc(n ? n : 1, 1);
    for (size_t fi = 0; fi < g; fi++) {
        if (ctx->families[fi].f != NO_POS)
            interesting[ctx->families[fi].f] = 1;
        if (ctx->families[fi].tip_idx != NO_POS)
            interesting[ctx->families[fi].tip_idx] = 1;
    }
    ctx->list_start = xmalloc((n ? n : 1) * sizeof(size_t));
    ctx->list_count = xmalloc((n ? n : 1) * sizeof(size_t));
    ctx->list_fill = xmalloc((n ? n : 1) * sizeof(size_t));
    for (size_t i = 0; i < n; i++)
        ctx->list_count[i] = 0;

    for (size_t fi = 0; fi < g; fi++) {
        for (size_t cur = ctx->families[fi].tip_idx; cur != NO_POS; cur = first_parent(ctx, cur)) {
            if (interesting[cur])
                ctx->list_count[cur]++;
        }
    }
    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        ctx->list_start[i] = total;
        ctx->list_fill[i] = total;
        total += ctx->list_count[i];
    }
    ctx->list_items = xmalloc((total ? total : 1) * sizeof(size_t));
    for (size_t fi = 0; fi < g; fi++) {
        for (size_t cur = ctx->families[fi].tip_idx; cur != NO_POS; cur = first_parent(ctx, cur)) {
            if (!interesting[cur])
                continue;
            ctx->list_items[ctx->list_fill[cur]++] = fi;
        }
    }
    free(interesting);
}

/* Commite su cui vale la pena fermarsi: il punto di diramazione di qualcuno o la
 * punta di una famiglia. */
static bool commit_is_interesting(const Ctx *ctx, size_t commit)
{
    return ctx->fork_head[commit] != NO_POS || ctx->tip_at[commit] != NO_POS;
}
/* Famiglie la cui catena contiene il commit: blocchi contigui costruiti da
 * build_peer_index, in ordine di famiglia (le famiglie hanno un indice stabile,
 * i nomi non c'entrano). */
static size_t peers_at(const Ctx *ctx, size_t commit)
{
    if (commit == NO_POS || commit >= ctx->d->commit_count)
        return 0;
    return ctx->list_count[commit];
}

static const size_t *peers_at_list(const Ctx *ctx, size_t commit)
{
    return &ctx->list_items[ctx->list_start[commit]];
}
/* Famiglie la cui catena contiene la punta di b: stanno dentro b (discendenti
 * o continuazioni), quindi non possono essere il padre di b. I tronchi non
 * contano: un tronco e' la base e non continua nessuno, quindi i commit che
 * condivide con b restano del tronco.
 *
 * Si calcola con una sola passata sulle catene: mentre si percorre la catena di X
 * si nota ogni punta incontrata e X viene messo fra le famiglie che contiene. */
static void compute_inside(Ctx *ctx)
{
    size_t n = ctx->d->commit_count;
    size_t g = ctx->family_count;
    ctx->tip_at = xmalloc((n ? n : 1) * sizeof(size_t));
    for (size_t i = 0; i < n; i++)
        ctx->tip_at[i] = NO_POS;
    for (size_t fi = 0; fi < g; fi++) {
        size_t tip = ctx->families[fi].tip_idx;
        if (tip != NO_POS)
            ctx->tip_at[tip] = fi;
    }
    /* marchi delle catene: una generazione per famiglia, cosi' si puo' chiedere
     * "un commit e' su questa catena?" in tempo costante */
    ctx->mark_gen = xcalloc(n ? n : 1, sizeof(uint32_t));
    ctx->mark = 0;
    for (size_t fi = 0; fi < g; fi++)
        ctx->families[fi].inside = bitset_new(g ? g : 1);

    for (size_t xi = 0; xi < g; xi++) {
        const Family *x = &ctx->families[xi];
        if (x->tip_idx == NO_POS || x->trunk)
            continue;
        for (size_t cur = x->tip_idx; cur != NO_POS; cur = first_parent(ctx, cur)) {
            size_t tipf = ctx->tip_at[cur];
            if (tipf == NO_POS || tipf == xi)
                continue;
            bitset_set(ctx->families[tipf].inside, xi);
        }
    }
}

static bool family_is_inside(const Ctx *ctx, size_t b, size_t x)
{
    const BitSet *set = ctx->families[b].inside;
    return set && bitset_test(set, x);
}

/* Marchia la catena della famiglia b: serve a chiedere in tempo costante se un
 * commit (per esempio il punto di diramazione di un'altra famiglia) sta su
 * questa catena. */
static void mark_chain(Ctx *ctx, size_t b)
{
    ctx->mark++;
    for (size_t cur = ctx->families[b].tip_idx; cur != NO_POS; cur = first_parent(ctx, cur))
        ctx->mark_gen[cur] = ctx->mark;
}

/* X e' un figlio di b (nel punto c): si stacca dalla catena di b, quindi sta
 * sotto e non puo' esserne il padre. Si riconosce dalla sola struttura e dagli
 * orari dei commit (dati di Git), mai dai nomi:
 *  - f(X) sta sulla catena di b ed e' piu' in alto di c: X e' nato dentro il
 *    ramo di b, sopra il punto in cui i due si incontrano (sulla stessa catena
 *    l'ordine delle posizioni nel log e' quello degli antenati);
 *  - f(X) == c e b continua in X (la catena di X passa per la punta di b): X
 *    e' il figlio, senza guardare le date;
 *  - f(X) == c e X ha integrato b con un merge (b sta nella storia di X ma non
 *    sulla sua catena dei primi genitori): X e' il padre, non un figlio;
 *  - f(X) == c e nessuno dei due contiene l'altro: i due rami sono fratelli,
 *    nati dallo stesso punto. Vince l'ordine di nascita: prima l'orario dei
 *    commit di punta (dato di Git); a parita' d'orario, come nei repository
 *    generati in blocco dove ogni commit ha lo stesso timestamp, la posizione
 *    nel log, dove i discendenti precedono gli antenati. Anche questa e' una
 *    posizione di Git, non un nome: ecco perche' permutare i nomi non cambia
 *    nulla (lo verifica il test di invarianza). Resta comunque un fatto, non
 *    una prova: se la struttura non piacesse, [parents] la dichiara.
 *
 * `mark` e' la generazione con cui e' stata marchiata la catena di b. */
static bool is_child_at(const Ctx *ctx, size_t b, size_t c, size_t x, uint32_t mark)
{
    const Family *xf = &ctx->families[x];
    const Family *bf = &ctx->families[b];
    if (xf->f == NO_POS || bf->tip_idx == NO_POS || xf->tip_idx == NO_POS)
        return false;
    if (ctx->mark_gen[xf->f] != mark)
        return false; /* f(X) non e' sulla catena di b */
    if (xf->f < c)
        return true; /* nato piu' in alto: dentro il ramo di b */
    if (xf->f == c) {
        if (family_is_inside(ctx, b, x))
            return true; /* b continua in X: X e' il figlio */
        if (bitset_test(xf->reach, bf->tip_idx))
            return false; /* X ha integrato b con un merge: X e' il padre */
        int64_t dx = date_at(ctx, xf->tip_idx);
        int64_t db = date_at(ctx, bf->tip_idx);
        if (dx != db)
            return dx > db; /* fratelli: nato dopo e' il figlio */
        return xf->tip_idx < bf->tip_idx; /* stessi orari: ordine di nascita */
    }
    return false;
}
/* X e' un plausibile padre di b al punto c se non e' b stesso, non sta dentro b
 * e non e' un figlio di b.
 *
 * Il solo fatto che X contenga i commit di b non esclude nulla: un merge porta
 * i commit di b dentro X (per esempio main dopo un merge di branch1), e main
 * resta il padre di b. */
static bool peer_is_parent(const Ctx *ctx, size_t b, size_t c, size_t x, uint32_t mark)
{
    if (x == b)
        return false;
    if (family_is_inside(ctx, b, x))
        return false;
    if (is_child_at(ctx, b, c, x, mark))
        return false;
    return true;
}

/* Primo commit proprio di ciascuna famiglia: il piu' vecchio della sua regione
 * propria, cioe' quello subito sopra il punto di diramazione (serve per ordinare
 * i figli nel layout e per il suggerimento fra candidati).
 *
 * Un commit e' proprio se nessun'altra famiglia lo raggiunge, oppure se le
 * famiglie che lo raggiungono stanno dentro b (figli e discendenti: b esiste
 * perche' qualcuno e' partito da li'). Senza commit proprio non c'e' una base:
 * la famiglia e' dentro un'altra (fast-forward, specchio lontano, alias) e non ha
 * un punto da cui sia partito qualcuno, quindi neppure un punto di diramazione. */
static void compute_first_own(Ctx *ctx)
{
    size_t g = ctx->family_count;
    for (size_t fi = 0; fi < g; fi++) {
        Family *fam = &ctx->families[fi];
        fam->first_own = NO_POS;
        if (fam->tip_idx == NO_POS || fam->trunk)
            continue;
        mark_chain(ctx, fi);
        uint32_t mark = ctx->mark;
        size_t own = NO_POS;
        for (size_t cur = fam->tip_idx; cur != NO_POS; cur = first_parent(ctx, cur)) {
            if (ctx->nchain[cur] == 1) {
                own = cur;
                continue;
            }
            if (ctx->trunk_at[cur] != NO_POS)
                break; /* un tronco possiede questo commit: la regione finisce */
            if (!commit_is_interesting(ctx, cur)) {
                own = cur; /* ramo comune: nessuno si stacca qui, resta proprio */
                continue;
            }
            const size_t *peers = peers_at_list(ctx, cur);
            size_t count = peers_at(ctx, cur);
            bool proprio = true;
            for (size_t k = 0; k < count && proprio; k++)
                if (peer_is_parent(ctx, fi, cur, peers[k], mark))
                    proprio = false;
            if (!proprio)
                break; /* qui la famiglia entra nella storia di un altro ramo */
            own = cur;
        }
        fam->first_own = own;
        if (fam->first_own == NO_POS)
            fam->f = NO_POS;
    }
}
/* ---------- candidati ---------- */

/* Scende la catena di b e si ferma al primo commit dove un'altra famiglia e' un
 * plausibile padre: quello e' il punto di diramazione che vale e i candidati sono
 * le famiglie presenti su quel commit. Se sul primo punto condiviso non resta
 * nessun plausibile padre (per esempio b e' una continuazione di X: la
 * condivisione e' con un ramo che si e' staccato da b) la discesa continua.
 *
 * Scrive il punto scelto in fam->fork_dec (NO_POS se nessuno) e i candidati in
 * fam->cand; la lista e' libera e va liberata da chi chiama. */
static void collect_candidates(Ctx *ctx, size_t bi)
{
    Family *b = &ctx->families[bi];
    free(b->cand);
    b->cand = NULL;
    b->cand_count = 0;
    b->fork_dec = NO_POS;
    if (b->tip_idx == NO_POS || b->trunk)
        return;
    /* senza commit proprio non c'e' una base da cui sia partito qualcosa: la
     * famiglia e' dentro un'altra (fast-forward, specchio lontano, alias) e il
     * fallback sui contenitori risponde meglio di un punto di diramazione */
    if (b->first_own == NO_POS)
        return;

    /* la punta di b non e' mai il suo punto di diramazione: b non e' nato da
     * se stesso (e la punta e' per definizione sua) */
    mark_chain(ctx, bi);
    for (size_t cur = first_parent(ctx, b->tip_idx); cur != NO_POS;
         cur = first_parent(ctx, cur)) {
        if (ctx->trunk_at[cur] != NO_POS) {
            /* un tronco possiede questo commit: e' il padre, non c'e' da
             * confrontare nessun altro (un long-lived non si deduce) */
            b->fork_dec = cur;
            b->cand = xmalloc(sizeof(size_t));
            b->cand[0] = ctx->trunk_at[cur];
            b->cand_count = 1;
            return;
        }
        if (peers_at(ctx, cur) < 2)
            continue;
        const size_t *peers = peers_at_list(ctx, cur);
        size_t count = peers_at(ctx, cur);
        size_t *vivi = xmalloc((count ? count : 1) * sizeof(size_t));
        size_t n = 0;
        for (size_t k = 0; k < count; k++)
            if (peer_is_parent(ctx, bi, cur, peers[k], ctx->mark))
                vivi[n++] = peers[k];
        if (n == 0) {
            free(vivi);
            continue;
        }
        b->fork_dec = cur;
        b->cand = vivi;
        b->cand_count = n;
        return;
    }
}

static void run_pass(Ctx *ctx)
{
    for (size_t fi = 0; fi < ctx->family_count; fi++)
        collect_candidates(ctx, fi);
}

/* Il tronco e' il padre quando e' presente fra i candidati: un long-lived non
 * si deduce, e' dichiarato. */
static size_t trunk_among(const Ctx *ctx, const Family *b)
{
    for (size_t k = 0; k < b->cand_count; k++)
        if (ctx->families[b->cand[k]].trunk)
            return b->cand[k];
    return NO_POS;
}

/* Punto di diramazione deciso dalla struttura: se il commit e' la punta di
 * un'altra famiglia, quella famiglia e' il padre. */
static size_t tip_family_at(const Ctx *ctx, size_t commit)
{
    if (commit == NO_POS)
        return NO_POS;
    for (size_t k = 0; k < ctx->family_count; k++)
        if (ctx->families[k].tip_idx == commit)
            return k;
    return NO_POS;
}
/* Nomi dei riferimenti delle famiglie indicate, nell'ordine deterministico
 * delle famiglie (long-lived prima, poi nome del rappresentante); dentro una
 * famiglia compaiono tutti i riferimenti, perche' sono la stessa genealogia e
 * non si puo' scegliere fra loro un nome unico. */
static char *join_family_names(const Ctx *ctx, const size_t *fams, size_t count)
{
    size_t g = ctx->family_count;
    char *want = xcalloc(g ? g : 1, 1);
    for (size_t i = 0; i < count; i++)
        want[fams[i]] = 1;
    StrBuf sb;
    sb_init(&sb);
    bool first = true;
    for (size_t oi = 0; oi < g; oi++) {
        size_t fi = ctx->family_order[oi];
        if (!want[fi])
            continue;
        const Family *fam = &ctx->families[fi];
        for (size_t k = 0; k < fam->node_count; k++) {
            if (!first)
                sb_append(&sb, ", ");
            first = false;
            sb_append(&sb, ctx->out->branches[fam->nodes[k]].name);
        }
    }
    free(want);
    return sb.data;
}

static void set_node_candidates(Ctx *ctx, BranchNode *n, const Family *fam)
{
    clear_candidates(n);
    if (fam->cand_count == 0)
        return;
    char *names = join_family_names(ctx, fam->cand, fam->cand_count);
    size_t count = 1;
    for (const char *p = names; *p; p++)
        if (*p == ',')
            count++;
    n->candidates = xmalloc(count * sizeof(char *));
    n->candidate_count = 0;
    char *p = names;
    while (*p) {
        char *comma = strstr(p, ", ");
        if (comma) {
            n->candidates[n->candidate_count++] = xstrndup(p, (size_t)(comma - p));
            p = comma + 2;
        } else {
            n->candidates[n->candidate_count++] = xstrdup(p);
            break;
        }
    }
    free(names);
}

/* ---------- deduzione del padre ---------- */

/* Nome del ramo locale che rappresenta una famiglia, se c'e' esattamente un
 * branch locale: gli altri riferimenti della famiglia sono i suoi specchi
 * (stesso commit) e non creano un'ambiguita' reale. Con piu' branch locali
 * (alias veri, es. amb1/amb2/ambbase) la scelta resta ambigua. */
static const char *family_local_name(const Ctx *ctx, const Family *fam)
{
    const char *name = NULL;
    for (size_t k = 0; k < fam->node_count; k++) {
        const BranchNode *n = node_at(ctx, fam->nodes[k]);
        if (n->is_remote)
            continue;
        if (name)
            return NULL;
        name = n->name;
    }
    return name;
}

/* Vero se i contenitori elencati sono tutti riferimenti allo stesso ramo
 * canonico (stesso nome, punte diverse): allora non e' un'ambiguita' reale,
 * risponde il rappresentante locale. */
static bool one_canonical_name(const Ctx *ctx, const size_t *fams, size_t count)
{
    const char *first = NULL;
    for (size_t i = 0; i < count; i++) {
        const BranchNode *n = node_at(ctx, ctx->families[fams[i]].rep_node);
        if (!first)
            first = n->canonical;
        else if (strcmp(first, n->canonical) != 0)
            return false;
    }
    return true;
}

typedef struct {
    ParentState state;
    char *parent;       /* da liberare dopo l'applicazione */
    char *motivation;   /* idem */
    char *suggested;    /* padre suggerito fra i candidati (solo se ambiguo) */
    SuggestionBasis basis; /* perche' e' stato suggerito */
    size_t fork_idx;
    size_t first_own_idx;
} Deduction;

/* Accoda una nota a una motivazione, preceduta dal separatore "; " solo se
 * c'e' gia' testo: una riga che comincia con "motivo: ; ..." direbbe che
 * l'hai letta male, perche' il separatore non separa niente. */
static void append_note(StrBuf *mot, const char *testo)
{
    if (mot->len)
        sb_append(mot, "; ");
    sb_append(mot, testo);
}

/* Deduce il padre di una famiglia e la motivazione della scelta. */
static void deduce(Ctx *ctx, size_t fi, Deduction *d)
{
    memset(d, 0, sizeof *d);
    d->state = PARENT_NONE;
    d->fork_idx = NO_POS;
    d->first_own_idx = NO_POS;
    const Family *b = &ctx->families[fi];
    char sh[16];
    StrBuf mot;
    sb_init(&mot);

    if (b->tip_idx == NO_POS) {
        sb_append(&mot, "nessun commit raggiungibile da questo riferimento");
    } else {
        /* il punto di diramazione che vale e' quello dove la scelta e' stata
         * fatta: se sul primo punto condiviso non restava un plausibile padre
         * la discesa e' continuata (fork_dec) */
        d->fork_idx = b->fork_dec;
        d->first_own_idx = b->first_own;
        if (b->fork_dec != NO_POS)
            gitdata_short_hash(commit_at(ctx, b->fork_dec)->hash, 8, sh);
        else
            sh[0] = '\0';

        if (b->cand_count == 0) {
            /* fallback: famiglie che contengono la punta (raggiungibilita') */
            size_t *holders = xmalloc((ctx->family_count ? ctx->family_count : 1) *
                                      sizeof(size_t));
            size_t nh = 0;
            for (size_t oi = 0; oi < ctx->family_count; oi++) {
                size_t hi = ctx->family_order[oi];
                if (hi == fi)
                    continue;
                const Family *h = &ctx->families[hi];
                if (h->tip_idx != NO_POS && bitset_test(h->reach, b->tip_idx))
                    holders[nh++] = hi;
            }
            if (nh == 0) {
                d->state = PARENT_NONE;
                if (b->fork_dec != NO_POS)
                    sb_appendf(&mot,
                               "genealogia non determinabile: nessun branch piu' antico "
                               "del punto di diramazione %s",
                               sh);
                else
                    sb_append(&mot,
                              "genealogia non determinabile: nessun altro branch "
                              "condivide la storia");
            } else if (nh == 1 || one_canonical_name(ctx, holders, nh)) {
                /* piu' riferimenti con lo stesso nome canonico (specchi lontani)
                 * sono lo stesso ramo: si risponde col rappresentante locale */
                size_t pick = holders[0];
                for (size_t i = 1; i < nh; i++) {
                    const Family *a = &ctx->families[holders[i]];
                    const Family *b2 = &ctx->families[pick];
                    bool a_local = !node_at(ctx, a->rep_node)->is_remote;
                    bool b_local = !node_at(ctx, b2->rep_node)->is_remote;
                    if (a_local && !b_local)
                        pick = holders[i];
                }
                const Family *h = &ctx->families[pick];
                d->fork_idx = b->fork_dec;
                d->first_own_idx = b->first_own;
                const char *local = family_local_name(ctx, h);
                if (local) {
                    d->state = PARENT_DEDUCED;
                    d->parent = xstrdup(local);
                    sb_appendf(&mot, "nessun commit proprio: interamente contenuto in '%s'",
                               d->parent);
                } else if (h->node_count == 1) {
                    d->state = PARENT_DEDUCED;
                    d->parent = xstrdup(ctx->out->branches[h->rep_node].name);
                    sb_appendf(&mot, "nessun commit proprio: interamente contenuto in '%s'",
                               d->parent);
                } else {
                    char *lhs = join_family_names(ctx, &pick, 1);
                    d->state = PARENT_AMBIGUOUS;
                    sb_appendf(&mot,
                               "nessun commit proprio; il contenitore e' uno dei riferimenti "
                               "sulla stessa punta: %s",
                               lhs);
                    free(lhs);
                }
            } else {
                char *lhs = join_family_names(ctx, holders, nh);
                d->state = PARENT_AMBIGUOUS;
                sb_appendf(&mot, "nessun commit proprio; piu' branch contengono la punta: %s",
                           lhs);
                free(lhs);
            }
            free(holders);
        } else {
            /* Scelta del padre, in ordine di precedenza:
             *  1. se il punto di diramazione e' la punta di un'altra famiglia,
             *     quella famiglia e' il padre: lo decide la struttura;
             *  2. se un tronco (long-lived) e' fra i candidati, e' il padre:
             *     un tronco non si deduce, e' dichiarato;
             *  3. se resta un solo candidato, e' il padre "dedotto";
             *  4. se ne restano piu' di uno, nessuno e' preferibile agli
             *     altri: stato ambiguo con l'elenco dei candidati e un
             *     suggerimento (non e' un padre dedotto: il layout lo tratta
             *     come tratteggiato finche' [parents] non dichiara il padre). */
            size_t chosen = NO_POS;
            size_t tip_parent = tip_family_at(ctx, b->fork_dec);
            /* se la punta di un'altra famiglia e' il punto di diramazione, quella
             * famiglia e' il padre: lo decide la struttura. Il filtro dei
             * candidati ha gia' escluso i figli e chi sta dentro b. */
            if (tip_parent != NO_POS && tip_parent != fi) {
                for (size_t k = 0; k < b->cand_count; k++) {
                    if (b->cand[k] == tip_parent) {
                        chosen = tip_parent;
                        break;
                    }
                }
            }
            if (chosen == NO_POS)
                chosen = trunk_among(ctx, b);
            if (chosen == NO_POS && b->cand_count == 1)
                chosen = b->cand[0]; /* un solo candidato: e' il padre dedotto */
            const char *chosen_name =
                (chosen != NO_POS) ? family_local_name(ctx, &ctx->families[chosen]) : NULL;
            if (chosen != NO_POS && chosen_name) {
                d->state = PARENT_DEDUCED;
                d->parent = xstrdup(chosen_name);
                if (chosen == tip_parent)
                    sb_appendf(&mot,
                               "punto di diramazione %s: e' la punta di '%s'", sh,
                               d->parent);
                else
                    sb_appendf(&mot, "punto di diramazione %s sul ramo di '%s'", sh,
                               d->parent);
            } else if (chosen != NO_POS &&
                       ctx->families[chosen].node_count == 1) {
                /* famiglia di un solo riferimento remoto (nessun branch locale) */
                d->state = PARENT_DEDUCED;
                d->parent = xstrdup(ctx->out->branches[ctx->families[chosen].rep_node].name);
                sb_appendf(&mot, "punto di diramazione %s sul ramo di '%s'", sh, d->parent);
            } else if (b->cand_count == 1 && chosen != NO_POS) {
                /* un solo candidato, ma con piu' nomi locali (alias veri):
                 * la famiglia e' il padre, il nome no */
                char *lhs = join_family_names(ctx, &chosen, 1);
                d->state = PARENT_AMBIGUOUS;
                sb_appendf(&mot,
                           "punto di diramazione %s; piu' riferimenti sulla stessa punta: %s",
                           sh, lhs);
                free(lhs);
            } else {
                char *lhs = join_family_names(ctx, b->cand, b->cand_count);
                d->state = PARENT_AMBIGUOUS;
                sb_appendf(&mot, "punto di diramazione %s: %s", sh, lhs);
                free(lhs);
                /* Suggerimento fra i candidati: il piu' vecchio, perche' la
                 * struttura non distingue e di solito il padre e' quello su cui
                 * gli altri sono nati. Non e' un fatto dedotto: [parents]
                 * dichiara il padre vero. */
                size_t best = NO_POS;
                int64_t best_date = 0;
                size_t best_name = NO_POS;
                bool name_tie = false;
                for (size_t k = 0; k < b->cand_count; k++) {
                    const Family *c = &ctx->families[b->cand[k]];
                    int64_t when = (c->first_own != NO_POS) ? date_at(ctx, c->first_own)
                                                             : date_at(ctx, c->tip_idx);
                    size_t rep = c->rep_node;
                    bool meglio;
                    if (best == NO_POS) {
                        meglio = true;
                    } else if (when != best_date) {
                        meglio = (when < best_date);
                    } else {
                        /* stessa data: spareggio dichiarato sul nome */
                        int cmp = strcmp(ctx->out->branches[rep].name,
                                         ctx->out->branches[best_name].name);
                        meglio = (cmp < 0);
                        if (cmp != 0)
                            name_tie = true;
                        else
                            name_tie = false;
                    }
                    if (best == NO_POS || meglio) {
                        if (best != NO_POS && when == best_date)
                            name_tie = true;
                        best = b->cand[k];
                        best_date = when;
                        best_name = rep;
                    }
                }
                if (best != NO_POS) {
                    const char *sug = family_local_name(ctx, &ctx->families[best]);
                    if (!sug)
                        sug = ctx->out->branches[ctx->families[best].rep_node].name;
                    d->suggested = xstrdup(sug);
                    if (name_tie)
                        d->basis = SUGGESTION_NAME_TIE;
                    else
                        d->basis = SUGGESTION_DATE;
                    append_note(&mot, name_tie
                                              ? "padre suggerito per nome, a parita' di data"
                                              : "padre suggerito per data, poco affidabile con rebase");
                    sb_appendf(&mot, ": '%s'", d->suggested);
                }
            }
        }
    }
    d->motivation = mot.data;
}

static void deduction_free(Deduction *d)
{
    free(d->parent);
    free(d->motivation);
    free(d->suggested);
}

/* ---------- assegnazione ---------- */

/* Alias: gli altri riferimenti con la stessa punta, in ordine alfabetico. */
static void set_alias_members(Ctx *ctx, const Family *fam, size_t self, BranchNode *n)
{
    if (fam->node_count < 2)
        return;
    n->alias_members = xmalloc((fam->node_count - 1) * sizeof(char *));
    n->alias_count = 0;
    for (size_t k = 0; k < fam->node_count; k++) {
        size_t other = fam->nodes[k];
        if (other == self)
            continue;
        n->alias_members[n->alias_count++] =
            xstrdup(ctx->out->branches[other].name);
    }
    qsort(n->alias_members, n->alias_count, sizeof(char *), cmp_str_ptr);
}

static void append_alias_note(Ctx *ctx, const Family *fam, size_t self, StrBuf *mot)
{
    if (fam->node_count < 2)
        return;
    StrBuf nomi;
    sb_init(&nomi);
    bool first = true;
    for (size_t k = 0; k < fam->node_count; k++) {
        size_t other = fam->nodes[k];
        if (other == self)
            continue;
        if (!first)
            sb_append(&nomi, ", ");
        first = false;
        sb_appendf(&nomi, "'%s'", ctx->out->branches[other].name);
    }
    char nota[512];
    snprintf(nota, sizeof nota,
             "stessa punta di %s (stesso padre dedotto; il nome interno e' ambiguo)",
             nomi.data ? nomi.data : "");
    sb_free(&nomi);
    append_note(mot, nota);
}

static void assign_family(Ctx *ctx, size_t fi)
{
    History *out = ctx->out;
    Family *fam = &ctx->families[fi];

    Deduction d;
    memset(&d, 0, sizeof d);
    d.fork_idx = NO_POS;
    d.first_own_idx = NO_POS;
    if (!fam->trunk)
        deduce(ctx, fi, &d);

    for (size_t k = 0; k < fam->node_count; k++) {
        size_t node = fam->nodes[k];
        BranchNode *n = &out->branches[node];
        set_alias_members(ctx, fam, node, n);

        /* specchio provato da Git: stesso nome canonico e stesso commit */
        if (n->mirror_of) {
            char mot[512];
            snprintf(mot, sizeof mot,
                     "remote-tracking che punta allo stesso commit di '%s'", n->mirror_of);
            set_parent(n, n->mirror_of, PARENT_CERTAIN, mot);
            set_suggestion(n, NULL, SUGGESTION_NONE);
            if (!n->is_long_lived) {
                set_fork(n, ctx, d.fork_idx);
                set_first_own(n, ctx, d.first_own_idx);
            } else {
                clear_lineage(n);
            }
            continue;
        }

        if (n->is_long_lived) {
            clear_lineage(n);
            clear_candidates(n);
            StrBuf mot;
            sb_init(&mot);
            sb_append(&mot, "tronco long-lived (configurazione): padre non dedotto");
            append_alias_note(ctx, fam, node, &mot);
            set_parent(n, NULL, PARENT_NONE, mot.data);
            set_suggestion(n, NULL, SUGGESTION_NONE);
            sb_free(&mot);
            continue;
        }

        /* override da configurazione (precedenza assoluta) */
        const char *ov_parent = NULL;
        char ov_note[256] = "";
        if (ctx->cfg) {
            for (size_t oi = 0; oi < ctx->cfg->override_count; oi++) {
                const ParentOverride *ov = &ctx->cfg->overrides[oi];
                if (strcmp(ov->branch, n->name) != 0)
                    continue;
                if (strmap_get(ctx->name_map, ov->parent)) {
                    ov_parent = ov->parent;
                } else {
                    snprintf(ov_note, sizeof ov_note,
                             "override da graph.cfg ignorato: branch '%s' inesistente",
                             ov->parent);
                }
                break;
            }
        }

        set_node_candidates(ctx, n, fam);
        set_fork(n, ctx, d.fork_idx);
        set_first_own(n, ctx, d.first_own_idx);

        StrBuf mot;
        sb_init(&mot);
        if (ov_parent) {
            sb_appendf(&mot, "override da graph.cfg: padre '%s'", ov_parent);
            if (ov_note[0])
                append_note(&mot, ov_note);
            append_alias_note(ctx, fam, node, &mot);
            set_parent(n, ov_parent, PARENT_CONFIGURED, mot.data);
            set_suggestion(n, NULL, SUGGESTION_NONE);
        } else {
            sb_append(&mot, d.motivation ? d.motivation : "");
            if (ov_note[0])
                append_note(&mot, ov_note);
            append_alias_note(ctx, fam, node, &mot);
            set_parent(n, d.parent, d.state, mot.data);
            set_suggestion(n, d.suggested, d.basis);
            if (d.state == PARENT_NONE && fam->tip_idx != NO_POS)
                n->genealogy_undetermined = true;
        }
        sb_free(&mot);
    }
    deduction_free(&d);
}

/* ---------- API pubblica ---------- */

size_t history_find_branch(const History *h, const char *name)
{
    for (size_t i = 0; i < h->branch_count; i++) {
        if (strcmp(h->branches[i].name, name) == 0)
            return i;
    }
    return (size_t)-1;
}

/* Rompe gli eventuali cicli di paternita' (possibili con override
 * incoerenti). Per ogni ciclo il nodo col nome minore diventa ambiguo: non si
 * inventa un padre per una relazione circolare.
 *
* Ogni passo usa la mappa nome->nodo (O(1)) e la memoizzazione dei nodi gia'
 * verificati senza ciclo: su un grafo aciclico il costo totale e' lineare nei
 * riferimenti, non quadratico nei nomi. */
static void break_parent_cycles(History *out, StrMap *name_map)
{
    size_t b = out->branch_count;
    if (b == 0)
        return;
    uint8_t *safe = xcalloc(b, 1); /* gia' verificato: il cammino non ha cicli */
    size_t *path = xmalloc((b + 1) * sizeof(size_t)); /* il ciclo puo' essere
                                                        formato da tutti i nodi */

    for (size_t guard = 0; guard <= b; guard++) {
        bool found = false;
        for (size_t i = 0; i < b && !found; i++) {
            if (!out->branches[i].parent)
                continue;
            size_t len = 0;
            size_t cur = i;
            bool cycle = false;
            for (size_t step = 0; step <= b; step++) {
                const char *pname = out->branches[cur].parent;
                if (!pname)
                    break;
                void *v = strmap_get(name_map, pname);
                if (!v)
                    break;
                size_t p = (size_t)(uintptr_t)v - 1;
                if (p == i) {
                    cycle = true;
                    break;
                }
                if (safe[p])
                    break; /* da li' in poi non ci sono cicli */
                path[len++] = cur;
                cur = p;
            }
            if (!cycle) {
                for (size_t k = 0; k < len; k++)
                    safe[path[k]] = 1;
                continue;
            }
            /* ciclo: il nodo col nome minore perde il padre */
            size_t best = i;
            cur = i;
            for (size_t step = 0; step <= b; step++) {
                if (strcmp(out->branches[cur].name, out->branches[best].name) < 0)
                    best = cur;
                const char *pname = out->branches[cur].parent;
                if (!pname)
                    break;
                void *v = strmap_get(name_map, pname);
                if (!v)
                    break;
                size_t p = (size_t)(uintptr_t)v - 1;
                if (p == i)
                    break;
                cur = p;
            }
            BranchNode *bn = &out->branches[best];
            free(bn->parent);
            bn->parent = NULL;
            bn->state = PARENT_AMBIGUOUS;
            bn->genealogy_undetermined = true;
            free(bn->motivation);
            bn->motivation =
                xstrdup("paternita' circolare tra branch: relazione non determinabile");
            found = true;
        }
        if (!found)
            break;
    }
    free(path);
    free(safe);
}

bool history_build(History *out, const GitData *data, const GraphConfig *cfg,
                   char *err, size_t err_size)
{
    memset(out, 0, sizeof *out);
    out->data = data;

    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.d = data;
    ctx.cfg = cfg;
    ctx.out = out;
    ctx.name_map = strmap_new();
    (void)err;
    (void)err_size;

    create_nodes(&ctx, data);
    if (out->branch_count > 0) {
        finalize_nodes(&ctx);
        build_families(&ctx);
        build_family_order(&ctx);

        size_t n = data->commit_count;
        ctx.stack = xmalloc((n ? n : 1) * sizeof(size_t));

        /* i bitset di raggiungibilita' servono per il fallback sui contenitori */
        for (size_t fi = 0; fi < ctx.family_count; fi++) {
            ctx.families[fi].reach = bitset_new(n);
            reach_build(&ctx, fi, ctx.families[fi].reach);
        }
        compute_forks(&ctx);
        build_fork_index(&ctx);
        compute_inside(&ctx);
        build_peer_index(&ctx);
        compute_first_own(&ctx);
        run_pass(&ctx);

        for (size_t oi = 0; oi < ctx.family_count; oi++)
            assign_family(&ctx, ctx.family_order[oi]);

        break_parent_cycles(out, ctx.name_map);
    }

    strmap_free(ctx.name_map);
    free(ctx.node_family);
    free(ctx.family_order);
    free(ctx.nchain);
    free(ctx.list_start);
    free(ctx.list_count);
    free(ctx.list_fill);
    free(ctx.list_items);
    free(ctx.fork_head);
    free(ctx.tip_at);
    free(ctx.trunk_at);
    free(ctx.mark_gen);
    free(ctx.fork_next);

    free(ctx.stack);
    for (size_t fi = 0; fi < ctx.family_count; fi++)
        family_free(&ctx.families[fi]);
    free(ctx.families);
    return true;
}

void history_free(History *h)
{
    if (!h || !h->branches)
        return;
    for (size_t i = 0; i < h->branch_count; i++) {
        BranchNode *n = &h->branches[i];
        free(n->name);
        free(n->canonical);
        free(n->mirror_of);
        free(n->parent);
        free(n->motivation);
        free(n->fork_hash);
        free(n->first_own_commit);
        free(n->suggested_parent);
        for (size_t k = 0; k < n->alias_count; k++)
            free(n->alias_members[k]);
        free(n->alias_members);
        for (size_t k = 0; k < n->candidate_count; k++)
            free(n->candidates[k]);
        free(n->candidates);
    }
    free(h->branches);
    memset(h, 0, sizeof *h);
}

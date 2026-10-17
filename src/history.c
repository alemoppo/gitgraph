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

/* Relazioni precalcolate, valutate sulla catena di g: rel[g*G + b].
 * "f(b)" e "tip(b)" sono il punto di diramazione e la punta della famiglia b. */
enum {
    REL_F_ON_CHAIN = 1,   /* f(b) giace sulla catena di g */
    REL_TIP_ON_CHAIN = 2, /* la punta di b giace sulla catena di g */
    REL_F_BELOW = 4,      /* f(b) sotto f(g) sulla catena di g (piu' vecchio) */
    REL_F_ABOVE = 8       /* f(b) sopra f(g) sulla catena di g (piu' recente) */
};

typedef struct {
    size_t *nodes;       /* riferimenti con la stessa punta */
    size_t node_count;
    size_t rep_node;     /* rappresentante: preferisce un nodo locale, poi
                            il primo in ordine di nome */
    size_t tip_idx;
    int ll_rank;         /* rank minimo dei membri long-lived, -1 se nessuno */
    bool trunk;           /* il rappresentante e' long-lived: famiglia di tronco */
    size_t f;            /* punto di diramazione, NO_POS se assente */
    size_t first_own;    /* primo commit proprio, NO_POS se assente */
    bool date_used;      /* un candidato e' stato scartato per data */
    size_t *cand;        /* indici famiglia candidati */
    size_t cand_count;
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
    size_t *owner;          /* commit -> famiglia che lo rivendica */
    uint8_t *rel;           /* matrice G*G delle relazioni */
    size_t *scratch_pos;    /* posizione sulla catena corrente */
    uint32_t *scratch_gen;  /* generazione della catena corrente */
    uint32_t gen;
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

static void chain_fill(Ctx *ctx, size_t fi);
static bool chain_has(const Ctx *ctx, size_t commit_idx);

/* Punti di diramazione e regioni proprie.
 *
 * Regole, indipendenti dall'ordine di visita:
 *  - un tronco (long-lived) non puo' essere una diramazione di un altro ramo:
 *    risalendo la sua catena non si ferma mai sulla punta di un'altra
 *    famiglia, e rivendica tutto cio' che incontra libero;
 *  - ogni altro branch che risale e trova la punta di un'altra famiglia si e'
 *    diramato li': quel commit e' il suo punto di diramazione e resta di chi
 *    lo aveva raggiunto prima;
 *  - un commit non ancora rivendicato diventa proprio di chi lo incontra; un
 *    commit gia' rivendicato da un'altra famiglia e' il punto di diramazione;
 *  - una famiglia che non rivendica nulla (fast-forward, specchio lontano,
 *    alias) non ha nemmeno una base: fork assente, resta da stabilire a chi
 *    appartiene con il fallback sui contenitori.
 * L'ordine conta solo per i tronchi, che sono sempre visitati per primi
 * (long-lived prima, poi nome): e' cio' che rende deterministico il resto. */
static void compute_ownership_and_forks(Ctx *ctx)
{
    size_t n = ctx->d->commit_count;
    size_t g = ctx->family_count;
    ctx->owner = xmalloc((n ? n : 1) * sizeof(size_t));
    for (size_t i = 0; i < n; i++)
        ctx->owner[i] = NO_POS;

    /* famiglia che possiede ciascuna punta (le famiglie sono per punta, quindi
     * il proprietario e' unico) */
    size_t *tip_owner = xmalloc((n ? n : 1) * sizeof(size_t));
    for (size_t i = 0; i < n; i++)
        tip_owner[i] = NO_POS;
    for (size_t fi = 0; fi < g; fi++) {
        if (ctx->families[fi].tip_idx != NO_POS)
            tip_owner[ctx->families[fi].tip_idx] = fi;
    }

    for (size_t oi = 0; oi < g; oi++) {
        size_t fi = ctx->family_order[oi];
        Family *fam = &ctx->families[fi];
        if (fam->tip_idx == NO_POS)
            continue;
        size_t cur = fam->tip_idx;
        size_t own = NO_POS;
        size_t f = NO_POS;
        while (cur != NO_POS) {
            size_t tip_g = tip_owner[cur];
            if (!fam->trunk && tip_g != NO_POS && tip_g != fi) {
                f = cur; /* biforcazione dalla punta di un altro ramo */
                break;
            }
            if (ctx->owner[cur] == NO_POS) {
                ctx->owner[cur] = fi;
                own = cur;
            } else if (ctx->owner[cur] != fi) {
                f = cur;
                break;
            }
            cur = first_parent(ctx, cur);
        }
        fam->f = (own == NO_POS) ? NO_POS : f; /* senza commit propri: niente base */
        fam->first_own = own;
    }
    free(tip_owner);
}

/* Riempie lo scratch con le posizioni (0 = punta) sulla catena della famiglia. */
static void chain_fill(Ctx *ctx, size_t fi)
{
    ctx->gen++;
    size_t pos = 0;
    size_t cur = ctx->families[fi].tip_idx;
    while (cur != NO_POS) {
        ctx->scratch_gen[cur] = ctx->gen;
        ctx->scratch_pos[cur] = pos++;
        cur = first_parent(ctx, cur);
    }
}

static bool chain_has(const Ctx *ctx, size_t commit_idx)
{
    return commit_idx != NO_POS && ctx->scratch_gen[commit_idx] == ctx->gen;
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

/* Matrice delle relazioni: per ogni catena (riga g) e ogni famiglia b, dove
 * stanno f(b) e la punta di b rispetto alla catena di g. */
static void build_rel(Ctx *ctx)
{
    size_t g = ctx->family_count;
    ctx->rel = xcalloc((g ? g * g : 1), 1);
    for (size_t gi = 0; gi < g; gi++) {
        chain_fill(ctx, gi);
        Family *gf = &ctx->families[gi];
        size_t pos_fg = NO_POS;
        if (gf->f != NO_POS && chain_has(ctx, gf->f))
            pos_fg = ctx->scratch_pos[gf->f];
        uint8_t *row = &ctx->rel[gi * g];
        for (size_t bi = 0; bi < g; bi++) {
            Family *b = &ctx->families[bi];
            uint8_t m = 0;
            if (chain_has(ctx, b->f)) {
                m |= REL_F_ON_CHAIN;
                if (pos_fg != NO_POS) {
                    size_t p = ctx->scratch_pos[b->f];
                    if (p > pos_fg)
                        m |= REL_F_BELOW;
                    else if (p < pos_fg)
                        m |= REL_F_ABOVE;
                }
            }
            if (chain_has(ctx, b->tip_idx))
                m |= REL_TIP_ON_CHAIN;
            row[bi] = m;
        }
    }
}

/* ---------- candidati ---------- */

/* Calcola i candidati di una famiglia. Con use_date false la regola D5 (fork
 * uguali, spareggio sulla data) non viene applicata: serve a confrontare il
 * risultato con e senza data. La lista dei candidati e' libera: chi chiama deve
 * liberarla.
 */
static void collect_candidates(const Ctx *ctx, size_t bi, bool use_date,
                               size_t **cand_out, size_t *cand_count_out,
                               bool *date_used_out)
{
    const Family *b = &ctx->families[bi];
    size_t g = ctx->family_count;
    size_t *cand = NULL;
    size_t n = 0;
    bool date_used = false;
    if (b->tip_idx == NO_POS || b->f == NO_POS || b->trunk) {
        *cand_out = cand;
        *cand_count_out = n;
        *date_used_out = date_used;
        return;
    }
    const uint8_t *rel_b = &ctx->rel[bi * g];
    for (size_t xi = 0; xi < g; xi++) {
        if (xi == bi)
            continue;
        const Family *beta = &ctx->families[xi];
        if (beta->tip_idx == NO_POS)
            continue;
        uint8_t rb = ctx->rel[xi * g + bi]; /* f(b), tip(b) vs catena di beta */
        if (rb & REL_TIP_ON_CHAIN)
            continue; /* D1: beta discende da b */
        if (rel_b[xi] & REL_F_ABOVE)
            continue; /* D2: beta si e' diramato da un commit interno di b */
        if (!(rb & REL_F_ON_CHAIN))
            continue; /* D6: beta non contiene il punto di diramazione */
        if (!beta->trunk) {
            if (beta->first_own != NO_POS) {
                if (rb & REL_F_BELOW)
                    continue; /* D3: beta non esisteva a f(b) */
            } else if (beta->tip_idx != b->f) {
                continue; /* D4: beta assorbito (FF/alias), non fa da padre */
            }
            /* D5: fork uguali, decisione per data (spareggio dichiarato) */
            if (use_date && beta->f == b->f && beta->first_own != NO_POS &&
                b->first_own != NO_POS &&
                date_at(ctx, beta->first_own) > date_at(ctx, b->first_own)) {
                date_used = true;
                continue;
            }
        }
        cand = xrealloc(cand, (n + 1) * sizeof(size_t));
        cand[n++] = xi;
    }
    *cand_out = cand;
    *cand_count_out = n;
    *date_used_out = date_used;
}

/* Una passata sui candidati: l'ordine di visita non conta (i candidati sono
 * determinati dalla struttura, non da chi viene prima), ma si tiene
 * esplicito perche' il risultato sia riproducibile. */
static void run_pass(Ctx *ctx)
{
    size_t g = ctx->family_count;
    for (size_t oi = 0; oi < g; oi++) {
        size_t bi = ctx->family_order[oi];
        Family *b = &ctx->families[bi];
        free(b->cand);
        b->cand = NULL;
        b->cand_count = 0;
        b->date_used = false;
        collect_candidates(ctx, bi, true, &b->cand, &b->cand_count, &b->date_used);
    }
}

/* ---------- nomi ---------- */

/* Sceglie il candidato padre fra quelli indicati: il candidato che possiede il
 * punto di diramazione (il commit e' nella sua regione propria) e' il padre
 * naturale; se nessuno lo possiede, l'unico candidato; altrimenti l'unico a
 * monte di tutti; altrimenti nessuno (ambiguo). Restituisce l'indice della
 * famiglia scelta, scrive in *by_owner se e' stata la regola del proprietario e
 * in *count quanti candidati soddisfano la regola applicata. */
static size_t select_parent_family(const Ctx *ctx, const Family *b,
                                   const size_t *cand, size_t cand_count,
                                   bool *by_owner, size_t *count_out)
{
    size_t chosen = NO_POS;
    size_t count = 0;
    *by_owner = false;

    if (b->f != NO_POS && ctx->owner[b->f] != NO_POS) {
        for (size_t i = 0; i < cand_count; i++) {
            if (cand[i] == ctx->owner[b->f]) {
                chosen = cand[i];
                count++;
            }
        }
        *by_owner = (count == 1);
    }
    if (!*by_owner && cand_count == 1) {
        chosen = cand[0];
        count = 1;
    }
    if (!*by_owner && count != 1 && cand_count > 1) {
        count = 0;
        for (size_t i = 0; i < cand_count; i++) {
            const Family *c = &ctx->families[cand[i]];
            bool ok = true;
            for (size_t j = 0; j < cand_count && ok; j++) {
                if (i == j)
                    continue;
                const Family *o = &ctx->families[cand[j]];
                if (!bitset_test(o->reach, c->tip_idx))
                    ok = false;
            }
            if (ok) {
                chosen = cand[i];
                count++;
            }
        }
    }
    *count_out = count;
    return chosen;
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
    /* la menzione della data si puo' rivelare non necessaria piu' in fondo */
    bool menziona_data = b->date_used;

    if (b->tip_idx == NO_POS) {
        sb_append(&mot, "nessun commit raggiungibile da questo riferimento");
    } else {
        d->fork_idx = b->f;
        d->first_own_idx = b->first_own;
        if (b->f != NO_POS)
            gitdata_short_hash(commit_at(ctx, b->f)->hash, 8, sh);
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
                if (b->f != NO_POS)
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
                d->fork_idx = b->f;
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
            /* Scelta: il candidato che possiede il punto di diramazione (il
             * commit e' nella sua regione propria) e' il padre naturale; se
             * nessuno lo possiede, unico candidato; altrimenti l'unico a monte
             * di tutti; altrimenti ambiguo. */
            bool by_owner = false;
            size_t count = 0;
            size_t chosen = select_parent_family(ctx, b, b->cand, b->cand_count,
                                                 &by_owner, &count);
            const char *chosen_name =
                (chosen != NO_POS) ? family_local_name(ctx, &ctx->families[chosen]) : NULL;
            if (count == 1 && chosen_name) {
                d->state = PARENT_DEDUCED;
                d->parent = xstrdup(chosen_name);
                if (by_owner)
                    sb_appendf(&mot,
                               "punto di diramazione %s: quel commit appartiene al ramo di '%s'",
                               sh, d->parent);
                else if (b->cand_count > 1)
                    sb_appendf(&mot,
                               "punto di diramazione %s; '%s' e' a monte degli altri candidati",
                               sh, d->parent);
                else
                    sb_appendf(&mot, "punto di diramazione %s sul ramo di '%s'", sh,
                               d->parent);
            } else if (count == 1 && chosen != NO_POS &&
                       ctx->families[chosen].node_count == 1) {
                /* famiglia di un solo riferimento remoto (nessun branch locale) */
                d->state = PARENT_DEDUCED;
                d->parent = xstrdup(ctx->out->branches[ctx->families[chosen].rep_node].name);
                sb_appendf(&mot, "punto di diramazione %s sul ramo di '%s'", sh, d->parent);
            } else {
                char *lhs = join_family_names(ctx, b->cand, b->cand_count);
                d->state = PARENT_AMBIGUOUS;
                if (b->cand_count == 1)
                    sb_appendf(&mot,
                               "punto di diramazione %s; piu' riferimenti sulla stessa punta: %s",
                               sh, lhs);
                else
                    sb_appendf(&mot,
                               "piu' branch contengono il punto di diramazione %s: %s", sh,
                               lhs);
                free(lhs);
            }
        }
    }

    /* La menzione della data compare solo se l'ordinamento per data ha cambiato
     * davvero l'esito: si ricalcolano i candidati senza la regola D5 e si
     * confronta la scelta. Se il risultato sarebbe stato lo stesso, la frase
     * sarebbe fuorviante (su branch1, per esempio, il candidato scartato non
     * avrebbe cambiato nulla e la frase non compare).
     *
     * Il confronto e' quello che conta davvero: la regola D5 scarta sempre
     * almeno un candidato, quindi le due liste non coincidono mai; se pero' la
     * scelta finale resta la stessa, la data non ha deciso niente e non va
     * dichiarata come spareggio. */
    if (b->date_used) {
        size_t *alt = NULL, alt_count = 0;
        bool alt_date = false;
        collect_candidates(ctx, fi, false, &alt, &alt_count, &alt_date);
        bool senza_owner = false, con_owner = false;
        size_t senza_count = 0, con_count = 0;
        size_t senza =
            select_parent_family(ctx, b, alt, alt_count, &senza_owner, &senza_count);
        size_t con =
            select_parent_family(ctx, b, b->cand, b->cand_count, &con_owner, &con_count);
        /* stesso esito: stessa famiglia scelta, stesso numero di candidati che
         * soddisfano la regola, stessa regola applicata e stesso ricorso al
         * percorso dei "holder" (nessun candidato, in entrambi i casi) */
        bool stesso_esito = (senza == con) && (senza_count == con_count) &&
                            (senza_owner == con_owner) &&
                            ((alt_count == 0) == (b->cand_count == 0));
        if (stesso_esito)
            menziona_data = false;
        free(alt);
    }
    if (menziona_data) {
        append_note(&mot, "scarto dei candidati basato sulla data (poco affidabile con rebase)");
    }
    d->motivation = mot.data;
}

static void deduction_free(Deduction *d)
{
    free(d->parent);
    free(d->motivation);
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
        } else {
            sb_append(&mot, d.motivation ? d.motivation : "");
            if (ov_note[0])
                append_note(&mot, ov_note);
            append_alias_note(ctx, fam, node, &mot);
            set_parent(n, d.parent, d.state, mot.data);
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
        ctx.scratch_pos = xmalloc((n ? n : 1) * sizeof(size_t));
        ctx.scratch_gen = xcalloc(n ? n : 1, sizeof(uint32_t));
        ctx.stack = xmalloc((n ? n : 1) * sizeof(size_t));

        compute_ownership_and_forks(&ctx);


        for (size_t fi = 0; fi < ctx.family_count; fi++) {
            ctx.families[fi].reach = bitset_new(n);
            reach_build(&ctx, fi, ctx.families[fi].reach);
        }
        build_rel(&ctx);
        run_pass(&ctx);

        for (size_t oi = 0; oi < ctx.family_count; oi++)
            assign_family(&ctx, ctx.family_order[oi]);

        break_parent_cycles(out, ctx.name_map);
    }

    strmap_free(ctx.name_map);
    free(ctx.node_family);
    free(ctx.family_order);
    free(ctx.owner);
    free(ctx.rel);
    free(ctx.scratch_pos);
    free(ctx.scratch_gen);
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

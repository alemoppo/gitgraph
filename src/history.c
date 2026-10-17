/* history.c - vedi history.h
 *
 * Euristica (documentata, deterministica). Tutto lavora su indici di commit
 * precalcolati una sola volta: nessuna scansione di hash nei passi interni.
 *
 * 1. Gruppi e famiglie. I riferimenti con lo stesso nome canonico (branch
 *    locale + remote-tracking omonimi) formano un "gruppo" (gli specchi).
 *    I gruppi con la stessa punta formano una "famiglia di alias": sono lo
 *    stesso stato della storia (stessa catena dei primi genitori), quindi la
 *    deduzione vale per la famiglia; solo l'ordine interno tra gli alias e'
 *    non determinabile.
 *
 * 2. Appartenenza dei commit. Le famiglie, nell'ordine deterministico
 *    (long-lived prima, poi nome), rivendicano i commit della propria catena
 *    dei primi genitori non ancora rivendicati da altri: ogni commit
 *    appartiene alla "regione propria" della prima famiglia che lo attraversa.
 *    La scelta tra diramazioni sorelle (stesso punto di partenza) e'
 *    arbitraria ma stabile: la storia non registra quale sia nata prima.
 *
 * 3. Punto di diramazione f(b): il primo commit che, risalendo la catena dei
 *    primi genitori dalla punta di b, appartiene alla regione propria di
 *    un'altra famiglia. Non e' la raggiungibilita' generica: dopo un merge i
 *    commit di b sono raggiungibili da altri, ma la catena dei primi genitori
 *    non li attraversa, quindi il fork resta corretto.
 *
 * 4. Primo commit proprio: il piu' vecchio commit rivendicato da b sulla
 *    catena (il figlio di f(b)); assente se la punta e' gia' rivendicata da
 *    altri (branch assorbito o fast-forward).
 *
 * 4. Candidati padre β di b (famiglia), con filtri O(1) grazie alla matrice
 *    di relazioni precalcolata sulle catene dei primi genitori:
 *      D1  la punta di b giace sulla catena di β          -> β discende da b;
 *      D2  f(β) sta sulla catena di b, piu' in alto di f(b) -> β e' figlio;
 *      D3  esistenza: f(b) sta sotto f(β) sulla catena di β -> β non esisteva;
 *      D4  assorbito: β senza commit propri e punta su un'altra catena, e non
 *          proprio il punto di diramazione -> escluso (FF/alias non fanno da
 *          padre); i long-lived sono esentati (il tronco esiste da sempre);
 *      D5  fork uguali: si usa la data del primo commit proprio come
 *          spareggio quando la struttura non decide (scarto del candidato
 *          piu' recente di b); la motivazione lo dichiara sempre.
 *      D6  contenimento: f(b) deve giacere sulla catena di β.
 *
 * 5. Scelta del padre: un solo candidato -> dedotto; piu' candidati -> quello
 *    che e' a monte di tutti (raggiungibilita'), altrimenti ambiguo. Nessun
 *    candidato -> fallback: contenitori della punta (raggiungibilita'), o
 *    radice. I long-lived non hanno fork ne' candidati.
 *
 * 6. Complessita'. Siano C i commit, R i riferimenti, G <= R le famiglie e
 *    L_g la lunghezza della catena dei primi genitori della famiglia g.
 *    Precalcolo: O(C + genitori) per gli indici (in gitdata), O(Σ L_g) per i
 *    conteggi di catena, O(Σ L_g + G²) per la matrice di relazioni,
 *    O(Σ |raggiungibili|) per i bitset di raggiungibilita' (limite superiore
 *    G·C, in pratica molto meno). Deduzione: una sola passata O(G² + Σ L_g).
 *    Nessun percorso di catena o DFS viene rifatto per coppia di branch, e
 *    nulla viene riallocato per commit: il costo dipende linearmente dal
 *    repository, non dal prodotto di branch per cammini.
 */
#include "history.h"

#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NO_POS ((size_t)-1)

/* Relazioni precalcolate, valutate sulla catena di g: rel[g*G + x].
 * "f(x)" e "tip(x)" sono il punto di diramazione e la punta della famiglia x. */
enum {
    REL_F_ON_CHAIN = 1,   /* f(x) giace sulla catena di g */
    REL_TIP_ON_CHAIN = 2, /* la punta di x giace sulla catena di g */
    REL_F_BELOW = 4,      /* f(x) sotto f(g) sulla catena di g (piu' vecchio) */
    REL_F_ABOVE = 8       /* f(x) sopra f(g) sulla catena di g (piu' recente) */
};

typedef struct {
    char *canonical;   /* nome canonico del gruppo */
    char *rep_name;    /* nome del rappresentante */
    size_t rep_node;   /* indice in History.branches del rappresentante */
    size_t *members;   /* indici nodo (rappresentante + specchi) */
    size_t member_count;
    size_t tip_idx;    /* indice del commit punta, NO_POS se fuori dal log */
    bool is_ll;        /* gruppo long-lived (configurazione) */
} Group;

typedef struct {
    size_t *groups;    /* gruppi della famiglia, in ordine deterministico */
    size_t group_count;
    size_t rep_group;  /* gruppo di riferimento (primo in ordine) */
    size_t tip_idx;
    int ll_rank;       /* rank minimo dei membri long-lived, -1 se nessuno */
    bool all_ll;       /* tutti i membri sono long-lived */
    size_t f;          /* punto di diramazione, NO_POS se assente */
    size_t first_own;  /* primo commit proprio, NO_POS se assente */
    bool date_used;    /* un candidato e' stato scartato per data */
    size_t *cand;      /* indici famiglia candidati */
    size_t cand_count;
    BitSet *reach;     /* commit raggiungibili dalla punta */
} Family;

typedef struct {
    const GitData *d;
    const GraphConfig *cfg;
    History *out;
    Group *groups;
    size_t group_count;
    Family *families;
    size_t family_count;
    size_t *group_family;    /* gruppo -> famiglia */
    size_t *group_order;     /* gruppi in ordine deterministico */
    size_t *group_rank;      /* posizione in group_order */
    size_t *family_order;    /* famiglie in ordine deterministico */
    uint8_t *rel;            /* matrice G*G delle relazioni */
    size_t *scratch_pos;     /* posizione sullla catena corrente */
    uint32_t *scratch_gen;   /* generazione della catena corrente */
    uint32_t gen;
    size_t *stack;           /* buffer riutilizzato per i DFS */
    StrMap *canonical_map;   /* nome canonico -> (indice gruppo + 1) */
    StrMap *name_map;        /* nome nodo -> (indice nodo + 1) */
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

/* ---------- ordine deterministico ---------- */

static int cmp_str_ptr(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Chiave di ordinamento: long-lived prima (nell'ordine della configurazione),
 * poi per nome canonico. */
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

/* Azzera fork e primo commit proprio (long-lived e riferimenti invalidI). */
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

/* ---------- costruzione di gruppi e famiglie ---------- */

static void group_free(Group *g)
{
    free(g->canonical);
    free(g->rep_name);
    free(g->members);
}

static void family_free(Family *f)
{
    free(f->groups);
    free(f->cand);
    bitset_free(f->reach);
}

static bool ctx_init(Ctx *ctx, History *out, const GitData *data, const GraphConfig *cfg)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->d = data;
    ctx->cfg = cfg;
    ctx->out = out;
    ctx->canonical_map = strmap_new();
    ctx->name_map = strmap_new();

    size_t cap_groups = data->ref_count ? data->ref_count : 1;
    ctx->groups = xcalloc(cap_groups, sizeof(Group));
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

        size_t gi;
        void *v = strmap_get(ctx->canonical_map, canonical);
        if (v) {
            gi = (size_t)(uintptr_t)v - 1;
        } else {
            gi = ctx->group_count++;
            Group *g = &ctx->groups[gi];
            memset(g, 0, sizeof *g);
            g->canonical = xstrdup(canonical);
            g->rep_node = NO_POS;
            g->tip_idx = NO_POS;
            strmap_put(ctx->canonical_map, g->canonical,
                       (void *)(uintptr_t)(gi + 1));
        }
        Group *g = &ctx->groups[gi];
        g->members = xrealloc(g->members, (g->member_count + 1) * sizeof(size_t));
        g->members[g->member_count++] = node;
    }
    return true;
}

/* Rank long-lived di un gruppo: si prova il nome canonico, poi il
 * rappresentante, poi gli altri nomi del gruppo. Cosi' "[long-lived]
 * origin/main" vale quanto "[long-lived] main", essendo il remote-tracking
 * uno specchio dello stesso branch. */
static int group_ll_rank(const Ctx *ctx, const Group *g)
{
    int rank = config_long_lived_rank(ctx->cfg, g->canonical);
    if (rank < 0 && g->rep_name)
        rank = config_long_lived_rank(ctx->cfg, g->rep_name);
    if (rank < 0) {
        for (size_t k = 0; k < g->member_count; k++) {
            const BranchNode *n = &ctx->out->branches[g->members[k]];
            rank = config_long_lived_rank(ctx->cfg, n->name);
            if (rank >= 0)
                break;
        }
    }
    return rank;
}

/* Rappresentante di ogni gruppo, specchi, punte, long-lived. */
static void finalize_groups(Ctx *ctx)
{
    History *out = ctx->out;
    for (size_t gi = 0; gi < ctx->group_count; gi++) {
        Group *g = &ctx->groups[gi];
        size_t rep = NO_POS;
        for (size_t k = 0; k < g->member_count; k++) {
            size_t node = g->members[k];
            if (!out->branches[node].is_remote) {
                rep = node;
                break;
            }
        }
        if (rep == NO_POS)
            rep = g->members[0];
        g->rep_node = rep;
        g->rep_name = xstrdup(out->branches[rep].name);

        const GitRef *r = &out->data->refs[out->branches[rep].ref_index];
        g->tip_idx = gitdata_find_commit(out->data, r->target);

        for (size_t k = 0; k < g->member_count; k++) {
            size_t node = g->members[k];
            if (node == rep)
                continue;
            out->branches[node].mirror_of = xstrdup(out->branches[rep].name);
        }

        int rank = group_ll_rank(ctx, g);
        g->is_ll = (rank >= 0);
        for (size_t k = 0; k < g->member_count; k++) {
            BranchNode *n = &out->branches[g->members[k]];
            n->is_long_lived = (rank >= 0);
            n->long_lived_rank = rank;
        }
    }
}

static void build_group_order(Ctx *ctx)
{
    size_t n = ctx->group_count;
    ctx->group_order = xmalloc((n ? n : 1) * sizeof(size_t));
    ctx->group_rank = xmalloc((n ? n : 1) * sizeof(size_t));
    OrderKey *keys = xmalloc((n ? n : 1) * sizeof(OrderKey));
    for (size_t i = 0; i < n; i++) {
        keys[i].idx = i;
        keys[i].rank = group_ll_rank(ctx, &ctx->groups[i]);
        keys[i].name = ctx->groups[i].canonical;
    }
    qsort(keys, n, sizeof(OrderKey), cmp_order_key);
    for (size_t i = 0; i < n; i++) {
        ctx->group_order[i] = keys[i].idx;
        ctx->group_rank[keys[i].idx] = i;
    }
    free(keys);
}

/* Famiglie: gruppi con la stessa punta. */
static void build_families(Ctx *ctx)
{
    size_t n = ctx->d->commit_count;
    size_t g = ctx->group_count;
    ctx->group_family = xmalloc((g ? g : 1) * sizeof(size_t));
    ctx->families = xcalloc(g ? g : 1, sizeof(Family));
    ctx->family_count = 0;

    size_t *fam_of_tip = xmalloc((n ? n : 1) * sizeof(size_t));
    for (size_t i = 0; i < n; i++)
        fam_of_tip[i] = NO_POS;

    for (size_t oi = 0; oi < g; oi++) {
        size_t gi = ctx->group_order[oi];
        Group *grp = &ctx->groups[gi];
        size_t fi;
        if (grp->tip_idx != NO_POS) {
            fi = fam_of_tip[grp->tip_idx];
            if (fi == NO_POS) {
                fi = ctx->family_count++;
                ctx->families[fi].tip_idx = grp->tip_idx;
                fam_of_tip[grp->tip_idx] = fi;
            }
        } else {
            fi = ctx->family_count++;
            ctx->families[fi].tip_idx = NO_POS;
        }
        Family *fam = &ctx->families[fi];
        fam->groups = xrealloc(fam->groups, (fam->group_count + 1) * sizeof(size_t));
        fam->groups[fam->group_count++] = gi;
        ctx->group_family[gi] = fi;
    }
    free(fam_of_tip);

    for (size_t fi = 0; fi < ctx->family_count; fi++) {
        Family *fam = &ctx->families[fi];
        fam->rep_group = fam->groups[0];
        fam->ll_rank = -1;
        bool all_ll = true;
        for (size_t k = 0; k < fam->group_count; k++) {
            const Group *grp = &ctx->groups[fam->groups[k]];
            if (grp->is_ll) {
                int rank = group_ll_rank(ctx, grp);
                if (fam->ll_rank < 0 || rank < fam->ll_rank)
                    fam->ll_rank = rank;
            } else {
                all_ll = false;
            }
        }
        fam->all_ll = all_ll;
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
        keys[i].name = ctx->groups[ctx->families[i].rep_group].canonical;
    }
    qsort(keys, n, sizeof(OrderKey), cmp_order_key);
    for (size_t i = 0; i < n; i++)
        ctx->family_order[i] = keys[i].idx;
    free(keys);
}

/* ---------- precalcoli sulle catene dei primi genitori ---------- */

/* f e primo commit proprio di ogni famiglia.
 *
 * Regola di appartenenza (deterministica, una sola passata): le famiglie,
 * nell'ordine stabilito (long-lived prima, poi nome), rivendicano i commit
 * della propria catena non ancora rivendicati. La "regione propria" di b e'
 * l'insieme dei commit che b ha rivendicato: f(b) e' il primo commit,
 * risalendo la catena dalla punta, rivendicato da un'altra famiglia; il primo
 * commit proprio e' il piu' vecchio commit rivendicato da b, assente se la
 * punta e' gia' rivendicata da altri (branch assorbito o fast-forward).
 * La scelta tra diramazioni sorelle (stesso punto di partenza) e' arbitraria
 * ma stabile: la storia non registra quale sia nata prima. */
static void compute_forks(Ctx *ctx)
{
    size_t n = ctx->d->commit_count;
    size_t *claimed = xmalloc((n ? n : 1) * sizeof(size_t));
    for (size_t i = 0; i < n; i++)
        claimed[i] = NO_POS;

    for (size_t oi = 0; oi < ctx->family_count; oi++) {
        size_t fi = ctx->family_order[oi];
        Family *fam = &ctx->families[fi];
        if (fam->tip_idx == NO_POS)
            continue;
        size_t cur = fam->tip_idx;
        size_t own = NO_POS;
        size_t f = NO_POS;
        while (cur != NO_POS) {
            if (claimed[cur] == NO_POS) {
                claimed[cur] = fi;
                own = cur;
            } else if (claimed[cur] != fi) {
                f = cur;
                break;
            }
            cur = first_parent(ctx, cur);
        }
        fam->f = f;
        fam->first_own = own;
    }
    free(claimed);
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

/* Matrice delle relazioni: per ogni catena (riga g) e ogni famiglia b,
 * dove stanno f(b) e la punta di b rispetto alla catena di g. Costo
 * O(Σ L_g + G²), una sola passata. */
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

static void run_pass(Ctx *ctx)
{
    size_t g = ctx->family_count;
    for (size_t oi = 0; oi < g; oi++) {
        size_t bi = ctx->family_order[oi];
        Family *b = &ctx->families[bi];
        b->cand_count = 0;
        b->date_used = false;
        if (b->tip_idx == NO_POS || b->f == NO_POS || b->all_ll)
            continue;
        const uint8_t *rel_b = &ctx->rel[bi * g];
        for (size_t xi = 0; xi < g; xi++) {
            if (xi == bi)
                continue;
            Family *beta = &ctx->families[xi];
            if (beta->tip_idx == NO_POS)
                continue;
            uint8_t rb = ctx->rel[xi * g + bi]; /* f(b), tip(b) vs catena di β */
            if (rb & REL_TIP_ON_CHAIN)
                continue; /* D1: β discende da b */
            if (rel_b[xi] & REL_F_ABOVE)
                continue; /* D2: β si e' diramato da un commit interno di b */
            if (!(rb & REL_F_ON_CHAIN))
                continue; /* D6: β non contiene il punto di diramazione */
            if (!beta->all_ll) {
                if (beta->first_own != NO_POS) {
                    if (rb & REL_F_BELOW)
                        continue; /* D3: β non esisteva a f(b) */
                } else if (beta->tip_idx != b->f) {
                    continue; /* D4: β assorbito (FF/alias), non fa da padre */
                }
                /* D5: fork uguali, decisione per data (spareggio dichiarato) */
                if (beta->f == b->f && beta->first_own != NO_POS &&
                    b->first_own != NO_POS &&
                    date_at(ctx, beta->first_own) > date_at(ctx, b->first_own)) {
                    b->date_used = true;
                    continue;
                }
            }
            b->cand = xrealloc(b->cand, (b->cand_count + 1) * sizeof(size_t));
            b->cand[b->cand_count++] = xi;
        }
    }
}

/* ---------- nomi ---------- */

/* Lista dei nomi delle famiglie indicate, in ordine deterministico di gruppo. */
static char *join_family_names(const Ctx *ctx, const size_t *fams, size_t count)
{
    /* una voce per ogni gruppo delle famiglie elencate */
    size_t *gs = xmalloc((ctx->group_count ? ctx->group_count : 1) * sizeof(size_t));
    size_t ng = 0;
    for (size_t i = 0; i < count; i++) {
        const Family *fam = &ctx->families[fams[i]];
        for (size_t k = 0; k < fam->group_count; k++)
            gs[ng++] = fam->groups[k];
    }
    /* ordina per posizione nell'ordine dei gruppi */
    for (size_t i = 1; i < ng; i++) {
        size_t key = gs[i];
        size_t j = i;
        while (j > 0 && ctx->group_rank[key] < ctx->group_rank[gs[j - 1]]) {
            gs[j] = gs[j - 1];
            j--;
        }
        gs[j] = key;
    }
    StrBuf sb;
    sb_init(&sb);
    for (size_t i = 0; i < ng; i++) {
        if (i)
            sb_append(&sb, ", ");
        sb_append(&sb, ctx->groups[gs[i]].rep_name);
    }
    free(gs);
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

typedef struct {
    ParentState state;
    char *parent;       /* da xstrdup sui nodi; ownership del chiamante */
    char *motivation;   /* idem */
    size_t fork_idx;
    size_t first_own_idx;
} Deduction;

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
        d->fork_idx = b->f;
        d->first_own_idx = b->first_own;
        if (b->f != NO_POS) {
            gitdata_short_hash(commit_at(ctx, b->f)->hash, 8, sh);
        } else {
            sh[0] = '\0';
        }

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
                    sb_appendf(&mot, "nessun branch piu' antico del punto di diramazione %s", sh);
                else
                    sb_append(&mot, "nessun punto di diramazione: nessun branch piu' antico condivide questa storia");
            } else if (nh == 1 && ctx->families[holders[0]].group_count == 1) {
                d->state = PARENT_DEDUCED;
                d->parent = xstrdup(ctx->groups[ctx->families[holders[0]].rep_group].rep_name);
                sb_appendf(&mot, "nessun commit proprio: interamente contenuto in '%s'", d->parent);
            } else {
                char *lhs = join_family_names(ctx, holders, nh);
                d->state = PARENT_AMBIGUOUS;
                if (nh == 1)
                    sb_appendf(&mot,
                               "nessun commit proprio; il contenitore e' uno dei branch sulla "
                               "stessa punta: %s", lhs);
                else
                    sb_appendf(&mot, "nessun commit proprio; piu' branch contengono la punta: %s", lhs);
                free(lhs);
            }
            free(holders);
        } else if (b->cand_count == 1 && ctx->families[b->cand[0]].group_count == 1) {
            d->state = PARENT_DEDUCED;
            d->parent = xstrdup(ctx->groups[ctx->families[b->cand[0]].rep_group].rep_name);
            sb_appendf(&mot, "punto di diramazione %s sul ramo di '%s'", sh, d->parent);
        } else {
            /* piu' nomi sulla stessa punta, o piu' candidati */
            size_t chosen = NO_POS;
            size_t count = 0;
            if (b->cand_count > 1) {
                for (size_t i = 0; i < b->cand_count; i++) {
                    const Family *c = &ctx->families[b->cand[i]];
                    bool ok = true;
                    for (size_t j = 0; j < b->cand_count && ok; j++) {
                        if (i == j)
                            continue;
                        const Family *o = &ctx->families[b->cand[j]];
                        if (!bitset_test(o->reach, c->tip_idx))
                            ok = false;
                    }
                    if (ok) {
                        chosen = b->cand[i];
                        count++;
                    }
                }
            } else {
                chosen = b->cand[0];
                count = 1;
            }
            if (count == 1 && ctx->families[chosen].group_count == 1) {
                d->state = PARENT_DEDUCED;
                d->parent = xstrdup(ctx->groups[ctx->families[chosen].rep_group].rep_name);
                if (b->cand_count > 1)
                    sb_appendf(&mot, "punto di diramazione %s; '%s' e' a monte degli altri candidati",
                               sh, d->parent);
                else
                    sb_appendf(&mot, "punto di diramazione %s sul ramo di '%s'", sh, d->parent);
            } else {
                char *lhs = join_family_names(ctx, b->cand, b->cand_count);
                d->state = PARENT_AMBIGUOUS;
                if (b->cand_count == 1)
                    sb_appendf(&mot, "punto di diramazione %s; piu' branch sulla stessa punta: %s",
                               sh, lhs);
                else
                    sb_appendf(&mot, "piu' branch contengono il punto di diramazione %s: %s", sh, lhs);
                free(lhs);
            }
        }
    }

    if (b->date_used) {
        sb_append(&mot,
                  "; scarto dei candidati basato sulla data (poco affidabile con rebase)");
    }
    d->motivation = mot.data;
}

static void deduction_free(Deduction *d)
{
    free(d->parent);
    free(d->motivation);
}

/* ---------- assegnazione ---------- */

/* Alias: gli altri gruppi della famiglia (nomi), in ordine alfabetico. */
static void set_alias_members(Ctx *ctx, const Family *fam, size_t gi, BranchNode *n)
{
    if (fam->group_count < 2)
        return;
    n->alias_members = xmalloc((fam->group_count - 1) * sizeof(char *));
    n->alias_count = 0;
    for (size_t k = 0; k < fam->group_count; k++) {
        size_t other = fam->groups[k];
        if (other == gi)
            continue;
        n->alias_members[n->alias_count++] = xstrdup(ctx->groups[other].rep_name);
    }
    qsort(n->alias_members, n->alias_count, sizeof(char *), cmp_str_ptr);
}

static void append_alias_note(Ctx *ctx, const Family *fam, size_t gi, StrBuf *mot)
{
    if (fam->group_count < 2)
        return;
    sb_append(mot, "; fa parte di un gruppo di alias con ");
    bool first = true;
    for (size_t k = 0; k < fam->group_count; k++) {
        size_t other = fam->groups[k];
        if (other == gi)
            continue;
        if (!first)
            sb_append(mot, ", ");
        first = false;
        sb_appendf(mot, "'%s'", ctx->groups[other].rep_name);
    }
    sb_append(mot, " sulla stessa punta: ordine interno non determinabile");
}

static void assign_family(Ctx *ctx, size_t fi)
{
    History *out = ctx->out;
    Family *fam = &ctx->families[fi];

    Deduction d;
    memset(&d, 0, sizeof d);
    d.fork_idx = NO_POS;
    d.first_own_idx = NO_POS;
    if (!fam->all_ll)
        deduce(ctx, fi, &d);

    for (size_t k = 0; k < fam->group_count; k++) {
        size_t gi = fam->groups[k];
        Group *g = &ctx->groups[gi];
        for (size_t mi = 0; mi < g->member_count; mi++) {
            size_t node = g->members[mi];
            BranchNode *n = &out->branches[node];
            set_alias_members(ctx, fam, gi, n);

            if (node != g->rep_node) {
                /* specchio: remote-tracking del rappresentante */
                char mot[512];
                snprintf(mot, sizeof mot, "remote-tracking di '%s'", g->rep_name);
                set_parent(n, g->rep_name, PARENT_CERTAIN, mot);
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
                append_alias_note(ctx, fam, gi, &mot);
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
                if (ov_note[0]) {
                    sb_append(&mot, "; ");
                    sb_append(&mot, ov_note);
                }
                append_alias_note(ctx, fam, gi, &mot);
                set_parent(n, ov_parent, PARENT_CONFIGURED, mot.data);
            } else {
                sb_append(&mot, d.motivation ? d.motivation : "");
                if (ov_note[0]) {
                    if (mot.len)
                        sb_append(&mot, "; ");
                    sb_append(&mot, ov_note);
                }
                append_alias_note(ctx, fam, gi, &mot);
                set_parent(n, d.parent, d.state, mot.data);
            }
            sb_free(&mot);
        }
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
 * incoerenti). Per ciascun ciclo il nodo con nome minore diventa ambiguo:
 * l'euristica non inventa un padre per una relazione circolare. */
static void break_parent_cycles(History *out)
{
    for (size_t guard = 0; guard <= out->branch_count; guard++) {
        bool found = false;
        for (size_t i = 0; i < out->branch_count && !found; i++) {
            if (!out->branches[i].parent)
                continue;
            size_t cur = i;
            bool cycle = false;
            for (size_t step = 0; step < out->branch_count; step++) {
                const char *pname = out->branches[cur].parent;
                if (!pname)
                    break;
                size_t p = history_find_branch(out, pname);
                if (p == NO_POS)
                    break;
                if (p == i) {
                    cycle = true;
                    break;
                }
                cur = p;
            }
            if (!cycle)
                continue;
            size_t best = i;
            cur = i;
            for (;;) {
                if (strcmp(out->branches[cur].name, out->branches[best].name) < 0)
                    best = cur;
                const char *pname = out->branches[cur].parent;
                if (!pname)
                    break;
                size_t p = history_find_branch(out, pname);
                if (p == NO_POS || p == i)
                    break;
                cur = p;
            }
            BranchNode *bn = &out->branches[best];
            free(bn->parent);
            bn->parent = NULL;
            bn->state = PARENT_AMBIGUOUS;
            free(bn->motivation);
            bn->motivation =
                xstrdup("paternita' circolare tra branch: relazione non determinabile");
            found = true;
        }
        if (!found)
            break;
    }
}

bool history_build(History *out, const GitData *data, const GraphConfig *cfg,
                   char *err, size_t err_size)
{
    memset(out, 0, sizeof *out);
    out->data = data;

    Ctx ctx;
    ctx_init(&ctx, out, data, cfg);
    (void)err;
    (void)err_size;

    if (ctx.group_count > 0) {
        finalize_groups(&ctx);
        build_group_order(&ctx);
        build_families(&ctx);
        build_family_order(&ctx);

        size_t n = data->commit_count;
        ctx.scratch_pos = xmalloc((n ? n : 1) * sizeof(size_t));
        ctx.scratch_gen = xcalloc(n ? n : 1, sizeof(uint32_t));
        ctx.stack = xmalloc((n ? n : 1) * sizeof(size_t));

        compute_forks(&ctx);

        for (size_t fi = 0; fi < ctx.family_count; fi++) {
            ctx.families[fi].reach = bitset_new(n);
            reach_build(&ctx, fi, ctx.families[fi].reach);
        }
        build_rel(&ctx);
        run_pass(&ctx);

        for (size_t oi = 0; oi < ctx.family_count; oi++)
            assign_family(&ctx, ctx.family_order[oi]);

        break_parent_cycles(out);
    }

    strmap_free(ctx.canonical_map);
    strmap_free(ctx.name_map);
    free(ctx.group_family);
    free(ctx.group_order);
    free(ctx.group_rank);
    free(ctx.family_order);
    free(ctx.rel);
    free(ctx.scratch_pos);
    free(ctx.scratch_gen);
    free(ctx.stack);
    for (size_t gi = 0; gi < ctx.group_count; gi++)
        group_free(&ctx.groups[gi]);
    free(ctx.groups);
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

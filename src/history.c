/* history.c - vedi history.h
 *
 * Euristica (documentata, deterministica):
 *
 * 1. Ogni "gruppo" di branch ha un rappresentante (il branch locale, se
 *    esiste, altrimenti il primo per nome). I remote-tracking con lo stesso
 *    nome canonico sono specchi: non partecipano alla deduzione.
 *
 * 2. Punto di diramazione f(b): il genitore del primo commit esclusivo di b
 *    (il primo commit della catena dei primi genitori non contenuto in nessun
 *    altro gruppo). Se b e' interamente contenuto altrove, f(b) = punta di b
 *    (equivalente del merge-base con il contenitore).
 *
 * 3. Un gruppo β e' candidato padre di b se contiene f(b), salvo esclusione
 *    per le regole di direzione D1..D3 (b discende da β, non viceversa):
 *      D1  la punta di β e' discendente (primi genitori) della punta di b;
 *      D2  f(β) sta sulla catena dei primi genitori di b, piu' in alto di f(b);
 *      D3  f(β) == f(b): si distingue per il primo commit esclusivo, usando la
 *          data solo come spareggio (in caso di parita': ambiguo);
 *      D4  (filtro di esistenza) β non esisteva a f(b): f(b) e' antenato
 *          stretto di f(β).
 *
 * 4. Scelta del padre fra i candidati:
 *      a. se un solo candidato ha f(b) sulla propria catena di primi genitori;
 *      b. filtro di esistenza: se ne resta uno solo;
 *      c. se un solo candidato e' antenato (o uguale) di tutti gli altri;
 *      d. altrimenti: ambiguo (nessun padre inventato).
 *
 * 5. Casi speciali: branch che puntano allo stesso commit di un altro gruppo:
 *    sempre ambigui. Gruppo senza alcun candidato: radice indipendente, con
 *    fallback "nessun commit esclusivo" se l'intera punta e' contenuta.
 *
 * L'euristica non modifica mai il grafo reale.
 */
#include "history.h"

#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NO_POS ((size_t)-1)

typedef struct {
    char *canonical;   /* nome canonico del gruppo */
    char *rep_name;    /* nome del rappresentante */
    size_t rep_node;   /* indice in History.branches del rappresentante */
    size_t *members;   /* indici nodo (rappresentante + specchi) */
    size_t member_count;
    size_t tip_idx;    /* indice del commit punta del rappresentante */
    bool is_ll;        /* gruppo long-lived (dalla configurazione) */
    BitSet *reach;     /* commit raggiungibili (unione dei membri) */
    /* risultati delle passate */
    size_t *cand;      /* indici gruppo candidati */
    size_t *cand_stamp; /* passata in cui il candidato e' stato scartato */
    size_t cand_count;
    size_t fork_idx;   /* SIZE_MAX se nessuno */
    size_t birth_idx;  /* genitore del piu' vecchio commit esclusivo; se non
                          ci sono commit esclusivi: la punta */
    size_t first_excl_idx;
} Group;

typedef struct {
    const GitData *d;
    const GraphConfig *cfg;
    StrMap *idx_by_hash;   /* hash -> (indice+1) */
    Group *groups;
    size_t group_count;
    History *out;
} Ctx;

/* ---------- helper su hash/commit ---------- */

static size_t hash_index(const Ctx *ctx, const char *hash)
{
    if (!hash)
        return NO_POS;
    void *v = strmap_get(ctx->idx_by_hash, hash);
    if (!v)
        return NO_POS;
    return (size_t)(uintptr_t)v - 1;
}

static const GitCommit *commit_at(const Ctx *ctx, size_t idx)
{
    if (idx == NO_POS || idx >= ctx->d->commit_count)
        return NULL;
    return &ctx->d->commits[idx];
}

/* Primo genitore, oppure NO_POS. */
static size_t first_parent(const Ctx *ctx, size_t idx)
{
    const GitCommit *c = commit_at(ctx, idx);
    if (!c || c->parent_count == 0)
        return NO_POS;
    return hash_index(ctx, c->parents[0]);
}

/* ---------- insiemi di raggiungibilita' ---------- */

static void reach_dfs(const Ctx *ctx, size_t tip, BitSet *set)
{
    if (tip == NO_POS)
        return;
    size_t n = ctx->d->commit_count;
    size_t *stack = xmalloc((n ? n : 1) * sizeof(size_t));
    size_t sp = 0;
    stack[sp++] = tip;
    bitset_set(set, tip);
    while (sp > 0) {
        size_t cur = stack[--sp];
        const GitCommit *c = &ctx->d->commits[cur];
        for (size_t k = 0; k < c->parent_count; k++) {
            size_t p = hash_index(ctx, c->parents[k]);
            if (p != NO_POS && !bitset_test(set, p)) {
                bitset_set(set, p);
                stack[sp++] = p;
            }
        }
    }
    free(stack);
}

static bool group_contains(const Group *g, size_t commit_idx)
{
    return commit_idx != NO_POS && bitset_test(g->reach, commit_idx);
}

/* ---------- catena dei primi genitori ---------- */

/* Posizione di `target` sulla catena dei primi genitori del gruppo (0 = punta),
 * oppure NO_POS. */
static size_t fp_position(const Ctx *ctx, const Group *g, size_t target)
{
    size_t cur = g->tip_idx;
    size_t pos = 0;
    while (cur != NO_POS) {
        if (cur == target)
            return pos;
        cur = first_parent(ctx, cur);
        pos++;
    }
    return NO_POS;
}

/* true se `tip_of` (gruppo) e' discendente stretto via primi genitori di
 * `commit_idx`. */
static bool is_first_parent_descendant(const Ctx *ctx, size_t tip_of, size_t commit_idx)
{
    size_t cur = first_parent(ctx, tip_of);
    while (cur != NO_POS) {
        if (cur == commit_idx)
            return true;
        cur = first_parent(ctx, cur);
    }
    return false;
}

/* true se `anc` e' antenato stretto di `of` (qualsiasi catena). */
static bool is_strict_ancestor(const Ctx *ctx, size_t anc, size_t of)
{
    if (anc == NO_POS || of == NO_POS || anc == of)
        return false;
    size_t n = ctx->d->commit_count;
    bool *seen = xcalloc(n, sizeof(bool));
    size_t *stack = xmalloc((n ? n : 1) * sizeof(size_t));
    size_t sp = 0;
    seen[of] = true;
    stack[sp++] = of;
    bool found = false;
    while (sp > 0 && !found) {
        size_t cur = stack[--sp];
        const GitCommit *c = &ctx->d->commits[cur];
        for (size_t k = 0; k < c->parent_count; k++) {
            size_t p = hash_index(ctx, c->parents[k]);
            if (p == NO_POS || seen[p])
                continue;
            if (p == anc) {
                found = true;
                break;
            }
            seen[p] = true;
            stack[sp++] = p;
        }
    }
    free(stack);
    free(seen);
    return found;
}

/* Evidenza che il gruppo β ha integrato b con un merge (b compare come
 * genitore non primo di un merge nella storia di β). */
static bool merge_back_evidence(const Ctx *ctx, const Group *beta, const Group *b)
{
    if (b->tip_idx == NO_POS)
        return false;
    size_t n = ctx->d->commit_count;
    size_t *stack = xmalloc((n ? n : 1) * sizeof(size_t));
    bool *seen = xcalloc(n, sizeof(bool));
    size_t sp = 0;
    seen[beta->tip_idx] = true;
    stack[sp++] = beta->tip_idx;
    bool found = false;
    while (sp > 0 && !found) {
        size_t cur = stack[--sp];
        const GitCommit *c = &ctx->d->commits[cur];
        if (c->parent_count >= 2) {
            for (size_t k = 1; k < c->parent_count && !found; k++) {
                size_t p = hash_index(ctx, c->parents[k]);
                if (p == NO_POS)
                    continue;
                /* b->tip raggiungibile da p? */
                if (p == b->tip_idx || is_strict_ancestor(ctx, b->tip_idx, p))
                    found = true;
            }
        }
        for (size_t k = 0; k < c->parent_count; k++) {
            size_t p = hash_index(ctx, c->parents[k]);
            if (p != NO_POS && !seen[p]) {
                seen[p] = true;
                stack[sp++] = p;
            }
        }
    }
    free(stack);
    free(seen);
    return found;
}

/* ---------- costruzione dei gruppi ---------- */

static int cmp_size_t(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Ordine deterministico: long-lived prima (ordine di configurazione), poi per
 * nome. */
static int group_order_cmp(const Ctx *ctx, size_t ga, size_t gb)
{
    const Group *a = &ctx->groups[ga];
    const Group *b = &ctx->groups[gb];
    int ra = config_long_lived_rank(ctx->cfg, a->canonical);
    int rb = config_long_lived_rank(ctx->cfg, b->canonical);
    if ((ra >= 0) != (rb >= 0))
        return ra >= 0 ? -1 : 1;
    if (ra >= 0 && rb >= 0 && ra != rb)
        return ra < rb ? -1 : 1;
    return strcmp(a->canonical, b->canonical);
}

static size_t *sort_group_indices(const Ctx *ctx)
{
    size_t *order = xmalloc(ctx->group_count * sizeof(size_t));
    for (size_t i = 0; i < ctx->group_count; i++)
        order[i] = i;
    /* insertion sort (stabile, n piccolo) */
    for (size_t i = 1; i < ctx->group_count; i++) {
        size_t key = order[i];
        size_t j = i;
        while (j > 0 && group_order_cmp(ctx, key, order[j - 1]) < 0) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }
    return order;
}

/* Costruzione dei gruppi e dei nodi: vedi ctx_init. */

static void group_free(Group *g)
{
    free(g->canonical);
    free(g->rep_name);
    free(g->members);
    free(g->cand);
    free(g->cand_stamp);
    bitset_free(g->reach);
}

/* ---------- creazione dei nodi e dei gruppi ---------- */

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
    n->first_exclusive_pos = NO_POS;
    return out->branch_count++;
}

/* ---------- costruzione completa ---------- */

static bool ctx_init(Ctx *ctx, History *out, const GitData *data, const GraphConfig *cfg,
                     char *err, size_t err_size)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->d = data;
    ctx->cfg = cfg;
    ctx->out = out;
    ctx->idx_by_hash = strmap_new();
    for (size_t i = 0; i < data->commit_count; i++)
        strmap_put(ctx->idx_by_hash, data->commits[i].hash, (void *)(uintptr_t)(i + 1));

    /* nodi per tutti i riferimenti branch, con nome canonico */
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
        /* nodo (uno per riferimento) */
        (void)add_node(out, r, canonical);

        size_t gi = NO_POS;
        for (size_t k = 0; k < ctx->group_count; k++) {
            if (strcmp(ctx->groups[k].canonical, canonical) == 0) {
                gi = k;
                break;
            }
        }
        if (gi == NO_POS) {
            gi = ctx->group_count++;
            Group *g = &ctx->groups[gi];
            memset(g, 0, sizeof *g);
            g->canonical = xstrdup(canonical);
            g->rep_node = NO_POS;
            g->tip_idx = NO_POS;
            g->fork_idx = NO_POS;
            g->first_excl_idx = NO_POS;
        }
        Group *g = &ctx->groups[gi];
        size_t node = out->branch_count - 1;
        g->members = xrealloc(g->members, (g->member_count + 1) * sizeof(size_t));
        g->members[g->member_count++] = node;
    }
    if (out->branch_count == 0 && data->commit_count > 0) {
        /* nessun branch: solo tag/HEAD scollegata; il grafo resta valido */
    }
    (void)err;
    (void)err_size;
    return true;
}

/* Sceglie per ogni gruppo il rappresentante (locale preferito), marca gli
 * specchi e assegna i rank long-lived. */
static void finalize_groups(Ctx *ctx)
{
    History *out = ctx->out;
    for (size_t gi = 0; gi < ctx->group_count; gi++) {
        Group *g = &ctx->groups[gi];
        /* ordina i membri per indice nodo (nome) per determinismo */
        qsort(g->members, g->member_count, sizeof(size_t), cmp_size_t);
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
        g->tip_idx = hash_index(ctx, r->target);

        /* specchi */
        for (size_t k = 0; k < g->member_count; k++) {
            size_t node = g->members[k];
            if (node == rep)
                continue;
            out->branches[node].mirror_of = xstrdup(out->branches[rep].name);
        }

        /* long-lived */
        int rank = config_long_lived_rank(ctx->cfg, g->canonical);
        g->is_ll = (rank >= 0);
        for (size_t k = 0; k < g->member_count; k++) {
            BranchNode *n = &out->branches[g->members[k]];
            n->is_long_lived = (rank >= 0);
            n->long_lived_rank = rank;
        }
    }
}

static void compute_reach(Ctx *ctx)
{
    for (size_t gi = 0; gi < ctx->group_count; gi++) {
        Group *g = &ctx->groups[gi];
        g->reach = bitset_new(ctx->d->commit_count);
        for (size_t k = 0; k < g->member_count; k++) {
            const GitRef *r = &ctx->out->data->refs[ctx->out->branches[g->members[k]].ref_index];
            size_t tip = hash_index(ctx, r->target);
            reach_dfs(ctx, tip, g->reach);
        }
    }
}

/* Calcola, risalendo la catena dei primi genitori dalla punta:
 *  - f: il primo commit condiviso con un altro gruppo (merge-base); se non
 *    esiste alcun commit condiviso, NO_POS (storia indipendente);
 *  - e1: il piu' vecchio commit esclusivo del gruppo (NO_POS se il gruppo e'
 *    interamente contenuto in altri).
 * f e' anche il genitore di e1 quando e1 esiste. */
static void compute_fork_and_e1(Ctx *ctx, const Group *g, size_t *f_out, size_t *e1_out)
{
    size_t cur = g->tip_idx;
    size_t oldest_excl = NO_POS;
    size_t f = NO_POS;
    while (cur != NO_POS) {
        bool shared = false;
        for (size_t gi = 0; gi < ctx->group_count && !shared; gi++) {
            if (&ctx->groups[gi] == g)
                continue;
            if (group_contains(&ctx->groups[gi], cur))
                shared = true;
        }
        if (shared) {
            f = cur;
            break;
        }
        oldest_excl = cur;
        cur = first_parent(ctx, cur);
    }
    *f_out = f;
    *e1_out = oldest_excl;
}

/* ---------- direzione e validita' dei candidati ---------- */

/* Verifica D1/D2/D3 per il candidato β su b. Restituisce NULL se valido,
 * altrimenti una descrizione della motivazione dell'esclusione. `fprev_*`
 * sono i punti di diramazione dell'ultima passata. */
static const char *candidate_reject_reason(Ctx *ctx, size_t b_gi, size_t beta_gi,
                                           size_t fprev_b, size_t fprev_beta)
{
    const Group *b = &ctx->groups[b_gi];
    const Group *beta = &ctx->groups[beta_gi];

    if (b->tip_idx != NO_POS && beta->tip_idx != NO_POS &&
        is_first_parent_descendant(ctx, beta->tip_idx, b->tip_idx))
        return "discende da b lungo i primi genitori";

    if (fprev_beta != NO_POS && fprev_b != NO_POS) {
        size_t pos_beta = fp_position(ctx, b, fprev_beta);
        size_t pos_b = fp_position(ctx, b, fprev_b);
        if (pos_beta != NO_POS && pos_b != NO_POS && pos_beta < pos_b)
            return "si e' diramato da un commit interno di b";
    }

    if (fprev_beta != NO_POS && fprev_beta == fprev_b) {
        /* fork comune: direzione per primo commit esclusivo + data (spareggio) */
        size_t e1_b = b->first_excl_idx;
        size_t e1_beta = beta->first_excl_idx;
        if (e1_b == NO_POS && !merge_back_evidence(ctx, beta, b))
            return "discende da b (nessun commit esclusivo di b)";
        if (e1_b == NO_POS)
            return NULL; /* merge back: b integrato in β */
        if (e1_beta == NO_POS)
            return NULL; /* b e' il lato figlio: mantieni β */
        int64_t t_b = commit_at(ctx, e1_b) ? commit_at(ctx, e1_b)->date : 0;
        int64_t t_beta = commit_at(ctx, e1_beta) ? commit_at(ctx, e1_beta)->date : 0;
        if (t_beta > t_b)
            return "primo commit esclusivo piu' recente di quello di b";
        if (t_beta == t_b)
            return "fork comune non distinguibile (date uguali)";
    }
    return NULL;
}

/* Filtro di esistenza D4. */
static bool candidate_did_not_exist(Ctx *ctx, size_t f_b, size_t f_beta)
{
    if (f_b == NO_POS || f_beta == NO_POS)
        return false;
    return is_strict_ancestor(ctx, f_b, f_beta);
}

/* La punta di gj giace sulla catena dei primi genitori di un altro gruppo?
 * Vale per branch "assorbiti sul ramo di qualcun altro" (FF, alias sulla
 * stessa punta): la loro storia non e' una linea propria. */
static bool tip_on_other_fp_chain(const Ctx *ctx, size_t gj)
{
    size_t tip = ctx->groups[gj].tip_idx;
    if (tip == NO_POS)
        return false;
    for (size_t gk = 0; gk < ctx->group_count; gk++) {
        if (gk == gj || ctx->groups[gk].tip_idx == NO_POS)
            continue;
        if (fp_position(ctx, &ctx->groups[gk], tip) != NO_POS)
            return true;
    }
    return false;
}

/* Una passata: calcola i candidati validi per ogni gruppo. I punti di
 * diramazione f[] sono fissi (calcolati dal primo commit esclusivo). */
static void run_pass(Ctx *ctx, const size_t *f, size_t *order)
{
    for (size_t oi = 0; oi < ctx->group_count; oi++) {
        size_t gi = order[oi];
        Group *b = &ctx->groups[gi];
        b->cand_count = 0;
        if (b->tip_idx == NO_POS)
            continue;
        size_t f_b = f[gi];
        if (f_b == NO_POS)
            continue; /* storia indipendente: nessun punto di diramazione */
        for (size_t gj = 0; gj < ctx->group_count; gj++) {
            if (gj == gi || ctx->groups[gj].tip_idx == NO_POS)
                continue;
            const char *reason = candidate_reject_reason(ctx, gi, gj, f_b, f[gj]);
            if (reason)
                continue;
            /* filtro di esistenza: si applica solo se la nascita del
             * candidato e' determinabile (ha commit esclusivi) e il candidato
             * non e' long-lived (il tronco esiste da sempre) */
            if (!ctx->groups[gj].is_ll &&
                ctx->groups[gj].first_excl_idx != NO_POS &&
                candidate_did_not_exist(ctx, f_b, ctx->groups[gj].birth_idx))
                continue;
            /* candidato: deve contenere il punto di diramazione */
            if (f_b != NO_POS && !group_contains(&ctx->groups[gj], f_b))
                continue;
            /* branch assorbito (senza commit esclusivi e non long-lived):
             * resta candidato solo se e' esattamente il punto di diramazione
             * (es. piu' branch sulla stessa punta) o se la sua punta non e'
             * appoggiata sulla catena di un altro branch. Un branch mergiato
             * (linea propria) puo' quindi fare da padre; un FF/alias no. */
            if (!ctx->groups[gj].is_ll &&
                ctx->groups[gj].first_excl_idx == NO_POS &&
                ctx->groups[gj].tip_idx != f_b &&
                tip_on_other_fp_chain(ctx, gj))
                continue;
            b->cand = xrealloc(b->cand, (b->cand_count + 1) * sizeof(size_t));
            b->cand[b->cand_count++] = gj;
        }
    }
}

/* ---------- assegnazione del padre ---------- */

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

/* Costruisce "a, b, c". */
static char *join_group_names(const Ctx *ctx, const size_t *gidx, size_t count)
{
    StrBuf sb;
    sb_init(&sb);
    for (size_t i = 0; i < count; i++) {
        if (i)
            sb_append(&sb, ", ");
        sb_append(&sb, ctx->groups[gidx[i]].rep_name);
    }
    return sb.data;
}

static void assign_parent(Ctx *ctx, size_t gi)
{
    Group *b = &ctx->groups[gi];
    BranchNode *n = &ctx->out->branches[b->rep_node];
    char shortbuf[16];

    /* specchi: remote-tracking del rappresentante */
    for (size_t k = 0; k < b->member_count; k++) {
        size_t node = b->members[k];
        if (node == b->rep_node)
            continue;
        BranchNode *m = &ctx->out->branches[node];
        char mot[512];
        snprintf(mot, sizeof mot, "remote-tracking di '%s'", b->rep_name);
        set_parent(m, b->rep_name, PARENT_CERTAIN, mot);
    }

    if (b->tip_idx == NO_POS) {
        set_parent(n, NULL, PARENT_NONE, "nessun commit raggiungibile da questo riferimento");
        return;
    }

    /* candidati informativi (in ordine deterministico) */
    {
        size_t *sorted = xmalloc((b->cand_count ? b->cand_count : 1) * sizeof(size_t));
        for (size_t i = 0; i < b->cand_count; i++)
            sorted[i] = b->cand[i];
        for (size_t i = 1; i < b->cand_count; i++) {
            size_t key = sorted[i];
            size_t j = i;
            while (j > 0 && group_order_cmp(ctx, key, sorted[j - 1]) < 0) {
                sorted[j] = sorted[j - 1];
                j--;
            }
            sorted[j] = key;
        }
        char **names = xmalloc((b->cand_count ? b->cand_count : 1) * sizeof(char *));
        for (size_t i = 0; i < b->cand_count; i++)
            names[i] = xstrdup(ctx->groups[sorted[i]].rep_name);
        for (size_t i = 0; i < n->candidate_count; i++)
            free(n->candidates[i]);
        free(n->candidates);
        n->candidates = names;
        n->candidate_count = b->cand_count;
        free(sorted);
    }

    /* override da configurazione (con precedenza sull'euristica) */
    const char *ov_parent = NULL;
    char ov_note[256] = "";
    if (ctx->cfg) {
        for (size_t oi = 0; oi < ctx->cfg->override_count; oi++) {
            const ParentOverride *ov = &ctx->cfg->overrides[oi];
            if (strcmp(ov->branch, n->name) != 0)
                continue;
            if (history_find_branch(ctx->out, ov->parent) != NO_POS) {
                ov_parent = ov->parent;
            } else {
                snprintf(ov_note, sizeof ov_note,
                         "override da graph.cfg ignorato: branch '%s' inesistente",
                         ov->parent);
            }
            break;
        }
    }

    const char *parent = NULL;
    ParentState st = PARENT_NONE;
    size_t fork = NO_POS;
    StrBuf mot;
    sb_init(&mot);

    if (ov_parent) {
        parent = ov_parent;
        st = PARENT_CERTAIN;
        fork = b->fork_idx;
        sb_appendf(&mot, "override da graph.cfg: padre '%s'", ov_parent);
    } else if (config_long_lived_rank(ctx->cfg, b->canonical) >= 0) {
        /* i long-lived non ricevono un padre dall'euristica: sono il tronco o
         * le linee principali, ordinate dalla configurazione */
        fork = b->fork_idx;
        st = PARENT_NONE;
        sb_append(&mot, "tronco long-lived (configurazione): padre non dedotto");
    } else {
        /* punta uguale ad un altro gruppo: sempre ambiguo */
        size_t same[64];
        size_t nsame = 0;
        for (size_t gj = 0; gj < ctx->group_count && nsame < 64; gj++) {
            if (gj == gi || ctx->groups[gj].tip_idx == NO_POS)
                continue;
            if (ctx->groups[gj].tip_idx == b->tip_idx)
                same[nsame++] = gj;
        }
        if (nsame > 0) {
            char *lhs = join_group_names(ctx, same, nsame);
            gitdata_short_hash(commit_at(ctx, b->tip_idx)->hash, 8, shortbuf);
            sb_appendf(&mot,
                       "stesso commit della punta di %s (%s): padre non determinabile",
                       lhs, shortbuf);
            free(lhs);
            st = PARENT_AMBIGUOUS;
        } else if (b->cand_count == 0) {
            /* fallback: nessun commit esclusivo, storia interamente contenuta */
            size_t holders[64];
            size_t nhold = 0;
            for (size_t gj = 0; gj < ctx->group_count && nhold < 64; gj++) {
                if (gj == gi || ctx->groups[gj].tip_idx == NO_POS)
                    continue;
                if (group_contains(&ctx->groups[gj], b->tip_idx))
                    holders[nhold++] = gj;
            }
            if (nhold == 1) {
                parent = ctx->groups[holders[0]].rep_name;
                st = PARENT_DEDUCED;
                fork = b->fork_idx;
                sb_appendf(&mot,
                           "nessun commit esclusivo: interamente contenuto in '%s'",
                           parent);
            } else if (nhold > 1) {
                char *lhs = join_group_names(ctx, holders, nhold);
                sb_appendf(&mot,
                           "nessun commit esclusivo; piu' branch contengono la punta: %s",
                           lhs);
                free(lhs);
                st = PARENT_AMBIGUOUS;
            } else {
                st = PARENT_NONE;
                sb_appendf(&mot,
                           "nessun altro branch condivide la storia (radice indipendente)");
            }
        } else {
            size_t f = b->fork_idx;

            /* regola a: unico candidato con f sulla catena dei primi genitori */
            size_t chosen = NO_POS;
            size_t count = 0;
            for (size_t k = 0; k < b->cand_count; k++) {
                if (fp_position(ctx, &ctx->groups[b->cand[k]], f) != NO_POS) {
                    chosen = b->cand[k];
                    count++;
                }
            }
            if (count == 1) {
                parent = ctx->groups[chosen].rep_name;
                st = PARENT_DEDUCED;
                fork = f;
                gitdata_short_hash(commit_at(ctx, f)->hash, 8, shortbuf);
                sb_appendf(&mot, "punto di diramazione %s sul ramo di '%s'",
                           shortbuf, parent);
            } else {
                /* regola b: filtro di esistenza */
                size_t remaining[64];
                size_t nrem = 0;
                for (size_t k = 0; k < b->cand_count && nrem < 64; k++) {
                    size_t gj = b->cand[k];
                    if (!ctx->groups[gj].is_ll &&
                        ctx->groups[gj].first_excl_idx != NO_POS &&
                        candidate_did_not_exist(ctx, f, ctx->groups[gj].birth_idx))
                        continue;
                    remaining[nrem++] = gj;
                }
                if (nrem == 1) {
                    parent = ctx->groups[remaining[0]].rep_name;
                    st = PARENT_DEDUCED;
                    fork = f;
                    gitdata_short_hash(commit_at(ctx, f)->hash, 8, shortbuf);
                    sb_appendf(&mot,
                               "punto di diramazione %s; unico branch esistente allora: '%s'",
                               shortbuf, parent);
                } else {
                    /* regola c: unico candidato antenato (o uguale) di tutti */
                    chosen = NO_POS;
                    count = 0;
                    for (size_t i = 0; i < nrem; i++) {
                        bool ok = true;
                        for (size_t j = 0; j < nrem && ok; j++) {
                            if (i == j)
                                continue;
                            size_t a = remaining[i], c = remaining[j];
                            if (ctx->groups[a].tip_idx == ctx->groups[c].tip_idx)
                                continue;
                            if (!group_contains(&ctx->groups[c], ctx->groups[a].tip_idx))
                                ok = false;
                        }
                        if (ok) {
                            chosen = remaining[i];
                            count++;
                        }
                    }
                    if (count == 1) {
                        parent = ctx->groups[chosen].rep_name;
                        st = PARENT_DEDUCED;
                        fork = f;
                        gitdata_short_hash(commit_at(ctx, f)->hash, 8, shortbuf);
                        sb_appendf(&mot,
                                   "punto di diramazione %s; '%s' e' a monte degli altri candidati",
                                   shortbuf, parent);
                    } else {
                        char *lhs = join_group_names(ctx, remaining, nrem);
                        gitdata_short_hash(commit_at(ctx, f)->hash, 8, shortbuf);
                        sb_appendf(&mot,
                                   "piu' branch contengono il punto di diramazione %s: %s",
                                   shortbuf, lhs);
                        free(lhs);
                        st = PARENT_AMBIGUOUS;
                        fork = f;
                    }
                }
            }
        }
    }

    if (ov_note[0]) {
        if (mot.len)
            sb_append(&mot, "; ");
        sb_append(&mot, ov_note);
    }

    set_parent(n, parent, st, mot.data);
    sb_free(&mot);
    set_fork(n, ctx, fork);
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
            /* nodo con nome minore del ciclo */
            size_t best = i;
            cur = i;
            for (;;) {
                if (strcmp(out->branches[cur].name,
                           out->branches[best].name) < 0)
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
    ctx_init(&ctx, out, data, cfg, err, err_size);

    if (ctx.group_count == 0) {
        /* Nessun branch: niente da dedurre (solo tag). */
        strmap_free(ctx.idx_by_hash);
        for (size_t gi = 0; gi < ctx.group_count; gi++)
            group_free(&ctx.groups[gi]);
        free(ctx.groups);
        return true;
    }

    finalize_groups(&ctx);
    compute_reach(&ctx);

    /* primo commit esclusivo e punto di diramazione per ogni gruppo */
    size_t *f = xcalloc(ctx.group_count, sizeof(size_t));
    for (size_t gi = 0; gi < ctx.group_count; gi++) {
        Group *g = &ctx.groups[gi];
        if (g->tip_idx == NO_POS) {
            f[gi] = NO_POS;
            g->first_excl_idx = NO_POS;
            continue;
        }
        compute_fork_and_e1(&ctx, g, &f[gi], &g->first_excl_idx);
        g->fork_idx = f[gi];
        g->birth_idx = (g->first_excl_idx != NO_POS)
                           ? first_parent(&ctx, g->first_excl_idx)
                           : g->tip_idx;
    }

    /* i candidati si stabilizzano in poche passate (di norma una) */
    size_t *order = sort_group_indices(&ctx);
    for (int pass = 0; pass < 3; pass++)
        run_pass(&ctx, f, order);

    /* assegnazione del padre (gruppi in ordine deterministico) */
    for (size_t oi = 0; oi < ctx.group_count; oi++) {
        size_t gi = order[oi];
        assign_parent(&ctx, gi);
        /* salva fork e primo commit esclusivo sul nodo rappresentante */
        Group *g = &ctx.groups[gi];
        BranchNode *n = &out->branches[g->rep_node];
        if (n->fork_hash == NULL && g->fork_idx != NO_POS)
            set_fork(n, &ctx, g->fork_idx);
        if (g->first_excl_idx != NO_POS) {
            const GitCommit *c = commit_at(&ctx, g->first_excl_idx);
            n->first_exclusive_hash = xstrdup(c->hash);
            n->first_exclusive_pos = g->first_excl_idx;
        }
        /* propaga fork/first-exclusive agli specchi del gruppo */
        for (size_t k = 0; k < g->member_count; k++) {
            size_t node = g->members[k];
            if (node == g->rep_node)
                continue;
            BranchNode *m = &out->branches[node];
            if (g->fork_idx != NO_POS)
                set_fork(m, &ctx, g->fork_idx);
            if (g->first_excl_idx != NO_POS) {
                const GitCommit *c = commit_at(&ctx, g->first_excl_idx);
                free(m->first_exclusive_hash);
                m->first_exclusive_hash = xstrdup(c->hash);
                m->first_exclusive_pos = g->first_excl_idx;
            }
        }
    }

    /* eventuali cicli creati dagli override: risolti in modo deterministico */
    break_parent_cycles(out);

    free(order);
    free(f);
    strmap_free(ctx.idx_by_hash);
    for (size_t gi = 0; gi < ctx.group_count; gi++)
        group_free(&ctx.groups[gi]);
    free(ctx.groups);
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
        free(n->first_exclusive_hash);
        for (size_t k = 0; k < n->candidate_count; k++)
            free(n->candidates[k]);
        free(n->candidates);
    }
    free(h->branches);
    memset(h, 0, sizeof *h);
}

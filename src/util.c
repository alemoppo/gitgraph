/* util.c - vedi util.h */
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "gitgraph: out of memory (%zu bytes)\n", n);
        exit(1);
    }
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) {
        fprintf(stderr, "gitgraph: out of memory\n");
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "gitgraph: out of memory (%zu bytes)\n", n);
        exit(1);
    }
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *r = xmalloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

char *xstrndup(const char *s, size_t n)
{
    char *r = xmalloc(n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

/* ---- StrBuf ---- */

void sb_init(StrBuf *sb)
{
    sb->cap = 64;
    sb->data = xmalloc(sb->cap);
    sb->data[0] = '\0';
    sb->len = 0;
}

void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

void sb_clear(StrBuf *sb)
{
    sb->len = 0;
    if (sb->data)
        sb->data[0] = '\0';
}

static void sb_reserve(StrBuf *sb, size_t extra)
{
    if (sb->len + extra + 1 <= sb->cap)
        return;
    while (sb->len + extra + 1 > sb->cap)
        sb->cap *= 2;
    sb->data = xrealloc(sb->data, sb->cap);
}

void sb_appendn(StrBuf *sb, const char *s, size_t n)
{
    sb_reserve(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_append(StrBuf *sb, const char *s)
{
    sb_appendn(sb, s, strlen(s));
}

void sb_appendf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    sb_reserve(sb, (size_t)n);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
    sb->len += (size_t)n;
    va_end(ap2);
}

/* ---- StrMap ---- */

typedef struct {
    uint64_t hash;
    const char *key; /* NULL = slot libero */
    void *value;
} StrMapSlot;

struct StrMap {
    StrMapSlot *slots;
    size_t cap;   /* potenza di due */
    size_t count;
};

static uint64_t fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

StrMap *strmap_new(void)
{
    StrMap *m = xmalloc(sizeof *m);
    m->cap = 64;
    m->count = 0;
    m->slots = xcalloc(m->cap, sizeof(StrMapSlot));
    return m;
}

void strmap_free(StrMap *m)
{
    if (!m)
        return;
    free(m->slots);
    free(m);
}

static void strmap_grow(StrMap *m)
{
    size_t oldcap = m->cap;
    StrMapSlot *old = m->slots;
    m->cap *= 2;
    m->slots = xcalloc(m->cap, sizeof(StrMapSlot));
    for (size_t i = 0; i < oldcap; i++) {
        StrMapSlot *s = &old[i];
        if (!s->key)
            continue;
        size_t idx = (size_t)s->hash & (m->cap - 1);
        while (m->slots[idx].key)
            idx = (idx + 1) & (m->cap - 1);
        m->slots[idx] = *s;
    }
    free(old);
}

void strmap_put(StrMap *m, const char *key, void *value)
{
    if ((m->count + 1) * 10 >= m->cap * 7)
        strmap_grow(m);
    uint64_t h = fnv1a(key);
    size_t idx = (size_t)h & (m->cap - 1);
    while (m->slots[idx].key) {
        if (strcmp(m->slots[idx].key, key) == 0) {
            m->slots[idx].value = value;
            return;
        }
        idx = (idx + 1) & (m->cap - 1);
    }
    m->slots[idx].hash = h;
    m->slots[idx].key = key;
    m->slots[idx].value = value;
    m->count++;
}

void *strmap_get(const StrMap *m, const char *key)
{
    if (!m)
        return NULL;
    uint64_t h = fnv1a(key);
    size_t idx = (size_t)h & (m->cap - 1);
    while (m->slots[idx].key) {
        if (strcmp(m->slots[idx].key, key) == 0)
            return m->slots[idx].value;
        idx = (idx + 1) & (m->cap - 1);
    }
    return NULL;
}

size_t strmap_count(const StrMap *m)
{
    return m ? m->count : 0;
}

/* ---- BitSet ---- */

BitSet *bitset_new(size_t nbits)
{
    BitSet *b = xmalloc(sizeof *b);
    b->nbits = nbits;
    b->words = xcalloc((nbits + 63) / 64, sizeof(uint64_t));
    return b;
}

void bitset_free(BitSet *b)
{
    if (!b)
        return;
    free(b->words);
    free(b);
}

void bitset_set(BitSet *b, size_t i)
{
    b->words[i / 64] |= 1ULL << (i % 64);
}

bool bitset_test(const BitSet *b, size_t i)
{
    if (i >= b->nbits)
        return false;
    return (b->words[i / 64] >> (i % 64)) & 1ULL;
}

void bitset_or(BitSet *dst, const BitSet *src)
{
    size_t n = (dst->nbits + 63) / 64;
    for (size_t i = 0; i < n; i++)
        dst->words[i] |= src->words[i];
}

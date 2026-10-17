/* util.h - piccoli servizi condivisi: allocazione, stringhe dinamiche,
 * mappa di stringhe, bitset. Nessuna dipendenza esterna. */
#ifndef GG_UTIL_H
#define GG_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* Buffer di stringa a crescita automatica. */
typedef struct {
    char *data;   /* sempre NUL-terminato */
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_free(StrBuf *sb);
void sb_clear(StrBuf *sb);
void sb_append(StrBuf *sb, const char *s);
void sb_appendn(StrBuf *sb, const char *s, size_t n);
void sb_appendf(StrBuf *sb, const char *fmt, ...);

/* Mappa hash aperta, chiavi stringa (prestito: il chiamante le mantiene vive),
 * valori void*. FNV-1a. */
typedef struct StrMap StrMap;

StrMap *strmap_new(void);
void strmap_free(StrMap *m);
void strmap_put(StrMap *m, const char *key, void *value);
void *strmap_get(const StrMap *m, const char *key);
size_t strmap_count(const StrMap *m);

/* Bitset semplice su indici. */
typedef struct {
    uint64_t *words;
    size_t nbits;
} BitSet;

BitSet *bitset_new(size_t nbits);
void bitset_free(BitSet *b);
void bitset_set(BitSet *b, size_t i);
bool bitset_test(const BitSet *b, size_t i);
void bitset_or(BitSet *dst, const BitSet *src);

#endif /* GG_UTIL_H */

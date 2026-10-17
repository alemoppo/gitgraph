/* gitdata.h - lettura in sola lettura della storia di un repository Git.
 *
 * Tutta l'acquisizione passa da processi `git` esterni (nessuna shell, nessuna
 * libgit2) e non esegue mai operazioni di scrittura sul repository.
 * Gli hash sono conservati completi come identificatori interni.
 */
#ifndef GG_GITDATA_H
#define GG_GITDATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *hash;      /* hash completo, es. 40 esadecimali */
    char **parents;  /* hash completi dei genitori, nell'ordine di git */
    size_t parent_count;
    char *author;    /* nome autore (UTF-8) */
    int64_t date;    /* data committer (secondi unix) */
    char *message;   /* messaggio completo, multi-riga, UTF-8 */
} GitCommit;

typedef enum {
    REF_LOCAL_BRANCH,
    REF_REMOTE_BRANCH,
    REF_TAG
} RefKind;

typedef struct {
    char *name;        /* nome breve: "main", "origin/main", "v1.0" */
    char *full_name;   /* es. "refs/heads/main" */
    char *target;      /* hash completo del commit puntato (tag dereferenziati) */
    RefKind kind;
    bool is_head;      /* HEAD puntata qui (branch corrente) */
} GitRef;

typedef struct {
    GitCommit *commits;   /* in ordine topologico, dal piu' recente */
    size_t commit_count;
    GitRef *refs;
    size_t ref_count;
    char *head_branch;    /* branch corrente, o NULL se HEAD scollegata */
    char *head_hash;      /* hash di HEAD (NULL in repo senza commit) */
    char *repo_path;      /* percorso usato per invocare git (assoluto) */
} GitData;

/* Carica commit e riferimenti del repository in `path` (NULL = directory
 * corrente). In caso di errore ritorna false e scrive in err (se non NULL)
 * un messaggio leggibile. Su successo va liberato con gitdata_free(). */
bool gitdata_load(GitData *out, const char *path, char *err, size_t err_size);
void gitdata_free(GitData *data);

/* Indice del commit con quell'hash, o SIZE_MAX se assente. */
size_t gitdata_find_commit(const GitData *d, const char *hash);

/* Helper di visualizzazione: copia i primi n caratteri dell'hash. */
void gitdata_short_hash(const char *hash, size_t n, char out[16]);

#endif /* GG_GITDATA_H */

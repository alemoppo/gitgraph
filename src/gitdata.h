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

#include "util.h"

typedef struct {
    char *hash;      /* hash completo, es. 40 esadecimali */
    char **parents;  /* hash completi dei genitori, nell'ordine di git */
    size_t *parent_idx; /* indici dei genitori in GitData.commits; risolti
                           una sola volta dopo il parsing (SIZE_MAX se il
                           genitore non compare nel log, es. clone shallow) */
    size_t parent_count;
    char *author;    /* nome autore (UTF-8) */
    int64_t date;    /* data committer (secondi unix) */
    char *message;   /* messaggio completo, multi-riga, UTF-8 */
} GitCommit;

/* Voce di PATH da considerare alla ricerca di git.exe: tolte virgolette e
 * spazi ai bordi (alcuni installer scrivono la voce quotata; le virgolette e
 * gli spazi estremi non sono parte del percorso), deve restare un percorso
 * assoluto: mai vuota, mai "." o "..", mai relativa, mai la directory
 * corrente. La funzione non costruisce nomi e non sposta puntatori (il
 * giudizio usa solo la copia locale dei bordi). Esposta perche' i test la
 * provano direttamente, senza dipendere dalla cache della ricerca (che vale
 * una sola volta per processo). */
bool git_path_entry_is_usable(const wchar_t *entry, size_t entry_len);

/* Da una voce del PATH (grezza: con virgolette e spazi ai bordi come la
 * scrive il PATH, con o senza barra finale) al nome del file da cercare
 * in quella directory. Ritorna true e scrive in out la voce ripulita piu'
 * "\\git.exe" (per "C:\Program Files\Git\cmd" esce
 * "C:\Program Files\Git\cmd\git.exe", per C:\Git\cmd\ esce
 * C:\Git\cmd\git.exe, senza doppie barre) solo se la voce e' utilizzabile e
 * il risultato entra per intero in out; false altrimenti, senza scrivere in
 * out se non entra. Esposta per i test. */
bool git_path_candidate(const wchar_t *entry, size_t entry_len,
                        wchar_t *out, size_t out_size);

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
    StrMap *by_hash;      /* hash -> indice commit, interno */
} GitData;

/* Carica commit e riferimenti del repository in `path` (NULL = directory
 * corrente). In caso di errore ritorna false e scrive in err (se non NULL)
 * un messaggio leggibile. Su successo va liberato con gitdata_free(). */
bool gitdata_load(GitData *out, const char *path, char *err, size_t err_size);
void gitdata_free(GitData *data);

/* Indice del commit con quell'hash, o SIZE_MAX se assente. O(1): usa la mappa
 * costruita al caricamento. */
size_t gitdata_find_commit(const GitData *d, const char *hash);

/* ---- ausiliari esposti per i test ---- */

/* Compone la riga di comando per un argv (terminato da NULL) con le regole di
 * quoting del runtime C di Windows. `out` riceve almeno NUL. */
void gitdata_build_cmdline(const char *const *argv, char *out, size_t out_size);

/* Esegue un programma catturando stdout e stderr in due buffer distinti, senza
 * shell. La lettura avviene con un thread per pipe: una pipe piena non puo'
 * bloccare il figlio (e quindi non puo' bloccare chi legge). `err` riceve un
 * messaggio quando la cattura non e' riuscita (processo non partito, thread di
 * lettura non creato, lettura incompleta); Ritorna false in quel caso, senza
 * lasciare processi sospesi. Un codice di uscita diverso da zero non e' un
 * errore: finisce in exit_code e il valore restituito resta true. */
bool gitdata_capture(const char *const *argv, StrBuf *out, StrBuf *errout,
                     int *exit_code, char *err, size_t err_size);

/* Aggancio per i test: se non NULL sostituisce la creazione del thread di
 * lettura delle pipe (l'argomento e' un puntatore opaco interno, il valore
 * restituito e' l'handle del thread). Ritornando NULL si simula il fallimento
 * di CreateThread e si verifica che il processo figlio venga terminato e
 * raccolto invece di restare bloccato. */
typedef void *(*GitdataReaderThreadHook)(void *reader);
void gitdata_set_reader_thread_hook(GitdataReaderThreadHook hook);

/* Helper di visualizzazione: copia i primi n caratteri dell'hash. */
void gitdata_short_hash(const char *hash, size_t n, char out[16]);

#endif /* GG_GITDATA_H */

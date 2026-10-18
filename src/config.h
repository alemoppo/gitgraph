/* config.h - configurazione del grafo (graph.cfg).
 *
 * Formato (facoltativo, letto da <radice-repository>/graph.cfg):
 *
 *     # commento su riga dedicata
 *     [long-lived]
 *     main
 *     release/\*
 *
 *     [parents]
 *     feature/x = main
 *
 * I commenti sono solo su righe dedicate: dentro una riga di dati '#' e ';'
 * fanno parte del valore (i nomi di riferimento Git possono contenerli),
 * quindi `main # nota` non vale come `main`: viene segnalato come nome non
 * valido, senza troncare il valore.
 *
 * In [long-lived] l'ordine conta: e' l'ordine di gerarchia usato dal modello
 * e dal layout. I pattern sono espansi sui branch esistenti al caricamento in
 * ordine naturale, e se un branch corrisponde a piu' righe vale la prima.
 * In [parents] ogni riga dichiara il padre di un branch; gli override hanno
 * precedenza sull'euristica e non sono un fatto ricavato da Git (stato
 * "configurato").
 *
 * Politica di validazione: un errore rifiuta il file per intero (niente
 * configurazione a meta'), gli avvisi lasciano applicare il file e vengono
 * elencati nel ConfigReport.
 */
#ifndef GG_CONFIG_H
#define GG_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *branch; /* nome del branch (nessuna wildcard) */
    char *parent; /* nome del branch padre dichiarato */
} ParentOverride;

typedef struct {
    /* [long-lived]: lista ordinata, con i pattern gia' espansi sui branch
     * esistenti; long_lived_explicit dice se la sezione era presente (in tal
     * caso non si applica il tronco implicito, anche se lista e' vuota). */
    char **long_lived;
    size_t long_lived_count;
    bool long_lived_explicit;

    /* [parents]: override manuali, con precedenza sull'euristica. */
    ParentOverride *overrides;
    size_t override_count;
} GraphConfig;

/* Rilascia il contenuto e azzera la struttura. */
void config_free(GraphConfig *cfg);

/* Livello di un messaggio diagnostico prodotto dal caricamento. */
typedef enum {
    CONFIG_DIAG_WARNING, /* il file resta valido */
    CONFIG_DIAG_ERROR    /* il file viene rifiutato */
} ConfigDiagLevel;

typedef struct {
    char *text;
    ConfigDiagLevel level;
} ConfigDiag;

/* Messaggi di caricamento, nell'ordine in cui sono stati prodotti. */
typedef struct {
    ConfigDiag *items;
    size_t count;
    bool has_errors;
} ConfigReport;

void config_report_free(ConfigReport *report);

/* Numero di messaggi di livello CONFIG_DIAG_ERROR (0 se assenti). */
size_t config_report_error_count(const ConfigReport *report);

void config_report_add(ConfigReport *report, ConfigDiagLevel level,
                       const char *fmt, ...);

/* Un riferimento noto al caricamento della configurazione. Per i branch
 * remoti il nome canonico e' quello senza il prefisso del remote: e' il nome
 * con cui l'utente scrive le regole ("origin/release/2.0" corrisponde a
 * "release/2.0"). */
typedef struct {
    const char *name;
    const char *canonical;
} ConfigBranch;

/* Carica graph.cfg.
 *
 *   path            percorso del file; NULL o inesistente = nessun errore,
 *                   cfg resta con il solo tronco implicito
 *   branches        riferimenti esistenti, necessari per espandere i pattern
 *                   di [long-lived] e per segnalare nomi che non corrispondono
 *                   a nulla
 *   report          diagnostica (facoltativo): viene azzerata e riempita
 *   err             messaggio sintetico in caso di errore (facoltativo)
 *
 * Ritorna false e rifiuta il file per intero nei tre casi di rifiuto:
 *   - contiene errori di sintassi o di validazione;
 *   - esiste ma non e' leggibile (permessi, directory al suo posto, errore di
 *     lettura): viene emesso un messaggio di errore nella diagnostica;
 *   - la lettura si interrompe con un errore.
 * Un file assente non e' un errore (la configurazione e' facoltativa).
 * In ogni caso di rifiuto cfg contiene solo il tronco implicito (niente
 * long-lived ne' override parziali), cosi' lo strumento resta comunque
 * utilizzabile. */
bool config_load(GraphConfig *cfg, const char *path,
                 const ConfigBranch *branches, size_t branch_count,
                 ConfigReport *report, char *err, size_t err_size);

/* Verifica se `name` e' sintatticamente un nome di riferimento Git valido
 * (sottinsieme delle regole che contano per graph.cfg: niente spazi ne'
 * caratteri di controllo, niente "~^:?*[\\", niente "//", niente "..",
 * niente "@{", niente "/" o "." ai bordi, niente suffisso ".lock", "@"
 * vietato). Esposta per i test. */
bool config_refname_syntax_ok(const char *name);

/* Costruisce il percorso <repo_path>/graph.cfg (il chiamante lo libera).
 * Ritorna NULL se repo_path e' NULL. */
char *config_path_in_repo(const char *repo_path);

/* Default impliciti: se la lista [long-lived] manca, il tronco e' `main`
 * oppure `master` (in quest'ordine), se esiste tra `branch_names`. */
void config_set_implicit_trunk(GraphConfig *cfg, char *const *branch_names,
                               size_t branch_count);

/* Posizione del branch nella lista [long-lived], -1 se non e' long-lived. */
int config_long_lived_rank(const GraphConfig *cfg, const char *branch_name);

#endif /* GG_CONFIG_H */

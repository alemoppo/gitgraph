/* render.h - geometria della scena da disegnare (nessuna dipendenza da SDL).
 *
 * Fase 5: dal layout (corsie e righe) si producono le primitive che servono al
 * disegno: i punti dei commit, le polilinee degli archi e le ancore delle
 * etichette dei riferimenti. Il modulo e' puro: nessuna finestra, nessun pixel,
 * solo numeri in UNITA' DI SCENA. Cosi' il disegno SDL resta un passaggio
 * separato (src/gui_main.c) e la geometria si prova senza aprire nulla.
 *
 * UNITA' DI SCENA. x e' la corsia (0 = la piu' a sinistra), y e' la riga
 * topologica (0 = il commit piu' recente, in alto; y cresce verso il passato,
 * cioe' verso il basso come nel dump). Il passaggio a pixel (margini, passo di
 * corsia e di riga, zoom, scorrimento) e' del disegnatore.
 *
 * ARCHI. Ogni arco e' una polilinea di al piu' RENDER_MAX_POINTS punti. Poiche'
 * ogni riga ha un solo commit, un tratto orizzontale all'altezza di una riga
 * non attraversa mai un nodo estraneo: conviene quindi appoggiare il gomito a
 * una riga, non a una mezzeria.
 *  - primo genitore (stessa corsia): segmento verticale di 2 punti;
 *  - diramazione (primo genitore in un'altra corsia): verticale nella corsia
 *    del figlio fino alla riga del genitore, poi orizzontale a quella riga fino
 *    alla corsia del genitore (3 punti);
 *  - genitore di merge: orizzontale alla riga del commit di merge fino alla
 *    corsia del genitore, poi verticale in quella corsia fino al genitore
 *    (3 punti);
 *  - gli archi di merge si disegnano piu' sottili e piu' trasparenti delle
 *    catene e il commit di destinazione porta un piccolo anello: cosi' un
 *    branch che continua dopo il merge non sembra puntare alla punta;
 *  - tratteggio: non si disegnano archi a livello di branch. Quando il padre di
 *    un ramo e' ambiguo, il suo arco di diramazione (dal commit proprietario
 *    piu' vecchio al primo genitore) si disegna tratteggiato, come ipotesi da
 *    confermare con [parents]; la paternita' coincide con la catena dei commit
 *    e non si duplica.
 *
 * ETICHETTE. Le etichette sono raggruppate per commit: ogni gruppo ha come
 * ancora la corsia e la riga del commit puntato (LayoutCommit.lane), non la
 * corsia del branch, cosi' un branch riallineato (fast-forward) e i suoi alias
 * compaiono sul commit di main dove sono davvero. I riferimenti dello stesso
 * tipo sullo stesso commit (per esempio un locale e il suo remoto con la stessa
 * punta) si uniscono in una sola etichetta con i nomi separati da ", ". I
 * gruppi dello stesso commit si impilano con uno slot crescente (0 = il piu' in
 * alto), cosi' non si sovrappongono: i branch prima dei tag. I tag sono un
 * gruppo a parte, per colore. `text` e' di proprieta' della scena e si rilascia
 * con render_free().
 */
#ifndef GG_RENDER_H
#define GG_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gitdata.h"
#include "history.h"
#include "layout.h"

/* Punto in unita' di scena. */
typedef struct {
    double x; /* corsia */
    double y; /* riga */
} RenderPt;

typedef enum {
    RENDER_LINE_FIRST_PARENT, /* catena: primo genitore nella stessa corsia */
    RENDER_LINE_MERGE_PARENT, /* secondo genitore (o successivo) di un merge */
    RENDER_LINE_BRANCH        /* diramazione: primo genitore in altra corsia */
} RenderLineKind;

#define RENDER_MAX_POINTS 3

typedef struct {
    RenderPt pts[RENDER_MAX_POINTS];
    size_t n;      /* punti usati (2 o 3) */
    size_t lane;   /* corsia del figlio: decide il colore */
    size_t from;   /* commit di partenza (il figlio) */
    size_t to;     /* commit di arrivo (il genitore) */
    bool dashed;   /* true per la diramazione di un padre ambiguo */
    RenderLineKind kind;
} RenderLine;

typedef struct {
    RenderPt p;
    size_t lane;
    size_t row;
    size_t commit; /* indice del commit (== row senza filtro) */
    bool head;     /* commit puntato da HEAD */
    bool anon;     /* nessun branch lo reclama */
    bool merge_target; /* destinazione di un arco di merge: anello */
} RenderDot;

typedef enum {
    RENDER_LABEL_BRANCH, /* riferimenti locali e remoti del commit */
    RENDER_LABEL_TAG     /* tag del commit */
} RenderLabelKind;

typedef struct {
    RenderPt p;     /* corsia e riga del commit puntato */
    size_t row;     /* riga del commit puntato, SIZE_MAX se fuori dal log */
    size_t commit;  /* indice del commit puntato, SIZE_MAX se fuori dal log */
    size_t slot;    /* posizione nella pila dello stesso commit (0 = in alto) */
    RenderLabelKind kind;
    char *text;       /* nomi uniti da ", " (es. "branch1, origin/branch1") */
    bool is_head;     /* un riferimento del gruppo e' il branch corrente */
    bool has_remote;  /* almeno un riferimento del gruppo e' remoto */
} RenderAnchor;

typedef struct {
    RenderDot *dots;
    size_t dot_count;
    RenderLine *lines;
    size_t line_count;
    RenderAnchor *anchors;
    size_t anchor_count;
    size_t lane_count;
    size_t rows;
} Render;

#define RENDER_PALETTE_SIZE 16

/* Colore della corsia `lane`: sempre lo stesso per lo stesso indice (palette
 * ciclica). Deterministico, quindi verificabile. */
void render_lane_color(size_t lane, uint8_t out[3]);

/* Costruisce la scena. `data` puo' essere NULL (nessun commit di HEAD noto),
 * `hist` puo' essere NULL (nessuna etichetta); `lay` non NULL. Azzera `out`,
 * che il chiamante rilascia con render_free(). */
void render_build(Render *out, const GitData *data, const History *hist,
                  const Layout *lay);

void render_free(Render *r);

#endif /* GG_RENDER_H */

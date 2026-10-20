/* gui_main.c - finestra SDL3/SDL3_ttf sulla scena (fase 5/6).
 *
 * Uso: gitgraph [opzioni] [percorso-repo]
 *   --font PERCORSO   font TrueType (default: un font di sistema)
 *   --width N         larghezza della finestra (default 1100)
 *   --height N        altezza della finestra (default 760)
 *   --headless        nessuna finestra visibile (driver video "dummy")
 *   --screenshot P    disegna un fotogramma, salva un BMP in P ed esce
 *   --frames N        disegna N fotogrammi ed esce
 *   --help            questo aiuto
 *
 * Il disegno vero e proprio e' qui; la geometria della scena (corsie, righe,
 * archi, ancore delle etichette) e la ricerca sotto il puntatore sono in
 * render.c, senza SDL. Questa fase non scrive sul repository: legge e disegna.
 *
 * Comandi: click sinistro su un commit o su un'etichetta per selezionare,
 * click nel vuoto per deselezionare, C per copiare l'hash completo, Tab per
 * il pannello dei dettagli, Esc per deselezionare (poi uscire); frecce per
 * scorrere, PagSu/PagGiu di una schermata, Home/Fine, "+"/"-" per lo zoom, "0"
 * per tornare all'inizio; rotella del mouse per scorrere, Ctrl+rotella per lo
 * zoom, trascinamento col tasto sinistro per spostare la vista.
 */
#include "config.h"
#include "gitdata.h"
#include "history.h"
#include "layout.h"
#include "render.h"
#include "util.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Passo base in pixel di una corsia e di una riga, a zoom 1. */
#define LANE_W 22.0f
#define ROW_H 24.0f
#define MARGIN_X 28.0f
#define MARGIN_Y 28.0f
#define DOT_R 5.0f
#define LABEL_PT 13.0f
#define LABEL_PAD 3.0f
#define PANEL_PT 12.0f
#define STATUS_PT 12.0f
#define PANEL_W 380
#define STATUS_H 24
#define ZOOM_MIN 0.35
#define ZOOM_MAX 4.0

#define NO_IDX ((size_t)-1)

typedef struct {
    const char *repo;
    const char *font;
    const char *screenshot;
    const char *select; /* --select HASH|BRANCH */
    const char *filter; /* --filter local|ancestors:BRANCH */
    int width;
    int height;
    int frames;
    bool headless;
} Options;

typedef struct {
    double off_x; /* prima corsia visibile (in unita' di scena) */
    double off_y; /* prima riga visibile */
    double zoom;
    int width;  /* area del grafo (senza il pannello) */
    int height;
} View;

typedef struct {
    GitData data;
    GraphConfig cfg;
    History hist;
    Layout lay;
    Render sc;
} Model;

typedef enum {
    FILTER_NONE,
    FILTER_LOCAL,    /* nasconde i riferimenti remoti */
    FILTER_ANCESTORS /* solo la storia ancestrale di un branch */
} FilterKind;

typedef struct {
    View view;
    int win_w; /* dimensioni in pixel della finestra */
    int win_h;
    bool panel_on;
    bool sel_active;
    bool sel_is_branch; /* true: la selezione e' un branch, non un commit */
    size_t sel_commit; /* commit selezionato (per un branch: la punta) */
    size_t sel_branch; /* indice del branch selezionato */
    FilterKind filter;
    size_t filter_branch; /* branch del filtro ancestrale */
    const char *repo;     /* per F5: ricaricare lo stesso repository */
    TTF_Font *font;       /* per misurare le etichette dopo un rebuild */
} App;

static const char *const g_font_candidates[] = {
    "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/tahoma.ttf",  "C:/Windows/Fonts/consola.ttf",
    NULL,
};

static void print_help(void)
{
    printf("uso: gitgraph [--font PERCORSO] [--width N] [--height N]\n"
           "              [--headless] [--screenshot P] [--frames N]\n"
           "              [--select HASH|BRANCH] [--filter local|ancestors:BRANCH]\n"
           "              [--help] [percorso-repo]\n"
           "  percorso-repo  default: directory corrente\n"
           "  --select       seleziona un commit (hash o prefisso) o un branch;\n"
           "                 col filtro deve restare visibile, altrimenti errore\n"
           "  --filter       vista filtrata: local (senza remoti) oppure\n"
           "                 ancestors:BRANCH (storia ancestrale del branch)\n"
           "comandi: click sinistro = seleziona, C = copia l'hash, Tab =\n"
           "         pannello, Esc = deseleziona (poi esce), R = senza remoti,\n"
           "         B = antenati del branch selezionato (di nuovo: tutti),\n"
           "         A = azzera selezione e filtri, F5 = ricarica il repo,\n"
           "         frecce/rotella = scorrimento, Ctrl+rotella = zoom\n");
}

static float px(const View *v, double x)
{
    return (float)((x - v->off_x) * LANE_W * v->zoom + MARGIN_X);
}

static float py(const View *v, double y)
{
    return (float)((y - v->off_y) * ROW_H * v->zoom + MARGIN_Y);
}

static void view_clamp(View *v, size_t lanes, size_t rows)
{
    if (v->zoom < ZOOM_MIN)
        v->zoom = ZOOM_MIN;
    if (v->zoom > ZOOM_MAX)
        v->zoom = ZOOM_MAX;
    double vis_x = (v->width + 2 * MARGIN_X) / (LANE_W * v->zoom);
    double vis_y = (v->height + 2 * MARGIN_Y) / (ROW_H * v->zoom);
    double max_x = (double)lanes + 1 - vis_x;
    double max_y = (double)rows + 1 - vis_y;
    if (max_x < 0)
        max_x = 0;
    if (max_y < 0)
        max_y = 0;
    if (v->off_x < -1)
        v->off_x = -1;
    if (v->off_x > max_x)
        v->off_x = max_x;
    if (v->off_y < -1)
        v->off_y = -1;
    if (v->off_y > max_y)
        v->off_y = max_y;
}

static void fill_circle(SDL_Renderer *ren, float cx, float cy, float r)
{
    int ri = (int)(r + 0.5f);
    for (int dy = -ri; dy <= ri; dy++) {
        float d2 = r * r - (float)(dy * dy);
        if (d2 < 0)
            continue;
        float dx = sqrtf(d2);
        SDL_FRect span = { cx - dx, cy + (float)dy, 2 * dx, 1.0f };
        SDL_RenderFillRect(ren, &span);
    }
}

/* Segmento con spessore: qualche linea parallela (il renderer software non ha
 * uno spessore di linea). */
static void thick_segment(SDL_Renderer *ren, float x0, float y0, float x1,
                          float y1, float w)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f)
        return;
    float nx = -dy / len, ny = dx / len;
    int half = (int)(w / 2.0f);
    for (int k = -half; k <= half; k++)
        SDL_RenderLine(ren, x0 + nx * k, y0 + ny * k, x1 + nx * k, y1 + ny * k);
}

static void dashed_segment(SDL_Renderer *ren, float x0, float y0, float x1,
                           float y1, float dash, float gap, float w)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f)
        return;
    float ux = dx / len, uy = dy / len;
    float t = 0;
    while (t < len) {
        float e = t + dash;
        if (e > len)
            e = len;
        thick_segment(ren, x0 + ux * t, y0 + uy * t, x0 + ux * e, y0 + uy * e, w);
        t = e + gap;
    }
}

static void stroke_path(SDL_Renderer *ren, const View *v, const RenderLine *ln,
                        float w)
{
    for (size_t i = 1; i < ln->n; i++) {
        float x0 = px(v, ln->pts[i - 1].x), y0 = py(v, ln->pts[i - 1].y);
        float x1 = px(v, ln->pts[i].x), y1 = py(v, ln->pts[i].y);
        if (ln->dashed)
            dashed_segment(ren, x0, y0, x1, y1, 7 * (float)v->zoom,
                           5 * (float)v->zoom, w);
        else
            thick_segment(ren, x0, y0, x1, y1, w);
    }
}

static void draw_text(SDL_Renderer *ren, TTF_Font *font, float x, float y,
                      const char *text, size_t len, SDL_Color fg)
{
    if (!font || len == 0)
        return;
    SDL_Surface *s = TTF_RenderText_Blended(font, text, len, fg);
    if (!s)
        return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    if (t) {
        SDL_FRect dst = { x, y, (float)s->w, (float)s->h };
        SDL_RenderTexture(ren, t, NULL, &dst);
        SDL_DestroyTexture(t);
    }
    SDL_DestroySurface(s);
}

static void draw_label(SDL_Renderer *ren, TTF_Font *font, float x, float y,
                       float pad, SDL_Color fg, Uint8 bg_r, Uint8 bg_g,
                       Uint8 bg_b, Uint8 alpha, const char *text)
{
    if (!font)
        return;
    fg.a = alpha;
    SDL_Surface *s = TTF_RenderText_Blended(font, text, 0, fg);
    if (!s)
        return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    if (t) {
        SDL_FRect bg = { x, y, (float)s->w + 2 * pad, (float)s->h + 2 * pad };
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, bg_r, bg_g, bg_b,
                               (Uint8)((unsigned)alpha * 215 / 255));
        SDL_RenderFillRect(ren, &bg);
        SDL_FRect dst = { x + pad, y + pad, (float)s->w, (float)s->h };
        SDL_RenderTexture(ren, t, NULL, &dst);
        SDL_DestroyTexture(t);
    }
    SDL_DestroySurface(s);
}

/* Sostituisce la larghezza nominale delle etichette con quella misurata dal
 * font. La misura e' a zoom 1 e in unita' di scena non dipende dallo zoom; il
 * padding viene anch'esso scalato dal disegnatore, quindi il rettangolo del
 * hit-test resta coerente. Il hit-test stesso resta in render.c, senza SDL. */
static void update_label_widths(Render *sc, TTF_Font *font)
{
    if (!font)
        return;
    TTF_SetFontSize(font, LABEL_PT);
    for (size_t i = 0; i < sc->anchor_count; i++) {
        RenderAnchor *a = &sc->anchors[i];
        int tw = 0, th = 0;
        if (TTF_GetStringSize(font, a->text, strlen(a->text), &tw, &th) &&
            tw > 0) {
            a->w = ((double)tw + 2.0 * LABEL_PAD) / (double)LANE_W;
            a->h = ((double)th + 2.0 * LABEL_PAD) / (double)ROW_H;
        }
    }
}

static void load_config(const GitData *data, GraphConfig *cfg)
{
    size_t n = 0;
    ConfigBranch *branches =
        xmalloc((data->ref_count ? data->ref_count : 1) * sizeof(ConfigBranch));
    for (size_t i = 0; i < data->ref_count; i++) {
        if (data->refs[i].kind == REF_TAG)
            continue;
        const char *name = data->refs[i].name;
        const char *canonical = name;
        if (data->refs[i].kind == REF_REMOTE_BRANCH) {
            const char *slash = strchr(name, '/');
            if (slash && slash[1])
                canonical = slash + 1;
        }
        branches[n].name = name;
        branches[n].canonical = canonical;
        n++;
    }
    char *cfg_path = config_path_in_repo(data->repo_path);
    ConfigReport report;
    char err[1024];
    bool ok =
        config_load(cfg, cfg_path, branches, n, &report, err, sizeof err);
    for (size_t i = 0; i < report.count; i++)
        fprintf(stderr, "%s: %s\n",
                report.items[i].level == CONFIG_DIAG_ERROR ? "errore" : "avviso",
                report.items[i].text);
    if (!ok)
        fprintf(stderr, "%s: configurazione ignorata\n", err);
    free(cfg_path);
    config_report_free(&report);
    free(branches);
    /* un file rifiutato non e' un errore: resta il solo tronco implicito */
}

static bool model_load(Model *m, const char *repo, char *err, size_t err_size)
{
    memset(m, 0, sizeof *m);
    if (!gitdata_load(&m->data, repo, err, err_size))
        return false;
    load_config(&m->data, &m->cfg);
    if (!history_build(&m->hist, &m->data, &m->cfg, err, err_size)) {
        config_free(&m->cfg);
        gitdata_free(&m->data);
        memset(m, 0, sizeof *m);
        return false;
    }
    layout_build(&m->lay, &m->hist);
    render_build(&m->sc, &m->data, &m->hist, &m->lay, NULL);
    return true;
}

static void model_free(Model *m)
{
    render_free(&m->sc);
    layout_free(&m->lay);
    history_free(&m->hist);
    config_free(&m->cfg);
    gitdata_free(&m->data);
    memset(m, 0, sizeof *m);
}

static void app_clear_selection(App *app)
{
    app->sel_active = false;
    app->sel_is_branch = false;
    app->sel_commit = NO_IDX;
    app->sel_branch = NO_IDX;
}

/* Riporta l'area del grafo alla larghezza della finestra (il pannello, se
 * acceso, toglie PANEL_W) e corregge lo scorrimento. */
static void app_resize(Model *m, App *app)
{
    app->view.width = app->win_w - (app->panel_on ? PANEL_W : 0);
    if (app->view.width < 100)
        app->view.width = 100;
    app->view.height = app->win_h;
    view_clamp(&app->view, m->sc.lane_count, m->sc.rows);
}

/* Ricostruisce la scena col filtro corrente. I filtri sono maschere di commit
 * visibili: Layout e History restano intatti. */
static void scene_rebuild(Model *m, App *app)
{
    render_free(&m->sc);
    bool *mask = NULL;
    RenderFilter f;
    memset(&f, 0, sizeof f);
    if (app->filter == FILTER_LOCAL) {
        mask = xmalloc((m->lay.commit_count ? m->lay.commit_count : 1) *
                       sizeof(bool));
        render_filter_local(&m->data, &m->hist, &m->lay, mask);
        f.visible = mask;
        f.flags = RENDER_F_HIDE_REMOTES;
    } else if (app->filter == FILTER_ANCESTORS &&
               app->filter_branch < m->hist.branch_count) {
        mask = xmalloc((m->lay.commit_count ? m->lay.commit_count : 1) *
                       sizeof(bool));
        render_filter_ancestors(&m->lay,
                                m->hist.branches[app->filter_branch].tip_pos,
                                mask);
        f.visible = mask;
    }
    if (mask) {
        render_build(&m->sc, &m->data, &m->hist, &m->lay, &f);
        free(mask);
    } else {
        render_build(&m->sc, &m->data, &m->hist, &m->lay, NULL);
    }
    update_label_widths(&m->sc, app->font);
    app_resize(m, app);
}

static void app_apply_filter(Model *m, App *app, FilterKind kind, size_t branch)
{
    app->filter = kind;
    app->filter_branch = (kind == FILTER_ANCESTORS) ? branch : NO_IDX;
    scene_rebuild(m, app);
    if (app->sel_active && !render_has_commit(&m->sc, app->sel_commit))
        app_clear_selection(app);
}

/* Tasto A: via selezione e filtri, vista intera. */
static void app_reset(Model *m, App *app)
{
    app_clear_selection(app);
    app->filter = FILTER_NONE;
    app->filter_branch = NO_IDX;
    scene_rebuild(m, app);
}

/* F5: rilegge il repository e ricostruisce modello, layout e scena. Il filtro
 * per nome sopravvive se il branch esiste ancora; la selezione no (gli indici
 * dei commit cambiano). */
static bool app_reload(Model *m, App *app)
{
    char keep[256] = "";
    FilterKind kind = app->filter;
    if (kind == FILTER_ANCESTORS && app->filter_branch < m->hist.branch_count)
        snprintf(keep, sizeof keep, "%s",
                 m->hist.branches[app->filter_branch].name);
    app_clear_selection(app);
    model_free(m);
    char err[1024];
    if (!model_load(m, app->repo, err, sizeof err)) {
        fprintf(stderr, "errore: %s\n", err);
        app->filter = FILTER_NONE;
        app->filter_branch = NO_IDX;
        return false;
    }
    app->filter = FILTER_NONE;
    app->filter_branch = NO_IDX;
    if (kind == FILTER_LOCAL) {
        app->filter = FILTER_LOCAL;
    } else if (kind == FILTER_ANCESTORS) {
        size_t bi = history_find_branch(&m->hist, keep);
        if (bi != NO_IDX && m->hist.branches[bi].tip_pos != NO_IDX) {
            app->filter = FILTER_ANCESTORS;
            app->filter_branch = bi;
        }
    }
    scene_rebuild(m, app);
    return true;
}

static void app_select_commit(Model *m, App *app, size_t commit)
{
    if (commit >= m->data.commit_count)
        return;
    app->sel_active = true;
    app->sel_is_branch = false;
    app->sel_commit = commit;
    app->sel_branch = NO_IDX;
    app->panel_on = true;
    app_resize(m, app);
}

static void app_select_branch(Model *m, App *app, size_t branch)
{
    if (branch >= m->hist.branch_count)
        return;
    size_t tip = m->hist.branches[branch].tip_pos;
    if (tip == NO_IDX || tip >= m->data.commit_count)
        return;
    app->sel_active = true;
    app->sel_is_branch = true;
    app->sel_commit = tip;
    app->sel_branch = branch;
    app->panel_on = true;
    app_resize(m, app);
}

/* Applica --filter e --select prima di aprire la finestra (usato anche dagli
 * screenshot headless). La selezione e' un nome di branch, oppure un hash
 * completo o un suo prefisso non ambiguo. */
static bool app_prepare(Model *m, App *app, const Options *opt)
{
    if (opt->filter) {
        if (strcmp(opt->filter, "local") == 0) {
            app_apply_filter(m, app, FILTER_LOCAL, NO_IDX);
        } else if (strncmp(opt->filter, "ancestors:", 10) == 0) {
            const char *name = opt->filter + 10;
            size_t bi = history_find_branch(&m->hist, name);
            if (bi == NO_IDX || m->hist.branches[bi].tip_pos == NO_IDX) {
                fprintf(stderr, "filtro: branch '%s' non trovato\n", name);
                return false;
            }
            app_apply_filter(m, app, FILTER_ANCESTORS, bi);
        } else {
            fprintf(stderr, "filtro non valido: %s (local|ancestors:BRANCH)\n",
                    opt->filter);
            return false;
        }
    }
    if (opt->select) {
        size_t bi = history_find_branch(&m->hist, opt->select);
        if (bi != NO_IDX && m->hist.branches[bi].tip_pos != NO_IDX) {
            app_select_branch(m, app, bi);
        } else {
            size_t len = strlen(opt->select);
            size_t found = 0, ci = NO_IDX;
            if (len >= 4) {
                for (size_t i = 0; i < m->data.commit_count; i++)
                    if (strncmp(m->data.commits[i].hash, opt->select, len) ==
                        0) {
                        if (ci == NO_IDX)
                            ci = i;
                        found++;
                    }
            }
            if (found != 1) {
                fprintf(stderr,
                        "selezione: '%s' non trovato o ambiguo (%zu)\n",
                        opt->select, found);
                return false;
            }
            app_select_commit(m, app, ci);
        }
        if (app->sel_active && !render_has_commit(&m->sc, app->sel_commit)) {
            fprintf(stderr,
                    "selezione: '%s' non visibile con il filtro richiesto\n",
                    opt->select);
            return false;
        }
    }
    return true;
}

static void draw_panel(SDL_Renderer *ren, TTF_Font *font, Model *m, App *app)
{
    float x = (float)(app->win_w - PANEL_W);
    float h = (float)(app->win_h - STATUS_H);
    SDL_SetRenderDrawColor(ren, 24, 26, 34, 255);
    SDL_FRect bg = { x, 0, (float)PANEL_W, h };
    SDL_RenderFillRect(ren, &bg);
    SDL_SetRenderDrawColor(ren, 70, 76, 92, 255);
    SDL_RenderLine(ren, x, 0, x, h);
    if (!font)
        return;

    char *text = NULL;
    if (app->sel_active && app->sel_commit < m->data.commit_count) {
        text = render_details_text(&m->data, &m->hist, &m->lay, app->sel_commit,
                                   app->sel_is_branch ? app->sel_branch : NO_IDX,
                                   44);
    }
    if (!text)
        text = xstrdup("nessuna selezione\n\n"
                       "click su un commit o su\n"
                       "un'etichetta: dettagli e\n"
                       "selezione\n\n"
                       "C: copia l'hash completo\n"
                       "Tab: mostra/nascondi il pannello\n"
                       "Esc: deseleziona, poi esce\n");

    TTF_SetFontSize(font, PANEL_PT);
    float lh = (float)TTF_GetFontHeight(font) + 2.0f;
    float tx = x + 14.0f;
    float ty = 12.0f;
    SDL_Color title = { 235, 238, 246, 255 };
    SDL_Color body = { 205, 212, 224, 255 };
    draw_text(ren, font, tx, ty, "DETTAGLI", 8, title);
    ty += lh * 1.7f;
    const char *p = text;
    while (*p && ty + lh < h) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        draw_text(ren, font, tx, ty, p, len, body);
        ty += lh;
        if (!nl)
            break;
        p = nl + 1;
    }
    free(text);
}

static void draw_status(SDL_Renderer *ren, TTF_Font *font, Model *m, App *app)
{
    SDL_SetRenderDrawColor(ren, 28, 30, 38, 255);
    SDL_FRect bar = { 0, (float)(app->win_h - STATUS_H), (float)app->win_w,
                      (float)STATUS_H };
    SDL_RenderFillRect(ren, &bar);
    char sel[200] = "nessuna";
    if (app->sel_active && app->sel_is_branch &&
        app->sel_branch < m->hist.branch_count) {
        snprintf(sel, sizeof sel, "branch %s",
                 m->hist.branches[app->sel_branch].name);
    } else if (app->sel_active && app->sel_commit < m->data.commit_count) {
        char sh[16];
        gitdata_short_hash(m->data.commits[app->sel_commit].hash, 8, sh);
        snprintf(sel, sizeof sel, "commit %s", sh);
    }
    char filt[220] = "nessuno";
    if (app->filter == FILTER_LOCAL)
        snprintf(filt, sizeof filt, "local (senza remoti)");
    else if (app->filter == FILTER_ANCESTORS &&
             app->filter_branch < m->hist.branch_count)
        snprintf(filt, sizeof filt, "antenati: %s",
                 m->hist.branches[app->filter_branch].name);
    char line[512];
    snprintf(line, sizeof line,
             "commit visibili: %zu   corsie: %zu   filtro: %s   "
             "selezione: %s",
             m->sc.dot_count, m->sc.lane_count, filt, sel);
    if (!font)
        return;
    TTF_SetFontSize(font, STATUS_PT);
    SDL_Color fg = { 190, 197, 210, 255 };
    draw_text(ren, font, 10.0f, (float)(app->win_h - STATUS_H) + 4.0f, line,
              strlen(line), fg);
}

static void draw_frame(SDL_Renderer *ren, TTF_Font *font, Model *m, App *app)
{
    const View *v = &app->view;
    SDL_SetRenderDrawColor(ren, 18, 20, 26, 255);
    SDL_RenderClear(ren);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    double vis_rows = (v->height + 2 * MARGIN_Y) / (ROW_H * v->zoom);
    double vis_cols = (v->width + 2 * MARGIN_X) / (LANE_W * v->zoom);
    double r0 = v->off_y - 1, r1 = v->off_y + vis_rows + 1;
    double c0 = v->off_x - 1, c1 = v->off_x + vis_cols + 1;

    /* archi: sotto i punti, cosi' i nodi restano leggibili */
    for (size_t i = 0; i < m->sc.line_count; i++) {
        const RenderLine *ln = &m->sc.lines[i];
        double lo = ln->pts[0].y, hi = ln->pts[0].y, lox = ln->pts[0].x,
               hix = ln->pts[0].x;
        for (size_t k = 1; k < ln->n; k++) {
            if (ln->pts[k].y < lo)
                lo = ln->pts[k].y;
            if (ln->pts[k].y > hi)
                hi = ln->pts[k].y;
            if (ln->pts[k].x < lox)
                lox = ln->pts[k].x;
            if (ln->pts[k].x > hix)
                hix = ln->pts[k].x;
        }
        if (hi < r0 || lo > r1 || hix < c0 || lox > c1)
            continue;
        bool hot = false, dim = false;
        if (app->sel_active) {
            if (app->sel_is_branch) {
                hot = ln->kind == RENDER_LINE_BRANCH &&
                      ln->from < m->lay.commit_count &&
                      m->lay.commits[ln->from].owner == app->sel_branch;
                dim = !hot;
            } else {
                hot = ln->from == app->sel_commit || ln->to == app->sel_commit;
            }
        }
        uint8_t rgb[3];
        render_lane_color(ln->lane, rgb);
        Uint8 alpha = 205;
        float w = 3.0f * (float)v->zoom;
        if (ln->kind == RENDER_LINE_MERGE_PARENT) {
            /* il merge non e' una catena: piu' sottile e piu' trasparente,
             * cosi' non sembra puntare alla punta del branch */
            alpha = 140;
            w = 1.6f * (float)v->zoom;
        } else if (ln->dashed) {
            w = 2.0f;
        }
        if (hot) {
            alpha = 255;
            w = 4.0f * (float)v->zoom;
        } else if (dim) {
            alpha = 55;
        }
        SDL_SetRenderDrawColor(ren, rgb[0], rgb[1], rgb[2], alpha);
        stroke_path(ren, v, ln, w);
    }

    /* punti dei commit (in ordine di riga: si puo' interrompere presto) */
    float r = DOT_R * (float)v->zoom;
    for (size_t i = 0; i < m->sc.dot_count; i++) {
        const RenderDot *d = &m->sc.dots[i];
        if ((double)d->row < r0)
            continue;
        if ((double)d->row > r1)
            break;
        bool hot = false;
        Uint8 alpha = 255;
        if (app->sel_active && app->sel_is_branch) {
            hot = d->commit < m->lay.commit_count &&
                  m->lay.commits[d->commit].owner == app->sel_branch;
            alpha = hot ? 255 : 55;
        } else if (app->sel_active) {
            hot = d->commit == app->sel_commit;
        }
        uint8_t rgb[3];
        render_lane_color(d->lane, rgb);
        float cx = px(v, d->p.x), cy = py(v, d->p.y);
        if (hot) {
            SDL_SetRenderDrawColor(ren, 120, 220, 255, 255);
            fill_circle(ren, cx, cy, r + 4.0f * (float)v->zoom);
        }
        if (d->head) {
            SDL_SetRenderDrawColor(ren, 245, 246, 250, alpha);
            fill_circle(ren, cx, cy, r + 3.0f * (float)v->zoom);
        }
        if (d->merge_target) {
            /* anello della destinazione di un merge: dice dove arriva davvero
             * l'arco anche se il branch prosegue oltre */
            SDL_SetRenderDrawColor(ren, 188, 196, 214, alpha);
            fill_circle(ren, cx, cy, r + 2.2f * (float)v->zoom);
        }
        if (d->anon) {
            SDL_SetRenderDrawColor(ren, (Uint8)(rgb[0] / 2 + 60),
                                   (Uint8)(rgb[1] / 2 + 60),
                                   (Uint8)(rgb[2] / 2 + 60), alpha);
        } else {
            SDL_SetRenderDrawColor(ren, rgb[0], rgb[1], rgb[2], alpha);
        }
        fill_circle(ren, cx, cy, r);
    }

    /* etichette dei riferimenti: gruppi uniti, impilati con lo slot */
    if (font && v->zoom >= 0.5) {
        TTF_SetFontSize(font, LABEL_PT * (float)v->zoom);
        int fh = TTF_GetFontHeight(font);
        /* lo slot vale una riga di scena, come nel hit-test; anche il padding
         * scala con lo zoom, cosi' il rettangolo disegnato resta coerente */
        float slot_h = ROW_H * (float)v->zoom;
        float pad = LABEL_PAD * (float)v->zoom;
        float label_h = (float)fh + 2.0f * pad;
        for (size_t i = 0; i < m->sc.anchor_count; i++) {
            const RenderAnchor *a = &m->sc.anchors[i];
            if (a->row == NO_IDX || (double)a->row < r0 ||
                (double)a->row > r1)
                continue;
            Uint8 alpha = 255;
            if (app->sel_active && app->sel_is_branch && a->branch != app->sel_branch)
                alpha = 70;
            float x = px(v, a->p.x) +
                      (float)(RENDER_LABEL_DX * LANE_W) * (float)v->zoom;
            float y = py(v, a->p.y) + (float)a->slot * slot_h - label_h / 2.0f;
            SDL_Color fg = { 235, 235, 240, 255 };
            Uint8 br = 30, bgc = 34, bb = 44;
            if (a->kind == RENDER_LABEL_TAG) {
                /* tag: colore diverso dai branch */
                fg.r = 226;
                fg.g = 200;
                fg.b = 120;
                br = 56;
                bgc = 46;
                bb = 18;
            } else if (a->is_head) {
                fg.r = 255;
                fg.g = 230;
                fg.b = 120;
                br = 60;
                bgc = 52;
                bb = 20;
            } else if (a->has_remote) {
                fg.r = fg.g = fg.b = 170;
            }
            draw_label(ren, font, x, y, pad, fg, br, bgc, bb, alpha, a->text);
        }
    }

    if (app->panel_on)
        draw_panel(ren, font, m, app);
    draw_status(ren, font, m, app);
}

/* Click sinistro senza trascinamento: commit, etichetta o vuoto. */
static void handle_click(Model *m, App *app, float mx, float my)
{
    if (app->panel_on && mx >= (float)(app->win_w - PANEL_W))
        return; /* il pannello non seleziona */
    if (my >= (float)(app->win_h - STATUS_H))
        return; /* la barra di stato non seleziona */
    const View *v = &app->view;
    double sx = (mx - MARGIN_X) / (LANE_W * v->zoom) + v->off_x;
    double sy = (my - MARGIN_Y) / (ROW_H * v->zoom) + v->off_y;
    double rx = (DOT_R + 5.0) / (LANE_W * v->zoom);
    double ry = (DOT_R + 5.0) / (ROW_H * v->zoom);
    size_t ci = render_commit_at(&m->sc, sx, sy, rx, ry);
    if (ci != NO_IDX) {
        app_select_commit(m, app, ci);
        return;
    }
    size_t ai = render_label_at(&m->sc, sx, sy);
    if (ai != NO_IDX) {
        const RenderAnchor *a = &m->sc.anchors[ai];
        if (a->kind == RENDER_LABEL_BRANCH && a->branch != NO_IDX)
            app_select_branch(m, app, a->branch);
        else
            app_select_commit(m, app, a->commit);
        return;
    }
    app_clear_selection(app);
}

static void handle_key(const SDL_KeyboardEvent *k, Model *m, App *app,
                       bool *running)
{
    View *v = &app->view;
    if (k->key == SDLK_ESCAPE) {
        if (app->sel_active)
            app_clear_selection(app);
        else
            *running = false;
        return;
    }
    if (k->key == SDLK_TAB) {
        app->panel_on = !app->panel_on;
        app_resize(m, app);
        return;
    }
    if (k->key == SDLK_C) {
        if (app->sel_active && app->sel_commit < m->data.commit_count)
            SDL_SetClipboardText(m->data.commits[app->sel_commit].hash);
        return;
    }
    if (k->key == SDLK_R) {
        app_apply_filter(m, app,
                         app->filter == FILTER_LOCAL ? FILTER_NONE : FILTER_LOCAL,
                         NO_IDX);
        return;
    }
    if (k->key == SDLK_B) {
        if (app->sel_active && app->sel_is_branch) {
            if (app->filter == FILTER_ANCESTORS &&
                app->filter_branch == app->sel_branch)
                app_apply_filter(m, app, FILTER_NONE, NO_IDX);
            else
                app_apply_filter(m, app, FILTER_ANCESTORS, app->sel_branch);
        }
        return;
    }
    if (k->key == SDLK_A) {
        app_reset(m, app);
        return;
    }
    if (k->key == SDLK_DOWN)
        v->off_y += 1;
    else if (k->key == SDLK_UP)
        v->off_y -= 1;
    else if (k->key == SDLK_RIGHT)
        v->off_x += 1;
    else if (k->key == SDLK_LEFT)
        v->off_x -= 1;
    else if (k->key == SDLK_PAGEDOWN)
        v->off_y += 10;
    else if (k->key == SDLK_PAGEUP)
        v->off_y -= 10;
    else if (k->key == SDLK_HOME)
        v->off_y = 0;
    else if (k->key == SDLK_END)
        v->off_y = 1e9;
    else if (k->key == SDLK_PLUS || k->key == SDLK_EQUALS ||
             k->key == SDLK_KP_PLUS)
        v->zoom *= 1.1;
    else if (k->key == SDLK_MINUS || k->key == SDLK_KP_MINUS)
        v->zoom /= 1.1;
    else if (k->key == SDLK_0) {
        v->zoom = 1.0;
        v->off_x = 0;
        v->off_y = 0;
    }
    view_clamp(v, m->sc.lane_count, m->sc.rows);
}

int main(int argc, char **argv)
{
    Options opt;
    memset(&opt, 0, sizeof opt);
    opt.width = 1100;
    opt.height = 760;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--font") == 0 && i + 1 < argc)
            opt.font = argv[++i];
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)
            opt.width = atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc)
            opt.height = atoi(argv[++i]);
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            opt.frames = atoi(argv[++i]);
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            opt.screenshot = argv[++i];
        else if (strcmp(argv[i], "--select") == 0 && i + 1 < argc)
            opt.select = argv[++i];
        else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc)
            opt.filter = argv[++i];
        else if (strcmp(argv[i], "--headless") == 0)
            opt.headless = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "opzione sconosciuta: %s\n", argv[i]);
            print_help();
            return 2;
        } else {
            opt.repo = argv[i];
        }
    }
    if (opt.width < 200 || opt.height < 150) {
        fprintf(stderr, "dimensioni della finestra troppo piccole\n");
        return 2;
    }

    char err[1024];
    Model m;
    if (!model_load(&m, opt.repo, err, sizeof err)) {
        fprintf(stderr, "errore: %s\n", err);
        return 1;
    }

    App app;
    memset(&app, 0, sizeof app);
    app.sel_commit = NO_IDX;
    app.sel_branch = NO_IDX;
    app.filter_branch = NO_IDX;
    app.repo = opt.repo;
    app.view.zoom = 1.0;
    app.win_w = opt.width;
    app.win_h = opt.height;
    if (!app_prepare(&m, &app, &opt)) {
        model_free(&m);
        return 2;
    }

    bool headless = opt.headless || opt.screenshot != NULL || opt.frames > 0;
    if (headless)
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        model_free(&m);
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow("gitgraph", opt.width, opt.height,
                                       SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    if (!win || !ren) {
        fprintf(stderr, "finestra/renderer: %s\n", SDL_GetError());
        if (ren)
            SDL_DestroyRenderer(ren);
        if (win)
            SDL_DestroyWindow(win);
        SDL_Quit();
        model_free(&m);
        return 1;
    }
    printf("render: %zu commit, %zu corsie, %zu righe, %zu archi, %zu etichette "
           "(driver %s, renderer %s)\n",
           m.sc.dot_count, m.sc.lane_count, m.sc.rows, m.sc.line_count,
           m.sc.anchor_count, SDL_GetCurrentVideoDriver(),
           SDL_GetRendererName(ren));
    fflush(stdout);

    TTF_Font *font = NULL;
    if (TTF_Init()) {
        if (opt.font) {
            font = TTF_OpenFont(opt.font, LABEL_PT);
            if (!font)
                fprintf(stderr, "font non caricabile (%s): %s\n", opt.font,
                        SDL_GetError());
        } else {
            for (size_t i = 0; g_font_candidates[i] && !font; i++)
                font = TTF_OpenFont(g_font_candidates[i], LABEL_PT);
        }
        if (!font)
            fprintf(stderr,
                    "avviso: nessun font trovato: le etichette non vengono "
                    "disegnate (usa --font)\n");
    } else {
        fprintf(stderr, "avviso: TTF_Init: %s (etichette disattivate)\n",
                SDL_GetError());
    }
    app.font = font;
    update_label_widths(&m.sc, font);

    SDL_GetWindowSizeInPixels(win, &app.win_w, &app.win_h);
    app_resize(&m, &app);

    bool running = true;
    if (headless) {
        int total = opt.screenshot ? 1 : (opt.frames > 0 ? opt.frames : 1);
        for (int f = 0; f < total; f++) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT)
                    running = false;
            }
            if (!running)
                break;
            SDL_GetWindowSizeInPixels(win, &app.win_w, &app.win_h);
            app_resize(&m, &app);
            draw_frame(ren, font, &m, &app);
            SDL_RenderPresent(ren);
        }
        if (opt.screenshot) {
            SDL_Surface *shot = SDL_RenderReadPixels(ren, NULL);
            if (!shot) {
                fprintf(stderr, "screenshot: %s\n", SDL_GetError());
                running = false;
            } else if (!SDL_SaveBMP(shot, opt.screenshot)) {
                fprintf(stderr, "salvataggio BMP: %s\n", SDL_GetError());
                running = false;
            } else {
                printf("screenshot: %s\n", opt.screenshot);
                fflush(stdout);
            }
            if (shot)
                SDL_DestroySurface(shot);
        }
    } else {
        bool dragging = false;
        bool moved = false;
        float down_x = 0, down_y = 0, last_x = 0, last_y = 0;

        SDL_GetWindowSizeInPixels(win, &app.win_w, &app.win_h);
        app_resize(&m, &app);
        draw_frame(ren, font, &m, &app);
        SDL_RenderPresent(ren);

        while (running) {
            SDL_Event ev;
            if (!SDL_WaitEvent(&ev))
                break;
            do {
                if (ev.type == SDL_EVENT_QUIT)
                    running = false;
                else if (ev.type == SDL_EVENT_KEY_DOWN) {
                    if (ev.key.key == SDLK_F5) {
                        if (!app_reload(&m, &app))
                            running = false;
                    } else {
                        handle_key(&ev.key, &m, &app, &running);
                    }
                } else if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                    if (SDL_GetModState() & SDL_KMOD_CTRL) {
                        app.view.zoom *= (ev.wheel.y > 0) ? 1.1 : 1 / 1.1;
                        app_resize(&m, &app);
                    } else {
                        app.view.off_y -= ev.wheel.y;
                        app.view.off_x -= ev.wheel.x;
                        view_clamp(&app.view, m.sc.lane_count, m.sc.rows);
                    }
                } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                           ev.button.button == SDL_BUTTON_LEFT) {
                    dragging = true;
                    moved = false;
                    down_x = last_x = ev.button.x;
                    down_y = last_y = ev.button.y;
                } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                           ev.button.button == SDL_BUTTON_LEFT) {
                    if (dragging && !moved)
                        handle_click(&m, &app, ev.button.x, ev.button.y);
                    dragging = false;
                } else if (ev.type == SDL_EVENT_MOUSE_MOTION && dragging) {
                    if (fabsf(ev.motion.x - down_x) > 3.0f ||
                        fabsf(ev.motion.y - down_y) > 3.0f)
                        moved = true;
                    app.view.off_x -=
                        (ev.motion.x - last_x) / (LANE_W * app.view.zoom);
                    app.view.off_y -=
                        (ev.motion.y - last_y) / (ROW_H * app.view.zoom);
                    last_x = ev.motion.x;
                    last_y = ev.motion.y;
                    view_clamp(&app.view, m.sc.lane_count, m.sc.rows);
                }
            } while (SDL_PollEvent(&ev));
            SDL_GetWindowSizeInPixels(win, &app.win_w, &app.win_h);
            app_resize(&m, &app);
            draw_frame(ren, font, &m, &app);
            SDL_RenderPresent(ren);
        }
    }

    if (font)
        TTF_CloseFont(font);
    TTF_Quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    model_free(&m);
    return 0;
}

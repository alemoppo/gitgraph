/* gui_main.c - finestra SDL3/SDL3_ttf sulla scena (fase 5).
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
 * archi, ancore delle etichette) e' in render.c, senza SDL. Questa fase non
 * scrive sul repository: legge e disegna.
 *
 * Comandi: frecce per scorrere, PagSu/PagGiu di una schermata, Home/Fine,
 * "+"/"-" per lo zoom, "0" per tornare all'inizio, ESC per uscire; rotella del
 * mouse per scorrere, Ctrl+rotella per lo zoom, trascinamento col sinistro per
 * spostare la vista.
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
#define ZOOM_MIN 0.35
#define ZOOM_MAX 4.0

typedef struct {
    const char *repo;
    const char *font;
    const char *screenshot;
    int width;
    int height;
    int frames;
    bool headless;
} Options;

typedef struct {
    double off_x; /* prima corsia visibile (in unita' di scena) */
    double off_y; /* prima riga visibile */
    double zoom;
    int width;
    int height;
} View;

static const char *const g_font_candidates[] = {
    "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/tahoma.ttf",  "C:/Windows/Fonts/consola.ttf",
    NULL,
};

static void print_help(void)
{
    printf("uso: gitgraph [--font PERCORSO] [--width N] [--height N]\n"
           "              [--headless] [--screenshot P] [--frames N] [--help]\n"
           "              [percorso-repo]\n"
           "  percorso-repo  default: directory corrente\n");
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

static void draw_label(SDL_Renderer *ren, TTF_Font *font, float x, float y,
                       SDL_Color fg, Uint8 bg_r, Uint8 bg_g, Uint8 bg_b,
                       const char *text)
{
    if (!font)
        return;
    SDL_Surface *s = TTF_RenderText_Blended(font, text, 0, fg);
    if (!s)
        return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    if (t) {
        float pad = 3.0f;
        SDL_FRect bg = { x, y, (float)s->w + 2 * pad, (float)s->h + 2 * pad };
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, bg_r, bg_g, bg_b, 215);
        SDL_RenderFillRect(ren, &bg);
        SDL_FRect dst = { x + pad, y + pad, (float)s->w, (float)s->h };
        SDL_RenderTexture(ren, t, NULL, &dst);
        SDL_DestroyTexture(t);
    }
    SDL_DestroySurface(s);
}

static void draw_frame(SDL_Renderer *ren, TTF_Font *font, const View *v,
                       const Render *sc)
{
    SDL_SetRenderDrawColor(ren, 18, 20, 26, 255);
    SDL_RenderClear(ren);

    double vis_rows = (v->height + 2 * MARGIN_Y) / (ROW_H * v->zoom);
    double vis_cols = (v->width + 2 * MARGIN_X) / (LANE_W * v->zoom);
    double r0 = v->off_y - 1, r1 = v->off_y + vis_rows + 1;
    double c0 = v->off_x - 1, c1 = v->off_x + vis_cols + 1;

    /* archi: sotto i punti, cosi' i nodi restano leggibili */
    for (size_t i = 0; i < sc->line_count; i++) {
        const RenderLine *ln = &sc->lines[i];
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
        uint8_t rgb[3];
        render_lane_color(ln->lane, rgb);
        SDL_SetRenderDrawColor(ren, rgb[0], rgb[1], rgb[2], 205);
        stroke_path(ren, v, ln, ln->dashed ? 2.0f : 3.0f * (float)v->zoom);
    }

    /* punti dei commit (in ordine di riga: si puo' interrompere presto) */
    float r = DOT_R * (float)v->zoom;
    for (size_t i = 0; i < sc->dot_count; i++) {
        const RenderDot *d = &sc->dots[i];
        if ((double)d->row < r0)
            continue;
        if ((double)d->row > r1)
            break;
        uint8_t rgb[3];
        render_lane_color(d->lane, rgb);
        float cx = px(v, d->p.x), cy = py(v, d->p.y);
        if (d->head) {
            SDL_SetRenderDrawColor(ren, 245, 246, 250, 255);
            fill_circle(ren, cx, cy, r + 3.0f * (float)v->zoom);
        }
        if (d->anon) {
            SDL_SetRenderDrawColor(ren, (Uint8)(rgb[0] / 2 + 60),
                                   (Uint8)(rgb[1] / 2 + 60),
                                   (Uint8)(rgb[2] / 2 + 60), 255);
        } else {
            SDL_SetRenderDrawColor(ren, rgb[0], rgb[1], rgb[2], 255);
        }
        fill_circle(ren, cx, cy, r);
    }

    /* etichette dei riferimenti: gruppi uniti, impilati con lo slot */
    if (font && v->zoom >= 0.5) {
        TTF_SetFontSize(font, LABEL_PT * (float)v->zoom);
        int fh = TTF_GetFontHeight(font);
        float slot_h = (float)fh + 4.0f;
        for (size_t i = 0; i < sc->anchor_count; i++) {
            const RenderAnchor *a = &sc->anchors[i];
            if (a->row == (size_t)-1 || (double)a->row < r0 ||
                (double)a->row > r1)
                continue;
            float x = px(v, a->p.x) + 9.0f * (float)v->zoom;
            float y = py(v, a->p.y) - fh / 2.0f + (float)a->slot * slot_h;
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
            draw_label(ren, font, x, y, fg, br, bgc, bb, a->text);
        }
    }
}

static void handle_key(const SDL_KeyboardEvent *k, View *v, bool *running,
                       size_t lanes, size_t rows)
{
    if (k->key == SDLK_ESCAPE) {
        *running = false;
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
    view_clamp(v, lanes, rows);
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
    GitData data;
    if (!gitdata_load(&data, opt.repo, err, sizeof err)) {
        fprintf(stderr, "errore: %s\n", err);
        return 1;
    }
    GraphConfig cfg;
    load_config(&data, &cfg);

    History hist;
    if (!history_build(&hist, &data, &cfg, err, sizeof err)) {
        fprintf(stderr, "errore: %s\n", err);
        config_free(&cfg);
        gitdata_free(&data);
        return 1;
    }
    Layout lay;
    layout_build(&lay, &hist);
    Render sc;
    render_build(&sc, &data, &hist, &lay);

    bool headless = opt.headless || opt.screenshot != NULL || opt.frames > 0;
    if (headless)
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        render_free(&sc);
        layout_free(&lay);
        history_free(&hist);
        config_free(&cfg);
        gitdata_free(&data);
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
        render_free(&sc);
        layout_free(&lay);
        history_free(&hist);
        config_free(&cfg);
        gitdata_free(&data);
        return 1;
    }
    printf("render: %zu commit, %zu corsie, %zu righe, %zu archi, %zu etichette "
           "(driver %s, renderer %s)\n",
           sc.dot_count, sc.lane_count, sc.rows, sc.line_count, sc.anchor_count,
           SDL_GetCurrentVideoDriver(), SDL_GetRendererName(ren));
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

    View view;
    view.off_x = 0;
    view.off_y = 0;
    view.zoom = 1.0;
    view.width = opt.width;
    view.height = opt.height;
    view_clamp(&view, sc.lane_count, sc.rows);

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
            SDL_GetWindowSizeInPixels(win, &view.width, &view.height);
            draw_frame(ren, font, &view, &sc);
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
        float drag_x = 0, drag_y = 0;

        SDL_GetWindowSizeInPixels(win, &view.width, &view.height);
        draw_frame(ren, font, &view, &sc);
        SDL_RenderPresent(ren);

        while (running) {
            SDL_Event ev;
            if (!SDL_WaitEvent(&ev))
                break;
            do {
                if (ev.type == SDL_EVENT_QUIT)
                    running = false;
                else if (ev.type == SDL_EVENT_KEY_DOWN)
                    handle_key(&ev.key, &view, &running, sc.lane_count, sc.rows);
                else if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                    if (SDL_GetModState() & SDL_KMOD_CTRL) {
                        view.zoom *= (ev.wheel.y > 0) ? 1.1 : 1 / 1.1;
                        view_clamp(&view, sc.lane_count, sc.rows);
                    } else {
                        view.off_y -= ev.wheel.y;
                        view.off_x -= ev.wheel.x;
                        view_clamp(&view, sc.lane_count, sc.rows);
                    }
                } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                           ev.button.button == SDL_BUTTON_LEFT) {
                    dragging = true;
                    drag_x = ev.button.x;
                    drag_y = ev.button.y;
                } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                           ev.button.button == SDL_BUTTON_LEFT) {
                    dragging = false;
                } else if (ev.type == SDL_EVENT_MOUSE_MOTION && dragging) {
                    view.off_x -= (ev.motion.x - drag_x) / (LANE_W * view.zoom);
                    view.off_y -= (ev.motion.y - drag_y) / (ROW_H * view.zoom);
                    drag_x = ev.motion.x;
                    drag_y = ev.motion.y;
                    view_clamp(&view, sc.lane_count, sc.rows);
                }
            } while (SDL_PollEvent(&ev));
            SDL_GetWindowSizeInPixels(win, &view.width, &view.height);
            draw_frame(ren, font, &view, &sc);
            SDL_RenderPresent(ren);
        }
    }

    if (font)
        TTF_CloseFont(font);
    TTF_Quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    render_free(&sc);
    layout_free(&lay);
    history_free(&hist);
    config_free(&cfg);
    gitdata_free(&data);
    return 0;
}

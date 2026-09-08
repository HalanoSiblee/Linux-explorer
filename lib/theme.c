/* theme.c -- the parts of Windows XP and Windows 7 that are painted, not
 * merely coloured.
 *
 * The classic look is edges: two pixels of light and shadow around a flat
 * face, which w2k_edge() draws. Luna threw that away for gradients,
 * rounded corners and glyphs on coloured buttons, and no colour table can
 * express any of it.
 *
 * Where a real screenshot of the element exists, the element is *cropped
 * from it* and shipped as a skin under skins/: the caption is a left cap,
 * a middle column and a right cap; the caption buttons are 21x21 cells;
 * the frame is its two bottom corners; the task button is two caps and a
 * column; the taskbar is one column. The painters below tile those. Each
 * skin was checked by rebuilding the element it came from and diffing:
 * zero pixels differ. That is the only sense in which "pixel-exact" means
 * anything.
 *
 * The stop tables that follow are the fallback for a system without the
 * skins: drawn from the two themes' documented palettes, and they say so.
 */
#include "w2k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * Skins, found and cached by name
 * ------------------------------------------------------------------ */
/* Where a skin may live: the user's own, beside the binaries when run
 * from the source tree, and the installed copy. */
int w2k_skin_path(const char *name, char *out, int n)
{
    const char *home = getenv("HOME");
    if (home) {
        snprintf(out, (size_t)n, "%s/.w2k/skins/%s", home, name);
        if (access(out, R_OK) == 0) return 1;
    }
    char exe[768];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0) {
        exe[len] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;                       /* .../bin */
            snprintf(out, (size_t)n, "%.760s/../skins/%.120s", exe, name);
            if (access(out, R_OK) == 0) return 1;
        }
    }
    snprintf(out, (size_t)n, W2K_PREFIX "/share/w2k/skins/%s", name);
    return access(out, R_OK) == 0;
}

/* `scale` is applied to every colour channel (256 = as is): the hot and
 * pressed states of a button are not in any screenshot to hand, so they
 * are the measured button lightened or darkened. */
static struct { char name[64]; int scale; W2kSkin *s; int tried; } skin_cache[16];
static int nskin_cache;

/* Forget every skin read so far: the files may have changed (a new set of
 * artwork, a theme edited under ~/.w2k/skins); they are read again on the
 * next paint. */
void w2k_skin_cache_flush(void)
{
    for (int i = 0; i < nskin_cache; i++) w2k_skin_free(skin_cache[i].s);
    nskin_cache = 0;
}

static W2kSkin *skin(const char *name, int scale)
{
#define cache skin_cache
#define n nskin_cache
    for (int i = 0; i < n; i++)
        if (cache[i].scale == scale && !strcmp(cache[i].name, name))
            return cache[i].s;
    if (n >= 16) return NULL;
    char path[1024];
    W2kSkin *s = w2k_skin_path(name, path, sizeof path)
               ? w2k_skin_load_scaled(path, scale) : NULL;
    snprintf(cache[n].name, sizeof cache[n].name, "%s", name);
    cache[n].scale = scale;
    cache[n].s = s;
    cache[n].tried = 1;
    n++;
    return s;
#undef cache
#undef n
}

/* ------------------------------------------------------------------ *
 * Fallback gradients (stop tables; positions in thousandths of height)
 * ------------------------------------------------------------------ */
typedef struct { int at; unsigned char r, g, b; } Stop;

static const Stop cap_xp_active[] = {
    {    0,   0,  88, 238 }, {   35,  63, 151, 255 }, {   70,  43, 144, 255 },
    {  105,   3, 114, 255 }, {  140,   3, 101, 241 }, {  175,   0,  92, 233 },
    {  210,   0,  84, 227 }, {  480,   0,  85, 229 }, {  620,   0,  90, 245 },
    {  740,   2, 106, 254 }, {  860,   0, 101, 253 }, {  930,   0,  77, 227 },
    { 1000,   0,  67, 207 },
};
static const Stop cap_xp_inactive[] = {
    {    0, 122, 153, 224 }, {  120, 121, 150, 222 }, {  420, 123, 151, 224 },
    {  620, 125, 155, 227 }, {  760, 129, 167, 232 }, {  860, 130, 169, 233 },
    {  930, 128, 165, 231 }, { 1000, 122, 147, 223 },
};
static const Stop cap_7_active[] = {
    {    0, 232, 241, 250 }, {  120, 215, 228, 242 }, {  700, 190, 212, 236 },
    { 1000, 176, 200, 226 },
};
static const Stop cap_7_inactive[] = {
    {    0, 245, 249, 252 }, {  200, 239, 245, 250 }, { 1000, 219, 232, 244 },
};
static const Stop btn_xp_close[] = {
    {    0, 228,  95,  62 }, {  110, 233, 124,  98 }, {  220, 231, 111,  84 },
    {  380, 227,  92,  58 }, {  610, 231, 101,  61 }, {  780, 230,  93,  50 },
    {  880, 226,  85,  42 }, {  950, 210,  69,  30 }, { 1000, 174,  49,  16 },
};
static const Stop btn_xp_blue[] = {
    {    0,  31,  95, 231 }, {  110,  51, 104, 230 }, {  220,  43,  96, 229 },
    {  390,  28,  86, 231 }, {  620,  28,  93, 236 }, {  780,  28, 100, 240 },
    {  880,  23, 100, 245 }, { 1000,   7,  78, 217 },
};
static const Stop btn_7_close[] = {
    {    0, 232, 108,  92 }, {  400, 214,  70,  56 }, { 1000, 180,  38,  30 },
};
static const Stop btn_7_blue[] = {
    {    0, 246, 249, 252 }, {  400, 226, 235, 245 }, { 1000, 206, 219, 234 },
};
static const Stop task_xp[] = {
    {    0,  72, 146, 247 }, {   60,  77, 139, 241 }, {  130,  66, 134, 244 },
    {  200,  61, 130, 244 }, {  830,  57, 128, 244 }, {  900,  49, 108, 228 },
    { 1000,  38,  83, 184 },
};
static const Stop task_7[] = {
    {    0,  78,  86,  96 }, {  120,  58,  65,  74 }, {  850,  44,  50,  58 },
    { 1000,  30,  34,  40 },
};

static void stop_rgb(const Stop *st, int n, int at, int *r, int *g, int *b)
{
    int i = 0;
    while (i < n - 1 && st[i + 1].at < at) i++;
    const Stop *a = &st[i], *c = &st[i + 1 < n ? i + 1 : i];
    int span = c->at - a->at;
    int t = span > 0 ? (at - a->at) * 255 / span : 0;
    *r = a->r + (c->r - a->r) * t / 255;
    *g = a->g + (c->g - a->g) * t / 255;
    *b = a->b + (c->b - a->b) * t / 255;
}

/* The window manager calls into this file in raw mode (frame chrome, in
 * screen pixels) and the taskbar in logical mode; S() turns a measured
 * constant into whichever the caller is working in. */
static int S(int v) { return w2k_scale_raw ? w2k_px(v) : v; }

static void grad_fill(Drawable d, int x, int y, int w, int h,
                      const Stop *st, int n, const int *inset, int scale)
{
    /* One row per screen pixel; `inset` is indexed by the caller's row. */
    int px = w2k_cx(x), py = w2k_cx(y), pw = w2k_cw(x, w), ph = w2k_cw(y, h);
    for (int i = 0; i < ph; i++) {
        int at = ph > 1 ? i * 1000 / (ph - 1) : 0;
        int r, g, b;
        stop_rgb(st, n, at, &r, &g, &b);
        r = r * scale / 256; g = g * scale / 256; b = b * scale / 256;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        int li = h > 0 ? (int)((long)i * h / ph) : 0;
        int off = inset ? w2k_cw(x, inset[li]) : 0;
        if (pw - 2 * off <= 0) continue;
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(r, g, b));
        XFillRectangle(w2k.dpy, d, w2k.gc, px + off, py + i,
                       (unsigned)(pw - 2 * off), 1);
    }
}

static int *corner_insets(int h, int rad, int bottom_too)
{
    int *ins = calloc((size_t)(h > 0 ? h : 1), sizeof *ins);
    if (!ins) return NULL;
    for (int i = 0; i < h && i < rad; i++) {
        int dy = rad - 1 - i, dx = rad - 1;
        while (dx > 0 && dx * dx + dy * dy > (rad - 1) * (rad - 1) + 1) dx--;
        ins[i] = rad - 1 - dx;
    }
    if (bottom_too)
        for (int i = 0; i < h && i < rad; i++)
            if (h - 1 - i >= 0 && ins[h - 1 - i] < ins[i]) ins[h - 1 - i] = ins[i];
    return ins;
}

/* Draw a skin strip laid out as [left cap][one column][right cap] across
 * a width, tiling the column. Rows beyond the strip repeat its last row,
 * so an element a pixel taller than the source does not tear. */
static void strip_draw(Drawable d, W2kSkin *s, int sy, int sh, int lcap,
                       int rcap, int x, int y, int w, int h)
{
    /* Source rectangles are in the sheet's own pixels; the destination
     * is in the caller's. */
    int sw = w2k_skin_w(s);
    int hl = w2k_scale_raw ? w2k_lp(h) : h;      /* rows wanted, in sheet pixels */
    int hh = hl < sh ? hl : sh;
    int lc = S(lcap), rc = S(rcap), shp = S(sh);
    int midw = w - lc - rc;
    /* Three requests: the caps copied, the column between them tiled. */
    w2k_skin_draw(d, s, x, y, 0, sy, lcap, hh);
    if (midw > 0) w2k_skin_tile(d, s, x + lc, y, midw, S(hh), lcap, sy, 1, hh);
    w2k_skin_draw(d, s, x + w - rc, y, sw - rcap, sy, rcap, hh);
    if (h > shp) {
        int ly = sy + sh - 1;
        w2k_skin_tile(d, s, x, y + shp, lc, h - shp, 0, ly, lcap, 1);
        if (midw > 0)
            w2k_skin_tile(d, s, x + lc, y + shp, midw, h - shp, lcap, ly, 1, 1);
        w2k_skin_tile(d, s, x + w - rc, y + shp, rc, h - shp, sw - rcap, ly,
                      rcap, 1);
    }
}

/* ------------------------------------------------------------------ *
 * Windows 7 Basic
 *
 * Cropped from the theme's own artwork, supplied as three strips: an
 * active title bar, an inactive one, and the taskbar's texture. The frame
 * is a flat steel blue with a vertical gradient down the caption -- no
 * variation across it -- inside a dark outline and a light line; ten
 * pixels of border, a 31-row caption strip (outline, light line, 29 rows
 * of gradient), buttons 32 by 18 hanging at row 10. So the caption is two
 * caps and a tiled column, and the borders a tiled row and column.
 * ------------------------------------------------------------------ */
#define W7_BORDER     10
#define W7_CAP_H      31
#define W7_CAP_LCAP   40      /* the corner and the icon's place */
#define W7_CAP_RCAP   12
#define W7_BTN_W      32
#define W7_BTN_H      18
#define W7_BTN_Y      10
#define W7_BTN_GAP     2
#define W7_BTN_INSET  10      /* Close's right edge from the frame's */
#define W7_BAR_H      40

/* ------------------------------------------------------------------ *
 * Window captions
 * ------------------------------------------------------------------ */
#define XP_CAP_H 30

/* Windows Vista Basic: the reference bar is 30 rows and its task buttons
 * 27, from row 2 to the light line under them at row 28. */
#define VISTA_BAR_H 30
#define VISTA_BTN_H 27

/* The Modern look: a 31-row caption under a one-pixel outline, with the
 * six-pixel invisible margin outside it that Windows 10 and 11 keep for
 * the resize cursor; caption buttons 46 by 30 like theirs, the glyphs
 * ten pixels wide in a one-pixel stroke. */
#define MD_CAP_H   31
#define MD_MARGIN  6
#define MD_BTN_W   46
#define MD_BTN_H   30
#define MD_GLYPH   10

int w2k_theme_modern_margin(void) { return w2k_px(MD_MARGIN); }

/* An rgb triple as a foreground. */
static void fg_rgb(const int c[3])
{
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(c[0], c[1], c[2]));
}

void w2k_theme_caption(Drawable d, int x, int y, int w, int h, int active,
                       int theme)
{
    if (theme == THEME_MODERN) {
        /* The caption colour, flat, with the outline along its top and
         * down both sides; the sides carry on below it in the frame. */
        int t = w2k_scale_raw ? w2k_th(1) : 1;
        w2k_fill(d, x, y, w, h, active ? C_ACTIVETITLE : C_INACTIVETITLE);
        w2k_fill(d, x, y, w, t, C_WINDOWFRAME);
        w2k_fill(d, x, y, t, h, C_WINDOWFRAME);
        w2k_fill(d, x + w - t, y, t, h, C_WINDOWFRAME);
        return;
    }
    if (W2K_THEME_IS7(theme)) {
        W2kSkin *s = skin("w7-caption.png", 256);
        if (s && w2k_skin_w(s) == W7_CAP_LCAP + 1 + W7_CAP_RCAP &&
            w2k_skin_h(s) == 2 * W7_CAP_H && w >= S(W7_CAP_LCAP + W7_CAP_RCAP)) {
            strip_draw(d, s, active ? 0 : W7_CAP_H, W7_CAP_H, W7_CAP_LCAP,
                       W7_CAP_RCAP, x, y, w, h);
            return;
        }
    }
    if (theme == THEME_XP) {
        W2kSkin *s = skin("xp-caption.png", 256);
        if (s && w2k_skin_w(s) == 59 && w2k_skin_h(s) == 2 * XP_CAP_H && w >= S(58)) {
            /* The caps are 28 wide: the caption's gradient fades over its
             * last 27 columns at each end. Tiling one column at a time is
             * a lot of tiny copies for a wide caption; the server handles
             * it, and it only happens on a focus or title change. */
            strip_draw(d, s, active ? 0 : XP_CAP_H, XP_CAP_H, 29, 29, x, y, w, h);
            return;
        }
    }
    const Stop *st;
    int n;
    if (W2K_THEME_IS7(theme)) {
        st = active ? cap_7_active : cap_7_inactive;
        n = active ? (int)(sizeof cap_7_active / sizeof *cap_7_active)
                   : (int)(sizeof cap_7_inactive / sizeof *cap_7_inactive);
    } else {
        st = active ? cap_xp_active : cap_xp_inactive;
        n = active ? (int)(sizeof cap_xp_active / sizeof *cap_xp_active)
                   : (int)(sizeof cap_xp_inactive / sizeof *cap_xp_inactive);
    }
    int *ins = corner_insets(h, S(W2K_THEME_IS7(theme) ? 4 : 6), 0);
    grad_fill(d, x, y, w, h, st, n, ins, 256);
    free(ins);
}

int w2k_theme_caption_h(int theme)
{
    /* Windows XP: 30 rows from the frame's top edge to the client, of
     * which 4 are the frame border. Windows 7 Basic: 31, of which 10. */
    if (theme == THEME_AERO) return AERO_TOP - AERO_BORDER;      /* Aero: 36, of which 8 */
    if (W2K_THEME_IS7(theme)) return W7_CAP_H - W7_BORDER;
    if (theme == THEME_MODERN) return MD_CAP_H;
    return XP_CAP_H - 4;
}

/* The frame's border: four pixels each side and along the bottom, with
 * rounded bottom corners, from the corner blocks in the frame skin. The
 * client window covers everything inside, so the corner blocks can be
 * blitted whole. */
void w2k_theme_frame_edges(Drawable d, int fw, int fh, int b, int active,
                           int theme)
{
    if (theme == THEME_MODERN) {
        /* One line, inside the invisible margin (which is shaped away,
         * so nothing is drawn there). */
        (void)active;
        int t = w2k_scale_raw ? w2k_th(1) : 1;
        int m = b - t;
        if (m < 0) m = 0;
        w2k_fill(d, m, m, t, fh - 2 * m, C_WINDOWFRAME);
        w2k_fill(d, fw - m - t, m, t, fh - 2 * m, C_WINDOWFRAME);
        w2k_fill(d, m, fh - m - t, fw - 2 * m, t, C_WINDOWFRAME);
        return;
    }
    if (W2K_THEME_IS7(theme) && b == S(W7_BORDER)) {
        W2kSkin *fr = skin("w7-frame.png", 256), *bt = skin("w7-bottom.png", 256);
        if (fr && bt && w2k_skin_w(fr) == 20 && w2k_skin_h(fr) == 2 &&
            w2k_skin_w(bt) == 1 && w2k_skin_h(bt) == 20) {
            /* The bottom first, then the sides over its ends, so the
             * outline turns the corner squarely. */
            int row = active ? 0 : 1;
            int bd = S(W7_BORDER), ch = S(W7_CAP_H);
            w2k_skin_tile(d, bt, 0, fh - bd, fw, bd, 0,
                          active ? 0 : W7_BORDER, 1, W7_BORDER);
            int sh = fh - ch;
            if (sh > 0) {
                w2k_skin_tile(d, fr, 0, ch, bd, sh, 0, row, W7_BORDER, 1);
                w2k_skin_tile(d, fr, fw - bd, ch, bd, sh,
                              W7_BORDER, row, W7_BORDER, 1);
            }
            return;
        }
    }
    W2kSkin *s = theme == THEME_XP ? skin("xp-frame.png", 256) : NULL;
    W2kSkin *bt = theme == THEME_XP ? skin("xp-bottom.png", 256) : NULL;
    W2kSkin *rs = theme == THEME_XP ? skin("xp-rightshade.png", 256) : NULL;
    if (s && bt && w2k_skin_w(s) == 36 && w2k_skin_h(s) == 16 && b == S(4)) {
        int sy = active ? 0 : 8;
        /* Sides: the corner block's top row, repeated down. The right side
         * carries seventeen rows of shading just under the caption -- the
         * corner's shadow -- which come from their own strip. */
        int side_h = fh - S(8);
        if (side_h > 0) {
            w2k_skin_tile(d, s, 0, 0, S(4), side_h, 0, sy, 4, 1);
            w2k_skin_tile(d, s, fw - S(4), 0, S(4), side_h, 32, sy, 4, 1);
            int n = w2k_scale_raw ? w2k_lp(side_h) - XP_CAP_H : side_h - XP_CAP_H;
            if (n > 17) n = 17;
            if (rs && n > 0)
                w2k_skin_draw(d, rs, fw - S(4), S(XP_CAP_H), 0, active ? 0 : 17, 4, n);
        }
        /* Bottom: four rows sampled well away from the corners. */
        if (fw - S(36) > 0)
            w2k_skin_tile(d, bt, S(28), fh - S(4), fw - S(36), S(4), 0, active ? 0 : 4, 1, 4);
        /* The bottom-left curve is a long one -- 26 columns -- so that
         * corner block is 28 wide; the right one is 8. */
        w2k_skin_draw(d, s, 0, fh - S(8), 0, sy, 28, 8);
        w2k_skin_draw(d, s, fw - S(8), fh - S(8), 28, sy, 8, 8);
        return;
    }
    int side = active ? C_ACTIVETITLE : C_INACTIVETITLE;
    w2k_fill(d, 0, 0, b, fh, side);
    w2k_fill(d, fw - b, 0, b, fh, side);
    w2k_fill(d, 0, fh - b, fw, b, side);
}

/* ------------------------------------------------------------------ *
 * Caption buttons
 * ------------------------------------------------------------------ */
int w2k_theme_capbtn_size(int theme)
{
    if (theme == THEME_AERO) return 20;       /* the cluster is twenty rows tall */
    return W2K_THEME_IS7(theme) ? W7_BTN_H : theme == THEME_MODERN ? MD_BTN_H : 21;
}

int w2k_theme_capbtn_w(int theme, int kind)
{
    /* Aero, measured: Minimise 30, Maximise 28, Close 50, touching. */
    if (theme == THEME_AERO) return kind == W2K_CAP_CLOSE ? 50 : kind == W2K_CAP_MIN ? 30 : 28;
    (void)kind;
    return W2K_THEME_IS7(theme) ? W7_BTN_W : theme == THEME_MODERN ? MD_BTN_W : 21;   /* all three alike */
}

/* Where the buttons sit on an XP caption, measured: 21 pixels square at
 * row 6, their right edges 27, 50 and 73 pixels in from the frame's. */
void w2k_theme_capbtn_place(int theme, int fw, int *y, int *close_x,
                            int *max_x, int *min_x)
{
    if (theme == THEME_MODERN) {
        /* Flush with the caption's right edge, inside its outline, and
         * touching one another. */
        *y = S(1);
        *close_x = fw - S(1 + MD_BTN_W);
        *max_x = *close_x - S(MD_BTN_W);
        *min_x = *max_x - S(MD_BTN_W);
        return;
    }
    if (theme == THEME_AERO) {
        /* The cluster hangs from the frame's top edge, its right end six
         * pixels in: Close 50 wide, then Maximise 28 and Minimise 30. */
        *y = S(1);
        *close_x = fw - S(55);
        *max_x = fw - S(83);
        *min_x = fw - S(113);
        return;
    }
    if (W2K_THEME_IS7(theme)) {
        /* Measured: 32 wide, two apart, Close ten pixels in from the edge. */
        *y = S(W7_BTN_Y);
        *close_x = fw - S(W7_BTN_INSET + W7_BTN_W);
        *max_x = *close_x - S(W7_BTN_GAP + W7_BTN_W);
        *min_x = *max_x - S(W7_BTN_GAP + W7_BTN_W);
        return;
    }
    *y = S(6);
    *close_x = fw - S(27);
    *max_x = fw - S(50);
    *min_x = fw - S(73);
}

void w2k_theme_capbtn(Drawable d, int x, int y, int w, int h, int kind,
                      int active, int pressed, int theme)
{
    if (theme == THEME_MODERN) {
        /* No button until it is pressed: then a darker cell, or the red
         * one for Close. The glyph is a one-pixel stroke in the title's
         * colour, ten pixels across, centred. */
        int t = w2k_scale_raw ? w2k_th(1) : 1;
        int g[3], bg[3];
        const unsigned char *tc = w2k_scheme_rgb(active ? C_TITLETEXT
                                                        : C_INACTIVETITLETEXT);
        const unsigned char *cc = w2k_scheme_rgb(active ? C_ACTIVETITLE
                                                        : C_INACTIVETITLE);
        g[0] = tc[0]; g[1] = tc[1]; g[2] = tc[2];
        bg[0] = cc[0]; bg[1] = cc[1]; bg[2] = cc[2];
        if (pressed) {
            if (kind == W2K_CAP_CLOSE) {
                bg[0] = 196; bg[1] = 43; bg[2] = 28;
                g[0] = g[1] = g[2] = 255;
            } else
                w2k_modern_rgb(MODERN_PRESSED, bg);
            w2k_fill_rgb(d, x, y, w, h, bg[0], bg[1], bg[2]);
        }
        fg_rgb(g);
        int n = S(MD_GLYPH);
        int cx = x + (w - n) / 2, cy = y + (h - n) / 2;   /* the glyph's box */
        switch (kind) {
        case W2K_CAP_MIN:
            w2k_fill_fg(d, cx, cy + n / 2, n, t);
            break;
        case W2K_CAP_MAX:
            w2k_frame_fg(d, cx, cy, n, n);
            break;
        case W2K_CAP_RESTORE: {
            /* Two overlapping frames: the back one's top and right sides
             * show past the front one. */
            int s = n - S(2);
            w2k_fill_fg(d, cx + S(2), cy, s, t);
            w2k_fill_fg(d, cx + n - t, cy, t, s);
            w2k_frame_fg(d, cx, cy + S(2), s, s);
            break;
        }
        default:
            /* The cross: two one-pixel diagonals. A bare diagonal of
             * single pixels touches only at the corners and reads as a
             * chain of dots, white on a dark caption especially, so at
             * one pixel each step gets two soft neighbours -- the same
             * anti-aliasing the Windows glyph has. */
            if (t == 1) {
                int soft[3];
                for (int k = 0; k < 3; k++) soft[k] = (g[k] * 2 + bg[k] * 3) / 5;
                fg_rgb(soft);
                for (int i = 0; i < n; i++) {
                    int xa = cx + i, xb = cx + n - 1 - i, yy = cy + i;
                    if (i + 1 < n) {
                        w2k_fill_fg(d, xa + 1, yy, 1, 1);
                        w2k_fill_fg(d, xa, yy + 1, 1, 1);
                        w2k_fill_fg(d, xb - 1, yy, 1, 1);
                        w2k_fill_fg(d, xb, yy + 1, 1, 1);
                    }
                }
                fg_rgb(g);
            }
            for (int i = 0; i < n; i++) {
                w2k_fill_fg(d, cx + i, cy + i, t, t);
                w2k_fill_fg(d, cx + n - t - i, cy + i, t, t);
            }
        }
        return;
    }
    if (W2K_THEME_IS7(theme)) {
        W2kSkin *s = skin("w7-capbtn.png", pressed ? 200 : 256);
        if (s && w2k_skin_w(s) == 134 && w2k_skin_h(s) == 2 * W7_BTN_H &&
            w == S(W7_BTN_W) && h == S(W7_BTN_H)) {
            /* Four cells: Minimise, Maximise, Close from the artwork, and
             * Restore built from the Maximise cell -- its frame glyph
             * drawn twice, one behind the other. */
            int cell = kind == W2K_CAP_CLOSE ? 2 : kind == W2K_CAP_MIN ? 0
                     : kind == W2K_CAP_RESTORE ? 3 : 1;
            w2k_skin_draw(d, s, x, y, cell * (W7_BTN_W + W7_BTN_GAP),
                          active ? 0 : W7_BTN_H, W7_BTN_W, W7_BTN_H);
            return;
        }
    }
    if (theme == THEME_XP) {
        W2kSkin *s = skin("xp-capbtn.png", pressed ? 200 : 256);
        if (s && w2k_skin_w(s) == 63 && w2k_skin_h(s) == 44 && w == S(21) && h == S(21)) {
            /* Cells are 21 by 22: the last row is the button's shadow. */
            int cell = kind == W2K_CAP_CLOSE ? 2 : kind == W2K_CAP_MIN ? 0 : 1;
            w2k_skin_draw(d, s, x, y, cell * 21, active ? 0 : 22, 21, 22);
            if (kind == W2K_CAP_RESTORE) {
                /* No restore button in the screenshots: the maximise
                 * button with the two-frames glyph over its own. */
                int cx = x + S(10), cy = y + S(10);
                w2k_fill(d, cx - S(4), cy - S(5), S(11), S(11), C_HIGHLIGHT);
                w2k_fill_rgb(d, cx - S(3), cy - S(4), S(9), S(9), 28, 93, 236);
                XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(255, 255, 255));
                w2k_fill_fg(d, cx - S(1), cy - S(5), S(7), S(2));
                w2k_fill_fg(d, cx + S(5), cy - S(5), S(1), S(5));
                w2k_fill_fg(d, cx - S(5), cy - S(1), S(7), S(2));
                w2k_fill_fg(d, cx - S(5), cy - S(1), S(1), S(6));
                w2k_fill_fg(d, cx - S(5), cy + S(4), S(7), S(1));
                w2k_fill_fg(d, cx + S(1), cy - S(1), S(1), S(6));
            }
            return;
        }
    }

    int seven = W2K_THEME_IS7(theme);
    const Stop *st = kind == W2K_CAP_CLOSE ? (seven ? btn_7_close : btn_xp_close)
                                           : (seven ? btn_7_blue : btn_xp_blue);
    int n;
    if (kind == W2K_CAP_CLOSE)
        n = seven ? (int)(sizeof btn_7_close / sizeof *btn_7_close)
                  : (int)(sizeof btn_xp_close / sizeof *btn_xp_close);
    else
        n = seven ? (int)(sizeof btn_7_blue / sizeof *btn_7_blue)
                  : (int)(sizeof btn_xp_blue / sizeof *btn_xp_blue);
    int *ins = corner_insets(h, S(3), 1);
    grad_fill(d, x, y, w, h, st, n, ins, active ? 256 : 200);
    free(ins);

    unsigned long edge;
    if (seven) edge = kind == W2K_CAP_CLOSE ? w2k_rgb(160, 30, 24)
                                            : w2k_rgb(140, 152, 166);
    else       edge = kind == W2K_CAP_CLOSE ? w2k_rgb(255, 236, 230)
                                            : w2k_rgb(160, 195, 252);
    XSetForeground(w2k.dpy, w2k.gc, edge);
    int *eins = corner_insets(h, S(3), 1);
    int t = S(1);
    for (int i = 0; i < h; i++) {
        int off = eins ? eins[i] : 0;
        if (i == 0 || i == h - 1 || (eins && i > 0 && eins[i] != eins[i - 1]))
            w2k_fill_fg(d, x + off, y + i, w - 2 * off, 1);
        else {
            w2k_fill_fg(d, x + off, y + i, t, 1);
            w2k_fill_fg(d, x + w - t - off, y + i, t, 1);
        }
    }
    free(eins);

    int o = pressed ? S(1) : 0;
    int cx = x + w / 2 + o, cy = y + h / 2 + o;
    if (seven && kind != W2K_CAP_CLOSE)
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(45, 55, 70));
    else
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(255, 255, 255));
    switch (kind) {
    case W2K_CAP_CLOSE:
        for (int i = 0; i < 7; i++) {
            w2k_fill_fg(d, cx - S(4) + i * t, cy - S(3) + i * t, S(2), t);
            w2k_fill_fg(d, cx + S(3) - i * t, cy - S(3) + i * t, S(2), t);
        }
        break;
    case W2K_CAP_MIN:
        w2k_fill_fg(d, cx - S(3), cy + S(2), S(7), S(2));
        break;
    case W2K_CAP_MAX:
        w2k_fill_fg(d, cx - S(4), cy - S(4), S(9), S(2));
        w2k_fill_fg(d, cx - S(4), cy - S(4), S(1), S(8));
        w2k_fill_fg(d, cx + S(4), cy - S(4), S(1), S(8));
        w2k_fill_fg(d, cx - S(4), cy + S(3), S(9), S(1));
        break;
    default:
        w2k_fill_fg(d, cx - S(1), cy - S(5), S(7), S(2));
        w2k_fill_fg(d, cx + S(5), cy - S(5), S(1), S(5));
        w2k_fill_fg(d, cx - S(5), cy - S(1), S(7), S(2));
        w2k_fill_fg(d, cx - S(5), cy - S(1), S(1), S(6));
        w2k_fill_fg(d, cx - S(5), cy + S(4), S(7), S(1));
        w2k_fill_fg(d, cx + S(1), cy - S(1), S(1), S(6));
        break;
    }
}

/* ------------------------------------------------------------------ *
 * The taskbar and its buttons
 * ------------------------------------------------------------------ */
int w2k_theme_task_h(int theme)
{
    /* XP's task buttons: rows 573..597 of a 570..599 bar. Windows 7's
     * fill the bar, top line and all: forty rows, or thirty with small
     * icons. */
    if (theme == THEME_MODERN) return 22;      /* the classic bar's height */
    if (theme == THEME_VISTA) return VISTA_BTN_H;
    return W2K_THEME_IS7(theme) ? (w2k_taskbar_small ? 30 : W7_BAR_H) : 25;
}

void w2k_theme_taskbutton(Drawable d, int x, int y, int w, int h, int state,
                          int theme)
{
    if (theme == THEME_VISTA) {
        /* Cropped from the reference: a glassy dark box, 27 rows with the
         * light line under it, eight-pixel caps. The pointer lightens
         * it and the active window's is lighter still. */
        W2kSkin *s = skin("vista-task.png", state == W2K_TB_DOWN ? 300 :
                                            state == W2K_TB_HOT ? 272 : 256);
        if (s && w2k_skin_w(s) == 17 && w2k_skin_h(s) == VISTA_BTN_H) {
            strip_draw(d, s, 0, VISTA_BTN_H, 8, 8, x, y, w, h);
            return;
        }
        /* Without the skin: the box drawn in its measured greys. */
        int top = state == W2K_TB_NORMAL ? 217 : 235;
        for (int i = 1; i < h - 1; i++) {
            int g = i < h / 2 ? top - (top - 134) * i / (h / 2) : i < h * 3 / 4 ? 60 : 24;
            w2k_fill_rgb(d, x + 1, y + i, w - 2, 1, g, g, g);
        }
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(55, 55, 55));
        w2k_frame_fg(d, x, y, w, h - 1);
        w2k_fill_rgb(d, x + 1, y + h - 1, w - 2, 1, 82, 82, 82);
        return;
    }
    if (theme == THEME_MODERN) {
        /* Every running window has a rounded box, as the classic bar
         * gives it a button; the pointer lightens it, and the active
         * window's is pressed in and marked with an accent line. */
        int f[3], l[3], a[3];
        w2k_modern_rgb(MODERN_ACCENT, a);
        w2k_modern_rgb(MODERN_BORDER, l);
        w2k_modern_rgb(state == W2K_TB_DOWN ? MODERN_PRESSED :
                       state == W2K_TB_HOT  ? MODERN_HOT : MODERN_BUTTON, f);
        w2k_round_rect_rgb(d, x, y, w, h, 4, f, l);
        if (state == W2K_TB_DOWN) {
            int lw = S(16), lh = S(2);
            if (lw > w - S(8)) lw = w - S(8);
            w2k_round_fill_rgb(d, x + (w - lw) / 2, y + h - lh - S(2), lw, lh, 1,
                               a[0], a[1], a[2]);
        }
        return;
    }
    if (W2K_THEME_IS7(theme)) {
        /* The active window's button, read row by row off a full-size
         * capture: it spans the bar's whole height, its top border on the
         * bar's own dark line; a near-black top edge, slate sides, a
         * white line inside the top and left, a lighter one inside the
         * right and bottom, and a glassy fill that falls from a pale
         * blue-grey to its darkest a little past the middle and lifts
         * again toward the bottom. The pointer's button is the same
         * faded most of the way in; a window's that is merely running,
         * a little under half. */
        static const struct { int at, r, g, b; } stops[] = {
            {    0, 212, 218, 226 }, {  550, 175, 187, 201 }, { 1000, 196, 205, 215 },
        };
        const int body[3] = { 129, 148, 170 };
        int k = state == W2K_TB_DOWN ? 256 : state == W2K_TB_HOT ? 205 : 115;
#define MIX(c, b) ((b) + ((c) - (b)) * k / 256)
        if (w < 4 || h < 5) return;
        for (int i = 2; i < h - 2; i++) {
            int at = (i - 2) * 1000 / (h - 5);
            int j = at < 550 ? 0 : 1;
            int span = stops[j + 1].at - stops[j].at, t = (at - stops[j].at) * 256 / (span ? span : 1);
            int r = stops[j].r + (stops[j + 1].r - stops[j].r) * t / 256;
            int g = stops[j].g + (stops[j + 1].g - stops[j].g) * t / 256;
            int b = stops[j].b + (stops[j + 1].b - stops[j].b) * t / 256;
            w2k_fill_rgb(d, x + 2, y + i, w - 4, 1, MIX(r, body[0]), MIX(g, body[1]), MIX(b, body[2]));
        }
        w2k_fill_rgb(d, x + 1, y + 1, w - 2, 1, MIX(252, body[0]), MIX(253, body[1]), MIX(254, body[2]));
        w2k_fill_rgb(d, x + 1, y + h - 2, w - 2, 1, MIX(243, body[0]), MIX(245, body[1]), MIX(247, body[2]));
        w2k_fill_rgb(d, x + 1, y + 2, 1, h - 4, MIX(246, body[0]), MIX(247, body[1]), MIX(249, body[2]));
        w2k_fill_rgb(d, x + w - 2, y + 2, 1, h - 4, MIX(234, body[0]), MIX(237, body[1]), MIX(241, body[2]));
        w2k_fill_rgb(d, x + 1, y, w - 2, 1, MIX(25, body[0]), MIX(29, body[1]), MIX(33, body[2]));
        w2k_fill_rgb(d, x + 1, y + h - 1, w - 2, 1, MIX(52, body[0]), MIX(59, body[1]), MIX(68, body[2]));
        w2k_fill_rgb(d, x, y + 1, 1, h - 2, MIX(58, body[0]), MIX(67, body[1]), MIX(77, body[2]));
        w2k_fill_rgb(d, x + w - 1, y + 1, 1, h - 2, MIX(71, body[0]), MIX(81, body[1]), MIX(93, body[2]));
        int cr = (MIX(64, body[0]) + body[0]) / 2, cg = (MIX(74, body[1]) + body[1]) / 2, cb = (MIX(85, body[2]) + body[2]) / 2;
        w2k_fill_rgb(d, x, y, 1, 1, cr, cg, cb);
        w2k_fill_rgb(d, x + w - 1, y, 1, 1, cr, cg, cb);
        w2k_fill_rgb(d, x, y + h - 1, 1, 1, cr, cg, cb);
        w2k_fill_rgb(d, x + w - 1, y + h - 1, 1, 1, cr, cg, cb);
#undef MIX
        return;
    }
    if (theme == THEME_XP) {
        /* The pressed button is cropped from a screenshot too: the active
         * window's button in it is drawn that way. Hot is the normal one
         * lightened -- no screenshot shows the pointer over a button. */
        W2kSkin *s = state == W2K_TB_DOWN ? skin("xp-task-down.png", 256)
                   : skin("xp-task.png", state == W2K_TB_HOT ? 292 : 256);
        if (s && w2k_skin_w(s) == 11 && w2k_skin_h(s) == 25) {
            strip_draw(d, s, 0, 25, 5, 5, x, y, w, h);
            return;
        }
    }
    const Stop *st = W2K_THEME_IS7(theme) ? task_7 : task_xp;
    int n = W2K_THEME_IS7(theme) ? (int)(sizeof task_7 / sizeof *task_7)
                                  : (int)(sizeof task_xp / sizeof *task_xp);
    int scale = state == W2K_TB_DOWN ? 200 : state == W2K_TB_HOT ? 292 : 256;
    int *ins = corner_insets(h, S(3), 1);
    grad_fill(d, x, y, w, h, st, n, ins, scale);
    free(ins);
}

/* The XP notification area is a *lighter* blue than the bar, with a
 * bright line along its top and a dark-then-light divider at its left.
 * Read off a 1:1 screenshot: (86,172,247) on top, (64,138,227) through
 * the body, a little lighter just above a darker bottom row; the divider
 * (35,74,167) then (85,166,229). */
static const Stop tray_xp[] = {
    {    0,  86, 172, 247 }, {   40,  72, 152, 233 }, {  100,  64, 140, 228 },
    {  850,  65, 139, 228 }, {  920,  69, 147, 232 }, { 1000,  60, 124, 221 },
};

/* Windows 7 Basic's notification area is darker than the bar and fades
 * into it over thirty-odd pixels at its left: a dark line on top, a
 * light one under it, then an even body that lightens a touch towards
 * the bottom. Read off a screenshot, like the sliver. */
static const Stop tray_7[] = {
    {    0,  83, 105, 142 }, {   45, 148, 161, 178 }, {   90, 133, 146, 162 },
    {  900, 134, 146, 166 }, { 1000, 141, 150, 159 },
};

void w2k_theme_tray(Drawable d, int x, int y, int w, int h, int theme)
{
    if (W2K_THEME_IS7(theme)) {
        /* Windows 7's notification area is the bar itself: nothing to
         * paint. (The darker well below is Vista's, kept for it.) */
        if (theme == THEME_BASIC7) return;
        if (w <= 0 || h <= 0) return;
        int fade = 32;
        int n = (int)(sizeof tray_7 / sizeof *tray_7);
        if (w > fade) grad_fill(d, x + fade, y, w - fade, h, tray_7, n, NULL, 256);
        /* The fade: each column a blend of the bar's flat colour and the
         * tray's, row by row. */
        int fw = w < fade ? w : fade;
        for (int i = 0; i < fw; i++) {
            int t = (i + 1) * 255 / (fade + 1);        /* 0 at the bar, 255 at the tray */
            int ph = w2k_cw(y, h), py = w2k_cx(y), px = w2k_cx(x + i);
            int pw = w2k_cw(x + i, 1);
            for (int r = 0; r < ph; r++) {
                int at = ph > 1 ? r * 1000 / (ph - 1) : 0;
                int tr, tg, tb;
                stop_rgb(tray_7, n, at, &tr, &tg, &tb);
                int br = 167, bg = 192, bb = 220;
                if (r == 0) { br = 74; bg = 107; bb = 142; }
                else if (r == 1) { br = 180; bg = 196; bb = 219; }
                XSetForeground(w2k.dpy, w2k.gc,
                               w2k_rgb(br + (tr - br) * t / 255, bg + (tg - bg) * t / 255,
                                       bb + (tb - bb) * t / 255));
                XFillRectangle(w2k.dpy, d, w2k.gc, px, py + r, (unsigned)pw, 1);
            }
        }
        return;
    }
    if (theme != THEME_XP || w <= 2 || h <= 0) return;
    grad_fill(d, x + 2, y, w - 2, h, tray_xp, (int)(sizeof tray_xp / sizeof *tray_xp), NULL, 256);
    w2k_fill_rgb(d, x, y, 1, h, 35, 74, 167);
    w2k_fill_rgb(d, x + 1, y, 1, h, 85, 166, 229);
}

void w2k_theme_bar(Drawable d, int x, int y, int w, int h, int theme)
{
    if (theme == THEME_VISTA) {
        /* Every row of the reference bar: a dark line, a white one, a
         * grey gradient down to the flat dark band at the bottom. A bar
         * of another height stretches the table. */
        static const unsigned char rows[VISTA_BAR_H] = {
            23, 251, 185, 181, 174, 168, 161, 153, 146, 138, 130, 121, 113, 104,
            96, 88, 80, 72, 66, 60, 54, 49, 24, 24, 24, 24, 24, 24, 24, 24 };
        for (int i = 0; i < h; i++) {
            int g = rows[i * VISTA_BAR_H / h];
            w2k_fill_rgb(d, x, y + i, w, 1, g, g, g);
        }
        return;
    }
    if (theme == THEME_MODERN) {
        /* The face, with the outline along its top. */
        int t = w2k_scale_raw ? w2k_th(1) : 1;
        w2k_fill(d, x, y, w, h, C_FACE);
        w2k_fill(d, x, y, w, t, C_WINDOWFRAME);
        return;
    }
    if (W2K_THEME_IS7(theme)) {
        /* Read off full-size captures of the real bar, at rest: one flat
         * colour, (129,148,170), under a dark line and a light one. The
         * Show Desktop sliver at the far end is fifteen columns: a dark
         * divider, twelve of a darker fill, a lighter column, a dark
         * edge. */
        w2k_fill_rgb(d, x, y, w, h, 129, 148, 170);
        w2k_fill_rgb(d, x, y, w, 1, 67, 77, 88);
        w2k_fill_rgb(d, x, y + 1, w, 1, 202, 217, 234);
        if (w >= 15) {
            w2k_fill_rgb(d, x + w - 15, y, 1, h, 72, 83, 95);
            w2k_fill_rgb(d, x + w - 14, y, 12, h, 92, 106, 121);
            w2k_fill_rgb(d, x + w - 2, y, 1, h, 112, 124, 138);
            w2k_fill_rgb(d, x + w - 1, y, 1, h, 75, 86, 99);
        }
        return;
    }
    if (theme == THEME_XP) {
        W2kSkin *s = skin("xp-taskbar.png", 256);
        if (s && w2k_skin_w(s) == 1) {
            int sh = w2k_skin_h(s);
            if (h == sh) { w2k_skin_tile(d, s, x, y, w, h, 0, 0, 1, sh); return; }
            for (int row = 0; row < h; row++) {
                int srow = row * sh / (h > 1 ? h : 1);
                if (srow >= sh) srow = sh - 1;
                w2k_skin_tile(d, s, x, y + row, w, 1, 0, srow, 1, 1);
            }
            return;
        }
    }
    w2k_bar_gradient(d, x, y, w, h, theme);
}

/* account.c -- the user as the Start menu shows them: a display name and a
 * picture, chosen in Control Panel > User Accounts and kept in
 * ~/.w2k/account (Name=, Picture=). Without the file the name is the
 * passwd entry's full name, or failing that the login name, and the
 * picture is the shell's own tile. The logon name itself is not touched:
 * "Log Off jack..." stays what the system calls you. */
#include "w2k.h"
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char acc_name[128], acc_pic[1024];
static int  loaded;

static Pixmap pic_pm;
static int    pic_size;
static char   pic_path[1024];

static void pic_drop(void)
{
    if (pic_pm && w2k.dpy) XFreePixmap(w2k.dpy, pic_pm);
    pic_pm = 0;
    pic_path[0] = 0;
}

static void path_of(char *buf, size_t n)
{
    const char *h = getenv("HOME");
    snprintf(buf, n, "%s/.w2k/account", h ? h : ".");
}

static void load(void)
{
    loaded = 1;
    acc_name[0] = acc_pic[0] = 0;
    char p[1100];
    path_of(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "Name=", 5))
            snprintf(acc_name, sizeof acc_name, "%s", line + 5);
        else if (!strncmp(line, "Picture=", 8))
            snprintf(acc_pic, sizeof acc_pic, "%s", line + 8);
    }
    fclose(f);
}

void w2k_account_reload(void)
{
    loaded = 0;
    pic_drop();
}

/* The passwd entry's idea of the name: the first GECOS field with its
 * first letter up, or the login name. */
const char *w2k_account_default_name(void)
{
    static char name[128];
    if (name[0]) return name;
    struct passwd *pw = getpwuid(getuid());
    const char *n = pw && pw->pw_gecos && pw->pw_gecos[0] ? pw->pw_gecos
                  : pw && pw->pw_name ? pw->pw_name : "User";
    snprintf(name, sizeof name, "%.127s", n);
    char *comma = strchr(name, ',');
    if (comma) *comma = 0;
    if (name[0] >= 'a' && name[0] <= 'z') name[0] -= 32;
    return name;
}

const char *w2k_account_name(void)
{
    if (!loaded) load();
    return acc_name[0] ? acc_name : w2k_account_default_name();
}

const char *w2k_account_picture(void)
{
    if (!loaded) load();
    return acc_pic;
}

/* Show another picture for now, without saving: the applet's preview.
 * Only this process sees it; the desktop keeps what is on disk. */
void w2k_account_preview(const char *picture)
{
    if (!loaded) load();
    if (!strcmp(acc_pic, picture ? picture : "")) return;
    snprintf(acc_pic, sizeof acc_pic, "%s", picture ? picture : "");
    pic_drop();
}

int w2k_account_save(const char *name, const char *picture)
{
    char p[1100], dir[1100];
    path_of(p, sizeof p);
    snprintf(dir, sizeof dir, "%.*s", (int)(strlen(p) - strlen("/account")), p);
    mkdir(dir, 0755);
    FILE *f = fopen(p, "w");
    if (!f) return -1;
    fprintf(f, "# Linux 2000 -- the user as the Start menu shows them\n");
    fprintf(f, "Name=%s\nPicture=%s\n", name ? name : "", picture ? picture : "");
    fclose(f);
    w2k_account_reload();
    return 0;
}

/* The picture as a square `size` pixels across: cropped to its middle,
 * resampled, and laid over white (a photograph is opaque; what
 * transparency a PNG has reads as paper). Kept until the path or the
 * size changes. 0 when there is no picture or it cannot be read. */
static Pixmap picture_pixmap(int size)
{
    const char *path = w2k_account_picture();
    if (!path[0] || size < 1 || !w2k.dpy) return 0;
    if (pic_pm && pic_size == size && !strcmp(pic_path, path)) return pic_pm;
    pic_drop();
    int w = 0, h = 0;
    unsigned char *rgba = w2k_image_load(path, &w, &h);
    if (!rgba || w < 1 || h < 1) { free(rgba); return 0; }
    int side = w < h ? w : h, ox = (w - side) / 2, oy = (h - side) / 2;
    unsigned char *sq = malloc((size_t)side * side * 4);
    if (!sq) { free(rgba); return 0; }
    for (int y = 0; y < side; y++)
        memcpy(sq + (size_t)y * side * 4, rgba + ((size_t)(oy + y) * w + ox) * 4,
               (size_t)side * 4);
    free(rgba);
    unsigned char *sc = side == size ? sq
                      : w2k_rgba_resample(sq, side, side, size, size, RS_CUBIC);
    if (sc != sq) free(sq);
    if (!sc) return 0;
    pic_pm = XCreatePixmap(w2k.dpy, w2k.root, (unsigned)size, (unsigned)size, w2k.depth);
    char *pixels = malloc((size_t)size * size * 4);
    XImage *im = pixels ? XCreateImage(w2k.dpy, w2k.visual, w2k.depth, ZPixmap, 0,
                                       pixels, (unsigned)size, (unsigned)size, 32, 0)
                        : NULL;
    if (im) {
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                const unsigned char *p = sc + ((size_t)y * size + x) * 4;
                int a = p[3];
                XPutPixel(im, x, y, w2k_rgb((p[0] * a + 255 * (255 - a)) / 255,
                                            (p[1] * a + 255 * (255 - a)) / 255,
                                            (p[2] * a + 255 * (255 - a)) / 255));
            }
        XPutImage(w2k.dpy, pic_pm, w2k.gc, im, 0, 0, 0, 0, (unsigned)size, (unsigned)size);
        XDestroyImage(im);
    } else {
        free(pixels);
    }
    free(sc);
    pic_size = size;
    snprintf(pic_path, sizeof pic_path, "%s", path);
    return pic_pm;
}

/* Draw the picture `size` across at (x, y), or `fallback` -- one of the
 * shell's icons, 32 pixels, centred in the square -- when there is none.
 * Coordinates are the caller's; the picture is built at the size it
 * takes on the screen. */
void w2k_account_picture_draw(Drawable d, int x, int y, int size, int fallback)
{
    int px = w2k_cx(x), py = w2k_cx(y), ps = w2k_cw(x, size);
    Pixmap pm = picture_pixmap(ps);
    if (pm) {
        XCopyArea(w2k.dpy, pm, d, w2k.gc, 0, 0, (unsigned)ps, (unsigned)ps, px, py);
        return;
    }
    if (fallback >= 0)
        w2k_bigicon_draw(d, x + (size - 32) / 2, y + (size - 32) / 2, fallback);
}

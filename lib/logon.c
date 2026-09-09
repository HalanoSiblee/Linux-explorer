/* logon.c -- how the logon screen looks: ~/.w2k/logon.
 *
 * Control Panel > Logon Screen writes it for the user running it; the
 * logon screen (l2kdm, which runs as root before anyone is logged on)
 * reads it from the home of the last user who logged on, so the screen
 * takes the look of whoever last set it. Keys:
 *
 *   Art=windows|linux2000|distro    the banner's artwork
 *   Background=r g b                the colour behind the dialog
 *   Wallpaper=/path                 a picture over it, stretched (or empty)
 *   ShowPicture=0|1                 the user's picture on the dialog
 */
#include "w2k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

void w2k_logon_defaults(W2kLogonCfg *c)
{
    memset(c, 0, sizeof *c);
    c->art = LOGON_ART_WINDOWS;
    c->bg[0] = 58; c->bg[1] = 110; c->bg[2] = 165;
    c->show_picture = 1;
}

int w2k_logon_load(W2kLogonCfg *c, const char *home)
{
    w2k_logon_defaults(c);
    if (!home || !*home) home = getenv("HOME");
    if (!home) return 0;
    char p[1100];
    snprintf(p, sizeof p, "%s/.w2k/logon", home);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        const char *v = eq + 1;
        if (!strcasecmp(line, "Art"))
            c->art = !strcasecmp(v, "linux2000") ? LOGON_ART_LINUX2000 :
                     !strcasecmp(v, "distro")    ? LOGON_ART_DISTRO : LOGON_ART_WINDOWS;
        else if (!strcasecmp(line, "Background")) {
            int r, g, b;
            if (sscanf(v, "%d %d %d", &r, &g, &b) == 3) {
                c->bg[0] = r < 0 ? 0 : r > 255 ? 255 : r;
                c->bg[1] = g < 0 ? 0 : g > 255 ? 255 : g;
                c->bg[2] = b < 0 ? 0 : b > 255 ? 255 : b;
            }
        } else if (!strcasecmp(line, "Wallpaper"))
            snprintf(c->wallpaper, sizeof c->wallpaper, "%s", v);
        else if (!strcasecmp(line, "ShowPicture"))
            c->show_picture = atoi(v) != 0;
    }
    fclose(f);
    return 1;
}

int w2k_logon_save(const W2kLogonCfg *c)
{
    const char *home = getenv("HOME");
    if (!home) return -1;
    char dir[1100], p[1200];
    snprintf(dir, sizeof dir, "%s/.w2k", home);
    mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/logon", dir);
    FILE *f = fopen(p, "w");
    if (!f) return -1;
    fprintf(f, "# Linux 2000 -- the logon screen's look (read from the last user's home)\n");
    fprintf(f, "Art=%s\n", c->art == LOGON_ART_LINUX2000 ? "linux2000" :
                          c->art == LOGON_ART_DISTRO ? "distro" : "windows");
    fprintf(f, "Background=%d %d %d\n", c->bg[0], c->bg[1], c->bg[2]);
    fprintf(f, "Wallpaper=%s\n", c->wallpaper);
    fprintf(f, "ShowPicture=%d\n", c->show_picture ? 1 : 0);
    fclose(f);
    return 0;
}

/* The distribution's name as /etc/os-release gives it: "Debian GNU/Linux
 * 13 (trixie)"; "Linux" when there is no such file. */
void w2k_distro_pretty_name(char *buf, int n)
{
    snprintf(buf, (size_t)n, "Linux");
    FILE *f = fopen("/etc/os-release", "r");
    if (!f) f = fopen("/usr/lib/os-release", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "PRETTY_NAME=", 12)) continue;
        char *v = line + 12;
        if (*v == '"') { v++; char *q = strchr(v, '"'); if (q) *q = 0; }
        snprintf(buf, (size_t)n, "%s", v);
        break;
    }
    fclose(f);
}

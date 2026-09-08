/* wine.c -- Windows programs, through Wine.
 *
 * The desktop treats a Windows program as a program: an .exe, .msi, .lnk
 * or .bat opens with `wine start /unix`, which hands it to whatever the
 * prefix associates it with (an installer runs, a shortcut is followed);
 * an .exe shows its own icon in Explorer when icoutils' wrestool is
 * there to pull it out; and what Wine installs turns up in the Start
 * menu's Windows Programs group (see wm/programs.c). The prefix is
 * Wine's own (WINEPREFIX, or ~/.wine), made by l2k-session the first
 * time it is missing. */
#include "w2k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

static int tool_present(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) return 0;
    char copy[4096], full[4200], *save = NULL;
    snprintf(copy, sizeof copy, "%s", path);
    for (char *d = strtok_r(copy, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        snprintf(full, sizeof full, "%s/%s", d, name);
        if (access(full, X_OK) == 0) return 1;
    }
    return 0;
}

int w2k_wine_available(void)
{
    static int have = -1;
    if (have < 0) have = tool_present("wine");
    return have;
}

void w2k_wine_prefix(char *buf, int n)
{
    const char *p = getenv("WINEPREFIX");
    if (p && *p) { snprintf(buf, (size_t)n, "%s", p); return; }
    const char *h = getenv("HOME");
    snprintf(buf, (size_t)n, "%s/.wine", h ? h : ".");
}

/* Is the name a Windows program, installer, shortcut or batch file? */
int w2k_wine_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    static const char *const exts[] = { ".exe", ".msi", ".lnk", ".bat", ".com", ".cmd" };
    for (size_t i = 0; i < sizeof exts / sizeof *exts; i++)
        if (!strcasecmp(dot, exts[i])) return 1;
    return 0;
}

/* The icon inside an .exe, pulled out with wrestool into ~/.w2k/cache and
 * registered once; ICO_APP without wrestool, or when there is none. Kept
 * by path and modification time, so a replaced program shows its new one. */
int w2k_wine_exe_icon(const char *path)
{
    static int have_wrestool = -1;
    if (have_wrestool < 0) have_wrestool = tool_present("wrestool");
    if (!have_wrestool) return ICO_APP;
    struct stat st;
    if (stat(path, &st) != 0) return ICO_APP;

    static struct { char *path; long mtime; int id; } seen[256];
    static int nseen, next;
    for (int i = 0; i < nseen; i++)
        if (!strcmp(seen[i].path, path) && seen[i].mtime == (long)st.st_mtime) return seen[i].id;

    /* A stable name for the cache file: the path and time, hashed. */
    unsigned long h = 5381;
    for (const char *p = path; *p; p++) h = h * 33 + (unsigned char)*p;
    h = h * 33 + (unsigned long)st.st_mtime;
    const char *home = getenv("HOME");
    char dir[1100], ico[1200];
    snprintf(dir, sizeof dir, "%s/.w2k/cache/exe-icons", home ? home : ".");
    snprintf(ico, sizeof ico, "%s/%lx.ico", dir, h);
    int id = -1;
    if (access(ico, R_OK) != 0) {
        char mk[1200];
        snprintf(mk, sizeof mk, "%s/.w2k/cache", home ? home : ".");
        mkdir(mk, 0755);
        mkdir(dir, 0755);
        /* wrestool writes one file per icon group when told a directory;
         * the first group is the program's own icon. */
        char tmp[1300];
        snprintf(tmp, sizeof tmp, "%s/%lx.d", dir, h);
        mkdir(tmp, 0755);
        char q[4200], cmd[6000];
        w2k_shell_quote(path, q, sizeof q);
        snprintf(cmd, sizeof cmd, "wrestool -x -t 14 -o %s %s >/dev/null 2>&1", tmp, q);
        if (system(cmd) == 0) {
            DIR *dp = opendir(tmp);
            char best[1400] = "";
            if (dp) {
                struct dirent *e;
                while ((e = readdir(dp))) {
                    size_t n = strlen(e->d_name);
                    if (n < 5 || strcasecmp(e->d_name + n - 4, ".ico")) continue;
                    if (!best[0] || strcmp(e->d_name, best + strlen(tmp) + 1) < 0)
                        snprintf(best, sizeof best, "%s/%s", tmp, e->d_name);
                }
                closedir(dp);
            }
            if (best[0]) rename(best, ico);
        }
        /* Whatever else came out is not needed. */
        char rm[1400];
        snprintf(rm, sizeof rm, "rm -rf %s", tmp);
        if (system(rm) != 0) { }
    }
    if (access(ico, R_OK) == 0) id = w2k_icon_from_file(ico);
    if (id < 0) id = ICO_APP;
    int slot = nseen < 256 ? nseen++ : next++ % 256;
    free(seen[slot].path);
    seen[slot].path = w2k_strdup(path);
    seen[slot].mtime = (long)st.st_mtime;
    seen[slot].id = id;
    return id;
}

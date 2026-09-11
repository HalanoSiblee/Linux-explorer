/* gpu.c -- which graphics processor the desktop's programs render on.
 *
 * A laptop with two GPUs shows the screen through one (the integrated
 * one, which the firmware booted on) and leaves the other asleep until a
 * program asks for it by name. Power Options chooses; the choice is the
 * scheme's Graphics= (a PCI address, empty for the default one) and every
 * shell process puts it in its own environment whenever it loads the
 * scheme, so whatever the taskbar, the Start menu, Run or Explorer starts
 * afterwards inherits it: DRI_PRIME for Mesa's drivers (nouveau, amdgpu,
 * i915 -- OpenGL and Vulkan both honour it), NVIDIA's render offload
 * variables for the proprietary driver. */
#include "w2k.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

char w2k_gpu_pref[16];

static int read_line(const char *path, char *out, int n)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(out, n, f)) { fclose(f); out[0] = 0; return 0; }
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
    return 1;
}

/* "Intel Corporation" + "Kaby Lake-U GT2 [HD Graphics 620]" -> "Intel HD
 * Graphics 620": the marketing name in the brackets when there is one,
 * the vendor without its "Corporation". */
static void pci_name(unsigned v, unsigned d, char *out, int n)
{
    static const char *const dbs[] = {
        "/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids",
        "/usr/share/pci.ids", "/var/lib/pciutils/pci.ids", NULL
    };
    out[0] = 0;
    for (int i = 0; dbs[i] && !out[0]; i++) {
        FILE *f = fopen(dbs[i], "r");
        if (!f) continue;
        char line[512], vname[200] = "";
        int in_vendor = 0;
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#' || line[0] == '\n') continue;
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] != '\t') {
                if (in_vendor) break;             /* past our vendor's devices */
                in_vendor = strtoul(line, NULL, 16) == v && strlen(line) > 6;
                if (in_vendor) snprintf(vname, sizeof vname, "%s", line + 6);
                continue;
            }
            if (!in_vendor || line[1] == '\t' || strtoul(line + 1, NULL, 16) != d ||
                strlen(line) <= 7)
                continue;
            char dev[200];
            snprintf(dev, sizeof dev, "%s", line + 7);
            char *lb = strrchr(dev, '['), *rb = lb ? strchr(lb, ']') : NULL;
            if (lb && rb && rb > lb + 1) { *rb = 0; memmove(dev, lb + 1, strlen(lb + 1) + 1); }
            /* "Advanced Micro Devices, Inc. [AMD/ATI]" -> "AMD". */
            lb = strrchr(vname, '[');
            if (lb && (rb = strchr(lb, ']'))) {
                *rb = 0;
                memmove(vname, lb + 1, strlen(lb + 1) + 1);
                char *slash = strchr(vname, '/');
                if (slash) *slash = 0;
            }
            char *corp = strstr(vname, " Corporation");
            if (!corp) corp = strstr(vname, " Corp.");
            if (corp) *corp = 0;
            /* Some device names already carry the vendor ("NVIDIA ..."). */
            if (!strncasecmp(dev, vname, strlen(vname))) snprintf(out, (size_t)n, "%s", dev);
            else snprintf(out, (size_t)n, "%s %s", vname, dev);
            break;
        }
        fclose(f);
    }
}

int w2k_gpus(W2kGpu *out, int max, int with_names)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < max) {
        if (de->d_name[0] == '.') continue;
        char dir[300], path[400], buf[64];
        snprintf(dir, sizeof dir, "/sys/bus/pci/devices/%.200s", de->d_name);
        snprintf(path, sizeof path, "%s/class", dir);
        /* Class 03: VGA, XGA, 3D and other display controllers. */
        if (!read_line(path, buf, sizeof buf) || strncmp(buf, "0x03", 4)) continue;
        W2kGpu *g = &out[n];
        memset(g, 0, sizeof *g);
        snprintf(g->addr, sizeof g->addr, "%.15s", de->d_name);
        snprintf(path, sizeof path, "%s/boot_vga", dir);
        g->primary = read_line(path, buf, sizeof buf) && buf[0] == '1';
        char link[400];
        snprintf(path, sizeof path, "%s/driver", dir);
        ssize_t l = readlink(path, link, sizeof link - 1);
        if (l > 0) {
            link[l] = 0;
            const char *slash = strrchr(link, '/');
            snprintf(g->driver, sizeof g->driver, "%s", slash ? slash + 1 : link);
        }
        if (with_names) {
            char vid[16] = "", did[16] = "";
            snprintf(path, sizeof path, "%s/vendor", dir); read_line(path, vid, sizeof vid);
            snprintf(path, sizeof path, "%s/device", dir); read_line(path, did, sizeof did);
            pci_name((unsigned)strtoul(vid, NULL, 16), (unsigned)strtoul(did, NULL, 16),
                     g->name, sizeof g->name);
            if (!g->name[0])
                snprintf(g->name, sizeof g->name, "Display adapter [%s:%s]",
                         vid[0] ? vid + 2 : "????", did[0] ? did + 2 : "????");
        }
        n++;
    }
    closedir(d);
    /* The default one first, the rest in address order. */
    for (int i = 1; i < n; i++) {
        W2kGpu t = out[i];
        int j = i - 1;
        while (j >= 0 && (t.primary > out[j].primary ||
                          (t.primary == out[j].primary && strcmp(out[j].addr, t.addr) > 0))) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = t;
    }
    return n;
}

static const char *const gpu_vars[] = {
    "DRI_PRIME", "__NV_PRIME_RENDER_OFFLOAD", "__GLX_VENDOR_LIBRARY_NAME",
    "__VK_LAYER_NV_optimus", NULL
};

void w2k_gpu_env_apply(void)
{
    /* Take back only what was put there last time (W2K_GPU marks it): a
     * DRI_PRIME exported from the user's own profile stays theirs. */
    if (getenv("W2K_GPU")) {
        for (int i = 0; gpu_vars[i]; i++) unsetenv(gpu_vars[i]);
        unsetenv("W2K_GPU");
    }
    if (!w2k_gpu_pref[0]) return;
    W2kGpu g[W2K_GPU_MAX];
    int n = w2k_gpus(g, W2K_GPU_MAX, 0);
    for (int i = 0; i < n; i++) {
        /* The default one needs nothing; a card without a driver, or one
         * that has gone (an external GPU unplugged), leaves the default. */
        if (strcmp(g[i].addr, w2k_gpu_pref) || g[i].primary || !g[i].driver[0]) continue;
        if (!strcmp(g[i].driver, "nvidia")) {
            setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
            setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1);
            setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 1);
        } else {
            /* Mesa's name for a card by its place on the bus. */
            char tag[32];
            snprintf(tag, sizeof tag, "pci-%s", g[i].addr);
            for (char *p = tag + 4; *p; p++) if (*p == ':' || *p == '.') *p = '_';
            setenv("DRI_PRIME", tag, 1);
        }
        setenv("W2K_GPU", g[i].addr, 1);
        return;
    }
}

/* l2kdiskmgmt.c -- Disk Management, the Windows 2000 snap-in: every disk
 * as a row of partitions drawn to scale, the volume list above it, and
 * the things one does to them: create and delete partitions, format them,
 * mount them somewhere, mark one active, initialise a blank disk.
 *
 * The picture comes from lsblk (util-linux), which needs no privileges;
 * the changes go through sfdisk, mkfs and mount under pkexec, one small
 * shell script per operation, with the Windows dialogs and warnings in
 * front of them. Nothing is done to a volume Linux is running from.
 *
 * W2K_FAKE_LSBLK=<file> reads a saved "lsblk -b -P" listing instead of the
 * real one, for pictures of disks one does not have. */
#define _POSIX_C_SOURCE 200809L
#include "w2kui.h"
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * What the machine has
 * ------------------------------------------------------------------ */
#define MAX_DISKS 16
#define MAX_PARTS 32
#define MAX_VOLS  96

typedef struct {
    char name[32], path[80], fstype[32], label[80], mount[256];
    char parttype[48], partlabel[80], flags[16];
    unsigned long long size, start;       /* bytes; sectors */
    unsigned long long avail;             /* bytes, or ~0ull when unknown */
    int number;                           /* the partition's number */
    int extended;                         /* an MBR extended container */
    int logical;                          /* lives inside one */
    int busy;                             /* holds LVM/LUKS, or is the root */
    int active;                           /* the boot flag */
} Part;

typedef struct {
    char name[32], path[80], model[80], vendor[32], tran[16], pttype[16];
    unsigned long long size;
    int rm, ro, rom, logsec;
    int index;                            /* Disk 0, 1... and CD-ROM 0, 1... apart */
    int wholefs;                          /* a file system straight on the disk */
    char fstype[32], label[80], mount[256];
    unsigned long long avail;
    Part part[MAX_PARTS];
    int nparts;
} Disk;

/* A volume in the top list: a partition, a whole-disk file system, or a
 * mapped device (LVM, LUKS, md) that has no place in the picture. */
typedef struct {
    int disk, part;                       /* -1 when it is a mapped device */
    char name[80], path[80], fstype[32], mount[256], type[32], layout[32];
    unsigned long long size, avail;
    int rom;
} Vol;

/* A stretch of a disk in the picture: a partition, or free space. */
typedef struct {
    int disk, part;                       /* part -1: free space */
    unsigned long long start, size;       /* bytes */
    int inext;                            /* inside the extended partition */
    int x, w;                             /* laid out */
} DRegion;

static Disk disks[MAX_DISKS];
static int ndisks;
static Vol vols[MAX_VOLS];
static int nvols;
static DRegion regions[MAX_DISKS * (MAX_PARTS + 2)];
static int nregions;

/* Children of partitions (LVM, LUKS) make them busy; recorded while
 * reading, matched afterwards. */
static char child_of[MAX_VOLS][32];
static int nchild;

static unsigned long long ull(const char *s) { return s && *s ? strtoull(s, NULL, 10) : 0; }

/* lsblk -P quotes values and escapes odd bytes as \xHH. */
static void unescape(char *s)
{
    char *o = s;
    for (const char *p = s; *p; p++) {
        if (p[0] == '\\' && p[1] == 'x' && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3])) {
            char h[3] = { p[2], p[3], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            p += 3;
        } else *o++ = *p;
    }
    *o = 0;
}

/* KEY="value" pairs out of one lsblk line. */
static int pair(const char *line, const char *key, char *out, int n)
{
    char pat[48];
    snprintf(pat, sizeof pat, "%s=\"", key);
    const char *p = line;
    size_t kl = strlen(pat);
    while ((p = strstr(p, pat))) {
        if (p == line || p[-1] == ' ') {
            p += kl;
            const char *e = strchr(p, '"');
            if (!e) break;
            int len = (int)(e - p);
            if (len >= n) len = n - 1;
            memcpy(out, p, (size_t)len);
            out[len] = 0;
            unescape(out);
            return 1;
        }
        p += kl;
    }
    out[0] = 0;
    return 0;
}

static int part_number(const char *name)
{
    int n = (int)strlen(name);
    while (n > 0 && isdigit((unsigned char)name[n - 1])) n--;
    return atoi(name + n);
}

static Disk *disk_by_name(const char *name)
{
    for (int i = 0; i < ndisks; i++) if (!strcmp(disks[i].name, name)) return &disks[i];
    return NULL;
}

static int is_extended_type(const char *t)
{
    return !strcasecmp(t, "0x5") || !strcasecmp(t, "0xf") || !strcasecmp(t, "0x85");
}

static void vol_add(int disk, int part, const char *name, const char *path,
                    const char *fstype, const char *mount, const char *type,
                    const char *layout, unsigned long long size,
                    unsigned long long avail, int rom)
{
    if (nvols >= MAX_VOLS) return;
    Vol *v = &vols[nvols++];
    memset(v, 0, sizeof *v);
    v->disk = disk; v->part = part; v->rom = rom;
    snprintf(v->name, sizeof v->name, "%s", name);
    snprintf(v->path, sizeof v->path, "%s", path);
    snprintf(v->fstype, sizeof v->fstype, "%s", fstype);
    snprintf(v->mount, sizeof v->mount, "%s", mount);
    snprintf(v->type, sizeof v->type, "%s", type);
    snprintf(v->layout, sizeof v->layout, "%s", layout);
    v->size = size; v->avail = avail;
}

static void scan(void)
{
    ndisks = nvols = nchild = 0;
    FILE *f;
    const char *fake = getenv("W2K_FAKE_LSBLK");
    if (fake && *fake) f = fopen(fake, "r");
    else f = popen("lsblk -b -P -o NAME,PATH,TYPE,SIZE,START,FSTYPE,LABEL,MOUNTPOINT,"
                   "PARTTYPE,PARTLABEL,PARTFLAGS,MODEL,RM,RO,TRAN,FSAVAIL,PTTYPE,PKNAME,"
                   "LOG-SEC,VENDOR 2>/dev/null", "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        char type[16], name[32], path[80], pk[32], fstype[32], mount[256];
        pair(line, "TYPE", type, sizeof type);
        pair(line, "NAME", name, sizeof name);
        pair(line, "PATH", path, sizeof path);
        pair(line, "PKNAME", pk, sizeof pk);
        pair(line, "FSTYPE", fstype, sizeof fstype);
        pair(line, "MOUNTPOINT", mount, sizeof mount);
        char sz[32], av[32];
        pair(line, "SIZE", sz, sizeof sz);
        int has_av = pair(line, "FSAVAIL", av, sizeof av) && av[0];
        if (!strcmp(type, "disk") || !strcmp(type, "rom")) {
            if (ndisks >= MAX_DISKS) continue;
            Disk *d = &disks[ndisks++];
            memset(d, 0, sizeof *d);
            snprintf(d->name, sizeof d->name, "%s", name);
            snprintf(d->path, sizeof d->path, "%s", path);
            pair(line, "MODEL", d->model, sizeof d->model);
            pair(line, "VENDOR", d->vendor, sizeof d->vendor);
            pair(line, "TRAN", d->tran, sizeof d->tran);
            pair(line, "PTTYPE", d->pttype, sizeof d->pttype);
            char v[16];
            pair(line, "RM", v, sizeof v); d->rm = atoi(v);
            pair(line, "RO", v, sizeof v); d->ro = atoi(v);
            pair(line, "LOG-SEC", v, sizeof v); d->logsec = atoi(v) > 0 ? atoi(v) : 512;
            d->rom = !strcmp(type, "rom");
            d->size = ull(sz);
            { int k = 0; for (int q = 0; q < ndisks - 1; q++) if (disks[q].rom == d->rom) k++; d->index = k; }
            /* Trailing blanks in MODEL/VENDOR. */
            for (char *s = d->model; *s; ) { size_t l = strlen(s); if (l && s[l - 1] == ' ') s[l - 1] = 0; else break; }
            for (char *s = d->vendor; *s; ) { size_t l = strlen(s); if (l && s[l - 1] == ' ') s[l - 1] = 0; else break; }
            if (fstype[0]) {
                d->wholefs = 1;
                snprintf(d->fstype, sizeof d->fstype, "%s", fstype);
                pair(line, "LABEL", d->label, sizeof d->label);
                snprintf(d->mount, sizeof d->mount, "%s", mount);
                d->avail = has_av ? ull(av) : ~0ull;
            }
        } else if (!strcmp(type, "part")) {
            Disk *d = disk_by_name(pk);
            if (!d || d->nparts >= MAX_PARTS) continue;
            Part *p = &d->part[d->nparts++];
            memset(p, 0, sizeof *p);
            snprintf(p->name, sizeof p->name, "%s", name);
            snprintf(p->path, sizeof p->path, "%s", path);
            snprintf(p->fstype, sizeof p->fstype, "%s", fstype);
            snprintf(p->mount, sizeof p->mount, "%s", mount);
            pair(line, "LABEL", p->label, sizeof p->label);
            pair(line, "PARTTYPE", p->parttype, sizeof p->parttype);
            pair(line, "PARTLABEL", p->partlabel, sizeof p->partlabel);
            pair(line, "PARTFLAGS", p->flags, sizeof p->flags);
            char st[32];
            pair(line, "START", st, sizeof st);
            p->start = ull(st);
            p->size = ull(sz);
            p->avail = has_av ? ull(av) : ~0ull;
            p->number = part_number(name);
            p->extended = is_extended_type(p->parttype);
            p->logical = !strcmp(d->pttype, "dos") && p->number >= 5;
            p->active = !strcmp(p->flags, "0x80");
            if (!strcmp(p->mount, "/") || !strcmp(p->mount, "/boot") ||
                !strcmp(p->mount, "/usr") || !strcmp(p->mount, "[SWAP]"))
                p->busy = 1;
        } else {
            /* crypt, lvm, md, ...: a volume with no place in the picture,
             * and whatever it sits on is spoken for. */
            if (pk[0] && nchild < MAX_VOLS) snprintf(child_of[nchild++], 32, "%s", pk);
            if (!strcmp(type, "loop")) continue;
            if (!fstype[0] && !mount[0]) continue;
            char label[80];
            pair(line, "LABEL", label, sizeof label);
            char nm[80];
            if (label[0]) snprintf(nm, sizeof nm, "%s", label);
            else snprintf(nm, sizeof nm, "%s", name);
            vol_add(-1, -1, nm, path, fstype, mount, "Dynamic",
                    !strcmp(type, "lvm") ? "Simple" : !strcmp(type, "crypt") ? "Encrypted" :
                    !strcmp(type, "md") ? "Mirror" : "Simple",
                    ull(sz), has_av ? ull(av) : ~0ull, 0);
        }
    }
    if (fake && *fake) fclose(f); else pclose(f);

    /* Partitions holding a mapped device are in use. */
    for (int i = 0; i < ndisks; i++)
        for (int j = 0; j < disks[i].nparts; j++)
            for (int k = 0; k < nchild; k++)
                if (!strcmp(child_of[k], disks[i].part[j].name)) disks[i].part[j].busy = 1;
}

/* ------------------------------------------------------------------ *
 * Words
 * ------------------------------------------------------------------ */
static const char *fmt_size(unsigned long long b, char *buf, int n)
{
    double v = (double)b;
    if (v >= 1024.0 * 1024 * 1024 * 1024) snprintf(buf, (size_t)n, "%.2f TB", v / (1024.0 * 1024 * 1024 * 1024));
    else if (v >= 1024.0 * 1024 * 1024) snprintf(buf, (size_t)n, "%.2f GB", v / (1024.0 * 1024 * 1024));
    else if (v >= 1024.0 * 1024) snprintf(buf, (size_t)n, "%.0f MB", v / (1024.0 * 1024));
    else if (v >= 1024.0) snprintf(buf, (size_t)n, "%.0f KB", v / 1024.0);
    else snprintf(buf, (size_t)n, "%llu bytes", b);
    return buf;
}

static const char *fs_name(const char *fstype)
{
    if (!fstype || !*fstype) return "RAW";
    if (!strcmp(fstype, "vfat")) return "FAT32";
    if (!strcmp(fstype, "ntfs")) return "NTFS";
    if (!strcmp(fstype, "exfat")) return "exFAT";
    if (!strcmp(fstype, "crypto_LUKS")) return "LUKS";
    if (!strcmp(fstype, "LVM2_member")) return "LVM";
    if (!strcmp(fstype, "swap")) return "Swap";
    if (!strcmp(fstype, "iso9660")) return "CDFS";
    if (!strcmp(fstype, "udf")) return "UDF";
    return fstype;
}

static int is_esp(const Part *p)
{
    return !strcasecmp(p->parttype, "c12a7328-f81f-11d2-ba4b-00a0c93ec93b") ||
           !strcasecmp(p->parttype, "0xef");
}

/* "Healthy (Boot)" and the rest, as the snap-in qualifies a volume. */
static const char *part_status(const Part *p, char *buf, int n)
{
    if (p->extended) return "Healthy (Extended Partition)";
    const char *q;
    if (!strcmp(p->mount, "/")) q = "Boot";
    else if (!strcmp(p->mount, "/boot") || !strcmp(p->mount, "/boot/efi") || is_esp(p))
        q = is_esp(p) ? "EFI System Partition" : "System";
    else if (!strcmp(p->fstype, "swap") || !strcmp(p->mount, "[SWAP]")) q = "Page File";
    else if (!strcmp(p->fstype, "crypto_LUKS")) q = "Encrypted";
    else if (!strcmp(p->fstype, "LVM2_member")) q = "LVM";
    else if (!strcasecmp(p->parttype, "e3c9e316-0b5c-4db8-817d-f92df00215ae")) q = "Reserved";
    else if (!strcasecmp(p->parttype, "de94bba4-06d1-4d40-a16a-bfd50179d6ac")) q = "Recovery Partition";
    else if (p->fstype[0]) q = p->logical ? "Logical Drive" : "Basic Data Partition";
    else q = p->logical ? "Logical Drive" : "Primary Partition";
    snprintf(buf, (size_t)n, "Healthy (%s)", q);
    return buf;
}

static void part_volname(const Disk *d, int di, const Part *p, char *buf, int n)
{
    (void)di;
    if (p->label[0]) snprintf(buf, (size_t)n, "%s", p->label);
    else if (p->partlabel[0] && strcasecmp(p->partlabel, "primary") &&
             strcasecmp(p->partlabel, "Basic data partition"))
        snprintf(buf, (size_t)n, "%s", p->partlabel);
    else snprintf(buf, (size_t)n, "(Disk %d partition %d)", d->index, p->number);
}

/* ------------------------------------------------------------------ *
 * The picture's regions
 * ------------------------------------------------------------------ */
#define MIN_GAP (4ull * 1024 * 1024)      /* alignment slack is not free space */

static void region_add(int disk, int part, unsigned long long start,
                       unsigned long long size, int inext)
{
    if (nregions >= (int)(sizeof regions / sizeof *regions)) return;
    DRegion *r = &regions[nregions++];
    r->disk = disk; r->part = part; r->start = start; r->size = size;
    r->inext = inext; r->x = r->w = 0;
}

static int cmp_start(const void *a, const void *b)
{
    const Part *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static void build_regions(void)
{
    nregions = 0;
    for (int i = 0; i < ndisks; i++) {
        Disk *d = &disks[i];
        if (d->rom) { region_add(i, d->size ? 0 : -2, 0, d->size, 0); continue; }
        if (d->wholefs) { region_add(i, 0, 0, d->size, 0); continue; }
        if (!d->nparts) { region_add(i, -1, 0, d->size, 0); continue; }
        qsort(d->part, (size_t)d->nparts, sizeof *d->part, cmp_start);
        unsigned long long sec = (unsigned long long)d->logsec;
        unsigned long long pos = MIN_GAP;      /* the first mebibyte is the table */
        for (int j = 0; j < d->nparts; j++) {
            Part *p = &d->part[j];
            if (p->logical) continue;             /* drawn inside the extended */
            unsigned long long b = p->start * sec;
            if (b > pos + MIN_GAP) region_add(i, -1, pos, b - pos, 0);
            if (p->extended) {
                /* The container. The kernel shows an extended partition
                 * as two sectors, so its extent is taken from the logical
                 * drives inside it (and from the next primary, or the
                 * disk's end, when it is empty). */
                unsigned long long eend = b + MIN_GAP;
                for (int k = 0; k < d->nparts; k++) {
                    Part *q = &d->part[k];
                    if (!q->logical) continue;
                    unsigned long long qe = q->start * sec + q->size;
                    if (q->start * sec >= b && qe > eend) eend = qe;
                }
                unsigned long long limit = d->size;
                for (int k = 0; k < d->nparts; k++) {
                    Part *q = &d->part[k];
                    if (q->logical || q == p) continue;
                    unsigned long long qb = q->start * sec;
                    if (qb > b && qb < limit) limit = qb;
                }
                if (eend < limit && p->size <= 4096) eend = limit;   /* empty: to the next one */
                p->size = eend - b;
                unsigned long long epos = b + MIN_GAP;
                for (int k = 0; k < d->nparts; k++) {
                    Part *q = &d->part[k];
                    if (!q->logical) continue;
                    unsigned long long qb = q->start * sec;
                    if (qb < b || qb >= eend) continue;
                    if (qb > epos + MIN_GAP) region_add(i, -1, epos, qb - epos, 1);
                    region_add(i, k, qb, q->size, 1);
                    epos = qb + q->size;
                }
                if (eend > epos + MIN_GAP) region_add(i, -1, epos, eend - epos, 1);
                pos = eend;
            } else {
                region_add(i, j, b, p->size, 0);
                pos = b + p->size;
            }
        }
        if (d->size > pos + MIN_GAP) region_add(i, -1, pos, d->size - pos, 0);
    }
}

static void build_vols(void)
{
    /* The mapped devices were added while reading; partitions first. */
    Vol mapped[MAX_VOLS];
    int nm = nvols;
    memcpy(mapped, vols, sizeof(Vol) * (size_t)nvols);
    nvols = 0;
    for (int i = 0; i < ndisks; i++) {
        Disk *d = &disks[i];
        if (d->rom) {
            char nm2[80];
            snprintf(nm2, sizeof nm2, "%s", d->label[0] ? d->label : d->wholefs ? d->name : "");
            if (d->wholefs)
                vol_add(i, 0, nm2, d->path, d->fstype, d->mount, "CD-ROM", "Partition", d->size, d->avail, 1);
            continue;
        }
        if (d->wholefs) {
            vol_add(i, 0, d->label[0] ? d->label : d->name, d->path, d->fstype, d->mount,
                    d->rm ? "Removable" : "Basic", "Partition", d->size, d->avail, 0);
            continue;
        }
        for (int j = 0; j < d->nparts; j++) {
            Part *p = &d->part[j];
            if (p->extended) continue;
            char nm2[80];
            part_volname(d, i, p, nm2, sizeof nm2);
            vol_add(i, j, nm2, p->path, p->fstype, p->mount, d->rm ? "Removable" : "Basic",
                    "Partition", p->size, p->avail, 0);
        }
    }
    for (int i = 0; i < nm && nvols < MAX_VOLS; i++) vols[nvols++] = mapped[i];
}

/* ------------------------------------------------------------------ *
 * Running as the administrator
 * ------------------------------------------------------------------ */
static char elevated_user[64];         /* who opened us, when running as root for them */

/* Windows asks for the administrator when Disk Management opens; so does
 * this, through pkexec, and the whole program runs as root from then on.
 * The user's home, display and cookie travel as arguments, since pkexec
 * starts the elevated copy with a clean environment. Returns 1 when this
 * is the elevated copy; 0 to carry on as the user (no pkexec, no agent,
 * or the prompt was dismissed). */
static int elevate(int argc, char **argv)
{
    if (argc >= 6 && !strcmp(argv[1], "--elevated")) {
        setenv("HOME", argv[2], 1);
        setenv("DISPLAY", argv[3], 1);
        setenv("XAUTHORITY", argv[4], 1);
        snprintf(elevated_user, sizeof elevated_user, "%s", argv[5]);
        /* Anything a library caches goes to root's own places. */
        setenv("XDG_CACHE_HOME", "/root/.cache", 1);
        setenv("XDG_CONFIG_HOME", "/root/.config", 1);
        return 1;
    }
    if (geteuid() == 0) return 1;
    if (getenv("W2K_RENDER") || getenv("W2K_FAKE_LSBLK") || getenv("W2K_NO_ELEVATE")) return 0;
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return 0;
    self[n] = 0;
    const char *home = getenv("HOME"), *disp = getenv("DISPLAY"), *xa = getenv("XAUTHORITY");
    if (!home || !disp) return 0;
    char xauth[PATH_MAX];
    if (xa && *xa) snprintf(xauth, sizeof xauth, "%s", xa);
    else snprintf(xauth, sizeof xauth, "%s/.Xauthority", home);
    struct passwd *pw = getpwuid(getuid());
    const char *user = pw ? pw->pw_name : getenv("USER") ? getenv("USER") : "";
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        execlp("pkexec", "pkexec", self, "--elevated", home, disp, xauth, user, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    if (code == 126 || code == 127) return 0;   /* dismissed, refused, or no pkexec */
    exit(code);                                  /* the elevated copy was the program */
}

/* ------------------------------------------------------------------ *
 * Running things as root
 * ------------------------------------------------------------------ */
typedef struct {
    pid_t pid;
    int fd;
    char out[4096];
    int len;
    int done, status;
    int phase;
    W2kWin *dlg;
    const char *text;
} Job;

static void job_reap(Job *j)
{
    if (j->done) return;
    for (;;) {
        char buf[512];
        ssize_t n = read(j->fd, buf, sizeof buf);
        if (n <= 0) break;
        int room = (int)sizeof j->out - 1 - j->len;
        if (room > 0) { if (n > room) n = room; memcpy(j->out + j->len, buf, (size_t)n); j->len += (int)n; }
    }
    j->out[j->len] = 0;
    int st;
    pid_t r = waitpid(j->pid, &st, WNOHANG);
    if (r == j->pid) {
        j->done = 1;
        j->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        close(j->fd);
    }
}

static void busy_paint(W2kWin *w, Drawable d)
{
    Job *j = w->user;
    w2k_bigicon_draw(d, 14, 14, ICO_DRIVE_HDD);
    w2k_text(d, F_UI, 58, 16, j->text, C_TEXT);
    w2k_text(d, F_UI, 58, 16 + w2k_font_height(F_UI) + 4, "Please wait...", C_GRAYTEXT);
    W2kRect pr = { 14, w->h - 34, w->w - 28, 18 };
    w2k_draw_progress(d, &pr, -1, j->phase);
}

static int busy_event(W2kWin *w, XEvent *e)
{
    (void)w; (void)e;
    return 1;                                  /* nothing to click, no Escape */
}

static void busy_tick(void *u)
{
    Job *j = u;
    job_reap(j);
    j->phase++;
    if (j->done) w2k_win_close(j->dlg, ID_OK);
    else w2k_win_dirty(j->dlg);
}

/* Run a shell script as root through pkexec, with a "please wait" box up
 * while it runs. Returns the exit status; the script's output (stderr
 * and stdout together) is left in `out` for the error message. */
static int run_root(W2kWin *over, const char *what, const char *script, char *out, int outsz)
{
    int p[2];
    if (pipe(p) < 0) { snprintf(out, (size_t)outsz, "%s", strerror(errno)); return -1; }
    char full[8192];
    snprintf(full, sizeof full, "export PATH=/usr/sbin:/sbin:/usr/bin:/bin; %s", script);
    pid_t pid = fork();
    if (pid < 0) { snprintf(out, (size_t)outsz, "%s", strerror(errno)); return -1; }
    if (pid == 0) {
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);
        close(p[0]); close(p[1]);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, STDIN_FILENO); close(nul); }
        if (geteuid() == 0) execl("/bin/sh", "sh", "-c", full, (char *)NULL);
        else execlp("pkexec", "pkexec", "sh", "-c", full, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    Job j = { .pid = pid, .fd = p[0], .text = what };
    W2kWin *w = w2k_win_new("Disk Management", "l2kdiskmgmt-wait", 340, 100, 0);
    w->user = &j;
    w->paint = busy_paint;
    w->event = busy_event;
    j.dlg = w;
    w2k_win_center(w, over);
    if (over) XSetTransientForHint(w2k.dpy, w->win, over->win);
    w2k_add_timer(120, busy_tick, &j);
    w2k_win_modal(w);
    w2k_del_timer(busy_tick, &j);
    while (!j.done) { job_reap(&j); if (!j.done) usleep(50000); }
    snprintf(out, (size_t)outsz, "%s", j.out);
    return j.status;
}

static void report(W2kWin *over, const char *title, int status, const char *out)
{
    char msg[4600];
    if (status == 126 || status == 127)
        snprintf(msg, sizeof msg, "The operation was not carried out: it was not authorized, or pkexec is not installed.");
    else if (out && *out) snprintf(msg, sizeof msg, "The operation failed:\n\n%s", out);
    else snprintf(msg, sizeof msg, "The operation failed (exit status %d).", status);
    w2k_msgbox(over, title, msg, MB_OK | MB_ICONERROR);
}

/* ------------------------------------------------------------------ *
 * The window
 * ------------------------------------------------------------------ */
enum { ID_EXIT = 100, ID_REFRESH, ID_RESCAN, ID_HELP, ID_ABOUT, ID_SETTINGS,
       ID_TOP_VOLUMES, ID_TOP_DISKS, ID_TOP_GRAPH, ID_BOTTOM_GRAPH, ID_BOTTOM_VOLUMES,
       ID_BOTTOM_DISKS, ID_BOTTOM_NONE,
       ID_BACK, ID_FORWARD, ID_UP, ID_TREE,
       /* the tasks */
       ID_OPEN, ID_EXPLORE, ID_ACTIVE, ID_MOUNT, ID_FORMAT, ID_EXTEND, ID_SHRINK,
       ID_MIRROR, ID_DELETE, ID_PROPERTIES, ID_CREATE, ID_INIT, ID_DYNAMIC, ID_GPT,
       ID_EJECT };

enum { PANE_VOLUMES, PANE_DISKS, PANE_GRAPH, PANE_NONE };

#define DISK_ROW_H   72
#define DISK_LABEL_W 86
#define LEGEND_H     22
#define STRIPE_H      8

typedef struct {
    W2kWin *win;
    W2kMenubar *mb;
    W2kToolbar *tb;
    W2kList *list, *dlist;
    int top_pane, bottom_pane;
    W2kRect top, bottom, legend;
    W2kScroll gsb;                             /* the picture's scrollbar */
    int sel_region;                            /* selected region, -1 */
    int sel_disk;                              /* selected disk label, -1 */
    int graph_focus;
    int split;                                 /* the top pane's height */
} App;
static App app;

static const char *legend_name[] = { "Unallocated", "Primary partition", "Extended partition",
                                     "Free space", "Logical drive" };
static const unsigned char legend_rgb[][3] = {
    { 0, 0, 0 }, { 0, 0, 128 }, { 0, 128, 0 }, { 128, 255, 128 }, { 0, 102, 204 } };
enum { LG_UNALLOC, LG_PRIMARY, LG_EXTENDED, LG_FREE, LG_LOGICAL };

static void refresh(void);

/* ---- the lists ---- */
static void fill_volume_list(void)
{
    W2kList *l = app.list;
    int keep = l->sel;
    w2k_list_clear(l);
    for (int i = 0; i < nvols; i++) {
        Vol *v = &vols[i];
        int icon = v->rom ? ICO_DRIVE_CD : ICO_DRIVE_HDD;
        if (v->disk >= 0 && disks[v->disk].rm) icon = ICO_DRIVE_FLOPPY;
        int row = w2k_list_add(l, icon, v);
        char b[64], c[64];
        snprintf(b, sizeof b, "%s%s%s%s", v->name, v->mount[0] ? " (" : "",
                 v->mount[0] ? v->mount : "", v->mount[0] ? ")" : "");
        w2k_list_set(l, row, 0, b);
        w2k_list_set(l, row, 1, v->layout);
        w2k_list_set(l, row, 2, v->type);
        w2k_list_set(l, row, 3, v->rom && !v->fstype[0] ? "" : fs_name(v->fstype));
        char st[64] = "Healthy";
        if (v->disk >= 0 && v->part >= 0 && !disks[v->disk].wholefs && !disks[v->disk].rom)
            part_status(&disks[v->disk].part[v->part], st, sizeof st);
        else if (!strcmp(v->mount, "/")) snprintf(st, sizeof st, "Healthy (Boot)");
        w2k_list_set(l, row, 4, st);
        w2k_list_set(l, row, 5, fmt_size(v->size, b, sizeof b));
        if (v->avail != ~0ull) {
            w2k_list_set(l, row, 6, fmt_size(v->avail, c, sizeof c));
            snprintf(b, sizeof b, "%llu %%", v->size ? v->avail * 100 / v->size : 0);
            w2k_list_set(l, row, 7, b);
        } else {
            w2k_list_set(l, row, 6, "");
            w2k_list_set(l, row, 7, "");
        }
    }
    if (keep >= 0 && keep < l->n) { l->sel = keep; l->items[keep].selected = 1; }
}

static void fill_disk_list(void)
{
    W2kList *l = app.dlist;
    w2k_list_clear(l);
    for (int i = 0; i < ndisks; i++) {
        Disk *d = &disks[i];
        int row = w2k_list_add(l, d->rom ? ICO_DRIVE_CD : d->rm ? ICO_DRIVE_FLOPPY : ICO_DRIVE_HDD, d);
        char b[64];
        snprintf(b, sizeof b, d->rom ? "CD-ROM %d" : "Disk %d", d->index);
        w2k_list_set(l, row, 0, b);
        w2k_list_set(l, row, 1, d->rom ? "DVD" : d->rm ? "Removable" : "Basic");
        w2k_list_set(l, row, 2, d->size ? fmt_size(d->size, b, sizeof b) : "");
        unsigned long long free = 0;
        for (int r = 0; r < nregions; r++)
            if (regions[r].disk == i && regions[r].part == -1) free += regions[r].size;
        w2k_list_set(l, row, 3, d->rom ? "" : fmt_size(free, b, sizeof b));
        w2k_list_set(l, row, 4, d->rom && !d->size ? "No Media" : "Online");
        snprintf(b, sizeof b, "%s%s%s", d->vendor[0] ? d->vendor : "",
                 d->vendor[0] && d->model[0] ? " " : "", d->model);
        w2k_list_set(l, row, 5, b[0] ? b : d->tran[0] ? d->tran : "");
        w2k_list_set(l, row, 6, !strcmp(d->pttype, "gpt") ? "GUID Partition Table (GPT)" :
                                !strcmp(d->pttype, "dos") ? "Master Boot Record (MBR)" :
                                d->rom ? "" : d->wholefs ? "None (file system on the disk)" : "Not Initialized");
    }
}

/* ---- the picture ---- */
static int graph_content_h(void)
{
    return ndisks * DISK_ROW_H + 6;
}

/* Lay the regions out across `w` pixels: every one at least wide enough
 * to read, the rest in proportion, as the snap-in does. */
static void layout_regions(int w)
{
    for (int i = 0; i < ndisks; i++) {
        int first = -1, n = 0;
        unsigned long long total = 0;
        for (int r = 0; r < nregions; r++)
            if (regions[r].disk == i) { if (first < 0) first = r; n++; total += regions[r].size; }
        if (n == 0) continue;
        int minw = 58;
        if (minw * n > w) minw = w / n;
        /* A disk contributes a gap and a partition per entry, so its
         * region count is not bounded by MAX_PARTS: size this like
         * regions[] itself. */
        int widths[MAX_DISKS * (MAX_PARTS + 2)];
        int sum = 0;
        for (int k = 0; k < n; k++) {
            DRegion *r = &regions[first + k];
            widths[k] = total ? (int)((double)w * (double)r->size / (double)total) : w / n;
            if (widths[k] < minw) widths[k] = minw;
            sum += widths[k];
        }
        /* Too wide: take the excess off the widest, repeatedly. */
        while (sum > w) {
            int big = 0;
            for (int k = 1; k < n; k++) if (widths[k] > widths[big]) big = k;
            if (widths[big] <= minw) break;
            widths[big]--; sum--;
        }
        /* Too narrow: the last one takes the rest. */
        if (sum < w) { widths[n - 1] += w - sum; sum = w; }
        int x = 0;
        for (int k = 0; k < n; k++) { regions[first + k].x = x; regions[first + k].w = widths[k]; x += widths[k]; }
    }
}

static int region_kind(const DRegion *r)
{
    if (r->part < 0) return r->inext ? LG_FREE : LG_UNALLOC;
    const Disk *d = &disks[r->disk];
    if (d->wholefs || d->rom) return LG_PRIMARY;
    const Part *p = &d->part[r->part];
    return p->logical ? LG_LOGICAL : p->extended ? LG_EXTENDED : LG_PRIMARY;
}

static void hatch(Drawable d, int x, int y, int w, int h)
{
    /* Diagonal lines every six pixels, the selected region's marking. */
    if (w <= 0 || h <= 0) return;
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(160, 160, 160));
    for (int s = -h; s < w; s += 6) {
        int x0 = x + s, y0 = y, x1 = x + s + h - 1, y1 = y + h - 1;
        if (x0 < x) { y0 += x - x0; x0 = x; }
        if (x1 > x + w - 1) { y1 -= x1 - (x + w - 1); x1 = x + w - 1; }
        if (x0 <= x1)
            XDrawLine(w2k.dpy, d, w2k.gc, w2k_cx(x0), w2k_cx(y0), w2k_cx(x1), w2k_cx(y1));
    }
}

/* A line of the label box or a region, cut to fit. */
static void fit_text(Drawable d, int font, int x, int y, int maxw, const char *text)
{
    char t[160];
    if (maxw <= 4) return;
    w2k_ellipsis(font, text, maxw, t, sizeof t);
    w2k_text(d, font, x, y, t, C_TEXT);
}

/* The rows are drawn on a pixmap the size of the pane's inside and
 * copied over, so a row scrolled half out of view stops at the edge. */
static void draw_graph(Drawable dst)
{
    W2kRect g = app.bottom;
    w2k_fill(dst, g.x, g.y, g.w, g.h, C_WINDOW);
    w2k_edge(dst, g.x, g.y, g.w, g.h, EDGE_SUNKEN, BF_RECT);
    int inner_x = g.x + 2, inner_y = g.y + 2, inner_w = g.w - 4, inner_h = g.h - 4;
    int need = w2k_scroll_needed(&app.gsb);
    if (need) inner_w -= SCROLL_W;
    if (inner_w <= 0 || inner_h <= 0) return;
    int pw = w2k_cw(inner_x, inner_w), ph = w2k_cw(inner_y, inner_h);
    Pixmap d = XCreatePixmap(w2k.dpy, w2k.root, (unsigned)pw, (unsigned)ph, w2k.depth);
    w2k_fill(d, 0, 0, inner_w, inner_h, C_WINDOW);
    int fh = w2k_font_height(F_UI);
    int y = 4 - app.gsb.pos;
    int lx = 6;
    int gx = lx + DISK_LABEL_W, gw = inner_w - 12 - DISK_LABEL_W;
    if (gw < 40) gw = 40;
    layout_regions(gw - 2);
    for (int i = 0; i < ndisks; i++, y += DISK_ROW_H) {
        Disk *dk = &disks[i];
        int rh = DISK_ROW_H - 6;
        if (y + rh < 0 || y > inner_h) continue;
        /* The disk's label box, grey in a black line. */
        w2k_fill(d, lx, y, DISK_LABEL_W, rh, C_FACE);
        w2k_frame(d, lx, y, DISK_LABEL_W + gw, rh, C_BLACK);
        if (app.sel_disk == i && app.graph_focus) hatch(d, lx + 1, y + 1, DISK_LABEL_W - 1, rh - 2);
        char b[64];
        int tw = DISK_LABEL_W - 8;
        w2k_icon_draw(d, lx + 4, y + 4, dk->rom ? ICO_DRIVE_CD : dk->rm ? ICO_DRIVE_FLOPPY : ICO_DRIVE_HDD);
        snprintf(b, sizeof b, dk->rom ? "CD-ROM %d" : "Disk %d", dk->index);
        fit_text(d, F_UI_BOLD, lx + 24, y + 4, DISK_LABEL_W - 28, b);
        fit_text(d, F_UI, lx + 4, y + 4 + fh + 2, tw, dk->rom ? "DVD" : dk->rm ? "Removable" : "Basic");
        if (dk->size) fit_text(d, F_UI, lx + 4, y + 4 + 2 * (fh + 2), tw, fmt_size(dk->size, b, sizeof b));
        fit_text(d, F_UI, lx + 4, y + 4 + 3 * (fh + 2), tw,
                 dk->rom && !dk->size ? "No Media" : dk->ro ? "Read-only" : "Online");
        w2k_vline(d, lx + DISK_LABEL_W, y, rh, C_BLACK);
        /* The regions. */
        for (int r = 0; r < nregions; r++) {
            DRegion *rg = &regions[r];
            if (rg->disk != i) continue;
            int rx = gx + 1 + rg->x, rw = rg->w, ry = y + 1, rhh = rh - 2;
            int kind = region_kind(rg);
            w2k_fill(d, rx, ry, rw, rhh, C_WINDOW);
            if (kind == LG_EXTENDED) w2k_fill_rgb(d, rx, ry, rw, rhh, 240, 255, 240);
            const unsigned char *c = legend_rgb[kind];
            w2k_fill_rgb(d, rx, ry, rw, STRIPE_H, c[0], c[1], c[2]);
            if (rg->inext) {
                w2k_fill_rgb(d, rx, ry, rw, 2, legend_rgb[LG_EXTENDED][0], legend_rgb[LG_EXTENDED][1], legend_rgb[LG_EXTENDED][2]);
                w2k_fill_rgb(d, rx, ry + rhh - 2, rw, 2, legend_rgb[LG_EXTENDED][0], legend_rgb[LG_EXTENDED][1], legend_rgb[LG_EXTENDED][2]);
            }
            if (rg->x > 0) w2k_vline(d, rx, ry, rhh, C_BLACK);
            if (app.sel_region == r && app.graph_focus) hatch(d, rx, ry + STRIPE_H, rw, rhh - STRIPE_H);
            /* Three lines of words, shortened to fit. */
            char l1[128], l2[64], l3[64];
            int ty = ry + STRIPE_H + 4;
            if (rg->part == -2) {
                snprintf(l1, sizeof l1, "DVD (%s)", dk->path);
                snprintf(l2, sizeof l2, "No Media");
                l3[0] = 0;
            } else if (rg->part < 0) {
                fmt_size(rg->size, l1, sizeof l1);
                snprintf(l2, sizeof l2, "%s", rg->inext ? "Free space" : "Unallocated");
                l3[0] = 0;
            } else if (dk->wholefs || dk->rom) {
                snprintf(l1, sizeof l1, "%s%s%s%s", dk->label[0] ? dk->label : dk->name,
                         dk->mount[0] ? " (" : "", dk->mount[0] ? dk->mount : "", dk->mount[0] ? ")" : "");
                snprintf(l2, sizeof l2, "%s %s", fmt_size(dk->size, b, sizeof b), fs_name(dk->fstype));
                snprintf(l3, sizeof l3, "Healthy");
            } else {
                Part *p = &dk->part[rg->part];
                char nm[80];
                part_volname(dk, i, p, nm, sizeof nm);
                if (p->extended) snprintf(nm, sizeof nm, "Extended partition");
                snprintf(l1, sizeof l1, "%s%s%s%s", nm, p->mount[0] ? " (" : "",
                         p->mount[0] ? p->mount : "", p->mount[0] ? ")" : "");
                snprintf(l2, sizeof l2, "%s %s", fmt_size(p->size, b, sizeof b),
                         p->extended ? "" : fs_name(p->fstype));
                part_status(p, l3, sizeof l3);
            }
            fit_text(d, F_UI_BOLD, rx + 4, ty, rw - 8, l1);
            fit_text(d, F_UI, rx + 4, ty + fh + 2, rw - 8, l2);
            if (l3[0]) fit_text(d, F_UI, rx + 4, ty + 2 * (fh + 2), rw - 8, l3);
        }
    }
    XCopyArea(w2k.dpy, d, dst, w2k.gc, 0, 0, (unsigned)pw, (unsigned)ph, w2k_cx(inner_x), w2k_cx(inner_y));
    XFreePixmap(w2k.dpy, d);
    if (need) w2k_scroll_draw(dst, &app.gsb);
}

static void draw_legend(Drawable d)
{
    W2kRect r = app.legend;
    w2k_fill(d, r.x, r.y, r.w, r.h, C_FACE);
    int x = r.x + 6, fh = w2k_font_height(F_UI);
    for (int i = 0; i < 5; i++) {
        w2k_fill_rgb(d, x, r.y + (r.h - 10) / 2, 10, 10, legend_rgb[i][0], legend_rgb[i][1], legend_rgb[i][2]);
        w2k_frame(d, x, r.y + (r.h - 10) / 2, 10, 10, C_BLACK);
        w2k_text(d, F_UI, x + 14, r.y + (r.h - fh) / 2, legend_name[i], C_TEXT);
        x += 14 + w2k_text_width(F_UI, legend_name[i], -1) + 14;
    }
}

static void paint(W2kWin *w, Drawable d)
{
    w2k_fill(d, 0, 0, w->w, w->h, C_FACE);
    w2k_menubar_draw(d, app.mb);
    w2k_toolbar_draw(d, app.tb);
    if (app.top_pane == PANE_VOLUMES) w2k_list_draw(d, app.list);
    else if (app.top_pane == PANE_DISKS) w2k_list_draw(d, app.dlist);
    if (app.bottom_pane == PANE_GRAPH) draw_graph(d);
    else if (app.bottom_pane == PANE_VOLUMES) w2k_list_draw(d, app.list);
    else if (app.bottom_pane == PANE_DISKS) w2k_list_draw(d, app.dlist);
    draw_legend(d);
}

static void layout(W2kWin *w)
{
    int y = 0;
    app.mb->r = (W2kRect){ 0, 0, w->w, MENUBAR_H };
    y += MENUBAR_H;
    app.tb->r = (W2kRect){ 0, y, w->w, TOOLBAR_H };
    y += TOOLBAR_H + 2;
    int avail = w->h - y - LEGEND_H - 4;
    int th = app.split > 0 ? app.split : avail * 2 / 5;
    if (th < 80) th = 80;
    if (th > avail - 80) th = avail - 80;
    if (app.bottom_pane == PANE_NONE) th = avail;
    app.top = (W2kRect){ 4, y, w->w - 8, th };
    app.bottom = (W2kRect){ 4, y + th + 6, w->w - 8, avail - th - 6 };
    app.legend = (W2kRect){ 0, w->h - LEGEND_H, w->w, LEGEND_H };
    W2kList *tl = app.top_pane == PANE_DISKS ? app.dlist : app.list;
    tl->r = app.top;
    if (app.bottom_pane == PANE_VOLUMES) app.list->r = app.bottom;
    if (app.bottom_pane == PANE_DISKS) app.dlist->r = app.bottom;
    w2k_list_layout(app.list);
    w2k_list_layout(app.dlist);
    app.gsb.vertical = 1;
    app.gsb.r = (W2kRect){ app.bottom.x + app.bottom.w - 2 - SCROLL_W, app.bottom.y + 2, SCROLL_W, app.bottom.h - 4 };
    app.gsb.page = app.bottom.h - 4;
    app.gsb.total = graph_content_h();
    w2k_scroll_clamp(&app.gsb);
}

/* ---- selection ---- */
static DRegion *sel_region(void) { return app.sel_region >= 0 && app.sel_region < nregions ? &regions[app.sel_region] : NULL; }

static Part *region_part(DRegion *r)
{
    if (!r || r->part < 0) return NULL;
    Disk *d = &disks[r->disk];
    if (d->wholefs || d->rom) return NULL;
    return &d->part[r->part];
}

static void select_region(int idx)
{
    app.sel_region = idx;
    app.sel_disk = -1;
    app.graph_focus = 1;
    /* The volume list follows. */
    DRegion *r = sel_region();
    for (int i = 0; i < app.list->n; i++) app.list->items[i].selected = 0;
    app.list->sel = -1;
    if (r && r->part >= 0)
        for (int i = 0; i < app.list->n; i++) {
            Vol *v = app.list->items[i].data;
            if (v && v->disk == r->disk && (v->part == r->part || disks[r->disk].wholefs || disks[r->disk].rom)) {
                app.list->items[i].selected = 1; app.list->sel = i;
                w2k_list_ensure_visible(app.list, i);
                break;
            }
        }
    w2k_win_dirty(app.win);
}

static void on_list_select(void *u, int idx)
{
    (void)u;
    if (idx < 0 || idx >= app.list->n) return;
    Vol *v = app.list->items[idx].data;
    app.graph_focus = 0;
    app.sel_region = -1;
    if (v && v->disk >= 0)
        for (int r = 0; r < nregions; r++)
            if (regions[r].disk == v->disk && (regions[r].part == v->part ||
                ((disks[v->disk].wholefs || disks[v->disk].rom) && regions[r].part >= 0)))
                { app.sel_region = r; break; }
    w2k_win_dirty(app.win);
}

static int region_at(int x, int y, int *disk_label)
{
    W2kRect g = app.bottom;
    *disk_label = -1;
    if (!w2k_rect_hit(&g, x, y)) return -1;
    int inner_x = g.x + 2, inner_y = g.y + 2;
    int yy = inner_y + 4 - app.gsb.pos;
    int gx = inner_x + 6 + DISK_LABEL_W;
    if (y < inner_y || y >= g.y + g.h - 2) return -1;
    for (int i = 0; i < ndisks; i++, yy += DISK_ROW_H) {
        int rh = DISK_ROW_H - 6;
        if (y < yy || y >= yy + rh) continue;
        if (x >= inner_x + 6 && x < gx) { *disk_label = i; return -1; }
        for (int r = 0; r < nregions; r++)
            if (regions[r].disk == i && x >= gx + 1 + regions[r].x && x < gx + 1 + regions[r].x + regions[r].w)
                return r;
    }
    return -1;
}

/* ------------------------------------------------------------------ *
 * Dialogs
 * ------------------------------------------------------------------ */
typedef struct { const char *name, *fstype, *mkfs; } FsChoice;
static const FsChoice fs_choices[] = {
    { "NTFS",  "ntfs",  "mkfs.ntfs" }, { "FAT32", "vfat", "mkfs.vfat" },
    { "exFAT", "exfat", "mkfs.exfat" }, { "ext4",  "ext4",  "mkfs.ext4" },
    { "ext3",  "ext3",  "mkfs.ext3" }, { "ext2",  "ext2",  "mkfs.ext2" },
    { "XFS",   "xfs",   "mkfs.xfs" }, { "Btrfs", "btrfs", "mkfs.btrfs" },
    { "Swap",  "swap",  "mkswap" },
};
#define NFS ((int)(sizeof fs_choices / sizeof *fs_choices))

static int have_tool(const char *name)
{
    const char *dirs[] = { "/usr/sbin", "/sbin", "/usr/bin", "/bin", NULL };
    for (int i = 0; dirs[i]; i++) {
        char p[256];
        snprintf(p, sizeof p, "%s/%s", dirs[i], name);
        if (access(p, X_OK) == 0) return 1;
    }
    return 0;
}

/* The mkfs line for a file system, label and quickness. */
static void mkfs_cmd(const FsChoice *fs, const char *label, int quick, const char *dev, int raw_dev, char *out, int n)
{
    char ql[256], qd[256];
    w2k_shell_quote(label, ql, sizeof ql);
    if (raw_dev) snprintf(qd, sizeof qd, "%s", dev); else w2k_shell_quote(dev, qd, sizeof qd);
    if (!strcmp(fs->fstype, "ntfs"))
        snprintf(out, (size_t)n, "mkfs.ntfs -F %s %s%s %s", quick ? "-Q" : "", label[0] ? "-L " : "", label[0] ? ql : "", qd);
    else if (!strcmp(fs->fstype, "vfat")) {
        char up[16]; int i;
        for (i = 0; i < 11 && label[i]; i++) up[i] = (char)toupper((unsigned char)label[i]);
        up[i] = 0;
        w2k_shell_quote(up, ql, sizeof ql);
        snprintf(out, (size_t)n, "mkfs.vfat -F 32 %s%s %s", up[0] ? "-n " : "", up[0] ? ql : "", qd);
    } else if (!strcmp(fs->fstype, "exfat"))
        snprintf(out, (size_t)n, "mkfs.exfat %s%s %s", label[0] ? "-n " : "", label[0] ? ql : "", qd);
    else if (!strncmp(fs->fstype, "ext", 3))
        snprintf(out, (size_t)n, "%s -F -q %s %s%s %s", fs->mkfs, quick ? "" : "-E lazy_itable_init=0,lazy_journal_init=0",
                 label[0] ? "-L " : "", label[0] ? ql : "", qd);
    else if (!strcmp(fs->fstype, "xfs") || !strcmp(fs->fstype, "btrfs"))
        snprintf(out, (size_t)n, "%s -f %s%s %s", fs->mkfs, label[0] ? "-L " : "", label[0] ? ql : "", qd);
    else
        snprintf(out, (size_t)n, "mkswap %s%s %s", label[0] ? "-L " : "", label[0] ? ql : "", qd);
}

/* ---- a small dialog framework: edits, combos, check boxes, radios and
 * buttons, drawn and routed by index ---- */
typedef struct {
    enum { C_EDIT, C_COMBO, C_CHECK, C_RADIO, C_BUTTON, C_LABEL } kind;
    W2kRect r;
    const char *text;
    W2kEdit *edit;
    W2kCombo *combo;
    int *value;                 /* checks and radios; radio group in `group` */
    int group, radio_id;
    int id;                     /* buttons */
    int disabled, def;
    void *aux;
} Ctl;

typedef struct {
    Ctl c[24];
    int n, focus, down;
    W2kWin *win;
} Dlg;

static Ctl *dlg_add(Dlg *g, int kind, int x, int y, int w, int h, const char *text)
{
    Ctl *c = &g->c[g->n++];
    memset(c, 0, sizeof *c);
    c->kind = kind; c->r = (W2kRect){ x, y, w, h }; c->text = text;
    return c;
}

static void dlg_paint(W2kWin *w, Drawable d)
{
    Dlg *g = w->user;
    for (int i = 0; i < g->n; i++) {
        Ctl *c = &g->c[i];
        int foc = g->focus == i;
        switch (c->kind) {
        case C_LABEL:  w2k_text(d, F_UI, c->r.x, c->r.y, c->text, c->disabled ? C_GRAYTEXT : C_TEXT); break;
        case C_EDIT:   w2k_edit_draw(d, c->edit); break;
        case C_COMBO:  w2k_combo_draw(d, c->combo); break;
        case C_CHECK:  w2k_draw_checkbox(d, c->r.x, c->r.y, c->text, *c->value, foc, c->disabled); break;
        case C_RADIO:  w2k_draw_radio(d, c->r.x, c->r.y, c->text, *c->value == c->radio_id, foc, c->disabled); break;
        case C_BUTTON: w2k_draw_pushbutton(d, &c->r, c->text, (c->def ? BS_DEFAULT : 0) | (foc ? BS_FOCUS : 0) |
                                           (g->down == i ? BS_PRESSED : 0) | (c->disabled ? BS_DISABLED : 0)); break;
        }
    }
}

static void dlg_focus(Dlg *g, int i)
{
    if (i < 0 || i >= g->n) return;
    g->focus = i;
    for (int k = 0; k < g->n; k++) {
        if (g->c[k].kind == C_EDIT) g->c[k].edit->focused = (k == i);
        if (g->c[k].kind == C_COMBO) g->c[k].combo->focused = (k == i);
    }
}

static int dlg_focusable(Ctl *c) { return c->kind != C_LABEL && !c->disabled; }

static void dlg_tab(Dlg *g, int dir)
{
    for (int k = 1; k <= g->n; k++) {
        int i = ((g->focus + dir * k) % g->n + g->n) % g->n;
        if (dlg_focusable(&g->c[i])) { dlg_focus(g, i); return; }
    }
}

static int dlg_event(W2kWin *w, XEvent *e)
{
    Dlg *g = w->user;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        for (int i = 0; i < g->n; i++) {
            Ctl *c = &g->c[i];
            if (c->disabled) continue;
            if (c->kind == C_EDIT && w2k_edit_press(c->edit, &e->xbutton)) { dlg_focus(g, i); w2k_win_dirty(w); return 1; }
            if (c->kind == C_COMBO && w2k_rect_hit(&c->r, x, y)) { dlg_focus(g, i); w2k_combo_press(c->combo, &e->xbutton); w2k_win_dirty(w); return 1; }
            if ((c->kind == C_CHECK || c->kind == C_RADIO) && x >= c->r.x && x < c->r.x + c->r.w && y >= c->r.y - 2 && y < c->r.y + c->r.h) {
                dlg_focus(g, i);
                if (c->kind == C_CHECK) *c->value = !*c->value; else *c->value = c->radio_id;
                w2k_win_dirty(w);
                return 1;
            }
            if (c->kind == C_BUTTON && w2k_rect_hit(&c->r, x, y)) { g->down = i; dlg_focus(g, i); w2k_win_dirty(w); return 1; }
        }
        return 1;
    }
    case MotionNotify:
        for (int i = 0; i < g->n; i++)
            if (g->c[i].kind == C_EDIT && w2k_edit_motion(g->c[i].edit, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease: {
        for (int i = 0; i < g->n; i++) if (g->c[i].kind == C_EDIT) w2k_edit_release(g->c[i].edit);
        int dn = g->down;
        g->down = -1;
        if (dn >= 0 && w2k_rect_hit(&g->c[dn].r, e->xbutton.x, e->xbutton.y)) w2k_win_close(w, g->c[dn].id);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        Ctl *c = g->focus >= 0 ? &g->c[g->focus] : NULL;
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Tab || ks == XK_ISO_Left_Tab) { dlg_tab(g, (e->xkey.state & ShiftMask) || ks == XK_ISO_Left_Tab ? -1 : 1); w2k_win_dirty(w); return 1; }
        if (c && c->kind == C_COMBO && w2k_combo_key(c->combo, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) {
            if (c && c->kind == C_BUTTON) { w2k_win_close(w, c->id); return 1; }
            for (int i = 0; i < g->n; i++) if (g->c[i].kind == C_BUTTON && g->c[i].def && !g->c[i].disabled) { w2k_win_close(w, g->c[i].id); return 1; }
            return 1;
        }
        if (ks == XK_space && c) {
            if (c->kind == C_CHECK) { *c->value = !*c->value; w2k_win_dirty(w); return 1; }
            if (c->kind == C_RADIO) { *c->value = c->radio_id; w2k_win_dirty(w); return 1; }
            if (c->kind == C_BUTTON) { w2k_win_close(w, c->id); return 1; }
        }
        if (c && c->kind == C_RADIO && (ks == XK_Up || ks == XK_Down || ks == XK_Left || ks == XK_Right)) {
            int dir = (ks == XK_Up || ks == XK_Left) ? -1 : 1;
            for (int k = 1; k < g->n; k++) {
                int i = ((g->focus + dir * k) % g->n + g->n) % g->n;
                if (g->c[i].kind == C_RADIO && g->c[i].group == c->group && !g->c[i].disabled) {
                    dlg_focus(g, i); *g->c[i].value = g->c[i].radio_id; break;
                }
            }
            w2k_win_dirty(w);
            return 1;
        }
        if (c && c->kind == C_EDIT && w2k_edit_key(c->edit, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void blink_cb(void *v) { w2k_edit_blink(v); }

/* Show and run; frees the edits and combos afterwards. */
static int dlg_run(Dlg *g, W2kWin *over, const char *title, int w, int h)
{
    W2kWin *win = w2k_win_new(title, "l2kdiskmgmt-dialog", w, h, 0);
    g->win = win;
    g->down = -1;
    win->user = g;
    win->paint = dlg_paint;
    win->event = dlg_event;
    for (int i = 0; i < g->n; i++) {
        Ctl *c = &g->c[i];
        if (c->kind == C_EDIT) { w2k_edit_bind(c->edit, win); c->edit->r = c->r; w2k_add_timer(w2k_caret_blink, blink_cb, c->edit); }
        if (c->kind == C_COMBO) c->combo->r = c->r;
    }
    g->focus = -1;
    dlg_tab(g, 1);
    w2k_win_center(win, over);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, win->win, w2k.a_net_wm_window_type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&t, 1);
    if (over) XSetTransientForHint(w2k.dpy, win->win, over->win);
    int r = w2k_win_modal(win);
    for (int i = 0; i < g->n; i++)
        if (g->c[i].kind == C_EDIT) w2k_del_timer(blink_cb, g->c[i].edit);
    return r;
}

static void dlg_free(Dlg *g)
{
    for (int i = 0; i < g->n; i++) {
        if (g->c[i].kind == C_EDIT) w2k_edit_free(g->c[i].edit);
        if (g->c[i].kind == C_COMBO) w2k_combo_free(g->c[i].combo);
    }
    g->n = 0;
}

static void checkbox_size(Ctl *c) { c->r.w = 13 + 6 + w2k_text_width(F_UI, c->text, -1) + 4; c->r.h = 16; }

/* ---- Format ---- */
static W2kCombo *fs_combo(int *first_default)
{
    W2kCombo *c = w2k_combo_new(0);
    int n = 0, def = -1;
    for (int i = 0; i < NFS; i++) {
        if (!have_tool(fs_choices[i].mkfs)) continue;
        w2k_combo_add(c, fs_choices[i].name);
        if (def < 0 && !strcmp(fs_choices[i].fstype, "ext4")) def = n;
        n++;
    }
    if (def < 0 && n) def = 0;
    c->sel = def;
    *first_default = def;
    return c;
}

static const FsChoice *fs_from_combo(W2kCombo *c)
{
    const char *t = w2k_combo_text(c);
    for (int i = 0; i < NFS; i++) if (t && !strcmp(fs_choices[i].name, t)) return &fs_choices[i];
    return NULL;
}

static void do_format(const char *dev, const char *volname, const char *cur_label, int over_mounted)
{
    Dlg g = { 0 };
    char title[160];
    snprintf(title, sizeof title, "Format %s", volname);
    int y = 14;
    dlg_add(&g, C_LABEL, 12, y + 3, 100, 16, "Volume label:");
    Ctl *lab = dlg_add(&g, C_EDIT, 130, y, 190, 21, NULL);
    lab->edit = w2k_edit_new(0); w2k_edit_set(lab->edit, cur_label ? cur_label : "");
    y += 30;
    dlg_add(&g, C_LABEL, 12, y + 3, 100, 16, "File system:");
    Ctl *fsc = dlg_add(&g, C_COMBO, 130, y, 190, 21, NULL);
    int def; fsc->combo = fs_combo(&def);
    y += 30;
    dlg_add(&g, C_LABEL, 12, y + 3, 110, 16, "Allocation unit size:");
    Ctl *au = dlg_add(&g, C_COMBO, 130, y, 190, 21, NULL);
    au->combo = w2k_combo_new(0); w2k_combo_add(au->combo, "Default"); au->combo->sel = 0;
    y += 34;
    int quick = 1, compress = 0;
    Ctl *q = dlg_add(&g, C_CHECK, 12, y, 0, 0, "Perform a quick format"); q->value = &quick; checkbox_size(q);
    y += 22;
    Ctl *cp = dlg_add(&g, C_CHECK, 12, y, 0, 0, "Enable file and folder compression"); cp->value = &compress; cp->disabled = 1; checkbox_size(cp);
    y += 34;
    Ctl *ok = dlg_add(&g, C_BUTTON, 332 - 12 - 75 * 2 - 6, y, 75, 23, "OK"); ok->id = ID_OK; ok->def = 1;
    Ctl *ca = dlg_add(&g, C_BUTTON, 332 - 12 - 75, y, 75, 23, "Cancel"); ca->id = ID_CANCEL;
    int r = dlg_run(&g, app.win, title, 332, y + 23 + 14);
    const FsChoice *fs = fs_from_combo(fsc->combo);
    char label[80];
    snprintf(label, sizeof label, "%s", w2k_edit_text(lab->edit));
    dlg_free(&g);
    if (r != ID_OK || !fs) return;
    if (w2k_msgbox(app.win, "Format", "Formatting this volume will erase all data on it. Back up any data you "
                   "want to keep before formatting. Do you want to continue?", MB_YESNO | MB_ICONWARNING) != ID_YES)
        return;
    char cmd[1200], qd[256], script[2000], out[4096];
    mkfs_cmd(fs, label, quick, dev, 0, cmd, sizeof cmd);
    w2k_shell_quote(dev, qd, sizeof qd);
    if (over_mounted)
        snprintf(script, sizeof script, "umount %s || exit 1; wipefs -a -q %s >/dev/null 2>&1; %s", qd, qd, cmd);
    else
        snprintf(script, sizeof script, "wipefs -a -q %s >/dev/null 2>&1; %s", qd, cmd);
    char what[200];
    snprintf(what, sizeof what, "Formatting %s as %s...", volname, fs->name);
    int st = run_root(app.win, what, script, out, sizeof out);
    if (st != 0) report(app.win, "Format", st, out);
    refresh();
}

/* ---- Create Partition ---- */
static void do_create(DRegion *rg)
{
    Disk *d = &disks[rg->disk];
    if (!d->pttype[0]) {
        w2k_msgbox(app.win, "Disk Management", "The disk has no partition table. Initialize it first "
                   "(right-click the disk and choose Initialize Disk).", MB_OK | MB_ICONINFO);
        return;
    }
    Dlg g = { 0 };
    char b[64], free_txt[96];
    unsigned long long max_mb = rg->size / (1024 * 1024);
    snprintf(free_txt, sizeof free_txt, "Free space: %s (%llu MB)", fmt_size(rg->size, b, sizeof b), max_mb);
    int y = 14;
    dlg_add(&g, C_LABEL, 12, y, 300, 16, free_txt);
    y += 24;
    dlg_add(&g, C_LABEL, 12, y + 3, 150, 16, "Partition size in MB:");
    Ctl *sz = dlg_add(&g, C_EDIT, 170, y, 120, 21, NULL);
    sz->edit = w2k_edit_new(0);
    snprintf(b, sizeof b, "%llu", max_mb);
    w2k_edit_set(sz->edit, b);
    w2k_edit_select_all(sz->edit);
    y += 32;
    int kind = rg->inext ? 2 : 0;                  /* 0 primary, 1 extended, 2 logical */
    int mbr = !strcmp(d->pttype, "dos");
    int has_ext = 0;
    for (int j = 0; j < d->nparts; j++) if (d->part[j].extended) has_ext = 1;
    if (mbr) {
        dlg_add(&g, C_LABEL, 12, y, 200, 16, "Select the partition type to create:");
        y += 20;
        Ctl *r1 = dlg_add(&g, C_RADIO, 24, y, 200, 16, "Primary partition"); r1->value = &kind; r1->radio_id = 0; r1->group = 1; r1->disabled = rg->inext;
        checkbox_size(r1);
        y += 20;
        Ctl *r2 = dlg_add(&g, C_RADIO, 24, y, 200, 16, "Extended partition"); r2->value = &kind; r2->radio_id = 1; r2->group = 1; r2->disabled = rg->inext || has_ext;
        checkbox_size(r2);
        y += 20;
        Ctl *r3 = dlg_add(&g, C_RADIO, 24, y, 200, 16, "Logical drive"); r3->value = &kind; r3->radio_id = 2; r3->group = 1; r3->disabled = !rg->inext;
        checkbox_size(r3);
        y += 26;
    }
    int fmt = 1, quick = 1;
    Ctl *fc = dlg_add(&g, C_CHECK, 12, y, 0, 0, "Format this partition with the following settings:"); fc->value = &fmt; checkbox_size(fc);
    y += 24;
    dlg_add(&g, C_LABEL, 24, y + 3, 100, 16, "File system:");
    Ctl *fsc = dlg_add(&g, C_COMBO, 130, y, 170, 21, NULL);
    int def; fsc->combo = fs_combo(&def);
    y += 28;
    dlg_add(&g, C_LABEL, 24, y + 3, 100, 16, "Volume label:");
    Ctl *lab = dlg_add(&g, C_EDIT, 130, y, 170, 21, NULL);
    lab->edit = w2k_edit_new(0); w2k_edit_set(lab->edit, "New Volume");
    y += 28;
    Ctl *q = dlg_add(&g, C_CHECK, 24, y, 0, 0, "Perform a quick format"); q->value = &quick; checkbox_size(q);
    y += 32;
    Ctl *ok = dlg_add(&g, C_BUTTON, 340 - 12 - 75 * 2 - 6, y, 75, 23, "OK"); ok->id = ID_OK; ok->def = 1;
    Ctl *ca = dlg_add(&g, C_BUTTON, 340 - 12 - 75, y, 75, 23, "Cancel"); ca->id = ID_CANCEL;
    int r = dlg_run(&g, app.win, "Create Partition", 340, y + 23 + 14);
    unsigned long long mb = strtoull(w2k_edit_text(sz->edit), NULL, 10);
    const FsChoice *fs = fs_from_combo(fsc->combo);
    char label[80];
    snprintf(label, sizeof label, "%s", w2k_edit_text(lab->edit));
    dlg_free(&g);
    if (r != ID_OK) return;
    if (mb < 1 || mb > max_mb) {
        w2k_msgbox(app.win, "Create Partition", "The size must be between 1 MB and the free space.", MB_OK | MB_ICONWARNING);
        return;
    }
    /* The new partition in sectors, its type by what will go on it. */
    unsigned long long sec = (unsigned long long)d->logsec;
    unsigned long long start = rg->start / sec, size = mb * 1024 * 1024 / sec;
    const char *type = "L";
    if (kind == 1) type = "E";
    else if (fs) {
        if (!strcmp(fs->fstype, "swap")) type = "S";
        else if (mbr && (!strcmp(fs->fstype, "vfat"))) type = "c";
        else if (mbr && (!strcmp(fs->fstype, "ntfs") || !strcmp(fs->fstype, "exfat"))) type = "7";
        else if (!mbr && (!strcmp(fs->fstype, "ntfs") || !strcmp(fs->fstype, "exfat") || !strcmp(fs->fstype, "vfat")))
            type = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
    }
    char qd[256], script[2400], out[4096];
    w2k_shell_quote(d->path, qd, sizeof qd);
    /* sfdisk appends the partition and tells the kernel; then the new
     * device is found by its start and formatted. */
    snprintf(script, sizeof script,
             "printf '%%s\\n' '%llu,%llu,%s' | sfdisk --append -q %s || exit 1; "
             "partprobe %s >/dev/null 2>&1; sleep 1; ",
             start, size, type, qd, qd);
    if (fmt && fs && kind != 1) {
        char cmd[1200];
        char find[400];
        snprintf(find, sizeof find,
                 "DEV=$(lsblk -b -P -o PATH,START,TYPE %s | grep 'START=\"%llu\"' | grep 'TYPE=\"part\"' | sed 's/.*PATH=\"\\([^\"]*\\)\".*/\\1/' | head -n 1); "
                 "[ -n \"$DEV\" ] || { echo 'The new partition did not appear.'; exit 1; }; ",
                 qd, start);
        mkfs_cmd(fs, label, quick, "\"$DEV\"", 1, cmd, sizeof cmd);
        strncat(script, find, sizeof script - strlen(script) - 1);
        strncat(script, cmd, sizeof script - strlen(script) - 1);
    }
    int st = run_root(app.win, "Creating the partition...", script, out, sizeof out);
    if (st != 0) report(app.win, "Create Partition", st, out);
    refresh();
}

/* ---- Delete ---- */
static void do_delete(DRegion *rg)
{
    Disk *d = &disks[rg->disk];
    Part *p = region_part(rg);
    if (!p) return;
    if (p->busy) {
        w2k_msgbox(app.win, "Disk Management", "This volume is in use by the system and cannot be deleted "
                   "from here.", MB_OK | MB_ICONWARNING);
        return;
    }
    if (p->extended)
        for (int j = 0; j < d->nparts; j++)
            if (d->part[j].logical) {
                w2k_msgbox(app.win, "Disk Management", "Delete the logical drives in the extended partition first.", MB_OK | MB_ICONWARNING);
                return;
            }
    if (w2k_msgbox(app.win, "Delete partition",
                   "All data on this partition will be lost. Do you want to continue?", MB_YESNO | MB_ICONWARNING) != ID_YES)
        return;
    char qd[256], qp[256], script[1200], out[4096];
    w2k_shell_quote(d->path, qd, sizeof qd);
    w2k_shell_quote(p->path, qp, sizeof qp);
    snprintf(script, sizeof script, "%s%s%s sfdisk -q --delete %s %d && partprobe %s >/dev/null 2>&1; true",
             p->mount[0] ? "umount " : "", p->mount[0] ? qp : "", p->mount[0] ? " || exit 1;" : "", qd, p->number, qd);
    int st = run_root(app.win, "Deleting the partition...", script, out, sizeof out);
    if (st != 0) report(app.win, "Delete partition", st, out);
    refresh();
}

/* ---- Mark active ---- */
static void do_active(DRegion *rg)
{
    Disk *d = &disks[rg->disk];
    Part *p = region_part(rg);
    if (!p) return;
    char qd[256], script[600], out[4096];
    w2k_shell_quote(d->path, qd, sizeof qd);
    if (!strcmp(d->pttype, "dos"))
        snprintf(script, sizeof script, "sfdisk -q --activate %s %d", qd, p->number);
    else
        snprintf(script, sizeof script, "sfdisk -q --part-attrs %s %d LegacyBIOSBootable", qd, p->number);
    int st = run_root(app.win, "Marking the partition active...", script, out, sizeof out);
    if (st != 0) report(app.win, "Mark Partition as Active", st, out);
    refresh();
}

/* ---- Mount points ---- */
static void do_mount(const char *dev, const char *name, const char *mount)
{
    char text[600];
    if (mount[0]) {
        snprintf(text, sizeof text, "%s is mounted at %s.\n\nDo you want to unmount it?", name, mount);
        if (w2k_msgbox(app.win, "Change Drive Letter and Paths", text, MB_YESNO | MB_ICONQUESTION) != ID_YES) return;
        char qp[256], script[400], out[4096];
        w2k_shell_quote(dev, qp, sizeof qp);
        snprintf(script, sizeof script, "umount %s", qp);
        int st = run_root(app.win, "Unmounting...", script, out, sizeof out);
        if (st != 0) report(app.win, "Change Drive Letter and Paths", st, out);
        refresh();
        return;
    }
    char dir[512], def[300];
    const char *base = strrchr(dev, '/');
    snprintf(def, sizeof def, "/mnt/%s", base ? base + 1 : dev);
    if (!w2k_prompt(app.win, "Change Drive Letter and Paths", "Mount in the following empty folder:", def, dir, sizeof dir, ICO_DRIVE_HDD))
        return;
    if (dir[0] != '/') {
        w2k_msgbox(app.win, "Change Drive Letter and Paths", "The folder must be an absolute path.", MB_OK | MB_ICONWARNING);
        return;
    }
    char qp[256], qq[600], script[1200], out[4096];
    w2k_shell_quote(dev, qp, sizeof qp);
    w2k_shell_quote(dir, qq, sizeof qq);
    snprintf(script, sizeof script, "mkdir -p %s && mount %s %s", qq, qp, qq);
    int st = run_root(app.win, "Mounting...", script, out, sizeof out);
    if (st != 0) report(app.win, "Change Drive Letter and Paths", st, out);
    refresh();
}

/* ---- Initialize disk ---- */
static void do_init(Disk *d)
{
    Dlg g = { 0 };
    int y = 14;
    char t[200];
    snprintf(t, sizeof t, "Disk Management will write a partition table to %s.", d->path);
    dlg_add(&g, C_LABEL, 12, y, 320, 16, t);
    y += 18;
    dlg_add(&g, C_LABEL, 12, y, 320, 16, "Anything on the disk now will be lost.");
    y += 26;
    dlg_add(&g, C_LABEL, 12, y, 320, 16, "Use the following partition style:");
    y += 20;
    int style = 1;
    Ctl *r1 = dlg_add(&g, C_RADIO, 24, y, 0, 0, "MBR (Master Boot Record)"); r1->value = &style; r1->radio_id = 0; r1->group = 1; checkbox_size(r1);
    y += 20;
    Ctl *r2 = dlg_add(&g, C_RADIO, 24, y, 0, 0, "GPT (GUID Partition Table)"); r2->value = &style; r2->radio_id = 1; r2->group = 1; checkbox_size(r2);
    y += 32;
    Ctl *ok = dlg_add(&g, C_BUTTON, 352 - 12 - 75 * 2 - 6, y, 75, 23, "OK"); ok->id = ID_OK; ok->def = 1;
    Ctl *ca = dlg_add(&g, C_BUTTON, 352 - 12 - 75, y, 75, 23, "Cancel"); ca->id = ID_CANCEL;
    int r = dlg_run(&g, app.win, "Initialize Disk", 352, y + 23 + 14);
    dlg_free(&g);
    if (r != ID_OK) return;
    if (d->nparts || d->wholefs) {
        if (w2k_msgbox(app.win, "Initialize Disk", "The disk holds data. Writing a new partition table will "
                       "erase it all. Do you want to continue?", MB_YESNO | MB_ICONWARNING) != ID_YES) return;
    }
    char qd[256], script[600], out[4096];
    w2k_shell_quote(d->path, qd, sizeof qd);
    snprintf(script, sizeof script, "wipefs -a -q %s >/dev/null 2>&1; printf 'label: %s\\n' | sfdisk -q %s && partprobe %s >/dev/null 2>&1; true",
             qd, style ? "gpt" : "dos", qd, qd);
    int st = run_root(app.win, "Initializing the disk...", script, out, sizeof out);
    if (st != 0) report(app.win, "Initialize Disk", st, out);
    refresh();
}

/* ---- Properties ---- */
static void props_paint(W2kWin *w, Drawable d)
{
    Dlg *g = w->user;
    dlg_paint(w, d);
    w2k_bigicon_draw(d, 14, 12, (int)(long)g->c[0].aux);
    /* The bar of used and free space under the figures, as the sheet
     * draws its pie: used in blue, free in magenta. */
    unsigned long long *nums = g->c[g->n - 1].aux;
    if (!nums) return;
    unsigned long long size = nums[0], avail = nums[1];
    W2kRect r = { 14, w->h - 90, w->w - 28, 18 };
    w2k_draw_well(d, &r);
    if (size) {
        int used_w = (int)((r.w - 4) * (double)(size - (avail == ~0ull ? size : avail)) / (double)size);
        w2k_fill_rgb(d, r.x + 2, r.y + 2, used_w, r.h - 4, 0, 0, 255);
        w2k_fill_rgb(d, r.x + 2 + used_w, r.y + 2, r.w - 4 - used_w, r.h - 4, 255, 0, 255);
    }
}

static void do_properties(Vol *v)
{
    Dlg g = { 0 };
    static char vals[8][200];
    const char *keys[8];
    int n = 0;
    keys[n] = "Type:"; snprintf(vals[n++], 200, "%s", v->rom ? "CD-ROM Drive" : v->type[0] && !strcmp(v->type, "Removable") ? "Removable Disk" : !strcmp(v->type, "Dynamic") ? "Dynamic Volume" : "Local Disk");
    keys[n] = "File system:"; snprintf(vals[n++], 200, "%s", fs_name(v->fstype));
    keys[n] = "Device:"; snprintf(vals[n++], 200, "%s", v->path);
    keys[n] = "Mounted at:"; snprintf(vals[n++], 200, "%s", v->mount[0] ? v->mount : "(not mounted)");
    if (v->avail != ~0ull) {
        keys[n] = "Used space:"; fmt_size(v->size - v->avail, vals[n], 200); n++;
        keys[n] = "Free space:"; fmt_size(v->avail, vals[n], 200); n++;
    }
    keys[n] = "Capacity:"; fmt_size(v->size, vals[n], 200); n++;
    int y = 14;
    static char title[160];
    snprintf(title, sizeof title, "%s Properties", v->name);
    Ctl *nm = dlg_add(&g, C_LABEL, 52, y + 8, 260, 16, v->name);
    nm->aux = (void *)(long)(v->rom ? ICO_DRIVE_CD : v->type[0] && !strcmp(v->type, "Removable") ? ICO_DRIVE_FLOPPY : ICO_DRIVE_HDD);
    y += 40;
    for (int i = 0; i < n; i++) {
        dlg_add(&g, C_LABEL, 14, y, 100, 16, keys[i]);
        dlg_add(&g, C_LABEL, 110, y, 210, 16, vals[i]);
        y += 18;
    }
    y += 10;
    Ctl *ok = dlg_add(&g, C_BUTTON, 340 - 12 - 75, y + 40, 75, 23, "OK"); ok->id = ID_OK; ok->def = 1;
    /* The figures for the bar ride on the last control. */
    static unsigned long long nums[2];
    nums[0] = v->size; nums[1] = v->avail;
    ok->aux = nums;
    W2kWin *win = w2k_win_new(title, "l2kdiskmgmt-dialog", 340, y + 40 + 23 + 14, 0);
    g.win = win; g.down = -1;
    win->user = &g;
    win->paint = props_paint;
    win->event = dlg_event;
    g.focus = -1;
    dlg_tab(&g, 1);
    w2k_win_center(win, app.win);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, win->win, w2k.a_net_wm_window_type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&t, 1);
    XSetTransientForHint(w2k.dpy, win->win, app.win->win);
    w2k_win_modal(win);
    dlg_free(&g);
}

/* ------------------------------------------------------------------ *
 * Commands
 * ------------------------------------------------------------------ */
static void open_in_explorer(const char *mount, int explore)
{
    if (!mount || !*mount) {
        w2k_msgbox(app.win, "Disk Management", "The volume is not mounted. Mount it first "
                   "(Change Drive Letter and Paths).", MB_OK | MB_ICONINFO);
        return;
    }
    char q[600], cmd[900], qu[128];
    w2k_shell_quote(mount, q, sizeof q);
    (void)explore;
    /* Explorer is the user's program, not the administrator's. */
    if (elevated_user[0]) {
        w2k_shell_quote(elevated_user, qu, sizeof qu);
        snprintf(cmd, sizeof cmd, "runuser -u %s -- l2kexplorer %s >/dev/null 2>&1 &", qu, q);
    } else
        snprintf(cmd, sizeof cmd, "l2kexplorer %s >/dev/null 2>&1 &", q);
    if (system(cmd) < 0) { /* nothing to say */ }
}

static Vol *vol_for_region(DRegion *r)
{
    if (!r || r->part < 0) return NULL;
    for (int i = 0; i < nvols; i++)
        if (vols[i].disk == r->disk && (vols[i].part == r->part || disks[r->disk].wholefs || disks[r->disk].rom))
            return &vols[i];
    return NULL;
}

static Vol *current_vol(void)
{
    if (app.graph_focus) return vol_for_region(sel_region());
    if (app.list->sel >= 0 && app.list->sel < app.list->n) return app.list->items[app.list->sel].data;
    return NULL;
}

static void command(void *u, int id)
{
    (void)u;
    DRegion *rg = sel_region();
    Vol *v = current_vol();
    Part *p = region_part(rg);
    switch (id) {
    case ID_EXIT: w2k_win_close(app.win, 0); return;
    case ID_REFRESH: case ID_RESCAN: refresh(); return;
    case ID_HELP:
        w2k_msgbox(app.win, "Disk Management",
                   "Disk Management shows every disk as a row of partitions drawn to scale, with the "
                   "volumes listed above.\n\nRight-click a partition to open, format, mount or delete it, "
                   "or to mark it active; right-click unallocated space to create a partition there; "
                   "right-click a disk with no partition table to initialize it.\n\nChanges are made with "
                   "sfdisk, mkfs and mount as root, and are asked for first.", MB_OK | MB_ICONINFO);
        return;
    case ID_ABOUT:
        w2k_msgbox(app.win, "About Disk Management",
                   "Disk Management\nPart of Linux 2000.\n\nPartitions and formats the disks in this computer.",
                   MB_OK | MB_ICONINFO);
        return;
    case ID_SETTINGS:
        w2k_msgbox(app.win, "Settings", "The colours of the legend are the ones the snap-in uses; there is nothing to set.", MB_OK | MB_ICONINFO);
        return;
    case ID_TOP_VOLUMES: app.top_pane = PANE_VOLUMES; layout(app.win); w2k_win_dirty(app.win); return;
    case ID_TOP_DISKS:   app.top_pane = PANE_DISKS; layout(app.win); w2k_win_dirty(app.win); return;
    case ID_BOTTOM_GRAPH:   app.bottom_pane = PANE_GRAPH; layout(app.win); w2k_win_dirty(app.win); return;
    case ID_BOTTOM_VOLUMES: app.bottom_pane = PANE_VOLUMES; if (app.top_pane == PANE_VOLUMES) app.top_pane = PANE_DISKS; layout(app.win); w2k_win_dirty(app.win); return;
    case ID_BOTTOM_DISKS:   app.bottom_pane = PANE_DISKS; if (app.top_pane == PANE_DISKS) app.top_pane = PANE_VOLUMES; layout(app.win); w2k_win_dirty(app.win); return;
    case ID_BOTTOM_NONE:    app.bottom_pane = PANE_NONE; layout(app.win); w2k_win_dirty(app.win); return;
    case ID_BACK: case ID_FORWARD: case ID_UP: case ID_TREE: return;
    case ID_OPEN: case ID_EXPLORE:
        if (v) open_in_explorer(v->mount, id == ID_EXPLORE);
        return;
    case ID_PROPERTIES:
        if (v) do_properties(v);
        else if (app.sel_disk >= 0) {
            Disk *d = &disks[app.sel_disk];
            char t[600], b[64];
            snprintf(t, sizeof t, "%s %s\n\nDevice: %s\nBus: %s\nCapacity: %s\nPartition style: %s\nSector size: %d bytes",
                     d->vendor, d->model, d->path, d->tran[0] ? d->tran : "unknown", fmt_size(d->size, b, sizeof b),
                     !strcmp(d->pttype, "gpt") ? "GPT" : !strcmp(d->pttype, "dos") ? "MBR" : "none", d->logsec);
            char title[80];
            snprintf(title, sizeof title, "%s %d Properties", d->rom ? "CD-ROM" : "Disk", d->index);
            w2k_msgbox(app.win, title, t, MB_OK | MB_ICONINFO);
        }
        return;
    case ID_FORMAT:
        if (rg && p) {
            if (p->busy) { w2k_msgbox(app.win, "Format", "This volume is in use by the system and cannot be formatted from here.", MB_OK | MB_ICONWARNING); return; }
            char nm[80]; part_volname(&disks[rg->disk], rg->disk, p, nm, sizeof nm);
            do_format(p->path, nm, p->label, p->mount[0]);
        } else if (rg && rg->part >= 0 && disks[rg->disk].wholefs) {
            Disk *d = &disks[rg->disk];
            do_format(d->path, d->label[0] ? d->label : d->name, d->label, d->mount[0]);
        } else if (v && v->disk < 0)
            w2k_msgbox(app.win, "Format", "Dynamic volumes (LVM, encrypted) are managed by their own tools.", MB_OK | MB_ICONINFO);
        return;
    case ID_DELETE:  if (rg) do_delete(rg); return;
    case ID_ACTIVE:  if (rg) do_active(rg); return;
    case ID_MOUNT:
        if (p) { char nm[80]; part_volname(&disks[rg->disk], rg->disk, p, nm, sizeof nm); do_mount(p->path, nm, p->mount); }
        else if (v) do_mount(v->path, v->name, v->mount);
        return;
    case ID_CREATE:  if (rg && rg->part == -1) do_create(rg); return;
    case ID_INIT:    if (app.sel_disk >= 0) do_init(&disks[app.sel_disk]); return;
    case ID_EJECT:
        if (app.sel_disk >= 0) {
            char qd[256], script[400], out[4096];
            w2k_shell_quote(disks[app.sel_disk].path, qd, sizeof qd);
            snprintf(script, sizeof script, "eject %s", qd);
            int st = run_root(app.win, "Ejecting...", script, out, sizeof out);
            if (st != 0) report(app.win, "Eject", st, out);
            refresh();
        }
        return;
    case ID_EXTEND: case ID_SHRINK: case ID_MIRROR: case ID_DYNAMIC: case ID_GPT:
        return;
    }
}

/* ---- menus ---- */
static W2kMenu *build_file(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_EXIT, "E&xit", NULL, ICO_NONE);
    return m;
}

/* The tasks for whatever is selected: the same list the context menu
 * shows, so Action > All Tasks and a right-click agree. */
static W2kMenu *build_tasks(void)
{
    W2kMenu *m = w2k_menu_new();
    DRegion *rg = app.graph_focus ? sel_region() : NULL;
    Vol *v = current_vol();
    Part *p = region_part(rg);
    if (app.graph_focus && app.sel_disk >= 0) {
        Disk *d = &disks[app.sel_disk];
        if (d->rom) { w2k_menu_item(m, ID_EJECT, "&Eject", NULL, ICO_NONE); }
        else {
            w2k_menu_item(m, ID_INIT, "&Initialize Disk...", NULL, ICO_NONE);
            w2k_menu_item(m, ID_DYNAMIC, "Upgrade to &Dynamic Disk...", NULL, ICO_NONE); w2k_menu_disable(m);
        }
        w2k_menu_sep(m);
        w2k_menu_item(m, ID_PROPERTIES, "P&roperties", NULL, ICO_NONE);
        w2k_menu_sep(m);
        w2k_menu_item(m, ID_HELP, "&Help", NULL, ICO_NONE);
        return m;
    }
    if (rg && rg->part == -1) {
        w2k_menu_item(m, ID_CREATE, rg->inext ? "Create &Logical Drive..." : "&Create Partition...", NULL, ICO_NONE);
        w2k_menu_sep(m);
        w2k_menu_item(m, ID_PROPERTIES, "P&roperties", NULL, ICO_NONE); w2k_menu_disable(m);
        w2k_menu_sep(m);
        w2k_menu_item(m, ID_HELP, "&Help", NULL, ICO_NONE);
        return m;
    }
    if (rg && rg->part == -2) {
        w2k_menu_item(m, ID_EJECT, "&Eject", NULL, ICO_NONE);
        w2k_menu_sep(m);
        w2k_menu_item(m, ID_HELP, "&Help", NULL, ICO_NONE);
        return m;
    }
    int has_fs = v && v->fstype[0];
    int mounted = v && v->mount[0];
    w2k_menu_item(m, ID_OPEN, "&Open", NULL, ICO_NONE); if (!mounted) w2k_menu_disable(m);
    w2k_menu_item(m, ID_EXPLORE, "&Explore", NULL, ICO_NONE); if (!mounted) w2k_menu_disable(m);
    w2k_menu_sep(m);
    if (p && !p->extended) {
        w2k_menu_item(m, ID_ACTIVE, "Mark Partition as &Active", NULL, ICO_NONE);
        if (p->active || p->logical) w2k_menu_disable(m);
    }
    w2k_menu_item(m, ID_MOUNT, mounted ? "&Change Drive Letter and Path..." : "&Change Drive Letter and Path...", NULL, ICO_NONE);
    if (!has_fs || (v && v->rom)) w2k_menu_disable(m);
    w2k_menu_item(m, ID_FORMAT, "&Format...", NULL, ICO_NONE);
    if ((p && (p->extended || p->busy)) || (v && (v->rom || v->disk < 0))) w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_EXTEND, "E&xtend Volume...", NULL, ICO_NONE); w2k_menu_disable(m);
    w2k_menu_item(m, ID_SHRINK, "&Shrink Volume...", NULL, ICO_NONE); w2k_menu_disable(m);
    w2k_menu_item(m, ID_MIRROR, "Add &Mirror...", NULL, ICO_NONE); w2k_menu_disable(m);
    w2k_menu_item(m, ID_DELETE, p && p->extended ? "&Delete Partition..." : "&Delete Volume...", NULL, ICO_NONE);
    if (!p || p->busy || (v && v->rom)) w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_PROPERTIES, "P&roperties", NULL, ICO_NONE);
    if (!v) w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_HELP, "&Help", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_action(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_REFRESH, "&Refresh", "F5", ICO_NONE);
    w2k_menu_item(m, ID_RESCAN, "Re&scan Disks", NULL, ICO_NONE);
    w2k_menu_sep(m);
    w2k_menu_sub(m, "All Tas&ks", ICO_NONE, build_tasks());
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_HELP, "&Help", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_view(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    W2kMenu *top = w2k_menu_new();
    w2k_menu_item(top, ID_TOP_DISKS, "&Disk List", NULL, ICO_NONE); w2k_menu_check(top, app.top_pane == PANE_DISKS);
    w2k_menu_item(top, ID_TOP_VOLUMES, "&Volume List", NULL, ICO_NONE); w2k_menu_check(top, app.top_pane == PANE_VOLUMES);
    w2k_menu_sub(m, "&Top", ICO_NONE, top);
    W2kMenu *bot = w2k_menu_new();
    w2k_menu_item(bot, ID_BOTTOM_DISKS, "&Disk List", NULL, ICO_NONE); w2k_menu_check(bot, app.bottom_pane == PANE_DISKS);
    w2k_menu_item(bot, ID_BOTTOM_VOLUMES, "&Volume List", NULL, ICO_NONE); w2k_menu_check(bot, app.bottom_pane == PANE_VOLUMES);
    w2k_menu_item(bot, ID_BOTTOM_GRAPH, "&Graphical View", NULL, ICO_NONE); w2k_menu_check(bot, app.bottom_pane == PANE_GRAPH);
    w2k_menu_item(bot, ID_BOTTOM_NONE, "&Hidden", NULL, ICO_NONE); w2k_menu_check(bot, app.bottom_pane == PANE_NONE);
    w2k_menu_sub(m, "&Bottom", ICO_NONE, bot);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_SETTINGS, "&Settings...", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_help(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_HELP, "&Help Topics", NULL, ICO_NONE);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_ABOUT, "&About Disk Management", NULL, ICO_NONE);
    return m;
}

static void context_menu(int rx, int ry)
{
    W2kMenu *m = build_tasks();
    int id = w2k_menu_popup(m, rx, ry, MPOP_LEFT);
    w2k_menu_free(m);
    if (id > 0) command(NULL, id);
}

/* ---- events ---- */
static int event(W2kWin *w, XEvent *e)
{
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_menubar_press(app.mb, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_toolbar_press(app.tb, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        W2kList *top = app.top_pane == PANE_DISKS ? app.dlist : app.list;
        W2kList *bot = app.bottom_pane == PANE_VOLUMES ? app.list : app.bottom_pane == PANE_DISKS ? app.dlist : NULL;
        W2kList *hit = w2k_rect_hit(&app.top, x, y) ? top : (bot && w2k_rect_hit(&app.bottom, x, y)) ? bot : NULL;
        if (hit) {
            app.graph_focus = 0;
            if (hit == app.list) {
                if (w2k_list_press(app.list, &e->xbutton)) { }
                on_list_select(NULL, app.list->sel);
                if (e->xbutton.button == Button3 && app.list->sel >= 0) context_menu(e->xbutton.x_root, e->xbutton.y_root);
            } else {
                w2k_list_press(app.dlist, &e->xbutton);
                if (app.dlist->sel >= 0) { app.sel_disk = app.dlist->sel; app.sel_region = -1; app.graph_focus = 1; }
                if (e->xbutton.button == Button3 && app.sel_disk >= 0) context_menu(e->xbutton.x_root, e->xbutton.y_root);
            }
            w2k_win_dirty(w);
            return 1;
        }
        if (app.bottom_pane == PANE_GRAPH && w2k_rect_hit(&app.bottom, x, y)) {
            if (w2k_scroll_needed(&app.gsb) && (e->xbutton.button == Button4 || e->xbutton.button == Button5)) {
                w2k_scroll_wheel(&app.gsb, e->xbutton.button == Button4 ? -1 : 1);
                w2k_win_dirty(w);
                return 1;
            }
            if (w2k_scroll_needed(&app.gsb) && w2k_rect_hit(&app.gsb.r, x, y)) {
                w2k_scroll_press(&app.gsb, x, y);
                w2k_win_dirty(w);
                return 1;
            }
            int dl;
            int r = region_at(x, y, &dl);
            if (r >= 0) select_region(r);
            else if (dl >= 0) { app.sel_disk = dl; app.sel_region = -1; app.graph_focus = 1; for (int i = 0; i < app.list->n; i++) app.list->items[i].selected = 0; app.list->sel = -1; }
            else { app.sel_region = -1; app.sel_disk = -1; }
            w2k_win_dirty(w);
            if (e->xbutton.button == Button3 && (r >= 0 || dl >= 0)) context_menu(e->xbutton.x_root, e->xbutton.y_root);
            return 1;
        }
        return 1;
    }
    case MotionNotify:
        if (w2k_toolbar_motion(app.tb, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        if (app.gsb.vertical && w2k_scroll_motion(&app.gsb, e->xmotion.x, e->xmotion.y)) { w2k_win_dirty(w); return 1; }
        if (w2k_list_motion(app.list, &e->xmotion) || w2k_list_motion(app.dlist, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease:
        w2k_toolbar_release(app.tb);
        w2k_scroll_release(&app.gsb);
        w2k_list_release(app.list, &e->xbutton);
        w2k_list_release(app.dlist, &e->xbutton);
        w2k_win_dirty(w);
        return 1;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (w2k_menubar_key(app.mb, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (ks == XK_F5) { refresh(); return 1; }
        if (ks == XK_Delete) { command(NULL, ID_DELETE); return 1; }
        if (ks == XK_Return && current_vol()) { command(NULL, ID_PROPERTIES); return 1; }
        if (ks == XK_Menu || (ks == XK_F10 && (e->xkey.state & ShiftMask))) {
            Window rw; int rx, ry, wx, wy; unsigned mask;
            XQueryPointer(w2k.dpy, w->win, &rw, &rw, &rx, &ry, &wx, &wy, &mask);
            context_menu(rx, ry);
            return 1;
        }
        if (!app.graph_focus && w2k_list_key(app.list, &e->xkey)) { on_list_select(NULL, app.list->sel); w2k_win_dirty(w); return 1; }
        if (app.graph_focus && (ks == XK_Left || ks == XK_Right)) {
            int cur = app.sel_region, next = cur;
            if (cur < 0 && nregions) next = 0;
            else if (ks == XK_Right && cur + 1 < nregions) next = cur + 1;
            else if (ks == XK_Left && cur > 0) next = cur - 1;
            select_region(next);
            return 1;
        }
        return 0;
    }
    }
    return 0;
}

static void refresh(void)
{
    scan();
    build_regions();
    build_vols();
    fill_volume_list();
    fill_disk_list();
    if (app.sel_region >= nregions) app.sel_region = -1;
    if (app.sel_disk >= ndisks) app.sel_disk = -1;
    layout(app.win);
    w2k_win_dirty(app.win);
}

static void on_activate(void *u, int idx)
{
    (void)u; (void)idx;
    Vol *v = current_vol();
    if (v && v->mount[0]) open_in_explorer(v->mount, 0);
    else if (v) do_properties(v);
}

int main(int argc, char **argv)
{
    int as_root = elevate(argc, argv);
    if (w2k_init("l2kdiskmgmt") < 0) return 1;
    memset(&app, 0, sizeof app);
    app.sel_region = app.sel_disk = -1;
    app.top_pane = PANE_VOLUMES;
    app.bottom_pane = PANE_GRAPH;
    app.win = w2k_win_new("Disk Management", "l2kdiskmgmt", 800, 540, 1);
    app.win->min_w = 560;
    app.win->min_h = 360;
    app.win->paint = paint;
    app.win->event = event;
    app.win->resized = layout;

    app.mb = w2k_menubar_new(NULL, command);
    app.mb->win_ref = app.win->win;
    w2k_menubar_add(app.mb, "&File", build_file);
    w2k_menubar_add(app.mb, "&Action", build_action);
    w2k_menubar_add(app.mb, "&View", build_view);
    w2k_menubar_add(app.mb, "&Help", build_help);

    app.tb = w2k_toolbar_new(NULL, command);
    w2k_toolbar_add(app.tb, ID_BACK, ICO_BACK, NULL);
    w2k_toolbar_add(app.tb, ID_FORWARD, ICO_FORWARD, NULL);
    w2k_toolbar_sep(app.tb);
    w2k_toolbar_add(app.tb, ID_UP, ICO_UP, NULL);
    w2k_toolbar_sep(app.tb);
    w2k_toolbar_add(app.tb, ID_PROPERTIES, ICO_PROPERTIES, NULL);
    w2k_toolbar_sep(app.tb);
    w2k_toolbar_add(app.tb, ID_HELP, ICO_HELP, NULL);
    w2k_toolbar_enable(app.tb, ID_BACK, 0);
    w2k_toolbar_enable(app.tb, ID_FORWARD, 0);
    w2k_toolbar_enable(app.tb, ID_UP, 0);

    app.list = w2k_list_new(LV_REPORT);
    app.list->fullrow = 1;
    app.list->user = NULL;
    app.list->on_select = on_list_select;
    app.list->on_activate = on_activate;
    w2k_scroll_bind(&app.list->vsb, app.win);
    w2k_scroll_bind(&app.list->hsb, app.win);
    w2k_list_add_col(app.list, "Volume", 150, 0);
    w2k_list_add_col(app.list, "Layout", 62, 0);
    w2k_list_add_col(app.list, "Type", 62, 0);
    w2k_list_add_col(app.list, "File System", 74, 0);
    w2k_list_add_col(app.list, "Status", 170, 0);
    w2k_list_add_col(app.list, "Capacity", 68, 1);
    w2k_list_add_col(app.list, "Free Space", 72, 1);
    w2k_list_add_col(app.list, "% Free", 52, 1);

    app.dlist = w2k_list_new(LV_REPORT);
    app.dlist->fullrow = 1;
    w2k_scroll_bind(&app.dlist->vsb, app.win);
    w2k_scroll_bind(&app.dlist->hsb, app.win);
    w2k_list_add_col(app.dlist, "Disk", 70, 0);
    w2k_list_add_col(app.dlist, "Type", 70, 0);
    w2k_list_add_col(app.dlist, "Capacity", 70, 1);
    w2k_list_add_col(app.dlist, "Unallocated Space", 110, 1);
    w2k_list_add_col(app.dlist, "Status", 64, 0);
    w2k_list_add_col(app.dlist, "Device Type", 170, 0);
    w2k_list_add_col(app.dlist, "Partition Style", 170, 0);

    w2k_scroll_bind(&app.gsb, app.win);
    layout(app.win);
    refresh();
    if (!as_root && !getenv("W2K_RENDER") && !getenv("W2K_FAKE_LSBLK"))
        w2k_msgbox(NULL, "Disk Management",
                   "Disk Management is running without administrator rights: the disks can be "
                   "looked at, but nothing can be changed.\n\nStart it again and give the "
                   "administrator's password when asked (pkexec needs a PolicyKit agent in the "
                   "session, which the desktop starts when one is installed).",
                   MB_OK | MB_ICONWARNING);
    if (nregions) { app.sel_region = 0; }
    /* W2K_RENDER_DIALOG=format|create|init|props, with W2K_RENDER: a
     * picture of that dialog for the first volume, free space or disk. */
    const char *rd = getenv("W2K_RENDER_DIALOG");
    if (rd && getenv("W2K_RENDER")) {
        if (!strcmp(rd, "format") && nvols) do_format(vols[0].path, vols[0].name, vols[0].name, 0);
        else if (!strcmp(rd, "create")) {
            for (int r = 0; r < nregions; r++) if (regions[r].part == -1) { do_create(&regions[r]); break; }
        }
        else if (!strcmp(rd, "init") && ndisks) do_init(&disks[0]);
        else if (!strcmp(rd, "props") && nvols) do_properties(&vols[0]);
    }
    w2k_win_show(app.win);
    w2k_run();
    w2k_list_free(app.list);
    w2k_list_free(app.dlist);
    w2k_menubar_free(app.mb);
    w2k_toolbar_free(app.tb);
    w2k_fini();
    return 0;
}

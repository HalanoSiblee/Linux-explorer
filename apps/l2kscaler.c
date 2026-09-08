/* l2kscaler.c -- the nested compositor: the whole desktop, scaled by us.
 *
 * X11 lets nothing but xrandr's two filters between the framebuffer and a
 * scaled monitor, so the desktop that wants a better picture runs inside
 * a headless X server (Xvfb) at its logical size, and this program shows
 * that server's screen on the real one: one window per monitor, each
 * drawn by the GPU with mpv's EWA Lanczos-sharp -- a polar jinc filter,
 * blurred a hair, in sigmoidised linear light, with a light anti-ringing
 * clamp -- and input on those windows sent back into the nested server
 * with XTest at the matching logical position. The nested server's
 * pointer is drawn here, scaled, since the real one is hidden.
 *
 * Experimental, off unless Display Properties turns it on; l2k-session
 * starts it. Programs in the nested server render in software (Xvfb has
 * no GPU), which a desktop bears and a game does not.
 *
 *   l2kscaler --nested :1 --layout "name,hW,hH,hX,hY,nW,nH,nX,nY;..."
 *             [--nested-window TITLE] [--linear-light] [--window] [--no-input]
 *
 * Each layout entry is one monitor: its window on this display (size and
 * position) and the rectangle of the nested screen it shows; hW/nW is the
 * scale. With --nested-window the nested server is Xephyr and its window
 * on this display carries that title: the screen is then taken straight
 * from that window's pixmap on the GPU (Composite and texture-from-pixmap)
 * and never copied; without it the nested server is Xvfb and the screen
 * is read through shared memory. --linear-light filters in sigmoidised
 * linear light as mpv does, which keeps a photograph honest but makes
 * thin dark text look lighter; the default filters in gamma space, which
 * text weight is drawn for. --window makes ordinary windows instead of
 * full-screen ones and --no-input sends nothing back, for trying it out
 * on a desktop that is already running. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xcomposite.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/select.h>
#include <sys/time.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * GL 2.0 entry points, fetched by name: libGL exports 1.x by contract.
 * ------------------------------------------------------------------ */
typedef char GLchar_;
static GLuint (*p_glCreateShader)(GLenum);
static void   (*p_glShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
static void   (*p_glCompileShader)(GLuint);
static void   (*p_glGetShaderiv)(GLuint, GLenum, GLint *);
static void   (*p_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static GLuint (*p_glCreateProgram)(void);
static void   (*p_glAttachShader)(GLuint, GLuint);
static void   (*p_glLinkProgram)(GLuint);
static void   (*p_glGetProgramiv)(GLuint, GLenum, GLint *);
static void   (*p_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static void   (*p_glUseProgram)(GLuint);
static GLint  (*p_glGetUniformLocation)(GLuint, const char *);
static void   (*p_glUniform1i)(GLint, GLint);
static void   (*p_glUniform1f)(GLint, GLfloat);
static void   (*p_glUniform2f)(GLint, GLfloat, GLfloat);
static void   (*p_glActiveTexture)(GLenum);
static void   (*p_glGenFramebuffers)(GLsizei, GLuint *);
static void   (*p_glBindFramebuffer)(GLenum, GLuint);
static void   (*p_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static GLenum (*p_glCheckFramebufferStatus)(GLenum);
static void   (*p_glDeleteFramebuffers)(GLsizei, const GLuint *);
static void   (*p_glXSwapIntervalEXT)(Display *, GLXDrawable, int);
static void   (*p_glXBindTexImageEXT)(Display *, GLXDrawable, int, const int *);
static void   (*p_glXReleaseTexImageEXT)(Display *, GLXDrawable, int);

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE2 0x84C2
#define GL_TEXTURE3 0x84C3
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#define GETPROC(name) do { p_##name = (void *)glXGetProcAddressARB((const GLubyte *)#name); \
    if (!p_##name) { fprintf(stderr, "l2kscaler: no %s\n", #name); return 0; } } while (0)

static int load_gl(void)
{
    GETPROC(glCreateShader); GETPROC(glShaderSource); GETPROC(glCompileShader);
    GETPROC(glGetShaderiv); GETPROC(glGetShaderInfoLog); GETPROC(glCreateProgram);
    GETPROC(glAttachShader); GETPROC(glLinkProgram); GETPROC(glGetProgramiv);
    GETPROC(glGetProgramInfoLog); GETPROC(glUseProgram); GETPROC(glGetUniformLocation);
    GETPROC(glUniform1i); GETPROC(glUniform1f); GETPROC(glUniform2f); GETPROC(glActiveTexture);
    GETPROC(glGenFramebuffers); GETPROC(glBindFramebuffer); GETPROC(glFramebufferTexture2D);
    GETPROC(glCheckFramebufferStatus); GETPROC(glDeleteFramebuffers);
    p_glXSwapIntervalEXT = (void *)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
    p_glXBindTexImageEXT = (void *)glXGetProcAddressARB((const GLubyte *)"glXBindTexImageEXT");
    p_glXReleaseTexImageEXT = (void *)glXGetProcAddressARB((const GLubyte *)"glXReleaseTexImageEXT");
    return 1;
}

/* ------------------------------------------------------------------ *
 * The filter: EWA Lanczos-sharp as mpv defines it. jinc(x) is the polar
 * sinc, 2 J1(pi x) / (pi x); the window is a jinc stretched so that its
 * first zero sits at the radius, the third zero of the kernel; the
 * whole thing is blurred by 0.9812505644269356, which is what makes it
 * "sharp" (a little less ringing than raw). Baked into a lookup table
 * on the CPU, sampled in the shader by distance.
 * ------------------------------------------------------------------ */
#define EWA_RADIUS 3.2383154841662362
#define EWA_BLUR   0.9812505644269356
#define JINC_ZERO1 1.2196698912665045
#define LUT_N      1024

static double jinc(double x)
{
    if (x < 1e-9) return 1.0;
    double px = M_PI * x;
    return 2.0 * j1(px) / px;
}

static void make_lut(float *lut)
{
    for (int i = 0; i < LUT_N; i++) {
        double d = EWA_RADIUS * i / (LUT_N - 1);   /* distance in source pixels */
        double x = d / EWA_BLUR;
        double k = jinc(x) * jinc(x * JINC_ZERO1 / EWA_RADIUS);
        lut[i] = (float)k;
    }
    lut[LUT_N - 1] = 0.0f;
}

/* sRGB in, sigmoidised linear out (256 entries), and the way back (1024).
 * mpv's sigmoid: centre 0.75, slope 6.5. */
#define SIG_CENTER 0.75
#define SIG_SLOPE  6.5
static double srgb_to_linear(double c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
static double linear_to_srgb(double c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
static void make_sigmoid_luts(float *fwd, float *inv)
{
    double off = 1.0 / (1.0 + exp(SIG_SLOPE * SIG_CENTER));
    double scl = 1.0 / (1.0 + exp(SIG_SLOPE * (SIG_CENTER - 1.0))) - off;
    for (int i = 0; i < 256; i++) {
        double l = srgb_to_linear(i / 255.0);
        fwd[i] = (float)((1.0 / (1.0 + exp(SIG_SLOPE * (SIG_CENTER - l))) - off) / scl);
    }
    for (int i = 0; i < 1024; i++) {
        double y = i / 1023.0;
        double v = y * scl + off;
        if (v <= 1e-6) v = 1e-6;
        if (v >= 1.0 - 1e-6) v = 1.0 - 1e-6;
        double l = SIG_CENTER - log(1.0 / v - 1.0) / SIG_SLOPE;
        if (l < 0) l = 0;
        if (l > 1) l = 1;
        inv[i] = (float)linear_to_srgb(l);
    }
}

static const char *vert_src =
    "#version 120\n"
    "varying vec2 p;\n"                    /* window pixel, y down */
    "void main() { p = gl_MultiTexCoord0.xy; gl_Position = gl_Vertex; }\n";

/* Scaled pass: writes one output pixel from the nested texture. */
static const char *ewa_src =
    "#version 120\n"
    "uniform sampler2D tex;\n"           /* the nested screen, sRGB bytes */
    "uniform sampler1D lut;\n"           /* jinc kernel by distance / radius */
    "uniform sampler1D sig;\n"           /* sRGB byte -> sigmoid linear */
    "uniform sampler1D unsig;\n"         /* sigmoid linear -> sRGB */
    "uniform vec2 texsize;\n"
    "uniform vec2 origin;\n"             /* nested pixel at the window's corner */
    "uniform float scale;\n"             /* host pixels per nested pixel */
    "uniform float radius;\n"
    "uniform float linlight;\n"          /* 1: filter in sigmoidised linear light */
    "uniform float texflip;\n"           /* 1: the texture's row 0 is the bottom */
    "varying vec2 p;\n"
    "vec3 fetch(vec2 j) {\n"
    "    vec2 t = (j + 0.5) / texsize;\n"
    "    if (texflip > 0.5) t.y = 1.0 - t.y;\n"
    "    vec3 c = texture2D(tex, t).rgb;\n"
    "    if (linlight < 0.5) return c;\n"
    "    return vec3(texture1D(sig, c.r).r, texture1D(sig, c.g).r, texture1D(sig, c.b).r);\n"
    "}\n"
    "void main() {\n"
    "    vec2 src = origin + p / scale;\n"          /* continuous nested coords */
    "    vec2 c = src - 0.5;\n"                      /* pixel-index space */
    "    vec2 base = floor(c);\n"
    "    vec3 acc = vec3(0.0); float wsum = 0.0;\n"
    "    for (int dy = -3; dy <= 3; dy++)\n"
    "        for (int dx = -3; dx <= 3; dx++) {\n"
    "            vec2 j = base + vec2(float(dx), float(dy));\n"
    "            float d = distance(j, c);\n"
    "            if (d < radius) {\n"
    "                float w = texture1D(lut, d / radius).r;\n"
    "                acc += w * fetch(j); wsum += w;\n"
    "            }\n"
    "        }\n"
    "    vec3 res = wsum != 0.0 ? acc / wsum : fetch(base);\n"
    /* Anti-ringing: pull the result toward the range of the four pixels
     * around the sample point, most of the way. */
    "    vec3 a = fetch(base), b = fetch(base + vec2(1.0, 0.0)),\n"
    "         cc = fetch(base + vec2(0.0, 1.0)), d4 = fetch(base + vec2(1.0, 1.0));\n"
    "    vec3 lo = min(min(a, b), min(cc, d4)), hi = max(max(a, b), max(cc, d4));\n"
    "    res = mix(res, clamp(res, lo, hi), 0.8);\n"
    "    res = clamp(res, 0.0, 1.0);\n"
    "    if (linlight < 0.5) { gl_FragColor = vec4(res, 1.0); return; }\n"
    "    gl_FragColor = vec4(texture1D(unsig, res.r).r, texture1D(unsig, res.g).r,\n"
    "                        texture1D(unsig, res.b).r, 1.0);\n"
    "}\n";

/* Plain pass: a texture rectangle, as is. Used for 1:1 monitors, for
 * presenting the scaled framebuffer, and for the cursor (blended). */
static const char *blit_src =
    "#version 120\n"
    "uniform sampler2D tex;\n"
    "uniform vec2 texsize;\n"
    "uniform vec2 origin;\n"
    "uniform float scale;\n"
    "uniform float flipy;\n"            /* 1: the texture is stored bottom-up */
    "varying vec2 p;\n"
    "void main() {\n"
    "    vec2 q = origin + p / scale;\n"
    "    if (flipy > 0.5) q.y = texsize.y - q.y;\n"
    "    gl_FragColor = texture2D(tex, q / texsize);\n"
    "}\n";

/* ------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------ */
typedef struct {
    char   name[64];
    int    hw, hh, hx, hy;          /* the window on this display */
    int    nw, nh, nx, ny;          /* the nested rectangle it shows */
    double scale;
    Window win;
    GLuint fbo, fbo_tex;            /* the scaled picture, kept (scale != 1) */
    int    dirty;                   /* needs presenting */
    int    fbo_x0, fbo_y0, fbo_x1, fbo_y1;   /* pending EWA rect, host px */
} Mon;

static Display *hd, *nd;            /* host, nested */
static int hscreen, nscreen;
static Window nroot;
static int NW, NH;                  /* nested screen size */
static Mon mons[8];
static int nmons;
static int windowed, no_input;   /* --window and --no-input: for trying it out */
static int linear_light;         /* --linear-light */
static const char *nested_title; /* --nested-window: Xephyr's window on this display */
static Window xwin;              /* that window */
static Pixmap xpix;              /* its pixmap, named by Composite */
static GLXPixmap xglx;           /* bound to desk_tex */
static int tex_flip;             /* the bound texture's row 0 is the bottom */
static Damage hdamage;           /* damage on that window, on this display */
static int hdamage_base;
static volatile sig_atomic_t quit;

static GLXContext ctx;
static XVisualInfo *vi;
static GLuint desk_tex, lut_tex, sig_tex, unsig_tex, cur_tex;
static GLuint prog_ewa, prog_blit;
static XShmSegmentInfo shm;
static XImage *shmimg;
static Damage damage;
static int damage_base, xfixes_base;
static int dmg_x0 = 0, dmg_y0 = 0, dmg_x1 = 0, dmg_y1 = 0, dmg_any = 1;
static int cur_w, cur_h, cur_xhot, cur_yhot, cur_valid;
static int ptr_x = -1, ptr_y = -1;

static void on_signal(int s) { (void)s; quit = 1; }

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static int xerror(Display *d, XErrorEvent *e)
{
    char buf[128];
    XGetErrorText(d, e->error_code, buf, sizeof buf);
    fprintf(stderr, "l2kscaler: X error: %s (request %d.%d)\n", buf,
            e->request_code, e->minor_code);
    return 0;
}

/* ------------------------------------------------------------------ *
 * Layout
 * ------------------------------------------------------------------ */
static int parse_layout(const char *s)
{
    nmons = 0;
    char *dup = strdup(s), *save = NULL;
    for (char *tok = strtok_r(dup, ";", &save); tok && nmons < 8; tok = strtok_r(NULL, ";", &save)) {
        Mon *m = &mons[nmons];
        memset(m, 0, sizeof *m);
        char name[64];
        if (sscanf(tok, "%63[^,],%d,%d,%d,%d,%d,%d,%d,%d", name, &m->hw, &m->hh, &m->hx, &m->hy,
                   &m->nw, &m->nh, &m->nx, &m->ny) != 9 || m->hw < 1 || m->hh < 1 || m->nw < 1 || m->nh < 1) {
            fprintf(stderr, "l2kscaler: bad layout entry \"%s\"\n", tok);
            free(dup);
            return 0;
        }
        snprintf(m->name, sizeof m->name, "%s", name);
        m->scale = (double)m->hw / m->nw;
        nmons++;
    }
    free(dup);
    return nmons > 0;
}

/* ------------------------------------------------------------------ *
 * GL setup
 * ------------------------------------------------------------------ */
static GLuint compile(GLenum kind, const char *src)
{
    GLuint sh = p_glCreateShader(kind);
    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    GLint ok = 0;
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "l2kscaler: shader: %s\n", log);
        return 0;
    }
    return sh;
}

static GLuint link_program(const char *frag)
{
    GLuint v = compile(GL_VERTEX_SHADER, vert_src), f = compile(GL_FRAGMENT_SHADER, frag);
    if (!v || !f) return 0;
    GLuint pr = p_glCreateProgram();
    p_glAttachShader(pr, v);
    p_glAttachShader(pr, f);
    p_glLinkProgram(pr);
    GLint ok = 0;
    p_glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetProgramInfoLog(pr, sizeof log, NULL, log);
        fprintf(stderr, "l2kscaler: link: %s\n", log);
        return 0;
    }
    return pr;
}

static GLuint tex1d(const float *data, int n)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_1D, t);
    glTexImage1D(GL_TEXTURE_1D, 0, GL_LUMINANCE32F_ARB, n, 0, GL_LUMINANCE, GL_FLOAT, data);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    return t;
}

static GLuint tex2d(int w, int h, GLenum filter)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static int gl_init(void)
{
    int attrs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None };
    vi = glXChooseVisual(hd, hscreen, attrs);
    if (!vi) { fprintf(stderr, "l2kscaler: no double-buffered RGBA visual\n"); return 0; }
    ctx = glXCreateContext(hd, vi, NULL, True);
    if (!ctx) { fprintf(stderr, "l2kscaler: cannot create a GL context\n"); return 0; }
    return 1;
}

static void mon_window(Mon *m)
{
    XSetWindowAttributes a;
    a.colormap = XCreateColormap(hd, RootWindow(hd, hscreen), vi->visual, AllocNone);
    a.override_redirect = !windowed;
    a.background_pixel = BlackPixel(hd, hscreen);
    a.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                   PointerMotionMask | EnterWindowMask | LeaveWindowMask | ExposureMask |
                   StructureNotifyMask | FocusChangeMask;
    m->win = XCreateWindow(hd, RootWindow(hd, hscreen), m->hx, m->hy, (unsigned)m->hw, (unsigned)m->hh,
                           0, vi->depth, InputOutput, vi->visual,
                           CWColormap | CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
    char title[128];
    snprintf(title, sizeof title, "Linux 2000 (%s)", m->name);
    XStoreName(hd, m->win, title);
    XClassHint ch = { "l2kscaler", "l2kscaler" };
    XSetClassHint(hd, m->win, &ch);
    XMapRaised(hd, m->win);
    XFixesHideCursor(hd, m->win);
}

static void gl_after_context(void)
{
    /* One vertical-blank wait per frame, not one per window: the first
     * scaled window (or the first) syncs, the rest are told not to. */
    if (p_glXSwapIntervalEXT) {
        int synced = -1;
        for (int i = 0; i < nmons && synced < 0; i++)
            if (fabs(mons[i].scale - 1.0) > 1e-6) synced = i;
        if (synced < 0) synced = 0;
        for (int i = 0; i < nmons; i++) {
            glXMakeCurrent(hd, mons[i].win, ctx);
            p_glXSwapIntervalEXT(hd, mons[i].win, i == synced ? 1 : 0);
        }
        glXMakeCurrent(hd, mons[0].win, ctx);
    }
    float lut[LUT_N], sfwd[256], sinv[1024];
    make_lut(lut);
    make_sigmoid_luts(sfwd, sinv);
    lut_tex = tex1d(lut, LUT_N);
    sig_tex = tex1d(sfwd, 256);
    unsig_tex = tex1d(sinv, 1024);
    desk_tex = tex2d(NW, NH, GL_NEAREST);
    cur_tex = tex2d(1, 1, GL_LINEAR);
    prog_ewa = link_program(ewa_src);
    prog_blit = link_program(blit_src);
    for (int i = 0; i < nmons; i++) {
        Mon *m = &mons[i];
        if (fabs(m->scale - 1.0) < 1e-6) continue;
        m->fbo_tex = tex2d(m->hw, m->hh, GL_NEAREST);
        p_glGenFramebuffers(1, &m->fbo);
        p_glBindFramebuffer(GL_FRAMEBUFFER, m->fbo);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m->fbo_tex, 0);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "l2kscaler: framebuffer for %s incomplete\n", m->name);
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m->fbo_x0 = 0; m->fbo_y0 = 0; m->fbo_x1 = m->hw; m->fbo_y1 = m->hh;
    }
}

/* A quad over the window rectangle (x0,y0)-(x1,y1) in pixels, y down,
 * carrying the same coordinates as texcoord for the shaders. */
static void quad(int x0, int y0, int x1, int y1, int vw, int vh)
{
    float fx0 = 2.0f * x0 / vw - 1.0f, fx1 = 2.0f * x1 / vw - 1.0f;
    float fy0 = 1.0f - 2.0f * y0 / vh, fy1 = 1.0f - 2.0f * y1 / vh;
    glBegin(GL_QUADS);
    glTexCoord2f((float)x0, (float)y0); glVertex2f(fx0, fy0);
    glTexCoord2f((float)x1, (float)y0); glVertex2f(fx1, fy0);
    glTexCoord2f((float)x1, (float)y1); glVertex2f(fx1, fy1);
    glTexCoord2f((float)x0, (float)y1); glVertex2f(fx0, fy1);
    glEnd();
}

static void bind_common(GLuint prog, GLuint tex, int tw, int th, double ox, double oy, double scale, int flipy)
{
    p_glUseProgram(prog);
    if (prog == prog_blit) p_glUniform1f(p_glGetUniformLocation(prog, "flipy"), flipy ? 1.0f : 0.0f);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    p_glUniform1i(p_glGetUniformLocation(prog, "tex"), 0);
    p_glUniform2f(p_glGetUniformLocation(prog, "texsize"), (float)tw, (float)th);
    p_glUniform2f(p_glGetUniformLocation(prog, "origin"), (float)ox, (float)oy);
    p_glUniform1f(p_glGetUniformLocation(prog, "scale"), (float)scale);
}

/* ------------------------------------------------------------------ *
 * The nested screen: damage in, texture out
 * ------------------------------------------------------------------ */
/* Xephyr's window on this display, by title. */
static Window find_window(Window root, const char *title, int depth)
{
    Window rr, parent, *kids = NULL;
    unsigned n = 0;
    if (!XQueryTree(hd, root, &rr, &parent, &kids, &n)) return 0;
    Window found = 0;
    for (unsigned i = 0; i < n && !found; i++) {
        char *name = NULL;
        if (XFetchName(hd, kids[i], &name) && name) {
            if (!strcmp(name, title)) found = kids[i];
            XFree(name);
        }
        if (!found && depth < 3) found = find_window(kids[i], title, depth + 1);
    }
    if (kids) XFree(kids);
    return found;
}

/* Take the window's pixmap as our screen texture: no copy, the GPU reads
 * what Xephyr drew. Done again whenever the window changes size. */
static void tfp_bind(void)
{
    if (xglx) { p_glXReleaseTexImageEXT(hd, xglx, GLX_FRONT_LEFT_EXT); glXDestroyPixmap(hd, xglx); xglx = 0; }
    if (xpix) { XFreePixmap(hd, xpix); xpix = 0; }
    xpix = XCompositeNameWindowPixmap(hd, xwin);
    int attrs[] = { GLX_BIND_TO_TEXTURE_RGB_EXT, True, GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
                    GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT, GLX_DOUBLEBUFFER, False,
                    GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None };
    int n = 0;
    GLXFBConfig *fbc = glXChooseFBConfig(hd, hscreen, attrs, &n);
    if (!fbc || !n) { fprintf(stderr, "l2kscaler: no framebuffer config binds a pixmap to a texture\n"); exit(1); }
    /* The one whose depth is the window's. */
    XWindowAttributes wa;
    XGetWindowAttributes(hd, xwin, &wa);
    NW = wa.width; NH = wa.height;         /* the window is the nested screen */
    GLXFBConfig pick = fbc[0];
    for (int i = 0; i < n; i++) {
        XVisualInfo *v = glXGetVisualFromFBConfig(hd, fbc[i]);
        if (v && v->depth == wa.depth) { pick = fbc[i]; XFree(v); break; }
        if (v) XFree(v);
    }
    XFree(fbc);
    int pattrs[] = { GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
                     GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGB_EXT, None };
    xglx = glXCreatePixmap(hd, pick, xpix, pattrs);
    unsigned inv = 0;
    glXQueryDrawable(hd, xglx, GLX_Y_INVERTED_EXT, &inv);
    tex_flip = inv != 0;                   /* as Mesa binds them: rows from the bottom */
    glBindTexture(GL_TEXTURE_2D, desk_tex);
    p_glXBindTexImageEXT(hd, xglx, GLX_FRONT_LEFT_EXT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

static int tfp_init(void)
{
    if (!p_glXBindTexImageEXT || !p_glXReleaseTexImageEXT) {
        fprintf(stderr, "l2kscaler: no GLX_EXT_texture_from_pixmap\n");
        return 0;
    }
    int ev, err;
    if (!XCompositeQueryExtension(hd, &ev, &err)) { fprintf(stderr, "l2kscaler: this server has no Composite\n"); return 0; }
    if (!XDamageQueryExtension(hd, &hdamage_base, &err)) { fprintf(stderr, "l2kscaler: this server has no DAMAGE\n"); return 0; }
    for (int tries = 0; tries < 100 && !xwin; tries++) {
        xwin = find_window(RootWindow(hd, hscreen), nested_title, 0);
        if (!xwin) usleep(100000);
    }
    if (!xwin) { fprintf(stderr, "l2kscaler: no window called \"%s\" on this display\n", nested_title); return 0; }
    XCompositeRedirectWindow(hd, xwin, CompositeRedirectAutomatic);
    XSelectInput(hd, xwin, StructureNotifyMask);
    hdamage = XDamageCreate(hd, xwin, XDamageReportBoundingBox);
    XSync(hd, False);
    return 1;
}

static int shm_init(void)
{
    if (!XShmQueryExtension(nd)) { fprintf(stderr, "l2kscaler: the nested server has no MIT-SHM\n"); return 0; }
    shmimg = XShmCreateImage(nd, DefaultVisual(nd, nscreen), (unsigned)DefaultDepth(nd, nscreen),
                             ZPixmap, NULL, &shm, (unsigned)NW, (unsigned)NH);
    if (!shmimg || shmimg->bits_per_pixel != 32) {
        fprintf(stderr, "l2kscaler: the nested server is not 32 bits per pixel\n");
        return 0;
    }
    shm.shmid = shmget(IPC_PRIVATE, (size_t)shmimg->bytes_per_line * NH, IPC_CREAT | 0600);
    if (shm.shmid < 0) { perror("shmget"); return 0; }
    shm.shmaddr = shmimg->data = shmat(shm.shmid, NULL, 0);
    shm.readOnly = False;
    if (!XShmAttach(nd, &shm)) { fprintf(stderr, "l2kscaler: XShmAttach failed\n"); return 0; }
    XSync(nd, False);
    shmctl(shm.shmid, IPC_RMID, NULL);      /* freed when both detach */
    return 1;
}

/* Fetch the damaged rectangle of the nested root into the texture. The
 * image is told the rectangle's size for the call: the server writes
 * rows of exactly that width. */
static void fetch_damage(void)
{
    int x0 = dmg_x0, y0 = dmg_y0, x1 = dmg_x1, y1 = dmg_y1;
    if (!dmg_any) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > NW) x1 = NW;
    if (y1 > NH) y1 = NH;
    dmg_any = 0;
    if (x1 <= x0 || y1 <= y0) return;
    if (xwin) {
        /* The texture is the pixmap: let go and take it again, which is
         * what the extension asks of a reader after the drawer has drawn. */
        glBindTexture(GL_TEXTURE_2D, desk_tex);
        p_glXReleaseTexImageEXT(hd, xglx, GLX_FRONT_LEFT_EXT);
        p_glXBindTexImageEXT(hd, xglx, GLX_FRONT_LEFT_EXT, NULL);
    } else {
        int rw = x1 - x0, rh = y1 - y0;
        shmimg->width = rw; shmimg->height = rh; shmimg->bytes_per_line = rw * 4;
        if (!XShmGetImage(nd, nroot, shmimg, x0, y0, AllPlanes)) return;
        glBindTexture(GL_TEXTURE_2D, desk_tex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, rw);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, rw, rh, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, shmimg->data);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    /* Which windows it touches, and where in their scaled picture. */
    for (int i = 0; i < nmons; i++) {
        Mon *m = &mons[i];
        int ax0 = x0 > m->nx ? x0 : m->nx, ay0 = y0 > m->ny ? y0 : m->ny;
        int ax1 = x1 < m->nx + m->nw ? x1 : m->nx + m->nw, ay1 = y1 < m->ny + m->nh ? y1 : m->ny + m->nh;
        if (ax1 <= ax0 || ay1 <= ay0) continue;
        m->dirty = 1;
        if (!m->fbo) continue;
        /* The filter reaches four source pixels around a sample: widen. */
        int hx0 = (int)floor((ax0 - m->nx - 4) * m->scale), hy0 = (int)floor((ay0 - m->ny - 4) * m->scale);
        int hx1 = (int)ceil((ax1 - m->nx + 4) * m->scale), hy1 = (int)ceil((ay1 - m->ny + 4) * m->scale);
        if (hx0 < 0) hx0 = 0;
        if (hy0 < 0) hy0 = 0;
        if (hx1 > m->hw) hx1 = m->hw;
        if (hy1 > m->hh) hy1 = m->hh;
        if (m->fbo_x1 <= m->fbo_x0) { m->fbo_x0 = hx0; m->fbo_y0 = hy0; m->fbo_x1 = hx1; m->fbo_y1 = hy1; }
        else {
            if (hx0 < m->fbo_x0) m->fbo_x0 = hx0;
            if (hy0 < m->fbo_y0) m->fbo_y0 = hy0;
            if (hx1 > m->fbo_x1) m->fbo_x1 = hx1;
            if (hy1 > m->fbo_y1) m->fbo_y1 = hy1;
        }
    }
}

static void fetch_cursor(void)
{
    XFixesCursorImage *ci = XFixesGetCursorImage(nd);
    if (!ci) { cur_valid = 0; return; }
    cur_w = ci->width; cur_h = ci->height; cur_xhot = ci->xhot; cur_yhot = ci->yhot;
    unsigned *px = malloc((size_t)cur_w * cur_h * 4);
    if (px) {
        for (int i = 0; i < cur_w * cur_h; i++) px[i] = (unsigned)ci->pixels[i];   /* ARGB, premultiplied */
        glBindTexture(GL_TEXTURE_2D, cur_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cur_w, cur_h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
        free(px);
        cur_valid = cur_w > 0 && cur_h > 0;
    }
    XFree(ci);
    for (int i = 0; i < nmons; i++) mons[i].dirty = 1;
}

/* ------------------------------------------------------------------ *
 * Drawing
 * ------------------------------------------------------------------ */
static void draw_mon(Mon *m)
{
    glXMakeCurrent(hd, m->win, ctx);
    if (m->fbo && m->fbo_x1 > m->fbo_x0 && m->fbo_y1 > m->fbo_y0) {
        /* The scaled pass, only where the nested screen changed. */
        p_glBindFramebuffer(GL_FRAMEBUFFER, m->fbo);
        glViewport(0, 0, m->hw, m->hh);
        bind_common(prog_ewa, desk_tex, NW, NH, m->nx, m->ny, m->scale, 0);
        p_glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_1D, lut_tex);
        p_glUniform1i(p_glGetUniformLocation(prog_ewa, "lut"), 1);
        p_glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_1D, sig_tex);
        p_glUniform1i(p_glGetUniformLocation(prog_ewa, "sig"), 2);
        p_glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_1D, unsig_tex);
        p_glUniform1i(p_glGetUniformLocation(prog_ewa, "unsig"), 3);
        p_glUniform1f(p_glGetUniformLocation(prog_ewa, "radius"), (float)EWA_RADIUS);
        p_glUniform1f(p_glGetUniformLocation(prog_ewa, "linlight"), linear_light ? 1.0f : 0.0f);
        p_glUniform1f(p_glGetUniformLocation(prog_ewa, "texflip"), tex_flip ? 1.0f : 0.0f);
        p_glActiveTexture(GL_TEXTURE0);
        glEnable(GL_SCISSOR_TEST);
        /* The framebuffer texture is y-up; our quad is y-down, so the
         * picture lands flipped in the texture and is flipped back when
         * presented (origin at the bottom). Scissor in texture rows. */
        glScissor(m->fbo_x0, m->hh - m->fbo_y1, m->fbo_x1 - m->fbo_x0, m->fbo_y1 - m->fbo_y0);
        quad(m->fbo_x0, m->fbo_y0, m->fbo_x1, m->fbo_y1, m->hw, m->hh);
        glDisable(GL_SCISSOR_TEST);
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m->fbo_x0 = m->fbo_y0 = m->fbo_x1 = m->fbo_y1 = 0;
    }
    glViewport(0, 0, m->hw, m->hh);
    glDisable(GL_BLEND);
    if (m->fbo) {
        /* Present the kept picture. It sits upside down in the texture
         * (see above): sample with the rows reversed. */
        bind_common(prog_blit, m->fbo_tex, m->hw, m->hh, 0, 0, 1.0, 1);
        quad(0, 0, m->hw, m->hh, m->hw, m->hh);
    } else {
        bind_common(prog_blit, desk_tex, NW, NH, m->nx, m->ny, 1.0, tex_flip);
        quad(0, 0, m->hw, m->hh, m->hw, m->hh);
    }
    /* The nested pointer, where the nested server says it is. */
    if (cur_valid && ptr_x >= 0) {
        int cx = ptr_x - cur_xhot, cy = ptr_y - cur_yhot;
        if (cx + cur_w > m->nx && cy + cur_h > m->ny && cx < m->nx + m->nw && cy < m->ny + m->nh) {
            double s = m->scale;
            int x0 = (int)lround((cx - m->nx) * s), y0 = (int)lround((cy - m->ny) * s);
            int x1 = x0 + (int)lround(cur_w * s), y1 = y0 + (int)lround(cur_h * s);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);       /* premultiplied */
            bind_common(prog_blit, cur_tex, cur_w, cur_h, -x0 / s, -y0 / s, s, 0);
            quad(x0, y0, x1, y1, m->hw, m->hh);
            glDisable(GL_BLEND);
        }
    }
    glXSwapBuffers(hd, m->win);
    m->dirty = 0;
}

/* ------------------------------------------------------------------ *
 * Input: host events become XTest events in the nested server
 * ------------------------------------------------------------------ */
static void pointer_to(Mon *m, int x, int y)
{
    if (no_input) return;
    int nx = m->nx + (int)floor(x / m->scale), ny = m->ny + (int)floor(y / m->scale);
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx >= NW) nx = NW - 1;
    if (ny >= NH) ny = NH - 1;
    XTestFakeMotionEvent(nd, nscreen, nx, ny, CurrentTime);
    XFlush(nd);
}

static Mon *mon_of(Window w)
{
    for (int i = 0; i < nmons; i++) if (mons[i].win == w) return &mons[i];
    return NULL;
}

static void host_event(XEvent *e)
{
    if (xwin && e->type == hdamage_base + XDamageNotify) {
        XDamageNotifyEvent *d = (XDamageNotifyEvent *)e;
        int x0 = d->area.x, y0 = d->area.y, x1 = x0 + d->area.width, y1 = y0 + d->area.height;
        if (!dmg_any) { dmg_x0 = x0; dmg_y0 = y0; dmg_x1 = x1; dmg_y1 = y1; dmg_any = 1; }
        else {
            if (x0 < dmg_x0) dmg_x0 = x0;
            if (y0 < dmg_y0) dmg_y0 = y0;
            if (x1 > dmg_x1) dmg_x1 = x1;
            if (y1 > dmg_y1) dmg_y1 = y1;
        }
        return;
    }
    if (xwin && e->xany.window == xwin) {
        if (e->type == ConfigureNotify) {
            glXMakeCurrent(hd, mons[0].win, ctx);
            tfp_bind();
            dmg_x0 = 0; dmg_y0 = 0; dmg_x1 = NW; dmg_y1 = NH; dmg_any = 1;
        }
        return;
    }
    Mon *m = mon_of(e->xany.window);
    if (!m) return;
    switch (e->type) {
    case MotionNotify:
        pointer_to(m, e->xmotion.x, e->xmotion.y);
        break;
    case EnterNotify:
        pointer_to(m, e->xcrossing.x, e->xcrossing.y);
        XSetInputFocus(hd, m->win, RevertToPointerRoot, CurrentTime);
        break;
    case ButtonPress:
    case ButtonRelease:
        if (no_input) break;
        pointer_to(m, e->xbutton.x, e->xbutton.y);
        XTestFakeButtonEvent(nd, e->xbutton.button, e->type == ButtonPress, CurrentTime);
        XFlush(nd);
        break;
    case KeyPress:
    case KeyRelease:
        if (no_input) break;
        XTestFakeKeyEvent(nd, e->xkey.keycode, e->type == KeyPress, CurrentTime);
        XFlush(nd);
        break;
    case Expose:
    case MapNotify:
        m->dirty = 1;
        break;
    case ConfigureNotify:
        if (windowed && (e->xconfigure.width != m->hw || e->xconfigure.height != m->hh)) {
            /* Trying it in a window: the window's size is the monitor's. */
            m->hw = e->xconfigure.width; m->hh = e->xconfigure.height;
            m->scale = (double)m->hw / m->nw;
            if (m->fbo) {
                glXMakeCurrent(hd, m->win, ctx);
                glBindTexture(GL_TEXTURE_2D, m->fbo_tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m->hw, m->hh, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
                m->fbo_x0 = 0; m->fbo_y0 = 0; m->fbo_x1 = m->hw; m->fbo_y1 = m->hh;
            }
            m->dirty = 1;
        }
        break;
    }
}

static void nested_event(XEvent *e)
{
    if (e->type == damage_base + XDamageNotify) {
        XDamageNotifyEvent *d = (XDamageNotifyEvent *)e;
        int x0 = d->area.x, y0 = d->area.y, x1 = x0 + d->area.width, y1 = y0 + d->area.height;
        if (!dmg_any) { dmg_x0 = x0; dmg_y0 = y0; dmg_x1 = x1; dmg_y1 = y1; dmg_any = 1; }
        else {
            if (x0 < dmg_x0) dmg_x0 = x0;
            if (y0 < dmg_y0) dmg_y0 = y0;
            if (x1 > dmg_x1) dmg_x1 = x1;
            if (y1 > dmg_y1) dmg_y1 = y1;
        }
    } else if (e->type == xfixes_base + XFixesCursorNotify) {
        fetch_cursor();
    }
}

/* ------------------------------------------------------------------ *
 * Main
 * ------------------------------------------------------------------ */
static void usage(void)
{
    fprintf(stderr, "usage: l2kscaler --nested DISPLAY --layout \"name,hW,hH,hX,hY,nW,nH,nX,nY;...\" [--nested-window TITLE] [--linear-light] [--window] [--no-input]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *nested = NULL, *layout = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--nested") && i + 1 < argc) nested = argv[++i];
        else if (!strcmp(argv[i], "--layout") && i + 1 < argc) layout = argv[++i];
        else if (!strcmp(argv[i], "--window")) windowed = 1;
        else if (!strcmp(argv[i], "--no-input")) no_input = 1;
        else if (!strcmp(argv[i], "--linear-light")) linear_light = 1;
        else if (!strcmp(argv[i], "--nested-window") && i + 1 < argc) nested_title = argv[++i];
        else usage();
    }
    if (!nested || !layout || !parse_layout(layout)) usage();

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);

    hd = XOpenDisplay(NULL);
    if (!hd) { fprintf(stderr, "l2kscaler: cannot open the display\n"); return 1; }
    nd = XOpenDisplay(nested);
    if (!nd) { fprintf(stderr, "l2kscaler: cannot open the nested display %s\n", nested); return 1; }
    XSetErrorHandler(xerror);
    hscreen = DefaultScreen(hd);
    nscreen = DefaultScreen(nd);
    nroot = RootWindow(nd, nscreen);
    NW = DisplayWidth(nd, nscreen);
    NH = DisplayHeight(nd, nscreen);

    int ev, err, xtest_maj, xtest_min, xtest_ev, xtest_err;
    if (!XDamageQueryExtension(nd, &damage_base, &err)) { fprintf(stderr, "l2kscaler: the nested server has no DAMAGE\n"); return 1; }
    if (!XFixesQueryExtension(nd, &xfixes_base, &err)) { fprintf(stderr, "l2kscaler: the nested server has no XFIXES\n"); return 1; }
    if (!XTestQueryExtension(nd, &xtest_ev, &xtest_err, &xtest_maj, &xtest_min)) { fprintf(stderr, "l2kscaler: the nested server has no XTEST\n"); return 1; }
    if (!XFixesQueryExtension(hd, &ev, &err)) { fprintf(stderr, "l2kscaler: this server has no XFIXES\n"); return 1; }
    if (!nested_title && !shm_init()) return 1;
    if (!load_gl() || !gl_init()) return 1;

    for (int i = 0; i < nmons; i++) mon_window(&mons[i]);
    XSync(hd, False);
    glXMakeCurrent(hd, mons[0].win, ctx);
    gl_after_context();
    if (!prog_ewa || !prog_blit) return 1;
    if (nested_title) {
        if (!tfp_init()) return 1;
        tfp_bind();
        for (int i = 0; i < nmons; i++) XRaiseWindow(hd, mons[i].win);
    }
    XSetInputFocus(hd, mons[0].win, RevertToPointerRoot, CurrentTime);

    if (!xwin) damage = XDamageCreate(nd, nroot, XDamageReportBoundingBox);
    XFixesSelectCursorInput(nd, nroot, XFixesDisplayCursorNotifyMask);
    dmg_x0 = 0; dmg_y0 = 0; dmg_x1 = NW; dmg_y1 = NH; dmg_any = 1;
    fetch_cursor();

    int hfd = ConnectionNumber(hd), nfd = ConnectionNumber(nd);
    long last_frame = 0;
    while (!quit) {
        while (XPending(hd)) { XEvent e; XNextEvent(hd, &e); host_event(&e); }
        while (XPending(nd)) { XEvent e; XNextEvent(nd, &e); nested_event(&e); }
        int any_dirty = dmg_any;
        for (int i = 0; i < nmons && !any_dirty; i++) any_dirty |= mons[i].dirty;
        /* The nested pointer moves without an event of its own when a
         * program warps it: ask every frame that draws. */
        long t = now_ms();
        if (any_dirty && t - last_frame >= 8) {
            last_frame = t;
            if (xwin) XDamageSubtract(hd, hdamage, None, None);
            else      XDamageSubtract(nd, damage, None, None);
            glXMakeCurrent(hd, mons[0].win, ctx);
            fetch_damage();
            Window rr, cw; int rx, ry, wx, wy; unsigned mask;
            if (XQueryPointer(nd, nroot, &rr, &cw, &rx, &ry, &wx, &wy, &mask) && (rx != ptr_x || ry != ptr_y)) {
                ptr_x = rx; ptr_y = ry;
                for (int i = 0; i < nmons; i++) mons[i].dirty = 1;
            }
            for (int i = 0; i < nmons; i++)
                if (mons[i].dirty) draw_mon(&mons[i]);
            continue;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(hfd, &fds);
        FD_SET(nfd, &fds);
        struct timeval tv = { 0, any_dirty ? 4000 : 16000 };
        select((hfd > nfd ? hfd : nfd) + 1, &fds, NULL, NULL, &tv);
        if (!any_dirty) {
            /* Idle: still follow a pointer warped by a program, at 60 Hz. */
            Window rr, cw; int rx, ry, wx, wy; unsigned mask;
            if (XQueryPointer(nd, nroot, &rr, &cw, &rx, &ry, &wx, &wy, &mask) && (rx != ptr_x || ry != ptr_y)) {
                ptr_x = rx; ptr_y = ry;
                for (int i = 0; i < nmons; i++) mons[i].dirty = 1;
            }
        }
    }
    if (xwin) { XDamageDestroy(hd, hdamage); XCompositeUnredirectWindow(hd, xwin, CompositeRedirectAutomatic); }
    else { XDamageDestroy(nd, damage); XShmDetach(nd, &shm); shmdt(shm.shmaddr); }
    XCloseDisplay(nd);
    XCloseDisplay(hd);
    return 0;
}

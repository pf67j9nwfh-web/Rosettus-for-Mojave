/* oah_cgs.h - guest-toolbox mode (OAH_GTB=1).
   The guest HIToolbox, QuickDraw and CoreGraphics run as PowerPC code, as under
   the real Rosetta. Only their link to the WindowServer is translated: the
   guest CGS* calls are marked in the overlay CoreGraphics (gtb/, CoreGraphicsG
   lines in bridges.spec) and answered here with Mojave's native SkyLight.
   Connection and window IDs are plain integers, valid on both sides.

   Window backing: each window gets a shim-owned pixel store (big-endian ARGB,
   32 bpp, the layout QuickDraw and the guest CG use). QuickDraw reaches it
   through CGSLockWindowBits; the guest CG through CGWindowContextCreate, which
   returns a guest bitmap context on the same store. Flushes copy the store
   into the native window with Mojave's CoreGraphics.
   Store layout: g_cgs_rows rows of g_cgs_rb bytes. A window of height h uses
   the LAST h rows, so a bitmap context made once over the whole store keeps
   its origin (bottom-left) on the window's bottom row after any resize. */
static int g_gtb = -1;
static int oah_gtb(void) {
    if (g_gtb < 0) g_gtb = getenv("OAH_GTB") ? 1 : 0;
    return g_gtb;
}
static int g_cgstrace = -1;
#define CGSLOG(...) do { if (g_cgstrace < 0) g_cgstrace = (getenv("OAH_CGSTRACE") || access("/tmp/oah_cgstrace", 0) == 0) ? 1 : 0; \
                         if (g_cgstrace) fprintf(stderr, __VA_ARGS__); } while (0)
static void *g_sky, *g_ncg;
static void *oah_ncgs(const char *n) {
    void *f;
    char b[128];
    if (!g_ncg) g_ncg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_sky) g_sky = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY | RTLD_GLOBAL);
    f = g_ncg ? dlsym(g_ncg, n) : 0;
    if (!f && g_sky && !strncmp(n, "CGS", 3)) { snprintf(b, sizeof b, "SLS%s", n + 3); f = dlsym(g_sky, b); }
    if (!f && g_sky && !strncmp(n, "CG", 2)) { snprintf(b, sizeof b, "SL%s", n + 2); f = dlsym(g_sky, b); }
    if (!f) fprintf(stderr, "[cgs] native %s not found\n", n);
    return f;
}
#define NCGS(ret, name, args) static ret (*p_##name) args; if (!p_##name) p_##name = (ret (*) args)oah_ncgs(#name)

typedef struct { float x, y, w, h; } oah_rect_t;
typedef struct { float x, y; } oah_pt_t;
typedef struct { double x, y; } oah_npt_t;     /* native 64-bit CGPoint */

static unsigned int g_ncid;
static void oah_dnd_start(void);   /* oah_dnd.h: files dropped on our windows */
static unsigned int g_menubar_wid;          /* the guest's menu bar window (CGSRegisterMenuBarWindow) */
static int g_cgs_rows, g_cgs_rb;            /* backing store geometry (all windows) */
static oah_rect_t g_cgs_desk;               /* union of display bounds */

static void oah_put32(unsigned int gp, unsigned int v) { if (gp) *(unsigned int *)gp = OSSwapHostToBigInt32(v); }
static unsigned int oah_get32(unsigned int gp) { return gp ? OSSwapBigToHostInt32(*(unsigned int *)gp) : 0; }
static void oah_putf(unsigned int gp, float f) { unsigned int u; memcpy(&u, &f, 4); oah_put32(gp, u); }
static float oah_getf(unsigned int gp) { unsigned int u = oah_get32(gp); float f; memcpy(&f, &u, 4); return f; }
static unsigned int oah_stackarg(unsigned int *R, int word) {     /* word 9 = first stack arg */
    return oah_get32(R[1] + 0x38 + (word - 9) * 4);
}

static unsigned int oah_cgs_cid(void) {
    if (!g_ncid) {
        NCGS(int, CGSMainConnectionID, (void));
        NCGS(int, CGGetActiveDisplayList, (unsigned int, unsigned int *, unsigned int *));
        NCGS(oah_rect_t, CGDisplayBounds, (unsigned int));
        unsigned int ids[16], n = 0, i;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (!g_carbon_handle)
            g_carbon_handle = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon", RTLD_LAZY | RTLD_GLOBAL);
        g_ncid = p_CGSMainConnectionID ? (unsigned int)p_CGSMainConnectionID() : 0;
        if (p_CGGetActiveDisplayList) p_CGGetActiveDisplayList(16, ids, &n);
        for (i = 0; i < n; i++) {
            oah_rect_t b = p_CGDisplayBounds(ids[i]);
            if (!i || b.x < x0) x0 = b.x;
            if (!i || b.y < y0) y0 = b.y;
            if (!i || b.x + b.w > x1) x1 = b.x + b.w;
            if (!i || b.y + b.h > y1) y1 = b.y + b.h;
        }
        if (x1 - x0 < 640) { x0 = 0; x1 = 1920; }
        if (y1 - y0 < 480) { y0 = 0; y1 = 1200; }
        g_cgs_desk.x = x0; g_cgs_desk.y = y0; g_cgs_desk.w = x1 - x0; g_cgs_desk.h = y1 - y0;
        g_cgs_rb = (((int)g_cgs_desk.w + 64) * 4 + 63) & ~63;
        g_cgs_rows = (int)g_cgs_desk.h + 64;
        fprintf(stderr, "[cgs] native connection %u, %u display(s), desktop %.0fx%.0f at %.0f,%.0f; store %d rows x %d bytes\n",
                g_ncid, n, g_cgs_desk.w, g_cgs_desk.h, x0, y0, g_cgs_rows, g_cgs_rb);
        if (!g_fg_done) { g_fg_done = 1; oah_make_foreground(); oah_dnd_start(); }
    }
    return g_ncid;
}

/* ---- guest helpers ---- */
static unsigned int oah_cgs_gsym(const char *sym) {
    return oah_guest_sym("CoreGraphics.framework", sym);
}
/* ---- guest Objective-C probe (debugging) ----
   /tmp/oah_objc.<pid>: one chain per line, run on the main thread at the next event pull:
     NSApp keyWindow firstResponder          (NSApp = [NSApplication sharedApplication])
     NSApplication sharedApplication windows
     0x12345678 title                        (start from an object address)
   Each word after the first is a no-argument selector sent to the previous result; every
   step prints the pointer and the object's -description. */
static void oah_cgs_dump_wins(void);
static void oah_objc_probe(void) {
    static unsigned int cnt, fgetc, fsel, fmsg;
    char pf[64], ln[512];
    FILE *f;
    if ((cnt++ & 15) != 0) return;
    snprintf(pf, sizeof pf, "/tmp/oah_objc.%d", getpid());
    if (!(f = fopen(pf, "r"))) return;
    unlink(pf);
    if (!fgetc) { fgetc = oah_guest_sym("libobjc", "_objc_getClass"); fsel = oah_guest_sym("libobjc", "_sel_registerName");
                  fmsg = oah_guest_sym("libobjc", "_objc_msgSend"); }
    while (fgets(ln, sizeof ln, f)) {
        char *tok, *save = 0, *cs;
        unsigned int obj = 0, a[4], d, u;
        int first = 1;
        ln[strcspn(ln, "\r\n")] = 0;
        fprintf(stderr, "[objc?] %s\n", ln);
        if (!strcmp(ln, "cgs")) { oah_cgs_dump_wins(); continue; }
        for (tok = strtok_r(ln, " ", &save); tok; tok = strtok_r(0, " ", &save), first = 0) {
            if (first) {
                if (!strncmp(tok, "0x", 2)) obj = (unsigned int)strtoul(tok, 0, 16);
                else {
                    a[0] = (unsigned int)(!strcmp(tok, "NSApp") ? "NSApplication" : tok);
                    obj = oah_callppc_n("objc_getClass", fgetc, 1, a);
                    if (!strcmp(tok, "NSApp") && obj) {
                        a[0] = (unsigned int)"sharedApplication"; u = oah_callppc_n("sel", fsel, 1, a);
                        a[0] = obj; a[1] = u; obj = oah_callppc_n("msgSend", fmsg, 2, a);
                    }
                }
            } else {
                if (!obj) break;
                {   /* "selector:value" sends one integer/pointer argument */
                    char sb[128], *c; unsigned int arg = 0; int has = 0;
                    strlcpy(sb, tok, sizeof sb);
                    if ((c = strrchr(sb, ':')) && c[1]) { arg = (unsigned int)strtoul(c + 1, 0, 0); c[1] = 0; has = 1; }
                    a[0] = (unsigned int)sb; u = oah_callppc_n("sel", fsel, 1, a);
                    a[0] = obj; a[1] = u; a[2] = arg; obj = oah_callppc_n("msgSend", fmsg, has ? 3 : 2, a);
                }
            }
            cs = "";
            {   /* only describe things that look like objects: readable isa whose isa is readable */
                unsigned int isa = 0, misa = 0;
                if (obj < 0x100000 || (obj & 3) || !probe_read(obj, &isa) || !(isa = OSSwapBigToHostInt32(isa)) || (isa & 3) ||
                    !probe_read(isa, &misa) || !misa) { fprintf(stderr, "[objc]   %s -> %08x (%d)\n", tok, obj, (int)obj); continue; }
            }
            if (obj && obj > 0x1000) {
                a[0] = (unsigned int)"description"; u = oah_callppc_n("sel", fsel, 1, a);
                a[0] = obj; a[1] = u; d = oah_callppc_n("msgSend", fmsg, 2, a);
                if (d) { a[0] = (unsigned int)"UTF8String"; u = oah_callppc_n("sel", fsel, 1, a);
                         a[0] = d; a[1] = u; cs = (char *)oah_callppc_n("msgSend", fmsg, 2, a); if (!cs) cs = ""; }
            }
            if (access("/tmp/oah_objc_long", F_OK) == 0) fprintf(stderr, "[objc]   %s -> %08x %s\n", tok, obj, cs);
            else fprintf(stderr, "[objc]   %s -> %08x %.300s\n", tok, obj, cs);
        }
    }
    fclose(f);
}
/* Bounds of a guest CGSRegion (window-local). Returns 0 on failure. */
static int oah_cgs_region_bounds(unsigned int rgn, oah_rect_t *r) {
    static unsigned int fn;
    unsigned int out[4], a[2];
    if (!rgn) return 0;
    if (!fn) fn = oah_cgs_gsym("_CGSGetRegionBounds");
    if (!fn) return 0;
    memset(out, 0, sizeof out);
    a[0] = rgn; a[1] = (unsigned int)out;
    oah_callppc_n("CGSGetRegionBounds", fn, 2, a);
    r->x = oah_getf((unsigned int)&out[0]); r->y = oah_getf((unsigned int)&out[1]);
    r->w = oah_getf((unsigned int)&out[2]); r->h = oah_getf((unsigned int)&out[3]);
    return 1;
}
/* Bounds of a guest region only if the guest-call lock is free right now (1 = got them). For the
   window flush path, called many times a second on the main thread: a guest call there waited
   for the lock while the clipboard sync thread held it inside Word's promise keeper, which needs
   the main thread (Word stopped taking events for 20 s after Cmd-C). */
static int oah_cgs_region_bounds_try(unsigned int rgn, oah_rect_t *r) {
    int ok;
    pthread_once(&g_callppc_once, oah_callppc_mu_init);
    if (pthread_mutex_trylock(&g_callppc_mu)) return 0;
    g_cp_owner = pthread_self(); g_cp_count++;
    ok = oah_cgs_region_bounds(rgn, r);
    oah_callppc_unlock();
    return ok;
}
/* New guest CGSRegion from a rect (caller owns it). */
static unsigned int oah_cgs_gregion(const oah_rect_t *r) {
    static unsigned int fn;
    unsigned int in[4], out = 0, a[2];
    if (!fn) fn = oah_cgs_gsym("_CGSNewRegionWithRect");
    if (!fn) return 0;
    oah_putf((unsigned int)&in[0], r->x); oah_putf((unsigned int)&in[1], r->y);
    oah_putf((unsigned int)&in[2], r->w); oah_putf((unsigned int)&in[3], r->h);
    a[0] = (unsigned int)in; a[1] = (unsigned int)&out;
    oah_callppc_n("CGSNewRegionWithRect", fn, 2, a);
    return oah_get32((unsigned int)&out);
}
static void oah_cgs_grelease(unsigned int rgn) {
    static unsigned int fn;
    unsigned int a[1];
    if (!fn) fn = oah_cgs_gsym("_CGSReleaseRegion");
    if (!fn || !rgn) return;
    a[0] = rgn;
    oah_callppc_n("CGSReleaseRegion", fn, 1, a);
}
static void *oah_cgs_nregion(const oah_rect_t *r) {
    NCGS(int, CGSNewRegionWithRect, (const oah_rect_t *, void **));
    void *rg = 0;
    if (p_CGSNewRegionWithRect) p_CGSNewRegionWithRect(r, &rg);
    return rg;
}
static void oah_cgs_nrelease(void *cf) {
    static void (*rel)(const void *);
    if (!rel) rel = (void (*)(const void *))dlsym(RTLD_DEFAULT, "CFRelease");
    if (cf && rel) rel(cf);
}

/* ---- window table ---- */
typedef struct {
    unsigned int wid;
    int type, opaque, ordered, level;
    float x, y, w, h;               /* global top-left origin, size */
    unsigned char *store;           /* g_cgs_rows * g_cgs_rb */
    void *nctx;                     /* native window context */
    unsigned int gctx[8];           /* guest bitmap contexts made for it */
    int ngctx;
    unsigned int visrgn;            /* guest region handed out by LockWindowVisibleRegion */
    unsigned long flushes;
    unsigned long seq;              /* creation order (menu tracking: windows opened since the click) */
    int dirty;                      /* whole window needs redrawing (exposed) */
    int dsh_added, dsh_nonempty;    /* dirty-shape regions added since the last flush; any not empty */
    int glsurf;                     /* a CGL context draws on a surface of it */
    int ghost;                      /* ordered in natively but invisible (see oah_cgs_ghost) */
    float galpha; int glevel;       /* native alpha / level saved while ghosted */
    int hod, hod_hidden;            /* hides on deactivation; ordered out natively for that now */
    int actlev, lev_active, lev_inactive;   /* CGSSetWindowLevelsForActivation: level follows app activation */
    int app_hidden;                 /* ordered out natively while the app is hidden (Cmd-H) */
    int passthru;                   /* mouse events go to the window below (help tags, oah_cgs_tags_note) */
    unsigned int tags;              /* low word of the guest's window tags */
    int gldeferred, autoghost;      /* GL frames presented while hidden; ghosted for that (oah_cgs_autoghost) */
    unsigned char *shadow;          /* last pixels sent to the native window (oah_cgs_blit) */
    int shw, shh;                   /* its size; 0 = none / send everything */
    int sc;                         /* pixels per point of its store: 2 = Retina (OAH_HIDPI), else 1 */
    int nres;                       /* native window resolution already set */
    unsigned char *qd, *qdsh;       /* Retina window: 1x pixels for QuickDraw and their state at lock time */
    int qdh, qdw;                   /* the size they were synced at */
    unsigned int hsid, htex;        /* Retina: CGS surface at resolution 2 and its texture (oah_hd_present) */
    void *hctx;                     /*   the CGL context drawing it */
    int htw, hth, hfail;            /*   texture size; set up failed (CG blit only) */
    double blit_t;                  /* when its pixels were last sent (oah_unlock_blit_run) */
    double fchk_t;                  /* last empty flush that was blitted anyway (CGSFlushWindow) */
    double shown_t;                 /* when a flush last found it newly ordered in or moved */
    float fl_x, fl_y; int fl_ord;   /* its place and order state at that flush */
    int pmv; float pmx, pmy;        /* a move on screen held back until its contents are sent */
    double shex_t; int shex_rows;   /* a shown sheet: exposure due at shex_t unless pixels were sent since */
    unsigned int dnd_wid;           /* the window id CoreDrag handlers were installed for (oah_dnd.h) */
    double pmt;
} oah_cgswin_t;
/* Retina windows (OAH_HIDPI=1 on a 2x display): the window's store has twice the pixels in each
   direction and the guest's CoreGraphics context over it starts with a 2x transform, so text and
   paths drawn through CG are rendered at full resolution; the native window gets resolution 2.
   Open problem: on Mojave the native window context (CGWindowContextCreate) still has device
   scale 1 after CGSSetWindowResolution and kCGSDefaultWindowResolution, so the 2x pixels are
   scaled down again when blitted (also with NSHighResolutionCapable set in the Info dictionary
   before check-in: tried, no change). Needs an IOSurface/layer-backed window to reach the screen.
   QuickDraw writes pixels directly (CGSLockWindowBits) and cannot draw at 2x: a window that asks
   for its bits is switched back to 1x. Store geometry: WROWS(w) rows of WRB(w) bytes. */
#define WSC(w) ((w)->sc > 1 ? (w)->sc : 1)
#define WRB(w) (g_cgs_rb * WSC(w))
#define WROWS(w) (g_cgs_rows * WSC(w))
static float oah_display_scale(void);
static int oah_hidpi(void) {
    static int h = -1;
    if (h < 0) { const char *e = getenv("OAH_HIDPI"); h = (e && atoi(e) > 0 && oah_display_scale() >= 1.5f) ? 2 : 1; }
    return h;
}
#define OAH_MAXWIN 256
static oah_cgswin_t g_cgswin[OAH_MAXWIN];
static oah_cgswin_t *oah_cgs_win(unsigned int wid) {
    int i;
    if (!wid) return 0;
    for (i = 0; i < OAH_MAXWIN; i++) if (g_cgswin[i].wid == wid) return &g_cgswin[i];
    return 0;
}
/* hidesOnDeactivate windows (CGSSetHideOnDeact): out natively while the app is inactive,
   back in on activation if the guest still has them ordered in. OAH_NO_HOD=1 disables. */
static void oah_actlevel_apply(int active) {
    int i;
    NCGS(int, CGSSetWindowLevel, (unsigned int, unsigned int, int));
    if (!p_CGSSetWindowLevel) return;
    for (i = 0; i < OAH_MAXWIN; i++) {
        oah_cgswin_t *w = &g_cgswin[i];
        if (!w->wid || !w->actlev || w->ghost || w->lev_active == w->lev_inactive) continue;
        p_CGSSetWindowLevel(g_ncid, w->wid, active ? w->lev_active : w->lev_inactive);
        CGSLOG("[cgs] window %u level -> %d (%s)\n", w->wid, active ? w->lev_active : w->lev_inactive, active ? "active" : "inactive");
    }
}
static void oah_hod_apply(int active) {
    static int off = -1;
    int i;
    oah_actlevel_apply(active);
    if (off < 0) off = getenv("OAH_NO_HOD") ? 1 : 0;
    if (off) return;
    NCGS(int, CGSOrderWindow, (unsigned int, unsigned int, int, unsigned int));
    if (!p_CGSOrderWindow) return;
    for (i = 0; i < OAH_MAXWIN; i++) {
        oah_cgswin_t *w = &g_cgswin[i];
        if (!w->wid || !w->hod) continue;
        if (!active && w->ordered && !w->hod_hidden && !w->ghost) {
            p_CGSOrderWindow(g_ncid, w->wid, 0, 0);
            w->hod_hidden = 1;
            CGSLOG("[cgs] hide-on-deactivate: %u out\n", w->wid);
        } else if (active && w->hod_hidden) {
            w->hod_hidden = 0;
            if (w->ordered) p_CGSOrderWindow(g_ncid, w->wid, 1, 0);
            CGSLOG("[cgs] hide-on-deactivate: %u %s\n", w->wid, w->ordered ? "back in" : "stays out");
        }
    }
}
/* Hide / unhide the app (ShowHideProcess on ourselves). 10.6's window server hid an app's
   windows itself; Mojave's leaves it to the app (AppKit), so Cmd-H changed nothing on screen.
   Our windows go out natively while hidden and come back on unhide or the next activation.
   OAH_NO_APPHIDE=1 disables. */
static int g_app_hidden;
static double g_app_hide_at;   /* when the app was last hidden */
static volatile int g_ls_hidden_want;   /* 0 none, 1 shown, 2 hidden: to tell Launch Services */
static void oah_app_hide(int hide) {
    int i;
    NCGS(int, CGSOrderWindow, (unsigned int, unsigned int, int, unsigned int));
    if (!p_CGSOrderWindow || getenv("OAH_NO_APPHIDE")) return;
    if (hide == g_app_hidden) return;
    g_app_hidden = hide;
    if (hide) g_app_hide_at = CFAbsoluteTimeGetCurrent();
    for (i = 0; i < OAH_MAXWIN; i++) {
        oah_cgswin_t *w = &g_cgswin[i];
        if (!w->wid || w->wid == g_menubar_wid) continue;
        if (hide && w->ordered && !w->app_hidden && !w->hod_hidden && !w->ghost) {
            p_CGSOrderWindow(g_ncid, w->wid, 0, 0);
            w->app_hidden = 1;
        } else if (!hide && w->app_hidden) {
            w->app_hidden = 0;
            if (w->ordered && !w->hod_hidden) p_CGSOrderWindow(g_ncid, w->wid, 1, 0);
        }
    }
    g_ls_hidden_want = hide + 1;          /* for oah_ls_thread (OAH_LSTHREAD=1 only) */
    CGSLOG("[cgs] app %s\n", hide ? "hidden" : "shown again");
}
static void oah_cgs_dump_wins(void) {
    int i;
    for (i = 0; i < OAH_MAXWIN; i++) if (g_cgswin[i].wid) {
        oah_cgswin_t *w = &g_cgswin[i];
        fprintf(stderr, "[cgs] win %u type %d level %d ordered %d opaque %d at %.0f,%.0f %.0fx%.0f flushes %lu seq %lu\n",
                w->wid, w->type, w->level, w->ordered, w->opaque, w->x, w->y, w->w, w->h, w->flushes, w->seq);
    }
}
static oah_cgswin_t *oah_cgs_win_by_ctx(unsigned int ctx) {
    int i, j;
    if (!ctx) return 0;
    for (i = 0; i < OAH_MAXWIN; i++)
        for (j = 0; j < g_cgswin[i].ngctx; j++) if (g_cgswin[i].gctx[j] == ctx) return &g_cgswin[i];
    return 0;
}
/* ---- GL surfaces of windows that are not on screen ----
   Keynote renders slide images for its Inspector transition preview (and its slideshow
   textures) with NSOpenGLContexts on surfaces of windows it never orders in, then reads them
   back. 10.6's WindowServer backed such surfaces; Mojave gives an unordered window's GL
   surface no backing and the reads are black. While a GL surface's window is "out", keep it
   ordered in natively at alpha 0 on the minimum level ("ghost"); a real order-in restores
   alpha and level, a later order-out ghosts it again. Opt-in (OAH_GLGHOST=1): the black Inspector transition preview turned out to look the same on 10.6. */
static void oah_cgs_ghost(oah_cgswin_t *w, int on) {
    NCGS(int, CGSSetWindowAlpha, (unsigned int, unsigned int, float));
    NCGS(int, CGSGetWindowAlpha, (unsigned int, unsigned int, float *));
    NCGS(int, CGSSetWindowLevel, (unsigned int, unsigned int, int));
    NCGS(int, CGSGetWindowLevel, (unsigned int, unsigned int, int *));
    NCGS(int, CGSOrderWindow, (unsigned int, unsigned int, int, unsigned int));
    if (!w || on == w->ghost || !p_CGSSetWindowAlpha || !p_CGSSetWindowLevel || !p_CGSOrderWindow) return;
    if (on) {
        w->galpha = 1.0f; w->glevel = 0;
        if (p_CGSGetWindowAlpha) p_CGSGetWindowAlpha(g_ncid, w->wid, &w->galpha);
        if (p_CGSGetWindowLevel) p_CGSGetWindowLevel(g_ncid, w->wid, &w->glevel);
        {   const char *ga = getenv("OAH_GLGHOST_ALPHA"), *gl = getenv("OAH_GLGHOST_LEVEL");
            p_CGSSetWindowAlpha(g_ncid, w->wid, ga ? (float)atof(ga) : 0.0f);
            p_CGSSetWindowLevel(g_ncid, w->wid, gl ? atoi(gl) : -2147483643);   /* kCGMinimumWindowLevel */
        }
        p_CGSOrderWindow(g_ncid, w->wid, -1, 0);
        w->ghost = 1;
    } else {
        p_CGSSetWindowAlpha(g_ncid, w->wid, w->galpha > 0.0f ? w->galpha : 1.0f);
        p_CGSSetWindowLevel(g_ncid, w->wid, w->glevel);
        w->ghost = 0;
    }
    {   static int nlog; if (nlog++ < 20) fprintf(stderr, "[cgs] GL surface window %u %s\n", w->wid, on ? "ghosted (ordered in, invisible)" : "unghosted"); }
}
static int oah_cgs_win_visible(unsigned int wid) { oah_cgswin_t *w = oah_cgs_win(wid); return !w || w->ordered; }
static int g_glghost = -1;
/* called by oah_glx.h after CGLSetSurface(ctx, cid, wid, sid) */
static void oah_cgs_gl_attached(unsigned int wid) {
    oah_cgswin_t *w = oah_cgs_win(wid);
    if (g_glghost < 0) g_glghost = getenv("OAH_GLGHOST") ? 1 : 0;   /* opt-in: SL shows the same black preview */
    if (!w || !g_glghost) return;
    w->glsurf = 1;
    if (!w->ordered) oah_cgs_ghost(w, 1);
}
/* A GL context keeps presenting into a window that is not on screen: an offscreen render target
   (Build-a-lot 2 draws its scenes into hidden windows and uses them with aglSurfaceTexture; the
   visible window stayed white). 10.6 backed such surfaces, Mojave does not: ghost the window
   (ordered in at alpha 0) after the third such frame. Returns 1 once it is ghosted, so the frame
   is presented. OAH_NO_AUTOGHOST=1 keeps deferring instead. */
static int oah_cgs_autoghost(unsigned int wid) {
    static int off = -1;
    oah_cgswin_t *w = oah_cgs_win(wid);
    if (off < 0) off = getenv("OAH_NO_AUTOGHOST") ? 1 : 0;
    if (off || !w) return 0;
    if (w->ghost) return 1;
    if (++w->gldeferred < 3) return 0;
    w->glsurf = 1; w->autoghost = 1;
    oah_cgs_ghost(w, 1);
    CGSLOG("[cgs] window %u: GL frames while hidden, ghosted (offscreen GL target)\n", wid);
    return 1;
}
static int oah_cgs_autoghost_now(unsigned int wid) {
    oah_cgswin_t *w = oah_cgs_win(wid);
    if (!w) return 0;
    if (w->gldeferred < 2) w->gldeferred = 2;
    return oah_cgs_autoghost(wid);
}
/* guest order request for w: returns 1 when it was absorbed (order-out of a GL window) */
static int oah_cgs_ghost_order(oah_cgswin_t *w, int mode) {
    if (!w || !w->glsurf || !(g_glghost || w->autoghost)) return 0;
    if (mode != 0) { oah_cgs_ghost(w, 0); return 0; }
    w->ordered = 0;
    oah_cgs_ghost(w, 1);
    return 1;
}
static unsigned char *oah_cgs_bits(oah_cgswin_t *w) {      /* window's top-left pixel */
    int h = (int)(w->h * WSC(w) + 0.5f);
    if (h > WROWS(w)) h = WROWS(w);
    if (h < 0) h = 0;
    return w->store + (size_t)(WROWS(w) - h) * WRB(w);
}
/* Retina windows and QuickDraw: QuickDraw writes pixels itself and only knows 1x. It gets a 1x
   copy of the window (every 2nd pixel of the 2x store) when it locks the window's bits, and what
   it changed is written back as 2x2 blocks when it unlocks them (and before a flush), so CG text
   drawn before or after keeps its full resolution and the drawing order is kept. */
static void oah_qd_pull(oah_cgswin_t *w) {
    int x, y, iw = (int)(w->w + 0.5f), ih = (int)(w->h + 0.5f);
    unsigned char *s2 = oah_cgs_bits(w), *d;
    if (iw > g_cgs_rb / 4) iw = g_cgs_rb / 4;
    if (ih > g_cgs_rows) ih = g_cgs_rows;
    if (iw <= 0 || ih <= 0) return;
    d = w->qd + (size_t)(g_cgs_rows - ih) * g_cgs_rb;
    for (y = 0; y < ih; y++) {
        const unsigned int *sr = (const unsigned int *)(s2 + (size_t)(2 * y) * WRB(w));
        unsigned int *dr = (unsigned int *)(d + (size_t)y * g_cgs_rb);
        for (x = 0; x < iw; x++) dr[x] = sr[2 * x];
    }
    memcpy(w->qdsh + (size_t)(g_cgs_rows - ih) * g_cgs_rb, d, (size_t)ih * g_cgs_rb);
    w->qdh = ih; w->qdw = iw;
}
static void oah_qd_push(oah_cgswin_t *w) {
    int x, y, iw = w->qdw, ih = w->qdh;
    unsigned char *s2, *q, *sh;
    if (!w->qd || iw <= 0 || ih <= 0) return;
    if ((int)(w->h + 0.5f) != ih) { w->qdh = 0; return; }        /* resized since: the next lock re-pulls */
    s2 = oah_cgs_bits(w);
    q = w->qd + (size_t)(g_cgs_rows - ih) * g_cgs_rb; sh = w->qdsh + (size_t)(g_cgs_rows - ih) * g_cgs_rb;
    for (y = 0; y < ih; y++) {
        unsigned int *qr = (unsigned int *)(q + (size_t)y * g_cgs_rb), *hr = (unsigned int *)(sh + (size_t)y * g_cgs_rb);
        unsigned int *a, *b;
        if (!memcmp(qr, hr, (size_t)iw * 4)) continue;
        a = (unsigned int *)(s2 + (size_t)(2 * y) * WRB(w)); b = (unsigned int *)(s2 + (size_t)(2 * y + 1) * WRB(w));
        for (x = 0; x < iw; x++) if (qr[x] != hr[x]) { a[2 * x] = a[2 * x + 1] = b[2 * x] = b[2 * x + 1] = qr[x]; hr[x] = qr[x]; }
    }
}
static oah_cgswin_t *oah_cgs_add(unsigned int wid, int type, float x, float y, const oah_rect_t *shape) {
    int i;
    for (i = 0; i < OAH_MAXWIN; i++) if (!g_cgswin[i].wid) break;
    if (i == OAH_MAXWIN) return 0;
    memset(&g_cgswin[i], 0, sizeof g_cgswin[i]);
    g_cgswin[i].wid = wid; g_cgswin[i].type = type; g_cgswin[i].opaque = 1;
    { static unsigned long seq; g_cgswin[i].seq = ++seq; }
    g_cgswin[i].x = x + shape->x; g_cgswin[i].y = y + shape->y;
    g_cgswin[i].w = shape->w; g_cgswin[i].h = shape->h;
    g_cgswin[i].sc = oah_hidpi();
    g_cgswin[i].store = (unsigned char *)mmap(0, (size_t)WROWS(&g_cgswin[i]) * WRB(&g_cgswin[i]), PROT_READ | PROT_WRITE,
                                              MAP_ANON | MAP_PRIVATE, -1, 0);
    if (g_cgswin[i].store == (unsigned char *)MAP_FAILED) { g_cgswin[i].wid = 0; return 0; }
    return &g_cgswin[i];
}

/* Copy a window's store to its native window. */

/* ---- Mojave menu bar integration ----
   Mojave's menu bar shows whatever the frontmost process publishes with SLSSetMenuBars: one
   dictionary per display naming an "image window" whose contents WindowServer composites into
   the bar (AppKit draws its titles there). Mouse-downs on the bar then arrive on the process's
   own connection (window = the system menu bar window). So a translated app gets a native-looking
   menu bar without AppKit:
   - the guest HIToolbox still draws its 10.6 menu bar into its own menu bar window (never shown);
   - every flush of that window is restyled for the current Mojave appearance (background removed,
     black text -> white in Dark mode / black in Light mode, the blue title highlight kept) into two
     image windows (active / inactive) that are published with SLSSetMenuBars;
   - mouse-downs in the bar are handed to the guest as clicks on its own menu bar, so its MenuSelect
     opens the menus (drawn by the guest, below Mojave's bar) at the same x positions.
   OAH_NO_MOJAVE_MENUBAR=1 disables (the guest bar is then left as is). */
static unsigned int g_mbimg[2];           /* active, inactive image windows */
static int g_mb_hx0, g_mb_hx1;            /* highlighted title (Mojave selection), 0/0 = none */
static unsigned long g_mb_trackseq;       /* menu tracking: windows newer than this are menus */
static double g_mb_trackt;
static int g_mb_tracking;
static void *g_mbctx[2];
static unsigned int *g_mbpix;             /* host-order premultiplied BGRA scratch */
static int g_mb_w, g_mb_published, g_mb_dark = -1;
static int g_mb_force;                     /* render even an unchanged bar */
static double g_mb_force_at[3];            /* re-render + republish at these times (after a hidden UI mode) */
static int oah_mb_enabled(void) {
    static int on = -1;
    if (on < 0) on = getenv("OAH_NO_MOJAVE_MENUBAR") ? 0 : 1;
    return on;
}
static int oah_mb_dark(void) {
    static double at; static int d;      /* a cfprefsd round trip per frame was most of the bar's cost */
    double t = CFAbsoluteTimeGetCurrent();
    if (t - at < 1.0 && at) return d;
    at = t;
    CFPropertyListRef v = CFPreferencesCopyAppValue(CFSTR("AppleInterfaceStyle"), kCFPreferencesAnyApplication);
    d = v && CFGetTypeID(v) == CFStringGetTypeID() && CFStringCompare((CFStringRef)v, CFSTR("Dark"), kCFCompareCaseInsensitive) == 0;
    if (v) CFRelease(v);
    return d;
}
static void oah_mb_put(CFMutableDictionaryRef d, const char *k, CFTypeRef v) {
    CFStringRef ks = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    CFDictionarySetValue(d, ks, v); CFRelease(ks);
}
static void oah_mb_putn(CFMutableDictionaryRef d, const char *k, long long n) {
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberLongLongType, &n);
    oah_mb_put(d, k, v); CFRelease(v);
}
static volatile int g_app_active;          /* defined with the activation code below */
static int g_mb_pending;                   /* a publish skipped while inactive: do it on activation */
static unsigned int g_ui_mode, g_ui_opts;   /* the guest's last SetSystemUIMode */
static void oah_mb_publish(void) {
    /* Only while the app is active (plus the first time): a background app that redraws its menu
       bar (menu items enabled/disabled after it lost focus) took the bar over from the frontmost
       app. OAH_MB_ANYTIME=1 restores the old behaviour. */
    {   static int any = -1;
        if (any < 0) any = getenv("OAH_MB_ANYTIME") ? 1 : 0;
        if (!any && g_mb_published && !g_app_active) { g_mb_pending = 1; return; }
        g_mb_pending = 0;
    }
    /* 4th argument: the display (AppKit passes CGMainDisplayID; without it the window server
       got stack garbage and the bars only showed until the app was first deactivated) */
    NCGS(int, CGSSetMenuBars, (unsigned int, CFArrayRef, CFDictionaryRef, unsigned int));
    NCGS(unsigned int, CGMainDisplayID, (void));
    CFMutableDictionaryRef bar, props;
    CFArrayRef bars, titles;
    CFStringRef s;
    int err, dark = oah_mb_dark();
    if (!p_CGSSetMenuBars || !g_mbimg[0]) return;
    bar = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    /* kUIModeAllHidden / AllSuppressed (full-screen games): the bar is published but not revealed,
       or it covered the top of the game (Build-a-lot 2's goals bar) */
    oah_mb_put(bar, "kSLMenuBarIsRevealedKey", (g_ui_mode == 3 || g_ui_mode == 4) ? kCFBooleanFalse : kCFBooleanTrue);
    oah_mb_putn(bar, "kSLMenuBarRevealDurationKey", 0);
    oah_mb_putn(bar, "menuBarDisplayID", p_CGMainDisplayID ? p_CGMainDisplayID() : 0);
    oah_mb_putn(bar, "menuBarFlags", 2);
    oah_mb_putn(bar, "menuBarImageWindow", g_mbimg[0]);
    oah_mb_putn(bar, "menuBarInactiveImageWindow", g_mbimg[1]);
    titles = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
    oah_mb_put(bar, "menuBarMenuTitlesArray", titles); CFRelease(titles);
    oah_mb_putn(bar, "menuBarStatusBarAvailableSpace", (long long)(g_cgs_desk.w * 0.8));
    bars = CFArrayCreate(NULL, (const void **)&bar, 1, &kCFTypeArrayCallBacks);
    props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    oah_mb_put(props, "menuBarRTL", kCFBooleanFalse);
    oah_mb_put(props, "menuBarHasBackstop", kCFBooleanFalse);
    s = dark ? CFSTR("MacUltradark") : CFSTR("MacLight"); oah_mb_put(props, "menuBarActiveMaterial", s);
    s = dark ? CFSTR("MacDark") : CFSTR("MacLight"); oah_mb_put(props, "menuBarInactiveMaterial", s);
    oah_mb_put(props, "menuBarTitleMaterial", CFSTR("SelectionBlue"));
    err = p_CGSSetMenuBars(g_ncid, bars, props, p_CGMainDisplayID ? p_CGMainDisplayID() : 0);
    CFRelease(bars); CFRelease(bar); CFRelease(props);
    if (!g_mb_published || dark != g_mb_dark || err) CGSLOG("[cgs] menu bar published: image windows %u/%u, %s appearance -> %d\n", g_mbimg[0], g_mbimg[1], dark ? "dark" : "light", err);
    g_mb_published = 1; g_mb_dark = dark;
}
static pthread_mutex_t g_mb_mu = PTHREAD_MUTEX_INITIALIZER;
/* Bars re-set a little later too (the window server may still be switching apps). Everything
   here runs on the guest's main thread: a native helper thread that called LaunchServices /
   SkyLight made the translator abort in spin_lock_wrapper (twice, on a menu click). */
static double g_mb_pub_at[3];
static void oah_mb_render(oah_cgswin_t *w);
static void oah_mb_schedule(void) {
    double t = CFAbsoluteTimeGetCurrent();
    g_mb_pub_at[0] = t + 0.05; g_mb_pub_at[1] = t + 0.3; g_mb_pub_at[2] = t + 1.0;
}
/* Activation by the Dock / Cmd-Tab. LaunchServices makes the app front and expects the app
   itself (AppKit) to take over the window server's front: until it does, Mojave keeps showing
   the previous app's menu bar, and neither re-setting the bars nor SLPSSetFrontProcess on an
   app that is already front changes that. So when this process becomes front (polled from
   launchservicesd; GetFrontProcess answers from a client cache refreshed on the native main run
   loop, which never runs here), hand the activation over the way an outside SetFrontProcess
   would: the previous front app is made front without raising its windows (kCPSNoWindows),
   then this one (kCPSUserGenerated), and the bars are set again. OAH_MB_NOHANDOFF=1 skips it.
   Called from the bridge and event paths (main thread), at most every 250 ms. */
static void oah_mb_menu_closed(void);
static void oah_mb_tick(int force) {
    /* Front process from the window server (CPS), not LaunchServices: launchservicesd waits on
       this app during a switch (its notifications go unanswered), and a blocking LS call made
       while guest code held a spin lock let another guest thread trip the translator's
       spin-lock trap in a menu click (GetSystemUIMode). */
    static int (*getfront)(unsigned int *);
    static int (*getcur)(unsigned int *);
    static int (*slps)(void *, unsigned int, unsigned int);
    static int init, was = 1;
    static double last, quiet_until;
    static unsigned int prev[2], self[2];
    unsigned int f[2] = { 0, 0 };
    double t;
    int k, now;
    if (!g_mb_published || !oah_mb_enabled()) return;
    {   /* debug trigger, checked once a second: this runs on every bridged CGS call */
        static time_t chk, chk2;
        time_t tn = time(0);
        if (tn != chk) { chk = tn; if (access("/tmp/oah_mbdump", 0) == 0) { oah_cgswin_t *mw = oah_cgs_win(g_menubar_wid); if (mw) oah_mb_render(mw); } }
        if (tn != chk2) { chk2 = tn; if (access("/tmp/oah_mbpub", 0) == 0) { unlink("/tmp/oah_mbpub"); g_mb_force = 1;
            {   oah_cgswin_t *mw = oah_cgs_win(g_menubar_wid); if (mw) oah_mb_render(mw); }
            oah_mb_publish(); fprintf(stderr, "[cgs] debug: menu bar re-rendered and republished\n"); } }
    }
    {
        static int off = -1;
        if (off < 0) off = getenv("OAH_MB_NOTICK") ? 1 : 0;
        if (off) return;
    }
    t = CFAbsoluteTimeGetCurrent();
    for (k = 0; k < 3; k++)
        if (g_mb_pub_at[k] && t >= g_mb_pub_at[k]) { g_mb_pub_at[k] = 0; oah_mb_publish(); }
    for (k = 0; k < 3; k++)
        if (g_mb_force_at[k] && t >= g_mb_force_at[k]) {
            oah_cgswin_t *mw = oah_cgs_win(g_menubar_wid);
            g_mb_force_at[k] = 0; g_mb_force = 1;
            if (mw) oah_mb_render(mw);
            oah_mb_publish();
        }
    /* A menu closed within 0.3 s of the click (a quick click-release, the same title clicked
       again, an item chosen at once) left its title highlighted and the bar still tracking:
       oah_mb_menu_closed waits that long for the menu to open and nothing called it again, so
       the title stayed blue and moving the mouse highlighted the other titles. Check again here
       once the time is up. (Rosettus for Mojave 1.1, from the Catalina port.) */
    if (g_mb_tracking && t - g_mb_trackt >= 0.35) oah_mb_menu_closed();
    if (!force && t - last < 0.25) return;
    last = t;
    if (!init) {
        init = 1;
        getfront = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "SLPSGetFrontProcess");
        getcur = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "SLPSGetCurrentProcess");
        slps = (int (*)(void *, unsigned int, unsigned int))dlsym(RTLD_DEFAULT, "_SLPSSetFrontProcessWithOptions");
        if (getcur) getcur(self);
    }
    if (!getfront || !self[1] || getfront(f)) return;
    oah_clip_start();
    oah_clip_service();                             /* clipboard sync requests (guest side) */
    oah_ae_checkin();
    now = f[0] == self[0] && f[1] == self[1];
    if (!now && f[1]) { prev[0] = f[0]; prev[1] = f[1]; }   /* remember who was front */
    if (now && !was && t >= quiet_until) {
        int e1 = -1, e2 = -1;
        if (!getenv("OAH_MB_NOHANDOFF") && slps && prev[1]) {
            e1 = slps(prev, 0, 0x400 /* kCPSNoWindows */);
            e2 = slps(self, 0, 0x200 /* kCPSUserGenerated */);
            quiet_until = t + 1.5;                         /* our own hand-off is not a new switch */
        }
        oah_mb_publish();
        oah_mb_schedule();
        CGSLOG("[cgs] became frontmost: activation handed over via %x.%x (%d %d), menu bar republished\n",
               prev[0], prev[1], e1, e2);
    }
    was = now;
}
/* ---- native menu titles (crisp on Retina) ----
   The guest's bar is 10.6 artwork drawn at 1x; restyled pixel by pixel and scaled up by Mojave on
   a Retina display, its titles were blurry. The titles are now read from the guest's root menu
   and drawn natively, in Mojave's menu-title font at the display's own scale, centred on the
   spans the guest's layout gives them so a click still opens the menu under it. If the spans and
   the menus do not match up (a hidden menu, a title with wide gaps) the restyled pixels are used.
   OAH_MB_PIXELS=1 always uses the pixels. */
static CFStringRef oah_guest_str_native(unsigned int gs);   /* oah_ls.h */
#define OAH_MB_MAXT 48
static CFStringRef g_mbt[OAH_MB_MAXT];
static CFStringRef g_mbmatch[OAH_MB_MAXT];   /* per span: its native title, 0 = keep the guest's pixels */
static int g_mbt_n;
static float g_disp_scale;
static float oah_display_scale(void) {
    if (!g_disp_scale) {
        unsigned int (*mid)(void) = (unsigned int (*)(void))dlsym(RTLD_DEFAULT, "CGMainDisplayID");
        void *(*cm)(unsigned int) = (void *(*)(unsigned int))dlsym(RTLD_DEFAULT, "CGDisplayCopyDisplayMode");
        size_t (*pw)(void *) = (size_t (*)(void *))dlsym(RTLD_DEFAULT, "CGDisplayModeGetPixelWidth");
        size_t (*ww)(void *) = (size_t (*)(void *))dlsym(RTLD_DEFAULT, "CGDisplayModeGetWidth");
        void (*rel)(void *) = (void (*)(void *))dlsym(RTLD_DEFAULT, "CGDisplayModeRelease");
        void *m;
        g_disp_scale = 1;
        if (mid && cm && pw && ww && (m = cm(mid()))) {
            if (ww(m)) g_disp_scale = (float)pw(m) / (float)ww(m);
            if (rel) rel(m);
        }
        if (getenv("OAH_SCALE")) g_disp_scale = (float)atof(getenv("OAH_SCALE"));
        if (g_disp_scale < 1) g_disp_scale = 1;
        CGSLOG("[cgs] display scale %.2f\n", g_disp_scale);
    }
    return g_disp_scale;
}
static void oah_set_window_scale(unsigned int wid, float sc) {
    static int (*swr)(unsigned int, unsigned int, double);
    if (sc <= 1 || !wid) return;
    if (!swr) swr = (int (*)(unsigned int, unsigned int, double))oah_ncgs("CGSSetWindowResolution");
    if (swr) { int e = swr(g_ncid, wid, sc); CGSLOG("[cgs] window %u resolution %.1f -> %d\n", wid, sc, e); }
    else CGSLOG("[cgs] no CGSSetWindowResolution\n");
}
/* the guest's menu titles in menu bar order (1 = the Apple menu); guest calls: main thread only */
static void oah_mb_fetch_titles(void) {
    static unsigned int fr, fc, fh, ft, frel;
    unsigned int a[3], root, n, i;
    if (getenv("OAH_MB_PIXELS")) return;
    if (!pthread_main_np()) { static int said; if (!said++) CGSLOG("[cgs] menu titles: not on the main thread\n"); return; }
    if (!fr) {
        fr = oah_guest_sym("HIToolbox", "_AcquireRootMenu"); fc = oah_guest_sym("HIToolbox", "_CountMenuItems");
        fh = oah_guest_sym("HIToolbox", "_GetMenuItemHierarchicalMenu"); ft = oah_guest_sym("HIToolbox", "_CopyMenuTitleAsCFString");
        frel = oah_guest_sym("CoreFoundation", "_CFRelease");
    }
    for (i = 0; i < OAH_MB_MAXT; i++) if (g_mbt[i]) { CFRelease(g_mbt[i]); g_mbt[i] = 0; }
    g_mbt_n = 0;
    if (!fr) fr = oah_guest_sym("HIToolbox", "_GetRootMenu");
    {   static int said;
        if (!said++) CGSLOG("[cgs] menu titles: GetRootMenu %08x CountMenuItems %08x GetMenuItemHierarchicalMenu %08x CopyMenuTitleAsCFString %08x\n", fr, fc, fh, ft); }
    if (!fr || !fc || !fh || !ft) return;
    if (!(root = oah_callppc_n("GetRootMenu", fr, 0, a))) { CGSLOG("[cgs] menu titles: no root menu\n"); return; }
    a[0] = root; n = oah_callppc_n("CountMenuItems", fc, 1, a) & 0xffff;
    CGSLOG("[cgs] menu titles: root %08x, %u items\n", root, n);
    for (i = 1; i <= n && g_mbt_n < OAH_MB_MAXT; i++) {
        unsigned int sub = 0, gs = 0; int e;
        /* the root item's own text first: asking a Cocoa app for a submenu makes AppKit build it
           (the Help menu's search field put a black 426x89 window on screen, which also kept
           Cmd-H from hiding the app) */
        static unsigned int fit, cocoa = 2;
        if (!fit) fit = oah_guest_sym("HIToolbox", "_CopyMenuItemTextAsCFString");
        if (cocoa == 2) cocoa = oah_guest_sym("AppKit", "_NSApp") ? 1 : 0;
        if (fit) {
            a[0] = root; a[1] = i; a[2] = (unsigned int)&gs;
            if (!(short)oah_callppc_n("CopyMenuItemTextAsCFString", fit, 3, a) && (gs = OSSwapBigToHostInt32(gs))) {
                CFStringRef s = oah_guest_str_native(gs);
                if (frel) { a[0] = gs; oah_callppc_n("CFRelease", frel, 1, a); }
                if (s && (CFStringGetLength(s) > 0 || cocoa)) {
                    g_mbt[g_mbt_n++] = s;
                    if (g_cgstrace > 0) { char tb[128] = ""; CFStringGetCString(s, tb, sizeof tb, kCFStringEncodingUTF8);
                        CGSLOG("[cgs] menu titles: item %u text \"%s\"\n", i, tb); }
                    continue;
                }
                if (s) CFRelease(s);
                gs = 0;
            }
        }
        if (cocoa) continue;
        a[0] = root; a[1] = i; a[2] = (unsigned int)&sub;
        if ((e = (short)oah_callppc_n("GetMenuItemHierarchicalMenu", fh, 3, a)) || !(sub = OSSwapBigToHostInt32(sub))) {
            CGSLOG("[cgs] menu titles: item %u: no submenu (%d)\n", i, e); continue; }
        a[0] = sub; a[1] = (unsigned int)&gs;
        if ((e = (short)oah_callppc_n("CopyMenuTitleAsCFString", ft, 2, a)) || !(gs = OSSwapBigToHostInt32(gs))) {
            CGSLOG("[cgs] menu titles: item %u: no title (%d)\n", i, e); continue; }
        g_mbt[g_mbt_n++] = oah_guest_str_native(gs);
        if (g_cgstrace > 0 && g_mbt[g_mbt_n - 1]) { char tb[128] = ""; CFStringGetCString(g_mbt[g_mbt_n - 1], tb, sizeof tb, kCFStringEncodingUTF8);
            CGSLOG("[cgs] menu titles: item %u \"%s\"\n", i, tb); }
        if (frel) { a[0] = gs; oah_callppc_n("CFRelease", frel, 1, a); }
    }
    {   static unsigned int frm;   /* AcquireRootMenu retained it */
        if (!frm) frm = oah_guest_sym("HIToolbox", "_ReleaseMenu");
        if (frm) { a[0] = root; oah_callppc_n("ReleaseMenu", frm, 1, a); } }
}
/* inked column runs of the guest bar (its titles, left to right) */
static int oah_mb_colink(oah_cgswin_t *w, int x);
static int oah_mb_spans(oah_cgswin_t *w, int iw, int *s0, int *s1, int max) {
    int x = 0, n = 0, a, b, gap;
    while (x < iw && n < max) {
        while (x < iw && !oah_mb_colink(w, x)) x++;
        if (x >= iw) break;
        a = b = x; gap = 0;
        for (; x < iw; x++) {
            if (oah_mb_colink(w, x)) { gap = 0; b = x; }
            else if (++gap >= 9) break;
        }
        s0[n] = a; s1[n] = b + 1; n++;
    }
    return n;
}
typedef struct { float a, b, c, d, tx, ty; } oah_aff_t;
static double oah_mb_guest_width(CFStringRef t, int bold) {
    static void *(*mkfont)(CFStringRef, float, void *);
    static void *(*mkline)(CFAttributedStringRef);
    static double (*bounds)(void *, float *, float *, float *);
    static CFStringRef *kfont;
    static void *f[2];
    CFTypeRef k, v; CFDictionaryRef at; CFAttributedStringRef as; void *line; double wd = -1;
    if (!mkfont) {
        void *ct = dlopen("/System/Library/Frameworks/CoreText.framework/CoreText", RTLD_LAZY | RTLD_GLOBAL);
        if (!ct) return -1;
        mkfont = (void *(*)(CFStringRef, float, void *))dlsym(ct, "CTFontCreateWithName");
        mkline = (void *(*)(CFAttributedStringRef))dlsym(ct, "CTLineCreateWithAttributedString");
        bounds = (double (*)(void *, float *, float *, float *))dlsym(ct, "CTLineGetTypographicBounds");
        kfont = (CFStringRef *)dlsym(ct, "kCTFontAttributeName");
    }
    if (!mkfont || !mkline || !bounds || !kfont || !t) return -1;
    if (!f[bold]) f[bold] = mkfont(bold ? CFSTR("LucidaGrande-Bold") : CFSTR("LucidaGrande"), 14, 0);
    if (!f[bold]) return -1;
    k = *kfont; v = f[bold];
    at = CFDictionaryCreate(0, &k, &v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    as = CFAttributedStringCreate(0, t, at);
    if (as && (line = mkline(as))) { wd = bounds(line, 0, 0, 0); CFRelease(line); }
    if (as) CFRelease(as);
    CFRelease(at);
    return wd;
}
/* the application menu's title: the bundle name (CFM apps: CFProcessPath points into the bundle) */
static CFStringRef oah_mb_app_name(void) {
    static CFStringRef nm;
    if (!nm) {
        CFBundleRef b = CFBundleGetMainBundle();
        CFTypeRef v = b ? CFBundleGetValueForInfoDictionaryKey(b, CFSTR("CFBundleName")) : 0;
        if (v && CFGetTypeID(v) == CFStringGetTypeID() && CFStringGetLength((CFStringRef)v)) nm = (CFStringRef)CFRetain(v);
        else {
            const char *e = getenv("OAH_GUEST_EXE"), *sl = e ? strrchr(e, '/') : 0;
            nm = CFStringCreateWithCString(0, sl ? sl + 1 : "", kCFStringEncodingUTF8);
        }
    }
    return nm;
}
/* Match the guest bar's title spans to the menus by width: the app menu, the root menu's titled
   menus in order, then Help. A span with no match (an icon title: Scripts, a status menu) keeps the
   guest's pixels. Returns how many spans got a native title. */
static int oah_mb_match(int nsp, const int *s0, const int *s1, int *who, int *bold) {
    CFStringRef c[OAH_MB_MAXT + 2]; int cb[OAH_MB_MAXT + 2], nc = 0, i, j = 0, k, got = 0;
    static CFStringRef help;
    if (!help) help = CFSTR("Help");
    c[nc] = oah_mb_app_name(); cb[nc++] = 1;
    for (i = 1; i < g_mbt_n && nc < OAH_MB_MAXT; i++)
        if (g_mbt[i] && CFStringGetLength(g_mbt[i]) && CFStringGetCharacterAtIndex(g_mbt[i], 0) >= 0x20) { c[nc] = g_mbt[i]; cb[nc++] = 0; }
    c[nc] = help; cb[nc++] = 0;
    for (i = 0; i < nsp; i++) {
        double ws = s1[i] - s0[i];
        who[i] = -1; bold[i] = 0;
        for (k = j; k < nc && k < j + 3; k++) {
            double wc = oah_mb_guest_width(c[k], cb[k]);
            if (wc > 0 && fabs(wc - ws) <= (ws * 0.08 > 3 ? ws * 0.08 : 3)) { who[i] = k; bold[i] = cb[k]; j = k + 1; got++; break; }
        }
    }
    for (i = 0; i < nsp; i++) if (who[i] >= 0) who[i] = (int)(uintptr_t)c[who[i]] ? who[i] : -1;
    /* hand back the strings through g_mbmatch */
    for (i = 0; i < nsp; i++) g_mbmatch[i] = who[i] >= 0 ? c[who[i]] : 0;
    return got;
}
static int oah_mb_draw_title_ok(void) {
    static int ok = -1;
    if (ok < 0) { void *ct = dlopen("/System/Library/Frameworks/CoreText.framework/CoreText", RTLD_LAZY | RTLD_GLOBAL);
                  ok = ct && dlsym(ct, "CTLineDraw") && dlsym(ct, "CTFontCreateUIFontForLanguage"); }
    return ok;
}
/* draw one title with CoreText; returns 0 if CoreText is unavailable */
static int oah_mb_draw_title(void *ctx, CFStringRef t, int bold, float cx, float r, float g, float b, float al) {
    static void *ct;
    static void *(*mkfont)(int, float, CFStringRef);
    static void *(*mkline)(CFAttributedStringRef);
    static double (*bounds)(void *, float *, float *, float *);
    static void (*draw)(void *, void *);
    static CFStringRef *kfont, *kfromctx;
    static void *fonts[2];
    NCGS(void, CGContextSetTextPosition, (void *, float, float));
    NCGS(void, CGContextSetTextMatrix, (void *, oah_aff_t));
    NCGS(void, CGContextSetRGBFillColor, (void *, float, float, float, float));
    CFTypeRef keys[2], vals[2];
    CFDictionaryRef at; CFAttributedStringRef as; void *line; double wd;
    oah_aff_t id = { 1, 0, 0, 1, 0, 0 };
    if (!ct) {
        ct = dlopen("/System/Library/Frameworks/CoreText.framework/CoreText", RTLD_LAZY | RTLD_GLOBAL);
        if (!ct) ct = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/CoreText.framework/CoreText", RTLD_LAZY | RTLD_GLOBAL);
        if (ct) {
            mkfont = (void *(*)(int, float, CFStringRef))dlsym(ct, "CTFontCreateUIFontForLanguage");
            mkline = (void *(*)(CFAttributedStringRef))dlsym(ct, "CTLineCreateWithAttributedString");
            bounds = (double (*)(void *, float *, float *, float *))dlsym(ct, "CTLineGetTypographicBounds");
            draw = (void (*)(void *, void *))dlsym(ct, "CTLineDraw");
            kfont = (CFStringRef *)dlsym(ct, "kCTFontAttributeName");
            kfromctx = (CFStringRef *)dlsym(ct, "kCTForegroundColorFromContextAttributeName");
        }
    }
    if (!mkfont || !mkline || !bounds || !draw || !kfont || !kfromctx || !p_CGContextSetTextPosition) return 0;
    if (!fonts[bold]) fonts[bold] = mkfont(bold ? 3 /* kCTFontUIFontEmphasizedSystem */ : 11 /* kCTFontUIFontMenuTitle */, 14, 0);
    if (!fonts[bold]) return 0;
    keys[0] = *kfont; vals[0] = fonts[bold]; keys[1] = *kfromctx; vals[1] = kCFBooleanTrue;
    at = CFDictionaryCreate(0, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    as = CFAttributedStringCreate(0, t, at);
    line = as ? mkline(as) : 0;
    if (line) {
        wd = bounds(line, 0, 0, 0);
        p_CGContextSetTextMatrix(ctx, id);
        p_CGContextSetRGBFillColor(ctx, r, g, b, al);
        p_CGContextSetTextPosition(ctx, (float)(cx - wd / 2), 6);
        draw(line, ctx);
        CFRelease(line);
    }
    if (as) CFRelease(as);
    CFRelease(at);
    return 1;
}
/* Column ink of the guest's bar (dark text on its light background). */
static int oah_mb_colink(oah_cgswin_t *w, int x) {
    unsigned char *src = oah_cgs_bits(w);
    int y, ih = (int)(w->h + 0.5f);
    if (ih > 22) ih = 22;
    for (y = 3; y < ih - 3; y++) {
        const unsigned char *p = src + (size_t)(y * WSC(w)) * WRB(w) + 4 * (x * WSC(w));
        float a = p[0] / 255.0f, L;
        if (a <= 0.01f) continue;
        L = (0.3f * p[1] + 0.59f * p[2] + 0.11f * p[3]) / 255.0f / a;
        if (a * (1.0f - L) > 0.3f) return 1;
    }
    return 0;
}
/* Title under x: the run of inked columns around it (gaps < 9 px belong to one title). */
static int oah_mb_title_at(oah_cgswin_t *w, int x, int *x0, int *x1) {
    int iw = (int)(w->w + 0.5f), a, b, gap, k, first = -1;
    if (iw > g_cgs_rb / 4) iw = g_cgs_rb / 4;
    for (k = 0; k < iw && first < 0; k++) if (oah_mb_colink(w, k)) first = k;
    if (first < 0 || x < first - 6) {                    /* the Apple menu slot */
        if (x >= 0 && x < 46) { *x0 = 8; *x1 = 44; return 1; }
        return 0;
    }
    for (a = x; a > 0; a--) {                           /* left edge */
        for (gap = 0; gap < 9 && a - gap > 0 && !oah_mb_colink(w, a - gap); gap++) ;
        if (gap >= 9) break;
        a -= gap;
    }
    for (b = x; b < iw - 1; b++) {                      /* right edge */
        for (gap = 0; gap < 9 && b + gap < iw && !oah_mb_colink(w, b + gap); gap++) ;
        if (gap >= 9) break;
        b += gap;
    }
    if (b - a < 4) return 0;
    *x0 = a - 8; *x1 = b + 9;
    return 1;
}
static void oah_mb_render(oah_cgswin_t *w);
/* Mouse in the bar while the guest tracks its menus: move Mojave's highlight along. */
static void oah_mb_track(int type, float x, float y) {
    oah_cgswin_t *w = oah_cgs_win(g_menubar_wid);
    int x0, x1;
    if (!w) return;
    if (type == 1) { g_mb_tracking = 1; g_mb_trackt = CFAbsoluteTimeGetCurrent(); }
    if (!g_mb_tracking) return;
    if (type == 1) {
        int ok = oah_mb_title_at(w, (int)x, &x0, &x1);
        CGSLOG("[cgs] menu bar click at %.0f,%.0f -> title %d (%d..%d)\n", x, y, ok, ok ? x0 : 0, ok ? x1 : 0);
    }
    if (y >= 0 && y < 22 && oah_mb_title_at(w, (int)x, &x0, &x1)) {
        if (type == 1) {                                  /* menus opened from here on are this click's */
            int i; unsigned long mx = 0;
            for (i = 0; i < OAH_MAXWIN; i++) if (g_cgswin[i].wid && g_cgswin[i].seq > mx) mx = g_cgswin[i].seq;
            g_mb_trackseq = mx;
        }
        if (x0 != g_mb_hx0 || x1 != g_mb_hx1) {
            g_mb_hx0 = x0; g_mb_hx1 = x1; oah_mb_render(w);
        }
    }
}
/* A window was ordered out / released: when no menu window of this tracking is left, unhighlight. */
static void oah_mb_menu_closed(void) {
    int i;
    oah_cgswin_t *w = oah_cgs_win(g_menubar_wid);
    if (!g_mb_tracking) return;
    for (i = 0; i < OAH_MAXWIN; i++)
        if (g_cgswin[i].wid && g_cgswin[i].seq > g_mb_trackseq && g_cgswin[i].ordered && g_cgswin[i].wid != g_menubar_wid) return;
    if (CFAbsoluteTimeGetCurrent() - g_mb_trackt < 0.3) return;     /* the menu has not opened yet (oah_mb_tick checks again) */
    CGSLOG("[cgs] menu bar: menu closed, tracking ends\n");
    g_mb_tracking = 0; g_mb_hx0 = g_mb_hx1 = 0;
    if (w) oah_mb_render(w);
}
/* Restyle the guest's menu bar pixels into the two image windows. */
static void oah_mb_render(oah_cgswin_t *w) {
    NCGS(int, CGSNewWindow, (unsigned int, int, float, float, void *, unsigned int *));
    NCGS(int, CGSSetWindowOpacity, (unsigned int, unsigned int, int));
    NCGS(void *, CGWindowContextCreate, (unsigned int, unsigned int, void *));
    NCGS(void *, CGColorSpaceCreateDeviceRGB, (void));
    NCGS(void *, CGDataProviderCreateWithData, (void *, const void *, size_t, void *));
    NCGS(void *, CGImageCreate, (size_t, size_t, size_t, size_t, size_t, void *, unsigned int, void *, const float *, int, int));
    NCGS(void, CGContextDrawImage, (void *, oah_rect_t, void *));
    NCGS(void, CGContextSetBlendMode, (void *, int));
    NCGS(void, CGContextClearRect, (void *, oah_rect_t));
    NCGS(void, CGContextFlush, (void *));
    NCGS(void, CGImageRelease, (void *));
    NCGS(void, CGDataProviderRelease, (void *));
    static void *cs;
    int iw = (int)(w->w + 0.5f), ih = (int)(w->h + 0.5f), x, y, k, dark = oah_mb_dark();
    unsigned char *src = oah_cgs_bits(w);
    oah_rect_t r;
    if (iw <= 0 || ih <= 0) return;
    if (ih > 22) ih = 22;
    if (iw > g_cgs_rb / 4) iw = g_cgs_rb / 4;
    if (access("/tmp/oah_mbdump", 0) == 0) {   /* debug: dump the guest bar's pixels (ARGB) */
        int x0 = 0, xx;
        unlink("/tmp/oah_mbdump");
        for (xx = 0; xx < iw && !x0; xx++) { const unsigned char *p = src + 6 * g_cgs_rb + 4 * xx; if (p[0] > 128 && p[3] - p[1] > 64) x0 = xx; }
        fprintf(stderr, "[mbdump] iw %d ih %d first blue column %d\n", iw, ih, x0);
        for (y = 0; y < ih; y++) {
            char line[2048]; int o = 0;
            for (xx = x0 > 2 ? x0 - 2 : 0; xx < x0 + 40 && xx < iw; xx++) {
                const unsigned char *p = src + y * g_cgs_rb + 4 * xx;
                o += snprintf(line + o, sizeof line - o, "%02x%02x%02x%02x ", p[0], p[1], p[2], p[3]);
            }
            fprintf(stderr, "[mbdump] %2d %s\n", y, line);
        }
    }
    {   /* iWork's 10.6 bar is redrawn (identically) 50-75 times a second; restyling and
           re-publishing every one of them cost Keynote ~50% CPU at idle. Skip unchanged frames. */
        static unsigned int last_h; static int last_hx0 = -1, last_hx1 = -1, last_dark = -1, last_iw;
        unsigned int h = 2166136261u; const unsigned int *q;
        for (y = 0; y < ih; y++) {
            q = (const unsigned int *)(src + (size_t)(y * WSC(w)) * WRB(w));
            for (x = 0; x < iw; x++) h = (h ^ q[x * WSC(w)]) * 16777619u;
        }
        if (g_mbimg[0] && iw == g_mb_w && h == last_h && g_mb_hx0 == last_hx0 && g_mb_hx1 == last_hx1 &&
            dark == last_dark && iw == last_iw && !g_mb_force) return;
        g_mb_force = 0;
        last_h = h; last_hx0 = g_mb_hx0; last_hx1 = g_mb_hx1; last_dark = dark; last_iw = iw;
    }
    if (!g_mbimg[0] || iw != g_mb_w) {
        for (k = 0; k < 2; k++) {
            if (!g_mbimg[k]) {
                oah_rect_t b; void *nr;
                b.x = 0; b.y = 0; b.w = (float)iw; b.h = 22;
                nr = oah_cgs_nregion(&b);
                p_CGSNewWindow(g_ncid, 2, 0, 0, nr, &g_mbimg[k]);
                oah_cgs_nrelease(nr);
                p_CGSSetWindowOpacity(g_ncid, g_mbimg[k], 0);
                oah_set_window_scale(g_mbimg[k], oah_display_scale());   /* Retina: text drawn at 2x */
                {   /* no window shadow: the image windows' shadow is a blurred dark copy of the
                       titles under them (visible in light mode; dark mode hides it).
                       kCGSNoShadowTagBit = 1 << 3. OAH_MB_SHADOW=1 keeps the shadow. */
                    NCGS(int, CGSSetWindowTags, (unsigned int, unsigned int, unsigned int *, int));
                    unsigned int tg[2] = { 1u << 3, 0 };
                    if (p_CGSSetWindowTags && !getenv("OAH_MB_SHADOW")) p_CGSSetWindowTags(g_ncid, g_mbimg[k], tg, 64);
                }
            }
            if (!g_mbctx[k]) g_mbctx[k] = p_CGWindowContextCreate(g_ncid, g_mbimg[k], 0);
        }
        free(g_mbpix);
        g_mbpix = (unsigned int *)malloc((size_t)iw * 22 * 4);
        g_mb_w = iw;
    }
    if (!cs) cs = p_CGColorSpaceCreateDeviceRGB();
    /* The guest's own title highlight (a title left selected while e.g. About runs): a 10.6
       blue gradient with WHITE text. Classifying pixel by pixel kept the blue but dropped the
       white text as "no ink" (transparent), so Mojave's dark bar showed through the letters:
       dark text with white fringes. Find the span by its columns instead (rows 2-3 hold only the
       gradient) and take each pixel's whiteness over that row's blue as the text. */
    static unsigned char *hlcol; static int hlcap;
    int hl_any = 0;
    if (hlcap < iw) { free(hlcol); hlcol = (unsigned char *)malloc(iw); hlcap = iw; }
    for (x = 0; x < iw; x++) {
        const unsigned char *p2 = src + (size_t)(2 * WSC(w)) * WRB(w) + 4 * (x * WSC(w)), *p3 = src + (size_t)(3 * WSC(w)) * WRB(w) + 4 * (x * WSC(w));
        hlcol[x] = ih > 4 && p2[0] > 200 && p3[0] > 200 && p2[3] - p2[1] > 64 && p3[3] - p3[1] > 64;
        hl_any |= hlcol[x];
    }
    int nsp = 0, sp0[OAH_MB_MAXT], sp1[OAH_MB_MAXT], spbold[OAH_MB_MAXT], native = 0;
    if (!getenv("OAH_MB_PIXELS")) {
        /* while a title is highlighted (menu tracking, or a title the guest left selected) its span
           is the whole highlight, not the text: keep the layout matched on the plain bar */
        static int c_nsp, c_s0[OAH_MB_MAXT], c_s1[OAH_MB_MAXT], c_bo[OAH_MB_MAXT], c_got;
        static CFStringRef c_m[OAH_MB_MAXT];
        if ((hl_any || g_mb_tracking) && c_nsp) {
            nsp = c_nsp; memcpy(sp0, c_s0, sizeof sp0); memcpy(sp1, c_s1, sizeof sp1); memcpy(spbold, c_bo, sizeof spbold);
            memcpy(g_mbmatch, c_m, sizeof c_m); native = c_got > 0;
        } else {
        oah_mb_fetch_titles();
        nsp = oah_mb_spans(w, iw, sp0, sp1, OAH_MB_MAXT);
        {   int who[OAH_MB_MAXT], bo[OAH_MB_MAXT], got = nsp > 0 && g_mbt_n > 0 ? oah_mb_match(nsp, sp0, sp1, who, bo) : 0;
            static int lastn = -1, lasts = -1, lastg = -1;
            native = got > 0;
            for (k = 0; k < nsp; k++) spbold[k] = bo[k];
            if (g_mbt_n != lastn || nsp != lasts || got != lastg)
                CGSLOG("[cgs] menu bar: %d guest menus, %d title spans, %d drawn natively\n", g_mbt_n, nsp, got);
            lastn = g_mbt_n; lasts = nsp; lastg = got;
            c_nsp = nsp; memcpy(c_s0, sp0, sizeof c_s0); memcpy(c_s1, sp1, sizeof c_s1); memcpy(c_bo, spbold, sizeof c_bo);
            for (k = 0; k < OAH_MB_MAXT; k++) { if (c_m[k]) CFRelease(c_m[k]); c_m[k] = k < nsp && g_mbmatch[k] ? (CFStringRef)CFRetain(g_mbmatch[k]) : 0; }
            c_got = got; }
        }
    }
    for (k = 0; k < 2; k++) {
        float tr = dark ? 1.0f : 0.0f, inactive = k ? 0.55f : 1.0f;
        memset(g_mbpix, 0, (size_t)iw * 22 * 4);
        if (!k && g_mb_hx1 > g_mb_hx0)
            for (y = ih; y < 22; y++) for (x = g_mb_hx0 < 0 ? 0 : g_mb_hx0; x < g_mb_hx1 && x < iw; x++) g_mbpix[y * iw + x] = 0xff007aff;
        for (y = 0; y < ih; y++) {
            const unsigned char *p = src + (size_t)(y * WSC(w)) * WRB(w);
            unsigned int *o = g_mbpix + y * iw;
            float bgr = -1;                         /* this row's highlight red (the gradient) */
            if (hl_any) for (x = 0; x < iw; x++) if (hlcol[x]) { bgr = src[(size_t)(y * WSC(w)) * WRB(w) + 4 * (x * WSC(w)) + 1] / 255.0f; break; }
            for (x = 0; x < iw; x++, p += 4 * WSC(w)) {
                float a = p[0] / 255.0f, rr, gg, bb, L, ink, oa, or_, og, ob;
                if (hl_any && hlcol[x] && bgr >= 0 && !(!k && g_mb_hx1 > g_mb_hx0 && x >= g_mb_hx0 && x < g_mb_hx1)) {
                    float wt = bgr < 0.98f ? (p[1] / 255.0f - bgr) / (1.0f - bgr) : 0;   /* white text coverage */
                    if (wt < 0) wt = 0; if (wt > 1) wt = 1;
                    if (a < 0.5f) wt = 0;
                    if (!k) {                                         /* selection blue, white text */
                        or_ = wt; og = 0.478f + wt * 0.522f; ob = 1.0f;
                        o[x] = 0xff000000u | ((unsigned int)(or_ * 255) << 16) | ((unsigned int)(og * 255) << 8) | (unsigned int)(ob * 255);
                    } else if (wt > 0) {                              /* inactive bar: a plain title */
                        oa = wt * inactive; or_ = og = ob = tr;
                        o[x] = ((unsigned int)(oa * 255) << 24) | ((unsigned int)(or_ * oa * 255) << 16) |
                               ((unsigned int)(og * oa * 255) << 8) | (unsigned int)(ob * oa * 255);
                    }
                    continue;
                }
                if (!k && g_mb_hx1 > g_mb_hx0 && x >= g_mb_hx0 && x < g_mb_hx1) {
                    /* Mojave's selected title: selection blue, white text */
                    float t = 0;
                    if (a > 0.01f) {
                        L = (0.3f * p[1] + 0.59f * p[2] + 0.11f * p[3]) / 255.0f / a;
                        t = (a * (1.0f - L) - 0.12f) / 0.6f;
                        if (t < 0) t = 0; if (t > 1) t = 1;
                    }
                    or_ = t; og = 0.478f + t * 0.522f; ob = 1.0f;
                    o[x] = 0xff000000u | ((unsigned int)(or_ * 255) << 16) | ((unsigned int)(og * 255) << 8) | (unsigned int)(ob * 255);
                    continue;
                }
                if (a <= 0.01f) continue;
                rr = p[1] / 255.0f / a; gg = p[2] / 255.0f / a; bb = p[3] / 255.0f / a;
                if (rr > 1) rr = 1; if (gg > 1) gg = 1; if (bb > 1) bb = 1;
                L = 0.3f * rr + 0.59f * gg + 0.11f * bb;
                if (!k && bb - rr > 0.25f && a > 0.5f) {
                    /* the guest's blue title highlight: Mojave's selection blue, its white text kept */
                    float wh = (L - 0.35f) / 0.6f;
                    if (wh < 0) wh = 0; if (wh > 1) wh = 1;
                    or_ = 0.0f + wh * 1.0f; og = 0.478f + wh * 0.522f; ob = 1.0f;
                    oa = 1.0f;
                } else {
                    ink = a * (1.0f - L);          /* dark text on the light 10.6 bar */
                    oa = (ink - 0.12f) / 0.6f;
                    if (oa <= 0) continue;
                    if (oa > 1) oa = 1;
                    oa *= inactive;
                    or_ = og = ob = tr;
                }
                o[x] = ((unsigned int)(oa * 255) << 24) | ((unsigned int)(or_ * oa * 255) << 16) |
                       ((unsigned int)(og * oa * 255) << 8) | (unsigned int)(ob * oa * 255);
            }
        }
        if (g_mbctx[k]) {
            void *dp = p_CGDataProviderCreateWithData(0, g_mbpix, (size_t)iw * 22 * 4, 0);
            /* 0x2000 = kCGBitmapByteOrder32Little, 2 = kCGImageAlphaPremultipliedFirst */
            void *img = p_CGImageCreate(iw, 22, 8, 32, iw * 4, cs, 0x2000 | 2, dp, 0, 0, 0);
            r.x = 0; r.y = 0; r.w = (float)iw; r.h = 22;
            p_CGContextSetBlendMode(g_mbctx[k], 17);
            if (native) {
                /* clear, the selection, then each title in Mojave's own font */
                NCGS(void, CGContextFillRect, (void *, oah_rect_t));
                NCGS(void, CGContextSetRGBFillColor, (void *, float, float, float, float));
                int j, hlsel = !k && g_mb_hx1 > g_mb_hx0;
                p_CGContextClearRect(g_mbctx[k], r);
                p_CGContextSetBlendMode(g_mbctx[k], 0);
                if (hlsel) {
                    oah_rect_t hr; hr.x = (float)g_mb_hx0; hr.y = 0; hr.w = (float)(g_mb_hx1 - g_mb_hx0); hr.h = 22;
                    p_CGContextSetRGBFillColor(g_mbctx[k], 0, 0.478f, 1, 1);
                    p_CGContextFillRect(g_mbctx[k], hr);
                }
                for (j = 0; j < nsp; j++) {
                    float cx = (sp0[j] + sp1[j]) / 2.0f, c = tr;
                    int on = hlsel && cx >= g_mb_hx0 && cx < g_mb_hx1;
                    int gsel = !k && !hlsel && hl_any && hlcol[(sp0[j] + sp1[j]) / 2];   /* the guest left this title selected */
                    if (!g_mbmatch[j] || !oah_mb_draw_title_ok()) {
                        /* an icon title: the restyled guest pixels of this span only */
                        NCGS(void, CGContextSaveGState, (void *));
                        NCGS(void, CGContextRestoreGState, (void *));
                        NCGS(void, CGContextClipToRect, (void *, oah_rect_t));
                        oah_rect_t cr; cr.x = sp0[j] - 9.0f; cr.y = 0; cr.w = (float)(sp1[j] - sp0[j]) + 18.0f; cr.h = 22;
                        p_CGContextSaveGState(g_mbctx[k]); p_CGContextClipToRect(g_mbctx[k], cr);
                        p_CGContextSetBlendMode(g_mbctx[k], 17); p_CGContextDrawImage(g_mbctx[k], r, img);
                        p_CGContextRestoreGState(g_mbctx[k]);
                        continue;
                    }
                    if (gsel) {
                        oah_rect_t hr; hr.x = sp0[j] - 8.0f; hr.y = 0; hr.w = (float)(sp1[j] - sp0[j]) + 17.0f; hr.h = 22;
                        p_CGContextSetRGBFillColor(g_mbctx[k], 0, 0.478f, 1, 1);
                        p_CGContextFillRect(g_mbctx[k], hr);
                    }
                    if (on || gsel) c = 1;
                    oah_mb_draw_title(g_mbctx[k], g_mbmatch[j], spbold[j], cx, c, c, c, (on || gsel) ? 1.0f : inactive);
                }
            }
            if (!native) { p_CGContextSetBlendMode(g_mbctx[k], 17); p_CGContextDrawImage(g_mbctx[k], r, img); }
            {   /* the 10.6 bar leaves the Apple menu's slot empty (its logo came from elsewhere): draw Mojave's */
                NCGS(void, CGContextSetBlendMode, (void *, int));
                NCGS(void, CGContextSelectFont, (void *, const char *, float, int));
                NCGS(void, CGContextSetRGBFillColor, (void *, float, float, float, float));
                NCGS(void, CGContextShowTextAtPoint, (void *, float, float, const char *, size_t));
                NCGS(void *, CGFontCreateWithFontName, (CFStringRef));
                NCGS(unsigned short, CGFontGetGlyphWithGlyphName, (void *, CFStringRef));
                NCGS(void, CGContextSetFont, (void *, void *));
                NCGS(void, CGContextSetFontSize, (void *, float));
                NCGS(void, CGContextShowGlyphsAtPoint, (void *, float, float, const unsigned short *, size_t));
                static void *af;
                static unsigned short ag;
                if (!af && p_CGFontCreateWithFontName) {
                    af = p_CGFontCreateWithFontName(CFSTR("HelveticaNeue"));
                    if (af) ag = p_CGFontGetGlyphWithGlyphName(af, CFSTR("apple"));
                }
                (void)p_CGContextSelectFont; (void)p_CGContextShowTextAtPoint;
                if (af && ag && p_CGContextShowGlyphsAtPoint && !getenv("OAH_MB_NOAPPLE")) {
                    int hl = !k && g_mb_hx0 < 30 && g_mb_hx1 > 30;
                    float c = (dark || hl) ? 1.0f : 0.0f;
                    p_CGContextSetBlendMode(g_mbctx[k], 0);
                    p_CGContextSetFont(g_mbctx[k], af);
                    p_CGContextSetFontSize(g_mbctx[k], 17);
                    p_CGContextSetRGBFillColor(g_mbctx[k], c, c, c, k ? 0.55f : 1.0f);
                    p_CGContextShowGlyphsAtPoint(g_mbctx[k], 18, 5, &ag, 1);
                }
            }
            p_CGContextFlush(g_mbctx[k]);
            p_CGImageRelease(img);
            p_CGDataProviderRelease(dp);
        }
    }
    if (getenv("OAH_MBDUMP")) {                 /* debug: the guest's own menu bar as a PPM over white */
        FILE *fp = fopen("/tmp/guest_menubar.ppm", "wb");
        if (fp) {
            fprintf(fp, "P6 %d %d 255\n", iw, ih);
            for (y = 0; y < ih; y++) for (x = 0; x < iw; x++) {
                const unsigned char *p = src + y * g_cgs_rb + 4 * x;
                unsigned char rgb[3];
                for (k = 0; k < 3; k++) rgb[k] = (unsigned char)(p[1 + k] + (255 - p[0]));
                fwrite(rgb, 1, 3, fp);
            }
            fclose(fp);
        }
    }
    /* Mojave composites the image windows when the bars are set: republish on appearance or highlight change */
    /* (a redraw of the guest's titles needs the same: just republish after every render) */
    pthread_mutex_lock(&g_mb_mu);
    oah_mb_publish();
    pthread_mutex_unlock(&g_mb_mu);
    if (++w->flushes <= 3) CGSLOG("[cgs] menu bar restyled into Mojave's bar (%dx%d)\n", iw, ih);
}

/* Blits run on the main thread and, for a window whose bits were just unlocked, on the event
   thread (oah_unlock_blit_run). CGSReleaseWindow unmaps the store under the same lock: Word 2004
   crashed in CGContextDrawImage on the event thread reading a store released a moment before. */
static pthread_mutex_t g_blit_mu;
static pthread_once_t g_blit_once = PTHREAD_ONCE_INIT;
static void oah_blit_mu_init(void) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_blit_mu, &a); pthread_mutexattr_destroy(&a);
}
static void oah_blit_lock(void) { pthread_once(&g_blit_once, oah_blit_mu_init); pthread_mutex_lock(&g_blit_mu); }
static void oah_blit_unlock(void) { pthread_mutex_unlock(&g_blit_mu); }
static void oah_cgs_blit_unl(oah_cgswin_t *w);
/* Retina windows on screen at 2x. The window contexts Mojave gives a non-AppKit client stay 1x
   whatever the window's resolution, so the 2x store also goes to a CGS surface of the window with
   resolution 2, drawn by a CGL context whose backing size is the store's pixel size: a texture of
   the store on one quad, device pixel for device pixel (tested: 1-pixel stripes reach the screen).
   The 1x CG blit still runs underneath (window shadow, shape, screenshots of the backing).
   Windows with a guest OpenGL surface keep the CG path only. OAH_HIDPI_CGONLY=1: CG path only. */
typedef struct { float x, y, w, h; } oah_hdrect_t;
static void oah_hd_release(oah_cgswin_t *w) {
    void (*destroy)(void *) = (void (*)(void *))oah_ogl_sym("CGLDestroyContext");
    int (*rmsurf)(unsigned int, unsigned int, unsigned int) = (int (*)(unsigned int, unsigned int, unsigned int))dlsym(RTLD_DEFAULT, "CGSRemoveSurface");
    if (w->hctx && destroy) destroy(w->hctx);
    if (w->hsid && rmsurf) rmsurf(g_ncid, w->wid, w->hsid);
    w->hctx = 0; w->hsid = 0; w->htex = 0; w->htw = w->hth = 0;
}
static int oah_hd_present(oah_cgswin_t *w, unsigned char *bits, size_t wrb, int iw, int ih, int y0, int y1) {
    static int off = -1;
    static int (*addsurf)(unsigned int, unsigned int, unsigned int *);
    static int (*setsb)(unsigned int, unsigned int, unsigned int, oah_hdrect_t);
    static int (*ordsurf)(unsigned int, unsigned int, unsigned int, int, unsigned int);
    static int (*setres)(unsigned int, unsigned int, unsigned int, double);
    static int (*setopq)(unsigned int, unsigned int, unsigned int, int);
    static int (*choose)(const int *, void **, int *), (*create)(void *, void *, void **), (*setsurf)(void *, unsigned int, unsigned int, unsigned int);
    static int (*setcur)(void *), (*setparam)(void *, int, const int *), (*enable)(void *, int), (*update)(void *), (*flushd)(void *);
    static void *(*getcur)(void);
    static void (*pfdestroy)(void *);
    static void (*gGen)(int, unsigned int *), (*gBind)(unsigned int, unsigned int), (*gTexP)(unsigned int, unsigned int, int);
    static void (*gTexI)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void *);
    static void (*gTexS)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void *);
    static void (*gVp)(int, int, int, int), (*gEn)(unsigned int), (*gDis)(unsigned int), (*gBegin)(unsigned int), (*gEnd)(void);
    static void (*gTc)(float, float), (*gV)(float, float), (*gPix)(unsigned int, int), (*gClear)(unsigned int), (*gCC)(float, float, float, float);
    void *prev;
    int full = 0;
    if (off < 0) {
        off = getenv("OAH_HIDPI_CGONLY") ? 1 : 0;
        addsurf = (int (*)(unsigned int, unsigned int, unsigned int *))dlsym(RTLD_DEFAULT, "CGSAddSurface");
        setsb = (int (*)(unsigned int, unsigned int, unsigned int, oah_hdrect_t))dlsym(RTLD_DEFAULT, "CGSSetSurfaceBounds");
        ordsurf = (int (*)(unsigned int, unsigned int, unsigned int, int, unsigned int))dlsym(RTLD_DEFAULT, "CGSOrderSurface");
        setres = (int (*)(unsigned int, unsigned int, unsigned int, double))dlsym(RTLD_DEFAULT, "CGSSetSurfaceResolution");
        setopq = (int (*)(unsigned int, unsigned int, unsigned int, int))dlsym(RTLD_DEFAULT, "CGSSetSurfaceOpacity");
        *(void **)&choose = oah_ogl_sym("CGLChoosePixelFormat"); *(void **)&create = oah_ogl_sym("CGLCreateContext");
        *(void **)&setsurf = oah_ogl_sym("CGLSetSurface"); *(void **)&setcur = oah_ogl_sym("CGLSetCurrentContext");
        *(void **)&getcur = oah_ogl_sym("CGLGetCurrentContext"); *(void **)&setparam = oah_ogl_sym("CGLSetParameter");
        *(void **)&enable = oah_ogl_sym("CGLEnable"); *(void **)&update = oah_ogl_sym("CGLUpdateContext");
        *(void **)&flushd = oah_ogl_sym("CGLFlushDrawable"); *(void **)&pfdestroy = oah_ogl_sym("CGLDestroyPixelFormat");
        *(void **)&gGen = oah_ogl_sym("glGenTextures"); *(void **)&gBind = oah_ogl_sym("glBindTexture");
        *(void **)&gTexP = oah_ogl_sym("glTexParameteri"); *(void **)&gTexI = oah_ogl_sym("glTexImage2D");
        *(void **)&gTexS = oah_ogl_sym("glTexSubImage2D"); *(void **)&gVp = oah_ogl_sym("glViewport");
        *(void **)&gEn = oah_ogl_sym("glEnable"); *(void **)&gDis = oah_ogl_sym("glDisable");
        *(void **)&gBegin = oah_ogl_sym("glBegin"); *(void **)&gEnd = oah_ogl_sym("glEnd");
        *(void **)&gTc = oah_ogl_sym("glTexCoord2f"); *(void **)&gV = oah_ogl_sym("glVertex2f");
        *(void **)&gPix = oah_ogl_sym("glPixelStorei"); *(void **)&gClear = oah_ogl_sym("glClear"); *(void **)&gCC = oah_ogl_sym("glClearColor");
        if (!addsurf || !setsb || !ordsurf || !setres || !choose || !create || !setsurf || !setcur || !getcur || !setparam ||
            !enable || !update || !flushd || !gGen || !gBind || !gTexP || !gTexI || !gTexS || !gVp || !gEn || !gBegin || !gEnd ||
            !gTc || !gV || !gPix) { off = 1; fprintf(stderr, "[hidpi] surface presentation unavailable (symbols)\n"); }
    }
    if (off || w->hfail || w->glsurf) return 0;
    prev = getcur();
    if (!w->hctx) {
        /* 73 accelerated, 5 double buffer, 8 color size, 11 alpha size */
        int at[] = { 73, 5, 8, 24, 11, 8, 0 };
        void *pf = 0, *ctx = 0; int np = 0, e;
        if (!w->hsid && addsurf(g_ncid, w->wid, &w->hsid)) { w->hfail = 1; fprintf(stderr, "[hidpi] window %u: CGSAddSurface failed\n", w->wid); return 0; }
        setres(g_ncid, w->wid, w->hsid, 2.0);
        if (setopq) setopq(g_ncid, w->wid, w->hsid, w->opaque ? 1 : 0);
        choose(at, &pf, &np);
        if (pf) { create(pf, 0, &ctx); if (pfdestroy) pfdestroy(pf); }
        if (!ctx) { w->hfail = 1; fprintf(stderr, "[hidpi] window %u: no CGL context\n", w->wid); oah_hd_release(w); return 0; }
        if (!w->opaque) { int z = 0; setparam(ctx, 236 /* kCGLCPSurfaceOpacity */, &z); }
        e = setsurf(ctx, g_ncid, w->wid, w->hsid);
        if (e) { w->hctx = ctx; oah_hd_release(w); w->hfail = 1; fprintf(stderr, "[hidpi] window %u: CGLSetSurface -> %d\n", w->wid, e); return 0; }
        w->hctx = ctx;
        CGSLOG("[hidpi] window %u: surface %u, context %p\n", w->wid, w->hsid, ctx);
    }
    setcur(w->hctx);
    if (w->htw != iw || w->hth != ih) {
        oah_hdrect_t b; int bs[2];
        b.x = 0; b.y = 0; b.w = (float)iw / 2; b.h = (float)ih / 2;
        setsb(g_ncid, w->wid, w->hsid, b);
        ordsurf(g_ncid, w->wid, w->hsid, 1, 0);
        bs[0] = iw; bs[1] = ih;
        setparam(w->hctx, 304 /* kCGLCPSurfaceBackingSize */, bs);
        enable(w->hctx, 305 /* kCGLCESurfaceBackingSize */);
        update(w->hctx);
        if (!w->htex) gGen(1, &w->htex);
        gBind(0x84F5 /* GL_TEXTURE_RECTANGLE_ARB */, w->htex);
        gTexP(0x84F5, 0x2801, 0x2600); gTexP(0x84F5, 0x2800, 0x2600);   /* MIN/MAG = NEAREST */
        gPix(0x0CF2 /* GL_UNPACK_ROW_LENGTH */, (int)(wrb / 4));
        /* store bytes A,R,G,B = GL_BGRA with GL_UNSIGNED_INT_8_8_8_8 on a little-endian host */
        gTexI(0x84F5, 0, 0x1908 /* RGBA */, iw, ih, 0, 0x80E1 /* BGRA */, 0x8035 /* 8_8_8_8 */, bits);
        w->htw = iw; w->hth = ih;
        full = 1;
        CGSLOG("[hidpi] window %u: %dx%d pixels\n", w->wid, iw, ih);
    }
    gBind(0x84F5, w->htex);
    if (!full && y1 > y0) {
        gPix(0x0CF2, (int)(wrb / 4));
        gTexS(0x84F5, 0, 0, y0, iw, y1 - y0, 0x80E1, 0x8035, bits + (size_t)y0 * wrb);
    }
    gVp(0, 0, iw, ih);
    gEn(0x84F5);
    if (gDis) gDis(0x0BE2 /* GL_BLEND */);
    if (!w->opaque && gCC && gClear) { gCC(0, 0, 0, 0); gClear(0x4000); }
    gBegin(7 /* GL_QUADS */);
    gTc(0, (float)ih); gV(-1, -1); gTc((float)iw, (float)ih); gV(1, -1);
    gTc((float)iw, 0); gV(1, 1); gTc(0, 0); gV(-1, 1);
    gEnd();
    flushd(w->hctx);
    setcur(prev);
    return 1;
}
/* A held-back move on screen (see CGSMoveWindowWithGroup): make it now. */
static pthread_mutex_t g_reveal_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_reveal_n;
static volatile int g_in_ulb;      /* in oah_unlock_blit_run (diagnostics; the event thread) */
static void oah_cgs_expose_now(oah_cgswin_t *w);
static int g_shex_n;            /* sheets waiting for their exposure check (CGSCreate*SheetWindowAnimation*) */
static void oah_cgs_reveal(oah_cgswin_t *w) {
    static int (*mwg)(unsigned int, unsigned int, const oah_pt_t *), init;
    NCGS(int, CGSMoveWindow, (unsigned int, unsigned int, const oah_pt_t *));
    oah_pt_t p;
    pthread_mutex_lock(&g_reveal_mu);
    if (!w->pmv) { pthread_mutex_unlock(&g_reveal_mu); return; }
    w->pmv = 0; g_reveal_n--;
    p.x = w->pmx; p.y = w->pmy;
    pthread_mutex_unlock(&g_reveal_mu);
    if (!init) { init = 1; if (!getenv("OAH_NO_MOVEGROUP")) *(void **)&mwg = dlsym(RTLD_DEFAULT, "CGSMoveWindowWithGroup"); }
    if (mwg) mwg(oah_cgs_cid(), w->wid, &p); else if (p_CGSMoveWindow) p_CGSMoveWindow(oah_cgs_cid(), w->wid, &p);
    CGSLOG("[cgs] window %u moved on screen to %.0f,%.0f (%.0f ms after the move)\n", w->wid, p.x, p.y, 1000 * (CFAbsoluteTimeGetCurrent() - w->pmt));
}
static void oah_cgs_blit(oah_cgswin_t *w) {
    oah_blit_lock();
    if (w->wid && w->store) oah_cgs_blit_unl(w);     /* released while we waited: nothing to do */
    oah_blit_unlock();
}
static void oah_cgs_blit_unl(oah_cgswin_t *w) {
    NCGS(void *, CGWindowContextCreate, (unsigned int, unsigned int, void *));
    NCGS(void *, CGColorSpaceCreateDeviceRGB, (void));
    NCGS(void *, CGDataProviderCreateWithData, (void *, const void *, size_t, void *));
    NCGS(void *, CGImageCreate, (size_t, size_t, size_t, size_t, size_t, void *, unsigned int, void *, const float *, int, int));
    NCGS(void, CGContextDrawImage, (void *, oah_rect_t, void *));
    NCGS(void, CGContextSetBlendMode, (void *, int));
    NCGS(void, CGContextFlush, (void *));
    NCGS(void, CGImageRelease, (void *));
    NCGS(void, CGDataProviderRelease, (void *));
    static void *cs;
    int sc = WSC(w), iw = (int)(w->w * sc + 0.5f), ih = (int)(w->h * sc + 0.5f);
    size_t wrb = (size_t)WRB(w);
    void *dp, *img;
    oah_rect_t r;
    if (iw <= 0 || ih <= 0 || !p_CGImageCreate) return;
    if (w->wid == g_menubar_wid && oah_mb_enabled()) { oah_mb_render(w); return; }
    if (sc > 1 && w->qd) oah_qd_push(w);
    if (iw > (int)wrb / 4) iw = (int)wrb / 4;
    if (ih > WROWS(w)) ih = WROWS(w);
    if (sc > 1 && !w->nres) {   /* before its context: the native backing gets 2x pixels */
        oah_set_window_scale(w->wid, (float)sc);
        w->nres = 1;
        if (w->nctx) { NCGS(void, CGContextRelease, (void *)); p_CGContextRelease(w->nctx); w->nctx = 0; }
    }
    if (!w->nctx) {
        w->nctx = p_CGWindowContextCreate(g_ncid, w->wid, 0);
        CGSLOG("[cgs] native context for window %u: %p\n", w->wid, w->nctx);
        if (w->nctx && g_cgstrace > 0) {
            typedef struct { float w, h; } oah_sz;
            oah_sz (*cv)(void *, oah_sz) = (oah_sz (*)(void *, oah_sz))dlsym(RTLD_DEFAULT, "CGContextConvertSizeToDeviceSpace");
            if (cv) { oah_sz one = { 1, 1 }, d = cv(w->nctx, one); CGSLOG("[cgs]   its device scale %.1f x %.1f\n", d.w, d.h); }
        }
        if (!w->nctx) return;
    }
    if (!cs) cs = p_CGColorSpaceCreateDeviceRGB();
    /* Only what changed. HIToolbox flushes every window at the end of each event-loop pass
       (a null region: the whole window), which 10.6's window server ignored when nothing had
       changed; each full blit here waited for the display refresh (CGSWindowSynchronizeBacking),
       so Bejeweled 2 spent its main thread on 60 window copies a second and drew 3 frames.
       Compare with the last pixels sent and blit the band of changed rows only.
       OAH_NO_BLITDIFF=1 always sends the whole window. */
    {
        static int nodiff = -1;
        unsigned char *bits = oah_cgs_bits(w);
        size_t rowb = (size_t)iw * 4;
        int y0 = 0, y1 = ih, y;
        if (nodiff < 0) nodiff = getenv("OAH_NO_BLITDIFF") ? 1 : 0;
        if (!nodiff) {
            if (!w->shadow || w->shw != iw || w->shh != ih || w->dirty) {
                free(w->shadow);
                w->shadow = (unsigned char *)malloc((size_t)ih * rowb);
                w->shw = w->shadow ? iw : 0; w->shh = w->shadow ? ih : 0;
                if (w->shadow) for (y = 0; y < ih; y++) memcpy(w->shadow + y * rowb, bits + (size_t)y * wrb, rowb);
            } else {
                for (y0 = 0; y0 < ih && !memcmp(w->shadow + y0 * rowb, bits + (size_t)y0 * wrb, rowb); y0++) ;
                if (y0 == ih) return;                                   /* nothing changed */
                for (y1 = ih; y1 > y0 && !memcmp(w->shadow + (y1 - 1) * rowb, bits + (size_t)(y1 - 1) * wrb, rowb); y1--) ;
                for (y = y0; y < y1; y++) memcpy(w->shadow + y * rowb, bits + (size_t)y * wrb, rowb);
            }
            if (getenv("OAH_BLITLOG")) {
                int nz = 0, nrgb = 0, xx, yy;
                for (yy = y0; yy < y1; yy += 4) for (xx = 0; xx < iw; xx += 4) {
                    unsigned char *px = bits + (size_t)yy * wrb + (size_t)xx * 4;
                    if (px[0]) nz++;
                    if (px[1] | px[2] | px[3]) nrgb++;
                }
                fprintf(stderr, "[blit] window %u rows %d-%d of %d (opaque %d, alpha samples %d, colour samples %d, of %d)\n", w->wid, y0, y1, ih, w->opaque, nz, nrgb, ((y1 - y0 + 3) / 4) * ((iw + 3) / 4));
                if (!nz && !nrgb && w->store) {     /* drawn somewhere else in the store? */
                    int r0 = -1, r1 = -1, c0 = 1 << 30, c1 = -1;
                    for (yy = 0; yy < WROWS(w); yy += 2) {
                        unsigned int *row = (unsigned int *)(w->store + (size_t)yy * WRB(w));
                        for (xx = 0; xx < WRB(w) / 4; xx += 2) if (row[xx]) { if (r0 < 0) r0 = yy; r1 = yy; if (xx < c0) c0 = xx; if (xx > c1) c1 = xx; }
                    }
                    fprintf(stderr, "[blit]   store %dx%d: drawn rows %d-%d cols %d-%d (window rows start %d)\n", WRB(w) / 4, WROWS(w), r0, r1, c0, c1, (int)(bits - w->store) / (int)WRB(w));
                }
            }
        }
        {   /* OAH_BLITDUMP=<wid>: every blit of that window as raw ARGB (diagnostic) */
            static int dw = -1, dn;
            if (dw < 0) dw = getenv("OAH_BLITDUMP") ? atoi(getenv("OAH_BLITDUMP")) : 0;
            /* OAH_BLITDUMP=1: every window, while /tmp/blitdump/on exists */
            static int fw = -1, fh, dmax;
            if (fw < 0) { const char *z = getenv("OAH_BLITDUMP_SIZE"); fw = fh = 0; if (z) sscanf(z, "%dx%d", &fw, &fh);
                          dmax = getenv("OAH_BLITDUMP_MAX") ? atoi(getenv("OAH_BLITDUMP_MAX")) : 600; }
            if (dw && (dw == 1 ? !access("/tmp/blitdump/on", F_OK) : (unsigned int)dw == w->wid) && dn < dmax &&
                (!fw || (iw == fw && ih == fh))) {   /* OAH_BLITDUMP_SIZE=WxH: only windows of that size */
                char fn[128]; FILE *f;
                snprintf(fn, sizeof fn, "/tmp/blitdump/%u_%04d_%d_%d_%d_%d_%s_%.3f.raw", w->wid, dn++, iw, ih, y0, y1, pthread_main_np() ? "m" : (g_in_ulb ? "u" : "t"), CFAbsoluteTimeGetCurrent());
                mkdir("/tmp/blitdump", 0755);
                if ((f = fopen(fn, "wb"))) { int yy; for (yy = 0; yy < ih; yy++) fwrite(bits + (size_t)yy * wrb, 1, (size_t)iw * 4, f); fclose(f); }
            }
        }
        if (sc > 1) { y0 &= ~1; y1 = (y1 + 1) & ~1; if (y1 > ih) y1 = ih; }   /* whole points */
        dp = p_CGDataProviderCreateWithData(0, bits + (size_t)y0 * wrb, (size_t)(y1 - y0) * wrb, 0);
        /* 0x4000 = kCGBitmapByteOrder32Big; 2 = PremultipliedFirst, 6 = NoneSkipFirst */
        img = p_CGImageCreate(iw, y1 - y0, 8, 32, wrb, cs, 0x4000 | (w->opaque ? 6 : 2), dp, 0, 0, 0);
        /* window context: bottom-left origin, in points */
        r.x = 0; r.y = (float)(ih - y1) / sc; r.w = (float)iw / sc; r.h = (float)(y1 - y0) / sc;
    }
    {
        static int fps = -1; static double t0, inblit; static unsigned long nb;
        double ta = 0;
        if (fps < 0) fps = getenv("OAH_FPS") ? 1 : 0;
        if (fps) ta = CFAbsoluteTimeGetCurrent();
        p_CGContextSetBlendMode(w->nctx, 17 /* kCGBlendModeCopy */);
        p_CGContextDrawImage(w->nctx, r, img);
        p_CGContextFlush(w->nctx);
        if (fps) {
            double tb = CFAbsoluteTimeGetCurrent();
            inblit += tb - ta; nb++;
            if (!t0) t0 = tb;
            if (tb - t0 >= 5) {
                fprintf(stderr, "[fps] window blits: %.1f/s, %.1f ms each (last: window %u %dx%d)\n", nb / (tb - t0), 1000 * inblit / nb, w->wid, iw, ih);
                t0 = tb; nb = 0; inblit = 0;
            }
        }
    }
    p_CGImageRelease(img);
    p_CGDataProviderRelease(dp);
    w->blit_t = CFAbsoluteTimeGetCurrent();
    w->shex_rows++;
    if (w->pmv) oah_cgs_reveal(w);
    if (sc > 1) {   /* the same pixels at 2x on screen (the CG blit above reaches it at 1x) */
        unsigned char *bits = oah_cgs_bits(w);
        if (getenv("OAH_HDPIXDUMP") && iw > 600 && ih > 40) {   /* diagnostic: ARGB along a row */
            int yy = atoi(getenv("OAH_HDPIXDUMP")), xx;
            if (yy < ih) { fprintf(stderr, "[hidpi] window %u row %d:", w->wid, yy);
                for (xx = 0; xx < iw && xx < 1200; xx += 100) fprintf(stderr, " %02x%02x%02x%02x", bits[yy * wrb + xx * 4], bits[yy * wrb + xx * 4 + 1], bits[yy * wrb + xx * 4 + 2], bits[yy * wrb + xx * 4 + 3]);
                fprintf(stderr, "\n"); }
        }
        int yy0 = (int)(ih - r.y * sc - r.h * sc + 0.5f), yy1 = yy0 + (int)(r.h * sc + 0.5f);
        oah_hd_present(w, bits, wrb, iw, ih, yy0, yy1);
    }
    if (++w->flushes <= 3) CGSLOG("[cgs] flushed window %u (%dx%d)\n", w->wid, iw, ih);
}

/* Create the native window and our record. */
static unsigned int oah_cgs_new_window(int type, float x, float y, unsigned int shape, unsigned int widp) {
    NCGS(int, CGSNewWindow, (unsigned int, int, float, float, void *, unsigned int *));
    oah_rect_t b;
    void *nr;
    unsigned int wid = 0;
    int err;
    oah_cgs_cid();
    /* Mojave's WindowServer only routes mouse/key events to connections of registered
       foreground apps; the guest's own CPS registration never reaches it. */
    if (!g_fg_done) { g_fg_done = 1; oah_make_foreground(); }
    {   /* Retina: new windows of this connection get 2x backings (a resolution set on a window
           afterwards is recorded, but its window contexts stay 1x) */
        static int done;
        if (!done && oah_hidpi() > 1) {
            int (*scp)(unsigned int, unsigned int, CFStringRef, CFTypeRef) =
                (int (*)(unsigned int, unsigned int, CFStringRef, CFTypeRef))oah_ncgs("CGSSetConnectionProperty");
            double r = oah_hidpi();
            CFNumberRef n = CFNumberCreate(0, kCFNumberDoubleType, &r);
            int e = scp ? scp(g_ncid, g_ncid, CFSTR("kCGSDefaultWindowResolution"), n) : -1;
            CGSLOG("[cgs] default window resolution %.0f -> %d\n", r, e);
            if (n) CFRelease(n);
            done = 1;
        }
    }
    if (!oah_cgs_region_bounds(shape, &b)) { b.x = b.y = 0; b.w = b.h = 1; }
    nr = oah_cgs_nregion(&b);
    err = p_CGSNewWindow ? p_CGSNewWindow(g_ncid, type, x, y, nr, &wid) : -1;
    oah_cgs_nrelease(nr);
    CGSLOG("[cgs] NewWindow type %d at %.0f,%.0f shape %.0f,%.0f %.0fx%.0f -> err %d wid %u\n",
           type, x, y, b.x, b.y, b.w, b.h, err, wid);
    if (err || !wid) return err ? (unsigned int)err : 1000;
    if (!oah_cgs_add(wid, type, x, y, &b)) fprintf(stderr, "[cgs] window table full\n");
    else if (oah_hidpi() > 1) {
        /* the resolution has to be set before the window gets its real shape: set on a window that
           already has its backing, it is recorded but the backing stays 1x (2x pixels downsampled) */
        oah_cgswin_t *w = oah_cgs_win(wid);
        oah_set_window_scale(wid, (float)oah_hidpi());
        if (w) w->nres = 1;
    }
    oah_put32(widp, wid);
    return 0;
}

/* is this guest context drawing into a Retina window's store? (CUIDraw fix in oah_gtrace.h) */
static int oah_cgs_is_retina_ctx(unsigned int ctx) {
    static unsigned int fdata;
    unsigned int data;
    int i, k;
    if (!ctx) return 0;
    for (i = 0; i < OAH_MAXWIN; i++) {
        oah_cgswin_t *w = &g_cgswin[i];
        if (!w->wid || WSC(w) < 2) continue;
        for (k = 0; k < w->ngctx; k++) if (w->gctx[k] == ctx) return 1;
    }
    if (!fdata) fdata = oah_cgs_gsym("_CGBitmapContextGetData");
    if (!fdata) return 0;
    {   unsigned int a[1]; a[0] = ctx; data = oah_callppc_n("CGBitmapContextGetData", fdata, 1, a); }
    if (!data) return 0;
    for (i = 0; i < OAH_MAXWIN; i++) {
        oah_cgswin_t *w = &g_cgswin[i];
        if (!w->wid || WSC(w) < 2 || !w->store) continue;
        if (data >= (unsigned int)w->store && data < (unsigned int)w->store + (unsigned int)(WROWS(w) * WRB(w))) return 1;
    }
    return 0;
}
/* a guest context that belongs to a Retina window: concatenate the 2x scale again */
static void oah_cgs_retina_ctm(unsigned int ctx) {
    static unsigned int fcat, fdata;
    int i, k;
    unsigned int data = 0;
    if (!ctx) return;
    /* which window: the contexts we made (a short list), or any bitmap context whose pixels are
       a Retina window's store (AppKit makes and drops many; a context that fell off the list
       went back to 1x on its next CGContextResetCTM and drew a quarter-size copy) */
    if (!fdata) fdata = oah_cgs_gsym("_CGBitmapContextGetData");
    for (i = 0; i < OAH_MAXWIN; i++) {
        oah_cgswin_t *w = &g_cgswin[i];
        int hit = 0;
        if (!w->wid || WSC(w) < 2) continue;
        for (k = 0; k < w->ngctx; k++) if (w->gctx[k] == ctx) hit = 1;
        if (!hit && fdata && w->store && pthread_main_np()) {
            if (!data) { unsigned int a[1]; a[0] = ctx; data = oah_callppc_n("CGBitmapContextGetData", fdata, 1, a); if (!data) data = 1; }
            if (data >= (unsigned int)w->store && data < (unsigned int)w->store + (unsigned int)(WROWS(w) * WRB(w))) hit = 1;
        }
        if (hit) {
            float two = (float)WSC(w); unsigned int tb, t[7];
            memcpy(&tb, &two, 4);
            if (!fcat) fcat = oah_cgs_gsym("_CGContextConcatCTM");
            if (!fcat) return;
            t[0] = ctx; t[1] = tb; t[2] = 0; t[3] = 0; t[4] = tb; t[5] = 0; t[6] = 0;
            oah_callppc_n("CGContextConcatCTM", fcat, 7, t);
            return;
        }
    }
}
/* Guest bitmap context over a window's store (for CGWindowContextCreate). */
static unsigned int oah_cgs_window_context(oah_cgswin_t *w) {
    static unsigned int fcs, fbc, cs;
    unsigned int a[7], ctx;
    if (!fbc) { fcs = oah_cgs_gsym("_CGColorSpaceCreateDeviceRGB"); fbc = oah_cgs_gsym("_CGBitmapContextCreate"); }
    if (!fbc || !fcs) return 0;
    if (!cs) cs = oah_callppc_n("CGColorSpaceCreateDeviceRGB", fcs, 0, a);
    a[0] = (unsigned int)w->store; a[1] = WRB(w) / 4; a[2] = WROWS(w); a[3] = 8; a[4] = WRB(w);
    a[5] = cs; a[6] = 0x4000 | 2;      /* big-endian premultiplied ARGB */
    ctx = oah_callppc_n("CGBitmapContextCreate", fbc, 7, a);
    if (ctx && WSC(w) > 1) {
        /* user space in points: a 2x base transform (CGContextResetCTM / AppKit's focus go back to
           it) and the same as the current CTM. A CGAffineTransform goes in r4-r9 (Darwin PPC passes
           structures in general registers). */
        static unsigned int fbase, fcat;
        float two = (float)WSC(w); unsigned int tb; memcpy(&tb, &two, 4);
        unsigned int t[7];
        if (!fbase) { fbase = oah_cgs_gsym("_CGContextSetBaseCTM"); fcat = oah_cgs_gsym("_CGContextConcatCTM");
                      fprintf(stderr, "[cgs] Retina contexts: CGContextSetBaseCTM %08x CGContextConcatCTM %08x\n", fbase, fcat); }
        t[0] = ctx; t[1] = tb; t[2] = 0; t[3] = 0; t[4] = tb; t[5] = 0; t[6] = 0;
        if (fbase && getenv("OAH_HIDPI_BASE")) oah_callppc_n("CGContextSetBaseCTM", fbase, 7, t);
        if (fcat && !getenv("OAH_HIDPI_NOCAT")) oah_callppc_n("CGContextConcatCTM", fcat, 7, t);
    }
    if (ctx) {
        if (w->ngctx < 8) w->gctx[w->ngctx++] = ctx;
        else { memmove(w->gctx, w->gctx + 1, 7 * sizeof w->gctx[0]); w->gctx[7] = ctx; }
    }
    CGSLOG("[cgs] CGWindowContextCreate(window %u) -> guest bitmap context 0x%08x\n", w->wid, ctx);
    return ctx;
}

/* ---- events ----
   A native pump thread pulls Mojave events off our connection (SLEventCreateNextEvent)
   into a queue and pokes a mach port that the guest HIToolbox listens on (the port it
   got from CGSGetEventPort, wrapped in a guest CFMachPort on its run loop). The guest's
   MessageHandler then calls PullEventsFromWindowServer -> CGEventCreateNextEvent, which
   is answered here with a guest CGEvent built by the guest CG itself (CGEventCreate +
   direct writes to its 10.6 CGSEventRecord + CGEventSetIntegerValueField), so the
   record layout and byte order are exactly what the guest expects.
   10.6 CGSEventRecord (at event+0xc, 0xcc bytes): +8 type, +0xc location, +0x14 window
   location, +0x1c time (u64), +0x24 flags, +0x28 window, +0x2c connection. */
#define OAH_EVQ 512
static void *g_evq[OAH_EVQ];
static int g_evq_h, g_evq_t, g_ev_pending, g_ev_started;
static pthread_mutex_t g_evq_mu = PTHREAD_MUTEX_INITIALIZER;
static double g_evq_at[OAH_EVQ];                /* when each event was queued */
static int g_evq_ty[OAH_EVQ];                   /* and its type */
static int oah_ev_ntype(void *e) {
    static int (*gt)(void *);
    if (!gt) gt = (int (*)(void *))dlsym(RTLD_DEFAULT, "CGEventGetType");
    return gt && e ? gt(e) : -1;
}
static mach_port_t g_ev_gport;                  /* guest-visible wake-up port (receive right) */
#define OAH_EXPQ 64
static unsigned int g_expq[OAH_EXPQ];
static int g_expq_n;
static unsigned long g_exp_sent;
static unsigned long g_ev_in, g_ev_out, g_ev_drop, g_ev_wakes;
static int g_evtrace = -1;
#define EVLOG(...) do { if (g_evtrace < 0) g_evtrace = (getenv("OAH_EVTRACE") || access("/tmp/oah_evtrace", 0) == 0) ? 1 : 0; \
                        if (g_evtrace) fprintf(stderr, __VA_ARGS__); } while (0)

/* App (de)activation for the guest. Mojave's window server queues the "app deactivated"
   AppKit-defined event (subtype 2) for this connection only when the app is activated again,
   so the guest never saw itself go inactive: hidesOnDeactivate panels (the Inspector, the
   Formula Editor) stayed up over other apps and windows kept their active look. The pump
   polls the CPS front process and queues the (de)activation itself; oah_ev_to_guest passes a
   (de)activation event only when it changes g_app_active, so the late native ones are
   dropped. OAH_NO_ACTSYNC=1 restores the old behaviour. */
static volatile int g_app_active;
static double g_app_act_at, g_act_click_until, g_raise_at, g_raise_all_at;
/* A mouse button or key event handed to the guest wakes its event source once more shortly
   after: Kid Pix (a Carbon app that polls Button()) took a click's down and up, or a Cmd-S,
   from us but handled them only when the next native event arrived, so its Save panel needed
   a second click and Cmd-S a second press. OAH_NO_CLICKREWAKE=1 disables. */
static volatile double g_ev_rewake_at;
static int g_hit_last_type;   /* OAH_HITLOG: type of the last event handed to the guest */
static void oah_ev_wake(void);
static volatile unsigned int g_raise_wid;
static void oah_cgs_group_follow(unsigned int parent, int mode);
static int g_actsync_off;
static void oah_ev_enqueue_locked(void *e);
static void oah_ev_wake_locked(void);
/* 1 if this process is the window server's front process, 0 if not, -1 if unknown */
static int oah_is_front(void) {
    static int (*getfront)(unsigned int *), (*getcur)(unsigned int *), init;
    static unsigned int self[2];
    unsigned int f[2] = { 0, 0 };
    if (!init) {
        init = 1;
        getfront = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "_SLPSGetFrontProcess");
        getcur = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "SLPSGetCurrentProcess");
    }
    if (getcur && !self[1]) getcur(self);
    if (!getfront || !self[1] || getfront(f) || !f[1]) return -1;
    return f[0] == self[0] && f[1] == self[1];
}
/* Pixels changed through CGSLockWindowBits (QuickDraw ports, HIToolbox and AppKit scrolling
   copy the window's own pixels that way) reached the screen only at the next flush, and a
   scroll flushes just the newly exposed strip: the moved part kept its old pixels (open panel
   sidebars and lists looked garbled until the window was redrawn). Unlocking queues a blit
   of the window shortly after (the blit sends only the rows that changed).
   OAH_NO_UNLOCKBLIT=1 disables. */
static unsigned int g_ulb_wid[8];
static double g_ulb_t[8];       /* first lock / unlock since the window was last blitted from here */
static double g_ulb_last[8];    /* the latest one */
static int g_ulb_n;
static double g_ulb_at;
static pthread_mutex_t g_ulb_mu = PTHREAD_MUTEX_INITIALIZER;
/* QuickDraw drawing that no flush follows (window bits locked and unlocked) is sent to the screen
   from the event thread. That thread runs beside the app's own drawing, so it waits until the
   window's bits have been left alone for 30 ms (or 200 ms in all, for an app that draws
   continuously): blitting 30 ms after the first unlock caught a Carbon dialog's default button
   between its erase and its redraw, and the pulsing button vanished for a frame.
   OAH_ULB_QUIET=<ms> / OAH_ULB_MAX=<ms> change the two delays. */
static double oah_ulb_quiet(void) { static double v = -1; if (v < 0) v = getenv("OAH_ULB_QUIET") ? atof(getenv("OAH_ULB_QUIET")) / 1000 : 0.03; return v; }
static int oah_ulb_old(void) { static int v = -1; if (v < 0) v = getenv("OAH_ULB_OLD") ? 1 : 0; return v; }   /* fixed timing, for A/B tests */
static double oah_ulb_max(void) { static double v = -1; if (v < 0) v = oah_ulb_old() ? 0.03 : getenv("OAH_ULB_MAX") ? atof(getenv("OAH_ULB_MAX")) / 1000 : 0.2; return v; }
static void oah_unlock_blit_queue(unsigned int wid) {
    static int off = -1;
    int i;
    double now;
    if (off < 0) off = getenv("OAH_NO_UNLOCKBLIT") ? 1 : 0;
    if (off) return;
    now = CFAbsoluteTimeGetCurrent();
    pthread_mutex_lock(&g_ulb_mu);
    for (i = 0; i < g_ulb_n && g_ulb_wid[i] != wid; i++) ;
    if (i == g_ulb_n && g_ulb_n < 8) { g_ulb_t[g_ulb_n] = now; g_ulb_wid[g_ulb_n++] = wid; }
    if (i < g_ulb_n) g_ulb_last[i] = now;
    if (!g_ulb_at) g_ulb_at = now + oah_ulb_quiet();
    pthread_mutex_unlock(&g_ulb_mu);
}
static void oah_unlock_blit_run(double t) {
    int i, k, nrun = 0;
    unsigned int run[8];
    double next = 0;
    if (g_shex_n) {                         /* shown sheets that drew nothing: send the exposure */
        int left = 0;
        for (i = 0; i < OAH_MAXWIN; i++) if (g_cgswin[i].wid && g_cgswin[i].shex_t) {
            if (t < g_cgswin[i].shex_t) { left++; continue; }
            g_cgswin[i].shex_t = 0;
            if (!g_cgswin[i].shex_rows && g_cgswin[i].ordered) {
                CGSLOG("[cgs] sheet %u drew nothing since it was shown: exposure\n", g_cgswin[i].wid);
                oah_cgs_expose_now(&g_cgswin[i]);
            }
        }
        g_shex_n = left;
    }
    if (g_reveal_n) {                       /* held-back moves: at most 100 ms */
        oah_blit_lock();
        int left = 0;
        for (i = 0; i < OAH_MAXWIN; i++) if (g_cgswin[i].wid && g_cgswin[i].pmv) {
            if (t - g_cgswin[i].pmt >= 0.1) oah_cgs_reveal(&g_cgswin[i]); else left++;
        }
        pthread_mutex_lock(&g_reveal_mu); g_reveal_n = left; pthread_mutex_unlock(&g_reveal_mu);   /* released windows drop out */
        oah_blit_unlock();
    }
    if (!g_ulb_n || t < g_ulb_at) return;
    pthread_mutex_lock(&g_ulb_mu);
    for (i = k = 0; i < g_ulb_n; i++) {
        if (t - g_ulb_last[i] >= oah_ulb_quiet() || t - g_ulb_t[i] >= oah_ulb_max()) {
            /* A window the app flushed since is on screen already, and its drawing may be going on
               right now on another thread (AppKit's default-button pulse on its heartbeat thread). */
            oah_cgswin_t *w = oah_cgs_win(g_ulb_wid[i]);
            if (w && w->ordered && w->blit_t < (oah_ulb_old() ? g_ulb_t[i] : g_ulb_last[i])) run[nrun++] = g_ulb_wid[i];
        } else {
            double d = g_ulb_last[i] + oah_ulb_quiet(), m = g_ulb_t[i] + oah_ulb_max();
            if (m < d) d = m;
            if (!next || d < next) next = d;
            g_ulb_wid[k] = g_ulb_wid[i]; g_ulb_t[k] = g_ulb_t[i]; g_ulb_last[k] = g_ulb_last[i]; k++;
        }
    }
    g_ulb_n = k; g_ulb_at = next;
    pthread_mutex_unlock(&g_ulb_mu);
    if (nrun && getenv("OAH_BLITLOG")) fprintf(stderr, "[blit] after unlock: %d window(s)\n", nrun);
    g_in_ulb = 1;
    for (i = 0; i < nrun; i++) { oah_cgswin_t *w = oah_cgs_win(run[i]); if (w) oah_cgs_blit(w); }
    g_in_ulb = 0;
}
static void oah_actsync_poll(void) {
    static int (*getfront)(unsigned int *), (*getcur)(unsigned int *), init, away;
    static unsigned int self[2];
    static double last, front_since;
    unsigned int f[2] = { 0, 0 };
    double t = CFAbsoluteTimeGetCurrent();
    int st = 0;
    oah_unlock_blit_run(t);
    if (g_raise_wid && t >= g_raise_at) {           /* delayed click-to-front (oah_ev_to_guest) */
        unsigned int wid = g_raise_wid;
        oah_cgswin_t *w = oah_cgs_win(wid);
        NCGS(int, CGSOrderWindow, (unsigned int, unsigned int, int, unsigned int));
        g_raise_wid = 0;
        if (w && w->ordered && g_app_active && p_CGSOrderWindow) {
            p_CGSOrderWindow(g_ncid, wid, 1, 0);
            oah_cgs_group_follow(wid, 1);
            CGSLOG("[cgs] activating click: window %u raised again\n", wid);
        }
    }
    /* Activation without a click (Dock, Cmd-Tab, another app's request): the 10.6 window
       server brought all of the app's windows forward; Mojave's leaves it to AppKit. Raise
       our normal-level windows above the others, keeping their own stacking order (native
       on-screen list, front to back). OAH_NO_RAISEALL=1 disables. */
    if (g_raise_all_at && t >= g_raise_all_at) {
        static int off = -1;
        g_raise_all_at = 0;
        if (off < 0) off = getenv("OAH_NO_RAISEALL") ? 1 : 0;
        if (!off && g_app_active) {
            NCGS(CFArrayRef, CGWindowListCreate, (unsigned int, unsigned int));
            NCGS(int, CGSOrderWindow, (unsigned int, unsigned int, int, unsigned int));
            CFArrayRef l = p_CGWindowListCreate ? p_CGWindowListCreate(1 /* on screen only */, 0) : 0;
            unsigned int mine[64];
            int nm = 0, k;
            if (l) {
                CFIndex i, c = CFArrayGetCount(l);
                for (i = 0; i < c && nm < 64; i++) {
                    unsigned int wid = (unsigned int)(uintptr_t)CFArrayGetValueAtIndex(l, i);
                    oah_cgswin_t *w = oah_cgs_win(wid);
                    if (w && w->level == 0 && w->ordered && !w->ghost && wid != g_menubar_wid) mine[nm++] = wid;
                }
                CFRelease(l);
            }
            for (k = nm - 1; k >= 0 && p_CGSOrderWindow; k--) {
                p_CGSOrderWindow(g_ncid, mine[k], 1, 0);
                oah_cgs_group_follow(mine[k], 1);
            }
            CGSLOG("[cgs] activation: %d window(s) brought forward\n", nm);
        }
    }
    if (g_actsync_off || t - last < 0.1) return;
    last = t;
    if (!init) {
        init = 1;
        /* the underscore variant asks the server; SLPSGetFrontProcess answers from a cache that
           only the native main run loop refreshes (it kept saying "us" after a switch) */
        getfront = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "_SLPSGetFrontProcess");
        if (!getfront) getfront = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "SLPSGetFrontProcess");
        getcur = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "SLPSGetCurrentProcess");
    }
    if (getcur && !self[1]) getcur(self);         /* only valid once registered with CPS */
    {   static int dbg = -1, n; int rc = getfront ? getfront(f) : -99;
        if (dbg < 0) dbg = getenv("OAH_ACTDBG") ? 1 : 0;
        if (dbg && (n++ % 10) == 0)
            fprintf(stderr, "[act] gf=%p rc=%d front %x.%x self %x.%x active %d\n", (void *)getfront, rc, f[0], f[1], self[0], self[1], g_app_active);
        if (!getfront || !self[1] || rc || !f[1]) return;
    }
    if (f[0] == self[0] && f[1] == self[1]) {
        away = 0;
        if (!front_since) front_since = t;
        /* the native activation normally comes at once; supply one if it does not. Also for an
           app that was never active: Organization Chart, launched in the background by Word
           (launchDontSwitch) and then made front with SetFrontProcess, got no activation from
           Mojave's window server and kept its window hidden until the Dock icon was clicked
           (it shows it on activation). Not in the first 2 s, when the launch activation is
           still on its way. OAH_NO_FIRSTACT=1 restores the old rule. */
        {   static double first_poll; static int nofirst = -1;
            if (!first_poll) first_poll = t;
            if (nofirst < 0) nofirst = getenv("OAH_NO_FIRSTACT") ? 1 : 0;
            if (!g_app_active && (g_app_act_at || (!nofirst && t - first_poll > 2.0)) && t - front_since > 0.4) st = 1;
        }
    } else {
        front_since = 0;
        if (g_app_active && ++away >= 2) st = 2;   /* two polls: not the hand-off's brief switch */
    }
    {   /* the guest may not pull events for a while (start-up, a long command): one queued
           (de)activation is enough; it was queued again every 0.1 s, dozens dropped later */
        static int qst; static double qat;
        if (st && st == qst && t - qat < 2.0) st = 0;
        else if (st) { qst = st; qat = t; }
    }
    if (st) {
        NCGS(void *, CGEventCreate, (void *));
        NCGS(void, CGEventSetType, (void *, int));
        NCGS(void, CGEventSetIntegerValueField, (void *, int, long long));
        void *e = p_CGEventCreate ? p_CGEventCreate(0) : 0;
        if (!e) return;
        p_CGEventSetType(e, 13);
        p_CGEventSetIntegerValueField(e, 99, st);
        p_CGEventSetIntegerValueField(e, 83, st);
        pthread_mutex_lock(&g_evq_mu);
        oah_ev_enqueue_locked(e);
        pthread_mutex_unlock(&g_evq_mu);
        EVLOG("[ev] app %sactivation queued (front %x.%x)\n", st == 1 ? "" : "de", f[0], f[1]);
        away = 0;
    }
}
static void oah_ev_wake_locked(void) {
    mach_msg_header_t h;
    if (!g_ev_gport || g_ev_pending || g_evq_h == g_evq_t) return;
    memset(&h, 0, sizeof h);
    h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    h.msgh_size = sizeof h;
    h.msgh_remote_port = g_ev_gport;
    h.msgh_id = 0x4f414845;                     /* 'OAHE' */
    if (mach_msg(&h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof h, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL) == KERN_SUCCESS) {
        g_ev_pending = 1; g_ev_wakes++;
    }
}
/* ---- Dock client (CoreDockRegisterClientWithRunLoop) ----
   The Dock tells its clients about minimized windows it restored, tile clicks etc. through a
   callback (type, data, ptr, refcon) registered with CoreDockRegisterClientWithRunLoop. The
   guest HIServices' registration never reached Mojave's Dock, so a window restored from the
   Dock came back on screen while AppKit still thought it was minimized (no typing, no drawing).
   The guest's callback (10.6 HIToolbox DockCallback) is registered natively here instead, on a
   native thread with its own run loop; messages are queued and a marker event (AppKit-defined
   type 13, subtype 0x7ff0) wakes the guest, whose event path then calls the guest callback on
   its own thread (oah_dock_deliver). OAH_NO_DOCKCLIENT=1 disables, OAH_DOCKTRACE=1 logs. */
static int oah_docktrace(void) {   /* OAH_DOCKTRACE=1, or /tmp/oah_docktrace exists (wrapper launches) */
    static int t = -1;
    if (t < 0) t = (getenv("OAH_DOCKTRACE") || access("/tmp/oah_docktrace", 0) == 0) ? 1 : 0;
    return t;
}
#define OAH_DOCKQ 32
static unsigned int g_dock_gcb, g_dock_grefcon;
static int g_in_dock_deliver;
static unsigned int g_last_focus;   /* last CGSSetMouseFocusWindow window */
static unsigned int g_dockq[OAH_DOCKQ][7];   /* type, data, has-words, 4 words of in-data */
static volatile int g_dockq_h, g_dockq_t;
static pthread_mutex_t g_dock_mu = PTHREAD_MUTEX_INITIALIZER;
static void oah_ev_enqueue_locked(void *e);
static void oah_ev_wake_locked(void);
/* Synchronous requests (the Dock menu, message 11): the native thread waits up to a second
   while the guest thread runs the guest callback with a guest out-pointer, and the guest's
   answer (a CFArray of menu item dictionaries) comes back as a property list. */
static pthread_cond_t g_dreq_cv = PTHREAD_COND_INITIALIZER;
static volatile int g_dreq_pending, g_dreq_done;
static unsigned int g_dreq_type, g_dreq_data;
static CFTypeRef g_dreq_result;
static CFTypeRef oah_guest_plist_native(unsigned int gobj) {
    static unsigned int fxml, flen, fptr, frel;
    unsigned int a[2], gd, n, p;
    CFDataRef d;
    CFTypeRef r = 0;
    if (!gobj) return 0;
    if (!fxml) {
        fxml = oah_guest_sym("CoreFoundation.framework", "_CFPropertyListCreateXMLData");
        flen = oah_guest_sym("CoreFoundation.framework", "_CFDataGetLength");
        fptr = oah_guest_sym("CoreFoundation.framework", "_CFDataGetBytePtr");
        frel = oah_guest_sym("CoreFoundation.framework", "_CFRelease");
    }
    if (!fxml || !flen || !fptr) return 0;
    a[0] = 0; a[1] = gobj;
    gd = oah_callppc_n("CFPropertyListCreateXMLData", fxml, 2, a);
    if (!gd) return 0;
    a[0] = gd;
    n = oah_callppc_n("CFDataGetLength", flen, 1, a);
    p = oah_callppc_n("CFDataGetBytePtr", fptr, 1, a);
    if (p && n && n < (16u << 20)) {
        d = CFDataCreate(0, (const UInt8 *)p, (CFIndex)n);
        if (d) { r = CFPropertyListCreateWithData(0, d, kCFPropertyListMutableContainersAndLeaves, 0, 0); CFRelease(d); }
    }
    if (frel) { a[0] = gd; oah_callppc_n("CFRelease", frel, 1, a); }
    return r;
}
static int oah_dock_native_cb(unsigned int type, unsigned int data, void *ptr, void *refcon) {   /* returns 0 like HIToolbox's */
    NCGS(void *, CGEventCreate, (void *));
    NCGS(void, CGEventSetType, (void *, int));
    NCGS(void, CGEventSetIntegerValueField, (void *, int, long long));
    void *e;
    (void)refcon;
    if (oah_docktrace()) fprintf(stderr, "[dock] callback type %u data %u ptr %p\n", type, data, ptr);
    pthread_mutex_lock(&g_dock_mu);
    if (type == 20 && ptr) {
        /* Mojave's "restore these windows": ptr -> { count, unsigned int *items }. The 10.6
           Dock sent message 1 (data = window) per window, which 10.6 HIToolbox and AppKit
           (_NSCoreDockMessageReceive -> _NSRestoreFromDock) handle. */
        unsigned int k, cnt = ((unsigned int *)ptr)[0], *it = ((unsigned int **)ptr)[1];
        for (k = 0; it && k < cnt && k < 16 && (g_dockq_t + 1) % OAH_DOCKQ != g_dockq_h; k++) {
            g_dockq[g_dockq_t][0] = 1; g_dockq[g_dockq_t][1] = it[k]; g_dockq[g_dockq_t][2] = 0;
            g_dockq_t = (g_dockq_t + 1) % OAH_DOCKQ;
        }
    } else if (type == 12 && ptr && (g_dockq_t + 1) % OAH_DOCKQ != g_dockq_h) {
        /* a Dock menu item was chosen: ptr -> { value, kind ('wind' = window item), ... };
           passed on as a guest (big-endian) copy */
        int k;
        g_dockq[g_dockq_t][0] = type; g_dockq[g_dockq_t][1] = data; g_dockq[g_dockq_t][2] = 1;
        for (k = 0; k < 4; k++) g_dockq[g_dockq_t][3 + k] = ((unsigned int *)ptr)[k];
        g_dockq_t = (g_dockq_t + 1) % OAH_DOCKQ;
        if (oah_docktrace()) fprintf(stderr, "[dock] menu choice %08x %08x %08x %08x\n", ((unsigned int *)ptr)[0],
                                     ((unsigned int *)ptr)[1], ((unsigned int *)ptr)[2], ((unsigned int *)ptr)[3]);
    } else if (ptr) {
        /* in/out data (11/13: the Dock menu, as CF objects): answered here, empty, since the
           guest cannot run on this thread and the Dock reads the answer when we return. The
           guest callback stored through the 0 we used to pass and crashed (wrapper Numbers,
           Dock icon right-click). */
        if (type == 11 && !*(void **)ptr && !getenv("OAH_DOCK_NOMENU")) {   /* the Dock menu */
            CFTypeRef res = 0;
            if (g_dock_gcb && !getenv("OAH_DOCK_EMPTYMENU")) {
                struct timespec ts; struct timeval tv;
                void *me;
                NCGS(void *, CGEventCreate, (void *));
                NCGS(void, CGEventSetType, (void *, int));
                NCGS(void, CGEventSetIntegerValueField, (void *, int, long long));
                g_dreq_type = type; g_dreq_data = data; g_dreq_result = 0; g_dreq_done = 0; g_dreq_pending = 1;
                pthread_mutex_unlock(&g_dock_mu);
                me = p_CGEventCreate ? p_CGEventCreate(0) : 0;
                if (me) {
                    p_CGEventSetType(me, 13);
                    p_CGEventSetIntegerValueField(me, 99, 0x7ff0);
                    pthread_mutex_lock(&g_evq_mu);
                    oah_ev_enqueue_locked(me);
                    oah_ev_wake_locked();
                    pthread_mutex_unlock(&g_evq_mu);
                }
                pthread_mutex_lock(&g_dock_mu);
                gettimeofday(&tv, 0);
                ts.tv_sec = tv.tv_sec + 1; ts.tv_nsec = tv.tv_usec * 1000;
                while (!g_dreq_done && pthread_cond_timedwait(&g_dreq_cv, &g_dock_mu, &ts) == 0) ;
                g_dreq_pending = 0;
                res = g_dreq_done ? g_dreq_result : 0;
                g_dreq_result = 0;
                if (oah_docktrace()) fprintf(stderr, "[dock] menu request -> %s\n", res ? "guest menu" : g_dreq_done ? "no guest menu" : "timed out");
            }
            *(CFTypeRef *)ptr = res ? res : (CFTypeRef)CFArrayCreateMutable(0, 0, &kCFTypeArrayCallBacks);
        }
        if (oah_docktrace()) fprintf(stderr, "[dock] type %u with data %p answered empty\n", type, ptr);
    } else if ((g_dockq_t + 1) % OAH_DOCKQ != g_dockq_h) {
        g_dockq[g_dockq_t][0] = type; g_dockq[g_dockq_t][1] = data; g_dockq[g_dockq_t][2] = (unsigned int)(uintptr_t)ptr;
        g_dockq_t = (g_dockq_t + 1) % OAH_DOCKQ;
    }
    pthread_mutex_unlock(&g_dock_mu);
    e = p_CGEventCreate ? p_CGEventCreate(0) : 0;
    if (!e) return 0;
    p_CGEventSetType(e, 13);
    p_CGEventSetIntegerValueField(e, 99, 0x7ff0);
    pthread_mutex_lock(&g_evq_mu);
    oah_ev_enqueue_locked(e);
    oah_ev_wake_locked();
    pthread_mutex_unlock(&g_evq_mu);
    return 0;
}
static void *oah_dock_thread(void *arg) {
    static void *hs;
    int (*reg)(void *, void *, CFRunLoopRef);
    (void)arg;
    hs = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/HIServices.framework/HIServices", RTLD_LAZY);
    reg = hs ? (int (*)(void *, void *, CFRunLoopRef))dlsym(hs, "CoreDockRegisterClientWithRunLoop") : 0;
    if (!reg) { fprintf(stderr, "[dock] no native CoreDockRegisterClientWithRunLoop\n"); return 0; }
    fprintf(stderr, "[dock] native client registered (%d)\n", reg((void *)oah_dock_native_cb, 0, CFRunLoopGetCurrent()));
    for (;;) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1e9, false);
    return 0;
}
/* guest thread: run the queued Dock messages through the guest's callback */
static void oah_dock_deliver(void) {
    pthread_mutex_lock(&g_dock_mu);
    if (g_dreq_pending && !g_dreq_done) {
        unsigned int gb[4] = { 0, 0, 0, 0 }, a[4], t = g_dreq_type, d = g_dreq_data;
        CFTypeRef r = 0;
        pthread_mutex_unlock(&g_dock_mu);
        if (g_dock_gcb) {
            unsigned int garr;
            a[0] = t; a[1] = d; a[2] = (unsigned int)(uintptr_t)gb; a[3] = g_dock_grefcon;
            oah_callppc_n("DockCallback", g_dock_gcb, 4, a);
            garr = OSSwapBigToHostInt32(gb[0]);
            r = oah_guest_plist_native(garr);
            if (oah_docktrace()) {
                fprintf(stderr, "[dock] guest answered type %u: %08x -> %p\n", t, garr, (void *)r);
                if (r) CFShow(r);
            }
            if (garr) { static unsigned int frel; if (!frel) frel = oah_guest_sym("CoreFoundation.framework", "_CFRelease");
                        if (frel) { a[0] = garr; oah_callppc_n("CFRelease", frel, 1, a); } }
        }
        pthread_mutex_lock(&g_dock_mu);
        if (g_dreq_pending) { g_dreq_result = r; g_dreq_done = 1; pthread_cond_signal(&g_dreq_cv); }
        else if (r) CFRelease(r);                  /* the native side gave up waiting */
    }
    pthread_mutex_unlock(&g_dock_mu);
    for (;;) {
        unsigned int m[7], a[4], gw[4];
        pthread_mutex_lock(&g_dock_mu);
        if (g_dockq_h == g_dockq_t) { pthread_mutex_unlock(&g_dock_mu); return; }
        memcpy(m, g_dockq[g_dockq_h], sizeof m);
        g_dockq_h = (g_dockq_h + 1) % OAH_DOCKQ;
        pthread_mutex_unlock(&g_dock_mu);
        if (!g_dock_gcb) continue;
        if (m[0] == 1 || m[0] == 8) {
            /* the Dock has put the window back on screen itself (the guest ordered it out after
               minimizing): record it as ordered in, or the guest's on-screen queries said no
               and AppKit never drew or keyed the restored window */
            oah_cgswin_t *w = oah_cgs_win(m[1]);
            if (w && !w->ordered) { w->ordered = 1; CGSLOG("[dock] window %u back on screen\n", m[1]); }
        }
        a[0] = m[0]; a[1] = m[1]; a[2] = 0; a[3] = g_dock_grefcon;   /* native ptr data is not passed */
        if (m[2] == 1) {                         /* copied in-data (menu choice) */
            int k;
            for (k = 0; k < 4; k++) gw[k] = OSSwapHostToBigInt32(m[3 + k]);
            a[2] = (unsigned int)(uintptr_t)gw;
        }
        g_in_dock_deliver = m[0] == 12;
        oah_callppc_n("DockCallback", g_dock_gcb, 4, a);
        g_in_dock_deliver = 0;
        if (oah_docktrace()) fprintf(stderr, "[dock] delivered type %u data %u to the guest\n", m[0], m[1]);
        if (m[0] == 12 && m[2] == 1 && m[3] == 1 && m[4] && !getenv("OAH_NO_DOCKRAISE")) {
            /* a window item (command 1, tag = the guest NSWindow): 10.6 AppKit makes it key but
               leaves the raise to the window server; raise it natively */
            static unsigned int fgc, fsel, fmsg;
            unsigned int a2[3], cls, sel, isa = 0, wid;
            if (!fgc) { fgc = oah_guest_sym("libobjc", "_objc_getClass"); fsel = oah_guest_sym("libobjc", "_sel_registerName");
                        fmsg = oah_guest_sym("libobjc", "_objc_msgSend"); }
            if (fgc && fsel && fmsg && !(m[4] & 3) && probe_read(m[4], &isa) && isa) {
                a2[0] = (unsigned int)"NSWindow"; cls = oah_callppc_n("objc_getClass", fgc, 1, a2);
                a2[0] = (unsigned int)"isKindOfClass:"; sel = oah_callppc_n("sel", fsel, 1, a2);
                a2[0] = m[4]; a2[1] = sel; a2[2] = cls;
                if (cls && (oah_callppc_n("msgSend", fmsg, 3, a2) & 0xff)) {
                    oah_cgswin_t *w;
                    NCGS(int, CGSOrderWindow, (unsigned int, unsigned int, int, unsigned int));
                    a2[0] = (unsigned int)"windowNumber"; sel = oah_callppc_n("sel", fsel, 1, a2);
                    a2[0] = m[4]; a2[1] = sel;
                    wid = oah_callppc_n("msgSend", fmsg, 2, a2);
                    w = oah_cgs_win(wid);
                    if (w && w->ordered && !w->ghost && p_CGSOrderWindow) {
                        p_CGSOrderWindow(g_ncid, wid, 1, 0);
                        oah_cgs_group_follow(wid, 1);
                    }
                    if (oah_docktrace()) fprintf(stderr, "[dock] menu window item -> window %u raised\n", wid);
                }
            }
        }
        if (m[0] == 8) {                         /* a Dock click always activates the app */
            /* 10.6 AppKit deactivates itself when its last window is minimized (the 10.6 Dock
               then activated the app again on the tile click); Mojave keeps the app in front,
               so no activation event came and the restored window stayed inactive, not key.
               Queue an activation that bypasses the state filter (subtype 0x7ff1, made 1 there). */
            NCGS(void *, CGEventCreate, (void *));
            NCGS(void, CGEventSetType, (void *, int));
            NCGS(void, CGEventSetIntegerValueField, (void *, int, long long));
            void *e = p_CGEventCreate ? p_CGEventCreate(0) : 0;
            if (e) {
                p_CGEventSetType(e, 13);
                p_CGEventSetIntegerValueField(e, 99, 0x7ff1);   /* (83 aliases 99 for this type) */
                pthread_mutex_lock(&g_evq_mu);
                oah_ev_enqueue_locked(e);
                oah_ev_wake_locked();
                pthread_mutex_unlock(&g_evq_mu);
            }
        }
    }
}
static void oah_ev_enqueue_locked(void *e) {
    if ((g_evq_t + 1) % OAH_EVQ == g_evq_h) { g_ev_drop++; oah_cgs_nrelease(e); return; }
    g_evq[g_evq_t] = e; g_evq_at[g_evq_t] = CFAbsoluteTimeGetCurrent(); g_evq_ty[g_evq_t] = oah_ev_ntype(e); g_evq_t = (g_evq_t + 1) % OAH_EVQ; g_ev_in++;
}
/* Launch Services thread: LS calls can block while launchservicesd waits on this app (an
   app switch), so they never run on the guest or event pump threads. It tells LS about our own
   hide/show (oah_app_hide) and polls LS's hidden flag every 0.5 s: a hide/show requested from
   outside (another app's Hide Others / Show All) that holds for two polls is carried out on the
   guest thread (markers 0x7ff2 hide / 0x7ff3 show). OAH_NO_LSHIDE=1 disables the poll. */
static void *oah_ls_thread(void *arg) {
    void *(*getasn)(void) = (void *(*)(void))dlsym(RTLD_DEFAULT, "_LSGetCurrentApplicationASN");
    CFTypeRef (*copyitem)(int, void *, CFStringRef) = (CFTypeRef (*)(int, void *, CFStringRef))dlsym(RTLD_DEFAULT, "_LSCopyApplicationInformationItem");
    int (*setitem)(int, void *, CFStringRef, CFTypeRef, void *) = (int (*)(int, void *, CFStringRef, CFTypeRef, void *))dlsym(RTLD_DEFAULT, "_LSSetApplicationInformationItem");
    CFStringRef *hkey = (CFStringRef *)dlsym(RTLD_DEFAULT, "_kLSApplicationIsHiddenKey");
    int pend = 0, cnt = 0, poll = !getenv("OAH_NO_LSHIDE");
    (void)arg;
    if (!getasn || !hkey || !*hkey) return 0;
    for (;;) {
        void *asn;
        usleep(500000);
        asn = getasn();
        if (!asn) continue;
        if (g_ls_hidden_want && setitem) {
            int w = g_ls_hidden_want;
            g_ls_hidden_want = 0;
            setitem(-2, asn, *hkey, w == 2 ? kCFBooleanTrue : kCFBooleanFalse, 0);
            pend = cnt = 0;
            continue;
        }
        if (poll && copyitem) {
            CFTypeRef v = copyitem(-2, asn, *hkey);
            int h = v == (CFTypeRef)kCFBooleanTrue;
            if (v) CFRelease(v);
            if (h != g_app_hidden) { if (pend == h + 1) cnt++; else { pend = h + 1; cnt = 1; } }
            else pend = cnt = 0;
            if (pend && cnt >= 2) {
                NCGS(void *, CGEventCreate, (void *));
                NCGS(void, CGEventSetType, (void *, int));
                NCGS(void, CGEventSetIntegerValueField, (void *, int, long long));
                void *e = p_CGEventCreate ? p_CGEventCreate(0) : 0;
                if (e) {
                    p_CGEventSetType(e, 13);
                    p_CGEventSetIntegerValueField(e, 99, h ? 0x7ff2 : 0x7ff3);
                    pthread_mutex_lock(&g_evq_mu);
                    oah_ev_enqueue_locked(e);
                    oah_ev_wake_locked();
                    pthread_mutex_unlock(&g_evq_mu);
                }
                EVLOG("[ev] Launch Services says %s\n", h ? "hidden" : "shown");
                pend = cnt = 0;
            }
        }
    }
    return 0;
}
/* Hide Others / Show All from another app. Mojave's _HideOtherApplications / _ShowAllApplications
   send each app a Launch Services notification (0x219 hide request, 0x218 show request) and the
   app hides itself (HIServices LS callback, AppKit). Native HIServices scheduled that callback on
   this process's native main run loop, which never runs here (the main thread runs the guest), so
   other apps' Hide Others left our windows up. Listen on our own thread's run loop and carry the
   request out through the pump (markers 0x7ff2 / 0x7ff3), and tell Launch Services the hidden
   state afterwards so Show All / the hidden-app counts see us. OAH_NO_LSNOTIFY=1 disables. */
static int oah_lsnall(void) {   /* OAH_LSNALL=1 or /tmp/oah_lsnall (wrapper launches): log every LS notification */
    static int v = -1;
    if (v < 0) v = (getenv("OAH_LSNALL") || access("/tmp/oah_lsnall", 0) == 0) ? 1 : 0;
    return v;
}
static void oah_lsn_post(int hide) {
    NCGS(void *, CGEventCreate, (void *));
    NCGS(void, CGEventSetType, (void *, int));
    NCGS(void, CGEventSetIntegerValueField, (void *, int, long long));
    void *e = p_CGEventCreate ? p_CGEventCreate(0) : 0;
    if (!e) return;
    p_CGEventSetType(e, 13);
    p_CGEventSetIntegerValueField(e, 99, hide ? 0x7ff2 : 0x7ff3);
    pthread_mutex_lock(&g_evq_mu);
    oah_ev_enqueue_locked(e);
    oah_ev_wake_locked();
    pthread_mutex_unlock(&g_evq_mu);
}
static void oah_lsn_cb(int code, void *a1, void *a2, CFTypeRef info, void *a4, void *ctx) {
    (void)a1; (void)a2; (void)info; (void)a4; (void)ctx;
    EVLOG("[ev] Launch Services notification 0x%x\n", code);
    if (oah_lsnall()) fprintf(stderr, "[lsn] notification 0x%x info %p\n", code, (void *)info);
    if (code == 0x219) oah_lsn_post(1);          /* another app's Hide Others */
    else if (code == 0x218) oah_lsn_post(0);     /* Show All */
    else if (code == 0x100 && g_app_hidden && !getenv("OAH_NO_REOPENSHOW")) {
        /* `open` / a double-click on a hidden app: Launch Services makes it front and sends this,
           but no show request (the Dock sends one: process request 3), so the app came to the
           front with its windows still hidden. Ask for the show the Dock's way. */
        static int (*shp)(const unsigned int *, unsigned char);
        static const unsigned int cur[2] = { 0, 2 };   /* kCurrentProcess */
        if (!shp) shp = (int (*)(const unsigned int *, unsigned char))dlsym(RTLD_DEFAULT, "ShowHideProcess");
        static int (*sfp)(const unsigned int *);
        int r1, r2;
        if (!sfp) sfp = (int (*)(const unsigned int *))dlsym(RTLD_DEFAULT, "SetFrontProcess");
        r1 = shp ? shp(cur, 1) : -1;
        /* the show alone left the app front without the keyboard: keys went to the previous
           app until the next activation */
        r2 = sfp ? sfp(cur) : -1;
        EVLOG("[ev] reopen while hidden: show (%d), front (%d)\n", r1, r2);
    }
}
static void oah_lsn_timer(CFRunLoopTimerRef t, void *info) {
    /* our own hide / show (Cmd-H, Hide Others' counterpart, unhide): tell Launch Services */
    static int (*setitem)(int, void *, CFStringRef, CFTypeRef, void *);
    static void *(*getasn)(void);
    static CFStringRef *hkey;
    (void)t; (void)info;
    if (!g_ls_hidden_want) return;
    if (!setitem) setitem = (int (*)(int, void *, CFStringRef, CFTypeRef, void *))dlsym(RTLD_DEFAULT, "_LSSetApplicationInformationItem");
    if (!getasn) getasn = (void *(*)(void))dlsym(RTLD_DEFAULT, "_LSGetCurrentApplicationASN");
    if (!hkey) hkey = (CFStringRef *)dlsym(RTLD_DEFAULT, "_kLSApplicationIsHiddenKey");
    {
        int w = g_ls_hidden_want; void *asn;
        g_ls_hidden_want = 0;
        asn = getasn ? getasn() : 0;
        if (setitem && asn && hkey && *hkey) setitem(-2, asn, *hkey, w == 2 ? kCFBooleanTrue : kCFBooleanFalse, 0);
        EVLOG("[ev] told Launch Services: %s\n", w == 2 ? "hidden" : "shown");
    }
}
static void *oah_lsn_thread(void *arg) {
    int (*sched)(int, void *, void *, void *, CFRunLoopRef, CFStringRef) =
        (int (*)(int, void *, void *, void *, CFRunLoopRef, CFStringRef))dlsym(RTLD_DEFAULT, "_LSScheduleNotificationFunction");
    int (*modify)(int, int, const int *, int, int, int, int) =
        (int (*)(int, int, const int *, int, int, int, int))dlsym(RTLD_DEFAULT, "_LSModifyNotification");
    void *(*getasn)(void) = (void *(*)(void))dlsym(RTLD_DEFAULT, "_LSGetCurrentApplicationASN");
    static const int codes[3] = { 0x218, 0x219, 0x100 };
    void *asn = 0;
    int id, k, e = -1;
    (void)arg;
    if (!sched || !modify || !getasn) return 0;
    sleep(4);                                      /* not during the launch check-in */
    for (k = 0; k < 60 && !(asn = getasn()); k++) sleep(1);
    if (!asn) return 0;
    id = sched(-2, (void *)oah_lsn_cb, 0, asn, CFRunLoopGetCurrent(), kCFRunLoopCommonModes);
    if (id && oah_lsnall()) {   /* discovery: every notification code, logged by oah_lsn_cb */
        static int all[0x300]; int i2; for (i2 = 0; i2 < 0x300; i2++) all[i2] = i2 + 1;
        e = modify(id, 0x300, all, 0, 0, 0, 0);
    } else if (id) e = modify(id, 3, codes, 0, 0, 0, 0);
    fprintf(stderr, "[ev] Launch Services hide/show requests: notification %d (%d)\n", id, e);
    {
        CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.5, 0.3, 0, 0, oah_lsn_timer, NULL);
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopCommonModes);
    }
    CFRunLoopRun();
    return 0;
}
static void *oah_ev_thread(void *arg) {
    NCGS(void *, CGEventCreateNextEvent, (unsigned int));
    NCGS(int, CGSGetEventPort, (unsigned int, mach_port_t *));
    NCGS(int, CGSExtractEventRecordsFromMessage, (void *));
    NCGS(int, CGEventGetType, (void *));
    mach_port_t nport = MACH_PORT_NULL;
    union { mach_msg_header_t h; char b[16384]; } m;
    unsigned int cid = oah_cgs_cid();
    (void)arg;
    g_actsync_off = getenv("OAH_NO_ACTSYNC") ? 1 : 0;
    /* experimental: Launch Services polling made the guest stop pulling events right after
       launch (LS calls from a second thread during check-in), so it is off unless asked for */
    if (getenv("OAH_LSTHREAD")) { pthread_t lt; if (!pthread_create(&lt, 0, oah_ls_thread, 0)) pthread_detach(lt); }
    else if (!getenv("OAH_NO_LSNOTIFY")) { pthread_t lt; if (!pthread_create(&lt, 0, oah_lsn_thread, 0)) pthread_detach(lt); }
    if (getenv("OAH_EV_RECV") && p_CGSGetEventPort) p_CGSGetEventPort(cid, &nport);
    fprintf(stderr, "[ev] pump started (connection %u, %s)\n", cid, nport ? "receiving on the native event port" : "polling");
    int got_prev = 1;
    for (;;) {
        void *e;
        int got = 0;
        if (nport) {
            kern_return_t kr = mach_msg(&m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof m, nport, 50, MACH_PORT_NULL);
            if (kr == KERN_SUCCESS && p_CGSExtractEventRecordsFromMessage) p_CGSExtractEventRecordsFromMessage(&m.h);
        } else {
            /* poll every 8 ms while events flow (or a delayed raise is due), then back off: an idle app
               woke 125 times a second here. OAH_EV_IDLE_MS sets the idle interval (default 50). */
            static double last_ev; static int idle_us = -1;
            double now = CFAbsoluteTimeGetCurrent();
            if (idle_us < 0) { const char *v = getenv("OAH_EV_IDLE_MS"); idle_us = (v ? atoi(v) : 50) * 1000; if (idle_us < 8000) idle_us = 8000; }
            if (got_prev) last_ev = now;
            usleep(now - last_ev < 1.0 || g_raise_wid || g_raise_all_at || g_snd_pend_n ? 8000 : idle_us);
        }
        while (p_CGEventCreateNextEvent && (e = p_CGEventCreateNextEvent(cid))) {
            int t = p_CGEventGetType ? p_CGEventGetType(e) : -1;
            pthread_mutex_lock(&g_evq_mu);
            if ((g_evq_t + 1) % OAH_EVQ == g_evq_h) { g_ev_drop++; pthread_mutex_unlock(&g_evq_mu); oah_cgs_nrelease(e); continue; }
            g_evq[g_evq_t] = e; g_evq_at[g_evq_t] = CFAbsoluteTimeGetCurrent(); g_evq_ty[g_evq_t] = oah_ev_ntype(e); g_evq_t = (g_evq_t + 1) % OAH_EVQ; g_ev_in++;
            pthread_mutex_unlock(&g_evq_mu);
            if ((t != 5 && t != 6 && t != 7 && t != 27) || (getenv("OAH_EVTRACE") && getenv("OAH_EVTRACE")[0] == '2'))
                EVLOG("[ev] native event type %d queued\n", t);
            got = 1;
        }
        if (g_ev_rewake_at != 0 && CFAbsoluteTimeGetCurrent() >= g_ev_rewake_at) { g_ev_rewake_at = 0; oah_ev_wake(); }
        {   /* Sound Manager callbacks are delivered on the guest main thread at its event poll; an
               app blocked in its run loop (Kid Pix's Save panel waits for its click sound to end)
               got them only with the next input event. Waiting callbacks wake the guest's event
               source (seen on two pump passes in a row, so apps that poll anyway see no extra
               wake-ups). OAH_NO_SNDWAKE=1 disables. */
            static int off = -1, seen;
            if (off < 0) off = getenv("OAH_NO_SNDWAKE") ? 1 : 0;
            if (!off && g_snd_pend_n) {
                if (seen++) { int pend; pthread_mutex_lock(&g_evq_mu); pend = g_ev_pending; pthread_mutex_unlock(&g_evq_mu); if (!pend) oah_ev_wake(); }
            } else seen = 0;
        }
        oah_actsync_poll();
        pthread_mutex_lock(&g_evq_mu);
        oah_ev_wake_locked();
        pthread_mutex_unlock(&g_evq_mu);
        got_prev = got;
    }
    return 0;
}
/* ---- test input injection ----
   /tmp/oah_inject.<pid>: one command per line, run in order, file removed when read.
     key <keycode> [cmd,shift,opt,ctrl]     press+release (keycode = Mac virtual key code)
     text <string>                          type characters (unicode, no modifiers)
     click <x> <y> [n]                      left click at global top-left coordinates (n = click count)
     rclick <x> <y>                         right click
     drag <x1> <y1> <x2> <y2>               left drag in 10 steps
     move <x> <y>                           mouse moved
     sleep <ms>
   Events are made with CGEventCreate* (never posted, so no Accessibility permission is needed) and go
   into the same queue as window-server events.  Mouse events carry the guest window under the point
   (fields 91/51); key events the newest ordered window. */
static unsigned int oah_inj_window_at(float x, float y) {
    int i; unsigned long best = 0; unsigned int wid = 0;
    for (i = 0; i < OAH_MAXWIN; i++) {
        oah_cgswin_t *w = &g_cgswin[i];
        if (!w->wid || !w->ordered || w->wid == g_menubar_wid) continue;
        if (x >= w->x && y >= w->y && x < w->x + w->w && y < w->y + w->h && w->seq >= best) { best = w->seq; wid = w->wid; }
    }
    return wid;
}
static unsigned int oah_inj_front_window(void) {
    int i; unsigned long best = 0; unsigned int wid = 0;
    for (i = 0; i < OAH_MAXWIN; i++) {
        oah_cgswin_t *w = &g_cgswin[i];
        if (w->wid && w->ordered && w->wid != g_menubar_wid && w->w > 1 && w->seq >= best) { best = w->seq; wid = w->wid; }
    }
    return wid;
}
static void oah_inj_push(void *e, unsigned int wid) {
    NCGS(void, CGEventSetIntegerValueField, (void *, int, long long));
    if (!e) return;
    if (wid && p_CGEventSetIntegerValueField) { p_CGEventSetIntegerValueField(e, 91, wid); p_CGEventSetIntegerValueField(e, 51, wid); }
    pthread_mutex_lock(&g_evq_mu);
    if ((g_evq_t + 1) % OAH_EVQ != g_evq_h) { g_evq[g_evq_t] = e; g_evq_at[g_evq_t] = CFAbsoluteTimeGetCurrent(); g_evq_ty[g_evq_t] = oah_ev_ntype(e); g_evq_t = (g_evq_t + 1) % OAH_EVQ; g_ev_in++; }
    else oah_cgs_nrelease(e);
    oah_ev_wake_locked();
    pthread_mutex_unlock(&g_evq_mu);
}
static volatile float g_inj_x, g_inj_y;            /* last injected pointer position */
static volatile unsigned int g_inj_btn;             /* injected buttons held */
static volatile time_t g_inj_t;                     /* when the injector last moved the pointer */
static volatile double g_inj_short_until;          /* wheel clicks: the injector drives the pointer only until then */
static int oah_inj_drives(void) {
    if (g_inj_short_until > 0) {
        if (CFAbsoluteTimeGetCurrent() < g_inj_short_until) return 1;
        g_inj_short_until = 0; g_inj_t = 0;
        return 0;
    }
    return g_inj_t && time(0) - g_inj_t < 10;
}
static void oah_inj_mouse(int type, float x, float y, int clicks, int button);
/* Clicks on a scroll bar arrow for a wheel scroll the application did not handle
   (oah_fixret_wheelbar): queued like window-server events; for a moment the pointer queries
   answer the arrow's position, so the app's TrackControl sees the click on the arrow. */
/* pending arrow clicks: > 0 on the down/right arrow, < 0 on the up/left one; wheel events that
   arrive while clicking add to it (a trackpad sends many) */
static volatile int g_inj_wc_pending, g_inj_wc_busy;
static float g_inj_wc_x[2], g_inj_wc_y[2];
static pthread_mutex_t g_inj_wc_mu = PTHREAD_MUTEX_INITIALIZER;
static void *oah_inj_wheelclick_thread(void *arg) {
    /* each click held ~60 ms like a hand's, so the app's TrackControl sees the button down and
       runs its arrow action once (queued back to back, the button was already up) */
    (void)arg;
    for (;;) {
        int d; float x, y;
        pthread_mutex_lock(&g_inj_wc_mu);
        d = g_inj_wc_pending > 0 ? 1 : g_inj_wc_pending < 0 ? -1 : 0;
        if (!d) { g_inj_wc_busy = 0; pthread_mutex_unlock(&g_inj_wc_mu); break; }
        g_inj_wc_pending -= d;
        x = g_inj_wc_x[d > 0]; y = g_inj_wc_y[d > 0];
        g_inj_short_until = CFAbsoluteTimeGetCurrent() + 0.4;
        pthread_mutex_unlock(&g_inj_wc_mu);
        oah_inj_mouse(1, x, y, 1, 0); usleep(60000);
        oah_inj_mouse(2, x, y, 1, 0); usleep(25000);
    }
    return 0;
}
static void oah_inj_wheelclicks(float x, float y, int n) {   /* n > 0: down/right arrow */
    pthread_t t;
    int start;
    pthread_mutex_lock(&g_inj_wc_mu);
    g_inj_wc_x[n > 0] = x; g_inj_wc_y[n > 0] = y;
    if ((g_inj_wc_pending > 0) != (n > 0)) g_inj_wc_pending = 0;     /* direction changed: drop the rest */
    g_inj_wc_pending += n;
    if (g_inj_wc_pending > 20) g_inj_wc_pending = 20;
    if (g_inj_wc_pending < -20) g_inj_wc_pending = -20;
    g_inj_short_until = CFAbsoluteTimeGetCurrent() + 0.4;
    start = !g_inj_wc_busy;
    if (start) g_inj_wc_busy = 1;
    pthread_mutex_unlock(&g_inj_wc_mu);
    if (start && pthread_create(&t, 0, oah_inj_wheelclick_thread, 0)) { g_inj_wc_busy = 0; return; }
    if (start) pthread_detach(t);
}
static void oah_inj_mouse(int type, float x, float y, int clicks, int button) {
    NCGS(void *, CGEventCreateMouseEvent, (void *, int, oah_pt_t, int));
    NCGS(void, CGEventSetIntegerValueField, (void *, int, long long));
    oah_pt_t p; void *e;
    p.x = x; p.y = y;
    g_inj_x = x; g_inj_y = y; g_inj_t = time(0);
    if (type == 1 || type == 3) g_inj_btn |= 1u << button;          /* down */
    else if (type == 2 || type == 4) g_inj_btn &= ~(1u << button);  /* up */
    if (!p_CGEventCreateMouseEvent) return;
    e = p_CGEventCreateMouseEvent(0, type, p, button);
    if (e && clicks > 1 && p_CGEventSetIntegerValueField) p_CGEventSetIntegerValueField(e, 1, clicks);
    {   /* like WindowServer: drags and the mouse-up go to the window that got the mouse-down,
           even when the pointer has left it (resize drags past the window's bottom went to
           whatever window was underneath) */
        static unsigned int grab;
        unsigned int wid;
        if (type == 1 || type == 3) wid = grab = oah_inj_window_at(x, y);
        else if ((type == 6 || type == 7 || type == 2 || type == 4) && grab) { wid = grab; if (type == 2 || type == 4) grab = 0; }
        else wid = oah_inj_window_at(x, y);
        oah_inj_push(e, wid);
    }
}
static void oah_inj_key(int code, unsigned long long flags, int down, const unsigned short *uni, int nuni) {
    NCGS(void *, CGEventCreateKeyboardEvent, (void *, unsigned short, int));
    NCGS(void, CGEventSetFlags, (void *, unsigned long long));
    NCGS(void, CGEventKeyboardSetUnicodeString, (void *, unsigned long, const unsigned short *));
    void *e;
    if (!p_CGEventCreateKeyboardEvent) return;
    e = p_CGEventCreateKeyboardEvent(0, (unsigned short)code, down);
    if (!e) return;
    if (p_CGEventSetFlags) p_CGEventSetFlags(e, flags);
    if (uni && nuni && p_CGEventKeyboardSetUnicodeString) p_CGEventKeyboardSetUnicodeString(e, nuni, uni);
    oah_inj_push(e, oah_inj_front_window());
}
static void *oah_inj_thread(void *arg) {
    char pf[64];
    (void)arg;
    snprintf(pf, sizeof pf, "/tmp/oah_inject.%d", getpid());
    for (;;) {
        FILE *f;
        char ln[1024];
        usleep(250000);                          /* test input files are rare: 4 checks a second */
        if (!(f = fopen(pf, "r"))) continue;
        unlink(pf);
        while (fgets(ln, sizeof ln, f)) {
            float x, y, x2, y2; int code, n, k; char mods[64] = "";
            ln[strcspn(ln, "\r\n")] = 0;
            fprintf(stderr, "[inject] %s\n", ln);
            if (sscanf(ln, "key %d %63s", &code, mods) >= 1) {
                unsigned long long fl = 0;
                if (strstr(mods, "cmd")) fl |= 0x100000; if (strstr(mods, "shift")) fl |= 0x20000;
                if (strstr(mods, "opt")) fl |= 0x80000; if (strstr(mods, "ctrl")) fl |= 0x40000;
                oah_inj_key(code, fl, 1, 0, 0); usleep(40000); oah_inj_key(code, fl, 0, 0, 0);
            } else if (!strncmp(ln, "text ", 5)) {
                /* US-layout virtual key codes: Carbon apps read the key code and the
                   character from the event record, so both must be right */
                static const char lo[] = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.\0 `";
                static const char hi[] = "ASDFHGZXCV\0BQWERYT!@#$^%+(&_*)}OU{IP\0LJ\"K:|<?NM>\0 ~";
                for (k = 5; ln[k]; k++) {
                    unsigned short u = (unsigned char)ln[k];
                    int vk = 0, j; unsigned long long sf = 0;
                    for (j = 0; j < (int)sizeof lo - 1; j++) {
                        if (lo[j] && lo[j] == ln[k]) { vk = j; break; }
                        if (hi[j] && hi[j] == ln[k] && hi[j] != lo[j]) { vk = j; sf = 0x20000; break; }
                    }
                    oah_inj_key(vk, sf, 1, &u, 1); usleep(20000); oah_inj_key(vk, sf, 0, &u, 1); usleep(20000);
                }
            } else if (sscanf(ln, "click %f %f %d", &x, &y, &n) >= 2) {
                if (n < 1 || n > 3) n = 1;
                for (k = 1; k <= n; k++) {
                    oah_inj_mouse(1, x, y, k, 0); usleep(60000); oah_inj_mouse(2, x, y, k, 0); usleep(60000);
                }
            } else if (sscanf(ln, "rclick %f %f", &x, &y) == 2) {
                oah_inj_mouse(3, x, y, 1, 1); usleep(60000); oah_inj_mouse(4, x, y, 1, 1);
            } else if (sscanf(ln, "drag %f %f %f %f", &x, &y, &x2, &y2) == 4) {
                oah_inj_mouse(1, x, y, 1, 0);
                for (k = 1; k <= 10; k++) { usleep(30000); oah_inj_mouse(6, x + (x2 - x) * k / 10, y + (y2 - y) * k / 10, 1, 0); }
                usleep(30000); oah_inj_mouse(2, x2, y2, 1, 0);
            } else if (sscanf(ln, "move %f %f", &x, &y) == 2) {
                oah_inj_mouse(5, x, y, 0, 0);
            } else if (sscanf(ln, "sleep %d", &n) == 1) {
                usleep((useconds_t)n * 1000);
            } else if (sscanf(ln, "act %d", &n) == 1) {   /* app (de)activation: 1, 2, or 0x7ff1 = 32753 forced */
                NCGS(void *, CGEventCreate, (void *));
                NCGS(void, CGEventSetType, (void *, int));
                NCGS(void, CGEventSetIntegerValueField, (void *, int, long long));
                void *e = p_CGEventCreate ? p_CGEventCreate(0) : 0;
                if (e) {
                    p_CGEventSetType(e, 13);
                    p_CGEventSetIntegerValueField(e, 99, n);
                    p_CGEventSetIntegerValueField(e, 83, n == 32753 ? 1 : n);
                    pthread_mutex_lock(&g_evq_mu); oah_ev_enqueue_locked(e); oah_ev_wake_locked(); pthread_mutex_unlock(&g_evq_mu);
                }
            }
            usleep(30000);
        }
        fclose(f);
    }
    return 0;
}
static void oah_ev_start(void) {
    pthread_t t;
    if (g_ev_started) return;
    g_ev_started = 1;
    oah_cgs_cid();
    pthread_create(&t, 0, oah_ev_thread, 0);
    pthread_detach(t);
    pthread_create(&t, 0, oah_inj_thread, 0);
    pthread_detach(t);
}
/* wake the guest's event source (it then pulls; nothing may be queued) */
static void oah_ev_wake(void) {
    pthread_mutex_lock(&g_evq_mu);
    static int old = -1;
    if (old < 0) old = getenv("OAH_WAKE_PENDING") ? 1 : 0;
    if (g_ev_gport && (!old || !g_ev_pending)) {   /* even if marked pending: it may already have been consumed */
        mach_msg_header_t h;
        memset(&h, 0, sizeof h);
        h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
        h.msgh_size = sizeof h;
        h.msgh_remote_port = g_ev_gport;
        h.msgh_id = 0x4f414845;
        if (!mach_msg(&h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof h, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL)) {
            g_ev_pending = 1; g_ev_wakes++;
        }
    }
    pthread_mutex_unlock(&g_evq_mu);
}
static void *oah_ev_pop(void) {
    void *e = 0;
    pthread_mutex_lock(&g_evq_mu);
    if (g_evq_h != g_evq_t) { e = g_evq[g_evq_h]; g_evq_h = (g_evq_h + 1) % OAH_EVQ; g_ev_out++; }
    else if (!g_expq_n) g_ev_pending = 0;        /* drained: the next event needs a new wake-up */
    pthread_mutex_unlock(&g_evq_mu);
    return e;
}
/* A click pulled by EventAvail got lost in an AppKit panel (Kid Pix's Save panel needed a
   second click). Kid Pix polls EventAvail from a run loop timer while the panel runs
   (RunAppModalLoopForWindow). Its pulls took the mouse-down and mouse-up off our queue into
   HIToolbox's, but events that arrive that way, inside a timer, were not noticed by the
   modal loop, which kept waiting for its window-server port; the down was dispatched only
   later and the button then waited for an up that had already gone. On a real system the
   window server message makes the port handler pull them. So a pull from inside EventAvail
   leaves a fresh (under 250 ms) button or key event at the head of the queue to the port
   handler while its wake-up is still pending; older ones are handed over as before, so an
   EventAvail busy loop still sees them. OAH_NO_PEEKDEFER=1 disables. */
static int oah_ev_defer_peek(unsigned int *R) {
    static int off = -1;
    static unsigned int ea;
    unsigned int sp, n, back, ra;
    int peek = 0, defer = 0, t;
    if (off < 0) off = getenv("OAH_NO_PEEKDEFER") ? 1 : 0;
    if (off) return 0;
    pthread_mutex_lock(&g_evq_mu);
    t = g_evq_h != g_evq_t && g_ev_pending ? g_evq_ty[g_evq_h] : -1;
    if (t >= 0 && !((t >= 1 && t <= 4) || t == 25 || t == 26 || t == 10 || t == 11)) t = -1;
    if (t >= 0 && CFAbsoluteTimeGetCurrent() - g_evq_at[g_evq_h] > 0.25) t = -1;
    pthread_mutex_unlock(&g_evq_mu);
    if (t < 0) return 0;
    if (!ea) ea = oah_guest_sym("HIToolbox", "_EventAvail");
    if (!ea || ea == 1) { ea = 1; return 0; }
    for (sp = R[1], n = 0; n < 4 && sp > 0x1000 && !(sp & 3); n++) {   /* PullEvents <- GetNextEventMatchingMask <- EventAvail */
        back = 0; ra = 0;
        if (!oah_bt_word(sp, &back) || !back || back <= sp || !oah_bt_word(back + 8, &ra)) break;
        if (ra - ea < 0x100) { peek = 1; break; }
        sp = back;
    }
    if (peek) defer = 1;
    if (defer && getenv("OAH_HITLOG")) fprintf(stderr, "[hit] event type %d left for the port handler (EventAvail pull)\n", t);
    return defer;
}
/* Native CGEvent -> new guest CGEventRef (0 if the type is not passed on). Consumes e. */
#include <mach/mach_time.h>
static CFStringRef oah_guest_str_native(unsigned int gs);   /* oah_ls.h */
/* ---- tracking areas ----
   10.6 AppKit registers NSTrackingAreas and cursor rects with the window server
   (CGSAddTrackingArea(cid, wid, region, A, B, C)) and expects mouse-entered / -exited events
   (types 8 / 9) back; they were unbridged no-ops, so no hover feedback and no cursor-rect
   cursor changes. Kept here per window; every mouse move/drag the guest pulls is checked
   against them and the transitions are delivered as guest events right after it. Event
   layout from a real 10.6 event (t/trk on Snow Leopard): field 61 = tracking number (A, the
   NSTrackingArea), 62 = user data, 63 = 5, 60 = 0, 51 = window. OAH_NO_TRACKING=1 disables,
   OAH_TRKTRACE=1 logs. */
#define OAH_NTRK 1024
static struct { unsigned int wid, A, B, C; float x, y, w, h; int en, inside; } g_trk[OAH_NTRK];
static int g_ntrk;
#define OAH_TRKQ 128
static struct { int type; unsigned int wid, A, B, C; float gx, gy, lx, ly; } g_trkq[OAH_TRKQ];
static int g_trkq_n;
static int g_trktrace = -1;
/* CGS system cursor (10.4/10.5 numbering) -> Mojave CoreCursor type, for the ones Mojave dropped */
static int oah_cursor_core(int t) { switch (t) { case 2: return 1; case 3: return 2; case 4: return 5; case 6: return 3; } return -1; }
/* ---- Photoshop's large brush cursor ----
   Photoshop CS2 shows a brush outline bigger than a system cursor through its AdobeLargeCursor
   plug-in: _CGSAdobeSetCursor(CGSRegionRef outline, int) hands it the outline as a region (empty,
   or 0xbeef at shutdown, to remove it) and Photoshop hides the system cursor meanwhile. The
   plug-in first asks dyld for _CGSAdobeSetCursorHook and, when it exists, only calls that.
   Otherwise it runs its own window server client: a window that follows mouse events read from
   its own connection's event port, drawn through CGSLockWindowRectBits straight into display
   memory. None of that exists on Mojave (the outline never appeared, and the hidden system
   cursor left no pointer at all over the canvas). The shim provides the hook (oah_gtrace.h,
   dyld lookup fixes) and shows the outline as a real cursor: black edge, white halo. While it
   shows, the guest's CGSHideCursor/ShowCursor and cursor changes are kept from Mojave and the
   last guest cursor is put back when the outline goes. OAH_NO_ADOBECURSOR=1 disables. */
static int g_alc_on, g_alc_hides, g_cur_hides;
static struct { int kind, type, rb, rb2, bpp, spp, bps, rows; unsigned char *d1, *d2; oah_rect_t r; oah_pt_t hot; } g_lastcur;
static void oah_lastcur_image(const unsigned char *d1, int rb, const unsigned char *d2, int rb2, int rows, oah_rect_t r, oah_pt_t hot, int bpp, int spp, int bps) {
    free(g_lastcur.d1); free(g_lastcur.d2); g_lastcur.d1 = g_lastcur.d2 = 0;
    g_lastcur.kind = 1;
    if (d1) { g_lastcur.d1 = malloc((size_t)rb * rows); if (g_lastcur.d1) memcpy(g_lastcur.d1, d1, (size_t)rb * rows); }
    if (d2) { g_lastcur.d2 = malloc((size_t)rb2 * rows); if (g_lastcur.d2) memcpy(g_lastcur.d2, d2, (size_t)rb2 * rows); }
    g_lastcur.rb = rb; g_lastcur.rb2 = rb2; g_lastcur.rows = rows; g_lastcur.r = r; g_lastcur.hot = hot;
    g_lastcur.bpp = bpp; g_lastcur.spp = spp; g_lastcur.bps = bps;
}
static void oah_lastcur_system(int kind, int type) { g_lastcur.kind = kind; g_lastcur.type = type; }
static int oah_core_cursor_set(int type) {
    static void *hs; int (*f)(unsigned int, int);
    if (!hs) hs = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/HIServices.framework/HIServices", RTLD_LAZY);
    f = hs ? (int (*)(unsigned int, int))dlsym(hs, "CoreCursorSet") : 0;
    return f ? f(g_ncid, type) : -1;
}
static void oah_lastcur_reapply(void) {
    NCGS(int, CGSSetCursorFromPremultipliedDataWithSeed, (unsigned int, void *, int, void *, int, oah_rect_t, oah_pt_t, int, int, int, int *));
    NCGS(int, CGSSetSystemDefinedCursor, (unsigned int, int));
    int seed = 0;
    if (g_lastcur.kind == 1 && g_lastcur.d1 && p_CGSSetCursorFromPremultipliedDataWithSeed)
        p_CGSSetCursorFromPremultipliedDataWithSeed(g_ncid, g_lastcur.d1, g_lastcur.rb, g_lastcur.d2, g_lastcur.rb2, g_lastcur.r, g_lastcur.hot,
                                                    g_lastcur.bpp, g_lastcur.spp, g_lastcur.bps, &seed);
    else if (g_lastcur.kind == 3) oah_core_cursor_set(g_lastcur.type);
    else if (g_lastcur.kind == 2) {
        if (oah_cursor_core(g_lastcur.type) >= 0) oah_core_cursor_set(oah_cursor_core(g_lastcur.type));
        else if (p_CGSSetSystemDefinedCursor) p_CGSSetSystemDefinedCursor(g_ncid, g_lastcur.type);
    } else oah_core_cursor_set(0);                                /* arrow */
}
static void oah_alc_onoff(int on) {
    int (*hide)(unsigned int) = (int (*)(unsigned int))oah_ncgs("CGSHideCursor");
    int (*show)(unsigned int) = (int (*)(unsigned int))oah_ncgs("CGSShowCursor");
    if (on == g_alc_on) return;
    g_alc_on = on;
    if (on) {      /* hides the guest made before: lift them while the outline shows */
        while (g_cur_hides > 0) { if (show) show(g_ncid); g_cur_hides--; g_alc_hides++; }
    } else {
        oah_lastcur_reapply();
        while (g_alc_hides > 0) { if (hide) hide(g_ncid); g_alc_hides--; g_cur_hides++; }
    }
}
/* the hook: (CGSRegionRef outline, int param); returns 0 */
static int oah_alc_set(unsigned int rgn, unsigned int param) {
    NCGS(int, CGSSetCursorFromPremultipliedDataWithSeed, (unsigned int, void *, int, void *, int, oah_rect_t, oah_pt_t, int, int, int, int *));
    static unsigned int fen, fnext, frel;
    static int nlog;
    oah_rect_t b;
    int x0, y0, W, H, x, y, err = -1, seed = 0, nr = 0;
    unsigned char *m, *px;
    oah_cgs_cid();
    if (param == 0xbeef || !rgn || !oah_cgs_region_bounds(rgn, &b) || b.w < 1 || b.h < 1) {
        if (nlog++ < 40) CGSLOG("[cursor] Adobe outline off (%08x, %x)\n", rgn, param);
        oah_alc_onoff(0);
        return 0;
    }
    x0 = (int)floorf(b.x) - 2; y0 = (int)floorf(b.y) - 2;
    W = (int)ceilf(b.x + b.w) + 2 - x0; H = (int)ceilf(b.y + b.h) + 2 - y0;
    if (W > 1024 || H > 1024) {                                    /* too large for a cursor: a crosshair */
        NCGS(int, CGSSetSystemDefinedCursor, (unsigned int, int));
        if (nlog++ < 40) CGSLOG("[cursor] Adobe outline %dx%d: too large: crosshair\n", W, H);
        oah_alc_onoff(1);
        if (p_CGSSetSystemDefinedCursor) p_CGSSetSystemDefinedCursor(g_ncid, 7);
        return 0;
    }
    if (!fen) { fen = oah_cgs_gsym("_CGSRegionEnumerator"); fnext = oah_cgs_gsym("_CGSNextRect"); frel = oah_cgs_gsym("_CGSReleaseRegionEnumerator"); }
    m = (unsigned char *)calloc((size_t)W * H, 1);
    px = (unsigned char *)calloc((size_t)W * H, 4);
    if (!m || !px || !fen || !fnext) { free(m); free(px); return 0; }
    {   unsigned int a[1], en, p;
        a[0] = rgn; en = oah_callppc_n("CGSRegionEnumerator", fen, 1, a);
        while (en && nr < 100000) {
            int rx0, ry0, rx1, ry1;
            a[0] = en; p = oah_callppc_n("CGSNextRect", fnext, 1, a);
            if (!p) break;
            nr++;
            rx0 = (int)floorf(oah_getf(p)) - x0; ry0 = (int)floorf(oah_getf(p + 4)) - y0;
            rx1 = rx0 + (int)(oah_getf(p + 8) + 0.5f); ry1 = ry0 + (int)(oah_getf(p + 12) + 0.5f);
            if (rx0 < 0) rx0 = 0; if (ry0 < 0) ry0 = 0; if (rx1 > W) rx1 = W; if (ry1 > H) ry1 = H;
            for (y = ry0; y < ry1; y++) for (x = rx0; x < rx1; x++) m[y * W + x] = 1;
        }
        if (en && frel) { a[0] = en; oah_callppc_n("CGSReleaseRegionEnumerator", frel, 1, a); }
    }
    /* black: region pixels on its edge; white: any other pixel next to a black one */
    for (y = 1; y < H - 1; y++) for (x = 1; x < W - 1; x++) {
        int i = y * W + x;
        if (m[i] && (!m[i - 1] || !m[i + 1] || !m[i - W] || !m[i + W])) { px[i * 4] = 255; m[i] = 2; }
    }
    for (y = 1; y < H - 1; y++) for (x = 1; x < W - 1; x++) {
        int i = y * W + x, dx, dy, near = 0;
        if (m[i] == 2) continue;
        for (dy = -1; dy <= 1 && !near; dy++) for (dx = -1; dx <= 1; dx++) if (m[i + dy * W + dx] == 2) { near = 1; break; }
        if (near) { px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = px[i * 4 + 3] = 255; }
    }
    {   oah_rect_t r; oah_pt_t hot;
        r.x = 0; r.y = 0; r.w = (float)W; r.h = (float)H;
        /* the outline comes centred on the hot spot (0,0); one placed at the origin is centred */
        if (b.x < 0 && b.y < 0) { hot.x = (float)-x0; hot.y = (float)-y0; }
        else { hot.x = W / 2.0f; hot.y = H / 2.0f; }
        oah_alc_onoff(1);
        if (p_CGSSetCursorFromPremultipliedDataWithSeed)
            err = p_CGSSetCursorFromPremultipliedDataWithSeed(g_ncid, px, W * 4, 0, 0, r, hot, 32, 4, 8, &seed);
        if (nlog++ < 40) CGSLOG("[cursor] Adobe outline %.0f,%.0f %.0fx%.0f (%d rects, param %x) -> cursor %dx%d hot %.0f,%.0f err %d\n",
                                b.x, b.y, b.w, b.h, nr, param, W, H, hot.x, hot.y, err);
    }
    free(m); free(px);
    return 0;
}
static int oah_alc_want(void) { return !getenv("OAH_NO_ADOBECURSOR"); }
#define TRKLOG(...) do { if (g_trktrace < 0) g_trktrace = getenv("OAH_TRKTRACE") ? 1 : 0; if (g_trktrace) fprintf(stderr, __VA_ARGS__); } while (0)
static int oah_trk_on(void) { static int on = -1; if (on < 0) on = getenv("OAH_NO_TRACKING") ? 0 : 1; return on; }
static int oah_trk_find(unsigned int wid, unsigned int A) {
    int i; for (i = 0; i < g_ntrk; i++) if (g_trk[i].wid == wid && g_trk[i].A == A) return i;
    return -1;
}
static int oah_trk_call(const char *n, unsigned int *R) {
    int i;
    if (!oah_trk_on()) return 0;
    if (!strcmp(n, "CGSAddTrackingArea")) {          /* (cid, wid, region, A, B, C) */
        oah_rect_t b;
        if (!oah_cgs_region_bounds(R[5], &b)) return 0;
        i = oah_trk_find(R[4], R[6]);
        if (i < 0) { if (g_ntrk >= OAH_NTRK) return 0; i = g_ntrk++; g_trk[i].inside = 0; }
        g_trk[i].wid = R[4]; g_trk[i].A = R[6]; g_trk[i].B = R[7]; g_trk[i].C = R[8];
        g_trk[i].x = b.x; g_trk[i].y = b.y; g_trk[i].w = b.w; g_trk[i].h = b.h; g_trk[i].en = 1;
        {   static int nlog; if (nlog++ < 4 || g_trktrace > 0)
                fprintf(stderr, "[trk] add window %u area %08x B %08x C %08x rect %.0f,%.0f %.0fx%.0f (%d areas)\n", R[4], R[6], R[7], R[8], b.x, b.y, b.w, b.h, g_ntrk); }
        return 0;
    }
    if (!strcmp(n, "CGSRemoveTrackingArea")) {       /* (cid, wid, A, ...) */
        if ((i = oah_trk_find(R[4], R[5])) >= 0) g_trk[i] = g_trk[--g_ntrk];
        TRKLOG("[trk] remove window %u area %08x (%d areas)\n", R[4], R[5], g_ntrk);
        return 0;
    }
    if (!strcmp(n, "CGSRemoveAllTrackingAreas")) {   /* (cid, wid) */
        for (i = 0; i < g_ntrk; ) if (g_trk[i].wid == R[4]) g_trk[i] = g_trk[--g_ntrk]; else i++;
        return 0;
    }
    if (!strcmp(n, "CGSSetTrackingAreaEnabled")) {   /* (cid, wid, A, enabled) */
        if ((i = oah_trk_find(R[4], R[5])) >= 0) { g_trk[i].en = R[6] != 0; if (!g_trk[i].en) g_trk[i].inside = 0; }
        TRKLOG("[trk] window %u area %08x enabled %u\n", R[4], R[5], R[6]);
        return 0;
    }
    if (!strcmp(n, "CGSSetTrackingAreaShape")) {     /* (cid, wid, A, region) */
        oah_rect_t b;
        if ((i = oah_trk_find(R[4], R[5])) >= 0 && oah_cgs_region_bounds(R[6], &b)) {
            g_trk[i].x = b.x; g_trk[i].y = b.y; g_trk[i].w = b.w; g_trk[i].h = b.h;
            TRKLOG("[trk] window %u area %08x shape %.0f,%.0f %.0fx%.0f\n", R[4], R[5], b.x, b.y, b.w, b.h);
        }
        return 0;
    }
    TRKLOG("[trk] %s(%08x, %08x, %08x, %08x, %08x) ignored\n", n, R[4], R[5], R[6], R[7], R[8]);
    return 0;                                         /* SetTrackingAreaOption & co. */
}
/* the pointer is at global (gx, gy) over window wid (0: none of ours) */
static void oah_trk_motion(unsigned int wid, float gx, float gy) {
    int i;
    oah_cgswin_t *w = wid ? oah_cgs_win(wid) : 0;
    float lx = w ? gx - w->x : 0, ly = w ? gy - w->y : 0;
    if (!oah_trk_on() || !g_ntrk) return;
    for (i = 0; i < g_ntrk; i++) {
        int in = g_trk[i].en && w && g_trk[i].wid == wid &&
                 lx >= g_trk[i].x && lx < g_trk[i].x + g_trk[i].w && ly >= g_trk[i].y && ly < g_trk[i].y + g_trk[i].h;
        if (in != g_trk[i].inside && g_trkq_n < OAH_TRKQ) {
            oah_cgswin_t *tw = oah_cgs_win(g_trk[i].wid);
            g_trkq[g_trkq_n].type = in ? 8 : 9;
            g_trkq[g_trkq_n].wid = g_trk[i].wid; g_trkq[g_trkq_n].A = g_trk[i].A;
            g_trkq[g_trkq_n].B = g_trk[i].B; g_trkq[g_trkq_n].C = g_trk[i].C;
            g_trkq[g_trkq_n].gx = gx; g_trkq[g_trkq_n].gy = gy;
            g_trkq[g_trkq_n].lx = tw ? gx - tw->x : gx; g_trkq[g_trkq_n].ly = tw ? gy - tw->y : gy;
            g_trkq_n++;
            TRKLOG("[trk] %s window %u area %08x at %.0f,%.0f\n", in ? "entered" : "exited", g_trk[i].wid, g_trk[i].A, lx, ly);
        }
        g_trk[i].inside = in;
    }
}
static unsigned int oah_trk_guest_event(void) {
    static unsigned int fcreate, fseti;
    unsigned int ev, rec, a[4];
    int k;
    unsigned long long ts = mach_absolute_time();
    if (!g_trkq_n) return 0;
    if (!fcreate) { fcreate = oah_cgs_gsym("_CGEventCreate"); fseti = oah_cgs_gsym("_CGEventSetIntegerValueField"); }
    if (!fcreate || !fseti) { g_trkq_n = 0; return 0; }
    a[0] = 0;
    if (!(ev = oah_callppc_n("CGEventCreate", fcreate, 1, a))) { g_trkq_n = 0; return 0; }
    rec = ev + 0xc;
    oah_put32(rec + 8, (unsigned int)g_trkq[0].type);
    oah_putf(rec + 0xc, g_trkq[0].gx); oah_putf(rec + 0x10, g_trkq[0].gy);
    oah_putf(rec + 0x14, g_trkq[0].lx); oah_putf(rec + 0x18, g_trkq[0].ly);
    oah_put32(rec + 0x1c, (unsigned int)(ts >> 32)); oah_put32(rec + 0x20, (unsigned int)ts);
    oah_put32(rec + 0x24, 0x100);
    oah_put32(rec + 0x28, g_trkq[0].wid);
    oah_put32(rec + 0x2c, g_ncid);
    {   const struct { int f; unsigned int v; } fv[] = { { 51, g_trkq[0].wid }, { 60, 0 }, { 61, g_trkq[0].A }, { 62, g_trkq[0].C }, { 63, 5 } };
        for (k = 0; k < 5; k++) {
            a[0] = ev; a[1] = (unsigned int)fv[k].f;
            a[2] = (fv[k].f == 61 || fv[k].f == 62) && (int)fv[k].v < 0 ? 0xffffffffu : 0; a[3] = fv[k].v;
            oah_callppc_n("CGEventSetIntegerValueField", fseti, 4, a);
        }
    }
    memmove(g_trkq, g_trkq + 1, (size_t)(--g_trkq_n) * sizeof g_trkq[0]);
    return ev;
}
static void oah_cgs_tags_note(oah_cgswin_t *w) {   /* help tag? (see CGSSetWindowTags) */
    static int off = -1;
    int lv = w->actlev ? w->lev_active : w->level, p;
    if (off < 0) off = getenv("OAH_NO_PASSTHRU") ? 1 : 0;
    p = !off && lv >= 101 && lv < 500 && (w->tags & 0x2) && !(w->tags & 0x04000000);
    if (p != w->passthru) CGSLOG("[cgs] window %u %s clicks (help tag)\n", w->wid, p ? "passes" : "takes");
    w->passthru = p;
}
/* our front-most ordered window at a global point, skipping click-through ones (help tags) */
static unsigned int oah_cgs_hit_nonpass(float x, float y) {
    static const void *(*wlc)(unsigned int, unsigned int);
    static long (*acount)(const void *);
    static const void *(*aget)(const void *, long);
    static int init;
    unsigned int found = 0;
    if (!init) {
        init = 1;
        wlc = (const void *(*)(unsigned int, unsigned int))dlsym(RTLD_DEFAULT, "CGWindowListCreate");
        acount = (long (*)(const void *))dlsym(RTLD_DEFAULT, "CFArrayGetCount");
        aget = (const void *(*)(const void *, long))dlsym(RTLD_DEFAULT, "CFArrayGetValueAtIndex");
    }
    if (wlc && acount && aget) {
        const void *arr = wlc(1, 0);
        long k, n = arr ? acount(arr) : 0;
        for (k = 0; k < n && !found; k++) {
            oah_cgswin_t *c = oah_cgs_win((unsigned int)(unsigned long)aget(arr, k));
            if (c && c->ordered && !c->ghost && !c->hod_hidden && !c->app_hidden && !c->passthru
                && x >= c->x && y >= c->y && x < c->x + c->w && y < c->y + c->h) found = c->wid;
        }
        if (arr) CFRelease(arr);
    }
    return found;
}
static unsigned int oah_ev_to_guest(void *e) {
    static unsigned int fcreate, fseti, fsetuni;
    NCGS(int, CGEventGetType, (void *));
    NCGS(oah_pt_t, CGEventGetLocation, (void *));
    NCGS(unsigned long long, CGEventGetFlags, (void *));
    NCGS(unsigned long long, CGEventGetTimestamp, (void *));
    NCGS(long long, CGEventGetIntegerValueField, (void *, int));
    NCGS(void, CGEventKeyboardGetUnicodeString, (void *, unsigned long, unsigned long *, unsigned short *));
    static const int mouse_f[] = { 0, 1, 3, 4, 5, 7, -1 };
    static const int key_f[] = { 8, 9, 10, -1 };
    static const int scroll_f[] = { 11, 12, 13, 88, 93, 94, 96, 97, -1 };
    static const int akd_f[] = { -1 };   /* compound data written straight into the record below */
    const int *fl = 0;
    unsigned int ev, rec, a[4], wid = 0;
    int type, i;
    oah_pt_t p, wl;
    oah_cgswin_t *w;
    if (!e) return 0;
    if (!fcreate) {
        fcreate = oah_cgs_gsym("_CGEventCreate");
        fseti = oah_cgs_gsym("_CGEventSetIntegerValueField");
        fsetuni = oah_cgs_gsym("_CGEventKeyboardSetUnicodeString");
    }
    type = p_CGEventGetType(e);
    switch (type) {
    case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 25: case 26: case 27: fl = mouse_f; break;
    case 10: case 11: case 12: fl = key_f; break;
    case 22: fl = scroll_f; break;
    case 13: case 14: case 21:                   /* AppKit/system defined, app (de)activation */
        if (type == 21 && p_CGEventGetIntegerValueField(e, 64) == 16 && !getenv("OAH_NO_LSHIDE")) {
            /* process request from the window server / Dock (field 70: 2 = hide, as from the Dock
               menu's Hide; 3 = show, e.g. a Dock click on the hidden app); Mojave's AppKit does
               the hiding itself */
            long long req = p_CGEventGetIntegerValueField(e, 70);
            EVLOG("[ev] process request %lld\n", req);
            if (req == 2) {
                static int (*bnf)(void);
                int fr = oah_is_front(), rc = -99;
                if (!g_app_hidden) oah_app_hide(1);
                if (!bnf) bnf = (int (*)(void))dlsym(RTLD_DEFAULT, "SLPSBringNextToFront");
                if (bnf && fr != 0) rc = bnf();        /* a hidden app gives up the front */
                EVLOG("[ev] hide request: front %d, next app brought to front (%d)\n", fr, rc);
            } else if (req == 3 && g_app_hidden) oah_app_hide(0);
        }
        if (type == 13 && (p_CGEventGetIntegerValueField(e, 99) == 0x7ff2 || p_CGEventGetIntegerValueField(e, 99) == 0x7ff3)) {
            int h = p_CGEventGetIntegerValueField(e, 99) == 0x7ff2;     /* hide / show from Launch Services */
            oah_cgs_nrelease(e);
            oah_app_hide(h);
            return 0;
        }
        if (type == 13 && p_CGEventGetIntegerValueField(e, 99) == 0x7ff0) {   /* Dock client marker */
            oah_cgs_nrelease(e);
            oah_dock_deliver();
            return 0;
        }
        if (type == 13 && !g_actsync_off) {      /* (de)activation: pass only real state changes */
            long long st = p_CGEventGetIntegerValueField(e, 99);
            if (st == 0x7ff1) {                      /* forced activation (Dock restore) */
                NCGS(void, CGEventSetIntegerValueField, (void *, int, long long));
                p_CGEventSetIntegerValueField(e, 99, 1);
                st = 1;
                g_app_active = 0;                    /* let it through the filter below */
                EVLOG("[ev] forced app activation (Dock restore)\n");
            }
            if (st == 1 || st == 2) {
                /* the menu bar hand-off (oah_mb_tick) briefly makes the previous app front again:
                   a deactivation that arrives while we are front is that, not a real switch */
                if (st == 2 && g_app_active && oah_is_front() == 1) {
                    EVLOG("[ev] app deactivation dropped (still front)\n");
                    oah_cgs_nrelease(e);
                    return 0;
                }
                if ((st == 1) == g_app_active) {
                    EVLOG("[ev] app %sactivation dropped (already in that state)\n", st == 1 ? "" : "de");
                    oah_cgs_nrelease(e);
                    return 0;
                }
                g_app_active = st == 1;
                g_app_act_at = CFAbsoluteTimeGetCurrent();
                g_act_click_until = g_app_active ? g_app_act_at + 1.0 : 0;
                g_raise_all_at = g_app_active ? g_app_act_at + 0.45 : 0;
                if (g_app_active && g_app_hidden) oah_app_hide(0);   /* Dock / Cmd-Tab brought a hidden app back */
                oah_hod_apply(g_app_active);
            }
        }
        if (!getenv("OAH_EV_NOAPPKIT")) { fl = akd_f; break; }
    default:
        EVLOG("[ev] native type %d (subtype %lld, data1 %lld) not passed on\n", type,
              p_CGEventGetIntegerValueField(e, 99), p_CGEventGetIntegerValueField(e, 83));
        oah_cgs_nrelease(e);
        return 0;
    }
    if (!fcreate || !fseti) { oah_cgs_nrelease(e); return 0; }
    a[0] = 0;
    ev = oah_callppc_n("CGEventCreate", fcreate, 1, a);
    if (!ev) { oah_cgs_nrelease(e); return 0; }
    {   /* every guest CGEvent has the same CF isa word; a different one means the guest call
           came back with garbage (seen once: HIToolbox's CGEventGetEventRecord rejected the
           event, then crashed releasing it) */
        static unsigned int isa0;
        unsigned int isa = 0;
        if (!probe_read(ev, &isa)) isa = 0xdeadbeef;
        if (!isa0 && isa != 0xdeadbeef) isa0 = isa;
        else if (isa != isa0) {
            fprintf(stderr, "[ev] CGEventCreate returned 0x%08x with isa 0x%08x (expected 0x%08x): event dropped\n", ev, isa, isa0);
            oah_cgs_nrelease(e);
            return 0;
        }
    }
    rec = ev + 0xc;
    p = p_CGEventGetLocation(e);
    if (fl == mouse_f || fl == scroll_f) {
        wid = (unsigned int)p_CGEventGetIntegerValueField(e, 91);          /* window under the pointer */
        if (!oah_cgs_win(wid)) wid = (unsigned int)p_CGEventGetIntegerValueField(e, 51);
    } else wid = (unsigned int)p_CGEventGetIntegerValueField(e, 51);
    if (fl == mouse_f && g_menubar_wid && oah_mb_enabled() && g_mb_published && p.y >= 0 && p.y < 22 &&
        (type == 1 || type == 2 || type == 3 || type == 4 || type == 6 || type == 7)) wid = g_menubar_wid;
    if (fl == mouse_f && g_menubar_wid && oah_mb_enabled() && g_mb_published &&
        ((type == 1 && wid == g_menubar_wid) || ((type == 5 || type == 6) && g_mb_tracking))) oah_mb_track(type, p.x, p.y);
    w = oah_cgs_win(wid);
    if (w && w->passthru && (fl == mouse_f || fl == scroll_f)) {   /* a help tag: the window below gets it */
        wid = oah_cgs_hit_nonpass(p.x, p.y);
        w = oah_cgs_win(wid);
    }
    if (!w) wid = 0;
    /* Click-to-front on activation. The 10.6 window server raised the window an inactive app
       was clicked in (AppKit does not order it itself); Mojave leaves that to the native
       AppKit, so the clicked document stayed behind the other app's windows. The first mouse
       down within a second of activation raises its (normal-level) window natively, with its
       ordering group. OAH_NO_CLICKRAISE=1 disables. */
    if ((type == 1 || type == 3) && g_act_click_until) {
        if (w && w->level == 0 && w->ordered && !w->ghost && wid != g_menubar_wid &&
            CFAbsoluteTimeGetCurrent() < g_act_click_until && !getenv("OAH_NO_CLICKRAISE")) {
            NCGS(int, CGSOrderWindow, (unsigned int, unsigned int, int, unsigned int));
            int rc = p_CGSOrderWindow ? p_CGSOrderWindow(g_ncid, wid, 1, 0) : -1;
            oah_cgs_group_follow(wid, 1);
            CGSLOG("[cgs] activating click: window %u raised (%d)\n", wid, rc);
            /* the menu bar hand-off briefly activates the previous app, whose AppKit then
               raises its own windows again: raise once more after that has settled */
            g_raise_wid = wid; g_raise_at = CFAbsoluteTimeGetCurrent() + 0.4;
            g_raise_all_at = 0;                     /* a click raises only its window */
        }
        g_act_click_until = 0;
    }
    if ((type >= 1 && type <= 4) || type == 25 || type == 26 || type == 10 || type == 11) {   /* buttons, keys */
        static int off = -1;
        if (off < 0) off = getenv("OAH_NO_CLICKREWAKE") ? 1 : 0;
        if (!off) g_ev_rewake_at = CFAbsoluteTimeGetCurrent() + 0.05;
    }
    g_hit_last_type = type;
    if ((type == 1 || type == 2 || type == 10 || type == 11) && getenv("OAH_HITLOG"))
        fprintf(stderr, "[hit] native %s at %.0f,%.0f -> wid %u (under pointer %lld, window %lld)\n",
                type == 1 ? "mouse down" : type == 2 ? "mouse up" : type == 10 ? "key down" : "key up", p.x, p.y, wid,
                p_CGEventGetIntegerValueField(e, 91), p_CGEventGetIntegerValueField(e, 51));
    if (type == 5 || type == 6 || type == 7 || type == 27) oah_trk_motion(wid, p.x, p.y);
    wl.x = w ? p.x - w->x : p.x; wl.y = w ? p.y - w->y : p.y;
    oah_put32(rec + 8, type);
    oah_putf(rec + 0xc, p.x); oah_putf(rec + 0x10, p.y);
    oah_putf(rec + 0x14, wl.x); oah_putf(rec + 0x18, wl.y);
    {
        unsigned long long ts = p_CGEventGetTimestamp(e);
        oah_put32(rec + 0x1c, (unsigned int)(ts >> 32)); oah_put32(rec + 0x20, (unsigned int)ts);
    }
    oah_put32(rec + 0x24, (unsigned int)p_CGEventGetFlags(e));
    oah_put32(rec + 0x28, wid);
    oah_put32(rec + 0x2c, g_ncid);
    for (i = 0; fl[i] >= 0; i++) {
        long long v = p_CGEventGetIntegerValueField(e, fl[i]);
        a[0] = ev; a[1] = fl[i]; a[2] = (unsigned int)((unsigned long long)v >> 32); a[3] = (unsigned int)v;
        oah_callppc_n("CGEventSetIntegerValueField", fseti, 4, a);
    }
    if (fl == key_f && fsetuni && p_CGEventKeyboardGetUnicodeString) {
        unsigned short u[8], *gu = (unsigned short *)malloc(sizeof u);
        unsigned long n = 0, k;
        p_CGEventKeyboardGetUnicodeString(e, 8, &n, u);
        for (k = 0; k < n && k < 8; k++) gu[k] = OSSwapHostToBigInt16(u[k]);
        a[0] = ev; a[1] = (unsigned int)n; a[2] = (unsigned int)gu;
        if (n) oah_callppc_n("CGEventKeyboardSetUnicodeString", fsetuni, 3, a);
        free(gu);
        /* NX key data (union at record +0x6c: origCharSet, repeat, charSet, charCode, keyCode,
           origCharCode, ...). The 10.6 window server filled charSet/charCode in the NeXTSTEP
           encoding; 10.6 HIToolbox builds kEventRawKeyDown from them, and with charCode 0 its
           key-downs came out of TSM with no key code and no modifiers (Escape / Cmd-Q lost). */
        if ((type == 10 || type == 11) && n == 1) {
            unsigned int d = rec + 0x6c;
            unsigned short cs = 0xffff, cc = 0;
            if (u[0] >= 0xf700 && u[0] <= 0xf703) { cs = 1; cc = (unsigned short)(0xad + (u[0] == 0xf701 ? 2 : u[0] == 0xf702 ? -1 : u[0] == 0xf703 ? 1 : 0)); }
            else if (u[0] < 0x80) { cs = 0; cc = u[0]; }
            else if (u[0] < 0xf700) {
                CFStringRef str = CFStringCreateWithCharacters(NULL, u, 1);
                UInt8 nb = 0;
                if (str && CFStringGetBytes(str, CFRangeMake(0, 1), kCFStringEncodingNextStepLatin, 0, false, &nb, 1, NULL) == 1) { cs = 0; cc = nb; }
                if (str) CFRelease(str);
            }
            if (cs != 0xffff) {
                *(unsigned short *)(d + 0) = OSSwapHostToBigInt16(cs);
                *(unsigned short *)(d + 4) = OSSwapHostToBigInt16(cs);
                *(unsigned short *)(d + 6) = OSSwapHostToBigInt16(cc);
                *(unsigned short *)(d + 10) = OSSwapHostToBigInt16(cc);
            }
        }
    }
    if (fl == key_f && getenv("OAH_EVTRACE")) {
        char hx[0x140 * 3];
        int o = 0;
        for (i = 0; i < 0xd0; i += 4) o += sprintf(hx + o, "%08x%s", oah_get32(ev + i), (i & 15) == 12 ? "\n    " : " ");
        EVLOG("[ev] key record (type %d):\n    %s\n", type, hx);
    }
    if (fl == akd_f) {   /* NX compound event: +0 reserved(16) +2 subtype(16) +4 data1 +8 data2 */
        /* 10.6 HIToolbox (PullEventsFromWindowServerOnConnection) reads the event data union
           at record +0x6c (subtype at +0x6e); OAH_AKD_OLD=1 restores the old +0x30 guess. */
        unsigned int d = rec + (getenv("OAH_AKD_OLD") ? 0x30 : 0x6c);
        unsigned short st = (unsigned short)p_CGEventGetIntegerValueField(e, 99);
        *(unsigned short *)(d + 2) = OSSwapHostToBigInt16(st);
        oah_put32(d + 4, (unsigned int)p_CGEventGetIntegerValueField(e, 83));
        oah_put32(d + 8, (unsigned int)p_CGEventGetIntegerValueField(e, 84));
    }
    /* App activated (AppKit-defined subtype 1): like AppKit, hand our bars to the window server
       again, or Mojave keeps showing the previous app's menus after Cmd-Tab / Dock clicks. */
    if (fl == akd_f && type == 13 && p_CGEventGetIntegerValueField(e, 99) == 1 && g_mb_published && g_menubar_wid) {
        oah_cgswin_t *mw = oah_cgs_win(g_menubar_wid);
        if (mw) oah_mb_render(mw); else oah_mb_publish();
        CGSLOG("[cgs] app activated: menu bar republished\n");
        oah_mb_tick(1);
    }
    if (fl == akd_f && type != 13 && g_evtrace > 0) {   /* raw record of system/app-defined events */
        NCGS(CFDataRef, CGEventCreateData, (void *, void *));
        CFDataRef d = p_CGEventCreateData ? p_CGEventCreateData(0, e) : 0;
        if (d) {
            const unsigned char *b = CFDataGetBytePtr(d);
            CFIndex L = CFDataGetLength(d), q;
            char hx[3 * 256 + 1]; int o = 0;
            for (q = 0; q < L && q < 256; q++) o += sprintf(hx + o, "%02x%s", b[q], (q & 3) == 3 ? " " : "");
            fprintf(stderr, "[ev] type %d raw (%ld bytes): %s\n", type, (long)L, hx);
            CFRelease(d);
        }
    }
    if (fl == akd_f)
        EVLOG("[ev] appkit-type %d fields 99=%lld 83=%lld 84=%lld 85=%lld 86=%lld\n", type,
              p_CGEventGetIntegerValueField(e, 99), p_CGEventGetIntegerValueField(e, 83),
              p_CGEventGetIntegerValueField(e, 84), p_CGEventGetIntegerValueField(e, 85),
              p_CGEventGetIntegerValueField(e, 86));
    if (type != 5 && type != 6 && type != 7 && type != 27)
        EVLOG("[ev] -> guest event 0x%08x type %d at %.0f,%.0f window %u (local %.0f,%.0f) flags 0x%x clicks %lld ts %llu\n",
              ev, type, p.x, p.y, wid, wl.x, wl.y, (unsigned int)p_CGEventGetFlags(e),
              fl == mouse_f ? p_CGEventGetIntegerValueField(e, 1) : 0LL, p_CGEventGetTimestamp(e));
    oah_cgs_nrelease(e);
    return ev;
}
/* Window exposure. 10.6 WindowServer told a client that a window's contents were needed
   with an AppKit-defined event (type 13, subtype 0 = NSWindowExposedEventType); HIToolbox's
   HandleWindowExposed then reads CGSGetWindowDirtyRegion and PaintOne()s it, which is what
   produces update events / kEventWindowDrawContent for non-compositing windows (dialogs,
   alerts, classic apps). Mojave keeps a retained backing store and never sends these, so
   the shim can queue one whenever a window is ordered in or resized (OAH_EXPOSE=1).
   Off by default: once IsProcessVisible answered correctly, HIToolbox generated the
   updates itself, and the exposure path's SyncWindowVisibleShape intersected a
   window-relative content shape with our region and clipped the port's visRgn to
   (size - window origin) -- updtest's erase covered only 280x102 of 400x200. */
static void oah_cgs_expose_now(oah_cgswin_t *w);
static void oah_cgs_expose(oah_cgswin_t *w) {
    static int off = -1;
    if (off < 0) off = getenv("OAH_EXPOSE") ? 0 : 1;
    if (!w || off) return;
    oah_cgs_expose_now(w);
}
static void oah_cgs_expose_now(oah_cgswin_t *w) {
    int i;
    if (!w) return;
    w->dirty = 1;
    pthread_mutex_lock(&g_evq_mu);
    for (i = 0; i < g_expq_n; i++) if (g_expq[i] == w->wid) break;
    if (i == g_expq_n && g_expq_n < OAH_EXPQ) g_expq[g_expq_n++] = w->wid;
    /* wake the guest's event source even if no native event is queued */
    if (g_ev_gport && !g_ev_pending) {
        mach_msg_header_t h;
        memset(&h, 0, sizeof h);
        h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
        h.msgh_size = sizeof h;
        h.msgh_remote_port = g_ev_gport;
        h.msgh_id = 0x4f414845;
        if (mach_msg(&h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof h, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL) == KERN_SUCCESS) {
            g_ev_pending = 1; g_ev_wakes++;
        }
    }
    pthread_mutex_unlock(&g_evq_mu);
}
static unsigned int oah_ev_expose_guest(void) {
    static unsigned int fcreate;
    unsigned int wid = 0, ev, rec, a[1];
    oah_cgswin_t *w;
    pthread_mutex_lock(&g_evq_mu);
    if (g_expq_n) { wid = g_expq[0]; memmove(g_expq, g_expq + 1, (--g_expq_n) * sizeof g_expq[0]); }
    pthread_mutex_unlock(&g_evq_mu);
    if (!wid || !(w = oah_cgs_win(wid))) return 0;
    if (!fcreate) fcreate = oah_cgs_gsym("_CGEventCreate");
    if (!fcreate) return 0;
    a[0] = 0;
    ev = oah_callppc_n("CGEventCreate", fcreate, 1, a);
    if (!ev) return 0;
    rec = ev + 0xc;
    oah_put32(rec + 8, 13);
    oah_putf(rec + 0xc, w->x); oah_putf(rec + 0x10, w->y);
    oah_putf(rec + 0x14, 0); oah_putf(rec + 0x18, 0);
    oah_put32(rec + 0x28, wid);
    oah_put32(rec + 0x2c, g_ncid);
    *(unsigned short *)(rec + 0x6e) = 0;                   /* subtype 0: window exposed */
    oah_put32(rec + 0x70, 0); oah_put32(rec + 0x74, 0);
    g_exp_sent++;
    CGSLOG("[cgs] expose event for window %u (%.0fx%.0f) #%lu\n", wid, w->w, w->h, g_exp_sent);
    return ev;
}
static unsigned int oah_ev_next_guest(void) {
    void *e;
    unsigned int ev;
    if (g_expq_n && (ev = oah_ev_expose_guest())) return ev;
    if (g_trkq_n && (ev = oah_trk_guest_event())) return ev;
    while ((e = oah_ev_pop()))
        if ((ev = oah_ev_to_guest(e))) return ev;
    return 0;
}

/* ---- ordering groups (child windows) ---- */
#define OAH_NGROUP 64
static struct { unsigned int parent, child; int mode; } g_cgs_group[OAH_NGROUP];
static void oah_cgs_group_add(unsigned int parent, unsigned int child, int mode) {
    int i, fr = -1;
    for (i = 0; i < OAH_NGROUP; i++) {
        if (g_cgs_group[i].child == child) { g_cgs_group[i].parent = parent; g_cgs_group[i].mode = mode; return; }
        if (!g_cgs_group[i].child && fr < 0) fr = i;
    }
    if (fr >= 0) { g_cgs_group[fr].parent = parent; g_cgs_group[fr].child = child; g_cgs_group[fr].mode = mode; }
}
static void oah_cgs_group_del(unsigned int child) {
    int i;
    for (i = 0; i < OAH_NGROUP; i++) if (g_cgs_group[i].child == child) g_cgs_group[i].child = g_cgs_group[i].parent = 0;
}
/* ---- movement groups: windows that move with a parent (CGSMoveWindowWithGroup) ----
   The window server moves them; this keeps our cached positions (hit testing, blits) in step. */
static struct { unsigned int parent, child; } g_cgs_mgroup[OAH_NGROUP];
static void oah_cgs_mgroup_set(unsigned int parent, unsigned int child, int add) {
    int i, fr = -1;
    for (i = 0; i < OAH_NGROUP; i++) {
        if (g_cgs_mgroup[i].parent == parent && g_cgs_mgroup[i].child == child) { if (!add) g_cgs_mgroup[i].parent = g_cgs_mgroup[i].child = 0; return; }
        if (!g_cgs_mgroup[i].child && fr < 0) fr = i;
    }
    if (add && fr >= 0) { g_cgs_mgroup[fr].parent = parent; g_cgs_mgroup[fr].child = child; }
}
static void oah_cgs_mgroup_moved(unsigned int parent, double dx, double dy, int depth) {
    int i;
    if (depth > 4) return;
    for (i = 0; i < OAH_NGROUP; i++) if (g_cgs_mgroup[i].child && g_cgs_mgroup[i].parent == parent) {
        oah_cgswin_t *c = oah_cgs_win(g_cgs_mgroup[i].child);
        if (c) { c->x += (float)dx; c->y += (float)dy; }
        CGSLOG("[cgs] movement group: %u follows %u by %.0f,%.0f\n", g_cgs_mgroup[i].child, parent, dx, dy);
        oah_cgs_mgroup_moved(g_cgs_mgroup[i].child, dx, dy, depth + 1);
    }
}
static void oah_cgs_order_child(oah_cgswin_t *cw, unsigned int parent, int mode) {
    NCGS(int, CGSOrderWindow, (unsigned int, unsigned int, int, unsigned int));
    if (!cw->flushes) oah_cgs_blit(cw);
    p_CGSOrderWindow(g_ncid, cw->wid, mode < 0 ? -1 : 1, parent);
    if (!cw->ordered) { cw->ordered = 1; oah_cgs_expose(cw); }
    CGSLOG("[cgs] child window %u ordered %s parent %u\n", cw->wid, mode < 0 ? "below" : "above", parent);
}
/* the parent was ordered in (mode != 0) or out: its children follow */
/* windows the current CGSOrderWindowList call orders itself: their parent's follow leaves them
   where the list put them (TextWrangler's print sheet: the list ordered the document window below
   the sheet, then the Documents drawer below the document; following the drawer re-ordered both
   children "above the drawer" one after the other, so the sheet ended up behind its document) */
static const unsigned int *g_cgs_follow_skip; static unsigned int g_cgs_follow_nskip;
static void oah_cgs_group_follow(unsigned int parent, int mode) {
    NCGS(int, CGSOrderWindow, (unsigned int, unsigned int, int, unsigned int));
    int i;
    for (i = 0; i < OAH_NGROUP; i++) if (g_cgs_group[i].child && g_cgs_group[i].parent == parent) {
        oah_cgswin_t *cw = oah_cgs_win(g_cgs_group[i].child);
        {   unsigned int k, skip = 0;
            for (k = 0; k < g_cgs_follow_nskip; k++) if (g_cgs_follow_skip[k] == g_cgs_group[i].child) skip = 1;
            if (skip && mode && cw) continue;
            /* the parent went down (ordered below another window): a child already in front of it
               stays in front of it where it is; re-ordering it "just above the parent" put it
               behind the window the parent was ordered below (the print sheet behind its document) */
            if (mode < 0 && cw && cw->ordered) continue;
        }
        if (!cw) { g_cgs_group[i].child = g_cgs_group[i].parent = 0; continue; }
        if (mode) oah_cgs_order_child(cw, parent, g_cgs_group[i].mode);
        else if (cw->ordered) { p_CGSOrderWindow(g_ncid, cw->wid, 0, 0); cw->ordered = 0; }
    }
}
static void *oah_hideothers_thread(void *p) {
    int (*f)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "_HideOtherApplications");
    (void)p; if (f) f();
    return 0;
}
static void *oah_showall_thread(void *p) {
    int (*f)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "_ShowAllApplications");
    (void)p; if (f) f();
    return 0;
}
static void *oah_showhide_thread(void *p) {
    unsigned int *a = (unsigned int *)p;
    int (*shp)(const unsigned int *, unsigned char) = (int (*)(const unsigned int *, unsigned char))dlsym(RTLD_DEFAULT, "ShowHideProcess");
    if (shp) shp(a, (unsigned char)a[2]);
    free(a);
    return 0;
}
/* SetSystemUIMode from the guest: Mojave's HIToolbox records the mode in the app's LaunchServices
   entry (UIPresentationMode / UIPresentationOptions), which the Dock and SystemUIServer follow,
   but called from the translated app it never got there (the key stayed unset) and Mojave's menu
   bar covered the top of full-screen games (Build-a-lot 2's goals bar). Set the entry directly.
   OAH_NO_LSUIMODE=1 disables. */
static void oah_ls_set_uimode(unsigned int mode, unsigned int opts) {
    static int (*set)(int, CFTypeRef, CFStringRef, CFTypeRef, CFDictionaryRef *);
    static CFTypeRef (*getasn)(void);
    static const char *const names[] = { "Normal", "ContentSuppressed", "ContentHidden", "AllHidden", "AllSuppressed" };
    CFTypeRef asn; CFStringRef v; CFNumberRef o; int e1 = -1, e2 = -1, oi = (int)opts;
    if (getenv("OAH_NO_LSUIMODE") || mode > 4) return;
    if (!set) {
        void *cs = dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_LAZY | RTLD_GLOBAL);
        if (cs) { *(void **)&set = dlsym(cs, "_LSSetApplicationInformationItem"); *(void **)&getasn = dlsym(cs, "_LSGetCurrentApplicationASN"); }
    }
    if (!set || !getasn || !(asn = getasn())) return;
    v = CFStringCreateWithCString(NULL, names[mode], kCFStringEncodingASCII);
    o = CFNumberCreate(NULL, kCFNumberIntType, &oi);
    if (o) e1 = set(-2, asn, CFSTR("UIPresentationOptions"), o, NULL);
    if (v) e2 = set(-2, asn, CFSTR("UIPresentationMode"), v, NULL);
    if (v) CFRelease(v);
    if (o) CFRelease(o);
    fprintf(stderr, "[cgs] system UI mode %s, options 0x%x -> LaunchServices %d %d\n", names[mode], opts, e1, e2);
}
static unsigned long g_cgs_flush_skipped;   /* empty flushes skipped (see CGSFlushWindowContentRegion) */
static int g_flushlog_v = -1;               /* OAH_FLUSHLOG=1: one line per window flush (time, thread, region) */
#define g_flushlog (g_flushlog_v < 0 ? (g_flushlog_v = getenv("OAH_FLUSHLOG") ? 1 : 0) : g_flushlog_v)
static int oah_cgs_call(const char *n, unsigned int *R) {
    double *F = (double *)((unsigned char *)R + 0x1e8);
    oah_cgswin_t *w;
    if (!oah_gtb()) return 0;
    oah_mb_tick(0);                                 /* menu bar: activation hand-off, late re-sets */
    if (n[0] == '_' && n[1] == '_') n++;            /* __CGSDefaultConnection */
    {
        static int calls = -1;
        if (calls < 0) calls = getenv("OAH_CGSCALLS") ? 1 : 0;
        if (calls && strcmp(n, "_CGSDefaultConnection") && strcmp(n, "CGSInputModifierKeyState"))
            fprintf(stderr, "[cgscall] %s r3=%x r4=%x r5=%x lr=%08x\n", n, R[3], R[4], R[5], R[0x90 / 4]);
    }

    /* connection */
    if (!strcmp(n, "_CGSDefaultConnection") || !strcmp(n, "CGSMainConnectionID") ||
        !strcmp(n, "CGSDefaultConnectionForThread")) { R[3] = oah_cgs_cid(); return 1; }
    if (!strcmp(n, "CGSInitialize")) { oah_cgs_cid(); R[3] = 0; return 1; }
    if (!strcmp(n, "CGSConnectionGetPID")) {
        /* (cid, pid_t *out): CoreDrag asks which process owns the window under a drag, to send
           it the drag messages. Unanswered, *out stayed garbage and a drag inside the app's own
           window (moving selected text in TextWrangler) found no destination. */
        static int (*get)(int, pid_t *);
        pid_t p = 0;
        int e = 0;
        if (R[3] == oah_cgs_cid()) p = getpid();
        else {
            if (!get) *(void **)&get = dlsym(RTLD_DEFAULT, "CGSConnectionGetPID");
            e = get ? get((int)R[3], &p) : 1000;
        }
        if (!e && R[4]) oah_put32(R[4], (unsigned int)p);
        R[3] = (unsigned int)e;
        return 1;
    }

    /* events (see the pump above) */
    if (!strcmp(n, "CGSGetEventPort")) {               /* (cid, mach_port_t *out) */
        if (!pthread_main_np() && !getenv("OAH_EVPORT_ANYTHREAD")) {
            /* A second thread with its own event loop on the window server port (Photoshop's
               AdobeLargeCursor plug-in: CGSNewConnection + CGSGetEventPort + mach_msg) took the
               wake-up messages meant for the main thread, which then slept through clicks and
               keys. Give other threads a port of their own that nothing posts to. */
            static mach_port_t other;
            if (!other) {
                mach_port_t p = MACH_PORT_NULL;
                if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p) == KERN_SUCCESS) {
                    mach_port_insert_right(mach_task_self(), p, p, MACH_MSG_TYPE_MAKE_SEND);
                    other = p;
                }
            }
            oah_put32(R[4], other);
            CGSLOG("[cgs] GetEventPort from a secondary thread -> private port 0x%x\n", other);
            R[3] = other ? 0 : 1000;
            return 1;
        }
        if (!g_ev_gport) {
            mach_port_t p = MACH_PORT_NULL;
            if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p) == KERN_SUCCESS) {
                mach_port_insert_right(mach_task_self(), p, p, MACH_MSG_TYPE_MAKE_SEND);
                pthread_mutex_lock(&g_evq_mu); g_ev_gport = p; pthread_mutex_unlock(&g_evq_mu);
            }
            oah_ev_start();
        }
        oah_put32(R[4], g_ev_gport);
        CGSLOG("[cgs] GetEventPort -> 0x%x\n", g_ev_gport);
        R[3] = g_ev_gport ? 0 : 1000;
        return 1;
    }
    if (!strcmp(n, "CGSExtractEventRecordsFromMessage")) { R[3] = 0; return 1; }
    if (!strcmp(n, "CGSGetCurrentMouseButtonState")) {   /* (cid, UInt32 *buttons) */
        /* Control tracking loops poll this until the button goes up. */
        static int (*bs)(int, int);
        unsigned int m = 0;
        int b;
        if (!bs) bs = (int (*)(int, int))dlsym(RTLD_DEFAULT, "CGEventSourceButtonState");
        /* WindowServer numbers buttons from bit 1: SL HIToolbox Button() is
           (state >> 1) & 1. With the left button in bit 0, StillDown() was false right after
           every mouse-down, so AppleWorks' pencil/brush drew a single dot. */
        if (bs) for (b = 0; b < 5; b++) if (bs(0, b)) m |= 1u << (b + 1);
        if (R[4]) oah_put32(R[4], m);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSGetKeys")) {                    /* (KeyMap *) - HIToolbox GetKeys(): keys held right now */
        /* Unbridged it returned an untouched buffer: Word 2004 polls it thousands of times a
           minute (modifier checks while typing) and saw stale or random keys held. */
        static _Bool (*ks)(int, unsigned short);
        unsigned char b[16] = { 0 };
        int k;
        if (!ks) ks = (_Bool (*)(int, unsigned short))dlsym(RTLD_DEFAULT, "CGEventSourceKeyState");
        if (ks) for (k = 0; k < 128; k++) if (ks(0 /* kCGEventSourceStateCombinedSessionState */, k)) b[k >> 3] |= 1 << (k & 7);
        if (R[3]) for (k = 0; k < 4; k++) oah_put32(R[3] + 4 * k, (unsigned)b[4 * k] << 24 | (unsigned)b[4 * k + 1] << 16 | (unsigned)b[4 * k + 2] << 8 | b[4 * k + 3]);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSGetWindowTransform")) {        /* (cid, wid, CGAffineTransform *) global -> window */
        float t[6] = { 1, 0, 0, 1, 0, 0 };
        unsigned int v;
        int i;
        w = oah_cgs_win(R[4]);
        if (w) { t[4] = -w->x; t[5] = -w->y; }
        if (R[5]) for (i = 0; i < 6; i++) { memcpy(&v, &t[i], 4); oah_put32(R[5] + 4 * i, v); }
        R[3] = w ? 0 : 1000;
        return 1;
    }
    if (!strcmp(n, "CGSFindWindowAndOwner")) {
        /* (cid, 0, 1, 0, CGPoint *screen, CGPoint *local out, wid *out, cid *owner out)
           HIToolbox hit-tests mouse-downs with this; unanswered, every click missed. */
        NCGS(int, CGSFindWindowAndOwner, (int, int, int, int, oah_npt_t *, oah_npt_t *, unsigned int *, int *));
        unsigned int sp = R[7], lp = R[8], widp = R[9], ownp = R[10], v, found = 0;
        oah_pt_t g = { 0, 0 };
        oah_npt_t np, nl = { 0, 0 };
        int owner = 0, i;
        if (sp) { v = oah_get32(sp); memcpy(&g.x, &v, 4); v = oah_get32(sp + 4); memcpy(&g.y, &v, 4); }
        np.x = g.x; np.y = g.y;
        if (p_CGSFindWindowAndOwner && p_CGSFindWindowAndOwner(g_ncid, 0, 1, 0, &np, &nl, &found, &owner) == 0
            && oah_cgs_win(found) && !oah_cgs_win(found)->passthru) {
            /* one of ours */
        } else {
            /* Hit-test our own windows front to back. Array order is creation order, so a
               later, larger window (a document) used to win over a floating palette in front
               of it, and Illustrator's toolbox clicks went to the canvas (OAH_NO_ZHIT). */
            static const void *(*wlc)(unsigned int, unsigned int);
            static long (*acount)(const void *);
            static const void *(*aget)(const void *, long);
            static void (*rel)(const void *);
            static int zinit;
            found = 0;
            if (!zinit) {
                zinit = 1;
                if (!getenv("OAH_NO_ZHIT")) {
                    wlc = (const void *(*)(unsigned int, unsigned int))dlsym(RTLD_DEFAULT, "CGWindowListCreate");
                    acount = (long (*)(const void *))dlsym(RTLD_DEFAULT, "CFArrayGetCount");
                    aget = (const void *(*)(const void *, long))dlsym(RTLD_DEFAULT, "CFArrayGetValueAtIndex");
                    rel = (void (*)(const void *))dlsym(RTLD_DEFAULT, "CFRelease");
                }
            }
            if (wlc && acount && aget && rel) {
                const void *arr = wlc(1 /* kCGWindowListOptionOnScreenOnly */, 0);
                long k, n = arr ? acount(arr) : 0;
                for (k = 0; k < n && !found; k++) {
                    unsigned int id = (unsigned int)(unsigned long)aget(arr, k);
                    oah_cgswin_t *c = oah_cgs_win(id);
                    if (c && c->ordered && !c->ghost && !c->hod_hidden && !c->app_hidden && !c->passthru
                        && g.x >= c->x && g.y >= c->y && g.x < c->x + c->w && g.y < c->y + c->h) {
                        found = c->wid; nl.x = g.x - c->x; nl.y = g.y - c->y;
                    }
                }
                if (arr) rel(arr);
            }
            if (!found) for (i = 0; i < OAH_MAXWIN; i++) {
                oah_cgswin_t *c = &g_cgswin[i];
                if (c->wid && c->ordered && !c->passthru && g.x >= c->x && g.y >= c->y && g.x < c->x + c->w && g.y < c->y + c->h) {
                    found = c->wid; nl.x = g.x - c->x; nl.y = g.y - c->y;
                }
            }
        }
        if (lp) { float f = (float)nl.x; memcpy(&v, &f, 4); oah_put32(lp, v); f = (float)nl.y; memcpy(&v, &f, 4); oah_put32(lp + 4, v); }
        if (widp) oah_put32(widp, found);
        if (ownp) oah_put32(ownp, found ? R[3] : 0);
        EVLOG("[ev] FindWindowAndOwner(%x %x %x) %.0f,%.0f -> wid %u local %.0f,%.0f level %d\n", R[4], R[5], R[6], g.x, g.y, found, nl.x, nl.y,
              oah_cgs_win(found) ? oah_cgs_win(found)->level : -99);
        if (getenv("OAH_HITLOG"))   /* just the hit tests (OAH_EVTRACE logs every event) */
            fprintf(stderr, "[hit] %.0f,%.0f -> wid %u level %d (%s)\n", g.x, g.y, found, oah_cgs_win(found) ? oah_cgs_win(found)->level : -99,
                    p_CGSFindWindowAndOwner ? "server or own list" : "own list");
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "_CPSRegisterWithServer")) {         /* (flags?, ProcessSerialNumber *out, ...) */
        /* The guest CG's CPS state has no server connection, so this returned 1003 and
           HIServices' _RegisterApplication aborted ("FAILED TO GET ASN FROM CORESERVICES").
           Register the process natively and hand back Mojave's PSN for it. */
        struct { unsigned int hi, lo; } psn = { 0, 0 };
        int (*gcp)(void *);
        int err;
        if (!g_carbon_handle)
            g_carbon_handle = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon", RTLD_LAZY | RTLD_GLOBAL);
        if (getenv("OAH_GTB") && !g_fg_done) { g_fg_done = 1; oah_make_foreground(); }
        gcp = g_carbon_handle ? (int (*)(void *))dlsym(g_carbon_handle, "GetCurrentProcess") : 0;
        err = gcp ? gcp(&psn) : -1;
        if (R[4]) { oah_put32(R[4], psn.hi); oah_put32(R[4] + 4, psn.lo); }
        CGSLOG("[cgs] CPSRegisterWithServer pid %u -> psn %u.%u (err %d)\n", R[5], psn.hi, psn.lo, err);
        R[3] = (err == 0 && psn.lo) ? 0 : 1003;
        return 1;
    }
    if (!strcmp(n, "CGWindowServerCFMachPort")) {      /* () -> CFMachPortRef (guest) */
        /* HIToolbox's _FirstEventTime only installs its WindowServer event source when
           this is non-NULL (it uses the port for a server-death callback). Hand it a
           guest CFMachPort on a private port that never receives anything. */
        static unsigned int gmp;
        if (!gmp) {
            unsigned int f = oah_guest_sym("CoreFoundation.framework", "_CFMachPortCreateWithPort"), a[5];
            mach_port_t p = MACH_PORT_NULL;
            if (f && mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p) == KERN_SUCCESS) {
                mach_port_insert_right(mach_task_self(), p, p, MACH_MSG_TYPE_MAKE_SEND);
                a[0] = 0; a[1] = p; a[2] = 0; a[3] = 0; a[4] = 0;
                gmp = oah_callppc_n("CFMachPortCreateWithPort", f, 5, a);
            }
            CGSLOG("[cgs] CGWindowServerCFMachPort -> 0x%08x (port 0x%x)\n", gmp, p);
        }
        R[3] = gmp;
        return 1;
    }
    if (!strcmp(n, "CGEventCreateNextEvent")) {        /* (cid) -> guest CGEventRef or NULL */
        unsigned int in3 = R[3];
        oah_objc_probe();
        oah_ev_start();
        oah_aen_dispatch();                          /* Apple Events from native processes */
        oah_clip_resolve_main();                     /* a Carbon app's clipboard promises */
        /* Sound Manager callbacks and APlayer "track done" are delivered at bridge entries, but
           only after the WindowServer bridges had returned. In guest-toolbox mode an idle app
           enters nothing else: Kid Pix's button-click sound got its first callback, then none,
           and the button waited for the sound to finish until the next mouse click came along
           (the Slide Show button needed a second click). The event poll is a safe point (the
           event loop, main thread). OAH_NO_EVSNDPUMP=1 turns this off. */
        {   static int off = -1;
            if (off < 0) off = getenv("OAH_NO_EVSNDPUMP") ? 1 : 0;
            if (!off) { oah_snd_pump(); oah_apl_poll(); }
        }
        g_hit_last_type = 0;
        if (oah_ev_defer_peek(R)) { R[3] = 0; return 1; }
        R[3] = oah_ev_next_guest();
        EVLOG("[ev] CGEventCreateNextEvent R=%p in=%08x out=%08x lr=%08x\n", (void *)R, in3, R[3], R[0x90 / 4]);
        if (R[3] && (g_hit_last_type == 1 || g_hit_last_type == 2) && getenv("OAH_HITLOG")) {   /* who pulled the click */
            unsigned int sp = R[1], n, b, back, ra;
            fprintf(stderr, "[hit]   pulled by:");
            for (n = 0; n < 16 && sp && !(sp & 3) && sp > 0x1000; n++) {
                const char *nm, *sl;
                back = 0; ra = 0;
                if (!oah_bt_word(sp, &back) || !back || back <= sp || !oah_bt_word(back + 8, &ra)) break;
                nm = oah_guest_image_name(ra, &b); sl = strrchr(nm, '/');
                fprintf(stderr, " %s+0x%x", sl ? sl + 1 : nm, ra - b);
                sp = back;
            }
            fprintf(stderr, "\n");
        }
        if (R[3] && getenv("OAH_EVOBJ")) {
            unsigned int w[6] = { 0, 0, 0, 0, 0, 0 }, k;
            for (k = 0; k < 6; k++) probe_read(R[3] + 4 * k, &w[k]);
            fprintf(stderr, "[evobj] %08x: %08x %08x %08x %08x %08x %08x\n", R[3], OSSwapBigToHostInt32(w[0]),
                    OSSwapBigToHostInt32(w[1]), OSSwapBigToHostInt32(w[2]), OSSwapBigToHostInt32(w[3]),
                    OSSwapBigToHostInt32(w[4]), OSSwapBigToHostInt32(w[5]));
        }
        return 1;
    }
    if (!strcmp(n, "CGSGetNextEventRecordInternal")) { /* (cid, CGSEventRecord *, size) */
        static unsigned int frel;
        unsigned int ev, sz = R[5] ? R[5] : 0xcc;
        oah_ev_start();
        oah_aen_dispatch();                          /* Apple Events from native processes */
        oah_clip_resolve_main();                     /* a Carbon app's clipboard promises */
        if (sz > 0xcc) sz = 0xcc;
        ev = oah_ev_next_guest();
        if (ev) {
            memcpy((void *)R[4], (void *)(ev + 0xc), sz);
            if (!frel) frel = oah_guest_sym("CoreFoundation.framework", "_CFRelease");
            if (frel) { unsigned int a[1]; a[0] = ev; oah_callppc_n("CFRelease", frel, 1, a); }
        } else if (R[4]) oah_put32(R[4] + 8, 0);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSGetCurrentCursorLocation")) {   /* (cid, CGPoint *) */
        NCGS(int, CGSGetCurrentCursorLocation, (unsigned int, oah_pt_t *));
        oah_pt_t p = { 0, 0 };
        R[3] = p_CGSGetCurrentCursorLocation ? p_CGSGetCurrentCursorLocation(oah_cgs_cid(), &p) : 1000;
        if (oah_inj_drives()) { p.x = g_inj_x; p.y = g_inj_y; R[3] = 0; }   /* injector drives the pointer */
        oah_putf(R[4], p.x); oah_putf(R[4] + 4, p.y);
        return 1;
    }
    if (!strcmp(n, "CGSGetWindowMouseLocation")) {     /* (cid, wid, CGPoint *) window-relative */
        NCGS(int, CGSGetWindowMouseLocation, (unsigned int, unsigned int, oah_pt_t *));
        oah_pt_t p = { 0, 0 };
        R[3] = p_CGSGetWindowMouseLocation ? p_CGSGetWindowMouseLocation(oah_cgs_cid(), R[4], &p) : 1000;
        if (oah_inj_drives() && (w = oah_cgs_win(R[4]))) { p.x = g_inj_x - w->x; p.y = g_inj_y - w->y; R[3] = 0; }
        oah_putf(R[5], p.x); oah_putf(R[5] + 4, p.y);
        return 1;
    }
    if (!strcmp(n, "CGSMoveWindowWithGroup")) {        /* (cid, wid, CGPoint *) */
        /* The windows in its movement group move along (CGSAddWindowToWindowMovementGroup): the
           window server's own CGSMoveWindowWithGroup does that. A plain CGSMoveWindow left them
           behind: Illustrator CS2's splash text window stayed where the splash was first placed,
           up and to the left of the centred splash. OAH_NO_MOVEGROUP=1: old way. */
        NCGS(int, CGSMoveWindow, (unsigned int, unsigned int, const oah_pt_t *));
        static int (*mwg)(unsigned int, unsigned int, const oah_pt_t *);
        static int nomg = -1;
        oah_pt_t p;
        double dx = 0, dy = 0;
        p.x = oah_getf(R[5]); p.y = oah_getf(R[5] + 4);
        if (nomg < 0) { nomg = getenv("OAH_NO_MOVEGROUP") ? 1 : 0; if (!nomg) *(void **)&mwg = dlsym(RTLD_DEFAULT, "CGSMoveWindowWithGroup"); }
        w = oah_cgs_win(R[4]);
        if (w) { dx = p.x - w->x; dy = p.y - w->y; }
        /* A window that comes on screen from far off it shows whatever was last sent to it. Office
           2004 builds an alert sheet at y -565, erases it and flushes the bare background, moves it
           under the title bar and only then draws its text and buttons: the alert appeared as an
           empty panel for a frame or two, its pulsing default button missing. Such a move waits
           (oah_cgs_reveal) until the window's pixels are next sent, at most 100 ms. Later moves
           before that only change where it will go. OAH_NO_REVEALWAIT=1 moves at once. */
        if (w && (w->pmv || (w->ordered && w->store && (dx || dy) && !getenv("OAH_NO_REVEALWAIT") &&
                              (w->y + w->h <= 0 || w->x + w->w <= 0) && p.y + w->h > 0 && p.x + w->w > 0))) {
            pthread_mutex_lock(&g_reveal_mu);
            if (!w->pmv) { w->pmt = CFAbsoluteTimeGetCurrent(); g_reveal_n++; }
            w->pmv = 1; w->pmx = (float)p.x; w->pmy = (float)p.y;
            pthread_mutex_unlock(&g_reveal_mu);
            w->x = p.x; w->y = p.y;
            if (mwg && (dx || dy)) oah_cgs_mgroup_moved(R[4], dx, dy, 0);   /* its group follows now */
            R[3] = 0;
            CGSLOG("[cgs] MoveWindowWithGroup %u to %.0f,%.0f held until its pixels are sent\n", R[4], p.x, p.y);
            return 1;
        }
        if (w) { w->x = p.x; w->y = p.y; }
        R[3] = mwg ? mwg(oah_cgs_cid(), R[4], &p) : p_CGSMoveWindow(oah_cgs_cid(), R[4], &p);
        if (mwg && (dx || dy)) oah_cgs_mgroup_moved(R[4], dx, dy, 0);
        CGSLOG("[cgs] MoveWindowWithGroup %u to %.0f,%.0f -> %d (lr %08x)\n", R[4], p.x, p.y, R[3], R[0x90 / 4]);
        if (getenv("OAH_CGSTRACE")) {                  /* guest frames: back chain at 0(r1), saved LR at 8(frame) */
            unsigned int fp = R[1], k;
            fprintf(stderr, "[cgs]   frames:");
            for (k = 0; k < 8 && fp > 0x1000 && oah_readable(fp, 12); k++) {
                fp = OSSwapBigToHostInt32(*(unsigned int *)(uintptr_t)fp);
                if (fp < 0x1000 || !oah_readable(fp, 12)) break;
                fprintf(stderr, " %08x", OSSwapBigToHostInt32(*(unsigned int *)(uintptr_t)(fp + 8)));
            }
            fprintf(stderr, "\n");
        }
        return 1;
    }
    if (!strcmp(n, "CGSNewConnection")) {             /* (attrs, CGSConnectionID *out) */
        oah_put32(R[4], oah_cgs_cid());
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSServerIsConnected")) { R[3] = 1; return 1; }
    if (!strcmp(n, "ParamText")) {                    /* diagnostic: log the four Pascal strings */
        int k;
        for (k = 0; k < 4; k++) {
            const unsigned char *p = (const unsigned char *)R[3 + k];
            fprintf(stderr, "[diag] ParamText ^%d = \"%.*s\"\n", k, p ? p[0] : 0, p ? (const char *)p + 1 : "");
        }
        return 1;
    }
    static unsigned int ui_mode, ui_opts;
    /* Menu bar. 10.6 registered the app's menu bar window with the WindowServer, which kept
       it on screen at menu bar level; HIToolbox orders it out itself afterwards. Here it is
       kept ordered in at kCGMainMenuWindowLevel, and Mojave's own menu bar is hidden while
       we are frontmost (SetSystemUIMode in oah_make_foreground). OAH_NATIVE_MENUBAR=1: off. */
    if (!strcmp(n, "CGSRegisterMenuBarWindow")) {      /* (cid, wid, ...) */
        NCGS(int, CGSSetWindowLevel, (unsigned int, unsigned int, int));
        NCGS(int, CGSOrderWindow, (unsigned int, unsigned int, int, unsigned int));
        if (!getenv("OAH_NATIVE_MENUBAR")) {
            g_menubar_wid = R[4];
            w = oah_cgs_win(R[4]);
            if (oah_mb_enabled()) {
                /* drawn into Mojave's own bar instead (oah_mb_render); the window itself stays off screen */
                p_CGSOrderWindow(g_ncid, R[4], 0, 0);
                if (w) { w->ordered = 1; oah_mb_render(w); }
                CGSLOG("[cgs] menu bar window %u registered: shown through Mojave's menu bar\n", R[4]);
            } else {
                p_CGSSetWindowLevel(g_ncid, R[4], 24);
                if (w) { w->level = 24; if (!w->flushes) oah_cgs_blit(w); w->ordered = 1; }
                p_CGSOrderWindow(g_ncid, R[4], 1, 0);
                CGSLOG("[cgs] menu bar window %u registered: level 24, ordered in\n", R[4]);
            }
        }
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSHideBackstopMenuBar") || !strcmp(n, "CGSShowBackstopMenuBar")) { R[3] = 0; return 1; }
    /* System UI mode: answer GetSystemUIMode from the last SetSystemUIMode instead of calling
       Mojave's HIToolbox, whose UI-mode lock is also taken on the activation hand-off thread
       (an AppleWorks menu click aborted in os_unfair_lock inside the native GetSystemUIMode).
       SetSystemUIMode still goes to Mojave (it hides/shows Mojave's menu bar and Dock). */
    {
        static int native = -1;
        if (native < 0) native = getenv("OAH_UIMODE_NATIVE") ? 1 : 0;
        if (!native && !strcmp(n, "GetSystemUIMode")) {
            if (R[3]) oah_put32(R[3], ui_mode);
            if (R[4]) oah_put32(R[4], ui_opts);
            return 1;
        }
        if (!strcmp(n, "SetSystemUIMode")) {
            ui_mode = R[3]; ui_opts = R[4];
            CGSLOG("[cgs] SetSystemUIMode(%u, 0x%x)\n", R[3], R[4]);
            oah_ls_set_uimode(R[3], R[4]);
            if (g_ui_mode != R[3]) {
                int was_hidden = g_ui_mode == 3 || g_ui_mode == 4;
                g_ui_mode = R[3]; g_ui_opts = R[4];
                if (oah_mb_enabled() && g_mb_published) {
                    /* Back from AllHidden / AllSuppressed (Keynote's slide show): the window server
                       has dropped what our bar image windows showed while the bar was hidden, and
                       Mojave fell back to a bare "Keynote  Help" bar after the show, even with the
                       bars republished. Draw them again (unchanged frames are otherwise skipped),
                       now and twice more while the switch settles. OAH_MB_NOREDRAW=1: publish only. */
                    if (was_hidden && R[3] != 3 && R[3] != 4 && !getenv("OAH_MB_NOREDRAW")) {
                        oah_cgswin_t *mw = oah_cgs_win(g_menubar_wid);
                        double t = CFAbsoluteTimeGetCurrent();
                        g_mb_force = 1;
                        if (mw) oah_mb_render(mw);
                        g_mb_force_at[0] = t + 0.5; g_mb_force_at[1] = t + 1.5; g_mb_force_at[2] = 0;
                        CGSLOG("[cgs] UI mode back to %u: menu bar redrawn\n", R[3]);
                    }
                    oah_mb_publish();   /* reveal or hide Mojave's bar now */
                }
            }
            return 0;
        }
    }
    if (!strcmp(n, "CGSSetMenuBarFilter")) {
        /* HIToolbox calls this on every activation change. Mojave shows the bars the front
           app last set, and an app brought back with Cmd-Tab / the Dock gets no activation
           event here, so hand our bars over again each time (AppKit does the same). */
        if (oah_mb_enabled() && g_mb_published && g_menubar_wid) {
            oah_cgswin_t *mw = oah_cgs_win(g_menubar_wid);
            if (mw) oah_mb_render(mw); else oah_mb_publish();
            oah_mb_schedule();          /* the switch may not have finished: set them again later */
            oah_mb_tick(1);
            CGSLOG("[cgs] CGSSetMenuBarFilter: menu bar republished\n");
        }
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSSetWindowLevelsForActivation")) {   /* (cid, wid, activeLevel, inactiveLevel) */
        /* 10.6's window server moved such windows (modal and floating panels) between the two
           levels when the app (de)activated; Mojave's does not, so a background app's dialog stayed
           above other apps' windows (Word's Project Gallery). oah_actlevel_apply() does it now. */
        NCGS(int, CGSSetWindowLevel, (unsigned int, unsigned int, int));
        int lv = (int)R[5];
        w = oah_cgs_win(R[4]);
        if (w) {
            w->actlev = !getenv("OAH_NO_ACTLEVEL"); w->lev_active = (int)R[5]; w->lev_inactive = (int)R[6];
            if (w->actlev && !g_app_active) lv = w->lev_inactive;
            w->level = (int)R[5];
        }
        CGSLOG("[cgs] levels for activation %u: active %d inactive %d -> %d\n", R[4], (int)R[5], (int)R[6], lv);
        if (w) oah_cgs_tags_note(w);
        R[3] = w && w->ghost ? 0 : p_CGSSetWindowLevel(g_ncid, R[4], lv);
        return 1;
    }
    if (!strcmp(n, "CGSInputModifierKeyState")) {     /* (source?, modifier index) -> Boolean down */
        /* HIToolbox GetCurrentKeyModifiers polls this per modifier; the stub said "up" for
           all of them, so keys were translated with stale modifiers. Index order from
           GetCurrentKeyModifiers: 0 caps lock, 1 shift, 2 control, 3 option, 4 command;
           5..7 (numeric pad, help, fn) mapped to the matching NX masks. */
        static const unsigned int mask[8] = { 0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000, 0x400000, 0x800000 };
        static unsigned long long (*fs)(int);
        if (!fs) fs = (unsigned long long (*)(int))dlsym(RTLD_DEFAULT, "CGEventSourceFlagsState");
        R[3] = (fs && R[4] < 8) ? ((fs(0 /* combined session state */) & mask[R[4]]) != 0) : 0;
        return 1;
    }
    if (!strcmp(n, "CGSGetSymbolicHotKeyValue")) {    /* (hotkey, UniChar *, UInt16 *keycode, UInt32 *mods) */
        /* HIToolbox's SyncWithKeyPrefs reads these on activation; the stub left them unset. */
        NCGS(int, CGSGetSymbolicHotKeyValue, (int, unsigned short *, unsigned short *, unsigned int *));
        unsigned short ch = 0xffff, kc = 0xffff;
        unsigned int mods = 0;
        int err = p_CGSGetSymbolicHotKeyValue ? p_CGSGetSymbolicHotKeyValue((int)R[3], &ch, &kc, &mods) : 1000;
        if (R[4]) *(unsigned short *)R[4] = OSSwapHostToBigInt16(ch);
        if (R[5]) *(unsigned short *)R[5] = OSSwapHostToBigInt16(kc);
        if (R[6]) oah_put32(R[6], mods);
        R[3] = (unsigned int)err;
        return 1;
    }
    if (!strcmp(n, "CGSIsSymbolicHotKeyEnabled")) {   /* (hotkey) -> bool */
        NCGS(int, CGSIsSymbolicHotKeyEnabled, (int));
        R[3] = p_CGSIsSymbolicHotKeyEnabled ? (unsigned int)(p_CGSIsSymbolicHotKeyEnabled((int)R[3]) != 0) : 0;
        return 1;
    }
    if (!strcmp(n, "CGSCopyCurrentSessionDictionary") || !strcmp(n, "CGSessionCopyCurrentDictionary")) {
        /* login session info (user name, uid, on-console...): native dictionary, rebuilt as guest CF */
        NCGS(CFDictionaryRef, CGSessionCopyCurrentDictionary, (void));
        CFDictionaryRef d = p_CGSessionCopyCurrentDictionary ? p_CGSessionCopyCurrentDictionary() : NULL;
        R[3] = d ? oah_cf_to_guest(d) : 0;
        if (d) CFRelease(d);
        return 1;
    }

    /* displays */
    if (!strcmp(n, "CGSGetActiveDisplayList") || !strcmp(n, "CGSGetOnlineDisplayList") ||
        !strcmp(n, "CGSGetDisplayList")) {           /* (max, ids *, count *) */
        NCGS(int, CGGetActiveDisplayList, (unsigned int, unsigned int *, unsigned int *));
        unsigned int ids[32], cnt = 0, i, mx = R[3] > 32 ? 32 : R[3];
        int err = p_CGGetActiveDisplayList(R[4] ? mx : 0, R[4] ? ids : 0, &cnt);
        for (i = 0; R[4] && i < cnt && i < mx; i++) oah_put32(R[4] + 4 * i, ids[i]);
        oah_put32(R[5], cnt);
        R[3] = err;
        return 1;
    }
    if (!strcmp(n, "CGSGetDisplayBounds")) {          /* (display, CGRect *) */
        NCGS(oah_rect_t, CGDisplayBounds, (unsigned int));
        NCGS(unsigned int, CGMainDisplayID, (void));
        /* display 0 is the main display to the 10.6 window server: AppKit's _NXGetScreenRect(0)
           (PScurrentmouse under the Colors panel's magnifier) got an empty rect from Mojave, the
           pointer was never on a screen and the loupe stayed black */
        oah_rect_t b = p_CGDisplayBounds(R[3] || !p_CGMainDisplayID ? R[3] : p_CGMainDisplayID());
        oah_putf(R[4], b.x); oah_putf(R[4] + 4, b.y); oah_putf(R[4] + 8, b.w); oah_putf(R[4] + 12, b.h);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSDisplayMappingChangedSeed")) { R[3] = 1; return 1; }
    if (!strcmp(n, "CGSCopyWindowColorSpace")) {     /* (cid, wid, CGColorSpaceRef *out): a new guest device RGB space */
        static unsigned int fcs;
        unsigned int a[1] = { 0 }, cs = 0;
        if (!fcs) fcs = oah_cgs_gsym("_CGColorSpaceCreateDeviceRGB");
        if (fcs) cs = oah_callppc_n("CGColorSpaceCreateDeviceRGB", fcs, 0, a);
        if (R[5]) oah_put32(R[5], cs);
        R[3] = cs ? 0 : 1000;
        return 1;
    }
    if (!strcmp(n, "CGSGetDisplayModeDescriptionOfLength")) {   /* (display, mode, desc *, length) */
        /* Mojave's own description (little-endian, the 10.6 "transport" layout) converted by the
           guest's CGSDisplayModeDescriptionTransportToHost.  Unbridged, the 200-byte struct was
           left uninitialised and Keynote spun forever on the garbage mode. */
        static int (*f)(unsigned int, unsigned int, void *, unsigned int);
        static unsigned int conv;
        unsigned char nb[0x200], hb[0x200];
        unsigned int len = R[6] > 0xc8 ? 0xc8 : R[6], a[2]; int err;
        if (!f) { f = (int (*)(unsigned int, unsigned int, void *, unsigned int))dlsym(RTLD_DEFAULT, "CGSGetDisplayModeDescriptionOfLength");
                  if (!f) f = (int (*)(unsigned int, unsigned int, void *, unsigned int))dlsym(RTLD_DEFAULT, "SLSGetDisplayModeDescriptionOfLength"); }
        if (!conv) conv = oah_cgs_gsym("_CGSDisplayModeDescriptionTransportToHost");
        memset(nb, 0, sizeof nb); memset(hb, 0, sizeof hb);
        err = f ? f(R[3], R[4], nb, 0xc8) : 1000;
        if (!err && conv && R[5]) {
            a[0] = (unsigned int)hb; a[1] = (unsigned int)nb;
            oah_callppc_n("CGSDisplayModeDescriptionTransportToHost", conv, 2, a);
            memcpy((void *)R[5], hb, len);
        }
        CGSLOG("[cgs] CGSGetDisplayModeDescriptionOfLength(%x, %u, len %u) -> %d (%ux%u)\n", R[3], R[4], R[6], err,
               *(unsigned int *)(nb + 8), *(unsigned int *)(nb + 12));
        R[3] = (unsigned int)err;
        return 1;
    }
    if (!strcmp(n, "CGSIsZoomed")) {                 /* (cid, Boolean *out): Universal Access zoom off */
        if (R[4]) *(unsigned char *)R[4] = 0;
        R[3] = 0; return 1;
    }
    if (!strcmp(n, "CGSGetNumberOfDisplays")) {      /* () -> count; +[NSScreen screens] gives up on 0 */
        NCGS(int, CGGetActiveDisplayList, (unsigned int, unsigned int *, unsigned int *));
        unsigned int cnt = 0;
        if (p_CGGetActiveDisplayList) p_CGGetActiveDisplayList(0, 0, &cnt);
        R[3] = cnt;
        return 1;
    }
    if (!strcmp(n, "CGSGetCurrentDisplayMode")) {     /* (display, int *): Mojave's mode number (1 was 720x450) */
        static int (*f)(unsigned int, int *);
        int m = 1;
        if (!f) { f = (int (*)(unsigned int, int *))dlsym(RTLD_DEFAULT, "CGSGetCurrentDisplayMode");
                  if (!f) f = (int (*)(unsigned int, int *))dlsym(RTLD_DEFAULT, "SLSGetCurrentDisplayMode"); }
        if (!f || f(R[3], &m)) m = 1;
        oah_put32(R[4], (unsigned int)m); R[3] = 0; return 1;
    }
    if (!strcmp(n, "CGSGetDisplayDepth") || !strcmp(n, "CGSGetWindowDepth")) {
        /* (display, int *fmt) / (cid, wid, int *fmt): QD's format index, 4 = 32 bpp.
           OAH_GD_DEPTH=16 reports the display (not windows) as 16 bpp (index 3). */
        static int gdd = -1;
        if (gdd < 0) { const char *v = getenv("OAH_GD_DEPTH"); gdd = v ? atoi(v) : 0; }
        oah_put32(n[6] == 'W' ? R[5] : R[4], (n[6] != 'W' && gdd == 16) ? 3 : 4);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSGetDisplayBaseAddress")) {
        /* No framebuffer on Mojave: QD gets a private desktop-sized buffer, so
           drawing straight to the screen port is harmless (and invisible). */
        static unsigned char *fb;
        oah_cgs_cid();
        if (!fb) fb = (unsigned char *)mmap(0, (size_t)g_cgs_rows * g_cgs_rb, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        oah_put32(R[4], (unsigned int)fb);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSGetDisplayRowBytes")) { oah_cgs_cid(); oah_put32(R[4], g_cgs_rb); R[3] = 0; return 1; }
    if (!strcmp(n, "CGGetActiveDisplayList") || !strcmp(n, "CGGetOnlineDisplayList")) {
        NCGS(int, CGGetActiveDisplayList, (unsigned int, unsigned int *, unsigned int *));
        unsigned int ids[32], cnt = 0, i, mx = R[3] > 32 ? 32 : R[3];
        int err = p_CGGetActiveDisplayList(R[4] ? mx : 0, R[4] ? ids : 0, &cnt);
        for (i = 0; R[4] && i < cnt && i < mx; i++) oah_put32(R[4] + 4 * i, ids[i]);
        oah_put32(R[5], cnt);
        R[3] = err;
        return 1;
    }
    if (!strcmp(n, "CGGetDisplaysWithRect") || !strcmp(n, "CGGetDisplaysWithPoint")) {
        /* (CGRect by value in r3-r6 | CGPoint in r3-r4, max, ids *, count *) */
        NCGS(int, CGGetDisplaysWithRect, (oah_rect_t, unsigned int, unsigned int *, unsigned int *));
        NCGS(int, CGGetDisplaysWithPoint, (oah_pt_t, unsigned int, unsigned int *, unsigned int *));
        unsigned int ids[32], cnt = 0, i, k = strstr(n, "Rect") ? 7 : 5, mx = R[k] > 32 ? 32 : R[k];
        int err;
        if (k == 7) {
            oah_rect_t r;
            memcpy(&r.x, &R[3], 4); memcpy(&r.y, &R[4], 4); memcpy(&r.w, &R[5], 4); memcpy(&r.h, &R[6], 4);
            err = p_CGGetDisplaysWithRect(r, R[k + 1] ? mx : 0, R[k + 1] ? ids : 0, &cnt);
        } else {
            oah_pt_t p;
            memcpy(&p.x, &R[3], 4); memcpy(&p.y, &R[4], 4);
            err = p_CGGetDisplaysWithPoint(p, R[k + 1] ? mx : 0, R[k + 1] ? ids : 0, &cnt);
        }
        for (i = 0; R[k + 1] && i < cnt && i < mx; i++) oah_put32(R[k + 1] + 4 * i, ids[i]);
        oah_put32(R[k + 2], cnt);
        R[3] = err;
        return 1;
    }
    if (!strcmp(n, "CGSGetDisplayPixelEncodingOfLength")) {   /* (display, char *buf, len): IOKit pixel encoding */
        static const char enc[] = "--------RRRRRRRRGGGGGGGGBBBBBBBB";   /* 32-bit, what Mojave displays are */
        if (R[4] && R[5]) { size_t l = R[5] > sizeof enc ? sizeof enc : R[5]; memcpy((void *)(uintptr_t)R[4], enc, l); ((char *)(uintptr_t)R[4])[l - 1] = 0; }
        R[3] = R[4] && R[5] ? 0 : 1001;
        return 1;
    }
    if (!strcmp(n, "CGSGetDisplaysWithRect") || !strcmp(n, "CGSGetDisplaysWithPoint")) {
        /* (const CGRect * | const CGPoint *, max, ids *, count *): AppKit's _captureRegion (the Colors
           panel's magnifier) asks which displays a rect is on and captured none when unanswered */
        NCGS(int, CGGetDisplaysWithRect, (oah_rect_t, unsigned int, unsigned int *, unsigned int *));
        NCGS(int, CGGetDisplaysWithPoint, (oah_pt_t, unsigned int, unsigned int *, unsigned int *));
        unsigned int ids[32], cnt = 0, i, mx = R[4] > 32 ? 32 : R[4];
        int err = 1000;
        if (R[3] && n[18] == 'R' && p_CGGetDisplaysWithRect) {
            oah_rect_t r = { oah_getf(R[3]), oah_getf(R[3] + 4), oah_getf(R[3] + 8), oah_getf(R[3] + 12) };
            err = p_CGGetDisplaysWithRect(r, R[5] ? mx : 0, R[5] ? ids : 0, &cnt);
        } else if (R[3] && n[18] == 'P' && p_CGGetDisplaysWithPoint) {
            oah_pt_t p = { oah_getf(R[3]), oah_getf(R[3] + 4) };
            err = p_CGGetDisplaysWithPoint(p, R[5] ? mx : 0, R[5] ? ids : 0, &cnt);
        }
        for (i = 0; R[5] && i < cnt && i < mx; i++) oah_put32(R[5] + 4 * i, ids[i]);
        if (R[6]) oah_put32(R[6], cnt);
        CGSLOG("[cgs] %s -> %d, %u display(s)\n", n, err, cnt);
        R[3] = (unsigned int)err;
        return 1;
    }
    if (!strcmp(n, "CGSGetDockRectWithReason")) {   /* (cid, CGRect *, int *reason) */
        NCGS(int, CGSGetDockRectWithReason, (unsigned int, oah_rect_t *, int *));
        oah_rect_t r;
        int why = 0, err = 1;
        memset(&r, 0, sizeof r);
        if (p_CGSGetDockRectWithReason) err = p_CGSGetDockRectWithReason(g_ncid, &r, &why);
        oah_putf(R[4], r.x); oah_putf(R[4] + 4, r.y); oah_putf(R[4] + 8, r.w); oah_putf(R[4] + 12, r.h);
        oah_put32(R[5], (unsigned int)why);
        CGSLOG("[cgs] DockRect %.0f,%.0f %.0fx%.0f reason %d err %d\n", r.x, r.y, r.w, r.h, why, err);
        R[3] = 0;
        return 1;
    }
    /* Notification callbacks: accepted, not delivered yet. */
    if (!strcmp(n, "CGSRegisterNotifyProc") || !strcmp(n, "CGSRegisterConnectionNotifyProc") ||
        !strcmp(n, "CGSRemoveNotifyProc") || !strcmp(n, "CGSRemoveConnectionNotifyProc")) {
        CGSLOG("[cgs] %s proc %08x type %u (accepted, not delivered)\n", n, n[11] == 'C' ? R[4] : R[3], n[11] == 'C' ? R[5] : R[4]);
        R[3] = 0;
        return 1;
    }
    /* Property getters: (cid, target cid / wid, CFStringRef key, CFTypeRef *out).  No guest-visible
       properties exist, so report "not set": NULL out and an error, which callers treat as default.
       (Left untouched, the out slot held stack garbage that AppKit passed to CFBooleanGetValue.) */
    /* Connection properties the guest sets on its own connection are kept here (guest objects,
       retained). CoreDrag publishes its drag port this way and the drag source reads it back
       from the connection that owns the window under the pointer: without it a drag inside the
       app (moving selected text in TextWrangler) found no destination and was dropped nowhere. */
    if (!strcmp(n, "CGSGetConnectionProperty") || !strcmp(n, "CGSCopyConnectionProperty") ||
        !strcmp(n, "CGSSetConnectionProperty")) {
        static struct { unsigned int cid; char key[96]; unsigned int val; } prop[32];
        static unsigned int fstr, fret, frel;
        char key[96];
        unsigned int a[4];
        int i, set = n[3] == 'S';
        if (!fstr) { fstr = oah_guest_sym("CoreFoundation", "_CFStringGetCString");
                     fret = oah_guest_sym("CoreFoundation", "_CFRetain"); frel = oah_guest_sym("CoreFoundation", "_CFRelease"); }
        key[0] = 0;
        if (fstr && R[5]) { a[0] = R[5]; a[1] = (unsigned int)(uintptr_t)key; a[2] = sizeof key; a[3] = 0x08000100u;
                            if (!(oah_callppc_n("oah_cgs_prop", fstr, 4, a) & 0xff)) key[0] = 0; }
        for (i = 0; i < 32; i++) if (prop[i].key[0] && prop[i].cid == R[4] && !strcmp(prop[i].key, key)) break;
        if (getenv("OAH_CGSTRACE")) fprintf(stderr, "[cgs] %s(%x, \"%s\", %08x)%s\n", n, R[4], key, R[6], i < 32 ? " (kept)" : "");
        if (!key[0] || !fret || !frel) { if (!set && R[6]) oah_put32(R[6], 0); R[3] = set ? 0 : 1000; return 1; }
        if (set) {
            if (i == 32) for (i = 0; i < 32 && prop[i].key[0]; i++) ;
            if (i < 32) {
                unsigned int old = prop[i].key[0] ? prop[i].val : 0;
                if (R[6]) { a[0] = R[6]; oah_callppc_n("oah_cgs_prop", fret, 1, a); }
                if (old) { a[0] = old; oah_callppc_n("oah_cgs_prop", frel, 1, a); }
                prop[i].cid = R[4]; strcpy(prop[i].key, key); prop[i].val = R[6];
                if (!R[6]) prop[i].key[0] = 0;
            }
            R[3] = 0;
            return 1;
        }
        if (i < 32 && prop[i].val) {
            if (n[3] == 'C') { a[0] = prop[i].val; oah_callppc_n("oah_cgs_prop", fret, 1, a); }   /* Copy: caller owns one */
            if (R[6]) oah_put32(R[6], prop[i].val);
            R[3] = 0;
            return 1;
        }
        if (R[6]) oah_put32(R[6], 0);
        R[3] = 1000;
        return 1;
    }
    if (!strcmp(n, "CGSGetWindowProperty") || !strcmp(n, "CGSCopyWindowProperty")) {
        if (R[6]) oah_put32(R[6], 0);
        R[3] = 1000;
        return 1;
    }
    /* Live input state, which 10.6 reads from the WindowServer scoreboard page (not mapped
       for a translated client): button/mouse tracking (StillDown, WaitMouseUp, link and
       drag tracking) and the pointer position.  The injector's state wins for 10 s after
       it last acted, since its events never reach the real HID state.
         Boolean CGSInputButtonState(int device (1 = mouse), int button)
         CGError CGSCurrentInputPointerPosition(CGPoint *) */
    if (!strcmp(n, "CGSInputButtonState")) {
        NCGS(int, CGEventSourceButtonState, (int, unsigned int));
        unsigned int b = R[4];
        if (R[3] != 1 || b > 31) R[3] = 0;
        else if (oah_inj_drives()) R[3] = (g_inj_btn >> b) & 1;
        else R[3] = p_CGEventSourceButtonState ? (p_CGEventSourceButtonState(0, b) ? 1 : 0) : 0;   /* combined session state */
        return 1;
    }
    if (!strcmp(n, "CGSCurrentInputPointerPosition")) {
        NCGS(void *, CGEventCreate, (void *));
        NCGS(oah_pt_t, CGEventGetLocation, (void *));
        oah_pt_t p = { 0, 0 };
        if (oah_inj_drives()) { p.x = g_inj_x; p.y = g_inj_y; }
        else if (p_CGEventCreate && p_CGEventGetLocation) {
            void *e = p_CGEventCreate(0);
            if (e) { p = p_CGEventGetLocation(e); oah_cgs_nrelease(e); }
        }
        if (R[3]) { union { float f; unsigned int u; } a, b2; a.f = p.x; b2.f = p.y; oah_put32(R[3], a.u); oah_put32(R[3] + 4, b2.u); }
        R[3] = 0;
        return 1;
    }
    /* SCNetworkReachability (SystemConfiguration, not window server, but routed here by name) */
    if (!strcmp(n, "SCNetworkConnectionCopyUserPreferences")) { R[3] = 0; return 1; }   /* no on-demand VPN */
    if (!strncmp(n, "SCNetworkReachability", 21)) {
        if (!strcmp(n, "SCNetworkReachabilityGetFlags") && R[4]) oah_put32(R[4], 2);   /* kSCNetworkReachabilityFlagsReachable */
        R[3] = 1;                                                                        /* Boolean true */
        return 1;
    }
    /* Sheet animations (HIToolbox's ShowSheetWindow): the 10.6 window server slid the sheet out of its
       parent and the sheet got an exposure as it became visible. The animation is not made here (the
       call answers success, as the unbridged stub did) and the sheet is on screen at once, so the app
       is sent that exposure now: Adobe Help Center's Opera "Certificate signer not found" sheet drew its
       contents only on such an update and stayed blank. Only a sheet that sent no pixels within 300 ms
       gets it: an exposure made Word 2004 erase its alert sheet without redrawing the buttons (the
       exposure path's visible-region problem, see oah_cgs_expose). OAH_NO_SHEETEXPOSE=1 turns this off. */
    if (!strcmp(n, "CGSCreateMetalSheetWindowAnimationWithParent") || !strcmp(n, "CGSCreateSheetWindowAnimation") ||
        !strcmp(n, "CGSCreateMetalSheetWindowAnimationWithParentAndShift") || !strcmp(n, "CGSCreateSheetWindowAnimationWithParent")) {
        oah_cgswin_t *sw = oah_cgs_win(R[4]);
        if (sw && !getenv("OAH_NO_SHEETEXPOSE")) { sw->shex_rows = 0; sw->shex_t = CFAbsoluteTimeGetCurrent() + 0.3; g_shex_n++; }
        CGSLOG("[cgs] %s(%u) -> 0 (no animation; exposure in 300 ms if nothing is drawn)\n", n, R[4]);
        R[3] = 0;
        return 1;
    }
    /* NSDisableScreenUpdates / NSEnableScreenUpdates: the screen keeps the app's windows as they
       are until updates are enabled again, so a window drawn in several steps (a sheet put up,
       moved and ordered; a button erased and redrawn) appears at once. These were unbridged
       no-ops: each step reached the screen on its own. Passed to Mojave's window server for our
       connection (it ends a disable that lasts more than about a second by itself).
       OAH_NO_DISABLEUPDATE=1 restores the no-op. */
    if (!strcmp(n, "CGSDisableUpdate") || !strcmp(n, "CGSReenableUpdate")) {   /* (cid) */
        static int off = -1; static volatile int depth;
        static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
        NCGS(int, CGSDisableUpdate, (unsigned int));
        NCGS(int, CGSReenableUpdate, (unsigned int));
        if (off < 0) off = getenv("OAH_NO_DISABLEUPDATE") ? 1 : 0;
        R[3] = 0;
        if (off || !p_CGSDisableUpdate || !p_CGSReenableUpdate) return 1;
        pthread_mutex_lock(&mu);
        if (n[3] == 'D') { depth++; p_CGSDisableUpdate(g_ncid); }
        else if (depth > 0) { depth--; p_CGSReenableUpdate(g_ncid); }   /* never more enables than disables */
        pthread_mutex_unlock(&mu);
        return 1;
    }
    /* Window lists for this app, answered from the bridge's own table (all of the guest's windows
       are ours), newest first as an approximation of front-to-back order:
         CGSGetWindowCount / CGSGetOnScreenWindowCount / CGSGetWorkspaceWindowCount (cid, target or ws, int *n)
         CGSGetWindowList / CGSGetOnScreenWindowList (cid, target, int max, int *list, int *n)
         CGSGetWorkspaceWindowList (cid, ws, int max, int *list, int *n)
         CGSGetWorkspaceWindowCountWithOptionsAndTags (cid, ws, options, 0x22, tags, tags, 0x20, int *n)
         CGSGetWorkspaceWindowListWithOptionsAndTags (cid, ws, options, int max, int *list, int *n, ...)
       The last two (+[NSWindow windowNumbersWithOptions:], from AppKit's activation handler) were
       unbridged: the count stayed uninitialised and Excel / Word crashed on reactivation. */
    {
        int cnt = !strcmp(n, "CGSGetWindowCount") || !strcmp(n, "CGSGetOnScreenWindowCount") ||
                  !strcmp(n, "CGSGetWorkspaceWindowCount");
        int cntws = !strcmp(n, "CGSGetWorkspaceWindowCountWithOptionsAndTags");
        int lstws = !strcmp(n, "CGSGetWorkspaceWindowListWithOptionsAndTags");
        int lst = !strcmp(n, "CGSGetWindowList") || !strcmp(n, "CGSGetOnScreenWindowList") ||
                  !strcmp(n, "CGSGetWorkspaceWindowList");
        if (cnt || cntws || lst || lstws) {
            int onscreen = strstr(n, "OnScreen") != 0, i, k = 0, j;
            int idx[OAH_MAXWIN];
            for (i = 0; i < OAH_MAXWIN; i++)
                if (g_cgswin[i].wid && (!onscreen || g_cgswin[i].ordered)) idx[k++] = i;
            for (i = 1; i < k; i++)                       /* newest first */
                for (j = i; j > 0 && g_cgswin[idx[j]].seq > g_cgswin[idx[j - 1]].seq; j--) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
            if ((lst || lstws) && k > 1 && !getenv("OAH_NO_ZORDER")) {
                /* Real front-to-back order from the window server for the windows on screen
                   (newest-first said a newer background window was in front, so AppKit skipped
                   ordering it front: Dock menu window choice, Window menu). Off-screen ones keep
                   their newest-first order after them. */
                NCGS(CFArrayRef, CGWindowListCreate, (unsigned int, unsigned int));
                CFArrayRef l = p_CGWindowListCreate ? p_CGWindowListCreate(1 /* on screen only */, 0) : 0;
                if (l) {
                    int out[OAH_MAXWIN], no = 0, used[OAH_MAXWIN];
                    CFIndex q, c = CFArrayGetCount(l);
                    memset(used, 0, sizeof used);
                    for (q = 0; q < c && no < k; q++) {
                        unsigned int wid = (unsigned int)(uintptr_t)CFArrayGetValueAtIndex(l, q);
                        for (j = 0; j < k; j++) if (!used[j] && g_cgswin[idx[j]].wid == wid) { used[j] = 1; out[no++] = idx[j]; break; }
                    }
                    for (j = 0; j < k; j++) if (!used[j]) out[no++] = idx[j];
                    memcpy(idx, out, sizeof(int) * k);
                    CFRelease(l);
                }
            }
            if (cnt) { if (R[5]) oah_put32(R[5], (unsigned int)k); }
            else if (cntws) { if (R[10]) oah_put32(R[10], (unsigned int)k); }
            else if (lstws) {
                int max = (int)R[6], m = k < max ? k : max;
                for (i = 0; i < m; i++) oah_put32(R[7] + 4 * i, g_cgswin[idx[i]].wid);
                if (R[8]) oah_put32(R[8], (unsigned int)(m > 0 ? m : 0));
            }
            else {
                int max = (int)R[5], m = k < max ? k : max;
                for (i = 0; i < m; i++) oah_put32(R[6] + 4 * i, g_cgswin[idx[i]].wid);
                if (R[7]) oah_put32(R[7], (unsigned int)(m > 0 ? m : 0));
            }
            R[3] = 0;
            return 1;
        }
    }
    /* Window attributes Mojave has no direct match for (or that need
       translating later): accepted silently. */
    {
        static const char *ok[] = {
            "CGSSetWindowShadowAndRimParametersWithStretch", "CGSSetWindowShadowAndRimParameters",
            "CGSSetDeferActivation", "CGSSetDeferOrdering", "CGSSetWindowProperty",
            "CGSSetWindowToReleaseBackingOnOrderOut", "CGSSetIgnoreAsFrontWindow",
            "CGSSetWindowTags", "CGSClearWindowTags",
            "CGSAddActivationRegion", "CGSClearActivationRegion", "CGSRemoveActivationSubregion",
            "CGSAddButtonRegion", "CGSAddButtonRegionInWindow", "CGSClearButtonRegion", "CGSClearButtonRegionInWindow",
            "CGSAddDragRegionInWindow", "CGSClearDragRegionInWindow", "CGSRemoveDragSubregion",
            "CGSRemoveDragSubregionInWindow", "CGSLastCompletedActivation", "CGSWindowDirtyRegionSeed", 0 };
        int i;
        if (!strcmp(n, "CGSSetWindowTags") || !strcmp(n, "CGSClearWindowTags"))
            CGSLOG("[cgs] %s(wid=%u, %08x %08x, %u)\n", n, R[4], R[5] ? oah_get32(R[5]) : 0,
                   R[5] ? oah_get32(R[5] + 4) : 0, R[6]);
        /* Help tags let clicks through. A data browser shows a help tag with the full text right
           over a truncated cell as soon as the pointer is on it (Entourage's message list), and on
           10.6 a click there still reached the list: FindWindow (CGSFindWindowAndOwner) passed over
           the tag. Mojave hit-tests it, so the click went to the tag and a double-click never
           opened the message. A help tag here: popup level (101) with tag 0x2 (floating
           panels have that too, at their own level); menus, at 101 as well, clear 0x2 and carry
           0x04000000. Such windows are left out of hit-testing and events over them go to the
           window below. OAH_NO_PASSTHRU=1 disables. */
        if ((!strcmp(n, "CGSSetWindowTags") || !strcmp(n, "CGSClearWindowTags")) && R[5]) {
            oah_cgswin_t *tw = oah_cgs_win(R[4]);
            if (tw) {
                if (n[3] == 'S') tw->tags |= oah_get32(R[5]); else tw->tags &= ~oah_get32(R[5]);
                oah_cgs_tags_note(tw);
            }
        }
        if ((!strcmp(n, "CGSSetWindowTags") || !strcmp(n, "CGSClearWindowTags")) && R[5] && getenv("OAH_TAGFWD")) {
            unsigned int t[2], mask = (unsigned int)strtoul(getenv("OAH_TAGFWD"), 0, 0);
            NCGS(int, CGSSetWindowTags, (unsigned int, unsigned int, unsigned int *, int));
            NCGS(int, CGSClearWindowTags, (unsigned int, unsigned int, unsigned int *, int));
            t[0] = oah_get32(R[5]) & mask; t[1] = 0;
            if (t[0]) {
                int rc = n[3] == 'S' ? p_CGSSetWindowTags(g_ncid, R[4], t, 64) : p_CGSClearWindowTags(g_ncid, R[4], t, 64);
                fprintf(stderr, "[cgs] native %s(%u, %x) -> %d\n", n, R[4], t[0], rc);
            }
        }
        for (i = 0; ok[i]; i++) if (!strcmp(n, ok[i])) { R[3] = 0; return 1; }
    }

    /* windows */
    if (!strcmp(n, "CGSNewWindowWithOpaqueShape")) {
        /* (cid, type, float x, float y, shape, opaqueShape, ?, tags *, tagSize, CGSWindowID *) */
        CGSLOG("[cgs] NewWindowWithOpaqueShape args r8 %08x r9 %08x r10 %08x (%08x %08x) s9 %08x\n", R[8], R[9], R[10],
               R[10] > 0x1000 ? oah_get32(R[10]) : 0, R[10] > 0x1000 ? oah_get32(R[10] + 4) : 0, oah_stackarg(R, 9));
        R[3] = oah_cgs_new_window((int)R[4], (float)F[1], (float)F[2], R[7], oah_stackarg(R, 10));
        return 1;
    }
    if (!strcmp(n, "CGSNewWindow")) {                /* (cid, type, float x, float y, shape, wid *) */
        R[3] = oah_cgs_new_window((int)R[4], (float)F[1], (float)F[2], R[7], R[8]);
        return 1;
    }
    if (!strcmp(n, "CGSReleaseWindow")) {
        NCGS(int, CGSReleaseWindow, (unsigned int, unsigned int));
        NCGS(void, CGContextRelease, (void *));
        oah_blit_lock();
        w = oah_cgs_win(R[4]);
        if (w) {
            if (w->hctx || w->hsid) oah_hd_release(w);
            if (w->nctx) p_CGContextRelease(w->nctx);
            munmap(w->store, (size_t)WROWS(w) * WRB(w));
            if (w->qd) { munmap(w->qd, (size_t)g_cgs_rows * g_cgs_rb); munmap(w->qdsh, (size_t)g_cgs_rows * g_cgs_rb); }
            free(w->shadow);
            memset(w, 0, sizeof *w);
        }
        oah_blit_unlock();
        R[3] = p_CGSReleaseWindow(g_ncid, R[4]);
        oah_mb_menu_closed();
        CGSLOG("[cgs] ReleaseWindow %u -> %d\n", R[4], R[3]);
        return 1;
    }
    if (!strcmp(n, "CGSAddWindowToWindowOrderingGroup") || !strcmp(n, "CGSRemoveFromOrderingGroup") ||
        !strcmp(n, "CGSAddWindowToWindowMovementGroup") || !strcmp(n, "CGSRemoveWindowFromWindowMovementGroup")) {
        /* (cid, wid[, childWid, ...]) with plain integer arguments: the window server still has
           these, so child windows (sheets, drawers, popups) order and move with their parent */
        int (*fn)(unsigned int, unsigned int, unsigned int, unsigned int) =
            (int (*)(unsigned int, unsigned int, unsigned int, unsigned int))dlsym(RTLD_DEFAULT, n);
        if (strstr(n, "OrderingGroup") && getenv("OAH_NO_NATIVE_OGROUP")) fn = 0;
        R[3] = fn ? (unsigned int)fn(g_ncid, R[4], R[5], R[6]) : 0;
        /* The 10.6 window server ordered a child in as soon as it joined a visible parent's
           ordering group (-[NSWindow addChildWindow:ordered:] sends nothing else): do that here,
           and remember the pair so the child follows the parent in and out. */
        if (!strcmp(n, "CGSAddWindowToWindowOrderingGroup")) {
            oah_cgs_group_add(R[4], R[5], (int)R[6]);
            /* A child already on screen is placed next to its parent too: Excel 2004 takes its
               formula entry field out of the formula bar's group, orders it below the toolbar
               window, and adds it back "above" the bar (each time a modal dialog ends); left where
               it was, the field stayed hidden behind the bar. OAH_NO_GROUPREORDER=1: old way. */
            {   oah_cgswin_t *pw = oah_cgs_win(R[4]), *cw = oah_cgs_win(R[5]);
                static int noreorder = -1;
                if (noreorder < 0) noreorder = getenv("OAH_NO_GROUPREORDER") ? 1 : 0;
                if (pw && pw->ordered && cw && (!cw->ordered || (!noreorder && R[6]))) oah_cgs_order_child(cw, R[4], (int)R[6]);
            }
        } else if (!strcmp(n, "CGSRemoveFromOrderingGroup")) oah_cgs_group_del(R[4]);
        else if (!strcmp(n, "CGSAddWindowToWindowMovementGroup")) oah_cgs_mgroup_set(R[4], R[5], 1);
        else if (!strcmp(n, "CGSRemoveWindowFromWindowMovementGroup")) oah_cgs_mgroup_set(R[4], R[5], 0);
        CGSLOG("[cgs] %s(%u, %u, %u) -> %d%s\n", n, R[4], R[5], R[6], (int)R[3], fn ? "" : " (not in this window server)");
        return 1;
    }
    if (!strcmp(n, "CGSOrderWindow") || !strcmp(n, "CGSOrderWindowWithGroup") ||
        !strcmp(n, "CGSOrderPanelIfActive")) {   /* (cid, wid, mode, relativeTo) */
        /* CGSOrderPanelIfActive: AppKit orders hidesOnDeactivate floating panels (Keynote's
           Inspector) with it; the unbridged guest version talked to no server, so the panel
           was "visible" to AppKit but never on screen. The app is active whenever it gets here.
           Its arguments are (cid, mode, relativeTo, wid); the 10.6 CG then calls
           CGSOrderWindowWithGroup(cid, wid, mode, relativeTo) if this process is frontmost. */
        if (!strcmp(n, "CGSOrderPanelIfActive")) {
            unsigned int m = R[4], rel = R[5];
            R[4] = R[6]; R[5] = m; R[6] = rel;
        }
        /* AppKit orders windows that have child windows or ordering groups (Keynote's
           "Warnings" panel) with ...WithGroup; as a no-op stub the window stayed hidden but
           key, and every menu item that needs the document window was disabled. */
        NCGS(int, CGSOrderWindow, (unsigned int, unsigned int, int, unsigned int));
        w = oah_cgs_win(R[4]);
        if (R[4] == g_menubar_wid && ((int)R[5] == 0 || oah_mb_enabled())) {   /* keep the menu bar */
            if (w) w->ordered = 1;
            R[3] = 0;
            return 1;
        }
        if (w && (w->hod_hidden || g_app_hidden) && (int)R[5] != 0) {   /* inactive / hidden: stays out for now */
            if (g_app_hidden) w->app_hidden = 1;
            w->ordered = 1;
            R[3] = 0; return 1;
        }
        if (oah_cgs_ghost_order(w, (int)R[5])) {
            oah_cgs_group_follow(R[4], 0); oah_mb_menu_closed();
            CGSLOG("[cgs] OrderWindow %u mode 0 -> ghosted (GL surface)\n", R[4]);
            R[3] = 0; return 1;
        }
        if (w && (int)R[5] != 0 && !w->flushes) oah_cgs_blit(w);
        R[3] = p_CGSOrderWindow(g_ncid, R[4], (int)R[5], R[6]);
        if (w && (int)R[5] != 0 && !w->ordered) { oah_cgs_expose(w); oah_glsurf_shown(R[4]); }
        if (w) w->ordered = (int)R[5] != 0;
        if (w) oah_cgs_group_follow(R[4], (int)R[5]);
        if ((int)R[5] == 0) oah_mb_menu_closed();
        CGSLOG("[cgs] OrderWindow %u mode %d rel %u -> %d\n", R[4], (int)R[5], R[6], R[3]);
        return 1;
    }
    if (!strcmp(n, "CGSSetFrontWindow") || !strcmp(n, "CGSOrderFrontConditionally")) {  /* (cid, wid, ...) */
        /* 10.6 HIToolbox shows a window of an app WindowServer has not activated
           by making it the front window; no CGSOrderWindow follows. Order it in. */
        NCGS(int, CGSOrderWindow, (unsigned int, unsigned int, int, unsigned int));
        w = oah_cgs_win(R[4]);
        if (w && !w->ordered && g_app_hidden && !getenv("OAH_NO_HIDDENFRONT")) {
            /* the app is hidden (Cmd-H): HIToolbox orders its windows out and then makes one the
               front window (Kid Pix's full-screen window came straight back on screen); keep it
               out until the app is shown again */
            w->ordered = 1; w->app_hidden = 1;
            CGSLOG("[cgs] %s %u while hidden -> stays out until shown\n", n, R[4]);
        } else if (w && !w->ordered) {
            oah_cgs_ghost_order(w, 1);
            if (!w->flushes) oah_cgs_blit(w);
            p_CGSOrderWindow(g_ncid, R[4], 1, 0);
            w->ordered = 1;
            oah_cgs_expose(w);
            oah_glsurf_shown(R[4]);
            CGSLOG("[cgs] %s %u -> ordered in\n", n, R[4]);
        }
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSOrderWindowList") || !strcmp(n, "CGSOrderWindowListWithGroups")) {   /* (cid, wid[], mode[], rel[], count) */
        NCGS(int, CGSOrderWindow, (unsigned int, unsigned int, int, unsigned int));
        unsigned int i, cnt = R[7], lw[256];
        for (i = 0; i < cnt && i < 256; i++) lw[i] = oah_get32(R[4] + 4 * i);
        g_cgs_follow_skip = lw; g_cgs_follow_nskip = i;
        for (i = 0; i < cnt && i < 256; i++) {
            unsigned int wid = oah_get32(R[4] + 4 * i), rel = R[6] ? oah_get32(R[6] + 4 * i) : 0;
            int mode = (int)oah_get32(R[5] + 4 * i);
            if (wid == g_menubar_wid && (mode == 0 || oah_mb_enabled())) continue;   /* keep the menu bar */
            w = oah_cgs_win(wid);
            if (oah_cgs_ghost_order(w, mode)) { oah_cgs_group_follow(wid, 0); oah_mb_menu_closed(); continue; }
            if (w && mode && !w->flushes) oah_cgs_blit(w);
            p_CGSOrderWindow(g_ncid, wid, mode, rel);
            if (w && mode && !w->ordered) { oah_cgs_expose(w); oah_glsurf_shown(wid); }
            if (w) w->ordered = mode != 0;
            if (w) oah_cgs_group_follow(wid, mode);
            if (!mode) oah_mb_menu_closed();
            CGSLOG("[cgs] OrderWindowList %u mode %d rel %u\n", wid, mode, rel);
        }
        g_cgs_follow_nskip = 0;
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGDisplayIOServicePort")) {      /* (display) -> io_service_t, a port in our own task */
        /* CoreVideo's CVDisplayLinkCreateWithCGDisplay failed (-6670) on the stub's 0, so Keynote's
           transition previews never animated. Mojave's (deprecated) call still answers. */
        NCGS(unsigned int, CGDisplayIOServicePort, (unsigned int));
        R[3] = p_CGDisplayIOServicePort ? p_CGDisplayIOServicePort(R[3]) : 0;
        { static int nlog; if (nlog++ < 3) fprintf(stderr, "[cgs] CGDisplayIOServicePort -> %#x\n", R[3]); }
        return 1;
    }
    if (!strcmp(n, "CGSWindowIsOrderedIn")) {        /* (cid, wid, Boolean *) */
        w = oah_cgs_win(R[4]);
        if (R[5]) *(unsigned char *)R[5] = w ? (unsigned char)w->ordered : 0;
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSMoveWindow")) {               /* (cid, wid, CGPoint *) */
        NCGS(int, CGSMoveWindow, (unsigned int, unsigned int, const oah_pt_t *));
        oah_pt_t p;
        p.x = oah_getf(R[5]); p.y = oah_getf(R[5] + 4);
        w = oah_cgs_win(R[4]);
        if (w) { w->x = p.x; w->y = p.y; }
        R[3] = p_CGSMoveWindow(g_ncid, R[4], &p);
        CGSLOG("[cgs] MoveWindow %u to %.0f,%.0f -> %d\n", R[4], p.x, p.y, R[3]);
        return 1;
    }
    if (!strcmp(n, "CGSWindowScrollRegion") && !getenv("OAH_NO_SCROLLREGION")) {
        /* (cid, wid, CGSRegionRef area, CGSRegionRef dirty, int dx, int dy): copy the pixels of
           the window's backing in "area" to area + (dx,dy). HIToolbox (ScrollWindowRect / scroll
           views) and AppKit's copy-on-scroll use it; as an accepted no-op, scrolled text did not
           move and only the newly exposed strip was drawn (TextWrangler and AppleWorks with the
           scroll wheel showed the old lines). OAH_NO_SCROLLREGION=1 disables. */
        oah_rect_t b;
        int dx = (int)R[7], dy = (int)R[8];
        w = oah_cgs_win(R[4]);
        if (w && w->store && (dx || dy) && oah_cgs_region_bounds(R[5], &b)) {
            int sc = WSC(w), x0, y0, x1, y1, y, rows, cols, sx0, sy0;
            unsigned char *bits;
            size_t rb = (size_t)WRB(w);
            if ((b.x >= w->w || b.y >= w->h) && b.x >= w->x && b.y >= w->y) { b.x -= w->x; b.y -= w->y; }   /* given in global coordinates */
            if (sc > 1 && w->qd) oah_qd_push(w);
            oah_blit_lock();
            bits = oah_cgs_bits(w);
            x0 = (int)(b.x * sc + 0.5f); y0 = (int)(b.y * sc + 0.5f);
            x1 = (int)((b.x + b.w) * sc + 0.5f); y1 = (int)((b.y + b.h) * sc + 0.5f);
            if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
            if (x1 > (int)(w->w * sc + 0.5f)) x1 = (int)(w->w * sc + 0.5f);
            if (y1 > (int)(w->h * sc + 0.5f)) y1 = (int)(w->h * sc + 0.5f);
            if (x1 > (int)rb / 4) x1 = (int)rb / 4;
            dx *= sc; dy *= sc;
            /* "area" is the source (the 10.6 server copied it to area + (dx,dy), e.g. a text view
               minus the band scrolled out): clip source and destination to the window */
            {
                int ww = (int)(w->w * sc + 0.5f), wh = (int)(w->h * sc + 0.5f);
                int ax0 = x0, ay0 = y0, ax1 = x1, ay1 = y1;
                if (ww > (int)rb / 4) ww = (int)rb / 4;
                if (ax0 + dx < 0) ax0 = -dx; if (ay0 + dy < 0) ay0 = -dy;
                if (ax1 + dx > ww) ax1 = ww - dx; if (ay1 + dy > wh) ay1 = wh - dy;
                cols = ax1 - ax0; rows = ay1 - ay0;
                sx0 = ax0; sy0 = ay0;
            }
            if (cols > 0 && rows > 0) {
                int dx0 = sx0 + dx, dy0 = sy0 + dy;
                if (dy > 0) for (y = rows - 1; y >= 0; y--)
                    memmove(bits + (size_t)(dy0 + y) * rb + (size_t)dx0 * 4, bits + (size_t)(sy0 + y) * rb + (size_t)sx0 * 4, (size_t)cols * 4);
                else for (y = 0; y < rows; y++)
                    memmove(bits + (size_t)(dy0 + y) * rb + (size_t)dx0 * 4, bits + (size_t)(sy0 + y) * rb + (size_t)sx0 * 4, (size_t)cols * 4);
            }
            if (sc > 1 && w->qd) w->qdh = 0;          /* the 1x QuickDraw copy is stale: re-pull at the next lock */
            oah_blit_unlock();
            CGSLOG("[cgs] ScrollRegion %u %.0f,%.0f %.0fx%.0f by %d,%d\n", R[4], b.x, b.y, b.w, b.h, (int)R[7], (int)R[8]);
        }
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSSetWindowOpaqueShape")) {     /* (cid, wid, region) */
        NCGS(int, CGSSetWindowOpaqueShape, (unsigned int, unsigned int, void *));
        oah_rect_t b;
        void *nr;
        if (!oah_cgs_region_bounds(R[5], &b)) { b.x = b.y = b.w = b.h = 0; }
        nr = oah_cgs_nregion(&b);
        R[3] = p_CGSSetWindowOpaqueShape ? p_CGSSetWindowOpaqueShape(g_ncid, R[4], nr) : 0;
        oah_cgs_nrelease(nr);
        return 1;
    }
    if (!strcmp(n, "CGSLockWindowVisibleRegion")) {  /* (cid, wid, CGSRegionRef *out) */
        oah_rect_t b;
        w = oah_cgs_win(R[4]);
        /* window-local (OAH_VISRGN_GLOBAL=1: global coordinates) */
        b.x = b.y = 0; b.w = w ? w->w : 0; b.h = w ? w->h : 0;
        if (w && getenv("OAH_VISRGN_GLOBAL")) { b.x = w->x; b.y = w->y; }
        if (w && w->visrgn) oah_cgs_grelease(w->visrgn);
        if (w) w->visrgn = oah_cgs_gregion(&b);
        CGSLOG("[cgs] LockWindowVisibleRegion %u -> %.0f,%.0f %.0fx%.0f\n", R[4], b.x, b.y, b.w, b.h);
        oah_put32(R[5], w ? w->visrgn : 0);
        R[3] = w ? 0 : 1001;
        return 1;
    }
    if (!strcmp(n, "CGSUnlockWindowVisibleRegion")) {   /* (cid, wid) */
        w = oah_cgs_win(R[4]);
        if (w && w->visrgn) { oah_cgs_grelease(w->visrgn); w->visrgn = 0; }
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSWindowVisibleRegionSeed")) {  /* (cid, wid) -> seed */
        w = oah_cgs_win(R[4]);
        R[3] = w ? (unsigned int)(w->w * 7 + w->h * 13 + w->ordered) + 1 : 1;
        return 1;
    }
    if (!strcmp(n, "CGSGetWindowDirtyRegion")) {     /* (cid, wid, CGSRegionRef *out), global coordinates */
        oah_rect_t b;
        w = oah_cgs_win(R[4]);
        b.x = b.y = b.w = b.h = 0;
        if (w && w->dirty) { b.x = w->x; b.y = w->y; b.w = w->w; b.h = w->h; }
        if (w && getenv("OAH_DIRTY_LOCAL")) b.x = b.y = 0;
        CGSLOG("[cgs] GetWindowDirtyRegion %u -> %.0f,%.0f %.0fx%.0f\n", R[4], b.x, b.y, b.w, b.h);
        oah_put32(R[5], oah_cgs_gregion(&b));
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSClearWindowDirtyRegion")) {
        w = oah_cgs_win(R[4]);
        if (w) w->dirty = 0;
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSWindowAddRegionToDirtyShape")) {   /* (cid, wid, region): see the flush below */
        oah_rect_t db;
        w = oah_cgs_win(R[4]);
        if (w) {
            w->dsh_added = 1;
            if (!R[5] || !oah_cgs_region_bounds_try(R[5], &db) || (db.w > 0 && db.h > 0)) w->dsh_nonempty = 1;
            else if (getenv("OAH_BLITLOG")) fprintf(stderr, "[blit] empty dirty-shape addition to %u (region %08x %.0f,%.0f %.0fx%.0f)\n", w->wid, R[5], db.x, db.y, db.w, db.h);
        }
        R[3] = 0; return 1;
    }
    if (!strcmp(n, "CGSAddDragRegion") || !strcmp(n, "CGSClearDragRegion")) { R[3] = 0; return 1; }
    if (!strcmp(n, "CGSSetWindowShape") || !strcmp(n, "CGSSetWindowShapeWithWeighting") ||
        (!strcmp(n, "CGSShapeWindow") && !getenv("OAH_NO_SHAPEWINDOW"))) {
        /* (cid, wid, float x, float y, shape [, weighting]). AppKit 10.6's live resize uses
           CGSShapeWindow(cid, wid, x, y, shape, ...) - same leading arguments (the floats take
           the r5/r6 slots); it was an unbridged no-op, so the native window kept its size while
           every intermediate frame was drawn into it (trails, content offset). */
        NCGS(int, CGSSetWindowShape, (unsigned int, unsigned int, float, float, void *));
        oah_rect_t b;
        void *nr;
        if (!oah_cgs_region_bounds(R[7], &b)) { R[3] = 1001; return 1; }
        nr = oah_cgs_nregion(&b);
        /* under the blit lock: the event thread may be blitting this window with the context
           released below (InDesign's palettes slide out with a reshape per frame; SkyLight
           crashed in CGSWindowGetSize under CGContextDrawImage) */
        oah_blit_lock();
        R[3] = p_CGSSetWindowShape(g_ncid, R[4], (float)F[1], (float)F[2], nr);
        oah_cgs_nrelease(nr);
        w = oah_cgs_win(R[4]);
        if (w && w->store && !getenv("OAH_NO_TOPANCHOR")) {
            /* WindowServer keeps a reshaped window's pixels anchored at its top-left, and AppKit's
               live resize only redraws what changed. Our store is bottom-anchored (top row at
               g_cgs_rows - h), so move the rows to keep the top where it was; without this the
               toolbar was drawn twice at two offsets while shrinking a Pages window. */
            int h0 = (int)(w->h * WSC(w) + 0.5f), h1 = (int)(b.h * WSC(w) + 0.5f);
            if (h0 > WROWS(w)) h0 = WROWS(w);
            if (h1 > WROWS(w)) h1 = WROWS(w);
            if (h0 > 0 && h1 > 0 && h0 != h1) {
                int keep = h0 < h1 ? h0 : h1;
                memmove(w->store + (size_t)(WROWS(w) - h1) * WRB(w), w->store + (size_t)(WROWS(w) - h0) * WRB(w), (size_t)keep * WRB(w));
            }
        }
        {   int expose = 0;
            if (w) {
                int grew = b.w > w->w || b.h > w->h;
                w->x = (float)F[1] + b.x; w->y = (float)F[2] + b.y; w->w = b.w; w->h = b.h;
                expose = grew && w->ordered && b.w > 0 && b.h > 0;
                if (w->nctx) { NCGS(void, CGContextRelease, (void *)); p_CGContextRelease(w->nctx); w->nctx = 0; }
                /* Mojave's window server does not keep a reshaped window's pixels (10.6's did): a
                   sheet reshaped at its final place after its one flush stayed blank, and the
                   blit's row diff then never sent those rows again. Send the whole store. */
                if (w->shadow) { free(w->shadow); w->shadow = 0; w->shw = w->shh = 0; }
                if (w->ordered && w->store && !expose && !getenv("OAH_NO_RESHAPEBLIT")) oah_cgs_blit_unl(w);
            }
            oah_blit_unlock();
            if (expose) oah_cgs_expose(w);
        }
        CGSLOG("[cgs] SetWindowShape %u at %.0f,%.0f %.0fx%.0f -> %d\n", R[4], F[1] + b.x, F[2] + b.y, b.w, b.h, R[3]);
        return 1;
    }
    /* Cursors: AppKit sets system cursors (arrow, I-beam, resize, crosshair, pointing hand...)
       with HIServices' CoreCursorSet and CGSSetSystemDefinedCursor[WithSeed]; the guest copies
       talked to no window server, so the cursor never changed shape. Mojave's own calls take
       the same cursor numbers. OAH_NO_CURSORS=1 disables. */
    if (!strcmp(n, "CoreCursorSet") || !strcmp(n, "CGSSetSystemDefinedCursor") || !strcmp(n, "CGSSetSystemDefinedCursorWithSeed") ||
        !strcmp(n, "CGSCurrentCursorSeed")) {
        static int on = -1;
        static void *hs;
        if (on < 0) on = getenv("OAH_NO_CURSORS") ? 0 : 1;
        if (!on) { R[3] = 0; return 1; }
        if (strcmp(n, "CGSCurrentCursorSeed")) oah_lastcur_system(n[0] == 'C' && n[1] == 'o' ? 3 : 2, (int)R[4]);
        if (g_alc_on && strcmp(n, "CGSCurrentCursorSeed")) {   /* under the outline cursor: kept for later */
            if (!strcmp(n, "CGSSetSystemDefinedCursorWithSeed") && R[5]) oah_put32(R[5], 0);
            R[3] = 0; return 1;
        }
        if (n[0] == 'C' && n[1] == 'o') {             /* CoreCursorSet(cid, type) */
            int (*f)(unsigned int, int);
            if (!hs) hs = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/HIServices.framework/HIServices", RTLD_LAZY);
            f = hs ? (int (*)(unsigned int, int))dlsym(hs, "CoreCursorSet") : 0;
            R[3] = f ? (unsigned int)f(g_ncid, (int)R[4]) : 0;
        } else if (!strcmp(n, "CGSCurrentCursorSeed")) {
            NCGS(int, CGSCurrentCursorSeed, (void));
            R[3] = p_CGSCurrentCursorSeed ? (unsigned int)p_CGSCurrentCursorSeed() : 0;
        } else if (oah_cursor_core((int)R[4]) >= 0 && !getenv("OAH_NO_CURSORMAP")) {
            /* Old CGS system cursors that Mojave dropped (it shows a blue square for them):
               2 I-beam XOR (QuickDraw's SetCursor(iBeam), AppleWorks' text tool), 3 alias,
               4 copy, 6 contextual menu. Show the matching CoreCursor instead. */
            int (*f)(unsigned int, int);
            int ct = oah_cursor_core((int)R[4]);
            if (!hs) hs = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/HIServices.framework/HIServices", RTLD_LAZY);
            f = hs ? (int (*)(unsigned int, int))dlsym(hs, "CoreCursorSet") : 0;
            if (f) f(g_ncid, ct);
            if (n[25] == 'W' && R[5]) {                /* ...WithSeed: hand back the new seed */
                NCGS(int, CGSCurrentCursorSeed, (void));
                oah_put32(R[5], p_CGSCurrentCursorSeed ? (unsigned int)p_CGSCurrentCursorSeed() : 0);
            }
            R[3] = 0;
        } else if (!strcmp(n, "CGSSetSystemDefinedCursor")) {   /* (cid, type) */
            NCGS(int, CGSSetSystemDefinedCursor, (unsigned int, int));
            R[3] = p_CGSSetSystemDefinedCursor ? (unsigned int)p_CGSSetSystemDefinedCursor(g_ncid, (int)R[4]) : 0;
        } else {                                       /* (cid, type, int *seed) */
            NCGS(int, CGSSetSystemDefinedCursorWithSeed, (unsigned int, int, int *));
            int seed = 0;
            R[3] = p_CGSSetSystemDefinedCursorWithSeed ? (unsigned int)p_CGSSetSystemDefinedCursorWithSeed(g_ncid, (int)R[4], &seed) : 0;
            if (R[5]) oah_put32(R[5], (unsigned int)seed);
        }
        {   static int nlog; if (nlog++ < 8 || g_trktrace > 0 || (getenv("OAH_CURSOR_DUMP") && n[3] != 'C'))
                fprintf(stderr, "[cursor] %s(%d) -> %d\n", n, (int)R[4], (int)R[3]); }
        return 1;
    }
    /* Custom (image) cursors from QuickDraw's SetCursor / SetCCursor (AppleWorks' tool cursors,
       IE's hand): CGSSetCursorFromPremultipliedDataWithSeed(cid, data, rowBytes, data2, rowBytes2,
       CGRect rect, CGPoint hotSpot, bpp, spp, bps, int *seed), signature from QD's i386 call site.
       PPC passes the two structs in GPR words (r8-r10, then the stack). The pixels are 32-bit
       A,R,G,B bytes in memory order on both sides, so they pass unchanged (OAH_CURSOR_SWAP=1
       swaps each word; OAH_CURSOR_DUMP=1 logs the guest bytes). QuickDraw's "invert" pixels come
       as A=0 with white colour (00ffffff). OAH_NO_CURSORS=1 disables. */
    /* Cursor visibility (cid): ObscureCursor hides it while typing until the mouse moves
       (NSCursor setHiddenUntilMouseMoves, TextEdit-style), Hide/Show nest. */
    if ((!strcmp(n, "CGSObscureCursor") || !strcmp(n, "CGSRevealCursor") || !strcmp(n, "CGSHideCursor") ||
         !strcmp(n, "CGSShowCursor")) && !getenv("OAH_NO_CURSORS")) {
        int (*f)(unsigned int) = (int (*)(unsigned int))oah_ncgs(n);
        int hide = !strcmp(n, "CGSHideCursor"), show = !strcmp(n, "CGSShowCursor");
        if (g_alc_on && (hide || show)) {          /* Photoshop's outline cursor shows: keep it visible */
            if (hide) g_alc_hides++;
            else if (g_alc_hides > 0 && !--g_alc_hides) oah_alc_onoff(0);   /* shown again: the outline is over */
            R[3] = 0;
        } else {
            R[3] = f ? (unsigned int)f(g_ncid) : 0;
            if (hide) g_cur_hides++;
            else if (show && g_cur_hides > 0) g_cur_hides--;
        }
        CGSLOG("[cursor] %s -> %d%s\n", n, (int)R[3], g_alc_on ? " (outline cursor)" : "");
        return 1;
    }
    if (!strcmp(n, "CGSSetCursorFromPremultipliedDataWithSeed") && !getenv("OAH_NO_CURSORS")) {
        NCGS(int, CGSSetCursorFromPremultipliedDataWithSeed, (unsigned int, void *, int, void *, int,
             oah_rect_t, oah_pt_t, int, int, int, int *));
        oah_rect_t r; oah_pt_t hot;
        unsigned int u[4], k;
        int bpp = (int)oah_stackarg(R, 12), spp = (int)oah_stackarg(R, 13), bps = (int)oah_stackarg(R, 14);
        unsigned int seedp = oah_stackarg(R, 15);
        int rb = (int)R[5], rb2 = (int)R[7], rows, seed = 0;
        unsigned char *d1 = 0, *d2 = 0;
        u[0] = R[8]; u[1] = R[9]; u[2] = R[10]; u[3] = oah_stackarg(R, 9);
        memcpy(&r, u, 16);
        u[0] = oah_stackarg(R, 10); u[1] = oah_stackarg(R, 11);
        memcpy(&hot, u, 8);
        rows = (int)(r.h + 0.5f);
        R[3] = 1000;
        if (p_CGSSetCursorFromPremultipliedDataWithSeed && R[4] && rb > 0 && rb <= 4096 && rows > 0 && rows <= 1024 && bpp == 32) {
            int swap = getenv("OAH_CURSOR_SWAP") != 0;
            d1 = (unsigned char *)malloc((size_t)rb * rows);
            memcpy(d1, (void *)R[4], (size_t)rb * rows);
            if (getenv("OAH_CURSOR_DUMP")) {          /* guest bytes, one row per line */
                int y, x;
                for (y = 0; y < rows && y < 32; y++) {
                    char line[1100]; int o = 0;
                    for (x = 0; x < rb / 4 && x < 32; x++) {
                        unsigned char *p = (unsigned char *)R[4] + y * rb + x * 4;
                        unsigned char *q = R[6] ? (unsigned char *)R[6] + y * rb2 + x * 4 : p;
                        o += sprintf(line + o, "%02x%02x%02x%02x/%02x ", p[0], p[1], p[2], p[3], q[0]);
                    }
                    fprintf(stderr, "[cursor] %2d %s\n", y, line);
                }
            }
            if (swap) for (k = 0; k < (unsigned int)(rb * rows) / 4; k++) ((unsigned int *)d1)[k] = OSSwapBigToHostInt32(((unsigned int *)d1)[k]);
            if (R[6] && rb2 > 0 && rb2 <= 4096) {
                d2 = (unsigned char *)malloc((size_t)rb2 * rows);
                memcpy(d2, (void *)R[6], (size_t)rb2 * rows);
                if (swap) for (k = 0; k < (unsigned int)(rb2 * rows) / 4; k++) ((unsigned int *)d2)[k] = OSSwapBigToHostInt32(((unsigned int *)d2)[k]);
            }
            oah_lastcur_image(d1, rb, d2, rb2, rows, r, hot, bpp, spp, bps);
            if (g_alc_on) { R[3] = 0; seed = 0; }   /* under the outline cursor: kept for later */
            else R[3] = (unsigned int)p_CGSSetCursorFromPremultipliedDataWithSeed(g_ncid, d1, rb, d2, rb2, r, hot, bpp, spp, bps, &seed);
            if (seedp) oah_put32(seedp, (unsigned int)seed);
            free(d1); free(d2);
        }
        {   static int nlog; if (nlog++ < 8 || g_trktrace > 0)
                fprintf(stderr, "[cursor] %s(rb %d/%d, %.0f,%.0f %.0fx%.0f, hot %.0f,%.0f, %d/%d/%d) -> %d seed %d\n", n, rb, rb2,
                        r.x, r.y, r.w, r.h, hot.x, hot.y, bpp, spp, bps, (int)R[3], seed); }
        return 1;
    }
    /* Minimize to the Dock: AppKit's -miniaturize: calls CoreDockMinimizeItemWithTitle(wid, title)
       and restores with CoreDockRestoreItem(wid). The guest HIServices' MIG messages never
       reached Mojave's Dock, so the yellow button and Cmd-M did nothing. The windows are
       native windows of this process, so Mojave's own calls work on them. OAH_NO_DOCKMIN=1. */
    if (!strcmp(n, "CGSDoDeferredOrdering") || !strcmp(n, "CGSSetMouseFocusWindow")) {   /* (cid, wid, ...) */
        /* Click-to-front inside the app: AppKit defers the window server's raise on mouse-down
           (CGSSetDeferOrdering) and then asks for it with CGSDoDeferredOrdering(cid, wid); as an
           accepted no-op a background document clicked in its title bar stayed behind.
           CGSSetMouseFocusWindow during a Dock menu choice is that window being selected. */
        NCGS(int, CGSOrderWindow, (unsigned int, unsigned int, int, unsigned int));
        w = oah_cgs_win(R[4]);
        if (g_in_dock_deliver && oah_docktrace())
            fprintf(stderr, "[dock] %s(%u) during a menu choice (ordered %d)\n", n, R[4], w ? w->ordered : -1);
        if (w && w->ordered && !w->ghost && !w->hod_hidden && !g_app_hidden && p_CGSOrderWindow && !getenv("OAH_NO_DEFERORDER") &&
            (n[3] == 'D' || g_in_dock_deliver || (w->level == 0 && R[4] != g_last_focus && !getenv("OAH_NO_FOCUSRAISE")))) {
            /* CGSSetMouseFocusWindow comes when the key window changes (Window menu, Cmd-`, Dock
               menu): AppKit makes the window key and leaves the raise to the window server.
               A child (sheet, drawer) raises its parent, and the parent's children follow. */
            unsigned int top = R[4];
            int g, depth;
            for (depth = 0; depth < 4; depth++) {
                for (g = 0; g < OAH_NGROUP; g++) if (g_cgs_group[g].child == top && g_cgs_group[g].parent) break;
                if (g == OAH_NGROUP) break;
                top = g_cgs_group[g].parent;
            }
            p_CGSOrderWindow(g_ncid, top, 1, 0);
            oah_cgs_group_follow(top, 1);
            CGSLOG("[cgs] %s %u -> raised (%u)\n", n, R[4], top);
        }
        if (n[3] == 'S') g_last_focus = R[4];
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "_HideOtherApplications") || !strcmp(n, "_ShowAllApplications")) {
        /* native versions on a helper thread (they go through Launch Services); Show All also
           shows us if we were hidden */
        pthread_t th;
        if (n[1] == 'S' && g_app_hidden) oah_app_hide(0);
        if (!pthread_create(&th, 0, n[1] == 'H' ? oah_hideothers_thread : oah_showall_thread, 0)) pthread_detach(th);
        CGSLOG("[cgs] %s\n", n);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "SetFrontProcessWithOptions")) {    /* (const ProcessSerialNumber *, OptionBits) */
        /* through the window server (SLPS), not Launch Services: the native Process Manager call
           goes to launchservicesd, which can be waiting on this app (it hung at launch) */
        static int (*slps)(void *, unsigned int, unsigned int);
        unsigned int psn[2];
        if (!slps) slps = (int (*)(void *, unsigned int, unsigned int))dlsym(RTLD_DEFAULT, "_SLPSSetFrontProcessWithOptions");
        psn[0] = oah_get32(R[3]); psn[1] = oah_get32(R[3] + 4);
        if (psn[0] == 0 && psn[1] == 2) {              /* kCurrentProcess */
            static int (*gcp)(unsigned int *);
            if (!gcp) gcp = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "GetCurrentProcess");
            if (gcp) gcp(psn);
        }
        R[3] = slps ? (unsigned int)slps(psn, 0, R[4] & 0xffff) : (unsigned int)-50;
        CGSLOG("[cgs] SetFrontProcessWithOptions(%x.%x, %x) -> %d\n", psn[0], psn[1], R[4], (int)R[3]);
        return 1;
    }
    if (!strcmp(n, "ShowHideProcess")) {               /* (const ProcessSerialNumber *, Boolean visible) */
        /* Never synchronous here: Launch Services can be waiting on this very app (at launch the
           guest shows itself with ShowHideProcess(self, true), and the native call hung it
           before its first window). Our own process is handled locally; another process's
           hide/show (Hide Others, Show All) is passed to the native call on a helper thread. */
        static int (*gcp)(unsigned int *);
        unsigned int psn[2], me[2] = { 0, 0 };
        if (!gcp) gcp = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "GetCurrentProcess");
        psn[0] = oah_get32(R[3]); psn[1] = oah_get32(R[3] + 4);
        if (gcp) gcp(me);
        if ((psn[1] == me[1] && psn[0] == me[0]) || (psn[0] == 0 && psn[1] == 2 /* kCurrentProcess */)) {
            oah_app_hide(R[4] ? 0 : 1);
            if (!R[4]) {                               /* hide: Launch Services marks us hidden and */
                unsigned int *arg = (unsigned int *)malloc(3 * sizeof(unsigned int));   /* brings the next app front */
                pthread_t th;
                arg[0] = 0; arg[1] = 2; arg[2] = 0;    /* kCurrentProcess, hidden (helper thread) */
                if (!pthread_create(&th, 0, oah_showhide_thread, arg)) pthread_detach(th); else free(arg);
            }
        } else {
            unsigned int *arg = (unsigned int *)malloc(3 * sizeof(unsigned int));
            pthread_t th;
            arg[0] = psn[0]; arg[1] = psn[1]; arg[2] = R[4] ? 1 : 0;
            if (!pthread_create(&th, 0, oah_showhide_thread, arg)) pthread_detach(th); else free(arg);
        }
        R[3] = 0;
        CGSLOG("[cgs] ShowHideProcess(%x.%x, %u)\n", psn[0], psn[1], R[4]);
        return 1;
    }
    if (!strcmp(n, "CoreDockRegisterClientWithRunLoop")) {   /* (callback, refcon, CFRunLoopRef) */
        static int started;
        g_dock_gcb = R[3]; g_dock_grefcon = R[4];
        if (!started && !getenv("OAH_NO_DOCKCLIENT")) {
            pthread_t th;
            started = 1;
            pthread_create(&th, 0, oah_dock_thread, 0);
            pthread_detach(th);
        }
        fprintf(stderr, "[dock] guest client callback %08x refcon %08x\n", R[3], R[4]);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CoreDockRestoreItemWithOrder") || !strcmp(n, "CoreDockRemoveItem") ||
        !strcmp(n, "CoreDockSetItemTitle") || !strcmp(n, "CoreDockUpdateWindow")) {
        /* the rest of the per-window Dock calls HIToolbox (Carbon windows) makes; the guest's
           synchronous versions wait on the guest client's run loop source, which no longer
           exists (CoreDockRegisterClientWithRunLoop is native now) and crashed in CFRunLoop */
        static void *hs;
        int (*f)() = 0;
        unsigned int wid = R[3];
        if (!hs) hs = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/HIServices.framework/HIServices", RTLD_LAZY);
        if (hs) {
            if (n[8] == 'R' && n[10] == 's') f = (int (*)())dlsym(hs, "CoreDockRestoreItemWithOrderAsync");
            else f = (int (*)())dlsym(hs, n);
        }
        R[3] = (unsigned int)-1;
        if (f) {
            if (n[8] == 'R' && n[10] == 's') R[3] = (unsigned int)((int (*)(int, int, int))f)((int)wid, (int)R[4], (int)R[5]);
            else if (n[8] == 'S') {
                CFStringRef t = oah_guest_str_native(R[4]);
                R[3] = (unsigned int)((int (*)(int, CFStringRef))f)((int)wid, t);
                if (t) CFRelease(t);
            } else R[3] = (unsigned int)((int (*)(int))f)((int)wid);
        }
        fprintf(stderr, "[dock] %s(%u) -> %d\n", n, wid, (int)R[3]);
        return 1;
    }
    if (!strncmp(n, "CoreDock", 8) && (!strcmp(n, "CoreDockMinimizeItemWithTitle") || !strcmp(n, "CoreDockRestoreItem"))) {
        static void *hs;
        int (*f)() = 0;
        if (!getenv("OAH_NO_DOCKMIN")) {
            char an[64];
            if (!hs) hs = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/HIServices.framework/HIServices", RTLD_LAZY);
            /* the synchronous versions wait for the Dock's reply on a run loop source that only
               CoreDockRegisterClientWithRunLoop creates (crashed in CFRunLoopAddSource) */
            snprintf(an, sizeof an, "%sAsync", n);
            f = hs ? (int (*)())dlsym(hs, an) : 0;
        }
        unsigned int wid = R[3];
        R[3] = (unsigned int)-1;
        if (f && n[8] == 'M') {
            CFStringRef t = oah_guest_str_native(R[4]);
            R[3] = (unsigned int)((int (*)(int, CFStringRef))f)((int)wid, t);
            if (t) CFRelease(t);
        } else if (f) R[3] = (unsigned int)((int (*)(int))f)((int)wid);
        fprintf(stderr, "[dock] %s(%u) -> %d\n", n, wid, (int)R[3]);
        return 1;
    }
    if (!strcmp(n, "CGSSetWindowTitle")) {           /* (cid, wid, CFStringRef) */
        /* native window titles: Mission Control, the Dock's window list, Accessibility */
        NCGS(int, CGSSetWindowTitle, (unsigned int, unsigned int, CFStringRef));
        CFStringRef t = getenv("OAH_NO_WINTITLE") ? NULL : oah_guest_str_native(R[5]);
        R[3] = 0;
        if (t && p_CGSSetWindowTitle) { R[3] = (unsigned int)p_CGSSetWindowTitle(g_ncid, R[4], t); }
        if (t) CFRelease(t);
        return 1;
    }
    if ((!strcmp(n, "CGSSetHideOnDeact") || !strcmp(n, "CGSSetIgnoresCycle")) && !getenv("OAH_NO_WINFLAGS")) {
        /* (cid, wid, Boolean): floating panels hide when the app deactivates; panels skip Cmd-` */
        /* CGSSetHideOnDeact(cid, Boolean a, Boolean hide, wid) (AppKit's call site; Mojave's
           SLSSetHideOnDeact has the same shape and keeps it in window tag 0x4000). Mojave's server
           hides such windows on deactivation but never shows them again for this app, so the
           hiding is done here instead (oah_hod_apply, on the guest's (de)activation events). */
        NCGS(int, CGSSetIgnoresCycle, (unsigned int, unsigned int, int));
        unsigned int a4 = R[4], a5 = R[5], a6 = R[6];
        if (n[6] == 'H') {
            w = oah_cgs_win(a6);
            if (w) w->hod = (a5 & 0xff) != 0;
            R[3] = 0;
        } else R[3] = p_CGSSetIgnoresCycle ? (unsigned int)p_CGSSetIgnoresCycle(g_ncid, a4, a5 ? 1 : 0) : 0;
        CGSLOG("[cgs] %s(%x, %u, %x) -> %d\n", n, a4, a5, a6, (int)R[3]);
        return 1;
    }
    if (!strncmp(n, "CGSAddTrackingArea", 18) || !strncmp(n, "CGSRemoveTrackingArea", 21) ||
        !strncmp(n, "CGSSetTrackingArea", 18) || !strcmp(n, "CGSRemoveAllTrackingAreas")) {
        R[3] = (unsigned int)oah_trk_call(n, R);
        return 1;
    }
    if (!strcmp(n, "CGSGetWindowBounds") || !strcmp(n, "CGSGetScreenRectForWindow")) {   /* (cid, wid, CGRect *) */
        w = oah_cgs_win(R[4]);
        if (!w) { R[3] = 1001; return 1; }
        oah_putf(R[5], w->x); oah_putf(R[5] + 4, w->y); oah_putf(R[5] + 8, w->w); oah_putf(R[5] + 12, w->h);
        R[3] = 0;
        return 1;
    }
    /* Getters the generic stub answered with 0 ("success") without filling the output: the
       caller then used stack garbage (Keynote's sheet animation passed a junk region from
       CGSGetWindowShape to CGSGetRegionBounds and died). */
    if (!strcmp(n, "CGSGetWindowShape")) {           /* (cid, wid, CGSRegionRef *) global coordinates */
        oah_rect_t rr;
        w = oah_cgs_win(R[4]);
        if (!w || !R[5]) { if (R[5]) oah_put32(R[5], 0); R[3] = 1001; return 1; }
        rr.x = w->x; rr.y = w->y; rr.w = w->w; rr.h = w->h;
        oah_put32(R[5], oah_cgs_gregion(&rr));
        R[3] = oah_get32(R[5]) ? 0 : 1000;
        return 1;
    }
    if (!strcmp(n, "CGSGetWindowTags")) {            /* (cid, wid, CGSWindowTagBit *tags, int nbits) */
        unsigned int k2, nw = (R[6] + 31) / 32;
        if (nw > 2) nw = 2;
        for (k2 = 0; k2 < nw && R[5]; k2++) oah_put32(R[5] + 4 * k2, 0);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSGetWindowEventMask") || !strcmp(n, "CGSGetWindowWorkspaceIgnoringVisibility") ||
        !strcmp(n, "CGSGetWindowAutofill")) {        /* (cid, wid, int *) */
        if (R[5]) oah_put32(R[5], !strcmp(n, "CGSGetWindowWorkspaceIgnoringVisibility") ? 1 : 0);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSGetWorkspace") || !strcmp(n, "CGSGetBackgroundEventMask") ||
        !strcmp(n, "CGSGetCoalesceEventsMask")) {    /* (cid, int *) */
        if (R[4]) oah_put32(R[4], !strcmp(n, "CGSGetWorkspace") ? 1 : 0);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSGetWindowType")) {            /* (cid, wid, int *) */
        w = oah_cgs_win(R[4]);
        oah_put32(R[5], w ? (unsigned int)w->type : 2);
        R[3] = w ? 0 : 1001;
        return 1;
    }
    if (!strcmp(n, "CGSGetWindowOwner")) { oah_put32(R[5], g_ncid); R[3] = 0; return 1; }
    if (!strcmp(n, "CGSSetWindowOpacity")) {         /* (cid, wid, bool) */
        NCGS(int, CGSSetWindowOpacity, (unsigned int, unsigned int, int));
        w = oah_cgs_win(R[4]);
        if (w) w->opaque = R[5] != 0;
        R[3] = p_CGSSetWindowOpacity(g_ncid, R[4], R[5] != 0);
        return 1;
    }
    if (!strcmp(n, "CGSGetWindowOpacity")) {
        w = oah_cgs_win(R[4]);
        if (R[5]) *(unsigned char *)R[5] = w ? (unsigned char)w->opaque : 1;
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSSetWindowLevel")) {           /* (cid, wid, level) */
        NCGS(int, CGSSetWindowLevel, (unsigned int, unsigned int, int));
        w = oah_cgs_win(R[4]);
        if (w) { w->level = (int)R[5]; w->actlev = 0; oah_cgs_tags_note(w); }   /* a plain level ends levels-for-activation */
        R[3] = p_CGSSetWindowLevel(g_ncid, R[4], (int)R[5]);
        return 1;
    }
    if (!strcmp(n, "CGSGetWindowLevel")) {
        w = oah_cgs_win(R[4]);
        oah_put32(R[5], w ? (unsigned int)w->level : 0);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSSetWindowAlpha")) {           /* (cid, wid, float) */
        NCGS(int, CGSSetWindowAlpha, (unsigned int, unsigned int, float));
        R[3] = p_CGSSetWindowAlpha(g_ncid, R[4], (float)F[1]);
        return 1;
    }
    if (!strcmp(n, "CGSGetWindowAlpha")) {           /* (cid, wid, float *): was unbridged, the float left as it was */
        NCGS(int, CGSGetWindowAlpha, (unsigned int, unsigned int, float *));
        float a = 1.0f; union { float f; unsigned int u; } cv;
        R[3] = p_CGSGetWindowAlpha ? (unsigned int)p_CGSGetWindowAlpha(g_ncid, R[4], &a) : 0;
        if (R[3]) a = 1.0f, R[3] = 0;
        cv.f = a;
        if (R[5]) oah_put32(R[5], cv.u);
        CGSLOG("[cgs] CGSGetWindowAlpha(%u) -> %.2f\n", R[4], a);
        return 1;
    }
    if (!strcmp(n, "CGSSetWindowEventMask")) {
        NCGS(int, CGSSetWindowEventMask, (unsigned int, unsigned int, unsigned int));
        R[3] = p_CGSSetWindowEventMask(g_ncid, R[4], R[5]);
        CGSLOG("[cgs] CGSSetWindowEventMask(%u, %08x) -> %d\n", R[4], R[5], (int)R[3]);
        return 1;
    }

    /* drawing */
    if (!strcmp(n, "CGSLockWindowBits")) {   /* (cid, wid, CGRect *bounds, int *depthFormat, void **base, int *rowBytes) */
        w = oah_cgs_win(R[4]);
        if (!w) { R[3] = 1001; return 1; }
        if (WSC(w) > 1) {
            if (!w->qd) {
                w->qd = (unsigned char *)mmap(0, (size_t)g_cgs_rows * g_cgs_rb, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
                w->qdsh = (unsigned char *)mmap(0, (size_t)g_cgs_rows * g_cgs_rb, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
                if (w->qd == (unsigned char *)MAP_FAILED || w->qdsh == (unsigned char *)MAP_FAILED) { R[3] = 1000; w->qd = w->qdsh = 0; return 1; }
            }
            oah_qd_push(w);                  /* anything QuickDraw left since */
            oah_qd_pull(w);
        }
        {
            static int glob = -1;
            if (glob < 0) glob = getenv("OAH_LWB_LOCAL") ? 0 : 1;
            /* every output is optional: QuickDraw asks for the row bytes alone (NULL bounds,
               depth and base) when the screen and window depths differ */
            if (R[5]) {
                oah_putf(R[5], glob ? w->x : 0); oah_putf(R[5] + 4, glob ? w->y : 0);
                oah_putf(R[5] + 8, w->w); oah_putf(R[5] + 12, w->h);
            }
        }
        CGSLOG("[cgs] LockWindowBits %u %.0fx%.0f\n", w->wid, w->w, w->h);
        oah_unlock_blit_queue(w->wid);                       /* in case no unlock follows */
        if (R[6]) oah_put32(R[6], 4);                        /* 32 bits per pixel (QD table) */
        if (R[7]) oah_put32(R[7], WSC(w) > 1 && w->qd ? (unsigned int)(w->qd + (size_t)(g_cgs_rows - w->qdh) * g_cgs_rb) : (unsigned int)oah_cgs_bits(w));
        if (R[8]) oah_put32(R[8], g_cgs_rb);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSLockWindowRectBits") && getenv("OAH_LOCKRECTBITS")) {
        /* (cid, wid, rect, CGRect *bounds, CGDirectDisplayID *display, int *depthFormat,
           void *base[2], int rowBytes[2]): like CGSLockWindowBits, with the window's backing
           (display 0xffff) as the 10.6 server answered for an off-screen window. Photoshop draws
           its selection outline and brush feedback straight into an overlay window this way; as
           an accepted no-op its outputs were left as stack garbage. OAH_NO_LOCKRECTBITS=1. */
        w = oah_cgs_win(R[4]);
        if (!w || !w->store) { R[3] = 1001; return 1; }
        if (WSC(w) > 1) {
            if (!w->qd) {
                w->qd = (unsigned char *)mmap(0, (size_t)g_cgs_rows * g_cgs_rb, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
                w->qdsh = (unsigned char *)mmap(0, (size_t)g_cgs_rows * g_cgs_rb, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
                if (w->qd == (unsigned char *)MAP_FAILED || w->qdsh == (unsigned char *)MAP_FAILED) { R[3] = 1000; w->qd = w->qdsh = 0; return 1; }
            }
            oah_qd_push(w);
            oah_qd_pull(w);
        }
        if (R[6]) {
            static int glob = -1;
            if (glob < 0) glob = getenv("OAH_LWB_LOCAL") ? 0 : 1;
            oah_putf(R[6], glob ? w->x : 0); oah_putf(R[6] + 4, glob ? w->y : 0);
            oah_putf(R[6] + 8, w->w); oah_putf(R[6] + 12, w->h);
        }
        if (R[7]) oah_put32(R[7], 0xffff);
        if (R[8]) oah_put32(R[8], 4);
        if (R[9]) {
            oah_put32(R[9], WSC(w) > 1 && w->qd ? (unsigned int)(w->qd + (size_t)(g_cgs_rows - w->qdh) * g_cgs_rb) : (unsigned int)oah_cgs_bits(w));
            oah_put32(R[9] + 4, 0);
        }
        if (R[10]) { oah_put32(R[10], g_cgs_rb); oah_put32(R[10] + 4, 0); }
        oah_unlock_blit_queue(w->wid);
        CGSLOG("[cgs] LockWindowRectBits %u %.0fx%.0f\n", w->wid, w->w, w->h);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGSLockWindowRectBits")) {
        /* Without OAH_LOCKRECTBITS: fail, with every output cleared. Photoshop's AdobeLargeCursor
           plug-in (brush outlines larger than the system cursor) draws into the display's memory
           through these and Mojave has no such access; given the window's bits it went on to region
           operations on garbage and crashed (CGSXorRegion) on the first brush stroke. */
        int a;
        for (a = 6; a <= 10; a++) if (R[a] && !(R[a] & 3)) { oah_put32(R[a], 0); if (a >= 9) oah_put32(R[a] + 4, 0); }
        R[3] = 1000;
        return 1;
    }
    if (!strcmp(n, "CGSLockWindowBackingStore")) {
        /* Direct access to the WindowServer's own backing store (guest CG's window contexts take
           it when a view draws outside the normal flush path). There is none to hand out here:
           the window's pixels live in the shim's store. Unbridged it returned success with its
           out-parameters untouched, and the caller released a stack garbage pointer (TextWrangler
           crashed typing into a new document). Clear the outputs and fail: the caller then draws
           through its regular context. */
        int a;
        for (a = 5; a <= 10; a++)
            if (R[a] && !(R[a] & 3) && R[a] > R[1] && R[a] - R[1] < 0x10000) oah_put32(R[a], 0);
        R[3] = 1000;                                     /* kCGErrorFailure */
        return 1;
    }
    if (!strcmp(n, "CGSUnlockWindowBackingStore")) { R[3] = 0; return 1; }
    if (!strcmp(n, "CGSUnlockWindowBits") || !strcmp(n, "CGSUnlockWindowRectBits")) {
        w = oah_cgs_win(R[4]);
        if (w && WSC(w) > 1) oah_qd_push(w);
        if (w) oah_unlock_blit_queue(w->wid);
        R[3] = 0; return 1;
    }
    if (!strcmp(n, "CGSFlushWindow") || !strcmp(n, "CGSFlushWindowContentRegion")) {   /* (cid, wid, region) */
        w = oah_cgs_win(R[4]);
        {   /* OAH_FLUSHBT=<wid|-1 for the menu bar>: who keeps flushing this window (every 64th) */
            static int fw = -2; static unsigned int fc;
            if (fw == -2) fw = getenv("OAH_FLUSHBT") ? atoi(getenv("OAH_FLUSHBT")) : 0;
            if (fw && (R[4] == (unsigned int)fw || (fw == -1 && R[4] == g_menubar_wid) || (fw == -3 && R[4] != g_menubar_wid)) && (fc++ & 63) == 0) {
                unsigned int sp = R[1], k; oah_rect_t rb = { 0, 0, 0, 0 };
                if (R[5]) oah_cgs_region_bounds(R[5], &rb);
                fprintf(stderr, "[flushbt] wid %u #%u rgn %.0f,%.0f %.0fx%.0f lr=%08x:", R[4], fc, rb.x, rb.y, rb.w, rb.h, R[0x90 / 4]);
                for (k = 0; k < 24 && sp && !(sp & 3) && sp > 0x1000; k++) {   /* raw: image names cost ~1 s each */
                    unsigned int back = 0, ra = 0;
                    if (!oah_bt_word(sp, &back) || !back || back <= sp || !oah_bt_word(back + 8, &ra)) break;
                    fprintf(stderr, " %08x", ra);
                    sp = back;
                }
                fprintf(stderr, "\n");
            }
        }
        {   /* QDFlushPortBuffer adds the port's dirty region to the window's dirty shape, then
               flushes the window (NULL region): the window server copies the dirty shape. Office
               2004 flushes every window 30 times a second while idle with an empty dirty region;
               each flush was a full blit here (~10% CPU for Word with a document and palettes).
               A flush whose dirty-shape additions were all empty, or whose own region is empty,
               is skipped. OAH_NO_EMPTYFLUSHSKIP=1 blits them all. */
            static int on = -1; oah_rect_t eb;
            int skip = 0;
            if (on < 0) on = getenv("OAH_NO_EMPTYFLUSHSKIP") ? 0 : 1;
            if (w && (w->ordered != w->fl_ord || w->x != w->fl_x || w->y != w->fl_y)) {
                if (w->ordered) w->shown_t = CFAbsoluteTimeGetCurrent();
                w->fl_ord = w->ordered; w->fl_x = w->x; w->fl_y = w->y;
            }
            if (on && w) {
                if (R[5]) skip = oah_cgs_region_bounds_try(R[5], &eb) && (eb.w <= 0 || eb.h <= 0);
                else skip = w->dsh_added && !w->dsh_nonempty && !w->dirty;
            }
            if (w) w->dsh_added = w->dsh_nonempty = 0;
            /* ...but not more than 0.25 s after the window's last blit: HIToolbox draws a Carbon
               sheet (TextWrangler's "Save changes?") and flushes it with an empty dirty shape, so the
               sheet stayed blank. The blit's row diff makes a flush with nothing changed cheap. */
            if (skip && w->store && !getenv("OAH_NO_FLUSHCHECK")) {
                double now = CFAbsoluteTimeGetCurrent();
                if (now - w->blit_t > 0.25 && now - w->fchk_t > 0.25) { w->fchk_t = now; skip = 0; }
                /* Nor for a second after a window came on screen or moved: Office 2004 builds an
                   alert sheet off screen (y -565), erases it and flushes the bare background, moves
                   it under the title bar, then draws the text and buttons with "empty" flushes. The
                   dialog showed for a quarter second without its buttons (until the check above):
                   the default button blinked out as the alert appeared. */
                if (skip && now - w->shown_t < 1.0) skip = 0;
            }
            if (skip && getenv("OAH_BLITLOG")) {
                oah_rect_t sb = {0, 0, 0, 0};
                if (R[5]) oah_cgs_region_bounds_try(R[5], &sb);
                fprintf(stderr, "[blit] skipped empty flush of %u (region %08x %.0f,%.0f %.0fx%.0f, dsh_added %d)\n", w->wid, R[5], sb.x, sb.y, sb.w, sb.h, w->dsh_added);
            }
            if (skip) { g_cgs_flush_skipped++; R[3] = 0; return 1; }
        }
        if (w && g_flushlog) {
            oah_rect_t fb = { 0, 0, -1, -1 };
            if (R[5]) oah_cgs_region_bounds_try(R[5], &fb);
            fprintf(stderr, "[flush] %.3f %s wid %u rgn %08x %.0f,%.0f %.0fx%.0f\n", CFAbsoluteTimeGetCurrent(), pthread_main_np() ? "main" : "thr ", w->wid, R[5], fb.x, fb.y, fb.w, fb.h);
        }
        if (w) oah_cgs_blit(w);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(n, "CGWindowContextCreate")) {       /* (cid, wid, CFDictionaryRef) */
        w = oah_cgs_win(R[4]);
        R[3] = w ? oah_cgs_window_context(w) : 0;
        return 1;
    }
    if (!strcmp(n, "CGContextFlush") || !strcmp(n, "CGContextSynchronize")) {
        w = oah_cgs_win_by_ctx(R[3]);
        if (w && n[9] == 'F' && g_flushlog) fprintf(stderr, "[flush] %.3f %s wid %u CGContextFlush\n", CFAbsoluteTimeGetCurrent(), pthread_main_np() ? "main" : "thr ", w->wid);
        if (w && n[9] == 'F') oah_cgs_blit(w);
        return 1;
    }
    return 0;
}

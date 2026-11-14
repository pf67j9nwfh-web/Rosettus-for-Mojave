/* ---- OpenGL / AGL bridge (included by liboah_mojave.c before oah_gen_call) ----
   The guest's PowerPC libGL has no driver on Mojave, so every gl* / agl* call
   the game makes is trapped here and replayed on Mojave's native i386 GL.
   Byte order:
     - scalar args arrive in GPRs/FPRs: no conversion
     - small in/out arrays (matrices, fog colour, ids): swapped here
     - pixel transfers: GL_UNPACK/PACK_SWAP_BYTES is forced on in every context
       (the game's own glPixelStorei of those two names is inverted)
     - client vertex arrays: only recorded; converted into native scratch
       buffers at glDrawArrays / glDrawElements time
     - compressed (DXT) data is byte-defined: passed through
   APPLE vertex array objects are emulated here (native VAO 0 is always used),
   vertex array range / object-finish calls are no-ops because GL copies
   client arrays at draw time anyway. */
#define OAH_GL_LIST \
 X(glActiveTexture) X(glActiveTextureARB) X(glAlphaFunc) X(glBindProgramARB) X(glBindTexture) \
 X(glBindVertexArrayAPPLE) X(glBlendFunc) X(glClear) X(glClearColor) X(glClearDepth) X(glClearStencil) \
 X(glClientActiveTexture) X(glColor4f) X(glColorMask) X(glColorMaterial) X(glColorPointer) \
 X(glCompressedTexImage2DARB) X(glCompressedTexSubImage2DARB) X(glCopyPixels) X(glCullFace) \
 X(glDeleteProgramsARB) X(glDeleteTextures) X(glDepthFunc) X(glDepthMask) X(glDisable) \
 X(glDisableClientState) X(glDrawArrays) X(glDrawBuffer) X(glDrawElements) X(glDrawPixels) X(glEnable) \
 X(glEnableClientState) X(glFinish) X(glFinishObjectAPPLE) X(glFlushVertexArrayRangeAPPLE) X(glFogfv) \
 X(glFogi) X(glFrontFace) X(glGenProgramsARB) X(glGenTextures) X(glGenVertexArraysAPPLE) X(glGetBooleanv) \
 X(glGetIntegerv) X(glGetString) X(glIsEnabled) X(glLightModelfv) X(glLoadIdentity) X(glLoadMatrixf) \
 X(glMaterialfv) X(glMatrixMode) X(glMultMatrixf) X(glNormalPointer) X(glPixelStorei) X(glPolygonMode) X(glPopAttrib) \
 X(glPopMatrix) X(glProgramStringARB) X(glPushAttrib) X(glPushMatrix) X(glRasterPos3f) X(glReadBuffer) \
 X(glReadPixels) X(glScissor) X(glShadeModel) X(glStencilMask) X(glTexCoordPointer) X(glTexEnvf) \
 X(glTexEnvfv) X(glTexEnvi) X(glTexImage2D) X(glTexParameteri) X(glTexSubImage2D) \
 X(glVertexArrayParameteriAPPLE) X(glVertexArrayRangeAPPLE) X(glVertexPointer) X(glViewport) X(glInterleavedArrays) \
 X(glWindowPos2iARB) X(gluOrtho2D) \
 X(aglChoosePixelFormat) X(aglCreateContext) X(aglDescribePixelFormat) X(aglDescribeRenderer) \
 X(aglDestroyContext) X(aglDestroyPixelFormat) X(aglDestroyRendererInfo) X(aglDisable) X(aglEnable) \
 X(aglGetDrawable) X(aglNextRendererInfo) X(aglQueryRendererInfo) X(aglSetCurrentContext) \
 X(aglSetDrawable) X(aglSetFullScreen) X(aglSetInteger) X(aglSwapBuffers) X(aglGetError) \
 X(glGetError)
enum {
    GLOP_NONE = 0,
#define X(n) OP_##n,
    OAH_GL_LIST
#undef X
    GLOP_COUNT
};
static const char *g_glnames[GLOP_COUNT] = {
    "",
#define X(n) #n,
    OAH_GL_LIST
#undef X
};
static void *g_glfp[GLOP_COUNT];
static char g_glres[GLOP_COUNT];
static unsigned long g_glcalls[GLOP_COUNT];
static short g_glop_of_gen[4096];      /* 0 unknown, -1 not GL, else op */
static int g_gltrace = -1, g_glerr;
typedef unsigned int U;
typedef U (*oah_g8_t)(U, U, U, U, U, U, U, U, U);

static void *oah_glfn(int op) {
    if (!g_glres[op]) {
        const char *nm = g_glnames[op];
        void *f = 0;
        g_glres[op] = 1;
        if (!g_ogl_handle) g_ogl_handle = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_GLOBAL);
        if (!g_agl_handle) g_agl_handle = dlopen("/System/Library/Frameworks/AGL.framework/AGL", RTLD_LAZY | RTLD_GLOBAL);
        if (nm[0] == 'a' && g_agl_handle) f = dlsym(g_agl_handle, nm);
        if (!f && g_ogl_handle) f = dlsym(g_ogl_handle, nm);
        if (!f) {   /* try the core name: glActiveTextureARB -> glActiveTexture */
            char alt[64];
            size_t l = strlen(nm);
            if (l > 3 && l < sizeof alt && !strcmp(nm + l - 3, "ARB")) {
                memcpy(alt, nm, l - 3); alt[l - 3] = 0;
                if (g_ogl_handle) f = dlsym(g_ogl_handle, alt);
            }
        }
        if (!f) fprintf(stderr, "[gl] cannot resolve native %s\n", nm);
        g_glfp[op] = f;
    }
    return g_glfp[op];
}
#define GLF(n) oah_glfn(OP_##n)
#define GLFR(n) ((double *)((unsigned char *)R + 0x1e8))[n]
static U oah_garg(U *R, int n) {    /* n-th (0-based) integer argument, incl. stack */
    if (n < 8) return R[3 + n];
    return OSSwapBigToHostInt32(*(U *)(R[1] + 24 + 4 * n));
}
static void oah_bswap32n(U *dst, const void *src, int n) {
    int i;
    const unsigned char *s = (const unsigned char *)src;
    for (i = 0; i < n; i++) { U v; memcpy(&v, s + 4 * i, 4); dst[i] = OSSwapInt32(v); }
}
static int oah_gl_getcount(U pname) {
    switch (pname) {
    case 0x0BA2: case 0x0C10: case 0x0C23: case 0x0C22: case 0x0B66: case 0x0B53:
    case 0x0B00: case 0x0B02: case 0x0B04: case 0x0B06: case 0x0B07: case 0x8005: return 4;
    case 0x0D3A: case 0x0B70: case 0x846D: case 0x846E: case 0x0B12: case 0x0B22: return 2;
    case 0x0BA6: case 0x0BA7: case 0x0BA8: return 16;
    }
    return 1;
}

/* ---- client vertex array emulation ---- */
typedef struct { int size, type, stride, en; U ptr; } oah_arr_t;
#define OAH_NTU 8
/* Normals (glNormalPointer) cannot be passed straight through: Mojave's GL would read the guest's
   big-endian floats, and every lit surface would come out black (blenderplayer). */
#define OAH_ANRM (2 + OAH_NTU)                 /* normal array slot */
#define OAH_SCR_IDX (3 + OAH_NTU)              /* scratch slot for converted indices */
static int oah_no_nrm(void) { static int v = -1; if (v < 0) v = getenv("OAH_NO_NORMALARR") != 0; return v; }
typedef struct { oah_arr_t a[3 + OAH_NTU]; } oah_vao_t;   /* 0 vertex, 1 colour, 2.. texcoord unit, OAH_ANRM normal */
/* The game makes one APPLE VAO per vertex buffer and never deletes them, so the
   table grows on demand (it used to stop at 4096 and hand out id 0, which made
   later models share one VAO and draw with stale pointers). */
#define OAH_NVAO_MAX (1u << 22)
static oah_vao_t g_vao0, *g_vao = &g_vao0, **g_vaos;
static U g_vao_next = 1, g_vao_cap, g_vao_cur;
static oah_vao_t *oah_vao_get(U id) {
    if (id == 0 || id >= OAH_NVAO_MAX) return &g_vao0;
    if (id >= g_vao_cap) {
        U nc = g_vao_cap ? g_vao_cap : 4096;
        oah_vao_t **nv;
        while (nc <= id) nc *= 2;
        nv = (oah_vao_t **)realloc(g_vaos, nc * sizeof *nv);
        if (!nv) return &g_vao0;
        memset(nv + g_vao_cap, 0, (nc - g_vao_cap) * sizeof *nv);
        g_vaos = nv; g_vao_cap = nc;
    }
    if (!g_vaos[id]) g_vaos[id] = (oah_vao_t *)calloc(1, sizeof(oah_vao_t));
    return g_vaos[id] ? g_vaos[id] : &g_vao0;
}
static int g_cat, g_tmax = 1, g_nat_force = 1;
static unsigned g_nat_en;
static struct { unsigned char *p; size_t n; } g_scr[OAH_SCR_IDX + 1];
static int oah_tsz(int t) {
    switch (t) { case 0x1400: case 0x1401: return 1; case 0x1402: case 0x1403: return 2; case 0x140A: return 8; }
    return 4;
}
static unsigned char *oah_scratch(int k, size_t n) {
    if (g_scr[k].n < n) {
        free(g_scr[k].p);
        g_scr[k].n = n + n / 2 + 256;
        g_scr[k].p = (unsigned char *)malloc(g_scr[k].n);
    }
    return g_scr[k].p;
}
static oah_arr_t *oah_arr_for_cap(U cap) {
    if (cap == 0x8074) return &g_vao->a[0];
    if (cap == 0x8076) return &g_vao->a[1];
    if (cap == 0x8075 && !oah_no_nrm()) return &g_vao->a[OAH_ANRM];
    if (cap == 0x8078) return &g_vao->a[2 + g_cat];
    return 0;
}
static void (*g_gl_attr_hook)(int lo, int hi);
static void oah_gl_setup_arrays(int lo, int hi) {
    int k, n = hi - lo + 1, used_tu = 0;
    void (*cat)(U) = (void (*)(U))GLF(glClientActiveTexture);
    void (*ecs)(U) = (void (*)(U))GLF(glEnableClientState);
    void (*dcs)(U) = (void (*)(U))GLF(glDisableClientState);
    if (n <= 0) return;
    for (k = 0; k < 2 + g_tmax || k == OAH_ANRM; k = (k + 1 == 2 + g_tmax) ? OAH_ANRM : k + 1) {
        oah_arr_t *a = &g_vao->a[k];
        U cap = k == 0 ? 0x8074 : k == 1 ? 0x8076 : k == OAH_ANRM ? 0x8075 : 0x8078;
        if (k == OAH_ANRM && oah_no_nrm()) continue;     /* OAH_NO_NORMALARR: passed through natively */
        int want = a->en && a->ptr;
        if (k >= 2 && k != OAH_ANRM && (want || (g_nat_en & (1u << k)) || g_nat_force)) { cat(0x84C0 + k - 2); used_tu = 1; }
        if (want) {
            int ts = oah_tsz(a->type), esz = a->size * ts, st = a->stride ? a->stride : esz, i, j;
            unsigned char *buf = oah_scratch(k, (size_t)n * esz), *d = buf;
            const unsigned char *s = (const unsigned char *)a->ptr + (size_t)lo * st;
            void (*ptrfn)(U, U, U, U) = (void (*)(U, U, U, U))(k == 0 ? GLF(glVertexPointer) : k == 1 ? GLF(glColorPointer) : GLF(glTexCoordPointer));
            if (ts == 1) for (i = 0; i < n; i++, s += st, d += esz) memcpy(d, s, esz);
            else if (ts == 2) for (i = 0; i < n; i++, s += st, d += esz)
                for (j = 0; j < a->size; j++) { unsigned short v; memcpy(&v, s + 2 * j, 2); v = OSSwapInt16(v); memcpy(d + 2 * j, &v, 2); }
            else if (ts == 4) for (i = 0; i < n; i++, s += st, d += esz)
                for (j = 0; j < a->size; j++) { U v; memcpy(&v, s + 4 * j, 4); v = OSSwapInt32(v); memcpy(d + 4 * j, &v, 4); }
            else for (i = 0; i < n; i++, s += st, d += esz)
                for (j = 0; j < a->size; j++) { unsigned long long v; memcpy(&v, s + 8 * j, 8); v = OSSwapInt64(v); memcpy(d + 8 * j, &v, 8); }
            if (k == OAH_ANRM) ((void (*)(U, U, U))GLF(glNormalPointer))(a->type, 0, (U)(buf - (size_t)lo * esz));
            else ptrfn(a->size, a->type, 0, (U)(buf - (size_t)lo * esz));
            if (g_nat_force || !(g_nat_en & (1u << k))) { ecs(cap); g_nat_en |= 1u << k; }
        } else if (g_nat_force || (g_nat_en & (1u << k))) { dcs(cap); g_nat_en &= ~(1u << k); }
    }
    g_nat_force = 0;
    if (used_tu) cat(0x84C0 + g_cat);
    if (g_gl_attr_hook) g_gl_attr_hook(lo, hi);     /* generic vertex attributes (oah_glx.h) */
}

/* ---- AGL helpers ---- */
static int oah_agl_valued(U t) {
    return t == 2 || t == 3 || t == 7 || (t >= 8 && t <= 17) || t == 50 || t == 55 || t == 56 ||
           t == 70 || t == 82 || t == 84;
}
/* SimCity 4 asks for a 16-bit depth buffer.  Hypothesis (unverified): the drivers it was
   tested on rounded that up to 24 bits at 32-bit colour, while Mojave's driver may give a
   literal 16, which would explain models clipping and the green "hidden behind something"
   pass showing on visible models.  Ask for at least 24; the depth actually obtained is
   logged.  OAH_GL_DEPTH=n overrides (0 = keep the game's value). */
static int oah_gl_depth_bits(void) {
    static int v = -2;
    if (v == -2) { const char *e = getenv("OAH_GL_DEPTH"); v = e ? atoi(e) : 24; }
    return v;
}
/* strip: 1 drop AGL_FULLSCREEN, 2 drop AGL_RENDERER_ID/SINGLE_RENDERER/ACCELERATED-independent
   selectors (70,71,72), 4 drop AGL_ALL_RENDERERS(1) */
static int oah_agl_attribs(U guest, U *out, int max, int strip, char *desc, size_t dl) {
    int n = 0, i = 0;
    size_t dp = 0;
    desc[0] = 0;
    if (!guest) return -1;
    for (;;) {
        U t = OSSwapBigToHostInt32(((U *)guest)[i++]), v = 0;
        int hasv = oah_agl_valued(t);
        if (t == 0 || i > 120) break;
        if (hasv) v = OSSwapBigToHostInt32(((U *)guest)[i++]);
        if (dp + 16 < dl) dp += snprintf(desc + dp, dl - dp, hasv ? "%u=%u " : "%u ", t, v);
        if ((strip & 1) && t == 54) continue;
        if ((strip & 2) && (t == 70 || t == 71 || t == 72)) continue;
        if ((strip & 4) && t == 1) continue;
        if (t == 12 && hasv) {   /* AGL_DEPTH_SIZE: see oah_gl_depth_bits() */
            int want = oah_gl_depth_bits();
            if (want > 0 && (int)v < want) v = (U)want;
        }
        if (n + 3 >= max) break;
        out[n++] = t;
        if (hasv) out[n++] = v;
    }
    out[n] = 0;
    return n;
}
/* A guest GDHandle that is not one of our shadows (guest-toolbox mode: the guest's own QuickDraw
   made it) was handed to native AGL as is, which read the guest memory as a native GDevice and
   crashed in _dspyMaskFromDevice (Bejeweled 2). Match its gdRect to a native device instead. */
static U oah_native_gdev_for_guest(U g) {
    static U (*gmd)(void), (*gdl)(void), (*gnd)(U);
    static int init;
    U gp, nd;
    short t, l, b, r;
    if (!init) {
        void *qd = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/QD.framework/QD", RTLD_LAZY | RTLD_GLOBAL);
        init = 1;
        if (qd) { gmd = (U (*)(void))dlsym(qd, "GetMainDevice"); gdl = (U (*)(void))dlsym(qd, "GetDeviceList"); gnd = (U (*)(U))dlsym(qd, "GetNextDevice"); }
    }
    if (!g || !(gp = OSSwapBigToHostInt32(*(U *)g))) return gmd ? gmd() : 0;
    t = (short)OSSwapBigToHostInt16(*(unsigned short *)(gp + 34)); l = (short)OSSwapBigToHostInt16(*(unsigned short *)(gp + 36));
    b = (short)OSSwapBigToHostInt16(*(unsigned short *)(gp + 38)); r = (short)OSSwapBigToHostInt16(*(unsigned short *)(gp + 40));
    for (nd = gdl ? gdl() : 0; nd && gnd; nd = gnd(nd)) {
        short *nr = (short *)(*(char **)nd + 34);   /* native GDevice, host byte order */
        if (nr[0] == t && nr[1] == l && nr[2] == b && nr[3] == r) return nd;
    }
    return gmd ? gmd() : 0;
}
static void oah_map_gdevs(U guest, int nd, U *out) {
    int i;
    for (i = 0; i < nd && i < 16; i++) {
        U g = OSSwapBigToHostInt32(((U *)guest)[i]), n = oah_native_handle(g);
        out[i] = n != g ? n : oah_native_gdev_for_guest(g);
    }
}
static U g_gdraw, g_gread; static int g_aux_sp;   /* defined with the AUX0 emulation below */
#define OAH_NCTX 16
static U g_ctx_init[OAH_NCTX], g_cur_ctx;
/* OAH_GLCTXTRACE=k: log every GL call made while the k-th context (in first-current order)
   is current, up to OAH_GLCTXTRACE_MAX (default 4000) lines */
static U g_trace_ctx; static int g_nctx_seen;
static int oah_ctxtrace(void) {
    static int max = -1, n;
    if (!g_trace_ctx || (g_trace_ctx != 0xffffffffu && g_cur_ctx != g_trace_ctx)) return 0;
    if (max < 0) { const char *m = getenv("OAH_GLCTXTRACE_MAX"); max = m ? atoi(m) : 4000; }
    return n++ < max;
}
static void oah_gl_ctx_made_current(U ctx) {
    int i;
    void (*ps)(U, U) = (void (*)(U, U))GLF(glPixelStorei);
    if (ctx != g_cur_ctx) g_nat_force = 1;
    g_cur_ctx = ctx;
    if (!ctx) return;
    for (i = 0; i < OAH_NCTX; i++) if (g_ctx_init[i] == ctx) return;
    {   const char *t = getenv("OAH_GLCTXTRACE");
        ++g_nctx_seen;
        if (t && !strcmp(t, "all")) g_trace_ctx = 0xffffffffu;
        if (g_nctx_seen == (t ? atoi(t) : -1)) { g_trace_ctx = ctx; fprintf(stderr, "[glctx] tracing context #%d %08x\n", g_nctx_seen, ctx); } }
    for (i = 0; i < OAH_NCTX; i++) if (!g_ctx_init[i]) { g_ctx_init[i] = ctx; break; }
    g_gdraw = g_gread = 0x0405; g_aux_sp = 0;   /* new context: default buffers (see AUX0 emulation) */
    ps(0x0CF0, 1);    /* GL_UNPACK_SWAP_BYTES */
    ps(0x0D00, 1);    /* GL_PACK_SWAP_BYTES */
    {
        const char *(*gs)(U) = (const char *(*)(U))GLF(glGetString);
        fprintf(stderr, "[gl] context %08x first current: vendor=\"%s\" renderer=\"%s\" version=\"%s\"\n",
                ctx, gs(0x1F00), gs(0x1F01), gs(0x1F02));
    }
}

/* ---- GL_AUX0 emulation ----
   SimCity 4 caches its static city view in AUX0 (pixel format: AGL_AUX_BUFFERS=1 +
   AGL_AUX_DEPTH_STENCIL).  Each frame it copies AUX0's colour and then its depth back into
   the back buffer with glCopyPixels and draws the moving models (cars, disaster robot, the
   green "hidden behind something" pass) on top, depth-tested against that restored depth.
   If the driver's aux buffer has no depth of its own, the restored depth is whatever the
   last frame left, so models clip against stale depth.  Here AUX0 is a framebuffer object
   with its own RGBA8 colour and 24-bit depth renderbuffers: guest glDrawBuffer/glReadBuffer
   (GL_AUX0) bind it as the draw/read framebuffer, so glCopyPixels, glClear and drawing keep
   their normal GL semantics (EXT_framebuffer_blit makes CopyPixels read the read FB and
   write the draw FB).  OAH_NO_AUXFBO=1 passes AUX0 to the driver as before. */
#define OAH_AUX0 0x0409
static U g_aux_fbo, g_aux_rb[2], g_aux_ctx, g_aux_w, g_aux_h;
static U g_gdraw = 0x0405, g_gread = 0x0405;     /* guest-visible draw / read buffer */
static int g_aux_on = -1;
static U g_aux_stack[32][3]; static int g_aux_sp;
static void *oah_gl_ext(const char *nm) {
    if (!g_ogl_handle) g_ogl_handle = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_GLOBAL);
    return g_ogl_handle ? dlsym(g_ogl_handle, nm) : 0;
}
static void (*p_glBindFB)(U, U), (*p_glGenFB)(U, U *), (*p_glGenRB)(U, U *), (*p_glBindRB)(U, U);
static void (*p_glRBStorage)(U, U, U, U), (*p_glFBRB)(U, U, U, U);
static U (*p_glFBStatus)(U);
static int oah_aux_enabled(void) {
    if (g_aux_on < 0) {
        g_aux_on = !getenv("OAH_NO_AUXFBO");
        p_glBindFB = (void (*)(U, U))oah_gl_ext("glBindFramebufferEXT");
        p_glGenFB = (void (*)(U, U *))oah_gl_ext("glGenFramebuffersEXT");
        p_glGenRB = (void (*)(U, U *))oah_gl_ext("glGenRenderbuffersEXT");
        p_glBindRB = (void (*)(U, U))oah_gl_ext("glBindRenderbufferEXT");
        p_glRBStorage = (void (*)(U, U, U, U))oah_gl_ext("glRenderbufferStorageEXT");
        p_glFBRB = (void (*)(U, U, U, U))oah_gl_ext("glFramebufferRenderbufferEXT");
        p_glFBStatus = (U (*)(U))oah_gl_ext("glCheckFramebufferStatusEXT");
        if (!p_glBindFB || !p_glGenFB || !p_glGenRB || !p_glBindRB || !p_glRBStorage || !p_glFBRB || !p_glFBStatus) {
            fprintf(stderr, "[gl] AUX0 emulation unavailable (framebuffer object entry points missing)\n");
            g_aux_on = 0;
        }
    }
    return g_aux_on;
}
static int oah_aux_ensure(void) {      /* framebuffer object for the current context */
    U st, vp[4] = { 0, 0, 0, 0 };
    if (!oah_aux_enabled() || !g_cur_ctx) return 0;
    if (g_aux_fbo && g_aux_ctx == g_cur_ctx) return 1;
    ((void (*)(U, U *))GLF(glGetIntegerv))(0x0BA2, vp);          /* GL_VIEWPORT: drawable size */
    g_aux_w = vp[2] > 2048 ? vp[2] : 2048; g_aux_h = vp[3] > 2048 ? vp[3] : 2048;
    p_glGenFB(1, &g_aux_fbo);
    p_glGenRB(2, g_aux_rb);
    p_glBindRB(0x8D41, g_aux_rb[0]); p_glRBStorage(0x8D41, 0x8058, g_aux_w, g_aux_h);   /* RGBA8 */
    p_glBindRB(0x8D41, g_aux_rb[1]); p_glRBStorage(0x8D41, 0x81A6, g_aux_w, g_aux_h);   /* DEPTH_COMPONENT24 */
    p_glBindRB(0x8D41, 0);
    p_glBindFB(0x8D40, g_aux_fbo);
    p_glFBRB(0x8D40, 0x8CE0, 0x8D41, g_aux_rb[0]);
    p_glFBRB(0x8D40, 0x8D00, 0x8D41, g_aux_rb[1]);
    ((void (*)(U))GLF(glDrawBuffer))(0x8CE0);
    ((void (*)(U))GLF(glReadBuffer))(0x8CE0);
    st = p_glFBStatus(0x8D40);
    p_glBindFB(0x8D40, 0);
    g_aux_ctx = g_cur_ctx;
    fprintf(stderr, "[gl] AUX0 -> framebuffer object %u (%ux%u, RGBA8 + depth24), status 0x%04x%s\n",
            g_aux_fbo, g_aux_w, g_aux_h, st, st == 0x8CD5 ? " complete" : " INCOMPLETE, emulation off");
    if (st != 0x8CD5) { g_aux_on = 0; return 0; }
    return 1;
}
/* Make the native bindings match the guest's draw / read buffer. */
static void oah_aux_apply(void) {
    int dfbo = g_gdraw == OAH_AUX0, rfbo = g_gread == OAH_AUX0;
    if ((dfbo || rfbo) && !oah_aux_ensure()) dfbo = rfbo = 0;
    if (g_aux_on > 0 && g_aux_fbo) {
        p_glBindFB(0x8CA9, dfbo ? g_aux_fbo : 0);     /* DRAW_FRAMEBUFFER_EXT */
        p_glBindFB(0x8CA8, rfbo ? g_aux_fbo : 0);     /* READ_FRAMEBUFFER_EXT */
    }
    if (!dfbo) ((void (*)(U))GLF(glDrawBuffer))(g_gdraw);
    if (!rfbo) ((void (*)(U))GLF(glReadBuffer))(g_gread);
}
static int oah_aux_bound(void) { return g_aux_on > 0 && g_aux_fbo && (g_gdraw == OAH_AUX0 || g_gread == OAH_AUX0); }

/* Returns 1 when the call was handled. */
static int oah_gl_call(unsigned int idx, oah_gen_t *g, U *R) {
    int op;
    U a0 = R[3], a1 = R[4], a2 = R[5], a3 = R[6];
    void *fn;
    if (idx >= sizeof g_glop_of_gen / sizeof g_glop_of_gen[0]) return 0;
    op = g_glop_of_gen[idx];
    if (op < 0) return 0;
    if (op == 0) {
        int k;
        g_glop_of_gen[idx] = -1;
        if (g->name[0] != 'g' && g->name[0] != 'a') return 0;
        for (k = 1; k < GLOP_COUNT; k++) if (!strcmp(g_glnames[k], g->name)) { g_glop_of_gen[idx] = (short)k; break; }
        if (g_glop_of_gen[idx] < 0) return 0;
        op = g_glop_of_gen[idx];
    }
    if (g_gltrace < 0) { g_gltrace = getenv("OAH_GLTRACE") != 0; g_glerr = getenv("OAH_GLERR") != 0; }
    g_glcalls[op]++;
    if (oah_ctxtrace()) fprintf(stderr, "[glctx] %08x %s(%x, %x, %x, %x, %x, %x)\n", g_cur_ctx, g_glnames[op], a0, a1, a2, a3, R[7], R[8]);
    if (g_gltrace && g_glcalls[op] <= 3)
        fprintf(stderr, "[gl] %s #%lu r3=%08x r4=%08x r5=%08x r6=%08x\n", g_glnames[op], g_glcalls[op], a0, a1, a2, a3);
    fn = oah_glfn(op);
    {   /* OAH_GLFMT=1: log each distinct texture format / alpha / blend setting once */
        static int fmt = -1; static U seen[512][4]; static int nseen;
        if (fmt < 0) fmt = getenv("OAH_GLFMT") != 0;
        if (fmt && (op == OP_glTexImage2D || op == OP_glCompressedTexImage2DARB || op == OP_glAlphaFunc ||
                    op == OP_glBlendFunc || op == OP_glTexEnvi || op == OP_glTexEnvf || op == OP_glEnable ||
                    op == OP_glDisable || op == OP_glTexParameteri || op == OP_glColorPointer)) {
            U k0 = op, k1 = a0, k2 = a1, k3 = a2; int i, dup = 0;
            if (op == OP_glTexImage2D) { k1 = a2; k2 = R[9]; k3 = R[10]; }      /* internalfmt, format, type */
            if (op == OP_glCompressedTexImage2DARB) { k1 = a2; k2 = 0; k3 = 0; }
            if (op == OP_glColorPointer) { k1 = a0; k2 = a1; k3 = 0; }
            if (op == OP_glAlphaFunc || op == OP_glTexEnvf) { float f = (float)GLFR(1); memcpy(&k3, &f, 4); }
            for (i = 0; i < nseen; i++) if (seen[i][0] == k0 && seen[i][1] == k1 && seen[i][2] == k2 && seen[i][3] == k3) { dup = 1; break; }
            if (!dup && nseen < 512) {
                seen[nseen][0] = k0; seen[nseen][1] = k1; seen[nseen][2] = k2; seen[nseen][3] = k3; nseen++;
                if (op == OP_glAlphaFunc || op == OP_glTexEnvf) { float f; memcpy(&f, &k3, 4);
                    fprintf(stderr, "[glfmt] %s(%04x, %04x, %g)\n", g_glnames[op], a0, op == OP_glTexEnvf ? a1 : 0, f); }
                else if (op == OP_glTexImage2D)
                    fprintf(stderr, "[glfmt] glTexImage2D internal=%04x format=%04x type=%04x (%ux%u)\n", k1, k2, k3, R[6], R[7]);
                else fprintf(stderr, "[glfmt] %s(%04x, %04x, %04x)\n", g_glnames[op], k1, k2, k3);
            }
        }
    }
    {   /* OAH_GLPIX=1: log each distinct buffer / depth / pixel-copy / program call once,
           and count them per 1024 frames */
        static int pix = -1; static U seen[768][5]; static int nseen;
        static unsigned long cnt[GLOP_COUNT];
        if (pix < 0) pix = getenv("OAH_GLPIX") != 0;
        if (pix) {
            int want = op == OP_glDrawBuffer || op == OP_glReadBuffer || op == OP_glCopyPixels ||
                       op == OP_glReadPixels || op == OP_glDrawPixels || op == OP_glWindowPos2iARB ||
                       op == OP_glDepthFunc || op == OP_glDepthMask || op == OP_glPolygonMode ||
                       op == OP_glColorMask || op == OP_glStencilMask || op == OP_glBindProgramARB ||
                       op == OP_glClear || op == OP_glClearDepth || op == OP_glCompressedTexSubImage2DARB ||
                       op == OP_glTexSubImage2D || op == OP_glCullFace || op == OP_glFrontFace ||
                       op == OP_glScissor || op == OP_glViewport || op == OP_glPushAttrib ||
                       op == OP_glEnable || op == OP_glDisable || op == OP_glDeleteTextures;
            if (want) {
                U k[5] = { op, a0, a1, a2, R[7] }; int i, dup = 0;
                cnt[op]++;
                if (op == OP_glBindProgramARB || op == OP_glWindowPos2iARB || op == OP_glDeleteTextures) k[2] = k[3] = k[4] = 0;
                if (op == OP_glBindProgramARB) k[2] = a1 ? 1 : 0;
                if (op == OP_glScissor || op == OP_glViewport) { k[1] = k[2] = 0; }
                if (op == OP_glTexSubImage2D || op == OP_glCompressedTexSubImage2DARB) { k[1] = a1; k[2] = R[9]; k[3] = R[10]; k[4] = 0; }
                for (i = 0; i < nseen; i++) if (!memcmp(seen[i], k, sizeof k)) { dup = 1; break; }
                if (!dup && nseen < 768) {
                    memcpy(seen[nseen++], k, sizeof k);
                    fprintf(stderr, "[glpix] %s(%x, %x, %x, %x, %x) frame %lu\n", g_glnames[op], a0, a1, a2, a3, R[7], g_glcalls[OP_aglSwapBuffers]);
                }
            }
            if (op == OP_aglSwapBuffers && (g_glcalls[op] & 1023) == 0) {
                int j;
                fprintf(stderr, "[glpix] counts at frame %lu:", g_glcalls[op]);
                for (j = 1; j < GLOP_COUNT; j++) if (cnt[j]) fprintf(stderr, " %s=%lu", g_glnames[j], cnt[j]);
                fprintf(stderr, "\n");
                memset(cnt, 0, sizeof cnt);
            }
        }
    }
    switch (op) {
    /* ---- AUX0 emulation (see oah_aux_apply) ---- */
    case OP_glDrawBuffer: case OP_glReadBuffer:
        if (!oah_aux_enabled()) { ((void (*)(U))fn)(a0); break; }
        if (op == OP_glDrawBuffer) g_gdraw = a0; else g_gread = a0;
        oah_aux_apply();
        break;
    case OP_glPushAttrib:
        if (getenv("OAH_GLATTR")) {
            int d = -1; void (*gi)(U, int *) = (void (*)(U, int *))dlsym(RTLD_DEFAULT, "glGetIntegerv");
            U (*cur)(void) = (U (*)(void))dlsym(RTLD_DEFAULT, "CGLGetCurrentContext");
            if (gi) gi(0x0BB0, &d);
            {   static int n; if (n++ < 60) fprintf(stderr, "[glattr] push %08x ctx %08x depth %d\n", a0, cur ? cur() : 0, d); }
        }
        if (oah_aux_enabled()) {
            int b = oah_aux_bound();
            if (b) { p_glBindFB(0x8CA9, 0); p_glBindFB(0x8CA8, 0); }   /* push the default framebuffer's buffers */
            ((void (*)(U))fn)(a0);
            if (g_aux_sp < 32) { g_aux_stack[g_aux_sp][0] = a0; g_aux_stack[g_aux_sp][1] = g_gdraw; g_aux_stack[g_aux_sp][2] = g_gread; }
            g_aux_sp++;
            if (b) oah_aux_apply();
        } else ((void (*)(U))fn)(a0);
        break;
    case OP_glPopAttrib:
        {   static int at = -1; if (at < 0) at = getenv("OAH_GLATTR") ? 1 : 0;
            if (at) {
                int d = -1; U pre = ((U (*)(void))GLF(glGetError))();
                void (*gi)(U, int *) = (void (*)(U, int *))dlsym(RTLD_DEFAULT, "glGetIntegerv");
                U (*cur)(void) = (U (*)(void))dlsym(RTLD_DEFAULT, "CGLGetCurrentContext");
                if (gi) gi(0x0BB0, &d);
                {   static int n; if (n++ < 60) fprintf(stderr, "[glattr] pop ctx %08x depth %d pending err %04x\n", cur ? cur() : 0, d, pre); }
            }
        }
        if (oah_aux_enabled() && g_aux_sp > 0) {
            g_aux_sp--;
            if (g_aux_fbo && g_aux_ctx == g_cur_ctx) { p_glBindFB(0x8CA9, 0); p_glBindFB(0x8CA8, 0); }
            ((void (*)(void))fn)();
            if (g_aux_sp < 32) {
                if (g_aux_stack[g_aux_sp][0] & 0x4000) g_gdraw = g_aux_stack[g_aux_sp][1];   /* COLOR_BUFFER_BIT */
                if (g_aux_stack[g_aux_sp][0] & 0x0020) g_gread = g_aux_stack[g_aux_sp][2];   /* PIXEL_MODE_BIT */
            }
            oah_aux_apply();
        } else ((void (*)(void))fn)();
        break;
    /* ---- emulated / tracked ---- */
    case OP_glBindVertexArrayAPPLE:
        g_vao = oah_vao_get(a0);
        g_vao_cur = g_vao == &g_vao0 ? 0 : a0;
        return 1;
    case OP_glGenVertexArraysAPPLE: {
        int i;
        for (i = 0; i < (int)a0; i++) {
            U id = g_vao_next < OAH_NVAO_MAX ? g_vao_next++ : 0;
            ((U *)a1)[i] = OSSwapHostToBigInt32(id);
            if (id && (id & (id - 1)) == 0 && id >= 1024)
                fprintf(stderr, "[gl] %u vertex array objects generated\n", id);
        }
        return 1;
    }
    case OP_glVertexArrayRangeAPPLE: case OP_glFlushVertexArrayRangeAPPLE:
    case OP_glVertexArrayParameteriAPPLE: case OP_glFinishObjectAPPLE:
        return 1;
    case OP_glVertexPointer: case OP_glColorPointer: case OP_glTexCoordPointer: {
        oah_arr_t *a = &g_vao->a[op == OP_glVertexPointer ? 0 : op == OP_glColorPointer ? 1 : 2 + g_cat];
        a->size = (int)a0; a->type = (int)a1; a->stride = (int)a2; a->ptr = a3;
        return 1;
    }
    case OP_glInterleavedArrays: {
        /* glInterleavedArrays(format, stride, pointer): one call that sets the vertex, colour, normal and
           (active unit's) texture coordinate arrays and turns them on or off. Passed through, the native
           GL read the guest's big-endian floats, and the arrays it turned on were turned off again at
           the next glDrawArrays (only the recorded ones are kept). iWork '09 draws the selection
           handles of shapes, text boxes and pictures this way (SFDCanvas drawQueuedDecorations, format
           T2F_V3F): there were none. Here it becomes the separate pointer calls, as the spec defines
           it, and the arrays are converted at draw time like the others. */
        static const struct { U fmt; signed char st, sc, ctype, sv, pc, pn, pv, sz; } F[] = {
            /* fmt     st sc  c  sv  pc  pn  pv  size (in 4-byte units; -1 none; c: 0 float, 1 ubyte) */
            { 0x2A20, 0, 0, 0, 2, -1, -1, 0, 2 },     /* V2F */
            { 0x2A21, 0, 0, 0, 3, -1, -1, 0, 3 },     /* V3F */
            { 0x2A22, 0, 4, 1, 2, 0, -1, 1, 3 },      /* C4UB_V2F */
            { 0x2A23, 0, 4, 1, 3, 0, -1, 1, 4 },      /* C4UB_V3F */
            { 0x2A24, 0, 3, 0, 3, 0, -1, 3, 6 },      /* C3F_V3F */
            { 0x2A25, 0, 0, 0, 3, -1, 0, 3, 6 },      /* N3F_V3F */
            { 0x2A26, 0, 4, 0, 3, 0, 4, 7, 10 },      /* C4F_N3F_V3F */
            { 0x2A27, 2, 0, 0, 3, -1, -1, 2, 5 },     /* T2F_V3F */
            { 0x2A28, 4, 0, 0, 4, -1, -1, 4, 8 },     /* T4F_V4F */
            { 0x2A29, 2, 4, 1, 3, 2, -1, 3, 6 },      /* T2F_C4UB_V3F */
            { 0x2A2A, 2, 3, 0, 3, 2, -1, 5, 8 },      /* T2F_C3F_V3F */
            { 0x2A2B, 2, 0, 0, 3, -1, 2, 5, 8 },      /* T2F_N3F_V3F */
            { 0x2A2C, 2, 4, 0, 3, 2, 6, 9, 12 },      /* T2F_C4F_N3F_V3F */
            { 0x2A2D, 4, 4, 0, 4, 4, 8, 11, 15 },     /* T4F_C4F_N3F_V4F */
        };
        int i, k = -1;
        for (i = 0; i < (int)(sizeof F / sizeof F[0]); i++) if (F[i].fmt == a0) { k = i; break; }
        if (k < 0) { if (fn) ((void (*)(U, U, U))fn)(a0, a1, a2); return 1; }   /* GL_INVALID_ENUM from the native GL */
        {
            int st = (int)a1 ? (int)a1 : 4 * F[k].sz;
            void (*dcs)(U) = (void (*)(U))GLF(glDisableClientState);
            oah_arr_t *v = &g_vao->a[0], *c = &g_vao->a[1], *t = &g_vao->a[2 + g_cat], *n = &g_vao->a[OAH_ANRM];
            v->size = F[k].sv; v->type = 0x1406; v->stride = st; v->ptr = a2 + 4 * F[k].pv; v->en = 1;
            c->en = F[k].sc > 0;
            if (c->en) { c->size = F[k].sc; c->type = F[k].ctype ? 0x1401 : 0x1406; c->stride = st; c->ptr = a2 + 4 * F[k].pc; }
            t->en = F[k].st > 0;
            if (t->en) { t->size = F[k].st; t->type = 0x1406; t->stride = st; t->ptr = a2; if (g_cat + 1 > g_tmax) g_tmax = g_cat + 1; }
            if (oah_no_nrm()) {   /* OAH_NO_NORMALARR: normals stay native */
                void (*np)(U, U, U) = (void (*)(U, U, U))GLF(glNormalPointer);
                if (F[k].pn >= 0) { if (np) np(0x1406, st, a2 + 4 * F[k].pn); ((void (*)(U))GLF(glEnableClientState))(0x8075); }
                else if (dcs) dcs(0x8075);
            } else {
                n->en = F[k].pn >= 0;
                if (n->en) { n->size = 3; n->type = 0x1406; n->stride = st; n->ptr = a2 + 4 * F[k].pn; }
            }
            /* edge flag, index, fog coordinate and secondary colour arrays: off, as the spec says */
            if (dcs) { dcs(0x8079); dcs(0x8077); dcs(0x8457); dcs(0x845E); }
        }
        return 1;
    }
    case OP_glNormalPointer: {
        oah_arr_t *a = &g_vao->a[OAH_ANRM];
        if (oah_no_nrm()) { if (fn) ((void (*)(U, U, U))fn)(a0, a1, a2); return 1; }
        a->size = 3; a->type = (int)a0; a->stride = (int)a1; a->ptr = a2;
        return 1;
    }
    case OP_glEnableClientState: case OP_glDisableClientState: {
        oah_arr_t *a = oah_arr_for_cap(a0);
        if (a) {
            a->en = op == OP_glEnableClientState;
            if (a0 == 0x8078 && a->en && g_cat + 1 > g_tmax) g_tmax = g_cat + 1;
        } else if (a0 != 0x851D && fn) ((void (*)(U))fn)(a0);   /* skip VERTEX_ARRAY_RANGE_APPLE */
        return 1;
    }
    case OP_glClientActiveTexture:
        if (a0 >= 0x84C0 && a0 < 0x84C0 + OAH_NTU) g_cat = (int)(a0 - 0x84C0);
        if (fn) ((void (*)(U))fn)(a0);
        return 1;
    case OP_glIsEnabled: {
        oah_arr_t *a = oah_arr_for_cap(a0);
        R[3] = a ? (U)(a->en != 0) : (fn ? (((U (*)(U))fn)(a0) & 0xff) : 0);
        return 1;
    }
    case OP_glDrawArrays:
        oah_gl_setup_arrays((int)a1, (int)a1 + (int)a2 - 1);
        ((void (*)(U, U, U))fn)(a0, a1, a2);
        break;
    case OP_glDrawElements: {
        int n = (int)a1, i, lo = 0x7fffffff, hi = -1, isz = a2 == 0x1401 ? 1 : a2 == 0x1403 ? 2 : 4;
        unsigned char *ib;
        if (n <= 0 || !a3) return 1;
        ib = oah_scratch(OAH_SCR_IDX, (size_t)n * isz);
        for (i = 0; i < n; i++) {
            U v;
            if (isz == 1) v = ((unsigned char *)a3)[i];
            else if (isz == 2) { unsigned short s; memcpy(&s, (unsigned char *)a3 + 2 * i, 2); v = OSSwapInt16(s); ((unsigned short *)ib)[i] = (unsigned short)v; }
            else { memcpy(&v, (unsigned char *)a3 + 4 * i, 4); v = OSSwapInt32(v); ((U *)ib)[i] = v; }
            if (isz == 1) ib[i] = (unsigned char)v;
            if ((int)v < lo) lo = (int)v;
            if ((int)v > hi) hi = (int)v;
        }
        oah_gl_setup_arrays(lo, hi);
        ((void (*)(U, U, U, U))fn)(a0, a1, a2, (U)ib);
        break;
    }
    case OP_glPixelStorei:
        ((void (*)(U, U))fn)(a0, (a0 == 0x0CF0 || a0 == 0x0D00) ? !a1 : a1);
        break;
    /* ---- float / double arguments ---- */
    case OP_glAlphaFunc: ((void (*)(U, float))fn)(a0, (float)GLFR(1)); break;
    case OP_glClearColor: case OP_glColor4f:
        ((void (*)(float, float, float, float))fn)((float)GLFR(1), (float)GLFR(2), (float)GLFR(3), (float)GLFR(4)); break;
    case OP_glClearDepth: ((void (*)(double))fn)(GLFR(1)); break;
    case OP_glRasterPos3f: ((void (*)(float, float, float))fn)((float)GLFR(1), (float)GLFR(2), (float)GLFR(3)); break;
    case OP_glTexEnvf: ((void (*)(U, U, float))fn)(a0, a1, (float)GLFR(1)); break;
    case OP_gluOrtho2D: ((void (*)(double, double, double, double))fn)(GLFR(1), GLFR(2), GLFR(3), GLFR(4)); break;
    /* ---- small arrays in ---- */
    case OP_glLoadMatrixf: case OP_glMultMatrixf: {
        U m[16];
        oah_bswap32n(m, (void *)a0, 16);
        ((void (*)(U *))fn)(m);
        break;
    }
    case OP_glFogfv: case OP_glLightModelfv: {
        U v[4];
        int c = (a0 == 0x0B66 || a0 == 0x0B53) ? 4 : 1;
        oah_bswap32n(v, (void *)a1, c);
        ((void (*)(U, U *))fn)(a0, v);
        break;
    }
    case OP_glTexEnvfv: {
        U v[4];
        oah_bswap32n(v, (void *)a2, a1 == 0x2201 ? 4 : 1);
        ((void (*)(U, U, U *))fn)(a0, a1, v);
        break;
    }
    case OP_glMaterialfv: {
        U v[4];
        oah_bswap32n(v, (void *)a2, a1 == 0x1601 ? 1 : a1 == 0x1603 ? 3 : 4);
        ((void (*)(U, U, U *))fn)(a0, a1, v);
        break;
    }
    case OP_glDeleteTextures: case OP_glDeleteProgramsARB: {
        int n = (int)a0;
        U tmp[256], *t = n <= 256 ? tmp : (U *)malloc(4 * (size_t)n);
        if (n <= 0) return 1;
        oah_bswap32n(t, (void *)a1, n);
        ((void (*)(U, U *))fn)(a0, t);
        if (t != tmp) free(t);
        break;
    }
    /* ---- arrays out ---- */
    case OP_glGenTextures: case OP_glGenProgramsARB: {
        int n = (int)a0, i;
        U tmp[256], *t = n <= 256 ? tmp : (U *)malloc(4 * (size_t)n);
        if (n <= 0) return 1;
        ((void (*)(U, U *))fn)(a0, t);
        for (i = 0; i < n; i++) ((U *)a1)[i] = OSSwapHostToBigInt32(t[i]);
        if (t != tmp) free(t);
        break;
    }
    case OP_glGetIntegerv: case OP_glGetBooleanv: {
        U tmp[512];
        int c = oah_gl_getcount(a0), i;
        memset(tmp, 0, sizeof tmp);
        if ((a0 == 0x0C01 || a0 == 0x0C02) && g_aux_on > 0) {   /* DRAW_BUFFER / READ_BUFFER: guest view (AUX0 is emulated) */
            U v = a0 == 0x0C01 ? g_gdraw : g_gread;
            if (op == OP_glGetBooleanv) *(unsigned char *)a1 = v != 0; else *(U *)a1 = OSSwapHostToBigInt32(v);
            return 1;
        }
        if (a0 == 0x85B5) {   /* GL_VERTEX_ARRAY_BINDING_APPLE: VAOs are emulated, answer from our state */
            if (op == OP_glGetBooleanv) *(unsigned char *)a1 = g_vao_cur != 0;
            else *(U *)a1 = OSSwapHostToBigInt32(g_vao_cur);
            return 1;
        }
        if (a0 == 0x86A3) {
            U nf = 0;
            ((void (*)(U, U *))GLF(glGetIntegerv))(0x86A2, &nf);
            c = nf > 500 ? 500 : (int)nf;
        }
        ((void (*)(U, U *))fn)(a0, tmp);
        if (op == OP_glGetBooleanv) memcpy((void *)a1, tmp, c);
        else for (i = 0; i < c; i++) ((U *)a1)[i] = OSSwapHostToBigInt32(tmp[i]);
        break;
    }
    case OP_glGetString:
        R[3] = fn ? (U)((const char *(*)(U))fn)(a0) : 0;
        if (g_glcalls[op] <= 8) fprintf(stderr, "[gl] glGetString(%04x) -> \"%.200s\"\n", a0, R[3] ? (char *)R[3] : "(null)");
        return 1;
    /* ---- nine arguments (9th on the PPC stack) ---- */
    case OP_glTexImage2D: case OP_glTexSubImage2D: {
        /* GL_UNPACK_SWAP_BYTES is on in every context (big-endian guest data), and with it every
           upload went through the driver's generic pixel processor (glgProcessPixelsWithProcessor):
           Keynote's theme-chooser zoom spent ~10 s there. Bytes need no swapping, and swapping the
           bytes of a packed 32-bit pixel is the same as reading it as the other of 8_8_8_8 /
           8_8_8_8_REV: those uploads go with the swap off (and the type flipped), on the driver's
           fast path. Only while the guest itself has not asked for swapped bytes (native swap on).
           OAH_NO_TEXFAST=1: as before. */
        static int nofast = -1, nlog;
        U type = R[10], sw = 0;
        int fast = 0;
        if (nofast < 0) nofast = getenv("OAH_NO_TEXFAST") ? 1 : 0;
        if (!nofast && (type == 0x1401 || type == 0x1400 || type == 0x8035 || type == 0x8367)) {
            ((void (*)(U, U *))GLF(glGetIntegerv))(0x0CF0, &sw);
            if (sw) {
                U pbo = 0, cs = 0;
                fast = 1;
                ((void (*)(U, U))GLF(glPixelStorei))(0x0CF0, 0);
                ((void (*)(U, U *))GLF(glGetIntegerv))(0x88EF, &pbo);          /* GL_PIXEL_UNPACK_BUFFER_BINDING */
                /* GL_UNPACK_CLIENT_STORAGE_APPLE: GL keeps using the caller's memory after the call,
                   so it must stay the guest's own pixels, not our reused scratch copy (with that,
                   Keynote's slide shows a band of another texture's bytes) */
                ((void (*)(U, U *))GLF(glGetIntegerv))(0x85B2, &cs);
                if ((type == 0x8035 || type == 0x8367) && !pbo && !cs && oah_garg(R, 8)) {
                    /* the driver converts 8_8_8_8 in software too: swap the words here and upload
                       them in native order (BGRA + 8_8_8_8_REV is its fast path) */
                    U w = op == OP_glTexImage2D ? R[6] : R[7], h = op == OP_glTexImage2D ? R[7] : R[8];
                    U rl = 0, orl, sr = 0, sp = 0, y, x;
                    static unsigned int *buf; static size_t cap;
                    ((void (*)(U, U *))GLF(glGetIntegerv))(0x0CF2, &rl);           /* UNPACK_ROW_LENGTH */
                    ((void (*)(U, U *))GLF(glGetIntegerv))(0x0CF3, &sr);           /* UNPACK_SKIP_ROWS */
                    ((void (*)(U, U *))GLF(glGetIntegerv))(0x0CF4, &sp);           /* UNPACK_SKIP_PIXELS */
                    orl = rl; if (!rl) rl = w;
                    if ((size_t)w * h > cap) { free(buf); cap = (size_t)w * h; buf = (unsigned int *)malloc(cap * 4); if (!buf) cap = 0; }
                    if (buf && w && h) {
                        const unsigned int *src = (const unsigned int *)(uintptr_t)oah_garg(R, 8) + (size_t)sr * rl + sp;
                        for (y = 0; y < h; y++) {
                            const unsigned int *sl = src + (size_t)y * rl; unsigned int *dl = buf + (size_t)y * w;
                            for (x = 0; x < w; x++) dl[x] = OSSwapInt32(sl[x]);
                        }
                        ((void (*)(U, U))GLF(glPixelStorei))(0x0CF2, 0);
                        ((void (*)(U, U))GLF(glPixelStorei))(0x0CF3, 0);
                        ((void (*)(U, U))GLF(glPixelStorei))(0x0CF4, 0);
                        ((oah_g8_t)fn)(R[3], R[4], R[5], R[6], R[7], R[8], R[9], type, (U)(uintptr_t)buf);
                        ((void (*)(U, U))GLF(glPixelStorei))(0x0CF2, orl);
                        ((void (*)(U, U))GLF(glPixelStorei))(0x0CF3, sr);
                        ((void (*)(U, U))GLF(glPixelStorei))(0x0CF4, sp);
                        ((void (*)(U, U))GLF(glPixelStorei))(0x0CF0, 1);
                        if (nlog < 4) { nlog++; fprintf(stderr, "[gl] texture upload %ux%u format %04x type %04x: words swapped here\n", w, h, R[9], type); }
                        break;
                    }
                }
                if (type == 0x8035) type = 0x8367; else if (type == 0x8367) type = 0x8035;
            }
        }
        ((oah_g8_t)fn)(R[3], R[4], R[5], R[6], R[7], R[8], R[9], type, oah_garg(R, 8));
        if (fast) ((void (*)(U, U))GLF(glPixelStorei))(0x0CF0, 1);
        break;
    }
    case OP_glCompressedTexSubImage2DARB: {
        /* The format must match the texture's internal format; RGB DXT1 textures are created
           as RGBA DXT1 below, so an RGB DXT1 sub-image update would fail with
           GL_INVALID_OPERATION and leave the texture unfilled. */
        U f = R[9];
        if (f == 0x83F0 && !getenv("OAH_NO_DXT1A")) f = 0x83F1;
        ((oah_g8_t)fn)(R[3], R[4], R[5], R[6], R[7], R[8], f, R[10], oah_garg(R, 8));
        break;
    }
    case OP_glProgramStringARB: {   /* (target, format, len, string): report compile errors */
        U e;
        ((void (*)(U, U, U, U))fn)(a0, a1, a2, a3);
        e = ((U (*)(void))GLF(glGetError))();
        if (e || getenv("OAH_GLPROG")) {
            U pos = 0;
            const char *es;
            ((void (*)(U, U *))GLF(glGetIntegerv))(0x864B, &pos);           /* GL_PROGRAM_ERROR_POSITION_ARB */
            es = (const char *)((const char *(*)(U))GLF(glGetString))(0x8874); /* GL_PROGRAM_ERROR_STRING_ARB */
            fprintf(stderr, "[gl] glProgramStringARB(%04x, len %u) -> error 0x%04x at %d: %s\n[gl]   program: %.*s\n",
                    a0, a2, e, (int)pos, es ? es : "", (int)(a2 > 1500 ? 1500 : a2), (const char *)a3);
        }
        return 1;
    }

    /* ---- AGL ---- */
    case OP_aglChoosePixelFormat: {
        U attr[128], devs[16], pf = 0;
        char desc[512];
        int nd = (int)a1, strip, try_;
        static const int strips[] = { 0, 0, 1, 3, 7 };
        oah_agl_attribs(a2, attr, 128, 0, desc, sizeof desc);
        if (a0 && nd > 0) oah_map_gdevs(a0, nd, devs);
        for (try_ = 0; try_ < 5 && !pf; try_++) {
            int usedev = try_ == 0 && a0 && nd > 0;
            strip = strips[try_];
            oah_agl_attribs(a2, attr, 128, strip, desc, sizeof desc);
            pf = ((U (*)(U *, U, U *))fn)(usedev ? devs : 0, usedev ? (U)nd : 0, attr);
            fprintf(stderr, "[agl] aglChoosePixelFormat try %d (devs=%s strip=%d) attribs: %s-> %08x\n",
                    try_, usedev ? "yes" : "no", strip, desc, pf);
        }
        if (pf && GLF(aglDescribePixelFormat)) {
            U (*dp)(U, U, U *) = (U (*)(U, U, U *))GLF(aglDescribePixelFormat);
            U dz = 0, ax = 0, st = 0, cs = 0;
            dp(pf, 12, &dz); dp(pf, 7, &ax); dp(pf, 13, &st); dp(pf, 50, &cs);
            fprintf(stderr, "[agl] pixel format %08x: depth=%u aux=%u stencil=%u pixel=%u (depth asked >= %d)\n",
                    pf, dz, ax, st, cs, oah_gl_depth_bits());
        }
        R[3] = pf;
        return 1;
    }
    case OP_aglQueryRendererInfo: {
        U devs[16], ri = 0;
        int nd = (int)a1;
        if (a0 && nd > 0) { oah_map_gdevs(a0, nd, devs); ri = ((U (*)(U *, U))fn)(devs, (U)nd); }
        if (!ri) ri = ((U (*)(U *, U))fn)(0, 0);
        fprintf(stderr, "[agl] aglQueryRendererInfo(ndev=%d) -> %08x\n", nd, ri);
        R[3] = ri;
        return 1;
    }
    case OP_aglDescribePixelFormat: case OP_aglDescribeRenderer: {
        U v = 0, ok = ((U (*)(U, U, U *))fn)(a0, a1, &v) & 0xff;
        if (a2) *(U *)a2 = OSSwapHostToBigInt32(v);
        if (g_glcalls[op] <= 40) fprintf(stderr, "[agl] %s(%08x, %u) -> %u value=%u (0x%x)\n", g_glnames[op], a0, a1, ok, v, v);
        R[3] = ok;
        return 1;
    }
    case OP_aglSetInteger: {
        U v[4] = { 0, 0, 0, 0 }, ok;
        int c = (a1 == 200 || a1 == 202) ? 4 : 1;
        if (a2) oah_bswap32n(v, (void *)a2, c);
        ok = ((U (*)(U, U, U *))fn)(a0, a1, a2 ? v : 0) & 0xff;
        fprintf(stderr, "[agl] aglSetInteger(%08x, %u, %d %d %d %d) -> %u\n", a0, a1, v[0], c > 1 ? v[1] : 0, c > 1 ? v[2] : 0, c > 1 ? v[3] : 0, ok);
        R[3] = ok;
        return 1;
    }
    case OP_aglSetCurrentContext: {
        U ok = ((U (*)(U))fn)(a0) & 0xff;
        if (ok) oah_gl_ctx_made_current(a0);
        if (g_glcalls[op] <= 10) fprintf(stderr, "[agl] aglSetCurrentContext(%08x) -> %u\n", a0, ok);
        R[3] = ok;
        return 1;
    }
    case OP_aglDestroyContext: {
        int i;
        for (i = 0; i < OAH_NCTX; i++) if (g_ctx_init[i] == a0) g_ctx_init[i] = 0;
        if (a0 == g_aux_ctx) { g_aux_fbo = 0; g_aux_ctx = 0; }
        if (g_cur_ctx == a0) g_cur_ctx = 0;
        R[3] = ((U (*)(U))fn)(a0) & 0xff;
        fprintf(stderr, "[agl] aglDestroyContext(%08x) -> %u\n", a0, R[3]);
        return 1;
    }
    case OP_aglSetDrawable: case OP_aglSetFullScreen: case OP_aglEnable: case OP_aglDisable:
    case OP_aglSwapBuffers: case OP_aglCreateContext: {
        U r = ((oah_g8_t)fn)(R[3], R[4], R[5], R[6], R[7], R[8], R[9], R[10], 0);
        if (op != OP_aglCreateContext) r &= 0xff;
        if (op != OP_aglSwapBuffers || g_glcalls[op] <= 3) {
            U e = GLF(aglGetError) ? ((U (*)(void))GLF(aglGetError))() : 0;
            fprintf(stderr, "[agl] %s(%08x, %08x, %08x, %08x, %08x) -> %08x aglGetError=%u\n", g_glnames[op], a0, a1, a2, a3, R[7], r, e);
        }
        if (op == OP_aglSwapBuffers && (g_glcalls[op] & 255) == 0) fprintf(stderr, "[agl] frames: %lu\n", g_glcalls[op]);
        R[3] = r;
        return 1;
    }
    case OP_glCompressedTexImage2DARB: {
        /* SC4 uploads its cut-out sprites (trees, building edges, vehicles) as RGB DXT1.  The PPC-era
           Mac drivers kept DXT1's punch-through texels transparent for that format; Mojave decodes them
           as opaque black (the black squares).  Upload as RGBA DXT1 instead: same bits, and opaque
           blocks still decode with alpha 1.  OAH_NO_DXT1A=1 restores the literal format. */
        static int keep = -1;
        U ifmt = R[5];
        if (keep < 0) keep = getenv("OAH_NO_DXT1A") != 0;
        if (ifmt == 0x83F0 && !keep) ifmt = 0x83F1;
        if (!fn) { R[3] = 0; return 1; }
        ((oah_g8_t)fn)(R[3], R[4], ifmt, R[6], R[7], R[8], R[9], R[10], 0);
        return 1;
    }
    default:   /* plain integer / pointer arguments, up to 8 */
        if (!fn) { R[3] = 0; return 1; }
        R[3] = ((oah_g8_t)fn)(R[3], R[4], R[5], R[6], R[7], R[8], R[9], R[10], 0);
        if (g_glnames[op][0] == 'a' && g_glcalls[op] <= 10)
            fprintf(stderr, "[agl] %s(%08x, %08x) -> %08x\n", g_glnames[op], a0, a1, R[3]);
        if (!g_glerr || op == OP_glGetError) return 1;
        break;
    }
    if (g_glerr && g_glnames[op][0] == 'g') {
        U e = ((U (*)(void))GLF(glGetError))();
        if (e) fprintf(stderr, "[gl] %s -> GL error 0x%04x (call #%lu)\n", g_glnames[op], e, g_glcalls[op]);
    }
    return 1;
}

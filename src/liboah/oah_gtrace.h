#include <sys/xattr.h>
#include <copyfile.h>
/* ---- OAH_GTRACE: log calls to guest functions (arguments and result) ----
   OAH_GTRACE="CoreGraphics:_CGBitmapContextCreate,HIToolbox:_CreateTransformedCGImageCopy[,...]"
   When an image is patched by the commpage patcher (before its code runs), each listed symbol in it gets
   "twi 31,r0,GT_ENTRY+i ; blr" over its first two words.  The entry trap logs r3..r10 / f1..f4 and LR,
   saves the caller's LR on a per-thread stack and sends the call to a trampoline that sets LR to a
   return stub ("twi 31,r0,GT_RET+i ; blr"), runs the two original words and branches to fn+8.  The
   return trap logs r3/f1 and returns to the saved LR.  Functions whose first two words branch, or use
   r12, are refused.  OAH_GTRACE_MAX=n stops logging an entry after n calls (default 200). */
#define GT_ENTRY 0x6000
#define GT_RET   0x6800
#define GT_MAX   512
typedef struct { char lib[48], sym[80]; unsigned int fn, tramp, *stub, w0, w1; unsigned long n; void (*fix)(unsigned int *R); void (*fixret)(unsigned int *R, unsigned int a0); int quiet, pic, follow; unsigned int ptramp; int r11w; int n_every; } oah_gt_t;
static oah_gt_t g_gt[GT_MAX];
/* set by a fixret: the guest returns through this stub, which goes on to the real return address
   (stored big-endian at g_gt_redirect_lrslot) */
static unsigned int g_gt_redirect, g_gt_redirect_lrslot;
static int g_gt_n = -1, g_gt_max = 200;
static pthread_key_t g_gt_key;
typedef struct { unsigned int lr[128], a0[128]; int sp; unsigned int nv[128][20]; int skip; unsigned int skip_ret; } oah_gt_stack;   /* nv: r13..r31, r1 at entry (OAH_GTRACE_NV) */
static int oah_gt_nv(void) { static int v = -1; if (v < 0) v = getenv("OAH_GTRACE_NV") ? 1 : 0; return v; }
/* a guest C string at p into s (n bytes at most), "" when unreadable */
static void oah_gt_cstr(unsigned int p, char *s, int n) {
    vm_size_t got = 0; int k;
    s[0] = 0;
    if (p < 0x1000 || vm_read_overwrite(mach_task_self(), p, n - 1, (vm_address_t)s, &got) || !got) { s[0] = 0; return; }
    for (k = 0; k < (int)got && s[k]; k++) if ((unsigned char)s[k] < 32 || (unsigned char)s[k] > 126) s[k] = '?';
    s[k < (int)got ? k : (int)got] = 0;
}
/* ---- built-in guest fixes: same hook, an entry callback that adjusts the arguments ---- */
static unsigned int g_iconunload_slot;
static void oah_fix_icon_unload(unsigned int *R) {
    /* CIconStorageEntry::Unload(this, flag, CFArrayRef *toRelease): SetIconFamilyPtr calls it with a NULL
       out-array, and when the entry holds shared data (flag 0x40000000, which Mojave-era icon data
       triggers) it stores through that NULL pointer (TextWrangler, Word: crash drawing an icon).
       Give it a scratch slot; the array of released items just leaks. */
    if (!R[5]) { g_iconunload_slot = 0; R[5] = (unsigned int)&g_iconunload_slot; }
}
/* Retina windows (oah_cgs.h): HIToolbox resets the CTM of a window's context before drawing each
   view (CGContextResetCTM), which takes it back to 1x. Scale it to 2x again on the way out. */
static void oah_cgs_retina_ctm(unsigned int ctx);
static int oah_hidpi(void);
static void oah_fixret_resetctm(unsigned int *R, unsigned int ctx) { (void)R; oah_cgs_retina_ctm(ctx); }
static int oah_want_hidpi(void) { return oah_hidpi() > 1; }
/* A fix can set g_gt_skip: the function is not run at all and returns 0 to its caller.
   Per thread (it was one global: a fix on one thread that set it and then made a guest call could
   have another thread's hooked function skipped), and saved around nested hooked calls. */
static oah_gt_stack *oah_gt_st(void) {
    static oah_gt_stack none;   /* before the key exists (no fix can run then) */
    oah_gt_stack *st;
    if (g_gt_n <= 0) return &none;
    st = (oah_gt_stack *)pthread_getspecific(g_gt_key);
    if (!st) { st = (oah_gt_stack *)calloc(1, sizeof *st); pthread_setspecific(g_gt_key, st); }
    return st;
}
#define g_gt_skip (oah_gt_st()->skip)
#define g_gt_skip_ret (oah_gt_st()->skip_ret)   /* with g_gt_skip: the value returned instead of 0 */
/* Quick Look in an Open / Save panel (Space on a file): the guest QuickLook framework finds no
   10.6 quicklookd to talk to and raises an assertion, which crashed the app (AppleWorks, through
   its Open dialog). Without it the preview just stays empty. */
static void oah_fix_qlassert(unsigned int *R) {
    static int said;
    (void)R;
    if (!said++) fprintf(stderr, "[ql] Quick Look assertion skipped (no 10.6 quicklookd)\n");
    g_gt_skip = 1;
}
/* Retina (OAH_HIDPI): HIToolbox calls CUIDraw without a scale, and the 10.6 CoreUI then takes it
   from the context: 2 in a Retina window, and with no 2x artwork in the 10.6 theme it draws nothing
   (title bar buttons, toolbar and pop-up controls missing). OAH_CUISCALE=1 asks for scale 1: the 1x
   artwork is drawn through the 2x transform, and text and paths stay sharp (TextWrangler, Hex Fiend).
   Experimental and off by default: Keynote trapped in CoreFoundation copying its options from here.
   OAH_CUIDUMP=n logs the first n calls. Each call gets a copy of its options with the
   scale set, released 32 calls later. */
static unsigned int oah_guest_sym(const char *img, const char *sym);
static int oah_cgs_is_retina_ctx(unsigned int ctx);
static unsigned int oah_callppc_n(const char *tag, unsigned int fn, int n, unsigned int *args);
static void oah_fix_cuidraw(unsigned int *R) {
    static unsigned int fget, fnum, fcopy, fset, fmk, keyp; static int n, max = -1, force = -1;
    unsigned int a[4], key, v;
    double d = 0;
    if (max < 0) { const char *e = getenv("OAH_CUIDUMP"), *f = getenv("OAH_CUISCALE"); max = e ? atoi(e) : 0; force = f && atof(f) > 0; }
    if (!R[9] || (n >= max && !force)) return;
    if (!pthread_main_np()) return;      /* guest calls from here are only safe on the main thread */
    /* offscreen contexts keep their own scale, except with OAH_CUISCALE: HIToolbox draws the title bar
       into a CGLayer made from the window context (2x too), where the unforced 10.6 CoreUI found no 2x
       artwork and left the title bar transparent */
    if (!oah_cgs_is_retina_ctx(R[8]) && !(force > 0 && oah_hidpi() > 1)) return;
    if (!fget) { const char *cf = "CoreFoundation.framework";
                 fget = oah_guest_sym(cf, "_CFDictionaryGetValue"); fnum = oah_guest_sym(cf, "_CFNumberGetValue");
                 fcopy = oah_guest_sym(cf, "_CFDictionaryCreateMutableCopy"); fset = oah_guest_sym(cf, "_CFDictionarySetValue");
                 fmk = oah_guest_sym(cf, "_CFNumberCreate"); keyp = oah_guest_sym("CoreUI.framework", "_kCUIScaleKey");
                 fprintf(stderr, "[cui] syms %08x %08x %08x %08x %08x key@%08x\n", fget, fnum, fcopy, fset, fmk, keyp); }
    if (!fget || !fnum || !keyp) return;
    key = OSSwapBigToHostInt32(*(unsigned int *)keyp);
    a[0] = R[9]; a[1] = key; v = oah_callppc_n("CFDictionaryGetValue", fget, 2, a);
    if (v) { unsigned long long be = 0; a[0] = v; a[1] = 13; a[2] = (unsigned int)&be; oah_callppc_n("CFNumberGetValue", fnum, 3, a);
             be = OSSwapBigToHostInt64(be); memcpy(&d, &be, 8); }
    if (n < max) {
        static unsigned int wkp, fgcs; char wn[64] = "?"; unsigned int wv = 0;
        if (!wkp) { wkp = oah_guest_sym("CoreUI.framework", "_kCUIWidgetKey"); fgcs = oah_guest_sym("CoreFoundation.framework", "_CFStringGetCString"); }
        if (wkp && fgcs) {
            a[0] = R[9]; a[1] = OSSwapBigToHostInt32(*(unsigned int *)wkp); wv = oah_callppc_n("CFDictionaryGetValue", fget, 2, a);
            if (wv) { a[0] = wv; a[1] = (unsigned int)(uintptr_t)wn; a[2] = sizeof wn; a[3] = 0x08000100u; if (!oah_callppc_n("CFStringGetCString", fgcs, 4, a)) strcpy(wn, "?"); }
        }
        n++; fprintf(stderr, "[cui] CUIDraw ctx %08x options %08x scale %s %g widget %s retina %d\n", R[8], R[9], v ? "set" : "unset", d, wn, oah_cgs_is_retina_ctx(R[8]));
    }
    /* Carbon window frames (kCUIWidgetWindowFrame): with the scale forced to 1 the 10.6 CoreUI still
       draws nothing into a 2x context (TextWrangler's title bar stayed transparent). Draw the frame
       into a 1x bitmap of its size and that image into the window (the background only: title text
       and buttons are drawn separately, at 2x). OAH_NO_CUIFRAME1X=1 disables. */
    if (force > 0 && oah_hidpi() > 1 && !getenv("OAH_NO_CUIFRAME1X")) {
        static int inframe; static unsigned int wkp2, fgcs2, fcs, fbc, fimg, fdraw, fcrel, firel, fcui, cs;
        unsigned int save[8], wv, k; char wn[40] = "";
        if (inframe) return;                              /* the 1x draw below: as is */
        for (k = 0; k < 8; k++) save[k] = R[3 + k];
        if (!wkp2) { wkp2 = oah_guest_sym("CoreUI.framework", "_kCUIWidgetKey"); fgcs2 = oah_guest_sym("CoreFoundation.framework", "_CFStringGetCString");
                     fcs = oah_guest_sym("CoreGraphics.framework", "_CGColorSpaceCreateDeviceRGB"); fbc = oah_guest_sym("CoreGraphics.framework", "_CGBitmapContextCreate");
                     fimg = oah_guest_sym("CoreGraphics.framework", "_CGBitmapContextCreateImage"); fdraw = oah_guest_sym("CoreGraphics.framework", "_CGContextDrawImage");
                     fcrel = oah_guest_sym("CoreGraphics.framework", "_CGContextRelease"); firel = oah_guest_sym("CoreGraphics.framework", "_CGImageRelease");
                     fcui = oah_guest_sym("CoreUI.framework", "_CUIDraw"); }
        if (wkp2 && fgcs2) {
            a[0] = save[6]; a[1] = OSSwapBigToHostInt32(*(unsigned int *)wkp2); wv = oah_callppc_n("CFDictionaryGetValue", fget, 2, a);
            if (wv) { a[0] = wv; a[1] = (unsigned int)(uintptr_t)wn; a[2] = sizeof wn; a[3] = 0x08000100u; if (!oah_callppc_n("CFStringGetCString", fgcs2, 4, a)) wn[0] = 0; }
        }
        if (!strcmp(wn, "kCUIWidgetWindowFrame") && fcs && fbc && fimg && fdraw && fcrel && firel && fcui) {
            float fw, fh; unsigned int bc, img, x[8], zero = 0; int W, H;
            memcpy(&fw, &save[3], 4); memcpy(&fh, &save[4], 4);
            W = (int)(fw + 0.999f); H = (int)(fh + 0.999f);
            if (W > 0 && H > 0 && W <= 8192 && H <= 8192) {
                if (!cs) cs = oah_callppc_n("CGColorSpaceCreateDeviceRGB", fcs, 0, x);
                x[0] = 0; x[1] = (unsigned int)W; x[2] = (unsigned int)H; x[3] = 8; x[4] = 0; x[5] = cs; x[6] = 2;   /* premultiplied first */
                bc = oah_callppc_n("CGBitmapContextCreate", fbc, 7, x);
                if (bc) {
                    x[0] = save[0]; x[1] = zero; x[2] = zero; x[3] = save[3]; x[4] = save[4]; x[5] = bc; x[6] = save[6]; x[7] = save[7];
                    inframe = 1; oah_callppc_n("CUIDraw1x", fcui, 8, x); inframe = 0;
                    x[0] = bc; img = oah_callppc_n("CGBitmapContextCreateImage", fimg, 1, x);
                    if (img) {
                        x[0] = save[5]; x[1] = save[1]; x[2] = save[2]; x[3] = save[3]; x[4] = save[4]; x[5] = img;
                        oah_callppc_n("CGContextDrawImage", fdraw, 6, x);
                        x[0] = img; oah_callppc_n("CGImageRelease", firel, 1, x);
                    }
                    x[0] = bc; oah_callppc_n("CGContextRelease", fcrel, 1, x);
                    for (k = 0; k < 8; k++) R[3 + k] = save[k];
                    { static int nl; if (nl++ < 3) fprintf(stderr, "[cui] window frame %dx%d drawn at 1x\n", W, H); }
                    g_gt_skip = 1; g_gt_skip_ret = 0;
                    return;
                }
            }
        }
        for (k = 0; k < 8; k++) R[3 + k] = save[k];
    }
    if (force && fcopy && fset && fmk) {
        double want = getenv("OAH_CUISCALE") ? atof(getenv("OAH_CUISCALE")) : 1.0; unsigned long long be; unsigned int m, num;
        if (v) return;                                   /* the caller chose a scale itself */
        memcpy(&be, &want, 8); be = OSSwapHostToBigInt64(be);
        static unsigned int cnum, ring[32], frel; static int ri;
        if (!cnum) { a[0] = 0; a[1] = 13; a[2] = (unsigned int)&be; cnum = oah_callppc_n("CFNumberCreate", fmk, 3, a); }
        if (!frel) frel = oah_guest_sym("CoreFoundation.framework", "_CFRelease");
        num = cnum;
        {   /* Keynote passes option dictionaries holding NULL entries: copying one trapped in
               CFRetain(NULL). Only dictionaries of non-NULL keys and values are copied. */
            static unsigned int fcnt, fkv; unsigned int cnt, kv[2][64], i2;
            if (!fcnt) { fcnt = oah_guest_sym("CoreFoundation.framework", "_CFDictionaryGetCount"); fkv = oah_guest_sym("CoreFoundation.framework", "_CFDictionaryGetKeysAndValues"); }
            if (!fcnt || !fkv) return;
            a[0] = R[9]; cnt = oah_callppc_n("CFDictionaryGetCount", fcnt, 1, a);
            if (cnt > 64) return;
            a[0] = R[9]; a[1] = (unsigned int)(uintptr_t)kv[0]; a[2] = (unsigned int)(uintptr_t)kv[1]; oah_callppc_n("CFDictionaryGetKeysAndValues", fkv, 3, a);
            for (i2 = 0; i2 < cnt; i2++) if (!kv[0][i2] || !kv[1][i2]) return;
        }
        a[0] = 0; a[1] = 0; a[2] = R[9]; m = oah_callppc_n("CFDictionaryCreateMutableCopy", fcopy, 3, a);
        if (m && num) {
            a[0] = m; a[1] = key; a[2] = num; oah_callppc_n("CFDictionarySetValue", fset, 3, a); R[9] = m;
            /* the copy must outlive this call; release it 32 calls later (nested calls stay covered) */
            if (ring[ri] && frel) { a[0] = ring[ri]; oah_callppc_n("CFRelease", frel, 1, a); }
            ring[ri] = m; ri = (ri + 1) & 31;
        }
    }
}
/* DiskArbitration. The 10.6 guest framework talks to diskarbitrationd over a MIG protocol that
   Mojave's daemon does not answer: DASessionCreate returned NULL. NSWorkspace
   -getFileSystemInfoForPath:... then reported every volume as not writable, and Adobe CS2's
   installer found no volume to install on (SelectTargetVolume.m: failed assertion
   `[inNewVolumes count] > 0'). The guest gets stand-in objects instead: a session and disks are
   guest CFStrings (so CFRetain/CFRelease work on them), and the calls that read a disk are
   answered from Mojave's DiskArbitration. Callbacks and run-loop scheduling are accepted and
   never fire; mount, unmount and eject requests are not carried out. OAH_NO_DAFIX=1 disables. */
static unsigned int oah_cf_to_guest(CFTypeRef o);
static struct { unsigned int tok; char name[64]; } g_da_tok[128];
static unsigned int g_da_ntok;
static pthread_mutex_t g_da_mx = PTHREAD_MUTEX_INITIALIZER;
static int oah_want_dafix(void) { return !getenv("OAH_NO_DAFIX"); }
static unsigned int oah_da_token(const char *name) {      /* guest CFString standing in for a session / disk */
    CFStringRef s = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    unsigned int t = s ? oah_cf_to_guest(s) : 0, i;
    if (s) CFRelease(s);
    if (!t) return 0;
    pthread_mutex_lock(&g_da_mx);
    i = g_da_ntok++ % 128;
    g_da_tok[i].tok = t; snprintf(g_da_tok[i].name, sizeof g_da_tok[i].name, "%s", name);
    pthread_mutex_unlock(&g_da_mx);
    return t;
}
static const char *oah_da_name(unsigned int t) {
    unsigned int i;
    if (!t) return 0;
    for (i = 0; i < 128 && i < g_da_ntok; i++) if (g_da_tok[i].tok == t) return g_da_tok[i].name;
    return 0;
}
static void oah_da_ret(unsigned int v) { g_gt_skip = 1; g_gt_skip_ret = v; }
static void oah_fix_da_session(unsigned int *R) { (void)R; oah_da_ret(oah_da_token("<DASession>")); }
static void oah_fix_da_noop(unsigned int *R) { (void)R; oah_da_ret(0); }
static void oah_fix_da_disk(unsigned int *R) {           /* DADiskCreateFromBSDName(alloc, session, name) */
    const char *nm = R[5] > 0x1000 ? (const char *)R[5] : 0;
    if (!nm) { oah_da_ret(0); return; }
    if (!strncmp(nm, "/dev/", 5)) nm += 5;
    oah_da_ret(oah_da_token(nm));
}
/* DADiskCreateFromVolumePath(alloc, session, CFURLRef): the real one creates a guest DADisk,
   whose CF class is registered only by the DASessionCreate that the guest never runs, so
   _CFRuntimeCreateInstance hit __HALT (TextWrangler crashed opening a document). Answer with
   the token of the volume's /dev node (statfs); NULL for volumes not backed by a disk. */
#include <sys/mount.h>
static void oah_fix_da_volpath(unsigned int *R) {
    static unsigned int getfsr;
    char path[1024]; struct statfs sf; unsigned int a[4], t = 0;
    if (!getfsr) getfsr = oah_guest_sym("CoreFoundation", "_CFURLGetFileSystemRepresentation");
    path[0] = 0;
    if (getfsr && R[5]) {
        a[0] = R[5]; a[1] = 1; a[2] = (unsigned int)path; a[3] = sizeof path;
        if (!(oah_callppc_n("DADiskCreateFromVolumePath", getfsr, 4, a) & 0xff)) path[0] = 0;
    }
    if (path[0] && !statfs(path, &sf) && !strncmp(sf.f_mntfromname, "/dev/", 5))
        t = oah_da_token(sf.f_mntfromname + 5);
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[da] DADiskCreateFromVolumePath(%s) -> %08x\n", path, t);
    oah_da_ret(t);
}
static void *oah_da_native(const char *name) {           /* +1 native DADisk, or NULL */
    static void *(*sess)(CFAllocatorRef), *(*mk)(CFAllocatorRef, void *, const char *), *session;
    if (!sess) {
        void *h = dlopen("/System/Library/Frameworks/DiskArbitration.framework/DiskArbitration", RTLD_LAZY);
        if (!h) return 0;
        *(void **)&sess = dlsym(h, "DASessionCreate"); *(void **)&mk = dlsym(h, "DADiskCreateFromBSDName");
        if (!sess || !mk) { sess = 0; return 0; }
    }
    if (!session) session = sess(NULL);
    return session && name && name[0] != '<' ? mk(NULL, session, name) : 0;
}
static void oah_fix_da_desc(unsigned int *R) {           /* DADiskCopyDescription(disk) */
    static CFDictionaryRef (*descf)(void *);
    const char *nm = oah_da_name(R[3]);
    void *disk; CFDictionaryRef dsc; unsigned int g = 0;
    if (!nm) { oah_da_ret(0); return; }
    if (!descf) *(void **)&descf = dlsym(RTLD_DEFAULT, "DADiskCopyDescription");
    if ((disk = oah_da_native(nm))) {
        if (descf && (dsc = descf(disk))) { g = oah_cf_to_guest(dsc); CFRelease(dsc); }
        CFRelease((CFTypeRef)disk);
    }
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[da] DADiskCopyDescription(%s) -> guest dict %08x\n", nm, g);
    oah_da_ret(g);
}
static void oah_fix_da_bsdname(unsigned int *R) {        /* DADiskGetBSDName(disk): a pointer that lives as long as we do */
    const char *nm = oah_da_name(R[3]);
    oah_da_ret(nm && nm[0] != '<' ? (unsigned int)nm : 0);
}
static void oah_fix_da_whole(unsigned int *R) {          /* DADiskCopyWholeDisk(disk): "disk1s7" -> "disk1" */
    const char *nm = oah_da_name(R[3]);
    char w[64], *p;
    if (!nm || nm[0] == '<') { oah_da_ret(0); return; }
    snprintf(w, sizeof w, "%s", nm);
    if ((p = strchr(w + 4, 's'))) *p = 0;
    oah_da_ret(oah_da_token(w));
}
/* Volume device type (LaunchServices: volume icons, network checks). The 10.6 File Manager reads
   it from its mount table, which Mojave's coreservicesd never fills (FSGetVolumeInfo is bridged
   for the same reason), so every volume came back '????' and Open panels and installers showed
   disks as plain folders. Answer from the mount table here: 'ntwk' for network file systems,
   'hdsk' otherwise; no backing store volume. (vref, OSType *type, FSVolumeRefNum *backing) */
#include <sys/mount.h>
static int oah_guest_vref_for(const char *path, unsigned int rootref, short *vref);
static int oah_mount_list(struct statfs **out);
static void oah_fix_voldevtype(unsigned int *R) {
    short want = (short)R[3], v;
    unsigned int type = 0x6864736b;                          /* 'hdsk' */
    struct statfs *m; int n, i;
    if (pthread_main_np())
        for (n = oah_mount_list(&m), i = 0; i < n; i++)
            if (!oah_guest_vref_for(m[i].f_mntonname, 0, &v) && v == want) {
                if (!(m[i].f_flags & MNT_LOCAL)) type = 0x6e74776b;   /* 'ntwk' */
                break;
            }
    if (R[4]) OSWriteBigInt32((void *)R[4], 0, type);
    if (R[5]) OSWriteBigInt16((void *)R[5], 0, 0);
    g_gt_skip = 1; g_gt_skip_ret = 0;
}
static int oah_want_voldevtype(void) { return !getenv("OAH_NO_VOLDEVFIX"); }
/* GetFrontProcess(ProcessSerialNumber *): in a process running as root in the user's session
   (an installer started through sudo, or relaunched by AuthorizationExecuteWithPrivileges) the
   guest's Process Manager answered procNotFound (-600), so HIToolbox took this app for a
   background one and turned every click into an activation click that never came: Logic's VISE
   installer ignored all mouse clicks. On failure, answer from the window server's front process.
   OAH_NO_FRONTFIX=1 disables. */
static void oah_fixret_frontproc(unsigned int *R, unsigned int psn) {
    static int (*getfront)(unsigned int *), (*getcur)(unsigned int *);
    static unsigned int self[2];
    unsigned int f[2] = { 0, 0 };
    if (!R[3] || !psn) return;
    if (!getfront) {
        getfront = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "_SLPSGetFrontProcess");
        getcur = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "SLPSGetCurrentProcess");
    }
    if (!getfront || getfront(f) || !f[1]) return;
    if (getcur && !self[1]) getcur(self);
    /* Our own process is answered with its native serial number, the one GetCurrentProcess and
       GetNextProcess (bridged) give the guest. kCurrentProcess here made Adobe Help Center's
       single-instance check (front process, then GetNextProcess over all, counting its own app
       name) count itself twice and quit at start. OAH_FRONT_KCURRENT=1: kCurrentProcess. */
    if (self[1] && f[0] == self[0] && f[1] == self[1] && getenv("OAH_FRONT_KCURRENT")) { f[0] = 0; f[1] = 2; }
    if (getenv("OAH_FRONTFIX_LOG")) fprintf(stderr, "[front] GetFrontProcess -> %d, answered %x.%x\n", (int)R[3], f[0], f[1]);
    OSWriteBigInt32((void *)psn, 0, f[0]);
    OSWriteBigInt32((void *)psn, 4, f[1]);
    R[3] = 0;
}
static int oah_want_frontfix(void) { return !getenv("OAH_NO_FRONTFIX"); }
/* Double-click time. HIToolbox's GetDblTime asks IOKit's NXClickTime, through an event status
   handle from NXOpenEventStatus - the 10.6 HID system's shared parameters, which do not exist on
   Mojave: the handle came back 0 and GetDblTime said 0 ticks, so applications that time double
   clicks themselves (Entourage's message list, which then never opened a message) never saw
   one. Answer from the user's double-click setting (com.apple.mouse.doubleClickThreshold,
   0.5 s when unset, as on Mojave). OAH_NO_DBLTIMEFIX=1 disables. */
static double oah_dbl_seconds(void) {
    static double s; static double at;
    double now = CFAbsoluteTimeGetCurrent();
    if (!s || now - at > 5) {
        CFPropertyListRef v = CFPreferencesCopyAppValue(CFSTR("com.apple.mouse.doubleClickThreshold"), kCFPreferencesAnyApplication);
        double d = 0.5;
        if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &d);
        if (v) CFRelease(v);
        if (d < 0.1 || d > 5) d = 0.5;
        s = d; at = now;
    }
    return s;
}
static void oah_fix_dbltime(unsigned int *R) {      /* UInt32 GetDblTime(void), ticks */
    R[3] = (unsigned int)(oah_dbl_seconds() * 60.0 + 0.5);
    g_gt_skip = 1; g_gt_skip_ret = R[3];
}
static void oah_fix_nxclicktime(unsigned int *R) {  /* double NXClickTime(NXEventHandle), seconds */
    double *F = (double *)(R + 0x1e8 / 4);
    F[1] = oah_dbl_seconds();
    g_gt_skip = 1; g_gt_skip_ret = R[3];
}
static int oah_want_dbltimefix(void) { return !getenv("OAH_NO_DBLTIMEFIX"); }
/* CarbonCore seeds (_CSCreateSeed and friends): named counters kept by coreservicesd in a shared
   memory page. The 10.6 seed service is not there on Mojave and every _CSCreateSeed failed with
   -900. CoreDrag gives each new drag a seed from it, so no PowerPC app could start a drag at all:
   dragging selected text inside a TextWrangler document put up "This operation couldn't be
   completed (MacOS Error code: -900)". Seeds are kept per process here instead (drags inside one
   app are all CoreDrag needs them for). OAH_NO_CSSEEDFIX=1 disables. */
#define OAH_CSSEED_N 1024
static unsigned int g_csseed_val[OAH_CSSEED_N];
static char *g_csseed_name[OAH_CSSEED_N];
static pthread_mutex_t g_csseed_mu = PTHREAD_MUTEX_INITIALIZER;
static int oah_csseed_idx(unsigned int seed) {     /* a seed of ours: { magic, index } */
    unsigned int i;
    if (!seed || oah_get32(seed) != 0x52735364u) return -1;
    i = oah_get32(seed + 4);
    return i < OAH_CSSEED_N ? (int)i : -1;
}
static void oah_fix_cscreate(unsigned int *R) {     /* OSStatus _CSCreateSeed(const char *name, flags, CSSeedRef *out) */
    const char *name = (const char *)(uintptr_t)R[3];
    unsigned int out = R[5], *ref, base = 0;
    int i, slot = -1, err = -50;
    /* only HIServices' seeds (CoreDrag, Finder labels): LaunchServices and OSServices treat a seed
       they can read as permission to trust their caches, and with seeds kept here their type data
       went stale - TextWrangler's copies lost their public text flavors (dyn.* types only) and
       nothing reached Mojave's clipboard. Those callers still get the 10.6 answer (-900). */
    {   const char *img = oah_guest_image_name_fast(R[0x90 / 4], &base);
        if (!img || !strstr(img, "HIServices")) return; }
    pthread_mutex_lock(&g_csseed_mu);
    if (name && out) {
        for (i = 0; i < OAH_CSSEED_N && g_csseed_name[i]; i++)
            if (!strcmp(g_csseed_name[i], name)) { slot = i; break; }
        if (slot < 0 && i < OAH_CSSEED_N && (g_csseed_name[i] = strdup(name))) { slot = i; g_csseed_val[i] = 1; }
        err = slot < 0 ? -108 : (ref = (unsigned int *)calloc(2, 4)) ? 0 : -108;
        if (!err) { oah_put32((unsigned int)(uintptr_t)ref, 0x52735364u); oah_put32((unsigned int)(uintptr_t)ref + 4, slot); oah_put32(out, (unsigned int)(uintptr_t)ref); }
    }
    pthread_mutex_unlock(&g_csseed_mu);
    R[3] = (unsigned int)err; g_gt_skip = 1; g_gt_skip_ret = R[3];
}
static void oah_fix_csread(unsigned int *R) {       /* OSStatus _CSReadSeed(CSSeedRef, UInt32 *value) */
    int i = oah_csseed_idx(R[3]);
    if (i < 0) return;                              /* not ours: CarbonCore's own */
    if (i >= 0 && R[4]) oah_put32(R[4], g_csseed_val[i]);
    R[3] = i >= 0 && R[4] ? 0 : (unsigned int)-50; g_gt_skip = 1; g_gt_skip_ret = R[3];
}
static void oah_fix_csincrement(unsigned int *R) {  /* OSStatus _CSIncrementSeed(CSSeedRef, UInt32 *newValue) */
    int i = oah_csseed_idx(R[3]);
    if (i < 0) return;                              /* not ours: CarbonCore's own */
    if (i >= 0 && R[4]) { unsigned int v = __sync_add_and_fetch(&g_csseed_val[i], 1); oah_put32(R[4], v); }
    R[3] = i >= 0 && R[4] ? 0 : (unsigned int)-50; g_gt_skip = 1; g_gt_skip_ret = R[3];
}
static void oah_fix_cswrite(unsigned int *R) {      /* OSStatus _CSWriteSeed(CSSeedRef, UInt32 value, ...) */
    int i = oah_csseed_idx(R[3]);
    if (i < 0) return;                              /* not ours: CarbonCore's own */
    if (i >= 0) g_csseed_val[i] = R[4];
    R[3] = i >= 0 ? 0 : (unsigned int)-50; g_gt_skip = 1; g_gt_skip_ret = R[3];
}
static void oah_fix_csdestroy(unsigned int *R) {    /* OSStatus _CSDestroySeed(CSSeedRef) */
    int i = oah_csseed_idx(R[3]);
    if (i < 0) return;                              /* not ours: CarbonCore's own */
    if (i >= 0) { oah_put32(R[3], 0); free((void *)(uintptr_t)R[3]); }
    R[3] = i >= 0 ? 0 : (unsigned int)-50; g_gt_skip = 1; g_gt_skip_ret = R[3];
}
static int oah_want_csseedfix(void) { return !getenv("OAH_NO_CSSEEDFIX"); }
/* SCDynamicStoreCopyConsoleUser(store, uid_t *, gid_t *): the 10.6 SystemConfiguration asks configd
   with messages Mojave's configd no longer answers the same way, and got NULL. The translated fontd
   then had no logged-in user and never registered ~/Library/Fonts: fonts the user installed (and
   Office 2004's own, which it puts there - "MT Extra", Haettenschweiler...) were missing in every
   PowerPC application (Equation Editor: 'screen font "MT Extra" is not available'). Answered from
   Mojave. OAH_NO_CONSOLEUSERFIX=1 disables. */
static CFTypeRef oah_kc_unwrap(unsigned int g);   /* oah_kc.h: a store of ours (oah_sc.h) */
static void oah_fix_consoleuser(unsigned int *R) {
    static CFStringRef (*cu)(void *, uid_t *, gid_t *);
    uid_t u = 0; gid_t g = 0;
    CFStringRef s;
    if (!cu) {
        void *h = dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration", RTLD_LAZY);
        if (h) cu = (CFStringRef (*)(void *, uid_t *, gid_t *))dlsym(h, "SCDynamicStoreCopyConsoleUser");
    }
    if (!cu || !(s = cu(NULL, &u, &g))) {                                        /* nobody on the console: the guest's own answer */
        if (oah_kc_unwrap(R[3])) { g_gt_skip = 1; g_gt_skip_ret = 0; }            /* ... unless the store is ours (the guest's would read the proxy) */
        return;
    }
    if (R[4]) OSWriteBigInt32((void *)R[4], 0, (unsigned int)u);
    if (R[5]) OSWriteBigInt32((void *)R[5], 0, (unsigned int)g);
    R[3] = oah_cf_to_guest(s);
    CFRelease(s);
    g_gt_skip = 1; g_gt_skip_ret = R[3];
}
static int oah_want_consoleuserfix(void) { return !getenv("OAH_NO_CONSOLEUSERFIX"); }
/* iWork '09 (SFUtility) poses its own NSUndoManager. It ends the undo groups of an event either
   after each run loop iteration (an ordered NSRunLoop perform) or, while its flag
   sUndoManagerEndsTopLevelGroupingsAfterRunLoopIterations is 0, only when AppKit calls
   +[NSUndoManager _endTopLevelGroupings]. The 10.6 AppKit does that only after Carbon menu events
   and Apple Events, and nothing set the flag: a cell or text edit stayed in an open group, the
   document was not marked edited, and Close / Quit discarded it without asking (until a menu was
   opened). The flag is set before each new group. OAH_NO_SFUNDOFIX=1 disables. */
static void oah_fix_sfundo(unsigned int *R) {
    static unsigned int a, done;
    (void)R;
    if (!a && !done) { a = oah_guest_sym("SFUtility", "_sUndoManagerEndsTopLevelGroupingsAfterRunLoopIterations"); done = 1; }
    if (a && !*(volatile unsigned char *)(uintptr_t)a) {
        *(volatile unsigned char *)(uintptr_t)a = 1;
        fprintf(stderr, "[undo] iWork undo groups now end after each run loop iteration\n");
    }
}
static int oah_want_sfundofix(void) { return !getenv("OAH_NO_SFUNDOFIX"); }
/* _UTTypeGetStatus(uti): -1 invalid, 0 undeclared, 1 declared, 2 dynamic. The guest database
   never saw the applications installed on Mojave, so the types they declare were "undeclared".
   NSSavePanel then took a UTI in allowedFileTypes for a filename extension: Transmission's Create
   Torrent File wanted names ending in ".org.bittorrent.torrent". Undeclared types that Mojave
   declares answer 1. OAH_NO_UTSTATUSFIX=1 disables. */
static void oah_fixret_utstatus(unsigned int *R, unsigned int uti) {
    static Boolean (*isdecl)(CFStringRef);
    CFStringRef s;
    if (R[3] != 0 || !uti) return;
    if (!isdecl) isdecl = (Boolean (*)(CFStringRef))dlsym(RTLD_DEFAULT, "UTTypeIsDeclared");
    if (!isdecl || !(s = oah_guest_str_native(uti))) return;
    if (isdecl(s)) {
        R[3] = 1;
        if (getenv("OAH_LSTRACE")) { char b[160] = ""; CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8); fprintf(stderr, "[ls] _UTTypeGetStatus(%s): declared on Mojave\n", b); }
    }
    CFRelease(s);
}
static int oah_want_utstatusfix(void) { return !getenv("OAH_NO_UTSTATUSFIX"); }
/* SCDynamicStoreCopyConsoleInformation(store) -> CFArray of session dictionaries: libATSServer's
   RunningInUserMode looks for our euid there before it registers the user font folder. */
static void oah_fix_consoleinfo(unsigned int *R) {
    static CFArrayRef (*ci)(void *);
    CFArrayRef a;
    if (!ci) {
        void *h = dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration", RTLD_LAZY);
        if (h) ci = (CFArrayRef (*)(void *))dlsym(h, "SCDynamicStoreCopyConsoleInformation");
    }
    if (!ci || !(a = ci(NULL))) { if (oah_kc_unwrap(R[3])) { g_gt_skip = 1; g_gt_skip_ret = 0; } return; }
    R[3] = oah_cf_to_guest(a);
    CFRelease(a);
    g_gt_skip = 1; g_gt_skip_ret = R[3];
}
static void oah_fix_aesendmessage(unsigned int *R);   /* oah_aen.h */
static int oah_want_aesmfix(void);
/* GetFrontProcess and HIServices' _GetFrontUIProcess (FindWindow -> IsCurrentProcessMenuBarOwner
   calls it on every null event) go through _LSCopyFrontUIApplication, whose CoreApplicationServices
   port setup fails here and logs through syslog each time: GoLive CS2 made ~200 such calls a second
   while idle (a third of a core, mostly system time). Answer from the window server's front process
   at entry instead and skip the guest code. Falls back to the guest's path if Mojave gives nothing. */
static void oah_fix_frontproc(unsigned int *R) {
    unsigned int psn = R[3];
    if (!psn) return;
    R[3] = 1;                                  /* "failed": oah_fixret_frontproc answers natively */
    oah_fixret_frontproc(R, psn);
    if (R[3] == 0) { g_gt_skip = 1; g_gt_skip_ret = 0; }
    else R[3] = psn;                           /* no native answer: run the guest's own code */
}
/* LaunchApplication(LaunchParamBlockRec *) of a PowerPC application: the guest Process Manager
   cannot start one on Mojave (-10827), and Vue 5 Esprit's installer - which copies itself to
   TemporaryItems and relaunches the copy - said "Setup cannot be launched". Start a PowerPC-only
   target under the translator, as LSOpenFromRefSpec does (oah_ls.h); anything else goes on to the
   guest's own LaunchApplication. Block: 6 launchBlockID, 16 launchAppSpec (FSSpec *),
   20 launchProcessSN. OAH_NO_LAUNCHAPPFIX=1 disables. */
static int oah_app_is_ppc_only(const char *app, char *exe, size_t exesz);
static int oah_launch_translated(const char *app, CFArrayRef docs);
static int oah_launch_translated_pid(const char *app, CFArrayRef docs, pid_t *outpid);
static unsigned int g_ls_launch_flags;   /* oah_ls.h */
static char g_ls_launch_ae[256];          /* oah_ls.h */
static void oah_fix_launchapp(unsigned int *R) {
    static unsigned int fmk, fmp;
    unsigned int pb = R[3], spec, a[3];
    unsigned char ref[80];
    char path[1024], exe[1100];
    int err;
    if (!pb) return;
    spec = OSSwapBigToHostInt32(*(unsigned int *)(pb + 16));
    if (!spec) return;
    if (!fmk) { fmk = oah_guest_sym("CarbonCore", "_FSpMakeFSRef"); fmp = oah_guest_sym("CarbonCore", "_FSRefMakePath"); }
    if (!fmk || !fmp) return;
    a[0] = spec; a[1] = (unsigned int)ref;
    if (oah_callppc_n("FSpMakeFSRef", fmk, 2, a)) return;
    path[0] = 0;
    a[0] = (unsigned int)ref; a[1] = (unsigned int)path; a[2] = sizeof path;
    if (oah_callppc_n("FSRefMakePath", fmp, 3, a) || !path[0]) return;
    if (!oah_app_is_ppc_only(path, exe, sizeof exe)) return;
    {   unsigned int ap = OSSwapBigToHostInt32(*(unsigned int *)(pb + 40)), k0;
        fprintf(stderr, "[ls] LaunchApplication block:");
        for (k0 = 0; k0 < 44; k0 += 4) fprintf(stderr, " %08x", OSSwapBigToHostInt32(*(unsigned int *)(pb + k0)));
        fprintf(stderr, "\n");
        if (ap) {
            unsigned int len = OSSwapBigToHostInt32(*(unsigned int *)(ap + 20)), k;
            fprintf(stderr, "[ls] LaunchApplication parameters: what %u message %08x where %08x refcon %08x len %u:",
                    OSSwapBigToHostInt16(*(unsigned short *)ap), OSSwapBigToHostInt32(*(unsigned int *)(ap + 2)),
                    OSSwapBigToHostInt32(*(unsigned int *)(ap + 10)), OSSwapBigToHostInt32(*(unsigned int *)(ap + 16)), len);
            for (k = 0; k < len && k < 256; k++) fprintf(stderr, "%s%02x", (k & 3) ? "" : " ", *(unsigned char *)(ap + 24 + k));
            fprintf(stderr, "\n");
        }
    }
    {   pid_t cpid = 0;
        unsigned int psn[2] = { 0, 0 };
        /* launchControlFlags (offset 14, not 10, which is launchEPBLength's low half):
           launchDontSwitch 0x0200, the same bit as kLSLaunchDontSwitch */
        g_ls_launch_flags = OSSwapBigToHostInt16(*(unsigned short *)(pb + (getenv("OAH_LAUNCHFLAGS_OLD") ? 10 : 14))) & 0x0200;
        int aefile = 0;
        /* launchAppParameters as the new process's first event: flattened here (addressed to the
           current process) into a file the child turns into its launch modifier (oah_aen.h), the way
           the 10.6 Process Manager handed it over. Sending it after the launch (below, the old way)
           reached Organization Chart before it had a handler (-1708). OAH_NO_LAUNCHAEMOD=1: send. */
        {   unsigned int ap = OSSwapBigToHostInt32(*(unsigned int *)(pb + 40));
            static unsigned int fcd, fco, fpa, fdd, fsz, ffl;
            static int seq;
            if (ap && !getenv("OAH_NO_LAUNCHEVENT") && !getenv("OAH_NO_LAUNCHAEMOD")) {
                unsigned int len = OSSwapBigToHostInt32(*(unsigned int *)(ap + 20)), d[2], ev[2], ad[2], a[6];
                unsigned int cur[2] = { 0, OSSwapHostToBigInt32(2) };   /* kCurrentProcess */
                if (!fcd) { fcd = oah_guest_sym("AE", "_AECreateDesc"); fco = oah_guest_sym("AE", "_AECoerceDesc");
                            fpa = oah_guest_sym("AE", "_AEPutAttributeDesc"); fdd = oah_guest_sym("AE", "_AEDisposeDesc");
                            fsz = oah_guest_sym("AE", "_AESizeOfFlattenedDesc"); ffl = oah_guest_sym("AE", "_AEFlattenDesc"); }
                if (fcd && fco && fpa && fdd && fsz && ffl && len < 0x100000) {
                    a[0] = 'appa'; a[1] = ap; a[2] = 24 + len; a[3] = (unsigned int)d;
                    if (!(short)oah_callppc_n("AECreateDesc", fcd, 4, a)) {
                        a[0] = (unsigned int)d; a[1] = 'aevt'; a[2] = (unsigned int)ev;
                        if (!(short)oah_callppc_n("AECoerceDesc", fco, 3, a)) {
                            unsigned int sz, got = 0; void *buf;
                            a[0] = 'psn '; a[1] = (unsigned int)cur; a[2] = 8; a[3] = (unsigned int)ad;
                            if (!(short)oah_callppc_n("AECreateDesc", fcd, 4, a)) {
                                a[0] = (unsigned int)ev; a[1] = 'addr'; a[2] = (unsigned int)ad;
                                oah_callppc_n("AEPutAttributeDesc", fpa, 3, a);
                                a[0] = (unsigned int)ad; oah_callppc_n("AEDisposeDesc", fdd, 1, a);
                            }
                            a[0] = (unsigned int)ev; sz = oah_callppc_n("AESizeOfFlattenedDesc", fsz, 1, a);
                            if (sz > 0 && sz < 0x200000 && (buf = malloc(sz))) {
                                a[0] = (unsigned int)ev; a[1] = (unsigned int)buf; a[2] = sz; a[3] = (unsigned int)&got;
                                if (!(short)oah_callppc_n("AEFlattenDesc", ffl, 4, a)) {
                                    FILE *fp;
                                    got = OSSwapBigToHostInt32(got);
                                    snprintf(g_ls_launch_ae, sizeof g_ls_launch_ae, "/tmp/oah_launchae.%d.%d", (int)getpid(), ++seq);
                                    if ((fp = fopen(g_ls_launch_ae, "wb"))) {
                                        aefile = fwrite(buf, 1, got ? got : sz, fp) == (got ? got : sz);
                                        fclose(fp);
                                    }
                                    if (!aefile) { unlink(g_ls_launch_ae); g_ls_launch_ae[0] = 0; }
                                }
                                free(buf);
                            }
                            a[0] = (unsigned int)ev; oah_callppc_n("AEDisposeDesc", fdd, 1, a);
                        }
                        a[0] = (unsigned int)d; oah_callppc_n("AEDisposeDesc", fdd, 1, a);
                    }
                }
                fprintf(stderr, "[ls] LaunchApplication: launch event %s\n", aefile ? g_ls_launch_ae : "not flattened (sent after the launch)");
            }
        }
        err = oah_launch_translated_pid(path, NULL, &cpid);
        if (aefile && g_ls_launch_ae[0]) {   /* not used: an already running instance took the launch */
            unlink(g_ls_launch_ae); g_ls_launch_ae[0] = 0; aefile = 0;
        }
        /* launchProcessSN: the caller talks to the new process through it right away (Word sends
           Microsoft Clip Gallery an Apple event and said "Word cannot launch Microsoft Clip Gallery"
           when it got procNotFound for PSN 0; the gallery then quit, "can only be launched from
           Office"). The app checks in with the window server within a few seconds of starting. */
        if (!err && cpid > 0) {
            static int (*gpfp)(pid_t, unsigned int *);
            int k, r = -1;
            if (!gpfp) gpfp = (int (*)(pid_t, unsigned int *))dlsym(RTLD_DEFAULT, "GetProcessForPID");
            for (k = 0; gpfp && k < 200 && (r = gpfp(cpid, psn)) != 0; k++) usleep(50000);
            fprintf(stderr, "[ls] LaunchApplication: pid %d -> PSN %u.%u (%d, %.1f s)\n", (int)cpid, psn[0], psn[1], r, k * 0.05);
            if (r) psn[0] = psn[1] = 0;
        }
        OSWriteBigInt32((void *)pb, 20, psn[0]);
        OSWriteBigInt32((void *)pb, 24, psn[1]);
        /* launchAppParameters: the Process Manager handed this high-level event to the new process as
           its first event. Word starts Organization Chart with DDE /EMBD (OLE embedding) this way;
           without it the chart said "can only be run from inside an Office application". Rebuild it
           in the guest (typeAppParameters -> typeAppleEvent), address it to the new PSN and send it
           (oah_fix_aesendmessage takes it to Mojave). OAH_NO_LAUNCHEVENT=1 disables. */
        {   unsigned int ap = OSSwapBigToHostInt32(*(unsigned int *)(pb + 40));
            if (ap && !aefile && (psn[0] || psn[1]) && !getenv("OAH_NO_LAUNCHEVENT")) {
                static unsigned int fcd, fco, fpa, fsm, fdd;
                unsigned int len = OSSwapBigToHostInt32(*(unsigned int *)(ap + 20)), d[2], ev[2], ad[2], pbe[2], a[6];
                int e1 = -1, e2 = -1, e3 = -1;
                if (!fcd) { fcd = oah_guest_sym("AE", "_AECreateDesc"); fco = oah_guest_sym("AE", "_AECoerceDesc");
                            fpa = oah_guest_sym("AE", "_AEPutAttributeDesc"); fsm = oah_guest_sym("AE", "_AESendMessage");
                            fdd = oah_guest_sym("AE", "_AEDisposeDesc"); }
                if (fcd && fco && fpa && fsm && fdd && len < 0x100000) {
                    a[0] = 'appa'; a[1] = ap; a[2] = 24 + len; a[3] = (unsigned int)d;
                    e1 = (short)oah_callppc_n("AECreateDesc", fcd, 4, a);
                    if (!e1) { a[0] = (unsigned int)d; a[1] = 'aevt'; a[2] = (unsigned int)ev; e2 = (short)oah_callppc_n("AECoerceDesc", fco, 3, a); }
                    if (!e2) {
                        pbe[0] = OSSwapHostToBigInt32(psn[0]); pbe[1] = OSSwapHostToBigInt32(psn[1]);
                        a[0] = 'psn '; a[1] = (unsigned int)pbe; a[2] = 8; a[3] = (unsigned int)ad;
                        if (!(short)oah_callppc_n("AECreateDesc", fcd, 4, a)) {
                            a[0] = (unsigned int)ev; a[1] = 'addr'; a[2] = (unsigned int)ad;
                            oah_callppc_n("AEPutAttributeDesc", fpa, 3, a);
                            a[0] = (unsigned int)ad; oah_callppc_n("AEDisposeDesc", fdd, 1, a);
                        }
                        a[0] = (unsigned int)ev; a[1] = 0; a[2] = 0x11; a[3] = (unsigned int)-1;   /* kAENoReply | kAENeverInteract */
                        e3 = (short)oah_callppc_n("AESendMessage", fsm, 4, a);
                        a[0] = (unsigned int)ev; oah_callppc_n("AEDisposeDesc", fdd, 1, a);
                    }
                    if (!e1) { a[0] = (unsigned int)d; oah_callppc_n("AEDisposeDesc", fdd, 1, a); }
                }
                fprintf(stderr, "[ls] LaunchApplication: launch event %.4s/%.4s -> desc %d coerce %d send %d\n",
                        (char *)&(unsigned int){ *(unsigned int *)(ap + 2) }, (char *)&(unsigned int){ *(unsigned int *)(ap + 10) }, e1, e2, e3);
            }
        }
    }
    fprintf(stderr, "[ls] LaunchApplication(%s) -> started under the translator (%d)\n", path, err);
    g_gt_skip = 1; g_gt_skip_ret = (unsigned int)err;
}
static int oah_want_launchappfix(void) { return !getenv("OAH_NO_LAUNCHAPPFIX"); }
/* PBHGetVInfo / PBXGetVolInfo with ioVolIndex > 0 (volume enumeration): the 10.6 File Manager
   walks its own volume list, filled by coreservicesd, which Mojave does not answer; index 1
   already gave nsvErr, so StuffIt's installer saw no volumes ("not enough room on """). Look the
   n-th mounted volume up (startup disk first, as FSGetVolumeInfo does) and run the call by
   vRefNum (ioVolIndex 0), then put the index back. Both blocks keep ioVRefNum at 22 and
   ioVolIndex at 28. OAH_NO_VOLINDEXFIX=1 disables. */
static int oah_mount_list(struct statfs **out);
static int g_volidx_saved[8], g_volidx_sp, g_volidx_busy;
static void oah_fix_volindex(unsigned int *R) {
    unsigned int pb = R[3];
    short idx, vref;
    struct statfs *m;
    int n, save = 0;
    if (getenv("OAH_VOLIDX_LOG")) fprintf(stderr, "[vol] PBHGetVInfo pb %08x index %d busy %d\n", pb, pb ? (short)OSSwapBigToHostInt16(*(unsigned short *)(pb + 28)) : 0, g_volidx_busy);
    if (pb && !g_volidx_busy) {
        idx = (short)OSSwapBigToHostInt16(*(unsigned short *)(pb + 28));
        if (idx > 0) {
            g_volidx_busy = 1;
            n = oah_mount_list(&m);
            if (getenv("OAH_VOLIDX_LOG") && idx > n) fprintf(stderr, "[vol] index %d past %d volumes\n", idx, n);
            if (idx > n) {                                   /* past the end: nsvErr, as before */
                g_volidx_busy = 0;
                OSWriteBigInt16((void *)pb, 16, (unsigned short)-35);
                g_gt_skip = 1; g_gt_skip_ret = (unsigned int)-35;
                return;
            }
            {   int vr = oah_guest_vref_for(m[idx - 1].f_mntonname, 0, &vref);
                if (getenv("OAH_VOLIDX_LOG")) fprintf(stderr, "[vol] index %d of %d: %s -> %d vref %d\n", idx, n, m[idx - 1].f_mntonname, vr, vr ? 0 : vref);
                if (!vr) {
                    OSWriteBigInt16((void *)pb, 22, (unsigned short)vref);
                    OSWriteBigInt16((void *)pb, 28, 0);   /* by ioVRefNum alone (-1 also looked at ioNamePtr, an output buffer) */
                    save = idx;
                }
            }
            g_volidx_busy = 0;
        }
    }
    if (g_volidx_sp < 8) g_volidx_saved[g_volidx_sp] = save;
    g_volidx_sp++;
}
/* The classic block counts: 10.6 picks ioVAlBlkSiz so the volume fits 65535 blocks, then caps
   ioVNmAlBlks and ioVFrBlk at 2 GB worth of blocks (old 32-bit size arithmetic). Here
   ioVNmAlBlks came back 0 and ioVFrBlk uncapped (813 GB free at 15 MB per block), so a
   size check overflowed. Recompute both from the mount table, as Snow Leopard reports them. */
/* guest vRefNum -> mount point (from the mount table, through the guest's own refnums) */
static int oah_vref_mount(short vref, struct statfs *out) {
    static struct { short vref; char mnt[MNAMELEN]; } map[32];
    static int nmap;
    struct statfs *m;
    int i, k, n;
    for (k = 0; k < 2; k++) {
        for (i = 0; i < nmap; i++)
            if (map[i].vref == vref && !statfs(map[i].mnt, out)) return 1;
        if (k) break;
        g_volidx_busy = 1;                                   /* (re)build the map */
        n = oah_mount_list(&m); nmap = 0;
        for (i = 0; i < n && nmap < 32; i++) {
            short v;
            if (!oah_guest_vref_for(m[i].f_mntonname, 0, &v)) {
                map[nmap].vref = v;
                strlcpy(map[nmap].mnt, m[i].f_mntonname, sizeof map[nmap].mnt);
                nmap++;
            }
        }
        g_volidx_busy = 0;
    }
    return 0;
}
/* The startup disk's blessed folder and boot file, as an HFS+ startup disk records them in its
   volume Finder info (Snow Leopard: [0] = /System/Library/CoreServices). APFS keeps none, and
   classic installers that look for "the disk with a System Folder" found no startup disk. */
static void oah_blessed_ids(unsigned int *dir, unsigned int *file) {
    struct stat st;
    *dir = *file = 0;
    if (!stat("/System/Library/CoreServices", &st)) *dir = (unsigned int)st.st_ino;
    if (!stat("/System/Library/CoreServices/boot.efi", &st)) *file = (unsigned int)st.st_ino;
}
static void oah_volblocks_fix(unsigned int pb) {
    unsigned int a = OSReadBigInt32((void *)pb, 48);
    unsigned int nm = OSReadBigInt16((void *)pb, 46), fr = OSReadBigInt16((void *)pb, 62);
    short vref = (short)OSReadBigInt16((void *)pb, 22);
    unsigned long long tot = 0, fre = 0, lim;
    struct statfs sf;
    int have = oah_vref_mount(vref, &sf);
    if (have) {
        tot = (unsigned long long)sf.f_blocks * sf.f_bsize;
        fre = (unsigned long long)sf.f_bavail * sf.f_bsize;
        if (!strcmp(sf.f_fstypename, "apfs") && !getenv("OAH_NO_APFS_HPLUS")) {
            /* ioVFSID: 0 for the native (HFS) file system; APFS came back as a foreign one */
            OSWriteBigInt16((void *)pb, 70, 0);
            if (!strcmp(sf.f_mntonname, "/") && !OSReadBigInt32((void *)pb, 90)) {
                unsigned int d, f;
                oah_blessed_ids(&d, &f);
                OSWriteBigInt32((void *)pb, 90, d);          /* ioVFndrInfo[0]: blessed folder */
                OSWriteBigInt32((void *)pb, 94, f);          /* [1]: startup file */
            }
        }
    }
    if (!a || nm) return;
    lim = 0x7fffffffULL / a; if (lim > 0xffff) lim = 0xffff;
    if (tot) {
        nm = (unsigned int)(tot / a < lim ? tot / a : lim);
        fr = (unsigned int)(fre / a < nm ? fre / a : nm);
    } else {
        nm = (unsigned int)lim;
        if (fr > nm) fr = nm;
    }
    OSWriteBigInt16((void *)pb, 46, (unsigned short)nm);
    OSWriteBigInt16((void *)pb, 62, (unsigned short)fr);
}
static void oah_fixret_volindex(unsigned int *R, unsigned int pb) {
    int save = 0;
    if (g_volidx_sp > 0) { g_volidx_sp--; if (g_volidx_sp < 8) save = g_volidx_saved[g_volidx_sp]; }
    if (save && pb) OSWriteBigInt16((void *)pb, 28, (unsigned short)save);
    if (pb && R[3] == 0 && !g_volidx_busy && !getenv("OAH_NO_VOLBLOCKFIX")) oah_volblocks_fix(pb);
}
static int oah_want_volindexfix(void) { return !getenv("OAH_NO_VOLINDEXFIX"); }
/* _FSCopyVolumeProperty(vref, ?, CFStringRef key, ...): coreservicesd stored each volume's
   Disk Arbitration media icon as the properties kFSIOMediaIconBundleIdentifier and
   kFSIOMediaIconFamilyName; on Mojave nobody does, IconServices found no icon for any volume
   and every disk in open panels and the sidebar was drawn as a folder. Answer them from Disk
   Arbitration's DAMediaIcon for the volume's device. OAH_NO_VOLICONFIX=1 disables. */
static unsigned int oah_cf_to_guest(CFTypeRef v);
static unsigned int g_vprop_key[8];
static int g_vprop_sp;
static int oah_guest_cfconst_is(unsigned int s, const char *want) {
    unsigned int cp, cl;
    if (s < 0x1000 || OSReadBigInt32((void *)s, 4) != 0x7c8) return 0;
    cp = OSReadBigInt32((void *)s, 8); cl = OSReadBigInt32((void *)s, 12);
    return cp > 0x1000 && cl == strlen(want) && !memcmp((void *)cp, want, cl);
}
static void oah_fix_volprop(unsigned int *R) {
    if (g_vprop_sp < 8) g_vprop_key[g_vprop_sp] = R[5];
    g_vprop_sp++;
}
static void oah_fixret_volprop(unsigned int *R, unsigned int a0) {
    static void *sess;
    static void *(*mk)(CFAllocatorRef, void *, const char *);
    static CFDictionaryRef (*desc)(void *);
    unsigned int key = 0;
    int which;
    struct statfs sf;
    if (g_vprop_sp > 0) { g_vprop_sp--; if (g_vprop_sp < 8) key = g_vprop_key[g_vprop_sp]; }
    if (R[3] || !key || g_volidx_busy) return;
    if (oah_guest_cfconst_is(key, "kFSIOMediaIconBundleIdentifier")) which = 0;
    else if (oah_guest_cfconst_is(key, "kFSIOMediaIconFamilyName")) which = 1;
    else return;
    if (!oah_vref_mount((short)a0, &sf) || strncmp(sf.f_mntfromname, "/dev/", 5)) return;
    if (!sess) {
        void *h = dlopen("/System/Library/Frameworks/DiskArbitration.framework/DiskArbitration", RTLD_LAZY | RTLD_GLOBAL);
        void *(*sc)(CFAllocatorRef) = h ? (void *(*)(CFAllocatorRef))dlsym(h, "DASessionCreate") : 0;
        if (h) { *(void **)&mk = dlsym(h, "DADiskCreateFromBSDName"); *(void **)&desc = dlsym(h, "DADiskCopyDescription"); }
        if (sc && mk && desc) sess = sc(NULL);
        if (!sess) return;
    }
    {
        void *disk = mk(NULL, sess, sf.f_mntfromname + 5);
        CFDictionaryRef d = disk ? desc(disk) : NULL;
        CFDictionaryRef icon = d ? (CFDictionaryRef)CFDictionaryGetValue(d, CFSTR("DAMediaIcon")) : NULL;
        CFTypeRef v = (icon && CFGetTypeID(icon) == CFDictionaryGetTypeID())
                          ? CFDictionaryGetValue(icon, which ? CFSTR("IOBundleResourceFile") : CFSTR("CFBundleIdentifier")) : NULL;
        if (v) R[3] = oah_cf_to_guest(v);
        if (getenv("OAH_VERBOSE")) fprintf(stderr, "[vol] %s for %s -> %08x\n", which ? "kFSIOMediaIconFamilyName" : "kFSIOMediaIconBundleIdentifier",
                                           sf.f_mntonname, R[3]);
        if (d) CFRelease(d);
        if (disk) CFRelease((CFTypeRef)disk);
    }
}
static int oah_want_voliconfix(void) { return !getenv("OAH_NO_VOLICONFIX"); }
/* CGContextCopyWindowContentsToRect(ctx, CGRect dest, cid, wid, CGRect src): AppKit scrolls by
   copying the window's own pixels (NSCopyBits) and redrawing only the exposed strip. The 10.6
   implementation asks the window server for the bits; Mojave's has no such window (ours lives in
   a guest-side store) and the call fails (1000), so scrolled views showed stale rows over new
   ones (sidebar and file list of open panels). When ctx draws on the same window, copy the rows
   inside the store. CG passes both rects in GPRs (r4-r7 dest, r10 + stack src); src is in window
   coordinates (top-left origin, points), dest in ctx user space. OAH_NO_COPYWINFIX=1 disables,
   OAH_COPYWIN_PROBE=1 logs the calls. */
static void oah_fix_copywin(unsigned int *R) {
    static unsigned int getctm; static int probe = -1;
    float m[6] = { 1, 0, 0, 1, 0, 0 }, d[4], s[4];
    unsigned int a[2], i, sw[4], *sp = (unsigned int *)(R[1] + 56);
    oah_cgswin_t *w = oah_cgs_win(R[9]), *cw = oah_cgs_win_by_ctx(R[3]);
    if (probe < 0) probe = getenv("OAH_COPYWIN_PROBE") ? 1 : 0;
    memcpy(d, &R[4], 16);
    sw[0] = R[10]; sw[1] = OSSwapBigToHostInt32(sp[0]); sw[2] = OSSwapBigToHostInt32(sp[1]); sw[3] = OSSwapBigToHostInt32(sp[2]);
    memcpy(s, sw, 16);
    if (!getctm) getctm = oah_guest_sym("CoreGraphics", "_CGContextGetCTM");
    if (getctm) {
        unsigned int out[6];
        a[0] = (unsigned int)out; a[1] = R[3];
        oah_callppc_n("CGContextGetCTM", getctm, 2, a);
        for (i = 0; i < 6; i++) { unsigned int v = OSSwapBigToHostInt32(out[i]); memcpy(&m[i], &v, 4); }
    }
    if (probe)
        fprintf(stderr, "[copywin] ctx %08x (window %u) wid %u dest %.1f,%.1f %.1fx%.1f src %.1f,%.1f %.1fx%.1f ctm [%.2f %.2f %.2f %.2f %.1f %.1f]\n",
                R[3], cw ? cw->wid : 0, R[9], d[0], d[1], d[2], d[3], s[0], s[1], s[2], s[3], m[0], m[1], m[2], m[3], m[4], m[5]);
    if (!w || w != cw || !w->store || m[1] != 0 || m[2] != 0 || m[0] <= 0 || m[3] <= 0) return;
    {
        int sc = WSC(w), H = (int)(w->h * sc + 0.5f), W = (int)(w->w * sc + 0.5f), rb = WRB(w), y, n;
        int sx = (int)(s[0] * sc + 0.5f), sy = (int)(s[1] * sc + 0.5f);
        int dx = (int)(m[0] * d[0] + m[4] + 0.5f), dw = (int)(m[0] * d[2] + 0.5f), dh = (int)(m[3] * d[3] + 0.5f);
        int dy = H - (int)(m[3] * d[1] + m[5] + 0.5f) - dh;     /* top row in the window */
        int cw_ = (int)(s[2] * sc + 0.5f), ch = (int)(s[3] * sc + 0.5f);
        unsigned char *bits = oah_cgs_bits(w);
        if (H > WROWS(w)) H = WROWS(w);
        if (W > rb / 4) W = rb / 4;
        if (dw < cw_) cw_ = dw;
        if (dh < ch) ch = dh;
        /* clip both rects to the window */
        if (sx < 0) { dx -= sx; cw_ += sx; sx = 0; }
        if (dx < 0) { sx -= dx; cw_ += dx; dx = 0; }
        if (sy < 0) { dy -= sy; ch += sy; sy = 0; }
        if (dy < 0) { sy -= dy; ch += dy; dy = 0; }
        if (sx + cw_ > W) cw_ = W - sx;
        if (dx + cw_ > W) cw_ = W - dx;
        if (sy + ch > H) ch = H - sy;
        if (dy + ch > H) ch = H - dy;
        if (cw_ > 0 && ch > 0) {
            n = cw_ * 4;
            if (dy <= sy) for (y = 0; y < ch; y++) memmove(bits + (size_t)(dy + y) * rb + dx * 4, bits + (size_t)(sy + y) * rb + sx * 4, n);
            else for (y = ch - 1; y >= 0; y--) memmove(bits + (size_t)(dy + y) * rb + dx * 4, bits + (size_t)(sy + y) * rb + sx * 4, n);
            if (probe) fprintf(stderr, "[copywin] copied %dx%d from %d,%d to %d,%d (window px)\n", cw_, ch, sx, sy, dx, dy);
        }
        g_gt_skip = 1; g_gt_skip_ret = 0;
    }
}
static int oah_want_copywin(void) { return !getenv("OAH_NO_COPYWINFIX"); }
/* CGContextCopyDisplayCaptureContentsToRect(ctx, CGRect dest, cid, CGDirectDisplayID, CGRect src):
   AppKit's _captureRegion, behind the Colors panel's magnifier (and NSReadPixel-style screen
   sampling). The guest CoreGraphics asks a window server it does not have and left the context
   untouched: the loupe showed black and every colour picked from the screen was black. The screen
   is read natively (CGDisplayCreateImageForRect) and drawn into the guest bitmap context's pixels.
   Only 32-bit RGBA/ARGB bitmap contexts in the default byte order with a scale-only CTM are handled; anything
   else falls through. OAH_NO_DISPCAPFIX=1 disables, OAH_COPYWIN_PROBE=1 logs. */
static void oah_fix_dispcap(unsigned int *R) {
    static unsigned int getctm, gdata, gw, gh, gbpr, gbpp, ginfo;
    static int probe = -1;
    float m[6] = { 1, 0, 0, 1, 0, 0 }, d[4], s[4];
    unsigned int a[2], i, sw[4], *sp = (unsigned int *)(R[1] + 56), data, W, H, bpr, bpp, info, disp = R[9];
    if (probe < 0) probe = getenv("OAH_COPYWIN_PROBE") ? 1 : 0;
    memcpy(d, &R[4], 16);
    sw[0] = R[10]; sw[1] = OSSwapBigToHostInt32(sp[0]); sw[2] = OSSwapBigToHostInt32(sp[1]); sw[3] = OSSwapBigToHostInt32(sp[2]);
    memcpy(s, sw, 16);
    if (!getctm) {
        getctm = oah_guest_sym("CoreGraphics", "_CGContextGetCTM");
        gdata = oah_guest_sym("CoreGraphics", "_CGBitmapContextGetData");
        gw = oah_guest_sym("CoreGraphics", "_CGBitmapContextGetWidth");
        gh = oah_guest_sym("CoreGraphics", "_CGBitmapContextGetHeight");
        gbpr = oah_guest_sym("CoreGraphics", "_CGBitmapContextGetBytesPerRow");
        gbpp = oah_guest_sym("CoreGraphics", "_CGBitmapContextGetBitsPerPixel");
        ginfo = oah_guest_sym("CoreGraphics", "_CGBitmapContextGetBitmapInfo");
    }
    if (!getctm || !gdata || !gw || !gh || !gbpr || !gbpp || !ginfo || !R[3]) {
        if (probe) fprintf(stderr, "[dispcap] ctx %08x: guest bitmap functions missing (%x %x %x %x %x %x %x)\n", R[3], getctm, gdata, gw, gh, gbpr, gbpp, ginfo);
        return;
    }
    {   unsigned int out[6];
        a[0] = (unsigned int)(uintptr_t)out; a[1] = R[3];
        oah_callppc_n("CGContextGetCTM", getctm, 2, a);
        for (i = 0; i < 6; i++) { unsigned int v = OSSwapBigToHostInt32(out[i]); memcpy(&m[i], &v, 4); }
    }
    a[0] = R[3];
    data = oah_callppc_n("CGBitmapContextGetData", gdata, 1, a);
    W = oah_callppc_n("CGBitmapContextGetWidth", gw, 1, a);
    H = oah_callppc_n("CGBitmapContextGetHeight", gh, 1, a);
    bpr = oah_callppc_n("CGBitmapContextGetBytesPerRow", gbpr, 1, a);
    bpp = oah_callppc_n("CGBitmapContextGetBitsPerPixel", gbpp, 1, a);
    info = oah_callppc_n("CGBitmapContextGetBitmapInfo", ginfo, 1, a);
    if (probe)
        fprintf(stderr, "[dispcap] ctx %08x display %u dest %.1f,%.1f %.1fx%.1f src %.1f,%.1f %.1fx%.1f ctm [%.2f %.2f %.2f %.2f %.1f %.1f] bitmap %08x %ux%u bpr %u bpp %u info %x\n",
                R[3], disp, d[0], d[1], d[2], d[3], s[0], s[1], s[2], s[3], m[0], m[1], m[2], m[3], m[4], m[5], data, W, H, bpr, bpp, info);
    /* 32 bpp RGBA/ARGB (premultiplied or skip, not straight alpha), default byte order (bytes in memory order) */
    if (!data || bpp != 32 || (info & 0x7000) != 0 || (info & 0x1f) < 1 || (info & 0x1f) > 6 || (info & 0x1f) == 3 || (info & 0x1f) == 4 ||
        m[1] != 0 || m[2] != 0 || m[0] <= 0 || m[3] <= 0 || s[2] <= 0 || s[3] <= 0) return;
    {   /* native CoreGraphics by dlsym (i386: CGFloat is float) */
        typedef struct { float x, y, w, h; } rect_t;
        static rect_t (*bounds)(unsigned int);
        static unsigned int (*mainid)(void);
        static void *(*capture)(unsigned int, rect_t);
        static void *(*mkcs)(void);
        static void *(*mkbm)(void *, size_t, size_t, size_t, size_t, void *, unsigned int);
        static void (*interp)(void *, int), (*draw)(void *, rect_t, void *), (*crel)(void *), (*csrel)(void *), (*irel)(void *);
        int dx = (int)floorf(m[0] * d[0] + m[4] + 0.5f), dw = (int)floorf(m[0] * d[2] + 0.5f);
        int dyb = (int)floorf(m[3] * d[1] + m[5] + 0.5f), dh = (int)floorf(m[3] * d[3] + 0.5f);
        rect_t b, sr = { s[0], s[1], s[2], s[3] }, dr = { 0, 0, 0, 0 };
        void *img;
        unsigned char *tmp;
        int y;
        if (!draw) {
            *(void **)&bounds = oah_ncgs("CGDisplayBounds"); *(void **)&mainid = oah_ncgs("CGMainDisplayID");
            *(void **)&capture = oah_ncgs("CGDisplayCreateImageForRect"); *(void **)&mkcs = oah_ncgs("CGColorSpaceCreateDeviceRGB");
            *(void **)&mkbm = oah_ncgs("CGBitmapContextCreate"); *(void **)&interp = oah_ncgs("CGContextSetInterpolationQuality");
            *(void **)&crel = oah_ncgs("CGContextRelease"); *(void **)&csrel = oah_ncgs("CGColorSpaceRelease");
            *(void **)&irel = oah_ncgs("CGImageRelease"); *(void **)&draw = oah_ncgs("CGContextDrawImage");
        }
        if (!bounds || !mainid || !capture || !mkcs || !mkbm || !interp || !crel || !csrel || !irel || !draw) return;
        b = bounds(disp);
        if (b.w <= 0 || b.h <= 0) disp = mainid();
        if (dw <= 0 || dh <= 0 || dw > 8192 || dh > 8192) return;
        img = capture(disp, sr);
        if (!img) { if (probe) fprintf(stderr, "[dispcap] no display image\n"); return; }
        tmp = (unsigned char *)calloc((size_t)dw * dh, 4);
        if (tmp) {
            void *cs = mkcs();
            void *c = mkbm(tmp, dw, dh, 8, dw * 4, cs, info & 0x1f);   /* the guest's layout, bytes in memory order */
            if (c) {
                interp(c, 1 /* none */);
                dr.w = (float)dw; dr.h = (float)dh;
                draw(c, dr, img);
                crel(c);
                /* bitmap memory: row 0 is the top; the device rect is measured from the bottom */
                for (y = 0; y < dh; y++) {
                    int row = (int)H - (dyb + dh) + y, x0 = dx, x1 = dx + dw, sx = 0;
                    if (row < 0 || row >= (int)H) continue;
                    if (x0 < 0) { sx = -x0; x0 = 0; }
                    if (x1 > (int)W) x1 = (int)W;
                    if (x1 > x0) memcpy((unsigned char *)(uintptr_t)data + (size_t)row * bpr + (size_t)x0 * 4, tmp + (size_t)y * dw * 4 + (size_t)sx * 4, (size_t)(x1 - x0) * 4);
                }
            }
            csrel(cs);
            free(tmp);
        }
        irel(img);
        if (probe) fprintf(stderr, "[dispcap] drew %dx%d at %d,%d (from bottom) from display %u\n", dw, dh, dx, dyb, disp);
        g_gt_skip = 1; g_gt_skip_ret = 0;
    }
}
static int oah_want_dispcap(void) { return !getenv("OAH_NO_DISPCAPFIX"); }
/* -[IMKClient _launch:fromBundle:throughPort:]: 10.6 HIToolbox asks the input-method launcher
   (a CFMessagePort request, id 8000) to start an InputMethodKit server. Mojave's service answers in
   a format the 10.6 CFMessagePort drops ("dropping corrupt reply Mach message"), so every request
   waited out its receive timeout - and HIToolbox makes one each time a TSM document is deactivated,
   i.e. every time a menu opens: menus took 5 to 15 s to appear in every PowerPC app once a
   non-keyboard input method (the Character Viewer, after Edit > Special Characters) had been
   selected. No 10.6 input-method server can start here, so the request fails at once.
   OAH_NO_IMKLAUNCHFIX=1 disables. */
static void oah_fix_imklaunch(unsigned int *R) {
    static int logged;
    if (!logged++) fprintf(stderr, "[imk] input method launch request refused (no 10.6 input-method servers on Mojave)\n");
    R[3] = (unsigned int)-2;                         /* kCFMessagePortReceiveTimeout */
    g_gt_skip = 1; g_gt_skip_ret = R[3];
}
static int oah_want_imklaunchfix(void) { return !getenv("OAH_NO_IMKLAUNCHFIX"); }
/* LaunchAppleCharacterPalette (Edit > Special Characters): the 10.6 call selects the Character
   Palette input source in the user's ByHost com.apple.HIToolbox preferences (shared with Mojave)
   and then asks for the palette server, which cannot start here: no palette opened, and the
   input source stayed selected for every app. It is a no-op now. OAH_NO_CHARPALFIX=1 disables. */
static void oah_fix_charpal(unsigned int *R) {
    fprintf(stderr, "[imk] Special Characters: the Snow Leopard character palette is not available\n");
    R[3] = 0; g_gt_skip = 1; g_gt_skip_ret = 0;
}
static int oah_want_charpalfix(void) { return !getenv("OAH_NO_CHARPALFIX"); }
/* CGSGetDisplaysWithRect / WithPoint are computed inside the guest's CoreGraphics from window-server
   state it does not have (they return 1011): answered natively by oah_cgs_call. */
static void oah_fix_dispswithrect(unsigned int *R) { if (oah_cgs_call("CGSGetDisplaysWithRect", R)) { g_gt_skip = 1; g_gt_skip_ret = R[3]; } }
static void oah_fix_dispswithpoint(unsigned int *R) { if (oah_cgs_call("CGSGetDisplaysWithPoint", R)) { g_gt_skip = 1; g_gt_skip_ret = R[3]; } }
/* AppKit's _getDisplayFormat: the screen capture is skipped unless the display reports a known
   pixel encoding (the guest CoreGraphics had none to give) */
static void oah_fix_dispenc(unsigned int *R) { if (oah_cgs_call("CGSGetDisplayPixelEncodingOfLength", R)) { g_gt_skip = 1; g_gt_skip_ret = R[3]; } }
#include "oah_gexc.h"
#include "oah_speech.h"
#include "oah_qtswf.h"
/* GetScriptManagerVariable(smKCHRCache = 38): 10.6 filled the KCHR cache when HIToolbox initialised the
   Script Manager's keyboard hooks ('scrx'); a program that asks before any HIToolbox call (a tool, or an
   app reading it at launch) got NULL. Answer the current keyboard layout's KCHR data, as 10.6 did.
   OAH_NO_KCHRFIX=1 disables. */
static void oah_fixret_smvar(unsigned int *R, unsigned int sel) {
    static unsigned int getcur, getprop;
    unsigned int a[3], kl = 0, data = 0;
    if (sel != 38 || R[3]) return;
    if (!getcur) { getcur = oah_guest_sym("HIToolbox", "_KLGetCurrentKeyboardLayout"); getprop = oah_guest_sym("HIToolbox", "_KLGetKeyboardLayoutProperty"); }
    if (!getcur || !getprop) return;
    a[0] = (unsigned int)&kl;
    if (oah_callppc_n("KLGetCurrentKeyboardLayout", getcur, 1, a) || !kl) return;
    kl = OSSwapBigToHostInt32(kl);
    a[0] = kl; a[1] = 0 /* kKLKCHRData */; a[2] = (unsigned int)&data;
    if (oah_callppc_n("KLGetKeyboardLayoutProperty", getprop, 3, a)) return;
    data = OSSwapBigToHostInt32(data);
    if (data) R[3] = data;
}
static int oah_want_kchrfix(void) { return !getenv("OAH_NO_KCHRFIX"); }
/* Thread Manager YieldToThread (YieldToAnyThread goes through it): while this thread is parked,
   let other guest threads call into the guest (see oah_callppc_release_all). OAH_NO_YIELDFIX=1. */
static pthread_key_t g_yield_key;
static pthread_once_t g_yield_once = PTHREAD_ONCE_INIT;
static void oah_yield_key_init(void) { pthread_key_create(&g_yield_key, 0); }
static void oah_fix_yield(unsigned int *R) {
    int n;
    (void)R;
    pthread_once(&g_yield_once, oah_yield_key_init);
    n = oah_callppc_release_all();
    pthread_setspecific(g_yield_key, (void *)(intptr_t)n);
}
static void oah_fixret_yield(unsigned int *R, unsigned int a0) {
    int n;
    (void)R; (void)a0;
    pthread_once(&g_yield_once, oah_yield_key_init);
    n = (int)(intptr_t)pthread_getspecific(g_yield_key);
    pthread_setspecific(g_yield_key, 0);
    oah_callppc_reacquire(n);
}
static int oah_want_yieldfix(void) { return !getenv("OAH_NO_YIELDFIX"); }
/* The same for a guest thread that blocks in pthread_cond_wait while it holds the callback lock.
   Our Apple Event dispatch runs a guest handler through oah_callppc_n, so the lock is held for
   the handler's whole run. Pages' odoc handler (Template Chooser up) closed the chooser and
   waited for AppKit's NSViewHierarchyLock, held by an AppKit drawing thread that was itself
   waiting for the callback lock in a bridge call: both stopped for good (SIGUSR2 dump). While a
   thread waits on a condition it runs no guest code, so the lock is given up for the wait and
   taken again before the thread goes on. OAH_NO_CVWAITFIX=1 disables. */
static pthread_key_t g_cvw_key;
static pthread_once_t g_cvw_once = PTHREAD_ONCE_INIT;
static void oah_cvw_key_init(void) { pthread_key_create(&g_cvw_key, 0); }
static void oah_fix_cvwait(unsigned int *R) {
    int n;
    (void)R;
    pthread_once(&g_cvw_once, oah_cvw_key_init);
    n = oah_callppc_release_all();
    pthread_setspecific(g_cvw_key, (void *)(intptr_t)n);
}
static void oah_fixret_cvwait(unsigned int *R, unsigned int a0) {
    int n;
    (void)R; (void)a0;
    pthread_once(&g_cvw_once, oah_cvw_key_init);
    n = (int)(intptr_t)pthread_getspecific(g_cvw_key);
    pthread_setspecific(g_cvw_key, 0);
    oah_callppc_reacquire(n);
}
static int oah_want_cvwfix(void) { return !getenv("OAH_NO_CVWAITFIX"); }
/* The same for CFMessagePortSendRequest. A Carbon app's copied data is often only promised; the
   clipboard sync thread reads it (inside a guest call, so holding the callback lock) and CFPasteboard
   - which reaches the pasteboard server and the promising app through CFMessagePort requests - waits
   for an answer this process's main thread must give. If the main thread was itself waiting for the
   callback lock in a bridged call, neither went on (TextWrangler: Cmd-C, then switching to another
   app, stopped the app for good). While a thread waits for a reply it runs no guest code, so a thread
   holding the lock gives it up for the request (nested calls keep their own counts).
   (Hooking mach_msg itself instead broke start-up: the hook's own set-up sends messages.)
   OAH_NO_MSGPORTFIX=1 disables. */
typedef struct { int sp; int n[32]; } oah_mpw_t;
static pthread_key_t g_mpw_key;
static pthread_once_t g_mpw_once = PTHREAD_ONCE_INIT;
static void oah_mpw_key_init(void) { pthread_key_create(&g_mpw_key, free); }
static oah_mpw_t *oah_mpw(void) {
    oah_mpw_t *s;
    pthread_once(&g_mpw_once, oah_mpw_key_init);
    if (!(s = (oah_mpw_t *)pthread_getspecific(g_mpw_key)) && (s = (oah_mpw_t *)calloc(1, sizeof *s))) pthread_setspecific(g_mpw_key, s);
    return s;
}
static void oah_fix_msgport(unsigned int *R) {
    oah_mpw_t *s = oah_mpw();
    int n = oah_callppc_release_all();
    (void)R;
    if (!s) { oah_callppc_reacquire(n); return; }
    if (s->sp < 32) s->n[s->sp] = n;
    else if (n) oah_callppc_reacquire(n);       /* too deep: keep the lock for this one */
    s->sp++;
}
static void oah_fixret_msgport(unsigned int *R, unsigned int a0) {
    oah_mpw_t *s = oah_mpw();
    (void)R; (void)a0;
    if (!s || s->sp <= 0) return;
    s->sp--;
    if (s->sp < 32) oah_callppc_reacquire(s->n[s->sp]);
}
static int oah_want_msgportfix(void) { return !getenv("OAH_NO_MSGPORTFIX"); }
/* Pasteboard-server watchdog. The translated pasteboard server (pboard) can get into a broken state
   ("spin lock ... neither locked nor unlocked. The memory has been smashed"): GoLive and InDesign then hang at
   start-up in CFPasteboardCreate and copies no longer reach Mojave until it is restarted. Calls that
   talk to it (CFPasteboardCreate / CFPasteboardSynchronize, from the app or from the clipboard sync) are
   counted; when one has been waiting OAH_PBOARD_STALL seconds (default 20), the agent is restarted
   (launchctl kickstart -k gui/UID/com.rosettus.ppc.pboard) - the waiting call then fails and the next one
   reaches the new server - at most once every 2 minutes. OAH_NO_PBWATCH=1 disables. */
static volatile int g_pb_inflight;
static volatile double g_pb_t0;
static void *oah_pb_watch(void *arg) {
    double limit = 20, last = 0;
    const char *e = getenv("OAH_PBOARD_STALL");
    (void)arg;
    if (e && atof(e) > 0) limit = atof(e);
    for (;;) {
        double now, t0;
        sleep(2);
        if (g_pb_inflight <= 0) continue;
        t0 = g_pb_t0; now = CFAbsoluteTimeGetCurrent();
        if (!t0 || now - t0 < limit || now - last < 120) continue;
        last = now;
        {   char target[96]; pid_t pid; int st = 0;
            char *argv[] = { "/bin/launchctl", "kickstart", "-k", target, 0 }, *envp[] = { "PATH=/usr/bin:/bin", 0 };
            snprintf(target, sizeof target, "gui/%u/com.rosettus.ppc.pboard", (unsigned)getuid());
            fprintf(stderr, "[pbwatch] a pasteboard call has waited %.0f s: restarting the translated pasteboard server\n", now - t0);
            if (!posix_spawn(&pid, argv[0], 0, 0, argv, envp)) waitpid(pid, &st, 0);
            fprintf(stderr, "[pbwatch] launchctl kickstart -> %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        }
    }
    return 0;
}
static void oah_fix_pbcall(unsigned int *R) {
    static int started;
    (void)R;
    if (__sync_fetch_and_add(&g_pb_inflight, 1) == 0) g_pb_t0 = CFAbsoluteTimeGetCurrent();
    if (!started && __sync_bool_compare_and_swap(&started, 0, 1)) {
        pthread_t t; pthread_attr_t at;
        pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_create(&t, &at, oah_pb_watch, 0);
        pthread_attr_destroy(&at);
    }
}
static void oah_fixret_pbcall(unsigned int *R, unsigned int a0) {
    (void)R; (void)a0;
    if (__sync_sub_and_fetch(&g_pb_inflight, 1) <= 0) { g_pb_inflight = 0; g_pb_t0 = 0; }
    else g_pb_t0 = CFAbsoluteTimeGetCurrent();      /* another call still waits: time it from now */
}
static int oah_want_pbwatch(void) { return !getenv("OAH_NO_PBWATCH"); }
/* The same release around a guest run loop (CFRunLoopRunSpecific: nested or modal loops). A launch
   document arrives as an odoc dispatched on the main thread through a guest call, so the lock was
   held for the whole handler; Keynote then opens the file in a modal "Progress..." loop while its
   loader threads called LaunchServices bridges that make guest calls of their own: they waited for
   the lock, the main thread waited for them, and the document never opened. OAH_NO_RLRELEASE=1
   keeps the lock. */
/* Not in Java (with it every PowerPC JVM stops at start: the launcher's main thread parks in
   CFRunLoopRunSpecific while the JVM thread starts, and with the lock released the JVM thread jumped into
   code the translator did not know yet: "translate fatal(2)"). The java tools (JavaVM.framework/Versions/
   x/Commands) and JavaApplicationStub apps keep the lock; OAH_RLRELEASE_JAVA=1 releases it anyway. */
static int oah_java_exe(void) {
    const char *x = oah_env_exe(), *b;
    if (!x) return 0;
    b = strrchr(x, '/'); b = b ? b + 1 : x;
    return (strstr(x, "/JavaVM.framework/") && strstr(x, "/Commands/")) || !strcmp(b, "JavaApplicationStub");
}
static int oah_want_rlrelease(void) {
    if (getenv("OAH_NO_RLRELEASE")) return 0;
    if (oah_java_exe() && !getenv("OAH_RLRELEASE_JAVA")) return 0;
    return 1;
}
/* Java 1.4.2's AWT (libawt.jnilib, NativeMethodEnter) asks +[NSAutoreleasePool autoreleasePoolExists]
   and, when Snow Leopard's Foundation says NO, calls AskUserToAttachDebugger, which prints "Please Attach
   Debugger : No Autorelease pool exists / Press any key to continue..." and waits on stdin: every Swing
   program hung there. It is a debugging aid only (the method goes on the same way afterwards): skip it.
   OAH_NO_AWTDEBUGFIX=1 keeps the prompt. */
static void oah_fix_awtdebugger(unsigned int *R) {
    static int n;
    if (n++ < 3) fprintf(stderr, "[java] AWT debugger prompt skipped: %s\n", R[3] > 0x1000 && oah_readable(R[3], 4) ? (const char *)R[3] : "?");
    g_gt_skip = 1; g_gt_skip_ret = 0;
}
static int oah_want_awtdebugfix(void) { return !getenv("OAH_NO_AWTDEBUGFIX"); }
/* libauto's Thread::clear_stack zeroes the stack between the lowest stack pointer it saw last
   time (Thread+0x20) and the current one. Native-to-guest callbacks (dock menu, Apple Events,
   timers delivered by liboah) run guest code on the translator's callback stack, a separate
   small region far below the main stack, so the saved mark could belong to that stack; the
   next clear on the main stack then zeroed everything between the two and faulted on a guard
   page (ScreenFlow crashed in clear_stack after "New Recording" from its helper's dock menu).
   When the mark is not on the current stack (more than 8 MB away), only move the mark.
   OAH_NO_GCCLEARFIX=1 disables. */
static void oah_fix_gcclear(unsigned int *R) {
    unsigned int t = R[3], old, sp = R[1];
    static int n;
    if (!t || !oah_readable(t + 0x20, 4)) return;
    old = OSSwapBigToHostInt32(*(unsigned int *)(t + 0x20));
    if (old < sp && sp - old > 0x800000u) {
        *(unsigned int *)(t + 0x20) = OSSwapHostToBigInt32(sp - 0x50);
        if (n++ < 3) fprintf(stderr, "[gc] clear_stack: mark %08x is not on this stack (r1 %08x), not cleared\n", old, sp);
        g_gt_skip = 1; g_gt_skip_ret = 0;
    }
}
static int oah_want_gcclearfix(void) { return !getenv("OAH_NO_GCCLEARFIX"); }
/* RosettusFlash.component (the Flash 5 media handler and importer of QuickTime 6.5, built by
   tools/mkflash.sh from the user's own QuickTime 6 installer) lives in the guest tree's
   /System/Library/QuickTime, but the guest Component Manager lists that folder by its
   (Mojave) directory ID and never saw it. Register it when the app starts QuickTime.
   OAH_NO_QTFLASH=1 leaves it out. */
static int g_cam_idx; static void oah_fix_cam_vdig(unsigned int *R); static void oah_cam_register(void);   /* oah_cam.h */
static void oah_fix_entermovies(unsigned int *R) {
    static int done;
    static unsigned char fsref[80];
    static const char gp[] = "/System/Library/QuickTime/RosettusFlash.component";
    char t[1200]; struct stat sb;
    unsigned int fm, fr, a[3], e1, e2;
    (void)R;
    if (done) return;
    done = 1;
    oah_cam_register();
    snprintf(t, sizeof t, "%s%s", g_rosetta_root, gp);
    if (stat(t, &sb)) return;
    fm = oah_guest_sym("CarbonCore", "_FSPathMakeRef");
    fr = oah_guest_sym("CarbonCore", "_RegisterComponentFileRef");
    if (!fm || !fr) return;
    a[0] = (unsigned int)gp; a[1] = (unsigned int)fsref; a[2] = 0;
    e1 = oah_callppc_n("FSPathMakeRef", fm, 3, a);
    a[0] = (unsigned int)fsref; a[1] = 0;
    e2 = e1 ? 0 : oah_callppc_n("RegisterComponentFileRef", fr, 2, a);
    fprintf(stderr, "[qt] Flash component registered (FSPathMakeRef %d, RegisterComponentFileRef %d)\n", (short)e1, (short)e2);
}
static int oah_want_qtflash(void) { return !getenv("OAH_NO_QTFLASH"); }
/* Scroll wheel for Carbon applications that never handled it (AppleWorks 6, ...): their windows
   use scroll bar controls with an action procedure, and a wheel or trackpad scroll did nothing
   (as on Mac OS X, where such apps needed a mouse driver's help). When a kEventMouseWheelMoved
   comes back from SendEventToEventTarget unhandled, find the scroll bar along the right (or
   bottom) edge of the window under the pointer and run its action procedure for the up/down
   arrow, once per line. Apps that handle the wheel themselves are not touched (they return
   noErr). OAH_NO_WHEELBAR=1 disables. */
static unsigned int g_wh_fn[16];
static int oah_wh_syms(void) {
    static int ok = -1;
    static const char *const n[] = { "_GetEventClass", "_GetEventKind", "_GetEventParameter", "_SetEventParameter", "_FindWindow",
        "_GetWindowBounds", "_FindControlUnderMouse", "_GetControlKind", "_GetControlAction", "_InvokeControlActionUPP",
        "_GetControlBounds", "_IsControlActive", "_IsControlVisible", "_TestControl", 0 };
    int i;
    if (ok >= 0) return ok;
    for (i = 0; n[i]; i++) if (!(g_wh_fn[i] = oah_guest_sym("HIToolbox", n[i]))) { ok = 0; return ok; }
    ok = 1;
    return ok;
}
static int oah_want_wheelbar(void) { return !getenv("OAH_NO_WHEELBAR"); }
static void oah_inj_wheelclicks(float x, float y, int n);
static void oah_fixret_wheelbar(unsigned int *R, unsigned int ev) {
    static unsigned char buf[64];
    unsigned int a[7], cls, kind, win = 0, ctl = 0, upp, part, k, n;
    int delta, axis = 1, gx, gy, cl, ct, cr, cb, lx, ly, cw, chh;
    short wp;
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("OAH_WHEELDEBUG") ? 1 : 0;
    if ((!dbg && (int)R[3] != -9874) || !ev || !oah_wh_syms()) return;   /* eventNotHandledErr only */
    a[0] = ev;
    cls = oah_callppc_n("GetEventClass", g_wh_fn[0], 1, a);
    if (cls != 0x6d6f7573u) return;                                    /* 'mous' */
    a[0] = ev;
    kind = oah_callppc_n("GetEventKind", g_wh_fn[1], 1, a);
    if (dbg && kind == 10) fprintf(stderr, "[ev] wheel event %08x sent: result %d\n", ev, (int)R[3]);
    if (kind != 10 || (int)R[3] != -9874) return;                       /* kEventMouseWheelMoved */
    /* once per event: the dispatcher's outer send also comes back unhandled */
    a[0] = ev; a[1] = 0x72735748; a[2] = 0x626f6f6c; a[3] = 0; a[4] = 1; a[5] = 0; a[6] = (unsigned int)buf;   /* 'rsWH' 'bool' */
    if (oah_callppc_n("GetEventParameter", g_wh_fn[2], 7, a) == 0) return;
    buf[0] = 1;
    a[0] = ev; a[1] = 0x72735748; a[2] = 0x626f6f6c; a[3] = 1; a[4] = (unsigned int)buf;
    oah_callppc_n("SetEventParameter", g_wh_fn[3], 5, a);
    a[0] = ev; a[1] = 0x6d77646c; a[2] = 0x6c6f6e67; a[3] = 0; a[4] = 4; a[5] = 0; a[6] = (unsigned int)(buf + 8);   /* 'mwdl' 'long' */
    if (oah_callppc_n("GetEventParameter", g_wh_fn[2], 7, a)) { if (dbg) fprintf(stderr, "[ev] wheel: no delta\n"); return; }
    delta = (int)OSSwapBigToHostInt32(*(unsigned int *)(buf + 8));
    a[0] = ev; a[1] = 0x6d776178; a[2] = 0x6d776178; a[3] = 0; a[4] = 2; a[5] = 0; a[6] = (unsigned int)(buf + 12);  /* 'mwax' */
    if (!oah_callppc_n("GetEventParameter", g_wh_fn[2], 7, a)) axis = OSSwapBigToHostInt16(*(unsigned short *)(buf + 12));
    a[0] = ev; a[1] = 0x6d6c6f63; a[2] = 0x51447074; a[3] = 0; a[4] = 4; a[5] = 0; a[6] = (unsigned int)(buf + 16); /* 'mloc' 'QDpt' */
    if (oah_callppc_n("GetEventParameter", g_wh_fn[2], 7, a) || !delta) { if (dbg) fprintf(stderr, "[ev] wheel: no location (delta %d)\n", delta); return; }
    gy = (short)OSSwapBigToHostInt16(*(unsigned short *)(buf + 16)); gx = (short)OSSwapBigToHostInt16(*(unsigned short *)(buf + 18));
    a[0] = ((unsigned int)(gy & 0xffff) << 16) | (unsigned int)(gx & 0xffff); a[1] = (unsigned int)(buf + 20);
    wp = (short)oah_callppc_n("FindWindow", g_wh_fn[4], 2, a);
    win = OSSwapBigToHostInt32(*(unsigned int *)(buf + 20));
    if (dbg) fprintf(stderr, "[ev] wheel %d axis %d at %d,%d: window %08x part %d\n", delta, axis, gx, gy, win, wp);
    if (wp != 3 || !win) return;                                        /* inContent */
    a[0] = win; a[1] = 33; a[2] = (unsigned int)(buf + 24);             /* kWindowContentRgn */
    if (oah_callppc_n("GetWindowBounds", g_wh_fn[5], 3, a)) return;
    ct = (short)OSSwapBigToHostInt16(*(unsigned short *)(buf + 24)); cl = (short)OSSwapBigToHostInt16(*(unsigned short *)(buf + 26));
    cb = (short)OSSwapBigToHostInt16(*(unsigned short *)(buf + 28)); cr = (short)OSSwapBigToHostInt16(*(unsigned short *)(buf + 30));
    cw = cr - cl; chh = cb - ct;
    lx = gx - cl; ly = gy - ct;
    /* probe for a scroll bar under the pointer, then along the far edge */
    for (k = 0; k < 3 && !ctl; k++) {
        int px = lx, py = ly;
        if (k == 1) { if (axis == 1) px = cw - 8; else py = chh - 8; }
        if (k == 2) { if (axis == 1) { px = cw - 8; py = chh / 2; } else { py = chh - 8; px = cw / 2; } }
        a[0] = ((unsigned int)(py & 0xffff) << 16) | (unsigned int)(px & 0xffff); a[1] = win; a[2] = (unsigned int)(buf + 32);
        ctl = oah_callppc_n("FindControlUnderMouse", g_wh_fn[6], 3, a);
        if (dbg) fprintf(stderr, "[ev] wheel: probe %d,%d -> control %08x\n", px, py, ctl);
        if (ctl) {
            a[0] = ctl; a[1] = (unsigned int)(buf + 36);
            if (oah_callppc_n("GetControlKind", g_wh_fn[7], 2, a) ||
                OSSwapBigToHostInt32(*(unsigned int *)(buf + 36)) != 0x6170706cu || OSSwapBigToHostInt32(*(unsigned int *)(buf + 40)) != 0x73626172u)
                ctl = 0;                                                /* not 'appl' 'sbar' */
            else {
                int bw, bh;
                a[0] = ctl; a[1] = (unsigned int)(buf + 44);
                oah_callppc_n("GetControlBounds", g_wh_fn[10], 2, a);
                bh = (short)OSSwapBigToHostInt16(*(unsigned short *)(buf + 48)) - (short)OSSwapBigToHostInt16(*(unsigned short *)(buf + 44));
                bw = (short)OSSwapBigToHostInt16(*(unsigned short *)(buf + 50)) - (short)OSSwapBigToHostInt16(*(unsigned short *)(buf + 46));
                if ((axis == 1) != (bh > bw)) ctl = 0;                  /* wrong orientation */
            }
        }
    }
    if (dbg) fprintf(stderr, "[ev] wheel: content %d,%d %dx%d, scroll bar %08x\n", cl, ct, cw, chh, ctl);
    if (!ctl) return;
    a[0] = ctl;
    if (!(oah_callppc_n("IsControlVisible", g_wh_fn[12], 1, a) & 0xff) || !(oah_callppc_n("IsControlActive", g_wh_fn[11], 1, a) & 0xff)) {
        if (dbg) fprintf(stderr, "[ev] wheel: scroll bar hidden or inactive\n");
        return;
    }
    a[0] = ctl;
    upp = oah_callppc_n("GetControlAction", g_wh_fn[8], 1, a);
    part = delta > 0 ? 20 : 21;                                         /* kControlUpButtonPart / DownButtonPart */
    n = (unsigned int)(delta < 0 ? -delta : delta);
    if (n > 12) n = 12;
    if (upp && upp != 0xffffffffu) {
        for (k = 0; k < n; k++) { a[0] = ctl; a[1] = part; a[2] = upp; oah_callppc_n("InvokeControlActionUPP", g_wh_fn[9], 3, a); }
        if (dbg || getenv("OAH_EVTRACE")) fprintf(stderr, "[ev] wheel %d (axis %d) unhandled: scroll bar %08x action x%u\n", delta, axis, ctl, n);
    } else {
        /* the app passes its action procedure to TrackControl itself (AppleWorks): click the arrow */
        int t, l, b, r, i, found = 0, px = 0, py = 0;
        a[0] = ctl; a[1] = (unsigned int)(buf + 44);
        oah_callppc_n("GetControlBounds", g_wh_fn[10], 2, a);
        t = (short)OSSwapBigToHostInt16(*(unsigned short *)(buf + 44)); l = (short)OSSwapBigToHostInt16(*(unsigned short *)(buf + 46));
        b = (short)OSSwapBigToHostInt16(*(unsigned short *)(buf + 48)); r = (short)OSSwapBigToHostInt16(*(unsigned short *)(buf + 50));
        for (i = 0; i < 4096 && !found; i += 2) {
            if (axis == 1) { px = (l + r) / 2; py = part == 21 ? b - 1 - i : t + 1 + i; if (py < t || py >= b) break; }
            else { py = (t + b) / 2; px = part == 21 ? r - 1 - i : l + 1 + i; if (px < l || px >= r) break; }
            a[0] = ctl; a[1] = ((unsigned int)(py & 0xffff) << 16) | (unsigned int)(px & 0xffff);
            if ((short)oah_callppc_n("TestControl", g_wh_fn[13], 2, a) == (short)part) found = 1;
        }
        if (found) {   /* the middle of the arrow: its far end can touch the window's size box */
            int ex = px, ey = py, j;
            for (j = 2; j < 64; j += 2) {
                int qx = axis == 1 ? px : (part == 21 ? px - j : px + j), qy = axis == 1 ? (part == 21 ? py - j : py + j) : py;
                a[0] = ctl; a[1] = ((unsigned int)(qy & 0xffff) << 16) | (unsigned int)(qx & 0xffff);
                if ((short)oah_callppc_n("TestControl", g_wh_fn[13], 2, a) != (short)part) break;
                ex = qx; ey = qy;
            }
            px = (px + ex) / 2; py = (py + ey) / 2;
            /* and it must be a content click there (not the size box or another window) */
            a[0] = ((unsigned int)((py + ct) & 0xffff) << 16) | (unsigned int)((px + cl) & 0xffff); a[1] = (unsigned int)(buf + 52);
            if ((short)oah_callppc_n("FindWindow", g_wh_fn[4], 2, a) != 3 || OSSwapBigToHostInt32(*(unsigned int *)(buf + 52)) != win) found = 0;
        }
        if (dbg) fprintf(stderr, "[ev] wheel: no action procedure; arrow %d %s at %d,%d\n", part, found ? "found" : "not found", px, py);
        if (!found) return;
        oah_inj_wheelclicks((float)(px + cl), (float)(py + ct), part == 21 ? (int)n : -(int)n);
        if (dbg || getenv("OAH_EVTRACE")) fprintf(stderr, "[ev] wheel %d (axis %d) unhandled: %u clicks on the scroll bar arrow\n", delta, axis, n);
    }
}
/* A collection started from guest code that liboah called back (a dock-menu choice, ...) scans
   "the current thread" from r1 up to the thread's stack base, but r1 is on the translator's
   callback stack, and the scan ran off its end (ScreenFlow, Dock menu > New Recording: fault in
   Thread::scan_current_thread). Defer it: the next collection from the run loop does the work. */
static int oah_on_callback_stack(void);
static void oah_fix_gccollect(unsigned int *R) {
    static int n;
    if (!oah_on_callback_stack()) return;
    if (n++ < 3) fprintf(stderr, "[gc] auto_collect called on a callback stack (r1 %08x): deferred\n", R[1]);
    g_gt_skip = 1; g_gt_skip_ret = 0;
}
/* Objective-C garbage collection (libauto: ScreenFlow, ...) suspends each other thread and reads
   its registers with thread_get_state(PPC_THREAD_STATE) to scan them for pointers. The kernel has
   no PowerPC state for a translated thread (err 5, "malloc: get_register_state(): unable to get
   thread state"), and the collector stopped the app with a trap. Answer from the thread's guest
   register file, found from its native registers and stack as the hang dumper finds it (translated
   code keeps it in %ebp). srr0 is reported as the LR. OAH_NO_PPCSTATE=1 disables. */
int oah_hang_isR_ext(unsigned int X);
/* r1 of a candidate register file, read safely: a file noted for a thread that has since ended
   can be unmapped, and a fault in this native code became a guest SIGTRAP (Evernote: "Uncaught
   system exception: signal 5" in objc's cache collector, no window) */
static unsigned int oah_tgs_r1(unsigned int Rc) { unsigned int v; return (Rc > 0x1000 && probe_read(Rc + 4, &v)) ? v : 0; }
static void oah_fix_thrgetstate(unsigned int *R) {
    unsigned int thr = R[3], st = R[5], cntp = R[6], cnt, Rt = 0, k, v, fp, cand[160], nc = 0, w[40];
    x86_thread_state32_t ts; mach_msg_type_number_t c = x86_THREAD_STATE32_COUNT;
    static int nlog;
    int src = -1;
    memset(&ts, 0, sizeof ts);
    if (getenv("OAH_GCLOG") && atoi(getenv("OAH_GCLOG")) > 1) fprintf(stderr, "[gc] enter thread_get_state(%x, %u) on %x\n", thr, R[4], pthread_mach_thread_np(pthread_self()));
    if (R[4] != 1 && getenv("OAH_GCLOG")) fprintf(stderr, "[gc] thread_get_state(%x, flavor %u) passed on (lr %08x)\n", thr, R[4], R[0x90 / 4]);
    if (R[4] != 1 /* PPC_THREAD_STATE */ || !st || !cntp || !probe_read(cntp, &cnt)) return;
    cnt = OSSwapBigToHostInt32(cnt);
    if (cnt < 40 || !oah_readable(st, 160)) return;
    if (thr == pthread_mach_thread_np(pthread_self())) Rt = (unsigned int)(uintptr_t)R;
    else {
        /* First the register files noted at this thread's bridge calls: of the valid ones, the
           deepest (lowest r1) on the stack the thread is on now (within 8 MB of the highest r1), so
           that the collector scans from there; a callback register file on another stack would make
           it scan the memory in between. Otherwise the first valid word from the thread's native
           registers and stack, in the hang dumper's order (other threads' register files can be
           on that stack too, so no "deepest" choice there). */
        unsigned int best = 0, maxsp = 0, r1, nn = (unsigned int)oah_thr_R_list(thr, cand), got;
        got = thread_get_state(thr, x86_THREAD_STATE32, (thread_state_t)&ts, &c) == KERN_SUCCESS;
        /* running translated code: %ebp is the register file in use (its r1 may be mid-prologue,
           so only a readable r1 is asked for) */
        for (k = 0; got && k < nn && !best; k++)
            if (ts.__ebp == cand[k] && probe_read(oah_tgs_r1(cand[k]), &v)) { best = cand[k]; src = -2; }
        for (k = 0; !best && k < nn; k++) if (oah_hang_isR_ext(cand[k])) { r1 = oah_tgs_r1(cand[k]); if (r1 > maxsp) maxsp = r1; }
        for (k = 0; maxsp && k < nn; k++) if (oah_hang_isR_ext(cand[k])) {
            r1 = oah_tgs_r1(cand[k]);
            if (r1 + 0x800000 >= maxsp && (!best || r1 < oah_tgs_r1(best))) best = cand[k];
        }
        /* none passes the back-chain check (a frame being pushed or popped): the noted ones are
           this thread's own, so take the deepest readable r1 by the same rule */
        for (k = 0; !best && k < nn; k++) if (probe_read(oah_tgs_r1(cand[k]), &v)) { r1 = oah_tgs_r1(cand[k]); if (r1 > maxsp) maxsp = r1; }
        for (k = 0; !best && maxsp && k < nn; k++) if (probe_read(oah_tgs_r1(cand[k]), &v)) {
            r1 = oah_tgs_r1(cand[k]);
            if (r1 + 0x800000 >= maxsp && r1 <= maxsp) { unsigned int j, low = r1;
                for (j = 0; j < nn; j++) { unsigned int r1j = oah_tgs_r1(cand[j]);
                    if (probe_read(r1j, &v) && r1j + 0x800000 >= maxsp && r1j < low) low = r1j; }
                if (low == r1) { best = cand[k]; src = -3; } }
        }
        nc = 0;
        if (!best && got) {
            cand[nc++] = ts.__ebp; cand[nc++] = ts.__ebx; cand[nc++] = ts.__esi; cand[nc++] = ts.__edi;
            for (fp = ts.__ebp, k = 0; k < 16 && nc < 32; k++) {
                unsigned int nfp;
                if (!probe_read(fp, &nfp) || nfp <= fp) break;
                cand[nc++] = nfp; fp = nfp;
            }
            /* the stack in one read (a word at a time was a system call each: 16384 of them, ~7 ms
               per thread_get_state - Numbers spent 9 s of a new sheet's 15 here, in the Objective-C
               runtime's cache collector). When the whole 64 KB is not readable, page by page up to the
               first unreadable page, so the scan covers what the word-by-word loop covered: halving the
               length cuts it short, the guest state is not found and PowerPC Java's
               garbage collector crashed (threads parked in the JVM). */
            /* PowerPC Java keeps the word-by-word scan: with the fast read its
               garbage collector crashed in most runs (a 4-thread allocation test: 1 of 5 passed, 5 of 5
               with this scan); why the slower scan matters is not understood (a timing effect: the
               candidates are the same words). The JVM asks thread_get_state only at collections.
               OAH_TGS_SLOWSCAN=1 uses it everywhere, OAH_TGS_SLOWSCAN=0 nowhere. */
            static int slow = -1;
            if (slow < 0) { const char *e = getenv("OAH_TGS_SLOWSCAN"); slow = e ? atoi(e) != 0 : oah_java_exe(); }
            if (slow) for (k = 0; k < 16384 && nc < 160; k++) {
                if (!probe_read(ts.__esp + 4 * k, &v)) break;
                if (v > 0x10000 && !(v & 3)) cand[nc++] = v;
            } else
            {   unsigned int *sb = (unsigned int *)malloc(16384 * 4);
                vm_size_t len = 16384 * 4, got2 = 0;
                if (sb && (vm_read_overwrite(mach_task_self(), ts.__esp, len, (vm_address_t)sb, &got2) != KERN_SUCCESS || got2 != len)) {
                    vm_address_t a = ts.__esp, e = ts.__esp + 16384 * 4;
                    len = 0;
                    while (a < e) {
                        vm_size_t n = ((a + 0x1000) & ~(vm_address_t)0xfff) - a;
                        if (n > e - a) n = e - a;
                        if (vm_read_overwrite(mach_task_self(), a, n, (vm_address_t)((char *)sb + len), &got2) != KERN_SUCCESS || got2 != n) break;
                        len += n; a += n;
                    }
                }
                for (k = 0; sb && k < len / 4 && nc < 160; k++) {
                    v = sb[k];
                    if (v > 0x10000 && !(v & 3)) cand[nc++] = v;
                }
                free(sb);
            }
            for (k = 0; k < nc && !best; k++) if (oah_hang_isR_ext(cand[k])) { best = cand[k]; src = k; }
        }
        Rt = best;
    }
    if (Rt && !oah_readable(Rt, 0x98)) Rt = 0;
    if (!Rt) {
        static int nfail;
        if (nfail++ < 5) {
            unsigned int nl[OAH_THR_RPER], nn = (unsigned int)oah_thr_R_list(thr, nl);
            fprintf(stderr, "[gc] thread_get_state(%x, PPC): no guest state found (%u noted, first r1 %08x; eip %08x ebp %08x)\n",
                    thr, nn, nn ? ((unsigned int *)(uintptr_t)nl[0])[1] : 0, ts.__eip, ts.__ebp);
        }
        return;
    }
    {
        unsigned int *G = (unsigned int *)(uintptr_t)Rt, *out = (unsigned int *)(uintptr_t)st;
        memset(w, 0, sizeof w);
        w[0] = G[0x90 / 4];                                      /* srr0 */
        for (k = 0; k < 32; k++) w[2 + k] = G[k];                /* r0..r31 */
        w[36] = G[0x90 / 4]; w[37] = G[0x94 / 4];                /* lr, ctr */
        for (k = 0; k < 40; k++) out[k] = OSSwapHostToBigInt32(w[k]);
        *(unsigned int *)(uintptr_t)cntp = OSSwapHostToBigInt32(40);
    }
    if (nlog++ < (getenv("OAH_GCLOG") ? 200 : 5))
        fprintf(stderr, "[gc] thread_get_state(%x, PPC) answered from the guest registers %08x (r1 %08x, %s %d; eip %08x esp %08x ebp %08x)\n",
                thr, Rt, w[3], src == -2 ? "running" : src == -3 ? "noted, unchecked" : src < 0 ? "noted" : "stack word", src, ts.__eip, ts.__esp, ts.__ebp);
    g_gt_skip = 1; g_gt_skip_ret = 0;
}
/* sigaction(SIGTRAP, handler) from the guest (NSExceptionHandler: Evernote; crash catchers): once
   the translator knows a guest SIGTRAP handler, it hands the traps of compiled guest code - the
   "twi" of every bridge and gtrace fix - to that handler instead of raising them natively, so a
   bridged function that got hot became "Uncaught system exception: signal 5", and Evernote's
   handler then deadlocked logging it (spinning, no window). The handler is remembered and reported
   back, but not installed. OAH_GUEST_TRAPHANDLER=1 installs it. */
static unsigned int g_guest_trapact[3];
static void oah_fix_sigaction(unsigned int *R) {
    unsigned int h;
    if (getenv("OAH_DIAG_NOSIGH") && R[3] != 5 && R[4] > 0x1000 && oah_readable(R[4], 4) && OSSwapBigToHostInt32(*(unsigned int *)(uintptr_t)R[4]) > 1) {
        fprintf(stderr, "[sig] diag: guest handler for signal %u not installed\n", R[3]);
        g_gt_skip = 1; g_gt_skip_ret = 0; return;
    }
    if (R[3] != 5 /* SIGTRAP */) return;
    if (R[5] > 0x1000 && oah_readable(R[5], 12)) {
        unsigned int *o = (unsigned int *)(uintptr_t)R[5];
        o[0] = OSSwapHostToBigInt32(g_guest_trapact[0]); o[1] = OSSwapHostToBigInt32(g_guest_trapact[1]); o[2] = OSSwapHostToBigInt32(g_guest_trapact[2]);
    }
    if (R[4] > 0x1000 && oah_readable(R[4], 12)) {
        unsigned int *a = (unsigned int *)(uintptr_t)R[4];
        h = OSSwapBigToHostInt32(a[0]);
        if (h <= 1) { g_guest_trapact[0] = 0; return; }   /* SIG_DFL / SIG_IGN: as before (the syscall fix keeps ours) */
        g_guest_trapact[0] = h; g_guest_trapact[1] = OSSwapBigToHostInt32(a[1]); g_guest_trapact[2] = OSSwapBigToHostInt32(a[2]);
        { static int n; if (n++ < 2) fprintf(stderr, "[sig] guest SIGTRAP handler %08x recorded, not installed (bridges use traps)\n", h); }
    }
    g_gt_skip = 1; g_gt_skip_ret = 0;
}
static int oah_want_sigactfix(void) { return !getenv("OAH_GUEST_TRAPHANDLER"); }
static int oah_want_ppcstate(void) { return !getenv("OAH_NO_PPCSTATE"); }
/* IOServiceGetMatchingService(s): Snow Leopard's IOKit sends the matching dictionary as XML text
   (io_service_get_matching_services, MIG 2804), which Mojave's kernel answers with
   kIOReturnUnsupported. Serialize the guest dictionary with the guest's IOCFSerialize, rebuild it
   natively and ask native IOKit; the iterator / service port is in our task, which is the guest's.
   The call consumes one reference to the dictionary, as the original does. ScreenFlow needs this
   (its start-up reads the primary network interface's MAC address). OAH_NO_IOMATCH=1 disables. */
extern CFTypeRef IOCFUnserialize(const char *, CFAllocatorRef, CFOptionFlags, CFStringRef *);
extern kern_return_t IOServiceGetMatchingServices(mach_port_t, CFDictionaryRef, unsigned int *);
extern unsigned int IOServiceGetMatchingService(mach_port_t, CFDictionaryRef);
static void oah_iok_release_guest(unsigned int g) {
    static unsigned int rel;
    unsigned int a[1];
    if (!rel) rel = oah_guest_sym("CoreFoundation", "_CFRelease");
    if (g && rel) { a[0] = g; oah_callppc_n("CFRelease", rel, 1, a); }
}
static CFDictionaryRef oah_iok_native_dict(unsigned int gdict) {
    static unsigned int ser, len, ptr;
    unsigned int a[2], gdata, n, p;
    CFDictionaryRef nd = 0;
    if (!ser) { ser = oah_guest_sym("IOKit", "_IOCFSerialize"); len = oah_guest_sym("CoreFoundation", "_CFDataGetLength"); ptr = oah_guest_sym("CoreFoundation", "_CFDataGetBytePtr"); }
    if (!gdict || !ser || !len || !ptr) return 0;
    a[0] = gdict; a[1] = 0;
    gdata = oah_callppc_n("IOCFSerialize", ser, 2, a);
    if (!gdata) return 0;
    a[0] = gdata; n = oah_callppc_n("CFDataGetLength", len, 1, a);
    a[0] = gdata; p = oah_callppc_n("CFDataGetBytePtr", ptr, 1, a);
    if (p && n && n < 0x100000 && oah_readable(p, n)) {
        char *s = (char *)malloc(n + 1);
        if (s) {
            memcpy(s, (void *)(uintptr_t)p, n); s[n] = 0;
            nd = (CFDictionaryRef)IOCFUnserialize(s, kCFAllocatorDefault, 0, 0);
            free(s);
        }
    }
    oah_iok_release_guest(gdata);
    if (nd && CFGetTypeID(nd) != CFDictionaryGetTypeID()) { CFRelease(nd); nd = 0; }
    return nd;
}
static void oah_fix_iok_services(unsigned int *R) {      /* (master, matching, io_iterator_t *existing) */
    CFDictionaryRef nd = oah_iok_native_dict(R[4]);
    unsigned int it = 0;
    kern_return_t kr;
    static int nlog;
    if (!nd) return;                                     /* the original call (and its error) */
    kr = IOServiceGetMatchingServices(0 /* kIOMasterPortDefault */, nd, &it);   /* consumes nd */
    oah_iok_release_guest(R[4]);
    if (R[5] && oah_readable(R[5], 4)) *(unsigned int *)(uintptr_t)R[5] = OSSwapHostToBigInt32(kr ? 0 : it);
    if (nlog++ < 5) fprintf(stderr, "[iokit] IOServiceGetMatchingServices natively -> 0x%x, iterator 0x%x\n", kr, it);
    g_gt_skip = 1; g_gt_skip_ret = (unsigned int)kr;
}
static void oah_fix_iok_service(unsigned int *R) {       /* (master, matching) -> io_service_t */
    CFDictionaryRef nd = oah_iok_native_dict(R[4]);
    unsigned int s;
    static int nlog;
    if (!nd) return;
    s = IOServiceGetMatchingService(0, nd);           /* consumes nd */
    oah_iok_release_guest(R[4]);
    if (nlog++ < 5) fprintf(stderr, "[iokit] IOServiceGetMatchingService natively -> 0x%x\n", s);
    g_gt_skip = 1; g_gt_skip_ret = s;
}
/* IOServiceAddMatchingNotification(port, type, matching, callback, refcon, io_iterator_t *):
   Snow Leopard's IOKit sends the matching dictionary in the old (XML) form, which Mojave's kernel no
   longer takes, so the call failed and IOHIDManager saw no devices at all (a PowerPC USB/HID test
   listed 0 HID devices, the native build 3). Answer it with an iterator over the services matching
   now (for the matched / first-match types) or over nothing (terminated, published...): the
   initial arming iteration then sees the devices present. Devices plugged in later are not
   announced. OAH_NO_IOMATCH=1 disables (with the other IOKit matching fixes). */
static void oah_fix_iok_addnotif(unsigned int *R) {
    CFDictionaryRef nd;
    char type[128] = "";
    unsigned int it = 0;
    kern_return_t kr;
    static int nlog;
    if (R[4] && oah_readable(R[4], 1)) { strncpy(type, (const char *)(uintptr_t)R[4], sizeof type - 1); }
    if (!strcmp(type, "IOServiceFirstMatch") || !strcmp(type, "IOServiceMatched") || !strcmp(type, "IOServicePublish") ||
        !strcmp(type, "IOServiceFirstPublish")) {
        if (!(nd = oah_iok_native_dict(R[5]))) return;                    /* the original call */
    } else {
        CFMutableDictionaryRef m = CFDictionaryCreateMutable(0, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(m, CFSTR("IOProviderClass"), CFSTR("RosettusNoSuchClass"));
        nd = m;
    }
    kr = IOServiceGetMatchingServices(0, nd, &it);                       /* consumes nd */
    oah_iok_release_guest(R[5]);                                         /* the call consumes the matching dictionary */
    if (R[8] && oah_readable(R[8], 4)) *(unsigned int *)(uintptr_t)R[8] = OSSwapHostToBigInt32(kr ? 0 : it);
    if (nlog++ < 8) fprintf(stderr, "[iokit] IOServiceAddMatchingNotification(%s) answered natively -> 0x%x, iterator 0x%x\n", type, kr, it);
    g_gt_skip = 1; g_gt_skip_ret = (unsigned int)kr;
}
static int oah_want_iomatch(void) { return !getenv("OAH_NO_IOMATCH"); }
/* Photoshop's large brush cursor (oah_cgs.h, oah_alc_set): AdobeLargeCursor asks dyld for
   _CGSAdobeSetCursorHook (NSIsSymbolNameDefined, NSLookupAndBindSymbol, NSAddressOfSymbol) and, when
   it is there, hands every outline to it instead of running its own window server client. The
   lookups answer with a guest stub that traps into oah_fix_alc_hook. */
#define OAH_ALC_SYMVAL 0x41444245u
static int g_alc_idx = -1;
static unsigned int g_alc_stub;
static int oah_alc_name(unsigned int p) {
    static const char nm[] = "_CGSAdobeSetCursorHook";
    return p > 0x1000 && oah_readable(p, sizeof nm) && !memcmp((const void *)p, nm, sizeof nm);
}
static void oah_fix_alc_defined(unsigned int *R) { if (g_alc_idx >= 0 && oah_alc_name(R[3])) { g_gt_skip = 1; g_gt_skip_ret = 1; } }
static void oah_fix_alc_lookup(unsigned int *R) { if (g_alc_idx >= 0 && oah_alc_name(R[3])) { g_gt_skip = 1; g_gt_skip_ret = OAH_ALC_SYMVAL; } }
static void oah_fix_alc_address(unsigned int *R) {
    if (g_alc_idx < 0 || R[3] != OAH_ALC_SYMVAL) return;
    if (!g_alc_stub) {
        unsigned int *t = (unsigned int *)oah_guest_code_alloc(8);
        if (t) {
            t[0] = OSSwapHostToBigInt32(0x0fe00000 | (GT_ENTRY + (unsigned int)g_alc_idx));
            t[1] = OSSwapHostToBigInt32(0x4e800020);                 /* blr */
            g_alc_stub = (unsigned int)t;
        }
        CGSLOG("[cursor] _CGSAdobeSetCursorHook -> stub %08x\n", g_alc_stub);
    }
    g_gt_skip = 1; g_gt_skip_ret = g_alc_stub;
}
static void oah_fix_alc_hook(unsigned int *R) { g_gt_skip = 1; g_gt_skip_ret = (unsigned int)oah_alc_set(R[3], R[4]); }
static void oah_fixret_launchmods(unsigned int *R, unsigned int a0);   /* oah_aen.h */
static void oah_fix_lsopenurls(unsigned int *R);                         /* oah_ls.h */
static void oah_fix_cupsprintfiles(unsigned int *R);                     /* oah_prt.h */
static void oah_fixret_cupsprintfiles(unsigned int *R, unsigned int a0);
static int oah_want_pictwps(void);
static int oah_want_lsurlsfix(void);
static int oah_want_launchmods(void);
/* follow = 1: the export only checks its arguments and branches ("b") to a local worker; the hook
   goes on the target of the first unconditional branch in its first 12 words. */
/* FSpExchangeFiles / FSExchangeObjects: Snow Leopard's CarbonCore swaps two files with
   exchangedata(2), which APFS does not have, so both returned paramErr (-50). Office 2004 saves
   its OLE registry ("OLE Registration Database 11") by writing a REGEDH temporary file and
   exchanging it with the database: every save failed, the database kept only its first 12 KB,
   and VBA, which needs its type libraries registered there, said "Could not open macro storage"
   on every start after the first. Both items are resolved to paths with the guest's
   FSpMakeFSRef/FSRefMakePath and their contents exchanged in place (see oah_xchg_do); on
   failure the guest's own code runs as before. OAH_NO_XCHGFIX=1 disables. */
static int oah_want_xchgfix(void) { return !getenv("OAH_NO_XCHGFIX"); }
static int oah_xchg_ref_path(unsigned int ref, char *out, unsigned int max) {
    static unsigned int fmp;
    unsigned int a[3];
    if (!fmp) fmp = oah_guest_sym("CarbonCore", "_FSRefMakePath");
    if (!fmp || !ref) return -1;
    out[0] = 0; a[0] = ref; a[1] = (unsigned int)out; a[2] = max;
    return (oah_callppc_n("FSRefMakePath", fmp, 3, a) & 0xffff) || !out[0] ? -1 : 0;
}
static int oah_xchg_spec_path(unsigned int spec, char *out, unsigned int max) {
    static unsigned int fmr;
    unsigned int a[2], ref[20];
    if (!fmr) fmr = oah_guest_sym("CarbonCore", "_FSpMakeFSRef");
    if (!fmr || !spec) return -1;
    memset(ref, 0, sizeof ref);
    a[0] = spec; a[1] = (unsigned int)ref;
    if (oah_callppc_n("FSpMakeFSRef", fmr, 2, a) & 0xffff) return -1;
    return oah_xchg_ref_path((unsigned int)ref, out, max);
}
static void oah_xchg_do(const char *what, const char *p1, const char *p2) {
    /* exchangedata semantics: the data and resource forks change places, each file keeps its
       name and file ID. Word's safe save exchanges the document with its work file and then
       deletes the work file by FSRef (file ID), so a rename swap (renamex_np RENAME_SWAP) would
       delete the new document; the contents are copied in place instead (copyfile keeps the
       destination's inode). */
    char t[1100];
    int r;
    snprintf(t, sizeof t, "%s.rosettus-xchg", p2);
    unlink(t);
    r = copyfile(p1, t, NULL, COPYFILE_DATA | COPYFILE_XATTR);
    if (!r) { removexattr(p1, "com.apple.ResourceFork", XATTR_NOFOLLOW); r = copyfile(p2, p1, NULL, COPYFILE_DATA | COPYFILE_XATTR); }
    if (!r) { removexattr(p2, "com.apple.ResourceFork", XATTR_NOFOLLOW); r = copyfile(t, p2, NULL, COPYFILE_DATA | COPYFILE_XATTR); }
    unlink(t);
    fprintf(stderr, "[xchg] %s(\"%s\", \"%s\") -> contents exchanged %s\n", what, p1, p2, r ? strerror(errno) : "ok");
    if (!r) { g_gt_skip = 1; g_gt_skip_ret = 0; }
}
static void oah_fix_fspexchange(unsigned int *R) {   /* (const FSSpec *source, const FSSpec *dest) */
    char p1[1024], p2[1024];
    if (oah_xchg_spec_path(R[3], p1, sizeof p1) || oah_xchg_spec_path(R[4], p2, sizeof p2)) return;
    oah_xchg_do("FSpExchangeFiles", p1, p2);
}
static void oah_fix_fsexchange(unsigned int *R) {    /* (const FSRef *ref, const FSRef *destRef) */
    char p1[1024], p2[1024];
    if (oah_xchg_ref_path(R[3], p1, sizeof p1) || oah_xchg_ref_path(R[4], p2, sizeof p2)) return;
    oah_xchg_do("FSExchangeObjects", p1, p2);
}
/* Pages '09 Word import (SFCompatibility): -[ADRoot(Private) nextIdForPrefix:] counts sfa:IDs per
   class in an NSMapTable keyed by C strings and, once a prefix is known, inserts the caller's
   string as the key again. -[ADRoot fixIdsInSubtree:idMap:] passes a prefix made by cutting an
   old ID ("SFTVectorStyle-3") at its '-' and puts the '-' back afterwards. 10.5's NSMapInsert
   kept the key already in the table; Snow Leopard's Foundation (the PowerPC one used here)
   stores the new pointer, so the key becomes "SFTVectorStyle-3", the count restarts, IDs
   repeat (two "SFWPParagraphStyle-0") and the import fails: "attempt to insert nil key".
   When the prefix is already counted, hand the method its own copy (xmlStrdup); the table
   owns it from then on and frees it as it freed the copy it replaced. */
static int oah_want_adidfix(void) { return !getenv("OAH_NO_ADIDFIX"); }
static void oah_adid_copy(unsigned int *R, int r) {   /* self in r3 with the table at +0x10, the prefix in rN */
    static unsigned int fget, fdup;
    unsigned int map = 0, a[2], c;
    if (R[3] < 0x1000 || R[r] < 0x1000 || !probe_read(R[3] + 0x10, &map) || !(map = OSSwapBigToHostInt32(map))) return;
    if (!fget) fget = oah_guest_sym("Foundation", "_NSMapGet");
    if (!fdup) fdup = oah_guest_sym("libxml2.2.dylib", "_xmlStrdup");
    if (!fget || !fdup) return;
    a[0] = map; a[1] = R[r];
    if (!oah_callppc_n("NSMapGet", fget, 2, a)) return;   /* first of its kind: the method copies it itself */
    a[0] = R[r];
    if ((c = oah_callppc_n("xmlStrdup", fdup, 1, a))) R[r] = c;
}
static void oah_fix_adroot_nextid(unsigned int *R) { oah_adid_copy(R, 5); }   /* (ADRoot *self, SEL, const xmlChar *prefix) */
/* Keynote's PowerPoint import has the same code in -[SDRoot(Private) setIdFor:prefix:] (node, prefix) */
static void oah_fix_sdroot_setid(unsigned int *R) { oah_adid_copy(R, 6); }
/* thread_info / thread_policy_set / thread_policy_get from PowerPC code: Mojave's kernel answered
   KERN_INVALID_ARGUMENT (thread_info, even for the calling thread) and MIG_BAD_ARGUMENTS (-304,
   thread_policy_set) to the translated MIG messages. QuickTime's H.264 decoder sets up its worker
   threads with thread_info(THREAD_BASIC_INFO) / (THREAD_SCHED_TIMESHARE_INFO) and
   thread_policy_set(THREAD_PRECEDENCE_POLICY), freed a thread whose set-up failed, and crashed
   (NULL+4) tearing its pool down: no H.264 movie played. Answered with the native calls; the
   flavours handled are arrays of 32-bit integers, swapped word by word. Port names are the same
   for the guest. OAH_NO_THRINFO=1 disables. */
static int oah_want_thrinfo(void) { return !getenv("OAH_NO_THRINFO"); }
static int oah_thr_words(unsigned int flavor, int info) {
    if (info) return flavor == 3 /* BASIC */ || flavor == 10 /* SCHED_TIMESHARE */ || flavor == 11 /* SCHED_RR */ || flavor == 12 /* SCHED_FIFO */;
    return flavor >= 1 && flavor <= 5;   /* STANDARD, EXTENDED/TIME_CONSTRAINT, PRECEDENCE, AFFINITY, ... */
}
static void oah_thr_info_or_get(unsigned int *R, int info) {
    unsigned int thr = R[3], fl = R[4], buf = R[5], cntp = R[6], cnt, k, out[32];
    mach_msg_type_number_t c; kern_return_t kr; boolean_t gdef = 0;
    if (!oah_thr_words(fl, info) || !buf || !cntp || !oah_readable(cntp, 4)) return;
    cnt = OSSwapBigToHostInt32(*(unsigned int *)(uintptr_t)cntp);
    if (cnt > 32 || !oah_readable(buf, cnt * 4 ? cnt * 4 : 4)) return;
    c = cnt; memset(out, 0, sizeof out);
    if (info) kr = thread_info((thread_act_t)thr, fl, (thread_info_t)out, &c);
    else {
        if (R[7] && oah_readable(R[7], 4)) gdef = OSSwapBigToHostInt32(*(unsigned int *)(uintptr_t)R[7]) != 0;
        kr = thread_policy_get((thread_act_t)thr, fl, (thread_policy_t)out, &c, &gdef);
    }
    if (kr == KERN_SUCCESS) {
        if (c > cnt) c = cnt;
        for (k = 0; k < c; k++) ((unsigned int *)(uintptr_t)buf)[k] = OSSwapHostToBigInt32(out[k]);
        *(unsigned int *)(uintptr_t)cntp = OSSwapHostToBigInt32(c);
        if (!info && R[7] && oah_readable(R[7], 4)) *(unsigned int *)(uintptr_t)R[7] = OSSwapHostToBigInt32((unsigned int)gdef);
    }
    if (getenv("OAH_THRLOG")) fprintf(stderr, "[thr] thread_%s(%x, %u) -> %d\n", info ? "info" : "policy_get", thr, fl, kr);
    g_gt_skip = 1; g_gt_skip_ret = (unsigned int)kr;
}
static void oah_fix_thrinfo(unsigned int *R) { oah_thr_info_or_get(R, 1); }
static void oah_fix_thrpolget(unsigned int *R) { oah_thr_info_or_get(R, 0); }
static void oah_fix_thrpolset(unsigned int *R) {
    unsigned int thr = R[3], fl = R[4], buf = R[5], cnt = R[6], k, in[32]; kern_return_t kr;
    if (!oah_thr_words(fl, 0) || cnt > 32 || (cnt && (!buf || !oah_readable(buf, cnt * 4)))) return;
    for (k = 0; k < cnt; k++) in[k] = OSSwapBigToHostInt32(((unsigned int *)(uintptr_t)buf)[k]);
    kr = thread_policy_set((thread_act_t)thr, fl, (thread_policy_t)in, cnt);
    if (getenv("OAH_THRLOG")) fprintf(stderr, "[thr] thread_policy_set(%x, %u, %u words) -> %d\n", thr, fl, cnt, kr);
    g_gt_skip = 1; g_gt_skip_ret = (unsigned int)kr;
}
/* A Cocoa app that copies with a lazy pasteboard owner (Hex Fiend: declareTypes:owner: with
   NSStringPboardType and its private type, no data) leaves only promises on the guest pasteboard.
   The clipboard sync does not run a Cocoa app's promise keeper inside its sync (Hex Fiend's ran a
   modal "Copying" session there that never ended), so nothing reached Mojave's clipboard. When the
   declared types include plain text, ask the owner for the text right away, inside the app's own
   copy (main thread, as Cocoa would when a paste asks): the text becomes real data and is exported
   like any other. Other types stay promised. OAH_NO_EAGERTEXT=1 disables. */
static int oah_want_eagertext(void) { return !getenv("OAH_NO_EAGERTEXT"); }
static unsigned int g_et_pb, g_et_types, g_et_owner;   /* main thread only */
static unsigned int g_et_stub, g_et_lrslot; static int g_et_actdepth, g_et_at;
/* the next few clipboard looks read the guest clipboard in full: data an owner adds to a clipboard
   it already owns does not show as "modified" in PasteboardSynchronize (oah_clip_run) */
static volatile int g_clip_dirty;   /* the question deferred to the action's return */
static void oah_fix_declaretypes(unsigned int *R) { if (getenv("OAH_CLIPTRACE")) fprintf(stderr, "[clip] declareTypes on %s thread, guest-call lock depth %d (owner %s)\n", pthread_main_np() ? "the main" : "another", g_cp_count, g_cp_owner && pthread_equal(g_cp_owner, pthread_self()) ? "this thread" : "other"); if (pthread_main_np()) { g_et_pb = R[3]; g_et_types = R[5]; g_et_owner = R[6]; } }
static void oah_fixret_declaretypes(unsigned int *R, unsigned int a0) {
    static unsigned int fsel, fmsg, fcnt, fget, fgcs;
    unsigned int pb = g_et_pb, types = g_et_types, owner = g_et_owner, n, i, a[4], sel, str = 0;
    char buf[64];
    (void)R; (void)a0;
    g_et_pb = g_et_types = g_et_owner = 0;
    if (!pb || !types || !owner || !pthread_main_np()) return;
    if (!fsel) { fsel = oah_guest_sym("libobjc", "_sel_registerName"); fmsg = oah_guest_sym("libobjc", "_objc_msgSend");
                 fcnt = oah_guest_sym("CoreFoundation", "_CFArrayGetCount"); fget = oah_guest_sym("CoreFoundation", "_CFArrayGetValueAtIndex");
                 fgcs = oah_guest_sym("CoreFoundation", "_CFStringGetCString"); }
    if (!fsel || !fmsg || !fcnt || !fget || !fgcs) return;
    a[0] = types; n = oah_callppc_n("et_count", fcnt, 1, a);
    for (i = 0; i < n && i < 64 && !str; i++) {
        unsigned int t;
        a[0] = types; a[1] = i; t = oah_callppc_n("et_get", fget, 2, a);
        buf[0] = 0; a[0] = t; a[1] = (unsigned int)(uintptr_t)buf; a[2] = sizeof buf; a[3] = kCFStringEncodingUTF8;
        if (t && oah_callppc_n("et_cstr", fgcs, 4, a) && (!strcmp(buf, "NSStringPboardType") || !strcmp(buf, "public.utf8-plain-text"))) str = t;
    }
    if (!str) return;
    a[0] = (unsigned int)(uintptr_t)"respondsToSelector:"; sel = oah_callppc_n("et_sel", fsel, 1, a);
    {   /* The owner's answer runs as the app's own code on the way back from declareTypes (a stub
           the return goes through), not as a call from here: Hex Fiend's makes the text on a worker
           thread while a modal "Copying" panel waits, and that worker needs to call into the guest
           (bridges), which it could not while this thread held the guest-call lock. */
        static unsigned int *stub, *blk;
        unsigned int psel;
        a[0] = (unsigned int)(uintptr_t)"pasteboard:provideDataForType:"; psel = oah_callppc_n("et_sel", fsel, 1, a);
        a[0] = owner; a[1] = sel; a[2] = psel;
        if (!(oah_callppc_n("et_responds", fmsg, 3, a) & 0xff)) return;
        if (!blk) blk = (unsigned int *)calloc(8, 4);
        if (!stub && blk && (stub = (unsigned int *)oah_guest_code_alloc(19 * 4))) {
            unsigned int P = (unsigned int)(uintptr_t)blk, ha = (P + 0x8000) >> 16, lo = P & 0xffff, k;
            unsigned int c[19] = { 0x3D800000u | ha, 0x398C0000u | lo,      /* lis r12,ha(P) ; addi r12,r12,lo(P) */
                0x906C0000u,                                                  /* stw r3,0(r12): declareTypes' result */
                0x9421FFA0u,                                                  /* stwu r1,-96(r1) */
                0x806C0004u, 0x808C0008u, 0x80AC000Cu, 0x80CC0010u,           /* r3..r6: owner, sel, pasteboard, type */
                0x800C0014u, 0x7C0903A6u, 0x7C0C0378u, 0x4E800421u,           /* objc_msgSend (ctr, r12) */
                0x38210060u,                                                  /* addi r1,r1,96 */
                0x3D800000u | ha, 0x398C0000u | lo, 0x800C0018u, 0x7C0803A6u, /* the real return address */
                0x806C0000u, 0x4E800020u };                                   /* r3 back ; blr */
            for (k = 0; k < 19; k++) stub[k] = OSSwapHostToBigInt32(c[k]);
        }
        if (!stub || !blk) return;
        blk[1] = OSSwapHostToBigInt32(owner); blk[2] = OSSwapHostToBigInt32(psel); blk[3] = OSSwapHostToBigInt32(pb);
        blk[4] = OSSwapHostToBigInt32(str); blk[5] = OSSwapHostToBigInt32(fmsg);
        /* Inside a menu action (Copy) the owner is asked when the action returns, not on the way
           back from declareTypes: Pages and Keynote declare first and record what they copied after,
           so an answer given at once was empty and their text never reached Mojave. */
        if (g_et_actdepth > 0 && !getenv("OAH_EAGERTEXT_NOW")) { g_et_stub = (unsigned int)(uintptr_t)stub; g_et_lrslot = (unsigned int)(uintptr_t)&blk[6]; g_et_at = g_et_actdepth; }
        else { g_gt_redirect = (unsigned int)(uintptr_t)stub; g_gt_redirect_lrslot = (unsigned int)(uintptr_t)&blk[6]; }
    }
    if (getenv("OAH_CLIPTRACE")) fprintf(stderr, "[clip] lazy owner %08x will be asked for its text on the way back from %s\n", owner, g_et_stub ? "the action" : "declareTypes");
}
/* pthread_get_stackaddr_np / pthread_get_stacksize_np while the guest runs on a callback stack.
   Guest code we call (an Apple Event dispatched into the guest, a callback) runs on the
   translator's callback stack, not on the thread's own stack, but pthread still reports the
   thread's own range (the main thread: 8 MB below 0xc0000000). CarbonCore's StackSpace (sp minus
   the stack's low end) was then nonsense, and AppleScript stopped with "Stack overflow" (-2706):
   xACT's open handler, run from Finder's odoc, never processed a file. When r1 is outside the
   reported range, answer the bounds of the memory region r1 is in (at most 16 MB of it).
   OAH_NO_CBSTACK=1 disables. */
static int oah_want_cbstack(void) { return !getenv("OAH_NO_CBSTACK"); }
static int oah_cbstack_region(unsigned int sp, unsigned int *top, unsigned int *size) {
    mach_vm_address_t a = sp; mach_vm_size_t sz = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) return 0;
    if (a > sp || a + sz <= sp) return 0;
    *top = (unsigned int)(a + sz);
    *size = (unsigned int)(sz > (16u << 20) ? (16u << 20) : sz);
    return 1;
}
static unsigned int g_cbs_size_ret;   /* the size pthread reported (stacksize comes first in StackSpace) */
static void oah_fixret_stacksize(unsigned int *R, unsigned int a0) {
    unsigned int top, size;
    (void)a0;
    g_cbs_size_ret = R[3];
    /* only the main thread (its range is known: 0xc0000000 down; the stack size is asked first,
       before the top is known); other threads are left as they are */
    if (!pthread_main_np()) return;
    if (R[1] >= 0xc0000000u - R[3] && R[1] < 0xc0000000u) return;
    if (oah_cbstack_region(R[1], &top, &size)) {
        if (getenv("OAH_CBSTACKTRACE")) fprintf(stderr, "[cbstack] stacksize %08x -> %08x (r1 %08x, region top %08x)\n", R[3], size, R[1], top);
        R[3] = size;
    }
}
static void oah_fixret_stackaddr(unsigned int *R, unsigned int a0) {
    unsigned int top, size, rs = g_cbs_size_ret ? g_cbs_size_ret : (8u << 20);
    (void)a0;
    if (!pthread_main_np()) return;
    if (R[1] < R[3] && R[1] >= R[3] - rs) return;   /* r1 is in the reported stack */
    if (oah_cbstack_region(R[1], &top, &size)) {
        if (getenv("OAH_CBSTACKTRACE")) fprintf(stderr, "[cbstack] stackaddr %08x -> %08x (r1 %08x)\n", R[3], top, R[1]);
        R[3] = top;
    }
}
/* -[NSApplication sendAction:to:from:] (menus, key equivalents, buttons): depth on the main thread,
   and the deferred owner question above is asked as the outermost action returns */
static void oah_fix_sendaction(unsigned int *R) { (void)R; if (pthread_main_np()) g_et_actdepth++; }
static void oah_fixret_sendaction(unsigned int *R, unsigned int a0) {
    (void)R; (void)a0;
    if (!pthread_main_np()) return;
    if (g_et_actdepth > 0) g_et_actdepth--;
    /* the action that copied returns (an action inside a modal loop is answered at its own return) */
    if (g_et_stub && g_et_actdepth < g_et_at && !g_gt_redirect) {
        g_gt_redirect = g_et_stub; g_gt_redirect_lrslot = g_et_lrslot;
        g_et_stub = g_et_lrslot = 0;
        g_clip_dirty = 3;
        if (getenv("OAH_CLIPTRACE")) fprintf(stderr, "[clip] action returned: asking the lazy owner now\n");
    }
}
/* CloseComponent / CloseComponentInstance of an output AudioUnit we serve natively: the guest frees
   its callback data right after (AudioQueueDispose closes its AUHAL without AudioOutputUnitStop),
   and the guest audio thread went on calling the old input callback (crash in AudioToolbox). */
static void oah_au_closed(unsigned int gci);
static void oah_fix_close_component(unsigned int *R) { oah_au_closed(R[3]); }
static int oah_want_auclose(void) { return !getenv("OAH_NO_AUBRIDGE"); }
#include "oah_midi.h"   /* CoreMIDI served natively: its fix rows are at the end of this table */
#include "oah_kc.h"     /* the keychain, likewise */
#include "oah_sc.h"     /* the System Configuration store */
#include <dispatch/dispatch.h>
#include "/System/Library/Frameworks/CoreServices.framework/Frameworks/CarbonCore.framework/Headers/FSEvents.h"   /* only FSEvents: CoreServices.h clashes with our IOKit declarations */
#include "oah_fse.h"    /* FSEvents streams */
#include "oah_ps.h"     /* power sources */
#include "oah_hid.h"    /* HID devices */
#include "oah_hidcom.h" /* the old IOHIDLib plug-in interface */
static void oah_fix_cvl_wait(unsigned int *R) { unsigned int r3 = oah_cvl_wait(R); g_gt_skip = 1; g_gt_skip_ret = r3; }
/* The PasteboardRefs an app (or its Scrap Manager) keeps promises on (seen at a PasteboardPutItemFlavor without data),
   for PasteboardResolvePromises on the main thread (oah_clip_resolve_main). Retained (guest CFRetain)
   so a ref the app lets go of stays valid; at most 8, the oldest released. */
static unsigned int g_keeper_refs[8]; static int g_nkeeper;
static int oah_want_keeperrefs(void) { return !getenv("OAH_CLIP_KEEPER_SYNCTHREAD"); }
static void oah_fixret_setkeeper(unsigned int *R, unsigned int ref);
/* PasteboardPutItemFlavor(ref, item, flavor, data, flags) with no data is a promise: note the ref
   (PasteboardSetPromiseKeeper itself starts with a branch and cannot be hooked) */
static unsigned int g_put_promise_ref;
static void oah_fix_putflavor(unsigned int *R) { g_put_promise_ref = (pthread_main_np() && R[6] == 0) ? R[3] : 0; }
static void oah_fixret_putflavor(unsigned int *R, unsigned int ref) {
    unsigned int p = g_put_promise_ref;
    g_put_promise_ref = 0;
    if (p && p == ref && R[3] == 0) oah_fixret_setkeeper(R, ref);
}
static void oah_fixret_setkeeper(unsigned int *R, unsigned int ref) {
    static unsigned int fret, frel;
    unsigned int a[1]; int i;
    (void)R;
    if (!ref || !pthread_main_np()) return;
    for (i = 0; i < g_nkeeper; i++) if (g_keeper_refs[i] == ref) return;
    if (!fret) { fret = oah_guest_sym("CoreFoundation", "_CFRetain"); frel = oah_guest_sym("CoreFoundation", "_CFRelease"); }
    if (!fret || !frel) return;
    a[0] = ref; oah_callppc_n("keeper_retain", fret, 1, a);
    if (g_nkeeper == 8) { a[0] = g_keeper_refs[0]; oah_callppc_n("keeper_release", frel, 1, a); memmove(g_keeper_refs, g_keeper_refs + 1, 7 * 4); g_nkeeper--; }
    g_keeper_refs[g_nkeeper++] = ref;
    if (getenv("OAH_CLIPTRACE")) fprintf(stderr, "[clip] promises on pasteboard %08x (%d kept)\n", ref, g_nkeeper);
}
static int g_aen_evh_idx = -1;                   /* the RosettusAEHandler hook (oah_aen.h) */
static void oah_fix_aen_evh(unsigned int *R);
/* TransformProcessType from the guest: a menu bar app (LSUIElement) that puts itself in the Dock
   (kProcessTransformToForegroundApplication, 1) or takes itself out again (...ToUIElementApplication,
   4). The guest's Process Manager answered -600 (procNotFound); the host's transforms this process.
   Other processes' serial numbers are left to the guest. OAH_NO_TRANSFORMFIX=1 disables. */
/* HIToolbox caches the process kind on first use (GetProcessState: 1 foreground, 2 background only,
   3 UI element) and never looks again, so after the transform it still took the app for a
   background one: SetMenuBarObscured(false) returned at once and the menu bar was never registered
   or drawn. The cache byte is found from the function's own code (bcl 20,31 / mflr r31 ...
   addis r29,r31,hi / lbz r0,lo(r29)) and set to the new kind; back into a UI element, the kind
   seen at launch is put back. */
static void oah_fix_transform_state(unsigned int type) {
    static unsigned int fn, slot;
    static int orig = -1;
    if (!fn) {
        unsigned int i, w, hi = 0, base, rt = 0;
        fn = oah_guest_sym("HIToolbox", "__ZL15GetProcessStatev");
        if (!fn) fn = 1;
        for (i = 0; fn > 1 && i < 16; i++) {
            w = OSSwapBigToHostInt32(((unsigned int *)fn)[i]);
            if (w == 0x429f0005) rt = fn + 4 * i + 4;                       /* bcl 20,31,.+4: r31 = next */
            else if ((w & 0xfc1f0000) == 0x3c1f0000 && (w >> 21 & 31) == 29) hi = w << 16;  /* addis r29,r31,hi */
            else if ((w & 0xffff0000) == 0x881d0000 && rt && hi) {           /* lbz r0,lo(r29) */
                base = rt + hi;
                slot = base + (unsigned int)(int)(short)(w & 0xffff);
                break;
            }
        }
        fprintf(stderr, "[bridge/gen] HIToolbox process-kind cache %s (0x%08x)\n", slot ? "found" : "not found", slot);
    }
    if (!slot || getenv("OAH_NO_TRANSFORMSTATE")) return;
    if (orig < 0) orig = *(unsigned char *)slot;
    *(unsigned char *)slot = type == 1 ? 1 : (unsigned char)(orig ? orig : 3);
}
static void oah_fix_transform(unsigned int *R) {
    static int (*tpt)(void *, unsigned int), (*gcp)(void *);
    struct { unsigned int hi, lo; } cur = { 0, 2 }, me = { 0, 0 };
    unsigned int hi, lo;
    if (!R[3]) return;
    hi = OSSwapBigToHostInt32(*(unsigned int *)R[3]); lo = OSSwapBigToHostInt32(*(unsigned int *)(R[3] + 4));
    if (!tpt) { *(void **)&tpt = dlsym(RTLD_DEFAULT, "TransformProcessType"); *(void **)&gcp = dlsym(RTLD_DEFAULT, "GetCurrentProcess"); }
    if (!tpt) return;
    if (!(hi == 0 && lo == 2) && !(gcp && gcp(&me) == 0 && me.hi == hi && me.lo == lo)) return;
    g_gt_skip = 1;
    if (R[4] == 1) oah_mb_note_front();
    g_gt_skip_ret = (unsigned int)tpt(&cur, R[4]);
    fprintf(stderr, "[bridge/gen] guest TransformProcessType(%u) -> %d\n", R[4], (int)g_gt_skip_ret);
    /* Into the Dock: the app was active already (a UI element in front), so the activation that
       follows was dropped as "already in that state", and HIToolbox's activation handler
       (HIApplication::HandleActivated), which puts up the app's menu bar, never ran: the previous
       app's menus stayed in the bar. A forced activation runs it. OAH_NO_TRANSFORMMENUBAR=1: not done. */
    if (!g_gt_skip_ret && (R[4] == 1 || R[4] == 4)) oah_fix_transform_state(R[4]);
    if (!g_gt_skip_ret && R[4] == 1 && !getenv("OAH_NO_TRANSFORMMENUBAR")) {
        oah_cgs_force_activation();
        oah_cgs_mb_adopt_scan();
    }
}
static int oah_want_transformfix(void) { return !getenv("OAH_NO_TRANSFORMFIX"); }
static const struct { const char *lib, *sym; void (*fix)(unsigned int *); void (*fixret)(unsigned int *, unsigned int); int (*want)(void); int follow; } g_gt_fixes[] = {
    { "HIServices", "_TransformProcessType", oah_fix_transform, 0, oah_want_transformfix },
    { "libSystem.B.dylib", "_NSIsSymbolNameDefined", oah_fix_alc_defined, 0, oah_alc_want },
    { "libSystem.B.dylib", "_NSLookupAndBindSymbol", oah_fix_alc_lookup, 0, oah_alc_want },
    { "libSystem.B.dylib", "_NSAddressOfSymbol", oah_fix_alc_address, 0, oah_alc_want },
    { "libSystem.B.dylib", "_thread_set_exception_ports", oah_fix_thr_setexc, 0, oah_want_gexc },
    { "CarbonCore", "_GetScriptManagerVariable", 0, oah_fixret_smvar, oah_want_kchrfix },
    { "libSystem.B.dylib", "_thread_get_state", oah_fix_thrgetstate, 0, oah_want_ppcstate },
    { "AppKit", "-[NSPasteboard declareTypes:owner:]", oah_fix_declaretypes, oah_fixret_declaretypes, oah_want_eagertext },
    { "HIServices", "_PasteboardPutItemFlavor", oah_fix_putflavor, oah_fixret_putflavor, oah_want_keeperrefs },
    { "AppKit", "-[NSApplication sendAction:to:from:]", oah_fix_sendaction, oah_fixret_sendaction, oah_want_eagertext },
    { "libSystem.B.dylib", "_pthread_get_stacksize_np", 0, oah_fixret_stacksize, oah_want_cbstack },
    { "libSystem.B.dylib", "_pthread_get_stackaddr_np", 0, oah_fixret_stackaddr, oah_want_cbstack },
    { "libSystem.B.dylib", "_thread_info", oah_fix_thrinfo, 0, oah_want_thrinfo },
    { "libSystem.B.dylib", "_thread_policy_get", oah_fix_thrpolget, 0, oah_want_thrinfo },
    { "libSystem.B.dylib", "_thread_policy_set", oah_fix_thrpolset, 0, oah_want_thrinfo },
    { "IOKit", "_IOServiceGetMatchingServices", oah_fix_iok_services, 0, oah_want_iomatch },
    { "CarbonCore", "_FSpExchangeFiles", oah_fix_fspexchange, 0, oah_want_xchgfix },
    { "CarbonCore", "_FSExchangeObjects", oah_fix_fsexchange, 0, oah_want_xchgfix },
    { "SFCompatibility", "-[ADRoot(Private) nextIdForPrefix:]", oah_fix_adroot_nextid, 0, oah_want_adidfix },
    { "CarbonCore", "_CloseComponent", oah_fix_close_component, 0, oah_want_auclose },
    { "SFCompatibility", "-[SDRoot(Private) setIdFor:prefix:]", oah_fix_sdroot_setid, 0, oah_want_adidfix },
    { "IOKit", "_IOServiceGetMatchingService", oah_fix_iok_service, 0, oah_want_iomatch },
    { "IOKit", "_IOServiceAddMatchingNotification", oah_fix_iok_addnotif, 0, oah_want_iomatch },
    { "QuickTime", "_EnterMovies", oah_fix_entermovies, 0, oah_want_qtflash },
    { "QuickTime", "_OpenMovieFile", oah_fix_swf_open, oah_fixret_swf_open, oah_want_swf },
    { "QuickTime", "_NewMovieFromFile", oah_fix_swf_newmovie, 0, oah_want_swf },
    { "SpeechSynthesis", "_CountVoices", oah_fix_sp_count, 0, oah_want_speech },
    { "SpeechSynthesis", "_GetIndVoice", oah_fix_sp_indvoice, 0, oah_want_speech },
    { "SpeechSynthesis", "_GetVoiceDescription", oah_fix_sp_vdesc, 0, oah_want_speech },
    { "SpeechSynthesis", "_GetVoiceInfo", oah_fix_sp_vinfo, 0, oah_want_speech },
    { "SpeechSynthesis", "_NewSpeechChannel", oah_fix_sp_new, 0, oah_want_speech },
    { "SpeechSynthesis", "_DisposeSpeechChannel", oah_fix_sp_dispose, 0, oah_want_speech },
    { "SpeechSynthesis", "_SpeakString", oah_fix_sp_string, 0, oah_want_speech },
    { "SpeechSynthesis", "_SpeakText", oah_fix_sp_text, 0, oah_want_speech },
    { "SpeechSynthesis", "_SpeakBuffer", oah_fix_sp_buffer, 0, oah_want_speech },
    { "SpeechSynthesis", "_SpeakCFString", oah_fix_sp_cfstring, 0, oah_want_speech },
    { "SpeechSynthesis", "_StopSpeech", oah_fix_sp_stop, 0, oah_want_speech },
    { "SpeechSynthesis", "_StopSpeechAt", oah_fix_sp_stopat, 0, oah_want_speech },
    { "SpeechSynthesis", "_PauseSpeechAt", oah_fix_sp_pauseat, 0, oah_want_speech },
    { "SpeechSynthesis", "_ContinueSpeech", oah_fix_sp_continue, 0, oah_want_speech },
    { "SpeechSynthesis", "_SpeechBusy", oah_fix_sp_busy, 0, oah_want_speech },
    { "SpeechSynthesis", "_SpeechBusySystemWide", oah_fix_sp_busysys, 0, oah_want_speech },
    { "SpeechSynthesis", "_SetSpeechRate", oah_fix_sp_setrate, 0, oah_want_speech },
    { "SpeechSynthesis", "_SetSpeechPitch", oah_fix_sp_setpitch, 0, oah_want_speech },
    { "SpeechSynthesis", "_GetSpeechRate", oah_fix_sp_getrate, 0, oah_want_speech },
    { "SpeechSynthesis", "_GetSpeechPitch", oah_fix_sp_getpitch, 0, oah_want_speech },
    { "SpeechSynthesis", "_SetSpeechInfo", oah_fix_sp_setinfo, 0, oah_want_speech },
    { "SpeechSynthesis", "_GetSpeechInfo", oah_fix_sp_getinfo, 0, oah_want_speech },
    { "SpeechSynthesis", "_TextToPhonemes", oah_fix_sp_phonemes, 0, oah_want_speech },
    { "SpeechSynthesis", "_UseDictionary", oah_fix_sp_dict, 0, oah_want_speech },
    { "CarbonCore", "_YieldToThread", oah_fix_yield, oah_fixret_yield, oah_want_yieldfix },
    { "CoreFoundation", "_CFMessagePortSendRequest", oah_fix_msgport, oah_fixret_msgport, oah_want_msgportfix },
    { "CoreFoundation", "_CFPasteboardCreate", oah_fix_pbcall, oah_fixret_pbcall, oah_want_pbwatch },
    { "CoreFoundation", "_CFPasteboardSynchronize", oah_fix_pbcall, oah_fixret_pbcall, oah_want_pbwatch },
    { "CoreFoundation", "_CFRunLoopRunSpecific", oah_fix_msgport, oah_fixret_msgport, oah_want_rlrelease },
    { "libSystem.B.dylib", "_pthread_cond_wait", oah_fix_cvwait, oah_fixret_cvwait, oah_want_cvwfix },
    { "libSystem.B.dylib", "_pthread_cond_wait$UNIX2003", oah_fix_cvwait, oah_fixret_cvwait, oah_want_cvwfix },
    { "libSystem.B.dylib", "_pthread_cond_timedwait", oah_fix_cvwait, oah_fixret_cvwait, oah_want_cvwfix },
    { "libSystem.B.dylib", "_pthread_cond_timedwait$UNIX2003", oah_fix_cvwait, oah_fixret_cvwait, oah_want_cvwfix },
    { "libSystem.B.dylib", "_pthread_cond_timedwait_relative_np", oah_fix_cvwait, oah_fixret_cvwait, oah_want_cvwfix },
    { "libawt.jnilib", "_AskUserToAttachDebugger", oah_fix_awtdebugger, 0, oah_want_awtdebugfix },
    { "libauto.dylib", "__ZN4Auto6Thread11clear_stackEv", oah_fix_gcclear, 0, oah_want_gcclearfix },
    { "libauto.dylib", "_auto_collect", oah_fix_gccollect, 0, oah_want_gcclearfix },
    { "libSystem.B.dylib", "_task_set_exception_ports", oah_fix_task_setexc, 0, oah_want_gexc },
    { "libSystem.B.dylib", "_sigaction", oah_fix_sigaction, 0, oah_want_sigactfix },
    { "libSystem.B.dylib", "_thread_get_exception_ports", oah_fix_thr_getexc, 0, oah_want_gexc },
    { "libSystem.B.dylib", "_task_get_exception_ports", oah_fix_task_getexc, 0, oah_want_gexc },
    { "libSystem.B.dylib", "_thread_swap_exception_ports", oah_fix_thr_swapexc, 0, oah_want_gexc },
    { "libSystem.B.dylib", "_task_swap_exception_ports", oah_fix_task_swapexc, 0, oah_want_gexc },
    { "CarbonCore", "__FSGetVolumeDeviceTypeAndBackingStoreVolume", oah_fix_voldevtype, 0, oah_want_voldevtype },
    { "HIServices", "_GetFrontProcess", oah_fix_frontproc, oah_fixret_frontproc, oah_want_frontfix },
    { "HIServices", "__GetFrontUIProcess", oah_fix_frontproc, 0, oah_want_frontfix },
    { "HIToolbox", "_GetDblTime", oah_fix_dbltime, 0, oah_want_dbltimefix },
    { "CarbonCore", "__CSCreateSeed", oah_fix_cscreate, 0, oah_want_csseedfix },
    { "CarbonCore", "__CSReadSeed", oah_fix_csread, 0, oah_want_csseedfix },
    { "CarbonCore", "__CSIncrementSeed", oah_fix_csincrement, 0, oah_want_csseedfix },
    { "CarbonCore", "__CSWriteSeed", oah_fix_cswrite, 0, oah_want_csseedfix },
    { "CarbonCore", "__CSDestroySeed", oah_fix_csdestroy, 0, oah_want_csseedfix },
    { "IOKit", "_NXClickTime", oah_fix_nxclicktime, 0, oah_want_dbltimefix },
    { "AE", "_AESendMessage", oah_fix_aesendmessage, 0, oah_want_aesmfix },
    { "SystemConfiguration", "_SCDynamicStoreCopyConsoleUser", oah_fix_consoleuser, 0, oah_want_consoleuserfix },
    { "SystemConfiguration", "_SCDynamicStoreCopyConsoleInformation", oah_fix_consoleinfo, 0, oah_want_consoleuserfix },
    { "SFUtility", "-[SFUUndoManager(SFUndoManagerPrivate) prepareEventGrouping]", oah_fix_sfundo, 0, oah_want_sfundofix },
    { "LaunchServices", "__UTTypeGetStatus", 0, oah_fixret_utstatus, oah_want_utstatusfix },
    { "HIToolbox", "_SendEventToEventTarget", 0, oah_fixret_wheelbar, oah_want_wheelbar },
    { "LaunchServices", "__LSCopyLaunchModifiers", 0, oah_fixret_launchmods, oah_want_launchmods },
    { "HIServices", "_LaunchApplication", oah_fix_launchapp, 0, oah_want_launchappfix },
    { "LaunchServices", "_LSOpenURLsWithRole", oah_fix_lsopenurls, 0, oah_want_lsurlsfix, 1 },
    { "libcups", "_cupsPrintFiles2", oah_fix_cupsprintfiles, oah_fixret_cupsprintfiles, oah_want_pictwps },
    { "CarbonCore", "_PBHGetVInfoSync", oah_fix_volindex, oah_fixret_volindex, oah_want_volindexfix },
    { "CarbonCore", "__FSCopyVolumeProperty", oah_fix_volprop, oah_fixret_volprop, oah_want_voliconfix },
    { "CoreGraphics", "_CGContextCopyWindowContentsToRect", oah_fix_copywin, 0, oah_want_copywin },
    { "CoreGraphics", "_CGContextCopyDisplayCaptureContentsToRect", oah_fix_dispcap, 0, oah_want_dispcap },
    { "CoreGraphics", "_CGSGetDisplaysWithRect", oah_fix_dispswithrect, 0, oah_want_dispcap },
    { "CoreGraphics", "_CGSGetDisplaysWithPoint", oah_fix_dispswithpoint, 0, oah_want_dispcap },
    { "CoreGraphics", "_CGSGetDisplayPixelEncodingOfLength", oah_fix_dispenc, 0, oah_want_dispcap },
    { "HIToolbox", "-[IMKClient _launch:fromBundle:throughPort:]", oah_fix_imklaunch, 0, oah_want_imklaunchfix },
    { "HIToolbox", "_LaunchAppleCharacterPalette", oah_fix_charpal, 0, oah_want_charpalfix },
    { "CarbonCore", "_PBXGetVolInfoSync", oah_fix_volindex, oah_fixret_volindex, oah_want_volindexfix },
    { "OSServices", "__ZN17CIconStorageEntry6UnloadEhPPK9__CFArray", oah_fix_icon_unload, 0, 0 },
    { "CoreGraphics", "_CGContextResetCTM", 0, oah_fixret_resetctm, oah_want_hidpi },
    { "QuickLook", "__QLRaiseAssert", oah_fix_qlassert, 0, 0 },
    { "CoreUI", "_CUIDraw", oah_fix_cuidraw, 0, oah_want_hidpi },
    { "DiskArbitration", "_DASessionCreate", oah_fix_da_session, 0, oah_want_dafix },
    { "DiskArbitration", "_DAApprovalSessionCreate", oah_fix_da_session, 0, oah_want_dafix },
    { "DiskArbitration", "_DADiskCreateFromBSDName", oah_fix_da_disk, 0, oah_want_dafix },
    { "DiskArbitration", "_DADiskCopyDescription", oah_fix_da_desc, 0, oah_want_dafix },
    { "DiskArbitration", "_DADiskGetBSDName", oah_fix_da_bsdname, 0, oah_want_dafix },
    { "DiskArbitration", "_DADiskCopyWholeDisk", oah_fix_da_whole, 0, oah_want_dafix },
    { "DiskArbitration", "_DADiskCreateFromIOMedia", oah_fix_da_noop, 0, oah_want_dafix },
    /* the export starts with "b +4" (not relocatable): hook the local function it falls into */
    { "DiskArbitration", "__DADiskCreateFromVolumePath", oah_fix_da_volpath, 0, oah_want_dafix },
    { "DiskArbitration", "_DASessionScheduleWithRunLoop", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DASessionUnscheduleFromRunLoop", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DAApprovalSessionScheduleWithRunLoop", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DAApprovalSessionUnscheduleFromRunLoop", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DARegisterDiskAppearedCallback", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DARegisterDiskDisappearedCallback", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DARegisterDiskDescriptionChangedCallback", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DARegisterDiskMountApprovalCallback", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DARegisterDiskUnmountApprovalCallback", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DARegisterDiskEjectApprovalCallback", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DAUnregisterCallback", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DAUnregisterApprovalCallback", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DASessionSetDispatchQueue", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DARegisterIdleCallback", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DARegisterDiskPeekCallback", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DADiskCreateFromVolumePath", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DADiskClaim", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DADiskUnclaim", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DADiskSetOptions", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DADiskMount", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DADiskMountWithArguments", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DADiskUnmount", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DADiskEject", oah_fix_da_noop, 0, oah_want_dafix },
    { "DiskArbitration", "_DADiskRename", oah_fix_da_noop, 0, oah_want_dafix },
    { "CoreMIDI", "_MIDIClientCreate", oah_fix_midi_CLIENTCREATE, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIClientDispose", oah_fix_midi_CLIENTDISPOSE, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIInputPortCreate", oah_fix_midi_INPORT, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIOutputPortCreate", oah_fix_midi_OUTPORT, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIPortDispose", oah_fix_midi_PORTDISPOSE, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIPortConnectSource", oah_fix_midi_CONNECT, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIPortDisconnectSource", oah_fix_midi_DISCONNECT, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIGetNumberOfDevices", oah_fix_midi_NDEV, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIGetDevice", oah_fix_midi_DEV, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIDeviceGetNumberOfEntities", oah_fix_midi_DEV_NENT, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIDeviceGetEntity", oah_fix_midi_DEV_ENT, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIEntityGetNumberOfSources", oah_fix_midi_ENT_NSRC, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIEntityGetSource", oah_fix_midi_ENT_SRC, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIEntityGetNumberOfDestinations", oah_fix_midi_ENT_NDST, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIEntityGetDestination", oah_fix_midi_ENT_DST, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIEntityGetDevice", oah_fix_midi_ENT_DEV, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIGetNumberOfSources", oah_fix_midi_NSRC, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIGetSource", oah_fix_midi_SRC, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIGetNumberOfDestinations", oah_fix_midi_NDST, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIGetDestination", oah_fix_midi_DST, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIEndpointGetEntity", oah_fix_midi_EP_ENT, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIDestinationCreate", oah_fix_midi_DSTCREATE, 0, oah_want_midi },
    { "CoreMIDI", "_MIDISourceCreate", oah_fix_midi_SRCCREATE, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIEndpointDispose", oah_fix_midi_EPDISPOSE, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIGetNumberOfExternalDevices", oah_fix_midi_NEXT, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIGetExternalDevice", oah_fix_midi_EXT, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIObjectGetIntegerProperty", oah_fix_midi_GETINT, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIObjectSetIntegerProperty", oah_fix_midi_SETINT, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIObjectGetStringProperty", oah_fix_midi_GETSTR, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIObjectSetStringProperty", oah_fix_midi_SETSTR, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIObjectGetDataProperty", oah_fix_midi_GETDATA, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIObjectGetDictionaryProperty", oah_fix_midi_GETDICT, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIObjectGetProperties", oah_fix_midi_GETPROPS, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIObjectFindByUniqueID", oah_fix_midi_FINDUID, 0, oah_want_midi },
    { "CoreMIDI", "_MIDISend", oah_fix_midi_SEND, 0, oah_want_midi },
    { "CoreMIDI", "_MIDISendSysex", oah_fix_midi_SYSEX, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIReceived", oah_fix_midi_RECEIVED, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIFlushOutput", oah_fix_midi_FLUSH, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIRestart", oah_fix_midi_RESTART, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIEndpointGetRefCons", oah_fix_midi_GETREFCONS, 0, oah_want_midi },
    { "CoreMIDI", "_MIDIEndpointSetRefCons", oah_fix_midi_SETREFCONS, 0, oah_want_midi },
    { "Security", "_SecKeychainCopyDefault", oah_fix_kc_COPYDEFAULT, 0, oah_want_keychain },
    { "Security", "_SecKeychainOpen", oah_fix_kc_OPEN, 0, oah_want_keychain },
    { "Security", "_SecKeychainCreate", oah_fix_kc_CREATE, 0, oah_want_keychain },
    { "Security", "_SecKeychainDelete", oah_fix_kc_DELETE, 0, oah_want_keychain },
    { "Security", "_SecKeychainUnlock", oah_fix_kc_UNLOCK, 0, oah_want_keychain },
    { "Security", "_SecKeychainLock", oah_fix_kc_LOCK, 0, oah_want_keychain },
    { "Security", "_SecKeychainGetStatus", oah_fix_kc_STATUS, 0, oah_want_keychain },
    { "Security", "_SecKeychainGetPath", oah_fix_kc_PATH, 0, oah_want_keychain },
    { "Security", "_SecKeychainAddGenericPassword", oah_fix_kc_ADDGEN, 0, oah_want_keychain },
    { "Security", "_SecKeychainFindGenericPassword", oah_fix_kc_FINDGEN, 0, oah_want_keychain },
    { "Security", "_SecKeychainAddInternetPassword", oah_fix_kc_ADDINET, 0, oah_want_keychain },
    { "Security", "_SecKeychainFindInternetPassword", oah_fix_kc_FINDINET, 0, oah_want_keychain },
    { "Security", "_SecKeychainItemFreeContent", oah_fix_kc_FREECONTENT, 0, oah_want_keychain },
    { "Security", "_SecKeychainItemModifyAttributesAndData", oah_fix_kc_MODIFY, 0, oah_want_keychain },
    { "Security", "_SecKeychainItemDelete", oah_fix_kc_ITEMDELETE, 0, oah_want_keychain },
    { "Security", "_SecKeychainItemCopyContent", oah_fix_kc_COPYCONTENT, 0, oah_want_keychain },
    { "Security", "_SecKeychainItemCopyAttributesAndData", oah_fix_kc_COPYATTRDATA, 0, oah_want_keychain },
    { "Security", "_SecKeychainItemFreeAttributesAndData", oah_fix_kc_FREEATTRDATA, 0, oah_want_keychain },
    { "Security", "_SecKeychainSearchCreateFromAttributes", oah_fix_kc_SEARCHCREATE, 0, oah_want_keychain },
    { "Security", "_SecKeychainSearchCopyNext", oah_fix_kc_SEARCHNEXT, 0, oah_want_keychain },
    { "Security", "_SecKeychainSetUserInteractionAllowed", oah_fix_kc_SETUI, 0, oah_want_keychain },
    { "Security", "_SecKeychainGetUserInteractionAllowed", oah_fix_kc_GETUI, 0, oah_want_keychain },
    { "Security", "_SecKeychainItemCopyKeychain", oah_fix_kc_ITEMKEYCHAIN, 0, oah_want_keychain },
    { "Security", "_SecKeychainItemCreateFromContent", oah_fix_kc_ITEMCREATE, 0, oah_want_keychain },
    { "Security", "_SecKeychainCopySearchList", oah_fix_kc_SEARCHLIST, 0, oah_want_keychain },
    { "SystemConfiguration", "_SCDynamicStoreCreate", oah_fix_sc_CREATE, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreCreateWithOptions", oah_fix_sc_CREATEOPT, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreCopyValue", oah_fix_sc_VALUE, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreCopyMultiple", oah_fix_sc_MULTIPLE, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreCopyKeyList", oah_fix_sc_KEYLIST, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreCopyComputerName", oah_fix_sc_COMPUTER, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreCopyLocalHostName", oah_fix_sc_LOCALHOST, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreSetNotificationKeys", oah_fix_sc_NOTIFYKEYS, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreCreateRunLoopSource", oah_fix_sc_SOURCE, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreAddValue", oah_fix_sc_ADDVALUE, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreAddTemporaryValue", oah_fix_sc_ADDTEMP, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreSetValue", oah_fix_sc_SETVALUE, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreRemoveValue", oah_fix_sc_REMOVE, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreNotifyValue", oah_fix_sc_NOTIFYVALUE, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreTouchValue", oah_fix_sc_TOUCH, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreSetMultiple", oah_fix_sc_SETMULTI, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreCopyNotifiedKeys", oah_fix_sc_NOTIFIED, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreCopyProxies", oah_fix_sc_PROXIES, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreCopyLocation", oah_fix_sc_LOCATION, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreCopyDHCPInfo", oah_fix_sc_DHCP, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreSetDispatchQueue", oah_fix_sc_DISPATCH, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreAddWatchedKey", oah_fix_sc_WATCH, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreRemoveWatchedKey", oah_fix_sc_UNWATCH, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreLock", oah_fix_sc_LOCK, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreUnlock", oah_fix_sc_UNLOCK, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreNotifyCallback", oah_fix_sc_NOTIFYOTHER, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreNotifyCancel", oah_fix_sc_NOTIFYOTHER, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreNotifyFileDescriptor", oah_fix_sc_NOTIFYOTHER, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreNotifyMachPort", oah_fix_sc_NOTIFYOTHER, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreNotifySignal", oah_fix_sc_NOTIFYOTHER, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreNotifyWait", oah_fix_sc_NOTIFYOTHER, 0, oah_want_scstore },
    { "SystemConfiguration", "_SCDynamicStoreGetTypeID", oah_fix_sc_TYPEID, 0, oah_want_scstore },
    { "CarbonCore", "_FSEventStreamCreate", oah_fix_fs_CREATE, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventStreamSetDispatchQueue", oah_fix_fs_SETQUEUE, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventStreamScheduleWithRunLoop", oah_fix_fs_SCHEDULE, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventStreamUnscheduleFromRunLoop", oah_fix_fs_UNSCHEDULE, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventStreamStart", oah_fix_fs_START, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventStreamStop", oah_fix_fs_STOP, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventStreamInvalidate", oah_fix_fs_INVALIDATE, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventStreamRelease", oah_fix_fs_RELEASE, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventStreamRetain", oah_fix_fs_RETAIN, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventStreamGetLatestEventId", oah_fix_fs_LATEST, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventsGetCurrentEventId", oah_fix_fs_CURRENT, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventStreamCopyPathsBeingWatched", oah_fix_fs_PATHS, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventStreamFlushSync", oah_fix_fs_FLUSHSYNC, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventStreamFlushAsync", oah_fix_fs_FLUSHASYNC, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventStreamGetDeviceBeingWatched", oah_fix_fs_DEVICE, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventStreamCopyDescription", oah_fix_fs_DESCR, 0, oah_want_fsevents },
    { "CarbonCore", "_FSEventStreamShow", oah_fix_fs_SHOW, 0, oah_want_fsevents },
    { "IOKit", "_IOPSCopyPowerSourcesInfo", oah_fix_ps_INFO, 0, oah_want_ps },
    { "IOKit", "_IOPSCopyPowerSourcesList", oah_fix_ps_LIST, 0, oah_want_ps },
    { "IOKit", "_IOPSGetPowerSourceDescription", oah_fix_ps_DESCR, 0, oah_want_ps },
    { "IOKit", "_IOPSGetProvidingPowerSourceType", oah_fix_ps_PROVIDING, 0, oah_want_ps },
    { "IOKit", "_IOPSGetBatteryWarningLevel", oah_fix_ps_WARNING, 0, oah_want_ps },
    { "IOKit", "_IOPSCopyExternalPowerAdapterDetails", oah_fix_ps_ADAPTER, 0, oah_want_ps },
    { "IOKit", "_IOPSNotificationCreateRunLoopSource", oah_fix_ps_NOTIFY, 0, oah_want_ps },
    { "IOKit", "_IOPSCopyInternalBatteriesArray", oah_fix_ps_NULL, 0, oah_want_ps },
    { "IOKit", "_IOPSCopyUPSArray", oah_fix_ps_NULL, 0, oah_want_ps },
    { "IOKit", "_IOPSGetActiveBattery", oah_fix_ps_NULL, 0, oah_want_ps },
    { "IOKit", "_IOPSGetActiveUPS", oah_fix_ps_NULL, 0, oah_want_ps },
    { "IOKit", "_IOPSPowerSourceSupported", oah_fix_ps_NULL, 0, oah_want_ps },
    { "IOKit", "_IOHIDManagerCreate", oah_fix_hid_HM_CREATE, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerOpen", oah_fix_hid_HM_OPEN, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerClose", oah_fix_hid_HM_CLOSE, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerGetProperty", oah_fix_hid_HM_GETPROP, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerSetProperty", oah_fix_hid_HM_SETPROP, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerSetDeviceMatching", oah_fix_hid_HM_MATCH, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerSetDeviceMatchingMultiple", oah_fix_hid_HM_MATCHMULTI, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerCopyDevices", oah_fix_hid_HM_COPYDEVICES, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerRegisterDeviceMatchingCallback", oah_fix_hid_HM_REGMATCH, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerRegisterDeviceRemovalCallback", oah_fix_hid_HM_REGREMOVAL, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerRegisterInputReportCallback", oah_fix_hid_HM_REGREPORT, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerRegisterInputValueCallback", oah_fix_hid_HM_REGVALUE, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerSetInputValueMatching", oah_fix_hid_HM_VALMATCH, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerSetInputValueMatchingMultiple", oah_fix_hid_HM_VALMATCHMULTI, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerScheduleWithRunLoop", oah_fix_hid_HM_SCHEDULE, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerUnscheduleFromRunLoop", oah_fix_hid_HM_UNSCHEDULE, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerGetTypeID", oah_fix_hid_HM_TYPEID, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceCreate", oah_fix_hid_HD_CREATE, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceOpen", oah_fix_hid_HD_OPEN, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceClose", oah_fix_hid_HD_CLOSE, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceConformsTo", oah_fix_hid_HD_CONFORMS, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceGetProperty", oah_fix_hid_HD_GETPROP, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceSetProperty", oah_fix_hid_HD_SETPROP, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceCopyMatchingElements", oah_fix_hid_HD_COPYELEMS, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceScheduleWithRunLoop", oah_fix_hid_HD_SCHEDULE, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceUnscheduleFromRunLoop", oah_fix_hid_HD_UNSCHEDULE, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceRegisterRemovalCallback", oah_fix_hid_HD_REGREMOVAL, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceRegisterInputValueCallback", oah_fix_hid_HD_REGVALUE, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceRegisterInputReportCallback", oah_fix_hid_HD_REGREPORT, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceSetInputValueMatching", oah_fix_hid_HD_VALMATCH, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceSetInputValueMatchingMultiple", oah_fix_hid_HD_VALMATCHMULTI, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceSetValue", oah_fix_hid_HD_SETVALUE, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceGetValue", oah_fix_hid_HD_GETVALUE, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceSetValueWithCallback", oah_fix_hid_HD_SETVALUECB, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceGetValueWithCallback", oah_fix_hid_HD_GETVALUECB, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceSetReport", oah_fix_hid_HD_SETREPORT, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceGetReport", oah_fix_hid_HD_GETREPORT, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceGetService", oah_fix_hid_HD_SERVICE, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceGetTypeID", oah_fix_hid_HD_TYPEID, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetCookie", oah_fix_hid_HE_COOKIE, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetType", oah_fix_hid_HE_TYPE, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetCollectionType", oah_fix_hid_HE_COLLTYPE, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetUsagePage", oah_fix_hid_HE_PAGE, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetUsage", oah_fix_hid_HE_USAGE, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementIsVirtual", oah_fix_hid_HE_VIRTUAL, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementIsRelative", oah_fix_hid_HE_RELATIVE, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementIsWrapping", oah_fix_hid_HE_WRAPPING, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementIsArray", oah_fix_hid_HE_ARRAY, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementIsNonLinear", oah_fix_hid_HE_NONLINEAR, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementHasPreferredState", oah_fix_hid_HE_PREFSTATE, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementHasNullState", oah_fix_hid_HE_NULLSTATE, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetName", oah_fix_hid_HE_NAME, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetReportID", oah_fix_hid_HE_REPORTID, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetReportSize", oah_fix_hid_HE_REPORTSIZE, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetReportCount", oah_fix_hid_HE_REPORTCOUNT, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetUnit", oah_fix_hid_HE_UNIT, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetUnitExponent", oah_fix_hid_HE_UNITEXP, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetLogicalMin", oah_fix_hid_HE_LMIN, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetLogicalMax", oah_fix_hid_HE_LMAX, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetPhysicalMin", oah_fix_hid_HE_PMIN, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetPhysicalMax", oah_fix_hid_HE_PMAX, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetDuplicateIndex", oah_fix_hid_HE_DUPINDEX, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetDevice", oah_fix_hid_HE_DEVICE, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetParent", oah_fix_hid_HE_PARENT, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetChildren", oah_fix_hid_HE_CHILDREN, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetProperty", oah_fix_hid_HE_GETPROP, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementSetProperty", oah_fix_hid_HE_SETPROP, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementGetTypeID", oah_fix_hid_HE_TYPEID, 0, oah_want_hid },
    { "IOKit", "_IOHIDValueCreateWithIntegerValue", oah_fix_hid_HV_CREATEINT, 0, oah_want_hid },
    { "IOKit", "_IOHIDValueCreateWithBytes", oah_fix_hid_HV_CREATEBYTES, 0, oah_want_hid },
    { "IOKit", "_IOHIDValueCreateWithBytesNoCopy", oah_fix_hid_HV_CREATEBYTESNC, 0, oah_want_hid },
    { "IOKit", "_IOHIDValueGetElement", oah_fix_hid_HV_ELEMENT, 0, oah_want_hid },
    { "IOKit", "_IOHIDValueGetTimeStamp", oah_fix_hid_HV_TIMESTAMP, 0, oah_want_hid },
    { "IOKit", "_IOHIDValueGetLength", oah_fix_hid_HV_LENGTH, 0, oah_want_hid },
    { "IOKit", "_IOHIDValueGetBytePtr", oah_fix_hid_HV_BYTEPTR, 0, oah_want_hid },
    { "IOKit", "_IOHIDValueGetIntegerValue", oah_fix_hid_HV_INT, 0, oah_want_hid },
    { "IOKit", "_IOHIDValueGetScaledValue", oah_fix_hid_HV_SCALED, 0, oah_want_hid },
    { "IOKit", "_IOHIDValueGetTypeID", oah_fix_hid_HV_TYPEID, 0, oah_want_hid },
    { "IOKit", "_IOHIDQueueCreate", oah_fix_hid_HQ_CREATE, 0, oah_want_hid },
    { "IOKit", "_IOHIDQueueGetDevice", oah_fix_hid_HQ_DEVICE, 0, oah_want_hid },
    { "IOKit", "_IOHIDQueueGetDepth", oah_fix_hid_HQ_GETDEPTH, 0, oah_want_hid },
    { "IOKit", "_IOHIDQueueSetDepth", oah_fix_hid_HQ_SETDEPTH, 0, oah_want_hid },
    { "IOKit", "_IOHIDQueueAddElement", oah_fix_hid_HQ_ADD, 0, oah_want_hid },
    { "IOKit", "_IOHIDQueueRemoveElement", oah_fix_hid_HQ_REMOVE, 0, oah_want_hid },
    { "IOKit", "_IOHIDQueueContainsElement", oah_fix_hid_HQ_CONTAINS, 0, oah_want_hid },
    { "IOKit", "_IOHIDQueueStart", oah_fix_hid_HQ_START, 0, oah_want_hid },
    { "IOKit", "_IOHIDQueueStop", oah_fix_hid_HQ_STOP, 0, oah_want_hid },
    { "IOKit", "_IOHIDQueueRegisterValueAvailableCallback", oah_fix_hid_HQ_REGAVAIL, 0, oah_want_hid },
    { "IOKit", "_IOHIDQueueScheduleWithRunLoop", oah_fix_hid_HQ_SCHEDULE, 0, oah_want_hid },
    { "IOKit", "_IOHIDQueueUnscheduleFromRunLoop", oah_fix_hid_HQ_UNSCHEDULE, 0, oah_want_hid },
    { "IOKit", "_IOHIDQueueCopyNextValue", oah_fix_hid_HQ_COPYNEXT, 0, oah_want_hid },
    { "IOKit", "_IOHIDQueueCopyNextValueWithTimeout", oah_fix_hid_HQ_COPYNEXTTO, 0, oah_want_hid },
    { "IOKit", "_IOHIDQueueGetTypeID", oah_fix_hid_HQ_TYPEID, 0, oah_want_hid },
    { "IOKit", "_IOCreatePlugInInterfaceForService", oah_fix_hidcom_create, 0, oah_want_hidcom },
    { "IOKit", "_IODestroyPlugInInterface", oah_fix_hidcom_destroy, 0, oah_want_hidcom },
    { "IOKit", "_IOHIDDeviceCopyValueMultiple", oah_fix_hid_HX_UNSUP, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceCopyValueMultipleWithCallback", oah_fix_hid_HX_UNSUP, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceSetValueMultiple", oah_fix_hid_HX_UNSUP, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceSetValueMultipleWithCallback", oah_fix_hid_HX_UNSUP, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceGetReportWithCallback", oah_fix_hid_HX_UNSUP, 0, oah_want_hid },
    { "IOKit", "_IOHIDDeviceSetReportWithCallback", oah_fix_hid_HX_UNSUP, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementAttach", oah_fix_hid_HX_UNSUP, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementDetach", oah_fix_hid_HX_UNSUP, 0, oah_want_hid },
    { "IOKit", "_IOHIDManagerSaveToPropertyDomain", oah_fix_hid_HX_UNSUP, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementCopyAttached", oah_fix_hid_HX_NULL, 0, oah_want_hid },
    { "IOKit", "_IOHIDElementCreateWithDictionary", oah_fix_hid_HX_NULL, 0, oah_want_hid },
};
static void oah_gt_parse(void) {
    const char *e = getenv("OAH_GTRACE"), *m;
    unsigned int k;
    g_gt_n = 0;
    if (!getenv("OAH_NO_GUESTFIX"))
        for (k = 0; k < sizeof g_gt_fixes / sizeof *g_gt_fixes && g_gt_n < GT_MAX; k++) {
            oah_gt_t *g;
            if (g_gt_fixes[k].want && !g_gt_fixes[k].want()) continue;
            g = &g_gt[g_gt_n++];
            snprintf(g->lib, sizeof g->lib, "%s", g_gt_fixes[k].lib);
            snprintf(g->sym, sizeof g->sym, "%s", g_gt_fixes[k].sym);
            g->fix = g_gt_fixes[k].fix; g->fixret = g_gt_fixes[k].fixret; g->quiet = 1;
            g->follow = g_gt_fixes[k].follow;
        }
    g_alc_idx = -1;
    if (!getenv("OAH_NO_GUESTFIX") && oah_alc_want() && g_gt_n < GT_MAX) {   /* the hook's stub (no guest symbol) */
        oah_gt_t *g = &g_gt[g_gt_n];
        snprintf(g->lib, sizeof g->lib, "hook"); snprintf(g->sym, sizeof g->sym, "_CGSAdobeSetCursorHook");
        g->fix = oah_fix_alc_hook; g->fixret = 0; g->quiet = 1; g->fn = 1;
        g_alc_idx = g_gt_n++;
    }
    g_midi_idx = -1;
    if (!getenv("OAH_NO_GUESTFIX") && oah_want_midi() && g_gt_n < GT_MAX) {   /* the MIDI delivery thread's body (oah_midi.h) */
        oah_gt_t *g = &g_gt[g_gt_n];
        snprintf(g->lib, sizeof g->lib, "hook"); snprintf(g->sym, sizeof g->sym, "RosettusMIDIDelivery");
        g->fix = oah_fix_midi_pump; g->fixret = 0; g->quiet = 1; g->fn = 1;
        g_midi_idx = g_gt_n++;
    }
    g_cam_idx = -1;
    if (!getenv("OAH_NO_GUESTFIX") && !getenv("OAH_NO_CAMERA") && g_gt_n < GT_MAX) {   /* the camera digitizer's routine (oah_cam.h) */
        oah_gt_t *g = &g_gt[g_gt_n];
        snprintf(g->lib, sizeof g->lib, "hook"); snprintf(g->sym, sizeof g->sym, "RosettusCameraDigitizer");
        g->fix = oah_fix_cam_vdig; g->fixret = 0; g->quiet = 1; g->fn = 1;
        g_cam_idx = g_gt_n++;
    }
    g_fse_pump_idx = g_fse_perf_idx = -1;
    if (!getenv("OAH_NO_GUESTFIX") && oah_want_fsevents() && g_gt_n + 1 < GT_MAX) {   /* FSEvents delivery (oah_fse.h) */
        oah_gt_t *g = &g_gt[g_gt_n];
        snprintf(g->lib, sizeof g->lib, "hook"); snprintf(g->sym, sizeof g->sym, "RosettusFSEventsPump");
        g->fix = oah_fix_fse_pump; g->fixret = 0; g->quiet = 1; g->fn = 1;
        g_fse_pump_idx = g_gt_n++;
        g = &g_gt[g_gt_n];
        snprintf(g->lib, sizeof g->lib, "hook"); snprintf(g->sym, sizeof g->sym, "RosettusFSEventsPerform");
        g->fix = oah_fix_fse_perform; g->fixret = 0; g->quiet = 1; g->fn = 1;
        g_fse_perf_idx = g_gt_n++;
    }
    g_aen_evh_idx = -1;
    if (!getenv("OAH_NO_GUESTFIX") && g_gt_n < GT_MAX) {   /* the guest event handler that runs native Apple Events (oah_aen.h) */
        oah_gt_t *g = &g_gt[g_gt_n];
        snprintf(g->lib, sizeof g->lib, "hook"); snprintf(g->sym, sizeof g->sym, "RosettusAEHandler");
        g->fix = oah_fix_aen_evh; g->fixret = 0; g->quiet = 1; g->fn = 1;
        g_aen_evh_idx = g_gt_n++;
    }
    g_cvl_wait_idx = -1;
    if (!getenv("OAH_NO_GUESTFIX") && g_gt_n < GT_MAX) {   /* the display-link thread's guest loop (oah_cvl.h) */
        oah_gt_t *g = &g_gt[g_gt_n];
        snprintf(g->lib, sizeof g->lib, "hook"); snprintf(g->sym, sizeof g->sym, "RosettusCVLinkWait");
        g->fix = oah_fix_cvl_wait; g->fixret = 0; g->quiet = 1; g->fn = 1;
        g_cvl_wait_idx = g_gt_n++;
    }
    g_gq_pump_idx = g_gq_perf_idx = -1;
    if (!getenv("OAH_NO_GUESTFIX") && oah_want_hid() && g_gt_n + 1 < GT_MAX) {   /* calls into the guest on its run loops (oah_hid.h) */
        oah_gt_t *g = &g_gt[g_gt_n];
        snprintf(g->lib, sizeof g->lib, "hook"); snprintf(g->sym, sizeof g->sym, "RosettusCallbackPump");
        g->fix = oah_fix_gq_pump; g->fixret = 0; g->quiet = 1; g->fn = 1;
        g_gq_pump_idx = g_gt_n++;
        g = &g_gt[g_gt_n];
        snprintf(g->lib, sizeof g->lib, "hook"); snprintf(g->sym, sizeof g->sym, "RosettusCallbackPerform");
        g->fix = oah_fix_gq_perform; g->fixret = 0; g->quiet = 1; g->fn = 1;
        g_gq_perf_idx = g_gt_n++;
    }
    g_hidcom_idx = g_gq_sched_idx = g_gq_cancel_idx = -1;
    if (!getenv("OAH_NO_GUESTFIX") && oah_want_hidcom() && g_gt_n + 2 < GT_MAX) {   /* the old HID interface's methods; run-loop source schedule / cancel (oah_hidcom.h) */
        static const char *const nm[3] = { "RosettusHIDMethod", "RosettusCallbackSchedule", "RosettusCallbackCancel" };
        static void (*const fx[3])(unsigned int *) = { oah_fix_hidcom, oah_fix_gq_sched, oah_fix_gq_cancel };
        int *const ix[3] = { &g_hidcom_idx, &g_gq_sched_idx, &g_gq_cancel_idx };
        int k;
        for (k = 0; k < 3; k++) {
            oah_gt_t *g = &g_gt[g_gt_n];
            snprintf(g->lib, sizeof g->lib, "hook"); snprintf(g->sym, sizeof g->sym, "%s", nm[k]);
            g->fix = fx[k]; g->fixret = 0; g->quiet = 1; g->fn = 1;
            *ix[k] = g_gt_n++;
        }
    }
    if (g_gt_n && pthread_key_create(&g_gt_key, 0)) g_gt_n = 0;
    if (!e) return;
    if ((m = getenv("OAH_GTRACE_MAX"))) g_gt_max = atoi(m);
    if (!g_gt_n) pthread_key_create(&g_gt_key, 0);
    while (*e && g_gt_n < GT_MAX) {
        const char *c = strchr(e, ','), *col = strchr(e, ':');
        size_t L = c ? (size_t)(c - e) : strlen(e);
        if (col && col < e + L) {
            oah_gt_t *g = &g_gt[g_gt_n];
            snprintf(g->lib, sizeof g->lib, "%.*s", (int)(col - e), e);
            snprintf(g->sym, sizeof g->sym, "%.*s", (int)(e + L - col - 1), col + 1);
            g_gt_n++;
        }
        e += L; if (*e == ',') e++;
    }
}
static int oah_gt_uses_reg(unsigned int w, unsigned int r) {
    unsigned int op = w >> 26, rs = (w >> 21) & 31, ra = (w >> 16) & 31, rb = (w >> 11) & 31;
    return rs == r || ra == r || (op == 31 && rb == r);
}
/* The trampoline uses r11 and enters the copied words with r12 = the function's address, as
   dyld stubs and objc_msgSend (IMP in r12) leave it, so PIC prologues and ObjC methods
   ("or r31,r12,r12") work; only words that touch r11 are refused. */
#define oah_gt_uses_r12(w) oah_gt_uses_reg((w), 11)
static void oah_gt_install(oah_gt_t *g, unsigned int fn, int idx) {
    unsigned int w0, w1, *t, pg = fn & ~0xfffu, rs;
    /* word by word: a function whose first two words straddle a page that an earlier patch made a
       region of its own (IOHIDDeviceSetValue at ...ffc) failed a single 8-byte check, every time */
    if (!oah_readable(fn, 4) || !oah_readable(fn + 4, 4)) return;
    if (g->follow) {
        unsigned int k, w, tgt = 0;
        for (k = 0; k < 12 && oah_readable(fn + 4 * k, 4); k++) {
            w = OSSwapBigToHostInt32(((unsigned int *)fn)[k]);
            if ((w >> 26) == 18 && !(w & 3)) {   /* b target (not ba / bl) */
                int li = (int)(w & 0x03fffffcu); if (li & 0x02000000) li -= 0x04000000;
                tgt = fn + 4 * k + (unsigned int)li; break;
            }
        }
        if (!tgt || !oah_readable(tgt, 8)) { fprintf(stderr, "[gtrace] %s: no branch to follow, not hooked\n", g->sym); g->fn = 1; return; }
        if (!g->quiet) fprintf(stderr, "[gtrace] %s: following its branch to %08x\n", g->sym, tgt);
        fn = tgt; pg = fn & ~0xfffu;
    }
    w0 = OSSwapBigToHostInt32(((unsigned int *)fn)[0]); w1 = OSSwapBigToHostInt32(((unsigned int *)fn)[1]);
    int picbcl = w0 == 0x7c0802a6u && w1 == 0x429f0005u;   /* mflr r0 ; bcl 20,31,.+4 (PIC prologue) */
    /* "li r11,imm" / "lis r11,imm" in the first two words (CoreGraphics'
       CGContextCopyWindowContentsToRect: mflr r0 ; li r11,0): only writes r11. The trampolines
       then load CTR before the copied words, so the value they leave in r11 reaches the function.
       Refused when the other word touches r11 or either word touches CTR. */
#define OAH_GT_LIR11(w) ((((w) >> 26) == 14 || ((w) >> 26) == 15) && (((w) >> 21) & 31) == 11 && (((w) >> 16) & 31) == 0)
#define OAH_GT_CTR(w) (((w) >> 26) == 31 && (((w) >> 1) & 0x3ff) != 0 && ((((w) >> 1) & 0x3ff) == 339 || (((w) >> 1) & 0x3ff) == 467) && (((w) >> 11) & 0x3ff) == 0x120)
    int r11w = !picbcl && !getenv("OAH_GT_NOR11W") && !OAH_GT_CTR(w0) && !OAH_GT_CTR(w1) &&
               ((OAH_GT_LIR11(w0) && !oah_gt_uses_r12(w1)) || (OAH_GT_LIR11(w1) && !oah_gt_uses_r12(w0)));
    /* a two-word getter ("lwz r3,8(r3) ; blr": IOHIDElementGetDevice): the copied blr returns through
       the return stub / pass-through trampoline like the function's own return would */
    if ((w0 & 0xffff0000u) == 0x0fe00000u && (w0 & 0xffffu) >= GT_ENTRY && (w0 & 0xffffu) < GT_RET + GT_MAX) {   /* already a row's (two rows, one function) */
        fprintf(stderr, "[gtrace] %s: already hooked by another row, not hooked\n", g->sym); g->fn = 1; return; }
    int leaf2 = w1 == 0x4e800020u && (w0 >> 26) != 16 && (w0 >> 26) != 18 && (w0 >> 26) != 19 && !oah_gt_uses_r12(w0);
    /* fixes that always answer (the guest's code never runs) need no relocatable words */
    int skiponly = g->fix == oah_fix_hid_HX_UNSUP || g->fix == oah_fix_hid_HX_NULL;
    if (!picbcl && !leaf2 && !skiponly && ((w0 >> 26) == 16 || (w0 >> 26) == 18 || (w1 >> 26) == 16 || (w1 >> 26) == 18 || (w0 >> 26) == 19 || (w1 >> 26) == 19 ||
        (!r11w && (oah_gt_uses_r12(w0) || oah_gt_uses_r12(w1))))) {
        fprintf(stderr, "[gtrace] %s: first words %08x %08x not relocatable, not traced\n", g->sym, w0, w1); g->fn = 1; return; }
    /* the trampoline is built on the first call (oah_gt_tramp): only then can it come from the guest's
       own mmap, which translate tracks; a native page made here ended in translate's fatal() the first
       time the return stub ran (TextWrangler, through the icon fix) */
    g->w0 = w0; g->w1 = w1; g->pic = picbcl; g->r11w = r11w; g->tramp = 0; (void)t; (void)rs;
    if (vm_protect(mach_task_self(), pg, 0x2000, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
        fprintf(stderr, "[gtrace] %s: vm_protect failed\n", g->sym); return; }
    ((unsigned int *)fn)[0] = OSSwapHostToBigInt32(0x0fe00000 | (GT_ENTRY + idx));
    ((unsigned int *)fn)[1] = OSSwapHostToBigInt32(0x4e800020);
    vm_protect(mach_task_self(), pg, 0x2000, 0, VM_PROT_READ | VM_PROT_EXECUTE);
    g->fn = fn;
    if (!g->quiet || getenv("OAH_GTRACE")) fprintf(stderr, "[gtrace] %s %s:%s at %08x\n", g->fix ? "fix" : "tracing", g->lib, g->sym, fn);
}
/* called for every newly patched guest image; its symbol table may not be mapped yet, so images
   are kept and retried from oah_gt_retry (every 128 bridge calls, 64 times at most) */
static unsigned int g_gt_img[512]; static int g_gt_nimg, g_gt_open, g_gt_tries;
/* the install name (LC_ID_DYLIB) of the guest image at hdr, "" if it has none or is not readable */
static void oah_gt_image_id(unsigned int hdr, char *out, size_t cap) {
    unsigned int hb[8], ncmds, sz, off = 28, i;
    out[0] = 0;
    if (!oah_peek2(hdr, hb) || hb[0] != OSSwapHostToBigInt32(0xfeedfaceu)) return;
    ncmds = BE32(hb[4]); sz = BE32(hb[5]);
    if (!oah_readable(hdr, 28 + sz)) return;
    for (i = 0; i < ncmds && off + 12 <= 28 + sz; i++) {
        const unsigned int *lc = (const unsigned int *)(hdr + off);
        unsigned int csz = BE32(lc[1]);
        if (csz < 8) break;
        if (BE32(lc[0]) == 0xd) {
            unsigned int no = BE32(lc[2]);
            if (no < csz && memchr((const char *)lc + no, 0, csz - no)) strlcpy(out, (const char *)lc + no, cap);
            return;
        }
        off += csz;
    }
}
/* Each open row used to read the image's header again (oah_guest_sym_in) just to find that the
   library name did not match: hundreds of rows, every pending image, on up to 400 rescans. Keynote
   spent seconds of its launch here. The install name is read once per scan and only rows for that
   library are looked up (same match as oah_guest_sym_in: the row's library is part of the name).
   OAH_NO_GTSCANFAST=1: the old way. */
static int oah_gt_scan(unsigned int hdr) {
    int i, open = 0;
    char id[512];
    static int slow = -1;
    if (slow < 0) slow = getenv("OAH_NO_GTSCANFAST") != 0;
    if (!slow) oah_gt_image_id(hdr, id, sizeof id);
    for (i = 0; i < g_gt_n; i++) {
        unsigned int a;
        if (g_gt[i].fn) continue;
        /* "abs:0x1234": an address (stripped code, e.g. a main executable's own functions) */
        if (!strcmp(g_gt[i].lib, "abs")) { a = (unsigned int)strtoul(g_gt[i].sym, 0, 16); if (a && oah_readable(a, 8)) oah_gt_install(&g_gt[i], a, i); }
        else if (!slow && (!id[0] || !strstr(id, g_gt[i].lib))) ;   /* not this library (or not readable yet) */
        else if ((a = oah_guest_sym_in(hdr, g_gt[i].lib, g_gt[i].sym))) oah_gt_install(&g_gt[i], a, i);
        if (!g_gt[i].fn) open++;
    }
    return open;
}
static void oah_gt_image(unsigned int hdr) {
    if (g_gt_n < 0) oah_gt_parse();
    if (g_gt_n <= 0) return;
    g_gt_open = oah_gt_scan(hdr);
    if (g_gt_open && g_gt_nimg < 512) g_gt_img[g_gt_nimg++] = hdr;
}
/* The guest's image list is scanned for the fix rows when an image is patched, but a library's
   symbol table may not be mapped yet then, and the retries ran only every 128 bridge calls: a
   program that called, say, the keychain or thread_info in its first instructions got the
   unfixed guest code. dyld maps the rest of an image with mmap; after a guest mmap, the next guest
   system call rescans the images that still have rows open. Called from the syscall hook. */
static volatile int g_gt_mapped;
static pthread_mutex_t g_gt_rescan_mu = PTHREAD_MUTEX_INITIALIZER;
static void oah_gt_syscall_tick(unsigned int num) {
    int k;
    if (num == 197) { g_gt_mapped = 1; return; }               /* mmap */
    static int count; static double last;
    double now;
    if (!g_gt_mapped || g_gt_n <= 0 || !g_gt_open || count > 400) return;
    now = CFAbsoluteTimeGetCurrent();
    if (now - last < 0.02) return;                             /* malloc maps memory too: at most 50 a second, 400 in all */
    if (pthread_mutex_trylock(&g_gt_rescan_mu)) return;
    last = now; count++;
    g_gt_mapped = 0;
    for (k = 0; k < g_gt_nimg && g_gt_open; k++) g_gt_open = oah_gt_scan(g_gt_img[k]);
    pthread_mutex_unlock(&g_gt_rescan_mu);
}
static void oah_gt_retry(void) {
    static unsigned long n;
    int k;
    static int lim = -1;
    if (lim < 0) lim = getenv("OAH_GTRACE") ? 64 + 200 : 64;
    if (g_gt_n <= 0 || !g_gt_open || g_gt_tries > lim || (++n & (g_gt_tries < 64 ? 127 : 8191))) return;
    g_gt_tries++;
    for (k = 0; k < g_gt_nimg && g_gt_open; k++) g_gt_open = oah_gt_scan(g_gt_img[k]);
    if (g_gt_tries == 64 && g_gt_open) { int i; for (i = 0; i < g_gt_n; i++) if (!g_gt[i].fn) fprintf(stderr, "[gtrace] %s:%s not found yet\n", g_gt[i].lib, g_gt[i].sym); }
    if (g_gt_tries > 64 && g_gt_open) {
        /* libraries loaded later, or never handed to the image patcher (prebuilt PowerPC shared
           cache, e.g. AppKit in a Carbon app that loads it for the Save dialog): scan memory.
           Only for what OAH_GTRACE asked to trace: a scan of a large app's memory takes seconds,
           and the built-in fixes for libraries an app never loads would repeat it (Photoshop's
           main thread spent its time in these scans and stopped responding). */
        int i, open = 0; unsigned int a;
        for (i = 0; i < g_gt_n; i++) {
            if (g_gt[i].fn) continue;
            open++;
            if (!getenv("OAH_GTRACE") || g_gt[i].fix || g_gt[i].fixret || !strcmp(g_gt[i].lib, "abs")) continue;
            if ((a = oah_guest_sym_q(g_gt[i].lib, g_gt[i].sym, 1))) { oah_gt_install(&g_gt[i], a, i); open--; }
        }
        g_gt_open = open;
    }
}
static unsigned int oah_gt_tramp(oah_gt_t *g) {
    unsigned int *t, rs, fn = g->fn;
    if (g->tramp) return g->tramp;
    t = (unsigned int *)oah_guest_code_alloc(18 * 4);
    if (!t) return 0;
    rs = (unsigned int)(t + 16);
    t[0] = OSSwapHostToBigInt32(0x3d600000 | (rs >> 16));            /* lis r11,hi(retstub) */
    t[1] = OSSwapHostToBigInt32(0x616b0000 | (rs & 0xffff));         /* ori r11,r11,lo */
    t[2] = OSSwapHostToBigInt32(0x7d6803a6);                         /* mtlr r11 */
    t[3] = OSSwapHostToBigInt32(0x3d800000 | (fn >> 16));            /* lis r12,hi(fn): r12 = entry */
    t[4] = OSSwapHostToBigInt32(0x618c0000 | (fn & 0xffff));         /* ori r12,r12,lo */
    t[5] = OSSwapHostToBigInt32(g->w0); t[6] = OSSwapHostToBigInt32(g->w1);
    t[7] = OSSwapHostToBigInt32(0x3d600000 | ((fn + 8) >> 16));      /* lis r11,hi(fn+8) */
    t[8] = OSSwapHostToBigInt32(0x616b0000 | ((fn + 8) & 0xffff));
    t[9] = OSSwapHostToBigInt32(0x7d6903a6);                         /* mtctr r11 */
    t[10] = OSSwapHostToBigInt32(0x4e800420);                        /* bctr */
    if (g->pic) {   /* the bcl's only effect is LR = fn+8: do that, then continue at fn+8 */
        t[6] = OSSwapHostToBigInt32(0x3d600000 | ((fn + 8) >> 16));
        t[7] = OSSwapHostToBigInt32(0x616b0000 | ((fn + 8) & 0xffff));
        t[8] = OSSwapHostToBigInt32(0x7d6803a6);                     /* mtlr r11 */
        t[9] = OSSwapHostToBigInt32(0x7d6903a6);                     /* mtctr r11 */
        t[10] = OSSwapHostToBigInt32(0x4e800420);                    /* bctr */
    }
    if (g->r11w) {  /* the copied words set r11: CTR = fn+8 first, then the words, then bctr */
        t[5] = OSSwapHostToBigInt32(0x3d600000 | ((fn + 8) >> 16));
        t[6] = OSSwapHostToBigInt32(0x616b0000 | ((fn + 8) & 0xffff));
        t[7] = OSSwapHostToBigInt32(0x7d6903a6);                     /* mtctr r11 */
        t[8] = OSSwapHostToBigInt32(g->w0); t[9] = OSSwapHostToBigInt32(g->w1);
        t[10] = OSSwapHostToBigInt32(0x4e800420);                    /* bctr */
    }
    t[16] = OSSwapHostToBigInt32(0x0fe00000 | (GT_RET + (unsigned int)(g - g_gt)));   /* return stub */
    t[17] = OSSwapHostToBigInt32(0x4e800020);
    g->stub = t + 16;
    g->tramp = (unsigned int)t;
    return g->tramp;
}
/* Pass-through trampoline for fixes that need no return hook: the entry trap leaves the caller's
   return address in r11 (volatile at a function's entry; the normal trampoline clobbers it too) and
   this puts it back in LR before the function's first two words run. The hooked function then runs
   with its real return address, as if it were not hooked. With the normal trampoline its LR was
   our return stub: the function saved that as its return address, and a C++ exception passing
   through it (Metrowerks MSL in Adobe CS2) could not find the real caller, landed in the catch
   block of a function that had already returned and crashed (Illustrator CS2 at start, after the
   with the AESendMessage fix and others). It also kept the gtrace call stack from going stale
   when an exception or longjmp skipped the return. OAH_GT_NOPASSTHRU=1: always the return hook. */
static unsigned int oah_gt_ptramp(oah_gt_t *g) {
    unsigned int *t, fn = g->fn;
    if (g->ptramp) return g->ptramp;
    t = (unsigned int *)oah_guest_code_alloc(16 * 4);
    if (!t) return 0;
    t[0] = OSSwapHostToBigInt32(0x7d6803a6);                         /* mtlr r11 (the caller's return address) */
    t[1] = OSSwapHostToBigInt32(0x3d800000 | (fn >> 16));            /* lis r12,hi(fn): r12 = entry */
    t[2] = OSSwapHostToBigInt32(0x618c0000 | (fn & 0xffff));         /* ori r12,r12,lo */
    t[3] = OSSwapHostToBigInt32(g->w0); t[4] = OSSwapHostToBigInt32(g->w1);
    t[5] = OSSwapHostToBigInt32(0x3d600000 | ((fn + 8) >> 16));      /* lis r11,hi(fn+8) */
    t[6] = OSSwapHostToBigInt32(0x616b0000 | ((fn + 8) & 0xffff));
    t[7] = OSSwapHostToBigInt32(0x7d6903a6);                         /* mtctr r11 */
    t[8] = OSSwapHostToBigInt32(0x4e800420);                         /* bctr */
    if (g->pic) {   /* mflr r0 ; bcl: r0 = the caller's LR, then LR = fn+8 */
        t[4] = OSSwapHostToBigInt32(0x3d600000 | ((fn + 8) >> 16));
        t[5] = OSSwapHostToBigInt32(0x616b0000 | ((fn + 8) & 0xffff));
        t[6] = OSSwapHostToBigInt32(0x7d6803a6);                     /* mtlr r11 */
        t[7] = OSSwapHostToBigInt32(0x7d6903a6);                     /* mtctr r11 */
        t[8] = OSSwapHostToBigInt32(0x4e800420);                     /* bctr */
    }
    if (g->r11w) {  /* the copied words set r11 (see oah_gt_install): CTR first */
        t[3] = OSSwapHostToBigInt32(0x3d600000 | ((fn + 8) >> 16));
        t[4] = OSSwapHostToBigInt32(0x616b0000 | ((fn + 8) & 0xffff));
        t[5] = OSSwapHostToBigInt32(0x7d6903a6);                     /* mtctr r11 */
        t[6] = OSSwapHostToBigInt32(g->w0); t[7] = OSSwapHostToBigInt32(g->w1);
        t[8] = OSSwapHostToBigInt32(0x4e800420);                     /* bctr */
    }
    g->ptramp = (unsigned int)t;
    return g->ptramp;
}
static int oah_gt_is(unsigned int id) { return g_gt_n > 0 && id >= GT_ENTRY && id < GT_RET + GT_MAX && (id & 0x7ff) < (unsigned)g_gt_n; }
static void oah_gt_call(unsigned int id, unsigned int *R) {
    oah_gt_t *g = &g_gt[id & 0x7ff];
    oah_gt_stack *st = pthread_getspecific(g_gt_key);
    double *F = (double *)(R + 0x1e8 / 4);
    if (!st) { st = calloc(1, sizeof *st); pthread_setspecific(g_gt_key, st); }
    if (id < GT_RET) {
        g->n++;
        {   /* a fix that makes guest calls may run other hooked functions on this thread: each of
               those starts clean and leaves this call's flag as it was */
            int o_skip = st->skip; unsigned int o_ret = st->skip_ret;
            st->skip = 0; st->skip_ret = 0;
            if (g->fix) g->fix(R);
            if (st->skip) { R[3] = st->skip_ret; st->skip = o_skip; st->skip_ret = o_ret; return; }   /* the blr after the trap returns to the caller */
            st->skip = o_skip; st->skip_ret = o_ret;
        }
        if (g->quiet && !g->fixret) {   /* no return hook needed: run on with the real return address */
            static int nopt = -1;
            unsigned int pt;
            if (nopt < 0) nopt = getenv("OAH_GT_NOPASSTHRU") ? 1 : 0;
            if (!nopt && (pt = oah_gt_ptramp(g))) {
                R[11] = R[0x90 / 4];
                R[0x90 / 4] = pt;   /* the blr after the trap goes to the pass-through trampoline */
                return;
            }
        }
        {   /* OAH_GTRACE_EVERY=N: past OAH_GTRACE_MAX, still every Nth call (what a steady state calls) */
            static int ev = -1;
            if (ev < 0) { const char *e = getenv("OAH_GTRACE_EVERY"); ev = e ? atoi(e) : 0; }
            if (ev > 0 && g->n > (unsigned long)g_gt_max && g->n % ev == 0) g->n_every = 1; else g->n_every = 0;
        }
        if (!g->quiet && (g->n <= (unsigned long)g_gt_max || g->n_every)) {
            unsigned int b = 0; const char *nm = oah_guest_image_name(R[0x90 / 4], &b), *sl = strrchr(nm, '/');
            fprintf(stderr, "[gtrace] %*s> %s(%08x %08x %08x %08x %08x %08x %08x %08x | %g %g %g %g) from %s+0x%x\n",
                    st->sp * 2, "", g->sym, R[3], R[4], R[5], R[6], R[7], R[8], R[9], R[10], F[1], F[2], F[3], F[4],
                    sl ? sl + 1 : nm, R[0x90 / 4] - b);
            if (getenv("OAH_GTRACE_SP")) fprintf(stderr, "[gtrace] %*s  r1=%08x r31=%08x lr=%08x thread %x\n", st->sp * 2, "", R[1], R[31], R[0x90 / 4], pthread_mach_thread_np(pthread_self()));
            if (strstr(g->sym, "_ATSUSetAttributes") || strstr(g->sym, "_ATSUSetLayoutControls")) {   /* (obj, n, tags, sizes, values) */
                unsigned int i, n = R[4] < 16 ? R[4] : 16, t, sz, vp, w, k;
                for (i = 0; i < n; i++) {
                    if (!probe_read(R[5] + 4 * i, &t) || !probe_read(R[6] + 4 * i, &sz) || !probe_read(R[7] + 4 * i, &vp)) break;
                    t = OSSwapBigToHostInt32(t); sz = OSSwapBigToHostInt32(sz); vp = OSSwapBigToHostInt32(vp);
                    fprintf(stderr, "[gtrace] %*s  attr %x size %u:", st->sp * 2, "", t, sz);
                    for (k = 0; k < sz && k < 24; k += 4) if (probe_read(vp + k, &w)) fprintf(stderr, " %08x", OSSwapBigToHostInt32(w));
                    fprintf(stderr, "\n");
                }
            }
        }
        if (!g->quiet && g->n <= (unsigned long)g_gt_max && getenv("OAH_GTRACE_STR")) {   /* show string-looking arguments */
            int a;
            for (a = 3; a <= 8; a++) {
                unsigned char b[64]; vm_size_t got = 0; int k, ok = 1, pas;
                if (R[a] < 0x1000 || vm_read_overwrite(mach_task_self(), R[a], 63, (vm_address_t)b, &got) || got < 8) continue;
                b[63] = 0; pas = b[0] > 0 && b[0] < 63;
                /* a constant CFString (isa, flags 0x7c8, C string, length): print its text */
                if (got >= 16 && OSReadBigInt32(b, 4) == 0x7c8) {
                    unsigned int cp = OSReadBigInt32(b, 8), cl = OSReadBigInt32(b, 12);
                    unsigned char s2[64]; vm_size_t g2 = 0;
                    if (cp > 0x1000 && cl < 64 && !vm_read_overwrite(mach_task_self(), cp, cl ? cl : 1, (vm_address_t)s2, &g2) && g2) {
                        fprintf(stderr, "[gtrace] %*s  r%d @\"%.*s\"\n", st->sp * 2, "", a, (int)cl, s2);
                        continue;
                    }
                }
                for (k = pas ? 1 : 0; k < (pas ? 1 + b[0] : 4); k++) if (b[k] < 32 || b[k] > 126) { ok = 0; break; }
                if (!ok) continue;
                if (pas) fprintf(stderr, "[gtrace] %*s  r%d \"%.*s\" (pascal)\n", st->sp * 2, "", a, b[0], b + 1);
                else { for (k = 0; k < 63 && b[k] >= 32 && b[k] < 127; k++); fprintf(stderr, "[gtrace] %*s  r%d \"%.*s\"\n", st->sp * 2, "", a, k, b); }
            }
        }
        if (!g->quiet && g->n <= (unsigned long)g_gt_max && getenv("OAH_GTRACE_XMLNODE")) {
            /* OAH_GTRACE_XMLNODE=n: rN is a libxml2 xmlNode; print its element name and attributes */
            unsigned int nd = R[atoi(getenv("OAH_GTRACE_XMLNODE")) & 31], v[12], a[7], c[11], k;
            char nm[64], val[64]; vm_size_t got = 0;
            if (nd > 0x1000 && !vm_read_overwrite(mach_task_self(), nd, sizeof v, (vm_address_t)v, &got) && got == sizeof v) {
                unsigned int at = OSSwapBigToHostInt32(v[11]);
                oah_gt_cstr(OSSwapBigToHostInt32(v[2]), nm, sizeof nm);
                fprintf(stderr, "[gtrace] %*s  node %08x <%s", st->sp * 2, "", nd, nm);
                for (k = 0; k < 10 && at > 0x1000; k++) {
                    got = 0;
                    if (vm_read_overwrite(mach_task_self(), at, sizeof a, (vm_address_t)a, &got) || got != sizeof a) break;
                    oah_gt_cstr(OSSwapBigToHostInt32(a[2]), nm, sizeof nm); val[0] = 0;
                    if (OSSwapBigToHostInt32(a[3]) > 0x1000 && !vm_read_overwrite(mach_task_self(), OSSwapBigToHostInt32(a[3]), sizeof c, (vm_address_t)c, &got) && got == sizeof c)
                        oah_gt_cstr(OSSwapBigToHostInt32(c[10]), val, sizeof val);
                    fprintf(stderr, " %s=\"%s\"", nm, val);
                    at = OSSwapBigToHostInt32(a[6]);
                }
                fprintf(stderr, ">\n");
            }
        }
        if (!g->quiet && g->n <= (unsigned long)g_gt_max && getenv("OAH_GTRACE_CFSTR")) {   /* rN as a (non-constant) CFString */
            int a = atoi(getenv("OAH_GTRACE_CFSTR"));
            static unsigned int ftid, fstid, fgcs;
            if (!ftid) { ftid = oah_guest_sym("CoreFoundation", "_CFGetTypeID"); fstid = oah_guest_sym("CoreFoundation", "_CFStringGetTypeID"); fgcs = oah_guest_sym("CoreFoundation", "_CFStringGetCString"); }
            if (a >= 3 && a <= 10 && R[a] > 0x1000 && ftid && fstid && fgcs) {
                unsigned int x[4], save[8], k; char buf[256];
                for (k = 0; k < 8; k++) save[k] = R[3 + k];
                x[0] = R[a];
                if (oah_callppc_n("gt_tid", ftid, 1, x) == oah_callppc_n("gt_stid", fstid, 0, x)) {
                    buf[0] = 0; x[0] = save[a - 3]; x[1] = (unsigned int)(uintptr_t)buf; x[2] = sizeof buf; x[3] = 0x08000100u;
                    if (oah_callppc_n("gt_gcs", fgcs, 4, x)) fprintf(stderr, "[gtrace] %*s  r%d cf\"%s\"\n", st->sp * 2, "", a, buf);
                }
                for (k = 0; k < 8; k++) R[3 + k] = save[k];
            }
        }
        if (!g->quiet && g->n <= (unsigned long)g_gt_max && getenv("OAH_GTRACE_CFDATA")) {   /* rN as a guest CFData: its bytes as text */
            int a = atoi(getenv("OAH_GTRACE_CFDATA"));
            static unsigned int ftid, fdtid, fptr, flen;
            if (!ftid) { ftid = oah_guest_sym("CoreFoundation", "_CFGetTypeID"); fdtid = oah_guest_sym("CoreFoundation", "_CFDataGetTypeID");
                         fptr = oah_guest_sym("CoreFoundation", "_CFDataGetBytePtr"); flen = oah_guest_sym("CoreFoundation", "_CFDataGetLength"); }
            if (a >= 3 && a <= 10 && R[a] > 0x1000 && ftid && fdtid && fptr && flen) {
                unsigned int x[2], save[8], k, p, L; 
                for (k = 0; k < 8; k++) save[k] = R[3 + k];
                x[0] = R[a];
                if (oah_callppc_n("gt_tid", ftid, 1, x) == oah_callppc_n("gt_dtid", fdtid, 0, x)) {
                    x[0] = save[a - 3]; L = oah_callppc_n("gt_dlen", flen, 1, x);
                    x[0] = save[a - 3]; p = oah_callppc_n("gt_dptr", fptr, 1, x);
                    fprintf(stderr, "[gtrace] %*s  r%d data(%u) \"", st->sp * 2, "", a, L);
                    for (k = 0; k < L && k < 600 && p; k++) { unsigned char c = ((unsigned char *)(uintptr_t)p)[k]; fputc(c >= 32 && c < 127 ? c : '.', stderr); }
                    fprintf(stderr, "\"\n");
                }
                for (k = 0; k < 8; k++) R[3 + k] = save[k];
            }
        }
        if (!g->quiet && (g->n <= (unsigned long)g_gt_max || g->n_every) && getenv("OAH_GTRACE_BT")) {   /* guest back trace, and the words at r4 */
            unsigned int sp = R[1], back, lr, k, w;
            fprintf(stderr, "[gtrace] %*s  bt:", st->sp * 2, "");
            for (k = 0; k < 16 && sp > 0x1000; k++) {
                if (!probe_read(sp, &back)) break;
                back = OSSwapBigToHostInt32(back);
                if (!back || (back <= sp && !getenv("OAH_GTRACE_BTANY")) || !probe_read(back + 8, &lr)) break;
                fprintf(stderr, getenv("OAH_GTRACE_BTANY") ? " %08x@%08x" : " %08x", OSSwapBigToHostInt32(lr), back);
                if (getenv("OAH_GTRACE_BTNAMES")) {   /* the backing file and the offset in it (for nm on Snow Leopard) */
                    unsigned int ra = OSSwapBigToHostInt32(lr);
                    vm_address_t ra0 = ra; vm_size_t rs = 0; vm_region_basic_info_data_t ri;
                    mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT; mach_port_t ro;
                    char fb[1024]; const char *fn = "";
                    if (proc_regionfilename(getpid(), ra, fb, sizeof fb) > 0) { fn = strrchr(fb, '/'); fn = fn ? fn + 1 : fb; }
                    {   unsigned int ib = 0; const char *in = oah_guest_image_name(ra, &ib), *isl = in ? strrchr(in, '/') : 0;
                        if (in && *in && ib && ib <= ra) fprintf(stderr, "(%s+0x%x)", isl ? isl + 1 : in, ra - ib);
                        else if (*fn && vm_region(mach_task_self(), &ra0, &rs, VM_REGION_BASIC_INFO, (vm_region_info_t)&ri, &rc, &ro) == KERN_SUCCESS && ra0 <= ra)
                            fprintf(stderr, "(%s@0x%x)", fn, (unsigned int)(ra - ra0 + ri.offset)); }
                }
                sp = back;
            }
            if (getenv("OAH_GTRACE_UCTX")) {   /* rN is a PowerPC ucontext (signal handlers: r5): the interrupted pc, lr, r1 */
                unsigned int u = R[atoi(getenv("OAH_GTRACE_UCTX")) & 31], mc = 0, v[40];
                if (u > 0x1000 && probe_read(u + 0x1c, &mc) && (mc = OSSwapBigToHostInt32(mc)) > 0x1000) {
                    for (k = 0; k < 40; k++) { if (!probe_read(mc + 0x20 + 4 * k, &w)) break; v[k] = OSSwapBigToHostInt32(w); }
                    if (k == 40) {
                        unsigned int s2 = v[3], b2;
                        { unsigned int q, uw; fprintf(stderr, "\n[gtrace] uctx %08x:", u); for (q = 0; q < 12 && probe_read(u + 4 * q, &uw); q++) fprintf(stderr, " %08x", OSSwapBigToHostInt32(uw));
                          fprintf(stderr, "\n[gtrace] mctx %08x:", mc); for (q = 0; q < 48 && probe_read(mc + 4 * q, &uw); q++) fprintf(stderr, " %08x", OSSwapBigToHostInt32(uw)); }
                        fprintf(stderr, "\n[gtrace] %*s  uctx: srr0=%08x lr=%08x r1=%08x r3=%08x ctr=%08x; bt from r1:", st->sp * 2, "", v[0], v[36], v[3], v[5], v[37]);
                        for (k = 0; k < 16 && s2 > 0x1000; k++) {
                            if (!probe_read(s2, &b2)) break;
                            b2 = OSSwapBigToHostInt32(b2);
                            if (!b2 || b2 <= s2 || !probe_read(b2 + 8, &w)) break;
                            fprintf(stderr, " %08x", OSSwapBigToHostInt32(w));
                            s2 = b2;
                        }
                    }
                }
            }
            if (getenv("OAH_GTRACE_STACKSCAN")) {   /* code-address-looking words on the guest stack above r1 */
                unsigned int q, nq = (unsigned int)atoi(getenv("OAH_GTRACE_STACKSCAN")), sw, cnt = 0, base = R[1], miss = 0;
                if (getenv("OAH_GTRACE_SCANUCTX")) {   /* from the interrupted r1 of the ucontext in rN */
                    unsigned int u = R[atoi(getenv("OAH_GTRACE_SCANUCTX")) & 31], mcx, r1x;
                    if (probe_read(u + 0x1c, &mcx) && probe_read(OSSwapBigToHostInt32(mcx) + 0x2c, &r1x)) base = OSSwapBigToHostInt32(r1x);
                }
                fprintf(stderr, "\n[gtrace] stackscan from %08x:", base);
                for (q = 0; q < nq && cnt < 200; q++) {
                    if (!probe_read(base + 4 * q, &sw)) { if (miss++ < 3) fprintf(stderr, " [unreadable +%x]", 4 * q); q += 1023; continue; }
                    sw = OSSwapBigToHostInt32(sw);
                    if (getenv("OAH_GTRACE_STACKRAW") ? (sw != 0 && sw != 0xebebebeb) : (sw >= 0x2000 && sw < 0x0a000000 && !(sw & 3))) { fprintf(stderr, " +%x:%08x", 4 * q, sw); cnt++; }
                }
            }
            fprintf(stderr, "\n[gtrace] %*s  *r4:", st->sp * 2, "");
            for (k = 0; k < 8 && R[4] > 0x1000; k++) { if (!probe_read(R[4] + 4 * k, &w)) break; fprintf(stderr, " %08x", OSSwapBigToHostInt32(w)); }
            fprintf(stderr, "\n");
        }
        if (!g->quiet && g->n <= (unsigned long)g_gt_max && getenv("OAH_GTRACE_ENTRYMEM") && R[3] > 0x1000) {
            /* OAH_GTRACE_ENTRYMEM=off:count[,off:count]: big-endian words at r3+off when the call starts
               (for functions that never return, such as a longjmp) */
            const char *e = getenv("OAH_GTRACE_ENTRYMEM");
            while (*e) {
                unsigned int off = (unsigned int)strtoul(e, (char **)&e, 0), cnt = 1, k, w;
                if (*e == ':') cnt = (unsigned int)strtoul(e + 1, (char **)&e, 0);
                fprintf(stderr, "[gtrace] %*s  r3+%x:", st->sp * 2, "", off);
                for (k = 0; k < cnt && k < 64 && probe_read(R[3] + off + 4 * k, &w); k++) fprintf(stderr, " %08x", OSSwapBigToHostInt32(w));
                fprintf(stderr, "\n");
                while (*e == ',' || *e == ' ') e++;
            }
        }
        {   /* OAH_GTRACE_ARG=n: OAH_GTRACE_PEEK / OAH_GTRACE_MEM3 look at argument rN (3..10) instead
               of r3 (WaitNextEvent's EventRecord is r4). Fixes still get r3 as their a0. */
            static int argr = -1;
            if (argr < 0) { const char *v = getenv("OAH_GTRACE_ARG"); argr = v ? atoi(v) : 3; if (argr < 3 || argr > 10) argr = 3; }
            if (st->sp < 128) { st->lr[st->sp] = R[0x90 / 4]; st->a0[st->sp] = (g->fixret || argr == 3) ? R[3] : R[argr]; }
            if (st->sp < 128 && oah_gt_nv()) { int k; for (k = 0; k < 19; k++) st->nv[st->sp][k] = R[13 + k]; st->nv[st->sp][19] = R[1]; }
        }
        st->sp++;
        R[0x90 / 4] = oah_gt_tramp(g);   /* the blr after the trap goes to the trampoline */
    } else {
        unsigned int lr = 0, a0 = 0;
        if (st->sp > 0) { st->sp--; if (st->sp < 128) { lr = st->lr[st->sp]; a0 = st->a0[st->sp]; } }
        if (g->fixret) g->fixret(R, a0);
        if (st->sp < 128 && oah_gt_nv()) {   /* a callee that did not preserve the non-volatile registers */
            int k, bad = 0;
            for (k = 0; k < 20; k++) {
                unsigned int now = k < 19 ? R[13 + k] : R[1];
                if (now != st->nv[st->sp][k]) {
                    if (!bad++) fprintf(stderr, "[gtrace] %*s! %s returned with changed non-volatile registers:", st->sp * 2, "", g->sym);
                    fprintf(stderr, " %s%d %08x->%08x", k < 19 ? "r" : "r", k < 19 ? 13 + k : 1, st->nv[st->sp][k], now);
                }
            }
            if (bad) fprintf(stderr, "\n");
        }
        if (!g->quiet && g->n <= (unsigned long)g_gt_max)
        {   unsigned int mp = 0, w0 = 0;
            if (getenv("OAH_GTRACE_DEREF") && R[3] > 0x1000 && probe_read(R[3], &mp)) {   /* handles: master pointer and first word */
                mp = OSSwapBigToHostInt32(mp);
                if (mp > 0x1000 && probe_read(mp, &w0)) w0 = OSSwapBigToHostInt32(w0);
                fprintf(stderr, "[gtrace] %*s< %s = %08x -> %08x -> %08x\n", st->sp * 2, "", g->sym, R[3], mp, w0);
            } else
            fprintf(stderr, "[gtrace] %*s< %s = %08x (f1 %g)\n", st->sp * 2, "", g->sym, R[3], F[1]);
            if (getenv("OAH_GTRACE_PEEK") && a0 > 0x1000) {   /* bytes at r3 (as passed) + offsets, at return: "0x628,0xaa0" */
                const char *pk = getenv("OAH_GTRACE_PEEK"); unsigned int w;
                fprintf(stderr, "[gtrace] %*s  peek %08x:", st->sp * 2, "", a0);
                while (*pk) { unsigned int off = (unsigned int)strtoul(pk, (char **)&pk, 0);
                    if (probe_read((a0 + off) & ~3u, &w)) fprintf(stderr, " +%x=%02x", off, ((unsigned char *)&w)[(a0 + off) & 3]);
                    while (*pk == ',' || *pk == ' ') pk++; if (!*pk) break; }
                fprintf(stderr, "\n");
            }
            if (getenv("OAH_GTRACE_MEM3") && a0 > 0x1000) {   /* parameter blocks: the words at the first argument */
                unsigned int k, w;
                fprintf(stderr, "[gtrace] %*s  @r3 %08x:", st->sp * 2, "", a0);
                for (k = 0; k < 32 && probe_read(a0 + 4 * k, &w); k++) fprintf(stderr, " %08x", OSSwapBigToHostInt32(w));
                fprintf(stderr, "\n");
            }
        }
        if (g_gt_redirect) {   /* a fixret asked to run a guest stub on the way back (it returns to lr itself) */
            if (g_gt_redirect_lrslot) *(volatile unsigned int *)(uintptr_t)g_gt_redirect_lrslot = OSSwapHostToBigInt32(lr);
            R[0x90 / 4] = g_gt_redirect;
            g_gt_redirect = g_gt_redirect_lrslot = 0;
        } else
        R[0x90 / 4] = lr;
    }
}
static const char *oah_gt_name(unsigned int id) { return g_gt[id & 0x7ff].sym; }

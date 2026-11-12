#include <malloc/malloc.h>
#include <CoreFoundation/CoreFoundation.h>
#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/ndr.h>
#include <libkern/OSByteOrder.h>
#include <spawn.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/mman.h>
#include <libproc.h>
#include <semaphore.h>
#include <dlfcn.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <crt_externs.h>
static const void *oah_memmem(const void *h, size_t hn, const void *nd, size_t nn) {
    const char *p = (const char *)h, *e;
    if (nn == 0 || hn < nn) return 0;
    for (e = p + hn - nn; p <= e; p++)
        if (*p == *(const char *)nd && !memcmp(p, nd, nn)) return p;
    return 0;
}
#include <pthread.h>
#include <servers/bootstrap.h>
#include <mach/mig_errors.h>
#include "cp_data.h"

static char g_self_path[1024];
/* the install root (translator, guest tree, this library): OAH_ROOT, else this library's folder */
static char g_rosetta_root_buf[1024] = "/Library/Rosettus";      /* replaced at load */
static const char *g_rosetta_root = g_rosetta_root_buf;
static size_t g_rosetta_root_len = 16;
static char g_dyld_root_env[1100] = "DYLD_ROOT_PATH=/Library/Rosettus";
static int g_root_inited;
/* HOME and OAH_GUEST_EXE as they were at start: the strings live at the top of the main stack, which a
   guest stack overrun can overwrite (AppleWorks' RTF translator), and a crash report must not read them
   then (strlen ran into the end of the stack inside the crash handler and faulted there forever) */
static char g_env_home[1024], g_env_exe[1024];
static const char *oah_env_home(void) { return g_env_home[0] ? g_env_home : 0; }
static const char *oah_env_exe(void) { return g_env_exe[0] ? g_env_exe : 0; }
__attribute__((constructor)) static void oah_root_init(void) {
    if (g_root_inited++) return;
    {   const char *h = getenv("HOME"), *x = getenv("OAH_GUEST_EXE");
        if (h) strlcpy(g_env_home, h, sizeof g_env_home);
        if (x) strlcpy(g_env_exe, x, sizeof g_env_exe); }
    const char *e = getenv("OAH_ROOT");
    Dl_info di;
    if (e && *e) snprintf(g_rosetta_root_buf, sizeof g_rosetta_root_buf, "%s", e);
    else if (dladdr((void *)oah_root_init, &di) && di.dli_fname) {
        char *sl;
        if (!realpath(di.dli_fname, g_rosetta_root_buf)) snprintf(g_rosetta_root_buf, sizeof g_rosetta_root_buf, "%s", di.dli_fname);
        if ((sl = strrchr(g_rosetta_root_buf, '/'))) *sl = 0;
    }
    g_rosetta_root_len = strlen(g_rosetta_root_buf);
    snprintf(g_dyld_root_env, sizeof g_dyld_root_env, "DYLD_ROOT_PATH=%s", g_rosetta_root_buf);
    {   /* Command-line tools (not inside an .app): our diagnostics must not mix with the tool's own
           stderr (Wireshark parses dumpcap's stderr; scripts capture tool output). They go to
           ~/Library/Logs/Rosettus/tools.log instead. Apps log to their own file through the launcher.
           OAH_VERBOSE=1 or OAH_TOOL_STDERR=1 keeps them on stderr. */
        const char *g = getenv("OAH_GUEST_EXE");
        struct stat es;
        /* only when stderr is a terminal or a pipe: launchers send an app's stderr to its log file.
           The log is opened on a high descriptor: Office 2004 reported "cannot load the shared
           libraries" when it found descriptor 3 taken. */
        /* Tools inside a bundle count too (Wireshark.app/Contents/Resources/bin/dumpcap-bin: our
           "[pathmap]" lines reached Wireshark as "Unknown message from dumpcap" and no interface was
           listed); only a bundle's own Contents/MacOS executables keep stderr. */
        /* stderr redirected to a file by the user (tool 2> errors.txt) counts too; only Rosettus's own
           log files (~/Library/Logs/Rosettus/..., where launchers send an app's output) keep them */
        int user_stderr = 0;
        if (!fstat(2, &es)) {
            char sp[1024];
            user_stderr = !S_ISREG(es.st_mode) ||
                          (fcntl(2, F_GETPATH, sp) != -1 && !strstr(sp, "/Library/Logs/Rosettus/"));
        }
        if (g && (!strstr(g, ".app/") || !strstr(g, ".app/Contents/MacOS/")) && !getenv("OAH_VERBOSE") && !getenv("OAH_TOOL_STDERR") && !getenv("OAH_AGENT") &&
            user_stderr) {
            const char *h = getenv("HOME"); char lp[1024]; FILE *f = 0; int fd, hfd;
            snprintf(lp, sizeof lp, "%s/Library/Logs/Rosettus/tools.log", h ? h : "/tmp");
            if ((fd = open(lp, O_WRONLY | O_APPEND | O_CREAT, 0644)) < 0) fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) {
                hfd = fcntl(fd, F_DUPFD, 240);
                if (hfd >= 0) { close(fd); fd = hfd; }
                fcntl(fd, F_SETFD, FD_CLOEXEC);
                f = fdopen(fd, "a");
            }
            if (f) {
                setvbuf(f, 0, _IOLBF, 0);
                fprintf(f, "=== %d %s\n", (int)getpid(), g);
                stderr = f;
            }
        }
    }
}
static unsigned long g_cb_depth;                 /* nested oah_callppc_n calls */
static pthread_t g_cb_owner;                     /* thread inside oah_callppc_n (under its lock) */
static volatile int g_cb_owner_depth;
static int oah_on_callback_stack(void) { return g_cb_owner_depth > 0 && pthread_equal(g_cb_owner, pthread_self()); }
static volatile unsigned int g_bcount[4096];     /* bridge calls per id (heartbeat shows the busiest) */
static const char *volatile g_last_cp_tag;       /* last oah_callppc_n target (fatal reports) */
static volatile unsigned int g_last_cp_fn;

/* Only paths requested by Rosetta itself (i.e. by guest PPC code) are redirected
   into the PPC root; Mojave's native frameworks in the same process (HIToolbox,
   CoreImage, OpenGL, ...) must keep seeing the real /System. */
static int g_redirect_native = -1;
static int oah_caller_is_guest(void *ra) {
    unsigned int a = (unsigned int)ra;
    if (g_redirect_native < 0) g_redirect_native = getenv("OAH_REDIRECT_ALL") ? 1 : 0;
    return g_redirect_native || (a >= 0xb8000000u && a < 0xb81df000u);
}
static void oah_gltest(const char *when);
static void oah_glwarm(void);
static int oah_guest_wants_gl(void);
static void oah_ensure_glwarm(void);
static const char *translate_path_core(const char *path, char *buf, size_t bufsz);
#define translate_path(p, b, n) translate_path_ra((p), (b), (n), __builtin_return_address(0))
static const char *translate_path_ra(const char *path, char *buf, size_t bufsz, void *ra) {
    const char *r;
    if (oah_caller_is_guest(ra)) return translate_path_core(path, buf, bufsz);
    r = translate_path_core(path, buf, bufsz);
    if (r != path && getenv("OAH_VERBOSE"))
        fprintf(stderr, "[oah_native_path] kept %s (caller %p)\n", path, ra);
    return path;
}
static const char *translate_path_core(const char *path, char *buf, size_t bufsz) {
    if (!path || path[0] != '/' || path[1] == '\0') return path;
    if (strncmp(path, g_rosetta_root, g_rosetta_root_len) == 0) return path;
    if (strncmp(path, "/tmp/", 5) == 0 || strncmp(path, "/private/tmp/", 13) == 0) return path;
    if (strncmp(path, "/Users/", 7) == 0 || strncmp(path, "/Volumes/", 9) == 0) return path;   /* user files are never in the guest tree */
    if (strncmp(path, "/dev/", 5) == 0) return path;
    snprintf(buf, bufsz, "%s%s", g_rosetta_root, path);
    if (access(buf, F_OK) == 0) {
        if (getenv("OAH_VERBOSE")) {
            fprintf(stderr, "[oah_redirect] %s -> %s\n", path, buf);
        }
        return buf;
    }
    return path;
}

static int is_ppc_commpage(unsigned int v) {
    /* 0xfffef000 page holds ba-stubs into the commpage (objc GC barriers, msgSend);
       the shim keeps a relocated copy at 0xffdef000, same -0x200000 delta. */
    return (v >= 0xffff3000 && v < 0xffffa000) || (v >= 0xfffef000 && v < 0xffff0000);
}

/* Rosetta region-lookup hooks: guest code may branch straight to the old
   commpage (e.g. "bla 0xfffeff00" = objc_msgSend). Rosetta looks the PC up
   in its region table before touching memory, so remap old commpage
   addresses [0xfffec000,0xfffff000) by -0x200000 at lookup time. */
extern void oah_trampA(void);
extern void oah_trampB(void);
/* Objective-C commpage entries (gcc -fobjc-direct-dispatch, iWork '09 and other apps built
   for 10.4/10.5): "bla 0xfffeff00" (objc_msgSend) and the GC write barriers at 0xfffefea0..ec0
   branch to Rosetta commpage descriptors that bind the routine by name in the guest. On Mojave
   that binding lands in junk (Pages and ddtest spun in a malloc block full of 0x899625b8).
   The first time the translator looks up an address in that range, bind them ourselves:
   each 16-byte entry becomes lis r12,hi / ori r12,lo / mtctr r12 / bctr to the guest symbol. */
volatile int g_objc_cp_bound;
static unsigned int oah_guest_sym(const char *lib, const char *sym);
/* Retarget guest "bla 0xfffefXXX" (objc commpage entries) to the relocated copy at
   0xffdefXXX.  The interpreter reaches 0xfffeXXXX through the region hooks, but once a
   commpage block is hot the translator compiles it by reading guest memory directly at
   0xfffeff00, which cannot be mapped on Mojave (user space ends below 0xffe00000), and the
   process spins.  bla can reach 0xffdeXXXX directly, so patch the callers instead.
   Images are scanned once each (keyed by header address); OAH_NO_BLAPATCH disables. */
#define OAH_MAX_PATCHED 1024
static unsigned int g_cp_low;   /* low page (< 32MB, reachable by bla) holding the objc commpage stubs */
/* Stubs at g_cp_low + {ea0,eb0,ec0,f00}: lis r12,ha(slot); lwz r12,lo(slot)(r12); mtctr r12; bctr.
   Slots (g_cp_low + 0xfe0 + 4*i) start out holding the architectural commpage address, so calls made
   before libobjc is bound still go through the commpage path (which binds); binding then stores the
   real libobjc entry points.  Only data changes after the stubs are first executed. */
static const unsigned int g_cp_offs[4] = { 0xea0, 0xeb0, 0xec0, 0xf00 };
static void oah_cp_low_setup(void) {
    unsigned int i;
    vm_address_t ra = 0xffdef000u; vm_size_t rs = 0; vm_region_basic_info_data_t bi;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT; mach_port_t o;
    if (g_cp_low || getenv("OAH_NO_CPLOW")) return;
    /* the relocated objc commpage page (mapped at init); the translator accepts code here */
    if (vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO, (vm_region_info_t)&bi, &cnt, &o) != KERN_SUCCESS || ra != 0xffdef000u) return;
    vm_protect(mach_task_self(), 0xffdef000u, 0x1000, 0, VM_PROT_ALL);
    g_cp_low = 0xffdef000u;
    for (i = 0; i < 4; i++) {
        unsigned int slot = g_cp_low + 0xfe0 + 4 * i, *c = (unsigned int *)(g_cp_low + g_cp_offs[i]);
        unsigned int orig = OSSwapBigToHostInt32(c[0]), tgt = 0xfffef000u + g_cp_offs[i];
        (void)orig;   /* first use goes through 0xfffefXXX: the lookup hook binds the slots, then relocates here */
        c[0] = OSSwapHostToBigInt32(0x3d800000u | (((slot >> 16) + ((slot & 0x8000) ? 1 : 0)) & 0xffff));
        c[1] = OSSwapHostToBigInt32(0x818c0000u | (slot & 0xffff));
        c[2] = OSSwapHostToBigInt32(0x7d8903a6u);
        c[3] = OSSwapHostToBigInt32(0x4e800420u);
        *(unsigned int *)slot = OSSwapHostToBigInt32(tgt);
    }
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[objc-cp] low stub page at 0x%08x\n", g_cp_low);
}
static void oah_cp_low_bind(unsigned int off, unsigned int target) {
    unsigned int i;
    if (!g_cp_low) return;
    for (i = 0; i < 4; i++) if (g_cp_offs[i] == off) *(volatile unsigned int *)(g_cp_low + 0xfe0 + 4 * i) = OSSwapHostToBigInt32(target);
}
static int oah_peek2(vm_address_t p, unsigned int *w);
static int oah_scan_region(vm_address_t a);
static unsigned int g_patched_img[OAH_MAX_PATCHED], g_npatched_img;
static unsigned int g_patched_tsz[OAH_MAX_PATCHED];   /* cached __TEXT size, 0 = not read yet */
static int oah_img_seen(unsigned int h) {
    unsigned int i;
    for (i = 0; i < g_npatched_img; i++) if (g_patched_img[i] == h) return 1;
    if (g_npatched_img < OAH_MAX_PATCHED) { g_patched_tsz[g_npatched_img] = 0; g_patched_img[g_npatched_img++] = h; }
    return 0;
}
/* ---- objc commpage call sites -> direct calls into libobjc ----
   Guest code compiled with direct dispatch (the default for Apple's ppc gcc; CoreFoundation alone
   has ~1700 sites) calls objc_msgSend as "bla 0xfffeff00" (and the assign helpers at fea0/eb0/ec0).
   On Snow Leopard libobjc copies objc_msgSend into that commpage slot; on Mojave the slot can only
   be reached relocated (0xffdeff00), and code executed there spins in the translator once it is hot.
   So every site is rewritten to a relative "bl" straight to libobjc's entry point, before the image's
   code runs: images are queued when dyld maps them (seen at the next guest syscall) and patched once
   libobjc's load address is known and the image's __TEXT is fully mapped.  libobjc's symbol offsets
   come from its file on disk.  Sites out of bl range fall back to the relocated commpage page.
   OAH_NO_BLAPATCH=1 disables; OAH_CPDEBUG=1 logs every image. */
static unsigned int g_objc_off[4];          /* strongCast, global, ivar, msgSend (commpage ea0, eb0, ec0, f00) */
static int g_objc_off_ok = -1;
static unsigned int g_objc_base;
static unsigned int g_cp_pend[512], g_cp_npend, g_cp_tries[512];
static int oah_objc_offsets(void) {
    /* the public _objc_assign_* are just "ba 0xfffefXXX" back into the commpage: bind to the _generic bodies */
    static const char *const names[4] = { "_objc_assign_strongCast_generic", "_objc_assign_global_generic", "_objc_assign_ivar_generic", "_objc_msgSend" };
    char path[1024]; Dl_info di; int fd, found = 0; unsigned int base = 0, k;
    struct stat sb; unsigned char *m;
    if (g_objc_off_ok >= 0) return g_objc_off_ok;
    g_objc_off_ok = 0;
    if (!dladdr((void *)oah_objc_offsets, &di) || !di.dli_fname) return 0;
    snprintf(path, sizeof path, "%s", di.dli_fname);
    if (strrchr(path, '/')) *strrchr(path, '/') = 0;
    strncat(path, "/usr/lib/libobjc.A.dylib", sizeof path - strlen(path) - 1);
    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &sb) || sb.st_size < 4096) { if (fd >= 0) close(fd); return 0; }
    m = mmap(0, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (m == MAP_FAILED) return 0;
    if (OSSwapBigToHostInt32(*(unsigned int *)m) == 0xcafebabeu) {
        unsigned int na = OSSwapBigToHostInt32(((unsigned int *)m)[1]), i;
        for (i = 0; i < na; i++) {
            unsigned int *fa = (unsigned int *)(m + 8 + 20 * i);
            if (OSSwapBigToHostInt32(fa[0]) == 18) { base = OSSwapBigToHostInt32(fa[2]); break; }
        }
    }
    {
        unsigned int *h = (unsigned int *)(m + base), ncmds, off = 28, i;
        if (OSSwapBigToHostInt32(h[0]) == 0xfeedfaceu && OSSwapBigToHostInt32(h[1]) == 18) {
            ncmds = OSSwapBigToHostInt32(h[4]);
            for (i = 0; i < ncmds; i++) {
                unsigned int *lc = (unsigned int *)(m + base + off), cmd = OSSwapBigToHostInt32(lc[0]);
                if (cmd == 2) {   /* LC_SYMTAB */
                    unsigned int symoff = OSSwapBigToHostInt32(lc[2]), nsyms = OSSwapBigToHostInt32(lc[3]);
                    unsigned int stroff = OSSwapBigToHostInt32(lc[4]), j;
                    for (j = 0; j < nsyms; j++) {
                        unsigned char *nl = m + base + symoff + 12 * j;
                        const char *nm = (const char *)(m + base + stroff + OSSwapBigToHostInt32(*(unsigned int *)nl));
                        if ((nl[4] & 0x0e) != 0x0e) continue;   /* N_SECT */
                        for (k = 0; k < 4; k++) if (!g_objc_off[k] && !strcmp(nm, names[k])) { g_objc_off[k] = OSSwapBigToHostInt32(*(unsigned int *)(nl + 8)); found++; }
                    }
                }
                off += OSSwapBigToHostInt32(lc[1]);
            }
        }
    }
    munmap(m, sb.st_size);
    g_objc_off_ok = found == 4;
    if (getenv("OAH_CPDEBUG")) fprintf(stderr, "[objc-cp] libobjc offsets %x %x %x %x (%s)\n", g_objc_off[0], g_objc_off[1], g_objc_off[2], g_objc_off[3], path);
    return g_objc_off_ok;
}
/* text range of a mapped image: returns bytes of __TEXT holding sections (0 if not parsable) */
static unsigned int oah_img_text_used(unsigned int hdr, int *is_objc) {
    unsigned int hb[8], ncmds, off = 28, i, used = 0, tvm = 0;
    if (is_objc) *is_objc = 0;
    if (!oah_peek2(hdr, hb) || hb[0] != OSSwapHostToBigInt32(0xfeedfaceu) || hb[1] != OSSwapHostToBigInt32(18u)) return 0;
    ncmds = OSSwapBigToHostInt32(hb[4]);
    for (i = 0; i < ncmds && off < 0x4000; i++) {
        unsigned int lw[20], cmd, csz;
        vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), hdr + off, sizeof lw, (vm_address_t)lw, &got) != KERN_SUCCESS) return 0;
        cmd = OSSwapBigToHostInt32(lw[0]); csz = OSSwapBigToHostInt32(lw[1]);
        if (cmd == 1 && !strncmp((const char *)(lw + 2), "__TEXT", 16)) {
            unsigned int ns = OSSwapBigToHostInt32(lw[12]), k;
            tvm = OSSwapBigToHostInt32(lw[6]);
            for (k = 0; k < ns && k < 64; k++) {
                unsigned int sw[17];
                if (vm_read_overwrite(mach_task_self(), hdr + off + 56 + 68 * k, sizeof sw, (vm_address_t)sw, &got) != KERN_SUCCESS) return 0;
                if (OSSwapBigToHostInt32(sw[8]) + OSSwapBigToHostInt32(sw[9]) - tvm > used) used = OSSwapBigToHostInt32(sw[8]) + OSSwapBigToHostInt32(sw[9]) - tvm;
            }
        } else if (cmd == 0xd && is_objc) {   /* LC_ID_DYLIB */
            char nm[128]; unsigned int no = OSSwapBigToHostInt32(lw[2]);
            if (vm_read_overwrite(mach_task_self(), hdr + off + no, sizeof nm - 1, (vm_address_t)nm, &got) == KERN_SUCCESS) {
                nm[sizeof nm - 1] = 0;
                if (strstr(nm, "/libobjc.A.dylib")) *is_objc = 1;
            }
        }
        if (csz < 8) break;
        off += csz;
    }
    return used;
}
static int oah_range_readable(unsigned int a, unsigned int e) {
    while (a < e) {
        vm_address_t ra = a; vm_size_t rs = 0; vm_region_basic_info_data_t bi;
        mach_msg_type_number_t c = VM_REGION_BASIC_INFO_COUNT; mach_port_t o;
        if (vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO, (vm_region_info_t)&bi, &c, &o) != KERN_SUCCESS ||
            ra > a || !(bi.protection & VM_PROT_READ)) return 0;
        a = ra + rs;
    }
    return 1;
}
/* returns sites patched, or -1 if the image is not ready yet */
static int oah_patch_image_bla(unsigned int hdr) {
    unsigned int used = oah_img_text_used(hdr, 0), a, e, pg = 0, n = 0;
    vm_prot_t pgprot = VM_PROT_READ | VM_PROT_EXECUTE;
    if (!used || used > 0x4000000) return 0;
    e = hdr + used;
    if (!oah_range_readable(hdr, e)) return -1;
    for (a = hdr; a < e; a += 4) {
        unsigned int w = OSSwapBigToHostInt32(*(volatile unsigned int *)a), idx, tgt;
        if ((a & 0xfff) == 0 && pg) { vm_protect(mach_task_self(), pg, 0x1000, 0, pgprot); pg = 0; }
        /* b(l)a 0xfffefXXX, or 0xffdefXXX in guest-tree libraries pre-patched on disk (*.precp backups) */
        {
            unsigned int m = w & 0xffdffffeu;
            if (m == 0x4bdeff02u) idx = 3; else if (m == 0x4bdefea2u) idx = 0;
            else if (m == 0x4bdefeb2u) idx = 1; else if (m == 0x4bdefec2u) idx = 2; else continue;
        }
        if (!pg) {
            vm_address_t ra = a; vm_size_t rs = 0; vm_region_basic_info_data_t bi;
            mach_msg_type_number_t c = VM_REGION_BASIC_INFO_COUNT; mach_port_t o;
            if (vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO, (vm_region_info_t)&bi, &c, &o) == KERN_SUCCESS) pgprot = bi.protection;
            pg = a & ~0xfffu;
            vm_protect(mach_task_self(), pg, 0x1000, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
        }
        tgt = g_objc_base + g_objc_off[idx];
        if (tgt - a + 0x2000000u < 0x4000000u) w = 0x48000000u | ((tgt - a) & 0x03fffffcu) | (w & 1);   /* b(l) libobjc entry */
        else w = 0x48000002u | ((0xffdef000u + g_cp_offs[idx]) & 0x03fffffcu) | (w & 1);                /* b(l)a relocated commpage */
        *(volatile unsigned int *)a = OSSwapHostToBigInt32(w);
        n++;
    }
    if (pg) vm_protect(mach_task_self(), pg, 0x1000, 0, pgprot);
    return (int)n;
}
static int oah_img_seen_only(unsigned int h);
static void oah_cp_queue(unsigned int hdr) {
    unsigned int i;
    if (oah_img_seen_only(hdr)) return;
    for (i = 0; i < g_cp_npend; i++) if (g_cp_pend[i] == hdr) return;
    if (g_cp_npend < 512) { g_cp_tries[g_cp_npend] = 0; g_cp_pend[g_cp_npend++] = hdr; }
}
static void oah_gt_image(unsigned int hdr);
static void oah_cp_process(void) {
    unsigned int i, j;
    if (g_cp_npend > 512) g_cp_npend = 0;   /* never trust a corrupted count */
    if (!g_cp_npend || !oah_objc_offsets()) return;
    if (!g_objc_base) {
        for (i = 0; i < g_cp_npend; i++) {
            int isobjc = 0;
            if (oah_img_text_used(g_cp_pend[i], &isobjc) && isobjc) {
                g_objc_base = g_cp_pend[i];
                for (j = 0; j < 4; j++) oah_cp_low_bind(g_cp_offs[j], g_objc_base + g_objc_off[j]);
                g_objc_cp_bound = 1;
                if (getenv("OAH_VERBOSE")) fprintf(stderr, "[objc-cp] libobjc at 0x%08x, objc_msgSend 0x%08x\n", g_objc_base, g_objc_base + g_objc_off[3]);
                break;
            }
        }
        if (!g_objc_base) return;
    }
    for (i = 0; i < g_cp_npend; ) {
        int k = oah_patch_image_bla(g_cp_pend[i]);
        if (k < 0 && ++g_cp_tries[i] > 64) {
            /* never became readable: give up, or every later mmap re-reads it (Numbers spent
               two threads at 100% here) */
            if (getenv("OAH_CPDEBUG")) fprintf(stderr, "[objc-cp] image %08x never ready, dropped\n", g_cp_pend[i]);
            k = 0;
        }
        if (k < 0) { i++; continue; }
        if (getenv("OAH_CPDEBUG") || (k && getenv("OAH_VERBOSE"))) fprintf(stderr, "[objc-cp] image %08x: %d call sites -> libobjc\n", g_cp_pend[i], k);
        oah_gt_image(g_cp_pend[i]);
        {   unsigned int slot = g_npatched_img < OAH_MAX_PATCHED ? g_npatched_img++ : OAH_MAX_PATCHED - 1;
            g_patched_img[slot] = g_cp_pend[i]; g_patched_tsz[slot] = 0; }
        --g_cp_npend; g_cp_pend[i] = g_cp_pend[g_cp_npend]; g_cp_tries[i] = g_cp_tries[g_cp_npend];
    }
}
static int oah_img_seen_only(unsigned int h) { unsigned int i; for (i = 0; i < g_npatched_img; i++) if (g_patched_img[i] == h) return 1; return 0; }
/* Called before every guest BSD syscall (from the int $0x80 hook). */
static unsigned int g_cp_pend_a;
static int g_cp_first = 1, g_cp_rescan;
/* first page of every region: queue PowerPC images not seen yet (cheap: one peek per region) */
static void oah_cp_quick_scan(void) {
    vm_address_t addr = 0x1000;
    for (;;) {
        vm_size_t size = 0; vm_region_basic_info_data_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT; mach_port_t obj = MACH_PORT_NULL;
        unsigned int hb[8], ft;
        if (vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) break;
        if (addr >= 0x90000000u) break;
        if ((info.protection & VM_PROT_READ) && oah_peek2(addr, hb) && hb[0] == OSSwapHostToBigInt32(0xfeedfaceu) &&
            hb[1] == OSSwapHostToBigInt32(18u)) {
            ft = OSSwapBigToHostInt32(hb[3]);
            if ((ft == 2 || ft == 6 || ft == 8) && !oah_img_seen_only((unsigned int)addr)) oah_cp_queue((unsigned int)addr);
        }
        addr += size;
    }
}
void oah_patch_cp_callers(void);
static void oah_cp_sc_note_locked(unsigned int num, unsigned int *a);
/* Every guest thread's syscalls come through here: the pending/patched lists are shared, and
   unlocked swap-removes from two threads underflowed g_cp_npend (Numbers launched by an Apple
   event then walked garbage "images" at 200% CPU). */
void oah_cp_sc_note(unsigned int num, unsigned int *a) {
    static int off = -1;
    static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
    if (off < 0) off = getenv("OAH_NO_BLAPATCH") != 0;
    if (off) return;
    pthread_mutex_lock(&mx);
    oah_cp_sc_note_locked(num, a);
    pthread_mutex_unlock(&mx);
}
static void oah_cp_sc_note_locked(unsigned int num, unsigned int *a) {
    static int dirty;
    if (g_cp_first) { g_cp_first = 0; oah_patch_cp_callers(); }
    if (g_cp_pend_a) {
        unsigned int hb[8];
        if (oah_peek2(g_cp_pend_a, hb) && hb[0] == OSSwapHostToBigInt32(0xfeedfaceu) && hb[1] == OSSwapHostToBigInt32(18u) &&
            !oah_img_seen_only(g_cp_pend_a)) oah_cp_queue(g_cp_pend_a);
        g_cp_pend_a = 0;
    }
    if (g_cp_rescan) { g_cp_rescan = 0; oah_cp_quick_scan(); dirty = 1; }
    /* only re-examine the queue after a new mapping: images become ready as dyld maps their segments */
    if (dirty && g_cp_npend) { dirty = 0; oah_cp_process(); }
    if ((num == 197 && a[0] && a[1]) || (num == 73 && a[1])) {
        /* a fixed mmap or munmap over a patched image replaces its pages with the file's
           (Core Image plug-ins are mapped twice at the same address): patch it again */
        unsigned int i2;
        /* The __TEXT size is read once per image and cached: re-reading every patched image's
           load commands on every mmap/munmap kept two Numbers threads at 100% each. */
        for (i2 = 0; i2 < g_npatched_img; ) {
            int hit = g_patched_img[i2] - a[0] < a[1];
            if (!hit && a[0] > g_patched_img[i2]) {
                if (!g_patched_tsz[i2]) { g_patched_tsz[i2] = oah_img_text_used(g_patched_img[i2], 0); if (!g_patched_tsz[i2]) g_patched_tsz[i2] = 1; }
                hit = a[0] < g_patched_img[i2] + g_patched_tsz[i2];
            }
            if (hit) {
                --g_npatched_img;
                g_patched_img[i2] = g_patched_img[g_npatched_img]; g_patched_tsz[i2] = g_patched_tsz[g_npatched_img];
                if (num == 197) g_cp_rescan = 1;
            } else i2++;
        }
    }
    if (num == 197 && a[1]) {
        dirty = 1;
        if ((a[2] & 4) && (int)a[4] >= 0) {
            if (a[0]) g_cp_pend_a = a[0];
            else g_cp_rescan = 1;      /* placed by the kernel (bundles loaded later, e.g. Core Image plug-ins): find it next time */
        }
    }
}
/* queue every PPC image already in memory (the main executable, dyld, anything mapped before us) */
void oah_patch_cp_callers(void) {
    vm_address_t addr = 0x1000;
    if (getenv("OAH_NO_BLAPATCH")) return;
    for (;;) {
        vm_size_t size = 0; vm_region_basic_info_data_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT; mach_port_t obj = MACH_PORT_NULL; vm_address_t p;
        if (vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) break;
        if (addr >= 0x90000000u) break;
        if (info.protection & VM_PROT_READ) {
            if (oah_scan_region(addr)) for (p = addr; p < addr + size; p += 4096) {
                unsigned int hb[8], ft;
                if (!oah_peek2(p, hb)) continue;
                if (hb[0] != OSSwapHostToBigInt32(0xfeedfaceu) || hb[1] != OSSwapHostToBigInt32(18u)) continue;
                ft = OSSwapBigToHostInt32(hb[3]);
                if (ft != 2 && ft != 6 && ft != 8) continue;
                if (!oah_img_seen_only((unsigned int)p)) oah_cp_queue((unsigned int)p);
            }
        }
        addr += size;
    }
    oah_cp_process();
}

void oah_bind_objc_commpage(void) {
    static const struct { unsigned int off; const char *sym; unsigned int desc; } ent[] = {
        { 0xf00, "_objc_msgSend", 0x970 }, { 0xec0, "_objc_assign_ivar_generic", 0x9cc },
        { 0xeb0, "_objc_assign_global_generic", 0xa3c }, { 0xea0, "_objc_assign_strongCast_generic", 0xaac },
    };
    unsigned int i, n = 0;
    if (g_objc_cp_bound) return;
    g_objc_cp_bound = 1;
    if (getenv("OAH_NO_OBJCCP")) return;
    if (!getenv("OAH_NO_CPMIRROR")) {
        /* Mirror the relocated objc commpage page at its architectural address, so
           paths that read 0xfffeff00 directly (hot-block retranslation) see real code. */
        vm_address_t ma = 0xfffef000u, ra2 = ma; vm_size_t sz2 = 0; vm_region_basic_info_data_t in2;
        mach_msg_type_number_t c2 = VM_REGION_BASIC_INFO_COUNT; mach_port_t o2;
        int mapped = vm_region(mach_task_self(), &ra2, &sz2, VM_REGION_BASIC_INFO, (vm_region_info_t)&in2, &c2, &o2) == KERN_SUCCESS && ra2 <= ma;
        if (!mapped) {
            kern_return_t kr = vm_allocate(mach_task_self(), &ma, 0x1000, VM_FLAGS_FIXED);
            if (kr == KERN_SUCCESS) memcpy((void *)0xfffef000u, (void *)0xffdef000u, 0x1000);
            if (getenv("OAH_VERBOSE")) fprintf(stderr, "[objc-cp] mirror 0xfffef000: kr=%d\n", kr);
        }
    }
    for (i = 0; i < sizeof ent / sizeof ent[0]; i++) {
        unsigned int t = oah_guest_sym("libobjc", ent[i].sym), code[4], pg;
        if (!t) continue;
        code[0] = OSSwapHostToBigInt32(0x3d800000u | (((t >> 16) + ((t & 0x8000) ? 1 : 0)) & 0xffff)); /* lis r12,ha */
        code[1] = OSSwapHostToBigInt32(0x398c0000u | (t & 0xffff));                                       /* addi r12,r12,lo */
        code[2] = OSSwapHostToBigInt32(0x7d8903a6u);                                                       /* mtctr r12 */
        code[3] = OSSwapHostToBigInt32(0x4e800420u);                                                       /* bctr */
        oah_cp_low_setup();
        oah_cp_low_bind(ent[i].off, t);
        if (g_cp_low) { n++; if (getenv("OAH_VERBOSE")) fprintf(stderr, "[objc-cp] %s -> 0x%08x (slot)\n", ent[i].sym, t); continue; }
        for (pg = 0; pg < 3; pg++) {
            unsigned int a = (pg == 2 ? 0xffdf3000u + ent[i].desc : (pg ? 0xfffef000u : 0xffdef000u) + ent[i].off);
            if (pg == 2 && getenv("OAH_NO_OBJCDESC")) continue;
            vm_size_t sz = 0; vm_address_t ra = a & ~0xfffu; vm_region_basic_info_data_t info;
            mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT; mach_port_t obj;
            if (vm_region(mach_task_self(), &ra, &sz, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS ||
                ra > a) continue;
            vm_protect(mach_task_self(), a & ~0xfffu, 0x1000, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
            memcpy((void *)a, code, sizeof code);
        }
        n++;
        if (getenv("OAH_VERBOSE")) fprintf(stderr, "[objc-cp] %s -> 0x%08x at +0x%x\n", ent[i].sym, t, ent[i].off);
    }
    if (!n) fprintf(stderr, "[objc-cp] libobjc not found; commpage objc entries left alone\n");
}
__asm__(
    ".text\n"
    ".globl _oah_trampA\n"
    "_oah_trampA:\n"
    "    movl 0x0c(%ebp),%esi\n"
    "    cmpl $0xffdefe00,%esi\n"
    "    jb 4f\n"
    "    cmpl $0xffdf0000,%esi\n"
    "    jb 64f\n"
    "    cmpl $0xfffefe00,%esi\n"
    "    jb 4f\n"
    "    cmpl $0xffff0000,%esi\n"
    "    jae 4f\n"
    "64:\n"
    "    pushal\n"
    "    movl %esp,%eax\n"
    "    andl $-16,%esp\n"
    "    subl $12,%esp\n"
    "    pushl %eax\n"
    "    call _oah_bind_objc_commpage\n"
    "    movl (%esp),%esp\n"
    "    popal\n"
    "4:\n"
    "    cmpl $0xfffec000,%esi\n"
    "    jb 1f\n"
    "    cmpl $0xfffff000,%esi\n"
    "    jae 1f\n"
    "    subl $0x200000,%esi\n"
    "1:  movl 0x84(%edi),%edx\n"
    "    ret\n"
    ".globl _oah_trampB\n"
    "_oah_trampB:\n"
    "    movl 0x0c(%ebp),%edi\n"
    "    cmpl $0xffdefe00,%edi\n"
    "    jb 5f\n"
    "    cmpl $0xffdf0000,%edi\n"
    "    jb 65f\n"
    "    cmpl $0xfffefe00,%edi\n"
    "    jb 5f\n"
    "    cmpl $0xffff0000,%edi\n"
    "    jae 5f\n"
    "65:\n"
    "    pushal\n"
    "    movl %esp,%eax\n"
    "    andl $-16,%esp\n"
    "    subl $12,%esp\n"
    "    pushl %eax\n"
    "    call _oah_bind_objc_commpage\n"
    "    movl (%esp),%esp\n"
    "    popal\n"
    "5:\n"
    "    cmpl $0xfffec000,%edi\n"
    "    jb 2f\n"
    "    cmpl $0xfffff000,%edi\n"
    "    jae 2f\n"
    "    subl $0x200000,%edi\n"
    "2:  movl 0x08(%ebp),%eax\n"
    "    ret\n"
    /* C: interpreter lwz.  b80c5d0a: testl %edx,%edx ; jne b80c5d5e (rA != 0: leal (%ecx,%eax),%edi ; jmp
       b80c5d10) ; movl %eax,%edi (rA == 0) ; b80c5d10: movl (%edi),%esi.  Guest "lwz rD,-0x7fe0(0)" reads the
       old commpage (cpu capabilities at 0xffff8020).  The hook replaces the 6 bytes before the load, computes
       the address for both paths and relocates only commpage addresses; the load stays at its own address so
       real guest faults still reach the translator (doing the load in here turned them into host crashes). */
    ".globl _oah_trampC\n"
    "_oah_trampC:\n"
    "    testl %edx,%edx\n"
    "    jne 1f\n"
    "    movl %eax,%edi\n"
    "    jmp 2f\n"
    "1:  leal (%ecx,%eax),%edi\n"
    "2:  cmpl $0xfffec000,%edi\n"
    "    jb 3f\n"
    "    cmpl $0xfffff000,%edi\n"
    "    jae 3f\n"
    "    subl $0x200000,%edi\n"
    "3:  ret\n"
);
extern void oah_trampC(void);
/* E: interpreter lbz at b80c597e (movzbl (%edx),%edx).  A guest "lbz rD,0x24(rX)" with rX = 0xffff8000
   (old commpage) faulted there; on a secondary thread the translator's exception handler turned that into
   a fatal guest SIGTRAP before our SIGSEGV fixer could relocate it.  The hook covers the 8 bytes before the
   load (b80c5976: movl (%edi),%eax ; shrl $21,%eax ; andl $31,%eax) and only relocates commpage
   addresses, so the load itself stays at its own address and real guest faults are handled as before. */
__asm__(
    ".text\n"
    ".globl _oah_trampE\n"
    "_oah_trampE:\n"
    "    movl (%edi),%eax\n"
    "    shrl $21,%eax\n"
    "    andl $31,%eax\n"
    "    cmpl $0xfffec000,%edx\n"
    "    jb 1f\n"
    "    cmpl $0xfffff000,%edx\n"
    "    jae 1f\n"
    "    subl $0x200000,%edx\n"
    "1:  ret\n"
);
extern void oah_trampE(void);
/* D: dispatcher ring buffer (OAH_DRING=1). b8070a19: movl 0x20(%edx),%eax ; movl 0x4(%esp),%ecx */
unsigned int g_dring[2 + 256 * 3] = {1};
__asm__(
    ".text\n"
    ".globl _oah_trampD\n"
    "_oah_trampD:\n"
    "    movl 0x20(%edx),%eax\n"
    "    movl 0x8(%esp),%ecx\n"
    "    pushl %ebx\n"
    "    pushl %esi\n"
    "    call 1f\n"
    "1:  popl %ebx\n"
    "    leal _g_dring-1b(%ebx),%ebx\n"
    "    cmpl %eax,3076(%ebx)\n"
    "    je 2f\n"
    "    movl %eax,3076(%ebx)\n"
    "    movl (%ebx),%esi\n"
    "    decl %esi\n"
    "    incl (%ebx)\n"
    "    andl $255,%esi\n"
    "    leal (%esi,%esi,2),%esi\n"
    "    movl %eax,4(%ebx,%esi,4)\n"
    "    movl %edx,8(%ebx,%esi,4)\n"
    "    movl 16(%esp),%eax\n"
    "    movl %eax,12(%ebx,%esi,4)\n"
    "2:  movl 0x20(%edx),%eax\n"
    "    popl %esi\n"
    "    popl %ebx\n"
    "    ret\n"
);
extern void oah_trampD(void);
void oah_dring_dump(void) {
    unsigned int n = g_dring[0] - 1, k, i;   /* starts at 1 so the array lands in __data, not common */
    if (!n || n == 0xffffffffu) return;
    fprintf(stderr, "[dring] %u dispatches; last %u (pc block caller):\n", n, n < 64 ? n : 64);
    for (k = (n < 64 ? 0 : n - 64); k < n; k++) {
        i = (k & 255) * 3;
        fprintf(stderr, "[dring]  %08x %08x %08x\n", g_dring[1 + i], g_dring[2 + i], g_dring[3 + i]);
    }
    for (k = (n < 4 ? 0 : n - 4); k < n; k++) {   /* raw block words, to identify the guest-pc field */
        unsigned int b = g_dring[2 + (k & 255) * 3], w[12], j;
        vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), b, sizeof w, (vm_address_t)w, &got) != KERN_SUCCESS) continue;
        fprintf(stderr, "[dring] block %08x:", b);
        for (j = 0; j < 12; j++) fprintf(stderr, " %08x", w[j]);
        fprintf(stderr, "\n");
        {
            unsigned int dsc[16];
            if (vm_read_overwrite(mach_task_self(), w[9], sizeof dsc, (vm_address_t)dsc, &got) == KERN_SUCCESS) {
                fprintf(stderr, "[dring]   desc %08x:", w[9]);
                for (j = 0; j < 16; j++) fprintf(stderr, " %08x", dsc[j]);
                fprintf(stderr, "\n");
            }
        }
    }
}

static void oah_hook_call(unsigned int addr, const unsigned char *expect, int len, void (*tramp)(void)) {
    unsigned char *p = (unsigned char *)addr;
    int i;
    if (memcmp(p, expect, len) != 0) {
        fprintf(stderr, "[oah_hook] 0x%08x bytes differ, not hooked\n", addr);
        return;
    }
    vm_address_t pg = addr & ~0xfffU;
    if (vm_protect(mach_task_self(), pg, 0x2000, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
        fprintf(stderr, "[oah_hook] vm_protect RW failed at 0x%08x\n", addr);
        return;
    }
    int rel = (int)((unsigned int)tramp - (addr + 5));
    p[0] = 0xe8;
    memcpy(p + 1, &rel, 4);
    for (i = 5; i < len; i++) p[i] = 0x90;
    vm_protect(mach_task_self(), pg, 0x2000, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[oah_hook] hooked 0x%08x -> %p\n", addr, (void *)tramp);
}

static void oah_install_region_hooks(void) {
    /* A: b81506da  movl 0x0c(%ebp),%esi ; movl 0x84(%edi),%edx */
    static const unsigned char ea[9] = {0x8b,0x75,0x0c, 0x8b,0x97,0x84,0x00,0x00,0x00};
    /* B: b81507b1  movl 0x0c(%ebp),%edi ; movl 0x08(%ebp),%eax */
    static const unsigned char eb[6] = {0x8b,0x7d,0x0c, 0x8b,0x45,0x08};
    oah_hook_call(0xb81506da, ea, 9, oah_trampA);
    oah_hook_call(0xb81507b1, eb, 6, oah_trampB);
    /* C: b80c5d0a  testl %edx,%edx ; jne +0x50 ; movl %eax,%edi  (before the lwz load at b80c5d10) */
    static const unsigned char ec[6] = {0x85,0xd2, 0x75,0x50, 0x89,0xc7};
    oah_hook_call(0xb80c5d0a, ec, 6, oah_trampC);
    {
        static const unsigned char ee[8] = {0x8b,0x07, 0xc1,0xe8,0x15, 0x83,0xe0,0x1f};
        oah_hook_call(0xb80c5976, ee, 8, oah_trampE);
    }
    if (getenv("OAH_DRING")) {
        static const unsigned char ed[7] = {0x8b,0x42,0x20, 0x8b,0x4c,0x24,0x04};
        oah_hook_call(0xb8070a19, ed, 7, oah_trampD);
    }
}

static void usr1_handler(int sig, siginfo_t *info, void *uap) {
    ucontext_t *uc = (ucontext_t *)uap;
    unsigned int r[7];
    int i;
    r[0] = uc->uc_mcontext->__ss.__eip; r[1] = uc->uc_mcontext->__ss.__eax;
    r[2] = uc->uc_mcontext->__ss.__ebx; r[3] = uc->uc_mcontext->__ss.__ecx;
    r[4] = uc->uc_mcontext->__ss.__edx; r[5] = uc->uc_mcontext->__ss.__esi;
    r[6] = uc->uc_mcontext->__ss.__ebp;
    fprintf(stderr, "[usr1] eip=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x ebp=%08x edi=%08x\n",
            r[0], r[1], r[2], r[3], r[4], r[5], r[6], uc->uc_mcontext->__ss.__edi);
    {
        vm_address_t ga = 0x00102000; vm_size_t gs = 0; vm_region_basic_info_data_t bi;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT; mach_port_t on;
        if (vm_region(mach_task_self(), &ga, &gs, VM_REGION_BASIC_INFO, (vm_region_info_t)&bi, &cnt, &on) == KERN_SUCCESS
            && ga <= 0x00102a40) {
            unsigned int *w = (unsigned int *)0x00102a40; int k;
            fprintf(stderr, "[usr1] guest@00102a40:");
            for (k = 0; k < 0x60; k++) fprintf(stderr, "%s%08x", (k % 8) ? " " : "\n[usr1]   ", w[k]);
            fprintf(stderr, "\n");
        }
    }
    {
        unsigned int c = r[3];
        if (c > 0x10000 && c < 0xb0000000U) {
            unsigned int *w = (unsigned int *)c; int k;
            fprintf(stderr, "[usr1] guestpc(ecx+74)=%08x  ecx[0..]:", w[0x74/4]);
            for (k = 0; k < 0x40; k++) fprintf(stderr, "%s%08x", (k % 8) ? " " : "\n[usr1]   ", w[k]);
            fprintf(stderr, "\n");
        }
    }
    for (i = 2; i < 7; i++) {
        unsigned int v = r[i];
        if (v >= 0x00100000 && v < 0x00200000) {
            unsigned int *w = (unsigned int *)v; int k;
            fprintf(stderr, "[usr1]  *%08x:", v);
            for (k = -2; k < 14; k++) fprintf(stderr, " %08x", w[k]);
            fprintf(stderr, "\n");
        }
    }
}

/* ---- unbridged-call telemetry ----
   oah_tel(cat, name) counts a call that reached a stub, an "ignored"/"unknown" path, or anything else
   that is a gap in the bridging.  The table is written (only when it changed) every 20 s by the hang
   watchdog, at guest exit and with every crash report, to ~/Library/Logs/Rosettus/Telemetry/<app>.<pid>.tsv
   ("cat<TAB>name<TAB>count<TAB>first-seen-epoch").  t/telemetry.sh merges and ranks them across apps.
   OAH_NO_TELEMETRY=1 disables. */
#define OAH_TEL_N 1024
static struct { char k[80]; unsigned long n; long t0; } g_tel[OAH_TEL_N];
static int g_tel_n, g_tel_dirty, g_tel_off = -1;
static pthread_mutex_t g_tel_mx = PTHREAD_MUTEX_INITIALIZER;
static void oah_tel(const char *cat, const char *name) {
    char k[80]; int i;
    if (g_tel_off < 0) g_tel_off = getenv("OAH_NO_TELEMETRY") ? 1 : 0;
    if (g_tel_off) return;
    snprintf(k, sizeof k, "%s\t%s", cat, name ? name : "?");
    if (pthread_mutex_trylock(&g_tel_mx)) return;   /* never block (may run in a signal path) */
    for (i = 0; i < g_tel_n; i++) if (!strcmp(g_tel[i].k, k)) break;
    if (i == g_tel_n && g_tel_n < OAH_TEL_N) { memcpy(g_tel[i].k, k, sizeof k); g_tel[i].n = 0; g_tel[i].t0 = time(0); g_tel_n++; }
    if (i < g_tel_n) { g_tel[i].n++; g_tel_dirty = 1; }
    pthread_mutex_unlock(&g_tel_mx);
}
static void oah_tel_save(void) {
    char dir[1024], p[1300], tp[1310], app[128];
    const char *home = oah_env_home(), *exe = oah_env_exe();
    FILE *f; int i;
    if (!g_tel_dirty || g_tel_off == 1 || !home) return;
    if (pthread_mutex_trylock(&g_tel_mx)) return;
    g_tel_dirty = 0;
    snprintf(dir, sizeof dir, "%s/Library/Logs/Rosettus", home); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/Library/Logs/Rosettus/Telemetry", home); mkdir(dir, 0755);
    snprintf(app, sizeof app, "%s", exe && strrchr(exe, '/') ? strrchr(exe, '/') + 1 : getprogname());
    { char *c; for (c = app; *c; c++) if (*c == ' ' || *c == '/' || *c == '\t') *c = '_'; }
    snprintf(p, sizeof p, "%s/%s.%d.tsv", dir, app, getpid());
    snprintf(tp, sizeof tp, "%s.tmp", p);
    if ((f = fopen(tp, "w"))) {
        for (i = 0; i < g_tel_n; i++) fprintf(f, "%s\t%lu\t%ld\n", g_tel[i].k, g_tel[i].n, g_tel[i].t0);
        fclose(f); rename(tp, p);
    }
    pthread_mutex_unlock(&g_tel_mx);
}

/* ---- crash / hang reports ----
   Every crash, fatal trap and hang dump is also saved on its own:
   ~/Library/Logs/Rosettus/Reports/<app>-<yyyymmdd-hhmmss>-<kind>.txt (the slice of the app's log
   written by the dump, under a short header), with one line per report in Reports/index.txt.
   oah_report_mark() at the start of a dump notes where the log ends; oah_report_flush() at its end
   (or right before the process exits) copies the slice. OAH_NO_REPORTS=1 disables. */
#include <pwd.h>
static off_t g_rep_off = -1;
static char g_rep_kind[24];
static char g_rep_stamp[32];   /* taken at the mark: localtime can block inside a crash */
static volatile int g_rep_crash;      /* inside the crash handler (other threads may be stopped holding locks) */
static off_t g_rep_crash_off = -1;
static void oah_report_mark(const char *kind) {
    time_t t;
    if (g_rep_off >= 0 || getenv("OAH_NO_REPORTS")) return;
    fflush(stderr);
    g_rep_off = lseek(2, 0, SEEK_END);
    snprintf(g_rep_kind, sizeof g_rep_kind, "%s", kind);
    t = time(0); strftime(g_rep_stamp, sizeof g_rep_stamp, "%Y%m%d-%H%M%S", localtime(&t));
}
static void oah_report_flush(void) {
    char logp[1024], dir[1024], rp[1300], app[128], stamp[32], first[200] = "";
    const char *home = oah_env_home(), *exe = oah_env_exe();
    off_t end, off = g_rep_off;
    int in, out;
    time_t t;
    if (!g_rep_crash) oah_tel_save();   /* (locks, allocation: not inside a crash) */
    if (off < 0) return;
    g_rep_off = -1;
    fflush(stderr);
    if (!home || !*home) { struct passwd *pw = getpwuid(getuid()); home = pw ? pw->pw_dir : 0; }
    if (!home || fcntl(2, F_GETPATH, logp) == -1) { fprintf(stderr, "[report] no home/log path (%s)\n", strerror(errno)); return; }
    end = lseek(2, 0, SEEK_END);
    if (end <= off) { fprintf(stderr, "[report] empty slice %lld..%lld\n", (long long)off, (long long)end); return; }
    if (end - off > 4 << 20) off = end - (4 << 20);
    {   /* include up to 16 KB of log before the mark (what led up to it), from a line start */
        static char pb[16384]; off_t c0 = off > (off_t)sizeof pb ? off - (off_t)sizeof pb : 0; ssize_t pn;
        int pf = open(logp, O_RDONLY);
        if (pf >= 0 && off > 0 && (pn = pread(pf, pb, off - c0, c0)) > 0) {
            char *nl = c0 ? memchr(pb, '\n', pn) : pb - 1;
            if (nl) off = c0 + (nl + 1 - pb);
        }
        if (pf >= 0) close(pf);
    }
    snprintf(dir, sizeof dir, "%s/Library/Logs/Rosettus/Reports", home);
    mkdir(dir, 0755);
    snprintf(app, sizeof app, "%s", exe && strrchr(exe, '/') ? strrchr(exe, '/') + 1 : getprogname());
    { char *c; for (c = app; *c; c++) if (*c == ' ' || *c == '/') *c = '_'; }
    if (g_rep_stamp[0]) snprintf(stamp, sizeof stamp, "%s", g_rep_stamp);
    else { t = time(0); strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&t)); }
    snprintf(rp, sizeof rp, "%s/%s-%s-%s.txt", dir, app, stamp, g_rep_kind);
    if ((in = open(logp, O_RDONLY)) < 0) { fprintf(stderr, "[report] open %s: %s\n", logp, strerror(errno)); return; }
    if ((out = open(rp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) fprintf(stderr, "[report] create %s: %s\n", rp, strerror(errno));
    else {
        static char hdr[2600], buf[65536];   /* static: a crash handler may run on a small signal stack */
        ssize_t n; off_t p = off;
        int hl = snprintf(hdr, sizeof hdr, "Rosettus %s report\napp: %s\nguest: %s\npid: %d\ntime: %s\nlog: %s\n\n",
                          g_rep_kind, app, exe ? exe : "-", getpid(), stamp, logp);
        write(out, hdr, hl);
        while (p < end && (n = pread(in, buf, sizeof buf < (size_t)(end - p) ? sizeof buf : (size_t)(end - p), p)) > 0) {
            if (!first[0]) {   /* first CRASH / fault / dump line for the index */
                char *s = buf, *e2;
                while (s < buf + n && !first[0]) {
                    e2 = memchr(s, '\n', buf + n - s); if (!e2) break;
                    if ((e2 - s > 6) && (!strncmp(s, "CRASH", 5) || strstr(s, "fault") || strstr(s, "===== dump") || !strncmp(s, "[trap] sig", 10) || !strncmp(s, "[fatal]", 7)) && e2 - s < (long)sizeof first)
                        { memcpy(first, s, e2 - s); first[e2 - s] = 0; }
                    s = e2 + 1;
                }
            }
            write(out, buf, n); p += n;
        }
        close(out);
        fprintf(stderr, "[report] wrote %s\n", rp);
        {   /* open/write, not stdio: no malloc inside a crash */
            char ip[1100], line[1400]; int f, L; snprintf(ip, sizeof ip, "%s/index.txt", dir);
            L = snprintf(line, sizeof line, "%s\t%s\t%s\t%s\t%s\n", stamp, app, g_rep_kind, strrchr(rp, '/') + 1, first);
            if (L > (int)sizeof line - 1) L = sizeof line - 1;
            if ((f = open(ip, O_WRONLY | O_APPEND | O_CREAT, 0644)) >= 0) { write(f, line, L); close(f); } }
    }
    close(in);
}

/* The crash handler must end the process. Writing its report once hung: AppleWorks crashed (a guest
   stack overrun in its RTF translator) and the handler then spun at full CPU for 35 minutes - in a
   lock another thread held when the fault came (the native backtrace symbolizes with dladdr). The app
   stayed registered with Launch Services as running, so opening it again only sent an Apple Event
   to the dead process (-1712). Now an alarm goes off 8 s into the handler: it writes the report with
   what was logged so far and exits; a second alarm (default action) ends the process if even that
   hangs. */
static char g_crash_rp[1300], g_crash_logp[1024], g_crash_idx[1100], g_crash_line[400];
static pthread_t g_crash_thread;
static void oah_crash_alarm(int sig) {
    /* only system calls from here: the locks libc needs may be held by threads stopped in the crash */
    static char buf[65536];
    int in, out; off_t p, end; ssize_t n;
    (void)sig;
    signal(SIGALRM, SIG_DFL);
    alarm(4);
    {   static const char m[] = "[crash] the crash handler did not finish in 8 s: writing the report and exiting\n";
        write(2, m, sizeof m - 1); }
    if (g_crash_rp[0] && g_rep_crash_off >= 0 && (in = open(g_crash_logp, O_RDONLY)) >= 0) {
        end = lseek(2, 0, SEEK_END);
        if ((out = open(g_crash_rp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
            static const char h[] = "Rosettus crash report (written by the crash watchdog)\n\n";
            write(out, h, sizeof h - 1);
            for (p = g_rep_crash_off; p < end && (n = pread(in, buf, sizeof buf < (size_t)(end - p) ? sizeof buf : (size_t)(end - p), p)) > 0; p += n)
                write(out, buf, n);
            close(out);
            write(2, "[report] wrote ", 15); write(2, g_crash_rp, strlen(g_crash_rp)); write(2, "\n", 1);
            if ((out = open(g_crash_idx, O_WRONLY | O_APPEND | O_CREAT, 0644)) >= 0) { write(out, g_crash_line, strlen(g_crash_line)); close(out); }
        }
        close(in);
    }
    _exit(1);
}
static void oah_crash_watchdog(void) {
    struct sigaction sa;
    const char *home = oah_env_home(), *exe = oah_env_exe();
    char app[128], stamp[32], *c; time_t t = time(0);
    g_crash_thread = pthread_self();
    g_rep_crash = 1; g_rep_crash_off = g_rep_off;
    /* everything the alarm needs, prepared now (before anything can hang) */
    if (g_rep_off >= 0 && fcntl(2, F_GETPATH, g_crash_logp) != -1 &&
        (home || strstr(g_crash_logp, "/Library/Logs/"))) {
        snprintf(app, sizeof app, "%s", exe && strrchr(exe, '/') ? strrchr(exe, '/') + 1 : getprogname());
        for (c = app; *c; c++) if (*c == ' ' || *c == '/') *c = '_';
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&t));
        if (home) snprintf(g_crash_idx, sizeof g_crash_idx, "%s/Library/Logs/Rosettus/Reports", home);
        else {   /* no HOME in this process's environment: the log's own ~/Library/Logs */
            snprintf(g_crash_idx, sizeof g_crash_idx, "%s", g_crash_logp);
            g_crash_idx[strstr(g_crash_idx, "/Library/Logs/") - g_crash_idx + 14] = 0;
            strlcat(g_crash_idx, "Rosettus/Reports", sizeof g_crash_idx);
        }
        mkdir(g_crash_idx, 0755);
        snprintf(g_crash_rp, sizeof g_crash_rp, "%s/%s-%s-crash.txt", g_crash_idx, app, stamp);
        snprintf(g_crash_line, sizeof g_crash_line, "%s\t%s\tcrash\t%s-%s-crash.txt\tCRASH (report written by the crash watchdog)\n", stamp, app, app, stamp);
        strlcat(g_crash_idx, "/index.txt", sizeof g_crash_idx);
    }
    if (getenv("OAH_VERBOSE") || !g_crash_rp[0]) fprintf(stderr, "[crash] watchdog: report %s, slice at %lld\n", g_crash_rp[0] ? g_crash_rp : "(none)", (long long)g_rep_crash_off);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = oah_crash_alarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, 0);
    {   sigset_t ss; sigemptyset(&ss); sigaddset(&ss, SIGALRM); pthread_sigmask(SIG_UNBLOCK, &ss, 0); }
    alarm(8);
}

/* Diagnostic only (OAH_VERBOSE): report where a SIGTRAP/SIGILL came from, then die normally.
   Rosetta compiles a guest "trap" to ud2/int3 followed by "movl $guest_pc,%ebp". */
static int oah_hang_isR(unsigned int X);
extern volatile int g_in_trap_report;
static int probe_read(unsigned int addr, unsigned int *out);
static void oah_crash_guest_frames(unsigned int RR);
static void trap_report_handler(int sig, siginfo_t *info, void *uap) {
    ucontext_t *uc = (ucontext_t *)uap;
    unsigned int eip = uc->uc_mcontext->__ss.__eip;
    if (g_in_trap_report++ > 2) { oah_report_flush(); _exit(sig == SIGILL ? 132 : 133); }   /* re-trapping: stop the loop */
    oah_report_mark(sig == SIGILL ? "trap-ill" : "trap");
    if (sig == SIGILL || sig == SIGTRAP) {   /* translator "terminate guest" (ud2) or a guest trap: show the guest state of this thread */
        unsigned int cand[400], nc = 0, k2, v, sp = uc->uc_mcontext->__ss.__esp, found = 0;
        cand[nc++] = uc->uc_mcontext->__ss.__esi; cand[nc++] = uc->uc_mcontext->__ss.__edi;
        cand[nc++] = uc->uc_mcontext->__ss.__ebx; cand[nc++] = uc->uc_mcontext->__ss.__ebp;
        for (k2 = 0; k2 < 1000 && nc < 400; k2++) if (probe_read(sp + 4 * k2, &v) && v > 0x10000 && !(v & 3)) cand[nc++] = v;
        for (k2 = 0; k2 < nc && !found; k2++) if (oah_hang_isR(cand[k2])) found = cand[k2];
        if (found) {
            unsigned int *Rt = (unsigned int *)found;
            fprintf(stderr, "[fatal] guest R=%08x r0=%08x r1=%08x r3=%08x r4=%08x r5=%08x r6=%08x lr=%08x ctr=%08x\n", found,
                    Rt[0], Rt[1], Rt[3], Rt[4], Rt[5], Rt[6], Rt[0x90 / 4], Rt[0x94 / 4]);
            if (getenv("OAH_RDUMP")) {   /* the register file past the GPRs, for layout work */
                unsigned int k3, n3 = (unsigned int)strtoul(getenv("OAH_RDUMP"), 0, 0); if (n3 < 0x80) n3 = 0x400;
                for (k3 = 0; k3 < n3; k3 += 32) fprintf(stderr, "[rdump] +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", k3,
                    Rt[k3 / 4], Rt[k3 / 4 + 1], Rt[k3 / 4 + 2], Rt[k3 / 4 + 3], Rt[k3 / 4 + 4], Rt[k3 / 4 + 5], Rt[k3 / 4 + 6], Rt[k3 / 4 + 7]);
            }
            oah_crash_guest_frames(found);
        } else fprintf(stderr, "[fatal] SIGILL: no guest register file found\n");
        fprintf(stderr, "[fatal] callppc depth %lu, last \"%s\" -> %08x\n", g_cb_depth, g_last_cp_tag ? g_last_cp_tag : "-", g_last_cp_fn);
        {   /* interpreter frames: state S at fp+8 with S+8 = R and S+0x1c = guest pc */
            unsigned int f = uc->uc_mcontext->__ss.__ebp, d2, nf, S, Ri, gpc, w2, q;
            for (d2 = 0; d2 < 24 && f > 0x1000; d2++) {
                if (!probe_read(f, &nf)) break;
                if (probe_read(f + 8, &S) && S > 0x1000 && probe_read(S + 8, &Ri) && Ri > 0x1000 && oah_hang_isR(Ri) &&
                    probe_read(S + 0x1c, &gpc) && gpc > 0x1000) {
                    fprintf(stderr, "[fatal] interpreter pc %08x (R %08x):", gpc, Ri);
                    for (q = 0; q < 6; q++) { if (!probe_read(gpc - 8 + 4 * q, &w2)) break; fprintf(stderr, " %08x", OSSwapBigToHostInt32(w2)); }
                    fprintf(stderr, "\n");
                    break;
                }
                if (nf <= f) break;
                f = nf;
            }
        }
    }
    unsigned char *b = (unsigned char *)eip;
    int k;
    fprintf(stderr, "[trap] sig=%d eip=0x%08x eax=0x%08x ebx=0x%08x ecx=0x%08x edx=0x%08x esi=0x%08x edi=0x%08x ebp=0x%08x\n[trap] bytes@eip-4:",
            sig, eip, uc->uc_mcontext->__ss.__eax, uc->uc_mcontext->__ss.__ebx, uc->uc_mcontext->__ss.__ecx,
            uc->uc_mcontext->__ss.__edx, uc->uc_mcontext->__ss.__esi, uc->uc_mcontext->__ss.__edi,
            uc->uc_mcontext->__ss.__ebp);
    for (k = -4; k < 12; k++) fprintf(stderr, " %02x", b[k]);
    fprintf(stderr, "\n");
    for (k = -2; k < 4; k++) if (b[k] == 0xbd) {
        fprintf(stderr, "[trap] guest pc = 0x%08x\n", *(unsigned int *)(b + k + 1)); break;
    }
    {
        unsigned int *fp = (unsigned int *)uc->uc_mcontext->__ss.__ebp;
        unsigned int *sp = (unsigned int *)uc->uc_mcontext->__ss.__esp;
        fprintf(stderr, "[trap] stack@esp:");
        for (k = 0; k < 16; k++) fprintf(stderr, " %08x", sp[k]);
        fprintf(stderr, "\n");
        for (k = 0; k < 16 && fp && ((unsigned int)fp & 3) == 0 && (unsigned int)fp > 0x1000; k++) {
            Dl_info di; unsigned int ra = fp[1];
            memset(&di, 0, sizeof di);
            dladdr((void *)ra, &di);
            fprintf(stderr, "[trap]  #%d ra=0x%08x %s %s\n", k, ra,
                    di.dli_fname ? di.dli_fname : "?", di.dli_sname ? di.dli_sname : "");
            if (ra == 0xb80bfb30 || ra == 0xb80bfade) {
                /* caller frame belongs to the raise-signal handler; its arg0 is the interpreter state */
                unsigned int *nfp = (unsigned int *)fp[0];
                unsigned int st = nfp[2];
                unsigned int gpc = *(unsigned int *)(st + 0x1c);
                unsigned int *g = (unsigned int *)(gpc - 0x20); int j;
                fprintf(stderr, "[trap] interp state=0x%08x guest pc=0x%08x code:", st, gpc);
                for (j = 0; j < 16; j++) fprintf(stderr, " %08x", OSSwapBigToHostInt32(g[j]));
                fprintf(stderr, "\n");
            }
            if ((unsigned int *)fp[0] <= fp) break;
            fp = (unsigned int *)fp[0];
        }
    }
    /* write the report, then let the signal take its default action when the faulting
       instruction runs again (it used to loop here: the handler stayed installed) */
    oah_report_flush();
    {   struct sigaction d; memset(&d, 0, sizeof d); d.sa_handler = SIG_DFL; sigaction(sig, &d, 0); }
}

/* OAH_BRIDGE_PROBE: on a guest trap, look for known guest register values
   near every x86 register, to learn where Rosetta keeps r3..r5 and LR. */
static int probe_read(unsigned int addr, unsigned int *out) {
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), addr, 4, (vm_address_t)out, &got) == KERN_SUCCESS && got == 4;
}
static void bridge_probe_handler(int sig, siginfo_t *info, void *uap) {
    ucontext_t *uc = (ucontext_t *)uap;
    unsigned int regs[8], k, base, off, w;
    const char *names[8] = {"eax","ebx","ecx","edx","esi","edi","ebp","esp"};
    unsigned int want[5] = {0x11111111, 0x22222222, 0x33333333, 0x00001e80, 0x00001e40};
    unsigned char *b = (unsigned char *)uc->uc_mcontext->__ss.__eip;
    regs[0] = uc->uc_mcontext->__ss.__eax; regs[1] = uc->uc_mcontext->__ss.__ebx;
    regs[2] = uc->uc_mcontext->__ss.__ecx; regs[3] = uc->uc_mcontext->__ss.__edx;
    regs[4] = uc->uc_mcontext->__ss.__esi; regs[5] = uc->uc_mcontext->__ss.__edi;
    regs[6] = uc->uc_mcontext->__ss.__ebp; regs[7] = uc->uc_mcontext->__ss.__esp;
    fprintf(stderr, "[probe] sig=%d eip=0x%08x bytes:", sig, (unsigned)b);
    for (k = 0; k < 12; k++) fprintf(stderr, " %02x", b[k]);
    fprintf(stderr, "\n[probe] eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n",
            regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7]);
    (void)names; (void)base;
    {   /* walk x86 frames; for each frame's first 4 args p, scan p and *(p+8) */
        unsigned int fp = regs[6], depth;
        for (depth = 0; depth < 24 && fp > 0x1000; depth++) {
            unsigned int ra = 0, nfp = 0, ai;
            if (!probe_read(fp + 4, &ra) || !probe_read(fp, &nfp)) break;
            fprintf(stderr, "[probe] frame %u fp=%08x ra=%08x\n", depth, fp, ra);
            for (ai = 0; ai < 4; ai++) {
                unsigned int p = 0, q = 0, pass;
                if (!probe_read(fp + 8 + ai * 4, &p) || p < 0x1000) continue;
                for (pass = 0; pass < 2; pass++) {
                    unsigned int bse = pass ? q : p;
                    if (pass && (!probe_read(p + 8, &q) || q < 0x1000)) break;
                    bse = pass ? q : p;
                    for (off = 0; off < 0x400; off += 4) {
                        unsigned int j;
                        if (!probe_read(bse + off, &w)) break;
                        for (j = 0; j < 5; j++)
                            if (w == want[j])
                                fprintf(stderr, "[probe]   arg%u%s=%08x +0x%03x: %08x (%s)\n", ai,
                                        pass ? "->[+8]" : "", bse, off, w,
                                        j < 3 ? "arg" : (j == 3 ? "LR" : "PC"));
                    }
                }
            }
            if (nfp <= fp) break;
            fp = nfp;
        }
    }
    (void)k;
    oah_report_flush();
    _exit(77);
}

/* ===== PowerPC -> native bridge =====
   A bridged guest function starts with "twi 31,r0,ID" (0x0FE00000|ID).
   Rosetta's interpreter turns that into a synchronous SIGTRAP. We find its
   interpreter state S (S+0x08 = register file R, S+0x1c = guest PC,
   S+0x20 = next PC; rN = R[N], LR = R[0x90/4]), run the native version,
   put the result in r3 and set next PC = LR so the guest function returns. */
typedef struct { float x, y, w, h; } oah_rect32;
static void *g_cg_handle;
static unsigned int (*n_CGMainDisplayID)(void);
static oah_rect32 (*n_CGDisplayBounds)(unsigned int);
static int oah_load_cg(void) {
    if (g_cg_handle) return 1;
    g_cg_handle = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY);
    if (!g_cg_handle) { fprintf(stderr, "[bridge] dlopen CoreGraphics failed: %s\n", dlerror()); return 0; }
    n_CGMainDisplayID = (unsigned int (*)(void))dlsym(g_cg_handle, "CGMainDisplayID");
    n_CGDisplayBounds = (oah_rect32 (*)(unsigned int))dlsym(g_cg_handle, "CGDisplayBounds");
    return n_CGMainDisplayID && n_CGDisplayBounds;
}
static void br_test_sum(unsigned int *R) { R[3] = R[3] + R[4] + R[5]; }
static void br_CGMainDisplayID(unsigned int *R) {
    R[3] = oah_load_cg() ? n_CGMainDisplayID() : 0;
}
static void br_CGDisplayBounds(unsigned int *R) {
    /* PPC32 Darwin: CGRect returned through hidden pointer in r3, display in r4 */
    unsigned int *dst = (unsigned int *)R[3], src[4], i;
    oah_rect32 r = {0, 0, 0, 0};
    if (oah_load_cg()) r = n_CGDisplayBounds(R[4]);
    memcpy(src, &r, sizeof src);
    for (i = 0; i < 4; i++) dst[i] = OSSwapInt32(src[i]);
}
/* ===== native -> PowerPC calls (callbacks) =====
   translate exports CallPPCFunctionAtAddressInt(name, ppc_fn, return_pc,
   nargs, args). From the disassembly: return_pc is written to register
   index 0x24 (byte offset 0x90 = LR); args[0..7] go to r3..r10; the result
   is r3. The nested run stops when guest code reaches return_pc, which must
   be a PPC stub that ends the nested run: we use a bridge trap (ID 5) whose
   native side calls translate's exported _dummyReturnContext(). */
typedef unsigned int (*oah_callppc_t)(const char *, unsigned int, unsigned int, int, unsigned int *);
static oah_callppc_t g_callppc;
static void (*g_dummyret)(void);
static unsigned long g_cb_calls;
/* CallPPCFunctionAtAddressInt runs the nested guest call on one shared
   callback stack/return context, so two threads calling into the guest at
   once (fontd: FSGetVolumeInfo on several ATS threads) corrupt each other and
   the return stub jumps to garbage. Serialize; recursive for nested calls. */
#include <pthread.h>
static pthread_mutex_t g_callppc_mu;
static pthread_once_t g_callppc_once = PTHREAD_ONCE_INIT;
static void oah_callppc_mu_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_callppc_mu, &a);
}
static pthread_t g_cp_owner;                 /* holder of g_callppc_mu and its recursion depth */
static int g_cp_count;
static void oah_callppc_lock(void) {
    pthread_once(&g_callppc_once, oah_callppc_mu_init); pthread_mutex_lock(&g_callppc_mu);
    g_cp_owner = pthread_self(); g_cp_count++;
}
static void oah_callppc_unlock(void) { if (--g_cp_count == 0) g_cp_owner = 0; pthread_mutex_unlock(&g_callppc_mu); }
/* A guest thread that gives up the CPU inside a nested guest call (Thread Manager YieldToThread:
   Internet Explorer's odoc handler yields to its cooperative threads) parks while holding the
   lock, and the thread it yields to blocks on the lock at its first call into the guest:
   deadlock. Release the lock for the time this thread is parked and take it again after;
   cooperative threads still run one at a time. Returns the depth released. */
static int oah_callppc_release_all(void) {
    int n = 0;
    if (!g_cp_count || !g_cp_owner || !pthread_equal(g_cp_owner, pthread_self())) return 0;
    while (g_cp_count) { oah_callppc_unlock(); n++; }
    return n;
}
static void oah_callppc_reacquire(int n) { while (n-- > 0) oah_callppc_lock(); }
static int oah_load_callppc(void) {
    if (g_callppc && g_dummyret) return 1;
    g_callppc = (oah_callppc_t)dlsym(RTLD_DEFAULT, "CallPPCFunctionAtAddressInt");
    g_dummyret = (void (*)(void))dlsym(RTLD_DEFAULT, "dummyReturnContext");
    if (!g_callppc) g_callppc = (oah_callppc_t)0xb8161e78u;   /* known address in translate_mojave */
    if (!g_dummyret) g_dummyret = (void (*)(void))0xb81614dau;
    return 1;
}
/* call_native(fn, a, b, retstub): calls guest fn(a, b) from native code */
static void br_call_ppc(unsigned int *R) {
    unsigned int args[2], fn = R[3], ret = R[6], r;
    args[0] = R[4]; args[1] = R[5];
    oah_load_callppc();
    g_cb_depth++;
    if (getenv("OAH_VERBOSE") && g_cb_calls < 3)
        fprintf(stderr, "[callppc] -> fn=0x%08x a=%u b=%u ret=0x%08x depth=%lu\n", fn, args[0], args[1], ret, g_cb_depth);
    oah_callppc_lock();
    r = g_callppc("oah_cb", fn, ret, 2, args);
    oah_callppc_unlock();
    if (getenv("OAH_VERBOSE") && g_cb_calls < 3)
        fprintf(stderr, "[callppc] <- r=%u\n", r);
    g_cb_depth--;
    g_cb_calls++;
    R[3] = r;
}
static void br_ret_stub(unsigned int *R) {
    (void)R;
    oah_load_callppc();
    g_dummyret();
}
/* fp_probe(1.5, 2.25, -3.0, 77): find where Rosetta keeps f1..f3 */
static void br_fp_probe(unsigned int *R) {
    static const double want[3] = { 1.5, 2.25, -3.0 };
    unsigned int off, k;
    unsigned char *b = (unsigned char *)R;
    for (off = 0; off + 8 <= 0x800; off += 4) {
        for (k = 0; k < 3; k++) {
            double d; unsigned long long q, s;
            memcpy(&q, b + off, 8);
            memcpy(&d, &q, 8);
            if (d == want[k]) fprintf(stderr, "[fp_probe] f%u as host double at R+0x%x\n", k + 1, off);
            s = OSSwapInt64(q); memcpy(&d, &s, 8);
            if (d == want[k]) fprintf(stderr, "[fp_probe] f%u as byte-swapped double at R+0x%x\n", k + 1, off);
        }
    }
    fprintf(stderr, "[fp_probe] r9=%u (want 77: Darwin PPC ABI, doubles shadow r3..r8)\n", R[9]);
    /* return f1 = f1 + f2 + f3 (fN is a host-order double at R+0x1e8+8*N) */
    {
        double *F = (double *)((unsigned char *)R + 0x1e8);
        F[1] = F[1] + F[2] + F[3];
    }
}
/* dump_windows(): list the native WindowServer windows owned by this process */
static void br_dump_windows(unsigned int *R) {
    CFArrayRef (*copyinfo)(unsigned int, unsigned int);
    CFArrayRef arr;
    CFIndex i, n, mine = 0;
    int me = getpid();
    R[3] = 0;
    if (!oah_load_cg()) return;
    copyinfo = (CFArrayRef (*)(unsigned int, unsigned int))dlsym(g_cg_handle, "CGWindowListCopyWindowInfo");
    if (!copyinfo || !(arr = copyinfo(0, 0))) { fprintf(stderr, "[windows] CGWindowListCopyWindowInfo failed\n"); return; }
    n = CFArrayGetCount(arr);
    for (i = 0; i < n; i++) {
        CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, i);
        CFNumberRef pidn = (CFNumberRef)CFDictionaryGetValue(d, CFSTR("kCGWindowOwnerPID"));
        int pid = 0, layer = 0, onscreen = 0, num = 0;
        double bx = 0, by = 0, bw = 0, bh = 0;
        char name[256] = "", owner[256] = "";
        CFTypeRef v;
        CFDictionaryRef b;
        if (!pidn || !CFNumberGetValue(pidn, kCFNumberIntType, &pid) || pid != me) continue;
        mine++;
        if ((v = CFDictionaryGetValue(d, CFSTR("kCGWindowNumber")))) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &num);
        if ((v = CFDictionaryGetValue(d, CFSTR("kCGWindowLayer")))) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &layer);
        if ((v = CFDictionaryGetValue(d, CFSTR("kCGWindowIsOnscreen")))) onscreen = CFBooleanGetValue((CFBooleanRef)v);
        if ((v = CFDictionaryGetValue(d, CFSTR("kCGWindowName")))) CFStringGetCString((CFStringRef)v, name, sizeof name, kCFStringEncodingUTF8);
        if ((v = CFDictionaryGetValue(d, CFSTR("kCGWindowOwnerName")))) CFStringGetCString((CFStringRef)v, owner, sizeof owner, kCFStringEncodingUTF8);
        if ((b = (CFDictionaryRef)CFDictionaryGetValue(d, CFSTR("kCGWindowBounds")))) {
            if ((v = CFDictionaryGetValue(b, CFSTR("X")))) CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &bx);
            if ((v = CFDictionaryGetValue(b, CFSTR("Y")))) CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &by);
            if ((v = CFDictionaryGetValue(b, CFSTR("Width")))) CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &bw);
            if ((v = CFDictionaryGetValue(b, CFSTR("Height")))) CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &bh);
        }
        fprintf(stderr, "[windows] pid=%d win=%d owner='%s' name='%s' layer=%d onscreen=%d bounds=%.0f,%.0f %.0fx%.0f\n",
                pid, num, owner, name, layer, onscreen, bx, by, bw, bh);
    }
    fprintf(stderr, "[windows] %ld of %ld WindowServer windows belong to pid %d\n", (long)mine, (long)n, me);
    CFRelease(arr);
    R[3] = (unsigned int)mine;
}

/* ===== Table-driven bridge (IDs 16 and up) =====
   Each entry names a native function in Mojave's 32-bit Carbon umbrella and
   gives its signature as "RET:ARGS".
   RET:  v void, i 32-bit, h 16-bit signed (OSErr), b Boolean, D double
   ARGS: i 32-bit value, handle or pointer passed through unchanged
         (native handles are only stored by guest code, never dereferenced;
          pointers to plain bytes such as Pascal strings need no swapping)
         d double (next FPR; also uses up two GPRs, Darwin PPC ABI)
         R pointer to a QuickDraw Rect read by native code (4 big-endian shorts)
         r pointer to a QuickDraw Rect written by native code
         o pointer to a 32-bit value written by native code
         O pointer to a 32-bit value read and written by native code
         f float (next FPR, passed natively as 4-byte float; uses one GPR)
         C CGRect by value (4 floats; on PPC these arrive raw in 4 GPRs)
         G CGPoint by value (2 raw GPRs)
         X Rect* read and written; p Point* read and written (2 shorts)
         K RGBColor* read (3 unsigned shorts); N 2 x UInt32 read and written
         L EventTypeSpec array (count = the previous argument), read
   RET T: trace stub - no native function; log the call and return 0.
   Guest and native code share one address space, so only the bytes behind
   pointers need converting between big- and little-endian.
   The table itself is generated from bridges.spec by genbridges.py. */
#define OAH_GEN_BASE 16
typedef struct { const char *name; const char *sig; void *fn; int state; unsigned long calls; } oah_gen_t;
#include "bridges_gen.h"
#define OAH_NGEN (sizeof g_gen / sizeof g_gen[0])
static void *g_carbon_handle;
static int g_fg_done;
/* Mojave knows the bundle is PowerPC and shows its Dock tile with the "can't run" badge.
   Give the running app its own icon (CFBundleIconFile of the guest bundle) as the Dock tile,
   the way Rosetta's apps looked. OAH_NO_DOCKICON=1 disables. */
/* Single-file apps (Office 2004's "Microsoft Word"): no bundle, the icon is an 'icns' resource in
   the resource fork. Prefer the custom icon (-16455), then 128, then the lowest id. +1 CFData or NULL. */
static CFDataRef oah_rsrc_icns(const char *file) {
    char f[PATH_MAX + 32]; struct stat st; unsigned char *b = 0; int fd, t, r, nt, nr, best = -1, bid = 0;
    unsigned int doff, moff, tl, n, bdp = 0, blen = 0;
    CFDataRef out = 0;
    snprintf(f, sizeof f, "%s/..namedfork/rsrc", file);
    if ((fd = open(f, O_RDONLY)) < 0) return 0;
    if (fstat(fd, &st) || st.st_size < 32 || st.st_size > (64 << 20) || !(b = (unsigned char *)malloc(st.st_size)) ||
        read(fd, b, st.st_size) != st.st_size) { close(fd); free(b); return 0; }
    close(fd);
    n = (unsigned int)st.st_size;
#define RB32(o) ((unsigned int)b[o] << 24 | (unsigned int)b[(o) + 1] << 16 | (unsigned int)b[(o) + 2] << 8 | b[(o) + 3])
#define RB16(o) ((unsigned int)b[o] << 8 | b[(o) + 1])
    doff = RB32(0); moff = RB32(4);
    if (moff + 30 > n) goto done;
    tl = moff + RB16(moff + 24);
    if (tl + 2 > n) goto done;
    nt = (int)RB16(tl) + 1;
    for (t = 0; t < nt; t++) {
        unsigned int te = tl + 2 + t * 8, rl;
        if (te + 8 > n) goto done;
        if (RB32(te) != 0x69636e73u) continue;              /* 'icns' */
        nr = (int)RB16(te + 4) + 1;
        rl = tl + RB16(te + 6);
        for (r = 0; r < nr; r++) {
            unsigned int re = rl + r * 12, dp, len;
            int id, rank;
            if (re + 12 > n) goto done;
            id = (short)RB16(re);
            dp = doff + (RB32(re + 4) & 0xffffff);
            if (dp + 4 > n || dp + 4 + (len = RB32(dp)) > n) continue;
            rank = id == -16455 ? 0 : id == 128 ? 1 : 2;
            if (best < 0 || rank < best || (rank == best && id < bid)) { best = rank; bid = id; bdp = dp + 4; blen = len; }
        }
    }
    if (best >= 0) out = CFDataCreate(NULL, b + bdp, blen);
#undef RB32
#undef RB16
done:
    free(b);
    return out;
}
static void oah_set_dock_icon(void) {
    char path[PATH_MAX], f[PATH_MAX + 64], icon[256] = "";
    const char *exe = getenv("OAH_GUEST_EXE"), *pl[] = { "Info.plist", "Info-macos.plist" };
    void *as = dlopen("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices", RTLD_LAZY | RTLD_GLOBAL);
    void *(*isrc)(CFURLRef, CFDictionaryRef) = as ? (void *(*)(CFURLRef, CFDictionaryRef))dlsym(as, "CGImageSourceCreateWithURL") : 0;
    void *(*isrcd)(CFDataRef, CFDictionaryRef) = as ? (void *(*)(CFDataRef, CFDictionaryRef))dlsym(as, "CGImageSourceCreateWithData") : 0;
    CFDataRef rd = 0;
    size_t (*icount)(void *) = as ? (size_t (*)(void *))dlsym(as, "CGImageSourceGetCount") : 0;
    void *(*iimg)(void *, size_t, CFDictionaryRef) = as ? (void *(*)(void *, size_t, CFDictionaryRef))dlsym(as, "CGImageSourceCreateImageAtIndex") : 0;
    size_t (*iw)(void *) = as ? (size_t (*)(void *))dlsym(as, "CGImageGetWidth") : 0;
    void (*irel)(void *) = as ? (void (*)(void *))dlsym(as, "CGImageRelease") : 0;
    int (*dock)(void *) = g_carbon_handle ? (int (*)(void *))dlsym(g_carbon_handle, "SetApplicationDockTileImage") : 0;
    char *m;
    int i, err = -1;
    void *src, *best = 0;
    if (getenv("OAH_NO_DOCKICON") || !exe || !realpath(exe, path) || !isrc || !iimg || !dock) return;
    if (!(m = strstr(path, ".app/Contents/"))) {
        if (!isrcd || !(rd = oah_rsrc_icns(path))) return;
        snprintf(f, sizeof f, "%s (resource fork)", path);
        icon[0] = 1;                                     /* skip the bundle lookups below */
    } else m[14] = 0;
    for (i = 0; i < 2 && !icon[0]; i++) {
        CFURLRef u;
        CFDataRef d = 0;
        CFPropertyListRef p = 0;
        snprintf(f, sizeof f, "%s%s", path, pl[i]);
        u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)f, strlen(f), false);
        if (u && CFURLCreateDataAndPropertiesFromResource(NULL, u, &d, NULL, NULL, NULL) && d)
            p = CFPropertyListCreateFromXMLData(NULL, d, kCFPropertyListImmutable, NULL);
        if (p && CFGetTypeID(p) == CFDictionaryGetTypeID()) {
            CFStringRef s = CFDictionaryGetValue((CFDictionaryRef)p, CFSTR("CFBundleIconFile"));
            if (s && CFGetTypeID(s) == CFStringGetTypeID()) CFStringGetCString(s, icon, sizeof icon, kCFStringEncodingUTF8);
        }
        if (p) CFRelease(p);
        if (d) CFRelease(d);
        if (u) CFRelease(u);
    }
    if (rd) ;
    else if (icon[0]) snprintf(f, sizeof f, "%sResources/%s%s", path, icon, strstr(icon, ".icns") ? "" : ".icns");
    if (!rd && (!icon[0] || access(f, R_OK)))   /* no icon of its own: the generic app icon, not Mojave's "can't run" badge */
        snprintf(f, sizeof f, "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericApplicationIcon.icns");
    if (rd) { src = isrcd(rd, NULL); CFRelease(rd); }
    else {
        CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)f, strlen(f), false);
        src = u ? isrc(u, NULL) : 0;
        if (u) CFRelease(u);
    }
    if (src) {
        size_t n = icount(src), k, bw = 0;
        for (k = 0; k < n; k++) {
            void *img = iimg(src, k, NULL);
            size_t w = img ? iw(img) : 0;
            if (img && w > bw && w <= 512) { if (best) irel(best); best = img; bw = w; }
            else if (img) irel(img);
        }
        CFRelease((CFTypeRef)src);
    }
    if (best) { err = dock(best); irel(best); }
    fprintf(stderr, "[bridge/gen] Dock tile from %s -> %d\n", f, err);
}
/* Mojave registered this process as "translate_mojave" with no bundle: Cmd-Tab, Force Quit,
   lsappinfo and Apple Event targeting by bundle id saw the translator. Copy the guest bundle's
   identity into its native LaunchServices record. OAH_NO_LSIDENT=1 disables, logs with OAH_LSTRACE. */
static void oah_ae_checkin(void);
static int oah_background_kind(void);
static volatile unsigned int g_main_bridge_id;          /* defined in oah_hang.h */
/* A background-only / UI-element guest app (Microsoft Database Daemon, started by Word):
   LaunchServices takes the type from the main bundle's Info dictionary when the process checks
   in, and translate_mojave has none, so the daemon checked in as a foreground app: its Dock tile
   bounced once and stayed under recent apps as the folder it lives in. Put the guest's
   LSBackgroundOnly / LSUIElement there before the first check-in. OAH_NO_BGINFO=1 disables. */
static void oah_bg_info(void) {
    static int done;
    int bg;
    CFBundleRef mb;
    CFDictionaryRef info;
    if (done++ || getenv("OAH_NO_BGINFO")) return;
    bg = oah_background_kind();
    mb = bg ? CFBundleGetMainBundle() : 0;
    info = mb ? CFBundleGetInfoDictionary(mb) : 0;
    if (!info) { if (bg) fprintf(stderr, "[bridge/gen] no main bundle Info dictionary to mark %s\n", bg == 2 ? "background-only" : "UI-element"); return; }
    CFDictionarySetValue((CFMutableDictionaryRef)info, bg == 2 ? CFSTR("LSBackgroundOnly") : CFSTR("LSUIElement"), kCFBooleanTrue);
    fprintf(stderr, "[bridge/gen] checking in as a %s app\n", bg == 2 ? "background-only" : "UI-element");
}
static void *oah_ae_early_thread(void *arg) {
    (void)arg;
    {   /* Let the translator set itself up and map the guest first. Our allocations here (CF,
           Launch Services) ran concurrently with that and could take address space it needed:
           the translator's allocator stopped with "translate fatal(2)" (Mactracker), or pages
           inside the guest executable's zero-fill __DATA went to us and the guest dyld later put
           Interposers.dylib there ("lazy pointer not found at address 0xd6058 in image
           G4Quasar"). Wait until the guest's main thread makes its first bridged call (its code
           is running), at most 1.5 s. */
        unsigned int k;
        for (k = 0; !g_main_bridge_id && k < 300; k++) usleep(5000);
        if (getenv("OAH_AETRACE")) fprintf(stderr, "[aen] early thread: guest running after %u ms\n", k * 5);
    }
    /* no delay: a launching osascript retries its -600 for only ~0.45 s after the launch */
    if (getenv("OAH_AE_EARLY_DELAY")) usleep(atoi(getenv("OAH_AE_EARLY_DELAY")) * 1000);
    /* ...and check in with LaunchServices now (Carbon GetCurrentProcess -> _RegisterApplication):
       until this process re-checked in ~2 s later, appleeventsd answered -600 for our PSN */
    if (getenv("OAH_AETRACE")) { struct timeval tv; gettimeofday(&tv, 0); fprintf(stderr, "[aen] early thread start wall %ld.%03d\n", (long)tv.tv_sec % 100, (int)(tv.tv_usec / 1000)); }
    oah_bg_info();
    if (!getenv("OAH_NO_EARLY_LSCHECKIN")) {
        void *hs = dlopen("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices", RTLD_LAZY | RTLD_GLOBAL);
        int (*gcp)(void *) = hs ? (int (*)(void *))dlsym(hs, "GetCurrentProcess") : 0;
        struct { unsigned int hi, lo; } psn = { 0, 0 };
        double t1 = CFAbsoluteTimeGetCurrent();
        int e = gcp ? gcp(&psn) : -999;
        if (getenv("OAH_AETRACE")) {
            struct timeval tv; gettimeofday(&tv, 0);
            fprintf(stderr, "[aen] early LS check-in: GetCurrentProcess -> %d psn %u/%u (%.3f s; wall %ld.%03d)\n", e, psn.hi, psn.lo,
                    CFAbsoluteTimeGetCurrent() - t1, (long)tv.tv_sec % 100, (int)(tv.tv_usec / 1000));
        }
    }
    oah_ae_checkin();
    return 0;
}
static void oah_set_ls_identity(void) {
    char path[PATH_MAX], f[PATH_MAX + 64], *m;
    const char *exe = getenv("OAH_GUEST_EXE");
    void *cs = dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_LAZY | RTLD_GLOBAL);
    CFTypeRef (*mk)(CFAllocatorRef, pid_t) = cs ? (CFTypeRef (*)(CFAllocatorRef, pid_t))dlsym(cs, "_LSASNCreateWithPid") : 0;
    int (*set)(int, CFTypeRef, CFStringRef, CFTypeRef, CFDictionaryRef *) =
        cs ? (int (*)(int, CFTypeRef, CFStringRef, CFTypeRef, CFDictionaryRef *))dlsym(cs, "_LSSetApplicationInformationItem") : 0;
    CFURLRef u; CFDataRef d = 0; CFPropertyListRef p = 0; CFTypeRef asn;
    if (getenv("OAH_NO_LSIDENT") || !exe || !mk || !set || !realpath(exe, path) || !(m = strstr(path, ".app/Contents/"))) return;
    m[14] = 0;
    snprintf(f, sizeof f, "%sInfo.plist", path);
    u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)f, strlen(f), false);
    if (u && CFURLCreateDataAndPropertiesFromResource(NULL, u, &d, NULL, NULL, NULL) && d)
        p = CFPropertyListCreateFromXMLData(NULL, d, kCFPropertyListImmutable, NULL);
    asn = mk(NULL, getpid());
    if (p && asn && CFGetTypeID(p) == CFDictionaryGetTypeID()) {
        static const char *keys[][2] = { { "CFBundleIdentifier", "CFBundleIdentifier" }, { "CFBundleName", "CFBundleName" },
                                         { "CFBundleName", "LSDisplayName" }, { "CFBundleSignature", "CFBundleSignature" } };
        CFStringRef bp, ep;
        int k, e;
        for (k = 0; k < 4; k++) {
            CFStringRef sk = CFStringCreateWithCString(0, keys[k][0], kCFStringEncodingUTF8);
            CFStringRef dk = CFStringCreateWithCString(0, keys[k][1], kCFStringEncodingUTF8);
            CFTypeRef v = CFDictionaryGetValue((CFDictionaryRef)p, sk);
            if (!v && k == 2) v = CFDictionaryGetValue((CFDictionaryRef)p, CFSTR("CFBundleExecutable"));
            e = v ? set(-2, asn, dk, v, NULL) : -1;
            if (getenv("OAH_LSTRACE")) fprintf(stderr, "[ls] set %s -> %d\n", keys[k][1], e);
            CFRelease(sk); CFRelease(dk);
        }
        m[4] = 0;                                      /* ".../Foo.app" */
        bp = CFStringCreateWithCString(0, path, kCFStringEncodingUTF8);
        ep = CFStringCreateWithCString(0, exe, kCFStringEncodingUTF8);
        e = set(-2, asn, CFSTR("LSBundlePath"), bp, NULL);
        if (getenv("OAH_LSTRACE")) fprintf(stderr, "[ls] set LSBundlePath %s -> %d\n", path, e);
        e = set(-2, asn, CFSTR("CFBundleExecutablePath"), ep, NULL);
        if (getenv("OAH_LSTRACE")) fprintf(stderr, "[ls] set CFBundleExecutablePath -> %d\n", e);
        CFRelease(bp); CFRelease(ep);
    }
    if (asn) CFRelease(asn);
    if (p) CFRelease(p);
    if (d) CFRelease(d);
    if (u) CFRelease(u);
}
/* Background-only apps (helpers such as Office 2004's Microsoft Database Daemon, or anything
   with LSBackgroundOnly / LSUIElement) must not become a foreground app: one that did took the
   frontmost slot without a window, so keystrokes went nowhere. Classic apps say so with the
   onlyBackground bit (0x0400) of their 'SIZE' resource, in the resource fork or, for a bundle,
   Contents/Resources/<executable>.rsrc. Returns 2 background-only, 1 UI element, 0 normal. */
static int oah_rsrc_size_flags(const char *file) {
    int fd = open(file, O_RDONLY), flags = -1;
    unsigned char h[16], *map = 0;
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) == 0 && st.st_size > 32 && st.st_size < (64 << 20) && pread(fd, h, 16, 0) == 16) {
        unsigned int doff = OSReadBigInt32(h, 0), moff = OSReadBigInt32(h, 4), mlen = OSReadBigInt32(h, 12);
        if (moff < st.st_size && mlen >= 30 && mlen < (4 << 20) && (map = malloc(mlen)) && pread(fd, map, mlen, moff) == (ssize_t)mlen) {
            unsigned int tl = OSReadBigInt16(map, 24), nt, i, j;
            if (tl + 2 <= mlen) {
                nt = (OSReadBigInt16(map, tl) + 1) & 0xffff;
                for (i = 0; i < nt && tl + 2 + 8 * (i + 1) <= mlen; i++) {
                    unsigned char *te = map + tl + 2 + 8 * i;
                    if (memcmp(te, "SIZE", 4)) continue;
                    unsigned int nr = OSReadBigInt16(te, 4) + 1, rl = tl + OSReadBigInt16(te, 6);
                    int best = -1;
                    for (j = 0; j < nr && rl + 12 * (j + 1) <= mlen; j++) {
                        unsigned char *re = map + rl + 12 * j;
                        short id = (short)OSReadBigInt16(re, 0);
                        unsigned int off = OSReadBigInt32(re, 4) & 0xffffff;
                        unsigned char d[6];
                        if ((id == -1 || (id == 0 && best < 0)) && pread(fd, d, 6, doff + off) == 6 && OSReadBigInt32(d, 0) >= 2)
                            { best = OSReadBigInt16(d, 4); if (id == -1) break; }
                    }
                    flags = best;
                    break;
                }
            }
        }
    }
    free(map);
    close(fd);
    return flags;
}
static int oah_background_kind(void) {
    static int kind = -1;
    char exe[PATH_MAX] = "", f[PATH_MAX + 64], *m;
    char ***av = _NSGetArgv();
    int i, fl;
    if (kind >= 0) return kind;
    kind = 0;
    if (getenv("OAH_NO_BGCHECK")) return kind;
    /* the program being run: after LaunchCFMApp for CFM apps, else what the rosetta script said */
    for (i = 0; av && *av && (*av)[i]; i++) {
        const char *a = (*av)[i]; size_t L = strlen(a);
        if (L >= 12 && !strcmp(a + L - 12, "LaunchCFMApp") && (*av)[i + 1]) { strlcpy(exe, (*av)[i + 1], sizeof exe); break; }
    }
    if (!exe[0] && getenv("OAH_GUEST_EXE")) strlcpy(exe, getenv("OAH_GUEST_EXE"), sizeof exe);
    if (!exe[0]) return kind;
    if ((m = strstr(exe, ".app/Contents/MacOS/"))) {          /* bundle: Info.plist keys, then the .rsrc file */
        CFURLRef u; CFDataRef d = 0; CFPropertyListRef p = 0;
        const char *names[] = { "Info.plist", "Info-macos.plist" };
        int k;
        for (k = 0; k < 2 && !p; k++) {
            snprintf(f, sizeof f, "%.*sContents/%s", (int)(m + 5 - exe), exe, names[k]);
            u = CFURLCreateFromFileSystemRepresentation(0, (const UInt8 *)f, strlen(f), false);
            if (u && CFURLCreateDataAndPropertiesFromResource(0, u, &d, 0, 0, 0) && d)
                p = CFPropertyListCreateWithData(0, d, 0, 0, 0);
            if (d) { CFRelease(d); d = 0; }
            if (u) CFRelease(u);
        }
        if (p && CFGetTypeID(p) == CFDictionaryGetTypeID()) {
            CFTypeRef b = CFDictionaryGetValue((CFDictionaryRef)p, CFSTR("LSBackgroundOnly"));
            CFTypeRef e = CFDictionaryGetValue((CFDictionaryRef)p, CFSTR("LSUIElement"));
            /* the older spellings (Office 2004's Alerts Daemon has NSUIElement = "1"; without this it
               showed a generic Dock icon) */
            if (!b) b = CFDictionaryGetValue((CFDictionaryRef)p, CFSTR("NSBGOnly"));
            if (!e) e = CFDictionaryGetValue((CFDictionaryRef)p, CFSTR("NSUIElement"));
            #define OAH_TRUEISH(v) ((v) && ((CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)(v))) || \
                (CFGetTypeID(v) == CFStringGetTypeID() && CFStringGetIntValue((CFStringRef)(v)) == 1)))
            if (OAH_TRUEISH(b)) kind = 2; else if (OAH_TRUEISH(e)) kind = 1;
        }
        if (p) CFRelease(p);
        if (!kind) {
            snprintf(f, sizeof f, "%.*sContents/Resources/%s.rsrc", (int)(m + 5 - exe), exe, m + 20);
            fl = oah_rsrc_size_flags(f);
            if (fl >= 0 && (fl & 0x0400)) kind = 2;
        }
    }
    if (!kind) {                                               /* resource fork of the executable itself */
        snprintf(f, sizeof f, "%s/..namedfork/rsrc", exe);
        fl = oah_rsrc_size_flags(f);
        if (fl >= 0 && (fl & 0x0400)) kind = 2;
    }
    if (kind) fprintf(stderr, "[bridge/gen] %s is a %s app: not made foreground\n", exe, kind == 2 ? "background-only" : "UI-element");
    return kind;
}
static void oah_make_foreground(void) {
    struct { unsigned int hi, lo; } psn = { 0, 2 };   /* kCurrentProcess */
    int bg = oah_background_kind();
    oah_bg_info();
    int (*tpt)(void *, unsigned int) = (int (*)(void *, unsigned int))dlsym(g_carbon_handle, "TransformProcessType");
    int (*sfp)(void *) = (int (*)(void *))dlsym(g_carbon_handle, "SetFrontProcess");
    /* kProcessTransformToForegroundApplication (1) or ...ToUIElementApplication (4) */
    int a = tpt ? tpt(&psn, bg == 2 ? 2 /* ...ToBackgroundApplication */ : bg == 1 ? 4 : 1) : -999;
    int b = bg == 2 ? 0 : sfp ? sfp(&psn) : -999;
    fprintf(stderr, "[bridge/gen] TransformProcessType=%d SetFrontProcess=%d\n", a, b);
    oah_set_ls_identity();
    /* Tell LaunchServices we finished launching (what a native app's first event loop does):
       without it the Dock bounced the icon forever and showed no running indicator whenever the
       app was started directly (Terminal / .command) rather than through a wrapper's native rlaunch. */
    if (!getenv("OAH_NO_SIGREADY")) {
        void *hs = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/HIServices.framework/HIServices", RTLD_LAZY | RTLD_GLOBAL);
        void (*sar)(void) = hs ? (void (*)(void))dlsym(hs, "_SignalApplicationReady") : 0;
        if (sar) sar();
        if (getenv("OAH_VERBOSE")) fprintf(stderr, "[bridge/gen] _SignalApplicationReady %s\n", sar ? "called" : "missing");
    }
    oah_ae_checkin();                                  /* native Apple Events (oah_aen.h): before the launch event goes missing */
    if (bg == 2) return;                               /* no Dock tile, menu bar or UI mode for a background helper */
    oah_set_dock_icon();
    /* Guest-toolbox apps draw their own 10.6 menu bar window at the top of the screen, but
       Mojave's menu bar (owned by WindowServer, still showing the previous app's menus since
       we have no NSMenu) sits above it and takes its clicks. While we are frontmost, hide
       Mojave's menu bar (and the Dock) so the app's own menu bar is visible and usable.
       OAH_NATIVE_MENUBAR=1 keeps Mojave's. */
    if (getenv("OAH_GTB") && getenv("OAH_HIDE_MOJAVE_MENUBAR")) {   /* old behaviour, off by default */
        int (*sum)(unsigned int, unsigned int) = (int (*)(unsigned int, unsigned int))dlsym(g_carbon_handle, "SetSystemUIMode");
        int c = sum ? sum(3 /* kUIModeAllHidden */, 0) : -999;
        fprintf(stderr, "[bridge/gen] SetSystemUIMode(all hidden) = %d\n", c);
    }
}
static void *g_agl_handle, *g_ogl_handle;
static void *oah_gen_resolve(oah_gen_t *g) {
    if (g->state == 0) {
        if (!g_carbon_handle)
            g_carbon_handle = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon", RTLD_LAZY | RTLD_GLOBAL);
        if (!g_carbon_handle) fprintf(stderr, "[bridge/gen] dlopen Carbon failed: %s\n", dlerror());
        g->fn = g_carbon_handle ? dlsym(g_carbon_handle, g->name) : 0;
        if (!g->fn && (!strncmp(g->name, "agl", 3) || !strncmp(g->name, "gl", 2) || !strncmp(g->name, "CGL", 3))) {
            if (!g_agl_handle) g_agl_handle = dlopen("/System/Library/Frameworks/AGL.framework/AGL", RTLD_LAZY | RTLD_GLOBAL);
            if (!g_ogl_handle) g_ogl_handle = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_GLOBAL);
            if (g_agl_handle) g->fn = dlsym(g_agl_handle, g->name);
            if (!g->fn && g_ogl_handle) g->fn = dlsym(g_ogl_handle, g->name);
        }
        g->state = g->fn ? 1 : 2;
        if (!g->fn) fprintf(stderr, "[bridge/gen] cannot resolve %s\n", g->name);
    }
    return g->fn;
}
typedef unsigned int (*oah_gen_int_t)(unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int,
                                      unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int);
typedef double (*oah_gen_dbl_t)(unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int,
                                unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int);
/* Handles returned to the guest: PPC code dereferences them itself (e.g.
   SetCursor(*GetCursor(n))), so give it a shadow master-pointer cell holding
   the native master pointer in big-endian. 'H' arguments map back. */
#define OAH_NSHADOW 512
static unsigned int g_shadow_cell[OAH_NSHADOW], *g_shadow_ptr[OAH_NSHADOW];
static unsigned int g_shadow_native[OAH_NSHADOW], g_nshadow;
static unsigned int oah_shadow_handle(unsigned int h) {
    unsigned int k;
    if (!h) return 0;
    for (k = 0; k < g_nshadow; k++)
        if (g_shadow_native[k] == h) {
            *g_shadow_ptr[k] = OSSwapHostToBigInt32(*(unsigned int *)h);   /* master pointer may have moved */
            return (unsigned int)g_shadow_ptr[k];
        }
    if (g_nshadow >= OAH_NSHADOW) { fprintf(stderr, "[bridge/gen] shadow handle table full\n"); return h; }
    k = g_nshadow++;
    g_shadow_native[k] = h;
    g_shadow_ptr[k] = &g_shadow_cell[k];
    g_shadow_cell[k] = OSSwapHostToBigInt32(*(unsigned int *)h);
    return (unsigned int)g_shadow_ptr[k];
}
/* GDHandles: PPC code reads the GDevice record itself (e.g. (*gd)->gdRect at
   0x22), so hand it a big-endian copy. GDevice and PixMap use mac68k (2-byte)
   packing on both sides, so offsets match; only byte order differs. Handles
   inside the copy are shadowed (gdPMap) or cleared. Refreshed on every return. */
static int oah_readable(vm_address_t a, vm_size_t n);
#define OAH_NGD 16
typedef struct { unsigned int native, cell, pmcell; unsigned char gd[64], pm[52]; } oah_gd_t;
static oah_gd_t g_gd[OAH_NGD];
static unsigned int g_ngd;
static void oah_swap_fields(unsigned char *dst, const unsigned char *src, const char *lay) {
    /* lay: 's' 2-byte field, 'l' 4-byte field, 'z' 4-byte field cleared */
    for (; *lay; lay++) {
        if (*lay == 's') { unsigned short v; memcpy(&v, src, 2); v = OSSwapHostToBigInt16(v); memcpy(dst, &v, 2); src += 2; dst += 2; }
        else { unsigned int v = 0; if (*lay == 'l') { memcpy(&v, src, 4); v = OSSwapHostToBigInt32(v); } memcpy(dst, &v, 4); src += 4; dst += 4; }
    }
}
static unsigned int oah_gd_shadow(unsigned int h) {
    unsigned int k, gp, pmh;
    oah_gd_t *s = 0;
    if (!h || !oah_readable(h, 4)) return h;
    gp = *(unsigned int *)h;
    if (!gp || !oah_readable(gp, 62)) return h;
    for (k = 0; k < g_ngd; k++) if (g_gd[k].native == h) { s = &g_gd[k]; break; }
    if (!s) {
        if (g_ngd >= OAH_NGD) { fprintf(stderr, "[bridge] GDevice shadow table full\n"); return h; }
        s = &g_gd[g_ngd++];
        s->native = h;
    }
    /* gdRefNum gdID gdType gdITable gdResPref gdSearchProc gdCompProc gdFlags gdPMap
       gdRefCon gdNextGD gdRect(4s) gdMode gdCCBytes gdCCDepth gdCCXData gdCCXMask gdExt */
    oah_swap_fields(s->gd, (const unsigned char *)gp, "ssszszzszlzsssslsszzz");
    pmh = *(unsigned int *)(gp + 22);
    if (pmh && oah_readable(pmh, 4) && *(unsigned int *)pmh && oah_readable(*(unsigned int *)pmh, 50)) {
        /* baseAddr rowBytes bounds(4s) pmVersion packType packSize hRes vRes pixelType
           pixelSize cmpCount cmpSize pixelFormat pmTable pmExt */
        oah_swap_fields(s->pm, (const unsigned char *)*(unsigned int *)pmh, "lssssssslllsssslzz");
        s->pmcell = OSSwapHostToBigInt32((unsigned int)s->pm);
        { unsigned int c = OSSwapHostToBigInt32((unsigned int)&s->pmcell); memcpy(s->gd + 22, &c, 4); }
    }
    s->cell = OSSwapHostToBigInt32((unsigned int)s->gd);
    return (unsigned int)&s->cell;
}
static unsigned int oah_native_handle(unsigned int g) {
    unsigned int k;
    for (k = 0; k < g_ngd; k++)
        if ((unsigned int)&g_gd[k].cell == g) return g_gd[k].native;
    for (k = 0; k < g_nshadow; k++)
        if ((unsigned int)g_shadow_ptr[k] == g) return g_shadow_native[k];
    return g;
}
static unsigned int *g_last_R;
static unsigned int *volatile g_main_R;   /* main thread's guest register file (heartbeat) */
/* the main thread's guest r1 when it last stopped in a bridge or a gtrace hook (the register file
   itself is reused by guest calls made from there, so its r1 is the callback stack's afterwards) */
static volatile unsigned int g_main_sp;
static void oah_get_process_bundle_location(unsigned int *R);
static void oah_fs_find_folder(unsigned int *R);
static void oah_cg_get_active_display_list(unsigned int *R);
static void oah_fs_get_volume_info(unsigned int *R);
static void oah_cg_display_modes(unsigned int *R, int avail);
static void oah_cg_best_mode(unsigned int *R, int with_rate);
static void oah_cg_switch_mode(unsigned int *R);
static unsigned int oah_guest_empty_array(void);
static void oah_get_new_cwindow(unsigned int *R);
static unsigned long g_trace_total;
static const char *oah_guest_image_name(unsigned int addr, unsigned int *base_out);
static const char *oah_guest_image_name_fast(unsigned int addr, unsigned int *base_out);
static void oah_crash_guest_frames(unsigned int RR);
void oah_exit_note(unsigned int *a);
static volatile unsigned int *g_dbg_rcvbuf;
/* Safe big-endian word read for the trace backtraces: a guest stack chain on a
   secondary thread ran into unmapped memory and the trace itself crashed (AppleWorks). */
static int oah_bt_word(unsigned int a, unsigned int *v) {
    vm_size_t got = 0;
    unsigned int w = 0;
    if (a < 0x1000 || (a & 3)) return 0;
    if (vm_read_overwrite(mach_task_self(), a, 4, (vm_address_t)&w, &got) != KERN_SUCCESS || got != 4) return 0;
    *v = OSSwapBigToHostInt32(w);
    return 1;
}
/* Status items, the parts outside oah_cgs.h. CGSSetPreventsActivation(cid, wid, flag): AppKit sets
   it on status item windows (a click opens the item's menu without activating the app); it was a
   stub (OAH_NO_PREVACT=1). CGSServerOperationState(16): AppKit 10.6 asks it (with IsMenuBarVisible
   and CGDisplayIsCaptured) whether the server orders status item windows in; answering 1 makes
   AppKit order them itself, which Mojave's server leaves to the app (OAH_NO_STATUSBAR=1: 0). */
static int oah_sb_misc_bridge(const char *nm, unsigned int *R) {
    if (!strcmp(nm, "CGSSetPreventsActivation")) {
        static int (*f)(int, int, int); static int off = -1;
        if (off < 0) off = getenv("OAH_NO_PREVACT") != 0;
        if (off) return 0;
        if (!f) { *(void **)&f = dlsym(RTLD_DEFAULT, "SLSSetPreventsActivation"); if (!f) *(void **)&f = dlsym(RTLD_DEFAULT, "CGSSetPreventsActivation"); }
        if (!f) return 0;
        R[3] = (unsigned int)f((int)R[3], (int)R[4], (int)R[5]);
        return 1;
    }
    if (!strcmp(nm, "CGSServerOperationState") && R[3] == 16 && !getenv("OAH_NO_STATUSBAR")) { R[3] = 1; return 1; }
    return 0;
}
static void oah_gen_trace(oah_gen_t *g, unsigned int *R) {
    unsigned long c = ++g->calls;
    g_trace_total++;
    oah_tel("stub", g->name);
    if (c <= 3 || (c & (c - 1)) == 0)
        fprintf(stderr, "[unbridged] %s #%lu r3=%08x r4=%08x r5=%08x r6=%08x lr=%08x -> 0\n",
                g->name, c, R[3], R[4], R[5], R[6], R[0x90 / 4]);
    if (c <= 2) {
        unsigned int sp = R[1], n;
        fprintf(stderr, "[unbridged]   bt:");
        for (n = 0; n < 12 && sp && !(sp & 3) && sp > 0x1000; n++) {
            unsigned int back = 0, ra = 0;
            if (!oah_bt_word(sp, &back) || !back || back <= sp || !oah_bt_word(back + 8, &ra)) break;
            fprintf(stderr, " %08x", ra);
            sp = back;
        }
        fprintf(stderr, "\n");
        if (getenv("OAH_BTNAMES")) {
            unsigned int b, lr = R[0x90 / 4];
            const char *nm = oah_guest_image_name(lr, &b), *sl = strrchr(nm, '/');
            fprintf(stderr, "[unbridged]   lr %s+0x%x\n", sl ? sl + 1 : nm, lr - b);
            sp = R[1];
            for (n = 0; n < 12 && sp && !(sp & 3) && sp > 0x1000; n++) {
                unsigned int back = 0, ra = 0;
                if (!oah_bt_word(sp, &back) || !back || back <= sp || !oah_bt_word(back + 8, &ra)) break;
                nm = oah_guest_image_name(ra, &b); sl = strrchr(nm, '/');
                fprintf(stderr, "[unbridged]     %s+0x%x\n", sl ? sl + 1 : nm, ra - b);
                sp = back;
            }
        }
        if (getenv("OAH_TRACE_ARGS")) {
            int k;
            for (k = 3; k <= 6; k++) {
                unsigned int v = R[k], w[4], p;
                char s[124];
                vm_size_t got = 0;
                if (v < 0x10000 || v >= 0xc0000000u) continue;
                if (vm_read_overwrite(mach_task_self(), v, 16, (vm_address_t)w, &got) || got != 16) continue;
                fprintf(stderr, "[unbridged]   r%d -> %08x %08x %08x %08x\n", k,
                        OSSwapBigToHostInt32(w[0]), OSSwapBigToHostInt32(w[1]),
                        OSSwapBigToHostInt32(w[2]), OSSwapBigToHostInt32(w[3]));
                p = OSSwapBigToHostInt32(w[2]);
                memset(s, 0, sizeof s);
                if (p >= 0x10000 && p < 0xc0000000u &&
                    !vm_read_overwrite(mach_task_self(), p, 120, (vm_address_t)s, &got))
                    fprintf(stderr, "[unbridged]   r%d str@+8: \"%.120s\"\n", k, s);
                memset(s, 0, sizeof s);
                if (!vm_read_overwrite(mach_task_self(), v + 9, 120, (vm_address_t)s, &got))
                    fprintf(stderr, "[unbridged]   r%d inline: \"%.120s\"\n", k, s);
                if (p > 0 && p < 2048) {   /* inline UTF-16BE CFString: length at +8, chars at +12 */
                    unsigned char u[4096];
                    char a[2049];
                    unsigned int j;
                    if (!vm_read_overwrite(mach_task_self(), v + 12, p * 2, (vm_address_t)u, &got)) {
                        for (j = 0; j < p; j++) a[j] = (u[2 * j] == 0 && u[2 * j + 1] >= 32 && u[2 * j + 1] < 127) ? u[2 * j + 1] : '.';
                        a[p] = 0;
                        fprintf(stderr, "[unbridged]   r%d utf16: \"%s\"\n", k, a);
                    }
                }
            }
        }
    }
    if (getenv("OAH_STUB_ABORT")) { fprintf(stderr, "[unbridged] OAH_STUB_ABORT set, exiting\n"); _exit(99); }
    R[3] = 0;
}
#define OAH_NFIX 6
#define OAH_MAXL 32
#include "oah_gl.h"
#include "oah_glx.h"
static unsigned int oah_callppc_n(const char *tag, unsigned int fn, int n, unsigned int *args);
#include "oah_snd.h"
#include "oah_apl.h"
static unsigned int oah_guest_sym(const char *lib, const char *sym);
#include "oah_qt.h"
static unsigned int oah_cf_to_guest(CFTypeRef o);
static void oah_clip_start(void);
static void oah_clip_service(void);
static void oah_ae_checkin(void);
static void oah_aen_dispatch(void);
static void oah_clip_resolve_main(void);
static int oah_aeout_call(const char *n, unsigned int *R);
#include "oah_cgs.h"
static int oah_userdb_call(const char *n, unsigned int *R);
static int oah_ls_call(const char *n, unsigned int *R);
static int oah_sfl_call(const char *n, unsigned int *R);
static int oah_notify_call(const char *n, unsigned int *R);
static int oah_task_call(const char *n, unsigned int *R);
#include <arpa/inet.h>
#include "oah_net.h"
#include "oah_cvl.h"
#include "oah_au.h"
#include "oah_auth.h"
static void oah_gen_call(unsigned int idx, unsigned int *R) {
    oah_gen_t *g = &g_gen[idx];
    oah_gli_trace(g->name, R);
    if (g->name[0] == 'a' && !strcmp(g->name, "abort") && !getenv("OAH_IGNORE_ABORT")) {
        /* the guest's abort() ends the process, as on 10.6. It used to be logged and ignored, and
           the guest ran on past its own fatal error: Adobe Bridge CS2, after "FLEXnet Activation
           toolkit runtime error 27", spun at 100% CPU with no window. OAH_IGNORE_ABORT=1: old way. */
        oah_gen_trace(g, R);
        fprintf(stderr, "[bridge] guest abort(): ending the process (OAH_IGNORE_ABORT=1 ignores it)\n");
        abort();
    }
    if (!g_au_thread) oah_au_pump();              /* refill output AudioUnits from guest render callbacks (oah_au.h) */
    if (g->name[0] == 'A' && g->name[1] == 'u' && (oah_auth_call(g->name, R) || oah_au_hal_call(g->name, R) || oah_au_unit_call(g->name, R))) return;
    if (g->name[0] == 'C' && g->name[1] == 'V' && oah_cvl_call(g->name, R)) return;   /* CVDisplayLink (oah_cvl.h) */
    if (g->name[0] == 'L' && g->name[2] == 'S' && oah_sfl_call(g->name, R)) return;   /* LSSharedFileList (oah_sfl.h) */
    if (g->name[0] == 'A' && g->name[1] == 'E' && oah_aeout_call(g->name, R)) return;   /* Apple Events out (oah_aen.h) */
    if (oah_glx_call(idx, g->name, R)) return;    /* generic GL, CGL, CGS surfaces (oah_glx.h) */
    if (oah_cgs_call(g->name, R)) return;         /* guest-toolbox mode: WindowServer bridge */
    if (oah_ls_call(g->name, R)) return;          /* process info from Mojave's LaunchServices (oah_ls.h) */
    oah_snd_pump();                               /* deliver Sound Manager callbacks (main thread only) */
    if (oah_gl_call(idx, g, R)) return;
    oah_apl_poll();                               /* APlayer "track done" (main thread only) */
    if (oah_snd_call(g->name, R)) return;
    if (oah_apl_call(g->name, R)) return;
    if (oah_qt_call(g->name, R)) return;          /* QuickTime movies -> native QuickTime 7 */
    oah_qt_note_port(g->name, R);
    double *F = (double *)((unsigned char *)R + 0x1e8);
    unsigned int a[12], na = 0, gr = 3, fr = 1, ret = 0, k, j;
    union { short s[4]; unsigned short u[4]; unsigned int w[2]; } fix[OAH_NFIX];
    unsigned int fixp[OAH_NFIX];
    char fixk[OAH_NFIX];
    unsigned int evl[OAH_MAXL][2];
    unsigned int nfix = 0;
    const char *p;
    if (!strcmp(g->name, "GetProcessBundleLocation")) { oah_get_process_bundle_location(R); return; }
    if (!strcmp(g->name, "GetNextProcess") && getenv("OAH_RLAUNCH_FWDPID") && R[3]) {
        /* Started by a launcher in forwarding mode (rlaunch passes events on to us): that launcher
           is still running and carries this app's signature and bundle id, so an app that looks for
           a copy of itself already running (Internet Explorer) found it and quit. Leave it out. */
        static int (*gnp)(unsigned int *); static int (*gpp)(const unsigned int *, pid_t *);
        unsigned int psn[2]; pid_t fp = (pid_t)atoi(getenv("OAH_RLAUNCH_FWDPID")), p = 0; int e;
        if (!gnp) { gnp = (int (*)(unsigned int *))dlsym(RTLD_DEFAULT, "GetNextProcess"); gpp = (int (*)(const unsigned int *, pid_t *))dlsym(RTLD_DEFAULT, "GetProcessPID"); }
        if (gnp && gpp) {
            psn[0] = OSSwapBigToHostInt32(((unsigned int *)R[3])[0]); psn[1] = OSSwapBigToHostInt32(((unsigned int *)R[3])[1]);
            do { e = gnp(psn); } while (!e && !gpp(psn, &p) && p == fp);
            ((unsigned int *)R[3])[0] = OSSwapHostToBigInt32(psn[0]); ((unsigned int *)R[3])[1] = OSSwapHostToBigInt32(psn[1]);
            R[3] = (unsigned int)e;
            return;
        }
    }
    if (!strcmp(g->name, "CGGetActiveDisplayList")) { oah_cg_get_active_display_list(R); return; }
    if (!strcmp(g->name, "FSGetVolumeInfo")) { oah_fs_get_volume_info(R); return; }
    if (!strcmp(g->name, "FSFindFolder")) { oah_fs_find_folder(R); return; }
    if (oah_userdb_call(g->name, R)) return;
    if (oah_notify_call(g->name, R)) return;
    if (oah_task_call(g->name, R)) return;
    if (oah_net_call(g->name, R)) return;
    if (!strcmp(g->name, "GetNewCWindow")) { oah_get_new_cwindow(R); return; }
    if (!strcmp(g->name, "GetWindowGreatestAreaDevice") || !strcmp(g->name, "GetAvailableWindowPositioningBounds")) {
        /* (WindowRef, WindowRegionCode, GDHandle *outGD, Rect *outRect) -> OSStatus
           (GDHandle, Rect *outRect) -> OSStatus.  GDHandles go through the shadow table. */
        int gw = g->name[3] == 'W';
        unsigned int gdout = gw ? R[5] : 0, rout = gw ? R[6] : R[4], native_gd = 0, k;
        short r[4] = { 0, 0, 0, 0 };
        int st;
        if (!g->fn && !g_carbon_handle)
            g_carbon_handle = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon", RTLD_LAZY | RTLD_GLOBAL);
        if (!g->fn && g_carbon_handle) g->fn = dlsym(g_carbon_handle, g->name);
        if (!g->fn) { R[3] = (unsigned int)-50; return; }
        if (gw) st = ((int (*)(void *, unsigned short, unsigned int *, short *))g->fn)((void *)R[3], (unsigned short)R[4], &native_gd, r);
        else st = ((int (*)(unsigned int, short *))g->fn)(oah_native_handle(R[3]), r);
        if (gdout) *(unsigned int *)gdout = OSSwapHostToBigInt32(oah_gd_shadow(native_gd));
        if (rout) for (k = 0; k < 4; k++) ((unsigned short *)rout)[k] = OSSwapHostToBigInt16((unsigned short)r[k]);
        if (getenv("OAH_VERBOSE"))
            fprintf(stderr, "[bridge] %s -> %d gd=%08x rect=(%d,%d,%d,%d)\n", g->name, st, native_gd, r[0], r[1], r[2], r[3]);
        R[3] = (unsigned int)st;
        return;
    }
    if (!strcmp(g->name, "LSFindApplicationForInfo")) {
        /* The guest LaunchServices cannot read Mojave's LS database (it crashes
           in _LSContextInit), so report "no application found". */
        if (getenv("OAH_VERBOSE"))
            fprintf(stderr, "[bridge] LSFindApplicationForInfo creator=%08x bundleID=%08x name=%08x -> kLSApplicationNotFoundErr\n",
                    R[3], R[4], R[5]);
        if (R[7] && R[7] < 0xc0000000u) *(unsigned int *)R[7] = 0;
        R[3] = (unsigned int)-10814;
        return;
    }
    if (!strcmp(g->name, "__LSContextInit")) {
        /* Every guest LaunchServices call that needs the database starts here.
           Mojave's database cannot be read by the 10.6 code, so fail cleanly. */
        if (getenv("OAH_VERBOSE")) fprintf(stderr, "[bridge] _LSContextInit lr=%08x -> kLSServerCommunicationErr\n", R[0x90 / 4]);
        R[3] = (unsigned int)-10822;
        return;
    }
    if (!strcmp(g->name, "DRCopyDeviceArray")) {
        /* Guest DiscRecording starts an IOKit worker that crashes on Mojave's
           replies; report no burner/drive devices instead (empty guest CFArray). */
        R[3] = oah_guest_empty_array();
        if (getenv("OAH_VERBOSE")) fprintf(stderr, "[bridge] DRCopyDeviceArray -> empty guest array 0x%08x\n", R[3]);
        return;
    }
    if (!strcmp(g->name, "CGDisplayCurrentMode")) { oah_cg_display_modes(R, 0); return; }
    if (!strcmp(g->name, "CGDisplayBestModeForParameters")) { oah_cg_best_mode(R, 0); return; }
    if (!strcmp(g->name, "CGDisplayBestModeForParametersAndRefreshRate")) { oah_cg_best_mode(R, 1); return; }
    if (!strcmp(g->name, "CGDisplaySwitchToMode")) { oah_cg_switch_mode(R); return; }
    if (!strcmp(g->name, "CGDisplayAvailableModes")) { oah_cg_display_modes(R, 1); return; }
    if (g->sig[0] == 'T' && g->name[0] == 'C' && oah_sb_misc_bridge(g->name, R)) return;
    if (g->sig[0] == 'T') { oah_gen_trace(g, R); return; }
    memset(a, 0, sizeof a);
    if (!oah_gen_resolve(g)) { R[3] = 0xffffffffu; return; }
    if (!g_fg_done && !strcmp(g->name, "CreateNewWindow")) { g_fg_done = 1; oah_make_foreground(); }
    if (!strcmp(g->name, "DMGetNextScreenDevice")) R[3] = oah_native_handle(R[3]);
    for (p = g->sig + 2; *p; p++) {
        unsigned int v;
        if (na >= 11) { fprintf(stderr, "[bridge/gen] %s: too many args\n", g->name); R[3] = 0xffffffffu; return; }
        if (*p == 'f') {
            float f = (float)F[fr++];
            gr += 1;
            memcpy(&a[na], &f, 4);
            na += 1;
            continue;
        }
        if (*p == 'd') {
            double d = F[fr++];
            gr += 2;
            memcpy(&a[na], &d, 8);
            na += 2;
            continue;
        }
        if (*p == 'C' || *p == 'G') {
            unsigned int n = *p == 'C' ? 4 : 2;
            if (gr + n - 1 > 10) { fprintf(stderr, "[bridge/gen] %s: stack args not supported\n", g->name); R[3] = 0xffffffffu; return; }
            for (k = 0; k < n; k++) a[na++] = R[gr++];
            continue;
        }
        if (gr > 10) { fprintf(stderr, "[bridge/gen] %s: stack args not supported\n", g->name); R[3] = 0xffffffffu; return; }
        v = R[gr++];
        if (*p == 'L') {
            unsigned int cnt = na ? a[na - 1] : 0;
            if (v && cnt) {
                if (cnt > OAH_MAXL) { fprintf(stderr, "[bridge/gen] %s: %u event types (max %d)\n", g->name, cnt, OAH_MAXL); cnt = OAH_MAXL; }
                for (k = 0; k < cnt; k++) {
                    evl[k][0] = OSSwapBigToHostInt32(((unsigned int *)v)[2 * k]);
                    evl[k][1] = OSSwapBigToHostInt32(((unsigned int *)v)[2 * k + 1]);
                }
                a[na++] = (unsigned int)evl;
            } else a[na++] = v;
            continue;
        }
        if (*p == 'H') { a[na++] = oah_native_handle(v); continue; }
        if (*p == 'i' || !v || nfix >= OAH_NFIX) {
            if (*p != 'i' && v) fprintf(stderr, "[bridge/gen] %s: too many converted pointers\n", g->name);
            a[na++] = v;
            continue;
        }
        memset(&fix[nfix], 0, sizeof fix[nfix]);
        switch (*p) {
        case 'R': case 'X':
            for (k = 0; k < 4; k++) fix[nfix].s[k] = (short)OSSwapBigToHostInt16(((unsigned short *)v)[k]);
            break;
        case 'p':
            for (k = 0; k < 2; k++) fix[nfix].s[k] = (short)OSSwapBigToHostInt16(((unsigned short *)v)[k]);
            break;
        case 'K':
            for (k = 0; k < 3; k++) fix[nfix].u[k] = OSSwapBigToHostInt16(((unsigned short *)v)[k]);
            break;
        case 'O':
            fix[nfix].w[0] = OSSwapBigToHostInt32(*(unsigned int *)v);
            break;
        case 'N':
            for (k = 0; k < 2; k++) fix[nfix].w[k] = OSSwapBigToHostInt32(((unsigned int *)v)[k]);
            break;
        default: /* r, o: output only */
            break;
        }
        fixp[nfix] = v; fixk[nfix] = *p;
        a[na++] = (unsigned int)&fix[nfix];
        nfix++;
    }
    if (g->sig[0] == 'D')
        F[1] = ((oah_gen_dbl_t)g->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
    else
        ret = ((oah_gen_int_t)g->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
    for (k = 0; k < nfix; k++) {
        unsigned int v = fixp[k];
        switch (fixk[k]) {
        case 'r': case 'X':
            for (j = 0; j < 4; j++) ((unsigned short *)v)[j] = OSSwapHostToBigInt16((unsigned short)fix[k].s[j]);
            break;
        case 'p':
            for (j = 0; j < 2; j++) ((unsigned short *)v)[j] = OSSwapHostToBigInt16((unsigned short)fix[k].s[j]);
            break;
        case 'o': case 'O':
            *(unsigned int *)v = OSSwapHostToBigInt32(fix[k].w[0]);
            break;
        case 'N':
            for (j = 0; j < 2; j++) ((unsigned int *)v)[j] = OSSwapHostToBigInt32(fix[k].w[j]);
            break;
        default:
            break;
        }
        /* functions like GetPortBounds return their output pointer: hand the guest its own */
        if (ret == (unsigned int)&fix[k]) ret = v;
    }
    if (!strcmp(g->name, "DMGetFirstScreenDevice") || !strcmp(g->name, "DMGetNextScreenDevice") ||
        !strcmp(g->name, "GetMainDevice"))
        ret = oah_gd_shadow(ret);
    switch (g->sig[0]) {
    case 'i': R[3] = ret; break;
    case 'H': R[3] = oah_shadow_handle(ret); break;
    case 'h': R[3] = (unsigned int)(int)(short)ret; break;
    case 'b': R[3] = ret & 0xff; break;
    default: break;
    }
    if (getenv("OAH_VERBOSE") && g->calls < 2)
        fprintf(stderr, "[bridge/gen] %s(%08x %08x %08x %08x) -> %08x\n", g->name, a[0], a[1], a[2], a[3], R[3]);
    g->calls++;
}

/* ===== Carbon events: native -> PowerPC handler trampolines (IDs 8..12) =====
   On Mach-O PPC a UPP is just the PPC function address. We install a native
   trampoline with Mojave's Carbon whose userData points at {ppc_fn, ppc_ud};
   when Mojave dispatches, the trampoline runs the PPC function through
   CallPPCFunctionAtAddressInt. The PPC code must first register a return
   stub ("twi 31,r0,5; blr") with set_retstub (ID 8). EventRefs, targets and
   handler refs are native objects passed through unchanged (sealed). */
typedef struct { unsigned int fn, ud; } oah_ppccb_t;
static unsigned int g_retstub;
static unsigned long g_evt_calls, g_timer_calls;
static void *oah_csym(const char *n) {
    if (!g_carbon_handle)
        g_carbon_handle = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon", RTLD_LAZY | RTLD_GLOBAL);
    return g_carbon_handle ? dlsym(g_carbon_handle, n) : 0;
}
/* Read two words without faulting: some regions report VM_PROT_READ but have
   pages that are not actually mapped (seen at 0xa7d32000 on Mojave). */
/* Image scans walk guest memory page by page looking for Mach-O headers. Reading a page of
   anonymous memory makes the kernel give it a real zero page, so a scan of the translator's
   26 MB code blocks, its 4 MB tables and every malloc region made all of them resident: about
   55 MB per process before an app ran any code (a 26 MB block showed 24 MB in use for a program
   that had translated a few hundred KB). Images are always mapped from files, so only
   file-backed regions are read. OAH_SCAN_ANON=1 scans everything as before. */
static int oah_scan_region(vm_address_t a) {
    static int all = -1;
    struct proc_regionwithpathinfo rp;
    if (all < 0) all = getenv("OAH_SCAN_ANON") ? 1 : 0;
    if (all) return 1;
    return proc_pidinfo(getpid(), PROC_PIDREGIONPATHINFO, a, &rp, sizeof rp) == (int)sizeof rp &&
           rp.prp_prinfo.pri_address == a && rp.prp_vip.vip_path[0] != 0;
}
static int oah_peek2(vm_address_t p, unsigned int *w) {
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), p, 32, (vm_address_t)w, &got) == KERN_SUCCESS && got == 32;
}
/* Library images are slid by the guest dyld, so find the stub at run time:
   scan readable memory for a big-endian PPC Mach-O header (0xfeedface) whose
   image holds "twi 31,r0,5 ; blr" at the given offset. */
static unsigned int oah_find_lib_stub(unsigned int off) {
    vm_address_t addr = 0x1000;
    unsigned int want0 = OSSwapHostToBigInt32(0x0FE00005u), want1 = OSSwapHostToBigInt32(0x4E800020u);
    unsigned int scanned = 0;
    for (;;) {
        vm_size_t size = 0;
        vm_region_basic_info_data_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT;
        mach_port_t obj = MACH_PORT_NULL;
        vm_address_t p;
        if (vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
            break;
        if (addr >= 0xb0000000u) break;              /* guest images live below Rosetta */
        if (info.protection & VM_PROT_READ) {
            if (oah_scan_region(addr)) for (p = addr; p < addr + size; p += 4096) {
                unsigned int hb[8]; const unsigned int *h = hb; if (!oah_peek2(p, hb)) continue;
                scanned++;
                if (h[0] == OSSwapHostToBigInt32(0xfeedfaceu) && h[1] == OSSwapHostToBigInt32(18u)) {
                    unsigned int s = (unsigned int)p + off;
                    vm_address_t ra = s & ~0xfffu; vm_size_t rs = 0;
                    vm_region_basic_info_data_t ri; mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT; mach_port_t ro;
                    if (vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO, (vm_region_info_t)&ri, &rc, &ro) == KERN_SUCCESS &&
                        ra <= s && s + 8 <= ra + rs && (ri.protection & VM_PROT_READ) &&
                        *(const unsigned int *)s == want0 && *(const unsigned int *)(s + 4) == want1) {
                        if (getenv("OAH_VERBOSE"))
                            fprintf(stderr, "[evt] found library stub: image 0x%08x + 0x%x = 0x%08x (%u pages scanned)\n",
                                    (unsigned int)p, off, s, scanned);
                        return s;
                    }
                }
            }
        }
        addr += size;
    }
    fprintf(stderr, "[evt] library stub at +0x%x not found (%u pages scanned)\n", off, scanned);
    return 0;
}
/* Guest stub for oah_callppc_n: load the target from g_align_target, align r1 to 16, make a
   64-byte frame (back chain = the old r1), call, restore. Lives in the zero padding after
   CarbonCore's load commands (inside its __TEXT, where the translator keeps running code). */
static volatile unsigned int g_align_target;             /* big-endian guest address */
/* Big-endian guest stack pointer the stub switches to (0: stay on the translator's callback stack).
   On the main thread, a guest call made while the guest is stopped in a bridge on its own stack
   runs just below that stack's r1, as the same code ran on 10.6 (an Apple Event handler, a
   callback): code that checks its stack against the thread's range (REALbasic's runtime,
   AppleScript's StackSpace) saw the translator's callback stack, far below the main thread's
   range. The stub's frame links back to the callback stack, so the return is unchanged.
   Mactracker (REALbasic) span forever after a reopen: its RuntimeStackCheck compared r1 with the
   main thread's stack, saw the callback stack, raised StackOverflowException, and its handler did
   the same again. The guest's r1 is taken at each bridge entry (g_main_sp; a guest call puts the
   value back when it returns, since the return - ret_stub - arrives with the callback stack's r1):
   only when it is on the main thread's own stack does the call move there.
   OAH_NO_MAINSTACKCB=1 keeps every call on the callback stack. */
static volatile unsigned int g_align_sp;
static unsigned int oah_align_stub(void) {
    static unsigned int stub; static int tried;
    unsigned int sym, base = 0, ncmd, st, i, k, *w, code[19];
    if (stub || tried) return stub;
    tried = 1;
    if (getenv("OAH_NO_ALIGNCB")) return 0;
    {   unsigned int a = (unsigned int)&g_align_target, ha = (a + 0x8000) >> 16, lo = a & 0xffff;
        unsigned int s = (unsigned int)&g_align_sp, sha = (s + 0x8000) >> 16, slo = s & 0xffff;
        unsigned int c[19] = {
            0x3D800000u | ha,          /* lis   r12,ha(slot) */
            0x818C0000u | lo,          /* lwz   r12,lo(slot)(r12) */
            0x7D8903A6u,               /* mtctr r12 */
            0x7C0802A6u,               /* mflr  r0 */
            0x7C2C0B78u,               /* mr    r12,r1 */
            0x3D600000u | sha,         /* lis   r11,ha(spslot) */
            0x816B0000u | slo,         /* lwz   r11,lo(spslot)(r11) */
            0x2C0B0000u,               /* cmpwi r11,0 */
            0x41820008u,               /* beq   +8 */
            0x7D615B78u,               /* mr    r1,r11 */
            0x54210036u,               /* clrrwi r1,r1,4 */
            0x9581FFC0u,               /* stwu  r12,-64(r1) */
            0x9001000Cu,               /* stw   r0,12(r1)   (reserved linkage word) */
            0x7D8902A6u,               /* mfctr r12 (callees may expect r12 = their address) */
            0x4E800421u,               /* bctrl */
            0x8001000Cu,               /* lwz   r0,12(r1) */
            0x7C0803A6u,               /* mtlr  r0 */
            0x80210000u,               /* lwz   r1,0(r1) */
            0x4E800020u };             /* blr */
        memcpy(code, c, sizeof c); }
    sym = oah_guest_sym("CarbonCore", "_FSPathMakeRef");
    if (sym) oah_guest_image_name(sym, &base);
    if (!base) return 0;
    ncmd = OSSwapBigToHostInt32(((unsigned int *)base)[5]);
    for (st = (base + 28 + ncmd + 15) & ~15u; st + 4 * 16 <= base + 4096; st += 16) {
        w = (unsigned int *)st;
        if (st + 4 * 22 > base + 4096) break;
        for (k = 0; k < 22 && !w[k]; k++) ;               /* 19 words + a gap from any neighbour */
        if (k < 22) continue;
        if (vm_protect(mach_task_self(), base, 4096, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE | VM_PROT_COPY) != KERN_SUCCESS &&
            vm_protect(mach_task_self(), base, 4096, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE) != KERN_SUCCESS) return 0;
        for (i = 0; i < 19; i++) w[1 + i] = OSSwapHostToBigInt32(code[i]);
        vm_protect(mach_task_self(), base, 4096, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
        stub = st + 4;
        if (getenv("OAH_VERBOSE")) fprintf(stderr, "[evt] aligned-stack call stub at 0x%08x\n", stub);
        return stub;
    }
    return 0;
}
static unsigned int oah_callppc_n(const char *tag, unsigned int fn, int n, unsigned int *args) {
    unsigned int r;
    if (!g_retstub) {
        /* Real apps never call set_retstub: use a stub patched into a system
           library (bridgepatch.py, e.g. HIToolbox DebugPrintMenu=5). Its address
           comes from OAH_RETSTUB and must hold "twi 31,r0,5" (big-endian). */
        const char *e = getenv("OAH_RETSTUB");
        unsigned int a = e ? (unsigned int)strtoul(e, 0, 16) : 0;
        const char *eo = getenv("OAH_RETSTUB_OFF");
        static int scanned_once;
        if (!a && eo && !scanned_once) { scanned_once = 1; a = oah_find_lib_stub((unsigned int)strtoul(eo, 0, 16)); }
        if (!a) {
            /* Daemons (fontd): the translator only runs guest code inside a
               loaded image's segment for long (a stray mmap page works until
               its code-region cache is recycled, then it exits(1)). Put the
               stub in the zero padding after CarbonCore's load commands,
               which lies inside its __TEXT segment. */
            unsigned int sym = oah_guest_sym("CarbonCore", "_FSPathMakeRef"), base = 0;
            if (!sym) sym = oah_guest_sym("libSystem", "_malloc");
            if (sym) oah_guest_image_name(sym, &base);
            if (base) {
                unsigned int ncmd = OSSwapBigToHostInt32(((unsigned int *)base)[5]);
                unsigned int st = (base + 28 + ncmd + 15) & ~7u, *w = (unsigned int *)st;
                if (st + 16 < base + 4096 && !w[0] && !w[1] && !w[2] && !w[3] &&
                    (vm_protect(mach_task_self(), base, 4096, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE | VM_PROT_COPY) == KERN_SUCCESS ||
                     vm_protect(mach_task_self(), base, 4096, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE) == KERN_SUCCESS)) {
                    w[0] = OSSwapHostToBigInt32(0x0FE00005u);
                    w[1] = OSSwapHostToBigInt32(0x4E800020u);
                    vm_protect(mach_task_self(), base, 4096, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
                    a = st;
                    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[evt] built image return stub at 0x%08x (image 0x%08x)\n", a, base);
                }
            }
        }
        if (!a) {
            /* Processes that never load HIToolbox (daemons such as fontd): build
               the stub in a page of our own; the translator runs guest code from
               any readable, executable memory. */
            unsigned int *pg = (unsigned int *)mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
            if (pg != (unsigned int *)MAP_FAILED) {
                pg[0] = OSSwapHostToBigInt32(0x0FE00005u);   /* twi 31,r0,5 */
                pg[1] = OSSwapHostToBigInt32(0x4E800020u);   /* blr */
                a = (unsigned int)pg;
                if (getenv("OAH_VERBOSE")) fprintf(stderr, "[evt] built private return stub at 0x%08x\n", a);
            }
        }
        if (a && *(volatile unsigned int *)a == OSSwapHostToBigInt32(0x0FE00005u)) {
            g_retstub = a;
            if (getenv("OAH_VERBOSE")) fprintf(stderr, "[evt] using library return stub at 0x%08x\n", a);
        } else {
            fprintf(stderr, "[evt] no return stub (OAH_RETSTUB=%s, word=%08x); cannot call PPC 0x%08x\n",
                    e ? e : "unset", a ? OSSwapBigToHostInt32(*(volatile unsigned int *)a) : 0, fn);
            return 0xffffd96eu;
        }
    }
    oah_load_callppc();
    g_cb_depth++;
    oah_callppc_lock();
    g_last_cp_tag = tag; g_last_cp_fn = fn;
    g_cb_owner = pthread_self(); g_cb_owner_depth++;     /* this thread runs guest code on a callback stack */
    {   /* The translator starts a callback's guest stack at page top - 0x264, so r1 is 12 mod 16
           and every AltiVec stvx/lvx to a stack slot (rounded down to 16) hits the neighbouring
           object: TextWrangler's _GetDocumentByFSRef zeroed its StCriticalRegion and crashed on
           odoc. Enter the target through a guest stub that 16-aligns r1 first (register
           arguments only). OAH_NO_ALIGNCB=1 calls the target directly. */
        unsigned int stub = n <= 8 ? oah_align_stub() : 0;
        if (stub) {
            unsigned int saved = g_align_target, saved_sp = g_align_sp, nsp = 0;
            static int mainstack = -1;
            if (mainstack < 0) mainstack = getenv("OAH_NO_MAINSTACKCB") ? 0 : 1;
            if (mainstack && pthread_main_np() && g_main_sp) {
                unsigned int gsp = g_main_sp;
                /* on the main thread's own stack (8 MB below 0xc0000000), with room left: 2 KB under
                   the stopped guest's r1 (its red zone and the bridge stub stay untouched) */
                if (gsp < 0xc0000000u && gsp > 0xbf800000u + (256u << 10)) nsp = gsp - 2048;
                if (getenv("OAH_CBSTACKTRACE")) fprintf(stderr, "[cbstack] %s: guest r1 %08x -> %s %08x\n", tag, gsp, nsp ? "main stack" : "callback stack", nsp);
            }
            g_align_sp = nsp ? OSSwapHostToBigInt32(nsp) : 0;
            g_align_target = OSSwapHostToBigInt32(fn);
            {   unsigned int msp = g_main_sp;          /* back in the same stop afterwards */
                r = g_callppc(tag, stub, g_retstub, n, args);
                if (pthread_main_np()) g_main_sp = msp; }
            g_align_target = saved;
            g_align_sp = saved_sp;
        } else {
            unsigned int msp = g_main_sp;
            r = g_callppc(tag, fn, g_retstub, n, args);
            if (pthread_main_np()) g_main_sp = msp;
        }
    }
    g_cb_owner_depth--;
    oah_callppc_unlock();
    g_cb_depth--;
    g_cb_calls++;
    return r;
}
/* Find an exported symbol in a guest (PowerPC) dylib that the guest dyld has
   mapped, by scanning for its Mach-O header and walking LC_SYMTAB in memory.
   lib is a substring of the install name (LC_ID_DYLIB). Returns 0 if absent. */
static int oah_readable(vm_address_t a, vm_size_t n) {
    vm_address_t ra = a; vm_size_t rs = 0;
    vm_region_basic_info_data_t ri; mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT; mach_port_t ro;
    if (vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO, (vm_region_info_t)&ri, &rc, &ro) != KERN_SUCCESS) return 0;
    return ra <= a && a + n <= ra + rs && (ri.protection & VM_PROT_READ);
}
#define BE32(x) OSSwapBigToHostInt32(x)
static unsigned int oah_guest_sym_in(unsigned int p, const char *lib, const char *sym) {
    unsigned int hb[8]; const unsigned int *h = hb; if (!oah_peek2(p, hb)) return 0;
    unsigned int ncmds = BE32(h[4]), sizeof_cmds = BE32(h[5]), i, off = 28;
    unsigned int text_vm = 0, le_vm = 0, le_fo = 0, symoff = 0, nsyms = 0, stroff = 0, slide, le_base;
    int have_le = 0, have_sym = 0, name_ok = 0;
    if (!oah_readable(p, 28 + sizeof_cmds)) return 0;
    for (i = 0; i < ncmds && off + 8 <= 28 + sizeof_cmds; i++) {
        const unsigned int *lc = (const unsigned int *)(p + off);
        unsigned int cmd = BE32(lc[0]), sz = BE32(lc[1]);
        if (sz < 8) break;
        if (cmd == 0xd) {                                  /* LC_ID_DYLIB */
            const char *nm = (const char *)lc + BE32(lc[2]);
            if (strstr(nm, lib)) name_ok = 1;
        } else if (cmd == 0x1) {                           /* LC_SEGMENT */
            const char *sn = (const char *)(lc + 2);
            if (!strncmp(sn, "__TEXT", 16)) text_vm = BE32(lc[6]);
            if (!strncmp(sn, "__LINKEDIT", 16)) { le_vm = BE32(lc[6]); le_fo = BE32(lc[8]); have_le = 1; }
        } else if (cmd == 0x2) {                           /* LC_SYMTAB */
            symoff = BE32(lc[2]); nsyms = BE32(lc[3]); stroff = BE32(lc[4]); have_sym = 1;
        }
        off += sz;
    }
    if (!name_ok || !have_le || !have_sym) return 0;
    slide = p - text_vm;
    le_base = le_vm + slide - le_fo;
    if (!oah_readable(le_base + symoff, nsyms * 12)) {
        if (getenv("OAH_GSYM_DEBUG")) fprintf(stderr, "[gsym] %s: symtab at %08x (%u syms) not readable (hdr %08x le_vm %08x fo %08x)\n", lib, le_base + symoff, nsyms, p, le_vm, le_fo);
        return 0;
    }
    for (i = 0; i < nsyms; i++) {
        const unsigned char *n = (const unsigned char *)(le_base + symoff + i * 12);
        unsigned int strx = BE32(*(const unsigned int *)n);
        unsigned char type = n[4];
        if ((type & 0xe0) || (type & 0x0e) != 0x0e) continue;
        if (!strcmp((const char *)(le_base + stroff + strx), sym))
            return BE32(*(const unsigned int *)(n + 8)) + slide;
    }
    if (getenv("OAH_GSYM_DEBUG")) fprintf(stderr, "[gsym] %s: %s not among %u symbols (hdr %08x)\n", lib, sym, nsyms, p);
    return 0;
}
/* Every defined section symbol of the guest image whose header is at p: fn(name, address). */
static int oah_guest_syms_each(unsigned int p, void (*fn)(const char *, unsigned int, void *), void *ctx) {
    unsigned int hb[8]; const unsigned int *h = hb; if (!oah_peek2(p, hb)) return 0;
    unsigned int ncmds = BE32(h[4]), sizeof_cmds = BE32(h[5]), i, off = 28, n = 0;
    unsigned int text_vm = 0, le_vm = 0, le_fo = 0, symoff = 0, nsyms = 0, stroff = 0, slide, le_base;
    int have_le = 0, have_sym = 0;
    if (!oah_readable(p, 28 + sizeof_cmds)) return 0;
    for (i = 0; i < ncmds && off + 8 <= 28 + sizeof_cmds; i++) {
        const unsigned int *lc = (const unsigned int *)(p + off);
        unsigned int cmd = BE32(lc[0]), sz = BE32(lc[1]);
        if (sz < 8) break;
        if (cmd == 0x1) {
            const char *sn = (const char *)(lc + 2);
            if (!strncmp(sn, "__TEXT", 16)) text_vm = BE32(lc[6]);
            if (!strncmp(sn, "__LINKEDIT", 16)) { le_vm = BE32(lc[6]); le_fo = BE32(lc[8]); have_le = 1; }
        } else if (cmd == 0x2) { symoff = BE32(lc[2]); nsyms = BE32(lc[3]); stroff = BE32(lc[4]); have_sym = 1; }
        off += sz;
    }
    if (!have_le || !have_sym) return 0;
    slide = p - text_vm;
    le_base = le_vm + slide - le_fo;
    if (!oah_readable(le_base + symoff, nsyms * 12)) return 0;
    for (i = 0; i < nsyms; i++) {
        const unsigned char *s = (const unsigned char *)(le_base + symoff + i * 12);
        if ((s[4] & 0xe0) || (s[4] & 0x0e) != 0x0e) continue;
        fn((const char *)(le_base + stroff + BE32(*(const unsigned int *)s)), BE32(*(const unsigned int *)(s + 8)) + slide, ctx);
        n++;
    }
    return (int)n;
}
/* Name of the guest image whose Mach-O header is the closest one at or below
   addr (good enough for frameworks; the main executable has no LC_ID_DYLIB). */
static const char *oah_guest_image_name(unsigned int addr, unsigned int *base_out);
/* The scan reads every page below addr (one vm_read each): ~0.1-1 s per call. The heartbeat
   names 7 frames a second with it, which alone kept a core 40-100% busy in every app. Cache the
   image headers (full scan at most every 20 s; a miss past the newest cached image rescans). */
static struct { unsigned int base; char name[160]; } g_gin[1024];   /* names copied: images can unload */
static int g_gin_n, g_gin_filling; static time_t g_gin_t;
static pthread_mutex_t g_gin_mu = PTHREAD_MUTEX_INITIALIZER;
static void oah_gin_add(unsigned int base, const char *name) {
    if (g_gin_filling && g_gin_n < 1024) { g_gin[g_gin_n].base = base; strlcpy(g_gin[g_gin_n].name, name, sizeof g_gin[0].name); g_gin_n++; }
}
static const char *oah_guest_image_name_fast(unsigned int addr, unsigned int *base_out) {
    unsigned int best = 0; const char *bn = "?"; int i;
    if (addr >= 0x90000000u) { if (base_out) *base_out = 0; return "?"; }
    pthread_mutex_lock(&g_gin_mu);
    if (!g_gin_t || time(0) - g_gin_t > 60) {
        g_gin_n = 0; g_gin_t = time(0);
        g_gin_filling = 1; oah_guest_image_name(0x8fffffffu, 0); g_gin_filling = 0;   /* exact scan fills g_gin */
    }
    for (i = 0; i < g_gin_n; i++) if (g_gin[i].base <= addr && g_gin[i].base >= best) { best = g_gin[i].base; bn = g_gin[i].name; }
    pthread_mutex_unlock(&g_gin_mu);
    if (base_out) *base_out = best;
    return bn;
}
static const char *oah_guest_image_name(unsigned int addr, unsigned int *base_out) {
    vm_address_t a = 0x1000;
    unsigned int best = 0;
    const char *bestname = "?";
    if (addr >= 0x90000000u) return "?";
    for (;;) {
        vm_size_t size = 0;
        vm_region_basic_info_data_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT;
        mach_port_t obj = MACH_PORT_NULL;
        vm_address_t p;
        if (vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
            break;
        if (a > addr || a >= 0xb0000000u) break;
        if (info.protection & VM_PROT_READ) {
            if (oah_scan_region(a)) for (p = a; p < a + size && p <= addr; p += 4096) {
                unsigned int hb[8]; const unsigned int *h = hb; if (!oah_peek2(p, hb)) continue;
                if (h[0] == OSSwapHostToBigInt32(0xfeedfaceu) && h[1] == OSSwapHostToBigInt32(18u)) {
                    /* The load commands are copied with vm_read first: this runs on the hang
                       watcher's thread too, and an image being mapped or unmapped meanwhile
                       (PowerPoint at launch) made a direct read fault past its region. */
                    unsigned int ncmds = BE32(h[4]), sz = BE32(h[5]), off = 28, i;
                    unsigned char lcb[8192]; vm_size_t got = 0; const char *lname = 0;
                    best = p; bestname = "(main executable or bundle)";
                    if (sz > sizeof lcb - 28) sz = sizeof lcb - 28;
                    if (vm_read_overwrite(mach_task_self(), p, 28 + sz, (vm_address_t)lcb, &got) != KERN_SUCCESS || got != 28 + sz)
                        ncmds = 0;
                    for (i = 0; i < ncmds && off + 12 <= 28 + sz; i++) {
                        const unsigned int *lc = (const unsigned int *)(lcb + off);
                        unsigned int csz = BE32(lc[1]);
                        if (BE32(lc[0]) == 0xd) {
                            unsigned int no = BE32(lc[2]);
                            if (no < csz && off + csz <= 28 + sz && memchr(lcb + off + no, 0, csz - no))
                                { bestname = (const char *)(p + off + no); lname = (const char *)lcb + off + no; }   /* mapped: it was just read */
                            break;
                        }
                        if (csz < 8) break;
                        off += csz;
                    }
                    oah_gin_add((unsigned int)p, lname ? lname : bestname);   /* the cache keeps a copy */
                }
            }
        }
        a += size;
    }
    if (base_out) *base_out = best;
    return bestname;
}
static unsigned int oah_guest_sym_q(const char *lib, const char *sym, int quiet);
static unsigned int oah_guest_sym(const char *lib, const char *sym) { return oah_guest_sym_q(lib, sym, 0); }
/* Each lookup used to walk the whole guest address space (one read per 4 KB page) until the image
   was found: 0.1-1 s a symbol. Keynote's launch spent ~3 s in it on the main thread (the first guest
   CFString made by oah_cf_to_guest loads a dozen CoreFoundation symbols). The image a library's
   lookups were answered from is remembered and tried first; a miss there still scans everything.
   OAH_NO_GSYMCACHE=1 always scans. */
#define OAH_GSB 128
static struct { char lib[64]; unsigned int base; } g_gsb[OAH_GSB];
static int g_gsb_n;
static pthread_mutex_t g_gsb_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned int oah_gsb_get(const char *lib) {
    unsigned int b = 0; int i;
    pthread_mutex_lock(&g_gsb_mu);
    for (i = 0; i < g_gsb_n; i++) if (!strcmp(g_gsb[i].lib, lib)) { b = g_gsb[i].base; break; }
    pthread_mutex_unlock(&g_gsb_mu);
    return b;
}
static void oah_gsb_put(const char *lib, unsigned int base) {
    int i;
    if (strlen(lib) >= sizeof g_gsb[0].lib) return;
    pthread_mutex_lock(&g_gsb_mu);
    for (i = 0; i < g_gsb_n && strcmp(g_gsb[i].lib, lib); i++) ;
    if (i < g_gsb_n) g_gsb[i].base = base;
    else if (g_gsb_n < OAH_GSB) { strcpy(g_gsb[g_gsb_n].lib, lib); g_gsb[g_gsb_n].base = base; g_gsb_n++; }
    pthread_mutex_unlock(&g_gsb_mu);
}
static unsigned int oah_guest_sym_q(const char *lib, const char *sym, int quiet) {
    vm_address_t addr = 0x1000;
    static int nocache = -1;
    if (nocache < 0) nocache = getenv("OAH_NO_GSYMCACHE") != 0;
    if (!nocache) {
        unsigned int b = oah_gsb_get(lib), hb[8];
        if (b && oah_peek2(b, hb) && hb[0] == OSSwapHostToBigInt32(0xfeedfaceu)) {
            unsigned int a = oah_guest_sym_in(b, lib, sym);
            if (a) return a;
        }
    }
    for (;;) {
        vm_size_t size = 0;
        vm_region_basic_info_data_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT;
        mach_port_t obj = MACH_PORT_NULL;
        vm_address_t p;
        if (vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
            break;
        if (addr >= 0xb0000000u) break;
        if (info.protection & VM_PROT_READ) {
            if (oah_scan_region(addr)) for (p = addr; p < addr + size; p += 4096) {
                unsigned int hb[8]; const unsigned int *h = hb; if (!oah_peek2(p, hb)) continue;
                if (h[0] == OSSwapHostToBigInt32(0xfeedfaceu) && h[1] == OSSwapHostToBigInt32(18u) &&
                    h[3] == OSSwapHostToBigInt32(6u)) {
                    unsigned int a = oah_guest_sym_in((unsigned int)p, lib, sym);
                    if (a) {
                        if (!nocache) oah_gsb_put(lib, (unsigned int)p);
                        if (getenv("OAH_VERBOSE"))
                            fprintf(stderr, "[gsym] %s in %s at 0x%08x (image 0x%08x)\n", sym, lib, a, (unsigned int)p);
                        return a;
                    }
                }
            }
        }
        addr += size;
    }
    if (!quiet) fprintf(stderr, "[gsym] %s not found in %s\n", sym, lib);
    return 0;
}
static int probe_read(unsigned int addr, unsigned int *out);
/* Each guest thread's register file by its Mach thread port, noted at its bridge calls: for
   thread_get_state(PPC_THREAD_STATE) on a thread that is deep in native code (oah_gtrace.h). */
/* A thread can have more than one register file (native code calling back into the guest), so
   the last 8 distinct ones are kept. */
#define OAH_THR_RMAX 256
#define OAH_THR_RPER 8
static struct { unsigned int port, n; unsigned int *R[OAH_THR_RPER]; } g_thr_R[OAH_THR_RMAX];
static int g_thr_Rn;
static pthread_mutex_t g_thr_Rmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t g_thr_Rkey;
static pthread_once_t g_thr_Ronce = PTHREAD_ONCE_INIT;
static void oah_thr_Rkey_init(void) { pthread_key_create(&g_thr_Rkey, 0); }
static void oah_thr_R_note(unsigned int *R) {
    unsigned int port, k; int i;
    pthread_once(&g_thr_Ronce, oah_thr_Rkey_init);
    if (pthread_getspecific(g_thr_Rkey) == (void *)R) return;
    pthread_setspecific(g_thr_Rkey, (void *)R);
    port = pthread_mach_thread_np(pthread_self());
    pthread_mutex_lock(&g_thr_Rmu);
    for (i = 0; i < g_thr_Rn && g_thr_R[i].port != port; i++) ;
    if (i == g_thr_Rn && g_thr_Rn < OAH_THR_RMAX) { g_thr_R[i].port = port; g_thr_R[i].n = 0; g_thr_Rn++; }
    if (i < g_thr_Rn) {
        for (k = 0; k < g_thr_R[i].n && k < OAH_THR_RPER && g_thr_R[i].R[k] != R; k++) ;
        if (k == g_thr_R[i].n || k >= OAH_THR_RPER) g_thr_R[i].R[g_thr_R[i].n++ % OAH_THR_RPER] = R;
    }
    pthread_mutex_unlock(&g_thr_Rmu);
}
/* the register files noted for this thread port: count, into out[OAH_THR_RPER] */
static int oah_thr_R_list(unsigned int port, unsigned int *out) {
    int i, k, n = 0;
    for (i = 0; i < g_thr_Rn; i++) if (g_thr_R[i].port == port) {
        int m = g_thr_R[i].n < OAH_THR_RPER ? (int)g_thr_R[i].n : OAH_THR_RPER;
        for (k = 0; k < m; k++) out[n++] = (unsigned int)(uintptr_t)g_thr_R[i].R[k];
        break;
    }
    return n;
}
#include "oah_gtrace.h"
#include "oah_cam.h"
/* GetProcessBundleLocation(const ProcessSerialNumber *psn, FSRef *location).
   The PPC Process Manager cannot answer this here, so build the answer from
   the guest executable path (OAH_GUEST_EXE, set by the rosetta script): the
   enclosing .app bundle, or the executable itself when it is not bundled.
   The FSRef is made by the guest's own FSPathMakeRef so its format matches. */
static void oah_get_process_bundle_location(unsigned int *R) {
    static unsigned int fsmake;
    static char path[PATH_MAX];
    const char *exe = getenv("OAH_GUEST_EXE");
    unsigned int args[3], fsref = R[4];
    char *m;
    if (!fsref || !exe || !realpath(exe, path)) { R[3] = (unsigned int)-50; return; }   /* paramErr */
    m = strstr(path, ".app/Contents/MacOS/");
    if (m) m[4] = 0;
    if (!fsmake) fsmake = oah_guest_sym("CarbonCore", "_FSPathMakeRef");
    if (!fsmake) { R[3] = (unsigned int)-600; return; }   /* procNotFound */
    args[0] = (unsigned int)path; args[1] = fsref; args[2] = 0;
    R[3] = oah_callppc_n("oah_fspathmakeref", fsmake, 3, args);
    if (getenv("OAH_VERBOSE"))
        fprintf(stderr, "[bridge] GetProcessBundleLocation -> \"%s\" err=%d\n", path, (int)R[3]);
}
/* User/group database.  The guest libSystem asks DirectoryService over the
   libinfo_v1 MIG protocol (msg 50000/50001), which Mojave's opendirectoryd does
   not speak, so getpwnam & co. fail and daemons like fontd exit.  These are
   answered with Mojave's own functions; struct passwd / group have the same
   layout on ppc32 and i386, only byte order differs. */
#include <pwd.h>
#include <grp.h>
static unsigned int oah_strdup_g(char **pool, char *end, const char *s) {
    size_t n;
    char *d;
    if (!s) return 0;
    n = strlen(s) + 1;
    if (*pool + n > end) return 0;
    d = *pool; memcpy(d, s, n); *pool += n;
    return (unsigned int)d;
}
static int oah_put_pw(const struct passwd *p, unsigned int gpw, char *buf, size_t bsz) {
    char *q = buf, *end = buf + bsz;
    unsigned int v[10];
    int i;
    v[0] = oah_strdup_g(&q, end, p->pw_name); v[1] = oah_strdup_g(&q, end, p->pw_passwd);
    v[2] = p->pw_uid; v[3] = p->pw_gid; v[4] = (unsigned int)p->pw_change;
    v[5] = oah_strdup_g(&q, end, p->pw_class); v[6] = oah_strdup_g(&q, end, p->pw_gecos);
    v[7] = oah_strdup_g(&q, end, p->pw_dir); v[8] = oah_strdup_g(&q, end, p->pw_shell);
    v[9] = (unsigned int)p->pw_expire;
    if (p->pw_name && !v[0]) return ERANGE;
    for (i = 0; i < 10; i++) ((unsigned int *)gpw)[i] = OSSwapHostToBigInt32(v[i]);
    return 0;
}
static int oah_put_gr(const struct group *g, unsigned int ggr, char *buf, size_t bsz) {
    char *q, *end = buf + bsz;
    unsigned int *mem, nm = 0, i, v[4];
    while (g->gr_mem && g->gr_mem[nm]) nm++;
    if ((nm + 1) * 4 > bsz) return ERANGE;
    mem = (unsigned int *)buf; q = buf + (nm + 1) * 4;
    for (i = 0; i < nm; i++) mem[i] = OSSwapHostToBigInt32(oah_strdup_g(&q, end, g->gr_mem[i]));
    mem[nm] = 0;
    v[0] = oah_strdup_g(&q, end, g->gr_name); v[1] = oah_strdup_g(&q, end, g->gr_passwd);
    v[2] = g->gr_gid; v[3] = (unsigned int)mem;
    if (g->gr_name && !v[0]) return ERANGE;
    for (i = 0; i < 4; i++) ((unsigned int *)ggr)[i] = OSSwapHostToBigInt32(v[i]);
    return 0;
}
/* ---- libnotify (notify_*) ----
   The 10.6 libnotify client speaks notifyd's old MIG protocol, which Mojave's
   notifyd no longer accepts: every call returned NOTIFY_STATUS_FAILED, so
   nothing posted by or for a translated process was ever delivered (fontd waits
   on genatsdb's completion notice this way). The public calls go to Mojave's own
   libnotify instead. Tokens and Mach ports are plain integers in the shared task;
   file-descriptor notifications carry the token in network order, which is what
   the big-endian guest expects. notify_register_dispatch (guest block and queue)
   is not bridged yet: it reports NOTIFY_STATUS_FAILED, as the guest's own code did. */
#include <notify.h>
static int oah_notify_call(const char *n, unsigned int *R) {
    const char *f; uint32_t s = 1000000; int tok = -1, v = 0;
    if (!strcmp(n, "confstr") || !strcmp(n, "confstr$UNIX2003")) {
        /* 10.6 confstr asks launchd's dirhelper (gone on Mojave) for the per-user
           folders, so _CS_DARWIN_USER_CACHE_DIR came back empty and fontd put its
           registry at "/fontregistry.user". The constants are the same natively. */
        unsigned int nm = R[3];
        R[3] = (unsigned int)confstr((int)R[3], (char *)R[4], (size_t)R[5]);
        if (getenv("OAH_NOTIFYTRACE")) fprintf(stderr, "[confstr] %u -> %u errno %d\n", nm, R[3], errno);
        return 1;
    }
    if (strncmp(n, "notify_", 7) && strcmp(n, "_notify_fork_child")) return 0;
    f = n + 7;
    if (!strcmp(n, "_notify_fork_child")) {
        void (*fc)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "_notify_fork_child");
        if (fc) fc();
        return 1;
    }
    if (!strcmp(f, "post")) s = notify_post((const char *)R[3]);
    else if (!strcmp(f, "register_check") || !strcmp(f, "register_plain")) {
        s = notify_register_check((const char *)R[3], &tok);
        if (R[4]) *(unsigned int *)R[4] = OSSwapHostToBigInt32((unsigned int)tok);
    } else if (!strcmp(f, "register_signal")) {
        s = notify_register_signal((const char *)R[3], (int)R[4], &tok);
        if (R[5]) *(unsigned int *)R[5] = OSSwapHostToBigInt32((unsigned int)tok);
    } else if (!strcmp(f, "register_mach_port") || !strcmp(f, "register_file_descriptor")) {
        /* (name, port_or_fd *inout, flags, int *token); NOTIFY_REUSE (1) reads *inout */
        int io = R[4] ? (int)OSSwapBigToHostInt32(*(unsigned int *)R[4]) : 0;
        if (f[9] == 'm') { mach_port_t p = (mach_port_t)io; s = notify_register_mach_port((const char *)R[3], &p, (int)R[5], &tok); io = (int)p; }
        else s = notify_register_file_descriptor((const char *)R[3], &io, (int)R[5], &tok);
        if (R[4] && s == 0) *(unsigned int *)R[4] = OSSwapHostToBigInt32((unsigned int)io);
        if (R[6]) *(unsigned int *)R[6] = OSSwapHostToBigInt32((unsigned int)tok);
    } else if (!strcmp(f, "check")) {
        s = notify_check((int)R[3], &v);
        if (R[4]) *(unsigned int *)R[4] = OSSwapHostToBigInt32((unsigned int)v);
    } else if (!strcmp(f, "cancel")) s = notify_cancel((int)R[3]);
    else if (!strcmp(f, "suspend")) s = notify_suspend((int)R[3]);
    else if (!strcmp(f, "resume")) s = notify_resume((int)R[3]);
    else if (!strcmp(f, "get_state")) {
        uint64_t st = 0;
        s = notify_get_state((int)R[3], &st);
        if (R[4]) { ((unsigned int *)R[4])[0] = OSSwapHostToBigInt32((unsigned int)(st >> 32));
                    ((unsigned int *)R[4])[1] = OSSwapHostToBigInt32((unsigned int)st); }
    } else if (!strcmp(f, "set_state")) s = notify_set_state((int)R[3], ((uint64_t)R[4] << 32) | R[5]);   /* r4:r5 */
    else if (!strcmp(f, "monitor_file")) s = notify_monitor_file((int)R[3], (char *)R[4], (int)R[5]);
    else if (!strcmp(f, "peek")) {
        uint32_t (*pk)(int, uint32_t *) = (uint32_t (*)(int, uint32_t *))dlsym(RTLD_DEFAULT, "notify_peek");
        uint32_t pv = 0;
        s = pk ? pk((int)R[3], &pv) : 1000000;
        if (R[4]) *(unsigned int *)R[4] = OSSwapHostToBigInt32(pv);
    } else if (!strcmp(f, "register_dispatch")) {
        if (R[4]) *(unsigned int *)R[4] = OSSwapHostToBigInt32(0xffffffffu);
        oah_tel("notify", (const char *)R[3]);
        fprintf(stderr, "[notify] notify_register_dispatch(\"%s\") not bridged\n", (const char *)R[3]);
    } else s = 1000000;   /* get_event, set_owner/access, release_name, *_pid: unsupported */
    if (getenv("OAH_NOTIFYTRACE"))
        fprintf(stderr, "[notify] %s(%s) -> %u tok %d\n", n,
                (!strcmp(f, "post") || !strncmp(f, "register", 8)) && R[3] > 0x1000 ? (const char *)R[3] : "", s, tok);
    R[3] = s;
    return 1;
}
/* ---- task broker (for the translated coreservicesd) ----
   Snow Leopard's coreservicesd shares LaunchServices' memory with each client by
   calling task_for_pid on it, which a normal user process may not do on Mojave.
   Instead, a translated client hands its own task port to the broker thread in
   our coreservicesd (Mach service com.rosettus.ppc.taskbroker, declared only in
   that agent's plist) just before it connects, and the server's guest
   task_for_pid is answered from that table.  Only that one daemon ever holds the
   ports, as it did on Snow Leopard. */
#define OAH_TB_SERVICE "com.rosettus.ppc.taskbroker"
#define OAH_TB_MSGID 0x0a51b000
typedef struct { mach_msg_header_t h; mach_msg_body_t b; mach_msg_port_descriptor_t task; int pid; } oah_tb_msg;
static struct { int pid; mach_port_t task; } g_tb[128];
static pthread_mutex_t g_tb_lock = PTHREAD_MUTEX_INITIALIZER;
static void *oah_tb_thread(void *arg) {
    mach_port_t rp = (mach_port_t)(uintptr_t)arg;
    for (;;) {
        union { oah_tb_msg m; char b[512]; } u;
        int i, slot = -1, pid = 0;
        memset(&u, 0, sizeof u.m);
        if (mach_msg(&u.m.h, MACH_RCV_MSG, 0, sizeof u, rp, 0, MACH_PORT_NULL) != KERN_SUCCESS) continue;
        if (u.m.h.msgh_id == OAH_TB_MSGID && (u.m.h.msgh_bits & MACH_MSGH_BITS_COMPLEX) && u.m.b.msgh_descriptor_count == 1 &&
            pid_for_task(u.m.task.name, &pid) == KERN_SUCCESS && pid == u.m.pid) {
            pthread_mutex_lock(&g_tb_lock);
            for (i = 0; i < 128; i++) {
                if (g_tb[i].pid == pid || (slot < 0 && !g_tb[i].pid)) { if (g_tb[i].pid == pid) { slot = i; break; } slot = i; }
                else if (g_tb[i].pid) { int p2 = 0; if (pid_for_task(g_tb[i].task, &p2) || p2 != g_tb[i].pid) {   /* dead */
                    mach_port_deallocate(mach_task_self(), g_tb[i].task); g_tb[i].pid = 0; if (slot < 0) slot = i; } }
            }
            if (slot >= 0) {
                if (g_tb[slot].pid == pid) mach_port_deallocate(mach_task_self(), g_tb[slot].task);
                g_tb[slot].pid = pid; g_tb[slot].task = u.m.task.name;
            } else mach_port_deallocate(mach_task_self(), u.m.task.name);
            pthread_mutex_unlock(&g_tb_lock);
            if (getenv("OAH_VERBOSE")) fprintf(stderr, "[taskbroker] got task of pid %d\n", pid);
        } else if (u.m.h.msgh_bits & MACH_MSGH_BITS_COMPLEX) mach_msg_destroy(&u.m.h);
        if (MACH_PORT_VALID(u.m.h.msgh_remote_port)) {   /* acknowledge so the client continues */
            mach_msg_header_t a; memset(&a, 0, sizeof a);
            a.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(u.m.h.msgh_bits), 0);
            a.msgh_size = sizeof a; a.msgh_remote_port = u.m.h.msgh_remote_port; a.msgh_id = OAH_TB_MSGID + 100;
            mach_msg(&a, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof a, 0, MACH_PORT_NULL, 100, MACH_PORT_NULL);
        }
    }
    return NULL;
}
static void oah_tb_server_start(void) {
    mach_port_t p = MACH_PORT_NULL; pthread_t t;
    if (bootstrap_check_in(bootstrap_port, OAH_TB_SERVICE, &p) != KERN_SUCCESS) return;
    pthread_create(&t, NULL, oah_tb_thread, (void *)(uintptr_t)p);
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[taskbroker] serving\n");
}
static void oah_tb_client_send(void) {
    static int done;
    mach_port_t sp = MACH_PORT_NULL, reply;
    oah_tb_msg m;
    union { mach_msg_header_t h; char b[256]; } r;
    if (done) return;
    done = 1;
    if (getenv("OAH_TASKBROKER")) return;                 /* we are the server */
    if (bootstrap_look_up(bootstrap_port, OAH_TB_SERVICE, &sp) != KERN_SUCCESS) return;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply) != KERN_SUCCESS) {
        mach_port_deallocate(mach_task_self(), sp); return;
    }
    memset(&m, 0, sizeof m);
    m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE) | MACH_MSGH_BITS_COMPLEX;
    m.h.msgh_size = sizeof m; m.h.msgh_remote_port = sp; m.h.msgh_local_port = reply; m.h.msgh_id = OAH_TB_MSGID;
    m.b.msgh_descriptor_count = 1;
    m.task.name = mach_task_self(); m.task.disposition = MACH_MSG_TYPE_COPY_SEND; m.task.type = MACH_MSG_PORT_DESCRIPTOR;
    m.pid = getpid();
    mach_msg(&m.h, MACH_SEND_MSG | MACH_RCV_MSG | MACH_SEND_TIMEOUT | MACH_RCV_TIMEOUT, sizeof m, sizeof r, reply, 3000, MACH_PORT_NULL);
    mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1);
    mach_port_deallocate(mach_task_self(), sp);
}
/* ---- per-PID name broker (for CFMessagePortCreatePerProcessLocal / Remote) ----
   10.6 CFPasteboard registers its client port as a per-PID bootstrap service (bootstrap_register2
   with BOOTSTRAP_PER_PID_SERVICE) and pboard looks it up by the client's pid. Mojave's launchd
   refuses dynamic registrations (1100). Translated processes keep those names in a broker thread
   instead, which runs in the translated pboard agent (OAH_NAMEBROKER=1; Mach service
   com.rosettus.ppc.namebroker declared in its plist). The bootstrap proxy forwards per-PID
   register2 / look_up2 here. */
#define OAH_NB_SERVICE "com.rosettus.ppc.namebroker"
#define OAH_NB_REG 0x0a51b100
#define OAH_NB_LOOK 0x0a51b101
typedef struct { mach_msg_header_t h; mach_msg_body_t b; mach_msg_port_descriptor_t port; int pid; char name[128]; } oah_nb_reg_t;
typedef struct { mach_msg_header_t h; int pid; char name[128]; } oah_nb_look_t;
typedef struct { mach_msg_header_t h; mach_msg_body_t b; mach_msg_port_descriptor_t port; int kr; } oah_nb_reply_t;
static struct { int pid; char name[128]; mach_port_t port; } g_nb[256];
static void *oah_nb_thread(void *arg) {
    mach_port_t rp = (mach_port_t)(uintptr_t)arg;
    for (;;) {
        union { oah_nb_reg_t r; oah_nb_look_t l; char b[1024]; } u;
        oah_nb_reply_t rep; int i, slot = -1;
        memset(&u, 0, sizeof u.r);
        if (mach_msg(&u.r.h, MACH_RCV_MSG, 0, sizeof u, rp, 0, MACH_PORT_NULL) != KERN_SUCCESS) { usleep(10000); continue; }
        memset(&rep, 0, sizeof rep);
        rep.h.msgh_remote_port = u.r.h.msgh_remote_port;
        rep.h.msgh_id = u.r.h.msgh_id + 100;
        rep.kr = 0x44e;   /* BOOTSTRAP_UNKNOWN_SERVICE */
        for (i = 0; i < 256; i++)   /* forget dead processes */
            if (g_nb[i].pid && kill(g_nb[i].pid, 0) && errno == ESRCH) { mach_port_deallocate(mach_task_self(), g_nb[i].port); g_nb[i].pid = 0; }
        if (u.r.h.msgh_id == OAH_NB_REG && (u.r.h.msgh_bits & MACH_MSGH_BITS_COMPLEX)) {
            u.r.name[127] = 0;
            for (i = 0; i < 256; i++) {
                if (g_nb[i].pid == u.r.pid && !strcmp(g_nb[i].name, u.r.name)) { mach_port_deallocate(mach_task_self(), g_nb[i].port); slot = i; break; }
                if (slot < 0 && !g_nb[i].pid) slot = i;
            }
            if (slot >= 0) { g_nb[slot].pid = u.r.pid; strlcpy(g_nb[slot].name, u.r.name, 128); g_nb[slot].port = u.r.port.name; rep.kr = 0; }
            else mach_port_deallocate(mach_task_self(), u.r.port.name);
            if (getenv("OAH_BSTRACE")) fprintf(stderr, "[namebroker] register \"%s\" pid %d -> %d\n", u.r.name, u.r.pid, rep.kr);
            rep.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
            rep.h.msgh_size = sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) + sizeof(mach_msg_port_descriptor_t) + sizeof(int);
            rep.b.msgh_descriptor_count = 0;
            /* no port in a register reply: send a plain header + kr */
            {   struct { mach_msg_header_t h; int kr; } p; memset(&p, 0, sizeof p);
                p.h = rep.h; p.h.msgh_size = sizeof p; p.kr = rep.kr;
                mach_msg(&p.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof p, 0, MACH_PORT_NULL, 100, MACH_PORT_NULL); }
            continue;
        }
        if (u.l.h.msgh_id == OAH_NB_LOOK) {
            u.l.name[127] = 0;
            for (i = 0; i < 256; i++) if (g_nb[i].pid && g_nb[i].pid == u.l.pid && !strcmp(g_nb[i].name, u.l.name)) break;
            rep.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
            rep.h.msgh_size = sizeof rep;
            rep.b.msgh_descriptor_count = 1;
            rep.port.type = MACH_MSG_PORT_DESCRIPTOR;
            if (i < 256) { rep.port.name = g_nb[i].port; rep.port.disposition = MACH_MSG_TYPE_COPY_SEND; rep.kr = 0; }
            else { rep.port.name = MACH_PORT_NULL; rep.port.disposition = MACH_MSG_TYPE_COPY_SEND; }
            if (getenv("OAH_BSTRACE")) fprintf(stderr, "[namebroker] look_up \"%s\" pid %d -> %d\n", u.l.name, u.l.pid, rep.kr);
            mach_msg(&rep.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof rep, 0, MACH_PORT_NULL, 100, MACH_PORT_NULL);
            continue;
        }
        mach_msg_destroy(&u.r.h);
    }
    return NULL;
}
static void oah_nb_server_start(void) {
    mach_port_t p = MACH_PORT_NULL; pthread_t t;
    if (bootstrap_check_in(bootstrap_port, OAH_NB_SERVICE, &p) != KERN_SUCCESS) { fprintf(stderr, "[namebroker] check_in failed\n"); return; }
    pthread_create(&t, NULL, oah_nb_thread, (void *)(uintptr_t)p);
    pthread_detach(t);
}
/* client side: kr; *port_out set on a successful look_up */
static kern_return_t oah_nb_request(int reg, const char *name, int pid, mach_port_t port, mach_port_t *port_out) {
    static mach_port_t sp;
    mach_port_t reply;
    union { oah_nb_reply_t r; struct { mach_msg_header_t h; int kr; } p; char b[256]; } in;
    kern_return_t kr;
    if (!sp && bootstrap_look_up(bootstrap_port, OAH_NB_SERVICE, &sp) != KERN_SUCCESS) { sp = 0; return 0x44e; }
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply) != KERN_SUCCESS) return 0x44e;
    memset(&in, 0, sizeof in);
    if (reg) {
        oah_nb_reg_t m; memset(&m, 0, sizeof m);
        m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE) | MACH_MSGH_BITS_COMPLEX;
        m.h.msgh_size = sizeof m; m.h.msgh_remote_port = sp; m.h.msgh_local_port = reply; m.h.msgh_id = OAH_NB_REG;
        m.b.msgh_descriptor_count = 1;
        m.port.name = port; m.port.disposition = MACH_MSG_TYPE_COPY_SEND; m.port.type = MACH_MSG_PORT_DESCRIPTOR;
        m.pid = pid; strlcpy(m.name, name, sizeof m.name);
        kr = mach_msg(&m.h, MACH_SEND_MSG | MACH_RCV_MSG | MACH_SEND_TIMEOUT | MACH_RCV_TIMEOUT, sizeof m, sizeof in, reply, 3000, MACH_PORT_NULL);
        if (kr == KERN_SUCCESS) { memcpy(&in, &m, sizeof m < sizeof in ? sizeof m : sizeof in); kr = in.p.kr; }   /* the reply lands in m */
    } else {
        oah_nb_look_t m; memset(&m, 0, sizeof m);
        m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
        m.h.msgh_size = sizeof m; m.h.msgh_remote_port = sp; m.h.msgh_local_port = reply; m.h.msgh_id = OAH_NB_LOOK;
        m.pid = pid; strlcpy(m.name, name, sizeof m.name);
        kr = mach_msg(&m.h, MACH_SEND_MSG | MACH_RCV_MSG | MACH_SEND_TIMEOUT | MACH_RCV_TIMEOUT, sizeof m, sizeof in, reply, 3000, MACH_PORT_NULL);
        if (kr == KERN_SUCCESS) { memcpy(&in, &m, sizeof m < sizeof in ? sizeof m : sizeof in);
                                  kr = in.r.kr; if (!kr && port_out) *port_out = in.r.port.name; }
    }
    if (kr == MACH_SEND_INVALID_DEST) { mach_port_deallocate(mach_task_self(), sp); sp = 0; }
    mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1);
    return kr;
}
static int oah_task_call(const char *n, unsigned int *R) {
    mach_port_t t = MACH_PORT_NULL; kern_return_t kr; int i;
    if (strcmp(n, "task_for_pid")) return 0;
    kr = task_for_pid((mach_port_t)R[3], (int)R[4], &t);
    if (kr != KERN_SUCCESS) {
        pthread_mutex_lock(&g_tb_lock);
        for (i = 0; i < 128; i++)
            if (g_tb[i].pid && g_tb[i].pid == (int)R[4]) {
                if (mach_port_mod_refs(mach_task_self(), g_tb[i].task, MACH_PORT_RIGHT_SEND, 1) == KERN_SUCCESS) { t = g_tb[i].task; kr = KERN_SUCCESS; }
                break;
            }
        pthread_mutex_unlock(&g_tb_lock);
    }
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[taskbroker] task_for_pid(%d) -> %d port 0x%x\n", (int)R[4], kr, t);
    if (R[5] && kr == KERN_SUCCESS) *(unsigned int *)R[5] = OSSwapHostToBigInt32(t);
    R[3] = kr;
    return 1;
}
static int oah_userdb_call(const char *n, unsigned int *R) {
    static unsigned int spw[10], sgr[4];
    static char spool[4096], sgpool[16384];
    int byname, err;
    if (strncmp(n, "getpw", 5) && strncmp(n, "getgr", 5)) return 0;
    byname = strstr(n, "nam") != 0;
    if (!strcmp(n, "getpwnam") || !strcmp(n, "getpwuid")) {
        struct passwd *p = byname ? getpwnam((const char *)R[3]) : getpwuid((uid_t)R[3]);
        R[3] = (p && !oah_put_pw(p, (unsigned int)spw, spool, sizeof spool)) ? (unsigned int)spw : 0;
    } else if (!strcmp(n, "getpwnam_r") || !strcmp(n, "getpwuid_r")) {
        /* (key, struct passwd *pwd, char *buf, size_t size, struct passwd **result) */
        struct passwd pw, *p = 0;
        char tmp[4096];
        err = byname ? getpwnam_r((const char *)R[3], &pw, tmp, sizeof tmp, &p)
                     : getpwuid_r((uid_t)R[3], &pw, tmp, sizeof tmp, &p);
        if (!err && p) err = oah_put_pw(p, R[4], (char *)R[5], R[6]);
        if (R[7]) *(unsigned int *)R[7] = (!err && p) ? OSSwapHostToBigInt32(R[4]) : 0;
        R[3] = (unsigned int)err;
    } else if (!strcmp(n, "getgrnam") || !strcmp(n, "getgrgid")) {
        struct group *g = byname ? getgrnam((const char *)R[3]) : getgrgid((gid_t)R[3]);
        R[3] = (g && !oah_put_gr(g, (unsigned int)sgr, sgpool, sizeof sgpool)) ? (unsigned int)sgr : 0;
    } else if (!strcmp(n, "getgrnam_r") || !strcmp(n, "getgrgid_r")) {
        struct group gr, *g = 0;
        char tmp[16384];
        err = byname ? getgrnam_r((const char *)R[3], &gr, tmp, sizeof tmp, &g)
                     : getgrgid_r((gid_t)R[3], &gr, tmp, sizeof tmp, &g);
        if (!err && g) err = oah_put_gr(g, R[4], (char *)R[5], R[6]);
        if (R[7]) *(unsigned int *)R[7] = (!err && g) ? OSSwapHostToBigInt32(R[4]) : 0;
        R[3] = (unsigned int)err;
    } else return 0;
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[bridge] %s -> 0x%x\n", n, R[3]);
    return 1;
}
/* FSFindFolder(FSVolumeRefNum vRefNum, OSType folderType, Boolean create, FSRef *ref).
   CFM apps (LaunchCFMApp -> CCFM_InitializeCrutchCFM) need the system-domain
   CFMSupport folder ('Ćlib'), which on Mojave does not exist in /System/Library
   (and cannot be created there).  It lives in the guest tree instead, so that one
   folder type is answered with a guest FSRef to ROOT/System/Library/CFMSupport
   (or ~/Library/CFMSupport, /Library/CFMSupport for the user/local domains, when
   present).  Every other request goes to the guest Folder Manager's own worker:
   FSFindFolder's first instructions are overwritten by the bridge mark, but its
   body calls the worker through a pointer in CarbonCore's data, which is read here
   after checking the instructions that load it (10.6.8 PPC CarbonCore). */
/* CFM bridge libraries ("CFMPriv_X" vector libraries that CarbonLib imports) are
   registered by CCFM_InitializeCrutchCFM from the CarbonLazyValues that the guest
   gets from Mojave's coreservicesd, which reads Mojave's framework Info.plists.
   Mojave's ColorSync, OSServices and CoreFoundation plists no longer carry the
   CodeFragmentManager entry, so CarbonLib fails with cfragNoLibraryErr on
   CFMPriv_ColorSync.  Here every guest framework plist is compared with Mojave's and
   each missing entry is registered through the guest's own
   RegisterOneFrameworkVectorLib(CLVContext *, CLVDatum[], count), with a small
   hand-built context: ctx+0x4 = end of strings, ctx+0x10 = string base, and a datum
   of 16-bit string offsets {image path, (unused), 0, fragment name}. */
static CFDictionaryRef oah_cfm_plist_dict(const char *plist) {
    CFDictionaryRef out = NULL;
    CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)plist, strlen(plist), false);
    CFDataRef d = NULL;
    SInt32 ec;
    if (u && CFURLCreateDataAndPropertiesFromResource(NULL, u, &d, NULL, NULL, &ec) && d) {
        CFPropertyListRef pl = CFPropertyListCreateWithData(NULL, d, kCFPropertyListImmutable, NULL, NULL);
        if (pl && CFGetTypeID(pl) == CFDictionaryGetTypeID()) {
            CFDictionaryRef clv = CFDictionaryGetValue(pl, CFSTR("CarbonLazyValues"));
            CFDictionaryRef cfm = (clv && CFGetTypeID(clv) == CFDictionaryGetTypeID()) ? CFDictionaryGetValue(clv, CFSTR("CodeFragmentManager")) : NULL;
            if (cfm && CFGetTypeID(cfm) == CFDictionaryGetTypeID()) out = CFRetain(cfm);
        }
        if (pl) CFRelease(pl);
    }
    if (d) CFRelease(d);
    if (u) CFRelease(u);
    return out;
}
static void oah_cfm_register_one(unsigned int reg, const char *path, const char *name) {
    static unsigned char strs[2048];
    static unsigned int ctx[8], datum[4];
    unsigned int args[3], n, err;
    size_t lp = strlen(path), ln = strlen(name);
    if (lp + ln + 3 > sizeof strs) return;
    memset(strs, 0, sizeof strs);
    memcpy(strs + 1, path, lp);
    memcpy(strs + 2 + lp, name, ln);
    memset(ctx, 0, sizeof ctx);
    ctx[1] = OSSwapHostToBigInt32((unsigned int)(strs + sizeof strs - 1));
    ctx[4] = OSSwapHostToBigInt32((unsigned int)strs);
    datum[0] = OSSwapHostToBigInt32(1); datum[1] = 0; datum[2] = 0;
    datum[3] = OSSwapHostToBigInt32((unsigned int)(2 + lp));
    args[0] = (unsigned int)ctx; args[1] = (unsigned int)datum; args[2] = 1;
    err = oah_callppc_n("oah_cfm_vlib", reg, 3, args);
    n = err;
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[cfm] registered missing bridge library %s -> %s (%u)\n", name, path, n);
}
static void oah_cfm_register_missing(void) {
    static int done;
    static const char *umbrellas[] = { "", "Carbon.framework/Versions/A/Frameworks/", "CoreServices.framework/Versions/A/Frameworks/",
                                       "ApplicationServices.framework/Versions/A/Frameworks/" };
    char dir[PATH_MAX], fw[PATH_MAX], gp[PATH_MAX], mp[PATH_MAX], exe[PATH_MAX], key[256], val[256];
    unsigned int reg;
    int u;
    if (done) return;
    done = 1;
    if (getenv("OAH_NO_CFMVLIB")) return;
    reg = oah_guest_sym("CarbonCore", "_RegisterOneFrameworkVectorLib");
    if (!reg) { fprintf(stderr, "[cfm] RegisterOneFrameworkVectorLib not found\n"); return; }
    for (u = 0; u < 4; u++) {
        DIR *dp;
        struct dirent *de;
        snprintf(dir, sizeof dir, "%s/System/Library/Frameworks/%s", g_rosetta_root, umbrellas[u]);
        if (!(dp = opendir(dir))) continue;
        while ((de = readdir(dp))) {
            CFDictionaryRef g, m;
            size_t nl = strlen(de->d_name);
            CFIndex i, cnt;
            if (nl < 11 || strcmp(de->d_name + nl - 10, ".framework")) continue;
            snprintf(fw, sizeof fw, "%s%s", dir, de->d_name);
            snprintf(gp, sizeof gp, "%s/Resources/Info.plist", fw);
            if (!(g = oah_cfm_plist_dict(gp))) continue;
            snprintf(mp, sizeof mp, "/System/Library/Frameworks/%s%s/Resources/Info.plist", umbrellas[u], de->d_name);
            m = oah_cfm_plist_dict(mp);
            cnt = CFDictionaryGetCount(g);
            if (cnt > 0 && cnt < 16) {
                const void *ks[16], *vs[16];
                CFDictionaryGetKeysAndValues(g, ks, vs);
                for (i = 0; i < cnt; i++) {
                    const void *mv = m ? CFDictionaryGetValue(m, ks[i]) : NULL;
                    if (CFGetTypeID(ks[i]) != CFStringGetTypeID() || CFGetTypeID(vs[i]) != CFStringGetTypeID()) continue;
                    if (mv && CFEqual(mv, vs[i])) continue;              /* Mojave's coreservicesd already has it */
                    if (!CFStringGetCString(ks[i], key, sizeof key, kCFStringEncodingUTF8)) continue;
                    if (!CFStringGetCString(vs[i], val, sizeof val, kCFStringEncodingUTF8)) continue;
                    snprintf(exe, sizeof exe, "%s/%s", fw, key);
                    if (access(exe, R_OK)) {
                        char base[256];
                        snprintf(base, sizeof base, "%.*s", (int)(nl - 10), de->d_name);
                        snprintf(exe, sizeof exe, "%s/%s", fw, base);
                    }
                    oah_cfm_register_one(reg, exe, val);
                }
            }
            CFRelease(g);
            if (m) CFRelease(m);
        }
        closedir(dp);
    }
}
/* Gestalt selectors from CarbonLazyValues. 10.6 CarbonCore installs most framework-owned
   selectors ('qtim' from QuickTime, 'qdrw' from QD, 'appr' and 'menu' from HIToolbox, ...)
   from the "Gestalt" dictionaries in the frameworks' Info.plists, as collected by
   coreservicesd. That data describes Mojave's frameworks, which no longer list them, so
   Gestalt('qtim') answered gestaltUndefSelectorErr and AppleWorks 6 quit at launch with
   "requires QuickTime 3.0 or later". Every numeric selector the guest frameworks' plists
   list that the guest Gestalt does not know is added with the guest's NewGestaltValue.
   (Function-valued entries, e.g. QuickTime's 'qtth', are not resolved here.)
   OAH_NO_GESTALTCLV=1 disables. */
static CFDictionaryRef oah_clv_gestalt_dict(const char *plist) {
    CFDictionaryRef out = NULL;
    CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)plist, strlen(plist), false);
    CFDataRef d = NULL;
    SInt32 ec;
    if (u && CFURLCreateDataAndPropertiesFromResource(NULL, u, &d, NULL, NULL, &ec) && d) {
        CFPropertyListRef pl = CFPropertyListCreateWithData(NULL, d, kCFPropertyListImmutable, NULL, NULL);
        if (pl && CFGetTypeID(pl) == CFDictionaryGetTypeID()) {
            CFDictionaryRef clv = CFDictionaryGetValue(pl, CFSTR("CarbonLazyValues"));
            CFDictionaryRef ge = (clv && CFGetTypeID(clv) == CFDictionaryGetTypeID()) ? CFDictionaryGetValue(clv, CFSTR("Gestalt")) : NULL;
            if (ge && CFGetTypeID(ge) == CFDictionaryGetTypeID()) out = CFRetain(ge);
        }
        if (pl) CFRelease(pl);
    }
    if (d) CFRelease(d);
    if (u) CFRelease(u);
    return out;
}
static int oah_gestalt_register_missing(void) {   /* 1 = done (or disabled), 0 = retry later */
    static int done;
    static const char *umbrellas[] = { "", "Carbon.framework/Versions/A/Frameworks/", "CoreServices.framework/Versions/A/Frameworks/",
                                       "ApplicationServices.framework/Versions/A/Frameworks/" };
    static unsigned int resp;
    char dir[PATH_MAX], gp[PATH_MAX], key[16];
    unsigned int fget, fnew, args[2];
    int u, added = 0, lazy = 0;
    if (done) return 1;
    if (getenv("OAH_NO_GESTALTCLV")) { done = 1; return 1; }
    fget = oah_guest_sym("CarbonCore", "_Gestalt");
    fnew = oah_guest_sym("CarbonCore", "_NewGestaltValue");
    if (!fget || !fnew) return 0;                                /* CarbonCore not loaded yet */
    done = 1;
    for (u = 0; u < 4; u++) {
        DIR *dp;
        struct dirent *de;
        snprintf(dir, sizeof dir, "%s/System/Library/Frameworks/%s", g_rosetta_root, umbrellas[u]);
        if (!(dp = opendir(dir))) continue;
        while ((de = readdir(dp))) {
            CFDictionaryRef g;
            size_t nl = strlen(de->d_name);
            CFIndex i, cnt;
            if (nl < 11 || strcmp(de->d_name + nl - 10, ".framework")) continue;
            snprintf(gp, sizeof gp, "%s%s/Resources/Info.plist", dir, de->d_name);
            if (!(g = oah_clv_gestalt_dict(gp))) continue;
            cnt = CFDictionaryGetCount(g);
            if (cnt > 0 && cnt < 128) {
                const void *ks[128], *vs[128];
                CFDictionaryGetKeysAndValues(g, ks, vs);
                for (i = 0; i < cnt; i++) {
                    unsigned int sel, err;
                    SInt64 v = 0;
                    if (CFGetTypeID(ks[i]) != CFStringGetTypeID()) continue;
                    if (!CFStringGetCString(ks[i], key, sizeof key, kCFStringEncodingMacRoman) || strlen(key) != 4) continue;
                    sel = ((unsigned)(unsigned char)key[0] << 24) | ((unsigned)(unsigned char)key[1] << 16) |
                          ((unsigned)(unsigned char)key[2] << 8) | (unsigned char)key[3];
                    resp = 0;
                    args[0] = sel; args[1] = (unsigned int)&resp;
                    err = oah_callppc_n("Gestalt", fget, 2, args);
                    if ((short)err != -5551) continue;                 /* already known */
                    if (CFGetTypeID(vs[i]) == CFStringGetTypeID()) {
                        /* "123" is a number; otherwise the name of an initializer in that
                           framework that 10.6 CarbonCore calls lazily.  Calling those from
                           here can block (OSAInstallGestaltValues did), so the few that matter
                           get Snow Leopard's values instead: 'otan' (Open Transport present
                           and loaded; Internet Explorer's stream library builds no TCP layer
                           without it), Script Manager and keyboard type. */
                        static const struct { const char *k; unsigned int v; } known[] = {
                            { "otan", 0x33 }, { "scri", 0x1000 }, { "scr#", 7 }, { "kbd ", 0x25 } };
                        char fn[128], *endp;
                        unsigned int kk, found = 0;
                        if (!CFStringGetCString(vs[i], fn, sizeof fn, kCFStringEncodingUTF8)) { lazy++; continue; }
                        v = strtoll(fn, &endp, 0);
                        if (*endp || !*fn) {
                            for (kk = 0; kk < sizeof known / sizeof *known; kk++)
                                if (!strcmp(known[kk].k, key)) { v = known[kk].v; found = 1; }
                            if (!found) {
                                /* a selector function: 10.6 CarbonCore installs it on first use. 'scrx'
                                   (INIT_ScriptHL) wires HIToolbox's keyboard hooks into the Script
                                   Manager; without it smKCHRCache stayed NULL and Office 2004 crashed at
                                   launch reading the KCHR.  Install it as a Gestalt selector function, so it
                                   runs lazily in the caller's context as it did on 10.6. */
                                static unsigned int fsel;
                                unsigned int fp;
                                if (!strcmp(key, "scrx") && !getenv("OAH_NO_SCRX") &&
                                    (fsel || (fsel = oah_guest_sym("CarbonCore", "_NewGestalt"))) &&
                                    (fp = oah_guest_sym("HIToolbox", "_" "INIT_ScriptHL"))) {
                                    unsigned int ga[2]; ga[0] = sel; ga[1] = fp;
                                    err = oah_callppc_n("NewGestalt", fsel, 2, ga);
                                    fprintf(stderr, "[gestalt] 'scrx' -> selector function INIT_ScriptHL %08x: %d\n", fp, (int)(short)err);
                                    added++;
                                    continue;
                                }
                                lazy++; continue;
                            }
                        }
                    } else if (CFGetTypeID(vs[i]) == CFNumberGetTypeID()) CFNumberGetValue(vs[i], kCFNumberSInt64Type, &v);
                    else { lazy++; continue; }
                    args[0] = sel; args[1] = (unsigned int)(SInt32)v;
                    err = oah_callppc_n("NewGestaltValue", fnew, 2, args);
                    added++;
                    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[gestalt] '%s' = 0x%x (%s) -> %d\n", key, (unsigned)v, de->d_name, (int)(short)err);
                }
            }
            CFRelease(g);
        }
        closedir(dp);
    }
    if (getenv("OAH_VERBOSE") || added)
        fprintf(stderr, "[gestalt] added %d selector(s) from guest CarbonLazyValues (%d function-valued skipped)\n", added, lazy);
    return 1;
}
static unsigned int g_fs_make, g_fs_catinfo;
static void oah_fs_find_folder(unsigned int *R) {
    oah_gestalt_register_missing();
    static unsigned int ffa, worker, checked;
    short dom = (short)R[3];
    unsigned int type = R[4], create = R[5] & 0xff, ref = R[6];
    if (ref) memset((void *)ref, 0, 80);
    if (type == 0xc46c6962u) {                                    /* kCFMSupportFolderType */
        static char path[PATH_MAX];
        struct stat st;
        unsigned int args[3];
        if (!g_fs_make) g_fs_make = oah_guest_sym("CarbonCore", "_FSPathMakeRef");
        if (dom == -32766 || dom == -32768 || dom == -32767 || dom == 0 || dom == -100)
            snprintf(path, sizeof path, "%s/System/Library/CFMSupport", g_rosetta_root);
        else if (dom == -32765) snprintf(path, sizeof path, "/Library/CFMSupport");
        else if (dom == -32763) snprintf(path, sizeof path, "%s/Library/CFMSupport", getenv("HOME") ? getenv("HOME") : "/tmp");
        else path[0] = 0;
        if (!path[0] || stat(path, &st) || !S_ISDIR(st.st_mode) || !g_fs_make || !ref) R[3] = (unsigned int)-43;   /* fnfErr */
        else {
            args[0] = (unsigned int)path; args[1] = ref; args[2] = 0;
            R[3] = oah_callppc_n("oah_ff_cfm", g_fs_make, 3, args);
        }
        if (getenv("OAH_VERBOSE")) fprintf(stderr, "[bridge] FSFindFolder(%d, CFMSupport) -> \"%s\" err=%d\n", dom, path, (int)R[3]);
        if (dom == -32766 && !R[3]) {                                  /* CFM crutch init is starting */
            unsigned int keep = R[3];
            oah_cfm_register_missing();
            R[3] = keep;
        }
        return;
    }
    if (!checked) {
        checked = 1;
        ffa = oah_guest_sym("CarbonCore", "_FSFindFolder");
        /* +0x50: addis r2,r31,0x2d   +0x5c: lwz r2,0x24c8(r2)   (r31 = ffa+0x10) */
        if (ffa && OSSwapBigToHostInt32(*(unsigned int *)(ffa + 0x50)) == 0x3c5f002du
                && OSSwapBigToHostInt32(*(unsigned int *)(ffa + 0x5c)) == 0x804224c8u)
            worker = OSSwapBigToHostInt32(*(unsigned int *)(ffa + 0x10 + 0x2d0000 + 0x24c8));
        if (getenv("OAH_VERBOSE")) fprintf(stderr, "[bridge] FSFindFolder at 0x%08x, worker 0x%08x\n", ffa, worker);
    }
    if (!worker) { R[3] = (unsigned int)-43; return; }
    {
        unsigned int args[6];
        args[0] = dom ? (unsigned int)(int)dom : 0xffff8001u;      /* 0 -> kOnAppropriateDisk */
        args[1] = type; args[2] = 0; args[3] = create; args[4] = ref; args[5] = 10;
        R[3] = oah_callppc_n("oah_ff", worker, 6, args);
    }
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[bridge] FSFindFolder(%d, '%.4s', %u) err=%d\n", dom,
                                       (char *)&(unsigned int){ OSSwapHostToBigInt32(type) }, create, (int)R[3]);
}
/* FSGetVolumeInfo(FSVolumeRefNum volume, ItemCount index, FSVolumeRefNum *actual,
                   FSVolumeInfoBitmap which, FSVolumeInfo *info, HFSUniStr255 *name,
                   FSRef *root).
   The 10.6 PPC CarbonCore builds its volume list from coreservicesd, which
   Mojave's daemon does not answer, so every index lookup failed with nsvErr.
   Here the list comes from Mojave's mount table (getfsstat, browsable mounts,
   "/" first); the refnum and root FSRef come from the guest's own
   FSPathMakeRef/FSGetCatalogInfo so they match the rest of the guest's File
   Manager. Results are written big-endian. */
#include <sys/mount.h>
#include <sys/attr.h>
/* Volume reference numbers (and root FSRefs) per mount point, from the guest's own FSGetCatalogInfo.
   Cached until the mount list changes: FSGetVolumeInfo runs on the main thread inside a bridged call
   (LaunchServices asks on every app switch) and each lookup was two guest calls, which have to wait
   for the callback lock - the main thread then sat behind the clipboard sync thread. */
static unsigned int g_mnt_gen;
static struct { char path[MNAMELEN]; short vref; unsigned char ref[80]; unsigned int gen; } g_vref_cache[64];
static int oah_guest_vref_for(const char *path, unsigned int rootref, short *vref) {
    static unsigned char ref[80], ci[160];
    unsigned int args[6];
    int slot = -1, i;
    for (i = 0; i < 64; i++) {
        if (g_vref_cache[i].gen == g_mnt_gen && g_mnt_gen && !strcmp(g_vref_cache[i].path, path)) {
            *vref = g_vref_cache[i].vref;
            if (rootref) memcpy((void *)rootref, g_vref_cache[i].ref, 80);
            return 0;
        }
        if (slot < 0 && g_vref_cache[i].gen != g_mnt_gen) slot = i;
    }
    if (!g_fs_make) g_fs_make = oah_guest_sym("CarbonCore", "_FSPathMakeRef");
    if (!g_fs_catinfo) g_fs_catinfo = oah_guest_sym("CarbonCore", "_FSGetCatalogInfo");
    if (!g_fs_make || !g_fs_catinfo) return -1;
    args[0] = (unsigned int)path; args[1] = (unsigned int)ref; args[2] = 0;
    if (oah_callppc_n("oah_fsvol_make", g_fs_make, 3, args)) return -1;
    memset(ci, 0, sizeof ci);
    args[0] = (unsigned int)ref; args[1] = 0x4 /* kFSCatInfoVolume */; args[2] = (unsigned int)ci; args[3] = 0; args[4] = 0; args[5] = 0;
    if (oah_callppc_n("oah_fsvol_cat", g_fs_catinfo, 6, args)) return -1;
    *vref = (short)OSSwapBigToHostInt16(*(unsigned short *)(ci + 2));   /* FSCatalogInfo.volume */
    if (rootref) memcpy((void *)rootref, ref, 80);
    if (slot >= 0 && g_mnt_gen && strlen(path) < MNAMELEN && !getenv("OAH_NO_VREFCACHE")) {
        snprintf(g_vref_cache[slot].path, MNAMELEN, "%s", path);
        g_vref_cache[slot].vref = *vref; memcpy(g_vref_cache[slot].ref, ref, 80); g_vref_cache[slot].gen = g_mnt_gen;
    }
    return 0;
}
static int oah_mount_list(struct statfs **out) {
    /* static: bridge handlers can run on a small signal stack */
    static struct statfs buf[64];
    static struct statfs all[64];
    static unsigned long long last;
    unsigned long long h = 1469598103934665603ULL;
    int n = getfsstat(all, sizeof all, MNT_NOWAIT), i, k = 0;
    const char *c;
    if (n < 0) n = 0;
    for (i = 0; i < n; i++) {                /* mount list changed: volume references are looked up again */
        for (c = all[i].f_mntonname; *c; c++) h = (h ^ (unsigned char)*c) * 1099511628211ULL;
        h = (h ^ (unsigned int)all[i].f_fsid.val[0]) * 1099511628211ULL;
    }
    if (h != last || !g_mnt_gen) { last = h; g_mnt_gen++; if (!g_mnt_gen) g_mnt_gen = 1; }
    for (i = 0; i < n; i++)
        if (!strcmp(all[i].f_mntonname, "/")) buf[k++] = all[i];
    for (i = 0; i < n && k < 64; i++) {
        if (!strcmp(all[i].f_mntonname, "/")) continue;
        if (all[i].f_flags & MNT_DONTBROWSE) continue;
        if (!strcmp(all[i].f_fstypename, "devfs") || !strcmp(all[i].f_fstypename, "autofs")) continue;
        buf[k++] = all[i];
    }
    *out = buf;
    return k;
}
static void oah_vol_name(const char *mnt, unsigned char *dst /* HFSUniStr255, guest */) {
    struct { unsigned int len; attrreference_t ref; char name[1024]; } ab;
    struct attrlist al;
    const char *nm;
    CFStringRef s;
    CFIndex n = 0, i;
    UniChar u[255];
    memset(&al, 0, sizeof al);
    al.bitmapcount = ATTR_BIT_MAP_COUNT; al.volattr = ATTR_VOL_INFO | ATTR_VOL_NAME;
    if (!getattrlist(mnt, &al, &ab, sizeof ab, 0)) nm = (const char *)&ab.ref + ab.ref.attr_dataoffset;
    else { nm = strrchr(mnt, '/'); nm = (nm && nm[1]) ? nm + 1 : mnt; }
    s = CFStringCreateWithCString(NULL, nm, kCFStringEncodingUTF8);
    if (s) {
        n = CFStringGetLength(s); if (n > 255) n = 255;
        CFStringGetCharacters(s, CFRangeMake(0, n), u);
        CFRelease(s);
    }
    *(unsigned short *)dst = OSSwapHostToBigInt16((unsigned short)n);
    for (i = 0; i < n; i++) ((unsigned short *)(dst + 2))[i] = OSSwapHostToBigInt16(u[i]);
}
static void oah_fs_get_volume_info(unsigned int *R) {
    short want = (short)R[3], vref = 0;
    unsigned int index = R[4], which = R[6];
    unsigned char *info = (unsigned char *)R[7];
    struct statfs *m, *hit = NULL;
    int n = oah_mount_list(&m), i;
    if (index > 0) {
        if ((int)index <= n) {
            hit = &m[index - 1];
            if (oah_guest_vref_for(hit->f_mntonname, R[9], &vref)) vref = (short)(-200 - (int)index);
        }
    } else {
        for (i = 0; i < n && !hit; i++) {
            short v;
            if (!oah_guest_vref_for(m[i].f_mntonname, 0, &v) && v == want) { hit = &m[i]; vref = v; }
        }
        if (hit && R[9]) oah_guest_vref_for(hit->f_mntonname, R[9], &vref);
    }
    if (!hit) { R[3] = (unsigned int)-35; return; }                      /* nsvErr */
    if (R[5]) *(unsigned short *)R[5] = OSSwapHostToBigInt16((unsigned short)vref);
    if (info) {
        unsigned long long bs = hit->f_bsize;
        unsigned long long files = hit->f_files > hit->f_ffree ? hit->f_files - hit->f_ffree : hit->f_files;
        memset(info, 0, 0x7e);
        if (which & 0x0010) OSWriteBigInt32(info, 0x20, (unsigned int)files);
        if (which & 0x0040) {
            OSWriteBigInt64(info, 0x28, (unsigned long long)hit->f_blocks * bs);
            OSWriteBigInt64(info, 0x30, (unsigned long long)hit->f_bavail * bs);
        }
        if (which & 0x0080) {
            OSWriteBigInt32(info, 0x38, (unsigned int)bs);
            OSWriteBigInt32(info, 0x3c, (unsigned int)hit->f_blocks);
            OSWriteBigInt32(info, 0x40, (unsigned int)hit->f_bavail);
        }
        if ((which & 0x1000) && !strcmp(hit->f_mntonname, "/") && !getenv("OAH_NO_APFS_HPLUS")) {
            unsigned int d, f;                                       /* finderInfo: blessed folder */
            oah_blessed_ids(&d, &f);
            OSWriteBigInt32(info, 0x54, d); OSWriteBigInt32(info, 0x58, f);
        }
        if (which & 0x2000)                                          /* read-only -> locked */
            OSWriteBigInt16(info, 0x74, (hit->f_flags & MNT_RDONLY) ? 0x0080 : 0);
        if (which & 0x4000)
            /* APFS too: 10.6 has no signature for it, and installers that want a Mac OS
               Extended target left the startup disk out of their list (CS2) */
            OSWriteBigInt16(info, 0x78, (!strcmp(hit->f_fstypename, "hfs") ||
                                         (!strcmp(hit->f_fstypename, "apfs") && !getenv("OAH_NO_APFS_HPLUS"))) ? 0x482B : 0);
    }
    if (R[8]) oah_vol_name(hit->f_mntonname, (unsigned char *)R[8]);
    if (getenv("OAH_VERBOSE"))
        fprintf(stderr, "[bridge] FSGetVolumeInfo vol=%d idx=%u which=0x%x -> %s vref=%d ro=%d\n",
                want, index, which, hit->f_mntonname, vref, (hit->f_flags & MNT_RDONLY) ? 1 : 0);
    R[3] = 0;
}
/* CGGetActiveDisplayList(uint32_t max, CGDirectDisplayID *list, uint32_t *count):
   native display IDs, written big-endian (the other CG display bridges take
   native IDs too). list may be NULL to ask only for the count. */
static void oah_cg_get_active_display_list(unsigned int *R) {
    static int (*fn)(unsigned int, unsigned int *, unsigned int *);
    unsigned int max = R[3], *list = (unsigned int *)R[4], *count = (unsigned int *)R[5];
    unsigned int buf[32], n = 0, i;
    int err;
    if (!fn && oah_load_cg())
        fn = (int (*)(unsigned int, unsigned int *, unsigned int *))dlsym(RTLD_DEFAULT, "CGGetActiveDisplayList");
    if (!fn) { R[3] = 1000; return; }                         /* kCGErrorFailure */
    if (max > 32) max = 32;
    err = fn(list ? max : 0, list ? buf : 0, &n);
    if (!err && list) for (i = 0; i < n && i < max; i++) list[i] = OSSwapHostToBigInt32(buf[i]);
    if (count) *count = OSSwapHostToBigInt32(n);
    if (getenv("OAH_VERBOSE"))
        fprintf(stderr, "[bridge] CGGetActiveDisplayList(max=%u list=%p) -> err=%d n=%u first=0x%x\n",
                R[3], list, err, n, n ? buf[0] : 0);
    R[3] = (unsigned int)err;
}
/* ===== native CF -> guest (PowerPC) CF =====
   The guest has its own CoreFoundation, so native CF objects cannot be handed
   to it. Rebuild them with the guest's CF functions (strings, numbers,
   booleans, arrays, dictionaries; anything else becomes NULL). */
enum { G_DICT_NEW, G_DICT_SET, G_ARR_NEW, G_ARR_ADD, G_NUM_NEW, G_STR_NEW, G_RELEASE,
       G_DKEYCB, G_DVALCB, G_ACB, G_TRUE, G_FALSE, G_NCF };
static const char *g_cf_names[G_NCF] = {
    "_CFDictionaryCreateMutable", "_CFDictionarySetValue", "_CFArrayCreateMutable", "_CFArrayAppendValue",
    "_CFNumberCreate", "_CFStringCreateWithCString", "_CFRelease",
    "_kCFTypeDictionaryKeyCallBacks", "_kCFTypeDictionaryValueCallBacks", "_kCFTypeArrayCallBacks",
    "_kCFBooleanTrue", "_kCFBooleanFalse" };
static unsigned int g_cf[G_NCF];
static int g_cf_state;
static int oah_guest_cf_load(void) {
    int i;
    if (g_cf_state) return g_cf_state == 1;
    g_cf_state = 1;
    for (i = 0; i < G_NCF; i++)
        if (!(g_cf[i] = oah_guest_sym("CoreFoundation", g_cf_names[i]))) g_cf_state = 2;
    return g_cf_state == 1;
}
static unsigned int gcall(int f, int n, unsigned int a0, unsigned int a1, unsigned int a2, unsigned int a3) {
    unsigned int a[4];
    a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a3;
    return oah_callppc_n("oah_cf", g_cf[f], n, a);
}
static unsigned int oah_cf_to_guest(CFTypeRef o) {
    CFTypeID t;
    if (!o || !oah_guest_cf_load()) return 0;
    t = CFGetTypeID(o);
    if (t == CFStringGetTypeID()) {
        char buf[1024];
        if (!CFStringGetCString((CFStringRef)o, buf, sizeof buf, kCFStringEncodingUTF8)) return 0;
        return gcall(G_STR_NEW, 3, 0, (unsigned int)buf, 0x08000100u, 0);
    }
    if (t == CFBooleanGetTypeID())
        return OSSwapBigToHostInt32(*(unsigned int *)g_cf[o == kCFBooleanTrue ? G_TRUE : G_FALSE]);
    if (t == CFNumberGetTypeID()) {
        static unsigned long long be;   /* guest reads it big-endian */
        if (CFNumberIsFloatType((CFNumberRef)o)) {
            double d; unsigned long long u;
            CFNumberGetValue((CFNumberRef)o, kCFNumberFloat64Type, &d);
            memcpy(&u, &d, 8); be = OSSwapHostToBigInt64(u);
            return gcall(G_NUM_NEW, 3, 0, kCFNumberFloat64Type, (unsigned int)&be, 0);
        } else {
            long long v;
            CFNumberGetValue((CFNumberRef)o, kCFNumberSInt64Type, &v);
            be = OSSwapHostToBigInt64((unsigned long long)v);
            return gcall(G_NUM_NEW, 3, 0, kCFNumberSInt64Type, (unsigned int)&be, 0);
        }
    }
    if (t == CFArrayGetTypeID()) {
        CFIndex i, n = CFArrayGetCount((CFArrayRef)o);
        unsigned int ga = gcall(G_ARR_NEW, 3, 0, 0, g_cf[G_ACB], 0);
        for (i = 0; ga && i < n; i++) {
            unsigned int gv = oah_cf_to_guest(CFArrayGetValueAtIndex((CFArrayRef)o, i));
            if (gv) { gcall(G_ARR_ADD, 2, ga, gv, 0, 0); gcall(G_RELEASE, 1, gv, 0, 0, 0); }
        }
        return ga;
    }
    if (t == CFDictionaryGetTypeID()) {
        CFIndex i, n = CFDictionaryGetCount((CFDictionaryRef)o);
        const void **k = (const void **)malloc(n * 2 * sizeof(void *) + 1), **v = k + n;
        unsigned int gd = gcall(G_DICT_NEW, 4, 0, 0, g_cf[G_DKEYCB], g_cf[G_DVALCB]);
        CFDictionaryGetKeysAndValues((CFDictionaryRef)o, k, v);
        for (i = 0; gd && i < n; i++) {
            unsigned int gk = oah_cf_to_guest(k[i]), gv = gk ? oah_cf_to_guest(v[i]) : 0;
            if (gk && gv) gcall(G_DICT_SET, 3, gd, gk, gv, 0);
            if (gk) gcall(G_RELEASE, 1, gk, 0, 0, 0);
            if (gv) gcall(G_RELEASE, 1, gv, 0, 0, 0);
        }
        free(k);
        return gd;
    }
    return 0;
}
#include "oah_ls.h"
#include "oah_prt.h"
#include "oah_clip.h"
#include "oah_aen.h"
#include "oah_dnd.h"
#include "oah_sfl.h"
/* GetNewCWindow(short windowID, void *wStorage, WindowRef behind).
   Read the 'WIND' template (big-endian) through the guest Resource Manager
   (GetResource in the guest CarbonCore, so the app's own resource chain is
   used); if that gives nothing, parse the bundle's data-fork .rsrc files
   directly. Then build the window with Mojave's native NewCWindow; the guest
   gets the native WindowRef, like CreateNewWindow. wStorage is ignored. */
static int oah_rsrc_file_find(const char *file, unsigned int type, short id, unsigned char *out, int max) {
    static unsigned char buf[1 << 20];
    int fd = open(file, O_RDONLY), n, t, r, nt, nr;
    unsigned int doff, moff, tl;
    if (fd < 0) return -1;
    n = (int)read(fd, buf, sizeof buf);
    close(fd);
    if (n < 16) return -1;
#define RB32(o) ((unsigned int)buf[o] << 24 | (unsigned int)buf[(o) + 1] << 16 | (unsigned int)buf[(o) + 2] << 8 | buf[(o) + 3])
#define RB16(o) ((unsigned int)buf[o] << 8 | buf[(o) + 1])
    doff = RB32(0); moff = RB32(4);
    if (moff + 30 > (unsigned int)n) return -1;
    tl = moff + RB16(moff + 24);
    nt = (int)RB16(tl) + 1;
    for (t = 0; t < nt; t++) {
        unsigned int te = tl + 2 + t * 8, rl;
        if (te + 8 > (unsigned int)n) return -1;
        if (RB32(te) != type) continue;
        nr = (int)RB16(te + 4) + 1;
        rl = tl + RB16(te + 6);
        for (r = 0; r < nr; r++) {
            unsigned int re = rl + r * 12, dp, len;
            if (re + 12 > (unsigned int)n) return -1;
            if ((short)RB16(re) != id) continue;
            dp = doff + (RB32(re + 4) & 0xffffff);
            if (dp + 4 > (unsigned int)n) return -1;
            len = RB32(dp);
            if (dp + 4 + len > (unsigned int)n) return -1;
            if ((int)len > max) len = max;
            memcpy(out, buf + dp + 4, len);
            return (int)len;
        }
    }
#undef RB32
#undef RB16
    return -1;
}
static int oah_bundle_rsrc_find(unsigned int type, short id, unsigned char *out, int max) {
    static char path[PATH_MAX], f[PATH_MAX + 256];
    const char *exe = getenv("OAH_GUEST_EXE"), *base;
    const char *lproj[] = { "", "English.lproj/", "en.lproj/" };
    char *m, exe_name[256];
    int i, n;
    if (!exe || !realpath(exe, path)) return -1;
    base = strrchr(path, '/'); base = base ? base + 1 : path;
    snprintf(exe_name, sizeof exe_name, "%s", base);
    m = strstr(path, ".app/Contents/MacOS/");
    if (!m) return -1;
    m[14] = 0;   /* path now ends at ".app/Contents/" */
    for (i = 0; i < 3; i++) {
        snprintf(f, sizeof f, "%sResources/%s%s.rsrc", path, lproj[i], exe_name);
        n = oah_rsrc_file_find(f, type, id, out, max);
        if (n < 0) { snprintf(f, sizeof f, "%sResources/%sLocalized.rsrc", path, lproj[i]); n = oah_rsrc_file_find(f, type, id, out, max); }
        if (n >= 0) {
            if (getenv("OAH_VERBOSE")) fprintf(stderr, "[bridge] resource %.4s %d from %s\n", (char *)&(unsigned int){ OSSwapHostToBigInt32(type) }, id, f);
            return n;
        }
    }
    return -1;
}
static void oah_get_new_cwindow(unsigned int *R) {
    static unsigned int g_getres;
    static unsigned char tmpl[512];
    typedef void *(*newcwin_t)(void *, const short *, const unsigned char *, unsigned char, short, void *, unsigned char, long);
    static newcwin_t ncw;
    short id = (short)R[3];
    unsigned int behind = R[5], h, args[2];
    int len = -1, src = 0;
    short rect[4], procID;
    unsigned char visible, goAway, title[256];
    unsigned int refCon;
    void *w;
    if (!g_getres) g_getres = oah_guest_sym("CarbonCore", "_GetResource");
    if (g_getres) {
        args[0] = 0x57494e44u; /* 'WIND' */ args[1] = (unsigned int)(int)id;
        h = oah_callppc_n("oah_getres", g_getres, 2, args);
        if (h && h < 0xc0000000u && oah_readable(h, 4)) {
            unsigned int mp = OSSwapBigToHostInt32(*(unsigned int *)h);
            if (mp && oah_readable(mp, 18)) {
                len = 0;
                while (len < 18 + 256 && oah_readable(mp + len, 1)) { tmpl[len] = ((unsigned char *)mp)[len]; len++; }
                src = 1;
            }
        }
        if (getenv("OAH_VERBOSE")) fprintf(stderr, "[bridge] GetNewCWindow(%d): guest GetResource -> handle %08x\n", id, h);
    }
    if (len < 18) { len = oah_bundle_rsrc_find(0x57494e44u, id, tmpl, sizeof tmpl); src = 2; }
    if (len < 18) {
        fprintf(stderr, "[bridge] GetNewCWindow(%d): no WIND resource -> NULL\n", id);
        R[3] = 0;
        return;
    }
    for (int k = 0; k < 4; k++) rect[k] = (short)((tmpl[2 * k] << 8) | tmpl[2 * k + 1]);
    procID = (short)((tmpl[8] << 8) | tmpl[9]);
    visible = tmpl[10] ? 1 : 0;
    goAway = tmpl[12] ? 1 : 0;
    refCon = (unsigned int)tmpl[14] << 24 | (unsigned int)tmpl[15] << 16 | (unsigned int)tmpl[16] << 8 | tmpl[17];
    title[0] = 0;
    if (len > 18) {
        unsigned int tl = tmpl[18];
        if ((int)(19 + tl) > len) tl = len - 19;
        title[0] = (unsigned char)tl;
        memcpy(title + 1, tmpl + 19, tl);
    }
    if (!g_carbon_handle)
        g_carbon_handle = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon", RTLD_LAZY | RTLD_GLOBAL);
    if (!ncw && g_carbon_handle) ncw = (newcwin_t)dlsym(g_carbon_handle, "NewCWindow");
    if (!ncw) { fprintf(stderr, "[bridge] GetNewCWindow: native NewCWindow missing\n"); R[3] = 0; return; }
    if (!g_fg_done) { g_fg_done = 1; oah_make_foreground(); }
    w = ncw(NULL, rect, title, visible, procID, (void *)behind, goAway, (long)refCon);
    R[3] = (unsigned int)w;
    fprintf(stderr, "[bridge] GetNewCWindow(%d) [%s] rect=(%d,%d,%d,%d) procID=%d vis=%d goAway=%d refCon=%08x title=\"%.*s\" -> %p\n",
            id, src == 1 ? "guest rsrc" : "bundle rsrc", rect[0], rect[1], rect[2], rect[3], procID, visible, goAway,
            refCon, title[0], title + 1, w);
}
static unsigned int oah_guest_empty_array(void) {
    return oah_guest_cf_load() ? gcall(G_ARR_NEW, 3, 0, 0, g_cf[G_ACB], 0) : 0;
}
/* CGDisplayCurrentMode / CGDisplayAvailableModes return objects the caller does not own, so the
   available list is converted once per display and kept.  Each guest mode dictionary is remembered
   next to the native one it came from, so CGDisplayBestModeForParameters(AndRefreshRate) can answer
   with the guest copy of the native best mode, and CGDisplaySwitchToMode can hand the native mode
   to native CGDisplaySwitchToMode (Build-a-lot / QuickTime full screen asked the guest CG, whose
   mode list - built from WindowServer display info this process does not have - was empty, so
   BestMode returned NULL and the game ran 1024x768 in the corner of the 1440x900 screen).
   As on Snow Leopard, the mode reverts when the display is released or the process exits. */
#define OAH_MAXMODES 128
typedef struct { unsigned int disp, gavail, n; CFArrayRef navail; unsigned int g[OAH_MAXMODES]; } oah_modeset_t;
static oah_modeset_t g_modesets[4]; static unsigned int g_nmodesets;
static oah_modeset_t *oah_modeset(unsigned int id) {
    static CFArrayRef (*fa)(unsigned int);
    oah_modeset_t *m; CFIndex i, n;
    unsigned int k;
    for (k = 0; k < g_nmodesets; k++) if (g_modesets[k].disp == id) return &g_modesets[k];
    if (g_nmodesets >= 4 || !oah_guest_cf_load()) return 0;
    if (!fa && oah_load_cg()) fa = (CFArrayRef (*)(unsigned int))dlsym(RTLD_DEFAULT, "CGDisplayAvailableModes");
    if (!fa) return 0;
    m = &g_modesets[g_nmodesets];
    memset(m, 0, sizeof *m);
    m->disp = id;
    m->navail = fa(id);
    if (m->navail) CFRetain(m->navail);
    m->gavail = gcall(G_ARR_NEW, 3, 0, 0, g_cf[G_ACB], 0);
    n = m->navail ? CFArrayGetCount(m->navail) : 0;
    for (i = 0; m->gavail && i < n && m->n < OAH_MAXMODES; i++) {
        unsigned int gv = oah_cf_to_guest(CFArrayGetValueAtIndex(m->navail, i));
        if (!gv) continue;
        gcall(G_ARR_ADD, 2, m->gavail, gv, 0, 0);
        m->g[m->n++] = gv;                      /* the array keeps it alive; drop our reference */
        gcall(G_RELEASE, 1, gv, 0, 0, 0);
    }
    g_nmodesets++;
    return m;
}
static int oah_mode_num(CFDictionaryRef d) {
    int v = -1; CFNumberRef nm = d ? (CFNumberRef)CFDictionaryGetValue(d, CFSTR("Mode")) : 0;
    if (nm) CFNumberGetValue(nm, kCFNumberIntType, &v);
    return v;
}
/* guest copy of a native mode dictionary (by its "Mode" number), 0 if not in the list */
static unsigned int oah_mode_to_guest(oah_modeset_t *m, CFDictionaryRef nd) {
    unsigned int i; int want = oah_mode_num(nd);
    if (!m || !nd || !m->navail) return 0;
    for (i = 0; i < m->n && i < (unsigned)CFArrayGetCount(m->navail); i++)
        if (oah_mode_num((CFDictionaryRef)CFArrayGetValueAtIndex(m->navail, i)) == want) return m->g[i];
    return 0;
}
static void oah_cg_display_modes(unsigned int *R, int avail) {
    static CFDictionaryRef (*fc)(unsigned int);
    unsigned int id = R[3], g = 0;
    oah_modeset_t *m = oah_modeset(id);
    if (avail) g = m ? m->gavail : 0;
    else {
        if (!fc && oah_load_cg()) fc = (CFDictionaryRef (*)(unsigned int))dlsym(RTLD_DEFAULT, "CGDisplayCurrentMode");
        if (fc) {
            CFDictionaryRef cd = fc(id);
            static unsigned int other[16]; static int nother;
            if (!(g = oah_mode_to_guest(m, cd)) && cd) {   /* not in the list: a stand-alone copy, kept */
                g = oah_cf_to_guest(cd);
                if (g && nother < 16) other[nother++] = g;
            }
        }
    }
    if (getenv("OAH_VERBOSE") || getenv("OAH_MODETRACE"))
        fprintf(stderr, "[modes] %s(0x%x) -> guest CF 0x%08x\n", avail ? "CGDisplayAvailableModes" : "CGDisplayCurrentMode", id, g);
    R[3] = g;
}
/* CGDisplayBestModeForParameters(display, bpp, w, h, boolean_t *exact)
   CGDisplayBestModeForParametersAndRefreshRate(display, bpp, w, h, double refresh (f1), boolean_t *exact) */
static void oah_cg_best_mode(unsigned int *R, int with_rate) {
    static CFDictionaryRef (*f1)(unsigned int, size_t, size_t, size_t, int *);
    static CFDictionaryRef (*f2)(unsigned int, size_t, size_t, size_t, double, int *);
    double rate = ((double *)(R + 0x1e8 / 4))[1];
    unsigned int exactp = with_rate ? R[9] : R[7];   /* the double takes the r7/r8 slots (Darwin PPC ABI) */
    int exact = 0; CFDictionaryRef nd = 0;
    oah_modeset_t *m = oah_modeset(R[3]);
    if (oah_load_cg()) {
        if (!f1) f1 = (CFDictionaryRef (*)(unsigned int, size_t, size_t, size_t, int *))dlsym(RTLD_DEFAULT, "CGDisplayBestModeForParameters");
        if (!f2) f2 = (CFDictionaryRef (*)(unsigned int, size_t, size_t, size_t, double, int *))dlsym(RTLD_DEFAULT, "CGDisplayBestModeForParametersAndRefreshRate");
    }
    if (with_rate && f2) nd = f2(R[3], R[4], R[5], R[6], rate, &exact);
    else if (!with_rate && f1) nd = f1(R[3], R[4], R[5], R[6], &exact);
    if (exactp) oah_put32(exactp, (unsigned int)exact);
    R[3] = oah_mode_to_guest(m, nd);
    if (getenv("OAH_VERBOSE") || getenv("OAH_MODETRACE"))
        fprintf(stderr, "[modes] CGDisplayBestModeForParameters%s(%x, %u bpp, %ux%u, %g) -> mode %d exact %d guest 0x%08x\n",
                with_rate ? "AndRefreshRate" : "", m ? m->disp : 0, R[4], R[5], R[6], rate, oah_mode_num(nd), exact, R[3]);
}
/* CGDisplaySwitchToMode(display, CFDictionaryRef mode) */
static void oah_cg_switch_mode(unsigned int *R) {
    static int (*fs)(unsigned int, CFDictionaryRef);
    oah_modeset_t *m = oah_modeset(R[3]);
    unsigned int i; int err = 1001;   /* kCGErrorIllegalArgument */
    CFDictionaryRef nd = 0;
    if (!fs && oah_load_cg()) fs = (int (*)(unsigned int, CFDictionaryRef))dlsym(RTLD_DEFAULT, "CGDisplaySwitchToMode");
    if (m && m->navail) for (i = 0; i < m->n; i++) if (m->g[i] == R[4]) { nd = (CFDictionaryRef)CFArrayGetValueAtIndex(m->navail, i); break; }
    if (nd && fs && !getenv("OAH_NO_MODESWITCH")) err = fs(m->disp, nd);
    else if (nd) err = 0;
    fprintf(stderr, "[modes] CGDisplaySwitchToMode(%x, guest %08x = mode %d) -> %d\n", R[3], R[4], oah_mode_num(nd), err);
    R[3] = (unsigned int)err;
}
static int oah_evt_tramp(void *next, void *ev, void *ud) {
    oah_ppccb_t *c = (oah_ppccb_t *)ud;
    unsigned int args[3];
    int r;
    args[0] = (unsigned int)next; args[1] = (unsigned int)ev; args[2] = c->ud;
    r = (int)oah_callppc_n("oah_evt", c->fn, 3, args);
    if (getenv("OAH_VERBOSE") && g_evt_calls < 8)
        fprintf(stderr, "[evt] handler ppc=0x%08x ev=%p -> %d\n", c->fn, ev, r);
    g_evt_calls++;
    return r;
}
static void oah_timer_tramp(void *t, void *ud) {
    oah_ppccb_t *c = (oah_ppccb_t *)ud;
    unsigned int args[2];
    args[0] = (unsigned int)t; args[1] = c->ud;
    oah_callppc_n("oah_timer", c->fn, 2, args);
    g_timer_calls++;
}
/* Convert event parameter data between PPC (big-endian) and native layout. */
static void oah_swap_evparam(unsigned int type, unsigned char *dst, const unsigned char *src, unsigned int size) {
    unsigned int i;
    memmove(dst, src, size);
    switch (type) {
    case 'hcmd':   /* HICommand: UInt32 attributes, UInt32 commandID, MenuRef, UInt16 index */
        for (i = 0; i + 4 <= size && i < 12; i += 4) *(unsigned int *)(dst + i) = OSSwapInt32(*(const unsigned int *)(src + i));
        if (size >= 14) *(unsigned short *)(dst + 12) = OSSwapInt16(*(const unsigned short *)(src + 12));
        return;
    case 'QDpt': case 'shor': case 'utxt': case 'ushr': case 'mbtn': case 'mwax':   /* 16-bit fields */
        for (i = 0; i + 2 <= size; i += 2) *(unsigned short *)(dst + i) = OSSwapInt16(*(const unsigned short *)(src + i));
        return;
    case 'TEXT': case 'bool': case 'char':
        return;
    case 'doub':
        for (i = 0; i + 8 <= size; i += 8) { unsigned long long q; memcpy(&q, src + i, 8); q = OSSwapInt64(q); memcpy(dst + i, &q, 8); }
        return;
    default:       /* magn long wind hipt hisz hirc menu ctrl type enum ... : 32-bit fields */
        for (i = 0; i + 4 <= size; i += 4) *(unsigned int *)(dst + i) = OSSwapInt32(*(const unsigned int *)(src + i));
        return;
    }
}
static void br_set_retstub(unsigned int *R) {
    if (getenv("OAH_GLTEST")) oah_gltest("guest");
    g_retstub = R[3];
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[evt] return stub at 0x%08x\n", g_retstub);
    R[3] = 0;
}
/* InstallEventHandler(target, upp, n, const EventTypeSpec *list, ud, EventHandlerRef *out) */
#include "oah_input.h"
/* OAH_EVTTRACE: native monitors on the dispatcher and application targets
   that log every mouse/keyboard event, to see where input gets lost. */
static int oah_evmon(void *next, void *ev, void *ud) {
    static unsigned long cnt;
    unsigned int (*gc)(void *) = (unsigned int (*)(void *))oah_csym("GetEventClass");
    unsigned int (*gk)(void *) = (unsigned int (*)(void *))oah_csym("GetEventKind");
    int (*gp)(void *, unsigned int, unsigned int, unsigned int *, unsigned int, unsigned int *, void *) =
        (int (*)(void *, unsigned int, unsigned int, unsigned int *, unsigned int, unsigned int *, void *))oah_csym("GetEventParameter");
    short pt[2] = {0, 0};
    unsigned int cls = gc ? gc(ev) : 0, kind = gk ? gk(ev) : 0, w = 0;
    if (ud && !strcmp((char *)ud, "disp")) { double (*now)(void) = (double (*)(void))oah_csym("GetCurrentEventTime"); if (now) g_real_input_ts = now(); }
    if (!inp_tr()) return -9874;
    if (gp) { gp(ev, 'mloc', 'QDpt', 0, 4, 0, pt); gp(ev, 'wind', 'wind', 0, 4, 0, &w); }
    if (cnt++ < 400 && !(cls == 'mous' && (kind == 5 || kind == 6) && cnt > 40))
        fprintf(stderr, "[evmon:%s] %.4s/%u mloc=(%d,%d) wind=%08x\n", (char *)ud,
                (char *)&(unsigned int){OSSwapInt32(cls)}, kind, pt[1], pt[0], w);
    (void)next;
    return -9874;   /* eventNotHandledErr: keep propagating */
}
static void oah_evmon_install(void) {
    static int done;
    int (*ins)(void *, void *, unsigned int, const unsigned int *, void *, void **) =
        (int (*)(void *, void *, unsigned int, const unsigned int *, void *, void **))oah_csym("InstallEventHandler");
    void *(*disp)(void) = (void *(*)(void))oah_csym("GetEventDispatcherTarget");
    void *(*app)(void) = (void *(*)(void))oah_csym("GetApplicationEventTarget");
    unsigned int specs[] = { 'mous', 1, 'mous', 2, 'mous', 5, 'mous', 6, 'mous', 10, 'keyb', 1, 'keyb', 2, 'keyb', 3, 'keyb', 4 };
    void *h;
    if (done || !ins) return;
    done = 1;
    if (disp) { int r = ins(disp(), (void *)oah_evmon, 9, specs, "disp", &h); if (inp_tr()) fprintf(stderr, "[evmon] dispatcher -> %d\n", r); }
    if (app && inp_tr()) fprintf(stderr, "[evmon] application %p -> %d\n", app(), ins(app(), (void *)oah_evmon, 9, specs, "app", &h));
}
static void br_InstallEventHandler(unsigned int *R) {
    int (*fn)(void *, void *, unsigned int, const unsigned int *, void *, void **) =
        (int (*)(void *, void *, unsigned int, const unsigned int *, void *, void **))oah_csym("InstallEventHandler");
    oah_evmon_install();
    unsigned int n = R[5], i, specs[128], *list = (unsigned int *)R[6];
    oah_ppccb_t *c;
    void *href = 0;
    int err;
    if (!fn) { R[3] = 0xffffffffu; return; }
    if (n > 64) n = 64;
    for (i = 0; i < 2 * n; i++) specs[i] = OSSwapBigToHostInt32(list[i]);
    c = (oah_ppccb_t *)malloc(sizeof *c);
    c->fn = R[4]; c->ud = R[7];
    err = fn((void *)R[3], (void *)oah_evt_tramp, n, specs, c, &href);
    for (i = 0; i < n; i++) if (specs[2 * i] == 'mous') { oah_input_start(); break; }
    if (R[8]) *(unsigned int *)R[8] = OSSwapHostToBigInt32((unsigned int)href);
    if (getenv("OAH_VERBOSE"))
        fprintf(stderr, "[evt] InstallEventHandler target=%08x ppc=%08x n=%u first=%.4s/%u -> %d\n",
                R[3], R[4], n, n ? (char *)&(unsigned int){OSSwapInt32(specs[0])} : "", n ? specs[1] : 0, err);
    R[3] = (unsigned int)err;
}
/* GetEventParameter(ev, name, desiredType, *outType, bufSize, *outSize, outData) */
static void br_GetEventParameter(unsigned int *R) {
    int (*fn)(void *, unsigned int, unsigned int, unsigned int *, unsigned int, unsigned int *, void *) =
        (int (*)(void *, unsigned int, unsigned int, unsigned int *, unsigned int, unsigned int *, void *))oah_csym("GetEventParameter");
    unsigned int want = R[5], bufsz = R[7], pdata = R[9], act = 0, sz = 0;
    unsigned char tmp[1024];
    int err;
    if (!fn) { R[3] = 0xffffffffu; return; }
    if (pdata && bufsz <= sizeof tmp) {
        err = fn((void *)R[3], R[4], want, &act, bufsz, &sz, tmp);
        if (!err) oah_swap_evparam(want == '****' ? act : want, (unsigned char *)pdata, tmp, sz < bufsz ? sz : bufsz);
    } else {
        err = fn((void *)R[3], R[4], want, &act, bufsz, &sz, (void *)pdata);
    }
    if (R[6]) *(unsigned int *)R[6] = OSSwapHostToBigInt32(act);
    if (R[8]) *(unsigned int *)R[8] = OSSwapHostToBigInt32(sz);
    R[3] = (unsigned int)err;
}
/* SetEventParameter(ev, name, type, size, const void *data) */
static void br_SetEventParameter(unsigned int *R) {
    int (*fn)(void *, unsigned int, unsigned int, unsigned int, const void *) =
        (int (*)(void *, unsigned int, unsigned int, unsigned int, const void *))oah_csym("SetEventParameter");
    unsigned int size = R[6];
    unsigned char tmp[1024];
    if (!fn) { R[3] = 0xffffffffu; return; }
    if (R[7] && size <= sizeof tmp) {
        oah_swap_evparam(R[5], tmp, (const unsigned char *)R[7], size);
        R[3] = (unsigned int)fn((void *)R[3], R[4], R[5], size, tmp);
    } else {
        R[3] = (unsigned int)fn((void *)R[3], R[4], R[5], size, (const void *)R[7]);
    }
}
/* InstallEventLoopTimer(loop, double delay, double interval, upp, ud, EventLoopTimerRef *out)
   PPC: r3 loop, f1 delay (shadows r4-r5), f2 interval (r6-r7), r8 upp, r9 ud, r10 out */
static void br_InstallEventLoopTimer(unsigned int *R) {
    int (*fn)(void *, double, double, void *, void *, void **) =
        (int (*)(void *, double, double, void *, void *, void **))oah_csym("InstallEventLoopTimer");
    double *F = (double *)((unsigned char *)R + 0x1e8);
    oah_ppccb_t *c;
    void *t = 0;
    int err;
    if (!fn) { R[3] = 0xffffffffu; return; }
    c = (oah_ppccb_t *)malloc(sizeof *c);
    c->fn = R[8]; c->ud = R[9];
    err = fn((void *)R[3], F[1], F[2], (void *)oah_timer_tramp, c, &t);
    if (R[10]) *(unsigned int *)R[10] = OSSwapHostToBigInt32((unsigned int)t);
    if (getenv("OAH_VERBOSE"))
        fprintf(stderr, "[evt] InstallEventLoopTimer delay=%g interval=%g ppc=%08x -> %d\n", F[1], F[2], R[8], err);
    R[3] = (unsigned int)err;
}

/* 14: the guest audio thread's body (oah_au.h): wait a little, or hand it a render callback to run */
static void br_au_wait(unsigned int *R) {
    static unsigned long n;
    if (n++ < 3 && oah_autr()) fprintf(stderr, "[au] audio thread tick %lu (main %d)\n", n, pthread_main_np());
    oah_au_thread_wait(R);
}
/* 15: the guest audio thread ran a render callback: its samples go to the ring */
static void br_au_done(unsigned int *R) { oah_au_thread_done(R); }
static void (*const g_bridge_fns[])(unsigned int *) = {
    0, br_test_sum, br_CGMainDisplayID, br_CGDisplayBounds, br_call_ppc, br_ret_stub, br_fp_probe, br_dump_windows,
    br_set_retstub, br_InstallEventHandler, br_GetEventParameter, br_SetEventParameter, br_InstallEventLoopTimer,
    br_gli_disp, br_au_wait, br_au_done
};
static const char *const g_bridge_names[] = {
    "", "test_sum", "CGMainDisplayID", "CGDisplayBounds", "call_ppc", "ret_stub", "fp_probe", "dump_windows",
    "set_retstub", "InstallEventHandler", "GetEventParameter", "SetEventParameter", "InstallEventLoopTimer",
    "gli_dispatch", "au_wait", "au_done"
};
#define OAH_NBRIDGES (sizeof g_bridge_fns / sizeof g_bridge_fns[0])
static int oah_is_bridge(unsigned int id) {
    return (id > 0 && id < OAH_NBRIDGES) || (id >= OAH_GEN_BASE && id < OAH_GEN_BASE + OAH_NGEN) || oah_gt_is(id);
}
static const char *oah_bridge_name(unsigned int id);
static const char *oah_regfile(unsigned int a, char *buf, int n);
#include "oah_hang.h"
/* OAH_BPROF=1: per-bridge call counts and time spent in the native side, dumped every 30 s
   and at exit (top 40 by time), to see where startup time goes. */
#include <mach/mach_time.h>
#define OAH_BPROF_N 2048
static unsigned long g_bprof_n[OAH_BPROF_N];
static unsigned long long g_bprof_t[OAH_BPROF_N], g_bprof_last;
static unsigned long g_gtprof_n[GT_MAX];          /* gtrace hooks (fixes), same profile */
static unsigned long long g_gtprof_t[GT_MAX];
static const char *oah_bridge_name(unsigned int id);
static void oah_bprof_dump(void) {
    static mach_timebase_info_data_t tb;
    unsigned int i, j, order[40], n = 0;
    unsigned long total = 0;
    unsigned long long ttot = 0;
    if (!tb.denom) mach_timebase_info(&tb);
    for (i = 0; i < OAH_BPROF_N; i++) { total += g_bprof_n[i]; ttot += g_bprof_t[i]; }
    for (j = 0; j < 40; j++) {
        unsigned int best = 0; unsigned long long bt = 0;
        for (i = 0; i < OAH_BPROF_N; i++) {
            unsigned int k, used = 0;
            for (k = 0; k < n; k++) if (order[k] == i) used = 1;
            if (!used && g_bprof_t[i] > bt) { bt = g_bprof_t[i]; best = i; }
        }
        if (!bt) break;
        order[n++] = best;
    }
    fprintf(stderr, "[bprof] %lu bridge calls, %.1f ms native time\n", total, ttot * tb.numer / tb.denom / 1e6);
    for (j = 0; j < n; j++)
        fprintf(stderr, "[bprof] %8lu calls %9.1f ms  %s\n", g_bprof_n[order[j]],
                g_bprof_t[order[j]] * tb.numer / tb.denom / 1e6, oah_bridge_name(order[j]));
    {   unsigned long gn = 0; unsigned long long gt = 0;
        for (i = 0; i < GT_MAX; i++) { gn += g_gtprof_n[i]; gt += g_gtprof_t[i]; }
        fprintf(stderr, "[bprof] %lu hook calls (guest fixes), %.1f ms\n", gn, gt * tb.numer / tb.denom / 1e6);
        for (i = 0; i < GT_MAX; i++)
            if (g_gtprof_n[i] >= 100 || g_gtprof_t[i] * tb.numer / tb.denom > 5000000ULL)
                fprintf(stderr, "[bprof] %8lu calls %9.1f ms  hook %s\n", g_gtprof_n[i],
                        g_gtprof_t[i] * tb.numer / tb.denom / 1e6, g_gt[i].sym);
    }
}
static int oah_bprof_on(void) {
    static int on = -1;
    if (on < 0) { on = getenv("OAH_BPROF") ? 1 : 0; if (on) atexit(oah_bprof_dump); }
    return on;
}
static void oah_bprof_note(unsigned int id, unsigned long long dt) {
    unsigned long long now = mach_absolute_time();
    if (id < OAH_BPROF_N) { g_bprof_n[id]++; g_bprof_t[id] += dt; }
    if (!g_bprof_last) g_bprof_last = now;
    if (now - g_bprof_last > 30ULL * 1000000000ULL) { g_bprof_last = now; oah_bprof_dump(); }
    /* On demand: `touch /tmp/oah_bprof` dumps at the next bridge call and starts the counts again,
       so an action (typing, a redraw) can be measured on its own: touch, act, touch. (The exit
       dump never came - the translator ends the process without running atexit handlers - and
       the 30 s one needs a run that long.) Checked every 4096 calls. */
    {   static unsigned int k;
        if (!(++k & 4095) && access("/tmp/oah_bprof", 0) == 0) {
            unlink("/tmp/oah_bprof");
            oah_bprof_dump();
            memset(g_bprof_n, 0, sizeof g_bprof_n); memset(g_bprof_t, 0, sizeof g_bprof_t);
            memset(g_gtprof_n, 0, sizeof g_gtprof_n); memset(g_gtprof_t, 0, sizeof g_gtprof_t);
            g_bprof_last = now;
        }
    }
}
static void oah_bridge_call(unsigned int id, unsigned int *R) {
    static int gest_state;            /* 0 pending, 1 running or done (see oah_gestalt_register_missing) */
    static unsigned int gest_tick;
    /* the guest's r1 where it stopped (oah_callppc_n puts back its own value when a guest call returns:
       the return - ret_stub - comes in here with the callback stack's r1) */
    if (pthread_main_np()) g_main_sp = R[1];
    if (id >= GT_ENTRY) {
        if (oah_bprof_on()) {
            unsigned long long t0 = mach_absolute_time();
            oah_gt_call(id, R);
            if ((id & 0x7ff) < GT_MAX) { g_gtprof_n[id & 0x7ff]++; g_gtprof_t[id & 0x7ff] += mach_absolute_time() - t0; }
        } else oah_gt_call(id, R);
        return;
    }
    oah_gt_retry();
    g_last_R = R;
    if (pthread_main_np()) g_main_R = R;
    oah_thr_R_note(R);
    if (id < 4096) g_bcount[id]++;
    oah_hang_note(id);
    {   /* Reserve the guest code chunk early, on the main thread, while no other thread can hold
           the guest-call lock: a first stub needed later on a busy thread got a native page, and
           translate's fatal() ended fontworker (Pages) on the first jump into it.
           OAH_NO_CODEWARM=1 disables. */
        static int warm; static unsigned int wt;
        if (!warm && pthread_main_np() && ++wt == 200) { warm = 1; if (!getenv("OAH_NO_CODEWARM")) oah_guest_code_alloc(64); }
    }
    /* Try at bridge calls 1, 2, 4, 8, ... : a symbol lookup per attempt is expensive, and
       processes without CarbonCore (bridgeloop, drawtest) must not pay for it every call. */
    if (!gest_state) {
        unsigned int t = ++gest_tick;
        if ((t & (t - 1)) == 0) {
            gest_state = 1;
            if (!oah_gestalt_register_missing() && t < (1u << 24)) gest_state = 0;
        }
    }

    /* (An early _CSCheckFix warm-up here deadlocked inside CarbonCore's initializer; the
       fontworker deadlock is fixed in the guest CarbonCore file instead.) */
    if (oah_bprof_on()) {
        unsigned long long t0 = mach_absolute_time();
        if (id < OAH_NBRIDGES) g_bridge_fns[id](R);
        else oah_gen_call(id - OAH_GEN_BASE, R);
        oah_bprof_note(id, mach_absolute_time() - t0);
        return;
    }
    if (id < OAH_NBRIDGES) g_bridge_fns[id](R);
    else oah_gen_call(id - OAH_GEN_BASE, R);
}
static const char *oah_bridge_name(unsigned int id) {
    if (id >= GT_ENTRY) return oah_gt_name(id);
    return id < OAH_NBRIDGES ? g_bridge_names[id] : g_gen[id - OAH_GEN_BASE].name;
}
static void trap_report_handler(int sig, siginfo_t *info, void *uap);
static void bridge_trap_handler(int sig, siginfo_t *info, void *uap) {
    ucontext_t *uc = (ucontext_t *)uap;
    unsigned int fp = uc->uc_mcontext->__ss.__ebp, depth;
    for (depth = 0; depth < 24 && fp > 0x1000; depth++) {
        unsigned int nfp = 0, S = 0, R = 0, pc = 0, pc2 = 0, insn = 0, id;
        if (!probe_read(fp, &nfp)) break;
        if (probe_read(fp + 8, &S) && S > 0x1000 &&
            probe_read(S + 8, &R) && R > 0x1000 &&
            probe_read(S + 0x1c, &pc) && probe_read(S + 0x24, &pc2) && pc == pc2 &&
            probe_read(pc, &insn)) {
            insn = OSSwapBigToHostInt32(insn);
            id = insn & 0xffff;
            if ((insn & 0xffff0000) == 0x0fe00000 && oah_is_bridge(id)) {
                unsigned int *Rp = (unsigned int *)R;
                if (getenv("OAH_VERBOSE"))
                    fprintf(stderr, "[bridge] %s pc=0x%08x R=0x%08x S=0x%08x r3=%08x r4=%08x lr=%08x\n",
                            oah_bridge_name(id), pc, R, S, Rp[3], Rp[4], Rp[0x90 / 4]);
                oah_bridge_call(id, Rp);
                *(unsigned int *)(S + 0x20) = Rp[0x90 / 4];
                return;
            }
        }
        /* Compiled path. Translated code calls Rosetta's tw/twi helper
           (returns at 0xb80f5c50 / 0xb80f5cd2). The helper's frame is the one
           above the signal raiser's frame (fp here), and its saved ebp is the
           translated code's ebp, which is the guest register file R (the same
           R the interpreter uses: rN = R[N], LR = R[0x90/4]). The raiser was
           called as raise(T, sig, x, guest_pc), so guest_pc is at fp+0x14.
           After the handler returns, the helper returns into translated code,
           which runs the next guest instruction: the "blr" that bridgepatch.py
           places after every bridge trap. So only r3 needs setting here. */
        {
            unsigned int ra = 0, hf = 0, RR = 0, gpc = 0, gw = 0, nx = 0, TT = 0;
            /* twi helper: take the trap immediate from R+0xa0 (see oah_fast_raise) */
            if (probe_read(fp + 4, &ra) && ra == 0xb80f5cd2 &&
                probe_read(fp + 8, &TT) && TT > 0x1000 && probe_read(TT, &RR) && RR > 0x1000) {
                unsigned int *Rp = (unsigned int *)RR;
                id = Rp[0xa0 / 4];
                if (Rp[0x98 / 4] == 31 && Rp[0x9c / 4] == 0 && oah_is_bridge(id)) {
                    oah_bridge_call(id, Rp);
                    return;
                }
            }
            if (probe_read(fp + 4, &ra) && ra == 0xb80f5c50 &&
                probe_read(fp, &hf) && probe_read(hf, &RR) && RR > 0x1000 &&
                probe_read(fp + 0x14, &gpc) && probe_read(gpc, &gw) &&
                probe_read(gpc + 4, &nx)) {
                gw = OSSwapBigToHostInt32(gw);
                id = gw & 0xffff;
                if ((gw & 0xffff0000) == 0x0fe00000 && oah_is_bridge(id) &&
                    OSSwapBigToHostInt32(nx) == 0x4e800020) {
                    unsigned int *Rp = (unsigned int *)RR;
                    static int logged;
                    if (getenv("OAH_VERBOSE") && logged < 3) {
                        logged++;
                        fprintf(stderr, "[bridge/jit] %s pc=0x%08x R=0x%08x r3=%08x r4=%08x r5=%08x lr=%08x\n",
                                oah_bridge_name(id), gpc, RR, Rp[3], Rp[4], Rp[5], Rp[0x90 / 4]);
                    }
                    oah_bridge_call(id, Rp);
                    return;
                }
                if (getenv("OAH_VERBOSE"))
                    fprintf(stderr, "[bridge/jit] trap helper but not a bridge: pc=0x%08x insn=%08x next=%08x\n",
                            gpc, gw, OSSwapBigToHostInt32(nx));
            }
        }
        if (nfp <= fp) break;
        fp = nfp;
    }
    /* Interpreter path, second chance: find the raise-signal frame (return address 0xb80bfb30)
       as trap_report_handler does and take the state from its caller's arg0. The check above
       wants the state's two pc copies (S+0x1c, S+0x24) to agree; in AppleWorks' application
       menu (SystemAppMenuHandler -> GetSystemUIMode) they did not, the bridge was reported as
       an unknown trap and the app died. */
    {
        unsigned int f = uc->uc_mcontext->__ss.__ebp, k2;
        for (k2 = 0; k2 < 16 && f > 0x1000 && (f & 3) == 0; k2++) {
            unsigned int ra = 0, nf = 0, st = 0, R = 0, gpc = 0, insn = 0, id;
            if (!probe_read(f + 4, &ra) || !probe_read(f, &nf)) break;
            if ((ra == 0xb80bfb30 || ra == 0xb80bfade) && nf > 0x1000 &&
                probe_read(nf + 8, &st) && st > 0x1000 && probe_read(st + 8, &R) && R > 0x1000 &&
                probe_read(st + 0x1c, &gpc) && probe_read(gpc, &insn)) {
                insn = OSSwapBigToHostInt32(insn);
                if ((insn & 0xffff0000) != 0x0fe00000) {
                    /* the state's pc can be the start of the block (e.g. "li r11,i" in a GL
                       dispatch thunk) while +0x20/+0x24 hold the neighbours: take whichever
                       of them is the bridge trap */
                    unsigned int alt[2], ai, w3;
                    probe_read(st + 0x20, &alt[0]); probe_read(st + 0x24, &alt[1]);
                    for (ai = 0; ai < 2; ai++)
                        if (alt[ai] > 0x1000 && (alt[ai] == gpc + 4 || alt[ai] == gpc - 4) && probe_read(alt[ai], &w3) &&
                            (OSSwapBigToHostInt32(w3) & 0xffff0000) == 0x0fe00000 && oah_is_bridge(OSSwapBigToHostInt32(w3) & 0xffff)) {
                            static int nalt;
                            if (nalt++ < 3) fprintf(stderr, "[bridge] raise frame pc %08x is not the trap; using %08x\n", gpc, alt[ai]);
                            insn = OSSwapBigToHostInt32(w3);
                            break;
                        }
                }
                id = insn & 0xffff;
                if ((insn & 0xffff0000) == 0x0fe00000 && oah_is_bridge(id)) {
                    unsigned int *Rp = (unsigned int *)R;
                    static int logged;
                    if (logged++ < 3) {
                        unsigned int p20 = 0, p24 = 0;
                        probe_read(st + 0x20, &p20); probe_read(st + 0x24, &p24);
                        fprintf(stderr, "[bridge] %s via the raise frame (pc=0x%08x +20=0x%08x +24=0x%08x state=0x%08x)\n",
                                oah_bridge_name(id), gpc, p20, p24, st);
                    }
                    oah_bridge_call(id, Rp);
                    *(unsigned int *)(st + 0x20) = Rp[0x90 / 4];
                    return;
                }
                break;
            }
            if (nf <= f) break;
            f = nf;
        }
    }
    /* not a bridge point: report (a PowerPC "tw"/"twi" assert in guest code ends up
       here; AppleWorks died with a bare "Trace/BPT trap"), then the default action */
    {   /* the translator registered its own SIGTRAP handler for the guest (see my_sigaction):
           a real guest trap goes there, as it would have without us */
        extern struct sigaction g_xl_trap_act; extern volatile int g_xl_trap_set;
        if (g_xl_trap_set && g_xl_trap_act.sa_handler != SIG_DFL && g_xl_trap_act.sa_handler != SIG_IGN) {
            static int n;
            if (n++ < 5) fprintf(stderr, "[sigtrap] not a bridge trap: passing it to the translator's handler\n");
            if (g_xl_trap_act.sa_flags & SA_SIGINFO) g_xl_trap_act.sa_sigaction(sig, info, uap);
            else g_xl_trap_act.sa_handler(sig);
            return;
        }
    }
    if (!getenv("OAH_QUIET_TRAP")) {
        trap_report_handler(sig, info, uap);
        {   /* compiled-code trap: guest pc at fp+0x14 of the twi helper frame, register file via its saved ebp */
            ucontext_t *uc2 = (ucontext_t *)uap;
            unsigned int f2 = uc2->uc_mcontext->__ss.__ebp, ra = 0, hf = 0, RR = 0, gpc = 0, gw = 0, b, d;
            for (d = 0; d < 24 && f2 > 0x1000; d++) {
                unsigned int nf = 0;
                if (probe_read(f2 + 4, &ra) && ra == 0xb80f5c50 && probe_read(f2, &hf) && probe_read(hf, &RR) &&
                    RR > 0x1000 && probe_read(f2 + 0x14, &gpc)) {
                    const char *nm = oah_guest_image_name(gpc, &b), *sl = strrchr(nm, '/');
                    probe_read(gpc, &gw);
                    fprintf(stderr, "[trap] guest trap at %s+0x%x (pc %08x insn %08x)\n", sl ? sl + 1 : nm, gpc - b, gpc, OSSwapBigToHostInt32(gw));
                    oah_crash_guest_frames(RR);
                    break;
                }
                if (!probe_read(f2, &nf) || nf <= f2) break;
                f2 = nf;
            }
        }
        {   /* this thread's guest state: a guest register file is usually in a callee-saved register or on the stack */
            unsigned int cand[64], nc = 0, k2, v, sp = uc->uc_mcontext->__ss.__esp, found = 0;
            cand[nc++] = uc->uc_mcontext->__ss.__esi; cand[nc++] = uc->uc_mcontext->__ss.__edi;
            cand[nc++] = uc->uc_mcontext->__ss.__ebx; cand[nc++] = uc->uc_mcontext->__ss.__ebp;
            for (k2 = 0; k2 < 400 && nc < 64; k2++) if (probe_read(sp + 4 * k2, &v) && v > 0x10000 && !(v & 3)) cand[nc++] = v;
            fprintf(stderr, "[trap] siginfo code=%d pid=%d addr=%p\n", info->si_code, (int)info->si_pid, info->si_addr);
            for (k2 = 0; k2 < nc && !found; k2++) if (oah_hang_isR(cand[k2])) found = cand[k2];
            if (found) {
                unsigned int *Rt = (unsigned int *)found;
                fprintf(stderr, "[trap] this thread's guest state R=%08x r1=%08x r3=%08x lr=%08x ctr=%08x\n", found, Rt[1], Rt[3], Rt[0x90 / 4], Rt[0x94 / 4]);
                if (getenv("OAH_RDUMP")) {   /* the register file past the GPRs, for layout work */
                    unsigned int k3, n3 = (unsigned int)strtoul(getenv("OAH_RDUMP"), 0, 0); if (n3 < 0x80) n3 = 0x400;
                    for (k3 = 0x80; k3 < n3; k3 += 32) fprintf(stderr, "[rdump] +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", k3,
                        Rt[k3 / 4], Rt[k3 / 4 + 1], Rt[k3 / 4 + 2], Rt[k3 / 4 + 3], Rt[k3 / 4 + 4], Rt[k3 / 4 + 5], Rt[k3 / 4 + 6], Rt[k3 / 4 + 7]);
                }
                oah_crash_guest_frames(found);
            }
        }
        if (g_last_R) {
            unsigned int *Rl = (unsigned int *)g_last_R;
            fprintf(stderr, "[trap] last bridge context: r1=%08x r3=%08x lr=%08x\n", Rl[1], Rl[3], Rl[0x90 / 4]);
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ===== Fast bridge path (no signal) =====
   Rosetta's tw/twi helpers in compiled code call raiser(T, sig, pc64) at
   0xb812d33a, which is just pthread_kill(pthread_self(), sig). We replace its
   first 5 bytes with a jmp to oah_fast_raise. For a bridge trap called from
   the twi/tw helper (return address 0xb80f5c50 / 0xb80f5cd2) we run the
   native function directly with R = *(unsigned *)T and return; the helper
   then returns into translated code, which executes the "blr". Anything
   else gets the original behaviour: pthread_kill(pthread_self(), sig). */
#define OAH_RAISER 0xb812d33au
static unsigned long g_fast_calls, g_fast_checked;
__attribute__((noinline, used))
static void oah_fast_raise(unsigned int T, int sig, unsigned int lo, unsigned int gpc) {
    unsigned int ra = (unsigned int)__builtin_return_address(0);
    (void)lo;
    if (sig == SIGTRAP && ra == 0xb80f5cd2u && T > 0x1000) {
        /* twi helper: it reads TO from R+0x98, the RA register index from
           R+0x9c and the immediate from R+0xa0. Use those instead of the
           guest pc argument, which is stale inside nested (callback) runs. */
        unsigned int RR = *(volatile unsigned int *)T;
        if (RR > 0x1000) {
            unsigned int *Rp = (unsigned int *)RR, id = Rp[0xa0 / 4];
            if (Rp[0x98 / 4] == 31 && Rp[0x9c / 4] == 0 && oah_is_bridge(id)) {
                if (g_fast_checked < 3 && getenv("OAH_VERBOSE")) {
                    g_fast_checked++;
                    fprintf(stderr, "[bridge/fast] %s T=0x%08x R=0x%08x r3=%08x r4=%08x r5=%08x lr=%08x\n",
                            oah_bridge_name(id), T, RR, Rp[3], Rp[4], Rp[5], Rp[0x90 / 4]);
                }
                oah_bridge_call(id, Rp);
                g_fast_calls++;
                return;
            }
        }
    }
    if (sig == SIGTRAP && ra == 0xb80f5c50u && T > 0x1000 && gpc > 0x1000) {
        /* Direct reads: gpc is guest code Rosetta just translated, and T is
           Rosetta's own thread context, so both are mapped. (probe_read is a
           Mach call and cost most of the time here.) */
        unsigned int gw, nx, id, RR;
        gw = ((volatile unsigned int *)gpc)[0];
        nx = ((volatile unsigned int *)gpc)[1];
        RR = *(volatile unsigned int *)T;
        if (RR > 0x1000) {
            gw = OSSwapBigToHostInt32(gw);
            id = gw & 0xffff;
            if ((gw & 0xffff0000) == 0x0fe00000 && oah_is_bridge(id) &&
                OSSwapBigToHostInt32(nx) == 0x4e800020) {
                unsigned int *Rp = (unsigned int *)RR;
                {   static int nlog;
                    if (Rp[0x98 / 4] == 31 && Rp[0x9c / 4] == 0 && Rp[0xa0 / 4] != id && nlog < 20) {
                        nlog++;
                        fprintf(stderr, "[bridge/fast] 5c50 path: pc says %s (%u), R fields say %s (%u), pthread %p\n",
                                oah_bridge_name(id), id, oah_is_bridge(Rp[0xa0 / 4]) ? oah_bridge_name(Rp[0xa0 / 4]) : "?",
                                Rp[0xa0 / 4], (void *)pthread_self());
                    }
                }
                if (g_fast_checked < 3 && getenv("OAH_VERBOSE")) {
                    g_fast_checked++;
                    fprintf(stderr, "[bridge/fast] %s pc=0x%08x T=0x%08x R=0x%08x r3=%08x r4=%08x r5=%08x lr=%08x\n",
                            oah_bridge_name(id), gpc, T, RR, Rp[3], Rp[4], Rp[5], Rp[0x90 / 4]);
                }
                oah_bridge_call(id, Rp);
                g_fast_calls++;
                return;
            }
        }
    }
    pthread_kill(pthread_self(), sig);
}
static void oah_fast_report(void) {
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[bridge/fast] %lu fast bridged calls\n", g_fast_calls);
}
static void install_fast_bridge(void) {
    unsigned char *p = (unsigned char *)OAH_RAISER;
    unsigned char expect[] = { 0x55, 0x89, 0xe5, 0x56, 0x83, 0xec, 0x04 };
    unsigned int pg = OAH_RAISER & ~0xfffu, i;
    for (i = 0; i < sizeof expect; i++) {
        unsigned int w = 0;
        if (!probe_read((OAH_RAISER + i) & ~3u, &w)) { fprintf(stderr, "[bridge/fast] raiser not mapped\n"); return; }
        if (p[i] != expect[i]) {
            if (getenv("OAH_VERBOSE")) fprintf(stderr, "[bridge/fast] raiser bytes differ at +%u, not hooking\n", i);
            return;
        }
    }
    if (vm_protect(mach_task_self(), pg, 0x2000, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
        fprintf(stderr, "[bridge/fast] vm_protect rw failed\n");
        return;
    }
    {
        int rel = (int)((unsigned int)oah_fast_raise - (OAH_RAISER + 5));
        p[0] = 0xe9;
        memcpy(p + 1, &rel, 4);
    }
    vm_protect(mach_task_self(), pg, 0x2000, 0, VM_PROT_READ | VM_PROT_EXECUTE);
    atexit(oah_fast_report);
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[bridge/fast] raiser hooked -> %p\n", (void *)oah_fast_raise);
}

/* ===== Translator signal-block nesting, per thread =====
   Rosetta brackets its thread-table lock (0xb812aea2) with block (0xb812d06a) and
   unblock (0xb812d0d4). Both use ONE process-wide nesting counter (0xb821405c): only
   the 0->1 caller blocks and only the 1->0 caller unblocks. With two threads
   interleaving, a thread can be left with every signal blocked for good. A bridge
   trap (twi) on that thread then stays pending instead of running: the guest goes on
   with r3 unchanged (PullEventsFromWindowServer got its connection id back from
   CGEventCreateNextEvent and CFReleased it), and the pending SIGTRAP arrives much
   later with no trap context ("pthread_kill(sig 5) from the translator").
   The pair is also used as a lock/unlock (0xb80a9f72 / 0xb80a9f90, around a per-thread
   field) that can be released by a different thread, so even a per-thread counter
   (OAH_SIGBLKFIX=1) leaves threads blocked: Keynote and fontd then burned CPU in the
   exception thread. Default (OAH_SIGBLKFIX=2): never block. The guarded sections are
   short table lookups; our bridge handlers do not re-enter them.
   OAH_NO_SIGBLKFIX=1 restores Rosetta's behaviour. */
static pthread_key_t g_sigblk_key;
static unsigned long g_sigblk_fix;
static int g_sigblk_mode;   /* 1 = per-thread nesting, 2 = never block */
__attribute__((used)) static void oah_sigblk(void) {
    int n;
    if (g_sigblk_mode == 2) return;
    n = (int)(intptr_t)pthread_getspecific(g_sigblk_key);
    pthread_setspecific(g_sigblk_key, (void *)(intptr_t)(n + 1));
    if (n == 0) pthread_sigmask(SIG_SETMASK, (const sigset_t *)0xb8214054u, NULL);
}
__attribute__((used)) static void oah_sigunblk(void) {
    int n;
    if (g_sigblk_mode == 2) return;
    n = (int)(intptr_t)pthread_getspecific(g_sigblk_key);
    if (n <= 0) {
        if (g_sigblk_fix++ < 10)
            fprintf(stderr, "[sigblk] unbalanced unblock on thread %p (ra %p)\n", (void *)pthread_self(), __builtin_return_address(0));
        n = 1;
    }
    pthread_setspecific(g_sigblk_key, (void *)(intptr_t)(n - 1));
    if (n == 1) pthread_sigmask(SIG_SETMASK, (const sigset_t *)0xb8214058u, NULL);
}
static void oah_hook5(unsigned int at, void *fn, const unsigned char *expect, unsigned int n, const char *what) {
    unsigned char *p = (unsigned char *)at;
    unsigned int i, w;
    if (!probe_read(at & ~3u, &w)) { fprintf(stderr, "[sigblk] %s not mapped\n", what); return; }
    for (i = 0; i < n; i++) if (p[i] != expect[i]) { fprintf(stderr, "[sigblk] %s bytes differ at +%u, not hooking\n", what, i); return; }
    if (vm_protect(mach_task_self(), at & ~0xfffu, 0x2000, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
        fprintf(stderr, "[sigblk] vm_protect rw failed\n"); return;
    }
    { int rel = (int)((unsigned int)fn - (at + 5)); p[0] = 0xe9; memcpy(p + 1, &rel, 4); }
    vm_protect(mach_task_self(), at & ~0xfffu, 0x2000, 0, VM_PROT_READ | VM_PROT_EXECUTE);
}
static void install_sigblk_fix(void) {
    static const unsigned char e[] = { 0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x14, 0xe8 };
    pthread_key_create(&g_sigblk_key, NULL);
    g_sigblk_mode = getenv("OAH_SIGBLKFIX") ? atoi(getenv("OAH_SIGBLKFIX")) : 2;
    if (g_sigblk_mode < 1) g_sigblk_mode = 1;
    /* threads already inside a block section keep the old counter's view; count it for this one */
    if (*(volatile int *)0xb821405cu > 0) pthread_setspecific(g_sigblk_key, (void *)(intptr_t)*(volatile int *)0xb821405cu);
    oah_hook5(0xb812d06au, (void *)oah_sigblk, e, sizeof e, "block");
    oah_hook5(0xb812d0d4u, (void *)oah_sigunblk, e, sizeof e, "unblock");
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[sigblk] per-thread signal-block nesting installed\n");
}

static unsigned int g_cp_patched;
static void segv_handler(int sig, siginfo_t *info, void *uap) {
    ucontext_t *uc = (ucontext_t *)uap;
    unsigned int fault_addr = (unsigned int)(uintptr_t)info->si_addr;
    unsigned int eip = uc->uc_mcontext->__ss.__eip;
    unsigned int eax = uc->uc_mcontext->__ss.__eax;
    if (g_rep_crash && pthread_equal(pthread_self(), g_crash_thread)) {
        /* a fault inside our own crash handling: never back into it (or to the guest's handler) */
        static const char m[] = "[crash] fault inside the crash handler: exiting\n";
        write(2, m, sizeof m - 1);
        oah_crash_alarm(0);   /* writes what there is and exits */
    }
    if (getenv("OAH_VERBOSE")) {
        fprintf(stderr, "[segv] sig=%d addr=0x%08x eip=0x%08x eax=0x%08x edx=0x%08x esi=0x%08x\n",
                sig, fault_addr, eip, eax, uc->uc_mcontext->__ss.__edx, uc->uc_mcontext->__ss.__esi);
        if (g_last_R && !is_ppc_commpage(fault_addr)) {
            unsigned int *R = g_last_R, k, sp, w[3];
            vm_size_t got;
            fprintf(stderr, "[segv] guest regs (as of last flush): lr=%08x ctr=?", R[0x90 / 4]);
            for (k = 0; k < 32; k++) fprintf(stderr, "%s r%u=%08x", (k % 8) ? "" : "\n[segv]  ", k, R[k]);
            fprintf(stderr, "\n[segv] guest bt:");
            for (sp = R[1], k = 0; k < 12 && sp > 0x1000 && !(sp & 3); k++) {
                if (vm_read_overwrite(mach_task_self(), sp, 12, (vm_address_t)w, &got)) break;
                if (k) {
                    unsigned int ra = OSSwapBigToHostInt32(w[2]), base = 0;
                    const char *nm = oah_guest_image_name(ra, &base);
                    const char *sl = strrchr(nm, '/');
                    fprintf(stderr, "\n[segv]   %08x %s+0x%x", ra, sl ? sl + 1 : nm, ra - base);
                }
                if (OSSwapBigToHostInt32(w[0]) <= sp) break;
                sp = OSSwapBigToHostInt32(w[0]);
            }
            fprintf(stderr, "\n");
        }
    }
    if (is_ppc_commpage(fault_addr)) {
        int fixed = 0;
        unsigned int *regs[6] = {
            &uc->uc_mcontext->__ss.__eax,
            &uc->uc_mcontext->__ss.__ebx,
            &uc->uc_mcontext->__ss.__ecx,
            &uc->uc_mcontext->__ss.__edx,
            &uc->uc_mcontext->__ss.__esi,
            &uc->uc_mcontext->__ss.__edi
        };
        for (int i = 0; i < 6; i++) {
            if (is_ppc_commpage(*regs[i])) {
                *regs[i] -= 0x00200000;
                fixed = 1;
            }
        }
        unsigned int esi = uc->uc_mcontext->__ss.__esi;
        if (esi > 0x1000 && esi < 0xf0000000) {
            /* esi is only sometimes Rosetta's state block; it can be any value, so probe first
               (an unchecked read here faulted inside this handler during a city load). */
            unsigned int v1 = 0, v2 = 0;
            unsigned int *pc1 = (unsigned int *)(esi + 0x1c);
            unsigned int *pc2 = (unsigned int *)(esi + 0x20);
            vm_address_t wa = esi + 0x1c; vm_size_t ws = 0; vm_region_basic_info_data_t wi;
            mach_msg_type_number_t wc = VM_REGION_BASIC_INFO_COUNT; mach_port_t wo;
            /* and writable: esi pointed into a guest code page once (Numbers), and the fix-up write
               itself faulted inside this handler */
            if (vm_region(mach_task_self(), &wa, &ws, VM_REGION_BASIC_INFO, (vm_region_info_t)&wi, &wc, &wo) == KERN_SUCCESS &&
                wa <= esi + 0x1c && esi + 0x24 <= wa + ws && (wi.protection & VM_PROT_WRITE) &&
                probe_read(esi + 0x1c, &v1) && probe_read(esi + 0x20, &v2)) {
                if (is_ppc_commpage(v1)) { *pc1 -= 0x00200000; fixed = 1; }
                if (is_ppc_commpage(v2)) { *pc2 -= 0x00200000; fixed = 1; }
            }
        }
        if (fixed) {
            if (getenv("OAH_VERBOSE")) {
                fprintf(stderr, "[segv_fixed] resumed eip=0x%08x edx=0x%08x\n",
                        uc->uc_mcontext->__ss.__eip, uc->uc_mcontext->__ss.__edx);
            }
            return;
        }
    }
    if (is_ppc_commpage(fault_addr)) {
        /* JIT-compiled guest loads read the old commpage with absolute/based x86 loads.
           Emulate: mov / movzx(b,w) r32, [ea]  where ea == fault_addr. */
        unsigned int *gpr[8] = {
            &uc->uc_mcontext->__ss.__eax, &uc->uc_mcontext->__ss.__ecx,
            &uc->uc_mcontext->__ss.__edx, &uc->uc_mcontext->__ss.__ebx,
            &uc->uc_mcontext->__ss.__esp, &uc->uc_mcontext->__ss.__ebp,
            &uc->uc_mcontext->__ss.__esi, &uc->uc_mcontext->__ss.__edi
        };
        unsigned char *p = (unsigned char *)(uintptr_t)eip;
        int width = 4, len = 0, dst = -1;
        unsigned int ea = 0;
        unsigned char *m = 0;
        if (p[0] == 0xa1) { dst = 0; ea = *(unsigned int *)(p + 1); len = 5; }
        else if (p[0] == 0x8b) { m = p + 1; len = 1; }
        else if (p[0] == 0x0f && (p[1] == 0xb6 || p[1] == 0xb7)) { width = (p[1] == 0xb6) ? 1 : 2; m = p + 2; len = 2; }
        if (m) {
            unsigned char modrm = m[0];
            int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
            dst = reg; len += 1;
            if (mod == 3) dst = -1;
            else if (rm == 4) {
                unsigned char sib = m[1];
                int sc = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
                len += 1;
                ea = (idx == 4) ? 0 : (*gpr[idx] << sc);
                if (base == 5 && mod == 0) { ea += *(unsigned int *)(m + 2); len += 4; }
                else ea += *gpr[base];
                if (mod == 1) { ea += (int)(signed char)m[2]; len += 1; }
                else if (mod == 2) { ea += *(unsigned int *)(m + 2); len += 4; }
            } else if (mod == 0 && rm == 5) { ea = *(unsigned int *)(m + 1); len += 4; }
            else {
                ea = *gpr[rm];
                if (mod == 1) { ea += (int)(signed char)m[1]; len += 1; }
                else if (mod == 2) { ea += *(unsigned int *)(m + 1); len += 4; }
            }
        }
        if (dst >= 0 && ea == fault_addr) {
            unsigned int src = fault_addr - 0x00200000, val;
            if (width == 4) val = *(unsigned int *)(uintptr_t)src;
            else if (width == 2) val = *(unsigned short *)(uintptr_t)src;
            else val = *(unsigned char *)(uintptr_t)src;
            *gpr[dst] = val;
            uc->uc_mcontext->__ss.__eip = eip + len;
            /* The CPU-capability word never changes, and libm (sqrt, etc.) reads it on every
               call. Each fault costs ~450 us (Mach exception + signal), which made city
               loading look hung. Rewrite an absolute 4-byte load into "mov reg, imm32" so the
               translated block never faults again. */
            /* also the CPU count byte at 0xffff8024 (movzbl abs32, 7 bytes): libdispatch-era code
               reads it per call too, and on a secondary thread each fault waits for the
               translator's exception thread (seen stalling Keynote for seconds per fault) */
            if (fault_addr >= 0xffff8020 && fault_addr < 0xffff8028 && !getenv("OAH_NO_CPPATCH") &&
                ((p[0] == 0xa1 && len == 5) || (p[0] == 0x8b && len == 6 && (p[1] & 0xc7) == 0x05) ||
                 (p[0] == 0x0f && (p[1] == 0xb6 || p[1] == 0xb7) && len == 7 && (p[2] & 0xc7) == 0x05))) {
                unsigned char nb[8];
                uintptr_t a8 = (uintptr_t)p & ~(uintptr_t)7;
                int off = (int)((uintptr_t)p - a8);
                vm_address_t pg = (vm_address_t)((uintptr_t)p & ~(uintptr_t)0xfff);
                vm_size_t pgsz = (((uintptr_t)p + len - 1) & ~(uintptr_t)0xfff) == pg ? 0x1000 : 0x2000;
                if (vm_protect(mach_task_self(), pg, pgsz, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE) == KERN_SUCCESS) {
                    nb[0] = (unsigned char)(0xb8 + dst);
                    memcpy(nb + 1, &val, 4);
                    nb[5] = nb[6] = nb[7] = 0x90;
                    if (off + len <= 8) {   /* single atomic 8-byte store */
                        unsigned long long oldv = *(volatile unsigned long long *)a8, newv = oldv;
                        memcpy((unsigned char *)&newv + off, nb, len);
                        __sync_bool_compare_and_swap((volatile unsigned long long *)a8, oldv, newv);
                    } else {
                        memcpy(p + 1, nb + 1, len - 1);   /* operand first, opcode last */
                        __sync_synchronize();
                        p[0] = nb[0];
                    }
                    g_cp_patched++;
                    if (getenv("OAH_VERBOSE"))
                        fprintf(stderr, "[segv_cppatch] eip=0x%08x patched to mov r%d,0x%08x (%u sites)\n", eip, dst, val, g_cp_patched);
                }
            }
            if (getenv("OAH_VERBOSE"))
                fprintf(stderr, "[segv_jitload] eip=0x%08x ea=0x%08x w=%d -> r%d=0x%08x len=%d\n",
                        eip, ea, width, dst, val, len);
            return;
        }
        if (p[0] >= 0xb8 && p[0] <= 0xbf && fault_addr >= 0xffff8020 && fault_addr < 0xffff8028 &&
            (*(unsigned int *)(p + 1) == *(unsigned int *)(uintptr_t)(0xffff8020 - 0x00200000) ||
             (p[5] == 0x90 && *(unsigned int *)(p + 1) == (fault_addr == 0xffff8024 || fault_addr == 0xffff8025 ?
                  (unsigned int)*(unsigned char *)(uintptr_t)(fault_addr - 0x00200000) : *(unsigned int *)(p + 1))))) {
            /* (iWeb crashed here: two threads faulted on the AltiVec byte at 0xffff8024; the loser saw
               the winner's "mov eax,0; nop; nop" and was not recognised as the race) */
            /* another thread faulted on this load first and already rewrote it to
               "mov reg, imm32" (see above): just run the patched instruction */
            if (getenv("OAH_VERBOSE")) fprintf(stderr, "[segv_cppatch] eip=0x%08x already patched (race), retrying\n", eip);
            return;
        }
        {   /* Generic: any instruction (SSE included) whose memory operand is an absolute disp32 into the
               commpage (e.g. "movsd xmm0,[0xfffef000]" from objc_msgSend's nil path after the JIT folded the
               constant base): rewrite the displacement in the translated code to the relocated copy and retry. */
            unsigned char *q = p; int k = 0, mr = -1;
            while (k < 4 && (*q == 0x66 || *q == 0xf2 || *q == 0xf3 || *q == 0x2e || *q == 0x3e || *q == 0x26 ||
                             *q == 0x36 || *q == 0x64 || *q == 0x65 || *q == 0xf0)) { q++; k++; }
            if (q[0] == 0x0f) { if (q[1] == 0x38 || q[1] == 0x3a) mr = 3; else mr = 2; }
            else mr = 1;
            {
                unsigned char modrm = q[mr], mod = modrm >> 6, rm = modrm & 7;
                unsigned char *dp = 0;
                if (mod == 0 && rm == 5) dp = q + mr + 1;
                else if (mod == 0 && rm == 4 && (q[mr + 1] & 7) == 5 && ((q[mr + 1] >> 3) & 7) == 4) dp = q + mr + 2;
                if (dp) {
                    unsigned int d = *(unsigned int *)dp;
                    if (fault_addr - d < 16 && d >= 0xfffec000u && d < 0xfffff000u) {
                        vm_address_t pg = (vm_address_t)((uintptr_t)dp & ~(uintptr_t)0xfff);
                        if (vm_protect(mach_task_self(), pg, 0x2000, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE) == KERN_SUCCESS) {
                            __sync_bool_compare_and_swap((volatile unsigned int *)dp, d, d - 0x00200000u);
                            if (getenv("OAH_VERBOSE"))
                                fprintf(stderr, "[segv_disp32] eip=0x%08x disp 0x%08x -> 0x%08x\n", eip, d, d - 0x00200000u);
                            return;
                        }
                    } else if (d == fault_addr - 0x00200000u || (d < 0xfffec000u && d >= 0xffdec000u)) {
                        return;   /* another thread already rewrote it */
                    }
                }
            }
        }
        {   /* A patch that crosses an 8-byte boundary is written operand first, opcode last (see
               above), so a second thread that faulted on the old load can read the old opcode with
               the new operand bytes, decode garbage and land here (Fractal Explorer: "unhandled
               commpage access" on "mov eax,imm32; nop"). If the bytes change or already are the
               patched form shortly after, just run the instruction again. */
            unsigned char snap[8]; int t2;
            memcpy(snap, p, 8);
            for (t2 = 0; t2 < 20; t2++) {
                if (p[0] >= 0xb8 && p[0] <= 0xbf) return;   /* "mov reg,imm32" cannot fault: it was patched */
                if (memcmp(snap, p, 8)) return;
                usleep(500);
                __sync_synchronize();
            }
        }
        oah_tel("segv", "commpage");
        fprintf(stderr, "[segv] unhandled commpage access, bytes at eip: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    }
    {   /* the translator's own handler (installed for a guest handler): real guest faults go there */
        extern struct sigaction g_xl_segv_act[2]; extern volatile int g_xl_segv_set[2];
        int k = sig == SIGBUS;
        /* ...but not a fault in native code (liboah, the system libraries: a bridged call given a bad
           guest pointer). The guest's handler cannot fix that, it returned, and the same native
           instruction faulted again forever: AppleWorks, after its RTF translator overran the stack,
           spun at full CPU in memmove for 35 minutes, still registered as running. Such a fault, and
           any fault that keeps coming back at one eip, is a crash. */
        char rf[1024]; int native = 0;
        static unsigned int last_eip, same;
        if (proc_regionfilename(getpid(), eip, rf, sizeof rf) > 0 && !strstr(rf, "/translate")) native = 1;
        if (eip == last_eip) same++; else { last_eip = eip; same = 0; }
        if ((sig == SIGSEGV || sig == SIGBUS) && g_xl_segv_set[k] && (native || same > 100))
            fprintf(stderr, "[sig] %s at %p in %s (eip %08x%s): not for the guest's handler\n", k ? "SIGBUS" : "SIGSEGV",
                    info->si_addr, native ? strrchr(rf, '/') + 1 : "translated code", eip, same > 100 ? ", 100 times" : "");
        else if ((sig == SIGSEGV || sig == SIGBUS) && g_xl_segv_set[k]) {
            static int n;
            if (n++ < 5) fprintf(stderr, "[sig] %s at %p (eip %08x) passed to the translator's handler\n", k ? "SIGBUS" : "SIGSEGV", info->si_addr, eip);
            if (g_xl_segv_act[k].sa_flags & SA_SIGINFO) g_xl_segv_act[k].sa_sigaction(sig, info, uap);
            else g_xl_segv_act[k].sa_handler(sig);
            return;
        }
    }
    oah_report_mark("crash");
    oah_crash_watchdog();
    fprintf(stderr, "CRASH pid %d sig=%d addr=%p eip=0x%08x eax=0x%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x\n", getpid(),
            sig, info->si_addr, eip, eax, uc->uc_mcontext->__ss.__ebx, uc->uc_mcontext->__ss.__ecx,
            uc->uc_mcontext->__ss.__edx, uc->uc_mcontext->__ss.__esi, uc->uc_mcontext->__ss.__edi, uc->uc_mcontext->__ss.__ebp);
    {
        unsigned int w0 = 0, w1 = 0, w2 = 0;
        probe_read(eip, &w0); probe_read(eip + 4, &w1); probe_read(eip + 8, &w2);
        fprintf(stderr, "[crash] bytes@eip: %08x %08x %08x (little-endian words) esp=%08x\n", w0, w1, w2, uc->uc_mcontext->__ss.__esp);
        if (getenv("OAH_EIPDUMP")) {   /* OAH_EIPDUMP=before,after: native code bytes around eip (for disassembly) */
            int before = atoi(getenv("OAH_EIPDUMP")), after = 256, k; const char *c = strchr(getenv("OAH_EIPDUMP"), ',');
            unsigned int w;
            if (c) after = atoi(c + 1);
            fprintf(stderr, "[eipdump] %08x %d:", eip - before, before);
            for (k = -before; k < after; k += 4) { if (!probe_read(eip + k, &w)) break; fprintf(stderr, " %08x", w); }
            fprintf(stderr, "\n");
            {   /* block prologue "mov $desc,%eax; addl $-1,(%eax)": the block's descriptor */
                unsigned int a, d = 0, w2, j;
                for (a = eip; a > eip - 4096; a--) {
                    unsigned char b[8]; int ok = 1;
                    for (j = 0; j < 8; j++) { unsigned int ww; if (!probe_read((a + j) & ~3u, &ww)) { ok = 0; break; } b[j] = (unsigned char)(ww >> (8 * ((a + j) & 3))); }
                    if (ok && b[0] == 0xb8 && b[5] == 0x83 && b[6] == 0x00 && b[7] == 0xff) { d = b[1] | b[2] << 8 | b[3] << 16 | (unsigned)b[4] << 24; break; }
                }
                if (d) {
                    fprintf(stderr, "[eipdump] block at %08x (eip-%u) desc %08x:", a, eip - a, d);
                    for (j = 0; j < 24; j++) { if (!probe_read(d - 16 + 4 * j, &w2)) break; fprintf(stderr, " %08x", w2); }
                    fprintf(stderr, "\n");
                }
            }
            {   unsigned int ws[64]; fprintf(stderr, "[espdump]"); for (k = 0; k < 64; k++) { if (!probe_read(uc->uc_mcontext->__ss.__esp + 4 * k, &ws[k])) break; fprintf(stderr, " %08x", ws[k]); } fprintf(stderr, "\n"); }
        }
        if (getenv("OAH_GDUMP")) {   /* OAH_GDUMP=addr:nwords[,addr:nwords] -> big-endian guest words */
            const char *s = getenv("OAH_GDUMP");
            while (s && *s) {
                unsigned int a = (unsigned int)strtoul(s, (char **)&s, 16), n = 32, k, w;
                if (*s == ':') n = (unsigned int)strtoul(s + 1, (char **)&s, 10);
                fprintf(stderr, "[gdump] %08x:", a);
                for (k = 0; k < n; k++) { if (!probe_read(a + 4 * k, &w)) break; fprintf(stderr, " %08x", OSSwapBigToHostInt32(w)); }
                fprintf(stderr, "\n");
                if (*s == ',') s++; else break;
            }
        }
        {   /* where is the fault address relative to mapped regions? */
            vm_address_t ra = (vm_address_t)info->si_addr & ~0xfffu, pa = 0; vm_size_t rs = 0, ps = 0;
            vm_region_basic_info_data_t bi; mach_msg_type_number_t bc = VM_REGION_BASIC_INFO_COUNT; mach_port_t on;
            vm_address_t q = 0x1000; vm_size_t qs;
            while (1) {   /* last region below the fault address */
                vm_address_t qa = q; bc = VM_REGION_BASIC_INFO_COUNT;
                if (vm_region(mach_task_self(), &qa, &qs, VM_REGION_BASIC_INFO, (vm_region_info_t)&bi, &bc, &on) != KERN_SUCCESS) break;
                if (qa > ra) { ra = qa; rs = qs; break; }
                pa = qa; ps = qs; q = qa + qs;
            }
            fprintf(stderr, "[crash] fault %p: region below %08x-%08x, next region %08x-%08x\n", info->si_addr,
                    (unsigned)pa, (unsigned)(pa + ps), (unsigned)ra, (unsigned)(ra + rs));
        }
        { extern void oah_dring_dump(void); oah_dring_dump(); }
    }
    {   /* guest context: translated code keeps the PPC register file in ebp */
        char gb[1024];
        unsigned int X = uc->uc_mcontext->__ss.__ebp;
        unsigned int RR = 0;
        unsigned int t0w;
        if (oah_hang_isR(X)) { oah_hang_dump_R(X, gb, sizeof gb); fputs(gb, stderr); RR = X; }
        else if (X >= 0x80000000u && X < 0x90000000u && oah_hang_rd(X + 4, &t0w)) {
            fprintf(stderr, "[crash] ebp looks like a guest register file (back chain broken):\n");
            oah_hang_dump_R(X, gb, sizeof gb); fputs(gb, stderr); RR = X;
        }
        else if (oah_hang_rd(X + 8, &t0w) && oah_hang_rd(t0w + 8, &RR) && oah_hang_isR(RR)) {
            /* interpreter helper (one PPC instruction emulated in C): arg0 is the CPU
               context, ctx+8 the register file, ctx+0x28 the instruction word */
            unsigned int *cx = (unsigned int *)t0w, k;
            fprintf(stderr, "[crash] in the interpreter, ctx=%#x insn=%08x ctx words:", t0w, cx[10]);
            for (k = 0; k < 16; k++) fprintf(stderr, " %08x", cx[k]);
            fprintf(stderr, "\n");
            oah_hang_dump_R(RR, gb, sizeof gb); fputs(gb, stderr);
        }
        else if ((RR = 0), ({ unsigned int cands[8 + 1024], nc2 = 0, k3, sp = uc->uc_mcontext->__ss.__esp, w;
                  cands[nc2++] = eax; cands[nc2++] = uc->uc_mcontext->__ss.__ebx; cands[nc2++] = uc->uc_mcontext->__ss.__ecx;
                  cands[nc2++] = uc->uc_mcontext->__ss.__edx; cands[nc2++] = uc->uc_mcontext->__ss.__esi;
                  cands[nc2++] = uc->uc_mcontext->__ss.__edi;
                  for (k3 = 0; k3 < 1024 && probe_read(sp + 4 * k3, &w); k3++) cands[nc2++] = w;
                  for (k3 = 0; k3 < nc2 && !RR; k3++) {
                      unsigned int c = cands[k3], c8;
                      if (c < 0x1000) continue;
                      if (oah_hang_isR(c)) RR = c;
                      else if (oah_hang_rd(c + 8, &c8) && c8 > 0x1000 && oah_hang_isR(c8)) RR = c8;
                  }
                  RR != 0; })) {
            fprintf(stderr, "[crash] guest register file found by scanning regs/stack:\n");
            oah_hang_dump_R(RR, gb, sizeof gb); fputs(gb, stderr);
        }
        else if ((RR = 0), oah_hang_rd(X + 8, &t0w) && t0w > 0x1000 && oah_hang_rd(t0w + 0x28, &RR) && oah_hang_rd(t0w + 8, &RR) && RR > 0x1000) {
            /* an interpreter helper whose register file failed the usual check: print it raw */
            unsigned int k, w;
            fprintf(stderr, "[crash] interpreter context (unchecked) ctx=%#x words:", t0w);
            for (k = 0; k < 16 && oah_hang_rd(t0w + 4 * k, &w); k++) fprintf(stderr, " %08x", w);
            fprintf(stderr, "\n[crash] its register file %#x:", RR);
            for (k = 0; k < 48 && oah_hang_rd(RR + 4 * k, &w); k++) fprintf(stderr, "%s%08x", (k & 7) ? " " : "\n   ", w);
            fprintf(stderr, "\n");
            RR = 0;
        }
        else if ((RR = 0), g_last_R && oah_hang_isR((unsigned int)g_last_R)) {
            fprintf(stderr, "[crash] ebp is not a guest register file; last bridge context:\n");
            oah_hang_dump_R((unsigned int)g_last_R, gb, sizeof gb); fputs(gb, stderr);
            RR = (unsigned int)g_last_R;
        }
        if (RR) {   /* guest code words: callee of bl at lr-4, for offline disassembly */
            unsigned int lr = ((unsigned int *)RR)[0x90 / 4], ins, tgt = 0, k, w;
            fprintf(stderr, "[gcode] gprs:");
            for (k = 0; k < 32; k++) fprintf(stderr, " r%u=%08x", k, ((unsigned int *)RR)[k]);
            fprintf(stderr, "\n");
            {   unsigned int regs3[3] = { ((unsigned int *)RR)[3], ((unsigned int *)RR)[30], ((unsigned int *)RR)[31] }, j;
                for (j = 0; j < 3; j++) {
                    fprintf(stderr, "[gcode] mem %08x:", regs3[j]);
                    for (k = 0; k < 24; k++) if (oah_hang_rd(regs3[j] + 4 * k, &w)) fprintf(stderr, " %08x", OSSwapBigToHostInt32(w));
                    fprintf(stderr, "\n");
                }
            }
            if (getenv("OAH_GDUMPTOC")) {   /* IE ISL: shared-library table at r2-0x4bac, 0x30-byte entries */
                unsigned int t = ((unsigned int *)RR)[2] - (unsigned int)strtoul(getenv("OAH_GDUMPTOC"), 0, 16), e, nm;
                for (e = 0; e < 24; e++) {
                    unsigned int *E = (unsigned int *)(t + 0x30 * e), q;
                    if (!probe_read((unsigned int)E, &q)) break;
                    fprintf(stderr, "[gtoc] %2u:", e);
                    for (k = 0; k < 12; k++) fprintf(stderr, " %08x", OSSwapBigToHostInt32(E[k]));
                    nm = OSSwapBigToHostInt32(E[8]);
                    if (nm > 0x1000 && probe_read(nm, &q)) {   /* Pascal or C string */
                        unsigned char *s = (unsigned char *)nm; char b[64]; int L = s[0] < 63 && s[0] > 0 ? s[0] : 0, j;
                        for (j = 0; j < 63 && (L ? j < L : s[j]); j++) b[j] = (char)(L ? s[j + 1] : s[j]);
                        b[j] = 0; fprintf(stderr, " \"%s\"", b);
                    }
                    fprintf(stderr, "\n");
                }
            }
            if (oah_hang_rd(lr - 4, &ins)) {
                ins = OSSwapBigToHostInt32(ins);
                if ((ins >> 26) == 18 && (ins & 1) && !(ins & 2)) {
                    int off = ins & 0x03fffffc; if (off & 0x02000000) off |= 0xfc000000;
                    tgt = lr - 4 + off;
                }
                fprintf(stderr, "[gcode] lr-4=%08x insn=%08x target=%08x\n[gcode] caller %08x:", lr - 4, ins, tgt, lr - 0x40);
                for (k = 0; k < 20; k++) if (oah_hang_rd(lr - 0x40 + 4 * k, &w)) fprintf(stderr, " %08x", OSSwapBigToHostInt32(w));
                fprintf(stderr, "\n");
                if (tgt) {
                    fprintf(stderr, "[gcode] callee %08x:", tgt);
                    for (k = 0; k < 160; k++) if (oah_hang_rd(tgt + 4 * k, &w)) fprintf(stderr, " %08x", OSSwapBigToHostInt32(w));
                    fprintf(stderr, "\n");
                }
            }
        }
        if (getenv("OAH_CRASHALL")) oah_hang_dump_all("crash");
        if (RR) oah_crash_guest_frames(RR);
    }
    {   /* native backtrace with symbol names (diagnostic) */
        unsigned int pc = eip, fpp = uc->uc_mcontext->__ss.__ebp, i;
        for (i = 0; i < 24 && pc; i++) {
            Dl_info di;
            if (dladdr((void *)pc, &di) && di.dli_sname)
                fprintf(stderr, "  #%u 0x%08x %s + %u (%s)\n", i, pc, di.dli_sname,
                        pc - (unsigned int)di.dli_saddr, di.dli_fname ? strrchr(di.dli_fname, '/') + 1 : "?");
            else
                fprintf(stderr, "  #%u 0x%08x ?\n", i, pc);
            if (fpp < 0x1000 || !probe_read(fpp + 4, &pc) || !probe_read(fpp, &fpp)) break;
        }
    }
    oah_report_flush();
    _exit(1);
}

/* CFM (Metrowerks/MrC) code carries a traceback table after each function:
   0x00000000, version/lang word, flags word, function length, then a 16-bit name
   length and the name.  Scan forward from pc for it (diagnostics only). */
static const char *oah_tb_name(unsigned int pc, char *out, int cap, unsigned int *fstart) {
    unsigned int a = pc & ~3u, w, k;
    out[0] = 0; *fstart = 0;
    for (k = 0; k < 16384; k++, a += 4) {
        unsigned int v1, v2, fl, nl;
        if (!probe_read(a, &w)) return out;
        if (w) continue;
        if (!probe_read(a + 4, &v1) || !probe_read(a + 8, &v2)) return out;
        v1 = OSSwapBigToHostInt32(v1); v2 = OSSwapBigToHostInt32(v2);
        {   /* word1: version lang flags1 flags2 (has_tboff 0x2000, name_present 0x40, has_ctl 0x800, int_hndl 0x80) */
            unsigned int o = 12;
            if ((v1 >> 24) != 0 || !(v1 & 0x2000) || !(v1 & 0x40) || (v1 & 0x880)) continue;
            if (v2 & 0xfffe) o += 4;                          /* fixedparms/floatparms -> parminfo word */
            if (!probe_read(a + o, &fl)) return out;
            fl = OSSwapBigToHostInt32(fl);
            if (!probe_read(a + o + 4, &nl)) return out;
            nl = OSSwapBigToHostInt32(nl) >> 16;
            if (nl == 0 || nl >= (unsigned)cap || nl > 200) continue;
            memcpy(out, (char *)(a + o + 6), nl); out[nl] = 0;
        }
        if (fl && fl < 0x100000) *fstart = a - fl;
        return out;
    }
    return out;
}
/* the file a guest address is mapped from (CFM fragments have no Mach-O header to name them) */
static const char *oah_regfile(unsigned int a, char *buf, int n) {
    char *s;
    buf[0] = 0;
    if (proc_regionfilename(getpid(), a, buf, n) <= 0) { buf[0] = 0; return buf; }
    s = strrchr(buf, '/');
    return s ? s + 1 : buf;
}
static void oah_crash_guest_frames(unsigned int RR) {   /* guest frames with image names */
    unsigned int *Rp = (unsigned int *)RR, sp = Rp[1], b, n, back, ra, fs;
    const char *nm, *sl;
    char tb[208];
    nm = oah_guest_image_name(Rp[0x90 / 4], &b); sl = strrchr(nm, '/');
    oah_tb_name(Rp[0x90 / 4], tb, sizeof tb, &fs);
    {   char rf[1024]; const char *f = oah_regfile(Rp[0x90 / 4], rf, sizeof rf);
        fprintf(stderr, "[crash]   lr %s+0x%x %s+0x%x%s%s%s\n", sl ? sl + 1 : nm, Rp[0x90 / 4] - b, tb, fs ? Rp[0x90 / 4] - fs : 0, *f ? " [" : "", f, *f ? "]" : ""); }
    for (n = 0; n < 24 && sp > 0x1000 && !(sp & 3); n++) {
        if (!probe_read(sp, &back)) break;
        back = OSSwapBigToHostInt32(back);
        if (!back || back <= sp || !probe_read(back + 8, &ra)) break;
        ra = OSSwapBigToHostInt32(ra);
        nm = oah_guest_image_name(ra, &b); sl = strrchr(nm, '/');
        oah_tb_name(ra, tb, sizeof tb, &fs);
        {   char rf[1024]; const char *f = oah_regfile(ra, rf, sizeof rf);
            fprintf(stderr, "[crash]     %08x %s+0x%x %s+0x%x%s%s%s\n", ra, sl ? sl + 1 : nm, ra - b, tb, fs ? ra - fs : 0, *f ? " [" : "", f, *f ? "]" : ""); }
        sp = back;
    }
}

#include <execinfo.h>
/* SIGABRT: say who aborted (native frames + last bridged guest context). */
static void abrt_handler(int sig, siginfo_t *info, void *uap) {
    ucontext_t *uc = (ucontext_t *)uap;
    unsigned int eip = uc->uc_mcontext->__ss.__eip, esp = uc->uc_mcontext->__ss.__esp, k, shown = 0;
    Dl_info di;
    fprintf(stderr, "ABORT sig=%d thread=%p main=%d eip=%08x esp=%08x sender_pid=%d\n", sig, (void *)pthread_self(),
            pthread_main_np(), eip, esp, info ? info->si_pid : -1);
    if (dladdr((void *)eip, &di) && di.dli_sname) fprintf(stderr, "  at %s (%s)\n", di.dli_sname, di.dli_fname ? strrchr(di.dli_fname, '/') + 1 : "?");
    for (k = 0; k < 1024 && shown < 30; k++) {     /* scan 4 KB of stack for native return addresses */
        unsigned int v;
        if (!probe_read(esp + 4 * k, &v)) break;
        if (v > 0x1000 && dladdr((void *)v, &di) && di.dli_sname && di.dli_fname &&
            (unsigned int)di.dli_saddr < v && v - (unsigned int)di.dli_saddr < 0x4000) {
            fprintf(stderr, "  [esp+%03x] %08x %s + %u (%s)\n", 4 * k, v, di.dli_sname, v - (unsigned int)di.dli_saddr,
                    strrchr(di.dli_fname, '/') + 1);
            shown++;
        }
    }
    if (g_last_R) {
        unsigned int *R = g_last_R, sp = R[1], k;
        fprintf(stderr, "  last guest bridge: r1=%08x r3=%08x lr=%08x bt:", R[1], R[3], R[0x90 / 4]);
        for (k = 0; k < 14 && sp > 0x1000 && !(sp & 3); k++) {
            unsigned int back, lr;
            if (!probe_read(sp, &back)) break;
            back = OSSwapBigToHostInt32(back);
            if (!back || back <= sp || !probe_read(back + 8, &lr)) break;
            fprintf(stderr, " %08x", OSSwapBigToHostInt32(lr));
            sp = back;
        }
        fprintf(stderr, "\n");
    }
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}

/* translate's fatal(code, 0, fmt, arg) at b800c540 (exit(1) when code != 0): log why. Callers include its
   internal allocator (b81507a2: code 2, arg = the size it could not allocate). */
static void __attribute__((noinline)) oah_tr_fatal(int code, int z, const char *fmt, unsigned int arg) {
    unsigned int *fp = (unsigned int *)__builtin_frame_address(0), ra = 0, cfp = 0, obj = 0, i;
    if (!code) return;
    probe_read((unsigned int)fp + 4, &ra);
    probe_read((unsigned int)fp, &cfp);
    if (cfp) probe_read(cfp + 8, &obj);
    fprintf(stderr, "[trfatal] translate fatal(%d, %d, %p, %#x) from %08x (allocator object %08x)\n", code, z, fmt, arg, ra, obj);
    if (obj) {
        unsigned int w[48];
        for (i = 0; i < 48 && probe_read(obj + 4 * i, &w[i]); i++) ;
        fprintf(stderr, "[trfatal]   object:"); { unsigned int k; for (k = 0; k < i; k++) fprintf(stderr, " %08x", w[k]); } fprintf(stderr, "\n");
        {   unsigned int pools[3] = { 0x84, 0x2c, 0 }, k;
            for (k = 0; k < 2; k++) {
                unsigned int p = 0, lo = 0, hi = 0, a = 0, b = 0;
                if (!probe_read(obj + pools[k], &p) || !p) continue;
                probe_read(p + 0x20018, &lo); probe_read(p + 0x2001c, &hi); probe_read(p + 0x20010, &a); probe_read(p + 0x20014, &b);
                fprintf(stderr, "[trfatal]   pool +%#x = %08x: [+0x20010]=%08x [+0x20014]=%08x min=%#x max=%#x\n", pools[k], p, a, b, lo, hi);
            }
        }
    }
    oah_hang_dump_all("translate fatal");
    oah_report_mark("fatal");
    oah_report_flush();
    exit(1);
}
static void oah_tr_fatal_hook(void) {
    unsigned char *t = (unsigned char *)0xb800c540;
    static const unsigned char want[] = { 0x55, 0x89, 0xe5, 0x83, 0xec, 0x18, 0x8b, 0x45, 0x08, 0x85, 0xc0 };
    int rel;
    if (getenv("OAH_NO_TRFATAL") || memcmp(t, want, sizeof want)) return;
    if (mprotect((void *)((unsigned int)t & ~0xfffu), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC)) return;
    rel = (int)((unsigned int)oah_tr_fatal - (unsigned int)(t + 5));
    t[0] = 0xe9; memcpy(t + 1, &rel, 4);
    mprotect((void *)((unsigned int)t & ~0xfffu), 0x2000, PROT_READ | PROT_EXEC);
}

void my__exit(int status) {
    if (status && !getenv("OAH_QUIET_EXIT")) {
        unsigned int st[1]; st[0] = (unsigned int)status;
        oah_exit_note(st);
    }
    if (getenv("OAH_VERBOSE")) {
        fprintf(stderr, "[oah__exit] status=%d caller=%p\n", status, __builtin_return_address(0));
    }
    _exit(status);
}

void my_exit(int status) {
    if (status && !getenv("OAH_QUIET_EXIT")) {
        unsigned int st[1]; st[0] = (unsigned int)status;
        oah_exit_note(st);
    }
    if (getenv("OAH_VERBOSE")) {
        fprintf(stderr, "[oah_exit] status=%d caller0=%p caller1=%p caller2=%p\n",
                status, __builtin_return_address(0), __builtin_return_address(1), __builtin_return_address(2));
        unsigned int *fp1 = (unsigned int *)__builtin_frame_address(1);
        if (fp1) {
            unsigned int *args = fp1 + 2;
            int k;
            fprintf(stderr, "[oah_exit] logger args:");
            for (k = 0; k < 6; k++) fprintf(stderr, " 0x%08x", args[k]);
            fprintf(stderr, "\n");
            for (k = 0; k < 4; k++) {
                if (args[k] > 0xb8000000U && args[k] < 0xb8300000U)
                    fprintf(stderr, "[oah_exit] arg%d str: \"%.200s\"\n", k, (const char *)args[k]);
            }
        }
        if (getenv("OAH_DUMPREG")) {
            unsigned int *fp2 = (unsigned int *)__builtin_frame_address(2);
            if (fp2) {
                unsigned int self = fp2[2], addr = fp2[3];
                unsigned int *t = (unsigned int *)self;
                int k, n;
                fprintf(stderr, "[reg] lookup this=0x%08x addr=0x%08x ret=0x%08x\n", self, addr, fp2[1]);
                if (self > 0x1000 && self < 0xf0000000U) {
                    for (k = 0; k < 0x24; k++)
                        fprintf(stderr, "[reg] this+0x%02x = 0x%08x\n", k * 4, t[k]);
                    /* cached current region at this+0x84 */
                    unsigned int cur = t[0x84 / 4];
                    if (cur > 0x1000 && cur < 0xf0000000U)
                        fprintf(stderr, "[reg] cur(+0x84) obj=0x%08x range 0x%08x-0x%08x\n", cur,
                                *(unsigned int *)(cur + 0x20018), *(unsigned int *)(cur + 0x2001c));
                    /* list A: container at this+0x0c, head at +0x10, node {data,next}, data has range at +0x20018 */
                    unsigned int node = t[0x10 / 4];
                    for (n = 0; node > 0x1000 && node < 0xf0000000U && n < 64; n++) {
                        unsigned int d = *(unsigned int *)node;
                        if (d > 0x1000 && d < 0xf0000000U)
                            fprintf(stderr, "[reg] A[%d] obj=0x%08x range 0x%08x-0x%08x\n", n, d,
                                    *(unsigned int *)(d + 0x20018), *(unsigned int *)(d + 0x2001c));
                        node = *(unsigned int *)(node + 4);
                    }
                    /* list B: container at this+0x1c, head at +0x20, node {data,next}, data = {start,end,...} */
                    node = t[0x20 / 4];
                    for (n = 0; node > 0x1000 && node < 0xf0000000U && n < 256; n++) {
                        unsigned int d = *(unsigned int *)node;
                        if (d > 0x1000 && d < 0xf0000000U)
                            fprintf(stderr, "[reg] B[%d] d=0x%08x start=0x%08x end=0x%08x w2=0x%08x w3=0x%08x\n", n, d,
                                    ((unsigned int *)d)[0], ((unsigned int *)d)[1], ((unsigned int *)d)[2], ((unsigned int *)d)[3]);
                        node = *(unsigned int *)(node + 4);
                    }
                }
            }
        }
    }
    /* The guest's own exit() already ran its atexit handlers and flushed its
       stdio. Native exit() would also run Mojave's destructors, and in
       daemons (fontd) that trips heap checks in tiny_free and the process
       then spins on the exception instead of exiting. */
    fflush(NULL);
    if (getenv("OAH_NATIVE_EXIT")) exit(status);
    _exit(status);
}

mach_msg_return_t my_mach_msg(mach_msg_header_t *msg, mach_msg_option_t option,
                              mach_msg_size_t send_size, mach_msg_size_t rcv_size,
                              mach_port_t rcv_name, mach_msg_timeout_t timeout,
                              mach_port_t notify) {
    mach_msg_id_t req_id = msg ? msg->msgh_id : 0;
    mach_msg_return_t kr = mach_msg(msg, option, send_size, rcv_size, rcv_name, timeout, notify);
    /* OAH_EXCLOG=1: Mach exceptions the translator receives on its own port (a native fault it
       turns into a guest signal, often SIGTRAP with no guest pc): exception, codes, native state */
    if (kr == 0 && msg && (option & MACH_RCV_MSG) && msg->msgh_id == 2403 && getenv("OAH_EXCLOG")) {
        unsigned char *b = (unsigned char *)msg;
        unsigned int thr = *(unsigned int *)(b + 28), exc = *(unsigned int *)(b + 60), nc = *(unsigned int *)(b + 64), k;
        unsigned int *cd = (unsigned int *)(b + 68), fl, sc, *st;
        if (nc > 2) nc = 2;
        fl = cd[nc]; sc = cd[nc + 1]; st = cd + nc + 2;
        fprintf(stderr, "[excmsg] thread %x exception %u codes", thr, exc);
        for (k = 0; k < nc; k++) fprintf(stderr, " %08x", cd[k]);
        fprintf(stderr, " flavor %u", fl);
        if (fl == 1 && sc >= 16) {
            unsigned int eip = st[10], ebp = st[6], f, ra, n;
            fprintf(stderr, " eip %08x esp %08x ebp %08x eax %08x ecx %08x edx %08x esi %08x edi %08x; bt:", eip, st[7], ebp, st[0], st[2], st[3], st[5], st[4]);
            for (f = ebp, n = 0; n < 12 && f > 0x1000; n++) {
                unsigned int nf;
                if (!probe_read(f + 4, &ra) || !probe_read(f, &nf)) break;
                { Dl_info di; if (dladdr((void *)(uintptr_t)ra, &di) && di.dli_sname) fprintf(stderr, " %08x(%s)", ra, di.dli_sname); else fprintf(stderr, " %08x", ra); }
                if (nf <= f) break;
                f = nf;
            }
        }
        fprintf(stderr, "\n");
    }
    if (getenv("OAH_VERBOSE")) {
        fprintf(stderr, "[oah_mach_msg] req_id=%d resp_id=%d send=%u rcv=%u kr=0x%x\n",
                req_id, msg ? msg->msgh_id : 0, send_size, rcv_size, kr);
        if (msg && (req_id == 3801 || req_id == 4800) && kr == 0) {
            unsigned int *w = (unsigned int *)((char *)msg + sizeof(mach_msg_header_t));
            fprintf(stderr, "[oah_mach_msg]   vm_allocate reply ndr=%08x %08x ret=%08x addr=%08x %08x\n",
                    w[0], w[1], w[2], w[3], w[4]);
        }
    }
    return kr;
}

__attribute__((visibility("default")))
kern_return_t map_fd(int fd, vm_offset_t offset, vm_offset_t *va, boolean_t find_space, vm_size_t size) {
    if (!va) return KERN_INVALID_ARGUMENT;
    if (find_space) {
        vm_address_t addr = 0;
        kern_return_t kr = vm_allocate(mach_task_self(), &addr, size, VM_FLAGS_ANYWHERE);
        if (kr != KERN_SUCCESS) return kr;
        *va = addr;
    }
    void *target = (void *)(*va);
    if (((offset & 0xfff) == 0) && (((uintptr_t)target & 0xfff) == 0)) {
        void *res = mmap(target, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, (off_t)offset);
        if (res != MAP_FAILED) {
            return KERN_SUCCESS;
        }
    }
    ssize_t n = pread(fd, target, size, (off_t)offset);
    if (n < 0) return KERN_FAILURE;
    return KERN_SUCCESS;
}

int my_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    char buf[2048];
    const char *newpath = translate_path(path, buf, sizeof(buf));
    if (getenv("OAH_VERBOSE")) {
        fprintf(stderr, "[oah_open] %s (orig: %s)\n", newpath, path);
    }
    return open(newpath, flags, mode);
}

int my_stat(const char *path, struct stat *st) {
    char buf[2048];
    const char *newpath = translate_path(path, buf, sizeof(buf));
    return stat(newpath, st);
}

int my_lstat(const char *path, struct stat *st) {
    char buf[2048];
    const char *newpath = translate_path(path, buf, sizeof(buf));
    return lstat(newpath, st);
}

int my_access(const char *path, int amode) {
    char buf[2048];
    const char *newpath = translate_path(path, buf, sizeof(buf));
    return access(newpath, amode);
}

int my_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    if (name && strcmp(name, "kern.exec.archhandler.powerpc") == 0) {
        const char *prog = g_self_path[0] ? g_self_path : "/usr/libexec/oah/translate";
        size_t len = strlen(prog) + 1;
        if (oldp && oldlenp && *oldlenp >= len) {
            memcpy(oldp, prog, len);
        }
        if (oldlenp) *oldlenp = len;
        return 0;
    }
    if (name && strcmp(name, "sysctl.proc_exec_affinity") == 0) {
        return 0;
    }
    return sysctlbyname(name, oldp, oldlenp, newp, newlen);
}
extern CFMutableDictionaryRef IOServiceMatching(const char *name);   /* as IOKitLib.h has it (oah_hidcom.h includes it) */
void *my_IOServiceMatching(const char *name) {
    if (name && strcmp(name, "AppleGraphicsControl") == 0 && oah_caller_is_guest(__builtin_return_address(0))) {
        return NULL;
    }
    return (void *)IOServiceMatching(name);
}


/* Mojave's OpenGL fails to find any renderer (kCGLBadPixelFormat) if it is first
   initialised from inside a bridged guest call; HIToolbox then crashes drawing the
   menu bar through CoreImage. Initialising it once here, before Rosetta starts,
   avoids that. OAH_NO_GLWARM=1 disables. */
static void oah_glwarm(void) {
    void *h = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
    int (*choose)(const int *, void **, int *) = h ? (int (*)(const int *, void **, int *))dlsym(h, "CGLChoosePixelFormat") : 0;
    void (*release)(void *) = h ? (void (*)(void *))dlsym(h, "CGLReleasePixelFormat") : 0;
    int attrs[] = { 73, 0 }, n = 0, e;
    void *pf = 0;
    if (!choose) return;
    e = choose(attrs, &pf, &n);
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[glwarm] err=%d renderers=%d\n", e, n);
    if (pf && release) release(pf);
}

/* Decide at load time whether the guest may draw (OpenGL must be initialised
   before Rosetta starts, see oah_glwarm). OAH_GLWARM=1 forces, OAH_NO_GLWARM=1
   disables; otherwise scan the guest Mach-O for GUI framework links. */
#include <crt_externs.h>
static int oah_guest_wants_gl(void) {
    static const char *const keys[] = { "/Carbon.framework", "/AppKit.framework", "/Cocoa.framework",
        "/ApplicationServices.framework", "/CoreGraphics.framework", "/OpenGL.framework", "/AGL.framework",
        "/QuickTime.framework", "/GLUT.framework", "oah_CreateNewWindow", 0 };
    int argc = *_NSGetArgc(), fd, n, i, hit = 0;
    char **argv = *_NSGetArgv();
    static char buf[262144];
    if (getenv("OAH_NO_GLWARM")) return 0;
    if (getenv("OAH_GLWARM")) return 1;
    if (argc < 2 || !argv[1]) return 0;
    if (strstr(argv[1], ".app/")) hit = 1;
    if (!hit && (fd = open(argv[1], O_RDONLY)) >= 0) {
        n = (int)read(fd, buf, sizeof buf);
        close(fd);
        for (i = 0; keys[i] && !hit && n > 0; i++)
            if (oah_memmem(buf, (size_t)n, keys[i], strlen(keys[i]))) hit = 1;
    }
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[glwarm] guest %s -> %s\n", argv[1], hit ? "warm" : "skip");
    return hit;
}

/* Lazy variant (does NOT work, kept for reference): on the first GUI bridge call, initialise OpenGL
   on a separate native thread (the guest thread is where it fails) and wait. */
static int g_glwarm_done;
static void *oah_glwarm_thread(void *arg) { (void)arg; oah_glwarm(); return 0; }
static void oah_ensure_glwarm(void) {
    pthread_t t;
    if (g_glwarm_done) return;
    g_glwarm_done = 1;
    if (getenv("OAH_NO_GLWARM")) return;
    if (pthread_create(&t, 0, oah_glwarm_thread, 0) == 0) pthread_join(t, 0);
}

/* OAH_GLTEST=1: try a native CGL context inside this process and report. */
static void oah_gltest(const char *when) {
    void *h = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
    int (*choose)(const int *, void **, int *) = h ? (int (*)(const int *, void **, int *))dlsym(h, "CGLChoosePixelFormat") : 0;
    int (*create)(void *, void *, void **) = h ? (int (*)(void *, void *, void **))dlsym(h, "CGLCreateContext") : 0;
    const char *(*errstr)(int) = h ? (const char *(*)(int))dlsym(h, "CGLErrorString") : 0;
    int attrs_acc[] = { 73 /*kCGLPFAAccelerated*/, 0 }, attrs_any[] = { 0 }, n = 0, e;
    void *pf = 0, *ctx = 0;
    if (!choose || !create) { fprintf(stderr, "[gltest %s] OpenGL not loadable\n", when); return; }
    e = choose(attrs_acc, &pf, &n);
    fprintf(stderr, "[gltest %s] ChoosePixelFormat(accelerated) err=%d (%s) pf=%p n=%d\n", when, e, errstr ? errstr(e) : "", pf, n);
    if (!pf) { e = choose(attrs_any, &pf, &n);
        fprintf(stderr, "[gltest %s] ChoosePixelFormat(default) err=%d (%s) pf=%p n=%d\n", when, e, errstr ? errstr(e) : "", pf, n); }
    if (pf) { e = create(pf, 0, &ctx);
        fprintf(stderr, "[gltest %s] CreateContext err=%d (%s) ctx=%p\n", when, e, errstr ? errstr(e) : "", ctx); }
}

static int g_trace = -1;
static int trace_on(void) { if (g_trace < 0) g_trace = getenv("OAH_TRACE") ? 1 : 0; return g_trace; }

long my_syscall(int num, ...) {
    va_list ap; unsigned int a[8]; int i;
    va_start(ap, num);
    for (i = 0; i < 8; i++) a[i] = va_arg(ap, unsigned int);
    va_end(ap);
    if (num == 184) {
        /* sigreturn from the translator's signal trampoline (0xb809f0da, which is both
           handler and sa_tramp). Since 10.14 the kernel calls the trampoline with a
           sixth argument, a token, and refuses sigreturn(uctx, style) without it; the
           10.6 translator never passes it, so sigreturn failed and the trampoline
           returned to the kernel's fake return address 0xffffffff (every guest
           signal crashed). The token is still in the trampoline's frame: kernel args
           at ebp+8 catcher, +0xc style, +0x10 sig, +0x14 sinfo, +0x18 uctx, +0x1c token. */
        unsigned int *fp = (unsigned int *)__builtin_frame_address(1);
        if (fp && fp[6] == a[0] && !getenv("OAH_NO_SIGTOKEN")) {
            long r2 = syscall(184, a[0], a[1], fp[7]);
            fprintf(stderr, "[sig] sigreturn with token failed: %ld errno %d\n", r2, errno);
            return r2;
        }
    }
    long r = syscall(num, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    if (trace_on()) {
        int e = errno;
        fprintf(stderr, "[sc] %d(0x%x,0x%x,0x%x,0x%x,0x%x,0x%x) = %ld errno=%d\n",
                num, a[0], a[1], a[2], a[3], a[4], a[5], r, r == -1 ? e : 0);
        errno = e;
    }
    return r;
}

unsigned int g_blk[8], g_nblk;   /* translator 26 MB pool blocks, in allocation order (memory work) */
kern_return_t my_vm_allocate(vm_map_t t, vm_address_t *addr, vm_size_t size, int flags) {
    vm_address_t req = addr ? *addr : 0;
    kern_return_t kr = vm_allocate(t, addr, size, flags);
    if (!kr && size == 0x1a01000) { extern unsigned int g_blk[8], g_nblk; if (g_nblk < 8) g_blk[g_nblk++] = (unsigned int)*addr; }
    if (trace_on())
        fprintf(stderr, "[vm_allocate] req=0x%08x size=0x%x flags=0x%x -> 0x%08x kr=%d caller=%p\n",
                (unsigned)req, (unsigned)size, flags, addr ? (unsigned)*addr : 0, kr, __builtin_return_address(0));
    return kr;
}

kern_return_t my_vm_map(vm_map_t t, vm_address_t *addr, vm_size_t size, vm_address_t mask, int flags,
                        mem_entry_name_port_t obj, vm_offset_t off, boolean_t copy, vm_prot_t cur,
                        vm_prot_t max, vm_inherit_t inh) {
    vm_address_t req = addr ? *addr : 0;
    kern_return_t kr = vm_map(t, addr, size, mask, flags, obj, off, copy, cur, max, inh);
    if (trace_on())
        fprintf(stderr, "[vm_map] req=0x%08x size=0x%x flags=0x%x obj=0x%x -> 0x%08x kr=%d caller=%p\n",
                (unsigned)req, (unsigned)size, flags, obj, addr ? (unsigned)*addr : 0, kr, __builtin_return_address(0));
    return kr;
}


kern_return_t my_vm_remap(vm_map_t t, vm_address_t *ta, vm_size_t size, vm_address_t mask, int flags, vm_map_t src,
                          vm_address_t sa, boolean_t copy, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inh) {
    vm_address_t req = ta ? *ta : 0;
    kern_return_t kr = vm_remap(t, ta, size, mask, flags, src, sa, copy, cur, max, inh);
    if (trace_on())
        fprintf(stderr, "[vm_remap] req=0x%08x size=0x%x flags=0x%x src=0x%08x copy=%d -> 0x%08x kr=%d caller=%p\n",
                (unsigned)req, (unsigned)size, flags, (unsigned)sa, copy, ta ? (unsigned)*ta : 0, kr, __builtin_return_address(0));
    return kr;
}
kern_return_t my_vm_protect(vm_map_t t, vm_address_t a, vm_size_t s, boolean_t setmax, vm_prot_t p) {
    kern_return_t kr = vm_protect(t, a, s, setmax, p);
    if (trace_on() && (kr || s >= 0x100000))
        fprintf(stderr, "[vm_protect] 0x%08x size=0x%x max=%d prot=%d kr=%d\n", (unsigned)a, (unsigned)s, setmax, p, kr);
    return kr;
}

void *my_mmap(void *a, size_t len, int prot, int flags, int fd, off_t off) {
    void *r = mmap(a, len, prot, flags, fd, off);
    if (trace_on())
        fprintf(stderr, "[mmap] %p len=0x%x prot=%d flags=0x%x fd=%d off=0x%llx -> %p\n",
                a, (unsigned)len, prot, flags, fd, (unsigned long long)off, r);
    return r;
}

/* ---- NDR conversion for guest (big-endian) Mach messages ----
 * Snow Leopard's kernel MIG servers converted big-endian request bodies using the
 * NDR record; Mojave's do not. Rosetta swaps the header/descriptors but leaves the
 * body big-endian, so we convert known routines here before the trap. */
struct ndr_layout { int id; const char *fmt; };
/* w = 32-bit int, q = 64-bit int, x = skip 4 bytes */
static const struct ndr_layout g_layouts[] = {
    { 200,  "ww" },        /* host_info(flavor, count) */
    { 206,  "w" },         /* host_get_clock_service(clock_id) */
    { 202,  "w" },         /* host_page_size? (no args) - harmless */
    { 3200, "" },          /* mach_port_names */
    { 3201, "w" },         /* mach_port_type(name) */
    { 3204, "w" },         /* mach_port_allocate(right) */
    { 3205, "w" },         /* mach_port_destroy(name) */
    { 3206, "w" },         /* mach_port_deallocate(name) */
    { 3207, "ww" },        /* mach_port_get_refs(name,right) */
    { 3208, "www" },       /* mach_port_mod_refs(name,right,delta) */
    { 3202, "ww" },        /* mach_port_rename(old, new) */
    { 3203, "ww" },        /* mach_port_allocate_name(right, name) */
    { 3210, "ww" },        /* mach_port_set_mscount(name, mscount) */
    { 3211, "w" },         /* mach_port_get_set_status(name) */
    { 3212, "ww" },        /* mach_port_move_member(member, after) */
    { 3213, "www" },       /* mach_port_request_notification(name, msgid, sync) + port desc */
    { 3214, "w" },         /* mach_port_insert_right(name) + port desc */
    { 3215, "ww" },        /* mach_port_extract_right(name, msgt_name) */
    { 3216, "ww" },        /* mach_port_set_seqno(name, seqno) */
    { 3217, "www" },       /* mach_port_get_attributes(name, flavor, count) */
    { 3222, "w" },         /* mach_port_get_srights(name) */
    { 3228, "w" },         /* mach_port_get_context(name) */
    { 3229, "wq" },        /* mach_port_set_context(name, context) */
    { 3218, "wwwa" },      /* mach_port_set_attributes(name, flavor, count, info[count]) */
    { 3226, "ww" },        /* mach_port_insert_member(name, pset) */
    { 3227, "ww" },        /* mach_port_extract_member(name, pset) */
    { 3616, "wwaw" },      /* thread_policy(policy, baseCnt, base[], set_limit) */
    { 3405, "ww" },        /* task_info(flavor, count) */
    { 3409, "w" },         /* task_get_special_port(which) */
    { 3410, "w" },         /* task_set_special_port(which) + port desc */
    { 3418, "ww" },        /* semaphore_create(policy, value) */
    { 3801, "www" },       /* vm_allocate(addr,size,flags) */
    { 3802, "ww" },        /* vm_deallocate(addr,size) */
    { 3803, "wwww" },      /* vm_protect(addr,size,set_max,prot) */
    { 3804, "www" },       /* vm_inherit(addr,size,inherit) */
    { 3805, "ww" },        /* vm_read(addr,size) */
    { 3812, "wwwwwwwww" }, /* vm_map(addr,size,mask,flags,offset,copy,cur,max,inh) */
    { 3800, "www" },       /* vm_region(addr,flavor,count) */
    { 3807, "ww" },        /* vm_write(addr) + OOL data, dataCnt */
    { 3808, "www" },       /* vm_copy(src,size,dst)  -- libATSServer VMGrow */
    { 3809, "www" },       /* vm_read_overwrite(addr,size,data) */
    { 3810, "www" },       /* vm_msync(addr,size,flags) */
    { 3811, "www" },       /* vm_behavior_set(addr,size,behavior) */
    { 3813, "wwww" },      /* vm_machine_attribute(addr,size,attr,value) */
    { 3814, "wwwwwww" },   /* vm_remap(target,size,mask,anywhere,src,copy,inh) + task port desc */
    { 3815, "w" },         /* task_wire(must_wire) */
    { 3817, "w" },         /* vm_map_page_query(offset) */
    { 3821, "www" },       /* vm_region_recurse(addr,depth,count) */
    { 3822, "www" },       /* vm_region_recurse_64(addr,depth,count) */
    { 3824, "www" },       /* vm_region_64(addr,flavor,count) */
    { 3830, "www" },       /* vm_purgable_control(addr,control,state) */
    /* shared memory between processes (ATS client <-> fontd rendezvous) */
    { 3816, "www" },       /* mach_make_memory_entry(size,offset,perm) + parent port desc */
    { 3825, "qqw" },       /* mach_make_memory_entry_64(size,offset,perm) + parent port desc */
    { 3826, "wwwwqwwww" }, /* vm_map_64(addr,size,mask,flags,offset64,copy,cur,max,inh) + object port */
    { 4811, "qqqwqwwww" }, /* mach_vm_map(addr,size,mask,flags,offset,copy,cur,max,inh) + object port */
    { 4813, "qqqwqww" },   /* mach_vm_remap(addr,size,mask,flags,src_addr,copy,inh) + src task port */
    { 4803, "qqw" },       /* mach_vm_inherit */
    { 4804, "qq" },        /* mach_vm_read */
    { 4807, "qqq" },       /* mach_vm_copy */
    { 4808, "qqq" },       /* mach_vm_read_overwrite */
    { 4809, "qqw" },       /* mach_vm_msync */
    { 4810, "qqw" },       /* mach_vm_behavior_set */
    { 4815, "qww" },       /* mach_vm_region_recurse(addr,depth,count) */
    { 4816, "qww" },       /* mach_vm_region(addr,flavor,count) */
    { 4800, "qqw" },       /* mach_vm_allocate */
    { 4801, "qq" },        /* mach_vm_deallocate */
    { 4802, "qqww" },      /* mach_vm_protect */
    { 4806, "qw" },        /* mach_vm_write(addr) + OOL data, dataCnt  -- CFPasteboard large data */
    { 4814, "q" },         /* mach_vm_page_query(offset) */
    { 4818, "qww" },       /* mach_vm_purgable_control(addr,control,state) */
    /* IOKit (iokit.defs, subsystem 2800). A string argument travels as Offset, Cnt and Cnt bytes
       rounded up to 4 ('s' skips them); found with ScreenFlow, whose start-up reads the primary
       network interface's MAC address and stopped with "Fatal Initialize Error" (MIG -304). */
    { 2801, "wws" },       /* io_object_conforms_to(className) */
    { 2804, "wws" },       /* io_service_get_matching_services(matching) */
    { 2805, "wws" },       /* io_registry_entry_get_property(property_name) */
    { 2806, "wwsw" },      /* io_registry_create_iterator(plane, options) */
    { 2809, "wws" },       /* io_registry_entry_from_path(path) */
    { 2812, "wwsw" },      /* io_registry_entry_get_property_bytes(property_name, dataCnt) */
    { 2813, "wws" },       /* io_registry_entry_get_child_iterator(plane) */
    { 2814, "wws" },       /* io_registry_entry_get_parent_iterator(plane) */
    { 2826, "wws" },       /* io_registry_entry_get_path(plane) */
    { 2829, "wws" },       /* io_registry_entry_in_plane(plane) */
    { 2832, "ww" },        /* io_service_wait_quiet(wait_time) */
    { 2833, "wwsw" },      /* io_registry_entry_create_iterator(plane, options) */
    { 2835, "www" },       /* io_make_matching(of_type, options, inputCnt) */
    { 2843, "wws" },       /* io_registry_entry_get_name_in_plane(plane) */
    { 2844, "wws" },       /* io_service_match_property_table(matching) */
    { 2854, "wws" },       /* io_registry_entry_get_location_in_plane(plane) */
    { 2855, "wwswwsw" },   /* io_registry_entry_get_property_recursively(plane, property_name, options) */
    { 2857, "w" },         /* io_service_get_matching_services_ool(matchingCnt) + OOL matching */
    { 2858, "w" },         /* io_service_match_property_table_ool(matchingCnt) + OOL matching */
    { 2860, "wws" },       /* io_object_get_superclass(obj_name) */
    { 2861, "wws" },       /* io_object_get_bundle_identifier(obj_name) */
    /* ids from the order of Snow Leopard's iokitmig.h request union (2865 = io_connect_method).
       Not converted: 2859 io_service_add_notification_ool, whose reference array comes back to
       the guest as it was sent, and the io_connect_* and catalog routines. */
    { 0, 0 }
};
static unsigned int g_unknown_ids[64]; static int g_n_unknown;

/* ---- in-process bootstrap proxy for Snow Leopard look_up2 / check_in2 ---- */
/* Snow Leopard services that Mojave no longer provides are served by translated
 * PowerPC daemons registered under com.rosettus.ppc.* (see agents/ in the repository). */
static void oah_bs_map(const char *name, char *out, size_t outsz) {
    /* coreservicesd: Snow Leopard's LaunchServices (file types/UTIs, app bindings) cannot
       talk to Mojave's; without it the font worker finds no font files at all. */
    static const char *const mapped[] = { "com.apple.FontServer", "com.apple.FontObjectsServer", "com.apple.FontWorker",
                                          "com.apple.CoreServices.coreservicesd",
                                          "com.apple.pasteboard.1",   /* 10.6 pboard (translated agent); Mojave's speaks XPC */
                                          0 };
    int i;
    /* the spell server: a 10.6 NSSpellChecker speaks a protocol Mojave's AppleSpell no longer
       has (_dataFromCheckingString:... raised NSInvalidArgumentException in the server); the
       guest uses Snow Leopard's AppleSpell.service from the guest tree, translated (oah_spell_start) */
    if (!getenv("OAH_BS_NOMAP") && !getenv("OAH_NO_PPCSPELL")) {
        size_t L = strlen(name);
        /* NSSpellServer registers one name per language, "<lang> (<vendor>)_OpenStep" (Mojave's
           AppleSpell holds the same names) */
        if (!strcmp(name, "AppleSpell") || (L > 17 && !strcmp(name + L - 17, " (Apple)_OpenStep"))) {
            snprintf(out, outsz, "com.rosettus.ppc.%s", name);
            return;
        }
    }
    if (!getenv("OAH_BS_NOMAP"))
        for (i = 0; mapped[i]; i++)
            if (!strncmp(name, mapped[i], strlen(mapped[i]))) {
                snprintf(out, outsz, "com.rosettus.ppc.%s", name + 10);
                return;
            }
    snprintf(out, outsz, "%s", name);
}
/* Start Snow Leopard's AppleSpell.service (guest tree) under the translator, once: a guest asked
   for the spell server and it is not running. Returns 1 when it is starting (worth waiting). */
static int oah_launch_translated(const char *app, CFArrayRef docs);
static int oah_spell_start(void) {
    static const char svc[] = "/System/Library/Services/AppleSpell.service";
    const char *lock = "/tmp/.rosettus_spell_start";
    char t[1200]; struct stat sb;
    int fd;
    snprintf(t, sizeof t, "%s%s/Contents/MacOS/AppleSpell", g_rosetta_root, svc);
    if (stat(t, &sb)) return 0;
    if (!stat(lock, &sb) && time(0) - sb.st_mtime < 20) return 1;   /* another process is starting it */
    if ((fd = open(lock, O_WRONLY | O_CREAT | O_TRUNC, 0666)) >= 0) close(fd);
    else return 0;
    utimes(lock, 0);
    fprintf(stderr, "[spell] starting Snow Leopard's spell server (translated)\n");
    g_ls_launch_flags = 0x200;   /* a service: never brought to the front */
    return oah_launch_translated(svc, NULL) == 0;
}
static mach_port_t g_bs_proxy;
extern kern_return_t bootstrap_register2(mach_port_t, const char *, mach_port_t, uint64_t);
extern kern_return_t bootstrap_look_up2(mach_port_t, const char *, mach_port_t *, pid_t, uint64_t);
static void *bs_proxy_thread(void *arg) {
    union { mach_msg_header_t h; char b[2048]; } rq;
    (void)arg;
    for (;;) {
        memset(&rq.h, 0, sizeof rq.h);
        kern_return_t kr = mach_msg(&rq.h, MACH_RCV_MSG | MACH_RCV_LARGE, 0, sizeof rq, g_bs_proxy, 0, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) {
            /* never spin: a dead/invalid port ends the proxy; anything else backs off (Keynote burned
               a whole core of system time in this loop) */
            static int nlog;
            if (nlog++ < 5) fprintf(stderr, "[bsproxy] receive failed 0x%x (port 0x%x)\n", kr, g_bs_proxy);
            if (kr == MACH_RCV_INVALID_NAME || kr == MACH_RCV_PORT_DIED || kr == MACH_RCV_PORT_CHANGED) return 0;
            if (kr == MACH_RCV_TOO_LARGE) {   /* drop the oversized message */
                mach_msg_header_t *big = (mach_msg_header_t *)malloc(rq.h.msgh_size + 1024);
                if (big) { mach_msg(big, MACH_RCV_MSG, 0, rq.h.msgh_size + 1024, g_bs_proxy, 0, MACH_PORT_NULL); mach_msg_destroy(big); free(big); }
                continue;
            }
            usleep(10000);
            continue;
        }
        if (rq.h.msgh_remote_port == MACH_PORT_NULL) continue;
        mach_msg_bits_t rbits = MACH_MSGH_BITS_REMOTE(rq.h.msgh_bits);
        mach_port_t rport = rq.h.msgh_remote_port;
        int id = rq.h.msgh_id;
        mach_port_t sp = MACH_PORT_NULL;
        kern_return_t lk = MIG_BAD_ID;
        char name[129];
        name[0] = 0;
        int disp = MACH_MSG_TYPE_MOVE_SEND;
        {   static unsigned long nmsg; static time_t t0;
            if ((++nmsg & 1023) == 0) {
                time_t now = time(0);
                fprintf(stderr, "[bsproxy] %lu requests (%ld s since the last 1024); last id %d \"%.60s\"\n",
                        nmsg, (long)(now - t0), id, (id == 404 || id == 402) && rq.h.msgh_size >= 160 ? rq.b + 32 : "");
                t0 = now;
            }
        }
        if (getenv("OAH_BSTRACE") && id != 402 && id != 404)
            fprintf(stderr, "[bsproxy] id=%d size=%u bits=%#x\n", id, rq.h.msgh_size, rq.h.msgh_bits);
        if (id == 403 && (rq.h.msgh_bits & MACH_MSGH_BITS_COMPLEX) && rq.h.msgh_size >= 176) {
            /* bootstrap_register2(bp, name, port, flags): CFMessagePortCreatePerProcessLocal (CFPasteboard's
               client port, "<name>.<pid>") registers a per-PID service; the reply has no port */
            char mapped[160];
            mach_msg_port_descriptor_t *pd = (mach_msg_port_descriptor_t *)(rq.b + 28);
            const unsigned char *ndr = (const unsigned char *)(rq.b + 40);
            unsigned long long fl = 0; int k;
            memcpy(name, rq.b + 48, 128); name[128] = 0;
            {   /* flags as written by the guest: big-endian unless the NDR says little */
                unsigned long long v = 0;
                for (k = 0; k < 8; k++) v = (v << 8) | (unsigned char)rq.b[176 + (ndr[4] ? 7 - k : k)];
                fl = rq.h.msgh_size >= 184 ? v : 0;
            }
            oah_bs_map(name, mapped, sizeof mapped);
            lk = (fl & 1) ? oah_nb_request(1, mapped, getpid(), pd->name, 0)    /* per-PID: our name broker */
                          : bootstrap_register2(bootstrap_port, mapped, pd->name, (uint64_t)fl);
            if (getenv("OAH_VERBOSE") || getenv("OAH_BSTRACE"))
                fprintf(stderr, "[bsproxy] register2 \"%s\" port 0x%x flags 0x%llx -> kr=0x%x\n", mapped, pd->name, fl, lk);
            {   mig_reply_error_t e;
                memset(&e, 0, sizeof e);
                e.Head.msgh_bits = MACH_MSGH_BITS(rbits, 0);
                e.Head.msgh_size = sizeof e;
                e.Head.msgh_remote_port = rport;
                e.Head.msgh_id = id + 100;
                e.NDR = NDR_record;
                e.RetCode = lk;
                mach_msg(&e.Head, MACH_SEND_MSG, sizeof e, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
            }
            continue;
        }
        if ((id == 404 || id == 402) && rq.h.msgh_size >= 160) {
            char mapped[160];
            memcpy(name, rq.b + 32, 128); name[128] = 0;
            oah_bs_map(name, mapped, sizeof mapped);
            if (id == 404 && rq.h.msgh_size >= 172) {
                /* look_up2(bp, name, pid, flags): per-PID lookups (CFMessagePortCreatePerProcessRemote) */
                const unsigned char *ndr = (const unsigned char *)(rq.b + 24), *q = (const unsigned char *)(rq.b + 160);
                unsigned int pid = ndr[4] ? (q[0] | q[1] << 8 | q[2] << 16 | (unsigned)q[3] << 24) : ((unsigned)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3]);
                unsigned long long fl = 0; int k;
                for (k = 0; k < 8; k++) fl = (fl << 8) | q[4 + (ndr[4] ? 7 - k : k)];
                if (fl & 1 /* BOOTSTRAP_PER_PID_SERVICE */) {
                    lk = oah_nb_request(0, mapped, (int)pid, 0, &sp);
                    if (getenv("OAH_VERBOSE") || getenv("OAH_BSTRACE"))
                        fprintf(stderr, "[bsproxy] look_up2 per-pid \"%s\" pid %u -> kr=0x%x port=0x%x\n", mapped, pid, lk, sp);
                    goto bs_reply;
                }
            }
            if (id == 404) {
                lk = bootstrap_look_up(bootstrap_port, mapped, &sp);
                if (lk == 0x44e && !strncmp(mapped, "com.rosettus.ppc.", 17) &&
                    (!strcmp(name, "AppleSpell") || strstr(name, " (Apple)_OpenStep")) && oah_spell_start()) {
                    int w;                                   /* give it a few seconds to register */
                    for (w = 0; w < 80 && lk == 0x44e; w++) { usleep(100000); lk = bootstrap_look_up(bootstrap_port, mapped, &sp); }
                    fprintf(stderr, "[spell] \"%s\" after starting the Snow Leopard spell server -> 0x%x\n", name, lk);
                }
            } else { lk = bootstrap_check_in(bootstrap_port, mapped, &sp); disp = MACH_MSG_TYPE_MOVE_RECEIVE; }
            if (getenv("OAH_VERBOSE") || getenv("OAH_BSTRACE"))
                fprintf(stderr, "[bsproxy] id=%d %s \"%s\"%s%s -> kr=0x%x port=0x%x\n", id, id == 404 ? "look_up" : "check_in",
                        name, strcmp(name, mapped) ? " as " : "", strcmp(name, mapped) ? mapped : "", lk, sp);
        }
    bs_reply:
        if (lk == KERN_SUCCESS) {
            struct { mach_msg_header_t h; mach_msg_body_t body; mach_msg_port_descriptor_t port; } r;
            memset(&r, 0, sizeof r);
            r.h.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(rbits, 0);
            r.h.msgh_size = sizeof r;
            r.h.msgh_remote_port = rport;
            r.h.msgh_id = id + 100;
            r.body.msgh_descriptor_count = 1;
            r.port.name = sp;
            r.port.disposition = disp;
            r.port.type = MACH_MSG_PORT_DESCRIPTOR;
            mach_msg(&r.h, MACH_SEND_MSG, sizeof r, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
        } else {
            mig_reply_error_t e;
            memset(&e, 0, sizeof e);
            e.Head.msgh_bits = MACH_MSGH_BITS(rbits, 0);
            e.Head.msgh_size = sizeof e;
            e.Head.msgh_remote_port = rport;
            e.Head.msgh_id = id + 100;
            e.NDR = NDR_record;
            e.RetCode = lk;
            mach_msg(&e.Head, MACH_SEND_MSG, sizeof e, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
        }
    }
    return NULL;
}
static void start_bs_proxy(void) {
    pthread_t t;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &g_bs_proxy)) { g_bs_proxy = 0; return; }
    mach_port_insert_right(mach_task_self(), g_bs_proxy, g_bs_proxy, MACH_MSG_TYPE_MAKE_SEND);
    if (pthread_create(&t, NULL, bs_proxy_thread, NULL)) { g_bs_proxy = 0; return; }
    pthread_detach(t);
}

static volatile unsigned int *g_dbg_rcvbuf;   /* last 0x710-byte guest receive (fontd server loop) */
void oah_fix_guest_msg(mach_msg_header_t *m, int option, unsigned int send_size, unsigned int rcv_size) {
    { static int mt = -1; static time_t chk;
      if (mt < 0) mt = getenv("OAH_MSGTRACE") ? 1 : 0;
      if (!getenv("OAH_MSGTRACE") && time(0) != chk) { chk = time(0); mt = access("/tmp/oah_msgtrace_on", F_OK) == 0; }  /* runtime toggle */
      {   /* while tracing: the header the main thread's previous receive got (raw words) */
          static mach_msg_header_t *pm;
          if (mt && pthread_main_np()) {
              if (pm) { unsigned int *w = (unsigned int *)pm;
                        fprintf(stderr, "[msg]   got: %08x %08x %08x %08x %08x %08x\n", w[0], w[1], w[2], w[3], w[4], w[5]); }
              pm = (m && (option & MACH_RCV_MSG)) ? m : 0;
          }
      }
      if (mt) fprintf(stderr, "[msg] t%x m=%p opt=%#x send=%u rcv=%u id=%d rp=%#x lp=%#x\n", (unsigned)pthread_mach_thread_np(pthread_self()),
                      (void *)m, option, send_size, rcv_size, (m && (option & MACH_SEND_MSG)) ? m->msgh_id : -1,
                      (m && (option & MACH_SEND_MSG)) ? (unsigned)m->msgh_remote_port : 0, m ? (unsigned)m->msgh_local_port : 0); }
    {   /* OAH_RCVTRACE=1: main thread only - what the previous receive got (id / local port words,
           as the guest sees them by now), counted; one summary line per 2 s */
        static int rt = -1; static mach_msg_header_t *prev; static unsigned int ids[16], lps[16], cnt[16], tot; static time_t last;
        if (rt < 0) rt = getenv("OAH_RCVTRACE") ? 1 : 0;
        if (rt && pthread_main_np()) {
            if (prev) {
                unsigned int id = ((unsigned int *)prev)[5], lp = ((unsigned int *)prev)[3], k;
                for (k = 0; k < 16 && cnt[k] && !(ids[k] == id && lps[k] == lp); k++) ;
                if (k < 16) { ids[k] = id; lps[k] = lp; cnt[k]++; }
                tot++;
                if (time(0) - last >= 2) {
                    last = time(0);
                    fprintf(stderr, "[rcv] main-thread receives %u:", tot);
                    for (k = 0; k < 16 && cnt[k]; k++) fprintf(stderr, " id %08x lp %08x x%u;", ids[k], lps[k], cnt[k]);
                    fprintf(stderr, "\n"); memset(cnt, 0, sizeof cnt); tot = 0;
                }
                prev = 0;
            }
            if (m && (option & MACH_RCV_MSG)) prev = m;
        }
    }
    /* SCSessionUniverseInitConnection (coreservicesd): hand our task port to the broker first */
    if (m && (option & MACH_SEND_MSG) && (m->msgh_id == 10002 || m->msgh_id == 10000) && send_size >= 0x20) oah_tb_client_send();   /* 10000: scclient_ServerCheckin */
    {   /* OAH_MSGDUMP=id,id: dump sends with these ids, and the reply buffer at the thread's next mach_msg */
        static const char *dl = (const char *)-1; static mach_msg_header_t *pend_m; static pthread_t pend_t; static int pend_id;
        if (dl == (const char *)-1) dl = getenv("OAH_MSGDUMP");
        if (dl) {
            char key[16]; unsigned int *w; int k;
            if (pend_m && pthread_equal(pend_t, pthread_self())) {
                w = (unsigned int *)pend_m;
                fprintf(stderr, "[msgdump] pid %d reply to %d at %p:", getpid(), pend_id, (void *)pend_m);
                for (k = 0; k < 20; k++) fprintf(stderr, " %08x", w[k]);
                fprintf(stderr, "\n"); pend_m = 0;
            }
            if (m && (option & MACH_SEND_MSG)) {
                snprintf(key, sizeof key, ",%d,", m->msgh_id);
                char dl2[256]; snprintf(dl2, sizeof dl2, ",%s,", dl);
                if (strstr(dl2, key)) {
                    unsigned int n = send_size / 4; w = (unsigned int *)m;
                    if (n > 24) n = 24;
                    fprintf(stderr, "[msgdump] pid %d send id=%d size=%u:", getpid(), m->msgh_id, send_size);
                    for (k = 0; k < (int)n; k++) fprintf(stderr, " %08x", w[k]);
                    fprintf(stderr, "\n");
                    if (option & MACH_RCV_MSG) { pend_m = m; pend_t = pthread_self(); pend_id = m->msgh_id; }
                }
            }
        }
    }
    if (!(option & MACH_SEND_MSG) || !m) return;
    /* MIG clients leave msgh_size unset; the kernel uses the send_size trap argument */
    unsigned int size = send_size;
    {   static int nbig;
        if (size > 0x6f0 && nbig < 20) {   /* temporary: who sends large messages (fontd exits on MACH_RCV_TOO_LARGE) */
            nbig++;
            fprintf(stderr, "[bigmsg] pid %d id=%d size=%u bits=0x%x remote=0x%x local=0x%x opt=0x%x\n", getpid(), m->msgh_id, size,
                    m->msgh_bits, m->msgh_remote_port, m->msgh_local_port, option);
        }
    }
    if (trace_on()) {
        unsigned int *w = (unsigned int *)m;
        fprintf(stderr, "[gmsg] id=%d size=%u bits=0x%x opt=0x%x w6..w11=%08x %08x %08x %08x %08x %08x\n",
                m->msgh_id, size, m->msgh_bits, option, w[6], w[7], w[8], w[9], w[10], w[11]);
    }
    /* Snow Leopard job.defs look_up2 (404): Mojave's launchd rejects it with MIG -304,
     * so send it to our in-process proxy which answers with a native bootstrap_look_up. */
    if (((m->msgh_id == 404 && size == 172) || (m->msgh_id == 402 && size == 168) ||
         (m->msgh_id == 403 && size == 184)) && g_bs_proxy && m->msgh_remote_port == bootstrap_port) {   /* 403: register2 */
        m->msgh_remote_port = g_bs_proxy;
        return;
    }
    if (size < sizeof(mach_msg_header_t) + sizeof(NDR_record_t) || size > 0x10000) return;
    unsigned char *p = (unsigned char *)(m + 1), *end = (unsigned char *)m + size;
    if (m->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
        unsigned int cnt = ((mach_msg_body_t *)p)->msgh_descriptor_count;
        if (cnt > 64) return;
        p += sizeof(mach_msg_body_t) + cnt * 12;
    }
    if (p + sizeof(NDR_record_t) > end) return;
    NDR_record_t *ndr = (NDR_record_t *)p;
    if (ndr->int_rep != 0 || ndr->mig_vers != 0) return;   /* not a big-endian guest message */
    p += sizeof(NDR_record_t);
    const char *fmt = 0; int i;
    for (i = 0; g_layouts[i].fmt; i++) if (g_layouts[i].id == m->msgh_id) { fmt = g_layouts[i].fmt; break; }
    if (!fmt) {
        int seen = 0;
        { char t[24]; snprintf(t, sizeof t, "id=%d", m->msgh_id); oah_tel("mig", t); }
        for (i = 0; i < g_n_unknown; i++) if (g_unknown_ids[i] == (unsigned)m->msgh_id) seen = 1;
        if (!seen && g_n_unknown < 64 && getenv("OAH_VERBOSE")) {
            g_unknown_ids[g_n_unknown++] = m->msgh_id;
            fprintf(stderr, "[ndr] unknown guest msg id=%d size=%u bits=0x%x remote=0x%x\n",
                    m->msgh_id, size, m->msgh_bits, m->msgh_remote_port);
            {
                unsigned int *bw = (unsigned int *)p; int k, n = (int)((end - p) / 4);
                if (n > 12) n = 12;
                fprintf(stderr, "[ndr]   body:");
                for (k = 0; k < n; k++) fprintf(stderr, " %08x", bw[k]);
                fprintf(stderr, "\n");
                if (m->msgh_id == 404 || m->msgh_id == 402 || m->msgh_id == 403)
                    fprintf(stderr, "[ndr]   name=\"%.128s\"\n", (const char *)p);
            }
        }
        return;
    }
    unsigned int last = 0;
    for (; *fmt; fmt++) {
        if (*fmt == 'w' && p + 4 <= end) { last = *(unsigned int *)p = OSSwapInt32(*(unsigned int *)p); p += 4; }
        else if (*fmt == 'a') {   /* array of 32-bit ints whose count is the previous 'w' */
            unsigned int k;
            for (k = 0; k < last && k < 64 && p + 4 <= end; k++, p += 4)
                *(unsigned int *)p = OSSwapInt32(*(unsigned int *)p);
        }
        else if (*fmt == 'q' && p + 8 <= end) {
            unsigned int hi = OSSwapInt32(((unsigned int *)p)[0]), lo = OSSwapInt32(((unsigned int *)p)[1]);
            ((unsigned int *)p)[0] = lo; ((unsigned int *)p)[1] = hi; p += 8;
        } else if (*fmt == 'x') p += 4;
        else if (*fmt == 's') { if (last > 4096) break; p += (last + 3) & ~3u; }   /* string of the previous count */
    }
    ndr->int_rep = NDR_record.int_rep;
    if (trace_on()) fprintf(stderr, "[ndr] converted guest msg id=%d size=%u\n", m->msgh_id, size);
}

/* Called (via patched NOP slot) right before Rosetta's `int $0x81`. eax = trap number. */
__asm__(
    ".text\n"
    ".globl _oah_mach_hook\n"
    "_oah_mach_hook:\n"
    "  cmpl $-31, %eax\n"
    "  je 1f\n"
    "  cmpl $-32, %eax\n"
    "  je 1f\n"
    "  ret\n"
    "1:\n"
    "  pushl %eax\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  andl $-16, %esp\n"
    "  pushl 36(%ebp)\n"      /* rcv_size */
    "  pushl 32(%ebp)\n"      /* send_size */
    "  pushl 28(%ebp)\n"      /* option */
    "  pushl 24(%ebp)\n"      /* msg */
    "  call _oah_fix_guest_msg\n"
    "  movl %ebp, %esp\n"
    "  popl %ebp\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  popl %eax\n"
    "  ret\n"
);
extern void oah_mach_hook(void);

/* ---- guest execve of PowerPC programs ----
   On 10.6 the kernel started Rosetta by itself when a translated process
   exec'd another PowerPC binary (fontd forks and execs genatsdb to rebuild
   the font database). Mojave's kernel answers EBADARCH, and the child then
   crashes in the translator. At the int $0x80 the translator has already
   built native path/argv/envp; when the target is PowerPC, point them at our
   rosetta script instead. Static buffers: this may run in a vfork child. */
static int oah_is_ppc_file(const char *path) {
    unsigned char h[4096]; int fd = open(path, O_RDONLY), n, ppc = 0, x86 = 0; unsigned int k, nf;
    if (fd < 0) return 0;
    n = (int)read(fd, h, sizeof h); close(fd);
    if (n < 8) return 0;
    if (h[0] == 0xfe && h[1] == 0xed && h[2] == 0xfa && h[3] == 0xce) return OSReadBigInt32(h, 4) == 18;
    if (h[0] == 0xca && h[1] == 0xfe && h[2] == 0xba && h[3] == 0xbe) {
        nf = OSReadBigInt32(h, 4);
        for (k = 0; k < nf && 8 + 20 * k + 20 <= (unsigned)n; k++) {
            unsigned int ct = OSReadBigInt32(h, 8 + 20 * k);
            if (ct == 18) ppc = 1;
            if (ct == 7 || ct == 0x01000007) x86 = 1;
        }
        return ppc && !x86;
    }
    return 0;
}
/* Shell scripts started by a PowerPC program (installers: Adobe CS2's checkHFS.sh calls its PowerPC
   HFSTest) run under Mojave's /bin/sh, which cannot start PowerPC tools. Run /bin/sh and /bin/bash,
   and scripts for them, with ROOT/native/bash (a copy of /bin/bash made by install.sh: copies of
   system binaries outside /bin honour DYLD_INSERT_LIBRARIES) and ROOT/native/libppcexec.dylib, whose
   execve / posix_spawn send PowerPC programs to rosettus. OAH_NO_SHELLFIX=1 disables. */
static void oah_exec_shell_fix(unsigned int *a, const char *path, char **argv, char **envp) {
    static char root[1024], nbash[1100], lib[1100], ins[1200], rootenv[1100], scr[1100], interp[256], iarg[256];
    static char *nargv[262], *nenv[1104];
    static int on = -1;
    const char *target = 0; char *opt = 0;
    int i, j, fd, n;
    struct stat st;
    if (on < 0) on = getenv("OAH_NO_SHELLFIX") ? 0 : 1;
    if (!on || !path) return;
    if (!root[0]) {
        Dl_info di; char *sl;
        if (!dladdr((void *)oah_is_ppc_file, &di) || !di.dli_fname) return;
        strlcpy(root, di.dli_fname, sizeof root);
        if (!(sl = strrchr(root, '/'))) return;
        *sl = 0;
        snprintf(nbash, sizeof nbash, "%s/native/bash", root);
        snprintf(lib, sizeof lib, "%s/native/libppcexec.dylib", root);
        snprintf(ins, sizeof ins, "DYLD_INSERT_LIBRARIES=%s", lib);
        snprintf(rootenv, sizeof rootenv, "OAH_ROSETTUS_ROOT=%s", root);
    }
    if (stat(nbash, &st) || stat(lib, &st)) return;
    if (!strcmp(path, "/bin/sh") || !strcmp(path, "/bin/bash")) target = 0;     /* the shell itself */
    else {                                                                       /* a script for it? */
        char h[300]; char *e;
        if ((fd = open(path, O_RDONLY)) < 0) return;
        n = (int)read(fd, h, sizeof h - 1); close(fd);
        if (n < 4 || h[0] != '#' || h[1] != '!') return;
        h[n] = 0; if ((e = strchr(h, '\n'))) *e = 0;
        for (e = h + 2; *e == ' ' || *e == '\t'; e++) ;
        strlcpy(interp, e, sizeof interp);
        if ((e = strpbrk(interp, " \t"))) { *e++ = 0; while (*e == ' ' || *e == '\t') e++; if (*e) { strlcpy(iarg, e, sizeof iarg); opt = iarg; } }
        if (strcmp(interp, "/bin/sh") && strcmp(interp, "/bin/bash")) return;
        strlcpy(scr, path, sizeof scr); target = scr;
    }
    j = 0;
    nargv[j++] = (char *)(target ? (strcmp(interp, "/bin/sh") ? "bash" : "sh") : (argv && argv[0] ? argv[0] : "sh"));
    if (target) { if (opt) nargv[j++] = opt; nargv[j++] = (char *)target; }
    for (i = 1; argv && argv[i] && j < 260; i++) nargv[j++] = argv[i];
    nargv[j] = 0;
    for (i = 0, j = 0; envp && envp[i] && j < 1100; i++) {
        if (!strncmp(envp[i], "DYLD_INSERT_LIBRARIES=", 22) || !strncmp(envp[i], "OAH_ROSETTUS_ROOT=", 18)) continue;
        nenv[j++] = envp[i];
    }
    nenv[j++] = ins; nenv[j++] = rootenv; nenv[j] = 0;
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[exec] shell %s%s%s -> %s\n", path, target ? " via " : "", target ? interp : "", nbash);
    a[0] = (unsigned int)nbash; a[1] = (unsigned int)nargv; a[2] = (unsigned int)nenv;
}
/* A native program started by a PowerPC one inherits the guest's DYLD_* settings (the PowerPC root's
   library paths, liboah): a native tool then failed in dyld ("Symbol not found: _environ") unless the
   system ignored them (SIP-protected /bin, /usr/bin). Drop them, keeping only the PowerPC-exec helper. */
static void oah_exec_native_env(unsigned int *a) {
    static char *nenv[1104];
    char **envp = (char **)a[2];
    int i, j, drop = 0;
    if (!envp || getenv("OAH_KEEP_DYLD_ENV")) return;
    for (i = 0; envp[i]; i++)
        if (!strncmp(envp[i], "DYLD_", 5) && !strstr(envp[i], "/native/libppcexec.dylib")) drop++;
    if (!drop) return;
    for (i = 0, j = 0; envp[i] && j < 1102; i++) {
        if (!strncmp(envp[i], "DYLD_", 5) && !strstr(envp[i], "/native/libppcexec.dylib")) continue;
        nenv[j++] = envp[i];
    }
    nenv[j] = 0;
    a[2] = (unsigned int)nenv;
}
/* /usr/sbin/installer started by a PowerPC program: run it through ROOT/native/installer-shim,
   which gives installd a copy of the package whose scripts can run PowerPC tools (CS2's package
   scripts call invokeSelfHeal, MakeAnAlias...; installd itself is outside Rosettus). */
static int oah_exec_installer_fix(unsigned int *a, char **argv) {
    static char shim[1100]; static char *nargv[262];
    struct stat st; int i, j;
    if (getenv("OAH_NO_PKGSHIM")) return 0;
    if (!shim[0]) {
        Dl_info di; char *sl;
        if (!dladdr((void *)oah_exec_installer_fix, &di) || !di.dli_fname) return 0;
        strlcpy(shim, di.dli_fname, sizeof shim);
        if (!(sl = strrchr(shim, '/'))) return 0;
        strlcpy(sl, "/native/installer-shim", sizeof shim - (sl - shim));
    }
    if (stat(shim, &st)) return 0;
    nargv[0] = "bash"; nargv[1] = shim;
    for (i = 1, j = 2; argv && argv[i] && j < 260; i++) nargv[j++] = argv[i];
    nargv[j] = 0;
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[exec] /usr/sbin/installer -> %s\n", shim);
    a[0] = (unsigned int)"/bin/bash"; a[1] = (unsigned int)nargv;
    return 1;
}
void oah_exec_fix(unsigned int *a) {
    static char root[1024], script[1100];
    static char *nargv[260], *nenv[1100];
    static char rp[1100];
    const char *path = (const char *)a[0];
    char **argv = (char **)a[1], **envp = (char **)a[2];
    int i, j;
    if (!path) return;
    path = translate_path_core(path, rp, sizeof rp);   /* guest /System -> PPC root */
    if (!strcmp(path, "/usr/sbin/installer") && oah_exec_installer_fix(a, argv)) { oah_exec_native_env(a); return; }
    if (!oah_is_ppc_file(path)) { oah_exec_shell_fix(a, path, argv, envp); oah_exec_native_env(a); return; }
    if (!root[0]) {
        Dl_info di; char *sl;
        if (!dladdr((void *)oah_is_ppc_file, &di) || !di.dli_fname) return;
        strlcpy(root, di.dli_fname, sizeof root);
        if (!(sl = strrchr(root, '/'))) return;
        *sl = 0;
        snprintf(script, sizeof script, "%s/rosettus", root);
    }
    nargv[0] = script; nargv[1] = (char *)path;
    for (i = 1, j = 2; argv && argv[i] && j < 258; i++) nargv[j++] = argv[i];
    nargv[j] = 0;
    for (i = 0, j = 0; envp && envp[i] && j < 1098; i++) {
        if (!strncmp(envp[i], "OAH_GUEST_EXE=", 14) || !strncmp(envp[i], "CFProcessPath=", 14)) continue;
        nenv[j++] = envp[i];
    }
    nenv[j] = 0;
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[exec] PowerPC %s -> %s\n", path, script);
    a[0] = (unsigned int)script; a[1] = (unsigned int)nargv; if (envp) a[2] = (unsigned int)nenv;
}

/* ---- guest kevent flag fix (always on) ----
   10.6-era guests (libdispatch-84) wake the dispatch manager with an EVFILT_USER kevent whose
   flags carry EV_TRIGGER (0x0100).  On 10.7+ kernels 0x0100 is EV_UDATA_SPECIFIC, so the change no
   longer matches the registered knote (ENOENT, swallowed by EV_RECEIPT): the manager queue is never
   drained and every dispatch timer / source registration / dispatch_after stalls.  The translator
   has already converted the changelist to native layout when the int $0x80 is reached; clear the
   bit there.  Disable: OAH_NO_KEVFIX=1. */
static int g_kevfix_n;
void oah_kevent_fix(unsigned int *a) {
    unsigned int k, n = a[2];
    unsigned char *c = (unsigned char *)a[1];
    if (!c || n > 4096) return;
    for (k = 0; k < n; k++, c += 20) {
        unsigned short *fl = (unsigned short *)(c + 6);
        if (*fl & 0x0100) { *fl &= (unsigned short)~0x0100; g_kevfix_n++; }
    }
}
/* ---- guest path redirects (always on) ----
   Some per-user caches have the same name on 10.6 and Mojave but different
   formats. The Snow Leopard font registry (fontd, genatsdb) keeps its SQLite
   databases in <user cache dir>/com.apple.FontRegistry, exactly where Mojave's
   own fontd keeps its newer (WAL-mode) ones. Send the guest's copy to a private
   folder so the two never touch each other's files. Every path-taking guest
   syscall is checked; the rewritten path lives in a per-thread buffer. */
static pthread_key_t g_pathkey; static pthread_once_t g_pathonce = PTHREAD_ONCE_INIT;
static char g_ppc_cache[512];
static char g_vol_syslib[64];   /* "/.vol/<dev>/<ino>" of the real /System/Library */
static char g_vol_lib[64];      /* ... and of /Library (for /Library/Fonts) */
static int g_libfonts_map = -1; /* guest tree has Library/Fonts (Snow Leopard's set) */
static void oah_path_init(void) {
    const char *h = getenv("HOME");
    struct stat sb;
    pthread_key_create(&g_pathkey, free);
    /* Carbon's File Manager turns FSRefs into volfs paths: the guest reaches
       /System/Library/Fonts as "/.vol/<dev>/<id of /System/Library>/Fonts", which
       bypasses the plain-path redirect below and showed fontd Mojave's own fonts. */
    if (stat("/System/Library", &sb) == 0)
        snprintf(g_vol_syslib, sizeof g_vol_syslib, "/.vol/%ld/%llu", (long)sb.st_dev, (unsigned long long)sb.st_ino);
    if (stat("/Library", &sb) == 0)
        snprintf(g_vol_lib, sizeof g_vol_lib, "/.vol/%ld/%llu", (long)sb.st_dev, (unsigned long long)sb.st_ino);
    snprintf(g_ppc_cache, sizeof g_ppc_cache, "%s/Library/Caches/com.rosettus.ppc", h && *h ? h : "/tmp");
    mkdir(g_ppc_cache, 0755);
}
static int oah_libfonts_map(void) {
    if (g_libfonts_map < 0) {
        char t[1100]; struct stat sb;
        snprintf(t, sizeof t, "%s/Library/Fonts", g_rosetta_root);
        g_libfonts_map = !getenv("OAH_NO_LIBFONTMAP") && g_rosetta_root[0] && !stat(t, &sb) && S_ISDIR(sb.st_mode);
    }
    return g_libfonts_map;
}
/* ---- icon files in the 10.7+ format ----
   Mojave's .icns files start with a 'TOC ' chunk and carry their large sizes as PNG ('ic07',
   'ic10'..'ic14') without the classic 128-pixel 'it32'. Snow Leopard's IconServices rejected
   them and drew the generic folder icon instead: every disk in a PowerPC app's open panel
   (IOStorageFamily's Internal.icns) showed as a folder. A converted copy is made once in the
   per-user cache: the classic entries are kept, the newer ones dropped, and a 128-pixel PNG
   becomes a classic it32/t8mk pair. OAH_NO_ICNSFIX=1 disables. */
static void oah_icns_put(unsigned char **b, size_t *n, size_t *cap, const void *d, size_t len) {
    if (*n + len > *cap) { *cap = (*n + len) * 2; *b = (unsigned char *)realloc(*b, *cap); }
    memcpy(*b + *n, d, len); *n += len;
}
/* one channel of 'it32' data, PackBits style: literal runs only (n-1, n bytes) */
static void oah_icns_rle(unsigned char **b, size_t *n, size_t *cap, const unsigned char *px, int count, int stride) {
    int i = 0;
    while (i < count) {
        unsigned char run[129];
        int k = count - i < 128 ? count - i : 128, j;
        run[0] = (unsigned char)(k - 1);
        for (j = 0; j < k; j++) run[1 + j] = px[(size_t)(i + j) * stride];
        oah_icns_put(b, n, cap, run, (size_t)k + 1);
        i += k;
    }
}
static int oah_icns_it32(const unsigned char *png, size_t len, unsigned char **b, size_t *n, size_t *cap) {
    /* CoreGraphics / ImageIO by name: liboah is not linked against them (it loads into every
       translated process, command-line tools too). i386: CGFloat is float. */
    typedef struct { float x, y, w, h; } oah_rectf;
    static void *(*srcmk)(CFDataRef, CFDictionaryRef);
    static void *(*imgat)(void *, size_t, CFDictionaryRef);
    static void *(*rgb)(void);
    static void *(*bmc)(void *, size_t, size_t, size_t, size_t, void *, unsigned int);
    static void (*draw)(void *, oah_rectf, void *);
    static void (*ctxrel)(void *), (*imgrel)(void *), (*csrel)(void *);
    CFDataRef d;
    void *src, *img, *cs, *ctx = NULL;
    unsigned char *px = NULL, hdr[8];
    size_t start, i;
    int ok = 0;
    if (!srcmk) {
        void *h = dlopen("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices", RTLD_LAZY | RTLD_GLOBAL);
        if (!h) return 0;
        *(void **)&imgat = dlsym(h, "CGImageSourceCreateImageAtIndex");
        *(void **)&rgb = dlsym(h, "CGColorSpaceCreateDeviceRGB");
        *(void **)&bmc = dlsym(h, "CGBitmapContextCreate");
        *(void **)&draw = dlsym(h, "CGContextDrawImage");
        *(void **)&ctxrel = dlsym(h, "CGContextRelease");
        *(void **)&imgrel = dlsym(h, "CGImageRelease");
        *(void **)&csrel = dlsym(h, "CGColorSpaceRelease");
        *(void **)&srcmk = dlsym(h, "CGImageSourceCreateWithData");
    }
    if (!srcmk || !imgat || !rgb || !bmc || !draw || !ctxrel || !imgrel || !csrel) return 0;
    d = CFDataCreate(NULL, png, (CFIndex)len);
    src = d ? srcmk(d, NULL) : NULL;
    img = src ? imgat(src, 0, NULL) : NULL;
    if (img && (px = (unsigned char *)calloc(128 * 128, 4)) && (cs = rgb())) {
        ctx = bmc(px, 128, 128, 8, 128 * 4, cs, 1 /* kCGImageAlphaPremultipliedLast */);
        csrel(cs);
    }
    if (ctx) {
        oah_rectf r = { 0, 0, 128, 128 };
        draw(ctx, r, img);
        for (i = 0; i < 128 * 128; i++) {               /* un-premultiply for the colour planes */
            unsigned int a = px[i * 4 + 3], c;
            if (a && a < 255) for (c = 0; c < 3; c++) px[i * 4 + c] = (unsigned char)((px[i * 4 + c] * 255 + a / 2) / a);
        }
        start = *n;
        memcpy(hdr, "it32", 4); oah_icns_put(b, n, cap, hdr, 8);
        memset(hdr, 0, 4); oah_icns_put(b, n, cap, hdr, 4);   /* it32 data starts with 4 zero bytes */
        for (i = 0; i < 3; i++) oah_icns_rle(b, n, cap, px + i, 128 * 128, 4);
        OSWriteBigInt32(*b, start + 4, (unsigned int)(*n - start));
        memcpy(hdr, "t8mk", 4); OSWriteBigInt32(hdr, 4, 8 + 128 * 128); oah_icns_put(b, n, cap, hdr, 8);
        for (i = 0; i < 128 * 128; i++) oah_icns_put(b, n, cap, px + i * 4 + 3, 1);
        ok = 1;
        ctxrel(ctx);
    }
    free(px);
    if (img) imgrel(img);
    if (src) CFRelease((CFTypeRef)src);
    if (d) CFRelease(d);
    return ok;
}
static const char *oah_icns_map(const char *p, char *out, size_t outsz) {
    static int off = -1;
    static const char *const keep[] = { "ICN#", "icl4", "icl8", "ics#", "ics4", "ics8", "is32", "s8mk",
                                        "il32", "l8mk", "ih32", "h8mk", "it32", "t8mk", "icm#", "icm4", "icm8", 0 };
    size_t L = strlen(p), n = 8, cap = 0, len, i;
    unsigned char head[16], *d = NULL, *b = NULL, *png128 = NULL;
    size_t png128len = 0;
    int fd, have_it32 = 0, k;
    struct stat sb;
    unsigned int h = 2166136261u;
    char dst[1100], tmp[1200];
    if (off < 0) off = getenv("OAH_NO_ICNSFIX") ? 1 : 0;
    static unsigned int neg[1024];                       /* paths seen to be classic icns files */
    if (off || L < 6 || strcmp(p + L - 5, ".icns") || !g_rosetta_root[0] ||
        !strncmp(p, g_rosetta_root, strlen(g_rosetta_root)) || !g_ppc_cache[0]) return 0;
    for (i = 0; i < L; i++) h = (h ^ (unsigned char)p[i]) * 16777619u;
    if (!h) h = 1;
    if (neg[h & 1023] == h) return 0;
    if ((fd = open(p, O_RDONLY)) < 0) return 0;
    if (fstat(fd, &sb) || sb.st_size < 16 || sb.st_size > 16 * 1024 * 1024 || read(fd, head, 16) != 16 ||
        memcmp(head, "icns", 4) || memcmp(head + 8, "TOC ", 4)) { close(fd); neg[h & 1023] = h; return 0; }
    snprintf(dst, sizeof dst, "%s/icns/%08x-%lx-%lx.icns", g_ppc_cache, h, (long)sb.st_mtime, (long)sb.st_size);
    if (!access(dst, R_OK)) { close(fd); snprintf(out, outsz, "%s", dst); return out; }
    d = (unsigned char *)malloc((size_t)sb.st_size);
    if (!d || pread(fd, d, (size_t)sb.st_size, 0) != sb.st_size) { close(fd); free(d); return 0; }
    close(fd);
    cap = (size_t)sb.st_size + 65536; b = (unsigned char *)malloc(cap);
    memcpy(b, "icns", 4);
    for (i = 8; i + 8 <= (size_t)sb.st_size; i += len) {
        len = OSReadBigInt32(d, i + 4);
        if (len < 8 || i + len > (size_t)sb.st_size) break;
        for (k = 0; keep[k] && memcmp(d + i, keep[k], 4); k++) ;
        if (keep[k]) { oah_icns_put(&b, &n, &cap, d + i, len); if (!memcmp(d + i, "it32", 4)) have_it32 = 1; }
        else if (!memcmp(d + i, "ic07", 4) && len > 16 && !memcmp(d + i + 8, "\x89PNG", 4)) { png128 = d + i + 8; png128len = len - 8; }
    }
    if (!have_it32 && png128) oah_icns_it32(png128, png128len, &b, &n, &cap);
    OSWriteBigInt32(b, 4, (unsigned int)n);
    snprintf(tmp, sizeof tmp, "%s/icns", g_ppc_cache); mkdir(tmp, 0700);
    snprintf(tmp, sizeof tmp, "%s.%d", dst, (int)getpid());
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
        int okw = write(fd, b, n) == (ssize_t)n;
        close(fd);
        if (!okw || rename(tmp, dst)) { unlink(tmp); free(b); free(d); return 0; }
    }
    free(b); free(d);
    if (access(dst, R_OK)) return 0;
    snprintf(out, outsz, "%s", dst);
    return out;
}
/* set once the guest opens a PowerPC JVM (libjvm.dylib): see oah_sysctl_fix */
static volatile int g_jvm_seen;
static const char *oah_guest_path_map(const char *p, char *out, size_t outsz) {
    static const char *const names[] = { "/com.apple.FontRegistry", 0 };
    const char *q; int i;
    if (!p) return 0;
    if (!g_jvm_seen) {   /* jvm.cfg names Libraries/libclient.dylib (libjvm.dylib links to it) */
        size_t L = strlen(p);
        if ((L >= 13 && !strcmp(p + L - 13, "/libjvm.dylib")) || (L >= 16 && !strcmp(p + L - 16, "/libclient.dylib") && strstr(p, "JavaVM.framework/")) ||
            (L >= 16 && !strcmp(p + L - 16, "/libserver.dylib") && strstr(p, "JavaVM.framework/")) ||
            (L >= 17 && !strcmp(p + L - 17, "/libhotspot.dylib") && strstr(p, "JavaVM.framework/"))) g_jvm_seen = 1;
    }
    if ((q = oah_icns_map(p, out, outsz))) return q;     /* 10.7+ icon files */
    /* system fonts: the guest sees Snow Leopard's own set (copied into the guest
       tree), matching its read-only font database; Mojave's newer fonts are not
       all readable by the 10.6 font code. /Library/Fonts and ~/Library/Fonts stay shared. */
    if (!strncmp(p, "/System/Library/Fonts", 21) && (p[21] == 0 || p[21] == '/')) {
        snprintf(out, outsz, "%s%s", g_rosetta_root, p);
        return out;
    }
    /* the guest fontd's per-user registry: its cache directory comes out empty under the translator,
       so it asked for "/fontregistry.user" (unwritable) and kept no user registry at all */
    if (!strncmp(p, "/fontregistry.user", 18)) {
        char d[600];
        snprintf(d, sizeof d, "%s/com.apple.FontRegistry", g_ppc_cache); mkdir(d, 0700);
        snprintf(out, outsz, "%s%s", d, p);
        return out;
    }
    /* X11 (exported from Snow Leopard when present): libraries, fontconfig, locale and xkb data for
       PowerPC X11 apps (GTK Wireshark, GIMP...). They talk to Mojave's XQuartz server over its socket. */
    if ((!strncmp(p, "/usr/X11/", 9) || !strncmp(p, "/usr/X11R6/", 11)) && !getenv("OAH_NO_X11MAP")) {
        struct stat xs; char t[1100];
        snprintf(t, sizeof t, "%s/usr/X11", g_rosetta_root);
        if (!stat(t, &xs)) { snprintf(out, outsz, "%s/usr/X11%s", g_rosetta_root, strchr(p + 5, '/')); return out; }
    }
    if (!strncmp(p, "/usr/libexec/oah/", 17)) {   /* Rosetta's guest shims (Interposers, BDL) live in the guest tree */
        snprintf(out, outsz, "%s%s", g_rosetta_root, p);
        return out;
    }
    /* Keyboard layouts: the guest (big-endian) reads AppleKeyboardLayouts-B.dat, which Mojave's bundle no
       longer has; with Mojave's directory listing the guest found no layouts at all, so the current
       keyboard's KCHR was NULL (GetScriptManagerVariable(smKCHRCache) = 0) and Office 2004 crashed at
       launch reading it. The whole folder (listings too) comes from the guest tree. */
    if (!strncmp(p, "/System/Library/Keyboard Layouts", 32) && (p[32] == 0 || p[32] == '/') && !getenv("OAH_NO_KBMAP")) {
        snprintf(out, outsz, "%s%s", g_rosetta_root, p);
        return out;
    }
    /* /Library/Fonts: Mojave moved many of Snow Leopard's local fonts (Marker Felt, Optima, Palatino, Bodoni,
       Snell Roundhand, ... - iWork theme fonts) out of it, and the 10.6 fontworker crashed parsing several of
       Mojave's newer files there (Arial, Georgia, ... never showed up in the Fonts panel). When the guest tree
       has a Library/Fonts (a copy of Snow Leopard's), the guest sees exactly that set. OAH_NO_LIBFONTMAP=1 shares
       Mojave's folder again. */
    if (!strncmp(p, "/Library/Fonts", 14) && (p[14] == 0 || p[14] == '/')) {
        if (oah_libfonts_map()) { snprintf(out, outsz, "%s%s", g_rosetta_root, p); return out; }
    }
    /* ATS.framework resources: guest CFBundle resolves the framework under the real
       /System path, so fontworker read Mojave's precomputed FontInfo .fontinfo files
       (newer flatten format: "don't how to resurrect type 261", then an uncaught
       exception).  Use the Snow Leopard copies in the guest tree. */
    if (!strncmp(p, "/System/Library/Frameworks/ApplicationServices.framework/", 57) &&
        (q = strstr(p + 56, "/ATS.framework")) && (q[14] == 0 || q[14] == '/')) {
        snprintf(out, outsz, "%s%s", g_rosetta_root, p);
        return out;
    }
    /* Any regular file under /System/Library/(Private)Frameworks that exists in the Snow Leopard guest
       tree is served from there.  dyld already loads guest frameworks from the tree (XYLD_FRAMEWORK_PATH),
       but CFM opens the system path itself: Internet Explorer's import of "JavaVM" found Mojave's
       Intel-only JavaVM binary and failed with cfragFragmentCorruptErr.  Resources then also match the
       binaries that read them.  Directories are left alone (their listings stay Mojave's).
       OAH_NO_SYSFWMAP=1 disables. */
    /* CoreServices stays Mojave's (Dock, Finder, LS paths), except its plug-in bundles: guest MLTE loads
       MLTEFile.bundle (Word 2004's text import: Mojave's is Intel-only, the load failed and MLTE's
       InsertFromFile then restored a CG state it never saved -> CGGStackRestore assertion). */
    if (!strncmp(p, "/System/Library/", 16) && (strncmp(p, "/System/Library/CoreServices/", 29) || strstr(p + 29, ".bundle/") ||
                                                (!strcmp(p + 29, "SystemVersion.plist") && !getenv("OAH_HOST_SYSVERSION")))) {
        /* SystemVersion.plist too: the guest is a 10.6.8 system (its frameworks are), and Gestalt('sysv')
           answered 0x1096 (Mojave capped) where Rosetta apps always saw 10.6.8 (0x1068). */   /* TextEncodings, Components, ... too */
        static struct { unsigned int h; signed char there; } fc[4096];
        static int off = -1;
        /* Guest-toolbox processes load their framework binaries from the gtb overlay first
           (DYLD_FRAMEWORK_PATH), but CFM opens a framework by its system path itself: Office 2004's
           Clip Gallery got the bridged QuickTime, whose movie calls go to native QuickTime (made for
           SimCity, which runs without the guest toolbox), and its PICT import failed ("An error
           occurred during conversion"). Such opens of a framework binary that has an overlay copy
           get the overlay copy. OAH_NO_GTBCFMMAP=1 disables. */
        if (!strncmp(p, "/System/Library/Frameworks/", 27) && getenv("OAH_GTB") && g_rosetta_root[0] && !getenv("OAH_NO_GTBCFMMAP")) {
            const char *fw = p + 27, *dot = strstr(fw, ".framework/");
            if (dot && dot - fw < 64) {
                char nm[64], t[1300]; struct stat sb;
                const char *rest = dot + 11;
                size_t L = (size_t)(dot - fw);
                memcpy(nm, fw, L); nm[L] = 0;
                if (!strcmp(rest, nm) || (!strncmp(rest, "Versions/", 9) && strchr(rest + 9, '/') && !strcmp(strchr(rest + 9, '/') + 1, nm))) {
                    snprintf(t, sizeof t, "%s/gtb/Frameworks/%s.framework/Versions/A/%s", g_rosetta_root, nm, nm);
                    if (!stat(t, &sb) && S_ISREG(sb.st_mode)) { snprintf(out, outsz, "%s", t); return out; }
                }
            }
        }
        unsigned int h = 2166136261u; const char *c; int slot; struct stat sb;
        if (off < 0) off = getenv("OAH_NO_SYSFWMAP") != 0;
        if (!off && g_rosetta_root[0]) {
            for (c = p; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
            if (!h) h = 1;
            slot = (int)(h & 4095);
            if (fc[slot].h != h) {
                char t[1300];
                snprintf(t, sizeof t, "%s%s", g_rosetta_root, p);
                fc[slot].there = (char)(!stat(t, &sb) && S_ISREG(sb.st_mode));
                /* a bundle's Resources directory (and the folders in it: .lproj, .nib) is listed from
                   the guest tree too: CFBundle finds resources by listing them, and Mojave's
                   PrintCocoaUI.bundle has no English.lproj nibs (only Base.lproj), so the 10.6 print
                   dialog failed with "failed to load window nib file 'CocoaPrintWindow'" (-30870).
                   OAH_NO_RESDIRMAP=1 keeps Mojave's listings. */
                if (!fc[slot].there && !stat(t, &sb) && S_ISDIR(sb.st_mode) && !getenv("OAH_NO_RESDIRMAP")) {
                    const char *rs = strstr(p, "/Resources");
                    /* .service too: Snow Leopard's AppleSpell (the guest's spell server) listed
                       Mojave's AppleSpell.service/Contents/Resources and loaded no dictionary */
                    if (rs && (rs[10] == 0 || rs[10] == '/') && (strstr(p, ".bundle/") || strstr(p, ".framework/") || strstr(p, ".service/")))
                        fc[slot].there = 2;
                }
                fc[slot].h = h;
            }
            if (fc[slot].there) { snprintf(out, outsz, "%s%s", g_rosetta_root, p); return out; }
        }
    }
    /* CarbonCore's _CSCheckFix reads the per-app compatibility-fix list (standalone path:
       coreservicesd isn't asked). Mojave's checkfixlist is a newer build; give the guest
       Snow Leopard's when the guest tree has it. */
    if (!strcmp(p, "/System/Library/CoreServices/checkfixlist")) {
        struct stat sb; char t[1300];
        snprintf(t, sizeof t, "%s%s", g_rosetta_root, p);
        if (!stat(t, &sb)) { snprintf(out, outsz, "%s", t); return out; }
    }
    /* QuickTime components: Mojave's QuickTimeComponents/Importers are Intel-only ("wrong
       architecture" for every component the guest QuickTime registers); use the Snow
       Leopard PowerPC copies in the guest tree. */
    /* Folders Mojave no longer has, listed by the guest: /System/Library/ColorPickers (Mojave keeps
       its pickers inside AppKit) - the 10.6 color panel found no picker bundles and opened as an
       empty window with no toolbar (Pages' Colors button, Format > Font > Show Colors).
       CFMSupport likewise. Served whole from the guest tree. */
    if ((!strncmp(p, "/System/Library/ColorPickers", 28) && (p[28] == 0 || p[28] == '/')) ||
        (!strncmp(p, "/System/Library/CFMSupport", 26) && (p[26] == 0 || p[26] == '/'))) {
        if (g_rosetta_root[0]) { snprintf(out, outsz, "%s%s", g_rosetta_root, p); return out; }
    }
    if (!strncmp(p, "/System/Library/QuickTime/", 26) ||
        /* the folder itself as well: it was listed from Mojave, so a component added to the guest
           tree (RosettusFlash.component, the Flash importer) was never registered */
        !strcmp(p, "/System/Library/QuickTime")) {
        snprintf(out, outsz, "%s%s", g_rosetta_root, p);
        return out;
    }
    /* Keyboard layouts: Mojave's AppleKeyboardLayouts.bundle only has the little-endian
       AppleKeyboardLayouts-L.dat; the PowerPC TIS code reads -B.dat, found nothing, and
       TISGetInputSourceProperty returned NULL (crash in TSMKeyTranslate on activation).
       Use Snow Leopard's bundle in the guest tree. */
    if (!strncmp(p, "/System/Library/Keyboard Layouts", 32) && (p[32] == 0 || p[32] == '/')) {
        snprintf(out, outsz, "%s%s", g_rosetta_root, p);
        return out;
    }
    /* ICU data: the guest libicucore (ICU 4.0) wants icudt40b.dat; Mojave only has
       icudt62l.  Without it CFLocaleCopyAvailableLocaleIdentifiers is empty and the
       font worker crashes building its language set. */
    if (!strncmp(p, "/usr/share/icu/icudt40b", 23)) {
        snprintf(out, outsz, "%s%s", g_rosetta_root, p);
        return out;
    }
    /* Language identification data: Mojave's langid.inv is a newer format; the 10.6 liblangid
       (AppKit's spelling and grammar checker on a background thread) aborted reading it (Evernote
       ended at start). Use Snow Leopard's copy in the guest tree when the export has it. */
    if (!strncmp(p, "/usr/share/langid/", 18)) {
        struct stat ls; char t[1100];
        snprintf(t, sizeof t, "%s%s", g_rosetta_root, p);
        if (!stat(t, &ls)) { snprintf(out, outsz, "%s", t); return out; }
    }
    /* Per-user data that Mojave guards with privacy consent (Contacts, Calendars): the 10.6
       AddressBook framework (loaded by iWork, Mail clients, ...) must not touch the modern
       databases, and touching them raises a TCC prompt the guest cannot answer (the syscall
       blocks).  Give the guest its own private copy under the Rosetta cache folder. */
    {
        static char home[512]; static int hl = -1;
        static const char *const prot[] = { "/Library/Application Support/AddressBook",
                                            "/Library/Preferences/com.apple.AddressBook",
                                            "/Library/Calendars", 0 };
        if (hl < 0) { const char *h = getenv("HOME"); snprintf(home, sizeof home, "%s", h ? h : ""); hl = (int)strlen(home); }
        if (hl > 0 && !strncmp(p, home, hl) && p[hl] == '/') {
            for (i = 0; prot[i]; i++) {
                size_t pl = strlen(prot[i]);
                if (!strncmp(p + hl, prot[i], pl)) {
                    char d[1300], *sl;
                    snprintf(out, outsz, "%s/home%s", g_ppc_cache, p + hl);
                    snprintf(d, sizeof d, "%s", out);           /* make sure the parent exists */
                    if ((sl = strrchr(d, '/'))) {
                        char *c;
                        *sl = 0;
                        for (c = d + strlen(g_ppc_cache) + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(d, 0755); *c = '/'; }
                        mkdir(d, 0755);
                    }
                    return out;
                }
            }
        }
    }
    /* /Library overlay: PowerPC support files that cannot be installed into Mojave's
       /Library (not writable for us, and they would sit beside Intel ones), e.g.
       "/Library/Application Support/iWork '09/Frameworks", live in <root>/Library and win
       for the whole third-level directory ("/Library/A/B") when it exists there. */
    if (!strncmp(p, "/Library/", 9)) {
        static struct { char dir[256]; int len, there; } cache[32];
        static int ncache;
        const char *s1 = strchr(p + 9, '/'), *s2 = s1 ? strchr(s1 + 1, '/') : 0;
        int len = s2 ? (int)(s2 - p) : (int)strlen(p), there = -1;
        if (s1 && len < 256) {
            for (i = 0; i < ncache; i++)
                if (cache[i].len == len && !strncmp(cache[i].dir, p, len)) { there = cache[i].there; break; }
            if (there < 0) {
                char t[1300]; struct stat sb;
                snprintf(t, sizeof t, "%s%.*s", g_rosetta_root, len, p);
                there = !stat(t, &sb);
                if (ncache < 32) { memcpy(cache[ncache].dir, p, len); cache[ncache].dir[len] = 0; cache[ncache].len = len; cache[ncache++].there = there; }
            }
            if (there) { snprintf(out, outsz, "%s%s", g_rosetta_root, p); return out; }
        }
        return 0;
    }
    if (g_vol_syslib[0] && !strncmp(p, "/.vol/", 6)) {
        size_t n = strlen(g_vol_syslib);
        if (!strncmp(p, g_vol_syslib, n) && !strncmp(p + n, "/Fonts", 6) && (p[n + 6] == 0 || p[n + 6] == '/')) {
            snprintf(out, outsz, "%s/System/Library%s", g_rosetta_root, p + n);
            return out;
        }
        n = strlen(g_vol_lib);
        if (g_vol_lib[0] && oah_libfonts_map() && !strncmp(p, g_vol_lib, n) && !strncmp(p + n, "/Fonts", 6) && (p[n + 6] == 0 || p[n + 6] == '/')) {
            snprintf(out, outsz, "%s/Library%s", g_rosetta_root, p + n);
            return out;
        }
        return 0;
    }
    if (strncmp(p, "/var/folders/", 13) && strncmp(p, "/private/var/folders/", 21)) return 0;
    for (i = 0; names[i]; i++)
        if ((q = strstr(p, names[i])) && (q[strlen(names[i])] == 0 || q[strlen(names[i])] == '/')) {
            snprintf(out, outsz, "%s%s", g_ppc_cache, q);
            return out;
        }
    return 0;
}
static char *oah_path_buf(int which) {
    char *b = (char *)pthread_getspecific(g_pathkey);
    if (!b) { b = (char *)malloc(2 * 1100); if (!b) return 0; pthread_setspecific(g_pathkey, b); }
    return b + which * 1100;
}
static void oah_gal_rootfix(unsigned int *a);
static int oah_gal_volfix(unsigned int *a);
static int oah_statfs_apfs(unsigned int num, unsigned int *a);
/* ---- HFS+ directory order for guest readdir ----
   HFS+ returned directory entries sorted by name (catalog order: ".", "..", then a
   case-insensitive Unicode compare); APFS returns them in hash order. Classic-era code shows
   that order directly (AppleWorks' Starting Points tabs, indexed PBGetCatInfo, Open dialogs
   without sorting). On the first getdirentries64 (344) for a directory fd at offset 0 read the
   whole directory natively, sort it, and serve the records from the cache; the trap is skipped
   (oah_path_fix returns 1, a[-3] = byte count). d_seekoff is the entry index, so telldir /
   seekdir (lseek to that index) keep working. OAH_NO_DIRSORT=1 disables. */
int g_sc_skip_ok;
struct oah_dcache { dev_t dev; ino_t ino; off_t endpos; unsigned int n, next; char **rec; };
static struct oah_dcache *g_dc[1024];
static pthread_mutex_t g_dc_mu = PTHREAD_MUTEX_INITIALIZER;
static CFStringRef oah_dc_name(const char *r) {
    unsigned short nl = *(unsigned short *)(r + 18);
    return CFStringCreateWithBytes(0, (const UInt8 *)r + 21, nl, kCFStringEncodingUTF8, 0);
}
static int oah_dc_dot(const char *r) {
    unsigned short nl = *(unsigned short *)(r + 18);
    return (nl == 1 && r[21] == '.') ? 1 : (nl == 2 && r[21] == '.' && r[22] == '.') ? 2 : 0;
}
static int oah_dc_cmp(const void *x, const void *y) {
    const char *a = *(const char * const *)x, *b = *(const char * const *)y;
    int da = oah_dc_dot(a), db = oah_dc_dot(b); CFStringRef sa, sb; int c;
    if (da || db) return (da ? da : 3) - (db ? db : 3);
    sa = oah_dc_name(a); sb = oah_dc_name(b);
    if (!sa || !sb) c = strcmp(a + 21, b + 21);
    else if (!(c = (int)CFStringCompare(sa, sb, kCFCompareCaseInsensitive | kCFCompareNonliteral)))
        c = (int)CFStringCompare(sa, sb, 0);
    if (sa) CFRelease(sa); if (sb) CFRelease(sb);
    return c;
}
static void oah_dc_free(struct oah_dcache *d) {
    unsigned int i; if (!d) return;
    for (i = 0; i < d->n; i++) free(d->rec[i]);
    free(d->rec); free(d);
}
static struct oah_dcache *oah_dc_build(int fd, struct stat *st) {
    struct oah_dcache *d = (struct oah_dcache *)calloc(1, sizeof *d);
    char *buf = (char *)malloc(65536); unsigned int cap = 64; long long base; int got;
    if (!d || !buf) { free(d); free(buf); return 0; }
    d->dev = st->st_dev; d->ino = st->st_ino; d->rec = (char **)malloc(cap * sizeof(char *));
    while ((got = syscall(344, fd, buf, 65536, &base)) > 0) {
        int o = 0;
        while (o + 21 < got) {
            unsigned short rl = *(unsigned short *)(buf + o + 16);
            if (rl < 22 || o + rl > got) break;
            if (d->n == cap) { cap *= 2; d->rec = (char **)realloc(d->rec, cap * sizeof(char *)); }
            d->rec[d->n] = (char *)malloc(rl); memcpy(d->rec[d->n], buf + o, rl); d->n++;
            o += rl;
        }
    }
    free(buf);
    if (got < 0) { oah_dc_free(d); return 0; }
    qsort(d->rec, d->n, sizeof(char *), oah_dc_cmp);
    d->endpos = lseek(fd, 0, SEEK_CUR);
    if (d->endpos <= 0) { oah_dc_free(d); return 0; }
    return d;
}
/* a[] = fd, buf, size, basep (native). Returns 1 when the call was served (a[-3] = result). */
static int oah_dirsort(unsigned int *a) {
    static int on = -1;
    int fd = (int)a[0]; char *out = (char *)a[1]; unsigned int sz = a[2], o = 0;
    struct oah_dcache *d; struct stat st; off_t pos;
    if (on < 0) on = getenv("OAH_NO_DIRSORT") ? 0 : 1;
    if (!on || !g_sc_skip_ok || fd < 0 || fd >= 1024 || !out || sz < 64) return 0;
    if (fstat(fd, &st) || !S_ISDIR(st.st_mode)) return 0;
    pthread_mutex_lock(&g_dc_mu);
    pos = lseek(fd, 0, SEEK_CUR);
    d = g_dc[fd];
    if (d && (d->dev != st.st_dev || d->ino != st.st_ino)) { oah_dc_free(d); d = g_dc[fd] = 0; }
    if (pos == 0) {   /* fresh open or rewinddir */
        if (d) { oah_dc_free(d); d = g_dc[fd] = 0; }
        if (!(d = g_dc[fd] = oah_dc_build(fd, &st))) { lseek(fd, 0, SEEK_SET); pthread_mutex_unlock(&g_dc_mu); return 0; }
    } else if (!d) { pthread_mutex_unlock(&g_dc_mu); return 0; }   /* opened before we saw it: pass through */
    else if (pos != d->endpos) {   /* seekdir to one of our indices */
        d->next = pos > 0 && pos <= d->n ? (unsigned int)pos : d->n;
        lseek(fd, d->endpos, SEEK_SET);
    }
    if (a[3]) *(long long *)a[3] = d->next;   /* basep = position of this block's start (libc telldir) */
    while (d->next < d->n) {
        char *r = d->rec[d->next]; unsigned short rl = *(unsigned short *)(r + 16);
        if (o + rl > sz) break;
        memcpy(out + o, r, rl);
        *(unsigned long long *)(out + o + 8) = d->next + 1;   /* d_seekoff */
        o += rl; d->next++;
    }
    pthread_mutex_unlock(&g_dc_mu);
    ((unsigned int *)a)[-3] = o;
    return 1;
}
/* sysctl {CTL_KERN, KERN_CLASSIC (44), pid}: 10.6's kernel answered 1 for a translated process. The guest
   dyld asks it (isRosetta()) and only then loads /usr/libexec/oah/Shims/Interposers.dylib, which hands
   libm and other hot libSystem routines to the native i386 code (BDL); without it every sin() ran as
   translated PowerPC code (slower, and wrong for huge arguments: sin(1e300)). Apps also used it to tell
   they ran under Rosetta. OAH_NO_KERNCLASSIC=1 answers "no" as before. */
static int oah_sysctl_fix(unsigned int *a) {
    static int on = -1;
    unsigned int *mib = (unsigned int *)a[0], n = a[1], m0, m1;
    if (on < 0) on = getenv("OAH_NO_KERNCLASSIC") ? 0 : 1;
    {   static int lg = -1, nl; if (lg < 0) lg = getenv("OAH_SYSCTLLOG") ? 1 : 0;   /* debug: the first 200 calls */
        if (lg && mib && n >= 1 && n <= 12 && nl++ < 200) {
            unsigned int k2; fprintf(stderr, "[sysctl] mib");
            for (k2 = 0; k2 < n; k2++) fprintf(stderr, " %d", (int)mib[k2]);
            fprintf(stderr, " old %08x len %08x new %08x\n", a[2], a[3], a[4]);
        } }
    if (!on || !g_sc_skip_ok || !mib || n < 2 || n > 12) return 0;
    m0 = mib[0]; m1 = mib[1];
    /* hw.usermem: a 32-bit int for this guest. With 16 GB, Mojave's value wrapped to a negative
       number and Photoshop CS2 stopped in a static initializer with memFullErr (-108), before any
       window. 10.6 on a 4 GB Mac gave about 1.7 GB; answer at most 2 GB less a page. */
    if (((m0 == 6 && m1 == 6) || (m0 == 0x06000000 && m1 == 0x06000000)) && a[2] && a[3] && !getenv("OAH_NO_USERMEMFIX")) {
        unsigned long long um = 0; size_t l = sizeof um; unsigned int v;
        if (sysctlbyname("hw.usermem", &um, &l, 0, 0)) return 0;
        if (l == 4) um = *(unsigned int *)&um;
        if (um <= 0x7ffff000ULL) return 0;              /* fits: the kernel's own answer */
        v = 0x7ffff000u;
        if (m0 != 6) v = OSSwapInt32(v);
        memcpy((void *)a[2], &v, 4);
        {   unsigned int four = (m0 == 6) ? 4u : 0x04000000u; memcpy((void *)a[3], &four, 4); }
        ((unsigned int *)a)[-3] = 0;
        return 1;
    }
    /* OAH_MAXCPU=N: the processor count a PowerPC program sees, at most N (off by default). hw.ncpu,
       hw.availcpu and the OID_AUTO names (hw.activecpu, physicalcpu, logicalcpu and their _max), whose
       numbers sysctlbyname looks up first; the same kernel gives us the same numbers. The translator
       answers the guest's hw.* queries from what it asked at start-up, through here too. (Tried for
       QuickTime's H.264 decoder, whose real problem was thread_info: see oah_fix_thrinfo.) */
    if (n == 2 && a[2] && a[3]) {
        static int cap = -1, autos[6];
        unsigned int k1 = (m0 == 6) ? m1 : OSSwapInt32(m1);
        int hit = 0, k;
        if (cap < 0) {
            static const char *nm[6] = { "hw.activecpu", "hw.physicalcpu", "hw.logicalcpu", "hw.physicalcpu_max", "hw.logicalcpu_max", "hw.availcpu" };
            const char *v = getenv("OAH_MAXCPU");
            for (k = 0; k < 6; k++) { int mb[4]; size_t ml = 4; autos[k] = (!sysctlnametomib(nm[k], mb, &ml) && ml == 2 && mb[0] == 6) ? mb[1] : -1; }
            cap = v ? atoi(v) : 0;
        }
        if (cap > 0 && (m0 == 6 || m0 == 0x06000000)) {
            hit = k1 == 3 || k1 == 25;
            for (k = 0; k < 6 && !hit; k++) hit = autos[k] >= 0 && (unsigned int)autos[k] == k1;
        }
        if (hit) {
            int mb[2] = { 6, (int)k1 }, val = 0; size_t l = sizeof val; unsigned int glen;
            memcpy(&glen, (void *)a[3], 4); if (m0 != 6) glen = OSSwapInt32(glen);
            if (glen >= 4 && !sysctl(mb, 2, &val, &l, 0, 0) && l == 4) {
                unsigned int v, four = (m0 == 6) ? 4u : 0x04000000u;
                if (val > cap) val = cap;
                v = (m0 == 6) ? (unsigned int)val : OSSwapInt32((unsigned int)val);
                memcpy((void *)a[2], &v, 4);
                memcpy((void *)a[3], &four, 4);
                ((unsigned int *)a)[-3] = 0;
                return 1;
            }
        }
    }
    if (!((m0 == 1 && m1 == 44) || (m0 == 0x01000000 && m1 == 0x2c000000))) return 0;
    /* Apple's PowerPC JVM (Java 5 for 10.4/10.5) asks the same question and refuses to run translated
       ("Java for ppc cannot run in this configuration"): once a JVM is loaded, answer "native".
       dyld asked long before (process start), so the interposers are loaded. OAH_NO_JAVAFIX=1 disables. */
    if (g_jvm_seen && !getenv("OAH_NO_JAVAFIX")) {
        if (a[2]) memset((void *)a[2], 0, 4);
        if (a[3]) { unsigned int four = (m0 == 1) ? 4u : 0x04000000u; memcpy((void *)a[3], &four, 4); }
        ((unsigned int *)a)[-3] = 0;
        return 1;
    }
    if (a[2]) { unsigned int one = (m0 == 1) ? 1u : 0x01000000u; memcpy((void *)a[2], &one, 4); }
    if (a[3]) { unsigned int four = (m0 == 1) ? 4u : 0x04000000u; memcpy((void *)a[3], &four, 4); }
    ((unsigned int *)a)[-3] = 0;
    return 1;
}
/* sigaction(SIGSEGV / SIGBUS, handler) by the translator, when the guest installs handlers of its own
   (NSExceptionHandler: Evernote): it replaced segv_handler, which fixes the translator's harmless
   faults (old-commpage loads, relocations), and each of those went to the guest as a crash (Evernote:
   "Uncaught system exception: signal 11" / 5, a dispatch queue left blocked, no window). Keep
   segv_handler; it passes the faults it cannot fix to the handler recorded here.
   OAH_NO_SEGVKEEP=1 disables. */
struct sigaction g_xl_segv_act[2]; volatile int g_xl_segv_set[2];
static int oah_segv_keep(unsigned int *a) {
    static int on = -1;
    unsigned int *na, k;
    if (on < 0) on = getenv("OAH_NO_SEGVKEEP") ? 0 : 1;
    if (!on || !g_sc_skip_ok || (a[0] != SIGSEGV && a[0] != SIGBUS)) return 0;
    k = a[0] == SIGBUS;
    if (a[2]) {                                       /* old action (native struct sigaction) */
        struct sigaction *o = (struct sigaction *)a[2];
        if (g_xl_segv_set[k]) *o = g_xl_segv_act[k];
        else { memset(o, 0, sizeof *o); o->sa_handler = SIG_DFL; }
    }
    if (a[1]) {                                       /* new action: __sigaction {handler, tramp, mask, flags} */
        na = (unsigned int *)a[1];
        if (na[0] <= 1) g_xl_segv_set[k] = 0;
        else {
            memset(&g_xl_segv_act[k], 0, sizeof g_xl_segv_act[k]);
            g_xl_segv_act[k].sa_handler = (void (*)(int))(uintptr_t)na[0];
            g_xl_segv_act[k].sa_mask = na[2]; g_xl_segv_act[k].sa_flags = (int)na[3];
            g_xl_segv_set[k] = 1;
            { static int n; if (n++ < 2) fprintf(stderr, "[sig] translator's %s handler %08x (tramp %08x flags %x) kept behind segv_handler\n",
                                                  k ? "SIGBUS" : "SIGSEGV", na[0], na[1], na[3]); }
        }
    }
    ((unsigned int *)a)[-3] = 0;
    return 1;
}
/* sigaction(SIGTRAP, SIG_DFL / SIG_IGN): the translator runs guest traps as SIGTRAP, so a guest that
   resets every signal (10.6 NSTask does in its forked child, before posix_spawn(SETEXEC)) killed itself
   with the next trap: every NSTask exited with status 5 (Adobe CS2's installer scripts "failed").
   Keep the translator's handler; the guest is told the change was made. OAH_NO_SIGTRAPKEEP=1 disables. */
static int oah_sigtrap_keep(unsigned int *a) {
    static int on = -1, said;
    unsigned int h;
    if (on < 0) on = getenv("OAH_NO_SIGTRAPKEEP") ? 0 : 1;
    if (!on || !g_sc_skip_ok || a[0] != SIGTRAP || !a[1]) return 0;
    h = *(unsigned int *)a[1];                       /* __sigaction.sa_handler (native layout by now) */
    if (h > 1) return 0;                              /* a real handler: the translator deals with it */
    if (a[2]) memset((void *)a[2], 0, 12);            /* old action: SIG_DFL */
    if (!said++ && getenv("OAH_VERBOSE")) fprintf(stderr, "[sig] guest reset of SIGTRAP kept for the translator\n");
    ((unsigned int *)a)[-3] = 0;
    return 1;
}
/* posix_spawn from the guest: 10.6's argument block (6 words) and attribute block (40 bytes) are not
   what Mojave's kernel reads (it takes more fields and checks the attribute size), so every guest
   posix_spawn failed with EINVAL. NSTask in 10.6 forks and then calls posix_spawn(POSIX_SPAWN_SETEXEC),
   so every NSTask "exited with status 5". Rebuild the call with libc's posix_spawn: flags, signal
   sets, process group, the open/close/dup2 file actions; PowerPC targets go through rosettus as for
   execve. OAH_NO_SPAWNFIX=1 disables. */
void oah_exec_fix(unsigned int *a);
static int oah_spawn_fix(unsigned int *a) {
    static int on = -1;
    unsigned int *d = (unsigned int *)a[2];
    posix_spawnattr_t at; posix_spawn_file_actions_t fa; int have_fa = 0, err; pid_t pid = 0;
    unsigned int e[3];
    if (on < 0) on = getenv("OAH_NO_SPAWNFIX") ? 0 : 1;
    if (!on || !g_sc_skip_ok || !a[1]) return 0;
    {   /* Bare Bones' crash-catcher (TextWrangler, BBEdit) takes over the parent's Mach exception
           ports, which the translator needs for its own traps: the app died at once. Before the
           spawn fix it simply failed to start; keep it that way. OAH_ALLOW_CRASHCATCHER=1 runs it. */
        const char *pp = (const char *)a[1], *bn = strrchr(pp, '/');
        if (bn && !strcmp(bn, "/crash-catcher") && !getenv("OAH_ALLOW_CRASHCATCHER")) {
            if (getenv("OAH_SPAWNTRACE") || getenv("OAH_VERBOSE")) fprintf(stderr, "[spawn] %s: not started (crash reporter)\n", pp);
            ((unsigned int *)a)[-3] = EINVAL;
            return 2;
        }
    }
    posix_spawnattr_init(&at);
    if (d && d[1] && d[0] >= 16) {
        unsigned char *ga = (unsigned char *)d[1];
        short fl; sigset_t sd, sm; pid_t pg;
        memcpy(&fl, ga, 2); memcpy(&sd, ga + 4, 4); memcpy(&sm, ga + 8, 4); memcpy(&pg, ga + 12, 4);
        posix_spawnattr_setflags(&at, fl & 0x00ff);       /* RESETIDS SETPGROUP SETSIGDEF SETSIGMASK SETEXEC START_SUSPENDED */
        posix_spawnattr_setsigdefault(&at, &sd);
        posix_spawnattr_setsigmask(&at, &sm);
        if (fl & POSIX_SPAWN_SETPGROUP) posix_spawnattr_setpgroup(&at, pg);
    }
    if (d && d[3] && d[2] >= 8) {                         /* 10.6 _posix_spawn_file_actions: alloc, count, actions */
        unsigned char *gf = (unsigned char *)d[3];
        int cnt, i; memcpy(&cnt, gf + 4, 4);
        posix_spawn_file_actions_init(&fa); have_fa = 1;
        for (i = 0; i < cnt && i < 64; i++) {
            unsigned char *x = gf + 8 + (size_t)i * (16 + PATH_MAX);
            int type, fd, ofl; mode_t mode;
            memcpy(&type, x, 4); memcpy(&fd, x + 4, 4); memcpy(&ofl, x + 8, 4); memcpy(&mode, x + 12, 2);
            if (type == 0) posix_spawn_file_actions_addopen(&fa, fd, (const char *)(x + 16), ofl, mode);
            else if (type == 1) posix_spawn_file_actions_addclose(&fa, fd);
            else if (type == 2) posix_spawn_file_actions_adddup2(&fa, fd, ofl);
        }
    }
    e[0] = a[1]; e[1] = a[3]; e[2] = a[4];
    oah_exec_fix(e);                                      /* a PowerPC program: run it through rosettus */
    err = posix_spawn(&pid, (const char *)e[0], have_fa ? &fa : 0, &at, (char *const *)e[1], (char *const *)e[2]);
    if (getenv("OAH_SPAWNTRACE")) fprintf(stderr, "[spawn] %s -> %d pid %d\n", (const char *)e[0], err, (int)pid);
    if (have_fa) posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (a[0]) *(pid_t *)a[0] = pid;
    /* posix_spawn(NULL, ...): after a successful call the translator copies the pid from its own
       buffer to the guest's pid pointer with no NULL check, and crashed at address 0 (TextWrangler
       starting its crash reporter). Find the guest registers (r4 = path, r6 = argv) and, when the
       guest passed no pid pointer, return through the error path with errno 0: the 10.6 libc
       wrapper then returns 0, and nothing is copied. */
    if (!err && !getenv("OAH_SPAWN_NONULLFIX")) {
        unsigned int *p = a, k, v, r4, r6, r3;
        for (k = 0; k < 1024; k++) {
            if (!probe_read((unsigned int)(p + k), &v) || v < 0x10000 || (v & 3)) continue;
            if (!probe_read(v + 16, &r4) || r4 != a[1] || !probe_read(v + 24, &r6)) continue;
            if (!oah_hang_isR(v) || !probe_read(v + 12, &r3)) continue;
            if (getenv("OAH_SPAWNTRACE")) fprintf(stderr, "[spawn] guest R %08x r3(pid ptr) %08x\n", v, r3);
            if (!r3) { ((unsigned int *)a)[-3] = 0; return 2; }
            break;
        }
    }
    if (!err && getenv("OAH_SPAWNTRACE")) fprintf(stderr, "[spawn] pid ptr %p\n", (void *)a[0]);
    ((unsigned int *)a)[-3] = (unsigned int)err;
    return 1;
}
/* Office 2004 keeps its shared state (ShMem.bundle) in POSIX named objects whose names end in the
   security session id: semaphores ShMemIntCrit<asid> / ShMemExtCrit<asid> and the shared memory
   PT_ShMemRef<asid>. They outlive the processes, so after an Office app was killed or crashed the
   next one in the same login session found stale state and stopped with "Microsoft Word cannot load
   the Microsoft Office shared libraries" (a Terminal-free launch from another session worked because
   its id differed). On the first such open, when no other Office 2004 process of this user is
   running, the stale objects are removed so ShMem starts fresh, as it would after a logout. */
static int oah_office_running_other(void) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_UID, (int)getuid() }, n, i, found = 0;
    size_t len = 0;
    struct kinfo_proc *kp;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) || !(kp = malloc(len + 4096))) return 1;
    len += 4096;
    if (sysctl(mib, 4, kp, &len, NULL, 0)) { free(kp); return 1; }
    n = (int)(len / sizeof *kp);
    for (i = 0; i < n && !found; i++) {
        int m2[3] = { CTL_KERN, KERN_PROCARGS2, kp[i].kp_proc.p_pid };
        char buf[8192]; size_t bl = sizeof buf, k;
        if (kp[i].kp_proc.p_pid == getpid()) continue;
        if (sysctl(m2, 3, buf, &bl, NULL, 0) || bl < 8) continue;
        for (k = 4; k + 22 < bl; k++)
            if (buf[k] == '/' && !memcmp(buf + k, "/Microsoft Office 2004/", 23)) { found = 1; break; }
    }
    free(kp);
    return found;
}
/* Office 2004 saves its OLE registry by exchanging a temporary file with "OLE Registration
   Database 11" (FSpExchangeFiles). Without help that exchange always fails on APFS, so the
   database a first run creates keeps only its first 12 KB: no VBA type libraries, and VBA said
   "Could not open macro storage" for good, because Office rebuilds the database only when it
   is missing. When an Office application starts alone and finds such a database (16 KB or
   less, without VBA's type library entry), it is moved aside as "... (Rosettus backup)" so
   this start rebuilds it in full. OAH_NO_OLEDBHEAL=1 disables. */
static void oah_office_oledb_heal(void) {
    const char *h = getenv("HOME");
    char db[1024], bk[1100];
    struct stat st;
    int fd, found = 0;
    if (!h || getenv("OAH_NO_OLEDBHEAL")) return;
    snprintf(db, sizeof db, "%s/Library/Preferences/Microsoft/OLE Registration Database 11", h);
    if (stat(db, &st) || st.st_size > 16384 || st.st_size == 0) return;
    if ((fd = open(db, O_RDONLY)) >= 0) {
        static const char g[] = "{000204EF-0000-0000-C000-000000000046}";
        char *b = malloc((size_t)st.st_size + 1);
        if (b) {
            ssize_t n = read(fd, b, (size_t)st.st_size), i;
            for (i = 0; n > 0 && i + (ssize_t)sizeof g - 1 <= n && !found; i++) if (!memcmp(b + i, g, sizeof g - 1)) found = 1;
            free(b);
        }
        close(fd);
    }
    if (found) return;
    snprintf(bk, sizeof bk, "%s (Rosettus backup)", db);
    if (!rename(db, bk)) fprintf(stderr, "[shmem] incomplete OLE Registration Database 11 (%lld bytes, no VBA type library) moved aside: Office rebuilds it\n", (long long)st.st_size);
}
static void oah_office_shmem_fix(unsigned int num, unsigned int *a) {
    static int done;
    const char *nm = (const char *)a[0], *suf;
    char n1[64];
    if (done || !nm || (unsigned int)a[0] < 0x1000) return;
    if (strncmp(nm, "ShMemIntCrit", 12) && strncmp(nm, "ShMemExtCrit", 12) && strncmp(nm, "PT_ShMemRef", 11)) return;
    done = 1;
    if (getenv("OAH_NO_SHMEMFIX") || oah_office_running_other()) return;
    oah_office_oledb_heal();
    suf = nm + (nm[0] == 'P' ? 11 : 12);
    if (strlen(suf) > 32) return;
    snprintf(n1, sizeof n1, "ShMemIntCrit%s", suf); sem_unlink(n1);
    snprintf(n1, sizeof n1, "ShMemExtCrit%s", suf); sem_unlink(n1);
    snprintf(n1, sizeof n1, "PT_ShMemRef%s", suf); shm_unlink(n1);
    if (getenv("OAH_VERBOSE") || getenv("OAH_SCTRACE")) fprintf(stderr, "[shmem] removed stale Office ShMem objects for session %s (syscall %u)\n", suf, num);
}
int oah_path_fix(unsigned int num, unsigned int *a) {
    oah_gt_syscall_tick(num);   /* fix rows for libraries whose symbols were not mapped yet (oah_gtrace.h) */
    /* guest exit(2) syscall: the guest's libc has already run its atexit handlers and flushed
       its streams, so end the process here. translate's own exit path could deadlock against a
       thread still running translated code (CS2's HFSTest, run by the installer: two threads
       spinning at 200% CPU for good, the install stalled). OAH_NO_FASTEXIT: old path. */
    if (num == 1) {
        static int fast = -1;
        if (fast < 0) fast = getenv("OAH_NO_FASTEXIT") ? 0 : 1;
        if (fast) {
            if (a[0] && !getenv("OAH_QUIET_EXIT")) oah_exit_note(a);
            else oah_tel_save();
            _exit((int)a[0]);
        }
    }
    if (num == 266 || num == 268) oah_office_shmem_fix(num, a);
    if (num == 202 && oah_sysctl_fix(a)) return 1;
    if (num == 46 && oah_sigtrap_keep(a)) return 1;
    if (num == 46 && oah_segv_keep(a)) return 1;
    if (num == 244) { int r = oah_spawn_fix(a); if (r) return r; }   /* 2: served as an error */
    if (num == 244 && getenv("OAH_SPAWNTRACE")) {
        unsigned int *d = (unsigned int *)a[2], i;
        fprintf(stderr, "[spawn] path %s adesc %p:", a[1] ? (char *)a[1] : "?", (void *)d);
        for (i = 0; d && i < 8; i++) fprintf(stderr, " %08x", d[i]);
        fprintf(stderr, "\n");
        if (d && d[1]) { unsigned char *at = (unsigned char *)d[1]; fprintf(stderr, "[spawn] attr:"); for (i = 0; i < 48; i++) fprintf(stderr, "%s%02x", i % 4 ? "" : " ", at[i]); fprintf(stderr, "\n"); }
        if (d && d[3]) { unsigned char *fa = (unsigned char *)d[3]; fprintf(stderr, "[spawn] factions:"); for (i = 0; i < 32; i++) fprintf(stderr, "%s%02x", i % 4 ? "" : " ", fa[i]); fprintf(stderr, "\n"); }
    }
    if (num == 344) return oah_dirsort(a);
    /* readlink("/etc/localtime"): Mojave's link points into /var/db/timezone/zoneinfo/; 10.6
       CFTimeZoneCopySystem only accepts a "/usr/share/zoneinfo/" target and fell back to GMT
       (NSDate, TextWrangler's print header and every CF-formatted time were in UTC). Answer with
       the /usr/share/zoneinfo spelling (the same file). OAH_NO_TZLINK=1 disables. */
    if (num == 58 && g_sc_skip_ok && a[0] >= 0x1000 && a[1] >= 0x1000 &&
        (!strcmp((const char *)a[0], "/etc/localtime") || !strcmp((const char *)a[0], "/private/etc/localtime")) &&
        !getenv("OAH_NO_TZLINK")) {
        char t[1024]; ssize_t n = readlink((const char *)a[0], t, sizeof t - 1);
        static const char vp[] = "/var/db/timezone/zoneinfo/";
        if (n > (ssize_t)(sizeof vp - 1) && !strncmp(t, vp, sizeof vp - 1)) {
            char o[1100]; size_t L;
            t[n] = 0;
            L = (size_t)snprintf(o, sizeof o, "/usr/share/zoneinfo/%s", t + sizeof vp - 1);
            if (L > a[2]) L = a[2];
            memcpy((void *)a[1], o, L);
            ((unsigned int *)a)[-3] = (unsigned int)L;
            return 1;
        }
    }
    { extern void oah_cp_sc_note(unsigned int, unsigned int *); oah_cp_sc_note(num, a); }
    {   /* syscall storm detector: a number called 50000 times in under 10 s is reported */
        static unsigned int cnt[512]; static time_t t0[512];
        if (num < 512 && ++cnt[num] % 50000 == 0) {
            time_t now = time(0);
            if (now - t0[num] < 10) {
                char pb[160] = "";
                if (num == 220 || num == 284 || num == 33 || num == 188 || num == 190 || num == 338 || num == 340) {
                    unsigned int w; int j;
                    for (j = 0; j < 150; j++) { if (!probe_read((a[0] + j) & ~3u, &w)) break; pb[j] = ((char *)&w)[(a[0] + j) & 3]; if (!pb[j]) break; }
                    pb[j < 150 ? j : 150] = 0;
                }
                fprintf(stderr, "[sc] syscall %u: %u calls (50000 in %lds); args %x %x %x %x %x %x %s\n", num, cnt[num],
                        (long)(now - t0[num]), a[0], a[1], a[2], a[3], a[4], a[5], pb);
                {   static int bt[512];
                    if (bt[num]++ < 2 && g_main_R && pthread_main_np()) {
                        unsigned int *Rm = (unsigned int *)g_main_R;
                        fprintf(stderr, "[sc]   main thread guest state r1=%08x lr=%08x r3=%08x:\n", Rm[1], Rm[0x90 / 4], Rm[3]);
                        oah_crash_guest_frames((unsigned int)g_main_R);
                    }
                }
            }
            t0[num] = now;
        }
    }
    int i0 = -1, i1 = -1, k; char *b; const char *r;
    static int off = -1;
    if (num == 220 && getenv("OAH_GALTRACE")) {
        struct attrlist *al = (struct attrlist *)a[1]; char tb[4096]; int r, e;
        unsigned int sz = a[3] < sizeof tb ? a[3] : sizeof tb;
        r = getattrlist((const char *)a[0], al, tb, sz, a[4]); e = errno;
        fprintf(stderr, "[gal] %s cmn=%#x vol=%#x dir=%#x file=%#x fork=%#x opt=%#x size=%u -> %d errno %d\n",
                (const char *)a[0], al->commonattr, al->volattr, al->dirattr, al->fileattr, al->forkattr, a[4], a[3], r, e);
    }
    if (num == 46 && a[1] > 0x1000 && getenv("OAH_SIGTRACE")) {
        unsigned int *s = (unsigned int *)a[1];
        fprintf(stderr, "[sig] sigaction(%u) handler=%#x tramp=%#x mask=%#x flags=%#x\n", a[0], s[0], s[1], s[2], s[3]);
    }
    switch (num) {
    case 5: case 10: case 15: case 16: case 33: case 58: case 136: case 137: case 138: case 157: case 188: case 190:
    case 191: case 200: case 220: case 221: case 234: case 236: case 238: case 240: case 338: case 340: case 345:
    case 398: case 32 /* chflags */:
    case 12 /* chdir */: case 277 /* open_extended */: case 279 /* stat_extended */: case 280 /* lstat_extended */:
    case 282 /* chmod_extended */: case 284 /* access_extended */: case 341 /* stat64_extended */:
    case 342 /* lstat64_extended */: case 225 /* searchfs */: case 214 /* fsctl */:
        i0 = 0; break;
    case 128: case 9: i0 = 0; i1 = 1; break;     /* rename, link */
    case 57: i1 = 1; break;                       /* symlink(target, path) */
    default: return 0;
    }
    if (off < 0) off = getenv("OAH_NO_PATHMAP") ? 1 : 0;
    if (off) return 0;
    pthread_once(&g_pathonce, oah_path_init);
    for (k = 0; k < 2; k++) {
        int i = k ? i1 : i0;
        if (i < 0 || a[i] < 0x1000) continue;
        if (!(b = oah_path_buf(k))) return 0;
        {   static int pl = -1; if (pl < 0) pl = getenv("OAH_PATHLOG") ? 1 : 0;
            if (pl && strstr((const char *)a[i], getenv("OAH_PATHLOG")))
                fprintf(stderr, "[pathlog] pid %d sc %u %s\n", getpid(), num, (const char *)a[i]); }
        if ((r = oah_guest_path_map((const char *)a[i], b, 1100))) {
            if (num == 136 || num == 5 || num == 398) {   /* make sure the private parent exists */
                char d[1100]; char *s; strlcpy(d, r, sizeof d);
                if ((s = strrchr(d, '/')) && s > d + strlen(g_ppc_cache)) { *s = 0; mkdir(d, 0755); }
            }
            { static int nlog; if (getenv("OAH_VERBOSE") || nlog < 3) { nlog++; fprintf(stderr, "[pathmap] %u %s -> %s\n", num, (const char *)a[i], r); } }
            a[i] = (unsigned int)r;
        }
    }
    if (num == 220 && !oah_gal_volfix(a)) oah_gal_rootfix(a);
    if (num == 157 || num == 345) oah_statfs_apfs(num, a);   /* like the gal fixes: trap made a no-op */
    return 0;
}
/* statfs on an APFS volume: 10.6 had no APFS, and programs that want a Mac OS Extended
   volume compare f_fstypename with "hfs" (the CS2 installer's target list held only HFS+
   disk images, then nothing). Report "hfs", as FSGetVolumeInfo reports 'H+'. The call is
   run here and the struct filled in native byte order (translate still converts the buffer
   for the original syscall number); the trap itself becomes a no-op (issetugid).
   157 = statfs (old 32-bit struct, 272 bytes), 345 = statfs64. OAH_NO_APFS_HPLUS: off. */
static int oah_statfs_apfs(unsigned int num, unsigned int *a) {
    static int on = -1;
    struct statfs s;
    unsigned char *g = (unsigned char *)a[1];
    if (on < 0) on = getenv("OAH_NO_APFS_HPLUS") ? 0 : 1;
    if (!on || a[0] < 0x1000 || (unsigned int)a[1] < 0x1000) return 0;
    if (statfs((const char *)a[0], &s) != 0 || strcmp(s.f_fstypename, "apfs")) return 0;
    if (num == 157) {
        unsigned long long blocks = s.f_blocks, bfree = s.f_bfree, bavail = s.f_bavail;
        unsigned int bs = s.f_bsize;
        while (blocks > 0x7fffffffULL) { blocks >>= 1; bfree >>= 1; bavail >>= 1; bs <<= 1; }
        memset(g, 0, 272);
        OSWriteLittleInt32(g, 4, bs); OSWriteLittleInt32(g, 8, s.f_iosize);
        OSWriteLittleInt32(g, 12, (unsigned int)blocks); OSWriteLittleInt32(g, 16, (unsigned int)bfree);
        OSWriteLittleInt32(g, 20, (unsigned int)bavail);
        OSWriteLittleInt32(g, 24, s.f_files > 0xffffffffULL ? 0xffffffffu : (unsigned int)s.f_files);
        OSWriteLittleInt32(g, 28, s.f_ffree > 0xffffffffULL ? 0xffffffffu : (unsigned int)s.f_ffree);
        OSWriteLittleInt32(g, 32, (unsigned int)s.f_fsid.val[0]); OSWriteLittleInt32(g, 36, (unsigned int)s.f_fsid.val[1]);
        OSWriteLittleInt32(g, 40, s.f_owner); OSWriteLittleInt16(g, 46, (unsigned short)s.f_type);
        OSWriteLittleInt32(g, 48, s.f_flags);
        strlcpy((char *)g + 60, "hfs", 15);
        strlcpy((char *)g + 75, s.f_mntonname, 90);
        strlcpy((char *)g + 165, s.f_mntfromname, 90);
    } else {
        memset(g, 0, 2168);
        OSWriteLittleInt32(g, 0, s.f_bsize); OSWriteLittleInt32(g, 4, s.f_iosize);
        OSWriteLittleInt64(g, 8, s.f_blocks); OSWriteLittleInt64(g, 16, s.f_bfree); OSWriteLittleInt64(g, 24, s.f_bavail);
        OSWriteLittleInt64(g, 32, s.f_files); OSWriteLittleInt64(g, 40, s.f_ffree);
        OSWriteLittleInt32(g, 48, (unsigned int)s.f_fsid.val[0]); OSWriteLittleInt32(g, 52, (unsigned int)s.f_fsid.val[1]);
        OSWriteLittleInt32(g, 56, s.f_owner); OSWriteLittleInt32(g, 60, s.f_type);
        OSWriteLittleInt32(g, 64, s.f_flags); OSWriteLittleInt32(g, 68, s.f_fssubtype);
        strlcpy((char *)g + 72, "hfs", 16);
        strlcpy((char *)g + 88, s.f_mntonname, 1024);
        strlcpy((char *)g + 1112, s.f_mntfromname, 1024);
    }
    ((unsigned int *)a)[-3] = 327;    /* issetugid() -> 0 */
    {   static int nlog; if (nlog++ < 2) fprintf(stderr, "[statfs] %s: apfs reported as hfs\n", (const char *)a[0]); }
    return 1;
}
/* APFS (Mojave) fails getattrlist with EINVAL when volume attributes are requested together
   with ATTR_CMN_BKUPTIME; HFS+ returned 0 for it. SL CarbonCore's FSGetVolumeInfo asks for
   exactly that (cmn 0x664aa, vol 0x80036674) on /.vol/<dev>/@, so PBHGetVInfo failed and
   AppleWorks' Starting Points built no tabs. Run the call without BKUPTIME, splice in a zero
   timespec, and turn the trap into a no-op. Returns 1 when it handled the call. */
static unsigned int oah_gal_cmnoff(unsigned int cmn, unsigned int bit) {
    static const struct { unsigned int b, sz; } t[] = {
        { ATTR_CMN_NAME, sizeof(attrreference_t) }, { ATTR_CMN_DEVID, sizeof(dev_t) }, { ATTR_CMN_FSID, sizeof(fsid_t) },
        { ATTR_CMN_OBJTYPE, sizeof(fsobj_type_t) }, { ATTR_CMN_OBJTAG, sizeof(fsobj_tag_t) }, { ATTR_CMN_OBJID, sizeof(fsobj_id_t) },
        { ATTR_CMN_OBJPERMANENTID, sizeof(fsobj_id_t) }, { ATTR_CMN_PAROBJID, sizeof(fsobj_id_t) }, { ATTR_CMN_SCRIPT, sizeof(text_encoding_t) },
        { ATTR_CMN_CRTIME, sizeof(struct timespec) }, { ATTR_CMN_MODTIME, sizeof(struct timespec) }, { ATTR_CMN_CHGTIME, sizeof(struct timespec) },
        { ATTR_CMN_ACCTIME, sizeof(struct timespec) }, { ATTR_CMN_BKUPTIME, sizeof(struct timespec) } };
    unsigned int off = 4, i;
    for (i = 0; i < sizeof t / sizeof t[0] && t[i].b != bit; i++) if (cmn & t[i].b) off += t[i].sz;
    return off;
}
static int oah_gal_volfix(unsigned int *a) {
    struct attrlist *al = (struct attrlist *)a[1], q;
    static int on = -1;
    char *tb; unsigned int sz = a[3], len, off, ts = sizeof(struct timespec);
    if (on < 0) on = getenv("OAH_NO_VOLBKFIX") ? 0 : 1;
    if (!on || !al || !a[0] || !a[2] || al->bitmapcount != ATTR_BIT_MAP_COUNT || !al->volattr ||
        !(al->commonattr & ATTR_CMN_BKUPTIME) || (al->commonattr & ATTR_CMN_RETURNED_ATTRS) || sz < 4 + ts) return 0;
    if (getattrlist((const char *)a[0], al, (void *)a[2], sz, a[4]) == 0 || errno != EINVAL) return 0;   /* trap redoes it */
    q = *al; q.commonattr &= ~ATTR_CMN_BKUPTIME;
    if (!(tb = (char *)malloc(sz))) return 0;
    if (getattrlist((const char *)a[0], &q, tb, sz, a[4]) != 0) { free(tb); return 0; }
    len = *(unsigned int *)tb; if (len > sz) len = sz;
    off = oah_gal_cmnoff(al->commonattr, ATTR_CMN_BKUPTIME);
    if (off > len) { free(tb); return 0; }
    memcpy((char *)a[2], tb, off);
    memset((char *)a[2] + off, 0, ts);
    if (len > off) memcpy((char *)a[2] + off + ts, tb + off, len + ts <= sz ? len - off : sz - off - ts);
    *(unsigned int *)a[2] = len + ts <= sz ? len + ts : sz;
    if (al->commonattr & ATTR_CMN_NAME) ((attrreference_t *)((char *)a[2] + 4))->attr_dataoffset += ts;   /* data moved, ref did not */
    if ((al->commonattr & (ATTR_CMN_OBJID | ATTR_CMN_PAROBJID)) == (ATTR_CMN_OBJID | ATTR_CMN_PAROBJID)) {   /* APFS root parent 2 -> 1 */
        fsobj_id_t *o = (fsobj_id_t *)((char *)a[2] + oah_gal_cmnoff(al->commonattr, ATTR_CMN_OBJID));
        fsobj_id_t *p = (fsobj_id_t *)((char *)a[2] + oah_gal_cmnoff(al->commonattr, ATTR_CMN_PAROBJID));
        if (o->fid_objno == 2 && p->fid_objno == 2) p->fid_objno = 1;
    }
    free(tb);
    ((unsigned int *)a)[-3] = 327;    /* issetugid() -> 0 */
    {   static int nlog; if (nlog++ < 3) fprintf(stderr, "[gal] %s: vol attrs %#x with BKUPTIME emulated (cmn %#x)\n", (const char *)a[0], al->volattr, al->commonattr); }
    return 1;
}
/* getattrlist on an APFS volume root reports ATTR_CMN_PAROBJID = 2 (itself); HFS+ said 1
   (fsRtParID), and Carbon / DesktopServices walk parents until they see 1: NSSavePanel spun
   for ever on getattrlist("/.vol/<dev>/2"). For a directory whose object id is 2 and whose
   parent is 2, run the call here, put 1 in the parent field, and let the trap do a no-op
   (issetugid) instead. a[] = path, attrlist, buffer, size, options (already native). */
static void oah_gal_rootfix(unsigned int *a) {
    struct attrlist *al = (struct attrlist *)a[1], q;
    struct { unsigned int len; fsobj_id_t obj, par; } __attribute__((packed)) ids;
    unsigned int off, rc;
    static int on = -1;
    if (on < 0) on = getenv("OAH_NO_ROOTPARFIX") ? 0 : 1;
    if (!on || !al || !a[0] || !a[2] || al->bitmapcount != ATTR_BIT_MAP_COUNT || !(al->commonattr & ATTR_CMN_PAROBJID)) return;
    memset(&q, 0, sizeof q);
    q.bitmapcount = ATTR_BIT_MAP_COUNT;
    q.commonattr = ATTR_CMN_OBJID | ATTR_CMN_PAROBJID;
    if (getattrlist((const char *)a[0], &q, &ids, sizeof ids, a[4]) != 0) return;
    if (ids.obj.fid_objno != 2 || ids.par.fid_objno != 2) return;
    if (getattrlist((const char *)a[0], al, (void *)a[2], a[3], a[4]) != 0) return;   /* the real call fails the same way */
    off = 4;
    if (al->commonattr & ATTR_CMN_RETURNED_ATTRS) off += sizeof(attribute_set_t);
    if (al->commonattr & ATTR_CMN_NAME) off += sizeof(attrreference_t);
    if (al->commonattr & ATTR_CMN_DEVID) off += sizeof(dev_t);
    if (al->commonattr & ATTR_CMN_FSID) off += sizeof(fsid_t);
    if (al->commonattr & ATTR_CMN_OBJTYPE) off += sizeof(fsobj_type_t);
    if (al->commonattr & ATTR_CMN_OBJTAG) off += sizeof(fsobj_tag_t);
    if (al->commonattr & ATTR_CMN_OBJID) off += sizeof(fsobj_id_t);
    if (al->commonattr & ATTR_CMN_OBJPERMANENTID) off += sizeof(fsobj_id_t);
    rc = off + sizeof(fsobj_id_t) <= a[3] ? 1 : 0;
    if (rc) ((fsobj_id_t *)(a[2] + off))->fid_objno = 1;
    ((unsigned int *)a)[-3] = 327;    /* saved eax: issetugid() -> 0, no side effects */
    {   static int nlog; if (nlog++ < 3) fprintf(stderr, "[gal] %s: volume root parent 2 -> 1 (cmn %#x, offset %u)\n", (const char *)a[0], al->commonattr, off); }
}
/* guest exit(2) with a non-zero status: say so, with the last bridged guest context */
static void oah_crash_guest_frames(unsigned int RR);
void oah_exit_note(unsigned int *a) {
    oah_tel_save();
    if ((a[0] == 0 && !getenv("OAH_EXITTRACE")) || getenv("OAH_QUIET_EXIT")) return;
    fprintf(stderr, "[exit] guest exit(%d) pid %d\n", (int)a[0], getpid());
    if (g_dbg_rcvbuf) {
        volatile unsigned int *w = g_dbg_rcvbuf; int k;
        fprintf(stderr, "[exit] last 0x710 receive buffer %p:", (void *)w);
        for (k = 0; k < 12; k++) fprintf(stderr, " %08x", w[k]);
        fprintf(stderr, "\n");
    }
    {   /* the exiting thread's own guest state: an R pointer somewhere up its native stack */
        unsigned int sp = (unsigned int)__builtin_frame_address(0), k, v, found = 0;
        unsigned int seen[8], ns = 0, j;
        fprintf(stderr, "[exit] native thread %p\n", (void *)pthread_self());
        {   unsigned int f = (unsigned int)__builtin_frame_address(0), d2, ra2, nf;
            fprintf(stderr, "[exit] native bt:");
            for (d2 = 0; d2 < 20 && f > 0x1000; d2++) {
                if (!probe_read(f + 4, &ra2) || !probe_read(f, &nf)) break;
                fprintf(stderr, " %08x", ra2);
                {   unsigned int S = 0, Ri = 0, gpc = 0;
                    if (probe_read(f + 8, &S) && S > 0x1000 && probe_read(S + 8, &Ri) && Ri > 0x1000 && oah_hang_isR(Ri) &&
                        probe_read(S + 0x1c, &gpc)) {
                        unsigned int *Rr = (unsigned int *)Ri, b0 = 0;
                        const char *nm = oah_guest_image_name(gpc, &b0), *sl = strrchr(nm, '/');
                        fprintf(stderr, "\n[exit]   interp state %08x R=%08x pc=%08x (%s+0x%x) r1=%08x r3=%08x lr=%08x\n[exit]  ",
                                S, Ri, gpc, sl ? sl + 1 : nm, gpc - b0, Rr[1], Rr[3], Rr[0x90 / 4]);
                        {   unsigned int q, w2; vm_address_t ra3 = gpc; vm_size_t rs3 = 0; vm_region_basic_info_data_t bi3;
                            mach_msg_type_number_t c3 = VM_REGION_BASIC_INFO_COUNT; mach_port_t o3;
                            if (vm_region(mach_task_self(), &ra3, &rs3, VM_REGION_BASIC_INFO, (vm_region_info_t)&bi3, &c3, &o3) == KERN_SUCCESS)
                                fprintf(stderr, "[exit]   pc region %08lx-%08lx prot %d\n", (unsigned long)ra3, (unsigned long)(ra3 + rs3), bi3.protection);
                            fprintf(stderr, "[exit]   code @%08x:", gpc - 32);
                            for (q = 0; q < 16; q++) { if (!probe_read(gpc - 32 + 4 * q, &w2)) break; fprintf(stderr, " %08x", OSSwapBigToHostInt32(w2)); }
                            fprintf(stderr, "\n[exit]   gprs:");
                            for (q = 0; q < 32; q++) fprintf(stderr, " r%u=%08x", q, Rr[q]);
                            fprintf(stderr, " ctr=%08x\n", Rr[0x94 / 4]);
                        }
                        oah_crash_guest_frames(Ri);
                        break;
                    }
                }
                if (nf <= f) break;
                f = nf;
            }
            fprintf(stderr, "\n");
        }
        for (k = 0; k < 6000 && !found; k++)
            if (probe_read(sp + 4 * k, &v) && v > 0x10000 && !(v & 3) && oah_hang_isR(v)) {
                unsigned int *Rc = (unsigned int *)v;
                for (j = 0; j < ns && seen[j] != v; j++) ;
                if (j < ns) continue;
                if (ns < 8) seen[ns++] = v;
                fprintf(stderr, "[exit]   candidate R=%08x (stack +%u) r1=%08x r3=%08x lr=%08x\n", v, 4 * k, Rc[1], Rc[3], Rc[0x90 / 4]);
                if (Rc[3] == a[0]) found = v;
            }
        if (!found && ns) found = seen[0];
        if (found) {
            unsigned int *Rt = (unsigned int *)found;
            fprintf(stderr, "[exit] exiting thread's guest state R=%08x r1=%08x r3=%08x lr=%08x\n", found, Rt[1], Rt[3], Rt[0x90 / 4]);
            oah_crash_guest_frames(found);
            return;
        }
    }
    if (g_last_R) oah_crash_guest_frames((unsigned int)g_last_R);
}
__asm__(
    ".text\n"
    ".globl _oah_kev_hook\n"
    "_oah_kev_hook:\n"
    "  cmpl $363, %eax\n"
    "  je 1f\n"
    "  cmpl $1, %eax\n"
    "  je 2f\n"
    "  cmpl $59, %eax\n"
    "  je 3f\n"
    "  cmpl $398, %eax\n"
    "  ja 9f\n"
    "  pushl %eax\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  andl $-16, %esp\n"
    "  subl $8, %esp\n"
    "  leal 24(%ebp), %ecx\n"
    "  pushl %ecx\n"
    "  pushl 12(%ebp)\n"
    "  call _oah_path_fix\n"
    "  movl %ebp, %esp\n"
    "  testl %eax, %eax\n"
    "  jnz 8f\n"
    "  popl %ebp\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  popl %eax\n"
    "9:\n"
    "  ret\n"
    "8:\n"                      /* served here: eax = result, skip the 2-byte int $0x80, CF clear */
    "  popl %ebp\n"
    "  popl %edx\n"
    "  movl %eax, %edx\n"         /* oah_path_fix's answer: 1 served, 2 served as an error */
    "  popl %ecx\n"
    "  popl %eax\n"
    "  addl $2, (%esp)\n"
    "  cmpl $2, %edx\n"
    "  movl $0, %edx\n"
    "  je 7f\n"
    "  clc\n"
    "  ret\n"
    "7:\n"                      /* ...or as an error: CF set, eax = errno */
    "  stc\n"
    "  ret\n"
    "3:\n"
    "  pushl %eax\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  andl $-16, %esp\n"
    "  subl $12, %esp\n"
    "  leal 24(%ebp), %ecx\n"
    "  pushl %ecx\n"
    "  call _oah_exec_fix\n"
    "  movl %ebp, %esp\n"
    "  popl %ebp\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  popl %eax\n"
    "  ret\n"
    "2:\n"
    "  pushl %eax\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  andl $-16, %esp\n"
    "  subl $12, %esp\n"
    "  leal 24(%ebp), %ecx\n"
    "  pushl %ecx\n"
    "  call _oah_exit_note\n"
    "  movl %ebp, %esp\n"
    "  popl %ebp\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  popl %eax\n"
    "  ret\n"
    "1:\n"
    "  pushl %eax\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  andl $-16, %esp\n"
    "  subl $12, %esp\n"
    "  leal 24(%ebp), %ecx\n"   /* int-time esp+4 = first syscall arg */
    "  pushl %ecx\n"
    "  call _oah_kevent_fix\n"
    "  movl %ebp, %esp\n"
    "  popl %ebp\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  popl %eax\n"
    "  ret\n"
);
extern void oah_kev_hook(void);

/* ---- optional guest BSD syscall tracer (OAH_SCTRACE=1) ---- */
void oah_log_sc(unsigned int num, unsigned int *a) {
    static int only = -1;
    if (num == 363 && !getenv("OAH_NO_KEVFIX")) oah_kevent_fix(a);
    if (num == 59) oah_exec_fix(a); else oah_path_fix(num, a);
    if (only < 0) { const char *e = getenv("OAH_SCTRACE"); only = (e && atoi(e) > 1) ? atoi(e) : 0; }
    if (only && (int)num != only) return;
    if (num == 363) {   /* kevent(kq, changelist, nchanges, eventlist, nevents, timeout) */
        char b[512]; int o, k;
        o = snprintf(b, sizeof b, "[gsc] t%x kevent(kq=%d nch=%d nev=%d tmo=", (unsigned)pthread_mach_thread_np(pthread_self()), a[0], a[2], a[4]);
        if (a[5]) o += snprintf(b + o, sizeof b - o, "%u.%09u)", ((unsigned *)a[5])[0], ((unsigned *)a[5])[1]);
        else o += snprintf(b + o, sizeof b - o, "inf)");
        for (k = 0; k < (int)a[2] && k < 4 && a[1]; k++) {
            unsigned char *c = (unsigned char *)a[1] + 20 * k;
            o += snprintf(b + o, sizeof b - o, " [id=%u f=%d fl=%#x ff=%#x d=%d]", *(unsigned *)c, *(short *)(c + 4),
                          *(unsigned short *)(c + 6), *(unsigned *)(c + 8), *(int *)(c + 12));
        }
        fprintf(stderr, "%s\n", b);
        return;
    }
    if (num == 54)
        fprintf(stderr, "[gsc] ioctl(fd=%d, req=0x%08x, arg=0x%08x)\n", a[0], a[1], a[2]);
    else if (num == 5 || num == 33 || num == 188 || num == 190 || num == 338 || num == 340 || num == 398 || num == 266 || num == 268 || num == 271)
        fprintf(stderr, "[gsc] %u(\"%.200s\", 0x%x)\n", num, (a[0] > 0x1000) ? (const char *)a[0] : "?", a[1]);
    else
        fprintf(stderr, "[gsc] %u(0x%x, 0x%x, 0x%x, 0x%x)\n", num, a[0], a[1], a[2], a[3]);
}
__asm__(
    ".text\n"
    ".globl _oah_sc_hook\n"
    "_oah_sc_hook:\n"
    "  pushl %eax\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  andl $-16, %esp\n"
    "  subl $8, %esp\n"
    "  leal 24(%ebp), %ecx\n"   /* int-time esp+4 = first syscall arg */
    "  pushl %ecx\n"
    "  pushl 12(%ebp)\n"        /* saved eax = syscall number */
    "  call _oah_log_sc\n"
    "  movl %ebp, %esp\n"
    "  popl %ebp\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  popl %eax\n"
    "  ret\n"
);
extern void oah_sc_hook(void);
static void install_sc_hooks_with(void (*hook)(void), const char *tag) {
    unsigned char *a, *lo = (unsigned char *)0xb815ab00, *hi = (unsigned char *)0xb815ae00;
    static const unsigned char pat[7] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0xcd, 0x80 };
    int n = 0;
    if (vm_protect(mach_task_self(), 0xb815a000, 0x4000, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) {
        fprintf(stderr, "[gsc] vm_protect rwx failed\n"); return;
    }
    for (a = lo; a + 7 <= hi; a++) {
        if (memcmp(a, pat, 7) == 0) {
            int rel = (int)((unsigned int)hook - (unsigned int)(a + 5));
            a[0] = 0xe8; memcpy(a + 1, &rel, 4);
            n++;
        }
    }
    vm_protect(mach_task_self(), 0xb815a000, 0x4000, 0, VM_PROT_READ | VM_PROT_EXECUTE);
    if (tag) fprintf(stderr, "[%s] patched %d int $0x80 sites\n", tag, n);
}

static void install_mach_hooks(void) {
    unsigned char *a, *lo = (unsigned char *)0xb815a800, *hi = (unsigned char *)0xb815ac00;
    static const unsigned char pat[7] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0xcd, 0x81 };
    int n = 0;
    if (vm_protect(mach_task_self(), 0xb815a000, 0x1000, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) {
        fprintf(stderr, "[ndr] vm_protect rwx failed\n"); return;
    }
    for (a = lo; a + 7 <= hi; a++) {
        if (memcmp(a, pat, 7) == 0) {
            int rel = (int)((unsigned int)oah_mach_hook - (unsigned int)(a + 5));
            a[0] = 0xe8; memcpy(a + 1, &rel, 4);
            n++;
        }
    }
    vm_protect(mach_task_self(), 0xb815a000, 0x1000, 0, VM_PROT_READ | VM_PROT_EXECUTE);
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[ndr] patched %d int $0x81 sites\n", n);
}

/* The translator delivers a guest's fatal signal (illegal instruction, unhandled bad access) by
   restoring the default action and signalling itself, so the process just exits 132/139 with no
   report.  Log the guest state first. */
static void oah_fatal_sig_report(int sig, const char *how) {
    static volatile int once;
    unsigned int cand[400], nc = 0, k, v, sp = (unsigned int)__builtin_frame_address(0), found = 0;
    extern int oah_hang_isR_ext(unsigned int);
    if (sig != SIGILL && sig != SIGSEGV && sig != SIGBUS && sig != SIGTRAP && sig != SIGABRT && sig != SIGFPE) return;
    if (__sync_fetch_and_add(&once, 1)) return;
    fprintf(stderr, "[fatal] %s(sig %d) from the translator (callppc depth %lu, last \"%s\" -> %08x)\n", how, sig,
            g_cb_depth, g_last_cp_tag ? g_last_cp_tag : "-", g_last_cp_fn);
    for (k = 0; k < 2000 && nc < 400; k++) {
        vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), sp + 4 * k, 4, (vm_address_t)&v, &got) != KERN_SUCCESS) break;
        if (v > 0x10000 && !(v & 3)) cand[nc++] = v;
    }
    for (k = 0; k < nc && !found; k++) if (oah_hang_isR_ext(cand[k])) found = cand[k];
    if (found) {
        unsigned int *Rt = (unsigned int *)found;
        fprintf(stderr, "[fatal] guest R=%08x r1=%08x r3=%08x r4=%08x lr=%08x ctr=%08x\n", found, Rt[1], Rt[3], Rt[4], Rt[0x90 / 4], Rt[0x94 / 4]);
        oah_crash_guest_frames(found);
    } else fprintf(stderr, "[fatal] no guest register file found on this stack\n");
}
static int my_kill(pid_t pid, int sig) {
    if (pid == getpid() || pid == 0) oah_fatal_sig_report(sig, "kill");
    return kill(pid, sig);
}
static int my_raise(int sig) { oah_fatal_sig_report(sig, "raise"); return raise(sig); }
static int my_pthread_kill(pthread_t t, int sig) { oah_fatal_sig_report(sig, "pthread_kill"); return pthread_kill(t, sig); }

static void trap_report_handler(int sig, siginfo_t *info, void *uap);
volatile int g_in_trap_report;
/* The guest's own SIGTRAP handler (Keynote's crash catcher, installed through the translator's
   sigaction) used to replace bridge_trap_handler: every bridge trap the interpreter raised after
   that went to the guest handler, which called exit(1).  Keep ours installed and remember the
   translator's; bridge_trap_handler hands it every trap that is not a bridge. */
volatile int g_trap_handler_installed;
struct sigaction g_xl_trap_act; volatile int g_xl_trap_set;
static int my_sigaction(int sig, const struct sigaction *act, struct sigaction *oact) {
    if (sig == SIGTRAP && g_trap_handler_installed && !getenv("OAH_NO_TRAPKEEP")) {
        struct sigaction prev = g_xl_trap_act;
        if (act) {
            g_xl_trap_act = *act; g_xl_trap_set = 1;
            { static int n; if (n++ < 3) fprintf(stderr, "[sigtrap] translator installs SIGTRAP handler %p (flags %x); keeping the bridge handler\n",
                                                  (void *)act->sa_handler, act->sa_flags); }
        }
        if (oact) {
            if (g_xl_trap_set) *oact = prev;
            else sigaction(SIGTRAP, 0, oact);
        }
        return 0;
    }
    if (act && (sig == SIGILL || sig == SIGBUS) && act->sa_handler == SIG_DFL && !g_in_trap_report && !getenv("OAH_NO_FATALREPORT")) {
        struct sigaction r;
        memset(&r, 0, sizeof r);
        r.sa_sigaction = trap_report_handler;          /* reports, then SIG_DFL + re-raise */
        r.sa_flags = SA_SIGINFO | SA_RESETHAND;
        return sigaction(sig, &r, oact);
    }
    return sigaction(sig, act, oact);
}

static int g_nofree = -1;
static void my_free(void *p) {
    if (g_nofree < 0) g_nofree = getenv("OAH_NOFREE") != 0;
    if (g_nofree) return;
    free(p);
}

static int g_rlog = -1;
static void *my_realloc(void *p, size_t n) {
    void *q;
    if (g_nofree < 0) g_nofree = getenv("OAH_NOFREE") != 0;
    if (g_rlog < 0) g_rlog = getenv("OAH_RLOG") != 0;
    if (!g_nofree || !p) {
        q = realloc(p, n);
    } else {
        size_t o = malloc_size(p);
        q = malloc(n);
        if (q) memcpy(q, p, o < n ? o : n);
    }
    if (g_rlog && p && q != p) fprintf(stderr, "[realloc] %p -> %p size %lu (from %p)\n", p, q, (unsigned long)n, __builtin_return_address(0));
    return q;
}

struct interpose_s {
    void *new_func;
    void *orig_func;
};

__attribute__((used)) static const struct interpose_s interposers[] __attribute__((section("__DATA, __interpose"))) = {
    { (void *)my_IOServiceMatching, (void *)IOServiceMatching },
    { (void *)my_sysctlbyname, (void *)sysctlbyname },
    { (void *)my_open, (void *)open },
    { (void *)my_stat, (void *)stat },
    { (void *)my_lstat, (void *)lstat },
    { (void *)my_access, (void *)access },
    { (void *)my__exit, (void *)_exit },
    { (void *)my_exit, (void *)exit },
    { (void *)my_mach_msg, (void *)mach_msg },
    { (void *)my_syscall, (void *)syscall },
    { (void *)my_vm_allocate, (void *)vm_allocate },
    { (void *)my_vm_map, (void *)vm_map },
    { (void *)my_vm_remap, (void *)vm_remap },
    { (void *)my_vm_protect, (void *)vm_protect },
    { (void *)my_mmap, (void *)mmap },
    { (void *)my_thread_suspend, (void *)thread_suspend },
    { (void *)my_thread_resume, (void *)thread_resume },
    { (void *)my_vm_deallocate, (void *)vm_deallocate },
    { (void *)my_munmap, (void *)munmap },
    { (void *)my_free, (void *)free },
    { (void *)my_realloc, (void *)realloc },
    { (void *)my_kill, (void *)kill },
    { (void *)my_raise, (void *)raise },
    { (void *)my_pthread_kill, (void *)pthread_kill },
    { (void *)my_sigaction, (void *)sigaction }
};

#include <sys/resource.h>
/* fontworker livelock watchdog. The translated 10.6 fontworker sometimes ends with two threads
   spinning forever in CarbonCore's volume/universe locks (FileIDTreeLockSharedVolumeInfo /
   _SCUniverseRelease: one in the translator's spin_lock_try wrapper, the other handing off to
   it in thread_switch), at 200-600% CPU; every translated app then waits for fonts with no
   windows. fontd starts a new fontworker on demand, so a worker that has burned more than
   1.6 CPUs for 20 s in a row exits and the apps carry on. OAH_NO_FWWATCH=1 disables. */
static void *oah_fontworker_watch(void *arg) {
    double last = -1, lastw = 0;
    int hot = 0;
    (void)arg;
    for (;;) {
        struct rusage ru;
        struct timeval tv;
        double cpu, w;
        sleep(2);
        if (getrusage(RUSAGE_SELF, &ru)) continue;
        gettimeofday(&tv, 0);
        cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
        w = tv.tv_sec + tv.tv_usec / 1e6;
        if (last >= 0 && w > lastw) {
            double load = (cpu - last) / (w - lastw);
            hot = load > 1.6 ? hot + 1 : 0;
            if (hot >= 10) {
                fprintf(stderr, "[fwwatch] fontworker spinning at %.0f%% CPU for 20 s: exiting (fontd restarts it)\n", load * 100);
                oah_hang_dump_all("fontworker livelock");
                _exit(1);
            }
        }
        last = cpu; lastw = w;
    }
    return 0;
}
/* Idle exit for the translated helper agents (com.rosettus.ppc.* launchd jobs: pboard, coreservicesd,
   fontd, fontworker). They are started on demand when a PowerPC app looks up their service; once no
   PowerPC app has run for OAH_IDLE_EXIT seconds (default 30) they exit, and launchd starts them again
   on the next lookup. So nothing started for an app outlives it. OAH_NO_IDLEEXIT=1 disables.
   Agents list their pids in /tmp/rosettus-agents-UID/ so they do not count each other as apps. */
static char g_agent_pidfile[256];
static int oah_is_ppc_app_pid(pid_t p) {
    char path[PROC_PIDPATHINFO_MAXSIZE], f[300];
    const char *b;
    if (proc_pidpath(p, path, sizeof path) <= 0) return 0;
    b = strrchr(path, '/'); b = b ? b + 1 : path;
    if (strcmp(b, "translate_mojave") && strcmp(b, "translate") && strcmp(b, ".rosetta-translate")) return 0;
    if (!strcmp(b, "translate") && !strstr(path, ".app/Contents/MacOS/")) return 0;   /* not a wrapper's link */
    snprintf(f, sizeof f, "/tmp/rosettus-agents-%u/%d", (unsigned)getuid(), (int)p);
    return access(f, F_OK) != 0;      /* listed agents are not apps */
}
static void oah_agent_cleanup(void) { if (g_agent_pidfile[0]) unlink(g_agent_pidfile); }
static void *oah_idle_exit_thread(void *arg) {
    int limit = (int)(long)arg, idle = 0;
    for (;;) {
        pid_t pids[4096]; int n, i, apps = 0;
        sleep(5);
        n = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof pids) / (int)sizeof(pid_t);   /* 10.6 SDK: no proc_listallpids */
        for (i = 0; i < n; i++) if (pids[i] != getpid() && pids[i] > 0 && oah_is_ppc_app_pid(pids[i])) {
            static pid_t lastseen;
            if (pids[i] != lastseen && getenv("OAH_VERBOSE")) fprintf(stderr, "[agent] PowerPC app still running: pid %d\n", (int)pids[i]);
            lastseen = pids[i]; apps++; break;
        }
        idle = apps ? 0 : idle + 5;
        if (idle >= limit) {
            fprintf(stderr, "[agent] no PowerPC app for %d s - exiting (launchd restarts it on demand)\n", idle);
            oah_tel_save();
            oah_agent_cleanup();
            exit(0);
        }
    }
    return 0;
}
static void oah_idle_exit_start(void) {
    const char *lab = getenv("OAH_AGENT"), *s;          /* set in the agent plists */
    pthread_t t; int limit;
    char dir[128];
    if (!lab || !*lab) lab = getenv("XPC_SERVICE_NAME");
    if (getenv("OAH_VERBOSE")) fprintf(stderr, "[agent] label %s\n", lab ? lab : "(none)");
    if (!lab || strncmp(lab, "com.rosettus.ppc.", 16) || getenv("OAH_NO_IDLEEXIT")) return;
    snprintf(dir, sizeof dir, "/tmp/rosettus-agents-%u", (unsigned)getuid());
    mkdir(dir, 0700);
    {   /* drop entries of agents that ended without atexit (SIGTERM from launchd) */
        DIR *dd = opendir(dir); struct dirent *de;
        if (dd) {
            while ((de = readdir(dd))) {
                int p = atoi(de->d_name); char f[300];
                if (p > 0 && kill(p, 0) != 0 && errno == ESRCH) { snprintf(f, sizeof f, "%s/%s", dir, de->d_name); unlink(f); }
            }
            closedir(dd);
        }
    }
    snprintf(g_agent_pidfile, sizeof g_agent_pidfile, "%s/%d", dir, (int)getpid());
    close(open(g_agent_pidfile, O_WRONLY | O_CREAT | O_TRUNC, 0600));
    atexit(oah_agent_cleanup);
    limit = (s = getenv("OAH_IDLE_EXIT")) ? atoi(s) : 30;
    if (limit < 5) limit = 5;
    if (!pthread_create(&t, 0, oah_idle_exit_thread, (void *)(long)limit)) pthread_detach(t);
}
/* OAH_MEMREPORT=N: N seconds after start, list resident / dirty memory by mapped file (guest
   libraries, translator) and anonymous memory, largest first (efficiency work). */
static void *oah_memreport_thread(void *arg) {
    typedef struct { char name[160]; unsigned long res, dirty, n; } ent_t;
    static ent_t e[512];
    int ne = 0, i, j;
    vm_address_t a = 0;
    unsigned long tres = 0, tdirty = 0;
    sleep((unsigned)(long)arg);
    {   int guard = 0;
        while (guard++ < 200000) {
            vm_size_t sz = 0; mach_port_t obj = MACH_PORT_NULL;
            vm_region_extended_info_data_t info;
            mach_msg_type_number_t cnt = VM_REGION_EXTENDED_INFO_COUNT;
            char path[PROC_PIDPATHINFO_MAXSIZE]; const char *nm;
            vm_address_t start = a;
            if (vm_region_64(mach_task_self(), &a, &sz, VM_REGION_EXTENDED_INFO, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) break;
            if (a < start || !sz) break;
            path[0] = 0;
            {   /* the path only when the region found starts here (anonymous regions report the next file) */
                struct proc_regionwithpathinfo rp;
                if (proc_pidinfo(getpid(), PROC_PIDREGIONPATHINFO, a, &rp, sizeof rp) == (int)sizeof rp &&
                    rp.prp_prinfo.pri_address == a && rp.prp_vip.vip_path[0])
                    snprintf(path, sizeof path, "%s", rp.prp_vip.vip_path);
            }
            nm = path[0] ? (strrchr(path, '/') ? strrchr(path, '/') + 1 : path) :
                 (a < 0xb8000000u ? "(anonymous, guest range)" : "(anonymous, native)");
            for (i = 0; i < ne; i++) if (!strcmp(e[i].name, nm)) break;
            if (i == ne && ne < 512) { snprintf(e[ne].name, sizeof e[ne].name, "%s", nm); e[ne].res = e[ne].dirty = e[ne].n = 0; ne++; }
            if (i < ne) { e[i].res += info.pages_resident; e[i].dirty += info.pages_dirtied; e[i].n++; }
            tres += info.pages_resident; tdirty += info.pages_dirtied;
            if (!path[0] && info.pages_dirtied >= 256)        /* anonymous regions of 1 MB dirty or more */
                fprintf(stderr, "[mem]   anon %08lx-%08lx %6.1f MB dirty (size %.1f MB, prot %d, tag %u, share %d)\n",
                        (unsigned long)a, (unsigned long)(a + sz), info.pages_dirtied / 256.0, sz / 1048576.0,
                        info.protection, info.user_tag, info.share_mode);
            if (a + sz < a) break;
            a += sz;
        }
    }
    if (getenv("OAH_DUMPBLOCKS")) {   /* the translator's 26 MB pool blocks, for offline analysis */
        extern unsigned int g_blk[8], g_nblk;
        unsigned int b;
        for (b = 0; b < g_nblk; b++) {
            char fn[64]; FILE *f;
            snprintf(fn, sizeof fn, "/tmp/oah_block%u_%d.bin", b, getpid());
            if ((f = fopen(fn, "wb"))) { fwrite((void *)g_blk[b], 1, 0x1a01000, f); fclose(f); }
            fprintf(stderr, "[mem] pool block %u at %08x -> %s\n", b, g_blk[b], fn);
        }
    }
    for (i = 0; i < ne; i++) for (j = i + 1; j < ne; j++) if (e[j].dirty > e[i].dirty) { ent_t t = e[i]; e[i] = e[j]; e[j] = t; }
    fprintf(stderr, "[mem] resident %lu MB, dirty %lu MB (4 KB pages); by mapping, most dirty first:\n", tres / 256, tdirty / 256);
    for (i = 0; i < ne && i < 40; i++)
        fprintf(stderr, "[mem] %8.1f MB dirty %8.1f MB resident %5lu regions  %s\n", e[i].dirty / 256.0, e[i].res / 256.0, e[i].n, e[i].name);
    return 0;
}
static void oah_memreport_start(void) {
    const char *s = getenv("OAH_MEMREPORT");
    pthread_t t;
    if (s && atoi(s) > 0 && !pthread_create(&t, 0, oah_memreport_thread, (void *)(long)atoi(s))) pthread_detach(t);
}
static void oah_fontworker_watch_start(int argc, char **argv) {
    int i, fw = 0;
    pthread_t t;
    if (getenv("OAH_NO_FWWATCH")) return;
    for (i = 0; i < argc && argv[i]; i++) if (strstr(argv[i], "/fontworker")) fw = 1;
    if (!fw) return;
    if (!pthread_create(&t, 0, oah_fontworker_watch, 0)) pthread_detach(t);
}
__attribute__((constructor))
static void init_oah_mojave(void) {
    struct sigaction sa;
    oah_root_init();
    oah_tr_fatal_hook();
    if (getenv("OAH_GLTEST")) oah_gltest("ctor");
    else if (oah_guest_wants_gl()) oah_glwarm();
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    if (getenv("OAH_BRIDGE_PROBE")) {
        struct sigaction sp;
        memset(&sp, 0, sizeof sp);
        sp.sa_sigaction = bridge_probe_handler;
        sp.sa_flags = SA_SIGINFO;
        sigaction(SIGTRAP, &sp, NULL);
        sigaction(SIGILL, &sp, NULL);
    } else if (!getenv("OAH_NO_BRIDGE")) {
        struct sigaction sb;
        memset(&sb, 0, sizeof sb);
        sb.sa_sigaction = bridge_trap_handler;
        sb.sa_flags = SA_SIGINFO | SA_NODEFER;  /* callbacks can trap again inside the handler */
        sigaction(SIGTRAP, &sb, NULL);
        g_trap_handler_installed = 1;
        if (!getenv("OAH_NO_FASTBRIDGE")) install_fast_bridge();
        if (!getenv("OAH_NO_SIGBLKFIX")) install_sigblk_fix();
        if (getenv("OAH_VERBOSE")) {
            struct sigaction sv;
            memset(&sv, 0, sizeof sv);
            sv.sa_sigaction = trap_report_handler;
            sv.sa_flags = SA_SIGINFO | SA_RESETHAND;
            sigaction(SIGILL, &sv, NULL);
        }
    } else if (getenv("OAH_VERBOSE")) {
        struct sigaction st;
        memset(&st, 0, sizeof st);
        st.sa_sigaction = trap_report_handler;
        st.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigaction(SIGTRAP, &st, NULL);
        sigaction(SIGILL, &st, NULL);
    }

    {
        struct sigaction sa2;
        memset(&sa2, 0, sizeof sa2);
        sa2.sa_sigaction = abrt_handler;
        sa2.sa_flags = SA_SIGINFO;
        sigaction(SIGABRT, &sa2, NULL);
    }
    if (!getenv("OAH_NO_BSPROXY")) start_bs_proxy();
    if (getenv("OAH_TASKBROKER")) oah_tb_server_start();
    if (getenv("OAH_NAMEBROKER")) oah_nb_server_start();
    if (!getenv("OAH_NO_NDR")) install_mach_hooks();
    /* The translator's Mach exception handler (0xb80b3ed6) calls a crash-report formatter
       (0xb80b3984) for faults when a report string is set.  In fontd that formatter itself
       dereferences NULL, so a harmless old-commpage read (0xffff8024) in the guest killed the
       process instead of reaching our SIGSEGV handler.  Skip the formatter: the handler then
       returns failure and the kernel delivers the signal as usual. */
    if (!getenv("OAH_NO_EXCREPORT_SKIP")) {
        unsigned char *c = (unsigned char *)0xb80b3f07;
        static const unsigned char want[5] = { 0xe8, 0x78, 0xfa, 0xff, 0xff };
        if (memcmp(c, want, 5) == 0 &&
            !vm_protect(mach_task_self(), 0xb80b3000, 0x1000, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) {
            memset(c, 0x90, 5);
            vm_protect(mach_task_self(), 0xb80b3000, 0x1000, 0, VM_PROT_READ | VM_PROT_EXECUTE);
            if (getenv("OAH_VERBOSE")) fprintf(stderr, "[exc] translator crash-report formatter skipped\n");
        } else if (getenv("OAH_VERBOSE")) fprintf(stderr, "[exc] crash-report patch site mismatch\n");
    }
    if (getenv("OAH_SCTRACE")) install_sc_hooks_with(oah_sc_hook, "gsc");
    else if (!getenv("OAH_NO_KEVFIX")) { install_sc_hooks_with(oah_kev_hook, getenv("OAH_VERBOSE") ? "kevfix" : NULL); g_sc_skip_ok = 1; }
    {   /* Launched from a wrapper's rlaunch front end (OAH_RLAUNCH = its pid, kept across the
           exec): rlaunch's Apple Event port died with the exec, and a sender that launched us
           (osascript "tell application ...") sends its event right away and got -600 until the
           guest UI came up seconds later. Register the native AE port now; the receiver queues
           events until the guest pulls them. */
        const char *rl = getenv("OAH_RLAUNCH");
        int child = getenv("OAH_RLAUNCH_CHILD") != 0;    /* spawned by a forwarding rlaunch */
        if (child) unsetenv("OAH_RLAUNCH_CHILD");
        if (((rl && atoi(rl) == getpid()) || child) && !getenv("OAH_NO_EARLY_AE")) {
            pthread_t t; pthread_create(&t, 0, oah_ae_early_thread, 0); pthread_detach(t);
        }
    }

    {
        char ***envp = _NSGetEnviron();
        if (envp && *envp) {
            char **e;
            for (e = *envp; *e; e++)
                if (strncmp(*e, "DYLD_INSERT_LIBRARIES=", 22) == 0) (*e)[0] = 'X';
        }
    }
    int *argcp = _NSGetArgc();
    char ***argvp = _NSGetArgv();
    if (argcp && argvp && *argvp) {
        int argc = *argcp;
        char **argv = *argvp;
        char **senv = argv + argc + 1;
        int envc = 0;
        while (senv[envc]) {
            if (strncmp(senv[envc], "OAH_DYLD_ROOT=", 14) == 0) {
                senv[envc] = g_dyld_root_env;
            } else if (strncmp(senv[envc], "OAH_DYLD_PRINT=", 15) == 0) {
                senv[envc] = "DYLD_PRINT_LIBRARIES=1";
            } else if (strncmp(senv[envc], "OAH_DYLD_SEG=", 13) == 0) {
                senv[envc] = "DYLD_PRINT_SEGMENTS=1";
            } else if (strncmp(senv[envc], "DYLD_INSERT_LIBRARIES=", 22) == 0) {
                /* hide the i386 shim from the PowerPC guest dyld: rename in place */
                senv[envc][0] = 'X';
            } else if (strncmp(senv[envc], "XYLD_", 5) == 0 &&
                       strncmp(senv[envc], "XYLD_INSERT_LIBRARIES=", 22) != 0) {
                /* guest-only dyld variable: XYLD_FOO -> DYLD_FOO after host dyld is done */
                senv[envc][0] = 'D';
                if (getenv("OAH_VERBOSE")) fprintf(stderr, "[oah_init] guest env: %s\n", senv[envc]);
            }
            envc++;
        }
        /* Find real apple[0] even if dyld left extra NULLs after senv[envc] */
        int aidx = envc + 1;
        while (aidx < envc + 8 && senv[aidx] == NULL) {
            aidx++;
        }
        char *raw_apple0 = senv[aidx];
        if (raw_apple0 && (uintptr_t)raw_apple0 > 0x80000000U) {
            if (strncmp(raw_apple0, "executable_path=", 16) == 0) {
                raw_apple0 += 16;
            }
            if (!realpath(raw_apple0, g_self_path)) {
                strlcpy(g_self_path, raw_apple0, sizeof(g_self_path));
            }
        } else if (argv[0]) {
            if (!realpath(argv[0], g_self_path)) {
                strlcpy(g_self_path, argv[0], sizeof(g_self_path));
            }
        }
        /* Put DYLD_ROOT_PATH in senv if dyld pruned DYLD_INSERT_LIBRARIES and left a NULL slot */
        if (aidx > envc + 1) {
            senv[envc] = g_dyld_root_env;
            senv[envc + 1] = NULL;
            envc++;
        }
        /* Ensure senv[envc + 1] (which translate reads as apple[0]) points to g_self_path */
        senv[envc + 1] = g_self_path;
        if (argv[0] && g_self_path[0]) {
            argv[0] = g_self_path;
        }
        if (getenv("OAH_VERBOSE")) {
            fprintf(stderr, "[oah_init] argc=%d envc=%d aidx=%d self=%s argv1=%s\n",
                    argc, envc, aidx, g_self_path, argc > 1 ? argv[1] : "NULL");
        }
    }

    vm_address_t af = 0xfffef000;
    if (vm_allocate(mach_task_self(), &af, 0x1000, VM_FLAGS_FIXED) == KERN_SUCCESS) {
        memcpy((void *)0xfffef000, cp_fef, 4096);
    }
    vm_address_t adf = 0xffdef000;
    if (vm_allocate(mach_task_self(), &adf, 0x1000, VM_FLAGS_FIXED) == KERN_SUCCESS) {
        memcpy((void *)0xffdef000, cp_fef, 4096);
        oah_cp_low_setup();
    } else if (getenv("OAH_VERBOSE")) fprintf(stderr, "[oah_init] could not map 0xffdef000\n");
    vm_address_t a3 = 0xffdf3000;
    if (vm_allocate(mach_task_self(), &a3, 0x1000, VM_FLAGS_FIXED) == KERN_SUCCESS) {
        memcpy((void *)0xffdf3000, cp_3000, 4096);
    }
    vm_address_t a8 = 0xffdf8000;
    if (vm_allocate(mach_task_self(), &a8, 0x2000, VM_FLAGS_FIXED) == KERN_SUCCESS) {
        memcpy((void *)0xffdf8000, cp_8000, 4096);
        memcpy((void *)0xffdf9000, cp_9000, 4096);
    }
    oah_install_region_hooks();
    {
        struct sigaction su;
        memset(&su, 0, sizeof su);
        su.sa_sigaction = usr1_handler;
        su.sa_flags = SA_SIGINFO;
        sigaction(SIGUSR1, &su, NULL);
    }
    oah_hang_init();
    oah_fontworker_watch_start(*_NSGetArgc(), *_NSGetArgv());
    oah_idle_exit_start();
    oah_memreport_start();
}

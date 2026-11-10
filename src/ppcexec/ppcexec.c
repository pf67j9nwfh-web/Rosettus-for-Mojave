/* libppcexec.dylib - lets native programs started from PowerPC apps run PowerPC programs.
   On Snow Leopard the kernel handed any PowerPC executable to Rosetta, so an installer's shell
   script could call a PowerPC helper tool (Adobe CS2's HFSTest). Mojave's kernel refuses
   ("Bad CPU type in executable"). Rosettus runs such scripts with a copy of bash that loads this
   library; its execve / posix_spawn of a PowerPC-only file start ROOT/rosettus instead.
   ROOT: $OAH_ROSETTUS_ROOT, else the folder above this library (ROOT/native/libppcexec.dylib). */
#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libkern/OSByteOrder.h>

/* 1 when this Mac's kernel runs no 32-bit code (macOS 10.15+, Darwin 19+): then a PowerPC program
   whose only Intel part is i386 (xACT's shntool, shorten: i386 + ppc) must go to Rosettus too
   ("Bad CPU type in executable" under Rosettus 2 on Catalina) */
#include <sys/utsname.h>
static int no_i386(void) {
    static int v = -1;
    if (v < 0) { struct utsname u; v = !uname(&u) && atoi(u.release) >= 19; }
    return v;
}
static int is_ppc_only(const char *path) {
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
            if (ct == 0x01000007 || (ct == 7 && !no_i386())) x86 = 1;
        }
        return ppc && !x86;
    }
    return 0;
}
static const char *script_path(void) {
    static char s[1200];
    if (!s[0]) {
        const char *r = getenv("OAH_ROSETTUS_ROOT");
        if (r) snprintf(s, sizeof s, "%s/rosettus", r);
        else {
            Dl_info di; char *sl;
            if (!dladdr((void *)is_ppc_only, &di) || !di.dli_fname) return 0;
            snprintf(s, sizeof s, "%s", di.dli_fname);
            if (!(sl = strrchr(s, '/'))) return 0; *sl = 0;          /* ROOT/native */
            if (!(sl = strrchr(s, '/'))) return 0; *sl = 0;          /* ROOT */
            strlcat(s, "/rosettus", sizeof s);
        }
    }
    return s;
}
static char **redirect(const char *path, char *const argv[]) {
    int n = 0, i; char **nv;
    while (argv && argv[n]) n++;
    if (!(nv = calloc(n + 3, sizeof *nv))) return 0;
    nv[0] = (char *)script_path(); nv[1] = (char *)path;
    for (i = 1; i < n; i++) nv[i + 1] = argv[i];
    return nv;
}
/* Children that would lose this library: /bin/sh and /bin/bash are SIP-protected (dyld drops
   DYLD_INSERT_LIBRARIES), so a script's sub-shell or a "#!/bin/sh" helper script ran PowerPC tools
   unaided. Run those with ROOT/native/bash instead. sudo (setuid, same problem) in a process that
   is already root - package scripts under installd: "sudo MakeAnAlias ..." - runs its command
   directly. Returns a new argv and sets *np to the program to run, or 0 to leave the call alone. */
static const char *native_bash(void) {
    static char b[1200];
    const char *s = script_path(), *sl;
    if (!b[0] && s && (sl = strrchr(s, '/'))) snprintf(b, sizeof b, "%.*s/native/bash", (int)(sl - s), s);
    return b[0] ? b : 0;
}
static char **sh_redirect(const char *path, char *const argv[], const char **np) {
    int n = 0, i, k; char **nv; char h[256]; const char *nb = native_bash();
    while (argv && argv[n]) n++;
    if (!nb || access(nb, X_OK)) return 0;
    if (!strcmp(path, "/bin/sh") || !strcmp(path, "/bin/bash")) {
        if (!(nv = calloc(n + 2, sizeof *nv))) return 0;
        for (i = 0; i < n; i++) nv[i] = argv[i];
        if (!n) nv[0] = (char *)(strcmp(path, "/bin/sh") ? "bash" : "sh");
        *np = nb; return nv;
    }
    if (!strcmp(path, "/usr/sbin/installer") && !getenv("OAH_IN_PKGSHIM") && !getenv("OAH_NO_PKGSHIM")) {
        /* a package script installing a nested package (InDesign CS2's LocalizedContent.pkg):
           that package's scripts need PowerPC tools too - go through ROOT/native/installer-shim */
        static char shim[1200]; const char *sl = strrchr(nb, '/');
        snprintf(shim, sizeof shim, "%.*s/installer-shim", (int)(sl - nb), nb);
        if (access(shim, R_OK)) return 0;
        if (!(nv = calloc(n + 3, sizeof *nv))) return 0;
        nv[0] = "bash"; nv[1] = shim;
        for (i = 1; i < n; i++) nv[i + 1] = argv[i];
        *np = nb; return nv;
    }
    if (geteuid() == 0 && !strcmp(path, "/usr/bin/sudo")) {
        for (k = 1; k < n && argv[k][0] == '-'; k++)          /* options; -u/-g/-C/-p/-U/-h take a value */
            if (argv[k][1] && argv[k][2] == 0 && strchr("ugCpUh", argv[k][1]) && k + 1 < n) k++;
        if (k < n && argv[k][0] == '/' && is_ppc_only(argv[k])) {
            if (!(nv = calloc(n + 3, sizeof *nv))) return 0;
            nv[0] = (char *)script_path(); nv[1] = argv[k]; i = 2;
            for (k++; k < n; k++) nv[i++] = argv[k];
            *np = nv[0]; return nv;
        }
        return 0;
    }
    {   int fd = open(path, O_RDONLY), m, L; char *e;
        if (fd < 0) return 0;
        m = (int)read(fd, h, sizeof h - 1); close(fd);
        if (m < 4 || h[0] != '#' || h[1] != '!') return 0;
        h[m] = 0; if ((e = strchr(h, '\n'))) *e = 0;
        for (e = h + 2; *e == ' ' || *e == '\t'; e++) ;
        if (!strncmp(e, "/bin/sh", 7)) L = 7; else if (!strncmp(e, "/bin/bash", 9)) L = 9; else return 0;
        if (e[L] && e[L] != ' ' && e[L] != '\t' && e[L] != '\r') return 0;
        if (!(nv = calloc(n + 3, sizeof *nv))) return 0;
        nv[0] = (char *)(L == 9 ? "bash" : "sh"); nv[1] = (char *)path;
        for (i = 1; i < n; i++) nv[i + 1] = argv[i];
        *np = nb; return nv;
    }
}
/* The PowerPC program started here is a program of its own: drop the identity of the app whose
   script started it (OAH_GUEST_EXE, CFProcessPath), as liboah does for a direct PowerPC exec. The
   rosettus script sets them for the new program. With the parent's OAH_GUEST_EXE, Wireshark's
   dumpcap passed for its app's main executable and wrote Rosettus diagnostics into the stderr
   pipe Wireshark parses ("Unknown message from dumpcap"). */
/* Also: the rosettus script runs under SIP-protected /bin/bash, which drops DYLD_* - the library
   paths a script set for its PowerPC program (Wireshark: DYLD_LIBRARY_PATH=Resources/lib, else
   "Library not loaded: /usr/local/lib/libwiretap.0.dylib"). Pass them as OAH_CALLER_DYLD_*, which
   the script hands to the guest dyld. (The strings are leaked: the process execs or spawns next.) */
static char **own_env(char *const envp[]) {
    static const char *keep[] = { "DYLD_LIBRARY_PATH=", "DYLD_FRAMEWORK_PATH=", "DYLD_FALLBACK_LIBRARY_PATH=", "DYLD_FALLBACK_FRAMEWORK_PATH=" };
    int n = 0, i, j, k; char **ne;
    while (envp && envp[n]) n++;
    if (!(ne = calloc(2 * n + 1, sizeof *ne))) return 0;
    for (i = 0, j = 0; i < n; i++) {
        if (!strncmp(envp[i], "OAH_GUEST_EXE=", 14) || !strncmp(envp[i], "CFProcessPath=", 14)) continue;
        if (!strncmp(envp[i], "OAH_CALLER_DYLD_", 16)) continue;          /* replaced below when set */
        ne[j++] = envp[i];
        for (k = 0; k < 4; k++) if (!strncmp(envp[i], keep[k], strlen(keep[k]))) {
            size_t L = strlen(envp[i]) + 12; char *c = malloc(L);
            if (c) { snprintf(c, L, "OAH_CALLER_%s", envp[i]); ne[j++] = c; }
        }
    }
    return ne;
}
static int my_execve(const char *path, char *const argv[], char *const envp[]) {
    if (path && script_path() && is_ppc_only(path)) {
        char **nv = redirect(path, argv), **ne = own_env(envp);
        if (nv) { execve(nv[0], nv, ne ? ne : envp); free(nv); }
        free(ne);
    } else if (path && script_path()) {
        const char *np = 0; char **nv = sh_redirect(path, argv, &np);
        if (nv) { execve(np, nv, envp); free(nv); }
    }
    return execve(path, argv, envp);
}
static int my_posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa,
                          const posix_spawnattr_t *at, char *const argv[], char *const envp[]) {
    if (path && script_path() && is_ppc_only(path)) {
        char **nv = redirect(path, argv), **ne = own_env(envp); int r;
        if (nv) { r = posix_spawn(pid, nv[0], fa, at, nv, ne ? ne : envp); free(nv); free(ne); return r; }
        free(ne);
    } else if (path && script_path()) {
        const char *np = 0; char **nv = sh_redirect(path, argv, &np); int r;
        if (nv) { r = posix_spawn(pid, np, fa, at, nv, envp); free(nv); return r; }
    }
    return posix_spawn(pid, path, fa, at, argv, envp);
}
typedef struct { const void *replacement, *replacee; } interpose_t;
__attribute__((used)) static const interpose_t interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)my_execve, (const void *)execve },
    { (const void *)my_posix_spawn, (const void *)posix_spawn },
};

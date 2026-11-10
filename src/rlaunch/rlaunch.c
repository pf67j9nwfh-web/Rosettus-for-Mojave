/* rlaunch: native front end of a Rosetta wrapper app (made by mkwrapper.sh).
   LaunchServices hands a launched app its documents in the launch Apple Event (odoc/pdoc), which
   arrives before the PowerPC app could ever register for it. Catch it here (up to ~3 s), pass the
   files on in OAH_OPEN_DOCS / OAH_PRINT_DOCS (newline-separated paths), then exec launcher.sh.

   A script that launched us ("tell application ..." while the app is not running) sends its
   event right after the launch event, to this process. Mach port rights do not survive exec, so
   that event would hit a dead port (-600). So after the launch event, wait ~0.7 s more: if another
   event comes, switch to forwarding: hide from the Dock, spawn launcher.sh as a child, and relay
   each event (suspended here) to the child once its own port is up, copying the reply back. Exit
   when the child is gone or after 15 s without events. */
#include <Carbon/Carbon.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <libproc.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
extern char **environ;
static char g_docs[65536], g_print[65536];
static int g_got, g_fwd_ok;
#define NQ 64
static AppleEvent g_qev[NQ], g_qrep[NQ];
static int g_nq;
static FILE *g_log;
static void lg(const char *fmt, ...) {
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fflush(g_log);
}
static void add_files(const AppleEvent *ev, char *out) {
    AEDescList list;
    long n = 0, i;
    if (AEGetParamDesc(ev, keyDirectObject, typeAEList, &list)) return;
    AECountItems(&list, &n);
    for (i = 1; i <= n; i++) {
        AEKeyword kw; DescType t; Size sz; char url[4096];
        if (AEGetNthPtr(&list, i, typeFileURL, &kw, &t, url, sizeof url - 1, &sz)) continue;
        url[sz] = 0;
        {
            CFURLRef u = CFURLCreateWithBytes(NULL, (const UInt8 *)url, sz, kCFStringEncodingUTF8, NULL);
            char path[4096];
            if (u && CFURLGetFileSystemRepresentation(u, true, (UInt8 *)path, sizeof path) &&
                strlen(out) + strlen(path) + 2 < sizeof g_docs) {
                strcat(out, path); strcat(out, "\n");
            }
            if (u) CFRelease(u);
        }
    }
    AEDisposeDesc(&list);
}
static void note_sender(const AppleEvent *ev);
static OSErr odoc(const AppleEvent *ev, AppleEvent *reply, SRefCon r) { (void)reply; note_sender(ev); add_files(ev, r ? g_print : g_docs); g_got = 1; return noErr; }
static char g_sender[256];
static void note_sender(const AppleEvent *ev) {
    SInt32 pid = 0; DescType t; Size sz;
    if (AEGetAttributePtr(ev, keySenderPIDAttr, typeSInt32, &t, &pid, sizeof pid, &sz) == noErr && pid > 0) {
        char cmd[128]; FILE *p;
        snprintf(cmd, sizeof cmd, "/bin/ps -o comm= -p %d", (int)pid);
        if ((p = popen(cmd, "r"))) { if (!fgets(g_sender, sizeof g_sender, p)) g_sender[0] = 0; pclose(p); }
        g_sender[strcspn(g_sender, "\n")] = 0;
    }
}
static OSErr oapp(const AppleEvent *ev, AppleEvent *reply, SRefCon r) { (void)reply; (void)r; note_sender(ev); g_got = 1; return noErr; }
/* any other event: keep it for the real app */
static OSErr later(const AppleEvent *ev, AppleEvent *reply, SRefCon r) {
    DescType t; Size sz; OSType cls = 0, id = 0;
    (void)r;
    AEGetAttributePtr(ev, keyEventClassAttr, typeType, &t, &cls, sizeof cls, &sz);
    AEGetAttributePtr(ev, keyEventIDAttr, typeType, &t, &id, sizeof id, &sz);
    lg("  event %.4s/%.4s\n", (char *)&(OSType){ OSSwapHostToBigInt32(cls) }, (char *)&(OSType){ OSSwapHostToBigInt32(id) });
    /* Get URL with a file URL (Mojave's LaunchServices opens an .html file in a browser that claims
       http this way): take it as a document. Queued, it put us in forwarding mode, and Internet
       Explorer - seeing this launcher, which carries its signature, as a copy of itself already
       running - quit at once. */
    if (cls == 'GURL' && id == 'GURL') {
        char url[4096] = "";
        if (!AEGetParamPtr(ev, keyDirectObject, typeUTF8Text, &t, url, sizeof url - 1, &sz)) {
            url[sz] = 0;
            if (!strncmp(url, "file://", 7)) {
                CFURLRef u = CFURLCreateWithBytes(NULL, (const UInt8 *)url, sz, kCFStringEncodingUTF8, NULL);
                char path[4096];
                if (u && CFURLGetFileSystemRepresentation(u, true, (UInt8 *)path, sizeof path) &&
                    strlen(g_docs) + strlen(path) + 2 < sizeof g_docs) {
                    strcat(g_docs, path); strcat(g_docs, "\n");
                    if (u) CFRelease(u);
                    note_sender(ev); g_got = 1;
                    return noErr;
                }
                if (u) CFRelease(u);
            }
        }
    }
    if (g_nq >= NQ) return errAEEventNotHandled;
    if (AESuspendTheCurrentEvent(ev)) return errAEEventNotHandled;
    g_qev[g_nq] = *ev; g_qrep[g_nq] = *reply; g_nq++;
    return noErr;
}
static void pump(double secs) {
    EventRef e;
    if (ReceiveNextEvent(0, NULL, secs / 2, true, &e) == noErr) {
        if (GetEventClass(e) == kEventClassAppleEvent) AEProcessEvent(e);
        else SendEventToEventTarget(e, GetEventDispatcherTarget());
        ReleaseEvent(e);
    }
    /* Apple Events sent once we are "running" come through the AE port's run loop source,
       which ReceiveNextEvent's private mode does not service */
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, secs / 2, true);
}
static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }
static int child_alive(pid_t c) { int st; return waitpid(c, &st, WNOHANG) == 0; }
/* send one suspended event to the child, retrying until its port exists; copy the reply back */
static void forward(pid_t child, AppleEvent *ev, AppleEvent *reply) {
    AEAddressDesc target; AppleEvent dup, r; OSErr e = -600; double t0 = now();
    AECreateDesc(typeKernelProcessID, &child, sizeof child, &target);
    while (now() - t0 < 120 && child_alive(child)) {
        if (AEDuplicateDesc(ev, &dup)) break;
        AEPutAttributeDesc(&dup, keyAddressAttr, &target);
        AEInitializeDesc(&r);
        e = AESendMessage(&dup, &r, kAEWaitReply | kAECanInteract, kNoTimeOut);
        AEDisposeDesc(&dup);
        if (e != procNotFound && e != connectionInvalid && e != errAEEventNotHandled) break;
        AEDisposeDesc(&r);
        pump(0.3);   /* keep queueing new events meanwhile */
    }
    lg("  forwarded: err %d after %.1f s\n", (int)e, now() - t0);
    if (!e) {
        long n = 0, i;
        g_fwd_ok = 1;
        AECountItems(&r, &n);
        for (i = 1; i <= n; i++) {
            AEKeyword kw; AEDesc d;
            if (AEGetNthDesc(&r, i, typeWildCard, &kw, &d)) continue;
            if (reply->descriptorType != typeNull) AEPutParamDesc(reply, kw, &d);
            AEDisposeDesc(&d);
        }
        AEDisposeDesc(&r);
    } else if (reply->descriptorType != typeNull) {
        SInt32 en = e;
        AEPutParamPtr(reply, keyErrorNumber, typeSInt32, &en, sizeof en);
    }
    AEResumeTheCurrentEvent(ev, reply, (AEEventHandlerUPP)kAENoDispatch, 0);
    AEDisposeDesc(&target);
}

/* ---- one copy per app ----
   The same PowerPC app can be reachable through more than one launcher (a launcher in
   /Applications/Rosettus and one in ~/Applications/Rosettus, or a launcher and the patched app).
   LaunchServices then does not see the running copy as "this app" and starts us again; without a
   check that would start a second copy of the PowerPC app. So: if a PowerPC process of this user
   is already running the same guest executable, pass the documents on to it (or ask it to reopen),
   bring it to the front and exit. OAH_RLAUNCH_MULTI=1 turns this off. */
static int same_guest(const char *arg, const char *guest) {
    char r[4096]; size_t n = strlen(guest);
    if (arg[0] != '/' || !realpath(arg, r)) return 0;
    return !strncmp(r, guest, n) && (r[n] == 0 || r[n] == '/');
}
static pid_t find_running(const char *guest) {
    static pid_t pids[8192];
    int np, i, mib[3] = { CTL_KERN, KERN_PROCARGS2, 0 }, amax = 0;
    size_t sz = sizeof amax; int m2[2] = { CTL_KERN, KERN_ARGMAX };
    char *buf; uid_t me = getuid();
    if (!guest[0]) return 0;
    if (sysctl(m2, 2, &amax, &sz, 0, 0) || amax <= 0) amax = 262144;
    if (!(buf = malloc(amax))) return 0;
    np = proc_listallpids(pids, sizeof pids) ;
    for (i = 0; i < np; i++) {
        struct proc_bsdinfo bi; int argc, k; char *q, *end; size_t len = amax;
        if (pids[i] <= 0 || pids[i] == getpid()) continue;
        if (proc_pidinfo(pids[i], PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != sizeof bi) continue;
        if (bi.pbi_uid != me || (bi.pbi_status != SRUN && bi.pbi_status != SSLEEP)) continue;
        mib[2] = pids[i];
        if (sysctl(mib, 3, buf, &len, 0, 0) || len < sizeof argc) continue;
        memcpy(&argc, buf, sizeof argc); end = buf + len;
        q = buf + sizeof argc; q += strnlen(q, end - q);            /* exec path */
        while (q < end && !*q) q++;
        if (argc < 2) continue;
        {   const char *a0 = q, *b = strrchr(a0, '/'); b = b ? b + 1 : a0;
            if (!strstr(b, "translate") && strcmp(b, "rhost")) continue; }   /* a PowerPC process (translate, .rosetta-translate; rhost under Rosettus 2) */
        for (k = 0; k < argc && q < end; k++) {
            if (k > 0 && same_guest(q, guest)) { free(buf); return pids[i]; }
            q += strnlen(q, end - q) + 1;
        }
    }
    free(buf);
    return 0;
}
static int hand_to_running(const char *guest) {
    pid_t pid; AEAddressDesc target; AppleEvent ev, rep; OSErr e; ProcessSerialNumber psn;
    if (getenv("OAH_RLAUNCH_MULTI") || !(pid = find_running(guest))) return 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    /* Bring it forward before the events: this launcher is the front process meanwhile, and
       keys typed then went to it (a Cmd-H right after `open` did nothing). A hidden app is shown
       the way the Dock shows it (a reopen alone left its windows hidden). */
    if (!GetProcessForPID(pid, &psn)) {
        if (!IsProcessVisible(&psn)) ShowHideProcess(&psn, true);
        SetFrontProcess(&psn);
    }
#pragma clang diagnostic pop
    AECreateDesc(typeKernelProcessID, &pid, sizeof pid, &target);
    if (g_docs[0] || g_print[0]) {
        /* one event per file, direct object a single typeFileURL "file:///path" (what the guest's
           launch-document path sends too: a list of 'furl' items hangs or crashes 10.6 apps) */
        char *docs = g_docs[0] ? g_docs : g_print, *line, *save = 0, tmp[65536];
        snprintf(tmp, sizeof tmp, "%s", docs);
        e = 0;
        for (line = strtok_r(tmp, "\n", &save); line; line = strtok_r(0, "\n", &save)) {
            CFURLRef u = CFURLCreateFromFileSystemRepresentation(0, (const UInt8 *)line, strlen(line), false);
            char b[4096];
            if (!u) continue;
            if (CFStringGetCString(CFURLGetString(u), b, sizeof b, kCFStringEncodingUTF8)) {
                if (!strncmp(b, "file://localhost/", 17)) memmove(b + 7, b + 16, strlen(b + 16) + 1);
                AECreateAppleEvent(kCoreEventClass, g_docs[0] ? kAEOpenDocuments : kAEPrintDocuments, &target,
                                   kAutoGenerateReturnID, kAnyTransactionID, &ev);
                AEPutParamPtr(&ev, keyDirectObject, typeFileURL, b, strlen(b));
                AEInitializeDesc(&rep);
                e = AESendMessage(&ev, &rep, kAEWaitReply | kAECanInteract, 10 * 60);
                AEDisposeDesc(&ev); AEDisposeDesc(&rep);
            }
            CFRelease(u);
        }
    } else {
        AECreateAppleEvent(kCoreEventClass, kAEReopenApplication, &target, kAutoGenerateReturnID, kAnyTransactionID, &ev);
        AEInitializeDesc(&rep);
        e = AESendMessage(&ev, &rep, kAENoReply | kAECanInteract, 10 * 60);   /* no need to wait: we exit */
        AEDisposeDesc(&ev); AEDisposeDesc(&rep);
    }
    AEDisposeDesc(&target);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (!GetProcessForPID(pid, &psn)) SetFrontProcess(&psn);
#pragma clang diagnostic pop
    lg("  already running as pid %d (%s): handed over, err %d\n", (int)pid, guest, (int)e);
    return 1;
}
static void guest_of_wrapper(char *out, size_t n) {
    CFTypeRef v = CFBundleGetValueForInfoDictionaryKey(CFBundleGetMainBundle(), CFSTR("RosettaGuestApp"));
    char p[4096];
    out[0] = 0;
    if (v && CFGetTypeID(v) == CFStringGetTypeID() && CFStringGetFileSystemRepresentation(v, p, sizeof p))
        if (!realpath(p, out)) snprintf(out, n, "%s", p);
}
/* ---- rstub: the x86_64 slice added to a PowerPC app's own executable ("patched" app) ----
   Patching makes the app's main executable fat: its PowerPC code plus this native slice. Mojave runs
   this slice when the app is opened from the Finder or the Dock; it passes the launch documents on
   (see rlaunch above) and hands the bundle to the Rosetta launcher script, which runs the PowerPC
   slice of the very same file. The app keeps its name, icon, bundle id and document types.

   Install root: $ROSETTUS_ROOT, ~/Library/Rosettus, /Library/Rosettus (first that has the
   "rosettus" launcher). Settings: Contents/Resources/rosetta.env inside the app if present
   (lines "export NAME=value"), else OAH_GTB=1. Log: ~/Library/Logs/Rosettus/<app>.log. */
static char g_stub_script[1200];   /* ROOT/rosettus */
static int stub_root(char *out, size_t n) {
    const char *h = getenv("HOME"), *e = getenv("ROSETTUS_ROOT");
    char c[3][1024]; int i; struct stat sb;
    snprintf(c[0], sizeof c[0], "%s", e ? e : "");
    snprintf(c[1], sizeof c[1], "%s/Library/Rosettus", h ? h : "");
    snprintf(c[2], sizeof c[2], "/Library/Rosettus");
    for (i = 0; i < 3; i++) {
        if (!c[i][0]) continue;
        snprintf(g_stub_script, sizeof g_stub_script, "%s/rosettus", c[i]);
        if (!stat(g_stub_script, &sb)) { snprintf(out, n, "%s", c[i]); return 1; }
    }
    g_stub_script[0] = 0;
    return 0;
}
static void stub_env_file(const char *bundle) {
    char p[4096], ln[2048]; FILE *f;
    int any = 0;
    snprintf(p, sizeof p, "%s/Contents/Resources/rosetta.env", bundle);
    if ((f = fopen(p, "r"))) {
        while (fgets(ln, sizeof ln, f)) {
            char *s = ln, *eq;
            ln[strcspn(ln, "\r\n")] = 0;
            if (!strncmp(s, "export ", 7)) s += 7;
            if (!(eq = strchr(s, '=')) || *s == '#') continue;
            *eq = 0; setenv(s, eq + 1, 1); any = 1;
        }
        fclose(f);
    }
    if (!any && !getenv("OAH_GTB")) setenv("OAH_GTB", "1", 1);
}
static int stub_exec(const char *self) {
    char bundle[4096], root[1024], script[1200], logp[1200], name[512], link[4200], *s;
    const char *h = getenv("HOME");
    int fd;
    snprintf(bundle, sizeof bundle, "%s", self);
    if (!(s = strstr(bundle, ".app/Contents/MacOS/"))) return 0;
    s[4] = 0;                                         /* .../Name.app */
    if (!stub_root(root, sizeof root)) {
        CFUserNotificationDisplayAlert(0, kCFUserNotificationStopAlertLevel, 0, 0, 0,
            CFSTR("Rosettus is not installed"),
            CFSTR("This PowerPC application was patched to run with Rosettus, but no Rosettus installation was found (/Library/Rosettus or ~/Library/Rosettus)."),
            CFSTR("OK"), 0, 0, 0);
        return 1;
    }
    {   char rb[4096]; if (realpath(bundle, rb) && hand_to_running(rb)) return 0; }
    stub_env_file(bundle);
    snprintf(name, sizeof name, "%s", strrchr(bundle, '/') + 1);
    name[strlen(name) - 4] = 0;
    snprintf(link, sizeof link, "%s/Contents/MacOS/.rosetta-translate", bundle);
    setenv("OAH_TRANSLATE_LINK", link, 1);
    snprintf(logp, sizeof logp, "%s/Library/Logs/Rosettus", h ? h : "/tmp");
    mkdir(logp, 0755);
    snprintf(logp, sizeof logp, "%s/Library/Logs/Rosettus/%s.log", h ? h : "/tmp", name);
    {   char old[1300]; snprintf(old, sizeof old, "%s.old", logp); rename(logp, old); }
    if ((fd = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
    snprintf(script, sizeof script, "%s", g_stub_script);
    if (chdir(root)) {}
    execl("/bin/bash", "bash", script, bundle, (char *)0);
    perror("exec rosetta");
    return 1;
}

int main(int argc, char **argv) {
    char self[4096], script[4096];
    int i;
    (void)argc;
    {   char lp[1024]; const char *h = getenv("HOME");
        snprintf(lp, sizeof lp, "%s/Library/Logs/Rosettus/rlaunch.log", h ? h : "/tmp");
        g_log = fopen(lp, "a"); }
    AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments, NewAEEventHandlerUPP(odoc), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEPrintDocuments, NewAEEventHandlerUPP(odoc), (SRefCon)1, false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenApplication, NewAEEventHandlerUPP(oapp), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEReopenApplication, NewAEEventHandlerUPP(oapp), 0, false);
    AEInstallEventHandler(typeWildCard, typeWildCard, NewAEEventHandlerUPP(later), 0, false);
    for (i = 0; i < 30 && !g_got && !g_nq; i++) pump(0.1);
    if (getenv("OAH_RLAUNCH_WAIT")) {
        /* Experimental (off by default): wait for a launching script's first event and forward
           it. On Mojave osascript's event was never delivered to this process while it waited
           (it failed with -609 the moment we exec'd), so this does not help yet. */
        double t0 = now(), w = atof(getenv("OAH_RLAUNCH_WAIT"));
        while (now() - t0 < w && !g_nq) pump(0.1);
    }
    lg("%s: event=%d after %d polls docs=[%s] print=[%s] sender=[%s] queued=%d\n", argv[0], g_got, i, g_docs, g_print, g_sender, g_nq);
    if (g_docs[0]) setenv("OAH_OPEN_DOCS", g_docs, 1);
    if (g_print[0]) setenv("OAH_PRINT_DOCS", g_print, 1);
    {   uint32_t sz = sizeof self; char tmp[4096];
        if (_NSGetExecutablePath(tmp, &sz) || !realpath(tmp, self)) snprintf(self, sizeof self, "%s", argv[0]); }
    {   char d[4096]; struct stat sb; snprintf(d, sizeof d, "%s", self);
        snprintf(script, sizeof script, "%s/launcher.sh", dirname(d));
        if (stat(script, &sb)) return stub_exec(self);   /* patched app: no wrapper script next to us */ }
    if (!g_nq) {   char gp[4096]; guest_of_wrapper(gp, sizeof gp); if (hand_to_running(gp)) return 0; }
    if (!g_nq) {
        char pb[16]; snprintf(pb, sizeof pb, "%d", (int)getpid()); setenv("OAH_RLAUNCH", pb, 1);   /* early AE check-in */
        execl("/bin/bash", "bash", script, (char *)0);
        perror("exec launcher.sh");
        return 1;
    }
    {   /* forwarding mode */
        ProcessSerialNumber me = { 0, kCurrentProcess };
        pid_t child = 0; char *cargv[] = { "bash", script, 0 }; double idle;
        TransformProcessType(&me, 4 /* kProcessTransformToUIElementApplication */);
        setenv("OAH_RLAUNCH_CHILD", "1", 1);
        {   char fp[16]; snprintf(fp, sizeof fp, "%d", (int)getpid()); setenv("OAH_RLAUNCH_FWDPID", fp, 1); }   /* liboah hides us from GetNextProcess */
        if (posix_spawn(&child, "/bin/bash", 0, 0, cargv, environ)) { lg("  spawn failed\n"); return 1; }
        lg("  forwarding to child %d\n", (int)child);
        idle = now();
        while (child_alive(child)) {
            if (g_nq) {
                AppleEvent ev = g_qev[0], rep = g_qrep[0];
                memmove(g_qev, g_qev + 1, (g_nq - 1) * sizeof g_qev[0]);
                memmove(g_qrep, g_qrep + 1, (g_nq - 1) * sizeof g_qrep[0]);
                g_nq--;
                forward(child, &ev, &rep);
                idle = now();
                continue;
            }
            if (g_fwd_ok && now() - idle > 15) break;
            pump(0.25);
        }
        lg("  forwarder done (child %s)\n", child_alive(child) ? "running" : "gone");
    }
    return 0;
}

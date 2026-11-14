/* oah_sfl.h - LSSharedFileList (open/save panel sidebar) from Mojave's own lists.

   The 10.6 guest LaunchServices keeps shared file lists in coreservicesd's "SFLServer", which the
   guest never reaches here: LSSharedFileListCreate returned NULL and every open/save panel had an
   empty sidebar (no Desktop / Documents / Applications, no volumes). The lists now come from
   Mojave's LaunchServices:
     - lists and items handed to the guest are small guest CFData objects (unique contents, so
       CFEqual/CFHash tell them apart; CFRetain/CFRelease work); a native table maps them back;
     - names/properties become guest CF objects (oah_cf_to_guest), URLs/FSRefs are rebuilt with the
       guest's own CFURL / File Manager, icons with the guest IconServices (GetIconRefFromFileInfo),
       alias data with the guest Alias Manager (FSNewAlias), so they match the guest's volumes;
     - observers are accepted and never called; changes the guest makes (insert / move / remove /
       set) are NOT written to Mojave's lists unless OAH_SFL_RW=1 - a 10.6 Finder-era framework
       must not rewrite the user's real sidebar. The app's own recent-documents list
       (kLSSharedFileListApplicationRecentDocuments: 10.6 AppKit's File > Open Recent) is the exception:
       it belongs to the app alone (Mojave keeps one per bundle identifier), so it is created natively
       and the app's inserts and removals go to it; it was NULL before and Open Recent forgot every
       document at quit (OAH_NO_APPRECENT=1: as before).
   Lists other than the global favorites / volumes / saved searches / recents stay unavailable (NULL).
   OAH_NO_SFL=1 disables (the guest functions answer as before). OAH_SFLTRACE=1 logs. */
#define OAH_SFL_MAX 2048
typedef struct { unsigned int g; void *n; unsigned int list; unsigned int id; int kind; } oah_sfl_t;   /* kind 1 list, 2 item */
static oah_sfl_t g_sfl[OAH_SFL_MAX];
static int g_sfl_n;
static unsigned int g_sfl_serial = 0x5f4c0000u;
static int oah_sfltr(void) { static int t = -1; if (t < 0) t = getenv("OAH_SFLTRACE") ? 1 : 0; return t; }
static oah_sfl_t *oah_sfl_find(unsigned int g) {
    int i;
    if (!g) return 0;
    /* newest first: a guest object the guest released can be freed and its address handed out
       again for a new wrapper (Transmission's save panel: an item address came back as a list,
       and the item lookup resolved the native list - SFLGenericList -bookmark, uncaught) */
    for (i = g_sfl_n - 1; i >= 0; i--) if (g_sfl[i].g == g) return &g_sfl[i];
    return 0;
}
static unsigned int oah_sfl_wrap(void *n, int kind, unsigned int list, unsigned int id) {
    static unsigned int fdc;
    unsigned int a[3], bytes[2], g; int i;
    if (kind == 2)
        for (i = 0; i < g_sfl_n; i++)
            if (g_sfl[i].kind == 2 && g_sfl[i].list == list && g_sfl[i].id == id) {
                if (g_sfl[i].n != n) { CFRetain(n); if (g_sfl[i].n) CFRelease(g_sfl[i].n); g_sfl[i].n = n; }
                return g_sfl[i].g;
            }
    if (g_sfl_n >= OAH_SFL_MAX) return 0;
    if (!fdc) fdc = oah_guest_sym("CoreFoundation", "_CFDataCreate");
    if (!fdc) return 0;
    bytes[0] = OSSwapHostToBigInt32(++g_sfl_serial); bytes[1] = OSSwapHostToBigInt32(id);
    a[0] = 0; a[1] = (unsigned int)bytes; a[2] = 8;
    if (!(g = oah_callppc_n("sfl_wrap", fdc, 3, a))) return 0;
    {   /* keep a reference of our own: item wrappers are handed out again (same list and id) after
           the guest released its copy, and a freed one was reused by the guest for other objects */
        static unsigned int frt;
        if (!frt) frt = oah_guest_sym("CoreFoundation", "_CFRetain");
        if (frt && !getenv("OAH_SFL_NOKEEP")) { a[0] = g; oah_callppc_n("sfl_keep", frt, 1, a); }
    }
    for (i = 0; i < g_sfl_n; i++)                      /* an older wrapper at this address was freed */
        if (g_sfl[i].g == g) { if (g_sfl[i].n) CFRelease(g_sfl[i].n); g_sfl[i].n = NULL; g_sfl[i].g = 0; g_sfl[i].kind = 0; }
    CFRetain(n);
    g_sfl[g_sfl_n].g = g; g_sfl[g_sfl_n].n = n; g_sfl[g_sfl_n].kind = kind; g_sfl[g_sfl_n].list = list; g_sfl[g_sfl_n].id = id;
    g_sfl_n++;
    return g;
}
static void *oah_sfl_nsym(const char *name) { return oah_nls(name); }
/* guest special item constant (kLSSharedFileListItemBeforeFirst / Last) -> native one */
static void *oah_sfl_after(unsigned int g) {
    static unsigned int gfirst, glast; static void **nfirst, **nlast;
    oah_sfl_t *e;
    if (!gfirst) {
        unsigned int s = oah_guest_sym("LaunchServices", "_kLSSharedFileListItemBeforeFirst"), t = oah_guest_sym("LaunchServices", "_kLSSharedFileListItemLast");
        gfirst = s ? OSSwapBigToHostInt32(*(unsigned int *)s) : 1; glast = t ? OSSwapBigToHostInt32(*(unsigned int *)t) : 1;
        nfirst = (void **)oah_sfl_nsym("kLSSharedFileListItemBeforeFirst"); nlast = (void **)oah_sfl_nsym("kLSSharedFileListItemLast");
    }
    if (g == gfirst && nfirst) return *nfirst;
    if (g == glast && nlast) return *nlast;
    if ((e = oah_sfl_find(g)) && e->kind == 2) return e->n;
    return nlast ? *nlast : 0;
}
/* native item -> POSIX path of its target (no UI, no mounting) */
static int oah_sfl_item_path(void *item, char *path, size_t cap) {
    static OSStatus (*res)(void *, UInt32, CFURLRef *, void *);
    CFURLRef u = NULL; int ok = 0;
    if (!res) res = (OSStatus (*)(void *, UInt32, CFURLRef *, void *))oah_sfl_nsym("LSSharedFileListItemResolve");
    if (!res || res(item, 1 | 2 /* NoUI | DoNotMountVolumes */, &u, NULL) || !u) return 0;
    ok = CFURLGetFileSystemRepresentation(u, true, (UInt8 *)path, (CFIndex)cap) && path[0] == '/' ? 1 : 0;
    /* Mojave's favorite volumes include items with no file path (network, cloud): "not found", as for
       a missing item; Blender's file browser passed the NULL URL on to CFURLCopyFileSystemPath */
    CFRelease(u);
    return ok;
}
static int oah_sfl_guest_fsref(const char *path, unsigned char *fsref80) {
    static unsigned int fpm; unsigned int a[3];
    if (!fpm) fpm = oah_guest_sym("CarbonCore", "_FSPathMakeRef");
    if (!fpm) return -1;
    a[0] = (unsigned int)path; a[1] = (unsigned int)fsref80; a[2] = 0;
    return (short)oah_callppc_n("sfl_fspathmakeref", fpm, 3, a);
}
static int oah_sfl_call(const char *n, unsigned int *R) {
    static int off = -1;
    oah_sfl_t *e;
    if (strncmp(n, "LSSharedFileList", 16)) return 0;
    if (off < 0) off = getenv("OAH_NO_SFL") ? 1 : 0;
    if (off) return 0;
    n += 16;
    if (!strcmp(n, "ItemGetTypeID") && R[3] == OAH_CLIP_MAGIC && g_clip_state == 1) { oah_clip_thread(); R[3] = 0; return 1; }
    if (!strcmp(n, "GetTypeID") || !strcmp(n, "ItemGetTypeID")) {
        static unsigned int f; if (!f) f = oah_guest_sym("CoreFoundation", "_CFDataGetTypeID");
        R[3] = f ? oah_callppc_n("sfl_tid", f, 0, R) : 0;
        return 1;
    }
    if (!strcmp(n, "Create")) {                        /* (alloc, CFStringRef listType, CFTypeRef options) */
        static void *(*cr)(CFAllocatorRef, CFStringRef, CFTypeRef);
        CFStringRef t = oah_guest_str_native(R[4]);
        char tb[160] = "";
        void *l = NULL;
        if (!cr) cr = (void *(*)(CFAllocatorRef, CFStringRef, CFTypeRef))oah_sfl_nsym("LSSharedFileListCreate");
        if (t) CFStringGetCString(t, tb, sizeof tb, kCFStringEncodingUTF8);
        if (t && cr && (strstr(tb, "FavoriteItems") || strstr(tb, "FavoriteVolumes") || strstr(tb, "SavedSearch") ||
                        strstr(tb, "RecentApplication") || strstr(tb, "RecentDocument") || strstr(tb, "RecentServer"))
            && (!strstr(tb, "ApplicationRecentDocuments") || !getenv("OAH_NO_APPRECENT")))
            l = cr(NULL, t, NULL);
        /* list field 1: the app's own list, which takes the app's changes */
        R[3] = l ? oah_sfl_wrap(l, 1, strstr(tb, "ApplicationRecentDocuments") ? 1 : 0, 0) : 0;
        if (l) CFRelease(l);
        if (oah_sfltr()) fprintf(stderr, "[sfl] Create(\"%s\") -> native %p guest %08x\n", tb, l, R[3]);
        if (t) CFRelease(t);
        return 1;
    }
    if (!strcmp(n, "AddObserver") || !strcmp(n, "RemoveObserver")) return 1;   /* never called back */
    if (!strcmp(n, "SetAuthorization")) { R[3] = 0; return 1; }
    if (!(e = oah_sfl_find(R[3]))) {
        oah_tel("sfl-unknown-obj", n);
    if (oah_sfltr()) fprintf(stderr, "[sfl] %s on unknown object %08x\n", n, R[3]);
        R[3] = (unsigned int)-50;                      /* paramErr; NULL for the Copy* calls */
        if (!strncmp(n, "Copy", 4) || !strncmp(n, "ItemCopy", 8) || !strncmp(n, "Insert", 6)) R[3] = 0;
        return 1;
    }
    if (!strncmp(n, "Item", 4) && e->kind != 2) {
        if (oah_sfltr()) fprintf(stderr, "[sfl] %s on a list %08x\n", n, R[3]);
        R[3] = (!strncmp(n, "ItemCopy", 8)) ? 0 : (unsigned int)-50;
        return 1;
    }
    if (!strcmp(n, "GetSeedValue")) {
        static UInt32 (*f)(void *); if (!f) f = (UInt32 (*)(void *))oah_sfl_nsym("LSSharedFileListGetSeedValue");
        R[3] = f ? f(e->n) : 1; return 1;
    }
    if (!strcmp(n, "CopySnapshot")) {                  /* (list, UInt32 *seed) -> guest CFArray of items */
        static CFArrayRef (*f)(void *, UInt32 *); static UInt32 (*gid)(void *);
        UInt32 seed = 0; CFArrayRef a; CFIndex i, cnt; unsigned int ga, list = R[3];
        if (!f) { f = (CFArrayRef (*)(void *, UInt32 *))oah_sfl_nsym("LSSharedFileListCopySnapshot");
                  gid = (UInt32 (*)(void *))oah_sfl_nsym("LSSharedFileListItemGetID"); }
        if (!f || !oah_guest_cf_load()) { R[3] = 0; return 1; }
        a = f(e->n, &seed);
        ga = gcall(G_ARR_NEW, 3, 0, 0, g_cf[G_ACB], 0);
        cnt = a ? CFArrayGetCount(a) : 0;
        static int keepall = -1;
        if (keepall < 0) keepall = getenv("OAH_SFL_ALL") ? 1 : 0;
        for (i = 0; ga && i < cnt; i++) {
            void *it = (void *)CFArrayGetValueAtIndex(a, i);
            unsigned int gi;
            char path[1024];
            /* Mojave's lists hold items with no file behind them: the Computer and Remote Disc
               special items (empty names), iCloud (x-apple-finder:icloud), AirDrop, Recents. The
               guest can neither name, draw nor open them (ItemResolve -> fnfErr): open panels
               showed blank rows and an icon-less "iCloud" in the sidebar. Leave them out. */
            if (!keepall && (!oah_sfl_item_path(it, path, sizeof path) ||
                             (strlen(path) > 13 && strstr(path, ".cannedSearch")))) {   /* Recents: a bundle to 10.6 */
                if (oah_sfltr()) fprintf(stderr, "[sfl]   item %ld has no file path: left out\n", (long)i);
                continue;
            }
            gi = oah_sfl_wrap(it, 2, list, gid ? gid(it) : (unsigned int)i);
            if (gi) gcall(G_ARR_ADD, 2, ga, gi, 0, 0);
        }
        if (R[4]) *(UInt32 *)R[4] = OSSwapHostToBigInt32(seed);
        if (oah_sfltr()) fprintf(stderr, "[sfl] CopySnapshot(%08x) -> %ld item(s), seed %u\n", list, (long)cnt, (unsigned)seed);
        if (a) CFRelease(a);
        R[3] = ga; return 1;
    }
    if (!strcmp(n, "CopyProperty") || !strcmp(n, "ItemCopyProperty")) {   /* (obj, CFStringRef name) */
        static CFTypeRef (*fl)(void *, CFStringRef), (*fi)(void *, CFStringRef);
        CFStringRef k = oah_guest_str_native(R[4]); CFTypeRef v = NULL;
        if (!fl) { fl = (CFTypeRef (*)(void *, CFStringRef))oah_sfl_nsym("LSSharedFileListCopyProperty");
                   fi = (CFTypeRef (*)(void *, CFStringRef))oah_sfl_nsym("LSSharedFileListItemCopyProperty"); }
        if (k) v = (n[0] == 'I' ? fi : fl) ? (n[0] == 'I' ? fi : fl)(e->n, k) : NULL;
        R[3] = v ? oah_cf_to_guest(v) : 0;
        if (!v && k && n[0] == 'I' && CFEqual(k, CFSTR("com.apple.LSSharedFileList.VolumeRefNum"))) {
            /* Mojave's items carry no Carbon volume number: take it from the guest File Manager
               (DesktopServices opens a sidebar volume by its vRefNum) */
            char path[1024]; unsigned char ref[80], info[160];
            static unsigned int fgc;
            if (!fgc) fgc = oah_guest_sym("CarbonCore", "_FSGetCatalogInfo");
            if (fgc && oah_sfl_item_path(e->n, path, sizeof path) && !oah_sfl_guest_fsref(path, ref)) {
                unsigned int a[6];
                memset(info, 0, sizeof info);
                a[0] = (unsigned int)ref; a[1] = 0x00000004 /* kFSCatInfoVolume */; a[2] = (unsigned int)info; a[3] = 0; a[4] = 0; a[5] = 0;
                if (!(short)oah_callppc_n("sfl_catinfo", fgc, 6, a)) {
                    short vr = (short)((info[2] << 8) | info[3]);   /* FSCatalogInfo.volume, big-endian */
                    CFNumberRef num = CFNumberCreate(NULL, kCFNumberSInt16Type, &vr);
                    R[3] = oah_cf_to_guest(num);
                    CFRelease(num);
                    if (oah_sfltr()) fprintf(stderr, "[sfl]   %s -> vRefNum %d\n", path, vr);
                }
            }
        }
        if (oah_sfltr()) { char kb[128] = ""; if (k) CFStringGetCString(k, kb, sizeof kb, kCFStringEncodingUTF8);
                           fprintf(stderr, "[sfl] %s(%08x, \"%s\") -> %p / guest %08x\n", n, e->g, kb, v, R[3]); }
        if (v) CFRelease(v);
        if (k) CFRelease(k);
        return 1;
    }
    if (!strcmp(n, "ItemGetID")) {
        static UInt32 (*f)(void *); if (!f) f = (UInt32 (*)(void *))oah_sfl_nsym("LSSharedFileListItemGetID");
        R[3] = f ? f(e->n) : e->id; return 1;
    }
    if (!strcmp(n, "ItemCopyDisplayName")) {
        static CFStringRef (*f)(void *); CFStringRef s;
        if (!f) f = (CFStringRef (*)(void *))oah_sfl_nsym("LSSharedFileListItemCopyDisplayName");
        s = f ? f(e->n) : NULL;
        R[3] = s ? oah_cf_to_guest(s) : 0;
        if (oah_sfltr()) { char b[256] = ""; if (s) CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8);
                           fprintf(stderr, "[sfl] ItemCopyDisplayName(%08x) -> \"%s\"\n", e->g, b); }
        if (s) CFRelease(s);
        return 1;
    }
    if (!strcmp(n, "ItemResolve")) {                   /* (item, flags, CFURLRef *outURL, FSRef *outRef) */
        char path[1024];
        if (!oah_sfl_item_path(e->n, path, sizeof path)) { R[3] = (unsigned int)-43; return 1; }   /* fnfErr */
        if (R[5]) {
            static unsigned int fu; unsigned int a[4];
            if (!fu) fu = oah_guest_sym("CoreFoundation", "_CFURLCreateFromFileSystemRepresentation");
            a[0] = 0; a[1] = (unsigned int)path; a[2] = strlen(path); a[3] = 1;
            *(unsigned int *)R[5] = OSSwapHostToBigInt32(fu ? oah_callppc_n("sfl_url", fu, 4, a) : 0);
        }
        if (R[6] && oah_sfl_guest_fsref(path, (unsigned char *)R[6])) { R[3] = (unsigned int)-43; return 1; }
        if (oah_sfltr()) fprintf(stderr, "[sfl] ItemResolve(%08x) -> %s\n", e->g, path);
        R[3] = 0; return 1;
    }
    if (!strcmp(n, "ItemCopyIconRef")) {
        char path[1024]; unsigned char ref[80]; unsigned int icon = 0;
        static unsigned int fic;
        R[3] = 0;
        if (!fic) fic = oah_guest_sym("LaunchServices", "_GetIconRefFromFileInfo");
        if (!fic) fic = oah_guest_sym("OSServices", "_GetIconRefFromFileInfo");
        if (!fic || !oah_sfl_item_path(e->n, path, sizeof path) || oah_sfl_guest_fsref(path, ref)) return 1;
        {   unsigned int a[8]; short label = 0;
            a[0] = (unsigned int)ref; a[1] = 0; a[2] = 0; a[3] = 0; a[4] = 0; a[5] = 0 /* kIconServicesNormalUsageFlag */;
            a[6] = (unsigned int)&icon; a[7] = (unsigned int)&label;
            if (!oah_callppc_n("sfl_icon", fic, 8, a)) R[3] = OSSwapBigToHostInt32(icon);
        }
        return 1;
    }
    if (!strcmp(n, "ItemCopyAliasData")) {            /* guest Alias Manager record of the target, as guest CFData */
        char path[1024]; unsigned char ref[80];
        static unsigned int fna, fgs, fdh, fdc;
        unsigned int a[3], h = 0, sz, p;
        R[3] = 0;
        if (!fna) { fna = oah_guest_sym("CarbonCore", "_FSNewAlias"); fgs = oah_guest_sym("CarbonCore", "_GetHandleSize");
                    fdh = oah_guest_sym("CarbonCore", "_DisposeHandle"); fdc = oah_guest_sym("CoreFoundation", "_CFDataCreate"); }
        if (!fna || !fgs || !fdc || !oah_sfl_item_path(e->n, path, sizeof path) || oah_sfl_guest_fsref(path, ref)) return 1;
        a[0] = 0; a[1] = (unsigned int)ref; a[2] = (unsigned int)&h;
        if ((short)oah_callppc_n("sfl_newalias", fna, 3, a) || !h) return 1;
        h = OSSwapBigToHostInt32(h);
        a[0] = h; sz = oah_callppc_n("sfl_hsize", fgs, 1, a);
        p = OSSwapBigToHostInt32(*(unsigned int *)h);
        a[0] = 0; a[1] = p; a[2] = sz;
        R[3] = (p && sz) ? oah_callppc_n("sfl_data", fdc, 3, a) : 0;
        if (fdh) { a[0] = h; oah_callppc_n("sfl_dispose", fdh, 1, a); }
        return 1;
    }
    {   /* changes: kept out of Mojave's lists unless OAH_SFL_RW=1 */
        static int rw = -1;
        if (rw < 0) rw = getenv("OAH_SFL_RW") ? 1 : 0;
        if (!strcmp(n, "InsertItemURL") || !strcmp(n, "InsertItemFSRef")) {
            /* (list, after, CFStringRef name, IconRef icon, CFURLRef url | FSRef *ref, props, propsClear) */
            static void *(*f)(void *, void *, CFStringRef, void *, CFURLRef, CFDictionaryRef, CFArrayRef);
            CFURLRef u = n[10] == 'U' ? oah_guest_url_native(R[7]) : oah_guest_fsref_native(R[7]);
            void *it = NULL;
            if (!f) f = (void *(*)(void *, void *, CFStringRef, void *, CFURLRef, CFDictionaryRef, CFArrayRef))oah_sfl_nsym("LSSharedFileListInsertItemURL");
            if ((rw || e->list == 1) && f && u) it = f(e->n, oah_sfl_after(R[4]), NULL, NULL, u, NULL, NULL);
            if (oah_sfltr()) { char b[1024] = ""; if (u) CFURLGetFileSystemRepresentation(u, true, (UInt8 *)b, sizeof b);
                               fprintf(stderr, "[sfl] %s(%08x, %s)%s\n", n, e->g, b, (rw || e->list == 1) ? "" : " (not written: OAH_SFL_RW=0)"); }
            if (u) CFRelease(u);
            if (it) { static UInt32 (*gid)(void *); if (!gid) gid = (UInt32 (*)(void *))oah_sfl_nsym("LSSharedFileListItemGetID");
                      R[3] = oah_sfl_wrap(it, 2, e->g, gid ? gid(it) : 0); CFRelease(it); }
            else R[3] = 0;
            return 1;
        }
        if (!strcmp(n, "ItemRemove") || !strcmp(n, "ItemMove") || !strcmp(n, "RemoveAllItems")) {
            oah_sfl_t *it = oah_sfl_find(R[4]);
            if (rw || e->list == 1) {
                if (n[0] == 'R') { static OSStatus (*f)(void *); if (!f) f = (OSStatus (*)(void *))oah_sfl_nsym("LSSharedFileListRemoveAllItems"); R[3] = f ? f(e->n) : 0; }
                else if (it && n[4] == 'R') { static OSStatus (*f)(void *, void *); if (!f) f = (OSStatus (*)(void *, void *))oah_sfl_nsym("LSSharedFileListItemRemove"); R[3] = f ? f(e->n, it->n) : 0; }
                else if (it) { static OSStatus (*f)(void *, void *, void *); if (!f) f = (OSStatus (*)(void *, void *, void *))oah_sfl_nsym("LSSharedFileListItemMove"); R[3] = f ? f(e->n, it->n, oah_sfl_after(R[5])) : 0; }
                else R[3] = (unsigned int)-50;
            } else R[3] = 0;
            if (oah_sfltr()) fprintf(stderr, "[sfl] %s(%08x)%s\n", n, e->g, (rw || e->list == 1) ? "" : " (not written)");
            return 1;
        }
    }
    /* SetProperty / ItemSetProperty / ItemSet / ItemSetProperties...: accepted, not stored */
    oah_tel("sfl-ignored", n);
    if (oah_sfltr()) fprintf(stderr, "[sfl] %s(%08x): ignored\n", n, e->g);
    R[3] = 0;
    return 1;
}

/* oah_snd.h - native Sound Manager for PowerPC guests (included by liboah_mojave.c).
   Snow Leopard's PowerPC CarbonSound cannot reach Mojave's coreaudiod
   (SndNewChannel -> notEnoughHardwareErr), so SndNewChannel, SndDoCommand,
   SndDoImmediate, SndChannelStatus and SndDisposeChannel are bridged here.
   All channels are mixed into one Mojave default-output AudioUnit (44.1 kHz
   float stereo).  Sample data is copied and converted when a bufferCmd is
   queued.  callBackCmd callbacks are guest code, which must not run on the
   CoreAudio thread: the render thread moves them to a pending list (a little
   before the preceding audio ends, to hide the delivery delay) and
   oah_snd_pump() calls them on the guest main thread at the next bridge entry. */
#include <AudioUnit/AudioUnit.h>
#include <pthread.h>

#define OAH_SND_OUTRATE 44100.0
#define OAH_SND_EARLY   (OAH_SND_OUTRATE * 0.06)   /* fire callbacks 60 ms early */
#define OAH_SND_MAXPEND 1024

typedef struct oah_snd_item {
    struct oah_snd_item *next;
    int kind;                       /* 0 audio, 1 callback, 2 wait (silence) */
    float *pcm;                     /* interleaved stereo */
    unsigned int frames;
    double rate, pos;               /* source rate (Hz), read position (frames) */
    unsigned short cmd; short p1; unsigned int p2;
    unsigned int src;               /* guest sample data, read at play time (pcm == 0) */
    unsigned char nch, bits, sgn, le;
} oah_snd_item;

/* Read one guest sample as float (lazy items). */
static inline float oah_snd_samp(const oah_snd_item *it, unsigned int frame, int ch) {
    unsigned int idx = frame * it->nch + (it->nch == 2 ? ch : 0);
    if (it->bits == 8) {
        unsigned char b = *(volatile unsigned char *)(it->src + idx);
        return it->sgn ? (signed char)b / 128.f : ((int)b - 128) / 128.f;
    } else {
        unsigned short w = *(volatile unsigned short *)(it->src + 2 * idx);
        return (short)(it->le ? OSSwapLittleToHostInt16(w) : OSSwapBigToHostInt16(w)) / 32768.f;
    }
}

typedef struct oah_snd_chan {
    struct oah_snd_chan *next;
    unsigned int guest, cbfn;
    int ours;                       /* guest SndChannel record allocated here */
    oah_snd_item *head, *tail;
    unsigned int volword;           /* volumeCmd: right<<16 | left, 0x0100 = unity */
    unsigned int amp;               /* ampCmd 0..255 */
    unsigned int ratemul;           /* Fixed 16.16 */
    unsigned int nqueued;
    unsigned int npend;             /* callbacks fired but not yet delivered to the guest */
    unsigned long pend_frames;      /* output frames rendered since the oldest of them fired */
    float gm;                       /* stall mute gain, ramps 0..1 */
} oah_snd_chan;
/* A channel whose callback has waited longer than this is faded out until the guest catches
   up: SimCity's DirectSound stream keeps queueing chunks of its ring buffer from the sound
   thread, but its play cursor only moves in the callback, so while the main thread is busy
   (city load) the ring's stale contents would replay as rapid beeping. */
#define OAH_SND_STALL   (OAH_SND_OUTRATE * 0.10)

typedef struct { unsigned int guest, fn; unsigned short cmd; short p1; unsigned int p2; } oah_snd_pend;

static pthread_mutex_t g_snd_mx = PTHREAD_MUTEX_INITIALIZER;
static oah_snd_chan *g_snd_chans;
static oah_snd_item *g_snd_garbage;
static oah_snd_pend g_snd_pend[OAH_SND_MAXPEND];
static volatile unsigned int g_snd_pend_n;
static AudioUnit g_snd_au;
static int g_snd_au_state;          /* 0 not tried, 1 running, -1 failed */
static unsigned long g_snd_cbs, g_snd_bufs, g_snd_cbs_bg, g_snd_muted, g_snd_cbdrop;
static volatile unsigned long g_snd_lockmiss;   /* render buffers given up on the mixer lock */
static unsigned long g_snd_gaps;                /* a channel ran out of queued audio mid-buffer */
static int g_snd_trace = -1;
static float g_snd_inpeak, g_snd_outpeak;
static int g_snd_hdrlog;
static unsigned long g_snd_renders;
static void (*volatile g_snd_music_mix)(float *out, unsigned int n);
/* Idle output. Once an app had made a sound, the output unit (Mojave) or the mixing thread feeding
   the host ring (Catalina: 250 wake-ups a second) ran until the app quit, rendering silence.
   After OAH_SND_IDLE_SECS (default 10; 0 = never) of silence with no channel holding queued
   commands, no pending callbacks and no music stream, the output stops; the next Sound Manager
   call starts it again (before its command is queued). */
static volatile unsigned long g_snd_quiet;      /* frames of silence in a row */
static volatile int g_snd_paused, g_snd_pause_req, g_snd_ring_mode;
static pthread_mutex_t g_snd_run_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_snd_run_cv = PTHREAD_COND_INITIALIZER;
static OSStatus (*g_snd_fstart)(AudioUnit), (*g_snd_fstop)(AudioUnit);
static int oah_snd_tr(void);
static unsigned long oah_snd_idle_frames(void) {
    static long f = -1;
    if (f < 0) { const char *e = getenv("OAH_SND_IDLE_SECS"); f = (long)((e ? atof(e) : 10.0) * OAH_SND_OUTRATE); if (f < 0) f = 0; }
    return (unsigned long)f;
}
static void oah_snd_pause_fn(void *a) {         /* off the audio thread (output unit mode) */
    (void)a;
    pthread_mutex_lock(&g_snd_run_mx);
    if (!g_snd_paused && oah_snd_idle_frames() && g_snd_quiet >= oah_snd_idle_frames() && g_snd_fstop && !g_snd_fstop(g_snd_au)) {
        g_snd_paused = 1;
        if (oah_snd_tr()) fprintf(stderr, "[snd] output stopped (silent)\n");
    }
    g_snd_pause_req = 0;
    pthread_mutex_unlock(&g_snd_run_mx);
}
static void oah_snd_wake(void) {                /* a Sound Manager call: output runs again */
    pthread_mutex_lock(&g_snd_run_mx);
    g_snd_quiet = 0;
    if (g_snd_paused) {
        if (g_snd_ring_mode) { g_snd_paused = 0; pthread_cond_signal(&g_snd_run_cv); }
        else if (g_snd_fstart && !g_snd_fstart(g_snd_au)) g_snd_paused = 0;
        if (oah_snd_tr()) fprintf(stderr, "[snd] output running again%s\n", g_snd_paused ? " (start failed)" : "");
    }
    pthread_mutex_unlock(&g_snd_run_mx);
}
static void oah_snd_note_render(int active, unsigned int n) {
    unsigned long lim = oah_snd_idle_frames();
    if (active || g_snd_music_mix || g_snd_pend_n) { g_snd_quiet = 0; return; }
    g_snd_quiet += n;
    if (lim && g_snd_quiet >= lim && !g_snd_ring_mode && !g_snd_paused && !g_snd_pause_req) {
        g_snd_pause_req = 1;
        dispatch_async_f(dispatch_get_global_queue(0, 0), 0, oah_snd_pause_fn);
    }
}

static int oah_snd_tr(void) {
    if (g_snd_trace < 0) g_snd_trace = (getenv("OAH_SNDTRACE") || getenv("OAH_VERBOSE")) ? 1 : 0;
    return g_snd_trace;
}
static unsigned int be32(unsigned int a) { return OSSwapBigToHostInt32(*(volatile unsigned int *)a); }
static unsigned short be16(unsigned int a) { return OSSwapBigToHostInt16(*(volatile unsigned short *)a); }

static oah_snd_chan *oah_snd_find(unsigned int guest) {
    oah_snd_chan *c;
    for (c = g_snd_chans; c; c = c->next) if (c->guest == guest) return c;
    return 0;
}
static void oah_snd_trash(oah_snd_item *it) { it->next = g_snd_garbage; g_snd_garbage = it; }
static void oah_snd_free_garbage(void) {    /* call with g_snd_mx held, not on the render thread */
    oah_snd_item *it = g_snd_garbage, *n;
    g_snd_garbage = 0;
    for (; it; it = n) { n = it->next; free(it->pcm); free(it); }
}
static void oah_snd_unlink(oah_snd_chan *c, oah_snd_item *prev, oah_snd_item *it) {
    if (prev) prev->next = it->next; else c->head = it->next;
    if (c->tail == it) c->tail = prev;
    if (c->nqueued) c->nqueued--;
    oah_snd_trash(it);
}
static void oah_snd_fire(oah_snd_chan *c, oah_snd_item *it) {
    /* The callback is the channel record's callBack field at the time it fires, as on the
       real Sound Manager: Apple's CarbonSndPlayDoubleBuffer (FMOD 3 in Bejeweled 2) creates the
       channel with no callback and stores chan->callBack itself before queueing callBackCmd,
       so the one passed to SndNewChannel (0) left it unfed after the first buffer: no sound. */
    unsigned int fn = c->guest ? OSSwapBigToHostInt32(*(volatile unsigned int *)(c->guest + 8)) : 0;
    if (!fn) fn = c->cbfn;
    if (fn && g_snd_pend_n < OAH_SND_MAXPEND) {
        oah_snd_pend *p = &g_snd_pend[g_snd_pend_n];
        p->guest = c->guest; p->fn = fn; p->cmd = it->cmd; p->p1 = it->p1; p->p2 = it->p2;
        g_snd_pend_n++;
        if (!c->npend++) c->pend_frames = 0;
    }
}
static double oah_snd_step(oah_snd_chan *c, oah_snd_item *it) {
    return it->rate * ((double)c->ratemul / 65536.0) / OAH_SND_OUTRATE;
}

/* ---- render thread ---- */
static void oah_snd_mix_chan(oah_snd_chan *c, float *out, unsigned int n) {
    unsigned int f = 0;
    float gl = (float)((c->volword & 0xffff) / 256.0 * c->amp / 255.0);
    float gr = (float)((c->volword >> 16) / 256.0 * c->amp / 255.0);
    float g0 = c->gm, g1 = (c->npend && c->pend_frames >= OAH_SND_STALL) ? 0.f : 1.f, gd;
    if (c->npend) c->pend_frames += n;
    if (g1 > g0) g1 = g0 + 0.25f > 1.f ? 1.f : g0 + 0.25f;   /* fade back in over ~4 buffers */
    gd = (g1 - g0) / (float)n;
    c->gm = g1;
    if (g1 == 0.f) g_snd_muted += n;
#define OAH_GM(f) (g0 + gd * (float)(f))
    oah_snd_item *it, *prev;
    double acc;
    while (f < n && (it = c->head)) {
        if (it->kind == 1) { oah_snd_fire(c, it); oah_snd_unlink(c, 0, it); continue; }
        if (it->kind == 2) {
            unsigned int left = it->frames - (unsigned int)it->pos, k = n - f < left ? n - f : left;
            f += k; it->pos += k;
            if ((unsigned int)it->pos >= it->frames) oah_snd_unlink(c, 0, it);
            continue;
        }
        {
            double step = oah_snd_step(c, it), pos = it->pos;
            const float *s = it->pcm;
            unsigned int nf = it->frames;
            if (step <= 0) step = 1;
            if (!s) {
                float pk = 0;
                while (f < n && pos < nf) {
                    unsigned int i = (unsigned int)pos;
                    float fr = (float)(pos - i);
                    unsigned int j = i + 1 < nf ? i + 1 : i;
                    float l0 = oah_snd_samp(it, i, 0), r0 = oah_snd_samp(it, i, 1);
                    float l = l0 + (oah_snd_samp(it, j, 0) - l0) * fr;
                    float r = r0 + (oah_snd_samp(it, j, 1) - r0) * fr;
                    float a = l < 0 ? -l : l;
                    if (a > pk) pk = a;
                    out[2 * f] += l * gl * OAH_GM(f); out[2 * f + 1] += r * gr * OAH_GM(f);
                    pos += step; f++;
                }
                if (pk > g_snd_inpeak) g_snd_inpeak = pk;
            } else
            while (f < n && pos < nf) {
                unsigned int i = (unsigned int)pos;
                float fr = (float)(pos - i);
                unsigned int j = i + 1 < nf ? i + 1 : i;
                float l = s[2 * i] + (s[2 * j] - s[2 * i]) * fr;
                float r = s[2 * i + 1] + (s[2 * j + 1] - s[2 * i + 1]) * fr;
                out[2 * f] += l * gl * OAH_GM(f); out[2 * f + 1] += r * gr * OAH_GM(f);
                pos += step; f++;
            }
            it->pos = pos;
            if (pos >= nf) oah_snd_unlink(c, 0, it);
        }
    }
#undef OAH_GM
    if (f && f < n) g_snd_gaps++;                /* ran dry partway through: a click */
    /* Fire callbacks whose preceding audio ends within OAH_SND_EARLY frames. */
    acc = 0; prev = 0;
    for (it = c->head; it; ) {
        if (it->kind == 0) { double st = oah_snd_step(c, it); acc += (it->frames - it->pos) / (st > 0 ? st : 1); }
        else if (it->kind == 2) acc += it->frames - it->pos;
        else {
            oah_snd_item *nx = it->next;
            if (acc >= OAH_SND_EARLY) break;
            oah_snd_fire(c, it); oah_snd_unlink(c, prev, it);
            it = nx;
            continue;
        }
        if (acc >= OAH_SND_EARLY) break;
        prev = it; it = it->next;
    }
}
static OSStatus oah_snd_render(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts,
                               UInt32 bus, UInt32 n, AudioBufferList *io) {
    float *out = (float *)io->mBuffers[0].mData;
    unsigned int i;
    oah_snd_chan *c;
    (void)ref; (void)fl; (void)ts; (void)bus;
    memset(out, 0, n * 8);
    if (g_snd_music_mix) g_snd_music_mix(out, n);           /* APlayer music stream (oah_apl.h) */
    /* Never block the audio thread long, but do not give up at once either: a whole buffer of
       silence (~11 ms) each time a guest thread was queueing the next FMOD buffer under the lock
       made Bejeweled 2's effects crackle. The guest-side critical sections are a few list
       operations: spin up to ~1 ms. */
    {
        static unsigned long misses;
        int k;
        for (k = 0; pthread_mutex_trylock(&g_snd_mx); k++) {
            if (k >= 200) { misses++; g_snd_lockmiss = misses; return noErr; }
            usleep(5);
        }
    }
    {
        int active = 0;
        for (c = g_snd_chans; c; c = c->next) { oah_snd_mix_chan(c, out, n); if (c->head) active = 1; }
        pthread_mutex_unlock(&g_snd_mx);
        g_snd_renders++;
        for (i = 0; i < 2 * n; i++) {
            float a = out[i] < 0 ? -out[i] : out[i];
            if (a > g_snd_outpeak) g_snd_outpeak = a;
            if (a > 0.f) active = 1;
            if (out[i] > 1.f) out[i] = 1.f; else if (out[i] < -1.f) out[i] = -1.f;
        }
        oah_snd_note_render(active, n);
    }
    return noErr;
}

/* ---- output unit (Mojave AudioToolbox, loaded on first use) ---- */
static int oah_snd_start_au(void) {
    void *h;
    AudioComponent (*fFind)(AudioComponent, const AudioComponentDescription *);
    OSStatus (*fNew)(AudioComponent, AudioComponentInstance *);
    OSStatus (*fSet)(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, const void *, UInt32);
    OSStatus (*fInit)(AudioUnit);
    OSStatus (*fStart)(AudioUnit);
    AudioComponentDescription d;
    AudioStreamBasicDescription f;
    AURenderCallbackStruct cb;
    AudioComponent comp;
    OSStatus e;
    if (g_snd_au_state) { if (g_snd_au_state > 0) oah_snd_wake(); return g_snd_au_state > 0; }
    g_snd_au_state = -1;
    h = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_LAZY | RTLD_GLOBAL);
    if (!h) h = dlopen("/System/Library/Frameworks/AudioUnit.framework/AudioUnit", RTLD_LAZY | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "[snd] cannot load AudioToolbox: %s\n", dlerror()); return 0; }
    fFind = (AudioComponent (*)(AudioComponent, const AudioComponentDescription *))dlsym(h, "AudioComponentFindNext");
    fNew = (OSStatus (*)(AudioComponent, AudioComponentInstance *))dlsym(h, "AudioComponentInstanceNew");
    fSet = (OSStatus (*)(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, const void *, UInt32))dlsym(h, "AudioUnitSetProperty");
    fInit = (OSStatus (*)(AudioUnit))dlsym(h, "AudioUnitInitialize");
    fStart = (OSStatus (*)(AudioUnit))dlsym(h, "AudioOutputUnitStart");
    if (!fFind || !fNew || !fSet || !fInit || !fStart) { fprintf(stderr, "[snd] AudioToolbox symbols missing\n"); return 0; }
    g_snd_fstart = fStart;
    *(void **)&g_snd_fstop = dlsym(h, "AudioOutputUnitStop");
    memset(&d, 0, sizeof d);
    d.componentType = kAudioUnitType_Output;
    d.componentSubType = kAudioUnitSubType_DefaultOutput;
    d.componentManufacturer = kAudioUnitManufacturer_Apple;
    comp = fFind(0, &d);
    if (!comp || (e = fNew(comp, &g_snd_au))) { fprintf(stderr, "[snd] no default output unit\n"); return 0; }
    memset(&f, 0, sizeof f);
    f.mSampleRate = OAH_SND_OUTRATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 8; f.mFramesPerPacket = 1; f.mBytesPerFrame = 8;
    f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32;
    if ((e = fSet(g_snd_au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f))) {
        fprintf(stderr, "[snd] StreamFormat -> %d\n", (int)e); return 0;
    }
    cb.inputProc = oah_snd_render; cb.inputProcRefCon = 0;
    if ((e = fSet(g_snd_au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb))) {
        fprintf(stderr, "[snd] SetRenderCallback -> %d\n", (int)e); return 0;
    }
    if ((e = fInit(g_snd_au)) || (e = fStart(g_snd_au))) { fprintf(stderr, "[snd] start -> %d\n", (int)e); return 0; }
    g_snd_au_state = 1;
    if (oah_snd_tr()) fprintf(stderr, "[snd] output unit running at %.0f Hz\n", OAH_SND_OUTRATE);
    return 1;
}

/* ---- sound header -> native float stereo ---- */
static oah_snd_item *oah_snd_load(unsigned int h) {
    unsigned int sp = be32(h), nch = 1, frames, bits = 8, fmt = 0, data, i;
    unsigned char enc = *(volatile unsigned char *)(h + 20);
    double rate = be32(h + 8) / 65536.0;
    int sgn = 0, le = 0;                 /* 8-bit: offset binary unless 'twos'; 16-bit: big-endian unless 'sowt' */
    oah_snd_item *it;
    float *o;
    if (enc == 0x00) { frames = be32(h + 4); data = sp ? sp : h + 22; }
    else if (enc == 0xFF || enc == 0xFE) {
        nch = be32(h + 4); frames = be32(h + 22); data = sp ? sp : h + 64;
        if (enc == 0xFF) bits = be16(h + 48);
        else {
            fmt = be32(h + 40); bits = be16(h + 62);
            if (fmt == 'twos') sgn = 1;
            else if (fmt == 'sowt') { sgn = 1; le = 1; }
            else if (fmt == 'raw ' || fmt == 'NONE' || fmt == 0) sgn = 0;
            else { fprintf(stderr, "[snd] compressed format '%.4s' not supported\n", (char *)&(unsigned int){OSSwapHostToBigInt32(fmt)}); return 0; }
        }
    } else { char t[16]; snprintf(t, sizeof t, "encode 0x%02x", enc); oah_tel("snd", t); fprintf(stderr, "[snd] unknown sound header encode 0x%02x\n", enc); return 0; }
    if (nch < 1 || nch > 2 || (bits != 8 && bits != 16) || frames > 50000000 || rate <= 0) {
        oah_tel("snd", "unsupported header");
        fprintf(stderr, "[snd] unsupported header: ch=%u bits=%u frames=%u rate=%g\n", nch, bits, frames, rate);
        return 0;
    }
    /* Like the real Sound Manager, samples are read from the guest buffer while
       playing, not when queued: apps that stream through a ring buffer (SC4's
       DirectSound layer) queue a segment before they fill it. */
    it = (oah_snd_item *)calloc(1, sizeof *it);
    if (!it) return 0;
    it->kind = 0; it->pcm = 0; it->frames = frames; it->rate = rate;
    it->src = data; it->nch = (unsigned char)nch; it->bits = (unsigned char)bits;
    it->sgn = (unsigned char)sgn; it->le = (unsigned char)le;
    (void)o; (void)i;
    {
        float pk = 0; unsigned int k;
        for (k = 0; k < frames; k++) { float a = oah_snd_samp(it, k, 0); if (a < 0) a = -a; if (a > pk) pk = a; }
        if (oah_snd_tr() && g_snd_hdrlog < 8) {
            g_snd_hdrlog++;
            fprintf(stderr, "[snd] header %08x enc=%02x ch=%u bits=%u frames=%u rate=%.1f fmt=%08x data=%08x sgn=%d le=%d peak=%.3f\n",
                    h, enc, nch, bits, frames, rate, fmt, data, sgn, le, pk);
        }
    }
    return it;
}

static void oah_snd_append(oah_snd_chan *c, oah_snd_item *it, int front) {
    if (front) { it->next = c->head; c->head = it; if (!c->tail) c->tail = it; }
    else { it->next = 0; if (c->tail) c->tail->next = it; else c->head = it; c->tail = it; }
    c->nqueued++;
}
static void oah_snd_quiet(oah_snd_chan *c) {          /* stop the sound now playing */
    if (c->head && c->head->kind != 1) oah_snd_unlink(c, 0, c->head);
}
static void oah_snd_flush(oah_snd_chan *c) {          /* drop queued commands, keep current sound */
    oah_snd_item *keep = c->head && c->head->kind == 0 && c->head->pos > 0 ? c->head : 0, *it, *n;
    for (it = keep ? keep->next : c->head; it; it = n) { n = it->next; oah_snd_trash(it); }
    if (keep) { keep->next = 0; c->head = c->tail = keep; c->nqueued = 1; }
    else { c->head = c->tail = 0; c->nqueued = 0; }
}

/* Run one command; called with g_snd_mx held.  Returns an OSErr. */
static int oah_snd_cmd(oah_snd_chan *c, unsigned short cmd, short p1, unsigned int p2, int immediate) {
    oah_snd_item *it;
    switch (cmd) {
    case 0: case 24: case 25: case 8: case 9:       /* null, available, version/reInit, pause, resume */
        return 0;
    case 3: oah_snd_quiet(c); return 0;             /* quietCmd */
    case 4: oah_snd_flush(c); return 0;             /* flushCmd */
    case 13:                                        /* callBackCmd */
        if (immediate) { it = (oah_snd_item *)calloc(1, sizeof *it); it->cmd = cmd; it->p1 = p1; it->p2 = p2; oah_snd_fire(c, it); free(it); return 0; }
        it = (oah_snd_item *)calloc(1, sizeof *it);
        it->kind = 1; it->cmd = cmd; it->p1 = p1; it->p2 = p2;
        oah_snd_append(c, it, 0);
        return 0;
    case 10:                                        /* waitCmd: param1 in half-milliseconds */
        it = (oah_snd_item *)calloc(1, sizeof *it);
        it->kind = 2; it->frames = (unsigned int)((unsigned short)p1 * OAH_SND_OUTRATE / 2000.0);
        oah_snd_append(c, it, immediate);
        return 0;
    case 80: case 81:                               /* soundCmd (keep for later), bufferCmd */
        if (cmd == 80) return 0;
        if (!p2) return -50;
        it = oah_snd_load(p2);
        if (!it) return -206;                       /* badFormat */
        g_snd_bufs++;
        if (immediate) { oah_snd_quiet(c); oah_snd_append(c, it, 1); }
        else oah_snd_append(c, it, 0);
        return 0;
    case 43: c->amp = (unsigned short)p1 > 255 ? 255 : (unsigned short)p1; return 0;          /* ampCmd */
    case 44: if (p2) *(volatile unsigned short *)p2 = OSSwapHostToBigInt16((unsigned short)c->amp); return 0;
    case 46: c->volword = p2; return 0;             /* volumeCmd */
    case 47: if (p2) *(volatile unsigned int *)p2 = OSSwapHostToBigInt32(c->volword); return 0;
    case 82: case 86: c->ratemul = p2 ? p2 : 0x10000; return 0;  /* rateCmd, rateMultiplierCmd */
    case 85: case 87: if (p2) *(volatile unsigned int *)p2 = OSSwapHostToBigInt32(c->ratemul); return 0;
    default:
        { char t[24]; snprintf(t, sizeof t, "cmd %u ignored", cmd); oah_tel("snd", t); }
        if (oah_snd_tr()) fprintf(stderr, "[snd] command %u (p1=%d p2=%08x) ignored\n", cmd, p1, p2);
        return 0;
    }
}

/* The SndChannel record's own command queue (qLength +0x1e, qHead +0x20, qTail +0x22, 8-byte
   SndCommands from +0x24). Apple's CarbonSndPlayDoubleBuffer sample (FMOD 3: Bejeweled 2 and
   other PopCap games) refills a double buffer from its callback by writing commands straight
   into that queue in front of qHead ("InsertSndDoCommand"), which the real Sound Manager then
   ran. Keep qHead = qTail = the middle while idle, and after each callback run whatever was
   inserted (qHead .. middle, in that order). Called with g_snd_mx held. */
static void oah_snd_queue_reset(unsigned int g) {
    unsigned short ql = OSSwapBigToHostInt16(*(volatile unsigned short *)(g + 0x1e)), mid;
    if (!ql || ql > 128) { ql = 128; *(volatile unsigned short *)(g + 0x1e) = OSSwapHostToBigInt16(ql); }
    mid = ql / 2;
    *(volatile unsigned short *)(g + 0x20) = OSSwapHostToBigInt16(mid);
    *(volatile unsigned short *)(g + 0x22) = OSSwapHostToBigInt16(mid);
}
static void oah_snd_drain_inserted(oah_snd_chan *c) {
    unsigned int g = c->guest;
    unsigned short ql, mid, hd;
    int i, n = 0;
    if (!g) return;
    ql = OSSwapBigToHostInt16(*(volatile unsigned short *)(g + 0x1e));
    if (!ql || ql > 128) return;
    mid = ql / 2;
    hd = OSSwapBigToHostInt16(*(volatile unsigned short *)(g + 0x20));
    if (hd >= mid) return;
    for (i = hd; i < mid; i++) {
        unsigned int e = g + 0x24 + 8 * i;
        unsigned short cmd = OSSwapBigToHostInt16(*(volatile unsigned short *)e);
        short p1 = (short)OSSwapBigToHostInt16(*(volatile unsigned short *)(e + 2));
        unsigned int p2 = OSSwapBigToHostInt32(*(volatile unsigned int *)(e + 4));
        oah_snd_cmd(c, cmd & 0x7fff, p1, p2, 0);
        n++;
    }
    oah_snd_queue_reset(g);
    if (oah_snd_tr() && g_snd_cbs < 6) fprintf(stderr, "[snd] chan %08x: ran %d command(s) inserted into its queue\n", g, n);
    if (oah_snd_tr()) {
        static time_t last; time_t now = time(0);
        if (now - last >= 5) {
            last = now;
            fprintf(stderr, "[snd] queue-fed stats: buffers=%lu callbacks=%lu renders=%lu gaps=%lu lockmiss=%lu inpeak=%.3f outpeak=%.3f\n",
                    g_snd_bufs, g_snd_cbs, g_snd_renders, g_snd_gaps, g_snd_lockmiss, g_snd_inpeak, g_snd_outpeak);
            g_snd_inpeak = g_snd_outpeak = 0;
        }
    }
}

/* Callbacks are delivered on the guest main thread.  OAH_SND_BGCB=1 lets any guest thread
   deliver them (as the real Sound Manager did), which keeps the stream fed during a city
   load, but with this translator it stalled the load: the main thread sat parked inside
   the advisor Lua scripts while the translator's worker thread ran at 100%, in two runs
   out of two.  The stall mute in the mixer (OAH_SND_STALL) handles the starved stream instead. */
static volatile int g_snd_pump_busy;
/* Held while a callback runs.  SndDisposeChannel takes it too, so (as on the real Sound
   Manager) dispose waits for an in-flight callback and no callback arrives afterwards:
   without this the game's sound thread could call CPCMPlayer::SndChannelCallBack for a
   player that the loader thread was deleting, which is the likely cause of the stalled city load in testing. */
static pthread_mutex_t g_snd_cbmx;
static pthread_once_t g_snd_cbmx_once = PTHREAD_ONCE_INIT;
static void oah_snd_cbmx_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_snd_cbmx, &a); pthread_mutexattr_destroy(&a);
}
static void oah_snd_cb_lock(void) { pthread_once(&g_snd_cbmx_once, oah_snd_cbmx_init); pthread_mutex_lock(&g_snd_cbmx); }
static void oah_snd_cb_unlock(void) { pthread_mutex_unlock(&g_snd_cbmx); }
static void oah_snd_pump(void) {
    static int mainonly = -1;
    oah_snd_pend local[64];
    unsigned int n, i;
    if (!g_snd_pend_n) return;
    if (mainonly < 0) mainonly = getenv("OAH_SND_BGCB") ? 0 : 1;
    if (mainonly && !pthread_main_np()) return;
    if (!__sync_bool_compare_and_swap(&g_snd_pump_busy, 0, 1)) return;
    for (;;) {
        oah_snd_cb_lock();                     /* snapshot + delivery atomic w.r.t. SndDisposeChannel */
        pthread_mutex_lock(&g_snd_mx);
        n = g_snd_pend_n < 64 ? g_snd_pend_n : 64;
        memcpy(local, g_snd_pend, n * sizeof local[0]);
        memmove(g_snd_pend, g_snd_pend + n, (g_snd_pend_n - n) * sizeof local[0]);
        g_snd_pend_n -= n;
        oah_snd_free_garbage();
        pthread_mutex_unlock(&g_snd_mx);
        if (!n) { oah_snd_cb_unlock(); break; }
        for (i = 0; i < n; i++) {
            static unsigned int cmdbuf[2];
            unsigned int args[2];
            int alive;
            pthread_mutex_lock(&g_snd_mx);
            { oah_snd_chan *pc_ = oah_snd_find(local[i].guest); alive = pc_ != 0; if (pc_ && pc_->npend) pc_->npend--; }
            pthread_mutex_unlock(&g_snd_mx);
            if (!alive) continue;
            cmdbuf[0] = OSSwapHostToBigInt32(((unsigned int)local[i].cmd << 16) | (unsigned short)local[i].p1);
            cmdbuf[1] = OSSwapHostToBigInt32(local[i].p2);
            args[0] = local[i].guest; args[1] = (unsigned int)cmdbuf;
            if (oah_snd_tr() && g_snd_cbs < 4)
                fprintf(stderr, "[snd] callback ppc=%08x chan=%08x p1=%d p2=%08x\n", local[i].fn, local[i].guest, local[i].p1, local[i].p2);
            oah_callppc_n("oah_snd", local[i].fn, 2, args);
            pthread_mutex_lock(&g_snd_mx);
            { oah_snd_chan *pc_ = oah_snd_find(local[i].guest); if (pc_) oah_snd_drain_inserted(pc_); }
            pthread_mutex_unlock(&g_snd_mx);
            g_snd_cbs++;
            if (!pthread_main_np()) g_snd_cbs_bg++;
        }
        oah_snd_cb_unlock();
    }
    __sync_lock_release(&g_snd_pump_busy);
}

/* Bridged Sound Manager calls.  Returns 1 if handled. */
static int oah_snd_call(const char *name, unsigned int *R) {
    oah_snd_chan *c;
    if (name[0] != 'S' || name[1] != 'n' || name[2] != 'd') return 0;
    if (g_snd_au_state > 0) oah_snd_wake();
    if (oah_snd_tr()) {                            /* stats every 5 s */
        static time_t last;
        time_t now = time(0);
        if (now - last >= 5) {
            unsigned int busy = 0, nch = 0;
            last = now;
            pthread_mutex_lock(&g_snd_mx);
            for (c = g_snd_chans; c; c = c->next) { nch++; if (c->head) busy++; }
            pthread_mutex_unlock(&g_snd_mx);
            fprintf(stderr, "[snd] stats: buffers=%lu callbacks=%lu (bg %lu) stallmuted=%.1fs dropped=%lu channels=%u busy=%u pending=%u renders=%lu inpeak=%.3f outpeak=%.3f\n",
                    g_snd_bufs, g_snd_cbs, g_snd_cbs_bg, g_snd_muted / (double)OAH_SND_OUTRATE, g_snd_cbdrop, nch, busy, g_snd_pend_n, g_snd_renders, g_snd_inpeak, g_snd_outpeak);
            g_snd_inpeak = g_snd_outpeak = 0;
        }
    }
    /* GetCompressionInfo(short compressionID, OSType format, short numChannels, short sampleSize, CompressionInfoPtr cp)
       The guest's PPC Sound Manager has no codec components and fails this; SimCity 4's CPCMPlayer::Open
       then runs Close() (which frees the sample buffer it was handed) and returns false, and its caller
       cGZSnd::Load deletes the same buffer again: "pointer being freed was not allocated" on city load. */
    if (!strcmp(name, "GetCompressionInfo")) {
        unsigned int cp = R[7], fmt = R[4], nch = (unsigned short)R[5] ? (unsigned short)R[5] : 1, bits = (unsigned short)R[6];
        short id = (short)R[3];
        unsigned int spp = 1, bpp, bps;
        if (!cp) { R[3] = (unsigned int)-50; return 1; }
        if (bits != 8 && bits != 16 && bits != 24 && bits != 32) bits = 16;
        bps = bits / 8;
        if (id == 0 || id == -1 || fmt == 'NONE' || fmt == 'raw ' || fmt == 'twos' || fmt == 'sowt' || fmt == 0) {
            if (fmt == 'NONE' || fmt == 0) fmt = bits == 8 ? 'raw ' : 'twos';
            id = 0; bpp = bps;
        } else if (fmt == 'ima4') { spp = 64; bpp = 34; bps = 2; id = -2; }
        else if (fmt == 'MAC3') { spp = 6; bpp = 2; bps = 1; id = 3; }
        else if (fmt == 'MAC6') { spp = 6; bpp = 1; bps = 1; id = 4; }
        else if (fmt == 'ulaw' || fmt == 'alaw') { bpp = 1; bps = 2; id = -2; }
        else { char t[24]; unsigned int b = OSSwapHostToBigInt32(fmt); snprintf(t, sizeof t, "compinfo %.4s", (char *)&b); oah_tel("snd", t); if (oah_snd_tr()) fprintf(stderr, "[snd] GetCompressionInfo '%.4s' unknown\n", (char *)&(unsigned int){OSSwapHostToBigInt32(fmt)}); R[3] = (unsigned int)-206; return 1; }
        *(volatile unsigned int *)(cp + 4) = OSSwapHostToBigInt32(fmt);
        *(volatile unsigned short *)(cp + 8) = OSSwapHostToBigInt16((unsigned short)id);
        *(volatile unsigned short *)(cp + 10) = OSSwapHostToBigInt16((unsigned short)spp);
        *(volatile unsigned short *)(cp + 12) = OSSwapHostToBigInt16((unsigned short)bpp);
        *(volatile unsigned short *)(cp + 14) = OSSwapHostToBigInt16((unsigned short)(bpp * nch));
        *(volatile unsigned short *)(cp + 16) = OSSwapHostToBigInt16((unsigned short)bps);
        if (oah_snd_tr()) fprintf(stderr, "[snd] GetCompressionInfo id=%d ch=%u bits=%u -> '%.4s' spp=%u bpp=%u\n",
                                  (short)R[3], nch, bits, (char *)&(unsigned int){OSSwapHostToBigInt32(fmt)}, spp, bpp);
        R[3] = 0;
        return 1;
    }
    if (!strcmp(name, "SndNewChannel")) {          /* (SndChannelPtr *chan, short synth, long init, SndCallBackUPP) */
        unsigned int pp = R[3], g = pp ? be32(pp) : 0;
        if (!pp) { R[3] = (unsigned int)-50; return 1; }
        oah_snd_start_au();
        c = (oah_snd_chan *)calloc(1, sizeof *c);
        if (!g) { g = (unsigned int)calloc(1, 1060); c->ours = 1; *(volatile unsigned int *)pp = OSSwapHostToBigInt32(g); }
        *(volatile unsigned int *)(g + 8) = OSSwapHostToBigInt32(R[6]);    /* SndChannel.callBack */
        if (c->ours) *(volatile unsigned short *)(g + 0x1e) = OSSwapHostToBigInt16(128);   /* stdQLength */
        oah_snd_queue_reset(g);
        c->guest = g; c->cbfn = R[6]; c->volword = 0x01000100; c->amp = 255; c->ratemul = 0x10000;
        pthread_mutex_lock(&g_snd_mx);
        c->next = g_snd_chans; g_snd_chans = c;
        pthread_mutex_unlock(&g_snd_mx);
        if (oah_snd_tr()) fprintf(stderr, "[snd] SndNewChannel synth=%d init=%08x cb=%08x -> chan %08x%s\n",
                                  (short)R[4], R[5], R[6], g, g_snd_au_state > 0 ? "" : " (no audio output)");
        R[3] = 0;
        return 1;
    }
    if (!strcmp(name, "SndDisposeChannel")) {      /* (SndChannelPtr, Boolean quietNow) */
        oah_snd_chan **pc;
        int ours = 0;
        unsigned int k, m;
        oah_snd_cb_lock();                         /* wait for an in-flight callback */
        pthread_mutex_lock(&g_snd_mx);
        for (k = m = 0; k < g_snd_pend_n; k++)     /* drop its undelivered callbacks */
            if (g_snd_pend[k].guest != R[3]) g_snd_pend[m++] = g_snd_pend[k];
        g_snd_pend_n = m;
        for (pc = &g_snd_chans; *pc; pc = &(*pc)->next) if ((*pc)->guest == R[3]) break;
        c = *pc;
        if (c) {
            *pc = c->next;
            oah_snd_flush(c); if (c->head) { oah_snd_trash(c->head); c->head = 0; }
            ours = c->ours;
            oah_snd_free_garbage();
        }
        pthread_mutex_unlock(&g_snd_mx);
        if (c) { if (ours) free((void *)c->guest); free(c); }
        oah_snd_cb_unlock();
        R[3] = c ? 0 : (unsigned int)-205;
        return 1;
    }
    if (!strcmp(name, "SndDoCommand") || !strcmp(name, "SndDoImmediate")) {
        unsigned int cp = R[4];
        int imm = name[3] == 'D' && name[5] == 'I', err;
        unsigned short cmd;
        short p1;
        unsigned int p2;
        oah_snd_pump();
        if (!cp) { R[3] = (unsigned int)-50; return 1; }
        cmd = be16(cp); p1 = (short)be16(cp + 2); p2 = be32(cp + 4);
        pthread_mutex_lock(&g_snd_mx);
        c = oah_snd_find(R[3]);
        err = c ? oah_snd_cmd(c, cmd & 0x7fff, p1, p2, imm) : -205;
        if (pthread_main_np()) oah_snd_free_garbage();
        pthread_mutex_unlock(&g_snd_mx);
        if (oah_snd_tr() && (g_snd_bufs < 6 || err) && cmd != 13)
            fprintf(stderr, "[snd] %s chan=%08x cmd=%u p1=%d p2=%08x -> %d\n", name, R[3], cmd, p1, p2, err);
        R[3] = (unsigned int)err;
        return 1;
    }
    if (!strcmp(name, "SndPlay")) {                /* (SndChannelPtr chan, SndListHandle snd, Boolean async) */
        /* The guest's SndPlay parses the 'snd ' and then uses its own channel internals, which a
           channel made here does not have (-205 on every call, also with chan NULL). Parse the
           resource (format 1 or 2) and queue its commands on our channel; with no channel, make one,
           play synchronously and dispose of it, as the Sound Manager does. */
        unsigned int chan = R[3], h = R[4], async = R[5] & 0xff, d, ncmd, k, off;
        int err = 0, tmp = 0, fmt;
        unsigned int sv[8];
        if (!h || !(d = be32(h))) { R[3] = (unsigned int)-50; return 1; }
        fmt = be16(d);
        if (fmt == 1) { unsigned int nmod = be16(d + 2); off = 4 + nmod * 6; ncmd = be16(d + off); off += 2; }
        else if (fmt == 2) { ncmd = be16(d + 4); off = 6; }
        else { R[3] = (unsigned int)-206; return 1; }          /* badFormat */
        memcpy(sv, R + 3, sizeof sv);
        if (!chan) {                                   /* a channel of our own, for this call */
            unsigned int slot = 0;
            R[3] = (unsigned int)&slot; R[4] = 5 /* sampledSynth */; R[5] = 0; R[6] = 0;
            oah_snd_call("SndNewChannel", R);
            chan = OSSwapBigToHostInt32(slot); tmp = 1; async = 0;
            if (!chan) { memcpy(R + 3, sv, sizeof sv); R[3] = (unsigned int)-205; return 1; }
        }
        pthread_mutex_lock(&g_snd_mx);
        c = oah_snd_find(chan);
        for (k = 0; k < ncmd && k < 64 && c && !err; k++) {
            unsigned int a = d + off + k * 8;
            unsigned short cmd = be16(a); short p1 = (short)be16(a + 2); unsigned int p2 = be32(a + 4);
            if (cmd & 0x8000) p2 += d;                 /* dataPointerFlag: an offset into the resource */
            err = oah_snd_cmd(c, cmd & 0x7fff, p1, p2, 0);
        }
        if (!c) err = -205;
        pthread_mutex_unlock(&g_snd_mx);
        if (oah_snd_tr()) fprintf(stderr, "[snd] SndPlay chan=%08x fmt %d, %u command(s), %s -> %d\n", chan, fmt, ncmd, async ? "async" : "sync", err);
        if (!err && !async) {                          /* wait until the channel has played it all */
            int n;
            for (n = 0; n < 6000; n++) {
                int busy;
                oah_snd_pump();
                pthread_mutex_lock(&g_snd_mx); c = oah_snd_find(chan); busy = c && c->head; pthread_mutex_unlock(&g_snd_mx);
                if (!busy) break;
                usleep(5000);
            }
        }
        if (tmp) { R[3] = chan; R[4] = 1; oah_snd_call("SndDisposeChannel", R); }
        memcpy(R + 3, sv, sizeof sv);
        R[3] = (unsigned int)err;
        return 1;
    }
    if (!strcmp(name, "SndChannelStatus")) {       /* (SndChannelPtr, short len, SCStatus *) */
        unsigned char st[24];
        unsigned int len = (unsigned short)R[4], out = R[5];
        oah_snd_pump();
        memset(st, 0, sizeof st);
        pthread_mutex_lock(&g_snd_mx);
        c = oah_snd_find(R[3]);
        if (c) st[12] = c->head != 0;               /* scChannelBusy */
        pthread_mutex_unlock(&g_snd_mx);
        if (out) memcpy((void *)out, st, len < 24 ? len : 24);
        R[3] = c ? 0 : (unsigned int)-205;
        return 1;
    }
    return 0;
}

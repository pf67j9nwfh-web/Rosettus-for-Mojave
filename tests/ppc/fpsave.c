/* fpsave: check that nonvolatile FPRs (f14-f31) survive calls that go
   through the translator's host paths (thread creation, mach traps, CF).
   Build: gcc -arch ppc -O1 -mmacosx-version-min=10.4 -o fpsave fpsave.c -framework CoreFoundation */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <CoreServices/CoreServices.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <netinet/in.h>

static volatile int g_run;
static void *thr(void *a) { g_run = 1; return a; }
static void *thr_sleep(void *a) { usleep(200000); g_run = 2; return a; }

static int fpchk(const char *what, double a, double b, double c) {
    int ok = a == 1.5 && b == 2.25 && c == 3.125;
    printf("%-22s %s  %g %g %g\n", what, ok ? "ok  " : "BAD ", a, b, c);
    return !ok;
}

int main(void) {
    int bad = 0;
    register double a = 1.5, b = 2.25, c = 3.125;
    pthread_t t;
    __asm__ volatile("" : "+f"(a), "+f"(b), "+f"(c));
    pthread_create(&t, 0, thr, 0);
    bad += fpchk("pthread_create", a, b, c);
    pthread_join(t, 0);
    bad += fpchk("pthread_join", a, b, c);
    pthread_create(&t, 0, thr_sleep, 0);
    pthread_join(t, 0);
    bad += fpchk("create+join(sleep)", a, b, c);
    pthread_detach(t);
    usleep(1000);
    bad += fpchk("usleep", a, b, c);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    bad += fpchk("CFRunLoopRunInMode", a, b, c);
    {
        CFRunLoopTimerRef tm = CFRunLoopTimerCreate(0, CFAbsoluteTimeGetCurrent() + 0.01, 0, 0, 0, 0, 0);
        CFRelease(tm);
    }
    bad += fpchk("CFRunLoopTimerCreate", a, b, c);
    { CFDictionaryRef d = SCDynamicStoreCopyProxies(0); if (d) CFRelease(d); }
    bad += fpchk("SCDynamicStoreCopyProxies", a, b, c);
    { struct addrinfo *ai = 0; getaddrinfo("example.com", "80", 0, &ai); if (ai) freeaddrinfo(ai); }
    bad += fpchk("getaddrinfo", a, b, c);
    { struct hostent *h = gethostbyname("example.org"); (void)h; }
    bad += fpchk("gethostbyname", a, b, c);
    { int k = kqueue(); struct kevent ev; struct timespec ts = {0, 1000000}; kevent(k, 0, 0, &ev, 1, &ts); close(k); }
    bad += fpchk("kqueue/kevent", a, b, c);
    { int sk = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in sa = {0}; sa.sin_family = AF_INET; sa.sin_port = htons(80); sa.sin_addr.s_addr = htonl(0x7f000001);
      connect(sk, (struct sockaddr *)&sa, sizeof sa); close(sk); }
    bad += fpchk("socket/connect", a, b, c);
    { CFStringRef h = CFSTR("example.net"); CFHostRef ho = CFHostCreateWithName(0, h); CFStreamError e; CFHostStartInfoResolution(ho, kCFHostAddresses, &e); CFRelease(ho); }
    bad += fpchk("CFHostStartInfoResolution", a, b, c);
    { CFReadStreamRef r; CFWriteStreamRef w; CFStreamCreatePairWithSocketToHost(0, CFSTR("example.com"), 80, &r, &w);
      bad += fpchk("CFStreamCreatePair", a, b, c);
      CFReadStreamOpen(r); bad += fpchk("CFReadStreamOpen(sock)", a, b, c);
      CFReadStreamClose(r); CFRelease(r); CFRelease(w); }
    { CFURLRef u = CFURLCreateWithString(0, CFSTR("http://example.com/"), 0);
      CFHTTPMessageRef rq = CFHTTPMessageCreateRequest(0, CFSTR("GET"), u, kCFHTTPVersion1_1);
      CFReadStreamRef rs = CFReadStreamCreateForHTTPRequest(0, rq);
      bad += fpchk("CreateForHTTPRequest", a, b, c);
      CFReadStreamOpen(rs); bad += fpchk("CFReadStreamOpen(http)", a, b, c);
      CFReadStreamClose(rs); CFRelease(rs); CFRelease(rq); CFRelease(u); }
    printf("RESULT fpsave %s %d\n", bad ? "FAIL" : "PASS", bad);
    return bad != 0;
}

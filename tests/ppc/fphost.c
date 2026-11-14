/* fphost: CFHostStartInfoResolution clobbered f30/f31 under Rosettus (fpsave). Narrow test with markers. */
#include <stdio.h>
#include <unistd.h>
#include <CoreServices/CoreServices.h>

int main(int argc, char **argv) {
    register double a = 1.5, b = 2.25;
    __asm__ volatile("" : "+f"(a), "+f"(b));
    setvbuf(stdout, 0, _IONBF, 0);
    write(2, "=== MARK start\n", 15);
    CFHostRef ho = CFHostCreateWithName(0, CFSTR("example.net"));
    CFStreamError e;
    Boolean r = CFHostStartInfoResolution(ho, argc > 1 ? kCFHostReachability : kCFHostAddresses, &e);
    write(2, "=== MARK end\n", 13);
    printf("r %d err %d/%d  a %g b %g -> %s\n", r, (int)e.domain, (int)e.error, a, b, a == 1.5 && b == 2.25 ? "PASS" : "FAIL");
    return 0;
}

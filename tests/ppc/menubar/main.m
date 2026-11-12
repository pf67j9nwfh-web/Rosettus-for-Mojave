/* PowerPC menu bar test apps for Rosettus (built on Snow Leopard, -arch ppc).
   MODE_DOCK: a regular app (Dock icon, main menu, a window) with a status item.
   MODE_BAR:  LSUIElement=1: only a status item, no Dock icon, no menu bar of its own.
   MODE_ICON: LSUIElement=1, an image status item: a black glyph with a white alternate image for the
              highlight; the menu switches it to a colour icon or to glyph and title.
   MODE_DYN:  starts as MODE_BAR; its status item title counts up every 2 s, its menu is rebuilt each
              time it opens (the time it was opened), and "Show in Dock" / "Hide from Dock" move it
              into and out of the Dock (TransformProcessType 1 / 4).
   Every event is appended to ~/Library/Logs/PPCMenuTest.log ("<app> <event>"). */
#import <Cocoa/Cocoa.h>
#include <Carbon/Carbon.h>
#include <sys/sysctl.h>

#if defined(MODE_DOCK)
#define APPNAME "PPC Menu Dock"
#define TITLE   @"PPC-Dock"
#elif defined(MODE_ICON)
#define APPNAME "PPC Menu Icon"
#define TITLE   @""
#elif defined(MODE_BAR)
#define APPNAME "PPC Menu Bar Only"
#define TITLE   @"PPC-Bar"
#else
#define MODE_DYN 1
#define APPNAME "PPC Menu Dynamic"
#define TITLE   @"PPC-Dyn"
#endif

static void tlog(NSString *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    NSString *s = [[[NSString alloc] initWithFormat:fmt arguments:ap] autorelease];
    va_end(ap);
    NSString *p = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/PPCMenuTest.log"];
    FILE *f = fopen([p fileSystemRepresentation], "a");
    if (f) { fprintf(f, "%ld %s %s\n", (long)time(0), APPNAME, [s UTF8String]); fclose(f); }
}

static const char *arch_name(void) {
    int v = 0; size_t n = sizeof v;
    if (sysctlbyname("sysctl.proc_native", &v, &n, 0, 0) == 0 && v == 0) return "ppc-translated";
#if defined(__ppc__)
    return "ppc";
#else
    return "other";
#endif
}

#ifdef MODE_ICON
static NSImage *glyph(NSColor *c) {      /* a lightning bolt, 18x18 */
    NSImage *im = [[[NSImage alloc] initWithSize:NSMakeSize(18, 18)] autorelease];
    NSBezierPath *b = [NSBezierPath bezierPath];
    [im lockFocus];
    [b moveToPoint:NSMakePoint(10, 17)]; [b lineToPoint:NSMakePoint(3, 8)]; [b lineToPoint:NSMakePoint(8.5, 8)];
    [b lineToPoint:NSMakePoint(6, 1)]; [b lineToPoint:NSMakePoint(15, 11)]; [b lineToPoint:NSMakePoint(9.5, 11)]; [b closePath];
    [c set]; [b fill];
    [im unlockFocus];
    return im;
}
static NSImage *colorIcon(void) {        /* a red disc with a blue ring */
    NSImage *im = [[[NSImage alloc] initWithSize:NSMakeSize(18, 18)] autorelease];
    [im lockFocus];
    [[NSColor colorWithCalibratedRed:0.1 green:0.3 blue:0.9 alpha:1] set];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(1, 1, 16, 16)] fill];
    [[NSColor colorWithCalibratedRed:0.9 green:0.15 blue:0.1 alpha:1] set];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(4, 4, 10, 10)] fill];
    [im unlockFocus];
    return im;
}
#endif
@interface T : NSObject {
    NSStatusItem *item; NSMenu *menu; NSWindow *win; int count; BOOL inDock;
}
@end

@implementation T
- (void)log:(NSString *)what {
    ProcessSerialNumber psn = { 0, kCurrentProcess }; ProcessInfoRec pi; FSSpec fs;
    memset(&pi, 0, sizeof pi); pi.processInfoLength = sizeof pi; pi.processAppSpec = &fs;
    GetProcessInformation(&psn, &pi);
    NSRect fr = NSZeroRect;
    id w = nil;
    @try { w = [item valueForKey:@"window"]; } @catch (id e) { w = nil; }
    if (w && [w respondsToSelector:@selector(frame)]) fr = [w frame];
    int vis = (w && [w respondsToSelector:@selector(isVisible)]) ? [w isVisible] : -1;
    tlog(@"%@ vis=%d arch=%s active=%d bg=%d uielem-flag=%d title=%@ statusframe=%.0f,%.0f,%.0f,%.0f",
         what, vis, arch_name(), [NSApp isActive], (pi.processMode & modeOnlyBackground) ? 1 : 0,
         inDock ? 0 : 1, [item title], fr.origin.x, fr.origin.y, fr.size.width, fr.size.height);
}
- (NSMenuItem *)add:(NSString *)t action:(SEL)a {
    NSMenuItem *m = [[[NSMenuItem alloc] initWithTitle:t action:a keyEquivalent:@""] autorelease];
    [m setTarget:self]; [menu addItem:m]; return m;
}
- (void)build {
    while ([menu numberOfItems]) [menu removeItemAtIndex:0];
    [self add:@"Say Hello" action:@selector(hello:)];
    [self add:@"Open Window" action:@selector(openWindow:)];
#ifdef MODE_ICON
    [self add:@"Mono Icon" action:@selector(iconMono:)];
    [self add:@"Colour Icon" action:@selector(iconColor:)];
    [self add:@"Icon and Title" action:@selector(iconTitle:)];
#endif
#ifdef MODE_DYN
    NSString *ts = [[NSDate date] descriptionWithCalendarFormat:@"%H:%M:%S" timeZone:nil locale:nil];
    NSMenuItem *m = [self add:[NSString stringWithFormat:@"Opened at %@ (count %d)", ts, count] action:nil];
    [m setEnabled:NO];
    [self add:(inDock ? @"Hide from Dock" : @"Show in Dock") action:@selector(toggleDock:)];
#endif
    [menu addItem:[NSMenuItem separatorItem]];
    [self add:@"Quit " APPNAME action:@selector(quit:)];
}
- (void)menuNeedsUpdate:(NSMenu *)m { [self build]; [self log:@"menu-opened"]; }
- (void)hello:(id)s { [self log:@"action hello"]; }
- (void)openWindow:(id)s {
    if (!win) {
        win = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 300, 360, 120)
                                          styleMask:NSTitledWindowMask | NSClosableWindowMask
                                            backing:NSBackingStoreBuffered defer:NO];
        [win setTitle:@APPNAME];
        [win setReleasedWhenClosed:NO];
        NSTextField *t = [[[NSTextField alloc] initWithFrame:NSMakeRect(20, 40, 320, 40)] autorelease];
        [t setStringValue:@APPNAME @" window (PowerPC)"]; [t setEditable:NO]; [t setBordered:NO]; [t setDrawsBackground:NO];
        [t setFont:[NSFont systemFontOfSize:18]];
        [[win contentView] addSubview:t];
    }
    [NSApp activateIgnoringOtherApps:YES];
    [win makeKeyAndOrderFront:nil];
    [self log:@"action open-window"];
}
- (void)toggleDock:(id)s {
    ProcessSerialNumber psn = { 0, kCurrentProcess };
    OSStatus e = TransformProcessType(&psn, inDock ? 4 /* ToUIElement (10.7+) */ : kProcessTransformToForegroundApplication);
    if (!e) inDock = !inDock;
    [self log:[NSString stringWithFormat:@"action toggle-dock err=%d inDock=%d", (int)e, inDock]];
    if (!e && inDock) [NSApp activateIgnoringOtherApps:YES];
}
#ifdef MODE_ICON
- (void)iconMono:(id)s { [item setTitle:@""]; [item setImage:glyph([NSColor blackColor])]; [item setAlternateImage:glyph([NSColor whiteColor])]; [self log:@"action icon-mono"]; }
- (void)iconColor:(id)s { [item setTitle:@""]; [item setImage:colorIcon()]; [item setAlternateImage:nil]; [self log:@"action icon-colour"]; }
- (void)iconTitle:(id)s { [item setImage:glyph([NSColor blackColor])]; [item setAlternateImage:glyph([NSColor whiteColor])]; [item setTitle:@"Icon"]; [self log:@"action icon-title"]; }
#endif
- (void)tick:(NSTimer *)t {
    count++;
    [item setTitle:[NSString stringWithFormat:@"Dyn %d", count]];
    if (count <= 3 || count % 15 == 0) [self log:@"tick"];
}
- (void)probe:(NSTimer *)t { static int n; if (++n <= 10) [self log:@"probe"]; }
- (void)quit:(id)s { [self log:@"action quit"]; [NSApp terminate:nil]; }
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    menu = [[NSMenu alloc] initWithTitle:@"status"];
    [menu setDelegate:self];
    [self build];
    item = [[[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength] retain];
    [item setTitle:TITLE];
#ifdef MODE_ICON
    [item setImage:glyph([NSColor blackColor])];
    [item setAlternateImage:glyph([NSColor whiteColor])];
#endif
    [item setHighlightMode:YES];
    [item setMenu:menu];
#ifdef MODE_DOCK
    inDock = YES;
    [self openWindow:nil];
#endif
#ifdef MODE_DYN
    [NSTimer scheduledTimerWithTimeInterval:2.0 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
#endif
    [NSTimer scheduledTimerWithTimeInterval:3.0 target:self selector:@selector(probe:) userInfo:nil repeats:YES];
    [self log:@"launched"];
}
- (void)applicationWillTerminate:(NSNotification *)n { tlog(@"terminating"); }
- (BOOL)applicationShouldHandleReopen:(NSApplication *)a hasVisibleWindows:(BOOL)v { [self log:@"reopen"]; [self openWindow:nil]; return NO; }
@end

int main(int argc, char **argv) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];
#if defined(MODE_DOCK) || defined(MODE_DYN)
    {   /* a minimal main menu, so the Dock app (and the dynamic one once in the Dock) has an app menu with Quit */
        NSMenu *mb = [[NSMenu alloc] initWithTitle:@""], *am = [[NSMenu alloc] initWithTitle:@APPNAME];
        NSMenuItem *ai = [[NSMenuItem alloc] initWithTitle:@APPNAME action:nil keyEquivalent:@""];
        [am addItemWithTitle:@"Quit " APPNAME action:@selector(terminate:) keyEquivalent:@"q"];
        [ai setSubmenu:am]; [mb addItem:ai]; [NSApp setMainMenu:mb];
        if ([NSApp respondsToSelector:@selector(setAppleMenu:)]) [NSApp performSelector:@selector(setAppleMenu:) withObject:am];
    }
#endif
    T *t = [[T alloc] init];
    [NSApp setDelegate:t];
    [NSApp run];
    [pool release];
    return 0;
}

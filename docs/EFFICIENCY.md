# Efficiency

Measured on a 2013 MacBook Pro with Mojave. The Snow Leopard column is the same app under the real
Rosetta on a 2007 MacBook Pro with Snow Leopard. Approximate figures.

## Idle cost (Hex Fiend, Cocoa, left idle)

| | Rosettus | Snow Leopard |
|---|---|---|
| Idle wakeups per second | 13-19 | ~0 |
| Context switches per second | 44-99 | - |
| Idle CPU | 0.9-1.1 % | 0.0 % |
| Helper daemons when no PowerPC app runs | exit after 30 s | (system daemons) |

* In the background an app settles at about 0.5 % CPU (0.01 s every 2 s). Switching away from it
  costs a short burst of about 0.5-1.5 s of CPU, for the inactive redraw and the clipboard check.
* **Event pump:** the window server is polled every 8 ms only while events arrive, and every 50 ms
  after a second of quiet (`OAH_EV_IDLE_MS`). In the background (another app in front) and after 2 s of quiet,
  every 200 ms (`OAH_EV_BG_IDLE_MS`); apps with menu bar items keep 50 ms. Idle CPU in the
  background: Hex Fiend 13.7 -> 11.3 ms/s (Mojave), 14.8 -> 12.2 ms/s (Catalina); TextWrangler
  24.5 -> 22.8 ms/s (Catalina).
* **Sound output:** once an app had played a sound, the output unit (Mojave) or the thread mixing
  for the host (Catalina, 250 wake-ups a second) ran until the app quit. It now stops after 10 s of
  silence with nothing queued, and the next Sound Manager call starts it again
  (`OAH_SND_IDLE_SECS`). Apps that stream continuously (Kid Pix's drawing screen) keep it running.
* The test-input file is checked once a second instead of four times.
* **Default buttons:** HIToolbox and AppKit 10.6 pulse the default button of the front window
  at 30 frames a second. Carbon default buttons are no longer animated; Cocoa ones draw one
  heartbeat frame every 2 s (that frame is what makes them blue). Idle CPU with the dialog open:
  Word 2004's Project Gallery 120 -> 10 ms/s (Mojave), TextWrangler's Open panel 152 -> 55 ms/s.
  `OAH_BUTTON_PULSE=1` keeps the pulse.
* **Helper agents:** the pasteboard, font server, font worker and coreservicesd start on demand and
  quit 30 s after the last PowerPC app.
* **No Terminal:** apps start directly from Rosettus Launcher, from patched bundles or from launcher
  apps. Nothing stays open after the app quits.

## Memory and launch time

The apps were opened through their launchers, as from the Finder. "Footprint" is the memory macOS
charges to the app (Activity Monitor's Memory column), 20-25 s after launch.

| App | Footprint | First window |
|---|---|---|
| Hex Fiend | 72 MB | 4.1 s |
| AppleWorks 6 | 101 MB | 3.5 s |
| TextWrangler | 150 MB | 9.0 s |
| Microsoft Word 2004 | 124 MB | - |
| Pages '09 | 224 MB | 14.1 s |
| A command-line tool | ~5 MB | |

The shared install (`/Library/Rosettus`) costs nothing over a per-user one.

Most of each app's footprint is made up of:

* the PowerPC frameworks' own data after binding: dyld fix-ups, 30-60 MB in Cocoa apps;
* the guest heap;
* Rosettus' window stores: one desktop-sized bitmap per window, about 5.5 MB each on a 1440x900
  screen.

File-backed pages, such as the PowerPC libraries' code, are shared between apps by the kernel and do
not count toward the footprint.

liboah finds PowerPC Mach-O images by reading guest memory. Images are always mapped from files, so
it reads only file-backed regions. Reading anonymous memory would make the kernel back every page it
touched: about 55 MB in an idle process and 150-230 MB in a GUI app. `OAH_SCAN_ANON=1` scans anonymous
memory too, for comparison.

Other savings:

* native libm, through the Rosetta interposers;
* a fast path for bridge calls;
* a coalescer for CGL flushes;
* skipping window pixels that did not change;
* lazy Gestalt registration.

## Open work

* **Launch-time binding.** Snow Leopard runs PowerPC apps against `dyld_shared_cache_rosetta`. In that
  cache, frameworks are prebound and Objective-C selectors are pre-uniqued. On Mojave every framework
  is bound and fixed up at launch, which runs and translates far more guest code.

  The 10.6 PowerPC cache cannot be reused as it is. It is linked to fixed addresses (0x90000000 for
  text, 0xA0000000 for data) and cannot slide. In a 32-bit Intel process on Mojave, that range holds
  Mojave's own i386 shared cache. Two ways forward: a relocated cache built at a free address, or
  less binding work in the guest at launch.
* **Event port.** `OAH_EV_RECV=1` receives window-server events on the native event port, at about
  13 wakeups a second when idle. It could replace polling once it has been tested more.
* `OAH_MEMREPORT` and the helpers in `tests/` make these measurements repeatable.

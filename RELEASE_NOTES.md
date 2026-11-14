# Rosettus for Mojave release notes

Rosettus 1.0 is now called **Rosettus for Mojave**. The port to macOS 10.15 Catalina is a separate
project, **Rosettus for Catalina**.

## Rosettus for Mojave 1.2

**November 13, 2026 - menu bar apps, iWork charts, and less work in the background.**

**PowerPC menu bar apps work.** Their status items now show in the menu bar (before, none showed),
open their menus and run their commands: apps that are also in the Dock, menu-bar-only apps, and
apps that change their item or move themselves into and out of the Dock while running. Hiding such
an app leaves its item in the menu bar, and in Dark Mode the items' titles are drawn light.
When one of these apps quits, the other PowerPC items move over to close the gap. Items drawn as pictures (a template glyph, a colour icon, or an icon with a title) work as well as text ones, and an app that puts itself in the Dock gets its own menus in the menu bar.

**Textured windows can be moved again.** The windows of Pages, Keynote, Numbers and other apps with textured or unified-toolbar windows did not move when dragged by the title bar or the toolbar background: Snow Leopard's window server moved those itself (AppKit only told it which parts of the window may be dragged), and that part was missing. Rosettus now moves them.

**Charts in Pages, Keynote and Numbers '09.** Inserting a chart showed "An unexpected error has occurred" and 3D charts stayed blank. iWork draws charts with OpenGL into offscreen buffers (pbuffers) that current macOS no longer creates; Rosettus now provides them itself, and copies the finished 3D chart into the document's canvas. 2D and 3D charts are drawn as on Snow Leopard.

**Less work in the background.** An app behind other apps checks for input less often (at most 0.2 s before the click that brings it forward is handled), and an app that has made a sound no longer keeps the sound output running: it stops after 10 s of silence and starts again with the next sound.

**Default buttons no longer pulse.** Snow Leopard animated the default button of every dialog at 30 frames a second, and redrawing it through the translated toolbox cost 10-15 % of a core for as long as a dialog was open (Word 2004's Project Gallery, an Open panel). Default buttons are now drawn blue without the pulse, as on current macOS. `OAH_BUTTON_PULSE=1` brings the pulse back.

**Helper tools stay in the background.** A PowerPC program that is not an application (no .app bundle, no CFM application, no 'APPL' file type) no longer becomes the front app when it draws or connects to the window server, as on Snow Leopard. Office 2004's FontCacheTool ("Optimizing font menu performance", started by Word, Excel and PowerPoint) took the front from the Office app while it started, so its first keystrokes went nowhere. `OAH_TOOL_FOREGROUND=1` restores the old behaviour.

**Floating-point values survive a host-name lookup.** After `CFHostStartInfoResolution` (a synchronous host lookup, also used inside CFNetwork's socket and HTTP streams) the caller's floating-point registers f30 and f31 came back changed (stock Rosetta on Snow Leopard does the same). Rosettus now puts f14-f31 back, as the PowerPC calling convention requires: a first network fetch could time out at once when its start time was kept in one of them. `OAH_NO_FPKEEP=1` turns this off.

**Selection handles in Pages, Keynote and Numbers '09.** A selected text box, shape, picture or chart showed no handles: the small white squares at its corners and edges that show its outline and are dragged to resize it. iWork draws them with OpenGL from one interleaved vertex array (`glInterleavedArrays`), which Rosettus passed to the native OpenGL unconverted (PowerPC byte order) and then switched off again before drawing. It is now converted like the other vertex arrays, and the handles show as on Snow Leopard.

**Version Cue no longer sticks at "Stopping".** When the Version Cue CS2 server hung while shutting down (its database kept running), the Version Cue preference pane showed "Stopping Version Cue CS2..." with its controls disabled for good, and reinstalling Rosettus left the service switched off. The service now ends a shutdown that has not finished after 90 s (the server and its database), so Version Cue can be started again, and the installer always starts the service again, even when it could not replace it. `VC_STOP_TIMEOUT` sets the wait in seconds.

**Games keep running on a Mac that has been up for weeks.** Build-a-lot 2 stopped at the HipSoft splash screen on a Mac that had been running for more than 24.8 days without a restart. Like many games ported from Windows, it keeps time in milliseconds since startup through `AbsoluteToDuration(UpTime())`, and that call can count only up to 2^31 ms (24.8 days); after that it answers "forever" every time, so the game's clock stopped and its frame pacing waited for ever (Snow Leopard does the same). When the Mac has been up for 12 days or more, Rosettus now counts such readings from shortly before the app started, so the clock runs as after a restart. `OAH_NO_UPTIMEFIX=1` turns this off.

**File > Open Recent remembers documents.** In Pages, Keynote, Numbers and other Cocoa apps the Open Recent menu was empty after the app was quit and opened again. Snow Leopard's AppKit keeps that list in the app's own recent-documents list, which Rosettus did not provide; it now comes from macOS's list for that app, as for a native app. The shared lists (the Finder sidebar, Apple menu > Recent Items) are still not changed by PowerPC apps. `OAH_NO_APPRECENT=1` turns this off.

Installing: open **Rosettus for Mojave 1.2.pkg**. It replaces 1.0 or 1.1 in place; launchers and
patched apps keep working. Nothing needs to be imported again.

## Rosettus for Mojave 1.1

**November 9, 2026 - a bug-fix release.**

Two menu bar fixes, brought back from Rosettus for Catalina:

* **A menu title no longer stays highlighted.** Clicking a menu title and then clicking it again to
  close the menu (or choosing an item very quickly) left the title highlighted in blue, and moving
  the mouse along the menu bar then highlighted the other titles too, until the next menu was
  opened. A menu that closes within 0.3 seconds of the click is now noticed shortly afterwards and
  the menu bar goes back to normal.
* **No shadow under the menu bar text.** The titles of a PowerPC app's menus had a faint, blurred
  dark copy of themselves underneath (the shadow of the windows that carry them). It was hard to
  see in Dark Mode but noticeable in Light Mode. The shadow is now turned off
  (`OAH_MB_SHADOW=1` brings it back).

Also in this release:

* The launcher stub of patched apps (`rlaunch`) also recognizes an app already running under
  Rosettus for Catalina's host process, and the private `bash` used for scripts of PowerPC programs
  sends i386-and-PowerPC tools to Rosettus on systems that cannot run 32-bit code. Neither changes anything on Mojave; they let
  the same sources serve Rosettus for Catalina.
* PowerPC command-line tools whose standard error is redirected to a file (`tool 2> errors.txt`)
  no longer get Rosettus's own messages in that file; they go to `~/Library/Logs/Rosettus/tools.log`
  as they already did for a terminal or a pipe.
* The README describes the project in more detail, with a new section on running PowerPC
  command-line tools from Terminal.

Installing: open **Rosettus for Mojave 1.1.pkg**. It replaces 1.0 in place; launchers and patched
apps keep working. Nothing needs to be imported again.

## Rosettus 1.0

**October 31, 2026 - the first release.**

Rosettus runs PowerPC Mac applications on macOS 10.14 Mojave, using the Rosetta translator from your
own Mac OS X 10.6 Snow Leopard installation. No Apple software is included: Rosettus copies the files
Rosetta needs from a Snow Leopard Mac or a Snow Leopard install DVD.

### What's included

* **Rosettus Launcher** (in `/Applications`): imports the Snow Leopard files, finds PowerPC apps,
  makes launchers for them, and opens them with a double-click, from the Dock or from the Finder.
* **Installer package** `Rosettus 1.0.pkg`: installs for every account on the Mac. A setup kit goes
  to `/Library/Rosettus/Setup`, and three folders to `/Applications/Rosettus`:
  * *Snow Leopard Export Kit*: copy it to the Snow Leopard Mac to make the export.
  * *Snow Leopard Install DVD Import*: makes the same export on Mojave from a Snow Leopard install
    DVD or disc image and Apple's 10.6.8 updates, without a Snow Leopard Mac.
  * *PowerPC Java Import*: builds PowerPC Java 1.3.1, 1.4.2 and 5.0 from Apple's own Java updates,
    downloaded from Apple's Software Update servers (optional).
* **The shim library** (`liboah_mojave.dylib`), which connects the PowerPC system frameworks to
  Mojave's window server, events, audio, OpenGL, clipboard, Launch Services and Apple Events.

### What works

Apps tested include:

* Microsoft Office 2004: Word, Excel and PowerPoint, with VBA macros and embedded objects.
* iWork '09, AppleWorks 6, Internet Explorer 5.2 and TextWrangler.
* Adobe Illustrator, InDesign and GoLive CS2.
* SimCity 4, Bejeweled 2 and Blender 2.63.
* Hex Fiend, Transmission and Kid Pix Deluxe 4.

Other things that work:

* Hardware and system services: printing, sound output and input, QuickTime movies, OpenGL, MIDI,
  the camera, HID game devices, the keychain and FSEvents.
* Documents opened from the Finder.
* Copy and paste with Mac apps.
* Old installer packages, screen savers, Dashboard widgets and preference panes.

[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) lists every app and feature tested, and what is known
not to work.

### Requirements

* macOS 10.14 Mojave. 32-bit code does not run on 10.15 or later.
* About 4 GB free.
* Mac OS X 10.6.8 with Rosetta, or a Snow Leopard install DVD or disc image.

### Known issues

* PowerPC Java:
  * A Java program that dereferences `null` (or otherwise relies on the JVM's own fault handling)
    crashes the JVM instead of getting a `NullPointerException`.
  * Java 1.3.1's garbage collector crashes under heavy allocation with several threads.
  * Java 5 built for Leopard (1.5.0_30 from Java for Mac OS X 10.5 Update 10, 1.5.0_13 from a Leopard
    DVD) crashes in its garbage collector under load. The Java import therefore installs Tiger's
    1.5.0_19 by default.
* Edit > Special Characters does nothing: there is no Character Viewer for PowerPC apps.
* Fonts that Office 2004 keeps in `~/Library/Fonts` are not available to it.
* Change notifications are not delivered for some services: network configuration, power sources
  and MIDI devices.
* AppleWorks 6 cannot open RTF files saved by current apps (a limit of its RTF translator).
* Illustrator and InDesign CS2 must be opened through their launchers (Mojave refuses them as apps that
  need Java 6).

For help, see [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md). Logs are in
`~/Library/Logs/Rosettus/`.

# Rosettus for Mojave

Rosettus for Mojave (formerly Rosettus) is a compatibility layer that allows you to run PowerPC Mac applications on **macOS 10.14 Mojave**, using the Rosetta translator from your own
**Mac OS X 10.6 Snow Leopard** installation or installation disc.

Apple removed Rosetta in Mac OS X 10.7 as an OS component but Rosettus restores almost all of that functionality.
It adds a shim library layer (`liboah_mojave.dylib`) that bridges the old PowerPC system frameworks to be compatible with
Mojave's window server, event system, audio, OpenGL, pasteboard, Launch Services and Apple Events.

Apps confirmed working include AppleWorks 6, Microsoft Office 2004 (Word, Excel, PowerPoint, with
VBA macros and embedded equations, charts and organization charts),
iWork '09 (via their PPC slices since they're universal), Bejeweled 2, SimCity 4 (PPC), Build-a-lot, Internet Explorer 5.2, Blender 2.63 (PPC),
TextWrangler, Adobe CS2, Transmission, and Hex Fiend. See [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

| | |
|---|---|
| ![Microsoft Word 2004 on Mojave](docs/images/app-word2004.png) | ![Internet Explorer 5.2 on Mojave](docs/images/app-internet-explorer.png) |
| Microsoft Word 2004 (Classic Carbon, single-file app) | Internet Explorer 5.2 (Carbon CFM) |
| ![Bejeweled 2 on Mojave](docs/images/app-bejeweled2.jpg) | ![AppleWorks 6 on Mojave](docs/images/app-appleworks.png) |
| Bejeweled 2 Deluxe (OpenGL, sound) | AppleWorks 6, patched to open with a double-click |

> **No Apple software is included in this repository.** You need a Mac running Snow Leopard 10.6
> with Rosetta installed or a Snow Leopard installation disc. A script copies the files Rosetta needs from it. 
> The repository holds only  this project's own code, a list of the files to copy (names and checksums), 
> and small binary patches containing only the bytes this project changes.

**Rosettus for Mojave 1.1** is the current release: see [RELEASE_NOTES.md](RELEASE_NOTES.md).
For macOS 10.15 Catalina there is a separate port, **Rosettus for Catalina**.

## Requirements

| | |
|---|---|
| Mojave Mac | macOS 10.14 (32-bit code no longer runs on 10.15 or later), about 4 GB free |
| Snow Leopard Mac | Mac OS X 10.6.8 with Rosetta (any Intel Mac that runs 10.6, or a virtual machine on Apple hardware), **or** a Mac OS X 10.6 install DVD or disc image (see [Without a Snow Leopard Mac](#without-a-snow-leopard-mac-install-dvd)) |

## Installs

1. **On Snow Leopard**, copy this repository over (or `git clone` it) and run:

   ```
   cd rosettus/import/sl
   ./export_from_sl.sh
   ```

   If Rosetta is not installed, the script opens **Install Rosetta.app**. That small PowerPC app
   makes Snow Leopard offer its *"To open Install Rosetta, you need to install Rosetta"* download.
   Accept it, then run the script again. Once Rosetta is there, the app itself runs and offers
   to start the export. The result is `~/Desktop/Rosettus-SL-export.tar.gz` (5-15 minutes).

2. **Copy the archive to the Mojave Mac** and, from the repository folder there, run:

   ```
   ./install.sh ~/Downloads/Rosettus-SL-export.tar.gz
   ```

   This unpacks the files into `~/Library/Rosettus`, checks every file against its checksum and
   applies the patches. It also installs **Rosettus Launcher** into `~/Applications`. No
   administrator password is needed.

3. Open **Rosettus Launcher**, click **Search This Mac**, then **Patch All…**. From then on your
   PowerPC apps open like any other app.

### Without a Snow Leopard Mac (install DVD)

The export can also be made on the Mojave Mac itself, from a **Mac OS X 10.6 Snow Leopard install
DVD** (the disc, or an `.iso`/`.dmg` image of it) and Apple's free Snow Leopard updates, which the
tool downloads from Apple's servers (about 1.5 GB: 10.6.8 Combo v1.1, 10.6.8 Supplemental, Security
Update 2013-004, Rosetta, Safari 5.1.10, Java for Mac OS X 10.6, Mac App Store update). Double-click
**Applications › Rosettus › Snow Leopard Install DVD Import › Import from Install DVD.command**
(installed by the package), or from the repository:

```
cd rosettus/import/dvd
./import_from_dvd.sh --dvd ~/Downloads/SnowLeopardInstall.iso --download
```

It unpacks the DVD's system packages and the updates into a work folder (nothing is installed;
20-40 minutes, 12 GB free while it runs), decodes Rosetta's translator by starting the 32-bit Intel
`translate` from the DVD under Mojave with a small library that saves its decrypted code, then
collects the files exactly as `export_from_sl.sh` does. The result is the same
`~/Desktop/Rosettus-SL-export.tar.gz`; continue with step 2. An install DVD carries other builds of
some files than an updated Snow Leopard Mac, so `install.sh` notes a few thousand "different build"
files; the files Rosettus patches have patches for both known builds (`*.alt1.rpatch`). Tested
with the 10.6.7 disc of the 2011 MacBook Pro (691-6812-A) plus the updates: installed into its
own folder, TextWrangler, AppleWorks, Word, Kid Pix and Photoshop start and all peripheral tests pass. See `import/dvd/README.txt`.

### Installing for every account (installer package)

`tools/mkpkg.sh VERSION` builds **`dist/Rosettus for Mojave VERSION.pkg`**, a standard installer package that
installs for all users of the Mac:

* **Rosettus Launcher** goes to `/Applications`, and a setup kit (scripts, patches, checksums;
  no Apple files) to `/Library/Rosettus/Setup`.
* If `Rosettus-SL-export.tar.gz` is next to the package, or in the Downloads or Desktop
  folder of the user running it, the package imports it into **`/Library/Rosettus`**. Otherwise
  Rosettus Launcher offers **Install Snow Leopard Files…** the first time it opens (File menu);
  you choose the archive and enter an administrator password in the system dialog.
* The helper agents are installed in `/Library/LaunchAgents`, so they run for whoever logs in.

With the shared install, an app patched by an administrator opens through Rosettus for every
account, and launchers made by Rosettus Launcher go to `/Applications/Rosettus`. The same from
Terminal: `sudo ./install.sh --system Rosettus-SL-export.tar.gz`; remove it with
`sudo ./uninstall.sh --system`. A per-user install in `~/Library/Rosettus` keeps working next to it
(patched apps use the per-user install when that account has one, otherwise the shared one).

## Using it

### Rosettus Launcher

![Rosettus Launcher](docs/images/launcher.png)

Rosettus Launcher lists the PowerPC programs in `/Applications`, `~/Applications`, `~/Downloads`, and
the screen saver folders.

**Patch All…** patches every listed app that can be changed, after asking.

**File ▸ Patch New PowerPC Apps Automatically** turns on a small background agent for your
account. Whenever a PowerPC-only app appears in `/Applications` or `~/Applications` (copied from
a disc or disk image, or installed by an old installer), it is patched within a few seconds, so it
opens with a double-click as it did on Snow Leopard. Universal apps, apps you cannot change and
apps already patched are left alone; the log is `~/Library/Logs/Rosettus/autopatch.log`.

> **Why patching at all?** On Snow Leopard the kernel itself noticed a PowerPC executable and
> started Rosetta. Mojave's kernel no longer has that code and refuses PowerPC programs ("Bad CPU
> type in executable"); it cannot be added back from outside the kernel, with or without System
> Integrity Protection. Turning SIP off would only allow hooking individual launch paths (Finder,
> Dock, Terminal...) in every process, which would be fragile and weaken the system's security, so
> Rosettus does not do that. Patching adds a tiny native launcher to the app and keeps the
> original intact (Unpatch restores it), and automatic patching makes it hands-off.

For each item:

| Button | What it does |
|---|---|
| **Open** | Runs the app under Rosettus. There is no Terminal window, and output goes to `~/Library/Logs/Rosettus/<App>.log`. |
| **Patch** | Changes the app so a normal double-click (Finder, Dock, Spotlight, *Open With*, documents) runs it through Rosettus. Mach-O apps get a native launcher slice next to their PowerPC code; Carbon CFM bundles (AppleWorks, Internet Explorer) get a native launcher executable and an Info.plist entry pointing at it. The originals are kept inside the app, and **Unpatch** restores them exactly. Single-file Classic apps (Office 2004) cannot carry a launcher, so for them Patch makes one in `~/Applications/Rosettus`. |
| **Launcher** | Creates a separate launcher app in `~/Applications/Rosettus` and leaves the app untouched. Use it for apps on read-only disks (installer CDs), which Patch All skips. |
| **Install** | For PowerPC installer packages, screen savers and Dashboard widgets (see below). |
| **Show Log** | Opens the last run's log in Console. |

**Universal apps.** An app with both Intel and PowerPC code runs natively, so the launcher does not list it.

To run its PowerPC version anyway, either turn on **File > Show Universal Apps (Run Their PowerPC Version)**, or drop the app on the window (or add it with **Add…**). It is then listed as *Universal*:

* **Open** runs the PowerPC version.
* **Launcher** makes a separate *<App> (PowerPC)* app in `Applications/Rosettus` that always does so.
* **Remove** moves that launcher to the Trash.

The app itself is never patched. The PowerPC launcher has its own bundle identifier and no document types, so Finder, Spotlight and documents keep opening the app natively.

This is useful when an app's Intel version misbehaves, or when it needs a PowerPC-only plug-in. From Terminal, `rosettus /path/App.app` runs the PowerPC version of a Universal app too.

From Terminal (`R` is `/Library/Rosettus` for the installer package's shared install, or
`~/Library/Rosettus` for a per-user install):

```
$R/rosettus /Applications/SomeApp.app            # run an app
python $R/rosettus_patch.py /Applications/SomeApp.app       # patch
python $R/rosettus_patch.py --unpatch /Applications/SomeApp.app
```

### Command-line tools

PowerPC command-line programs (Unix tools, compilers, helpers) run through the same `rosettus`
command. Mojave's kernel cannot start them itself: typed directly, a PowerPC tool fails with
*Bad CPU type in executable*.

```
/Library/Rosettus/rosettus /path/to/ppc-tool arg1 "arg 2"        # shared install
~/Library/Rosettus/rosettus ./ppc-tool --help                      # per-user install, relative path
some-command | /Library/Rosettus/rosettus /path/to/ppc-filter > out.txt
```

The tool runs like a native one: it gets its arguments, environment and current folder, reads
standard input (a pipe, a file, or the terminal for interactive tools), writes standard output and
error, and `rosettus` exits with the tool's exit status, so it works in scripts and pipelines. Ctrl-C
stops it as usual. Rosettus's own messages do not mix with the tool's output: they go to
`~/Library/Logs/Rosettus/tools.log` (`OAH_TOOL_STDERR=1` keeps them on stderr, see
[docs/ENVIRONMENT.md](docs/ENVIRONMENT.md)).

To type just the tool's name, put `rosettus` on your `PATH`, or wrap a tool in a small script:

```
sudo ln -s /Library/Rosettus/rosettus /usr/local/bin/rosettus   # then: rosettus ppc-tool ...
printf '#!/bin/sh\nexec /Library/Rosettus/rosettus /path/to/ppc-tool "$@"\n' > ~/bin/ppc-tool
chmod +x ~/bin/ppc-tool                                              # then: ppc-tool ...
```

Tools that a PowerPC app starts itself (helper commands, shell scripts and the PowerPC programs they
run) need none of this: Rosettus sends them through the translator automatically (see *Helper
programs and scripts* below). For PowerPC Java there is `rjava` (see [PowerPC Java](#powerpc-java-optional)).

### Running PowerPC apps

A patched or launcher-started app behaves like a native one: it has its own Dock icon and name,
appears in Cmd-Tab and Force Quit, receives the documents Finder and other apps hand to it, and
answers Apple Events (a few apps still mishandle documents opened this way; see
[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md)). Its menus appear in Mojave's own menu bar while it is frontmost:

![AppleWorks menus in Mojave's menu bar](docs/images/menu-appleworks-format.png)

**Clipboard.** Copy and paste work in both directions between PowerPC and native apps. A copy in
a PowerPC app reaches Mojave's clipboard within about half a second; Classic apps that publish
their clipboard only when you switch away (AppleWorks) do it then, and their Mac Roman text is
converted to Unicode. Data an app only promises to provide later (Hex Fiend's large copies) is
not carried over.

**Drag and drop.** Files dragged from Finder (or any native app) onto a PowerPC app's window open
in that app, as if they were dropped on its Dock icon. Dragging inside a PowerPC app works too
(moving selected text in TextWrangler, for example). Drags from a PowerPC app to a native app, and
text or pictures dragged from native apps into a PowerPC app, are not carried over yet.

**Retina displays.** The titles of a PowerPC app's menus in Mojave's menu bar are drawn natively
at full Retina resolution (in Lucida Grande, the font the app laid them out with), so they are as
sharp as a native app's. Window contents are drawn at 1x and scaled up by the window server, as
they were for every Rosetta app on Snow Leopard. Sharp window text is
experimental: turn on **File > Sharp Text on Retina Displays (Experimental)** in Rosettus Launcher
(or set `OAH_HIDPI=1 OAH_CUISCALE=1`, see [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md)) and PowerPC
apps draw their windows at 2x, shown on screen through an OpenGL surface of each window: text and
Quartz drawing are as sharp as in a native app (tested: Hex Fiend, TextWrangler, Keynote,
AppleWorks, Bejeweled 2). Carbon title bar backgrounds are drawn at 1x and scaled, and QuickDraw
content stays 1x. It applies to apps opened after the switch.

**Helper programs and scripts.** PowerPC apps that run helper commands (NSTask) or shell scripts
work as on Snow Leopard, and a script started by a PowerPC app can itself run PowerPC tools: such
scripts run with a private copy of `/bin/bash` that sends PowerPC programs to Rosettus (Adobe CS2's
installer depends on this).

**X11 apps.** PowerPC X11 programs (GTK apps such as Wireshark 1.x) use Snow Leopard's X11
libraries from the export and draw through [XQuartz](https://www.xquartz.org) 2.7.11, the
Mojave-compatible X server. Install XQuartz, log out and back in once, and open the app as usual.

**One copy per app.** Opening a PowerPC app that is already running (from its launcher, the
patched app, the Dock or a document in Finder) brings the running copy to the front and hands it
the documents, as with any Mac app, even when it was started through a different launcher.

Background helpers that old apps start for themselves (for example Office 2004's *Microsoft
Database Daemon*) stay in the background, as their `SIZE` resource or Info.plist asks, so they
never take keyboard focus away from the app.

When an app quits, everything started for it ends too. The translated helper daemons (pasteboard,
font server, Launch Services) are launchd agents that start on demand and exit 30 seconds after the
last PowerPC app quits.

| | |
|---|---|
| ![TextWrangler](docs/images/app-textwrangler.png) | ![Fractal Explorer](docs/images/app-fractal-explorer.png) |
| TextWrangler 3 (PowerPC Cocoa) | Fractal Explorer (a PowerPC test app built for this project) |
| ![AppleWorks Help](docs/images/help-appleworks.png) | |
| AppleWorks 6 Help in Help Viewer | |
| ![Wireshark](docs/images/app-wireshark.png) | ![Illustrator CS2](docs/images/app-illustrator-cs2.jpg) |
| Wireshark 1.9.2 (PowerPC GTK) through XQuartz | Illustrator CS2, installed from the CS2 Premium discs |
| ![InDesign CS2](docs/images/app-indesign-cs2.jpg) | ![GoLive CS2](docs/images/app-golive-cs2.jpg) |
| InDesign CS2 | GoLive CS2 |
| ![Mini vMac](docs/images/app-minivmac.png) | ![Festive Lights widget](docs/images/widget-festive-lights.jpg) |
| Mini vMac (PowerPC build) running System 7.5.3 | Festive Lights, an HTML Dashboard widget, on Mojave's Dashboard |
| ![Kid Pix Deluxe 4](docs/images/app-kidpix4.png) | ![Photoshop CS2 Open panel](docs/images/photoshop-open-panel.png) |
| Kid Pix Deluxe 4 (it insists on a thousands-of-colors screen; Rosettus reports one) | Photoshop CS2's Open panel: disk icons and scrolling as on 10.6 |

Old installer applications work too, including Installer VISE, StuffIt InstallerMaker and e-on's
own installer, also when they restart themselves with administrator rights:

| | | |
|---|---|---|
| ![Logic Platinum 5.4 installer](docs/images/installer-logic.png) | ![Kid Pix Deluxe 4 installer](docs/images/installer-kidpix.png) | ![Vue 5 Esprit installer](docs/images/installer-vue.png) |
| Logic Platinum 5.4 (Installer VISE, CFM) | Kid Pix Deluxe 4 (StuffIt InstallerMaker, CFM) | Vue 5 Esprit (e-on installer, Mach-O) |

### PowerPC Java (optional)

Snow Leopard has no PowerPC Java, so it is not part of the Snow Leopard export. Rosettus runs Apple's
PowerPC Java 1.3.1, 1.4.2 and 5.0, and can build them from Apple's own installers - no PowerPC Mac needed:

1. Double-click **Import PowerPC Java.command** in `/Applications/Rosettus/PowerPC Java Import`
   (`import/java` in the repository). It downloads Apple's last Tiger Java updates from Apple's
   Software Update servers (about 130 MB, checked against known checksums), builds
   `Rosettus-Java-PPC.tar.gz` on the Desktop and installs it (it asks for an administrator password):

   | Apple update | gives |
   | --- | --- |
   | Java 1.3.1 and 1.4.2 Release 2 | Java 1.3.1_16 |
   | Java for Mac OS X 10.4 Release 9 | Java 1.4.2_21 and 1.5.0_19 |

   Java 5 built for Leopard (Java for Mac OS X 10.5 Update 10, 1.5.0_30, fetched with
   `fetch_java_update.sh --update10`) starts and runs small programs but crashes under load in its
   garbage collector, so it is not installed by default.

   Without Internet access, a Mac OS X 10.5 Leopard install DVD image works too
   (`import_java_from_media.sh "Leopard.iso"`: Java 1.4.2_16 and 1.5.0_13). A Tiger DVD is the weaker
   source (1.3.1_15, which does not start under Rosettus, and 1.4.2_07). See `import/java/README.txt`.
2. Or, on a PowerPC Mac with Tiger or Leopard, run `import/tiger/export_java_from_tiger.sh` and install
   the archive it writes with `/Library/Rosettus/tools/install_java.sh Rosettus-Java-PPC.tar.gz`.
3. Run Java programs with `/Library/Rosettus/rjava`: `rjava -version`, `rjava -jar App.jar`,
   `rjava javac Hello.java`, or `rjava -j 1.4 ...` for an older version.

The runtimes go into Rosettus' PowerPC system folder; Mojave's own Java is not touched.

Command-line programs run well (Java 5's compiler, threads, collections), and so do Java 5 Swing
windows:

![A Swing window from PowerPC Java 5 on Mojave](docs/images/java-swing.png)

Known issue: a program
that dereferences `null`, and larger programs such as the PowerPC Minecraft port, crash the JVM -
see [Compatibility](docs/COMPATIBILITY.md).

### Installer packages, screen savers, widgets, preference panes

* **Installer packages**: most old packages install with Mojave's Installer as they are. For
  packages with PowerPC-only scripts or tools, **Install** makes a copy in which those run through
  Rosetta and PowerPC-only Installer plug-ins are left out, then opens it in Installer, which
  asks for the administrator password itself. When a PowerPC installer app (Adobe CS2) runs
  `/usr/sbin/installer` on its own packages, Rosettus puts its `installer-shim` in between: the
  packages' PowerPC `preflight`/`postinstall` scripts then run through Rosetta, and the
  progress text reaches the installer's window.
* **Installer applications** (Installer VISE, StuffIt InstallerMaker, Adobe's and e-on's
  installers) run like any other app. Those that need administrator rights ask for the
  password with the normal macOS prompt and restart themselves as root under Rosettus.
  Mac OS 9 ("Classic") installers and apps cannot run; Rosettus says so instead of starting them.
* **Screen savers**: PowerPC `.saver` bundles cannot load into Mojave's screen saver engine.
  **Install** wraps one in a small native screen saver that runs the original through Rosetta
  inside the saver's view. It then appears in *System Preferences > Desktop & Screen Saver*.

  ![A PowerPC screen saver running on Mojave](docs/images/screensaver.png)

* **Dashboard widgets**: HTML/JavaScript widgets work on Mojave already, so the launcher only lists
  widgets that have a PowerPC plug-in. **Install** copies such a widget into `~/Library/Widgets`
  and detaches the plug-in (which cannot load into Mojave's 64-bit Dashboard).
  See [docs/SAVERS_WIDGETS.md](docs/SAVERS_WIDGETS.md).
* **Preference panes**: PowerPC `.prefPane` bundles cannot load into Mojave's System
  Preferences. **Rosettus Preference Pane Viewer** (in `/Applications`) lists the PowerPC panes in
  the PreferencePanes folders and opens each in its own PowerPC host; **Other…** opens one from
  anywhere. Rosettus Launcher's **Preference Panes…** button (also in its File menu) opens
  the Viewer. Adobe Version Cue CS2's pane and its background server run as well. See
  [docs/PREFERENCE_PANES.md](docs/PREFERENCE_PANES.md) and [docs/VERSION_CUE.md](docs/VERSION_CUE.md).

## How it works (short version)

* Snow Leopard's `translate` (Rosetta) runs unchanged on Mojave, apart from a 38-byte patch and an
  ad-hoc signature. It translates PowerPC code to x86 and runs the **PowerPC** system frameworks from
  10.6 (the guest tree: Carbon, Cocoa, QuickDraw and others) in the same process.
* About 2,000 framework functions are **bridged**. Their first instruction becomes a trap that
  `liboah_mojave.dylib` handles natively on Mojave (window server calls, OpenGL, CoreAudio,
  Launch Services, authorization). Everything else runs as translated 10.6 code.
* Services that Mojave no longer provides in a form 10.6 understands (pasteboard, font server,
  coreservicesd) run as translated 10.6 daemons, under launchd agents named `com.rosettus.ppc.*`.

* A typical app uses 70-150 MB (Hex Fiend 72 MB, AppleWorks 101 MB, Word 124 MB) and shows its
  first window in 3.5-9 s. The PowerPC libraries are mapped from files, so the kernel shares their
  pages between apps.

More detail: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Performance measurements and open work:
[docs/EFFICIENCY.md](docs/EFFICIENCY.md).

## Troubleshooting

* Every run writes `~/Library/Logs/Rosettus/<App>.log`.
* Crashes and hangs are captured automatically in `~/Library/Logs/Rosettus/Reports/`, with PowerPC
  and native backtraces. Unbridged calls are recorded in `~/Library/Logs/Rosettus/Telemetry/`.
* `OAH_*` environment variables switch individual fixes off or turn on tracing:
  [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md).
* [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) covers common problems.

## Building from source

The prebuilt binaries in `bin/` are built from `src/`:

| Component | Built on | Command |
|---|---|---|
| `liboah_mojave.dylib` (i386) | Snow Leopard + Xcode 3.2.6 | `src/liboah/build.sh` |
| `rlaunch` (x86_64 patched-app stub) | Mojave + Command Line Tools | `src/rlaunch/build.sh` |
| `Rosettus Launcher.app` | Mojave + Command Line Tools | `src/launcher/build.sh` |
| `Install Rosetta.app`, `dump_text_ppc` (PowerPC) | Snow Leopard + Xcode 3.2.6 | `src/trigger/build.sh` |
| Saver host, test saver (PowerPC) | Snow Leopard + Xcode 3.2.6 | `src/savers/build_ppc.sh` |
| `RosettusSaverShim.saver` (x86_64) | Mojave + Command Line Tools | `src/savers/build_shim.sh` |
| `libppcexec.dylib` (x86_64) | Mojave + Command Line Tools | `src/ppcexec/build.sh` |
| `libjavashim.dylib` (PowerPC) | Snow Leopard + Xcode 3.2.6 | `src/javashim/build.sh` |
| Preference Pane Viewer and Host | Mojave / Snow Leopard | `src/prefpanes/build.sh viewer`, `... host` |
| Version Cue helpers | see [docs/VERSION_CUE.md](docs/VERSION_CUE.md) | `src/versioncue/build.sh` |

`bin/SOURCES.sha1` records which sources each prebuilt file was built from. After rebuilding one
and copying it into `bin/`, run `tools/binstamp.sh --update NAME` (for example
`tools/binstamp.sh --update liboah_mojave.dylib`); `tools/binstamp.sh --check` lists files whose
sources changed since, and `tools/mkpkg.sh` will not build a package while any are listed.

The shim has to be built on Snow Leopard because Mojave's toolchain can no longer link 32-bit
Intel code. After changing `bridges/bridges.spec`, run `python genbridges.py bridges.spec` and
`bash bridges_patch.sh` in the install folder. Append new spec entries at the end, because IDs are
assigned in order. A new mark also changes the guest library itself: regenerate its
`import/patches/<path>.rpatch` with `import/rpatch.py make` and its line in `import/expected.sha1`. Tests: `tests/difftest.sh` (differential, against Snow Leopard reference output),
`tests/smoke.sh`, `tests/behave_all.sh` (`tests/behave_clean.sh` kills leftover test apps between apps) and
`tests/racetest.sh` (repeated launches of a launcher). `tests/periph/periph.sh` checks peripherals
from PowerPC code (speakers, microphone, USB and serial devices, MIDI, the camera, QuickTime movies, Mach
thread calls, HID devices with `hid` and `hidold`, and with `services`: the keychain, Bonjour, FSEvents, HTTP, network configuration, preferences, power
sources) against the same programs built natively, `tests/blender/run.sh` uses Blender to
model, animate, render and save a scene, with and without its window, and `tests/blender/bge_run.sh` runs a small
physics game in blenderplayer and checks its lighting from the game's own screenshot.

### Spelling in PowerPC Cocoa apps

Spell checking uses Snow Leopard's own spell server (AppleSpell.service), which Rosettus starts
translated the first time an app needs it. It is part of the Snow Leopard export. If an install lacks it, copy it from the Snow Leopard Mac:
`/System/Library/Services/AppleSpell.service` to `/Library/Rosettus/System/Library/Services/`.

### Optional: Flash in QuickTime

QuickTime 7.3.1 and later (Snow Leopard's included) cannot play Flash (.swf) movies, so PowerPC
apps that use Flash through QuickTime show empty boxes; Kid Pix Deluxe 4's animated stickers are
one example. Rosettus draws simple Flash itself, but QuickTime 6's real Flash 5 player is more
complete (sounds, all of Flash 5). If you have a QuickTime 6 for Mac OS X installer (the Kid Pix
Deluxe 4 disc has "QuickTime 6.5.1 OSX/QuickTime651.pkg"), add its Flash components to Rosettus:

    bash /Library/Rosettus/tools/install_qtflash.sh "/path/to/Kid Pix 4.iso"

The source can be the disc image, the mounted disc, the .pkg or a QuickTimeComponents.component.
It needs the Xcode Command Line Tools (Rez/DeRez). Only the Flash parts are used, from your own
copy; Rosettus does not include them. Delete
/Library/Rosettus/System/Library/QuickTime/RosettusFlash.component to remove it.

## Legal

Rosettus is not affiliated with Apple. Rosetta, Mac OS X and the frameworks it uses are Apple's
software. They are copied only from a Snow Leopard installation that you own, onto your own Mac,
for interoperability with your own PowerPC software. `src/liboah/cp_data.h` is the PowerPC commpage
from xnu (Apple Public Source License 2.0). Everything else in this repository is under the MIT
License ([LICENSE](LICENSE)). Screenshots show third-party applications for illustration only;
they belong to their respective owners.


### Future Plans

This initial release should be pretty well put together from what I can tell, but if this gets a broader audience and issues are found;
I will try to update and maintain this.  Getting to Mojave, isn't the ultimate goal though.  I want to try to work on getting this shim
across the hurdle of the jump to Catalina.  If I can make a 32-bit compatibility layer and get it working on a Catalina, I'm going to try
to take it even further all the way to Golden Gate.  These are big ifs though and there are not promissies.

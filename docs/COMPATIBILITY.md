# Compatibility

Tested on a 2013 MacBook Pro with macOS 10.14.6 and the Rosetta from Mac OS X 10.6.8.
"Works" means the app starts, its main features were used, and it quits cleanly.

The behavior script (`tests/behave.py`) launches each app, makes a new document, types, copies and
pastes, opens preferences, hides and shows the app, and quits without saving. Its clipboard check is
timing sensitive, so a "type+copy" miss below is usually that check rather than the app.

| App | Kind | Status | Notes |
|---|---|---|---|
| AppleWorks 6.2.9 | Carbon CFM | Works | Word processing, drawing, painting and spreadsheets work. Printing works: QuickDraw print jobs are converted with Snow Leopard's pictwpstops. Patched in place, it opens with a double-click. Plain-text files handed over by the Finder open as a new word-processing document. The text tool and word processing show the I-beam pointer. Save As proposes "untitled.cwk". Multi-page word-processing documents show their pages and page breaks. They scroll with the scroll bar, Page Up/Down, and the scroll wheel or trackpad (AppleWorks never handled the wheel itself, so Rosettus clicks the scroll arrow for it). |
| Microsoft Word / Excel / PowerPoint 2004 | Carbon CFM (single-file apps) | Works | Project Gallery, documents and typing work. A new document draws within a few seconds. Patch / Patch All makes launchers in `~/Applications/Rosettus`, and the Database Daemon helper stays in the background. Help > Word Help opens the Office Help viewer. Alert sheets ("Do you want to save the changes?") show their text and buttons. Documents opened from the Finder open at launch, and in an app that is already running. Apps opened from the Dock or Finder start after another Office app crashed, because stale shared memory is cleared. Insert > Object works for all seven object types: Equation, Excel Chart, Excel Worksheet, Graph Chart, Organization Chart, Word Document and Word Picture. VBA macros and the Visual Basic Editor work; an incomplete OLE registry is rebuilt once automatically. PowerPoint slide shows work. Fonts that Office keeps in ~/Library/Fonts are not available (see below). |
| Microsoft Entourage 2004 | Carbon CFM | Works | Mail, address book and calendar views work. Help > Entourage Help opens the Office Help viewer. Mail accounts were not tested. A reopen (Dock click, double-click, `open`) during start-up is held until the main window is up. Double-clicking a message opens it. New event and new message windows, typing, and the Discard / Don't Save alerts were checked. |
| Office 2004 Equation Editor | Carbon CFM | Works | Launches and edits equations. Its "MT Extra" font in ~/Library/Fonts is found, because user fonts reach PowerPC apps. |
| Pages, Keynote, Numbers, iWeb ('09) | Cocoa | Works | The Fonts and Colors panels work, including the Colors magnifier. Edit > Special Characters does nothing (there is no Character Viewer for PowerPC apps). Keynote theme previews and slideshows work. Numbers tables and charts recalculate. Close and Quit ask to save an edited document. Pages opens Word .doc files, Keynote opens PowerPoint .ppt files and Numbers opens Excel .xls files. Exports to Word, PowerPoint and Excel work. |
| Internet Explorer 5.2 | Carbon CFM | Works | Browsing, typed addresses and local files work. Pages handed over by the Finder or `open` open at launch or while running. Modern TLS sites do not load (a limitation of the period). |
| Bejeweled 2 | Carbon | Works | Sound and music play. The title screen and game menus take clicks. |
| SimCity 4 Rush Hour | Carbon + OpenGL | Works | The region view works; tested windowed (Preferences > Play in a Window). The intro movies play with picture and sound. |
| Build-a-lot, Build-a-lot 2 | Carbon + OpenGL | Works | Display mode switching works. The Build-a-lot 2 board runs at 40 frames/s (`OAH_FPS=1`). Build-a-lot 2's tutorial panels and arrows clear as they should, and Mojave's menu bar stays clear of its goals bar. The loading screens and logo screens show. |
| Blender 2.63 (PPC) | Cocoa/GHOST + OpenGL + OpenAL | Works | `tests/blender/run.sh` uses it as a modelling and rendering program. It checks primitives, modifiers, edit-mode extrude, materials, keyframe animation and particles. It renders with ray-traced Blender Internal and checks the render pixel by pixel. It also checks save and reopen, and the OpenGL 3-D viewport with the window open. All 28 checks pass, and the numbers and render match 10.6 Rosetta. |
| blenderplayer 2.63 (PPC, Blender Game Engine) | Cocoa/GHOST + OpenGL + Bullet + Python | Works | `tests/blender/bge_run.sh` runs a physics scene at 60 Hz in a 640x480 window. It checks gravity, collision, lit GLSL and multitexture materials, and the game's own screenshot. All 10 checks pass. |
| Transmission 2.22 | Cocoa | Works | The first-run notice and Preferences (port check) work. Create Torrent File works: the save panel shows its sidebar, and the .torrent has the right piece hash. |
| Hex Fiend, TextWrangler 3, The Unarchiver, xACT | Cocoa | Works | TextWrangler: new documents, editing and typing work. Documents open from the Finder or `open`. File > Print shows the print sheet, and PDF > Save as PDF writes the PDF. The "Save changes?" sheet shows its text and buttons. TextWrangler starts normally after a force-quit with unsaved changes, but the unsaved document is not recovered. |
| G4 Quasar, Reactor, Supernova, Event Horizon, Fractal Explorer | OpenGL | Works | |
| Adobe Creative Suite 2 Premium (installer) | Carbon + pkg | Works | The full suite installs from the 4 discs: serial, license, and all component packages with status 0. Its packages' PowerPC scripts run through the installer shim. The progress bar and status text update, but the billboard pane above them is not redrawn during the install. |
| Illustrator CS2, InDesign CS2, GoLive CS2 | Carbon | Works | Run them through a launcher: Mojave refuses Illustrator and InDesign as apps that need Java 6. Copy reaches Mojave's clipboard from a type object, a text frame or a GoLive page. GoLive opens documents double-clicked in the Finder or dropped on it, at launch and while running. Illustrator: new documents, drawing, text and printing work, the Help Center opens, and the toolbox picks tools by click with a document open. Photoshop CS2 prints too. InDesign: fonts, swatches, Export to PDF, Place, printing, Save and Open work. |
| ImageReady CS2 | Carbon | Works | The tool bar and palettes work. Painting shows the brush outline cursor. Optimized, 2-Up and 4-Up previews work. |
| Adobe Bridge (CS2) | Carbon | Does not start | Stops with *FLEXnet Activation toolkit runtime error 27* (Adobe's licensing runtime) and ends. Open issue. |
| Acrobat Distiller 7 | Carbon | Stops at personalization | Says "An invalid password or user name was entered" and quits: it needs Acrobat 7's personalization (see Acrobat). |
| MSN Messenger 4.0 (Office 2004) | Carbon | Works (no service) | The Setup Assistant works: its pages, Save As of the license as RTF, and Print and Preview. Signing in is impossible because the .NET Messenger service has closed. Quitting leaves no second Daemon running. |
| ExtendScript Toolkit (CS2) | Carbon | Partly | The editor, console and data browser work. Running a script does not finish (it stays "running"). Open issue. |
| Demonstrator (Illustrator CS2) | Carbon | Starts | Asks for *Enable access for assistive devices*, an accessibility permission the user grants. |
| DropStuff / DropZip / DropTar (StuffIt 7) | Carbon CFM | Works | A file handed over at launch is compressed to .sit / .zip / .tgz, after *Not Yet* on the registration reminder. |
| Photoshop CS2 | Carbon | Works | Needs a launcher (Rosettus Launcher / mkwrapper.sh): Mojave refuses to open this version on its own or patched in place, as an app that needs Java 6. The New and Open dialogs work. Painting shows the brush outline cursor. Type layers work. Filters and adjustments show a live preview (Gaussian Blur, Levels). Marquee, zoom, scroll wheel and Save As PSD work. Help > Updates starts Adobe Updater, which finds nothing because Adobe's update servers are gone. |
| Acrobat 7 Professional (CS2) | Carbon | Stops at personalization | Its first start runs Adobe's self-healing, which asks for an administrator password. It rejects the CS2 serial numbers in *Personalize Your Software*. Open issue. |
| Version Cue CS2 | daemon + preference pane | Server and pane run (experimental) | The Viewer finds the installed pane automatically; Other… selects a pane by hand. With the optional PowerPC Java 5 and the Rosettus system service, the server controls and the web administration page work. Start-up and clean shutdown were tested; authenticated project workflows were not. See [VERSION_CUE](VERSION_CUE.md). |
| Logic Platinum 5.4 | Carbon CFM | Installs; needs the XSKey | The Installer VISE installer runs (as root) and installs. The app starts to its splash screen and stops at *Emagic XSKey not found*. It needs Emagic's USB copy-protection key, whose PowerPC driver cannot load on Mojave. Leave the two driver items unticked in the installer. |
| Kid Pix Deluxe 4 | Carbon CFM | Works | The StuffIt InstallerMaker installer works (mount the disc image's HFS partition if Mojave says the image has a problem). The app checks for a 16-bit (thousands of colors) display, which Mojave does not offer. Rosettus reports one to it (`OAH_GD_DEPTH=16`, set automatically for Kid Pix), so it runs full screen and drawing works. Animated stickers (Flash) show and play. With QuickTime 6's Flash added from the Kid Pix disc (`tools/install_qtflash.sh`, see the README), QuickTime plays them itself; otherwise Rosettus renders them. On a real Snow Leopard Mac they are empty boxes, because QuickTime 7.3.1 and later dropped Flash. Speech goes through Mojave's Speech Manager. Buttons react to the first click, and the Save panel saves on the first click. |
| Vue 5 Esprit | Mach-O PPC | Installer runs | The e-on installer copies itself to a temporary folder and starts the copy, which runs under Rosettus. It asks for a serial number of the form `VUE5ESP-xxxx-xxxx-xxxx`. |
| StuffIt Standard 7.0.3 | Carbon CFM | Works | The installer must run as root (it writes `/Library/Frameworks/StuffIt.framework`). StuffIt Expander extracts .sit archives, by drag-and-drop or `open -a`, including an archive opened with Expander at launch. |
| The Unarchiver 3.2 (PowerPC slice) | Cocoa | Works | It unpacked the .sit/.bin archives of this test set (Kid Pix, iUnit, StuffIt, Vue). |
| Mini vMac (PowerPC build) | Carbon | Works | Boots a System 7.5.3 disk image. The emulated Finder responds to mouse and keyboard. |
| iUnit | Classic CFM | Not supported | A Mac OS 9 application (InterfaceLib). Rosettus refuses it with a message. |
| PowerPC Java 1.3.1, 1.4.2 and 5.0 (from Apple's updates or a PowerPC Mac) | Java | Partly works | An optional add-on (see README, *PowerPC Java*). See the notes after this table. |
| Minecraft 1.8.9 (PowerPC port) | Java | Does not run | Starts under PowerPC Java 5 (log in, "Setting user"), then crashes on the JVM's first fault-handled memory access (the known Java issue below). |
| Festive Lights, Macworld '05 widget collection | Dashboard widgets | Works | The HTML widgets run in Mojave's Dashboard as they are. 10 of the collection's widgets ship PowerPC-only plug-ins, which `tools/install_widget.py` detaches. |
| Wireshark 1.9.2 | X11 (GTK) | Works (no live capture) | Opens and decodes capture files: packet list, tree, hex and display filters. tshark, capinfos, editcap and mergecap work. XQuartz starts by itself when needed. Live capture needs access to /dev/bpf*; without it no interface is listed, as on Snow Leopard. |
| Halo: Combat Evolved 2.0 (Universal, PowerPC slice) | Carbon | Stops at the CD check | The license agreement and alerts work. It then asks for the "Halo Universal" CD. The mounted disc image is not accepted, and the native Intel slice rejects it the same way, so it needs the physical CD. |
| ScreenFlow 1.5.4 (Universal, PowerPC slice) | Cocoa (garbage collected) | Partly | Menus and dialogs work. New Recording (File menu or Dock menu) starts ScreenFlowHelper under Rosettus and shows the recording window with the audio sources. Record does not record, for two reasons: the helper crashes in its audio-level timer, and the bundled screenflowrecorder captures the screen with a full-screen OpenGL context. That recorder is a universal tool that runs as Intel code, and macOS removed full-screen OpenGL contexts in 10.7. Recording computer audio needs its VaraAudio kernel extension. |
| Adobe Help Center (CS2) on its own | Carbon (Opera 7 inside) | Works | Starts and shows its window. The Opera "Certificate signer not found" sheet it shows for adobe.com is drawn. It works from Illustrator's Help menu too. |
| Evernote 2.2.3 (Universal, PowerPC slice) | Cocoa (garbage collected) | Works | The main window, new notes, typing and quit work. It needs `usr/share/langid` in the Snow Leopard export. Its EvernoteHelper, inside the app bundle, starts under Rosettus with the app. Spelling is checked as you type (red underlines, suggestions) by Snow Leopard's spell server, which Rosettus starts translated. |
| Help menus | | Works | AppleWorks opens Help Viewer; Word and Entourage open the Office Help viewer. The Help menu's Search field finds menu items (Cocoa: Evernote; Carbon: AppleWorks). Highlighting a result, with the arrow keys or the mouse, opens its menu (and submenu) with the blue arrow pointing at the item, and Return chooses it. |
| Mactracker (REALbasic; reports itself as 5.3) | Carbon | Works | Starts with its Welcome and Software Update windows; the update check reaches the network. Browses the categories and models and opens a model's window. Plays the Startup and Death Chimes through QuickTime, and quits. |
| PowerPC screen savers | ScreenSaver bundle | Works | Installed with `tools/install_saver.py`. They run full screen; the System Preferences preview shows a label. |
| Dashboard widgets | HTML/JS | Works | PowerPC-only widget plug-ins are detached (`tools/install_widget.py`). |
| Installer packages with PowerPC scripts | pkg | Prepared | `tools/ppc_pkg_install.sh` wraps PowerPC scripts. Installing needs your password in Installer. |

PowerPC Java was tested with:

* 1.3.1_16, 1.4.2_21 and 1.5.0_19, built from Apple's Java updates (`import/java`) and from Mac OS X
  10.4.11.
* 1.4.2_16 from a Leopard DVD.

What works:

* `java -version`, `javac`, and command-line programs with threads, garbage collection, collections
  and strings run at good speed with Java 1.4.2 and 5.0.
* Swing/AWT windows work with Java 5, 1.4.2 and 1.3.1 (Aqua look, buttons, text fields).

Known issues:

* A Java program that dereferences `null` (or otherwise needs the JVM's own fault handling) crashes
  the JVM instead of getting a `NullPointerException`.
* Java 1.3.1's garbage collector stops a 4-thread allocation test.
* Java 5 built for Leopard is experimental: 1.5.0_30 (from Java for Mac OS X 10.5 Update 10) and
  1.5.0_13 (from the Leopard DVD) crash in the garbage collector under load.

## Peripherals

Checked with the PowerPC test programs in `tests/periph/`. `periph.sh` runs each one under Rosettus,
and also runs a native build of the same source for comparison.

| Device / API | Status | Notes |
|---|---|---|
| Speakers: CoreAudio HAL, AudioUnit output, AudioQueue output | Works | |
| Speakers: Sound Manager (`SndPlay`, sound channels) | Works | |
| Microphone: AudioQueue input, AUHAL input, SPB devices | Works | The system asks once for microphone permission. |
| QuickTime movies (SVQ1, H.264, ...) | Works | |
| Serial ports (USB serial adapters) | Works | IOKit lookup, open, termios |
| MIDI (CoreMIDI: devices, the IAC bus, network MIDI, USB MIDI interfaces) | Works | Device change notifications are not delivered. |
| USB device list (IOKit registry) | Works | Lists the same devices as native code. |
| Keychain (passwords: generic and internet, items, searches) | Works | Through macOS' own keychain. Rosettus' helper `rosettus-kc` reads and writes item data, so an item made by another app asks for permission. |
| Network configuration (System Configuration store: computer name, interfaces, IP state) | Works | Change notifications are not delivered. |
| FSEvents (folder watching) | Works | |
| Battery and power adapter (IOPowerSources) | Works | Change notifications are not delivered. |
| Bonjour, CFNetwork HTTP, preferences | Works | |
| HID devices through IOHIDManager / IOHIDDevice (10.5 API): game pads, joysticks, keyboards | Works | Answered by macOS' own HID calls: device list, elements, values, queues, LEDs, callbacks. Checked with the built-in keyboard and trackpad, not with a game pad. |
| HID devices through the old IOHIDLib plug-in (`IOHIDDeviceInterface`, HID Utilities, games written for 10.4 and earlier) | Works | Served by the same native calls. Checked with the keyboard: values, queues, LED, output transactions. |
| USB device interfaces (IOUSBLib), other device user clients | Not supported | The macOS 10.14 kernel refuses device user clients to a PowerPC process (`IOServiceOpen` fails). |
| Camera (Sequence Grabber: preview, grab, record) | Works | Through a video digitizer that Rosettus serves with macOS' own camera capture: the built-in FaceTime HD camera, or another camera macOS sees. The system asks once for camera permission. USB webcams are reached through this digitizer, not through 10.6's USB digitizer, which needs `IOServiceOpen`. |

Known gaps:

* Classic (Mac OS 9) apps are not supported, as on Snow Leopard. `rosettus` says so for a CFM app that
  links InterfaceLib instead of CarbonLib; `OAH_ALLOW_CLASSIC=1` tries anyway.
* Kernel extensions and input methods in PowerPC code cannot load.
* AltiVec code runs, but slower than on a G4/G5.

## Test results for Rosettus for Mojave 1.1

The full app test was run again on the 1.1 library (shared install, apps opened through their
launchers in `/Applications/Rosettus`), with the same steps as for 1.0 below. Every app passed the
same steps as with 1.0: AppleWorks 6, TextWrangler, Hex Fiend, Office 2004 (Word 10/10, Excel 10/10,
PowerPoint 10/10, Entourage 9/9, Project Gallery Launcher 5/5), Keynote, Pages and Numbers (11/11),
iWeb, Internet Explorer, Mactracker, The Unarchiver, Transmission, xACT, Fractal Explorer, Kid Pix
Deluxe 4, Equation Editor (9/9), Photoshop, GoLive (12/12), Illustrator and InDesign CS2 (11/11),
blender (26/26), the G4 demos, the Office helpers, the Drop utilities and SimCity 4 (`sc4test.py`
4/4). Acrobat 7 Professional stops at its first-run dialog as with 1.0 (2/4).

Also checked for 1.1: clicking a menu title twice leaves no highlighted title behind and moving the
mouse along the bar afterwards highlights nothing; the menu bar titles of PowerPC apps have no shadow
in Light Mode.

## Test results for Rosettus 1.0

Run on the installed 1.0 package (shared install in `/Library/Rosettus`, apps opened through their
launchers). Steps: launch, front, new document, type + copy, paste, preferences, hide / show, quit,
crash reports, plus a chooser, reopen or app-specific step for some apps.

| App | Steps passed |
|---|---|
| AppleWorks 6 | 9/9 |
| TextWrangler | 9/9 |
| Hex Fiend | 9/9 |
| Microsoft Word | 10/10 |
| Microsoft Excel | 10/10 |
| Microsoft PowerPoint | 10/10 |
| Microsoft Entourage | 9/9 |
| Project Gallery Launcher | 5/5 |
| Equation Editor | 9/9 |
| Keynote | 11/11 |
| Pages | 11/11 |
| Numbers | 11/11 |
| iWeb | 9/9 |
| Internet Explorer | 9/9 |
| Mactracker | 9/9 |
| The Unarchiver | 9/9 |
| StuffIt Expander | 9/9 |
| Transmission | 9/9 |
| xACT | 9/9 |
| Fractal Explorer | 9/9 |
| Kid Pix Deluxe 4 | 9/9 |
| Bejeweled 2 | 9/9 |
| Adobe Photoshop CS2 | 12/12 |
| Adobe Illustrator CS2 | 11/11 |
| Adobe InDesign CS2 | 11/11 |
| Adobe GoLive CS2 | 12/12 |

AppleWorks 6 passed 7/9 when it ran first in the batch (its new document did not open in time, so the
type and quit steps had nothing to work on), then 9/9 on its own.

Other checks:

* SimCity 4 Rush Hour (`tests/sc4test.py`): 4/4. The game window appears, the game runs for 60 s, and
  it quits.
* Blender (`tests/blender/run.sh`): 28 checks, 0 failed.
* blenderplayer GLSL scene (`tests/blender/bge_run.sh`): 10 checks, 0 failed.
* Peripherals (`tests/periph/periph.sh`): no failures.
* QuickTime movie playback (`qt_play`): pass.
* Keynote: a document opened while it runs shows its window in 21 s.
* PowerPC Java (`tests/java/javatest.sh`): `java -version`, `javac`, the thread and allocation test and
  a Swing window pass with 1.4.2_21 and 1.5.0_19. With 1.3.1_16, `java -version`, `javac` and the Swing
  window pass; its garbage collector fails the allocation test (known issue).
* No crash or hang reports.

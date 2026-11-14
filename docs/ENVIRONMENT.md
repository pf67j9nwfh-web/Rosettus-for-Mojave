# Environment variables

Set them for one run in Terminal (`OAH_GLTRACE=1 ~/Library/Rosettus/rosettus /Applications/X.app`).
For a patched app, add `export NAME=value` lines to `X.app/Contents/Resources/rosetta.env`; for
a launcher app, to `~/Applications/Rosettus/X.app/Contents/Resources/rosetta.env`. There are
about 150 switches in the source (`grep -o 'getenv("OAH_[A-Z_]*")' src/liboah/*`); these are
the useful ones.

## General

| Variable | Effect |
|---|---|
| `OAH_ROOT=dir` | install folder (default: the folder of liboah_mojave.dylib) |
| `OAH_NO_GTB=1` | run the bridged main frameworks instead of the guest-toolbox overlay (gtb mode is the default) |
| `OAH_HOST_SYSVERSION=1` | report Mojave's version to apps instead of 10.6.8 |
| `OAH_NO_APPRECENT=1` | the app's own recent-documents list (File > Open Recent in Cocoa apps) is unavailable, as before 1.2: Open Recent is empty after a relaunch |
| `OAH_NO_UPTIMEFIX=1` | on a Mac up for 12 days or more, `AbsoluteToDuration` of an `UpTime()` reading is not counted from the app's start; it then answers kDurationForever after 24.8 days of uptime (Build-a-lot 2 stops at its splash screen). `OAH_UPTIMEFIX=1` applies the fix at any uptime (testing), `OAH_UPTIMEFIX_LOG=1` logs when it is on |
| `VC_STOP_TIMEOUT=N` | Version Cue service: seconds to wait for a server shutdown before ending it and its database (default 90, minimum 10) |
| `OAH_QUIET_EXIT=1` | do not log non-zero exit codes |
| `OAH_NO_REPORTS=1` | no crash/hang reports in ~/Library/Logs/Rosettus/Reports |
| `OAH_NO_TELEMETRY=1` | no unbridged-call tables in ~/Library/Logs/Rosettus/Telemetry |
| `OAH_NO_IDLEEXIT=1`, `OAH_IDLE_EXIT=s` | helper agents: never exit when idle / idle time before exiting (default 30 s) |
| `OAH_EV_IDLE_MS=ms` | event polling interval while the app is idle (default 50; active: 8) |
| `OAH_EV_RECV=1` | receive window-server events on the native event port instead of polling (experimental) |
| `OAH_NO_BGCHECK=1` | treat every app as a foreground app (normally background-only helpers, from their `SIZE` resource or `LSBackgroundOnly`/`LSUIElement`, are kept out of the Dock and never take focus) |
| `OAH_NO_GURLFILE=1` | pass `GURL file:///...` events on unchanged (normally turned into the `odoc` that 10.6 sent for files) |
| `OAH_AE_NEVERINTERACT=1` | deliver incoming Apple Events with kAENeverInteract instead of kAECanInteract |
| `OAH_AE_NOREPLY_IF_UNWANTED=1` | deliver events whose sender wants no reply with kAENoReply (default: always wait for the handler) |
| `OAH_GD_DEPTH=16` | report a 16-bit (thousands of colors) main screen to the app; windows keep 32-bit pixels. Set automatically for Kid Pix Deluxe 4, which refuses to start otherwise |
| `OAH_NO_BMRK=1` | pass Mojave's bookmark-data documents ('bmrk') on unchanged instead of resolving them to file URLs |

## Tracing

| Variable | Logs |
|---|---|
| `OAH_TRACE=1`, `OAH_VERBOSE=1` | general start-up and memory mapping |
| `OAH_SCTRACE=1` | guest system calls |
| `OAH_PATHLOG=1` | guest paths redirected into the guest tree |
| `OAH_CGSTRACE=1`, `OAH_EVTRACE=1` | window server calls, events |
| `OAH_GLTRACE=1`, `OAH_GLERR=1` | OpenGL calls, GL errors |
| `OAH_GLITRACE=glBegin,CGLFlushDrawable,...` or `all` | every guest call of the named GL/CGL/AGL functions with the current native context (40000 lines); `OAH_GLITRACE_DELAY=<s>` starts after s seconds, `-1` traces only while `/tmp/oah_glitrace.on` exists |
| `OAH_STEXTRACE=1`, `OAH_STEX_DUMP=1` | surface textures (aglSurfaceTexture): bindings and refreshes; dump each texture to `/tmp/stex_<n>.ppm` every 100 refreshes |
| `OAH_AUTRACE=1`, `OAH_SNDTRACE=1` | CoreAudio, Sound Manager |
| `OAH_LSTRACE=1`, `OAH_AETRACE=1` | Launch Services, Apple Events |
| `OAH_AUTHTRACE=1` | Authorization Services |
| `OAH_HITLOG=1` | window hit tests, mouse button and key events handed to the guest, and the guest backtrace that pulled each click |
| `OAH_GTRACE=Lib:_sym,...` | entry/exit of any guest function, with arguments (`OAH_GTRACE_STR=1` prints C strings, `OAH_GTRACE_MAX=n` limits). `abs:0xADDR` traces a function by address; `OAH_GTRACE_BT=1` adds the guest backtrace and the words at r4; `OAH_GTRACE_BTNAMES=1` names the backtrace frames (image+offset); `OAH_GTRACE_XMLNODE=n` prints argument rN as a libxml2 node with its attributes; `OAH_GTRACE_MEM3=1` dumps 32 words at the first argument on return (parameter blocks); `OAH_GTRACE_EVERY=n` keeps logging every nth call past the limit |
| `OAH_COPYWIN_PROBE=1` | each scroll copy (`CGContextCopyWindowContentsToRect`) with its rectangles |
| `OAH_EIPDUMP=before,after` | on a crash: the native code bytes around the faulting instruction and the translated block it belongs to |
| `OAH_RDUMP=bytes` | on a guest trap or crash: the guest register file past the GPRs (XER, CR fields, FPRs) |
| `OAH_BLITLOG=1` | rows of each window sent to the screen |
| `OAH_FRONTFIX_LOG=1` | each time the front-process fix answers a failed `GetFrontProcess` |
| `OAH_BPROF=1` | time spent in each bridged call (printed every 30 s and at exit) |
| `OAH_MEMREPORT=s` | after s seconds: resident/dirty memory by mapped file and the largest anonymous regions |

## Switching individual fixes off (for bisecting a problem)

`OAH_NO_PATHMAP`, `OAH_NO_KERNCLASSIC` (Rosetta shims, native libm), `OAH_NO_SCRX` (Script
Manager Gestalt), `OAH_NO_GUESTFIX` (built-in guest code fixes), `OAH_NO_TRFATAL` (translate
fatal() logging), `OAH_NO_AUBRIDGE` (CoreAudio output units), `OAH_NO_ACTLEVEL` (window levels
that follow app activation), `OAH_NO_HOD` (hide floating panels on deactivate),
`OAH_NO_MODESWITCH` (display mode changes), `OAH_NO_KBMAP` (keyboard layouts), `OAH_NO_QT`
(native QuickTime bridge). Each is `NAME=1`.

Installer-related fixes (each `NAME=1`): `OAH_NO_PKGSHIM` (run `/usr/sbin/installer` directly
instead of through `native/installer-shim`), `OAH_NO_ROOTAEWP` (when already root,
`AuthorizationExecuteWithPrivileges` still goes through authd instead of starting the tool in the
login session), `OAH_NO_FRONTFIX` (guest `GetFrontProcess` failures are not answered from the window
server), `OAH_NO_LAUNCHAPPFIX` (guest `LaunchApplication` of a PowerPC app is not sent to
Rosettus), `OAH_NO_LOCKFIX` (translator lock self-deadlock breaker), `OAH_NO_FASTEXIT` (exit
through the translator's own path), `OAH_NO_APFS_HPLUS` (`statfs` reports `apfs` instead of
`hfs`), `OAH_NO_VOLDEVFIX` (volume device type from the guest's own table), `OAH_NO_USERMEMFIX`
(`hw.usermem` not clamped to 2 GB). `OAH_ALLOW_CLASSIC=1` makes `rosettus` start a Mac OS 9 CFM app
anyway.

PowerPC Java (each `NAME=1`): `OAH_NO_JAVAFIX` (a loaded PowerPC JVM is told it runs translated, and
refuses to start), `OAH_NO_GEXC` (guest `thread_`/`task_set_exception_ports` go to the kernel, which
rejects the PowerPC flavors; the JVM then stops at start-up), `OAH_GEXC_LOG` (log the guest's
exception-port registrations), `OAH_NO_KCHRFIX` (`GetScriptManagerVariable(smKCHRCache)` answers NULL until HIToolbox has started),
`OAH_NO_ALIGNCB` (guest code that Rosettus calls back into, such as Apple Event handlers, runs on a stack
that is 4 bytes off a 16-byte boundary, and AltiVec stack stores then overwrite neighbouring data),
`OAH_NO_YIELDFIX` (a Thread Manager yield inside an Apple Event handler keeps Rosettus's guest-call lock, and
the cooperative thread it yields to waits for that lock forever: Internet Explorer hung opening a page),
`OAH_NO_GLBACKING` (OpenGL: double-buffered pixel formats are not given kCGLPFABackingStore, so a back buffer is undefined after a flush, as Mojave has it), `OAH_IGNORE_ABORT` (a guest abort() is logged and ignored instead of ending the process), `OAH_NO_LSUIMODE` (SetSystemUIMode: the app's LaunchServices presentation mode is left unset; Mojave's menu bar stays over full-screen games), `OAH_NO_BGINFO` (a background-only guest app checks in with LaunchServices as a foreground app first; its Dock tile bounces once), `OAH_NO_LAUNCHAE` (Apple Events: a Carbon app launched with documents gets 'oapp' and then the documents, instead of the first document as its launch event), `OAH_NO_RAPPWAIT` (Apple Events: a reopen that arrives while the app is still starting is delivered at once instead of when its first document-sized window is up), `OAH_GLI_NORESTORE` (OpenGL: a call through another context's dispatch table leaves that context current), `OAH_STEX_FRONTCOPY` (surface textures: aglSurfaceTexture always copies the source's front buffer), `OAH_STEX_ROWCOPY` / `OAH_STEX_SWAP` (surface textures, Build-a-lot 2: copy the source window row by row instead of one flipped blit / still swap a hidden source window's buffers), `OAH_NO_PPCSTATE` (Objective-C garbage collection: thread_get_state(PPC_THREAD_STATE) is not answered from the guest registers; GC apps such as ScreenFlow stop with "get_register_state(): unable to get thread state"), `OAH_NO_IOMATCH` (IOKit: IOServiceGetMatchingService(s) is not done natively; Mojave answers Snow Leopard's text matching request with kIOReturnUnsupported), `OAH_NO_SPEECH` (the Speech Manager: the guest's own SpeechSynthesis is used instead of Mojave's; it never finishes a channel, and StopSpeech waits forever), `OAH_NO_SWF` (QuickTime: NewMovieFromFile on a Flash .swf fails with -2048 as on Snow Leopard, instead of a movie rendered by Rosettus), `OAH_NO_RESDIRMAP` (paths: the Resources directories of system bundles and frameworks are listed from Mojave instead of the guest tree; the 10.6 print dialog then fails with "failed to load window nib file 'CocoaPrintWindow'"), `OAH_NO_TZLINK` (readlink("/etc/localtime") gives Mojave's /var/db/timezone target; 10.6 CFTimeZone then falls back to GMT), `OAH_NO_NATIVE_OGROUP` (window ordering groups are kept by Rosettus only and not also passed to Mojave's window server; for testing), `OAH_GUEST_TRAPHANDLER` (a guest SIGTRAP handler is installed: the translator then hands bridge traps of compiled code to it), `OAH_NO_SEGVKEEP` (the translator's SIGSEGV/SIGBUS handler, installed when the guest has its own, replaces Rosettus's fault fixer), `OAH_DIAG_NOSIGH` (diagnostic: guest handlers for SIGILL/SIGBUS/SIGSEGV/... are not installed), `OAH_EXCLOG` (log the Mach exceptions the translator receives, with the native state), `OAH_GTRACE_UCTX=n` / `OAH_GTRACE_STACKSCAN=n` / `OAH_GTRACE_STACKRAW` / `OAH_GTRACE_SCANUCTX=n` / `OAH_GTRACE_BTANY` (gtrace: rN as a ucontext, code addresses or raw words on the guest stack, back chains that jump), `OAH_GCLOG=2` (log every thread_get_state call), `OAH_GTRACE_CFDATA=n` (gtrace: rN as a guest CFData, its bytes as text), `OAH_GTRACE_PEEK=off,off,...` (gtrace: at return, the bytes at the first argument plus each offset - object fields), `OAH_FRONT_KCURRENT=1` (GetFrontProcess answers kCurrentProcess for our own process instead of its serial number), `OAH_NO_NESTED_HELPER` (LaunchServices: a universal helper app inside the running app's bundle, such as ScreenFlowHelper or EvernoteHelper, is handed to Mojave as an Intel app instead of starting under Rosettus), `OAH_NO_PPCSPELL` (Cocoa spell checking talks to Mojave's AppleSpell, which does not understand the 10.6 messages, instead of Snow Leopard's AppleSpell.service started translated), `OAH_NO_QTFLASH` (RosettusFlash.component, QuickTime 6's Flash added by tools/install_qtflash.sh, is not registered, and Rosettus's own SWF renderer is used instead), `OAH_SWF=1` (use Rosettus's own SWF renderer even when RosettusFlash.component is installed), `OAH_NO_EVSNDPUMP` (Sound Manager callbacks and APlayer track-done notices are not delivered at the guest's event poll, only at other bridge calls; an idle guest-toolbox app then never gets them, and Kid Pix buttons waited for their click sound until the next click), `OAH_GTRACE_ARG=n` (with `OAH_GTRACE_MEM3` / `OAH_GTRACE_PEEK`: look at argument rN instead of r3, e.g. 4 for WaitNextEvent's EventRecord), `OAH_NO_GCCLEARFIX` (Objective-C garbage collection: libauto clears and scans the stack even when the guest runs on the translator's callback stack; ScreenFlow then crashed after a Dock-menu command), `OAH_NO_THRINFO` (thread_info / thread_policy_set / thread_policy_get from PowerPC code go to Mojave's kernel, which rejects them; QuickTime's H.264 decoder then crashed), `OAH_NO_CAMERA` (no video digitizer for the Mac's camera; the Sequence Grabber finds no camera), `OAH_NO_MIDI` (CoreMIDI is left to the guest's own, which cannot talk to Mojave's MIDI server), `ROSETTUS_DUMP_TEXT` (import/dvd: the file dump_text_i386.dylib writes Snow Leopard translate's decrypted text to; not used at run time), `OAH_MEMSTATS=n` (every n seconds: the guest's malloc totals and the resident size in the log), `OAH_LAUNCHAE_APPKIT` (a launch document goes into the launch modifiers only for apps without AppKit loaded), `OAH_CLIP_XLATE_EARLY` (clipboard: read a Carbon app's system-translated flavors on the sync thread even while its promised flavors wait for the main thread; Word 2004 then crashes after 10-20 copies), `OAH_CLIP_KEEPER_SYNCTHREAD` (clipboard: a Carbon app's promised flavors are read on the sync thread, its promise keeper running there; Illustrator CS2 then crashes after a copy), `OAH_NO_HIDDENFRONT` (a window the guest makes the front window while the app is hidden is ordered in at once; Kid Pix's window then stays on screen after Cmd-H),
`OAH_NO_AWTDEBUGFIX` (Java 1.4.2's AWT may stop at its "Please Attach
Debugger" prompt and wait for a key).

File Manager, icon and drawing fixes (each `NAME=1`): `OAH_NO_VOLINDEXFIX` (indexed
`PBHGetVInfo`/`PBXGetVolInfo` calls are not answered from the mount list), `OAH_NO_VOLBLOCKFIX`
(volume block counts, file-system ID and boot volume blessed folder left as the guest computed
them), `OAH_NO_VOLICONFIX` (volumes have no media icon properties: disks draw as folders),
`OAH_NO_ICNSFIX` (Mojave .icns files are not converted for the 10.6 icon code; converted copies
are kept in `~/Library/Caches/com.rosettus.ppc/icns`), `OAH_NO_COPYWINFIX` (scrolling does not
copy window pixels), `OAH_NO_UNLOCKBLIT` (QuickDraw drawing is not sent to the screen shortly
after the window's bits are unlocked), `OAH_NO_FLUSHCHECK` (a flush with an empty dirty shape is
always skipped, even when the window was not blitted for a while: Carbon sheets stayed blank),
`OAH_NO_RESHAPEBLIT` (no full re-blit after a window is reshaped), `OAH_NO_DISABLEUPDATE`
(`CGSDisableUpdate` / `CGSReenableUpdate` are no-ops again), `OAH_NO_REVEALWAIT` (a window moved on screen from off it is moved at once, not when its
pixels are next sent), `OAH_ULB_QUIET=ms` / `OAH_ULB_MAX=ms` (QuickDraw drawing without a flush is sent once the
window's bits were left alone that long, default 30 / 200 ms; `OAH_ULB_OLD=1`: a fixed timing, for comparison). Diagnostics:
`OAH_FLUSHLOG=1` (a line per window flush), `OAH_BLITDUMP=<wid>` (every blit of that window as raw ARGB in
`/tmp/blitdump`; `=1`: all windows while `/tmp/blitdump/on` exists); `OAH_BLITDUMP_SIZE=WxH`, `OAH_BLITDUMP_MAX=n`).
`OAH_NO_SHEETEXPOSE=1`: a sheet that drew nothing within 300 ms of being shown gets no exposure event.
`OAH_NO_DND=1`: files dragged onto a PowerPC app's window are not accepted (no CoreDrag handlers on its
windows); `OAH_DNDLOG=1` logs the handlers, tracking and drops. `OAH_NO_CSSEEDFIX=1`: CarbonCore seeds
(`_CSCreateSeed`...) go to the missing 10.6 seed service again (every drag inside a PowerPC app fails with -900).
`OAH_NO_DISPCAPFIX=1`: no native screen capture for the Colors panel magnifier (`CGContextCopyDisplayCaptureContentsToRect`,
`CGSGetDisplaysWithRect/Point`, `CGSGetDisplayPixelEncodingOfLength`). `OAH_NO_IMKLAUNCHFIX=1`: 10.6 input-method launch
requests are sent again (each waits out a timeout: slow menus). `OAH_NO_CHARPALFIX=1`: Edit > Special Characters calls the
10.6 `LaunchAppleCharacterPalette` again.
`OAH_NO_MSGPORTFIX=1`: a guest thread holding the callback lock keeps it while it waits in `CFMessagePortSendRequest`
(the clipboard export can then deadlock with the main thread). `OAH_NO_VREFCACHE=1`: `FSGetVolumeInfo` looks volume
references up through guest calls every time. `OAH_NO_CSSEEDFIX=1`: HIServices' CarbonCore seeds go to the missing seed
service again (other callers always do).
`OAH_NO_NORMALARR=1`: `glNormalPointer` arrays go to Mojave's GL unconverted again (lit 3-D comes out black).
`OAH_NO_AUCLAIM=1`: the guest audio thread publishes the unit it renders only after choosing it (a stop in between does
not wait for that render). `OAH_NO_AUDEVPROPS=1`: AudioDevice properties asked of a guest output AudioUnit ('fsiz', 'ltnc'...) get an error
instead of the native device's answer (QuickTime then renders 0 frames at a time: no movie sound, a stalled clock).
`OAH_NO_AUGOIN=1`: 'goin' (DeviceIsRunning) is asked of the device rather than answered from the unit.
`OAH_NO_AURATESCALAR=1`: render timestamps carry sample and host time only. `OAH_AUTRACE2=1`: log calls on
non-output AudioUnits (with `OAH_AUTRACE`'s output-unit log).
`OAH_NO_GTSCANFAST=1`: gtrace looks every open fix row up in every image (header read per row). `OAH_NO_GSYMCACHE=1`:
every guest symbol lookup scans the whole address space (no remembered image per library). Both only slow launches down.
`OAH_CVL_CALLPPC=1`: the CVDisplayLink thread calls its output callback through the guest-call lock again (Keynote's
Theme Chooser can then deadlock at launch).
`OAH_MB_NOREDRAW=1`: back from a hidden UI mode (a slide show), the menu bar is republished without being redrawn (Mojave may
then show a bare "App  Help" bar). Touch `/tmp/oah_mbpub` to force a redraw and republish (debug). `OAH_NO_TEXFAST=1`:
texture uploads keep GL_UNPACK_SWAP_BYTES on (the driver's slow pixel path).
`OAH_NO_RLRELEASE=1`: a thread keeps the guest-call lock while it runs a guest run loop (a document opened from Finder
could then hang Keynote at "Progress..."). PowerPC Java (the JavaVM.framework tools and JavaApplicationStub apps)
always keeps it (the JVM stopped at start with the release); `OAH_RLRELEASE_JAVA=1` releases it there too.
`OAH_TGS_SLOWSCAN=1|0`: `thread_get_state` looks for a thread's guest registers with the slower word-by-word stack scan
(default in PowerPC Java, whose garbage collector crashed with the fast read) / never.
`OAH_EAGERTEXT_NOW=1`: a lazy pasteboard owner is asked for its text on the way back from `declareTypes:owner:` even
inside a menu action (Pages' and Keynote's copies can then reach Mojave empty).
`OAH_RAPPWAIT_HIDDEN=1`: a reopen of a hidden app is held like one during start-up (until a document window shows or
40 s after launch).
`OAH_NO_CBSTACK=1`: on a callback stack, `pthread_get_stackaddr_np` / `pthread_get_stacksize_np` describe the thread's
own stack (AppleScript then reported "Stack overflow"). `OAH_CBSTACKTRACE=1` logs the corrected answers.
`OAH_ODOC_FURL=1`: AppleScript Studio apps and apps using Adobe's scripting layer (SCL: GoLive CS2) also get a document as a file URL instead of a list of aliases.
`OAH_PBOARD_STALL=s` (default 20): a call to the translated pasteboard server (CFPasteboardCreate / Synchronize) that has waited this long restarts the server (`launchctl kickstart -k gui/UID/com.rosettus.ppc.pboard`, at most every 2 minutes). `OAH_NO_PBWATCH=1` disables.
`OAH_ODOC_ALIAS=2`: every Cocoa app gets documents as a list of aliases (experimental).
`OAH_NO_MAINSTACKCB=1`: guest calls made on the main thread run on the translator's callback stack, not just below the
guest's r1 on its own stack (REALbasic apps then raise StackOverflowException in callbacks).
`OAH_AE_QUEUED=1` (experimental): native Apple Events run when the app dispatches a private Carbon event, not inside
HIToolbox's event fetch (Carbon apps; `=2` Cocoa apps too). `OAH_RAPP_NODISPATCH=1` (diagnostic): a reopen brings the
app forward without the event.
`OAH_BPROF=1` with `touch /tmp/oah_bprof`: per-bridge (and per-hook: the guest fixes) call counts and native time,
dumped on demand.

## Clipboard, menu bar, Retina

| Variable | Effect |
|---|---|
| `OAH_NO_CLIPSYNC=1` | no clipboard sync between PowerPC apps and Mojave |
| `OAH_CLIPTRACE=1` | log each clipboard import / export and the flavors seen |
| `OAH_CLIP_OFFMAIN=1` | read and write the guest clipboard on the sync thread instead of the app's main thread |
| `OAH_CLIP_PROMISED=1` | also export flavors the copying app only promised (normally left out: reading them runs the app's own provider inside the sync, and Hex Fiend's put up a modal window) |
| `OAH_NO_STATUSBAR=1` | status items (menu bar apps) as before 1.2: not shown (the status bar calls are stubs again) |
| `OAH_STATUSBAR_LOG=1` | log the status items' registration and placement |
| `OAH_NO_SBDARK=1` | a status item's title is not made light in Dark Mode |
| `OAH_NO_PREVACT=1` | `CGSSetPreventsActivation` stays a no-op |
| `OAH_NO_TRANSFORMFIX=1` | the guest's `TransformProcessType` is left to its own Process Manager (which answers -600) |
| `OAH_NO_SBWATCH=1` | our status items are not moved when other apps' items come or go (the gap a quitting app leaves stays) |
| `OAH_NO_TRANSFORMMENUBAR=1` | after Show in Dock (TransformProcessType to foreground) the app's menu bar is not put up: the previous app's menus stay |
| `OAH_NO_TRANSFORMSTATE=1` | HIToolbox's cached process kind is not updated by TransformProcessType |
| `OAH_NO_DRAGREGION=1` | textured windows are not moved by a drag in their drag regions (CGSAddDragRegion is a no-op again) |
| `OAH_NO_PBUFFER=1` | CGL pbuffers are not emulated (CGLCreatePBuffer fails, as natively) |
| `OAH_NO_NEWWINRETRY=1` | a window the window server refuses (backing type 0) is not created again as buffered |
| `OAH_NO_MSFALLBACK=1` | a pixel format that asks for multisampling fails instead of being chosen without it |
| `OAH_NO_STEX_GLFLUSH=1` | surface textures are refreshed only at a buffer swap, not at glFlush / glFinish of a front-buffer context |
| `OAH_NO_STEX_RESOLVE=1` | a multisampled surface is copied into its surface texture without resolving it first |
| `OAH_NO_STEX_MEMCOPY=1` | a surface texture in another share group is copied on the GPU (draws into the wrong texture) |
| `OAH_NO_WCTXIMAGE=1` | CGWindowContextCreateImage of a GL window is not read back from OpenGL |
| `OAH_PBLOG=1` | logs the pbuffer emulation |
| `OAH_EV_BG_IDLE_MS=n` | event polling interval in ms while the app is in the background and quiet (default 200; 0 = the foreground interval) |
| `OAH_SND_IDLE_SECS=n` | seconds of silence before the Sound Manager output stops until the next sound (default 10; 0 = never) |
| `OAH_BUTTON_PULSE=1` | default buttons pulse as on Snow Leopard (Carbon and Cocoa) |
| `OAH_MB_PIXELS=1` | menu bar: publish the guest's own 1x title pixels instead of drawing the titles natively at Retina resolution |
| `OAH_HIDPI=1` | experimental, off by default: give PowerPC windows a 2x backing store on Retina displays, so the guest renders text and Quartz drawing at 2x (QuickDraw stays 1x, pixel-doubled). The native window context stays 1x on Mojave, so each window also gets a CGS surface at resolution 2 drawn by a CGL context (backing size = the store's pixels): the 2x pixels reach the screen. Also switched on by Rosettus Launcher's File > Sharp Text on Retina Displays (com.rosettus Retina) |
| `OAH_NO_CUIFRAME1X=1` | with `OAH_CUISCALE`: Carbon window frames are not drawn through a 1x bitmap (their title bar background is then missing at 2x) |
| `OAH_HIDPI_CGONLY=1` | with `OAH_HIDPI=1`: no OpenGL surface; the 2x store is only blitted into the 1x window context (downsampled) |
| `OAH_HDPIXDUMP=row` | diagnostic: log the ARGB of every 100th pixel of that row of a Retina window's store at each blit |
| `OAH_CUISCALE=1` | with `OAH_HIDPI=1`: have CoreUI draw standard controls, title bars and toolbars from its 1x artwork through the 2x transform. Without it the 10.6 theme finds no 2x artwork and leaves them out. Renders complete 2x windows in TextWrangler and Hex Fiend; Keynote stopped in CoreFoundation with it, so it is a separate switch |
| `OAH_CUIDUMP=n` | log the first n CoreUI draw calls of a Retina window (context, scale, widget) |
| `OAH_RLAUNCH_MULTI=1` | let a launcher or patched app start a second copy of a PowerPC app that is already running (normally it hands its documents to the running copy and brings that to the front) |
| `OAH_NO_REOPENSHOW=1` | do not bring the app to the front when it gets a reopen event (Finder / Spotlight / `open` on the running app) |
| `OAH_EXITTRACE=1` | log every guest exit, also with status 0 (with `OAH_VERBOSE=1`: the native caller) |
| `OAH_SCAN_ANON=1` | also scan anonymous memory page by page when looking for PowerPC images (the behaviour before 1.2: it made the translator's code blocks and every malloc region resident, 300-500 MB per app) |
| `OAH_MEMREPORT=s`, `OAH_DUMPBLOCKS=1` | s seconds after start, log memory by mapping (resident / dirty); with `OAH_DUMPBLOCKS` also write the translator's 26 MB pool blocks to /tmp for analysis |
| `OAH_NO_SHMEMFIX=1` | Office 2004: keep stale ShMem semaphores and shared memory left by a crashed Office app (normally removed when no other Office app runs) |
| `OAH_NO_SPAWNFIX=1` | pass a PowerPC app's `posix_spawn` to the kernel unchanged (it fails with EINVAL on Mojave: NSTask helpers exit with status 5) |
| `OAH_SPAWNTRACE=1` | log every `posix_spawn` from the PowerPC app: path, result, pid |
| `OAH_SPAWN_NONULLFIX=1` | turn off the work-around for `posix_spawn` with no pid pointer (the translator wrote the pid to address 0) |
| `OAH_ALLOW_CRASHCATCHER=1` | start Bare Bones' crash-catcher helper (TextWrangler); it takes over the exception ports the translator needs |
| `OAH_NO_SIGTRAPKEEP=1` | let the app reset SIGTRAP (normally kept: the translator runs guest traps as SIGTRAP) |
| `OAH_NO_SHELLFIX=1` | run `/bin/sh` and shell scripts started by a PowerPC app with the system shell instead of Rosettus' copy, which sends PowerPC tools back to Rosettus |
| `OAH_KEEP_DYLD_ENV=1` | keep the app's `DYLD_*` variables when it starts a native program |
| `OAH_NO_X11MAP=1` | do not map `/usr/X11` and `/usr/X11R6` to the PowerPC X11 libraries in the install folder |
| `OAH_TOOL_STDERR=1` | PowerPC command-line tools keep Rosettus' messages on stderr instead of `~/Library/Logs/Rosettus/tools.log` |
| `OAH_ODOC_ALIAS=1`, `OAH_LAUNCHDOC_NOW=1` | experimental: give Carbon apps documents as a list of aliases (as 10.6's Finder did) / at the first event instead of after the app settles |
| `OAH_NO_GUESTFIX=1` | turn off the built-in guest function fixes (icon cache unload, Retina CTM, Quick Look assertion) |

`OAH_GCLOG=1` logs up to 200 answered thread_get_state(PPC) calls (which register file, from where) instead of 5.
`OAH_SPEECHTRACE=1` logs the speech calls answered by Mojave's Speech Manager. `OAH_SWFTRACE=1` logs Flash files opened
as movies; `OAH_SWF_SCALE=n` renders them at n x (default 2, for stickers scaled up); `OAH_SWF_OPAQUE=1` keeps the
Flash background color (not antialiased) instead of a transparent background.
`OAH_GTRACE_CFSTR=n` (with `OAH_GTRACE`): print argument rN of each traced call as text when it is a guest CFString.
`OAH_THRLOG=1` logs the answered thread_info / thread_policy calls. `OAH_CAMLOG=1` logs the camera digitizer's calls and
the capture session; `OAH_CAMERA_ASK=1` opens the camera even when the app's own Info.plist lacks NSCameraUsageDescription
(for an app bundle that has it and starts `rosettus`; without it macOS 10.14 ends the process). `OAH_MIDILOG=1` logs the
CoreMIDI calls answered natively. `OAH_MAXCPU=n` reports at most n processors to the PowerPC program (hw.ncpu,
hw.activecpu, ...; off by default).
`OAH_NO_EAGERTEXT=1`: a Cocoa app that promises text on the clipboard (`declareTypes:owner:`) is not asked for it at
once (without it, Hex Fiend hung on Copy). `OAH_NO_KEYCHAIN=1` leaves the keychain to the PowerPC Security
framework (which cannot reach Mojave's securityd); `OAH_KCLOG=1` logs the keychain calls; `OAH_KC_NOHELPER=1` reads and
writes item data in the process instead of through `native/rosettus-kc` (securityd then refuses it). `OAH_NO_SCSTORE=1` /
`OAH_SCLOG=1`: the same for the System Configuration store, `OAH_NO_FSEVENTS=1` / `OAH_FSELOG=1` for FSEvents streams,
`OAH_NO_POWERSOURCES=1` / `OAH_PSLOG=1` for the power sources.
`OAH_TOOL_FOREGROUND=1`: a PowerPC program that is no application (no .app bundle, CFM application or
'APPL' type: Office 2004's FontCacheTool) is made a foreground app when it connects to the window
server, as before (by default it checks in as background-only, as on 10.6).
`OAH_NO_FPKEEP=1`: f14-f31 are not put back after `CFHostStartInfoResolution` (the translator
loses f30/f31 during the lookup); `OAH_FPKEEP_LOG=1` logs each time they had changed.
`OAH_NO_HID=1` leaves HID devices (IOHIDManager, IOHIDDevice, ...) to the PowerPC IOKit, which cannot open them;
`OAH_HIDLOG=1` logs the HID calls. `OAH_NO_HIDCOM=1` leaves only the old IOHIDLib plug-in interface to the guest.

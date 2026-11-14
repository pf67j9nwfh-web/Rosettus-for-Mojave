#!/usr/bin/python
# behave.py "<wrapper name>" [--keep]: the standard behaviour pass for one Rosetta app.
# Launch -> new document -> type -> select all / copy (checks the native pasteboard) -> paste ->
# preferences -> hide / show -> quit (Don't Save if asked).  Every step records: process alive,
# window count, front app, new crash/hang reports; a screenshot per step.
# Output: ~/Library/Logs/Rosettus/Behave/<app>-<stamp>/report.txt (+ png), one line in summary.tsv.
import sys, os, time, subprocess, re
import Quartz as Q
from AppKit import NSPasteboard, NSStringPboardType

HOME = os.path.expanduser('~')
APP = sys.argv[1]
KEEP = '--keep' in sys.argv
WRAP = os.path.join(os.environ.get("BEHAVE_WRAPDIR") or os.path.join(HOME, "Applications/Rosettus"), APP + ".app")
PROC = WRAP + '/Contents/MacOS/translate'
if not os.path.isdir(WRAP) and os.path.isdir('/Applications/Rosettus/' + APP + '.app'):   # a shared install's wrappers
    WRAP = '/Applications/Rosettus/' + APP + '.app'; PROC = WRAP + '/Contents/MacOS/translate'
if not os.path.isdir(WRAP):   # an app patched in place: open the app itself; its translator is .rosetta-translate
    for d in ('/Applications', os.path.join(HOME, 'Applications')):
        for r, ds, fs in os.walk(d):
            if r.count(os.sep) - d.count(os.sep) > 2: del ds[:]; continue
            if APP + '.app' in ds: WRAP = os.path.join(r, APP + '.app'); break
        if os.path.isdir(WRAP): break
    PROC = WRAP + '/Contents/MacOS/.rosetta-translate'
    if os.path.exists(WRAP + '/Contents/MacOS/translate'): PROC = WRAP + '/Contents/MacOS/translate'   # a wrapper in /Applications/Rosettus
if os.path.exists(WRAP + '/Contents/MacOS/launcher.sh'): PROC = WRAP + '/Contents/MacOS/translate'   # (a new wrapper links its translate at the first launch)
DIRECT = os.path.isdir(WRAP) and not os.path.exists(PROC) and not os.path.exists(WRAP + '/Contents/MacOS/launcher.sh')
if DIRECT:   # neither wrapped nor patched (Photoshop CS2): run it with the rosettus command, as Terminal users do
    PROC = WRAP + '/Contents/MacOS/'
def open_app():
    if DIRECT:
        subprocess.Popen(['/Library/Rosettus/rosettus', WRAP], stdout=open(os.devnull, 'w'), stderr=subprocess.STDOUT, preexec_fn=os.setsid)
        return
    subprocess.call(['open', WRAP])
def show_app(pid):   # the Finder / Spotlight path for a running app (a reopen event); activate when it is not an app LS can open
    if not DIRECT: subprocess.call(['open', WRAP]); return
    # (NSRunningApplication's activate does nothing from this ssh / background python - not even
    # for the Finder; Launch Services knows the process but lsappinfo shows no bundle id for it)
    from AppKit import NSRunningApplication
    ra = NSRunningApplication.runningApplicationWithProcessIdentifier_(pid)
    if ra and ra.bundleIdentifier():
        subprocess.call(['osascript', '-e', 'tell application id "%s" to activate' % ra.bundleIdentifier()])
STAMP = time.strftime('%Y%m%d-%H%M%S')
OUT = os.path.join(HOME, 'Library/Logs/Rosettus/Behave', APP.replace(' ', '_') + '-' + STAMP)
os.makedirs(OUT)
REPIDX = os.path.join(HOME, 'Library/Logs/Rosettus/Reports/index.txt')
rep = open(os.path.join(OUT, 'report.txt'), 'w')
results = []

# Blender: the generic steps type text into the 3-D view (every letter is a Blender shortcut) and
# click the splash screen's web links. Use Blender for what it is for instead: tests/blender/run.sh
# (modelling, render, save / reopen, the OpenGL viewport), and record its checks as the steps.
if APP.lower() == 'blender':
    here = os.path.dirname(os.path.abspath(__file__))
    guest = subprocess.Popen(['/usr/libexec/PlistBuddy', '-c', 'Print :RosettaGuestApp', WRAP + '/Contents/Info.plist'],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE).communicate()[0].strip()
    out = subprocess.Popen(['bash', os.path.join(here, 'blender', 'run.sh')] + ([guest] if guest else []),
                           stdout=subprocess.PIPE).communicate()[0]
    for l in out.splitlines():
        m = re.match(r'RESULT (\S+) (PASS|FAIL)(.*)', l)
        if m and m.group(1) not in ('blender_func', 'blender_gui'):
            results.append((m.group(1), m.group(2) == 'PASS')); rep.write(l + '\n')
    if not results: results.append(('blender_test', False))
    passed = sum(1 for n, ok in results if ok)
    print 'RESULT %s: %d/%d steps ok -> %s' % (APP, passed, len(results), OUT)
    rep.close()
    s = open(os.path.join(HOME, 'Library/Logs/Rosettus/Behave/summary.tsv'), 'a')
    s.write('%s\t%s\t%d/%d\t%s\t%s\n' % (STAMP, APP, passed, len(results), ' '.join(n for n, ok in results if not ok) or '-', OUT))
    s.close()
    sys.exit(0)

def log(s):
    rep.write(s + '\n'); rep.flush()
    print s
    sys.stdout.flush()

def nreports():
    try: return len(open(REPIDX).readlines())
    except IOError: return 0

def find_pid():
    out = subprocess.Popen(['pgrep', '-f', re.escape(PROC)], stdout=subprocess.PIPE).communicate()[0].split()
    return int(out[0]) if out else 0

def windows(pid, onscreen_only=True):
    opt = Q.kCGWindowListOptionOnScreenOnly if onscreen_only else Q.kCGWindowListOptionAll
    wl = Q.CGWindowListCopyWindowInfo(opt, Q.kCGNullWindowID) or []
    r = []
    for w in wl:
        if int(w.get('kCGWindowOwnerPID', 0)) != pid: continue
        b = w.get('kCGWindowBounds', {})
        if b.get('Width', 0) < 40 or b.get('Height', 0) < 30: continue
        if onscreen_only and float(w.get('kCGWindowAlpha', 1)) == 0: continue   # invisible (ghosted offscreen GL target)
        r.append((int(w.get('kCGWindowLayer', 0)), w.get('kCGWindowName', '') or '', int(b['Width']), int(b['Height'])))
    return r

def front_pid():
    out = subprocess.Popen(['lsappinfo', 'info', '-only', 'pid', subprocess.Popen(['lsappinfo', 'front'], stdout=subprocess.PIPE).communicate()[0].strip()], stdout=subprocess.PIPE).communicate()[0]
    m = re.search(r'=(\d+)', out)
    return int(m.group(1)) if m else 0

def cputime(pid):
    out = subprocess.Popen(['ps', '-o', 'time=', '-p', str(pid)], stdout=subprocess.PIPE).communicate()[0].strip()
    m = re.match(r'(?:(\d+):)?(\d+):(\d+(?:\.\d+)?)', out)
    if not m: return 0.0
    return int(m.group(1) or 0) * 3600 + int(m.group(2)) * 60 + float(m.group(3))

def settle(pid, limit=20.0):
    # wait until the app has worked through its input: < 0.1 s of CPU per 0.5 s, twice in a row
    t0 = time.time(); c0 = cputime(pid); quiet = 0
    while time.time() - t0 < limit:
        time.sleep(0.5); c1 = cputime(pid)
        quiet = quiet + 1 if c1 - c0 < 0.1 else 0
        c0 = c1
        if quiet >= 2: break
    return time.time() - t0

def new_alerts(pid, before):
    # untitled windows that were not there before (an app alert such as Excel's paste-shape message)
    names = [w[1] for w in before]
    return [w for w in windows(pid) if w[1] == '' and w not in before and len(windows(pid)) > len(before)]

def click(x, y):
    if TARGET[0] and front_pid() != TARGET[0]:
        raise NotFront('front pid %d, not %d' % (front_pid(), TARGET[0]))
    for t in (Q.kCGEventLeftMouseDown, Q.kCGEventLeftMouseUp):
        e = Q.CGEventCreateMouseEvent(None, t, (x, y), 0); Q.CGEventPost(Q.kCGHIDEventTap, e); time.sleep(0.08)
    time.sleep(0.2)

def win_bounds(pid, title):
    for w in Q.CGWindowListCopyWindowInfo(Q.kCGWindowListOptionOnScreenOnly, Q.kCGNullWindowID) or []:
        if int(w.get('kCGWindowOwnerPID', 0)) == pid and (w.get('kCGWindowName', '') or '') == title:
            b = w['kCGWindowBounds']; return int(b['X']), int(b['Y']), int(b['Width']), int(b['Height'])
    return None

# After New, what it takes to reach a place that takes typed text, where New alone does not give one.
#   iWeb: New asks for a page template (Return: Choose); a page has no text box until one is
#   inserted (the Text Box toolbar button, 324 px from the window's left, 50 px above its bottom).
# Before New: what stands in its way. Illustrator CS2's Welcome Screen is modal (Esc closes it).
BEFORE_NEW = {
    'Adobe Illustrator': [('wait', 2), ('key', 53), ('wait', 2)],
}
AFTER_NEW = {
    'iWeb': [('key', 36), ('wait', 8), ('click', 'iWeb', 324, -50), ('wait', 4)],
    #   Transmission: New is Create Torrent File (a file chooser: Esc); its text field is the
    #   transfer filter (View > Filter Bar, Cmd-Opt-F)
    #   (a toggle: pressed again if it hid a bar that was already showing)
    #   Mactracker: its updater's "A new version is available" window comes up ~15 s after launch
    #   (Esc: Remind Me Later); the text field is the model search (Cmd-F)
    'Mactracker': [('esc_window', 'Software Update', 25), ('cmdkey', 3), ('wait', 1)],
    #   Hex Fiend: typing goes into the hex column (only hex digits; Copy gives hex); Tab moves to the text column
    'Hex Fiend': [('key', 48), ('wait', 1)],
    #   Internet Explorer: New opens a window on the home page; the text field is the address bar (Cmd-L)
    'Internet Explorer': [('cmdkey', 37), ('wait', 2)],
    'Transmission': [('key', 53), ('wait', 2), ('toggle_taller', 'Transmission', 3), ('wait', 2)],
    #   Photoshop CS2: New is a dialog (Return: OK); letters on a canvas are tool shortcuts, so the
    #   text goes into a type layer: T (Type tool), a click in the document, then the typing
    'Adobe Photoshop CS2': [('key', 36), ('wait', 5), ('key', 17), ('wait', 1), ('click_doc',), ('wait', 2)],
    #   Illustrator CS2 the same (New Document dialog, T, a click on the artboard: point type); InDesign
    #   CS2 needs a text frame (T, a drag); GoLive CS2's New is a chooser dialog (Return: HTML page)
    #   whose layout view takes the typing
    'Adobe Illustrator': [('key', 36), ('wait', 5), ('key', 17), ('wait', 1), ('click_doc',), ('wait', 2)],
    'Adobe InDesign CS2': [('key', 36), ('wait', 5), ('key', 17), ('wait', 1), ('drag_doc',), ('wait', 2)],
    'Adobe GoLive CS2': [('key', 36), ('wait', 6)],
}

TARGET = [0]
class NotFront(Exception): pass
def key(code, cmd=False, shift=False, opt=False, ctrl=False):
    # never type into another app: a hidden test app once left AppleWorks in front and got its Cmd-Q
    if TARGET[0] and front_pid() != TARGET[0]:
        raise NotFront('front pid %d, not %d' % (front_pid(), TARGET[0]))
    fl = (Q.kCGEventFlagMaskCommand if cmd else 0) | (Q.kCGEventFlagMaskShift if shift else 0) | (Q.kCGEventFlagMaskAlternate if opt else 0) | (Q.kCGEventFlagMaskControl if ctrl else 0)
    if cmd:
        e = Q.CGEventCreateKeyboardEvent(None, 55, True); Q.CGEventSetFlags(e, fl); Q.CGEventPost(Q.kCGHIDEventTap, e); time.sleep(0.05)
    for d in (True, False):
        e = Q.CGEventCreateKeyboardEvent(None, code, d); Q.CGEventSetFlags(e, fl); Q.CGEventPost(Q.kCGHIDEventTap, e); time.sleep(0.04)
    if cmd:
        time.sleep(0.08); e = Q.CGEventCreateKeyboardEvent(None, 55, False); Q.CGEventSetFlags(e, 0); Q.CGEventPost(Q.kCGHIDEventTap, e)
    time.sleep(0.15)

KC = dict(a=0, s=1, d=2, f=3, h=4, g=5, z=6, x=7, c=8, v=9, b=11, q=12, w=13, e=14, r=15, y=16, t=17,
          o=31, u=32, i=34, p=35, l=37, j=38, k=40, n=45, m=46)
KC.update({'/': 44, '-': 27})
KC.update({'1': 18, '2': 19, '3': 20, '4': 21, '6': 22, '5': 23, '9': 25, '7': 26, '8': 28, '0': 29, ' ': 49})
def type_text(s):
    for ch in s.lower():
        if ch in KC: key(KC[ch])
    time.sleep(0.3)

def shot(name):
    p = os.path.join(OUT, name + '.png')
    subprocess.call(['screencapture', '-x', p])
    subprocess.call(['sips', '-Z', '1100', p], stdout=open(os.devnull, 'w'))

# screen pixels and mouse, for games / paint programs that draw their own controls
def grab(x, y, w, h):
    # (bytes per row, BGRA bytes) of a screen rectangle in points
    img = Q.CGWindowListCreateImage(Q.CGRectMake(x, y, w, h), Q.kCGWindowListOptionOnScreenOnly, 0, 0)
    return Q.CGImageGetBytesPerRow(img), Q.CGImageGetWidth(img), Q.CGImageGetHeight(img), str(Q.CGDataProviderCopyData(Q.CGImageGetDataProvider(img)))
def pixels_total(g, step=3): return len(range(0, g[2], step)) * len(range(0, g[1], step))
def pixels_where(g, pred, step=3):
    bpr, w, h, d = g; n = 0
    for yy in range(0, h, step):
        for xx in range(0, w, step):
            o = yy * bpr + xx * 4
            if pred(ord(d[o + 2]), ord(d[o + 1]), ord(d[o])): n += 1
    return n
def pixels_changed(g1, g2, step=3):
    bpr, w, h, a = g1; b = g2[3]; n = 0
    for yy in range(0, h, step):
        for xx in range(0, w, step):
            o = yy * bpr + xx * 4
            if abs(ord(a[o]) - ord(b[o])) + abs(ord(a[o + 1]) - ord(b[o + 1])) + abs(ord(a[o + 2]) - ord(b[o + 2])) > 60: n += 1
    return n
def mouse(t, p): Q.CGEventPost(Q.kCGHIDEventTap, Q.CGEventCreateMouseEvent(None, t, p, 0))
def mclick(x, y, hold=0.3):
    # games poll the button state (Kid Pix: Button() in a loop): move there first and hold it a moment
    tf = time.time()
    while TARGET[0] and front_pid() != TARGET[0] and time.time() - tf < 3: time.sleep(0.25)   # (Kid Pix's quit question: front for a moment elsewhere)
    if TARGET[0] and front_pid() != TARGET[0]: raise NotFront('front pid %d, not %d' % (front_pid(), TARGET[0]))
    mouse(Q.kCGEventMouseMoved, (x, y)); time.sleep(0.3)
    mouse(Q.kCGEventLeftMouseDown, (x, y)); time.sleep(hold); mouse(Q.kCGEventLeftMouseUp, (x, y)); time.sleep(0.2)
def mdrag(x0, y0, x1, y1, n=30):
    if TARGET[0] and front_pid() != TARGET[0]: raise NotFront('front pid %d, not %d' % (front_pid(), TARGET[0]))
    mouse(Q.kCGEventMouseMoved, (x0, y0)); time.sleep(0.3)
    mouse(Q.kCGEventLeftMouseDown, (x0, y0)); time.sleep(0.3)
    for i in range(1, n + 1):
        mouse(Q.kCGEventLeftMouseDragged, (x0 + (x1 - x0) * i / float(n), y0 + (y1 - y0) * i / float(n))); time.sleep(0.02)
    mouse(Q.kCGEventLeftMouseUp, (x1, y1)); time.sleep(0.2)
def screen_size():
    b = Q.CGDisplayBounds(Q.CGMainDisplayID()); return int(b.size.width), int(b.size.height)

# Save As into /tmp/rosettus-save, close, reopen: (extension, keys that end text editing first, bytes the file starts with)
SAVE = {
    'Adobe Photoshop CS2': ('psd', [(76,)], '8BPS'),          # keypad Enter commits the type layer
    'Adobe Illustrator': ('ai', [(53,)], '%PDF'),            # Esc leaves the type object (kept)
    'Adobe InDesign CS2': ('indd', [(53,)], '\x06\x06\xed\xf5'),
    'Adobe GoLive CS2': ('html', [], '<'),
}
# Steps that do not apply
NO_TEXT = {'Database Utility': 'no text field (a list of the Office databases)',
           'Microsoft Office Notifications': 'a reminder window with no text field',
           'Build-a-lot 2': 'a game: its title screen has no text field and nothing to copy',
           'SimCity 4 Rush Hour': 'a game: its region view has no text field and nothing to copy'}
IMAGE_COPY = ('Equation Editor',)
QUIT_RETURN = ('Equation Editor',)   # its MathType tip at quit (default No)
# Hide by the application menu: (x of its title, y of its Hide item) on this 1440x900 screen
HIDE_MENU = {'Adobe Illustrator': (90, 146)}
REOPEN = {'open': 0, 'activate': 0}
def ensure_front(pid):
    # first the Finder / Spotlight path (open the running app: a reopen event), then an activate
    # by bundle id (the Dock / Cmd-Tab path); how often each was needed goes in the report
    for i in range(4):
        if front_pid() == pid:
            if i == 1: REOPEN['open'] += 1
            elif i > 1: REOPEN['activate'] += 1
            return True
        if i == 0 or DIRECT: show_app(pid)
        else:
            out = subprocess.Popen(['lsappinfo', 'info', '-only', 'bundleid', '-app', 'pid=%d' % pid], stdout=subprocess.PIPE).communicate()[0]
            m = re.search(r'="([^"]+)"', out)
            if m: subprocess.call(['osascript', '-e', 'tell application id "%s" to activate' % m.group(1)])
        time.sleep(1.5)
    return front_pid() == pid

def step(name, ok, detail=''):
    results.append((name, ok))
    log('%-12s %-4s %s' % (name, 'ok' if ok else 'FAIL', detail))

# StuffIt's droplets (DropStuff, DropZip, DropTar): no window, no documents; a file opened with one is
# compressed next to it and the droplet quits. Unregistered, each asks for a serial first: Esc (Not Yet).
# The archive must hold the file: zip / tar read here, a .sit expanded by StuffIt Expander.
if APP in ('DropStuff', 'DropZip', 'DropTar'):
    import shutil, zipfile, tarfile
    r0 = nreports()
    d = '/tmp/rosettus-drop'; shutil.rmtree(d, True); os.makedirs(d)
    body = 'rosettus %s %d\n' % (APP, int(time.time()) % 100000)
    f = os.path.join(d, 'hello.txt'); open(f, 'w').write(body)
    t0 = time.time(); subprocess.call(['open', '-a', WRAP, f])
    pid = 0; nag = 0; arc = None
    while time.time() - t0 < 90:
        pid = pid or find_pid()
        arc = [n for n in os.listdir(d) if n.startswith('hello.txt.')]
        if arc: break
        if pid and [w for w in windows(pid) if w[0] in (0, 8) and w[1] == '' and w[2] >= 400 and w[3] >= 300]:
            if front_pid() == pid: key(53); nag += 1; time.sleep(2)
        time.sleep(0.5)
    shot('1-done')
    step('launch', bool(pid), 'pid %d, serial question answered Not Yet %d time(s)' % (pid, nag))
    ok = False; detail = 'no archive after %.0f s' % (time.time() - t0)
    if arc:
        a = os.path.join(d, arc[0]); time.sleep(2)
        try:
            if a.endswith('.zip'):
                z = zipfile.ZipFile(a); ok = z.read('hello.txt') == body; detail = '%s %s' % (arc[0], z.namelist())
            elif a.endswith(('.tar', '.tgz', '.gz')):
                t = tarfile.open(a); m = t.getmember('hello.txt'); ok = t.extractfile(m).read() == body; detail = '%s %s' % (arc[0], t.getnames())
            elif a.endswith('.sit'):
                x = os.path.join(d, 'x'); os.makedirs(x); shutil.copy(a, x)
                se = '/Applications/StuffIt Standard 7.0.3/StuffIt Expander.app'
                subprocess.call(['open', '-a', se, os.path.join(x, arc[0])])
                te = time.time(); out = os.path.join(x, 'hello.txt')
                while time.time() - te < 60 and not (os.path.exists(out) and open(out).read() == body): time.sleep(0.5)
                ok = os.path.exists(out) and open(out).read() == body
                detail = '%s (%d bytes, header %r), StuffIt Expander gave back %s' % (arc[0], os.path.getsize(a), open(a).read(8), 'the file' if ok else sorted(os.listdir(x)))
                subprocess.call(['pkill', '-f', 'StuffIt Expander.app/Contents/MacOS'])
        except Exception as ex:
            detail = '%s: %s' % (arc[0], ex)
    step('archive', ok, detail)
    t1 = time.time()
    while pid and os.system('kill -0 %d 2>/dev/null' % pid) == 0 and time.time() - t1 < 15: time.sleep(0.5)
    gone = not pid or os.system('kill -0 %d 2>/dev/null' % pid) != 0
    step('quit', gone, 'the droplet %s' % ('quit by itself' if gone else 'was still running'))
    if not gone: os.system('kill -9 %d' % pid)
    nr = nreports() - r0
    step('reports', nr == 0, '%d new crash/hang reports' % nr)
    passed = sum(1 for n, ok in results if ok)
    log('RESULT %s: %d/%d steps ok -> %s' % (APP, passed, len(results), OUT))
    s = open(os.path.join(HOME, 'Library/Logs/Rosettus/Behave/summary.tsv'), 'a')
    s.write('%s\t%s\t%d/%d\t%s\t%s\n' % (STAMP, APP, passed, len(results), ' '.join(n for n, ok in results if not ok) or '-', OUT))
    s.close()
    sys.exit(0)

# Office helpers meant to run inside an Office app (Graph, Organization Chart, Clip Gallery), Microsoft
# Query (Excel's) and the Database Daemon (background): started by themselves they say so (or show
# no window) and must quit cleanly; the daemon must keep running. Their real use is tested from Word.
HOSTED = ('Microsoft Graph', 'Organization Chart', 'Microsoft Clip Gallery', 'Microsoft Query', 'Microsoft Database Daemon')
if APP in HOSTED:
    r0 = nreports(); t0 = time.time(); open_app()
    pid = 0; al = []
    while time.time() - t0 < 30:
        pid = pid or find_pid()
        if pid:
            al = [w for w in windows(pid) if w[0] in (0, 8) and 300 <= w[2] <= 600 and 100 <= w[3] <= 230]
            if al or (time.time() - t0 > 15): break
        time.sleep(0.5)
    time.sleep(2); shot('1-launch')
    step('launch', bool(pid), 'pid %d, %.1fs, %s' % (pid, time.time() - t0, 'its message %r' % (al[0],) if al else 'windows %s' % windows(pid)[:3]))
    def alive(): return pid and os.system('kill -0 %d 2>/dev/null' % pid) == 0
    if APP == 'Microsoft Database Daemon':
        time.sleep(8); step('running', alive(), 'a background process, still running after 10 s')
        os.system('kill %d' % pid); time.sleep(3)
        step('quit', not alive(), 'ended on SIGTERM')
    else:
        TARGET[0] = pid
        try:
            if ensure_front(pid):
                if al: key(36); time.sleep(4)            # OK / Yes: quit
                if alive(): key(12, cmd=True); time.sleep(4)
        except NotFront: pass
        t1 = time.time()
        while alive() and time.time() - t1 < 10: time.sleep(0.5)
        step('quit', not alive(), 'quit %s' % ('after its message' if al else 'with Cmd-Q'))
    if alive(): os.system('kill -9 %d' % pid)
    nr = nreports() - r0
    step('reports', nr == 0, '%d new crash/hang reports' % nr)
    passed = sum(1 for n, ok in results if ok)
    log('RESULT %s: %d/%d steps ok -> %s' % (APP, passed, len(results), OUT))
    s = open(os.path.join(HOME, 'Library/Logs/Rosettus/Behave/summary.tsv'), 'a')
    s.write('%s\t%s\t%d/%d\t%s\t%s\n' % (STAMP, APP, passed, len(results), ' '.join(n for n, ok in results if not ok) or '-', OUT))
    s.close()
    sys.exit(0)

# Project Gallery Launcher: the Office 2004 Project Gallery; its default (a Word document) starts Word
# with a new document, and the launcher quits. Word is quit afterwards (Don't Save).
if APP == 'Project Gallery Launcher':
    r0 = nreports(); t0 = time.time(); open_app()
    pid = 0; pg = []
    while time.time() - t0 < 40:
        pid = pid or find_pid()
        pg = [w for w in windows(pid) if 'Project Gallery' in w[1]] if pid else []
        if pg: break
        time.sleep(0.5)
    time.sleep(2); shot('1-launch')
    step('launch', bool(pg), 'pid %d, %.1fs, %s' % (pid, time.time() - t0, pg[:1]))
    def wpid():
        o = subprocess.Popen(['pgrep', '-f', 'translate.*Microsoft Word'], stdout=subprocess.PIPE).communicate()[0].split()
        return int(o[0]) if o else 0
    w_before = wpid()
    if pg:
        TARGET[0] = pid
        try:
            ensure_front(pid); key(36)
        except NotFront: pass
    t1 = time.time(); wp = 0; wd = []
    while time.time() - t1 < 60:
        wp = wpid()
        wd = [w for w in windows(wp) if w[0] == 0 and w[2] >= 300 and w[3] >= 200] if wp else []
        if wd: break
        time.sleep(1)
    t2 = time.time()   # past the splash (462x262: Word ignores Cmd-Q while it shows) to a document window
    while wp and time.time() - t2 < 40 and not [w for w in windows(wp) if w[0] == 0 and (w[2], w[3]) != (462, 262) and w[2] >= 300]:
        time.sleep(1)
    if wp: settle(wp, 15)
    time.sleep(3); shot('2-word')
    step('opens Word', bool(wd) and not w_before, 'Word pid %d, %.0f s, windows %s' % (wp, time.time() - t1, wd[:2]))
    if wp and not w_before:
        TARGET[0] = wp
        try:
            ensure_front(wp); time.sleep(1)    # (Word started by Project Gallery: its bundle id is the wrapper's)
            key(53); time.sleep(2)             # Word opens on its own Project Gallery (modal): Cancel
            key(12, cmd=True); time.sleep(3)
            for i in range(3):
                if os.system('kill -0 %d 2>/dev/null' % wp) != 0: break
                key(2, cmd=True); time.sleep(3)
        except NotFront: pass
        t2 = time.time()
        while os.system('kill -0 %d 2>/dev/null' % wp) == 0 and time.time() - t2 < 15: time.sleep(0.5)
        wgone = os.system('kill -0 %d 2>/dev/null' % wp) != 0
        step('Word quit', wgone, '')
        if not wgone: os.system('kill -9 %d' % wp)
    lgone = not pid or os.system('kill -0 %d 2>/dev/null' % pid) != 0
    if not lgone: time.sleep(5); lgone = os.system('kill -0 %d 2>/dev/null' % pid) != 0
    step('quit', lgone, 'the launcher %s' % ('quit' if lgone else 'was still running'))
    if not lgone: os.system('kill -9 %d' % pid)
    nr = nreports() - r0
    step('reports', nr == 0, '%d new crash/hang reports' % nr)
    passed = sum(1 for n, ok in results if ok)
    log('RESULT %s: %d/%d steps ok -> %s' % (APP, passed, len(results), OUT))
    s = open(os.path.join(HOME, 'Library/Logs/Rosettus/Behave/summary.tsv'), 'a')
    s.write('%s\t%s\t%d/%d\t%s\t%s\n' % (STAMP, APP, passed, len(results), ' '.join(n for n, ok in results if not ok) or '-', OUT))
    s.close()
    sys.exit(0)

# ---- launch ----
r0 = nreports()
t0 = time.time()
open_app()
pid = 0; ws = []
while time.time() - t0 < 60:
    pid = pid or find_pid()
    if pid:
        ws = windows(pid)
        # a real window, not only a progress panel (iWeb shows "Opening..." for ~25 s first); any
        # window at all after 45 s (some apps open with a small one)
        # (AppleWorks' Starting Points is a floating palette, 631x189, and counts)
        if [w for w in ws if w[0] < 20 and w[2] >= 300 and w[3] >= 150] or (ws and time.time() - t0 > 45): break
    time.sleep(1)
step('launch', bool(pid and ws), 'pid %d, %.1fs, windows %s' % (pid, time.time() - t0, ws[:4]))
time.sleep(3)
shot('1-launch')
if not pid:
    log('no process; giving up')
else:
  TARGET[0] = pid
  try:
    def alive(): return os.system('kill -0 %d 2>/dev/null' % pid) == 0
    step('front', ensure_front(pid), 'front pid %d' % front_pid())
    # an alert at start-up (Entourage: "recovered the interrupted messages" after an earlier run was
    # killed) blocks everything after it: answer its default button, and say so in the report
    def answer_alerts(when, limit=10):
        st = settle(pid, limit)
        if st > 5: log('   (%s: the app was busy for %.1f s)' % (when, st))
        for i in range(3):
            al = [w for w in windows(pid) if (w[1] == '' or w[1].endswith(' Tip')) and w[0] in (0, 8) and 300 <= w[2] <= 600 and 110 <= w[3] <= 230]   # (Equation Editor's MathType tip)
            if not al: break
            shot('alert-%s-%d' % (when, i)); key(36); time.sleep(2)
            log('   (%s alert %s answered with its default button)' % (when, al[0]))
    answer_alerts('start-up', 30)   # a cold first launch (AppleWorks at the start of a sweep) is busy for 20 s and more
    def accept_chooser(tag):
        tw = []; tc = time.time()
        while time.time() - tc < (10 if tag == 'chooser2' else 3):   # a chooser can take seconds to come up
            tw = [w for w in windows(pid) if re.search('Chooser|Welcome|Gallery|Assistant|Project Browser', w[1])]
            if tw: break
            time.sleep(0.5)
        if tw:
            # Numbers' chooser opens on a sample (Personal Budget) where typing reaches no cell: pick
            # the first template (Blank), whose table takes the typing
            if APP in ('Numbers',):
                for c in (123,) * 4 + (126,) * 4: key(c)
                time.sleep(0.5)
            key(36); time.sleep(4)
            step(tag, alive(), 'accepted %r -> windows %s' % (tw[0][1], windows(pid)[:3]))
    # ---- a chooser / welcome window first (iWork Template Chooser): accept its default ----
    accept_chooser('chooser')
    # ---- new document ----
    def run_actions(acts):
      for act in acts:
          if act[0] == 'key': key(act[1])
          elif act[0] == 'cmdkey': key(act[1], cmd=True)
          elif act[0] == 'esc_window':
              while time.time() - t0 < act[2] and not win_bounds(pid, act[1]): time.sleep(0.5)
              if win_bounds(pid, act[1]):
                  key(53); time.sleep(1.5)
                  log('   (%r window closed with Esc: %s)' % (act[1], 'gone' if not win_bounds(pid, act[1]) else 'STILL THERE'))
          elif act[0] == 'toggle_taller':
              h0 = (win_bounds(pid, act[1]) or (0, 0, 0, 0))[3]
              key(act[2], cmd=True, opt=True); time.sleep(1.5)
              if (win_bounds(pid, act[1]) or (0, 0, 0, 0))[3] < h0: key(act[2], cmd=True, opt=True); time.sleep(1.5)
          elif act[0] == 'wait': time.sleep(act[1]); settle(pid, 15)
          elif act[0] == 'click_doc':   # the middle of the app's largest ordinary window
              td = time.time()   # the new document's window can take seconds (Photoshop: 10 s busy)
              while time.time() - td < 15 and not [w for w in windows(pid) if w[0] == 0]: time.sleep(0.5)
              ws = sorted([w for w in windows(pid) if w[0] == 0], key=lambda w: -w[2] * w[3])
              b = win_bounds(pid, ws[0][1]) if ws else None
              if b: mclick(b[0] + b[2] / 2, b[1] + b[3] / 2, 0.15)
              log('   (clicked in %r)' % (ws[0][1] if ws else None,))
          elif act[0] == 'drag_doc':   # a drag across the middle of the largest ordinary window (a text frame)
              td = time.time()   # the new document's window can take seconds (Photoshop: 10 s busy)
              while time.time() - td < 15 and not [w for w in windows(pid) if w[0] == 0]: time.sleep(0.5)
              ws = sorted([w for w in windows(pid) if w[0] == 0], key=lambda w: -w[2] * w[3])
              b = win_bounds(pid, ws[0][1]) if ws else None
              if b: mdrag(b[0] + b[2] * 0.3, b[1] + b[3] * 0.35, b[0] + b[2] * 0.7, b[1] + b[3] * 0.6)
              log('   (dragged in %r)' % (ws[0][1] if ws else None,))
          elif act[0] == 'click':
              b = win_bounds(pid, act[1])
              if b: click(b[0] + act[2], b[1] + (act[3] if act[3] >= 0 else b[3] + act[3]))
              else: log('   (no window %r to click in)' % act[1])
    if APP in BEFORE_NEW: run_actions(BEFORE_NEW[APP]); log('   (before New: %s)' % ', '.join(str(a[:2]) for a in BEFORE_NEW[APP]))
    def nbig():   # the window count once it holds still for a second (iWork animates the chosen template
        last = None; t5 = time.time()   # in 140x108 and 310x240 windows that come and go)
        while time.time() - t5 < 8:
            cur = windows(pid)
            if cur == last: break
            last = cur; time.sleep(1)
        return len(last or [])
    n0 = nbig(); key(45, cmd=True); time.sleep(3)
    accept_chooser('chooser2')
    # Entourage with no mail account: "You will not be able to send this message until ..." on each
    # new message (it took the first two typed letters)
    answer_alerts('new-doc')
    n1 = nbig()   # before the steps below (Mactracker's closes its updater window)
    run_actions(AFTER_NEW.get(APP, []))
    if APP in AFTER_NEW: log('   (after New: %s)' % ', '.join(str(a[:2]) for a in AFTER_NEW[APP]))
    step('new-doc', alive() and n1 >= n0, 'windows %d -> %d' % (n0, n1))
    shot('2-new')
    # ---- typing, select all, copy ----
    # Apps with no text to type into get a check of what they are for instead of type+copy:
    #   Fractal Explorer (a GLUT viewer driven by keys: "+" raises the iteration count in its title),
    #   The Unarchiver (no windows but its preferences: an archive opened with it is extracted).
    if APP == 'Fractal Explorer':
        ensure_front(pid)
        def title(): return ' '.join(w[1] for w in windows(pid) if w[1])
        t_a = title()
        for i in range(2):
            key(24, shift=True); time.sleep(1.5)   # '+'
        settle(pid, 10); t_b = title()
        ia = re.search(r'iter (\d+)', t_a); ib = re.search(r'iter (\d+)', t_b)
        step('keys', alive() and ia and ib and int(ib.group(1)) > int(ia.group(1)), '%r -> %r' % (t_a, t_b))
        shot('3-keys')
    elif APP == 'xACT':
        # xACT (AppleScript Studio, runs PowerPC/i386 command-line encoders from shell scripts): a WAV
        # opened with it is listed on its encode tab; Encode (default button) asks for a folder;
        # shorten output must decode back to the same samples (lossless)
        import wave, struct, math, shutil
        d = '/tmp/rosettus-xact'; shutil.rmtree(d, True); os.makedirs(d)
        wv = wave.open(os.path.join(d, 'tone.wav'), 'wb'); wv.setnchannels(2); wv.setsampwidth(2); wv.setframerate(44100)
        wv.writeframes(''.join(struct.pack('<hh', int(8000 * math.sin(i * 0.0627)), int(6000 * math.sin(i * 0.031))) for i in range(44100)))
        wv.close()
        subprocess.call(['open', '-a', WRAP, os.path.join(d, 'tone.wav')]); time.sleep(6); settle(pid, 15)
        ensure_front(pid); key(36); time.sleep(3)                      # Encode
        if win_bounds(pid, 'Choose a Folder'):
            key(5, cmd=True, shift=True); time.sleep(1.5); type_text(d); time.sleep(1); key(36); time.sleep(2); key(36)
        out = os.path.join(d, 'tone.shn'); te = time.time()
        while time.time() - te < 40 and not os.path.exists(out): time.sleep(0.5)
        time.sleep(2)
        ok = False; detail = 'no %s after %.0f s: %s' % (out, time.time() - te, sorted(os.listdir(d)))
        if os.path.exists(out):
            bin_ = os.path.join(subprocess.Popen(['/usr/libexec/PlistBuddy', '-c', 'Print :RosettaGuestApp', WRAP + '/Contents/Info.plist'], stdout=subprocess.PIPE).communicate()[0].strip(), 'Contents/Resources/Binaries/bin/shorten')
            back = os.path.join(d, 'back.wav')
            subprocess.call([bin_, '-x', out, back])
            a = wave.open(os.path.join(d, 'tone.wav')).readframes(44100)
            b = wave.open(back).readframes(44100) if os.path.exists(back) else ''
            ok = a == b and len(a) > 0
            detail = '%s %d bytes, decodes to %s samples' % (out, os.path.getsize(out), 'the same' if ok else 'DIFFERENT')
        step('encode', alive() and ok, detail)
        shot('3-encoded')
        for w in windows(pid):   # its "output" log window
            if w[1] == 'output': key(13, cmd=True); time.sleep(1)
        ensure_front(pid)
    elif APP == 'Kid Pix Deluxe 4':
        # Kid Pix (CFM, a full-screen window, controls of its own): splash, then "Enter your name"
        # (GO), then the paint canvas; a drag with the default pencil must draw on it
        wb = [w for w in windows(pid) if w[2] >= 600] or [(0, '', 0, 0)]
        b = win_bounds(pid, wb[0][1]) or (0, 0) + screen_size()
        cx, cy = b[0] + b[2] / 2, b[1] + b[3] / 2
        def canvas(): return grab(cx - 150, cy - 160, 400, 260)
        def white(g): return pixels_where(g, lambda r, gg, bb: r > 235 and gg > 235 and bb > 235)
        tk = time.time(); total = pixels_total(canvas())
        while time.time() - tk < 90 and white(canvas()) < total * 0.9:
            mclick(cx + 131, cy - 72); time.sleep(4)          # GO (also ends the splash)
        g1 = canvas(); w1 = white(g1)
        log('   (canvas after %.0f s: %d%% white)' % (time.time() - tk, 100 * w1 / total))
        mdrag(cx - 70, cy - 150, cx + 230, cy + 90); time.sleep(1.5)
        g2 = canvas(); ch = pixels_changed(g1, g2)
        step('draw', alive() and w1 >= total * 0.9 and ch > 100, 'canvas %d%% white, %d sampled pixels drawn' % (100 * w1 / total, ch))
        shot('3-drawn')
    elif APP == 'Bejeweled 2':
        # Bejeweled 2 (a game, all its own drawing): the title screen's "Click here to play" must
        # take the click and bring up the next screen
        b = win_bounds(pid, [w for w in windows(pid) if w[2] >= 600][0][1]) if [w for w in windows(pid) if w[2] >= 600] else None
        ok = False; detail = 'no game window'
        if b:
            def win(): return grab(b[0], b[1] + 22, b[2], b[3] - 22)
            time.sleep(3); g1 = win(); mclick(b[0] + b[2] / 2, b[1] + 662 * b[3] / 790); time.sleep(4); g2 = win()
            ch = pixels_changed(g1, g2, 6); tot = pixels_total(g1, 6)
            ok = ch > tot * 0.2; detail = '%d%% of the window changed after the Play click' % (100 * ch / tot)
        step('play', alive() and ok, detail)
        shot('3-play')
    elif APP in ('The Unarchiver', 'StuffIt Expander'):
        import zipfile, shutil
        d = '/tmp/rosettus-unar'; shutil.rmtree(d, True); os.makedirs(d)
        body_txt = 'rosettus unarchive %d\n' % (int(time.time()) % 100000)
        z = zipfile.ZipFile(os.path.join(d, 'behave-test.zip'), 'w', zipfile.ZIP_DEFLATED)
        z.writestr('behave-test/hello.txt', body_txt); z.writestr('behave-test/second.txt', 'two\n'); z.close()
        subprocess.call(['open', '-a', WRAP, os.path.join(d, 'behave-test.zip')])
        out = os.path.join(d, 'behave-test', 'hello.txt'); tu = time.time()
        while time.time() - tu < 40 and not (os.path.exists(out) and open(out).read() == body_txt): time.sleep(0.5)
        ok = os.path.exists(out) and open(out).read() == body_txt
        step('unarchive', alive() and ok, '%s after %.1f s: %s' % (out, time.time() - tu, sorted(os.listdir(d))))
        shot('3-unarchived')
        ensure_front(pid)
    elif APP in NO_TEXT:
        log('   (type+copy: not applicable - %s)' % NO_TEXT[APP])
    elif APP.startswith('G4 '):
        # the G4 demos animate a full window (space pauses, Esc quits): frames must keep changing,
        # and stop while paused
        wb = win_bounds(pid, [w for w in windows(pid) if w[0] == 0][0][1]) if [w for w in windows(pid) if w[0] == 0] else None
        ok = False; detail = 'no window'
        if wb:
            def g(): return grab(wb[0], wb[1] + 22, wb[2], wb[3] - 22)
            a = g(); time.sleep(1); b = g(); ch1 = pixels_changed(a, b, 6)
            key(49); time.sleep(1.5); a = g(); time.sleep(1); b = g(); ch2 = pixels_changed(a, b, 6); key(49)   # Space: pause
            ok = ch1 > 20 and (ch2 < ch1 / 4.0 or APP == 'G4 Quasar')   # (Quasar's Space is autoplay, not pause)
            detail = 'sampled pixels changed in 1 s: %d running, %d paused' % (ch1, ch2)
        step('animate', alive() and ok, detail)
        shot('3-animate')
    else:
        settle(pid, 10)   # a new window can take seconds to finish opening (Entourage's lost its first letters)
        ensure_front(pid)
        txt = 'rosetta behave %d' % (int(time.time()) % 100000)
        type_text(txt)
        log('   (typing settled after %.1f s)' % settle(pid))
        shot('3-typed')
        pb = NSPasteboard.generalPasteboard(); pb.clearContents()
        key(0, cmd=True); time.sleep(0.5); key(8, cmd=True); time.sleep(2.5)
        subprocess.call(["osascript", "-e", "tell application \"Finder\" to activate"])   # Classic scrap apps convert on suspend (a live copy goes out at once)
        # wait until the Finder really is in front: a busy Finder (no window open, its Recents
        # window to build) came forward up to 20 s later, in the middle of the next steps, and the
        # test stopped with "not in front" (Save As in Illustrator / InDesign, Cmd-H in TextWrangler)
        try: fnd = int(subprocess.Popen(['pgrep', '-x', 'Finder'], stdout=subprocess.PIPE).communicate()[0].split()[0])
        except Exception: fnd = 0
        tf = time.time()
        while fnd and time.time() - tf < 25 and front_pid() != fnd: time.sleep(0.25)
        if fnd and time.time() - tf > 3: log('   (the Finder came to the front after %.1f s)' % (time.time() - tf))
        t3 = time.time()
        while time.time() - t3 < 15 and txt not in (pb.stringForType_(NSStringPboardType) or ""): time.sleep(0.5)
        got = pb.stringForType_(NSStringPboardType) or ''
        ensure_front(pid)
        if APP in IMAGE_COPY:   # the copy is a picture of what was typed (Equation Editor: PICT / PDF)
            tys = [str(t) for t in (pb.types() or [])]
            pic = [t for t in tys if t in ('com.apple.pict', 'com.adobe.pdf', 'public.tiff', 'Apple PDF pasteboard type', 'Apple PICT pasteboard type', 'NeXT TIFF v4.0 pasteboard type')]
            step('type+copy', alive() and bool(pic), 'pasteboard picture types %s' % pic[:3])
        else: step('type+copy', alive() and txt.lower() in got.lower(), 'pasteboard %r' % got[:60])
    # ---- paste ----
    key(124)  # right arrow: collapse the selection
    wp = windows(pid)
    key(9, cmd=True); time.sleep(1); settle(pid, 8)
    al = new_alerts(pid, wp)
    if al:   # the app answered the paste with an alert: note it, press its default button
        shot('4-paste-alert'); key(36); time.sleep(1)
        log('   (paste alert %s dismissed)' % (al[:2],))
    step('paste', alive(), '')
    shot('4-pasted')
    # ---- save, close, reopen (apps listed in SAVE) ----
    if APP in SAVE:
        ext, commit, magic = SAVE[APP]
        sd = '/tmp/rosettus-save'
        import shutil; shutil.rmtree(sd, True); os.makedirs(sd)
        name = 'behave' + str(int(time.time()) % 100000)
        path = os.path.join(sd, name + '.' + ext)
        ensure_front(pid)   # (the copy check activated the Finder)
        for c in commit: key(c[0], cmd=len(c) > 1 and c[1]); time.sleep(1)   # leave text editing
        ensure_front(pid); w0 = windows(pid)
        key(1, cmd=True, shift=True); time.sleep(3)                           # Save As...
        key(5, cmd=True, shift=True); time.sleep(1.5); type_text(sd); time.sleep(0.5); key(36); time.sleep(2)   # Go to folder
        key(0, cmd=True); time.sleep(0.3); type_text(name); time.sleep(0.5); key(36)
        ts = time.time(); answered = 0
        import glob
        def saved(): return [p for p in glob.glob(os.path.join(sd, name + '*')) if os.path.getsize(p) > 0]
        while time.time() - ts < 40 and not saved():
            time.sleep(1)
            nw = [w for w in windows(pid) if w not in w0 and w[0] in (0, 3, 8)]
            if nw and time.time() - ts > 3 and answered < 3 and front_pid() == pid:
                key(36); answered += 1; time.sleep(2)                            # its options dialog (Illustrator, Photoshop): defaults
        time.sleep(2); settle(pid, 10)
        if not os.path.exists(path):   # Carbon save panels add no extension unless asked (GoLive: TEXT/GoMk 'behaveN')
            import glob
            alt = sorted(glob.glob(os.path.join(sd, name + '*')))
            if alt: path = alt[0]
        got = os.path.exists(path) and os.path.getsize(path) > 0
        head = open(path, 'rb').read(64) if got else ''
        okm = got and (magic in head)
        step('save', alive() and okm, '%s: %s, %d bytes, starts %r (%d option dialog(s))' % (path, 'written' if got else 'MISSING', os.path.getsize(path) if got else 0, head[:12], answered))
        shot('4s-saved')
        # close the document, open the file again
        key(13, cmd=True); time.sleep(3)
        for i in range(2):
            if [w for w in windows(pid) if w[1] == '' and w[0] in (0, 8) and 300 <= w[2] <= 600]: key(2, cmd=True); time.sleep(2)   # Don't Save
        wc = windows(pid)
        subprocess.call(['open', '-a', WRAP, path]); tr = time.time()
        while time.time() - tr < 40 and not [w for w in windows(pid) if name in (w[1] or '')]: time.sleep(1)
        back = [w for w in windows(pid) if name in (w[1] or '')]
        step('reopen', alive() and bool(back), 'window %r after %.0f s' % (back[0][1] if back else None, time.time() - tr))
        shot('4r-reopened')
        ensure_front(pid)
    # ---- preferences ----
    ensure_front(pid)
    w0 = windows(pid); key(43, cmd=True)
    def newwins(): return [w for w in windows(pid) if w not in w0]
    tp = time.time()
    while time.time() - tp < 8 and not newwins(): time.sleep(0.25)   # Entourage's can take seconds
    settle(pid, 10)
    pw = newwins(); w1 = windows(pid)
    step('prefs', alive(), 'windows %d -> %d %s' % (len(w0), len(w1), pw[:1]))
    shot('5-prefs')
    if pw:
        # close it: Esc, then Cmd-W, then the default button of a modal dialog (Entourage); each one
        # only if the preferences window is still there, so nothing else gets the key
        for code, cmd in ((53, False), (13, True), (36, False)):
            if not [w for w in windows(pid) if w in pw]: break
            key(code, cmd=cmd)
            tc = time.time()
            while time.time() - tc < 3 and [w for w in windows(pid) if w in pw]: time.sleep(0.25)
        if [w for w in windows(pid) if w in pw]: log('   (preferences window still open: %s)' % pw[:1])
    # ---- hide / show ----
    ensure_front(pid); fb = front_pid()
    if APP in HIDE_MENU:   # no Hide shortcut (Illustrator CS2): the application menu's Hide item, by mouse
        mclick(HIDE_MENU[APP][0], 11, 0.08); time.sleep(1); mclick(HIDE_MENU[APP][0] + 18, HIDE_MENU[APP][1], 0.08); time.sleep(2)
    else: key(4, cmd=True, ctrl=APP in ('Adobe Photoshop CS2', 'Adobe InDesign CS2')); time.sleep(2)   # (Photoshop: Cmd-H is View > Extras; Hide Photoshop is Ctrl-Cmd-H; InDesign CS2 the same)
    hw = windows(pid); hid = len(hw) == 0
    if not hid: log("   (still on screen after Cmd-H: %s; front pid %d, app pid %d, front before %d)" % (hw, front_pid(), pid, fb))
    show_app(pid)
    t2 = time.time()
    while time.time() - t2 < 8 and not windows(pid): time.sleep(0.5)
    back = len(windows(pid)) > 0
    if not back:   # Dock-style reopen did not unhide: try the Show All path through the Dock icon menu equivalent
        log('   (open did not unhide after 8 s)')
    if APP.startswith('G4 ') and not hid: log('   (hide/show: not applicable - the G4 demos have no Hide; their application menu holds only Quit)')
    else: step('hide/show', alive() and hid and back, 'hidden %s, shown again %s' % (hid, back))
    shot('6-shown')
    # ---- quit ----
    if not KEEP:
        ensure_front(pid)
        key(12, cmd=True); time.sleep(3)
        asked = False
        if alive():
            asked = True
            shot('7-quit-dialog')
            if APP == 'Kid Pix Deluxe 4': mclick(cx - 2, cy + 33); time.sleep(4)   # its own "save your picture?": NO
            sb = win_bounds(pid, 'SimCity 4 Rush Hour') if APP == 'SimCity 4 Rush Hour' else None
            if sb:   # its own box, centred in the game window: "Quit SimCity 4" above "Cancel"
                mclick(sb[0] + sb[2] / 2, sb[1] + sb[3] / 2 - 115); time.sleep(5)
            for i in range(4 if alive() else 0):                       # one question per unsaved window
                key(2, cmd=True); time.sleep(3)      # Don't Save
                if alive(): key(51, cmd=True); time.sleep(3)   # cmd-delete: Don't Save (10.6 sheets)
                cb = win_bounds(pid, 'Close') if alive() and APP == 'Equation Editor' else None
                if cb:   # "This window contains an equation. Do you still want to close it?" (Cancel is the default)
                    mclick(cb[0] + cb[2] * 0.84, cb[1] + cb[3] * 0.79, 0.1); time.sleep(3)
                elif alive() and APP in QUIT_RETURN: key(36); time.sleep(3)   # a question whose default lets it quit
                if not alive(): break
        t1 = time.time()
        while alive() and time.time() - t1 < 10: time.sleep(0.5)
        ok = not alive()
        step('quit', ok, 'save dialog %s' % asked)
        if not ok: os.system('kill -9 %d' % pid)
  except NotFront as ex:
    TARGET[0] = 0
    # what took the front, and what the test app still shows: for the Finder-in-front stops
    try:
        fa = subprocess.Popen(['lsappinfo', 'info', '-only', 'name', subprocess.Popen(['lsappinfo', 'front'], stdout=subprocess.PIPE).communicate()[0].strip()], stdout=subprocess.PIPE).communicate()[0].strip()
        log('   (in front: %s; the test app\'s windows %s)' % (fa, windows(pid)[:4]))
        shot('abort')
    except Exception: pass
    step('aborted', False, 'the test app was not in front, stopped (%s)' % ex)
    os.system('kill -9 %d' % pid)
nr = nreports() - r0
step('reports', nr == 0, '%d new crash/hang reports' % nr)
if nr:
    for l in open(REPIDX).readlines()[-nr:]: log('   ' + l.strip())
passed = sum(1 for n, ok in results if ok)
log('reactivate   open %d, activate %d  (REOPEN counts)' % (REOPEN['open'], REOPEN['activate']))
log('RESULT %s: %d/%d steps ok -> %s' % (APP, passed, len(results), OUT))
s = open(os.path.join(HOME, 'Library/Logs/Rosettus/Behave/summary.tsv'), 'a')
s.write('%s\t%s\t%d/%d\t%s\t%s\n' % (STAMP, APP, passed, len(results), ' '.join(n for n, ok in results if not ok) or '-', OUT))

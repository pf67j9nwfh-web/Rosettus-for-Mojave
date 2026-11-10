#!/usr/bin/python
# sc4test.py: SimCity 4 game test.  The game process (the "SimCity 4 Rush Hour" executable) shows
# its Preferences dialog first and starts a background helper (APlayer), so behave.py's
# document-app steps do not fit.  This test: launch -> accept the Preferences dialog (Return) ->
# wait for the game window -> stay alive 60 s, sampling CPU / memory and taking screenshots ->
# quit (SIGTERM; the game's own quit is a menu inside its window).
# Output: ~/Library/Logs/Rosettus/Behave/SimCity_4-<stamp>/report.txt (+ png), one line in summary.tsv.
import os, sys, time, subprocess
import Quartz as Q

HOME = os.path.expanduser('~')
APP = sys.argv[1] if len(sys.argv) > 1 else 'SimCity 4 Rush Hour'
WRAP = os.path.join(os.environ.get('BEHAVE_WRAPDIR') or os.path.join(HOME, 'Applications/Rosettus'), APP + '.app')
STAMP = time.strftime('%Y%m%d-%H%M%S')
OUT = os.path.join(HOME, 'Library/Logs/Rosettus/Behave', 'SimCity_4-' + STAMP)
os.makedirs(OUT)
rep = open(os.path.join(OUT, 'report.txt'), 'w')
results = []

def log(s):
    rep.write(s + '\n'); rep.flush(); print s; sys.stdout.flush()

def step(name, ok, detail=''):
    results.append(ok); log('%-12s %s %s' % (name, 'ok  ' if ok else 'FAIL', detail))

def pgrep(pat):
    out = subprocess.Popen(['pgrep', '-f', pat], stdout=subprocess.PIPE).communicate()[0].split()
    return [int(p) for p in out]

def windows(pid):
    r = []
    for w in Q.CGWindowListCopyWindowInfo(Q.kCGWindowListOptionOnScreenOnly, Q.kCGNullWindowID) or []:
        if int(w.get('kCGWindowOwnerPID', 0)) != pid: continue
        b = w.get('kCGWindowBounds', {})
        if b.get('Width', 0) < 100 or b.get('Height', 0) < 100: continue
        r.append((int(w['kCGWindowNumber']), w.get('kCGWindowName', '') or '', int(b['Width']), int(b['Height'])))
    return r

def usage(pid):
    out = subprocess.Popen(['ps', '-o', '%cpu=,rss=', '-p', str(pid)], stdout=subprocess.PIPE).communicate()[0].split()
    return (float(out[0]), int(out[1]) // 1024) if len(out) == 2 else (0.0, 0)

def shot(name, wid=None):
    subprocess.call(['screencapture', '-x', '-o'] + (['-l%d' % wid] if wid else []) + [os.path.join(OUT, name + '.png')])

def key(code):
    for d in (True, False):
        e = Q.CGEventCreateKeyboardEvent(None, code, d); Q.CGEventPost(Q.kCGHIDEventTap, e); time.sleep(0.05)

PAT = APP + '.app/Contents/MacOS/translate'
before = set(pgrep(PAT))
t0 = time.time()
subprocess.call(['open', WRAP])
pid = 0
while time.time() - t0 < 60 and not pid:
    new = [p for p in pgrep(PAT) if p not in before]
    if new: pid = new[0]
    else: time.sleep(0.5)
step('launch', pid > 0, 'pid %d after %.1fs' % (pid, time.time() - t0))
if pid:
    game = []; prefs = False
    while time.time() - t0 < 150 and pid in pgrep(PAT) and not game:
        ws = windows(pid)
        if any('Preferences' in w[1] for w in ws) and not prefs:
            prefs = True; shot('1-preferences', [w for w in ws if 'Preferences' in w[1]][0][0]); time.sleep(1); key(36)
        game = [w for w in ws if w[2] >= 640 and w[3] >= 480]
        if not game: time.sleep(1)
    step('window', bool(game), '%s after %.1fs%s' % (game[:1], time.time() - t0, ' (Preferences accepted)' if prefs else ''))
    if game: time.sleep(8); shot('2-game', game[0][0])
    samples = []
    t1 = time.time()
    while time.time() - t1 < 60 and pid in pgrep(PAT):
        samples.append(usage(pid))
        if os.environ.get("SC4_SHOTS") and game: shot("t%02d" % (time.time() - t1), game[0][0])   # SC4_SHOTS=1: the intro, frame by frame
        time.sleep(5)
    alive = pid in pgrep(PAT)
    cpu = sum(s[0] for s in samples) / max(1, len(samples)); rss = max([s[1] for s in samples] or [0])
    step('alive60', alive, 'cpu avg %.0f%%, rss max %d MB, %d samples' % (cpu, rss, len(samples)))
    w = windows(pid)
    if w: shot('3-final', w[0][0])
    for p in [pid] + pgrep('APlayer'):
        try: os.kill(p, 15)
        except OSError: pass
    time.sleep(3)
    step('quit', pid not in pgrep(PAT), '')
ok = sum(1 for r in results if r)
log('RESULT %s: %d/%d steps ok -> %s' % (APP, ok, len(results), OUT))
open(os.path.join(HOME, 'Library/Logs/Rosettus/Behave/summary.tsv'), 'a').write('%s\t%s\t%d/%d\t%s\t%s\n' % (STAMP, APP, ok, len(results), '-' if ok == len(results) else 'see report', OUT))

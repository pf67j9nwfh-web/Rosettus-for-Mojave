#!/bin/bash
# install.sh - install Rosettus from a Snow Leopard export.
#
# usage: ./install.sh EXPORT.tar.gz [--root DIR] [--no-agents] [--no-apps]
#        ./install.sh --update            (re-install this repository's files into an existing root)
#        sudo ./install.sh EXPORT.tar.gz --system    (for every user account; the installer package does this)
#
#   EXPORT.tar.gz   made on Snow Leopard by import/sl/export_from_sl.sh
#   --root DIR      install location (default: ~/Library/Rosettus)
#   --no-agents     do not install the launchd agents for the translated helper daemons
#   --no-apps       do not install "Rosettus Launcher.app" into ~/Applications
#   --system        install for all users: /Library/Rosettus, /Library/LaunchAgents and
#                   /Applications/Rosettus Launcher.app (run as root). Administrators can then
#                   patch apps and make launchers without a password; every user can run them.
#
# Without --system nothing here needs administrator rights. Everything goes into one folder plus
# ~/Library/LaunchAgents/com.rosettus.ppc.*.plist; uninstall.sh removes both.
set -e
REPO="$(cd "$(dirname "$0")" && pwd -P)"
ROOT="$HOME/Library/Rosettus"
EXPORT=""; AGENTS=1; APPS=1; UPDATE=0; SYSTEM=0
while [ $# -gt 0 ]; do
  case "$1" in
    --root) ROOT="$2"; shift;;
    --no-agents) AGENTS=0;;
    --no-apps) APPS=0;;
    --update) UPDATE=1;;
    --system) SYSTEM=1; ROOT=/Library/Rosettus;;
    -h|--help) sed -n '2,14p' "$0"; exit 0;;
    *) EXPORT="$1";;
  esac
  shift
done
case "$ROOT" in *" "*) echo "The install folder may not contain spaces: $ROOT"; exit 1;; esac
say() { if [ -t 1 ]; then printf '\033[1m==> %s\033[0m\n' "$*"; else echo "==> $*"; fi; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
if [ $SYSTEM = 1 ]; then
  [ "$(id -u)" = 0 ] || die "--system installs for every user and needs root: sudo $0 $*  (or use the installer package)"
  AGENTDIR=/Library/LaunchAgents; APPDIR=/Applications
else
  AGENTDIR="$HOME/Library/LaunchAgents"; APPDIR="$HOME/Applications"
fi

ver=$(sw_vers -productVersion)
case "$ver" in 10.14|10.14.*) ;; 10.13*|10.15*) echo "warning: tested on macOS 10.14 Mojave only (this is $ver)";;
  *) die "macOS $ver is not supported (Mojave 10.14 is; Catalina and later cannot run 32-bit code)";; esac
[ -x /usr/bin/python ] || die "/usr/bin/python is missing"


if [ $UPDATE = 0 ]; then
  [ -n "$EXPORT" ] || die "give the Snow Leopard export: ./install.sh Rosettus-SL-export.tar.gz"
  [ -e "$EXPORT" ] || die "$EXPORT not found"
  mkdir -p "$ROOT"
  say "Unpacking the Snow Leopard files into $ROOT (3.3 GB)..."
  if [ -d "$EXPORT" ]; then (cd "$EXPORT" && tar cf - .) | (cd "$ROOT" && tar xf -)
  else tar xzf "$EXPORT" -C "$ROOT"; fi
  # exports made in the C locale have mangled non-ASCII names (Hiragino and Chinese fonts): repair them
  /usr/bin/python "$REPO/tools/fix_names.py" "$ROOT" | tail -1
  [ -f "$ROOT/translate.decoded" ] || die "the export has no translate.decoded - was it made by export_from_sl.sh?"
  # An export made by an older Rosettus lacks files added to the manifest since (Snow Leopard's X11,
  # QuickTime codec components, the spelling service, ...): the install works, but those features do
  # not.
  nmiss=$(/usr/bin/python - "$REPO/import/manifest.tsv.gz" "$ROOT" <<'PY'
import gzip, os, sys
n = 0
for l in gzip.open(sys.argv[1]):
    f = l.decode('utf-8').rstrip('\n').split('\t')
    # (non-ASCII names are left out: fix_names.py repairs those, and their normalization varies)
    if len(f) > 1 and f[1] != 'optional' and not any(ord(c) > 127 for c in f[0]) \
       and not os.path.lexists(os.path.join(sys.argv[2], f[0])): n += 1
print(n)
PY
)
  if [ "${nmiss:-0}" -gt 0 ]; then
    echo "WARNING: $nmiss files this release needs are not in the Snow Leopard export (made by an older"
    echo "         Rosettus?). X11 programs, some QuickTime codecs and spell checking will not work."
    echo "         Make a new export with this release's Snow Leopard Export Kit and install again."
  fi
  if ! xattr -p com.apple.FinderInfo "$ROOT/System/Library/CFMSupport/CarbonLib" >/dev/null 2>&1; then
    echo "WARNING: this export has no resource forks (it was copied without them)."
    echo "         Carbon CFM apps (AppleWorks, Office 2004, ...) will fail with cfragNoLibraryErr and"
    echo "         suitcase fonts will be empty. Re-export with this release's export_from_sl.sh, or"
    echo "         repair in place:  tools/copy_forks.sh \"/Volumes/<Snow Leopard disk>\" \"$ROOT\""
  fi
  if [ -s "$ROOT/mismatch.txt" ] && grep -q 'install DVD' "$ROOT/sl_version.txt" 2>/dev/null; then
    # an install DVD's Snow Leopard is another build of many files (machine-specific discs above all)
    echo "note: $(wc -l < "$ROOT/mismatch.txt" | tr -d ' ') files come from a different 10.6 build than the reference Snow Leopard (normal for an export made from an install DVD)"
  elif [ -s "$ROOT/mismatch.txt" ]; then
    echo "warning: $(wc -l < "$ROOT/mismatch.txt" | tr -d ' ') exported files differ from the 10.6.8 build this release was made for:"
    head -5 "$ROOT/mismatch.txt" | sed 's/^/    /'
  fi

  say "Applying this project's patches to the Snow Leopard files..."
  P="$REPO/import/rpatch.py"; PD="$REPO/import/patches"; failed=0
  # A patch is made for one build of the file; X.altN.rpatch carries the same changes for another
  # 10.6.8 build (the install-DVD path: import/dvd). Files in import/optional_patches.txt are left
  # as they are when no patch fits (tools no PowerPC app needs).
  apply_patch() {   # BASE PATCH OUT REL
    python "$P" check "$1" "$2" 2>/dev/null && { python "$P" apply "$1" "$2" "$3"; return; }
    for a in "${2%.rpatch}".alt[0-9]*.rpatch; do
      [ -f "$a" ] && python "$P" check "$1" "$a" 2>/dev/null && { python "$P" apply "$1" "$a" "$3"; return; }
    done
    if grep -qxF "$4" "$REPO/import/optional_patches.txt" 2>/dev/null; then
      echo "  note: $4 is a different build; left unpatched (not needed by PowerPC apps)"; return 3
    fi
    python "$P" apply "$1" "$2" "$3"
  }
  # .prebridge files first: they start from the pristine Snow Leopard file too
  ( cd "$PD" && find . -name '*.prebridge.rpatch' ) | while IFS= read -r p; do
    rel="${p#./}"; rel="${rel%.prebridge.rpatch}"
    rc=0; apply_patch "$ROOT/$rel" "$PD/$p" "$ROOT/$rel.prebridge" "$rel.prebridge" || rc=$?
    [ $rc = 0 ] || [ $rc = 3 ] || { echo "  FAILED $rel.prebridge"; exit 1; }
  done || failed=1
  ( cd "$PD" && find . -name '*.rpatch' ! -name '*.prebridge.rpatch' ! -name '*.alt[0-9]*.rpatch' ! -name translate.rpatch ) | while IFS= read -r p; do
    rel="${p#./}"; rel="${rel%.rpatch}"
    rc=0; apply_patch "$ROOT/$rel" "$PD/$p" "$ROOT/$rel.new" "$rel" || rc=$?
    if [ $rc = 0 ]; then mv -f "$ROOT/$rel.new" "$ROOT/$rel"
    elif [ $rc != 3 ]; then echo "  FAILED $rel"; exit 1; fi
  done || failed=1
  [ $failed = 0 ] || die "some patches did not apply (see above): the export does not match this release"

  say "Preparing the translator..."
  cp "$ROOT/translate.decoded" "$ROOT/translate.tmp"
  codesign --remove-signature "$ROOT/translate.tmp"
  python "$P" apply "$ROOT/translate.tmp" "$PD/translate.rpatch" "$ROOT/translate_mojave"
  rm -f "$ROOT/translate.tmp" "$ROOT/translate.decoded"
  chmod 755 "$ROOT/translate_mojave"
fi

say "Installing the Rosettus runtime..."
install -m 644 "$REPO/bin/liboah_mojave.dylib" "$ROOT/liboah_mojave.dylib"
install -m 755 "$REPO/bin/rlaunch" "$ROOT/rlaunch"
# shell scripts started by PowerPC programs run with a copy of /bin/bash (copies outside /bin honour
# DYLD_INSERT_LIBRARIES) and libppcexec.dylib, so they can start PowerPC tools (src/ppcexec)
mkdir -p "$ROOT/native"
install -m 755 "$REPO/bin/libppcexec.dylib" "$ROOT/native/libppcexec.dylib"
install -m 755 "$REPO/bin/rosettus-kc" "$ROOT/native/rosettus-kc"   # keychain item data for PowerPC programs (src/kchelper)
install -m 755 "$REPO/bin/libjavashim.dylib" "$ROOT/native/libjavashim.dylib"   # PowerPC Java compatibility (tools/install_java.sh)
# /usr/sbin/installer started by a PowerPC installer (CS2): packages whose scripts run PowerPC tools
install -m 755 "$REPO/scripts/installer-shim" "$ROOT/native/installer-shim"
cp /bin/bash "$ROOT/native/bash.new" && codesign --remove-signature "$ROOT/native/bash.new" 2>/dev/null &&
  chmod 755 "$ROOT/native/bash.new" && mv -f "$ROOT/native/bash.new" "$ROOT/native/bash" || rm -f "$ROOT/native/bash.new"
for f in rosettus rjava mkwrapper.sh mkgtb.sh rosettus_patch.py rsrc_icns.py; do install -m 755 "$REPO/scripts/$f" "$ROOT/$f"; done
# refresh the native front end of existing launchers (single-copy check, documents)
for d in /Applications/Rosettus "$HOME/Applications/Rosettus"; do
  for w in "$d"/*.app; do
    [ -f "$w/Contents/MacOS/launcher.sh" ] && [ -w "$w/Contents/MacOS/launcher" ] && cp "$ROOT/rlaunch" "$w/Contents/MacOS/launcher"
    # the camera and the microphone: macOS ends an app that uses them without saying why
    p="$w/Contents/Info.plist"; n=$(basename "$w" .app)
    [ -f "$w/Contents/MacOS/launcher.sh" ] && [ -w "$p" ] && for k in Camera:camera:video\ capture Microphone:microphone:sound\ input; do
      /usr/libexec/PlistBuddy -c "Print :NS${k%%:*}UsageDescription" "$p" >/dev/null 2>&1 ||
        /usr/libexec/PlistBuddy -c "Add :NS${k%%:*}UsageDescription string $n (a PowerPC app run by Rosettus) uses the $(echo "$k" | cut -d: -f2) for $(echo "$k" | cut -d: -f3)." "$p" >/dev/null 2>&1
    done
  done
done 2>/dev/null || true
for f in bridges.spec genbridges.py bridgepatch.py; do install -m 644 "$REPO/bridges/$f" "$ROOT/$f"; done
install -m 755 "$REPO/uninstall.sh" "$ROOT/uninstall.sh"
mkdir -p "$ROOT/tools"
for f in ppc_pkg_install.sh install_saver.py install_widget.py autopatch.py install_java.sh javafix.py install_versioncue.py versioncue_wake_db.py install_qtflash.sh; do
  [ -f "$REPO/tools/$f" ] && install -m 755 "$REPO/tools/$f" "$ROOT/tools/$f"
done
for f in import_java_from_media.sh fetch_java_update.sh; do   # PowerPC Java from Apple's installers
  [ -f "$REPO/import/java/$f" ] && install -m 755 "$REPO/import/java/$f" "$ROOT/tools/$f"
done
[ -d "$REPO/bin/savers" ] && { rm -rf "$ROOT/savers"; cp -R "$REPO/bin/savers" "$ROOT/savers"; }
[ -d "$REPO/bin/prefpanes" ] && { rm -rf "$ROOT/prefpanes"; cp -R "$REPO/bin/prefpanes" "$ROOT/prefpanes"; }
if [ -d "$REPO/bin/versioncue" ]; then
  mkdir -p "$ROOT/versioncue"
  for f in "$REPO/bin/versioncue"/*; do
    [ -f "$f" ] || continue
    cp "$f" "$ROOT/versioncue/$(basename "$f").new"
    mv -f "$ROOT/versioncue/$(basename "$f").new" "$ROOT/versioncue/$(basename "$f")"
  done
  install -m 755 "$REPO/scripts/versioncue" "$ROOT/versioncue/versioncue.new"
  mv -f "$ROOT/versioncue/versioncue.new" "$ROOT/versioncue/versioncue"
fi
if [ $APPS = 1 ] && [ -d "$REPO/bin/Rosettus Preference Pane Viewer.app" ]; then
  mkdir -p "$APPDIR"
  rm -rf "$APPDIR/Rosettus Preference Pane Viewer.app"
  cp -R "$REPO/bin/Rosettus Preference Pane Viewer.app" "$APPDIR/"
  /usr/libexec/PlistBuddy -c "Add :RosettusRoot string $ROOT" "$APPDIR/Rosettus Preference Pane Viewer.app/Contents/Info.plist"
  # Reuse the local OS icon without redistributing Apple's artwork.
  preficon="/Applications/System Preferences.app/Contents/Resources/PrefApp.icns"
  if [ -f "$preficon" ]; then
    for paneapp in "$APPDIR/Rosettus Preference Pane Viewer.app" "$ROOT/prefpanes/Rosettus Preference Pane Host.app"; do
      [ -d "$paneapp" ] || continue
      mkdir -p "$paneapp/Contents/Resources"
      cp "$preficon" "$paneapp/Contents/Resources/AppIcon.icns"
    done
  fi
  codesign -f -s - "$APPDIR/Rosettus Preference Pane Viewer.app"
fi
# ad-hoc signature: SecurityAgent and TCC check the process's code signature
codesign -f -s - --identifier com.apple.translate "$ROOT/translate_mojave" 2>/dev/null

say "Building the guest-toolbox overlay and bridge marks..."
( cd "$ROOT" && python genbridges.py bridges.spec >/dev/null && bash mkgtb.sh >/dev/null &&
  grep ' gtb/' bridges_patch.sh > .gtb_patch.sh && bash .gtb_patch.sh >/dev/null && rm -f .gtb_patch.sh ) ||
  die "could not build the gtb overlay"

if [ -f "$REPO/import/expected.sha1" ]; then
  say "Verifying..."
  bad=$(cd "$ROOT" && shasum -c "$REPO/import/expected.sha1" 2>&1 | grep ': FAILED' || true)
  # another accepted result (expected.alt.sha1: the patched files of the install-DVD build), or
  # an optional file left unpatched
  [ -z "$bad" ] || bad=$(echo "$bad" | while IFS= read -r l; do
    f="${l%: FAILED*}"; f="${f%: FAILED open or read}"
    grep -qxF "$f" "$REPO/import/optional_patches.txt" 2>/dev/null && continue
    ok=0
    [ -f "$REPO/import/expected.alt.sha1" ] && grep -F "  $f" "$REPO/import/expected.alt.sha1" | awk -v f="$f" 'substr($0, 43) == f' |
      while IFS= read -r a; do (cd "$ROOT" && echo "$a" | shasum -c -s) && echo ok; done | grep -q ok && ok=1
    [ $ok = 1 ] || echo "$l"
  done || true)
  [ -z "$bad" ] || { echo "$bad"; die "verification failed: the files above differ from this release"; }
fi

if [ $SYSTEM = 1 ]; then
  # shared install: owned by root, writable by administrators (patching apps, making launchers),
  # readable and runnable by everyone
  say "Setting permissions for all users..."
  mkdir -p "$ROOT/apps" /Applications/Rosettus
  chown -R root:admin "$ROOT" /Applications/Rosettus
  chmod -R u+rwX,g+rwX,o+rX,o-w "$ROOT" /Applications/Rosettus
fi
if [ $AGENTS = 1 ]; then
  say "Installing the helper agents (started on demand, exit when no PowerPC app runs)..."
  mkdir -p "$AGENTDIR"
  for t in "$REPO"/agents/*.plist; do
    a="$AGENTDIR/$(basename "$t")"
    sed "s#__ROOT__#$ROOT#g" "$t" > "$a.new"
    if [ $SYSTEM = 1 ]; then
      chown root:wheel "$a.new"; chmod 644 "$a.new"
      # replace any per-user copy of the agent for the person logged in now, then load the shared one
      u=$(stat -f %Su /dev/console); uid=$(id -u "$u" 2>/dev/null || echo)
      if [ -n "$uid" ] && [ "$uid" != 0 ]; then
        launchctl bootout "gui/$uid/$(basename "$t" .plist)" 2>/dev/null || true
        h=$(dscl . -read "/Users/$u" NFSHomeDirectory 2>/dev/null | awk '{print $2}')
        [ -n "$h" ] && rm -f "$h/Library/LaunchAgents/$(basename "$t")"
      fi
      mv -f "$a.new" "$a"
      [ -n "$uid" ] && [ "$uid" != 0 ] && launchctl bootstrap "gui/$uid" "$a" 2>/dev/null || true
    else
      launchctl unload "$a" 2>/dev/null || true
      mv -f "$a.new" "$a"
      launchctl load "$a"
    fi
  done
fi
if [ $APPS = 1 ] && [ -d "$REPO/bin/Rosettus Launcher.app" ]; then
  say "Installing Rosettus Launcher.app into $APPDIR..."
  mkdir -p "$APPDIR"
  rm -rf "$APPDIR/Rosettus Launcher.app"
  cp -R "$REPO/bin/Rosettus Launcher.app" "$APPDIR/"
  /usr/libexec/PlistBuddy -c "Delete :RosettusRoot" "$APPDIR/Rosettus Launcher.app/Contents/Info.plist" 2>/dev/null || true
  /usr/libexec/PlistBuddy -c "Add :RosettusRoot string $ROOT" "$APPDIR/Rosettus Launcher.app/Contents/Info.plist"
  codesign -f -s - "$APPDIR/Rosettus Launcher.app" 2>/dev/null || true
fi
# Version Cue is optional and uses Adobe's existing installation plus the PPC Java export.
if [ "$SYSTEM" = 1 ] && [ "$AGENTS" = 1 ] && [ "$(id -u)" = 0 ] &&
   [ -f /Library/Preferences/com.adobe.versioncueCS2.plist ] &&
   [ -f "$ROOT/System/Library/Frameworks/JavaVM.framework/Versions/1.5.0/Libraries/libclient.dylib" ] &&
   [ -x "$ROOT/versioncue/controller" ]; then
  /usr/bin/python "$ROOT/tools/install_versioncue.py" --root "$ROOT" ||
    say "Version Cue setup did not complete; run tools/install_versioncue.py for diagnostics."
fi
say "Done."
echo "Run a PowerPC app:   open \"$APPDIR/Rosettus Launcher.app\"   (or: $ROOT/rosettus /path/App.app)"
echo "Patch an app so a double-click just works:   python $ROOT/rosettus_patch.py /path/App.app"

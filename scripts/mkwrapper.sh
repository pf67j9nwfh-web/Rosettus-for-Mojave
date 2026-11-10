#!/bin/bash
# mkwrapper.sh "/path/to/PowerPC App.app" [ENV=VALUE ...]
# Makes a small Intel launcher bundle with the same name and icon that runs the PowerPC app through
# ROOT/rosettus (Finder double-click, Dock, Spotlight,
# Launchpad all work; no Terminal window). ENV=VALUE pairs (e.g. OAH_GTB=1) are stored in the
# wrapper and exported before launch. Log: ~/Library/Logs/Rosettus/<name>.log
# RL_PPC_COPY=1 (Rosettus Launcher, for a Universal app): the launcher is "<name> (PowerPC).app" with
# its own bundle identifier and no document types, so the app itself keeps running natively by
# default and only this launcher runs its PowerPC version.
set -e
SRC="${1%/}"; shift || true
# a bare classic CFM application (Office 2004: "Microsoft Word", no bundle) is accepted too
BARE=
if [ ! -d "$SRC/Contents" ]; then
  if [ -f "$SRC" ] && [ "$(head -c 8 "$SRC")" = "Joy!peff" ]; then BARE=1
  else echo "not an app bundle or CFM application: $SRC" >&2; exit 1; fi
fi
ROOT="$(cd "$(dirname "$0")" && pwd -P)"
# where launchers and the space-free links go: a shared install (/Library/Rosettus) that this user may
# write to (an administrator) puts them in /Applications/Rosettus for every account; otherwise this
# user's ~/Applications/Rosettus, with links in ROOT/apps or, if ROOT is read-only, ~/.rosettus-apps
case "$ROOT" in "$HOME"/*) SHARED=;; *) SHARED=1;; esac
if [ -n "$SHARED" ] && [ -w "$ROOT" ] && { [ -w /Applications/Rosettus ] || [ -w /Applications ]; }; then
  WDIR=/Applications/Rosettus; LDIR="$ROOT/apps"
else
  WDIR="$HOME/Applications/Rosettus"; LDIR="$ROOT/apps"; [ -w "$ROOT" ] || LDIR="$HOME/.rosettus-apps"
fi
NAME=$(basename "$SRC" .app)
PL="$SRC/Contents/Info.plist"; [ -f "$PL" ] || PL="$SRC/Contents/Info-macos.plist"; [ -n "$BARE" ] && PL=/dev/null
# (PlistBuddy prints "Error Reading File" on stdout: every bare Office app got that text as its
# bundle identifier, so Launch Services took Word, Excel, Entourage... for one application)
get() { [ -f "$PL" ] || return 0; /usr/libexec/PlistBuddy -c "Print :$1" "$PL" 2>/dev/null | grep -v "^Error" || true; }
ID=$(get CFBundleIdentifier); [ -n "$ID" ] || ID="local.rosettus.$(echo "$NAME" | tr -cd 'A-Za-z0-9')"
# Launch Services refuses to open the versions it lists as needing Apple's Java 6 (CoreTypes'
# Exceptions.plist, LSMaximumVersionRequiringJava: Photoshop through CS5, Illustrator through CS6,
# InDesign through CS5.5...) whatever the bundle runs: their launcher failed with -10658 and the app
# never started. Such a launcher gets an identifier of its own.
EXC=/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/Exceptions.plist
if [ -f "$EXC" ] && /usr/libexec/PlistBuddy -c "Print :Additions:$ID:LSMaximumVersionRequiringJava" "$EXC" >/dev/null 2>&1; then
  ID="$ID.rosettus"
fi
PPCCOPY=; [ "$RL_PPC_COPY" = 1 ] && [ -z "$BARE" ] && PPCCOPY=1
# (a normal launcher carries version + ".1" so Launch Services prefers it to the PowerPC original)
VSUF=.1; if [ -n "$PPCCOPY" ]; then NAME="$NAME (PowerPC)"; ID="$ID.rosettus-ppc"; VSUF=; fi
ICON=$(get CFBundleIconFile)
# the creator code: apps that look for a running copy of themselves (Internet Explorer) compare
# process signatures, and a wrapper registered as '????' matched an unrelated app (IE activated it
# and quit at launch)
SIG=$(get CFBundleSignature)
[ -n "$SIG" ] && [ "$SIG" != "????" ] || SIG=$(head -c 8 "$SRC/Contents/PkgInfo" 2>/dev/null | tail -c 4)
# a single-file Classic app keeps its creator in the Finder info (Office 2004: MSWD, MDr1, OFR2...);
# the Office helpers find themselves by it and crashed at start under a wrapper registered as '????'
[ -z "$SIG" ] && [ -n "$BARE" ] && SIG=$(/usr/bin/python -c 'import sys, xattr
try: print xattr.getxattr(sys.argv[1], "com.apple.FinderInfo")[4:8]
except Exception: pass' "$SRC" 2>/dev/null)
case "$SIG" in ????) ;; *) SIG=;; esac; [ "$SIG" = "????" ] && SIG=
[ -n "$ICON" ] && case "$ICON" in *.icns) ;; *) ICON="$ICON.icns";; esac
SHORT=$(get CFBundleShortVersionString); VERS=$(get CFBundleVersion)
# space-free link to the real bundle (paths with spaces trip parts of the PowerPC side)
mkdir -p "$LDIR"
if [ -n "$BARE" ]; then LINK="$SRC"; else
LINK="$LDIR/$(echo "$NAME" | tr -cd 'A-Za-z0-9._-').app"
ln -sfn "$SRC" "$LINK"; fi
# apps that keep support folders next to themselves (AppleWorks Essentials, Starting Points...) look
# for them beside the running bundle: mirror the siblings next to the link too
PARENT=$(dirname "$SRC")
[ -n "$BARE" ] && PARENT=/Applications   # the bare app runs from its own folder
MIRROR=
# Only an app's own folder is mirrored: one with many items (Downloads, Desktop, /tmp) is not an app
# folder, and mirroring /tmp put thousands of links into the install's apps folder.
NSIB=$(ls -A "$PARENT" 2>/dev/null | wc -l | tr -d ' ')
case "$PARENT" in /Applications|$HOME/Applications|$HOME|$HOME/Desktop|$HOME/Downloads|$HOME/Documents|/tmp|/private/tmp|/) ;; *)
  if [ "${NSIB:-0}" -le 64 ]; then
  MIRROR=1
  for sib in "$PARENT"/*; do b=$(basename "$sib"); [ "$sib" = "$SRC" ] && continue
    case "$b" in *.app) continue;; esac
    [ -e "$LDIR/$b" ] || ln -sfn "$sib" "$LDIR/$b"; done   # -f: a dangling link from an older copy of the app
  fi;;
esac
W="$WDIR/$NAME.app"
rm -rf "$W"; mkdir -p "$W/Contents/MacOS" "$W/Contents/Resources"
[ -n "$ICON" ] && [ -f "$SRC/Contents/Resources/$ICON" ] && cp "$SRC/Contents/Resources/$ICON" "$W/Contents/Resources/AppIcon.icns"
[ -n "$BARE" ] && /usr/bin/python "$ROOT/rsrc_icns.py" "$SRC" "$W/Contents/Resources/AppIcon.icns" || true
: > "$W/Contents/Resources/rosetta.env"
for kv in "$@"; do echo "export $kv" >> "$W/Contents/Resources/rosetta.env"; done
cat > "$W/Contents/MacOS/launcher.sh" <<EOF
#!/bin/bash
# Rosetta launcher for $NAME (made by mkwrapper.sh)
HERE="\$(cd "\$(dirname "\$0")/.." && pwd)"
. "\$HERE/Resources/rosetta.env"
export OAH_TRANSLATE_LINK="\$HERE/MacOS/translate"
mkdir -p "\$HOME/Library/Logs/Rosettus"
LOG="\$HOME/Library/Logs/Rosettus/$NAME.log"
[ -f "\$LOG" ] && mv -f "\$LOG" "\$LOG.old"
cd "$ROOT"
LINK="$LINK"
if [ ! -e "\$LINK" ]; then
  # the space-free link went away (Rosetta was reinstalled): make it again, in the user's own
  # ~/.rosettus-apps if the install folder is not writable for this account
  mkdir -p "\$(dirname "\$LINK")" 2>/dev/null
  ln -sfn "$SRC" "\$LINK" 2>/dev/null || { mkdir -p "\$HOME/.rosettus-apps"; LINK="\$HOME/.rosettus-apps/\$(basename "\$LINK")"; ln -sfn "$SRC" "\$LINK"; }
  if [ "$MIRROR" = 1 ]; then for sib in "$PARENT"/*; do b=\$(basename "\$sib"); case "\$b" in *.app) continue;; esac
    [ -e "\$(dirname "\$LINK")/\$b" ] || ln -sfn "\$sib" "\$(dirname "\$LINK")/\$b" 2>/dev/null; done; fi
fi
exec ./rosettus "\$LINK" > "\$LOG" 2>&1
EOF
chmod +x "$W/Contents/MacOS/launcher.sh"
# native front end: catches the launch odoc/pdoc (documents opened from Finder) and runs launcher.sh
cp "$ROOT/rlaunch" "$W/Contents/MacOS/launcher"
cat > "$W/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>launcher</string>
	<key>CFBundleIdentifier</key><string>$ID</string>
	<key>CFBundleName</key><string>$NAME</string>
	<key>CFBundleDisplayName</key><string>$NAME</string>
	<key>CFBundlePackageType</key><string>APPL</string>
${SIG:+	<key>CFBundleSignature</key><string>$SIG</string>
}	<key>CFBundleIconFile</key><string>AppIcon</string>
	<key>CFBundleShortVersionString</key><string>${SHORT:-1.0}</string>
	<key>CFBundleVersion</key><string>${VERS:-1}$VSUF</string>
	<key>LSMinimumSystemVersion</key><string>10.9</string>
	<key>NSHighResolutionCapable</key><false/>
	<key>NSCameraUsageDescription</key><string>$NAME (a PowerPC app run by Rosettus) uses the camera for video capture.</string>
	<key>NSMicrophoneUsageDescription</key><string>$NAME (a PowerPC app run by Rosettus) uses the microphone for sound input.</string>
	<key>RosettaGuestApp</key><string>$SRC</string>
</dict>
</plist>
EOF
# document / URL types of the guest app, so Finder can open documents with the wrapper
PB=/usr/libexec/PlistBuddy
for key in CFBundleDocumentTypes CFBundleURLTypes UTExportedTypeDeclarations UTImportedTypeDeclarations; do
  [ -n "$PPCCOPY" ] && break      # the native app keeps its documents
  if $PB -x -c "Print :$key" "$PL" > /tmp/mkw.$$.plist 2>/dev/null; then
    $PB -c "Add :$key array" "$W/Contents/Info.plist" >/dev/null 2>&1
    $PB -c "Merge /tmp/mkw.$$.plist :$key" "$W/Contents/Info.plist" >/dev/null 2>&1 || true
  fi
done
rm -f /tmp/mkw.$$.plist
# their icons
$PB -c "Print :CFBundleDocumentTypes" "$PL" 2>/dev/null | sed -n "s/.*CFBundleTypeIconFile = //p" | sort -u | while read -r ic; do
  for f in "$SRC/Contents/Resources/$ic" "$SRC/Contents/Resources/$ic.icns"; do if [ -f "$f" ]; then cp "$f" "$W/Contents/Resources/"; fi; done
done
touch "$W"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$W"
echo "made $W -> $SRC ($*)"

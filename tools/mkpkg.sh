#!/bin/bash
# tools/mkpkg.sh [VERSION] - build "Rosettus for Mojave VERSION.pkg" in dist/.
#
# The package contains only this project's files (no Apple software), like the repository:
#   /Applications/Rosettus Launcher.app      the GUI, for every user
#   /Library/Rosettus/Setup/                 install.sh, the patches, manifest and runtime files
# Its postinstall script finishes the install for every user (install.sh --system) when it finds
# the Snow Leopard export: next to the package, or in the logged-in user's Downloads or Desktop.
# Otherwise Rosettus Launcher asks for the export the first time it is opened.
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
# Refuse to package a bin/ file whose sources changed after it was built (see tools/binstamp.sh).
"$R/tools/binstamp.sh" --check || [ -n "$ROSETTUS_ALLOW_STALE" ] || {
  echo "mkpkg: rebuild the files above and run tools/binstamp.sh --update NAME (or set ROSETTUS_ALLOW_STALE=1)"; exit 1; }
V="${1:-$(cd "$R" && git describe --tags --always 2>/dev/null || echo 1.0)}"
V="${V#v}"
W="$(mktemp -d /tmp/rmpkg.XXXXXX)"
trap 'rm -rf "$W"' EXIT
S="$W/root/Library/Rosettus/Setup"
mkdir -p "$S" "$W/root/Applications" "$W/scripts" "$W/res" "$R/dist"
for d in import bin scripts bridges agents tools; do cp -R "$R/$d" "$S/"; done
cp "$R/install.sh" "$R/uninstall.sh" "$R/LICENSE" "$R/RELEASE_NOTES.md" "$S/"
# The Snow Leopard side (export_from_sl.sh, Install Rosetta.app, the Java export for Tiger) stays in
# the Setup kit, and a copy the user can find goes to /Applications/Rosettus as the "Snow Leopard
# Export Kit": the folder to copy to the Snow Leopard Mac, with its own README.
K="$W/root/Applications/Rosettus/Snow Leopard Export Kit"
mkdir -p "$K"
cp -R "$R/import/sl/." "$K/"
rm -f "$K/dump_text_ppc.c"
mkdir -p "$K/tiger"; cp -R "$R/import/tiger/." "$K/tiger/"
cp "$R/import/manifest.tsv.gz" "$K/manifest.tsv.gz"
cp "$R/import/sl/README.txt" "$K/README.txt" 2>/dev/null || true
# Without a Snow Leopard Mac: the same export made on this Mac from a Snow Leopard install DVD and
# Apple's 10.6.8 updates (import/dvd), as a folder with a double-clickable .command
D="$W/root/Applications/Rosettus/Snow Leopard Install DVD Import"
mkdir -p "$D"
cp -R "$R/import/dvd/." "$D/"
cp "$R/import/sl/export_from_sl.py" "$R/import/manifest.tsv.gz" "$D/"
# PowerPC Java from Apple's installers (import/java), with a double-clickable .command
J="$W/root/Applications/Rosettus/PowerPC Java Import"
mkdir -p "$J"
cp -R "$R/import/java/." "$J/"
chmod 755 "$J"/*.sh "$J"/*.command
find "$S" -name .DS_Store -delete
cp -R "$R/bin/Rosettus Launcher.app" "$W/root/Applications/"
/usr/libexec/PlistBuddy -c "Delete :RosettusRoot" "$W/root/Applications/Rosettus Launcher.app/Contents/Info.plist" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :RosettusRoot string /Library/Rosettus" "$W/root/Applications/Rosettus Launcher.app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $V" "$W/root/Applications/Rosettus Launcher.app/Contents/Info.plist"   # the release shows in Get Info / About
codesign -f -s - "$W/root/Applications/Rosettus Launcher.app" 2>/dev/null || true

cat > "$W/scripts/postinstall" <<'EOF'
#!/bin/bash
# Rosettus package: finish the install for every user account.
exec >> /Library/Logs/RosettusInstall.log 2>&1
echo "=== $(date) postinstall"
S=/Library/Rosettus/Setup
chown -R root:wheel "$S"; chmod -R go-w "$S"
u=$(stat -f %Su /dev/console); h=""
[ -n "$u" ] && [ "$u" != root ] && h=$(dscl . -read "/Users/$u" NFSHomeDirectory 2>/dev/null | awk '{print $2}')
EXPORT=""
for c in "$(dirname "$PACKAGE_PATH")/Rosettus-SL-export.tar.gz" ${h:+"$h/Downloads/Rosettus-SL-export.tar.gz" "$h/Desktop/Rosettus-SL-export.tar.gz"}; do
  [ -f "$c" ] && { EXPORT="$c"; break; }
done
if [ -x /Library/Rosettus/translate_mojave ]; then
  echo "updating the existing install"
  "$S/install.sh" --update --system --no-apps; st=$?
elif [ -n "$EXPORT" ]; then
  echo "installing from $EXPORT"
  "$S/install.sh" "$EXPORT" --system --no-apps; st=$?
else
  echo "no Snow Leopard export found: Rosettus Launcher will ask for it"
  mkdir -p /Applications/Rosettus; chown root:admin /Library/Rosettus /Applications/Rosettus; chmod 775 /Library/Rosettus /Applications/Rosettus
  st=0
fi
chown -R root:admin "/Applications/Rosettus Launcher.app"; chmod -R go-w "/Applications/Rosettus Launcher.app"
echo "postinstall exit $st"
exit $st
EOF
chmod 755 "$W/scripts/postinstall"

# Bundles are not relocatable: Rosettus Launcher.app is in the payload twice (in /Applications and in
# Setup/bin, with the same bundle identifier). With relocation (pkgbuild's default) an upgrade over an
# installed copy sent both to /Applications and failed: "Rosettus Launcher.app couldn't be moved to
# Applications because an item with the same name already exists".
pkgbuild --analyze --root "$W/root" "$W/components.plist" >/dev/null
i=0
while /usr/libexec/PlistBuddy -c "Print :$i:RootRelativeBundlePath" "$W/components.plist" >/dev/null 2>&1; do
  /usr/libexec/PlistBuddy -c "Delete :$i:BundleIsRelocatable" "$W/components.plist" >/dev/null 2>&1 || true
  /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$W/components.plist"
  i=$((i + 1))
done
pkgbuild --root "$W/root" --scripts "$W/scripts" --component-plist "$W/components.plist" --identifier io.github.rosettus --version "$V" \
         --install-location / --ownership recommended "$W/Rosettus.pkg" >/dev/null

cat > "$W/res/welcome.html" <<'EOF'
<html><body style="font-family:-apple-system,Helvetica;font-size:13px">
<h2>Rosettus for Mojave</h2>
<p>Runs PowerPC Mac applications on macOS 10.14 Mojave with the Rosetta translator from Mac OS X 10.6 Snow Leopard.</p>
<p>This package installs <b>Rosettus Launcher</b> in the Applications folder and the Rosettus runtime in
<code>/Library/Rosettus</code>, for every user account on this Mac.</p>
<p><b>No Apple software is included.</b> The Snow Leopard files come from your own Snow Leopard Mac:
run <code>import/sl/export_from_sl.sh</code> there, and copy <code>Rosettus-SL-export.tar.gz</code>
next to this package or into your Downloads folder before installing. If it is not found, Rosettus Launcher
asks for it the first time you open it.</p>
<p>No Snow Leopard Mac? After installing, <b>Applications &rsaquo; Rosettus &rsaquo; Snow Leopard Install DVD Import</b>
makes the same files on this Mac from a Mac OS X 10.6 install DVD (or its disc image) and Apple's free 10.6.8 updates.
The <b>Snow Leopard Export Kit</b> folder next to it is what you copy to a Snow Leopard Mac.</p>
</body></html>
EOF
cat > "$W/res/conclusion.html" <<'EOF'
<html><body style="font-family:-apple-system,Helvetica;font-size:13px">
<h2>Installed</h2>
<p>Open <b>Rosettus Launcher</b> from the Applications folder, click <b>Search This Mac</b>, then <b>Patch All…</b>.
Patched apps open with a normal double-click for every user. Administrators can patch apps; everyone can run them.</p>
<p>Install log: <code>/Library/Logs/RosettusInstall.log</code>. To remove everything:
<code>sudo /Library/Rosettus/Setup/uninstall.sh --system</code></p>
</body></html>
EOF
cat > "$W/dist.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Rosettus for Mojave $V</title>
    <welcome file="welcome.html" mime-type="text/html"/>
    <conclusion file="conclusion.html" mime-type="text/html"/>
    <options customize="never" require-scripts="true" rootVolumeOnly="true" hostArchitectures="x86_64"/>
    <volume-check><allowed-os-versions><os-version min="10.13" before="10.15"/></allowed-os-versions></volume-check>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <choices-outline><line choice="default"><line choice="io.github.rosettus"/></line></choices-outline>
    <choice id="default"/>
    <choice id="io.github.rosettus" visible="false"><pkg-ref id="io.github.rosettus"/></choice>
    <pkg-ref id="io.github.rosettus" version="$V" onConclusion="none">Rosettus.pkg</pkg-ref>
</installer-gui-script>
EOF
OUT="$R/dist/Rosettus for Mojave $V.pkg"
productbuild --distribution "$W/dist.xml" --resources "$W/res" --package-path "$W" "$OUT" >/dev/null
echo "built $OUT ($(du -h "$OUT" | cut -f1))"

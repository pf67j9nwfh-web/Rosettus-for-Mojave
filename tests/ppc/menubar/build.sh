#!/bin/bash
# build.sh: the four PowerPC menu bar test apps into ./build (Snow Leopard, Xcode 3.2 PowerPC compilers)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/build"; mkdir -p "$OUT"
mk() {   # mk DEFINE "App Name" bundle-id uielement(0/1)
  local APP="$OUT/$2.app"
  rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
  gcc-4.0 -arch ppc -O1 -isysroot /Developer/SDKs/MacOSX10.5.sdk -mmacosx-version-min=10.4 -D$1 \
    -framework Cocoa -framework Carbon -o "$APP/Contents/MacOS/$2" "$HERE/main.m"
  local UI=""; [ "$4" = 1 ] && UI="<key>LSUIElement</key><string>1</string>"
  cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>$2</string>
	<key>CFBundleIdentifier</key><string>$3</string>
	<key>CFBundleName</key><string>$2</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>LSMinimumSystemVersion</key><string>10.4</string>
	<key>NSPrincipalClass</key><string>NSApplication</string>
	$UI
</dict>
</plist>
EOF
  printf 'APPL????' > "$APP/Contents/PkgInfo"
  file "$APP/Contents/MacOS/$2"
}
mk MODE_DOCK "PPC Menu Dock" io.github.rosettus.test.menudock 0
mk MODE_BAR "PPC Menu Bar Only" io.github.rosettus.test.menubar 1
mk MODE_DYN "PPC Menu Dynamic" io.github.rosettus.test.menudyn 1
mk MODE_ICON "PPC Menu Icon" io.github.rosettus.test.menuicon 1
echo built

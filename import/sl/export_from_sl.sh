#!/bin/bash
# export_from_sl.sh - run on a Mac with Mac OS X 10.6 Snow Leopard and Rosetta installed.
#
# Copies the Snow Leopard system files that Rosettus needs (PowerPC slices of the
# frameworks, Rosetta's translator, fonts, ...) into one archive that you then copy to your
# Mojave Mac and give to install.sh. Apple's files are never part of the repository: they come
# from your own Snow Leopard installation.
#
# usage: ./export_from_sl.sh [OUTPUT.tar.gz]      (default: ~/Desktop/Rosettus-SL-export.tar.gz)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HOME/Desktop/Rosettus-SL-export.tar.gz}"
WORK="${TMPDIR:-/tmp}/rosetta-sl-export.$$"

ver=$(sw_vers -productVersion)
case "$ver" in
  10.6|10.6.*) ;;
  *) echo "This script must run on Mac OS X 10.6 Snow Leopard (this is $ver)."; exit 1;;
esac
[ "$ver" = "10.6.8" ] || echo "Note: made for 10.6.8; on $ver some files may differ (they are listed at the end)."

if [ ! -x /usr/libexec/oah/translate ]; then
  echo "Rosetta is not installed. Open \"Install Rosetta.app\" (in this folder) - Snow Leopard"
  echo "will offer to download and install Rosetta - then run this script again."
  open "$HERE/Install Rosetta.app" 2>/dev/null || true
  exit 1
fi

avail=$(df -k "$(dirname "$OUT")" | awk 'NR==2 {print $4}')
if [ "${avail:-0}" -lt 6000000 ]; then
  echo "Need about 6 GB free next to $OUT (3.3 GB of files plus the archive)."; exit 1
fi

echo "Collecting files into $WORK (about 3.3 GB, 115,000 files; takes 5-15 minutes)..."
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
MANIFEST="$HERE/../manifest.tsv.gz"; [ -f "$MANIFEST" ] || MANIFEST="$HERE/manifest.tsv.gz"   # repository / Export Kit
/usr/bin/python "$HERE/export_from_sl.py" "$MANIFEST" "$HERE/dump_text_ppc" "$WORK"

echo "Writing $OUT ..."
( cd "$WORK" && cp mismatch.txt missing.txt root/ 2>/dev/null; sw_vers > root/sl_version.txt
  # UTF-8 locale: in the C locale (an ssh session, a .command) tar stored non-ASCII names (the
  # Japanese and Chinese fonts) so that they came out mangled on extraction (tools/fix_names.py)
  LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 tar czf "$OUT" -C root . )
ls -lh "$OUT"
echo
echo "Done. Copy $(basename "$OUT") to your Mojave Mac and run:"
echo "    ./install.sh $(basename "$OUT")"

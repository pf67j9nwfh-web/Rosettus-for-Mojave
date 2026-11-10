#!/bin/bash
# behave_all.sh [apps...]: run behave.py for each wrapper in turn (default: every wrapper whose guest
# app is not already running), then print summary.tsv lines of this batch.
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || exit 1
apps=("$@")
if [ ${#apps[@]} -eq 0 ]; then for w in "${BEHAVE_WRAPDIR:-$HOME/Applications/Rosettus}"/*.app; do apps+=("$(basename "$w" .app)"); done; fi
start=$(date +%Y%m%d-%H%M%S)
for a in "${apps[@]}"; do
  g=$(/usr/libexec/PlistBuddy -c "Print :RosettaGuestApp" "${BEHAVE_WRAPDIR:-$HOME/Applications/Rosettus}/$a.app/Contents/Info.plist" 2>/dev/null)
  if [ -n "$g" ] && pgrep -f "translate.* .*$(basename "$g")" >/dev/null; then echo "skip $a (running)"; continue; fi
  /usr/bin/python "$HERE/behave.py" "$a"
  # a copy the test started again after a crash (its reopen) is not the next app's business, nor are
  # helpers inside its bundle (SimCity's APlayer stayed after a killed game and blocked its next run)
  [ -n "$g" ] && pkill -9 -f "$(basename "$g")/Contents/" 2>/dev/null
  sleep 4
done
awk -F'\t' -v s="$start" '$1 >= s' ~/Library/Logs/Rosettus/Behave/summary.tsv

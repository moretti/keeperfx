#!/bin/bash
# Wrap a KeeperFX.app in a styled, drag-to-/Applications .dmg.
#
# Uses only built-in tools (hdiutil/codesign/osascript). The opened window shows the
# KeeperFX banner background with the app + Applications icons and a drag arrow.
#
# Inputs (environment):
#   APP      path to the .app bundle        (required)
#   DMG      output .dmg path               (required)
#   BG       background .tiff               (required; see tools/macos_dmg_background.py)
#   VOLNAME  mounted volume name            (default: KeeperFX)
#
# The Finder geometry below is tuned to pair with that background image — if you
# regenerate the background with a different layout, update these to match.
set -euo pipefail

APP=${APP:?set APP to the .app bundle path}
DMG=${DMG:?set DMG to the output .dmg path}
BG=${BG:?set BG to the background .tiff}
VOLNAME=${VOLNAME:-KeeperFX}
APP_ITEM=$(basename "$APP")

# --- window + icon geometry (paired with res/dmg-background.tiff) ---
ICON_SIZE=128
WIN_L=220 WIN_T=160 WIN_W=560 WIN_H=460
APP_X=150 APP_Y=246          # app icon centre
APPS_X=410 APPS_Y=246        # Applications icon centre
WIN_R=$((WIN_L + WIN_W))
WIN_B=$((WIN_T + WIN_H))

out_dir=$(dirname "$DMG")
stage="$out_dir/.dmg-stage"
rw="$out_dir/.dmg-rw.dmg"

cleanup() { hdiutil detach "/Volumes/$VOLNAME" >/dev/null 2>&1 || true; rm -rf "$stage" "$rw"; }
trap cleanup EXIT
cleanup
rm -f "$DMG"

# 1. Stage the bundle + Applications shortcut + hidden background.
mkdir -p "$stage/.background"
cp -R "$APP" "$stage/"
ln -s /Applications "$stage/Applications"
cp "$BG" "$stage/.background/background.tiff"
chflags hidden "$stage/.background" 2>/dev/null || true

# 2. Create a writable image sized to the staged content (+headroom), then mount it.
sizem=$(( $(du -sm "$stage" | cut -f1) + 60 ))
hdiutil create -ov -srcfolder "$stage" -volname "$VOLNAME" -fs HFS+ \
  -format UDRW -size "${sizem}m" "$rw" >/dev/null
dev=$(hdiutil attach -readwrite -noverify -noautoopen "$rw" | awk '/^\/dev\//{print $1; exit}')
chflags hidden "/Volumes/$VOLNAME/.background" 2>/dev/null || true

# 3. Lay out the window via Finder (non-fatal: a plain DMG still works if this is skipped,
#    e.g. in a headless session with no Finder).
echo "  Styling Finder window (${ICON_SIZE}px icons) ..."
osascript - "$VOLNAME" "$APP_ITEM" "$WIN_L" "$WIN_T" "$WIN_R" "$WIN_B" \
             "$ICON_SIZE" "$APP_X" "$APP_Y" "$APPS_X" "$APPS_Y" <<'APPLESCRIPT' \
  || echo "  (Finder styling skipped — DMG still works)"
on run argv
  set {vol, appItem, l, t, r, b, isz, ax, ay, px, py} to argv
  tell application "Finder"
    tell disk vol
      open
      set current view of container window to icon view
      set toolbar visible of container window to false
      set statusbar visible of container window to false
      set the bounds of container window to {l as integer, t as integer, r as integer, b as integer}
      set opts to the icon view options of container window
      set arrangement of opts to not arranged
      set icon size of opts to (isz as integer)
      set text size of opts to 13
      set background picture of opts to file ".background:background.tiff"
      set position of item appItem of container window to {ax as integer, ay as integer}
      set position of item "Applications" of container window to {px as integer, py as integer}
      update without registering applications
      delay 1
      close
    end tell
  end tell
end run
APPLESCRIPT

# 4. Detach and compress to the final read-only image.
sync
hdiutil detach "$dev" >/dev/null 2>&1 || hdiutil detach "$dev" -force >/dev/null 2>&1
hdiutil convert "$rw" -format UDZO -imagekey zlib-level=9 -ov -o "$DMG" >/dev/null
echo "Done: $DMG  (drag $APP_ITEM to /Applications)"

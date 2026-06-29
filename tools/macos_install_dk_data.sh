#!/bin/sh
# macos_install_dk_data.sh — install the original Dungeon Keeper files KeeperFX needs.
#
# KeeperFX ships its own generated data, but a handful of original DK files (copyright,
# not redistributable) must be supplied by the user. This pulls them out of the GOG
# "Dungeon Keeper Gold" Windows installer with innoextract and copies DATA/ and SOUND/
# into KeeperFX's writable data directory.
#
# Usage:
#   macos_install_dk_data.sh [GOG_SETUP_EXE] [TARGET_DIR]
#
#     GOG_SETUP_EXE  Path to setup_dungeon_keeper_gold_*.exe. If omitted, looks on
#                    ~/Desktop and ~/Downloads.
#     TARGET_DIR     KeeperFX data dir. Default: ~/Library/Application Support/KeeperFX
#
# Only the full setup_*.exe is needed — NOT the patch_*_to_*.exe (the setup already
# includes the latest "fix" build). When bundled in KeeperFX.app, a self-contained
# innoextract sits next to this script; otherwise it falls back to one on PATH.

set -eu

TARGET_DEFAULT="$HOME/Library/Application Support/KeeperFX"

err()  { printf 'Error: %s\n' "$*" >&2; exit 1; }
info() { printf '%s\n' "$*"; }

# --- locate the GOG installer -------------------------------------------------
INSTALLER="${1:-}"
if [ -z "$INSTALLER" ]; then
    for d in "$HOME/Desktop" "$HOME/Downloads"; do
        for f in "$d"/setup_dungeon_keeper_gold_*.exe; do
            [ -f "$f" ] && INSTALLER="$f" && break
        done
        [ -n "$INSTALLER" ] && break
    done
fi
[ -n "$INSTALLER" ] && [ -f "$INSTALLER" ] || \
    err "GOG installer not found. Pass it as the first argument (setup_dungeon_keeper_gold_*.exe)."
info "Installer: $INSTALLER"

TARGET="${2:-$TARGET_DEFAULT}"
mkdir -p "$TARGET/data" "$TARGET/sound"
info "Target:    $TARGET"

# --- locate innoextract (bundled next to this script, else PATH) --------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -x "$SCRIPT_DIR/innoextract/innoextract" ]; then
    INNO="$SCRIPT_DIR/innoextract/innoextract"
elif command -v innoextract >/dev/null 2>&1; then
    INNO="$(command -v innoextract)"
else
    err "innoextract not found (expected bundled in the app, or: brew install innoextract)."
fi

# --- extract only DATA/ and SOUND/ to a temp dir ------------------------------
TMP="$(mktemp -d "${TMPDIR:-/tmp}/kfx_dkdata.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
info "Extracting original data ..."
"$INNO" -m -I DATA -I SOUND -d "$TMP" "$INSTALLER" >/dev/null

# Locate the real DATA/ and SOUND/ by the files they must contain — the installer
# also unpacks empty save-template dirs (__support/save/{data,sound}) that would
# otherwise match a directory-name search.
DATA_FILE="$(find "$TMP" -type f -iname bluepal.dat | head -1)"
[ -n "$DATA_FILE" ] || err "no BLUEPAL.DAT in installer output — is this Dungeon Keeper Gold?"
SRC_DATA="$(dirname "$DATA_FILE")"
SND_FILE="$(find "$TMP" -type f -iname '*.sbk' | head -1)"

info "Copying into $TARGET ..."
cp -R "$SRC_DATA"/. "$TARGET/data/"
[ -n "$SND_FILE" ] && cp -R "$(dirname "$SND_FILE")"/. "$TARGET/sound/"

# --- verify the install-check file landed (case-insensitive) ------------------
if find "$TARGET/data" -iname bluepal.dat | grep -q .; then
    info "Done. Original Dungeon Keeper data installed."
else
    err "copy finished but data/bluepal.dat is missing — unexpected installer edition."
fi

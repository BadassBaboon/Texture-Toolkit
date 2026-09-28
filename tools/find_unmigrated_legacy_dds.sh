#!/usr/bin/env bash
#
# find_unmigrated_legacy_dds.sh
#
# Run from the TT directory (the folder holding dump/, inject/, hash_migrate.txt and
# TextureToolkit.ini -- see HashAlgorithm in README.md). Lists every 8-hex-digit v1.0-style
# .dds file in inject/ whose hash does NOT appear as the "oldhash" (first) column of any line
# in hash_migrate.txt.
#
# A file that shows up here has not been paired with its current-algorithm hash yet, because the
# game has not drawn that texture in any HashAlgorithm=2 session so far. It is not necessarily a
# real problem: an unused texture (an alternate livery, a leftover from an older game version, ...)
# may simply never be drawn. Play further, or point the camera at whatever uses it, and it should
# be found on a later pass.
#
# Usage: ./find_unmigrated_legacy_dds.sh [inject-dir] [hash_migrate.txt]
# Both arguments default to "inject" and "hash_migrate.txt" in the current directory.

set -u

INJECT_DIR="${1:-inject}"
MIGRATE_FILE="${2:-hash_migrate.txt}"

if [[ ! -f "$MIGRATE_FILE" ]]; then
    echo "error: '$MIGRATE_FILE' not found. Run this from the TT directory, or pass its path as the 2nd argument." >&2
    exit 1
fi

if [[ ! -d "$INJECT_DIR" ]]; then
    echo "error: '$INJECT_DIR' not found. Run this from the TT directory, or pass its path as the 1st argument." >&2
    exit 1
fi

# Old hashes already paired up, uppercased and stripped of an optional "0x" prefix -- the same
# way Texture Toolkit's own filename parsing treats them (see TextureManager::rescan_injected).
declare -A mapped
while read -r old_hex _rest; do
    [[ -z "$old_hex" ]] && continue
    old_hex="${old_hex^^}"
    old_hex="${old_hex#0X}"
    mapped["$old_hex"]=1
done < "$MIGRATE_FILE"

shopt -s nullglob nocaseglob
checked=0
missing=0

for f in "$INJECT_DIR"/*.dds; do
    stem="$(basename "$f")"
    stem="${stem%.*}"          # drop the extension regardless of its case
    upper="${stem^^}"
    upper="${upper#0X}"        # drop an optional "0x"/"0X" prefix

    # Only the 8-hex-digit (legacy) naming is in scope; leave 16-hex and Special K names alone.
    [[ "$upper" =~ ^[0-9A-F]{8}$ ]] || continue

    checked=$((checked + 1))
    if [[ -z "${mapped[$upper]:-}" ]]; then
        echo "$f"
        missing=$((missing + 1))
    fi
done
shopt -u nullglob nocaseglob

echo "---" >&2
echo "$missing of $checked legacy-named inject file(s) not yet in '$MIGRATE_FILE'" >&2

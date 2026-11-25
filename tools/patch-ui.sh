#!/usr/bin/env bash
# BloxStation UI for your copy of UniversalApp.rbxm: the app in PlayStation
# navy/blue (tools/make-theme.py) and "Guide" in the left menu
# (tools/patch-guide-menu.sh).
# usage: tools/patch-ui.sh ORIGINAL.rbxm OUT_DIR   (then tools/put-ua.py OUT_DIR)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
in=$1 out=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
python3 "$here/rbxm-unpack.py" "$in" "$tmp/blob" >/dev/null
python3 "$here/make-theme.py" "$tmp/blob" "$tmp/theme.txt"
mkdir -p "$tmp/step"
# The app's top FoundationProvider maps the theme name to a colour mode
# {dark = ColorMode.Dark, light = ColorMode.Light}; the console runs "light",
# and some pages (game details) only follow the colour mode. Map light to
# ColorMode.Dark too (GETTABLEKS constant 'Light' -> 'Dark').
python3 "$here/rbxm-patch.py" "$in" "$tmp/step/UniversalApp.rbxm" --at-file="$tmp/theme.txt" \
    --bytes=4d170a172f000000301716f7300000004d170a0f31000000301716ef32000000=4d170a172f000000301716f7300000004d170a0f2f000000301716ef32000000
bash "$here/patch-guide-menu.sh" "$tmp/step/UniversalApp.rbxm" "$out"

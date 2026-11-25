#!/usr/bin/env bash
# Adds "Guide" to Roblox's left menu (under More) in your copy of
# UniversalApp.rbxm; selecting it opens the title's on-screen guide.
# usage: tools/patch-guide-menu.sh ORIGINAL.rbxm OUT_DIR   (then tools/put-ua.py OUT_DIR)
#
# The menu list (the bottom/side bar items function in the app bundle) already
# has a "Build" item that only Roblox Studio policies show. Four byte patches
# in that bundle (each must match exactly once; rbxm-patch.py checks):
#  1. LOADNIL R12; JUMPIFNOT R11 (skip the item)  ->  LOADB R11 true; NOP
#  2. the console branch builds {Home, Avatar, Chat, More}; its Chat/Connect
#     choice is replaced by Chat plus SETLIST R12 -> index 5 (the item, last)
#  3. R14 (an experiment that, once loaded, switches to lists without the
#     item) is set to false: GETUPVAL R14; CALL R14  ->  LOADB R14 false; NOP
#  4. the item's page constant "Build" -> "BuildersClub", whose only action is
#     a web page (www.roblox.com/premium/membership) that roblox_webview.h
#     turns into the guide
# and the English label "Build" becomes "Guide" (English text table only).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
in=$1 out=$2
mkdir -p "$out" "$out/.step"
python3 "$here/rbxm-patch.py" "$in" "$out/.step/UniversalApp.rbxm" \
    --bytes=520b0c00c60c00000e0b1600=520b0c00a90b010000000000 \
    --bytes=fb1723000e170200521609006500010052160d0052170a00c5131405=5216090052170a00c5130c02050000000000000052170a00c5131405 \
    --bytes=fb0e19009f0e0102fb0f1a009f0f0102=a90e000000000000fb0f1a009f0f0102 \
    --bytes=0000060700000003ab030387f4030806=0000060700000003d1030387f4030806
python3 "$here/rbxm-patch.py" "$out/.step/UniversalApp.rbxm" "$out/UniversalApp.rbxm" \
    'Build=Guide' '--only-with=Send your local log file to B'
rm -rf "$out/.step"

#!/usr/bin/env bash
# BloxStation - build the title (configured in sce_sys/param-radv.json).
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# radv/radv_smoke.c (the native title that loads Roblox's Android engine) linked
# with a RADV Vulkan driver archive for the PS5, into dist/<titleId>. The link
# is tools/radv-link.sh's: the repository's CRT and C++ runtime, the AGC link
# stubs and the payload SDK's libc++ for ACO.
#
# RADV: by default the prebuilt release under .deps/native/radv-release
# (lib/libvulkan_radeon.ps5.a + include/), else the Mesa fork's build tree
# (PS5_MESA_FORK, default ../PS5_Mesa). RADV_ARCHIVE overrides the archive.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mesa=${PS5_MESA_FORK:-$root/../PS5_Mesa}
release="$root/.deps/native/radv-release"
if [[ -z ${RADV_ARCHIVE:-} && -f $release/lib/libvulkan_radeon.ps5.a ]]; then
    archive="$release/lib/libvulkan_radeon.ps5.a"
    vk_include="$release/include"
else
    archive=${RADV_ARCHIVE:-$mesa/build-ps5/src/amd/vulkan/libvulkan_radeon.a}
    vk_include="$mesa/include"
    [[ -d $release/include ]] && vk_include="$release/include"
fi
sdk_root="$root/.deps/native/ps5-payload-sdk"
native="$root/tooling/native"
tool="$root/build/host/ps5-native-tool"
work="$root/build/radv"
param="${RADV_TITLE_PARAM:-$root/sce_sys/param-radv.json}"

# The host packaging tool (ELF -> SELF), built once from tooling/native.
if [[ ! -x $tool ]]; then
    zlib_root="$root/.deps/native/zlib/root"
    zlib_archive=$(find "$zlib_root" -type f -name libz.a -print -quit 2>/dev/null || true)
    [[ -n $zlib_archive ]] || { echo "missing zlib: run tools/setup-native-dependencies.sh" >&2; exit 2; }
    mkdir -p "$(dirname "$tool")"
    "${CXX:-$(command -v clang++ || command -v clang++-18)}" -std=c++20 -O2 -Wall -Wextra -Werror \
        -I "$zlib_root/usr/include" "$native/native_app_builder.cpp" "$native/self_container.cpp" \
        "$native/elf_object.cpp" "$native/sce_module_writer.cpp" "$zlib_archive" -o "$tool"
fi

for file in "$archive" "$tool" "$param" "$sdk_root/bin/prospero-lld"; do
    [[ -e $file ]] || { echo "missing $file" >&2; exit 2; }
done
command -v glslangValidator >/dev/null || { echo "glslangValidator was not found" >&2; exit 2; }

title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
module_sdk=0x02000009
companion_sdk=0x08050001
fself_magic=0x1D3D154F

mkdir -p "$work/obj" "$work/stubs" "$work/gen"
cc() { PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" "$@"; }

# The smoke test's shaders, as SPIR-V arrays.
: > "$work/gen/radv_smoke_shaders.h.tmp"
for stage in comp vert frag; do
    glslangValidator -V --target-env vulkan1.0 --vn "radv_smoke_$stage" \
        "$root/radv/shaders/smoke.$stage" -o "$work/gen/smoke_$stage.h" > /dev/null
    cat "$work/gen/smoke_$stage.h" >> "$work/gen/radv_smoke_shaders.h.tmp"
done
for shader in tess.vert tess.tesc tess_coord.tese tess_patch.tese tess_varying.vert tess_varying.tesc \
        tess_varying.tese colour.frag tess_level.tesc gs_points.vert gs_16.geom gs_32.geom gs_64.geom \
        gs_100.geom gs_128.geom gs_primid.geom gs_colour.vert gs_colour.geom gs_primid_fixed.geom gs_record.geom \
        scratch.frag bary.vert bary.frag bary6.vert full.vert rate.frag prim_id.vert prim_id.frag prim_id_raw.frag \
        gs_quads.vert gs_triangles.geom ptr_write.comp; do
    name=${shader//./_}
    glslangValidator -V --target-env vulkan1.0 --vn "radv_smoke_$name" \
        "$root/radv/shaders/$shader" -o "$work/gen/smoke_$name.h" > /dev/null
    cat "$work/gen/smoke_$name.h" >> "$work/gen/radv_smoke_shaders.h.tmp"
done
# Mesh shaders need SPIR-V 1.4.
for shader in mesh_ticket.mesh mesh_colour.frag task_payload.task mesh_payload.mesh; do
    name=${shader//./_}
    glslangValidator -V --target-env vulkan1.2 --vn "radv_smoke_$name" \
        "$root/radv/shaders/$shader" -o "$work/gen/smoke_$name.h" > /dev/null
    cat "$work/gen/smoke_$name.h" >> "$work/gen/radv_smoke_shaders.h.tmp"
done
mv "$work/gen/radv_smoke_shaders.h.tmp" "$work/gen/radv_smoke_shaders.h"

cc -std=c11 -O2 -ffile-prefix-map="$root/"= -Wall -Wextra -ffunction-sections -fdata-sections \
    -I "$vk_include" -I "$work/gen" -c "$root/radv/radv_smoke.c" -o "$work/obj/radv_smoke.o"
cc -std=c++20 -O2 -ffile-prefix-map="$root/"= -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
cc -std=c++20 -O2 -ffile-prefix-map="$root/"= -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"

# The in-title WARP gateway (radv/android_probe/warp): lwIP and wireguard-lwip
# from third_party (BSD licences; wireguard-lwip carries
# third_party/wireguard-lwip-ps5.patch). Their own warnings are not ours.
warp_dir="$root/radv/android_probe/warp"
lw="$root/third_party/lwip/src"
wgsrc="$root/third_party/wireguard-lwip/src"
warp_inc=(-I "$warp_dir" -I "$lw/include" -I "$wgsrc" -I "$wgsrc/crypto/refc")
warp_objs=()
for src in "$lw"/core/{init,def,dns,inet_chksum,ip,mem,memp,netif,pbuf,stats,sys,tcp,tcp_in,tcp_out,timeouts,udp}.c \
        "$lw"/core/ipv4/{ip4,ip4_addr,ip4_frag,icmp}.c "$wgsrc"/{wireguard,wireguardif,crypto}.c "$wgsrc"/crypto/refc/*.c; do
    obj="$work/obj/warp_$(basename "${src%.c}").o"
    cc -std=gnu11 -O2 -ffile-prefix-map="$root/"= -w -ffunction-sections -fdata-sections "${warp_inc[@]}" -c "$src" -o "$obj"
    warp_objs+=("$obj")
done
cc -std=gnu11 -O2 -ffile-prefix-map="$root/"= -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections "${warp_inc[@]}" \
    -c "$warp_dir/warp_gateway.c" -o "$work/obj/warp_gateway.o"
warp_objs+=("$work/obj/warp_gateway.o")

# The web relay for the PS5 browser (radv/android_probe/web_relay.c): TLS
# with BearSSL from third_party/bearssl (MIT). Entropy is injected by the
# relay (the title has no getentropy), so the OS seeders are off.
ssl="$root/third_party/bearssl"
ssl_flags=(-std=gnu11 -O2 -ffile-prefix-map="$root/"= -w -ffunction-sections -fdata-sections -DBR_USE_GETENTROPY=0 -DBR_USE_URANDOM=0
    -I "$ssl/inc" -I "$ssl/src")
while IFS= read -r src; do
    rel=${src#"$ssl/src/"}
    obj="$work/obj/ssl_${rel//\//_}"
    obj="${obj%.c}.o"
    cc "${ssl_flags[@]}" -c "$src" -o "$obj"
    warp_objs+=("$obj")
done < <(find "$ssl/src" -name '*.c' | sort)
cc -std=gnu11 -O2 -ffile-prefix-map="$root/"= -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections -I "$ssl/inc" \
    -c "$root/radv/android_probe/web_relay.c" -o "$work/obj/web_relay.o"
warp_objs+=("$work/obj/web_relay.o")
# First-run WARP registration on the console (no warp.json yet).
cc -std=gnu11 -O2 -ffile-prefix-map="$root/"= -Wall -Wextra -ffunction-sections -fdata-sections -I "$ssl/inc" -I "$wgsrc" \
    -c "$warp_dir/warp_register.c" -o "$work/obj/warp_register.o"
warp_objs+=("$work/obj/warp_register.o")

# AGC comes from system modules; these host-link stubs only name the imports.
stub() {
    local library=$1 source=$2
    cc -std=c11 -O2 -fPIC -c "$root/$source" -o "$work/obj/${library}_stub.o"
    "$sdk_root/bin/prospero-lld" --shared -soname "${library}.prx" \
        -o "$work/stubs/${library}.so" "$work/obj/${library}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c
# system dialogs the payload SDK has no stubs for (the in-game browser)
stub libSceWebBrowserDialog vendor/ps5/sdk/stubs/web_browser_dialog_link_stub.c
stub libSceCommonDialog vendor/ps5/sdk/stubs/common_dialog_link_stub.c

# shellcheck source=tools/radv-link.sh
source "$root/tools/radv-link.sh"
radv_link_recipe "$root" "$sdk_root" "$archive" || exit 2
"$sdk_root/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$work/llvm-pie.elf" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "$work/obj/radv_smoke.o" "${warp_objs[@]}" \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    "$work/stubs/libSceWebBrowserDialog.so" "$work/stubs/libSceCommonDialog.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk_root"/target/lib/*.so
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk_root/target/lib" --stub "$work/stubs/libSceAgc.so" \
    --stub "$work/stubs/libSceAgcDriver.so" --stub "$work/stubs/libSceWebBrowserDialog.so" \
    --stub "$work/stubs/libSceCommonDialog.so" --module-sdk "$module_sdk" \
    --companion-sdk "$companion_sdk" --file-name eboot.elf

app="$root/dist/$title_id"
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic "$fself_magic"
cp "$param" "$app/sce_sys/param.json"
# The title's own art (tools/make-art.py); no launch sound.
for asset in icon0.png pic0.dds pic1.dds; do
    cp "$root/sce_sys/bloxstation/$asset" "$app/sce_sys/$asset"
done
[[ -f $root/runtime/libc.prx ]] || bash "$root/tools/rebuild-libc.sh"
(cd "$root/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$root/runtime/libc.prx" "$app/sce_module/libc.prx"
"$tool" self --inspect --file "$app/sce_module/libc.prx" > /dev/null
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
printf 'RADV title: %s (%s bytes; archive %s)\n' "$app" "$(stat -c %s "$app/eboot.bin")" \
    "$(git -C "$mesa" describe --always --dirty --abbrev=11 --exclude='*' 2>/dev/null || echo unknown)"

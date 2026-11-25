# Building

## 1. Tools on the PC (Linux)

clang and clang++ 18 or newer, lld, python3, glslangValidator, curl, unzip.
For building RADV yourself: meson, ninja, and Mesa's python modules
(mako, packaging, pyyaml).

## 2. Dependencies

```sh
tools/setup-native-dependencies.sh
```

This downloads the public PS5 payload SDK and zlib, checks their SHA-256 and
installs them in `.deps/native/` (ignored by git).

## 3. RADV (the Vulkan driver)

The title links RADV as a static archive. Build it from the public Mesa fork
at the pinned revision:

```sh
git clone https://github.com/mpereiraesaa/PS5_Mesa ../PS5_Mesa
tools/build-radv.sh release      # -> .deps/native/radv-release/{lib,include}
```

This takes a long time and a lot of memory. If you already have
`libvulkan_radeon.ps5.a`, copy it to
`.deps/native/radv-release/lib/` and its Vulkan headers to
`.deps/native/radv-release/include/`. `RADV_ARCHIVE=/path/to/archive.a`
overrides the archive.

## 4. The title

```sh
tools/build-radv-title.sh
```

Output: `dist/PPSA99187/` (`eboot.bin`, `sce_module/libc.prx`, `sce_sys/`).
The first run also builds the host packaging tool (`build/host/ps5-native-tool`)
and rebuilds `runtime/libc.prx`, which must match the checksum in
`runtime/libc.prx.sha256`. The build treats a mismatch as an error.

The title ID and name come from `sce_sys/param-radv.json`.
`RADV_TITLE_PARAM=other.json` builds a copy under another title ID.

## 5. Tests

```sh
tests/run.sh
```

Runs on the PC only. It covers the network tunnel: endpoint list, saved
endpoint, gateway address parsing, key generation, private no-overwrite
key storage, and first-run registration against a local fake TLS server
(including a rejected self-signed certificate). It also covers the direct
memory page tracker, file locks and the sign-in cookie jar, all under
AddressSanitizer.

## Optional: BloxStation look and Guide in the left menu

The title has an on-screen guide (5 pages: getting started, menus, every box
under More, in-game controls, web pages and connection). It opens by itself
the first time each player reaches Home. To also put a **Guide** item in
Roblox's left menu, under More, and give the whole app the BloxStation
PlayStation-blue look, patch your own copy of
`assets/ExtraContent/models/UniversalApp/UniversalApp.rbxm`:

```sh
tools/patch-ui.sh UniversalApp.rbxm.orig out   # theme + Guide item
tools/put-ua.py out      # uploads it with its checksum; --restore undoes it
```

`tools/patch-guide-menu.sh` alone adds only the Guide item; `tools/make-theme.py`
explains the colour rule. The game details page keeps a dark grey background
(white text). The guide pages are drawn by
`tools/make-guide-pages.py` (needs Pillow and the Inter font) into
`radv/android_probe/guide_pages.h`; run it again after editing the text.

## Optional: home screen art

The tile on the PS5 home screen is named **BloxStation** and uses
`sce_sys/bloxstation/` (icon0.png, pic0.dds, pic1.dds). `tools/make-art.py`
draws it; six styles are built in (navy, psblue, neon, red, light, sunset; psblue is used):

```sh
tools/make-art.py --preview previews/       # a picture of every style
tools/make-art.py neon sce_sys/bloxstation  # use one, then build again
```

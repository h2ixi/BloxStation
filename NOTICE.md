# Notices

This project is GPL-3.0-or-later (see `LICENSE`). It contains or builds on:

- **PS5_Vulkan** (https://github.com/mpereiraesaa/PS5_Vulkan): the RADV
  test title (`radv/radv_smoke.c`), its build and link scripts. Copyright (C)
  2026 Mihawk-99, GPL-3.0-or-later.
- **ps5-native-app-boilerplate**: the native title tooling in
  `tooling/native/`, `tools/rebuild-libc.sh` and the runtime shim recorded in
  `runtime/`. Copyright (C) 2026 BlackBearReloaded and Mihawk-99,
  GPL-3.0-or-later.
- **lwIP 2.2.1** (`third_party/lwip`, BSD license),
  **wireguard-lwip** (`third_party/wireguard-lwip`, BSD-3-Clause, with
  `third_party/wireguard-lwip-ps5.patch`) and **BearSSL**
  (`third_party/bearssl`, MIT). Details in `third_party/README.md`.
- `radv/android_probe/web_relay_tas.h`: trust anchors generated from the
  Mozilla CA certificate list (MPL-2.0 data).
- `vendor/ps5/sdk/stubs/`: link-time stubs that only name system module
  imports; no Sony code.

Not included, fetched or supplied at build/install time, under their own
licenses: the public PS5 payload SDK and zlib (`tools/setup-native-dependencies.sh`),
the RADV driver archive built from Mesa (MIT), and the Roblox app files,
which you provide from your own copy and which remain Roblox's property.

Roblox is a trademark of Roblox Corporation. PlayStation and PS5 are
trademarks of Sony Interactive Entertainment. This project is not affiliated
with either.

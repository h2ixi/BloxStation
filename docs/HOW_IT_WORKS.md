# How it works

## The title

`radv/radv_smoke.c` is a native PS5 application (an `eboot.bin` started by
ShadowMount+). It sets up the GPU through RADV and then starts the Roblox
engine instead of a test scene (`ROBLOX_STAGE=activity` in
`radv-smoke-env.txt`).

## Loading an Android library on the PS5

- `android_elf_loader.h` maps `libroblox.so` (an x86_64 Android ELF) into
  the title's memory, applies its relocations and runs its constructors.
- Every import the library needs is resolved to a function in
  `bionic_shims.h` (Android's C library on top of the PS5's FreeBSD libc). It
  handles the differences that would otherwise corrupt memory: 2-byte versus
  4-byte `wchar_t`, `errno` values, structure layouts, `mmap` of direct memory
  (`bionic_dm_*.h`) and futexes.
- The engine also makes some Linux system calls directly, without going
  through the C library. They trap, and the title answers them
  (`ROBLOX_OPENAT_TEST=1` enables the `/proc` and file calls games need).
- `fake_jni.h` and `jni_slots.h` stand in for the Java side of the Android app:
  activity lifecycle, platform parameters, settings, cookies, links.

## Engine services

| Engine needs | PS5 provides | File |
|---|---|---|
| Vulkan surface and swapchain | RADV on the PS5 GPU, 120 Hz output | `roblox_vulkan.h` |
| Touch/gamepad input | DualSense via `scePad`; touchpad as mouse | `radv_smoke.c` |
| Text fields | PS5 on-screen keyboard (`sceImeDialog`) | `roblox_keyboard.h` |
| Audio (FMOD) | `sceAudioOut` | `roblox_audio.h` |
| Sign-in cookies | private file on the console | `roblox_cookies.h`, `roblox_accounts.h` |
| Web pages | PS5 browser dialog + local relay | `roblox_webview.h`, `web_relay.c` |
| Guide | pages copied into Roblox's frames before they are shown | `roblox_guide.h`, `roblox_vulkan.h` |
| Look (optional) | Guide menu item and navy theme, patched into your copy of the UI model | `tools/patch-ui.sh`, `tools/make-theme.py` |
| Network | SOCKS5 gateway in the title | `bionic_net.h`, `warp/` |

## Network

Every TCP connection the engine opens goes to an in-title SOCKS5 gateway on
`127.0.0.1:1080` (`warp/warp_gateway.c`). The gateway runs its own TCP/IP
stack (lwIP) inside a WireGuard tunnel to Cloudflare WARP, so DNS and IP blocks
on the local network do not apply. Names are looked up through the tunnel.

- **First launch** (`warp/warp_register.c`): the title makes an X25519 key
  from the console's random device and registers it with Cloudflare over a
  verified TLS connection. It stores `/data/roblox/files/warp.json` (0600) and
  never overwrites it.
- **Choosing a server** (`warp/endpoint_list.h`): it starts with the server
  that worked last time. Until the tunnel comes up it moves on every 6 s
  through 378 address/port pairs (7 Cloudflare ranges x 54 ports). Each step
  changes both the range and the port, so a block on one port or one range is
  passed quickly. A server that goes silent is replaced the same way.
- **Web pages** (`web_relay.c`): the PS5 browser opens
  `http://127.0.0.1:8900/...`. The relay fetches roblox.com over TLS (BearSSL)
  through the tunnel and adds the signed-in cookies. It turns `roblox://` links
  (Join, profiles) into actions in the engine.

- **Game traffic** (`bionic_net.h`, `warp_gateway.c`): Roblox's game servers
  speak UDP. A game socket's first datagram to a public address opens a
  SOCKS5 UDP association with the gateway; every datagram then goes through
  the tunnel, and replies come back to the same socket, so the engine sees
  the real server. LAN traffic stays direct. Without this, networks that
  block Roblox's game servers end every join with error 279.

Limits: networks that detect and block WireGuard itself block every server.

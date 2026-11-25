# third_party

Vendored sources built into the title (tools/build-radv-title.sh)
for its built-in WARP gateway (radv/android_probe/warp):

- **lwip/** - lwIP 2.2.1 (STABLE-2_2_1_RELEASE), BSD licence (COPYING).
  Only the src/ files the title compiles are kept (IPv4 core, TCP, UDP, DNS, headers). Unmodified.
- **wireguard-lwip/** - smartalock/wireguard-lwip, BSD-3-Clause (LICENSE),
  with wireguard-lwip-ps5.patch applied: its two UDP sends go to the
  gateway's own host UDP socket (wg_uplink_sendto) instead of lwIP's UDP.

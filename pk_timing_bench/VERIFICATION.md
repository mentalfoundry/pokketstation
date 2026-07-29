# Real-hardware verification log

This log holds real, raw output from running `pk_timing_bench.mcs` on real PocketStation hardware. Use it to compare your own unit, or your own emulator's output, against a known-good run.

See [README.md](README.md) for what each screen means, how to build this app, and the full debugging history behind the current build. That history runs: crash, then LCD_MODE/VRAM/FLASH-width fixes, then the Timer0 wraparound fix, then both icons.

Each entry below is one full run. Each entry lists the build state tested, the exact hardware, and every screen's raw hex values, read directly off the device.

---

## 2026-07-28 — retail PocketStation unit

**Build tested:** the current committed `pk_timing_bench.mcs`, a GNU-toolchain build. It includes the Timer0-wraparound 16-bit mask fix, the `FLASH_CTRL` fast-rate emulator update, and both icons: the PocketStation on-device browse icon, and the standard PS1 memory-card icon.

| Screen | Test (top) | Control (bottom) |
|---|---|---|
| 1 — ARM vs Thumb sanity check | `0x00002BF2` | `0x000019A2` |
| 2 — `FLASH_CTRL` vs WRAM | `0x00002BF2` | `0x00002BF2` |
| 3 — real BIOS ARM helper vs WRAM copy | `0x0000802C` | `0x00006A33` |
| 4 — real BIOS Thumb helper vs WRAM copy | `0x0000B71B` | `0x0000B371` |

Screen 5 (raw Timer0 diagnostic, not deltas):

| single_before | single_after | full_before | full_after |
|---|---|---|---|
| `0x0000FFF0` | `0x0000FFEF` | `0x00006269` | `0x0000E23D` |

**Interpretation:**

- Screen 1: top is notably larger than bottom, about 1.7:1. Non-fetch loop overhead dilutes this from a pure 2:1 opcode-fetch signal. The sanity check passes; the measurement methodology is trustworthy.
- Screen 2: test equals control exactly. This confirms `FLASH_CTRL` gets WRAM's fast 1-cycle data-access rate, not the slow rate.
- Screens 3 and 4: both show clean, small values. Neither shows the `0xFFFFxxxx` wraparound artifact. This confirms the 16-bit Timer0 masking fix holds on real hardware, not just in the emulator.
- Screen 5 sanity check: `(0x6269 − 0xE23D) mod 65536 = 0x802C`. This matches screen 3's test value exactly. The raw counter data is internally consistent.
- Both icons were visually confirmed correct: the on-device browse-screen stopwatch icon, and the standard PS1 memory-card icon, as shown by a real console's own manager or PC tools.

This run had no crashes, no hangs, and no other anomalies.

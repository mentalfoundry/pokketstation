# Real-hardware verification log

Real, raw output from running `pk_timing_bench.mcs` on actual PocketStation hardware, kept here so anyone comparing their own unit (or their own emulator's output) against a known-good run has something concrete to check against. See [README.md](README.md) for what each screen means, how to build, and the full debugging history behind the current build (crash → LCD_MODE/VRAM/FLASH-width fixes → Timer0 wraparound fix → both icons).

Each entry below is one full run: build state tested, the exact hardware, and every screen's raw hex values as read directly off the device.

---

## 2026-07-28 — retail PocketStation unit

**Build tested:** the current committed `pk_timing_bench.mcs` (GNU-toolchain build; includes the Timer0-wraparound 16-bit mask fix, the `FLASH_CTRL` fast-rate emulator update, and both icons - the PocketStation on-device browse icon and the standard PS1 memory-card icon).

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

- Screen 1: top notably larger than bottom (~1.7:1, diluted from a pure 2:1 opcode-fetch signal by non-fetch loop overhead) — sanity check passes, the measurement methodology is trustworthy.
- Screen 2: test == control exactly — confirms `FLASH_CTRL` gets WRAM's fast 1-cycle data-access rate, not the slow rate.
- Screens 3/4: clean, small values — no `0xFFFFxxxx` wraparound artifact, confirming the 16-bit Timer0 masking fix holds on real hardware, not just in the emulator.
- Screen 5 sanity check: `(0x6269 − 0xE23D) mod 65536 = 0x802C`, matching screen 3's test value exactly — the raw counter data is internally consistent.
- Both icons (the on-device browse-screen stopwatch, and the standard PS1 memory-card icon as shown by a real console's own manager or PC tools) were visually confirmed correct.

No crashes, hangs, or other anomalies on this run.

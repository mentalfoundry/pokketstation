# PocketStation hardware notes

## CPU

ARM7TDMI (ARM/THUMB), Atmel-fabricated. Variable clock, max ~7.995 MHz; drops to a low-power mode around 33 kHz when idle.

## Memory map

| Range | Size | Purpose |
|---|---|---|
| `0x00000000` | 2KB | RAM. `0x000–0x1FF` kernel, `0x200–0x7FF` user. |
| `0x02000000` | virtual | FLASH1 — the currently selected/running app, mapped from FLASH2. |
| `0x04000000` | 16KB | BIOS ROM. Kernel around `0x1E00`, GUI around `0x2200`. |
| `0x06000000` | — | Flash control I/O (`F_CTRL`, `F_BANK_FLG`, `F_WAIT`). |
| `0x08000000` | 128KB | FLASH2 — physical flash, 15 blocks. |
| `0x0A000000` | — | `INT_LATCH`. |
| `0x0A000004` | — | `INT_INPUT` — buttons, bits 0–4 (Up/Right/Down/Left/Fire). |
| `0x0A800000`+ | — | Timers. Register layout (count/reload/ctrl) implemented in `core/src/timer.c` is a **best-effort approximation**, not sourced from primary documentation - revise once real register details surface. |
| `0x0B000000`+ | — | **Not in any sourced doc.** A real BIOS dump polls a register here (`LDR/TST #0x10/BEQ`) before touching flash control, apparently waiting for some hardware-ready signal. Modeled in `core/src/memory.c` as always reading back with bit 4 set, purely to get the observed boot sequence unstuck - semantics otherwise unknown. |
| `0x0C800000`+ | — | IR registers (protocol/send-receive mode at `+0`, beam on/off at `+4`). Exact bit-level IR timing is **unverified** —  |
| `0x0D000100` | 128B | LCD VRAM. |

BIOS ROM and FLASH2 are only readable in 16/32-bit units; RAM is freely 8/16/32-bit.

## LCD

32×32 pixels, 1 bit per pixel. VRAM is 128 bytes (32 rows × 32-bit words). Bit 0 of each word is the leftmost pixel; 0 = white, 1 = black. Hardware refreshes at roughly 32Hz once the CPU writes a row.

## Buttons

5 face buttons (Up, Right, Down, Left, Fire) plus a physical reset, read as bits 0–4 of `INT_INPUT` at `0x0A000004`.

## App file format: PSX Title Sector

PocketStation apps are **not** VMS (that's Dreamcast VMU) or GME (that's the DexDrive whole-card dump format) — don't reuse that terminology in code or docs. The real container is the **PSX Title Sector**, an 80-byte header followed by icon data, an optional snapshot, a function table, and the executable body:

- Offset `0x00–0x4F`: standard PS1 memory-card title/icon header fields.
- Offset `0x50–0x5F`: PocketStation-specific — icon frame counts, an `"MCX0"`/`"MCX1"` identifier at `0x52`, entry point + THUMB-mode flag (bit 0) at `0x5C`.
- If the identifier is `MCX1`: an `0x800`-byte snapshot follows (saved ARM register/RAM state), then the function table and executable body. `MCX0` skips the snapshot.

Source: the documented file-header/icons page.

## Confirmed against a real BIOS dump

`tools/inspect.c` (a small headless CLI, not part of the automated test suite) loads a real BIOS + a real app extracted from a memory card and free-runs the interpreter, logging mode transitions and stopping on any unrecognized opcode. Real dumps are never committed - see `testdata/` (gitignored). Findings so far, running two different real PocketStation apps for 2M+ instructions each with no unimplemented-opcode hits:

- Reset genuinely starts in Supervisor mode, ARM state, and the very first ~12 instructions visit FIQ then IRQ then back to SVC - textbook ARM startup code initializing each mode's banked SP in turn. Confirms the banked-register model works for real code, not just hand-written tests.
- The SWI vector is at `0x00000008` and does live in RAM (as expected - the BIOS populates the low-memory vector table itself; there's no separate hidden vector ROM).
- After boot, the kernel switches to User mode and runs the loaded app directly; the app periodically re-enters the kernel via `SWI` (a real syscall, not a crash) and returns - looks like a per-frame "wait for something, then continue" cycle, consistent with a vblank/LCD-refresh wait.
- The app does write to LCD VRAM (`psemu_framebuffer_dirty()` goes true during the run) - real, working end-to-end LCD I/O.
- Real register-offset addressing (`LDR/STR reg,[reg,reg]`) is what caught a genuine dispatch bug: the single-data-transfer class check incorrectly required the I-bit (bit 25, a data field selecting immediate-vs-register offset) to be 0, when the real class boundary is only the top 2 bits (`27:26 == "01"`). No hand-written test had exercised register-offset addressing through `arm_execute`'s dispatch before this.

## Licensing note

This project is GPLv3 ([LICENSE](../LICENSE)). BSD-3 code can be referenced/adapted into a GPLv3 project with attribution — that direction is compatible. Do not pull code from another open-source implementation (unlicensed) or lean on other closed-source docs beyond documentation-level facts (all closed-source, no code available anyway).

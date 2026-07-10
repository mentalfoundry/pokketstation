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
- After boot, the kernel switches to User mode and runs a periodic SVC↔USR↔`SWI` cycle - **but the USR-mode code runs from a fixed BIOS address (`0x04001AF8`), not from FLASH1 (`0x02000000+`) where a loaded app lives.** Confirmed by loading two different real apps and a "no app" run and getting byte-for-byte identical instruction traces - the kernel hadn't yet reached whatever logic reads the flash header and jumps to the app's own entry point, at least within a 5M-instruction window. Don't read early mode-switching/SWI activity as "the app is running" without checking the PC is actually inside FLASH1.
- The app-independent cycle does write to LCD VRAM (`psemu_framebuffer_dirty()` goes true during the run, always to an all-zero/blank pattern) - real, working end-to-end LCD I/O, but this is the kernel's own idle/menu behavior, not evidence the app has drawn anything.
- Real register-offset addressing (`LDR/STR reg,[reg,reg]`) is what caught a genuine dispatch bug: the single-data-transfer class check incorrectly required the I-bit (bit 25, a data field selecting immediate-vs-register offset) to be 0, when the real class boundary is only the top 2 bits (`27:26 == "01"`). No hand-written test had exercised register-offset addressing through `arm_execute`'s dispatch before this.
- **PocketStation apps are frequently bundled as multi-block linked memory-card files together with the game's regular PS1 save**, not as a standalone single 8KB block. A naive per-block scan for the `MCX0`/`MCX1` magic will find the right *first* block but silently miss the rest of the app if you don't follow the card's actual directory (block 0): each of the 15 data-block frames is 128 bytes with allocation state at offset `0x00` (`0x51`=first/solo, `0x52`=middle, `0x53`=last, `0xA0`=free), total filesize at `0x04`, and a next-block link at `0x08` (a **little-endian u16, 0-based among the 15 data blocks - add 1 to get the physical block number**, `0xFFFF` sentinel = end of chain). Extracting only the first block still parses fine (the header's in there) and still boots/runs without crashing, but any code/data the app references past its first 8KB reads back as zeroed flash - worth checking chain length before assuming a boot problem is a CPU bug.

### App-selection and dispatch, reverse-engineered from a real BIOS disassembly (Capstone)

The kernel's app-selection routine (BIOS file offset `~0x1A00-0x1B00`) does the following, in order - this is read directly off real disassembly, not inferred from docs:

1. Calls a helper (`0x1BC8`) that returns a **selected app-slot index** from RAM: `*(u16*)0x000000D0` if nonzero, else `*(u8*)0x000000CE`. If both are zero, no app is selected and the kernel takes an early-exit fallback (this is the "idle" path observed before any of this was understood).
2. **`FLASH2` (`0x08000000`) carries the exact same directory structure as a PS1 memory card** - not just the app's own block, the whole card layout is mirrored into flash. Given the selected slot index `N`, the kernel reads the directory frame at `FLASH2 + N*128` and walks the chain forward via the frame's next-block link (same `+1` convention as the memory card), accumulating a bitmask of every physical block belonging to that app (`mask |= 1 << block_index` per link hop).
3. Writes that bitmask to `FLASH_CTRL + 8` (`0x06000008`) and the value `2` to `FLASH_CTRL + 0` (`0x06000000`) - this is almost certainly programming a **hardware bank-select window**: FLASH1 (`0x02000000+`) is not a fixed alias of "block 0" or "flash offset 0", it's a virtual view assembled from *only the blocks in that bitmask*, presumably concatenated in chain order and remapped to start at `0x02000000`. **Not yet implemented in the emulator** - `core/src/memory.c` still aliases FLASH1 straight onto `flash->data[0]` regardless of any bank-select write, which is only correct by accident when the loaded image happens to start its data at flash offset 0.
4. Re-reads the slot index, then reads the entry point directly from `FLASH2 + slot*8192 + 0x5C` (the same entry-point field already documented above, just read from its real physical position this time instead of a relocated offset-0 copy).
5. Some further validation (calls into two small Thumb helpers reached via classic ARM↔Thumb `add lr,pc,#1 / bx lr` interworking trampolines - decode these as Thumb starting at `trampoline_addr+8`, not as ARM, or the rest of the disassembly is garbage) checks the block's own header a second time (`FLASH2 + slot*8192 + 2`, a 5-bit field there minus 15, times 128 - looks like it locates the function table based on icon-frame count) before deciding whether to jump fresh or resume from an `MCX1` snapshot.
6. Clears user RAM (`0x200-0x7FF`), switches `CPSR` to User mode, sets User SP to `0x800` (top of RAM), and does `BX` to the entry point read in step 4.

**Status: got a real app to execute for the first time** by forcing RAM `0x00D0` to a chosen slot and constructing a flash image with a real directory (`testdata/` tooling, not committed). Execution did reach real code inside FLASH1 (confirmed by PC landing inside the app's own block), then hit what decodes as a coprocessor instruction (`CDP`) a few hundred instructions in - almost certainly a sign of a remaining address-calculation mismatch (most likely the missing FLASH1 bank-select from step 3) rather than a genuinely missing CPU feature; ARM7TDMI-based PocketStation has no coprocessor, so landing on a `CDP` opcode means the PC is probably pointing at the wrong data, not that a real instruction is unimplemented. Next step here is implementing the FLASH_CTRL bank-select so FLASH1 correctly windows onto the bitmask-selected blocks instead of always aliasing block 0.

## Licensing note

This project is GPLv3 ([LICENSE](../LICENSE)). BSD-3 code can be referenced/adapted into a GPLv3 project with attribution — that direction is compatible. Do not pull code from another open-source implementation (unlicensed) or lean on other closed-source docs beyond documentation-level facts (all closed-source, no code available anyway).

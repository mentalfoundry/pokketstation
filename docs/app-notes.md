# PocketStation application notes

This file documents **writing PocketStation applications**: the container format, how the real BIOS discovers and launches an app, and how the app-selection screen renders an app's title and icon. [hardware-notes.md](hardware-notes.md) documents this emulator's own CPU/peripheral implementation instead. This file is meant to grow into reference material for a future PocketStation dev kit.

This file uses the same trust order as hardware-notes.md:

1. Direct testing on real hardware.
2. Tracing real BIOS and app execution with `tools/inspect.c` against real dumps. These dumps are not committed to the repo. See `testdata/` (gitignored).
3. Official register documentation.

Where the BIOS revision matters, facts confirmed against the real `110`-revision retail BIOS (`J110.bin`/`C110`) are called out as such.

**[`pk_timing_bench/`](../pk_timing_bench/)** (top-level directory) is a working example of everything in this file: a homebrew memory-timing benchmark app, its full build source, and a real-hardware-findings writeup. Building and debugging it on real retail hardware found several facts below: VRAM/FLASH access width, and Timer0 wraparound.

## App file format: PSX Title Sector

PocketStation apps are not GME. GME is the DexDrive whole-card dump format (`.gme`); it needs its container header stripped down to the raw 128KB before it loads here.

The real container is the **PSX Title Sector**: an 80-byte header, followed by icon data, an optional snapshot, a function table, and the executable body:

- Offset `0x00`-`0x4F`: standard PS1 memory-card title/icon header fields.
- Offset `0x50`-`0x5F`: PocketStation-specific fields — icon frame counts, an `"MCX0"`/`"MCX1"` identifier at `0x52`, and the entry point plus THUMB-mode flag (bit 0) at `0x5C`.
- If the identifier is `MCX1`: an `0x800`-byte snapshot follows (saved ARM register/RAM state), then the function table and executable body. If the identifier is `MCX0`, there is no snapshot.

A PocketStation app extracted as a single PS1 save is this same Title Sector body, prefixed with one real PS1 directory frame. Most PS1 save-management tools export these as `.mcs` files. The directory frame is `0x80` bytes: an in-use marker, a little-endian data size at `0x04`, and link/filename fields.

`psemu_load_mcs` strips that frame, validates the size against it, then loads the Title Sector the same way `psemu_load_app` does. Both frontends try `.pss` first and fall back to `.mcs`, regardless of the file's actual extension.

**PocketStation apps are frequently bundled as multi-block linked memory-card files**, together with the game's regular PS1 save, not as a standalone single 8KB block.

A naive per-block scan for the `MCX0`/`MCX1` magic finds the right first block, but silently misses the rest of the app. To find the whole app, follow the card's actual directory (block 0). Each of the 15 data-block frames is 128 bytes, with:

- Allocation state at offset `0x00`: `0x51` = first/solo, `0x52` = middle, `0x53` = last, `0xA0` = free.
- Total file size at `0x04`.
- A next-block link at `0x08`: a little-endian `u16`, 0-based among the 15 data blocks. Add 1 to get the physical block number. `0xFFFF` marks the end of the chain.

## App-selection and dispatch

Reverse-engineered directly from real BIOS disassembly:

1. A helper reads the selected app-slot index from RAM: `*(u16*)0x000000D0` if nonzero, else `*(u8*)0x000000CE`.
2. **`FLASH2` carries the exact same directory structure as a PS1 memory card.** The kernel reads the directory frame at `FLASH2 + slot*128`, then walks the chain via the frame's next-block link. This builds a bitmask of every physical block belonging to that app.
3. The kernel writes that bitmask to `F_BANK_FLG` and commits it (write `2` to `FLASH_CTRL+0`).
4. The kernel reads the entry point from `FLASH2 + slot*8192 + 0x5C` (the resolved physical block).
5. The kernel validates the block's header a second time, via two Thumb helper functions, reached through classic ARM-to-Thumb `add lr,pc,#1 / bx lr` interworking trampolines. This decides whether to jump fresh or resume from an `MCX1` snapshot.
6. The kernel clears user RAM (`0x200`-`0x7FF`), switches to User mode, sets User SP to `0x800`, and branches (`BX`) to the entry point.

**A single loaded app (`.pss`/`.mcs`) needs a synthesized directory to be reachable this way.** `flash_load_app` builds one: a card header, one directory frame per app block (chained starting at slot 1), and the app's own data starting at physical block 1.

**Directory frame offset `0x10` (byte 6 of the filename field) must be ASCII `'P'`.** Without it, the BIOS's menu-browsing code (separate from, and running before, the dispatch routine above) will not let a user navigate to or select the slot at all. Every other filename byte can be garbage.

This mirrors a real PS1 convention: a save bundling a PocketStation app replaces its product code's mandatory hyphen (example: `SLUS-00892`) with `P`. See `core/src/flash.c`'s `test_flash_load_app_synthesizes_directory` (in `tests/cpu_test.c`).

**The real, hardware-confirmed button sequence to reach a single loaded app from a cold boot** (see `docs/hardware-notes.md`'s CPU-related boot notes, and `tools/inspect.c`'s `button_sim==3`):

1. Wait for the HELLO/heart/beep power-on animation to finish.
2. Press **Down** once, then **Action**, to get past the date/time screen.
3. Press **Right** once, to move from the clock screen to the app.
4. Press **Action** to launch it.

Real button taps are brief, approximately 40ms. A deliberate, clean press-and-release reads more reliably than mashing the button. The real BIOS's input handling sometimes needs more than one clean attempt to register a press; this is not a sign the sequence is wrong.

## Browse-screen icon/graphic

**This entire section was reverse-engineered by directly tracing a real BIOS's execution against a real app, and validated by user testing.** It was not derived by decoding header fields and guessing at a format. Ground truth comes only from single-stepping the real BIOS and watching which memory it touched.

**The real icon lives immediately after the standard PS1 save icon's own frame data**: `FLASH1_BASE + 0x80 + std_frames*128`, once the app is banked in. `std_frames` is the 1/2/3 value encoded by the standard PS1 icon-flag byte at Title Sector `0x02`. This is not offset `0x60`, even though `0x60` is where the icon data would start under a standard PS1-save-icon reading of the format.

A prior version of this project's own homebrew test app placed its own code right after the standard icon. The BIOS silently read this code back as icon data: its first instruction's raw encoding showed up verbatim, where a real palette/pixel value should be. This is why the icon rendered as garbage or blank the first several times this was attempted.

For a single-frame indirect-layout app specifically (a 1-frame standard icon), this location is body offset `0x100`. The description below was originally anchored to that offset, but it is not a fixed offset in general; see below.

This layout is confirmed via live tracing. BIOS addresses below are for the `110`-revision retail BIOS (`J110.bin`). This layout is cross-checked against three further real apps: a single-frame icon, a slow 2-frame-toggle icon, and a fast 3-frame animated icon (see `testdata/`, gitignored):

- **Two distinct layouts exist.** Which one an app uses is not yet tied to a confirmed selector bit. This project infers the layout from the PocketStation-specific header byte at Title Sector `0x50`: this byte is `0` for both indirect-layout apps seen so far, and matches the frame count for the one inline-layout app seen so far. Treat this correlation as a hypothesis, based on static cross-file analysis, not BIOS-traced, not a confirmed selector.
  - **Indirect layout (2 of the 3 apps checked here):** an 8-byte header sits right after the standard icon. `word @ +0x00` is a flags/count word. `word @ +0x04` is an absolute FLASH1 pointer to the actual bitmap data, which can sit anywhere in FLASH1. One real app points to a location near the very end of its own body, nowhere near the icon header, confirming that the BIOS's pointer recovery (`(pointer - FLASH1_BASE) / 4`) is computed and relocatable, not just theoretically so.
  - **Inline layout (one app checked here, a 3-frame animated icon):** no header, no pointer. The bitmap frames sit directly and contiguously, right where the standard icon's data ends.
- **The flags word's bit meaning is partly decoded**, from comparing a 1-frame, a 2-frame, and a third 1-frame example. The low 16 bits give the on-device icon frame count: `1` for a static icon, `2` for the slow-toggling one. The high 16 bits correlate with animation speed: `1` for the 1-frame app's static icon, `0x30` for the slow ~1-2s-toggle icon. This may be a per-frame delay, where a larger value means slower animation, but this is circumstantial: only 3 data points, with no BIOS/timer trace confirming the unit. Not confirmed.
- **At the bitmap location:** one 128-byte, 32×32, 1bpp bitmap per frame, stored back-to-back. The frame count is given by the flags word for the indirect layout, or by the standard icon's own frame count for the inline layout. This packing is byte-for-byte identical to LCD VRAM's own packing: 32 rows of 4 bytes each, bit 0 = leftmost pixel of each byte's 8-pixel span, 1 = black/lit. There is no palette and no 4-bit indexing, despite the byte count (128) also matching a standard PS1 4bpp 16×16 icon bitmap by coincidence. That coincidence is what made the wrong model look plausible for as long as it did.
- **For a single-frame indirect-layout icon specifically**, the 128 bytes immediately following its one bitmap hold real, nonzero data in every such app inspected. This data is still unidentified. A plausible guess is a function table, since the Title Sector format documents a "function table" following icon data, but this is unconfirmed. This emulator's icon leaves these bytes untouched, copied verbatim from a real reference app, rather than guessed at. This does not apply to multi-frame icons, where that same space holds the next animation frame(s) instead.

**The standard PS1 title-text field (Title Sector offset `0x04`-`0x4F`, meant to hold 2-byte Shift-JIS text) does not appear to be used by the PocketStation's own on-device browse screen at all.** A correctly SJIS-encoded custom title in that field never rendered on the browse screen, in any configuration tested. This was verified byte-correct by decoding a real app's own title the same way, and getting a real, human-readable string back.

Replacing only the 32×32 bitmap immediately after the standard icon, leaving the `0x04` title field untouched, was enough on its own to produce a fully correct-looking custom result. Current working theory: the PocketStation's standalone LCD browse UI reads only the on-device-icon bitmap(s), where real apps bake their own logo or text in as pixels. The standard title field is likely for a PS1-console-side memory-card browser instead; this project has not investigated that browser.

### How to build a custom icon

1. Draw a 32×32, 1-bit image per frame. Any tool that exports a monochrome bitmap works, or generate it directly. See [`pk_timing_bench/`](../pk_timing_bench/)'s stopwatch icon for a worked example: it procedurally draws shapes into a 32×32 array, then packs 8 pixels per byte, bit 0 = leftmost.
2. Place the 128-byte-per-frame packed bitmap(s) either right after the standard icon's data (inline layout), or wherever a flags-plus-pointer header points (indirect layout, following a real app's own values as a template).
3. If using the indirect layout: set the flags word's low 16 bits to the frame count, and the pointer word to `FLASH1_BASE` plus wherever you placed the bitmap data. The high 16 bits' exact effect on animation timing is not confirmed; copy a real app's value for the animation speed you want, until this is BIOS-traced.
4. For a single, static (1-frame) icon: leave the 128 bytes after the bitmap as copied from a real reference app, rather than zeroing them, until this region's real purpose is confirmed.

## Memory access width (VRAM and FLASH1): word-only, confirmed the hard way

**Both LCD VRAM (`0x0D000100`) and FLASH1/app code+data (`0x02000000`) reject byte-granularity access on real hardware.** This emulator's own model allows byte-granularity access to both without complaint. This project found the difference via `pk_timing_bench` (its own homebrew timing-benchmark app), which worked perfectly in the emulator and failed in two confirmed ways on a real retail unit:

- **A blind `LDR`-value overwrite of `LCD_MODE`** (example: `mov r1,#0x40; str r1,[r0]`) instantly blanked the display and hung the device, on first real-hardware boot. `LCD_MODE`'s true pre-app/POR value is undocumented. This emulator's own default happens to already equal the value being written, so the bug was invisible in every emulator test. **Always read-modify-write `LCD_MODE`**: read the current value, OR or AND in only the bit(s) you want to change, then write it back. Never assume you know its full starting value.
- **A byte-wide (`LDRB`/`STRB`) read-modify-write into VRAM**, to set a single pixel, produced an entire lit horizontal row (a whole 32-bit row) instead of one bit. This happened on real hardware only. **Always access VRAM as full 32-bit words.** Each row is already exactly one word (4 bytes), so a per-pixel set or clear is `ldr` / modify one bit / `str` on the containing row-word, never a byte-wide operation.
- **A byte-wide (`LDRB`) read from FLASH1**, reading a font table one byte at a time, produced scrambled data. This happened on real hardware only. This restriction is already documented (see `hardware-notes.md`'s "Memory access timing": *"FLASH and BIOS ROM seem to be allowed to be read only in 16bit and 32bit units"*), but it is easy to violate by accident, since the emulator does not enforce it. **Always read FLASH1 in 16-bit or 32-bit units**: word-align the address down, use `ldr`/`ldrh`, then shift and mask in a register to extract a sub-word value if you need one.

None of these three bugs reproduce in this emulator as it stands. `core/src/lcd.c`'s `lcd_write8`/`lcd_mode_write8`, and `core/src/memory.c`'s FLASH1 read path, are all unconditionally byte-granular, with no width restriction modeled. Do not trust emulator-only testing to catch this class of bug. If you are writing real homebrew, audit every VRAM/FLASH access for accidental byte-wide instructions before testing on real hardware.

## Timer0's COUNT register wraps at 0x10000, not 0x100000000

A fourth real-hardware-only issue turned up in the same timing-benchmark app, this time in application logic rather than a bus-width mismatch. A measurement loop read Timer0's `count` register once before a long operation and once after, then computed elapsed ticks via a plain 32-bit subtraction (`before - after`). This assumed a full 32-bit free-running down-counter.

On real hardware, a long-enough loop produced an `after` reading numerically larger than `before`. This is the classic sign of a counter wrapping past zero and reloading mid-measurement. The naive 32-bit subtraction turned this into a large, wrong `0xFFFFxxxx` "negative" result, instead of the small, correct positive tick count.

Diagnosis: dumping raw before/after `count` snapshots directly, instead of the computed delta, on real hardware. Every raw reading came back with its upper 16 bits at zero. Subtracting the two raw values modulo 65536 recovered the correct, expected result exactly. This is strong evidence that Timer0's `count` is effectively a 16-bit register on real hardware (see `hardware-notes.md`'s "Timers" section).

**If you time anything longer than a trivial number of instructions with a raw timer register, mask your computed delta to 16 bits.** `AND #0xFFFF` does not encode as an ARM immediate; use `LSL #16` then `LSR #16` instead. Thumb needs the same shift pair, since it has no immediate `AND` either. This mask is a safe no-op when no wrap occurred, and the fix when one did.

### Open questions for future dev-kit work

- **The flags word's high 16 bits** (tentatively, a per-frame animation delay): only 3 real data points so far (values `1` and `0x30`), correlating loosely with a static icon vs. a slow ~1-2s toggle. No BIOS/timer trace confirms the actual unit or formula.
- **Whether the Title Sector `0x50` header byte really governs the indirect-vs-inline icon layout choice**, or whether the correlation seen so far is a coincidence of a small sample (2 indirect apps with `0x50=0`, 1 inline app with `0x50` matching its frame count). Needs more real apps, ideally with a BIOS trace of whatever code branches on this.
- **What the data immediately following a single-frame indirect-layout icon's bitmap is.** Real, nonzero data is present there in every such app checked, but its structure is not reverse-engineered. A plausible guess is function-table entries.
- **The wider GUI/menu subsystem** traced while chasing the icon: a per-character glyph blitter at BIOS `0x0400383A` (indexed by `char_code - 0x30` into a font table), a generic scroll/animation blit that reads a RAM scratch buffer at `0x2D0` into VRAM, and state structs at RAM `0x204`/`0x3D0`/`0x428`. This subsystem is real and substantial, but only the one path relevant to the icon bitmap (the copy loop at BIOS `0x04002f8c`-`0x04002f9a`) is fully traced. The rest is unexplored: for example, exactly how and when the menu decides to scroll, and whether the glyph blitter is ever used for anything app-controllable.

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

## How an app reaches the PS1 save on the same card

**Many PocketStation apps are useless on their own: they exist to exchange data with the console game's own PS1 save, sitting in a different block of the same memory card.** Yu-Gi-Oh Forbidden Memories is one. This section documents the mechanism, reverse-engineered from that app's Japanese release (`testdata/YGO_jap.mcr`, gitignored); the English release uses byte-identical code at the same offsets.

The mechanism rests entirely on the **`FLASH1`/`FLASH2` split** already described under "App-selection and dispatch":

- **`FLASH1` (`0x02000000`) is a banked window holding only the app's own blocks.** The kernel builds `F_BANK_FLG` from the app's directory chain, so an app cannot see anything outside itself here. Body offset 0 is `0x02000000`.
- **`FLASH2` (`0x08000000`) is the whole physical card, unwindowed.** Every block is visible, including the card's own directory and every other file's data.

`FLASH2` is the only route between the two files, and apps use it directly. There is no BIOS SWI for "find the game's save"; the app walks the card's directory itself, exactly the way the kernel does when dispatching an app.

In Yu-Gi-Oh's app the routine is at `0x0200225E`-`0x020022A6` (Thumb), and it works like this:

1. Write a `0x55555555` "not found" sentinel to RAM `0x6A4` and `0x6A8`.
2. Walk directory frames 1 through 15, at `FLASH2 + 0x80*n` - the same 128-byte frames a PS1 memory card already uses.
3. Skip any frame whose allocation state (offset `0x00`) is not `0x51`, that is, anything that is not the first block of a file.
4. Compare the frame's **filename field at offset `0x0A`**, a halfword at a time, against a null-terminated ASCII key compiled into the app. In this app the key sits at `0x02002354` and reads `"BASLUS-01411-YUGIOH"` - the exact filename of the game's PS1 save in the card directory.
5. On a match, compute the save's data address as **`0x08000200 + block_index * 0x2000`**. The `0x200` skips the PS1 save's own title/icon header (a `"SC"` title frame plus up to three 128-byte icon frames), landing on the game's real save data.
6. Store that pointer at RAM `0x69C`, and cache the word at save data `+0x334` into RAM `0x6A0`.

Two details are worth calling out for anyone writing an app:

- **The key is the *filename*, not the product code.** Offset `0x0A` is the start of the directory frame's filename field, which is why this lines up with the `'P'`-at-offset-`0x10` rule above: `0x0A + 6 = 0x10`, so that rule is "byte 6 of the filename field" in both descriptions.
- **The pairing is hardcoded, and region pairing can be surprising.** The Japanese-titled app (`BISLPMP86398-YUGIOH` in the directory) searches for the *US* save `BASLUS-01411-YUGIOH`, and both the Japanese and English cards in `testdata/` carry that same US save. An app looking for a save that is not on the card simply finds nothing and keeps its sentinel.

### The write side: reads are memory-mapped, writes go through the kernel

**An app can read the PS1 save directly through `FLASH2`, but it cannot write it that way.** Writes go through **kernel SWI `0x10`, one 128-byte frame at a time**. This asymmetry is the single most important fact in this section, and it is why the read path above is a handful of `ldr`s while the write path is a syscall with a retry loop.

Yu-Gi-Oh's write routine is at `0x020028FA`:

1. Call a gate at `0x02000C84` and bail out entirely if it returns carry set.
2. Load the resolved save pointer from RAM `0x69C` and convert it to a **frame number** with `(pointer >> 7)` - 128-byte frames - then add the caller's starting frame offset.
3. Per frame: up to **10 attempts** at `SWI 0x10` with the frame number in `r0` and a 128-byte source buffer in `r1`, retrying while it returns nonzero.
4. On success, advance one frame and 128 source bytes, and repeat for the caller's frame count.

The J110 kernel handler is at `0x0400126C`, and every step of it has to work or the write fails silently:

1. `F_WAIT2` (`FLASH_CTRL+0x10`) `= 0x21`, enabling flash writes.
2. The three-step unlock at `0x04001228`: `F_KEY2`(`FLASH2+0x55AA`)`=0xFFAA`, `F_KEY1`(`FLASH2+0x2A54`)`=0xFF55`, `F_KEY2`=`0xFFA0`. These are the same key offsets and order `core/src/flash.c` already models in `unlock_step`.
3. Copy `0x40` halfwords (128 bytes) to **`FLASH2 + frame*128`**.
4. The teardown at `0x04001250`: a fixed delay, then poll `F_WAIT2` until **bit 2 reads back set**, then clear `F_WAIT2`. This is exactly the busy-wait `flash.h` documents having fixed; an unmapped read returning 0 would spin here forever.
5. **Verify**: compare 32 words of destination against source, returning `0` for success and `1` for mismatch - which is what drives the app's retry loop.

This whole path is modelled correctly, and `test_flash_frame_write_lands_in_a_ps1_save_block` (in `tests/cpu_test.c`) replays the sequence directly - no BIOS or app image needed, so it runs in CI - and asserts the frame lands where the handler's own verify would look for it.

### Nothing commits to flash during an IR transfer

**No app this project can drive attempts a single flash write during an IR transfer** - not Yu-Gi-Oh, and not Chocobo World even on a fully verified bidirectional exchange. This is measured at the level of *attempted* writes, not inferred from a diff, which matters: a diff reports nothing whether the app never wrote, wrote a value identical to what was stored, or had its write discarded by some layer in between. `tools/ir_probe.c`'s `flashwatch` mode uses `psemu_bus_write_trace_cb` (`core/src/memory.c`) to separate those cases, and reports zero attempts to both FLASH1 and FLASH2.

Two false leads are worth recording so they are not followed again:

- **Diffing the loaded file against flash is not a write check.** `psemu_load_mcs` synthesizes a 16-frame directory and relocates the save's data to block 1, and `psemu_load_state` then overwrites flash again. Both happen before the first instruction. A file-vs-flash diff reported thousands of "written" bytes across every block for a `.mcs`-loaded app that had in fact written nothing. The baseline has to be flash as it stood once the run was set up.
- **The only FLASH_CTRL traffic during a transfer is not a flash write.** Both apps generate writes to `F_WAIT1` (`+0x0C`) and `F_WAIT2` (`+0x10`) from BIOS `0x040017C6`/`0x040017C8`. That routine (entry `0x040017B6`, distinct from `FlashReadSerial` at `0x040017A5`) sets flash waitstates and then programs `CLK_MODE` and polls for the change to take effect: it is the **clock-speed change routine**, and it fires because both apps switch CLK_MODE for the duration of a transfer (measured: 507904Hz to 1998848Hz). `F_WAIT2` is also what a real flash-write routine polls, so this region is worth watching - but these particular writes are not a commit.

So the received data reaches RAM and the screen, and the commit is gated on something none of these scenarios reach. Button scripts covering repeated confirmation presses, long `Action` holds, directional navigation, and 2500 frames (~78s emulated) of idle runtime all produced zero write attempts.

**The two apps are not equivalent here, and the difference is the point.** Chocobo World's transfers move its own app state, which lives in its own blocks. Yu-Gi-Oh is the case that genuinely writes the console game's PS1 save, adding traded cards to it - so it is the app to drive when testing this path, and `SWI 0x10` reaching `FLASH2` block 1 is the signal to watch for.

What is not yet located is the in-app trigger: which screen or confirmation makes `0x020028FA`'s gate at `0x02000C84` return carry clear and let the write proceed. That is a question about reaching the right app state, not about the emulator - the kernel path underneath it is modelled and tested. `tools/ir_probe.c`'s `flashwatch` mode reports `SWI 0x10` traffic the moment any scenario does reach it.

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

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
| `0x0A000000` | 0x14 | Interrupt controller: hold(+0x0, R), status(+0x4, R), enable(+0x8, W, ORs in), mask(+0xC, W, ANDs matching bits out of enable), acknowledge(+0x10, W, clears matching hold+status bits). See "Interrupt controller, timers, and RTC" below. |
| `0x0A800000`+ | 0x30 | 3 timers, 0x10 bytes apart: period(+0x0), count(+0x4), control(+0x8, bits0-1 = clock divisor, bit2 = enable). See below. |
| `0x0B000000`+ | — | `CLK_MODE` - CPU/timer clock speed control (confirmed against real hardware behavior: bits 0-3 index a `CPU_FREQ` table, readback ORs in a "steady" bit `0x10`). A real BIOS dump polls this (`LDR/TST #0x10/BEQ`) before touching flash control, waiting for the clock to stabilize after a speed change. Modeled in `core/src/memory.c` as always reading back with bit 4 set (`SetCpuSpeed` reprogramming isn't modeled, so it's always "stable"). |
| `0x0B800000` | 0x10 | RTC: mode(+0x0), control/adjust(+0x4), time(+0x8, R), date(+0xC, R). See below. |
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
3. Writes that bitmask to `FLASH_CTRL + 8` (`0x06000008`) and the value `2` to `FLASH_CTRL + 0` (`0x06000000`) - programming a **hardware bank-select window**: FLASH1 (`0x02000000+`) is not a fixed alias of "block 0", it's a window offset onto FLASH2 by the *lowest-numbered block* in that bitmask. **Implemented** in `core/src/flash.c` (`flash_ctrl_write8`/`flash1_read8`/`flash1_write8`) - commits on any write to the command register, since the real BIOS always writes the mask first. FLASH2 itself stays a plain, unwindowed physical view (matches the BIOS's own absolute `FLASH2_BASE + block*8192 + ...` addressing when reading the directory/entry point). Not yet confirmed: whether the window is a simple linear offset (what's implemented) or actually compacts non-contiguous blocks in chain order - untested since every real chain observed so far happens to be physically contiguous.
4. Re-reads the slot index, then reads the entry point directly from `FLASH2 + slot*8192 + 0x5C` (the same entry-point field already documented above, just read from its real physical position this time instead of a relocated offset-0 copy).
5. Some further validation (calls into two small Thumb helpers reached via classic ARM↔Thumb `add lr,pc,#1 / bx lr` interworking trampolines - decode these as Thumb starting at `trampoline_addr+8`, not as ARM, or the rest of the disassembly is garbage) checks the block's own header a second time (`FLASH2 + slot*8192 + 2`, a 5-bit field there minus 15, times 128 - looks like it locates the function table based on icon-frame count) before deciding whether to jump fresh or resume from an `MCX1` snapshot.
6. Clears user RAM (`0x200-0x7FF`), switches `CPSR` to User mode, sets User SP to `0x800` (top of RAM), and does `BX` to the entry point read in step 4.

### The real SWI (syscall) handler, disassembled

The vector table at RAM `0x00000000-0x0000001C` is 7 identical `LDR PC,[PC,#0x18]` entries (the standard ARM "vector → literal pool → real handler" idiom) plus one `MOV R0,R0` filler at `0x14`; the actual handler addresses live in the literal pool right after, at `0x20-0x3C` (reset, undef, SWI, prefetch-abort, data-abort, reserved, IRQ, FIQ, in that order). For this BIOS dump, SWI's literal (`0x28`) resolves to `0x04001528`. That function:

1. Saves `r1-r12,lr`, reads `SPSR` into a scratch register, and tests its `T` bit to compute the *original* SWI instruction's own address in `lr` (`-2` if the caller was in Thumb, `-4` if ARM - both correctly undo the `pc+2`/`pc+4` return-address convention to point back at the `SWI` itself).
2. Re-reads that instruction's low byte as a **syscall number**, looks up a function pointer from a dispatch table whose base address is stored at RAM `0x000000E0` (`table[syscall_number]`), and calls it via an interworking `BX` (so a Thumb-tagged target correctly switches the CPU into Thumb mode for the call).
3. Returns via **`LDM SP!,{r1-r12,lr,pc}^`** - not `MOVS/SUBS PC,LR`. The `^` suffix on an `LDM` that includes `PC` is architecturally identical to the data-processing `Rd=15,S=1` idiom (restore the whole `CPSR` - mode, `I`/`F`/`T`, flags - from the current mode's `SPSR`, not just load `PC`) but is a completely separate encoding path. **This was a real, confirmed emulator bug**: `exec_block_transfer` decoded but silently ignored the block-transfer S-bit entirely. Fixed - see the CPU test `test_arm_ldm_exception_return` for the regression coverage, and the "App-selection" section above for how this was found (a real Thumb `SWI` issued by the app itself was returning in the wrong mode, landing on data that only decoded sensibly as Thumb while the CPU was stuck in ARM mode).

**Status**: with the app-slot selector, flash bank-select, and this `LDM^` fix all in place, a real BIOS + a real app now runs cleanly for 20M+ instructions (including simulated button presses) with zero unimplemented-opcode hits, executes multiple genuine syscalls through the real dispatch mechanism above, and correctly threads ARM/Thumb mode across all of them. The LCD stays blank throughout this window - not yet known whether that's because the app hasn't reached its "draw something" logic yet, needs a trigger not yet simulated (real button timing, IR, something else), or there's a further bug still to find. That's the next open question, not a settled failure.

### A real, confirmed CPU-architecture bug: Thumb `BL` must tag `LR`'s bit 0

Tracing the halt trap above to its root cause: the app's own first syscall returns correctly via the fixed `LDM^`, continues into what looks like ordinary app code, then hits `BX LR` after a plain Thumb `BL` call - and the CPU incorrectly switches to ARM mode, because `LR` held a plain even return address. **This is wrong even on pure ARMv4T** (verified directly against real ARMv4T architecture behavior, which computes the return address as `(R15 + 2) | 1`) - Thumb `BL` has always tagged the return address's bit 0, precisely so a later `BX LR` correctly stays in Thumb. This is not an ARMv5 `BLX`-only feature, contrary to what I'd assumed earlier in this investigation. Fixed in `exec_long_branch_link` - see `test_thumb_bl_bx_lr_stays_thumb` for the regression case, modeled directly on the real failure.

Fixing this (plus two related memory-map bugs it exposed - `INT_INPUT`/`INT_LATCH` were only handled as single exact-address byte matches, silently losing bit 9 on a 32-bit read; and a second undocumented "ready" register at `0x0B800000`, distinct from the existing `0x0B000000` one) got the same real app running 20M+ instructions with no halt trap at all, versus hitting it within ~5000 instructions before.

### Interrupt controller, timers, and RTC

Implemented in `core/src/intc.c`, `core/src/timer.c`, `core/src/rtc.c`.
   Verified register-by-register against real hardware behavior (using
   official register names like `RTCControl`/`RTCInc`/`RTCClock` alongside
   the informal `RTC_MODE` names). That verification surfaced real,
   confirmed discrepancies between this codebase's original implementation
   and real hardware - the point of doing it at all, since that implementation hadn't been independently validated against real hardware and was informally known to be incomplete in places.

**Interrupt controller** (`0x0A000000`): `hold`, `status`, `enable`, `mask` registers; `intc_irq_asserted`/`intc_fiq_asserted` compute `hold & enable & INT_IRQ_MASK`/`INT_FIQ_MASK` on demand. Register layout, byte-write accumulation, and the four operations (hold/status read-only, enable ORs in, mask ANDs out of enable + latches, acknowledge clears both) match the real register semantics exactly (`INT_LATCH`/`INT_INPUT`/`INT_MASK_SET`/`INT_MASK_CLR`/`INT_ACK` map onto the same four registers).

**A second real, confirmed bug, found by disassembling the real BIOS directly** (going past what any prior documentation covered): an earlier, uncorrected version of this codebase's interrupt-routing function routed `INT_STATUS_MASK` sources (buttons, RTC) into `status` *only*, never `hold` - matching the assumption that `hold` drives real interrupts while `status` is a separate polling-only view. Tracing why the app-dispatch routine (below) was never reached led to disassembling the real IRQ vector handler at `0x04001414` and the periodic system-tick callback it invokes (installed at `0x040036EC` via `SetCallbacks`, RAM `0xFC`) - both explicitly test `hold & enable & INT_RTC` and `hold & INT_BTN_ACTION` respectively, and both branches land on real, working handlers (an RTC acknowledge + day-rollover bookkeeping routine at `0x04000672`, confirmed by tracing it; a button/wake handler at `0x04003784`) that could never execute under that earlier status-only routing, since these bits would never appear in `hold`. Real hardware evidently latches these sources into *both* registers. Fixed `intc_set_line` to do the same; confirmed via instrumentation that both handlers now actually execute in response to simulated RTC ticks and button presses (previously: zero executions, ever). See `test_intc_status_sources_also_latch_hold`.

**Timers** (`0x0A800000`+, 3 channels): control register bits 0-1 select a clock divisor - `0` or `3` = `/2`, `1` = `/32`, `2` = `/512` (the officially documented divider table matches exactly). **A real, confirmed bug**: an earlier version of `timer_tick` in this codebase decremented `count` by raw elapsed cycles directly, ignoring the divisor entirely - any timer configured for `/32` or `/512` would have fired 16x or 256x too often relative to real hardware. Fixed by giving each timer its own cycle accumulator and only decrementing `count` once `divisor` cycles have accumulated (see `test_timer_clock_divisor`). Measured effect on a real BIOS+app trace: mode/IRQ transitions per instruction dropped by roughly 25x after the fix, and LCD-framebuffer writes per instruction went up by over 200x - strong evidence the CPU was previously being starved by a runaway IRQ rate that doesn't reflect real hardware timing. Deliberately *not* modeled: having timer-count reads themselves decrement the count as a side effect of *reading* it - count reads are documented as plain "current value (decrementing)" with no read-side-effect language, so that would be an unconfirmed behavior, and copying it would be adding an unconfirmed bug, not fixing one.

**RTC** (`0x0B800000`): `mode` bit 0 (official `PRGSEL`) is Run (0, ticks at 1Hz, auto-advances the clock every tick) vs Program/pause (1, ticks at ~4096Hz, does *not* auto-advance - used only to let a manual adjust-write step one field at a time without the clock moving underneath it); bits 1-3 (`CNTSEL`) select which BCD field a `control`/`RTC_ADJUST` write adjusts. Two more confirmed, fixed discrepancies:

- **Missing auto-advance.** an earlier, unconfirmed source's own `rtc_tick` callback auto-increments `time` (seconds → minutes → hours → day-of-week, *not* cascading into `date` on a day rollover - a gap in an earlier, unconfirmed source itself that no independent source explains, so it's inherited rather than invented) whenever `mode & 1 == 0`. An earlier version of this codebase's `rtc_tick` only toggled the interrupt line and never advanced the actual time value - real BIOS code reading `RTC_TIME` would see a clock that never moves on its own. Fixed by implementing this exact cascade arithmetic as `rtc_advance_second`, gated the same way.
- **Flat tick rate.** an earlier, unconfirmed source's RTC timer always fires at 1Hz, even in Program/pause mode - the `~4096Hz` behavior documented for paused mode isn't implemented in an earlier, unconfirmed source at all (grep confirms only one `attotime::from_hz(1)` in its entire RTC path). Fixed by giving this codebase two tick-cycle constants, `RTC_TICK_CYCLES_RUN` and `RTC_TICK_CYCLES_PAUSED` (exactly 4096x faster), selected by `mode & 1` each tick.
- **Reset defaults.** An earlier version of this codebase's `rtc_init` hardcoded `time = 0x01000000, date = 0x19990101` (1999-01-01) - this matches the BIOS's well-known "resets itself to Jan 1999" *software* quirk (see below), not the actual silicon power-on-reset value. the real POR values are as `RTCClock = 0x04000000` (day-of-week BCD 4, 00:00:00) and `RTCCalendar = 0x00980101` (1998-01-01) - also confirming that `RTC_DATE` bits 24-31 are an unused/unknown field, *not* a "year-hi"/century byte as an earlier version of this codebase assumed (the real century lives in battery-backed kernel RAM, surfaced only via the `GetBcdDate` SWI). Fixed `rtc_init` to use the officially documented POR values.

The BIOS's own "resets itself to Jan 1 1999" behavior (known as "The RTC Problem" - apparently Sony's software-side workaround for inaccurate clock hardware) is a *separate*, software-driven action taken via the normal `RTC_ADJUST` mechanism, not something to bake into `rtc_init`.

None of these fixes (including the hold-routing bug above) resolved the separate, still-open issue of the app-dispatch routine (`0x04001900-0x04001AFC`, see below) never being reached - confirmed by direct instrumentation before and after each one. They're independently-confirmed correctness fixes regardless.

### Open: the app-dispatch routine is never reached

Direct instrumentation (tracking every instruction executed in `0x04001900-0x04001AFC`, the full app-selection/dispatch function documented above) shows it is **never entered at all**, regardless of which real app's slot index is forced into RAM `0x00D0`, across runs up to 500M instructions - and a whole-BIOS scan for any direct `BL` or indirect (literal-pool + `BX`) reference to this function found none either, so its real caller hasn't been located yet. PC instead spends the entire run oscillating within a different BIOS region (`~0x04003xxx`), which disassembly confirms is the BIOS's own menu/event loop: a tight wait (`0x0400356C`) on a RAM flag, followed by dispatch on an action code read from RAM (cases 0/1/3/6 call different handlers, e.g. `0x4002cd0` - a splash/logo animation loop that exits once a separate "input occurred" check at `0x04001fdc` goes true).

Two hypotheses tested and ruled out so far:

- **Docking (`INT_IOP`, bit 11).** This is a real wake/launch trigger ("Wakeup usually done by IRQ-0 (Fire) and IRQ-11 (Docking)"), and it was never simulated at all before this investigation (`INT_IOP` was previously asserted nowhere in this codebase). Added a `dock` flag to `tools/inspect.c` that asserts it once mid-run; confirmed via instrumentation that the real docking IRQ handler (`0x04001cf4`) does execute, but the dispatch routine still isn't reached afterward - docking is likely a separate PS1-connectivity feature, not the standalone app-launch trigger.
- **The interrupt-controller hold-routing bug** (previous section). Fixing it made the button-press and RTC hold-bit handlers execute for the first time (confirmed via instrumentation - 197 button-handler entries and 1 RTC-handler entry in a 5M-instruction run, versus zero before), but still didn't unblock dispatch. The button handler in question (`0x04003784`) turned out to just clear a wake/timeout flag, not feed into the menu's action-dispatch value.

Next step: the menu loop's action-dispatch value is read from a fixed RAM offset (confirmed via instrumentation: `[r4+0x24]` relative to a base loaded early in the function) that never changes from 0 under simulated Fire-button presses - trace what code actually *writes* that location (and the splash-exit check at `0x04001fdc`, which compares two RAM counters rather than reading button state directly) to find the real input path, since it isn't the interrupt-callback mechanism traced so far.

## Licensing note

This project is GPLv3 ([LICENSE](../LICENSE)). BSD-3 code can be referenced/adapted into a GPLv3 project with attribution — that direction is compatible. Do not pull code from another open-source implementation (unlicensed) or lean on other closed-source docs beyond documentation-level facts (all closed-source, no code available anyway).

# PocketStation hardware notes

This file gives the hardware model of this emulator.

The facts here come from three sources, in this order of trust:

1. A direct test on real hardware.
2. A trace of real BIOS execution and real app execution, with `tools/inspect.c`, against real dumps. This repository does not contain those dumps. See `testdata/`, which .gitignore excludes.
3. The available register documentation.

Some facts are inferences, and some are unconfirmed. The section "Known open questions and unconfirmed behavior" at the end lists each one. Do not use an item in that section as a confirmed fact.

A trace of real BIOS execution and real app execution validates the CPU core of this emulator. Those traces used a real copy of the serial-carrying app, and a real homebrew ID-editor app. Together, the traces cover hundreds of millions of instructions. After the corrections below, those traces gave no unrecognized-opcode faults.

## CPU

The CPU is an ARM7TDMI (ARM and Thumb instruction sets), fabricated by Atmel.

At reset, the CPU starts in Supervisor mode, in the ARM state. The first approximately 12 instructions enter FIQ mode, then IRQ mode, and then return to Supervisor mode. Each mode entry initializes the banked stack pointer (SP) of that mode. A real BIOS trace confirms this.

**Real ARM7TDMI behaviors that tests against real hardware and real apps confirm.** Each behavior below was a gap in the CPU core of this emulator. Real execution traces found each gap; no synthetic test found them. Each gap is now corrected:

- **A Thumb `BL` instruction sets bit 0 of the return address**: `(R15+2)|1`. This is true on ARMv4T. It is not a feature of the ARMv5 `BLX` instruction alone. A set bit 0 makes a later `BX LR` stay in the Thumb state. Corrected in `exec_long_branch_link`. See `test_thumb_bl_bx_lr_stays_thumb`.
- **An `LDM` instruction with the `^` suffix, where the register list contains `PC`**, for example `LDM SP!,{r1-r12,lr,pc}^`, which is the return sequence of the real SWI handler: this instruction restores the full CPSR from the SPSR of the current mode. It does not restore only `PC`. This encoding is different from `MOVS PC,LR` and `SUBS PC,LR`. Corrected in `exec_block_transfer`. See `test_arm_ldm_exception_return`.
- **A misaligned `LDRH` instruction, at an odd address,** reads the aligned halfword below the address, and then rotates the result right by 8 bits. Thus it exchanges the two bytes of the halfword before the value gets to the register. It does not only round the address down and discard the low bit.

  **A misaligned `LDRSH` instruction** does not get that rotation and sign extension. Real hardware instead does a sign-extended byte load (`LDRSB`) from the odd address.

  Corrected in `exec_halfword_transfer`. See `test_arm_ldrh_misaligned_quirks`.

  The font-glyph routine of a real homebrew ID editor found this behavior. That routine reads a byte-packed table one byte at a time, with `LDRH Rd,[Rn],#1`, which is a post-increment of 1 and not 2. It then masks the result to the low byte. That method operates only because of the rotation above.
- **FIQ has a higher priority than IRQ.** FIQ entry sets both `CPSR.F` and `CPSR.I`. IRQ entry, SWI entry, and abort entry set only `CPSR.I`. See "Interrupt controller" below.

The CPU clock speed is variable, and `CLK_MODE` controls it. See "CLK_MODE" below.

## Memory map

| Range | Size | Purpose |
|---|---|---|
| `0x00000000` | 2KB | RAM. `0x000–0x1FF` kernel, `0x200–0x7FF` user. |
| `0x02000000` | virtual | FLASH1 — the currently selected/running app, mapped from FLASH2. |
| `0x04000000` | 16KB | BIOS ROM. Kernel around `0x1E00`, GUI around `0x2200`. |
| `0x06000000` | — | Flash control I/O (`F_CTRL`, `F_BANK_FLG`, `F_WAIT`); `+0x300` carries `F_EXTRA` (see "Hardware ID (F_SN)" below). |
| `0x08000000` | 128KB | FLASH2 — physical flash, 15 blocks. |
| `0x0A000000` | 0x14 | Interrupt controller: hold(+0x0, R), status(+0x4, R), enable(+0x8, W, ORs in), mask(+0xC, W, ANDs matching bits out of enable), acknowledge(+0x10, W, clears matching hold+status bits). |
| `0x0A800000`+ | 0x30 | 3 timers, 0x10 bytes apart: period(+0x0), count(+0x4), control(+0x8, bits0-1 = clock divisor, bit2 = enable). |
| `0x0B000000`+ | 0x8 | `CLK_MODE`(+0x0) - CPU/timer clock speed control. `CLK control`(+0x4) bit0 - stop/standby, halts the CPU and the timers until a button wakes them. A real-hardware test confirms this. Both words read back as `CLK_MODE`, thus `+0x4` has no readable stop status (see "CLK control" below). |
| `0x0B800000` | 0x10 | RTC: mode(+0x0), control/adjust(+0x4), time(+0x8, R), date(+0xC, R). |
| `0x0C000000`+ | 0x20 | Communication port to a PS1: `COM_MODE`(+0x0), `COM_STAT1`(+0x4), `COM_DATA`(+0x8), `COM_CTRL1`(+0x10), `COM_STAT2`(+0x14), `COM_CTRL2`(+0x18). See "Communication port". |
| `0x0C800000`+ | 0x10 | IR: `IRDA_MODE`(+0x0, protocol/send-receive mode), `IRDA_DATA`(+0x4, beam on/off), `IRDA_MISC`(+0xC, unknown/reserved). See "IR / IR Link". |
| `0x0D000000` | 0x8 | `LCD_MODE`: bit6 `DISON` (display on/off), bit7 `ROT` (rotate 180°). |
| `0x0D000100` | 128B | LCD VRAM. |
| `0x0D800000` | 0x10 | IOP power control: IOP_CTRL(+0x0, unmodeled, no known effect), IOP_STOP/IOP_STAT(+0x4, W/R, sets bits), IOP_START(+0x8, W, clears bits), IOP_DATA(+0xC, unused by the real BIOS). Bit5 = Sound Enable. |
| `0x0D800010` | 0x10 | DAC: `ctrl`(+0x0, bit0 enable), `data`(+0x4, bits6-15 signed 10-bit `DACV`). |

BIOS ROM and FLASH2 permit only 16-bit reads and 32-bit reads. RAM permits 8-bit, 16-bit, and 32-bit access.

## LCD

The LCD is 32 by 32 pixels, at 1 bit for each pixel. VRAM is 128 bytes: 32 rows, with one 32-bit word for each row. Bit 0 of each word is the leftmost pixel. A clear bit (0) is white, and a set bit (1) is black. The hardware refreshes the display at approximately 32Hz after the CPU writes a row.

`LCD_MODE` (`0x0D000000`) is a register that is separate from VRAM.
- Bit 6 (`DISON`) controls whether the display shows content. While the bit is clear, the display is blank.
- Bit 7 (`ROT`) rotates the image 180 degrees: it reverses the order of the scanlines, and it reverses the bits of each scanline from left to right. Real hardware keeps this bit in agreement with the docking flag. Thus the screen is upright when a user holds the device, and also when the device is in its dock.

`psemu_get_framebuffer` returns this processed image. It does not return the raw VRAM.

The default value of `mode` has `DISON` set. This is the safe default of this emulator. The real power-on-reset (POR) value has no documentation. This default makes an app visible, even if that app never writes to `LCD_MODE`. See `test_lcd_mode_dison_and_rotate`.

**Confirmed on real hardware**, with `pk_timing_bench`, the homebrew app of this project: both VRAM and `LCD_MODE` refuse operations that the model of this emulator permits with no error.

**An `LCD_MODE` fault.** A first real-hardware build turned the display on with a direct write: `mov r1,#0x40; str`. That code writes `LCD_MODE` directly; it does not read, change, and write the value. The `LCD_MODE` default of this emulator is already equal to `0x40`, thus the direct write had no effect in each emulator test. On real hardware, the same write made the screen blank immediately and stopped the device. Recovery needed the physical reset button. The real value of `LCD_MODE` before dispatch, and at power-on reset, has no documentation. The current theory: the real BIOS leaves a different, unidentified bit set, and the real display output depends on that bit. The direct write cleared it. A change to a read, modify, and write sequence corrected the fault. That sequence reads the current value, and then ORs in only bit 6.

**A VRAM fault.** A byte-size (`LDRB` and `STRB`) read, modify, and write into VRAM gave one lit horizontal row on real hardware, in place of the one intended pixel. The VRAM model of this emulator permits byte addresses, and it gives no error. A change to a word-size (`LDR` and `STR`) read, modify, and write corrected the fault. The VRAM rows are already word-aligned, thus this change needs no division into bytes.

Neither fault occurs in this emulator: `lcd_mode_write8` and `lcd_write8` (`core/src/lcd.c`) permit byte access always, and they model no width restriction. See `docs/app-notes.md` for the homebrew view of both corrections.

## Buttons

There are 5 face buttons (Up, Right, Down, Left, and Fire), and one physical reset button. The face buttons read as bits 0 to 4 of `INT_INPUT`, at `0x0A000004`.

The `hold` bit of a button is a momentary edge pulse for each physical press. It is not a continuous level for the full time that a user holds the button. This behavior is different from the behavior of a level-triggered source, for example the IOP, the battery, or a timer. The `hold` bit of such a source stays set while its condition continues.

This difference is important for the system-tick callback of the BIOS. That callback is a chain with a fixed priority: the IOP, then the battery, then Timer1, then Action (the buttons), and then the RTC. The chain gets to the RTC redraw step only if no earlier bit is set in `hold`. If the Action bit stays asserted while a user holds a button, that bit permanently prevents the RTC processing for that time.

`status` still follows the live button level, for code that reads the register directly.

`intc_clear_hold_only` (`core/src/intc.c`) does this operation. `psemu_set_buttons` calls it when a button stays held and there is no new press edge. See `test_button_hold_pulses_not_sustained`.

The button input uses the interrupt (`hold`) path and the callback path. It does not use a direct read of `status`.

**[docs/app-notes.md](app-notes.md) gives the app container format (the PSX Title Sector), and the app-selection and dispatch routine of the real BIOS.** That file is for a PocketStation app developer, as reference material for a future development kit. This file gives only the implementation of the emulator.

## Flash memory

**FLASH2** (`0x08000000`, physical, 128KB) has 16 blocks of 8KB each. Block 0 holds a PS1-format memory-card directory: 16 frames of 128 bytes each. Frame 0 is the card header, and frames 1 to 15 give the properties of blocks 1 to 15.

**FLASH1** (`0x02000000`, virtual) is a banked window onto FLASH2, with 16 slots. Two `FLASH_CTRL` registers resolve this window during execution:
- `F_BANK_FLG` (`FLASH_CTRL+8`): a bitmask of the physical 8KB blocks that are enabled for the current app.
- `F_BANK_VAL` (`FLASH_CTRL+0x100` to `0x13C`, 16 words, with a reset value of 0): for each physical block, this table gives the virtual bank slot (0 to 15) of that block, as `table[physical]=virtual`. This is the opposite direction from a usual page table. Thus a conversion from virtual to physical needs a reverse search over the 16 entries (`flash_resolve_physical_bank`).

While `F_BANK_VAL` holds its reset value for each physical bank, the resolution uses a contiguous linear map. That map starts at the enabled physical block with the lowest number. Each observed app-dispatch trace of a real BIOS uses this condition: the real BIOS writes only `F_BANK_FLG`, and it never writes `F_BANK_VAL`.

See `test_flash_bank_val_remapping` for a non-contiguous, reordered mapping, and `test_flash_bank_select` for the fallback case.

`FLASH_CTRL` (`0x06000000`) also has these registers:
- `+0`: a command and commit trigger. A write of `2` commits a bank-select change. A real BIOS routine then waits at this same address, until bit 0 reads back as `1`. The commit operations of this emulator are immediate, thus each read of `+0` also sets bit 0.
- `+0x10` (`F_WAIT2`, the waitstates and the flash-write status): the flash-write routine of a real app reads this register, and waits for bit 2 to read back as set after a write completes. The writes of this emulator complete immediately, thus this bit always reports "not busy".

See `test_flash_ctrl_busy_wait_bits`.

**`F_KEY1` (`0x08002A54`) and `F_KEY2` (`0x080055AA`) are flash-unlock command addresses. They are not data storage.** Real flash hardware receives a write of the `FFAAh`, `FF55h`, and `FFA0h` sequence at these addresses as an unlock command, and it then arms the write-unlock state machine of the chip. It does not store the value; the byte at that physical address does not change. `flash_write8` and `flash1_write8` (`core/src/flash.c`) discard a write to either 16-bit key address. They do this in both the physical FLASH2 path and the virtual FLASH1 window. See `test_flash_key_addresses_are_not_data_storage`.

**App-selection and dispatch is documented in [docs/app-notes.md](app-notes.md).** See that file for the real BIOS's app-selection routine, and for how `flash_load_app` synthesizes a directory for a single loaded app.

## Register banking

The registers are banked for each mode, the same as on each ARM7TDMI: `r13`, `r14`, and `SPSR`, for each of FIQ, IRQ, SVC, ABT, and UND. User mode and System mode use one shared set.

**`r8` to `r12` are also banked, but only for FIQ.** That bank lets a FIQ handler use those five registers as scratch registers with no save operation. It is the reason that a "fast interrupt" is fast. Each mode that is not FIQ uses one shared copy. Thus a change from SVC to IRQ must not change them. Only a change across the FIQ boundary exchanges them.

This emulator banked only `r13` and `r14` for a long time. Thus a FIQ handler destroyed `r8` to `r12` of the interrupted code, and gave no error. **This was a real fault, and not a theoretical one.** One music app drives its audio from Timer2, which routes to FIQ (`INT_FIQ_MASK`). A measurement shows that its FIQ handler writes four of the five registers at almost each entry: in a boot-and-use run, 462 of 463 FIQs changed `r8`, `r9`, `r11`, and `r12`. With the earlier model, each of those writes changed a register of the interrupted code. See `test_fiq_banks_r8_to_r12`.

Real hardware also does *not* bank these registers for IRQ. Thus an IRQ handler must save them, and the handler of the real BIOS does this. A measurement gives 0 of 25120 IRQ entries with a change to those registers.

**An `LDM` or `STM` instruction with the `^` suffix, and with the PC absent from the register list,** transfers the User bank, and not the bank of the current mode. This is the method that a privileged handler uses to reach `r13` and `r14` of an interrupted app, with no mode change. `exec_block_transfer` ignored the S bit in that condition, and it moved the registers of the current mode. The real BIOS uses this form: `STMIA r0!,{r13,r14}^` at `0x04001944`, with the matching `LDM` at `0x04001B90`. But no observed app that this project can operate executes those instructions. Thus this was a latent gap, and not an active fault. See `test_ldm_stm_user_bank_transfer`. The condition with the S bit and the PC is the separate CPSR-restore method in "CPU" above.

## SWI (syscall) mechanism

The vector table, at RAM `0x00000000` to `0x0000001C`, has 7 identical `LDR PC,[PC,#0x18]` entries, and one filler entry. The real handler addresses come immediately after the table, in a literal pool, in this order: reset, undefined instruction, SWI, prefetch abort, data abort, reserved, IRQ, and FIQ. The SWI vector is at `0x08`.

The real SWI handler does these steps:

1. Save `r1` to `r12`, and `lr`.
2. Read the `T` bit of `SPSR`, to calculate the address of the original SWI instruction in `lr`. Subtract 2 for a Thumb instruction, and 4 for an ARM instruction.
3. Read the low byte of that instruction as a syscall number.
4. Get a function pointer from a dispatch table, as `table[syscall_number]`. RAM `0x000000E0` holds the base address of the table.
5. Call the function pointer with an interworking `BX`.
6. Return with `LDM SP!,{r1-r12,lr,pc}^` (see "CPU" above).

## Interrupt controller

`0x0A000000` has four registers: `hold`, `status`, `enable`, and `mask`. `intc_irq_asserted` and `intc_fiq_asserted` (`core/src/intc.c`) calculate `hold & enable & INT_IRQ_MASK` and `hold & enable & INT_FIQ_MASK` when a caller asks for the value.

The map from a bit to a source (`INT_BTN_*`, `INT_TIMER*`, `INT_RTC`, `INT_IOP`, and `INT_IRDA`) agrees exactly with the recorded 14-source table. The FIQ sources are bit 6 (`COM`) and bit 13 (`Timer2`). Each other source uses IRQ.

IRQ and FIQ are both level-triggered: this emulator reads both lines at each CPU step. It does not use a single latched request. Thus the CPU enters the handler again while the line stays asserted.

**FIQ has a higher priority than IRQ.** FIQ entry sets `CPSR.F` and `CPSR.I`. IRQ entry sets only `CPSR.I`. See `arm7tdmi_step` in `core/src/cpu.c`.

This project implemented `intc_fiq_asserted` correctly early, but no code called that function until the correction. Before the correction, this emulator never delivered a FIQ, for any app. See "Hardware ID (F_SN)" below for the method that found this fault. See `test_fiq_delivery_and_priority` and `test_fiq_takes_priority_over_irq`.

The button sources and the RTC source (`INT_STATUS_MASK`) latch into both `hold` and `status` at an assertion. They clear from both registers together at a de-assertion.

**The IRQ vector handler and the FIQ vector handler of the BIOS each call a separate callback that an app registers. Each handler uses a different fixed slot in low RAM.** A disassembly of a real J-110 BIOS dump confirms this. The IRQ vector handler (`0x04001414`) reads its callback pointer from RAM offset `0xFC`. The FIQ vector handler (`0x040014D4`) reads its pointer from offset `0x100`, and only for a FIQ source that is not `COM`. A `COM` FIQ (bit 6) branches to internal BIOS code first, and it never gets to that slot. Neither handler acknowledges the interrupt. Both handlers leave that operation to the registered callback. This agrees with the `IRDA_DATA` pattern, which also has no BIOS interface (see "IR / IR Link" below). An app installs each callback with `SWI 1`. `r0` selects the slot: `1` for IRQ, and `2` for FIQ. `r1` is the handler address, and a value of `0` removes the callback. A registration in the incorrect slot for the exception type of a source gives no error message. That source then has a HOLD bit that nothing acknowledges. Thus the CPU enters the vector at each step, and it never leaves. Screen 10 of `pk_timing_bench` caused this condition directly (see `pk_timing_bench/README.md`): it first used the IRQ slot for Timer2, which is a FIQ source, and it stopped in this emulator before a test on real hardware.

## Timers

There are 3 timer channels at `0x0A800000` and above, at intervals of `0x10` bytes. Each channel has a `period` register (+0x0), a `count` register (+0x4), and a `control` register (+0x8). Bits 0 to 1 of `control` select the clock divisor: `0` or `3` gives /2, `1` gives /32, and `2` gives /512. Bit 2 of `control` starts the timer. `count` decreases one time for each `divisor` raw cycles. It does not decrease at each raw cycle.

Timer1 operates the audio loop of the BIOS. It also operates the general GUI ticks: the blink on the date-setting screen, and the HELLO boot animation. Timer1 is not an audio-only IRQ source. Timer2 is the FIQ timer (see "Interrupt controller" above).

The `count` value of a timer uses raw cycles, which `CLK_MODE` scales. The System Clock clocks the real timers, and that clock connects directly to the variable clock of the CPU (see "CLK_MODE" below). The RTC and the DAC do not use this clock.

**Confirmed: a timer with period P expires each P+1 ticks. It does not expire each P ticks.** The counter goes from P, to P-1, down to 1, and then to 0. It reloads at the tick *after* it gets to zero. Thus zero is a state of the counter.

Screen 6 of `pk_timing_bench` measured this behavior directly on real hardware. `pk_timing_bench/VERIFICATION.md` records the raw values. That screen reads the reloads of Timer2, and it uses Timer0 to measure the time. It takes no interrupts. That screen used Timer2 at period 1016 over 256 reloads, and at period 2032 over 128 reloads. Each test gave a result that is **exactly 1 tick for each reload** slower than a period of P ticks predicts. The extra time is the same absolute value at both periods. That result removes a rate error and a divisor error as causes, and it gives a fixed error of one tick for each period.

`core/src/timer.c` used exactly `count` ticks for each reload before. Thus each timer expired one tick early. That fault is now corrected. Three timer values in `tests/cpu_test.c` held the earlier behavior, and this project corrected them at the same time.

**A timer loads `count` from `period` at the edge where software enables it.** `timer_write8` does this load when bit 2 of `control` goes from clear to set. A disassembly of the timer-set helper of a real IR app gives this behavior. That helper disables the timer, writes `period`, and then enables the timer again. It never writes `count`. The receive handler of that app arms Timer2 again between pulses, and it reads the elapsed time as `armed period - current count`. Without the load at the enable edge, the counter continues to decrease from its old value, and each measured interval after the sync signal is incorrect. This behavior does not change the P+1 rule above. Screens 8, 10, and 11 of `pk_timing_bench` give identical values before and after this correction: 31.0, 39.0, and 71.8 ticks.

**The `period` and `count` registers of a timer are 16-bit registers. They are not the 32-bit counters that this emulator modeled at first.** The current code: `timer_write8` masks both registers with `TIMER_REG_MASK` (`core/src/timer.h`), thus it discards the upper half of a wider store. Each reload also uses the mask. See `test_timer_registers_are_16_bit`.

`pk_timing_bench`, the homebrew timing-benchmark app of this project, found this fact:

- Each raw `count` value from a real unit had its upper 16 bits at zero.
- A measurement loop wrote `period` and `control` one time. It then read `count` before and after a long loop, with no configuration change between the two reads. When the loop accumulated more than 65536 raw ticks, the *after* value was numerically larger than the *before* value. There is only one explanation: the counter wrapped past zero and reloaded during the loop, at a 16-bit boundary and not at a 32-bit boundary.

This file recorded the width as an inference from those values alone at first. **A real commercial app has since confirmed the width independently.** One music app drives its audio from Timer2, the FIQ channel, and it programs that timer with a value whose upper half is not zero. With the earlier 32-bit model, the upper bits increased the period from 851 ticks to `0x03240353`, which is approximately 52.6 million ticks, or approximately 62,000 times too long. Thus the audio interrupt never occurred. The app opened its DAC gate: it set `DAC_CTRL` to 1, and it started IOP bit 5. It then played a full song, with notes on the screen and a results screen. It wrote `DAC_DATA` exactly zero times. The result was full silence, with each gate open. A mask to the real 16-bit width gives period `0x0353` (851). That value agrees with the `0x34F` (847) that the app programs into Timer1 at the same time, and the music then plays.

This condition shows an important failure mode: a timer period that is too wide gives no message. Nothing faults, the app operates normally, and the only symptom is a peripheral that never operates.

`test_timer_scales_with_clk_mode` held the earlier behavior. It loaded a period of 100000000, which no real timer can store. It now uses the largest real 16-bit period, with one half of the earlier budget. Thus the fast-clock condition still never wraps.

See `docs/app-notes.md`'s timing-benchmark writeup for the full before/after data.

## RTC

`0x0B800000`: bit 0 of `mode` (`PRGSEL`) selects the operating mode of the RTC:

- Run mode (`0`): the RTC ticks at 1Hz, and it advances the clock.
- Program or pause mode (`1`): the RTC ticks at approximately 4096Hz, and it does not advance the clock. Thus a manual adjust operation can change one field, and the clock does not move at the same time.

Bits 1 to 3 of `mode` (`CNTSEL`) select the BCD field that a `control` or `RTC_ADJUST` write changes.

The automatic advance cascades in this order: seconds, minutes, hours, and the day of the week. It does not cascade into `date` at a day rollover. No documentation confirms or denies this gap. This emulator has this gap from earlier work, and this project does not confirm it as correct.

The power-on-reset values of real hardware are `RTCClock = 0x04000000` (day of the week BCD 4, and 00:00:00), and `RTCCalendar = 0x00980101` (1998-01-01). Bits 24 to 31 of `RTC_DATE` are an unused, unidentified field. They are not a "year-high" byte or a century byte. The real century value is in battery-backed kernel RAM, and only the `GetBcdDate` SWI supplies it.

The RTC ticks at a fixed 1Hz in Run mode, for each `CLK_MODE` value. It uses a separate oscillator, independent of the CPU clock.

**Both recorded rates are waveform rates, and not transition rates: 1Hz while the RTC runs, and 4096Hz while it is paused, with two line transitions for each pulse.** Screen 14 of `pk_timing_bench` measured this on real hardware (see its `VERIFICATION.md`). While the RTC is paused, it gave 256 transitions in exactly 0.031250 seconds. That result is 8192 transitions each second, thus 4096 full pulses. While the RTC runs, it gave four transitions in exactly 2.000 seconds. This emulator used both figures as transition rates, and its line operated at one half of the real frequency in both modes. `rtc_tick` now advances the clock one second for each full pulse, and not for each transition. Thus the wall clock stays correct, and the line operates at the real rate.

That paused measurement also **confirms that `CLK_MODE 7` is 3,997,696Hz**. The frequency table had that figure only from documentation. A result of exactly 0.031250 seconds is not possible if the real CPU rate is significantly different.

**`RTC_TICK_CYCLES_RUN` is that 1Hz rate, in `PSEMU_ASSUMED_CPU_HZ` reference cycles. Thus the two constants are equal by definition.** This needs no measurement: the RTC operates a wall clock, and one emulated second must last one real second. The value was `4000000` for a long time. Somebody selected that value only to make a wait-for-pulse loop complete quickly, and nobody compared it against a real 1Hz reference. That value gives 3.79 reference-seconds for each tick. Thus the clock of the device operated almost 4 times too slowly: 60 seconds of real time advanced it by 15 seconds, and it lost approximately 45 minutes each hour. Each function that read the clock of the PocketStation saw that error.

The BIOS sets the clock to January 1, 1999, in a recorded condition. The manufacturer calls that condition "The RTC Problem". It is a software correction for inaccurate clock hardware. This reset is a software action, and it uses the usual `RTC_ADJUST` mechanism. The reset state of this emulator does not include this behavior.

### Where the date/time settings actually live

`tools/datetime_probe.c` traced this data. The settings are in the RTC registers and in several RAM copies. No part of the data goes to flash:

| Location | Holds | Written by | Role |
|---|---|---|---|
| `RTC_TIME`/`RTC_DATE` | seconds, minutes, hours, day-of-week; day, month, year (BCD) | `RTC_ADJUST` increments from `0x0400055A`, `0x04000580` and neighbours | drives the clock display |
| RAM `0x426` | century, BCD `0x19` | `0x0400330C`, once at boot | drives the clock display |
| RAM `0x0CF` | century, BCD `0x19` | `0x04000350` | derived from the RTC year, not an input |
| RAM `0x0CD` | year, BCD `0x99` | `0x04000668` | derived, as above |
| RAM `0x120`-`0x123`, `0x128`-`0x12B` | a date, `01 01 99 19` | `0x040003D4`, `0x04000498` | boot-time working copies; nothing observable reads them back |

**The century value is only in RAM.** `RTC_DATE` has no century field. This is the reason that `GetBcdDate` is the only method to read the century.

**An experiment found the century byte. It is `0x426`, and not `0x0CF`.** The `screen` mode of `tools/datetime_probe.c` boots to the clock screen. It then writes a selected combination of these addresses, after the end of the boot reset, and it writes the LCD content to the output. Thus a person can read the digits directly. The screen shows the date as `YYYY/MM/DD`. The 32-pixel width cuts off the day. These results all use an RTC value of 2026-08-01:

- The RTC registers alone: the screen shows `1926/08/`. The RTC supplies the month and the two low digits of the year. The century does not follow the RTC.
- The RTC registers, and `0x0CF` and `0x0CD`: the screen still shows `1926/08/`. A write to those addresses changes nothing on the screen.
- The above, and the `0x120` and `0x128` copies: the screen still shows `1926/08/`.
- The above, and `0x426`: the screen shows `2026/08/`.

Code writes `0x426` exactly one time, at boot, from `0x0400330C`. After that write, code only reads the byte: 251 reads across 3 million instructions, all from `0x04002542`, which is the clock-display routine.

**`0x0CF` and `0x0CD` are outputs. They are not inputs.** In the RTC-only run above, the BIOS changed `0x0CF` from `0x19` to `0x20`, and `0x0CD` to `0x26`, in response to an RTC year of 26. No code wrote to either byte. Thus the BIOS calculates a century from the two-digit year with a window rule, and it stores the result at those addresses.

**There are two independent century bytes, and they can hold different values.** `GetBcdDate` reads `0x0CF`, and the clock screen reads `0x426`. A direct call to the SWI (see below) confirms this. With `RTC = 2026-08-01`, `0x0CF = 0x20`, and `0x426` at `0x19`, the SWI returned `0x20260801`, and the screen showed `1926/08/`. Each function that overrides the date must write both bytes. If it does not, an app and the clock of the BIOS report different years.

**After an app dispatch, `0x426` is app RAM. An override of that byte at each frame corrupts the app.** The dispatch routine clears user RAM from `0x200` to `0x7FF` before it branches to the entry point (see `docs/app-notes.md`, "App-selection and dispatch"), and `0x426` is in that range. The `browse` mode of `tools/datetime_probe.c` traced this behavior, against a real card image in `testdata/`. While the app operates, it reads `0x426`, from FLASH1 PCs `0x02002772` and `0x02001F3E`. It also writes that byte continuously, and it cycles through values such as `0xE7`, `0x24`, and `0x66`, at intervals of approximately 11000 instructions. Thus a frontend that writes a century byte there 32 times each second writes into the working memory of an app in operation. This condition is not theoretical: it made the `datetime_override=os` setting of the desktop app send the PocketStation app of the card-data app to its "ODD DATA" screen. It is also the reason that `psemu_app_running` exists (see `core/include/psemu/psemu.h`).

**The browse screen of the BIOS reads neither century byte. Thus a hold on those bytes is safe while the BIOS shell operates.** This test was necessary for two reasons. First, the browse screen draws and animates the icon of each app on a card. Second, `0x426` is in user RAM, next to the open question about the volume byte. The `browse` mode of `tools/datetime_probe.c` moves to the browse screen, writes `0x426 = 0x5A` and `0x0CF = 0xA5`, and then monitors both bytes for the remainder of the run. This test used three real cards. Each rendered frame across 5.5 million instructions is identical, byte for byte, to a control run that writes nothing. The read counts and the reading PCs also agree exactly. Only four PCs read either byte, and each of those PCs is date or clock code: `0x04000386` reads `0x0CF` one time at boot, and `0x04002542` (the clock-display routine), `0x040029E2`, and `0x04002A08` read `0x426`. The last two PCs read the byte one time each, while the screen still shows the clock. No icon-drawing code accesses either byte.

### SWI dispatch table (J110)

The `swi` mode of `tools/datetime_probe.c` calls each entry of the kernel dispatch table separately, from a state that this project recorded after the boot sequence. It uses a sentinel return address and a scratch buffer with a known pattern, and it restores the state between entries. The table base is at RAM `0x0E0`, and it holds `0x04001688`.

| SWI | Entry | Returns |
|---|---|---|
| 10 (`0Ah`) | `0x040017A5` | `FlashReadSerial` - returned `0x410000D3`, this emulator's default `F_SN`, confirming the documented mapping |
| 13 (`0Dh`) | `0x04000369` | `GetBcdDate` - `r0` = BCD `CCYYMMDD`. Returned `0x19990101` at boot, `0x20260801` with the RTC and `0x0CF` moved |
| 14 (`0Eh`) | `0x04000391` | `GetBcdTime` - `r0` = BCD, day-of-week/hours/minutes/seconds, matching `RTC_TIME` exactly |

Both date SWIs return their value in `r0`. Neither writes through a buffer that the caller supplies: the scratch buffer in `r0` came back with no change. `GetBcdDate` builds its result from two sources: the century byte at RAM `0x0CF`, and the day, month, and year from `RTC_DATE`.

Entries 2, 9, 12, and 15 never got to the sentinel in 20000 instructions. Thus this file gives no data about them. Entry 12 (`0x04000519`) is inside the boot date-setting routine, and it reads `0x0CF`. Thus `SetBcdDate` is a reasonable assumption, and a separate test can confirm it.

**The boot path always writes the date.** At approximately instruction 14686, the BIOS writes 1999-01-01 into the RAM copy. It then changes the RTC to program mode (`PRGSEL=1`), and it increases each field with `RTC_ADJUST` until the hardware clock agrees. It reads `RTC_DATE` first, at `0x0400036E` and `0x04000372`, and gets the power-on-reset value `0x00980101`. But no code reads a RAM date byte before it writes that byte. Thus the contents of RAM do not control this decision.

**Three separate attempts to reach a warm-boot path all failed.** In each attempt, the RAM copy still held 1999-01-01, and the BIOS still adjusted the RTC to agree:

- Load `RTC_DATE` and `RTC_TIME` with a valid later date, 2007-06-15, before the boot.
- The same, and also load each RAM copy and the century byte.
- The same, and also load a real 128KB memory card image. The BIOS reads `FLASH2` offset 0 a short time before the decision, thus the card contents were the last untested input.

**This condition has the same shape as the volume setting, but it is less severe.** The volume byte (`0x290`) is also cleared at each boot that this emulator can produce. Real hardware must also keep that byte, because the battery keeps the SRAM powered. But the BIOS clears the volume byte one time, early, and it never writes the byte again. Thus a hold on the byte is sufficient (see "System sound volume setting"). For the date and the time, the BIOS adjusts the value to a target over many instructions. Thus there is no equivalent byte to hold. `psemu_reset` is equivalent to a new battery, and this emulator has no other boot path. Thus the January 1999 reset can be correct for that specific condition, and not a fault. This project has not established whether real hardware has a warm path at all, or what condition selects it.

The result for persistence: a write to the registers before the boot cannot operate for the date and the time, because the BIOS writes over them after that. There are two options. The first is a full save-state load, which does not use the boot path at all, and which already operates. The second is a write of the necessary date after the boot, through the same `RTC_ADJUST` sequence that the BIOS uses.

## Communication port

The communication port is the link to a PS1. The link goes through the memory card connector. A PocketStation is a memory card in that connector. `core/src/com.c` and `com.h` hold this peripheral.

### Registers

The block is at `0x0C000000`, and it has a span of `0x20`.

| Offset | Register | Contents |
|---|---|---|
| `+0x0` | `COM_MODE` | bit0 Data Output Enable. bit1 /ACK Output Level (1 = drive LOW). bit2 unknown. |
| `+0x4` | `COM_STAT1` | bit0 a byte arrived. bit1 the console released /SEL. See "The /SEL line". |
| `+0x8` | `COM_DATA` | bits 0 to 7. A read gets the byte from the PS1. A write sends a byte to the PS1. |
| `+0x10` | `COM_CTRL1` | Unknown. The observed values are 0, 2, and 3. |
| `+0x14` | `COM_STAT2` | bit0 Ready (0 = Busy, 1 = Ready). The hardware sets the bit after 8 bits. |
| `+0x18` | `COM_CTRL2` | Unknown. The observed values are 1 and 3. |

The ranges at `+0x0C` and `+0x1C` have no known register. Both ranges read back as 0. A write to them has no effect. `IRDA_MISC` gets the same treatment.

A published register map is the source of this layout. That map is community reverse engineering. It is not a manufacturer specification. The map marks the function of the `COM_CTRL1` bits and the `COM_CTRL2` bits as unknown. It gives only the values that it observed. A trace of a real BIOS confirms each of those values exactly. That trace also adds the two behaviors in "The handshake" below. No map records those two behaviors.

`INT_COM` (bit 6) is the interrupt of this block. It is a FIQ source. `INT_IOP` (bit 11) is the separate docking sense. A value of 0 is undocked. A value of 1 is docked to a PS1. Real hardware probably senses the supply voltage on the connector.

**`INT_IOP` must reach `STATUS`. An acknowledge must not clear it.** The register map names this bit in `INT_INPUT` directly. It records a necessary read of that live level during a transfer. That read finds an undock event while the transfer is in progress. Only a level in `STATUS` can supply that reading. Docking is also a continuous condition, and not an event. A device that stays in the connector stays docked. Thus `INT_IOP` is now in `INT_STATUS_MASK` and in `INT_LEVEL_MASK` (`core/src/intc.h`). Without the second mask, an acknowledge cleared the level. Each dock event then read back as an undock event. `INT_IRDA` and the buttons are in those masks for the same reasons.

### The protocol is firmware, and not hardware

**The kernel of the real BIOS holds the memory card protocol. This emulator implements no command.** A byte from the console raises `INT_COM`. The FIQ handler of the kernel then answers that byte. That handler holds the three memory card commands: `0x52` Read Sector, `0x53` Get ID, and `0x57` Write Sector. It also holds the PocketStation commands `0x50`, and `0x58` to `0x5F`.

This division is necessary. The commands `0x5B` and `0x5C` execute a function number. The numbers `0x80` to `0xFF` resolve through a function table in the header of the app file. Only the app holds that code. Thus no protocol code outside this emulated machine can answer those commands.

The kernel handles a full command inside one FIQ. IRQs and FIQs stay disabled for that time. The main program stops. Thus only the first byte of a command needs the interrupt. The kernel polls `COM_STAT2` for each byte after the first byte. That poll is at `0x04001592` in the `110` revision.

This behavior has a real result on hardware. An audio generator that an interrupt drives stops during a command, and the sound distorts. This emulator gives the same result, because the behavior comes from the firmware. No model in this repository produces it.

### The handshake

`tools/com_probe.c` recovered this sequence from a real `J110` dump. Three kernel addresses are relevant. The COM initialization is at `0x0400073E` to `0x0400078C`. The FIQ entry is at `0x04001072`. The loop for each byte is at `0x04001574` to `0x0400159E`.

The kernel does these steps for each byte:

1. Write `COM_CTRL2 = 1`.
2. Read `COM_STAT1` for the error flag.
3. Poll `COM_STAT2` for the Ready bit.
4. Read `COM_DATA`, but only if the command needs the incoming byte.
5. Write the reply to `COM_DATA`.
6. Write `COM_MODE = 1`. This step drives the data line.
7. Write `COM_MODE = 3`. This step pulls /ACK LOW.

**The block is a shift register.** One exchange moves the byte of the console in. The same exchange moves the held byte out. Thus the console receives the byte of exchange N during exchange N+1. The kernel writes `0xFF` into `COM_DATA` at initialization. That write gives the first exchange a byte to send.

Two facts together give this conclusion. The kernel writes FLAG while it processes the `0x81` byte. It writes `0x5A` while it processes the `0x53` byte. The published command table gives the reply to `0x81` as "N/A". It gives the reply to `0x53` as FLAG. A model with immediate replies delivered each byte one position early.

**The Ready bit of `COM_STAT2` clears at a read of `COM_DATA`. It also clears when the kernel drives /ACK LOW.** Both events are true. One event alone is not sufficient.

No published source records the acknowledge event. A trace of the `J110` kernel gives it.

The data-read rule alone is not sufficient. The data phase of a command sends bytes and receives only dummy bytes, thus the kernel never reads `COM_DATA` during that phase. Ready must still clear there. Without that, the poll loop of the kernel reads a byte that never arrived, and it runs through the remainder of the command inside one exchange.

The acknowledge rule alone is not sufficient either. At the first byte the kernel acknowledges and then reads `COM_STAT2`, at `0x04000800`. That read must give 0.

### The /SEL line

**A console holds the /SEL line of the connector for the full duration of one command. It releases the line between commands. `COM_STAT1` bit 1 reports that release, and the kernel needs the signal to end a command.**

The kernel waits at `0x040007B8` after the last byte. It polls `COM_STAT1` there. Nothing else releases that wait. Without the signal, the kernel answers the first command after docking and then answers nothing. The `selbit` mode of `tools/com_probe.c` sets one candidate bit at a time and then sends a second command. Bit 1 is the only bit that lets that second command run. Bits 2 to 7 all fail.

The published register map names bit 1 "Error flag", and it gives four candidate meanings. One candidate is "/SEL disabled during transfer". That candidate is the correct one. A release of /SEL is not a fault. It is the usual end of each command.

A read of `COM_STAT1` clears this latch. The same map records a dummy read of the register by the kernel at the end of each transfer. It gives a hardware clear at a read as one candidate reason for that dummy read.

**`COM_STAT1` bit 0 reports an arrived byte.** It gives the same condition as the Ready bit of `COM_STAT2`. The write path of the kernel polls `COM_STAT1` in place of `COM_STAT2`, at `0x040015D6`. The bit must follow the arrival of a byte exactly. A bit that stays clear stops that path after one data byte. A bit that stays set makes the kernel read one byte many times, and Write Sector then answers `0x4E` ("N", bad checksum).

**The kernel programs flash only after the command ends.** No store reaches the flash regions during the byte exchanges of a Write Sector command. The stores follow the release of /SEL.

### Verified exchanges

`tools/com_probe.c` runs these exchanges against a real `J110` dump. Each reply below comes from the firmware. No code in this repository produces one of these values.

- **Get ID (`0x53`)** returns `0xFF`, FLAG (`0x08`), `0x5A`, `0x5D`, `0x5C`, `0x5D`, `0x04`, `0x00`, `0x00`, and `0x80`. The last byte carries no acknowledge. A usual memory card gives the same values. It also gives no acknowledge for that last byte.
- **Read Sector (`0x52`)** takes 140 exchanges. It returns FLAG, `0x5A`, `0x5D`, the address echo, `0x5C`, `0x5D`, the confirmed address, 128 data bytes, the checksum, and the `0x47` ("G", Good) terminator. The dummy bytes at the address positions are `0x00`. A usual card sends the previous byte at those positions. The register map records this difference. The trace confirms it.
- **Command `0x5A` (Get Dir_index, ComFlags, F_SN, Date, and Time)** returns six items in order: the length `0x12`, the current dir index, four `ComFlags` bits, `F_SN` as `D3 00 00 41`, the BCD date, and the BCD time. That `F_SN` value is `0x410000D3`, which is `PSEMU_DEFAULT_HARDWARE_ID`. The date is 1999-01-01. The BIOS boot path always writes that date (see "RTC" above). A console game uses this command to find a PocketStation in place of a memory card.
- **Write Sector (`0x57`)** takes 139 exchanges against a real card. It returns FLAG, `0x5A`, `0x5D`, the address echo, `0x5C`, `0x5D`, and the `0x47` ("G", Good) terminator. The `write` mode of `com_probe` then reads the frame back out of flash. The frame holds the sent data exactly, and no other frame of the card changes. A run with a deliberately incorrect checksum returns `0x4E` ("N", bad checksum), and no frame changes.
- The flash-write counts agree with the documented write sequence exactly. One Write Sector command makes 134 stores into `FLASH2` from `0x0400122C`, and 8 stores into `FLASH_CTRL` from `0x04001278`. The 134 stores are 128 data bytes and the three halfword unlock keys. The 8 stores are `F_WAIT2 = 0x21` and then `F_WAIT2 = 0x00`.
- **Command `0x5B` (Execute Function and transfer data to the console)** operates with the two kernel functions that need no app. `FUNC 00h` returns the marker `0xFF`, `LEN1 = 0x00`, `LEN2 = 0x08`, the BCD date and time, and the `0xFF` terminator. `FUNC 01h` reads memory: it returns `LEN1 = 0x05`, it accepts a 32-bit address and a length, and it then returns that number of bytes. A read of 16 bytes at `0x04000000` returns the first 16 bytes of BIOS ROM, and each byte agrees with the loaded image. The `func` mode of `com_probe` makes that comparison.

  **This command carries the whole reason to model the hardware and not the protocol.** The function numbers `0x80` to `0xFF` resolve through a function table in the header of the app file. Only the app holds that code. The transport above is the same transport that those app functions use, and only the table lookup is different. Thus a working `0x5B` is the evidence that an app-supplied function can answer a console.
- **Command `0x5F` (Get-and-Send ComFlags.bit0)** operates, and it changes the word from `0x0007020F` to `0x0007020E`. This command is NOT necessary before a write on this revision. An official kernel specification gives bit 0 as a flash-write enable. A published register map records that the two layouts of this word come from different BIOS revisions. The J110 revision follows the reverse-engineered layout, where bits 0 to 3 have no recorded meaning.

`ComFlags` is a word in kernel RAM at `0x0C0`. Bit 9 is "Communication Enabled And Docked". The kernel sets that bit one frame after `psemu_com_set_docked` asserts the docking level. No code answers a command while that bit is clear. `com_probe` reports the word before the transition and after it. The recorded values are `0x00070000` and then `0x0007020F`.

### Known open questions

- The function of the `COM_CTRL1` bits and the `COM_CTRL2` bits is still unknown. This emulator accepts the writes. It reads the values back. No observed behavior depends on those bits. Thus this treatment is sufficient for the exchanges above. A command that this project did not test can still depend on them.
- `COM_STAT1` bit 1 is an error flag, and this emulator never sets it. This model has no error condition. A caller delivers a complete byte, or it delivers nothing. Real hardware can report three more conditions: a timeout, a parity error, or a /SEL line that went low during a transfer.
- A transfer that arrives during a clock stop is untested. `INT_COM` is not in the wake sources of `psemu_run`. Such a transfer reports no acknowledge after its cycle budget expires. Real hardware probably wakes, because a console must reach a card in a device that sleeps.

## IR / IR Link

### Registers

The IR block is at `0x0C800000`, and it has a span of `0x10`. `ir_read` and `ir_write` in `core/src/ir.c` hold this behavior.

| Offset | Register | Contents |
|---|---|---|
| `+0x0` | `IRDA_MODE` | bit0 `IFMODE` (0 = Receive, 1 = Transmit), bit1 `STDBY` (0 = Active, 1 = Stand-by), bit2 `BGEN` (0 = the 40kHz carrier generator is enabled, 1 = disabled), bit3 `BFLT` (0 = the glitch filter is enabled, 1 = disabled) |
| `+0x4` | `IRDA_DATA` | bit0 `LED`. In transmit mode this is the level that software bit-bangs. In receive mode this is the live demodulated line. |
| `+0x8` | no known name | Reads 0. A write has no effect. |
| `+0xC` | `IRDA_MISC` | Unknown or reserved. Reads 0. A write has no effect. This is the same stub treatment as `BATT_CTRL` below. |

No available data separates offset `+0x8` from `IRDA_MISC`. Thus this emulator gives both offsets the same treatment.

**The demodulated line is active low.** A carrier that is present reads 0, and an idle line reads 1. Real IR demodulator receivers also give an active-low output. `ir_t::rx_level` keeps physical terms, where 1 means that the carrier is present. The inversion is at the two places that emulated software can read the line: `ir_read`, and the level that `apply_rx_level` gives to `intc_set_level_and_pulse`. Software reads that same live level from `INT_IRDA` (bit 12) of the INTC `STATUS` register. See "Interrupt controller" above.

A disassembly of the receive handler of a real app gives that polarity. The handler arms itself for level 0 before a carrier arrives, and it measures the sync burst as the interval that ends when the line returns to 1. No real-hardware measurement confirms the polarity directly.

### Sources for the register layout

A disassembly by this project is the source. A secondary register map for this range agrees with that layout independently. That map is community reverse engineering, and not a manufacturer specification. It marks several IR details as uncertain.

Two versions of that map disagree with each other on `IRDA_MODE` bits 1 to 3. One version gives `STDBY`, `BGEN`, and `BFLT`. The other version gives a simple "disable" bit and two bits with different names, and it records that guess as uncertain. The behavior of a real app decides the disagreement in favor of `STDBY`, `BGEN`, and `BFLT`. The behavior of a real app has more authority than a secondary source.

The secondary map confirms three more facts independently of that disassembly:

- The real BIOS has no IR functions, except basic initialization and power-down handling. A real app writes `IRDA_MODE` and `IRDA_DATA` directly from its own code, and it uses its own interrupt handler. There is no BIOS SWI for IR, thus this project does not have to look for a BIOS-level IR interface.
- Real pulses alternate between ON and OFF. They do not hold one long ON period. Real IR receiver hardware adapts to ambient light, thus a continuous signal can look like a new ambient level and not like data.
- `INT_IRDA` occurs at a rising edge and at a falling edge of incoming data. A real handler reads the live counter of Timer 2 (reload `0xFFFF`) to measure the interval.

**No source gives numeric timing for this peripheral.** There are no microsecond pulse widths, and no carrier-to-pulse ratio more exact than "a long pulse is usually two times a short pulse". A register map cannot answer that question. Only a measurement on real hardware can answer it. This is the reason for screens 6 to 11 of `pk_timing_bench`.

### The model: an asynchronous edge relay

This emulator models IR as an **asynchronous edge relay between two instances that have independent clocks**. It is not a lockstep timing simulation. This model agrees with the real hardware: two separate PocketStation units, two separate oscillators, an optical signal between them, and no shared clock. `core/src/ir.c` and `ir.h` hold the state machine.

- **Transmit.** A write to the LED bit of `IRDA_DATA`, while the transmitter emits a signal (`IFMODE` = transmit, and `STDBY` = active), puts a timestamped edge onto the TX queue. That edge holds the level and the local monotonic clock of this `ir_t` instance. When the transmitter leaves the emit condition, for standby or for receive, `handle_mode_write` puts one final "LED off" edge into the queue. Thus the LED can never appear to stay on to the receiver. `BGEN` is not part of the emit condition. See "Two transmit styles" below.
- **Receive.** `ir_tick` advances the local clock and resolves each RX-queue edge that is due. `psemu_run` calls it one time for each CPU step, together with `timer_tick`, `rtc_tick`, and `dac_tick`. Each edge passes the `BFLT` glitch-filter debounce first. `apply_rx_level` then writes `rx_level`, and it calls `intc_set_level_and_pulse(intc, INT_IRDA, !level)` while the receiver is active (`IFMODE` = receive, and `STDBY` = active). In each other condition it discards the edge. A real half-duplex transceiver also does not see a pulse during a transmission.
- **Why the INTC call is not a plain `intc_set_line`.** The receive handler of a real app acknowledges `INT_IRDA`, and it then reads `STATUS` to sample the live line. Thus `STATUS` must follow the real level, and `HOLD` must still latch at both edges. See the comment on `INT_STATUS_MASK` in `intc.h`.
- **The debounce window.** `IR_BFLT_DEBOUNCE_CYCLES` is approximately 2 carrier periods, which is 52 reference cycles or approximately 49us. A filtered transition stays pending for that time, and an opposite edge inside that time cancels it. This constant is an inference. No source records the real value.
- **The core interface.** `psemu_ir_pop_tx_edge`, `psemu_ir_push_rx_edge`, and `psemu_ir_get_clock_us` (`psemu.h`) supply this data as a pull-and-push edge queue. `psemu_get_audio_samples` uses the same shape, which lets a frontend operate real I/O while the core knows nothing about the transport of that I/O. The core has no network code, and it will never have network code. A timestamp uses real microseconds only at this interface, and the code converts it from the internal cycle clock. In each other location, the core uses the same reference-rate cycle units as each other peripheral.
- **The queue capacity.** `IR_EDGE_QUEUE_CAPACITY` is 4096 edges. One real 41-byte burst from the serial-carrying app makes 658 edges, thus one queue holds more than four such messages. A full queue discards the newest edge and keeps the order of the queue. `psemu_ir_trace_enabled` reports each discarded edge.
- **Reset.** `psemu_reset` and `psemu_load_state` both clear the clock of `ir_t` and each edge in its two queues.

### Two transmit styles: `BGEN` does not gate emission

**Real apps do not agree on `BGEN`, and both operate on real hardware.**

| | The serial-carrying app | The card-data app |
|---|---|---|
| `IRDA_MODE` while transmitting | `0x01` | `0x0D` |
| `BGEN` (bit 2) | 0, thus the hardware 40kHz carrier is **on** | 1, thus the hardware carrier is **off** |
| `BFLT` (bit 3) | 0, thus the glitch filter is **on** | 1, thus the glitch filter is **off** |
| Pulse shape | wide envelopes, which the hardware fills with carrier | approximately 7 reference cycles (6.6us), with the LED driven directly |
| Encoding | pulse *width*, where a long pulse is about twice a short pulse | pulse *distance*, with gaps of 205 or 406 cycles at a slot of approximately 194us |

The two rows at the bottom of the table explain the two rows at the top. The pulses of the card-data app are much narrower than `IR_BFLT_DEBOUNCE_CYCLES`. This is the exact reason that the app turns the glitch filter off, in the same register write that turns the carrier generator off. The app has no configuration error. It uses a different, self-clocked signal method, and it disables the two hardware functions that obstruct that method.

**Thus `BGEN` selects whether the hardware divides the ON envelope of the LED into a 40kHz burst. It does not control whether the LED comes on.** This emulator relays only that ON and OFF envelope, and it does not model the sub-carrier in the envelope. Thus `BGEN` has nothing to gate, and `tx_emit_active` does not test it.

This condition is an example where correct source text still gives an incorrect model. The wording of the secondary register map, "0=Enable 40kHz carrier generator, 1=Disable", is correct. That wording gives the effect of the bit on the carrier. It gives no data about a dependency between the emission and the bit. An earlier model of this emulator inferred such a dependency, and real apps disprove it. With that gate present, each `IRDA_DATA` write of the card-data app had no effect, and `tools/ir_probe.c` reported `edges relayed: A->B 0` against a save state on the transfer screen of that app.

**Two encodings, one link.** `tools/ir_probe.c` recovers the encoding from the relayed edges alone, with no knowledge of either app. It clusters both the gap between pulses and the width of the pulses, and it reports whichever axis splits into two populations. The serial-carrying app comes back as pulse-width, with symbols at 803 and 1408 reference cycles (760us and 1333us) on a constant 404-cycle gap. The card-data app comes back as pulse-distance, with gaps of 228 and 413 cycles between fixed pulses of approximately 7 cycles.

The two symbols of the serial-carrying app have a ratio of **1.75, and not 2**. The statement "a long pulse is usually two times a short pulse" is an approximation. A detector that uses integer multiples of a unit reads the short symbol of that app as one half of a unit, and it then rounds that value to 1. Thus it reports a stream with one symbol, and it has no data to decode. A method that groups two populations and measures their separation makes no such assumption, and it reads both apps correctly. The state block of the serial-carrying app records its nominal unit at `+0x20` as `1200`, against the value 1207 that a measurement of the edge stream gives.

### The transmitted falling edge is stretched

**A falling edge, which is the point where software commands the LED off, is delayed by a fixed quantity before it goes onto the TX queue.** `IR_TX_FALL_STRETCH_CYCLES` in `ir.c` is 200 reference cycles. A rising edge is never delayed, and the OFF gap between pulses is never stretched.

This is a deliberate, recorded concession. It is not a model of a physical effect. Real-hardware measurements remove each digital cause for the shortfall that it corrects (see "Timing measurements on real hardware" below). The best remaining explanation is the physics of a real transceiver, which this emulator does not model as an analog signal: an LED does not switch off immediately, and the response of a receiving photodiode and its AGC settling both add real time before a pulse reads as complete. The tuning of the constant uses the measured shortfall of this emulator, which is 38 Timer2 ticks. It does not use the unrelated `184` constant of the real app.

Two limits protect the edge stream, and both are necessary:

- **The stretch is never more than the ON duration of the pulse.** The constant was tuned against the serial-carrying app, whose pulses are wide envelopes, and where 200 cycles is a small additive correction. A flat 200 cycles inverts the waveform of the card-data app outright. A measured 7-on and 205-off pattern arrives as 207-on and 5-off, and 272 gaps collapse to exactly 0 cycles, which merges pulse pairs and the bits that their gaps encode. A tail that is longer than its own pulse is not a tail. The limit has no effect on the app that supplied the tuning, because its sync pulse is approximately 4068 cycles wide.
- **`ir_t::tx_last_edge_cycles` keeps the TX timestamps monotonic.** A stretched falling edge can never land after the rising edge that follows it, even for a pulse that is shorter than the stretch. A zero-length gap is already unrecoverable, thus this guard alone is not sufficient. The cap above is the part that keeps a gap nonzero.

At 200 cycles, the sync pulse of the serial-carrying app measures 4268 against the acceptance window of that app, which is 4200 to 5400. The shortest real gap keeps more than 3 times the debounce margin. A sweep of the constant across 0, 100, 200, 300, and 380 changes only that sync measurement. It does not change whether a transfer completes.

### Timing measurements on real hardware

`pk_timing_bench`, the homebrew app of this project, measured these values on a real retail unit. Each one is a present fact about the hardware, and each one is independent of the emulator:

| Screen | Measurement | Result |
|---|---|---|
| 6 | The period behavior of a timer | Real hardware uses P+1 ticks for each period, and not P ticks. `timer.c` has the correction. |
| 7 | The cost of interrupt entry and dispatch | Approximately 98 raw cycles, against approximately 96 cycles in this emulator. |
| 8 | The latency from expiry to the arm operation, with a minimal handler, over IRQ | 0 ticks. |
| 9 | The MMIO write cost of `IRDA_DATA` | Identical to this emulator. The test gives `0x2BF2`, and the control gives `0x2849`. |
| 10 | The same latency as screen 8, over FIQ | 0 ticks. Raw values `0x0FE4` and `0x03F8`. |
| 11 | The same latency, with the full realistic dispatch of the real transmit handler, over FIQ | 0 ticks. The same raw values as screen 10. |

**Timer2 reloads in hardware at the moment that it expires, independently of the time when software services the interrupt.** Screens 8, 10, and 11 give the same 0-tick result across a dispatch that goes from two instructions to a full sequence of nested calls with an ARM-to-Thumb transition. An arm write must only occur before the *next* natural expiry. At a period of approximately 1017 ticks, against a dispatch of much less than 200 ticks, that write always occurs in time with a large margin.

**One real cost error in this emulator remains open.** The same dispatch sequence costs 128 to 160 Timer2 ticks here, and `tools/ir_probe.c` measured that cost directly over 658 real writes from an actual transmit burst. Real hardware gives 0 extra ticks for the same shape of dispatch. Thus this emulator applies too much cost to a dispatch sequence that has nested calls and an ARM-to-Thumb transition. The 32-cycle sample resolution of Timer0 limits the accuracy of that measurement.

**The `184` constant of the real app is not a compensation for dispatch latency.** A disassembly of the transmit state machine gives the mechanism. The period of Timer2 does not encode one pulse. It gives the rate of a repeating quantum clock. Each FIQ reads one bit from a data buffer, at `field+0x14`, from the MSB. It then holds the LED for 1 quantum for a 0 bit, or for 2 quanta for a 1 bit, and a gap of 1 quantum follows each pulse. `unit` (`field+0x20`, which is 1200) is not the quantum length. It appears only in the sync-pulse formula of the receive side, which is `4 x unit +/- unit/2`. The meaning of the subtraction that gives the armed value of 1016 is still unknown.

### The receive state machine of the real app

A disassembly gives these states, at `field+0x28`. `tools/ir_probe.c` uses them to judge a transfer:

- State 1 waits for the first qualifying edge.
- State 2 measures the sync burst and tests it. The sync signal is 4 sequential ON quanta.
- States 3 and 4 alternate, to measure each gap and each pulse. State 3 accepts `|m + 224 - unit| <= unit/2`. State 4 decodes `|m - 224 - unit| <= unit/2` as a 0 bit, and `|m - 224 - 2 x unit| <= unit/2` as a 1 bit.
- State 6 occurs at the end of the transfer.

`field+0x14` is a table of buffer pointers, and it is not a buffer. The receive bit-store code calculates its target as `table[(field+0x27) - 1]`. A byte comparison against `field+0x14` compares pointers and adjacent state, and not message content.

**A buffer comparison cannot prove that a transfer is successful.** Three properties of the app defeat that method. Both instances execute the same app from the same save file, thus their buffers are identical before the relay carries one edge. The app clears the buffers and uses them again immediately after a transfer, thus a read at the end of a run finds only zeros. And `field+0x14` is a pointer table. The test that operates correctly gives the two instances **different hardware IDs**. A real IR message contains the ID of the sender, and neither instance can get the ID of the other instance by any path except the link.

### Verified transfers

`tools/ir_probe.c` verifies both encodings with two instances in one process.

**The serial-carrying app** completes a full bidirectional exchange. In a run with `0xAA1111AA` for instance A and `0xBB2222BB` for instance B, each instance holds the ID of the other instance at `0x0000034C`. The relay carries 980 edges from A to B, and 658 edges from B to A. The same run with no button input reports that neither ID is present, and that there are zero edges. Thus the test has a working negative control.

**The card-data app** transfers a card image. The relay carries 13060 edges from the sender to the receiver, and a reply of 398 edges. Each of the 6529 rise-to-rise gaps is exactly 1 or 2 slot widths, and no gap is unclear. Thus the modulation continues through the relay with no change. A decode of those gaps as pulse-distance data gives an 816-byte message, where 1 slot is a 0 bit and 2 slots is a 1 bit, which agrees with the transmit ISR at `0x020018D8`. The message is a `0x0000000A` header, a table of 199 `u32` values, and a trailer. The reply decodes to `FFFFFFFF`, and then five `0x0000007F` words. The screen of the receiving instance shows a received card image that a control run with a silent sender never shows.

**A relay needs a playout delay.** A relay that passes each edge timestamp without a change gives the receiver a group of edges whose timestamps are already in its past. The receiver releases each edge at one time, and each interval that it measures becomes zero. `tools/ir_probe.c` uses `IR_LINK_PLAYOUT_DELAY_US` as its default for this reason. With that default, a transfer is successful at each tested relay resolution, from 4 cycles to a full frame of 33000 cycles.

**A small relay period costs timing accuracy.** The loop of `psemu_run` always executes at least one instruction. When the real duration of one instruction is more than the full period budget, that call uses much more time than the budget. At `slice_cycles=4`, the budget is 3.79us, and one instruction at the approximately 254KHz rate of a slow instance takes approximately 11.8us. Thus two instances at different `CLK_MODE` speeds diverge by approximately 957000 cycles at that period. The divergence falls to approximately 78600 cycles at a period of 64, to approximately 20700 cycles at a period of 256, and to approximately 8200 cycles at a period of 1024. At a period of 1024, the total elapsed time is exact. This is a property of the diagnostic harness only. The desktop frontend relays one time for each frame, and it never uses a period of this size.

### The IR link between two processes

`frontends/desktop/ir_link.h` and `ir_link.c` hold the two-*process* part of this function, for Windows only. They relay edges between two independent `pokketstation.exe` instances, on a local named pipe. The `IR Link` menu has Host Session, Connect, and Disconnect. A test confirms that the link operates between two real `pokketstation.exe` instances. That test is the only important test here, because no single-process test can find the three properties below.

**The transport uses absolute host wall-clock microseconds**, from `GetSystemTimePreciseAsFileTime`. Nothing synchronizes the two IR clocks of the processes, and both processes operate on the same machine, thus each one can read that same wall clock with no coordination. Each process converts to and from its own local IR timeline only when an edge crosses the pipe.

Three properties of that conversion are necessary, and each one is a separate requirement:

- **The transport must drain its queues.** `pump_connected` runs one time for each rendered frame. A real burst makes approximately 65 edges for each frame. Both directions drain the queue until the pipe is empty or full, and the queue and the pipe buffers each hold a full message. `IR_LINK_WRITE_QUEUE_CAPACITY` is 4096. A discarded edge is unrecoverable, because the space between edges holds the data.
- **The wall-to-core offset must stay constant in one message.** The timestamp of an edge records the time of its creation. A new sample of the offset at a later time mixes two different moments. Emulated time and wall time never advance at the same rate, thus edges from different frames get different shifts. That difference changes the space between the edges, and that space encodes each bit.
- **The offset must be latched again between messages.** Each instance advances its emulated clock by exactly one frame of cycles for each rendered frame, but a real frame takes more wall-clock time than that, and the extra time is different for each process. A permanent offset lets that drift accumulate with no limit. A measurement between two real processes gives the result: one side received *each* arriving edge approximately 200ms in its own past, released all 658 edges at one time, and decoded nothing. `IR_LINK_OFFSET_RELATCH_IDLE_US` is 250000us, thus the code latches the offset again only after the link is quiet for long enough that no message can be in transit.

Two endpoints in one process drift by the *same* quantity, thus their drift cancels. That is the exact reason that a single-process test can pass while two real windows fail.

`IR_LINK_PLAYOUT_DELAY_US` is 250000us. The drift across one message uses approximately 85ms of the margin, and the measured margin that remains is approximately 174ms. An earlier value of 100000us left approximately 16ms, which is too small on a slower or busier machine.

`ir_link_t` holds counters that always operate. Those counters give the edges that the link sent, received, discarded, and received too late for correct placement. The connected status line reports them in the window title. Those counters separate three conditions that look the same without them: "the peer sent nothing", "this instance discarded the data", and "the data arrived too late for use". No other method finds the third condition, because the link then appears to operate correctly and it decodes nothing.

`psemu_reset` and `psemu_load_state` both clear the IR clock and both edge queues. Thus either operation closes an active link, and the two instances cannot lose synchronization without a message.

### Unconfirmed and inferred

The comments in `ir.h` mark these items the same way as each other unconfirmed fact in this document.

- The `BFLT` debounce window of approximately 2 carrier periods. No source records this value, and there is no real-hardware measurement for a comparison.
- The active-low polarity of the receive line. A disassembly of a real receive handler supports it, and real IR demodulator receivers behave the same way, but no real-hardware measurement confirms it directly.
- The receive function of `IRDA_DATA` bit 0 at all. No available documentation gives it. Only the transmit function ("LED") has documentation.
- `IR_TX_FALL_STRETCH_CYCLES`, which is a concession and not a model of a physical effect. See above.
- `IRDA_MISC` and offset `+0x8`. Each source calls this range unknown or reserved. This emulator gives it no behavior, and it makes no assumption about behavior that no source records.

Across more than 200 million traced instructions in this project, no BIOS code accessed these registers. There is no second real PocketStation for a validation of a model of the protocol.

Real IR pulse-length measurement, which reads the live counter of Timer2 from the `INT_IRDA` handler, needs no special code here. Timer2 already ticks independently at each step, for each IR state. Thus a real ISR that reads it during an `INT_IRDA` handler gets a correct value, and `ir.c` and `timer.c` need no connection.

## CLK_MODE

`0x0B000000`: bits 0 to 3 are an index into a CPU-frequency table with 16 entries. These are the exact recorded `PMFrequency` and `SetCpuSpeed` values. They are not a sequence of doubled values. Examples: mode 1 is 63488 Hz, and not 65536 Hz; mode 7 is 3997696 Hz, and not 4194304 Hz; mode 8 is 7995392 Hz, and not 8388608 Hz; and the step from mode 5 to mode 6 is approximately 1.97 times, and not 2 times.

Mode 0 is 32.768kHz, and this emulator uses it as the idle default. Modes 9 to 15 use the rate of mode 8. See `core/src/clk.c` for the full table.

A read of `CLK_MODE` also sets a "steady" bit (`0x10`), which shows that the PLL is locked. This bit always reports a stable clock, because a mode change is immediate in this emulator. A read of `CLK control` (`+0x4`) returns that same value. See "CLK control" below.

A real boot trace of 20 million instructions confirms that real firmware never writes `CLK_MODE=0`. The first action of the real BIOS is `CLK_MODE=7`. Each subsequent write uses only mode 7, mode 4, or mode 3.

The available documentation gives mode `00h` as an invalid or reserved setting that stops the hardware. It gives no frequency for that mode. The use of mode 0 as an idle default in this emulator has no bad effect in practice, because real firmware never uses that code path.

**These functions follow `CLK_MODE`:** the total CPU instruction throughput, which is the cycle budget of `psemu_run`, and the count-down rate of the timers.

**These functions do not follow `CLK_MODE`.** They use real elapsed time:

- The RTC, because it uses a separate oscillator.
- The DAC, because the audio resample function of this emulator needs a fixed real-time output rate, for each CPU speed that an app selects.

See `test_clk_mode_scales_run_speed`, `test_timer_scales_with_clk_mode`, `test_clk_mode_keeps_rtc_dac_on_real_time`.

## CLK control (0x0B000004): stop/standby

**Bit 0 of the second `CLK` register halts the CPU, and each other part that this same oscillator clocks, until a button wakes the CPU.** This is the sleep method of a PocketStation.

**Confirmed by a direct test on a real retail unit.** Screen 13 of `pk_timing_bench` makes the store to this register alone, with no other part of the power-down sequence of a real app. It registers an interrupt handler, it arms Timer1 at approximately 8Hz, it unmasks the buttons and Timer1, and it then writes `1` to `0x0B000004`. With a wait of approximately 10 seconds before a button press, the device reported 10 seconds of RTC time across the store, and **zero** Timer1 interrupts. See `pk_timing_bench/VERIFICATION.md`.

That measurement gives two facts:

- **Bit 0 of this register causes the stop.** A different write in the power-down sequence of a real app is not the cause. No quantity of tracing of that app can give this separation, because the app always makes each write together.
- **The timers stop with the CPU.** Timer1 was armed and unmasked for the full ten seconds, and it counted nothing.

**An earlier, independent observation agrees.** A real commercial app, with no user input, makes the screen blank approximately 37 seconds after the last button press, and the device then sleeps. The next button press wakes the device, and the screen returns to its earlier content. In that app, the write is at the end of a clear power-down sequence:

| write | meaning |
|---|---|
| `IOP_STOP = 0x62` | sound and other IOP subsystems off |
| `INTC mask = 0x200` | RTC interrupt disabled |
| `LCD_MODE &= ~0x48` | `DISON` cleared, thus the display is off |
| `CLK control = 1` | this |

A model of that write as a stop reproduces the observed behavior exactly: the framebuffer has zero lit pixels at the stop, the CPU executes nothing until a button press arrives, and the screen returns at the wake.

**This register does not read back the value that software wrote to it.** Screen 13 read `0x17` from `+0x4` after the wake. That value is the `CLK_MODE` value that the app set, which is 7, with the steady bit (`0x10`) ORed in. Thus both words of this register read back as `CLK_MODE`, and `+0x4` has no readable stop status. `clk_read8` in `core/src/clk.c` models this. There is one data point, thus "the two words give the same value at a read" is the simplest explanation, and it is not a proof. A return of the stored control value was definitely incorrect, because no hardware reads back a 1 there.

**To make this write inert is not a safe simplification. It corrupts the app.** The app writes its idle countdown back only *after* its sleep call returns. A CPU that continues to operate executes the short delay loop after the stop. That loop advances the tick of the app, which reads the same expired countdown and calls sleep again. That recursion has no limit, at 28 bytes of stack for each level. The app has 388 bytes before its stack gets to its globals. Its user stack starts at `0x800`, and its globals are at a fixed address of `0x67C`. The recursion overruns the globals. An `STRB` instruction for a record array then writes over a byte of a saved return address on the stack. Thus the `POP {pc}` instruction after it jumps into data. The CPU then continues through memory that is not code, until it meets an unrecognized opcode. That opcode is the visible fault in a crash report, several million instructions after the true cause.

**What stops, and what does not.** The System Clock clocks the Timer (see "Timers"), thus the Timer also stops here, as the measurement above confirms. That behavior is essential, and not incidental. A wake on each asserted interrupt is not a stop at all, because a Timer in operation asserts again in microseconds, and the CPU never pauses. The RTC continues on its own oscillator, which lets a device know the time while it sleeps. The DAC keeps the fixed resample rate of this emulator.

**Whether real hardware clears the stop bit at a wake is not answerable by a read.** There is no readable stop status, as above. The run does show that the CPU started again and continued to operate. Thus the stop does not persist across a wake, whatever the internal implementation is. `clk_clear_stop` clears the bit in this emulator, thus emulated software does not have to clear it.

**One open question remains.** Whether a source that is not a button can wake the device. To answer it, run screen 13 again with `INTC_MASK = 0x1F`, which masks the buttons and leaves a timer active, and find whether the device wakes. No run has met that condition. No app that this project can operate causes it.

See `test_clk_stop_halts_until_a_button_wakes_it`.

## Memory access timing

`arm7tdmi_step` returns the real wait-state cost of the instruction that it executed. It does not return a flat value of 1. The cycle budget of `psemu_run`, and thus the count-down rate of the timers (see "CLK_MODE" above), uses this real per-instruction cost. It does not use a fixed unit for each instruction.

Two cost tables control this behavior. Both come from the "Memory Access Time" reference material, and this project compared both against the raw source document, and not against a rendered page (see below). `memory.c` contains both tables, in `psemu_region_fetch_cycles` and `psemu_region_data_cycles`:

- **The opcode fetch** (`psemu_bus_fetch16` and `psemu_bus_fetch32`, which only the fetch operation of `arm7tdmi_step` uses): WRAM costs 1 cycle, in the ARM state and in the Thumb state. FLASH (`FLASH1` and `FLASH2`, which hold app code) costs 2 cycles in the ARM state, and 1 cycle in the Thumb state. Thus a fetch of Thumb code from flash is faster than a fetch of ARM code. This is a real cost difference, and not only a result of the smaller Thumb encoding. BIOS uses the same rate and the same division as FLASH. A test on real hardware confirms that rate; see below.
- **A data read or write** (each other `psemu_bus_read*` and `psemu_bus_write*` call): WRAM costs 1 cycle. `FLASH_CTRL`, which holds the F_xxx bank-select registers, the F_WAIT registers, and the F_SN registers, also costs 1 cycle. A real hardware test confirms this; see below. Each other region (FLASH, BIOS, VRAM, and I/O) costs 2 cycles. A test confirms that this cost does not change with the access width (8, 16, or 32 bits), and it does not change between a sequential access and a non-sequential access. This emulator applies the cost one time for each logical call, and never one time for each byte.

`arm7tdmi_add_cycles` applies two more kinds of cost, above the per-access cost. A bus access alone does not include these costs. They follow the standard ARM7TDMI instruction-class timing formulas, which have full documentation and are not specific to the PocketStation:

- Internal "I" cycles, which have no bus operation: a register-specified shift, the fixed `+1I` of `LDR` and `LDM`, and the data-dependent extra cycles of `MUL`, `MLA`, `UMULL`, and similar instructions, through the early-termination rule on `Rs`.
- Pipeline-refill fetches, at each instruction that changes the PC: branches, `BX`, a data-processing, `LDR`, or `LDM` instruction with R15 as its target, and exception entry. This emulator models this cost as 2 more opcode fetches at the new PC, in the ARM or Thumb state that applies after the change.

See the calls in `arm_exec.c` and `thumb_exec.c` for the cost of each opcode.

**Confirmed on real retail hardware: the `F_xxx` ports that get the faster 1-cycle data-access rate of WRAM.**

The available table gives only "WRAM (and some F_xxx ports)", and it names no port. This project changed its assumption two times before a real hardware test gave the answer:

1. The first assumption: all of `FLASH_CTRL` is fast. This came only from the wording of the documentation.
2. The second assumption: all of `FLASH_CTRL` is slow. This came from a disassembly of the source of an independent third-party emulator (see below), which puts each `FLASH_CTRL` address on the slow path.
3. Confirmed by real hardware: `FLASH_CTRL` gets the fast (WRAM) rate. `pk_timing_bench`, the homebrew app of this project, executed a loop that reads `FLASH_CTRL+0x100` (`F_BANK_VAL[0]`) 30000 times, immediately before an identical loop that reads WRAM, on a real retail unit. Both loops gave exactly the same elapsed-tick count.

`psemu_region_data_cycles` (`core/src/memory.c`) contains this behavior. The order of trust of this project (see the top of this file) gives real hardware more authority than the earlier assumption: the source of the other emulator is real independent evidence, but real hardware is stronger evidence. See `docs/app-notes.md` for the full real-hardware result.

**Confirmed on real retail hardware: the BIOS opcode-fetch cost follows the ARM and Thumb division of FLASH.**

The "Memory Access Time for Opcode Fetch" table gives numbers for WRAM and for FLASH, but it gives only a `?` for BIOS. Thus the table alone cannot give this value. The kernel executes from BIOS ROM continuously, thus this cost must have a value. This emulator gives BIOS the full rate and division of FLASH: 2 cycles for ARM, and 1 cycle for Thumb. It does not use a flat 2 cycles for both. Two independent lines of evidence give that rate:

1. Real hardware. `pk_timing_bench` measured a real BIOS helper call in the ARM mode, against an identical copy of that helper in WRAM. The BIOS call was approximately 1.2 times slower. That result agrees with a 2-cycle ARM rate for BIOS against a 1-cycle rate for WRAM. The same test in the Thumb mode gave approximately 1.02 times. That result agrees with a Thumb rate for BIOS that is equal to the 1-cycle rate of WRAM, and equal to the Thumb rate of FLASH. The loop control and the timer-read cost reduce each ratio from a pure 2:1 signal. The same app measures the documented 2:1 rate of FLASH as approximately 1.7:1, thus that reduction is a known property of the method, and not a doubt about the result. See screens 1, 3, and 4 in `pk_timing_bench/VERIFICATION.md`.
2. The documentation. Two other parts of the same source document, which have no `?` mark, put BIOS with FLASH and not with WRAM. First, the data-access table, which is a different table from the opcode table, gives `BIOS` a cost of 2 cycles, in the same row as `VIRT/PHYS/XTRA_FLASH`. Second, the bus-width section gives that FLASH and BIOS ROM permit only 16-bit and 32-bit reads, and that RAM permits 8-bit, 16-bit, and 32-bit access.

Both lines of evidence point at the same rate, and the stronger of the two is a direct test on real hardware. Thus this project holds this value as confirmed, in agreement with the order of trust at the top of this file. A future BIOS-disassembly trace can add detail, but this value does not wait for one.

**Two more items come from the same source material. One is a confirmed hazard, and one is a modeling decision:**

- **The recorded restriction that FLASH is "readable only in 16-bit or 32-bit units" is a confirmed real hazard, and not only a note in a document.** `bus_read8_raw` and `flash1_read8` in this emulator (`core/src/memory.c` and `flash.c`) service an 8-bit `LDRB` from FLASH1, and they model no restriction. `pk_timing_bench` read a font table from FLASH1 one byte at a time with `LDRB`, and it got incorrect glyph data on real hardware. Thus the recorded restriction is important in practice. A change to a word-aligned `LDR` and a shift in a register, with no 8-bit bus access to FLASH1, corrected the fault. See `docs/app-notes.md` and "LCD" above. VRAM has a similar word-only restriction, which had no documentation, and the same method found it.
- **This emulator does not model the waitstate-control bits of `F_WAIT1` and `F_WAIT2` as a change to the timing.** The documentation gives `F_WAIT1` as a register that follows the `CLK_MODE` speed band automatically: `00000000h` for modes 0 to 7, and `00000010h` for modes 8 to 15. It gives bit 5 of `F_WAIT2` as a control that changes WRAM and F_xxx between 1 and 2 cycles. The source document marks most of the other bits of `F_WAIT2` as uncertain: "no effect? but that bit is used in some cases!". No real BIOS trace and no real app trace from this project writes this register for waitstate control.

  A model of a register control that nobody has observed in use can record an assumption as a fact. Thus this emulator uses a fixed table, with bit 5 = 0. This agrees with the order of trust of this project, which gives a real trace more authority than documentation (see the top of this file).

**A note for a person who verifies this section again.** This project compared the reference material against its raw source document. It did not use an automatic summary of a rendered page. That summary had removed the BIOS "`?`" note above, and it gave no indication of the removal.

This project also compared the two assumptions above against two other public sources of register data and emulator source code. Neither source gave an answer. One source repeats the same text. The other source stores `F_WAIT1` and `F_WAIT2`, but it never reads them for timing, and it uses a flat rate for each access, with no per-region wait-state model.

A more useful comparison came from a disassembly of an independent third-party PocketStation emulator. Its x86 memory-dispatch routines have separate 8-bit, 16-bit, and 32-bit read and write paths, and separate ARM and Thumb opcode-fetch decoders. They calculate the cost from two accumulator cells, and each call site writes those cells the same way. Thus that emulator uses only a flat two-level model: `addr <= 0x1FFFFFF` (WRAM) costs 1 unit, and each other address costs 2 units. That model is the same for reads, for writes, and for both instruction sets.

That other emulator does not model the recorded ARM and Thumb opcode-fetch difference of FLASH. BIOS and FLASH1 use exactly the same fetch code in both its ARM decoder and its Thumb decoder. This is the reason that it cannot answer the BIOS opcode-fetch question: it makes no ARM and Thumb distinction for any region, and not only for BIOS.

That emulator does give an answer for the position of `FLASH_CTRL` on its two-level axis: each address from `0x06000000` to `0x063FF` uses the same `> 0x1FFFFFF` slow path as FLASH and BIOS. It does not use the fast WRAM path. This project used that result as the best available evidence for a period of time, until a real hardware test (see above) disproved it directly. That result is the assumption of one independent developer, from the same unclear source text. It was useful when no better evidence existed. Real hardware now replaces it for this question.

## DAC / audio

`0x0D800010`: `ctrl` (+0x0, where bit 0 is the enable bit), and `data` (+0x4, where bits 6 to 15 hold a signed 10-bit `DACV` value). This code multiplies the `DACV` value by 64, to give the full `int16` sample range.

Real hardware has no square-wave generator, no noise generator, and no sound DMA channel. Software makes each tone when it writes new `DACV` levels directly to `DAC_DATA`, at audio rates. The Timer1 IRQ usually causes those writes.

`dac_tick` resamples the level that the DAC holds into a ring buffer, with a zero-order hold, at a fixed internal rate. That rate is `PSEMU_AUDIO_SAMPLE_RATE_HZ` (8000Hz), and it is independent of `CLK_MODE`.

`PSEMU_ASSUMED_CPU_HZ` (1,056,000, which is 33000 multiplied by 32) is the reference cycle rate for this conversion. The per-frame budget of `psemu_run` (33000 cycles at a 32Hz refresh rate) uses the same reference rate. If either value changes, change the other value to agree with it.

Audio output also needs bit 5 of `IOP_STOP` and `IOP_START` ("Sound Enable", at `0x0D800004` and `0x0D800008`), in addition to bit 0 of `DAC_CTRL`. Both gates must be open before the output is not silence.

`IOP_STOP` ORs bits into a shared mask, and `IOP_START` ANDs them out. Real code writes these registers with single-byte stores; it does not always use a full 32-bit store. Both registers apply the effect of each byte immediately. They do not wait for a full-word write. See `test_iop_sound_gate_mutes_dac` and `test_iop_stop_start_take_effect_via_single_byte_writes`.

### System sound volume setting (RAM `0x290`)

**The three-level sound setting of the BIOS system menu is one byte of RAM, at `0x290`.** There is no hardware volume control for this setting: the DAC has only an enable bit and a `DACV` level, and the only other audio gate (`IOP` bit 5) has two states. Thus software applies this setting completely, when it scales the `DACV` amplitude that the BIOS writes.

| `0x290` | Menu setting | Beep amplitude |
|---|---|---|
| `0x00` | Loud | `DACV` -500..496 |
| `0x02` | Quiet | `DACV` -125..124 |
| `0x04` | Mute | no `DACV` writes at all |

A press of Up on the sound-setting screen cycles the value: `0x04`, then `0x02`, then `0x00`, and then `0x04` again. BIOS address `0x04002994` writes each such change. That address is the only observed writer.

**The BIOS clears this byte early in its boot sequence, at `0x04000060`, at approximately instruction 696.** Four addresses read the byte: `0x040032F4` and `0x04003020` during the sound initialization, immediately around the `DAC_CTRL` enable at `0x0400304E`; `0x04003910` in the tone loop; and `0x04002BC6` in the menu. Outside that clear at boot, code only reads the byte. The only writer is the menu writer at `0x04002994`. Thus the BIOS does use the byte as existing state, and on real hardware the battery-backed SRAM supplies that state. This project has not established whether real hardware does not clear the byte at a warm boot. This emulator always does a cold boot, which is the same limit as the limit for the date and the time above.

This file first recorded this behavior as "the BIOS never initializes this byte", and that statement was incorrect. This error stays visible here, because it is easy to repeat. The `watch` mode of `volume_probe` finds a write with a comparison of the byte before and after each instruction. It starts from a `psemu_reset`, where each RAM byte is already zero. A clear operation that writes `0x00` over `0x00` gives no difference. Thus 12 million instructions of monitoring reported no writer. If you write a nonzero value first (`volume_probe <bios> boot 04 0`), the clear operation is immediately visible. **Each before-and-after memory watch cannot see a write that stores the value that the location already holds.** Write a value into the location that the code under test does not use.

**Nothing writes this setting to flash.** A byte comparison of the full 128KB card image across all three settings shows no change. The setting is only in RAM.

Between the three menu values, the byte gives a power-of-two attenuation of `DACV`. The values `0`, `1`, `2`, and `3` give approximately the full amplitude, one half, one quarter, and one eighth. The values `5` to `8` continue to divide the amplitude by 2, and a value of `9` or more gives silence. The value `4` does not follow that sequence: it is a special case for full silence, and the menu uses it for Mute. Nobody has disassembled the implementation, thus it can be a shift or a small gain table. A write of `0xFF` gives an amplitude that neither model predicts.

**The result for this emulator:** `psemu_reset` sets each RAM byte to zero, and the BIOS then clears the byte again during its boot sequence. Thus `0x290` reads `0x00`, and the emulated PocketStation is at full volume, unless code holds the byte against both operations.

A frontend cannot hold this byte with a write between frames, which is the method for each other RAM setting here. The full sequence fits in one frame of 33000 cycles: the RAM clear at instruction 696, the sound initialization that reads `0x290` at instruction 14548, and the end of the boot sound at instruction 15405. Thus a frame boundary never occurs between the clear and the read. A write for each frame does make each sound *after* the boot sound silent. That result is exactly the appearance of the original fault: the setting appeared to operate, until a user restarted the device.

`psemu_set_volume_override` holds the byte. It writes the byte, and it makes the byte read-only to emulated code. That protection is one compare on the RAM write path in `bus_write8_raw`, and it has no cost on the read path or the opcode-fetch path. It also writes the byte again at each `psemu_reset`, and thus it does the function of the battery on real hardware. `psemu_clear_volume_override` returns the byte to the BIOS sound menu. `psemu_set_volume` is still the simple write, with no hold. A test against a real boot with `volume_probe <bios> boot <level> 2` confirms this: Mute gives zero `DACV` writes, and the BIOS does not enable `DAC_CTRL`. Quiet gives the recorded range of `-125` to `124`.

A save state keeps the override, with each other value, because `psemu_save_state` copies the full `psemu_t` structure. The lock is in `psemu_bus_t`, thus its addition changed the size of the state data. The quicksave version of the desktop frontend went to 2, to refuse a version-1 file.

`tools/volume_probe.c` found this byte by experiment. A write sweep across the full RAM, over the boot sound, isolated `0x290`. That sweep repeated the sequence from a save state for each candidate byte. `0x290` is the only address that scales the amplitude and leaves the timing and the write count of the sound unchanged. A desktop quicksave on the sound-setting screen then confirmed the three real values. `psemu_bus_read_trace_cb` (`core/src/memory.c`) gave the read addresses. That diagnostic hook reports each bus read with its real PC. A snapshot comparison probe cannot see a read, and the sweep alone was not sufficient to find this data. That hook is compiled in only for the `psemu_trace` library target, and the diagnostic tools link that target in place of `psemu`. The hook is on the busiest path in the emulator, and a measurement showed approximately 20% more time on a fixed workload of 6.5 million instructions, even with the callback set to NULL. Frontends link the usual `psemu` library, and this hook has no effect on them.

Note that `0x290` is in the region that the memory map above calls user RAM (`0x200` to `0x7FF`). The BIOS keeps much of its own state in that region: `0x230`, `0x254`, `0x264`, `0x280` to `0x2A7`, `0x300` to `0x31F`, `0x3F0`, and `0x410`. Thus the kernel and user division does not give the end of the BIOS-owned state. See "Known open questions" for the result of this fact, for an app that writes over the setting.

## Save states

`core/src/state.c` holds the format. `PSEMU_STATE_VERSION` (`core/src/state.h`) is its version.

**One visitor serves the three operations.** `state_visit` walks the machine one field at a time. The mode of the cursor selects measure, write, or read. Thus a write and a read cannot become different from each other. That divergence is the usual fault of a format with two separate functions. It also gives no error at the time of the write.

The format has four required properties. A raw copy of `psemu_t` satisfies none of them:

- **It is portable across targets.** Each integer uses an explicit width and little-endian order. Structure padding and pointer width are not the same on each target, and this project has targets with 32-bit pointers and targets with 64-bit pointers.
- **It holds no BIOS image.** A BIOS dump is not the property of this project. `test_state_holds_no_bios_and_is_smaller_than_the_structure` loads a BIOS of one repeated byte, and it then searches the state for a run of that byte.
- **It carries its own version.** `psemu_load_state` returns `PSEMU_ERR_BAD_FORMAT` for a file that it cannot read. Thus a frontend does not track the layout of `psemu_t`. `QUICKSAVE_VERSION` in the desktop frontend now changes only for the parts of its file that the frontend owns.
- **It holds only the state that machine behavior needs.** The diagnostic trace ring of the CPU is 64KB, and only `psemu_write_crash_report` reads it. A load clears that ring, thus a report after a load covers only the steps after the load.

The size is 218.8KB, against 354.9KB for `sizeof(psemu_t)`.

**The size is the same for each state of the machine.** A ring buffer writes its full capacity, and not only the entries that it holds now. The count travels with it. A fixed size is a requirement: the libretro interface calls `retro_serialize_size` one time and keeps the result, and a frontend measures the state, then changes the machine, and then writes into the buffer of that measurement.

**`real_time_cycle_carry` is the only floating-point field.** The file stores it as a fixed-point fraction of 32 bits, and not as the raw bits of the double. `psemu_run` keeps the value between 0.0 and 1.0, thus 32 bits give more resolution than the reference clock can use.

## Diagnostics

`arm7tdmi_t` keeps a ring buffer of the most recent `(pc, cpsr)` pairs, and an instruction counter that only increases. Each step writes both, for each caller.

`psemu_write_crash_report` and `psemu_cpu_faulted`, which are part of the public interface, give the full register state, the fault opcode with its real fetch address if a fault occurred, and this trace.

The desktop frontend writes a `pokketstation_report_*.log` file with a timestamp. It writes that file automatically at a CPU fault, and also when a user presses the **F12** hotkey. See `test_crash_report_contents`, `test_cpu_faulted_flag`, and `test_faulted_cpu_stops_advancing`.

`psemu_exec_trace_cb` (`core/src/cpu.h`) occurs one time for each executed instruction, and the compiler includes it only for the `psemu_trace` target. It answers a question that the ring buffer above cannot answer: does execution get to a given address at any time in a full run? The ring buffer gives only the last few thousand steps. `IR_PROBE_WATCH_PC` in `tools/ir_probe.c` uses this callback to separate "the app never called its flash-write routine" from "the app called the routine and the write was discarded".

### The test suite must not depend on NDEBUG

**Each test in `tests/` is an `assert()` call. A Release build defines `NDEBUG`, and `NDEBUG` makes each `assert()` call do nothing.** Thus the full suite executed as a few hundred empty operations, and one `printf` call for each test, in each Release configuration. It reported "all cpu tests passed", and it tested nothing. Both release workflows in `.github/workflows/` execute `ctest` in Release. Thus CI was the configuration where this fault was most important.

A test proves this. It is not an inference: an `assert(0)` call at the top of `main` still gave an exit code of 0, and it printed the full passing output, in a Release build.

Each test file now has `#undef NDEBUG` before `#include <assert.h>`. Thus the tests operate in each configuration, and they need no change to the build files. `assert.h` gives the definition of `assert()` at the time of its inclusion, thus the order is important.

One real assertion was hidden by this fault from the day that a person wrote it. `test_clk_stop_halts_until_a_button_wakes_it` asserted that `INT_RTC` is set in `STATUS` after ten seconds of a stopped CPU. That assertion tried to show that the RTC continues to operate while the CPU sleeps. `STATUS` holds the **raw level** of the RTC interrupt line (see "Interrupt controller"), and that line is a square wave. Thus an assertion that the level is high tests only the half of the waveform where the run ended. Ten seconds is a whole number of periods at the rate of 1 transition each second, which the original assertion used. It is also a whole number of periods at the 2Hz rate that a later measurement gave. Thus the level was low, and the test failed in a Debug build, in both conditions. The test now asserts the property that the comment always gave: the clock reads ten seconds later, and one more transition still changes the line and `STATUS`.

## Hardware ID (F_SN)

Each real unit holds a 32-bit serial number in `F_EXTRA` (`FLASH_CTRL+0x300`, 256 bytes). That region holds `F_SN_LO` and `F_SN_HI` (`+0x300` and `+0x302`, which are two 16-bit halves), and `F_CAL` (`+0x308`, the LCD calibration). Real code reads `F_SN_LO` and `F_SN_HI` with two separate 16-bit `LDRH` instructions. It never uses one 32-bit `LDR` instruction. `core/src/flash.c` and `flash.h` contain this behavior, in `flash_get_serial` and `flash_set_serial`. See `test_flash_serial_number_register_access`.

**A read**: a real app uses `SWI 0Ah` (`FlashReadSerial`), or it reads `F_EXTRA` directly.

**A write**: `SWI 0Fh` (`FlashWriteSerial`) operates only on the `061` BIOS revision (see "BIOS and kernel revisions" below). It stops the CPU on the retail `110` revision.

On retail hardware, the only write path that operates is a 3-step NOR-flash unlock sequence, at fixed physical addresses: `F_KEY2` = `0xFFAA`, `F_KEY1` = `0xFF55`, and `F_KEY2` = `0xFFA0`. Writes to physical `FLASH2` offsets `0`, `2`, and `8` come after that sequence. Those writes do not go to `F_EXTRA`, which is the read address.

The available register documentation records this: *"At physical address 08000000h: `[8000000h]=new F_SN_LO value [8000002h]=new F_SN_HI value`"*. This project confirmed the sequence with a disassembly of the flash-write routine of a real homebrew ID editor. It also confirmed that the sequence operates on real retail hardware.

`flash_write8` (`core/src/flash.c`) contains this behavior as a redirection with a condition. It is not an unconditional address alias. Thus this code cannot send another correct write to the wrong destination, if that write goes to one of the same three offsets. The address alone is not a sufficiently safe signal.

An `unlock_step` field in `flash_t` holds the position in the 3-step unlock sequence. It uses only the next key address. This emulator does not validate the values that the code writes. This agrees with the method that this project always uses for these addresses: they are commands, and not data.

While the sequence is armed, a write to physical offset `0`, `2`, or `8` goes to `F_SN_LO`, `F_SN_HI`, or `F_CAL`. It does not go to `flash->data[]`. The armed state continues through the 3 halfword writes of a real header update. It stops at the first write to a different offset.

This work found one implementation problem: the state first advanced at each byte of each key halfword, and not one time for each halfword. The cause is that `psemu_bus_write16`, the same as a real `STRH` instruction, issues two separate 8-bit bus writes. The correction advances the state only at the low byte.

See `test_flash_header_write_via_unlock_sequence`, `test_flash_header_write_requires_unlock_first`, `test_flash_header_write_disarms_after_unrelated_write`, `test_flash_header_write_requires_correct_key_order`.

**A complete test confirms this behavior**, against the real homebrew app, a real BIOS, and a real button sequence (`tools/inspect.c`, with `button_sim=9`). That test moves the digit cursor on the screen to its last position, changes the digit, and confirms the change. The redirection then writes `F_SN` correctly: the value `0x410000D3` becomes `0x410000D4` after one change.

This test also found the FIQ delivery fault in "Interrupt controller" above. The confirmation sound of the homebrew app, after a write, configures Timer2, which is a FIQ source. Before the FIQ correction, that sound was silence. After the correction, the sound plays correctly: `DAC_CTRL` and `DAC_DATA` show continuous activity immediately after the write completes, at the configured rate of Timer2.

### Human-readable ID format

A real PocketStation prints its hardware ID on a sticker below the front cover. That form is one ASCII letter, and then 8 decimal digits. A user reported the value `"A02374684"` from a real unit. The letter is the high byte of `F_SN`. The 8 digits are its low 24 bits, with a maximum of `16777215`, in decimal.

The rank calculation of the serial-carrying app applies this same mask to `F_SN`. It reads the register with `SWI 0Ah`, removes the high byte, and uses the last 3 decimal digits of the remainder as its "ID" statistic. That statistic sets the rank. A disassembly of a real copy of the game confirms this. Public research gives `211` as the best rank. That value is also the day and the month of the Japanese release of that game, which is 2/11.

`psemu_parse_hardware_id` and `psemu_format_hardware_id` (`core/src/psemu.c`) accept and give exactly 8 hex digits (`0-9`, `A-F`, or `a-f`). This form agrees exactly with the form that a real homebrew ID editor shows and changes, and it can hold each value that the hardware permits. A test on real hardware confirms this: a write of `"EEEEEEEE"` continues correctly. The sticker form with a letter prefix cannot hold that value, because `0xEE` is not an ASCII letter.

This parser does not accept the sticker form, as an input or as an output. A hardware-ID string that this app keeps (the `hardware_id=` line in `settings.cfg`) holds the raw value exactly. It hides nothing, and it translates nothing. A converter from the sticker form to the raw value belongs in the desktop app, as a separate function. See `test_hardware_id_string_conversion`.

**The default value is `0x410000D3`, which is `"410000D3"` in hex form.** Its low 24 bits are `211` in decimal. Thus each new save of the serial-carrying app gets the best rank with no user action, for the reasons above. This default is a selection of this emulator. A real unit has an arbitrary serial number from the factory.

This value is outside the usual 128KB card image, thus it needs its own storage. The desktop frontend keeps it in `settings.cfg`, as a string of 8 hex digits, with its other preferences: the BIOS path, the color scheme, the key bindings, and more. The libretro frontend has no such storage, and it always uses the core default.

## BIOS/kernel revisions

Two BIOS and kernel ROM revisions have documentation. ASCII tag strings in the ROM identify them: the Core Kernel Version, at BIOS offset `0x1DFC` (`"C061"` or `"C110"`), and the Japanese GUI Version, at `0x3FFC` (`"J061"` or `"J110"`). Only revision `110` was in a retail unit.

The available data gives the `061` dump as a dump from prototype hardware. That dump does not operate correctly with some games, and it is not a real retail BIOS. The tests of this project use the real `110`-revision BIOS dump.

Both revisions are factory mask-ROM revisions. Neither is a patch that a user or a service center applies. `BIOS_ROM` is true ROM, and not the writable `FLASH` region. No documentation gives an update method, for example a disc-based programmer or a service program.

Revision `110` has corrections against `061`, and it keeps the same SWI dispatch-table addresses. Thus it is a binary-compatible correction revision. This agrees with a `061` revision from before the retail release.

The confirmed difference between the revisions: `SWI 0Fh` (`FlashWriteSerial`) operates only on `061`. On `110`, that vector holds jump opcodes that hold the CPU in a loop with no exit. Thus a call to that SWI on a `110` BIOS stops the device; it does not only fail.

A real homebrew ID editor never calls `SWI 0Fh` in its code. A full scan of its binary for the `SWI #0xF` opcode confirms this: there are zero results. The binary has no BIOS-version test during execution. It never makes the call, because a person wrote it that way from the start.

## Known open questions and unconfirmed behavior

- **The event-screen stop in the serial-carrying app, from a counter overflow: this cause is not fully confirmed.** A real, repeatable CPU fault occurred: a branch to an incorrect address from an old `LR` value, into the second half of a Thumb `BL` instruction. A trace found the cause: a call to the command dispatcher with a value that is out of range (`0x200`). That value comes from a small counter in RAM, at `0x332`. That counter is in the private user-RAM state of the app; it is not a kernel structure. Code decreases that counter one step at a time, and the expected range is `0` to `0x13`.

  This stop occurred only after a long run with pseudo-random button presses from a fixed seed: 1.3 billion instructions. The evidence is good, but the cause is not confirmed. No trace has found the code that increases the counter. Thus this question is still open: does the counter need faster input than a person can give before it overflows? If it does, the fault is a latent fault in the code of that app, and not in this emulator.

  If a person examines this again: monitor RAM `0x332` for writes, across a new `button_sim=6` run in `tools/inspect.c`. Find whether button input causes the increases, which supports the hypothesis, or whether a timer or a frame tick causes them, which shows a timing-accuracy fault in this emulator.
- **Unconfirmed: whether the serial-carrying app has more sound during use than one launch sound of approximately 17ms.** Across more than 200 million instructions of general automatic exploration, no DAC activity occurred after that one sound. The general button-press exploration of this emulator can be unable to reach the states that permit more sound. An answer in either direction needs real interactive use, with a focus on the later parts of the app.
- **Unconfirmed: whether real hardware has a warm-boot path that does not do the January 1999 clock reset, and the condition that selects that path.** See "Where the date/time settings actually live" above for the three inputs that this project has removed as causes: the RTC contents, the RAM copies, and the card contents. The best remaining candidate is `BATT_CTRL` (`0x0D800020`), which reads 0 in this emulator. A real unit that can tell "the battery is still good" from "a user just replaced the battery" must read that condition from an address, and this emulator always looks like the second condition. If a person examines this again, model `BATT_CTRL` with a nonzero read, and execute the `warm` mode of `tools/datetime_probe.c` again. Note also that the warm runs left `RTC_DATE` at `0x009A0101`. The year byte of that value is not valid BCD. The cold run gives `0x00990101`, which is correct. Thus the `RTC_ADJUST` sequence appears to go past its target when it starts from a year that is not the power-on value. Examine that behavior against the BCD carry code in `rtc.c`, separately from the warm-boot question.
- **Unconfirmed: whether a dispatched app can change the system sound volume setting.** The setting is one byte at RAM `0x290` (see "System sound volume setting" above), and that address is in the `0x200` to `0x7FF` range that the memory map calls user RAM. Outside the clear at boot, and outside its own menu writer, the BIOS never writes that byte. Thus an app that uses that byte as its own scratch memory changes the selection of the user, and gives no message. The setting then stays incorrect until the user sets it again in the menu. Nobody has tested whether a real app writes that byte. The test is to execute the real apps in `testdata/`, and to monitor `0x290` for a write from a FLASH1 PC. The `watch` mode of `tools/volume_probe.c` already reports the writing PC. Write a nonzero value into the byte first, for the reason in "System sound volume setting". This test also gives the quantity of RAM that a battery-backed-SRAM function must save and restore, and the correct time for those operations. Note that `psemu_set_volume_override` prevents this condition, because it makes the byte read-only to emulated code. But no frontend calls that function now: this project removed the Volume Override menu of the desktop app, and replaced it with an output volume at the application level. Thus in normal use, nothing prevents an app write to the byte. The core interface and its tests are still present, and `tools/volume_probe.c` still tests them.
- **IR (`0x0C800000` and above)**: see "IR / IR Link" above for the full model, and for the list of inferred items at the end of that section. The short form: the `BFLT` debounce window, the active-low polarity of the receive line, the receive function of `IRDA_DATA` bit 0, and the behavior of `IRDA_MISC` are all unconfirmed against real hardware. `IR_TX_FALL_STRETCH_CYCLES` is a recorded concession and not a model of a physical effect. No BIOS trace in the test set of this project, which is more than 200 million instructions, accessed these registers. There is no second real PocketStation for a validation of a model of the protocol.
- **Unconfirmed: the meaning of the `184` value in the transmit setup of the serial-carrying app.** That app arms Timer2 with `1200 - 184 = 1016`, where 1200 is its nominal unit. Six separate measurements on real hardware remove each digital cause for a compensation of that size (see "Timing measurements on real hardware" above). Three of those measurements give exactly 0 ticks of dispatch latency, across a handler that goes from two instructions to the full realistic sequence. Thus the value is very probably not a timing compensation. It can be a protocol constant, a different arithmetic relation, or another part of the encoding that this project has not examined. A new disassembly of the setup routine can answer this question.
- **Open: this emulator applies too much cost to a nested dispatch with an ARM-to-Thumb transition.** The transmit-handler dispatch of a real app costs 128 to 160 Timer2 ticks here, and real hardware adds 0 ticks for the same shape (screen 11 of `pk_timing_bench`). `tools/ir_probe.c` measured the emulator value directly, over 658 real writes from an actual transmit burst. The 32-cycle sample resolution of Timer0 limits that measurement. This is a cycle-cost fault in this emulator, and it is separate from the IR model.
- **An `F_BANK_VAL` table that maps more than one physical block to the same virtual slot** has real hardware behavior that no source records. The documentation gives "maybe the data becomes ANDed together". In this condition, `flash_resolve_physical_bank` returns the applicable physical block with the lowest index. That result is a reasonable selection among the unknown behaviors, but it is not a confirmed correct model.
- **The behavior of `FLASH_CTRL+0`, where bit 0 always reads back as 1,** is necessary in practice: it permits a real BIOS busy-wait loop to complete. It does not agree with the recorded `GENREM` and `FLASHVIR` bit functions of the `REGRemap` register. Do not assume that this behavior agrees with a specification.
- **This emulator does not model the sequence where the BIOS appears at address 0, and then the `GENREM` remap operation occurs.** In that sequence, BIOS ROM has an alias at `0x00000000`, until the kernel maps RAM at that address. `psemu_reset` starts the CPU directly at `PSEMU_BIOS_BASE`, and it does not do the pre-remap phase. This is a deliberate simplification, and not an error. It has shown no cost to the behavior, across 150 million traced instructions.
- **This emulator does not model `BATT_CTRL`** (`0x0D800020`, the low-voltage detection register). Reads return 0, and writes have no effect. No emulator behavior depends on the battery level, because this emulator has no battery.
- **The cycle timing of each instruction follows the recorded memory-access-time table, and the standard ARM7TDMI instruction-class formulas** (see "Memory access timing" above). It does not use an approximation of 1 cycle for each instruction. A test on real hardware confirms the two values that the source documentation left unclear: `FLASH_CTRL` gets the faster data rate of WRAM, and the BIOS opcode fetch follows the full ARM and Thumb division of FLASH (see "Memory access timing"). One item in this area is still unconfirmed. This emulator does not model the waitstate-control bits of `F_WAIT1` and `F_WAIT2` as a change to the timing, because no real trace shows a write to those bits for that purpose.

## A note about licensing

The license of this project is GPLv3 ([LICENSE](../LICENSE)). A GPLv3 project can reference BSD-3 code, or use BSD-3 code with attribution. That direction is compatible.

Do not copy code from another open-source implementation that has no license. Do not use other closed-source documentation for more than documentation-level facts. No other implementation makes its code available.

# PocketStation application notes

This file gives the data that you need **to write PocketStation applications**. It covers three subjects. The first subject is the container format. The second subject is the method that the real BIOS uses to find and start an app. The third subject is the method that the app-selection screen uses to show the title and the icon of an app. [hardware-notes.md](hardware-notes.md) gives the CPU and peripheral implementation of this emulator. This file will become reference material for a future PocketStation development kit.

This file uses the same order of trust as hardware-notes.md:

1. A direct test on real hardware.
2. A trace of real BIOS execution and real app execution, with `tools/inspect.c`, against real dumps. This repository does not contain those dumps. See `testdata/`, which .gitignore excludes.
3. The available register documentation.

Where the BIOS revision is important, this file identifies each fact that a test against the real `110`-revision retail BIOS (`J110.bin` or `C110`) confirms.

**[`pk_timing_bench/`](../pk_timing_bench/)**, in the top-level directory, is a working example of each item in this file. It is a homebrew memory-timing benchmark app, with its full build source and a report of its real-hardware results. The build and the diagnosis of that app on real retail hardware found several facts below: the VRAM and FLASH access width, and the Timer0 wraparound.

## The app file format: the PSX Title Sector

A PocketStation app is not a GME file. GME is a full-card dump format (`.gme`). You must remove its container header, to give the raw 128KB, before this emulator loads it.

The real container is the **PSX Title Sector**. It has a header of 80 bytes, and then the icon data, an optional snapshot, a function table, and the executable body:

- Offset `0x00` to `0x4F`: the standard PS1 memory-card title and icon header fields.
- Offset `0x50` to `0x5F`: the PocketStation fields. These are the icon frame counts, an `"MCX0"` or `"MCX1"` identifier at `0x52`, and the entry point with a THUMB-mode flag (bit 0) at `0x5C`.
- If the identifier is `MCX1`, a snapshot of `0x800` bytes comes next. That snapshot holds saved ARM register state and RAM state. The function table and the executable body come after it. If the identifier is `MCX0`, there is no snapshot.

A PocketStation app that a tool exports as one PS1 save is this same Title Sector body, with one real PS1 directory frame in front of it. Most PS1 save-management tools export these files with the `.mcs` extension. The directory frame is `0x80` bytes. It holds an in-use marker, a little-endian data size at `0x04`, and link and file-name fields.

`psemu_load_mcs` removes that frame, validates the size against the frame, and then loads the Title Sector the same way as `psemu_load_app`. Both frontends try `.pss` first, and then `.mcs`. The actual extension of the file has no effect.

**A PocketStation app is frequently a multi-block linked memory-card file**, together with the usual PS1 save of the game. It is frequently not one 8KB block alone.

A simple scan of each block for the `MCX0` or `MCX1` magic number finds the correct first block, but it does not find the other blocks, and it gives no error. To find the full app, follow the directory of the card, which is block 0. Each of the 15 data-block frames is 128 bytes, with these fields:

- The allocation state, at offset `0x00`: `0x51` is the first or only block, `0x52` is a middle block, `0x53` is the last block, and `0xA0` is a free block.
- The total file size, at `0x04`.
- A link to the next block, at `0x08`. That link is a little-endian `u16`, and it is 0-based among the 15 data blocks. Add 1 to get the physical block number. The value `0xFFFF` is the end of the chain.

## App selection and dispatch

Reverse engineering of a real BIOS disassembly gives this sequence:

1. A helper function reads the index of the selected app slot from RAM. It uses `*(u16*)0x000000D0` if that value is not zero. If it is zero, the helper uses `*(u8*)0x000000CE`.
2. **`FLASH2` holds exactly the same directory structure as a PS1 memory card.** The kernel reads the directory frame at `FLASH2 + slot*128`. It then follows the chain with the next-block link of each frame. This gives a bitmask of each physical block of that app.
3. The kernel writes that bitmask to `F_BANK_FLG`, and it then commits the value with a write of `2` to `FLASH_CTRL+0`.
4. The kernel reads the entry point from `FLASH2 + slot*8192 + 0x5C`, which is the resolved physical block.
5. The kernel validates the header of the block a second time, with two Thumb helper functions. It reaches those functions through the standard ARM-to-Thumb `add lr,pc,#1 / bx lr` transition sequence. This step decides whether to start the app from its entry point, or to continue from an `MCX1` snapshot.
6. The kernel clears the user RAM (`0x200` to `0x7FF`), changes to User mode, sets the User SP to `0x800`, and branches (`BX`) to the entry point.

**One loaded app (`.pss` or `.mcs`) needs a synthesized directory before this sequence can reach it.** `flash_load_app` builds that directory: a card header, one directory frame for each app block (in a chain that starts at slot 1), and the data of the app from physical block 1.

**Directory frame offset `0x10`, which is byte 6 of the file-name field, must be the ASCII character `'P'`.** Without that byte, the menu-browsing code of the BIOS does not let a user move to the slot or select it. That code is separate from the dispatch routine above, and it executes before that routine. Each other file-name byte can hold any value.

This rule agrees with a real PS1 convention: a save that contains a PocketStation app replaces the mandatory hyphen of its product code with `P`. One example of a product code is `SLUS-00892`. See `core/src/flash.c`, and `test_flash_load_app_synthesizes_directory` in `tests/cpu_test.c`.

**The real button sequence to reach one loaded app from a cold boot**, which hardware tests confirm (see the CPU boot notes in `docs/hardware-notes.md`, and `button_sim==3` in `tools/inspect.c`):

1. Wait for the end of the HELLO, heart, and sound power-on animation.
2. Press **Down** one time, and then **Action**, to go past the date/time screen.
3. Press **Right** one time, to move from the clock screen to the app.
4. Press **Action** to start the app.

A real button press is short, approximately 40ms. A clean press and release is more reliable than many fast presses. The input code of the real BIOS sometimes needs more than one correct attempt to accept a press. That behavior does not show that the sequence is incorrect.

### App exit: the browse screen starts each app that a user still holds

**If an app returns control to the BIOS while `Action` is still asserted, the BIOS starts the app again immediately.** The browse screen acts at the *release* edge, and it starts the app that it already shows. Thus a press that continues after the exit of the app reads as a new press on that same app. To the user, the app appears to restart, and not to exit.

A report on real hardware first gave this condition. `pk_timing_bench` prevents it in its own code: it waits for the release before it exits (see `tools/pk_exit_test.c`). This emulator now reproduces the condition against the browse screen of the real BIOS, with `tools/button_timing_probe.c`, and a real commercial app with an exit screen.

**A commercial app does not always wait for the release**, and one measured app does not wait. `tools/button_timing_probe.c` measures both limits of the resulting window, in emulated real time:

- The browse screen **ignores an Action press of less than approximately 35ms.** That limit is near to the real press of approximately 40ms above. Thus the real BIOS operates normally, and this is not an emulation timing error.
- The exit screen of that app **exits approximately 62ms to 94ms after the confirming press**. The exact time depends on the position of the press in the tick of the app.

Thus a press must be longer than approximately 35ms, and it must end before approximately 62ms. That window is approximately 27ms wide, and a real hardware press of approximately 40ms is in the middle of it. This is the reason that the sequence operates on hardware, and the reason that a long press does not.

**A frontend that samples the buttons one time for each rendered frame can give only whole steps of 31.25ms.** Thus its available press durations are 31ms, 62ms, 94ms, and longer. Only one of those durations is in the window. This is the reason that `BUTTON_MIN_PRESS_FRAMES` in the desktop frontend is 2. It is also the reason that an increase to that value is not a free safety margin. At 2 frames, the exit is correct at each tested timing offset. At 3 frames, it is correct at much less than one half of the offsets. At 4 frames, the app almost always starts again.

If you write an app, wait for the release of `Action` before you exit. The window where this is important is small, but you cannot control the BIOS side of it.

## The method that an app uses to reach the PS1 save on the same card

**Many PocketStation apps have no use alone: their function is an exchange of data with the save of the PS1 game, in a different block of the same memory card.** One trading-card app is an example. This section gives the mechanism, from reverse engineering of the Japanese release of that app (`testdata/`, which .gitignore excludes). The English release has the same code at the same offsets, byte for byte.

The mechanism depends fully on the **`FLASH1` and `FLASH2` division** in "App selection and dispatch" above:

- **`FLASH1` (`0x02000000`) is a banked window. It holds only the blocks of the app.** The kernel builds `F_BANK_FLG` from the directory chain of the app. Thus an app cannot see data outside itself in this window. Body offset 0 is `0x02000000`.
- **`FLASH2` (`0x08000000`) is the full physical card, with no window.** Each block is visible, which includes the directory of the card and the data of each other file.

`FLASH2` is the only path between the two files, and an app uses it directly. There is no BIOS SWI that finds the save of the game. The app follows the directory of the card itself, exactly the way that the kernel does during app dispatch.

In the trading-card app, this routine is at `0x0200225E` to `0x020022A6` (Thumb). It operates this way:

1. Write a "not found" sentinel value of `0x55555555` to RAM `0x6A4` and `0x6A8`.
2. Follow directory frames 1 to 15, at `FLASH2 + 0x80*n`. These are the same 128-byte frames that a PS1 memory card uses.
3. Skip each frame whose allocation state (at offset `0x00`) is not `0x51`. Thus skip each frame that is not the first block of a file.
4. Compare the **file-name field of the frame, at offset `0x0A`**, one halfword at a time, against a null-terminated ASCII key in the app. In this app, the key is at `0x02002354`, and it holds `"BASLUS-01411-YUGIOH"`. That value is the exact file name of the PS1 save of the game in the card directory.
5. If the names agree, calculate the data address of the save as **`0x08000200 + block_index * 0x2000`**. The `0x200` value skips the title and icon header of the PS1 save. That header is an `"SC"` title frame, and a maximum of three icon frames of 128 bytes. Thus the result is the address of the real save data of the game.
6. Store that pointer at RAM `0x69C`, and copy the word at save data `+0x334` into RAM `0x6A0`.

Two details are important if you write an app:

- **The key is the *file name*, and not the product code.** Offset `0x0A` is the start of the file-name field of the directory frame. This agrees with the rule above that byte `0x10` must be `'P'`: `0x0A + 6 = 0x10`. Thus that rule is "byte 6 of the file-name field" in both descriptions.
- **The pair of files is fixed in the code, and the region pair can be unexpected.** The Japanese app, which the directory names `BISLPMP86398-YUGIOH`, looks for the *US* save `BASLUS-01411-YUGIOH`. Both the Japanese card and the English card in `testdata/` hold that same US save. If an app looks for a save that is not on the card, it finds nothing and keeps its sentinel value.

### The write path: reads use the memory map, and writes use the kernel

**An app can read the PS1 save directly through `FLASH2`, but it cannot write the save that way.** A write uses **kernel SWI `0x10`, one frame of 128 bytes at a time**. This difference is the most important fact in this section. It is the reason that the read path is a few `ldr` instructions, and the write path is a syscall with a retry loop.

The write routine of the trading-card app is at `0x020028FA`:

1. Call a gate function at `0x02000C84`, and stop the write if that function returns with the carry flag set.
2. Load the resolved save pointer from RAM `0x69C`, and convert it to a **frame number** with `(pointer >> 7)`, because the frames are 128 bytes. Then add the start frame offset of the caller.
3. For each frame: a maximum of **10 attempts** at `SWI 0x10`, with the frame number in `r0` and a source buffer of 128 bytes in `r1`. The routine tries again while the SWI returns a nonzero value.
4. After a success, advance one frame and 128 source bytes, and repeat for the frame count of the caller.

**No code calls `0x020028FA` at its entry point.** Both of its callers set `r0`, `r1`, and `r2` themselves, push the same registers, and branch into the middle of the routine, at `0x020028FE`. A cross-reference search for the entry address finds nothing. That result appears to show that the routine is unused, and that conclusion is incorrect. Search for `0x020028FE` instead.

The two callers are important, because together they cover the first `0x400` bytes of the save:

- **`0x02002960` writes 7 frames from save frame 0** (`r0=0`, `r2=7`). Thus it writes save data `+0x000` to `+0x37F`, from a fixed buffer. Before the write, it calculates a table of 64 bytes at save data `+0x340` again, one word at a time, with the helper function at `0x02002878`. A completed IR trade calls this routine.
- **`0x0200292C` writes 1 frame at frame offset 7**, which is save data `+0x380` to `+0x3FF`. It also calculates a checksum over `0x6C` bytes first, at `+0x6C` and `+0x7C` of the structure of the caller.

**The gate function at `0x02000C84` is a low-battery test. It is not a confirmation prompt.** It calls `0x020025E8`. The full body of that function is `ldr r0,[0x0A000004]` and then `lsrs r0,r0,#0xb`. Those instructions put **INTC `STATUS` bit 10, `INT_BATTERY`,** into the carry flag (see `core/src/intc.h`). A clear carry flag means that the battery is good, and the write continues. A set carry flag makes the gate set a flag bit, and then wait in a redraw and poll loop until the condition ends. It then returns with the carry flag set, and the write routine stops the write. `INT_BATTERY` is not in `INT_STATUS_MASK`, thus this emulator reads it as 0, and the gate always permits the write. That result agrees with a device that has a good battery, which is the condition that a user wants.

This fact is important because the gate appeared for a long time to be the obstacle between a received transfer and a committed save. It is not that obstacle, and you do not have to find an in-app confirmation to satisfy it.

The J110 kernel handler is at `0x0400126C`. Each step of it must operate correctly, or the write fails and gives no message:

1. `F_WAIT2` (`FLASH_CTRL+0x10`) `= 0x21`, which enables flash writes.
2. The three-step unlock at `0x04001228`: `F_KEY2` (`FLASH2+0x55AA`) `= 0xFFAA`, `F_KEY1` (`FLASH2+0x2A54`) `= 0xFF55`, and `F_KEY2` `= 0xFFA0`. `core/src/flash.c` already models these same key offsets, in this same order, in `unlock_step`.
3. Copy `0x40` halfwords (128 bytes) to **`FLASH2 + frame*128`**.
4. The completion sequence at `0x04001250`: a fixed delay, then a poll of `F_WAIT2` until **bit 2 reads back as set**, and then a clear of `F_WAIT2`. This is exactly the busy-wait that `flash.h` records as corrected. An unmapped read that returns 0 makes this loop continue for an unlimited time.
5. **Verify**: compare 32 words of the destination against the source. Return `0` for success, and `1` for a difference. That return value operates the retry loop of the app.

This emulator models this full path correctly. `test_flash_frame_write_lands_in_a_ps1_save_block`, in `tests/cpu_test.c`, does the sequence directly. It needs no BIOS image and no app image, thus it executes in CI. It confirms that the frame goes to the address where the verify step of the handler looks for it.

### A completed IR trade does write to the PS1 save, on both sides

**The trading-card app writes the save of the PS1 game at the end of a successful card trade, and this emulator sends that write to the card.** A measurement with `tools/ir_probe.c` gives this result, with two instances that operate against each other over the IR relay:

- 7 `SWI 0x10` calls on each side, at `0x0200291A` in the app, through `0x02002960` and then `0x020028FE`.
- 1876 attempted FLASH2 byte writes for each run, and **65 changed bytes in physical block 1 on each card**. Block 1 is `BASLUS-01411-YUGIOH`, which is the PS1 save of the game, and not a block of the app.
- The timing, in emulated frames: the receiver writes the data approximately 4 frames after the end of the transfer, and the sender writes approximately 3 frames after that.

The 65 changed bytes are exactly the result that a trade must give, and the two sides show equivalent changes:

- **Save data `+0x59`: `01` becomes `00` on the sender, and `00` becomes `01` on the receiver.** This is one byte for each card ID, and it holds the number of copies that the save owns. One card left one save and arrived in the other save.
- **Save data `+0x340` to `+0x37F`: 64 bytes change on both sides.** This is the calculated table that `0x02002960` builds again before each write (see above). It changes on both sides, because its source data changed on both sides.

Both resulting cards stay structurally valid: the `"MC"` header is unchanged, the XOR checksum of each directory frame is still correct, and no block outside block 1 changed. A written card loads back into this emulator directly, and it uses the same raw layout as a real card dump.

**The app state, and not the emulator, is what had to be correct here.** Each earlier attempt operated both roles from one shared screen, or from states that had not moved to the send screen and the receive screen of the app. Those runs reported zero flash writes. That result is a true measurement of the incorrect condition, and a reader took it as a missing emulator function. The trading-card app puts the sender and the receiver on *different* screens. Thus a person must arm the two roles from two separately captured states. The `quicksaveA,quicksaveB` form of `ir_probe`, with a comma, is for that purpose. With both sides armed correctly, the write needs no further confirmation press: the app writes the data itself after the trade completes.

To reproduce this result:

```
ir_probe J110.bin card2.mcr "sender.sav,receiver.sav" 33000 300 "fire@20-21" "fire@10-11" flashwatch
```

Here, `sender.sav` is a state on the send screen of the app, and `receiver.sav` is a state on its receive screen. Each state is one Action press from its start. The run reports the block comparison, and it writes the resulting card of each side to `ir_probe_A_card.mcr` and `ir_probe_B_card.mcr`.

Three diagnostics answer three different questions here. A person who uses the incorrect diagnostic loses real time:

- `flashwatch` (`psemu_bus_write_trace_cb`) reports **attempted** writes. A comparison cannot do this: it reports nothing in three conditions, when the app never wrote, when the app wrote the same value as the stored value, and when a layer discarded the write.
- `IR_PROBE_WATCH_PC` (`psemu_exec_trace_cb`, `core/src/cpu.h`) reports whether **execution got to** a given address at any time in a run. This question comes first, because "the app never called its write routine" and "it called the routine and the write was discarded" look the same to a write hook. The trace ring of the CPU cannot answer this question, because a run of sufficient length writes over the ring many times.
- The **block comparison** at the end of the run reports the true changes, against flash in the condition after the setup.

Two incorrect conclusions are recorded here, thus nobody repeats them:

- **A comparison of the loaded file against flash is not a write test.** `psemu_load_mcs` synthesizes a directory of 16 frames, and it moves the data of the save to block 1. `psemu_load_state` then writes over flash again. Both operations occur before the first instruction. Thus a file-against-flash comparison reported thousands of "written" bytes across each block, for an app from a `.mcs` file that wrote nothing. The reference must be flash in the condition after the setup.
- **Not each FLASH_CTRL access during a transfer is a flash write.** Both apps write to `F_WAIT1` (`+0x0C`) and `F_WAIT2` (`+0x10`), from BIOS addresses `0x040017C6` and `0x040017C8`. That routine, whose entry is `0x040017B6`, is different from `FlashReadSerial` at `0x040017A5`. It sets the flash waitstates, then programs `CLK_MODE`, and then polls for the change. Thus it is the **clock-speed change routine**. It executes because both apps change CLK_MODE for the duration of a transfer. The measured change is from 507904Hz to 1998848Hz. A real flash-write routine also polls `F_WAIT2`, thus this region is useful to monitor. But these particular writes are not a save.

**The two apps are not equivalent here, and that difference is important.** The transfers of the serial-carrying app move its own app state, which is in its own blocks. That app still attempts no flash write during a fully verified bidirectional exchange. The trading-card app is the app that writes the save of the PS1 game. Thus use that app for a test of this path, and monitor for a `SWI 0x10` call that reaches FLASH2 block 1.

### How to get the edited card out of the emulator

An edit that stays in the emulator has little use. Three public functions get content out, and all three are in `core/include/psemu/psemu.h`:

- **`psemu_save_flash_image`** copies flash out, in the same raw layout that a `.mcr` file uses. It is the inverse of `psemu_load_flash_image`.
- **`psemu_save_app_image`** copies only the body of the loaded app out. It is the inverse of `psemu_load_app`, and of the part of `psemu_load_mcs` that comes after the directory frame. The body is contiguous at physical block 1, where the final `memcpy` of `flash_load_app` put it. Thus the inverse is a simple copy.
- **`psemu_identify_content`** gives the result that `psemu_load_content` would give for a buffer, and it loads nothing. `psemu_load_content` calls this function. Thus a caller cannot become different from the dispatch logic, which can occur with a separate size check or extension check.
- **`psemu_content_identity_hash`** hashes the identity of a buffer: which app or card the buffer *is*. For a card, it uses the directory file names. For an app, it uses the title-sector metadata. It does not hash the data that the buffer holds. The write-back function makes this necessary, and not only convenient: a frontend that identifies a save state with a hash of the full file finds that each state stops agreeing at the moment that the app saves, because the file is the data that changed. A measurement against the real condition confirms this: a card trade does not change the identity hash of either card, and the two cards still give different hashes.

The desktop frontend writes all three kinds back automatically. See `frontends/desktop/content_writeback.h` for the rules, and `docs/desktop_readme.md` for the user view. These facts are important for other work on this code:

- **This code builds a `.mcs` file again as its own directory frame, and then the app body.** Keep the frame unchanged from the loaded file: it gives the properties of the file (the size, the name, and the link), and not the contents, and an app cannot reach it. Thus the loaded copy is still correct. Do not write the *synthesized* directory from flash there. That directory is the scaffolding of this emulator, and not part of the file.
- **For a `.mcs` or `.pss` file, only the blocks of the app are in the file.** An app can write anywhere in the synthesized card through `FLASH2`, and the file has no space for those bytes. Also compare only the region that the code will write. If you compare a larger region, a single write to the synthesized directory marks the file as changed permanently, and the code writes the file again in a loop.
- **A reset and a save-state load replace all of flash**, with no app write. Those operations are not edits to keep. Use them as a new reference instead. If you do not, a state load writes the card of the state over the file.

`frontends/desktop/content_writeback_selftest.c` covers all three kinds. It also accepts an optional file argument, for a manual round-trip of real content from `testdata/`. Six real files, which are four `.mcs` files of up to 13 blocks and two `.mcr` files, rebuild byte for byte when nothing changes them.

## The browse-screen icon and graphic

**A direct trace of the execution of a real BIOS against a real app gave the full contents of this section, and user tests validated it.** Nobody derived this data from a decode of the header fields and an assumption about the format. The only source of truth is a single-step trace of the real BIOS, and a record of the memory that it accessed.

**The real icon is immediately after the frame data of the standard PS1 save icon**, at `FLASH1_BASE + 0x80 + std_frames*128`, after the kernel banks the app in. `std_frames` is the value 1, 2, or 3 that the standard PS1 icon-flag byte at Title Sector `0x02` encodes. This position is not offset `0x60`, even though `0x60` is the position of the icon data under a standard PS1-save-icon interpretation of the format.

An earlier version of the homebrew test app of this project put its own code immediately after the standard icon. The BIOS read that code back as icon data and gave no error: the raw encoding of its first instruction appeared exactly where a palette or pixel value must be. This is the reason that the icon appeared as incorrect pixels, or as an empty area, in the first several attempts.

A live trace confirms this layout. The BIOS addresses below are for the `110`-revision retail BIOS (`J110.bin`). A comparison against three further real apps confirms this layout: an app with a single-frame icon, an app with a slow 2-frame toggle icon, and an app with a fast 3-frame animated icon (see `testdata/`, which .gitignore excludes):

- **Two different layouts exist.** No confirmed selector bit gives the layout of an app. This project infers the layout from the PocketStation header byte at Title Sector `0x50`. That byte is `0` for both indirect-layout apps that this project examined, and it is equal to the frame count for the one inline-layout app that this project examined. This correlation is a hypothesis from a static comparison of the files. No BIOS trace confirms it, and it is not a confirmed selector.
  - **The indirect layout**, in 2 of the 3 examined apps: a header of 8 bytes is immediately after the standard icon. The `word @ +0x00` value is a flags and count word. The `word @ +0x04` value is an absolute FLASH1 pointer to the bitmap data, and that data can be at any address in FLASH1. One real app points to an address near the end of its own body, which is far from the icon header. That app confirms that the pointer calculation of the BIOS, `(pointer - FLASH1_BASE) / 4`, is a true calculation and permits relocation.
  - **The inline layout**, in one examined app with a 3-frame animated icon: there is no header and no pointer. The bitmap frames are contiguous, immediately after the end of the standard icon data.
- **A comparison of a 1-frame app, a 2-frame app, and a second 1-frame app gives a partial decode of the flags word.** The low 16 bits give the icon frame count on the device: `1` for a static icon, and `2` for the slow toggle icon. The high 16 bits correlate with the animation speed: `1` for the static icon of the 1-frame app, and `0x30` for the slow icon that toggles each 1 to 2 seconds. This value can be a delay for each frame, where a larger value gives a slower animation. But that conclusion is circumstantial: there are only 3 data points, and no BIOS or timer trace confirms the unit. It is not confirmed.
- **At the bitmap address:** one 1bpp bitmap of 128 bytes, 32 by 32 pixels, for each frame, in sequence. For the indirect layout, the flags word gives the frame count. For the inline layout, the frame count of the standard icon gives it. This packing is identical, byte for byte, to the packing of the LCD VRAM: 32 rows of 4 bytes each, where bit 0 is the leftmost pixel of the 8 pixels of each byte, and 1 is black or lit. There is no palette, and there is no 4-bit index. The byte count of 128 is also the byte count of a standard PS1 4bpp 16 by 16 icon bitmap, but that agreement is a coincidence. That coincidence made the incorrect model appear correct for a long time.
- **For a single-frame indirect-layout icon**, the 128 bytes immediately after its one bitmap hold real, nonzero data, in each examined app. Nobody has identified that data. It can be a function table, because the Title Sector format records a "function table" after the icon data. But that conclusion is not confirmed. The icon of this emulator does not change those bytes: it copies them unchanged from a real reference app, and it does not use an assumed value. This does not apply to a multi-frame icon, where that same space holds the next animation frames.

**The standard PS1 title-text field, at Title Sector offset `0x04` to `0x4F`, holds 2-byte Shift-JIS text. The browse screen of the PocketStation appears to make no use of that field.** A correctly SJIS-encoded custom title in that field never appeared on the browse screen, in each tested configuration. A decode of the title of a real app, by the same method, gave a real, readable string. Thus the encoding was correct.

A replacement of only the 32 by 32 bitmap immediately after the standard icon, with no change to the `0x04` title field, gave a fully correct result. The current theory: the standalone LCD browse interface of the PocketStation reads only the icon bitmaps of the device, and a real app draws its own logo or text into those pixels. The standard title field is probably for a memory-card browser on the PS1. This project has not examined that browser.

### How to build a custom icon

1. Draw a 32 by 32, 1-bit image for each frame. Each tool that exports a monochrome bitmap is applicable, or you can generate the image directly. See the stopwatch icon of [`pk_timing_bench/`](../pk_timing_bench/) for a complete example: that code draws shapes into a 32 by 32 array, and then packs 8 pixels into each byte, with bit 0 as the leftmost pixel.
2. Put the packed bitmaps, at 128 bytes for each frame, in one of two positions. For the inline layout, put them immediately after the data of the standard icon. For the indirect layout, put them at the address of a flags-and-pointer header. Use the values of a real app as a template.
3. For the indirect layout: set the low 16 bits of the flags word to the frame count, and set the pointer word to `FLASH1_BASE` plus the offset of your bitmap data. The exact effect of the high 16 bits on the animation timing is not confirmed. Copy the value of a real app with the animation speed that you need, until a BIOS trace gives the rule.
4. For one static icon with 1 frame: copy the 128 bytes after the bitmap from a real reference app. Do not set them to zero, until somebody confirms the true function of that region.

## Memory access width for VRAM and FLASH1: words only

**Both the LCD VRAM (`0x0D000100`) and FLASH1, which holds the app code and data (`0x02000000`), refuse a byte-size access on real hardware.** The model of this emulator permits a byte-size access to both, and it gives no error. This project found this difference with `pk_timing_bench`, its own homebrew timing-benchmark app. That app operated correctly in the emulator, and it failed in two confirmed ways on a real retail unit:

- **An `LDR`-value write to `LCD_MODE` with no read first**, for example `mov r1,#0x40; str r1,[r0]`, made the display blank immediately, and it stopped the device, at the first boot on real hardware. The true value of `LCD_MODE` before the app, and at power-on reset, has no documentation. The default value of this emulator is equal to the value that the app wrote. Thus this fault was not visible in any emulator test. **Always read `LCD_MODE`, change it, and write it back**: read the current value, OR or AND only the bits that you must change, and then write the value back. Never assume that you know its full start value.
- **A byte-size (`LDRB` and `STRB`) read, change, and write into VRAM**, to set one pixel, gave a full lit horizontal row of 32 bits, and not one bit. This occurred only on real hardware. **Always access VRAM as full 32-bit words.** Each row is exactly one word (4 bytes). Thus a set or clear of one pixel is an `ldr`, a change to one bit, and an `str` on the word of that row. It is never a byte-size operation.
- **A byte-size (`LDRB`) read from FLASH1**, which read a font table one byte at a time, gave incorrect data. This occurred only on real hardware. The available data already records this restriction (see "Memory access timing" in `hardware-notes.md`: *"FLASH and BIOS ROM seem to be allowed to be read only in 16bit and 32bit units"*). But it is easy to break this rule accidentally, because the emulator does not enforce it. **Always read FLASH1 in 16-bit units or 32-bit units**: align the address down to a word, use `ldr` or `ldrh`, and then shift and mask in a register to get a smaller value.

None of these three faults occurs in this emulator. `lcd_write8` and `lcd_mode_write8` in `core/src/lcd.c`, and the FLASH1 read path in `core/src/memory.c`, all permit byte-size access, and they model no width restriction. Do not use only emulator tests to find this class of fault. If you write real homebrew, examine each VRAM and FLASH access for an accidental byte-size instruction, before you test on real hardware.

## The Timer0 COUNT register wraps at 0x10000, and not at 0x100000000

A fourth real-hardware fault occurred in the same timing-benchmark app. This fault is in the application logic, and not in a bus-width difference. A measurement loop read the `count` register of Timer0 one time before a long operation, and one time after it. It then calculated the elapsed ticks with a 32-bit subtraction (`before - after`). That method assumes a full 32-bit down-counter that operates continuously.

On real hardware, a sufficiently long loop gave an `after` value that was numerically larger than the `before` value. That result is the usual sign of a counter that wraps past zero and reloads during the measurement. The 32-bit subtraction then gave a large, incorrect "negative" result of `0xFFFFxxxx`, in place of the small, correct positive tick count.

The diagnosis: a direct dump of the raw `before` and `after` `count` values, in place of the calculated difference, on real hardware. Each raw value had its upper 16 bits at zero. A subtraction of the two raw values, modulo 65536, gave the correct expected result exactly. This is strong evidence that the `count` register of Timer0 is a 16-bit register on real hardware (see the "Timers" section of `hardware-notes.md`).

**If you measure a duration longer than a few instructions with a raw timer register, mask your calculated difference to 16 bits.** `AND #0xFFFF` is not a valid ARM immediate value; use `LSL #16` and then `LSR #16`. Thumb needs the same pair of shifts, because it also has no immediate `AND` instruction. This mask has no effect if no wrap occurred, and it is the correction if a wrap did occur.

### Open questions for future development-kit work

- **The high 16 bits of the flags word**, which can be an animation delay for each frame: there are only 3 real data points, with the values `1` and `0x30`. They correlate loosely with a static icon against a slow toggle of 1 to 2 seconds. No BIOS or timer trace confirms the unit or the formula.
- **Whether the Title Sector `0x50` header byte truly selects the indirect layout or the inline layout**, or whether the correlation is a coincidence of a small sample. That sample is 2 indirect apps with `0x50 = 0`, and 1 inline app whose `0x50` value is equal to its frame count. This question needs more real apps, and a BIOS trace of the code that tests this byte.
- **The function of the data immediately after the bitmap of a single-frame indirect-layout icon.** Real, nonzero data is present there in each examined app, but nobody has reverse engineered its structure. It can be function-table entries.
- **The larger GUI and menu subsystem**, which a trace found during the icon work: a glyph writer for each character, at BIOS `0x0400383A`, which uses `char_code - 0x30` as an index into a font table; a general scroll and animation copy function, which reads a RAM buffer at `0x2D0` into VRAM; and state structures at RAM `0x204`, `0x3D0`, and `0x428`. This subsystem is real and large. But only the one path for the icon bitmap has a full trace: the copy loop at BIOS `0x04002f8c` to `0x04002f9a`. The other paths have no trace. Two examples of unknown behavior are the exact conditions for a menu scroll, and whether an app can control the glyph writer.

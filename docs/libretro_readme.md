# Libretro frontend

Libretro core for use with [RetroArch](https://www.retroarch.com/).

## Usage

1. Copy `pokketstation_libretro.dll` (`.so` on Linux, `.dylib` on macOS) into RetroArch's **cores** directory.
2. Copy your BIOS dump, **renamed exactly to `pocketstation.bin`**, into RetroArch's **System** directory (Settings → Directory → System/BIOS Directory).
3. In RetroArch: **Load Core** → select **PokketStation** → **Load Content** → select your app file.

This core loads the same three content types as the desktop app, picked the same way: by content, not by the file's extension.
- A full memory-card image (`.mcr`), if the file's size exactly matches the real flash size.
- Otherwise, a single-save `.mcs` file.
- Failing that, a bare raw PSX Title Sector dump (`.pss`).

A `.gme` whole-card dump does not work directly. It wraps the same 128KB of card data in DexDrive's own container header; strip that header down to the raw 128KB first.

RetroArch's content browser defaults to showing `.mcr`/`.mcs`/`.pss` files for this core, but the extension itself is not checked at load time.

## Saves

Saving is automatic. RetroArch keeps the emulated memory card in a `.srm` file named after the loaded content, in its **Saves** directory (Settings → Directory → Savefiles), and writes it on exit and on its own autosave interval. Nothing needs to be enabled.

**That `.srm` is a memory-card image — byte for byte a `.mcr`.** It is the full 128KB card in exactly the layout a `.mcr` already uses, so you can rename it, load it straight back into this emulator, or open it in any external PS1 memory-card tool. This is how you get an app's work back off the emulator: a PocketStation app's real output is often an edit to the PS1 game's save sitting in another block of the same card — Yu-Gi-Oh Forbidden Memories trades cards into the game's own save this way — and that edit travels with the card.

This holds for all three content types. A `.mcs`/`.pss` runs inside a full card this emulator synthesizes around it, so its `.srm` is a valid one-app card; a memory-card tool will open it and can export the save back out as a `.mcs`.

**If you edit a card outside RetroArch, delete its `.srm` first.** RetroArch copies the `.srm` over the loaded content every time, without checking, so a leftover `.srm` from an older session will silently put the old card back and discard your edit. The core detects this and shows an on-screen warning when the save data holds a different card than the content you loaded — but it still lets the save data win, since refusing it could throw away a real session's progress.

Save states work too, and unlike the `.srm` they capture the whole machine, including the hardware ID and in-progress emulator state. They are not portable between builds of this core, so treat them as a within-session convenience rather than long-term storage; the `.srm` is the durable format.

This differs from the desktop frontend on purpose. That one writes changes back over the original file in place, which suits a desktop app; here the card lives in the `.srm` instead, which is what RetroArch's save handling, autosave, and cloud sync all expect, and which works on platforms where writing next to the content isn't possible.

RetroArch's cheat search (Cheats → Start or Continue Cheat Search) and memory viewers also work, over the PocketStation's 2KB of work RAM.

Controls use RetroArch's standard RetroPad mapping: D-pad for Up/Down/Left/Right, and the **A** button for Fire/Action. Remap these in RetroArch's own input settings, the same as any other core.

The PocketStation's hardware ID (`F_SN`, which sets Final Fantasy VIII Chocobo World's rank) defaults to the best rank, the same as the desktop app, so there is nothing to configure for the one app known to read it. This value is a hardware register rather than card data, so it lives outside the 128KB card image and is not part of the `.srm`. This core has no config-file mechanism, so an edit made during a session — with a homebrew ID-editing app, say — lasts only until the content is unloaded. A save state does preserve it.

Single-app loads (`.pss`/`.mcs`) boot through the real BIOS menu the same way a full memory card does. See [Reaching a single loaded app](../README.md#reaching-a-single-loaded-app) in the main README for the button sequence.

## Building

Follow the general build steps in the [desktop frontend's build guide](desktop_readme.md#building). The core, tools, tests, and this libretro core all build together the same way, with no SDL2/desktop-specific step needed. This core fetches `libretro-common` at configure time, via CMake's `FetchContent`. The first configure needs internet access for that.

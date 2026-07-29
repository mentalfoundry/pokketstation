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

Controls use RetroArch's standard RetroPad mapping: D-pad for Up/Down/Left/Right, and the **A** button for Fire/Action. Remap these in RetroArch's own input settings, the same as any other core.

The PocketStation's hardware ID (`F_SN`, which sets Final Fantasy VIII Chocobo World's rank) defaults to the best rank, the same as the desktop app. This core has no config-file mechanism, so this value does not persist between sessions. You can still edit this value during a session.

Single-app loads (`.pss`/`.mcs`) boot through the real BIOS menu the same way a full memory card does. See [Reaching a single loaded app](../README.md#reaching-a-single-loaded-app) in the main README for the button sequence.

## Building

Follow the general build steps in the [desktop frontend's build guide](desktop_readme.md#building). The core, tools, tests, and this libretro core all build together the same way, with no SDL2/desktop-specific step needed. This core fetches `libretro-common` at configure time, via CMake's `FetchContent`. The first configure needs internet access for that.

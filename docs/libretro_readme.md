# Libretro frontend

This is a libretro core for use with [RetroArch](https://www.retroarch.com/).

## Prebuilt cores

The libretro buildbot makes this core for Android and for the other supported systems. Get
those builds from the [pipelines of the libretro
repository](https://git.libretro.com/libretro/pokketstation/-/pipelines). Select a
successful pipeline, and then download the artifact for your system. To build the core
yourself, see [Building](#building) below.

## Usage

1. Copy `pokketstation_libretro.dll` (`.so` on Linux, `.dylib` on macOS) into the **cores** directory of RetroArch.
2. Copy your BIOS dump into the **System** directory of RetroArch (Settings → Directory → System/BIOS Directory). **Give the file the exact name `pocketstation.bin`.**

   > **The BIOS file name is `pocketstation.bin`, with a `c`. It is not `pokketstation.bin`.**
   > `pocketstation` is the name of the Sony hardware. `pokketstation`, with two `k`
   > letters, is the name of this emulator, and it is the name of the core file in step 1.
   > This core opens `pocketstation.bin` only. With a file of any other name, the content
   > does not load, and RetroArch shows only its usual failure message. Examine the file
   > name first, if the content does not load.

3. In RetroArch, select **Load Core** → **pokketstation** → **Load Content** → your app file.

This core loads the same three content types as the desktop app, and it selects the type the same way: from the content, and not from the file extension.
- A full memory-card image (`.mcr`), if the size of the file is exactly the real flash size.
- If not, a single-save `.mcs` file.
- If not, a raw PSX Title Sector dump (`.pss`).

A `.gme` full-card dump does not load directly. That format holds the same 128KB of card data in a container with its own header. Remove that header first, to give the raw 128KB.

The content browser of RetroArch shows `.mcr`, `.mcs`, and `.pss` files for this core by default. But this core does not test the extension at load time.

## Saves

This core saves automatically. RetroArch keeps the emulated memory card in a `.srm` file. That file has the name of the loaded content, and it is in the **Saves** directory of RetroArch (Settings → Directory → Savefiles). RetroArch writes the file at exit, and at its own autosave interval. You do not have to enable a setting.

**That `.srm` file is a memory-card image. It is a `.mcr` file, byte for byte.** It is the full 128KB card, in exactly the layout that a `.mcr` file uses. Thus you can give it a new name, load it back into this emulator, or open it in an external PS1 memory-card tool. This is the method to get the work of an app out of the emulator. The real output of a PocketStation app is often an edit to the PS1 save of the console game in a different block of the same card. One trading-card app sends cards into the save of its own game in this manner. That edit stays with the card.

This is true for all three content types. A `.mcs` or `.pss` file operates inside a full card that this emulator synthesizes around it. Thus its `.srm` file is a valid card with one app. A memory-card tool opens that file, and it can export the save as a `.mcs` file.

**If you edit a card outside RetroArch, delete its `.srm` file first.** RetroArch copies the `.srm` file over the loaded content each time, and it does no test. Thus an old `.srm` file from an earlier session replaces the new card and discards your edit, with no message. This core finds that condition and shows a warning on the screen, when the save data holds a different card from the loaded content. But this core still lets the save data win, because a refusal can discard the real progress of a session.

Save states also operate. A save state holds the full machine, which includes the hardware ID and the in-progress emulator state. The `.srm` file does not hold those values. A save state is not portable between builds of this core. Thus use a save state only during one session. The `.srm` file is the durable format.

This behavior is different from the desktop frontend, deliberately. That frontend writes the changes back over the original file, which is correct for a desktop app. Here, the card is in the `.srm` file. That is what the save handling, the autosave function, and the cloud sync of RetroArch all expect. It also operates on each platform where a write next to the content is not possible.

The cheat search of RetroArch (Cheats → Start or Continue Cheat Search) and its memory viewers also operate, over the 2KB of work RAM of the PocketStation.

The controls use the standard RetroPad map of RetroArch: the D-pad gives Up, Down, Left, and Right, and the **A** button gives Fire (Action). You can change these controls in the input settings of RetroArch, the same as for each other core.

The hardware ID of the PocketStation is `F_SN`. It sets the rank in the companion app of one console game. This core gives it the best-rank value by default, the same as the desktop app. Thus you do not have to configure a value for the one app that reads it. This value is a hardware register, and not card data. It is outside the 128KB card image, and it is not part of the `.srm` file. This core has no configuration file. Thus an edit during a session, for example with a homebrew ID-editor app, continues only until you unload the content. A save state does keep the value.

A single-app load (`.pss` or `.mcs`) boots through the real BIOS menu, the same way as a full memory card. See [How to reach a single loaded app](../README.md#how-to-reach-a-single-loaded-app) in the main README for the button sequence.

## Building

Follow the general build steps in the [build guide of the desktop frontend](desktop_readme.md#building). The core, the tools, the tests, and this libretro core all build together, the same way. This core needs no SDL2 step and no desktop-specific step. It gets `libretro-common` at configure time, with the `FetchContent` function of CMake. The first configure operation needs internet access for that step.

# Desktop frontend

This is an SDL2 standalone app for Windows.

## Usage

A prebuilt release contains `pokketstation.exe` directly. A build from source puts the file at `build\Debug\pokketstation.exe`. The exact path depends on your `--config` value; see [Building](#building) below. In both conditions, execute:
```
pokketstation.exe [--console|--no-console] <bios.bin> <app-or-card-file>
```
The example below uses a real BIOS dump and a memory-card image. Use your own paths; neither file is part of this project. **This project supplies no PocketStation BIOS.** The BIOS is copyrighted Sony firmware. You must supply your own dump, from real hardware:
```
pokketstation.exe .\bios.bin .\samplememcard.mcr
```
- **The extension of the second file has no effect.** This app selects the loader from the content:
  - If the size of the file is exactly the real flash size, the app loads it as a full memory-card image (`.mcr`). Use the keyboard to move through its real BIOS menu, the same as on real hardware.
  - If not, the app first tries the file as a single-save file with a real PS1 directory frame in front of it (`.mcs`). Most PS1 save managers use this format to export one save, and it is the more frequent of the two formats.
  - If that attempt fails, the app tries the file as a raw PSX Title Sector app dump (`.pss`).

  Tests confirm that both `.mcr` card images and single-app loads operate correctly, against a real BIOS and real app dumps. See [hardware-notes.md](hardware-notes.md).
- **If you start the .exe with a double click**, and give no command line, the app uses the last BIOS path from an earlier run, if one exists (see `settings.cfg` below). If no such path exists, the app uses `bios.bin` next to the executable. For the app or card, the app always looks for `memcard.mcr` next to the executable. The app never keeps that path.
- A start with no BIOS, no app, no card, or an invalid file does not stop the app. The window opens. Use **File > Load BIOS...** or **File > Open App/Card...** to select a file.
- By default, this app operates with no console window. Thus its diagnostic `stderr` output goes to no visible location. Supply `--console` to get a console window, or `--no-console` to prevent one. The app keeps either flag in `settings.cfg` for later starts.
- **Controls:** the arrow keys give Up, Down, Left, and Right. **Z** gives the Fire (Action) button by default. **F12** writes a diagnostic report. You can change each of these keys, and also **Reset** (F8), **Save State** (F5), and **Load State** (F9), from **Tools > Remap Controls...**. If you select a key that a different row uses, that row loses the key; two actions cannot share one key. A row with no key shows "(unbound)", and it does nothing until you give it a key. You can change the window size freely. **View > Native Size (1x)** and **Double Size (2x)** return the window to a known size.
- **Save states:** **File > Save State** and **File > Load State** each give three slots. The keyboard operates the **Quick Slot** only: **F5** saves, and **F9** loads. You can change both keys. Only the menu can reach **Slot 1** and **Slot 2**. Thus an incorrect hotkey cannot write over a state in one of those slots. Each slot is a separate file next to the executable, with the name of the loaded app or card. For a card with the name `mycard.mcr`, the files are `mycard.mcr.sav` for the quick slot, and `mycard.mcr_1.sav` and `mycard.mcr_2.sav` for slots 1 and 2. A state records its source app or card, and it refuses a load onto a different one. That test continues to operate after an app saves to the card; see [Save write-back](#save-write-back). Save states are also **not portable between versions of this emulator**; see the note at the top of the [main README](../README.md).
- **View > Colors** changes the appearance of the LCD: **Classic** (the default, which is a muted LCD ink-on-sage appearance), **Light** (black on white), **Dark** (white on black), or **Advanced Colors...** for each other appearance. A scheme is three colors: the active pixel, the background, and the sprite shadow. Each item above sets all three colors.
- **Advanced Colors...** asks for one color, the screen color (the background). It then calculates the other two colors: they use the same hue, with sufficient contrast for legibility. A live preview shows the three colors together. This dialog also has the **sprite shadows** control. That control adds a small one-row "ghosting" trail, which approximates the slow pixel response of a real passive-matrix LCD. Open **Custom Colors** in that dialog to set all three colors manually, or select **Match to Screen Color** to return to calculated colors. No control in the Custom Colors group calculates a color by itself. Only **Choose Screen Color...** and **Match to Screen Color** write over a color that you set manually.
- Press **F12** at any time to write a diagnostic report to a log file. See [Diagnostic reports](#diagnostic-reports-for-bug-reports) below.
- The hardware ID of the PocketStation is `F_SN`. It sets the rank in the companion app of one PS1 game. This app gives it the best-rank value by default. You can see it and change it from **Tools > Edit Hardware ID...**; see [hardware-notes.md](hardware-notes.md) for the format. This app keeps this setting, and each other setting above, in `settings.cfg` next to the executable. Those settings are the last BIOS path, the hardware ID, the color scheme, the sprite-shadow state, the key bindings, and the `--console` or `--no-console` preference. The app writes each setting at the moment that it changes.
- **Tools > Sound** has two separate items: **Volume**, which sets the output level, and **Speaker**, which sets the character of the sound.
- **Tools > Sound > Volume** sets the output volume of this emulator, from **Mute**, through steps of 10%, to **Full** (the default). It is a usual application volume: it scales the data that this window sends to your sound device, and the emulated PocketStation cannot read it. Thus it operates on each BIOS, it takes effect immediately, and the app keeps it in `settings.cfg`. It is separate from the sound setting of the PocketStation, and the two values multiply. A device that is mute in its own system menu stays silent, at each value of this setting. Only the system menu of the emulated machine sets the volume of that machine, the same as on the real device. This app no longer has a control to hold that value.

  The percentages are **loudness, and not signal level**: 50% must sound one half as loud, 25% one quarter as loud, and so on. Human hearing is logarithmic, thus this is not the same as one half of the sample values. One half of the samples is only -6 dB, and it still sounds approximately two thirds as loud. Each division of the percentage by 2 gives a real 10 dB. Thus 50% is -10 dB, and 10% is -33 dB.
- **Tools > Sound > Speaker** sets the *character* of the output, and not its level. The speaker of a real PocketStation is approximately one centimetre wide, in a plastic shell. It reproduces almost nothing below approximately 1 kHz to 2 kHz. This emulator gives your computer the signal at the terminals of that speaker. A laptop speaker or a desktop speaker reproduces each low frequency that the real device could not produce. That is the reason that the raw output sounds thick and unclear against the hardware. This setting removes the frequencies that the real speaker never produced:

  - **Full Range (raw DAC output)**: no filter. This is exactly the data that the emulated DAC held.
  - **Light**: removes the low-frequency energy, with only a small change to the character of the sound. Use this preset on speakers that are already small, or if the other two presets sound too thin.
  - **PocketStation Speaker** (the default): the device. It removes the low frequencies from approximately 1.1 kHz down, and it keeps the resonant peak of a small transducer.
  - **Tinny**: this preset removes more low-frequency energy than the hardware does. Use it for large speakers, or for a subwoofer, where the default preset still gives more low-frequency output than the device could produce.

  The app keeps this setting in `settings.cfg`, and it takes effect immediately. Like Volume above, it is the output processing of this app. Thus it operates on each BIOS, and the emulated machine cannot read it. **Each of the three filtered settings is approximately 4 dB quieter than Full Range.** This is not a fault to correct: the low frequencies that the filter removes are real energy, and a speaker that cannot produce them is quieter. The real device is also quieter. Increase **Volume** if you need more level.

  A person tuned this setting by ear against the sound of the hardware. Nobody measured it at the speaker of a real unit. Thus it is an approximation of the correct *shape*, and not a hardware-confirmed response.
- **Tools > Date/Time Override** holds the clock setting of the PocketStation at a value that you select. **Default** means that the frontend makes no change, and the system menus of the PocketStation operate normally. **OS Date/Time** follows the clock of your computer continuously. The app keeps this setting in `settings.cfg`.

  Two facts are important. **While the override is active, the clock screen of the PocketStation does not operate.** You can open it and press buttons, but the value does not stay, because the frontend holds it. That behavior is the function of an override. Change the menu item back to **Default** to make the system screen operate again.

  Also, **the override needs a BIOS that this project traced**. This setting is in usual RAM, at an address that a trace of a real BIOS found. No published register map gives it. Thus, on an unknown BIOS revision, the same address has a different function. The menu becomes unavailable in that condition; the app does not write to the address. See [hardware-notes.md](hardware-notes.md) for the method that found the address.

  An override of the date and time also means that the emulated clock continues to advance while the app is closed. That behavior is nearer to a real PocketStation, whose clock operates from the battery at each time.
- **Help > About pokketstation...** shows the version and a link to this repository.
- **IR Link** connects two copies of this app over IR. This is the same as two physical PocketStation units that a user holds together for local multiplayer. See [IR Link](#ir-link) below.
- **Save write-back** keeps the file that you opened in agreement with the changes that an app makes. Thus the progress of an app continues after you close the window. An edit that an app makes to the save of the PS1 game also continues. The app keeps a backup of the original file. See [Save write-back](#save-write-back) below.

A single-app load (`.pss` or `.mcs`) boots through the real BIOS menu, the same way as a full memory card. See [How to reach a single loaded app](../README.md#how-to-reach-a-single-loaded-app) in the main README for the button sequence.

## IR Link

Two separate copies of `pokketstation.exe`, **on the same Windows machine**, can exchange real IR signals. Each copy is an independent emulator, with its own window, its own loaded BIOS and app, and its own save state. IR is the only connection between them, the same as two real PocketStation units.

**To connect two copies:**
1. Start `pokketstation.exe` two times. For example, execute it one time, and then start it again with a double click, or start a second copy from a terminal. You then have two separate windows.
2. In one window, select **IR Link > Host Session**. The title bar shows "IR - Waiting...".
3. In the other window, select **IR Link > Connect**. The title bar shows "IR - Connecting..." until the two copies find each other, which is usually immediate. Both title bars then show "IR - Connected".
4. Use each copy normally. The IR port operations of the loaded app now reach the other copy.
5. **IR Link > Disconnect** ends the session, from either side, at any time.

**Important data:**
- This version does not ask for a pipe name or a session name. Host Session and Connect always use the same known local connection. Only one connected pair can be active on a machine at one time.
- This function operates only between two copies on **one machine**. There is no network or remote play support.
- Three actions end an active IR link automatically: a load of a different BIOS, app, or card; a press of **Reset**; and a **Load State** operation. All three actions reset the IR state of the emulator. Thus a link that stays connected through one of them loses synchronization with the other copy. Connect again through **IR Link > Connect** or **Host Session** after such an action, if you still need the link.
- The IR timing is an inference. No test against real hardware confirms it. Two details in particular are inferences: the quantity of filtering that this emulator applies to a noisy signal, and the data that a receiving app reads during a transfer. No trace of an app in the test set of this project uses IR. If an app operates differently over the IR link than on real hardware, please report that condition. See [hardware-notes.md](hardware-notes.md#ir--ir-link) for the technical detail.

## Save write-back

**When an app saves, this app updates the file that you opened.** This function operates for each kind of file that this app loads: a full card (`.mcr`), one save (`.mcs`), or an app (`.pss`). This app writes each kind back in its own format, which is the format that you opened.

*Each* change is applicable, and not one kind only:

- an app that saves **its own progress** into its own blocks, which is what most apps do; and
- an app that edits the **save of the PS1 game** in a different block of the same memory card. One trading-card app sends cards over the IR link directly into the save of its own game.

Both kinds go to the same storage, thus this app writes both back. Without this function, a completed trade, or several hours of app progress, is lost when you close the window.

- **Before the first change to your file, this app copies the original file next to it, as `<yourfile>.bak`.** Examples are `mycard.mcr.bak` and `myapp.mcs.bak`. This app writes that copy one time, and it never writes over the copy. Thus the copy always holds the file exactly as it was before this emulator changed it. If a fault occurs, that copy is your method to recover: give it the name of the original file.
- This app writes the file approximately one second after the end of a change. It writes the file again when you exit, when you open a different file, and when you press Reset. Each write goes to a temporary file, which then replaces the original file. Thus an interrupted write cannot leave a truncated save.
- **Load State does not write your file.** A save state contains its own copy of the card. Thus a load is not an edit to keep; it becomes the new start point. This app then writes the subsequent changes of the app to the file.
- Over the IR link, each of the two copies writes its own file independently. That behavior is correct: this app saves both sides of a trade.
- This app reports each write on stderr. Execute the app with `--console` to see the reports. A failed write also gives a message, and it does not change the file on disk.

**Save states continue to operate after an app saves.** A state records its source card or app, and it refuses a load onto a different one. That test deliberately uses the *identity* of the card: the file names in its directory, and the title and icon of an app. It does not use the contents of the file. Thus a set of states from before a card trade still loads after the trade, even though the card on disk changed. This app still separates two different cards, and a card that gained or lost a save, and it still refuses those loads.

**For a `.mcs` or `.pss` file, this app saves only the data of the app.** An app that you load alone operates inside a memory card that this emulator builds around it. No other part of that card is in your file. In practice, an app writes only its own data. If an app must reach the save of a PS1 game, load a full `.mcr` card that contains both files.

## Building

### Windows

**Prerequisites:**
- Visual Studio, or the separate [Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio), with the **"Desktop development with C++"** workload. That workload includes the MSVC compiler, and a copy of CMake and Ninja. You do not have to install CMake separately.
- [vcpkg](https://github.com/microsoft/vcpkg), for the SDL2 dependency of this frontend (see below). You do not need vcpkg if you build only the core, the tools, the tests, or the libretro core.

**1. Open the correct terminal.** A usual `cmd`, PowerShell, or Git Bash window does not have the compiler or CMake on its `PATH`. From the Start Menu, find and open **"Developer Command Prompt for VS"**, or "x64 Native Tools Command Prompt for VS". That shortcut prepares the compiler environment automatically. If you cannot find that shortcut, open a usual `cmd` window and execute the two commands below first. Change the version directory if your version is not `18`:
```
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%
```

**2. Move to the repository with `cd`.** If the repository is on a different drive from the current drive of your terminal, `cd` alone does not change the drive. Use `/d`:
```
cd /d D:\path\to\pokketstation
```

**3. Configure and build.** The Visual Studio generator of CMake is a "multi-config" generator: a Debug build and a Release build exist together. Supply the same `--config` value to the build step and the test step:
```
cmake -B build -S .
cmake --build build --config Debug
ctest --test-dir build -C Debug
```
These commands build the core, the tools, the tests, and the libretro core. They do not build this desktop frontend, and they give a warning, unless SDL2 is also available (see below).

### Linux and macOS

```
cmake -B build -S .
cmake --build build
ctest --test-dir build
```

### How to enable this frontend (SDL2)

The build does not include this frontend, and it gives a warning, if it does not find SDL2. The other targets (the core, the tools, and the tests) still build correctly without SDL2. Install SDL2 with vcpkg:
```
vcpkg install sdl2:x64-windows
```
Then **delete your `build/` directory, and configure again from the start**. Give CMake the path of the vcpkg toolchain file. This step is necessary: CMake keeps a "not found" result for SDL2. If you configure again in the existing `build/` directory, the build still fails after you install SDL2:
```
rmdir /s /q build
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Debug
```

## Diagnostic reports (for bug reports)

The desktop app can write a detailed diagnostic report to a log file, in two conditions:

- **Automatically**, the first time that the CPU meets an unrecognized or unimplemented opcode. This condition is always a fault in the emulator; you did not cause it. The emulator does not stop. It stops execution of the CPU, holds the last correct frame, and prints a message with the name of the report. That message is visible only if you started the app with `--console` (see above).
- **When you press F12**, at any time. Use this method for each condition that does not cause a fault but still looks incorrect: incorrect graphics, no sound, an app that does not respond, and similar conditions.

This app writes each report to the current directory, as `pokketstation_report_<timestamp>.log`. One example is `pokketstation_report_20260721_143012.log`. Each report contains:
- The reason for the report (a fault, or a manual F12 press), and the frame number.
- The paths of the BIOS file and the app or card file.
- The total number of executed instructions, the held button state, `CLK_MODE`, and the flash bank-select state (`F_BANK_FLG` and `F_BANK_VAL`).
- Each CPU register, `PC`, and `CPSR`, which includes the ARM or Thumb mode.
- If a fault occurred: the exact unrecognized opcode, and its fetch address.
- The last 8192 executed program counters or less, oldest first, each with an ARM or Thumb label.

**When you file a bug report, attach the applicable `pokketstation_report_*.log` file.** Include a description of your actions immediately before the condition. That trace is usually the difference between a fault that a developer can correct and a fault that a developer cannot correct.

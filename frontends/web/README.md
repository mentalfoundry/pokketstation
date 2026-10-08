# Web build of the core

`build.sh` compiles the core to one JavaScript file with Emscripten. A web page loads that file and
operates the machine with the public API in `core/include/psemu/psemu.h`. This directory holds no
page. The page is part of the site that uses the build.

## Build

Install Emscripten, and put `emcc` on the PATH. Then:

```
sh frontends/web/build.sh path/to/pokketstation-core.js
```

The script compiles the core from git HEAD. It does not compile the working tree.

## Properties of the output

- One file. The WebAssembly module is inside the file, in base64. A host that does not accept a
  `.wasm` file can thus serve it.
- No threads and no file system. The page runs the machine in its animation loop.
- `createPokketstationCore()` returns a promise for the module. The module gives each exported
  `psemu_*` function with a leading underscore, and `HEAPU8` and `HEAP16` for data transfer.

## Operation of a page

A page must operate the machine the same way as `frontends/desktop/main.c`:

1. `psemu_load_bios`, then `psemu_load_content`, then `psemu_reset`.
2. At 32 frames each second: `psemu_set_buttons`, then `psemu_run(ps, 33000)`, then
   `psemu_get_audio_samples`, then draw `psemu_get_framebuffer`.
3. Hold each button press for a minimum of 2 frames. See `BUTTON_MIN_PRESS_FRAMES` in
   `frontends/desktop/main.c` for the limits that the real BIOS gives.

This project gives no PocketStation BIOS. A page must get the BIOS from the user.

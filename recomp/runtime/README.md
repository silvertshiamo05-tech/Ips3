# Bayonetta runtime

The host side of the native Bayonetta port: the Xbox 360 kernel and XAM functions the
recompiled game calls, guest memory and threads, and (to come) graphics, audio and input.

Adapted from [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp)'s runtime,
GPL-3.0 (see `../COPYING`). From it: `src/kernel/` (function bridging, memory, heap, kernel
objects, imports, XAM, file system), `src/cpu/` (guest threads), `src/mutex.h`,
`src/framework.h`, `src/xxHashMap.h`. Changes for Bayonetta:

- `src/main.cpp`: loads the encrypted, LZX-compressed retail XEX directly through
  XenonRecomp's loader (UnleashedRecomp expects a pre-decrypted one) and mounts the game data.
- `src/kernel/imports_bayonetta.cpp` and the end of `src/kernel/xam.cpp`: the 19 imports
  Sonic Unleashed never calls.
- `src/hid/`, `src/apu/`, `src/os/`, `src/user/`: small stand-ins for UnleashedRecomp's SDL
  input, audio, logger and config modules. Input is fed by the host through `hid::SetHostState`.
- Sonic-specific file-system redirects and the mod loader are removed.

Third-party: o1heap (MIT), unordered_dense (MIT).

## Build (Linux, for testing)

```sh
recomp/setup.sh
recomp/bayonetta/recompile.sh /path/to/default.xex
cmake -S recomp/runtime -B recomp/runtime/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C recomp/runtime/build
recomp/runtime/build/bayonetta /path/to/default.xex /path/to/extracted/game
```

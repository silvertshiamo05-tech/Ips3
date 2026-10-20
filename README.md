# Ips3

A PS3 emulator for iPad, built on [ARMSX3](https://github.com/ARMSX2/ARMSX3) (RPCS3 with
ARMSX3's ARM64 work). Ips3 keeps ARMSX3's core and replaces its Android shell
(JNI + Kotlin) with an iOS one: a C bridge, a SwiftUI app, MoltenVK for Vulkan, and patches
for the places where iOS differs from Android and macOS.

The target is the **iPad (10th generation)**: A14 Bionic, 4 GB of RAM. It is sideloaded
through **LiveContainer** and needs **JIT**.

> Status: this is a port in progress. CI builds the core and the app into `Ips3.ipa`, but
> nothing here has been run on a device yet. Expect the first device runs to need fixes.

## Why other iOS ports crash "when loading the PS3 core"

RPCS3 sets up the PS3 memory map from static initialisers, which run the moment the core
library is loaded. On iOS three of the things it does there fail, and each one aborts the
process before any UI can explain why:

| What RPCS3 does at load | Why it fails on iOS | What Ips3 does |
| --- | --- | --- |
| Reserves ~56 GB of address space (4 GB guest memory + mirror, 12 GB executable map, a 32 GB "hook" area, 4 GB of stats, JIT arenas) | An iOS process gets a few GB unless signed with `extended-virtual-addressing`, and even then the space is limited and shared with the dyld cache | Drops the 32 GB hook area (nothing outside `vm.cpp` uses it). ~26 GB remains, which fits once the entitlement is present. The app checks this before loading the core. |
| Creates shared memory with `shm_open()` | Denied by the iOS app sandbox | Mach named memory entries (`mach_make_memory_entry_64` + `vm_map`), which give the same "map one object at several addresses" semantics |
| Allocates JIT memory with `MAP_JIT` and toggles `pthread_jit_write_protect_np` | `MAP_JIT` requires Apple's private `dynamic-codesigning` entitlement; the toggle API does not exist on iOS; RWX pages are refused | JIT pages are plain RW memory kept W^X: the first execution of a page faults, the fault handler makes it RX; a later write faults and makes it RW again. This is allowed once the process is `CS_DEBUGGED`, which is what "JIT enabled" means on iOS. |

The app (not the core) is what launches first. It checks JIT, address space and memory
limit, and only then `dlopen()`s `Ips3Core.framework`. If something is missing you get a
setup screen saying which entitlement or step is missing, not a crash.

## Requirements

- iPad with an A12 or newer chip and 4 GB of RAM or more. Built and tuned for the iPad
  (10th generation). iPadOS 17 or newer.
- **LiveContainer**, signed with these entitlements (SideStore/AltStore can enable them
  when installing LiveContainer):
  - `com.apple.developer.kernel.extended-virtual-addressing` (required)
  - `com.apple.developer.kernel.increased-memory-limit` (strongly recommended on 4 GB devices)
- **JIT**: launch Ips3 from LiveContainer with JIT enabled (StikDebug, SideStore's JIT, or
  JitStreamer). On the A14 the standard debugger-attach method (`CS_DEBUGGED`) is what
  Ips3 expects; newer chips under iOS 26 use a different JIT scheme that is not handled yet.
- Your own PS3 firmware (`PS3UPDAT.PUP` from Sony) and your own game dumps. Nothing
  copyrighted is bundled or downloaded.

The same entitlements are in `app/Ips3.entitlements`, so they apply when Ips3 is
sideloaded directly with SideStore/AltStore instead of LiveContainer.

## Installing

1. Download `Ips3-ipa` from the latest successful run of the **iOS build** workflow
   (Actions tab), unzip it to get `Ips3.ipa`.
2. Add `Ips3.ipa` to LiveContainer.
3. Launch it with JIT. The setup screen lists anything that is still missing.
4. Tap **+ → Import File** and pick `PS3UPDAT.PUP` to install the firmware.
5. Import games: `.pkg` files are installed, `.rap` licences go to `exdata`, and `.iso`
   files or disc folders (the folder that contains `PS3_GAME/`) are copied to
   `Documents/RPCS3/games`. You can also copy them there with the Files app.

## Running on 4 GB

On first launch Ips3 writes a low-memory configuration, sized from the real jetsam limit
(`os_proc_available_memory`), not from the RAM size:

- PPU and SPU on the LLVM recompilers (the only option on ARM64), **1 LLVM compile thread**
  when the budget is under ~3.2 GB, no up-front precompilation of every module.
  Each LLVM compile can take several hundred MB; this is what gets apps killed during a
  game's first boot.
- 100% resolution scale (720p), no MSAA, no color/depth buffer readback, single-threaded
  RSX, 1 shader compiler thread, async shader recompiler without the shader interpreter.
- PPU/SPU caches in `Library/Caches/RPCS3`, so the second boot of a game compiles nothing.

Settings → **Restore 4 GB iPad defaults** puts these back. GPU memory counts as app memory
on Apple silicon, so every step above 100% resolution costs RAM directly.

Expect lighter titles to be the ones that run. The A14 has two performance cores; the PS3
has a PPU with two hardware threads and six SPUs.

## Controls

- On-screen DualShock 3 (sticks, d-pad with diagonals, L1–R3, Select/Start/PS).
- Any MFi / Xbox / DualShock 4 / DualSense controller through GameController, with rumble.
  The first controller shares port 1 with the touch controls and hides them; more
  controllers become players 2, 3, ...
- Menu button (top right) pauses and offers the PS home menu, controls toggle and stop.

## Repository layout

```
core/armsx3                 ARMSX3, pinned (git submodule)
patches/armsx3/             iOS patches applied to the core at build time
ios/core/                   Ips3Core.framework: CMake target, C API and bridge
  include/ips3_core.h       the C API the app uses
  src/ips3_core.cpp         callbacks, Metal frame, virtual pads, install, game list
  src/stubs/hidapi_stub.c   iOS has no HID/USB access; controllers come via GameController
  cmake/ffmpeg.cmake        prebuilt FFmpeg import
app/                        SwiftUI app (XcodeGen project)
scripts/                    build scripts used by CI and locally
.github/workflows/ios.yml   CI: LLVM + FFmpeg for iOS (cached), core, app, IPA
```

### What the core patch changes

- `rpcs3/util/vm_native.cpp`, `rpcs3/util/vm.hpp`: the iOS memory backend described above
  (no `MAP_JIT`, fault-driven W^X with a registry of JIT ranges, Mach memory entries).
- `rpcs3/Emu/Memory/vm.cpp`: no hook area on iOS.
- `Utilities/Thread.cpp`: the W^X flip runs first in the SIGSEGV/SIGBUS handler.
- `Utilities/File.cpp`: config/cache/resource directories come from the app
  (`IPS3_DATA_DIR`, `IPS3_CACHE_DIR`, `IPS3_RESOURCE_DIR`), because under LiveContainer
  `HOME` and the executable path belong to LiveContainer.
- `rpcs3/Emu/RSX/VK/vkutils/metal_layer.mm`: UIKit/CAMetalLayer instead of AppKit/NSView.
- Input: the ARMSX3 virtual pad handler is enabled on iOS; Qt keyboard/mouse code is not built.
- Build: Qt/desktop targets off, OpenAL off (headers only, as on Android), FFmpeg from the
  iOS prebuilt, libusb's null backend, a no-device hidapi, cubeb without CoreServices,
  `pthread_jit_write_protect_np` compiled out.

## Building locally

On a Mac with Xcode (the newest one you have; RPCS3 needs C++23):

```sh
git clone --recursive https://github.com/silvertshiamo05-tech/Ips3.git   # or init submodules after cloning
cd Ips3
brew install ninja ccache xcodegen
scripts/fetch-submodules.sh
scripts/build-llvm-ios.sh      # ~1 h, once
scripts/build-ffmpeg-ios.sh    # ~10 min, once
scripts/fetch-moltenvk.sh
scripts/build-core.sh          # applies patches/armsx3 and builds Ips3Core.framework
scripts/build-app.sh           # builds the app and writes build/Ips3.ipa
```

## Known gaps

- Not yet run on hardware; the W^X fault path, Mach shared memory and MoltenVK presentation
  are the parts most likely to need adjustment once it is.
- No on-screen keyboard dialog (OSK) yet; games that ask for text get nothing.
- No savestate UI (the core supports them).
- RPCN (online) is not wired up.
- Localised strings for RPCS3's own overlays are empty, so a few overlay labels are blank.

## License

GPL-2.0-only, the same as RPCS3 and ARMSX3. Ips3 is not affiliated with Sony, RPCS3 or
ARMSX3.

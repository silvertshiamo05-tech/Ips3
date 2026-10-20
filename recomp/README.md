# Native ports by static recompilation

Ahead-of-time recompilation of Xbox 360 games to C++ with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), as a route to native iOS builds that
need neither JIT nor the extended-virtual-addressing entitlement.

**License:** everything under `recomp/` is GPL-3.0 (`recomp/COPYING`), because the runtime
is adapted from UnleashedRecomp. It is a separate program from the PS3 emulator in the rest of
this repository, which stays GPL-2.0-only like RPCS3.

**No game data lives in this repository.** Every script reads your own copy of the game and
writes everything derived from it (the decrypted executable, jump tables, generated C++) to a
git-ignored `private/` directory.

## Bayonetta (Xbox 360)

```sh
recomp/setup.sh                                  # XenonRecomp + tools, once
recomp/bayonetta/recompile.sh /path/to/default.xex
```

Status on the retail `default.xex` (SHA-1 `3bdd4988...`):

| | |
|---|---|
| XEX decrypt/decompress | works: 13.1 MB `.text`, ~30k `.pdata` functions |
| Jump tables | 1,127, found by `tools/find_switch_tables.py` (stock XenonAnalyse finds 1) |
| Instructions | all translated. `patches/xenonrecomp-bayonetta.patch` adds the 28 opcodes stock XenonRecomp lacked (`bdzf`/`bdzt`/`bdnzt`, update-form loads/stores, `vsel128`, `vpkswss128`, `vcfpuxws128`, `lvehx`, `eqv`, `addc`/`addme`/`subfze`, ...) and makes `bdnzf` test the CR bit it names instead of assuming `eq` |
| Function boundaries | 0 switch cases outside their function, 0 unresolved branch targets (see below) |
| Output | 247 MB of C++ in ~308 files; every file compiles (clang, `-fsyntax-only`) |

What it took beyond the missing instructions, all in the patch or the tools:

- **Function analysis ran over known functions.** A function without a `.pdata` entry was analysed
  past the start of the next one, and the walk then split that function at every switch case.
  Analysis now stops at the next known function.
- **`bdz` chains and shared tails.** The compiler writes small switches as `mtctr; bdz; bdz; ...`
  and shares identical case code between functions. CTR branches and jump-table cases that leave
  their function are now emitted as calls (like other conditional branches), and
  `tools/leaf_function_bounds.py` gives every such target its own entry point, iterating until
  the boundaries stop changing.

`setjmp`/`longjmp` are located (`sub_8258D7B0`/`sub_8258D150`, matched by their register-buffer
layout) and redirected to native versions: 10 and 21 call sites.

## Runtime (`runtime/`)

UnleashedRecomp's kernel layer, with Sonic-specific hooks removed and the parts Bayonetta needs
filled in. On Linux the recompiled game now boots through the C runtime, kernel and XAM setup,
starts its 22 worker threads and audio driver, and runs its own init until it asks for game data
(`shader.dat` from its archives). Without the disc contents it stops there; with them the next
wall is the first GPU call.

```
bayonetta <game.iso | game folder> [save directory] [--xex default.xex]
```

The game can be the disc image itself (`kernel/io/disc_image.cpp` reads XDVDFS at any of the
known partition offsets, XGD2 included), so nothing has to be extracted on a PC.
`IPS3_TRACE=1` logs every kernel call.

Added on top of UnleashedRecomp:

- `kernel/module.cpp`: the XEX header in guest memory, the loader entry
  (`XexExecutableModuleHandle`), and every *data* import (`KeTimeStampBundle`,
  `XboxHardwareInfo`, ...) bound to real storage. XenonRecomp leaves those slots holding the
  import record, so the game read garbage through them.
- `kernel/virtual_memory.cpp`: `NtAllocate/Free/QueryVirtualMemory` (reserve/commit ranges at
  `0x40000000`+), `MmQueryStatistics`, kernel pool.
- `kernel/io/nt_file.cpp`: the NT file API (`NtCreateFile`, `NtReadFile`, `NtQueryDirectoryFile`,
  ...). Bayonetta's CRT calls the kernel directly instead of the Win32-style wrappers UR hooks.
  Paths resolve `game:`, `D:`, `\Device\Cdrom0` and `\??\` links, without regard to case.
- `kernel/rtl.cpp`, `kernel/crt.cpp`: time fields, strings, status codes, `sprintf` family,
  `DbgPrint`.
- Thread fixes: closing a thread handle no longer joins the thread (it deadlocked), suspending
  another thread no longer suspends the caller.

Next: the Xenos -> Metal renderer (`Vd*` and the GPU command stream), XMA audio, input.

## Tools

- `tools/xex_dump.cpp` – loads a XEX with XenonRecomp's loader, prints sections, writes the flat image.
- `tools/find_switch_tables.py` – finds jump tables by executing each switch preamble per case
  value, so it does not depend on the compiler's instruction order. Writes XenonRecomp's TOML.
- `tools/leaf_function_bounds.py` – explicit `functions` entries: leaf functions cut at a jump
  table, `bdz`-chain switches, and entry points for shared tails reached from other functions.
- `tools/make_xiso.py` – builds an XDVDFS image from a folder (optionally at the XGD2 offset) to
  test the disc reader.
- `patches/xenonrecomp-bayonetta.patch` – applied to the pinned XenonRecomp by `setup.sh`.

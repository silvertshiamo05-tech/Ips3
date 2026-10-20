# Native ports by static recompilation

Ahead-of-time recompilation of Xbox 360 games to C++ with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), as a route to native iOS builds that
need neither JIT nor the extended-virtual-addressing entitlement.

**No game data lives in this repository.** Every script reads your own copy of the game and
writes everything derived from it (the decrypted executable, jump tables, generated C++) to a
git-ignored `private/` directory.

## Bayonetta (Xbox 360)

```sh
recomp/setup.sh                                  # XenonRecomp + tools, once
recomp/bayonetta/recompile.sh /path/to/default.xex
```

Status of the feasibility pass on the retail `default.xex`:

| | |
|---|---|
| XEX decrypt/decompress | works: 13.1 MB `.text`, ~30k `.pdata` functions, 77.7k functions after analysis |
| Jump tables | 1,127 found by `tools/find_switch_tables.py` (XenonAnalyse finds 1: its fixed patterns expect a different instruction order than Bayonetta's compiler emits) |
| Function boundaries | `tools/leaf_function_bounds.py` fixes the leaf functions XenonRecomp cuts at a jump table; 2 switch sites left |
| Unimplemented instructions | 8,886 occurrences of 28 opcodes in 2,200 functions (`bdzf`, update-form loads/stores like `lfsu`/`stfsu`/`sthu`, `vsel128`, `eqv`, carry arithmetic `addc`/`addme`/`subfze`, `ldux`/`stdux`, ...). All ordinary PowerPC/VMX128, they need adding to XenonRecomp's translator. |
| Output | 246 MB of C++ |

Next: implement the missing instructions in a XenonRecomp fork, locate setjmp/longjmp, then
start the runtime (Xbox 360 kernel/XAM calls, memory, threads, file system) and the Xenos ->
Metal renderer.

## Tools

- `tools/xex_dump.cpp` – loads a XEX with XenonRecomp's loader, prints sections, writes the flat image.
- `tools/find_switch_tables.py` – finds jump tables by executing each switch preamble per case
  value, so it does not depend on the compiler's instruction order. Writes XenonRecomp's TOML.
- `tools/leaf_function_bounds.py` – explicit `functions` entries for jump tables in functions
  without `.pdata` records.

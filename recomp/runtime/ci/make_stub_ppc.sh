#!/usr/bin/env bash
# Writes a stand-in for the recompiled game (bayonetta/private/ppc) that holds no game code: no
# functions, an empty function table. The runtime then compiles and links in public CI exactly as
# it does with the real code, which is generated on the owner's machine and never committed.
# Usage: make_stub_ppc.sh <out-dir>   (pass <out-dir>/ppc as PPC_DIR)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$1/ppc"
XU="$HERE/../../build/XenonRecomp/XenonUtils"
mkdir -p "$OUT"
cat > "$OUT/ppc_config.h" <<'H'
#pragma once
#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED
#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0x1D50000ull
#define PPC_CODE_BASE 0x821C0000ull
#define PPC_CODE_SIZE 0xD1D2F4ull
#endif
H
{ printf '#pragma once\n#include "ppc_config.h"\n\n'; cat "$XU/ppc_context.h"; } > "$OUT/ppc_context.h"
printf '#pragma once\n\n#include "ppc_config.h"\n#include "ppc_context.h"\n' > "$OUT/ppc_recomp_shared.h"
printf '#include "ppc_recomp_shared.h"\n\nPPCFuncMapping PPCFuncMappings[] = {\n\t{ 0, nullptr }\n};\n' > "$OUT/ppc_func_mapping.cpp"

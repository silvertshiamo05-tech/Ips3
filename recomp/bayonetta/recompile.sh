#!/usr/bin/env bash
# Recompiles Bayonetta (Xbox 360) to C++.
#
#   recomp/bayonetta/recompile.sh /path/to/default.xex
#
# Everything derived from the game -- the decrypted image, jump tables and the generated C++
# -- goes to recomp/bayonetta/private/, which is git-ignored. Never commit it.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TOOLS="$HERE/../tools"
BUILD="$HERE/../build"
PRIVATE="$HERE/private"
XEX="${1:?usage: recompile.sh default.xex}"

[ -x "$BUILD/xex_dump" ] || "$HERE/../setup.sh"

mkdir -p "$PRIVATE/ppc"
[ "$XEX" -ef "$PRIVATE/default.xex" ] || cp "$XEX" "$PRIVATE/default.xex"

"$BUILD/xex_dump" "$PRIVATE/default.xex" "$PRIVATE/image.bin"

# Section bounds of the retail executable (see xex_dump output).
TEXT=821c0000:82ed8090
PDATA=8217c600:821b7fe0

python3 "$TOOLS/find_switch_tables.py" "$PRIVATE/image.bin" "$PRIVATE/switch_tables.toml" --text "$TEXT" --pdata "$PDATA"

recompile() {
	cat "$HERE/bayonetta.toml" > "$PRIVATE/config.toml"
	[ -f "$PRIVATE/functions.toml" ] && cat "$PRIVATE/functions.toml" >> "$PRIVATE/config.toml"
	rm -rf "$PRIVATE/ppc" && mkdir -p "$PRIVATE/ppc"
	(cd "$PRIVATE" && "$BUILD/XenonRecomp/build/XenonRecomp/XenonRecomp" config.toml \
		"$BUILD/XenonRecomp/XenonUtils/ppc_context.h" > recomp.log 2>&1)
}

# The first pass finds function starts; the boundary fix-ups need them. Each fix can expose
# another (a new entry point reaches a new shared tail), so repeat until nothing changes.
rm -f "$PRIVATE/functions.toml"
recompile
for pass in 1 2 3 4 5 6 7 8; do
	before="$(cat "$PRIVATE/functions.toml" 2>/dev/null || true)"
	python3 "$TOOLS/leaf_function_bounds.py" "$PRIVATE/image.bin" "$PRIVATE/switch_tables.toml" \
		"$PRIVATE/ppc/ppc_func_mapping.cpp" "$PDATA" "$PRIVATE/functions.toml"
	if [ "$(cat "$PRIVATE/functions.toml")" = "$before" ]; then
		echo "function boundaries settled after $pass passes"
		break
	fi
	recompile
done

echo
echo "Output: $PRIVATE/ppc ($(du -sh "$PRIVATE/ppc" | cut -f1))"
echo "Switch sites still outside their function: $(grep -o 'Switch case at [0-9A-F]*' "$PRIVATE/recomp.log" | sort -u | wc -l)"
echo "Unimplemented instruction occurrences:   $(grep -c 'Unrecognized instruction' "$PRIVATE/recomp.log")"
echo "Unresolved branch targets:               $(cat "$PRIVATE"/ppc/ppc_recomp.*.cpp | grep -c '// ERROR')"

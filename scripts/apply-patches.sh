#!/usr/bin/env bash
# Applies patches/armsx3/*.patch to the core/armsx3 submodule. Safe to run twice.
set -euo pipefail
source "$(dirname "$0")/common.sh"

for patch in "$ROOT"/patches/armsx3/*.patch; do
	name="$(basename "$patch")"
	if git -C "$CORE" apply --reverse --check "$patch" 2>/dev/null; then
		echo "already applied: $name"
	else
		git -C "$CORE" apply --whitespace=nowarn "$patch"
		echo "applied: $name"
	fi
done

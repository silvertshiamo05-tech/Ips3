#!/usr/bin/env bash
# Picks the newest non-beta Xcode on the runner: RPCS3 needs a recent clang (C++23).
set -euo pipefail

candidates=$(ls -d /Applications/Xcode_*.app 2>/dev/null | grep -vi beta | sort -V || true)
latest=$(echo "$candidates" | tail -n1)

if [ -n "$latest" ]; then
	sudo xcode-select -s "$latest/Contents/Developer"
fi

xcodebuild -version
clang --version | head -n1
xcrun --sdk iphoneos --show-sdk-version

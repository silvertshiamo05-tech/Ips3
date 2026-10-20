#!/usr/bin/env bash
# Builds the SwiftUI app with xcodebuild (unsigned), embeds Ips3Core.framework and packages
# build/Ips3.ipa. LiveContainer / SideStore sign it on import.
set -euo pipefail
source "$(dirname "$0")/common.sh"

APP_DIR="$ROOT/app"
DERIVED="$BUILD/app-derived"

# APP_ONLY=1 compiles the app without the core (the app dlopens the core, so it does not
# need it to build); used by CI to check the Swift side quickly.
fw="$(find "$BUILD/core" -name Ips3Core.framework -type d -maxdepth 4 2>/dev/null | head -n1 || true)"
if [ -z "$fw" ] && [ "${APP_ONLY:-0}" != 1 ]; then
	echo "Ips3Core.framework not found; run scripts/build-core.sh first" >&2
	exit 1
fi

log "Generate Xcode project"
(cd "$APP_DIR" && xcodegen generate --spec project.yml)

log "Build app"
xcodebuild \
	-project "$APP_DIR/Ips3.xcodeproj" \
	-scheme Ips3 \
	-configuration Release \
	-sdk iphoneos \
	-destination 'generic/platform=iOS' \
	-derivedDataPath "$DERIVED" \
	CODE_SIGNING_ALLOWED=NO \
	CODE_SIGNING_REQUIRED=NO \
	CODE_SIGN_IDENTITY="" \
	build | tee "$BUILD/xcodebuild.log" | grep -E "error:|warning: .*Swift|\*\* BUILD" || true

app="$DERIVED/Build/Products/Release-iphoneos/Ips3.app"
if [ ! -d "$app" ]; then
	echo "Ips3.app was not produced" >&2
	grep -E "error:" "$BUILD/xcodebuild.log" | head -n 100 >&2 || true
	exit 1
fi

if [ "${APP_ONLY:-0}" = 1 ]; then
	log "App built (core not embedded)"
	exit 0
fi

log "Embed Ips3Core.framework"
mkdir -p "$app/Frameworks"
rm -rf "$app/Frameworks/Ips3Core.framework"
# ditto keeps the framework's symlinks intact.
ditto "$fw" "$app/Frameworks/Ips3Core.framework"
rm -rf "$app/Frameworks/Ips3Core.framework/Headers" "$app/Frameworks/Ips3Core.framework/Versions/A/Headers"

# Debug info for a statically linked LLVM is most of the binary; the symbol table is kept
# for crash logs.
bin="$app/Frameworks/Ips3Core.framework/Ips3Core"
[ -L "$bin" ] && bin="$(cd "$(dirname "$bin")" && pwd -P)/$(readlink "$bin")"
xcrun strip -S -x "$bin" || true

# iOS frameworks must be flat (no Versions/ symlink layout).
fwdir="$app/Frameworks/Ips3Core.framework"
if [ -d "$fwdir/Versions" ]; then
	tmp="$(mktemp -d)"
	cp -RL "$fwdir/Versions/A/." "$tmp/"
	rm -rf "$fwdir"
	mkdir -p "$fwdir"
	cp -R "$tmp/." "$fwdir/"
	rm -rf "$tmp"
fi
if [ -d "$fwdir/Resources" ]; then
	[ -f "$fwdir/Resources/Info.plist" ] && mv "$fwdir/Resources/Info.plist" "$fwdir/Info.plist"
	rm -rf "$fwdir/Resources"
fi
# Minimum Info.plist keys iOS checks when loading a framework.
/usr/libexec/PlistBuddy -c "Set :CFBundlePackageType FMWK" "$fwdir/Info.plist" 2>/dev/null || \
	/usr/libexec/PlistBuddy -c "Add :CFBundlePackageType string FMWK" "$fwdir/Info.plist"
/usr/libexec/PlistBuddy -c "Add :MinimumOSVersion string $IOS_DEPLOYMENT_TARGET" "$fwdir/Info.plist" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :CFBundleSupportedPlatforms array" -c "Add :CFBundleSupportedPlatforms:0 string iPhoneOS" "$fwdir/Info.plist" 2>/dev/null || true

# Ad-hoc signature so the bundle is well formed; the installer replaces it.
codesign --force --sign - --timestamp=none "$fwdir" || true
codesign --force --sign - --timestamp=none --entitlements "$APP_DIR/Ips3.entitlements" "$app" || true

log "Check the core"
# The app resolves these with dlsym; a missing one means a broken build, not a broken device.
exported="$(xcrun nm -gU "$fwdir/Ips3Core" | grep -c ' _ips3_' || true)"
echo "exported ips3_* symbols: $exported"
[ "$exported" -ge 20 ] || { echo "Ips3Core does not export its C API" >&2; exit 1; }
# Every dependency must be an iOS system library or framework.
if xcrun otool -L "$fwdir/Ips3Core" | tail -n +2 | grep -v -E '^[[:space:]]*(/usr/lib/|/System/Library/|@rpath/Ips3Core)'; then
	echo "Ips3Core links something that is not part of iOS" >&2
	exit 1
fi
xcrun vtool -show-build "$fwdir/Ips3Core" | grep -E "platform|minos" || true

log "Package IPA"
rm -rf "$BUILD/ipa"
mkdir -p "$BUILD/ipa/Payload"
ditto "$app" "$BUILD/ipa/Payload/Ips3.app"
(cd "$BUILD/ipa" && zip -qry "$BUILD/Ips3.ipa" Payload)

ls -la "$BUILD/Ips3.ipa"
du -sh "$app/Frameworks/Ips3Core.framework"

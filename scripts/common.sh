# Shared settings for the build scripts. Sourced, not run.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE="$ROOT/core/armsx3"
BUILD="${BUILD:-$ROOT/build}"

# iPad (10th generation) shipped with iOS 16 and runs everything up to iOS 26. 17.0 is the
# floor because libc++'s std::pmr and a few other C++20/23 library pieces RPCS3 uses are
# only available from there.
IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-17.0}"

LLVM_IOS_DIR="${LLVM_IOS_DIR:-$BUILD/llvm-ios}"
FFMPEG_IOS_DIR="${FFMPEG_IOS_DIR:-$BUILD/ffmpeg-ios}"
MOLTENVK_DIR="${MOLTENVK_DIR:-$BUILD/moltenvk}"

MOLTENVK_VERSION="${MOLTENVK_VERSION:-v1.4.2}"
FFMPEG_VERSION="${FFMPEG_VERSION:-n7.1.2}"

JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"

llvm_commit() {
	git -C "$CORE" ls-tree HEAD 3rdparty/llvm/llvm | awk '{print $3}'
}

log() { printf '\n==> %s\n' "$*"; }

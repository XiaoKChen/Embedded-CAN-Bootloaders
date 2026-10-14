#!/usr/bin/env bash
# =============================================================================
# build.sh - One-command build of the ESP32-S3 CAN updater (Linux / macOS)
# =============================================================================
#   ./build.sh [idf.py arguments]
#
# With no arguments it runs `idf.py build` and prints each binary and its
# flash offset; any arguments are passed to idf.py instead, inside the same
# repo-local environment (e.g. `menuconfig`, or `-p /dev/ttyUSB0 flash`).
#
# First run: clones ESP-IDF (the exact tag in idf-version.txt) into
# .tools/esp-idf, installs its esp32s3 toolchain, CMake, Ninja and Python
# environment into .tools/espressif (IDF_TOOLS_PATH), then builds. Later runs
# reuse .tools and only rebuild. Nothing is written to the user profile: pip
# and ccache caches also live under .tools. CI runs this same script.
#
# Requires only git and Python 3.8-3.13 with the venv module. Everything else
# is installed by ESP-IDF itself.
# =============================================================================

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
target=esp32s3
idf_repo_url=https://github.com/espressif/esp-idf.git
idf_version="$(head -n 1 "$repo_root/idf-version.txt" | tr -d '[:space:]')"
tools_dir="$repo_root/.tools"
idf_path="$tools_dir/esp-idf"
idf_tools_path="$tools_dir/espressif"
build_dir="$repo_root/build"
install_stamp="$idf_tools_path/installed-$idf_version-$target"

fail() { printf 'build.sh: %s\n' "$*" >&2; exit 1; }
step() { printf '==> %s\n' "$*"; }

# -----------------------------------------------------------------------------
# 1. Prerequisites - fail fast, naming exactly what is missing
# -----------------------------------------------------------------------------
case "$repo_root" in
    *[[:space:]]*) fail "the checkout path contains whitespace, which ESP-IDF does not support: $repo_root" ;;
esac
command -v git >/dev/null 2>&1 \
    || fail "git was not found (Debian/Ubuntu: sudo apt install git; macOS: xcode-select --install)"

python=""
for candidate in python3.12 python3.11 python3.10 python3 python; do
    if command -v "$candidate" >/dev/null 2>&1 &&
       "$candidate" -c 'import sys; assert (3, 8) <= sys.version_info[:2] < (3, 14)' 2>/dev/null; then
        python="$(command -v "$candidate")"
        break
    fi
done
[ -n "$python" ] \
    || fail "Python 3.8-3.13 was not found (Debian/Ubuntu: sudo apt install python3 python3-venv; macOS: xcode-select --install)"
"$python" -c 'import venv, ensurepip' 2>/dev/null \
    || fail "$python lacks the venv/ensurepip modules (Debian/Ubuntu: sudo apt install python3-venv)"
step "Using Python $python"

# ESP-IDF's scripts look for a bare python3: make it the interpreter above.
mkdir -p "$tools_dir/bin"
ln -sf "$python" "$tools_dir/bin/python3"
export PATH="$tools_dir/bin:$PATH"

# Keep every ESP-IDF artifact repo-local; ignore any globally exported IDF.
export IDF_PATH="$idf_path"
export IDF_TOOLS_PATH="$idf_tools_path"
export PIP_CACHE_DIR="$tools_dir/pip-cache"
export CCACHE_DIR="$tools_dir/ccache"
unset IDF_PYTHON_ENV_PATH

# -----------------------------------------------------------------------------
# 2. ESP-IDF source, pinned (cloned under a temporary name, renamed when
#    complete, so an interrupted clone never looks installed)
# -----------------------------------------------------------------------------
if [ -d "$idf_path" ]; then
    installed="$(git -C "$idf_path" describe --tags --exact-match 2>/dev/null || true)"
    if [ "$installed" != "$idf_version" ]; then
        step "ESP-IDF '$installed' found but $idf_version is pinned: replacing it"
        rm -rf "$idf_path"
    fi
fi
if [ ! -d "$idf_path" ]; then
    rm -rf "$idf_path.partial"
    step "Cloning ESP-IDF $idf_version into .tools/esp-idf (first run only)"
    git clone --depth 1 --branch "$idf_version" --recursive --shallow-submodules \
        "$idf_repo_url" "$idf_path.partial" \
        || fail "git clone of ESP-IDF $idf_version failed (network, or tag missing upstream)"
    mv "$idf_path.partial" "$idf_path"
fi

# -----------------------------------------------------------------------------
# 3. Toolchain + Python environment for esp32s3 (first run only). CMake and
#    Ninja are opt-in tools on Linux/macOS, so they are requested by name.
# -----------------------------------------------------------------------------
if [ ! -f "$install_stamp" ]; then
    step "Installing ESP-IDF tools for $target into .tools/espressif (first run only)"
    "$idf_path/install.sh" "$target" || fail "ESP-IDF install.sh failed; see the output above"
    python3 "$idf_path/tools/idf_tools.py" install cmake ninja \
        || fail "installing CMake/Ninja through idf_tools.py failed"
    touch "$install_stamp"
fi

step "Exporting the ESP-IDF environment"
set +eu  # export.sh is written for interactive shells
. "$idf_path/export.sh" >/dev/null
set -eu
command -v xtensa-esp32s3-elf-gcc >/dev/null 2>&1 \
    || fail "export.sh did not put the esp32s3 toolchain on PATH; delete .tools and re-run"

# -----------------------------------------------------------------------------
# 4. Build
# -----------------------------------------------------------------------------
# A build dir configured from another location (e.g. the Docker build at
# /project) cannot be reused: CMake refuses it. It is only build output.
cache="$build_dir/CMakeCache.txt"
if [ -f "$cache" ]; then
    configured="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache")"
    if [ "$configured" != "$repo_root" ]; then
        step "Discarding build/ configured for '$configured'"
        rm -rf "$build_dir"
    fi
fi

cd "$repo_root"
if [ ! -f sdkconfig ]; then
    step "idf.py set-target $target"
    idf.py set-target "$target" || fail "idf.py set-target failed"
fi
if [ $# -gt 0 ]; then
    step "idf.py $*"
    idf.py "$@" || fail "idf.py $* failed"
    exit 0  # custom idf.py command: its own output says what it did
fi
step "idf.py build"
idf.py build || fail "idf.py build failed"

# -----------------------------------------------------------------------------
# 5. Where the binaries are (offsets straight from the build's flasher args)
# -----------------------------------------------------------------------------
printf '\nBuilt with ESP-IDF %s for %s. Flash offsets and files:\n' "$idf_version" "$target"
python - "$build_dir" <<'EOF'
import json, os, sys
build_dir = sys.argv[1]
files = json.load(open(os.path.join(build_dir, "flasher_args.json")))["flash_files"]
for offset in sorted(files, key=lambda o: int(o, 16)):
    print("  %-9s %s" % (offset, os.path.join(build_dir, files[offset])))
EOF
printf '\nFlash all of them over USB/UART (from build/):  python -m esptool --chip esp32s3 write_flash @flash_args\n'
printf 'Details: docs/build.md\n'

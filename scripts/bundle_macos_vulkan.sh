#!/usr/bin/env bash
# Bundle the Vulkan loader (libvulkan) + MoltenVK (libMoltenVK) next to the
# wmr AI binary so it runs on a clean macOS install — no Homebrew, no Vulkan SDK,
# no MoltenVK installed system-wide.
#
# The AI build's only non-system dynamic dependency is the Vulkan *loader*
# (libvulkan.1.dylib, a hard dyld load command — without it the binary won't
# even launch, before the CPU fallback can engage). The loader then dlopens an
# ICD at runtime; MoltenVK (libMoltenVK.dylib) is the ICD that gives us a Metal
# GPU. Both must ship with the binary.
#
# Layout produced under <out_dir>/:
#   wmr                    launcher: points the loader at the bundled ICD manifest
#   wmr.bin                the real Mach-O binary (libvulkan load cmd -> @rpath)
#   lib/libvulkan.1.dylib  the loader, install-id @rpath/libvulkan.1.dylib
#   lib/libMoltenVK.dylib  the Metal ICD
#   lib/MoltenVK_icd.json  manifest with library_path relative to itself
#
# Usage: bundle_macos_vulkan.sh <binary> <out_dir>
# Requires: brew install vulkan-loader molten-vk
set -euo pipefail

BIN="${1:?usage: $0 <binary> <out_dir>}"
OUT="${2:?usage: $0 <binary> <out_dir>}"

VULKAN_LOADER_LIB="$(brew --prefix vulkan-loader)/lib/libvulkan.1.dylib"
MOLTENVK_LIB="$(brew --prefix molten-vk)/lib/libMoltenVK.dylib"

for f in "$BIN" "$VULKAN_LOADER_LIB" "$MOLTENVK_LIB"; do
    if [ ! -f "$f" ]; then
        echo "error: not found: $f" >&2
        echo "  (loader/MoltenVK need: brew install vulkan-loader molten-vk)" >&2
        exit 1
    fi
done

rm -rf "$OUT"
mkdir -p "$OUT/lib"

cp "$BIN" "$OUT/wmr.bin"
# -L resolves the Homebrew symlinks (libvulkan.1.dylib -> libvulkan.1.4.x.dylib).
cp -L "$VULKAN_LOADER_LIB" "$OUT/lib/libvulkan.1.dylib"
cp -L "$MOLTENVK_LIB"      "$OUT/lib/libMoltenVK.dylib"

# ONNX Runtime shared lib (LaMa inpainter, WMR_BUILD_AI_LAMA). The configure-time
# ORT fetch leaves it next to the binary under onnxruntime-osx-*/lib. Co-located
# in lib/ like the Vulkan libs — the binary's load cmd is @rpath/libonnxruntime.1.dylib
# and @loader_path/lib is added to the rpath below (for Vulkan), so dyld finds it.
# 1.10.0: macOS uses native CoreML for MI-GAN (no ORT), so this is a no-op on mac
# (the glob finds nothing). Kept for a hypothetical future ORT-on-mac build.
# (Pathname expansion does not occur in a bare assignment, so resolve via ls -d.)
ORT_LIB_DIR=$(ls -d "$(dirname "$BIN")"/onnxruntime-osx-*/lib 2>/dev/null | head -n1 || true)
if [ -n "$ORT_LIB_DIR" ] && [ -d "$ORT_LIB_DIR" ]; then
    cp -L "$ORT_LIB_DIR"/libonnxruntime.1.dylib "$OUT/lib/libonnxruntime.1.dylib"
    codesign --force --sign - "$OUT/lib/libonnxruntime.1.dylib"
    echo "  + libonnxruntime.1.dylib (ORT MI-GAN)"
fi

# ICD manifest: the Vulkan loader resolves `library_path` RELATIVE TO THE MANIFEST
# file, so a bare filename finds the co-located libMoltenVK.dylib.
cat > "$OUT/lib/MoltenVK_icd.json" <<'JSON'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "libMoltenVK.dylib",
        "api_version": "1.4.0",
        "is_portability_driver": true
    }
}
JSON

# Rewrite the binary's libvulkan load command (a Homebrew absolute path) to
# @rpath, and add @loader_path/lib so dyld resolves it from ./lib next to the
# binary — independent of CWD and of any installed loader.
OLD_VK="$(otool -L "$OUT/wmr.bin" | grep 'libvulkan' | awk '{print $1}' | sed 's/^[[:space:]]*//' | head -1)"
if [ -n "$OLD_VK" ]; then
    install_name_tool -change "$OLD_VK" "@rpath/libvulkan.1.dylib" "$OUT/wmr.bin"
fi
# idempotent: ignore "would duplicate path" if an @loader_path/lib rpath exists.
install_name_tool -add_rpath "@loader_path/lib" "$OUT/wmr.bin" 2>/dev/null || true

install_name_tool -id "@rpath/libvulkan.1.dylib" "$OUT/lib/libvulkan.1.dylib"

# install_name_tool invalidates code signatures; on Apple Silicon a stale-signed
# Mach-O is killed on launch. Re-sign ad-hoc ONLY what we modified with
# install_name_tool (wmr.bin + libvulkan). libMoltenVK.dylib is copied verbatim
# — its original Homebrew signature is valid, and re-signing it ad-hoc can make
# the Vulkan loader's dlopen() reject it on stricter runners (process SIGKILL).
codesign --force --sign - "$OUT/lib/libvulkan.1.dylib"
codesign --force --sign - "$OUT/wmr.bin"
# Sanity: wmr.bin must still launch (catches a broken @rpath/sign here, not later).
"$OUT/wmr.bin" --version >/dev/null 2>&1 || { echo "error: bundled wmr.bin fails to launch" >&2; exit 1; }

# Launcher: makes the bundled MoltenVK ICD the *only* source the loader consults
# (VK_ICD_FILENAMES replaces the default search), so the GPU driver ships with
# the binary. The loader still works if Metal is absent — ncnn falls back to CPU.
# A pre-set VK_ICD_FILENAMES wins (lets users/tests point elsewhere or force no
# ICD -> CPU, e.g. on a CI runner whose paravirtualized GPU can't run MoltenVK).
cat > "$OUT/wmr" <<'EOF'
#!/usr/bin/env bash
# Launcher for the macOS AI build. Points the Vulkan loader at the MoltenVK ICD
# shipped alongside this binary so it runs on a machine with no Vulkan SDK /
# MoltenVK installed, then execs the real binary. A pre-set VK_ICD_FILENAMES is
# respected (override the ICD, or set to a nonexistent path to force CPU).
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-$DIR/lib/MoltenVK_icd.json}"
export VK_LAYER_PATH=""
exec "$DIR/wmr.bin" "$@"
EOF
chmod +x "$OUT/wmr" "$OUT/wmr.bin"

echo "Bundled AI binary -> $OUT"
echo "  launcher : $OUT/wmr"
echo "  binary   : $OUT/wmr.bin  (libvulkan -> @rpath, rpath += @loader_path/lib)"

#!/bin/bash
set -e

# ============================================================
# Build a minimal Vulkan smoke-test NRO for Nintendo Switch
# Uses the packaged NVK Vulkan image/prefix
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$SCRIPT_DIR/SmokeTest.cpp"
BUILD_DIR="$SCRIPT_DIR/build"
NRO_OUT="$SCRIPT_DIR/build/SmokeTest.nro"
USE_DOCKER="${SmokeTest_DOCKER:-1}"
DOCKER_IMAGE="${SmokeTest_DOCKER_IMAGE:-ticohq/switch-nvk-vulkan}"

run() {
    if [ "$USE_DOCKER" = "1" ]; then
        docker run --rm \
            -v "$SCRIPT_DIR:$PROJECT_PREFIX" \
            --workdir "$PROJECT_PREFIX" \
            "$DOCKER_IMAGE" bash -lc "$1"
    else
        bash -c "$1"
    fi
}

if [ "$USE_DOCKER" = "1" ]; then
    PROJECT_PREFIX="${SmokeTest_PROJECT_PREFIX:-/work}"
    NVK_PREFIX="${SmokeTest_NVK_PREFIX:-/opt/nvk-switch}"
else
    PROJECT_PREFIX="$SCRIPT_DIR"
    NVK_PREFIX="${SmokeTest_NVK_PREFIX:-/opt/nvk-switch}"
fi

echo "=== Building Vulkan Smoke Test NRO ==="

run "rm -rf '$PROJECT_PREFIX/build' && mkdir -p '$PROJECT_PREFIX/build'"

# DevkitPro paths
DKP=/opt/devkitpro
DKA=$DKP/devkitA64
LIBNX=$DKP/libnx
PORTLIBS=$DKP/portlibs/switch

CXX=$DKA/bin/aarch64-none-elf-g++

# Compile flags
CXXFLAGS="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
CXXFLAGS+=" -O2 -std=c++17 -ffunction-sections -fdata-sections"
CXXFLAGS+=" -I$LIBNX/include"
CXXFLAGS+=" -I$PORTLIBS/include"
PKG_CONFIG_ENV="export PKG_CONFIG_PATH='$NVK_PREFIX/lib/pkgconfig:$PORTLIBS/lib/pkgconfig':\${PKG_CONFIG_PATH:-}"

echo "Compiling SmokeTest.cpp..."
run "
$PKG_CONFIG_ENV
$CXX $CXXFLAGS \$(pkg-config --cflags nvk-switch-vulkan) \
    -c '$PROJECT_PREFIX/SmokeTest.cpp' \
    -o '$PROJECT_PREFIX/build/SmokeTest.o'
"

LDFLAGS="-specs=$LIBNX/switch.specs"
LDFLAGS+=" -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
LDFLAGS+=" -L$DKA/lib -L$LIBNX/lib -L$PORTLIBS/lib"
LDFLAGS+=" -Wl,--gc-sections"

echo "Linking..."
run "
$PKG_CONFIG_ENV
$CXX $LDFLAGS \
    '$PROJECT_PREFIX/build/SmokeTest.o' \
    \$(pkg-config --static --libs nvk-switch-vulkan) \
    -o '$PROJECT_PREFIX/build/SmokeTest.elf'
"

# Generate NRO
echo "Generating NRO..."
run "
nacptool --create 'NVK Smoke Test' 'NVK Examples' '0.0.1' '$PROJECT_PREFIX/build/SmokeTest.nacp'
elf2nro '$PROJECT_PREFIX/build/SmokeTest.elf' '$PROJECT_PREFIX/build/SmokeTest.nro' --nacp='$PROJECT_PREFIX/build/SmokeTest.nacp'
"

if [ -f "$NRO_OUT" ]; then
    SIZE=$(stat -c%s "$NRO_OUT" 2>/dev/null || stat -f%z "$NRO_OUT" 2>/dev/null)
    echo "SUCCESS: $NRO_OUT ($SIZE bytes)"
else
    echo "FAILED: NRO not produced"
    exit 1
fi

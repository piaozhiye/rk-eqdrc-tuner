#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
# Cross-compile eqdrc_tuner for aarch64 (RK3576/RK3308 buildroot sysroot)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN=${TOOLCHAIN:-/opt/toolchains/gcc-arm-buildroot-aarch}

if [ ! -x "$TOOLCHAIN/bin/aarch64-linux-gcc" ]; then
    echo "build_aarch64: toolchain not found: $TOOLCHAIN/bin/aarch64-linux-gcc" >&2
    echo "  set TOOLCHAIN=/path/to/toolchain (a buildroot aarch64 sysroot toolchain)" >&2
    exit 1
fi
BUILD_DIR="$SCRIPT_DIR/build_aarch64"

export LD_LIBRARY_PATH=$TOOLCHAIN/libexec/gcc/aarch64-buildroot-linux-gnu/9.3.0:$LD_LIBRARY_PATH

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake \
    -DCMAKE_C_COMPILER=$TOOLCHAIN/bin/aarch64-linux-gcc \
    -DCMAKE_CXX_COMPILER=$TOOLCHAIN/bin/aarch64-linux-g++ \
    -DCMAKE_FIND_ROOT_PATH=$TOOLCHAIN/aarch64-buildroot-linux-gnu/sysroot \
    -DCROSS_COMPILE_AARCH64=1 \
    "$SCRIPT_DIR"

make -j$(nproc)

echo ""
echo "=== Build complete ==="
ls -la eqdrc_tuner

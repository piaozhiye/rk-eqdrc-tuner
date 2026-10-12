#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
# build_pc.sh - x86_64 Linux SDL simulation build of eqdrc_tuner.
#
# ui_main.c / eqdrc_para.c build verbatim (-DEQDRC_PC_SIM only swaps the
# port layer to lv_port_pc.c: SDL window + mouse input, half-scale view
# of the 1080x1920 panel). Links the x86 liblvgl.so from lib/x86.
#
# Prerequisites (once):
#   sudo apt install libsdl2-dev
#   x86 liblvgl.so in lib/x86 (built from lvgl 8.4 with include/lv_conf.h)
#
# Usage:
#   ./build_pc.sh && ./eqdrc_tuner_pc
#   (ESC/Q quit, S dumps the framebuffer to eqdrc_tuner_dump.ppm)

set -euo pipefail
cd "$(dirname "$0")"

LIBDIR=lib/x86
[ -f $LIBDIR/liblvgl.so ] || {
    echo "x86 liblvgl.so missing in lib/x86 - build it from lvgl 8.4 sources with include/lv_conf.h"
    exit 1
}

# a local working copy of the parameter bin (the board path doesn't
# exist on the PC); refreshed from the eqdrc preset set if absent
# working parameter bin for the sim: EQT_PARA env, a local copy, or
# synthesize a flat default so the UI still runs
PARA_SRC=${EQT_PARA:-}
if [ ! -f eqdrc_tuner.bin ]; then
    for c in "$PARA_SRC" ./para.bin; do
        [ -n "$c" ] && [ -f "$c" ] && cp "$c" eqdrc_tuner.bin && break
    done
fi

mkdir -p slots

# layout + bin I/O tests gate the build (artifacts stay in build_pc/)
mkdir -p build_pc
gcc -O2 -Wall -o build_pc/test_eqdrc_para tests/test_eqdrc_para.c \
    eqdrc_para.c -I.
./build_pc/test_eqdrc_para

gcc -O2 -Wall -DEQDRC_PC_SIM -o eqdrc_tuner_pc \
    ui_main.c eqdrc_para.c lv_port_pc.c \
    -I. -Iinclude -I/usr/include/SDL2 \
    -L$LIBDIR -llvgl -lSDL2 -lpthread -lm -lrt \
    -Wl,-rpath,'$ORIGIN/lib/x86'

echo "built ./eqdrc_tuner_pc; run with:"
echo "  ./eqdrc_tuner_pc            # rpath finds the lib automatically"

#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
# Build + deploy eqdrc_tuner to the board.
#
# NOTE: the DSI panel cannot be re-woken by a warm restart (DRM master
# release puts it to sleep and no userland sequence revives it), so the
# board is rebooted after pushing. Boot autostart (S99eqdrc_tuner) brings
# the UI straight up.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

"$SCRIPT_DIR/build_aarch64.sh"
adb push "$SCRIPT_DIR/build_aarch64/eqdrc_tuner" /data/eqdrc_tuner/ | tail -1
# LVGL lib with the 26/28/36 font set (rebuilt from lvgl 8.4.1 + our lv_conf)
adb push "$SCRIPT_DIR/lib/aarch64/liblvgl.so" /data/eqdrc_tuner/lib/ | tail -1

# EQDRC runtime (ALSA plugin, effect lib, preset bins) is NOT in this
# repo - it ships with the Rockchip SDK. Push the
# components if you keep a local copy, else ensure they are on the board.
RT="$SCRIPT_DIR/eqdrc-runtime"
[ -f "$RT/alsa-lib/aarch64/libasound_module_pcm_eqdrc.so" ] && \
    adb push "$RT/alsa-lib/aarch64/libasound_module_pcm_eqdrc.so" /usr/lib/alsa-lib/ | tail -1
[ -f "$RT/lib/aarch64/librkaudio_effect_eqdrc.so" ] && \
    adb push "$RT/lib/aarch64/librkaudio_effect_eqdrc.so" /usr/lib/ | tail -1
if [ -d "$RT/bins" ]; then
    adb shell 'mkdir -p /usr/share/rkaudio'
    adb push "$RT/bins/." /usr/share/rkaudio/ | tail -1
fi
echo "rebooting board..."
adb reboot
sleep 45
adb wait-for-device
sleep 12
adb shell 'pidof eqdrc_tuner && echo "tuner up" || echo "tuner NOT running"'

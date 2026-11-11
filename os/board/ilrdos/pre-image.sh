#!/bin/sh
# Roda antes do post-image da Raspberry Pi: ajusta cmdline.txt/config.txt e
# coloca ilrdos.txt (config de Wi-Fi) na partição BOOT. $1 = BINARIES_DIR.
set -eu
BINARIES_DIR="$1"
FW="$BINARIES_DIR/rpi-firmware"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$FW"

echo "root=/dev/mmcblk0p2 rootwait ro rootfstype=ext4 console=tty1 quiet loglevel=3 logo.nologo consoleblank=0" > "$FW/cmdline.txt"

if ! grep -q '^# ILrDOS' "$FW/config.txt" 2>/dev/null; then
	cat >> "$FW/config.txt" <<CFG

# ILrDOS
disable_splash=1
boot_delay=0
avoid_warnings=1
CFG
fi

cp "$HERE/ilrdos.txt.example" "$FW/ilrdos.txt"
cp "$HERE/ilrdos-robot.txt.example" "$FW/ilrdos-robot.txt"
cp "$HERE/ilrdos-iot.txt.example" "$FW/ilrdos-iot.txt"
cp "$HERE/ilrdos-fleet.txt.example" "$FW/ilrdos-fleet.txt"

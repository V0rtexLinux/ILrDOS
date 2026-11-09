#!/bin/sh
# Recebe o diretório do rootfs ($1). Executado depois do post-build do Buildroot.
set -eu
TARGET_DIR="$1"
VERSION="$(cat "$BR2_EXTERNAL_ILRDOS_PATH/../VERSION" 2>/dev/null || echo 1.0.0)"

# Sem getty/login: o console é o banner do ILrDOS e o daemon fica sob respawn.
sed -i -E '/getty|askfirst/d' "$TARGET_DIR/etc/inittab"
cat >> "$TARGET_DIR/etc/inittab" <<'INITTAB'

# ILrDOS
::respawn:/usr/libexec/ilrdos/run-daemon
tty1::respawn:/usr/sbin/ilrdos-banner
INITTAB

chmod 0755 "$TARGET_DIR/etc/init.d/S10ilrdos-boot" \
           "$TARGET_DIR/etc/init.d/S35ilrdos-wifi" \
           "$TARGET_DIR/usr/libexec/ilrdos/run-daemon" \
           "$TARGET_DIR/usr/sbin/ilrdos-banner"

echo "$VERSION" > "$TARGET_DIR/etc/ilrdos-release"
cat > "$TARGET_DIR/etc/os-release" <<OSREL
NAME="ILrDOS"
PRETTY_NAME="ILrDOS $VERSION"
ID=ilrdos
VERSION_ID=$VERSION
HOME_URL=""
OSREL

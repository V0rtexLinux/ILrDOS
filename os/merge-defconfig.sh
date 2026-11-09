#!/usr/bin/env bash
# Uso: merge-defconfig.sh BASE_DEFCONFIG FRAGMENTO > SAIDA
# Remove do base as chaves que o fragmento redefine e anexa o fragmento.
set -euo pipefail
base="$1"; frag="$2"

esc() { sed 's/[&|\\]/\\&/g'; }
base_val() { sed -n "s/^$1=\"\(.*\)\"\$/\1/p" "$base" | head -n1 | esc; }

keys="$(sed -nE 's/^(BR2_[A-Za-z0-9_]+)=.*/\1/p; s/^# (BR2_[A-Za-z0-9_]+) is not set$/\1/p' "$frag")"

awk -v keys="$keys" '
BEGIN { n = split(keys, a, "\n"); for (i = 1; i <= n; i++) k[a[i]] = 1 }
{
  if (match($0, /^BR2_[A-Za-z0-9_]+=/)) {
    key = substr($0, RSTART, RLENGTH - 1); if (key in k) next
  } else if ($0 ~ /^# BR2_[A-Za-z0-9_]+ is not set$/) {
    if ($2 in k) next
  }
  print
}' "$base"

echo
sed -e "s|@BASE_POST_BUILD@|$(base_val BR2_ROOTFS_POST_BUILD_SCRIPT)|" \
    -e "s|@BASE_POST_IMAGE@|$(base_val BR2_ROOTFS_POST_IMAGE_SCRIPT)|" \
    -e 's|"  *|"|; s|  *"|"|' "$frag"

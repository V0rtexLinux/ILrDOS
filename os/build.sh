#!/usr/bin/env bash
# Gera a imagem do ILrDOS (cartão SD) para Raspberry Pi Zero 2 W com o Buildroot.
# Requisitos: Linux x86_64 (ou WSL2/Docker), internet, ~15 GB livres, sem root.
set -euo pipefail

OS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$OS_DIR/.." && pwd)"
BUILDROOT_VERSION="${BUILDROOT_VERSION:-2026.08}"
BASE_DEFCONFIG="${BASE_DEFCONFIG:-raspberrypizero2w_64_defconfig}"
WORK="${ILRDOS_WORK:-$HOME/ilrdos-os-build}"
VERSION="$(cat "$ROOT/VERSION" 2>/dev/null || echo 1.0.0)"

die() { echo "ERRO: $*" >&2; exit 1; }

[[ $EUID -ne 0 ]] || die "não rode como root (o Buildroot recusa)."
[[ "$(uname -s)" == Linux ]] || die "use Linux, WSL2 ou Docker."
[[ "$ROOT$WORK" != *" "* ]] || die "caminhos com espaço quebram o Buildroot."
for cmd in curl make gcc g++ tar xz unzip rsync cpio bc file patch perl python3 sed awk; do
  command -v "$cmd" >/dev/null || die "falta '$cmd'. Debian/Ubuntu: sudo apt install build-essential curl xz-utils unzip rsync cpio bc file patch perl python3 libncurses-dev"
done

mkdir -p "$WORK/dl" "$WORK/dist"

# Zips/checkouts sem bit de execução (ex.: Windows) causam "Permission denied" (Error 126)
# no post-build/pre-image. Garante +x em todos os scripts usados pelo Buildroot.
chmod +x "$OS_DIR"/merge-defconfig.sh "$OS_DIR"/board/ilrdos/post-build.sh \
         "$OS_DIR"/board/ilrdos/pre-image.sh "$ROOT"/scripts/*.sh
"$ROOT/scripts/fetch-models.sh"   # SFace (reconhecimento de rostos)
BR="$WORK/buildroot-$BUILDROOT_VERSION"
OUT="$WORK/output"

if [[ ! -d "$BR" ]]; then
  TARBALL="$WORK/dl/buildroot-$BUILDROOT_VERSION.tar.xz"
  [[ -s "$TARBALL" ]] || curl -fL "https://buildroot.org/downloads/buildroot-$BUILDROOT_VERSION.tar.xz" -o "$TARBALL"
  if [[ -n "${BUILDROOT_SHA256:-}" ]]; then
    echo "$BUILDROOT_SHA256  $TARBALL" | sha256sum --check --status || die "SHA-256 do Buildroot não confere."
  else
    echo "Aviso: defina BUILDROOT_SHA256 para verificar o download. SHA-256 atual: $(sha256sum "$TARBALL" | cut -d' ' -f1)"
  fi
  tar -C "$WORK" -xf "$TARBALL"
fi

[[ -f "$BR/configs/$BASE_DEFCONFIG" ]] || die "$BASE_DEFCONFIG não existe no Buildroot $BUILDROOT_VERSION. Veja: ls $BR/configs | grep zero2w"

# Defconfig final = base da Raspberry + fragmento do ILrDOS (gerado, não edite)
"$OS_DIR/merge-defconfig.sh" "$BR/configs/$BASE_DEFCONFIG" "$OS_DIR/configs/ilrdos_pizero2w.fragment" \
  > "$OS_DIR/configs/ilrdos_pizero2w_defconfig"

export BR2_DL_DIR="$WORK/dl"
make -C "$BR" O="$OUT" BR2_EXTERNAL="$OS_DIR" ilrdos_pizero2w_defconfig
make -C "$BR" O="$OUT" BR2_EXTERNAL="$OS_DIR" olddefconfig

# O Kconfig descarta em silêncio opções com dependência não atendida: confira as essenciais.
missing=0
for sym in BR2_PACKAGE_ILRDOS BR2_PACKAGE_OPENCV4 BR2_PACKAGE_OPENCV4_LIB_DNN \
           BR2_PACKAGE_OPENCV4_LIB_OBJDETECT BR2_PACKAGE_OPENCV4_LIB_VIDEOIO \
           BR2_PACKAGE_OPENCV4_WITH_V4L BR2_PACKAGE_WPA_SUPPLICANT \
           BR2_PACKAGE_WPA_SUPPLICANT_PASSPHRASE BR2_PACKAGE_BRCMFMAC_SDIO_FIRMWARE_RPI_WIFI \
           BR2_PACKAGE_ALSA_LIB BR2_PACKAGE_ESPEAK BR2_PACKAGE_ILRDOS_VOSK; do
  grep -q "^$sym=y" "$OUT/.config" || { echo "FALTA no .config: $sym"; missing=1; }
done
(( missing == 0 )) || die "opções essenciais descartadas; rode: make -C $BR O=$OUT BR2_EXTERNAL=$OS_DIR menuconfig"

make -C "$BR" O="$OUT" BR2_EXTERNAL="$OS_DIR" -j"$(nproc)"

IMG="$OUT/images/sdcard.img"
[[ -s "$IMG" ]] || die "sdcard.img não foi gerado."

# Confere se a config de Wi-Fi entrou na partição BOOT e se sobra espaço para o token
MTOOLS="$OUT/host/bin"
if [[ -x "$MTOOLS/mdir" && -s "$OUT/images/boot.vfat" ]]; then
  "$MTOOLS/mdir" -i "$OUT/images/boot.vfat" :: | grep -qi 'ilrdos *txt' \
    || die "ilrdos.txt não entrou na partição BOOT (o genimage do Buildroot mudou?)."
  "$MTOOLS/mdir" -i "$OUT/images/boot.vfat" :: | tail -n 2
fi

DEST="$WORK/dist/ILrDOS-$VERSION-pizero2w.img"
cp "$IMG" "$DEST"
xz -T0 -k -f "$DEST"
( cd "$WORK/dist" && sha256sum "$(basename "$DEST").xz" > "$(basename "$DEST").xz.sha256" )

echo
echo "Imagem pronta: $DEST(.xz)"
echo "Grave com Raspberry Pi Imager (Usar personalizado) ou balenaEtcher."
echo "Depois edite ilrdos.txt na partição BOOT com o SSID e a senha do Wi-Fi."

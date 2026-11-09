#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/build"
mkdir -p "$OUT"

OPENSSL_CFLAGS=()
OPENSSL_LDFLAGS=()
OPENCV_CFLAGS=()
OPENCV_LIBS=()
if pkg-config --exists openssl 2>/dev/null; then
  read -r -a OPENSSL_CFLAGS <<<"$(pkg-config --cflags openssl)"
  read -r -a OPENSSL_LDFLAGS <<<"$(pkg-config --libs-only-L openssl)"
elif [[ ! -f /usr/include/openssl/hmac.h ]]; then
  HEADER="$(compgen -G '/nix/store/*-openssl-*-dev/include/openssl/hmac.h' | sort -V | tail -n 1 || true)"
  if [[ -n "$HEADER" ]]; then
    DEV_PREFIX="${HEADER%/include/openssl/hmac.h}"
    OPENSSL_CFLAGS+=("-I${DEV_PREFIX}/include")
    DEV_NAME="${DEV_PREFIX##*/}"
    OPENSSL_VERSION="${DEV_NAME#*-openssl-}"
    OPENSSL_VERSION="${OPENSSL_VERSION%-dev}"
    OPENSSL_LIB_DIR="$(compgen -G "/nix/store/*-openssl-${OPENSSL_VERSION}/lib" | head -n 1 || true)"
    if [[ -n "$OPENSSL_LIB_DIR" ]]; then
      OPENSSL_LDFLAGS+=("-L${OPENSSL_LIB_DIR}" "-Wl,-rpath,${OPENSSL_LIB_DIR}")
    fi
  fi
fi

if pkg-config --exists opencv4 2>/dev/null; then
  read -r -a OPENCV_CFLAGS <<<"$(pkg-config --cflags opencv4)"
  read -r -a OPENCV_LIBS <<<"$(pkg-config --libs opencv4)"
  OPENCV_CFLAGS+=("-DILRDOS_HAVE_OPENCV=1")
  echo "OpenCV 4 encontrado: detecção facial YuNet habilitada."
else
  echo "Aviso: OpenCV 4 não encontrado; o daemon compilará sem câmera."
  echo "No Raspberry Pi: sudo apt install -y libopencv-dev"
fi

# Voz nativa (opcional): ALSA para microfone/alto-falante e libvosk para reconhecimento.
VOICE_CFLAGS=()
VOICE_LIBS=()
if pkg-config --exists alsa 2>/dev/null; then
  read -r -a ALSA_C <<<"$(pkg-config --cflags alsa)"
  read -r -a ALSA_L <<<"$(pkg-config --libs alsa)"
  VOICE_CFLAGS+=("${ALSA_C[@]}" "-DILRDOS_HAVE_ALSA=1")
  VOICE_LIBS+=("${ALSA_L[@]}")
  echo "ALSA encontrado: áudio habilitado."
else
  echo "Aviso: ALSA não encontrado (sudo apt install libasound2-dev); o robô não terá áudio."
fi
VOSK_DIR="${VOSK_DIR:-/usr/local}"
if [[ -f "${VOSK_DIR}/include/vosk_api.h" || -f /usr/include/vosk_api.h ]]; then
  [[ -f "${VOSK_DIR}/include/vosk_api.h" ]] && VOICE_CFLAGS+=("-I${VOSK_DIR}/include" "-L${VOSK_DIR}/lib" "-Wl,-rpath,${VOSK_DIR}/lib")
  VOICE_CFLAGS+=("-DILRDOS_HAVE_VOSK=1")
  VOICE_LIBS+=("-lvosk")
  echo "libvosk encontrada: reconhecimento de fala nativo habilitado."
else
  echo "Aviso: libvosk não encontrada (VOSK_DIR=...); sem reconhecimento de fala nativo (a conversa por texto, 'hear', continua)."
fi
read -r -a EXTRA_C <<<"${ILRDOS_EXTRA_CFLAGS:-}"
read -r -a EXTRA_L <<<"${ILRDOS_EXTRA_LIBS:-}"

c++ -std=c++20 -O2 -Wall -Wextra -Wpedantic \
  -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
  "${OPENSSL_CFLAGS[@]}" "${OPENCV_CFLAGS[@]}" "${VOICE_CFLAGS[@]}" "${EXTRA_C[@]}" -pthread "${ROOT}/src/robotd.cpp" \
  "${OPENSSL_LDFLAGS[@]}" -lcrypto "${OPENCV_LIBS[@]}" "${VOICE_LIBS[@]}" "${EXTRA_L[@]}" \
  -o "${OUT}/ilrdosd"

c++ -std=c++20 -O2 -Wall -Wextra -Wpedantic \
  -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
  "${OPENSSL_CFLAGS[@]}" "${EXTRA_C[@]}" "${ROOT}/client/ilrdos_client.cpp" \
  "${OPENSSL_LDFLAGS[@]}" -lcrypto "${EXTRA_L[@]}" \
  -o "${OUT}/ilrdos-client"

echo "Build concluído: ${OUT}/ilrdosd e ${OUT}/ilrdos-client"

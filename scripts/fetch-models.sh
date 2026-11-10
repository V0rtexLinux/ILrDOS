#!/usr/bin/env bash
# Baixa o modelo de reconhecimento facial SFace (OpenCV Zoo, ~37 MB, licença Apache-2.0).
# Sem ele o robô continua detectando rostos, mas não os identifica.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/models/face_recognition_sface_2021dec.onnx"
URL="https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx"
if [[ -s "$DEST" ]]; then echo "SFace já existe: $DEST"; else
  command -v curl >/dev/null || { echo "Instale curl."; exit 1; }
  curl -fL "$URL" -o "$DEST"
fi
echo "SHA-256: $(sha256sum "$DEST" | cut -d' ' -f1)  (confira com a página do modelo no OpenCV Zoo)"

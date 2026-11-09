#!/usr/bin/env bash
set -euo pipefail

if [[ $EUID -eq 0 ]]; then
  echo "Execute sem sudo; o script solicitará sudo quando necessário."
  exit 1
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROBOT_USER="${SUDO_USER:-$USER}"
ROBOT_GROUP="$(id -gn "$ROBOT_USER")"

if ! command -v openssl >/dev/null || ! test -f /usr/include/openssl/hmac.h; then
  echo "Falta OpenSSL de desenvolvimento. Instale com:"
  echo "  sudo apt update && sudo apt install -y build-essential libssl-dev"
  exit 1
fi
if ! pkg-config --exists opencv4 2>/dev/null; then
  echo "A visão precisa de OpenCV 4. Instale-o antes de continuar:"
  echo "  sudo apt update && sudo apt install -y pkg-config libopencv-dev"
  exit 1
fi

"${ROOT}/scripts/fetch-models.sh" || echo "Aviso: sem o modelo SFace o robô não identifica rostos."
"${ROOT}/scripts/build-linux.sh"
sudo install -D -m 0755 "${ROOT}/build/ilrdosd" /usr/local/bin/ilrdosd
sudo install -D -m 0644 "${ROOT}/models/face_detection_yunet_2023mar.onnx" \
  /usr/share/ilrdos/models/face_detection_yunet_2023mar.onnx
if [[ -s "${ROOT}/models/face_recognition_sface_2021dec.onnx" ]]; then
  sudo install -D -m 0644 "${ROOT}/models/face_recognition_sface_2021dec.onnx" \
    /usr/share/ilrdos/models/face_recognition_sface_2021dec.onnx
fi
if ! getent group gpio >/dev/null; then
  echo "O grupo gpio não existe. Verifique a instalação do Raspberry Pi OS."
  exit 1
fi
sudo install -d -m 0750 -o "$ROBOT_USER" -g "$ROBOT_GROUP" /etc/ilrdos
if ! sudo test -s /etc/ilrdos/token; then
  sudo sh -c 'umask 077; openssl rand -hex 32 > /etc/ilrdos/token'
  sudo chown "$ROBOT_USER:$ROBOT_GROUP" /etc/ilrdos/token
  sudo chmod 0600 /etc/ilrdos/token
fi

sudo usermod -aG gpio,audio,video "$ROBOT_USER"
if ! sudo test -f /etc/ilrdos/robot.txt; then
  sudo install -m 0640 -o "$ROBOT_USER" -g "$ROBOT_GROUP" "${ROOT}/os/board/ilrdos/ilrdos-robot.txt.example" /etc/ilrdos/robot.txt
fi
if ! sudo test -f /etc/ilrdos/iot.txt; then
  sudo install -m 0640 -o "$ROBOT_USER" -g "$ROBOT_GROUP" "${ROOT}/os/board/ilrdos/ilrdos-iot.txt.example" /etc/ilrdos/iot.txt
fi

sudo tee /etc/systemd/system/ilrdos.service >/dev/null <<EOF
[Unit]
Description=ILrDOS Robot Runtime
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=${ROBOT_USER}
SupplementaryGroups=gpio audio video
StateDirectory=ilrdos
Environment=ILRDOS_IOT_CONFIG=/etc/ilrdos/iot.txt
Environment=ILRDOS_FACE_DB=/var/lib/ilrdos/faces.db
ExecStart=/usr/local/bin/ilrdosd --bind 0.0.0.0 --port 4317 --token-file /etc/ilrdos/token --config /etc/ilrdos/robot.txt --gpio-chip /dev/gpiochip0 --eye-pin 18 --alert-pin 23 --vision-model /usr/share/ilrdos/models/face_detection_yunet_2023mar.onnx --camera-index 0
Restart=on-failure
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
RestrictAddressFamilies=AF_INET AF_UNIX

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now ilrdos.service
echo
echo "ILrDOS instalado e iniciado."
echo "Token guardado em /etc/ilrdos/token (não compartilhe nem mostre em público)."
echo "Para achar o IP do Pi: hostname -I"
echo "Status: sudo systemctl status ilrdos"
echo "Logs:   sudo journalctl -u ilrdos -f"
echo "Câmera: conecte uma câmera compatível com V4L2 em /dev/video0."
echo
echo "Use somente uma rede Wi-Fi privada da demonstração; não exponha a porta 4317 à internet."

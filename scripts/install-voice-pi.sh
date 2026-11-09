#!/usr/bin/env bash
set -euo pipefail

if [[ $EUID -eq 0 ]]; then
  echo "Execute sem sudo; o script solicitará sudo quando necessário."
  exit 1
fi

if ! command -v python3 >/dev/null || ! python3 -m venv --help >/dev/null 2>&1 ||
   ! test -f /usr/include/portaudio.h ||
   ! test -f /usr/include/alsa/asoundlib.h || ! command -v espeak >/dev/null ||
   ! command -v aplay >/dev/null || ! command -v wget >/dev/null || ! command -v unzip >/dev/null; then
  echo "Instale as dependências: sudo apt update && sudo apt install -y python3-venv portaudio19-dev libasound2-dev espeak alsa-utils wget unzip"
  exit 1
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROBOT_USER="${SUDO_USER:-$USER}"
ROBOT_GROUP="$(id -gn "$ROBOT_USER")"
VOICE_HOME="/opt/ilrdos/voice"
MODEL_DIR="${VOICE_HOME}/models/vosk-model-small-pt-0.3"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [[ ! -f "${MODEL_DIR}/am/final.mdl" ]]; then
  echo "Baixando modelo Vosk pequeno de português brasileiro (~40 MB)..."
  wget -q --show-progress \
    https://alphacephei.com/vosk/models/vosk-model-small-pt-0.3.zip \
    -O "${TMP}/vosk-pt.zip"
  unzip -q "${TMP}/vosk-pt.zip" -d "${TMP}/models"
fi

sudo install -d -m 0750 -o "$ROBOT_USER" -g "$ROBOT_GROUP" "$VOICE_HOME" "${VOICE_HOME}/models"
sudo chown "$ROBOT_USER:$ROBOT_GROUP" "$VOICE_HOME"
if [[ ! -x "${VOICE_HOME}/venv/bin/python" ]]; then
  python3 -m venv "${VOICE_HOME}/venv"
fi
"${VOICE_HOME}/venv/bin/pip" install --disable-pip-version-check -r "${ROOT}/voice/requirements.txt"
sudo install -m 0755 -o "$ROBOT_USER" -g "$ROBOT_GROUP" \
  "${ROOT}/voice/ilrdos_voice.py" "${VOICE_HOME}/ilrdos_voice.py"
sudo install -m 0644 -o "$ROBOT_USER" -g "$ROBOT_GROUP" \
  "${ROOT}/voice/requirements.txt" "${VOICE_HOME}/requirements.txt"
sudo install -m 0644 "${ROOT}/models/silero_vad_16k_op15.onnx" \
  "${VOICE_HOME}/models/silero_vad_16k_op15.onnx"
sudo install -d -m 0755 "$MODEL_DIR"
if [[ -d "${TMP}/models/vosk-model-small-pt-0.3" ]]; then
  sudo cp -a "${TMP}/models/vosk-model-small-pt-0.3/." "$MODEL_DIR/"
  sudo chown -R "$ROBOT_USER:$ROBOT_GROUP" "$MODEL_DIR"
fi
sudo chown -R "$ROBOT_USER:$ROBOT_GROUP" "${VOICE_HOME}/venv"

if ! sudo test -f /etc/ilrdos/voice.env; then
  sudo tee /etc/ilrdos/voice.env >/dev/null <<'EOF'
# Edite com o IP do computador que executa Ollama e, opcionalmente, o TTS.
ILRDOS_PC_IP=192.168.1.100
ILRDOS_OLLAMA_MODEL=qwen2.5:7b-instruct
# ILRDOS_TTS_URL=http://192.168.1.100:5050/v1/audio/speech
# Para usar o servidor Kokoro do ILrDOS, configure: ILRDOS_TTS_VOICE=af_alloy
EOF
fi
sudo chown root:"$ROBOT_GROUP" /etc/ilrdos/voice.env
sudo chmod 0640 /etc/ilrdos/voice.env

sudo tee /etc/systemd/system/ilrdos-voice.service >/dev/null <<EOF
[Unit]
Description=ILrDOS Voice Assistant
After=ilrdos.service network-online.target sound.target
Wants=network-online.target

[Service]
Type=simple
User=${ROBOT_USER}
SupplementaryGroups=audio
EnvironmentFile=/etc/ilrdos/voice.env
Environment=ILRDOS_TOKEN_FILE=/etc/ilrdos/token
Environment=ILRDOS_VOSK_MODEL=${MODEL_DIR}
Environment=ILRDOS_VAD_MODEL=${VOICE_HOME}/models/silero_vad_16k_op15.onnx
WorkingDirectory=${VOICE_HOME}
ExecStart=${VOICE_HOME}/venv/bin/python ${VOICE_HOME}/ilrdos_voice.py
Restart=on-failure
RestartSec=5
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
RestrictAddressFamilies=AF_INET AF_UNIX

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload

echo
echo "Ponte de voz instalada. Edite /etc/ilrdos/voice.env com o IP do PC."
echo "No PC, instale e inicie Ollama, baixe o modelo configurado e permita a conexão"
echo "somente na rede privada da demonstração."
echo "Depois: sudo systemctl enable --now ilrdos-voice"
echo "Logs:   sudo journalctl -u ilrdos-voice -f"
echo "O Vosk transcreve localmente; o texto da pergunta é enviado ao Ollama na LAN."

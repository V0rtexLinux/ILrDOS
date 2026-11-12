#!/usr/bin/env bash
# Instala o agente num computador de placa única com Linux (Raspberry Pi, Orange Pi, Rock, Jetson, mini PC...).
#   sudo ./install.sh        depois edite os scripts em /etc/ilrdos-robot/ e rode: sudo systemctl enable --now ilrdos-robot-agent
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "rode com sudo"; exit 1; }
command -v python3 >/dev/null || { echo "instale o python3 (sudo apt install python3)"; exit 1; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

id ilrdos-robot >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin ilrdos-robot
for g in gpio i2c spi dialout video pwm; do          # acesso ao hardware, só nos grupos que existem nesta placa
  getent group "$g" >/dev/null && usermod -aG "$g" ilrdos-robot
done

install -m 0755 "$HERE/ilrdos_robot_agent.py" /usr/local/bin/ilrdos-robot-agent
install -d -m 0755 /etc/ilrdos-robot
for f in "$HERE"/hooks/*; do                         # não sobrescreve scripts que você já editou
  [[ -e /etc/ilrdos-robot/$(basename "$f") ]] || install -m 0755 -o root -g root "$f" /etc/ilrdos-robot/
done
install -m 0644 "$HERE/ilrdos-robot-agent.service" /etc/systemd/system/
systemctl daemon-reload
echo "Pronto. Edite /etc/ilrdos-robot/{start,stop,...} e ative: sudo systemctl enable --now ilrdos-robot-agent"

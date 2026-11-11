#!/usr/bin/env bash
# Instala só o que falta, com tentativas repetidas: o mirror do apt nos runners às vezes não resolve
# ("Temporary failure resolving ..."), e o CMake vem do pip para não depender dele.
# Uso: scripts/ci-install-deps.sh [--cmake] [pacote-apt ...]
set -euo pipefail

want_cmake=0
pkgs=()
for arg in "$@"; do
  if [[ "$arg" == "--cmake" ]]; then want_cmake=1; else pkgs+=("$arg"); fi
done

if (( want_cmake )) && ! command -v cmake >/dev/null 2>&1; then
  for i in 1 2 3; do
    python3 -m pip install --user --break-system-packages cmake && break
    echo "pip falhou (tentativa $i); repetindo..."; sleep $((i * 10))
  done
  export PATH="$HOME/.local/bin:$PATH"
  [[ -n "${GITHUB_PATH:-}" ]] && echo "$HOME/.local/bin" >> "$GITHUB_PATH"
fi

missing=()
for p in "${pkgs[@]}"; do
  dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
done

if (( ${#missing[@]} )); then
  echo "Instalando via apt: ${missing[*]}"
  ok=0
  for i in 1 2 3; do
    if sudo apt-get update -o Acquire::Retries=3 \
       && sudo apt-get install -y --no-install-recommends -o Acquire::Retries=3 "${missing[@]}"; then ok=1; break; fi
    echo "apt falhou (tentativa $i); repetindo..."; sleep $((i * 15))
  done
  (( ok )) || { echo "Não foi possível instalar: ${missing[*]}"; exit 1; }
else
  echo "Dependências do apt já presentes; pulando apt-get."
fi

command -v g++ >/dev/null || { echo "g++ ausente"; exit 1; }
(( want_cmake )) && cmake --version | head -1
exit 0

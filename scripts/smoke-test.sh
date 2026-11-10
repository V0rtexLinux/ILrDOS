#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 -m py_compile "${ROOT}/voice/ilrdos_voice.py" "${ROOT}/voice/tts_api.py"
echo "7ed98ddbad84ccac4cd0aeb3099049280713df825c610a8ed34543318f1b2c49  ${ROOT}/models/silero_vad_16k_op15.onnx" |
  sha256sum --check --status
"${ROOT}/scripts/build-linux.sh"

TMP="$(mktemp -d)"
PORT=$((20000 + RANDOM % 30000))
TOKEN_FILE="${TMP}/token"
SERVER_LOG="${TMP}/server.log"
CLIENT_LOG="${TMP}/client.log"
openssl rand -hex 32 >"$TOKEN_FILE"
chmod 0600 "$TOKEN_FILE"

"${ROOT}/build/ilrdosd" --simulate --bind 127.0.0.1 --port "$PORT" \
  --token-file "$TOKEN_FILE" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
cleanup() {
  kill "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT
sleep 0.3

TOKEN="$(cat "$TOKEN_FILE")"
printf '%s\n%s\n%s\n%s\n%s\n%s\n%s\n' \
  "$TOKEN" "status" "vision status" "vision scan" "eye alert" "status" "exit" |
  "${ROOT}/build/ilrdos-client" 127.0.0.1 "$PORT" >"$CLIENT_LOG" 2>&1

grep -q "GPIO=simulado" "$CLIENT_LOG"
grep -q "Visão aguardando uma varredura" "$CLIENT_LOG"
grep -q "Visão" "$CLIENT_LOG"
grep -q "Olho definido para: alert" "$CLIENT_LOG"
grep -q "olho=alert" "$CLIENT_LOG"

python3 "${ROOT}/voice/ilrdos_voice.py" --self-test --host 127.0.0.1 --port "$PORT" \
  --token-file "$TOKEN_FILE" >"${TMP}/voice-link.log"
grep -q "ILrDOS" "${TMP}/voice-link.log"

if printf '%s\n' "00000000000000000000000000000000" |
  "${ROOT}/build/ilrdos-client" 127.0.0.1 "$PORT" >"${TMP}/denied.log" 2>&1; then
  echo "ERRO: a conexão com token errado foi aceita."
  exit 1
fi
grep -q "Acesso negado" "${TMP}/denied.log"

"${ROOT}/tests/sim-test.sh" | tail -n 1 | grep -q "TUDO OK"
echo "Smoke test OK: build C++, visão configurável, comandos allowlist, ponte de voz/HMAC e rejeição de token incorreto."

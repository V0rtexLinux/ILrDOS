#!/usr/bin/env bash
# Testa o agente do robô DIY (computador de placa única) sem hardware: protocolo, segurança e limites.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$(mktemp -d)"; PORT=$((20000 + RANDOM % 30000)); trap 'kill $PID 2>/dev/null || true; rm -rf "$T"' EXIT
mkdir "$T/hooks"
for h in start stop pause; do printf '#!/bin/sh\necho %s >> "%s/log"\nexit 0\n' "$h" "$T" > "$T/hooks/$h"; done
printf '#!/bin/sh\n[ -e "%s/unsafe" ] && { echo queda detectada; exit 1; }\nexit 0\n' "$T" > "$T/hooks/safe"
chmod 755 "$T"/hooks/*
python3 "$ROOT/examples/diy-robot/ilrdos_robot_agent.py" --hooks "$T/hooks" --port "$PORT" --bind 127.0.0.1 --max-run 3 >"$T/agent.log" 2>&1 &
PID=$!; sleep 1
ask() { python3 - "$PORT" "$1" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 3); s.sendall((sys.argv[2] + "\n").encode()); print(s.makefile().readline().strip())
PY
}
fail=0
expect() { local got; got="$(ask "$1")"; if [[ "$got" == $2 ]]; then echo "[ok]   $1 -> $got"; else echo "[FALHA] $1 -> '$got' (esperado '$2')"; fail=1; fi; }
expect STATUS "OK stopped"
expect START "OK"
expect STATUS "OK working"
expect DOCK "ERR este robô não tem a ação dock"
expect PAUSE "OK"
expect BOGUS "ERR comando desconhecido"
expect STOP "OK"
touch "$T/unsafe"; expect START "ERR inseguro agora: queda detectada"; rm "$T/unsafe"
expect START "OK"; sleep 5; expect STATUS "OK stopped"        # limite de 3 s: parou sozinho
grep -q "^stop$" "$T/log" && echo "[ok]   hook stop foi executado pelo limite de tempo" || { echo "[FALHA] stop não rodou"; fail=1; }
chmod 777 "$T/hooks/pause"; expect PAUSE "ERR este robô não tem a ação pause"   # hook gravável por todos é recusado
exit $fail

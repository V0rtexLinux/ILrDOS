#!/usr/bin/env bash
# Testa o protocolo de incêndio do agente DIY (detector de gás + 3 minibots) sem hardware.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$(mktemp -d)"; PORT=$((20000 + RANDOM % 30000)); trap 'kill $PID 2>/dev/null || true; rm -rf "$T"' EXIT
mkdir -p "$T/hooks/minibot"
for h in start stop; do printf '#!/bin/sh\necho %s >> "%s/log"\nexit 0\n' "$h" "$T" > "$T/hooks/$h"; done
printf '#!/bin/sh\n[ -e "%s/fault" ] && exit 2\n[ -e "%s/fire" ] && exit 1\nexit 0\n' "$T" "$T" > "$T/hooks/gas"
printf '#!/bin/sh\necho contencao >> "%s/log"\nexit 0\n' "$T" > "$T/hooks/minibot/contencao"
# o extintor "apaga" o fogo na 2ª vez que roda (testa a repetição enquanto o gás indica fogo)
printf '#!/bin/sh\necho extintor >> "%s/log"\n[ "$(grep -c extintor "%s/log")" -ge 2 ] && rm -f "%s/fire"\nexit 0\n' "$T" "$T" "$T" > "$T/hooks/minibot/extintor"
printf '#!/bin/sh\n[ -e "%s/fire" ] && echo "restauracao com fogo!" >> "%s/log"\necho restauracao >> "%s/log"\nexit 0\n' "$T" "$T" "$T" > "$T/hooks/minibot/restauracao"
chmod 755 "$T"/hooks/start "$T"/hooks/stop "$T"/hooks/gas "$T"/hooks/minibot/*
python3 "$ROOT/examples/diy-robot/ilrdos_robot_agent.py" --hooks "$T/hooks" --port "$PORT" --bind 127.0.0.1 --clear-seconds 2 --fire-timeout 5 >"$T/agent.log" 2>&1 &
PID=$!; sleep 1.5
ask() { python3 - "$PORT" "$1" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 3); s.sendall((sys.argv[2] + "\n").encode()); print(s.makefile().readline().strip())
PY
}
fail=0
expect() { local got; got="$(ask "$1")"; if [[ "$got" == $2 ]]; then echo "[ok]   $1 -> $got"; else echo "[FALHA] $1 -> '$got' (esperado '$2')"; fail=1; fi; }
expect START "OK"
expect STATUS "OK working"
touch "$T/fault"; sleep 2.5; expect STATUS "OK working"; rm "$T/fault"      # falha do sensor não é fogo
touch "$T/fire"; sleep 1.5
expect STATUS "OK fire"                                                    # fogo: robô parado e protocolo em andamento
expect START "ERR incêndio em andamento"
expect STOP "OK"
sleep 7
expect STATUS "OK stopped"                                                 # fogo apagado e minibot 3 terminou
for w in contencao extintor restauracao; do grep -q "^$w$" "$T/log" && echo "[ok]   minibot $w rodou" || { echo "[FALHA] $w não rodou"; fail=1; }; done
[[ "$(grep -c '^extintor$' "$T/log")" -ge 2 ]] && echo "[ok]   minibot 2 repetiu enquanto havia fogo" || { echo "[FALHA] sem repetição"; fail=1; }
grep -q "com fogo" "$T/log" && { echo "[FALHA] minibot 3 rodou com fogo"; fail=1; } || echo "[ok]   minibot 3 só rodou com o fogo apagado"
[[ "$(grep -n '^restauracao$' "$T/log" | head -1 | cut -d: -f1)" -gt "$(grep -n '^extintor$' "$T/log" | tail -1 | cut -d: -f1)" ]] && echo "[ok]   ordem: extintor antes da restauração" || { echo "[FALHA] ordem"; fail=1; }
expect START "OK"                                                          # robô normal de novo
exit $fail

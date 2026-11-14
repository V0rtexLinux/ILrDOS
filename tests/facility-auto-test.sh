#!/usr/bin/env bash
# Autonomia da câmara central (perfil industrial): o ILrDOS decide sozinho, sem comando de terminal.
# Os comandos do terminal abaixo só LEEM o status (e simulam o alarme da planta); as decisões são do daemon.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -x "${ROOT}/build/ilrdosd" ]] || "${ROOT}/scripts/build-linux.sh"
TMP="$(mktemp -d)"; PORT=$((20000 + RANDOM % 30000))
openssl rand -hex 32 >"$TMP/token"; chmod 0600 "$TMP/token"
cat >"$TMP/iot.txt" <<'CFG'
device olho_1 http on=http://192.168.1.50/on off=http://192.168.1.50/off
device luz_geral http on=http://192.168.1.52/on off=http://192.168.1.52/off
device exaustor http on=http://192.168.1.53/on off=http://192.168.1.53/off
device painel_teto http on=http://192.168.1.51/open off=http://192.168.1.51/close
device alarme_x http on=http://192.168.1.60/on off=http://192.168.1.60/off
CFG
: >"$TMP/fleet.txt"
cat >"$TMP/facility.txt" <<'CFG'
auto idle_seconds=5 release_seconds=10
ring olho_1
panel painel_teto
lights luz_geral
vent exaustor
watch alarme_x on -> lockdown
CFG
echo 200 >"$TMP/sonar"; : >"$TMP/faces"
export ILRDOS_IOT_CONFIG="$TMP/iot.txt" ILRDOS_FLEET_CONFIG="$TMP/fleet.txt" ILRDOS_FACILITY_CONFIG="$TMP/facility.txt" ILRDOS_BODY_PROFILE=industrial \
       ILRDOS_FACE_DB="$TMP/faces.db" ILRDOS_SIM_FACES_FILE="$TMP/faces" ILRDOS_SIM_SONAR_FILE="$TMP/sonar" ILRDOS_SIM_MOTORS=1 ILRDOS_SIM_SILENT=1
"${ROOT}/build/ilrdosd" --simulate --autonomy --scan-interval 1 --bind 127.0.0.1 --port "$PORT" --token-file "$TMP/token" --config /nonexistent >"$TMP/daemon.log" 2>&1 &
PID=$!
cleanup() { kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; rm -rf "$TMP"; }
trap cleanup EXIT
ilr() { { cat "$TMP/token"; for c in "$@"; do echo "$c"; sleep 0.3; done; echo exit; } |
  "${ROOT}/build/ilrdos-client" 127.0.0.1 "$PORT" 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;]*m//g; s/ilrdos> //g'; }
fail=0
check() { if grep -qF -- "$2" <<<"$3"; then echo "ok   - $1"; else echo "FALHA - $1 (esperava: $2)"; echo "$3" | sed 's/^/        /' | tail -4; fail=1; fi; }
st() { ilr "facility status" | grep "câmara central"; }

sleep 6;  check "partida sozinha ao ligar" "partida" "$(st)"
echo "Maria 0.5 0.3" >"$TMP/faces"; sleep 4
check "chegou gente: luzes e anel sozinhos" "anel scan" "$(st)"
: >"$TMP/faces"; sleep 14
check "ninguém por um tempo: economia sozinha" "núcleo=economia" "$(st)"
echo "Maria 0.5 0.3" >"$TMP/faces"; sleep 4
check "voltou gente: sai da economia sozinha" "núcleo=normal" "$(st)"
ilr "iot alarme_x on" >/dev/null; sleep 7
check "alarme da planta: bloqueio sozinho" "bloqueio=SIM" "$(st)"
ilr "iot alarme_x off" >/dev/null; sleep 19
check "sem alarme: libera sozinho" "bloqueio=não" "$(st)"
exit $fail

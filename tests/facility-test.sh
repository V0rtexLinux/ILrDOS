#!/usr/bin/env bash
# Teste da Câmara Central de IA (estilo GLaDOS) em modo simulado: anel, painéis, cenas, bloqueio e modo seguro.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -x "${ROOT}/build/ilrdosd" ]] || "${ROOT}/scripts/build-linux.sh"
TMP="$(mktemp -d)"; PORT=$((20000 + RANDOM % 30000))
openssl rand -hex 32 >"$TMP/token"; chmod 0600 "$TMP/token"
cat >"$TMP/iot.txt" <<'CFG'
device olho_1 http on=http://192.168.1.50/on off=http://192.168.1.50/off alias=olho
device painel_teto http on=http://192.168.1.51/open off=http://192.168.1.51/close alias=painel
device luz_geral http on=http://192.168.1.52/on off=http://192.168.1.52/off alias=luz
device exaustor http on=http://192.168.1.53/on off=http://192.168.1.53/off alias=exaustor
device esteira http on=http://192.168.1.54/on off=http://192.168.1.54/off sensitive=1 alias=esteira
CFG
cat >"$TMP/fleet.txt" <<'CFG'
robot agv1 http start=http://192.168.1.40/start stop=http://192.168.1.40/stop dock=http://192.168.1.40/dock class=agv alias=agv interval=0
CFG
cat >"$TMP/facility.txt" <<'CFG'
zone camara_central
ring olho_1
panel painel_teto
lights luz_geral
vent exaustor
scene teste_1 : ring scan ; iot painel_teto on ; say Bem-vindo ao centro de enriquecimento.
scene linha_dois sensitive=1 : iot esteira on
scene startup : say Partida concluida.
CFG
echo 200 >"$TMP/sonar"; : >"$TMP/faces"
export ILRDOS_IOT_CONFIG="$TMP/iot.txt" ILRDOS_FLEET_CONFIG="$TMP/fleet.txt" ILRDOS_FACILITY_CONFIG="$TMP/facility.txt" ILRDOS_BODY_PROFILE="${PROFILE:-industrial}" \
       ILRDOS_FACE_DB="$TMP/faces.db" ILRDOS_SIM_FACES_FILE="$TMP/faces" ILRDOS_SIM_SONAR_FILE="$TMP/sonar" ILRDOS_SIM_MOTORS=1 ILRDOS_SIM_SILENT=1
"${ROOT}/build/ilrdosd" --simulate --bind 127.0.0.1 --port "$PORT" --token-file "$TMP/token" --config /nonexistent >"$TMP/daemon.log" 2>&1 &
PID=$!
cleanup() { kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; rm -rf "$TMP"; }
trap cleanup EXIT
sleep 0.5
ilr() { { cat "$TMP/token"; for c in "$@"; do echo "$c"; sleep 0.3; done; echo exit; } |
  "${ROOT}/build/ilrdos-client" 127.0.0.1 "$PORT" 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;]*m//g; s/ilrdos> //g'; }
fail=0
check() { if grep -qF -- "$2" <<<"$3"; then echo "ok   - $1"; else echo "FALHA - $1 (esperava: $2)"; echo "$3" | sed 's/^/        /'; fail=1; fi; }

out=$(ilr "facility status" "facility ring scan" "facility panels open" "facility scene teste_1" "facility scene linha_dois")
check "status da câmara" "câmara central: núcleo=normal" "$out"
check "anel de olhos" "Anel de olhos: scan" "$out"
check "cena de teste" "Câmara teste 1:" "$out"
out=$(ilr "facility core seguro" "hear abra os paineis" "facility core normal" "facility ring off")
check "moralidade bloqueia a voz" "Modo seguro ativo" "$out"
check "terminal remove a moralidade" "Núcleo normal" "$out"
out=$(ilr "hear bloqueio da fabrica" "facility ring scan" "facility release" "facility panels close")
check "bloqueio trava a instalação" "Instalação em bloqueio" "$out"
check "terminal libera" "Bloqueio liberado" "$out"
out=$(ilr "facility startup" "facility core economia" "hear ligue o anel de olhos" "facility core normal" "facility status")
check "partida" "Partida:" "$out"
check "economia desliga o não essencial" "Modo economia ativo" "$out"
check "economia bloqueia a voz" "Modo economia" "$out"
check "terminal restaura" "núcleo=normal" "$out"
out=$(ilr "help")
check "help lista facility" "facility (só perfil industrial)" "$out"
if [[ -z "${PROFILE:-}" ]]; then PROFILE=standard "$0" >/tmp/facility-std.out 2>&1 || true
  grep -q "so funciona no perfil industrial\|só funciona no perfil industrial" /tmp/facility-std.out && echo "ok   - perfil padrão recusa facility" || { echo "FALHA - perfil padrão deveria recusar"; fail=1; }
fi
exit $fail

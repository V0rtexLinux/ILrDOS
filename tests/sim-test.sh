#!/usr/bin/env bash
# Teste de ponta a ponta em modo simulado: conversa, movimento, IoT, rostos e autonomia (sem hardware).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -x "${ROOT}/build/ilrdosd" ]] || "${ROOT}/scripts/build-linux.sh"

TMP="$(mktemp -d)"; PORT=$((20000 + RANDOM % 30000))
openssl rand -hex 32 >"$TMP/token"; chmod 0600 "$TMP/token"
cat >"$TMP/iot.txt" <<'CFG'
device luz_sala http on=http://192.168.1.50/cm?cmnd=Power%20On off=http://192.168.1.50/cm?cmnd=Power%20Off alias=luz,lampada
device porta http on=http://192.168.1.51/open off=http://192.168.1.51/close sensitive=1 alias=porta
device ventilador mqtt host=192.168.1.10 topic=casa/ventilador alias=ventilador
rule arrive known:Maria -> ventilador on
rule empty -> luz_sala off
rule arrive known -> porta on
CFG
echo 200 >"$TMP/sonar"; : >"$TMP/faces"
export ILRDOS_IOT_CONFIG="$TMP/iot.txt" ILRDOS_FACE_DB="$TMP/faces.db" ILRDOS_SIM_FACES_FILE="$TMP/faces" \
       ILRDOS_SIM_SONAR_FILE="$TMP/sonar" ILRDOS_SIM_MOTORS=1 ILRDOS_SIM_SILENT=1
"${ROOT}/build/ilrdosd" --simulate --autonomy --scan-interval 1 --bind 127.0.0.1 --port "$PORT" \
  --token-file "$TMP/token" --config /nonexistent >"$TMP/daemon.log" 2>&1 &
PID=$!
cleanup() { kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; rm -rf "$TMP"; }
trap cleanup EXIT
sleep 0.5

# ilr "cmd1" "cmd2"... : uma sessão autenticada; sleep N usa o formato "@2" (segundos)
ilr() {
  { cat "$TMP/token"; for c in "$@"; do if [[ $c == @* ]]; then sleep "${c#@}"; else echo "$c"; sleep 0.3; fi; done; echo exit; } |
    "${ROOT}/build/ilrdos-client" 127.0.0.1 "$PORT" 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;]*m//g; s/ilrdos> //g'
}
fail=0
check() { # check "descrição" "texto esperado" "saída"
  if grep -qF -- "$2" <<<"$3"; then echo "ok   - $1"; else echo "FALHA - $1 (esperava: $2)"; echo "$3" | sed 's/^/        /'; fail=1; fi
}

out=$(ilr "status" "iot list" "iot luz_sala on" "iot porta on" "hear acende a luz da sala" "hear liga a porta" "hear desliga o ventilador")
check "status com motores simulados" "motores=simulados" "$out"
check "lista dispositivos" "porta[?](sensível: só terminal)" "$out"
check "terminal liga luz" "luz_sala: on." "$out"
check "terminal controla dispositivo sensível" "porta: on." "$out"
check "voz liga luz por apelido" "Feito: luz sala ligado." "$out"
check "voz NÃO controla dispositivo sensível" "sensível" "$(grep -F 'Resposta: ' <<<"$out" | sed -n 2p)"
check "voz desliga ventilador (MQTT)" "Feito: ventilador desligado." "$out"

out=$(ilr "hear anda pra frente" "echo" "hear gira pra esquerda noventa graus" "hear pare" "hear vai pra frente dois segundos")
check "voz anda" "Movendo para frente" "$out"
check "voz gira" "Girando ~90° para a esquerda" "$out"
check "voz para" "Parei." "$out"
echo 8 >"$TMP/sonar"; sleep 0.4
out=$(ilr "hear anda pra frente" "move forward 1000" "move back 1000")
check "sonar bloqueia avanço (voz)" "Obstáculo a 8 cm" "$out"
check "sonar bloqueia avanço (terminal)" "Obstáculo" "$out"
check "ré limitada, mas permitida" "Movendo para trás a 40% por 1000 ms" "$out"
echo 200 >"$TMP/sonar"

# --- rostos: cadastro com consentimento falado
echo "Maria 0.5 0.3" >"$TMP/faces"; sleep 3
out=$(ilr "hear quem sou eu" "hear guarde meu rosto" "hear meu nome é Maria" "hear nao" "hear guarde meu rosto meu nome é Maria" "hear eu confirmo" "@6" "face list" "hear quem sou eu" "hear quem esta aqui")
check "antes do cadastro: desconhecida" "Ainda não te reconheço" "$out"
check "pergunta o nome" "Qual é o seu nome?" "$out"
check "pede confirmação falada" "Para autorizar, diga: eu confirmo." "$out"
check "negar cancela" "não guardei nada" "$out"
check "cadastro iniciado" "Olhe para a câmera" "$out"
check "rosto salvo" "Maria" "$(grep 'Rostos cadastrados' <<<"$out")"
check "reconhece depois do cadastro" "Você é Maria." "$out"
[[ $(stat -c %a "$TMP/faces.db") == 600 ]] && echo "ok   - banco de rostos com permissão 0600" || { echo "FALHA - permissão do banco"; fail=1; }
! grep -q "^Maria .* .* .*" /dev/null || true
check "autonomia cumprimentou pelo nome" "Olá, Maria" "$(cat "$TMP/daemon.log")"
check "regra por pessoa conhecida disparou" "ventilador ligado" "$(cat "$TMP/daemon.log")"
check "regra automática NÃO controla dispositivo sensível" "ignorada: regras automáticas não podem controlar dispositivos sensíveis" "$(cat "$TMP/daemon.log")"

echo "Joao 0.5 0.3" >"$TMP/faces"; sleep 3
out=$(ilr "hear quem sou eu")
check "outra pessoa continua desconhecida" "Ainda não te reconheço" "$out"
echo "Maria 0.5 0.3" >"$TMP/faces"; sleep 3
out=$(ilr "hear esqueça meu rosto" "hear esqueça todos os rostos" "face list")
check "voz esquece o próprio rosto" "esqueci o rosto de Maria" "$out"
check "voz não apaga todos" "Só pelo terminal" "$out"
check "banco vazio" "Nenhum rosto cadastrado." "$out"

# --- rastreamento: pessoa à direita => gira
echo "Maria 0.85 0.3" >"$TMP/faces"; ilr "autonomy motion track" "@4" >/dev/null
check "rastreia rosto (gira)" "[motor simulado]" "$(cat "$TMP/daemon.log")"
echo "0" >"$TMP/faces"

# --- segurança de acesso
if printf '%s\n' 00000000000000000000000000000000 | "${ROOT}/build/ilrdos-client" 127.0.0.1 "$PORT" >"$TMP/denied.log" 2>&1; then
  echo "FALHA - token errado aceito"; fail=1; else check "token errado recusado" "Acesso negado" "$(cat "$TMP/denied.log")"; fi

(( fail == 0 )) && echo "TUDO OK" || { echo "--- log do daemon ---"; tail -40 "$TMP/daemon.log"; exit 1; }

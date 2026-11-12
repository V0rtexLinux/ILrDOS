// ILrDOS — robô feito por você, por Wi-Fi (ESP32 / ESP8266 / Pico W).
// O GLaDOS conecta na porta 7777, manda UMA palavra por linha e espera UMA linha de resposta:
//   START | STOP | PAUSE | DOCK | STATUS   ->   OK | OK <estado> | ERR <motivo>
// Estados válidos no STATUS: working, stopped, paused, docking.
//
// SEGURANÇA: o ILrDOS NÃO é circuito de segurança. Este firmware deve parar sozinho
// (limite de tempo, para-choque, sensor de queda). Ajuste MAX_RUN_MS ao seu robô.
#include <WiFi.h>            // ESP8266: troque por <ESP8266WiFi.h>

const char* WIFI_SSID = "SUA_REDE";
const char* WIFI_PASS = "SUA_SENHA";
const unsigned long MAX_RUN_MS = 60UL * 1000;   // para sozinho depois de 1 minuto

WiFiServer server(7777);
enum State { STOPPED, WORKING, PAUSED, DOCKING } state = STOPPED;
unsigned long startedAt = 0;

// ── ligue aqui o SEU robô ──────────────────────────────────────────────────
void robotStart()  { /* ex.: ligar motores */ }
void robotStop()   { /* ex.: desligar motores, SEMPRE funciona */ }
void robotPause()  { /* ex.: segurar posição */ }
void robotDock()   { /* ex.: voltar para a base */ }
bool robotSafe()   { return true; }             // ex.: !bumperPressionado && !quedaDetectada
// ───────────────────────────────────────────────────────────────────────────

const char* stateName() {
  switch (state) { case WORKING: return "working"; case PAUSED: return "paused"; case DOCKING: return "docking"; default: return "stopped"; }
}

String handle(const String& cmd) {
  if (cmd == "STOP")   { robotStop(); state = STOPPED; return "OK"; }       // STOP vence tudo
  if (cmd == "STATUS") { return String("OK ") + stateName(); }
  if (cmd == "START")  { if (!robotSafe()) return "ERR inseguro agora"; robotStart(); state = WORKING; startedAt = millis(); return "OK"; }
  if (cmd == "PAUSE")  { robotPause(); state = PAUSED; return "OK"; }
  if (cmd == "DOCK")   { robotDock(); state = DOCKING; return "OK"; }
  return "ERR comando desconhecido";
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(300);
  Serial.println(WiFi.localIP());                 // use este IP em host= no ilrdos-fleet.txt
  server.begin();
}

void loop() {
  if (state == WORKING && (!robotSafe() || millis() - startedAt > MAX_RUN_MS)) { robotStop(); state = STOPPED; }
  WiFiClient client = server.available();
  if (client) {
    client.setTimeout(1000);
    String cmd = client.readStringUntil('\n');
    cmd.trim();
    client.print(handle(cmd) + "\n");
    client.stop();
  }
}

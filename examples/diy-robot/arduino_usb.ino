// ILrDOS — robô feito por você, por cabo USB (Arduino Leonardo/Micro, Pico, ESP32 ou qualquer placa com UART).
// Mesmo protocolo do exemplo Wi-Fi: uma palavra por linha, uma linha de resposta (OK | OK <estado> | ERR <motivo>).
//
// Arduino Uno/Nano reiniciam quando a porta serial abre: use  wait=2000  na linha do robô.
// SEGURANÇA: o ILrDOS NÃO é circuito de segurança. Mantenha parada de emergência e limites no hardware.
const unsigned long MAX_RUN_MS = 60UL * 1000;
enum State { STOPPED, WORKING, PAUSED, DOCKING } state = STOPPED;
unsigned long startedAt = 0;

void robotStart() { digitalWrite(LED_BUILTIN, HIGH); }   // troque pelos seus motores
void robotStop()  { digitalWrite(LED_BUILTIN, LOW); }
void robotPause() { digitalWrite(LED_BUILTIN, LOW); }
void robotDock()  { digitalWrite(LED_BUILTIN, LOW); }

const char* stateName() {
  switch (state) { case WORKING: return "working"; case PAUSED: return "paused"; case DOCKING: return "docking"; default: return "stopped"; }
}

void setup() { pinMode(LED_BUILTIN, OUTPUT); Serial.begin(115200); }

void loop() {
  if (state == WORKING && millis() - startedAt > MAX_RUN_MS) { robotStop(); state = STOPPED; }
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd == "STOP")        { robotStop(); state = STOPPED; Serial.println("OK"); }
  else if (cmd == "STATUS") { Serial.print("OK "); Serial.println(stateName()); }
  else if (cmd == "START")  { robotStart(); state = WORKING; startedAt = millis(); Serial.println("OK"); }
  else if (cmd == "PAUSE")  { robotPause(); state = PAUSED; Serial.println("OK"); }
  else if (cmd == "DOCK")   { robotDock(); state = DOCKING; Serial.println("OK"); }
  else                      Serial.println("ERR comando desconhecido");
}

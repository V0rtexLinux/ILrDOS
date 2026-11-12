# Robôs feitos por você (tipo `diy`)

O GLaDOS/ILrDOS controla robôs que **você mesmo constrói** (carrinho, braço, rastreador, o que inventar) usando um
protocolo de texto bem simples. Qualquer placa que fale Wi-Fi (ESP32, ESP8266, Pico W) ou USB/serial (Arduino, Pico,
ESP32) serve.

## Como funciona

O ILrDOS manda **uma palavra por linha** e o robô responde **uma linha**:

| ILrDOS envia | Quando | Resposta do robô |
|---|---|---|
| `START` / `STOP` / `PAUSE` / `DOCK` | você diz "carrinho, começa", ou usa `fleet carrinho start` | `OK` ou `ERR <motivo>` |
| `STATUS` | `fleet status carrinho` | `OK working` (ou `stopped`, `paused`, `docking`) |

O robô só é marcado como iniciado/parado quando responde `OK`. Se responder `ERR ...`, o motivo aparece para você.

## Passo a passo

1. Grave `examples/diy-robot/esp32_wifi.ino` (Wi-Fi) ou `arduino_usb.ino` (USB) na sua placa e ligue seus motores nas
   funções `robotStart()`, `robotStop()` etc.
2. Descubra o IP que a placa mostra no Serial Monitor.
3. Declare o robô em `/boot/ilrdos-fleet.txt`:

```
# Wi-Fi (porta padrão 7777)
robot carrinho diy host=192.168.1.60 class=carrinho zone=sala alias=meu_carrinho,carrinho

# USB (Arduino Uno/Nano reiniciam ao abrir a porta: use wait=2000)
robot rover diy serial=/dev/ttyACM0 baud=115200 wait=2000 class=rover alias=rover
```

4. Reinicie o ilrdosd e teste: `fleet list`, `fleet carrinho start`, `fleet status carrinho`, `fleet carrinho stop`.

Sem hardware, teste com o robô falso: `python3 examples/diy-robot/fake_robot.py 7777` e `host=127.0.0.1`.

## Opções da linha `robot ... diy`

`host=` (Wi-Fi) ou `serial=` (USB) · `port=` (7777) · `baud=` (115200) · `wait=` (ms de espera após abrir a serial) ·
`start_payload=`, `stop_payload=`, `pause_payload=`, `dock_payload=` (trocam as palavras de comando, só letras/números/`_-.:`;
deixe vazio para dizer que o robô não tem aquela ação) · e as comuns da frota: `class=`, `zone=`, `alias=`, `confirm=1`,
`sensitive=1`, `interval=`, `max_run=`.

## Regras de segurança (as mesmas da frota)

- `STOP` sempre funciona, de qualquer origem. `PAUSE` e `DOCK` também.
- `START` de robôs `confirm=1`, ou de classes pesadas (braço, esteira, cortador...) só com **"eu confirmo"** por voz ou pelo
  terminal autenticado; nunca por regra automática ou marca do modelo de linguagem.
- Dois robôs na mesma `zone=` não trabalham juntos.
- Só endereços da rede local; só os robôs declarados no arquivo.
- **O ILrDOS não é circuito de segurança.** Faça seu firmware parar sozinho (limite de tempo, para-choque, sensor de queda)
  e mantenha parada de emergência em hardware.

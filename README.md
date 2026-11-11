# ILrDOS — Intelligent Lifeform robot Disk Operating System

Runtime do robô em C++ (Raspberry Pi OS ou a imagem própria em `os/`). Desde a 2.0.0 o robô fala, ouve, anda, controla dispositivos IoT, conversa, cadastra e reconhece rostos — tudo decidido pelo próprio robô. A arquitetura alvo é o Raspberry Pi Zero 2 W e os GPIO 18 (olho) / 23 (alerta vermelho) descritos no material original.

## Usar como biblioteca (open source, MIT)

O ILrDOS é uma biblioteca C++20 *header-only* (`include/ilrdos/`) e o `ilrdosd` é só um programa que a usa. Qualquer projeto pode usar a estrutura do robô, a frota, o IoT etc. como API:

```cmake
include(FetchContent)
FetchContent_Declare(ilrdos GIT_REPOSITORY https://github.com/V0rtexLinux/ILrDOS.git GIT_TAG v2.2.0)
FetchContent_MakeAvailable(ilrdos)

add_executable(meu_robo main.cpp)
target_link_libraries(meu_robo PRIVATE ilrdos::ilrdos)
```

```cpp
#include <ilrdos/ilrdos.hpp>
```

Ou instale e use `find_package(ilrdos CONFIG REQUIRED)` (`cmake --install build-cmake`). Opções do CMake: `ILRDOS_WITH_OPENCV`, `ILRDOS_WITH_ALSA`, `ILRDOS_WITH_VOSK`, `ILRDOS_BUILD_DAEMON`. Dependência obrigatória: OpenSSL. Veja [CONTRIBUTING.md](CONTRIBUTING.md) para modificar e contribuir.

## Frota: o ILrDOS coordenando outros robôs (2.2.0)

O ILrDOS agora comanda **outros robôs**: aspiradores, esfregões, cortadores de grama, robôs de piscina e também máquinas de indústria (AGVs, esteiras, braços, empilhadeiras). Ele não pilota cada robô; manda comandos de alto nível pelo canal que o robô já tem e coordena quem trabalha onde e quando. As ações são sempre as mesmas, para qualquer marca: **start** (iniciar), **stop** (parar), **pause** (pausar) e **dock** (voltar para a base/carregar).

| Canal | Serve para |
|---|---|
| `http` | Home Assistant, Roborock/Xiaomi via integração, qualquer API local |
| `mqtt` | Valetudo (padrões START/STOP/PAUSE/HOME), Tasmota, ESPHome |
| `modbus` / `rtu` | CLPs, AGVs, esteiras, robôs industriais (TCP ou RS-485 por cabo) |
| `ble` | Robôs de brinquedo e pequenos robôs Bluetooth LE |

Configuração em `ilrdos-fleet.txt` (BOOT, ou `/etc/ilrdos/fleet.txt`; exemplo comentado em `os/board/ilrdos/ilrdos-fleet.txt.example`):

```
robot aspirador mqtt host=192.168.1.10 topic=valetudo/aspirador/BasicControlCapability/operation/set class=aspirador zone=casa alias=aspirador
robot esfregao  http start=http://192.168.1.20/start stop=http://192.168.1.20/stop dock=http://192.168.1.20/dock class=esfregao zone=casa alias=esfregao,mop
robot agv1      modbus host=192.168.1.40 unit=1 coil_start=0 coil_stop=1 class=agv zone=corredor_a alias=agv
group limpeza aspirador,esfregao alias=limpeza,casa
rule empty -> aspirador start          # ninguém à vista: manda limpar
rule arrive any -> aspirador dock      # chegou alguém: o aspirador volta para a base
rule arrive unknown -> agv1 stop       # desconhecido na área: para o AGV
```

**O que a coordenação faz**
- **Voz, sem PC:** *“inicia o aspirador”*, *“manda o esfregão limpar”*, *“aspirador, volta pra base”*, *“como está o aspirador?”*, *“limpa a casa”* (grupo), *“pare tudo”* / *“emergência”* (para a frota inteira ao mesmo tempo).
- **Regras por presença:** o robô vê e ouve a casa e decide: sai todo mundo, o aspirador começa; chega alguém, ele volta para a base.
- **Zonas (`zone=`):** dois robôs da mesma zona não trabalham juntos (aspirador e esfregão na mesma sala, dois AGVs no mesmo corredor). O segundo espera o primeiro parar ou passar de `max_run` minutos.
- **Grupos:** um comando para vários robôs; o `stop` de grupo e o `pare tudo` vão em paralelo, então um robô fora do ar não atrasa os outros.
- **Terminal:** `fleet list`, `fleet status [nome]`, `fleet stop all`, `fleet <robô|grupo> start|stop|pause|dock`.

**Segurança da frota**
- **`stop`, `pause` e `dock` valem sempre**, de qualquer origem (voz, regra, IA, terminal), sem confirmação nem intervalo mínimo: são a direção segura. Todo robô precisa declarar `start` e `stop`.
- **`start` de máquina pesada** (`class=` industrial, agv, amr, braço, esteira, empilhadeira, cnc, máquina…), de **cortador de grama** (lâmina) ou de robô com `confirm=1` só acontece por voz com **“eu confirmo”** (um robô por vez) ou pelo terminal autenticado. Nunca por regra automática, por marca do modelo de IA nem por grupo.
- **`sensitive=1`:** só o terminal autenticado inicia o robô.
- Celulares, roteadores e computadores nunca entram; endereços só da rede local; só existem os robôs declarados no arquivo (nada de URL vinda de fala ou da rede).
- O estado mostrado é o do último comando, ou o que o próprio robô informar (`status=`/`status_reg=`); textos vindos do robô são tratados como dado.
- **O ILrDOS não substitui parada de emergência, cortina de luz nem intertravamento.** O `stop` da frota é um comando de rede e pode falhar se o robô estiver fora do ar; em indústria, a segurança continua em hardware.

## Estrutura do robô (2.1.0)

O OS segue o desenho do projeto. Cada peça vira uma entrada de `include/ilrdos/structure.hpp` com estado (`ok`, `atencao`, `falha` ou `sem-sensor`):

```
[cabos de eletricidade principais  - / +] --> [cérebro / computador principal]
[visor / câmera] == cabos de transmissão de dados ==> [cérebro / computador principal]
[casco protetor] envolve o cérebro
```

| Peça do desenho | O que o OS verifica |
|---|---|
| **Cabos de eletricidade principais (− e +)** | Aviso de tensão baixa do firmware do Pi (`get_throttled` ou hwmon `rpi_volt`). Sem esse sensor: `sem-sensor`. Caminho alternativo: `THROTTLED_FILE`. |
| **Cérebro / computador principal** | Temperatura da CPU, carga e tempo ligado. Quente = atenção; crítico = falha. |
| **Cabos de transmissão de dados (câmera/visor → computador)** | O dispositivo `/dev/video<N>` existe e a câmera entrega imagem. |
| **Casco protetor** | Sensor de impacto (queda/batida): para os motores e emite 3 bips suaves (~900 Hz, 30 % duty, não ensurdece). Estado `atencao` por 30 s após a queda. Também monitora temperatura interna (rodas paradas e visão reduzida quando esquenta). |
| **Visor / câmera** | Câmera aberta, rostos na última varredura, se o reconhecimento está ativo. |

Terminal: `structure` (todas as peças), `structure map` (o desenho em texto) e `structure power|brain|datalink|casing|visor` (uma peça). O `status` mostra `estrutura=...` e a pergunta falada *“como você está?”* cita só o que não está bem.

## Novidades da 2.0.0 (robô autônomo)

| Capacidade | Como funciona |
|---|---|
| **Conversar** | Fica ouvindo (Vosk PT-BR no próprio Pi). Diga *“ILrDOS, …”*; depois de falar ele aceita respostas por 10 s sem repetir o nome. Comandos conhecidos são entendidos localmente (funciona sem PC); o resto vai para o Ollama no PC. Sem PC, ainda obedece comandos e se apresenta. |
| **Falar** | TTS do PC (GLaDOS/Kokoro, porta 5050) e `espeak` local como reserva. |
| **Se mexer** | Ponte H + sonar HC-SR04. Ele encara quem fala/está na frente (`track`), se aproxima parando longe e passeia sozinho (`roam`). Limites de segurança na seção abaixo. |
| **IoT** | `ilrdos-iot.txt`: HTTP local (Tasmota, Shelly, Home Assistant…), MQTT e relés em GPIO, mais regras (*chegou alguém → liga a luz*). |
| **Rostos** | SFace (OpenCV): guarda só vetores numéricos, nunca fotos. Cumprimenta pelo nome e dispara regras por pessoa. |
| **Autonomia** | Cumprimenta, rastreia, passeia, regula visão pelo calor, reinicia Wi‑Fi e avisa falando quando algo falha. |

Configuração: `ilrdos-robot.txt` (pinos, PC, limites), `ilrdos-iot.txt` (dispositivos e regras) e `ilrdos-fleet.txt` (outros robôs) na partição BOOT (ou `/etc/ilrdos/` no Pi OS). Exemplos comentados em `os/board/ilrdos/`.

### Falas que ele entende sem o PC
*“inicia o aspirador”*, *“aspirador, volta pra base”*, *“pare tudo”*, *“anda pra frente dois segundos”*, *“gira pra esquerda noventa graus”*, *“pare”*, *“acende a luz da sala”*, *“desliga o ventilador”*, *“quem sou eu?”*, *“quem está aqui?”*, *“quem você conhece?”*, *“guarde meu rosto”*, *“esqueça meu rosto”*, *“como você está?”*.

### Rostos e privacidade (dado biométrico)
- Cadastro só com consentimento: pelo terminal (`face enroll Maria`) ou por voz, quando ele pergunta o nome, repete que vai guardar e exige você dizer **“eu confirmo”**. Só cadastra com **uma** pessoa na frente. Nunca cadastra desconhecidos sozinho.
- Fica no cartão SD (`ilrdos-faces.db` na BOOT; `/var/lib/ilrdos/faces.db` no Pi OS), permissão 0600. Apagar o arquivo apaga tudo. `face forget <nome|all>` pelo terminal; por voz só *“esqueça meu rosto”* (e só se ele estiver te reconhecendo).
- O reconhecimento **não é autenticação**: dispositivos `sensitive=1` (portas, portões…) só obedecem ao terminal autenticado, nunca a voz, IA, regras ou rosto (uma foto enganaria a câmera).
- Em feira/escola, avise as pessoas e só cadastre quem concordar (LGPD trata biometria como dado sensível; menores precisam de autorização dos responsáveis).
- `SEND_NAMES_TO_LLM=0` impede que os nomes cheguem ao Ollama.

### Segurança do movimento
Todo movimento tem prazo (máx. 3 s) e para sozinho se o daemon/rede cair; velocidade limitada (`MOTOR_MAX_SPEED`, padrão 60%); ré mais curta e lenta (sem sensor traseiro); para a frente o sonar para o robô abaixo de `SONAR_STOP_CM` e **sonar sem leitura = não anda**; **movimento respeita a estrutura do desenho**: o giro acumulado é contado e limitado a `MAX_TWIST_DEG` (padrão 270°) para os cabos de energia -/+ e de dados do visor não esticarem (no limite só gira para o lado de volta; `untwist` desfaz a torção, `untwist zero` zera depois de conferir os cabos à mão); partida e parada suaves em curva smoothstep, como a carcaça da GLaDOS (pendurada por cabos, balança devagar e sem tranco; `MOTOR_RAMP_MS`); inversão de sentido só depois de uma pausa (`REVERSE_SETTLE_MS`); depois de uma batida no casco não move por `IMPACT_LOCKOUT_MS`; tensão baixa nos cabos de energia ou calor crítico no computador principal = não anda (a torção é estimada por tempo, sem encoder, e fica salva em `TWIST_FILE`); **autopercepção do corpo**: o robô sente cada peça do desenho em primeira pessoa (`structure`, "como você está?", "você é consciente?"), e essa sensação entra no contexto da conversa; ele é honesto ao dizer que tem um modelo de si mesmo, não que prova consciência; no desenho o visor fica na ponta de um corpo longo e olha só para a frente, então giros usam rampa 50% mais longa e a ré é lenta e curta (30%, 1 s); **casa e indústria**: além de Wi-Fi (HTTP/MQTT/Home Assistant) e relés GPIO, controla por cabo equipamentos Modbus (TCP e RS-485/RTU, com o estado conferido de volta e intervalo mínimo entre comandos), lê sensores em cabo (contatos de porta/janela e presença em GPIO, temperatura 1-Wire, registradores Modbus: `sensor ...`, `iot read`, \"qual a temperatura da caldeira?\") e liga atuadores Bluetooth LE declarados; máquinas com `confirm=1` só ligam por voz com \"eu confirmo\" e nunca por regra ou pelo modelo; celulares, roteadores e computadores são sempre recusados; o ILrDOS não substitui parada de emergência nem intertravamentos de segurança; passeio autônomo só existe com sonar; `pare`/`stop` vencem tudo; calor crítico para as rodas; **queda detectada pelo sensor do casco para os motores imediatamente** e emite alerta sonoro suave (3 × 220 ms a ~900 Hz, intervalo de 5 s entre alertas). Fala é tratada como dado: o modelo de linguagem só consegue ações da lista permitida, com os mesmos limites. **Teste primeiro com as rodas no ar** e mantenha um interruptor físico de bateria à mão. O sonar HC-SR04 usa 5 V: ligue o ECHO ao Pi por divisor de tensão (ex.: 1 kΩ + 2 kΩ) para não queimar o GPIO.

### Comandos novos do terminal
`fleet list|status [nome]|stop all|<robô|grupo> start|stop|pause|dock`, `face list|who|enroll <nome>|forget <nome|all>|cancel`, `move forward|back|left|right [ms] [vel]`, `turn left|right [graus]`, `untwist [zero]`, `stop`, `iot list|<disp> on|off|toggle`, `say <texto>`, `hear <texto>` (simula uma fala ouvida), `voice status`, `structure [map|power|brain|datalink|casing|visor]`, `impact` (estado do sensor de queda), `impact test` (dispara alerta manualmente), `autonomy motion off|track|roam`.

### Teste sem hardware
`tests/sim-test.sh` sobe o daemon em `--simulate` e exercita conversa, movimento, sonar, IoT, regras, cadastro/reconhecimento de rosto e rastreamento.

## O que já existia (1.x)

- Serviço Linux que inicia com o Raspberry Pi.
- Padrões de LED `idle`, `listen`, `think`, `speak`, `alert` e `off`.
- Console remoto para Windows pela rede local, com autenticação por desafio HMAC-SHA256.
- Comandos permitidos: `help`, `status`, `eye <estado>`, `vision status`, `vision scan`, `autonomy on|off|status` e `exit`.
- Autonomia (v1.1.0): o daemon reage a rostos e cuida de si (calor, Wi‑Fi, câmera) sozinho; veja `os/README.md`.
- Detector facial YuNet em C++/OpenCV; nenhuma imagem é salva (desde a 2.0.0 também identifica rostos cadastrados com consentimento).
- Ponte de voz opcional em Python: Vosk + Silero VAD no Pi, Ollama no PC e TTS GLaDOS remoto com `espeak` como alternativa.
- O assistente usa contexto curto em memória e responde com foco educativo; o texto transcrito segue apenas para o Ollama configurado na rede local.
- Nenhum comando de shell é executado pela rede.
- Modo `--simulate` para demonstrar sem o hardware conectado.

O daemon e o controle do robô permanecem em C++; a ponte de voz usa Python porque reaproveita as bibliotecas de áudio/Vosk do pacote Pi. Ela não controla GPIO diretamente: envia somente comandos permitidos ao daemon autenticado.

## Teste rápido no computador

Linux com `g++`, headers do Linux e OpenSSL de desenvolvimento:

```sh
cd robot/ilrdos
./scripts/build-linux.sh
umask 077
openssl rand -hex 32 > /tmp/ilrdos-token
./build/ilrdosd --simulate --bind 127.0.0.1 --port 4317 --token-file /tmp/ilrdos-token
```

Em outro terminal, execute `./build/ilrdos-client 127.0.0.1 4317`, digite o token e teste `status`, `eye alert` e `exit`.

## Instalar no Raspberry Pi Zero 2 W

Use Raspberry Pi OS 64-bit e uma rede Wi‑Fi privada. Copie `robot/ilrdos` para o Pi e rode:

```sh
sudo apt update
sudo apt install -y build-essential libssl-dev pkg-config libopencv-dev
cd robot/ilrdos
./scripts/install-pi.sh
hostname -I
```

O instalador compila o serviço no próprio Pi, instala o modelo YuNet e uma unidade `systemd`. OpenCV 4 é necessário para a detecção; sem ele, o daemon continua funcionando, mas responde que a visão não está compilada. Conecte uma câmera USB compatível com V4L2 em `/dev/video0`; o código não grava a imagem.

**Nota 2.0.0:** a conversa agora é nativa no daemon C++ (veja acima); a ponte Python `voice/` abaixo é o caminho antigo e não deve rodar junto (ambas usariam o microfone).

O modelo de conversa não está dentro dos ZIPs e o Pi Zero 2 W não foi configurado para rodar um LLM grande. A voz usa Ollama no computador da feira. Para instalar a ponte no Pi:

```sh
./scripts/install-voice-pi.sh
sudo nano /etc/ilrdos/voice.env
```

Troque `ILRDOS_PC_IP` pelo IP do PC. No PC, inicie Ollama, baixe o modelo indicado em `ILRDOS_OLLAMA_MODEL` (`qwen2.5:7b-instruct` por padrão) e permita acesso apenas pela rede Wi‑Fi privada da feira. Depois:

```sh
sudo systemctl enable --now ilrdos-voice
sudo journalctl -u ilrdos-voice -f
```

O instalador da voz baixa o modelo Vosk PT-BR pequeno (~40 MB) da distribuição oficial porque ele não está anexado e instala o ONNX Silero VAD enviado. O TTS GLaDOS remoto na porta 5050 é opcional; sem ele, o Pi tenta `espeak`. Os modelos de fonemização/GLaDOS anexados são em inglês; eles não alteram a personalidade das respostas nem oferecem fala PT-BR adequada.

Para usar a voz GLaDOS original no PC, extraia `glados-feira.zip`, coloque `glados.onnx` e `phomenizer_en.onnx` em `models/TTS/` junto aos arquivos JSON/PKL do ZIP, e execute:

```bat
robot\ilrdos\scripts\start-glados-tts-windows.bat C:\caminho\para\glados-feira
```

O serviço usa os pesos de voz GLaDOS e responde ao endpoint que a ponte do Pi já chama. É apenas a voz: a personalidade ILrDOS vem do prompt do Ollama. Esse TTS foi treinado para inglês; a qualidade da pronúncia em português pode ser limitada. No Windows, permita a porta 5050 somente na rede privada local.

O arquivo Kokoro também pode ser usado no PC: coloque `kokoro-v1.0.fp16.onnx`, `kokoro-voices-v1.0.bin` (do anexo anterior) e `phomenizer_en.onnx` em `models/TTS/`; então execute:

```bat
robot\ilrdos\scripts\start-kokoro-tts-windows.bat C:\caminho\para\glados-feira
```

No Pi, configure `ILRDOS_TTS_VOICE=af_alloy` em `/etc/ilrdos/voice.env` e reinicie `ilrdos-voice.service`. Para voltar à voz GLaDOS, use `ILRDOS_TTS_VOICE=glados` com o iniciador GLaDOS. Kokoro também usa fonemização em inglês e não é otimizado para o Pi Zero 2 W.

Para consultar o token no Pi sem alterá-lo: `sudo cat /etc/ilrdos/token`. Não o mostre no telão nem envie por chat.

Fiação conforme o arquivo original `FIACAO.md`: GPIO BCM 18 ao LED do olho e BCM 23 ao LED vermelho, sempre com resistor de 220 Ω em série; GND comum. Não conecte LED diretamente sem resistor.

## Cliente Windows

No Windows, abra o **Developer Command Prompt for Visual Studio** e execute:

```bat
cd robot\ilrdos
scripts\build-windows.bat
build\ILrDOS-Terminal.exe 192.168.1.50 4317
```

Troque `192.168.1.50` pelo endereço exibido por `hostname -I` no Raspberry Pi. O cliente pede o token sem exibi-lo. MSVC/Visual Studio Build Tools com SDK do Windows são necessários para gerar o `.exe`.

## Segurança para a feira

- Mantenha Pi e computador na mesma rede privada sob seu controle, de preferência um hotspot dedicado com senha WPA2/WPA3.
- Não faça port-forward da porta 4317 nem use Wi‑Fi público.
- O token é validado por desafio HMAC e não é enviado pela rede; limite os comandos a estados do olho e leitura de status.
- O protocolo não cifra o conteúdo da sessão. Use-o apenas numa rede local confiável.
- O serviço recusa um arquivo de token legível por grupo/outros e limita tentativas de autenticação por endereço.
- O Vosk transcreve áudio no Pi; perguntas em texto e contexto de detecção facial seguem para o Ollama/TTS configurado no PC. Nenhum frame é enviado ou salvo.

## Arquivos de entrada

Os arquivos incluem YuNet para rostos e Silero VAD para atividade de voz, ambos integrados. O Parakeet v2 agora tem encoder e decoder, mas ainda falta o joiner v2 correspondente; os pesos v3 enviados também não formam um conjunto completo. Não misture o joiner v3 com o v2. O `vision_encoder_fp16` é apenas um encoder visual, sem o modelo de linguagem necessário para descrever imagens. GLaDOS e Kokoro podem sintetizar fala no PC, mas usam fonemização em inglês. A personalidade ILrDOS é definida pelo prompt do Ollama. O reconhecimento PT-BR continua usando Vosk baixado separadamente.

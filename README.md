# ILrDOS — Intelligent Lifeform robot Disk Operating System

Runtime do robô em C++ (Raspberry Pi OS ou a imagem própria em `os/`). Desde a 2.0.0 o robô fala, ouve, anda, controla dispositivos IoT, conversa, cadastra e reconhece rostos — tudo decidido pelo próprio robô. A arquitetura alvo é o Raspberry Pi Zero 2 W e os GPIO 18 (olho) / 23 (alerta vermelho) descritos no material original.

## Novidades da 2.0.0 (robô autônomo)

| Capacidade | Como funciona |
|---|---|
| **Conversar** | Fica ouvindo (Vosk PT-BR no próprio Pi). Diga *“ILrDOS, …”*; depois de falar ele aceita respostas por 10 s sem repetir o nome. Comandos conhecidos são entendidos localmente (funciona sem PC); o resto vai para o Ollama no PC. Sem PC, ainda obedece comandos e se apresenta. |
| **Falar** | TTS do PC (GLaDOS/Kokoro, porta 5050) e `espeak` local como reserva. |
| **Se mexer** | Ponte H + sonar HC-SR04. Ele encara quem fala/está na frente (`track`), se aproxima parando longe e passeia sozinho (`roam`). Limites de segurança na seção abaixo. |
| **IoT** | `ilrdos-iot.txt`: HTTP local (Tasmota, Shelly, Home Assistant…), MQTT e relés em GPIO, mais regras (*chegou alguém → liga a luz*). |
| **Rostos** | SFace (OpenCV): guarda só vetores numéricos, nunca fotos. Cumprimenta pelo nome e dispara regras por pessoa. |
| **Autonomia** | Cumprimenta, rastreia, passeia, regula visão pelo calor, reinicia Wi‑Fi e avisa falando quando algo falha. |

Configuração: `ilrdos-robot.txt` (pinos, PC, limites) e `ilrdos-iot.txt` (dispositivos e regras) na partição BOOT (ou `/etc/ilrdos/` no Pi OS). Exemplos comentados em `os/board/ilrdos/`.

### Falas que ele entende sem o PC
*“anda pra frente dois segundos”*, *“gira pra esquerda noventa graus”*, *“pare”*, *“acende a luz da sala”*, *“desliga o ventilador”*, *“quem sou eu?”*, *“quem está aqui?”*, *“quem você conhece?”*, *“guarde meu rosto”*, *“esqueça meu rosto”*, *“como você está?”*.

### Rostos e privacidade (dado biométrico)
- Cadastro só com consentimento: pelo terminal (`face enroll Maria`) ou por voz, quando ele pergunta o nome, repete que vai guardar e exige você dizer **“eu confirmo”**. Só cadastra com **uma** pessoa na frente. Nunca cadastra desconhecidos sozinho.
- Fica no cartão SD (`ilrdos-faces.db` na BOOT; `/var/lib/ilrdos/faces.db` no Pi OS), permissão 0600. Apagar o arquivo apaga tudo. `face forget <nome|all>` pelo terminal; por voz só *“esqueça meu rosto”* (e só se ele estiver te reconhecendo).
- O reconhecimento **não é autenticação**: dispositivos `sensitive=1` (portas, portões…) só obedecem ao terminal autenticado, nunca a voz, IA, regras ou rosto (uma foto enganaria a câmera).
- Em feira/escola, avise as pessoas e só cadastre quem concordar (LGPD trata biometria como dado sensível; menores precisam de autorização dos responsáveis).
- `SEND_NAMES_TO_LLM=0` impede que os nomes cheguem ao Ollama.

### Segurança do movimento
Todo movimento tem prazo (máx. 3 s) e para sozinho se o daemon/rede cair; velocidade limitada (`MOTOR_MAX_SPEED`, padrão 60%); ré mais curta e lenta (sem sensor traseiro); para a frente o sonar para o robô abaixo de `SONAR_STOP_CM` e **sonar sem leitura = não anda**; passeio autônomo só existe com sonar; `pare`/`stop` vencem tudo; calor crítico para as rodas. Fala é tratada como dado: o modelo de linguagem só consegue ações da lista permitida, com os mesmos limites. **Teste primeiro com as rodas no ar** e mantenha um interruptor físico de bateria à mão. O sonar HC-SR04 usa 5 V: ligue o ECHO ao Pi por divisor de tensão (ex.: 1 kΩ + 2 kΩ) para não queimar o GPIO.

### Comandos novos do terminal
`face list|who|enroll <nome>|forget <nome|all>|cancel`, `move forward|back|left|right [ms] [vel]`, `turn left|right [graus]`, `stop`, `iot list|<disp> on|off|toggle`, `say <texto>`, `hear <texto>` (simula uma fala ouvida), `voice status`, `autonomy motion off|track|roam`.

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

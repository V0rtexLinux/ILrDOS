# ILrDOS OS — imagem de sistema para Raspberry Pi Zero 2 W

Em vez de instalar o ILrDOS sobre o Raspberry Pi OS, esta pasta gera uma **imagem de cartão SD própria**
(Buildroot + kernel Linux da Raspberry Pi). O Pi liga direto no ILrDOS: sem desktop, sem login, sem apt.

## O que a imagem faz

- Kernel Linux 64-bit + firmware Wi-Fi do Zero 2 W + OpenCV (YuNet) + `ilrdosd` compilado para o Pi.
- Raiz **somente leitura** (queda de energia não corrompe o cartão).
- Primeira inicialização gera o token em `ilrdos-token` na partição **BOOT** (legível no PC com o cartão).
- Wi-Fi vem de `ilrdos.txt` na partição BOOT (`WIFI_SSID`, `WIFI_PASS`, `WIFI_COUNTRY`), rede 2,4 GHz.
- O DHCP anuncia o hostname `ilrdos`; o IP também aparece no console HDMI (vermelho/preto).
- `ilrdosd` roda sob `respawn` do init na porta 4317, mesmo protocolo e mesmo `ILrDOS-Terminal.exe`.
- Sem shell remoto. Para depurar com teclado + HDMI, acrescente `ilrdos.debug` ao `cmdline.txt` da partição BOOT.

## Autonomia (v1.1.0)

O `ilrdosd` agora decide sozinho, sem ninguém pedir (ligada por padrão; `--no-autonomy` desliga):

- **Pessoas:** varre a câmera a cada 4 s (`--scan-interval`). Com rosto em 2 varreduras seguidas o olho vai para `listen`;
  sem rosto em 3 seguidas volta para `idle`. Só detecta rostos: não identifica ninguém nem guarda imagem.
- **Atenção:** sozinho por 5 min, faz um pulso `think` de 4 s.
- **Calor:** a CPU em 80 °C reduz as varreduras (4x mais lentas); em 85 °C pausa a visão e pisca o alerta vermelho até esfriar (< 75 °C).
- **Wi‑Fi:** se `wlan0` ficar fora do ar por mais de 60 s, reinicia o Wi‑Fi (no máximo uma vez a cada 2 min).
- **Câmera:** 5 varreduras seguidas com erro viram falha; o alerta pisca 5 s por minuto e a leitura tenta de novo a cada 30 s.
- **Controle:** `autonomy on|off|status` no terminal. Um `eye ...` manual vale por 60 s antes de a autonomia reassumir.

Logs: `dmesg | grep autonomia`.

## v2.0.0 na imagem

Conversa, voz, locomoção, IoT e rostos rodam dentro do `ilrdosd`. Na BOOT ficam `ilrdos-robot.txt` (pinos, PC/Ollama),
`ilrdos-iot.txt` (dispositivos/regras) e, depois do primeiro cadastro, `ilrdos-faces.db` (vetores, não fotos; a BOOT é
remontada em escrita só por instantes). Áudio: placa USB (`snd-usb-audio`), escolhida automaticamente.
A build baixa libvosk (aarch64) e o modelo PT-BR do Vosk (`package/ilrdos-vosk`) e o SFace (`scripts/fetch-models.sh`).

## Gerar a imagem (Linux, WSL2 ou Docker; ~15 GB livres; a primeira build leva horas)

```sh
sudo apt install build-essential curl xz-utils unzip rsync cpio bc file patch perl python3 libncurses-dev
./os/build.sh
```

Resultado em `~/ilrdos-os-build/dist/ILrDOS-1.0.0-pizero2w.img.xz`. Variáveis: `BUILDROOT_VERSION`
(padrão 2026.08), `BASE_DEFCONFIG`, `ILRDOS_WORK`, `BUILDROOT_SHA256`.

## Gravar e usar

1. Grave o `.img.xz` com Raspberry Pi Imager (“Usar personalizado”) ou balenaEtcher.
2. Abra a partição **BOOT** no PC e preencha `ilrdos.txt`.
3. Coloque o cartão no Pi, ligue, espere ~1 min e leia o IP no HDMI (ou no roteador, host `ilrdos`).
4. No PC: `ILrDOS-Terminal.exe <IP> 4317`; o token está em `ilrdos-token` na BOOT.

## Estado

A voz Python não é usada aqui: foi portada para C++ dentro do daemon.
**Testado só em modo simulado (`tests/sim-test.sh`).** Ainda NÃO testados no hardware/na build do Buildroot: câmera/OpenCV,
SFace, ALSA/Vosk, PWM dos motores, sonar, GPIO, remontagem da BOOT, hashes dos downloads (`ilrdos-vosk` não tem `.hash`) e
a compatibilidade da libvosk pré-compilada com a toolchain.
O daemon roda como root (sem queda de privilégio). A rede continua sem cifra; use só em rede privada.
Estrutura: `configs/` (fragmento sobre o defconfig da Raspberry), `package/ilrdos/` (pacote Buildroot),
`board/ilrdos/` (overlay do rootfs, scripts de build/boot).

# Robôs feitos por você (tipo `diy`)

O GLaDOS/ILrDOS controla robôs que **você mesmo constrói**, desde que o "cérebro" do robô seja um **computador de placa
única com Linux**: Raspberry Pi (qualquer modelo), Orange Pi, Banana Pi, Radxa/Rock, Odroid, Jetson, BeagleBone,
VisionFive, Khadas, mini PC x86... Serve qualquer placa que rode Linux e Python 3.8+ (ARM, x86 ou RISC-V). Não precisa
instalar bibliotecas.

## Como funciona

No robô roda o **agente** (`examples/diy-robot/ilrdos_robot_agent.py`). O ILrDOS manda **uma palavra por linha** e o agente
responde **uma linha**:

| ILrDOS envia | Quando | Resposta |
|---|---|---|
| `START` / `STOP` / `PAUSE` / `DOCK` | "rover, começa", ou `fleet rover start` | `OK` ou `ERR <motivo>` |
| `STATUS` | `fleet status rover` | `OK working` (ou `stopped`, `paused`, `docking`) |

O agente **não mexe no hardware**. Ele executa **scripts seus**, na pasta de hooks, em qualquer linguagem (shell, Python,
C, ROS...). Assim o mesmo agente serve para um carrinho com ponte H, um braço, um rastreador ou o que você inventar.

| Hook | Obrigatório | O que faz |
|---|---|---|
| `start` | sim | faz o robô começar a trabalhar |
| `stop` | sim | desliga tudo que se move; roda no STOP e nas paradas automáticas |
| `pause`, `dock` | não | se não existir, o GLaDOS recebe `ERR este robô não tem a ação ...` |
| `status` | não | imprime `working`, `stopped`, `paused` ou `docking` conforme o estado real |
| `safe` | não | sai com 0 se é seguro andar (sensor de queda, para-choque, bateria) |

Cada hook sai com código 0 se deu certo; qualquer outro código vira `ERR` para o GLaDOS, e a primeira linha que o script
imprime é o motivo. Exemplos prontos em `examples/diy-robot/hooks/` (com dicas de GPIO via `gpioset`, Python e ROS).

## Passo a passo

1. Na placa do robô: `git clone https://github.com/V0rtexLinux/ILrDOS.git && cd ILrDOS/examples/diy-robot && sudo ./install.sh`
   (cria o usuário `ilrdos-robot`, com acesso aos grupos de hardware que a placa tiver, e o serviço systemd).
2. Edite os scripts em `/etc/ilrdos-robot/` (`start` e `stop` primeiro) e ative: `sudo systemctl enable --now ilrdos-robot-agent`.
   Para testar sem instalar: `python3 ilrdos_robot_agent.py --hooks ./hooks --port 7777`.
3. Descubra o IP da placa (`hostname -I`) e declare o robô no `/boot/ilrdos-fleet.txt` do GLaDOS:

```
robot rover diy host=192.168.1.60 class=rover zone=sala alias=meu_rover,rover
```

4. Reinicie o ilrdosd e teste: `fleet list`, `fleet rover start`, `fleet status rover`, `fleet rover stop`.

Também dá para ligar por cabo serial/USB-serial (`robot rover diy serial=/dev/ttyUSB0 baud=115200`) quando o programa do
lado do robô fala o mesmo protocolo de linha na serial.

## Opções do agente

`--hooks` (pasta, padrão `/etc/ilrdos-robot`) · `--port` (7777) · `--bind` · `--allow` (IPs/redes que podem comandar; padrão só
redes locais, o recomendado é só o IP do GLaDOS: `--allow 192.168.1.10`) · `--max-run` (segundos até parar sozinho, padrão 120) ·
`--hook-timeout` (10 s).

## Opções da linha `robot ... diy`

`host=` ou `serial=` · `port=` (7777) · `baud=` (115200) · `start_payload=`, `stop_payload=`, `pause_payload=`, `dock_payload=`
(trocam as palavras do protocolo; deixe vazio para dizer que não existe a ação) · e as comuns da frota: `class=`, `zone=`,
`alias=`, `confirm=1`, `sensitive=1`, `interval=`, `max_run=`.

## Regras de segurança

- No ILrDOS (as da frota): `STOP` sempre funciona, de qualquer origem; `PAUSE` e `DOCK` também. `START` de `confirm=1` ou de
  classes pesadas (braço, esteira, cortador...) só com **"eu confirmo"** por voz ou terminal autenticado, nunca por regra
  automática; dois robôs na mesma `zone=` não trabalham juntos; só rede local e só robôs declarados.
- No agente: só aceita IPs de `--allow`; hooks só rodam se pertencerem a quem pode escrever neles (recusa hook com escrita
  para grupo/outros); sem shell; limite de tempo; `safe` é checado antes do START e a cada segundo; o serviço roda com
  usuário sem privilégios.
- **O ILrDOS não é circuito de segurança.** Mantenha parada de emergência em hardware, e hooks `stop` que funcionem sempre.

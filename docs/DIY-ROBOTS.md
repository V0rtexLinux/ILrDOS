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
| `STATUS` | `fleet status rover` | `OK working` (ou `stopped`, `paused`, `docking`, `fire`, `recovering`) |

O agente **não mexe no hardware**. Ele executa **scripts seus**, na pasta de hooks, em qualquer linguagem (shell, Python,
C, ROS...). Assim o mesmo agente serve para um carrinho com ponte H, um braço, um rastreador ou o que você inventar.

| Hook | Obrigatório | O que faz |
|---|---|---|
| `start` | sim | faz o robô começar a trabalhar |
| `stop` | sim | desliga tudo que se move; roda no STOP e nas paradas automáticas |
| `pause`, `dock` | não | se não existir, o GLaDOS recebe `ERR este robô não tem a ação ...` |
| `status` | não | imprime `working`, `stopped`, `paused` ou `docking` conforme o estado real |
| `safe` | não | sai com 0 se é seguro andar (sensor de queda, para-choque, bateria) |
| `gas` | não | detector de gás carbônico (fogo): 0 = ar normal, 1 = fogo, outro = falha do sensor. Ativa o protocolo de incêndio |
| `minibot/contencao`, `minibot/extintor`, `minibot/restauracao` | só com `gas` | os 3 minibots do protocolo de incêndio (ver abaixo) |

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

## Comandos que você mesmo define

Além de START/STOP/PAUSE/DOCK, **você decide os comandos do seu robô**: acender uma luz, fechar a garra, andar 50%, o que o
seu hardware fizer. São dois passos, os dois seus:

1. No robô, crie um script em `/etc/ilrdos-robot/cmd/` com o nome da palavra **em minúsculas** (ex.: `cmd/luz_on`). Ele roda sem
   shell, sai com 0 se deu certo, e a 1ª linha impressa volta ao GLaDOS. Exemplos em `examples/diy-robot/hooks/cmd/`.
2. No `ilrdos-fleet.txt`, declare o nome falado e a palavra do protocolo: `cmd=nome:PALAVRA` (repita para cada comando):

```
robot carrinho diy host=192.168.1.60 class=carrinho alias=carrinho cmd=acende_luz:LUZ_ON cmd=anda_meio:ANDAR:50 cmd=fecha_garra:GARRA cmd_confirm=fecha_garra
```

Uso: por voz, **"carrinho, acende a luz"** (o nome com `_` vira espaço) ou no terminal `fleet carrinho run acende_luz`.
`PALAVRA:argumento` chega ao script como `$1` (`cmd=anda_meio:ANDAR:50` roda `cmd/andar 50`; argumento só com letras, números e `_ . : -`).

Segurança dos comandos próprios (vale para qualquer comando que você inventar):

- Só **voz e terminal** mandam; regras automáticas e o modelo de linguagem nunca. Robô `sensitive=1`: só terminal.
- `cmd_confirm=nome1,nome2` (e todo comando de robô `confirm=1`/pesado) pede **"eu confirmo"** por voz.
- O agente só executa scripts que **existem** em `cmd/`, que só o dono pode escrever; não há shell nem comando livre.
  Havendo hook `safe`, ele é checado antes de cada comando próprio. Eles não mudam o estado (working/stopped).
- `STOP` continua vencendo tudo e não pode ser redefinido (`cmd=stop:...` é recusado).

## Protocolo de incêndio (3 minibots)

Com o hook `gas`, o agente consulta o detector de gás carbônico a cada segundo. Se ele indicar fogo (saída 1):

1. O robô é parado (hook `stop`) e o estado vira `fire`. Daí em diante só `STOP` e `STATUS` são aceitos (`ERR incêndio em andamento`).
2. **Minibot 1** (`minibot/contencao`) e **minibot 2** (`minibot/extintor`) rodam **na hora**, ao mesmo tempo. A contenção evita que alguém
   perto se machuque; o extintor solta a mesma substância do extintor de incêndio pela sala. O minibot 2 se repete enquanto o gás
   ainda indicar fogo (`--extinguish-tries`, padrão 5).
3. Com o ar normal por `--clear-seconds` seguidos (padrão 10), o fogo é dado por apagado, o estado vira `recovering` e roda o
   **minibot 3** (`minibot/restauracao`): troca as partes danificadas e restaura a programação.
4. Se o minibot 3 sair com 0, o robô volta ao normal, **parado** (nunca volta sozinho a trabalhar). Se falhar, fica bloqueado até alguém
   consertar e reiniciar o agente.

Uma falha do sensor (código diferente de 0 e 1) só é registrada: não dispara o protocolo. Os scripts de exemplo só imprimem uma
mensagem; troque-os pelo seu hardware. Teste: `tests/diy-fire-test.sh`.

## Opções do agente

`--hooks` (pasta, padrão `/etc/ilrdos-robot`) · `--port` (7777) · `--bind` · `--allow` (IPs/redes que podem comandar; padrão só
redes locais, o recomendado é só o IP do GLaDOS: `--allow 192.168.1.10`) · `--max-run` (segundos até parar sozinho, padrão 120) ·
`--hook-timeout` (10 s) · `--fire-timeout` (120 s por minibot) · `--clear-seconds` (10) · `--extinguish-tries` (5).

## Opções da linha `robot ... diy`

`host=` ou `serial=` · `port=` (7777) · `baud=` (115200) · `start_payload=`, `stop_payload=`, `pause_payload=`, `dock_payload=`
(trocam as palavras do protocolo; deixe vazio para dizer que não existe a ação) · e as comuns da frota: `class=`, `zone=`,
`alias=`, `confirm=1`, `sensitive=1`, `interval=`, `max_run=` · `cmd=nome:PALAVRA` e `cmd_confirm=` (ver acima).

## Regras de segurança

- No ILrDOS (as da frota): `STOP` sempre funciona, de qualquer origem; `PAUSE` e `DOCK` também. `START` de `confirm=1` ou de
  classes pesadas (braço, esteira, cortador...) só com **"eu confirmo"** por voz ou terminal autenticado, nunca por regra
  automática; dois robôs na mesma `zone=` não trabalham juntos; só rede local e só robôs declarados.
- No agente: só aceita IPs de `--allow`; hooks só rodam se pertencerem a quem pode escrever neles (recusa hook com escrita
  para grupo/outros); sem shell; limite de tempo; `safe` é checado antes do START e a cada segundo; o serviço roda com
  usuário sem privilégios.
- Incêndio: o protocolo dos 3 minibots é um auxílio, não substitui detector, extintor e corte de energia certificados no hardware.
- **O ILrDOS não é circuito de segurança.** Mantenha parada de emergência em hardware, e hooks `stop` que funcionem sempre.

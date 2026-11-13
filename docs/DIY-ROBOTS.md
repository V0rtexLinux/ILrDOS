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

## Chassi vertebral (barramento de dados como "sangue")

Ideia de hardware para quem monta o robô fisicamente: os cabos de dados de visão, reconhecimento
facial e áudio podem correr juntos por um único barramento de alta velocidade (ex.: ~1,3 Gbps,
perto de Gigabit Ethernet) dentro do "chassi vertebral" — a peça que liga a cabeça (sensores) ao
computador central ("cérebro"). O protocolo de incêndio já existente (`gas` + 3 minibots) é o lugar
certo para cortar e religar esse barramento:

- **Minibot 1 (contenção)** corta fisicamente o barramento (relé ou MOSFET em série nos 3 cabos de
  dados), travado em nível alto, assim que o `gas` indica fogo — antes de qualquer outra ação. Isso
  evita que dado corrompido por calor/curto chegue ao cérebro.
- **Minibot 2 (extintor)** age normalmente, sem tocar no barramento.
- **Minibot 3 (restauração)** é o único ponto que religa o barramento, e só depois que o ar está
  normal por `--clear-seconds` seguidos; se um sensor de corrente/temperatura no próprio barramento
  ainda indicar anomalia, o script deve sair com código diferente de 0 para manter o corte.

Isso mantém a trava em hardware (latch, não software) e usa o mesmo handshake de minibots que o
agente já implementa — nenhuma mudança no protocolo START/STOP/STATUS é necessária. Para redundância
(duas "veias" em paralelo, bloqueio por níveis em vez de corte total), isso é trabalho de hardware
fora do agente; os hooks acima são só o ponto de corte/religamento que o software já controla.

## Anatomia física do robô (cabos grossos, haste motorizada e cabeça de visão vermelha)

Esta parte descreve o desenho do robô de verdade. Tudo aqui é **hardware**: o agente só enxerga os pontos de controle
(`gas`, `minibot/*` e `cmd/*`). Nenhuma mudança no protocolo START/STOP/STATUS.

```
 SUPORTE/BASE + FONTE ──(cabos principais + e −)──┐         ┌── Entrada de controle de fábrica (Modbus)
        │                                          │         │   Detector de CO2 (fogo)  ──► hook `gas`
   CASCO PROTETOR ── COMPUTADOR CENTRAL ("cérebro": RAM, SSD, HD, speaker, LED)
                            │
                  HASTE MOTORIZADA (preta)           ← só estrutura + motores, NÃO leva dados
                            │
                    CHASSI VERTEBRAL ═══ cabos grossos de visão / rosto / áudio ═══► CHASSI FACIAL (cabeça)
                                                                                      └─ visor/câmera
```

### Cabos (todos grossos, de bancada industrial)

| Cabo | Função | Sugestão de construção |
|---|---|---|
| Principais de energia (+ e −) | base/fonte → cérebro e motores | cobre multifilar de grande seção (ex.: 6 a 16 mm²), terminais de compressão, fusível/disjuntor DC logo na saída da fonte, capa dupla vermelha (+) e preta (−) |
| Entrada de controle de fábrica (Modbus) | liga o robô a CLP/máquinas | RS-485 par trançado blindado, resistor de terminação de 120 Ω nas pontas, blindagem aterrada só em um lado |
| Detector de gás carbônico (fogo) | alimenta o hook `gas` | cabo blindado curto, sensor longe do calor direto, falha do sensor só é registrada |
| Dados de visão | câmera → cérebro | blindado, com ferrites; é o mais grosso do feixe |
| Dados de rostos (reconhecimento facial) | câmera/sensor facial → cérebro | blindado, par dedicado |
| Dados de áudio | microfone e speaker ↔ cérebro | blindado e afastado dos cabos de energia para evitar zumbido |

Regras práticas para cabos grossos: raio mínimo de curvatura (o fio grosso não dobra apertado), alívio de tração em cada ponta
(prensa-cabos), identificação (etiquetas e cores) e o feixe de dados **nunca** paralelo e colado ao de energia: se tiver de cruzar,
cruze a 90°. O feixe de visão/rosto/áudio dentro do chassi vertebral é o "sangue" do barramento descrito acima.

### Haste motorizada (entre o computador central e o chassi vertebral)

O "cabo" preto do desenho **não é cabo**: é uma haste estrutural com motores que movimenta o corpo.

- Só leva torque e estrutura. Os dados passam pelo chassi vertebral, não pela haste.
- Motores com driver próprio (ex.: servos ou passo) alimentados pelos cabos principais, com fim de curso em cada eixo.
- Os motores só mexem com o robô em estado normal: o hook `stop` corta o driver e, em incêndio, o minibot 1 também.
- Parada de emergência física em série com a alimentação dos drivers (o ILrDOS não substitui isso).

### Cabeça (chassi facial) e visão totalmente vermelha

O chassi facial é montado nesta ordem, de trás para a frente (o visor/câmera fica na frente):

1. **Fita LED RGB** no fundo da cabeça, ligada **só no canal vermelho** (verde e azul em 0), com luz forte.
2. **Câmera** logo à frente da fita, apontada para o visor.
3. **Lente de vidro** na frente da câmera para amplificar/concentrar o vermelho. O robô passa a ver o mundo em vermelho.

Para ele continuar enxergando **corretamente** mesmo só vendo vermelho:

- Use sensor **monocromático** (ou desligue a matriz de cores) e leia só o canal vermelho; assim a imagem tem luminância
  completa e não aparece "escura demais".
- Ajuste exposição e ganho fixos (sem balanço de branco automático) e calibre com alvos de cor conhecidos.
- Treine/calibre o reconhecimento facial com imagens em vermelho (ou converta para tons de cinza do canal R antes de reconhecer).
- Deixe a intensidade da fita ajustável por PWM e meça a imagem: se saturar, reduza; se ficar escura, aumente.
- Fita forte esquenta: dissipador de alumínio, limite de corrente e sensor de temperatura; o fogo desliga a fita (minibot 1).
- Não aponte a fita forte para os olhos de ninguém; proteja o visor com a lente e deixe a luz só dentro da cabeça.

Comandos próprios de exemplo (veja "Comandos próprios") em `examples/diy-robot/hooks/cmd/`:

- `visao_vermelha` liga/desliga/ajusta a fita vermelha (`cmd=visao_vermelha:VISAO_VERMELHA:80`, argumento 0 a 100).
- `haste_mover` move a haste motorizada (`cmd=haste_mover:HASTE:30`).

### Complexidade do projeto = grossura dos cabos

A camada de corpo (`examples/diy-robot/body/`) liga o código ao tamanho físico dos cabos. `body.json` lista cada cabo, cada
condutor e cada peça (com um peso). `ilrdos_body.py report` soma **peças + condutores + linhas de código do projeto / 100** e
transforma o total em um **nível** (1 a cada 100 pontos). Quanto maior o nível, mais grossos ficam os cabos:

- **Veias reservas** em paralelo: +50% de condutores por nível acima do 1 (redundância, as "veias" do chassi vertebral).
- **Margem de corrente**: +35% de seção por nível nos condutores de potência.
- **Armadura**: +1,5 mm de capa por nível.

Com o projeto atual (nível 8, ~770 pontos, 13 cabos e mais de 1000 condutores) os feixes de dados saem com 40 a 45 mm de
diâmetro, energia principal com 74 mm, motores da haste com 86 mm e a bateria de reserva com mais de 180 mm. Cada peça ou linha
nova no projeto pode subir o nível e engrossar o cabo no relatório.

Subsistemas simulados no código (`ilrdos_body.py`), cada um com cabos e peças próprios: veias redundantes A/B do chassi
vertebral, watchdog em hardware, energia com bateria de reserva (BMS), arrefecimento líquido ligado a 24 sensores de temperatura,
12 sensores de corrente, IMU, 16 sensores de proximidade, switch Ethernet e caixa-preta de eventos. Em incêndio, a contenção
corta as duas veias, a haste, a fita LED e o arrefecimento (para não soprar o fogo). O mesmo módulo simula o chassi vertebral (corte e religação em trava), a haste (passos e fim de
curso) e a cabeça vermelha (só canal R, limite térmico): `ilrdos_body.py sim-fogo`. Teste: `tests/diy-body-test.sh`.

## Versão industrial gigante (porte de fábrica)

Perfil para fábricas e indústrias grandes, com o robô em escala gigante (80 m de altura e 110 m de envergadura por padrão, estimativa um pouco maior que a GLaDOS (não existe medida oficial), suspenso em
pórtico de trilhos no teto; mude só `escala.altura_m` e todos os comprimentos de cabo escalam junto). Fica em `examples/diy-robot/body/body-industrial.json` e usa o mesmo `ilrdos_body.py`:

`python3 examples/diy-robot/body/ilrdos_body.py report examples/diy-robot/body/body-industrial.json`

O que muda em relação à versão DIY:

- **Comprimento real dos cabos** (67 a 890 m com a altura padrão de 80 m), com peso total em kg e **queda de tensão** (%) nos de potência. Energia trifásica de
  690 V (L1/L2/L3/PE de 400 A), 6 servomotores de 250 A e banco de UPS de 400 V. Acima do maior condutor da tabela (630 mm²) a
  corrente é dividida em condutores em paralelo.
- **Fibra óptica** nos cabos de dados com mais de 90 m (o cobre Ethernet passa de 100 m e perde sinal), o que o relatório indica na coluna `meio`.
- **Segurança de fábrica**: CLP de segurança SIL3, laços duplos de parada de emergência, cortinas de luz e scanners laser de zona,
  gaiola com porta intertravada e bloqueio/etiquetagem (LOTO), extinção fixa por gás inerte, hidráulica/pneumática com 48 válvulas
  e gateway OPC UA/Modbus para MES/ERP.
- **Nível de complexidade 21** (~2000 pontos, mais de 8700 condutores). Quando um cabo passa de 200 mm de diâmetro o relatório avisa e `ilrdos_body.py plano <json>` gera o plano:
  **divisão** em cabos iguais em paralelo de no máximo 150 mm (os condutores são repartidos, os maiores primeiro; em trifásico,
  cada cabo deve levar as três fases + PE) e **barramento blindado (busway)** para os condutores de potência (>=100 A), com
  corrente nominal padrão (800 a 6300 A), número de polos, carcaças de 4 polos e barramentos em paralelo acima de 6300 A.
  Potência vai em busway no tronco (cabos só nos ramais); sinal (sensores, CLP de segurança) não vai em busway e fica dividido em
  bandejas. Com o perfil industrial: energia principal 1× 3200 A, motores da haste 3× 2000 A, bateria 1× 2500 A e hidráulica
  1× 1000 A; sem busway seriam 49, 189, 146 e 128 cabos de ~150 mm.

Os números são dimensionamento de projeto, não projeto elétrico aprovado: em fábrica real, cabos, proteções e a gaiola precisam de
engenheiro eletricista, norma (IEC 60204-1, ISO 13849 / IEC 62061) e aceite do responsável pela segurança. O ILrDOS continua não
sendo circuito de segurança.

### Versão industrial média (recomendada)

`examples/diy-robot/body/body-industrial-medio.json`: 30 m de altura e 40 m de envergadura, 4 servomotores de 100 A, energia
trifásica de 400 V (160 A por fase), 48 sensores de temperatura e hidráulica de 60 A. Mantém a segurança (CLP SIL3, laços duplos,
gaiola, extinção por gás), a redundância A/B e a visão vermelha; corta tamanho, corrente e número de sensores. Fica no nível 15
(contra 21 da gigante), com 3600 condutores (contra 8700). Comandos: `report`, `plano` e `autovalida` com esse arquivo.

Estimativa (sem fonte, ordem de grandeza): **3 a 5 anos e US$ 120 a 300 milhões**, contra 6 a 10 anos e US$ 500 milhões a
1,2 bilhão da gigante de 80 m. Potência no tronco: energia principal 1× 1000 A, motores 2× 800 A e bateria 1× 800 A em busway.

### Autovalidação

`ilrdos_body.py autovalida [json]` confere o manifesto contra regras fixas e sai com código 1 se algo falhar: peças que apontam
para cabo inexistente ou repetidas, cabo sem peça, queda de tensão acima de 3%, cobre de dados acima de 100 m (deve ser fibra),
divisão que não chega a Ø <= 150 mm, busway subdimensionado, cabo que afina com mais complexidade, e o comportamento do corpo
(contenção corta tudo, restauração não religa com anomalia, LED só no canal vermelho). A pontuação inclui as linhas de código do
repositório, então o nível e os diâmetros mudam um pouco conforme o projeto cresce.

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

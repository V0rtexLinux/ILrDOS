#!/usr/bin/env python3
"""ILrDOS - camada de corpo do robô DIY.

Descreve cada condutor, calcula bitola/diâmetro dos cabos (quanto mais canais, mais grosso o feixe)
e simula os subsistemas: haste motorizada, cabeça de visão vermelha e chassi vertebral (barramento).
Uso:  ilrdos_body.py report [body.json]   |   ilrdos_body.py sim-fogo [body.json]
"""
import json, math, os, sys

# ampacidade de chassi (A) por seção (mm²), conservadora; vale para cabo multifilar de cobre
AMPACIDADE = [(0.5, 3), (0.75, 6), (1.5, 15), (2.5, 21), (4, 28), (6, 36), (10, 50),
              (16, 68), (25, 90), (35, 110), (50, 140), (70, 170), (95, 210), (120, 245), (150, 285), (185, 330), (240, 390), (300, 450)]
FILL = 0.55          # fração da seção do cabo ocupada por cobre+isolamento
JAQUETA_MM = 2.0     # espessura da capa externa (somada nos dois lados)
BLINDAGEM_MM = 1.0   # malha + fita
SINAL_MM = 1.1       # diâmetro de um condutor de sinal (par trançado AWG24 isolado)


NIVEL = {"n": 1, "reserva": 1.0, "margem": 1.0, "armadura_mm": 0.0}


def complexidade(man, raiz=None):
    """Pontos = peso das peças + condutores + 1 ponto a cada 100 linhas de código do projeto."""
    raiz = raiz or os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
    loc = 0
    for pasta in ("src", "include", "examples", "os", "client", "voice", "tests"):
        for dp, _, fs in os.walk(os.path.join(raiz, pasta)):
            for f in fs:
                if f.endswith((".c", ".cpp", ".h", ".hpp", ".py", ".sh", ".json", ".service")) or "." not in f:
                    try:
                        with open(os.path.join(dp, f), encoding="utf-8", errors="ignore") as fh:
                            loc += sum(1 for _ in fh)
                    except OSError:
                        pass
    pecas = sum(p["peso"] for p in man.get("pecas", []))
    conds = sum(len(expandir(c)) for c in man["cabos"].values())
    return {"pecas": pecas, "condutores": conds, "linhas": loc,
            "pontos": pecas + conds + loc // 100}


def aplicar_nivel(pontos):
    """Cada 100 pontos sobe 1 nível: mais condutores reserva (veias redundantes), mais margem de
    corrente (seção maior) e mais armadura na capa. É isso que engrossa os cabos do desenho."""
    n = 1 + pontos // 100
    NIVEL.update(n=n, reserva=1 + 0.5 * (n - 1), margem=1 + 0.35 * (n - 1), armadura_mm=1.5 * (n - 1))
    return n


def secao_para(corrente):
    corrente = corrente * NIVEL["margem"]
    for mm2, amp in AMPACIDADE:
        if corrente <= amp:
            return mm2
    raise ValueError(f"corrente {corrente} A passa do maior condutor da tabela")


def diam_condutor(mm2):
    # círculo equivalente + 1,2 mm de isolamento (grosso, tipo cabo de solda/bateria)
    return 2 * math.sqrt(mm2 / math.pi) + 1.2


def expandir(cabo):
    """Lista de (nome, diâmetro_mm, seção_mm2|None) de todos os condutores do cabo."""
    saida = []
    if cabo["tipo"] == "lanes":
        for i in range(cabo["lanes"]):          # cada lane = par diferencial
            saida += [(f"lane{i}+", SINAL_MM, None), (f"lane{i}-", SINAL_MM, None)]
        for i in range(cabo.get("extra", 0)):
            saida.append((f"aux{i}", SINAL_MM, None))
        return saida
    for c in cabo["condutores"]:
        n = c.get("n", 1)
        for k in range(n):
            nome = c["nome"] if n == 1 else f'{c["nome"]}#{k}'
            if "corrente_a" in c:
                mm2 = secao_para(c["corrente_a"])
                saida.append((nome, diam_condutor(mm2), mm2))
            else:
                saida.append((nome, SINAL_MM, None))
    return saida


def dimensionar(cabo):
    cond = expandir(cabo)
    if NIVEL["reserva"] > 1:          # veias reservas em paralelo (redundância)
        medios = [d for _, d, _ in cond if True]
        extra = int(len(cond) * (NIVEL["reserva"] - 1))
        cond = cond + [(f"reserva{i}", max(medios) if cabo["tipo"] != "lanes" else SINAL_MM, None)
                       for i in range(extra)]
    area = sum(math.pi * (d / 2) ** 2 for _, d, _ in cond) / FILL
    od = 2 * math.sqrt(area / math.pi)
    od += 2 * (JAQUETA_MM + NIVEL["armadura_mm"]) + (2 * BLINDAGEM_MM if cabo.get("blindagem") else 0)
    maior = max((m for _, _, m in cond if m), default=None)
    gbps = cabo.get("lanes", 0) * cabo.get("gbps_por_lane", 0)
    return {"condutores": len(cond), "od_mm": round(od, 1), "maior_secao_mm2": maior,
            "gbps": round(gbps, 2), "raio_curva_mm": round(od * 6, 0)}


def validar(man):
    erros = []
    for nome, cabo in man["cabos"].items():
        if cabo["tipo"] == "lanes" and cabo.get("lanes", 0) < 1:
            erros.append(f"{nome}: lanes inválido")
        if cabo["tipo"] != "lanes" and not cabo.get("condutores"):
            erros.append(f"{nome}: sem condutores")
    return erros


class ChassiVertebral:
    """Barramento visão/rosto/áudio. Corte é trava (latch): só restaura() religa."""
    def __init__(self, cfg):
        self.cfg, self.cortado, self.corrente, self.temp = cfg, False, 0.0, 25.0

    def cortar(self):
        self.cortado = True

    def anomalia(self):
        return self.corrente > self.cfg["limite_corrente_a"] or self.temp > self.cfg["limite_temp_c"]

    def restaurar(self):
        if self.anomalia():
            return 1       # mantém o corte
        self.cortado = False
        return 0


class Haste:
    """Haste motorizada (só estrutura + motores). Passos limitados e fim de curso por motor."""
    def __init__(self, cfg):
        self.m = {x["id"]: dict(x, pos=0) for x in cfg["motores"]}
        self.habilitada = True

    def mover(self, mid, alvo):
        if not self.habilitada:
            raise RuntimeError("driver da haste desabilitado")
        m = self.m[mid]
        if not (m["min"] <= alvo <= m["max"]):
            raise ValueError("fim de curso")
        passo = max(-m["passo_max"], min(m["passo_max"], alvo - m["pos"]))
        m["pos"] += passo
        return m["pos"]

    def cortar(self):
        self.habilitada = False


class CabecaVermelha:
    """Fita LED só no canal R + câmera mono lendo o canal R."""
    def __init__(self, cfg):
        self.cfg, self.nivel, self.temp = cfg, 0, 25.0

    def ajustar(self, nivel):
        lim = self.cfg["led"]
        self.nivel = max(lim["min"], min(lim["max"], int(nivel)))
        return self.nivel

    def tick_termico(self):
        # aquecimento simples: reduz sozinho se passar da temperatura máxima
        self.temp += self.nivel * 0.05 - 1.0
        if self.temp > self.cfg["led"]["temp_max_c"]:
            self.nivel = max(0, self.nivel - 20)

    def apagar(self):
        self.nivel = 0

    def rgb(self):
        return (self.nivel, 0, 0)   # verde e azul sempre 0


class Watchdog:
    """Hardware watchdog: se o cérebro não alimentar dentro do prazo, o corpo vai para contenção."""
    def __init__(self, prazo=3):
        self.prazo, self.ultimo = prazo, 0

    def alimentar(self, t):
        self.ultimo = t

    def vencido(self, t):
        return t - self.ultimo > self.prazo


class Energia:
    """Fonte principal + bateria de reserva (BMS). Troca sem queda; bateria baixa bloqueia motores."""
    def __init__(self, soc=100.0):
        self.fonte_ok, self.soc = True, soc

    def tick(self, carga_pct=10):
        if not self.fonte_ok:
            self.soc = max(0.0, self.soc - carga_pct * 0.1)

    def fonte(self):
        return "rede" if self.fonte_ok else "bateria"

    def permite_motores(self):
        return self.fonte_ok or self.soc > 20


class Arrefecimento:
    """Bomba + ventoinhas ligadas à temperatura mais alta dos 24 sensores. Em fogo, desliga (não sopra o fogo)."""
    def __init__(self, n=24):
        self.temps, self.fans, self.bomba = [25.0] * n, 0, False

    def tick(self):
        t = max(self.temps)
        self.bomba = t > 40
        self.fans = 0 if t < 35 else min(100, int((t - 35) * 5))

    def desligar(self):
        self.bomba, self.fans = False, 0


class VeiaRedundante:
    """Veias A e B do chassi vertebral: usa a A; se ela falhar assume a B; em fogo as duas cortam."""
    def __init__(self, cfg):
        self.a, self.b, self.ativa = ChassiVertebral(cfg), ChassiVertebral(cfg), "A"

    def falha_a(self):
        self.a.cortar()
        if not self.b.cortado:
            self.ativa = "B"

    def cortar_tudo(self):
        self.a.cortar(); self.b.cortar(); self.ativa = None

    def restaurar(self):
        ra, rb = self.a.restaurar(), self.b.restaurar()
        self.ativa = "A" if ra == 0 else ("B" if rb == 0 else None)
        return 0 if self.ativa else 1


class Telemetria:
    """Caixa-preta: guarda os últimos eventos (anel) para análise depois de incêndio ou falha."""
    def __init__(self, cap=256):
        self.cap, self.eventos = cap, []

    def log(self, t, msg):
        self.eventos.append((t, msg))
        del self.eventos[:-self.cap]


class Corpo:
    def __init__(self, man):
        self.barramento = ChassiVertebral(man["barramento"])
        self.haste = Haste(man["haste"])
        self.cabeca = CabecaVermelha(man["cabeca"])
        self.veias = VeiaRedundante(man["barramento"])
        self.watchdog, self.energia = Watchdog(), Energia()
        self.refrig, self.caixa_preta = Arrefecimento(), Telemetria()

    def contencao(self):           # minibot 1: ordem importa, barramento primeiro
        self.barramento.cortar()
        self.veias.cortar_tudo()
        self.haste.cortar()
        self.cabeca.apagar()
        self.refrig.desligar()
        self.caixa_preta.log(0, "contenção: barramento A/B, haste, LED e arrefecimento cortados")

    def restauracao(self):         # minibot 3: só religa se o barramento estiver normal
        rc = max(self.barramento.restaurar(), self.veias.restaurar())
        if rc == 0:
            self.haste.habilitada = True
        return rc


def carregar(path=None):
    path = path or os.path.join(os.path.dirname(os.path.abspath(__file__)), "body.json")
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def cmd_report(man):
    c = complexidade(man)
    n = aplicar_nivel(c["pontos"])
    print(f"complexidade: {c['pontos']} pts (peças {c['pecas']} + condutores {c['condutores']} + "
          f"{c['linhas']} linhas/100) -> nível {n}: reserva x{NIVEL['reserva']:.1f}, "
          f"margem de corrente x{NIVEL['margem']:.2f}, armadura +{NIVEL['armadura_mm']:.1f} mm")
    erros = validar(man)
    if erros:
        print("\n".join("ERRO " + e for e in erros)); return 1
    print(f"{'cabo':20} {'cond.':>6} {'Ø mm':>6} {'maior mm²':>10} {'Gbps':>6} {'raio curva':>11}")
    tot = 0
    for nome, cabo in man["cabos"].items():
        d = dimensionar(cabo); tot += d["condutores"]
        print(f"{nome:20} {d['condutores']:>6} {d['od_mm']:>6} {str(d['maior_secao_mm2'] or '-'):>10} "
              f"{d['gbps'] or '-':>6} {d['raio_curva_mm']:>9.0f}mm")
    print(f"total de condutores: {tot}")
    return 0


def cmd_sim_fogo(man):
    c = Corpo(man)
    c.cabeca.ajustar(90); c.haste.mover("m1", 20)
    c.contencao()
    ok = c.barramento.cortado and not c.haste.habilitada and c.cabeca.nivel == 0
    print("contenção:", "ok" if ok else "FALHOU")
    c.barramento.temp = 120                      # anomalia: continua cortado
    print("restauração com anomalia sai com", c.restauracao(), "(esperado 1)")
    c.barramento.temp = 30
    print("restauração normal sai com", c.restauracao(), "(esperado 0)")
    c.energia.fonte_ok = False; c.energia.soc = 15
    print("bateria baixa libera motores?", c.energia.permite_motores(), "(esperado False)")
    c.watchdog.alimentar(0)
    print("watchdog vencido após 5 s?", c.watchdog.vencido(5), "(esperado True)")
    return 0 if ok else 1


if __name__ == "__main__":
    a = sys.argv[1:]
    cmd = a[0] if a else "report"
    man = carregar(a[1] if len(a) > 1 else None)
    sys.exit({"report": cmd_report, "sim-fogo": cmd_sim_fogo}.get(cmd, cmd_report)(man))

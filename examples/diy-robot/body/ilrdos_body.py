#!/usr/bin/env python3
"""ILrDOS - camada de corpo do robô DIY.

Descreve cada condutor, calcula bitola/diâmetro dos cabos (quanto mais canais, mais grosso o feixe)
e simula os subsistemas: haste motorizada, cabeça de visão vermelha e chassi vertebral (barramento).
Uso:  ilrdos_body.py report [body.json]   |   ilrdos_body.py sim-fogo [body.json]
"""
import json, math, os, sys

# ampacidade de chassi (A) por seção (mm²), conservadora; vale para cabo multifilar de cobre
AMPACIDADE = [(0.5, 3), (0.75, 6), (1.5, 15), (2.5, 21), (4, 28), (6, 36), (10, 50),
              (16, 68), (25, 90), (35, 110), (50, 140), (70, 170), (95, 210), (120, 245), (150, 285), (185, 330), (240, 390), (300, 450), (400, 540), (500, 630), (630, 720)]
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
                # acima do maior condutor da tabela, divide a corrente em condutores em paralelo
                par = max(1, math.ceil(c["corrente_a"] * NIVEL["margem"] / AMPACIDADE[-1][1]))
                mm2 = secao_para(c["corrente_a"] / par)
                for j in range(par):
                    saida.append((nome if par == 1 else f"{nome}||{j}", diam_condutor(mm2), mm2))
            else:
                saida.append((nome, SINAL_MM, None))
    return saida


RHO_CU = 0.0175          # ohm·mm²/m (cobre)
FIBRA_ACIMA_M = 90       # acima disso, cobre de dados vira fibra óptica (limite de 100 m do Ethernet)


def queda_tensao(cabo):
    """% de queda no maior condutor de potência (ida e volta) para o comprimento e a tensão do cabo."""
    L, V = cabo.get("comprimento_m"), cabo.get("tensao_v")
    if not (L and V):
        return None
    cs = [c for c in cabo.get("condutores", []) if "corrente_a" in c]
    if not cs:
        return None
    c = max(cs, key=lambda x: x["corrente_a"])
    par = max(1, math.ceil(c["corrente_a"] * NIVEL["margem"] / AMPACIDADE[-1][1]))
    mm2 = secao_para(c["corrente_a"] / par) * par
    return round(100 * c["corrente_a"] * RHO_CU * 2 * L / mm2 / V, 2)


def meio(cabo):
    if cabo["tipo"] == "lanes" and cabo.get("comprimento_m", 0) > FIBRA_ACIMA_M:
        return "fibra"
    return "cobre"


def _condutores(cabo):
    cond = expandir(cabo)
    if NIVEL["reserva"] > 1:          # veias reservas em paralelo (redundância)
        medios = [d for _, d, _ in cond]
        extra = int(len(cond) * (NIVEL["reserva"] - 1))
        cond = cond + [(f"reserva{i}", max(medios) if cabo["tipo"] != "lanes" else SINAL_MM, None)
                       for i in range(extra)]
    return cond


def _od(cond, cabo):
    area = sum(math.pi * (d / 2) ** 2 for _, d, _ in cond) / FILL
    return 2 * math.sqrt(area / math.pi) + 2 * (JAQUETA_MM + NIVEL["armadura_mm"]) + \
        (2 * BLINDAGEM_MM if cabo.get("blindagem") else 0)


def dimensionar(cabo):
    cond = _condutores(cabo)
    od = _od(cond, cabo)
    maior = max((m for _, _, m in cond if m), default=None)
    gbps = cabo.get("lanes", 0) * cabo.get("gbps_por_lane", 0)
    L = cabo.get("comprimento_m")
    kg_m = round(sum(math.pi * (d / 2) ** 2 * 0.0089 * FILL for _, d, _ in cond) * 1.6, 1)  # cobre+capas
    return {"comprimento_m": L, "kg_total": round(kg_m * L) if L else None, "queda_pct": queda_tensao(cabo),
            "meio": meio(cabo), "condutores": len(cond), "od_mm": round(od, 1), "maior_secao_mm2": maior,
            "gbps": round(gbps, 2), "raio_curva_mm": round(od * 6, 0)}


OD_MAX_MM = 150                                    # maior cabo que se puxa em bandeja/eletroduto
BUSWAY_A = [800, 1000, 1250, 1600, 2000, 2500, 3200, 4000, 5000, 6300]   # correntes nominais de busway (A)


def dividir(cabo, od_max=OD_MAX_MM):
    """Menor número N de cabos iguais em paralelo (condutores repartidos, grandes primeiro) com Ø <= od_max."""
    cond = sorted(_condutores(cabo), key=lambda c: -c[1])
    for n in range(1, len(cond) + 1):
        grupos = [cond[i::n] for i in range(n)]
        od = max(_od(g, cabo) for g in grupos)
        if od <= od_max:
            return n, round(od, 1), max(len(g) for g in grupos)
    return len(cond), round(_od(cond[:1], cabo), 1), 1


def busway(cabo):
    """Barramento blindado para o tronco de potência: polos = condutores nominais de potência; corrente por polo
    = maior corrente de projeto (com margem do nível). Acima de 6300 A, barramentos em paralelo."""
    cs = [c for c in cabo.get("condutores", []) if c.get("corrente_a", 0) >= 100]
    if not cs:
        return None
    polos = sum(c.get("n", 1) for c in cs)
    a = max(c["corrente_a"] for c in cs) * NIVEL["margem"]
    pot = next((r for r in BUSWAY_A if r >= a), BUSWAY_A[-1])
    paralelos = max(1, math.ceil(a / BUSWAY_A[-1]))
    carcacas = math.ceil(polos / 4)                # carcaça de 4 polos (3F+PE ou 2 circuitos CC)
    return {"corrente_projeto_a": round(a), "nominal_a": pot, "paralelos": paralelos,
            "carcacas": carcacas, "polos": polos, "total": carcacas * paralelos}


def cmd_plano(man):
    c = complexidade(man)
    n = aplicar_nivel(c["pontos"])
    print(f"nível {n} | alvo: Ø máx {OD_MAX_MM} mm por cabo | busway só para condutores de potência (>=100 A)")
    print(f"{'cabo':24} {'Ø atual':>8} {'só cabos (N × Ø)':>22} {'busway':>34}  recomendação")
    for nome, cabo in man["cabos"].items():
        d = dimensionar(cabo)
        if d["od_mm"] <= 200:
            print(f"{nome:24} {d['od_mm']:>8}  {'-':>21} {'-':>34}  cabo único")
            continue
        nd, od, cpc = dividir(cabo)
        b = busway(cabo)
        if b:
            bw = f"{b['total']}× {b['nominal_a']} A ({b['polos']} polos)"
            rec = "busway no tronco (cabos só nos ramais)"
        else:
            bw, rec = "-", "dividir em bandeja (sinal ou potência <100 A: sem busway)"
        print(f"{nome:24} {d['od_mm']:>8}  {f'{nd} × {od} mm ({cpc} cond.)':>21} {bw:>34}  {rec}")
    print("Estimativa de dimensionamento; busway e bandejas exigem projeto elétrico e aceite do fabricante.")
    return 0


def cmd_autovalida(man):
    """Autovalidação: confere o manifesto e o dimensionamento contra regras fixas. Sai com 1 se algo falhar."""
    c = complexidade(man)
    aplicar_nivel(c["pontos"])
    falhas, ok = [], 0

    def checa(cond, msg):
        nonlocal ok
        if cond:
            ok += 1
        else:
            falhas.append(msg)

    for e in validar(man):
        falhas.append(e)
    cabos = man["cabos"]
    for p in man.get("pecas", []):
        checa(p["cabo"] in cabos, f"peça '{p['nome']}' aponta para cabo inexistente '{p['cabo']}'")
        checa(p["peso"] > 0, f"peça '{p['nome']}' com peso inválido")
    checa(len({p["nome"] for p in man.get("pecas", [])}) == len(man.get("pecas", [])), "peças com nome repetido")
    usados = {p["cabo"] for p in man.get("pecas", [])}
    for nome, cabo in cabos.items():
        d = dimensionar(cabo)
        checa(nome in usados, f"cabo '{nome}' sem nenhuma peça ligada")
        checa(d["condutores"] > 0 and d["od_mm"] > 0, f"{nome}: dimensionamento inválido")
        if d["queda_pct"] is not None:
            checa(d["queda_pct"] <= 3.0, f"{nome}: queda de tensão {d['queda_pct']}% > 3%")
        if cabo["tipo"] == "lanes" and cabo.get("comprimento_m"):
            esperado = "fibra" if cabo["comprimento_m"] > FIBRA_ACIMA_M else "cobre"
            checa(d["meio"] == esperado, f"{nome}: meio {d['meio']} errado para {cabo['comprimento_m']} m")
            checa(cabo["comprimento_m"] <= 100 or d["meio"] == "fibra", f"{nome}: cobre acima de 100 m")
        if d["od_mm"] > 200:
            n, od, _ = dividir(cabo)
            checa(od <= OD_MAX_MM, f"{nome}: divisão não chega a Ø <= {OD_MAX_MM} mm")
            if any(x.get("corrente_a", 0) >= 100 for x in cabo.get("condutores", [])):
                b = busway(cabo)
                checa(b is not None and b["nominal_a"] * b["paralelos"] >= b["corrente_projeto_a"],
                      f"{nome}: busway subdimensionado")
    # cabeça: tamanho opcional, tem de caber no corpo (escala.altura_m)
    alt_c, alt_t = man.get("cabeca", {}).get("altura_m"), man.get("escala", {}).get("altura_m")
    if alt_c is not None:
        checa(alt_c > 0, "cabeca.altura_m inválida")
        if alt_t:
            checa(alt_c <= alt_t * 0.5, f"cabeça de {alt_c} m passa de metade do corpo ({alt_t} m)")
    # monotonia: mais complexidade nunca afina o cabo
    ref = next(iter(cabos.values()))
    aplicar_nivel(0); a = dimensionar(ref)["od_mm"]
    aplicar_nivel(c["pontos"]); b2 = dimensionar(ref)["od_mm"]
    checa(b2 >= a, "mais complexidade afinou o cabo")
    # corpo: contenção e restauração
    corpo = Corpo(man); corpo.cabeca.ajustar(90); corpo.contencao()
    checa(corpo.barramento.cortado and corpo.veias.ativa is None and not corpo.haste.habilitada
          and corpo.cabeca.rgb() == (0, 0, 0) and corpo.refrig.fans == 0, "contenção não cortou tudo")
    corpo.barramento.temp = 999
    checa(corpo.restauracao() == 1 and corpo.barramento.cortado, "restauração religou com anomalia")
    corpo.barramento.temp = 25
    checa(corpo.restauracao() == 0, "restauração normal falhou")
    corpo.cabeca.ajustar(100)
    checa(corpo.cabeca.rgb()[1:] == (0, 0), "LED fora do canal vermelho")
    for f in falhas:
        print("FALHA", f)
    print(f"autovalidação: {ok} checagens ok, {len(falhas)} falhas")
    return 1 if falhas else 0


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
        man = json.load(f)
    esc = man.get("escala")
    if esc and esc.get("altura_ref_m") and esc.get("altura_m"):
        k = esc["altura_m"] / esc["altura_ref_m"]      # comprimentos dos cabos seguem a altura do robô
        for cabo in man["cabos"].values():
            if "comprimento_m" in cabo:
                cabo["comprimento_m"] = round(cabo["comprimento_m"] * k)
    return man


def cmd_report(man):
    c = complexidade(man)
    n = aplicar_nivel(c["pontos"])
    print(f"complexidade: {c['pontos']} pts (peças {c['pecas']} + condutores {c['condutores']} + "
          f"{c['linhas']} linhas/100) -> nível {n}: reserva x{NIVEL['reserva']:.1f}, "
          f"margem de corrente x{NIVEL['margem']:.2f}, armadura +{NIVEL['armadura_mm']:.1f} mm")
    erros = validar(man)
    if erros:
        print("\n".join("ERRO " + e for e in erros)); return 1
    print(f"{'cabo':20} {'cond.':>6} {'Ø mm':>6} {'maior mm²':>10} {'Gbps':>6} {'raio curva':>11} {'m':>5} {'kg':>7} {'queda%':>7} meio")
    tot = 0
    for nome, cabo in man["cabos"].items():
        d = dimensionar(cabo); tot += d["condutores"]
        print(f"{nome:20} {d['condutores']:>6} {d['od_mm']:>6} {str(d['maior_secao_mm2'] or '-'):>10} "
              f"{d['gbps'] or '-':>6} {d['raio_curva_mm']:>9.0f}mm {d['comprimento_m'] or '-':>5} "
              f"{d['kg_total'] or '-':>7} {d['queda_pct'] if d['queda_pct'] is not None else '-':>7} {d['meio']}")
        if d["od_mm"] > 200:
            print("  ↳ Ø > 200 mm: veja `plano` (divisão em cabos de ≤150 mm e barramento blindado)")
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
    sys.exit({"report": cmd_report, "sim-fogo": cmd_sim_fogo, "plano": cmd_plano, "autovalida": cmd_autovalida}.get(cmd, cmd_report)(man))

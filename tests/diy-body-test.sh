#!/bin/bash
# Testa a camada de corpo: nível de complexidade engrossa os cabos; contenção e restauração se comportam.
set -eu
B="$(dirname "$0")/../examples/diy-robot/body/ilrdos_body.py"
python3 -I "$B" report | grep -q "nível" && echo "[ok]   relatório com nível de complexidade"
python3 -I "$B" sim-fogo | grep -q "contenção: ok" && echo "[ok]   contenção corta barramento, haste e LED"
python3 -I "$B" sim-fogo | grep -q "anomalia sai com 1" && echo "[ok]   restauração com anomalia mantém o corte"
python3 -I - "$B" <<'PY'
import importlib.util, sys
sp = importlib.util.spec_from_file_location("b", sys.argv[1]); b = importlib.util.module_from_spec(sp); sp.loader.exec_module(b)
man = b.carregar(); cabo = man["cabos"]["dados_visao"]
b.aplicar_nivel(0); fino = b.dimensionar(cabo)["od_mm"]
b.aplicar_nivel(500); grosso = b.dimensionar(cabo)["od_mm"]
assert grosso > fino * 1.5, (fino, grosso)
print(f"[ok]   mais complexidade = cabo mais grosso ({fino} mm -> {grosso} mm)")
PY
IND="$(dirname "$0")/../examples/diy-robot/body/body-industrial.json"
python3 -I "$B" report "$IND" | grep -q "fibra" && echo "[ok]   industrial: fibra óptica nos trechos longos"
python3 -I "$B" report "$IND" | grep -q "busway" && echo "[ok]   industrial: aviso de cabo grosso demais"
python3 -I "$B" sim-fogo "$IND" | grep -q "contenção: ok" && echo "[ok]   industrial: contenção"
python3 -I "$B" plano "$IND" | grep -q "busway no tronco" && echo "[ok]   industrial: plano com barramento blindado na potência"
python3 -I "$B" plano "$IND" | grep -q "dividir em bandeja" && echo "[ok]   industrial: plano com divisão dos cabos de sinal"
python3 -I - "$B" "$IND" <<'PY'
import importlib.util, sys
sp = importlib.util.spec_from_file_location("b", sys.argv[1]); b = importlib.util.module_from_spec(sp); sp.loader.exec_module(b)
man = b.carregar(sys.argv[2]); b.aplicar_nivel(b.complexidade(man)["pontos"])
for nome, cabo in man["cabos"].items():
    if b.dimensionar(cabo)["od_mm"] > 200:
        n, od, _ = b.dividir(cabo)
        assert od <= b.OD_MAX_MM, (nome, od)
print("[ok]   industrial: todo cabo dividido fica com Ø <= 150 mm")
PY

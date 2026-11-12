#!/usr/bin/env python3
"""ILrDOS robot agent: faz o SEU robô (um computador de placa única com Linux) obedecer o GLaDOS.

Roda em qualquer placa com Linux e Python 3.8+, sem instalar nada: Raspberry Pi, Orange Pi, Banana Pi, Rock/Radxa,
Odroid, Jetson, BeagleBone, VisionFive, mini PC x86... (ARM, x86 ou RISC-V).

Protocolo (uma palavra por linha, uma linha de resposta):
    START | STOP | PAUSE | DOCK | STATUS   ->   OK | OK <estado> | ERR <motivo>
    <PALAVRA>[:<argumento>]                 ->   comando SEU: roda o hook cmd/<palavra> (ver docs/DIY-ROBOTS.md)
O agente não mexe no hardware: ele executa scripts SEUS na pasta de hooks (start, stop, pause, dock, status, safe).
Cada hook é um executável qualquer (shell, Python, C...) que liga/desliga motores, GPIO, ROS, etc.

PROTOCOLO DE INCÊNDIO (opcional): se existir o hook `gas` (detector de gás carbônico), o agente o consulta a cada segundo.
    gas sai com 0 = ar normal, 1 = fogo detectado, outro código = falha do sensor (não dispara nada, só registra).
    Fogo detectado -> estado `fire`, o robô é parado e os 3 minibots são sinalizados na hora (hooks em minibot/):
      minibot/contencao    minibot 1: contenção, para ninguém por perto se machucar
      minibot/extintor     minibot 2: solta o agente extintor na sala (repetido enquanto o gás indicar fogo)
      minibot/restauracao  minibot 3: só depois do fogo apagado: troca as partes e restaura a programação
    Durante o incêndio só STOP e STATUS são aceitos. Ao terminar o robô volta a `stopped` (nunca volta sozinho a trabalhar).

SEGURANÇA: o ILrDOS não é circuito de segurança. Este agente ajuda (limite de tempo, hook `safe`, lista de IPs
permitidos, hooks que só o dono pode alterar), mas parada de emergência continua sendo hardware.
"""
import argparse
import ipaddress
import os
import re
import socket
import stat
import subprocess
import sys
import threading
import time

PRIVATE = ["10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "169.254.0.0/16", "127.0.0.0/8"]
STATES = ("working", "stopped", "paused", "docking", "fire", "recovering")
FIRE_STATES = ("fire", "recovering")
CUSTOM_WORD = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,31}$")   # vira nome de arquivo em hooks/cmd/: nada de / nem ..
CUSTOM_ARG = re.compile(r"^[A-Za-z0-9_.:-]{1,32}$")


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


class Robot:
    def __init__(self, hooks, max_run, hook_timeout, fire_timeout=120, clear_seconds=10, extinguish_tries=5):
        self.hooks = os.path.realpath(hooks)
        self.max_run = max_run
        self.hook_timeout = hook_timeout
        self.fire_timeout = fire_timeout            # tempo máximo de cada hook de minibot (apagar leva mais que um comando comum)
        self.clear_seconds = clear_seconds          # segundos seguidos de ar normal para dar o fogo por apagado
        self.extinguish_tries = extinguish_tries    # quantas vezes o minibot 2 repete se o gás continuar indicando fogo
        self.state = "stopped"
        self.started_at = 0.0
        self.state_lock = threading.Lock()
        self.action_lock = threading.Lock()

    def hook_path(self, name):
        path = os.path.join(self.hooks, name)
        if not os.path.isfile(path) or not os.access(path, os.X_OK):
            return None
        mode = os.stat(path).st_mode
        if mode & (stat.S_IWGRP | stat.S_IWOTH):
            log(f"hook '{name}' recusado: só o dono pode ter permissão de escrita (chmod go-w)")
            return None
        return path

    def has(self, name):
        return self.hook_path(name) is not None

    def exec_hook(self, name, arg=None, timeout=None):
        """Executa o hook sem shell (o argumento opcional vai como argv[1]). Devolve (código de saída ou None, primeira linha)."""
        path = self.hook_path(name)
        if path is None:
            return None, f"sem hook '{name}'"
        try:
            done = subprocess.run([path] + ([arg] if arg else []), capture_output=True, text=True, timeout=timeout or self.hook_timeout, cwd=self.hooks)
        except subprocess.TimeoutExpired:
            return None, f"hook '{name}' demorou demais"
        except OSError as error:
            return None, f"hook '{name}' não executou ({error.strerror})"
        lines = (done.stdout or done.stderr or "").strip().splitlines()
        text = "".join(c for c in (lines[0] if lines else "") if c.isprintable())[:80]
        return done.returncode, text

    def run_hook(self, name, arg=None, timeout=None):
        """Devolve (ok, primeira linha de saída)."""
        code, text = self.exec_hook(name, arg, timeout)
        return code == 0, text

    def set_state(self, state):
        with self.state_lock:
            self.state = state
            if state == "working":
                self.started_at = time.monotonic()

    def get_state(self):
        with self.state_lock:
            return self.state

    def stop(self):
        ok, text = self.run_hook("stop")                      # STOP vence tudo: não espera nenhum outro comando
        if ok:
            self.set_state("stopped")
            return "OK"
        return "ERR stop falhou" + (": " + text if text else "")

    def fire_active(self):
        return self.get_state() in FIRE_STATES

    def smoke(self):
        """Lê o detector de gás carbônico: 'fire' (código 1), 'clear' (0) ou 'fault' (falha do sensor)."""
        code, _ = self.exec_hook("gas")
        return {0: "clear", 1: "fire"}.get(code, "fault")

    def fire_protocol(self):
        """Roda numa thread própria: minibots 1 e 2 na hora, minibot 3 só com o fogo apagado."""
        log("FOGO detectado (gás carbônico): parando o robô e sinalizando os 3 minibots")
        self.set_state("fire")
        self.stop()                                           # para tudo que se move; o estado volta a ser 'stopped' abaixo
        self.set_state("fire")
        results = {}

        def bot(label, hook):
            ok, text = self.run_hook(hook, timeout=self.fire_timeout)
            results[label] = ok
            log(f"{label} ({hook}): {'ok' if ok else 'FALHOU'}" + (f" - {text}" if text else ""))

        containment = threading.Thread(target=bot, args=("minibot 1", "minibot/contencao"), daemon=True)
        containment.start()
        tries, clear_since = 0, None
        while True:                                           # minibot 2: apaga, e repete enquanto o gás indicar fogo
            if clear_since is None and tries < self.extinguish_tries:
                tries += 1
                bot("minibot 2", "minibot/extintor")
            time.sleep(1)
            reading = self.smoke()
            if reading == "fire":
                clear_since = None
            elif reading == "clear":
                clear_since = clear_since or time.monotonic()
                if time.monotonic() - clear_since >= self.clear_seconds:
                    break
            if tries >= self.extinguish_tries and clear_since is None and reading == "fire":
                log("fogo persiste depois de todas as tentativas do minibot 2: chame os bombeiros; o robô fica bloqueado")
                time.sleep(5)
        containment.join(self.fire_timeout)
        self.set_state("recovering")
        log("fogo apagado: minibot 3 troca as partes e restaura a programação")
        bot("minibot 3", "minibot/restauracao")
        if results.get("minibot 3"):
            self.set_state("stopped")                         # volta ao normal, mas parado: quem manda trabalhar é você
            log("restauração concluída: robô normal (parado)")
        else:
            self.set_state("fire")
            log("restauração FALHOU: robô bloqueado até alguém consertar e reiniciar o agente")

    def fire_monitor(self):
        while True:
            time.sleep(1)
            if not self.fire_active() and self.smoke() == "fire":
                self.fire_protocol()

    def handle(self, command):
        if command == "STOP":
            return self.stop()
        if self.fire_active() and command != "STATUS":
            return "ERR incêndio em andamento"
        if command == "STATUS":
            state = self.get_state()
            if self.has("status"):
                ok, text = self.run_hook("status")
                word = text.split()[0] if text.split() else ""
                if ok and word in STATES:
                    state = word
            return "OK " + state
        action = {"START": ("start", "working"), "PAUSE": ("pause", "paused"), "DOCK": ("dock", "docking")}.get(command)
        if action is None:
            return self.custom(command)
        hook, new_state = action
        if not self.has(hook):
            return f"ERR este robô não tem a ação {command.lower()}"
        if not self.action_lock.acquire(timeout=3):
            return "ERR robô ocupado"
        try:
            if command == "START" and self.has("safe"):
                ok, text = self.run_hook("safe")
                if not ok:
                    return "ERR inseguro agora" + (": " + text if text else "")
            ok, text = self.run_hook(hook)
            if not ok:
                return f"ERR {command.lower()} falhou" + (": " + text if text else "")
            self.set_state(new_state)
            return "OK"
        finally:
            self.action_lock.release()

    def custom(self, command):
        """Comando do dono: PALAVRA ou PALAVRA:argumento -> hooks/cmd/<palavra> [argumento]. Não muda o estado do robô.
        Só roda se o hook existir (mesmas regras de dono/permissão dos outros) e, havendo hook `safe`, só se for seguro."""
        word, _, arg = command.partition(":")
        word = word.lower()
        if not CUSTOM_WORD.match(word) or ".." in word or (arg and not CUSTOM_ARG.match(arg)):
            return "ERR comando desconhecido"
        name = "cmd/" + word
        if not self.has(name):
            return "ERR comando desconhecido"
        if not self.action_lock.acquire(timeout=3):
            return "ERR robô ocupado"
        try:
            if self.has("safe"):
                ok, text = self.run_hook("safe")
                if not ok:
                    return "ERR inseguro agora" + (": " + text if text else "")
            ok, text = self.run_hook(name, arg.lower() if arg else None)
            if not ok:
                return f"ERR {word} falhou" + (": " + text if text else "")
            return "OK" + (" " + text if text else "")
        finally:
            self.action_lock.release()

    def watchdog(self):
        """Enquanto trabalha: para sozinho no limite de tempo ou quando o hook `safe` falhar."""
        while True:
            time.sleep(1)
            if self.get_state() != "working":
                continue
            with self.state_lock:
                too_long = time.monotonic() - self.started_at > self.max_run
            if too_long:
                log(f"limite de {self.max_run}s atingido: parando")
                self.stop()
            elif self.has("safe") and not self.run_hook("safe")[0]:
                log("hook 'safe' falhou: parando")
                self.stop()


def read_line(conn, limit=64, timeout=2.0):
    conn.settimeout(timeout)
    data = b""
    while len(data) < limit:
        chunk = conn.recv(1)
        if not chunk:
            break
        if chunk == b"\n":
            return data.decode("ascii", errors="ignore").strip()
        if chunk != b"\r":
            data += chunk
    return None


def serve(robot, bind, port, allowed):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((bind, port))
    server.listen(8)
    log(f"robô no ar em {bind}:{port} (hooks: {robot.hooks}, limite {robot.max_run}s)")

    def client(conn, address):
        try:
            command = read_line(conn)
            if command is None:
                return
            reply = robot.handle(command.upper())
            log(f"{address[0]} {command} -> {reply}")
            conn.sendall((reply + "\n").encode())
        except OSError:
            pass
        finally:
            conn.close()

    while True:
        conn, address = server.accept()
        ip = ipaddress.ip_address(address[0])
        if not any(ip in net for net in allowed):
            log(f"{address[0]} recusado (fora de --allow)")
            conn.close()
            continue
        threading.Thread(target=client, args=(conn, address), daemon=True).start()


def main():
    ap = argparse.ArgumentParser(description="Agente ILrDOS para robôs feitos em computadores de placa única.")
    ap.add_argument("--hooks", default="/etc/ilrdos-robot", help="pasta com os hooks (start, stop, pause, dock, status, safe)")
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--allow", default=",".join(PRIVATE), help="IPs/redes que podem comandar (ex.: 192.168.1.10,192.168.1.0/24)")
    ap.add_argument("--max-run", type=int, default=120, help="segundos; depois disso o robô para sozinho")
    ap.add_argument("--hook-timeout", type=int, default=10)
    ap.add_argument("--fire-timeout", type=int, default=120, help="segundos que cada minibot pode levar no protocolo de incêndio")
    ap.add_argument("--clear-seconds", type=int, default=10, help="segundos seguidos de ar normal para dar o fogo por apagado")
    ap.add_argument("--extinguish-tries", type=int, default=5, help="quantas vezes o minibot 2 repete se o gás continuar indicando fogo")
    args = ap.parse_args()
    try:
        allowed = [ipaddress.ip_network(item.strip(), strict=False) for item in args.allow.split(",") if item.strip()]
    except ValueError as error:
        sys.exit(f"--allow inválido: {error}")
    robot = Robot(args.hooks, max(1, args.max_run), max(1, args.hook_timeout), max(1, args.fire_timeout), max(1, args.clear_seconds), max(1, args.extinguish_tries))
    if not robot.has("stop") or not robot.has("start"):
        sys.exit(f"precisa dos hooks 'start' e 'stop' (executáveis, sem escrita para grupo/outros) em {robot.hooks}")
    threading.Thread(target=robot.watchdog, daemon=True).start()
    if robot.has("gas"):
        if not all(robot.has("minibot/" + n) for n in ("contencao", "extintor", "restauracao")):
            sys.exit("com o hook 'gas' precisa dos 3 hooks minibot/contencao, minibot/extintor e minibot/restauracao")
        threading.Thread(target=robot.fire_monitor, daemon=True).start()
        log("protocolo de incêndio ativo (detector de gás carbônico + 3 minibots)")
    serve(robot, args.bind, args.port, allowed)


if __name__ == "__main__":
    main()

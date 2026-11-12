#!/usr/bin/env python3
"""ILrDOS robot agent: faz o SEU robô (um computador de placa única com Linux) obedecer o GLaDOS.

Roda em qualquer placa com Linux e Python 3.8+, sem instalar nada: Raspberry Pi, Orange Pi, Banana Pi, Rock/Radxa,
Odroid, Jetson, BeagleBone, VisionFive, mini PC x86... (ARM, x86 ou RISC-V).

Protocolo (uma palavra por linha, uma linha de resposta):
    START | STOP | PAUSE | DOCK | STATUS   ->   OK | OK <estado> | ERR <motivo>
O agente não mexe no hardware: ele executa scripts SEUS na pasta de hooks (start, stop, pause, dock, status, safe).
Cada hook é um executável qualquer (shell, Python, C...) que liga/desliga motores, GPIO, ROS, etc.

SEGURANÇA: o ILrDOS não é circuito de segurança. Este agente ajuda (limite de tempo, hook `safe`, lista de IPs
permitidos, hooks que só o dono pode alterar), mas parada de emergência continua sendo hardware.
"""
import argparse
import ipaddress
import os
import socket
import stat
import subprocess
import sys
import threading
import time

PRIVATE = ["10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "169.254.0.0/16", "127.0.0.0/8"]
STATES = ("working", "stopped", "paused", "docking")


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


class Robot:
    def __init__(self, hooks, max_run, hook_timeout):
        self.hooks = os.path.realpath(hooks)
        self.max_run = max_run
        self.hook_timeout = hook_timeout
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

    def run_hook(self, name):
        """Executa o hook sem shell. Devolve (ok, primeira linha de saída)."""
        path = self.hook_path(name)
        if path is None:
            return False, f"sem hook '{name}'"
        try:
            done = subprocess.run([path], capture_output=True, text=True, timeout=self.hook_timeout, cwd=self.hooks)
        except subprocess.TimeoutExpired:
            return False, f"hook '{name}' demorou demais"
        except OSError as error:
            return False, f"hook '{name}' não executou ({error.strerror})"
        lines = (done.stdout or done.stderr or "").strip().splitlines()
        text = "".join(c for c in (lines[0] if lines else "") if c.isprintable())[:80]
        return done.returncode == 0, text

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

    def handle(self, command):
        if command == "STOP":
            return self.stop()
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
            return "ERR comando desconhecido"
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
    args = ap.parse_args()
    try:
        allowed = [ipaddress.ip_network(item.strip(), strict=False) for item in args.allow.split(",") if item.strip()]
    except ValueError as error:
        sys.exit(f"--allow inválido: {error}")
    robot = Robot(args.hooks, max(1, args.max_run), max(1, args.hook_timeout))
    if not robot.has("stop") or not robot.has("start"):
        sys.exit(f"precisa dos hooks 'start' e 'stop' (executáveis, sem escrita para grupo/outros) em {robot.hooks}")
    threading.Thread(target=robot.watchdog, daemon=True).start()
    serve(robot, args.bind, args.port, allowed)


if __name__ == "__main__":
    main()

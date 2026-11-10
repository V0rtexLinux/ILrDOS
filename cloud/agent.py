#!/usr/bin/env python3
"""Pairing and sync agent for an ILrDOS robot; standard library only."""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import secrets
import socket
import stat
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

from protocol import validate_remote_command


class CloudError(RuntimeError):
    pass


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


_opener = urllib.request.build_opener(NoRedirect)


def validate_url(base_url: str) -> str:
    parsed = urllib.parse.urlsplit(base_url)
    if parsed.scheme != "https":
        if parsed.scheme != "http" or parsed.hostname not in {"localhost", "127.0.0.1", "::1"}:
            raise ValueError("cloud URL must use HTTPS (HTTP is allowed only for localhost testing)")
    if not parsed.netloc or parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise ValueError("invalid cloud URL")
    return base_url.rstrip("/")


def api_request(base_url: str, path: str, method="GET", payload=None, token=None, timeout=8):
    url = base_url + path
    body = None if payload is None else json.dumps(payload, separators=(",", ":")).encode()
    headers = {"Accept": "application/json"}
    if body is not None:
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        with _opener.open(request, timeout=timeout) as response:
            raw = response.read(65537)
            if len(raw) > 65536:
                raise CloudError("cloud response too large")
            return response.status, json.loads(raw) if raw else {}
    except urllib.error.HTTPError as exc:
        raw = exc.read(4096).decode("utf-8", "replace")
        raise CloudError(f"cloud API returned HTTP {exc.code}: {raw}") from None
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise CloudError(f"cloud connection failed: {exc}") from None


def atomic_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(path.parent, 0o700)
    temporary = path.with_name(path.name + ".tmp")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(data, stream, separators=(",", ":"))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        os.chmod(path, 0o600)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def load_device(path: Path) -> dict:
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
        raise RuntimeError("device credential file must be a regular file with mode 0600")
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("device_id"), str) or not isinstance(data.get("device_token"), str):
        raise RuntimeError("invalid device credential file")
    return data


def read_local_token(path: Path) -> str:
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
        raise RuntimeError("ILrDOS token file must be a regular file with mode 0600")
    token = path.read_text(encoding="utf-8").strip()
    if len(token) < 24:
        raise RuntimeError("ILrDOS local token is too short")
    return token


def recv_line(sock: socket.socket, limit=1024) -> str:
    result = bytearray()
    while len(result) < limit:
        chunk = sock.recv(1)
        if not chunk:
            raise RuntimeError("local daemon closed the connection")
        if chunk == b"\n":
            return result.decode("utf-8", "replace").rstrip("\r")
        if chunk != b"\r":
            result.extend(chunk)
    raise RuntimeError("local daemon line too long")


def local_command(command: str, token_file: Path, port: int = 4317) -> str:
    command = validate_remote_command(command)
    token = read_local_token(token_file)
    with socket.create_connection(("127.0.0.1", port), timeout=4) as sock:
        sock.settimeout(4)
        challenge = recv_line(sock)
        prefix = "ILrDOS/1 CHALLENGE "
        if not challenge.startswith(prefix):
            raise RuntimeError("unexpected local daemon handshake")
        response = hmac.new(token.encode(), challenge[len(prefix):].encode(), hashlib.sha256).hexdigest()
        sock.sendall((response + "\n").encode())
        if recv_line(sock) != "ILrDOS/1 OK":
            raise RuntimeError("local daemon authentication failed")
        recv_line(sock)  # welcome text
        sock.sendall((command + "\n").encode())
        return recv_line(sock)


def pair(args) -> None:
    base_url = validate_url(args.url)
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        raise SystemExit("Pairing must be started interactively on the robot.")
    state_path = Path(args.state_dir) / "device.json"
    if state_path.exists():
        raise SystemExit("This robot already has cloud credentials. Revoke it before pairing again.")
    device_id_path = Path(args.state_dir) / "device-id"
    if device_id_path.exists():
        device_id = device_id_path.read_text(encoding="utf-8").strip()
        uuid.UUID(device_id)
    else:
        device_id = str(uuid.uuid4())
        device_id_path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        fd = os.open(device_id_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(device_id + "\n")
    print("O pareamento permite comandos remotos limitados a status, parar e movimento breve.")
    print("Inicie isto somente quando a pessoa responsável estiver junto ao robô.")
    if input("Para consentir, digite AUTORIZO: ").strip() != "AUTORIZO":
        raise SystemExit("Pareamento cancelado.")

    code = f"{secrets.randbelow(100_000_000):08d}"
    pair_secret = secrets.token_urlsafe(32)
    status, started = api_request(base_url, "/v1/pairings", method="POST", payload={
        "device_id": device_id, "label": args.name, "code": code, "pair_secret": pair_secret,
    })
    pairing_id = started["pairing_id"]
    print(f"PIN temporário do robô: {code}")
    print("Ele expira em 5 minutos. Insira-o no painel/API administrativa do servidor.")
    deadline = time.monotonic() + 5 * 60
    while time.monotonic() < deadline:
        try:
            _, response = api_request(
                base_url, f"/v1/pairings/{pairing_id}/poll", method="POST",
                payload={"pair_secret": pair_secret}, timeout=5,
            )
            if response.get("status") == "approved":
                device_token = response.get("device_token")
                if not isinstance(device_token, str) or len(device_token) < 40:
                    raise RuntimeError("invalid device credential returned by server")
                atomic_json(state_path, {"device_id": device_id, "device_token": device_token})
                api_request(
                    base_url, f"/v1/pairings/{pairing_id}/complete", method="POST",
                    payload={"pair_secret": pair_secret},
                )
                print("Pareamento concluído. O token foi salvo localmente com permissão 0600.")
                return
        except CloudError as exc:
            print(str(exc), file=sys.stderr)
            raise SystemExit(1)
        time.sleep(2)
    raise SystemExit("O PIN expirou. Inicie um novo pareamento no robô.")


def run_agent(args) -> None:
    base_url = validate_url(args.url)
    state_path = Path(args.state_dir) / "device.json"
    device = load_device(state_path)
    token_file = Path(args.local_token_file)
    device_id = device["device_id"]
    cloud_token = device["device_token"]
    last_heartbeat = 0.0
    print(f"Agente ILrDOS conectado para o robô {device_id}. Ctrl+C encerra.")
    try:
        while True:
            current = time.monotonic()
            if current - last_heartbeat >= args.heartbeat_seconds:
                try:
                    state_text = local_command("status", token_file, args.local_port)
                    api_request(
                        base_url, f"/v1/device/{device_id}/heartbeat", method="POST",
                        payload={"state": {"status": state_text}}, token=cloud_token,
                    )
                except CloudError as exc:
                    if "HTTP 403" in str(exc):
                        try:
                            local_command("stop", token_file, args.local_port)
                        except Exception:
                            pass
                        raise SystemExit("Controle revogado ou credencial inválida; o robô recebeu stop.")
                    raise
                last_heartbeat = current
            try:
                status, command_data = api_request(
                    base_url, f"/v1/device/{device_id}/commands/next", token=cloud_token, timeout=5,
                )
            except CloudError as exc:
                if "HTTP 403" in str(exc):
                    try:
                        local_command("stop", token_file, args.local_port)
                    except Exception:
                        pass
                    raise SystemExit("Controle revogado ou credencial inválida; o robô recebeu stop.")
                raise
            if status == 204:
                time.sleep(args.poll_seconds)
                continue
            command_id = command_data.get("id")
            try:
                command = validate_remote_command(command_data.get("command"))
                result = local_command(command, token_file, args.local_port)
                result_status = "succeeded"
            except Exception as exc:
                result = str(exc)[:500]
                result_status = "failed"
            try:
                api_request(
                    base_url, f"/v1/device/{device_id}/commands/{command_id}/result",
                    method="POST", payload={"status": result_status, "result": result}, token=cloud_token,
                )
            except CloudError as exc:
                if "HTTP 403" in str(exc):
                    try:
                        local_command("stop", token_file, args.local_port)
                    except Exception:
                        pass
                    raise SystemExit("Controle revogado; o robô recebeu stop.")
                raise
            print(f"Comando {command_id}: {result_status}")
    except KeyboardInterrupt:
        try:
            local_command("stop", token_file, args.local_port)
        except Exception:
            pass
        print("\nAgente encerrado; solicitado stop ao daemon local.")


def make_parser():
    parser = argparse.ArgumentParser(description="Agente de sincronização em nuvem do ILrDOS")
    sub = parser.add_subparsers(dest="action", required=True)
    pair_parser = sub.add_parser("pair", help="iniciar pareamento consentido no robô")
    pair_parser.add_argument("--url", required=True)
    pair_parser.add_argument("--name", default="ILrDOS robot")
    pair_parser.add_argument("--state-dir", default="/var/lib/ilrdos-cloud")
    pair_parser.set_defaults(func=pair)
    run_parser = sub.add_parser("run", help="sincronizar estado, comandos e logs")
    run_parser.add_argument("--url", required=True)
    run_parser.add_argument("--state-dir", default="/var/lib/ilrdos-cloud")
    run_parser.add_argument("--local-token-file", default="/etc/ilrdos/token")
    run_parser.add_argument("--local-port", type=int, default=4317)
    run_parser.add_argument("--poll-seconds", type=float, default=1.0)
    run_parser.add_argument("--heartbeat-seconds", type=int, default=15)
    run_parser.set_defaults(func=run_agent)
    return parser


if __name__ == "__main__":
    args = make_parser().parse_args()
    args.url = validate_url(args.url)
    args.func(args)

#!/usr/bin/env python3
"""ILrDOS's self-hosted cloud sync API (Python standard library + SQLite)."""

from __future__ import annotations

import hashlib
import hmac
import json
import os
import re
import secrets
import sqlite3
import threading
import time
import uuid
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

from protocol import validate_remote_command

MAX_BODY = 64 * 1024
PAIRING_TTL = 5 * 60
COMMAND_TTL = 15
CLAIM_LIMIT = 5
PAIRING_START_LIMIT = 30
ADMIN_TOKEN_ENV = "ILRDOS_CLOUD_ADMIN_TOKEN"
DB_ENV = "ILRDOS_CLOUD_DB"
_maintenance_lock = threading.Lock()
_last_maintenance = 0


def digest(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def now() -> int:
    return int(time.time())


def connect_db(path: str) -> sqlite3.Connection:
    db = sqlite3.connect(path, timeout=5, isolation_level=None)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys = ON")
    db.execute("PRAGMA busy_timeout = 5000")
    return db


def initialize_db(path: str) -> None:
    if path != ":memory:":
        db_path = Path(path)
        db_path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    db = connect_db(path)
    try:
        db.executescript("""
            PRAGMA journal_mode = WAL;
            CREATE TABLE IF NOT EXISTS devices (
                id TEXT PRIMARY KEY,
                label TEXT NOT NULL,
                token_hash TEXT NOT NULL,
                created_at INTEGER NOT NULL,
                last_seen INTEGER,
                revoked_at INTEGER,
                state_json TEXT NOT NULL DEFAULT '{}'
            );
            CREATE TABLE IF NOT EXISTS pairings (
                id TEXT PRIMARY KEY,
                device_id TEXT NOT NULL,
                label TEXT NOT NULL,
                code_hash TEXT NOT NULL,
                secret_hash TEXT NOT NULL,
                created_at INTEGER NOT NULL,
                expires_at INTEGER NOT NULL,
                approved_at INTEGER,
                device_token TEXT,
                completed_at INTEGER
            );
            CREATE INDEX IF NOT EXISTS pairings_code_idx ON pairings(code_hash, expires_at);
            CREATE TABLE IF NOT EXISTS commands (
                id TEXT PRIMARY KEY,
                device_id TEXT NOT NULL,
                command TEXT NOT NULL,
                status TEXT NOT NULL,
                created_at INTEGER NOT NULL,
                expires_at INTEGER NOT NULL,
                dispatched_at INTEGER,
                finished_at INTEGER,
                result TEXT NOT NULL DEFAULT ''
            );
            CREATE INDEX IF NOT EXISTS commands_queue_idx ON commands(device_id, status, created_at);
            CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                created_at INTEGER NOT NULL,
                kind TEXT NOT NULL,
                payload_json TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS events_device_idx ON events(device_id, id DESC);
            CREATE TABLE IF NOT EXISTS claim_attempts (
                source_ip TEXT NOT NULL,
                attempted_at INTEGER NOT NULL
            );
            CREATE INDEX IF NOT EXISTS claim_attempts_time_idx ON claim_attempts(source_ip, attempted_at);
            CREATE TABLE IF NOT EXISTS pairing_starts (
                source_ip TEXT NOT NULL,
                started_at INTEGER NOT NULL
            );
            CREATE INDEX IF NOT EXISTS pairing_starts_time_idx ON pairing_starts(source_ip, started_at);
        """)
        if path != ":memory:":
            os.chmod(path, 0o600)
    finally:
        db.close()


def add_event(db: sqlite3.Connection, device_id: str, kind: str, payload: object) -> None:
    db.execute(
        "INSERT INTO events(device_id, created_at, kind, payload_json) VALUES(?,?,?,?)",
        (device_id, now(), kind, json.dumps(payload, ensure_ascii=False, separators=(",", ":"))),
    )


def maybe_cleanup(db: sqlite3.Connection) -> None:
    global _last_maintenance
    current = now()
    if current - _last_maintenance < 3600:
        return
    with _maintenance_lock:
        if current - _last_maintenance < 3600:
            return
        retention = current - 30 * 86400
        db.execute("DELETE FROM events WHERE created_at < ?", (retention,))
        db.execute(
            "DELETE FROM commands WHERE created_at < ? AND status IN "
            "('succeeded','failed','unknown','expired','cancelled')",
            (retention,),
        )
        db.execute("DELETE FROM pairings WHERE expires_at < ?", (retention,))
        db.execute("DELETE FROM claim_attempts WHERE attempted_at < ?", (current - 3600,))
        db.execute("DELETE FROM pairing_starts WHERE started_at < ?", (current - 3600,))
        _last_maintenance = current


class CloudServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, handler, db_path: str, admin_token: str):
        self.db_path = db_path
        self.admin_token = admin_token
        super().__init__(address, handler)


class Handler(BaseHTTPRequestHandler):
    server: CloudServer
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        # Avoid logging authorization headers or request bodies.
        print(f"[http] {self.client_address[0]} {self.command} {self.path.split('?')[0]}")

    def send_json(self, status: int, payload: object) -> None:
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_empty(self, status: int) -> None:
        self.send_response(status)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def read_json(self) -> dict:
        raw_length = self.headers.get("Content-Length", "")
        if not raw_length.isdigit() or int(raw_length) > MAX_BODY:
            raise ValueError("invalid or oversized Content-Length")
        length = int(raw_length)
        raw = self.rfile.read(length)
        data = json.loads(raw.decode("utf-8") if raw else "{}")
        if not isinstance(data, dict):
            raise ValueError("JSON body must be an object")
        return data

    @contextmanager
    def db(self):
        connection = connect_db(self.server.db_path)
        try:
            yield connection
        finally:
            connection.close()

    def require_admin(self) -> bool:
        header = self.headers.get("Authorization", "")
        supplied = header[7:] if header.startswith("Bearer ") else ""
        return bool(supplied) and hmac.compare_digest(supplied, self.server.admin_token)

    def authenticated_device(self, device_id: str, db: sqlite3.Connection):
        header = self.headers.get("Authorization", "")
        supplied = header[7:] if header.startswith("Bearer ") else ""
        if not supplied:
            return None
        row = db.execute(
            "SELECT * FROM devices WHERE id=? AND revoked_at IS NULL", (device_id,)
        ).fetchone()
        if row is None or not hmac.compare_digest(digest(supplied), row["token_hash"]):
            return None
        return row

    def do_GET(self):
        path = urlsplit(self.path).path.rstrip("/") or "/"
        if path == "/healthz":
            return self.send_json(200, {"ok": True})
        if path == "/v1/admin/devices":
            if not self.require_admin():
                return self.send_json(401, {"error": "unauthorized"})
            with self.db() as db:
                rows = db.execute(
                    "SELECT id,label,created_at,last_seen,revoked_at,state_json FROM devices ORDER BY created_at DESC"
                ).fetchall()
            return self.send_json(200, {
                "devices": [{
                    "id": r["id"], "label": r["label"], "created_at": r["created_at"],
                    "last_seen": r["last_seen"], "revoked": r["revoked_at"] is not None,
                    "state": json.loads(r["state_json"]),
                } for r in rows]
            })
        if path.startswith("/v1/admin/devices/"):
            if not self.require_admin():
                return self.send_json(401, {"error": "unauthorized"})
            bits = path.split("/")
            if len(bits) == 6 and bits[5] == "events":
                device_id = bits[4]
                with self.db() as db:
                    rows = db.execute(
                        "SELECT id,created_at,kind,payload_json FROM events WHERE device_id=? ORDER BY id DESC LIMIT 100",
                        (device_id,),
                    ).fetchall()
                return self.send_json(200, {"events": [{
                    "id": r["id"], "created_at": r["created_at"], "kind": r["kind"],
                    "payload": json.loads(r["payload_json"]),
                } for r in rows]})
        if path.startswith("/v1/device/") and path.endswith("/commands/next"):
            bits = path.split("/")
            if len(bits) != 6:
                return self.send_json(404, {"error": "not_found"})
            device_id = bits[3]
            with self.db() as db:
                if not self.authenticated_device(device_id, db):
                    return self.send_json(403, {"error": "device_revoked_or_unauthorized"})
                current = now()
                db.execute("BEGIN IMMEDIATE")
                db.execute(
                    "UPDATE commands SET status='expired',finished_at=? WHERE device_id=? AND status='queued' AND expires_at<=?",
                    (current, device_id, current),
                )
                db.execute(
                    "UPDATE commands SET status='unknown',finished_at=?,result='agent did not report a result' "
                    "WHERE device_id=? AND status='dispatched' AND expires_at<=?",
                    (current, device_id, current),
                )
                row = db.execute(
                    "SELECT id,command,expires_at FROM commands WHERE device_id=? AND status='queued' "
                    "AND expires_at>? ORDER BY created_at,id LIMIT 1",
                    (device_id, current),
                ).fetchone()
                if row is None:
                    db.execute("COMMIT")
                    return self.send_empty(204)
                db.execute(
                    "UPDATE commands SET status='dispatched',dispatched_at=? WHERE id=? AND status='queued'",
                    (current, row["id"]),
                )
                add_event(db, device_id, "command_dispatched", {"command_id": row["id"]})
                db.execute("COMMIT")
            return self.send_json(200, {
                "id": row["id"], "command": row["command"], "expires_at": row["expires_at"]
            })
        return self.send_json(404, {"error": "not_found"})

    def do_POST(self):
        path = urlsplit(self.path).path.rstrip("/")
        try:
            data = self.read_json()
        except (ValueError, UnicodeDecodeError, json.JSONDecodeError):
            return self.send_json(400, {"error": "invalid_json"})

        if path == "/v1/pairings":
            return self.start_pairing(data)
        if path == "/v1/pairings/claim":
            if not self.require_admin():
                return self.send_json(401, {"error": "unauthorized"})
            return self.claim_pairing(data)
        if path.startswith("/v1/pairings/"):
            parts = path.split("/")
            if len(parts) == 5 and parts[4] in {"poll", "complete"}:
                return self.pairing_action(parts[3], parts[4], data)
        if path == "/v1/admin/commands":
            if not self.require_admin():
                return self.send_json(401, {"error": "unauthorized"})
            return self.create_command(data)
        if path.startswith("/v1/admin/devices/") and path.endswith("/revoke"):
            if not self.require_admin():
                return self.send_json(401, {"error": "unauthorized"})
            parts = path.split("/")
            if len(parts) == 6:
                return self.revoke_device(parts[4])
        if path.startswith("/v1/device/") and path.endswith("/heartbeat"):
            parts = path.split("/")
            if len(parts) == 5:
                return self.heartbeat(parts[3], data)
        if path.startswith("/v1/device/") and "/commands/" in path and path.endswith("/result"):
            parts = path.split("/")
            if len(parts) == 7:
                return self.command_result(parts[3], parts[5], data)
        return self.send_json(404, {"error": "not_found"})

    def start_pairing(self, data: dict):
        device_id = data.get("device_id")
        label = data.get("label", "ILrDOS robot")
        code = data.get("code")
        secret = data.get("pair_secret")
        if not isinstance(device_id, str) or not re.fullmatch(r"[0-9a-fA-F-]{36}", device_id):
            return self.send_json(400, {"error": "invalid_device_id"})
        if not isinstance(label, str) or not label.strip() or len(label) > 80:
            return self.send_json(400, {"error": "invalid_label"})
        if not isinstance(code, str) or not re.fullmatch(r"[0-9]{8}", code):
            return self.send_json(400, {"error": "invalid_pairing_code"})
        if not isinstance(secret, str) or len(secret) < 40 or len(secret) > 128:
            return self.send_json(400, {"error": "invalid_pairing_secret"})
        pairing_id = str(uuid.uuid4())
        created = now()
        with self.db() as db:
            source_ip = self.client_address[0]
            db.execute("DELETE FROM pairing_starts WHERE started_at < ?", (created - 3600,))
            starts = db.execute(
                "SELECT COUNT(*) AS n FROM pairing_starts WHERE source_ip=?", (source_ip,)
            ).fetchone()["n"]
            if starts >= PAIRING_START_LIMIT:
                return self.send_json(429, {"error": "too_many_pairing_requests"})
            db.execute(
                "INSERT INTO pairing_starts(source_ip,started_at) VALUES(?,?)",
                (source_ip, created),
            )
            device = db.execute("SELECT revoked_at FROM devices WHERE id=?", (device_id,)).fetchone()
            if device is not None and device["revoked_at"] is None:
                return self.send_json(409, {"error": "device_already_paired"})
            pending = db.execute(
                "SELECT id FROM pairings WHERE device_id=? AND expires_at>? AND approved_at IS NULL",
                (device_id, created),
            ).fetchone()
            if pending is not None:
                return self.send_json(409, {"error": "pairing_already_pending"})
            db.execute(
                "INSERT INTO pairings(id,device_id,label,code_hash,secret_hash,created_at,expires_at) VALUES(?,?,?,?,?,?,?)",
                (pairing_id, device_id, label.strip(), digest(code), digest(secret), created, created + PAIRING_TTL),
            )
            add_event(db, device_id, "pairing_requested", {"label": label.strip(), "expires_at": created + PAIRING_TTL})
        return self.send_json(201, {"pairing_id": pairing_id, "expires_at": created + PAIRING_TTL})

    def claim_pairing(self, data: dict):
        code = data.get("code")
        if not isinstance(code, str) or not re.fullmatch(r"[0-9]{8}", code):
            return self.send_json(400, {"error": "invalid_pairing_code"})
        source_ip = self.client_address[0]
        current = now()
        token = secrets.token_urlsafe(32)
        with self.db() as db:
            db.execute("DELETE FROM claim_attempts WHERE attempted_at < ?", (current - 60,))
            attempts = db.execute(
                "SELECT COUNT(*) AS n FROM claim_attempts WHERE source_ip=?", (source_ip,)
            ).fetchone()["n"]
            if attempts >= CLAIM_LIMIT:
                return self.send_json(429, {"error": "too_many_attempts"})
            db.execute("INSERT INTO claim_attempts(source_ip,attempted_at) VALUES(?,?)", (source_ip, current))
            db.execute("BEGIN IMMEDIATE")
            row = db.execute(
                "SELECT * FROM pairings WHERE code_hash=? AND expires_at>? AND approved_at IS NULL "
                "ORDER BY created_at DESC LIMIT 1",
                (digest(code), current),
            ).fetchone()
            if row is None:
                db.execute("COMMIT")
                return self.send_json(404, {"error": "pairing_not_found_or_expired"})
            existing = db.execute("SELECT revoked_at FROM devices WHERE id=?", (row["device_id"],)).fetchone()
            if existing is not None and existing["revoked_at"] is None:
                db.execute("COMMIT")
                return self.send_json(409, {"error": "device_already_paired"})
            db.execute(
                "UPDATE pairings SET approved_at=?,device_token=? WHERE id=?",
                (current, token, row["id"]),
            )
            db.execute(
                "INSERT INTO devices(id,label,token_hash,created_at,last_seen,revoked_at,state_json) "
                "VALUES(?,?,?,?,NULL,NULL,'{}') ON CONFLICT(id) DO UPDATE SET "
                "label=excluded.label,token_hash=excluded.token_hash,created_at=excluded.created_at,"
                "last_seen=NULL,revoked_at=NULL,state_json='{}'",
                (row["device_id"], row["label"], digest(token), current),
            )
            add_event(db, row["device_id"], "pairing_approved", {"approved_at": current})
            db.execute("COMMIT")
        return self.send_json(200, {"pairing_id": row["id"], "status": "approved"})

    def pairing_action(self, pairing_id: str, action: str, data: dict):
        secret = data.get("pair_secret")
        if not isinstance(secret, str) or len(secret) < 40:
            return self.send_json(400, {"error": "invalid_pairing_secret"})
        with self.db() as db:
            row = db.execute("SELECT * FROM pairings WHERE id=?", (pairing_id,)).fetchone()
            if row is None or not hmac.compare_digest(digest(secret), row["secret_hash"]):
                return self.send_json(403, {"error": "pairing_not_found"})
            if row["expires_at"] <= now() and row["approved_at"] is None:
                return self.send_json(410, {"error": "pairing_expired"})
            if action == "poll":
                if row["approved_at"] is None or row["device_token"] is None:
                    return self.send_json(200, {"status": "pending"})
                return self.send_json(200, {"status": "approved", "device_token": row["device_token"]})
            if row["approved_at"] is None or row["device_token"] is None:
                return self.send_json(409, {"error": "pairing_not_approved"})
            db.execute(
                "UPDATE pairings SET device_token=NULL,secret_hash='',completed_at=? WHERE id=?",
                (now(), pairing_id),
            )
        return self.send_json(200, {"status": "complete"})

    def create_command(self, data: dict):
        device_id = data.get("device_id")
        try:
            command = validate_remote_command(data.get("command"))
        except ValueError as exc:
            return self.send_json(400, {"error": str(exc)})
        if not isinstance(device_id, str):
            return self.send_json(400, {"error": "invalid_device_id"})
        command_id = str(uuid.uuid4())
        current = now()
        with self.db() as db:
            device = db.execute(
                "SELECT id FROM devices WHERE id=? AND revoked_at IS NULL", (device_id,)
            ).fetchone()
            if device is None:
                return self.send_json(404, {"error": "active_device_not_found"})
            pending = db.execute(
                "SELECT COUNT(*) AS n FROM commands WHERE device_id=? AND status IN ('queued','dispatched')",
                (device_id,),
            ).fetchone()["n"]
            if pending >= 10:
                return self.send_json(429, {"error": "too_many_pending_commands"})
            db.execute(
                "INSERT INTO commands(id,device_id,command,status,created_at,expires_at) VALUES(?,?,?,'queued',?,?)",
                (command_id, device_id, command, current, current + COMMAND_TTL),
            )
            add_event(db, device_id, "command_queued", {"command_id": command_id, "command": command})
        return self.send_json(201, {"id": command_id, "status": "queued", "expires_at": current + COMMAND_TTL})

    def revoke_device(self, device_id: str):
        current = now()
        with self.db() as db:
            row = db.execute("SELECT id FROM devices WHERE id=?", (device_id,)).fetchone()
            if row is None:
                return self.send_json(404, {"error": "device_not_found"})
            db.execute("UPDATE devices SET revoked_at=? WHERE id=?", (current, device_id))
            db.execute(
                "UPDATE commands SET status='cancelled',finished_at=? WHERE device_id=? AND status='queued'",
                (current, device_id),
            )
            add_event(db, device_id, "control_revoked", {"revoked_at": current})
        return self.send_json(200, {"device_id": device_id, "revoked": True})

    def heartbeat(self, device_id: str, data: dict):
        state = data.get("state")
        if not isinstance(state, dict) or len(json.dumps(state)) > 4096:
            return self.send_json(400, {"error": "invalid_state"})
        status = state.get("status", "")
        if not isinstance(status, str) or len(status) > 512:
            return self.send_json(400, {"error": "invalid_status"})
        status = re.sub(r"\s*\|\s*ligado=\d+s\s*$", "", status)
        safe_state = {"status": status, "control_mode": "autonomous"}
        current = now()
        with self.db() as db:
            device = self.authenticated_device(device_id, db)
            if not device:
                return self.send_json(403, {"error": "device_revoked_or_unauthorized"})
            maybe_cleanup(db)
            state_changed = device["state_json"] != json.dumps(safe_state, ensure_ascii=False)
            db.execute(
                "UPDATE devices SET last_seen=?,state_json=? WHERE id=?",
                (current, json.dumps(safe_state, ensure_ascii=False), device_id),
            )
            if state_changed:
                add_event(db, device_id, "state_changed", safe_state)
        return self.send_json(200, {"ok": True, "server_time": current})

    def command_result(self, device_id: str, command_id: str, data: dict):
        status = data.get("status")
        result = data.get("result", "")
        if status not in {"succeeded", "failed", "unknown"}:
            return self.send_json(400, {"error": "invalid_result_status"})
        if not isinstance(result, str) or len(result) > 512:
            return self.send_json(400, {"error": "invalid_result"})
        current = now()
        with self.db() as db:
            if not self.authenticated_device(device_id, db):
                return self.send_json(403, {"error": "device_revoked_or_unauthorized"})
            changed = db.execute(
                "UPDATE commands SET status=?,finished_at=?,result=? "
                "WHERE id=? AND device_id=? AND status='dispatched'",
                (status, current, result, command_id, device_id),
            ).rowcount
            if not changed:
                return self.send_json(409, {"error": "command_not_dispatched"})
            add_event(db, device_id, "command_result", {
                "command_id": command_id, "status": status, "result": result,
            })
        return self.send_json(200, {"ok": True})


def main() -> None:
    os.umask(0o077)
    admin_token = os.environ.get(ADMIN_TOKEN_ENV, "")
    if len(admin_token) < 32:
        raise SystemExit(f"Set {ADMIN_TOKEN_ENV} to a random secret of at least 32 characters.")
    db_path = os.environ.get(DB_ENV, "./data/ilrdos-cloud.sqlite3")
    initialize_db(db_path)
    host = os.environ.get("ILRDOS_CLOUD_BIND", "127.0.0.1")
    port = int(os.environ.get("ILRDOS_CLOUD_PORT", "8765"))
    server = CloudServer((host, port), Handler, db_path, admin_token)
    print(f"ILrDOS cloud API listening on {host}:{port}; SQLite database ready.")
    print("Expose it only through a TLS-terminating reverse proxy; do not expose this port directly.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()

from __future__ import annotations

import http.client
import json
import os
import socket
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from agent import local_command, validate_url
from protocol import validate_remote_command
from server import CloudServer, Handler, initialize_db


ADMIN = "test-admin-token-that-is-long-enough-0123456789"


class CloudApiTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.db_path = str(Path(self.temp.name) / "cloud.sqlite3")
        initialize_db(self.db_path)
        self.server = CloudServer(("127.0.0.1", 0), Handler, self.db_path, ADMIN)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        host, port = self.server.server_address
        self.base = f"http://{host}:{port}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)
        self.temp.cleanup()

    def request(self, path, method="GET", payload=None, token=None, expect_error=False):
        body = None if payload is None else json.dumps(payload).encode()
        headers = {"Content-Type": "application/json"}
        if token:
            headers["Authorization"] = f"Bearer {token}"
        request = urllib.request.Request(self.base + path, data=body, headers=headers, method=method)
        try:
            with urllib.request.urlopen(request, timeout=3) as response:
                raw = response.read()
                return response.status, json.loads(raw) if raw else {}
        except urllib.error.HTTPError as exc:
            raw = exc.read()
            if not expect_error:
                raise
            return exc.code, json.loads(raw) if raw else {}

    def pair_device(self):
        import secrets
        import uuid
        device_id = str(uuid.uuid4())
        pairing_secret = secrets.token_urlsafe(32)
        code = "01234567"
        status, started = self.request("/v1/pairings", "POST", {
            "device_id": device_id,
            "label": "test robot",
            "code": code,
            "pair_secret": pairing_secret,
        })
        self.assertEqual(status, 201)
        status, claim = self.request("/v1/pairings/claim", "POST", {"code": code}, ADMIN)
        self.assertEqual(status, 200)
        status, credentials = self.request(
            f"/v1/pairings/{started['pairing_id']}/poll", "POST",
            {"pair_secret": pairing_secret},
        )
        self.assertEqual(status, 200)
        self.assertEqual(credentials["status"], "approved")
        self.request(
            f"/v1/pairings/{started['pairing_id']}/complete", "POST",
            {"pair_secret": pairing_secret},
        )
        return device_id, credentials["device_token"]

    def test_pair_heartbeat_command_result_and_revocation(self):
        device_id, device_token = self.pair_device()
        status, _ = self.request(
            f"/v1/device/{device_id}/heartbeat", "POST",
            {"state": {"status": "ILrDOS simulado"}}, device_token,
        )
        self.assertEqual(status, 200)
        status, devices = self.request("/v1/admin/devices", token=ADMIN)
        self.assertEqual(status, 200)
        saved = devices["devices"][0]
        self.assertEqual(saved["id"], device_id)
        self.assertEqual(saved["state"]["control_mode"], "autonomous")

        status, created = self.request("/v1/admin/commands", "POST", {
            "device_id": device_id, "command": "move forward 1000 25",
        }, ADMIN)
        self.assertEqual(status, 201)
        status, next_command = self.request(
            f"/v1/device/{device_id}/commands/next", token=device_token,
        )
        self.assertEqual(status, 200)
        self.assertEqual(next_command["id"], created["id"])
        self.assertEqual(next_command["command"], "move forward 1000 25")
        status, _ = self.request(
            f"/v1/device/{device_id}/commands/{created['id']}/result", "POST",
            {"status": "succeeded", "result": "Movendo para frente."}, device_token,
        )
        self.assertEqual(status, 200)
        status, events = self.request(f"/v1/admin/devices/{device_id}/events", token=ADMIN)
        self.assertEqual(status, 200)
        self.assertTrue(any(event["kind"] == "command_result" for event in events["events"]))

        self.request(f"/v1/admin/devices/{device_id}/revoke", "POST", {}, ADMIN)
        status, _ = self.request(
            f"/v1/device/{device_id}/heartbeat", "POST",
            {"state": {"status": "still running"}}, device_token, expect_error=True,
        )
        self.assertEqual(status, 403)

    def test_admin_auth_and_command_allowlist(self):
        status, _ = self.request("/v1/admin/devices", expect_error=True)
        self.assertEqual(status, 401)
        device_id, _ = self.pair_device()
        for command in ("shell reboot", "iot portao on", "move forward 9000 80", "turn left 90"):
            status, _ = self.request(
                "/v1/admin/commands", "POST",
                {"device_id": device_id, "command": command}, ADMIN, expect_error=True,
            )
            self.assertEqual(status, 400, command)


class ProtocolTest(unittest.TestCase):
    def test_command_bounds(self):
        self.assertEqual(validate_remote_command(" move forward 1000 35 "), "move forward 1000 35")
        self.assertEqual(validate_remote_command("turn right 45"), "turn right 45")
        for command in (
            "move back 1001 10", "move forward 3001 20", "turn left 46",
            "iot lock on", "exec echo hi", "status anything",
        ):
            with self.subTest(command=command), self.assertRaises(ValueError):
                validate_remote_command(command)

    def test_https_required_for_remote_cloud(self):
        self.assertEqual(validate_url("https://cloud.example/api/"), "https://cloud.example/api")
        self.assertEqual(validate_url("http://127.0.0.1:8765"), "http://127.0.0.1:8765")
        with self.assertRaises(ValueError):
            validate_url("http://cloud.example")

    def test_local_agent_uses_challenge_response_and_allowlist(self):
        with tempfile.TemporaryDirectory() as temp:
            token_file = Path(temp) / "token"
            token = "local-token-that-is-long-enough-0123456789"
            token_file.write_text(token + "\n")
            token_file.chmod(0o600)
            listener = socket.socket()
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            port = listener.getsockname()[1]
            outcome = {}

            def serve():
                conn, _ = listener.accept()
                with conn:
                    conn.sendall(b"ILrDOS/1 CHALLENGE abc123\n")
                    answer = b""
                    while not answer.endswith(b"\n"):
                        answer += conn.recv(1)
                    import hashlib
                    import hmac
                    expected = hmac.new(token.encode(), b"abc123", hashlib.sha256).hexdigest().encode() + b"\n"
                    outcome["authenticated"] = answer == expected
                    conn.sendall(b"ILrDOS/1 OK\nWelcome\n")
                    command = b""
                    while not command.endswith(b"\n"):
                        command += conn.recv(1)
                    outcome["command"] = command.decode().strip()
                    conn.sendall(b"Movendo para frente.\n")
                listener.close()

            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            reply = local_command("move forward 1000 25", token_file, port)
            thread.join(timeout=3)
            self.assertTrue(outcome["authenticated"])
            self.assertEqual(outcome["command"], "move forward 1000 25")
            self.assertEqual(reply, "Movendo para frente.")


if __name__ == "__main__":
    unittest.main()

# ILrDOS Cloud Sync

Self-hosted synchronization for robot presence/state, a bounded command queue, and audit events. The service uses Python's standard library and a SQLite database owned by the ILrDOS deployment; it does not depend on Supabase or a hosted database vendor. SQLite is appropriate for a single service instance. Use a managed database or a replicated design only if the deployment later needs multiple API instances.

## Security model

- The robot remains autonomous by default. The agent does not turn autonomy off.
- Pairing starts only from an interactive terminal on the robot, requires the operator to type `AUTORIZO`, and creates an eight-digit PIN that expires after five minutes. An administrator must claim that PIN using the server's separate admin secret before the robot receives its per-device credential.
- Device credentials are random and stored as hashes on the server. The robot's credential file and local daemon token must both have mode `0600`.
- The cloud agent connects only to the ILrDOS daemon on `127.0.0.1`; it never opens the robot daemon port to the internet.
- Remote commands are an allowlist: `status`, `stop`, movement up to 3 seconds at 35% speed (reverse is limited to 1 second and 30%), and turns up to 45 degrees. No shell, arbitrary text, IoT, face, or configuration commands are accepted.
- Local daemon movement limits, sonar, impact lockout, temperature, voltage, and twist safeguards remain authoritative. A disconnected agent cannot extend a movement; daemon movement commands already stop on their local timeout.
- Revocation invalidates the device credential, cancels queued commands, and makes the agent request `stop` when it observes revocation.
- Command delivery is at-most-once. A command that expires or whose result is lost is recorded as expired/unknown; the service will not replay a possibly executed movement.
- The API process binds to `127.0.0.1` by default. Put it behind a TLS-terminating reverse proxy and do not expose port 8765 directly. The robot agent rejects non-HTTPS cloud URLs except for localhost tests.

Pairing uses an operator confirmation prompt and an expiring PIN. It is not hardware attestation: for higher-assurance deployments, wire a physical consent button into the robot and require that signal before enabling the pairing command.

## Run the cloud service

Python 3.10+ is sufficient; no pip packages are required.

1. Put `cloud/` on a Linux host and create a dedicated service account and writable database directory.
2. Configure these variables through the host's secret manager or a root-owned `0600` environment file:
   - `ILRDOS_CLOUD_ADMIN_TOKEN`: random secret, at least 32 characters.
   - `ILRDOS_CLOUD_DB`: SQLite file path, for example `/var/lib/ilrdos-cloud/cloud.sqlite3`.
   - Optional `ILRDOS_CLOUD_BIND` and `ILRDOS_CLOUD_PORT` (defaults: `127.0.0.1:8765`).
3. Start `python3 cloud/server.py` behind HTTPS. `ilrdos-cloud.service.example` is a systemd starting point. Keep the database and its backups private.

`GET /healthz` is a liveness check. Administrative endpoints require the bearer token; the PIN alone cannot claim a robot.

## Install and pair a robot

Copy the `cloud/` directory onto the robot. The standard ILrDOS local daemon must already be installed and running. As the robot's service user, start pairing from an interactive terminal:

```sh
python3 /opt/ilrdos/cloud/agent.py pair \
  --url https://YOUR-CLOUD-HOST \
  --name living-room-robot \
  --state-dir /var/lib/ilrdos-cloud
```

After typing `AUTORIZO`, the terminal displays the temporary PIN and waits. On the admin host, load the admin secret from its secret manager into the process environment (do not put it in shell history), then claim the PIN:

```sh
python3 /opt/ilrdos/cloud/admin.py --url https://YOUR-CLOUD-HOST claim 12345678
```

The PIN is replaced above by the value shown locally. The robot receives its device credential after approval and writes it with mode `0600`. Then run the agent:

```sh
python3 /opt/ilrdos/cloud/agent.py run \
  --url https://YOUR-CLOUD-HOST \
  --state-dir /var/lib/ilrdos-cloud \
  --local-token-file /etc/ilrdos/token
```

Use `ilrdos-cloud-agent.service.example` as a systemd template once the robot has been paired. Keep the service environment file root-owned and mode `0600`; it contains the cloud URL, not the local daemon token.

## Admin operations

The admin CLI reads `ILRDOS_CLOUD_ADMIN_TOKEN` from the environment and never accepts the secret as a command-line argument:

```sh
python3 cloud/admin.py --url https://YOUR-CLOUD-HOST devices
python3 cloud/admin.py --url https://YOUR-CLOUD-HOST events DEVICE_ID
python3 cloud/admin.py --url https://YOUR-CLOUD-HOST send DEVICE_ID "status"
python3 cloud/admin.py --url https://YOUR-CLOUD-HOST send DEVICE_ID "move forward 1000 25"
python3 cloud/admin.py --url https://YOUR-CLOUD-HOST send DEVICE_ID "stop"
python3 cloud/admin.py --url https://YOUR-CLOUD-HOST revoke DEVICE_ID
```

The event log records pairing, state changes, command dispatch/results, and revocation. Heartbeats update `last_seen`; identical status lines are not duplicated as events, and the volatile uptime counter is removed. Events and finished commands are retained for 30 days. The service intentionally stores only the daemon's status line and bounded command results—not camera frames, face vectors, voice recordings, local tokens, or arbitrary user text.

## API outline

- `POST /v1/pairings`: robot creates a short-lived pending pairing.
- `POST /v1/pairings/claim`: admin approves a PIN.
- `POST /v1/pairings/{id}/poll` and `/complete`: robot retrieves and acknowledges its one-time credential.
- `GET /v1/admin/devices`, `GET /v1/admin/devices/{id}/events`: state and audit log.
- `POST /v1/admin/commands`: enqueue a validated command.
- `POST /v1/admin/devices/{id}/revoke`: revoke a device.
- Device-authenticated `heartbeat`, `commands/next`, and command `result` endpoints sync status and execution.

The current adapter targets the documented ILrDOS daemon protocol. Other robot models should be integrated through their documented, official local/cloud APIs with a dedicated adapter; this implementation does not reverse-engineer vendor protocols.

## Tests

From the repository root:

```sh
python3 -m unittest discover -s cloud/tests -v
```

#!/usr/bin/env python3
"""Small admin CLI; credentials are read only from the environment."""

from __future__ import annotations

import argparse
import json
import os
import sys

from agent import CloudError, api_request, validate_url


def main() -> int:
    parser = argparse.ArgumentParser(description="Administração do servidor ILrDOS Cloud")
    parser.add_argument("--url", required=True, help="URL HTTPS do serviço")
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("devices")
    events = sub.add_parser("events")
    events.add_argument("device_id")
    claim = sub.add_parser("claim")
    claim.add_argument("pin")
    send = sub.add_parser("send")
    send.add_argument("device_id")
    send.add_argument("command", help="status | stop | move ... | turn ...")
    revoke = sub.add_parser("revoke")
    revoke.add_argument("device_id")
    args = parser.parse_args()
    token = os.environ.get("ILRDOS_CLOUD_ADMIN_TOKEN", "")
    if len(token) < 32:
        print("Defina ILRDOS_CLOUD_ADMIN_TOKEN no gerenciador de segredos do host.", file=sys.stderr)
        return 2
    try:
        base_url = validate_url(args.url)
        if args.action == "devices":
            _, result = api_request(base_url, "/v1/admin/devices", token=token)
        elif args.action == "events":
            _, result = api_request(base_url, f"/v1/admin/devices/{args.device_id}/events", token=token)
        elif args.action == "claim":
            _, result = api_request(
                base_url, "/v1/pairings/claim", method="POST",
                payload={"code": args.pin}, token=token,
            )
        elif args.action == "send":
            _, result = api_request(
                base_url, "/v1/admin/commands", method="POST",
                payload={"device_id": args.device_id, "command": args.command}, token=token,
            )
        else:
            _, result = api_request(
                base_url, f"/v1/admin/devices/{args.device_id}/revoke",
                method="POST", payload={}, token=token,
            )
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except (CloudError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

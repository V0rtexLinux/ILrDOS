"""Small, explicit command allowlist shared by the cloud API and robot agent."""

from __future__ import annotations

import re


def validate_remote_command(command: object) -> str:
    """Return a canonical safe command, or raise ValueError."""
    if not isinstance(command, str) or len(command) > 64:
        raise ValueError("command must be a short string")
    parts = command.split()
    if not parts:
        raise ValueError("empty command")
    if parts == ["stop"] or parts == ["status"]:
        return parts[0]
    if len(parts) == 4 and parts[0] == "move" and parts[1] in {
        "forward", "back", "left", "right"
    }:
        if not re.fullmatch(r"[0-9]{1,4}", parts[2]) or not re.fullmatch(r"[0-9]{1,2}", parts[3]):
            raise ValueError("move requires integer duration and speed")
        duration, speed = int(parts[2]), int(parts[3])
        if not 100 <= duration <= 3000 or not 1 <= speed <= 35:
            raise ValueError("move is outside remote-control limits")
        if parts[1] == "back" and (duration > 1000 or speed > 30):
            raise ValueError("reverse movement is limited to 1000 ms and 30%")
        return f"move {parts[1]} {duration} {speed}"
    if len(parts) == 3 and parts[0] == "turn" and parts[1] in {"left", "right"}:
        if not re.fullmatch(r"[0-9]{1,3}", parts[2]) or not 1 <= int(parts[2]) <= 45:
            raise ValueError("turn must be between 1 and 45 degrees")
        return f"turn {parts[1]} {int(parts[2])}"
    raise ValueError("only status, stop, bounded move, and bounded turn are allowed")

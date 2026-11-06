#!/usr/bin/env python3
"""Claude Code hook: forwards session events to the e-ink bridge.

Wired up for SessionStart, UserPromptSubmit, PostToolUse, Notification, Stop
and SessionEnd. Reads the hook payload on stdin and fires a single datagram
at the bridge's socket. It must never block or fail a Claude session, so
every error is swallowed and it always exits 0.

With --statusline it reads Claude Code's status line input instead and
forwards only its rate_limits (5 h / 7 day usage), so the bridge needs no
usage API calls. It prints nothing; call it from your status line script:

    printf '%s' "$payload" | python3 -I ~/.local/share/claude-eink/eink_hook.py --statusline &
"""
import json
import os
import socket
import sys
import tempfile

SOCK = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir(), "claude-eink.sock")
KEEP = ("hook_event_name", "session_id", "cwd", "message", "notification_type")


def hook_event(payload):
    event = {k: payload[k] for k in KEEP if k in payload}
    # cwd follows `cd`s inside the session; the project dir names it better.
    if os.environ.get("CLAUDE_PROJECT_DIR"):
        event["cwd"] = os.environ["CLAUDE_PROJECT_DIR"]
    return event


def statusline_event(payload):
    rl = payload.get("rate_limits")
    if not isinstance(rl, dict):
        return None  # API-key login, or no API response yet this session
    windows = {k: rl[k] for k in ("five_hour", "seven_day") if isinstance(rl.get(k), dict)}
    return {"rate_limits": windows} if windows else None


def main():
    try:
        payload = json.load(sys.stdin)
        event = statusline_event(payload) if "--statusline" in sys.argv[1:] else hook_event(payload)
        if not event:
            return
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as s:
            s.setblocking(False)
            s.sendto(json.dumps(event).encode()[:4096], SOCK)
    except Exception:
        pass  # bridge not running, malformed payload, ... not our problem


if __name__ == "__main__":
    main()
    sys.exit(0)

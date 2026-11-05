#!/usr/bin/env python3
"""Claude Code hook: forwards session events to the e-ink bridge.

Wired up for SessionStart, UserPromptSubmit, PostToolUse, Notification, Stop
and SessionEnd. Reads the hook payload on stdin and fires a single datagram
at the bridge's socket. It must never block or fail a Claude session, so
every error is swallowed and it always exits 0.
"""
import json
import os
import socket
import sys
import tempfile

SOCK = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir(), "claude-eink.sock")
KEEP = ("hook_event_name", "session_id", "cwd", "message", "notification_type")


def main():
    try:
        payload = json.load(sys.stdin)
        event = {k: payload[k] for k in KEEP if k in payload}
        # cwd follows `cd`s inside the session; the project dir names it better.
        if os.environ.get("CLAUDE_PROJECT_DIR"):
            event["cwd"] = os.environ["CLAUDE_PROJECT_DIR"]
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as s:
            s.setblocking(False)
            s.sendto(json.dumps(event).encode()[:4096], SOCK)
    except Exception:
        pass  # bridge not running, malformed payload, ... not our problem


if __name__ == "__main__":
    main()
    sys.exit(0)

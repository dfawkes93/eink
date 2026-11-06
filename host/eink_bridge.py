#!/usr/bin/env python3
"""Feeds Claude usage and session state to the LilyGO e-ink display.

- Takes the 5 h / 7 day usage from Claude Code's status line (eink_hook.py
  --statusline forwards its rate_limits), which costs no API calls.
- Polls the Claude usage endpoint with the OAuth token Claude Code keeps in
  ~/.claude/.credentials.json (read-only: we never refresh or write it; Claude
  Code does that whenever it runs) for the per-product breakdown, and as a
  fallback when no status line feeds us. The endpoint is rate-limited, so this
  poll is slow, skipped while Claude is idle, and backs off on HTTP 429.
- Listens on a unix datagram socket for events from eink_hook.py.
- Pushes the combined state to the board over USB serial and/or BLE as
  newline-delimited JSON (protocol: firmware/src/main.cpp).
"""
import argparse
import asyncio
import collections
import datetime as dt
import email.utils
import json
import logging
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unicodedata
import urllib.error
import urllib.request
from pathlib import Path

log = logging.getLogger("eink")

CREDENTIALS = Path.home() / ".claude" / ".credentials.json"
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
# Must match eink_hook.py.
SOCK = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir(), "claude-eink.sock")
BOARD_USB_ID = (0x1A86, 0x55D4)  # the T5's CH9102 USB-serial chip
BLE_NAME = "Claude E-Ink"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

USAGE_EVERY = 300        # seconds between usage polls without a status line feed
USAGE_EVERY_LIVE = 1800  # ... and with one (then the poll is only for the breakdown)
LIVE_FRESH = 3600        # status line data this recent counts as a live feed
USAGE_MIN_GAP = 60       # floor for event-triggered polls
USAGE_BUTTON_GAP = 10    # floor when you ask for it with the button
BACKOFF_MIN = 300        # first wait after HTTP 429 without Retry-After, then doubles
BACKOFF_MAX = 6 * 3600
DND_LENGTH = 3600
PING_EVERY = 30
SESSION_TTL = 12 * 3600  # forget sessions we haven't heard from in this long
SESSION_WINDOW = dt.timedelta(hours=5)
WEEK_WINDOW = dt.timedelta(days=7)


def ascii_only(s, limit):
    """The board's fonts are ASCII-only."""
    s = s.replace("—", "-").replace("–", "-").replace("…", "...")
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()
    return " ".join(s.split())[:limit]


def hhmm(t):
    return t.astimezone().strftime("%H:%M")


# --- usage -------------------------------------------------------------------

class UsageError(Exception):
    pass


class RateLimited(UsageError):
    def __init__(self, msg, retry_after=None):
        super().__init__(msg)
        self.retry_after = retry_after  # seconds, if the server said


def retry_after_seconds(value):
    """Retry-After is either delta-seconds or an HTTP date."""
    if not value:
        return None
    try:
        return max(0, int(value))
    except ValueError:
        pass
    try:
        when = email.utils.parsedate_to_datetime(value)
        return max(0, int((when - dt.datetime.now(dt.timezone.utc)).total_seconds()))
    except (TypeError, ValueError):
        return None


def read_credentials():
    """Claude Code keeps its login in a file on Linux and in the Keychain on macOS."""
    try:
        return json.loads(CREDENTIALS.read_text())["claudeAiOauth"]
    except (OSError, ValueError, KeyError):
        pass
    if sys.platform == "darwin":
        try:
            out = subprocess.run(
                ["security", "find-generic-password", "-s", "Claude Code-credentials", "-w"],
                capture_output=True, text=True, timeout=10).stdout
            return json.loads(out)["claudeAiOauth"]
        except (OSError, subprocess.SubprocessError, ValueError, KeyError):
            pass
    raise UsageError("no Claude login found")


def fetch_usage():
    creds = read_credentials()
    if creds.get("expiresAt", 0) / 1000 < time.time():
        raise UsageError("token expired - run claude to refresh")
    req = urllib.request.Request(USAGE_URL, headers={
        "Authorization": "Bearer " + creds["accessToken"],
        "anthropic-beta": "oauth-2025-04-20",
        "User-Agent": "claude-eink-bridge",
    })
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        if e.code in (429, 503):
            raise RateLimited(f"usage API HTTP {e.code}",
                              retry_after_seconds(e.headers.get("Retry-After"))) from e
        raise UsageError(f"usage API HTTP {e.code}") from e
    except (urllib.error.URLError, TimeoutError, ValueError) as e:
        raise UsageError("usage API unreachable") from e


def describe_window(w, length, now):
    """Turns one usage window into what the board shows, deliberately coarse
    so the panel doesn't need redrawing every minute."""
    if not w or w.get("utilization") is None:
        return None
    out = {"pct": round(w["utilization"])}
    if w.get("resets_at"):
        reset = dt.datetime.fromisoformat(w["resets_at"])
        left = reset - now
        if left.total_seconds() <= 0:
            # Polls stop while Claude is idle, so the data can outlive its
            # window. A window that has ended has nothing used in it.
            return {"pct": 0, "reset": "-- window ended"}
        local = reset.astimezone()
        same_day = local.date() == now.astimezone().date()
        out["reset"] = local.strftime("%H:%M" if same_day else "%a %H:%M")
        mins = max(0, int(left.total_seconds() // 60))
        if mins >= 24 * 60:
            out["reset_in"] = f"{mins // 1440}d {mins % 1440 // 60}h"
        elif mins >= 60:
            out["reset_in"] = f"{mins // 60}h{mins % 60 // 10 * 10:02d}m"
        else:
            out["reset_in"] = f"{mins // 5 * 5}m"
        elapsed = 1 - left / length
        out["elapsed"] = max(0, min(100, round(elapsed * 20) * 5))
    return out


# --- sessions ----------------------------------------------------------------

class Sessions:
    def __init__(self):
        self.by_id = {}
        self.alert_seq = 0
        self.acked = set()   # session ids whose current wait was dismissed
        self.last_event = 0.0  # usage can only move when Claude is in use

    def handle(self, ev):
        """Applies one hook event. Returns True if a usage refresh is worth doing."""
        sid = ev.get("session_id")
        name = ev.get("hook_event_name")
        if not sid or not name:
            return False
        now = time.time()
        self.last_event = now
        if name == "SessionEnd":
            self.by_id.pop(sid, None)
            self.acked.discard(sid)
            return True
        s = self.by_id.setdefault(sid, {"state": "idle", "msg": "", "since": now})
        s["cwd"] = ev.get("cwd") or s.get("cwd", "")
        s["seen"] = now

        def set_state(state, msg=""):
            if state == "waiting" and s["state"] != "waiting":
                self.alert_seq += 1
                self.acked.discard(sid)
            if state != s["state"]:
                s["since"] = now
            s["state"], s["msg"] = state, msg

        if name == "SessionStart":
            set_state("idle", "Started")
        elif name in ("UserPromptSubmit", "PostToolUse"):
            set_state("working")
        elif name == "Notification":
            kind = ev.get("notification_type", "")
            if kind == "auth_success":
                return False
            msg = ev.get("message") or "Waiting for your input"
            # idle_prompt fires ~60 s after a turn ends with no reply from you.
            set_state("waiting", msg)
        elif name == "Stop":
            set_state("idle", "Finished")
            return True
        return False

    def dismiss(self):
        self.acked |= {sid for sid, s in self.by_id.items() if s["state"] == "waiting"}

    def prune(self):
        cutoff = time.time() - SESSION_TTL
        for sid in [k for k, s in self.by_id.items() if s["seen"] < cutoff]:
            del self.by_id[sid]
            self.acked.discard(sid)

    def for_display(self):
        order = {"waiting": 0, "working": 1, "idle": 2}
        items = sorted(self.by_id.items(), key=lambda kv: (order[kv[1]["state"]], -kv[1]["since"]))
        names = [os.path.basename(s["cwd"].rstrip("/")) or "?" for _, s in items]
        out = []
        for (sid, s), n in zip(items, names):
            if names.count(n) > 1:
                n = f"{n} ({sid[:4]})"
            out.append({
                "name": ascii_only(n, 31),
                "state": s["state"],
                "msg": ascii_only(s["msg"], 95),
                "since": time.strftime("%H:%M", time.localtime(s["since"])),
            })
        alert = any(s["state"] == "waiting" and sid not in self.acked for sid, s in self.by_id.items())
        return out, alert


# --- transports --------------------------------------------------------------

class Transport:
    name = "?"
    connected = False

    def __init__(self, on_line):
        self.on_line = on_line

    async def send(self, line):
        raise NotImplementedError


class SerialTransport(Transport):
    name = "usb"

    def __init__(self, on_line, port):
        super().__init__(on_line)
        self.port = port
        self.ser = None

    def candidates(self):
        if self.port:
            return [self.port]
        from serial.tools import list_ports
        return sorted(p.device for p in list_ports.comports() if (p.vid, p.pid) == BOARD_USB_ID)

    async def run(self):
        import serial
        loop = asyncio.get_running_loop()
        attempt = 0
        while True:
            ports = self.candidates()
            if not ports:
                await asyncio.sleep(5)
                continue
            port = ports[attempt % len(ports)]
            attempt += 1
            try:
                self.ser = serial.Serial(port, 115200, timeout=1)
                await self.talk(loop, port, many=len(ports) > 1)
            except (OSError, serial.SerialException) as e:
                if self.connected:
                    log.info("serial: disconnected (%s)", e)
            finally:
                self.connected = False
                if self.ser:
                    self.ser.close()
                    self.ser = None
            await asyncio.sleep(2)

    async def talk(self, loop, port, many):
        # Other ESP32 boards use the same USB chip, so only treat the port as
        # ours once the firmware answers. A board in battery saver only
        # answers when it next wakes (up to 2 min), so keep listening.
        buf = b""
        opened = last_ping = 0.0
        while True:
            now = time.monotonic()
            if not self.connected:
                if opened == 0:
                    opened = now
                elif many and now - opened > 150:
                    return  # try the next candidate
                if now - last_ping > 30:
                    self.ser.write(b'{"t":"ping"}\n')
                    last_ping = now
            # Block for the first byte only, then take whatever else is there.
            buf += await loop.run_in_executor(
                None, lambda: self.ser.read(max(1, self.ser.in_waiting)))
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip()
                if not self.connected:
                    try:
                        if json.loads(line).get("t") not in ("pong", "hello"):
                            continue
                    except (ValueError, AttributeError):
                        continue
                    self.connected = True
                    log.info("serial: found the board on %s", port)
                self.on_line(self, line)

    async def send(self, line):
        if self.ser:
            try:
                self.ser.write(line.encode() + b"\n")
            except (OSError, Exception) as e:
                log.info("serial: write failed (%s)", e)


def drop_stale_ble_link():
    """If a previous bridge died without disconnecting, BlueZ keeps the link up
    and the board stops advertising, so scans can't find it. Drop that link."""
    try:
        out = subprocess.run(["bluetoothctl", "devices", "Connected"],
                             capture_output=True, text=True, timeout=10).stdout
        for line in out.splitlines():
            parts = line.split(" ", 2)
            if len(parts) == 3 and parts[2] == BLE_NAME:
                log.info("ble: dropping stale link to %s", parts[1])
                subprocess.run(["bluetoothctl", "disconnect", parts[1]],
                               capture_output=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        pass


class BleTransport(Transport):
    name = "ble"

    def __init__(self, on_line):
        super().__init__(on_line)
        self.client = None
        self.lock = asyncio.Lock()

    async def run(self):
        from bleak import BleakClient, BleakScanner
        from bleak.exc import BleakError
        while True:
            try:
                # Scan continuously: in battery saver the board is only awake
                # for a few seconds every couple of minutes.
                dev = await BleakScanner.find_device_by_name(BLE_NAME, timeout=15)
                if not dev:
                    drop_stale_ble_link()
                    continue
                disconnected = asyncio.Event()
                # BlueZ needs asking to pair; macOS pairs on its own.
                async with BleakClient(dev, pair=sys.platform == "linux",
                                       disconnected_callback=lambda _: disconnected.set()) as client:
                    buf = bytearray()

                    def on_notify(_, data):
                        buf.extend(data)
                        while b"\n" in buf:
                            i = buf.index(b"\n")
                            self.on_line(self, buf[:i].decode("utf-8", "replace").strip())
                            del buf[:i + 1]

                    if hasattr(client._backend, "_acquire_mtu"):
                        # BlueZ only reports the negotiated MTU when asked.
                        await client._backend._acquire_mtu()
                    await client.start_notify(NUS_TX, on_notify)
                    self.client = client
                    self.connected = True
                    log.info("ble: connected to %s (mtu %d)", dev.address, client.mtu_size)
                    # The board's own hello went out before we subscribed.
                    self.on_line(self, '{"t":"hello","synthetic":true}')
                    await disconnected.wait()
            except (BleakError, OSError, asyncio.TimeoutError) as e:
                log.info("ble: %s", e)
            finally:
                if self.connected:
                    log.info("ble: disconnected")
                self.client = None
                self.connected = False
            await asyncio.sleep(1)

    async def send(self, line):
        client = self.client
        if not client:
            return
        data = (line + "\n").encode()
        size = max(20, client.mtu_size - 3)
        async with self.lock:
            try:
                for i in range(0, len(data), size):
                    await client.write_gatt_char(NUS_RX, data[i:i + size], response=True)
            except Exception as e:
                log.info("ble: write failed (%s)", e)


# --- bridge ------------------------------------------------------------------

class Bridge:
    def __init__(self, args):
        self.sessions = Sessions()
        self.usage = None
        self.usage_at = None
        self.usage_error = ""
        self.last_fetch = 0.0
        self.last_check = 0.0
        self.live_at = 0.0       # last status line rate_limits we took
        self.backoff = 0         # current 429 backoff step, seconds
        self.backoff_until = 0.0
        self.refresh_soon = asyncio.Event()
        self.refresh_gap = USAGE_MIN_GAP
        self.dnd_until = 0.0
        self.last_sent = {}
        self.seen_events = collections.deque(maxlen=32)
        self.transports = []
        if not args.no_serial:
            self.transports.append(SerialTransport(self.on_line, args.port))
        if not args.no_ble:
            self.transports.append(BleTransport(self.on_line))

    def on_line(self, transport, line):
        log.debug("%s <- %s", transport.name, line)
        if not line.startswith("{"):
            return  # boot log noise
        try:
            msg = json.loads(line)
        except ValueError:
            return
        t = msg.get("t")
        if "id" in msg:
            # Board events arrive once per link; act on the first copy only.
            if msg["id"] in self.seen_events:
                return
            self.seen_events.append(msg["id"])
        if t == "hello":
            if not msg.get("synthetic"):
                log.info("%s: board said hello (fw %s, battery %s V%s)", transport.name,
                         msg.get("fw"), msg.get("battery"), ", saver" if msg.get("saver") else "")
            self.last_sent.pop(transport.name, None)
            asyncio.ensure_future(self.push())
        elif t == "btn":
            log.info("button: GPIO%s %s press", msg.get("pin"), msg.get("press", "short"))
        elif t == "cmd":
            self.command(msg.get("do"))
        elif t == "dismiss":
            log.info("dismiss pressed")
            self.sessions.dismiss()
            asyncio.ensure_future(self.push())

    def command(self, what):
        if what == "refresh_usage":
            if self.backoff_until > time.time():
                log.info("refresh asked; the usage API is rate-limited, refreshing at %s",
                         time.strftime("%H:%M", time.localtime(self.backoff_until)))
            else:
                log.info("refreshing usage (button)")
            self.refresh_gap = USAGE_BUTTON_GAP
            self.refresh_soon.set()
        elif what == "dnd":
            self.dnd_until = 0.0 if self.dnd_until > time.time() else time.time() + DND_LENGTH
            log.info("do not disturb %s", "on for 1 h" if self.dnd_until else "off")
            asyncio.ensure_future(self.push())

    def state(self):
        now = dt.datetime.now(dt.timezone.utc)
        sessions, alert = self.sessions.for_display()
        # Do not disturb: sessions still show as waiting, but no banner or blink.
        dnd = self.dnd_until > time.time()
        alert = alert and not dnd
        st = {
            "t": "state",
            "now": hhmm(now),
            "usage_at": hhmm(self.usage_at) if self.usage_at else "",
            "error": ascii_only(self.usage_error, 63),
            "sessions": sessions,
            "alert": alert,
            "alert_seq": self.sessions.alert_seq,
            "dnd": time.strftime("%H:%M", time.localtime(self.dnd_until)) if dnd else "",
        }
        if self.usage:
            st["session"] = describe_window(self.usage.get("five_hour"), SESSION_WINDOW, now)
            st["week"] = describe_window(self.usage.get("seven_day"), WEEK_WINDOW, now)
            rows = (self.usage.get("seven_day_breakdown") or {}).get("rows") or []
            st["breakdown"] = [{"name": ascii_only(r.get("display_name", "?"), 19), "pct": r.get("percent", 0)}
                               for r in rows][:4]
        return st

    async def push(self):
        st = self.state()
        # "now" changes every minute; only resend when something real changed.
        key = json.dumps({k: v for k, v in st.items() if k != "now"}, sort_keys=True)
        line = json.dumps(st, separators=(",", ":"))
        for tr in self.transports:
            if tr.connected and self.last_sent.get(tr.name) != key:
                await tr.send(line)
                self.last_sent[tr.name] = key

    def live_fresh(self):
        return time.time() - self.live_at < LIVE_FRESH

    def apply_live(self, rl):
        """Takes the rate_limits object from Claude Code's status line input."""
        if not isinstance(rl, dict):
            return
        windows = {}
        for key in ("five_hour", "seven_day"):
            w = rl.get(key)
            if not isinstance(w, dict):
                continue  # Claude Code drops a window once it resets
            try:
                pct = float(w["used_percentage"])
                reset = w.get("resets_at")
                reset = dt.datetime.fromtimestamp(float(reset), dt.timezone.utc).isoformat() if reset else None
            except (KeyError, TypeError, ValueError, OverflowError, OSError):
                continue
            windows[key] = {"utilization": pct, "resets_at": reset}
        if not windows:
            return
        if not self.live_fresh():
            log.info("usage: status line feed live, polling the API every %d min", USAGE_EVERY_LIVE // 60)
        old = self.usage or {}
        changed = any(old.get(k) != v for k, v in windows.items())
        self.usage = {**old, **windows}
        self.live_at = time.time()
        now = dt.datetime.now(dt.timezone.utc)
        # The status line runs many times a minute; moving usage_at on each
        # one would redraw the panel for nothing.
        if changed or not self.usage_at or (now - self.usage_at).total_seconds() >= USAGE_EVERY:
            self.usage_at = now
            self.usage_error = ""

    async def fetch_once(self, loop):
        self.last_fetch = time.time()
        try:
            usage = await loop.run_in_executor(None, fetch_usage)
        except RateLimited as e:
            self.backoff = min(BACKOFF_MAX, max(BACKOFF_MIN, self.backoff * 2))
            wait = e.retry_after if e.retry_after is not None else self.backoff
            wait = max(USAGE_MIN_GAP, min(BACKOFF_MAX, wait))
            self.backoff_until = time.time() + wait
            until = time.strftime("%H:%M", time.localtime(self.backoff_until))
            log.warning("usage: %s, backing off until %s", e, until)
            self.usage_error = f"{e}, retry {until}"
            return
        except UsageError as e:
            log.warning("usage: %s", e)
            self.usage_error = str(e)
            return
        self.backoff = 0
        self.usage = usage
        self.usage_at = dt.datetime.now(dt.timezone.utc)
        self.usage_error = ""

    async def poll_usage(self):
        loop = asyncio.get_running_loop()
        asked = True  # always fetch once at start-up
        while True:
            self.last_check = time.time()
            if self.last_check + 0.5 < self.backoff_until:
                pass  # rate-limited: wait it out, keeping any request pending
            elif asked or self.usage is None or self.sessions.last_event > self.last_fetch:
                await self.fetch_once(loop)
                asked = False
            else:
                log.debug("usage: Claude idle since the last poll, skipping")
            await self.push()
            self.refresh_soon.clear()
            every = USAGE_EVERY_LIVE if self.live_fresh() else USAGE_EVERY
            wake = max(self.last_check + every, self.backoff_until)
            try:
                await asyncio.wait_for(self.refresh_soon.wait(), max(1, wake - time.time()))
                asked = True
                earliest = max(self.last_fetch + self.refresh_gap, self.backoff_until)
                self.refresh_gap = USAGE_MIN_GAP
                await asyncio.sleep(max(0, earliest - time.time()))
            except asyncio.TimeoutError:
                pass

    async def listen_hooks(self):
        loop = asyncio.get_running_loop()
        if os.path.exists(SOCK):
            os.unlink(SOCK)
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        sock.bind(SOCK)
        os.chmod(SOCK, 0o600)
        sock.setblocking(False)
        while True:
            data = await loop.sock_recv(sock, 8192)
            try:
                ev = json.loads(data)
            except ValueError:
                continue
            if not isinstance(ev, dict):
                continue
            if "rate_limits" in ev:
                self.apply_live(ev["rate_limits"])
            else:
                log.debug("hook: %s", ev)
                # With a live status line feed the numbers are already fresh.
                if self.sessions.handle(ev) and not self.live_fresh():
                    self.refresh_soon.set()
            await self.push()

    async def tick(self):
        while True:
            await asyncio.sleep(PING_EVERY)
            self.sessions.prune()
            await self.push()  # reset countdowns tick over now and then
            for tr in self.transports:
                if tr.connected:
                    await tr.send('{"t":"ping"}')

    async def run(self):
        await asyncio.gather(self.listen_hooks(), self.poll_usage(), self.tick(),
                             *(tr.run() for tr in self.transports))


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--port", help="serial port (default: find the board by USB ID)")
    p.add_argument("--no-serial", action="store_true")
    p.add_argument("--no-ble", action="store_true")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("bleak").setLevel(logging.WARNING)
    asyncio.run(run(args))


async def run(args):
    # Exit via task cancellation on SIGTERM/SIGINT so the BLE client's context
    # manager gets to disconnect cleanly.
    task = asyncio.current_task()
    for sig in (signal.SIGTERM, signal.SIGINT):
        asyncio.get_running_loop().add_signal_handler(sig, task.cancel)
    try:
        await Bridge(args).run()
    except asyncio.CancelledError:
        log.info("shutting down")


if __name__ == "__main__":
    main()

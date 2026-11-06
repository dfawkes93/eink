# Claude e-ink display

Shows Claude session (5 h) and weekly usage on a LilyGO T5 4.7" e-paper board
(original ESP32, CH9102 USB, 18650 holder), and lights up a "NEEDS INPUT"
banner - with a full-panel blink - when a Claude Code session is waiting on you.

```
Claude Code hooks       --datagram--> host/eink_bridge.py --USB serial / BLE--> board
Claude Code status line --datagram--^
usage endpoint          --HTTPS-----^
```

- `firmware/` - PlatformIO project for the board. `pio run -t upload` from there.
- `host/eink_bridge.py` - takes 5 h / weekly usage from the status line feed,
  polls the usage endpoint (with the OAuth token Claude Code keeps in
  `~/.claude/.credentials.json`; read-only) for the per-product breakdown,
  tracks sessions from hook events, pushes state to the board over serial and BLE.
- `host/eink_hook.py` - the Claude Code hook. Registered by `install.sh` in
  `~/.claude/settings.json` for SessionStart, UserPromptSubmit, PostToolUse,
  Notification, Stop and SessionEnd. Async, and silent if the bridge is down.
  With `--statusline` it forwards the status line's `rate_limits` instead.

## Usage data and rate limits

The usage endpoint is rate-limited, so the bridge calls it as little as it can:

- **Status line feed (preferred).** Claude Code gives its status line command
  the 5 h and weekly usage (`rate_limits`, Pro/Max plans) at no API cost. Add
  this line to your status line script, after it reads stdin into `$payload`:

  ```sh
  printf '%s' "$payload" | python3 -I ~/.local/share/claude-eink/eink_hook.py --statusline >/dev/null 2>&1 &
  ```

  While this feed is live (data in the last hour) the bridge polls the endpoint
  only every 30 min, for the per-product breakdown.
- **Without the feed** the bridge polls every 5 min, plus after a turn ends
  (at most once a minute).
- **Either way** it skips a poll when no Claude session did anything since the
  last one, and on HTTP 429/503 it waits for `Retry-After` or backs off
  (5 min, doubling to 6 h). The BOOT button doesn't skip the backoff.
- `probe/` - hardware probe sketch (chip/flash/PSRAM report).

## Setting up a computer

```sh
./install.sh              # install or update (Linux or macOS)
./install.sh --uninstall  # remove it again
```

This copies the bridge and hook to `~/.local/share/claude-eink` (with its own
venv), registers the hooks in `~/.claude/settings.json` (backup kept as
`settings.json.bak-claude-eink`), and runs the bridge as a login service:
a systemd user unit on Linux (`journalctl --user -u claude-eink -f`), a
launchd agent on macOS (`~/Library/Logs/claude-eink.log`). Re-run it after
changing anything in `host/`.

The bridge finds the board itself: over USB by its CH9102 ID (it only claims
a port once the firmware answers), over BLE by name. On Linux you need to be
in the `uucp` or `dialout` group for USB; the installer offers to add you.
On macOS, Claude Code's login comes from the Keychain - allow Python access
when asked. Windows isn't supported (the hook uses unix sockets).

For debugging, run it in the foreground instead of the service:
`~/.local/share/claude-eink/venv/bin/python host/eink_bridge.py -v`
(flags: `--no-serial`, `--no-ble`, `--port PATH`).

## Board

Buttons, left to right as they sit on the board (hold = about 1 s):

| Button | Press | Hold |
|---|---|---|
| RST | restart (hardware) | |
| BOOT (GPIO0) | refresh usage now | do not disturb for 1 h, on/off |
| GPIO35 | switch Overview / Details page | **power off** (Off screen; 35 or 34 turns it on) |
| GPIO34 | dismiss the alert | |
| GPIO39 | full refresh (clears ghosting) | battery saver now (until USB is back) |

Only 35 and 34 can wake the board from battery saver or off. BOOT's pin is
also the display's strobe, so BOOT acts on release and screen updates wait
while it's held. Every press is reported to the bridge (`button: GPIO.. press`
in its log), ready for PC-side bindings.

Session states: filled square = waiting for you, grey = working, outline = idle.
The tick on each usage bar marks how far through the window you are; fill past
the tick means you're on pace to hit the limit before it resets.

## Battery

Fit an 18650 Li-ion cell (3.6/3.7 V, not LiFePO4) in the holder; USB charges
it. PY2 is an alternative LiPo input in parallel with the holder - don't use
both.

| Power | Behaviour |
|---|---|
| USB with the bridge talking over serial | always on, instant alerts |
| Battery above ~1/3 (3.70 V) | always on over BLE, instant alerts, battery % in header |
| Battery at/below ~1/3 | **battery saver**: deep sleep, wakes every 2 min to sync over BLE (~3 s awake); dismiss/page buttons wake it at once |

Saver ends when USB serial is back or the cell reads >= 3.80 V. The board keeps
the last state in RTC memory so a wake only redraws what changed. For testing,
sending `{"t":"saver"}` over serial forces saver mode until serial is heard
again.

BLE uses the Nordic UART service, encrypted with Just Works pairing; BlueZ
pairs automatically on first connect.

No firmware-controllable LEDs on this board (all are wired to power/charging).
GPIO 13/14/15 on the 4-pin headers are free for an external LED or WS2812 strip;
avoid GPIO12 (boot strapping pin).

The usage endpoint (`/api/oauth/usage`) is undocumented and may change. The
status line `rate_limits` field is documented at
<https://code.claude.com/docs/en/statusline>.

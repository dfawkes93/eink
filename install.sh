#!/usr/bin/env bash
# Sets up this computer to drive the Claude e-ink display:
#   - installs the bridge + hook into ~/.local/share/claude-eink (own venv)
#   - registers the Claude Code hooks in ~/.claude/settings.json
#   - runs the bridge as a login service (systemd user unit / launchd agent)
#
#   ./install.sh              install or update
#   ./install.sh --uninstall  remove everything it added
#
# Linux and macOS. The board itself needs no changes: plug it in over USB,
# or let it pair over Bluetooth on its own.
set -euo pipefail

PREFIX="$HOME/.local/share/claude-eink"
SETTINGS="$HOME/.claude/settings.json"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/host"
UNIT="$HOME/.config/systemd/user/claude-eink.service"
PLIST="$HOME/Library/LaunchAgents/com.claude-eink.bridge.plist"
LABEL="com.claude-eink.bridge"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

case "$(uname -s)" in
  Linux) OS=linux ;;
  Darwin) OS=mac ;;
  *) die "only Linux and macOS are supported" ;;
esac

# Adds (or with "remove", strips) our hooks in Claude Code's settings,
# leaving everything else in the file alone.
edit_hooks() {
  local mode=$1 python=$2
  [[ -f "$SETTINGS" || $mode == add ]] || return 0
  mkdir -p "$(dirname "$SETTINGS")"
  [[ -f "$SETTINGS" ]] && cp "$SETTINGS" "$SETTINGS.bak-claude-eink"
  "$python" -I - "$SETTINGS" "$mode" "$PREFIX" <<'EOF'
import json, os, shlex, sys
path, mode, prefix = sys.argv[1:]
settings = json.load(open(path)) if os.path.exists(path) else {}
hooks = settings.get("hooks", {})
# Drop any earlier eink hooks, wherever they pointed, so re-runs don't stack.
for event in list(hooks):
    for group in hooks[event]:
        group["hooks"] = [h for h in group.get("hooks", []) if "eink_hook.py" not in h.get("command", "")]
    hooks[event] = [g for g in hooks[event] if g.get("hooks")]
    if not hooks[event]:
        del hooks[event]
if mode == "add":
    cmd = "%s -I %s" % (shlex.quote(prefix + "/venv/bin/python"), shlex.quote(prefix + "/eink_hook.py"))
    for event in ("SessionStart", "UserPromptSubmit", "PostToolUse", "Notification", "Stop", "SessionEnd"):
        hooks.setdefault(event, []).append(
            {"hooks": [{"type": "command", "command": cmd, "async": True, "timeout": 5}]})
if hooks:
    settings["hooks"] = hooks
else:
    settings.pop("hooks", None)
with open(path + ".tmp", "w") as f:
    json.dump(settings, f, indent=2, ensure_ascii=False)
    f.write("\n")
os.replace(path + ".tmp", path)
EOF
}

stop_service() {
  if [[ $OS == linux ]]; then
    systemctl --user disable --now claude-eink.service 2>/dev/null || true
  else
    launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
  fi
}

uninstall() {
  say "Stopping the bridge"
  stop_service
  rm -f "$UNIT" "$PLIST"
  [[ $OS == linux ]] && systemctl --user daemon-reload || true
  say "Removing Claude Code hooks"
  edit_hooks remove "$(command -v python3)"
  rm -rf "$PREFIX"
  say "Uninstalled. (A backup of your settings is at $SETTINGS.bak-claude-eink)"
}

[[ ${1:-} == --uninstall ]] && { uninstall; exit 0; }
[[ -n ${1:-} ]] && die "unknown option: $1 (try --uninstall)"

# --- checks ------------------------------------------------------------------

command -v python3 >/dev/null || die "python3 is required"
python3 -c 'import sys; sys.exit(sys.version_info < (3, 9))' || die "python 3.9 or newer is required"
python3 -c 'import venv, ensurepip' 2>/dev/null ||
  die "python3 venv support is missing (Debian/Ubuntu: sudo apt install python3-venv)"
[[ -f "$SRC/eink_bridge.py" && -f "$SRC/eink_hook.py" ]] || die "run this from the eink repo checkout"

if [[ $OS == linux ]]; then
  command -v systemctl >/dev/null || die "systemd is required on Linux"
  # Serial ports belong to uucp (Arch) or dialout (Debian/Fedora).
  serial_group=""
  for g in uucp dialout; do getent group "$g" >/dev/null && { serial_group=$g; break; }; done
  in_group() { id -nG "$@" | tr ' ' '\n' | grep -qx "$serial_group"; }
  if [[ -n $serial_group ]] && ! in_group "$USER"; then
    warn "you're not in the '$serial_group' group, so the bridge can't open the USB port."
    read -rp "    Add yourself now with sudo? [y/N] " ans || ans=""
    if [[ $ans == [yY]* ]]; then
      sudo usermod -aG "$serial_group" "$USER"
      warn "log out and back in for the group to apply; until then only Bluetooth works."
    fi
  elif [[ -n $serial_group ]] && ! in_group; then
    warn "you were added to '$serial_group' but haven't logged in again since;"
    warn "until you do, the bridge can only use Bluetooth."
  fi
  systemctl is-active --quiet bluetooth 2>/dev/null ||
    warn "the bluetooth service isn't running - only USB will work"
fi

if [[ ! -f "$HOME/.claude/.credentials.json" && $OS == linux ]]; then
  warn "no Claude login found - run 'claude' and log in, or usage won't show"
fi

# --- install -----------------------------------------------------------------

say "Installing bridge into $PREFIX"
stop_service
mkdir -p "$PREFIX"
cp "$SRC/eink_bridge.py" "$SRC/eink_hook.py" "$SRC/requirements.txt" "$PREFIX/"
[[ -x "$PREFIX/venv/bin/python" ]] || python3 -m venv "$PREFIX/venv"
"$PREFIX/venv/bin/pip" install --quiet --disable-pip-version-check -r "$PREFIX/requirements.txt"

say "Registering Claude Code hooks in $SETTINGS"
edit_hooks add "$PREFIX/venv/bin/python"

say "Starting the bridge as a login service"
if [[ $OS == linux ]]; then
  mkdir -p "$(dirname "$UNIT")"
  cat >"$UNIT" <<EOF
[Unit]
Description=Claude usage / input alerts on the LilyGO e-ink display
After=bluetooth.target

[Service]
ExecStart="$PREFIX/venv/bin/python" "$PREFIX/eink_bridge.py"
Restart=on-failure
RestartSec=10

[Install]
WantedBy=default.target
EOF
  systemctl --user daemon-reload
  systemctl --user enable --now claude-eink.service
  logs="journalctl --user -u claude-eink -f"
else
  mkdir -p "$(dirname "$PLIST")" "$HOME/Library/Logs"
  cat >"$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$PREFIX/venv/bin/python</string>
    <string>$PREFIX/eink_bridge.py</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$HOME/Library/Logs/claude-eink.log</string>
  <key>StandardErrorPath</key><string>$HOME/Library/Logs/claude-eink.log</string>
</dict>
</plist>
EOF
  launchctl bootstrap "gui/$(id -u)" "$PLIST"
  logs="tail -f ~/Library/Logs/claude-eink.log"
  warn "macOS may ask to allow Bluetooth and Keychain access for Python - allow both."
fi

# The status line is the cheap usage source; we don't own it, so only check.
statusline_feeds_us() {
  python3 - "$SETTINGS" <<'PY'
import json, os, shlex, sys
try:
    cmd = json.load(open(sys.argv[1])).get("statusLine", {}).get("command", "")
except (OSError, ValueError, AttributeError):
    sys.exit(1)
if "eink_hook.py" in cmd:
    sys.exit(0)
try:
    script = os.path.expanduser(shlex.split(cmd)[0])
    sys.exit(0 if "eink_hook.py" in open(script, errors="ignore").read() else 1)
except (OSError, ValueError, IndexError):
    sys.exit(1)
PY
}
if ! statusline_feeds_us; then
  warn "your Claude Code status line doesn't feed the bridge, so it polls the rate-limited"
  warn "usage API every 5 min while Claude is in use. To avoid that, add to your status line script:"
  echo "    printf '%s' \"\$payload\" | python3 -I $PREFIX/eink_hook.py --statusline >/dev/null 2>&1 &"
fi

say "Done. Plug the board in over USB or switch it on nearby; it shows up within a minute."
echo "    Logs:      $logs"
echo "    Uninstall: $0 --uninstall"

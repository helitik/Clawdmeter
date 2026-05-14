#!/bin/bash
# Claude Code Stop hook → Clawdmeter celebration animation.
#
# Wires up to ~/.claude/settings.json like this:
#
#   {
#     "hooks": {
#       "Stop": [
#         {
#           "matcher": "*",
#           "hooks": [
#             { "type": "command",
#               "command": "/abs/path/to/Clawdmeter/daemon/hooks/claude-stop.sh" }
#           ]
#         }
#       ]
#     }
#   }
#
# Signals the running daemon via SIGUSR1; the daemon then writes a tiny
# {"e":"done"} payload over BLE and the firmware plays a random energetic
# splash animation for ~6s. No-op if the daemon isn't running.

PID_FILE="$HOME/.config/claude-usage-monitor/daemon.pid"
[ -f "$PID_FILE" ] || exit 0
PID=$(cat "$PID_FILE" 2>/dev/null)
[ -n "$PID" ] || exit 0
kill -USR1 "$PID" 2>/dev/null || true
exit 0

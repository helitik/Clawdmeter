#!/bin/bash
# Claude Code Stop hook → Clawdmeter celebration animation + context tokens.
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
# Two side-effects:
#  1. Read the session's transcript_path (passed by Claude Code on stdin),
#     extract the latest assistant message's token usage (sum of plain
#     input + cache_creation + cache_read), and write it to a flag file
#     the daemon picks up at signal-handling time.
#  2. Send SIGUSR1 to the running daemon so it pushes a `{"e":"done","c":N}`
#     event over BLE; the firmware plays a random energetic splash and
#     updates the context-tokens indicator. No-op if the daemon isn't
#     running or stdin doesn't contain a transcript path.

CONTEXT_FILE="/tmp/clawdmeter-context.txt"
PID_FILE="$HOME/.config/claude-usage-monitor/daemon.pid"

# --- 1. Compute context tokens from the session transcript -----------------
# Claude Code feeds us a JSON object on stdin. The transcript_path field
# points to a JSONL file where every assistant turn has a `.message.usage`
# block. Sum the three input-side counters of the LAST assistant turn to
# get the context size the model saw at that point.
INPUT=$(cat 2>/dev/null)
if [ -n "$INPUT" ] && command -v jq >/dev/null 2>&1; then
    TRANSCRIPT=$(echo "$INPUT" | jq -r '.transcript_path // empty' 2>/dev/null)
    if [ -n "$TRANSCRIPT" ] && [ -f "$TRANSCRIPT" ]; then
        TOKENS=$(jq -s '
            map(select(.type == "assistant" and (.message.usage // null) != null))
            | last
            | (.message.usage as $u
               | (($u.input_tokens // 0)
                  + ($u.cache_creation_input_tokens // 0)
                  + ($u.cache_read_input_tokens // 0)))
        ' "$TRANSCRIPT" 2>/dev/null)
        if [ -n "$TOKENS" ] && [ "$TOKENS" != "null" ] && [ "$TOKENS" -gt 0 ] 2>/dev/null; then
            echo "$TOKENS" > "$CONTEXT_FILE"
        fi
    fi
fi

# --- 2. Signal the daemon ---------------------------------------------------
[ -f "$PID_FILE" ] || exit 0
PID=$(cat "$PID_FILE" 2>/dev/null)
[ -n "$PID" ] || exit 0
kill -USR1 "$PID" 2>/dev/null || true
exit 0

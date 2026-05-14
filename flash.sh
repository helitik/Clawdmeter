#!/bin/bash
# Build and flash firmware. Selects PlatformIO env via BOARD (default: tdisplay_s3).
# Usage: ./flash.sh [port]           # uses BOARD env var
#        BOARD=waveshare_amoled_216 ./flash.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PORT="${1:-/dev/ttyACM0}"
BOARD="${BOARD:-tdisplay_s3}"

echo "=== Flashing Claude Usage Tracker ==="
echo "Board: $BOARD"
echo "Port: $PORT"
echo ""

cd "$SCRIPT_DIR/firmware"

# Use ~/.platformio/penv/bin/pio if present, otherwise rely on PATH.
if [ -x "$HOME/.platformio/penv/bin/pio" ]; then
    PIO="$HOME/.platformio/penv/bin/pio"
else
    PIO="pio"
fi

"$PIO" run -e "$BOARD" -t upload --upload-port "$PORT"

echo ""
echo "=== Done! ==="

#!/usr/bin/env bash
#
# send_arduino_cmd.sh - Send custom commands to Arduino USB PWM controller via screen
#
# Usage:
#   ./send_arduino_cmd.sh "COMMAND" [DEVICE]
#   ./send_arduino_cmd.sh           # Runs in interactive prompt mode
#
# Examples:
#   ./send_arduino_cmd.sh "GET"
#   ./send_arduino_cmd.sh "SET PWM1 128"
#   ./send_arduino_cmd.sh "SET PWM2 255" /dev/ttyACM0

set -euo pipefail

SESSION_NAME="arduino_pwm"
BAUD="115200"

CMD="${1:-}"
TTY_DEV="${2:-/dev/ttyACM0}"

# If first argument looks like a device path rather than a command
if [[ "$CMD" =~ ^/dev/ ]]; then
    TTY_DEV="$CMD"
    CMD=""
fi

if [ ! -e "$TTY_DEV" ]; then
    echo "Error: Device $TTY_DEV not found." >&2
    exit 1
fi

LOG_FILE="/tmp/screen_${SESSION_NAME}.log"

# Ensure screen session is running
if ! screen -list | grep -q "[0-9]\+\.${SESSION_NAME}[[:space:]]"; then
    echo "Starting screen session '$SESSION_NAME' on $TTY_DEV ($BAUD)..."
    rm -f "$LOG_FILE"
    screen -dmS "$SESSION_NAME" -L -Logfile "$LOG_FILE" "$TTY_DEV" "$BAUD"
    sleep 1.5 # Allow time for Arduino serial connection reset
    # Disable log buffering so writes are flushed to disk immediately
    screen -S "$SESSION_NAME" -X logfile flush 0
else
    # Ensure log flushing is active on existing sessions
    screen -S "$SESSION_NAME" -X logfile flush 0 >/dev/null 2>&1 || true
fi

# Function to send a command and display response
send_and_read() {
    local command="$1"
    
    # Mark offset in log file before sending
    local start_size=0
    if [ -f "$LOG_FILE" ]; then
        start_size=$(stat -c%s "$LOG_FILE")
    fi

    # Send command with proper carriage return
    screen -S "$SESSION_NAME" -p 0 -X stuff "${command}"$'\r'

    # Wait briefly for Arduino to process and respond
    sleep 0.2
    screen -S "$SESSION_NAME" -X logfile flush 0 >/dev/null 2>&1 || true

    # Display newly appended log output
    if [ -f "$LOG_FILE" ]; then
        tail -c +"$((start_size + 1))" "$LOG_FILE" | tr -d '\r'
    fi
}

# Single command execution mode
if [ -n "$CMD" ]; then
    send_and_read "$CMD"
    exit 0
fi

# Interactive loop mode
echo "Connected to $TTY_DEV ($SESSION_NAME). Enter commands (e.g. GET, SET PWM1 128) or 'exit':"
while true; do
    read -r -p "arduino> " user_input
    [ -z "$user_input" ] && continue
    [ "$user_input" = "exit" ] || [ "$user_input" = "quit" ] && break

    send_and_read "$user_input"
done

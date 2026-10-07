#!/usr/bin/env bash
# Usage: tools/monitor.sh [seconds]   (no argument = run until Ctrl+C)
set -u
duration="${1:-}"
ports=(/dev/cu.usbserial-* /dev/cu.usbmodem* /dev/cu.wchusbserial*)
pids=()

for port in "${ports[@]}"; do
  [ -e "$port" ] || continue
  tag="${port##*/cu.}"
  tail -f /dev/null | arduino-cli monitor -p "$port" -c baudrate=460800 --quiet 2>&1 \
    | LC_ALL=C sed -u "s/^/[$tag] /" &
  pids+=($!)
done

[ ${#pids[@]} -eq 0 ] && { echo "no boards found"; exit 1; }
trap '{ kill ${pids[*]}; pkill -f "arduino-cli monitor"; pkill -f "tail -f /dev/null"; wait; } 2>/dev/null' EXIT

if [ -n "$duration" ]; then sleep "$duration"; else wait; fi

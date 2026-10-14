#!/usr/bin/env bash
# Sends one command to the running game's DGMP dev console and prints the answer.
#   tools/dev.sh "props pawn Health"
dir="$(cd "$(dirname "$0")/.." && pwd)/dev"
mkdir -p "$dir"
seq=$(( $(cat "$dir/seq" 2>/dev/null || echo 0) + 1 )); echo $seq > "$dir/seq"
printf '%s\n%s\n' "$seq" "$*" > "$dir/cmd.txt"
for i in $(seq 1 100); do
  if [ -f "$dir/out.txt" ] && [ "$(head -1 "$dir/out.txt")" = "$seq" ]; then tail -n +2 "$dir/out.txt"; exit 0; fi
  sleep 0.1
done
echo "no answer (game not running or world not ready)"; exit 1

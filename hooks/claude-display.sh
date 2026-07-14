#!/bin/sh
# Claude Code -> Status Display bridge.
# Registered as a Claude Code hook (see README). On each session lifecycle
# event it POSTs a compact JSON status to the ESP32 board over your LAN.
#
# Set BOARD to your board's address. The board advertises itself over mDNS as
# "claude-display.local"; if that doesn't resolve on your network, use its IP,
# e.g. BOARD="http://192.168.1.42".
BOARD="http://claude-display.local"

payload=$(cat)
event=$(printf '%s' "$payload" | jq -r '.hook_event_name // empty')
sid=$(printf '%s'   "$payload" | jq -r '.session_id // empty')
cwd=$(printf '%s'   "$payload" | jq -r '.cwd // empty')
tpath=$(printf '%s' "$payload" | jq -r '.transcript_path // empty')
[ -z "$sid" ] && exit 0
label=$(basename "$cwd" 2>/dev/null); [ -z "$label" ] && label="session"

# Claude fires Notification for BOTH "needs permission" and the ~60s "waiting
# for your input" idle nudge. Ignore the idle nudge so a finished (green)
# session doesn't drift to red on its own; real permission prompts still pass.
if [ "$event" = "Notification" ]; then
  msg=$(printf '%s' "$payload" | jq -r '.message // empty')
  case "$msg" in *[Ww]aiting*|*[Ii]dle*) exit 0 ;; esac
fi

# Context window usage % + short model tag, from the transcript's last turn
# with usage. context tokens = input + cache_creation + cache_read.
# Window is inferred: <=200k -> 200k, otherwise 1M (Claude Code doesn't expose it).
ctx=0
model=""
if [ -n "$tpath" ] && [ -f "$tpath" ]; then
  read toks m <<EOF
$(tail -n 200 "$tpath" | jq -rs 'map(select(.message.usage!=null))|last|if .==null then "0 unknown" else ((.message.usage.input_tokens//0)+(.message.usage.cache_creation_input_tokens//0)+(.message.usage.cache_read_input_tokens//0)) as $t|"\($t) \(.message.model//"unknown")" end' 2>/dev/null)
EOF
  case "$toks" in ''|*[!0-9]*) toks=0 ;; esac
  if [ "$toks" -le 200000 ]; then win=200000; else win=1000000; fi
  ctx=$(( toks * 100 / win ))
  [ "$ctx" -gt 99 ] && ctx=99
  [ "$m" != "unknown" ] && model=$(printf '%s' "$m" | cut -d- -f2)
fi

# Fire-and-forget: backgrounded with a short timeout so Claude never waits,
# and a down/absent board is harmless.
curl -s --max-time 1 -X POST "$BOARD/event" \
  -H 'Content-Type: application/json' \
  -d "{\"event\":\"$event\",\"session_id\":\"$sid\",\"cwd\":\"$cwd\",\"label\":\"$label\",\"ctx\":$ctx,\"model\":\"$model\"}" \
  >/dev/null 2>&1 &
exit 0

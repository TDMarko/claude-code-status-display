# Claude Code Status Display

A small always-on hardware dashboard for [Claude Code](https://claude.com/claude-code). It shows, one row per session, what every Claude Code session across your terminal tabs is doing right now — **working**, **done**, or **needs you** — plus each session's context-window usage and model. When a session changes state it flashes a big traffic-light circle so you notice from across the room.

Runs on a **LilyGO T-Display-S3** (ESP32-S3 + 1.9" ST7789, 320×170). Your machine pushes updates to it over WiFi via Claude Code hooks — no cloud, no broker, no daemon.

```
 Terminal tab 1 (Claude Code) ┐
 Terminal tab 2 (Claude Code) ┤  hook → curl POST (JSON)     ┌───────────────────────┐
 Terminal tab 3 (Claude Code) ┘  ─────────────────────────► │  ESP32 T-Display-S3    │
                                    over your WiFi / LAN      │  • HTTP server :80     │
                                                              │  • in-RAM session table│
                                                              │  • renders dashboard   │
                                                              └───────────────────────┘
```

## Features

- **One row per session**, labelled by project folder (with `#1`/`#2` suffixes when two sessions share a folder).
- **Live status**: ready (gray) · working (amber) · done (green) · needs you (red, blinking, pinned to top).
- **Context bar + %** per session (green → amber → red as the window fills).
- **Model tag** per session (`opus`, `fable`, …).
- **Traffic-light flash**: on any state change, a big colored circle blinks for ~1s with the session's name, so background tabs grab your attention.
- **Marquee names**: long folder names scroll (with a 5s pause) instead of truncating.
- **Scales**: tracks up to 24 sessions; prioritises needs-you/working; summarises the overflow (`+13 more: 5 work, 6 done, 2 idle`).
- **Quiet by default**: the 60-second "waiting for your input" idle nudge is filtered out, so a finished session stays green instead of drifting to red.

## Hardware

| Requirement | Notes |
|---|---|
| **LilyGO T-Display-S3** | ESP32-S3, 1.9" ST7789 IPS, 320×170. The non-touch version is fine. |
| **USB-C cable** | For flashing and (optionally) power. |
| **Power** | After flashing, run it off any USB charger/power bank, or a LiPo on the JST connector. The USB cable is only power + flashing — all data is over WiFi. |
| **2.4 GHz WiFi** | The ESP32-S3 radio is 2.4 GHz only. Your computer can be on 5 GHz as long as it's the same router/LAN. |

> **Does it work on any ESP32?** Not out of the box. The networking and dashboard logic are board-agnostic, but the display driver is hard-coded for this board's ST7789 parallel panel and pins. See [Porting](#porting-to-other-boards).

## Software

- [PlatformIO](https://platformio.org/) (VS Code extension or `pio` CLI).
- `jq` and `curl` on the machine running Claude Code (macOS/Linux). `curl` is preinstalled; install `jq` with `brew install jq` / `apt install jq`.
- Claude Code.

Arduino_GFX and ArduinoJson are fetched automatically by PlatformIO — nothing to install by hand.

## Setup

### 1. Flash the firmware

```bash
git clone <your-repo-url> claude-code-status-display
cd claude-code-status-display
cp src/secrets.h.example src/secrets.h
```

Edit `src/secrets.h` with your **2.4 GHz** WiFi credentials (this file is git-ignored, so it's never committed):

```c
#define WIFI_SSID "your-network"
#define WIFI_PASS "your-password"
```

Plug in the board and flash:

```bash
pio run -e claude_display -t upload
```

> If upload fails with `Invalid head of packet` / a serial-sync error, put the board in bootloader mode: **hold BOOT, tap RST, release BOOT**, then run upload again. Make sure no serial monitor is holding the port.

Open the serial monitor to see the IP it gets:

```bash
pio device monitor -b 115200
```

You should see `WiFi: 192.168.x.y`. The board also advertises itself as `claude-display.local` via mDNS. Confirm it's reachable:

```bash
curl http://claude-display.local/      # or  curl http://192.168.x.y/
# -> claude-display ok
```

### 2. Install the hook script

```bash
mkdir -p ~/.claude/hooks
cp hooks/claude-display.sh ~/.claude/hooks/
chmod +x ~/.claude/hooks/claude-display.sh
```

Open `~/.claude/hooks/claude-display.sh` and set `BOARD`. Leave the `claude-display.local` default if mDNS resolved above; otherwise use the IP:

```sh
BOARD="http://192.168.x.y"
```

### 3. Wire up Claude Code hooks

Add these five events to `~/.claude/settings.json`. **Merge** into any existing `hooks` block — don't overwrite hooks you already have. Use the absolute path (Claude Code does not expand `~`):

```json
{
  "hooks": {
    "SessionStart":     [{ "hooks": [{ "type": "command", "command": "/Users/YOU/.claude/hooks/claude-display.sh", "async": true }] }],
    "UserPromptSubmit": [{ "hooks": [{ "type": "command", "command": "/Users/YOU/.claude/hooks/claude-display.sh", "async": true }] }],
    "Notification":     [{ "hooks": [{ "type": "command", "command": "/Users/YOU/.claude/hooks/claude-display.sh", "async": true }] }],
    "Stop":             [{ "hooks": [{ "type": "command", "command": "/Users/YOU/.claude/hooks/claude-display.sh", "async": true }] }],
    "SessionEnd":       [{ "hooks": [{ "type": "command", "command": "/Users/YOU/.claude/hooks/claude-display.sh", "async": true }] }]
  }
}
```

Validate the file: `jq . ~/.claude/settings.json >/dev/null && echo OK`

### 4. Test

Hooks load when a session starts, so open a **new** terminal tab and run `claude`. A row should appear labelled by the folder, tracking working → done as you go. Or fire a fake event by hand:

```bash
curl -s http://claude-display.local/event -H 'Content-Type: application/json' \
  -d '{"event":"Stop","session_id":"t1","cwd":"/x/demo","label":"demo","ctx":42,"model":"opus"}'
```

## Status reference

| Claude Code hook | Row status | Colour |
|---|---|---|
| `SessionStart` | ready | gray |
| `UserPromptSubmit` | working | amber |
| `Stop` | done | green |
| `Notification` (permission/attention) | needs you | red, blinking, pinned to top |
| `Notification` (60s idle nudge) | *ignored* | — |
| `SessionEnd` | row removed | — |

## Configuration

Firmware knobs (top of `src/main.ino`):

- **Colours** — `C_READY / C_WORKING / C_NEEDS / C_DONE` (RGB565).
- **Layout** — `ROW_H`, `MAX_ROWS`, `MAX_SESSIONS`.
- **Flash** — `FLASH_MS` (traffic-light duration).
- **Marquee** — `HOLD_L` / `HOLD_R` (end pauses) and the `* 25` / `/ 25` scroll speed in `drawMarqueeLabel`.

Host knobs (`hooks/claude-display.sh`):

- **`BOARD`** — board address (hostname or IP).
- **Context window** — inferred as 200k, or 1M once a turn exceeds 200k. Claude Code doesn't expose the real window to hooks; hard-code `win` if you always run one size (e.g. a `[1m]` model).

## Porting to other boards

The display is the only board-specific part. In `src/main.ino`, the block between the `Arduino_GFX setting` comments defines the panel:

```c
Arduino_DataBus *bus = new Arduino_ESP32PAR8Q(
    7 /*DC*/, 6 /*CS*/, 8 /*WR*/, 9 /*RD*/,
    39,40,41,42,45,46,47,48 /*D0..D7*/);
Arduino_G *output = new Arduino_ST7789(
    bus, 5 /*RST*/, 3 /*rotation*/, true /*IPS*/, 170, 320, 35,0,35,0);
```

To run on a different ESP32 + display, replace this with the correct Arduino_GFX bus/driver for your panel (e.g. an SPI `Arduino_HWSPI` + `Arduino_ILI9341`), fix the width/height/rotation, and update `board`/`build_flags` in `platformio.ini`. Everything else (WiFi, HTTP endpoint, rendering) is unchanged.

## Troubleshooting

- **Upload: `Invalid head of packet`** — bootloader mode (hold BOOT, tap RST, release BOOT); close any serial monitor first.
- **`claude-display.local` won't resolve** — mDNS is flaky on some networks; use the board's IP in `BOARD` (and set a DHCP reservation on your router so the IP is stable when it runs untethered).
- **Serial shows `WiFi: FAILED`** — it must be a 2.4 GHz network; double-check the SSID/password (special characters in the password can break the C string); WPA3-only routers may need WPA2/WPA3 mixed mode.
- **Nothing appears on the board** — check the board is reachable (`curl http://<board>/`), that you started a **new** session after adding the hooks, and that `jq` is installed.

## Credits

- Board definition and hardware: [LilyGO T-Display-S3](https://github.com/Xinyuan-LilyGO/T-Display-S3).
- Graphics: [Arduino_GFX](https://github.com/moononournation/Arduino_GFX) by moononournation.
- JSON parsing: [ArduinoJson](https://arduinojson.org/) by Benoît Blanchon.

## License

MIT — see [LICENSE](LICENSE).

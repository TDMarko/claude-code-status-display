# Claude Code Status Display

A small always-on hardware dashboard for [Claude Code](https://claude.com/claude-code) that tracks **all your concurrent sessions at once** — one row per terminal tab. See at a glance what each session is doing right now — **working**, **done**, or **needs you** — plus its context-window usage and model. When a session changes state it flashes a big traffic-light circle so you notice from across the room.

Runs on a **LilyGO T-Display-S3** (ESP32-S3 + 1.9" ST7789, 320×170). Your machine pushes updates to it over WiFi via Claude Code hooks — no cloud, no broker, no daemon.

![The dashboard running on a LilyGO T-Display-S3: four Claude Code sessions with per-row model tag, status, and context-usage bars](docs/example.png)

```
 Terminal tab 1 (Claude Code) ┐
 Terminal tab 2 (Claude Code) ┤  hook → curl POST (JSON)     ┌───────────────────────┐
 Terminal tab 3 (Claude Code) ┘  ─────────────────────────► │  ESP32 T-Display-S3    │
                                    over your WiFi / LAN      │  • HTTP server :80     │
                                                              │  • in-RAM session table│
                                                              │  • renders dashboard   │
                                                              └───────────────────────┘
```

## Compatibility

The integration is a Claude Code **hook**, so it works with any Claude Code that runs **locally on your machine** and reads `~/.claude/settings.json`:

| Surface | Works? | Why |
|---|---|---|
| Terminal / CLI | ✅ | Runs locally, fires hooks. |
| VS Code / JetBrains extension | ✅ | Same local engine and `~/.claude/settings.json`. |
| Desktop app (Mac/Windows) | ✅ *(should)* | Runs locally; not hardware-tested here. |
| Web app (claude.ai/code) | ❌ | Runs on Anthropic's servers — the hook can't execute on your machine or reach a device on your LAN. |

Multiple surfaces at once is fine — every local session that fires hooks shows up, regardless of which app started it.

## Features

- **Multi-session by design** — tracks every concurrent Claude Code session/tab at once (up to 24), one row each, priority-sorted so anything needing you jumps to the top; overflow collapses into a per-state summary (`+13 more: 5 work, 6 done, 2 idle`).
- **One row per session**, labelled by project folder (with `#1`/`#2` suffixes when two sessions share a folder).
- **Live status**: ready (gray) · working (amber) · done (green) · needs you (red, blinking, pinned to top).
- **Context bar + %** per session (green → amber → red as the window fills).
- **Model tag** per session (`opus`, `fable`, …).
- **Traffic-light flash**: on any state change, a big colored circle blinks for ~1s with the session's name, so background tabs grab your attention.
- **Marquee names**: long folder names scroll (with a 5s pause) instead of truncating.
- **Scales**: tracks up to 24 sessions; prioritises needs-you/working; summarises the overflow (`+13 more: 5 work, 6 done, 2 idle`).
- **Quiet by default**: the 60-second "waiting for your input" idle nudge is filtered out, so a finished session stays green instead of drifting to red.
- **Configurable display**: driver, bus, pins, and resolution live in one `display_config.h`; the layout auto-adapts. Ships tuned for the T-Display-S3, portable to other ESP32 + ST7789/ILI9341 panels.

## Hardware

| Requirement | Notes |
|---|---|
| **LilyGO T-Display-S3** | ESP32-S3, 1.9" ST7789 IPS, 320×170. The non-touch version is fine. |
| **USB-C cable** | For flashing and (optionally) power. |
| **Power** | After flashing, run it off any USB charger/power bank, or a LiPo on the JST connector. The USB cable is only power + flashing — all data is over WiFi. |
| **2.4 GHz WiFi** | The ESP32-S3 radio is 2.4 GHz only. Your computer can be on 5 GHz as long as it's the same router/LAN. |

> **Does it work on any ESP32?** Not automatically — the display driver, bus, and pins are compile-time settings, not runtime ones. But they all live in one file (`src/display_config.h`): set your bus/driver/pins/resolution, reflash, and the dashboard layout adapts to the new screen. Ships tuned for the T-Display-S3. See [Porting](#porting-to-other-boards).

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

Display hardware — **`src/display_config.h`** (the file you edit for a different board):

- **Bus** — `BUS_PARALLEL8` or `BUS_SPI`.
- **Driver** — `DRIVER_ST7789` or `DRIVER_ILI9341`.
- **Geometry** — `TFT_WIDTH` / `TFT_HEIGHT` / `TFT_ROTATION` / `TFT_IPS` / offsets.
- **Pins** — `PIN_RST`, `PIN_BL`, `PIN_PWR` (use `-1` if absent) and the bus pins.

The dashboard layout **auto-adapts** to the configured resolution: row count comes from the height, and the model/status/context columns anchor to the right edge with the name filling the rest. (Very narrow panels, <~250px wide, will be cramped since the font is fixed-size.)

Behaviour knobs (top of `src/main.ino`):

- **Colours** — `C_READY / C_WORKING / C_NEEDS / C_DONE` (RGB565).
- **Rows / capacity** — `ROW_H`, `MAX_SESSIONS`.
- **Flash** — `FLASH_MS` (traffic-light duration).
- **Marquee** — `HOLD_L` / `HOLD_R` (end pauses) and the `* 25` / `/ 25` scroll speed in `drawMarqueeLabel`.

Host knobs (`hooks/claude-display.sh`):

- **`BOARD`** — board address (hostname or IP).
- **Context window** — inferred as 200k, or 1M once a turn exceeds 200k. Claude Code doesn't expose the real window to hooks; hard-code `win` if you always run one size (e.g. a `[1m]` model).

## Porting to other boards

Everything except the display is board-agnostic, so porting is just editing **`src/display_config.h`** and reflashing:

1. Pick the **bus** (`BUS_PARALLEL8` / `BUS_SPI`) and **driver** (`DRIVER_ST7789` / `DRIVER_ILI9341`).
2. Set the **pins** for your wiring and the **geometry** (`TFT_WIDTH`/`TFT_HEIGHT`/`TFT_ROTATION`/offsets).
3. If your board isn't an ESP32-S3 dev variant, update `board` in `platformio.ini`.

The layout re-flows to the new resolution automatically. To support a **driver or bus not listed** (e.g. ST7735, SSD1306, software SPI), add one `#elif` branch to the bus/driver `#if` blocks in `src/main.ino` — that's the only code that touches the panel type.

### Example: generic ESP32 + SPI ILI9341 (240×320)

A common 2.4"/2.8" ILI9341 breakout on a classic ESP32 dev board. Set these in `src/display_config.h` (the SPI pins go in the existing `#ifdef BUS_SPI` block):

```c
// #define BUS_PARALLEL8
#define BUS_SPI                 // <- select SPI

// #define DRIVER_ST7789
#define DRIVER_ILI9341          // <- select ILI9341

// ILI9341 is fixed 240x320; rotation 1 = 320x240 landscape
#define TFT_WIDTH      240
#define TFT_HEIGHT     320
#define TFT_ROTATION   1
#define TFT_IPS        false
#define TFT_COL_OFFSET 0
#define TFT_ROW_OFFSET 0

#define PIN_RST 4
#define PIN_BL  32              // backlight GPIO (or wire LED to 3V3 and use -1)
#define PIN_PWR -1              // no separate panel-power pin

// inside the #ifdef BUS_SPI block — classic ESP32 VSPI pins:
#define PIN_DC   2
#define PIN_CS   15
#define PIN_SCK  18
#define PIN_MOSI 23
#define PIN_MISO 19
```

Then point `platformio.ini` at a plain ESP32 and drop the ESP32-S3-only USB flags:

```ini
board = esp32dev
build_flags =
    -DDISABLE_ALL_LIBRARY_WARNINGS
```

Wiring: panel `VCC`→3V3, `GND`→GND, `LED`→`PIN_BL` (or 3V3), and `SDI/MOSI`, `SCK`, `CS`, `DC`, `RESET`, `SDO/MISO` to the pins above. (Exact GPIOs are up to your wiring — these are just common defaults.)

## Troubleshooting

- **Upload: `Invalid head of packet`** — bootloader mode (hold BOOT, tap RST, release BOOT); close any serial monitor first.
- **`claude-display.local` won't resolve** — mDNS is flaky on some networks; use the board's IP in `BOARD` (and set a DHCP reservation on your router so the IP is stable when it runs untethered).
- **Serial shows `WiFi: FAILED`** — it must be a 2.4 GHz network; double-check the SSID/password (special characters in the password can break the C string); WPA3-only routers may need WPA2/WPA3 mixed mode.
- **Nothing appears on the board** — check the board is reachable (`curl http://<board>/`), that you started a **new** session after adding the hooks, and that `jq` is installed.

## Related projects

Prior art in the "physical status display for Claude Code" space — worth a look, and thanks for the inspiration:

- [alonw0/claude-monitor-esp32](https://github.com/alonw0/claude-monitor-esp32) — MicroPython OLED notifier with sound and a "waiting" LED.
- [houxiaomu/m5stack-coding-toys](https://github.com/houxiaomu/m5stack-coding-toys) — mirrors a single live session (model, context, cost, git) onto an M5Stack CoreS3.
- [rootedlab-code/claude-code-usage-monitor](https://github.com/rootedlab-code/claude-code-usage-monitor) — Waveshare ESP32-S3 usage monitor (cost, tokens, rate-limit window) via a Python bridge.
- [puritysb/AgentDeck](https://github.com/puritysb/AgentDeck) — multi-surface controller/dashboard for AI coding agents.

**What's different here:** a **multi-session** dashboard (one row per concurrent tab, priority-sorted) driven purely by Claude Code **hooks → HTTP over WiFi** — no bridge daemon and no Bluetooth.

## Credits

- Board definition and hardware: [LilyGO T-Display-S3](https://github.com/Xinyuan-LilyGO/T-Display-S3).
- Graphics: [Arduino_GFX](https://github.com/moononournation/Arduino_GFX) by moononournation.
- JSON parsing: [ArduinoJson](https://arduinojson.org/) by Benoît Blanchon.

## License

MIT — see [LICENSE](LICENSE).

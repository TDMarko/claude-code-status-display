#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "secrets.h"
#include <WebServer.h>
#include <ArduinoJson.h>

// ---- Display (from Arduino_GFX_PDQgraphicstest; rotation 3 = landscape 320x170) ----
#define GFX_EXTRA_PRE_INIT() { pinMode(15, OUTPUT); digitalWrite(15, HIGH); } // PWD
#define GFX_BL 38                                                             // backlight
Arduino_DataBus *bus = new Arduino_ESP32PAR8Q(
    7 /*DC*/, 6 /*CS*/, 8 /*WR*/, 9 /*RD*/,
    39, 40, 41, 42, 45, 46, 47, 48 /*D0..D7*/);
Arduino_G *output = new Arduino_ST7789(
    bus, 5 /*RST*/, 3 /*rotation*/, true /*IPS*/,
    170 /*width*/, 320 /*height*/, 35, 0, 35, 0 /*offsets*/);
// Off-screen framebuffer (320x170) blitted in one pass -> no flicker
Arduino_Canvas *gfx = new Arduino_Canvas(320 /*w*/, 170 /*h*/, output);

// ---- Colors (RGB565) ----
#define C_BG      0x0000
#define C_TEXT    0xFFFF
#define C_HEADER  0x5AEB
#define C_READY   0x7BEF
#define C_WORKING 0xFD20
#define C_NEEDS   0xF800
#define C_DONE    0x07E0

// ---- Session table ----
enum State { ST_READY = 0, ST_WORKING, ST_NEEDS_YOU, ST_DONE };
const int MAX_SESSIONS = 24;
struct Session {
  char sessionId[40];
  char label[24];
  char model[8];         // short model tag, e.g. "opus"
  uint8_t state;
  uint8_t ctxPct;        // context window used, 0-100
  uint32_t lastChange;   // millis() at last state change
  bool active;
};
Session sessions[MAX_SESSIONS];

// ---- Layout ----
const int HEADER_H = 22;
const int ROW_H    = 18;
const int MAX_ROWS = 7;

uint32_t lastRender = 0;
uint32_t lastBlink  = 0;
bool blinkOn = false;
uint32_t lastReconnect = 0;
bool needFastRender = false;   // set when a label is scrolling, so loop renders at ~30fps

WebServer server(80);
const uint32_t EXPIRY_MS = 30UL * 60UL * 1000UL;   // 30 min zombie expiry

// Attention flash: pulse a thick colored border on any state change
const uint32_t FLASH_MS = 1000;
uint32_t flashUntil = 0;
uint16_t flashColor = 0;
char flashLabel[24] = "";   // name of the session that triggered the flash

uint16_t stateColor(uint8_t st) {
  switch (st) {
    case ST_WORKING:   return C_WORKING;
    case ST_NEEDS_YOU: return C_NEEDS;
    case ST_DONE:      return C_DONE;
    default:           return C_READY;
  }
}
const char* stateText(uint8_t st) {
  switch (st) {
    case ST_WORKING:   return "working";
    case ST_NEEDS_YOU: return "needs you";
    case ST_DONE:      return "done";
    default:           return "ready";
  }
}
// Appends "#k" when >1 active session shares the same label; else the plain label.
void displayLabel(int i, char* out, size_t n) {
  int dup = 0, idx = 0;
  for (int j = 0; j < MAX_SESSIONS; j++) {
    if (!sessions[j].active) continue;
    if (strcmp(sessions[j].label, sessions[i].label) == 0) { dup++; if (j == i) idx = dup; }
  }
  if (dup > 1) snprintf(out, n, "%s#%d", sessions[i].label, idx);
  else         snprintf(out, n, "%s", sessions[i].label);
}

// Draw a size-2 label at (x,y) inside a w-wide column, ping-pong scrolling it
// horizontally if it's too long. Overflow is masked so it can't bleed into
// neighboring columns; caller must draw the dot and right-hand columns AFTER.
void drawMarqueeLabel(int x, int y, int w, const char* text) {
  gfx->setTextSize(2);
  gfx->setTextColor(C_TEXT);
  int tw = (int)strlen(text) * 12;         // 12 px per char at text size 2
  int off = 0;
  if (tw > w) {
    needFastRender = true;
    uint32_t over = tw - w;
    const uint32_t HOLD_L = 5000, HOLD_R = 1000;     // ms pause at each end
    uint32_t travel = over * 25;                     // ~40 px/sec
    uint32_t period = HOLD_L + travel + HOLD_R + travel;
    uint32_t ph = millis() % period;
    if (ph < HOLD_L)                       off = 0;                              // hold left
    else if (ph < HOLD_L + travel)         off = (ph - HOLD_L) / 25;            // scroll right
    else if (ph < HOLD_L + travel + HOLD_R) off = over;                          // hold right
    else                                   off = over - (ph - HOLD_L - travel - HOLD_R) / 25; // back
  }
  gfx->setCursor(x - off, y);
  gfx->print(text);
  gfx->fillRect(x + w, y, 320 - (x + w), 16, C_BG);        // mask right overflow
  gfx->fillRect(0, y, x, 16, C_BG);                        // mask left overflow
}

void render() {
  // Attention flash: blank the screen and blink one big traffic-light circle
  // in the new state's color for FLASH_MS, then fall through to the dashboard.
  if (millis() < flashUntil) {
    gfx->fillScreen(C_BG);
    int cx = gfx->width() / 2, cy = gfx->height() / 2, r = 62;
    bool on = ((flashUntil - millis()) / 150) % 2 == 0;   // blink ~3x/sec
    gfx->drawCircle(cx, cy, r, C_HEADER);                 // lamp socket ring
    if (on) gfx->fillCircle(cx, cy, r - 3, flashColor);   // lamp lit
    gfx->setTextSize(2);                                  // which session, centered at bottom
    gfx->setTextColor(C_TEXT);
    int lw = (int)strlen(flashLabel) * 12;
    gfx->setCursor((gfx->width() - lw) / 2, gfx->height() - 18);
    gfx->print(flashLabel);
    gfx->flush();
    return;
  }

  gfx->fillScreen(C_BG);
  needFastRender = false;   // recomputed below; set true if any label scrolls

  // Header: "Claude N" + IP/status on the right
  int count = 0;
  for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].active) count++;
  gfx->setTextSize(2);
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(4, 3);
  char hdr[16];
  snprintf(hdr, sizeof(hdr), "Claude %d", count);
  gfx->print(hdr);
  gfx->setTextSize(1);
  gfx->setTextColor(C_HEADER);
  String status = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("no wifi");
  gfx->setCursor(320 - status.length() * 6 - 4, 7);
  gfx->print(status);
  gfx->drawFastHLine(0, HEADER_H, 320, C_HEADER);

  // Order by priority: needs-you, then working, then done, then ready/idle
  int order[MAX_SESSIONS]; int m = 0;
  const uint8_t prio[4] = { ST_NEEDS_YOU, ST_WORKING, ST_DONE, ST_READY };
  for (int pr = 0; pr < 4; pr++)
    for (int i = 0; i < MAX_SESSIONS; i++)
      if (sessions[i].active && sessions[i].state == prio[pr]) order[m++] = i;

  int shown = m < MAX_ROWS ? m : MAX_ROWS;
  for (int r = 0; r < shown; r++) {
    int i = order[r];
    int y = HEADER_H + 4 + r * ROW_H;
    uint16_t col = stateColor(sessions[i].state);

    char lbl[28]; displayLabel(i, lbl, sizeof(lbl));
    drawMarqueeLabel(18, y, 134, lbl);     // scrolls long names; masks its own band

    bool drawDot = !(sessions[i].state == ST_NEEDS_YOU && !blinkOn);   // blink for needs-you
    if (drawDot) gfx->fillCircle(8, y + 6, 4, col);

    gfx->setTextSize(1);
    gfx->setTextColor(C_HEADER);           // model tag
    gfx->setCursor(156, y + 3);
    gfx->print(sessions[i].model);

    gfx->setTextColor(col);                // status
    gfx->setCursor(196, y + 3);
    gfx->print(stateText(sessions[i].state));

    // context bar (green < 60, amber 60-84, red >= 85) + percentage
    uint8_t p = sessions[i].ctxPct > 99 ? 99 : sessions[i].ctxPct;   // 99% is the max shown
    int bx = 256, bw = 40, by = y + 3, bh = 8;
    gfx->drawRect(bx, by, bw, bh, C_HEADER);
    if (p > 0) {
      uint16_t bc = p >= 85 ? C_NEEDS : (p >= 60 ? C_WORKING : C_DONE);
      gfx->fillRect(bx + 1, by + 1, (bw - 2) * p / 100, bh - 2, bc);
    }
    char pc[6]; snprintf(pc, sizeof(pc), "%d%%", p);
    gfx->setTextColor(C_TEXT);
    gfx->setCursor(318 - (int)strlen(pc) * 6, y + 3);   // right-aligned to the edge
    gfx->print(pc);
  }
  if (m > MAX_ROWS) {
    int y = HEADER_H + 4 + MAX_ROWS * ROW_H;
    // Summarize the hidden (lowest-priority) sessions by state
    int c[4] = { 0, 0, 0, 0 };   // indexed by state enum
    for (int r = MAX_ROWS; r < m; r++) c[sessions[order[r]].state]++;
    char more[64];
    int n = snprintf(more, sizeof(more), "+%d more:", m - MAX_ROWS);
    if (c[ST_NEEDS_YOU]) n += snprintf(more + n, sizeof(more) - n, " %d need", c[ST_NEEDS_YOU]);
    if (c[ST_WORKING])   n += snprintf(more + n, sizeof(more) - n, " %d work", c[ST_WORKING]);
    if (c[ST_DONE])      n += snprintf(more + n, sizeof(more) - n, " %d done", c[ST_DONE]);
    if (c[ST_READY])     n += snprintf(more + n, sizeof(more) - n, " %d idle", c[ST_READY]);
    gfx->setTextSize(1);
    gfx->setTextColor(C_HEADER);
    gfx->setCursor(20, y);
    gfx->print(more);
  }
  gfx->flush();
}

int findSlot(const char* sid) {
  for (int i = 0; i < MAX_SESSIONS; i++)
    if (sessions[i].active && strcmp(sessions[i].sessionId, sid) == 0) return i;
  return -1;
}
int allocSlot() {
  for (int i = 0; i < MAX_SESSIONS; i++) if (!sessions[i].active) return i;
  return -1;
}
void expireOld() {
  uint32_t now = millis();
  for (int i = 0; i < MAX_SESSIONS; i++)
    if (sessions[i].active && (now - sessions[i].lastChange) > EXPIRY_MS)
      sessions[i].active = false;
}
void handleEvent() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
  JsonDocument doc;   // ArduinoJson v7 elastic document
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "text/plain", "bad json"); return; }
  const char* event = doc["event"]      | "";
  const char* sid   = doc["session_id"] | "";
  const char* label = doc["label"]      | "session";
  if (strlen(sid) == 0) { server.send(400, "text/plain", "no session_id"); return; }

  if (strcmp(event, "SessionEnd") == 0) {
    int i = findSlot(sid);
    if (i >= 0) sessions[i].active = false;
    server.send(200, "text/plain", "ok");
    return;
  }
  int i = findSlot(sid);
  if (i < 0) {
    i = allocSlot();
    if (i < 0) { server.send(507, "text/plain", "table full"); return; }
    sessions[i].active = true;
    strncpy(sessions[i].sessionId, sid, sizeof(sessions[i].sessionId) - 1);
    sessions[i].sessionId[sizeof(sessions[i].sessionId) - 1] = '\0';
  }
  strncpy(sessions[i].label, label, sizeof(sessions[i].label) - 1);
  sessions[i].label[sizeof(sessions[i].label) - 1] = '\0';
  const char* model = doc["model"] | "";
  strncpy(sessions[i].model, model, sizeof(sessions[i].model) - 1);
  sessions[i].model[sizeof(sessions[i].model) - 1] = '\0';
  sessions[i].ctxPct = doc["ctx"] | 0;

  uint8_t st = sessions[i].state;
  if      (strcmp(event, "SessionStart")     == 0) st = ST_READY;
  else if (strcmp(event, "UserPromptSubmit") == 0) st = ST_WORKING;
  else if (strcmp(event, "Notification")     == 0) st = ST_NEEDS_YOU;
  else if (strcmp(event, "Stop")             == 0) st = ST_DONE;
  sessions[i].state = st;
  sessions[i].lastChange = millis();
  flashColor = stateColor(st);          // traffic-light circle in the new state's color
  flashUntil = millis() + FLASH_MS;
  displayLabel(i, flashLabel, sizeof(flashLabel));   // remember which session
  server.send(200, "text/plain", "ok");
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(250);
  Serial.print("WiFi: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("FAILED"));
}

void setup() {
  Serial.begin(115200);
  GFX_EXTRA_PRE_INIT();
  pinMode(GFX_BL, OUTPUT); digitalWrite(GFX_BL, HIGH);
  gfx->begin();
  gfx->fillScreen(C_BG);
  gfx->setTextSize(2); gfx->setTextColor(C_TEXT);
  gfx->setCursor(4, 4); gfx->print("Connecting WiFi...");
  connectWiFi();
  if (MDNS.begin("claude-display")) MDNS.addService("http", "tcp", 80);
  server.on("/", HTTP_GET, []() { server.send(200, "text/plain", "claude-display ok"); });
  server.on("/event", HTTP_POST, handleEvent);
  server.begin();
  render();
}

void loop() {
  server.handleClient();
  uint32_t now = millis();
  if (now - lastBlink > 500)  { blinkOn = !blinkOn; lastBlink = now; }
  uint32_t renderEvery = (now < flashUntil || needFastRender) ? 33 : 500;   // fast for pulse/marquee
  if (now - lastRender > renderEvery) { expireOld(); render(); lastRender = now; }
  if (WiFi.status() != WL_CONNECTED && now - lastReconnect > 10000) {
    WiFi.reconnect(); lastReconnect = now;
  }
}

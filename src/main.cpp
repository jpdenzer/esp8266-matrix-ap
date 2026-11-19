/*
 * MatrixBoard — standalone Wi-Fi message board
 * --------------------------------------------
 * Wemos D1 Mini (ESP8266) + 4-in-1 MAX7219 (FC16) 32x8 LED matrix.
 *
 *  - Runs its own access point (no router / internet needed).
 *  - A DNS "catch-all" makes phones pop up the control page automatically
 *    (captive portal). The page is also reachable at http://192.168.4.1/
 *  - Message, brightness, speed and mode are saved to LittleFS and restored
 *    after a power cycle.
 *  - The active display text lives in fixed-size char buffers; no Arduino
 *    String objects are used in loop() (only briefly inside HTTP handlers,
 *    where ESP8266WebServer hands them to us).
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <SPI.h>
#include <MD_MAX72xx.h>
#include <MD_Parola.h>

// ============================================================================
//  User configuration
// ============================================================================

// --- Display wiring ---------------------------------------------------------
// Hardware SPI is fixed: CLK -> D5 (GPIO14), DIN -> D7 (GPIO13).
// CS can be any free GPIO; set this to match your wiring.
//  - D6 (GPIO12) is the SPI MISO pin, but the MAX7219 never sends data back,
//    and MD_MAX72XX reclaims the pin as a plain output after SPI.begin().
//  - D2 (GPIO4) / D1 (GPIO5) have no boot role either — also good choices.
//  - D4 (GPIO2) works but drives the on-board LED (it will flicker).
//  - Avoid D8 (GPIO15): it must be LOW at boot.
// Not sure how yours is wired? Flash the wiring finder: `pio run -e diag -t upload`
#define CS_PIN          D6

#define HARDWARE_TYPE   MD_MAX72XX::FC16_HW
#define MAX_DEVICES     4                // 4 x (8x8) = 32x8

// --- Access point -----------------------------------------------------------
static const char AP_SSID[]     = "MatrixBoard";
static const char AP_PASSWORD[] = "matrix1234";   // >= 8 chars, or "" for an open AP
static const uint8_t AP_CHANNEL = 6;
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_NETMASK(255, 255, 255, 0);

// --- Behaviour --------------------------------------------------------------
static const size_t   MSG_MAX_LEN    = 160;        // max characters in a message
static const uint8_t  SPEED_MIN      = 1;          // UI speed scale (1 = slowest)
static const uint8_t  SPEED_MAX      = 10;
static const uint32_t SAVE_DELAY_MS  = 3000;       // debounce flash writes
static const char     SETTINGS_PATH[] = "/settings.bin";

static const char DEFAULT_MESSAGE[] = "Join Wi-Fi \"MatrixBoard\" to edit me!";

// ============================================================================
//  Types & globals
// ============================================================================

enum DisplayMode : uint8_t { MODE_SCROLL = 0, MODE_STATIC = 1 };

// Persisted settings. Stored as a raw binary blob; `magic` + `version` let us
// reject a missing, corrupt or outdated file and fall back to defaults.
struct Settings {
  uint32_t magic;
  uint8_t  version;
  uint8_t  brightness;               // 0..15 (MAX7219 intensity)
  uint8_t  speed;                    // SPEED_MIN..SPEED_MAX
  uint8_t  mode;                     // DisplayMode
  char     message[MSG_MAX_LEN + 1]; // always NUL-terminated
};

static const uint32_t SETTINGS_MAGIC   = 0x4D545258;  // "MTRX"
static const uint8_t  SETTINGS_VERSION = 1;

static Settings settings;

// MD_Parola keeps a *pointer* to the text it animates, so it gets its own
// buffer that is only rewritten right before a displayText() call.
static char displayBuf[MSG_MAX_LEN + 1];

static bool     settingsDirty = false;
static uint32_t lastChangeMs  = 0;

MD_Parola        matrix(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);  // hardware SPI
ESP8266WebServer server(80);
DNSServer        dnsServer;

// ============================================================================
//  Web page (stored in flash)
// ============================================================================

static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MatrixBoard</title>
<style>
:root{--bg:#111418;--card:#1b2027;--fg:#e8eaed;--mut:#9aa3ad;--acc:#ff4d3a;--bd:#2c333d}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:480px;margin:0 auto;padding:20px 16px 40px}
h1{font-size:1.4rem;margin:0 0 4px;letter-spacing:.02em}
h1 span{color:var(--acc)}
.sub{color:var(--mut);font-size:.9rem;margin:0 0 20px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:14px;padding:16px;margin-bottom:14px}
label{display:block;font-weight:600;margin-bottom:8px}
.row{display:flex;justify-content:space-between;align-items:baseline}
.row output{color:var(--acc);font-weight:700;font-variant-numeric:tabular-nums}
textarea{width:100%;min-height:84px;resize:vertical;background:var(--bg);color:var(--fg);border:1px solid var(--bd);border-radius:10px;padding:10px;font:inherit}
textarea:focus{outline:2px solid var(--acc);border-color:transparent}
.hint{color:var(--mut);font-size:.8rem;margin-top:6px;display:flex;justify-content:space-between}
input[type=range]{width:100%;accent-color:var(--acc);height:28px}
.seg{display:flex;background:var(--bg);border:1px solid var(--bd);border-radius:10px;padding:3px}
.seg label{flex:1;margin:0;text-align:center;padding:9px 0;border-radius:8px;cursor:pointer;font-weight:600;color:var(--mut)}
.seg input{display:none}
.seg input:checked+span{color:var(--fg)}
.seg label:has(input:checked){background:var(--card);box-shadow:0 0 0 1px var(--bd)}
button{width:100%;padding:14px;border:0;border-radius:12px;background:var(--acc);color:#fff;font:inherit;font-weight:700;cursor:pointer}
button:active{transform:translateY(1px)}
#st{text-align:center;color:var(--mut);font-size:.85rem;min-height:1.2em;margin-top:10px}
</style></head><body><main>
<h1>Matrix<span>Board</span></h1>
<p class="sub">32&times;8 LED message display</p>
<form id="f" method="post" action="/set">
 <div class="card">
  <label for="msg">Message</label>
  <textarea id="msg" name="msg" maxlength="160" placeholder="Type your message&hellip;"></textarea>
  <div class="hint"><span>Plain ASCII characters</span><span id="cnt">0/160</span></div>
 </div>
 <div class="card">
  <label>Mode</label>
  <div class="seg">
   <label><input type="radio" name="mode" value="0" checked><span>Scroll</span></label>
   <label><input type="radio" name="mode" value="1"><span>Static</span></label>
  </div>
  <div class="hint"><span>Static centres short text (about 5&ndash;6 characters fit)</span></div>
 </div>
 <div class="card">
  <div class="row"><label for="bri">Brightness</label><output id="briv">4</output></div>
  <input type="range" id="bri" name="bri" min="0" max="15" value="4">
  <div class="row"><label for="spd">Scroll speed</label><output id="spdv">5</output></div>
  <input type="range" id="spd" name="spd" min="1" max="10" value="5">
 </div>
 <button type="submit">Update display</button>
 <div id="st"></div>
</form>
</main>
<script>
(function(){
var $=function(i){return document.getElementById(i)},f=$('f'),st=$('st');
function cnt(){$('cnt').textContent=$('msg').value.length+'/160'}
function out(){$('briv').textContent=$('bri').value;$('spdv').textContent=$('spd').value}
function fill(s){$('msg').value=s.msg;$('bri').value=s.bri;$('spd').value=s.spd;
 f.querySelector('input[name=mode][value="'+s.mode+'"]').checked=true;cnt();out()}
function send(p,msg){p.append('js','1');st.textContent='Sending…';
 fetch('/set',{method:'POST',body:p}).then(function(r){return r.json()})
 .then(function(s){if(msg)fill(s);st.textContent='Saved ✓'})
 .catch(function(){st.textContent='Could not reach the board'})}
$('msg').addEventListener('input',cnt);
$('bri').addEventListener('input',out);$('spd').addEventListener('input',out);
// Brightness & speed apply live when the slider is released.
['bri','spd'].forEach(function(id){$(id).addEventListener('change',function(){
 var p=new URLSearchParams();p.append(id,this.value);send(p,false)})});
f.addEventListener('submit',function(e){e.preventDefault();send(new URLSearchParams(new FormData(f)),true)});
fetch('/api/state').then(function(r){return r.json()}).then(fill).catch(function(){});
})();
</script></body></html>)rawliteral";

// ============================================================================
//  Helpers
// ============================================================================

// Map the 1..10 UI speed to an MD_Parola frame delay in ms (lower = faster).
static uint16_t frameDelayForSpeed(uint8_t speed) {
  speed = constrain(speed, SPEED_MIN, SPEED_MAX);
  return 160 - (uint16_t)speed * 14;  // 1 -> 146 ms ... 10 -> 20 ms
}

// Copy user input into `dst`, keeping only printable ASCII (what the built-in
// font can render). Line breaks/tabs become spaces; each multi-byte UTF-8
// character becomes a single '?'.
static void sanitizeMessage(const char *src, char *dst, size_t dstSize) {
  size_t o = 0;
  for (const uint8_t *p = (const uint8_t *)src; *p && o + 1 < dstSize; ++p) {
    uint8_t c = *p;
    if (c >= 0x20 && c <= 0x7E) {
      dst[o++] = (char)c;
    } else if (c == '\n' || c == '\r' || c == '\t') {
      if (o > 0 && dst[o - 1] != ' ') dst[o++] = ' ';
    } else if (c >= 0xC0) {        // UTF-8 lead byte
      dst[o++] = '?';
    }                              // continuation bytes / other control: drop
  }
  dst[o] = '\0';
}

static void loadDefaults() {
  memset(&settings, 0, sizeof(settings));
  settings.magic      = SETTINGS_MAGIC;
  settings.version    = SETTINGS_VERSION;
  settings.brightness = 4;
  settings.speed      = 5;
  settings.mode       = MODE_SCROLL;
  strlcpy(settings.message, DEFAULT_MESSAGE, sizeof(settings.message));
}

static bool loadSettings() {
  File f = LittleFS.open(SETTINGS_PATH, "r");
  if (!f) return false;

  Settings tmp;
  bool ok = (f.size() == sizeof(tmp)) &&
            (f.read((uint8_t *)&tmp, sizeof(tmp)) == sizeof(tmp));
  f.close();

  if (!ok || tmp.magic != SETTINGS_MAGIC || tmp.version != SETTINGS_VERSION)
    return false;

  // Never trust what came off flash: clamp and terminate.
  tmp.message[MSG_MAX_LEN] = '\0';
  tmp.brightness = min<uint8_t>(tmp.brightness, 15);
  tmp.speed      = constrain(tmp.speed, SPEED_MIN, SPEED_MAX);
  if (tmp.mode > MODE_STATIC) tmp.mode = MODE_SCROLL;

  settings = tmp;
  return true;
}

static bool saveSettings() {
  // LittleFS is copy-on-write: the old file stays intact until close(), so a
  // power cut mid-write does not corrupt the previous settings.
  File f = LittleFS.open(SETTINGS_PATH, "w");
  if (!f) return false;
  size_t n = f.write((const uint8_t *)&settings, sizeof(settings));
  f.close();
  return n == sizeof(settings);
}

static void markDirty() {
  settingsDirty = true;
  lastChangeMs  = millis();
}

// (Re)start the text animation from `settings`.
static void startText() {
  strlcpy(displayBuf, settings.message, sizeof(displayBuf));
  matrix.displayClear();

  if (settings.mode == MODE_STATIC) {
    // Print once, centred, and leave it on screen.
    matrix.displayText(displayBuf, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  } else {
    // Continuous right-to-left scroll, no pause between passes.
    matrix.displayText(displayBuf, PA_LEFT, frameDelayForSpeed(settings.speed),
                       0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  }
}

// ============================================================================
//  HTTP handlers
// ============================================================================

// Reply with the current settings as JSON. Messages are sanitised to printable
// ASCII, so only '"' and '\' need escaping (worst case 2x length).
static void sendState() {
  char json[2 * MSG_MAX_LEN + 64];
  size_t o = 0;
  o += snprintf(json + o, sizeof(json) - o, "{\"msg\":\"");
  for (const char *p = settings.message; *p && o + 2 < sizeof(json) - 40; ++p) {
    if (*p == '"' || *p == '\\') json[o++] = '\\';
    json[o++] = *p;
  }
  snprintf(json + o, sizeof(json) - o, "\",\"bri\":%u,\"spd\":%u,\"mode\":%u}",
           settings.brightness, settings.speed, settings.mode);

  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), json);
}

static void handleRoot() {
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send_P(200, "text/html", INDEX_HTML);
}

// POST /set   fields (all optional): msg, mode, bri, spd, js
//   js=1 -> JSON reply (used by the page's fetch())
//   else -> 303 redirect back to "/" (plain form post without JavaScript)
static void handleSet() {
  bool restart = false;

  if (server.hasArg("msg")) {
    char clean[MSG_MAX_LEN + 1];
    sanitizeMessage(server.arg("msg").c_str(), clean, sizeof(clean));
    if (strcmp(clean, settings.message) != 0) {
      strlcpy(settings.message, clean, sizeof(settings.message));
      restart = true;
    }
  }

  if (server.hasArg("mode")) {
    uint8_t m = server.arg("mode").toInt() == MODE_STATIC ? MODE_STATIC : MODE_SCROLL;
    if (m != settings.mode) {
      settings.mode = m;
      restart = true;
    }
  }

  if (server.hasArg("bri")) {
    settings.brightness = constrain(server.arg("bri").toInt(), 0, 15);
    matrix.setIntensity(settings.brightness);       // applies instantly
  }

  if (server.hasArg("spd")) {
    settings.speed = constrain(server.arg("spd").toInt(), SPEED_MIN, SPEED_MAX);
    if (settings.mode == MODE_SCROLL)
      matrix.setSpeed(frameDelayForSpeed(settings.speed));  // no restart needed
  }

  if (restart) startText();
  markDirty();

  Serial.printf("[web] mode=%u bri=%u spd=%u msg=\"%s\"\n", settings.mode,
                settings.brightness, settings.speed, settings.message);

  if (server.hasArg("js")) {
    sendState();
  } else {
    server.sendHeader(F("Location"), F("/"));
    server.send(303);
  }
}

// Captive portal: every unknown URL (including the OS connectivity checks
// such as /generate_204, /hotspot-detect.html, /connecttest.txt) is
// redirected to the control page, which makes the phone show it.
static void handleNotFound() {
  server.sendHeader(F("Location"), F("http://192.168.4.1/"));
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(302, F("text/plain"), "");
}

// ============================================================================
//  Setup & loop
// ============================================================================

static void setupFileSystem() {
  if (!LittleFS.begin()) {
    Serial.println(F("[fs] mount failed, formatting..."));
    LittleFS.format();
    LittleFS.begin();
  }
  if (loadSettings()) {
    Serial.println(F("[fs] settings restored"));
  } else {
    Serial.println(F("[fs] no valid settings, using defaults"));
    loadDefaults();
    saveSettings();
  }
  Serial.printf("[fs] mode=%u bri=%u spd=%u len=%u msg=\"%s\"\n", settings.mode,
                settings.brightness, settings.speed,
                (unsigned)strlen(settings.message), settings.message);
}

static void setupAccessPoint() {
  WiFi.persistent(false);          // don't wear flash with Wi-Fi config writes
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);

  // softAP requires a password of at least 8 chars; otherwise run open.
  const char *pass = (strlen(AP_PASSWORD) >= 8) ? AP_PASSWORD : nullptr;
  bool ok = WiFi.softAP(AP_SSID, pass, AP_CHANNEL);

  Serial.printf("[wifi] AP \"%s\" %s, IP %s\n", AP_SSID, ok ? "up" : "FAILED",
                WiFi.softAPIP().toString().c_str());

  // Answer every DNS query with our own IP -> captive portal.
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", AP_IP);
}

static void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/state", HTTP_GET, sendState);
  server.on("/set", HTTP_POST, handleSet);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println(F("[web] server started on port 80"));
}

void setup() {
  Serial.begin(115200);
  Serial.println(F("\n\nMatrixBoard starting"));

  matrix.begin();
  matrix.setIntensity(0);
  matrix.displayClear();

  setupFileSystem();

  matrix.setIntensity(settings.brightness);
  startText();

  setupAccessPoint();
  setupWebServer();
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  // Non-blocking animation: returns true when the current pass has finished.
  if (matrix.displayAnimate()) {
    if (settings.mode == MODE_SCROLL) matrix.displayReset();  // loop forever
    // Static mode: text stays on screen; nothing to do.
  }

  // Debounced persistence: write to flash only after changes settle.
  if (settingsDirty && (millis() - lastChangeMs >= SAVE_DELAY_MS)) {
    settingsDirty = false;
    Serial.println(saveSettings() ? F("[fs] settings saved")
                                  : F("[fs] save FAILED"));
  }
}

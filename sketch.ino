#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_system.h>

static const uint8_t PIN_LED         = 1;
static const uint8_t PIN_BUZZER      = 25;
static const uint8_t PIN_BOOT_BUTTON = 0;

static const uint32_t LED_BLINK_MS       = 250;
static const uint32_t BUZZER_ON_MS       = 150;
static const uint32_t BUZZER_OFF_MS      = 850;
static const uint32_t FACTORY_HOLD_MS    = 5000;
static const uint32_t STA_RECHECK_MS     = 15000;
static const uint32_t BUTTON_DEBOUNCE_MS = 30;

static const uint32_t CONFIG_MAGIC        = 0x45535031;
static const uint16_t CONFIG_VERSION      = 2;
static const uint16_t CONFIG_VERSION_PREV = 1;
static const char*    NVS_NAMESPACE       = "espu";
static const char*    NVS_KEY             = "cfg";

struct __attribute__((packed)) Config {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;

  char     ap_ssid[33];
  char     ap_pass[65];
  uint8_t  ap_channel;
  uint8_t  ap_txpower;
  uint8_t  ap_hidden;
  uint8_t  ap_maxconn;
  uint8_t  ap_authmode;

  char     sta_ssid[33];
  char     sta_pass[65];
  char     sta_hostname[33];
  uint8_t  sta_dhcp;
  uint32_t sta_ip;
  uint32_t sta_gw;
  uint32_t sta_mask;
  uint8_t  sta_mac[6];
  uint8_t  sta_mac_custom;

  uint32_t crc;
};

static Config      g_cfg;
static Preferences g_prefs;

static WebServer g_server(80);
static DNSServer g_dns;

static uint32_t g_lastStaCheck     = 0;
static uint32_t g_lastLedToggle    = 0;
static bool     g_ledState         = false;
static uint32_t g_lastBuzzerToggle = 0;
static bool     g_buzzerState      = false;

static bool     g_btnStable         = false;
static bool     g_btnLastRead       = false;
static uint32_t g_btnLastChange     = 0;
static uint32_t g_btnPressStart     = 0;
static bool     g_btnCountingHold   = false;

static bool     g_dnsStarted        = false;
static bool     g_apPendingApply    = false;
static uint32_t g_apApplyAt         = 0;
static bool     g_pendingStaApply   = false;
static uint32_t g_staApplyAt        = 0;
static bool     g_staReapplyPending = false;
static uint32_t g_staReapplyAt      = 0;
static uint32_t g_staDownSince      = 0;
static uint8_t  g_hwStaMac[6]       = {0,0,0,0,0,0};
static bool     g_factoryResetPending = false;

static uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      uint32_t m = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & m);
    }
  }
  return ~crc;
}

static uint32_t configCrc(const Config& c) {
  return crc32(reinterpret_cast<const uint8_t*>(&c), sizeof(Config) - sizeof(uint32_t));
}

static void safeCopy(char* dst, size_t dstLen, const char* src) {
  if (!dst || dstLen == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  size_t n = strlen(src);
  if (n >= dstLen) n = dstLen - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static void safeCopyStr(char* dst, size_t dstLen, const String& src) {
  safeCopy(dst, dstLen, src.c_str());
}

static String htmlEsc(const char* s) {
  String out; out.reserve(64);
  if (!s) return out;
  for (const char* p = s; *p; p++) {
    switch (*p) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += *p;
    }
  }
  return out;
}

static bool validCustomMac(const uint8_t m[6]) {
  if (m[0]==0 && m[1]==0 && m[2]==0 && m[3]==0 && m[4]==0 && m[5]==0) return false;
  if (m[0] & 0x01) return false;
  return true;
}

static void applyDefaults(Config& c) {
  memset(&c, 0, sizeof(c));
  c.magic   = CONFIG_MAGIC;
  c.version = CONFIG_VERSION;

  safeCopy(c.ap_ssid, sizeof(c.ap_ssid), "ESP32-Setup");
  safeCopy(c.ap_pass, sizeof(c.ap_pass), "12345678");
  c.ap_channel  = 1;
  c.ap_txpower  = 20;
  c.ap_hidden   = 0;
  c.ap_maxconn  = 4;
  c.ap_authmode = 1;

  safeCopy(c.sta_ssid, sizeof(c.sta_ssid), "");
  safeCopy(c.sta_pass, sizeof(c.sta_pass), "");
  safeCopy(c.sta_hostname, sizeof(c.sta_hostname), "esp32-unified");
  c.sta_dhcp = 1;
  c.sta_ip = c.sta_gw = c.sta_mask = 0;
  memset(c.sta_mac, 0, sizeof(c.sta_mac));
  c.sta_mac_custom = 0;

  c.crc = configCrc(c);
}

static bool migrateV1(Config* c) {
  if (c->ap_channel < 1 || c->ap_channel > 13) c->ap_channel = 1;
  if (c->ap_txpower < 5 || c->ap_txpower > 20) c->ap_txpower = 20;
  if (c->ap_maxconn < 1 || c->ap_maxconn > 8)  c->ap_maxconn = 4;
  if (c->ap_authmode > 1) c->ap_authmode = 1;
  if (c->sta_dhcp > 1)    c->sta_dhcp = 1;
  if (c->sta_mac_custom > 1) c->sta_mac_custom = 0;
  c->version = CONFIG_VERSION;
  c->crc = configCrc(*c);
  return true;
}

static bool loadConfig() {
  g_prefs.begin(NVS_NAMESPACE, false);
  size_t got = g_prefs.getBytesLength(NVS_KEY);
  bool ok = false;
  bool migrated = false;
  if (got == sizeof(Config)) {
    if (g_prefs.getBytes(NVS_KEY, &g_cfg, sizeof(Config)) == sizeof(Config)) {
      if (g_cfg.magic == CONFIG_MAGIC) {
        if (g_cfg.version == CONFIG_VERSION_PREV &&
            g_cfg.crc == configCrc(g_cfg)) {
          if (migrateV1(&g_cfg)) { ok = true; migrated = true; }
        } else if (g_cfg.version == CONFIG_VERSION &&
                   g_cfg.crc == configCrc(g_cfg) &&
                   g_cfg.ap_channel >= 1 && g_cfg.ap_channel <= 13 &&
                   g_cfg.ap_txpower >= 5 && g_cfg.ap_txpower <= 20 &&
                   g_cfg.ap_maxconn >= 1 && g_cfg.ap_maxconn <= 8 &&
                   g_cfg.ap_authmode <= 1 &&
                   g_cfg.sta_dhcp <= 1 &&
                   g_cfg.sta_mac_custom <= 1) {
          ok = true;
        }
      }
    }
  }
  g_prefs.end();
  if (!ok) {
    applyDefaults(g_cfg);
    g_prefs.begin(NVS_NAMESPACE, false);
    g_prefs.putBytes(NVS_KEY, &g_cfg, sizeof(Config));
    g_prefs.end();
  } else if (migrated) {
    g_prefs.begin(NVS_NAMESPACE, false);
    g_prefs.putBytes(NVS_KEY, &g_cfg, sizeof(Config));
    g_prefs.end();
  }
  return ok;
}

static bool saveConfigIfChanged(const Config& candidate) {
  Config copy = candidate;
  copy.magic   = CONFIG_MAGIC;
  copy.version = CONFIG_VERSION;
  copy.crc     = configCrc(copy);

  if (memcmp(&copy, &g_cfg, sizeof(Config)) == 0) return false;

  g_prefs.begin(NVS_NAMESPACE, false);
  size_t w = g_prefs.putBytes(NVS_KEY, &copy, sizeof(Config));
  g_prefs.end();
  if (w == sizeof(Config)) {
    memcpy(&g_cfg, &copy, sizeof(Config));
    return true;
  }
  return false;
}

static uint32_t ipToU32(const String& s) {
  IPAddress a;
  if (!a.fromString(s)) return 0;
  return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) |
         ((uint32_t)a[2] << 8)  |  (uint32_t)a[3];
}

static String u32ToIp(uint32_t v) {
  IPAddress a((uint8_t)(v >> 24), (uint8_t)(v >> 16),
              (uint8_t)(v >> 8),  (uint8_t)v);
  return a.toString();
}

static void applyAP() {
  if (WiFi.getMode() != WIFI_AP_STA && WiFi.getMode() != WIFI_AP) {
    WiFi.mode(WIFI_AP_STA);
  }

  wifi_config_t apc;
  memset(&apc, 0, sizeof(apc));
  safeCopy(reinterpret_cast<char*>(apc.ap.ssid), sizeof(apc.ap.ssid), g_cfg.ap_ssid);
  apc.ap.ssid_len = strlen(g_cfg.ap_ssid);
  safeCopy(reinterpret_cast<char*>(apc.ap.password), sizeof(apc.ap.password), g_cfg.ap_pass);
  apc.ap.channel  = g_cfg.ap_channel ? g_cfg.ap_channel : 1;
  apc.ap.max_connection = g_cfg.ap_maxconn ? g_cfg.ap_maxconn : 4;
  apc.ap.authmode = (g_cfg.ap_authmode == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  apc.ap.ssid_hidden = g_cfg.ap_hidden ? 1 : 0;

  uint8_t dbm = g_cfg.ap_txpower ? g_cfg.ap_txpower : 20;
  if (dbm < 5)  dbm = 5;
  if (dbm > 20) dbm = 20;

  esp_wifi_stop();
  esp_wifi_set_config(WIFI_IF_AP, &apc);
  esp_wifi_start();
  esp_wifi_set_max_tx_power(dbm * 4);

  if (strlen(g_cfg.sta_ssid) > 0) {
    WiFi.begin(g_cfg.sta_ssid, g_cfg.sta_pass);
  }

  IPAddress apIp = WiFi.softAPIP();
  if (!g_dnsStarted && apIp != IPAddress(0, 0, 0, 0)) {
    g_dns.setErrorReplyCode(DNSReplyCode::NoError);
    if (g_dns.start(53, "*", apIp)) {
      g_dnsStarted = true;
    }
  }
}

static void doApplySTAConfig();

static void applySTA() {
  if (strlen(g_cfg.sta_ssid) == 0) {
    WiFi.disconnect(false, false);
    g_staDownSince = 0;
    return;
  }
  WiFi.disconnect(false, false);
  g_staReapplyPending = true;
  g_staReapplyAt = millis() + 100;
}

static void doApplySTAConfig() {
  if (g_cfg.sta_mac_custom && validCustomMac(g_cfg.sta_mac)) {
    esp_wifi_set_mac(WIFI_IF_STA, g_cfg.sta_mac);
  } else if (g_hwStaMac[0] || g_hwStaMac[1] || g_hwStaMac[2] ||
             g_hwStaMac[3] || g_hwStaMac[4] || g_hwStaMac[5]) {
    esp_wifi_set_mac(WIFI_IF_STA, g_hwStaMac);
  }
  if (strlen(g_cfg.sta_hostname)) {
    WiFi.setHostname(g_cfg.sta_hostname);
  }
  if (!g_cfg.sta_dhcp && g_cfg.sta_ip && g_cfg.sta_gw && g_cfg.sta_mask) {
    IPAddress ip(g_cfg.sta_ip >> 24, g_cfg.sta_ip >> 16,
                 g_cfg.sta_ip >> 8,  g_cfg.sta_ip);
    IPAddress gw(g_cfg.sta_gw >> 24, g_cfg.sta_gw >> 16,
                 g_cfg.sta_gw >> 8,  g_cfg.sta_gw);
    IPAddress mask(g_cfg.sta_mask >> 24, g_cfg.sta_mask >> 16,
                   g_cfg.sta_mask >> 8,  g_cfg.sta_mask);
    WiFi.config(ip, gw, mask);
  } else {
    WiFi.config(IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(0,0,0,0));
  }
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(g_cfg.sta_ssid, g_cfg.sta_pass);
  g_lastStaCheck = millis();
  g_staDownSince = 0;
}

static void factoryResetWipe() {
  g_prefs.begin(NVS_NAMESPACE, false);
  g_prefs.clear();
  g_prefs.end();
  applyDefaults(g_cfg);
  g_prefs.begin(NVS_NAMESPACE, false);
  g_prefs.putBytes(NVS_KEY, &g_cfg, sizeof(Config));
  g_prefs.end();
}

static void factoryResetApply() {
  WiFi.disconnect(true, true);
  delay(50);

  g_dns.stop();
  g_dnsStarted = false;
  applyAP();
  applySTA();
}

static void factoryReset() {
  factoryResetWipe();
  factoryResetApply();
}

static void apScheduleApply() {
  g_apPendingApply = true;
  g_apApplyAt = millis() + 500;
}

static const char* HTML_HEAD =
  "<!doctype html><html><head><meta charset='utf-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>ESP32 Unified</title>"
  "<style>"
  "body{font-family:system-ui,Arial,sans-serif;margin:0;padding:0;background:#0f172a;color:#e2e8f0}"
  ".nav{display:flex;flex-wrap:wrap;gap:8px;padding:12px;background:#1e293b}"
  ".nav a{color:#93c5fd;text-decoration:none;padding:6px 12px;border-radius:8px;background:#0f172a}"
  ".nav a:hover{background:#334155}"
  ".wrap{max-width:820px;margin:0 auto;padding:16px}"
  "h1{font-size:1.3rem}label{display:block;margin:10px 0 4px}"
  "input,select{width:100%;box-sizing:border-box;padding:8px;border-radius:8px;border:1px solid #334155;background:#0f172a;color:#e2e8f0}"
  "button{margin-top:16px;padding:10px 18px;border-radius:8px;border:0;background:#2563eb;color:#fff;font-weight:600}"
  ".ok{background:#14532d;padding:10px;border-radius:8px;margin:10px 0}"
  ".err{background:#7f1d1d;padding:10px;border-radius:8px;margin:10px 0}"
  ".row{display:flex;gap:8px;flex-wrap:wrap}.row>*{flex:1 1 200px}"
  "small{color:#94a3b8}"
  "</style></head><body>"
  "<div class='nav'>"
  "<a href='/'>Dashboard</a>"
  "<a href='/ap'>AP Settings</a>"
  "<a href='/sta'>STA Settings</a>"
  "<a href='/reset' onclick=\"return confirm('Factory reset?')\">Factory Reset</a>"
  "</div><div class='wrap'>";

static const char* HTML_FOOT = "</div></body></html>";

static String pageDashboard(const String& msg, bool err) {
  String h; h.reserve(2600);
  h += HTML_HEAD;
  h += "<h1>ESP32 Unified Dashboard</h1>";
  if (msg.length()) h += err ? "<div class='err'>" + htmlEsc(msg.c_str()) + "</div>"
                             : "<div class='ok'>"  + htmlEsc(msg.c_str()) + "</div>";
  h += "<h3>Access Point (always on)</h3><ul>";
  h += "<li>SSID: <b>" + htmlEsc(g_cfg.ap_ssid) + "</b>";
  h += g_cfg.ap_hidden ? " (hidden)" : "";
  h += "</li><li>IP: <b>" + WiFi.softAPIP().toString() + "</b></li>";
  h += "<li>Channel: " + String(g_cfg.ap_channel) + "</li>";
  h += "<li>Clients: " + String(WiFi.softAPgetStationNum()) + "</li></ul>";

  h += "<h3>Station (always on)</h3><ul>";
  h += "<li>SSID: <b>" + htmlEsc(g_cfg.sta_ssid) + "</b></li>";
  h += "<li>Status: <b>";
  h += (WiFi.status() == WL_CONNECTED) ? "CONNECTED" : "DISCONNECTED";
  h += "</b></li>";
  if (WiFi.status() == WL_CONNECTED) {
    h += "<li>IP: " + WiFi.localIP().toString() + "</li>";
    h += "<li>RSSI: " + String(WiFi.RSSI()) + " dBm</li>";
  }
  h += "<li>Hostname: " + htmlEsc(g_cfg.sta_hostname) + "</li>";
  h += "<li>Mode: " + String(g_cfg.sta_dhcp ? "DHCP" : "Static") + "</li></ul>";

  h += "<h3>System</h3><ul>";
  h += "<li>Uptime: " + String(millis() / 1000UL) + " s</li>";
  h += "<li>Free heap: " + String(ESP.getFreeHeap()) + " bytes</li>";
  h += "<li>Min free heap: " + String(ESP.getMinFreeHeap()) + " bytes</li>";
  h += "</ul>";
  h += HTML_FOOT;
  return h;
}

static String pageAP(const String& msg, bool err) {
  String h; h.reserve(3600);
  h += HTML_HEAD;
  h += "<h1>Access Point Settings</h1>";
  if (msg.length()) h += err ? "<div class='err'>" + htmlEsc(msg.c_str()) + "</div>"
                             : "<div class='ok'>"  + htmlEsc(msg.c_str()) + "</div>";
  h += "<form method='POST' action='/ap' autocomplete='off'>";
  h += "<label>SSID</label><input name='ssid' maxlength='32' value='" + htmlEsc(g_cfg.ap_ssid) + "'>";
  h += "<label>Password (blank = unchanged, empty for open)</label><input type='password' name='pass' maxlength='64' value='' autocomplete='new-password' autocapitalize='off' spellcheck='false'>";
  h += "<small>Changing channel will disconnect all AP clients.</small>";
  h += "<label>Channel (1-13)</label><input name='ch' type='number' min='1' max='13' value='" + String(g_cfg.ap_channel) + "'>";
  h += "<label>TX Power (dBm, 5-20)</label><input name='txp' type='number' min='5' max='20' value='" + String(g_cfg.ap_txpower) + "'>";
  h += "<label>Max clients (1-8)</label><input name='mc' type='number' min='1' max='8' value='" + String(g_cfg.ap_maxconn) + "'>";
  h += "<label>Hidden SSID</label><select name='hid'>";
  h += "<option value='0'" + String(g_cfg.ap_hidden ? "" : " selected") + ">No</option>";
  h += "<option value='1'" + String(g_cfg.ap_hidden ? " selected" : "") + ">Yes</option>";
  h += "</select>";
  h += "<label>Auth</label><select name='auth'>";
  h += "<option value='0'" + String(g_cfg.ap_authmode == 0 ? " selected" : "") + ">OPEN</option>";
  h += "<option value='1'" + String(g_cfg.ap_authmode == 1 ? " selected" : "") + ">WPA2-PSK</option>";
  h += "</select>";
  h += "<button type='submit'>Save &amp; Apply</button></form>";
  h += HTML_FOOT;
  return h;
}

static String pageSTA(const String& msg, bool err) {
  String h; h.reserve(3800);
  h += HTML_HEAD;
  h += "<h1>Station Settings</h1>";
  if (msg.length()) h += err ? "<div class='err'>" + htmlEsc(msg.c_str()) + "</div>"
                             : "<div class='ok'>"  + htmlEsc(msg.c_str()) + "</div>";
  h += "<form method='POST' action='/sta' autocomplete='off'>";
  h += "<label>SSID</label><input name='ssid' maxlength='32' value='" + htmlEsc(g_cfg.sta_ssid) + "'>";
  h += "<label>Password (blank = unchanged)</label><input type='password' name='pass' maxlength='64' value='' autocomplete='new-password' autocapitalize='off' spellcheck='false'>";
  h += "<label>Hostname</label><input name='host' maxlength='32' value='" + htmlEsc(g_cfg.sta_hostname) + "'>";
  h += "<label>IP mode</label><select name='dhcp'>";
  h += "<option value='1'" + String(g_cfg.sta_dhcp ? " selected" : "") + ">DHCP</option>";
  h += "<option value='0'" + String(g_cfg.sta_dhcp ? "" : " selected") + ">Static</option>";
  h += "</select>";
  h += "<div class='row'>";
  h += "<div><label>Static IP</label><input name='ip' value='" + u32ToIp(g_cfg.sta_ip) + "'></div>";
  h += "<div><label>Gateway</label><input name='gw' value='" + u32ToIp(g_cfg.sta_gw) + "'></div>";
  h += "<div><label>Subnet mask</label><input name='mask' value='" + u32ToIp(g_cfg.sta_mask) + "'></div>";
  h += "</div>";
  h += "<label>Custom MAC (blank = hardware MAC)</label>";
  char macStr[18] = {0};
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           g_cfg.sta_mac[0], g_cfg.sta_mac[1], g_cfg.sta_mac[2],
           g_cfg.sta_mac[3], g_cfg.sta_mac[4], g_cfg.sta_mac[5]);
  h += "<input name='mac' value='";
  if (g_cfg.sta_mac_custom) h += macStr;
  h += "'>";
  h += "<button type='submit'>Save &amp; Reconnect</button></form>";
  h += HTML_FOOT;
  return h;
}

static void handleCaptiveProbe() {
  IPAddress apIp = WiFi.softAPIP();
  if (apIp == IPAddress(0, 0, 0, 0)) {
    g_server.send(503, "text/plain", "AP not ready");
    return;
  }
  g_server.sendHeader("Location",
    String("http://") + apIp.toString() + "/", true);
  g_server.send(302, "text/plain", "");
}

static void handle204() {
  g_server.send(204, "text/plain", "");
}

static void handleRoot() {
  g_server.send(200, "text/html", pageDashboard("", false));
}

static void handleAPGet() {
  g_server.send(200, "text/html", pageAP("", false));
}

static void handleAPPost() {
  Config candidate = g_cfg;
  String ssid = g_server.arg("ssid");
  String pass = g_server.arg("pass");
  long   ch   = g_server.arg("ch").toInt();
  long   txp  = g_server.arg("txp").toInt();
  long   mc   = g_server.arg("mc").toInt();
  long   hid  = g_server.arg("hid").toInt();
  long   auth = g_server.arg("auth").toInt();

  bool passBlank = (pass.length() == 0);
  String effectivePass = passBlank ? String(g_cfg.ap_pass) : pass;

  String err;
  if (ssid.length() == 0 || ssid.length() > 32) err = "SSID must be 1..32 chars";
  else if (auth == 1 && effectivePass.length() > 0 && effectivePass.length() < 8) err = "WPA2 password must be >=8 chars";
  else if (auth == 1 && effectivePass.length() == 0) err = "WPA2 requires password";
  else if (effectivePass.length() > 64) err = "Password too long";
  else if (ch < 1 || ch > 13) err = "Channel must be 1..13";
  else if (txp < 5 || txp > 20) err = "TX power must be 5..20 dBm";
  else if (mc < 1 || mc > 8) err = "Max clients must be 1..8";

  if (err.length()) {
    g_server.send(400, "text/html", pageAP(err, true));
    return;
  }

  safeCopyStr(candidate.ap_ssid, sizeof(candidate.ap_ssid), ssid);
  safeCopyStr(candidate.ap_pass, sizeof(candidate.ap_pass), effectivePass);
  candidate.ap_channel  = (uint8_t)ch;
  candidate.ap_txpower  = (uint8_t)txp;
  candidate.ap_maxconn  = (uint8_t)mc;
  candidate.ap_hidden   = (uint8_t)(hid ? 1 : 0);
  candidate.ap_authmode = (uint8_t)(auth ? 1 : 0);

  if (!saveConfigIfChanged(candidate)) {
    g_server.send(200, "text/html", pageAP("No changes.", false));
    return;
  }
  g_server.sendHeader("Location", "/ap", true);
  g_server.send(303, "text/plain", "");
  apScheduleApply();
}

static void handleSTAGet() {
  g_server.send(200, "text/html", pageSTA("", false));
}

static void handleSTAPost() {
  Config candidate = g_cfg;
  String ssid = g_server.arg("ssid");
  String pass = g_server.arg("pass");
  String host = g_server.arg("host");
  long   dhcp = g_server.arg("dhcp").toInt();
  String ipS  = g_server.arg("ip");
  String gwS  = g_server.arg("gw");
  String mkS  = g_server.arg("mask");
  String macS = g_server.arg("mac");

  bool passBlank = (pass.length() == 0);
  String effectivePass = passBlank ? String(g_cfg.sta_pass) : pass;

  String err;
  if (ssid.length() > 32) err = "SSID too long";
  else if (effectivePass.length() > 64) err = "Password too long";
  else if (host.length() > 32) err = "Hostname too long";
  else if (host.length() && host.indexOf(' ') >= 0) err = "Hostname has spaces";

  uint32_t ip = 0, gw = 0, mk = 0;
  if (!err.length() && dhcp == 0) {
    ip = ipToU32(ipS);
    gw = ipToU32(gwS);
    mk = ipToU32(mkS);
    if (!ip || !gw || !mk) {
      err = "Static IP/GW/Mask must be valid IPv4";
    } else if (ip == gw) {
      err = "IP cannot equal gateway";
    } else if ((ip & mk) != (gw & mk)) {
      err = "IP and gateway must be on the same subnet";
    }
  }

  uint8_t mac[6] = {0};
  bool useMac = false;
  if (!err.length() && macS.length()) {
    int v[6];
    if (sscanf(macS.c_str(), "%x:%x:%x:%x:%x:%x",
               &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) {
      err = "MAC must be XX:XX:XX:XX:XX:XX";
    } else {
      for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
      if (!validCustomMac(mac)) err = "MAC must be unicast and non-zero";
      else useMac = true;
    }
  }

  if (err.length()) {
    g_server.send(400, "text/html", pageSTA(err, true));
    return;
  }

  safeCopyStr(candidate.sta_ssid, sizeof(candidate.sta_ssid), ssid);
  safeCopyStr(candidate.sta_pass, sizeof(candidate.sta_pass), effectivePass);
  safeCopyStr(candidate.sta_hostname, sizeof(candidate.sta_hostname), host);
  candidate.sta_dhcp = (uint8_t)(dhcp ? 1 : 0);
  candidate.sta_ip   = ip;
  candidate.sta_gw   = gw;
  candidate.sta_mask = mk;
  memcpy(candidate.sta_mac, mac, 6);
  candidate.sta_mac_custom = useMac ? 1 : 0;

  if (!saveConfigIfChanged(candidate)) {
    g_server.send(200, "text/html", pageSTA("No changes.", false));
    return;
  }
  g_server.sendHeader("Location", "/sta", true);
  g_server.send(303, "text/plain", "");
  g_pendingStaApply = true;
  g_staApplyAt = millis() + 500;
}

static void handleResetGet() {
  String h = HTML_HEAD;
  h += "<h1>Factory Reset</h1>";
  h += "<p>Hold BOOT (GPIO0) for 5 seconds, or press the button below.</p>";
  h += "<form method='POST' action='/reset'>"
       "<button type='submit'>Confirm Factory Reset</button></form>";
  h += HTML_FOOT;
  g_server.send(200, "text/html", h);
}

static void handleResetPost() {
  factoryResetWipe();

  g_server.sendHeader("Location", "/", true);
  g_server.send(303, "text/plain", "");

  g_factoryResetPending = true;
  g_apPendingApply = true;
  g_apApplyAt = millis() + 500;
  g_pendingStaApply = true;
  g_staApplyAt = millis() + 500;
}

static void registerRoutes() {
  g_server.on("/",      HTTP_GET,  handleRoot);
  g_server.on("/ap",    HTTP_GET,  handleAPGet);
  g_server.on("/ap",    HTTP_POST, handleAPPost);
  g_server.on("/sta",   HTTP_GET,  handleSTAGet);
  g_server.on("/sta",   HTTP_POST, handleSTAPost);
  g_server.on("/reset", HTTP_GET,  handleResetGet);
  g_server.on("/reset", HTTP_POST, handleResetPost);

  g_server.on("/generate_204",              HTTP_GET, handle204);
  g_server.on("/gen_204",                   HTTP_GET, handle204);
  g_server.on("/hotspot-detect.html",       HTTP_GET, handleCaptiveProbe);
  g_server.on("/library/test/success.html", HTTP_GET, handleCaptiveProbe);
  g_server.on("/connecttest.txt",           HTTP_GET, handleCaptiveProbe);
  g_server.on("/ncsi.txt",                  HTTP_GET, handleCaptiveProbe);

  g_server.onNotFound(handleCaptiveProbe);
}

static void updateAlertIndicators() {
  bool disconnected = (WiFi.status() != WL_CONNECTED) &&
                      (strlen(g_cfg.sta_ssid) > 0);

  uint32_t now = millis();

  if (!disconnected) {
    if (g_ledState)    { digitalWrite(PIN_LED, LOW);    g_ledState = false; }
    if (g_buzzerState) { digitalWrite(PIN_BUZZER, LOW); g_buzzerState = false; }
    return;
  }

  if ((uint32_t)(now - g_lastLedToggle) >= LED_BLINK_MS) {
    g_lastLedToggle = now;
    g_ledState = !g_ledState;
    digitalWrite(PIN_LED, g_ledState ? HIGH : LOW);
  }

  uint32_t interval = g_buzzerState ? BUZZER_ON_MS : BUZZER_OFF_MS;
  if ((uint32_t)(now - g_lastBuzzerToggle) >= interval) {
    g_lastBuzzerToggle = now;
    g_buzzerState = !g_buzzerState;
    digitalWrite(PIN_BUZZER, g_buzzerState ? HIGH : LOW);
  }
}

static void updateBootButton() {
  bool raw = (digitalRead(PIN_BOOT_BUTTON) == LOW);
  uint32_t now = millis();

  if (raw != g_btnLastRead) {
    g_btnLastChange = now;
    g_btnLastRead = raw;
  }
  if ((uint32_t)(now - g_btnLastChange) >= BUTTON_DEBOUNCE_MS &&
      g_btnStable != raw) {
    g_btnStable = raw;
    if (g_btnStable) {
      g_btnPressStart   = now;
      g_btnCountingHold = true;
    } else {
      g_btnCountingHold = false;
    }
  }
  if (g_btnCountingHold && g_btnStable) {
    if ((uint32_t)(now - g_btnPressStart) >= FACTORY_HOLD_MS) {
      g_btnCountingHold = false;
      factoryReset();
    }
  }
}

static void updateSTAWatchdog() {
  if (strlen(g_cfg.sta_ssid) == 0) return;

  uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    g_staDownSince = 0;
    return;
  }

  if (g_staDownSince == 0) {
    g_staDownSince = now;
    return;
  }

  if ((uint32_t)(now - g_staDownSince) < 60000UL) return;
  g_staDownSince = now;

  WiFi.begin(g_cfg.sta_ssid, g_cfg.sta_pass);
}

void setup() {
  pinMode(PIN_LED, OUTPUT);          digitalWrite(PIN_LED, LOW);
  pinMode(PIN_BUZZER, OUTPUT);       digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

  loadConfig();

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);

  esp_wifi_get_mac(WIFI_IF_STA, g_hwStaMac);

  applyAP();
  applySTA();

  registerRoutes();
  g_server.begin();

  g_lastStaCheck     = millis();
  g_lastLedToggle    = millis();
  g_lastBuzzerToggle = millis();
}

void loop() {
  g_dns.processNextRequest();
  g_server.handleClient();
  updateAlertIndicators();
  updateBootButton();
  updateSTAWatchdog();

  if (g_apPendingApply && (int32_t)(millis() - g_apApplyAt) >= 0) {
    g_apPendingApply = false;
    if (g_factoryResetPending) {
      g_factoryResetPending = false;
      WiFi.disconnect(true, true);
      g_dns.stop();
      g_dnsStarted = false;
    }
    applyAP();
  }

  if (g_pendingStaApply && (int32_t)(millis() - g_staApplyAt) >= 0) {
    g_pendingStaApply = false;
    applySTA();
  }

  if (g_staReapplyPending && (int32_t)(millis() - g_staReapplyAt) >= 0) {
    g_staReapplyPending = false;
    doApplySTAConfig();
  }

  yield();
}

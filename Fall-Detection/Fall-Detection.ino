#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <MPU6050_tockn.h>
#include <ESP_Mail_Client.h>
#include <TinyGPS++.h>
#include <HardwareSerial.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ─── WiFi Credentials ────────────────────────────────────────────────────────
const char* ssid     = 
const char* password = 

// ─── Email ────────────────────────────────────────────────────────────────────
#define SENDER_EMAIL    
#define SENDER_PASSWORD 
#define RECEIVER_EMAIL  

// ─── Telegram ─────────────────────────────────────────────────────────────────
#define TELEGRAM_BOT_TOKEN   // from @BotFather
#define TELEGRAM_CHAT_ID    // from @userinfobot

// ─── GPS ─────────────────────────────────────────────────────────────────────
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);
#define GPS_RX 18
#define GPS_TX 17

#define GPS_MIN_SATS     6
#define GPS_MIN_HDOP     1.5f
#define GPS_AGE_LIMIT    3000

// ─── Pins ─────────────────────────────────────────────────────────────────────
#define BTN_SYSTEM   2
#define BTN_BUZZER   15
#define BUZZER_PIN   6
#define LED_FALL_PIN 5

// ─── Buzzer PWM Config ────────────────────────────────────────────────────────
#define BUZZER_CHANNEL  0
#define BUZZER_FREQ     2700
#define BUZZER_RES      8
#define BUZZER_VOL      200

// ─── OLED ─────────────────────────────────────────────────────────────────────
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET   -1
#define OLED_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ─── Fall Detection Thresholds ────────────────────────────────────────────────
#define FALL_LOW       0.5
#define FALL_HIGH      2.5
#define STARTUP_DELAY  5000
#define DEBOUNCE_DELAY 200

// ─── State ────────────────────────────────────────────────────────────────────
bool systemON      = false;
bool fallDetected  = false;
bool emailSending  = false;
bool buzzerON      = false;
bool ledON         = false;
bool lastSystemBtn = HIGH;
bool lastBuzzerBtn = HIGH;

unsigned long lastDebounceSystem = 0;
unsigned long lastDebounceBuzzer = 0;
unsigned long startTime          = 0;
unsigned long fallTimestampMs    = 0;

float lastAX = 0, lastAY = 0, lastAZ = 0;
float lastGX = 0, lastGY = 0, lastGZ = 0;
float lastTotalAcc = 1.0;

char fallISTTime[32] = "";

float lastKnownLat  = 0.0;
float lastKnownLng  = 0.0;
float lastKnownHdop = 99.9f;
int   lastKnownSats = 0;
bool  hadGpsFix     = false;

// ─── Buzzer Helpers ───────────────────────────────────────────────────────────
void buzzerOn() {
  ledcWrite(BUZZER_CHANNEL, BUZZER_VOL);
  buzzerON = true;
}

void buzzerOff() {
  ledcWrite(BUZZER_CHANNEL, 0);
  buzzerON = false;
}

// ─── IST Helper ───────────────────────────────────────────────────────────────
void getISTTimeString(char* buf, size_t len) {
  if (gps.time.isValid() && gps.date.isValid()) {
    int h = gps.time.hour() + 5;
    int m = gps.time.minute() + 30;
    int s = gps.time.second();
    if (m >= 60) { m -= 60; h += 1; }
    if (h >= 24)   h -= 24;
    snprintf(buf, len, "%02d:%02d:%02d IST", h, m, s);
  } else {
    unsigned long sec = millis() / 1000;
    snprintf(buf, len, "Uptime %02lu:%02lu:%02lu", sec/3600, (sec%3600)/60, sec%60);
  }
}

// ─── GPS Quality Check ────────────────────────────────────────────────────────
bool isGpsAccurate() {
  if (!gps.location.isValid()) return false;
  if (gps.location.age() > GPS_AGE_LIMIT) return false;
  if (gps.satellites.isValid() && gps.satellites.value() < GPS_MIN_SATS) return false;
  if (gps.hdop.isValid() && gps.hdop.hdop() > GPS_MIN_HDOP) return false;
  return true;
}

// ─── Telegram Send ────────────────────────────────────────────────────────────
void sendTelegram(String message) {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();  // skip SSL cert verification (fine for IoT)
  HTTPClient http;
  String url = "https://api.telegram.org/bot";
  url += TELEGRAM_BOT_TOKEN;
  url += "/sendMessage";
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  // Build JSON body
  String payload = "{\"chat_id\":\"" + String(TELEGRAM_CHAT_ID) +
                   "\",\"text\":\"" + message +
                   "\",\"parse_mode\":\"HTML\"}";
  int httpCode = http.POST(payload);
  if (httpCode > 0) Serial.println("Telegram sent: " + String(httpCode));
  else              Serial.println("Telegram failed: " + http.errorToString(httpCode));
  http.end();
}

void sendTelegramLocation(float lat, float lng) {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.telegram.org/bot";
  url += TELEGRAM_BOT_TOKEN;
  url += "/sendLocation";
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  String payload = "{\"chat_id\":\"" + String(TELEGRAM_CHAT_ID) +
                   "\",\"latitude\":" + String(lat, 7) +
                   ",\"longitude\":" + String(lng, 7) + "}";
  int httpCode = http.POST(payload);
  if (httpCode > 0) Serial.println("Telegram location sent: " + String(httpCode));
  else              Serial.println("Telegram location failed: " + http.errorToString(httpCode));
  http.end();
}

// ─── Forward Declarations ─────────────────────────────────────────────────────
void showOLED_SystemOff();
void showOLED_Fall();
void showOLED_Boot(String msg);
void showOLED_Normal(float ax, float ay, float az, float gx, float gy, float gz, float total);
void sendFallAlert(float lat, float lng, bool gpsValid);

// ─── Web Server & Sensors ─────────────────────────────────────────────────────
WebServer server(80);
MPU6050 mpu6050(Wire);
SMTPSession smtp;

// ─── HTML Page ────────────────────────────────────────────────────────────────
const char* htmlPage = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1.0"/>
<title>Fall Detection System</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800&display=swap');
  :root {
    --bg:     #07070f;
    --bg2:    #0e0e1a;
    --bg3:    #161625;
    --bg4:    #1e1e30;
    --border: #252538;
    --border2:#303048;
    --text:   #eeeef8;
    --muted:  #7878a0;
    --red:    #f04e4e;
    --red2:   #c0392b;
    --green:  #27d96a;
    --blue:   #4a8fff;
    --yellow: #ffb830;
    --orange: #ff7a30;
    --purple: #9b6dff;
    --cyan:   #30d4d4;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: 'Inter', -apple-system, sans-serif; background: var(--bg); color: var(--text); min-height: 100vh; }
  .header {
    background: linear-gradient(135deg, #110a1a 0%, #0a0a18 100%);
    border-bottom: 1px solid var(--border);
    padding: 18px 28px;
    display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 12px;
    position: sticky; top: 0; z-index: 100;
    backdrop-filter: blur(12px);
  }
  .header-left { display: flex; align-items: center; gap: 14px; }
  .header-icon {
    width: 44px; height: 44px; border-radius: 12px;
    background: linear-gradient(135deg, #c0392b, #f04e4e);
    display: flex; align-items: center; justify-content: center;
    font-size: 22px; box-shadow: 0 0 20px #f04e4e44;
  }
  .header-title { font-size: 17px; font-weight: 800; letter-spacing: -0.3px; }
  .header-sub   { font-size: 11px; color: var(--muted); margin-top: 2px; font-weight: 500; }
  .header-right { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
  .hbadge {
    background: var(--bg3); border: 1px solid var(--border);
    border-radius: 22px; padding: 6px 14px;
    font-size: 12px; color: var(--muted); font-weight: 500;
    display: flex; align-items: center; gap: 6px;
  }
  .hbadge b { color: var(--text); font-weight: 700; }
  .live-dot {
    width: 7px; height: 7px; background: var(--green);
    border-radius: 50%; animation: pulse-dot 2s infinite;
    box-shadow: 0 0 6px var(--green);
  }
  @keyframes pulse-dot { 0%,100%{opacity:1;transform:scale(1);} 50%{opacity:.5;transform:scale(.7);} }
  .fall-banner {
    background: linear-gradient(135deg, #3a0808, #6b1111);
    border-bottom: 2px solid var(--red);
    padding: 18px 28px; display: none;
    animation: pulse-banner 1.2s infinite alternate;
  }
  .fall-banner.visible { display: block; }
  @keyframes pulse-banner {
    from { background: linear-gradient(135deg,#3a0808,#6b1111); box-shadow: none; }
    to   { background: linear-gradient(135deg,#5a0e0e,#8b1515); box-shadow: 0 4px 30px #f04e4e33; }
  }
  .fall-banner-inner {
    max-width: 860px; margin: 0 auto;
    display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 14px;
  }
  .fall-banner-left { display: flex; align-items: center; gap: 16px; }
  .fall-icon { font-size: 36px; animation: shake .45s infinite; }
  @keyframes shake { 0%,100%{transform:translateX(0) rotate(0);} 25%{transform:translateX(-4px) rotate(-3deg);} 75%{transform:translateX(4px) rotate(3deg);} }
  .fall-banner-title { font-size: 22px; font-weight: 800; color: #fff; letter-spacing: -0.5px; }
  .fall-banner-ist   { font-size: 13px; color: #ffaaaa; margin-top: 4px; font-weight: 600; }
  .fall-banner-time  { font-size: 12px; color: #ff9999; margin-top: 2px; }
  .fall-banner-ago   { font-size: 12px; color: #ff8888; margin-top: 2px; font-weight: 600; }
  .maps-btn {
    background: linear-gradient(135deg,#1a56cc,#4a8fff);
    color: #fff; text-decoration: none;
    padding: 11px 20px; border-radius: 12px;
    font-size: 13px; font-weight: 700; display: inline-flex; align-items: center; gap: 6px;
    box-shadow: 0 4px 16px #4a8fff44;
  }
  .gps-na { color: #ff9999; font-size: 13px; font-weight: 600; }
  .main { max-width: 860px; margin: 0 auto; padding: 28px 20px; }
  .grid2 { display: grid; grid-template-columns: 1fr 1fr; gap: 18px; margin-bottom: 18px; }
  @media(max-width:620px){ .grid2 { grid-template-columns: 1fr; } }
  .card {
    background: var(--bg2); border: 1px solid var(--border);
    border-radius: 20px; padding: 22px; margin-bottom: 18px;
    transition: border-color .2s;
  }
  .card:hover { border-color: var(--border2); }
  .card-header { display: flex; align-items: center; gap: 10px; margin-bottom: 18px; }
  .card-icon {
    width: 32px; height: 32px; border-radius: 9px;
    display: flex; align-items: center; justify-content: center; font-size: 15px;
  }
  .card-title { font-size: 11px; font-weight: 700; color: var(--muted); text-transform: uppercase; letter-spacing: 1.2px; }
  .status-row {
    display: flex; align-items: center; justify-content: space-between;
    padding: 11px 0; border-bottom: 1px solid var(--border);
  }
  .status-row:last-child { border-bottom: none; }
  .status-left { display: flex; align-items: center; gap: 10px; }
  .dot { width: 8px; height: 8px; border-radius: 50%; flex-shrink: 0; }
  .dot.green  { background: var(--green);  box-shadow: 0 0 8px var(--green); }
  .dot.red    { background: var(--red);    box-shadow: 0 0 8px var(--red); animation: blink .55s infinite; }
  .dot.gray   { background: #333348; }
  .dot.yellow { background: var(--yellow); box-shadow: 0 0 8px var(--yellow); }
  .dot.orange { background: var(--orange); box-shadow: 0 0 8px var(--orange); }
  .dot.cyan   { background: var(--cyan);   box-shadow: 0 0 8px var(--cyan); }
  @keyframes blink { 0%,100%{opacity:1} 50%{opacity:.1} }
  .status-label { font-size: 14px; font-weight: 500; }
  .badge {
    padding: 3px 11px; border-radius: 20px; font-size: 10px; font-weight: 800;
    letter-spacing: 0.5px; text-transform: uppercase;
  }
  .badge-red    { background: #3a0a0a; color: var(--red);    border: 1px solid #6b1111; }
  .badge-green  { background: #0a2e18; color: var(--green);  border: 1px solid #1a5c34; }
  .badge-gray   { background: var(--bg3); color: var(--muted); border: 1px solid var(--border); }
  .badge-yellow { background: #2e1e00; color: var(--yellow); border: 1px solid #6b4400; }
  .badge-orange { background: #2e1200; color: var(--orange); border: 1px solid #6b2e00; }
  .badge-cyan   { background: #002e2e; color: var(--cyan);   border: 1px solid #006b6b; }
  .metric-grid { display: grid; grid-template-columns: repeat(3,1fr); gap: 10px; }
  .metric {
    background: var(--bg3); border: 1px solid var(--border);
    border-radius: 14px; padding: 14px 8px; text-align: center;
    transition: border-color .2s;
  }
  .metric:hover { border-color: var(--border2); }
  .metric-val { font-size: 22px; font-weight: 800; font-variant-numeric: tabular-nums; letter-spacing: -0.5px; }
  .metric-lbl { font-size: 10px; color: var(--muted); margin-top: 5px; text-transform: uppercase; letter-spacing: 0.8px; font-weight: 600; }
  .total-row {
    margin-top: 12px; background: var(--bg3); border: 1px solid var(--border);
    border-radius: 14px; padding: 14px 18px;
    display: flex; align-items: center; justify-content: space-between;
  }
  .total-label { font-size: 12px; color: var(--muted); font-weight: 600; }
  .total-val   { font-size: 24px; font-weight: 800; letter-spacing: -0.5px; }
  .gps-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 12px; }
  .gps-box {
    background: var(--bg3); border: 1px solid var(--border);
    border-radius: 14px; padding: 14px 16px;
    font-size: 12px; color: var(--muted); font-weight: 600;
  }
  .gps-box span { display: block; color: var(--text); font-weight: 700; font-size: 15px; margin-top: 4px; font-variant-numeric: tabular-nums; }
  .gps-meta { display: flex; gap: 10px; margin-bottom: 12px; }
  .gps-meta-item {
    flex: 1; background: var(--bg3); border: 1px solid var(--border);
    border-radius: 14px; padding: 10px 14px; font-size: 11px; color: var(--muted); font-weight: 600;
    text-align: center;
  }
  .gps-meta-item b { display: block; font-size: 16px; font-weight: 800; color: var(--text); margin-top: 2px; }
  .btn {
    display: block; width: 100%; padding: 14px 18px;
    font-size: 14px; font-weight: 700; border: none;
    border-radius: 14px; cursor: pointer;
    transition: transform .1s, opacity .1s, box-shadow .15s;
    letter-spacing: 0.2px; font-family: inherit;
  }
  .btn:active { transform: scale(0.96); opacity: .85; }
  .btn-primary {
    background: linear-gradient(135deg,#c0392b,#f04e4e);
    color: #fff; box-shadow: 0 4px 18px #f04e4e30; margin-bottom: 10px;
  }
  .btn-primary:hover { box-shadow: 0 6px 24px #f04e4e50; }
  .btn-ghost-red    { background: #1a0808; color: var(--red);    border: 1.5px solid #5a1111; margin-bottom: 10px; }
  .btn-ghost-green  { background: #081a10; color: var(--green);  border: 1.5px solid #115a2a; margin-bottom: 10px; }
  .btn-ghost-blue   { background: #08101a; color: var(--blue);   border: 1.5px solid #1a3a6b; margin-bottom: 10px; }
  .btn-ghost-yellow { background: #1a1208; color: var(--yellow); border: 1.5px solid #6b4a0e; margin-bottom: 10px; }
  .btn-ghost-cyan   { background: #001a1a; color: var(--cyan);   border: 1.5px solid #006b6b; margin-bottom: 10px; }
  .btn-ghost-red:hover, .btn-ghost-green:hover, .btn-ghost-blue:hover,
  .btn-ghost-yellow:hover, .btn-ghost-cyan:hover { filter: brightness(1.15); }
  .btn-power {
    background: var(--bg3); color: var(--text);
    border: 1.5px solid var(--border2); margin-bottom: 10px;
    font-size: 13px;
  }
  .btn-power:hover { border-color: var(--green); color: var(--green); }
  .btn-led-on  { background: linear-gradient(135deg,#5a3800,#ffb830); color: #1a0a00; margin-bottom: 10px; box-shadow: 0 0 20px #ffb83044; }
  .btn-led-off { background: var(--bg3); color: var(--muted); border: 1.5px solid #333348; margin-bottom: 10px; }
  .btn-led-off:hover { border-color: var(--yellow); color: var(--yellow); }
  .divider { border: none; border-top: 1px solid var(--border); margin: 14px 0; }
  .btn-map-full {
    display: flex; align-items: center; justify-content: center; gap: 8px;
    width: 100%; padding: 14px;
    background: linear-gradient(135deg,#1a3a8a,#4a8fff);
    color: #fff; text-decoration: none;
    border-radius: 14px; font-size: 14px; font-weight: 700;
    box-shadow: 0 4px 18px #4a8fff30; transition: box-shadow .2s;
  }
  .btn-map-full:hover { box-shadow: 0 6px 28px #4a8fff55; }
  .btn-map-last {
    display: flex; align-items: center; justify-content: center; gap: 8px;
    width: 100%; padding: 14px;
    background: linear-gradient(135deg,#5a3800,#ffb830);
    color: #1a0a00; text-decoration: none;
    border-radius: 14px; font-size: 14px; font-weight: 700;
    box-shadow: 0 4px 18px #ffb83030; transition: box-shadow .2s;
  }
  .btn-map-last:hover { box-shadow: 0 6px 28px #ffb83055; }
  .acc-bar-wrap { background: var(--bg4); border-radius: 6px; height: 6px; overflow: hidden; margin-top: 6px; }
  .acc-bar { height: 100%; border-radius: 6px; transition: width .6s, background .6s; }
  .api-box {
    background: var(--bg3); border: 1px solid var(--border);
    border-radius: 14px; padding: 12px 16px;
    font-size: 11px; color: var(--muted); word-break: break-all;
    font-family: 'Courier New', monospace; display: none; margin-top: 4px;
  }
  .api-box.visible { display: block; }
  .footer {
    text-align: center; padding: 22px;
    font-size: 11px; color: #33334a;
    border-top: 1px solid var(--border);
  }
  .footer span { color: var(--muted); }
  ::-webkit-scrollbar { width: 6px; }
  ::-webkit-scrollbar-track { background: var(--bg); }
  ::-webkit-scrollbar-thumb { background: var(--border2); border-radius: 3px; }
</style>
</head>
<body>

<div class="header">
  <div class="header-left">
    <div class="header-icon">&#128737;</div>
    <div>
      <div class="header-title">Fall Detection System</div>
      <div class="header-sub">ESP32 Wearable &mdash; Real-Time Monitor</div>
    </div>
  </div>
  <div class="header-right">
    <div class="hbadge"><div class="live-dot"></div> LIVE</div>
    <div class="hbadge">&#9201; <b id="uptimeVal">--:--:--</b></div>
  </div>
</div>

<div class="fall-banner" id="fallBanner">
  <div class="fall-banner-inner">
    <div class="fall-banner-left">
      <div class="fall-icon">&#128680;</div>
      <div>
        <div class="fall-banner-title">&#9888; Fall Detected!</div>
        <div class="fall-banner-ist"  id="fallBannerIST">&#8212;</div>
        <div class="fall-banner-time" id="fallBannerTime">&#8212;</div>
        <div class="fall-banner-ago"  id="fallBannerAgo"></div>
      </div>
    </div>
    <div id="fallBannerLocation"></div>
  </div>
</div>

<div class="main">
  <div class="grid2">
    <div class="card">
      <div class="card-header">
        <div class="card-icon" style="background:#141428;">&#128187;</div>
        <div class="card-title">System Status</div>
      </div>
      <div class="status-row">
        <div class="status-left"><div class="dot gray" id="sysDot"></div><span class="status-label">System Power</span></div>
        <span class="badge badge-gray" id="sysBadge">OFF</span>
      </div>
      <div class="status-row">
        <div class="status-left"><div class="dot gray" id="buzzDot"></div><span class="status-label">Buzzer</span></div>
        <span class="badge badge-gray" id="buzzBadge">SILENT</span>
      </div>
      <div class="status-row">
        <div class="status-left"><div class="dot gray" id="ledDot"></div><span class="status-label">LED Light</span></div>
        <span class="badge badge-gray" id="ledBadge">OFF</span>
      </div>
      <div class="status-row">
        <div class="status-left"><div class="dot gray" id="fallDot"></div><span class="status-label">Fall Status</span></div>
        <span class="badge badge-gray" id="fallBadge">NORMAL</span>
      </div>
      <div class="status-row">
        <div class="status-left"><div class="dot gray" id="emailDot"></div><span class="status-label">Email Alert</span></div>
        <span class="badge badge-gray" id="emailBadge">IDLE</span>
      </div>
      <div class="status-row">
        <div class="status-left"><div class="dot gray" id="tgDot"></div><span class="status-label">Telegram Alert</span></div>
        <span class="badge badge-gray" id="tgBadge">IDLE</span>
      </div>
      <div class="status-row">
        <div class="status-left"><div class="dot gray" id="gpsDot"></div><span class="status-label">GPS Signal</span></div>
        <span class="badge badge-gray" id="gpsBadge">NO FIX</span>
      </div>
      <hr class="divider">
      <button class="btn btn-power" id="sysBtn" onclick="toggleSystem()">&#9889; Power On System</button>
    </div>

    <div class="card">
      <div class="card-header">
        <div class="card-icon" style="background:#1a0808;">&#128276;</div>
        <div class="card-title">Controls</div>
      </div>
      <button class="btn btn-primary" id="buzzerBtn" onclick="toggleBuzzer()">&#128276; Ring Buzzer</button>
      <button class="btn btn-ghost-green" onclick="silenceBuzzer()">&#128263; Silence Buzzer</button>
      <button class="btn btn-led-off" id="ledBtn" onclick="toggleLED()">&#128161; Toggle LED</button>
      <hr class="divider">
      <div class="card-header" style="margin-bottom:12px;">
        <div class="card-icon" style="background:#08081a;">&#9889;</div>
        <div class="card-title">Emergency Actions</div>
      </div>
      <button class="btn btn-ghost-red"    onclick="triggerFall()">&#128680; Simulate Fall + Alerts</button>
      <button class="btn btn-ghost-blue"   onclick="testEmail()">&#128231; Send Test Email</button>
      <button class="btn btn-ghost-cyan"   onclick="testTelegram()">&#128240; Send Test Telegram</button>
      <button class="btn btn-ghost-yellow" onclick="silenceAll()">&#9989; Reset &amp; Silence All</button>
    </div>
  </div>

  <div class="grid2">
    <div class="card">
      <div class="card-header">
        <div class="card-icon" style="background:#0a1a0a;">&#128208;</div>
        <div class="card-title">Accelerometer (g)</div>
      </div>
      <div class="metric-grid">
        <div class="metric"><div class="metric-val" id="mAX" style="color:#4a8fff;">&#8212;</div><div class="metric-lbl">X Axis</div></div>
        <div class="metric"><div class="metric-val" id="mAY" style="color:#27d96a;">&#8212;</div><div class="metric-lbl">Y Axis</div></div>
        <div class="metric"><div class="metric-val" id="mAZ" style="color:#ff7a30;">&#8212;</div><div class="metric-lbl">Z Axis</div></div>
      </div>
      <div class="total-row">
        <span class="total-label">&#128200; Total Acceleration</span>
        <span class="total-val" id="mTotal">&#8212; g</span>
      </div>
    </div>
    <div class="card">
      <div class="card-header">
        <div class="card-icon" style="background:#1a0a1a;">&#127744;</div>
        <div class="card-title">Gyroscope (&deg;/s)</div>
      </div>
      <div class="metric-grid">
        <div class="metric"><div class="metric-val" id="mGX" style="color:#9b6dff;">&#8212;</div><div class="metric-lbl">X Axis</div></div>
        <div class="metric"><div class="metric-val" id="mGY" style="color:#30d4d4;">&#8212;</div><div class="metric-lbl">Y Axis</div></div>
        <div class="metric"><div class="metric-val" id="mGZ" style="color:#ffb830;">&#8212;</div><div class="metric-lbl">Z Axis</div></div>
      </div>
      <div class="total-row">
        <span class="total-label">&#128752; Satellites</span>
        <span class="total-val" id="gpsSats">&#8212;</span>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="card-header">
      <div class="card-icon" style="background:#0a181a;">&#128205;</div>
      <div class="card-title">GPS Location &amp; Accuracy</div>
    </div>
    <div class="gps-grid">
      <div class="gps-box">Latitude<span id="gpsLat">No fix</span></div>
      <div class="gps-box">Longitude<span id="gpsLng">No fix</span></div>
    </div>
    <div class="gps-meta">
      <div class="gps-meta-item">Satellites<b id="gpsSatCount">&#8212;</b></div>
      <div class="gps-meta-item">HDOP<b id="gpsHdop">&#8212;</b></div>
      <div class="gps-meta-item">Age (ms)<b id="gpsAge">&#8212;</b></div>
      <div class="gps-meta-item">Accuracy<b id="gpsAccuracy">&#8212;</b></div>
    </div>
    <div style="margin-bottom:12px;">
      <div style="font-size:11px;color:var(--muted);font-weight:600;margin-bottom:6px;">SIGNAL QUALITY</div>
      <div class="acc-bar-wrap"><div class="acc-bar" id="gpsQualBar" style="width:0%;background:var(--red);"></div></div>
    </div>
    <div id="gpsBtnWrap">
      <div style="color:var(--muted);font-size:13px;text-align:center;padding:12px;font-weight:500;">&#128752; Waiting for GPS signal...</div>
    </div>
  </div>

  <div class="api-box" id="apiResponse"></div>
</div>

<div class="footer">
  <span class="live-dot" style="display:inline-block;width:6px;height:6px;border-radius:50%;background:var(--green);margin-right:6px;vertical-align:middle;"></span>
  Connected to ESP32 &bull; Auto-refreshes every <span>2s</span> &bull; Fall Detection System v2.2
</div>

<script>
let buzzerActive = false;
let ledActive    = false;
let fallTime     = null;
let fallAgoInterval = null;

async function fetchStatus() {
  try {
    const r = await fetch('/status');
    const d = await r.json();

    document.getElementById('uptimeVal').textContent = d.uptime || '--:--:--';

    const sysOn = d.systemON;
    setDot('sysDot', sysOn ? 'green' : 'gray');
    setBadge('sysBadge', sysOn ? 'ON' : 'OFF', sysOn ? 'green' : 'gray');
    document.getElementById('sysBtn').textContent = sysOn ? '\u26A1 Power Off System' : '\u26A1 Power On System';
    document.getElementById('sysBtn').style.color = sysOn ? 'var(--red)' : '';
    document.getElementById('sysBtn').style.borderColor = sysOn ? 'var(--red)' : '';

    buzzerActive = d.buzzerON;
    setDot('buzzDot', d.buzzerON ? 'red' : 'gray');
    setBadge('buzzBadge', d.buzzerON ? 'RINGING' : 'SILENT', d.buzzerON ? 'red' : 'gray');
    const bBtn = document.getElementById('buzzerBtn');
    bBtn.textContent = d.buzzerON ? '\uD83D\uDD15 Ringing...' : '\uD83D\uDD14 Ring Buzzer';
    bBtn.className   = 'btn ' + (d.buzzerON ? 'btn-ghost-red' : 'btn-primary');

    ledActive = d.ledON;
    setDot('ledDot', d.ledON ? 'orange' : 'gray');
    setBadge('ledBadge', d.ledON ? 'ON' : 'OFF', d.ledON ? 'orange' : 'gray');
    const lBtn = document.getElementById('ledBtn');
    lBtn.textContent = d.ledON ? '\uD83D\uDCA1 LED On' : '\uD83D\uDCA1 LED Off';
    lBtn.className   = 'btn ' + (d.ledON ? 'btn-led-on' : 'btn-led-off');

    const banner = document.getElementById('fallBanner');
    if (d.fallDetected) {
      setDot('fallDot', 'red');
      setBadge('fallBadge', 'FALL!', 'red');
      banner.classList.add('visible');
      if (!fallTime) { fallTime = Date.now(); startAgoTimer(); }
      document.getElementById('fallBannerIST').textContent  = '\uD83D\uDD50 ' + (d.fallIST || '--') + ' (IST)';
      document.getElementById('fallBannerTime').textContent = 'Device uptime at fall: ' + (d.uptime || '--');
      const locEl = document.getElementById('fallBannerLocation');
      if (d.gpsValid) {
        locEl.innerHTML = '<a class="maps-btn" href="https://maps.google.com/?q=' + d.lat + ',' + d.lng + '" target="_blank">\uD83D\uDCCD Open in Maps</a>';
      } else if (d.hadGpsFix) {
        locEl.innerHTML = '<a class="maps-btn" style="background:linear-gradient(135deg,#5a3800,#ffb830);color:#1a0a00;" href="https://maps.google.com/?q=' + d.lat + ',' + d.lng + '" target="_blank">\uD83D\uDCCD Last Known Location</a>';
      } else {
        locEl.innerHTML = '<span class="gps-na">&#128752; GPS unavailable</span>';
      }
    } else {
      setDot('fallDot', 'gray');
      setBadge('fallBadge', 'NORMAL', 'gray');
      banner.classList.remove('visible');
      fallTime = null;
      clearInterval(fallAgoInterval);
      document.getElementById('fallBannerAgo').textContent = '';
      document.getElementById('fallBannerIST').textContent = '--';
    }

    setDot('emailDot', d.emailSending ? 'yellow' : 'gray');
    setBadge('emailBadge', d.emailSending ? 'SENDING' : 'IDLE', d.emailSending ? 'yellow' : 'gray');

    setDot('tgDot', d.telegramSending ? 'cyan' : 'gray');
    setBadge('tgBadge', d.telegramSending ? 'SENDING' : 'IDLE', d.telegramSending ? 'cyan' : 'gray');

    document.getElementById('mAX').textContent = fmtN(d.ax, 2);
    document.getElementById('mAY').textContent = fmtN(d.ay, 2);
    document.getElementById('mAZ').textContent = fmtN(d.az, 2);
    document.getElementById('mTotal').textContent = fmtN(d.totalAcc, 3) + ' g';
    if (d.gx !== undefined) {
      document.getElementById('mGX').textContent = fmtN(d.gx, 1);
      document.getElementById('mGY').textContent = fmtN(d.gy, 1);
      document.getElementById('mGZ').textContent = fmtN(d.gz, 1);
    }

    const sats = d.satellites !== undefined ? d.satellites : 0;
    const hdop = d.hdop !== undefined ? d.hdop : 99;
    const age  = d.gpsAge !== undefined ? d.gpsAge : '--';

    document.getElementById('gpsSats').textContent     = sats + ' \uD83D\uDEF0';
    document.getElementById('gpsSatCount').textContent = sats;
    document.getElementById('gpsHdop').textContent     = hdop < 90 ? hdop.toFixed(1) : '--';
    document.getElementById('gpsAge').textContent      = age !== '--' ? age : '--';

    let qual = 0;
    if (d.gpsValid) {
      qual = Math.min(100, Math.max(0,
        (sats / 12 * 50) + (hdop < 99 ? Math.max(0, (5 - hdop) / 5 * 50) : 0)
      ));
    } else if (d.hadGpsFix) { qual = 20; }
    const bar = document.getElementById('gpsQualBar');
    bar.style.width = qual + '%';
    bar.style.background = qual > 70 ? 'var(--green)' : qual > 40 ? 'var(--yellow)' : qual > 0 ? 'var(--orange)' : 'var(--red)';

    const accStr = d.gpsValid
      ? (hdop < 1.5 ? 'Excellent' : hdop < 2.5 ? 'Good' : 'Fair')
      : (d.hadGpsFix ? 'Last Known' : 'Poor');
    document.getElementById('gpsAccuracy').textContent = accStr;
    document.getElementById('gpsAccuracy').style.color =
      d.gpsValid
        ? (hdop < 1.5 ? 'var(--green)' : hdop < 2.5 ? 'var(--cyan)' : 'var(--yellow)')
        : (d.hadGpsFix ? 'var(--orange)' : 'var(--red)');

    if (d.gpsValid) {
      setDot('gpsDot', 'green');
      setBadge('gpsBadge', 'ACCURATE', 'green');
      document.getElementById('gpsLat').textContent = d.lat.toFixed(7);
      document.getElementById('gpsLng').textContent = d.lng.toFixed(7);
      document.getElementById('gpsBtnWrap').innerHTML =
        '<a class="btn-map-full" href="https://maps.google.com/?q=' + d.lat + ',' + d.lng + '" target="_blank">' +
        '\uD83D\uDCCD Open Live Location in Google Maps</a>';
    } else if (d.hadGpsFix) {
      setDot('gpsDot', 'yellow');
      setBadge('gpsBadge', 'LAST KNOWN', 'yellow');
      document.getElementById('gpsLat').textContent = d.lat.toFixed(7) + ' *';
      document.getElementById('gpsLng').textContent = d.lng.toFixed(7) + ' *';
      document.getElementById('gpsBtnWrap').innerHTML =
        '<div style="margin-bottom:10px;background:#2e1e00;border:1px solid #6b4400;border-radius:10px;padding:10px 14px;font-size:12px;color:var(--yellow);font-weight:600;">' +
        '\u26A0\uFE0F GPS signal lost \u2014 showing last known location (may be outdated)</div>' +
        '<a class="btn-map-last" href="https://maps.google.com/?q=' + d.lat + ',' + d.lng + '" target="_blank">' +
        '\uD83D\uDCCD Last Known Location in Google Maps</a>';
    } else {
      setDot('gpsDot', 'gray');
      setBadge('gpsBadge', 'NO FIX', 'gray');
      document.getElementById('gpsLat').textContent = 'No fix';
      document.getElementById('gpsLng').textContent = 'No fix';
      document.getElementById('gpsBtnWrap').innerHTML =
        '<div style="color:var(--muted);font-size:13px;text-align:center;padding:12px;font-weight:500;">\uD83D\uDEF0 Waiting for GPS signal... (' + sats + ' sats)</div>';
    }
  } catch(e) { console.log('Fetch error:', e); }
}

function fmtN(v, dec) {
  if (v === undefined || v === null) return '--';
  return Number(v).toFixed(dec);
}
function setDot(id, cls) { document.getElementById(id).className = 'dot ' + cls; }
function setBadge(id, text, color) {
  const el = document.getElementById(id);
  el.textContent = text;
  el.className   = 'badge badge-' + color;
}
function startAgoTimer() {
  clearInterval(fallAgoInterval);
  fallAgoInterval = setInterval(() => {
    if (!fallTime) return;
    const ago = Math.floor((Date.now() - fallTime) / 1000);
    const m = Math.floor(ago / 60), s = ago % 60;
    document.getElementById('fallBannerAgo').textContent =
      '\u23F1 ' + (m > 0 ? m + 'm ' : '') + s + 's since fall';
  }, 1000);
}
async function postAPI(payload) {
  try {
    const r = await fetch('/api', {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify(payload)
    });
    const d = await r.json();
    const el = document.getElementById('apiResponse');
    el.textContent = JSON.stringify(d, null, 2);
    el.classList.add('visible');
    fetchStatus();
    return d;
  } catch(e) {
    document.getElementById('apiResponse').textContent = 'Error: ' + e;
    document.getElementById('apiResponse').classList.add('visible');
  }
}
async function toggleBuzzer()   { await fetch(buzzerActive ? '/buzzer/off' : '/buzzer/on'); fetchStatus(); }
async function silenceBuzzer()  { await fetch('/buzzer/off'); fetchStatus(); }
async function toggleSystem()   { await fetch('/system/toggle'); fetchStatus(); }
async function toggleLED()      { await fetch(ledActive ? '/led/off' : '/led/on'); fetchStatus(); }
async function triggerFall()    { await postAPI({ triggerFall: true }); }
async function testEmail()      { await postAPI({ sendEmail: true }); }
async function testTelegram()   { await postAPI({ sendTelegram: true }); }
async function silenceAll()     { await postAPI({ silence: true }); }

fetchStatus();
setInterval(fetchStatus, 2000);
</script>
</body>
</html>
)rawhtml";

// ─── Web Handlers ─────────────────────────────────────────────────────────────
void handleRoot() { server.send(200, "text/html", htmlPage); }

void handleStatus() {
  unsigned long sec = millis() / 1000;
  char uptimeBuf[32];
  snprintf(uptimeBuf, sizeof(uptimeBuf), "%02lu:%02lu:%02lu", sec/3600, (sec%3600)/60, sec%60);
  bool accurate = isGpsAccurate();
  StaticJsonDocument<512> doc;
  doc["systemON"]       = systemON;
  doc["fallDetected"]   = fallDetected;
  doc["buzzerON"]       = buzzerON;
  doc["ledON"]          = ledON;
  doc["emailSending"]   = emailSending;
  doc["telegramSending"]= false;  // reflected via emailSending task
  doc["ax"]             = lastAX;  doc["ay"] = lastAY;  doc["az"] = lastAZ;
  doc["gx"]             = lastGX;  doc["gy"] = lastGY;  doc["gz"] = lastGZ;
  doc["totalAcc"]       = lastTotalAcc;
  doc["uptime"]         = uptimeBuf;
  doc["hadGpsFix"]      = hadGpsFix;
  doc["gpsValid"]       = accurate;
  doc["lat"]            = lastKnownLat;
  doc["lng"]            = lastKnownLng;
  doc["satellites"]     = lastKnownSats;
  doc["hdop"]           = gps.hdop.isValid() ? gps.hdop.hdop() : 99.9f;
  doc["gpsAge"]         = gps.location.isValid() ? (long)gps.location.age() : -1;
  doc["fallUptime"]     = fallTimestampMs > 0 ? (long)(fallTimestampMs / 1000) : 0;
  doc["fallIST"]        = fallISTTime;
  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

void handleLEDOn() {
  ledON = true;
  digitalWrite(LED_FALL_PIN, HIGH);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", "LED ON");
}

void handleLEDOff() {
  ledON = false;
  if (!fallDetected) digitalWrite(LED_FALL_PIN, LOW);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", "LED OFF");
}

void handleBuzzerOn() {
  buzzerOn();
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", "Buzzer ON");
}

void handleBuzzerOff() {
  buzzerOff();
  if (!ledON) digitalWrite(LED_FALL_PIN, LOW);
  fallDetected = false; fallTimestampMs = 0;
  memset(fallISTTime, 0, sizeof(fallISTTime));
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", "Buzzer OFF");
}

void handleSystemToggle() {
  systemON = !systemON;
  if (!systemON) {
    buzzerOff();
    if (!ledON) digitalWrite(LED_FALL_PIN, LOW);
    fallDetected = false; fallTimestampMs = 0;
    memset(fallISTTime, 0, sizeof(fallISTTime));
    showOLED_SystemOff();
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", systemON ? "System ON" : "System OFF");
}

void handleJSON() {
  if (server.method() == HTTP_POST) {
    StaticJsonDocument<200> req;
    DeserializationError err = deserializeJson(req, server.arg("plain"));
    if (err) { server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return; }

    if (req.containsKey("buzzer")) {
      bool state = req["buzzer"];
      if (state) { buzzerOn(); digitalWrite(LED_FALL_PIN, HIGH); }
      else {
        buzzerOff(); fallDetected = false; fallTimestampMs = 0;
        memset(fallISTTime, 0, sizeof(fallISTTime));
        if (!ledON) digitalWrite(LED_FALL_PIN, LOW);
      }
    }
    if (req.containsKey("led")) {
      ledON = (bool)req["led"];
      if (!fallDetected) digitalWrite(LED_FALL_PIN, ledON ? HIGH : LOW);
    }
    if (req.containsKey("triggerFall") && req["triggerFall"] == true) {
      fallDetected = true; fallTimestampMs = millis();
      getISTTimeString(fallISTTime, sizeof(fallISTTime));
      buzzerOn(); digitalWrite(LED_FALL_PIN, HIGH);
      showOLED_Fall();
      sendFallAlert(hadGpsFix ? lastKnownLat : 0.0f, hadGpsFix ? lastKnownLng : 0.0f, hadGpsFix);
    }
    if (req.containsKey("sendEmail") && req["sendEmail"] == true) {
      sendFallAlert(hadGpsFix ? lastKnownLat : 0.0f, hadGpsFix ? lastKnownLng : 0.0f, hadGpsFix);
    }
    if (req.containsKey("sendTelegram") && req["sendTelegram"] == true) {
      // Quick test telegram message
      String msg = "&#128680; <b>Test Alert</b>\nFall Detection System is working!\nDevice uptime: ";
      unsigned long sec = millis() / 1000;
      char uptimeStr[32];
      snprintf(uptimeStr, sizeof(uptimeStr), "%02lu:%02lu:%02lu", sec/3600, (sec%3600)/60, sec%60);
      msg += String(uptimeStr);
      sendTelegram(msg);
    }
    if (req.containsKey("system")) {
      systemON = (bool)req["system"];
      if (!systemON) {
        buzzerOff();
        if (!ledON) digitalWrite(LED_FALL_PIN, LOW);
        fallDetected = false; fallTimestampMs = 0;
        memset(fallISTTime, 0, sizeof(fallISTTime));
        showOLED_SystemOff();
      }
    }
    if (req.containsKey("silence") && req["silence"] == true) {
      buzzerOff();
      if (!ledON) digitalWrite(LED_FALL_PIN, LOW);
      fallDetected = false; fallTimestampMs = 0;
      memset(fallISTTime, 0, sizeof(fallISTTime));
    }
  }
  unsigned long sec = millis() / 1000;
  char uptimeBuf[32];
  snprintf(uptimeBuf, sizeof(uptimeBuf), "%02lu:%02lu:%02lu", sec/3600, (sec%3600)/60, sec%60);
  StaticJsonDocument<512> res;
  res["systemON"]     = systemON;   res["fallDetected"] = fallDetected;
  res["buzzerON"]     = buzzerON;   res["ledON"]        = ledON;
  res["emailSending"] = emailSending;
  res["ax"] = lastAX; res["ay"] = lastAY; res["az"] = lastAZ;
  res["gx"] = lastGX; res["gy"] = lastGY; res["gz"] = lastGZ;
  res["totalAcc"]   = lastTotalAcc; res["uptime"]     = uptimeBuf;
  res["hadGpsFix"]  = hadGpsFix;   res["gpsValid"]   = isGpsAccurate();
  res["lat"]        = lastKnownLat; res["lng"]        = lastKnownLng;
  res["satellites"] = lastKnownSats;
  res["hdop"]       = gps.hdop.isValid() ? gps.hdop.hdop() : 99.9f;
  res["gpsAge"]     = gps.location.isValid() ? (long)gps.location.age() : -1;
  res["fallIST"]    = fallISTTime;
  String out;
  serializeJson(res, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// ─── OLED ─────────────────────────────────────────────────────────────────────
void showOLED_Normal(float ax, float ay, float az, float gx, float gy, float gz, float total) {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);  display.println("--- MPU6050 ---");
  display.setCursor(0, 12); display.print("AX:"); display.print(ax, 2); display.print(" AY:"); display.println(ay, 2);
  display.setCursor(0, 22); display.print("AZ:"); display.println(az, 2);
  display.drawLine(0, 32, 127, 32, SSD1306_WHITE);
  display.setCursor(0, 36); display.print("GX:"); display.print(gx, 1); display.print(" GY:"); display.println(gy, 1);
  display.setCursor(0, 46); display.print("GZ:"); display.print(gz, 1);
  display.print("  SAT:"); display.println(lastKnownSats);
  display.setCursor(0, 56);
  if (hadGpsFix) {
    if (isGpsAccurate()) {
      display.print(lastKnownLat, 5); display.print(","); display.print(lastKnownLng, 5);
    } else {
      display.print("Last:"); display.print(lastKnownLat, 4); display.print(","); display.print(lastKnownLng, 4);
    }
  } else {
    display.print("GPS: No fix (");
    display.print(gps.satellites.isValid() ? (int)gps.satellites.value() : 0);
    display.print("sat)");
  }
  display.display();
}

void showOLED_Fall() {
  display.clearDisplay();
  display.setTextSize(2); display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 0);  display.println("!! FALL !!");
  display.setCursor(5,  20); display.println("DETECTED!");
  display.setTextSize(1);
  display.setCursor(0, 42);
  display.println(strlen(fallISTTime) ? fallISTTime : (emailSending ? "Sending..." : "Alert sent!"));
  display.setCursor(0, 54);
  if (hadGpsFix) {
    display.print(lastKnownLat, 5); display.print(","); display.print(lastKnownLng, 5);
  } else {
    display.print("GPS: No fix");
  }
  display.display();
}

void showOLED_Boot(String msg) {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20); display.println(msg);
  display.display();
}

void showOLED_SystemOff() {
  display.clearDisplay();
  display.setTextSize(2); display.setTextColor(SSD1306_WHITE);
  display.setCursor(15, 10); display.println("SYSTEM");
  display.setCursor(30, 30); display.println("OFF");
  display.setTextSize(1);
  display.setCursor(0, 52); display.println(WiFi.localIP().toString());
  display.display();
}

// ─── Email + Telegram Task ────────────────────────────────────────────────────
void smtpCallback(SMTP_Status status) {
  Serial.println(status.info());
  if (status.success()) Serial.println(">>> EMAIL SENT <<<");
  else                   Serial.println(">>> EMAIL FAILED <<<");
}

struct EmailParams { float lat; float lng; bool gpsValid; char istTime[32]; int sats; float hdop; };

void emailTask(void* pvParameters) {
  EmailParams* p = (EmailParams*)pvParameters;
  if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); delay(2000); }

  // ── Send Telegram first (fast) ──────────────────────────────────────────────
  unsigned long sec = millis() / 1000;
  char uptimeStr[32];
  snprintf(uptimeStr, sizeof(uptimeStr), "%02lu:%02lu:%02lu", sec/3600, (sec%3600)/60, sec%60);

  String tgMsg = "&#128680; <b>FALL DETECTED!</b>\n\n";
  tgMsg += "&#128336; <b>Time (IST):</b> " + String(p->istTime) + "\n";
  tgMsg += "&#9201; <b>Uptime:</b> " + String(uptimeStr) + "\n";
  tgMsg += "&#128241; <b>Device:</b> ESP32 Wearable\n\n";

  if (p->gpsValid) {
    tgMsg += "&#128205; <b>Location (Live GPS):</b>\n";
    tgMsg += "Lat: " + String(p->lat, 7) + "\n";
    tgMsg += "Lng: " + String(p->lng, 7) + "\n";
    tgMsg += "Satellites: " + String(p->sats) + "\n";
    tgMsg += "HDOP: " + String(p->hdop, 1) + "\n\n";
    tgMsg += "&#128279; https://maps.google.com/?q=" + String(p->lat, 7) + "," + String(p->lng, 7);
  } else if (hadGpsFix) {
    tgMsg += "&#9888; <b>Last Known Location:</b>\n";
    tgMsg += "Lat: " + String(p->lat, 7) + "\n";
    tgMsg += "Lng: " + String(p->lng, 7) + "\n\n";
    tgMsg += "&#128279; https://maps.google.com/?q=" + String(p->lat, 7) + "," + String(p->lng, 7);
  } else {
    tgMsg += "&#128752; GPS unavailable at time of fall.";
  }

  tgMsg += "\n\n&#128994; <i>Please check on the person immediately!</i>";

  sendTelegram(tgMsg);

  // Send live location pin as a separate Telegram message if GPS valid
  if (p->gpsValid || hadGpsFix) {
    sendTelegramLocation(p->lat, p->lng);
  }

  // ── NTP Sync ────────────────────────────────────────────────────────────────
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing NTP time");
  struct tm timeinfo;
  int ntpTries = 0;
  while (!getLocalTime(&timeinfo) && ntpTries < 20) {
    delay(500); Serial.print("."); ntpTries++;
  }
  if (ntpTries < 20) Serial.println("\nNTP time synced!");
  else               Serial.println("\nNTP sync failed, continuing anyway...");

  // ── Send Email ───────────────────────────────────────────────────────────────
  smtp.callback(smtpCallback);
  ESP_Mail_Session session;
  session.server.host_name = "smtp.gmail.com";
  session.server.port      = 465;
  session.login.email      = SENDER_EMAIL;
  session.login.password   = SENDER_PASSWORD;

  SMTP_Message message;
  message.sender.name  = "Fall Detection System";
  message.sender.email = SENDER_EMAIL;
  message.subject      = "🚨 URGENT: Fall Detected – Immediate Action Required";
  message.addRecipient("Caretaker", RECEIVER_EMAIL);

  String accLabel = "Unavailable";
  String accColor = "#92400e";
  String accBg    = "#fef3c7";
  String accBorder= "#fcd34d";
  if (p->gpsValid) {
    if (p->hdop < 1.5f)      { accLabel = "Excellent"; accColor = "#065f46"; accBg = "#d1fae5"; accBorder = "#34d399"; }
    else if (p->hdop < 2.5f) { accLabel = "Good";      accColor = "#1e40af"; accBg = "#dbeafe"; accBorder = "#60a5fa"; }
    else                      { accLabel = "Fair";      accColor = "#92400e"; accBg = "#fef3c7"; accBorder = "#fcd34d"; }
  }

  String locationBlock = "";
  if (p->gpsValid) {
    String mapsUrl    = "https://maps.google.com/?q=" + String(p->lat, 7) + "," + String(p->lng, 7);
    String mapsUrlSat = "https://maps.google.com/?q=" + String(p->lat, 7) + "," + String(p->lng, 7) + "&t=k";
    locationBlock =
      "<tr><td style='padding:8px 0;color:#6b7280;font-size:13px;width:130px;font-weight:500;'>Latitude</td>"
      "<td style='padding:8px 0;font-size:14px;font-weight:700;color:#111827;font-family:monospace;'>" + String(p->lat, 7) + "</td></tr>"
      "<tr><td style='padding:8px 0;color:#6b7280;font-size:13px;font-weight:500;'>Longitude</td>"
      "<td style='padding:8px 0;font-size:14px;font-weight:700;color:#111827;font-family:monospace;'>" + String(p->lng, 7) + "</td></tr>"
      "<tr><td style='padding:8px 0;color:#6b7280;font-size:13px;font-weight:500;'>Satellites</td>"
      "<td style='padding:8px 0;font-size:13px;font-weight:600;color:#111827;'>" + String(p->sats) + " satellites</td></tr>"
      "<tr><td style='padding:8px 0;color:#6b7280;font-size:13px;font-weight:500;'>Accuracy</td>"
      "<td style='padding:8px 0;'><span style='background:" + accBg + ";color:" + accColor + ";border:1px solid " + accBorder + ";border-radius:20px;padding:2px 10px;font-size:12px;font-weight:700;'>" + accLabel + " (HDOP: " + String(p->hdop, 1) + ")</span></td></tr>"
      "<tr><td colspan='2' style='padding-top:16px;'>"
      "<table width='100%' cellpadding='0' cellspacing='0'><tr>"
      "<td style='padding-right:6px;'><a href='" + mapsUrl + "' style='display:block;background:linear-gradient(135deg,#1d4ed8,#3b82f6);color:#fff;text-decoration:none;padding:12px 0;border-radius:10px;font-size:13px;font-weight:700;text-align:center;'>&#128205; Street View</a></td>"
      "<td style='padding-left:6px;'><a href='" + mapsUrlSat + "' style='display:block;background:linear-gradient(135deg,#065f46,#10b981);color:#fff;text-decoration:none;padding:12px 0;border-radius:10px;font-size:13px;font-weight:700;text-align:center;'>&#128752; Satellite View</a></td>"
      "</tr></table></td></tr>";
  } else if (hadGpsFix) {
    String mapsUrl = "https://maps.google.com/?q=" + String(p->lat, 7) + "," + String(p->lng, 7);
    locationBlock =
      "<tr><td colspan='2'><div style='background:#fffbeb;border:1px solid #fcd34d;border-radius:10px;padding:10px 14px;margin-bottom:12px;'>"
      "<span style='font-size:12px;color:#92400e;font-weight:700;'>&#9888; GPS signal was lost at time of fall. Showing last known location.</span>"
      "</div></td></tr>"
      "<tr><td style='padding:8px 0;color:#6b7280;font-size:13px;width:130px;font-weight:500;'>Last Latitude</td>"
      "<td style='padding:8px 0;font-size:14px;font-weight:700;color:#111827;font-family:monospace;'>" + String(p->lat, 7) + "</td></tr>"
      "<tr><td style='padding:8px 0;color:#6b7280;font-size:13px;font-weight:500;'>Last Longitude</td>"
      "<td style='padding:8px 0;font-size:14px;font-weight:700;color:#111827;font-family:monospace;'>" + String(p->lng, 7) + "</td></tr>"
      "<tr><td colspan='2' style='padding-top:12px;'>"
      "<a href='" + mapsUrl + "' style='display:block;background:linear-gradient(135deg,#92400e,#d97706);color:#fff;text-decoration:none;padding:12px 0;border-radius:10px;font-size:13px;font-weight:700;text-align:center;'>&#128205; Open Last Known Location in Maps</a>"
      "</td></tr>";
  } else {
    locationBlock =
      "<tr><td colspan='2'><div style='background:#fef3c7;border:1px solid #fcd34d;border-radius:10px;padding:14px 16px;'>"
      "<div style='font-size:13px;color:#92400e;font-weight:700;'>&#128752; GPS Signal Unavailable</div>"
      "<div style='font-size:12px;color:#b45309;margin-top:2px;'>The device had no GPS fix at the time of the fall.</div>"
      "</div></td></tr>";
  }

  String body = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'/><meta name='viewport' content='width=device-width,initial-scale=1'/></head>";
  body += "<body style='margin:0;padding:0;background:#f1f5f9;font-family:-apple-system,BlinkMacSystemFont,Arial,sans-serif;'>";
  body += "<table width='100%' cellpadding='0' cellspacing='0' style='background:#f1f5f9;padding:40px 16px;'><tr><td align='center'>";
  body += "<table width='580' cellpadding='0' cellspacing='0' style='background:#ffffff;border-radius:20px;overflow:hidden;box-shadow:0 8px 40px rgba(0,0,0,0.12);border:1px solid #e2e8f0;'>";
  body += "<tr><td style='background:linear-gradient(135deg,#7f1d1d 0%,#b91c1c 50%,#ef4444 100%);padding:40px 44px 36px;text-align:center;'>";
  body += "<div style='width:72px;height:72px;background:rgba(255,255,255,0.15);border-radius:50%;margin:0 auto 16px;display:flex;align-items:center;justify-content:center;font-size:36px;line-height:72px;text-align:center;'>&#128680;</div>";
  body += "<div style='font-size:28px;font-weight:800;color:#ffffff;letter-spacing:-0.5px;margin-bottom:8px;'>Fall Detected!</div>";
  body += "<div style='font-size:14px;color:#fca5a5;font-weight:500;'>Automatic alert from ESP32 Wearable Device</div>";
  body += "</td></tr>";
  body += "<tr><td style='background:#fff1f2;border-bottom:3px solid #fecaca;padding:14px 44px;text-align:center;'>";
  body += "<div style='display:inline-flex;align-items:center;gap:8px;'>";
  body += "<span style='font-size:16px;'>&#9888;</span>";
  body += "<span style='font-size:13px;font-weight:800;color:#be123c;letter-spacing:0.8px;text-transform:uppercase;'>Immediate attention required</span>";
  body += "<span style='font-size:16px;'>&#9888;</span></div></td></tr>";
  body += "<tr><td style='padding:36px 44px;'>";
  body += "<p style='font-size:15px;color:#374151;line-height:1.8;margin:0 0 30px;'>A <strong style='color:#b91c1c;'>fall event</strong> has been detected by the ESP32 wearable sensor. Please verify the person's safety <strong>immediately</strong> and contact emergency services if needed.</p>";
  body += "<div style='background:#fff5f5;border-radius:14px;border:1px solid #fecaca;margin-bottom:22px;overflow:hidden;'>";
  body += "<div style='background:#fef2f2;padding:14px 22px;border-bottom:1px solid #fecaca;'>";
  body += "<span style='font-size:11px;font-weight:800;color:#b91c1c;text-transform:uppercase;letter-spacing:1.2px;'>&#128336; Fall Event Details</span></div>";
  body += "<div style='padding:20px 22px;'><table width='100%' cellpadding='0' cellspacing='0'>";
  body += "<tr><td style='padding:8px 0;color:#6b7280;font-size:13px;width:130px;font-weight:500;'>Time (IST)</td>";
  body += "<td style='padding:8px 0;font-size:14px;font-weight:700;color:#111827;'>" + String(p->istTime) + "</td></tr>";
  body += "<tr><td style='padding:8px 0;color:#6b7280;font-size:13px;font-weight:500;'>Device Uptime</td>";
  body += "<td style='padding:8px 0;font-size:13px;font-weight:600;color:#111827;'>" + String(uptimeStr) + "</td></tr>";
  body += "<tr><td style='padding:8px 0;color:#6b7280;font-size:13px;font-weight:500;'>Device</td>";
  body += "<td style='padding:8px 0;font-size:13px;font-weight:600;color:#111827;'>ESP32 Wearable Sensor</td></tr>";
  body += "</table></div></div>";
  body += "<div style='background:#f0f9ff;border-radius:14px;border:1px solid #bae6fd;margin-bottom:22px;overflow:hidden;'>";
  body += "<div style='background:#e0f2fe;padding:14px 22px;border-bottom:1px solid #bae6fd;'>";
  body += "<span style='font-size:11px;font-weight:800;color:#0369a1;text-transform:uppercase;letter-spacing:1.2px;'>&#128205; Last Known Location</span></div>";
  body += "<div style='padding:20px 22px;'><table width='100%' cellpadding='0' cellspacing='0'>" + locationBlock + "</table></div></div>";
  body += "<div style='background:#f0fdf4;border-radius:14px;border:1px solid #bbf7d0;overflow:hidden;'>";
  body += "<div style='background:#dcfce7;padding:14px 22px;border-bottom:1px solid #bbf7d0;'>";
  body += "<span style='font-size:11px;font-weight:800;color:#166534;text-transform:uppercase;letter-spacing:1.2px;'>&#9989; Recommended Actions</span></div>";
  body += "<div style='padding:20px 22px;'><table width='100%' cellpadding='0' cellspacing='0;'>";
  body += "<tr><td style='padding:6px 0;vertical-align:top;width:28px;'><span style='display:inline-block;width:22px;height:22px;background:#16a34a;color:#fff;border-radius:50%;font-size:11px;font-weight:800;text-align:center;line-height:22px;'>1</span></td>";
  body += "<td style='padding:6px 0;font-size:14px;color:#374151;font-weight:500;'>Check on the person <strong>immediately</strong></td></tr>";
  body += "<tr><td style='padding:6px 0;vertical-align:top;'><span style='display:inline-block;width:22px;height:22px;background:#16a34a;color:#fff;border-radius:50%;font-size:11px;font-weight:800;text-align:center;line-height:22px;'>2</span></td>";
  body += "<td style='padding:6px 0;font-size:14px;color:#374151;font-weight:500;'>Call emergency services <strong>(112)</strong> if unresponsive</td></tr>";
  body += "<tr><td style='padding:6px 0;vertical-align:top;'><span style='display:inline-block;width:22px;height:22px;background:#16a34a;color:#fff;border-radius:50%;font-size:11px;font-weight:800;text-align:center;line-height:22px;'>3</span></td>";
  body += "<td style='padding:6px 0;font-size:14px;color:#374151;font-weight:500;'>Do <strong>not</strong> move the person if a spinal injury is suspected</td></tr>";
  body += "<tr><td style='padding:6px 0;vertical-align:top;'><span style='display:inline-block;width:22px;height:22px;background:#16a34a;color:#fff;border-radius:50%;font-size:11px;font-weight:800;text-align:center;line-height:22px;'>4</span></td>";
  body += "<td style='padding:6px 0;font-size:14px;color:#374151;font-weight:500;'>Press the buzzer button on device to silence alarm after confirming safety</td></tr>";
  body += "</table></div></div>";
  body += "</td></tr>";
  body += "<tr><td style='background:#f8fafc;border-top:1px solid #e2e8f0;padding:22px 44px;text-align:center;'>";
  body += "<div style='font-size:12px;color:#9ca3af;line-height:1.8;'>Sent automatically &bull; <strong style='color:#6b7280;'>Fall Detection System</strong> &bull; ESP32 Wearable &bull; Do not reply</div>";
  body += "</td></tr></table></td></tr></table></body></html>";

  message.html.content = body.c_str();
  message.html.charSet = "UTF-8";
  smtp.debug(1);
  if (!smtp.connect(&session))
    Serial.println("SMTP ERROR: " + smtp.errorReason());
  else if (!MailClient.sendMail(&smtp, &message))
    Serial.println("SEND ERROR: " + smtp.errorReason());
  smtp.closeSession();

  delete p;
  emailSending = false;
  vTaskDelete(NULL);
}

void sendFallAlert(float lat, float lng, bool gpsValid) {
  if (emailSending) return;
  emailSending = true;
  EmailParams* p = new EmailParams{
    lat, lng, gpsValid, {}, lastKnownSats,
    gps.hdop.isValid() ? gps.hdop.hdop() : 99.9f
  };
  strncpy(p->istTime, fallISTTime, sizeof(p->istTime));
  xTaskCreate(emailTask, "AlertTask", 12288, p, 1, NULL);  // increased stack for HTTPS
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(BTN_SYSTEM,   INPUT_PULLUP);
  pinMode(BTN_BUZZER,   INPUT_PULLUP);
  pinMode(LED_FALL_PIN, OUTPUT);
  digitalWrite(LED_FALL_PIN, LOW);

  ledcSetup(BUZZER_CHANNEL, BUZZER_FREQ, BUZZER_RES);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcWrite(BUZZER_CHANNEL, 0);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500); Serial.print("."); attempts++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi FAILED! Restarting...");
    delay(5000); ESP.restart();
  }
  Serial.println("\nWiFi OK: " + WiFi.localIP().toString());

  server.on("/",              handleRoot);
  server.on("/status",        handleStatus);
  server.on("/buzzer/on",     handleBuzzerOn);
  server.on("/buzzer/off",    handleBuzzerOff);
  server.on("/led/on",        handleLEDOn);
  server.on("/led/off",       handleLEDOff);
  server.on("/system/toggle", handleSystemToggle);
  server.on("/api",           handleJSON);
  server.begin();

  Wire.begin(8, 9, 100000);
  delay(500);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED failed!"); while (true);
  }
  showOLED_Boot("WiFi OK\n" + WiFi.localIP().toString());
  delay(1500);

  mpu6050.begin();
  showOLED_Boot("Calibrating MPU\nKeep still...");
  delay(2000);
  mpu6050.calcGyroOffsets(true);
  Serial.println("MPU Ready!");

  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

  startTime = millis();
  showOLED_SystemOff();
  Serial.println("Open: http://" + WiFi.localIP().toString());
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost! Reconnecting...");
    WiFi.reconnect();
    delay(3000);
  }

  bool currentSystemBtn = digitalRead(BTN_SYSTEM);
  if (currentSystemBtn == LOW && lastSystemBtn == HIGH &&
      millis() - lastDebounceSystem > DEBOUNCE_DELAY) {
    systemON = !systemON;
    lastDebounceSystem = millis();
    if (!systemON) {
      buzzerOff();
      if (!ledON) digitalWrite(LED_FALL_PIN, LOW);
      fallDetected = false; fallTimestampMs = 0;
      memset(fallISTTime, 0, sizeof(fallISTTime));
      showOLED_SystemOff();
    }
  }
  lastSystemBtn = currentSystemBtn;

  while (gpsSerial.available()) gps.encode(gpsSerial.read());
  if (gps.location.isValid() && gps.location.age() < GPS_AGE_LIMIT) {
    int   newSats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
    float newHdop = gps.hdop.isValid() ? gps.hdop.hdop() : 99.9f;
    if (!hadGpsFix || (newHdop < lastKnownHdop && newSats >= lastKnownSats)) {
      lastKnownLat  = gps.location.lat();
      lastKnownLng  = gps.location.lng();
      lastKnownSats = newSats;
      lastKnownHdop = newHdop;
      hadGpsFix     = true;
    }
  }

  if (!systemON) { delay(50); return; }

  mpu6050.update();
  lastAX = mpu6050.getAccX();  lastAY = mpu6050.getAccY();  lastAZ = mpu6050.getAccZ();
  lastGX = mpu6050.getGyroX(); lastGY = mpu6050.getGyroY(); lastGZ = mpu6050.getGyroZ();
  lastTotalAcc = sqrt(lastAX*lastAX + lastAY*lastAY + lastAZ*lastAZ);

  if (fallDetected) showOLED_Fall();
  else              showOLED_Normal(lastAX, lastAY, lastAZ, lastGX, lastGY, lastGZ, lastTotalAcc);

  if (millis() - startTime < STARTUP_DELAY) { delay(50); return; }

  if (!fallDetected && (lastTotalAcc < FALL_LOW || lastTotalAcc > FALL_HIGH)) {
    Serial.println("!!! FALL DETECTED !!!");
    fallDetected    = true;
    fallTimestampMs = millis();
    getISTTimeString(fallISTTime, sizeof(fallISTTime));
    buzzerOn();
    digitalWrite(LED_FALL_PIN, HIGH);
    showOLED_Fall();
    sendFallAlert(hadGpsFix ? lastKnownLat : 0.0f, hadGpsFix ? lastKnownLng : 0.0f, hadGpsFix);
  }

  bool currentBuzzerBtn = digitalRead(BTN_BUZZER);
  if (currentBuzzerBtn == LOW && lastBuzzerBtn == HIGH &&
      millis() - lastDebounceBuzzer > DEBOUNCE_DELAY) {
    buzzerOff();
    if (!ledON) digitalWrite(LED_FALL_PIN, LOW);
    fallDetected = false; fallTimestampMs = 0;
    memset(fallISTTime, 0, sizeof(fallISTTime));
    lastDebounceBuzzer = millis();
    Serial.println("Buzzer OFF (physical button)");
  }
  lastBuzzerBtn = currentBuzzerBtn;

  delay(50);
}
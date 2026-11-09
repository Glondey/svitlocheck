#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <UniversalTelegramBot.h>
#include <time.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#if __has_include(<esp_netif.h>)
#include <esp_netif.h>
#endif
#if __has_include(<tcpip_adapter.h>)
#include <tcpip_adapter.h>
#endif

void applyDeviceHostname();

// ================= КОРИСТУВАЦЬКІ НАЛАШТУВАННЯ =================
#define SENSOR_PIN 18            // GPIO пін датчика (якщо ESP32 підключено до USB, краще GPIO 18)
#define SENSOR_ACTIVE_LOW true  // true: LOW = світло є, HIGH = світла немає (стандарт для оптопар)
                                // false: HIGH = світло є, LOW = світла немає
#define NTP_SERVER "pool.ntp.org"

inline bool isPowerPresent() {
  int val = digitalRead(SENSOR_PIN);
  return SENSOR_ACTIVE_LOW ? (val == LOW) : (val == HIGH);
}

#define CONFIG_FILE "/config.json"
#define LOG_FILE "/history.bin"
#define MAX_LOGS 500

// ================= ДІАГНОСТИЧНІ ЖУРНАЛИ РОЗРОБНИКА =================
#define MAX_TEMP_LOGS 15
#define MAX_NET_LOGS 20

struct TempLogEntry {
  time_t timestamp;
  float temp;
};

struct NetLogEntry {
  time_t timestamp;
  bool connected;
  uint16_t pingMs;
};

TempLogEntry tempLogs[MAX_TEMP_LOGS];
int tempLogCount = 0;

NetLogEntry netLogs[MAX_NET_LOGS];
int netLogCount = 0;

void addTempLog(float temp) {
  time_t now = time(NULL);
  if (tempLogCount >= MAX_TEMP_LOGS) {
    memmove(&tempLogs[0], &tempLogs[1], sizeof(TempLogEntry) * (MAX_TEMP_LOGS - 1));
    tempLogCount = MAX_TEMP_LOGS - 1;
  }
  tempLogs[tempLogCount].timestamp = (now > 1000000000) ? now : time(NULL);
  tempLogs[tempLogCount].temp = temp;
  tempLogCount++;
}

void addNetLog(bool connected, uint16_t pingMs) {
  time_t now = time(NULL);
  if (netLogCount >= MAX_NET_LOGS) {
    memmove(&netLogs[0], &netLogs[1], sizeof(NetLogEntry) * (MAX_NET_LOGS - 1));
    netLogCount = MAX_NET_LOGS - 1;
  }
  netLogs[netLogCount].timestamp = (now > 1000000000) ? now : time(NULL);
  netLogs[netLogCount].connected = connected;
  netLogs[netLogCount].pingMs = pingMs;
  netLogCount++;
}

// ================= ГЛОБАЛЬНІ ЗМІННІ =================
struct Config {
  char botToken[128] = "";
  char chatIds[512] = ""; 
  char locationName[128] = "Моя адреса / Об'єкт";
  char deviceId[64] = "SvitloCheck-01";
} config;

void applyDeviceHostname() {
  String hName = String(config.deviceId);
  hName.trim();
  if (hName.length() == 0) hName = "SvitloCheck";

  String cleanH = "";
  for (size_t i = 0; i < hName.length(); i++) {
    char c = hName[i];
    if (isalnum(c) || c == '-') {
      cleanH += c;
    } else if (c == ' ' || c == '_') {
      cleanH += '-';
    }
  }
  if (cleanH.length() == 0) cleanH = "SvitloCheck";

  WiFi.setHostname(cleanH.c_str());
#if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR >= 4)
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif) {
    esp_netif_set_hostname(netif, cleanH.c_str());
  }
#else
  tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, cleanH.c_str());
#endif
}

struct LogEntry {
  time_t timestamp;
  bool state;       // Новий стан, в який перейшла система (true = з'явилось, false = зникло)
  uint32_t duration;
  bool sent;       
};

LogEntry logs[MAX_LOGS];
int logCount = 0;

bool currentState = false;
unsigned long lastStateChangeTime = 0;

// Змінні для антибрязкоту та миттєвого оновлення
bool pendingStateChange = false;
bool pendingNewState = false;
unsigned long stateChangeMillis = 0;
bool pendingNtpLog = false;
uint32_t pendingLogDuration = 0;

WebServer server(80);
unsigned long lastPingCheckTime = 0;
uint16_t lastPingMs = 0;
bool isInternetConnected = false;
WiFiClientSecure secClient;
UniversalTelegramBot* bot = NULL;

unsigned long lastTelegramCheck = 0;
const unsigned long TELEGRAM_INTERVAL = 3000;

const String kbdMarkup = "[[\"📊 Стан\", \"📈 Звіт\"]]";

String formatDuration(uint32_t sec);

// ================= ФУНКЦІЇ ПАМ'ЯТІ =================
void saveConfig() {
  DynamicJsonDocument doc(1024);
  doc["botToken"] = config.botToken;
  doc["chatIds"] = config.chatIds;
  doc["locationName"] = config.locationName;
  doc["deviceId"] = config.deviceId;

  File file = LittleFS.open(CONFIG_FILE, "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
}

void loadConfig() {
  if (!LittleFS.exists(CONFIG_FILE)) return;
  File file = LittleFS.open(CONFIG_FILE, "r");
  if (file) {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, file);
    strlcpy(config.botToken, doc["botToken"] | "", sizeof(config.botToken));
    strlcpy(config.chatIds, doc["chatIds"] | "", sizeof(config.chatIds));
    strlcpy(config.locationName, doc["locationName"] | "Моя адреса / Об'єкт", sizeof(config.locationName));
    strlcpy(config.deviceId, doc["deviceId"] | "SvitloCheck-01", sizeof(config.deviceId));
    file.close();
  }
}

void saveLogs() {
  File file = LittleFS.open(LOG_FILE, "w");
  if (file) {
    file.write((uint8_t*)&logCount, sizeof(logCount));
    file.write((uint8_t*)logs, sizeof(LogEntry) * MAX_LOGS);
    file.close();
  }
}

void loadLogs() {
  logCount = 0;
  if (!LittleFS.exists(LOG_FILE)) return;
  File file = LittleFS.open(LOG_FILE, "r");
  if (file) {
    file.read((uint8_t*)&logCount, sizeof(logCount));
    if (logCount < 0 || logCount > MAX_LOGS) logCount = 0;
    file.read((uint8_t*)logs, sizeof(LogEntry) * MAX_LOGS);
    file.close();
  }
}

// ================= ПЕРЕВІРКА ПІНГУ ТА ІНТЕРНЕТУ =================
void checkInternetAndPing() {
  if (WiFi.status() != WL_CONNECTED) {
    if (isInternetConnected) {
      isInternetConnected = false;
      lastPingMs = 0;
      addNetLog(false, 0);
    }
    return;
  }

  WiFiClient client;
  unsigned long startT = millis();
  bool ok = client.connect(IPAddress(8, 8, 8, 8), 53, 500);
  unsigned long rtt = millis() - startT;
  if (ok) {
    client.stop();
    lastPingMs = (rtt > 0) ? (uint16_t)rtt : 1;
  } else {
    lastPingMs = 0;
  }

  // Тільки в разі зміни стану оновлюємо статус і фіксуємо в журнал розробника
  if (ok != isInternetConnected) {
    isInternetConnected = ok;
    addNetLog(isInternetConnected, lastPingMs);
  }
}

// ================= ЧАС ТА ФОРМАТУВАННЯ =================
void syncNTP() {
  configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", NTP_SERVER);
}

String formatTime(time_t t) {
  if (t == 0) return "Н/Д";
  struct tm * timeinfo = localtime(&t);
  char buf[32];
  strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M:%S", timeinfo);
  return String(buf);
}

String formatDuration(uint32_t sec) {
  uint32_t d = sec / 86400;
  uint32_t h = (sec % 86400) / 3600;
  uint32_t m = (sec % 3600) / 60;
  uint32_t s = sec % 60;
  String res = "";
  if (d > 0) res += String(d) + " дн. ";
  if (h > 0 || d > 0) res += String(h) + " год. ";
  res += String(m) + " хв. " + String(s) + " сек.";
  return res;
}

// ================= TELEGRAM =================
bool sendToAllChats(String text) {
  if (strlen(config.botToken) == 0 || bot == NULL) return false;
  bool anySent = false;
  String chats = String(config.chatIds);
  int start = 0;
  int end = chats.indexOf(',');
  while (end != -1) {
    String chatId = chats.substring(start, end);
    chatId.trim();
    if (chatId.length() > 0) {
      if (bot->sendMessageWithReplyKeyboard(chatId, text, "HTML", kbdMarkup, true, false, false)) {
        anySent = true;
      }
    }
    start = end + 1;
    end = chats.indexOf(',', start);
  }
  String lastId = chats.substring(start);
  lastId.trim();
  if (lastId.length() > 0) {
    if (bot->sendMessageWithReplyKeyboard(lastId, text, "HTML", kbdMarkup, true, false, false)) {
      anySent = true;
    }
  }
  return anySent;
}

void processUnsentMessages() {
  if (WiFi.status() != WL_CONNECTED || !isInternetConnected) return;

  for (int i = 0; i < logCount; i++) {
    if (!logs[i].sent) {
      String currStateStr = logs[i].state ? "🟢 Світло з'явилося" : "🔴 Світло зникло";
      String pastDurationLabel = logs[i].state ? "⏱️ <b>Світла не було:</b> " : "⏱️ <b>Світло було:</b> ";

      String msg = "<b>Поточний стан:</b> " + currStateStr + "\n";
      msg += "📍 <b>Адреса:</b> " + String(config.locationName) + "\n";
      msg += "📅 <b>Дата, час:</b> " + formatTime(logs[i].timestamp) + "\n";
      msg += pastDurationLabel + formatDuration(logs[i].duration);

      if (sendToAllChats(msg)) {
        logs[i].sent = true;
        saveLogs();
      } else {
        isInternetConnected = false;
      }
      break;
    }
  }
}

String generateReportText(uint32_t secondsBack) {
  time_t now = time(NULL);
  uint32_t tStart = now - secondsBack;
  uint32_t tEnd = now;
  uint32_t timeOn = 0, timeOff = 0;

  for (int i = 0; i < logCount; i++) {
    uint32_t entryEnd = logs[i].timestamp;
    uint32_t entryStart = entryEnd - logs[i].duration;
   
    uint32_t overlapStart = (entryStart > tStart) ? entryStart : tStart;
    uint32_t overlapEnd = (entryEnd < tEnd) ? entryEnd : tEnd;
   
    if (overlapStart < overlapEnd) {
      uint32_t dur = overlapEnd - overlapStart;
      if (!logs[i].state) timeOn += dur;
      else timeOff += dur;              
    }
  }

  uint32_t currStart = lastStateChangeTime;
  uint32_t currOverlapStart = (currStart > tStart) ? currStart : tStart;
  if (currOverlapStart < tEnd) {
    uint32_t dur = tEnd - currOverlapStart;
    if (currentState) timeOn += dur;
    else timeOff += dur;
  }

  String res = "<b>📊 ЗВІТ ПО ЕЛЕКТРОМЕРЕЖІ (Останні 7 днів)</b>\n";
  res += "📍 <b>Адреса:</b> " + String(config.locationName) + "\n\n";
  res += "🟢 <b>Час зі світлом:</b> " + formatDuration(timeOn) + "\n";
  res += "🔴 <b>Час без світла:</b> " + formatDuration(timeOff) + "\n";
  return res;
}

void handleTelegramCommands() {
  if (bot == NULL) return;
  int numNewMessages = bot->getUpdates(bot->last_message_received + 1);

  while (numNewMessages) {
    isInternetConnected = true;
    for (int i = 0; i < numNewMessages; i++) {
      String text = bot->messages[i].text;
      String from_id = String(bot->messages[i].chat_id);
      text.trim();

      if (text.indexOf("/status") != -1 || text.indexOf("Стан") != -1) {
        time_t now = time(NULL);
        String currStateStr = currentState ? "🟢 Світло є" : "🔴 Світла немає";
        String durationLabel = currentState ? "⏱️ <b>Світло є вже:</b> " : "⏱️ <b>Світла немає вже:</b> ";
       
        String msg = "<b>Поточний стан:</b> " + currStateStr + "\n";
        msg += "📍 <b>Адреса:</b> " + String(config.locationName) + "\n";
        msg += "📅 <b>Дата, час:</b> " + formatTime(now) + "\n";
        msg += durationLabel + formatDuration((uint32_t)(now - lastStateChangeTime)) + "\n\n";
        msg += "<b>Останні 5 подій:</b>\n";
       
        int startIdx = (logCount > 5) ? (logCount - 5) : 0;
        for (int j = logCount - 1; j >= startIdx; j--) {
          msg += (logs[j].state ? "🟢 З'явилося: " : "🔴 Зникло: ") + formatTime(logs[j].timestamp) + " (" + (logs[j].state ? "не було " : "було ") + formatDuration(logs[j].duration) + ")\n";
        }
        bot->sendMessageWithReplyKeyboard(from_id, msg, "HTML", kbdMarkup, true, false, false);
      }
      else if (text.indexOf("/report") != -1 || text.indexOf("Звіт") != -1) {
        bot->sendMessageWithReplyKeyboard(from_id, generateReportText(7 * 24 * 3600), "HTML", kbdMarkup, true, false, false);
      }
      else if (text == "/start") {
        bot->sendMessageWithReplyKeyboard(from_id, "Привіт! Я бот для моніторингу електромережі SvitloCheck Pro. Скористайтеся кнопками нижче:", "HTML", kbdMarkup, true, false, false);
      }
    }
    numNewMessages = bot->getUpdates(bot->last_message_received + 1);
  }
}

// ================= ВЕБ-СЕРВЕР =================
void handleRoot() {
  String html = R"rawliteral(<!DOCTYPE html>
<html lang="uk">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>SvitloCheck Pro — Моніторинг мережі живлення</title>
<!-- Неблокуючі іконки та скрипти графіків -->
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/@phosphor-icons/web@2.1.1/src/regular/style.css" media="print" onload="this.media='all'">
<script src="https://cdn.jsdelivr.net/npm/chart.js" defer></script>
<script src="https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns" defer></script>
<style>
    :root, [data-theme="slate"] {
        --bg-body: #070B14; --bg-card: rgba(17, 25, 40, 0.88); --bg-sub: rgba(30, 41, 59, 0.6);
        --text-primary: #F8FAFC; --text-secondary: #94A3B8; --text-muted: #64748B;
        --accent: #38BDF8; --accent-glow: rgba(56, 189, 248, 0.35); --success: #4ADE80;
        --danger: #FB7185; --warning: #F59E0B; --border: rgba(255, 255, 255, 0.08);
        --chart-grid: rgba(255, 255, 255, 0.06); --hero-grad-start: #FFFFFF; --hero-grad-end: #9B9B9B;
        --glow-opacity: 0.24;
    }
    [data-theme="black"] { --bg-body: #000000; --bg-card: #181818; --bg-sub: #1F1F1F; --text-primary: #F8FAFC; --text-secondary: #A1A1AA; --text-muted: #71717A; --accent: #38BDF8; --accent-glow: rgba(56, 189, 248, 0.3); --success: #10B981; --danger: #F43F5E; --warning: #F59E0B; --border: #272727; --chart-grid: #272727; --hero-grad-start: #FFFFFF; --hero-grad-end: #9B9B9B; --glow-opacity: 0.18; }
    [data-theme="light"] { --bg-body: #F1F5F9; --bg-card: rgba(255, 255, 255, 0.95); --bg-sub: #F8FAFC; --text-primary: #0F172A; --text-secondary: #475569; --text-muted: #94A3B8; --accent: #0284C7; --accent-glow: rgba(2, 132, 199, 0.25); --success: #16A34A; --danger: #E11D48; --warning: #D97706; --border: rgba(15, 23, 42, 0.1); --chart-grid: rgba(15, 23, 42, 0.08); --hero-grad-start: #000000; --hero-grad-end: #666666; --glow-opacity: 0.16; }
    [data-theme="amber"] { --bg-body: #131209; --bg-card: rgba(28, 26, 17, 0.9); --bg-sub: rgba(42, 39, 25, 0.65); --text-primary: #FEFCE8; --text-secondary: #D6D3D1; --text-muted: #78716C; --accent: #F59E0B; --accent-glow: rgba(245, 158, 11, 0.35); --success: #4ADE80; --danger: #FB7185; --warning: #FBBF24; --border: rgba(245, 158, 11, 0.14); --chart-grid: rgba(255, 255, 255, 0.06); --hero-grad-start: #FFFFFF; --hero-grad-end: #9B9B9B; --glow-opacity: 0.22; }
    [data-theme="emerald"] { --bg-body: #041311; --bg-card: rgba(6, 30, 26, 0.9); --bg-sub: rgba(11, 46, 40, 0.6); --text-primary: #ECFDF5; --text-secondary: #6EE7B7; --text-muted: #34D399; --accent: #10B981; --accent-glow: rgba(16, 185, 129, 0.35); --success: #34D399; --danger: #FB7185; --warning: #FBBF24; --border: rgba(52, 211, 153, 0.14); --chart-grid: rgba(52, 211, 153, 0.08); --hero-grad-start: #FFFFFF; --hero-grad-end: #9B9B9B; --glow-opacity: 0.25; }
    [data-theme="violet"] { --bg-body: #0D0718; --bg-card: rgba(24, 14, 41, 0.9); --bg-sub: rgba(38, 22, 64, 0.6); --text-primary: #F5F3FF; --text-secondary: #C4B5FD; --text-muted: #8B5CF6; --accent: #A855F7; --accent-glow: rgba(168, 85, 247, 0.38); --success: #4ADE80; --danger: #FB7185; --warning: #F59E0B; --border: rgba(168, 85, 247, 0.16); --chart-grid: rgba(168, 85, 247, 0.08); --hero-grad-start: #FFFFFF; --hero-grad-end: #9B9B9B; --glow-opacity: 0.26; }
    :root {
        --font-sans: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, system-ui, sans-serif;
        --font-mono: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", monospace;
        --ease-base: cubic-bezier(0.22, 1, 0.36, 1); --ease-layout: 300ms var(--ease-base); --ease-color: 200ms var(--ease-base);
    }
    * { box-sizing: border-box; }
    *:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
    body { margin: 0; background-color: var(--bg-body); color: var(--text-primary); font-family: var(--font-sans); padding: 24px 16px 40px; display: flex; justify-content: center; overflow-x: hidden; min-height: 100vh; position: relative; transition: background-color var(--ease-color), color var(--ease-color); -webkit-font-smoothing: antialiased; }
    .ambient-glow { position: fixed; top: -180px; left: 50%; transform: translateX(-50%); width: 900px; height: 450px; border-radius: 50%; filter: blur(130px); opacity: var(--glow-opacity); pointer-events: none; z-index: 0; transition: background var(--ease-layout), opacity var(--ease-color); }
    .glow-on { background: radial-gradient(circle, var(--success), transparent 70%); } .glow-off { background: radial-gradient(circle, var(--danger), transparent 70%); }
    .heading-2 { font-size: 38px; line-height: 42px; font-weight: 700; letter-spacing: -0.02em; } .heading-3 { font-size: 32px; line-height: 36px; font-weight: 700; letter-spacing: -0.02em; } .heading-4 { font-size: 26px; line-height: 32px; font-weight: 600; letter-spacing: -0.02em; } .heading-5 { font-size: 22px; line-height: 28px; font-weight: 600; } .title-1 { font-size: 19px; line-height: 26px; font-weight: 600; } .title-3 { font-size: 15px; line-height: 22px; font-weight: 600; } .body-2 { font-size: 15px; line-height: 22px; font-weight: 500; } .body-5 { font-size: 13.5px; line-height: 20px; font-weight: 500; } .caption-2 { font-size: 13px; line-height: 18px; font-weight: 400; } .caption-4 { font-size: 12px; line-height: 16px; font-weight: 500; } .mono-val { font-family: var(--font-mono); }
    .container { width: 100%; max-width: 1180px; position: relative; z-index: 1; }
    .demo-bar { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 12px; background: var(--bg-card); border: 1px solid var(--border); border-radius: 14px; padding: 10px 14px; margin-bottom: 20px; }
    .demo-bar-left { display: flex; align-items: center; gap: 8px; color: var(--text-secondary); }
    .demo-controls { display: flex; gap: 6px; flex-wrap: wrap; align-items: center; }
    .theme-dot { width: 10px; height: 10px; border-radius: 50%; display: inline-block; border: 1px solid rgba(255,255,255,0.3); }
    .top-nav { display: flex; justify-content: space-between; align-items: flex-end; flex-wrap: wrap; gap: 16px; margin-bottom: 20px; }
    .loc-pill { display: inline-flex; align-items: center; gap: 8px; color: var(--accent); letter-spacing: 0.03em; text-transform: uppercase; margin-bottom: 6px; font-weight: 600; }
    .header-gradient { background: linear-gradient(to right, var(--hero-grad-start), var(--hero-grad-end)); -webkit-background-clip: text; -webkit-text-fill-color: transparent; margin: 0; }
    .nav-pills { display: flex; gap: 10px; flex-wrap: wrap; }
    .status-pill { background: var(--bg-card); border: 1px solid var(--border); border-radius: 12px; padding: 6px 14px; display: flex; align-items: center; gap: 8px; }
    .dot { width: 8px; height: 8px; border-radius: 50%; display: inline-block; } .dot-online { background-color: var(--success); box-shadow: 0 0 8px var(--success); } .dot-offline { background-color: var(--danger); box-shadow: 0 0 8px var(--danger); }
    .bento-grid { display: grid; grid-template-columns: repeat(12, 1fr); gap: 20px; }
    .col-12 { grid-column: span 12; } .col-6 { grid-column: span 6; } .col-3 { grid-column: span 3; }
    .card { background: var(--bg-card); border-radius: 14px; padding: 20px; border: 1px solid var(--border); box-shadow: 0 8px 24px -8px rgba(0, 0, 0, 0.4); transition: background-color var(--ease-color), border-color var(--ease-color); }
    .card-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 18px; gap: 12px; flex-wrap: wrap; }
    .card-title { display: flex; align-items: center; gap: 8px; color: var(--text-primary); margin: 0 0 4px 0; }
    .card-subtitle { color: var(--text-secondary); margin: 0; }
    .hero-card { display: grid; grid-template-columns: 1.6fr 1fr; gap: 24px; align-items: center; }
    .hero-on { border-color: rgba(74, 222, 128, 0.4); box-shadow: 0 0 24px -6px rgba(74, 222, 128, 0.2); } .hero-off { border-color: rgba(251, 113, 133, 0.4); box-shadow: 0 0 24px -6px rgba(251, 113, 133, 0.2); }
    .hero-status-row { display: flex; align-items: center; gap: 20px; margin: 12px 0 18px; }
    .icon-container { display: flex; justify-content: center; align-items: center; width: 56px; height: 56px; border-radius: 14px; font-size: 28px; border: 1px solid var(--border); background: var(--bg-sub); }
    .icon-state-on { color: var(--success); border-color: rgba(74, 222, 128, 0.4); } .icon-state-off { color: var(--danger); border-color: rgba(251, 113, 133, 0.4); }
    .status-text-on { color: var(--success); } .status-text-off { color: var(--danger); }
    .hero-timer-box { background: var(--bg-sub); border: 1px solid var(--border); border-radius: 8px; padding: 12px 20px; display: inline-block; min-width: 260px; }
    .duration-sub { color: var(--text-secondary); margin-top: 6px; display: flex; align-items: center; gap: 6px; }
    .hero-sla-box { display: flex; flex-direction: column; align-items: center; justify-content: center; position: relative; height: 210px; }
    .sla-center-label { position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); text-align: center; pointer-events: none; }
    .kpi-card { display: flex; flex-direction: column; justify-content: space-between; min-height: 110px; }
    .kpi-top { display: flex; justify-content: space-between; align-items: center; color: var(--text-secondary); } .kpi-val { margin: 8px 0 4px; }
    .analytics-controls { display: flex; justify-content: space-between; align-items: flex-end; flex-wrap: wrap; gap: 16px; margin-bottom: 18px; }
    .flex-dates { display: flex; gap: 10px; flex-wrap: wrap; }
    .summary-strips { display: grid; grid-template-columns: repeat(2, 1fr); gap: 14px; margin-bottom: 18px; }
    .strip-box { background: var(--bg-sub); border: 1px solid var(--border); border-radius: 8px; padding: 12px 18px; display: flex; justify-content: space-between; align-items: center; }
    .chart-timeline-box { position: relative; height: 140px; width: 100%; }
    .res-item { margin-bottom: 14px; } .res-label { display: flex; justify-content: space-between; margin-bottom: 6px; color: var(--text-secondary); }
    .progress-track { width: 100%; height: 8px; background: var(--bg-sub); border: 1px solid var(--border); border-radius: 8px; overflow: hidden; }
    .progress-fill { height: 100%; background: var(--text-secondary); border-radius: 8px; transform-origin: left; transition: transform 300ms ease; }
    .fill-accent { background: var(--accent); } .fill-success { background: var(--success); } .fill-warning { background: var(--warning); } .fill-danger { background: var(--danger); }
    .form-group { margin-bottom: 14px; position: relative; } .form-group label { display: block; color: var(--text-secondary); margin-bottom: 6px; }
    input { width: 100%; padding: 10px 14px; border-radius: 8px; border: 1px solid var(--border); background-color: var(--bg-sub); color: var(--text-primary); font-family: var(--font-sans); font-size: 14.5px; }
    input:focus { outline: none; border-color: var(--accent); box-shadow: 0 0 14px var(--accent-glow); }
    .pass-wrapper { position: relative; } .toggle-pass { position: absolute; right: 10px; top: 50%; transform: translateY(-50%); background: none; border: none; color: var(--text-secondary); cursor: pointer; font-size: 16px; padding: 4px; }
    .btn-row { display: flex; gap: 10px; margin-top: 18px; flex-wrap: wrap; }
    .btn { background: var(--accent); color: #000000; padding: 8px 15px; border-radius: 8px; border: 1px solid transparent; cursor: pointer; display: inline-flex; align-items: center; justify-content: center; gap: 6px; font-weight: 600; text-decoration: none; }
    .btn:hover { opacity: 0.9; } .btn-secondary { background: var(--bg-sub); border: 1px solid var(--border); color: var(--text-primary); font-weight: 500; }
    .btn-secondary:hover { border-color: var(--accent); } .btn-secondary.active { background: var(--accent); color: #000000; font-weight: 600; }
    .btn-danger { background: transparent; border: 1px solid var(--danger); color: var(--danger); } .btn-danger:hover { background: var(--danger); color: #FFFFFF; }
    .btn-row-del { background: transparent; border: 1px solid var(--border); color: var(--text-secondary); width: 30px; height: 30px; border-radius: 6px; display: inline-flex; align-items: center; justify-content: center; cursor: pointer; }
    .btn-row-del:hover { background: rgba(244, 63, 94, 0.15); border-color: var(--danger); color: var(--danger); }
    .table-wrap { max-height: 320px; overflow-y: auto; border-radius: 8px; border: 1px solid var(--border); background: var(--bg-sub); }
    table { width: 100%; border-collapse: collapse; font-size: 13.5px; } th, td { border-bottom: 1px solid var(--border); padding: 10px 14px; text-align: left; } th { color: var(--text-secondary); background: var(--bg-card); position: sticky; top: 0; z-index: 2; }
    tbody tr:hover { background: rgba(255, 255, 255, 0.03); }
    .state-badge { display: inline-flex; align-items: center; gap: 6px; padding: 3px 10px; border-radius: 6px; border: 1px solid var(--border); }
    .badge-on { background: rgba(74, 222, 128, 0.12); color: var(--success); border-color: rgba(74, 222, 128, 0.3); } .badge-off { background: rgba(251, 113, 133, 0.12); color: var(--danger); border-color: rgba(251, 113, 133, 0.3); }
    .empty-state { display: flex; flex-direction: column; align-items: center; justify-content: center; padding: 30px 20px; color: var(--text-muted); text-align: center; }
    .legal-footer { margin-top: 32px; padding-top: 20px; border-top: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 14px; color: var(--text-muted); }
    .legal-links { display: flex; gap: 14px; flex-wrap: wrap; }
    .legal-link { color: var(--text-secondary); text-decoration: none; cursor: pointer; background: none; border: none; font-family: inherit; font-size: 13px; padding: 0; }
    .legal-link:hover { color: var(--accent); text-decoration: underline; }
    .modal-overlay { position: fixed; inset: 0; background: rgba(0, 0, 0, 0.75); display: none; justify-content: center; align-items: center; z-index: 100; padding: 16px; }
    .modal-overlay.open { display: flex; }
    .modal-box { background: var(--bg-card); border: 1px solid var(--border); border-radius: 14px; max-width: 650px; width: 100%; max-height: 85vh; display: flex; flex-direction: column; }
    .modal-header { padding: 16px 20px; border-bottom: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; }
    .modal-body { padding: 20px; overflow-y: auto; color: var(--text-secondary); line-height: 1.6; font-size: 13.5px; }
    .modal-body h4 { color: var(--text-primary); margin: 14px 0 6px 0; font-size: 15px; }
    .modal-body h4:first-child { margin-top: 0; }
    .modal-close { background: var(--bg-sub); border: 1px solid var(--border); color: var(--text-primary); width: 30px; height: 30px; border-radius: 6px; cursor: pointer; display: flex; align-items: center; justify-content: center; }
    @media (max-width: 960px) { .hero-card { grid-template-columns: 1fr; } .col-6, .col-3 { grid-column: span 12; } .summary-strips { grid-template-columns: 1fr; } .legal-footer { flex-direction: column; text-align: center; } }
</style>
</head>
<body data-theme="slate">
<div id="ambientGlow" class="ambient-glow glow-on"></div>
<div class="container">
    <div class="demo-bar">
        <div class="demo-bar-left caption-2"><i class="ph ph-palette"></i><span>Тема:</span></div>
        <div class="demo-controls" id="themeButtons">
            <button class="btn btn-secondary active caption-2" data-theme-btn="slate" onclick="setTheme('slate')"><span class="theme-dot" style="background:#070B14;"></span> Темна</button>
            <button class="btn btn-secondary caption-2" data-theme-btn="black" onclick="setTheme('black')"><span class="theme-dot" style="background:#000000;"></span> Чорна</button>
            <button class="btn btn-secondary caption-2" data-theme-btn="light" onclick="setTheme('light')"><span class="theme-dot" style="background:#F1F5F9;"></span> Світла</button>
            <button class="btn btn-secondary caption-2" data-theme-btn="amber" onclick="setTheme('amber')"><span class="theme-dot" style="background:#F59E0B;"></span> Бурштин</button>
            <button class="btn btn-secondary caption-2" data-theme-btn="emerald" onclick="setTheme('emerald')"><span class="theme-dot" style="background:#10B981;"></span> Смарагд</button>
            <button class="btn btn-secondary caption-2" data-theme-btn="violet" onclick="setTheme('violet')"><span class="theme-dot" style="background:#A855F7;"></span> Аметист</button>
        </div>
    </div>

    <header class="top-nav">
        <div>
            <div class="loc-pill caption-4"><i class="ph ph-map-pin"></i> Об'єкт: <span id="locName" style="color: var(--text-primary);">Завантаження...</span></div>
            <h1 class="heading-2 header-gradient">SvitloCheck Telemetry</h1>
        </div>
        <div class="nav-pills">
            <div class="status-pill body-5"><span id="netDot" class="dot dot-online"></span><span>Інтернет: <span class="mono-val" id="sysInternet">...</span></span></div>
            <div class="status-pill body-5"><i class="ph ph-clock"></i><span>Uptime: <span class="mono-val" id="sysUptime">...</span></span></div>
        </div>
    </header>

    <div class="bento-grid">
        <div class="card col-12 hero-card hero-on" id="cardState">
            <div>
                <div class="caption-4" style="color: var(--text-secondary); text-transform: uppercase;">Поточний стан електромережі</div>
                <div class="hero-status-row">
                    <div id="statusIcon" class="icon-container icon-state-on"><i class="ph ph-lightbulb"></i></div>
                    <div><div id="currState" class="heading-3 status-text-on">СВІТЛО Є</div><div class="body-5" style="color: var(--text-secondary); margin-top: 4px;" id="stateSubDesc">Живлення стабільне</div></div>
                </div>
                <div class="hero-timer-box">
                    <div class="heading-4 mono-val" id="currDuration">...</div>
                    <div class="duration-sub caption-2"><span>Тривалість поточного стану</span></div>
                </div>
            </div>
            <div class="hero-sla-box">
                <canvas id="slaChart"></canvas>
                <div class="sla-center-label"><span class="heading-5 mono-val" id="slaCenterPct">--%</span><span class="caption-4" style="color: var(--text-secondary); display: block;">Зі світлом</span></div>
            </div>
        </div>

        <div class="card col-3 kpi-card"><div class="kpi-top caption-2"><span>Індекс SLA</span><i class="ph ph-chart-line-up"></i></div><div class="kpi-val heading-5 mono-val" style="color: var(--success);" id="kpiSla">--%</div></div>
        <div class="card col-3 kpi-card"><div class="kpi-top caption-2"><span>Відключень</span><i class="ph ph-lightning"></i></div><div class="kpi-val heading-5 mono-val" id="kpiOutages">...</div></div>
        <div class="card col-3 kpi-card"><div class="kpi-top caption-2"><span>Температура чипа</span><i class="ph ph-thermometer-simple"></i></div><div class="kpi-val heading-5 mono-val" id="kpiTemp" style="color: var(--accent);">-- °C</div></div>
        <div class="card col-3 kpi-card"><div class="kpi-top caption-2"><span>Записів у Flash</span><i class="ph ph-floppy-disk"></i></div><div class="kpi-val heading-5 mono-val" style="color: var(--accent);" id="kpiLogs">... / 500</div></div>

        <div class="card col-12">
            <div class="analytics-controls">
                <div><h3 class="title-1 card-title"><i class="ph ph-trend-up"></i> Аналітика мережі</h3></div>
                <div class="flex-dates">
                    <div class="form-group" style="margin: 0;"><label class="caption-4">Період від</label><input type="date" class="body-5" id="dateStart" onchange="updateReport()"></div>
                    <div class="form-group" style="margin: 0;"><label class="caption-4">Період до</label><input type="date" class="body-5" id="dateEnd" onchange="updateReport()"></div>
                </div>
            </div>
            <div class="summary-strips">
                <div class="strip-box"><div><div class="caption-2" style="color: var(--text-secondary);">Час зі світлом</div><div class="title-1 mono-val" style="margin-top: 2px;" id="repOnText">...</div></div><div class="heading-5 mono-val" style="color: var(--success);" id="repOnPct">--%</div></div>
                <div class="strip-box"><div><div class="caption-2" style="color: var(--text-secondary);">Час без світла</div><div class="title-1 mono-val" style="margin-top: 2px;" id="repOffText">...</div></div><div class="heading-5 mono-val" style="color: var(--danger);" id="repOffPct">--%</div></div>
            </div>
            <div class="chart-timeline-box"><canvas id="tlChart"></canvas></div>
        </div>

        <div class="card col-6">
            <div class="card-header"><div><h3 class="title-1 card-title"><i class="ph ph-cpu"></i> Телеметрія мікроконтролера</h3></div><span class="caption-4 mono-val" id="cpuLoad">...</span></div>
            <div class="res-item"><div class="res-label body-5"><span>Температура процесора (ESP32)</span><span class="mono-val" id="tempValText">...</span></div><div class="progress-track"><div class="progress-fill fill-accent" id="tempBar" style="transform: scaleX(0);"></div></div></div>
            <div class="res-item"><div class="res-label body-5"><span>Пінг до сервера (8.8.8.8)</span><span class="mono-val" id="pingValText">...</span></div><div class="progress-track"><div class="progress-fill fill-success" id="pingBar" style="transform: scaleX(0);"></div></div></div>
            <div class="res-item"><div class="res-label body-5"><span>Оперативна пам'ять (RAM)</span><span class="mono-val" id="ramUsageText">...</span></div><div class="progress-track"><div class="progress-fill fill-accent" id="ramBar" style="transform: scaleX(0);"></div></div></div>
            <div class="res-item"><div class="res-label body-5"><span>Wi-Fi сигнал (<span style="color: var(--accent);" id="wifiSsid">...</span>)</span><span class="mono-val" id="wifiSignalText">...</span></div><div class="progress-track"><div class="progress-fill fill-warning" id="wifiBar" style="transform: scaleX(0);"></div></div></div>
        </div>

        <div class="card col-6">
            <div class="card-header"><div><h3 class="title-1 card-title"><i class="ph ph-gear"></i> Налаштування Telegram</h3></div></div>
            <form action="/save" method="POST">
                <div class="form-group"><label class="body-5">Назва об'єкта / локації:</label><input type="text" name="location" id="inpLocation" value=""></div>
                <div class="form-group"><label class="body-5">Telegram Bot Token:</label>
                    <div class="pass-wrapper">
                        <input type="password" name="token" id="inpToken" value="">
                        <button type="button" class="toggle-pass" onclick="toggleTokenVis()"><i class="ph ph-eye" id="eyeIcon"></i></button>
                    </div>
                </div>
                <div class="form-group"><label class="body-5">Chat IDs (через кому):</label><input type="text" name="chats" id="inpChats" value=""></div>
                <div class="btn-row">
                    <button type="submit" class="btn body-2"><i class="ph ph-floppy-disk"></i> Зберегти</button>
                    <button type="button" class="btn btn-secondary body-2" onclick="location.href='/wifi'"><i class="ph ph-wifi-high"></i> Змінити Wi-Fi</button>
                </div>
            </form>
        </div>

        <div class="card col-12">
            <div class="card-header">
                <div>
                    <h3 class="title-1 card-title"><i class="ph ph-list-dashes"></i> Журнал подій енергомережі</h3>
                    <p class="body-5 card-subtitle">Останні збережені події перемикання мережі</p>
                </div>
                <div style="display: flex; gap: 8px; flex-wrap: wrap;">
                    <button class="btn btn-secondary active caption-2" id="fltAll" onclick="filterLogs('all')">Всі події</button>
                    <button class="btn btn-secondary caption-2" id="fltOn" onclick="filterLogs('on')">З'явилось</button>
                    <button class="btn btn-secondary caption-2" id="fltOff" onclick="filterLogs('off')">Зникло</button>
                    <a href="/api/export" download="svitlocheck_history.csv" class="btn btn-secondary caption-2"><i class="ph ph-download-simple"></i> CSV (усі 500)</a>
                    <button class="btn btn-danger caption-2" onclick="clearHistory()"><i class="ph ph-trash"></i> Очистити</button>
                </div>
            </div>
            <div class="table-wrap">
                <table>
                    <thead class="caption-2"><tr><th>#</th><th>Дата</th><th>Час</th><th>Подія</th><th>Минулий стан</th><th>Telegram</th><th style="text-align: right;">Дія</th></tr></thead>
                    <tbody class="body-5" id="logTable"></tbody>
                </table>
            </div>
        </div>

        <!-- Блок авторизації розробника -->
        <div class="card col-12" id="devAuthCard" style="display: none; border: 1px dashed var(--accent);">
            <div class="card-header" style="border-bottom: none; padding-bottom: 0;">
                <div>
                    <h3 class="title-1 card-title" style="color: var(--accent); margin: 0;"><i class="ph ph-terminal-window"></i> Режим розробника</h3>
                    <p class="body-5 card-subtitle" style="margin-top: 4px;">Введіть пароль для доступу до сервісних параметрів</p>
                </div>
                <button type="button" class="btn btn-secondary caption-2" onclick="closeDevAuthCard()"><i class="ph ph-x"></i> Скасувати</button>
            </div>
            <div style="padding: 14px 20px 20px; max-width: 460px;">
                <div style="display: flex; gap: 8px;">
                    <input type="password" id="devPassInput" class="body-5" placeholder="Пароль розробника" onkeydown="if(event.key==='Enter')checkDevAuth()">
                    <button type="button" class="btn body-5" onclick="checkDevAuth()"><i class="ph ph-sign-in"></i> Увійти</button>
                </div>
                <div id="devAuthError" style="color: var(--danger); font-size: 13px; margin-top: 8px; display: none;"><i class="ph ph-warning-circle"></i> Невірний пароль!</div>
            </div>
        </div>

        <!-- Окремий блок розробника -->
        <div class="card col-12" id="devPanelCard" style="display: none; border: 1px solid var(--accent); box-shadow: 0 0 20px rgba(56, 189, 248, 0.15);">
            <div class="card-header" style="border-bottom: 1px solid var(--border);">
                <div>
                    <div style="display: flex; align-items: center; gap: 8px;">
                        <span style="display: inline-block; width: 8px; height: 8px; border-radius: 50%; background: var(--accent);"></span>
                        <h3 class="title-1 card-title" style="color: var(--accent); margin: 0;"><i class="ph ph-terminal-window"></i> Панель розробника (Device ID & Діагностика)</h3>
                    </div>
                    <p class="body-5 card-subtitle" style="margin-top: 4px;">Керування номером роутера та діагностичними журналами системи</p>
                </div>
                <div style="display: flex; gap: 8px; flex-wrap: wrap;">
                    <button type="button" class="btn btn-secondary caption-2" onclick="closeDevPanel()"><i class="ph ph-eye-slash"></i> Сховати (автоблок через 2 хв)</button>
                    <button type="button" class="btn btn-danger caption-2" onclick="lockDevPanelNow()"><i class="ph ph-lock-key"></i> Заблокувати зараз</button>
                </div>
            </div>
            
            <div style="padding: 20px; display: flex; flex-direction: column; gap: 20px;">
                <!-- Секція 1: Device ID -->
                <div style="background: rgba(56, 189, 248, 0.06); border: 1px solid var(--accent); border-radius: 10px; padding: 16px 18px;">
                    <div style="display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 8px; margin-bottom: 10px;">
                        <div style="font-weight: 600; color: var(--accent); font-size: 15px; display: flex; align-items: center; gap: 6px;">
                            <i class="ph ph-broadcast"></i> Унікальний номер пристрою (Hostname у роутері)
                        </div>
                        <span class="caption-4 mono-val" style="color: var(--accent); border: 1px solid var(--accent); padding: 2px 6px; border-radius: 4px;">DHCP Option 12</span>
                    </div>
                    <p class="body-5" style="color: var(--text-secondary); margin: 0 0 12px 0;">
                        Цей номер передається роутеру як <b>Hostname</b> (відображається в списку клієнтів роутера) та використовується як ім'я точки доступу Wi-Fi.
                    </p>
                    <div style="display: flex; gap: 10px; flex-wrap: wrap; max-width: 580px;">
                        <input type="text" id="inpDeviceId" class="body-5 mono-val" style="flex: 1; min-width: 220px;" placeholder="наприклад, SvitloCheck-01">
                        <button type="button" class="btn body-2" onclick="saveDeviceId()"><i class="ph ph-floppy-disk"></i> Зберегти</button>
                    </div>
                    <div id="devIdSavedAlert" style="color: var(--success); font-size: 13.5px; margin-top: 10px; display: none;">
                        <i class="ph ph-check-circle"></i> Номер збережено! Натисніть «Перезавантажити ESP32» нижче, щоб роутер оновив ім'я хоста.
                    </div>
                </div>

                <!-- Секція 2: Статус мережі -->
                <div>
                    <h4 class="title-3" style="margin: 0 0 10px 0;"><i class="ph ph-identification-card"></i> Статус у локальній мережі</h4>
                    <div style="background: var(--bg-sub); border: 1px solid var(--border); border-radius: 10px; padding: 14px 18px; font-family: var(--font-mono); font-size: 13.5px; display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 12px 18px;">
                        <div><div class="caption-4" style="color: var(--text-muted);">MAC-адреса:</div><div class="mono-val" id="devMac">...</div></div>
                        <div><div class="caption-4" style="color: var(--text-muted);">IP від роутера:</div><div class="mono-val" id="devIp" style="color: var(--success);">...</div></div>
                        <div><div class="caption-4" style="color: var(--text-muted);">Ім'я хоста (DHCP):</div><div class="mono-val" id="devHostname" style="color: var(--accent); font-weight: 600;">...</div></div>
                        <div><div class="caption-4" style="color: var(--text-muted);">Wi-Fi SSID:</div><div class="mono-val" id="devSsid">...</div></div>
                    </div>
                </div>

                <!-- Секція 3: Журнали розробника (Перегрів >68°C та Зв'язок з інтернетом) -->
                <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 16px;">
                    <!-- Журнал 1: Перегрів -->
                    <div style="background: var(--bg-sub); border: 1px solid var(--border); border-radius: 10px; padding: 14px 16px;">
                        <div style="display: flex; align-items: center; justify-content: space-between; margin-bottom: 10px;">
                            <div style="font-weight: 600; font-size: 14px; display: flex; align-items: center; gap: 6px; color: var(--danger);">
                                <i class="ph ph-warning"></i> Журнал перегріву (> 68 °C)
                            </div>
                            <span class="caption-4 mono-val" id="devTempBadge" style="color: var(--accent);">...</span>
                        </div>
                        <div class="table-wrap" style="max-height: 180px;">
                            <table>
                                <thead class="caption-4"><tr><th>#</th><th>Дата та час</th><th>Темп.</th></tr></thead>
                                <tbody class="caption-2 mono-val" id="tempLogTable">
                                    <tr><td colspan="3" style="text-align:center; color:var(--text-muted); padding:16px;">Перевищень >68°C не зафіксовано</td></tr>
                                </tbody>
                            </table>
                        </div>
                    </div>

                    <!-- Журнал 2: Інтернет -->
                    <div style="background: var(--bg-sub); border: 1px solid var(--border); border-radius: 10px; padding: 14px 16px;">
                        <div style="display: flex; align-items: center; justify-content: space-between; margin-bottom: 10px;">
                            <div style="font-weight: 600; font-size: 14px; display: flex; align-items: center; gap: 6px; color: var(--accent);">
                                <i class="ph ph-globe"></i> Журнал змін зв'язку з інтернетом
                            </div>
                            <span class="caption-4 mono-val" id="devPingBadge">...</span>
                        </div>
                        <div class="table-wrap" style="max-height: 180px;">
                            <table>
                                <thead class="caption-4"><tr><th>#</th><th>Дата та час</th><th>Статус</th><th>Пінг</th></tr></thead>
                                <tbody class="caption-2 mono-val" id="netLogTable">
                                    <tr><td colspan="4" style="text-align:center; color:var(--text-muted); padding:16px;">Змін статусу не зафіксовано</td></tr>
                                </tbody>
                            </table>
                        </div>
                    </div>
                </div>

                <!-- Секція 4: Швидкі дії -->
                <div>
                    <h4 class="title-3" style="margin: 0 0 10px 0;"><i class="ph ph-sliders"></i> Швидкі дії</h4>
                    <div style="display: flex; gap: 8px; flex-wrap: wrap;">
                        <button type="button" class="btn btn-secondary body-5" onclick="location.href='/wifi'"><i class="ph ph-wifi-high"></i> Змінити Wi-Fi</button>
                        <button type="button" class="btn btn-secondary body-5" onclick="document.getElementById('inpLocation').scrollIntoView({behavior:'smooth'});"><i class="ph ph-gear"></i> До Telegram</button>
                        <button type="button" class="btn btn-secondary body-5" onclick="document.getElementById('fltAll').scrollIntoView({behavior:'smooth'});"><i class="ph ph-list-dashes"></i> До журналу</button>
                        <button type="button" class="btn btn-secondary body-5" onclick="clearHistory()"><i class="ph ph-trash"></i> Очистити базу</button>
                        <button type="button" class="btn btn-danger body-5" onclick="restartDevice()"><i class="ph ph-arrows-clockwise"></i> Перезавантажити ESP32 (DHCP renew)</button>
                    </div>
                </div>
            </div>
        </div>
    </div>

    <footer class="legal-footer caption-2">
        <div>© 2026 <b>SvitloCheck Pro</b> — Апаратно-програмний комплекс моніторингу електромережі.</div>
        <div class="legal-links">
            <button class="legal-link" onclick="openModal('termsModal')"><i class="ph ph-file-text"></i> Умови та Гарантія</button>
            <button class="legal-link" onclick="openModal('safetyModal')"><i class="ph ph-shield-warning"></i> Відмова від відповідальності</button>
            <button class="legal-link" onclick="openModal('privacyModal')"><i class="ph ph-lock-key"></i> Конфіденційність</button>
            <button class="legal-link" style="color: var(--accent); font-weight: 600;" onclick="toggleDevSection()"><i class="ph ph-code"></i> Розробник</button>
        </div>
    </footer>
</div>

<div class="modal-overlay" id="termsModal" onclick="if(event.target===this)closeModal('termsModal')">
    <div class="modal-box">
        <div class="modal-header"><h3 class="title-1 card-title" style="margin:0;"><i class="ph ph-file-text"></i> Умови використання та Гарантія</h3><button class="modal-close" onclick="closeModal('termsModal')"><i class="ph ph-x"></i></button></div>
        <div class="modal-body">
            <h4>1. Призначення</h4><p>SvitloCheck Pro призначений для інформаційного моніторингу напруги в побутовій мережі 220В та відправки сповіщень через інтернет.</p>
            <h4>2. Гарантія</h4><p>Гарантійний термін складає 12 місяців за умови дотримання правил експлуатації та відсутності фізичних пошкоджень.</p>
        </div>
    </div>
</div>

<div class="modal-overlay" id="safetyModal" onclick="if(event.target===this)closeModal('safetyModal')">
    <div class="modal-box">
        <div class="modal-header"><h3 class="title-1 card-title" style="margin:0;"><i class="ph ph-shield-warning"></i> Відмова від відповідальності</h3><button class="modal-close" onclick="closeModal('safetyModal')"><i class="ph ph-x"></i></button></div>
        <div class="modal-body">
            <h4>Некритичне обладнання</h4><p>Пристрій не є сертифікованим лічильником або медичним обладнанням. Виробник не несе відповідальності за наслідки затримок сповіщень через збої зв'язку.</p>
        </div>
    </div>
</div>

<div class="modal-overlay" id="privacyModal" onclick="if(event.target===this)closeModal('privacyModal')">
    <div class="modal-box">
        <div class="modal-header"><h3 class="title-1 card-title" style="margin:0;"><i class="ph ph-lock-key"></i> Політика конфіденційності</h3><button class="modal-close" onclick="closeModal('privacyModal')"><i class="ph ph-x"></i></button></div>
        <div class="modal-body">
            <h4>100% Локальне зберігання</h4><p>Усі облікові дані (Wi-Fi, Telegram Bot Token, Chat ID) зберігаються виключно в пам'яті ESP32 (LittleFS) без надсилання на жодні хмарні сервери.</p>
        </div>
    </div>
</div>

<script>
    let slaChart = null, tlChart = null;
    let isFirstLoad = true, logsData = [];
    let currentFilter = 'all';
    let lastState = null;
    let lastWifi = -1;
    let lastInternet = null;
    let lastTotalLogs = -1;
    let liveDurationSec = 0;

    function openModal(id) { document.getElementById(id).classList.add('open'); }
    function closeModal(id) { document.getElementById(id).classList.remove('open'); }

    function setTheme(themeName) {
        document.body.setAttribute('data-theme', themeName);
        localStorage.setItem('svitlo_theme', themeName);
        document.querySelectorAll('[data-theme-btn]').forEach(btn => {
            btn.classList.toggle('active', btn.getAttribute('data-theme-btn') === themeName);
        });
        updateChartTheme();
    }

    function updateChartTheme() {
        if (!tlChart) return;
        const styles = getComputedStyle(document.body);
        tlChart.options.scales.x.ticks.color = styles.getPropertyValue('--text-secondary').trim();
        tlChart.options.scales.x.grid.color = styles.getPropertyValue('--chart-grid').trim();
        tlChart.options.scales.y.ticks.color = styles.getPropertyValue('--text-primary').trim();
        tlChart.update('none');
    }

    function formatDur(sec) {
        if (!sec || sec < 0) return '0 сек.';
        let d = Math.floor(sec / 86400), h = Math.floor((sec % 86400) / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
        let res = '';
        if (d > 0) res += d + ' дн. ';
        if (h > 0 || d > 0) res += h + ' год. ';
        res += m + ' хв. ' + s + ' сек.';
        return res;
    }

    // Локальний секундний таймер для плавності
    setInterval(() => {
        if (liveDurationSec >= 0) {
            liveDurationSec++;
            let el = document.getElementById('currDuration');
            if (el && !el.innerText.includes('Синхронізація')) {
                el.innerText = formatDur(liveDurationSec);
            }
        }
    }, 1000);

    function updateStateUI(isLightOn) {
        let st = document.getElementById('currState');
        let cd = document.getElementById('cardState');
        let ic = document.getElementById('statusIcon');
        let gl = document.getElementById('ambientGlow');
        let desc = document.getElementById('stateSubDesc');

        if (isLightOn) {
            st.className = "heading-3 status-text-on";
            st.innerText = "СВІТЛО Є";
            cd.className = "card col-12 hero-card hero-on";
            ic.className = "icon-container icon-state-on";
            ic.innerHTML = '<i class="ph ph-lightbulb"></i>';
            gl.className = "ambient-glow glow-on";
            desc.innerText = "Живлення від основної мережі стабільне";
        } else {
            st.className = "heading-3 status-text-off";
            st.innerText = "СВІТЛА НЕМАЄ";
            cd.className = "card col-12 hero-card hero-off";
            ic.className = "icon-container icon-state-off";
            ic.innerHTML = '<i class="ph ph-plug"></i>';
            gl.className = "ambient-glow glow-off";
            desc.innerText = "Аварійне або планове відключення лінії живлення";
        }
    }

    // === ШВИДКЕ ОПИТУВАННЯ (кожні 800мс): МОМЕНТАЛЬНЕ ОНОВЛЕННЯ СВІТЛА, WI-FI, ТЕМПЕРАТУРИ ТА ПІНГУ ===
    let isQuickFetching = false;
    function quickPoll() {
        if (isQuickFetching) return;
        isQuickFetching = true;

        fetch('/api/quick?t=' + Date.now()).then(r => r.json()).then(q => {
            isQuickFetching = false;

            // 1. Моментальний статус мережі 220В
            if (q.s !== lastState) {
                lastState = q.s;
                updateStateUI(q.s);
                liveDurationSec = q.d;
                document.getElementById('currDuration').innerText = formatDur(liveDurationSec);
                loadFullData();
                updateReport();
            }

            // 2. Сигнал Wi-Fi
            if (q.w !== lastWifi) {
                lastWifi = q.w;
                document.getElementById('wifiSignalText').innerText = q.w + '% (' + q.r + ' dBm)';
                document.getElementById('wifiBar').style.transform = `scaleX(${q.w / 100})`;
            }

            // 3. Температура мікроконтролера
            if (q.temp !== undefined) {
                let tempStr = q.temp.toFixed(1) + ' °C';
                document.getElementById('kpiTemp').innerText = tempStr;
                document.getElementById('tempValText').innerText = tempStr;
                let tBar = document.getElementById('tempBar');
                tBar.style.transform = `scaleX(${Math.min(q.temp / 100, 1.0)})`;
                if (q.temp >= 68) {
                    tBar.className = "progress-fill fill-danger";
                    document.getElementById('kpiTemp').style.color = "var(--danger)";
                } else if (q.temp >= 55) {
                    tBar.className = "progress-fill fill-warning";
                    document.getElementById('kpiTemp').style.color = "var(--warning)";
                } else {
                    tBar.className = "progress-fill fill-accent";
                    document.getElementById('kpiTemp').style.color = "var(--accent)";
                }
            }

            // 4. Пінг до сервера та статус інтернету
            let pingStr = (q.p > 0) ? (q.p + ' ms') : 'Таймаут';
            document.getElementById('pingValText').innerText = pingStr;
            let pBar = document.getElementById('pingBar');
            if (q.p > 0) {
                pBar.style.transform = `scaleX(${Math.min(q.p / 300, 1.0)})`;
                pBar.className = (q.p < 80) ? "progress-fill fill-success" : "progress-fill fill-warning";
            } else {
                pBar.style.transform = "scaleX(0)";
            }

            if (q.i !== lastInternet) {
                lastInternet = q.i;
                let intEl = document.getElementById('sysInternet'), dotEl = document.getElementById('netDot');
                if (q.i) {
                    intEl.innerText = "Є зв'язок (" + (q.p > 0 ? q.p + " ms" : "OK") + ")";
                    intEl.style.color = "var(--success)";
                    dotEl.className = "dot dot-online";
                } else {
                    intEl.innerText = "Відсутній";
                    intEl.style.color = "var(--danger)";
                    dotEl.className = "dot dot-offline";
                }
            } else if (q.i) {
                document.getElementById('sysInternet').innerText = "Є зв'язок (" + (q.p > 0 ? q.p + " ms" : "OK") + ")";
            }

            // Якщо змінилась кількість логів
            if (q.l !== lastTotalLogs) {
                lastTotalLogs = q.l;
                document.getElementById('kpiLogs').innerText = `${q.l} / 500`;
                loadFullData();
                updateReport();
            }

            if (Math.abs(liveDurationSec - q.d) > 3) {
                liveDurationSec = q.d;
            }
        }).catch(() => { isQuickFetching = false; });
    }

    // === ПОВНЕ ОНОВЛЕННЯ ДАНИХ (раз на 5 секунд) ===
    let isFullFetching = false;
    function loadFullData() {
        if (isFullFetching) return;
        isFullFetching = true;

        fetch('/api/data?t=' + Date.now()).then(r => r.json()).then(d => {
            isFullFetching = false;
            document.getElementById('locName').innerText = d.location;
           
            if (isFirstLoad) {
                document.getElementById('inpLocation').value = d.location || '';
                document.getElementById('inpToken').value = d.botToken || '';
                document.getElementById('inpChats').value = d.chatIds || '';

                let firstDate = new Date((d.firstLogTime || Math.floor(Date.now()/1000)) * 1000);
                if (firstDate.getFullYear() < 2020) firstDate = new Date();
                const off = firstDate.getTimezoneOffset() * 60000;
                document.getElementById('dateStart').value = new Date(firstDate.getTime() - off).toISOString().split('T')[0];
                document.getElementById('dateEnd').value = new Date(Date.now() - off).toISOString().split('T')[0];
                isFirstLoad = false;
                setTimeout(updateReport, 200);
            }

            if (lastState === null) {
                lastState = d.currentState;
                updateStateUI(d.currentState);
                if (d.durationSec !== undefined) liveDurationSec = d.durationSec;
            }

            document.getElementById('ramUsageText').innerText = d.ramUsage;
            if(d.ramUsage) { document.getElementById('ramBar').style.transform = `scaleX(${parseFloat(d.ramUsage)/100})`; }
            document.getElementById('wifiSsid').innerText = d.wifiSsid || 'N/A';
            document.getElementById('cpuLoad').innerText = d.cpuLoad || '...';
            document.getElementById('sysUptime').innerText = d.uptime || '...';

            logsData = d.logs || [];
            let total = (d.totalLogs !== undefined) ? d.totalLogs : logsData.length;
            document.getElementById('kpiLogs').innerText = `${total} / 500`;

            window.lastDevData = d;
            if (document.getElementById('devPanelCard') && document.getElementById('devPanelCard').style.display !== 'none') {
                syncDevPanel();
            }

            renderLogsTable();
        }).catch(() => { isFullFetching = false; });
    }

    function renderLogsTable() {
        let tbody = document.getElementById('logTable');
        let filtered = logsData.filter(l => {
            if (currentFilter === 'on') return l.s === true;
            if (currentFilter === 'off') return l.s === false;
            return true;
        });

        if (filtered.length === 0) {
             tbody.innerHTML = `<tr><td colspan="7"><div class="empty-state"><div class="body-2">Журнал порожній</div></div></td></tr>`;
             return;
        }

        let th = '';
        filtered.forEach(l => {
            let dt = (l.t || '').split(' ');
            let dateStr = dt[0] || '';
            let timeStr = dt[1] || '';
            let isLightOn = l.s;
            let badgeClass = isLightOn ? 'badge-on' : 'badge-off';
            let badgeIcon = isLightOn ? 'ph-lightbulb' : 'ph-plug';
            let badgeText = isLightOn ? '🟢 З\'явилося' : '🔴 Зникло';
            let durLabel = isLightOn ? 'Не було: ' : 'Було: ';
            let tgStatus = l.sent ? '<span style="color: var(--success);">✓ Надіслано</span>' : '<span style="color: var(--warning);">В черзі</span>';
            th += `<tr>
                <td class="mono-val" style="color: var(--text-muted);">#${l.idx + 1}</td>
                <td class="mono-val">${dateStr}</td>
                <td class="mono-val">${timeStr}</td>
                <td><span class="state-badge caption-4 ${badgeClass}"><i class="ph ${badgeIcon}"></i> ${badgeText}</span></td>
                <td class="mono-val">${durLabel}${l.d}</td>
                <td>${tgStatus}</td>
                <td style="text-align: right;">
                    <button class="btn-row-del" onclick="deleteLogEntry(${l.idx})" title="Видалити">
                        <i class="ph ph-trash"></i>
                    </button>
                </td>
            </tr>`;
        });
        tbody.innerHTML = th;
    }

    function deleteLogEntry(idx) {
        if (!confirm("Видалити цей запис із журналу?")) return;
        fetch('/api/delete?index=' + idx, { method: 'POST' }).then(r => {
            if (r.ok) { lastTotalLogs = -1; loadFullData(); updateReport(); }
        });
    }

    function filterLogs(mode) {
        currentFilter = mode;
        document.getElementById('fltAll').classList.toggle('active', mode === 'all');
        document.getElementById('fltOn').classList.toggle('active', mode === 'on');
        document.getElementById('fltOff').classList.toggle('active', mode === 'off');
        renderLogsTable();
    }

    let isReportFetching = false;
    function updateReport() {
        if (isReportFetching) return;
        let ds = document.getElementById('dateStart').value, de = document.getElementById('dateEnd').value;
        if (!ds || !de) return;
        let ts = Math.floor(new Date(ds + "T00:00:00").getTime() / 1000), te = Math.floor(new Date(de + "T23:59:59").getTime() / 1000);
       
        isReportFetching = true;
        fetch(`/api/report?start=${ts}&end=${te}`).then(r => r.json()).then(r => {
            isReportFetching = false;
            let total = r.on_sec + r.off_sec;
            let onPct = total > 0 ? ((r.on_sec / total) * 100).toFixed(1) : 0;
            let offPct = total > 0 ? ((r.off_sec / total) * 100).toFixed(1) : 0;
           
            document.getElementById('repOnText').innerText = r.on; document.getElementById('repOnPct').innerText = onPct + '%';
            document.getElementById('repOffText').innerText = r.off; document.getElementById('repOffPct').innerText = offPct + '%';
            document.getElementById('slaCenterPct').innerText = onPct + '%'; document.getElementById('kpiSla').innerText = onPct + '%';
           
            let outages = (r.events || []).filter(e => !e.state).length;
            document.getElementById('kpiOutages').innerText = outages + ' разів';

            if (window.Chart) {
                renderCharts(onPct, offPct, r.events || [], ds, de);
            }
        }).catch(() => { isReportFetching = false; });
    }

    function renderCharts(onPct, offPct, events, dsStr, deStr) {
        if (!window.Chart) return;
        Chart.defaults.font.family = "-apple-system, system-ui, sans-serif";
        if (!slaChart) {
            let el = document.getElementById('slaChart');
            if (el) {
                slaChart = new Chart(el.getContext('2d'), {
                    type: 'doughnut', data: { labels: ['Є світло', 'Немає світла'], datasets: [{ data: [onPct, offPct], backgroundColor: ['#4ADE80', '#FB7185'], borderWidth: 0, hoverOffset: 8 }] },
                    options: { responsive: true, maintainAspectRatio: false, cutout: '74%', layout: { padding: 10 }, plugins: { legend: { display: false }, tooltip: { displayColors: false, callbacks: { label: c => c.parsed + '%' } } } }
                });
            }
        } else {
            slaChart.data.datasets[0].data = [onPct, offPct];
            slaChart.update('none');
        }

        let dataOn = [], dataOff = [];
        (events || []).forEach(ev => {
            if (ev.start > 1000000000) {
                let obj = { x: [ev.start * 1000, ev.end * 1000], y: 'Мережа' };
                if (ev.state) dataOn.push(obj); else dataOff.push(obj);
            }
        });
        let minTime = new Date(dsStr + "T00:00:00").getTime(), maxTime = new Date(deStr + "T23:59:59").getTime();

        let tlEl = document.getElementById('tlChart');
        if (!tlEl) return;
        if (!tlChart) {
            tlChart = new Chart(tlEl.getContext('2d'), {
                type: 'bar', data: { datasets: [{ label: 'Є світло', data: dataOn, backgroundColor: '#4ADE80', barPercentage: 0.8, borderRadius: 4, borderSkipped: false }, { label: 'Немає світла', data: dataOff, backgroundColor: '#FB7185', barPercentage: 0.8, borderRadius: 4, borderSkipped: false }] },
                options: { indexAxis: 'y', responsive: true, maintainAspectRatio: false, scales: { x: { type: 'time', min: minTime, max: maxTime, time: { displayFormats: { hour: 'dd.MM HH:mm' } }, grid: { color: 'rgba(255,255,255,0.06)' } }, y: { stacked: true, grid: { display: false } } }, plugins: { legend: { display: false } } }
            });
            updateChartTheme();
        } else {
            tlChart.data.datasets[0].data = dataOn; tlChart.data.datasets[1].data = dataOff;
            tlChart.options.scales.x.min = minTime; tlChart.options.scales.x.max = maxTime;
            tlChart.update('none');
        }
    }

    function toggleTokenVis() { let inp = document.getElementById('inpToken'), icon = document.getElementById('eyeIcon'); if(inp.type === 'password') { inp.type = 'text'; icon.className = 'ph ph-eye-slash'; } else { inp.type = 'password'; icon.className = 'ph ph-eye'; } }
    function clearHistory() { if (confirm("Очистити історію повністю?")) { fetch('/api/clear', { method: 'POST' }).then(() => { lastTotalLogs = -1; if(tlChart){ tlChart.destroy(); tlChart = null; } loadFullData(); updateReport(); }); } }

    // === РЕЖИМ РОЗРОБНИКА ТА АВТОБЛОКУВАННЯ (2 ХВИЛИНИ) ===
    let devLockTimer = null;

    function isDevUnlocked() {
        let auth = sessionStorage.getItem('svitlo_dev_auth');
        let closedAt = parseInt(sessionStorage.getItem('svitlo_dev_closed_at') || '0');
        if (auth !== '1') return false;
        if (closedAt > 0 && (Date.now() - closedAt) > 120000) {
            lockDevPanelNow();
            return false;
        }
        return true;
    }

    function toggleDevSection() {
        let panel = document.getElementById('devPanelCard');
        let authCard = document.getElementById('devAuthCard');

        if (panel && panel.style.display !== 'none') { closeDevPanel(); return; }
        if (authCard && authCard.style.display !== 'none') { closeDevAuthCard(); return; }

        if (isDevUnlocked()) {
            sessionStorage.removeItem('svitlo_dev_closed_at');
            if (devLockTimer) { clearTimeout(devLockTimer); devLockTimer = null; }
            if (authCard) authCard.style.display = 'none';
            if (panel) { panel.style.display = 'block'; syncDevPanel(); panel.scrollIntoView({ behavior: 'smooth' }); }
        } else {
            if (panel) panel.style.display = 'none';
            if (authCard) {
                authCard.style.display = 'block';
                document.getElementById('devAuthError').style.display = 'none';
                let inp = document.getElementById('devPassInput');
                if (inp) inp.value = '';
                authCard.scrollIntoView({ behavior: 'smooth' });
                setTimeout(() => { let p = document.getElementById('devPassInput'); if (p) p.focus(); }, 250);
            }
        }
    }

    function closeDevAuthCard() {
        let a = document.getElementById('devAuthCard');
        if (a) a.style.display = 'none';
    }

    function closeDevPanel() {
        let panel = document.getElementById('devPanelCard');
        if (panel) panel.style.display = 'none';
        sessionStorage.setItem('svitlo_dev_closed_at', Date.now());
        if (devLockTimer) clearTimeout(devLockTimer);
        devLockTimer = setTimeout(lockDevPanelNow, 120000);
    }

    function lockDevPanelNow() {
        sessionStorage.removeItem('svitlo_dev_auth');
        sessionStorage.removeItem('svitlo_dev_closed_at');
        if (devLockTimer) { clearTimeout(devLockTimer); devLockTimer = null; }
        let p = document.getElementById('devPanelCard'); if (p) p.style.display = 'none';
        let a = document.getElementById('devAuthCard'); if (a) a.style.display = 'none';
    }

    function checkDevAuth() {
        let pass = document.getElementById('devPassInput').value;
        if (pass === '12366999') {
            sessionStorage.setItem('svitlo_dev_auth', '1');
            sessionStorage.removeItem('svitlo_dev_closed_at');
            if (devLockTimer) { clearTimeout(devLockTimer); devLockTimer = null; }
            let authCard = document.getElementById('devAuthCard');
            if (authCard) authCard.style.display = 'none';
            let panel = document.getElementById('devPanelCard');
            if (panel) { panel.style.display = 'block'; syncDevPanel(); panel.scrollIntoView({ behavior: 'smooth' }); }
        } else {
            document.getElementById('devAuthError').style.display = 'block';
        }
    }

    function syncDevPanel() {
        if (!window.lastDevData) return;
        let d = window.lastDevData;
        if (document.activeElement !== document.getElementById('inpDeviceId')) {
            document.getElementById('inpDeviceId').value = d.deviceId || '';
        }
        document.getElementById('devMac').innerText = d.mac || 'N/A';
        document.getElementById('devIp').innerText = d.ip || 'N/A';
        document.getElementById('devHostname').innerText = d.hostname || d.deviceId || 'SvitloCheck-01';
        document.getElementById('devSsid').innerText = d.wifiSsid || 'N/A';
        if (d.chipTemp) {
            document.getElementById('devTempBadge').innerText = d.chipTemp + ' °C';
        }
        if (d.ping !== undefined) {
            document.getElementById('devPingBadge').innerText = (d.ping > 0) ? (d.ping + ' ms') : 'Відсутній';
        }

        // Рендеринг журналу перегріву (>68°C)
        let tTbody = document.getElementById('tempLogTable');
        if (tTbody) {
            let tLogs = d.tempLogs || [];
            if (tLogs.length === 0) {
                tTbody.innerHTML = '<tr><td colspan="3" style="text-align:center; color:var(--text-muted); padding:16px;">Перевищень >68°C не зафіксовано</td></tr>';
            } else {
                let html = '';
                tLogs.forEach((item, idx) => {
                    html += `<tr><td>#${idx + 1}</td><td>${item.t}</td><td style="color:var(--danger); font-weight:600;">${item.v} °C</td></tr>`;
                });
                tTbody.innerHTML = html;
            }
        }

        // Рендеринг журналу інтернет-зв'язку
        let nTbody = document.getElementById('netLogTable');
        if (nTbody) {
            let nLogs = d.netLogs || [];
            if (nLogs.length === 0) {
                nTbody.innerHTML = '<tr><td colspan="4" style="text-align:center; color:var(--text-muted); padding:16px;">Змін статусу не зафіксовано</td></tr>';
            } else {
                let html = '';
                nLogs.forEach((item, idx) => {
                    let badge = item.c ? '<span class="state-badge badge-on" style="padding:2px 8px;">🟢 Є зв\'язок</span>' : '<span class="state-badge badge-off" style="padding:2px 8px;">🔴 Відсутній</span>';
                    let pStr = item.c ? (item.p + ' ms') : '---';
                    html += `<tr><td>#${idx + 1}</td><td>${item.t}</td><td>${badge}</td><td>${pStr}</td></tr>`;
                });
                nTbody.innerHTML = html;
            }
        }
    }

    function saveDeviceId() {
        let val = document.getElementById('inpDeviceId').value.trim();
        let fd = new FormData();
        fd.append('deviceId', val);
        fetch('/api/setDeviceId', { method: 'POST', body: fd }).then(r => r.json()).then(res => {
            let al = document.getElementById('devIdSavedAlert');
            al.style.display = 'block';
            setTimeout(() => { al.style.display = 'none'; }, 4000);
            loadFullData();
        }).catch(e => alert('Помилка збереження'));
    }

    function restartDevice() {
        if (confirm('Перезавантажити пристрій зараз? Роутер оновить ім\'я хоста в списку клієнтів.')) {
            fetch('/api/restart', { method: 'POST' }).then(() => {
                alert('Пристрій перезавантажується... Зачекайте 10-15 секунд.');
                setTimeout(() => location.reload(), 12000);
            });
        }
    }

    setTheme(localStorage.getItem('svitlo_theme') || 'slate');
    loadFullData();
    quickPoll();
    // 1. Швидкий пінг кожні 800мс: моментально оновлює статус мережі та Wi-Fi
    setInterval(quickPoll, 800);
    // 2. Графіки, аналітика та повна телеметрія оновлюються спокійно кожні 5 секунд
    setInterval(() => {
        loadFullData();
        updateReport();
    }, 5000);
</script>
</body>
</html>)rawliteral";
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "text/html", html);
}

// Ультра-легкий швидкий роут статусу (~60 байт, час обробки < 0.1 мс)
void handleApiQuick() {
  time_t now = time(NULL);
  uint32_t durSec = 0;
  if (now > 1000000000 && lastStateChangeTime > 1000000000) {
    durSec = (uint32_t)(now - lastStateChangeTime);
  } else if (stateChangeMillis > 0) {
    durSec = (uint32_t)((millis() - stateChangeMillis) / 1000);
  }
  int rssi = WiFi.RSSI();
  int sigPct = (WiFi.status() == WL_CONNECTED) ? (rssi <= -100 ? 0 : (rssi >= -50 ? 100 : 2 * (rssi + 100))) : 0;
  float chipTemp = temperatureRead();

  char buf[160];
  snprintf(buf, sizeof(buf),
    "{\"s\":%s,\"d\":%lu,\"w\":%d,\"r\":%d,\"i\":%s,\"p\":%u,\"temp\":%.1f,\"l\":%d}",
    currentState ? "true" : "false",
    (unsigned long)durSec,
    sigPct,
    rssi,
    isInternetConnected ? "true" : "false",
    lastPingMs,
    chipTemp,
    logCount
  );
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "application/json", buf);
}

void handleApiClear() {
  logCount = 0;
  saveLogs();
  server.send(200, "text/plain", "OK");
}

void handleApiDelete() {
  if (!server.hasArg("index")) {
    server.send(400, "text/plain", "Missing index");
    return;
  }
  int idx = server.arg("index").toInt();
  if (idx >= 0 && idx < logCount) {
    if (idx < logCount - 1) {
      memmove(&logs[idx], &logs[idx + 1], sizeof(LogEntry) * (logCount - idx - 1));
    }
    logCount--;
    saveLogs();
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Invalid index");
  }
}

void handleApiExportCsv() {
  String csv = "\xEF\xBB\xBF";
  csv += "ID;Дата та час переходу;Завершений стан;Тривалість (сек);Тривалість (формат);Відправлено в Telegram\r\n";
  for (int i = 0; i < logCount; i++) {
    String completedState = logs[i].state ? "Світла не було" : "Світло було";
    String sentStr = logs[i].sent ? "Так" : "Ні";
    csv += String(i + 1) + ";" +
           formatTime(logs[i].timestamp) + ";" +
           completedState + ";" +
           String(logs[i].duration) + ";" +
           formatDuration(logs[i].duration) + ";" +
           sentStr + "\r\n";
  }

  server.sendHeader("Content-Disposition", "attachment; filename=\"svitlocheck_history.csv\"");
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "text/csv; charset=utf-8", csv);
}

void handleSave() {
  if (server.hasArg("location")) {
    String loc = server.arg("location"); loc.trim();
    strlcpy(config.locationName, loc.c_str(), sizeof(config.locationName));
  }
  if (server.hasArg("token")) {
    String tok = server.arg("token"); tok.trim();
    strlcpy(config.botToken, tok.c_str(), sizeof(config.botToken));
  }
  if (server.hasArg("chats")) {
    String ch = server.arg("chats"); ch.trim();
    strlcpy(config.chatIds, ch.c_str(), sizeof(config.chatIds));
  }
  if (server.hasArg("deviceId")) {
    String dev = server.arg("deviceId"); dev.trim();
    if (dev.length() > 0) {
      strlcpy(config.deviceId, dev.c_str(), sizeof(config.deviceId));
      applyDeviceHostname();
    }
  }
  saveConfig();
  if (strlen(config.botToken) > 0) {
    if (bot) delete bot;
    bot = new UniversalTelegramBot(config.botToken, secClient);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleApiData() {
  DynamicJsonDocument doc(8192);
  doc["location"] = config.locationName;
  doc["botToken"] = config.botToken;
  doc["chatIds"] = config.chatIds;
  doc["currentState"] = currentState;
  doc["totalLogs"] = logCount;
  time_t now = time(NULL);
  if (now > 1000000000 && lastStateChangeTime > 1000000000) {
    uint32_t durSec = (uint32_t)(now - lastStateChangeTime);
    doc["durationSec"] = durSec;
    doc["currentDuration"] = formatDuration(durSec);
  } else if (stateChangeMillis > 0) {
    uint32_t durSec = (uint32_t)((millis() - stateChangeMillis) / 1000);
    doc["durationSec"] = durSec;
    doc["currentDuration"] = formatDuration(durSec);
  } else {
    doc["currentDuration"] = "Синхронізація часу...";
  }

  doc["firstLogTime"] = (logCount > 0) ? logs[0].timestamp : time(NULL);

  uint32_t totalHeap = ESP.getHeapSize(), freeHeap = ESP.getFreeHeap(), usedHeap = totalHeap - freeHeap;
  doc["ramUsage"] = String(totalHeap > 0 ? ((float)usedHeap / totalHeap) * 100.0 : 0, 1) + "% (" + String(usedHeap / 1024) + " KB / " + String(totalHeap / 1024) + " KB)";
  size_t totalBytes = LittleFS.totalBytes(), usedBytes = LittleFS.usedBytes();
  doc["fsUsage"] = String(totalBytes > 0 ? ((float)usedBytes / totalBytes) * 100.0 : 0, 1) + "% (" + String(usedBytes / 1024) + " KB / " + String(totalBytes / 1024) + " KB)";
  doc["cpuLoad"] = String(ESP.getCpuFreqMHz()) + " MHz";
  int rssi = WiFi.RSSI();
  doc["wifiSsid"] = WiFi.SSID();
  doc["wifiSignal"] = String(rssi <= -100 ? 0 : (rssi >= -50 ? 100 : 2 * (rssi + 100))) + "% (" + String(rssi) + " dBm)";
  doc["uptime"] = formatDuration((uint32_t)(millis() / 1000));
  doc["internet"] = isInternetConnected;
  doc["ping"] = lastPingMs;
  float chipTemp = temperatureRead();
  doc["chipTemp"] = String(chipTemp, 1);
  doc["deviceId"] = config.deviceId;
  doc["mac"] = WiFi.macAddress();
  doc["ip"] = WiFi.localIP().toString();
  doc["hostname"] = (strlen(config.deviceId) > 0) ? String(config.deviceId) : WiFi.getHostname();

  JsonArray arr = doc.createNestedArray("logs");
  int renderCount = 0;
  for (int i = logCount - 1; i >= 0 && renderCount < 30; i--, renderCount++) {
    JsonObject obj = arr.createNestedObject();
    obj["idx"] = i;
    obj["t"] = formatTime(logs[i].timestamp);
    obj["s"] = logs[i].state;
    obj["d"] = formatDuration(logs[i].duration);
    obj["sent"] = logs[i].sent;
  }

  // Журнали розробника
  JsonArray tArr = doc.createNestedArray("tempLogs");
  for (int i = tempLogCount - 1; i >= 0; i--) {
    JsonObject obj = tArr.createNestedObject();
    obj["t"] = formatTime(tempLogs[i].timestamp);
    obj["v"] = String(tempLogs[i].temp, 1);
  }

  JsonArray nArr = doc.createNestedArray("netLogs");
  for (int i = netLogCount - 1; i >= 0; i--) {
    JsonObject obj = nArr.createNestedObject();
    obj["t"] = formatTime(netLogs[i].timestamp);
    obj["c"] = netLogs[i].connected;
    obj["p"] = netLogs[i].pingMs;
  }

  String res;
  serializeJson(doc, res);
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "application/json", res);
}

void handleApiReport() {
  uint32_t tStart = server.hasArg("start") ? server.arg("start").toInt() : 0;
  uint32_t tEnd = server.hasArg("end") ? server.arg("end").toInt() : time(NULL);
  uint32_t timeOn = 0, timeOff = 0;
  time_t now = time(NULL);

  DynamicJsonDocument doc(4096);
  JsonArray events = doc.createNestedArray("events");

  for (int i = 0; i < logCount; i++) {
    uint32_t entryEnd = logs[i].timestamp;
    uint32_t entryStart = entryEnd - logs[i].duration;
    uint32_t overlapStart = (entryStart > tStart) ? entryStart : tStart;
    uint32_t overlapEnd = (entryEnd < tEnd) ? entryEnd : tEnd;
   
    if (overlapStart < overlapEnd) {
      if (!logs[i].state) timeOn += (overlapEnd - overlapStart);
      else timeOff += (overlapEnd - overlapStart);              
     
      if (events.size() < 30) {
        JsonObject ev = events.createNestedObject();
        ev["state"] = !logs[i].state;
        ev["start"] = overlapStart;
        ev["end"] = overlapEnd;
      }
    }
  }

  uint32_t currEnd = (now < tEnd) ? now : tEnd;
  uint32_t currOverlapStart = (lastStateChangeTime > tStart) ? lastStateChangeTime : tStart;
  if (currOverlapStart < currEnd && lastStateChangeTime > 1000000000) {
    if (currentState) timeOn += (currEnd - currOverlapStart);
    else timeOff += (currEnd - currOverlapStart);
   
    if (events.size() < 30) {
      JsonObject ev = events.createNestedObject();
      ev["state"] = currentState;
      ev["start"] = currOverlapStart;
      ev["end"] = currEnd;
    }
  }

  doc["on_sec"] = timeOn;
  doc["off_sec"] = timeOff;
  doc["on"] = formatDuration(timeOn);
  doc["off"] = formatDuration(timeOff);
  String res;
  serializeJson(doc, res);
  server.send(200, "application/json", res);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  pinMode(SENSOR_PIN, INPUT_PULLUP);

  if (!LittleFS.begin(true)) {
    Serial.println("Помилка LittleFS!");
  }

  loadConfig();
  loadLogs();

  WiFi.mode(WIFI_STA);
  applyDeviceHostname();

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setShowInfoErase(false);
  wm.setShowInfoUpdate(false);
  wm.setCustomHeadElement("<style>a[href*='github'],p:has(a[href*='github']){display:none!important;}</style>");

  String apName = (strlen(config.deviceId) > 0) ? String(config.deviceId) : "ESP32-Power-Monitor";
  wm.setHostname(apName.c_str());
  if (!wm.autoConnect(apName.c_str())) {
    ESP.restart();
  }

  applyDeviceHostname();

  secClient.setInsecure();
  if (strlen(config.botToken) > 0) {
    bot = new UniversalTelegramBot(config.botToken, secClient);
  }

  currentState = isPowerPresent();
  stateChangeMillis = millis();
  lastStateChangeTime = time(NULL);

  syncNTP();
  unsigned long startNtp = millis();
  while (time(NULL) < 1000000000 && millis() - startNtp < 20000) {
    delay(200);
    if (millis() % 2000 < 200) checkInternetAndPing();
  }

  checkInternetAndPing();
  lastPingCheckTime = millis();
  if (time(NULL) > 1000000000) {
    lastStateChangeTime = time(NULL);
  }

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/api/quick", handleApiQuick);
  server.on("/api/data", handleApiData);
  server.on("/api/report", handleApiReport);
  server.on("/api/clear", HTTP_POST, handleApiClear);
  server.on("/api/delete", HTTP_POST, handleApiDelete);
  server.on("/api/export", HTTP_GET, handleApiExportCsv);
  server.on("/api/setDeviceId", HTTP_POST, []() {
    if (server.hasArg("deviceId")) {
      String id = server.arg("deviceId");
      id.trim();
      strlcpy(config.deviceId, id.c_str(), sizeof(config.deviceId));
      saveConfig();
      if (strlen(config.deviceId) > 0) {
        applyDeviceHostname();
      }
      server.send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
      server.send(400, "text/plain", "Missing deviceId");
    }
  });
  server.on("/api/restart", HTTP_POST, []() {
    server.send(200, "application/json", "{\"status\":\"restarting\"}");
    delay(1000);
    ESP.restart();
  });
  server.on("/wifi", []() {
    String apName = (strlen(config.deviceId) > 0) ? String(config.deviceId) : "ESP32-Power-Monitor";
    String msg = "<html><body style='background:#0F172A;color:#F1F5F9;font-family:sans-serif;text-align:center;padding:40px;'>"
                 "<h2 style='color:#38BDF8;'>Режим налаштування Wi-Fi активовано!</h2>"
                 "<p>Підключіться до: <b>" + apName + "</b>, адреса: <b>http://192.168.4.1</b></p></body></html>";
    server.send(200, "text/html", msg);
    delay(1000); server.stop();
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    wm.setShowInfoErase(false);
    wm.setShowInfoUpdate(false);
    wm.setCustomHeadElement("<style>a[href*='github'],p:has(a[href*='github']){display:none!important;}</style>");
    wm.setHostname(apName.c_str());
    wm.startConfigPortal(apName.c_str());
    ESP.restart();
  });
  server.begin();
}

// ================= MAIN LOOP =================
void loop() {
  server.handleClient();

  // --- ПЕРЕВІРКА ПІНГУ ТА ІНТЕРНЕТУ КОЖНІ 5 СЕКУНД ---
  if (millis() - lastPingCheckTime >= 5000) {
    lastPingCheckTime = millis();
    checkInternetAndPing();

    // Моніторинг температури на перевищення 68°C для журналу розробника
    float currentChipTemp = temperatureRead();
    static bool wasOverheatState = false;
    static unsigned long lastOverheatLogMs = 0;
    if (currentChipTemp >= 68.0f) {
      if (!wasOverheatState || (millis() - lastOverheatLogMs > 600000)) {
        wasOverheatState = true;
        lastOverheatLogMs = millis();
        addTempLog(currentChipTemp);
      }
    } else if (currentChipTemp < 66.0f) {
      wasOverheatState = false;
    }
  }

  // --- МИТТЄВА РЕАКЦІЯ НА ДАТЧИК СВІТЛА З АНТИБРЯЗКОТОМ 300мс ---
  bool newRead = isPowerPresent();
  if (newRead != currentState) {
    if (!pendingStateChange) {
      pendingStateChange = true;
      pendingNewState = newRead;
      stateChangeMillis = millis();
    } else if (millis() - stateChangeMillis >= 300) {
      time_t now = time(NULL);
      unsigned long delaySec = (millis() - stateChangeMillis) / 1000;
      time_t actualChangeTime = (now > delaySec && now > 1000000000) ? (now - delaySec) : now;
      uint32_t duration = (lastStateChangeTime > 1000000000 && actualChangeTime > lastStateChangeTime) ?
                          (uint32_t)(actualChangeTime - lastStateChangeTime) : 0;

      currentState = pendingNewState;
      lastStateChangeTime = (now > 1000000000) ? actualChangeTime : time(NULL);

      if (now > 1000000000) {
        if (logCount >= MAX_LOGS) {
          memmove(&logs[0], &logs[1], sizeof(LogEntry) * (MAX_LOGS - 1));
          logCount = MAX_LOGS - 1;
        }
        logs[logCount].timestamp = actualChangeTime;
        logs[logCount].state = currentState;
        logs[logCount].duration = duration;
        logs[logCount].sent = false;
        logCount++;
        saveLogs();
        pendingNtpLog = false;
      } else {
        pendingNtpLog = true;
        pendingLogDuration = duration;
      }

      pendingStateChange = false;
    }
  } else {
    pendingStateChange = false;
  }

  if (pendingNtpLog && time(NULL) > 1000000000) {
    time_t now = time(NULL);
    if (logCount >= MAX_LOGS) {
      memmove(&logs[0], &logs[1], sizeof(LogEntry) * (MAX_LOGS - 1));
      logCount = MAX_LOGS - 1;
    }
    logs[logCount].timestamp = now;
    logs[logCount].state = currentState;
    logs[logCount].duration = pendingLogDuration;
    logs[logCount].sent = false;
    logCount++;
    saveLogs();
    pendingNtpLog = false;
  }

  // --- ВІДПРАВКА TELEGRAM ТА КОМАНДИ ---
  if (millis() - lastTelegramCheck > TELEGRAM_INTERVAL) {
    if (!pendingStateChange && isInternetConnected) {
      processUnsentMessages();
      handleTelegramCommands();
    }
    lastTelegramCheck = millis();
  }

  static int lastSyncDay = -1;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    if (timeinfo.tm_wday == 0 && timeinfo.tm_hour == 2 && timeinfo.tm_min == 0 && lastSyncDay != timeinfo.tm_yday) {
      if (WiFi.status() == WL_CONNECTED) { syncNTP(); lastSyncDay = timeinfo.tm_yday; delay(1000); }
    }
  }
}

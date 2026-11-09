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
String escapeHtml(const String& s);

// ================= КОРИСТУВАЦЬКІ НАЛАШТУВАННЯ =================
#define SENSOR_PIN 18            // GPIO пін датчика (якщо ESP32 підключено до USB, краще GPIO 18)
#define SENSOR_ACTIVE_LOW true  // true: LOW = світло є, HIGH = світла немає (стандарт для оптопар)
#define NTP_SERVER "pool.ntp.org"
#define DEV_PASSWORD "12366999"

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
  tempLogs[tempLogCount].timestamp = now;
  tempLogs[tempLogCount].temp = temp;
  tempLogCount++;
}

void addNetLog(bool connected, uint16_t pingMs) {
  time_t now = time(NULL);
  if (netLogCount >= MAX_NET_LOGS) {
    memmove(&netLogs[0], &netLogs[1], sizeof(NetLogEntry) * (MAX_NET_LOGS - 1));
    netLogCount = MAX_NET_LOGS - 1;
  }
  netLogs[netLogCount].timestamp = now;
  netLogs[netLogCount].connected = connected;
  netLogs[netLogCount].pingMs = pingMs;
  netLogCount++;
}

struct Config {
  char botToken[64];
  char chatIds[128];
  char locationName[64];
  char deviceId[32];
};

Config config;

void applyDeviceHostname() {
  String hName = String(config.deviceId);
  hName.trim();
  if (hName.length() == 0) hName = "SvitloCheck";

  String cleanH = "";
  for (size_t i = 0; i < hName.length(); i++) {
    char c = hName[i];
    if (isalnum(c) || c == '-') {
      cleanH += c;
    } else if (c == ' ' || c == '_' || c == '.') {
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
time_t lastStateChangeTime = 0;
unsigned long stateStartMillis = 0;

// Змінні для антибрязкоту
bool pendingStateChange = false;
bool pendingNewState = false;
unsigned long debounceMillis = 0;

WebServer server(80);
WiFiManager wm;
bool isConfigPortalRunning = false;

// Енергоефективний пінг
enum PingStep {
  PING_IDLE,
  PING_AWAITING_RETRY
};

PingStep pingStep = PING_IDLE;
unsigned long lastPingCheckTime = 0;
unsigned long pingRetryWaitStart = 0;
uint16_t lastPingMs = 0;
bool isInternetConnected = false;

WiFiClientSecure secClient;
UniversalTelegramBot* bot = NULL;

unsigned long lastTelegramCheck = 0;
const unsigned long TELEGRAM_INTERVAL = 3000;

const String kbdMarkup = "[[\"📊 Стан\", \"📅 Звіт сьогодні\"], [\"📆 Звіт за 3 дні\", \"🗓 Звіт за 7 днів\"]]";

String formatDuration(uint32_t sec);
String formatTime(time_t t);

// ================= ФУНКЦІЇ ПАМ'ЯТІ (АТОМАРНИЙ ЗАПИС) =================
void saveConfig() {
  DynamicJsonDocument doc(1024);
  doc["botToken"] = config.botToken;
  doc["chatIds"] = config.chatIds;
  doc["locationName"] = config.locationName;
  doc["deviceId"] = config.deviceId;

  File file = LittleFS.open("/config.tmp", "w");
  if (file) {
    size_t written = serializeJson(doc, file);
    file.close();
    if (written > 0) {
      LittleFS.remove(CONFIG_FILE);
      LittleFS.rename("/config.tmp", CONFIG_FILE);
    } else {
      LittleFS.remove("/config.tmp");
    }
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
  File file = LittleFS.open("/history.tmp", "w");
  if (file) {
    size_t bytesCount = sizeof(logCount);
    size_t writtenCount = file.write((uint8_t*)&logCount, bytesCount);
    size_t bytesLogs = sizeof(LogEntry) * MAX_LOGS;
    size_t writtenLogs = file.write((uint8_t*)logs, bytesLogs);
    file.close();
    if (writtenCount == bytesCount && writtenLogs == bytesLogs) {
      LittleFS.remove(LOG_FILE);
      LittleFS.rename("/history.tmp", LOG_FILE);
    } else {
      LittleFS.remove("/history.tmp");
    }
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

// ================= ЕНЕРГОЕФЕКТИВНА ПЕРЕВІРКА ПІНГУ ТА ІНТЕРНЕТУ =================
uint16_t probeHostPing(const IPAddress& ip, uint16_t port, uint32_t timeoutMs = 250) {
  WiFiClient client;
  client.setTimeout(1);
  unsigned long startT = millis();
  if (client.connect(ip, port, timeoutMs)) {
    unsigned long rtt = millis() - startT;
    client.stop();
    return (rtt > 0) ? (uint16_t)rtt : 1;
  }
  return 0;
}

void processPingStateMachine() {
  if (WiFi.status() != WL_CONNECTED) {
    if (isInternetConnected) {
      isInternetConnected = false;
      lastPingMs = 0;
      addNetLog(false, 0);
    }
    pingStep = PING_IDLE;
    lastPingCheckTime = millis();
    return;
  }

  unsigned long now = millis();

  if (pingStep == PING_IDLE) {
    // 1. Пінгуємо 1.1.1.1:80 кожні 6 секунд (таймаут 250 мс)
    if (now - lastPingCheckTime >= 6000) {
      lastPingCheckTime = now;
      uint16_t rtt = probeHostPing(IPAddress(1, 1, 1, 1), 80, 250);
      if (rtt > 0) {
        lastPingMs = rtt;
        if (!isInternetConnected) {
          isInternetConnected = true;
          addNetLog(true, lastPingMs);
        }
      } else {
        // 1.1.1.1 не відповів: запланувати повторну спробу через 1 секунду
        pingStep = PING_AWAITING_RETRY;
        pingRetryWaitStart = now;
      }
    }
  } else if (pingStep == PING_AWAITING_RETRY) {
    // 2. Через 1 секунду перевіряємо 8.8.8.8:53 (таймаут 250 мс)
    if (now - pingRetryWaitStart >= 1000) {
      uint16_t rtt = probeHostPing(IPAddress(8, 8, 8, 8), 53, 250);
      if (rtt > 0) {
        lastPingMs = rtt;
        if (!isInternetConnected) {
          isInternetConnected = true;
          addNetLog(true, lastPingMs);
        }
      } else {
        lastPingMs = 0;
        if (isInternetConnected) {
          isInternetConnected = false;
          addNetLog(false, 0);
        }
      }
      pingStep = PING_IDLE;
      lastPingCheckTime = millis();
    }
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

String escapeHtml(const String& s) {
  String res = "";
  res.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') res += "&amp;";
    else if (c == '<') res += "&lt;";
    else if (c == '>') res += "&gt;";
    else res += c;
  }
  return res;
}

bool isAuthorizedChat(const String& fromId) {
  String id = fromId;
  id.trim();
  if (id.length() == 0 || strlen(config.chatIds) == 0) return false;
  String chats = String(config.chatIds);
  int start = 0;
  int end = chats.indexOf(',');
  while (end != -1) {
    String chat = chats.substring(start, end);
    chat.trim();
    if (chat.equals(id)) return true;
    start = end + 1;
    end = chats.indexOf(',', start);
  }
  String lastChat = chats.substring(start);
  lastChat.trim();
  return lastChat.equals(id);
}

bool verifyDevAuth() {
  if (server.hasHeader("X-Dev-Pass")) {
    if (server.header("X-Dev-Pass") == DEV_PASSWORD) return true;
  }
  if (server.hasArg("devPass")) {
    if (server.arg("devPass") == DEV_PASSWORD) return true;
  }
  server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
  return false;
}

// ================= TELEGRAM =================
bool sendToAllChats(String text) {
  if (strlen(config.botToken) == 0 || bot == NULL) return false;
  bool anySent = false;
  String chats = String(config.chatIds);
  int start = 0;
  int end = chats.indexOf(',');
  while (end != -1) {
    String chat = chats.substring(start, end);
    chat.trim();
    if (chat.length() > 0) {
      if (bot->sendMessageWithReplyKeyboard(chat, text, "HTML", kbdMarkup, true)) anySent = true;
    }
    start = end + 1;
    end = chats.indexOf(',', start);
  }
  String lastChat = chats.substring(start);
  lastChat.trim();
  if (lastChat.length() > 0) {
    if (bot->sendMessageWithReplyKeyboard(lastChat, text, "HTML", kbdMarkup, true)) anySent = true;
  }
  return anySent;
}

void processUnsentMessages() {
  if (time(NULL) < 1000000000) return;
  for (int i = 0; i < logCount; i++) {
    if (!logs[i].sent) {
      String msg = logs[i].state ? "🟢 <b>СВІТЛО З'ЯВИЛОСЯ!</b>\n" : "🔴 <b>СВІТЛО ЗНИКЛО!</b>\n";
      msg += "📍 <b>Локація:</b> " + escapeHtml(config.locationName) + "\n";
      msg += "🕒 <b>Час:</b> " + formatTime(logs[i].timestamp) + "\n";
      if (logs[i].duration > 0) {
        msg += logs[i].state ? "⏳ <b>Не було:</b> " : "⏳ <b>Було:</b> ";
        msg += formatDuration(logs[i].duration) + "\n";
      }
      if (!sendToAllChats(msg)) {
        break;
      }
      logs[i].sent = true;
      saveLogs();
      delay(500);
    }
  }
}

String generateReportText(int days) {
  time_t now = time(NULL);
  if (now < 1000000000) {
    return "⚠️ Неможливо сформувати звіт: годинник пристрою ще не синхронізовано з Інтернетом.";
  }

  struct tm tmNow;
  localtime_r(&now, &tmNow);

  // Обчислення початку періоду від 00:00:00 відповідного дня
  struct tm tmStart = tmNow;
  if (days <= 1) {
    // Сьогодні з 00:00:00 по поточний момент
    tmStart.tm_hour = 0;
    tmStart.tm_min = 0;
    tmStart.tm_sec = 0;
  } else {
    // Сьогодні та попередні (days - 1) діб від 00:00:00
    tmStart.tm_mday -= (days - 1);
    tmStart.tm_hour = 0;
    tmStart.tm_min = 0;
    tmStart.tm_sec = 0;
  }
  time_t filterStart = mktime(&tmStart);
  if (filterStart == (time_t)-1 || filterStart > now) {
    int64_t secondsBack = (int64_t)days * 86400LL;
    filterStart = (now > secondsBack) ? (time_t)(now - secondsBack) : 0;
  }

  uint32_t totalTimeOn = 0, totalTimeOff = 0, outagesCount = 0;

  for (int i = 0; i < logCount; i++) {
    time_t entryEnd = logs[i].timestamp;
    time_t entryStart = (entryEnd > (time_t)logs[i].duration) ? (entryEnd - (time_t)logs[i].duration) : 0;
    time_t overlapStart = (entryStart > filterStart) ? entryStart : filterStart;
    time_t overlapEnd = (entryEnd < now) ? entryEnd : now;

    if (overlapStart < overlapEnd) {
      uint32_t dur = (uint32_t)(overlapEnd - overlapStart);
      if (!logs[i].state) {
        totalTimeOn += dur;
        outagesCount++;
      } else {
        totalTimeOff += dur;
      }
    }
  }

  if (lastStateChangeTime > 0) {
    time_t currStart = lastStateChangeTime;
    time_t currEnd = now;
    time_t overlapStart = (currStart > filterStart) ? currStart : filterStart;
    time_t overlapEnd = (currEnd < now) ? currEnd : now;
    if (overlapStart < overlapEnd) {
      uint32_t dur = (uint32_t)(overlapEnd - overlapStart);
      if (currentState) totalTimeOn += dur;
      else { totalTimeOff += dur; outagesCount++; }
    }
  }

  uint32_t totalTracked = totalTimeOn + totalTimeOff;
  float sla = totalTracked > 0 ? ((float)totalTimeOn / totalTracked) * 100.0 : 0.0;

  char timeBuf[16];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", tmNow.tm_hour, tmNow.tm_min);

  String header;
  String periodInfo;

  if (days <= 1) {
    header = "📊 <b>ЗВІТ ЗА СЬОГОДНІ</b>\n";
    periodInfo = "🕒 <b>Період:</b> з 00:00 по " + String(timeBuf) + "\n";
  } else if (days == 3) {
    header = "📊 <b>ЗВІТ ЗА 3 ДНІ</b>\n";
    char pBuf[64];
    struct tm tmRange;
    localtime_r(&filterStart, &tmRange);
    snprintf(pBuf, sizeof(pBuf), "%02d.%02d — %02d.%02d (до %s)", tmRange.tm_mday, tmRange.tm_mon + 1, tmNow.tm_mday, tmNow.tm_mon + 1, timeBuf);
    periodInfo = "🕒 <b>Період:</b> " + String(pBuf) + "\n";
  } else {
    header = "📊 <b>ЗВІТ ЗА 7 ДНІВ (ТИЖДЕНЬ)</b>\n";
    char pBuf[64];
    struct tm tmRange;
    localtime_r(&filterStart, &tmRange);
    snprintf(pBuf, sizeof(pBuf), "%02d.%02d — %02d.%02d (до %s)", tmRange.tm_mday, tmRange.tm_mon + 1, tmNow.tm_mday, tmNow.tm_mon + 1, timeBuf);
    periodInfo = "🕒 <b>Період:</b> " + String(pBuf) + "\n";
  }

  String rep = header;
  rep += "📍 <b>Локація:</b> " + escapeHtml(config.locationName) + "\n";
  rep += periodInfo + "\n";
  rep += "🟢 <b>Час зі світлом:</b> " + formatDuration(totalTimeOn) + "\n";
  rep += "🔴 <b>Час без світла:</b> " + formatDuration(totalTimeOff) + "\n";
  rep += "⚡ <b>Кількість відключень:</b> " + String(outagesCount) + "\n";
  rep += "📈 <b>Індекс SLA (наявність):</b> " + String(sla, 1) + "%\n";

  return rep;
}

void handleTelegramCommands() {
  if (strlen(config.botToken) == 0 || bot == NULL) return;
  int numNewMessages = bot->getUpdates(bot->last_message_received + 1);
  while (numNewMessages) {
    for (int i = 0; i < numNewMessages; i++) {
      String chat_id = bot->messages[i].chat_id;
      if (!isAuthorizedChat(chat_id)) {
        continue;
      }
      String text = bot->messages[i].text;
      text.trim();

      if (text == "/start" || text == "Стан" || text == "📊 Стан" || text == "/status") {
        time_t now = time(NULL);
        uint32_t durSec = 0;
        if (now > 1000000000 && lastStateChangeTime > 1000000000) {
          durSec = (uint32_t)(now - lastStateChangeTime);
        } else {
          durSec = (uint32_t)((millis() - stateStartMillis) / 1000);
        }
        String reply = currentState ? "🟢 <b>СВІТЛО Є</b>\n" : "🔴 <b>СВІТЛА НЕМАЄ</b>\n";
        reply += "📍 <b>Локація:</b> " + escapeHtml(config.locationName) + "\n";
        reply += "⏳ <b>Тривалість поточного стану:</b> " + formatDuration(durSec) + "\n";
        reply += "🕒 <b>Останнє перемикання:</b> " + formatTime(lastStateChangeTime) + "\n";
        bot->sendMessageWithReplyKeyboard(chat_id, reply, "HTML", kbdMarkup, true);
      } else if (text == "/report" || text == "/today" || text == "Звіт" || text == "📈 Звіт" || 
                 text == "Звіт сьогодні" || text == "📅 Звіт сьогодні" || text == "Звіт за сьогодні" || text == "Сьогодні") {
        bot->sendMessageWithReplyKeyboard(chat_id, generateReportText(1), "HTML", kbdMarkup, true);
      } else if (text == "/report3" || text == "Звіт за 3 дні" || text == "📆 Звіт за 3 дні" || text == "За 3 дні" || text == "3 дні") {
        bot->sendMessageWithReplyKeyboard(chat_id, generateReportText(3), "HTML", kbdMarkup, true);
      } else if (text == "/report7" || text == "/week" || text == "Звіт за 7 днів" || text == "🗓 Звіт за 7 днів" || 
                 text == "🗓️ Звіт за 7 днів" || text == "Звіт за тиждень" || text == "🗓 Звіт за тиждень" || 
                 text == "Звіт за неділю" || text == "🗓 Звіт за неділю" || text == "За 7 днів" || text == "За тиждень" || 
                 text == "За неділю" || text == "7 днів" || text == "Тиждень") {
        bot->sendMessageWithReplyKeyboard(chat_id, generateReportText(7), "HTML", kbdMarkup, true);
      }
    }
    numNewMessages = bot->getUpdates(bot->last_message_received + 1);
  }
}

// ================= HTML СТОРІНКА У PROGMEM =================
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="uk">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>SvitloCheck Pro Telemetry</title>
    <script src="/chart.js"></script>
    <script src="/adapter.js"></script>
    <script>
        if (typeof Chart === 'undefined') {
            document.write('<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.3/dist/chart.umd.min.js"><\/script>');
            document.write('<script src="https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns@3.0.0/dist/chartjs-adapter-date-fns.bundle.min.js"><\/script>');
        } else if (!Chart._adapters || !Chart._adapters._date) {
            document.write('<script src="https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns@3.0.0/dist/chartjs-adapter-date-fns.bundle.min.js"><\/script>');
        }
    </script>
    <style>
        :root {
            --bg-base: #070B14; --bg-surface: #0E1526; --bg-card: #131C31; --bg-sub: #18233C;
            --border: rgba(255, 255, 255, 0.08); --border-focus: #38BDF8;
            --text-primary: #F8FAFC; --text-secondary: #94A3B8; --text-muted: #64748B;
            --accent: #38BDF8; --accent-glow: rgba(56, 189, 248, 0.25);
            --success: #4ADE80; --success-glow: rgba(74, 222, 128, 0.25);
            --danger: #FB7185; --danger-glow: rgba(251, 113, 133, 0.25);
            --warning: #FBBF24; --warning-glow: rgba(251, 191, 36, 0.25);
        }
        [data-theme="black"] {
            --bg-base: #000000; --bg-surface: #0A0A0A; --bg-card: #121212; --bg-sub: #1C1C1C;
            --border: rgba(255, 255, 255, 0.12); --border-focus: #FFFFFF;
            --text-primary: #FFFFFF; --text-secondary: #A1A1AA; --text-muted: #71717A;
            --accent: #E4E4E7; --accent-glow: rgba(255, 255, 255, 0.15);
            --success: #22C55E; --success-glow: rgba(34, 197, 94, 0.25);
            --danger: #EF4444; --danger-glow: rgba(239, 68, 68, 0.25);
            --warning: #EAB308; --warning-glow: rgba(234, 179, 8, 0.25);
        }
        [data-theme="light"] {
            --bg-base: #F8FAFC; --bg-surface: #FFFFFF; --bg-card: #F1F5F9; --bg-sub: #E2E8F0;
            --border: rgba(0, 0, 0, 0.08); --border-focus: #0284C7;
            --text-primary: #0F172A; --text-secondary: #475569; --text-muted: #64748B;
            --accent: #0284C7; --accent-glow: rgba(2, 132, 199, 0.15);
            --success: #16A34A; --success-glow: rgba(22, 163, 74, 0.15);
            --danger: #DC2626; --danger-glow: rgba(220, 38, 38, 0.15);
            --warning: #D97706; --warning-glow: rgba(217, 119, 6, 0.15);
        }
        [data-theme="amber"] {
            --bg-base: #0C0A09; --bg-surface: #1C1917; --bg-card: #292524; --bg-sub: #44403C;
            --border: rgba(245, 158, 11, 0.15); --border-focus: #F59E0B;
            --text-primary: #FAFAF9; --text-secondary: #D6D3D1; --text-muted: #A8A29E;
            --accent: #F59E0B; --accent-glow: rgba(245, 158, 11, 0.25);
            --success: #10B981; --success-glow: rgba(16, 185, 129, 0.25);
            --danger: #F43F5E; --danger-glow: rgba(244, 63, 94, 0.25);
            --warning: #FBBF24; --warning-glow: rgba(251, 191, 36, 0.25);
        }
        [data-theme="emerald"] {
            --bg-base: #022C22; --bg-surface: #064E3B; --bg-card: #065F46; --bg-sub: #047857;
            --border: rgba(52, 211, 153, 0.2); --border-focus: #34D399;
            --text-primary: #F0FDF4; --text-secondary: #A7F3D0; --text-muted: #6EE7B7;
            --accent: #34D399; --accent-glow: rgba(52, 211, 153, 0.25);
            --success: #4ADE80; --success-glow: rgba(74, 222, 128, 0.25);
            --danger: #FB7185; --danger-glow: rgba(251, 113, 133, 0.25);
            --warning: #FCD34D; --warning-glow: rgba(252, 211, 77, 0.25);
        }
        [data-theme="violet"] {
            --bg-base: #0F0A1C; --bg-surface: #18112C; --bg-card: #22183E; --bg-sub: #2E2154;
            --border: rgba(168, 85, 247, 0.15); --border-focus: #A855F7;
            --text-primary: #FAF5FF; --text-secondary: #D8B4FE; --text-muted: #A855F7;
            --accent: #A855F7; --accent-glow: rgba(168, 85, 247, 0.25);
            --success: #34D399; --success-glow: rgba(52, 211, 153, 0.25);
            --danger: #F43F5E; --danger-glow: rgba(244, 63, 94, 0.25);
            --warning: #FBBF24; --warning-glow: rgba(251, 191, 36, 0.25);
        }

        * { box-sizing: border-box; margin: 0; padding: 0; transition: background-color 0.2s, border-color 0.2s, color 0.2s; }
        body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; background-color: var(--bg-base); color: var(--text-primary); min-height: 100vh; padding: 20px 16px 40px; position: relative; overflow-x: hidden; }

        .ph { display: inline-block; width: 1.15em; height: 1.15em; vertical-align: -0.18em; stroke: currentColor; fill: none; stroke-width: 2; stroke-linecap: round; stroke-linejoin: round; }

        .ambient-glow { position: fixed; top: 0; left: 50%; transform: translateX(-50%); width: 800px; height: 350px; border-radius: 50%; pointer-events: none; z-index: 0; opacity: 0.5; filter: blur(100px); transition: background 0.6s cubic-bezier(0.16, 1, 0.3, 1); }
        .glow-on { background: radial-gradient(circle, var(--success-glow) 0%, transparent 70%); }
        .glow-off { background: radial-gradient(circle, var(--danger-glow) 0%, transparent 70%); }

        .container { max-width: 1100px; margin: 0 auto; position: relative; z-index: 1; }

        .demo-bar { background: var(--bg-surface); border: 1px solid var(--border); border-radius: 12px; padding: 8px 14px; margin-bottom: 20px; display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 10px; }
        .demo-bar-left { display: flex; align-items: center; gap: 8px; color: var(--text-secondary); font-size: 13px; font-weight: 500; }
        .demo-controls { display: flex; gap: 6px; flex-wrap: wrap; }
        .theme-dot { width: 12px; height: 12px; border-radius: 50%; display: inline-block; border: 1px solid rgba(255,255,255,0.3); }

        .top-nav { display: flex; justify-content: space-between; align-items: center; margin-bottom: 24px; flex-wrap: wrap; gap: 14px; }
        .header-gradient { font-size: 26px; font-weight: 700; letter-spacing: -0.02em; background: linear-gradient(135deg, var(--text-primary) 0%, var(--text-secondary) 100%); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
        .loc-pill { display: inline-flex; align-items: center; gap: 6px; padding: 4px 10px; background: var(--bg-surface); border: 1px solid var(--border); border-radius: 20px; color: var(--text-secondary); font-size: 13px; margin-bottom: 6px; }
        .nav-pills { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
        .status-pill { display: inline-flex; align-items: center; gap: 8px; padding: 6px 14px; background: var(--bg-surface); border: 1px solid var(--border); border-radius: 20px; font-size: 13px; color: var(--text-secondary); }
        .dot { width: 8px; height: 8px; border-radius: 50%; display: inline-block; }
        .dot-online { background: var(--success); box-shadow: 0 0 10px var(--success); }
        .dot-offline { background: var(--danger); box-shadow: 0 0 10px var(--danger); }

        .bento-grid { display: grid; grid-template-columns: repeat(12, 1fr); gap: 16px; margin-bottom: 24px; }
        .col-12 { grid-column: span 12; } .col-6 { grid-column: span 6; } .col-3 { grid-column: span 3; }

        .card { background: var(--bg-card); border: 1px solid var(--border); border-radius: 16px; padding: 20px; position: relative; overflow: hidden; backdrop-filter: blur(12px); }
        .card-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 16px; }
        .card-title { display: flex; align-items: center; gap: 8px; font-size: 16px; font-weight: 600; color: var(--text-primary); }
        .card-subtitle { font-size: 13px; color: var(--text-muted); margin-top: 2px; }

        .hero-card { display: grid; grid-template-columns: 1fr auto; gap: 24px; align-items: center; padding: 24px; }
        .hero-on { border-color: rgba(74, 222, 128, 0.35); background: linear-gradient(135deg, var(--bg-card) 0%, rgba(74, 222, 128, 0.08) 100%); }
        .hero-off { border-color: rgba(251, 113, 133, 0.35); background: linear-gradient(135deg, var(--bg-card) 0%, rgba(251, 113, 133, 0.08) 100%); }
        .hero-status-row { display: flex; align-items: center; gap: 16px; margin-top: 10px; }
        .icon-container { width: 64px; height: 64px; border-radius: 16px; display: flex; align-items: center; justify-content: center; font-size: 32px; }
        .icon-container .ph { width: 34px; height: 34px; }
        .icon-state-on { background: rgba(74, 222, 128, 0.15); color: var(--success); }
        .icon-state-off { background: rgba(251, 113, 133, 0.15); color: var(--danger); }
        .status-text-on { font-size: 34px; font-weight: 800; letter-spacing: -0.02em; color: var(--success); line-height: 1.1; }
        .status-text-off { font-size: 34px; font-weight: 800; letter-spacing: -0.02em; color: var(--danger); line-height: 1.1; }

        .hero-timer-box { margin-top: 16px; display: inline-flex; flex-direction: column; }
        .timer-badge { display: inline-flex; align-items: baseline; gap: 8px; margin-top: 4px; }
        .timer-val { font-size: 26px; font-weight: 700; font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace; letter-spacing: -0.02em; color: var(--text-primary); }

        .hero-sla-box { display: flex; flex-direction: column; align-items: center; justify-content: center; position: relative; width: 170px; height: 170px; }
        .sla-center-label { position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); text-align: center; pointer-events: none; }

        .kpi-card { padding: 16px; display: flex; flex-direction: column; justify-content: space-between; }
        .kpi-top { display: flex; justify-content: space-between; align-items: center; color: var(--text-muted); font-size: 13px; font-weight: 500; }
        .kpi-val { font-size: 24px; font-weight: 700; margin-top: 10px; font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace; }

        .analytics-controls { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 12px; margin-bottom: 16px; }
        .flex-dates { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
        .summary-strips { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-bottom: 20px; }
        .strip-box { background: var(--bg-sub); border: 1px solid var(--border); border-radius: 12px; padding: 14px 16px; display: flex; justify-content: space-between; align-items: center; }

        .chart-timeline-box { position: relative; height: 130px; width: 100%; margin-top: 12px; }

        .res-item { margin-bottom: 14px; }
        .res-item:last-child { margin-bottom: 0; }
        .res-label { display: flex; justify-content: space-between; font-size: 13px; color: var(--text-secondary); margin-bottom: 6px; }
        .progress-track { height: 6px; background: var(--bg-sub); border-radius: 10px; overflow: hidden; position: relative; }
        .progress-fill { height: 100%; border-radius: 10px; transition: transform 0.4s ease, background 0.3s; transform-origin: left; }
        .fill-accent { background: var(--accent); }
        .fill-success { background: var(--success); }
        .fill-danger { background: var(--danger); }
        .fill-warning { background: var(--warning); }

        .form-group { margin-bottom: 14px; }
        label { display: block; font-size: 13px; color: var(--text-secondary); margin-bottom: 6px; font-weight: 500; }
        input[type="text"], input[type="password"], input[type="date"] {
            width: 100%; background: var(--bg-surface); border: 1px solid var(--border); border-radius: 10px;
            padding: 10px 12px; font-size: 14px; color: var(--text-primary); outline: none; font-family: inherit;
        }
        input:focus { border-color: var(--border-focus); box-shadow: 0 0 0 2px var(--accent-glow); }
        .pass-wrapper { position: relative; }
        .toggle-pass { position: absolute; right: 10px; top: 50%; transform: translateY(-50%); background: none; border: none; color: var(--text-muted); cursor: pointer; padding: 4px; font-size: 16px; }
        .toggle-pass:hover { color: var(--text-primary); }

        .btn-row { display: flex; gap: 10px; margin-top: 18px; }
        .btn { display: inline-flex; align-items: center; justify-content: center; gap: 8px; padding: 10px 16px; border-radius: 10px; font-size: 13px; font-weight: 600; cursor: pointer; border: 1px solid transparent; background: var(--accent); color: #070B14; }
        .btn:hover { filter: brightness(1.1); }
        .btn-secondary { background: var(--bg-sub); color: var(--text-primary); border-color: var(--border); }
        .btn-secondary:hover { background: var(--bg-surface); }
        .btn-secondary.active { background: var(--accent); color: #070B14; border-color: var(--accent); }
        .btn-danger { background: rgba(251, 113, 133, 0.15); color: var(--danger); border-color: rgba(251, 113, 133, 0.3); }
        .btn-danger:hover { background: var(--danger); color: #FFFFFF; }

        .table-wrap { overflow-x: auto; margin-top: 10px; border: 1px solid var(--border); border-radius: 12px; background: var(--bg-surface); }
        table { width: 100%; border-collapse: collapse; text-align: left; font-size: 13px; }
        th { padding: 12px 14px; background: var(--bg-sub); color: var(--text-muted); font-weight: 600; border-bottom: 1px solid var(--border); }
        td { padding: 12px 14px; border-bottom: 1px solid var(--border); color: var(--text-secondary); }
        tr:last-child td { border-bottom: none; }
        .mono-val { font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace; }
        .state-badge { display: inline-flex; align-items: center; gap: 6px; padding: 4px 10px; border-radius: 12px; font-size: 12px; font-weight: 600; }
        .badge-on { background: rgba(74, 222, 128, 0.15); color: var(--success); }
        .badge-off { background: rgba(251, 113, 133, 0.15); color: var(--danger); }
        .btn-row-del { background: none; border: none; color: var(--text-muted); cursor: pointer; padding: 4px; border-radius: 6px; font-size: 15px; }
        .btn-row-del:hover { color: var(--danger); background: rgba(251, 113, 133, 0.1); }

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
<svg xmlns="http://www.w3.org/2000/svg" style="display:none">
<symbol id="ph-palette" viewBox="0 0 24 24"><path d="M12 2C6.5 2 2 6.5 2 12c0 3.6 2 6.7 5 8.3.5.3 1.1-.1 1.1-.7v-.6c0-1.7 1.3-3 3-3h1.8c4.4 0 8.1-3.6 8.1-8 0-4.4-4-8-9-8z"/><circle cx="7.5" cy="10.5" r="1" fill="currentColor"/><circle cx="12" cy="7.5" r="1" fill="currentColor"/><circle cx="16.5" cy="10.5" r="1" fill="currentColor"/></symbol>
<symbol id="ph-map-pin" viewBox="0 0 24 24"><path d="M21 10c0 7-9 13-9 13s-9-6-9-13a9 9 0 0 1 18 0z"/><circle cx="12" cy="10" r="3"/></symbol>
<symbol id="ph-clock" viewBox="0 0 24 24"><circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/></symbol>
<symbol id="ph-lightbulb" viewBox="0 0 24 24"><path d="M9 18h6m-4 4h2M12 2a7 7 0 0 0-7 7c0 2.6 1.4 4.8 3.5 6 .6.3 1 1 1 1.7V17h5v-.3c0-.7.4-1.4 1-1.7 2.1-1.2 3.5-3.4 3.5-6a7 7 0 0 0-7-7z"/></symbol>
<symbol id="ph-plug" viewBox="0 0 24 24"><path d="M12 22v-5M9 8V2M15 8V2M18 8H6a2 2 0 0 0-2 2v2a8 8 0 0 0 16 0v-2a2 2 0 0 0-2-2z"/></symbol>
<symbol id="ph-chart-line-up" viewBox="0 0 24 24"><path d="M3 3v18h18M7 16l5-5 4 4 6-7M17 8h5v5"/></symbol>
<symbol id="ph-lightning" viewBox="0 0 24 24"><polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"/></symbol>
<symbol id="ph-thermometer-simple" viewBox="0 0 24 24"><path d="M14 14.76V3.5a2.5 2.5 0 0 0-5 0v11.26a4.5 4.5 0 1 0 5 0z"/></symbol>
<symbol id="ph-floppy-disk" viewBox="0 0 24 24"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/></symbol>
<symbol id="ph-trend-up" viewBox="0 0 24 24"><polyline points="23 6 13.5 15.5 8.5 10.5 1 18"/><polyline points="17 6 23 6 23 12"/></symbol>
<symbol id="ph-cpu" viewBox="0 0 24 24"><rect x="4" y="4" width="16" height="16" rx="2"/><rect x="9" y="9" width="6" height="6"/><path d="M9 1v3m6-3v3M9 20v3m6-3v3M20 9h3m-3 6h3M1 9h3m-3 6h3"/></symbol>
<symbol id="ph-gear" viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"/></symbol>
<symbol id="ph-eye" viewBox="0 0 24 24"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></symbol>
<symbol id="ph-eye-slash" viewBox="0 0 24 24"><path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"/><line x1="1" y1="1" x2="23" y2="23"/></symbol>
<symbol id="ph-wifi-high" viewBox="0 0 24 24"><path d="M5 12.55a11 11 0 0 1 14.08 0M1.42 9a16 16 0 0 1 21.16 0M8.53 16.11a6 6 0 0 1 6.95 0M12 20h.01"/></symbol>
<symbol id="ph-list-dashes" viewBox="0 0 24 24"><line x1="8" y1="6" x2="21" y2="6"/><line x1="8" y1="12" x2="21" y2="12"/><line x1="8" y1="18" x2="21" y2="18"/><line x1="3" y1="6" x2="3.01" y2="6"/><line x1="3" y1="12" x2="3.01" y2="12"/><line x1="3" y1="18" x2="3.01" y2="18"/></symbol>
<symbol id="ph-download-simple" viewBox="0 0 24 24"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M7 10l5 5 5-5M12 15V3"/></symbol>
<symbol id="ph-trash" viewBox="0 0 24 24"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/><line x1="10" y1="11" x2="10" y2="17"/><line x1="14" y1="11" x2="14" y2="17"/></symbol>
<symbol id="ph-terminal-window" viewBox="0 0 24 24"><rect x="2" y="3" width="20" height="18" rx="3"/><line x1="2" y1="8" x2="22" y2="8"/><polyline points="6 12 9 15 6 18"/><line x1="12" y1="18" x2="16" y2="18"/></symbol>
<symbol id="ph-x" viewBox="0 0 24 24"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></symbol>
<symbol id="ph-sign-in" viewBox="0 0 24 24"><path d="M15 3h4a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-4M10 17l5-5-5-5M15 12H3"/></symbol>
<symbol id="ph-warning-circle" viewBox="0 0 24 24"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></symbol>
<symbol id="ph-lock-key" viewBox="0 0 24 24"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/><circle cx="12" cy="16" r="1"/></symbol>
<symbol id="ph-broadcast" viewBox="0 0 24 24"><circle cx="12" cy="12" r="2"/><path d="M16.24 7.76a6 6 0 0 1 0 8.49M7.76 16.24a6 6 0 0 1 0-8.49M19.07 4.93a10 10 0 0 1 0 14.14M4.93 19.07a10 10 0 0 1 0-14.14"/></symbol>
<symbol id="ph-check-circle" viewBox="0 0 24 24"><path d="M22 11.08V12a10 10 0 1 1-5.93-9.14"/><polyline points="22 4 12 14.01 9 11.01"/></symbol>
<symbol id="ph-identification-card" viewBox="0 0 24 24"><rect x="2" y="4" width="20" height="16" rx="2"/><circle cx="8" cy="10" r="2"/><path d="M14 9h4M14 13h4M6 16c0-1.5 1.5-2 3-2s3 .5 3 2"/></symbol>
<symbol id="ph-warning" viewBox="0 0 24 24"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></symbol>
<symbol id="ph-globe" viewBox="0 0 24 24"><circle cx="12" cy="12" r="10"/><line x1="2" y1="12" x2="22" y2="12"/><path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"/></symbol>
<symbol id="ph-sliders" viewBox="0 0 24 24"><line x1="4" y1="21" x2="4" y2="14"/><line x1="4" y1="10" x2="4" y2="3"/><line x1="12" y1="21" x2="12" y2="12"/><line x1="12" y1="8" x2="12" y2="3"/><line x1="20" y1="21" x2="20" y2="16"/><line x1="20" y1="12" x2="20" y2="3"/><line x1="1" y1="14" x2="7" y2="14"/><line x1="9" y1="8" x2="15" y2="8"/><line x1="17" y1="16" x2="23" y2="16"/></symbol>
<symbol id="ph-arrows-clockwise" viewBox="0 0 24 24"><polyline points="23 4 23 10 17 10"/><polyline points="1 20 1 14 7 14"/><path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/></symbol>
<symbol id="ph-file-text" viewBox="0 0 24 24"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/><line x1="16" y1="13" x2="8" y2="13"/><line x1="16" y1="17" x2="8" y2="17"/><polyline points="10 9 9 9 8 9"/></symbol>
<symbol id="ph-shield-warning" viewBox="0 0 24 24"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></symbol>
<symbol id="ph-code" viewBox="0 0 24 24"><polyline points="16 18 22 12 16 6"/><polyline points="8 6 2 12 8 18"/></symbol>
<symbol id="ph-arrow-counter-clockwise" viewBox="0 0 24 24"><path d="M1 4v6h6"/><path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10"/></symbol>
</svg>
<div id="ambientGlow" class="ambient-glow glow-on"></div>
<div class="container">
    <div class="demo-bar">
        <div class="demo-bar-left caption-2"><svg class="ph"><use href="#ph-palette"></use></svg><span>Тема:</span></div>
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
            <div class="loc-pill caption-4"><svg class="ph"><use href="#ph-map-pin"></use></svg> Об'єкт: <span id="locName" style="color: var(--text-primary);">Завантаження...</span></div>
            <h1 class="heading-2 header-gradient">SvitloCheck Telemetry</h1>
        </div>
        <div class="nav-pills">
            <div class="status-pill body-5"><span id="netDot" class="dot dot-online"></span><span>Інтернет: <span class="mono-val" id="sysInternet">...</span></span></div>
            <div class="status-pill body-5"><svg class="ph"><use href="#ph-clock"></use></svg><span>Uptime: <span class="mono-val" id="sysUptime">...</span></span></div>
        </div>
    </header>

    <div class="bento-grid">
        <div class="card col-12 hero-card hero-on" id="cardState">
            <div>
                <div class="caption-4" style="color: var(--text-secondary); text-transform: uppercase;">Поточний стан електромережі</div>
                <div class="hero-status-row">
                    <div id="statusIcon" class="icon-container icon-state-on"><svg class="ph"><use href="#ph-lightbulb"></use></svg></div>
                    <div><div id="currState" class="heading-3 status-text-on">СВІТЛО Є</div><div class="body-5" style="color: var(--text-secondary); margin-top: 4px;" id="stateSubDesc">Живлення стабільне</div></div>
                </div>
                <div class="hero-timer-box">
                    <span class="caption-4" style="color: var(--text-muted); text-transform: uppercase;">Тривалість поточного стану</span>
                    <div class="timer-badge">
                        <span class="timer-val" id="currDuration">--:--:--</span>
                    </div>
                    <span class="caption-4 mono-val" style="color: var(--text-muted); margin-top: 6px;" id="lastChangeClock">Останнє перемикання: ...</span>
                </div>
            </div>
            <div class="hero-sla-box">
                <canvas id="slaChart"></canvas>
                <div class="sla-center-label"><span class="heading-5 mono-val" id="slaCenterPct">--%</span><span class="caption-4" style="color: var(--text-secondary); display: block;">Зі світлом</span></div>
            </div>
        </div>

        <div class="card col-3 kpi-card"><div class="kpi-top caption-2"><span>Індекс SLA</span><svg class="ph"><use href="#ph-chart-line-up"></use></svg></div><div class="kpi-val heading-5 mono-val" style="color: var(--success);" id="kpiSla">--%</div></div>
        <div class="card col-3 kpi-card"><div class="kpi-top caption-2"><span>Відключень</span><svg class="ph"><use href="#ph-lightning"></use></svg></div><div class="kpi-val heading-5 mono-val" id="kpiOutages">...</div></div>
        <div class="card col-3 kpi-card"><div class="kpi-top caption-2"><span>Температура чипа</span><svg class="ph"><use href="#ph-thermometer-simple"></use></svg></div><div class="kpi-val heading-5 mono-val" id="kpiTemp" style="color: var(--accent);">-- °C</div></div>
        <div class="card col-3 kpi-card"><div class="kpi-top caption-2"><span>Записів у Flash</span><svg class="ph"><use href="#ph-floppy-disk"></use></svg></div><div class="kpi-val heading-5 mono-val" style="color: var(--accent);" id="kpiLogs">... / 500</div></div>

        <div class="card col-12">
            <div class="analytics-controls">
                <div><h3 class="title-1 card-title"><svg class="ph"><use href="#ph-trend-up"></use></svg> Аналітика мережі</h3></div>
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
            <div class="card-header"><div><h3 class="title-1 card-title"><svg class="ph"><use href="#ph-cpu"></use></svg> Телеметрія мікроконтролера</h3></div><span class="caption-4 mono-val" id="cpuLoad">...</span></div>
            <div class="res-item"><div class="res-label body-5"><span>Температура процесора (ESP32)</span><span class="mono-val" id="tempValText">...</span></div><div class="progress-track"><div class="progress-fill fill-accent" id="tempBar" style="transform: scaleX(0);"></div></div></div>
            <div class="res-item"><div class="res-label body-5"><span>Пінг до сервера</span><span class="mono-val" id="pingValText">...</span></div><div class="progress-track"><div class="progress-fill fill-success" id="pingBar" style="transform: scaleX(0);"></div></div></div>
            <div class="res-item"><div class="res-label body-5"><span>Оперативна пам'ять (RAM)</span><span class="mono-val" id="ramUsageText">...</span></div><div class="progress-track"><div class="progress-fill fill-accent" id="ramBar" style="transform: scaleX(0);"></div></div></div>
            <div class="res-item"><div class="res-label body-5"><span>Wi-Fi сигнал (<span style="color: var(--accent);" id="wifiSsid">...</span>)</span><span class="mono-val" id="wifiSignalText">...</span></div><div class="progress-track"><div class="progress-fill fill-warning" id="wifiBar" style="transform: scaleX(0);"></div></div></div>
            
            <div style="margin-top: 18px; padding-top: 14px; border-top: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 10px;">
                <div>
                    <div class="body-5" style="font-weight: 600; color: var(--text-primary);">Пам'ять контролера</div>
                    <div class="caption-4" style="color: var(--text-muted);">Очищення історії та Telegram</div>
                </div>
                <button type="button" class="btn btn-danger caption-2" onclick="factoryResetUser()"><svg class="ph"><use href="#ph-arrow-counter-clockwise"></use></svg> Очистити пам'ять</button>
            </div>
        </div>

        <div class="card col-6">
            <div class="card-header"><div><h3 class="title-1 card-title"><svg class="ph"><use href="#ph-gear"></use></svg> Налаштування Telegram</h3></div></div>
            <form action="/save" method="POST">
                <div class="form-group"><label class="body-5">Назва об'єкта / локації:</label><input type="text" name="location" id="inpLocation" value=""></div>
                <div class="form-group"><label class="body-5">Telegram Bot Token:</label>
                    <div class="pass-wrapper">
                        <input type="password" name="token" id="inpToken" value="">
                        <button type="button" class="toggle-pass" onclick="toggleTokenVis()"><svg class="ph" id="eyeIcon"><use href="#ph-eye"></use></svg></button>
                    </div>
                </div>
                <div class="form-group"><label class="body-5">Chat IDs (через кому):</label><input type="text" name="chats" id="inpChats" value=""></div>
                <div class="btn-row">
                    <button type="submit" class="btn body-2"><svg class="ph"><use href="#ph-floppy-disk"></use></svg> Зберегти</button>
                    <button type="button" class="btn btn-secondary body-2" onclick="location.href='/wifi'"><svg class="ph"><use href="#ph-wifi-high"></use></svg> Змінити Wi-Fi</button>
                </div>
            </form>
        </div>

        <div class="card col-12">
            <div class="card-header">
                <div>
                    <h3 class="title-1 card-title"><svg class="ph"><use href="#ph-list-dashes"></use></svg> Журнал подій енергомережі</h3>
                    <p class="body-5 card-subtitle">Останні збережені події перемикання мережі (прокрутка всередині)</p>
                </div>
                <div style="display: flex; gap: 8px; flex-wrap: wrap;">
                    <button class="btn btn-secondary active caption-2" id="fltAll" onclick="filterLogs('all')">Всі події</button>
                    <button class="btn btn-secondary caption-2" id="fltOn" onclick="filterLogs('on')">З'явилось</button>
                    <button class="btn btn-secondary caption-2" id="fltOff" onclick="filterLogs('off')">Зникло</button>
                    <a href="/api/export" download="svitlocheck_history.csv" class="btn btn-secondary caption-2"><svg class="ph"><use href="#ph-download-simple"></use></svg> CSV (усі 500)</a>
                    <button class="btn btn-danger caption-2" onclick="clearHistory()"><svg class="ph"><use href="#ph-trash"></use></svg> Очистити</button>
                </div>
            </div>
            <div class="table-wrap" style="max-height: 380px; overflow-y: auto;">
                <table>
                    <thead class="caption-2" style="position: sticky; top: 0; z-index: 2; background: var(--bg-sub);"><tr><th>#</th><th>Дата</th><th>Час</th><th>Подія</th><th>Минулий стан</th><th>Telegram</th><th style="text-align: right;">Дія</th></tr></thead>
                    <tbody class="body-5" id="logTable"></tbody>
                </table>
            </div>
        </div>

        <!-- Блок авторизації розробника -->
        <div class="card col-12" id="devAuthCard" style="display: none; border: 1px dashed var(--accent);">
            <div class="card-header" style="border-bottom: none; padding-bottom: 0;">
                <div>
                    <h3 class="title-1 card-title" style="color: var(--accent); margin: 0;"><svg class="ph"><use href="#ph-terminal-window"></use></svg> Режим розробника</h3>
                    <p class="body-5 card-subtitle" style="margin-top: 4px;">Введіть пароль для доступу до сервісних параметрів</p>
                </div>
                <button type="button" class="btn btn-secondary caption-2" onclick="closeDevAuthCard()"><svg class="ph"><use href="#ph-x"></use></svg> Скасувати</button>
            </div>
            <div style="padding: 14px 20px 20px; max-width: 460px;">
                <div style="display: flex; gap: 8px;">
                    <input type="password" id="devPassInput" class="body-5" placeholder="Пароль розробника" onkeydown="if(event.key==='Enter')checkDevAuth()">
                    <button type="button" class="btn body-5" onclick="checkDevAuth()"><svg class="ph"><use href="#ph-sign-in"></use></svg> Увійти</button>
                </div>
                <div id="devAuthError" style="color: var(--danger); font-size: 13px; margin-top: 8px; display: none;"><svg class="ph"><use href="#ph-warning-circle"></use></svg> Невірний пароль!</div>
            </div>
        </div>

        <!-- Окремий блок розробника -->
        <div class="card col-12" id="devPanelCard" style="display: none; border: 1px solid var(--accent); box-shadow: 0 0 20px rgba(56, 189, 248, 0.15);">
            <div class="card-header" style="border-bottom: 1px solid var(--border);">
                <div>
                    <div style="display: flex; align-items: center; gap: 8px;">
                        <span style="display: inline-block; width: 8px; height: 8px; border-radius: 50%; background: var(--accent);"></span>
                        <h3 class="title-1 card-title" style="color: var(--accent); margin: 0;"><svg class="ph"><use href="#ph-terminal-window"></use></svg> Панель розробника (Device ID & Діагностика)</h3>
                    </div>
                    <p class="body-5 card-subtitle" style="margin-top: 4px;">Керування номером роутера та діагностичними журналами системи</p>
                </div>
                <div style="display: flex; gap: 8px; flex-wrap: wrap;">
                    <button type="button" class="btn btn-secondary caption-2" onclick="closeDevPanel()"><svg class="ph"><use href="#ph-eye-slash"></use></svg> Сховати (автоблок через 2 хв)</button>
                    <button type="button" class="btn btn-danger caption-2" onclick="lockDevPanelNow()"><svg class="ph"><use href="#ph-lock-key"></use></svg> Заблокувати зараз</button>
                </div>
            </div>

            <div style="padding: 20px 0 0 0;">
                <div class="bento-grid" style="margin-bottom: 16px;">
                    <div class="card col-6" style="background: var(--bg-surface); padding: 16px;">
                        <label class="body-5" style="display: flex; align-items: center; gap: 6px; font-weight: 600; color: var(--text-primary);">
                            <svg class="ph"><use href="#ph-broadcast"></use></svg> Унікальний номер пристрою (Hostname в роутері)
                        </label>
                        <p class="caption-4" style="color: var(--text-muted); margin-bottom: 10px;">
                            Це ім'я передається роутеру (DHCP Option 12) та задає мережевий хост (наприклад, <code>SvitloCheck-01</code>).
                        </p>
                        <div style="display: flex; gap: 8px;">
                            <input type="text" id="inpDeviceId" class="body-5" placeholder="SvitloCheck-01" style="max-width: 280px;">
                            <button type="button" class="btn body-2" onclick="saveDeviceId()"><svg class="ph"><use href="#ph-floppy-disk"></use></svg> Зберегти</button>
                        </div>
                        <div id="devIdSavedAlert" style="display: none; color: var(--success); font-size: 13px; margin-top: 8px;">
                            <svg class="ph"><use href="#ph-check-circle"></use></svg> Збережено! Перезавантажте ESP32 нижче, щоб оновити ім'я в роутері.
                        </div>
                    </div>

                    <div class="card col-6" style="background: var(--bg-surface); padding: 16px;">
                        <h4 class="title-3" style="margin: 0 0 10px 0;"><svg class="ph"><use href="#ph-identification-card"></use></svg> Статус в локальній мережі</h4>
                        <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 10px; font-size: 13px;">
                            <div><span style="color: var(--text-muted);">DHCP Hostname:</span> <b id="devHostname" class="mono-val" style="color: var(--accent);">--</b></div>
                            <div><span style="color: var(--text-muted);">IP адреса:</span> <b id="devIp" class="mono-val">--</b></div>
                            <div><span style="color: var(--text-muted);">MAC адреса:</span> <b id="devMac" class="mono-val">--</b></div>
                            <div><span style="color: var(--text-muted);">SSID роутера:</span> <b id="devSsid" class="mono-val">--</b></div>
                            <div><span style="color: var(--text-muted);">Температура CPU:</span> <b id="devTempBadge" class="mono-val" style="color: var(--warning);">--</b></div>
                            <div><span style="color: var(--text-muted);">Пінг (Cloudflare):</span> <b id="devPingBadge" class="mono-val" style="color: var(--success);">--</b></div>
                        </div>
                    </div>
                </div>

                <!-- Діагностичні журнали розробника -->
                <div class="bento-grid" style="margin-bottom: 16px;">
                    <div class="card col-6" style="background: var(--bg-surface); padding: 16px;">
                        <div class="card-header" style="margin-bottom: 10px; border-bottom: none; padding-bottom: 0;">
                            <div>
                                <h4 class="title-3" style="margin: 0; color: var(--danger);"><svg class="ph"><use href="#ph-warning"></use></svg> Журнал перегріву (&gt; 68 °C)</h4>
                                <p class="caption-4" style="color: var(--text-muted);">Фіксація пікових температур процесора</p>
                            </div>
                        </div>
                        <div class="table-wrap" style="max-height: 220px; overflow-y: auto;">
                            <table>
                                <thead class="caption-2"><tr><th>#</th><th>Дата та час</th><th>Температура</th></tr></thead>
                                <tbody class="body-5" id="tempLogTable"></tbody>
                            </table>
                        </div>
                    </div>

                    <div class="card col-6" style="background: var(--bg-surface); padding: 16px;">
                        <div class="card-header" style="margin-bottom: 10px; border-bottom: none; padding-bottom: 0;">
                            <div>
                                <h4 class="title-3" style="margin: 0; color: var(--accent);"><svg class="ph"><use href="#ph-globe"></use></svg> Журнал змін зв'язку з інтернетом</h4>
                                <p class="caption-4" style="color: var(--text-muted);">Фіксація переходів (підключено / втрачено)</p>
                            </div>
                        </div>
                        <div class="table-wrap" style="max-height: 220px; overflow-y: auto;">
                            <table>
                                <thead class="caption-2"><tr><th>#</th><th>Дата та час</th><th>Подія</th><th>Пінг</th></tr></thead>
                                <tbody class="body-5" id="netLogTable"></tbody>
                            </table>
                        </div>
                    </div>
                </div>

                <div class="card col-12" style="background: var(--bg-surface); padding: 16px;">
                    <h4 class="title-3" style="margin: 0 0 10px 0;"><svg class="ph"><use href="#ph-sliders"></use></svg> Швидкі дії</h4>
                    <div style="display: flex; gap: 10px; flex-wrap: wrap;">
                        <button type="button" class="btn btn-secondary body-5" onclick="location.href='/wifi'"><svg class="ph"><use href="#ph-wifi-high"></use></svg> Змінити Wi-Fi</button>
                        <button type="button" class="btn btn-secondary body-5" onclick="document.getElementById('inpLocation').scrollIntoView({behavior:'smooth'});"><svg class="ph"><use href="#ph-gear"></use></svg> До Telegram</button>
                        <button type="button" class="btn btn-secondary body-5" onclick="document.getElementById('fltAll').scrollIntoView({behavior:'smooth'});"><svg class="ph"><use href="#ph-list-dashes"></use></svg> До журналу</button>
                        <button type="button" class="btn btn-secondary body-5" onclick="clearHistory()"><svg class="ph"><use href="#ph-trash"></use></svg> Очистити БД</button>
                        <button type="button" class="btn btn-danger body-5" onclick="restartDevice()"><svg class="ph"><use href="#ph-arrows-clockwise"></use></svg> Перезавантажити ESP32 (DHCP renew)</button>
                    </div>
                </div>
            </div>
        </div>
    </div>

    <footer class="legal-footer">
        <div class="caption-2">© 2026 SvitloCheck Pro. Моніторинг електромережі та автономна телеметрія.</div>
        <div class="legal-links">
            <button class="legal-link" onclick="openModal('termsModal')"><svg class="ph"><use href="#ph-file-text"></use></svg> Умови та дисклеймер</button>
            <button class="legal-link" onclick="openModal('safetyModal')"><svg class="ph"><use href="#ph-shield-warning"></use></svg> Безпека при підключенні</button>
            <button class="legal-link" onclick="openModal('privacyModal')"><svg class="ph"><use href="#ph-lock-key"></use></svg> Конфіденційність</button>
            <button class="legal-link" style="color: var(--accent); font-weight: 600;" onclick="toggleDevSection()"><svg class="ph"><use href="#ph-code"></use></svg> Розробник</button>
        </div>
    </footer>
</div>

<div class="modal-overlay" id="termsModal">
    <div class="modal-box">
        <div class="modal-header"><h3 class="title-1 card-title" style="margin:0;"><svg class="ph"><use href="#ph-file-text"></use></svg> Умови використання та дисклеймер</h3><button class="modal-close" onclick="closeModal('termsModal')"><svg class="ph"><use href="#ph-x"></use></svg></button></div>
        <div class="modal-body">
            <h4>1. Призначення пристрою</h4>
            <p>SvitloCheck Pro призначений виключно для некомерційного індивідуального моніторингу наявності фазної напруги побутової електромережі.</p>
            <h4>2. Відмова від відповідальності</h4>
            <p>Розробники та виробники обладнання не несуть жодної відповідальності за прямі, непрямі або випадкові збитки внаслідок використання пристрою.</p>
        </div>
    </div>
</div>

<div class="modal-overlay" id="safetyModal">
    <div class="modal-box">
        <div class="modal-header"><h3 class="title-1 card-title" style="margin:0;"><svg class="ph"><use href="#ph-shield-warning"></use></svg> Безпека при підключенні</h3><button class="modal-close" onclick="closeModal('safetyModal')"><svg class="ph"><use href="#ph-x"></use></svg></button></div>
        <div class="modal-body">
            <h4>1. Небезпека високої напруги (220-240В)</h4>
            <p>Будь-які електромонтажні роботи повинні виконуватися суворо за умови повного знеструмлення мережі ввідним автоматом.</p>
            <h4>2. Гальванічна розв'язка</h4>
            <p>Підключення датчика до високовольтної лінії повинно здійснюватися виключно через сертифікований оптоізольований модуль.</p>
        </div>
    </div>
</div>

<div class="modal-overlay" id="privacyModal">
    <div class="modal-box">
        <div class="modal-header"><h3 class="title-1 card-title" style="margin:0;"><svg class="ph"><use href="#ph-lock-key"></use></svg> Політика конфіденційності</h3><button class="modal-close" onclick="closeModal('privacyModal')"><svg class="ph"><use href="#ph-x"></use></svg></button></div>
        <div class="modal-body">
            <h4>1. Локальне збереження</h4>
            <p>Всі журнали переходів, токени та налаштування мережі зберігаються виключно в енергонезалежній пам'яті LittleFS вашого контролера ESP32.</p>
            <h4>2. Відсутність стороннього стеження</h4>
            <p>Пристрій не надсилає телеметрію на сторонні трекери чи аналітичні сервери розробників.</p>
        </div>
    </div>
</div>

<script>
    let rawLogs = [];
    let currentFilter = 'all';
    let lastState = null;
    let liveDurationSec = 0;
    let lastTotalLogs = -1;
    let lastWifi = -1;
    let lastInternet = null;
    let slaChart = null;
    let tlChart = null;
    let isFullFetching = false;

    function openModal(id) { document.getElementById(id).classList.add('open'); }
    function closeModal(id) { document.getElementById(id).classList.remove('open'); }

    function setTheme(theme) {
        document.body.setAttribute('data-theme', theme);
        localStorage.setItem('svitlo_theme', theme);
        document.querySelectorAll('[data-theme-btn]').forEach(b => {
            b.classList.toggle('active', b.getAttribute('data-theme-btn') === theme);
        });
        updateChartTheme();
    }

    function updateChartTheme() {
        if (!tlChart) return;
        let isLight = document.body.getAttribute('data-theme') === 'light';
        let gridColor = isLight ? 'rgba(0,0,0,0.06)' : 'rgba(255,255,255,0.06)';
        let textColor = isLight ? '#64748B' : '#94A3B8';
        tlChart.options.scales.x.grid.color = gridColor;
        tlChart.options.scales.x.ticks.color = textColor;
        tlChart.update('none');
    }

    function formatDur(sec) {
        let d = Math.floor(sec / 86400);
        let h = Math.floor((sec % 86400) / 3600);
        let m = Math.floor((sec % 3600) / 60);
        let s = sec % 60;
        let r = "";
        if (d > 0) r += d + " дн. ";
        if (h > 0 || d > 0) r += h + " год. ";
        r += m + " хв. " + s + " сек.";
        return r;
    }

    setInterval(() => {
        if (liveDurationSec >= 0) {
            liveDurationSec++;
            document.getElementById('currDuration').innerText = formatDur(liveDurationSec);
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
            ic.innerHTML = '<svg class="ph"><use href="#ph-lightbulb"></use></svg>';
            gl.className = "ambient-glow glow-on";
            desc.innerText = "Живлення стабільне / Мережа в нормі";
        } else {
            st.className = "heading-3 status-text-off";
            st.innerText = "СВІТЛА НЕМАЄ";
            cd.className = "card col-12 hero-card hero-off";
            ic.className = "icon-container icon-state-off";
            ic.innerHTML = '<svg class="ph"><use href="#ph-plug"></use></svg>';
            gl.className = "ambient-glow glow-off";
            desc.innerText = "Аварійне або планове відключення лінії живлення";
        }
    }

    // === ШВИДКЕ ОПИТУВАННЯ (кожні 1500мс): МОМЕНТАЛЬНЕ ОНОВЛЕННЯ СВІТЛА, WI-FI, ТЕМПЕРАТУРИ ТА ПІНГУ ===
    let isQuickFetching = false;
    function quickPoll() {
        if (document.hidden) return;
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
            let pingStr = (q.p > 0) ? (q.p + ' ms') : 'Немає зв\'язку';
            document.getElementById('pingValText').innerText = pingStr;
            let pBar = document.getElementById('pingBar');
            if (q.p > 0) {
                pBar.style.transform = `scaleX(${Math.min(q.p / 300, 1.0)})`;
                pBar.className = (q.p < 80) ? "progress-fill fill-success" : (q.p < 200 ? "progress-fill fill-warning" : "progress-fill fill-danger");
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
                    intEl.innerText = "Немає інтернету";
                    intEl.style.color = "var(--danger)";
                    dotEl.className = "dot dot-offline";
                }
            } else if (q.i) {
                document.getElementById('sysInternet').innerText = "Є зв'язок (" + (q.p > 0 ? q.p + " ms" : "OK") + ")";
            } else {
                document.getElementById('sysInternet').innerText = "Немає інтернету";
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

    function loadFullData() {
        if (document.hidden) return;
        if (isFullFetching) return;
        isFullFetching = true;

        fetch('/api/data?t=' + Date.now()).then(r => r.json()).then(d => {
            isFullFetching = false;
            document.getElementById('locName').innerText = d.location || "Не налаштовано";
            document.getElementById('sysUptime').innerText = d.uptime;

            let intEl = document.getElementById('sysInternet'), dotEl = document.getElementById('netDot');
            lastInternet = d.internet;
            if (d.internet) {
                intEl.innerText = "Є зв'язок (" + (d.ping > 0 ? d.ping + " ms" : "OK") + ")";
                intEl.style.color = "var(--success)";
                dotEl.className = "dot dot-online";
            } else {
                intEl.innerText = "Немає інтернету";
                intEl.style.color = "var(--danger)";
                dotEl.className = "dot dot-offline";
            }

            document.getElementById('pingValText').innerText = (d.ping > 0) ? (d.ping + ' ms') : 'Немає зв\'язку';
            let pBar = document.getElementById('pingBar');
            if (d.ping > 0) {
                pBar.style.transform = `scaleX(${Math.min(d.ping / 300, 1.0)})`;
                pBar.className = (d.ping < 80) ? "progress-fill fill-success" : (d.ping < 200 ? "progress-fill fill-warning" : "progress-fill fill-danger");
            } else {
                pBar.style.transform = "scaleX(0)";
            }

            if (lastState !== d.currentState) {
                lastState = d.currentState;
                updateStateUI(d.currentState);
            }

            liveDurationSec = d.durationSec;
            document.getElementById('currDuration').innerText = d.currentDuration;

            if (document.activeElement !== document.getElementById('inpLocation')) {
                document.getElementById('inpLocation').value = d.location || '';
            }
            if (document.activeElement !== document.getElementById('inpToken')) {
                document.getElementById('inpToken').value = d.botToken || '';
            }
            if (document.activeElement !== document.getElementById('inpChats')) {
                document.getElementById('inpChats').value = d.chatIds || '';
            }

            document.getElementById('cpuLoad').innerText = d.cpuLoad;
            document.getElementById('ramUsageText').innerText = d.ramUsage;
            let ramPct = parseFloat(d.ramUsage) || 0;
            document.getElementById('ramBar').style.transform = `scaleX(${ramPct / 100})`;

            document.getElementById('wifiSsid').innerText = d.wifiSsid || 'N/A';
            document.getElementById('wifiSignalText').innerText = d.wifiSignal;
            let sigPct = parseInt(d.wifiSignal) || 0;
            document.getElementById('wifiBar').style.transform = `scaleX(${sigPct / 100})`;

            if (d.chipTemp) {
                let tempVal = parseFloat(d.chipTemp);
                let tempStr = tempVal.toFixed(1) + ' °C';
                document.getElementById('kpiTemp').innerText = tempStr;
                document.getElementById('tempValText').innerText = tempStr;
                let tBar = document.getElementById('tempBar');
                tBar.style.transform = `scaleX(${Math.min(tempVal / 100, 1.0)})`;
                if (tempVal >= 68) {
                    tBar.className = "progress-fill fill-danger";
                    document.getElementById('kpiTemp').style.color = "var(--danger)";
                } else if (tempVal >= 55) {
                    tBar.className = "progress-fill fill-warning";
                    document.getElementById('kpiTemp').style.color = "var(--warning)";
                } else {
                    tBar.className = "progress-fill fill-accent";
                    document.getElementById('kpiTemp').style.color = "var(--accent)";
                }
            }

            lastTotalLogs = d.totalLogs;
            document.getElementById('kpiLogs').innerText = `${d.totalLogs} / 500`;

            rawLogs = d.logs || [];
            if (rawLogs.length > 0) {
                document.getElementById('lastChangeClock').innerText = "Останнє перемикання: " + rawLogs[0].t;
            } else {
                document.getElementById('lastChangeClock').innerText = "Останнє перемикання: Синхронізація...";
            }
            renderLogsTable();

            let ds = document.getElementById('dateStart'), de = document.getElementById('dateEnd');
            let now = new Date();
            let todayStr = now.toISOString().split('T')[0];
            if (!de.value) de.value = todayStr;
            if (!ds.value) {
                let past = new Date(now.getTime() - (7 * 24 * 3600 * 1000));
                ds.value = past.toISOString().split('T')[0];
            }

            window.lastDevData = d;
            syncDevPanel();
            updateReport();
        }).catch(() => { isFullFetching = false; });
    }

    function renderLogsTable() {
        let tbody = document.getElementById('logTable');
        let filtered = rawLogs.filter(l => {
            if (currentFilter === 'on') return l.s;
            if (currentFilter === 'off') return !l.s;
            return true;
        });

        if (filtered.length === 0) {
            tbody.innerHTML = '<tr><td colspan="7" class="empty-state">Подій у журналі поки що немає</td></tr>';
            return;
        }

        let th = '';
        filtered.forEach(l => {
            let isLightOn = l.s;
            let parts = (l.t || "").split(' ');
            let dateStr = parts[0] || '---', timeStr = parts[1] || '---';
            let badgeClass = isLightOn ? 'badge-on' : 'badge-off';
            let badgeIcon = isLightOn ? 'ph-lightbulb' : 'ph-plug';
            let badgeText = isLightOn ? '🟢 З\'явилося' : '🔴 Зникло';
            let durLabel = isLightOn ? 'Не було: ' : 'Було: ';
            let tgStatus = l.sent ? '<span style="color: var(--success);">✓ Надіслано</span>' : '<span style="color: var(--warning);">В черзі</span>';
            th += `<tr>
                <td class="mono-val" style="color: var(--text-muted);">#${l.idx + 1}</td>
                <td class="mono-val">${dateStr}</td>
                <td class="mono-val">${timeStr}</td>
                <td><span class="state-badge caption-4 ${badgeClass}"><svg class="ph"><use href="#${badgeIcon}"></use></svg> ${badgeText}</span></td>
                <td class="mono-val">${durLabel}${l.d}</td>
                <td>${tgStatus}</td>
                <td style="text-align: right;">
                    <button class="btn-row-del" onclick="deleteLogEntry(${l.idx})" title="Видалити">
                        <svg class="ph"><use href="#ph-trash"></use></svg>
                    </button>
                </td>
            </tr>`;
        });
        tbody.innerHTML = th;
    }

    function deleteLogEntry(idx) {
        if (!confirm("Видалити цей запис із журналу?")) return;
        let fd = new FormData();
        fd.append('index', idx);
        fd.append('devPass', '12366999');
        fetch('/api/delete?index=' + idx, { method: 'POST', body: fd, headers: { 'X-Dev-Pass': '12366999' } }).then(r => {
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
            
            let slaCenterEl = document.getElementById('slaCenterPct');
            if (slaCenterEl) slaCenterEl.innerText = onPct + '%';
            document.getElementById('kpiSla').innerText = onPct + '%';
           
            let outages = (r.events || []).filter(e => !e.state).length;
            document.getElementById('kpiOutages').innerText = outages + ' разів';

            if (window.Chart) {
                renderCharts(onPct, offPct, r.events || [], ds, de);
            } else {
                setTimeout(() => {
                    if (window.Chart) renderCharts(onPct, offPct, r.events || [], ds, de);
                }, 400);
            }
        }).catch(() => { isReportFetching = false; });
    }

    function renderCharts(onPct, offPct, events, dsStr, deStr) {
        if (!window.Chart) return;
        Chart.defaults.font.family = "-apple-system, system-ui, sans-serif";

        let slaEl = document.getElementById('slaChart');
        if (slaEl) {
            try {
                let pOn = parseFloat(onPct) || 0;
                let pOff = parseFloat(offPct) || 0;
                if (!slaChart) {
                    slaChart = new Chart(slaEl.getContext('2d'), {
                        type: 'doughnut',
                        data: {
                            labels: ['Є світло', 'Немає світла'],
                            datasets: [{
                                data: [pOn, pOff],
                                backgroundColor: ['#4ADE80', '#FB7185'],
                                borderWidth: 0,
                                hoverOffset: 6
                            }]
                        },
                        options: {
                            responsive: true,
                            maintainAspectRatio: false,
                            cutout: '74%',
                            layout: { padding: 8 },
                            plugins: {
                                legend: { display: false },
                                tooltip: { displayColors: false, callbacks: { label: c => c.parsed + '%' } }
                            }
                        }
                    });
                } else {
                    slaChart.data.datasets[0].data = [pOn, pOff];
                    slaChart.update('none');
                }
            } catch (err) {
                console.error("Помилка slaChart:", err);
            }
        }

        let tlEl = document.getElementById('tlChart');
        if (tlEl) {
            try {
                let dataOn = [], dataOff = [];
                (events || []).forEach(ev => {
                    if (ev.start > 1000000000) {
                        let obj = { x: [ev.start * 1000, ev.end * 1000], y: 'Мережа' };
                        if (ev.state) dataOn.push(obj); else dataOff.push(obj);
                    }
                });
                let minTime = new Date(dsStr + "T00:00:00").getTime();
                let maxTime = new Date(deStr + "T23:59:59").getTime();
                if (isNaN(minTime)) minTime = Date.now() - 7 * 86400000;
                if (isNaN(maxTime)) maxTime = Date.now();

                let hasDateAdapter = !!(window.Chart._adapters && window.Chart._adapters._date);

                let xScaleConfig = hasDateAdapter ? {
                    type: 'time',
                    min: minTime,
                    max: maxTime,
                    time: { displayFormats: { hour: 'dd.MM HH:mm' } },
                    grid: { color: 'rgba(255,255,255,0.06)' }
                } : {
                    type: 'linear',
                    min: minTime,
                    max: maxTime,
                    ticks: {
                        callback: function(val) {
                            let d = new Date(val);
                            let dd = String(d.getDate()).padStart(2, '0');
                            let mm = String(d.getMonth() + 1).padStart(2, '0');
                            let hh = String(d.getHours()).padStart(2, '0');
                            let mi = String(d.getMinutes()).padStart(2, '0');
                            return `${dd}.${mm} ${hh}:${mi}`;
                        }
                    },
                    grid: { color: 'rgba(255,255,255,0.06)' }
                };

                if (!tlChart) {
                    tlChart = new Chart(tlEl.getContext('2d'), {
                        type: 'bar',
                        data: {
                            datasets: [
                                { label: 'Є світло', data: dataOn, backgroundColor: '#4ADE80', barPercentage: 0.8, borderRadius: 4, borderSkipped: false },
                                { label: 'Немає світла', data: dataOff, backgroundColor: '#FB7185', barPercentage: 0.8, borderRadius: 4, borderSkipped: false }
                            ]
                        },
                        options: {
                            indexAxis: 'y',
                            responsive: true,
                            maintainAspectRatio: false,
                            scales: {
                                x: xScaleConfig,
                                y: { stacked: true, grid: { display: false } }
                            },
                            plugins: { legend: { display: false } }
                        }
                    });
                    updateChartTheme();
                } else {
                    tlChart.data.datasets[0].data = dataOn;
                    tlChart.data.datasets[1].data = dataOff;
                    tlChart.options.scales.x.min = minTime;
                    tlChart.options.scales.x.max = maxTime;
                    tlChart.update('none');
                }
            } catch (err) {
                console.error("Помилка tlChart:", err);
            }
        }
    }

    function toggleTokenVis() {
        let inp = document.getElementById('inpToken');
        let icon = document.getElementById('eyeIcon');
        if (inp.type === 'password') {
            inp.type = 'text';
            icon.innerHTML = '<use href="#ph-eye-slash"></use>';
        } else {
            inp.type = 'password';
            icon.innerHTML = '<use href="#ph-eye"></use>';
        }
    }

    function clearHistory() {
        if (confirm("Очистити історію повністю?")) {
            let fd = new FormData();
            fd.append('devPass', '12366999');
            fetch('/api/clear', { method: 'POST', body: fd, headers: { 'X-Dev-Pass': '12366999' } }).then(() => {
                lastTotalLogs = -1;
                if (slaChart) { slaChart.data.datasets[0].data = [0, 0]; slaChart.update('none'); }
                if (tlChart) { tlChart.destroy(); tlChart = null; }
                loadFullData();
                updateReport();
            });
        }
    }

    function factoryResetUser() {
        if (confirm("УВАГА! Ви дійсно бажаєте повністю очистити пам'ять пристрою?\n\nБуде видалено:\n- Весь журнал подій енергомережі\n- Налаштування Telegram (токен бота та ID чатів)\n- Назву локації\n\nПараметри розробника (Device ID, Wi-Fi та діагностика) залишаться.")) {
            fetch('/api/resetUser', { method: 'POST' }).then(r => r.json()).then(res => {
                alert("Пам'ять успішно очищено! Сторінка оновиться.");
                location.reload();
            }).catch(e => alert("Помилка очищення пам'яті"));
        }
    }

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
            document.getElementById('devPingBadge').innerText = (d.ping > 0) ? (d.ping + ' ms') : 'Немає зв\'язку';
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
                    let badge = item.c ? '<span class="state-badge badge-on" style="padding:2px 8px;">🟢 Є зв\'язок</span>' : '<span class="state-badge badge-off" style="padding:2px 8px;">🔴 Немає інтернету</span>';
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
        fd.append('devPass', '12366999');
        fetch('/api/setDeviceId', { method: 'POST', body: fd, headers: { 'X-Dev-Pass': '12366999' } }).then(r => r.json()).then(res => {
            let al = document.getElementById('devIdSavedAlert');
            al.style.display = 'block';
            setTimeout(() => { al.style.display = 'none'; }, 4000);
            loadFullData();
        }).catch(e => alert('Помилка збереження'));
    }

    function restartDevice() {
        if (confirm('Перезавантажити пристрій зараз? Роутер оновить ім\'я хоста в списку клієнтів.')) {
            let fd = new FormData();
            fd.append('devPass', '12366999');
            fetch('/api/restart', { method: 'POST', body: fd, headers: { 'X-Dev-Pass': '12366999' } }).then(() => {
                alert('Пристрій перезавантажується... Зачекайте 10-15 секунд.');
                setTimeout(() => location.reload(), 12000);
            });
        }
    }

    setTheme(localStorage.getItem('svitlo_theme') || 'slate');
    let initNow = new Date();
    let initToday = initNow.toISOString().split('T')[0];
    let initPast = new Date(initNow.getTime() - (7 * 24 * 3600 * 1000)).toISOString().split('T')[0];
    let elDs = document.getElementById('dateStart');
    let elDe = document.getElementById('dateEnd');
    if (elDs && !elDs.value) elDs.value = initPast;
    if (elDe && !elDe.value) elDe.value = initToday;

    loadFullData();
    quickPoll();
    updateReport();
    // 1. Швидкий пінг кожні 1500мс: моментально оновлює статус мережі та Wi-Fi
    setInterval(quickPoll, 1500);
    // 2. Графіки, аналітика та повна телеметрія оновлюються спокійно кожні 5 секунд
    setInterval(() => {
        loadFullData();
        updateReport();
    }, 5000);
</script>
</body>
</html>)rawliteral";

// ================= WEB HANDLERS =================
void handleApiQuick() {
  time_t now = time(NULL);
  uint32_t durSec = 0;
  if (now > 1000000000 && lastStateChangeTime > 1000000000) {
    durSec = (uint32_t)(now - lastStateChangeTime);
  } else {
    durSec = (uint32_t)((millis() - stateStartMillis) / 1000);
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
  } else {
    uint32_t durSec = (uint32_t)((millis() - stateStartMillis) / 1000);
    doc["durationSec"] = durSec;
    doc["currentDuration"] = formatDuration(durSec);
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
  uint32_t tStart = server.hasArg("start") ? (uint32_t)server.arg("start").toInt() : 0;
  uint32_t tEnd = server.hasArg("end") ? (uint32_t)server.arg("end").toInt() : (uint32_t)time(NULL);
  uint32_t timeOn = 0, timeOff = 0;
  time_t now = time(NULL);

  DynamicJsonDocument doc(8192);
  JsonArray events = doc.createNestedArray("events");

  int matchIndices[MAX_LOGS];
  int matchCount = 0;

  for (int i = 0; i < logCount; i++) {
    time_t entryEnd = logs[i].timestamp;
    time_t entryStart = (entryEnd > (time_t)logs[i].duration) ? (entryEnd - (time_t)logs[i].duration) : 0;
    uint32_t overlapStart = (entryStart > (time_t)tStart) ? (uint32_t)entryStart : tStart;
    uint32_t overlapEnd = (entryEnd < (time_t)tEnd) ? (uint32_t)entryEnd : tEnd;

    if (overlapStart < overlapEnd) {
      if (!logs[i].state) timeOn += (overlapEnd - overlapStart);
      else timeOff += (overlapEnd - overlapStart);
      matchIndices[matchCount++] = i;
    }
  }

  // Беремо до 60 останніх (найновіших) подій періоду і додаємо в хронологічному порядку
  int takeStart = (matchCount > 60) ? (matchCount - 60) : 0;
  for (int m = takeStart; m < matchCount; m++) {
    int i = matchIndices[m];
    time_t entryEnd = logs[i].timestamp;
    time_t entryStart = (entryEnd > (time_t)logs[i].duration) ? (entryEnd - (time_t)logs[i].duration) : 0;
    uint32_t overlapStart = (entryStart > (time_t)tStart) ? (uint32_t)entryStart : tStart;
    uint32_t overlapEnd = (entryEnd < (time_t)tEnd) ? (uint32_t)entryEnd : tEnd;
    JsonObject ev = events.createNestedObject();
    ev["state"] = !logs[i].state;
    ev["start"] = overlapStart;
    ev["end"] = overlapEnd;
  }

  // Обробка поточного стану
  if (lastStateChangeTime > 0) {
    time_t currStart = lastStateChangeTime;
    time_t currEnd = (now > 1000000000) ? now : (currStart + (millis() - stateStartMillis) / 1000);
    uint32_t currOverlapStart = (currStart > (time_t)tStart) ? (uint32_t)currStart : tStart;
    uint32_t currOverlapEnd = (currEnd < (time_t)tEnd) ? (uint32_t)currEnd : tEnd;

    if (currOverlapStart < currOverlapEnd) {
      if (currentState) timeOn += (currOverlapEnd - currOverlapStart);
      else timeOff += (currOverlapEnd - currOverlapStart);

      JsonObject ev = events.createNestedObject();
      ev["state"] = currentState;
      ev["start"] = currOverlapStart;
      ev["end"] = currOverlapEnd;
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

void handleApiClear() {
  if (!verifyDevAuth()) return;
  logCount = 0;
  saveLogs();
  server.send(200, "text/plain", "OK");
}

void handleApiDelete() {
  if (!verifyDevAuth()) return;
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
  server.sendHeader("Content-Disposition", "attachment; filename=\"svitlocheck_history.csv\"");
  server.sendHeader("Cache-Control", "no-cache");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv; charset=utf-8", "");

  String header = "\xEF\xBB\xBF";
  header += "ID;Дата та час переходу;Завершений стан;Тривалість (сек);Тривалість (формат);Відправлено в Telegram\r\n";
  server.sendContent(header);

  String chunk = "";
  chunk.reserve(1024);
  for (int i = 0; i < logCount; i++) {
    String completedState = logs[i].state ? "Світла не було" : "Світло було";
    String sentStr = logs[i].sent ? "Так" : "Ні";
    chunk += String(i + 1) + ";" +
             formatTime(logs[i].timestamp) + ";" +
             completedState + ";" +
             String(logs[i].duration) + ";" +
             formatDuration(logs[i].duration) + ";" +
             sentStr + "\r\n";

    if ((i + 1) % 25 == 0 || i == logCount - 1) {
      server.sendContent(chunk);
      chunk = "";
    }
  }
  server.sendContent("");
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
    bot->waitForResponse = 1500;
  } else {
    if (bot) {
      delete bot;
      bot = NULL;
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
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

  wm.setConfigPortalTimeout(180);
  wm.setShowInfoErase(false);
  wm.setShowInfoUpdate(false);
  wm.setCustomHeadElement("<style>a[href*='github'],p:has(a[href*='github']){display:none!important;}</style>");

  String apName = (strlen(config.deviceId) > 0) ? String(config.deviceId) : "ESP32-Power-Monitor";
  wm.setHostname(apName.c_str());
  wm.setConfigPortalBlocking(false);
  WiFi.setAutoReconnect(true);

  // Автономний старт: не блокуємось і не перезавантажуємось при відсутності Wi-Fi!
  wm.autoConnect(apName.c_str());

  applyDeviceHostname();

  secClient.setInsecure();
  secClient.setTimeout(1500);
  if (strlen(config.botToken) > 0) {
    bot = new UniversalTelegramBot(config.botToken, secClient);
    bot->waitForResponse = 1500;
  }

  currentState = isPowerPresent();
  stateStartMillis = millis();
  lastStateChangeTime = 0;

  if (WiFi.status() == WL_CONNECTED) {
    syncNTP();
    unsigned long startNtp = millis();
    while (time(NULL) < 1000000000 && millis() - startNtp < 10000) {
      delay(200);
    }
  }

  if (time(NULL) > 1000000000) {
    lastStateChangeTime = time(NULL) - ((millis() - stateStartMillis) / 1000);
  }

  uint16_t initPing = probeHostPing(IPAddress(1, 1, 1, 1), 80, 300);
  if (initPing == 0) initPing = probeHostPing(IPAddress(8, 8, 8, 8), 53, 300);
  if (initPing > 0) {
    isInternetConnected = true;
    lastPingMs = initPing;
    addNetLog(true, initPing);
  } else {
    isInternetConnected = false;
    lastPingMs = 0;
  }
  lastPingCheckTime = millis();

  // Статичні стиснуті файли графіків із LittleFS
  server.on("/chart.js", HTTP_GET, []() {
    if (LittleFS.exists("/chart.js.gz")) {
      File f = LittleFS.open("/chart.js.gz", "r");
      server.sendHeader("Cache-Control", "max-age=31536000");
      server.sendHeader("Content-Encoding", "gzip");
      server.streamFile(f, "application/javascript");
      f.close();
    } else if (LittleFS.exists("/chart.js")) {
      File f = LittleFS.open("/chart.js", "r");
      server.sendHeader("Cache-Control", "max-age=31536000");
      server.streamFile(f, "application/javascript");
      f.close();
    } else {
      server.send(404, "text/plain", "Not Found");
    }
  });

  server.on("/adapter.js", HTTP_GET, []() {
    if (LittleFS.exists("/adapter.js.gz")) {
      File f = LittleFS.open("/adapter.js.gz", "r");
      server.sendHeader("Cache-Control", "max-age=31536000");
      server.sendHeader("Content-Encoding", "gzip");
      server.streamFile(f, "application/javascript");
      f.close();
    } else if (LittleFS.exists("/adapter.js")) {
      File f = LittleFS.open("/adapter.js", "r");
      server.sendHeader("Cache-Control", "max-age=31536000");
      server.streamFile(f, "application/javascript");
      f.close();
    } else {
      server.send(404, "text/plain", "Not Found");
    }
  });

  const char* headerkeys[] = {"X-Dev-Pass"};
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
  server.collectHeaders(headerkeys, headerkeyssize);

  server.on("/", []() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/save", HTTP_POST, handleSave);
  server.on("/api/quick", handleApiQuick);
  server.on("/api/data", handleApiData);
  server.on("/api/report", handleApiReport);
  server.on("/api/clear", HTTP_POST, handleApiClear);
  server.on("/api/delete", HTTP_POST, handleApiDelete);
  server.on("/api/export", HTTP_GET, handleApiExportCsv);

  // Скидання користувацької пам'яті (журнали та Telegram), зберігаючи налаштування розробника
  server.on("/api/resetUser", HTTP_POST, []() {
    logCount = 0;
    LittleFS.remove(LOG_FILE);
    LittleFS.remove("/history.tmp");

    char savedDeviceId[32];
    strlcpy(savedDeviceId, config.deviceId, sizeof(savedDeviceId));

    memset(&config, 0, sizeof(config));
    strlcpy(config.deviceId, savedDeviceId, sizeof(config.deviceId));
    strlcpy(config.locationName, "Моя адреса / Об'єкт", sizeof(config.locationName));
    saveConfig();

    if (bot) {
      delete bot;
      bot = NULL;
    }

    server.send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/api/setDeviceId", HTTP_POST, []() {
    if (!verifyDevAuth()) return;
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
    if (!verifyDevAuth()) return;
    server.send(200, "application/json", "{\"status\":\"restarting\"}");
    delay(1000);
    ESP.restart();
  });

  server.on("/wifi", []() {
    String apName = (strlen(config.deviceId) > 0) ? String(config.deviceId) : "ESP32-Power-Monitor";
    String escApName = escapeHtml(apName);
    String msg = "<html><body style='background:#0F172A;color:#F1F5F9;font-family:sans-serif;text-align:center;padding:40px;'>"
                 "<h2 style='color:#38BDF8;'>Точка доступу Wi-Fi активована!</h2>"
                 "<p>Підключіться до: <b>" + escApName + "</b>, адреса: <b>http://192.168.4.1</b></p></body></html>";
    server.send(200, "text/html", msg);
    if (!isConfigPortalRunning) {
      wm.setConfigPortalBlocking(false);
      wm.startConfigPortal(apName.c_str());
      isConfigPortalRunning = true;
    }
  });

  server.begin();
}

// ================= MAIN LOOP =================
void loop() {
  server.handleClient();

  if (isConfigPortalRunning) {
    wm.process();
    if (!wm.getConfigPortalActive()) {
      isConfigPortalRunning = false;
    }
  }

  // --- ЕНЕРГОЕФЕКТИВНА ПЕРЕВІРКА ПІНГУ ТА ІНТЕРНЕТУ ---
  processPingStateMachine();

  // --- МОНІТОРИНГ ТЕМПЕРАТУРИ ПРОЦЕСОРА (ПЕРЕГРІВ > 68°C) КОЖНІ 5 СЕКУНД ---
  static unsigned long lastTempCheckMs = 0;
  if (millis() - lastTempCheckMs >= 5000) {
    lastTempCheckMs = millis();
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
      debounceMillis = millis();
    } else if (millis() - debounceMillis >= 300) {
      time_t now = time(NULL);
      time_t actualChangeTime = 0;
      uint32_t duration = 0;

      if (now > 1000000000) {
        unsigned long delaySec = (millis() - debounceMillis) / 1000;
        actualChangeTime = (now > delaySec) ? (now - delaySec) : now;
        duration = (lastStateChangeTime > 1000000000 && actualChangeTime > lastStateChangeTime) ?
                   (uint32_t)(actualChangeTime - lastStateChangeTime) : (uint32_t)((millis() - stateStartMillis) / 1000);
        lastStateChangeTime = actualChangeTime;
      } else {
        actualChangeTime = (time_t)(debounceMillis / 1000);
        duration = (uint32_t)((debounceMillis - stateStartMillis) / 1000);
        lastStateChangeTime = actualChangeTime;
      }

      currentState = pendingNewState;
      stateStartMillis = millis();

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

      pendingStateChange = false;
    }
  } else {
    pendingStateChange = false;
  }

  // --- КОРЕКЦІЯ ЛОГІВ ПІСЛЯ СИНХРОНІЗАЦІЇ ЧАСУ NTP ---
  static bool ntpSynced = false;
  time_t currentEpoch = time(NULL);
  if (!ntpSynced && currentEpoch > 1000000000) {
    ntpSynced = true;
    time_t bootEpoch = currentEpoch - (time_t)(millis() / 1000);
    bool needSave = false;
    for (int i = 0; i < logCount; i++) {
      if (logs[i].timestamp < 1000000000) {
        logs[i].timestamp += bootEpoch;
        needSave = true;
      }
    }
    if (lastStateChangeTime < 1000000000) {
      lastStateChangeTime += bootEpoch;
    }
    if (needSave) {
      saveLogs();
    }
  }

  // --- ВІДПРАВКА TELEGRAM ТА КОМАНДИ ---
  if (millis() - lastTelegramCheck > TELEGRAM_INTERVAL) {
    if (!pendingStateChange && isInternetConnected) {
      processUnsentMessages();
      handleTelegramCommands();
    }
    lastTelegramCheck = millis();
  }

  // --- НЕБЛОКУЮЧА ПЕРЕВІРКА ГОДИННИКА ТА ЩОТИЖНЕВА СИНХРОНІЗАЦІЯ NTP ---
  static unsigned long lastNtpCheckMs = 0;
  static int lastSyncDay = -1;
  if (millis() - lastNtpCheckMs >= 30000) {
    lastNtpCheckMs = millis();
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
      if (timeinfo.tm_wday == 0 && timeinfo.tm_hour == 2 && timeinfo.tm_min == 0 && lastSyncDay != timeinfo.tm_yday) {
        if (WiFi.status() == WL_CONNECTED) {
          syncNTP();
          lastSyncDay = timeinfo.tm_yday;
        }
      }
    }
  }
}

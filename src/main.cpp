#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <UniversalTelegramBot.h>
#include <time.h>


// ================= КОРИСТУВАЦЬКІ НАЛАШТУВАННЯ =================
#define SENSOR_PIN 3            // GPIO пін датчика (якщо ESP32 підключено до USB, краще GPIO 18)
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


// ================= ГЛОБАЛЬНІ ЗМІННІ =================
struct Config {
 char botToken[128] = "";
 char chatIds[512] = ""; 
 char locationName[128] = "Моя адреса / Об'єкт";
} config;


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


// Змінні для відкладеного запису стану (очікування інтернету/NTP)
bool pendingStateChange = false;
bool pendingNewState = false;
unsigned long stateChangeMillis = 0;


WebServer server(80);
unsigned long lastPingTime = 0;
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


// ================= ПЕРЕВІРКА ІНТЕРНЕТУ =================
bool checkInternet() {
 if (WiFi.status() != WL_CONNECTED) return false;
 WiFiClient client;
 if (client.connect(IPAddress(8, 8, 8, 8), 53, 1000)) {
   client.stop();
   return true;
 }
 return false;
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


     // Позначаємо як відправлене ТІЛЬКИ якщо Telegram API підтвердив прийом
     if (sendToAllChats(msg)) {
       logs[i].sent = true;
       saveLogs();
     } else {
       isInternetConnected = false; // Якщо не вдалося з'єднатися з TG — скидаємо прапорець інтернету
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


// ================= ВЕБ-СЕРВЕР (Новий UI) =================
void handleRoot() {
 String html = R"rawliteral(<!DOCTYPE html>
<html lang="uk">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>SvitloCheck Pro — Моніторинг мережі живлення</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Geist+Mono:wght@400;500;600&family=Geist:wght@400;500;600&display=swap" rel="stylesheet">
<script src="https://unpkg.com/@phosphor-icons/web"></script>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<script src="https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns"></script>
<style>
   :root, [data-theme="slate"] {
       --bg-body: #070B14; --bg-card: rgba(17, 25, 40, 0.84); --bg-sub: rgba(30, 41, 59, 0.6);
       --text-primary: #F8FAFC; --text-secondary: #94A3B8; --text-muted: #64748B;
       --accent: #38BDF8; --accent-glow: rgba(56, 189, 248, 0.35); --success: #4ADE80;
       --danger: #FB7185; --warning: #F59E0B; --border: rgba(255, 255, 255, 0.08);
       --chart-grid: rgba(255, 255, 255, 0.06); --hero-grad-start: #FFFFFF; --hero-grad-end: #9B9B9B;
       --glow-opacity: 0.24;
   }
   [data-theme="black"] { --bg-body: #000000; --bg-card: #181818; --bg-sub: #1F1F1F; --text-primary: #F8FAFC; --text-secondary: #A1A1AA; --text-muted: #71717A; --accent: #38BDF8; --accent-glow: rgba(56, 189, 248, 0.3); --success: #10B981; --danger: #F43F5E; --warning: #F59E0B; --border: #272727; --chart-grid: #272727; --hero-grad-start: #FFFFFF; --hero-grad-end: #9B9B9B; --glow-opacity: 0.18; }
   [data-theme="light"] { --bg-body: #F1F5F9; --bg-card: rgba(255, 255, 255, 0.9); --bg-sub: #F8FAFC; --text-primary: #0F172A; --text-secondary: #475569; --text-muted: #94A3B8; --accent: #0284C7; --accent-glow: rgba(2, 132, 199, 0.25); --success: #16A34A; --danger: #E11D48; --warning: #D97706; --border: rgba(15, 23, 42, 0.1); --chart-grid: rgba(15, 23, 42, 0.08); --hero-grad-start: #000000; --hero-grad-end: #666666; --glow-opacity: 0.16; }
   [data-theme="amber"] { --bg-body: #131209; --bg-card: rgba(28, 26, 17, 0.88); --bg-sub: rgba(42, 39, 25, 0.65); --text-primary: #FEFCE8; --text-secondary: #D6D3D1; --text-muted: #78716C; --accent: #F59E0B; --accent-glow: rgba(245, 158, 11, 0.35); --success: #4ADE80; --danger: #FB7185; --warning: #FBBF24; --border: rgba(245, 158, 11, 0.14); --chart-grid: rgba(255, 255, 255, 0.06); --hero-grad-start: #FFFFFF; --hero-grad-end: #9B9B9B; --glow-opacity: 0.22; }
   [data-theme="emerald"] { --bg-body: #041311; --bg-card: rgba(6, 30, 26, 0.86); --bg-sub: rgba(11, 46, 40, 0.6); --text-primary: #ECFDF5; --text-secondary: #6EE7B7; --text-muted: #34D399; --accent: #10B981; --accent-glow: rgba(16, 185, 129, 0.35); --success: #34D399; --danger: #FB7185; --warning: #FBBF24; --border: rgba(52, 211, 153, 0.14); --chart-grid: rgba(52, 211, 153, 0.08); --hero-grad-start: #FFFFFF; --hero-grad-end: #9B9B9B; --glow-opacity: 0.25; }
   [data-theme="violet"] { --bg-body: #0D0718; --bg-card: rgba(24, 14, 41, 0.86); --bg-sub: rgba(38, 22, 64, 0.6); --text-primary: #F5F3FF; --text-secondary: #C4B5FD; --text-muted: #8B5CF6; --accent: #A855F7; --accent-glow: rgba(168, 85, 247, 0.38); --success: #4ADE80; --danger: #FB7185; --warning: #F59E0B; --border: rgba(168, 85, 247, 0.16); --chart-grid: rgba(168, 85, 247, 0.08); --hero-grad-start: #FFFFFF; --hero-grad-end: #9B9B9B; --glow-opacity: 0.26; }
   :root { --font-sans: 'Geist', sans-serif; --font-mono: 'Geist Mono', monospace; --ease-base: cubic-bezier(0.22, 1, 0.36, 1); --ease-layout: 500ms var(--ease-base); --ease-color: 300ms var(--ease-base); }
   * { box-sizing: border-box; }
   *:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
   body { margin: 0; background-color: var(--bg-body); color: var(--text-primary); font-family: var(--font-sans); padding: 24px 16px 40px; display: flex; justify-content: center; overflow-x: hidden; min-height: 100vh; position: relative; transition: background-color var(--ease-color), color var(--ease-color); -webkit-font-smoothing: antialiased; }
   .ambient-glow { position: fixed; top: -180px; left: 50%; transform: translateX(-50%); width: 900px; height: 450px; border-radius: 50%; filter: blur(130px); opacity: var(--glow-opacity); pointer-events: none; z-index: 0; transition: background var(--ease-layout), opacity var(--ease-color); }
   .glow-on { background: radial-gradient(circle, var(--success), transparent 70%); } .glow-off { background: radial-gradient(circle, var(--danger), transparent 70%); }
   .heading-2 { font-size: 48px; line-height: 48px; font-weight: 600; letter-spacing: -0.02em; } .heading-3 { font-size: 36px; line-height: 40px; font-weight: 600; letter-spacing: -0.02em; } .heading-4 { font-size: 30px; line-height: 36px; font-weight: 600; letter-spacing: -0.02em; } .heading-5 { font-size: 24px; line-height: 32px; font-weight: 600; } .title-1 { font-size: 20px; line-height: 28px; font-weight: 500; } .title-3 { font-size: 16px; line-height: 24px; font-weight: 500; } .body-2 { font-size: 16px; line-height: 24px; font-weight: 500; } .body-5 { font-size: 14px; line-height: 20px; font-weight: 500; } .caption-2 { font-size: 14px; line-height: 20px; font-weight: 400; } .caption-4 { font-size: 12px; line-height: 16px; font-weight: 500; } .mono-val { font-family: var(--font-mono); }
   .container { width: 100%; max-width: 1180px; position: relative; z-index: 1; }
   .demo-bar { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 12px; background: var(--bg-card); border: 1px solid var(--border); border-radius: 16px; padding: 12px 16px; margin-bottom: 24px; backdrop-filter: blur(12px); }
   .demo-bar-left { display: flex; align-items: center; gap: 12px; color: var(--text-secondary); flex-wrap: wrap; }
   .demo-controls { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
   .theme-dot { width: 12px; height: 12px; border-radius: 50%; display: inline-block; border: 1px solid rgba(255,255,255,0.3); }
   .top-nav { display: flex; justify-content: space-between; align-items: flex-end; flex-wrap: wrap; gap: 16px; margin-bottom: 24px; }
   .loc-pill { display: inline-flex; align-items: center; gap: 8px; color: var(--accent); letter-spacing: 0.03em; text-transform: uppercase; margin-bottom: 8px; }
   .header-gradient { background: linear-gradient(to right, var(--hero-grad-start), var(--hero-grad-end)); -webkit-background-clip: text; -webkit-text-fill-color: transparent; margin: 0; }
   .nav-pills { display: flex; gap: 12px; flex-wrap: wrap; }
   .status-pill { background: var(--bg-card); border: 1px solid var(--border); border-radius: 12px; padding: 8px 16px; display: flex; align-items: center; gap: 8px; backdrop-filter: blur(12px); }
   .dot { width: 8px; height: 8px; border-radius: 50%; display: inline-block; } .dot-online { background-color: var(--success); box-shadow: 0 0 8px var(--success); } .dot-offline { background-color: var(--danger); box-shadow: 0 0 8px var(--danger); }
   .bento-grid { display: grid; grid-template-columns: repeat(12, 1fr); gap: 24px; }
   .col-12 { grid-column: span 12; } .col-6 { grid-column: span 6; } .col-3 { grid-column: span 3; }
   .card { background: var(--bg-card); backdrop-filter: blur(16px); border-radius: 16px; padding: 24px; border: 1px solid var(--border); box-shadow: 0 10px 30px -10px rgba(0, 0, 0, 0.35); transition: background-color var(--ease-color), border-color var(--ease-color), box-shadow var(--ease-layout); }
   .card-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 24px; gap: 16px; flex-wrap: wrap; }
   .card-title { display: flex; align-items: center; gap: 8px; color: var(--text-primary); margin: 0 0 4px 0; }
   .card-subtitle { color: var(--text-secondary); margin: 0; }
   .hero-card { display: grid; grid-template-columns: 1.6fr 1fr; gap: 32px; align-items: center; }
   .hero-on { border-color: rgba(74, 222, 128, 0.35); box-shadow: 0 0 32px -8px rgba(74, 222, 128, 0.18); } .hero-off { border-color: rgba(251, 113, 133, 0.35); box-shadow: 0 0 32px -8px rgba(251, 113, 133, 0.18); }
   @keyframes pulse-glow-on { 0%, 100% { box-shadow: 0 0 0 0 rgba(74, 222, 128, 0.35); } 50% { box-shadow: 0 0 20px 4px rgba(74, 222, 128, 0.15); } }
   @keyframes pulse-glow-off { 0%, 100% { box-shadow: 0 0 0 0 rgba(251, 113, 133, 0.35); } 50% { box-shadow: 0 0 20px 4px rgba(251, 113, 133, 0.15); } }
   .hero-status-row { display: flex; align-items: center; gap: 24px; margin: 16px 0 24px; }
   .icon-container { display: flex; justify-content: center; align-items: center; width: 64px; height: 64px; border-radius: 16px; font-size: 32px; border: 1px solid var(--border); background: var(--bg-sub); transition: transform var(--ease-layout), color var(--ease-color), border-color var(--ease-color); }
   .icon-state-on { color: var(--success); border-color: rgba(74, 222, 128, 0.4); animation: pulse-glow-on 3s infinite; } .icon-state-off { color: var(--danger); border-color: rgba(251, 113, 133, 0.4); animation: pulse-glow-off 2.5s infinite; }
   .status-text-on { color: var(--success); } .status-text-off { color: var(--danger); }
   .hero-timer-box { background: var(--bg-sub); border: 1px solid var(--border); border-radius: 8px; padding: 16px 24px; display: inline-block; min-width: 300px; }
   .duration-sub { color: var(--text-secondary); margin-top: 8px; display: flex; align-items: center; gap: 8px; }
   .hero-sla-box { display: flex; flex-direction: column; align-items: center; justify-content: center; position: relative; height: 240px; }
   .sla-center-label { position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); text-align: center; pointer-events: none; }
   .kpi-card { display: flex; flex-direction: column; justify-content: space-between; min-height: 120px; }
   .kpi-top { display: flex; justify-content: space-between; align-items: center; color: var(--text-secondary); } .kpi-val { margin: 12px 0 8px; } .kpi-foot { color: var(--text-muted); }
   .analytics-controls { display: flex; justify-content: space-between; align-items: flex-end; flex-wrap: wrap; gap: 24px; margin-bottom: 24px; }
   .flex-dates { display: flex; gap: 12px; flex-wrap: wrap; }
   .summary-strips { display: grid; grid-template-columns: repeat(2, 1fr); gap: 16px; margin-bottom: 24px; }
   .strip-box { background: var(--bg-sub); border: 1px solid var(--border); border-radius: 8px; padding: 16px 24px; display: flex; justify-content: space-between; align-items: center; }
   .chart-timeline-box { position: relative; height: 160px; width: 100%; }
   .res-item { margin-bottom: 16px; } .res-label { display: flex; justify-content: space-between; margin-bottom: 8px; color: var(--text-secondary); }
   .progress-track { width: 100%; height: 8px; background: var(--bg-sub); border: 1px solid var(--border); border-radius: 8px; overflow: hidden; }
   .progress-fill { height: 100%; background: var(--text-secondary); border-radius: 8px; transform-origin: left; transition: transform var(--ease-layout); }
   .fill-accent { background: var(--accent); } .fill-success { background: var(--success); } .fill-warning { background: var(--warning); }
   .form-group { margin-bottom: 16px; position: relative; } .form-group label { display: block; color: var(--text-secondary); margin-bottom: 8px; }
   input { width: 100%; padding: 12px 16px; border-radius: 8px; border: 1px solid var(--border); background-color: var(--bg-sub); color: var(--text-primary); font-family: var(--font-sans); font-size: 16px; transition: border-color var(--ease-color), box-shadow var(--ease-color), background-color var(--ease-color); }
   input:focus { outline: none; border-color: var(--accent); box-shadow: 0 0 18px var(--accent-glow); }
   .pass-wrapper { position: relative; } .toggle-pass { position: absolute; right: 12px; top: 50%; transform: translateY(-50%); background: none; border: none; color: var(--text-secondary); cursor: pointer; font-size: 18px; display: flex; align-items: center; padding: 4px; }
   .btn-row { display: flex; gap: 12px; margin-top: 24px; flex-wrap: wrap; }
   .btn { background: var(--accent); color: #000000; padding: 8px 16px; border-radius: 8px; border: 1px solid transparent; cursor: pointer; display: inline-flex; align-items: center; justify-content: center; gap: 8px; font-weight: 600; text-decoration: none; transition: transform var(--ease-layout), opacity var(--ease-color), background var(--ease-color); }
   .btn:hover { opacity: 0.9; } .btn-secondary { background: var(--bg-sub); border: 1px solid var(--border); color: var(--text-primary); font-weight: 500; }
   .btn-secondary:hover { border-color: var(--accent); } .btn-secondary.active { background: var(--accent); color: #000000; border-color: var(--accent); font-weight: 600; }
   .btn-danger { background: transparent; border: 1px solid var(--danger); color: var(--danger); } .btn-danger:hover { background: var(--danger); color: #FFFFFF; }
   .btn-row-del { background: transparent; border: 1px solid var(--border); color: var(--text-secondary); width: 32px; height: 32px; border-radius: 8px; display: inline-flex; align-items: center; justify-content: center; cursor: pointer; transition: all var(--ease-color); }
   .btn-row-del:hover { background: rgba(244, 63, 94, 0.15); border-color: var(--danger); color: var(--danger); }
   .table-wrap { max-height: 360px; overflow-y: auto; border-radius: 8px; border: 1px solid var(--border); background: var(--bg-sub); }
   table { width: 100%; border-collapse: collapse; } th, td { border-bottom: 1px solid var(--border); padding: 12px 16px; text-align: left; } th { color: var(--text-secondary); background: var(--bg-card); position: sticky; top: 0; z-index: 2; }
   tbody tr:hover { background: rgba(255, 255, 255, 0.03); }
   .state-badge { display: inline-flex; align-items: center; gap: 6px; padding: 4px 12px; border-radius: 8px; border: 1px solid var(--border); }
   .badge-on { background: rgba(74, 222, 128, 0.12); color: var(--success); border-color: rgba(74, 222, 128, 0.3); } .badge-off { background: rgba(251, 113, 133, 0.12); color: var(--danger); border-color: rgba(251, 113, 133, 0.3); }
   .empty-state { display: flex; flex-direction: column; align-items: center; justify-content: center; padding: 48px 24px; color: var(--text-muted); text-align: center; } .empty-state i { font-size: 40px; margin-bottom: 12px; }
  
   /* Футер та юридичні положення */
   .legal-footer { margin-top: 40px; padding-top: 24px; border-top: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 16px; color: var(--text-muted); }
   .legal-links { display: flex; gap: 16px; flex-wrap: wrap; }
   .legal-link { color: var(--text-secondary); text-decoration: none; cursor: pointer; transition: color var(--ease-color); background: none; border: none; font-family: inherit; font-size: 13px; padding: 0; }
   .legal-link:hover { color: var(--accent); text-decoration: underline; }
  
   /* Модальне вікно */
   .modal-overlay { position: fixed; inset: 0; background: rgba(0, 0, 0, 0.75); backdrop-filter: blur(8px); display: none; justify-content: center; align-items: center; z-index: 100; padding: 16px; }
   .modal-overlay.open { display: flex; }
   .modal-box { background: var(--bg-card); border: 1px solid var(--border); border-radius: 16px; max-width: 680px; width: 100%; max-height: 85vh; display: flex; flex-direction: column; box-shadow: 0 20px 50px rgba(0,0,0,0.6); }
   .modal-header { padding: 20px 24px; border-bottom: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; }
   .modal-body { padding: 24px; overflow-y: auto; color: var(--text-secondary); line-height: 1.6; font-size: 14px; }
   .modal-body h4 { color: var(--text-primary); margin: 16px 0 8px 0; font-size: 16px; }
   .modal-body h4:first-child { margin-top: 0; }
   .modal-body ul { padding-left: 20px; margin: 8px 0; }
   .modal-close { background: var(--bg-sub); border: 1px solid var(--border); color: var(--text-primary); width: 34px; height: 34px; border-radius: 8px; cursor: pointer; display: flex; align-items: center; justify-content: center; }
   .modal-close:hover { border-color: var(--danger); color: var(--danger); }


   @media (max-width: 960px) { .hero-card { grid-template-columns: 1fr; } .col-6, .col-3 { grid-column: span 12; } .summary-strips { grid-template-columns: 1fr; } .legal-footer { flex-direction: column; text-align: center; } }
</style>
</head>
<body data-theme="slate">
<div id="ambientGlow" class="ambient-glow glow-on"></div>
<div class="container">
   <div class="demo-bar">
       <div class="demo-bar-left caption-2"><i class="ph ph-palette" style="font-size: 18px; color: var(--accent);"></i><span>Тема оформлення:</span></div>
       <div class="demo-controls" id="themeButtons">
           <button class="btn btn-secondary active caption-2" data-theme-btn="slate" onclick="setTheme('slate')"><span class="theme-dot" style="background: #070B14;"></span> Темна</button>
           <button class="btn btn-secondary caption-2" data-theme-btn="black" onclick="setTheme('black')"><span class="theme-dot" style="background: #000000;"></span> Чорна OLED</button>
           <button class="btn btn-secondary caption-2" data-theme-btn="light" onclick="setTheme('light')"><span class="theme-dot" style="background: #F1F5F9;"></span> Світла</button>
           <button class="btn btn-secondary caption-2" data-theme-btn="amber" onclick="setTheme('amber')"><span class="theme-dot" style="background: #F59E0B;"></span> Бурштинова</button>
           <button class="btn btn-secondary caption-2" data-theme-btn="emerald" onclick="setTheme('emerald')"><span class="theme-dot" style="background: #10B981;"></span> Смарагдова</button>
           <button class="btn btn-secondary caption-2" data-theme-btn="violet" onclick="setTheme('violet')"><span class="theme-dot" style="background: #A855F7;"></span> Аметистова</button>
       </div>
   </div>


   <header class="top-nav">
       <div>
           <div class="loc-pill caption-4"><i class="ph ph-map-pin"></i> Об'єкт моніторингу: <span id="locName" style="color: var(--text-primary);">Завантаження...</span></div>
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
                   <div><div id="currState" class="heading-3 status-text-on">ОНОВЛЕННЯ...</div><div class="body-5" style="color: var(--text-secondary); margin-top: 4px;" id="stateSubDesc">Синхронізація даних...</div></div>
               </div>
               <div class="hero-timer-box">
                   <div class="heading-4 mono-val" id="currDuration">...</div>
                   <div class="duration-sub caption-2"><span>Тривалість поточного стану</span></div>
               </div>
           </div>
           <div class="hero-sla-box">
               <canvas id="slaChart"></canvas>
               <div class="sla-center-label"><span class="heading-5 mono-val" id="slaCenterPct">...</span><span class="caption-4" style="color: var(--text-secondary); display: block;">Зі світлом</span></div>
           </div>
       </div>


       <div class="card col-3 kpi-card"><div class="kpi-top caption-2"><span>Індекс SLA (Вибірка)</span><i class="ph ph-chart-line-up"></i></div><div class="kpi-val heading-5 mono-val" style="color: var(--success);" id="kpiSla">...</div></div>
       <div class="card col-3 kpi-card"><div class="kpi-top caption-2"><span>Відключень за період</span><i class="ph ph-lightning"></i></div><div class="kpi-val heading-5 mono-val" id="kpiOutages">...</div></div>
       <div class="card col-3 kpi-card"><div class="kpi-top caption-2"><span>Пам'ять контролера</span><i class="ph ph-cpu"></i></div><div class="kpi-val heading-5 mono-val" style="color: var(--warning);" id="kpiRam">...</div></div>
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
               <div class="strip-box"><div><div class="caption-2" style="color: var(--text-secondary);">Загальний час зі світлом</div><div class="title-1 mono-val" style="margin-top: 4px;" id="repOnText">...</div></div><div class="heading-5 mono-val" style="color: var(--success);" id="repOnPct">...</div></div>
               <div class="strip-box"><div><div class="caption-2" style="color: var(--text-secondary);">Загальний час без світла</div><div class="title-1 mono-val" style="margin-top: 4px;" id="repOffText">...</div></div><div class="heading-5 mono-val" style="color: var(--danger);" id="repOffPct">...</div></div>
           </div>
           <div class="chart-timeline-box"><canvas id="tlChart"></canvas></div>
       </div>


       <div class="card col-6">
           <div class="card-header"><div><h3 class="title-1 card-title"><i class="ph ph-cpu"></i> Телеметрія мікроконтролера</h3></div><span class="demo-badge caption-4 mono-val" id="cpuLoad">...</span></div>
           <div class="res-item"><div class="res-label body-5"><span>Оперативна пам'ять (Heap RAM)</span><span class="mono-val" id="ramUsageText">...</span></div><div class="progress-track"><div class="progress-fill fill-accent" id="ramBar" style="transform: scaleX(0);"></div></div></div>
           <div class="res-item"><div class="res-label body-5"><span>Файлова система (LittleFS)</span><span class="mono-val" id="fsUsageText">...</span></div><div class="progress-track"><div class="progress-fill fill-success" id="fsBar" style="transform: scaleX(0);"></div></div></div>
           <div class="res-item"><div class="res-label body-5"><span>Сигнал Wi-Fi (<span style="color: var(--accent);" id="wifiSsid">...</span>)</span><span class="mono-val" id="wifiSignalText">...</span></div><div class="progress-track"><div class="progress-fill fill-warning" id="wifiBar" style="transform: scaleX(0);"></div></div></div>
       </div>


       <div class="card col-6">
           <div class="card-header"><div><h3 class="title-1 card-title"><i class="ph ph-gear"></i> Налаштування Telegram</h3></div></div>
           <form action="/save" method="POST">
               <div class="form-group"><label class="body-5">Назва об'єкта / локації:</label><input type="text" name="location" id="inpLocation" value=""></div>
               <div class="form-group"><label class="body-5">Telegram Bot Token:</label>
                   <div class="pass-wrapper">
                       <input type="password" name="token" id="inpToken" value="">
                       <button type="button" class="toggle-pass" onclick="toggleTokenVis()" title="Показати / приховати токен"><i class="ph ph-eye" id="eyeIcon"></i></button>
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
                   <p class="body-5 card-subtitle">Експортуйте таблицю у CSV або видаляйте окремі помилкові спрацювання</p>
               </div>
               <div style="display: flex; gap: 8px; flex-wrap: wrap;">
                                       <button class="btn btn-secondary active caption-2" id="fltAll" onclick="filterLogs('all')">Всі події</button>
                    <button class="btn btn-secondary caption-2" id="fltOn" onclick="filterLogs('on')">З'явилось</button>
                    <button class="btn btn-secondary caption-2" id="fltOff" onclick="filterLogs('off')">Зникло</button>
                   <a href="/api/export" download="svitlocheck_history.csv" class="btn btn-secondary caption-2"><i class="ph ph-download-simple"></i> Експорт CSV</a>
                   <button class="btn btn-danger caption-2" onclick="clearHistory()"><i class="ph ph-trash"></i> Очистити базу</button>
               </div>
           </div>
           <div class="table-wrap">
               <table>
                                       <thead class="caption-2"><tr><th>#</th><th>Дата</th><th>Час</th><th>Подія</th><th>Минулий стан</th><th>Статус TG</th><th style="text-align: right;">Дія</th></tr></thead>
                   <tbody class="body-5" id="logTable"></tbody>
               </table>
           </div>
       </div>
   </div>


   <!-- Юридичний футер для комерційного продажу -->
   <footer class="legal-footer caption-2">
       <div>© 2026 <b>SvitloCheck Pro</b> — Апаратно-програмний комплекс моніторингу електромережі. Усі права захищено.</div>
       <div class="legal-links">
           <button class="legal-link" onclick="openModal('termsModal')"><i class="ph ph-file-text"></i> Умови використання та Гарантія</button>
           <button class="legal-link" onclick="openModal('safetyModal')"><i class="ph ph-shield-warning"></i> Обмеження відповідальності</button>
           <button class="legal-link" onclick="openModal('privacyModal')"><i class="ph ph-lock-key"></i> Політика конфіденційності</button>
       </div>
   </footer>
</div>


<!-- Модальне вікно: Умови використання та Гарантія -->
<div class="modal-overlay" id="termsModal" onclick="if(event.target===this)closeModal('termsModal')">
   <div class="modal-box">
       <div class="modal-header">
           <h3 class="title-1 card-title" style="margin:0;"><i class="ph ph-file-text"></i> Умови використання та Гарантійні зобов'язання</h3>
           <button class="modal-close" onclick="closeModal('termsModal')"><i class="ph ph-x"></i></button>
       </div>
       <div class="modal-body">
           <h4>1. Призначення пристрою</h4>
           <p>Пристрій <b>SvitloCheck Pro</b> призначений виключно для інформаційного побутового та комерційного моніторингу наявності напруги в однофазній мережі змінного струму та надсилання сповіщень через мережу Інтернет.</p>
           <h4>2. Умови експлуатації</h4>
           <ul>
               <li>Пристрій повинен експлуатуватися в сухих опалюваних приміщеннях за температури від +5°C до +40°C та відносної вологості до 80%.</li>
               <li>Для коректної фіксації часу відновлення живлення роутер користувача та пристрій повинні мати стабільне Wi-Fi покриття та доступ до серверів точного часу (NTP).</li>
           </ul>
           <h4>3. Гарантійні зобов'язання</h4>
           <p>Гарантійний термін експлуатації становить <b>12 місяців</b> з моменту придбання. Гарантія втрачає чинність у разі механічних пошкоджень корпусу, потрапляння рідини, самостійного втручання в схемотехніку або пробою високої напруги внаслідок аварії в електромережі понад допустимі норми блоку живлення/оптопари.</p>
       </div>
   </div>
</div>


<!-- Модальне вікно: Обмеження відповідальності -->
<div class="modal-overlay" id="safetyModal" onclick="if(event.target===this)closeModal('safetyModal')">
   <div class="modal-box">
       <div class="modal-header">
           <h3 class="title-1 card-title" style="margin:0;"><i class="ph ph-shield-warning"></i> Положення про обмеження відповідальності</h3>
           <button class="modal-close" onclick="closeModal('safetyModal')"><i class="ph ph-x"></i></button>
       </div>
       <div class="modal-body">
           <h4>1. Статус некритичного обладнання</h4>
           <p>Даний пристрій <b>не є засобом комерційного обліку електроенергії</b>, сертифікованим вимірювальним приладом, медичним обладнанням або системою протипожежної чи аварійної безпеки. Дані журналу подій не можуть бути єдиною правовою підставою для судових чи фінансових претензій до операторів системи розподілу (ОСР / обленерго).</p>
           <h4>2. Відмова від відповідальності за збитки</h4>
           <p>Виробник та продавець не несуть відповідальності за будь-які прямі чи непрямі майнові збитки (псування продуктів харчування, вихід з ладу побутової техніки, простій бізнесу), викликані затримкою чи відсутністю сповіщень у месенджері Telegram внаслідок збоїв провайдера Інтернет, відсутності живлення маршрутизатора або блокування API серверів Telegram.</p>
           <h4>3. Електробезпека</h4>
           <p>Категорично забороняється розбирати пристрій, увімкнений у мережу 220В, або експлуатувати виріб із пошкодженою ізоляцією мережевого кабелю чи сенсора.</p>
       </div>
   </div>
</div>


<!-- Модальне вікно: Політика конфіденційності -->
<div class="modal-overlay" id="privacyModal" onclick="if(event.target===this)closeModal('privacyModal')">
   <div class="modal-box">
       <div class="modal-header">
           <h3 class="title-1 card-title" style="margin:0;"><i class="ph ph-lock-key"></i> Політика конфіденційності та локального зберігання даних</h3>
           <button class="modal-close" onclick="closeModal('privacyModal')"><i class="ph ph-x"></i></button>
       </div>
       <div class="modal-body">
           <h4>1. Повна автономність та приватність</h4>
           <p>Пристрій працює за принципом <b>100% Local Storage</b>. Жодні дані про вашу локацію, графік присутності, паролі від Wi-Fi чи токени не передаються на сторонні хмарні сервери виробника.</p>
           <h4>2. Збереження облікових даних</h4>
           <p>Усі налаштування (SSID та пароль Wi-Fi, Telegram Bot Token, список Chat ID та історія подій) зберігаються виключно у внутрішній енергонезалежній флеш-пам'яті мікроконтролера (файлова система LittleFS). Доступ до веб-панелі можливий лише всередині вашої локальної мережі.</p>
           <h4>3. Передача пристрою третім особам</h4>
           <p>Перед перепродажем або передачею пристрою іншому користувачу рекомендується очистити журнал подій та видалити збережений Bot Token і налаштування Wi-Fi.</p>
       </div>
   </div>
</div>


<script>
   let slaChart = null, tlChart = null;
   let isFirstLoad = true, logsData = [];
   let currentFilter = 'all';


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


   function loadData() {
       fetch('/api/data?t=' + new Date().getTime()).then(r => r.json()).then(d => {
           document.getElementById('locName').innerText = d.location;
          
           // Заповнення збережених полів при першому відкритті сторінки
           if (isFirstLoad) {
               document.getElementById('inpLocation').value = d.location || '';
               document.getElementById('inpToken').value = d.botToken || '';
               document.getElementById('inpChats').value = d.chatIds || '';


               let firstDate = new Date((d.firstLogTime || Math.floor(Date.now()/1000)) * 1000);
               if (firstDate.getFullYear() < 2020) firstDate = new Date();
               const off = firstDate.getTimezoneOffset() * 60000;
               document.getElementById('dateStart').value = new Date(firstDate.getTime() - off).toISOString().split('T')[0];
               document.getElementById('dateEnd').value = new Date(new Date().getTime() - off).toISOString().split('T')[0];
               isFirstLoad = false;
               updateReport();
           }


           let st = document.getElementById('currState'), cd = document.getElementById('cardState'), ic = document.getElementById('statusIcon'), gl = document.getElementById('ambientGlow'), desc = document.getElementById('stateSubDesc');
           if(d.currentState) {
               st.className="heading-3 status-text-on"; st.innerText="СВІТЛО Є"; cd.className="card col-12 hero-card hero-on";
               ic.className="icon-container icon-state-on"; ic.innerHTML='<i class="ph ph-lightbulb"></i>'; gl.className="ambient-glow glow-on";
               desc.innerText="Живлення від основної мережі стабільне";
           } else {
               st.className="heading-3 status-text-off"; st.innerText="СВІТЛА НЕМАЄ"; cd.className="card col-12 hero-card hero-off";
               ic.className="icon-container icon-state-off"; ic.innerHTML='<i class="ph ph-plug"></i>'; gl.className="ambient-glow glow-off";
               desc.innerText="Аварійне або планове відключення лінії живлення";
           }
           document.getElementById('currDuration').innerText = d.currentDuration;


           document.getElementById('ramUsageText').innerText = d.ramUsage;
           if(d.ramUsage) { document.getElementById('ramBar').style.transform = `scaleX(${parseFloat(d.ramUsage)/100})`; document.getElementById('kpiRam').innerText = d.ramUsage.split(' ')[0]; }
           document.getElementById('fsUsageText').innerText = d.fsUsage;
           if(d.fsUsage) document.getElementById('fsBar').style.transform = `scaleX(${parseFloat(d.fsUsage)/100})`;
          
           document.getElementById('wifiSsid').innerText = d.wifiSsid || 'N/A';
           document.getElementById('wifiSignalText').innerText = d.wifiSignal || 'N/A';
           if(d.wifiSignal) document.getElementById('wifiBar').style.transform = `scaleX(${parseFloat(d.wifiSignal)/100})`;


           document.getElementById('cpuLoad').innerText = d.cpuLoad || '...';
           document.getElementById('sysUptime').innerText = d.uptime || '...';


           let intEl = document.getElementById('sysInternet'), dotEl = document.getElementById('netDot');
           if (d.internet) { intEl.innerText = "Є зв'язок"; intEl.style.color = "var(--success)"; dotEl.className = "dot dot-online"; }
           else { intEl.innerText = "Відсутній"; intEl.style.color = "var(--danger)"; dotEl.className = "dot dot-offline"; }


           logsData = d.logs || [];
           document.getElementById('kpiLogs').innerText = `${d.totalLogs || logsData.length} / 500`;
           renderLogsTable();
           updateReport();
       });
   }


       function renderLogsTable() {
        let tbody = document.getElementById('logTable');
        let filtered = logsData.filter(l => {
            if (currentFilter === 'on') return l.s === true;
            if (currentFilter === 'off') return l.s === false;
            return true;
        });

        if (filtered.length === 0) {
             tbody.innerHTML = `<tr><td colspan="7"><div class="empty-state"><i class="ph ph-tray"></i><div class="body-2">Журнал порожній</div></div></td></tr>`;
             return;
        }

        let th = '';
        filtered.forEach(l => {
            let dt = l.t.split(' ');
            let dateStr = dt[0] || '';
            let timeStr = dt[1] || '';
            let isLightOn = l.s;
            let badgeClass = isLightOn ? 'badge-on' : 'badge-off';
            let badgeIcon = isLightOn ? 'ph-lightbulb' : 'ph-plug';
            let badgeText = isLightOn ? '🟢 З\'явилося світло' : '🔴 Зникло світло';
            let durLabel = isLightOn ? 'Не було: ' : 'Було: ';
            let tgStatus = l.sent ? '<span style="color: var(--success);"><i class="ph ph-check"></i> Доставлено</span>' : '<span style="color: var(--warning);"><i class="ph ph-clock"></i> В черзі</span>';
            th += `<tr>
                <td class="mono-val" style="color: var(--text-muted);">#${l.idx + 1}</td>
                <td class="mono-val">${dateStr}</td>
                <td class="mono-val">${timeStr}</td>
                <td><span class="state-badge caption-4 ${badgeClass}"><i class="ph ${badgeIcon}"></i> ${badgeText}</span></td>
                <td class="mono-val">${durLabel}${l.d}</td>
                <td>${tgStatus}</td>
                <td style="text-align: right;">
                    <button class="btn-row-del" onclick="deleteLogEntry(${l.idx})" title="Видалити цей запис">
                        <i class="ph ph-trash"></i>
                    </button>
                </td>
            </tr>`;
        });
        tbody.innerHTML = th;
    }


   function deleteLogEntry(idx) {
       if (!confirm("Видалити запис #" + (idx + 1) + " із журналу?")) return;
       fetch('/api/delete?index=' + idx, { method: 'POST' }).then(r => {
           if (r.ok) {
               loadData();
           }
       });
   }


   function filterLogs(mode) {
       currentFilter = mode;
       document.getElementById('fltAll').classList.toggle('active', mode === 'all');
       document.getElementById('fltOn').classList.toggle('active', mode === 'on');
       document.getElementById('fltOff').classList.toggle('active', mode === 'off');
       renderLogsTable();
   }


   function updateReport() {
       let ds = document.getElementById('dateStart').value, de = document.getElementById('dateEnd').value;
       if (!ds || !de) return;
       let ts = Math.floor(new Date(ds + "T00:00:00").getTime() / 1000), te = Math.floor(new Date(de + "T23:59:59").getTime() / 1000);
      
       fetch(`/api/report?start=${ts}&end=${te}`).then(r => r.json()).then(r => {
           let total = r.on_sec + r.off_sec;
           let onPct = total > 0 ? ((r.on_sec / total) * 100).toFixed(1) : 0;
           let offPct = total > 0 ? ((r.off_sec / total) * 100).toFixed(1) : 0;
          
           document.getElementById('repOnText').innerText = r.on; document.getElementById('repOnPct').innerText = onPct + '%';
           document.getElementById('repOffText').innerText = r.off; document.getElementById('repOffPct').innerText = offPct + '%';
           document.getElementById('slaCenterPct').innerText = onPct + '%'; document.getElementById('kpiSla').innerText = onPct + '%';
          
           let outages = (r.events || []).filter(e => !e.state).length;
           document.getElementById('kpiOutages').innerText = outages + ' разів';


           renderCharts(onPct, offPct, r.events || [], ds, de);
       });
   }


   function renderCharts(onPct, offPct, events, dsStr, deStr) {
       Chart.defaults.font.family = "'Geist Mono', monospace";
       if (!slaChart) {
   slaChart = new Chart(document.getElementById('slaChart').getContext('2d'), {
       type: 'doughnut', data: { labels: ['Є світло', 'Немає світла'], datasets: [{ data: [onPct, offPct], backgroundColor: ['#4ADE80', '#FB7185'], borderWidth: 0, hoverOffset: 12 }] },
       options: {
           responsive: true,
           maintainAspectRatio: false,
           cutout: '74%',
           layout: { padding: 16 },
           plugins: {
               legend: { display: false },
               tooltip: {
                   displayColors: false, // Приховує кольоровий квадрат
                   callbacks: {
                       label: function(context) {
                           return context.parsed + '%'; // Повертає лише число та знак відсотка
                       }
                   }
               }
           }
       }
   });
}
           else { slaChart.data.datasets[0].data = [onPct, offPct]; slaChart.update('none'); }


       let dataOn = [], dataOff = [];
       events.forEach(ev => {
           if (ev.start > 1000000000) {
               let obj = { x: [ev.start * 1000, ev.end * 1000], y: 'Мережа' };
               if (ev.state) dataOn.push(obj); else dataOff.push(obj);
           }
       });
       let minTime = new Date(dsStr + "T00:00:00").getTime(), maxTime = new Date(deStr + "T23:59:59").getTime();


       if (!tlChart) {
           tlChart = new Chart(document.getElementById('tlChart').getContext('2d'), {
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
   function clearHistory() { if (confirm("Очистити історію логів повністю?")) { fetch('/api/clear', { method: 'POST' }).then(() => { isFirstLoad = true; if(tlChart) { tlChart.destroy(); tlChart = null; } loadData(); }); } }


   setTheme(localStorage.getItem('svitlo_theme') || 'slate');
   setInterval(loadData, 2000); loadData();
</script>
</body>
</html>)rawliteral";
 server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
 server.send(200, "text/html", html);
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
 // Додаємо UTF-8 BOM, щоб Microsoft Excel коректно відкривав українські літери
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
  saveConfig();
 if (strlen(config.botToken) > 0) {
   if (bot) delete bot;
   bot = new UniversalTelegramBot(config.botToken, secClient);
 }
 server.sendHeader("Location", "/");
 server.send(303);
}


void handleApiData() {
 DynamicJsonDocument doc(16384);
 doc["location"] = config.locationName;
 doc["botToken"] = config.botToken;
 doc["chatIds"] = config.chatIds;
 doc["currentState"] = currentState;
 doc["totalLogs"] = logCount;
  time_t now = time(NULL);
 if(now > 1000000000 && lastStateChangeTime > 1000000000) {
     doc["currentDuration"] = formatDuration((uint32_t)(now - lastStateChangeTime));
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


 JsonArray arr = doc.createNestedArray("logs");
 int renderCount = 0;
 for (int i = logCount - 1; i >= 0 && renderCount < 100; i--, renderCount++) {
   JsonObject obj = arr.createNestedObject();
   obj["idx"] = i;
   obj["t"] = formatTime(logs[i].timestamp);
   obj["s"] = logs[i].state;
   obj["d"] = formatDuration(logs[i].duration);
   obj["sent"] = logs[i].sent;
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


 DynamicJsonDocument doc(16384);
 JsonArray events = doc.createNestedArray("events");


 for (int i = 0; i < logCount; i++) {
   uint32_t entryEnd = logs[i].timestamp;
   uint32_t entryStart = entryEnd - logs[i].duration;
   uint32_t overlapStart = (entryStart > tStart) ? entryStart : tStart;
   uint32_t overlapEnd = (entryEnd < tEnd) ? entryEnd : tEnd;
  
   if (overlapStart < overlapEnd) {
     // Якщо logs[i].state == false (світло зникло), то період ДО цього був зі світлом (!logs[i].state == true)
     if (!logs[i].state) timeOn += (overlapEnd - overlapStart);
     else timeOff += (overlapEnd - overlapStart);              
    
     JsonObject ev = events.createNestedObject();
     ev["state"] = !logs[i].state;
     ev["start"] = overlapStart;
     ev["end"] = overlapEnd;
   }
 }


 uint32_t currEnd = (now < tEnd) ? now : tEnd;
 uint32_t currOverlapStart = (lastStateChangeTime > tStart) ? lastStateChangeTime : tStart;
  if (currOverlapStart < currEnd && lastStateChangeTime > 1000000000) {
   if (currentState) timeOn += (currEnd - currOverlapStart);
   else timeOff += (currEnd - currOverlapStart);
  
   JsonObject ev = events.createNestedObject();
   ev["state"] = currentState;
   ev["start"] = currOverlapStart;
   ev["end"] = currEnd;
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


 WiFiManager wm;
 wm.setConfigPortalTimeout(180);
 if (!wm.autoConnect("ESP32-Power-Monitor")) {
   ESP.restart();
 }


 secClient.setInsecure();
  if (strlen(config.botToken) > 0) {
   bot = new UniversalTelegramBot(config.botToken, secClient);
 }


 // Чекаємо на початковий час до 60 секунд, якщо роутер тільки-но піднявся
 syncNTP();
 unsigned long startNtp = millis();
 while (time(NULL) < 1000000000 && millis() - startNtp < 60000) {
     delay(500);
     if(millis() % 3000 < 500) isInternetConnected = checkInternet();
 }


 isInternetConnected = checkInternet();
 lastPingTime = millis();
  delay(9);
 currentState = isPowerPresent();
 lastStateChangeTime = time(NULL);


 server.on("/", handleRoot);
 server.on("/save", HTTP_POST, handleSave);
 server.on("/api/data", handleApiData);
 server.on("/api/report", handleApiReport);
 server.on("/api/clear", HTTP_POST, handleApiClear);
 server.on("/api/delete", HTTP_POST, handleApiDelete);
 server.on("/api/export", HTTP_GET, handleApiExportCsv);
 server.on("/wifi", []() {
   String msg = "<html><body style='background:#0F172A;color:#F1F5F9;font-family:sans-serif;text-align:center;padding:40px;'>"
                "<h2 style='color:#38BDF8;'>Режим налаштування Wi-Fi активовано!</h2>"
                "<p>Підключіться до: <b>ESP32-Power-Monitor</b>, адреса: <b>http://192.168.4.1</b></p></body></html>";
   server.send(200, "text/html", msg);
   delay(1000); server.stop();
   WiFiManager wm; wm.setConfigPortalTimeout(180); wm.startConfigPortal("ESP32-Power-Monitor"); ESP.restart();
 });
 server.begin();
}


// ================= MAIN LOOP =================
void loop() {
 server.handleClient();


 if (millis() - lastPingTime > 15000) {
   isInternetConnected = checkInternet();
   lastPingTime = millis();
 }


 // --- ЛОГІКА ОЧІКУВАННЯ РОУТЕРА ТА ВІДРАХУНКУ ЧАСУ ---
 bool newRead = isPowerPresent();
  // 1. Початок відліку зміни стану (з базовим антибрязкотом)
 if (newRead != currentState) {
   if (!pendingStateChange) {
     pendingStateChange = true;
     pendingNewState = newRead;
     stateChangeMillis = millis();
   }
 } else {
   // Якщо стан повернувся до поточного до того, як ми його обробили - скасовуємо зміну
   pendingStateChange = false;
 }


 if (pendingStateChange) {
   bool readyToProcess = false;


   // Антибрязкіт: чекаємо мінімум 500мс стабільного сигналу перед тим як реагувати
   if (millis() - stateChangeMillis > 500) {
    
     if (pendingNewState == true) {
       // СВІТЛО З'ЯВИЛОСЯ. Чекаємо на інтернет і вірний годинник
       if (isInternetConnected && time(NULL) > 1000000000) {
         readyToProcess = true;
       } else {
         // Якщо немає інтернету, пробуємо перепідключитись/пінганути
         if (millis() - lastPingTime > 3000) {
            isInternetConnected = checkInternet();
            if (isInternetConnected) syncNTP();
            lastPingTime = millis();
         }
         // Таймаут очікування роутера - 10 хвилин
         if (millis() - stateChangeMillis > 600000) {
             readyToProcess = true;
         }
       }
     } else {
       // СВІТЛО ЗНИКЛО. Обробляємо відразу після антибрязкоту
       readyToProcess = true;
     }
   }


   if (readyToProcess) {
     time_t now = time(NULL);
     // Віднімаємо час, який ми чекали, щоб зафіксувати точний час переходу
     unsigned long delaySec = (millis() - stateChangeMillis) / 1000;
     time_t actualChangeTime = (now > delaySec && now > 1000000000) ? (now - delaySec) : now;


     // Захист від помилок розрахунку часу
     if (lastStateChangeTime > 1000000000 && actualChangeTime <= lastStateChangeTime) {
         actualChangeTime = lastStateChangeTime + 1;
     }


     uint32_t duration = (lastStateChangeTime > 1000000000) ? (uint32_t)(actualChangeTime - lastStateChangeTime) : 0;


     if (logCount >= MAX_LOGS) {
       memmove(&logs[0], &logs[1], sizeof(LogEntry) * (MAX_LOGS - 1));
       logCount = MAX_LOGS - 1;
     }


     if (lastStateChangeTime > 1000000000) {
         logs[logCount].timestamp = actualChangeTime;
         logs[logCount].state = pendingNewState;
         logs[logCount].duration = duration;
         logs[logCount].sent = false;
         logCount++;
     }


     // Застосовуємо новий стан
     currentState = pendingNewState;
     lastStateChangeTime = actualChangeTime;
     saveLogs();


     pendingStateChange = false;
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


 static int lastSyncDay = -1;
 struct tm timeinfo;
 if (getLocalTime(&timeinfo)) {
   if (timeinfo.tm_wday == 0 && timeinfo.tm_hour == 2 && timeinfo.tm_min == 0 && lastSyncDay != timeinfo.tm_yday) {
     if(WiFi.status() == WL_CONNECTED) { syncNTP(); lastSyncDay = timeinfo.tm_yday; delay(1000); }
   }
 }
}
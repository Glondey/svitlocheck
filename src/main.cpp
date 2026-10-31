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
#define SENSOR_PIN 18             
#define NTP_SERVER "pool.ntp.org" 

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
  bool state;       
  uint32_t duration; 
  bool sent;        
};

LogEntry logs[MAX_LOGS];
int logCount = 0;

bool currentState = false;
unsigned long lastStateChangeTime = 0;

WebServer server(80);
unsigned long lastPingTime = 0;
bool isInternetConnected = false;
WiFiClientSecure secClient;
UniversalTelegramBot* bot = NULL;

unsigned long lastTelegramCheck = 0;
const unsigned long TELEGRAM_INTERVAL = 3000; 

// Виправлений формат кнопок (тільки масив), бібліотека сама додасть необхідні JSON теги
const String kbdMarkup = "[[\"📊 Стан\", \"📈 Звіт\"]]";

String formatDuration(uint32_t sec);

// ================= ФУНКЦІЇ ПАМ'ЯТІ =================
void saveConfig() {
  DynamicJsonDocument doc(512);
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
    DynamicJsonDocument doc(512);
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
  struct tm timeinfo;
  getLocalTime(&timeinfo);
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
void sendToAllChats(String text) {
  if (strlen(config.botToken) == 0 || bot == NULL) return;
  
  String chats = String(config.chatIds);
  int start = 0;
  int end = chats.indexOf(',');
  
  while (end != -1) {
    String chatId = chats.substring(start, end);
    chatId.trim();
    if (chatId.length() > 0) {
      bot->sendMessageWithReplyKeyboard(chatId, text, "HTML", kbdMarkup, true, false, false);
    }
    start = end + 1;
    end = chats.indexOf(',', start);
  }
  String lastId = chats.substring(start);
  lastId.trim();
  if (lastId.length() > 0) {
    bot->sendMessageWithReplyKeyboard(lastId, text, "HTML", kbdMarkup, true, false, false);
  }
}

void processUnsentMessages() {
  if (WiFi.status() != WL_CONNECTED) return;

  for (int i = 0; i < logCount; i++) {
    if (!logs[i].sent) {
      String currStateStr = logs[i].state ? "🟢 Світло є" : "🔴 Світла немає";
      String transitionStr = logs[i].state ? "🔴 Немає світла ➔ 🟢 Є світло" : "🟢 Є світло ➔ 🔴 Немає світла";

      String msg = "<b>Поточний стан:</b> " + currStateStr + "\n";
      msg += "📍 <b>Адреса:</b> " + String(config.locationName) + "\n";
      msg += "📅 <b>Дата, час:</b> " + formatTime(logs[i].timestamp) + "\n";
      msg += "🔄 <b>Змінилося з:</b> " + transitionStr + "\n";
      msg += "⏱️ <b>Минулий стан тривав:</b> " + formatDuration(logs[i].duration);

      sendToAllChats(msg);
      logs[i].sent = true;
      isInternetConnected = true; 
      saveLogs();
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
        
        String msg = "<b>Поточний стан:</b> " + currStateStr + "\n";
        msg += "📍 <b>Адреса:</b> " + String(config.locationName) + "\n";
        msg += "📅 <b>Дата, час:</b> " + formatTime(now) + "\n";
        msg += "⏱️ <b>Поточний стан триває:</b> " + formatDuration((uint32_t)(now - lastStateChangeTime)) + "\n\n";
        msg += "<b>Останні 5 змін:</b>\n";
        
        int startIdx = (logCount > 5) ? (logCount - 5) : 0;
        for (int j = logCount - 1; j >= startIdx; j--) {
          msg += (logs[j].state ? "🟢 Ввімк.: " : "🔴 Вимк.: ") + formatTime(logs[j].timestamp) + "\n";
        }
        bot->sendMessageWithReplyKeyboard(from_id, msg, "HTML", kbdMarkup, true, false, false);
      }
      else if (text.indexOf("/report") != -1 || text.indexOf("Звіт") != -1) {
        bot->sendMessageWithReplyKeyboard(from_id, generateReportText(7 * 24 * 3600), "HTML", kbdMarkup, true, false, false);
      } 
      else if (text == "/start") {
        bot->sendMessageWithReplyKeyboard(from_id, "Привіт! Я бот для моніторингу електромережі. Скористайтеся кнопками нижче:", "HTML", kbdMarkup, true, false, false);
      }
    }
    numNewMessages = bot->getUpdates(bot->last_message_received + 1);
  }
}

// ================= ВЕБ-СЕРВЕР =================
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="uk">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Моніторинг мережі живлення</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<script src="https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns"></script>
<style>
    :root {
        --bg-body: #0F172A; --bg-card: #1E293B; --text-primary: #F1F5F9;
        --text-secondary: #94A3B8; --accent: #38BDF8; --success: #4ADE80;
        --danger: #FB7185; --border: #334155;
        --font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
    }
    body { margin: 0; background-color: var(--bg-body); color: var(--text-primary); font-family: var(--font-family); padding: 20px; display: flex; justify-content: center; }
    .container { width: 100%; max-width: 1000px; }
    .header { margin-bottom: 30px; text-align: center; }
    .header-title { font-size: 1.75rem; font-weight: 700; margin: 0; color: var(--accent); }
    .header-subtitle { color: var(--text-secondary); margin-top: 5px; font-size: 1rem; }
    .dashboard-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 20px; margin-bottom: 20px; }
    .card { background-color: var(--bg-card); border-radius: 16px; padding: 20px; border: 1px solid var(--border); box-shadow: 0 4px 6px rgba(0,0,0,0.2); }
    .card-full { grid-column: span 2; }
    .card-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 15px; }
    .card-title { font-size: 1.1rem; font-weight: 600; margin: 0; color: var(--text-primary); }
    .status-on { border-left: 5px solid var(--success); }
    .status-off { border-left: 5px solid var(--danger); }
    .status-text-on { color: var(--success); font-weight: 800; font-size: 1.5rem; }
    .status-text-off { color: var(--danger); font-weight: 800; font-size: 1.5rem; }
    .duration-text { font-size: 1.5rem; font-weight: 700; margin-top: 5px; }
    .duration-sub { color: var(--text-secondary); font-size: 0.85rem; }
    .progress-container { margin-bottom: 10px; }
    .progress-label { display: flex; justify-content: space-between; margin-bottom: 5px; color: var(--text-secondary); font-size: 0.9rem; }
    .form-group { margin-bottom: 12px; }
    .form-group label { display: block; color: var(--text-secondary); margin-bottom: 5px; font-size: 0.85rem; }
    .form-group input { width: 100%; padding: 10px; border-radius: 8px; border: 1px solid var(--border); background-color: var(--bg-body); color: var(--text-primary); box-sizing: border-box; }
    .form-group input:focus { outline: none; border-color: var(--accent); }
    .btn { background-color: var(--accent); color: var(--bg-body); padding: 10px 15px; border-radius: 8px; font-weight: 600; border: none; cursor: pointer; width: 100%; transition: opacity 0.2s; }
    .btn:hover { opacity: 0.9; }
    .btn-secondary { background-color: transparent; border: 1px solid var(--border); color: var(--text-secondary); }
    .btn-danger { background-color: var(--danger); width: auto; padding: 5px 10px; }
    .flex-dates { display: flex; gap: 10px; }
    .flex-dates div { flex: 1; }
    table { width: 100%; border-collapse: collapse; margin-top: 10px; font-size: 0.9rem; }
    th, td { border-bottom: 1px solid var(--border); padding: 10px 8px; text-align: left; }
    th { color: var(--text-secondary); font-weight: 500; background: rgba(255,255,255,0.02); }
    
    /* Виправлено напливання графіків */
    .charts-grid { display: grid; grid-template-columns: 1fr 2.5fr; gap: 20px; min-height: 250px; margin-top: 20px; padding-bottom: 15px; }
    .chart-container { position: relative; height: 250px; width: 100%; }
    
    @media (max-width: 768px) { 
        .dashboard-grid { grid-template-columns: 1fr; } .card-full { grid-column: span 1; } 
        .charts-grid { grid-template-columns: 1fr; min-height: 500px; }
        .chart-container { height: 250px; }
    }
</style>
</head>
<body>
<div class="container">
    <div class="header">
        <h1 class="header-title">Моніторинг мережі живлення</h1>
        <p class="header-subtitle" id="locName">Завантаження...</p>
    </div>
    <div class="dashboard-grid">
        <div class="card card-full" id="cardState">
            <div class="card-header" style="align-items: flex-start; flex-wrap: wrap; border-bottom: none;">
                <div>
                    <h3 class="card-title" style="margin-bottom: 10px;">Поточний стан мережі</h3>
                    <div id="currState" class="status-text-on">Завантаження...</div>
                    <div class="duration-text" id="currDuration">...</div>
                    <div class="duration-sub">Тривалість поточного стану</div>
                </div>
                <div style="text-align: right; min-width: 220px; padding-top: 5px;">
                    <div style="margin-bottom: 8px; font-size: 0.95rem; color: var(--text-secondary);">Доступ до Інтернету: <span id="sysInternet" style="font-weight: bold;">...</span></div>
                    <div style="font-size: 0.95rem; color: var(--text-secondary); border-top: 1px solid var(--border); padding-top: 8px;">Uptime: <b id="sysUptime" style="color: var(--text-primary);">...</b></div>
                </div>
            </div>
        </div>

        <div class="card">
            <div class="card-header"><h3 class="card-title">Системні ресурси</h3></div>
            <div class="progress-container"><div class="progress-label"><span>RAM (ОЗП)</span><span id="ramUsage">...</span></div></div>
            <div class="progress-container" style="margin-top: 15px;"><div class="progress-label"><span>Flash (Пам'ять)</span><span id="fsUsage">...</span></div></div>
            <div class="progress-container" style="margin-top: 15px;"><div class="progress-label"><span>CPU</span><span id="cpuLoad">...</span></div></div>
            <div class="progress-container" style="margin-top: 15px; border-top: 1px solid var(--border); padding-top: 10px;">
                <div class="progress-label"><span>Wi-Fi: <b id="wifiSsid" style="color: var(--accent);">...</b></span><span id="wifiSignal">...</span></div>
            </div>
        </div>

        <div class="card">
            <div class="card-header"><h3 class="card-title">Налаштування</h3></div>
            <form action="/save" method="POST">
                <div class="form-group"><label>Назва об'єкта:</label><input type="text" name="location" value=")rawliteral" + String(config.locationName) + R"rawliteral("></div>
                <div class="form-group"><label>Telegram Bot Token:</label><input type="text" name="token" value=")rawliteral" + String(config.botToken) + R"rawliteral("></div>
                <div class="form-group"><label>Chat IDs (через кому):</label><input type="text" name="chats" value=")rawliteral" + String(config.chatIds) + R"rawliteral("></div>
                <button type="submit" class="btn" style="margin-bottom: 10px;">Зберегти</button>
            </form>
            <button type="button" onclick="location.href='/wifi'" class="btn btn-secondary">Змінити Wi-Fi</button>
        </div>

        <div class="card card-full">
            <div class="card-header"><h3 class="card-title">Звіт та графік доступності</h3></div>
            <div class="flex-dates">
                <div class="form-group"><label>Від</label><input type="date" id="dateStart" onchange="updateReport()"></div>
                <div class="form-group"><label>До</label><input type="date" id="dateEnd" onchange="updateReport()"></div>
            </div>
            
            <div id="reportRes" style="margin-top: 15px; font-size: 1.05rem; padding-bottom: 15px; border-bottom: 1px solid var(--border);"></div>
            
            <div class="charts-grid">
                <div class="chart-container"><canvas id="slaChart"></canvas></div>
                <div class="chart-container"><canvas id="tlChart"></canvas></div>
            </div>
        </div>

        <div class="card card-full">
            <div class="card-header"><h3 class="card-title">Історія подій (останні 100)</h3><button class="btn btn-danger" onclick="clearHistory()">Очистити</button></div>
            <div style="max-height: 250px; overflow-y: auto;">
                <table><thead><tr><th>Дата</th><th>Час</th><th>Стан</th><th>Тривалість</th></tr></thead><tbody id="logTable"></tbody></table>
            </div>
        </div>
    </div>
</div>
<script>
    let slaChart = null, tlChart = null;
    let isFirstLoad = true;

    function loadData() {
        fetch('/api/data?t=' + new Date().getTime()).then(r => r.json()).then(d => {
            document.getElementById('locName').innerText = d.location;
            
            // Автоматичне встановлення дати з першого запису історії
            if (isFirstLoad && d.firstLogTime) {
                let firstDate = new Date(d.firstLogTime * 1000);
                if (firstDate.getFullYear() < 2020) firstDate = new Date(); // Захист від 1970
                const off = firstDate.getTimezoneOffset() * 60000;
                document.getElementById('dateStart').value = new Date(firstDate.getTime() - off).toISOString().split('T')[0];
                document.getElementById('dateEnd').value = new Date(new Date().getTime() - off).toISOString().split('T')[0];
                isFirstLoad = false;
            }

            let st = document.getElementById('currState'), cd = document.getElementById('cardState');
            if(d.currentState) { st.className="status-text-on"; st.innerText="🟢 СВІТЛО Є"; cd.className="card card-full status-on"; }
            else { st.className="status-text-off"; st.innerText="🔴 СВІТЛА НЕМАЄ"; cd.className="card card-full status-off"; }
            document.getElementById('currDuration').innerText = d.currentDuration;
            document.getElementById('ramUsage').innerText = d.ramUsage;
            document.getElementById('fsUsage').innerText = d.fsUsage;
            document.getElementById('cpuLoad').innerText = d.cpuLoad;
            if (d.wifiSsid) { document.getElementById('wifiSsid').innerText = d.wifiSsid; document.getElementById('wifiSignal').innerText = d.wifiSignal; }
            if (d.uptime !== undefined) document.getElementById('sysUptime').innerText = d.uptime;
            if (d.internet !== undefined) {
                let intEl = document.getElementById('sysInternet');
                if (d.internet) { intEl.innerText = "🌐 Є зв'язок"; intEl.style.color = "var(--success)"; }
                else { intEl.innerText = "❌ Відсутній"; intEl.style.color = "var(--danger)"; }
            }
            let th = '';
            d.logs.forEach(l => {
                let dt = l.t.split(' ');
                let bc = !l.s ? 'color: var(--success);' : 'color: var(--danger);';
                let sn = !l.s ? '🟢 Було світло' : '🔴 Не було світла';
                th += `<tr><td>${dt[0]||''}</td><td>${dt[1]||''}</td><td style="${bc} font-weight:bold;">${sn}</td><td>${l.d}</td></tr>`;
            });
            document.getElementById('logTable').innerHTML = th; 
            if (!isFirstLoad) updateReport();
        });
    }

    function updateReport() {
        let ds = document.getElementById('dateStart').value, de = document.getElementById('dateEnd').value;
        if (!ds || !de) return;
        let ts = Math.floor(new Date(ds + "T00:00:00").getTime() / 1000), te = Math.floor(new Date(de + "T23:59:59").getTime() / 1000);
        
        fetch(`/api/report?start=${ts}&end=${te}`).then(r => r.json()).then(r => {
            let total = r.on_sec + r.off_sec;
            let onPct = total > 0 ? ((r.on_sec / total) * 100).toFixed(1) : 0;
            let offPct = total > 0 ? ((r.off_sec / total) * 100).toFixed(1) : 0;
            
            document.getElementById('reportRes').innerHTML = `
                <p style="margin:5px 0;">🟢 Зі світлом: <b style="color:var(--text-primary);">${r.on}</b> <span style="color:var(--success); font-weight:bold; margin-left:10px;">${onPct}% часу</span></p>
                <p style="margin:5px 0;">🔴 Без світла: <b style="color:var(--text-primary);">${r.off}</b> <span style="color:var(--danger); font-weight:bold; margin-left:10px;">${offPct}% часу</span></p>
            `;

            renderCharts(onPct, offPct, r.events, ds, de);
        });
    }

    function renderCharts(onPct, offPct, events, dateStartStr, dateEndStr) {
        // Оновлення кольцевого графіка без перезавантаження
        if (!slaChart) {
            slaChart = new Chart(document.getElementById('slaChart').getContext('2d'), {
                type: 'doughnut',
                data: { labels: ['Є світло (%)', 'Немає світла (%)'], datasets: [{ data: [onPct, offPct], backgroundColor: ['#4ADE80', '#FB7185'], borderWidth: 0 }] },
                options: { responsive: true, maintainAspectRatio: false, plugins: { legend: { position: 'bottom', labels:{color:'#F1F5F9'} } } }
            });
        } else {
            slaChart.data.datasets[0].data = [onPct, offPct];
            slaChart.update('none'); // Оновлення без анімації
        }

        let dataOn = [], dataOff = [];
        events.forEach(ev => {
            // Фільтрація подій 1970 року
            if (ev.start > 1000000000) { 
                let obj = { x: [ev.start * 1000, ev.end * 1000], y: 'Мережа' };
                if (ev.state) dataOn.push(obj); else dataOff.push(obj);
            }
        });

        // Жорстке обмеження осі X за обраними датами
        let minTime = new Date(dateStartStr + "T00:00:00").getTime();
        let maxTime = new Date(dateEndStr + "T23:59:59").getTime();

        if (!tlChart) {
            tlChart = new Chart(document.getElementById('tlChart').getContext('2d'), {
                type: 'bar',
                data: { datasets: [
                    { label: 'Є світло', data: dataOn, backgroundColor: '#4ADE80', barPercentage: 0.9 },
                    { label: 'Немає світла', data: dataOff, backgroundColor: '#FB7185', barPercentage: 0.9 }
                ]},
                options: {
                    indexAxis: 'y', responsive: true, maintainAspectRatio: false,
                    scales: {
                        x: { type: 'time', min: minTime, max: maxTime, time: { displayFormats: { hour: 'dd.MM HH:mm' } }, ticks: { color: '#94A3B8' }, grid: { color: '#334155' } },
                        y: { stacked: true, ticks: { color: '#F1F5F9' }, grid: { display: false } }
                    },
                    plugins: { legend: { display: false }, tooltip: { callbacks: { label: function(c) { return c.dataset.label; } } } }
                }
            });
        } else {
            tlChart.data.datasets[0].data = dataOn;
            tlChart.data.datasets[1].data = dataOff;
            tlChart.options.scales.x.min = minTime;
            tlChart.options.scales.x.max = maxTime;
            tlChart.update('none');
        }
    }

    function clearHistory() { 
        if (confirm("Очистити історію?")) {
            fetch('/api/clear', { method: 'POST' }).then(() => {
                isFirstLoad = true;
                if(tlChart) { tlChart.destroy(); tlChart = null; }
                if(slaChart) { slaChart.destroy(); slaChart = null; }
                loadData();
            });
        } 
    }
    
    setInterval(loadData, 2000); 
    loadData();
</script>
</body>
</html>
  )rawliteral";
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "text/html", html);
}

void handleApiClear() {
  logCount = 0;
  saveLogs();
  server.send(200, "text/plain", "OK");
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
  DynamicJsonDocument doc(12288); 
  doc["location"] = config.locationName;
  doc["currentState"] = currentState;
  doc["currentDuration"] = formatDuration((uint32_t)(time(NULL) - lastStateChangeTime));

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
    obj["t"] = formatTime(logs[i].timestamp);
    obj["s"] = logs[i].state;
    obj["d"] = formatDuration(logs[i].duration);
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
  
  if (currOverlapStart < currEnd) {
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

// ================= SETUP ТА MAIN LOOP =================
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

  syncNTP();
  unsigned long startNtp = millis();
  while (time(NULL) < 1000000000 && millis() - startNtp < 10000) delay(500);

  isInternetConnected = checkInternet();
  lastPingTime = millis();
  currentState = (digitalRead(SENSOR_PIN) == LOW);
  lastStateChangeTime = time(NULL);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/api/data", handleApiData);
  server.on("/api/report", handleApiReport);
  server.on("/api/clear", HTTP_POST, handleApiClear);
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

void loop() {
  server.handleClient();

  if (millis() - lastPingTime > 15000) {
    isInternetConnected = checkInternet();
    lastPingTime = millis();
  }

  bool newRead = (digitalRead(SENSOR_PIN) == LOW);
  if (newRead != currentState) {
    time_t now = time(NULL);
    uint32_t duration = (uint32_t)(now - lastStateChangeTime);

    if (logCount >= MAX_LOGS) {
      memmove(&logs[0], &logs[1], sizeof(LogEntry) * (MAX_LOGS - 1));
      logCount = MAX_LOGS - 1;
    }

    logs[logCount].timestamp = now;
    logs[logCount].state = newRead;
    logs[logCount].duration = duration;
    logs[logCount].sent = false; 
    logCount++;

    currentState = newRead;
    lastStateChangeTime = now;

    saveLogs();
  }

  if (millis() - lastTelegramCheck > TELEGRAM_INTERVAL) {
    processUnsentMessages();
    handleTelegramCommands();
    lastTelegramCheck = millis();
  }

  static int lastSyncDay = -1;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    if (timeinfo.tm_wday == 0 && timeinfo.tm_hour == 2 && timeinfo.tm_min == 0 && lastSyncDay != timeinfo.tm_yday) {
      syncNTP(); lastSyncDay = timeinfo.tm_yday; delay(1000); 
    }
  }
}
// ============================
// Lilygo t display s3 long as BMS Monitor
// ============================

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "AXS15231B.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// WiFi настройки
const char* ssid = "YouSSID";
const char* password = "PassWord";

// MQTT настройки
const char* mqtt_server = "IP_Local_MQTT_Broker";
const int mqtt_port = 1883;
const char* mqtt_user = "User";
const char* mqtt_password = "PassWord";
const char* mqtt_topic_status = "bms/status";
const char* mqtt_topic_cells = "bms/cells";
const char* mqtt_topic_system = "bms/system";

// UUID для JBD/Xiaoxiang BMS
static BLEUUID serviceUUID("0000ff00-0000-1000-8000-00805f9b34fb");
static BLEUUID charTxUUID("0000ff02-0000-1000-8000-00805f9b34fb");
static BLEUUID charRxUUID("0000ff01-0000-1000-8000-00805f9b34fb");

// Создаём объекты
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite sprite = TFT_eSprite(&tft);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// BLE переменные
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pTxCharacteristic = nullptr;
BLERemoteCharacteristic* pRxCharacteristic = nullptr;
bool bleConnected = false;
bool doConnect = false;
BLEAdvertisedDevice* myDevice = nullptr;


AsyncWebServer server(80); 

// История данных (для графиков)
#define HISTORY_SIZE 60
float voltageHistory[HISTORY_SIZE] = { 0 };
float currentHistory[HISTORY_SIZE] = { 0 };
int historyIndex = 0;
unsigned long lastHistoryUpdate = 0;

// Данные BMS
float totalVoltage = 0.0;
float current = 0.0;
int soc = 0;
float cellVoltages[16] = { 0 };
int cellCount = 4;
float temp1 = 0.0;
float temp2 = 0.0;
uint32_t cycles = 0;
uint8_t rxBuffer[256];
uint8_t rxIndex = 0;
bool dataReady = false;

// Статусы подключений
bool wifiConnected = false;
bool mqttConnected = false;
int rssi = 0;
String ipAddress = "";

unsigned long lastMqttPublish = 0;
unsigned long lastBMSRequest = 0;
unsigned long lastWiFiCheck = 0;
bool requestBasic = true;

// ============================
// WIFI ПОДКЛЮЧЕНИЕ
// ============================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    ipAddress = WiFi.localIP().toString();
    rssi = WiFi.RSSI();
    Serial.println("\nWiFi connected!");
    Serial.print("IP: ");
    Serial.println(ipAddress);
    Serial.print("RSSI: ");
    Serial.println(rssi);
  } else {
    wifiConnected = false;
    Serial.println("\nWiFi connection failed!");
  }
}

// ============================
// MQTT ПОДКЛЮЧЕНИЕ
// ============================
void connectMQTT() {
  if (!wifiConnected) return;
  if (mqttClient.connected()) return;

  Serial.print("Connecting to MQTT broker: ");
  Serial.println(mqtt_server);

  String clientId = "BMS_Monitor_" + String(random(0xffff), HEX);

  if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
    mqttConnected = true;
    Serial.println("MQTT connected!");
    mqttClient.publish(mqtt_topic_system, "{\"status\":\"online\"}");
  } else {
    mqttConnected = false;
    Serial.print("MQTT connection failed, rc=");
    Serial.println(mqttClient.state());
  }
}

// ============================
// ОТПРАВКА ДАННЫХ В MQTT
// ============================
void publishMQTT() {
  if (!mqttConnected) return;

  StaticJsonDocument<512> docStatus;
  docStatus["voltage"] = totalVoltage;
  docStatus["current"] = current;
  docStatus["soc"] = soc;
  docStatus["temp1"] = temp1;
  docStatus["temp2"] = temp2;
  docStatus["cycles"] = cycles;
  docStatus["power"] = totalVoltage * current;
  docStatus["rssi"] = rssi;

  char bufferStatus[512];
  serializeJson(docStatus, bufferStatus);
  mqttClient.publish(mqtt_topic_status, bufferStatus, true);

  StaticJsonDocument<512> docCells;
  JsonArray cells = docCells.createNestedArray("cells");
  for (int i = 0; i < cellCount; i++) {
    cells.add(cellVoltages[i]);
  }
  docCells["count"] = cellCount;

  float minCell = cellVoltages[0];
  float maxCell = cellVoltages[0];
  for (int i = 1; i < cellCount; i++) {
    if (cellVoltages[i] < minCell) minCell = cellVoltages[i];
    if (cellVoltages[i] > maxCell) maxCell = cellVoltages[i];
  }
  docCells["min"] = minCell;
  docCells["max"] = maxCell;
  docCells["delta"] = maxCell - minCell;

  char bufferCells[512];
  serializeJson(docCells, bufferCells);
  mqttClient.publish(mqtt_topic_cells, bufferCells, true);

  Serial.println("MQTT published");
}

// ============================
// BLE CALLBACKS
// ============================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    Serial.println("BLE Connected!");
    bleConnected = true;
  }

  void onDisconnect(BLEClient* pclient) {
    Serial.println("BLE Disconnected!");
    bleConnected = false;
  }
};

static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic,
                           uint8_t* pData, size_t length, bool isNotify) {
  for (int i = 0; i < length && rxIndex < 255; i++) {
    rxBuffer[rxIndex++] = pData[i];
  }

  if (rxIndex >= 4 && rxBuffer[0] == 0xDD) {
    uint8_t dataLen = rxBuffer[3];
    uint8_t totalLen = dataLen + 7;

    if (rxIndex >= totalLen) {
      dataReady = true;
    }
  }
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID)) {
      Serial.println("Found BMS device!");
      Serial.print("Address: ");
      Serial.println(advertisedDevice.getAddress().toString().c_str());

      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ============================
// ПОДКЛЮЧЕНИЕ К BMS
// ============================
bool connectToBMS() {
  Serial.println("Connecting to BMS...");

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(myDevice)) {
    Serial.println("Failed to connect");
    return false;
  }

  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    Serial.println("Failed to find service");
    pClient->disconnect();
    return false;
  }

  pTxCharacteristic = pRemoteService->getCharacteristic(charTxUUID);
  pRxCharacteristic = pRemoteService->getCharacteristic(charRxUUID);

  if (pTxCharacteristic == nullptr || pRxCharacteristic == nullptr) {
    Serial.println("Failed to find characteristics");
    pClient->disconnect();
    return false;
  }

  if (pRxCharacteristic->canNotify()) {
    pRxCharacteristic->registerForNotify(notifyCallback);
  }

  Serial.println("BLE Connected successfully!");
  return true;
}

// ============================
// ОТПРАВКА КОМАНДЫ BMS
// ============================
void sendBMSCommand(uint8_t cmd) {
  if (!bleConnected || pTxCharacteristic == nullptr) return;

  uint8_t command[7];
  command[0] = 0xDD;
  command[1] = 0xA5;
  command[2] = cmd;
  command[3] = 0x00;
  command[4] = 0xFF;
  command[5] = 0xFD - cmd + 0x03;
  command[6] = 0x77;

  rxIndex = 0;
  dataReady = false;
  pTxCharacteristic->writeValue(command, 7);
}

// ============================
// ПАРСИНГ ОСНОВНЫХ ДАННЫХ
// ============================
void parseBasicInfo() {
  if (!dataReady || rxBuffer[0] != 0xDD || rxBuffer[1] != 0x03) return;

  totalVoltage = ((rxBuffer[4] << 8) | rxBuffer[5]) * 0.01;
  int16_t currentRaw = (rxBuffer[6] << 8) | rxBuffer[7];
  current = currentRaw * 0.01;
  soc = rxBuffer[23];

  if (rxBuffer[3] >= 24) {
    int16_t temp1Raw = (rxBuffer[27] << 8) | rxBuffer[28];
    temp1 = (temp1Raw - 2731) * 0.1;
    int16_t temp2Raw = (rxBuffer[29] << 8) | rxBuffer[30];
    temp2 = (temp2Raw - 2731) * 0.1;
  }

  cycles = (rxBuffer[12] << 8) | rxBuffer[13];

  Serial.printf("V:%.2f I:%.2fA SOC:%d%% T1:%.1f T2:%.1f\n",
                totalVoltage, current, soc, temp1, temp2);
}

// ============================
// ПАРСИНГ НАПРЯЖЕНИЙ ЯЧЕЕК
// ============================
void parseCellVoltages() {
  if (!dataReady || rxBuffer[0] != 0xDD || rxBuffer[1] != 0x04) return;

  uint8_t dataLen = rxBuffer[3];
  cellCount = dataLen / 2;

  for (int i = 0; i < cellCount && i < 16; i++) {
    cellVoltages[i] = ((rxBuffer[4 + i * 2] << 8) | rxBuffer[5 + i * 2]) * 0.001;
  }
}

// ============================
// ОТРИСОВКА ДИСПЛЕЯ
// ============================
void drawBMSMonitor() {
  sprite.fillSprite(TFT_BLACK);
  sprite.drawRoundRect(0, 0, 640, 180, 8, TFT_CYAN);
  sprite.drawRoundRect(1, 1, 638, 178, 7, TFT_CYAN);

  int marginLeft = 12;

  sprite.setFreeFont(&FreeSansBold12pt7b);
  sprite.setTextColor(TFT_WHITE);
  sprite.setTextDatum(TL_DATUM);
  sprite.drawString("BMS MONITOR", marginLeft + 8, 8);

  sprite.setFreeFont(&FreeSansBold24pt7b);
  sprite.setTextColor(TFT_CYAN);
  char voltBuf[10];
  sprintf(voltBuf, "%.2fV", totalVoltage);
  sprite.drawString(voltBuf, marginLeft + 15, 40);

  sprite.setFreeFont(&FreeSansBold24pt7b);
  sprite.setTextColor(TFT_YELLOW);
  char currBuf[10];
  sprintf(currBuf, "%.1fA", current);
  sprite.drawString(currBuf, marginLeft + 15, 85);

  sprite.setFreeFont(&FreeSans9pt7b);
  if (current < 0) {
    sprite.setTextColor(TFT_ORANGE);
    sprite.drawString("Discharge", marginLeft + 15, 125);
  } else {
    sprite.setTextColor(TFT_GREEN);
    sprite.drawString("Charge", marginLeft + 15, 125);
  }

  int battX = 240;
  int battY = 28;
  int battW = 155;
  int battH = 65;

  sprite.drawRoundRect(battX, battY, battW, battH, 6, TFT_WHITE);
  sprite.fillRect(battX + battW, battY + 20, 8, 25, TFT_WHITE);

  int fillW = (battW - 6) * soc / 100;
  uint16_t battColor;
  if (soc < 20) battColor = TFT_RED;
  else if (soc < 50) battColor = TFT_ORANGE;
  else battColor = TFT_GREEN;

  sprite.fillRoundRect(battX + 3, battY + 3, fillW, battH - 6, 4, battColor);

  sprite.setFreeFont(&FreeSansBold24pt7b);
  sprite.setTextColor(battColor);
  sprite.setTextDatum(TC_DATUM);
  char socBuf[6];
  sprintf(socBuf, "%d%%", soc);
  sprite.drawString(socBuf, 317, 103);

  int cellStartX = 455;
  int cellStartY = 25;
  int cellW = 80;
  int cellH = 55;
  int cellGapX = 90;
  int cellGapY = 65;

  int cellPositions[4][2] = {
    { cellStartX, cellStartY },
    { cellStartX + cellGapX, cellStartY },
    { cellStartX, cellStartY + cellGapY },
    { cellStartX + cellGapX, cellStartY + cellGapY }
  };

  uint16_t cellColors[] = { TFT_ORANGE, TFT_CYAN, TFT_MAGENTA, TFT_YELLOW };
  const char* cellLabels[] = { "C1", "C2", "C3", "C4" };

  for (int i = 0; i < 4 && i < cellCount; i++) {
    int x = cellPositions[i][0];
    int y = cellPositions[i][1];

    sprite.fillRoundRect(x, y, cellW, cellH, 5, 0x2104);
    sprite.drawRoundRect(x, y, cellW, cellH, 5, cellColors[i]);

    sprite.setFreeFont(&FreeSansBold12pt7b);
    sprite.setTextColor(cellColors[i]);
    sprite.setTextDatum(TC_DATUM);
    sprite.drawString(cellLabels[i], x + cellW / 2, y + 8);

    sprite.setFreeFont(&FreeMonoBold12pt7b);
    sprite.setTextColor(TFT_WHITE);
    char cellBuf[8];
    sprintf(cellBuf, "%.2fV", cellVoltages[i]);
    sprite.drawString(cellBuf, x + cellW / 2, y + 32);
  }

  int statY = 158;
  sprite.setFreeFont(&FreeSans9pt7b);
  sprite.setTextDatum(TL_DATUM);

  if (wifiConnected) {
    sprite.setTextColor(TFT_GREEN);
    sprite.drawString("WiFi", marginLeft + 5, statY);
    sprite.setTextColor(0x7BEF);
    sprite.drawString(ipAddress, marginLeft + 50, statY);
  } else {
    sprite.setTextColor(TFT_RED);
    sprite.drawString("WiFi: OFF", marginLeft + 5, statY);
  }

  if (mqttConnected) {
    sprite.setTextColor(TFT_MAGENTA);
    sprite.drawString("MQTT", 205, statY);
    sprite.setTextColor(TFT_GREEN);
    sprite.drawString("Connected", 261, statY);
  } else {
    sprite.setTextColor(TFT_RED);
    sprite.drawString("MQTT: OFF", 205, statY);
  }

  if (bleConnected) {
    sprite.setTextColor(TFT_BLUE);
    sprite.drawString("BLE", 385, statY);
    sprite.setTextColor(TFT_GREEN);
    sprite.drawString("Paired", 425, statY);
  } else {
    sprite.setTextColor(0x7BEF);
    sprite.drawString("BLE: OFF", 385, statY);
  }

  sprite.setTextDatum(TR_DATUM);
  sprite.setTextColor(TFT_WHITE);
  char rssiBuf[15];
  sprintf(rssiBuf, "%d dBm", rssi);
  sprite.drawString(rssiBuf, 625, statY);

  lcd_PushColors_rotated_90(0, 0, 640, 180, (uint16_t*)sprite.getPointer());
}

// ============================
// 🌐 WEB ИНТЕРФЕЙС 
// ============================
const char HTML_PAGE[] PROGMEM = R"=====(<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>BMS Monitor</title><style>*{margin:0;padding:0;box-sizing:border-box}body{font-family:'Segoe UI',sans-serif;background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;min-height:100vh;padding:10px}.container{max-width:1400px;margin:0 auto}h1{text-align:center;margin:20px 0;font-size:2.5em}.status-bar{display:flex;justify-content:center;gap:15px;margin-bottom:20px;flex-wrap:wrap}.status-item{background:rgba(255,255,255,.15);backdrop-filter:blur(10px);border-radius:12px;padding:12px 20px;display:flex;align-items:center;gap:8px}.status-dot{width:10px;height:10px;border-radius:50%;animation:pulse 2s infinite}@keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}.online{background:#10b981}.offline{background:#ef4444}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:20px;margin-bottom:20px}.card{background:rgba(255,255,255,.12);backdrop-filter:blur(15px);border-radius:16px;padding:20px;box-shadow:0 8px 32px rgba(0,0,0,.3)}.card h2{font-size:1.1em;margin-bottom:12px;opacity:.9}.big-value{font-size:2.8em;font-weight:bold;margin:8px 0}.voltage{color:#60a5fa}.current{color:#fbbf24}.battery{width:100%;height:70px;border:3px solid #fff;border-radius:12px;position:relative;overflow:hidden}.battery-fill{height:100%;background:linear-gradient(90deg,#10b981,#059669);transition:width .5s;display:flex;align-items:center;justify-content:center;font-size:1.8em;font-weight:bold}.battery-tip{width:15px;height:35px;background:#fff;position:absolute;right:-18px;top:17.5px;border-radius:0 6px 6px 0}.cells{display:grid;grid-template-columns:repeat(2,1fr);gap:12px}.cell{background:rgba(255,255,255,.1);padding:12px;border-radius:10px;text-align:center}.cell-value{font-size:1.6em;font-weight:bold;margin-top:4px}.info-row{display:flex;justify-content:space-between;padding:10px 0;border-bottom:1px solid rgba(255,255,255,.15)}.info-row:last-child{border:none}.chart-container{height:200px}</style></head><body><div class="container"><h1>⚡ BMS Monitor</h1><div class="status-bar"><div class="status-item"><div class="status-dot" id="b"></div><span>BLE</span></div><div class="status-item"><div class="status-dot" id="w"></div><span>WiFi</span></div><div class="status-item"><div class="status-dot" id="m"></div><span>MQTT</span></div><div class="status-item"><span id="ip">⏳</span></div></div><div class="grid"><div class="card"><h2>⚡ Напряжение</h2><div class="big-value voltage" id="v">0.00 В</div></div><div class="card"><h2>🔋 Ток</h2><div class="big-value current" id="i">0.00 А</div><div id="s">—</div></div><div class="card"><h2>📊 Заряд</h2><div class="battery"><div class="battery-fill" id="soc">0%</div><div class="battery-tip"></div></div></div></div><div class="grid"><div class="card"><h2>🔌 Ячейки</h2><div class="cells"><div class="cell"><div>Ячейка 1</div><div class="cell-value" id="c1">0.000</div></div><div class="cell"><div>Ячейка 2</div><div class="cell-value" id="c2">0.000</div></div><div class="cell"><div>Ячейка 3</div><div class="cell-value" id="c3">0.000</div></div><div class="cell"><div>Ячейка 4</div><div class="cell-value" id="c4">0.000</div></div></div></div><div class="card"><h2>📋 Информация</h2><div class="info-row"><span>🌡️ T1:</span><span id="t1">—</span></div><div class="info-row"><span>🌡️ T2:</span><span id="t2">—</span></div><div class="info-row"><span>⚡ Мощность:</span><span id="p">—</span></div><div class="info-row"><span>🔄 Циклы:</span><span id="c">—</span></div><div class="info-row"><span>📡 RSSI:</span><span id="r">—</span></div></div></div><div class="card"><h2>📈 Напряжение</h2><div class="chart-container"><canvas id="vc"></canvas></div></div><div class="card"><h2>📉 Ток</h2><div class="chart-container"><canvas id="ic"></canvas></div></div></div><script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0"></script><script>let vC,iC;const o={responsive:1,maintainAspectRatio:0,plugins:{legend:{display:0}},scales:{x:{display:0},y:{ticks:{color:'#fff'},grid:{color:'rgba(255,255,255,0.1)'}}}};vC=new Chart(document.getElementById('vc'),{type:'line',data:{labels:Array(60).fill(''),datasets:[{data:Array(60).fill(0),borderColor:'#60a5fa',backgroundColor:'rgba(96,165,250,0.15)',tension:0.4,fill:1,pointRadius:0}]},options:o});iC=new Chart(document.getElementById('ic'),{type:'line',data:{labels:Array(60).fill(''),datasets:[{data:Array(60).fill(0),borderColor:'#fbbf24',backgroundColor:'rgba(251,191,36,0.15)',tension:0.4,fill:1,pointRadius:0}]},options:o});function upd(){fetch('/api/data').then(r=>r.json()).then(d=>{document.getElementById('v').textContent=d.voltage.toFixed(2)+' В';document.getElementById('i').textContent=d.current.toFixed(2)+' А';document.getElementById('s').textContent=d.current<0?'🔻 Разряд':'🔺 Заряд';document.getElementById('soc').style.width=d.soc+'%';document.getElementById('soc').textContent=d.soc+'%';document.getElementById('c1').textContent=d.cells[0].toFixed(3);document.getElementById('c2').textContent=d.cells[1].toFixed(3);document.getElementById('c3').textContent=d.cells[2].toFixed(3);document.getElementById('c4').textContent=d.cells[3].toFixed(3);document.getElementById('t1').textContent=d.temp1>-200?d.temp1.toFixed(1)+'°C':'—';document.getElementById('t2').textContent=d.temp2>-200?d.temp2.toFixed(1)+'°C':'—';document.getElementById('p').textContent=(d.voltage*Math.abs(d.current)).toFixed(1)+' Вт';document.getElementById('c').textContent=d.cycles;document.getElementById('r').textContent=d.rssi+' dBm';document.getElementById('ip').textContent='🌐 '+d.ip;document.getElementById('b').className='status-dot '+(d.bleConnected?'online':'offline');document.getElementById('w').className='status-dot '+(d.wifiConnected?'online':'offline');document.getElementById('m').className='status-dot '+(d.mqttConnected?'online':'offline');if(d.voltageHistory){vC.data.datasets[0].data=d.voltageHistory;vC.update('none')}if(d.currentHistory){iC.data.datasets[0].data=d.currentHistory;iC.update('none')}});}upd();setInterval(upd,1000);</script></body></html>)=====";

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send_P(200, "text/html", HTML_PAGE);
  });
  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest* request) {
    StaticJsonDocument<1024> doc;
    doc["voltage"] = totalVoltage;
    doc["current"] = current;
    doc["soc"] = soc;
    doc["temp1"] = temp1;
    doc["temp2"] = temp2;
    doc["cycles"] = cycles;
    doc["rssi"] = rssi;
    doc["ip"] = ipAddress;
    doc["bleConnected"] = bleConnected;
    doc["wifiConnected"] = wifiConnected;
    doc["mqttConnected"] = mqttConnected;
    JsonArray cells = doc.createNestedArray("cells");
    for (int i = 0; i < cellCount; i++) cells.add(cellVoltages[i]);
    JsonArray vHist = doc.createNestedArray("voltageHistory");
    for (int i = 0; i < HISTORY_SIZE; i++) vHist.add(voltageHistory[i]);
    JsonArray cHist = doc.createNestedArray("currentHistory");
    for (int i = 0; i < HISTORY_SIZE; i++) cHist.add(currentHistory[i]);
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });
  server.begin();
  Serial.println("Web: http://" + ipAddress);
}

void updateHistory() {
  voltageHistory[historyIndex] = totalVoltage;
  currentHistory[historyIndex] = current;
  historyIndex = (historyIndex + 1) % HISTORY_SIZE;
}

// ============================
// SETUP 
// ============================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\nBMS Monitor Starting...");

  // ========== 1. ДИСПЛЕЙ САМЫМ ПЕРВЫМ! ==========
  Serial.println("Step 1: Display init");
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);

  delay(100);

  axs15231_init();
  sprite.createSprite(640, 180);
  sprite.setSwapBytes(1);
  lcd_fill(0, 0, 180, 640, 0x00);

  digitalWrite(TFT_BL, HIGH);
  Serial.println("Display OK!");

  delay(500);

  // ========== 2. BLE ВТОРЫМ ==========
  Serial.println("Step 2: BLE init");
  BLEDevice::init("");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(10, false);
  Serial.println("BLE scanning...");

  delay(500);

  // ========== 3. WiFi третьим ==========
  Serial.println("Step 3: WiFi init");
  connectWiFi();

  // ========== 4. MQTT последним ==========
  Serial.println("Step 4: MQTT setup");
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setBufferSize(512);
  connectMQTT();

  Serial.println("Setup complete!");

  setupWebServer();  // ← Добавить
}

// ============================
// LOOP
// ============================
void loop() {
  unsigned long now = millis();

  if (now - lastWiFiCheck > 10000) {
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      connectWiFi();
    } else {
      wifiConnected = true;
      rssi = WiFi.RSSI();
    }
    lastWiFiCheck = now;
  }

  if (wifiConnected && !mqttClient.connected()) {
    mqttConnected = false;
    connectMQTT();
  }

  if (mqttClient.connected()) {
    mqttClient.loop();
  }

  if (doConnect) {
    if (connectToBMS()) {
      bleConnected = true;
    }
    doConnect = false;
  }

  if (bleConnected && now - lastBMSRequest > 2000) {
    if (requestBasic) {
      sendBMSCommand(0x03);
      delay(200);
      parseBasicInfo();
      requestBasic = false;
    } else {
      sendBMSCommand(0x04);
      delay(200);
      parseCellVoltages();
      requestBasic = true;
    }
    lastBMSRequest = now;
  }

  if (mqttConnected && bleConnected && now - lastMqttPublish > 5000) {
    publishMQTT();
    lastMqttPublish = now;
  }

  if (!bleConnected && myDevice != nullptr && now > 20000) {
    Serial.println("BLE reconnecting...");
    BLEDevice::getScan()->start(5, false);
    delay(5000);
  }

  // История для графиков
  if (now - lastHistoryUpdate > 1000) {
    updateHistory();
    lastHistoryUpdate = now;
  }

  drawBMSMonitor();
  delay(500);
}  

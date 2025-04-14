#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <DHTesp.h>

// Константы
#define AP_SSID "ESP_Config"
#define AP_PASS "12345678"
#define EEPROM_SIZE 512
#define DEFAULT_PORT "3000"
#define DHT_PIN 4      // Пин к которому подключен датчик DHT
#define DHT_TYPE DHTesp::DHT11  // Тип датчика (DHT11, DHT21, DHT22) 

// Структура для хранения настроек
struct Config {
  char wifiSSID[32];
  char wifiPass[64];
  char sensorId[32];
  char serverIP[40];
  char serverPort[6];  // Поле для порта (до 5 символов)
};

Config config;
ESP8266WebServer server(80);
WiFiClient wifiClient;
bool wifiConfigured = false;
bool wifiConnected = false;
String lastError = "";
DHTesp dht;
unsigned long LRT = 0;
const unsigned long readingInterval = 2000; // Интервал отправки данных (30 секунд)


// HTML-страницы
const char wifiConfigPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Настройка Wi-Fi</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; }
    .container { max-width: 500px; margin: auto; }
    input { width: 100%; padding: 10px; margin: 8px 0; box-sizing: border-box; }
    button { background: #4CAF50; color: white; padding: 12px; border: none; width: 100%; }
  </style>
</head>
<body>
  <div class="container">
    <h2>Настройка Wi-Fi</h2>
    <form method="post" action="/saveWifi">
      <input type="text" name="wifiSSID" placeholder="SSID сети" required>
      <input type="password" name="wifiPass" placeholder="Пароль" required>
      <button type="submit">Подключиться</button>
    </form>
  </div>
</body>
</html>
)rawliteral";

const char sensorConfigPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Регистрация датчика</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; }
    .container { max-width: 500px; margin: auto; }
    input, select { width: 100%; padding: 10px; margin: 8px 0; box-sizing: border-box; }
    button { background: #4CAF50; color: white; padding: 12px; border: none; width: 100%; }
    .server-addr { display: flex; gap: 10px; }
    .server-addr input:first-child { flex: 3; }
    .server-addr input:last-child { flex: 1; }
  </style>
</head>
<body>
  <div class="container">
    <h2>Регистрация датчика</h2>
    <form method="post" action="/saveSensor">
      <h3>Сервер</h3>
      <div class="server-addr">
        <input type="text" name="serverIP" placeholder="IP или Домен сервера" required>
        <input type="text" name="serverPort" placeholder="Порт сервера" value="3000" required>
      </div>
      <h3>Аутентификация</h3>
      <input type="text" name="authUser" placeholder="Логин" required>
      <input type="password" name="authPass" placeholder="Пароль" required>
      
      <h3>Данные датчика</h3>
      <input type="text" name="sensorId" placeholder="ID датчика" required>
      <input type="number" name="scaleId" placeholder="ID шкалы" required>
      <input type="text" name="sensorName" placeholder="Название">
      <input type="text" name="locationName" placeholder="Местоположение">
      
      <button type="submit">Зарегистрировать</button>
    </form>
  </div>
</body>
</html>
)rawliteral";


const char successPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Успешно</title>
  <style>
    body { font-family: Arial; text-align: center; margin-top: 50px; }
  </style>
</head>
<body>
  <h1>Настройки сохранены!</h1>
  <p>Датчик готов к работе.</p>
</body>
</html>
)rawliteral";

const char errorPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Ошибка</title>
  <style>
    body { font-family: Arial; text-align: center; margin-top: 50px; }
    .error { color: red; margin: 20px; }
    button { background: #4CAF50; color: white; padding: 10px 20px; border: none; margin-top: 20px; }
  </style>
</head>
<body>
  <h1>Произошла ошибка!</h1>
  <div class="error">%ERROR_MESSAGE%</div>
  <button onclick="window.location.href='/'">Попробовать снова</button>
</body>
</html>
)rawliteral";

void showErrorPage(String message) {
  lastError = message;
  String page = FPSTR(errorPage);
  page.replace("%ERROR_MESSAGE%", message);
  server.send(200, "text/html", page);
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, config);
  EEPROM.end();
  wifiConfigured = (strlen(config.wifiSSID) > 0);
  
  // Установка порта по умолчанию если не задан
  if (strlen(config.serverPort) == 0) {
    strlcpy(config.serverPort, DEFAULT_PORT, sizeof(config.serverPort));
  }
}

void saveConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, config);
  EEPROM.commit();
  EEPROM.end();
  wifiConfigured = true;
}
void connectToWiFi() {
  if (wifiConfigured) {
    WiFi.begin(config.wifiSSID, config.wifiPass);
    Serial.print("Подключение к ");
    Serial.println(config.wifiSSID);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    
    wifiConnected = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected) {
      Serial.println("\nПодключено! IP: " + WiFi.localIP().toString());
    } else {
      Serial.println("\nНе удалось подключиться к Wi-Fi");
    }
  }
}

void registerSensor(String serverIP, String serverPort, String authUser, String authPass, 
                   String sensorId, int scaleId, String sensorName, String locationName) {
  if (WiFi.status() != WL_CONNECTED) {
    showErrorPage("Нет подключения к Wi-Fi");
    return;
  }

  // Сохраняем конфигурацию
  strlcpy(config.serverIP, serverIP.c_str(), sizeof(config.serverIP));
  strlcpy(config.serverPort, serverPort.c_str(), sizeof(config.serverPort));
  saveConfig();

  // Формируем URL с портом
  String authUrl = "http://" + serverIP + ":" + serverPort + "/auth/login";
  String sensorUrl = "http://" + serverIP + ":" + serverPort + "/sensors";

  // Авторизация
  HTTPClient authClient;
  authClient.begin(wifiClient, authUrl);
  authClient.addHeader("Content-Type", "application/json");
  
  DynamicJsonDocument authDoc(256);
  authDoc["username"] = authUser;
  authDoc["password"] = authPass;
  
  String authPayload;
  serializeJson(authDoc, authPayload);
  
  int authCode = authClient.POST(authPayload);
  
  if (authCode != 201) {
    String response = authCode > 0 ? authClient.getString() : "";
    authClient.end();
    showErrorPage("Ошибка авторизации. Код: " + String(authCode) + 
                 (response.length() ? "<br>" + response : ""));
    return;
  }
  
  String response = authClient.getString();
  authClient.end();

  DynamicJsonDocument jsonDoc(512);
  DeserializationError error = deserializeJson(jsonDoc, response);
  if (error) {
    showErrorPage("Ошибка формата ответа сервера");
    return;
  }
  
  String token = jsonDoc["access_token"].as<String>();
  if (token.length() == 0) {
    showErrorPage("Не удалось получить токен авторизации");
    return;
  }

  // Регистрация датчика
  HTTPClient http;
  http.begin(wifiClient, sensorUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + token);
  
  DynamicJsonDocument doc(512);
  doc["sensorId"] = sensorId;
  doc["scaleId"] = scaleId;
  doc["isActive"] = true;
  doc["name"] = sensorName;
  doc["location"]["name"] = locationName;
  
  String payload;
  serializeJson(doc, payload);
  
  int httpCode = http.POST(payload);
  String sensorResponse = httpCode > 0 ? http.getString() : "";
  http.end();

  if (httpCode == HTTP_CODE_OK || httpCode == 201) {
    server.send(200, "text/html", successPage);
  } else {
    showErrorPage("Ошибка регистрации. Код: " + String(httpCode) + 
                 (sensorResponse.length() ? "<br>" + sensorResponse : ""));
  }
}

void handleRoot() {
  if (!wifiConfigured || !wifiConnected) {
    server.send(200, "text/html", wifiConfigPage);
  } else {
    server.send(200, "text/html", sensorConfigPage);
  }
}

void handleSaveWifi() {
  strlcpy(config.wifiSSID, server.arg("wifiSSID").c_str(), sizeof(config.wifiSSID));
  strlcpy(config.wifiPass, server.arg("wifiPass").c_str(), sizeof(config.wifiPass));
  saveConfig();
  connectToWiFi();

  if (wifiConnected) {
    server.send(200, "text/html", R"rawliteral(
      <!DOCTYPE html>
      <html>
      <head>
        <meta charset="UTF-8">
        <title>Wi-Fi подключен</title>
        <style>
          body { font-family: Arial; text-align: center; margin-top: 50px; }
        </style>
      </head>
      <body>
        <h1>Wi-Fi подключен!</h1>
        <p>Переход к регистрации датчика...</p>
        <script>setTimeout(() => window.location = "/", 2000);</script>
      </body>
      </html>
    )rawliteral");
  } else {
    showErrorPage("Не удалось подключиться к Wi-Fi. Проверьте данные и попробуйте снова.");
  }
}

void handleSaveSensor() {
  String serverIP = server.arg("serverIP");
  String serverPort = server.arg("serverPort");
  String authUser = server.arg("authUser");
  String authPass = server.arg("authPass");
  String sensorId = server.arg("sensorId");
  int scaleId = server.arg("scaleId").toInt();
  String sensorName = server.arg("sensorName");
  String locationName = server.arg("locationName");

  // Проверка обязательных полей
  if (serverIP.isEmpty() || serverPort.isEmpty() || authUser.isEmpty() || 
      authPass.isEmpty() || sensorId.isEmpty()) {
    showErrorPage("Заполните все обязательные поля");
    return;
  }

  // Сохраняем и регистрируем
  strlcpy(config.sensorId, sensorId.c_str(), sizeof(config.sensorId));
  registerSensor(serverIP, serverPort, authUser, authPass, 
                sensorId, scaleId, sensorName, locationName);
}


void sendSensorData() {
  if (!wifiConnected || strlen(config.sensorId) == 0 || strlen(config.serverIP) == 0) {
    return;
  }

  // Чтение данных с датчика
  float temperature = dht.getTemperature();
  
  if (isnan(temperature)) {
    Serial.println("Ошибка чтения данных с датчика DHT!");
    return;
  }

  // Формируем URL для отправки данных
  String readingsUrl = "http://" + String(config.serverIP) + ":" + String(config.serverPort) + "/readings";

  // Формируем JSON тело запроса
  DynamicJsonDocument doc(256);
  doc["sensorId"] = config.sensorId;
  doc["value"] = temperature;
  
  String payload;
  serializeJson(doc, payload);

  // Отправляем данные на сервер
  HTTPClient http;
  http.begin(wifiClient, readingsUrl);
  http.addHeader("Content-Type", "application/json");
  
  int httpCode = http.POST(payload);
  String response = httpCode > 0 ? http.getString() : "";
  http.end();

  if (httpCode == HTTP_CODE_OK || httpCode == 201) {
    Serial.println("Данные успешно отправлены: " + String(temperature) + "°C");
  } else {
    Serial.println("Ошибка отправки данных. Код: " + String(httpCode) + 
                 (response.length() ? ", Ответ: " + response : ""));
  }
}

void setup() {
  Serial.begin(115200);
  dht.setup(DHT_PIN,DHT_TYPE);
  loadConfig();
  
  if (wifiConfigured) {
    connectToWiFi();
  }
  
  if (!wifiConnected) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.println("Режим AP. IP: " + WiFi.softAPIP().toString());
  }
  
  server.on("/", handleRoot);
  server.on("/saveWifi", handleSaveWifi);
  server.on("/saveSensor", handleSaveSensor);
  server.begin();
}

void loop() {
  server.handleClient();
    if (wifiConnected && millis() - LRT >= readingInterval) {
    sendSensorData();
    LRT = millis();
  }
}

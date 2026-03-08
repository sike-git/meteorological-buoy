#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HardwareSerial.h>
#include <GyverDS18.h>
#include <TinyGPSPlus.h>

#define PIN_MQ135     34
#define PIN_UV        36
#define PIN_BAT       32
#define PIN_DS18B20   14
#define GPS_RX_PIN    12
#define GPS_TX_PIN    13

GyverDS18Single ds(PIN_DS18B20);  // пин


const float ADC_MAX = 4095.0;
const float ADC_REF_VOLT = 3.3; // если 3.3 v

// Battery divider: настройте R1 и R2 под ваш делитель
const float BAT_R1 = 47000.0; // Верхний резистор в делителе, Ом
const float BAT_R2 = 47000.0; // Нижний резистор в делителе, Ом
// Задержки
const unsigned long SAMPLE_INTERVAL_MS = 2000;


// ----- GPS -----
HardwareSerial SerialGPS(2);
TinyGPSPlus gps;

// ----- Web server (AP mode) -----
WebServer server(80);
const char* AP_SSID = "ESP32_SensorHub";
const char* AP_PASS = "sensors123";

// Дефолтные значения
float last_mq135_voltage = 0.0;
float last_uv_voltage = 0.0;
float last_bat_voltage = 0.0;
float last_temp_c = NAN;
float t;
double last_lat = 0.0, last_lng = 0.0;
bool gps_has_fix = false;
unsigned long lastSample = 0;

// ---- утилиты ----
float readADCVoltage(int pin) {
  int raw = analogRead(pin);
  float v = (raw / ADC_MAX) * ADC_REF_VOLT;
  return v;
}

float readBatteryVoltage() {
  float vadc = readADCVoltage(PIN_BAT);
  float vbat = vadc * ((BAT_R1 + BAT_R2) / BAT_R2);
  return vbat;
}
String sensorsToHTML() {
  String s = "<!doctype html><html><head><meta charset='utf-8'><title>ESP32 Датчики</title></head><body>";
  s += "<h2>ESP32 — Показания датчиков</h2><table border='1' cellpadding='6'>";
  s += "<tr><th>Датчик</th><th>Значение</th></tr>";
  s += "<tr><td>MQ-135 (Vout)</td><td>" + String(last_mq135_voltage, 3) + " В</td></tr>";
  s += "<tr><td>UV-датчик (Vout)</td><td>" + String(last_uv_voltage, 3) + " В</td></tr>";
  s += "<tr><td>Батарея (2S)</td><td>" + String(last_bat_voltage, 3) + " В</td></tr>";
  s += "<tr><td>DS18B20</td><td>" + (isnan(last_temp_c) ? String("Нет данных") : String(last_temp_c, 2) + " °C") + "</td></tr>";
  s += "<tr><td>GPS</td><td>" + String(gps_has_fix ? "Фикс присутствует" : "Фикс отсутствует") + "</td></tr>";
  if (gps_has_fix) {
    s += "<tr><td>Широта, Долгота</td><td>" + String(last_lat, 6) + ", " + String(last_lng, 6) + "</td></tr>";
    s += "<tr><td>Спутники</td><td>" + String(gps.satellites.value()) + "</td></tr>";
    s += "<tr><td>Скорость (км/ч)</td><td>" + String(gps.speed.kmph(), 2) + "</td></tr>";
  }
  s += "</table><p><a href='/sensors'>JSON (машинно)</a></p></body></html>";
  return s;
}

String sensorsToJSON() {
  String j = "{";
  j += "\"mq135_v\": " + String(last_mq135_voltage, 3) + ",";
  j += "\"uv_v\": " + String(last_uv_voltage, 3) + ",";
  j += "\"battery_v\": " + String(last_bat_voltage, 3) + ",";
  j += "\"temp_c\": " + (isnan(last_temp_c) ? String("null") : String(last_temp_c, 2)) + ",";
  j += "\"gps_fix\": " + String(gps_has_fix ? "true" : "false") + ",";
  j += "\"latitude\": " + (gps_has_fix ? String(last_lat, 6) : String("null")) + ",";
  j += "\"longitude\": " + (gps_has_fix ? String(last_lng, 6) : String("null"));
  j += "}";
  return j;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", sensorsToHTML());
}
void handleJSON() {
  server.send(200, "application/json; charset=utf-8", sensorsToJSON());
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("ESP32: Запуск хаба датчиков...");

 
  ds.setResolution(12);
  // попытка получить адрес первого DS18B20 (если есть)

  // Настройка GPS (Serial2)
  SerialGPS.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("GPS: Serial2 запущен на RX=" + String(GPS_RX_PIN) + " TX=" + String(GPS_TX_PIN));

  // Настройка WiFi AP
  WiFi.mode(WIFI_AP);
  bool apOK = WiFi.softAP(AP_SSID, AP_PASS);
  if (apOK) {
    IPAddress IP = WiFi.softAPIP();
    Serial.println("Точка доступа запущена. SSID: " + String(AP_SSID));
    Serial.print("IP точки доступа: "); Serial.println(IP);
  } else {
    Serial.println("Ошибка запуска точки доступа (softAP)");
  }

  // Web server endpoints
  server.on("/", handleRoot);
  server.on("/sensors", handleJSON);
  server.begin();
  Serial.println("Веб-сервер запущен.");

  
  analogSetPinAttenuation(PIN_MQ135, ADC_11db);
  analogSetPinAttenuation(PIN_UV, ADC_11db);
  analogSetPinAttenuation(PIN_BAT, ADC_11db);

  lastSample = millis() - SAMPLE_INTERVAL_MS;
}

void loop() {
  server.handleClient();
  while (SerialGPS.available()) {
    if (gps.encode(SerialGPS.read())) {
      // обновлённые данные GPS доступны внутри gps
    }
  }

  // Периодическое снятие показаний
  if (millis() - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = millis();

    // MQ-135 (аналоговый)
    last_mq135_voltage = readADCVoltage(PIN_MQ135);

    // UV sensor (аналоговый)
    last_uv_voltage = readADCVoltage(PIN_UV);

    // Battery
    last_bat_voltage = readBatteryVoltage();

    // DS18B20
    if (!ds.tick()) {
        t = ds.getTemp();
    }

    if (t == NAN) {
      last_temp_c = NAN;
    } else {
      last_temp_c = t;
    }

    // GPS
    gps_has_fix = gps.location.isValid();
    if (gps_has_fix) {
      last_lat = gps.location.lat();
      last_lng = gps.location.lng();
    }

    // Вывод в Serial
    Serial.printf("MQ-135 (напряжение): %.3f В\n", last_mq135_voltage);
    Serial.printf("UV-датчик (напряжение): %.3f В\n", last_uv_voltage);
    Serial.printf("Батарея (2S) напряжение: %.3f В\n", last_bat_voltage);
    if (isnan(last_temp_c)) Serial.println("DS18B20: нет данных");
    else Serial.printf("DS18B20: %.2f °C\n", last_temp_c);
    if (gps_has_fix) {
      Serial.printf("GPS: фиксация есть, ширина = %.6f, долгота = %.6f, спутники = %d, скорость=%.2f км/ч\n",
                    last_lat, last_lng, gps.satellites.value(), gps.speed.kmph());
    } else {
      Serial.println("GPS: фиксация отсутствует");
    }
  }
}
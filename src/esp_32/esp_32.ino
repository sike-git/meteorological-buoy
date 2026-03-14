#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HardwareSerial.h>
#include <GyverDS18.h>
#include <TinyGPSPlus.h>
#include <math.h>

#define PIN_MQ35 34
#define PIN_UV 36
#define PIN_BAT 2
#define PIN_DS18B20 14
#define GPS_RX_PIN 12
#define GPS_TX_PIN 13

GyverDS18Single ds(PIN_DS18B20);

// ------------------- Константы -------------------
const float ADC_MAX = 4095.0;
const float ADC_REF_VOLT = 3.3;

// Battery divider: проверьте реальные номиналы в железе
const float BAT_R1 = 47000.0;
const float BAT_R2 = 47000.0;

const unsigned long SAMPLE_INTERVAL_MS = 2000;
const unsigned long GPS_STARTUP_CHECK_MS = 3000;
const unsigned long DS18B20_CHECK_TIMEOUT_MS = 3000;
const unsigned long GPS_STREAM_TIMEOUT_MS = 5000;

// ------------------- GPS -------------------
HardwareSerial SerialGPS(2);
TinyGPSPlus gps;

// ------------------- Web server (AP mode) -------------------
WebServer server(80);
const char* AP_SSID = "ESP32_SensorHub";
const char* AP_PASS = "sensors123";

// ------------------- Последние данные -------------------
float last_mq35_voltage = 0.0;
float last_uv_voltage = 0.0;
float last_bat_voltage = 0.0;
float last_temp_c = NAN;
double last_lat = 0.0;
double last_lng = 0.0;
bool gps_has_fix = false;

unsigned long lastSample = 0;
unsigned long lastGpsByteMs = 0;

// ------------------- Статусы датчиков -------------------
bool mq35_ok = false;
bool uv_ok = false;
bool bat_ok = false;
bool ds18b20_ok = false;
bool gps_stream_ok = false;

String mq35_status = "не проверен";
String uv_status = "не проверен";
String bat_status = "не проверен";
String ds18b20_status = "не проверен";
String gps_status = "не проверен";

// ------------------- Утилиты -------------------
float readADCVoltage(int pin) {
  int raw = analogRead(pin);
  return (raw / ADC_MAX) * ADC_REF_VOLT;
}

int readADCRawAvg(int pin, int samples = 10) {
  uint32_t sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delay(2);
  }
  return (int)(sum / samples);
}

float readADCVoltageAvg(int pin, int samples = 10) {
  int raw = readADCRawAvg(pin, samples);
  return (raw / ADC_MAX) * ADC_REF_VOLT;
}

float readBatteryVoltage() {
  float vadc = readADCVoltageAvg(PIN_BAT, 12);
  return vadc * ((BAT_R1 + BAT_R2) / BAT_R2);
}

bool isPlausibleTemperature(float t) {
  return !isnan(t) && t > -55.0 && t < 125.0;
}

// Универсальное чтение DS18B20 с ожиданием
bool readDS18B20Once(float &tempOut, uint32_t timeoutMs = 1500) {
  uint32_t start = millis();
  
  while (millis() - start < timeoutMs) {
    ds.tick();
    float t = ds.getTemp();
    
    if (isPlausibleTemperature(t)) {
      tempOut = t;
      return true;
    }
    
    delay(20);
  }
  
  return false;
}

bool checkAnalogSensor(int pin, String &statusText, const char* sensorName) {
  int raw = readADCRawAvg(pin, 12);
  
  if (raw < 0 || raw > 4095) {
    statusText = String(sensorName) + ": ошибка АЦП";
    return false;
  }
  
  // Это не гарантирует, что сам датчик физически исправен,
  // но подтверждает, что канал АЦП читается.
  statusText = String(sensorName) + ": канал АЦП читается, raw=" + String(raw);
  return true;
}

bool checkDS18B20Sensor(String &statusText) {
  float t = NAN;
  bool ok = readDS18B20Once(t, DS18B20_CHECK_TIMEOUT_MS);
  
  if (ok) {
    last_temp_c = t;
    statusText = "DS18B20: исправен, температура = " + String(t, 2) + " °C";
    return true;
  }
  
  last_temp_c = NAN;
  statusText = "DS18B20: нет ответа или некорректные данные";
  return false;
}

bool checkGPSSensor(String &statusText) {
  uint32_t start = millis();
  bool gotBytes = false;
  
  while (millis() - start < GPS_STARTUP_CHECK_MS) {
    while (SerialGPS.available()) {
      char c = (char)SerialGPS.read();
      gotBytes = true;
      lastGpsByteMs = millis();
      gps.encode(c);
    }
    delay(5);
  }
  
  if (!gotBytes) {
    statusText = "GPS: нет данных по UART, модуль не подтвержден";
    return false;
  }
  
  statusText = "GPS: поток NMEA обнаружен";
  return true;
}

void printStatusLine(const char* name, bool ok, const String& statusText) {
  Serial.print(name);
  Serial.print(" -> ");
  Serial.print(ok ? "ИСПРАВЕН" : "НЕИСПРАВЕН");
  Serial.print(" | ");
  Serial.println(statusText);
}

String sensorsToHTML() {
  String s = "<!doctype html><html><head><meta charset='utf-8'><title>ESP32 Датчики</title></head><body>";
  s += "<h2>ESP32 — Показания датчиков</h2>";
  s += "<table border='1' cellpadding='6'>";
  s += "<tr><th>Датчик</th><th>Статус</th><th>Значение</th></tr>";
  
  s += "<tr><td>MQ-35</td><td>" + String(mq35_ok ? "исправен" : "неисправен") + "</td><td>";
  s += mq35_ok ? (String(last_mq35_voltage, 3) + " В") : "данные не выводятся";
  s += "</td></tr>";
  
  s += "<tr><td>UV</td><td>" + String(uv_ok ? "исправен" : "неисправен") + "</td><td>";
  s += uv_ok ? (String(last_uv_voltage, 3) + " В") : "данные не выводятся";
  s += "</td></tr>";
  
  s += "<tr><td>Battery</td><td>" + String(bat_ok ? "исправен" : "неисправен") + "</td><td>";
  s += bat_ok ? (String(last_bat_voltage, 3) + " В") : "данные не выводятся";
  s += "</td></tr>";
  
  s += "<tr><td>DS18B20</td><td>" + String(ds18b20_ok ? "исправен" : "неисправен") + "</td><td>";
  s += ds18b20_ok ? (isnan(last_temp_c) ? "нет данных" : String(last_temp_c, 2) + " °C") : "данные не выводятся";
  s += "</td></tr>";
  
  s += "<tr><td>GPS</td><td>" + String(gps_stream_ok ? "исправен" : "неисправен") + "</td><td>";
  if (!gps_stream_ok) {
    s += "данные не выводятся";
  } else if (!gps_has_fix) {
    s += "модуль отвечает, но фикса нет";
  } else {
    s += String(last_lat, 6) + ", " + String(last_lng, 6);
  }
  s += "</td></tr>";
  
  s += "</table>";
  
  s += "<h3>Диагностика</h3><ul>";
  s += "<li>" + mq35_status + "</li>";
  s += "<li>" + uv_status + "</li>";
  s += "<li>" + bat_status + "</li>";
  s += "<li>" + ds18b20_status + "</li>";
  s += "<li>" + gps_status + "</li>";
  s += "</ul>";
  
  s += "<p><a href='/sensors'>JSON (машинно)</a></p></body></html>";
  return s;
}

String sensorsToJSON() {
  String j = "{";
  
  j += "\"mq35_ok\": " + String(mq35_ok ? "true" : "false") + ",";
  j += "\"mq35_status\": \"" + mq35_status + "\",";
  j += "\"mq35_v\": " + (mq35_ok ? String(last_mq35_voltage, 3) : String("null")) + ",";
  
  j += "\"uv_ok\": " + String(uv_ok ? "true" : "false") + ",";
  j += "\"uv_status\": \"" + uv_status + "\",";
  j += "\"uv_v\": " + (uv_ok ? String(last_uv_voltage, 3) : String("null")) + ",";
  
  j += "\"battery_ok\": " + String(bat_ok ? "true" : "false") + ",";
  j += "\"battery_status\": \"" + bat_status + "\",";
  j += "\"battery_v\": " + (bat_ok ? String(last_bat_voltage, 3) : String("null")) + ",";
  
  j += "\"ds18b20_ok\": " + String(ds18b20_ok ? "true" : "false") + ",";
  j += "\"ds18b20_status\": \"" + ds18b20_status + "\",";
  j += "\"temp_c\": " + (ds18b20_ok && !isnan(last_temp_c) ? String(last_temp_c, 2) : String("null")) + ",";
  
  j += "\"gps_ok\": " + String(gps_stream_ok ? "true" : "false") + ",";
  j += "\"gps_status\": \"" + gps_status + "\",";
  j += "\"gps_fix\": " + String(gps_has_fix ? "true" : "false") + ",";
  j += "\"latitude\": " + (gps_stream_ok && gps_has_fix ? String(last_lat, 6) : String("null")) + ",";
  j += "\"longitude\": " + (gps_stream_ok && gps_has_fix ? String(last_lng, 6) : String("null"));
  
  j += "}";
  return j;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", sensorsToHTML());
}

void handleJSON() {
  server.send(200, "application/json; charset=utf-8", sensorsToJSON());
}

void printStartupDiagnostics() {
  Serial.println();
  Serial.println("========== ПЕРВИЧНАЯ ПРОВЕРКА ДАТЧИКОВ ==========");
  printStatusLine("MQ-35", mq35_ok, mq35_status);
  printStatusLine("UV", uv_ok, uv_status);
  printStatusLine("BAT", bat_ok, bat_status);
  printStatusLine("DS18B20", ds18b20_ok, ds18b20_status);
  printStatusLine("GPS", gps_stream_ok, gps_status);
  Serial.println("=================================================");
  Serial.println();
}

void printCycleDiagnosticsAndData() {
  Serial.println();
  Serial.println("========== СТАТУС ДАТЧИКОВ ==========");
  
  printStatusLine("MQ-35", mq35_ok, mq35_status);
  if (mq35_ok) {
    Serial.printf("MQ-35: %.3f В\n", last_mq35_voltage);
  } else {
    Serial.println("MQ-35: данные не выводим");
  }
  
  printStatusLine("UV", uv_ok, uv_status);
  if (uv_ok) {
    Serial.printf("UV: %.3f В\n", last_uv_voltage);
  } else {
    Serial.println("UV: данные не выводим");
  }
  
  printStatusLine("BAT", bat_ok, bat_status);
  if (bat_ok) {
    Serial.printf("Батарея: %.3f В\n", last_bat_voltage);
  } else {
    Serial.println("Батарея: данные не выводим");
  }
  
  printStatusLine("DS18B20", ds18b20_ok, ds18b20_status);
  if (ds18b20_ok) {
    if (isnan(last_temp_c)) Serial.println("DS18B20: нет данных");
    else Serial.printf("DS18B20: %.2f °C\n", last_temp_c);
  } else {
    Serial.println("DS18B20: данные не выводим");
  }
  
  printStatusLine("GPS", gps_stream_ok, gps_status);
  if (!gps_stream_ok) {
    Serial.println("GPS: данные не выводим");
  } else if (!gps_has_fix) {
    Serial.println("GPS: модуль отвечает, но фикса нет, координаты не выводим");
  } else {
    Serial.printf("GPS: lat=%.6f, lng=%.6f, спутники=%d, скорость=%.2f км/ч\n",
                  last_lat, last_lng, gps.satellites.value(), gps.speed.kmph());
  }
  
  Serial.println("=====================================");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("ESP32: запуск хаба датчиков...");
  
  analogSetPinAttenuation(PIN_MQ35, ADC_11db);
  analogSetPinAttenuation(PIN_UV, ADC_11db);
  analogSetPinAttenuation(PIN_BAT, ADC_11db);
  
  ds.setResolution(12);
  
  SerialGPS.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("GPS: Serial2 запущен на RX=" + String(GPS_RX_PIN) + " TX=" + String(GPS_TX_PIN));
  
  mq35_ok = checkAnalogSensor(PIN_MQ35, mq35_status, "MQ-35");
  uv_ok = checkAnalogSensor(PIN_UV, uv_status, "UV");
  bat_ok = checkAnalogSensor(PIN_BAT, bat_status, "Battery");
  ds18b20_ok = checkDS18B20Sensor(ds18b20_status);
  gps_stream_ok = checkGPSSensor(gps_status);
  
  // Если аналоговые каналы исправны — снимем первые значения
  if (mq35_ok) last_mq35_voltage = readADCVoltageAvg(PIN_MQ35, 12);
  if (uv_ok) last_uv_voltage = readADCVoltageAvg(PIN_UV, 12);
  if (bat_ok) last_bat_voltage = readBatteryVoltage();
  
  gps_has_fix = gps.location.isValid();
  if (gps_has_fix) {
    last_lat = gps.location.lat();
    last_lng = gps.location.lng();
  }
  
  printStartupDiagnostics();
  
  // Wi-Fi AP
  WiFi.mode(WIFI_AP);
  bool apOK = WiFi.softAP(AP_SSID, AP_PASS);
  if (apOK) {
    IPAddress IP = WiFi.softAPIP();
    Serial.println("Точка доступа запущена. SSID: " + String(AP_SSID));
    Serial.print("IP точки доступа: ");
    Serial.println(IP);
  } else {
    Serial.println("Ошибка запуска точки доступа (softAP)");
  }
  
  // Web server
  server.on("/", handleRoot);
  server.on("/sensors", handleJSON);
  server.begin();
  Serial.println("Веб-сервер запущен.");
  
  lastSample = millis() - SAMPLE_INTERVAL_MS;
}

void loop() {
  server.handleClient();
  
  // Постоянно читаем GPS поток
  while (SerialGPS.available()) {
    char c = (char)SerialGPS.read();
    lastGpsByteMs = millis();
    gps.encode(c);
  }
  
  // Актуализация состояния GPS-потока
  gps_stream_ok = (gps.charsProcessed() > 0) && ((millis() - lastGpsByteMs) < GPS_STREAM_TIMEOUT_MS);
  gps_status = gps_stream_ok ? "GPS: поток NMEA активен" : "GPS: нет актуальных данных по UART";
  
  // Периодический опрос датчиков
  if (millis() - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = millis();
    
    // MQ-35
    if (mq35_ok) {
      last_mq35_voltage = readADCVoltageAvg(PIN_MQ35, 12);
      mq35_status = "MQ-35: измерение выполнено успешно";
    } else {
      mq35_status = "MQ-35: неисправен, данные не выводятся";
    }
    
    // UV
    if (uv_ok) {
      last_uv_voltage = readADCVoltageAvg(PIN_UV, 12);
      uv_status = "UV: измерение выполнено успешно";
    } else {
      uv_status = "UV: неисправен, данные не выводятся";
    }
    
    // Battery
    if (bat_ok) {
      last_bat_voltage = readBatteryVoltage();
      bat_status = "Battery: измерение выполнено успешно";
    } else {
      bat_status = "Battery: неисправен, данные не выводятся";
    }
    
    // DS18B20
    if (ds18b20_ok) {
      float t = NAN;
      if (readDS18B20Once(t, 1200)) {
        last_temp_c = t;
        ds18b20_status = "DS18B20: измерение выполнено успешно";
      } else {
        last_temp_c = NAN;
        ds18b20_ok = false;
        ds18b20_status = "DS18B20: потерян ответ, данные не выводятся";
      }
    } else {
      last_temp_c = NAN;
      ds18b20_status = "DS18B20: неисправен, данные не выводятся";
    }
    
    // GPS
    if (gps_stream_ok) {
      gps_has_fix = gps.location.isValid();
      
      if (gps_has_fix) {
        last_lat = gps.location.lat();
        last_lng = gps.location.lng();
        gps_status = "GPS: поток есть, фикс получен";
      } else {
        gps_status = "GPS: поток есть, но фикса нет";
      }
    } else {
      gps_has_fix = false;
      gps_status = "GPS: неисправен или нет потока, данные не выводятся";
    }
    
    printCycleDiagnosticsAndData();
  }
}
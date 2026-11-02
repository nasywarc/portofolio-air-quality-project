// ─────────────────────────────────────────────────────────────────────────────
// ESP32 Air Quality Monitor
// Sensors : ZH03B (PM1/PM2.5/PM10) + DHT22 (Temp/Humidity)
// Display  : 16x2 I2C LCD (address 0x27) using LCD_I2C library
// Backend  : Flask on AWS — sends data, receives US AQI + prediction
// ─────────────────────────────────────────────────────────────────────────────

#define BLYNK_PRINT Serial

#include "secrets.h"

#define DHTPIN  32
#define DHTTYPE DHT22

#include "DHT.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <BlynkSimpleEsp32.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>
#include <LCD_I2C.h>

// ── Hardware ──────────────────────────────────────────────────────────────────
DHT            dht(DHTPIN, DHTTYPE);
HardwareSerial zhSerial(2);
LCD_I2C        lcd(0x27, 16, 2);   // address, cols, rows

// ── WiFi & server ─────────────────────────────────────────────────────────────
char ssid[] = WIFI_SSID;
char pass[] = WIFI_PASS;

// ── Sensor values ─────────────────────────────────────────────────────────────
float   temp = 0, hum = 0;
uint8_t buf[24];
int     pm1 = 0, pm25 = 0, pm10 = 0;

// ── Flask response values ─────────────────────────────────────────────────────
float  cur_pm25      = -1;
int    cur_aqi       = -1;
String cur_category  = "---";

float  pred_pm25     = -1;
int    pred_aqi      = -1;
String pred_category = "---";
bool   pred_ready    = false;

// ── LCD cycling (3 screens × 5 seconds each) ──────────────────────────────────
#define LCD_CYCLE_MS 5000UL
uint8_t  lcdScreen     = 0;
uint32_t lastLcdSwitch = 0;

BlynkTimer timer;


// ─────────────────────────────────────────────────────────────────────────────
// SENSORS
// ─────────────────────────────────────────────────────────────────────────────

void getDHT() {
  temp = dht.readTemperature();
  hum  = dht.readHumidity();
}

void getZH03B() {
  while (zhSerial.available() >= 24) {
    if (zhSerial.read() == 0x42) {
      if (zhSerial.read() == 0x4D) {
        if (zhSerial.readBytes(buf, 22) == 22) {
          pm1  = (buf[8]  << 8) | buf[9];
          pm25 = (buf[10] << 8) | buf[11];
          pm10 = (buf[12] << 8) | buf[13];
        }
      }
    }
  }
}


// ─────────────────────────────────────────────────────────────────────────────
// LCD HELPERS
// ─────────────────────────────────────────────────────────────────────────────

// Print exactly `width` characters — pads short strings, truncates long ones.
// Prevents ghost characters when switching between screens of different lengths.
void lcdPrint(uint8_t col, uint8_t row, const String& text, uint8_t width = 16) {
  lcd.setCursor(col, row);
  String out = text;
  if ((int)out.length() > width) out = out.substring(0, width);
  while ((int)out.length() < width) out += ' ';
  lcd.print(out);
}

// Shorten US AQI category to fit 16 chars
// "Unhealthy for Sensitive Groups" → "Sensitive Grp"
String shortCategory(const String& cat) {
  if (cat == "Good")                            return "Good";
  if (cat == "Moderate")                        return "Moderate";
  if (cat == "Unhealthy for Sensitive Groups")  return "Sensitive Grp";
  if (cat == "Unhealthy")                       return "Unhealthy";
  if (cat == "Very Unhealthy")                  return "Very Unhealthy";
  if (cat == "Hazardous")                       return "Hazardous";
  return cat.substring(0, 16);
}

void updateLCD() {
  switch (lcdScreen) {

    // Screen 0 — PM2.5 now + predicted
    case 0:
      lcdPrint(0, 0, "Now PM2.5:" + (cur_pm25 >= 0 ? String((int)cur_pm25) : "---"));
      lcdPrint(0, 1, pred_ready && pred_pm25 >= 0
                     ? "Pre PM2.5:" + String((int)pred_pm25)
                     : "Pre: warmup");
      break;

    // Screen 1 — AQI now + predicted
    case 1:
      lcdPrint(0, 0, "AQI Now:" + (cur_aqi >= 0 ? String(cur_aqi) : "---"));
      lcdPrint(0, 1, "AQI Pre:" + (pred_ready && pred_aqi >= 0
                                    ? String(pred_aqi) : "---"));
      break;

    // Screen 2 — AQI category + Temp/Humidity
    case 2:
      lcdPrint(0, 0, cur_category != "---" ? shortCategory(cur_category) : "Connecting...");
      lcdPrint(0, 1, (!isnan(temp) && !isnan(hum))
                     ? "T:" + String(temp, 1) + "C H:" + String((int)hum) + "%"
                     : "T:--- H:---");
      break;
  }
}


// ─────────────────────────────────────────────────────────────────────────────
// HTTP: SEND + PARSE RESPONSE
// ─────────────────────────────────────────────────────────────────────────────

void sendData() {
  float send_temp = (!isnan(temp)) ? temp  : -1.0;
  float send_hum  = (!isnan(hum))  ? hum   : -1.0;
  int   send_pm25 = (pm25 > 0)     ? pm25  : -1;
  int   send_pm10 = (pm10 > 0)     ? pm10  : -1;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Not connected");
    lcdPrint(0, 0, "WiFi lost...");
    lcdPrint(0, 1, "Reconnecting");
    return;
  }

  HTTPClient http;
  http.begin(SERVER_URL);
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");

  JsonDocument reqDoc;
  reqDoc["pm25"] = send_pm25;
  reqDoc["pm10"] = send_pm10;
  reqDoc["temp"] = send_temp;
  reqDoc["hum"]  = send_hum;

  String jsonString;
  serializeJson(reqDoc, jsonString);
  Serial.println("[HTTP] Sending: " + jsonString);

  int httpCode = http.POST(jsonString);
  Serial.println("[HTTP] Response: " + String(httpCode));

  if (httpCode == 200) {
    String payload = http.getString();
    Serial.println("[HTTP] Payload: " + payload);

    JsonDocument resDoc;
    DeserializationError err = deserializeJson(resDoc, payload);

    if (err) {
      Serial.println("[JSON] Error: " + String(err.c_str()));
      lcdPrint(0, 0, "JSON Err!");
      lcdPrint(0, 1, String(err.c_str()).substring(0, 16));
    } else {
      // ── Current ────────────────────────────────────────────────────────
      if (!resDoc["current"]["pm25_ugm3"].isNull())
        cur_pm25 = resDoc["current"]["pm25_ugm3"].as<float>();

      if (!resDoc["current"]["aqi"].isNull()) {
        cur_aqi      = resDoc["current"]["aqi"]["aqi_value"].as<int>();
        cur_category = resDoc["current"]["aqi"]["category"].as<String>();
      }

      // ── Prediction ─────────────────────────────────────────────────────
      if (!resDoc["prediction"]["pm25_ugm3"].isNull()) {
        pred_pm25    = resDoc["prediction"]["pm25_ugm3"].as<float>();
        pred_aqi     = resDoc["prediction"]["aqi"]["aqi_value"].as<int>();
        pred_category= resDoc["prediction"]["aqi"]["category"].as<String>();
        pred_ready   = true;
      } else {
        pred_ready   = false;
      }

      Serial.printf("[DATA] cur PM2.5=%.1f AQI=%d (%s)\n",
                    cur_pm25, cur_aqi, cur_category.c_str());
      Serial.printf("[DATA] pre PM2.5=%.1f AQI=%d (%s) ready=%d\n",
                    pred_pm25, pred_aqi, pred_category.c_str(), pred_ready);
    }

  } else {
    Serial.println("[HTTP] Error: " + String(httpCode));
    lcdPrint(0, 0, "Server Err");
    lcdPrint(0, 1, "HTTP:" + String(httpCode));
  }

  http.end();
}


// ─────────────────────────────────────────────────────────────────────────────
// MAIN CYCLE
// ─────────────────────────────────────────────────────────────────────────────

void inputAndSendData() {
  getDHT();
  delay(1000);
  while (zhSerial.available() > 0) { zhSerial.read(); }
  delay(2000);
  getZH03B();
  delay(1000);
  sendData();
}


// ─────────────────────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(9600);
  zhSerial.begin(9600, SERIAL_8N1, 16, 17);
  dht.begin();

  lcd.begin();
  lcd.backlight();
  lcdPrint(0, 0, "Air Quality Mon");
  lcdPrint(0, 1, "Connecting WiFi");

  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WIFI] Connected: " + WiFi.localIP().toString());

  lcdPrint(0, 0, "WiFi Connected!");
  lcdPrint(0, 1, WiFi.localIP().toString());
  delay(2000);

  lcdPrint(0, 0, "Warming up...");
  lcdPrint(0, 1, "First read 2min");

  // Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
  timer.setInterval(120000L, inputAndSendData);
}


// ─────────────────────────────────────────────────────────────────────────────
// LOOP
// ─────────────────────────────────────────────────────────────────────────────

void loop() {
  // Blynk.run();
  timer.run();

  uint32_t now = millis();
  if (now - lastLcdSwitch >= LCD_CYCLE_MS) {
    lastLcdSwitch = now;
    lcdScreen = (lcdScreen + 1) % 3;
    updateLCD();
  }
}

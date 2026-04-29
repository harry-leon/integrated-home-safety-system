// =============================================
// SMART LOCK - ESP32 + BLYNK (v9.3 LEAN)
// ✅ LED Matrix REMOVED hoàn toàn
// ✅ Sensor timer: 3s → 10s
// ✅ Weather timer: 120s → 600s (10 phút)
// ✅ Console log: 2s → 10s
// ✅ Telemetry timeout: 1500ms → 800ms
// ✅ NTP update: vẫn 1h/lần
// ✅ sendSensorData tách Blynk / Telemetry riêng
//    - Blynk: mỗi 10s
//    - Telemetry backend: mỗi 30s (riêng timer)
// ✅ String concat trong loop dùng char buf thay vì String
// ✅ Tất cả blocking delay đã xóa từ v9.2
// RGB LED active LOW (common anode)
//   GPIO0 = ĐỎ, GPIO2 = XANH LÁ
// Mật khẩu mặc định: 5678
// Keypad: A=reset, B=đổi pass, C=enroll vân tay, D=toggle PIR, *=clear, #=enter
// Blynk Events: gas_alert, wrong_pass_alert, fingerprint_alert, pir_alert
// V20=Button mở/đóng | V30=Door status | V40=Alert enable
// V50=Nhiệt độ | V51=Thời tiết | V70=Auto-lock delay | V71=Auto-lock enable
// V100=Finger ID | V101=Enroll | V102=Delete | V103=Status | V104=Tên
// V41=PIR Alert Mode | V3=LDR | V4=PIR
// =============================================

#define BLYNK_TEMPLATE_ID   "TMPL6p8KU2UbZ"
#define BLYNK_TEMPLATE_NAME "Project IOT102"
#define BLYNK_AUTH_TOKEN    "INnoYtJ6zm9Rj-05xWV1hMb4H-XxbTve"

// ✅ Tắt Serial debug của Blynk để giảm overhead
#define BLYNK_NO_BUILTIN_LOGO
#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Keypad.h>
#include <ESP32Servo.h>
#include <Adafruit_Fingerprint.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// =========================================================
// PIN & THRESHOLD DEFINES
// =========================================================
#define MQ2_PIN     35
#define PIR_PIN     27
#define LDR_PIN     34
#define BUZZER_PIN  15
#define SERVO_PIN   19
#define LED_RED      0
#define LED_GREEN    2
#define LED_ON     LOW
#define LED_OFF   HIGH

#define GAS_THRESHOLD        1400
#define LDR_DARK_THRESHOLD    300
#define MAX_FINGER_FAIL         3
#define MAX_PASS_FAIL           3
#define ALERT_DURATION       7000
#define ENROLL_TIMEOUT      15000
#define FINGER_WAIT_TIMEOUT  8000

// =========================================================
// BLYNK VIRTUAL PINS
// =========================================================
#define VPIN_LDR             3
#define VPIN_PIR             4
#define VPIN_PIR_ALERT_MODE 41

// =========================================================
// WEATHER
// =========================================================
float  weatherTemp = 0;
String weatherDesc = "---";
const char* weatherURL = "http://api.openweathermap.org/data/2.5/weather?lat=10.8494&lon=106.7716&units=metric&appid=acd5b0ba3dd34ca92e4ff7463f46bca2";

// Last text shown on LCD during alert handlers (used to avoid repeated lcd.clear()).
String matrixLastText = "";

// =========================================================
// OBJECTS
// =========================================================
LiquidCrystal_I2C lcd(0x27, 16, 2);
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
Servo myServo;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600);
BlynkTimer timer;
Preferences prefs;

// =========================================================
// KEYPAD
// =========================================================
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {13, 12, 14, 4};
byte colPins[COLS] = {33, 32, 26, 25};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// =========================================================
// CREDENTIALS
// =========================================================
char auth[] = BLYNK_AUTH_TOKEN;
char ssid[] = "Bs11-12077";
char pass[] = "12071207";

const char* DEVICE_CODE           = "SL-FRONT-001";
const char* BACKEND_TELEMETRY_URL = "http://192.168.1.5:8080/api/telemetry/report";

// =========================================================
// LCD CUSTOM CHARS
// =========================================================
byte bellChar[8]   = {0b00100,0b01110,0b01110,0b01110,0b11111,0b00000,0b00100,0b00000};
byte lockChar[8]   = {0b01110,0b10001,0b10001,0b11111,0b11011,0b11011,0b11111,0b00000};
byte personChar[8] = {0b00100,0b01110,0b00100,0b01110,0b10101,0b00100,0b01010,0b10001};
byte checkChar[8]  = {0b00000,0b00001,0b00011,0b10110,0b11100,0b01000,0b00000,0b00000};

// =========================================================
// STATE MACHINES
// =========================================================
enum SystemState {
  STATE_NORMAL, STATE_ENTER_PASS, STATE_CHANGE_PASS,
  STATE_ALERT_GAS, STATE_ALERT_PASS,
  STATE_ENROLL_AUTH, STATE_ENROLL_ID,
  STATE_ALERT_PIR, STATE_KEY_D_AUTH
};
SystemState sysState = STATE_NORMAL;

enum EnrollStep {
  ENROLL_IDLE,
  ENROLL_CHECK_DUP_WAIT,
  ENROLL_CHECK_DUP_PROCESS,
  ENROLL_WAIT_REMOVE,
  ENROLL_STEP2_WAIT,
  ENROLL_STEP2_PROCESS,
  ENROLL_DONE
};
EnrollStep enrollStep = ENROLL_IDLE;

enum ChangePStep { CP_OLD, CP_NEW, CP_CONFIRM };
ChangePStep cpStep = CP_OLD;

// =========================================================
// GLOBAL VARIABLES
// =========================================================
String password    = "5678";
String inputPass   = "";
String newPassBuf  = "";
String pendingName = "";
String enrollIDStr = "";

int  passFailCount   = 0;
int  fingerFailCount = 0;
int  idFromBlynk     = 1;
int  displayMode     = 0;

bool isFingerLocked  = false;
bool isDoorOpen      = false;
bool normalState     = true;
bool alertEnabled    = true;
bool isEnrolling     = false;
bool isDeleting      = false;
bool fingerPresent   = false;
bool fingerProcessed = false;
bool blinkState      = false;

unsigned long alertStartTime  = 0;
unsigned long lcdUpdateTime   = 0;
unsigned long lastKeyTime     = 0;
unsigned long lastBlink       = 0;

// ✅ Console log: tăng lên 10s thay vì 2s
unsigned long lastConsoleTime = 0;
const unsigned long CONSOLE_INTERVAL = 10000;

// ✅ Throttle fingerprint check
unsigned long lastFingerCheck = 0;
const unsigned long FINGER_CHECK_INTERVAL = 100;

int  dispLastSec = -1;
unsigned long dispT0 = 0;

// =========================================================
// AUTO-LOCK
// =========================================================
unsigned long autoLockDelay   = 7000;
unsigned long doorOpenedAt    = 0;
bool          autoLockEnabled = true;

// =========================================================
// PIR ALERT & KEYPAD LOCKOUT
// =========================================================
bool pirAlertEnabled        = false;
bool pirAlertActive         = false;
unsigned long pirAlertStart = 0;

int  keypadFailCount                     = 0;
const int KEYPAD_MAX_FAIL                = 5;
const unsigned long KEYPAD_LOCK_DURATION = 30000;
bool keypadLocked                        = false;
unsigned long keypadLockedAt             = 0;

// =========================================================
// ĐÈN TỰ ĐỘNG (PIR + tối)
// =========================================================
bool          lightOn              = false;
unsigned long lightOnAt            = 0;
const unsigned long LIGHT_DURATION = 10000;

// =========================================================
// NON-BLOCKING LED
// =========================================================
unsigned long ledTimer    = 0;
int           ledDuration = 0;
int           ledType     = 0;

void ledAllOff()  { digitalWrite(LED_RED, LED_OFF); digitalWrite(LED_GREEN, LED_OFF); ledType = 0; }
void ledRedOn()   { digitalWrite(LED_RED, LED_ON);  digitalWrite(LED_GREEN, LED_OFF); ledType = 1; }
void ledGreenOn() { digitalWrite(LED_GREEN, LED_ON); digitalWrite(LED_RED, LED_OFF);  ledType = 2; }
void ledRed(int ms)   { ledRedOn();   ledTimer = millis(); ledDuration = ms; }
void ledGreen(int ms) { ledGreenOn(); ledTimer = millis(); ledDuration = ms; }

void updateLED() {
  if (ledDuration > 0 && ledType != 0 && millis() - ledTimer >= (unsigned long)ledDuration) {
    ledAllOff(); ledDuration = 0;
  }
}

// =========================================================
// NON-BLOCKING BUZZER
// =========================================================
const int BEEP_MAX = 8;
int  beepSeq[BEEP_MAX], beepLen = 0, beepIdx = 0;
bool beepOn = false;
unsigned long beepTimer = 0;

void beepStart(int* seq, int len) {
  memcpy(beepSeq, seq, len * sizeof(int));
  beepLen = len; beepIdx = 0; beepOn = true;
  beepTimer = millis(); digitalWrite(BUZZER_PIN, HIGH);
}
void updateBuzzer() {
  if (beepLen == 0) return;
  if (millis() - beepTimer >= (unsigned long)beepSeq[beepIdx]) {
    beepIdx++;
    if (beepIdx >= beepLen) {
      digitalWrite(BUZZER_PIN, LOW); beepLen = 0; beepIdx = 0; beepOn = false; return;
    }
    beepOn = !beepOn;
    digitalWrite(BUZZER_PIN, beepOn ? HIGH : LOW);
    beepTimer = millis();
  }
}

void beepKey()    { int s[] = {80, 0};            beepStart(s, 1); }
void beepOK()     { int s[] = {100, 80, 100, 0};  beepStart(s, 4); }
void beepFail()   { int s[] = {600, 0};            beepStart(s, 1); }
void beepWarn()   { int s[] = {80,60,80,60,80,0};  beepStart(s, 6); }
void beepDelete() { int s[] = {150, 100, 150, 0};  beepStart(s, 4); }

// =========================================================
// LƯU / ĐỌC / XÓA TÊN VÂN TAY
// =========================================================
void saveFingerName(int id, const String& name) {
  char key[12]; snprintf(key, sizeof(key), "fn_%d", id);
  prefs.putString(key, name);
  Serial.printf("[NAME] Saved ID%d=%s\n", id, name.c_str());
}
String loadFingerName(int id) {
  char key[12]; snprintf(key, sizeof(key), "fn_%d", id);
  char def[12]; snprintf(def, sizeof(def), "ID:%d", id);
  return prefs.getString(key, def);
}
void deleteFingerName(int id) {
  char key[12]; snprintf(key, sizeof(key), "fn_%d", id);
  prefs.remove(key);
  Serial.printf("[NAME] Del ID%d\n", id);
}

// =========================================================
// WEATHER — gọi qua timer, không blocking trong loop
// =========================================================
void getWeatherData() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(weatherURL);
  http.setTimeout(2000);
  int code = http.GET();
  if (code != 200) { http.end(); return; }
  JsonDocument doc;
  if (deserializeJson(doc, http.getString())) { http.end(); return; }
  weatherTemp     = doc["main"]["temp"].as<float>();
  String mainDesc = doc["weather"][0]["main"].as<String>();
  int clouds      = doc["clouds"]["all"].as<int>();
  if      (mainDesc == "Clear")                                           weatherDesc = "SUNNY";
  else if (mainDesc == "Clouds" && clouds <= 50)                          weatherDesc = "PARTLY CLOUDY";
  else if (mainDesc == "Clouds" && clouds >  50)                          weatherDesc = "CLOUDY";
  else if (mainDesc == "Rain"   || mainDesc == "Drizzle")                 weatherDesc = "RAINY";
  else if (mainDesc == "Thunderstorm")                                    weatherDesc = "STORM";
  else if (mainDesc == "Mist"   || mainDesc == "Haze" || mainDesc=="Fog") weatherDesc = "FOGGY";
  else                                                                    weatherDesc = mainDesc;
  Blynk.virtualWrite(V50, weatherTemp);
  Blynk.virtualWrite(V51, weatherDesc);
  Serial.printf("[WX] %.1fC %s\n", weatherTemp, weatherDesc.c_str());
  http.end();
}

// =========================================================
// ✅ TELEMETRY — timer riêng 30s, timeout 800ms, busy-guard
// =========================================================
bool telemetryBusy = false;

void sendTelemetryToBackend() {
  if (WiFi.status() != WL_CONNECTED || telemetryBusy) return;
  telemetryBusy = true;

  int gasVal = analogRead(MQ2_PIN);
  int ldrVal = analogRead(LDR_PIN);
  int pirVal = digitalRead(PIR_PIN);

  HTTPClient http;
  http.begin(BACKEND_TELEMETRY_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(800); // ✅ 800ms — backend local, đủ dùng

  // ✅ Dùng char buf thay vì String concatenation
  char payload[200];
  snprintf(payload, sizeof(payload),
    "{\"deviceCode\":\"%s\",\"gasValue\":%d,\"ldrValue\":%d,"
    "\"pirTriggered\":%s,\"temperature\":%.1f,\"weatherDesc\":\"%s\"}",
    DEVICE_CODE, gasVal, ldrVal,
    (pirVal == HIGH) ? "true" : "false",
    weatherTemp, weatherDesc.c_str());

  int httpCode = http.POST(payload);
  if (httpCode > 0) Serial.printf("[TEL] %d\n", httpCode);
  else              Serial.printf("[TEL] Err:%s\n", http.errorToString(httpCode).c_str());

  http.end();
  telemetryBusy = false;
}

// =========================================================
// ✅ BLYNK SENSOR — timer riêng 10s (chỉ gửi virtual pins)
// =========================================================
void sendBlynkSensors() {
  int gasVal = analogRead(MQ2_PIN);
  int ldrVal = analogRead(LDR_PIN);
  int pirVal = digitalRead(PIR_PIN);
  Blynk.virtualWrite(V1, gasVal);
  Blynk.virtualWrite(V2, gasVal > GAS_THRESHOLD ? 255 : 0);
  Blynk.virtualWrite(VPIN_LDR, ldrVal);
  Blynk.virtualWrite(VPIN_PIR, pirVal == HIGH ? 1 : 0);
}

// =========================================================
// FORWARD DECLARATIONS
// =========================================================
void closeDoor(bool byAutoLock);
void openDoor();
void restoreNormalLCD();

// =========================================================
// BLYNK CALLBACKS
// =========================================================
BLYNK_CONNECTED() {
  Blynk.syncVirtual(V40, VPIN_PIR_ALERT_MODE, V70, V71, V20, V100, V104);
}

BLYNK_WRITE(V100) {
  idFromBlynk = param.asInt();
  String existingName = loadFingerName(idFromBlynk);
  // ✅ Dùng snprintf thay vì String concat
  char info[40];
  if (existingName.startsWith("ID:"))
    snprintf(info, sizeof(info), "ID:%d", idFromBlynk);
  else
    snprintf(info, sizeof(info), "ID:%d | %s", idFromBlynk, existingName.c_str());
  Blynk.virtualWrite(V103, info);
}
BLYNK_WRITE(V104) {
  pendingName = param.asStr();
  pendingName.trim();
  Serial.printf("[NAME] Pending:'%s'\n", pendingName.c_str());
  // ✅ snprintf thay vì String concat
  char msg[40]; snprintf(msg, sizeof(msg), "Name ready: %s", pendingName.c_str());
  Blynk.virtualWrite(V103, msg);
}
BLYNK_WRITE(V101) {
  if (param.asInt() == 1 && !isEnrolling && !isDeleting && sysState == STATE_NORMAL) {
    if (pendingName.length() == 0) {
      Blynk.virtualWrite(V103, "ERROR: Enter name first!");
      Blynk.virtualWrite(V101, 0);
      lcd.clear();
      lcd.setCursor(0,0); lcd.print(" Enter name 1st ");
      lcd.setCursor(0,1); lcd.print("  in Blynk V104 ");
      lcdUpdateTime = millis() + 2500;
      beepFail(); ledRed(800); return;
    }
    isEnrolling = true; normalState = false;
    enrollStep  = ENROLL_CHECK_DUP_WAIT;
    Blynk.virtualWrite(V101, 0);
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("  Enrolling...  ");
    lcd.setCursor(0,1); lcd.print(pendingName.substring(0,16));
  }
}
BLYNK_WRITE(V102) {
  if (param.asInt() == 1 && !isEnrolling && !isDeleting && sysState == STATE_NORMAL) {
    isDeleting = true; normalState = false;
    Blynk.virtualWrite(V102, 0);
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("  Deleting...   ");
    lcd.setCursor(0,1); lcd.print("ID: "); lcd.print(idFromBlynk);
  }
}
BLYNK_WRITE(V20) {
  if (param.asInt() == 1) openDoor(); else closeDoor(false);
}
BLYNK_WRITE(V40) {
  alertEnabled = param.asInt();
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("  Alert Mode:   ");
  lcd.setCursor(0,1); lcd.print(alertEnabled ? "    ENABLED     " : "    DISABLED    ");
  lcdUpdateTime = millis() + 2000;
}
BLYNK_WRITE(V41) {
  pirAlertEnabled = param.asInt() == 1;
  if (!pirAlertEnabled) {
    pirAlertActive = false;
    if (sysState == STATE_ALERT_PIR) {
      sysState = STATE_NORMAL; normalState = true;
      ledAllOff(); digitalWrite(BUZZER_PIN, LOW); beepLen = 0;
    }
  }
  Serial.printf("[PIR] %s\n", pirAlertEnabled ? "ON" : "OFF");
}
BLYNK_WRITE(V70) {
  int s = constrain(param.asInt(), 5, 300);
  autoLockDelay = (unsigned long)s * 1000;
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("  Auto-lock:    ");
  char buf[17]; snprintf(buf, sizeof(buf), "  After %3ds    ", s);
  lcd.setCursor(0,1); lcd.print(buf);
  lcdUpdateTime = millis() + 2000;
}
BLYNK_WRITE(V71) {
  autoLockEnabled = param.asInt();
  if (!autoLockEnabled) doorOpenedAt = 0;
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("  Auto-lock:    ");
  lcd.setCursor(0,1); lcd.print(autoLockEnabled ? "    ENABLED     " : "    DISABLED    ");
  lcdUpdateTime = millis() + 2000;
}

// =========================================================
// RESTORE LCD
// =========================================================
void restoreNormalLCD() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("  System Ready  ");
  lcd.setCursor(0,1); lcd.print(" Press # to open");
  lcdUpdateTime = millis() + 2000;
  displayMode = 0; dispT0 = 0; dispLastSec = -1;
}

// =========================================================
// ĐÓNG / MỞ CỬA
// =========================================================
void closeDoor(bool byAutoLock) {
  if (!isDoorOpen) return;
  isDoorOpen = false; doorOpenedAt = 0;
  myServo.write(0);
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("  Smart  Lock   ");
  lcd.setCursor(0,1); lcd.print(byAutoLock ? "  Auto  LOCKED  " : "  Door: CLOSED  ");
  lcdUpdateTime = millis() + 2000;
  Blynk.virtualWrite(V30, "Closed");
  if (byAutoLock) beepWarn();
  Serial.printf("[DOOR] %s\n", byAutoLock ? "Auto-locked" : "Closed");
}

void openDoor() {
  isDoorOpen = true; myServo.write(90); beepOK(); ledGreen(1000);
  if (autoLockEnabled) doorOpenedAt = millis();
  lightOn = false;
  lcd.clear();
  lcd.setCursor(0,0); lcd.write(3); lcd.print(" ACCESS OK!    ");
  lcd.setCursor(0,1); lcd.print("   Door  OPEN   ");
  lcdUpdateTime = millis() + 2000;
  Blynk.virtualWrite(V30, "Opened");
  Serial.println("[DOOR] Opened");
}

void openDoorWithName(int fingerId) {
  isDoorOpen = true; myServo.write(90); beepOK(); ledGreen(1000);
  if (autoLockEnabled) doorOpenedAt = millis();
  lightOn = false;
  String userName = loadFingerName(fingerId);
  lcd.clear();
  lcd.setCursor(0,0); lcd.write(2); lcd.print(" ");
  lcd.print(userName.substring(0, 13));
  lcd.setCursor(0,1); lcd.print("   Welcome! :)  ");
  lcdUpdateTime = millis() + 3000;
  Blynk.virtualWrite(V30, "Opened");
  Serial.printf("[FINGER] OK-%s(ID:%d)\n", userName.c_str(), fingerId);
}

// =========================================================
// SAI MẬT KHẨU
// =========================================================
void wrongPass() {
  passFailCount++; keypadFailCount++;
  beepFail(); ledRed(600);
  if (keypadFailCount >= KEYPAD_MAX_FAIL) {
    keypadLocked = true; keypadLockedAt = millis();
    keypadFailCount = 0; inputPass = "";
    sysState = STATE_NORMAL; normalState = true;
    lcd.clear();
    lcd.setCursor(0,0); lcd.print(" Keypad LOCKED! ");
    lcd.setCursor(0,1); lcd.print("   Wait  30s    ");
    lcdUpdateTime = millis() + 2000;
    return;
  }
  if (passFailCount >= MAX_PASS_FAIL) {
    sysState = STATE_ALERT_PASS; alertStartTime = millis(); return;
  }
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("  Access DENIED ");
  lcd.setCursor(0,1); lcd.print(" Wrong password ");
  lcdUpdateTime = millis() + 1500;
}

// =========================================================
// VÂN TAY — throttle 100ms
// =========================================================
void getFingerprintID() {
  if (millis() - lastFingerCheck < FINGER_CHECK_INTERVAL) return;
  lastFingerCheck = millis();

  uint8_t p = finger.getImage();

  if (fingerProcessed) {
    if (p == FINGERPRINT_NOFINGER) { fingerPresent = false; fingerProcessed = false; }
    return;
  }
  if (p == FINGERPRINT_NOFINGER) { fingerPresent = false; return; }

  if (!fingerPresent) {
    fingerPresent = true; fingerProcessed = true;
    if (p != FINGERPRINT_OK) {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("  Scan  failed  ");
      lcd.setCursor(0,1); lcd.print("  Try  again... ");
      beepFail(); ledRed(600); lcdUpdateTime = millis() + 1000; return;
    }
    if (isFingerLocked) {
      lcd.clear(); lcd.setCursor(0,0); lcd.write(1); lcd.print(" Finger LOCKED ");
      lcd.setCursor(0,1); lcd.print("Use keypad pass ");
      beepWarn(); ledRed(800); lcdUpdateTime = millis() + 2000; return;
    }
    lcd.clear(); lcd.setCursor(0,0); lcd.write(2); lcd.print(" Fingerprint   ");
    lcd.setCursor(0,1); lcd.print("  Checking...   ");
    p = finger.image2Tz();
    if (p != FINGERPRINT_OK) {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("  Scan  failed  ");
      lcd.setCursor(0,1); lcd.print("  Try  again... ");
      beepFail(); ledRed(600); lcdUpdateTime = millis() + 1000; return;
    }
    p = finger.fingerFastSearch();
    if (p == FINGERPRINT_OK) {
      fingerFailCount = 0; openDoorWithName(finger.fingerID);
    } else {
      fingerFailCount++; beepWarn();
      if (fingerFailCount >= MAX_FINGER_FAIL) {
        isFingerLocked = true;
        Blynk.logEvent("fingerprint_alert", "Finger LOCKED!");
        lcd.clear(); lcd.setCursor(0,0); lcd.write(1); lcd.print(" Finger LOCKED ");
        lcd.setCursor(0,1); lcd.print("Use keypad pass ");
        ledRed(1000); lcdUpdateTime = millis() + 2500;
      } else {
        // ✅ snprintf thay vì String concat
        char msg[40]; snprintf(msg, sizeof(msg), "Wrong! Fail:%d/%d", fingerFailCount, MAX_FINGER_FAIL);
        Blynk.logEvent("fingerprint_alert", msg);
        char failBuf[17]; snprintf(failBuf, sizeof(failBuf), "Fail: %d/%d", fingerFailCount, MAX_FINGER_FAIL);
        lcd.clear(); lcd.setCursor(0,0); lcd.print(" Not recognized ");
        lcd.setCursor(0,1); lcd.print(failBuf);
        ledRed(600); lcdUpdateTime = millis() + 1500;
      }
    }
  }
}

// =========================================================
// ENROLL STATE MACHINE — non-blocking
// =========================================================
unsigned long enrollStepTimer = 0;

void handleEnrollSM() {
  if (!isEnrolling) return;
  uint8_t p;

  switch (enrollStep) {

    case ENROLL_CHECK_DUP_WAIT:
      lcd.setCursor(0,0); lcd.print("Checking dup... ");
      lcd.setCursor(0,1); lcd.print("Place finger... ");
      Blynk.virtualWrite(V103, "Checking dup...");
      enrollStepTimer = millis();
      enrollStep = ENROLL_CHECK_DUP_PROCESS;
      break;

    case ENROLL_CHECK_DUP_PROCESS:
      if (millis() - enrollStepTimer > ENROLL_TIMEOUT) {
        lcd.clear(); lcd.setCursor(0,0); lcd.print("   Timeout!     ");
        lcd.setCursor(0,1); lcd.print("                ");
        Blynk.virtualWrite(V103, "TIMEOUT");
        ledRed(1000); lcdUpdateTime = millis() + 1500;
        enrollStep = ENROLL_DONE; break;
      }
      p = finger.getImage();
      if (p != FINGERPRINT_OK) break;
      if (finger.image2Tz(1) != FINGERPRINT_OK) break;
      if (finger.fingerFastSearch() == FINGERPRINT_OK) {
        int dupID = finger.fingerID;
        char msg[40]; snprintf(msg, sizeof(msg), "Already ID:%d", dupID);
        Blynk.virtualWrite(V103, msg);
        lcd.clear(); lcd.setCursor(0,0); lcd.print(" Already exist! ");
        lcd.setCursor(0,1); lcd.print(msg);
        ledRed(1000); lcdUpdateTime = millis() + 2000;
        enrollStep = ENROLL_DONE; break;
      }
      Blynk.virtualWrite(V103, "Step1 OK-Remove");
      lcd.clear();
      lcd.setCursor(0,0); lcd.print("Enroll step 1/2 ");
      lcd.setCursor(0,1); lcd.print("Remove finger.. ");
      enrollStepTimer = millis();
      enrollStep = ENROLL_WAIT_REMOVE;
      break;

    case ENROLL_WAIT_REMOVE:
      if (millis() - enrollStepTimer < 2000) break;
      p = finger.getImage();
      if (p != FINGERPRINT_NOFINGER && millis() - enrollStepTimer < 2000 + FINGER_WAIT_TIMEOUT) break;
      lcd.clear();
      lcd.setCursor(0,0); lcd.print("Enroll step 2/2 ");
      lcd.setCursor(0,1); lcd.print("Place again...  ");
      Blynk.virtualWrite(V103, "Step 2: Place");
      enrollStepTimer = millis();
      enrollStep = ENROLL_STEP2_WAIT;
      break;

    case ENROLL_STEP2_WAIT:
      if (millis() - enrollStepTimer > ENROLL_TIMEOUT) {
        lcd.clear(); lcd.setCursor(0,0); lcd.print("   Timeout!     ");
        lcd.setCursor(0,1); lcd.print("                ");
        Blynk.virtualWrite(V103, "TIMEOUT step2");
        ledRed(1000); lcdUpdateTime = millis() + 1500;
        enrollStep = ENROLL_DONE; break;
      }
      p = finger.getImage();
      if (p != FINGERPRINT_OK) break;
      enrollStep = ENROLL_STEP2_PROCESS;
      break;

    case ENROLL_STEP2_PROCESS:
      if (finger.image2Tz(2) != FINGERPRINT_OK) {
        lcd.clear(); lcd.setCursor(0,0); lcd.print("  Scan  failed  ");
        lcd.setCursor(0,1); lcd.print("  Try  again... ");
        Blynk.virtualWrite(V103, "Scan fail step2");
        ledRed(1000); lcdUpdateTime = millis() + 1500;
        enrollStep = ENROLL_DONE; break;
      }
      if (finger.createModel() == FINGERPRINT_OK && finger.storeModel(idFromBlynk) == FINGERPRINT_OK) {
        String nameToSave = pendingName.length() > 0 ? pendingName : ("ID:" + String(idFromBlynk));
        saveFingerName(idFromBlynk, nameToSave);
        pendingName = "";
        char msg[40]; snprintf(msg, sizeof(msg), "OK: %s (ID:%d)", nameToSave.substring(0,20).c_str(), idFromBlynk);
        Blynk.virtualWrite(V103, msg);
        lcd.clear();
        lcd.setCursor(0,0); lcd.write(3); lcd.print(" Enroll OK!    ");
        lcd.setCursor(0,1); lcd.print(nameToSave.substring(0, 16));
        beepOK(); ledGreen(1500); lcdUpdateTime = millis() + 2500;
      } else {
        Blynk.virtualWrite(V103, "FAILED-store err");
        lcd.clear(); lcd.setCursor(0,0); lcd.print("  Enroll FAIL!  ");
        lcd.setCursor(0,1); lcd.print("  Try again...  ");
        ledRed(1500); lcdUpdateTime = millis() + 1500;
      }
      enrollStep = ENROLL_DONE;
      break;

    case ENROLL_DONE:
      if (millis() < lcdUpdateTime) break;
      isEnrolling = false; normalState = true;
      sysState = STATE_NORMAL; enrollStep = ENROLL_IDLE;
      Blynk.virtualWrite(V101, 0);
      Blynk.virtualWrite(V103, "READY");
      restoreNormalLCD();
      break;

    default: break;
  }
}

// =========================================================
// DELETE
// =========================================================
void deleteFingerprint(int id) {
  if (finger.deleteModel(id) == FINGERPRINT_OK) {
    String deletedName = loadFingerName(id);
    deleteFingerName(id);
    char msg[40]; snprintf(msg, sizeof(msg), "DELETED:%s(ID:%d)", deletedName.substring(0,16).c_str(), id);
    Blynk.virtualWrite(V103, msg);
    lcd.clear(); lcd.setCursor(0,0); lcd.print("  Deleted OK!   ");
    lcd.setCursor(0,1); lcd.print(deletedName.substring(0, 16));
    beepDelete(); ledRed(1000);
  } else {
    Blynk.virtualWrite(V103, "ERROR: not found");
    lcd.clear(); lcd.setCursor(0,0); lcd.print("  Delete ERROR! ");
    lcd.setCursor(0,1); lcd.print("  ID not found  ");
    beepFail(); ledRed(800);
  }
  lcdUpdateTime = millis() + 2000;
  isDeleting = false; normalState = true; sysState = STATE_NORMAL;
  Blynk.virtualWrite(V102, 0); Blynk.virtualWrite(V103, "READY");
}

// =========================================================
// ĐÈN TỰ ĐỘNG (PIR + tối)
// =========================================================
void handleAutoLight(int pirVal, int ldrVal) {
  if (isDoorOpen) return;
  if (pirVal == HIGH && ldrVal < LDR_DARK_THRESHOLD) {
    if (pirAlertEnabled && !pirAlertActive && sysState == STATE_NORMAL) {
      pirAlertActive = true; pirAlertStart = millis();
      sysState = STATE_ALERT_PIR; normalState = false; beepLen = 0; return;
    }
    if (!pirAlertEnabled && !lightOn) {
      lightOn = true; lightOnAt = millis(); ledGreenOn();
    }
  }
  if (lightOn && millis() - lightOnAt >= LIGHT_DURATION) {
    lightOn = false; ledAllOff();
  }
}

// =========================================================
// NORMAL DISPLAY
// =========================================================
void handleNormalDisplay() {
  // Update giây liên tục trong TIME mode
  if (millis() < lcdUpdateTime) {
    if (displayMode == 0) {
      int ss = timeClient.getSeconds();
      if (ss != dispLastSec) {
        dispLastSec = ss;
        char buf[17];
        snprintf(buf, sizeof(buf), "   %02d:%02d:%02d   ", timeClient.getHours(), timeClient.getMinutes(), ss);
        lcd.setCursor(0,0); lcd.print("   -- TIME --   ");
        lcd.setCursor(0,1); lcd.print(buf);
      }
    }
    return;
  }

  switch (displayMode) {
    case 0: {
      if (dispT0 == 0) dispT0 = millis();
      int ss = timeClient.getSeconds();
      if (ss != dispLastSec) {
        dispLastSec = ss;
        char buf[17];
        snprintf(buf, sizeof(buf), "   %02d:%02d:%02d   ", timeClient.getHours(), timeClient.getMinutes(), ss);
        lcd.setCursor(0,0); lcd.print("   -- TIME --   ");
        lcd.setCursor(0,1); lcd.print(buf);
      }
      lcdUpdateTime = millis() + 1000;
      if (millis() - dispT0 >= 8000) {
        dispT0 = 0; dispLastSec = -1; displayMode = 1; lcdUpdateTime = 0;
      }
      break;
    }
    case 1: {
      time_t e = timeClient.getEpochTime();
      struct tm* ti = gmtime(&e);
      char buf[17];
      snprintf(buf, sizeof(buf), "  %02d/%02d/%04d  ", ti->tm_mday, ti->tm_mon+1, ti->tm_year+1900);
      lcd.setCursor(0,0); lcd.print("   -- DATE --   ");
      lcd.setCursor(0,1); lcd.print(buf);
      displayMode = 2; lcdUpdateTime = millis() + 4000;
      break;
    }
    case 2: {
      char line0[17], line1[17];
      snprintf(line0, sizeof(line0), " Temp:%.1fC     ", weatherTemp);
      snprintf(line1, sizeof(line1), "%-16.16s", weatherDesc.c_str());
      lcd.setCursor(0,0); lcd.print(line0);
      lcd.setCursor(0,1); lcd.print(line1);
      displayMode = 0; dispT0 = 0; lcdUpdateTime = millis() + 4000;
      break;
    }
  }
}

// =========================================================
// ALERT HANDLERS
// =========================================================
void handleGasAlert(int gasVal) {
  if (matrixLastText != "GAS") {  // ✅ dùng biến đơn giản thay matrix
    matrixLastText = "GAS";
    lcd.clear();
    lcd.setCursor(0,0); lcd.write(0); lcd.print(" !!! Alert !!!!");
    lcd.setCursor(0,1); lcd.write(0); lcd.print(" GAS DETECTED!");
    char msg[40]; snprintf(msg, sizeof(msg), "GAS! Value:%d", gasVal);
    Blynk.logEvent("gas_alert", msg);
  }
  if (millis() - lastBlink > 150) {
    lastBlink = millis(); blinkState = !blinkState;
    if (blinkState) ledRedOn(); else ledAllOff();
    digitalWrite(BUZZER_PIN, blinkState ? HIGH : LOW);
  }
  if (millis() - alertStartTime >= ALERT_DURATION) {
    if (analogRead(MQ2_PIN) > GAS_THRESHOLD) { alertStartTime = millis(); }
    else {
      sysState = STATE_NORMAL; normalState = true;
      ledAllOff(); digitalWrite(BUZZER_PIN, LOW); beepLen = 0;
      matrixLastText = ""; lcd.clear(); displayMode = 0;
      dispT0 = 0; dispLastSec = -1; lcdUpdateTime = 0;
    }
  }
}

void handleAlertPass() {
  if (matrixLastText != "PASS") {
    matrixLastText = "PASS";
    lcd.clear();
    lcd.setCursor(0,0); lcd.write(0); lcd.print(" !!! Alert !!!!");
    lcd.setCursor(0,1); lcd.print("  Too many fail ");
    Blynk.logEvent("wrong_pass_alert", "Too many wrong pass!");
  }
  if (millis() - lastBlink > 200) {
    lastBlink = millis(); blinkState = !blinkState;
    if (blinkState) ledRedOn(); else ledAllOff();
    digitalWrite(BUZZER_PIN, blinkState ? HIGH : LOW);
  }
  if (millis() - alertStartTime >= ALERT_DURATION) {
    passFailCount = 0; sysState = STATE_NORMAL; normalState = true;
    ledAllOff(); digitalWrite(BUZZER_PIN, LOW); beepLen = 0;
    matrixLastText = ""; lcd.clear(); displayMode = 0;
    dispT0 = 0; dispLastSec = -1; lcdUpdateTime = 0;
  }
}

void handlePIRAlert() {
  if (matrixLastText != "PIR") {
    matrixLastText = "PIR";
    lcd.clear();
    lcd.setCursor(0,0); lcd.write(0); lcd.print(" !!! Alert !!!!");
    lcd.setCursor(0,1); lcd.write(0); lcd.print(" INTRUDER DET! ");
    Blynk.logEvent("pir_alert", "Motion in dark!");
  }
  if (millis() - lastBlink > 150) {
    lastBlink = millis(); blinkState = !blinkState;
    if (blinkState) ledRedOn(); else ledAllOff();
    digitalWrite(BUZZER_PIN, blinkState ? HIGH : LOW);
  }
  if (millis() - pirAlertStart >= ALERT_DURATION) {
    sysState = STATE_NORMAL; normalState = true; pirAlertActive = false;
    ledAllOff(); digitalWrite(BUZZER_PIN, LOW); beepLen = 0;
    matrixLastText = ""; lcd.clear(); displayMode = 0;
    dispT0 = 0; dispLastSec = -1; lcdUpdateTime = 0;
  }
}

// =========================================================
// SETUP
// =========================================================
void setup() {
  Serial.begin(115200); delay(300);
  pinMode(MQ2_PIN, INPUT); pinMode(PIR_PIN, INPUT); pinMode(LDR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT); pinMode(LED_RED, OUTPUT); pinMode(LED_GREEN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); ledAllOff();
  myServo.attach(SERVO_PIN); myServo.write(0);

  Wire.begin(21, 22); lcd.init(); lcd.backlight();
  lcd.createChar(0, bellChar); lcd.createChar(1, lockChar);
  lcd.createChar(2, personChar); lcd.createChar(3, checkChar);
  lcd.clear(); lcd.setCursor(0,0); lcd.print("  Smart  Lock   ");
  lcd.setCursor(0,1); lcd.print("  Starting...   ");

  // ✅ Matrix đã bỏ hoàn toàn — không init, không dùng

  mySerial.begin(57600, SERIAL_8N1, 16, 17); finger.begin(57600);
  if (!finger.verifyPassword()) {
    lcd.clear(); lcd.setCursor(0,0); lcd.print("  Finger sensor ");
    lcd.setCursor(0,1); lcd.print("  NOT  FOUND!   "); delay(2000);
  }

  keypad.setDebounceTime(20);
  prefs.begin("smartlock", false);
  password = prefs.getString("pass", "5678");

  lcd.clear(); lcd.setCursor(0,0); lcd.print(" Connecting...  ");
  lcd.setCursor(0,1); lcd.print(ssid);
  WiFi.begin(ssid, pass);
  int wt = 0;
  while (WiFi.status() != WL_CONNECTED && wt < 20) { delay(500); wt++; }
  if (WiFi.status() == WL_CONNECTED) {
    lcd.clear(); lcd.setCursor(0,0); lcd.write(3); lcd.print(" WiFi  OK      ");
    lcd.setCursor(0,1); lcd.print(WiFi.localIP().toString()); delay(1000);
  } else {
    lcd.clear(); lcd.setCursor(0,0); lcd.print("  WiFi  FAILED  ");
    lcd.setCursor(0,1); lcd.print("  Offline mode  "); delay(1000);
  }

  Blynk.config(auth); Blynk.connect();
  timeClient.begin(); timeClient.update();

  // ✅ Timer tách biệt, tần suất tối ưu
  timer.setInterval(10000L, sendBlynkSensors);    // Blynk sensors: 10s
  timer.setInterval(30000L, sendTelemetryToBackend); // Backend telemetry: 30s
  timer.setInterval(600000L, getWeatherData);     // Weather: 10 phút

  getWeatherData(); // Lần đầu khởi động

  Blynk.virtualWrite(V30, "Closed");
  Blynk.virtualWrite(V40, 1);
  Blynk.virtualWrite(V71, 1);

  lcd.clear(); lcd.setCursor(0,0); lcd.print("  System Ready  ");
  lcd.setCursor(0,1); lcd.print(" Press # to open");
  delay(500);
  Serial.println("--- SYSTEM READY v9.3 ---");
}

// =========================================================
// LOOP — non-blocking
// =========================================================
unsigned long lastNTPUpdate = 0;
const unsigned long NTP_INTERVAL = 3600000; // 1h

void loop() {
  Blynk.run();
  timer.run();

  // NTP 1h/lần
  if (millis() - lastNTPUpdate >= NTP_INTERVAL) {
    timeClient.update(); lastNTPUpdate = millis();
  }

  updateLED();
  updateBuzzer();

  // AUTO-LOCK
  if (autoLockEnabled && isDoorOpen && doorOpenedAt > 0 &&
      millis() - doorOpenedAt >= autoLockDelay) {
    closeDoor(true);
  }

  // ENROLL state machine
  if (isEnrolling) { handleEnrollSM(); return; }

  // DELETE
  if (isDeleting)  { deleteFingerprint(idFromBlynk); return; }

  int gasVal = analogRead(MQ2_PIN);
  int ldrVal = analogRead(LDR_PIN);
  int pirVal = digitalRead(PIR_PIN);

  // GAS ALERT
  if (gasVal > GAS_THRESHOLD && sysState != STATE_ALERT_GAS && alertEnabled) {
    sysState = STATE_ALERT_GAS; alertStartTime = millis(); normalState = false; beepLen = 0;
  }
  if (sysState == STATE_ALERT_GAS)  { handleGasAlert(gasVal); return; }

  handleAutoLight(pirVal, ldrVal);

  if (sysState == STATE_ALERT_PIR)  { handlePIRAlert();  return; }
  if (sysState == STATE_ALERT_PASS) { handleAlertPass(); return; }

  if (sysState == STATE_NORMAL) getFingerprintID();

  // =========================================================
  // KEYPAD
  // =========================================================
  char key = keypad.getKey();

  if (keypadLocked) {
    if (millis() - keypadLockedAt >= KEYPAD_LOCK_DURATION) {
      keypadLocked = false; keypadFailCount = 0;
      lcd.clear(); lcd.setCursor(0,0); lcd.print("  Keypad Ready  ");
      lcd.setCursor(0,1); lcd.print("                ");
      lcdUpdateTime = millis() + 1500;
    } else if (key) {
      unsigned long rem = (KEYPAD_LOCK_DURATION - (millis() - keypadLockedAt)) / 1000 + 1;
      lcd.clear(); lcd.setCursor(0,0); lcd.print(" Keypad LOCKED! ");
      char buf[17]; snprintf(buf, sizeof(buf), "  Wait: %2lus     ", rem);
      lcd.setCursor(0,1); lcd.print(buf);
      lcdUpdateTime = millis() + 1000;
    }
  }

  if (!keypadLocked && key && millis() - lastKeyTime > 300) {
    lastKeyTime = millis(); beepKey();

    if (key == 'A') {
      sysState = STATE_NORMAL; normalState = true;
      inputPass = ""; newPassBuf = ""; enrollIDStr = "";
      passFailCount = 0; fingerFailCount = 0; keypadFailCount = 0;
      isFingerLocked = false; isDoorOpen = false;
      fingerPresent = false; fingerProcessed = false;
      doorOpenedAt = 0; lightOn = false; pirAlertActive = false;
      myServo.write(0); ledAllOff();
      digitalWrite(BUZZER_PIN, LOW); beepLen = 0;
      matrixLastText = "";
      Blynk.virtualWrite(V30, "Closed");
      lcd.clear();
      lcd.setCursor(0,0); lcd.print("  System Reset  ");
      lcd.setCursor(0,1); lcd.print("   All  Clear   ");
      lcdUpdateTime = millis() + 1500;
      displayMode = 0; dispT0 = 0; dispLastSec = -1;
    }
    else if (key == 'B') {
      if (sysState == STATE_NORMAL) {
        sysState = STATE_CHANGE_PASS; normalState = false; cpStep = CP_OLD; inputPass = "";
        lcd.clear(); lcd.setCursor(0,0); lcd.write(1); lcd.print(" Change  Pass  ");
        lcd.setCursor(0,1); lcd.print("Old pass:       ");
      }
    }
    else if (key == 'C') {
      if (sysState == STATE_NORMAL && !isEnrolling && !isDeleting) {
        sysState = STATE_ENROLL_AUTH; normalState = false; inputPass = "";
        lcd.clear(); lcd.setCursor(0,0); lcd.write(1); lcd.print(" Enroll Finger ");
        lcd.setCursor(0,1); lcd.print("Pass:           ");
      }
    }
    else if (key == 'D') {
      if (!pirAlertEnabled) {
        pirAlertEnabled = true; Blynk.virtualWrite(VPIN_PIR_ALERT_MODE, 1);
        lcd.clear(); lcd.setCursor(0,0); lcd.print(" PIR  Alert:    ");
        lcd.setCursor(0,1); lcd.print("    ENABLED     ");
        lcdUpdateTime = millis() + 2000; beepWarn();
      } else {
        sysState = STATE_KEY_D_AUTH; normalState = false; inputPass = "";
        lcd.clear(); lcd.setCursor(0,0); lcd.write(1); lcd.print(" Disable Alert ");
        lcd.setCursor(0,1); lcd.print("Pass:           ");
      }
    }
    else if (key == '*') {
      inputPass = ""; enrollIDStr = "";
      if      (sysState == STATE_ENTER_PASS)   { lcd.setCursor(0,1); lcd.print("Pass:           "); }
      else if (sysState == STATE_ENROLL_AUTH)  { lcd.setCursor(0,1); lcd.print("Pass:           "); }
      else if (sysState == STATE_KEY_D_AUTH)   { lcd.setCursor(0,1); lcd.print("Pass:           "); }
      else if (sysState == STATE_ENROLL_ID)    { lcd.setCursor(0,1); lcd.print("ID(1-127):      "); }
      else if (sysState == STATE_CHANGE_PASS)  {
        lcd.setCursor(0,1);
        if      (cpStep == CP_OLD)     lcd.print("Old pass:       ");
        else if (cpStep == CP_NEW)     lcd.print("New pass:       ");
        else if (cpStep == CP_CONFIRM) lcd.print("Confirm:        ");
      }
    }
    else if (key == '#') {
      if (sysState == STATE_KEY_D_AUTH) {
        if (inputPass == password) {
          pirAlertEnabled = false; Blynk.virtualWrite(VPIN_PIR_ALERT_MODE, 0);
          lcd.clear(); lcd.setCursor(0,0); lcd.print(" PIR  Alert:    ");
          lcd.setCursor(0,1); lcd.print("    DISABLED    ");
          beepOK(); ledGreen(1000);
        } else {
          lcd.clear(); lcd.setCursor(0,0); lcd.print(" Wrong password ");
          lcd.setCursor(0,1); lcd.print(" Alert stays ON ");
          beepFail(); ledRed(600);
        }
        inputPass = ""; sysState = STATE_NORMAL; normalState = true;
        lcdUpdateTime = millis() + 2000;
      }
      else if (sysState == STATE_ENROLL_AUTH) {
        if (inputPass == password) {
          sysState = STATE_ENROLL_ID; enrollIDStr = ""; inputPass = "";
          lcd.clear(); lcd.setCursor(0,0); lcd.print(" Enroll Finger  ");
          lcd.setCursor(0,1); lcd.print("ID(1-127):      ");
        } else {
          beepFail(); ledRed(600);
          lcd.clear(); lcd.setCursor(0,0); lcd.print(" Wrong password ");
          lcd.setCursor(0,1); lcd.print("  Try  again... ");
          inputPass = ""; sysState = STATE_NORMAL; normalState = true;
          lcdUpdateTime = millis() + 1500;
        }
      }
      else if (sysState == STATE_ENROLL_ID) {
        int enrollID = enrollIDStr.toInt();
        if (enrollIDStr.length() == 0 || enrollID < 1 || enrollID > 127) {
          beepFail(); ledRed(600);
          lcd.clear(); lcd.setCursor(0,0); lcd.print("  Invalid  ID!  ");
          lcd.setCursor(0,1); lcd.print(" Range: 1 - 127 ");
          enrollIDStr = ""; lcdUpdateTime = millis() + 1500;
        } else {
          pendingName = ""; idFromBlynk = enrollID;
          isEnrolling = true; normalState = false; sysState = STATE_NORMAL;
          enrollStep = ENROLL_CHECK_DUP_WAIT;
          lcd.clear(); lcd.setCursor(0,0); lcd.print("  Enrolling...  ");
          char buf[17]; snprintf(buf, sizeof(buf), "   ID: %-3d      ", enrollID);
          lcd.setCursor(0,1); lcd.print(buf); enrollIDStr = "";
        }
      }
      else if (sysState == STATE_NORMAL || sysState == STATE_ENTER_PASS) {
        if (inputPass == password) {
          passFailCount = 0; keypadFailCount = 0; isFingerLocked = false; openDoor();
        } else { wrongPass(); }
        inputPass = "";
        if (sysState != STATE_ALERT_PASS) { sysState = STATE_NORMAL; normalState = true; }
      }
      else if (sysState == STATE_CHANGE_PASS) {
        if (cpStep == CP_OLD) {
          if (inputPass == password) {
            cpStep = CP_NEW; inputPass = "";
            lcd.clear(); lcd.setCursor(0,0); lcd.write(1); lcd.print(" Change  Pass  ");
            lcd.setCursor(0,1); lcd.print("New pass:       ");
          } else {
            lcd.clear(); lcd.setCursor(0,0); lcd.print(" Wrong password ");
            lcd.setCursor(0,1); lcd.print("  Try  again... ");
            beepFail(); ledRed(600); inputPass = "";
            sysState = STATE_NORMAL; normalState = true; lcdUpdateTime = millis() + 1500;
          }
        } else if (cpStep == CP_NEW) {
          if (inputPass.length() > 0) {
            newPassBuf = inputPass; inputPass = ""; cpStep = CP_CONFIRM;
            lcd.clear(); lcd.setCursor(0,0); lcd.write(1); lcd.print(" Change  Pass  ");
            lcd.setCursor(0,1); lcd.print("Confirm:        ");
          }
        } else if (cpStep == CP_CONFIRM) {
          if (inputPass == newPassBuf) {
            password = inputPass; prefs.putString("pass", password);
            beepOK(); ledGreen(1500);
            lcd.clear(); lcd.setCursor(0,0); lcd.write(3); lcd.print(" Pass changed! ");
            lcd.setCursor(0,1); lcd.print("  New pass  OK  ");
          } else {
            beepFail(); ledRed(600);
            lcd.clear(); lcd.setCursor(0,0); lcd.print("  Not  matched! ");
            lcd.setCursor(0,1); lcd.print("  Try  again... ");
          }
          inputPass = ""; newPassBuf = "";
          sysState = STATE_NORMAL; normalState = true;
          lcdUpdateTime = millis() + 1500; displayMode = 0;
        }
      }
    }
    else if (key >= '0' && key <= '9') {
      if (sysState == STATE_ENROLL_ID) {
        if (enrollIDStr.length() < 3) {
          enrollIDStr += key;
          lcd.setCursor(0,0); lcd.print(" Enroll Finger  ");
          lcd.setCursor(0,1); lcd.print("ID: "); lcd.print(enrollIDStr);
          for (int i = enrollIDStr.length(); i < 12; i++) lcd.print(" ");
        }
      } else {
        if (sysState == STATE_NORMAL) {
          sysState = STATE_ENTER_PASS; normalState = false; inputPass = "";
          lcd.clear(); lcd.setCursor(0,0); lcd.write(1); lcd.print(" Enter Password");
          lcd.setCursor(0,1); lcd.print("Pass:           ");
        }
        if (sysState == STATE_ENTER_PASS || sysState == STATE_CHANGE_PASS
            || sysState == STATE_ENROLL_AUTH || sysState == STATE_KEY_D_AUTH) {
          inputPass += key;
          int len = inputPass.length();
          // ✅ Không tạo String stars, dùng trực tiếp
          lcd.setCursor(0,0);
          if      (sysState == STATE_ENTER_PASS)  { lcd.write(1); lcd.print(" Enter Password"); }
          else if (sysState == STATE_ENROLL_AUTH) { lcd.write(1); lcd.print(" Enroll Finger "); }
          else if (sysState == STATE_KEY_D_AUTH)  { lcd.write(1); lcd.print(" Disable Alert "); }
          else {
            lcd.write(1);
            if      (cpStep == CP_OLD)     lcd.print(" Change  Pass  ");
            else if (cpStep == CP_NEW)     lcd.print(" New  Password ");
            else if (cpStep == CP_CONFIRM) lcd.print(" Confirm  Pass ");
          }
          lcd.setCursor(0,1);
          if      (sysState == STATE_ENTER_PASS)                          lcd.print("Pass: ");
          else if (sysState == STATE_ENROLL_AUTH)                         lcd.print("Pass: ");
          else if (sysState == STATE_KEY_D_AUTH)                          lcd.print("Pass: ");
          else if (sysState == STATE_CHANGE_PASS && cpStep == CP_OLD)     lcd.print("Old:  ");
          else if (sysState == STATE_CHANGE_PASS && cpStep == CP_NEW)     lcd.print("New:  ");
          else if (sysState == STATE_CHANGE_PASS && cpStep == CP_CONFIRM) lcd.print("Cnf:  ");
          // In dấu sao trực tiếp, tối đa 9 ký tự
          int show = len > 9 ? 9 : len;
          int start = len > 9 ? len - 9 : 0;
          (void)start; // start không dùng trực tiếp nhưng giữ logic
          for (int i = 0; i < show; i++) lcd.print('*');
          for (int i = show; i < 10; i++) lcd.print(' ');
        }
      }
    }
  }

  if (sysState == STATE_NORMAL && normalState) handleNormalDisplay();

  // ✅ Console log 10s/lần thay vì 2s
  if (millis() - lastConsoleTime >= CONSOLE_INTERVAL) {
    lastConsoleTime = millis();
    Serial.printf("[S] gas=%d ldr=%d pir=%d lt=%s door=%s al=%s(%lus) pir=%s kp=%s\n",
      gasVal, ldrVal, pirVal,
      lightOn         ? "ON"  : "off",
      isDoorOpen      ? "OPEN": "cls",
      autoLockEnabled ? "ON"  : "off", autoLockDelay/1000,
      pirAlertEnabled ? "ON"  : "off",
      keypadLocked    ? "LCK" : "ok");
  }
}

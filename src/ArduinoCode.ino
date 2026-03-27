// =============================================
// =============================================

#define BLYNK_TEMPLATE_ID   "TMPL6p8KU2UbZ"
#define BLYNK_TEMPLATE_NAME "Project IOT102"
#define BLYNK_AUTH_TOKEN    "INnoYtJ6zm9Rj-05xWV1hMb4H-XxbTve"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
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
#define DATA_PIN    23
#define CS_PIN       5
#define CLK_PIN     18
#define MAX_DEVICES  4
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
// WEATHER
// =========================================================
float  weatherTemp = 0;
String weatherDesc = "---";
const String weatherURL = "http://api.openweathermap.org/data/2.5/weather?lat=10.8494&lon=106.7716&units=metric&appid=acd5b0ba3dd34ca92e4ff7463f46bca2";

// =========================================================
// OBJECTS
// =========================================================
LiquidCrystal_I2C lcd(0x27, 16, 2);
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
Servo myServo;
MD_Parola matrix = MD_Parola(MD_MAX72XX::FC16_HW, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
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
char ssid[] = "Bs11-1207";
char pass[] = "12071207";

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
  STATE_ALERT_GAS, STATE_ALERT_PIR, STATE_ALERT_PASS
};
SystemState sysState = STATE_NORMAL;

enum ChangePStep { CP_OLD, CP_NEW, CP_CONFIRM };
ChangePStep cpStep = CP_OLD;

// =========================================================
// GLOBAL VARIABLES
// =========================================================
String password   = "5678";
String inputPass  = "";
String newPassBuf = "";
String matrixLastText = "";

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
unsigned long lastConsoleTime = 0;

// display mode 0 — dùng global thay vì static local
int  dispLastSec = -1;
unsigned long dispT0 = 0;

// =========================================================
// NON-BLOCKING LED
// =========================================================
unsigned long ledTimer   = 0;
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

void beepKey()  { int s[] = {80, 0};              beepStart(s, 1); }
void beepOK()   { int s[] = {100, 80, 100, 0};    beepStart(s, 4); }
void beepFail() { int s[] = {600, 0};              beepStart(s, 1); }
void beepWarn() { int s[] = {80,60,80,60,80, 0};   beepStart(s, 6); }

// =========================================================
// MATRIX — PA_PRINT tĩnh
// =========================================================
void matrixShow(const char* txt) {
  if (matrixLastText == String(txt)) return;
  matrixLastText = String(txt);
  matrix.displayClear();
  matrix.displayText(txt, PA_CENTER, 0, 0, PA_PRINT, PA_PRINT);
  matrix.displayAnimate();
}
void matrixSetNormal() { matrixShow("Normal"); }
void matrixSetAlert()  { matrixShow("ALERT!!"); }

// =========================================================
// HELPER: flush Blynk sau khi thoát blocking
// =========================================================
void blynkFlush(int cycles = 10, int delayMs = 50) {
  for (int i = 0; i < cycles; i++) {
    Blynk.run();
    timer.run();
    delay(delayMs);
  }
}

// =========================================================
// HELPER: tick dùng trong các vòng while blocking
// =========================================================
void blockingTick() {
  Blynk.run();
  timer.run();
  timeClient.update();
  delay(20);
}

// =========================================================
// WEATHER
// =========================================================
void getWeatherData() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(weatherURL);
  http.setTimeout(5000);  // tránh treo nếu server chậm
  if (http.GET() != 200) { http.end(); return; }
  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, http.getString())) { http.end(); return; }

  weatherTemp    = doc["main"]["temp"].as<float>();
  String main    = doc["weather"][0]["main"].as<String>();
  int    clouds  = doc["clouds"]["all"].as<int>();

  if      (main == "Clear")                                     weatherDesc = "SUNNY";
  else if (main == "Clouds" && clouds <= 50)                    weatherDesc = "PARTLY CLOUDY";
  else if (main == "Clouds" && clouds > 50)                     weatherDesc = "CLOUDY";
  else if (main == "Rain"   || main == "Drizzle")               weatherDesc = "RAINY";
  else if (main == "Thunderstorm")                              weatherDesc = "STORM";
  else if (main == "Mist"   || main == "Haze" || main == "Fog") weatherDesc = "FOGGY";
  else                                                          weatherDesc = main;

  Blynk.virtualWrite(V50, weatherTemp);
  Blynk.virtualWrite(V51, weatherDesc);
  Serial.printf("[WEATHER] %.1f C  %s\n", weatherTemp, weatherDesc.c_str());
  http.end();
}

// =========================================================
// BLYNK TIMER
// =========================================================
void sendSensorData() {
  int g = analogRead(MQ2_PIN);
  Blynk.virtualWrite(V1, g);
  Blynk.virtualWrite(V2, g > GAS_THRESHOLD ? 255 : 0);
}

// =========================================================
// BLYNK VIRTUAL WRITES
// =========================================================
BLYNK_WRITE(V100) {
  idFromBlynk = param.asInt();
  Blynk.virtualWrite(V103, String("Selected ID: ") + idFromBlynk);
}
BLYNK_WRITE(V101) {
  // Chỉ nhận tín hiệu HIGH, và chỉ khi hệ thống đang rảnh
  // Guard isEnrolling/isDeleting ngăn Blynk reconnect trigger lại
  if (param.asInt() == 1 && !isEnrolling && !isDeleting && sysState == STATE_NORMAL) {
    isEnrolling = true; normalState = false;
    Blynk.virtualWrite(V103, "ENROLLING...");
    Blynk.virtualWrite(V101, 0);  // reset button ngay để tránh trigger lại khi reconnect
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("  Enrolling...  ");
    lcd.setCursor(0,1); lcd.print("ID:             ");
    lcd.setCursor(4,1); lcd.print(idFromBlynk);
  }
}
BLYNK_WRITE(V102) {
  if (param.asInt() == 1 && !isEnrolling && !isDeleting && sysState == STATE_NORMAL) {
    isDeleting = true; normalState = false;
    Blynk.virtualWrite(V103, "DELETING...");
    Blynk.virtualWrite(V102, 0);  // reset button ngay
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("  Deleting...   ");
    lcd.setCursor(0,1); lcd.print("ID:             ");
    lcd.setCursor(4,1); lcd.print(idFromBlynk);
  }
}
BLYNK_WRITE(V20) {
  if (param.asInt() == 1) {
    openDoor();
  } else {
    isDoorOpen = false; myServo.write(0);
    matrixLastText = ""; matrixSetNormal();
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("  Smart  Lock   ");
    lcd.setCursor(0,1); lcd.print("  Door: CLOSED  ");
    lcdUpdateTime = millis() + 2000;
    Blynk.virtualWrite(V30, "Closed");
    Serial.println("[BLYNK] Door closed");
  }
}
BLYNK_WRITE(V40) {
  alertEnabled = param.asInt();
  Serial.printf("[ALERT] %s\n", alertEnabled ? "BAT" : "TAT");
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("  Alert Mode:   ");
  lcd.setCursor(0,1); lcd.print(alertEnabled ? "    ENABLED     " : "    DISABLED    ");
  lcdUpdateTime = millis() + 2000;
}

// =========================================================
// RESTORE LCD sau enroll/delete — dùng chung
// =========================================================
void restoreNormalLCD() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("  System Ready  ");
  lcd.setCursor(0,1); lcd.print(" Press # to open");
  lcdUpdateTime = millis() + 2000;
}

// =========================================================
// CHECK VÂN TAY TRÙNG — quét nhanh trước khi enroll
// Trả về ID nếu đã tồn tại, -1 nếu chưa có
// =========================================================
int checkDuplicateFingerprint() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Checking dup... ");
  lcd.setCursor(0,1); lcd.print("Place finger... ");
  Blynk.virtualWrite(V103, "Checking duplicate...");

  unsigned long t = millis();
  int p = -1;
  while (p != FINGERPRINT_OK) {
    if (millis() - t > ENROLL_TIMEOUT) return -2;  // timeout
    p = finger.getImage();
    blockingTick();
  }
  if (finger.image2Tz(1) != FINGERPRINT_OK) return -1;
  if (finger.fingerFastSearch() == FINGERPRINT_OK) {
    return finger.fingerID;  // trùng với ID này
  }
  return -1;  // chưa có → ok để enroll
}

// =========================================================
// ENROLL — có check trùng, restore LCD ở mọi exit path
// =========================================================
bool enrollFingerprint(int id) {
  int p = -1;
  unsigned long t;

  // --- Kiểm tra vân tay đã tồn tại chưa ---
  int dupID = checkDuplicateFingerprint();
  if (dupID == -2) {
    // timeout khi check dup
    lcd.clear(); lcd.setCursor(0,0); lcd.print("   Timeout!     ");
    lcd.setCursor(0,1); lcd.print("                ");
    Blynk.virtualWrite(V103, "TIMEOUT - no finger");
    beepFail(); ledRed(1500);
    delay(1500); restoreNormalLCD();
    return false;
  }
  if (dupID >= 0) {
    // vân tay đã được đăng ký
    char msg[40]; sprintf(msg, "Already enrolled! ID: %d", dupID);
    Blynk.virtualWrite(V103, msg);
    lcd.clear(); lcd.setCursor(0,0); lcd.print(" Already exist! ");
    char buf[17]; sprintf(buf, " Already ID:%-4d", dupID);
    lcd.setCursor(0,1); lcd.print(buf);
    beepFail(); ledRed(1500);
    delay(2000); restoreNormalLCD();
    return false;
  }

  // --- Bước 1: Đặt ngón tay lần 1 (image đã lấy trong checkDuplicate, dùng lại) ---
  // image2Tz(1) đã chạy trong checkDuplicate → chuyển thẳng sang bước nhấc tay
  Blynk.virtualWrite(V103, "Step 1 OK - Remove finger");
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Enroll step 1/2 ");
  lcd.setCursor(0,1); lcd.print("Remove finger.. ");

  unsigned long rm = millis();
  while (millis() - rm < 2000) { blockingTick(); }

  // Chờ nhấc tay hoàn toàn
  p = 0; t = millis();
  while (p != FINGERPRINT_NOFINGER) {
    if (millis() - t > FINGER_WAIT_TIMEOUT) break;
    p = finger.getImage();
    blockingTick();
  }

  // --- Bước 2: Đặt lại ngón tay ---
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Enroll step 2/2 ");
  lcd.setCursor(0,1); lcd.print("Place again...  ");
  Blynk.virtualWrite(V103, "Step 2: Place again");
  p = -1; t = millis();
  while (p != FINGERPRINT_OK) {
    if (millis() - t > ENROLL_TIMEOUT) {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("   Timeout!     ");
      lcd.setCursor(0,1); lcd.print("                ");
      Blynk.virtualWrite(V103, "TIMEOUT step 2");
      beepFail(); ledRed(1000);
      delay(1500); restoreNormalLCD();
      return false;
    }
    p = finger.getImage();
    blockingTick();
  }

  if (finger.image2Tz(2) != FINGERPRINT_OK) {
    lcd.clear(); lcd.setCursor(0,0); lcd.print("  Scan  failed  ");
    lcd.setCursor(0,1); lcd.print("  Try  again... ");
    Blynk.virtualWrite(V103, "Scan failed step 2");
    beepFail(); ledRed(1000);
    delay(1500); restoreNormalLCD();
    return false;
  }

  // --- Tạo & lưu model ---
  if (finger.createModel() == FINGERPRINT_OK && finger.storeModel(id) == FINGERPRINT_OK) {
    Blynk.virtualWrite(V103, "SUCCESS ID: " + String(id));
    lcd.clear(); lcd.setCursor(0,0); lcd.write(3); lcd.print(" Enroll OK!    ");
    char buf[16]; sprintf(buf, "ID:%-13d", id);
    lcd.setCursor(0,1); lcd.print(buf);
    beepOK(); ledGreen(1500);
    delay(2000); restoreNormalLCD();
    return true;
  }

  Blynk.virtualWrite(V103, "FAILED - store error");
  lcd.clear(); lcd.setCursor(0,0); lcd.print("  Enroll FAIL!  ");
  lcd.setCursor(0,1); lcd.print("  Try again...  ");
  beepFail(); ledRed(1500);
  delay(1500); restoreNormalLCD();
  return false;
}

// =========================================================
// DELETE
// =========================================================
void deleteFingerprint(int id) {
  if (finger.deleteModel(id) == FINGERPRINT_OK) {
    Blynk.virtualWrite(V103, "DELETED ID: " + String(id));
    lcd.clear(); lcd.setCursor(0,0); lcd.print("  Deleted OK!   ");
    char buf[17]; sprintf(buf, "ID: %-12d", id);
    lcd.setCursor(0,1); lcd.print(buf);
    beepOK(); ledGreen(1000);
  } else {
    Blynk.virtualWrite(V103, "ERROR: not found");
    lcd.clear(); lcd.setCursor(0,0); lcd.print("  Delete ERROR! ");
    lcd.setCursor(0,1); lcd.print("  ID not found  ");
    beepFail(); ledRed(800);
  }
  unsigned long t = millis();
  while (millis() - t < 2000) { blockingTick(); }
  restoreNormalLCD();
}

// =========================================================
// MỞ CỬA
// =========================================================
void openDoor() {
  isDoorOpen = true; myServo.write(90); beepOK(); ledGreen(1000);
  lcd.clear();
  lcd.setCursor(0,0); lcd.write(3); lcd.print(" ACCESS OK!    ");
  lcd.setCursor(0,1); lcd.print("   Door  OPEN   ");
  lcdUpdateTime = millis() + 2000;
  matrixLastText = ""; matrixSetNormal();
  Blynk.virtualWrite(V30, "Opened");
}

// =========================================================
// SAI MẬT KHẨU
// =========================================================
void wrongPass() {
  passFailCount++; beepFail(); ledRed(600);
  if (passFailCount >= MAX_PASS_FAIL) {
    sysState = STATE_ALERT_PASS; alertStartTime = millis(); return;
  }
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("  Access DENIED ");
  lcd.setCursor(0,1); lcd.print(" Wrong password ");
  lcdUpdateTime = millis() + 1500;
}

// =========================================================
// VÂN TAY — check 1 lần khi chạm, chờ nhấc tay mới check lại
// =========================================================
void getFingerprintID() {
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
      fingerFailCount = 0;
      char idBuf[16]; sprintf(idBuf, " ID: %-10d", finger.fingerID);
      lcd.clear(); lcd.setCursor(0,0); lcd.write(2); lcd.print(idBuf);
      lcd.setCursor(0,1); lcd.print("  Access  OK!   ");
      openDoor();
    } else {
      fingerFailCount++; beepWarn();
      if (fingerFailCount >= MAX_FINGER_FAIL) {
        isFingerLocked = true;
        Blynk.logEvent("fingerprint_alert", "Finger LOCKED! Too many failed attempts!");
        lcd.clear(); lcd.setCursor(0,0); lcd.write(1); lcd.print(" Finger LOCKED ");
        lcd.setCursor(0,1); lcd.print("Use keypad pass ");
        ledRed(1000); lcdUpdateTime = millis() + 2500;
      } else {
        Blynk.logEvent("fingerprint_alert",
          "Wrong fingerprint! Fail: " + String(fingerFailCount) + "/" + String(MAX_FINGER_FAIL));
        char failBuf[17]; sprintf(failBuf, "Fail: %d/%-9d", fingerFailCount, MAX_FINGER_FAIL);
        lcd.clear(); lcd.setCursor(0,0); lcd.print(" Not recognized ");
        lcd.setCursor(0,1); lcd.print(failBuf);
        ledRed(600); lcdUpdateTime = millis() + 1500;
      }
    }
  }
}

// =========================================================
// HIỂN THỊ BÌNH THƯỜNG
// LCD xoay: TIME(8s) → DATE(4s) → WEATHER(4s)
// Dùng biến global dispLastSec, dispT0 thay vì static local
// =========================================================
void handleNormalDisplay() {
  matrixSetNormal();
  if (millis() < lcdUpdateTime) return;
  switch (displayMode) {
    case 0: {
      int hh = timeClient.getHours();
      int mm = timeClient.getMinutes();
      int ss = timeClient.getSeconds();
      char buf[17]; sprintf(buf, "   %02d:%02d:%02d   ", hh, mm, ss);
      lcd.setCursor(0,0); lcd.print("   -- TIME --   ");
      if (ss != dispLastSec) {
        dispLastSec = ss;
        lcd.setCursor(0,1); lcd.print(buf);
      }
      if (!dispT0) dispT0 = millis();
      if (millis() - dispT0 >= 8000) {
        dispT0 = 0; dispLastSec = -1; displayMode = 1;
      }
      break;
    }
    case 1: {
      time_t e = timeClient.getEpochTime();
      struct tm* ti = gmtime(&e);
      char buf[17]; sprintf(buf, "  %02d/%02d/%04d  ", ti->tm_mday, ti->tm_mon + 1, ti->tm_year + 1900);
      lcd.setCursor(0,0); lcd.print("   -- DATE --   ");
      lcd.setCursor(0,1); lcd.print(buf);
      lcdUpdateTime = millis() + 4000; displayMode = 2;
      break;
    }
    case 2: {
      char line0[17], line1[17];
      sprintf(line0, " Temp: %.1f C   ", weatherTemp); line0[16] = 0;
      sprintf(line1, "%-16s", weatherDesc.c_str());    line1[16] = 0;
      lcd.setCursor(0,0); lcd.print(line0);
      lcd.setCursor(0,1); lcd.print(line1);
      lcdUpdateTime = millis() + 4000; displayMode = 0;
      break;
    }
  }
}

// =========================================================
// BÁO ĐỘNG GAS
// =========================================================
void handleGasAlert(int gasVal) {
  if (matrixLastText != "ALERT!!") {
    matrixSetAlert();
    lcd.clear();
    lcd.setCursor(0,0); lcd.write(0); lcd.print(" !!! Alert !!!!");
    lcd.setCursor(0,1); lcd.write(0); lcd.print(" GAS DETECTED!");
    lcd.setCursor(15,1); lcd.print(" ");
    Blynk.logEvent("gas_alert", "GAS DETECTED! Value: " + String(gasVal));
  }
  if (millis() - lastBlink > 150) {
    lastBlink = millis(); blinkState = !blinkState;
    if (blinkState) ledRedOn(); else ledAllOff();
    digitalWrite(BUZZER_PIN, blinkState ? HIGH : LOW);
  }
  if (millis() - alertStartTime >= ALERT_DURATION) {
    if (analogRead(MQ2_PIN) > GAS_THRESHOLD) {
      alertStartTime = millis();
    } else {
      sysState = STATE_NORMAL; normalState = true;
      ledAllOff(); digitalWrite(BUZZER_PIN, LOW); beepLen = 0;
      matrixLastText = ""; lcd.clear(); displayMode = 0; lcdUpdateTime = 0;
    }
  }
}

// =========================================================
// BÁO ĐỘNG TRỘM (PIR)
// =========================================================
void handleAlertPIR() {
  if (matrixLastText != "ALERT!!") {
    matrixSetAlert();
    lcd.clear();
    lcd.setCursor(0,0); lcd.write(0); lcd.print(" !!! Alert !!!!");
    lcd.setCursor(0,1); lcd.write(0); lcd.print("INTRUDER DETECT");
    Blynk.logEvent("intruder_alert", "INTRUDER DETECTED!");
  }
  if (millis() - lastBlink > 200) {
    lastBlink = millis(); blinkState = !blinkState;
    if (blinkState) ledRedOn(); else ledAllOff();
    digitalWrite(BUZZER_PIN, blinkState ? HIGH : LOW);
  }
  if (millis() - alertStartTime >= ALERT_DURATION) {
    int pir = digitalRead(PIR_PIN);
    int ldr = analogRead(LDR_PIN);
    if (pir == HIGH && ldr < LDR_DARK_THRESHOLD) {
      alertStartTime = millis();
    } else {
      sysState = STATE_NORMAL; normalState = true;
      ledAllOff(); digitalWrite(BUZZER_PIN, LOW); beepLen = 0;
      matrixLastText = ""; lcd.clear(); displayMode = 0; lcdUpdateTime = 0;
    }
  }
}

// =========================================================
// BÁO ĐỘNG SAI PASS — không bị ảnh hưởng bởi alertEnabled
// =========================================================
void handleAlertPass() {
  if (matrixLastText != "ALERT!!") {
    matrixSetAlert();
    lcd.clear();
    lcd.setCursor(0,0); lcd.write(0); lcd.print(" !!! Alert !!!!");
    lcd.setCursor(0,1); lcd.print("  Too many fail ");
    Blynk.logEvent("wrong_pass_alert", "Too many wrong password attempts!");
  }
  if (millis() - lastBlink > 200) {
    lastBlink = millis(); blinkState = !blinkState;
    if (blinkState) ledRedOn(); else ledAllOff();
    digitalWrite(BUZZER_PIN, blinkState ? HIGH : LOW);
  }
  if (millis() - alertStartTime >= ALERT_DURATION) {
    passFailCount = 0; sysState = STATE_NORMAL; normalState = true;
    ledAllOff(); digitalWrite(BUZZER_PIN, LOW); beepLen = 0;
    matrixLastText = ""; lcd.clear(); displayMode = 0; lcdUpdateTime = 0;
  }
}

// =========================================================
// SETUP
// =========================================================
void setup() {
  Serial.begin(115200); delay(500);

  pinMode(MQ2_PIN, INPUT); pinMode(PIR_PIN, INPUT); pinMode(LDR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT); pinMode(LED_RED, OUTPUT); pinMode(LED_GREEN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); ledAllOff();

  myServo.attach(SERVO_PIN); myServo.write(0);

  Wire.begin(21, 22); lcd.init(); lcd.backlight();
  lcd.createChar(0, bellChar); lcd.createChar(1, lockChar);
  lcd.createChar(2, personChar); lcd.createChar(3, checkChar);
  lcd.clear(); lcd.setCursor(0,0); lcd.print("  Smart  Lock   ");
  lcd.setCursor(0,1); lcd.print("  Starting...   ");

  matrix.begin(); matrix.setIntensity(2); matrix.displayClear();
  matrixShow("Smart Lock");
  delay(1500);

  mySerial.begin(57600, SERIAL_8N1, 16, 17); finger.begin(57600);
  if (!finger.verifyPassword()) {
    lcd.clear(); lcd.setCursor(0,0); lcd.print("  Finger sensor ");
    lcd.setCursor(0,1); lcd.print("  NOT  FOUND!   "); delay(2000);
  }
  keypad.setDebounceTime(50);

  // Đọc password từ flash (persistent)
  prefs.begin("smartlock", false);
  password = prefs.getString("pass", "5678");

  lcd.clear(); lcd.setCursor(0,0); lcd.print(" Connecting...  ");
  lcd.setCursor(0,1); lcd.print(ssid);
  WiFi.begin(ssid, pass);
  int wt = 0;
  while (WiFi.status() != WL_CONNECTED && wt < 20) { delay(500); wt++; }
  if (WiFi.status() == WL_CONNECTED) {
    lcd.clear(); lcd.setCursor(0,0); lcd.write(3); lcd.print(" WiFi  OK      ");
    lcd.setCursor(0,1); lcd.print(WiFi.localIP().toString()); delay(1500);
  } else {
    lcd.clear(); lcd.setCursor(0,0); lcd.print("  WiFi  FAILED  ");
    lcd.setCursor(0,1); lcd.print("  Offline mode  "); delay(1500);
  }

  Blynk.config(auth); Blynk.connect();
  timeClient.begin(); timeClient.update();
  timer.setInterval(1000L,    sendSensorData);
  timer.setInterval(120000L,  getWeatherData);
  getWeatherData();

  Blynk.virtualWrite(V30, "Closed");
  Blynk.virtualWrite(V40, 1);

  lcd.clear(); lcd.setCursor(0,0); lcd.print("  System Ready  ");
  lcd.setCursor(0,1); lcd.print(" Press # to open");
  matrixLastText = ""; matrixSetNormal();
  delay(1000); Serial.println("--- SYSTEM READY ---");
}

// =========================================================
// LOOP
// =========================================================
void loop() {
  Blynk.run(); timer.run(); timeClient.update();
  updateLED(); updateBuzzer();

  // --- Enroll ---
  if (isEnrolling) {
    enrollFingerprint(idFromBlynk);  // LCD đã được restore bên trong hàm
    isEnrolling = false;
    Blynk.virtualWrite(V101, 0);     // đảm bảo button reset (phòng hờ)
    blynkFlush();
    normalState = true; sysState = STATE_NORMAL;
    displayMode = 0; lcdUpdateTime = millis() + 2000;
    dispT0 = 0; dispLastSec = -1;
    Blynk.virtualWrite(V103, "SYSTEM NORMAL");
    return;
  }

  // --- Delete ---
  if (isDeleting) {
    deleteFingerprint(idFromBlynk);
    isDeleting = false;
    Blynk.virtualWrite(V102, 0);     // đảm bảo button reset
    blynkFlush();
    normalState = true; sysState = STATE_NORMAL;
    displayMode = 0; lcdUpdateTime = millis() + 2000;
    Blynk.virtualWrite(V103, "SYSTEM NORMAL");
    return;
  }

  int gasVal = analogRead(MQ2_PIN);
  int ldrVal = analogRead(LDR_PIN);
  int pirVal = digitalRead(PIR_PIN);

  // Gas & PIR chỉ kích hoạt khi alertEnabled = true
  if (gasVal > GAS_THRESHOLD && sysState != STATE_ALERT_GAS && alertEnabled) {
    sysState = STATE_ALERT_GAS; alertStartTime = millis(); normalState = false; beepLen = 0;
  }
  if (sysState == STATE_ALERT_GAS) { handleGasAlert(gasVal); return; }

  if (pirVal == HIGH && ldrVal < LDR_DARK_THRESHOLD && sysState != STATE_ALERT_PIR && alertEnabled) {
    sysState = STATE_ALERT_PIR; alertStartTime = millis(); normalState = false; beepLen = 0;
  }
  if (sysState == STATE_ALERT_PIR) { handleAlertPIR(); return; }

  // Sai pass vẫn báo động dù alertEnabled = false
  if (sysState == STATE_ALERT_PASS) { handleAlertPass(); return; }

  if (sysState == STATE_NORMAL) getFingerprintID();

  // --- KEYPAD ---
  char key = keypad.getKey();
  if (key && millis() - lastKeyTime > 300) {
    lastKeyTime = millis(); beepKey();

    if (key == 'D') {
      // Reset toàn bộ hệ thống
      sysState = STATE_NORMAL; normalState = true;
      inputPass = ""; newPassBuf = "";
      passFailCount = 0; fingerFailCount = 0;
      isFingerLocked = false; isDoorOpen = false;
      fingerPresent = false; fingerProcessed = false;
      myServo.write(0); ledAllOff(); digitalWrite(BUZZER_PIN, LOW); beepLen = 0;
      matrixLastText = ""; matrixSetNormal();
      Blynk.virtualWrite(V30, "Closed");
      lcd.clear(); lcd.setCursor(0,0); lcd.print("  System Reset  ");
      lcd.setCursor(0,1); lcd.print("   All  Clear   ");
      lcdUpdateTime = millis() + 1500;
      displayMode = 0; dispT0 = 0; dispLastSec = -1;
      Serial.println("[RESET] System reset");
    }
    else if (key == 'B') {
      sysState = STATE_CHANGE_PASS; normalState = false; cpStep = CP_OLD; inputPass = "";
      lcd.clear(); lcd.setCursor(0,0); lcd.write(1); lcd.print(" Change  Pass  ");
      lcd.setCursor(0,1); lcd.print("Old pass:       ");
    }
    else if (key == 'C' || key == '*') {
      inputPass = "";
      if (sysState == STATE_ENTER_PASS) {
        lcd.setCursor(0,1); lcd.print("Pass:           ");
      } else if (sysState == STATE_CHANGE_PASS) {
        lcd.setCursor(0,1);
        if      (cpStep == CP_OLD)     lcd.print("Old pass:       ");
        else if (cpStep == CP_NEW)     lcd.print("New pass:       ");
        else if (cpStep == CP_CONFIRM) lcd.print("Confirm:        ");
      } else {
        sysState = STATE_ENTER_PASS; normalState = false;
        lcd.clear(); lcd.setCursor(0,0); lcd.write(1); lcd.print(" Enter Password");
        lcd.setCursor(0,1); lcd.print("Pass:           ");
      }
    }
    else if (key == '#') {
      if (sysState == STATE_NORMAL || sysState == STATE_ENTER_PASS) {
        if (inputPass == password) {
          passFailCount = 0; isFingerLocked = false; openDoor();
        } else {
          wrongPass();
        }
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
            sysState = STATE_NORMAL; normalState = true;
            lcdUpdateTime = millis() + 1500;
          }
        }
        else if (cpStep == CP_NEW) {
          if (inputPass.length() > 0) {
            newPassBuf = inputPass; inputPass = ""; cpStep = CP_CONFIRM;
            lcd.clear(); lcd.setCursor(0,0); lcd.write(1); lcd.print(" Change  Pass  ");
            lcd.setCursor(0,1); lcd.print("Confirm:        ");
          }
        }
        else if (cpStep == CP_CONFIRM) {
          if (inputPass == newPassBuf) {
            password = inputPass;
            prefs.putString("pass", password);  // lưu xuống flash
            beepOK(); ledGreen(1500);
            lcd.clear(); lcd.setCursor(0,0); lcd.write(3); lcd.print(" Pass changed! ");
            lcd.setCursor(0,1); lcd.print("  New pass  OK  ");
            Serial.println("[PASS] New: " + password);
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
    else if (key != 'A' && key != 'D') {
      if (sysState == STATE_NORMAL) {
        sysState = STATE_ENTER_PASS; normalState = false; inputPass = "";
        lcd.clear(); lcd.setCursor(0,0); lcd.write(1); lcd.print(" Enter Password");
        lcd.setCursor(0,1); lcd.print("Pass:           ");
      }
      if (sysState == STATE_ENTER_PASS || sysState == STATE_CHANGE_PASS) {
        inputPass += key;
        String stars = "";
        for (size_t i = 0; i < inputPass.length(); i++) stars += "*";
        String disp = stars.length() > 9 ? stars.substring(stars.length() - 9) : stars;
        lcd.setCursor(0,0);
        if (sysState == STATE_ENTER_PASS) {
          lcd.write(1); lcd.print(" Enter Password");
        } else {
          lcd.write(1);
          if      (cpStep == CP_OLD)     lcd.print(" Change  Pass  ");
          else if (cpStep == CP_NEW)     lcd.print(" New  Password ");
          else if (cpStep == CP_CONFIRM) lcd.print(" Confirm  Pass ");
        }
        lcd.setCursor(0,1);
        if      (sysState == STATE_ENTER_PASS)                        lcd.print("Pass: ");
        else if (sysState == STATE_CHANGE_PASS && cpStep == CP_OLD)   lcd.print("Old:  ");
        else if (sysState == STATE_CHANGE_PASS && cpStep == CP_NEW)   lcd.print("New:  ");
        else if (sysState == STATE_CHANGE_PASS && cpStep == CP_CONFIRM) lcd.print("Cnf:  ");
        lcd.print(disp);
        for (int i = disp.length(); i < 10; i++) lcd.print(" ");
      }
    }
  }

  if (sysState == STATE_NORMAL && normalState) handleNormalDisplay();

  if (millis() - lastConsoleTime > 1000) {
    lastConsoleTime = millis();
    Serial.printf("[GAS]%d [LDR]%d [PIR]%d [LOCKED]%s [DOOR]%s [ALERT]%s\n",
      gasVal, ldrVal, pirVal,
      isFingerLocked ? "YES" : "NO",
      isDoorOpen     ? "OPEN" : "CLOSED",
      alertEnabled   ? "ON" : "OFF");
  }
}

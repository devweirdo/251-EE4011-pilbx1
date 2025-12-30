#include <Wire.h>
#include <RTClib.h>
#include <WiFi.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <vector>
#include <algorithm>
#include <LiquidCrystal_I2C.h>

// --- CONFIGURATION ---
#define I2C_SDA 6
#define I2C_SCL 5
#define SQW_PIN 8
#define BUZZER_PIN 3
#define HALL_SENSOR_PIN 7
#define LED_PIN 2

// BLE UUIDs
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_WIFI_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_SCHED_UUID "8899aabb-ccdd-eeff-0011-223344556677"
#define CHAR_VOL_UUID "12345678-1234-1234-1234-1234567890ab"
#define CHAR_HIST_UUID "99999999-9999-9999-9999-999999999999"
#define CHAR_CONF_UUID "55555555-5555-5555-5555-555555555555"  // <--- NEW: Settings UUID

// --- GLOBALS ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS3231 rtc;
Preferences preferences;
BLECharacteristic *pWifiChar, *pSchedChar, *pVolChar, *pHistChar, *pConfChar;

struct AlarmTime {
  int hour;
  int minute;
};
std::vector<AlarmTime> alarms;
std::vector<String> historyEntries;

volatile bool alarmInterruptTriggered = false;
bool wifiCredsUpdated = false;
bool scheduleUpdated = false;
bool isTimeSynced = false;
bool use12hr = false;  // <--- NEW: Track time format
bool configUpdated = false;
String nextAlarmDisplay = "--:--";
int currentVolume = 128;

byte checkIcon[8] = { 0b00000, 0b00001, 0b00011, 0b10110, 0b11100, 0b01000, 0b00000, 0b00000 };

// --- CONSTANTS FOR TIMING ---
const unsigned long RING_DURATION = 60000;     // 1 Minute ringing
const unsigned long SNOOZE_DURATION = 300000;  // 5 Minutes silence
const unsigned long TOTAL_WINDOW = 900000;     // 15 Minutes total timeout


void IRAM_ATTR onAlarmISR() {
  alarmInterruptTriggered = true;
}

// --- BLE SERVER CALLBACKS ---
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    Serial.println("Connected");
  };
  void onDisconnect(BLEServer *pServer) {
    delay(500);
    pServer->startAdvertising();
  }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) {
    String data = pChar->getValue();
    if (data.length() > 0) {
      if (pChar->getUUID().toString() == CHAR_WIFI_UUID) {
        int commaIndex = data.indexOf(',');
        if (commaIndex > 0) {
          preferences.putString("ssid", data.substring(0, commaIndex));
          preferences.putString("pass", data.substring(commaIndex + 1));
          wifiCredsUpdated = true;
        }
      } else if (pChar->getUUID().toString() == CHAR_SCHED_UUID) {
        preferences.putString("sched", data);
        pSchedChar->setValue(data.c_str());
        scheduleUpdated = true;
      } else if (pChar->getUUID().toString() == CHAR_VOL_UUID) {
        int v = data.toInt();
        currentVolume = constrain(v, 0, 255);
        preferences.putInt("vol", currentVolume);
        pVolChar->setValue(String(currentVolume).c_str());
        ledcWrite(BUZZER_PIN, currentVolume);
        delay(100);
        ledcWrite(BUZZER_PIN, 0);
      }
      // --- NEW: Handle Settings (12H/24H) ---
      else if (pChar->getUUID().toString() == CHAR_CONF_UUID) {
        if (data == "12H") use12hr = true;
        else if (data == "24H") use12hr = false;
        preferences.putBool("12h", use12hr);
        pConfChar->setValue(use12hr ? "12H" : "24H");
        configUpdated = true;
      }
    }
  }
};

bool compareAlarms(AlarmTime a, AlarmTime b) {
  if (a.hour == b.hour) return a.minute < b.minute;
  return a.hour < b.hour;
}

// Convert "HH:MM" strings into logic
void parseSchedule(String data) {
  alarms.clear();
  int start = 0, end = data.indexOf(',');
  while (end != -1) {
    String t = data.substring(start, end);
    int s = t.indexOf(':');
    if (s > 0) alarms.push_back({ t.substring(0, s).toInt(), t.substring(s + 1).toInt() });
    start = end + 1;
    end = data.indexOf(',', start);
  }
  String t = data.substring(start);
  int s = t.indexOf(':');
  if (s > 0) alarms.push_back({ t.substring(0, s).toInt(), t.substring(s + 1).toInt() });
}

// Updated Next Alarm Logic (Handles 12h/24h display internally)
void scheduleNextAlarm() {
  if (alarms.empty()) {
    nextAlarmDisplay = "No Alarms";
    rtc.disableAlarm(1);
    rtc.clearAlarm(1);
    return;
  }
  std::sort(alarms.begin(), alarms.end(), compareAlarms);
  DateTime now = rtc.now();
  DateTime nextT;
  bool found = false;

  for (auto &a : alarms) {
    if (a.hour > now.hour() || (a.hour == now.hour() && a.minute > now.minute())) {
      nextT = DateTime(now.year(), now.month(), now.day(), a.hour, a.minute, 0);
      found = true;
      break;
    }
  }
  if (!found) {
    DateTime tom = now + TimeSpan(1, 0, 0, 0);
    nextT = DateTime(tom.year(), tom.month(), tom.day(), alarms[0].hour, alarms[0].minute, 0);
  }

  rtc.clearAlarm(1);
  rtc.setAlarm1(nextT, DS3231_A1_Hour);


  // Format Next Alarm String based on preference
  char b[16];
  if (use12hr) {
    int h = nextT.hour();
    String ampm = (h >= 12) ? "PM" : "AM";
    if (h == 0) h = 12;
    else if (h > 12) h -= 12;
    sprintf(b, "Next: %02d:%02d %s  ", h, nextT.minute(), ampm.c_str());
  } else {
    sprintf(b, "Next: %02d:%02d    ", nextT.hour(), nextT.minute());
  }
  nextAlarmDisplay = String(b);
}

void updateHistoryLog(String timeStr, String result) {
  String entry = timeStr + " " + result;
  historyEntries.push_back(entry);
  if (historyEntries.size() > 20) historyEntries.erase(historyEntries.begin());
  String fullLog = "Log History:\n";
  for (const String &s : historyEntries) fullLog += s + "\n";
  pHistChar->setValue(fullLog.c_str());
}

void enterAlarmMode() {
  unsigned long sessionStart = millis();
  DateTime nowDT = rtc.now();

  // Log Timestamp string
  char timeStr[20];
  sprintf(timeStr, "[%02d/%02d %02d:%02d]", nowDT.day(), nowDT.month(), nowDT.hour(), nowDT.minute());

  String status = "MISSED";  // Default assumption until proven otherwise
  bool pillTaken = false;

  Serial.println("!!! STARTING ALARM SESSION (15 MIN WINDOW) !!!");

  // --- MAIN 15-MINUTE LOOP ---
  while (millis() - sessionStart < TOTAL_WINDOW) {

    // ===========================
    // PHASE 1: ACTIVE RINGING
    // ===========================
    unsigned long ringStart = millis();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(" TAKE YOUR PILL ");

    while (millis() - ringStart < RING_DURATION) {
      // 1. CHECK SENSOR (Success)
      if (digitalRead(HALL_SENSOR_PIN) == LOW) {
        pillTaken = true;
        goto end_session;  // Jump out of all loops
      }

      // 2. BEEP PATTERN
      unsigned long c = millis() % 1000;
      if ((c < 100) || (c > 200 && c < 300)) {
        ledcWrite(BUZZER_PIN, currentVolume);
        digitalWrite(LED_PIN, HIGH);
      } else {
        ledcWrite(BUZZER_PIN, 0);
        digitalWrite(LED_PIN, LOW);
      }

      // 3. DISPLAY CLOCK (Bottom Row)
      if (millis() % 500 == 0) {
        DateTime dt = rtc.now();
        lcd.setCursor(4, 1);
        char tB[10];
        // Show time in HH:MM:SS
        sprintf(tB, "%02d:%02d:%02d", dt.hour(), dt.minute(), dt.second());
        lcd.print(tB);
      }
      delay(10);
    }

    // Silence Buzzer/LED after ringing phase
    ledcWrite(BUZZER_PIN, 0);
    digitalWrite(LED_PIN, LOW);

    // Check if we timed out the whole 15 mins during the ring
    if (millis() - sessionStart >= TOTAL_WINDOW) break;

    // ===========================
    // PHASE 2: SNOOZE (SILENT MONITORING)
    // ===========================
    unsigned long snoozeStart = millis();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("SNOOZING... ");

    while (millis() - snoozeStart < SNOOZE_DURATION) {
      // 1. CHECK SENSOR (Success during silence)
      if (digitalRead(HALL_SENSOR_PIN) == LOW) {
        pillTaken = true;
        goto end_session;
      }

      // 2. CHECK TOTAL TIMEOUT
      // If 15 mins pass while snoozing, we stop immediately
      if (millis() - sessionStart >= TOTAL_WINDOW) break;

      // 3. DISPLAY CLOCK (Update every second)
      if (millis() % 1000 == 0) {
        DateTime dt = rtc.now();
        lcd.setCursor(4, 1);
        char tB[10];
        sprintf(tB, "%02d:%02d:%02d", dt.hour(), dt.minute(), dt.second());
        lcd.print(tB);
      }
      delay(100);
    }
  }

end_session:

  // --- FINAL CLEANUP ---
  ledcWrite(BUZZER_PIN, 0);
  digitalWrite(LED_PIN, LOW);

  if (pillTaken) {
    status = "TAKEN";
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("  WELL DONE!  ");
    delay(2000);
  } else {
    status = "MISSED";  // Confirmed missed after 15 mins
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("  MISSED PILL ");
    delay(2000);
  }

  updateHistoryLog(String(timeStr), status);
  scheduleNextAlarm();  // Move on to next scheduled time
}

void syncTimeWithNTP() {
  String s = preferences.getString("ssid", "");
  String p = preferences.getString("pass", "");

  if (s == "") {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("No WiFi Setup");
    delay(2000);
    lcd.clear();
    return;
  }
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Syncing Time...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(s.c_str(), p.c_str());


  int att = 0;
  while (WiFi.status() != WL_CONNECTED && att < 16) {
    delay(500);
    lcd.setCursor(att, 1);
    lcd.print(".");
    att++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    struct tm ti;
    if (getLocalTime(&ti, 5000)) {
      time_t nS = mktime(&ti) + (7 * 3600);
      struct tm *lT = localtime(&nS);
      rtc.adjust(DateTime(lT->tm_year + 1900, lT->tm_mon + 1, lT->tm_mday, lT->tm_hour, lT->tm_min, lT->tm_sec));
      isTimeSynced = true;
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Time Synced!");
      lcd.setCursor(15, 0);
      lcd.write(0);
      delay(2000);
    }
  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Failed!");
    lcd.setCursor(0, 1);
    lcd.print("Check Creds");
    delay(3000);
    isTimeSynced = false;
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  lcd.clear();
}

void setup() {
  Serial.begin(115200);
  preferences.begin("alarmclock", false);
  currentVolume = preferences.getInt("vol", 128);
  use12hr = preferences.getBool("12h", false);  // Load setting

  ledcAttach(BUZZER_PIN, 2000, 8);
  pinMode(LED_PIN, OUTPUT);
  pinMode(HALL_SENSOR_PIN, INPUT_PULLUP);
  pinMode(SQW_PIN, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, checkIcon);
  lcd.setCursor(0, 0);
  lcd.print("Booting...");

  if (!rtc.begin())
    while (1)
      ;

  rtc.writeSqwPinMode(DS3231_OFF);


  rtc.disableAlarm(1);
  rtc.disableAlarm(2);

  rtc.clearAlarm(1);
  rtc.clearAlarm(2);

  String savedSched = preferences.getString("sched", "08:00,12:00,18:00");
  parseSchedule(savedSched);
  scheduleNextAlarm();

  attachInterrupt(digitalPinToInterrupt(SQW_PIN), onAlarmISR, FALLING);

  BLEDevice::init("pilbx_v2");  // Device Name
  BLEServer *pSrv = BLEDevice::createServer();
  pSrv->setCallbacks(new MyServerCallbacks());
  BLEService *pSvc = pSrv->createService(SERVICE_UUID);

  pWifiChar = pSvc->createCharacteristic(CHAR_WIFI_UUID, BLECharacteristic::PROPERTY_WRITE);
  pWifiChar->setCallbacks(new MyCallbacks());

  pSchedChar = pSvc->createCharacteristic(CHAR_SCHED_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pSchedChar->setCallbacks(new MyCallbacks());
  pSchedChar->setValue(savedSched.c_str());

  pVolChar = pSvc->createCharacteristic(CHAR_VOL_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pVolChar->setCallbacks(new MyCallbacks());
  pVolChar->setValue(String(currentVolume).c_str());

  pHistChar = pSvc->createCharacteristic(CHAR_HIST_UUID, BLECharacteristic::PROPERTY_READ);
  pHistChar->setValue("No logs yet.");

  // --- NEW: CONFIG CHARACTERISTIC ---
  pConfChar = pSvc->createCharacteristic(CHAR_CONF_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pConfChar->setCallbacks(new MyCallbacks());
  pConfChar->setValue(use12hr ? "12H" : "24H");

  pSvc->start();
  BLEDevice::startAdvertising();
  syncTimeWithNTP();
}

unsigned long lastUpd = 0;

// --- ADD THIS GLOBAL VARIABLE ABOVE SETUP OR LOOP ---
int lastDisplayedSecond = -1;
unsigned long lastPoll = 0;

void loop() {
  // 1. Handle Alarm Interrupt
  if (alarmInterruptTriggered) {
    alarmInterruptTriggered = false;
    rtc.clearAlarm(1);
    rtc.clearAlarm(2);
    enterAlarmMode();
  }

  // 2. Handle Settings/WiFi Updates
  if (wifiCredsUpdated) {
    wifiCredsUpdated = false;
    syncTimeWithNTP();
    scheduleNextAlarm();
  }
  if (scheduleUpdated) {
    scheduleUpdated = false;
    parseSchedule(preferences.getString("sched", ""));
    scheduleNextAlarm();
  }
  if (configUpdated) {
    configUpdated = false;
    scheduleNextAlarm();
    // Force a screen refresh immediately so the user sees the 12/24h change
    lastDisplayedSecond = -1;
  }

  // 3. OPTIMIZED LCD UPDATE LOGIC
  // Check the time every 100ms (fast polling)
  if (millis() - lastPoll > 100) {
    lastPoll = millis();

    DateTime n = rtc.now();

    // Only update the screen if the second has changed
    if (n.second() != lastDisplayedSecond) {
      lastDisplayedSecond = n.second();

      lcd.setCursor(0, 0);

      // --- 12H / 24H DISPLAY LOGIC ---
      char l0[16];
      if (use12hr) {
        int h = n.hour();
        String ampm = (h >= 12) ? "PM" : "AM";
        if (h == 0) h = 12;
        else if (h > 12) h -= 12;
        // Format: "12:30:45 PM"
        sprintf(l0, "%02d:%02d:%02d %s    ", h, n.minute(), n.second(), ampm.c_str());
      } else {
        // Format: "13:30:45"
        sprintf(l0, "%02d:%02d:%02d       ", n.hour(), n.minute(), n.second());
      }
      // ------------------------------------

      lcd.print(l0);

      // Update Sync Icon
      lcd.setCursor(15, 0);
      if (isTimeSynced) lcd.write(0);
      else lcd.print(" ");

      // Update Next Alarm Text
      lcd.setCursor(0, 1);
      lcd.print(nextAlarmDisplay);
      lcd.print("      ");  // Clear trailing characters
    }
  }
}
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <RTClib.h>
#include <EEPROM.h>
#include <PrayerTimes.h>  // sesuai header yang kamu kirim (v2.0)

LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS1307 rtc;

// Lokasi Sepatan Timur
float latitude  = -6.1189;
float longitude = 106.6023;
int   timezone  = 7; // jam

// Konstruktor memakai offset timezone dalam MENIT
PrayerTimes pt(latitude, longitude, timezone * 60);

// Keypad 4x4
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {D5, D6, D7, D8};
byte colPins[COLS] = {D0, D3, D4, D9};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// EEPROM untuk offset menit ON/OFF
int offsetMinutes = 30;

void saveOffset() {
  EEPROM.begin(512);
  EEPROM.write(0, offsetMinutes);
  EEPROM.commit();
}

void loadOffset() {
  EEPROM.begin(512);
  offsetMinutes = EEPROM.read(0);
  if (offsetMinutes <= 0 || offsetMinutes > 180) offsetMinutes = 30;
}

// ESP-NOW: MAC penerima (ganti sesuai MAC receiver kamu)
uint8_t receiverAddress[] = {0xE0, 0x98, 0x06, 0x93, 0x9E, 0x1B};

typedef struct { char cmd[4]; } AcCommand;
AcCommand outCmd;

void sendAcCommand(bool turnOn) {
  strncpy(outCmd.cmd, turnOn ? "ON" : "OFF", 3);
  esp_now_send(receiverAddress, (uint8_t*)&outCmd, sizeof(outCmd));
}

// Scroll jadwal di LCD
int prayerIndex = 0;
unsigned long lastScroll = 0;
const unsigned long scrollInterval = 5000;

// Password menu
const String menuPassword = "14000";
bool menuUnlocked = false;

bool checkPassword() {
  String input = "";
  lcd.clear(); lcd.print("Enter Password:");
  while (true) {
    char k = keypad.getKey();
    if (k) {
      if (k == '#') break;
      if (k == '*') { if (input.length() > 0) input.remove(input.length() - 1); }
      else { input += k; }
      lcd.setCursor(0, 1);
      lcd.print("                ");
      lcd.setCursor(0, 1);
      lcd.print(input);
    }
    delay(10);
  }
  return (input == menuPassword);
}

// Ambil jadwal harian (Subuh, Dzuhur, Ashar, Maghrib, Isya) → HH:MM
void getDailyHHMM(DateTime now, int hhmm[5][2]) {
  // Pastikan konfigurasi metode perhitungan (Indonesia, Asr Shafii, tanpa high-lat rule, tanpa adjustment)
  pt.setCalculationMethod(CalculationMethods::INDONESIA);
  pt.setAsrMethod(SHAFII);
  pt.setHighLatitudeRule(NONE);
  pt.setAdjustments(0,0,0,0,0,0);

  // Legacy API: langsung menghasilkan jam-menit per waktu
  int fajrH, fajrM, sunriseH, sunriseM, dhuhrH, dhuhrM, asrH, asrM, magH, magM, ishaH, ishaM;
  pt.calculate(now.day(), now.month(), now.year(),
               fajrH, fajrM,
               sunriseH, sunriseM,
               dhuhrH, dhuhrM,
               asrH, asrM,
               magH, magM,
               ishaH, ishaM);

  // Isi urutan: 0 Subuh, 1 Dzuhur, 2 Ashar, 3 Maghrib, 4 Isya
  hhmm[0][0] = fajrH; hhmm[0][1] = fajrM;
  hhmm[1][0] = dhuhrH; hhmm[1][1] = dhuhrM;
  hhmm[2][0] = asrH;   hhmm[2][1] = asrM;
  hhmm[3][0] = magH;   hhmm[3][1] = magM;
  hhmm[4][0] = ishaH;  hhmm[4][1] = ishaM;
}

// Logika jendela AC: normal untuk Subuh/Dzuhur/Ashar; khusus rentang Maghrib–Isya
bool acWindowActive(int hhmm[5][2], DateTime now) {
  DateTime tSb(now.year(), now.month(), now.day(), hhmm[0][0], hhmm[0][1], 0);
  DateTime tDz(now.year(), now.month(), now.day(), hhmm[1][0], hhmm[1][1], 0);
  DateTime tAs(now.year(), now.month(), now.day(), hhmm[2][0], hhmm[2][1], 0);
  DateTime tMg(now.year(), now.month(), now.day(), hhmm[3][0], hhmm[3][1], 0);
  DateTime tIs(now.year(), now.month(), now.day(), hhmm[4][0], hhmm[4][1], 0);

  // Normal: ON offset sebelum, OFF offset sesudah
  DateTime sbOn = tSb - TimeSpan(0, 0, offsetMinutes, 0);
  DateTime sbOff = tSb + TimeSpan(0, 0, offsetMinutes, 0);
  DateTime dzOn = tDz - TimeSpan(0, 0, offsetMinutes, 0);
  DateTime dzOff = tDz + TimeSpan(0, 0, offsetMinutes, 0);
  DateTime asOn = tAs - TimeSpan(0, 0, offsetMinutes, 0);
  DateTime asOff = tAs + TimeSpan(0, 0, offsetMinutes, 0);

  // Khusus Maghrib–Isya: ON offset sebelum Maghrib, OFF offset setelah Isya
  DateTime mgOn  = tMg - TimeSpan(0, 0, offsetMinutes, 0);
  DateTime isOff = tIs + TimeSpan(0, 0, offsetMinutes, 0);

  return ((now >= sbOn && now <= sbOff) ||
          (now >= dzOn && now <= dzOff) ||
          (now >= asOn && now <= asOff) ||
          (now >= mgOn && now <= isOff));
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  lcd.init(); lcd.backlight();
  rtc.begin();
  loadOffset();

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != 0) {
    lcd.clear(); lcd.print("ESP-NOW gagal");
    while (true) { delay(1000); }
  }
  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_add_peer(receiverAddress, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);

  lcd.clear();
  lcd.print("Init OK Offset:");
  lcd.setCursor(0,1);
  lcd.print(offsetMinutes);
  delay(800);
}

void loop() {
  DateTime now = rtc.now();

  // Ambil jadwal
  int hhmm[5][2];
  getDailyHHMM(now, hhmm);

  // Status AC otomatis
  bool active = acWindowActive(hhmm, now);
  sendAcCommand(active);

  // LCD baris 1: jam & tanggal
  lcd.setCursor(0, 0);
  lcd.printf("%02d:%02d %02d/%02d", now.hour(), now.minute(), now.day(), now.month());

  // Bergantian tampil jadwal & status AC
  if (millis() - lastScroll > scrollInterval) {
    prayerIndex = (prayerIndex + 1) % 5;
    lastScroll = millis();
  }
  const char* prayerName[5] = {"Sb","Dz","As","Mg","Is"};
  int h = hhmm[prayerIndex][0];
  int m = hhmm[prayerIndex][1];
  lcd.setCursor(0, 1);
  lcd.printf("%s:%02d:%02d AC:%s", prayerName[prayerIndex], h, m, active ? "ON " : "OFF");

  // Keypad/menu
  char key = keypad.getKey();
  if (key) {
    if (key == '*') {
      if (checkPassword()) {
        menuUnlocked = true;
        lcd.clear(); lcd.print("Menu Unlocked");
        delay(500);
      } else {
        menuUnlocked = false;
        lcd.clear(); lcd.print("Wrong Password");
        delay(700);
      }
    } else if (menuUnlocked) {
      if (key == 'A') {
        sendAcCommand(true);
        lcd.clear(); lcd.print("AC Manual ON");
        delay(700);
      } else if (key == 'B') {
        sendAcCommand(false);
        lcd.clear(); lcd.print("AC Manual OFF");
        delay(700);
      } else if (key == 'C') {
        // Ubah offset menit
        lcd.clear(); lcd.print("Offset (min):");
        String numStr = "";
        while (true) {
          char k = keypad.getKey();
          if (k == '#') break;
          if (k == '*') { if (numStr.length() > 0) numStr.remove(numStr.length() - 1); }
          else if (k >= '0' && k <= '9') { numStr += k; }
          lcd.setCursor(0, 1);
          lcd.print("                ");
          lcd.setCursor(0, 1);
          lcd.print(numStr);
          delay(10);
        }
        int val = numStr.toInt();
        if (val <= 0 || val > 180) val = 30;
        offsetMinutes = val;
        saveOffset();
        lcd.clear(); lcd.print("Saved Offset:");
        lcd.setCursor(0,1); lcd.print(offsetMinutes);
        delay(1000);
      }
    }
  }

  delay(150);
}

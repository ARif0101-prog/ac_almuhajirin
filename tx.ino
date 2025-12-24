#include <ESP8266WiFi.h>
#include <espnow.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <RTClib.h>
#include "PrayTimes.h"

LiquidCrystal_I2C lcd(0x27, 16, 2);   // LCD 16x2
RTC_DS1307 rtc;
PrayTimes pt;

// Keypad 4x4
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {D5, D6, D7, D8};
byte colPins[COLS] = {D0, D3, D4, D9};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Lokasi hardcode: Sepatan Timur
double latitude = -6.1189;
double longitude = 106.6023;
int timezone = 7;
int offsetMinutes = 30;

// Password
const String menuPassword = "14000";
bool menuUnlocked = false;

// MAC penerima (ganti sesuai MAC penerima)
uint8_t receiverAddress[] = {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC};

typedef struct {
  char cmd[4];
} AcCommand;
AcCommand outCmd;

// Variabel untuk scroll jadwal
int prayerIndex = 0;
unsigned long lastScroll = 0;
const unsigned long scrollInterval = 5000; // 5 detik berganti jadwal

// ------------------- Fungsi -------------------
double inputNumber() {
  String numStr = "";
  while (true) {
    char k = keypad.getKey();
    if (k) {
      if (k == '#') break;
      if (k == '*') { if (numStr.length()>0) numStr.remove(numStr.length()-1); }
      else numStr += k;
      lcd.setCursor(0,1);
      lcd.print("                ");
      lcd.setCursor(0,1);
      lcd.print(numStr);
    }
  }
  return numStr.toDouble();
}

// Logika AC: normal untuk Subuh, Dzuhur, Ashar; khusus untuk Maghrib–Isya
bool acWindowActive(double times[], DateTime now) {
  // Subuh (0), Dzuhur (2), Ashar (3), Maghrib (5), Isya (6)
  int hSb = (int)times[0]; int mSb = (int)((times[0]-hSb)*60);
  int hDz = (int)times[2]; int mDz = (int)((times[2]-hDz)*60);
  int hAs = (int)times[3]; int mAs = (int)((times[3]-hAs)*60);
  int hMg = (int)times[5]; int mMg = (int)((times[5]-hMg)*60);
  int hIs = (int)times[6]; int mIs = (int)((times[6]-hIs)*60);

  DateTime tSb(now.year(), now.month(), now.day(), hSb, mSb, 0);
  DateTime tDz(now.year(), now.month(), now.day(), hDz, mDz, 0);
  DateTime tAs(now.year(), now.month(), now.day(), hAs, mAs, 0);
  DateTime tMg(now.year(), now.month(), now.day(), hMg, mMg, 0);
  DateTime tIs(now.year(), now.month(), now.day(), hIs, mIs, 0);

  // Normal: ON offset sebelum, OFF offset sesudah
  DateTime sbOn = tSb - TimeSpan(0,0,offsetMinutes,0);
  DateTime sbOff = tSb + TimeSpan(0,0,offsetMinutes,0);
  DateTime dzOn = tDz - TimeSpan(0,0,offsetMinutes,0);
  DateTime dzOff = tDz + TimeSpan(0,0,offsetMinutes,0);
  DateTime asOn = tAs - TimeSpan(0,0,offsetMinutes,0);
  DateTime asOff = tAs + TimeSpan(0,0,offsetMinutes,0);

  // Khusus Maghrib–Isya: ON offset sebelum Maghrib, OFF offset sesudah Isya
  DateTime mgOn = tMg - TimeSpan(0,0,offsetMinutes,0);
  DateTime isOff = tIs + TimeSpan(0,0,offsetMinutes,0);

  if ((now >= sbOn && now <= sbOff) ||
      (now >= dzOn && now <= dzOff) ||
      (now >= asOn && now <= asOff) ||
      (now >= mgOn && now <= isOff)) {
    return true;
  }
  return false;
}

bool checkPassword() {
  String input = "";
  lcd.clear(); lcd.print("Enter Password:");
  while (true) {
    char k = keypad.getKey();
    if (k) {
      if (k == '#') break; // konfirmasi
      if (k == '*') { if (input.length()>0) input.remove(input.length()-1); }
      else input += k;
      lcd.setCursor(0,1);
      lcd.print("                ");
      lcd.setCursor(0,1);
      lcd.print(input);
    }
  }
  return (input == menuPassword);
}

void sendAcCommand(bool turnOn) {
  strncpy(outCmd.cmd, turnOn ? "ON" : "OFF", 3);
  esp_now_send(receiverAddress, (uint8_t*)&outCmd, sizeof(outCmd));
}

// ------------------- Setup -------------------
void setup() {
  Serial.begin(115200);
  Wire.begin();
  lcd.init();
  lcd.backlight();
  rtc.begin();
  pt.setMethod("MWL");

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != 0) {
    lcd.clear(); lcd.print("ESP-NOW gagal");
    while (true) { delay(1000); }
  }
  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_add_peer(receiverAddress, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
}

// ------------------- Loop -------------------
void loop() {
  DateTime now = rtc.now();
  double times[sizeof(TimeName)/sizeof(char*)];
  pt.getTimes(now.year(), now.month(), now.day(), latitude, longitude, timezone, times);

  // Status AC otomatis
  bool active = acWindowActive(times, now);
  sendAcCommand(active);

  // Baris pertama: jam & tanggal
  lcd.setCursor(0,0);
  lcd.printf("%02d:%02d %02d/%02d", now.hour(), now.minute(), now.day(), now.month());

  // Scroll jadwal di baris kedua
  if (millis() - lastScroll > scrollInterval) {
    prayerIndex = (prayerIndex + 1) % 5; // bergantian 5 waktu sholat
    lastScroll = millis();
  }

  int prayerIdx[5] = {0,2,3,5,6};
  const char* prayerName[5] = {"Sb","Dz","As","Mg","Is"};
  int h = (int)times[prayerIdx[prayerIndex]];
  int m = (int)((times[prayerIdx[prayerIndex]]-h)*60);

  lcd.setCursor(0,1);
  lcd.printf("%s:%02d:%02d AC:%s", prayerName[prayerIndex], h, m, active ? "ON " : "OFF");

  // Keypad input
  char key = keypad.getKey();
  if (key) {
    if (key == '*') {
      if (checkPassword()) {
        menuUnlocked = true;
        lcd.clear(); lcd.print("Menu Unlocked");
        delay(1000);
      } else {
        menuUnlocked = false;
        lcd.clear(); lcd.print("Wrong Password");
        delay(1000);
      }
    } else if (menuUnlocked) {
      if (key == 'A') {
        sendAcCommand(true);
        lcd.clear(); lcd.print("AC Manual ON");
        delay(1000);
      } else if (key == 'B') {
        sendAcCommand(false);
        lcd.clear(); lcd.print("AC Manual OFF");
        delay(1000);
      } else if (key == 'C') {
        lcd

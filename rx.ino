#include <ESP8266WiFi.h>
#include <espnow.h>

const int relayPin = D1; // pin untuk relay AC

typedef struct {
  char cmd[4]; // "ON" / "OFF"
} AcCommand;

AcCommand inCmd;

// Callback saat data diterima dari pemancar
void onDataRecv(uint8_t * mac, uint8_t *incomingData, uint8_t len) {
  if (len >= sizeof(AcCommand)) {
    memcpy(&inCmd, incomingData, sizeof(AcCommand));
    Serial.print("Received: ");
    Serial.println(inCmd.cmd);

    if (strcmp(inCmd.cmd, "ON") == 0) {
      digitalWrite(relayPin, HIGH); // AC hidup
    } else {
      digitalWrite(relayPin, LOW);  // AC mati
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW); // default AC mati

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW init gagal");
    while (true) { delay(1000); }
  }
  esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
  esp_now_register_recv_cb(onDataRecv);

  // Cetak MAC penerima (gunakan di kode pemancar)
  Serial.print("MAC Penerima: ");
  Serial.println(WiFi.macAddress());
}

void loop() {
  // Relay dikendalikan via callback, loop kosong
  delay(100);
}

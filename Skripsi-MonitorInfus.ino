#include <WiFi.h>
#include "HX711.h"
#include <Firebase_ESP_Client.h>

// konfigurasi
const char* WIFI_SSID = "";
const char* WIFI_PASSWORD = "";
#define API_KEY ""
#define DATABASE_URL "https://infusense-default-rtdb.firebaseio.com/"

// HX711 (load cell)
#define DT 18
#define SCK 19
HX711 scale;
// float calibration_factor = 1190.00;
float calibration_factor = 1066.00;

// Firebase Objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
FirebaseJson json;

// IR sensor
const int IR_SENSOR_PIN = 13;

// inisialisasi variabel
volatile unsigned int tetesDalamMenit = 0;
volatile unsigned long lastIRMillis = 0;
volatile unsigned long lastDripMillis = 0;
volatile bool dripDetectedFlag = false;
volatile float dripInterval_ISR_float = 0.0;
const unsigned long debounceDelay = 150;

int lastKnownStableWeight = 0;
const unsigned long tpmInterval = 60000;    
unsigned long lastTPMSend = 0;
bool infusionAttached = false;
bool initialStartSent = false;
int latestTPM = 0;

// Jalur path
String firebasePatientPath = "/patients/room-001-bed-01"; 
String roomId = "room-001-bed-01";

// ISR (Interrupt Service Routine) akan berhenti menghitung tetesan jika mode penggantian aktif
void IRAM_ATTR detectTetesan() {
  unsigned long now = millis();
  unsigned long currentDripInterval_ms;

  if ((now - lastIRMillis > debounceDelay && infusionAttached)) {
    if (now - lastDripMillis == 0) {
      return;
    }

    tetesDalamMenit++;
    currentDripInterval_ms = now - lastDripMillis;
    lastDripMillis = now;

    noInterrupts();
    dripDetectedFlag = true;
    dripInterval_ISR_float = currentDripInterval_ms / 1000.0;
    interrupts();
    
    lastIRMillis = now;
  }
}

void setup() {
  Serial.begin(115200);

  scale.begin(DT, SCK);
  Serial.println("Inisialisasi Load Cell");
  while (!scale.is_ready()) {
    delay(500);
  }
  Serial.println("Load Cell siap!");

  scale.set_scale(calibration_factor);
  Serial.println("Melakukan tare, pastikan tidak ada beban di atas load cell");
  delay(3000);
  scale.tare();
  Serial.println("Tare selesai. Mulai pembacaan berat");

  pinMode(IR_SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(IR_SENSOR_PIN), detectTetesan, FALLING);
  Serial.println("Sensor IR Siap.");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi terhubung.");
  Serial.println(WiFi.localIP());

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  Firebase.signUp(&config, &auth, "", "");
  Firebase.begin(&config, &auth);

  Firebase.reconnectWiFi(true);
  sendInitialDataToFirebase(0);

  Serial.println("Menunggu 10 detik sebelum mulai monitoring");
  delay(10000);

  Serial.println("Monitoring dimulai");
}

void loop() {
  unsigned long now = millis();

  if (!Firebase.ready() || WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi atau Firebase tidak siap, coba lagi");
    delay(2000);
    return;
  }

  int currentVolume = scale.get_units(10);
  Serial.print("Cek status infus: ");
  Serial.print(currentVolume);
  Serial.println(" gram");

  // Menghitung perubahan berat relatif terhadap berat stabil terakhir
  int deltaUp = lastKnownStableWeight - currentVolume;
  float persen = 0;

  if (lastKnownStableWeight == 0) {
    persen = 0.0;
  } else {
    persen = abs((float)deltaUp / (float)lastKnownStableWeight) * 100.0;
  }
  Serial.println("deltaUp: " + String(deltaUp) + " g | lastKnown: " + String(lastKnownStableWeight) + " g | persen: " + String(persen, 2) + " %");

  // Logika deteksi infus terpasang/dilepas
  // Jika deltaUp kurang dari -10, berarti berat saat ini lebih besar dari lastKnownStableWeight 
  // yang menandakan infus baru terpasang atau infus yang sebelumnya dilepas kini ada lagi.
  if (deltaUp < -10) {
    if (!infusionAttached) {
      Serial.println("Infus terdeteksi terpasang");
      lastTPMSend = now;
      initialStartSent = false;
      int currentVolume = scale.get_units(10);

      lastKnownStableWeight = currentVolume;
    }
    infusionAttached = true;
    lastDripMillis = millis();

  } 
  // Jika persentase penurunan berat mencapai atau melebihi 10%, infus dianggap sedang dilepas atau kosong.
  else if (persen >= 10.0) {
    if (infusionAttached) {
      json.set("status", "initializing");
      json.set("volume", 0);
      json.set("drip_rate_tpm", 0);
      json.set("interval", 0);
      if (Firebase.RTDB.updateNode(&fbdo, firebasePatientPath, &json)) {
        Serial.println("Data reset berhasil dikirim.");
      } else {
        Serial.print("Gagal kirim status");
        Serial.println(fbdo.errorReason());
      }

      noInterrupts();
      tetesDalamMenit = 0;
      lastDripMillis = 0;
      interrupts();

      initialStartSent = false;
      lastKnownStableWeight = 0.0;
    }
    infusionAttached = false;
  }

  if (!infusionAttached) {
      delay(500);
      return;
  }

  // Jika flag dripDetectedFlag disetel oleh ISR, berarti ada tetesan baru
  if (dripDetectedFlag) {
      noInterrupts();
      dripDetectedFlag = false;
      float intervalToProcess = dripInterval_ISR_float;
      interrupts();

      if (infusionAttached) {
          int currentVolume = scale.get_units(10);

          sendDripDataToFirebase(currentVolume, intervalToProcess);
      }
  }

  // Kirim drip_rate_tpm tiap 60 detik
  if (now - lastTPMSend >= tpmInterval) {
    if (infusionAttached) {
      if (!initialStartSent) {
          json.set("status", "start");
          initialStartSent = true;
      }

      noInterrupts();
      latestTPM = tetesDalamMenit;
      tetesDalamMenit = 0;
      interrupts();

      int currentVolume = scale.get_units(10);

      Serial.print("Tetes per Menit: ");
      Serial.println(latestTPM);

      json.set("drip_rate_tpm", latestTPM);
      json.set("volume", currentVolume);

      String debugJsonString;
      json.toString(debugJsonString, true);
      Serial.println("Mengirim data TPM dan Status:");
      Serial.println(debugJsonString);

      if (Firebase.RTDB.updateNode(&fbdo, firebasePatientPath, &json)) {
        Serial.println("TPM berhasil dikirim ke Firebase");
      } else {
        Serial.print("TPM gagal kirim: ");
        Serial.println(fbdo.errorReason());
      }
    } 

    lastTPMSend = now;
  }

  delay(100);
}

void sendInitialDataToFirebase(int initialBerat) {
  json.add("status", "initializing");
  json.add("volume", initialBerat);
  json.add("interval", 0);
  json.add("drip_rate_tpm", 0);
  if (Firebase.RTDB.updateNode(&fbdo, firebasePatientPath, &json)) {
    Serial.println("Data awal terkirim ke Firebase");
  } else {
    Serial.print("Gagal mengirim: " + fbdo.errorReason());
  }
}

void sendDripDataToFirebase(int volume, float interval) {
  if (Firebase.ready() && WiFi.status() == WL_CONNECTED) {
    json.set("volume", volume);
    json.set("interval", interval);
    Serial.println("Mengirim data saat tetesan terdeteksi:");

    String debugStr;
    lastKnownStableWeight = volume;
    json.toString(debugStr, true);
    Serial.println(debugStr);

    if (Firebase.RTDB.updateNode(&fbdo, firebasePatientPath, &json)) {
      Serial.println("Data berhasil dikirim");
    } else {
      Serial.print("Gagal kirim: ");
      Serial.println(fbdo.errorReason());
    }
  } else {
    Serial.println("WiFi atau Firebase tidak siap, data tetesan tidak terkirim");
  }
}

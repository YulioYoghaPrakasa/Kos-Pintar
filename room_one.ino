#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <MFRC522.h>
#include <SPI.h>
#include <WiFiManager.h>
#include <Firebase_ESP_Client.h>

static unsigned long lastOnlineUpdate = 0;
unsigned long lastCheckTime = 0;
const unsigned long checkInterval = 5000; // cek tiap 5 detik

// long expiredTime = 0;
uint64_t expiredTime = 0;
unsigned long lastExpiredCheck = 0;
const unsigned long expiredCheckInterval = 3600000; // 1 jam (ms)

// RFID error tracking
unsigned long lastRFIDReset = 0;
const unsigned long rfidResetInterval = 30000; // Reset RFID setiap 30 detik jika tidak ada aktivitas

// Firebase config
#define DATABASE_URL "ta-smart-kos-default-rtdb.firebaseio.com"
#define API_KEY ""  // Kosong karena pakai test mode

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

#define RELAY1_PIN 35
#define RELAY2_PIN 32
#define BUZZER_PIN 25
#define BTN_PIN 14

int buttonState = 0;

// RFID pins
#define SS_PIN 5
#define RST_PIN 4

MFRC522 rfid(SS_PIN, RST_PIN);

const String allowedUIDs[] = { "f5453b3", "5ec8c01", "83f54092" };
const String deviceName = "room one";
bool rfidVerified = false;

// Inisialisasi LCD I2C address 0x27, ukuran 16 kolom x 2 baris
LiquidCrystal_I2C lcd(0x27, 16, 2);

HardwareSerial mySerial(2); // UART2
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

int mode = 0;
int regFingerMode = 0;

// === FUNCTION PROTOTYPES ===
void lcdPrint(String line1, String line2 = "");
void printMenu();
int findFreeID();
void enrollFingerprint(int id);
bool getFingerprintEnroll(int id);
void verifyFingerprint();
void deleteSingleFingerprint();
void deleteAllFingerprints();
void bebeep(int delayTime = 500, int repeat = 2);
void checkRFID(); // Sekarang langsung meminta scan fingerprint setelah RFID valid
void openRelayAccess(String source = "fingerprint");
void updateLatestOnline();
void checkFirebaseAccess();
void writeLog(String keterangan, int status = 1);
void checkAccess(String source = "fingerprint");
void checkFingerprintMode();
void resetRFIDModule(); // Reset RFID module setelah relay operation

void setup() {
  Serial.begin(115200);
  mySerial.begin(57600, SERIAL_8N1, 16, 17);  // RX, TX
  finger.begin(57600);
  
  while (!Serial);

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLDOWN);
  digitalWrite(RELAY1_PIN, HIGH);
  digitalWrite(RELAY2_PIN, HIGH);
  digitalWrite(BUZZER_PIN, LOW);

  lcd.init();
  lcd.backlight();

  lcdPrint("Menghubungkan", "WiFi...");
  WiFiManager wm;
  wm.resetSettings();
  if (!wm.autoConnect("RoomOne_AP")) {
    lcdPrint("Gagal konek", "Restarting...");
    delay(3000);
    ESP.restart();
  }
  lcdPrint("WiFi Terhubung", WiFi.localIP().toString());
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
  delay(2000);

  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // WIB

  // finger.begin(57600);
  if (finger.verifyPassword()) {
    Serial.println("Sensor ditemukan!");
    lcdPrint("Fingerprint", "OK");
  } else {
    lcdPrint("Fingerprint", "ERROR");
    Serial.println("Sensor tidak ditemukan :(");
    Serial.println("Cek wiring, baud rate, dan jenis sensor.");
    while (1);
  }
  delay(1000);
  Serial.println("Sensor fingerprint diinisialisasi!");
  finger.getTemplateCount();
  Serial.print("Jumlah sidik jari tersimpan: ");
  Serial.println(finger.templateCount);

  SPI.begin();
  rfid.PCD_Init();
  Serial.println("RFID diinisialisasi!");
  lcdPrint("RFID", "OK");
  delay(1000);

  // Konfigurasi Firebase (tanpa API key karena test mode)
  config.database_url = DATABASE_URL;
  config.signer.test_mode = true;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  lcdPrint("Firebase", "Terhubung...");
  if (Firebase.ready()) {
    Serial.println("Firebase connected!");
    lcdPrint("Firebase", "OK");
  } else {
    Serial.println("Firebase tidak siap!");
    lcdPrint("Firebase", "Gagal");
  }
  delay(1500);

  if (Firebase.RTDB.get(&fbdo, "/rooms/roomOne/expired")) {
    // Ambil sebagai int64_t atau double untuk menghindari overflow
    int64_t expiredMillis = fbdo.to<int64_t>();  
    expiredTime = expiredMillis / 1000; // konversi dari ms ke detik
    Serial.printf("Initial expiredTime: %lld\n", expiredTime);
  } else {
    Serial.println("Gagal ambil expired saat setup");
  }

  lastExpiredCheck = millis(); // set waktu update terakhir
  lastRFIDReset = millis(); // set waktu reset RFID terakhir

  printMenu();
}

void loop() {

  unsigned long currentMillis = millis();
  
  if (currentMillis - lastExpiredCheck >= expiredCheckInterval) {
    if (Firebase.RTDB.get(&fbdo, "/rooms/roomOne/expired")) {
      // Ambil sebagai int64_t atau double untuk menghindari overflow
      int64_t expiredMillis = fbdo.to<int64_t>();  
      expiredTime = expiredMillis / 1000; // konversi dari ms ke detik
      Serial.printf("Initial expiredTime: %lld\n", expiredTime);
    } else {
      Serial.println("Gagal ambil expired saat setup");
    }

    lastExpiredCheck = millis(); // set waktu update terakhir
  }

  checkFirebaseAccess();

  if (millis() - lastOnlineUpdate > 60000) {
    updateLatestOnline();
    lastOnlineUpdate = millis();
  }

  if (Serial.available()) {
    mode = Serial.parseInt();
    switch (mode) {
      case 1:
        lcdPrint("Registrasi...");
        registerFingerprint();
        break;
      case 2:
        lcdPrint("Verifikasi...");
        verifyFingerprint();
        break;
      case 3:
        lcdPrint("Hapus ID...");
        deleteSingleFingerprint();
        break;
      case 4:
        lcdPrint("Reset semua...");
        deleteAllFingerprints();
        break;
      default:
        lcdPrint("Pilihan tidak", "valid");
        break;
    }
    printMenu();
  } else {
    // Auto-reset RFID jika terlalu lama tidak ada aktivitas
    if (millis() - lastRFIDReset > rfidResetInterval) {
      resetRFIDModule();
      lastRFIDReset = millis();
    }

    if (finger.getImage() == FINGERPRINT_OK) {
      if (rfidVerified) {
        verifyFingerprint();
        rfidVerified = false; 
      } else {
        lcdPrint("Scan RFID", "lebih dulu");
        bebeep(500, 2);
        delay(2000);
      }
    }
        
    // Cek RFID terlebih dahulu
    checkRFID();
    
    // Jika tidak ada RFID yang di-scan, cek fingerprint mode
    if (!rfidVerified) {
      checkFingerprintMode();
    }

    if(regFingerMode == 1){
      lcdPrint("Registrasi...");
      registerFingerprint();
    }

    buttonState = digitalRead(BTN_PIN);
    Serial.print("BUTTON INTERNAL: ");
    Serial.println(buttonState);
    if(buttonState == HIGH){
      openRelayAccess("BUTTON INTERNAL");
    }

    // printMenu();
    delay(500);
  }
}

void lcdPrint(String line1, String line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
  delay(200);
}

void bebeep(int delayTime, int repeat) {
  for (int i = 0; i < repeat; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(delayTime);
    digitalWrite(BUZZER_PIN, LOW);
    delay(delayTime);
  }
}

void checkRFID() {
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  // Update waktu reset RFID karena ada aktivitas
  lastRFIDReset = millis();

  Serial.print("UID RFID: ");
  String rfidUID = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    rfidUID += String(rfid.uid.uidByte[i], HEX);
  }
  Serial.println(rfidUID);

  bool accessGranted = false;
  for (int i = 0; i < sizeof(allowedUIDs)/sizeof(allowedUIDs[0]); i++) {
    if (rfidUID.equalsIgnoreCase(allowedUIDs[i])) {
      accessGranted = true;
      break;
    }
  }

  if (accessGranted) {
    Serial.println("RFID valid, silakan verifikasi sidik jari");
    lcdPrint("RFID Valid!", "Scan Finger");
    rfidVerified = true; // ✅ izinkan fingerprint
    bebeep(200, 2);
    
    // Langsung minta scan fingerprint setelah RFID valid
    // delay(4000);
    // lcdPrint("Letakkan jari", "untuk verifikasi");
    // verifyFingerprint();
    // rfidVerified = false; // Reset setelah verifikasi
  } else {
    Serial.println("RFID tidak valid");
    lcdPrint("RFID Tidak", "valid");
    bebeep(1000, 1);
    writeLog("Gagal verifikasi kartu RFID", 0);
  }

  rfid.PICC_HaltA();
}

void openRelayAccess(String source) {
  bebeep(200, 2);
  digitalWrite(RELAY2_PIN, LOW);
  Serial.println("Relay 32 AKTIF (LOW)");
  lcdPrint("Akses dibuka");

  delay(5000);
  Serial.println("Relay 32 NONAKTIF (HIGH)");
  lcdPrint("Akses ditutup");
  digitalWrite(BUZZER_PIN, LOW);
  delay(500);
  digitalWrite(RELAY2_PIN, HIGH);

  // Update latestOpen di Firebase juga bisa di sini
  Firebase.RTDB.setTimestamp(&fbdo, "/rooms/roomOne/latestOpen");

  if (source != "") {
    writeLog("Akses dibuka menggunakan " + source);
  }

  // Reset RFID module setelah relay operation untuk mengatasi interferensi
  delay(500); // Tunggu stabilisasi power
  resetRFIDModule();
}

void printMenu() {
  Serial.println("\n=== MENU FINGERPRINT ===");
  Serial.println("1 - Daftar sidik jari (otomatis)");
  Serial.println("2 - Verifikasi sidik jari");
  Serial.println("3 - Hapus satu sidik jari");
  Serial.println("4 - Reset semua sidik jari");
  Serial.println("Pilih opsi (1-4):");
  lcdPrint("Room One","Stand By");
}

int findFreeID() {
  finger.getTemplateCount();
  for (int id = 0; id < 128; id++) {
    if (finger.loadModel(id) != FINGERPRINT_OK) {
      return id;
    }
  }
  return -1;
}

void enrollFingerprint(int id) {
  Serial.print("Mendaftarkan sidik jari ID #"); Serial.println(id);
  Serial.println("Letakkan jari pertama...");
  lcdPrint("Letak jari", "pertama...");
  while (!getFingerprintEnroll(id));
  Firebase.RTDB.setInt(&fbdo, "/settings/regFingerOne", 0);
}

bool getFingerprintEnroll(int id) {
  int p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) continue;
    if (p != FINGERPRINT_OK) {
      Serial.println("Gagal ambil gambar");
      lcdPrint("Gagal ambil", "gambar");
      return false;
    }
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    Serial.println("Gagal ke template");
    lcdPrint("Gagal ke", "template 1");
    return false;
  }

  lcdPrint("Angkat jari...", "Tunggu...");
  Serial.println("Angkat jari dan letakkan kembali...");
  delay(2000);

  p = 0;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) continue;
    if (p != FINGERPRINT_OK) {
      Serial.println("Gagal gambar ke-2");
      lcdPrint("Gagal gambar", "kedua");
      return false;
    }
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    Serial.println("Gagal ke template 2");
    lcdPrint("Gagal ke", "template 2");
    return false;
  }

  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    Serial.println("Gagal buat model");
    lcdPrint("Gagal buat", "model");
    return false;
  }

  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.println("Berhasil daftar!");
    lcdPrint("Daftar", "sukses!");
    delay(1500);
    return true;
  } else {
    Serial.println("Gagal simpan");
    lcdPrint("Gagal", "simpan");
    return false;
  }
}

void verifyFingerprint() {
  Serial.println("Letakkan jari...");
  lcdPrint("Verifikasi...");

  int p = finger.getImage();
  if (p != FINGERPRINT_OK) {
    Serial.println("Gagal gambar");
    lcdPrint("Gagal", "gambar");
    bebeep(1000, 1);
    return;
  }

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    Serial.println("Gagal ke template");
    lcdPrint("Gagal ke", "template");
    bebeep(1000, 1);
    return;
  }

  p = finger.fingerSearch();
  if (p == FINGERPRINT_OK) {
    Serial.print("Cocok ID: ");
    Serial.println(finger.fingerID);
    lcdPrint("Cocok! ID:", String(finger.fingerID));
    checkAccess("fingerprint");
  } else {
    Serial.println("Tidak cocok");
    lcdPrint("Tidak", "cocok!");
    bebeep(1000, 1);
    writeLog("Gagal verifikasi fingerprint", 0);
  }
  delay(2000);
}

void deleteSingleFingerprint() {
  Serial.println("Masukkan ID yang ingin dihapus (0-127): ");
  while (!Serial.available());
  int id = Serial.parseInt();
  if (id < 0 || id > 127) {
    Serial.println("ID tidak valid.");
    lcdPrint("ID", "tidak valid");
    return;
  }

  int p = finger.deleteModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.println("Berhasil hapus ID");
    lcdPrint("Hapus", "sukses");
  } else {
    Serial.println("Gagal hapus");
    lcdPrint("Gagal", "hapus");
  }
  delay(1500);
}

void deleteAllFingerprints() {
  bool anyDeleted = false;
  for (int id = 0; id < 128; id++) {
    if (finger.loadModel(id) == FINGERPRINT_OK) {
      finger.deleteModel(id);
      anyDeleted = true;
      Serial.print("Menghapus ID "); Serial.println(id);
    }
  }

  if (anyDeleted) {
    lcdPrint("Semua", "dihapus");
    Serial.println("Semua dihapus");
  } else {
    lcdPrint("Tidak ada", "data");
    Serial.println("Tidak ada yang dihapus");
  }
  delay(1500);
}

void updateLatestOnline() {
  if (!Firebase.ready()) return;

  if (Firebase.RTDB.setTimestamp(&fbdo, "/rooms/roomOne/latestOnline")) {
    Serial.println("latestOnline updated (server timestamp)");
  } else {
    Serial.println("Gagal update latestOnline: " + fbdo.errorReason());
  }
}

void checkFirebaseAccess() {
  if (Firebase.ready() && (millis() - lastCheckTime > checkInterval)) {
    lastCheckTime = millis();

    // Baca nilai isOpen
    if (Firebase.RTDB.getInt(&fbdo, "/rooms/roomOne/isOpen")) {
      int isOpen = fbdo.intData();

      // Baca nilai override
      int overrideCount = 0;
      if (Firebase.RTDB.getInt(&fbdo, "/rooms/roomOne/override")) {
        overrideCount = fbdo.intData();
      }

      if (isOpen == 1) {
        if (overrideCount >= 3) {
          lcdPrint("Akses dibatasi", "Override max");
          Serial.println("Akses dibatasi: override > 3");
          writeLog("Akses darurat dibatasi (override > 3)", 0);
          // Jangan buka akses
        } else {
          // Tambah override dan buka akses
          overrideCount++;
          if (Firebase.RTDB.setInt(&fbdo, "/rooms/roomOne/override", overrideCount)) {
            Serial.println("Override updated: " + String(overrideCount));
          } else {
            Serial.println("Gagal update override: " + fbdo.errorReason());
          }

          // Buka akses
          checkAccess("tombol darurat");
        }
      } else {
        // isOpen == 0, tutup akses
        digitalWrite(RELAY2_PIN, HIGH);  // Pastikan akses tertutup
        // lcdPrint("Akses ditutup");
        // Serial.println("Akses ditutup via Firebase");
      }

    } else {
      Serial.println("Gagal baca isOpen: " + fbdo.errorReason());
    }
  }
}

void writeLog(String keterangan, int status) {
  if (Firebase.ready()) {
    // Buat node unik berdasarkan waktu dan millis untuk keunikan
    String nodePath = "/logs/roomOne";

    // Data log
    FirebaseJson json;
    json.set("timestamp/.sv", "timestamp");
    json.set("keterangan", keterangan);
    json.set("status", status); // 1 = berhasil, 0 = gagal

    if (Firebase.RTDB.pushJSON(&fbdo, nodePath.c_str(), &json)) {
      Serial.println("Log tersimpan: " + keterangan);
    } else {
      Serial.println("Gagal simpan log: " + fbdo.errorReason());
    }
  }
}

void checkAccess(String source) {
  time_t nowEpoch = time(nullptr);

  if (expiredTime < nowEpoch) {
    lcdPrint("Akses Ditolak", "Kamar expired");
    writeLog("Akses ditolak: expired", 0);
    bebeep();
    return;
  }

  openRelayAccess(source);
}

void checkFingerprintMode() {
  if (Firebase.RTDB.getInt(&fbdo, "/settings/regFingerOne")) {
    regFingerMode = fbdo.intData();
    Serial.println("regFingerOne: " + String(regFingerMode));
  } else {
    Serial.println("Gagal baca regFingerOne: " + fbdo.errorReason());
  }
}

void registerFingerprint(){
  int freeID = findFreeID();
  if (freeID == -1) {
    Serial.println("Memori penuh.");
    lcdPrint("Memori", "Penuh");
  } else {
    lcdPrint("Daftar ID", String(freeID));
    enrollFingerprint(freeID);
  }
}

void resetRFIDModule() {
  Serial.println("Resetting RFID module...");
  
  // Halt RFID communication
  rfid.PICC_HaltA();
  
  // Stop SPI communication
  SPI.end();
  
  // Delay untuk stabilisasi
  delay(100);
  
  // Restart SPI communication
  SPI.begin();
  
  // Re-initialize RFID module
  rfid.PCD_Init();
  
  // Set antenna gain (optional, untuk memastikan performa optimal)
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);
  
  Serial.println("RFID module reset completed!");
}
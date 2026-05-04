#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_task_wdt.h>

// === DUPLO 10427 (2025) Remote Controller ===
//
// Uses raw NimBLE with BLE bonding — required for 10427's TI CC2642R chip.
// Legoino does NOT support this train model.
//
// Wiring:
//   D34 — 10kΩ potentiometer wiper (speed: center=stop, left=reverse, right=forward)
//   D32 — button to GND (cycles LED colors)
//   D33 — button to GND (horn + emergency stop)
//   D2  — onboard LED (solid=connected, blink=scanning)
//
// 10427 protocol (differs from older 10874/10875):
//   Motor port 0x32 (old: 0x00) — direct speed control
//   Multi port 0x34 (old: 0x11) — LED color, horn sound, app-style motor
//   BLE bonding required — without secureConnection(), commands are silently ignored

// --- Configuration ---

#define PIN_POT      34
#define PIN_BTN_ACT  32
#define PIN_BTN_STOP 33
#define PIN_LED       2
#define DEBOUNCE_MS 200

static const char* LPF2_SERVICE_UUID = "00001623-1212-efde-1623-785feabcd123";
static const char* LPF2_CHAR_UUID    = "00001624-1212-efde-1623-785feabcd123";

static const uint8_t MOTOR_PORT = 0x32;
static const uint8_t MULTI_PORT = 0x34;

static const uint8_t COLORS[] = {
  0x00, 0x01, 0x07, 0x08, 0x09, 0x0f, 0x0a, 0x0e, 0x0b, 0x0d, 0x0c
};
static const char* COLOR_NAMES[] = {
  "Off", "White", "Green", "Yellow", "LightBlue",
  "DarkBlue", "Purple", "PurplePink", "LightPink", "RedPink", "Red"
};
static const int NUM_COLORS = sizeof(COLORS);

// Dead zone boundaries for potentiometer center position
static const int POT_CENTER_LOW  = 1800;
static const int POT_CENTER_HIGH = 2300;
static const int SPEED_MIN       = 20;   // minimum speed to overcome motor friction

// --- State ---

static NimBLERemoteCharacteristic* pChar = nullptr;
static NimBLEClient* pClient = nullptr;
static NimBLEAddress trainAddress;
static volatile bool bleConnected = false;  // written from NimBLE task, read from loop()
static bool foundTrain   = false;

static bool emergencyStop = false;
static float smoothedPot  = 2048.0f;
static int  currentSpeed  = 0;
static int  lastSpeed     = 0;
static int  colorIndex    = 0;
static bool lastBtnAct    = HIGH;
static bool lastBtnStop   = HIGH;
static unsigned long lastBtnActTime  = 0;
static unsigned long lastBtnStopTime = 0;

// --- Train commands ---

static bool sendCmd(uint8_t* data, size_t len, bool needsResponse = false) {
  NimBLERemoteCharacteristic* c = pChar;  // local copy — pChar can be nulled from BLE task
  if (c && bleConnected) {
    return c->writeValue(data, len, needsResponse);
  }
  return false;
}

static void setMotorSpeed(int speed) {
  speed = constrain(speed, -100, 100);
  uint8_t cmd[] = {0x08, 0x00, 0x81, MOTOR_PORT, 0x11, 0x51, 0x00, (uint8_t)(int8_t)speed};
  // Use write-with-response for stop commands (safety-critical)
  if (!sendCmd(cmd, sizeof(cmd), speed == 0)) Serial.println("Write failed: motor");
}

static void setLedColor(uint8_t color) {
  uint8_t cmd[] = {0x0b, 0x00, 0x81, MULTI_PORT, 0x11, 0x51, 0x01, 0x04, 0x01, color, 0x00};
  if (!sendCmd(cmd, sizeof(cmd))) Serial.println("Write failed: LED");
}

static void playHorn() {
  uint8_t cmd[] = {0x0b, 0x00, 0x81, MULTI_PORT, 0x11, 0x51, 0x01, 0x07, 0x01, 0x00, 0x00};
  if (!sendCmd(cmd, sizeof(cmd))) Serial.println("Write failed: horn");
}

static int mapPotToSpeed(int adc) {
  if (adc >= POT_CENTER_LOW && adc <= POT_CENTER_HIGH) return 0;
  float normalized;
  int sign;
  if (adc > POT_CENTER_HIGH) {
    normalized = (float)(adc - POT_CENTER_HIGH) / (4095 - POT_CENTER_HIGH);
    sign = 1;
  } else {
    normalized = (float)(POT_CENTER_LOW - adc) / POT_CENTER_LOW;
    sign = -1;
  }
  int speed = SPEED_MIN + (int)(normalized * normalized * (100 - SPEED_MIN));
  return sign * speed;
}

// --- BLE ---

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* dev) override {
    if (!dev->haveServiceUUID() ||
        !dev->getServiceUUID().equals(NimBLEUUID(LPF2_SERVICE_UUID))) return;

    Serial.printf("Train found: %s\n", dev->getAddress().toString().c_str());
    trainAddress = dev->getAddress();
    foundTrain = true;
    NimBLEDevice::getScan()->stop();
  }
};

static void handleDisconnect() {
  bleConnected = false;
  pChar = nullptr;
  digitalWrite(PIN_LED, LOW);
}

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* client) override {
    Serial.println("Disconnected!");
    handleDisconnect();
  }
};

static ScanCallbacks scanCB;
static ClientCallbacks clientCB;

static void onScanComplete(NimBLEScanResults results) {
  // Called by NimBLE when a scan window finishes — loop() restarts if needed
}

static void startScan() {
  foundTrain = false;
  auto* scan = NimBLEDevice::getScan();
  scan->clearResults();
  scan->start(0, onScanComplete, false);  // continuous non-blocking scan
}

static void blinkLED(int periodMs = 600) {
  digitalWrite(PIN_LED, (millis() % periodMs) < (periodMs / 2) ? HIGH : LOW);
}

static void notifyCB(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len >= 5 && data[2] == 0x05) {
    Serial.printf("Hub error: port=0x%02x err=0x%02x\n", data[3], data[4]);
  }
}

static void connectToTrain() {
  Serial.println("Connecting...");

  // Reuse existing client or create one (NimBLE has a 3-client limit)
  if (!pClient) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(&clientCB, false);
  }
  pClient->setConnectTimeout(5);

  blinkLED(200);
  if (!pClient->connect(trainAddress)) {
    Serial.println("Connect failed!");
    return;
  }

  // BLE bonding — REQUIRED for 10427 (commands are ignored without it)
  blinkLED(200);
  if (!pClient->secureConnection()) {
    Serial.println("Bonding failed!");
    pClient->disconnect();
    return;
  }

  // Let bonding complete and connection stabilize
  for (int i = 0; i < 10; i++) { blinkLED(200); delay(50); }

  auto* svc = pClient->getService(NimBLEUUID(LPF2_SERVICE_UUID));
  if (!svc) { Serial.println("No service!"); pClient->disconnect(); return; }

  auto* chr = svc->getCharacteristic(NimBLEUUID(LPF2_CHAR_UUID));
  if (!chr) { Serial.println("No char!"); pClient->disconnect(); return; }

  if (chr->canNotify()) {
    chr->subscribe(true, notifyCB);
  }

  // Publish pChar only after everything is validated — loop() checks
  // bleConnected before dereferencing, and we set it after pChar.
  pChar = chr;
  lastSpeed = 0;
  digitalWrite(PIN_LED, HIGH);
  bleConnected = true;
  emergencyStop = false;
  Serial.println("Connected & bonded!");
  setMotorSpeed(0);  // ensure train starts stopped regardless of pot position

  // Wait for hub to finish sending port-attach notifications
  unsigned long settleStart = millis();
  while (millis() - settleStart < 1000) {
    blinkLED(200);
    delay(50);
  }

  colorIndex = 1;
  setLedColor(COLORS[colorIndex]);
}

// --- Arduino entry points ---

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== DUPLO 10427 Controller ===");

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_POT, INPUT);
  pinMode(PIN_BTN_ACT, INPUT_PULLUP);
  pinMode(PIN_BTN_STOP, INPUT_PULLUP);

  esp_task_wdt_init(10, true);  // 10s timeout, reboot on expire
  esp_task_wdt_add(NULL);

  NimBLEDevice::init("ESP32-Train");
  NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_SC);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  auto* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&scanCB);
  scan->setActiveScan(true);
  startScan();

  Serial.println("Scanning for train...");
}

void loop() {
  esp_task_wdt_reset();

  if (foundTrain && !bleConnected) {
    connectToTrain();
    if (!bleConnected) {
      for (int i = 0; i < 40; i++) { blinkLED(); delay(50); }  // 2s retry with blink
      startScan();
    }
  }

  // Blink onboard LED while not connected
  if (!bleConnected) {
    blinkLED();
    // Restart scan if it finished without finding the train
    if (!foundTrain && !NimBLEDevice::getScan()->isScanning()) {
      startScan();
    }
    delay(50);
    return;
  }

  // Periodic connection health check — detect stale connections
  if (pClient && !pClient->isConnected()) {
    Serial.println("Connection lost (stale)!");
    handleDisconnect();
    startScan();
    return;
  }

  // --- Potentiometer → speed ---
  smoothedPot = smoothedPot * 0.85f + analogRead(PIN_POT) * 0.15f;
  int potValue = (int)smoothedPot;
  currentSpeed = mapPotToSpeed(potValue);
  if (emergencyStop) {
    if (currentSpeed == 0) emergencyStop = false;  // pot returned to center — unlatch
  } else if (abs(currentSpeed - lastSpeed) >= 5 || (currentSpeed == 0 && lastSpeed != 0)) {
    setMotorSpeed(currentSpeed);
    lastSpeed = currentSpeed;
    Serial.printf("Speed: %d (pot=%d)\n", currentSpeed, potValue);
  }

  // --- Button 1: cycle LED colors ---
  bool btnAct = digitalRead(PIN_BTN_ACT);
  if (btnAct == LOW && lastBtnAct == HIGH && millis() - lastBtnActTime > DEBOUNCE_MS) {
    colorIndex = (colorIndex + 1) % NUM_COLORS;
    setLedColor(COLORS[colorIndex]);
    Serial.printf("Color: %s\n", COLOR_NAMES[colorIndex]);
    lastBtnActTime = millis();
  }
  lastBtnAct = btnAct;

  // --- Button 2: horn + emergency stop ---
  bool btnStop = digitalRead(PIN_BTN_STOP);
  if (btnStop == LOW && lastBtnStop == HIGH && millis() - lastBtnStopTime > DEBOUNCE_MS) {
    playHorn();
    if (currentSpeed != 0) {
      delay(50);
      setMotorSpeed(0);
      lastSpeed = 0;
      emergencyStop = true;
      Serial.println("HORN + STOP!");
    } else {
      Serial.println("HORN!");
    }
    lastBtnStopTime = millis();
  }
  lastBtnStop = btnStop;

  delay(50);
}

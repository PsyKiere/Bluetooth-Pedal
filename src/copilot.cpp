/*************************************************************
 * ESP32 BLE HID Pedal Board – "PipoLaPipe"
 * - Ground-to-press switches with internal pull-ups
 * - NimBLE HID Keyboard (Android-friendly, low power)
 * - Light sleep between events; deep sleep after inactivity
 * - Double-click: Concert mode (LEDs off)
 * - Triple-click: Pairing mode (clear bonds, fast advertising)
 * - Hold 3s: Deep sleep (wake via function button only)
 *************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

// NimBLE
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEHIDDevice.h>

// ---------------- Pin map (GROUND-to-press) ----------------
// Pedals (use these names exactly as requested)
constexpr uint8_t main_pedal = 13;  // PageDown
constexpr uint8_t module1    = 26;  // PageUp
constexpr uint8_t module2    = 27;  // Right Arrow
constexpr uint8_t module3    = 14;  // Left Arrow

// Main function button (wake from deep sleep)
constexpr gpio_num_t function_btn = GPIO_NUM_25; // press -> LOW

// LEDs
constexpr uint8_t LED_POWER = 32; // active-high
constexpr uint8_t LED_BT    = 33; // active-low

// ---------------- Behavior tuning ----------------
constexpr uint32_t INACTIVITY_TIMEOUT_MS = 20UL * 60UL * 1000UL; // 20 minutes
constexpr uint32_t LONG_PRESS_MS         = 3000;                 // deep sleep
constexpr uint32_t DOUBLE_CLICK_WINDOW   = 400;                  // ms
constexpr uint32_t TRIPLE_CLICK_WINDOW   = 600;                  // ms from first click
constexpr uint32_t DEBOUNCE_MS           = 8;                    // small guard on top of RC

// Light-sleep tick (blink, housekeeping)
constexpr uint32_t IDLE_TICK_MS          = 100;  // wake at least every 100 ms

// HID keycodes (USB HID Usage IDs)
constexpr uint8_t KEY_NONE      = 0x00;
constexpr uint8_t KEY_PGUP      = 0x4B;
constexpr uint8_t KEY_PGDN      = 0x4E;
constexpr uint8_t KEY_RIGHT     = 0x4F;
constexpr uint8_t KEY_LEFT      = 0x50;

// ---------------- Globals ----------------
RTC_DATA_ATTR bool concertMode = false;  // persists across deep sleep
RTC_DATA_ATTR uint32_t bootCount = 0;

static NimBLEServer*      pServer       = nullptr;
static NimBLEHIDDevice*   pHID          = nullptr;
static NimBLECharacteristic* inputReport = nullptr;

static volatile bool isConnected = false;
static bool pairingMode = false;

static uint32_t lastActivityMs = 0;
static uint32_t lastBlinkMs = 0;
static bool btBlinkState = false;

// Button state tracking (for debounce and click detection)
struct BtnState {
  uint8_t pin;
  bool    lastLevel;
  uint32_t lastChangeMs;
  uint32_t pressedAtMs;
  uint8_t  clickCount;
};
static BtnState fnBtn { (uint8_t)function_btn, true, 0, 0, 0 };

// Prototypes
void setupBLE();
void startAdvertising(bool fast);
void clearBondsAndPair();
void updateLEDs();
void setPowerLED(bool on);
void setBTLED(bool on);
void handleFunctionButton();
void handlePedals();
void sendKey(uint8_t keycode);
bool isPressed(uint8_t pin);
void lightSleepIdle(uint32_t maxMs);
void enterDeepSleepNow();
void configureDeepSleepWake();
void configureLightSleepWake();

// ---------------- Utility: LEDs ----------------
inline void setPowerLED(bool on) {
  if (concertMode) { digitalWrite(LED_POWER, LOW); return; }
  digitalWrite(LED_POWER, on ? HIGH : LOW);
}
inline void setBTLED(bool on) {
  if (concertMode) { digitalWrite(LED_BT, HIGH); return; } // active-low OFF
  digitalWrite(LED_BT, on ? LOW : HIGH); // active-low
}

void updateLEDs() {
  // Power LED: ON when awake unless concert mode
  setPowerLED(true);

  // BT LED depends on connection/pairing state (unless concert mode)
  if (concertMode) return;

  if (isConnected) {
    setBTLED(true); // solid
  } else if (pairingMode) {
    // 2 Hz blink -> toggle every 250 ms
    uint32_t now = millis();
    if (now - lastBlinkMs >= 250) {
      lastBlinkMs = now;
      btBlinkState = !btBlinkState;
      setBTLED(btBlinkState);
    }
  } else {
    setBTLED(false); // off when not connected and not pairing
  }
}

// ---------------- BLE HID Keyboard ----------------
void setupBLE() {
  NimBLEDevice::init("PipoLaPipe");
  NimBLEDevice::setPower(ESP_PWR_LVL_P7); // moderate TX power
  NimBLEDevice::setSecurityAuth(true, true, true); // bonding, MITM, secure
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  pServer = NimBLEDevice::createServer();

  pServer->setCallbacks(new struct : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, ble_gap_conn_desc* desc) override {
      isConnected = true;
      pairingMode = false; // auto-exit pairing on connect
      lastActivityMs = millis();
    }
    void onDisconnect(NimBLEServer* s) override {
      isConnected = false;
      lastActivityMs = millis();
      // Resume normal advertising
      startAdvertising(false);
    }
  });

  pHID = new NimBLEHIDDevice(pServer);
  pHID->manufacturer()->setValue("Pipo");
  pHID->pnp(0x02, 0x05AC, 0x820A, 0x0100); // generic
  pHID->hidInfo(0x00, 0x01);

  // Simple keyboard report map
  static const uint8_t reportMap[] = {
    0x05, 0x01,       // USAGE_PAGE (Generic Desktop)
    0x09, 0x06,       // USAGE (Keyboard)
    0xA1, 0x01,       // COLLECTION (Application)
    0x85, 0x01,       //   REPORT_ID (1)
    0x05, 0x07,       //   USAGE_PAGE (Keyboard)
    0x19, 0xE0,       //   USAGE_MINIMUM (Keyboard LeftControl)
    0x29, 0xE7,       //   USAGE_MAXIMUM (Keyboard Right GUI)
    0x15, 0x00,       //   LOGICAL_MINIMUM (0)
    0x25, 0x01,       //   LOGICAL_MAXIMUM (1)
    0x75, 0x01,       //   REPORT_SIZE (1)
    0x95, 0x08,       //   REPORT_COUNT (8)
    0x81, 0x02,       //   INPUT (Data,Var,Abs) ; Modifier byte
    0x95, 0x01,       //   REPORT_COUNT (1)
    0x75, 0x08,       //   REPORT_SIZE (8)
    0x81, 0x03,       //   INPUT (Cnst,Var,Abs) ; Reserved
    0x95, 0x06,       //   REPORT_COUNT (6)
    0x75, 0x08,       //   REPORT_SIZE (8)
    0x15, 0x00,       //   LOGICAL_MINIMUM (0)
    0x25, 0x65,       //   LOGICAL_MAXIMUM (101)
    0x05, 0x07,       //   USAGE_PAGE (Keyboard)
    0x19, 0x00,       //   USAGE_MINIMUM (Reserved)
    0x29, 0x65,       //   USAGE_MAXIMUM (Keyboard Application)
    0x81, 0x00,       //   INPUT (Data,Ary,Abs) ; 6 key array
    0xC0              // END_COLLECTION
  };
  pHID->reportMap((uint8_t*)reportMap, sizeof(reportMap));
  pHID->start();

  inputReport = pHID->inputReport(1);

  startAdvertising(false);
}

void startAdvertising(bool fast) {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();

  adv->setAppearance(HID_KEYBOARD);
  adv->addServiceUUID(pHID->hidService()->getUUID());
  adv->setScanResponse(true);
  adv->setMinInterval(fast ? 0x20 : 0x80); // 20 ms vs 100 ms
  adv->setMaxInterval(fast ? 0x30 : 0x100);
  adv->start();
}

void clearBondsAndPair() {
  NimBLEDevice::deleteAllBonds();
  pairingMode = true;
  startAdvertising(true);
}

// Send a single key press + release
void sendKey(uint8_t keycode) {
  if (!isConnected) return;

  uint8_t report[8] = {0}; // [mods, reserved, k1..k6]
  // Press
  report[2] = keycode;
  inputReport->setValue(report, sizeof(report));
  inputReport->notify();

  delay(12); // brief
  // Release
  memset(report, 0, sizeof(report));
  inputReport->setValue(report, sizeof(report));
  inputReport->notify();
}

// ---------------- Button helpers ----------------
bool isPressed(uint8_t pin) {
  // Ground-to-press -> LOW when pressed
  return digitalRead(pin) == LOW;
}

// Debounced read for function button with click detection
void handleFunctionButton() {
  uint32_t now = millis();
  bool level = digitalRead((uint8_t)function_btn);
  if (level != fnBtn.lastLevel) {
    if (now - fnBtn.lastChangeMs >= DEBOUNCE_MS) {
      fnBtn.lastLevel = level;
      fnBtn.lastChangeMs = now;

      if (level == LOW) {
        // pressed
        fnBtn.pressedAtMs = now;
      } else {
        // released -> count click
        uint32_t pressDur = now - fnBtn.pressedAtMs;
        fnBtn.clickCount++;
        lastActivityMs = now;

        // Long press supersedes clicks if held long enough
        if (pressDur >= LONG_PRESS_MS) {
          // Deep sleep now
          enterDeepSleepNow();
          return;
        }
      }
    }
  }

  // Interpret clicks in windows
  // If no further clicks within window, execute action
  if (fnBtn.clickCount > 0) {
    uint32_t window = (fnBtn.clickCount >= 2) ? TRIPLE_CLICK_WINDOW : DOUBLE_CLICK_WINDOW;
    if (now - fnBtn.lastChangeMs > window) {
      uint8_t cc = fnBtn.clickCount;
      fnBtn.clickCount = 0;

      if (cc == 2) {
        // Toggle concert mode
        concertMode = !concertMode;
      } else if (cc >= 3) {
        // Pairing mode: disconnect, clear bonds, fast advertising
        if (isConnected) {
          NimBLEDevice::getServer()->disconnectAll();
          delay(50);
        }
        clearBondsAndPair();
      }
    }
  }
}

// Handle pedals -> send keys
void handlePedals() {
  static struct {
    uint8_t pin, key;
    bool last;
    uint32_t lastChange;
  } ped[4] = {
    { main_pedal, KEY_PGDN, true, 0 },
    { module1,    KEY_PGUP, true, 0 },
    { module2,    KEY_RIGHT,true, 0 },
    { module3,    KEY_LEFT, true, 0 }
  };

  uint32_t now = millis();
  for (int i = 0; i < 4; ++i) {
    bool lvl = digitalRead(ped[i].pin);
    if (lvl != ped[i].last && (now - ped[i].lastChange) >= DEBOUNCE_MS) {
      ped[i].last = lvl;
      ped[i].lastChange = now;
      if (lvl == LOW) { // pressed
        lastActivityMs = now;
        sendKey(ped[i].key);
      }
    }
  }
}

// ---------------- Power management ----------------
void configureLightSleepWake() {
  // Use GPIO low-level wake for ground-to-press pins
  // Enable per-pin wake on LOW level
  gpio_wakeup_enable((gpio_num_t)main_pedal, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)module1,    GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)module2,    GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)module3,    GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(function_btn,           GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
}

void lightSleepIdle(uint32_t maxMs) {
  // Timer wake alongside GPIO to keep periodic tasks alive
  esp_sleep_enable_timer_wakeup((uint64_t)maxMs * 1000ULL);
  esp_light_sleep_start();
  // Upon wake, timer or GPIO brought us back
}

void configureDeepSleepWake() {
  // Only the function button should wake deep sleep
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup(function_btn, 0); // wake when goes LOW
}

void enterDeepSleepNow() {
  // Graceful BLE disconnect
  if (isConnected) {
    NimBLEDevice::getServer()->disconnectAll();
    delay(50);
  }
  NimBLEDevice::stopAdvertising();

  // LEDs off
  digitalWrite(LED_POWER, LOW);
  digitalWrite(LED_BT, HIGH); // active-low off

  configureDeepSleepWake();
  Serial.println("Entering deep sleep...");
  Serial.flush();
  esp_deep_sleep_start();
}

// ---------------- Setup & Loop ----------------
void setup() {
  ++bootCount;

  // Pre-set LED OFF levels before switching to OUTPUT (avoid flicker)
  digitalWrite(LED_POWER, LOW);
  digitalWrite(LED_BT, HIGH); // active-low off
  pinMode(LED_POWER, OUTPUT);
  pinMode(LED_BT, OUTPUT);

  // Inputs (GROUND-to-press)
  pinMode(main_pedal, INPUT_PULLUP);
  pinMode(module1,    INPUT_PULLUP);
  pinMode(module2,    INPUT_PULLUP);
  pinMode(module3,    INPUT_PULLUP);
  pinMode((uint8_t)function_btn, INPUT_PULLUP);

  // Disable Wi-Fi to save power
  WiFi.mode(WIFI_OFF);
  btStop(); // ensure BT controller not running before NimBLE init
  delay(10);
  btStart(); // start fresh for NimBLE

  Serial.begin(115200);
  delay(100);
  Serial.printf("Boot #%lu. Wake cause: %d\n", (unsigned long)bootCount, (int)esp_sleep_get_wakeup_cause());

  // NimBLE HID keyboard
  setupBLE();

  // Light sleep wake sources
  configureLightSleepWake();

  lastActivityMs = millis();

  // Initial LED state
  updateLEDs();
}

void loop() {
  // Handle UI
  handleFunctionButton();
  handlePedals();

  // LEDs (blink state if pairing)
  updateLEDs();

  // Inactivity -> deep sleep
  if ((millis() - lastActivityMs) >= INACTIVITY_TIMEOUT_MS) {
    enterDeepSleepNow();
  }

  // Idle: light sleep for short tick; GPIO wakes immediately on any press
  lightSleepIdle(IDLE_TICK_MS);
}

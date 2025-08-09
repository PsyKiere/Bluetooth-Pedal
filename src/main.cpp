// Minimal ESP32 BLE Page Turner (reset-safe)
// - Keeps original pin assignments
// - Features:
//   * BLE keyboard advertising/pairing
//   * On/Off via long-press on MAIN_BUTTON_PIN (deep sleep)
//   * Turn page right on MODULE2_PIN press (sends Right Arrow once per press)

// =============================================================================
// LIBRARIES
// =============================================================================
#include <Arduino.h>
#include <BleKeyboard.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"


// =============================================================================
// CONSTANTS
// =============================================================================
static const uint32_t LONG_PRESS_MS = 3000;
static const uint16_t BT_LED_BLINK_INTERVAL_MS = 500;
static const uint8_t KEY_PRESS_DELAY_MS = 30;


// =============================================================================
// PIN DEFINITIONS
// =============================================================================
// --- LEDs ---
#define POWER_LED_PIN   32
#define BT_LED_PIN      33

// --- Main Control Button ---
#define MAIN_BUTTON_PIN 25

// --- Pedals (we will only use MODULE2_PIN for Right Arrow in this minimal build) ---
#define MAIN_PEDAL_PIN  13 // Down Arrow (unused in minimal)
#define MODULE1_PIN     26 // Left Arrow (unused in minimal)
#define MODULE2_PIN     27 // Right Arrow (used)
#define MODULE3_PIN     14 // Up Arrow (unused in minimal)

// =============================================================================
// GLOBAL STATE
// =============================================================================
BleKeyboard bleKeyboard("PipoLaPipe", "ESP32-Pedal", 100);


// =============================================================================
// HELPERS
// =============================================================================
static inline bool isActiveLowPressed(int pin) { return digitalRead(pin) == LOW; }
void goToDeepSleep();
void handleMainButtonDeepSleep();
void handleRightArrowPedal(bool isConnected);
void updateLedStatus(bool isConnected);

// =============================================================================
// SETUP - Runs once on boot/reset
// =============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("Starting ESP32 Page Turner...");
  // Diagnostic: last reset reason and wakeup cause
  Serial.printf("Reset reason: %d, wakeup cause: %d\n", (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause());

  // If previous sleep enabled RTC hold on MAIN_BUTTON_PIN, release it now
  rtc_gpio_hold_dis(GPIO_NUM_25);

  // --- Configure Pins ---
  pinMode(POWER_LED_PIN, OUTPUT);
  pinMode(BT_LED_PIN, OUTPUT);
  pinMode(MAIN_BUTTON_PIN, INPUT_PULLUP);
  
  pinMode(MAIN_PEDAL_PIN, INPUT_PULLUP);
  pinMode(MODULE1_PIN, INPUT_PULLUP);
  pinMode(MODULE2_PIN, INPUT_PULLUP);
  pinMode(MODULE3_PIN, INPUT_PULLUP);

  digitalWrite(POWER_LED_PIN, HIGH); // Turn power LED on immediately
  
  // --- Start Bluetooth ---
  bleKeyboard.begin();
}

// =============================================================================
// MAIN LOOP - Runs repeatedly
// =============================================================================
void loop() {
  bool connected = bleKeyboard.isConnected();

  handleMainButtonDeepSleep();
  handleRightArrowPedal(connected);
  updateLedStatus(connected);

  delay(5);
}

// =============================================================================
// CUSTOM FUNCTIONS
// =============================================================================

// --- Enter deep sleep ---
void goToDeepSleep() {
  Serial.println("Entering deep sleep now.");
  digitalWrite(POWER_LED_PIN, LOW);
  digitalWrite(BT_LED_PIN, LOW);

  // Ensure RTC domain keeps the pull-up on GPIO 25 during deep sleep
  rtc_gpio_init(GPIO_NUM_25);
  rtc_gpio_set_direction(GPIO_NUM_25, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(GPIO_NUM_25);
  rtc_gpio_pulldown_dis(GPIO_NUM_25);
  rtc_gpio_hold_en(GPIO_NUM_25);

  // Wake when pin goes LOW (button pressed)
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_25, 0);
  delay(20);
  esp_deep_sleep_start();
}

// --- On/Off via long-press ---
void handleMainButtonDeepSleep() {
  static bool wasPressed = false;
  static unsigned long pressStartMs = 0;

  bool pressed = isActiveLowPressed(MAIN_BUTTON_PIN);

  // Edge: press down
  if (pressed && !wasPressed) {
    wasPressed = true;
    pressStartMs = millis();
  }

  // While held: check for long-press and power off
  if (pressed && wasPressed) {
    if (millis() - pressStartMs >= LONG_PRESS_MS) {
      goToDeepSleep();
    }
  }

  // Edge: release
  if (!pressed && wasPressed) {
    wasPressed = false;
  }
}

// --- Right Arrow pedal on MODULE2_PIN ---
void handleRightArrowPedal(bool isConnected) {
  static bool wasPressed = false;

  if (!isConnected) {
    wasPressed = false;
    return;
  }

  bool pressed = isActiveLowPressed(MODULE2_PIN);
  if (pressed && !wasPressed) {
    bleKeyboard.press(KEY_RIGHT_ARROW);
    delay(KEY_PRESS_DELAY_MS);
    bleKeyboard.releaseAll();
    wasPressed = true;
  } else if (!pressed) {
    wasPressed = false;
  }
}

// --- LEDs: power always on; BT solid when connected, blink when not ---
void updateLedStatus(bool isConnected) {
  static unsigned long btLedLastToggleMs = 0;
  static bool btLedState = false;

  if (isConnected) {
    digitalWrite(BT_LED_PIN, HIGH);
    btLedState = true;
  } else {
    unsigned long now = millis();
    if (now - btLedLastToggleMs >= BT_LED_BLINK_INTERVAL_MS) {
      btLedLastToggleMs = now;
      btLedState = !btLedState;
      digitalWrite(BT_LED_PIN, btLedState ? HIGH : LOW);
    }
  }
}
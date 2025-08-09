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

static const uint32_t LONG_PRESS_MS = 3000;

bool mainButtonWasPressed = false;
unsigned long mainButtonPressStartMs = 0;
bool sleepArmed = false; // set after long-press; sleep on release

bool module2WasPressed = false; // For edge detection

unsigned long btLedLastToggleMs = 0;
bool btLedState = false;

// =============================================================================
// HELPERS
// =============================================================================
static inline bool isActiveLowPressed(int pin) { return digitalRead(pin) == LOW; }
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

// --- On/Off via long-press ---
void handleMainButtonDeepSleep() {
  bool pressed = isActiveLowPressed(MAIN_BUTTON_PIN);

  // Edge: press down
  if (pressed && !mainButtonWasPressed) {
    mainButtonWasPressed = true;
    mainButtonPressStartMs = millis();
  }

  // While held: check for long-press
  if (pressed && mainButtonWasPressed && !sleepArmed) {
    if (millis() - mainButtonPressStartMs >= LONG_PRESS_MS) {
      sleepArmed = true;
      Serial.println("Long-press detected. Release button to power off...");
    }
  }

  // Edge: release
  if (!pressed && mainButtonWasPressed) {
    mainButtonWasPressed = false;
    if (sleepArmed) {
      // Debounce release and ensure line stays HIGH before arming wake
      unsigned long stableStart = millis();
      while (millis() - stableStart < 200) {
        if (isActiveLowPressed(MAIN_BUTTON_PIN)) {
          // Bounced back to LOW; cancel sleep arming
          sleepArmed = false;
          Serial.println("Power-off canceled due to button bounce.");
          return;
        }
        delay(5);
      }

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
    sleepArmed = false;
  }
}

// --- Right Arrow pedal on MODULE2_PIN ---
void handleRightArrowPedal(bool isConnected) {
  if (!isConnected) {
    module2WasPressed = false;
    return;
  }

  bool pressed = isActiveLowPressed(MODULE2_PIN);
  if (pressed && !module2WasPressed) {
    bleKeyboard.press(KEY_RIGHT_ARROW);
    delay(30);
    bleKeyboard.releaseAll();
    module2WasPressed = true;
  } else if (!pressed) {
    module2WasPressed = false;
  }
}

// --- LEDs: power always on; BT solid when connected, blink when not ---
void updateLedStatus(bool isConnected) {
  digitalWrite(POWER_LED_PIN, HIGH);
  if (isConnected) {
    digitalWrite(BT_LED_PIN, HIGH);
    btLedState = true;
  } else {
    unsigned long now = millis();
    if (now - btLedLastToggleMs >= 500) {
      btLedLastToggleMs = now;
      btLedState = !btLedState;
      digitalWrite(BT_LED_PIN, btLedState ? HIGH : LOW);
    }
  }
}
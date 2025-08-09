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
static const uint32_t LONG_PRESS_OFF_MS = 3000;
static const uint32_t LONG_PRESS_ON_MS = 2000;
static const uint32_t IDLE_TIMEOUT_MS = 5000;
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
unsigned long lastActivityMs = 0;


// =============================================================================
// HELPERS
// =============================================================================
static inline bool isActiveLowPressed(int pin) { return digitalRead(pin) == LOW; }
void enterDeepSleep();
void goToDeepSleep();
void handleIdleLightSleep(bool isConnected);
void handleMainButtonDeepSleep();
void handleRightArrowPedal(bool isConnected);
void updateLedStatus(bool isConnected);

// =============================================================================
// SETUP - Runs once on boot/reset
// =============================================================================
void setup() {
  // Configure the main button pin early for the power-on check
  pinMode(MAIN_BUTTON_PIN, INPUT_PULLUP);

  // --- Power-on check: require long press to boot from deep sleep ---
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    unsigned long pressStartMs = millis();
    while (isActiveLowPressed(MAIN_BUTTON_PIN)) {
      if (millis() - pressStartMs >= LONG_PRESS_ON_MS) {
        goto continue_boot; // Long press confirmed, continue booting.
      }
      delay(10);
    }
    // If the loop finishes, the button was released too early. Go back to sleep.
    Serial.println("Power-on press was too short. Going back to sleep.");
    enterDeepSleep();
  }

continue_boot:
  lastActivityMs = millis();
  Serial.begin(115200);
  Serial.println("Starting ESP32 Page Turner...");
  // Diagnostic: last reset reason and wakeup cause
  Serial.printf("Reset reason: %d, wakeup cause: %d\n", (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause());

  // If previous sleep enabled RTC hold on MAIN_BUTTON_PIN, release it now
  rtc_gpio_hold_dis(GPIO_NUM_25);

  // --- Configure Pins ---
  pinMode(POWER_LED_PIN, OUTPUT);
  pinMode(BT_LED_PIN, OUTPUT);
  
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

  handleIdleLightSleep(connected);
  handleMainButtonDeepSleep();
  handleRightArrowPedal(connected);
  updateLedStatus(connected);

  delay(5);
}

// =============================================================================
// CUSTOM FUNCTIONS
// =============================================================================

void handleIdleLightSleep(bool isConnected) {
  // Only enter light sleep if connected and idle for the timeout period
  if (!isConnected || (millis() - lastActivityMs < IDLE_TIMEOUT_MS)) {
    return;
  }

  Serial.println("Idle timeout. Entering light sleep.");

  // Turn off LEDs before sleeping
  digitalWrite(POWER_LED_PIN, LOW);
  digitalWrite(BT_LED_PIN, LOW);

  // Configure wakeup sources: wake on main button or pedal press (LOW)
  uint64_t wakeup_mask = (1ULL << MAIN_BUTTON_PIN) | (1ULL << MODULE2_PIN);
  esp_sleep_enable_ext1_wakeup(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW);

  esp_light_sleep_start();

  // --- WOKE UP FROM LIGHT SLEEP ---
  uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();

  // No need to disable ext1 wakeup, it's configured per-sleep
  // gpio_wakeup_disable((gpio_num_t)MAIN_BUTTON_PIN);
  // gpio_wakeup_disable((gpio_num_t)MODULE2_PIN);

  Serial.println("Woke up from light sleep.");

  // Debounce by waiting for the wakeup button to be released
  while(isActiveLowPressed(MAIN_BUTTON_PIN) || isActiveLowPressed(MODULE2_PIN)) {
    delay(10);
  }

  // Reset keyboard state in case it got stuck during sleep
  bleKeyboard.releaseAll();

  // Reset idle timer
  lastActivityMs = millis();

  // If the page turner pedal woke the device, perform its action.
  // The main button's only job on wake is to wake the device, no other action needed.
  if (wakeup_pin_mask & (1ULL << MODULE2_PIN)) {
    Serial.println("Woken up by pedal. Sending key press.");
    bleKeyboard.press(KEY_RIGHT_ARROW);
    delay(KEY_PRESS_DELAY_MS);
    bleKeyboard.releaseAll();
  }
}

// --- Enter deep sleep ---
void enterDeepSleep() {
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

void goToDeepSleep() {
  Serial.println("Long-press detected. Powering down. Release button to sleep.");
  digitalWrite(POWER_LED_PIN, LOW);
  digitalWrite(BT_LED_PIN, LOW);

  // Wait for button release to avoid immediate wakeup
  while(isActiveLowPressed(MAIN_BUTTON_PIN)) {
    delay(10);
  }
  delay(50); // Debounce release

  Serial.println("Button released. Entering deep sleep now.");
  enterDeepSleep();
}

// --- On/Off via long-press ---
void handleMainButtonDeepSleep() {
  static bool wasPressed = false;
  static unsigned long pressStartMs = 0;
  static bool sleepTriggered = false;

  bool pressed = isActiveLowPressed(MAIN_BUTTON_PIN);

  // Edge: press down
  if (pressed && !wasPressed) {
    wasPressed = true;
    pressStartMs = millis();
    sleepTriggered = false; // Reset on new press
    lastActivityMs = millis();
  }

  // While held: check for long-press and power off
  if (pressed && wasPressed && !sleepTriggered) {
    if (millis() - pressStartMs >= LONG_PRESS_OFF_MS) {
      sleepTriggered = true;
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
    lastActivityMs = millis();
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

  // Power LED is always on when the device is awake.
  // It's turned off before light sleep and this ensures it's restored.
  digitalWrite(POWER_LED_PIN, HIGH);

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
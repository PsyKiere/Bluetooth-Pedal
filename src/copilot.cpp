#include <Arduino.h>
#include <BleKeyboard.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"

// =============================================================================
// CONSTANTS
// =============================================================================
static const uint32_t LONG_PRESS_OFF_MS = 3000;
static const uint32_t LONG_PRESS_ON_MS = 2000;
static const uint16_t BT_LED_BLINK_INTERVAL_MS = 500;
static const uint8_t KEY_PRESS_DELAY_MS = 30;

// =============================================================================
// PIN DEFINITIONS
// =============================================================================
#define POWER_LED_PIN   32
#define BT_LED_PIN      33
#define MAIN_BUTTON_PIN 25

#define MAIN_PEDAL_PIN  13 // Down Arrow
#define MODULE1_PIN     26 // Left Arrow
#define MODULE2_PIN     27 // Right Arrow
#define MODULE3_PIN     14 // Up Arrow

// =============================================================================
// GLOBAL STATE
// =============================================================================
BleKeyboard bleKeyboard("PipoLaPipe", "ESP32-Pedal", 100);

// =============================================================================
// HELPERS
// =============================================================================
static inline bool isActiveLowPressed(int pin) { return digitalRead(pin) == LOW; }
void enterDeepSleep();
void goToDeepSleep();
void handleMainButtonDeepSleep();
void handlePedals(bool isConnected);
void updateLedStatus(bool isConnected);

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  pinMode(MAIN_BUTTON_PIN, INPUT_PULLUP);

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    unsigned long pressStartMs = millis();
    while (isActiveLowPressed(MAIN_BUTTON_PIN)) {
      if (millis() - pressStartMs >= LONG_PRESS_ON_MS) {
        goto continue_boot;
      }
      delay(10);
    }
    enterDeepSleep();
  }

continue_boot:
  Serial.begin(115200);
  Serial.println("Starting ESP32 Page Turner...");
  Serial.printf("Reset reason: %d, wakeup cause: %d\n", (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause());

  rtc_gpio_hold_dis(GPIO_NUM_25);

  pinMode(POWER_LED_PIN, OUTPUT);
  pinMode(BT_LED_PIN, OUTPUT);

  pinMode(MAIN_PEDAL_PIN, INPUT_PULLUP);
  pinMode(MODULE1_PIN, INPUT_PULLUP);
  pinMode(MODULE2_PIN, INPUT_PULLUP);
  pinMode(MODULE3_PIN, INPUT_PULLUP);

  digitalWrite(POWER_LED_PIN, HIGH);

  bleKeyboard.begin();
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
  bool connected = bleKeyboard.isConnected();

  handleMainButtonDeepSleep();
  handlePedals(connected);
  updateLedStatus(connected);

  delay(5);
}

// =============================================================================
// FUNCTIONS
// =============================================================================
void enterDeepSleep() {
  rtc_gpio_init(GPIO_NUM_25);
  rtc_gpio_set_direction(GPIO_NUM_25, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(GPIO_NUM_25);
  rtc_gpio_pulldown_dis(GPIO_NUM_25);
  rtc_gpio_hold_en(GPIO_NUM_25);

  esp_sleep_enable_ext0_wakeup(GPIO_NUM_25, 0);
  delay(20);
  esp_deep_sleep_start();
}

void goToDeepSleep() {
  Serial.println("Long-press detected. Powering down. Release button to sleep.");
  digitalWrite(POWER_LED_PIN, LOW);
  digitalWrite(BT_LED_PIN, LOW);

  while(isActiveLowPressed(MAIN_BUTTON_PIN)) {
    delay(10);
  }
  delay(50);
  Serial.println("Button released. Entering deep sleep now.");
  enterDeepSleep();
}

void handleMainButtonDeepSleep() {
  static bool wasPressed = false;
  static unsigned long pressStartMs = 0;
  static bool sleepTriggered = false;

  bool pressed = isActiveLowPressed(MAIN_BUTTON_PIN);

  if (pressed && !wasPressed) {
    wasPressed = true;
    pressStartMs = millis();
    sleepTriggered = false;
  }

  if (pressed && wasPressed && !sleepTriggered) {
    if (millis() - pressStartMs >= LONG_PRESS_OFF_MS) {
      sleepTriggered = true;
      goToDeepSleep();
    }
  }

  if (!pressed && wasPressed) {
    wasPressed = false;
  }
}

void handlePedals(bool isConnected) {
  static bool wasPressed[4] = {false, false, false, false};
  const int pins[4] = {MAIN_PEDAL_PIN, MODULE1_PIN, MODULE2_PIN, MODULE3_PIN};
  const uint8_t keycodes[4] = {
    KEY_DOWN_ARROW,
    KEY_LEFT_ARROW,
    KEY_RIGHT_ARROW,
    KEY_UP_ARROW
  };

  if (!isConnected) {
    for (int i = 0; i < 4; ++i) wasPressed[i] = false;
    return;
  }

  for (int i = 0; i < 4; ++i) {
    bool pressed = isActiveLowPressed(pins[i]);
    if (pressed && !wasPressed[i]) {
      bleKeyboard.press(keycodes[i]);
      delay(KEY_PRESS_DELAY_MS);
      bleKeyboard.releaseAll();
      wasPressed[i] = true;
    } else if (!pressed) {
      wasPressed[i] = false;
    }
  }
}

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

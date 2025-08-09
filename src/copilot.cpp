#include <Arduino.h>
#include <esp_sleep.h>

// ---------------- Pin map ----------------
constexpr gpio_num_t PIN_BTN_MAIN = GPIO_NUM_25; // main wake button (LOW on press, pull-up to 3.3V)
constexpr uint8_t    PIN_PEDAL_1  = 13;          // HIGH on press
constexpr uint8_t    PIN_PEDAL_2  = 26;
constexpr uint8_t    PIN_PEDAL_3  = 27;
constexpr uint8_t    PIN_PEDAL_4  = 14;

constexpr uint8_t    PIN_LED1     = 32;          // active-high LED
constexpr uint8_t    PIN_LED2     = 33;          // active-low LED (boot-safe)

// LED helpers
inline void led1On()  { digitalWrite(PIN_LED1, HIGH); }
inline void led1Off() { digitalWrite(PIN_LED1, LOW);  }
inline void led2On()  { digitalWrite(PIN_LED2, LOW);  }   // active-low
inline void led2Off() { digitalWrite(PIN_LED2, HIGH); }   // active-low

RTC_DATA_ATTR uint32_t bootCount = 0;

// Build mask for EXT1 wake
static constexpr uint64_t PEDAL_MASK =
  (1ULL << PIN_PEDAL_1) |
  (1ULL << PIN_PEDAL_2) |
  (1ULL << PIN_PEDAL_3) |
  (1ULL << PIN_PEDAL_4);

String wakeCauseToString(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT0: return "EXT0 (main button)";
    case ESP_SLEEP_WAKEUP_EXT1: return "EXT1 (pedal)";
    default: return "Other/Unknown";
  }
}

int identifyPedalFromExt1(uint64_t statusMask) {
  if (statusMask & (1ULL << PIN_PEDAL_1)) return 1;
  if (statusMask & (1ULL << PIN_PEDAL_2)) return 2;
  if (statusMask & (1ULL << PIN_PEDAL_3)) return 3;
  if (statusMask & (1ULL << PIN_PEDAL_4)) return 4;
  return 0;
}

void configureWakeupSources() {
  // Main button: EXT0, wake LOW (button press to GND)
  esp_sleep_enable_ext0_wakeup(PIN_BTN_MAIN, 0);

  // Pedals: EXT1, wake on any HIGH (button press to 3.3V)
  esp_sleep_enable_ext1_wakeup(PEDAL_MASK, ESP_EXT1_WAKEUP_ANY_HIGH);
}

void waitForRelease() {
  // Ensure all pedals/buttons are released before sleeping again
  while (digitalRead(PIN_BTN_MAIN) == LOW ||
         digitalRead(PIN_PEDAL_1) == HIGH ||
         digitalRead(PIN_PEDAL_2) == HIGH ||
         digitalRead(PIN_PEDAL_3) == HIGH ||
         digitalRead(PIN_PEDAL_4) == HIGH) {
    delay(10);
  }
}

void goToDeepSleep(uint32_t delaySeconds = 0) {
  if (delaySeconds) delay(delaySeconds * 1000UL);
  led1Off();
  led2Off();
  configureWakeupSources();
  waitForRelease();
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  bootCount++;
  Serial.begin(115200);
  delay(50);

  // LEDs
  pinMode(PIN_LED1, OUTPUT);
  pinMode(PIN_LED2, OUTPUT);
  led1Off();
  led2Off();

  // Inputs
  pinMode(PIN_BTN_MAIN, INPUT_PULLUP);    // LOW on press
  pinMode(PIN_PEDAL_1, INPUT_PULLDOWN);   // HIGH on press
  pinMode(PIN_PEDAL_2, INPUT_PULLDOWN);
  pinMode(PIN_PEDAL_3, INPUT_PULLDOWN);
  pinMode(PIN_PEDAL_4, INPUT_PULLDOWN);

  auto cause = esp_sleep_get_wakeup_cause();
  Serial.printf("Boot #%lu, wake cause: %s\n", (unsigned long)bootCount, wakeCauseToString(cause).c_str());

  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Woke by MAIN button");
  } else if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t mask = esp_sleep_get_ext1_wakeup_status();
    int which = identifyPedalFromExt1(mask);
    Serial.printf("Woke by PEDAL %d (mask=0x%llX)\n", which, mask);
  }

  // Blink LEDs for visual feedback
  led1On(); delay(200); led1Off();
  led2On(); delay(200); led2Off();

  // Return to deep sleep after 1 second
  goToDeepSleep(1);
}

void loop() {}

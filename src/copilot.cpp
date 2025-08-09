#include <Arduino.h>
#include <esp_sleep.h>

// ---------------- Pin map (one-side header) ----------------
constexpr gpio_num_t PIN_BTN_MAIN = GPIO_NUM_36; // VP, input-only, needs external pull-up to 3.3V, button to GND
constexpr uint8_t    PIN_PEDAL_1  = 27;          // active-high (internal pulldown), press connects to 3.3V
constexpr uint8_t    PIN_PEDAL_2  = 26;
constexpr uint8_t    PIN_PEDAL_3  = 25;
constexpr uint8_t    PIN_PEDAL_4  = 33;

constexpr uint8_t    PIN_LED1     = 13;          // active-high LED
constexpr uint8_t    PIN_LED2     = 12;          // active-low LED (boot-safe)

// Helper macros for LED2 polarity
inline void led1On()  { digitalWrite(PIN_LED1, HIGH); }
inline void led1Off() { digitalWrite(PIN_LED1, LOW);  }
inline void led2On()  { digitalWrite(PIN_LED2, LOW);  }   // active-low
inline void led2Off() { digitalWrite(PIN_LED2, HIGH); }   // active-low

// RTC memory to persist across deep sleep
RTC_DATA_ATTR uint32_t bootCount = 0;

// Build the EXT1 bitmask for pedals
static constexpr uint64_t PEDAL_MASK =
  (1ULL << PIN_PEDAL_1) |
  (1ULL << PIN_PEDAL_2) |
  (1ULL << PIN_PEDAL_3) |
  (1ULL << PIN_PEDAL_4);

// ---------------- Utility: decode wake cause ----------------
String wakeCauseToString(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT0: return "EXT0 (main button)";
    case ESP_SLEEP_WAKEUP_EXT1: return "EXT1 (pedal)";
    case ESP_SLEEP_WAKEUP_TIMER: return "Timer";
    case ESP_SLEEP_WAKEUP_TOUCHPAD: return "Touch";
    case ESP_SLEEP_WAKEUP_ULP: return "ULP";
    default: return "Other/Unknown";
  }
}

// Identify which pedal triggered EXT1 (if possible)
int identifyPedalFromExt1(uint64_t statusMask) {
  if (!statusMask) return -1;
  if (statusMask & (1ULL << PIN_PEDAL_1)) return 1;
  if (statusMask & (1ULL << PIN_PEDAL_2)) return 2;
  if (statusMask & (1ULL << PIN_PEDAL_3)) return 3;
  if (statusMask & (1ULL << PIN_PEDAL_4)) return 4;
  return 0; // multiple or unknown
}

// ---------------- Sleep setup ----------------
void configureWakeupSources() {
  // EXT0: main button on GPIO36, wake on LOW (button pressed to GND).
  // Requires external pull-up to 3.3V on the pin.
  esp_sleep_enable_ext0_wakeup(PIN_BTN_MAIN, 0); // level 0 = LOW

  // EXT1: any pedal goes HIGH (active-high wiring with pulldowns)
  esp_sleep_enable_ext1_wakeup(PEDAL_MASK, ESP_EXT1_WAKEUP_ANY_HIGH);
}

void goToDeepSleep(uint32_t secondsDelayBeforeSleep = 0) {
  if (secondsDelayBeforeSleep) delay(secondsDelayBeforeSleep * 1000UL);

  // Turn LEDs off before sleeping
  led1Off();
  led2Off();

  // Optional: isolate GPIOs to reduce leakage (commented by default)
  // rtc_gpio_isolate((gpio_num_t)PIN_LED1);
  // rtc_gpio_isolate((gpio_num_t)PIN_LED2);

  Serial.flush();
  esp_deep_sleep_start();
}

// ---------------- Setup/Loop ----------------
void setup() {
  bootCount++;

  Serial.begin(115200);
  delay(50);

  // LEDs
  pinMode(PIN_LED1, OUTPUT);
  pinMode(PIN_LED2, OUTPUT);
  led1Off();
  led2Off(); // keep GPIO12 high at boot for safety

  // Pedals: active-high with internal pulldown
  pinMode(PIN_PEDAL_1, INPUT_PULLDOWN);
  pinMode(PIN_PEDAL_2, INPUT_PULLDOWN);
  pinMode(PIN_PEDAL_3, INPUT_PULLDOWN);
  pinMode(PIN_PEDAL_4, INPUT_PULLDOWN);

  // Main button on GPIO36: input-only, NO internal pullups/downs available.
  // Ensure external 10k pull-up to 3.3V, button to GND.
  pinMode((uint8_t)PIN_BTN_MAIN, INPUT); // floating here; external pull-up does the work

  // Print wake info
  auto cause = esp_sleep_get_wakeup_cause();
  Serial.println();
  Serial.printf("Boot #%lu, wake cause: %s\n", (unsigned long)bootCount, wakeCauseToString(cause).c_str());

  // Give a quick visual cue
  led1On();
  delay(120);
  led1Off();

  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Woke by MAIN button (GPIO36)");
  } else if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t mask = esp_sleep_get_ext1_wakeup_status();
    int which = identifyPedalFromExt1(mask);
    Serial.printf("Woke by PEDAL (mask=0x%llX) -> pedal %d\n", mask, which);
  } else {
    Serial.println("Power-on/reset or other wake");
  }

  // Example app behavior:
  // - Light LED2 (active-low) for 2s to signal we are awake
  led2On();
  delay(2000);
  led2Off();

  // Re-arm wakeup sources and go back to deep sleep after a short grace period
  configureWakeupSources();
  goToDeepSleep(1);
}

void loop() {
  // Not used; we always deep sleep from setup()
}

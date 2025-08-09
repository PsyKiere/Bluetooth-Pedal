/*********************************************************************************
 * ESP32 Bluetooth Page Turner Pedal (v4 - Final)
 * * Features:
 * - Up to 4 modular pedals for page turning (Left, Right, Up, Down).
 * - Main button for system control:
 * - Double-click: Toggle Concert Mode (all LEDs off).
 * - Triple-click: Enter Bluetooth pairing mode.
 * - Hold (3s): Enter Deep Sleep (power off).
 * - Power-efficient: Uses Light Sleep between presses and Deep Sleep for power off.
 * - Auto-reconnects to the last paired device on startup.
 * - Status LEDs for Power and Bluetooth connection.
 *
 * Libraries Required:
 * - ESP32-BLE-Keyboard by T-vK
 * - OneButton by Matthias Hertel
 *
 *********************************************************************************/

// =============================================================================
// LIBRARIES
// =============================================================================
#include <Arduino.h>
#include <BleKeyboard.h>
#include <OneButton.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>


// =============================================================================
// PIN DEFINITIONS
// =============================================================================
// --- LEDs ---
#define POWER_LED_PIN   32
#define BT_LED_PIN      33

// --- Main Control Button ---
#define MAIN_BUTTON_PIN 25

// --- Pedals ---
#define MAIN_PEDAL_PIN  13 // Down Arrow
#define MODULE1_PIN     26 // Left Arrow
#define MODULE2_PIN     27 // Right Arrow
#define MODULE3_PIN     14 // Up Arrow

// =============================================================================
// GLOBAL VARIABLES & OBJECTS
// =============================================================================
BleKeyboard bleKeyboard("PipoLaPipe", "ESP32-Pedal", 100);
OneButton mainButton(MAIN_BUTTON_PIN, true); // true = active low (button to GND)

// --- State Variables ---
volatile bool isConnected = false;
bool concertMode = false;
bool lastConnectionState = false; // Used to detect connection changes

// --- Timing for non-blocking LED blink ---
unsigned long previousMillis = 0;
const long blinkInterval = 500; // Blink every 500ms
int ledState = LOW;

// =============================================================================
// FUNCTION PROTOTYPES
// =============================================================================
void doubleClick();
void tripleClick();
void startHold();
void handleMultiClick();
void handlePedals();
void updateLeds();
void goToLightSleep();
void configureWakeupPins();
void handleConnectionStateChange();

// =============================================================================
// SETUP - Runs once on boot/reset
// =============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("Starting ESP32 Page Turner...");
  // Diagnostic: print last reset reason and wakeup cause
  Serial.printf("Reset reason: %d, wakeup cause: %d\n", (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause());

  // --- Configure Pins ---
  pinMode(POWER_LED_PIN, OUTPUT);
  pinMode(BT_LED_PIN, OUTPUT);
  pinMode(MAIN_BUTTON_PIN, INPUT_PULLUP);
  
  pinMode(MAIN_PEDAL_PIN, INPUT_PULLUP);
  pinMode(MODULE1_PIN, INPUT_PULLUP);
  pinMode(MODULE2_PIN, INPUT_PULLUP);
  pinMode(MODULE3_PIN, INPUT_PULLUP);

  digitalWrite(POWER_LED_PIN, HIGH); // Turn power LED on immediately

  // --- Attach Functions to Main Button ---
  mainButton.attachDoubleClick(doubleClick);
  mainButton.attachLongPressStart(startHold);
  mainButton.attachMultiClick(handleMultiClick);

  // --- Configure Wakeup Sources ---
  configureWakeupPins();
  
  // --- Start Bluetooth ---
  bleKeyboard.begin();
}

// =============================================================================
// MAIN LOOP - Runs repeatedly
// =============================================================================
void loop() {
  // Always check the main button state
  mainButton.tick();

  // Handle connection/disconnection events
  handleConnectionStateChange();

  // If connected, check for pedal presses
  if (isConnected) {
    handlePedals();
  }
  
  // Update the status LEDs
  updateLeds();
  
  // Go to light sleep to save power if connected and idle
  if (isConnected) {
    goToLightSleep();
  } else {
    // If not connected, delay slightly to prevent the loop from running too fast
    delay(50); 
  }
}

// =============================================================================
// CUSTOM FUNCTIONS
// =============================================================================

/**
 * @brief Main Button: Double Click - Toggles concert mode on/off
 */
void doubleClick() {
  concertMode = !concertMode;
  Serial.print("Concert mode: ");
  Serial.println(concertMode ? "ON" : "OFF");
}

/**
 * @brief Handles multi-clicks from the main button. We only care about triple clicks.
 */
void handleMultiClick() {
  if (mainButton.getNumberClicks() == 3) {
    tripleClick();
  }
}

/**
 * @brief Action for Triple Click - Enters pairing mode
 */
void tripleClick() {
  // This is the most reliable, stack-agnostic way to force pairing.
  // It completely restarts the BLE service.
  Serial.println("Forcing pairing mode by restarting BLE...");
  bleKeyboard.end(); // Shut down BLE service
  delay(100);        // Brief pause for stability
  bleKeyboard.begin(); // Restart BLE, which automatically starts advertising
}

/**
 * @brief Main Button: Hold - Enters deep sleep
 */
void startHold() {
  Serial.println("Entering deep sleep. Press main button to wake.");
  digitalWrite(POWER_LED_PIN, LOW); // Turn off LEDs before sleeping
  digitalWrite(BT_LED_PIN, LOW);
  delay(100); // Allow serial to print
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_25, 0); // 0 = Wake when pin is LOW
  // Ensure button is released before entering deep sleep to avoid instant wake
  while (digitalRead(MAIN_BUTTON_PIN) == LOW) {
    delay(10);
  }
  esp_deep_sleep_start();
}

/**
 * @brief Polls connection state and handles changes (replaces callbacks)
 */
void handleConnectionStateChange() {
  isConnected = bleKeyboard.isConnected();
  if (isConnected != lastConnectionState) {
    if (isConnected) {
      Serial.println("Device connected");
      // Stop advertising to save power
      // CORRECTED: Use the global BLEDevice to control advertising.
      BLEDevice::getAdvertising()->stop();
    } else {
      Serial.println("Device disconnected");
      // Start advertising to allow reconnection
      // CORRECTED: Use the global BLEDevice to control advertising.
      BLEDevice::getAdvertising()->start();
    }
    lastConnectionState = isConnected;
  }
}

/**
 * @brief Checks all pedals and sends keystrokes if pressed
 */
void handlePedals() {
  bool pedalPressed = false;
  if (digitalRead(MAIN_PEDAL_PIN) == LOW) {
    bleKeyboard.press(KEY_DOWN_ARROW);
    pedalPressed = true;
  }
  if (digitalRead(MODULE1_PIN) == LOW) {
    bleKeyboard.press(KEY_LEFT_ARROW);
    pedalPressed = true;
  }
  if (digitalRead(MODULE2_PIN) == LOW) {
    bleKeyboard.press(KEY_RIGHT_ARROW);
    pedalPressed = true;
  }
  if (digitalRead(MODULE3_PIN) == LOW) {
    bleKeyboard.press(KEY_UP_ARROW);
    pedalPressed = true;
  }
  
  if (pedalPressed) {
    // A small delay to ensure the key press is registered by the host
    delay(50);
    // Release all keys to prevent sticky keys
    bleKeyboard.releaseAll();
  }
}

/**
 * @brief Manages the state of the Power and Bluetooth LEDs
 */
void updateLeds() {
  if (concertMode) {
    digitalWrite(POWER_LED_PIN, LOW);
    digitalWrite(BT_LED_PIN, LOW);
    return; // Exit function, no LEDs should be on
  }

  // Power LED is always on unless in concert mode
  digitalWrite(POWER_LED_PIN, HIGH);

  // Bluetooth LED logic
  if (isConnected) {
    digitalWrite(BT_LED_PIN, HIGH); // Solid ON when connected
  } else {
    // Blinking when not connected (i.e., pairing mode)
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= blinkInterval) {
      previousMillis = currentMillis;
      ledState = (ledState == LOW) ? HIGH : LOW;
      digitalWrite(BT_LED_PIN, ledState);
    }
  }
}

/**
 * @brief Configures all input pins to be able to wake the ESP32 from light sleep
 */
void configureWakeupPins() {
  // This is the correct method for enabling wakeup on multiple pins for light sleep
  gpio_wakeup_enable(GPIO_NUM_13, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(GPIO_NUM_14, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(GPIO_NUM_25, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(GPIO_NUM_26, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(GPIO_NUM_27, GPIO_INTR_LOW_LEVEL);
  
  esp_sleep_enable_gpio_wakeup();
}

/**
 * @brief Enters power-saving Light Sleep mode
 */
void goToLightSleep() {
  // Wakeup sources are configured once in setup(). We just need to start sleep.
  esp_light_sleep_start();
  
  // After waking up, the loop will continue, detect the pressed pedal,
  // send the key, and then return here to go back to sleep.
}
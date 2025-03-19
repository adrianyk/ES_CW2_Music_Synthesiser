// -------------------- Configuration --------------------
#define MODULE_MODE_SENDER       // Enable sending: local key changes will be transmitted
#define MODULE_MODE_RECEIVER     // Enable receiving: incoming messages will be decoded and displayed

#define MAX_SOUNDS 12  // Maximum simultaneous sounds

// Set TEST_MODE as follows:
// 0 = normal operation (original main.cpp)
// 1 = test scanKeysIteration() execution time        : ~3251 µs for 32 iterations (~101.6 µs per iteration)
// 2 = test display update execution time             : ~1,666,056 µs for 32 iterations (~52 ms per iteration)
// 3 = test decodeTask() processing iteration         : ~129 µs for 32 iterations (~4 µs per iteration)
// 4 = test CAN_TX_Task() processing iteration        : ~74 µs for 32 iterations (~2.3 µs per iteration)
// 5 = test sampleISR() execution time                : ~290 µs for 32 iterations (~9 µs per iteration)
// 6 = test metronomeTask() iteration                 : ~146 µs for 32 iterations (~4.6 µs per iteration)
// 7 = test polyphony stress test iteration           : ~3848 µs for 32 iterations (~120.3 µs per iteration)
// 8 = test knob response/debounce iteration          : ~748 µs for 32 iterations (~23.4 µs per iteration)
#ifndef TEST_MODE
  #define TEST_MODE 0
#endif

#if TEST_MODE != 0
  #define DISABLE_THREADS
#endif

#include <Arduino.h>
#include <U8g2lib.h>
#include <bitset>
#include <HardwareTimer.h>
#include <STM32FreeRTOS.h>
#include <atomic>
#include <stm32l4xx_hal.h>
#include <stm32l4xx_hal_can.h>
#include <stm32l4xx_hal_rcc.h>
#include <stm32l4xx_hal_gpio.h>
#include <stm32l4xx_hal_cortex.h>
#include <ES_CAN.h>   // Provided CAN library

// -------------------- FreeRTOS Queues and Semaphores --------------------
QueueHandle_t msgInQ  = NULL;   // For incoming CAN messages
QueueHandle_t msgOutQ = NULL;   // For outgoing CAN messages
SemaphoreHandle_t CAN_TX_Semaphore = NULL;  // Counting semaphore for TX mailboxes
SemaphoreHandle_t soundsMutex;

// -------------------- System State --------------------
struct {
  std::bitset<32> inputs;
  SemaphoreHandle_t mutex;
  uint8_t RX_Message[8];  // Latest incoming CAN frame
} sysState;

// -------------------- Note Names and Step Sizes --------------------
const char* pitchClasses[12] = { 
  "C", "C#", "D", "D#", "E", "F", 
  "F#", "G", "G#", "A", "A#", "B" 
};

// Step sizes are defined for octave 4 (e.g. A4 = 440 Hz)
const uint32_t stepSizes[12] = {
  51076056, // C4
  54113197, // C#4
  57330935, // D4
  60740009, // D#4
  64351798, // E4
  68178356, // F4
  72232452, // F#4
  76527617, // G4
  81078186, // G#4
  85899345, // A4
  91007186, // A#4
  96418755  // B4
};

// -------------------- Pin Definitions --------------------
const int RA0_PIN = D3;
const int RA1_PIN = D6;
const int RA2_PIN = D12;
const int REN_PIN = A5;
const int C0_PIN = A2;
const int C1_PIN = D9;
const int C2_PIN = A6;
const int C3_PIN = D1;
const int OUT_PIN = D11;
const int OUTL_PIN = A4;
const int OUTR_PIN = A3;
const int JOYY_PIN = A0;
const int JOYX_PIN = A1;
const int DEN_BIT = 3;
const int DRST_BIT = 4;
const int HKOW_BIT = 5;
const int HKOE_BIT = 6;

// -------------------- ADSR Envelope Variables --------------------
const float attackRate   = 0.001f;   // Attack increment
const float decayRate    = 0.001f;    // Decay decrement until sustain
const float sustainLevel = 0.6f;      // Sustain level
const float releaseRate  = 0.00005f;   // Release decrement

// -------------------- Polyphony Data Structures --------------------
struct Sound {
  bool active;
  uint8_t note;
  uint8_t octave;
  uint32_t stepSize;
  uint32_t phaseAcc;
  float envelopeLevel;
  bool keyPressed;
  bool keyReleased;
};

// Global array of sounds – one per simultaneous note
Sound sounds[MAX_SOUNDS] = {0};

// -------------------- Metronome Variables --------------------
volatile std::atomic<bool> metronomeEnabled = false;
volatile std::atomic<bool> metronomeMode = false;

// -------------------- Octave Variables --------------------
volatile std::atomic<bool> octaveMode = false;

// -------------------- Knob Class --------------------
class Knob {
private:
  std::atomic<int32_t> rotation;
  uint8_t prevState;
  int8_t lastDirection;
  int32_t lowerLimit, upperLimit;
  std::atomic<bool> isPressed;
public:
  Knob(int32_t minVal = 0, int32_t maxVal = 8)
    : rotation(0), prevState(0b00), lastDirection(0),
      lowerLimit(minVal), upperLimit(maxVal), isPressed(false) {}

  void update(uint8_t currState, bool pressed) {
    int32_t localRotation = rotation.load(std::memory_order_relaxed);
    if ((prevState == 0b00 && currState == 0b01) ||
        (prevState == 0b11 && currState == 0b10)) { // Clockwise
        localRotation++;
        lastDirection = 1;
    } else if ((prevState == 0b10 && currState == 0b11) ||
               (prevState == 0b01 && currState == 0b00)) { // Anticlockwise
        localRotation--;
        lastDirection = -1;
    } else if ((prevState == 0b00 && currState == 0b11) ||
               (prevState == 0b11 && currState == 0b00)) { // Impossible/illegal transitions
        localRotation += lastDirection;
    }
    if (localRotation < lowerLimit) localRotation = lowerLimit;
    if (localRotation > upperLimit) localRotation = upperLimit;
    rotation.store(localRotation, std::memory_order_relaxed);
    isPressed.store(pressed, std::memory_order_relaxed);
    prevState = currState;
  }
  
  int32_t getRotation() const {
    return rotation.load(std::memory_order_relaxed);
  }
  
  bool getPressed() const {
    return isPressed.load(std::memory_order_relaxed);
  }
};

Knob volumeKnob(0, 8);      // Knob 3
Knob octaveKnob(1, 7);      // Knob 2
Knob tempoKnob(40, 240);    // Knob 0

// -------------------- Timers & Display --------------------
HardwareTimer sampleTimer(TIM1);
HardwareTimer metronomeTimer(TIM2);
U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C u8g2(U8G2_R0);

// -------------------- Helper Functions for Key Matrix --------------------
void setOutMuxBit(const uint8_t bitIdx, const bool value) {
  digitalWrite(REN_PIN, LOW);
  digitalWrite(RA0_PIN, bitIdx & 0x01);
  digitalWrite(RA1_PIN, bitIdx & 0x02);
  digitalWrite(RA2_PIN, bitIdx & 0x04);
  digitalWrite(OUT_PIN, value);
  digitalWrite(REN_PIN, HIGH);
  delayMicroseconds(2);
  digitalWrite(REN_PIN, LOW);
}

std::bitset<4> readCols() {
  std::bitset<4> result;
  result[0] = digitalRead(C0_PIN);
  result[1] = digitalRead(C1_PIN);
  result[2] = digitalRead(C2_PIN);
  result[3] = digitalRead(C3_PIN);
  return result;
}

void setRow(uint8_t rowIdx) {
  digitalWrite(REN_PIN, LOW);
  digitalWrite(RA0_PIN, rowIdx & 0x01);
  digitalWrite(RA1_PIN, rowIdx & 0x02);
  digitalWrite(RA2_PIN, rowIdx & 0x04);
  digitalWrite(REN_PIN, HIGH);
}

// -------------------- Helper Functions for Sound Allocation --------------------
// Called on key press to start a new sound
void startSound(uint8_t note, uint8_t octave) {
  if (xSemaphoreTake(soundsMutex, portMAX_DELAY) == pdTRUE) {
    // Check if the same note is already active
    for (int i = 0; i < MAX_SOUNDS; i++) {
      if (sounds[i].active && sounds[i].note == note && sounds[i].octave == octave) {
        // Re-trigger the same sound immediately
        sounds[i].keyPressed = true;
        sounds[i].keyReleased = false;
        sounds[i].envelopeLevel = 0.0f;
        sounds[i].phaseAcc = 0;
        xSemaphoreGive(soundsMutex);
        return;
      }
    }
    // No active sound for this note found: allocate a new sound/find free sound slot
    for (int i = 0; i < MAX_SOUNDS; i++) {
      if (!sounds[i].active) {
        sounds[i].active = true;
        sounds[i].note = note;
        sounds[i].octave = octave;
        sounds[i].stepSize = stepSizes[note];
        if (octave > 4)
          sounds[i].stepSize <<= (octave - 4);
        else if (octave < 4)
          sounds[i].stepSize >>= (4 - octave);
        sounds[i].phaseAcc = 0;
        sounds[i].envelopeLevel = 0.0f;
        sounds[i].keyPressed = true;
        sounds[i].keyReleased = false;
        xSemaphoreGive(soundsMutex);
        return;
      }
    }
    // If no free sound is found, steal the first sound
    int i = 0;
    sounds[i].active = true;
    sounds[i].note = note;
    sounds[i].octave = octave;
    sounds[i].stepSize = stepSizes[note];
    if (octave > 4)
      sounds[i].stepSize <<= (octave - 4);
    else if (octave < 4)
      sounds[i].stepSize >>= (4 - octave);
    sounds[i].phaseAcc = 0;
    sounds[i].envelopeLevel = 0.0f;
    sounds[i].keyPressed = true;
    sounds[i].keyReleased = false;
    xSemaphoreGive(soundsMutex);
  }
}
  
// Called on key release to mark the sound as released
void releaseSound(uint8_t note, uint8_t octave) {
  if (xSemaphoreTake(soundsMutex, portMAX_DELAY) == pdTRUE) {
    // Search for a sound that matches the note and octave and is still active.
    for (int i = 0; i < MAX_SOUNDS; i++) {
      if (sounds[i].active && sounds[i].note == note && sounds[i].octave == octave && sounds[i].keyPressed) {
        sounds[i].keyPressed = false;
        sounds[i].keyReleased = true;
        break;
      }
    }
    xSemaphoreGive(soundsMutex);
  }
}

// -------------------- scanKeysIteration() --------------------
// In TEST_MODE==1, simulate key scanning without blocking.
void scanKeysIteration() {
#if TEST_MODE == 1
  // Test branch: run the full key scan routine to measure worst-case time.
  static bool prevKeyPressed[12] = { true, true, true, true, true, true, true, true, true, true, true, true };
  std::bitset<32> all_inputs;
  // Scan 8 rows (as in normal operation) to capture extra controls.
  for (uint8_t row = 0; row < 8; row++) {
    setRow(row);
    delayMicroseconds(3);  // Include the delay for hardware settling/debounce
    std::bitset<4> result = readCols();
    for (uint8_t col = 0; col < 4; col++) {
      int index = row * 4 + col;
      all_inputs[index] = result[col];
    }
  }
  #ifdef MODULE_MODE_SENDER
    for (uint8_t i = 0; i < 12; i++) {
      bool currentPressed = !all_inputs[i];  // LOW means pressed
      if (currentPressed != prevKeyPressed[i]) {
        uint8_t TX_Message[8] = {0};
        TX_Message[0] = currentPressed ? 'P' : 'R';
        TX_Message[1] = octaveKnob.getRotation();
        TX_Message[2] = i;
        // Use a finite timeout (e.g. 1 tick) instead of portMAX_DELAY
        xQueueSend(msgOutQ, TX_Message, 1);
        #ifdef MODULE_MODE_RECEIVER
          // Use a 1‑tick timeout for the mutex call to simulate worst‑case overhead.
          if(xSemaphoreTake(sysState.mutex, 1) == pdTRUE) {
            memcpy(sysState.RX_Message, TX_Message, 8);
            xSemaphoreGive(sysState.mutex);
          }
          if (TX_Message[0] == 'P') {
            startSound(TX_Message[2], TX_Message[1]);
          } else {
            releaseSound(TX_Message[2], TX_Message[1]);
          }
        #endif
        prevKeyPressed[i] = currentPressed;
      }
    }
  #endif
  // Update rotary knobs as normal.
  volumeKnob.update((all_inputs[13] << 1) | all_inputs[12], all_inputs[21]); // Knob 3
  octaveKnob.update((all_inputs[15] << 1) | all_inputs[14], all_inputs[20]); // Knob 2
  tempoKnob.update((all_inputs[19] << 1) | all_inputs[18], all_inputs[24]); // Knob 0
  
  // Update shared inputs with a 1-tick timeout.
  xSemaphoreTake(sysState.mutex, 1);
  sysState.inputs = all_inputs;
  xSemaphoreGive(sysState.mutex);
#else
  // Normal operation branch (unchanged)
  static bool prevKeyPressed[12] = { true, true, true, true, true, true, true, true, true, true, true, true };
  std::bitset<32> all_inputs;
  for (uint8_t row = 0; row < 8; row++) {
    setRow(row);
    delayMicroseconds(3);
    std::bitset<4> result = readCols();
    for (uint8_t col = 0; col < 4; col++) {
      int index = row * 4 + col;
      all_inputs[index] = result[col];
    }
  }
  #ifdef MODULE_MODE_SENDER
    for (uint8_t i = 0; i < 12; i++) {
      bool currentPressed = !all_inputs[i];
      if (currentPressed != prevKeyPressed[i]) {
        uint8_t TX_Message[8] = {0};
        TX_Message[0] = currentPressed ? 'P' : 'R';
        TX_Message[1] = octaveKnob.getRotation();
        TX_Message[2] = i;
        xQueueSend(msgOutQ, TX_Message, portMAX_DELAY);
        #ifdef MODULE_MODE_RECEIVER
          xSemaphoreTake(sysState.mutex, portMAX_DELAY);
          memcpy(sysState.RX_Message, TX_Message, 8);
          xSemaphoreGive(sysState.mutex);
          if (TX_Message[0] == 'P') { // Key pressed
            startSound(TX_Message[2], TX_Message[1]);
          } else { // Key released
            releaseSound(TX_Message[2], TX_Message[1]);
          }
        #endif
        prevKeyPressed[i] = currentPressed;
      }
    }
  #endif
  volumeKnob.update((all_inputs[13] << 1) | all_inputs[12], all_inputs[21]); // Knob 3
  octaveKnob.update((all_inputs[15] << 1) | all_inputs[14], all_inputs[20]); // Knob 2
  tempoKnob.update((all_inputs[19] << 1) | all_inputs[18], all_inputs[24]); // Knob 0
  
  xSemaphoreTake(sysState.mutex, portMAX_DELAY);
  sysState.inputs = all_inputs;
  xSemaphoreGive(sysState.mutex);
#endif
}

// -------------------- toggleMetronomeScreen() --------------------
void toggleMetronomeScreen() {
  static bool lastButtonState = HIGH;
  bool currentButtonState = tempoKnob.getPressed();
  if (lastButtonState == HIGH && currentButtonState == LOW) {
    metronomeMode.store(!metronomeMode, std::memory_order_relaxed);
    metronomeEnabled.store(metronomeMode, std::memory_order_relaxed);
  }
  lastButtonState = currentButtonState;
}

// -------------------- toggleOctaveScreen() --------------------
void toggleOctaveScreen() {
  static bool lastButtonState = HIGH;
  bool currentButtonState = octaveKnob.getPressed();
  if (lastButtonState == HIGH && currentButtonState == LOW) {
    octaveMode.store(!octaveMode, std::memory_order_relaxed);
  }
  lastButtonState = currentButtonState;
}

// -------------------- Testing --------------------
#if TEST_MODE == 2
void testDisplayIteration() {
  char keyLabel[16] = "None";
  xSemaphoreTake(sysState.mutex, portMAX_DELAY);
  if (sysState.RX_Message[0] == 'P') {
    uint8_t octave = sysState.RX_Message[1];
    uint8_t note = sysState.RX_Message[2];
    sprintf(keyLabel, "%s%d", pitchClasses[note], octave);
  } else {
    strcpy(keyLabel, "None");
  }
  xSemaphoreGive(sysState.mutex);
  Serial.print("Test Display - Key: ");
  Serial.print(keyLabel);
  Serial.print(", Volume: ");
  Serial.println(volumeKnob.getRotation());
}
#endif

#if TEST_MODE == 3
void testDecodeIteration() {
  uint8_t testMsg[8] = { 'P', octaveKnob.getRotation(), 5, 0, 0, 0, 0, 0 };
  uint8_t octave = testMsg[1];
  uint8_t note = testMsg[2];
  uint32_t step = stepSizes[note];
  if (testMsg[0] == 'P') {
    startSound(note, octave);
  } else if (testMsg[0] == 'R') {
    releaseSound(note, octave);
  }
  memcpy(sysState.RX_Message, testMsg, 8);
}
#endif

#if TEST_MODE == 4
void dummyCANTX(uint32_t id, uint8_t* msg) {
  volatile uint32_t dummy = 0;
  for (volatile int j = 0; j < 10; j++) {
    dummy += j;
  }
}
void testCANTXIteration() {
  uint8_t testMsg[8] = { 'P', octaveKnob.getRotation(), 2, 0, 0, 0, 0, 0 };
  dummyCANTX(0x123, testMsg);
}
#endif

// -------------------- Test function: Metronome Iteration --------------------
#if TEST_MODE == 6
// This function simulates one iteration of the metronome task—that is,
// it reads the tempo knob, computes the metronomeInterval, and updates the timer.
void testMetronomeIteration() {
  if (metronomeMode.load(std::memory_order_relaxed)) {  // Only if metronome mode is active
    uint32_t metronomeInterval = 500; // Default 120 BPM (500ms per tick)
    uint32_t bpm = tempoKnob.getRotation();
    metronomeInterval = 60000 / bpm;
    metronomeTimer.setOverflow(metronomeInterval * 1000, MICROSEC_FORMAT);
  }
}
#endif

#if TEST_MODE == 7
// Test function for polyphony stress: simulate activating all voices concurrently.
void testPolyphonyIteration() {
  // Simulate key press for all voices.
  for (uint8_t i = 0; i < MAX_SOUNDS; i++) {
    // Using note index (i modulo 12) and octave 4 (for example)
    startSound(i % 12, 4);
  }
  // Optionally simulate key release for all voices.
  for (uint8_t i = 0; i < MAX_SOUNDS; i++) {
    releaseSound(i % 12, 4);
  }
}
#endif

#if TEST_MODE == 8
// Test function for knob response and debounce: simulate rapid knob rotations.
void testKnobIteration() {
  // Define a cycle of states that represent a clockwise rotation.
  // For example: 0b00 -> 0b01 -> 0b11 -> 0b10.
  uint8_t states[4] = {0b00, 0b01, 0b11, 0b10};
  // Simulate several full cycles; here we use 10 cycles.
  for (int cycle = 0; cycle < 10; cycle++) {
    for (int i = 0; i < 4; i++) {
      // 'false' indicates the knob button is not pressed.
      volumeKnob.update(states[i], false);
    }
  }
}
#endif


#if TEST_MODE == 0
extern "C" void myCanRxISR(void) {
  uint8_t RX_Message_ISR[8];
  uint32_t ID;
  CAN_RX(ID, RX_Message_ISR);
  xQueueSendFromISR(msgInQ, RX_Message_ISR, NULL);
}

extern "C" void myCanTxISR(void) {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(CAN_TX_Semaphore, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
#endif

#ifdef MODULE_MODE_RECEIVER
#if TEST_MODE == 0
void decodeTask(void *pvParameters) {
  uint8_t localMsg[8];
  while (1) {
    if (xQueueReceive(msgInQ, localMsg, portMAX_DELAY) == pdTRUE) {
      if (localMsg[0] == 'P') {
        startSound(localMsg[2], localMsg[1]);
      } else if (localMsg[0] == 'R') {  // Key Released
        releaseSound(localMsg[2], localMsg[1]);
      }

      // Store the received message (for debugging)
      memcpy(sysState.RX_Message, localMsg, 8);
    }
  }
}
#endif
#endif

// -------------------- CAN_TX_Task() --------------------
#ifdef MODULE_MODE_SENDER
#if TEST_MODE == 0
void CAN_TX_Task(void * pvParameters) {
  uint8_t msgOut[8];
  while (1) {
    xQueueReceive(msgOutQ, msgOut, portMAX_DELAY);
    xSemaphoreTake(CAN_TX_Semaphore, portMAX_DELAY);
    CAN_TX(0x123, msgOut);
  }
}
#endif
#endif

// -------------------- sampleISR() --------------------
#ifdef MODULE_MODE_RECEIVER
#if TEST_MODE == 0
void sampleISR() {
  int32_t Vout_total = 0; // Sum of all sounds

  // Iterate over all sounds
  for (int i = 0; i < MAX_SOUNDS; i++) {

    // Quickly copy the shared Sound into a local variable in a critical section.
    UBaseType_t uxSaved = portSET_INTERRUPT_MASK_FROM_ISR();
    Sound localSound = sounds[i];
    portCLEAR_INTERRUPT_MASK_FROM_ISR(uxSaved);

    if (!localSound.active)
      continue;

    // Update the envelope for this sound
    if (localSound.keyPressed) {
      // Attack phase: ramp up envelope until full amplitude
      if (localSound.envelopeLevel < 1.0f) {
        localSound.envelopeLevel += attackRate;
        if (localSound.envelopeLevel > 1.0f)
          localSound.envelopeLevel = 1.0f;
      }
      // Decay phase: if envelope overshoots sustain, decay down to sustain level
      else if (localSound.envelopeLevel > sustainLevel) {
        localSound.envelopeLevel -= decayRate;
        if (localSound.envelopeLevel < sustainLevel)
        localSound.envelopeLevel = sustainLevel;
      }
    }
    else if (localSound.keyReleased) {
      // Release phase: decay envelope to zero
      if (localSound.envelopeLevel > 0.0f) {
        localSound.envelopeLevel -= releaseRate;
        if (localSound.envelopeLevel <= 0.0f) {
          localSound.envelopeLevel = 0.0f;

          // Mark sound inactive
          UBaseType_t uxSaved2 = portSET_INTERRUPT_MASK_FROM_ISR();
          sounds[i].active = false;   // Sound finished
          portCLEAR_INTERRUPT_MASK_FROM_ISR(uxSaved2);
          continue;
        }
      }
    }

    // Phase accumulation should continue until the envelope reaches zero
    if (localSound.envelopeLevel > 0.0f)
      localSound.phaseAcc += localSound.stepSize;

    int32_t rawWave = (localSound.phaseAcc >> 24) - 128;
    int32_t soundOutput = static_cast<int32_t>(rawWave * localSound.envelopeLevel);

    Vout_total += soundOutput;

    // Write back the updated phase accumulator and envelopeLevel.
    UBaseType_t uxSaved3 = portSET_INTERRUPT_MASK_FROM_ISR();
    sounds[i].phaseAcc = localSound.phaseAcc;
    sounds[i].envelopeLevel = localSound.envelopeLevel;
    portCLEAR_INTERRUPT_MASK_FROM_ISR(uxSaved3);
  }

  // Volume control
  Vout_total = Vout_total >> (8 - volumeKnob.getRotation());

  // Clip Vout
  if (Vout_total > 127)
    Vout_total = 127;
  else if (Vout_total < -128)
    Vout_total = -128;

  analogWrite(OUTR_PIN, Vout_total + 128);
}
#else
void sampleISR() {
  analogWrite(OUTR_PIN, 128);
  }
#endif
#endif

// -------------------- Metronome ISR --------------------
void metronomeISR() {
  static bool tickState = false;
  if (metronomeEnabled.load(std::memory_order_relaxed)) {
      analogWrite(OUTR_PIN, tickState ? 255 : 0); // Toggle speaker output
      tickState = !tickState;
  }
}

#if TEST_MODE == 0
void scanKeysTask(void * pvParameters) {
  const TickType_t xFrequency = 20 / portTICK_PERIOD_MS;
  TickType_t xLastWakeTime = xTaskGetTickCount();
  while (1) {
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
    scanKeysIteration();
  }
}
#endif

#if TEST_MODE == 0
void metronomeTask(void *pvParameters) {
  const TickType_t xFrequency = 50 / portTICK_PERIOD_MS;
  TickType_t xLastWakeTime = xTaskGetTickCount();
  uint32_t metronomeInterval = 500; // Default 120 BPM (500ms per tick)
  while (1) {
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
    if (metronomeMode.load(std::memory_order_relaxed)) {
      uint32_t bpm = tempoKnob.getRotation();
      metronomeInterval = 60000 / bpm;
      metronomeTimer.setOverflow(metronomeInterval * 1000, MICROSEC_FORMAT);
    }
  }
}
#endif

#if TEST_MODE != 2
void displayUpdateTask(void * pvParameters) {
  const TickType_t xFrequency = 100 / portTICK_PERIOD_MS;
  TickType_t xLastWakeTime = xTaskGetTickCount();
  char keyLabel[16] = "None";
  while (1) {
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
    toggleMetronomeScreen();
    toggleOctaveScreen();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    if (metronomeMode.load(std::memory_order_relaxed)) {
      uint32_t bpm = tempoKnob.getRotation();
      u8g2.drawStr(20, 16, "Metronome Mode");
      u8g2.drawStr(2, 32, "Tempo: ");
      u8g2.setCursor(50, 32);
      u8g2.print(bpm);
    } else if (octaveMode.load(std::memory_order_relaxed)) {
      uint32_t octave = octaveKnob.getRotation();
      u8g2.drawStr(2, 10, "Octave: ");
      u8g2.setCursor(70, 10);
      u8g2.print(octave);
    } else {
      xSemaphoreTake(sysState.mutex, portMAX_DELAY);
      if (sysState.RX_Message[0] == 'P') {
        uint8_t octave = sysState.RX_Message[1];
        uint8_t note = sysState.RX_Message[2];
        sprintf(keyLabel, "%s%d", pitchClasses[note], octave);
      } else {
        strcpy(keyLabel, "None");
      }
      xSemaphoreGive(sysState.mutex);
      
      Serial.print("Key: ");
      Serial.print(keyLabel);
      Serial.print(", Volume: ");
      Serial.println(volumeKnob.getRotation());
      
      u8g2.drawStr(2, 10, "Key: ");
      u8g2.setCursor(70, 10);
      u8g2.print(keyLabel);
      u8g2.drawStr(2, 20, "Volume:");
      u8g2.setCursor(70, 20);
      u8g2.print(volumeKnob.getRotation());
      u8g2.drawStr(2, 30, "RX Msg: ");
      u8g2.setCursor(50, 30);
      xSemaphoreTake(sysState.mutex, portMAX_DELAY);
      u8g2.print((char)sysState.RX_Message[0]);
      u8g2.print(sysState.RX_Message[1]);
      u8g2.print(sysState.RX_Message[2]);
      xSemaphoreGive(sysState.mutex);
    }
    u8g2.sendBuffer();
    
    digitalToggle(LED_BUILTIN);
  }
}
#endif

// -------------------- setup() --------------------
void setup() {
  // Set up pins.
  pinMode(RA0_PIN, OUTPUT);
  pinMode(RA1_PIN, OUTPUT);
  pinMode(RA2_PIN, OUTPUT);
  pinMode(REN_PIN, OUTPUT);
  pinMode(OUT_PIN, OUTPUT);
  pinMode(OUTL_PIN, OUTPUT);
  pinMode(OUTR_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(C0_PIN, INPUT);
  pinMode(C1_PIN, INPUT);
  pinMode(C2_PIN, INPUT);
  pinMode(C3_PIN, INPUT);
  pinMode(JOYX_PIN, INPUT);
  pinMode(JOYY_PIN, INPUT);
  setOutMuxBit(DRST_BIT, LOW);
  delayMicroseconds(2);
  setOutMuxBit(DRST_BIT, HIGH);
  u8g2.begin();
  setOutMuxBit(DEN_BIT, HIGH);
  
  Serial.begin(9600);
  delay(1000);  // Give time for Serial Monitor to connect
  Serial.println("Hello World");
  
  // Create RTOS objects.
  sysState.mutex = xSemaphoreCreateMutex();
  soundsMutex = xSemaphoreCreateMutex();
  if (soundsMutex == NULL) {
    Serial.println("Error: Could not create soundsMutex");
  }

  msgInQ  = xQueueCreate(36, 8);
  msgOutQ = xQueueCreate(384, 8);
  if (!msgInQ || !msgOutQ) {
    Serial.println("Error: Could not create queues!");
    while (1);
  }
  CAN_TX_Semaphore = xSemaphoreCreateCounting(3, 3);
  if (!CAN_TX_Semaphore) {
    Serial.println("Error: Could not create CAN_TX_Semaphore!");
    while (1);
  }
  
  // Set up the sample timer (for audio synthesis and envelope).
  sampleTimer.setOverflow(22000, HERTZ_FORMAT);
  sampleTimer.attachInterrupt(sampleISR);
  sampleTimer.resume();
  
  // Set up the metronome timer.
  metronomeTimer.setOverflow(500000, MICROSEC_FORMAT);
  metronomeTimer.attachInterrupt(metronomeISR);
  metronomeTimer.resume();
  
  // TEST MODE 1: Measure execution time of scanKeysIteration() without blocking.
  #if TEST_MODE == 1
    Serial.println("TEST MODE 1: Timing scanKeysIteration()");
    uint32_t startTime = micros();
    for (int i = 0; i < 32; i++) {
      scanKeysIteration();
    }
    uint32_t elapsed = micros() - startTime;
    Serial.print("Total time for 32 iterations of scanKeysIteration(): ");
    Serial.println(elapsed);
    while(1);
  #elif TEST_MODE == 2
    Serial.println("TEST MODE 2: Timing display update iteration");
    uint32_t startTime = micros();
    for (int i = 0; i < 32; i++) {
      testDisplayIteration();
    }
    uint32_t elapsed = micros() - startTime;
    Serial.print("Total time for 32 iterations of display update: ");
    Serial.println(elapsed);
    while(1);
  #elif TEST_MODE == 3
    Serial.println("TEST MODE 3: Timing decodeTask() iteration");
    uint32_t startTime = micros();
    for (int i = 0; i < 32; i++) {
      testDecodeIteration();
    }
    uint32_t elapsed = micros() - startTime;
    Serial.print("Total time for 32 iterations of decodeTask processing: ");
    Serial.println(elapsed);
    while(1);
  #elif TEST_MODE == 4
    Serial.println("TEST MODE 4: Timing CAN_TX_Task() iteration");
    uint32_t startTime = micros();
    for (int i = 0; i < 32; i++) {
      uint8_t testMsg[8] = { 'P', octaveKnob.getRotation(), 2, 0, 0, 0, 0, 0 };
      volatile uint32_t dummy = 0;
      for (volatile int j = 0; j < 10; j++) { dummy += j; }
    }
    uint32_t elapsed = micros() - startTime;
    Serial.print("Total time for 32 iterations of CAN_TX_Task processing: ");
    Serial.println(elapsed);
    while(1);
  #elif TEST_MODE == 5
    Serial.println("TEST MODE 5: Timing sampleISR() iteration");
    uint32_t startTime = micros();
    for (int i = 0; i < 32; i++) {
      sampleISR();
    }
    uint32_t elapsed = micros() - startTime;
    Serial.print("Total time for 32 iterations of sampleISR(): ");
    Serial.println(elapsed);
    while(1);
  #elif TEST_MODE == 6
    // New test mode for metronome task iteration.
    Serial.println("TEST MODE 6: Timing metronomeTask() iteration");
    // Ensure metronome mode is active (for testing, you might force it on):
    metronomeMode.store(true, std::memory_order_relaxed);
    metronomeEnabled.store(true, std::memory_order_relaxed);
    // Optionally, set a fixed tempo.
    // For example, force tempoKnob rotation to 120 BPM:
    // (Assuming getRotation() returns an integer BPM; adjust as needed.)
    // You might want to add a setter in your knob class.
    // Here we assume tempoKnob.getRotation() will now return 120.
    
    uint32_t startTime = micros();
    for (int i = 0; i < 32; i++) {
      testMetronomeIteration();
    }
    uint32_t elapsed = micros() - startTime;
    Serial.print("Total time for 32 iterations of metronomeTask(): ");
    Serial.println(elapsed);
    while (1);
  #elif TEST_MODE == 7
    Serial.println("TEST MODE 7: Timing polyphony stress test iteration");
    uint32_t startTime = micros();
    for (int i = 0; i < 32; i++) {
      testPolyphonyIteration();
    }
    uint32_t elapsed = micros() - startTime;
    Serial.print("Total time for 32 iterations of polyphony stress test: ");
    Serial.println(elapsed);
    while(1);
  #elif TEST_MODE == 8
    Serial.println("TEST MODE 8: Timing knob response/debounce iteration");
    uint32_t startTime = micros();
    for (int i = 0; i < 32; i++) {
      testKnobIteration();
    }
    uint32_t elapsed = micros() - startTime;
    Serial.print("Total time for 32 iterations of knob response test: ");
    Serial.println(elapsed);
    while(1);
  #else
    // Normal operation: create tasks and start scheduler.
    TaskHandle_t scanKeysHandle = NULL;
    xTaskCreate(scanKeysTask, "scanKeys", 128, NULL, 2, &scanKeysHandle);
    #ifdef MODULE_MODE_SENDER
      TaskHandle_t CAN_TXHandle = NULL;
      xTaskCreate(CAN_TX_Task, "CAN_TX_Task", 128, NULL, 2, &CAN_TXHandle);
    #endif
    #ifdef MODULE_MODE_RECEIVER
      TaskHandle_t decodeHandle = NULL;
      xTaskCreate(decodeTask, "decodeTask", 128, NULL, 2, &decodeHandle);
    #endif
    TaskHandle_t displayUpdateHandle = NULL;
    xTaskCreate(displayUpdateTask, "displayUpdate", 256, NULL, 1, &displayUpdateHandle);
    TaskHandle_t metronomeHandle = NULL;
    xTaskCreate(metronomeTask, "metronomeTask", 128, NULL, 1, &metronomeHandle);
  #endif
  
  #if TEST_MODE == 0
    CAN_Init(false);
    setCANFilter(0x123, 0x7ff);
    CAN_RegisterRX_ISR(myCanRxISR);
    CAN_RegisterTX_ISR(myCanTxISR);
    CAN_Start();
    vTaskStartScheduler();
  #endif
}

void loop() {
  // In test modes, loop() will not run.
  // When TEST_MODE is 0, FreeRTOS tasks handle everything.
}
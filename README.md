# Embedded Systems Coursework 2

## Contributors
- Yi Keat (Adrian) Khoo (CID: 02021759)
- Jungwon Bae (CID: 02032319)
- Keegan Lee (CID: 01892378)

## Introduction
This GitHub repo serves as the submission for ES CW2. The full core functionality of the ES-SynthStarter has been implemented. In addition, advanced functionality and timing analyses are discussed below.

## Completed Core Functionalities
1. The synthesiser shall play the appropriate musical tone as a sawtooth wave when a key is pressed 
2. There shall be no perceptible delay between pressing a key and the tone starting 
3. There shall be a volume control with at least 8 increments, which shall be operated by turning a knob 
4. The OLED display shall show the name of the note being played and the current volume level
5. Every 100ms the OLED display shall refresh and an LED shall toggle
6. The synthesiser shall be configurable, during compilation or operation, to act as a sender module or receiver module.
7. If the synthesiser is configured as a sender, it shall send a message on the CAN bus whenever a key is pressed or released
8. If the synthesiser is configured as a receiver, it shall play a note or stop playing a note when it receives an appropriate message on the CAN bus

---

## Task Identification and Implementation

This section identifies all the tasks performed by the synthesiser system and specifies whether they are implemented as FreeRTOS threads (tasks) or as hardware interrupt service routines (ISRs).

### FreeRTOS Tasks (Threads)

1. **scanKeysTask**  
   - **Implementation:** FreeRTOS thread  
   - **Purpose:**  
     - Scans the key matrix every 20 ms to detect key presses and releases.  
     - Updates rotary knob states.  
     - Shares input data with other tasks via a mutex.
   - **Location in Code:** Defined in the block under `#if TEST_MODE == 0`.

2. **metronomeTask**  
   - **Implementation:** FreeRTOS thread  
   - **Purpose:**  
     - Runs every 50 ms to update metronome timing based on the tempo knob.  
     - Computes the appropriate timer interval and updates the metronome timer’s overflow.
   - **Location in Code:** Defined in the block under `#if TEST_MODE == 0`.

3. **displayUpdateTask**  
   - **Implementation:** FreeRTOS thread  
   - **Purpose:**  
     - Updates the OLED display every 100 ms to show the current note (or octave) and volume level.  
     - Toggles an LED with each update.
   - **Location in Code:** Defined in the block under `#if TEST_MODE != 2`.

4. **CAN_TX_Task** (Sender Mode Only)  
   - **Implementation:** FreeRTOS thread  
   - **Purpose:**  
     - Waits for key events on a message queue.  
     - Transmits CAN messages after acquiring a counting semaphore to ensure a transmit mailbox is available.
   - **Location in Code:** Enclosed within `#ifdef MODULE_MODE_SENDER` and `#if TEST_MODE == 0`.

5. **decodeTask** (Receiver Mode Only)  
   - **Implementation:** FreeRTOS thread  
   - **Purpose:**  
     - Processes incoming CAN messages from a queue.  
     - Triggers the appropriate sound (start or release) based on the received message.
   - **Location in Code:** Enclosed within `#ifdef MODULE_MODE_RECEIVER` and `#if TEST_MODE == 0`.

### Interrupt Service Routines (ISRs)

1. **sampleISR**  
   - **Implementation:** Hardware Timer Interrupt  
   - **Purpose:**  
     - Executes at a high frequency to perform audio synthesis.  
     - Iterates through all active sounds, updating their ADSR envelope and phase accumulator.  
     - Computes the final audio sample and writes it to the output.
   - **Location in Code:** Defined under `#ifdef MODULE_MODE_RECEIVER` and `#if TEST_MODE == 0`.

2. **metronomeISR**  
   - **Implementation:** Hardware Timer Interrupt  
   - **Purpose:**  
     - Toggles the speaker output for the metronome at each timer overflow based on the metronome mode flag.
   - **Location in Code:** Defined immediately after the `sampleISR()` function.

3. **myCanRxISR**  
   - **Implementation:** CAN Receive Interrupt  
   - **Purpose:**  
     - Triggered when a CAN message is received.  
     - Reads the incoming message and places it on a FreeRTOS queue for processing by `decodeTask`.
   - **Location in Code:** Defined with `extern "C"` linkage under `#if TEST_MODE == 0`.

4. **myCanTxISR**  
   - **Implementation:** CAN Transmit Interrupt  
   - **Purpose:**  
     - Triggered upon completion of a CAN transmission.  
     - Releases a counting semaphore to signal that a transmit mailbox is available.
   - **Location in Code:** Defined with `extern "C"` linkage under `#if TEST_MODE == 0`.

### Knob Handling

- **Implementation:**  
  - The rotary knobs (for volume, octave, and tempo of metronome) are encapsulated within the `Knob` class.  
  - Their update logic is integrated into the key scanning routine (`scanKeysIteration()`), rather than implemented as separate tasks or ISRs.

### Test Modes vs. Normal Operation

- **Test Modes (TEST_MODE ≠ 0):**  
  - Threads are disabled (using `#define DISABLE_THREADS`) to enable isolated performance measurements of individual routines.
  
- **Normal Operation (TEST_MODE = 0):**  
  - All tasks and ISRs run concurrently under FreeRTOS.

---

>[!IMPORTANT]
>Things to add:
>1. An identification of all the tasks that are performed by the system with their method of implementation: thread or interrupt
>2. A characterisation of each task with its theoretical minimum initiation interval (including assumptions used) and measured maximum execution time
>3. A critical instant analysis of the rate monotonic scheduler, showing that all deadlines are met under worst-case conditions
>4. A quantification of total CPU utilisation
>5. An identification of all the shared data structures and the methods used to guarantee safe access and synchronisation
>6. An analysis of inter-task blocking dependencies that shows any possibility of deadlock

## Advanced Features
1. Octave Tuning
2. Polyphony
3. ASDR Envelope
4. Metronome Functionality

### Octave Tuning
>[!IMPORTANT]
>Add details of this task

### Polyphony
>[!IMPORTANT]
>Add details of this task

### ASDR Envelope
>[!IMPORTANT]
>Add details of this task

### Metronome Functionality
>[!IMPORTANT]
>Add details of this task

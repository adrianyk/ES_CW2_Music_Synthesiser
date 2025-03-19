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

### ADSR Envelope
The Attack Develope Sustain Release (ASDR) envelope. Each stage of the envelope controls a different aspect of the sound's dynamics:
- Attack (A): The time taken for the sound to reach its peak volume after a key is pressed. A short attack results in an immediate sound, while a long attack creates a gradual buildup.
- Decay (D): The time taken for the sound to decrease from its peak to the sustain level. Short decay times create a sharp drop-off, while longer times make the transition smoother.
- Sustain (S): The constant volume level maintained as long as the key is held down. Unlike the other parameters, sustain is a level rather than a time-based value.
- Release (R): The time it takes for the sound to fade out completely after the key is released. A short release results in an abrupt stop, while a longer release allows the sound to decay gradually.
By tuning the parameters of each stage, the sound emitted can be changed from a flat single tone, to have a dynamic fade in and fade out when a key is pressed.

### Metronome Functionality
The metronome mode  allows the user to keep time while using keyboad. The metronome is toggled by depressing the `knob0` button at which point the display changes from the default into metronome mode. This mode periodically plays an audible 'click' sound where the tempo is adjustable between 40 and 240 bpm, to cover the range of tempos which a musician might use. 
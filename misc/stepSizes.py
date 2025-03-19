F_S = 22000 # Sampling frequency
BASE_FREQUENCY = 440 # A4
SEMITONE_RATIO = 2**(1/12)

def calculateStepSize(frequency):
    step_size = int((2**32) * frequency / F_S)
    return step_size

step_sizes = [
    calculateStepSize(BASE_FREQUENCY * (SEMITONE_RATIO ** -9)), # C4
    calculateStepSize(BASE_FREQUENCY * (SEMITONE_RATIO ** -8)), # C#4
    calculateStepSize(BASE_FREQUENCY * (SEMITONE_RATIO ** -7)), # D4
    calculateStepSize(BASE_FREQUENCY * (SEMITONE_RATIO ** -6)), # D#4
    calculateStepSize(BASE_FREQUENCY * (SEMITONE_RATIO ** -5)), # E4
    calculateStepSize(BASE_FREQUENCY * (SEMITONE_RATIO ** -4)), # F4
    calculateStepSize(BASE_FREQUENCY * (SEMITONE_RATIO ** -3)), # F#4
    calculateStepSize(BASE_FREQUENCY * (SEMITONE_RATIO ** -2)), # G4
    calculateStepSize(BASE_FREQUENCY * (SEMITONE_RATIO ** -1)), # G#4
    calculateStepSize(BASE_FREQUENCY * (SEMITONE_RATIO ** 0)), # A4 (440 Hz)
    calculateStepSize(BASE_FREQUENCY * (SEMITONE_RATIO ** 1)), # A#4
    calculateStepSize(BASE_FREQUENCY * (SEMITONE_RATIO ** 2)), # B4
]

print(step_sizes)
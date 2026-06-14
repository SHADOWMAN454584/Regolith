/**
 * ============================================================================
 * Project: Ultrasonic Lunar Dust Mitigation Driver Board Firmware
 * Version: v1.0 
 * Target: Arduino Nano (ATmega328P)
 * Output Pin: D3 (OC2B - Timer 2 Compare Match Output B)
 * Frequency: 40kHz (Default), Tunable 30kHz - 60kHz via Hardware Registers
 * ============================================================================
 * Description:
 * Direct register configuration of Timer 2 is used to generate a precise high-
 * frequency PWM signal for the piezoelectric transducer. This firmware implements:
 * 1. Precise hardware PWM frequency/duty-cycle generation.
 * 2. Automatic Frequency Sweeping to mitigate thermal & dust resonance shift.
 * 3. Space Vacuum Thermal Protection Simulation (Auto-cutoff on overheat).
 * 4. Serial Command Line Interface (CLI) for telemetry and control.
 * ============================================================================
 */

#include <EEPROM.h>

// --- Pin Definitions ---
const int PWM_PIN = 3;         // Driven by Timer 2 OC2B
const int STATUS_LED = 13;     // Built-in Arduino LED

// --- EEPROM Address Map ---
const int EEPROM_ADDR_MAGIC = 0;   // Config validity check
const int EEPROM_ADDR_STATE = 1;   // Saved operating state

// --- System Configuration & State ---
enum Mode {
  MODE_STANDBY,
  MODE_FIXED,
  MODE_SWEEP
};

struct SystemConfig {
  uint32_t fixedFrequency;     // Active frequency in Fixed Mode (Hz)
  uint32_t sweepStart;         // Sweep start frequency (Hz)
  uint32_t sweepEnd;           // Sweep end frequency (Hz)
  uint32_t sweepStep;          // Sweep frequency step size (Hz)
  uint16_t sweepInterval;      // Step interval (ms)
  uint8_t dutyCycle;           // Duty cycle percent (1-50%)
  uint16_t burstOnTime;        // Active duration in seconds (0 = disabled)
  uint16_t burstOffTime;       // Cool-down duration in seconds
};

// Global State variables
Mode currentMode = MODE_SWEEP; // Default to Sweep Mode for plug-and-play demo
SystemConfig config;
bool isOutputActive = false;
uint32_t currentFrequency = 40000;
uint32_t lastSweepStepTime = 0;
uint32_t burstTimerStart = 0;
bool inBurstCoolDown = false;

// --- Simulated Space Vacuum Thermals (Hackathon Feature) ---
float simulatedTemp = 25.0;            // Starting temperature (°C)
const float AMBIENT_TEMP = 20.0;       // Simulated ambient temp in rover bay
const float THERMAL_LIMIT = 85.0;      // Cutoff temperature (°C)
const float HEAT_FACTOR = 0.005;       // Temperature rise per ms of active drive
const float COOL_FACTOR_VACUUM = 0.0008; // Slower cooling rate (vacuum radiation only)
uint32_t lastThermalUpdate = 0;

// --- Function Prototypes ---
void initTimer2();
void setFrequencyAndDuty(uint32_t freq, uint8_t dutyPercent);
void enableOutput(bool enable);
void handleSerialCLI();
void runSweepStateMachine();
void updateThermalSimulation();
void printStatus();
void printHelp();
void loadConfig();
void saveConfig();

void setup() {
  Serial.begin(115200);
  pinMode(PWM_PIN, OUTPUT);
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(PWM_PIN, LOW);

  // Initialize and load configurations
  loadConfig();
  initTimer2();
  
  // Set initial state
  setFrequencyAndDuty(currentFrequency, config.dutyCycle);
  
  if (currentMode != MODE_STANDBY) {
    enableOutput(true);
    burstTimerStart = millis();
  }

  Serial.println(F("\n======================================================="));
  Serial.println(F(" ULTRASONIC LUNAR DUST MITIGATION DRIVER ONLINE         "));
  Serial.println(F("======================================================="));
  Serial.print(F("Current Mode: "));
  if (currentMode == MODE_STANDBY) Serial.println(F("STANDBY"));
  else if (currentMode == MODE_FIXED) Serial.println(F("FIXED (40kHz)"));
  else Serial.println(F("AUTO RESONANCE SWEEP (37kHz-43kHz)"));
  Serial.println(F("Type 'HELP' to view available serial telemetry commands.\n"));
  
  lastThermalUpdate = millis();
}

void loop() {
  handleSerialCLI();
  runSweepStateMachine();
  updateThermalSimulation();
}

// --- Timer 2 Direct Register Setup ---
void initTimer2() {
  // Set Timer 2 to Fast PWM mode, where TOP is defined by OCR2A (Mode 7)
  // Clear OC2B on compare match (non-inverting PWM on Pin 3)
  TCCR2A = _BV(COM2B1) | _BV(WGM21) | _BV(WGM20);
  
  // Set Prescaler to 8 (CS21 = 1) and enable Mode 7 (WGM22 = 1)
  TCCR2B = _BV(WGM22) | _BV(CS21); 
  
  // Output initially disabled
  TCCR2A &= ~(_BV(COM2B1));
}

// Configure frequency and duty cycle using hardware registers
void setFrequencyAndDuty(uint32_t freq, uint8_t dutyPercent) {
  if (freq < 30000) freq = 30000;
  if (freq > 60000) freq = 60000;
  if (dutyPercent > 50) dutyPercent = 50; // Max efficiency cap for piezo transducers

  currentFrequency = freq;

  // Formula: OCR2A = (F_CPU / (Prescaler * Target_Freq)) - 1
  // With F_CPU = 16MHz and Prescaler = 8:
  // OCR2A = (2,000,000 / freq) - 1
  uint8_t topVal = (2000000UL / freq) - 1;
  
  // Set TOP value
  OCR2A = topVal;

  // Calculate compare match value for duty cycle
  // OCR2B = (OCR2A + 1) * (DutyPercent / 100) - 1
  uint32_t rawDuty = ((uint32_t)(topVal + 1) * dutyPercent) / 100;
  if (rawDuty > 0) {
    OCR2B = rawDuty - 1;
  } else {
    OCR2B = 0;
  }
}

// Connect/disconnect pin D3 to/from Timer 2 PWM output
void enableOutput(bool enable) {
  if (enable && !inBurstCoolDown && simulatedTemp < THERMAL_LIMIT) {
    TCCR2A |= _BV(COM2B1);
    isOutputActive = true;
    digitalWrite(STATUS_LED, HIGH);
  } else {
    TCCR2A &= ~(_BV(COM2B1));
    digitalWrite(PWM_PIN, LOW); // Pull low for safety
    isOutputActive = false;
    digitalWrite(STATUS_LED, LOW);
  }
}

// --- Telemetry CLI Parser ---
void handleSerialCLI() {
  if (!Serial.available()) return;

  String input = Serial.readStringUntil('\n');
  input.trim();
  input.toUpperCase();

  if (input.length() == 0) return;

  if (input == "HELP") {
    printHelp();
  } 
  else if (input == "START") {
    currentMode = MODE_FIXED;
    inBurstCoolDown = false;
    enableOutput(true);
    burstTimerStart = millis();
    Serial.println(F("OK: Transducer Driver Started (Fixed Mode)"));
  } 
  else if (input == "STOP") {
    currentMode = MODE_STANDBY;
    enableOutput(false);
    Serial.println(F("OK: Transducer Driver Paused (Standby Mode)"));
  } 
  else if (input.startsWith("SETFREQ ")) {
    uint32_t targetFreq = input.substring(8).toInt();
    if (targetFreq >= 30000 && targetFreq <= 60000) {
      config.fixedFrequency = targetFreq;
      currentMode = MODE_FIXED;
      setFrequencyAndDuty(targetFreq, config.dutyCycle);
      Serial.print(F("OK: Frequency set to "));
      Serial.print(targetFreq);
      Serial.println(F(" Hz"));
    } else {
      Serial.println(F("ERROR: Frequency must be between 30000 and 60000 Hz."));
    }
  } 
  else if (input.startsWith("SETDUTY ")) {
    int targetDuty = input.substring(8).toInt();
    if (targetDuty >= 1 && targetDuty <= 50) {
      config.dutyCycle = targetDuty;
      setFrequencyAndDuty(currentFrequency, targetDuty);
      Serial.print(F("OK: Duty cycle set to "));
      Serial.print(targetDuty);
      Serial.println(F("%"));
    } else {
      Serial.println(F("ERROR: Duty cycle must be between 1% and 50%."));
    }
  } 
  else if (input == "SWEEP") {
    currentMode = MODE_SWEEP;
    inBurstCoolDown = false;
    currentFrequency = config.sweepStart;
    enableOutput(true);
    burstTimerStart = millis();
    Serial.println(F("OK: Resonance Sweeping Active."));
  } 
  else if (input.startsWith("SETSWEEP ")) {
    // Expected format: SETSWEEP [start_hz] [end_hz] [step_hz] [interval_ms]
    // Parse arguments
    int index1 = input.indexOf(' ', 9);
    int index2 = input.indexOf(' ', index1 + 1);
    int index3 = input.indexOf(' ', index2 + 1);

    if (index1 != -1 && index2 != -1 && index3 != -1) {
      uint32_t start = input.substring(9, index1).toInt();
      uint32_t end = input.substring(index1 + 1, index2).toInt();
      uint32_t step = input.substring(index2 + 1, index3).toInt();
      uint16_t interval = input.substring(index3 + 1).toInt();

      if (start >= 30000 && end <= 60000 && start < end && step > 0 && interval > 0) {
        config.sweepStart = start;
        config.sweepEnd = end;
        config.sweepStep = step;
        config.sweepInterval = interval;
        Serial.println(F("OK: Sweep parameters updated."));
      } else {
        Serial.println(F("ERROR: Invalid sweep parameters."));
      }
    } else {
      Serial.println(F("ERROR: Format: SETSWEEP [start] [end] [step] [interval]"));
    }
  }
  else if (input.startsWith("BURST ")) {
    int index = input.indexOf(' ', 6);
    if (index != -1) {
      int onTime = input.substring(6, index).toInt();
      int offTime = input.substring(index + 1).toInt();
      if (onTime >= 0 && offTime >= 0) {
        config.burstOnTime = onTime;
        config.burstOffTime = offTime;
        Serial.println(F("OK: Burst limits updated."));
      } else {
        Serial.println(F("ERROR: Times must be positive."));
      }
    } else {
      Serial.println(F("ERROR: Format: BURST [on_sec] [off_sec]"));
    }
  }
  else if (input == "STATUS") {
    printStatus();
  }
  else if (input == "SAVE") {
    saveConfig();
    Serial.println(F("OK: Settings saved to EEPROM."));
  }
  else {
    Serial.println(F("ERROR: Command unrecognized. Type 'HELP' for commands."));
  }
}

// --- Resonance Sweep State Machine ---
void runSweepStateMachine() {
  if (currentMode != MODE_SWEEP || !isOutputActive) return;

  uint32_t now = millis();
  if (now - lastSweepStepTime >= config.sweepInterval) {
    lastSweepStepTime = now;

    // Advance sweep
    uint32_t nextFreq = currentFrequency + config.sweepStep;
    if (nextFreq > config.sweepEnd) {
      nextFreq = config.sweepStart; // Wrap around
    }

    setFrequencyAndDuty(nextFreq, config.dutyCycle);
    
    // Status flashing effect
    digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
  }
}

// --- Vacuum Thermal Protection & Safety System ---
void updateThermalSimulation() {
  uint32_t now = millis();
  uint32_t elapsed = now - lastThermalUpdate;
  lastThermalUpdate = now;

  if (isOutputActive) {
    // 1. Thermal Simulation: Temperature rises based on driver power in vacuum
    float heatGen = (float)elapsed * HEAT_FACTOR * ((float)config.dutyCycle / 50.0);
    simulatedTemp += heatGen;

    // Overtemperature Cutoff Check
    if (simulatedTemp >= THERMAL_LIMIT) {
      enableOutput(false);
      Serial.print(F("\n[CRITICAL ERROR] Thermal Cutoff triggered! Transducer Temp: "));
      Serial.print(simulatedTemp, 1);
      Serial.println(F(" C. Output shut down for safety."));
    }

    // 2. Active Burst Timer Limit Check
    if (config.burstOnTime > 0) {
      uint32_t activeSec = (now - burstTimerStart) / 1000;
      if (activeSec >= config.burstOnTime) {
        inBurstCoolDown = true;
        enableOutput(false);
        burstTimerStart = now; // Reset timer for cooldown track
        Serial.println(F("\n[INFO] Duty Burst limit reached. Entering cool-down cycle."));
      }
    }
  } else {
    // Cool down simulation (Radiative cooling in vacuum follows Stefan-Boltzmann cooling curve simulation)
    // Slower than normal air convection cooling
    float coolRate = (simulatedTemp - AMBIENT_TEMP) * COOL_FACTOR_VACUUM * (float)elapsed;
    simulatedTemp -= coolRate;
    if (simulatedTemp < AMBIENT_TEMP) simulatedTemp = AMBIENT_TEMP;

    // Handle Cool-down timer recovery
    if (inBurstCoolDown) {
      uint32_t coolSec = (now - burstTimerStart) / 1000;
      if (coolSec >= config.burstOffTime && simulatedTemp < (THERMAL_LIMIT - 15.0)) {
        inBurstCoolDown = false;
        if (currentMode != MODE_STANDBY) {
          enableOutput(true);
          burstTimerStart = now;
          Serial.println(F("\n[INFO] Cool-down complete. Resuming dust mitigation driving."));
        }
      }
    }
  }
}

// --- Status Telemetry Output ---
void printStatus() {
  Serial.println(F("\n--- SYSTEM TELEMETRY STATUS ---"));
  
  Serial.print(F("Operating Mode:    "));
  if (currentMode == MODE_STANDBY) Serial.println(F("STANDBY"));
  else if (currentMode == MODE_FIXED) Serial.println(F("FIXED"));
  else Serial.println(F("AUTO SWEEP"));

  Serial.print(F("Output Driver:     "));
  Serial.println(isOutputActive ? F("ACTIVE (ON)") : F("INACTIVE (OFF)"));

  Serial.print(F("Drive Frequency:   "));
  Serial.print(currentFrequency);
  Serial.println(F(" Hz"));

  Serial.print(F("PWM Duty Cycle:    "));
  Serial.print(config.dutyCycle);
  Serial.println(F("%"));

  Serial.print(F("Transducer Temp:   "));
  Serial.print(simulatedTemp, 1);
  Serial.print(F(" C  (State: "));
  if (simulatedTemp >= (THERMAL_LIMIT - 10.0)) Serial.print(F("WARNING HOT"));
  else if (isOutputActive) Serial.print(F("HEATING"));
  else Serial.print(F("COOLING"));
  Serial.println(F(")"));

  Serial.print(F("Safety Cooldown:   "));
  Serial.println(inBurstCoolDown ? F("ACTIVE (COOLING PERIOD)") : F("OFF (READY)"));

  Serial.print(F("Burst Settings:    "));
  Serial.print(config.burstOnTime);
  Serial.print(F("s ON / "));
  Serial.print(config.burstOffTime);
  Serial.println(F("s OFF"));

  Serial.print(F("Sweep Bounds:      "));
  Serial.print(config.sweepStart);
  Serial.print(F(" Hz - "));
  Serial.print(config.sweepEnd);
  Serial.print(F(" Hz, Step: "));
  Serial.print(config.sweepStep);
  Serial.print(F(" Hz, Rate: "));
  Serial.print(config.sweepInterval);
  Serial.println(F("ms"));
  
  Serial.println(F("--------------------------------"));
}

void printHelp() {
  Serial.println(F("\n--- COMMAND INTERFACE HELP MENU ---"));
  Serial.println(F("START             - Turn ON transducer drive (Fixed Mode)"));
  Serial.println(F("STOP              - Turn OFF transducer drive (Standby Mode)"));
  Serial.println(F("SWEEP             - Turn ON dynamic resonance sweep mode"));
  Serial.println(F("STATUS            - Print full system status and temperature telemetry"));
  Serial.println(F("SETFREQ [Hz]      - Set fixed frequency (30000 - 60000 Hz)"));
  Serial.println(F("SETDUTY [%]       - Set power duty cycle (1 - 50 %)"));
  Serial.println(F("SETSWEEP [start_hz] [end_hz] [step_hz] [interval_ms]"));
  Serial.println(F("                  - Configure resonance frequency sweep profile"));
  Serial.println(F("BURST [on] [off]  - Set active timer limit and cooldown time (seconds)"));
  Serial.println(F("SAVE              - Store current configurations in EEPROM"));
  Serial.println(F("HELP              - Show this menu"));
  Serial.println(F("------------------------------------"));
}

// --- Configuration Storage Management ---
void loadConfig() {
  uint8_t magic = EEPROM.read(EEPROM_ADDR_MAGIC);
  
  if (magic == 0xA5) {
    // Loaded from saved configuration
    EEPROM.get(sizeof(magic), config);
    currentMode = (Mode)EEPROM.read(EEPROM_ADDR_STATE);
  } else {
    // Load factory defaults
    config.fixedFrequency = 40000;
    config.sweepStart = 37000;
    config.sweepEnd = 43000;
    config.sweepStep = 100;
    config.sweepInterval = 100; // Step every 100ms
    config.dutyCycle = 50;      // 50% duty cycle (ideal square wave efficiency)
    config.burstOnTime = 15;    // Run for 15s before cooling down
    config.burstOffTime = 30;   // Cooldown for 30s
    currentMode = MODE_SWEEP;   // Start in sweep mode by default
  }
}

void saveConfig() {
  uint8_t magic = 0xA5;
  EEPROM.write(EEPROM_ADDR_MAGIC, magic);
  EEPROM.put(sizeof(magic), config);
  EEPROM.write(EEPROM_ADDR_STATE, (uint8_t)currentMode);
}

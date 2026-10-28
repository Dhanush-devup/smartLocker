/*
  Smart Locker - Tamper State Machine (Stage 1)
  ------------------------------------------------------------
  Stage 1 implements the approved movement-confirmation behavior:
    - Weak/moderate movement must be confirmed across several samples.
    - Strong movement requires brief corroboration, not a single spike.
    - An alarm incident latches independently of the buzzer timer.
    - LOCK/UNLOCK commands do not clear a latched alarm.
    - IR state is debounced; an object removed (or inserted) while armed latches
      the alarm, the same as confirmed movement.
    - Sensor failures are reported as faults, not treated as zero movement.

  Hardware pins confirmed for this project:
    ADXL345 SDA = GPIO 21, SCL = GPIO 22, I2C address = 0x53
    Servo signal = GPIO 18
    Buzzer = GPIO 23
    IR OUT = GPIO 27 (active LOW means object detected)

  Arduino IDE libraries required:
    - Adafruit ADXL345
    - Adafruit Unified Sensor
    - ESP32Servo

  IMPORTANT:
    - Set Wi-Fi, server URL and device key in secrets.h before uploading.
    - The API endpoints here match the current project backend contract.
    - This version uses plain HTTP for the local Flask server; use only on a trusted
      network. HTTPS requires WiFiClientSecure and proper certificate validation.
    - Movement thresholds below are PROVISIONAL. Calibrate them on the real box.
    - Supports LOCK, UNLOCK, and remote CLEAR_ALARM commands from the Flask backend.
    - Remote clear commands are acknowledged only after the sensor task executes
      the clear operation, using POST /api/command-ack.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>
#include <ESP32Servo.h>
#include <math.h>


// ----------------------------- USER CONFIG -----------------------------
// WIFI_SSID, WIFI_PASSWORD, SERVER_BASE_URL and DEVICE_API_KEY live in
// secrets.h (not committed). Copy secrets.example.h to secrets.h to set them.
#include "secrets.h"

// ----------------------------- PIN CONFIG ------------------------------
static constexpr int I2C_SDA_PIN = 21;
static constexpr int I2C_SCL_PIN = 22;
static constexpr int SERVO_PIN = 18;
static constexpr int BUZZER_PIN = 23;
static constexpr int IR_PIN = 27;

// --------------------------- TIMING SETTINGS ----------------------------
static constexpr uint32_t SENSOR_INTERVAL_MS = 50;
static constexpr uint32_t IR_DEBOUNCE_MS = 100;
static constexpr uint32_t WEAK_CONFIRM_WINDOW_MS = 600;
static constexpr uint32_t WEAK_CONFIRM_MIN_HITS = 4;
static constexpr uint32_t STRONG_CONFIRM_WINDOW_MS = 180;
static constexpr uint32_t STRONG_CONFIRM_MIN_HITS = 2;
static constexpr uint32_t REARM_STABLE_MS = 2000;
static constexpr uint32_t BUZZER_DURATION_MS = 2000;
static constexpr uint32_t WIFI_RETRY_MS = 10000;
static constexpr uint32_t COMMAND_POLL_MS = 1000;
static constexpr uint32_t HEARTBEAT_MS = 3000;
static constexpr uint32_t EVENT_RETRY_MS = 1500;
static constexpr uint32_t IR_EVENT_RATE_LIMIT_MS = 5000;

// ----------------------- PROVISIONAL THRESHOLDS ------------------------
// Adafruit_ADXL345_Unified reports acceleration in m/s^2, NOT raw counts.
// These are conservative starting points for diagnostic calibration.
// Review the live [ADXL] delta output while stationary and while shaking.
static constexpr float MOVEMENT_THRESHOLD = 3.0f;
static constexpr float STRONG_MOVEMENT_THRESHOLD = 8.0f;
static constexpr float STABILITY_THRESHOLD = 2.5f;
static constexpr uint32_t ADXL_DIAGNOSTIC_INTERVAL_MS = 250;
static constexpr uint8_t BASELINE_SAMPLE_COUNT = 30;

// Servo angles may need adjustment for your physical latch.
static constexpr int SERVO_LOCK_ANGLE = 0;
static constexpr int SERVO_UNLOCK_ANGLE = 90;

// ------------------------------ OBJECTS --------------------------------
Adafruit_ADXL345_Unified accelerometer = Adafruit_ADXL345_Unified(12345);
Servo lockServo;
WiFiClient httpClient;

// ------------------------------- STATES --------------------------------
enum LockerCommand : uint8_t { CMD_NONE, CMD_LOCK, CMD_UNLOCK, CMD_CLEAR_ALARM };
enum MovementState : uint8_t { MOVE_NORMAL, MOVE_SUSPECTED, MOVE_TAMPERING };
enum SecurityState : uint8_t { SEC_STARTING, SEC_ARMED, SEC_REARMING, SEC_AUTHORIZED_ACCESS, SEC_ALARM_LATCHED, SEC_SENSOR_FAULT };
enum IRState : uint8_t { IR_UNKNOWN, IR_PRESENT, IR_ABSENT };

struct LockerSnapshot {
  bool locked;
  bool alarmBuzzerOn;
  bool incidentLatched;
  bool accelerometerFault;
  MovementState movement;
  SecurityState security;
  IRState ir;
  float lastDelta;
  uint32_t eventSequence;
};

struct QueuedLockerCommand {
  LockerCommand command;
  uint32_t commandId;  // Non-zero only for a remote CLEAR_ALARM command.
  bool remote;
};

struct ClearAckMessage {
  uint32_t commandId;
  bool success;
};

struct EventMessage {
  char type[28];
  char message[140];
  bool locked;
  bool alarm;
  bool hasMovement;
  bool hasIR;
  char movement[24];
  char security[28];
  char ir[16];
  uint32_t id;
};

static QueueHandle_t commandQueue = nullptr;
static QueueHandle_t clearAckQueue = nullptr;
static QueueHandle_t eventQueue = nullptr;
static uint32_t lastQueuedClearCommandId = 0;
static SemaphoreHandle_t stateMutex = nullptr;
static SemaphoreHandle_t networkMutex = nullptr;

static LockerSnapshot state = { true, false, false, false, MOVE_NORMAL, SEC_STARTING, IR_UNKNOWN, 0.0f, 0 };
static bool sensorReady = false;
static bool irBaselineSet = false;
static IRState irBaseline = IR_UNKNOWN;
static bool rearmPending = false;
static uint32_t rearmStableSince = 0;
static uint32_t buzzerStartedAt = 0;
static uint32_t lastWiFiRetryAt = 0;
static uint32_t lastCommandPollAt = 0;
static uint32_t lastHeartbeatAt = 0;
static uint32_t lastIREventAt = 0;
static uint32_t lastEventRetryAt = 0;
static uint32_t nextEventId = 1;
static float baselineMagnitude = 0.0f;
static bool baselineReady = false;
static uint8_t baselineCount = 0;
static float baselineAccumulator = 0.0f;
static uint32_t weakWindowStartedAt = 0;
static uint32_t strongWindowStartedAt = 0;
static uint8_t weakHits = 0;
static uint8_t strongHits = 0;
static uint32_t lastSignificantAt = 0;
static float lastMagnitude = 0.0f;
static float rearmLastMagnitude = 0.0f;
static bool rearmHaveLastMagnitude = false;
static bool candidateWasStrong = false;
static int irRawStable = HIGH;
static int irRawCandidate = HIGH;
static uint32_t irCandidateSince = 0;
static bool irDebounceInitialized = false;
static bool lastPostedAlarm = false;
static char lastPostedEvent[28] = "";

// -------------------------- STRING HELPERS ----------------------------
const char* movementName(MovementState v) {
  switch (v) {
    case MOVE_NORMAL: return "NORMAL";
    case MOVE_SUSPECTED: return "SUSPECTED";
    case MOVE_TAMPERING: return "TAMPERING";
    default: return "UNKNOWN";
  }
}
const char* securityName(SecurityState v) {
  switch (v) {
    case SEC_STARTING: return "STARTING";
    case SEC_ARMED: return "ARMED";
    case SEC_REARMING: return "REARMING";
    case SEC_AUTHORIZED_ACCESS: return "AUTHORIZED_ACCESS";
    case SEC_ALARM_LATCHED: return "ALARM_LATCHED";
    case SEC_SENSOR_FAULT: return "SENSOR_FAULT";
    default: return "UNKNOWN";
  }
}
const char* irName(IRState v) {
  switch (v) {
    case IR_PRESENT: return "PRESENT";
    case IR_ABSENT: return "ABSENT";
    default: return "UNKNOWN";
  }
}

LockerSnapshot getSnapshot() {
  LockerSnapshot copy;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    copy = state;
    xSemaphoreGive(stateMutex);
  } else {
    copy = state;
  }
  return copy;
}

void setSecurityState(SecurityState next) {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    // A latched incident takes precedence over ordinary security states.
    if (state.incidentLatched && next != SEC_SENSOR_FAULT) state.security = SEC_ALARM_LATCHED;
    else state.security = next;
    xSemaphoreGive(stateMutex);
  }
}

void queueEvent(const char* type, const char* message, bool hasMovement = true, bool hasIR = true) {
  EventMessage ev = {};
  snprintf(ev.type, sizeof(ev.type), "%s", type);
  snprintf(ev.message, sizeof(ev.message), "%s", message);
  LockerSnapshot s = getSnapshot();
  ev.locked = s.locked;
  ev.alarm = s.incidentLatched;
  ev.hasMovement = hasMovement;
  ev.hasIR = hasIR;
  snprintf(ev.movement, sizeof(ev.movement), "%s", movementName(s.movement));
  snprintf(ev.security, sizeof(ev.security), "%s", securityName(s.security));
  snprintf(ev.ir, sizeof(ev.ir), "%s", irName(s.ir));
  ev.id = nextEventId++;
  if (xQueueSend(eventQueue, &ev, 0) != pdTRUE) {
    // If full, discard one old low-priority event so a security event can be queued.
    EventMessage discarded;
    xQueueReceive(eventQueue, &discarded, 0);
    xQueueSend(eventQueue, &ev, 0);
  }
  Serial.printf("[EVENT %lu] %s: %s\n", (unsigned long)ev.id, ev.type, ev.message);
}

void setMovementState(MovementState next) {
  bool changed = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (state.movement != next) {
      state.movement = next;
      changed = true;
    }
    xSemaphoreGive(stateMutex);
  }
  if (changed) Serial.printf("[STATE] Movement -> %s\n", movementName(next));
}

// --------------------------- ACTUATOR LOGIC ----------------------------
void startBuzzer() {
  digitalWrite(BUZZER_PIN, HIGH);
  buzzerStartedAt = millis();
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    state.alarmBuzzerOn = true;
    xSemaphoreGive(stateMutex);
  }
}

void stopBuzzer() {
  digitalWrite(BUZZER_PIN, LOW);
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    state.alarmBuzzerOn = false;
    xSemaphoreGive(stateMutex);
  }
}

// movementCaused=false is used for IR-triggered alarms so the movement state
// is not reported as TAMPERING when the accelerometer did not confirm it.
void triggerTamperAlarm(const char* reason, const char* eventType = "TAMPER_ALARM", bool movementCaused = true) {
  bool wasLatched = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    wasLatched = state.incidentLatched;
    state.incidentLatched = true;
    if (movementCaused) state.movement = MOVE_TAMPERING;
    state.security = SEC_ALARM_LATCHED;
    state.eventSequence++;
    xSemaphoreGive(stateMutex);
  }
  if (!wasLatched) {
    startBuzzer();
    queueEvent(eventType, reason, true, true);
  } else {
    Serial.println("[ALARM] Incident already latched; not generating duplicate alarm.");
  }
}

void clearLatchedAlarmLocal() {
  bool hadIncident = false;
  bool isLocked = true;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    hadIncident = state.incidentLatched;
    state.incidentLatched = false;
    state.movement = MOVE_NORMAL;
    isLocked = state.locked;
    state.security = isLocked ? SEC_REARMING : SEC_AUTHORIZED_ACCESS;
    xSemaphoreGive(stateMutex);
  }

  // IMPORTANT: entering SEC_REARMING must also enable the re-arm state machine.
  // Without this, updateRearmState() returns immediately forever and movement
  // monitoring remains suppressed after CLEAR.
  if (isLocked) {
    rearmPending = true;
    rearmStableSince = 0;
    rearmHaveLastMagnitude = false;
    rearmLastMagnitude = 0.0f;
    baselineReady = false;
    baselineCount = 0;
    baselineAccumulator = 0.0f;
  } else {
    rearmPending = false;
    rearmStableSince = 0;
    rearmHaveLastMagnitude = false;
  }
  resetMovementCandidate();
  stopBuzzer();

  if (hadIncident) {
    queueEvent("ALARM_CLEARED", "Latched alarm cleared by an explicit clear command", false, false);
    Serial.println("[ALARM] Incident cleared by explicit command.");
    if (isLocked) Serial.println("[SECURITY] Re-arming started; keep locker still until [SECURITY] Re-arming complete appears.");
  } else {
    Serial.println("[ALARM] No latched incident to clear.");
  }
}

void applyLockCommand() {
  lockServo.write(SERVO_LOCK_ANGLE);
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    state.locked = true;
    if (!state.incidentLatched) state.security = SEC_REARMING;
    xSemaphoreGive(stateMutex);
  }
  rearmPending = true;
  rearmStableSince = 0;
  rearmHaveLastMagnitude = false;
  baselineReady = false;
  baselineCount = 0;
  baselineAccumulator = 0.0f;
  weakHits = strongHits = 0;
  setMovementState(MOVE_NORMAL);
  queueEvent("LOCK", "Locker lock command applied; re-arming after stability check", false, false);
  Serial.println("[LOCK] Servo commanded to LOCK. This does not prove the physical door is closed.");
}

void applyUnlockCommand() {
  lockServo.write(SERVO_UNLOCK_ANGLE);
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    state.locked = false;
    if (!state.incidentLatched) state.security = SEC_AUTHORIZED_ACCESS;
    xSemaphoreGive(stateMutex);
  }
  rearmPending = false;
  rearmStableSince = 0;
  weakHits = strongHits = 0;
  setMovementState(MOVE_NORMAL);
  queueEvent("UNLOCK", "Locker unlock command applied; movement monitoring suppressed during access", false, false);
  Serial.println("[UNLOCK] Servo commanded to UNLOCK. Alarm incidents, if any, remain latched.");
}

// --------------------------- SENSOR HELPERS ----------------------------
bool readAcceleration(float &magnitude) {
  if (!sensorReady) return false;
  sensors_event_t event;
  if (!accelerometer.getEvent(&event)) return false;
  const float x = event.acceleration.x;
  const float y = event.acceleration.y;
  const float z = event.acceleration.z;
  if (!isfinite(x) || !isfinite(y) || !isfinite(z)) return false;
  magnitude = sqrtf(x * x + y * y + z * z);
  return isfinite(magnitude);
}

IRState rawIRState(int raw) {
  return raw == LOW ? IR_PRESENT : IR_ABSENT; // active LOW = object detected
}

void updateIRDebounce() {
  const int raw = digitalRead(IR_PIN);
  const uint32_t now = millis();
  if (!irDebounceInitialized) {
    irRawCandidate = raw;
    irRawStable = raw;
    irCandidateSince = now;
    irDebounceInitialized = true;
    IRState initial = rawIRState(raw);
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      state.ir = initial;
      xSemaphoreGive(stateMutex);
    }
    irBaseline = initial;
    irBaselineSet = true;
    Serial.printf("[IR] Initial state = %s (not treated as a removal event)\n", irName(initial));
    return;
  }
  if (raw != irRawCandidate) {
    irRawCandidate = raw;
    irCandidateSince = now;
  }
  if (irRawCandidate != irRawStable && (now - irCandidateSince) >= IR_DEBOUNCE_MS) {
    irRawStable = irRawCandidate;
    IRState next = rawIRState(irRawStable);
    IRState previous;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      previous = state.ir;
      state.ir = next;
      xSemaphoreGive(stateMutex);
    } else {
      previous = next;
    }
    if (previous != next) {
      LockerSnapshot s = getSnapshot();
      Serial.printf("[IR] Stable transition: %s -> %s; locker=%s\n", irName(previous), irName(next), s.locked ? "LOCKED" : "UNLOCKED");
      if (!s.locked) {
        // During authorized access, contents may legitimately change. Track current state.
        irBaseline = next;
        irBaselineSet = true;
        queueEvent("IR_CHANGE", "IR state changed during unlocked access; accepted as expected activity", false, true);
      } else if (s.security == SEC_REARMING || rearmPending) {
        // While re-arming, accept a stable state as the new baseline.
        irBaseline = next;
        irBaselineSet = true;
        queueEvent("IR_CHANGE", "IR state changed while re-arming; new baseline recorded", false, true);
      } else {
        // Locked and armed: any stable change from the locked baseline is tampering.
        // This still applies if the ADXL345 is faulted, so IR keeps protecting the locker.
        if (!irBaselineSet) {
          irBaseline = next;
          irBaselineSet = true;
        } else if (next != irBaseline) {
          const bool removed = (next == IR_ABSENT);
          const bool movementSuspected = (s.movement == MOVE_SUSPECTED);
          if (!s.incidentLatched) {
            char reason[140];
            snprintf(reason, sizeof(reason), "%s while locker is locked%s",
                     removed ? "Object removed" : "Object inserted",
                     movementSuspected ? "; movement was also suspected" : "");
            triggerTamperAlarm(reason, removed ? "TAMPER_OBJECT_REMOVED" : "TAMPER_OBJECT_INSERTED", false);
          } else if ((now - lastIREventAt) >= IR_EVENT_RATE_LIMIT_MS) {
            // Alarm already latched: record further object changes without re-alarming.
            lastIREventAt = now;
            queueEvent(removed ? "OBJECT_REMOVED" : "OBJECT_INSERTED", "IR object state changed while alarm is latched", false, true);
          }
        } else {
          queueEvent("IR_CHANGE", "IR state returned to the locked baseline", false, true);
        }
      }
    }
  }
}

void resetMovementCandidate() {
  weakHits = 0;
  strongHits = 0;
  weakWindowStartedAt = 0;
  strongWindowStartedAt = 0;
  candidateWasStrong = false;
  lastSignificantAt = 0;
  if (!getSnapshot().incidentLatched) setMovementState(MOVE_NORMAL);
}

void updateRearmState(float magnitude, bool validRead) {
  LockerSnapshot s = getSnapshot();
  if (!rearmPending || !s.locked || s.incidentLatched) return;
  if (!validRead) {
    rearmStableSince = 0;
    rearmHaveLastMagnitude = false;
    return;
  }
  // During re-arming, compare consecutive readings so the stability timer does
  // not pass merely because the long-term movement baseline has not been built yet.
  if (rearmHaveLastMagnitude && fabsf(magnitude - rearmLastMagnitude) > STABILITY_THRESHOLD) {
    rearmStableSince = 0;
    rearmLastMagnitude = magnitude;
    return;
  }
  rearmLastMagnitude = magnitude;
  rearmHaveLastMagnitude = true;
  if (rearmStableSince == 0) rearmStableSince = millis();
  if ((millis() - rearmStableSince) >= REARM_STABLE_MS) {
    rearmPending = false;
    baselineReady = false;
    baselineCount = 0;
    baselineAccumulator = 0.0f;
    resetMovementCandidate();
    irBaseline = s.ir;
    irBaselineSet = (s.ir != IR_UNKNOWN);
    setSecurityState(SEC_ARMED);
    queueEvent("ARMED", "Locker stable; monitoring armed and sensor baselines refreshed", false, true);
    Serial.println("[SECURITY] Re-arming complete.");
  }
}

void processMovementSample(float magnitude) {
  const uint32_t now = millis();
  LockerSnapshot s = getSnapshot();
  if (!s.locked || s.incidentLatched || rearmPending || s.security == SEC_REARMING || s.security == SEC_STARTING) {
    return;
  }
  if (s.security == SEC_SENSOR_FAULT) setSecurityState(SEC_ARMED);

  if (!baselineReady) {
    baselineAccumulator += magnitude;
    baselineCount++;
    if (baselineCount >= BASELINE_SAMPLE_COUNT) {
      baselineMagnitude = baselineAccumulator / baselineCount;
      baselineReady = true;
      lastMagnitude = magnitude;
      Serial.printf("[SENSOR] Baseline ready: %.2f m/s^2 magnitude\n", baselineMagnitude);
      queueEvent("SENSOR_BASELINE", "Movement baseline established", false, false);
    }
    return;
  }

  const float delta = fabsf(magnitude - baselineMagnitude);
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    state.lastDelta = delta;
    xSemaphoreGive(stateMutex);
  }
  if (delta < MOVEMENT_THRESHOLD) {
    // Don't clear a candidate immediately after a single quiet sample. Expire by time.
    if (weakHits > 0 && (now - lastSignificantAt) > WEAK_CONFIRM_WINDOW_MS) resetMovementCandidate();
    lastMagnitude = magnitude;
    return;
  }

  if (weakHits == 0) weakWindowStartedAt = now;
  if ((now - weakWindowStartedAt) > WEAK_CONFIRM_WINDOW_MS) {
    weakHits = 0;
    strongHits = 0;
    weakWindowStartedAt = now;
    strongWindowStartedAt = 0;
  }
  weakHits++;
  lastSignificantAt = now;
  setMovementState(MOVE_SUSPECTED);

  if (delta >= STRONG_MOVEMENT_THRESHOLD) {
    if (strongHits == 0) strongWindowStartedAt = now;
    if ((now - strongWindowStartedAt) <= STRONG_CONFIRM_WINDOW_MS) strongHits++;
    else {
      strongWindowStartedAt = now;
      strongHits = 1;
    }
    candidateWasStrong = true;
    Serial.printf("[MOVE] Strong candidate delta=%.2f, corroborations=%u\n", delta, strongHits);
    if (strongHits >= STRONG_CONFIRM_MIN_HITS) {
      triggerTamperAlarm("Strong movement confirmed by multiple consecutive sensor samples while locked");
      return;
    }
  } else {
    Serial.printf("[MOVE] Movement candidate delta=%.2f, confirmations=%u/%u\n", delta, weakHits, WEAK_CONFIRM_MIN_HITS);
    if (weakHits >= WEAK_CONFIRM_MIN_HITS && (now - weakWindowStartedAt) <= WEAK_CONFIRM_WINDOW_MS) {
      triggerTamperAlarm(candidateWasStrong ? "Movement confirmed by repeated significant readings" : "Repeated moderate movement confirmed within the confirmation window");
      return;
    }
  }
  lastMagnitude = magnitude;
}

void sensorTask(void* parameter) {
  (void)parameter;
  uint32_t lastReadAt = 0;
  uint32_t lastDiagnosticAt = 0;
  bool faultReported = false;
  while (true) {
    const uint32_t now = millis();
    updateIRDebounce();
    if (now - lastReadAt >= SENSOR_INTERVAL_MS) {
      lastReadAt = now;
      float magnitude = 0.0f;
      const bool valid = readAcceleration(magnitude);
      if (!valid) {
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          state.accelerometerFault = true;
          if (!state.incidentLatched) state.security = SEC_SENSOR_FAULT;
          xSemaphoreGive(stateMutex);
        }
        if (!faultReported) {
          queueEvent("SENSOR_FAULT", "ADXL345 read failed or returned invalid acceleration; movement decisions paused", false, false);
          faultReported = true;
        }
        rearmStableSince = 0;
      } else {
        if (faultReported) {
          if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            state.accelerometerFault = false;
            if (!state.incidentLatched && state.locked) state.security = rearmPending ? SEC_REARMING : SEC_ARMED;
            xSemaphoreGive(stateMutex);
          }
          queueEvent("SENSOR_RECOVERED", "ADXL345 readings recovered", false, false);
          faultReported = false;
          baselineReady = false;
          baselineCount = 0;
          baselineAccumulator = 0.0f;
        }

        // Diagnostic output is deliberately independent of event detection:
        // it proves whether readings change even when no tamper threshold is met.
        if (now - lastDiagnosticAt >= ADXL_DIAGNOSTIC_INTERVAL_MS) {
          lastDiagnosticAt = now;
          LockerSnapshot diagnosticState = getSnapshot();
          Serial.printf("[ADXL] magnitude=%.2f m/s^2 | ",
                        magnitude);
          if (baselineReady) {
            Serial.printf("delta=%.2f m/s^2 | threshold=%.2f/%.2f | ",
                          fabsf(magnitude - baselineMagnitude),
                          MOVEMENT_THRESHOLD,
                          STRONG_MOVEMENT_THRESHOLD);
          } else {
            Serial.print("delta=CALIBRATING | ");
          }
          Serial.printf("baseline=%s | locked=%s | security=%s\n",
                        baselineReady ? "READY" : "BUILDING",
                        diagnosticState.locked ? "YES" : "NO",
                        securityName(diagnosticState.security));
        }

        updateRearmState(magnitude, true);
        LockerSnapshot s = getSnapshot();
        if (s.locked && !rearmPending && s.security != SEC_STARTING && !s.incidentLatched) {
          processMovementSample(magnitude);
        }
      }
    }

    // The sensor/control task owns all actuator and command changes.
    // Network task only puts received commands into commandQueue.
    QueuedLockerCommand queuedCommand;
    while (xQueueReceive(commandQueue, &queuedCommand, 0) == pdTRUE) {
      if (queuedCommand.command == CMD_LOCK) {
        applyLockCommand();
      } else if (queuedCommand.command == CMD_UNLOCK) {
        applyUnlockCommand();
      } else if (queuedCommand.command == CMD_CLEAR_ALARM) {
        // The sensor/control task executes the clear; only afterwards is the
        // result queued for the network task to acknowledge to Flask.
        clearLatchedAlarmLocal();
        if (queuedCommand.remote && queuedCommand.commandId != 0) {
          ClearAckMessage ack = { queuedCommand.commandId, true };
          if (xQueueSend(clearAckQueue, &ack, pdMS_TO_TICKS(50)) != pdTRUE) {
            Serial.printf("[ACK] ERROR: Could not queue clear acknowledgement for command_id=%lu\n",
                          (unsigned long)queuedCommand.commandId);
          } else {
            Serial.printf("[ACK] Clear command_id=%lu executed; acknowledgement queued.\n",
                          (unsigned long)queuedCommand.commandId);
          }
        }
      }
    }

    // Buzzer timeout is independent from the latched incident state.
    LockerSnapshot s = getSnapshot();
    if (s.alarmBuzzerOn && (millis() - buzzerStartedAt) >= BUZZER_DURATION_MS) {
      stopBuzzer();
      Serial.println("[BUZZER] Timed sound ended; incident remains latched until explicitly cleared.");
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---------------------------- NETWORK HELPERS --------------------------
String jsonEscape(const char* input) {
  String out;
  while (*input) {
    char c = *input++;
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\n";
    else if (c == '\r') out += "\\r";
    else if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}

bool postEvent(const EventMessage &ev) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  const String url = String(SERVER_BASE_URL) + "/api/event";
  if (!http.begin(httpClient, url)) return false;
  http.setTimeout(2500);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_API_KEY);
  String body = "{";
  body += "\"event_id\":" + String(ev.id) + ",";
  body += "\"type\":\"" + jsonEscape(ev.type) + "\",";
  body += "\"message\":\"" + jsonEscape(ev.message) + "\",";
  body += "\"locked\":" + String(ev.locked ? "true" : "false") + ",";
  body += "\"movement\":\"" + String(ev.movement) + "\",";
  body += "\"security\":\"" + String(ev.security) + "\",";
  body += "\"alarm\":" + String(ev.alarm ? "true" : "false") + ",";
  body += "\"ir\":\"" + String(ev.ir) + "\"}";
  int code = http.POST(body);
  String response = code > 0 ? http.getString() : "";
  http.end();
  Serial.printf("[NET] Event POST code=%d id=%lu\n", code, (unsigned long)ev.id);
  return code >= 200 && code < 300;
}

void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return;
  LockerSnapshot s = getSnapshot();
  HTTPClient http;
  if (!http.begin(httpClient, String(SERVER_BASE_URL) + "/api/heartbeat")) return;
  http.setTimeout(2000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_API_KEY);
  String body = "{";
  body += "\"locked\":" + String(s.locked ? "true" : "false") + ",";
  body += "\"movement\":\"" + String(movementName(s.movement)) + "\",";
  body += "\"security\":\"" + String(securityName(s.security)) + "\",";
  body += "\"alarm\":" + String(s.incidentLatched ? "true" : "false") + ",";
  body += "\"ir\":\"" + String(irName(s.ir)) + "\",";
  body += "\"sensor_fault\":" + String(s.accelerometerFault ? "true" : "false") + "}";
  int code = http.POST(body);
  Serial.printf("[NET] Heartbeat code=%d\n", code);
  http.end();
}

bool postClearCommandAck(const ClearAckMessage &ack) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  if (!http.begin(httpClient, String(SERVER_BASE_URL) + "/api/command-ack")) return false;
  http.setTimeout(2500);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_API_KEY);
  String body = "{\"command_id\":" + String(ack.commandId) +
                ",\"executed\":true,\"success\":" + String(ack.success ? "true" : "false") + "}";
  int code = http.POST(body);
  String response = code > 0 ? http.getString() : "";
  http.end();
  Serial.printf("[ACK] POST /api/command-ack code=%d command_id=%lu\n",
                code, (unsigned long)ack.commandId);
  if (code < 200 || code >= 300) {
    if (response.length()) Serial.printf("[ACK] Server response: %s\n", response.c_str());
    return false;
  }
  return true;
}

uint32_t parseCommandId(const String &payload) {
  int keyAt = payload.indexOf("\"COMMAND_ID\"");
  if (keyAt < 0) return 0;
  int colonAt = payload.indexOf(':', keyAt);
  if (colonAt < 0) return 0;
  int pos = colonAt + 1;
  while (pos < (int)payload.length() && (payload[pos] == ' ' || payload[pos] == '\t')) pos++;
  uint32_t id = 0;
  bool foundDigit = false;
  while (pos < (int)payload.length() && payload[pos] >= '0' && payload[pos] <= '9') {
    foundDigit = true;
    id = id * 10U + (uint32_t)(payload[pos] - '0');
    pos++;
  }
  return foundDigit ? id : 0;
}

void pollCommand() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  if (!http.begin(httpClient, String(SERVER_BASE_URL) + "/api/command")) return;
  http.setTimeout(2000);
  http.addHeader("X-Device-Key", DEVICE_API_KEY);
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    payload.toUpperCase();

    // Check CLEAR_ALARM first. Do not acknowledge it here: the sensor task
    // must execute clearLatchedAlarmLocal() before the ACK is sent.
    if (payload.indexOf("CLEAR_ALARM") >= 0) {
      uint32_t commandId = parseCommandId(payload);
      if (commandId == 0) {
        Serial.println("[NET] CLEAR_ALARM response has no valid command_id; not queueing it.");
      } else if (commandId != lastQueuedClearCommandId) {
        QueuedLockerCommand cmd = { CMD_CLEAR_ALARM, commandId, true };
        if (xQueueSend(commandQueue, &cmd, 0) == pdTRUE) {
          lastQueuedClearCommandId = commandId;
          Serial.printf("[NET] Queued remote CLEAR_ALARM command_id=%lu\n", (unsigned long)commandId);
        } else {
          Serial.printf("[NET] Command queue full; will retry CLEAR_ALARM command_id=%lu\n",
                        (unsigned long)commandId);
        }
      }
    } else if (payload.indexOf("UNLOCK") >= 0) {
      LockerSnapshot snapshot = getSnapshot();
      if (snapshot.locked) {
        QueuedLockerCommand cmd = { CMD_UNLOCK, 0, false };
        xQueueSend(commandQueue, &cmd, 0);
      }
    } else if (payload.indexOf("LOCK") >= 0) {
      LockerSnapshot snapshot = getSnapshot();
      if (!snapshot.locked) {
        QueuedLockerCommand cmd = { CMD_LOCK, 0, false };
        xQueueSend(commandQueue, &cmd, 0);
      }
    }
  } else {
    Serial.printf("[NET] Command poll code=%d\n", code);
  }
  http.end();
}

void networkTask(void* parameter) {
  (void)parameter;
  EventMessage pendingEvent;
  bool hasPendingEvent = false;
  ClearAckMessage pendingClearAck = {};
  bool hasPendingClearAck = false;
  uint32_t lastClearAckAttemptAt = 0;
  while (true) {
    const uint32_t now = millis();
    if (!hasPendingClearAck) {
      hasPendingClearAck = (xQueueReceive(clearAckQueue, &pendingClearAck, 0) == pdTRUE);
    }
    if (hasPendingClearAck && WiFi.status() == WL_CONNECTED &&
        (now - lastClearAckAttemptAt) >= EVENT_RETRY_MS) {
      lastClearAckAttemptAt = now;
      if (postClearCommandAck(pendingClearAck)) {
        Serial.printf("[ACK] Flask confirmed command_id=%lu.\n", (unsigned long)pendingClearAck.commandId);
        hasPendingClearAck = false;
      } else {
        Serial.printf("[ACK] Will retry command_id=%lu.\n", (unsigned long)pendingClearAck.commandId);
      }
    }
    if (WiFi.status() != WL_CONNECTED && (now - lastWiFiRetryAt) >= WIFI_RETRY_MS) {
      lastWiFiRetryAt = now;
      Serial.println("[WiFi] Reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    if (WiFi.status() == WL_CONNECTED) {
      if (now - lastCommandPollAt >= COMMAND_POLL_MS) {
        lastCommandPollAt = now;
        pollCommand();
      }
      if (now - lastHeartbeatAt >= HEARTBEAT_MS) {
        lastHeartbeatAt = now;
        sendHeartbeat();
      }
      if (!hasPendingEvent) hasPendingEvent = (xQueueReceive(eventQueue, &pendingEvent, 0) == pdTRUE);
      if (hasPendingEvent && (now - lastEventRetryAt) >= EVENT_RETRY_MS) {
        lastEventRetryAt = now;
        if (postEvent(pendingEvent)) hasPendingEvent = false;
      }
    } else {
      if (!hasPendingEvent) hasPendingEvent = (xQueueReceive(eventQueue, &pendingEvent, 0) == pdTRUE);
    }
    // Keep all servo/buzzer/security-state actions in sensorTask.
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ------------------------------- SETUP ---------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("====================================================");
  Serial.println(" Smart Locker - Tamper State Machine Stage 1");
  Serial.println(" Type CLEAR + Enter for local clear; dashboard clear is also supported.");
  Serial.println("====================================================");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(IR_PIN, INPUT);

  stateMutex = xSemaphoreCreateMutex();
  networkMutex = xSemaphoreCreateMutex();
  commandQueue = xQueueCreate(8, sizeof(QueuedLockerCommand));
  clearAckQueue = xQueueCreate(8, sizeof(ClearAckMessage));
  eventQueue = xQueueCreate(24, sizeof(EventMessage));
  if (!stateMutex || !commandQueue || !clearAckQueue || !eventQueue) {
    Serial.println("[FATAL] Could not create RTOS mutex/queues.");
    while (true) { digitalWrite(BUZZER_PIN, !digitalRead(BUZZER_PIN)); delay(250); }
  }

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (accelerometer.begin(0x53)) {
    sensorReady = true;
    accelerometer.setRange(ADXL345_RANGE_16_G);
    Serial.println("[SENSOR] ADXL345 found at 0x53, range set to +/-16g.");
  } else {
    sensorReady = false;
    Serial.println("[SENSOR] ERROR: ADXL345 not found at 0x53. Check SDA=21, SCL=22, power and ground.");
    state.accelerometerFault = true;
    state.security = SEC_SENSOR_FAULT;
  }

  lockServo.setPeriodHertz(50);
  lockServo.attach(SERVO_PIN, 500, 2400);
  lockServo.write(SERVO_LOCK_ANGLE); 
  state.security = SEC_REARMING;
  rearmPending = true;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WiFi] Connecting to '%s'", WIFI_SSID);
  uint32_t connectStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - connectStart < 12000) {
    Serial.print('.');
    delay(300);
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WiFi] Not connected yet; background retry will continue.");
  }

  // Start independent sensor/control and network tasks.
  xTaskCreatePinnedToCore(sensorTask, "SensorTask", 6144, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(networkTask, "NetworkTask", 8192, nullptr, 1, nullptr, 0);
  Serial.println("[SYSTEM] Ready. First boot starts locked and enters re-arming/stability check.");
  Serial.println("[ADXL] Live diagnostics print every 250 ms after each successful sensor read.");
}

void loop() {
  // Local diagnostic clear remains available. Serial Monitor: 115200 baud, Newline.
  while (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    command.toUpperCase();
    if (command == "CLEAR") {
      QueuedLockerCommand cmd = { CMD_CLEAR_ALARM, 0, false };
      if (commandQueue) xQueueSend(commandQueue, &cmd, 0);
    } else if (command.length() > 0) {
      Serial.println("[SERIAL] Supported local command: CLEAR");
    }
  }
  vTaskDelay(pdMS_TO_TICKS(10));
}
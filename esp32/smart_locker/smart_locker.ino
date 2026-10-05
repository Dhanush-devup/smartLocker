#include <Wire.h>
#include <ESP32Servo.h>
#include <math.h>
#include <WiFi.h>
#include <HTTPClient.h>

// =====================================================
// PINS
// =====================================================

#define SDA_PIN 21
#define SCL_PIN 22

#define SERVO_PIN 18
#define BUZZER_PIN 23

#define ADXL345_ADDR 0x53


// =====================================================
// SERVO
// =====================================================

Servo lockServo;

const int LOCK_ANGLE = 0;
const int UNLOCK_ANGLE = 90;


// =====================================================
// MOVEMENT DETECTION
// =====================================================

float movementThreshold = 50.0;
float stableThreshold = 30.0;

const unsigned long stableTime = 2000;

float baseline = 0;

bool locked = true;

bool waitingForStable = false;

unsigned long stableStartTime = 0;


// =====================================================
// WIFI
// =====================================================

const char* ssid = "Net ledha bro";
const char* password = "dhanush12";

const char* serverURL = "http://10.132.15.33:5000";


// =====================================================
// TIMERS
// =====================================================

unsigned long lastSensorRead = 0;
unsigned long lastCommandCheck = 0;
unsigned long lastHeartbeat = 0;

const unsigned long sensorInterval = 300;
const unsigned long commandInterval = 1000;
const unsigned long heartbeatInterval = 3000;


// =====================================================
// CURRENT STATES
// =====================================================

String movementState = "LOW";
String alarmState = "OFF";
String securityState = "SECURE";


// =====================================================
// ADXL345 RAW I2C
// =====================================================

void writeRegister(byte reg, byte value) {

  Wire.beginTransmission(ADXL345_ADDR);

  Wire.write(reg);
  Wire.write(value);

  Wire.endTransmission();
}


byte readRegister(byte reg) {

  Wire.beginTransmission(ADXL345_ADDR);

  Wire.write(reg);

  Wire.endTransmission(false);

  Wire.requestFrom(ADXL345_ADDR, 1);

  if (Wire.available()) {
    return Wire.read();
  }

  return 0;
}


void readAcceleration(float &x, float &y, float &z) {

  Wire.beginTransmission(ADXL345_ADDR);

  Wire.write(0x32);

  Wire.endTransmission(false);

  Wire.requestFrom(ADXL345_ADDR, 6);

  if (Wire.available() == 6) {

    int16_t rawX = Wire.read() | (Wire.read() << 8);
    int16_t rawY = Wire.read() | (Wire.read() << 8);
    int16_t rawZ = Wire.read() | (Wire.read() << 8);

    // ADXL345 full resolution ≈ 3.9 mg/LSB
    x = rawX * 0.0039;
    y = rawY * 0.0039;
    z = rawZ * 0.0039;

  } else {

    x = 0;
    y = 0;
    z = 0;

  }
}


float getMagnitude() {

  Wire.beginTransmission(ADXL345_ADDR);
  Wire.write(0x32);
  Wire.endTransmission(false);

  Wire.requestFrom(ADXL345_ADDR, 6);

  if (Wire.available() == 6) {

    int16_t rawX = Wire.read() | (Wire.read() << 8);
    int16_t rawY = Wire.read() | (Wire.read() << 8);
    int16_t rawZ = Wire.read() | (Wire.read() << 8);

    return sqrt(
      (float)rawX * rawX +
      (float)rawY * rawY +
      (float)rawZ * rawZ
    );
  }

  return 0;
}

// =====================================================
// SERVER COMMUNICATION
// =====================================================

void sendStateToServer(String eventType = "", String message = "") {

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  HTTPClient http;

  String url = String(serverURL) + "/api/event";

  http.begin(url);

  http.addHeader("Content-Type", "application/json");

  String json = "{";

  json += "\"locked\":" + String(locked ? "true" : "false");
  json += ",";
  json += "\"movement\":\"" + movementState + "\"";
  json += ",";
  json += "\"alarm\":\"" + alarmState + "\"";
  json += ",";
  json += "\"security\":\"" + securityState + "\"";

  if (eventType != "") {

    json += ",";
    json += "\"event\":\"" + eventType + "\"";

  }

  if (message != "") {

    json += ",";
    json += "\"message\":\"" + message + "\"";

  }

  json += "}";

  int httpCode = http.POST(json);

  Serial.print("Server update: ");

  if (httpCode > 0) {

    Serial.print(httpCode);

  } else {

    Serial.print("FAILED");

  }

  Serial.println();

  http.end();
}


// =====================================================
// LOCK
// =====================================================

void lockLocker(bool logEvent = true) {

  lockServo.write(LOCK_ANGLE);

  locked = true;

  waitingForStable = false;

  movementState = "LOW";

  alarmState = "OFF";

  securityState = "SECURE";

  Serial.println("LOCKED");

  if (logEvent) {

    sendStateToServer(
      "LOCK",
      "Locker locked"
    );

  } else {

    sendStateToServer();

  }
}


// =====================================================
// UNLOCK
// =====================================================

void unlockLocker() {

  lockServo.write(UNLOCK_ANGLE);

  locked = false;

  waitingForStable = false;

  alarmState = "OFF";

  movementState = "LOW";

  securityState = "UNLOCKED";

  Serial.println("UNLOCKED");

  sendStateToServer(
    "UNLOCK",
    "Locker unlocked"
  );
}


// =====================================================
// BUZZER
// =====================================================

void triggerAlarm() {

  alarmState = "ON";

  movementState = "TAMPERING";

  securityState = "TAMPERING";

  sendStateToServer(
    "TAMPER",
    "Tampering detected"
  );

  Serial.println("🚨 TAMPERING DETECTED");

  digitalWrite(BUZZER_PIN, HIGH);

  delay(1000);

  digitalWrite(BUZZER_PIN, LOW);

  alarmState = "OFF";

  movementState = "WAITING";

  securityState = "WAITING_FOR_STABILITY";

  waitingForStable = true;

  stableStartTime = 0;

  sendStateToServer();

  Serial.println("Waiting for stabilization...");
}


// =====================================================
// WIFI CONNECTION
// =====================================================

void connectToWiFi() {

  Serial.println();
  Serial.println("Connecting to WiFi...");

  WiFi.mode(WIFI_STA);

  WiFi.begin(ssid, password);

  int attempts = 0;

  while (
    WiFi.status() != WL_CONNECTED &&
    attempts < 30
  ) {

    delay(500);

    Serial.print(".");

    attempts++;

  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println("WiFi connected!");

    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());

  } else {

    Serial.println("WiFi connection failed.");

  }
}


// =====================================================
// CHECK SERVER COMMAND
// =====================================================

void checkServerCommand() {

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  HTTPClient http;

  String url = String(serverURL) + "/api/command";

  http.begin(url);

  int httpCode = http.GET();

  if (httpCode == 200) {

    String response = http.getString();

    Serial.print("Command: ");
    Serial.println(response);

    // IMPORTANT:
    // Check UNLOCK before LOCK because
    // "UNLOCK" contains the word "LOCK".

    if (response.indexOf("UNLOCK") >= 0) {

      if (locked) {

        Serial.println(
          "Remote command received: UNLOCK"
        );

        unlockLocker();

      }

    }

    else if (response.indexOf("LOCK") >= 0) {

      if (!locked) {

        Serial.println(
          "Remote command received: LOCK"
        );

        lockLocker();

      }

    }

  }

  http.end();
}


// =====================================================
// BASELINE
// =====================================================

void calculateBaseline() {

  Serial.println();
  Serial.println("Calculating movement baseline...");

  float total = 0;

  const int samples = 30;

  for (int i = 0; i < samples; i++) {

    float magnitude = getMagnitude();

    total += magnitude;

    Serial.print(".");

    delay(50);
  }

  baseline = total / samples;

  Serial.println();

  Serial.print("Baseline: ");
  Serial.println(baseline);
}


// =====================================================
// MOVEMENT HANDLING
// =====================================================

void handleMovement() {

  float magnitude = getMagnitude();

  float difference = fabs(
    magnitude - baseline
  );

  Serial.print("Magnitude: ");
  Serial.print(magnitude);

  Serial.print(" | Difference: ");
  Serial.println(difference);


  // ---------------------------------------------------
  // UNLOCKED
  // ---------------------------------------------------

  if (!locked) {

    movementState = "LOW";

    securityState = "UNLOCKED";

    return;
  }


  // ---------------------------------------------------
  // WAITING FOR STABILITY
  // ---------------------------------------------------

  if (waitingForStable) {

    movementState = "WAITING";

    securityState = "WAITING_FOR_STABILITY";

    // Movement still present
    if (difference >= stableThreshold) {

      stableStartTime = 0;

      return;
    }


    // Start stability timer
    if (stableStartTime == 0) {

      stableStartTime = millis();

      return;
    }


    // Stable long enough
    if (
      millis() - stableStartTime
      >= stableTime
    ) {

      waitingForStable = false;

      stableStartTime = 0;

      movementState = "LOW";

      securityState = "SECURE";

      alarmState = "OFF";

      Serial.println(
        "Locker stabilized."
      );

      sendStateToServer(
        "STABLE",
        "Locker stabilized"
      );

    }

    return;
  }


  // ---------------------------------------------------
  // NORMAL LOCKED STATE
  // ---------------------------------------------------

  movementState = "LOW";

  securityState = "SECURE";


  // ---------------------------------------------------
  // TAMPER DETECTION
  // ---------------------------------------------------

  if (difference >= movementThreshold) {

    triggerAlarm();

  }
}


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println("       SMART LOCKER START       ");
  Serial.println("================================");


  // -------------------------
  // I2C
  // -------------------------

  Wire.begin(
    SDA_PIN,
    SCL_PIN
  );


  // -------------------------
  // ADXL345
  // -------------------------

  byte deviceID = readRegister(0x00);

  Serial.print("ADXL345 Device ID: 0x");
  Serial.println(deviceID, HEX);

  if (deviceID == 0xE5) {

    Serial.println(
      "ADXL345 detected!"
    );

  } else {

    Serial.println(
      "WARNING: ADXL345 not detected!"
    );

  }


  // Measurement mode
  writeRegister(
    0x2D,
    0x08
  );


  // Full resolution ±16g
  writeRegister(
    0x31,
    0x0B
  );


  // -------------------------
  // BUZZER
  // -------------------------

  pinMode(
    BUZZER_PIN,
    OUTPUT
  );

  digitalWrite(
    BUZZER_PIN,
    LOW
  );


  // -------------------------
  // SERVO
  // -------------------------

  lockServo.attach(
    SERVO_PIN
  );

  lockServo.write(
    LOCK_ANGLE
  );

  locked = true;


  // -------------------------
  // WIFI
  // -------------------------

  connectToWiFi();


  // -------------------------
  // BASELINE
  // -------------------------

  calculateBaseline();


  // -------------------------
  // INITIAL STATE
  // -------------------------

  movementState = "LOW";

  alarmState = "OFF";

  securityState = "SECURE";


  // Tell backend system is alive
  sendStateToServer(
    "LOCK",
    "Locker locked"
  );


  Serial.println();
  Serial.println(
    "Smart Locker ready."
  );

}


// =====================================================
// LOOP
// =====================================================

void loop() {

  unsigned long now = millis();


  // ===================================================
  // WIFI RECONNECT
  // ===================================================

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    connectToWiFi();

  }


  // ===================================================
  // CHECK REMOTE COMMAND
  // ===================================================

  if (
    now - lastCommandCheck
    >= commandInterval
  ) {

    lastCommandCheck = now;

    checkServerCommand();

  }


  // ===================================================
  // MOVEMENT
  // ===================================================

  if (
    now - lastSensorRead
    >= sensorInterval
  ) {

    lastSensorRead = now;

    handleMovement();

  }


  // ===================================================
  // HEARTBEAT
  // ===================================================

  if (
    now - lastHeartbeat
    >= heartbeatInterval
  ) {

    lastHeartbeat = now;

    sendStateToServer();

  }

}
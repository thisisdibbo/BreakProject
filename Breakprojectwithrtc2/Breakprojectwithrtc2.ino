#include <Wire.h>
#include <VL53L0X.h>
#include <SPI.h>
#include <SD.h>
#include <RTClib.h>

// ================== PINS ==================
#define ENC_A 25
#define ENC_B 26
#define SD_CS 5

// ================== CONSTANTS ==================
#define PPR                20
#define WHEEL_RADIUS       0.21
#define TOF_THRESHOLD      120
#define LOOP_DT            200
#define TOF_DEBOUNCE_COUNT 5
#define WHEEL_STOP_SPEED   0.01
#define SOFT_BRAKE_TIME    5.0
#define FILTER_SIZE        5
#define STOP_TIMEOUT       5000

// ================== OBJECTS ==================
VL53L0X tof;
RTC_DS3231 rtc;

// ================== FILES ==================
File motionLog;
File brakeLog;

// ================== STATE ==================
enum WheelState { MOVING, BRAKING, STOPPED };
WheelState state = STOPPED;

// ================== VARIABLES ==================
volatile long pulseCount = 0;

unsigned long lastCalcTime = 0;
unsigned long lastFlushTime = 0;
unsigned long lastPulseTime = 0;

// motion
float rpm = 0;
float speed = 0;
float prevSpeed = 0;
float acceleration = 0;

float distanceStep = 0;
float totalDistance = 0;
float brakingDistance = 0;

// filter
float speedBuffer[FILTER_SIZE] = {0};
int filterIndex = 0;

// brake
bool brakeActive = false;
bool brakeLogged = false;

unsigned long brakeStartMillis = 0;
String brakeStartTime = "";
float brakeStartSpeed = 0;

int tofBelowCount = 0;
int baseDistance = 0;

// ================== ISR ==================
void IRAM_ATTR encoderISR() {
  pulseCount++;
  lastPulseTime = millis();
}

// ================== FILTER ==================
float smoothSpeed(float v) {
  speedBuffer[filterIndex] = v;
  filterIndex = (filterIndex + 1) % FILTER_SIZE;

  float sum = 0;
  for (int i = 0; i < FILTER_SIZE; i++) sum += speedBuffer[i];
  return sum / FILTER_SIZE;
}

// ================== TIME ==================
String timeStamp() {
  DateTime now = rtc.now();

  char buf[25];
  snprintf(buf, sizeof(buf),
           "%04d-%02d-%02d %02d:%02d:%02d",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute(), now.second());

  return String(buf);
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(21, 22);

  pinMode(ENC_A, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_A), encoderISR, RISING);

  // RTC
  rtc.begin();

  if (rtc.lostPower()) {
    Serial.println("RTC lost power");
  }

  // ================== TOF FIX ==================
  if (!tof.init()) {
    Serial.println("TOF INIT FAILED");
  } else {
    tof.setTimeout(50);
    tof.startContinuous();   // IMPORTANT FIX
    delay(100);

    baseDistance = tof.readRangeContinuousMillimeters();
    Serial.print("TOF base: ");
    Serial.println(baseDistance);
  }

  // SD
  if (!SD.begin(SD_CS)) {
    Serial.println("SD FAIL");
    return;
  }

  int s = 1;
  while (SD.exists("/S" + String(s))) s++;

  String folder = "/S" + String(s);
  SD.mkdir(folder);

  String motionFile = folder + "/motion.csv";
  String brakeFile  = folder + "/brake.csv";

  motionLog = SD.open(motionFile, FILE_WRITE);
  if (motionLog)
    motionLog.println("Time,RPM,Speed,Accel,Distance,TOF");

  brakeLog = SD.open(brakeFile, FILE_WRITE);
  if (brakeLog)
    brakeLog.println("Start,Stop,Time,StartSpeed,BrakeDist,Type,Decel");

  lastCalcTime = millis();
  lastPulseTime = millis();
  lastFlushTime = millis();

  Serial.println("SYSTEM READY");
}

// ================== BRAKE ==================
void updateBrake(int tofDist, float speed, unsigned long now) {

  // ---------- START ----------
  if (!brakeActive && !brakeLogged) {

    if (tofDist < TOF_THRESHOLD) {
      tofBelowCount++;

      if (tofBelowCount >= TOF_DEBOUNCE_COUNT) {

        brakeActive = true;
        brakingDistance = 0;

        brakeStartMillis = now;
        brakeStartTime = timeStamp();
        brakeStartSpeed = speed;

        tofBelowCount = 0;

        Serial.println("BRAKE START");
      }
    } else {
      tofBelowCount = 0;
    }
  }

  // ---------- TOF STOP ----------
  if (brakeActive && tofDist >= TOF_THRESHOLD) {

    float t = (now - brakeStartMillis) / 1000.0;

    if (brakeLog) {
      brakeLog.print(brakeStartTime); brakeLog.print(",");
      brakeLog.print(timeStamp()); brakeLog.print(",");
      brakeLog.print(t, 2); brakeLog.print(",");
      brakeLog.print(brakeStartSpeed, 3); brakeLog.print(",");
      brakeLog.print(brakingDistance, 3); brakeLog.print(",");
      brakeLog.print((t >= SOFT_BRAKE_TIME) ? "Soft" : "Hard"); brakeLog.print(",");
      brakeLog.println((t > 0) ? (speed - brakeStartSpeed) / t : 0);
    }

    Serial.println("BRAKE STOP (TOF)");

    brakeActive = false;
    brakeLogged = true;
  }

  // ---------- WHEEL STOP ----------
  bool wheelStopped = (millis() - lastPulseTime > STOP_TIMEOUT);

  if (brakeActive && wheelStopped) {

    float t = (now - brakeStartMillis) / 1000.0;

    if (brakeLog) {
      brakeLog.print(brakeStartTime); brakeLog.print(",");
      brakeLog.print(timeStamp()); brakeLog.print(",");
      brakeLog.print(t, 2); brakeLog.print(",");
      brakeLog.print(brakeStartSpeed, 3); brakeLog.print(",");
      brakeLog.print(brakingDistance, 3); brakeLog.print(",");
      brakeLog.print((t >= SOFT_BRAKE_TIME) ? "Soft" : "Hard"); brakeLog.print(",");
      brakeLog.println((t > 0) ? (speed - brakeStartSpeed) / t : 0);
    }

    Serial.println("BRAKE STOP (WHEEL)");

    brakeActive = false;
    brakeLogged = true;
  }

  if (!brakeActive && brakeLogged) {
    if (tofDist >= TOF_THRESHOLD) brakeLogged = false;
  }
}

// ================== LOOP ==================
void loop() {

  unsigned long now = millis();

  // ===== TOF READ FIXED =====
  int tofDist = tof.readRangeContinuousMillimeters();

  if (tof.timeoutOccurred()) {
    tofDist = baseDistance;
  }

  // ================== SPEED ==================
  if (now - lastCalcTime >= LOOP_DT) {

    noInterrupts();
    long pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    float dt = (now - lastCalcTime) / 1000.0;
    lastCalcTime = now;

    rpm = (pulses * 60.0) / (PPR * dt);

    float revs = pulses / (float)PPR;
    distanceStep = revs * 2 * 3.1415926 * WHEEL_RADIUS;

    if (pulses > 0) {
      speed = smoothSpeed(distanceStep / dt);
    } else {
      speed *= 0.9;
      if (speed < 0.02) speed = 0;
    }

    acceleration = (speed - prevSpeed) / dt;
    prevSpeed = speed;

    totalDistance += distanceStep;
    if (brakeActive) brakingDistance += distanceStep;

    if (motionLog) {
      motionLog.print(timeStamp()); motionLog.print(",");
      motionLog.print(rpm, 1); motionLog.print(",");
      motionLog.print(speed, 3); motionLog.print(",");
      motionLog.print(acceleration, 3); motionLog.print(",");
      motionLog.print(totalDistance, 4); motionLog.print(",");
      motionLog.println(tofDist);
    }

    Serial.print("Speed:");
    Serial.print(speed);
    Serial.print(" TOF:");
    Serial.print(tofDist);
    Serial.print(" STOP:");
   bool wheelStopped = (millis() - lastPulseTime > STOP_TIMEOUT);

Serial.println(wheelStopped);
  }

  updateBrake(tofDist, speed, now);

  // ================== FLUSH ==================
  if (millis() - lastFlushTime > 5000) {
    if (motionLog) motionLog.flush();
    if (brakeLog) brakeLog.flush();

    Serial.print("HEAP:");
    Serial.println(ESP.getFreeHeap());

    lastFlushTime = millis();
  }
}
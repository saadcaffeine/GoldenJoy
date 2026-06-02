#include <Arduino.h>

#ifndef GOLDENJOY_STATUS_LED_RGB
#define GOLDENJOY_STATUS_LED_RGB 1
#endif

#ifndef GOLDENJOY_STATUS_LED_ACTIVE_LOW
#define GOLDENJOY_STATUS_LED_ACTIVE_LOW 0
#endif

#ifndef GOLDENJOY_I2C_DIAGNOSTICS
#define GOLDENJOY_I2C_DIAGNOSTICS 0
#endif

#if GOLDENJOY_STATUS_LED_RGB
#include <Adafruit_NeoPixel.h>
#endif

#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Wire.h>

namespace {

constexpr char kDeviceName[] = "GoldenJoy BLE Mouse";
constexpr char kManufacturer[] = "GoldenJoy";

constexpr uint8_t kNunchuckAddress = 0x52;
constexpr int kSdaPin = 5;
constexpr int kSclPin = 4;
constexpr uint32_t kI2cClockHz = 400000;

constexpr int kStatusLedPin = 8;
#if GOLDENJOY_STATUS_LED_RGB
constexpr int kStatusLedCount = 1;
constexpr uint8_t kStatusLedBrightness = 24;
#endif

constexpr uint16_t kPollIntervalMs = 10;
constexpr uint16_t kCalibrationSamples = 80;
constexpr uint16_t kNunchuckRetryMs = 1000;
#if GOLDENJOY_I2C_DIAGNOSTICS
constexpr uint16_t kI2cDiagnosticIntervalMs = 5000;
#endif

constexpr int kDeadzone = 9;
constexpr float kPointerGain = 0.11f;
constexpr int kMaxStep = 12;
constexpr bool kInvertX = false;
constexpr bool kInvertY = true;

constexpr bool kZIsLeftClick = true;
constexpr bool kCIsRightClick = true;

NimBLEHIDDevice* hidDevice = nullptr;
NimBLECharacteristic* mouseInput = nullptr;
NimBLEServer* bleServer = nullptr;

#if GOLDENJOY_STATUS_LED_RGB
Adafruit_NeoPixel statusLed(kStatusLedCount, kStatusLedPin, NEO_GRB + NEO_KHZ800);
#endif

bool bleConnected = false;
bool nunchuckReady = false;
bool nunchuckReadFault = false;
bool neutralReportPending = false;
bool lastSyncedBleConnected = false;

int joyCenterX = 128;
int joyCenterY = 128;
uint32_t lastPollMs = 0;
uint32_t lastNunchuckRetryMs = 0;
#if GOLDENJOY_I2C_DIAGNOSTICS
uint32_t lastI2cDiagnosticMs = 0;
#endif
#if GOLDENJOY_STATUS_LED_RGB
uint32_t lastStatusColor = 0xFFFFFFFF;
#else
bool lastStatusLedOn = false;
#endif

const uint8_t kMouseReportMap[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (Button 1)
    0x29, 0x03,        //     Usage Maximum (Button 3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data, Variable, Absolute)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x03,        //     Input (Constant, Variable, Absolute)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x06,        //     Input (Data, Variable, Relative)
    0xC0,              //   End Collection
    0xC0               // End Collection
};

bool calibrateJoystick();

void setBleConnected(bool connected) {
  if (bleConnected == connected) {
    return;
  }

  bleConnected = connected;
  neutralReportPending = connected;
  Serial.println(connected ? "BLE central connected" : "BLE central disconnected");
}

void syncBleConnectionState() {
  if (bleServer == nullptr) {
    return;
  }

  const bool connected = bleServer->getConnectedCount() > 0;
  if (connected == lastSyncedBleConnected) {
    return;
  }

  setBleConnected(connected);
  lastSyncedBleConnected = connected;
}

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server) override {
    (void)server;
    setBleConnected(true);
    lastSyncedBleConnected = true;
  }

  void onDisconnect(NimBLEServer*) override {
    setBleConnected(false);
    lastSyncedBleConnected = false;
    NimBLEDevice::startAdvertising();
  }
};

struct NunchuckState {
  uint8_t joyX = 128;
  uint8_t joyY = 128;
  bool zPressed = false;
  bool cPressed = false;
};

void showStatusColor(uint8_t red, uint8_t green, uint8_t blue);

void beginStatusLed() {
#if GOLDENJOY_STATUS_LED_RGB
  statusLed.begin();
  statusLed.setBrightness(kStatusLedBrightness);
#else
  pinMode(kStatusLedPin, OUTPUT);
#endif
}

void showStatusLed(bool on) {
#if GOLDENJOY_STATUS_LED_RGB
  showStatusColor(0, 0, on ? 12 : 0);
#else
  if (on == lastStatusLedOn) {
    return;
  }
  digitalWrite(kStatusLedPin, on == GOLDENJOY_STATUS_LED_ACTIVE_LOW ? LOW : HIGH);
  lastStatusLedOn = on;
#endif
}

void showStatusColor(uint8_t red, uint8_t green, uint8_t blue) {
#if GOLDENJOY_STATUS_LED_RGB
  const uint32_t color = statusLed.Color(red, green, blue);
  if (color == lastStatusColor) {
    return;
  }

  statusLed.setPixelColor(0, color);
  statusLed.show();
  lastStatusColor = color;
#else
  showStatusLed(red > 0 || green > 0 || blue > 0);
#endif
}

void updateStatusLed() {
  const uint32_t now = millis();

#if GOLDENJOY_STATUS_LED_RGB
  if (!bleConnected) {
    const bool on = (now / 500) % 2 == 0;
    showStatusColor(0, 0, on ? 24 : 0);
    return;
  }

  if (!nunchuckReady) {
    const bool on = (now / 500) % 2 == 0;
    showStatusColor(on ? 24 : 0, on ? 18 : 0, 0);
    return;
  }

  if (nunchuckReadFault) {
    const bool on = (now / 250) % 2 == 0;
    showStatusColor(on ? 32 : 0, 0, 0);
    return;
  }

  showStatusColor(0, 24, 0);
#else
  if (!bleConnected) {
    showStatusLed((now / 500) % 2 == 0);
    return;
  }

  if (!nunchuckReady) {
    const uint16_t phase = now % 1000;
    showStatusLed(phase < 100 || (phase >= 200 && phase < 300));
    return;
  }

  if (nunchuckReadFault) {
    showStatusLed((now / 125) % 2 == 0);
    return;
  }

  showStatusLed(true);
#endif
}

bool writeNunchuckRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kNunchuckAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

#if GOLDENJOY_I2C_DIAGNOSTICS
bool scanI2cAddress(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission(true) == 0;
}

void runI2cDiagnostics() {
  static constexpr int kPinPairs[][2] = {
      {kSdaPin, kSclPin},
      {kSclPin, kSdaPin},
      {0, 1},
      {1, 0},
      {2, 3},
      {3, 2},
      {6, 7},
      {7, 6},
      {8, 9},
      {9, 8},
  };

  Serial.println("I2C diagnostic scan for Nunchuck address 0x52:");
  bool found = false;
  for (const auto& pair : kPinPairs) {
    Wire.end();
    delay(2);
    Wire.begin(pair[0], pair[1]);
    Wire.setClock(kI2cClockHz);
    delay(2);

    if (scanI2cAddress(kNunchuckAddress)) {
      Serial.printf("  Found 0x52 on SDA GPIO %d / SCL GPIO %d\n", pair[0], pair[1]);
      found = true;
    } else {
      Serial.printf("  No 0x52 on SDA GPIO %d / SCL GPIO %d\n", pair[0], pair[1]);
    }
  }

  Wire.end();
  delay(2);
  Wire.begin(kSdaPin, kSclPin);
  Wire.setClock(kI2cClockHz);

  if (!found) {
    Serial.println("  No Nunchuck detected on tested I2C pin pairs.");
  }
}
#endif

bool initNunchuck() {
  delay(100);
  const bool firstInit = writeNunchuckRegister(0xF0, 0x55);
  delay(10);
  const bool secondInit = writeNunchuckRegister(0xFB, 0x00);
  delay(10);
  return firstInit && secondInit;
}

bool markNunchuckReady() {
  Serial.println("Nunchuck detected");
  if (!calibrateJoystick()) {
    nunchuckReady = false;
    nunchuckReadFault = false;
    Serial.println("Nunchuck calibration failed. Will retry.");
    return false;
  }

  nunchuckReady = true;
  nunchuckReadFault = false;
  return true;
}

bool readNunchuck(NunchuckState& state) {
  Wire.beginTransmission(kNunchuckAddress);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  delayMicroseconds(250);
  if (Wire.requestFrom(kNunchuckAddress, static_cast<uint8_t>(6)) != 6) {
    return false;
  }

  uint8_t data[6];
  for (uint8_t& byte : data) {
    byte = Wire.read();
  }

  state.joyX = data[0];
  state.joyY = data[1];
  state.zPressed = (data[5] & 0x01) == 0;
  state.cPressed = (data[5] & 0x02) == 0;
  return true;
}

bool calibrateJoystick() {
  long xTotal = 0;
  long yTotal = 0;
  uint16_t samples = 0;
  uint16_t attempts = 0;
  constexpr uint16_t kMaxCalibrationAttempts = kCalibrationSamples * 4;

  Serial.println("Calibrating joystick center. Leave the Nunchuck at rest.");
  while (samples < kCalibrationSamples && attempts < kMaxCalibrationAttempts) {
    NunchuckState state;
    attempts++;
    if (readNunchuck(state)) {
      xTotal += state.joyX;
      yTotal += state.joyY;
      samples++;
    }
    delay(8);
  }

  if (samples < kCalibrationSamples) {
    return false;
  }

  joyCenterX = xTotal / kCalibrationSamples;
  joyCenterY = yTotal / kCalibrationSamples;
  Serial.printf("Joystick center: x=%d y=%d\n", joyCenterX, joyCenterY);
  return true;
}

void tryInitNunchuck() {
  if (initNunchuck()) {
    if (markNunchuckReady()) {
      return;
    }
    return;
  }

  nunchuckReady = false;
  nunchuckReadFault = false;
  Serial.println("Nunchuck init failed. Check 3V3, GND, SDA, and SCL wiring.");
}

int applyDeadzone(int value) {
  if (abs(value) <= kDeadzone) {
    return 0;
  }

  return value > 0 ? value - kDeadzone : value + kDeadzone;
}

int8_t axisToMouseDelta(int rawValue, bool invert) {
  int adjusted = applyDeadzone(rawValue);
  if (adjusted == 0) {
    return 0;
  }

  float scaled = adjusted * kPointerGain;
  if (invert) {
    scaled *= -1.0f;
  }

  int rounded = static_cast<int>(roundf(scaled));
  if (rounded == 0) {
    rounded = scaled > 0 ? 1 : -1;
  }

  return static_cast<int8_t>(constrain(rounded, -kMaxStep, kMaxStep));
}

void sendMouseReport(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel = 0) {
  if (!bleConnected || mouseInput == nullptr) {
    return;
  }

  uint8_t report[] = {
      buttons,
      static_cast<uint8_t>(dx),
      static_cast<uint8_t>(dy),
      static_cast<uint8_t>(wheel),
  };
  mouseInput->setValue(report, sizeof(report));
  mouseInput->notify();
}

void sendNeutralMouseReport() {
  sendMouseReport(0, 0, 0, 0);
  neutralReportPending = false;
}

void setupBleMouse() {
  NimBLEDevice::init(kDeviceName);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(true, false, true);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  hidDevice = new NimBLEHIDDevice(bleServer);
  mouseInput = hidDevice->inputReport(1);

  hidDevice->manufacturer()->setValue(kManufacturer);
  hidDevice->pnp(0x02, 0x303A, 0x4001, 0x0200);
  hidDevice->hidInfo(0x00, 0x01);
  hidDevice->reportMap(const_cast<uint8_t*>(kMouseReportMap), sizeof(kMouseReportMap));
  hidDevice->setBatteryLevel(100);
  hidDevice->startServices();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setAppearance(HID_MOUSE);
  advertising->addServiceUUID(hidDevice->hidService()->getUUID());
  advertising->setScanResponse(true);
  advertising->start();

  Serial.println("BLE HID mouse advertising started");
}

uint8_t buttonsFromState(const NunchuckState& state) {
  uint8_t buttons = 0;
  if (kZIsLeftClick && state.zPressed) {
    buttons |= 0x01;
  }
  if (kCIsRightClick && state.cPressed) {
    buttons |= 0x02;
  }
  return buttons;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);

  beginStatusLed();
  showStatusLed(true);

  Wire.begin(kSdaPin, kSclPin);
  Wire.setClock(kI2cClockHz);

  tryInitNunchuck();

  setupBleMouse();
}

void loop() {
  syncBleConnectionState();
  updateStatusLed();

  const uint32_t now = millis();
  if (!nunchuckReady) {
    if (now - lastNunchuckRetryMs >= kNunchuckRetryMs) {
      lastNunchuckRetryMs = now;
      tryInitNunchuck();
    }
#if GOLDENJOY_I2C_DIAGNOSTICS
    if (now - lastI2cDiagnosticMs >= kI2cDiagnosticIntervalMs) {
      lastI2cDiagnosticMs = now;
      runI2cDiagnostics();
    }
#endif
    delay(5);
    return;
  }

  if (neutralReportPending) {
    sendNeutralMouseReport();
  }

  if (now - lastPollMs < kPollIntervalMs) {
    delay(1);
    return;
  }
  lastPollMs = now;

  NunchuckState state;
  if (!readNunchuck(state)) {
    nunchuckReadFault = true;
    sendMouseReport(0, 0, 0, 0);
    delay(25);
    return;
  }
  nunchuckReadFault = false;

  const int rawX = static_cast<int>(state.joyX) - joyCenterX;
  const int rawY = static_cast<int>(state.joyY) - joyCenterY;
  const int8_t dx = axisToMouseDelta(rawX, kInvertX);
  const int8_t dy = axisToMouseDelta(rawY, kInvertY);
  const uint8_t buttons = buttonsFromState(state);

  sendMouseReport(buttons, dx, dy, 0);
}

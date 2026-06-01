#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Wire.h>

namespace {

constexpr char kDeviceName[] = "GoldenJoy Mouse";
constexpr char kManufacturer[] = "GoldenJoy";

constexpr uint8_t kNunchuckAddress = 0x52;
constexpr int kSdaPin = 8;
constexpr int kSclPin = 9;
constexpr uint32_t kI2cClockHz = 400000;

constexpr uint16_t kPollIntervalMs = 10;
constexpr uint16_t kCalibrationSamples = 80;

constexpr int kDeadzone = 9;
constexpr float kPointerGain = 0.11f;
constexpr int kMaxStep = 12;
constexpr bool kInvertX = false;
constexpr bool kInvertY = true;

constexpr bool kZIsLeftClick = true;
constexpr bool kCIsRightClick = true;

NimBLEHIDDevice* hidDevice = nullptr;
NimBLECharacteristic* mouseInput = nullptr;
bool bleConnected = false;

int joyCenterX = 128;
int joyCenterY = 128;
uint32_t lastPollMs = 0;

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

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server) override {
    (void)server;
    bleConnected = true;
    Serial.println("BLE central connected");
  }

  void onDisconnect(NimBLEServer*) override {
    bleConnected = false;
    Serial.println("BLE central disconnected");
    NimBLEDevice::startAdvertising();
  }
};

struct NunchuckState {
  uint8_t joyX = 128;
  uint8_t joyY = 128;
  bool zPressed = false;
  bool cPressed = false;
};

bool writeNunchuckRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kNunchuckAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool initNunchuck() {
  delay(100);
  const bool firstInit = writeNunchuckRegister(0xF0, 0x55);
  delay(10);
  const bool secondInit = writeNunchuckRegister(0xFB, 0x00);
  delay(10);
  return firstInit && secondInit;
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

void calibrateJoystick() {
  long xTotal = 0;
  long yTotal = 0;
  uint16_t samples = 0;

  Serial.println("Calibrating joystick center. Leave the Nunchuck at rest.");
  while (samples < kCalibrationSamples) {
    NunchuckState state;
    if (readNunchuck(state)) {
      xTotal += state.joyX;
      yTotal += state.joyY;
      samples++;
    }
    delay(8);
  }

  joyCenterX = xTotal / kCalibrationSamples;
  joyCenterY = yTotal / kCalibrationSamples;
  Serial.printf("Joystick center: x=%d y=%d\n", joyCenterX, joyCenterY);
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

void setupBleMouse() {
  NimBLEDevice::init(kDeviceName);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(true, true, true);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  hidDevice = new NimBLEHIDDevice(server);
  mouseInput = hidDevice->inputReport(1);

  hidDevice->manufacturer()->setValue(kManufacturer);
  hidDevice->pnp(0x02, 0x05AC, 0x820A, 0x0210);
  hidDevice->hidInfo(0x00, 0x01);
  hidDevice->reportMap(const_cast<uint8_t*>(kMouseReportMap), sizeof(kMouseReportMap));
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

  Wire.begin(kSdaPin, kSclPin);
  Wire.setClock(kI2cClockHz);

  if (!initNunchuck()) {
    Serial.println("Nunchuck init failed. Check 3V3, GND, SDA, and SCL wiring.");
  } else {
    Serial.println("Nunchuck detected");
    calibrateJoystick();
  }

  setupBleMouse();
}

void loop() {
  const uint32_t now = millis();
  if (now - lastPollMs < kPollIntervalMs) {
    delay(1);
    return;
  }
  lastPollMs = now;

  NunchuckState state;
  if (!readNunchuck(state)) {
    sendMouseReport(0, 0, 0, 0);
    delay(25);
    return;
  }

  const int rawX = static_cast<int>(state.joyX) - joyCenterX;
  const int rawY = static_cast<int>(state.joyY) - joyCenterY;
  const int8_t dx = axisToMouseDelta(rawX, kInvertX);
  const int8_t dy = axisToMouseDelta(rawY, kInvertY);
  const uint8_t buttons = buttonsFromState(state);

  sendMouseReport(buttons, dx, dy, 0);
}

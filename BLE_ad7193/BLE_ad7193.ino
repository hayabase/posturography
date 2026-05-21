#include <SPI.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled. Enable Bluetooth in the ESP32 board settings.
#endif

// ==== BLE settings ====
// BLE notifications use a compact 20-byte binary frame:
//   uint32 little-endian: sample_index, ch1, ch2, ch3, ch4
const char* BLE_NAME = "ESP32-AD7193-BLE";
const char* BLE_SERVICE_UUID = "7b7f0001-8f4c-4d52-a9f8-9c7d2b1f0001";
const char* BLE_DATA_CHAR_UUID = "7b7f0002-8f4c-4d52-a9f8-9c7d2b1f0001";

BLECharacteristic* dataCharacteristic = nullptr;
volatile bool bleClientConnected = false;
uint32_t sampleIndex = 0;

struct __attribute__((packed)) BleAd7193Frame {
  uint32_t sample_index;
  uint32_t ch1;
  uint32_t ch2;
  uint32_t ch3;
  uint32_t ch4;
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    bleClientConnected = true;
    Serial.println("[BLE] Client connected.");
  }

  void onDisconnect(BLEServer* server) override {
    (void)server;
    bleClientConnected = false;
    Serial.println("[BLE] Client disconnected. Restart advertising.");
    BLEDevice::startAdvertising();
  }
};

// ==== Pin assignment ====
const int PIN_MISO = 19;   // DOUT/RDY
const int PIN_MOSI = 23;   // DIN
const int PIN_SCLK = 18;   // SCLK
const int PIN_SYNC = 21;   // SYNC, kept HIGH

const int CS1 = 5;
const int CS2 = 17;
const int CS3 = 27;
const int CS4 = 25;

// ==== SPI settings ====
SPISettings ad7193SPISettings(1000000, MSBFIRST, SPI_MODE3);

// ==== AD7193 register addresses ====
const uint8_t REG_STATUS = 0b000;
const uint8_t REG_MODE   = 0b001;
const uint8_t REG_CONFIG = 0b010;
const uint8_t REG_DATA   = 0b011;
const uint8_t REG_ID     = 0b100;

uint8_t buildCommByte(bool isRead, uint8_t regAddr) {
  uint8_t comm = 0;
  if (isRead) {
    comm |= 0x40;
  }
  comm |= (regAddr & 0x07) << 3;
  return comm;
}

void ad7193Reset(int csPin) {
  digitalWrite(csPin, LOW);
  SPI.beginTransaction(ad7193SPISettings);
  for (int i = 0; i < 8; i++) {
    SPI.transfer(0xFF);
  }
  SPI.endTransaction();
  digitalWrite(csPin, HIGH);
  delay(5);
}

void ad7193WriteReg24(int csPin, uint8_t regAddr, uint32_t value) {
  digitalWrite(csPin, LOW);
  SPI.beginTransaction(ad7193SPISettings);

  SPI.transfer(buildCommByte(false, regAddr));
  SPI.transfer((value >> 16) & 0xFF);
  SPI.transfer((value >> 8) & 0xFF);
  SPI.transfer(value & 0xFF);

  SPI.endTransaction();
  digitalWrite(csPin, HIGH);
}

uint32_t ad7193ReadReg24(int csPin, uint8_t regAddr) {
  digitalWrite(csPin, LOW);
  SPI.beginTransaction(ad7193SPISettings);

  SPI.transfer(buildCommByte(true, regAddr));
  uint8_t b2 = SPI.transfer(0x00);
  uint8_t b1 = SPI.transfer(0x00);
  uint8_t b0 = SPI.transfer(0x00);

  SPI.endTransaction();
  digitalWrite(csPin, HIGH);

  return ((uint32_t)b2 << 16) | ((uint32_t)b1 << 8) | b0;
}

uint8_t ad7193ReadStatus(int csPin) {
  digitalWrite(csPin, LOW);
  SPI.beginTransaction(ad7193SPISettings);

  SPI.transfer(buildCommByte(true, REG_STATUS));
  uint8_t status = SPI.transfer(0x00);

  SPI.endTransaction();
  digitalWrite(csPin, HIGH);

  return status;
}

void ad7193WaitForReadyStatus(int csPin) {
  while (true) {
    uint8_t status = ad7193ReadStatus(csPin);
    if ((status & 0x80) == 0) {
      break;
    }
    delayMicroseconds(50);
  }
}

uint32_t ad7193ReadData(int csPin) {
  digitalWrite(csPin, LOW);
  SPI.beginTransaction(ad7193SPISettings);

  SPI.transfer(buildCommByte(true, REG_DATA));
  uint8_t b2 = SPI.transfer(0x00);
  uint8_t b1 = SPI.transfer(0x00);
  uint8_t b0 = SPI.transfer(0x00);

  SPI.endTransaction();
  digitalWrite(csPin, HIGH);

  return ((uint32_t)b2 << 16) | ((uint32_t)b1 << 8) | b0;
}

void ad7193SetMode_100Hz(int csPin) {
  uint32_t mode_before = ad7193ReadReg24(csPin, REG_MODE);

  uint32_t mode = 0;
  const uint16_t FS = 48;

  mode |= (0b000UL << 21);       // continuous conversion
  mode |= (0b10UL << 18);        // internal 4.92 MHz clock
  mode |= (uint32_t)(FS & 0x03FF);

  ad7193WriteReg24(csPin, REG_MODE, mode);

  uint32_t mode_after = ad7193ReadReg24(csPin, REG_MODE);
  Serial.print("AD7193 MODE (before) @CS");
  Serial.print(csPin);
  Serial.print(" = 0x");
  Serial.println(mode_before, HEX);
  Serial.print("AD7193 MODE (after ) @CS");
  Serial.print(csPin);
  Serial.print(" = 0x");
  Serial.println(mode_after, HEX);
}

void ad7193SetConfig(int csPin) {
  uint32_t config = 0;
  config |= (1UL << 8);  // CH0: AIN1-AIN2
  config |= (1UL << 4);  // BUF=1
  config |= 0x07;        // gain 128

  ad7193WriteReg24(csPin, REG_CONFIG, config);

  uint32_t config_after = ad7193ReadReg24(csPin, REG_CONFIG);
  Serial.print("AD7193 CONFIG @CS");
  Serial.print(csPin);
  Serial.print(" = 0x");
  Serial.println(config_after, HEX);
}

void ad7193InitOne(int csPin) {
  ad7193Reset(csPin);

  digitalWrite(csPin, LOW);
  SPI.beginTransaction(ad7193SPISettings);
  SPI.transfer(buildCommByte(true, REG_ID));
  uint8_t id = SPI.transfer(0x00);
  SPI.endTransaction();
  digitalWrite(csPin, HIGH);

  Serial.print("AD7193 ID @CS");
  Serial.print(csPin);
  Serial.print(" = 0x");
  Serial.println(id, HEX);

  ad7193SetMode_100Hz(csPin);
  ad7193SetConfig(csPin);

  ad7193WaitForReadyStatus(csPin);
  (void)ad7193ReadData(csPin);
}

void setupBle() {
  BLEDevice::init(BLE_NAME);

  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService* service = server->createService(BLE_SERVICE_UUID);

  dataCharacteristic = service->createCharacteristic(
    BLE_DATA_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ
  );
  dataCharacteristic->addDescriptor(new BLE2902());

  BleAd7193Frame emptyFrame = {0, 0, 0, 0, 0};
  dataCharacteristic->setValue((uint8_t*)&emptyFrame, sizeof(emptyFrame));

  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.print("[BLE] Advertising as ");
  Serial.println(BLE_NAME);
  Serial.print("[BLE] Service UUID: ");
  Serial.println(BLE_SERVICE_UUID);
  Serial.print("[BLE] Data characteristic UUID: ");
  Serial.println(BLE_DATA_CHAR_UUID);
}

void setupPins() {
  pinMode(PIN_MISO, INPUT);
  pinMode(PIN_MOSI, OUTPUT);
  pinMode(PIN_SCLK, OUTPUT);
  pinMode(PIN_SYNC, OUTPUT);
  digitalWrite(PIN_SYNC, HIGH);

  pinMode(CS1, OUTPUT);
  pinMode(CS2, OUTPUT);
  pinMode(CS3, OUTPUT);
  pinMode(CS4, OUTPUT);
  digitalWrite(CS1, HIGH);
  digitalWrite(CS2, HIGH);
  digitalWrite(CS3, HIGH);
  digitalWrite(CS4, HIGH);
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  setupBle();
  setupPins();

  SPI.begin(PIN_SCLK, PIN_MISO, PIN_MOSI);

  Serial.println("AD7193 x4 initialized (100 Hz target).");
  Serial.println("USB Serial output: ch1,ch2,ch3,ch4");
  Serial.println("BLE Notify output: 20-byte frame, little-endian uint32 x5.");

  ad7193InitOne(CS1);
  ad7193InitOne(CS2);
  ad7193InitOne(CS3);
  ad7193InitOne(CS4);
}

void loop() {
  ad7193WaitForReadyStatus(CS1);

  uint32_t ch1 = ad7193ReadData(CS1);
  uint32_t ch2 = ad7193ReadData(CS2);
  uint32_t ch3 = ad7193ReadData(CS3);
  uint32_t ch4 = ad7193ReadData(CS4);

  BleAd7193Frame frame = {
    sampleIndex++,
    ch1,
    ch2,
    ch3,
    ch4
  };

  Serial.print(frame.ch1);
  Serial.print(",");
  Serial.print(frame.ch2);
  Serial.print(",");
  Serial.print(frame.ch3);
  Serial.print(",");
  Serial.println(frame.ch4);

  if (bleClientConnected && dataCharacteristic != nullptr) {
    dataCharacteristic->setValue((uint8_t*)&frame, sizeof(frame));
    dataCharacteristic->notify();
  }
}

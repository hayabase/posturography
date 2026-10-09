#include <SPI.h>
#include <esp_timer.h>

/*
  AD7193 x4 / ESPr Developer 32 (ESP-WROOM-32) master firmware

  Hardware pinout: first four ADC connections from the original 6ch master.net:
    MISO : GPIO19
    MOSI : GPIO23
    SCK  : GPIO18
    SYNC : GPIO16   (common to all four AD7193)
    CS1  : GPIO22
    CS2  : GPIO17
    CS3  : GPIO21
    CS4  : GPIO27

  Slave board (Untitled.net):
    RJ45-1 SCK
    RJ45-2 GND
    RJ45-3 MOSI
    RJ45-4 GND
    RJ45-5 MISO (AD7193 DOUT/RDY via 47 ohm)
    RJ45-6 CS
    RJ45-7 3.3V
    RJ45-8 SYNC

    Load cell / ADC:
      REFIN1+ = 3.3V
      REFIN1- = GND (BLACK-)
      AIN1 = GREEN+ via 100 ohm
      AIN2 = WHITE- via 100 ohm

  ---------------------------------------------------------------------------
  USER SETTINGS

  Recommended production measurement:
      SAMPLE_RATE_HZ = 200
      OUTPUT_MODE    = OUTPUT_BINARY

  Easy serial-monitor debugging:
      SAMPLE_RATE_HZ = 100
      OUTPUT_MODE    = OUTPUT_CSV

  4 ch x 200 Hz CSV is intentionally disallowed at 115200 baud because worst-case
  CSV output exceeds the available serial bandwidth.

  ---------------------------------------------------------------------------
  IMPORTANT: ESP32 / AD7193 LAST-BIT WORKAROUND

  On this hardware, the final bit of an AD7193 SPI register read may be sampled
  as 1 because DOUT/RDY returns HIGH immediately after the final clock edge.

  Observed example:
      Expected ID     : 0xA2-like (low nibble = 0x2)
      Read ID         : 0xA3
      Expected MODE   : 0x80030
      Read MODE       : 0x80031

  Therefore bit0 is ignored when verifying ID / CONFIG / MODE.
  The DATA register bit0 is also cleared before use. This loses only 1 LSB.
  ---------------------------------------------------------------------------
*/

// ===== User-selectable settings =====
// 200Hzを選択する場合はOUTPUT_BINARYが通信レート的に安定．

// ここでサンプリングレート選択100 or 200.
#define SAMPLE_RATE_HZ 100

#define OUTPUT_CSV    0
#define OUTPUT_BINARY 1

// ここでモード選択.
#define OUTPUT_MODE   OUTPUT_CSV
// #define OUTPUT_MODE   OUTPUT_BINARY

static_assert(SAMPLE_RATE_HZ == 100 || SAMPLE_RATE_HZ == 200,
              "SAMPLE_RATE_HZ must be 100 or 200");
static_assert(!(OUTPUT_MODE == OUTPUT_CSV && SAMPLE_RATE_HZ > 100),
              "At 115200 baud, use CSV at 100 Hz or binary at 200 Hz");

// ===== Serial / SPI =====
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t SPI_CLOCK_HZ = 1000000;
SPISettings ad7193SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE3);

// ===== ESP32 pins from master.net =====
constexpr uint8_t PIN_MISO = 19;
constexpr uint8_t PIN_MOSI = 23;
constexpr uint8_t PIN_SCLK = 18;
constexpr uint8_t PIN_SYNC = 16;

constexpr uint8_t NUM_ADC = 4;
constexpr uint8_t ALL_ADC_MASK = (1U << NUM_ADC) - 1U;

// ESPr Developer 32 (ESP-WROOM-32)
// CS2 remains on GPIO17 as wired on the existing PCB.
constexpr uint8_t CS_PINS[NUM_ADC] = {22, 17, 21, 27};

// ===== AD7193 registers =====
constexpr uint8_t REG_STATUS = 0b000;
constexpr uint8_t REG_MODE   = 0b001;
constexpr uint8_t REG_CONFIG = 0b010;
constexpr uint8_t REG_DATA   = 0b011;
constexpr uint8_t REG_ID     = 0b100;

// Internal 4.92 MHz clock, chop off, sinc4, no averaging, continuous mode.
// fADC = 4.92 MHz / (1024 * FS)
constexpr uint16_t FS_WORD = (SAMPLE_RATE_HZ == 200) ? 24 : 48;

// AIN1-AIN2, buffer ON, bipolar, gain 128.
constexpr uint32_t CONFIG_VALUE =
    (1UL << 8) |   // CH0 = AIN1-AIN2
    (1UL << 4) |   // BUF = 1
    0x07UL;        // G2:G0 = 111 -> gain 128

constexpr uint32_t MODE_VALUE =
    (0b000UL << 21) |  // continuous conversion
    (0b10UL  << 18) |  // internal 4.92 MHz clock, MCLK2 tristated
    (uint32_t)(FS_WORD & 0x03FF);

// Ignore the final SPI bit when checking registers.
constexpr uint8_t  VERIFY_MASK_8  = 0xFEU;
constexpr uint32_t VERIFY_MASK_24 = 0x00FFFFFEUL;

// ===== ADC state =====
uint8_t adcInitMask = 0;
uint32_t latestRaw[NUM_ADC] = {0};
uint64_t latestTimeUs[NUM_ADC] = {0};
uint8_t csvFreshMask = 0;

// ---------------------------------------------------------------------------
// AD7193 low-level SPI
// ---------------------------------------------------------------------------
uint8_t buildCommByte(bool isRead, uint8_t regAddr) {
  uint8_t comm = (uint8_t)((regAddr & 0x07U) << 3);
  if (isRead) comm |= 0x40U;
  return comm;
}

void selectAdc(uint8_t csPin) {
  digitalWrite(csPin, LOW);
}

void deselectAdc(uint8_t csPin) {
  digitalWrite(csPin, HIGH);
}

void ad7193Reset(uint8_t csPin) {
  selectAdc(csPin);
  SPI.beginTransaction(ad7193SPISettings);

  // >= 40 consecutive 1s required. 8 x 0xFF = 64 ones.
  for (uint8_t i = 0; i < 8; ++i) {
    SPI.transfer(0xFF);
  }

  SPI.endTransaction();
  deselectAdc(csPin);

  // Datasheet minimum after reset is 500 us; use 2 ms margin.
  delay(2);
}

uint8_t ad7193ReadReg8(uint8_t csPin, uint8_t regAddr) {
  selectAdc(csPin);
  SPI.beginTransaction(ad7193SPISettings);

  SPI.transfer(buildCommByte(true, regAddr));
  uint8_t value = SPI.transfer(0x00);

  SPI.endTransaction();
  deselectAdc(csPin);
  return value;
}

uint32_t ad7193ReadReg24(uint8_t csPin, uint8_t regAddr) {
  selectAdc(csPin);
  SPI.beginTransaction(ad7193SPISettings);

  SPI.transfer(buildCommByte(true, regAddr));
  uint32_t value = ((uint32_t)SPI.transfer(0x00) << 16);
  value |= ((uint32_t)SPI.transfer(0x00) << 8);
  value |= (uint32_t)SPI.transfer(0x00);

  SPI.endTransaction();
  deselectAdc(csPin);
  return value & 0x00FFFFFFUL;
}

void ad7193WriteReg24(uint8_t csPin, uint8_t regAddr, uint32_t value) {
  selectAdc(csPin);
  SPI.beginTransaction(ad7193SPISettings);

  SPI.transfer(buildCommByte(false, regAddr));
  SPI.transfer((uint8_t)((value >> 16) & 0xFF));
  SPI.transfer((uint8_t)((value >> 8) & 0xFF));
  SPI.transfer((uint8_t)(value & 0xFF));

  SPI.endTransaction();
  deselectAdc(csPin);
}

uint32_t ad7193ReadData(uint8_t csPin) {
  const uint32_t raw = ad7193ReadReg24(csPin, REG_DATA);

  // ESP32/AD7193 final-bit workaround:
  // bit0 may be sampled as HIGH after DOUT/RDY returns HIGH.
  // Clear that unreliable LSB. Effective loss is only one bit.
  return raw & VERIFY_MASK_24;
}

/*
  DOUT/RDY is shared on GPIO19.
  Only one CS is asserted at a time, so that ADC alone drives the shared MISO.
  RDY = LOW means a new conversion is available.
*/
bool ad7193IsReady(uint8_t csPin) {
  selectAdc(csPin);
  delayMicroseconds(1);  // allow DOUT/RDY to settle after CS assertion
  bool ready = (digitalRead(PIN_MISO) == LOW);
  deselectAdc(csPin);
  return ready;
}

bool ad7193InitOne(uint8_t index) {
  const uint8_t csPin = CS_PINS[index];

  ad7193Reset(csPin);

  const uint8_t id = ad7193ReadReg8(csPin, REG_ID);

  // AD7193 ID low nibble should be 0x2.
  // Ignore bit0 because it may falsely read as 1 on the final SPI bit.
  // Thus both x2 and x3 are accepted here, while bits 3:1 must match 0x2.
  const bool idOk = ((id & 0x0EU) == 0x02U);

  // Configure analog channel first, then write MODE last to restart conversion
  // using the final setup.
  ad7193WriteReg24(csPin, REG_CONFIG, CONFIG_VALUE);
  ad7193WriteReg24(csPin, REG_MODE, MODE_VALUE);

  const uint32_t configRead = ad7193ReadReg24(csPin, REG_CONFIG);
  const uint32_t modeRead   = ad7193ReadReg24(csPin, REG_MODE);

  // Ignore only bit0 when comparing register readback.
  const bool configOk =
      ((configRead & VERIFY_MASK_24) ==
       (CONFIG_VALUE & VERIFY_MASK_24));

  const bool modeOk =
      ((modeRead & VERIFY_MASK_24) ==
       (MODE_VALUE & VERIFY_MASK_24));

#if OUTPUT_MODE == OUTPUT_CSV
  Serial.print("# ADC");
  Serial.print(index + 1);
  Serial.print(" CS=");
  Serial.print(csPin);
  Serial.print(" ID=0x");
  Serial.print(id, HEX);
  Serial.print(" CONFIG=0x");
  Serial.print(configRead, HEX);
  Serial.print(" MODE=0x");
  Serial.print(modeRead, HEX);
  Serial.print(" -> ");
  Serial.println((idOk && configOk && modeOk) ? "OK" : "ERROR");
#endif

  return idOk && configOk && modeOk;
}

// Common SYNC aligns the digital filter/modulator start point of all four ADCs.
// It does NOT remove long-term drift between their independent internal clocks.
void ad7193SyncAll() {
  digitalWrite(PIN_SYNC, LOW);
  delayMicroseconds(5);  // > 4 master-clock cycles at ~4.92 MHz
  digitalWrite(PIN_SYNC, HIGH);
}

// ---------------------------------------------------------------------------
// CSV output
// ---------------------------------------------------------------------------
// Debug mode only: one row after every ADC has supplied at least one new value.
// The four values are unsigned AD7193 24-bit offset-binary raw codes.
void sendCsvFrame() {
  for (uint8_t i = 0; i < NUM_ADC; ++i) {
    if (i != 0) Serial.print(',');
    Serial.print(latestRaw[i]);
  }
  Serial.println();
}

// ---------------------------------------------------------------------------
// Binary output
// ---------------------------------------------------------------------------
/*
  Binary packet format, little-endian unless noted:

    Byte 0       0xA5
    Byte 1       0x5A
    Byte 2       packet type = 0x01
    Byte 3       nominal sample rate (100 or 200)
    Byte 4       ADC init mask, bit0=ADC1 ... bit3=ADC4
    Byte 5..6    packet sequence uint16
    Byte 7..14   base timestamp uint64, microseconds from esp_timer_get_time()
    Byte 15      number of events N

    Then N events, 6 bytes each:
      +0         channel index 0..3
      +1..2      delta time from base timestamp, uint16 microseconds
      +3..5      AD7193 raw 24-bit value, little-endian

    Final 2 bytes:
      CRC-16/CCITT-FALSE over bytes 2 through the last event byte.
      CRC stored little-endian.

  This event-based format preserves each AD7193's actual RDY timing, which is
  useful because the four ADCs use separate internal oscillators.
*/

struct AdcEvent {
  uint8_t channel;
  uint64_t timeUs;
  uint32_t raw24;
};

constexpr uint8_t MAX_EVENTS = 24;
constexpr uint32_t BINARY_FLUSH_US = 10000;  // ~10 ms packet latency
AdcEvent eventBuffer[MAX_EVENTS];
uint8_t eventCount = 0;
uint16_t binaryPacketSequence = 0;

uint16_t crc16CcittFalse(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      if (crc & 0x8000) {
        crc = (uint16_t)((crc << 1) ^ 0x1021);
      } else {
        crc <<= 1;
      }
    }
  }
  return crc;
}

void putU16LE(uint8_t* p, size_t& pos, uint16_t v) {
  p[pos++] = (uint8_t)(v & 0xFF);
  p[pos++] = (uint8_t)((v >> 8) & 0xFF);
}

void putU64LE(uint8_t* p, size_t& pos, uint64_t v) {
  for (uint8_t i = 0; i < 8; ++i) {
    p[pos++] = (uint8_t)((v >> (8U * i)) & 0xFFU);
  }
}

void flushBinaryPacket() {
  if (eventCount == 0) return;

  // 16-byte prefix through event count + 24*6 events + 2-byte CRC = 162 B max.
  uint8_t packet[170];
  size_t pos = 0;

  packet[pos++] = 0xA5;
  packet[pos++] = 0x5A;
  packet[pos++] = 0x01;  // packet type: ADC events
  packet[pos++] = (uint8_t)SAMPLE_RATE_HZ;
  packet[pos++] = adcInitMask;
  putU16LE(packet, pos, binaryPacketSequence++);

  const uint64_t baseTimeUs = eventBuffer[0].timeUs;
  putU64LE(packet, pos, baseTimeUs);
  packet[pos++] = eventCount;

  for (uint8_t i = 0; i < eventCount; ++i) {
    packet[pos++] = eventBuffer[i].channel;

    uint64_t delta64 = eventBuffer[i].timeUs - baseTimeUs;
    if (delta64 > 65535ULL) delta64 = 65535ULL;
    putU16LE(packet, pos, (uint16_t)delta64);

    const uint32_t raw = eventBuffer[i].raw24 & 0x00FFFFFFUL;
    packet[pos++] = (uint8_t)(raw & 0xFF);
    packet[pos++] = (uint8_t)((raw >> 8) & 0xFF);
    packet[pos++] = (uint8_t)((raw >> 16) & 0xFF);
  }

  const uint16_t crc = crc16CcittFalse(packet + 2, pos - 2);
  putU16LE(packet, pos, crc);

  // A large TX buffer is configured in setup(), so normal operation should
  // enqueue this packet without stalling ADC acquisition.
  Serial.write(packet, pos);
  eventCount = 0;
}

void addBinaryEvent(uint8_t channel, uint64_t timeUs, uint32_t raw24) {
  if (eventCount > 0) {
    const uint64_t age = timeUs - eventBuffer[0].timeUs;
    if (age > 60000ULL) {
      flushBinaryPacket();
    }
  }

  if (eventCount >= MAX_EVENTS) {
    flushBinaryPacket();
  }

  eventBuffer[eventCount].channel = channel;
  eventBuffer[eventCount].timeUs = timeUs;
  eventBuffer[eventCount].raw24 = raw24 & 0x00FFFFFFUL;
  ++eventCount;
}

// ---------------------------------------------------------------------------
// Arduino setup / loop
// ---------------------------------------------------------------------------
void setup() {
  // Arduino-ESP32 default TX buffer is 0. A real TX buffer prevents a burst of
  // Serial.write() from blocking the sampling loop at 115200 baud.
  Serial.setTxBufferSize(4096);
  Serial.begin(SERIAL_BAUD);

#if OUTPUT_MODE == OUTPUT_CSV
  delay(500);
  Serial.println("# AD7193 x4 load-cell acquisition");
  Serial.print("# nominal sample rate = ");
  Serial.print(SAMPLE_RATE_HZ);
  Serial.println(" Hz");
  Serial.println("# CSV = raw1,raw2,raw3,raw4");
  Serial.println("# NOTE: SPI read bit0 is ignored/cleared as a workaround");
#endif

  pinMode(PIN_MISO, INPUT);
  pinMode(PIN_MOSI, OUTPUT);
  pinMode(PIN_SCLK, OUTPUT);
  pinMode(PIN_SYNC, OUTPUT);
  digitalWrite(PIN_SYNC, HIGH);

  for (uint8_t i = 0; i < NUM_ADC; ++i) {
    pinMode(CS_PINS[i], OUTPUT);
    digitalWrite(CS_PINS[i], HIGH);
  }

  SPI.begin(PIN_SCLK, PIN_MISO, PIN_MOSI);

  adcInitMask = 0;
  for (uint8_t i = 0; i < NUM_ADC; ++i) {
    if (ad7193InitOne(i)) {
      adcInitMask |= (uint8_t)(1U << i);
    }
  }

  // Start all configured ADCs from the same SYNC edge.
  ad7193SyncAll();
  csvFreshMask = 0;

#if OUTPUT_MODE == OUTPUT_CSV
  Serial.print("# init mask = 0x");
  Serial.println(adcInitMask, HEX);
  if (adcInitMask != ALL_ADC_MASK) {
    Serial.println("# WARNING: one or more AD7193 devices failed initialization");
  }
#endif
}

void loop() {
  // Poll each ADC's DOUT/RDY through its own CS while sharing one MISO line.
  // When ready, read immediately so a subsequent conversion cannot overwrite it.
  for (uint8_t i = 0; i < NUM_ADC; ++i) {
    const uint8_t bit = (uint8_t)(1U << i);
    if ((adcInitMask & bit) == 0) continue;

    if (ad7193IsReady(CS_PINS[i])) {
      const uint64_t tUs = (uint64_t)esp_timer_get_time();
      const uint32_t raw = ad7193ReadData(CS_PINS[i]);

      latestRaw[i] = raw;
      latestTimeUs[i] = tUs;

#if OUTPUT_MODE == OUTPUT_BINARY
      addBinaryEvent(i, tUs, raw);
#else
      csvFreshMask |= bit;
#endif
    }
  }

#if OUTPUT_MODE == OUTPUT_BINARY
  if (eventCount > 0) {
    const uint64_t nowUs = (uint64_t)esp_timer_get_time();
    if ((nowUs - eventBuffer[0].timeUs) >= BINARY_FLUSH_US) {
      flushBinaryPacket();
    }
  }
#else
  // Output one CSV frame after all four ADCs have produced a fresh sample.
  // If an ADC failed init, still require all four to avoid silently presenting
  // incomplete data as a valid 4-channel frame.
  if (adcInitMask == ALL_ADC_MASK && csvFreshMask == ALL_ADC_MASK) {
    sendCsvFrame();
    csvFreshMask = 0;
  }
#endif
}

#include <SPI.h>
#include <esp_timer.h>
#include <driver/uart.h>

/* AD7193 x4, ESP32, timestamped CSV at FIXED 115200 baud.
 * Columns: timestamp_us,raw1,raw2,raw3,raw4 (decimal, LF terminated).
 * timestamp_us: uint64 ESP32 uptime when all four fresh values are assembled;
 * it is not a simultaneous ADC conversion timestamp. Raw is offset-binary.
 * Independent ADC clocks: faster ADC values may supersede earlier values;
 * this is explicitly counted in #STATS, not silently called lossless sampling.
 * New data is checked using STATUS.RDY (bit7), not digitalRead(SPI MISO).
 * Commands: FS=48\n (1..1023), STATS\n, WIDE=0/1\n (bandwidth stress padding). FS change resets statistics.
 * # prefixed metadata/statistics are not sample rows.
 * A full UART TX queue drops a whole frame and increments tx_drop instead
 * of blocking acquisition. Passing a rate test requires ZERO tx_drop.
 * Wiring: MISO19 MOSI23 SCK18 SYNC16 CS={22,17,21,27}.
 * Original hardware's bit0 SPI-read workaround is retained.
 */
#ifndef DEFAULT_FS
#define DEFAULT_FS 24
#endif
static_assert(DEFAULT_FS >= 1 && DEFAULT_FS <= 1023, "FS must be 1..1023");
constexpr uint32_t SERIAL_BAUD = 115200; // DO NOT CHANGE
constexpr uint32_t SPI_CLOCK_HZ = 1000000;
SPISettings ad7193SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE3);
constexpr uint8_t PIN_MISO=19, PIN_MOSI=23, PIN_SCLK=18, PIN_SYNC=16;
constexpr uint8_t NUM_ADC=4, ALL_ADC_MASK=0x0F;
constexpr uint8_t CS_PINS[NUM_ADC]={22,17,21,27};
constexpr uint8_t REG_STATUS=0, REG_MODE=1, REG_CONFIG=2, REG_DATA=3, REG_ID=4;
constexpr uint32_t CONFIG_VALUE=(1UL<<8)|(1UL<<4)|7UL;
constexpr uint32_t VERIFY_MASK_24=0x00FFFFFEUL;
uint16_t fsWord=DEFAULT_FS;
uint8_t adcInitMask=0, freshMask=0;
uint32_t latestRaw[NUM_ADC]={0};
uint32_t readCount[NUM_ADC]={0}, superseded[NUM_ADC]={0}, adcErrors[NUM_ADC]={0};
uint32_t frames=0, sentFrames=0, txDrop=0, statSkip=0;
uint64_t lastStatsUs=0, lastLoopUs=0, maxLoopGapUs=0;
char command[32];
size_t commandLength=0;
bool commandOverflow=false;
bool wideCsv=false; // WIDE=1: zero-pad real values to worst-case 57-byte rows
// Own bounded queue + uart_tx_chars (FIFO-only, nonblocking). The Arduino
// ring-buffer availableForWrite() check did not prevent write stalls in v3.0.7.
constexpr size_t TX_CAPACITY=4096, TX_DIAGNOSTIC_RESERVE=600;
uint8_t txQueue[TX_CAPACITY];
size_t txHead=0, txTail=0, txCount=0;

void pumpTx() {
  if(!txCount) return;
  size_t contiguous=TX_CAPACITY-txHead;
  if(contiguous>txCount) contiguous=txCount;
  int written=uart_tx_chars(UART_NUM_0,(const char*)(txQueue+txHead),contiguous);
  if(written>0) {
    txHead=(txHead+written)%TX_CAPACITY;
    txCount-=written;
  }
}

bool enqueueTx(const char* data,size_t n,bool sample) {
  size_t reserve=sample ? TX_DIAGNOSTIC_RESERVE : 0;
  if(n+reserve>TX_CAPACITY-txCount) return false;
  for(size_t i=0;i<n;++i) {
    txQueue[txTail]=(uint8_t)data[i];
    txTail=(txTail+1)%TX_CAPACITY;
  }
  txCount+=n;
  return true;
}

void drainTx() {
  while(txCount) { pumpTx(); delayMicroseconds(20); }
  Serial.flush();
}

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


uint32_t modeValue() { return (0b10UL<<18) | fsWord; }

void syncAll() {
  digitalWrite(PIN_SYNC,LOW);
  delayMicroseconds(5);
  digitalWrite(PIN_SYNC,HIGH);
}

bool initOne(uint8_t index) {
  uint8_t cs=CS_PINS[index];
  ad7193Reset(cs);
  uint8_t id=ad7193ReadReg8(cs,REG_ID);
  ad7193WriteReg24(cs,REG_CONFIG,CONFIG_VALUE);
  ad7193WriteReg24(cs,REG_MODE,modeValue());
  uint32_t config=ad7193ReadReg24(cs,REG_CONFIG);
  uint32_t mode=ad7193ReadReg24(cs,REG_MODE);
  bool ok=(id&0x0E)==0x02 &&
    (config&VERIFY_MASK_24)==(CONFIG_VALUE&VERIFY_MASK_24) &&
    (mode&VERIFY_MASK_24)==(modeValue()&VERIFY_MASK_24);
  Serial.printf("#ADC index=%u id=0x%02X config=0x%06lX mode=0x%06lX ok=%u\n",
    index+1,id,(unsigned long)config,(unsigned long)mode,ok);
  return ok;
}

void resetCounters() {
  freshMask=0;
  frames=sentFrames=txDrop=statSkip=0;
  for(uint8_t i=0;i<NUM_ADC;++i) readCount[i]=superseded[i]=adcErrors[i]=0;
  lastStatsUs=esp_timer_get_time();
  lastLoopUs=0; maxLoopGapUs=0;
}

void printConfig() {
  Serial.printf("#CONFIG fs=%u nominal_hz=%.6f baud=%lu init_mask=%u wide=%u columns=timestamp_us,raw1,raw2,raw3,raw4\n",
    fsWord,4800.0/fsWord,(unsigned long)SERIAL_BAUD,adcInitMask,wideCsv);
}

void sendStats() {
  char text[600];
  int n=snprintf(text,sizeof(text),
    "#STATS {\"fs\":%u,\"t_us\":%llu,\"frames\":%lu,\"sent\":%lu,\"tx_drop\":%lu,\"stat_skip\":%lu,"
    "\"reads\":[%lu,%lu,%lu,%lu],\"superseded\":[%lu,%lu,%lu,%lu],\"adc_errors\":[%lu,%lu,%lu,%lu],\"max_loop_gap_us\":%llu}\n",
    fsWord,(unsigned long long)esp_timer_get_time(),(unsigned long)frames,(unsigned long)sentFrames,(unsigned long)txDrop,(unsigned long)statSkip,
    (unsigned long)readCount[0],(unsigned long)readCount[1],(unsigned long)readCount[2],(unsigned long)readCount[3],
    (unsigned long)superseded[0],(unsigned long)superseded[1],(unsigned long)superseded[2],(unsigned long)superseded[3],
    (unsigned long)adcErrors[0],(unsigned long)adcErrors[1],(unsigned long)adcErrors[2],(unsigned long)adcErrors[3],
    (unsigned long long)maxLoopGapUs);
  if(!(n>0 && n<(int)sizeof(text) && enqueueTx(text,n,false))) ++statSkip;
}

void changeFs(uint16_t value) {
  drainTx();
  fsWord=value;
  adcInitMask=0;
  for(uint8_t i=0;i<NUM_ADC;++i) {
    ad7193WriteReg24(CS_PINS[i],REG_MODE,modeValue());
    uint32_t readback=ad7193ReadReg24(CS_PINS[i],REG_MODE);
    if((readback&VERIFY_MASK_24)==(modeValue()&VERIFY_MASK_24)) adcInitMask|=(1U<<i);
  }
  syncAll();
  resetCounters();
  printConfig();
}

void handleCommands() {
  while(Serial.available()) {
    char c=Serial.read();
    if(c=='\r') continue;
    if(c=='\n') {
      command[commandLength]='\0';
      if(!commandOverflow && strncmp(command,"FS=",3)==0) {
        char* end=nullptr;
        long value=strtol(command+3,&end,10);
        if(end!=command+3 && *end=='\0' && value>=1 && value<=1023) changeFs(value);
        else Serial.println("#ERROR invalid FS");
      } else if(!commandOverflow && (strcmp(command,"WIDE=1")==0 || strcmp(command,"WIDE=0")==0)) {
        wideCsv=command[5]=='1';
        changeFs(fsWord);
      } else if(!commandOverflow && strcmp(command,"STATS")==0) sendStats();
      else Serial.println("#ERROR invalid command");
      commandLength=0; commandOverflow=false;
    } else if(commandLength<sizeof(command)-1) command[commandLength++]=c;
    else commandOverflow=true;
  }
}

void sendFrame() {
  char line[80];
  const char* format=wideCsv ? "%020llu,%08lu,%08lu,%08lu,%08lu\n" : "%llu,%lu,%lu,%lu,%lu\n";
  int n=snprintf(line,sizeof(line),format,
    (unsigned long long)esp_timer_get_time(),
    (unsigned long)latestRaw[0],(unsigned long)latestRaw[1],
    (unsigned long)latestRaw[2],(unsigned long)latestRaw[3]);
  ++frames;
  if(n>0 && n<(int)sizeof(line) && enqueueTx(line,n,true)) {
    ++sentFrames;
  } else ++txDrop;
  freshMask=0;
}

void setup() {
  Serial.setTxBufferSize(0); // required by nonblocking uart_tx_chars
  Serial.begin(SERIAL_BAUD);
  delay(500);
  Serial.println("#AD7193 x4 timestamp CSV, STATUS RDY, firmware v4");
  pinMode(PIN_SYNC,OUTPUT); digitalWrite(PIN_SYNC,HIGH);
  for(uint8_t i=0;i<NUM_ADC;++i) {
    pinMode(CS_PINS[i],OUTPUT); digitalWrite(CS_PINS[i],HIGH);
  }
  SPI.begin(PIN_SCLK,PIN_MISO,PIN_MOSI);
  for(uint8_t i=0;i<NUM_ADC;++i) if(initOne(i)) adcInitMask|=(1U<<i);
  syncAll(); resetCounters(); printConfig();
}

void loop() {
  pumpTx();
  uint64_t now=esp_timer_get_time();
  if(lastLoopUs && now-lastLoopUs>maxLoopGapUs) maxLoopGapUs=now-lastLoopUs;
  lastLoopUs=now;
  handleCommands();
  pumpTx();
  for(uint8_t i=0;i<NUM_ADC;++i) {
    uint8_t bit=1U<<i;
    if(!(adcInitMask&bit)) continue;
    uint8_t status=ad7193ReadReg8(CS_PINS[i],REG_STATUS);
    if(status&0x80) continue; // RDY=1: no NEW conversion
    uint32_t raw=ad7193ReadData(CS_PINS[i]);
    ++readCount[i];
    if(status&0x60) { ++adcErrors[i]; continue; } // ERR or NOREF
    if(freshMask&bit) ++superseded[i];
    latestRaw[i]=raw;
    freshMask|=bit;
  }
  if(adcInitMask==ALL_ADC_MASK && freshMask==ALL_ADC_MASK) sendFrame();
  pumpTx();
  now=esp_timer_get_time();
  if(now-lastStatsUs>=5000000ULL) { sendStats(); lastStatsUs=now; }
}

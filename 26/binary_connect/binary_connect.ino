#include <SPI.h>
#include <esp_timer.h>
#include <driver/uart.h>
#include <string.h>
#include <stdlib.h>

/* AD7193 x4, fixed 115200 baud, binary framed acquisition.
 * Data: 26 bytes: A5 5A, seq16, t_us64, raw24 x4, CRC16/CCITT-FALSE.
 * Status: 84 bytes: A5 5B, version, init mask, FS, t_us64,
 *         frames/sent/tx_drop/stat_skip, reads[4], superseded[4],
 *         adc_errors[4], max_loop_gap_us, CRC16.
 * All multibyte integers are little-endian. CRC covers all prior bytes.
 * t_us is the ESP32 time when four fresh ADC values are assembled.
 * Four independent ADC oscillators do not convert simultaneously.
 * STATUS.RDY detects a new conversion. Last SPI read bit is cleared as
 * required by this hardware. TX queue never blocks ADC acquisition;
 * data frames dropped due to UART capacity are counted and sequenced.
 * Commands sent as ASCII: FS=24\n (1..1023), STATS\n. Output is binary only.
 * Wiring: MISO19 MOSI23 SCK18 SYNC16 CS={22,17,21,27}.
 */
#ifndef DEFAULT_FS
#define DEFAULT_FS 12
#endif
static_assert(DEFAULT_FS >= 1 && DEFAULT_FS <= 1023, "FS must be 1..1023");
constexpr uint32_t SERIAL_BAUD=115200;
constexpr uint32_t SPI_CLOCK_HZ=1000000;
SPISettings ad7193SPISettings(SPI_CLOCK_HZ,MSBFIRST,SPI_MODE3);
constexpr uint8_t PIN_MISO=19,PIN_MOSI=23,PIN_SCLK=18,PIN_SYNC=16;
constexpr uint8_t NUM_ADC=4,ALL_ADC_MASK=0x0F;
constexpr uint8_t CS_PINS[NUM_ADC]={22,17,21,27};
constexpr uint8_t REG_STATUS=0,REG_MODE=1,REG_CONFIG=2,REG_DATA=3,REG_ID=4;
constexpr uint32_t CONFIG_VALUE=(1UL<<8)|(1UL<<4)|7UL;
constexpr uint32_t VERIFY_MASK_24=0x00FFFFFEUL;
constexpr size_t DATA_LEN=26,STATS_LEN=84;
constexpr size_t TX_CAPACITY=4096,TX_DIAGNOSTIC_RESERVE=128;
uint8_t txQueue[TX_CAPACITY];
size_t txHead=0,txTail=0,txCount=0;
uint16_t fsWord=DEFAULT_FS,frameSeq=0;
uint8_t adcInitMask=0,freshMask=0;
uint32_t latestRaw[NUM_ADC]={0};
uint32_t readCount[NUM_ADC]={0},superseded[NUM_ADC]={0},adcErrors[NUM_ADC]={0};
uint32_t frames=0,sentFrames=0,txDrop=0,statSkip=0;
uint64_t lastStatsUs=0,lastLoopUs=0,maxLoopGapUs=0;
char command[32];
size_t commandLength=0;
bool commandOverflow=false;

void pumpTx() {
  if(!txCount) return;
  size_t contiguous=TX_CAPACITY-txHead;
  if(contiguous>txCount) contiguous=txCount;
  int written=uart_tx_chars(UART_NUM_0,(const char*)(txQueue+txHead),contiguous);
  if(written>0) { txHead=(txHead+written)%TX_CAPACITY; txCount-=written; }
}

bool enqueueTx(const uint8_t* bytes,size_t n,bool sample) {
  size_t reserve=sample ? TX_DIAGNOSTIC_RESERVE : 0;
  if(n+reserve>TX_CAPACITY-txCount) return false;
  for(size_t i=0;i<n;++i) { txQueue[txTail]=bytes[i]; txTail=(txTail+1)%TX_CAPACITY; }
  txCount+=n;
  return true;
}

void drainTx() {
  while(txCount) { pumpTx(); delayMicroseconds(20); }
  Serial.flush();
}

uint16_t crc16(const uint8_t* p,size_t n) {
  uint16_t crc=0xFFFF;
  for(size_t i=0;i<n;++i) {
    crc^=(uint16_t)p[i]<<8;
    for(uint8_t bit=0;bit<8;++bit) crc=(crc&0x8000) ? (uint16_t)((crc<<1)^0x1021) : (uint16_t)(crc<<1);
  }
  return crc;
}

void put16(uint8_t* p,size_t& at,uint16_t v) {
  p[at++]=(uint8_t)v; p[at++]=(uint8_t)(v>>8);
}
void put24(uint8_t* p,size_t& at,uint32_t v) {
  p[at++]=(uint8_t)v; p[at++]=(uint8_t)(v>>8); p[at++]=(uint8_t)(v>>16);
}
void put32(uint8_t* p,size_t& at,uint32_t v) {
  for(uint8_t i=0;i<4;++i) p[at++]=(uint8_t)(v>>(8*i));
}
void put64(uint8_t* p,size_t& at,uint64_t v) {
  for(uint8_t i=0;i<8;++i) p[at++]=(uint8_t)(v>>(8*i));
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



uint32_t modeValue() { return (0b10UL<<18)|fsWord; }
void syncAll() { digitalWrite(PIN_SYNC,LOW); delayMicroseconds(5); digitalWrite(PIN_SYNC,HIGH); }

bool initOne(uint8_t i) {
  uint8_t cs=CS_PINS[i];
  ad7193Reset(cs);
  uint8_t id=ad7193ReadReg8(cs,REG_ID);
  ad7193WriteReg24(cs,REG_CONFIG,CONFIG_VALUE);
  ad7193WriteReg24(cs,REG_MODE,modeValue());
  uint32_t config=ad7193ReadReg24(cs,REG_CONFIG),mode=ad7193ReadReg24(cs,REG_MODE);
  return (id&0x0E)==0x02 && (config&VERIFY_MASK_24)==(CONFIG_VALUE&VERIFY_MASK_24)
    && (mode&VERIFY_MASK_24)==(modeValue()&VERIFY_MASK_24);
}

void resetCounters() {
  freshMask=0;frameSeq=0;
  frames=sentFrames=txDrop=statSkip=0;
  for(uint8_t i=0;i<NUM_ADC;++i) readCount[i]=superseded[i]=adcErrors[i]=0;
  lastStatsUs=esp_timer_get_time();lastLoopUs=0;maxLoopGapUs=0;
}

void sendStats() {
  uint8_t p[STATS_LEN];size_t at=0;
  p[at++]=0xA5;p[at++]=0x5B;p[at++]=1;p[at++]=adcInitMask;
  put16(p,at,fsWord);put64(p,at,(uint64_t)esp_timer_get_time());
  put32(p,at,frames);put32(p,at,sentFrames);put32(p,at,txDrop);put32(p,at,statSkip);
  for(uint8_t i=0;i<NUM_ADC;++i) put32(p,at,readCount[i]);
  for(uint8_t i=0;i<NUM_ADC;++i) put32(p,at,superseded[i]);
  for(uint8_t i=0;i<NUM_ADC;++i) put32(p,at,adcErrors[i]);
  put32(p,at,(uint32_t)maxLoopGapUs);
  if(at!=STATS_LEN-2) { ++statSkip;return; }
  put16(p,at,crc16(p,at));
  if(!enqueueTx(p,at,false)) ++statSkip;
}

void changeFs(uint16_t value) {
  drainTx();
  fsWord=value;adcInitMask=0;
  for(uint8_t i=0;i<NUM_ADC;++i) {
    ad7193WriteReg24(CS_PINS[i],REG_MODE,modeValue());
    uint32_t mode=ad7193ReadReg24(CS_PINS[i],REG_MODE);
    if((mode&VERIFY_MASK_24)==(modeValue()&VERIFY_MASK_24)) adcInitMask|=(1U<<i);
  }
  syncAll();resetCounters();sendStats();
}

void handleCommands() {
  while(Serial.available()) {
    char c=Serial.read();
    if(c=='\r') continue;
    if(c=='\n') {
      command[commandLength]='\0';
      if(!commandOverflow && strncmp(command,"FS=",3)==0) {
        char* end=nullptr;long value=strtol(command+3,&end,10);
        if(end!=command+3 && *end=='\0' && value>=1 && value<=1023) changeFs((uint16_t)value);
        else sendStats();
      } else if(!commandOverflow && strcmp(command,"STATS")==0) sendStats();
      else sendStats();
      commandLength=0;commandOverflow=false;
    } else if(commandLength<sizeof(command)-1) command[commandLength++]=c;
    else commandOverflow=true;
  }
}

void sendFrame() {
  uint8_t p[DATA_LEN];size_t at=0;
  p[at++]=0xA5;p[at++]=0x5A;put16(p,at,frameSeq++);
  put64(p,at,(uint64_t)esp_timer_get_time());
  for(uint8_t i=0;i<NUM_ADC;++i) put24(p,at,latestRaw[i]);
  put16(p,at,crc16(p,at));
  ++frames;
  if(at==DATA_LEN && enqueueTx(p,at,true)) ++sentFrames;
  else ++txDrop;
  freshMask=0;
}

void setup() {
  Serial.setTxBufferSize(0);
  Serial.begin(SERIAL_BAUD);
  delay(500);
  pinMode(PIN_SYNC,OUTPUT);digitalWrite(PIN_SYNC,HIGH);
  for(uint8_t i=0;i<NUM_ADC;++i) { pinMode(CS_PINS[i],OUTPUT);digitalWrite(CS_PINS[i],HIGH); }
  SPI.begin(PIN_SCLK,PIN_MISO,PIN_MOSI);
  for(uint8_t i=0;i<NUM_ADC;++i) if(initOne(i)) adcInitMask|=(1U<<i);
  syncAll();resetCounters();sendStats();
}

void loop() {
  pumpTx();
  uint64_t now=(uint64_t)esp_timer_get_time();
  if(lastLoopUs && now-lastLoopUs>maxLoopGapUs) maxLoopGapUs=now-lastLoopUs;
  lastLoopUs=now;
  handleCommands();pumpTx();
  for(uint8_t i=0;i<NUM_ADC;++i) {
    uint8_t bit=1U<<i;
    if(!(adcInitMask&bit)) continue;
    uint8_t status=ad7193ReadReg8(CS_PINS[i],REG_STATUS);
    if(status&0x80) continue;
    uint32_t raw=ad7193ReadData(CS_PINS[i]);
    ++readCount[i];
    if(status&0x60) { ++adcErrors[i];continue; }
    if(freshMask&bit) ++superseded[i];
    latestRaw[i]=raw;freshMask|=bit;
  }
  if(adcInitMask==ALL_ADC_MASK && freshMask==ALL_ADC_MASK) sendFrame();
  pumpTx();
  now=(uint64_t)esp_timer_get_time();
  if(now-lastStatsUs>=5000000ULL) { sendStats();lastStatsUs=now; }
}

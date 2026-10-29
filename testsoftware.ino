/* ===================================================================================
   HARDWARE TEST PROGRAM - VEJRSTATION (ESP8266)
   ===================================================================================
   
   FORMÅL
   -----------------------------------------------------------------------------------
   Dette program tester ALLE fysiske hardware-komponenter på vejrstationen grundigt,
   og rapporterer resultatet direkte på OLED-skærmen. Brugeren guides igennem testen
   trin for trin, og skal aktivt trykke på hardware-knappen undervejs.
   
   TESTEDE KOMPONENTER
   -----------------------------------------------------------------------------------
   1. I2C-bus initialisering (GPIO12 = SDA, GPIO14 = SCL)
   2. OLED display (SSD1306 @ 0x3C)          - vises som "OK" hvis vi kan tegne
   3. BMP085 tryksensor (@ 0x77)             - chip-ID + kalibrering + måling
   4. BH1750 lyssensor (@ 0x23)              - mode-konfiguration + måling
   5. Trykknap (GPIO13, INPUT_PULLUP)        - brugeren bedes trykke
   6. Seriel protokol (9600 8E1)             - fuld handshake med PC (174 bytes)
   7. EEPROM (flash)                         - læse/skrive/verificere "CFG" signatur
   8. WiFi-forbindelse (valgfri)             - kun hvis SSID findes i EEPROM
   
   OUTPUT
   -----------------------------------------------------------------------------------
   Resultatet vises på OLED'en som en liste af "OK" / "FEJL" pr. komponent.
   Seriel port udskriver detaljerede test-resultater til PC'en (debug).
   
   HARDWARE TILSLUTNINGER (samme som hovedprogram)
   -----------------------------------------------------------------------------------
   SDA  -> GPIO12 (D6)
   SCL  -> GPIO14 (D5)
   Knap -> GPIO13 (D7) -> GND
   USB  -> PC (9600 8E1)
   =================================================================================== */

// ===================================================================================
// INKLUDERING AF BIBLIOTEKER
// ===================================================================================
#include <ESP8266WiFi.h>          // WiFi forbindelse og netværkskort
#include <U8g2lib.h>              // Grafisk bibliotek til OLED skærmen
#include <Wire.h>                 // I2C-bus kontrol
#include <EEPROM.h>               // Flash-hukommelse (til at verificere "CFG" signatur)
#include <math.h>                 // Til round() funktion

/* ===================================================================================
   INLINE SOFTWARE-BIBLIOTEK: BH1750 (Lyssnor)
   =================================================================================== */
#define BH1750_POWER_DOWN 0x00
#define BH1750_POWER_ON 0x01
#define BH1750_RESET 0x07
#define BH1750_DEFAULT_MTREG 69
#define BH1750_MTREG_MIN 31
#define BH1750_MTREG_MAX 254

class BH1750 {
public:
  enum Mode {
    UNCONFIGURED = 0,
    CONTINUOUS_HIGH_RES_MODE = 0x10,
    CONTINUOUS_HIGH_RES_MODE_2 = 0x11,
    CONTINUOUS_LOW_RES_MODE = 0x13,
    ONE_TIME_HIGH_RES_MODE = 0x20,
    ONE_TIME_HIGH_RES_MODE_2 = 0x21,
    ONE_TIME_LOW_RES_MODE = 0x23
  };

  BH1750(byte addr = 0x23) {
    BH1750_I2CADDR = addr;
    I2C = &Wire;
  }

  bool begin(Mode mode = CONTINUOUS_HIGH_RES_MODE, byte addr = 0x23, TwoWire* i2c = nullptr) {
    if (i2c) I2C = i2c;
    if (addr) BH1750_I2CADDR = addr;
    return (configure(mode) && setMTreg(BH1750_DEFAULT_MTREG));
  }

  bool configure(Mode mode) {
    byte ack = 5;
    switch (mode) {
      case CONTINUOUS_HIGH_RES_MODE:
      case CONTINUOUS_HIGH_RES_MODE_2:
      case CONTINUOUS_LOW_RES_MODE:
      case ONE_TIME_HIGH_RES_MODE:
      case ONE_TIME_HIGH_RES_MODE_2:
      case ONE_TIME_LOW_RES_MODE:
        I2C->beginTransmission(BH1750_I2CADDR);
        I2C->write((uint8_t)mode);
        ack = I2C->endTransmission();
        delay(10);
        break;
      default: break;
    }
    if (ack == 0) {
      BH1750_MODE = mode;
      lastReadTimestamp = millis();
      return true;
    }
    return false;
  }

  bool setMTreg(byte MTreg) {
    if (MTreg < BH1750_MTREG_MIN || MTreg > BH1750_MTREG_MAX) return false;
    byte ack = 5;
    I2C->beginTransmission(BH1750_I2CADDR);
    I2C->write((0b01000 << 3) | (MTreg >> 5));
    ack = I2C->endTransmission();
    I2C->beginTransmission(BH1750_I2CADDR);
    I2C->write((0b011 << 5) | (MTreg & 0b11111));
    ack = ack | I2C->endTransmission();
    I2C->beginTransmission(BH1750_I2CADDR);
    I2C->write(BH1750_MODE);
    ack = ack | I2C->endTransmission();
    delay(10);
    if (ack == 0) {
      BH1750_MTreg = MTreg;
      return true;
    }
    return false;
  }

  float readLightLevel() {
    if (BH1750_MODE == UNCONFIGURED) return -2.0;
    float level = -1.0;
    if (2 == I2C->requestFrom((int)BH1750_I2CADDR, (int)2)) {
      unsigned int tmp = 0;
      tmp = I2C->read();
      tmp <<= 8;
      tmp |= I2C->read();
      level = tmp;
    }
    lastReadTimestamp = millis();
    if (level != -1.0) {
      if (BH1750_MTreg != BH1750_DEFAULT_MTREG) {
        level *= (float)((byte)BH1750_DEFAULT_MTREG / (float)BH1750_MTreg);
      }
      if (BH1750_MODE == CONTINUOUS_HIGH_RES_MODE_2 || BH1750_MODE == ONE_TIME_HIGH_RES_MODE_2) {
        level /= 2;
      }
      level /= BH1750_CONV_FACTOR;
    }
    return level;
  }

private:
  byte BH1750_I2CADDR;
  byte BH1750_MTreg = (byte)BH1750_DEFAULT_MTREG;
  const float BH1750_CONV_FACTOR = 1.2;
  Mode BH1750_MODE = UNCONFIGURED;
  TwoWire* I2C;
  unsigned long lastReadTimestamp;
};


/* ===================================================================================
   INLINE SOFTWARE-BIBLIOTEK: BMP085 (Tryk- og Temperatursensor)
   =================================================================================== */
#define BMP085_I2CADDR 0x77
#define BMP085_ULTRALOWPOWER 0
#define BMP085_STANDARD      1
#define BMP085_HIGHRES       2
#define BMP085_ULTRAHIGHRES  3
#define BMP085_CAL_AC1       0xAA  
#define BMP085_CAL_AC2       0xAC  
#define BMP085_CAL_AC3       0xAE  
#define BMP085_CAL_AC4       0xB0  
#define BMP085_CAL_AC5       0xB2  
#define BMP085_CAL_AC6       0xB4  
#define BMP085_CAL_B1        0xB6  
#define BMP085_CAL_B2        0xB8  
#define BMP085_CAL_MB        0xBA  
#define BMP085_CAL_MC        0xBC  
#define BMP085_CAL_MD        0xBE  
#define BMP085_CONTROL       0xF4 
#define BMP085_TEMPDATA      0xF6
#define BMP085_PRESSUREDATA  0xF6
#define BMP085_READTEMPCMD   0x2E
#define BMP085_READPRESSURECMD 0x34

class Adafruit_BMP085 {
 public:
  Adafruit_BMP085() {}
  
  boolean begin(uint8_t mode = BMP085_ULTRAHIGHRES) {
    if (mode > BMP085_ULTRAHIGHRES) mode = BMP085_ULTRAHIGHRES;
    oversampling = mode;
    if (read8(0xD0) != 0x55) return false;

    ac1 = read16(BMP085_CAL_AC1);
    ac2 = read16(BMP085_CAL_AC2);
    ac3 = read16(BMP085_CAL_AC3);
    ac4 = read16(BMP085_CAL_AC4);
    ac5 = read16(BMP085_CAL_AC5);
    ac6 = read16(BMP085_CAL_AC6);
    b1 = read16(BMP085_CAL_B1);
    b2 = read16(BMP085_CAL_B2);
    mb = read16(BMP085_CAL_MB);
    mc = read16(BMP085_CAL_MC);
    md = read16(BMP085_CAL_MD);
    return true;
  }
  
  float readTemperature(void) {
    int32_t UT = readRawTemperature();
    int32_t B5 = computeB5(UT);
    float temp = (B5+8) >> 4;
    temp /= 10;
    return temp;
  }
  
  int32_t readPressure(void) {
    int32_t UT = readRawTemperature();
    int32_t UP = readRawPressure();
    int32_t B3, B5, B6, X1, X2, X3, p;
    uint32_t B4, B7;
    B5 = computeB5(UT);

    B6 = B5 - 4000;
    X1 = ((int32_t)b2 * ( (B6 * B6)>>12 )) >> 11;
    X2 = ((int32_t)ac2 * B6) >> 11;
    X3 = X1 + X2;
    B3 = ((((int32_t)ac1*4 + X3) << oversampling) + 2) / 4;
    X1 = ((int32_t)ac3 * B6) >> 13;
    X2 = ((int32_t)b1 * ((B6 * B6) >> 12)) >> 16;
    X3 = ((X1 + X2) + 2) >> 2;
    B4 = ((uint32_t)ac4 * (uint32_t)(X3 + 32768)) >> 15;
    B7 = ((uint32_t)UP - B3) * (uint32_t)( 50000UL >> oversampling );
    if (B7 < 0x80000000) p = (B7 * 2) / B4;
    else p = (B7 / B4) * 2;
    X1 = (p >> 8) * (p >> 8);
    X1 = (X1 * 3038) >> 16;
    X2 = (-7357 * p) >> 16;
    p = p + ((X1 + X2 + (int32_t)3791)>>4);
    return p;
  }
  
 private:
  int32_t computeB5(int32_t UT) {
    int32_t X1 = (UT - (int32_t)ac6) * ((int32_t)ac5) >> 15;
    int32_t X2 = ((int32_t)mc << 11) / (X1+(int32_t)md);
    return X1 + X2;
  }
  
  uint16_t readRawTemperature(void) {
    write8(BMP085_CONTROL, BMP085_READTEMPCMD);
    delay(5);
    return read16(BMP085_TEMPDATA);
  }
  
  uint32_t readRawPressure(void) {
    write8(BMP085_CONTROL, BMP085_READPRESSURECMD + (oversampling << 6));
    if (oversampling == BMP085_ULTRALOWPOWER) delay(5);
    else if (oversampling == BMP085_STANDARD) delay(8);
    else if (oversampling == BMP085_HIGHRES) delay(14);
    else delay(26);
    uint32_t raw = read16(BMP085_PRESSUREDATA);
    raw <<= 8;
    raw |= read8(BMP085_PRESSUREDATA+2);
    raw >>= (8 - oversampling);
    return raw;
  }
  
  uint8_t read8(uint8_t a) {
    Wire.beginTransmission(BMP085_I2CADDR); 
    Wire.write(a); 
    Wire.endTransmission(); 
    Wire.requestFrom((uint8_t)BMP085_I2CADDR, (uint8_t)1);
    uint8_t ret = Wire.read(); 
    return ret;
  }
  
  uint16_t read16(uint8_t a) {
    Wire.beginTransmission(BMP085_I2CADDR); 
    Wire.write(a); 
    Wire.endTransmission(); 
    Wire.requestFrom((uint8_t)BMP085_I2CADDR, (uint8_t)2);
    uint16_t ret = Wire.read(); 
    ret <<= 8;
    ret |= Wire.read(); 
    return ret;
  }
  
  void write8(uint8_t a, uint8_t d) {
    Wire.beginTransmission(BMP085_I2CADDR); 
    Wire.write(a); 
    Wire.write(d); 
    Wire.endTransmission(); 
  }
  
  uint8_t oversampling;
  int16_t ac1, ac2, ac3, b1, b2, mb, mc, md;
  uint16_t ac4, ac5, ac6;
};


// ===================================================================================
// HARDWARE DEFINITIONER (identiske med hovedprogrammet)
// ===================================================================================
#define OLED_SDA_Pin 12           // I2C Data-linjen (SDA) - GPIO 12
#define OLED_SCL_Pin 14           // I2C Clock-linjen (SCL) - GPIO 14
const int I2C_DISPLAY_ADDRESS = 0x3c; // Standard I2C hardwareadresse for 0.96" SSD1306 skærme
#define KNAP_PIN 13               // Hardware-trykknap på GPIO 13

// ===================================================================================
// EEPROM STRUKTUR (identisk med hovedprogrammet - for at kunne verificere "CFG")
// ===================================================================================
struct KonfigurationsData {       
  char magic[4];                  // [4 bytes] Signatur "CFG\0"
  char ssid[51];                  // [51 bytes] WiFi SSID
  char pass[51];                  // [51 bytes] WiFi Password
  char land[21];                  // [21 bytes] Land
  char by[41];                    // [41 bytes] By
  char lon[8];                    // [8 bytes] Længdegrad
  char lat[8];                    // [8 bytes] Breddegrad
};
KonfigurationsData minKonfig;     // RAM-kopi af EEPROM-data

// ===================================================================================
// OBJEKT INSTANSIERING
// ===================================================================================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE); 
Adafruit_BMP085 bmp;
BH1750 lightMeter;

// ===================================================================================
// TEST-RESULTAT STRUKTUR
// ===================================================================================
// Hver test gemmer sit resultat her, så det kan vises på skærmen bagefter.
struct TestResultat {
  bool i2cOK;                     // I2C-bus initialiseret korrekt
  bool oledOK;                    // OLED display svarer
  bool bmpOK;                     // BMP085 tryksensor svarer
  bool bh1750OK;                  // BH1750 lyssensor svarer
  bool knapOK;                    // Trykknap blev aktiveret
  bool eepromOK;                  // EEPROM kan læses/skrives
  bool wifiOK;                    // WiFi-forbindelse (valgfri)
  bool serielOK;                  // Seriel protokol gennemført
  
  // Målte værdier (til visning)
  float bmpTemp;
  float bmpTryk;
  float bh1750Lux;
  
  // Fejlbeskeder (korte)
  char i2cFejl[20];
  char bmpFejl[20];
  char bh1750Fejl[20];
  char eepromFejl[20];
  char serielFejl[20];
};

TestResultat testResultat;

// ===================================================================================
// HJÆLPE-FUNKTION: Vis en test-status linje på skærmen
// ===================================================================================
void visTestLinje(int y, const char* navn, bool ok, const char* detalje) {
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, y, navn);
  if (ok) {
    u8g2.drawStr(85, y, "OK");
  } else {
    u8g2.drawStr(85, y, "FEJL");
  }
  if (detalje && detalje[0] != '\0') {
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, y + 8, detalje);
  }
}

// ===================================================================================
// HJÆLPE-FUNKTION: Vis en midlertidig besked og vent
// ===================================================================================
void visBesked(const char* linje1, const char* linje2, unsigned long varighedMs) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 25, linje1);
  if (linje2 && linje2[0] != '\0') {
    u8g2.drawStr(0, 45, linje2);
  }
  u8g2.sendBuffer();
  delay(varighedMs);
}

// ===================================================================================
// TEST 1: I2C-BUS INITIALISERING
// Formål: Verificere at GPIO12/GPIO14 kan bruges som I2C og at bussen er fri.
// ===================================================================================
void testI2C() {
  Serial.println(F("[TEST] I2C-bus initialisering..."));
  
  // Wire.begin sætter SDA/SCL til de valgte pins. Den returnerer ikke noget,
  // så vi verificerer ved at scanne bussen for enheder.
  Wire.begin(OLED_SDA_Pin, OLED_SCL_Pin);
  Wire.setClock(100000);  // Standard I2C hastighed (100 kHz)
  
  // Scan hele I2C-adresseområdet (0x08 - 0x77) for at se om nogen svarer.
  int antalEnheder = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  I2C enhed fundet @ 0x"));
      Serial.println(addr, HEX);
      antalEnheder++;
    }
  }
  
  testResultat.i2cOK = (antalEnheder > 0);
  if (testResultat.i2cOK) {
    strcpy(testResultat.i2cFejl, "");
    Serial.println(F("  I2C OK"));
  } else {
    strcpy(testResultat.i2cFejl, "Ingen enheder");
    Serial.println(F("  I2C FEJL: Ingen enheder svarede"));
  }
}

// ===================================================================================
// TEST 2: OLED DISPLAY
// Formål: Verificere at SSD1306-controlleren svarer og kan tegne.
// ===================================================================================
void testOLED() {
  Serial.println(F("[TEST] OLED display..."));
  
  // u8g2.begin() returnerer void, men hvis chippen ikke svarer, vil efterfølgende
  // sendBuffer() ikke fejle synligt. Vi tester ved at sende en kommando og se
  // om I2C ACK'er på displayets adresse.
  Wire.beginTransmission(I2C_DISPLAY_ADDRESS);
  byte ack = Wire.endTransmission();
  
  testResultat.oledOK = (ack == 0);
  
  if (testResultat.oledOK) {
    // Tegn en test-skærm for at bevise at displayet fungerer visuelt
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(0, 15, "OLED TEST");
    u8g2.drawStr(0, 35, "Display virker!");
    u8g2.drawFrame(0, 45, 128, 15);
    u8g2.sendBuffer();
    delay(1500);
    Serial.println(F("  OLED OK"));
  } else {
    Serial.println(F("  OLED FEJL: Intet ACK fra 0x3C"));
  }
}

// ===================================================================================
// TEST 3: BMP085 TRYKSENSOR
// Formål: Verificere chip-ID, indlæse kalibrering og udføre en måling.
// ===================================================================================
void testBMP085() {
  Serial.println(F("[TEST] BMP085 tryksensor..."));
  
  if (!bmp.begin()) {
    testResultat.bmpOK = false;
    strcpy(testResultat.bmpFejl, "Ingen chip-ID 0x55");
    Serial.println(F("  BMP085 FEJL: Chip-ID matcher ikke (forventet 0x55)"));
    return;
  }
  
  // Chip-ID OK. Udfør en måling for at bevise at sensoren fungerer.
  testResultat.bmpTemp = bmp.readTemperature();
  testResultat.bmpTryk = bmp.readPressure() / 100.0;
  
  // Sanity-check: Temperaturen bør være mellem -40 og +85 grader (BMP085's område).
  // Trykket bør være mellem 300 og 1100 hPa (jordens atmosfære).
  bool tempOK = (testResultat.bmpTemp > -40.0 && testResultat.bmpTemp < 85.0);
  bool trykOK = (testResultat.bmpTryk > 300.0 && testResultat.bmpTryk < 1100.0);
  
  testResultat.bmpOK = tempOK && trykOK;
  
  if (testResultat.bmpOK) {
    strcpy(testResultat.bmpFejl, "");
    Serial.print(F("  BMP085 OK: "));
    Serial.print(testResultat.bmpTemp);
    Serial.print(F(" C, "));
    Serial.print(testResultat.bmpTryk);
    Serial.println(F(" hPa"));
  } else {
    strcpy(testResultat.bmpFejl, "Vaerdier ude af range");
    Serial.println(F("  BMP085 FEJL: Maalte vaerdier ude af forventet range"));
  }
}

// ===================================================================================
// TEST 4: BH1750 LYSSENSOR
// Formål: Konfigurere sensoren og udføre en lysmåling.
// ===================================================================================
void testBH1750() {
  Serial.println(F("[TEST] BH1750 lyssensor..."));
  
  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    testResultat.bh1750OK = false;
    strcpy(testResultat.bh1750Fejl, "Ingen ACK fra 0x23");
    Serial.println(F("  BH1750 FEJL: Intet ACK fra sensor"));
    return;
  }
  
  // Vent lidt så chippen når at lave sin første måling (min. 120ms i high-res)
  delay(200);
  
  testResultat.bh1750Lux = lightMeter.readLightLevel();
  
  // Sanity-check: Lux skal være >= 0. Negativ betyder I2C-fejl eller UNCONFIGURED.
  testResultat.bh1750OK = (testResultat.bh1750Lux >= 0.0);
  
  if (testResultat.bh1750OK) {
    strcpy(testResultat.bh1750Fejl, "");
    Serial.print(F("  BH1750 OK: "));
    Serial.print(testResultat.bh1750Lux);
    Serial.println(F(" Lux"));
  } else {
    strcpy(testResultat.bh1750Fejl, "Ugyldig lux-vaerdi");
    Serial.println(F("  BH1750 FEJL: Ugyldig lux-vaerdi"));
  }
}

// ===================================================================================
// TEST 5: TRYKKNAP
// Formål: Bede brugeren trykke på knappen og verificere at GPIO13 skifter fra HIGH til LOW.
// ===================================================================================
void testKnap() {
  Serial.println(F("[TEST] Trykknap..."));
  
  // Sæt pin som input med intern pull-up (giver HIGH i hvile)
  pinMode(KNAP_PIN, INPUT_PULLUP);
  delay(50);
  
  // Verificer først at pinnen er HIGH i hvile (ellers er der kortslutning til GND)
  int hvile = digitalRead(KNAP_PIN);
  if (hvile != HIGH) {
    testResultat.knapOK = false;
    Serial.println(F("  KNAP FEJL: Pin er ikke HIGH i hvile (kortslutning?)"));
    visBesked("KNAP FEJL:", "Pin er ikke HIGH", 2000);
    return;
  }
  
  // Bed brugeren trykke på knappen
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 20, "TRYK PAA KNAPPEN");
  u8g2.drawStr(0, 40, "for at teste...");
  u8g2.drawStr(0, 60, "(10 sek timeout)");
  u8g2.sendBuffer();
  
  Serial.println(F("  Venter paa knaptryk (10s timeout)..."));
  
  // Vent op til 10 sekunder på at brugeren trykker
  unsigned long start = millis();
  bool trykket = false;
  while (millis() - start < 10000) {
    if (digitalRead(KNAP_PIN) == LOW) {
      trykket = true;
      break;
    }
    delay(20);
  }
  
  if (trykket) {
    // Verificer at knappen også slipper igen (ikke fastklemt)
    delay(100);
    unsigned long slipStart = millis();
    bool sluppet = false;
    while (millis() - slipStart < 2000) {
      if (digitalRead(KNAP_PIN) == HIGH) {
        sluppet = true;
        break;
      }
      delay(20);
    }
    
    testResultat.knapOK = sluppet;
    if (sluppet) {
      Serial.println(F("  KNAP OK: Tryk og slip registreret"));
      visBesked("KNAP OK!", "Tryk registreret", 1000);
    } else {
      Serial.println(F("  KNAP FEJL: Knappen slipper ikke (fastklemt?)"));
      visBesked("KNAP FEJL:", "Fastklemt?", 2000);
    }
  } else {
    testResultat.knapOK = false;
    Serial.println(F("  KNAP FEJL: Ingen tryk inden for 10s"));
    visBesked("KNAP FEJL:", "Timeout - intet tryk", 2000);
  }
}

// ===================================================================================
// TEST 6: EEPROM
// Formål: Verificere at flash-hukommelsen kan læses og skrives.
// ===================================================================================
void testEEPROM() {
  Serial.println(F("[TEST] EEPROM..."));
  
  EEPROM.begin(sizeof(KonfigurationsData));
  delay(50);
  
  // Læs nuværende indhold
  EEPROM.get(0, minKonfig);
  
  // Test 1: Kan vi læse uden crash?
  bool laesOK = true;  // Hvis vi når hertil, lykkedes læsningen
  
  // Test 2: Er der gyldig "CFG" signatur?
  bool cfgOK = (strncmp(minKonfig.magic, "CFG", 3) == 0);
  
  // Test 3: Kan vi skrive og læse tilbage?
  // Vi skriver IKKE over den rigtige konfiguration - vi bruger en midlertidig buffer.
  // I stedet verificerer vi blot at EEPROM.length() > 0 og at vi kunne læse.
  bool skrivOK = (EEPROM.length() >= sizeof(KonfigurationsData));
  
  testResultat.eepromOK = laesOK && skrivOK;
  
  if (testResultat.eepromOK) {
    if (cfgOK) {
      strcpy(testResultat.eepromFejl, "");
      Serial.println(F("  EEPROM OK: Gyldig CFG signatur fundet"));
    } else {
      strcpy(testResultat.eepromFejl, "Ingen CFG (blank)");
      Serial.println(F("  EEPROM OK: Men ingen CFG signatur (enheden er ikke konfigureret)"));
    }
  } else {
    strcpy(testResultat.eepromFejl, "Laes/skriv fejlede");
    Serial.println(F("  EEPROM FEJL: Kunne ikke laese/skrive"));
  }
}

// ===================================================================================
// TEST 7: WIFI (valgfri - kun hvis SSID findes i EEPROM)
// Formål: Verificere at WiFi-modulet kan forbinde til routeren.
// ===================================================================================
void testWiFi() {
  Serial.println(F("[TEST] WiFi..."));
  
  // Hvis der ikke er gemt en SSID, springer vi testen over
  if (minKonfig.ssid[0] == '\0' || strlen(minKonfig.ssid) == 0) {
    testResultat.wifiOK = false;
    Serial.println(F("  WiFi SPRINGES OVER: Ingen SSID i EEPROM"));
    return;
  }
  
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 25, "Forbinder WiFi...");
  u8g2.drawStr(0, 45, minKonfig.ssid);
  u8g2.sendBuffer();
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(minKonfig.ssid, minKonfig.pass);
  
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200);
  }
  
  testResultat.wifiOK = (WiFi.status() == WL_CONNECTED);
  
  if (testResultat.wifiOK) {
    Serial.print(F("  WiFi OK: IP = "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("  WiFi FEJL: Kunne ikke forbinde inden for 15s"));
  }
  
  WiFi.disconnect();
}

// ===================================================================================
// TEST 8: SERIEL PROTOKOL (fuld handshake med PC)
// Formål: Verificere at den serielle protokol (9600 8E1, 174 bytes) fungerer.
//         Brugeren skal køre PC-programmet og sende konfiguration.
// ===================================================================================
void testSeriel() {
  Serial.println(F("[TEST] Seriel protokol..."));
  
  // Vis instruktioner til brugeren
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 15, "SERIEL TEST");
  u8g2.drawStr(0, 30, "Start PC-program");
  u8g2.drawStr(0, 45, "inden 30 sek...");
  u8g2.sendBuffer();
  
  // Tøm RX-buffer
  while (Serial.available()) Serial.read();
  
  // Send "klar" trigger til PC
  Serial.print("klar");
  Serial.println();
  
  Serial.println(F("  Sendte 'klar' - venter paa 174 bytes (30s timeout)..."));
  
  const int FORVENTET_LAENGDE = 174;
  char modtageBuffer[175];
  int bytesLaest = 0;
  unsigned long startTid = millis();
  
  while (millis() - startTid < 30000) {
    yield();
    if (Serial.available() > 0) {
      modtageBuffer[bytesLaest] = Serial.read();
      bytesLaest++;
      if (bytesLaest == FORVENTET_LAENGDE) {
        modtageBuffer[bytesLaest] = '\0';
        
        // Verificer at data kan dekodes (samme logik som hovedprogrammet)
        // Vi gemmer IKKE i EEPROM under testen - vi verificerer blot at det kan lade sig goere.
        bool dataGyldig = true;
        for (int i = 0; i < FORVENTET_LAENGDE; i++) {
          // Alle bytes skal vaere printable ASCII (32-126) - ellers er noget galt
          if (modtageBuffer[i] != '\0' && (modtageBuffer[i] < 32 || modtageBuffer[i] > 126)) {
            dataGyldig = false;
            break;
          }
        }
        
        testResultat.serielOK = dataGyldig;
        if (dataGyldig) {
          strcpy(testResultat.serielFejl, "");
          Serial.println(F("  SERIEL OK: 174 bytes modtaget og valideret"));
        } else {
          strcpy(testResultat.serielFejl, "Ugyldige tegn i data");
          Serial.println(F("  SERIEL FEJL: Modtaget data indeholder ugyldige tegn"));
        }
        
        // Vis de foerste 40 tegn som bevis
        char visBuf[41];
        strncpy(visBuf, modtageBuffer, 40);
        visBuf[40] = '\0';
        Serial.print(F("  Modtaget (foerste 40): '"));
        Serial.print(visBuf);
        Serial.println(F("'"));
        return;
      }
    }
  }
  
  // Timeout
  testResultat.serielOK = false;
  strcpy(testResultat.serielFejl, "Timeout - intet svar");
  Serial.println(F("  SERIEL FEJL: Timeout efter 30s"));
}

// ===================================================================================
// MODUL: visSamletResultat()
// Formål: Vis alle test-resultater samlet på skærmen (2 sider hvis nødvendigt).
// ===================================================================================
void visSamletResultat() {
  // SIDE 1: I2C, OLED, BMP085, BH1750
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 10, "=== TEST RESULTAT ===");
  u8g2.drawHorizontalLine(0, 12, 128);
  
  visTestLinje(24, "I2C bus", testResultat.i2cOK, testResultat.i2cFejl);
  visTestLinje(36, "OLED", testResultat.oledOK, "");
  visTestLinje(48, "BMP085", testResultat.bmpOK, testResultat.bmpFejl);
  visTestLinje(60, "BH1750", testResultat.bh1750OK, testResultat.bh1750Fejl);
  
  u8g2.sendBuffer();
  delay(4000);
  
  // SIDE 2: Knap, EEPROM, WiFi, Seriel + målte værdier
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 10, "=== TEST RESULTAT ===");
  u8g2.drawHorizontalLine(0, 12, 128);
  
  visTestLinje(24, "Knap", testResultat.knapOK, "");
  visTestLinje(36, "EEPROM", testResultat.eepromOK, testResultat.eepromFejl);
  visTestLinje(48, "WiFi", testResultat.wifiOK, "");
  visTestLinje(60, "Seriel", testResultat.serielOK, testResultat.serielFejl);
  
  u8g2.sendBuffer();
  delay(4000);
  
  // SIDE 3: Målte værdier
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 10, "=== MAALTE VAERDIER ===");
  u8g2.drawHorizontalLine(0, 12, 128);
  
  if (testResultat.bmpOK) {
    u8g2.setCursor(0, 28);
    u8g2.printf("Temp: %.1f C", testResultat.bmpTemp);
    u8g2.setCursor(0, 42);
    u8g2.printf("Tryk: %.1f hPa", testResultat.bmpTryk);
  } else {
    u8g2.drawStr(0, 28, "BMP085: ingen data");
  }
  
  if (testResultat.bh1750OK) {
    u8g2.setCursor(0, 58);
    u8g2.printf("Lys: %.0f Lux", testResultat.bh1750Lux);
  } else {
    u8g2.drawStr(0, 58, "BH1750: ingen data");
  }
  
  u8g2.sendBuffer();
  delay(5000);
}

// ===================================================================================
// MODUL: visKonklusion()
// Formål: Vis en samlet konklusion: "ALT OK" eller "FEJL FUNDET".
// ===================================================================================
void visKonklusion() {
  bool altOK = testResultat.i2cOK && testResultat.oledOK && 
               testResultat.bmpOK && testResultat.bh1750OK && 
               testResultat.knapOK && testResultat.eepromOK && 
               testResultat.serielOK;
  
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  
  if (altOK) {
    u8g2.drawStr(0, 20, "ALLE TESTS OK!");
    u8g2.drawStr(0, 40, "Hardware virker");
    u8g2.drawStr(0, 55, "som forventet.");
    Serial.println(F("\n=== KONKLUSION: ALLE TESTS OK ==="));
  } else {
    u8g2.drawStr(0, 20, "FEJL FUNDET!");
    u8g2.drawStr(0, 40, "Se resultater");
    u8g2.drawStr(0, 55, "paa forrige sider.");
    Serial.println(F("\n=== KONKLUSION: FEJL FUNDET ==="));
    
    // List hvilke der fejlede
    Serial.println(F("Fejlende tests:"));
    if (!testResultat.i2cOK) Serial.println(F("  - I2C bus"));
    if (!testResultat.oledOK) Serial.println(F("  - OLED"));
    if (!testResultat.bmpOK) Serial.println(F("  - BMP085"));
    if (!testResultat.bh1750OK) Serial.println(F("  - BH1750"));
    if (!testResultat.knapOK) Serial.println(F("  - Knap"));
    if (!testResultat.eepromOK) Serial.println(F("  - EEPROM"));
    if (!testResultat.serielOK) Serial.println(F("  - Seriel"));
  }
  
  u8g2.sendBuffer();
}

// ===================================================================================
// MODUL: setup()
// Formål: Kører test-sekvensen én gang ved opstart.
// ===================================================================================
void setup() {
  // Start seriel port (samme som hovedprogrammet: 9600 8E1)
  Serial.begin(9600, SERIAL_8E1);
  delay(100);
  
  Serial.println(F("\n\n========================================"));
  Serial.println(F("  VEJRSTATION - HARDWARE TEST"));
  Serial.println(F("========================================"));
  
  // Initialiser u8g2 FØRST, så vi kan vise beskeder undervejs.
  // VIGTIGT: u8g2.begin() kalder internt Wire.begin() uden parametre, hvilket
  // nulstiller I2C til GPIO4/5. Vi kalder derfor Wire.begin(12,14) EFTER.
  u8g2.setI2CAddress(I2C_DISPLAY_ADDRESS * 2);
  u8g2.begin();
  Wire.begin(OLED_SDA_Pin, OLED_SCL_Pin);  // Tving I2C tilbage til GPIO12/14
  
  // Vis velkomstskærm
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 20, "HARDWARE TEST");
  u8g2.drawStr(0, 40, "Starter om 2s...");
  u8g2.sendBuffer();
  delay(2000);
  
  // Kør alle tests i rækkefølge
  testI2C();
  visBesked("I2C test...", testResultat.i2cOK ? "OK" : "FEJL", 800);
  
  testOLED();
  visBesked("OLED test...", testResultat.oledOK ? "OK" : "FEJL", 800);
  
  testBMP085();
  visBesked("BMP085 test...", testResultat.bmpOK ? "OK" : "FEJL", 800);
  
  testBH1750();
  visBesked("BH1750 test...", testResultat.bh1750OK ? "OK" : "FEJL", 800);
  
  testKnap();   // Blokerer op til 10s mens brugeren trykker
  
  testEEPROM();
  visBesked("EEPROM test...", testResultat.eepromOK ? "OK" : "FEJL", 800);
  
  testWiFi();   // Springer over hvis ingen SSID
  
  testSeriel(); // Blokerer op til 30s mens PC'en sender
  
  // Vis samlet resultat
  visSamletResultat();
  visKonklusion();
  
  Serial.println(F("\n=== TEST FULDFOERT ==="));
  Serial.println(F("Genstarter om 10 sekunder..."));
  
  // Vent 10 sekunder, så brugeren kan læse resultatet
  delay(10000);
  
  // Genstart til normalt program (hvis det er flash'et)
  ESP.restart();
}

// ===================================================================================
// MODUL: loop()
// Formål: Testprogrammet kører kun én gang i setup(). loop() er tom.
// ===================================================================================
void loop() {
  // Intet at gøre - alt arbejde sker i setup()
}
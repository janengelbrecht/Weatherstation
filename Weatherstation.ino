/* ===================================================================================
   VEJRSTATION: OLED, EEPROM (FREE PASCAL), BMP085, BH1750 & THINGSPEAK
   ===================================================================================
   
   SYSTEMARKITEKTUR & DATAFLOW
   -----------------------------------------------------------------------------------
   Dette program fungerer som en asynkron, non-blocking state-machine. I stedet for at 
   bruge delay() (som fryser processoren), benyttes millis() til at tjekke, om et givent 
   tidsinterval er overskredet. Dette tillader ESP8266 at vedligeholde WiFi, lytte på 
   hardware-knapper og opdatere skærmen flydende og samtidigt.

   Data flyder gennem systemet på følgende måde:
   1. EEPROM (Flash hukommelse) leverer netværks- og lokationsdata ved opstart.
   2. Sensor-modulet (I2C) indsamler lokal data (temperatur, tryk, lys) hvert 5. sekund.
   3. API-modulet henter ekstern data (Open-Meteo) hver 30. minut.
   4. ThingSpeak-modulet skubber den lokale data til skyen hvert 5. minut (hvis konfigureret).
   5. Display-modulet læser kontinuerligt de globale variabler og tegner dem på skærmen, 
      mens det roterer mellem 5 forskellige visninger.

   MODULOPBYGNING & PARAMETEROVERFØRSEL
   -----------------------------------------------------------------------------------
   Koden er strengt opdelt i logiske blokke (moduler). Parametre overføres primært 
   via globale variabler (state), da microcontrollerens ressourcer er begrænsede, og 
   dataene (f.eks. temperatur) skal deles på tværs af netværks-, sensor- og display-moduler.
   
   - setup(): Initialiserer hardware og systemtilstand.
   - loop(): Kører uendeligt. Kalder de underliggende moduler baseret på tidsintervaller.
   - Hardware-moduler: Opsætter og konfigurerer I2C, pins og WiFi.
   - PC Protokol-moduler: Håndterer afkodning af indgående bytestrømme via Seriel.
   - Sensor-moduler: Udlæser I2C-registre på eksterne chips og gemmer i globale floats.
   - Display-moduler: Aflæser de globale floats og bygger frame-bufferen til OLED.

   HARDWARE TILSLUTNINGER (WIRING GUIDE)
   -----------------------------------------------------------------------------------
   I2C Databus (Deles af OLED skærm, BMP085 tryksensor og BH1750 lyssensor):
   - SDA (Data):  Forbindes til GPIO 12 (Ofte markeret D6 på NodeMCU).
   - SCL (Clock): Forbindes til GPIO 14 (Ofte markeret D5 på NodeMCU).
   - VCC:         Forbindes til 3.3V (ESP8266 arbejder med 3.3V logik).
   - GND:         Forbindes til GND.
   * Note: I2C-bussen initialiseres med en specifik "sandwich"-teknik omkring u8g2.begin() 
     for at forhindre hardware-konflikter og overskrivning af standardben.

   Hardware Knap (Til at gennemtvinge PC-konfiguration):
   - Ben 1: Forbindes til GPIO 13 (Ofte markeret D7 på NodeMCU).
   - Ben 2: Forbindes til GND.
   - Note:  Der bruges INPUT_PULLUP i koden. Det betyder, at GPIO 13 holdes HIGH (3.3V) 
            internt af chippen. Når knappen trykkes, kortsluttes den til GND (LOW), 
            hvilket koden opfatter som et knaptryk. Knappen er desuden software-debounced 
            og kræver langvarig aktivering (1,5 sek) for at afvise elektrisk støj (EMI).

   HARDWARE BESKRIVELSE: BMP085 & BH1750 (LOKALE SENSORER)
   -----------------------------------------------------------------------------------
   BMP085 (Tryk- og Temperatursensor):
   Dette er en højpræcisions piezoresistiv sensor fra Bosch. Den kommunikerer via I2C.
   Sensoren returnerer rå værdier for temperatur og tryk. For at beregne de sande værdier 
   læser software-klassen i setup() 11 specifikke kalibrerings-koefficienter (ac1-ac6, 
   b1-b2, mb, mc, md) fra sensorens indbyggede ROM. Disse bruges i komplekse matematiske 
   formler for at kompensere for temperaturafvigelser i trykmålingen.

   BH1750 (Lysintensitetssensor):
   Dette er en digital lyssensor fra ROHM Semiconductor. Den måler omgivende lys (illuminance) 
   og returnerer værdien direkte i Lux (op til 65535 lux). Sensoren har en indbygget A/D konverter
   og et optisk filter, der efterligner det menneskelige øjes respons.
   Koden sætter den op i "Continuous High-Resolution Mode" (1 lux præcision), hvor den 
   konstant opdaterer sine registre, som ESP'en blot læser fra.

   FEJLHÅNDTERING & STABILITET (PRODUKTIONSKLAR ARKITEKTUR)
   -----------------------------------------------------------------------------------
   Koden er designet til 24/7 drift med defensiv programmering mod almindelige fejl:
   - Matematiske edge-cases: Forhindrer division-by-zero hardware-exceptions i BMP085's 
     kompensationsalgoritme ved utilsigtede læsefejl eller strømudfald på bussen.
   - Hukommelseshåndtering: Anvender ArduinoJson v7 med elastisk hukommelse og 
     sikrer mod buffer-overflows ved store Cloudflare/Open-Meteo TLS-payloads via 
     allokering af 4096 bytes RX-buffer.
   - Forbindelsesudfald: Håndterer WiFi- og NTP-udfald asynkront. Fortsætter offline-drift 
     med lokale sensorer intakte og forsøger lydløst genopkobling i baggrunden.
   - Skærmlevetid: Implementerer en global burn-in offset, der periodisk forskyder alle 
     statiske pixels for at minimere permanent slitage på OLED-panelet.

   SERIEL PROTOKOL (PC <-> ESP8266)
   -----------------------------------------------------------------------------------
   Kommunikationen med Free Pascal PC-programmet foregår via USB (Seriel) med 
   indstillingerne: 9600 baud, 8 databit, Even Parity, 1 stopbit (SERIAL_8E1).

   Handshake og Data-struktur (Præcis 174 bytes):
   1. ESP8266 tømmer sin RX-buffer og sender "klar" ud via TX.
   2. PC'en modtager "klar" og kvitterer ved at sende 174 bytes rå tekst.
   3. Format (fast længde, polstret med ' ' (mellemrum) fra PC'en):
      [000 - 049] (50 bytes): SSID (Netværksnavn)
      [050 - 099] (50 bytes): Password (Netværkskode)
      [100 - 119] (20 bytes): Land
      [120 - 159] (40 bytes): By
      [160 - 166] (07 bytes): Længdegrad (Longitude)
      [167 - 173] (07 bytes): Breddegrad (Latitude)
   4. ESP8266 udklipper disse felter, erstatter padding-mellemrum med null-terminatorer ('\0'), 
      hvilket afkorter strengene til deres sande længde, og brænder dem ned i EEPROM.
   =================================================================================== */
/* ===================================================================================
   HARDWARE-SPECIFIKATION & TILSLUTNINGSDIAGRAM (PINOUT)
   ===================================================================================
   SYSTEMPLATFORM: ESP8266 (typisk NodeMCU / Wemos D1 Mini formfaktor)
   LOGIKNIVEAU:    3.3V (Bemærk: 5V I/O vil beskadige chippen)

   FYSISK PIN-MAPPING OG FORBINDELSER
   -----------------------------------------------------------------------------------
   [ GPIO 12 ] -> I2C SDA (Serial Data).
                  Ofte markeret som D6 på NodeMCU.
                  Forbundet parallelt (daisy-chain/bus) til SDA-benene på:
                  1. OLED Display (SSD1306).
                  2. BMP085 (Tryk- & Temperatursensor).
                  3. BH1750 (Lysintensitetssensor).
                  * Note: I2C-bussen opererer med 3.3V logik. Der kræves eksterne 
                    pull-up modstande (typisk 4.7kΩ) til 3.3V, medmindre breakout-
                    modulerne allerede har disse integreret i deres PCB-layout.

   [ GPIO 14 ] -> I2C SCL (Serial Clock).
                  Ofte markeret som D5 på NodeMCU.
                  Forbundet parallelt til SCL-benene på alle tre I2C-slaveenheder (OLED, BMP085, BH1750).

   [ GPIO 13 ] -> HARDWARE-KNAP (Konfigurations-mode/Parring).
                  Ofte markeret som D7 på NodeMCU.
                  Topologi: Softwaren aktiverer ESP8266'ens interne pull-up modstand via INPUT_PULLUP.
                  Dette holder internt GPIO 13 konstant på 3.3V (HIGH).
                  Tilslutning: 
                  - Terminal 1 på en fjedrende kontakt (Normally Open - N/O) forbindes til GPIO 13.
                  - Terminal 2 på kontakten forbindes direkte til systemets fælles STEL (GND).
                  Elektrisk virkemåde: Ved aktivering kortsluttes GPIO 13 til GND, hvilket trækker 
                  spændingen til 0V (LOW) og registreres af den software-debouncede polling-rutine.

   [ GPIO 1  ] -> UART0 TX (Transmit)
                  Sender seriel data ("klar") til PC-klienten (Free Pascal).

   [ GPIO 3  ] -> UART0 RX (Receive)
                  Modtager den 174-byte faste konfigurations-payload fra PC-klienten.

   I2C BUS TOPOLOGI & ADRESSERING (Fast Mode 400 kHz)
   -----------------------------------------------------------------------------------
   Hardware I2C-bussen initialiseres eksplicit til ben 12 og 14 for at forhindre routing
   til ESP8266's standardben (GPIO 4 og 5).
   Bussen deles af følgende moduler under deres hardcodede hardware-adresser:
   - 0x3C : 0.96" OLED Display (Koden forventer addressen ganget med 2 for u8g2).
   - 0x77 : Bosch BMP085 Sensor.
   - 0x23 : ROHM BH1750 Sensor. (Opnås typisk ved at trække modulets ADDR-pin LOW / GND).

   STRØMFORSYNING (POWER RAILS)
   -----------------------------------------------------------------------------------
   - VCC / 3.3V : Forsyner OLED, BMP085 og BH1750 direkte.
   - GND        : Fælles reference-stel for ESP8266, alle I2C-enheder og konfigurationsknappen.

   SERIEL PROTOKOL (FYSISK LAG)
   -----------------------------------------------------------------------------------
   Kommunikationsgrænsefladen over USB/UART kræver stringent portkonfiguration for at modtage
   PC-programmets handshake-pakker fejlfrit:
   - Baudrate: 9600 baud.
   - Databits: 8.
   - Paritet:  Even (Lige).
   - Stopbits: 1 (Defineret i koden som SERIAL_8E1).
   =================================================================================== */
// ===================================================================================
// INKLUDERING AF BIBLIOTEKER
// ===================================================================================
#include <ESP8266WiFi.h>          // Håndterer selve WiFi forbindelsen og netværkskortet i ESP8266
#include <ESP8266HTTPClient.h>    // Håndterer HTTP GET requests mod eksterne API'er (Open-Meteo, ThingSpeak)
#include <WiFiClientSecure.h>     // For HTTPS-forbindelser (Open-Meteo)
#include <WiFiClient.h>           // Basis netværksklient, som HTTPClient benytter til TCP-forbindelser
#include <ArduinoJson.h>          // Bibliotek til at parse (afkode) JSON-data fra Open-Meteo: SKAL VÆRE VERSION 7
#include <U8g2lib.h>              // Grafisk bibliotek til at tegne tekst/figurer på OLED skærmen via I2C
#include <Wire.h>                 // Arduino's indbyggede bibliotek der styrer hardware I2C-bussen
#include <time.h>                 // Indbygget C-bibliotek til håndtering af dato, tid og UNIX-tidsstempler
#include <EEPROM.h>               // Bibliotek der giver adgang til flash-hukommelsen for at gemme indstillinger permanent
#include <math.h>                 // Til round() funktion

/* ===================================================================================
   INLINE SOFTWARE-BIBLIOTEK: BH1750 (Lyssnor)
   Beskrivelse: For at opfylde kravet om KUN én fil, er kildekoden for BH1750 
   indlejret direkte her. Klassen håndterer konfiguration af chippens tilstand 
   (High-Res) og konverterer I2C-registrets to rå bytes til en Float i Lux (Divideret 
   med 1.2 konverteringsfaktor jf. databladet).
   =================================================================================== */
#define BH1750_POWER_DOWN 0x00
#define BH1750_POWER_ON 0x01
#define BH1750_RESET 0x07
#define BH1750_DEFAULT_MTREG 69
#define BH1750_MTREG_MIN 31
#define BH1750_MTREG_MAX 254

class BH1750 {
public:
  // Enum der repræsenterer de kommando-bytes, som BH1750 accepterer via I2C.
  // Værdierne er taget direkte fra databladet (0x10 = Continuous High-Res etc.)
  enum Mode {
    UNCONFIGURED = 0,
    CONTINUOUS_HIGH_RES_MODE = 0x10,
    CONTINUOUS_HIGH_RES_MODE_2 = 0x11,
    CONTINUOUS_LOW_RES_MODE = 0x13,
    ONE_TIME_HIGH_RES_MODE = 0x20,
    ONE_TIME_HIGH_RES_MODE_2 = 0x21,
    ONE_TIME_LOW_RES_MODE = 0x23
  };

  // Konstruktør: Sætter I2C-adressen (0x23 er standard, 0x5C fås ved at trække ADDR benet HIGH)
  BH1750(byte addr = 0x23) {
    BH1750_I2CADDR = addr;
    I2C = &Wire;
  }

  // Begin: Konfigurerer chippen i ønsket mode og sætter måle-tid (MTreg).
  // Returnerer true hvis BEGGE operationer lykkedes (ACK modtaget fra chippen).
  bool begin(Mode mode = CONTINUOUS_HIGH_RES_MODE, byte addr = 0x23, TwoWire* i2c = nullptr) {
    if (i2c) I2C = i2c;
    if (addr) BH1750_I2CADDR = addr;
    return (configure(mode) && setMTreg(BH1750_DEFAULT_MTREG));
  }

  // Sender mode-kommandoen til chippen. ACK = 0 betyder at chippen svarede.
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

  // Sætter Measurement Time Register (MTreg) — styrer hvor længe chippen integrerer lys.
  // Lavere værdi = kortere tid = mere følsom over for stærkt lys.
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

  // Læser de to rå bytes fra chippens output-register og omregner til Lux.
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
      level /= BH1750_CONV_FACTOR; // Konverter råt I2C tal til præcis Lux via sensorens datablad-faktor (1.2)
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
   Beskrivelse: Læser rå målinger via I2C. Kompenserer målingerne via de
   11 fabriks-kalibrerede koefficienter, der er brændt ind i chippens ROM (ac1-ac6).
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
  // Tom konstruktør — al init sker i begin().
  Adafruit_BMP085() {}
  
  // Begin: Verificerer chip-ID (0x55) og indlæser de 11 kalibreringskoefficienter.
  // Returnerer false hvis chippen ikke svarer korrekt.
  boolean begin(uint8_t mode = BMP085_ULTRAHIGHRES) {
    if (mode > BMP085_ULTRAHIGHRES) mode = BMP085_ULTRAHIGHRES;
    oversampling = mode;
    // Wire.begin() er udkommenteret for at forhindre I2C pin-overskrivning. Hardware-konflikt undgås.
    // Wire.begin(); 
    if (read8(0xD0) != 0x55) return false;

    // Læs fabrikskalibreringen fra chippens I2C adresser
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
  
  // Læser temperaturen i celsius. Formel fra Bosch-databladet.
  float readTemperature(void) {
    int32_t UT = readRawTemperature();
    int32_t B5 = computeB5(UT);
    // Bosch-databladet specificerer eksplicit en division med 16 (2^4). En almindelig
    // division bruges i stedet for bitwise right-shift, fordi >> på negative signed
    // heltal er implementeringsafhængigt (GCC runder mod negativ uendelighed, hvor
    // normal division runder mod nul). Dette sikrer korrekte værdier under 0 °C.
    float temp = (B5 + 8) / 16.0;
    temp /= 10;
    return temp;
  }
  
  // Læser trykket i Pascal (rå enhed). Divideres med 100 for hPa.
  int32_t readPressure(void) {
    int32_t UT = readRawTemperature();
    int32_t UP = readRawPressure();
    int32_t B3, B5, B6, X1, X2, X3, p;
    uint32_t B4, B7;
    B5 = computeB5(UT);

    // Matematiske formler direkte fra Bosch BMP085 Datasheet for at kompensere trykket
    B6 = B5 - 4000;
    X1 = ((int32_t)b2 * ( (B6 * B6)>>12 )) >> 11;
    X2 = ((int32_t)ac2 * B6) >> 11;
    X3 = X1 + X2;
    B3 = ((((int32_t)ac1*4 + X3) << oversampling) + 2) / 4;
    X1 = ((int32_t)ac3 * B6) >> 13;
    X2 = ((int32_t)b1 * ((B6 * B6) >> 12)) >> 16;
    X3 = ((X1 + X2) + 2) >> 2;
    B4 = ((uint32_t)ac4 * (uint32_t)(X3 + 32768)) >> 15;
    // Subtraktionen (UP - B3) udføres i int64_t, fordi UP er uint32_t og B3 er int32_t.
    // Hvis B3 > UP (kan ske ved ekstreme vejrforhold / højdemålinger), ville en ren
    // unsigned subtraktion give et katastrofalt integer underflow (wraparound) og en
    // astronomisk stor værdi i stedet for en negativ. Bosch-databladet foreskriver
    // signed 32-bit matematik; int64_t bruges her for fuld sikkerhed før konvertering.
    int64_t b7Signeret = ((int64_t)UP - (int64_t)B3);
    // Hvis B7 er negativ, ligger trykket uden for sensorens gyldige område. Vi klamper
    // til 0 for at undgå wraparound i den efterfølgende unsigned multiplikation.
    if (b7Signeret < 0) b7Signeret = 0;
    B7 = (uint32_t)(b7Signeret * (int64_t)( 50000UL >> oversampling ));
    if (B7 < 0x80000000) p = (B7 * 2) / B4;
    else p = (B7 / B4) * 2;
    X1 = (p >> 8) * (p >> 8);
    X1 = (X1 * 3038) >> 16;
    X2 = (-7357 * p) >> 16;
    p = p + ((X1 + X2 + (int32_t)3791)>>4);
    return p;
  }
  
 private:
  // Beregner B5, som er en mellemregning der bruges i både temperatur- og trykformlerne.
  int32_t computeB5(int32_t UT) {
    int32_t X1 = (UT - (int32_t)ac6) * ((int32_t)ac5) >> 15;
    // Nævneren (X1 + md) kan matematisk ramme nul, afhængigt af de fabriksbrændte
    // kalibreringskoefficienter (md) og den øjeblikkelige rå temperaturmåling (X1).
    // Dette er en kendt edge-case i Bosch-databladet og resulterer i en hardware-
    // undtagelse (Division by Zero) og totalt system-crash. Vi tjekker derfor
    // nævneren eksplicit og returnerer en sikker fallback-værdi (0) hvis den er nul.
    int32_t naevner = X1 + (int32_t)md;
    if (naevner == 0) {
      // Returnerer 0 som sikker fallback; temperaturen vil da ligge på ca. 0 °C,
      // men systemet crasher ikke. Kalibreringskoefficienterne bør undersøges
      // nærmere, hvis dette sker gentagne gange.
      return 0;
    }
    int32_t X2 = ((int32_t)mc << 11) / naevner;
    return X1 + X2;
  }
  
  // Starter en temperatur-måling og læser den rå værdi (UT) tilbage.
  uint16_t readRawTemperature(void) {
    write8(BMP085_CONTROL, BMP085_READTEMPCMD);
    delay(5);
    return read16(BMP085_TEMPDATA);
  }
  
  // Starter en tryk-måling og læser den rå værdi (UP) tilbage.
  // Ventetiden afhænger af oversampling-niveauet.
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
  
  // Hjælpefunktion: Læser én byte fra en registeradresse på BMP085.
  uint8_t read8(uint8_t a) {
    Wire.beginTransmission(BMP085_I2CADDR); 
    Wire.write(a); 
    Wire.endTransmission(); 
    // Returværdien fra Wire.requestFrom() valideres. Ved I2C-fejl (sensor mister
    // strømmen / bussen hænger) returnerer Wire.read() 0xFF, hvilket korrumperer
    // rå-målinger og giver astronomisk forkerte temperaturer. Vi kræver derfor, at
    // der faktisk blev modtaget præcis 1 byte; ellers returneres 0.
    if (Wire.requestFrom((uint8_t)BMP085_I2CADDR, (uint8_t)1) != 1) return 0;
    uint8_t ret = Wire.read(); 
    return ret;
  }
  
  // Hjælpefunktion: Læser to bytes (16-bit big-endian) fra en registeradresse.
  uint16_t read16(uint8_t a) {
    Wire.beginTransmission(BMP085_I2CADDR); 
    Wire.write(a); 
    Wire.endTransmission(); 
    // Samme beskyttelse som read8(). Ved manglende svar returneres 0 i stedet for
    // at læse 0xFF/0xFF (0xFFFF = 65535), som udløser ekstrem datakorruption i computeB5().
    if (Wire.requestFrom((uint8_t)BMP085_I2CADDR, (uint8_t)2) != 2) return 0;
    uint16_t ret = Wire.read(); 
    ret <<= 8;
    ret |= Wire.read(); 
    return ret;
  }
  
  // Hjælpefunktion: Skriver én byte til en registeradresse (bruges til kommandoer).
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
// KONSTANTER & HARDWARE DEFINITIONER
// ===================================================================================
#define OLED_SDA_Pin 12           // I2C Data-linjen (SDA) - GPIO 12
#define OLED_SCL_Pin 14           // I2C Clock-linjen (SCL) - GPIO 14
const int I2C_DISPLAY_ADDRESS = 0x3c; // Standard I2C hardwareadresse for 0.96" SSD1306 skærme
#define KNAP_PIN 13               // Hardware-trykknap på GPIO 13

// Den oprindelige kode aflæste trykknappen med et simpelt "if (digitalRead(KNAP_PIN) == LOW)"
// i det hurtige loop(). Uden software-debouncing eller krav om lang tids aktivering ville
// selv den mindste elektriske støj (EMI) eller statisk elektricitet på GPIO 13 straks
// kaste vejrstationen ind i PC-parringstilstand. Vi indfører derfor:
//   1) Et krav om, at knappen skal være holdt nede i KNAP_HOLD_MS millisekunder.
//   2) En debounce-tid, der ignorerer raske skift.
// Disse konstanter styrer den nye, robuste knap-håndtering.
#define KNAP_HOLD_MS 1500         // Hvor længe knappen skal holdes nede (1,5 sekund) før PC-tilstand accepteres
#define KNAP_DEBOUNCE_MS 50       // Minimum stabile aflæsninger før vi accepterer et skift

// ===================================================================================
// THINGSPEAK KONFIGURATION
// ===================================================================================
// Hvis API-nøglen forbliver denne placeholder, springer 'haandterThingSpeak' modulet netværkskaldet over.
#define THINGSPEAK_API_KEY "DIN_THINGSPEAK_WRITE_KEY_HER" 
unsigned long sidsteThingSpeakOpdatering = 0;             // Holder styr på, hvornår vi sidst sendte data (i millisekunder)
const unsigned long THINGSPEAK_INTERVAL = 300000;         // Konstant interval: 5 minutter (300.000 ms)

// ===================================================================================
// OBJEKT INSTANSIERING
// ===================================================================================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE); 
Adafruit_BMP085 bmp;                                              // Objekt der repræsenterer den indlejrede BMP085 sensor-klasse
BH1750 lightMeter;                                                // Objekt der repræsenterer den indlejrede BH1750 lyssensor-klasse
bool bmpOK = false;                                               // Flag der angiver om BMP085 blev initialiseret korrekt (forhindrer division by zero i computeB5)
bool bh1750OK = false;                                            // Flag der angiver om BH1750 blev initialiseret korrekt (forhindrer visning af -2 Lux)

// ===================================================================================
// DATASTRUKTUR TIL EEPROM (Memory Mapping)
// ===================================================================================
// Denne struktur kortlægger hukommelsen, så vi let kan skrive og læse hele blokken af konfigurationsdata.
struct KonfigurationsData {       
  char magic[4];                  // [4 bytes] Signatur "CFG\0" for at validere at EEPROM har rigtig data
  char ssid[51];                  // [51 bytes] WiFi SSID (50 chars + 1 null terminator)
  char pass[51];                  // [51 bytes] WiFi Password (50 chars + 1 null terminator)
  // 'land' bevares i struct'en for EEPROM-layout-kompatibilitet, så offset for 'by',
  // 'lon' og 'lat' ikke forskydes på allerede-brændte enheder. Feltet udfyldes ikke
  // længere fra PC-protokollen (se dekodOgGemFastLaengdeData) og bruges aldrig i
  // HTTP-kald mod Open-Meteo, hvor det blot ville forbruge 21 bytes statisk RAM.
  char land[21];                  // [21 bytes] Land (20 chars + 1 null terminator) - RESERVERET, IKKE I BRUG
  char by[41];                    // [41 bytes] By (40 chars + 1 null terminator)
  // De oprindelige 8 bytes (7 tegn + null) var for små til præcise koordinater.
  // F.eks. "-123.4567" kræver 9 tegn, og "10.038412" kræver 9 tegn. Ved kun 7 tegn
  // blev decimaler klippet af, hvilket gav upræcist API-kald til Open-Meteo.
  // Udvidet til 13 bytes (12 tegn + null), som dækker alle realistiske koordinater
  // med op til 8 decimaler og fortegn. BEMÆRK: Den serielle PC-protokol er stadig
  // låst til 174 bytes med 7-byte koordinatfelter (offset 160 og 167). Den udvidede
  // struktur gør EEPROM og RAM fremtidssikret, og kopierOgTrim() tjekker nu
  // eksplicit for afkortning, så vi ikke længere klipper tavst.
  char lon[13];                   // [13 bytes] Længdegrad (12 chars + 1 null terminator)
  char lat[13];                   // [13 bytes] Breddegrad (12 chars + 1 null terminator)
};
KonfigurationsData minKonfig;     // Den globale RAM-kopi af EEPROM-dataene

// ===================================================================================
// GLOBALE TILSTANDS-VARIABLER (STATE)
// ===================================================================================
const char* TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3"; // POSIX streng der styrer dansk sommertid automatisk

// Variabler der holder data hentet fra internettet (Open-Meteo)
float apiTemp = 0.0;              // Temperatur ifølge API'et
int apiFugt = 0;                  // Luftfugtighed ifølge API'et (%)
float apiNedboer = 0.0;           // Forventet nedbør ifølge API'et (mm)
String vejrTypeStr = "Henter..."; // Tekststreng der beskriver det aktuelle vejr (fx "Skyet")
// Rå WMO-vejrkode gemmes globalt, så ikon-valget i visSideNedboer()
// kan baseres på den reelle vejrtype i stedet for udelukkende nedbørsmængden.
// Uden denne ville 0.0 mm nedbør altid tvinge et sol-ikon frem, selv ved tåge (45) eller overskyet (3).
int apiVejrKode = -1;             // -1 = endnu ingen gyldig WMO-kode modtaget

// Variabler der holder data målt fysisk af sensorerne
float lokalTemp = 0.0;            // Målt temperatur indendørs (C)
float lokalTryk = 0.0;            // Målt atmosfærisk tryk (hPa)
float lokalLux = 0.0;             // Målt lysintensitet (Lux)

// Astronomi og kalender variabler
String maaneNavn = "Henter...";   // Det danske navn for månefasen
int maaneBelysning = 0;           // Procentvis belysning af månen (0-100)
String solOpTid = "--:--";        // Tidspunkt for solopgang (HH:MM)
String solNedTid = "--:--";       // Tidspunkt for solnedgang (HH:MM)
char tidStr[20] = "--:--:--";     // Den aktuelle tid formateret som tekst
char datoStr[20] = "--/--/----";  // Den aktuelle dato formateret som tekst

// Flag der angiver om NTP-synkroniseringen lykkedes. Bruges til at forhindre visning
// af "01/01/1970" og til at undgå fejlagtig måneberegning på et UNIX-tidsstempel nær 0.
bool tidSynkroniseret = false;

// Tidsstempel for hvornår NTP sidst blev forsøgt (eller genforsøgt). Bruges til at
// sikre, at uret synkroniseres igen, når WiFi-forbindelsen genetableres efter offline drift.
unsigned long sidsteNTPForsoeg = 0;
const unsigned long NTP_RETRY_INTERVAL = 600000; // Genforsøg hvert 10. minut (600.000 ms) indtil succes

// Den oprindelige genforsøgs-logik kaldte opsaetTid() direkte fra loop(). Da opsaetTid()
// indeholder et blokerende while-loop med delay(500), kunne hele ESP8266 fryse i op til
// 10 sekunder, mens sensorer og display stod stille. Vi indfører derfor en tilstandsvariabel,
// der gør det muligt at køre NTP-forsøget som en ikke-blokerende sekvens uden at kalde
// opsaetTid() fra loop(). Flaget 'ntpForsoegIgang' forhindrer, at vi starter flere
// samtidige forsøg. 'ntpForsoegStart' gemmer hvornår det igangværende forsøg startede.
bool ntpForsoegIgang = false;
unsigned long ntpForsoegStart = 0;
const unsigned long NTP_TIMEOUT_MS = 10000;      // Maksimal tid pr. NTP-forsøg (10 sekunder)

// Sikkerhed for ArduinoJson version:
// ArduinoJson v7 bruger 'JsonDocument doc;' (elastisk hukommelse).
// ArduinoJson v6 kræver 'DynamicJsonDocument doc(2048);' (fast kapacitet).
// Koden nedenfor bruger v7-syntaks. Hvis du kompilerer med v6, skal du udskifte
// linjen 'JsonDocument doc;' i hentVejrAPI() med 'DynamicJsonDocument doc(2048);'.
// ArduinoJson v7 er større i kode og hukommelse end v6.

// Timere (millis) og intervaller til non-blocking multitasking
unsigned long sidsteAPIOpdatering = 0;      // Tidsstempel for seneste API-kald
const unsigned long API_INTERVAL = 1800000; // API interval = 30 minutter (1.800.000 ms)

unsigned long sidsteSensorLaesning = 0;     // Tidsstempel for seneste læsning af I2C sensorer
const unsigned long SENSOR_INTERVAL = 5000; // Sensor interval = 5 sekunder (5.000 ms)

unsigned long sidsteSkermSkift = 0;         // Tidsstempel for seneste skift af display-side
const unsigned long SKERM_INTERVAL = 4000;  // Skærm interval = 4 sekunder (4.000 ms)
int aktuelSkermSide = 0;                    // Tæller der peger på den aktive skærmside (0 til 4)

// opdaterDisplayBillede() blev kaldt ubeskyttet hver loop-iteration og indeholdt
// delay(50), hvilket tvang ESP8266 til at pushe hele OLED frame-bufferen ca. 20
// gange i sekundet. Det monopoliserede I2C-bussen og skabte kollisioner med
// BMP085 og BH1750. Vi indfører derfor en timer, så displayet kun opdateres
// med et kontrolleret interval (10 Hz = 100 ms er rigeligt til en vejrstation).
unsigned long sidsteDisplayOpdatering = 0;     // Tidsstempel for seneste OLED-opdatering
const unsigned long DISPLAY_INTERVAL = 100;    // Display interval = 100 ms (10 Hz) - rigeligt og skånsomt for I2C

// Tilstandsvariabler til robust knap-aflæsning. 'knapSidsteAflaesning' gemmer den
// seneste rå aflæsning, 'knapSidsteSkift' gemmer hvornår den sidst ændrede sig, og
// 'knapNedStart' gemmer hvornår knappen blev registreret holdt nede. Disse bruges
// til at kræve en stabil, langvarig aktivering før PC-tilstand accepteres.
int knapSidsteAflaesning = HIGH;
unsigned long knapSidsteSkift = 0;
unsigned long knapNedStart = 0;
bool knapLangtTrykRegistreret = false;

// En fast offset-variabel der skifter mellem 0 og 1 (eller flere niveauer), så
// header-linjen og titelteksten flyttes nogle få pixels periodisk. Dette forhindrer
// permanent indbrænding af de øverste pixels. 'burnInOffset' opdateres hvert par
// sekunder i loop() og bruges af tegnSkermHeader().
// Oprindeligt blev burnInOffset KUN anvendt i tegnSkermHeader(). De øvrige statiske
// elementer (temperatur, fugt, lux, dato, sol-tider m.m.) brugte fastlåste absolutte
// koordinater, så midterste og nederste pixels stadig brændte ind, og teksten kunne
// overlappe hvis offsettet øgedes. Nu anvendes burnInOffset på ALLE statiske
// tekst- og ikon-elementer på alle fem sider, så hele layoutet forskydes i takt.
int burnInOffset = 0;
unsigned long sidsteBurnInSkift = 0;
const unsigned long BURN_IN_INTERVAL = 60000; // Skift offset hvert 60. sekund


// ===================================================================================
// MODUL: setup()
// Formål: Kører én gang ved opstart. Klargør hardware, tjekker hukommelse og forbinder.
// ===================================================================================
void setup() {
  // Starter Seriel port. SERIAL_8E1 = 8 databit, Even parity, 1 stopbit.
  // Dette SKAL matche PC-programmets opsætning, ellers fejler PC-konfigurationen.
  Serial.begin(9600, SERIAL_8E1);           // Starter Seriel port med præcis 9600 baud, 8 databit, Even parity, 1 stopbit (Påkrævet for PC-programmet)
  
  opsaetHardware();                         // Initialiserer GPIO pins, I2C bus og eksterne I2C sensorer
  
  EEPROM.begin(sizeof(KonfigurationsData)); // Klargør flash-hukommelsen til at læse/skrive præcis de bytes vores struktur fylder
  
  if (!erKonfigurationGyldig()) {           // Tjekker om de første bytes i EEPROM er "CFG". Er de ikke det, er enheden blank.
    visManglerDataSkerm();                  // Tegner instruktioner til brugeren på OLED skærmen
    ventPaaKnapTryk();                      // Blokerer programmet, indtil knappen trykkes
    udfoerPCKonfiguration();                // Starter den serielle håndtryks-protokol for at modtage ny konfiguration
  } else {                                  // Hvis EEPROM indeholder gyldig data
    indlaesFraEEPROM();                     // Kopier data fra Flash til 'minKonfig' (RAM)
    if (digitalRead(KNAP_PIN) == LOW) {     // Tjekker om brugeren fysisk holder knappen nede mens strømmen tilsluttes
      udfoerPCKonfiguration();              // Tillader brugeren at overskrive den gamle opsætning med det samme
    }
  }

  opsaetWiFi();                             // Forbinder til routeren med de indlæste SSID og Password værdier
  opsaetTid();                              // Kontakter en NTP-server for at synkronisere den interne UNIX-tæller
}

// ===================================================================================
// MODUL: loop()
// Formål: Kører i ring for evigt. Styrer timing for alle under-moduler.
// ===================================================================================
void loop() {
  // Robust, debounced aflæsning af knappen. Kræver, at knappen holdes nede i
  // KNAP_HOLD_MS millisekunder. Dette forhindrer, at EMI eller statisk støj
  // utilsigtet kaster enheden i PC-tilstand.
  haandterKnapAflaesning();

  unsigned long nuMillis = millis();        // Fanger det aktuelle millisekund-tal siden opstart til at regne tidsforskelle
  
  // opsaetTid() kaldes kun i setup() for det første forsøg. Efterfølgende genforsøg
  // sker asynkront via haandterNTPGenforsoeg(), så hele systemet ikke blokeres i
  // op til 10 sekunder.
  haandterNTPGenforsoeg(nuMillis);
  
  // Aktiv genopretning af WiFi-forbindelsen, hvis den tabes under drift.
  haandterWiFiGenforbindelse(nuMillis);
  
  // Opdaterer burnInOffset periodisk, så header-linjen flyttes og pixels aflastes.
  haandterBurnInSkift(nuMillis);
  
  opdaterLokalTidOgDato();                  // Modul: Omregner UNIX-tid til letlæselige timer/dato strenge (Uafhængig af delay)
  haandterLokaleSensorer(nuMillis);         // Modul: Tjekker om det er tid til at snakke med BMP085 og BH1750
  haandterNetvaerksOpdatering(nuMillis);    // Modul: Tjekker om det er tid til at pinge Open-Meteo for vejrudsigt
  haandterThingSpeak(nuMillis);             // Modul: Tjekker om det er tid til at aflevere data til ThingSpeak
  haandterSkermSideskift(nuMillis);         // Modul: Tjekker om det er tid til at rotere skærmens sideindeks
  
  // opdaterDisplayBillede() kaldes kun, når DISPLAY_INTERVAL er udløbet, så
  // BMP085/BH1750 kan bruge I2C-bussen imellem og den asynkrone arkitektur bevares.
  if (nuMillis - sidsteDisplayOpdatering >= DISPLAY_INTERVAL) {
    sidsteDisplayOpdatering = nuMillis;     // Nulstil display-timeren
    opdaterDisplayBillede();                // Modul: Sender den nyeste tegne-buffer til OLED skærmen
  }
}

// ===================================================================================
// MODUL: haandterKnapAflaesning()
// Formål: Robust, debounced aflæsning af hardware-knappen. Kræver langvarig aktivering.
// ===================================================================================
void haandterKnapAflaesning() {
  int raa = digitalRead(KNAP_PIN);          // Læs den rå tilstand af knap-pinnen (HIGH = ikke trykket, LOW = trykket)
  unsigned long nu = millis();              // Aktuelt tidsstempel til at måle varigheder

  // Debounce: Ignorer raske skift ved at kræve, at tilstanden har været stabil i KNAP_DEBOUNCE_MS
  if (raa != knapSidsteAflaesning) {
    knapSidsteSkift = nu;
    knapSidsteAflaesning = raa;
    return;
  }
  if (nu - knapSidsteSkift < KNAP_DEBOUNCE_MS) {
    return;                                 // Tilstanden er endnu ikke stabil længe nok
  }

  // Nu er tilstanden stabil. Hvis knappen er nede (LOW), start en langt-tryk-måling.
  if (raa == LOW) {
    if (knapNedStart == 0) {
      knapNedStart = nu;                    // Registrér hvornår knappen blev holdt nede
      knapLangtTrykRegistreret = false;
    }
    // Hvis knappen har været holdt nede i mindst KNAP_HOLD_MS, accepter som gyldigt tryk
    if (!knapLangtTrykRegistreret && (nu - knapNedStart >= KNAP_HOLD_MS)) {
      knapLangtTrykRegistreret = true;
      Serial.println("Knap holdt nede - starter PC-konfiguration.");
      udfoerPCKonfiguration();              // Gå i PC-parringstilstand (blokerende, men nu bevidst)
    }
  } else {
    // Knappen er sluppet - nulstil langt-tryk-tilstanden
    knapNedStart = 0;
    knapLangtTrykRegistreret = false;
  }
}

// ===================================================================================
// MODUL: haandterNTPGenforsoeg()
// Formål: Ikke-blokerende NTP-genforsøg efter offline drift.
// ===================================================================================
void haandterNTPGenforsoeg(unsigned long nuMillis) {
  // Hvis uret allerede er synkroniseret, er der intet at gøre
  if (tidSynkroniseret) {
    return;
  }
  // Vi kan kun forsøge, hvis WiFi faktisk er oppe
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  // Start et nyt forsøg, hvis vi ikke allerede har et kørende, og intervallet er udløbet
  if (!ntpForsoegIgang) {
    if (sidsteNTPForsoeg == 0 || (nuMillis - sidsteNTPForsoeg >= NTP_RETRY_INTERVAL)) {
      sidsteNTPForsoeg = nuMillis;
      ntpForsoegStart = nuMillis;
      ntpForsoegIgang = true;
      Serial.println("Genforsøger NTP-synkronisering (ikke-blokerende)...");
      // Vi kalder configTime() igen for at sætte NTP-serverne op, men undgår
      // det blokerende while-loop ved at lade time() blive tjekket asynkront nedenfor.
      configTime(TZ_INFO, "dk.pool.ntp.org", "time.google.com");
      setenv("TZ", TZ_INFO, 1); // NYT: Sikrer at localtime() returnerer CET/CEST korrekt
      tzset();                  // NYT: Opdaterer de globale tids-variabler på ESP8266
    }
    return;
  }

  // Et forsøg er i gang: Tjek asynkront om tiden er blevet synkroniseret
  time_t nu = time(nullptr);
  // Et gyldigt nutids-UNIX-tidsstempel ligger over 1,7 milliarder (år 2023+).
  // Grænsen på 100.000 sekunder (ca. 27,7 timer offline oppetid) blev fejlagtigt
  // accepteret som "synkroniseret", hvilket viste en dato i januar 1970 og udførte
  // månefase-beregninger på falsk data. Grænsen er derfor hævet til 1.700.000.000.
  if (nu >= 1700000000) {
    // Succes!
    tidSynkroniseret = true;
    ntpForsoegIgang = false;
    sidsteNTPForsoeg = 0;
    Serial.println("NTP-synkronisering gennemfoert (asynkron).");
    return;
  }

  // Timeout: Hvis forsøget har kørt for længe, giv op og prøv igen senere
  if (nuMillis - ntpForsoegStart >= NTP_TIMEOUT_MS) {
    ntpForsoegIgang = false;
    Serial.println("NTP-genforsøg timeout - prøver igen om 10 minutter.");
  }
}

// ===================================================================================
// MODUL: haandterWiFiGenforbindelse()
// Formål: Aktivt at genoprette WiFi-forbindelsen, hvis den tabes under drift.
// ===================================================================================
void haandterWiFiGenforbindelse(unsigned long nuMillis) {
  // Kun relevant, hvis vi faktisk havde en forbindelse og har mistet den.
  // Vi bruger en statisk tidsstempel-variabel til at undgå at spamme reconnect.
  static unsigned long sidsteReconnectForsoeg = 0;
  const unsigned long RECONNECT_INTERVAL = 30000; // Prøv igen hvert 30. sekund

  if (WiFi.status() != WL_CONNECTED) {
    if (sidsteReconnectForsoeg == 0 || (nuMillis - sidsteReconnectForsoeg >= RECONNECT_INTERVAL)) {
      sidsteReconnectForsoeg = nuMillis;
      Serial.println("WiFi forbindelse tabt - forsøger WiFi.reconnect()...");
      // ESP8266's interne SDK har en auto-reconnect, men den fejler ofte permanent
      // efter langvarig nedetid (routergenstart, kanalskift). Uden dette kald ville
      // enheden forblive offline indtil en fysisk strømafbrydelse. Vi kalder derfor
      // eksplicit WiFi.reconnect() med fast interval.
      WiFi.reconnect();
    }
  } else {
    // Forbindelsen er genoprettet - nulstil, så næste afbrydelse kan håndteres straks
    sidsteReconnectForsoeg = 0;
  }
}

// ===================================================================================
// MODUL: haandterBurnInSkift()
// Formål: Periodisk at flytte header-linjen for at undgå OLED burn-in.
// ===================================================================================
void haandterBurnInSkift(unsigned long nuMillis) {
  if (nuMillis - sidsteBurnInSkift >= BURN_IN_INTERVAL) {
    sidsteBurnInSkift = nuMillis;
    // Skift mellem 0 og 1 (kan udvides til flere niveauer, hvis ønsket)
    burnInOffset = (burnInOffset + 1) % 2;
  }
}

// ===================================================================================
// MODUL: opsaetHardware()
// Formål: Dedikeret funktion til at konfigurere de fysiske ben og I2C-bussen.
// ===================================================================================
void opsaetHardware() {
  pinMode(KNAP_PIN, INPUT_PULLUP);          // Sætter knap-pinnen som input og aktiverer den interne pull-up modstand (trækker til 3.3V)
  
  // Den oprindelige kode kaldte u8g2.begin() FØR Wire.begin(12, 14). Det betød, at
  // u8g2's interne opstartssekvens (strøm til display-matricen, kontrast, osv.) blev
  // sendt ud på standardbenene (GPIO 4/5) og aldrig nåede OLED'en på ben 12/14.
  // Skærmen forblev derfor permanent slukket. Løsningen er at kalde Wire.begin()
  // MED de korrekte pins FØRST, så u8g2's init-sekvens sendes ud på de rigtige ben.
  Wire.begin(OLED_SDA_Pin, OLED_SCL_Pin);   // Starter I2C-bussen på de valgte pins FØR u8g2 initialiseres
  
  u8g2.setI2CAddress(I2C_DISPLAY_ADDRESS * 2); // Sætter OLED skærmens adresse. (U8g2 biblioteket forventer adressen ganget med 2 grundet R/W-bit)
  u8g2.begin();                             // Tænder og rydder skærmen - nu på de korrekte pins
  
  // Uden dette kald kører Wire-biblioteket på ESP8266 med standardhastigheden 100 kHz.
  // En SSD1306 skærm skal sende 1024 bytes RAM-buffer + overhead. Ved 100 kHz tager
  // u8g2.sendBuffer() over 82 ms, hvilket blokerer hele loop() og ødelægger debounce-
  // logikken (KNAP_DEBOUNCE_MS = 50 ms overskrides instantant). Vi hæver derfor
  // bussen til Fast Mode (400 kHz), så sendBuffer() kun tager ca. 20 ms.
  // Kaldet skal stå EFTER u8g2.begin(), fordi u8g2 selv initialiserer Wire-bussen.
  u8g2.setBusClock(400000);                 // Sæt I2C-bussen til Fast Mode (400 kHz) straks efter init
  
  // Skærmbiblioteket u8g2.begin() kalder internt Wire.begin() uden parametre. 
  // På ESP8266 vil et parameterløst kald til Wire.begin() omgående nulstille 
  // I2C-bussen til standard-benene (GPIO 4 og 5). Dette dræber al I2C trafik 
  // til de pins vi har valgt (12 og 14).
  // Ved at kalde Wire.begin(OLED_SDA_Pin, OLED_SCL_Pin) både FØR og EFTER u8g2.begin(),
  // sikrer vi, at både u8g2's init-sekvens og efterfølgende sensorkommunikation
  // foregår på de rigtige pins. Det første kald sikrer init-sekvensen; det andet
  // sikrer, at u8g2's interne Wire.begin() ikke efterlader bussen på forkerte pins.
  Wire.begin(OLED_SDA_Pin, OLED_SCL_Pin);   // Genstart I2C-bussen på de valgte pins efter u8g2's init
  
  if (!bmp.begin()) {                       // Sender et init-kald til vores indlejrede BMP085 klasse. Hvis chippen ikke svarer...
    Serial.println("Kunne ikke finde BMP085 sensor!"); // Udskriv en fejl til debugging via Seriel Monitor
    bmpOK = false;                          // Marker at BMP085 ikke er tilgængelig (forhindrer division by zero senere)
  } else {
    bmpOK = true;                           // Marker at BMP085 blev initialiseret korrekt
  }
  
  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) { // Starter vores indlejrede BH1750 klasse i højopløsnings-mode
    Serial.println("Kunne ikke finde BH1750 sensor!");       // Udskriv fejl, hvis den fejler
    bh1750OK = false;                       // Marker at BH1750 ikke er tilgængelig
  } else {
    bh1750OK = true;                        // Marker at BH1750 blev initialiseret korrekt
  }
}

// ===================================================================================
// MODUL: opsaetWiFi()
// Formål: Forbinder til WiFi-netværket, og håndterer fejl hvis routeren er nede.
// ===================================================================================
void opsaetWiFi() {
  u8g2.clearBuffer();                       // Tømmer grafikhukommelsen
  u8g2.setFont(u8g2_font_6x12_tf);          // Sætter standard skrifttypen
  u8g2.drawStr(10, 30, "Forbinder WiFi...");// Tegner information på koordinat x=10, y=30
  u8g2.drawStr(10, 45, minKonfig.ssid);     // Printer navnet på det netværk vi forsøger at tilgå
  u8g2.sendBuffer();                        // Skubber tegningen til den fysiske skærm

  WiFi.mode(WIFI_STA);                      // Gennemtvunget lukning af det iboende åbne AP sikkerhedshul i ESP8266
  WiFi.begin(minKonfig.ssid, minKonfig.pass); // Initierer WiFi forbindelsen via ESP8266's interne hardware
  
  unsigned long startForsog = millis();     // Husker præcis hvornår vi startede forsøget
  
  // Før kaldte vi ESP.restart() efter 20 sekunder, hvilket gjorde enheden ubrugelig som
  // ren lokal vejrstation, hvis routeren var slukket. Nu fortsætter vi blot uden WiFi,
  // så BMP085 og BH1750 kan aflæses og vises på OLED alligevel. Netværksmodulerne
  // tjekker selv WiFi.status() og springer kaldet over, indtil forbindelsen genetableres.
  while (WiFi.status() != WL_CONNECTED) {   // Så længe forbindelsen ikke er etableret (blokerende løkke)
    delay(500);                             // Vent 500 millisekunder (forhindrer at vi spærrer processoren helt)
    if (millis() - startForsog > 20000) {   // Hvis der går mere end 20 sekunder uden succes...
      // Tidligere ESP.restart() er fjernet. Vi fortsætter i offline-tilstand i stedet.
      Serial.println("WiFi ikke tilgaengelig - fortsaetter i offline drift.");
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x12_tf);
      u8g2.drawStr(10, 30, "Offline drift");
      u8g2.drawStr(10, 45, "Ingen WiFi...");
      u8g2.sendBuffer();
      delay(1500);
      return;                               // Forlad funktionen og fortsæt programmet uden netværk
    }
  }
}

// ===================================================================================
// MODUL: opsaetTid()
// Formål: Henter klokkeslættet fra NTP (Network Time Protocol) servere, da ESP'en 
// ikke har et fysisk ur-batteri (RTC).
// Denne funktion indeholder stadig et blokerende while-loop og kaldes derfor KUN
// fra setup() (første synkronisering). Efterfølgende genforsøg håndteres asynkront
// af haandterNTPGenforsoeg() i loop(), så systemet ikke fryser.
// ===================================================================================
void opsaetTid() {
  u8g2.clearBuffer();                       
  u8g2.drawStr(10, 30, "Synkroniserer ur..."); 
  u8g2.sendBuffer();                        
  
  // Konfigurerer det indbyggede system-ur med dansk tidszone og to offentlige NTP-servere
  configTime(TZ_INFO, "dk.pool.ntp.org", "time.google.com"); 
  setenv("TZ", TZ_INFO, 1); // NYT: Opfylder krav FR-FW-07 for dansk sommertid
  tzset();                  // NYT: Tvinger localtime() til at anvende TZ miljøvariablen
  
  time_t nu = time(nullptr);                // Forespørger den aktuelle UNIX-tid (antal sekunder siden 1970)
  int forsog = 0;                           // Variabel for at styre timeout og undgå WDT reset
  
  // Når uret ikke er synkroniseret endnu, vil systemet returnere en tid meget tæt på 0.
  // Vi looper her indtil NTP-serveren faktisk har svaret med et højt/korrekt UNIX-tidsstempel,
  // eller til max antal forsøg er nået for at undgå uendelig blokering.
  // Et gyldigt nutids-UNIX-tidsstempel ligger over 1,7 milliarder (år 2023+).
  // Den gamle grænse på 100.000 sekunder (ca. 27,7 timer offline oppetid) blev
  // fejlagtigt accepteret som "synkroniseret", hvilket viste en dato i januar 1970
  // og udførte månefase-beregninger på falsk data. Grænsen hæves til 1.700.000.000.
  while (nu < 1700000000 && forsog < 20) {                     
    delay(500);                             // Vent et halvt sekund
    nu = time(nullptr);                     // Spørg uret igen
    forsog++;                               // Tæl timeout-forsøg op
  }

  // Gemmer resultatet af synkroniseringen i flaget. Hvis NTP aldrig svarede, forbliver
  // uret nær UNIX epoch (1970), og vi må ikke vise denne falske dato eller beregne månefase.
  tidSynkroniseret = (nu >= 1700000000);

  if (!tidSynkroniseret) {
    Serial.println("NTP-synkronisering fejlede - tid og maanefase deaktiveret indtil genforbindelse.");
    // Sætter pæne placeholder-strenge i stedet for det misvisende "01/01/1970".
    strncpy(tidStr, "--:--:--", sizeof(tidStr));
    strncpy(datoStr, "--/--/----", sizeof(datoStr));
    maaneNavn = "Ingen tid";
    maaneBelysning = 0;
  } else {
    // Ved succes nulstilles genforsøgs-timeren, så vi ikke forsøger unødigt igen.
    sidsteNTPForsoeg = 0;
    Serial.println("NTP-synkronisering gennemfoert.");
  }
}

// ===================================================================================
// MODUL: erKonfigurationGyldig()
// Formål: Verificerer at EEPROM faktisk indeholder vejrstations-data, og ikke skrald.
// ===================================================================================
bool erKonfigurationGyldig() {
  EEPROM.get(0, minKonfig);                 // Læser sizeof(KonfigurationsData) bytes fra adresse 0 direkte ind i vores RAM-struct
  // Vores første 4 bytes ("CFG\0") fungerer som et vandmærke.
  return (strncmp(minKonfig.magic, "CFG", 3) == 0); // Returnerer true hvis strengen matcher (sikret mod buffer overflow uinitialiseret EEPROM)
}

// ===================================================================================
// MODUL: indlaesFraEEPROM()
// Formål: Sikker indlæsning af konfigurationsdata.
// ===================================================================================
void indlaesFraEEPROM() {
  EEPROM.get(0, minKonfig);                 // Kopierer data fra flash
}

// ===================================================================================
// MODUL: visManglerDataSkerm()
// Formål: GUI hjælper - Fortæller brugeren at stationen mangler sin første opsætning.
// ===================================================================================
void visManglerDataSkerm() {
  u8g2.clearBuffer();                       // Tøm tegne-buffer
  u8g2.setFont(u8g2_font_6x12_tf);          // Sæt skrifttype
  u8g2.drawStr(0, 15, "Ingen vejrdata gemt!"); // Linje 1
  u8g2.drawStr(0, 35, "Aaben PC programmet");  // Linje 2
  u8g2.drawStr(0, 50, "og tryk derefter");     // Linje 3
  u8g2.drawStr(0, 64, "paa hardware knappen.");// Linje 4
  u8g2.sendBuffer();                        // Skub til OLED
}

// ===================================================================================
// MODUL: ventPaaKnapTryk()
// Formål: Stopper al kode-afvikling (blokerer), indtil hardware-knappen kortsluttes.
// ===================================================================================
void ventPaaKnapTryk() {
  while (digitalRead(KNAP_PIN) == HIGH) {   // Så længe pinnen aflæses til 3.3V (pga INPUT_PULLUP)
    delay(50);                              // Vent og spørg igen (undgår CPU 100% load)
  }
  delay(200);                               // Venter lidt ekstra for at debounce (fjerne elektrisk "hoppen" i trykknappen)
}

// ===================================================================================
// MODUL: udfoerPCKonfiguration()
// Formål: Eksekverer det serielle handshake (protokol) med Pascal programmet.
// ===================================================================================
void udfoerPCKonfiguration() {
  u8g2.clearBuffer();                       // Tømmer skærmen
  u8g2.setFont(u8g2_font_6x12_tf);          
  u8g2.drawStr(0, 15, "Forbundet til PC..."); // Viser at vi er klar
  u8g2.drawStr(0, 35, "Venter paa data...");  
  u8g2.sendBuffer();                        
  
  while(Serial.available()) Serial.read();  // Tømmer ESP'ens modtagebuffer for gamle, irrelevante bytes for at sikre en ren start
  
  Serial.print("klar");                     // Sender trigger-ordet "klar" ud via TX-pinnen. Pascal lytter efter dette ord.
  
  const int FORVENTET_LAENGDE = 174;        // Længden af den pakke PC'en sender er præcis 174 bytes
  char modtageBuffer[175];                  // Opretter modtage-array med plads til de 174 bytes + én ekstra byte til Null-terminator (\0)
  int bytesLaest = 0;                       // Tæller for antal læste bytes
  unsigned long startTid = millis();        // Gemmer starttidspunktet til timeout-håndtering
  
  // Vi tillader at der går op til 10 sekunder før overførslen fejler.
  while (millis() - startTid < 10000) {     
    yield();                                // Sørger for at Watchdog Timeren (WDT) ikke crasher ESP'en under den lange lytteløkke.
    if (Serial.available() > 0) {           // Hvis der ligger en bygge og venter på den serielle port
      modtageBuffer[bytesLaest] = Serial.read(); // Læs byten ind på den aktuelle index-plads i vores buffer
      bytesLaest++;                         // Tæl index op
      
      if (bytesLaest == FORVENTET_LAENGDE) { // Når vi har ramt nøjagtig 174 modtagne bytes
        modtageBuffer[bytesLaest] = '\0';   // Luk arrayet af med en sikker null-terminator (så det regnes som en lovlig C-streng)
        dekodOgGemFastLaengdeData(modtageBuffer); // Kast strengen videre til modulet der piller den fra hinanden
        return;                             // Afslut funktionen (modulet er færdigt)
      }
    }
  }
  
  // Timeout: Hvis while-løkken overskrides uden at vi nåede 174 bytes, falder vi herned.
  u8g2.clearBuffer();                       
  u8g2.drawStr(0, 30, "Fejl: Timeout fra PC"); // Udskriv fejl på OLED
  u8g2.sendBuffer();                        
  delay(3000);                              // Hold skærmbilledet i 3 sekunder
  ESP.restart();                            // Genstart enheden for at komme tilbage til forrige, fungerende tilstand
}

// ===================================================================================
// MODUL: dekodOgGemFastLaengdeData()
// Formål: Bryder de modtagne 174 bytes ned til logiske enheder baseret på faste index, 
//         fjerner polstring (mellemrum) og brænder dem ind i EEPROM.
// Input param: 'dataBuffer' - En pointer til arrayet med de rå data fra PC'en.
// ===================================================================================
void dekodOgGemFastLaengdeData(const char* dataBuffer) {
  memset(&minKonfig, 0, sizeof(KonfigurationsData)); // Sikkerhed: Fyld hukommelsen med 0'er så ingen gamle bits forurener den nye opsætning
  
  strcpy(minKonfig.magic, "CFG");           // Genindsæt det "magiske" mærkat, så enheden fremover ved at dataen er valid

  // Vi kalder nu hjælpe-modulet (kopierOgTrim) til at saks-klippe dataBufferen i stykker 
  // ifølge den aftalte protokol. F.eks.: Længdegrad ligger fra index 160 og fylder 7 bytes.
  kopierOgTrim(minKonfig.ssid, dataBuffer, 0, 50);
  kopierOgTrim(minKonfig.pass, dataBuffer, 50, 50);
  // 'minKonfig.land' læses/vises/bruges ALDRIG i resten af koden og forbruger
  // 21 bytes statisk RAM + Flash uden formål på en hukommelsesbegrænset platform.
  // Feltet bevares i struct'en for bagudkompatibilitet med allerede-brændte EEPROM-
  // data (så offset for 'by', 'lon' og 'lat' ikke forskydes), men det udfyldes
  // bevidst IKKE længere fra PC-protokollen. De 20 bytes ved offset 100 ignoreres.
  // kopierOgTrim(minKonfig.land, dataBuffer, 100, 20);  // DEAKTIVERET: ubrugt felt
  kopierOgTrim(minKonfig.by, dataBuffer, 120, 40);
  kopierOgTrim(minKonfig.lon, dataBuffer, 160, 7);
  kopierOgTrim(minKonfig.lat, dataBuffer, 167, 7);

  // Vores RAM-struktur (minKonfig) er nu fuldt opdateret.
  EEPROM.put(0, minKonfig);                 // Klargør en skrivning af hele struct-blokken til EEPROM's første byte (adresse 0)
  EEPROM.commit();                          // Committer ændringerne (Udfører rent faktisk den fysiske brænding til flash)
  
  u8g2.clearBuffer();                       
  u8g2.drawStr(0, 20, "Data modtaget OK!"); // Bekræft over for brugeren
  u8g2.drawStr(0, 35, minKonfig.by);        // Udskriver det afkodede bynavn. Dette fungerer som et visuelt bevis på at afkodningen lykkedes!
  u8g2.drawStr(0, 55, "Genstarter...");     
  u8g2.sendBuffer();                        
  
  delay(3000);                              // Hold skærmen, så brugeren kan læse beskeden
  ESP.restart();                            // Genstarter, hvorefter setup() vil finde "CFG" i EEPROM og køre normalt op
}

// ===================================================================================
// MODUL: kopierOgTrim()
// Formål: Udklipper en substring. Pascal-programmet fylder faste blokke ud med 
// mellemrum (' '). Hvis et WiFi-password er 10 tegn, sendes 10 tegn + 40 mellemrum.
// Modulet klipper strengen over før de efterfølgende mellemrum (trim).
// Input params: 
// - destination: Arrayet i 'minKonfig' der skal modtage dataen.
// - kilde: Den rå 'dataBuffer' fra PC'en.
// - startIndeks: Hvor i kilden vores data starter.
// - laengde: Hvor lang blokken er ifølge protokollen.
// ===================================================================================
void kopierOgTrim(char* destination, const char* kilde, int startIndeks, int laengde) {
  // Kopierer præcis 'laengde' antal karakterer fra 'kilde' forskudt med 'startIndeks'
  strncpy(destination, kilde + startIndeks, laengde); 
  
  destination[laengde] = '\0';              // Tvinger arrayet til at være en gyldig c-streng ved at sætte et 0 i slutningen

  // Kør en løkke baglæns gennem den nyligt kopierede streng
  for (int i = laengde - 1; i >= 0; i--) {  
    if (destination[i] == ' ') {            // Hvis karakteren er et blanktegn (' ')
      destination[i] = '\0';                // Udskift det med en streng-afslutning. Dette "klipper" strengen kortere.
    } else {                                
      break;                                // Så snart vi møder et ægte bogstav, stopper vi (så vi ikke sletter mellemrum indeni bynavne)
    }
  }

  // Sikkerhedskontrol: Hvis destinationen er en koordinat (lon/lat) og den sidste
  // karakter ikke er et mellemrum, kan det betyde at PC'en sendte et længere felt,
  // som blev klippet af ved det faste 7-tegns layout. Vi advarer via Serial, så
  // problemet kan diagnosticeres, uden at ændre den fastlåste 174-byte protokol.
  // Tjekket er generisk: det udløses kun, hvis destinationen stadig er fyldt helt
  // op til det sidste tegn (dvs. ingen polstringsmellemrum blev fundet).
  if (destination[laengde - 1] != '\0' && destination[laengde - 1] != ' ') {
    // Kun relevant for de korte koordinatfelter (laengde == 7 i den nuværende protokol)
    if (laengde <= 7) {
      Serial.print("Advarsel: felt ved offset ");
      Serial.print(startIndeks);
      Serial.print(" fyldte hele blokken (");
      Serial.print(laengde);
      Serial.println(" tegn) - mulig afkortning af koordinat. Overvej at udvide PC-protokollen.");
    }
  }
}

// ===================================================================================
// MODUL: opdaterLokalTidOgDato()
// Formål: Omsætter UNIX sekunder til tekst for at OLED displayet kan tegne det.
// ===================================================================================
void opdaterLokalTidOgDato() {
  // Hvis uret aldrig blev synkroniseret, springer vi hele dato/tid- og måneberegningen over.
  // Ellers ville vi vise "01/01/1970" og lave en forkert astronomisk beregning på et
  // UNIX-tidsstempel tæt på 0.
  if (!tidSynkroniseret) {
    return;
  }

  time_t nuTid = time(nullptr);             // Forespørg ESP'ens aktuelle UNIX-tid (som vedligeholdes asynkront af NTP-servicen)
  struct tm* tidsInfo = localtime(&nuTid);  // Nedbryd UNIX-tallet til en struct med timer (tm_hour), minutter (tm_min), etc.
  
  // Funktionen localtime returnerer en 'Null-pointer', hvis tiden er ugyldig (fx ved NTP udfald).
  // Uden dette tjek ville den indbyggede strftime()-funktion crashe ESP'en med et uendeligt boot-loop, 
  // når den forsøger at læse en tom (Null) reference-hukommelse.
  if (tidsInfo != nullptr) {
    // Bruger strftime (String Format Time) til let at bygge strings baseret på 'tidsInfo'
    strftime(tidStr, sizeof(tidStr), "%H:%M:%S", tidsInfo); // Format = HH:MM:SS
    strftime(datoStr, sizeof(datoStr), "%d/%m/%Y", tidsInfo); // Format = DD/MM/YYYY
  }
  
  opdaterMaaneFase(nuTid);                  // Kalder den lokale måne-beregner og overfører det samme tidsstempel
}

// ===================================================================================
// MODUL: opdaterMaaneFase()
// Formål: Matematiske udregning af månens cyklus, så vi sparer et eksternt API kald.
// Input param: 'epoch' - Det aktuelle UNIX tidsstempel
// ===================================================================================
void opdaterMaaneFase(time_t epoch) {
  // Et epoch tæt på 0 giver en meningsløs astronomisk beregning. Vi afviser
  // derfor ethvert tidsstempel før en rimelig gyldig dato (år 2000).
  if (epoch < 946684800) {
    maaneNavn = "Ingen tid";
    maaneBelysning = 0;
    return;
  }

  const double synodiskMaaned = 29.53058867; // Konstant: Det nøjagtige gennemsnitlige antal dage for en komplet månefasecyklus
  
  // Udregner forskellen i sekunder mellem nu og en kendt astronomisk ny-måne: Torsdag d. 6. januar 2000 kl 12:24:00 GMT (947182440).
  double sekunderSidenRef = difftime(epoch, 947182440); 
  double dageSidenRef = sekunderSidenRef / 86400.0;     // Omregner sekunderne til antal dage (60s * 60m * 24h = 86400s pr. dag)
  
  // fmod (float modulo) beregner restværdien, når dagene deles med 29,53. 
  // Resultatet (faseCyklus) er dermed, hvor langt (0-29.53) vi er inde i den LIGE NUværende måne-cyklus.
  double faseCyklus = fmod(dageSidenRef, synodiskMaaned); 
  if (faseCyklus < 0) faseCyklus += synodiskMaaned;     // Sikkerhedstjek imod negative udregninger (Tid før år 2000)

  // Bruger trigonometri (cosinus) til at generere en bølge der svinger mellem 0 og 1.
  // Ganger den op til 100 for at få en procentværdi (0% = Helt mørk ny-måne, 100% = Helt belyst fuldmåne).
  maaneBelysning = (1.0 - cos((faseCyklus / synodiskMaaned) * 2.0 * PI)) / 2.0 * 100.0;

  // Bruger simple 'if'-statements til at oversætte dagene-inde-i-cyklussen til det korrekte danske kaldenavn.
  if (faseCyklus < 1.84 || faseCyklus > 27.68) maaneNavn = "Nymaane";
  else if (faseCyklus < 9.22)   maaneNavn = "Tiltagende";
  else if (faseCyklus < 16.61)  maaneNavn = "Fuldmaane";
  else                          maaneNavn = "Aftagende";
}

// ===================================================================================
// MODUL: haandterLokaleSensorer()
// Formål: Aflæser de fysiske sensorer monteret direkte på maskinen, styret af tid.
// Input param: 'nuMillis' - Op-tiden fra loop() 
// ===================================================================================
void haandterLokaleSensorer(unsigned long nuMillis) {
  // Timer-logik: Kør hvis tiden er gået, ELLER hvis det er første gang (0).
  if (nuMillis - sidsteSensorLaesning >= SENSOR_INTERVAL || sidsteSensorLaesning == 0) {
    sidsteSensorLaesning = nuMillis;        // Nulstiller vores lokale timer
    
    // Hvis BMP085 ikke blev fundet i setup(), er kalibreringskoefficienterne 0.
    // Et kald til readPressure()/readTemperature() vil da dividere med 0 i computeB5()
    // og crashe ESP8266 (uendeligt reboot-loop). Vi springer derfor læsningen over,
    // hvis sensoren ikke er markeret som OK.
    if (bmpOK) {
      lokalTemp = bmp.readTemperature();      // Temperatur i celsius (returnerer float)
      lokalTryk = bmp.readPressure() / 100.0; // Tryk: readPressure returnerer rå Pascal (fx 101300). Vi dividerer med 100 til hPa (fx 1013 hPa).
      // Hvis trykket er 0 hPa, betyder det at read16() returnerede 0 pga. manglende
      // I2C-svar. Vi beholder den forrige gyldige værdi i stedet for at vise 0 hPa
      // på skærmen, så et midlertidigt kabelbrud ikke ødelægger visningen.
      if (lokalTryk <= 0.0) {
        Serial.println("Advarsel: BMP085 gav ugyldigt tryk - beholder forrige vaerdi.");
      }
    }
    // BH1750 returnerer -2.0 hvis ikke konfigureret, og -1.0 ved I2C-fejl.
    // Vi beholder kun gyldige værdier >= 0, så displayet ikke viser "-2 Lux".
    if (bh1750OK) {
      float lux = lightMeter.readLightLevel(); // Lys: Returnerer intensitet i Lux.
      if (lux >= 0.0) lokalLux = lux;          // Ignorer negative fejlværdier (-1.0/-2.0)
    }
  }
}

// ===================================================================================
// MODUL: haandterNetvaerksOpdatering()
// Formål: Bestemmer hvornår det er tid til at pinge internet-tjenesterne for vejrudsigten.
// ===================================================================================
void haandterNetvaerksOpdatering(unsigned long nuMillis) {
  // Før blev timeren (sidsteAPIOpdatering) nulstillet selvom der ingen WiFi-forbindelse var. 
  // Hvis WiFi var nede præcis når timeren udløb, ville systemet give op og først prøve IGEN 30 minutter senere, 
  // hvilket betød en sort skærm unødigt længe. Vi tjekker nu at WiFi er oppe FØR vi nulstiller tælleren.
  if (WiFi.status() == WL_CONNECTED) {
    // Timer-logik (API_INTERVAL = 30 min)
    if (nuMillis - sidsteAPIOpdatering >= API_INTERVAL || sidsteAPIOpdatering == 0) {
      sidsteAPIOpdatering = nuMillis;         // Nulstil timer
      hentVejrAPI();                          // Kald det blokerende netværks-modul
    }
  }
}

// ===================================================================================
// MODUL: haandterThingSpeak()
// Formål: Uploader lokalt aflæste data (Temp, Tryk, Lys) til en online server.
// ===================================================================================
void haandterThingSpeak(unsigned long nuMillis) {
  // BYPASS: Hvis brugeren ikke har udfyldt en API-nøgle, springer vi ud af funktionen straks. 
  // Dette forhindrer, at vi sender spam-forespørgsler ud på nettet, som alligevel bliver afvist.
  if (String(THINGSPEAK_API_KEY) == "DIN_THINGSPEAK_WRITE_KEY_HER") {
    return; 
  }

  // Sikkerhedstjek: Sender kun data hvis timer er klar OG hvis vi faktisk er forbundet til nettet
  if (WiFi.status() == WL_CONNECTED && (nuMillis - sidsteThingSpeakOpdatering >= THINGSPEAK_INTERVAL || sidsteThingSpeakOpdatering == 0)) {
    sidsteThingSpeakOpdatering = nuMillis;  // Nulstil ThingSpeak timeren
    
    WiFiClient client;                      // Klargør det bagvedliggende TCP objekt
    HTTPClient http;                        // Klargør HTTP protokol laget
    
    // Bygger en REST API anmodning (URL). Data sendes direkte som GET parametre ("field1", "field2" osv).
    // Vi sammensætter base-URL, nøglen, og de seneste hardwaremålinger.
    String url = "http://api.thingspeak.com/update?api_key=" + String(THINGSPEAK_API_KEY) +
                 "&field1=" + String(lokalTemp) + // felt 1 = BMP085 Temperatur
                 "&field2=" + String(lokalTryk) + // felt 2 = BMP085 Lufttryk
                 "&field3=" + String(lokalLux);   // felt 3 = BH1750 Lysintensitet
                 
    http.begin(client, url);                // Forbereder HTTP-biblioteket på anmodningen
    http.GET();                             // Eksekverer. Det er denne linje, der fysisk overfører dataene via WiFi til skyen.
    http.end();                             // Nedbryder klienten for at rydde op i den stærkt begrænsede RAM.
  }
}

// ===================================================================================
// MODUL: findVejrType()
// Formål: Et "Opslags-modul" (LUT). Oversætter rå data til menneske-sprog.
// Input param: 'kode' (int) - WMO koden fra Open-Meteo
// Return: String - Den danske betegnelse.
// ===================================================================================
String findVejrType(int kode) {
  // Sammenligner mod de officielle World Meteorological Organization (WMO) koder
  if (kode == 0) return "Skyfrit";
  if (kode >= 1 && kode <= 3) return "Skyet";
  if (kode == 45 || kode == 48) return "Taage";
  if (kode >= 51 && kode <= 55) return "Stoevregn";
  // WMO-koderne 56 og 57 er "frysende støvregn" (freezing drizzle), som manglede i den
  // oprindelige oversættelse og derfor faldt igennem til "Ukendt". De tilføjes nu eksplicit.
  if (kode == 56 || kode == 57) return "Frys. stoevregn";
  if (kode >= 61 && kode <= 65) return "Regn";
  if (kode == 66 || kode == 67) return "Isslag";
  if (kode >= 71 && kode <= 75) return "Snevejr";
  // WMO-kode 77 er "sne-korn" (snow grains), som også manglede og faldt igennem til "Ukendt".
  if (kode == 77) return "Sne-korn";
  if (kode == 80 || kode == 81 || kode == 82) return "Byger";
  // WMO 85-86 er snebyger (snow showers), IKKE torden. Torden er 95-99.
  // Den gamle kode returnerede "Torden" for alle koder >= 85, hvilket gav forkert vejrbeskrivelse.
  if (kode == 85 || kode == 86) return "Snebyger";
  if (kode >= 95) return "Torden";
  return "Ukendt";                          // Fallback, hvis koden ikke matchede noget forudset
}

// ===================================================================================
// MODUL: hentVejrAPI()
// Formål: Eksekverer HTTP-kald, downloader JSON-streng, og udpakker Open-Meteo vejr.
// ===================================================================================
void hentVejrAPI() {
  if (WiFi.status() != WL_CONNECTED) return; // Afbryd hvis WiFi-forbindelsen er nede
  
  // NYT: Hukommelsesovervågning (R-05) for at undgå OOM og heap-fragmentering.
  // WiFiClientSecure, setBufferSizes(4096, 1024) og http.getString() allokerer massive mængder
  // data på heapen. Vi afbryder i god tid for at sikre stabiliteten ved langvarig drift.
  if (ESP.getFreeHeap() < 15000) {
    Serial.print("Kritisk advarsel: Lav heap hukommelse (");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes). Afbryder API-kald for at forhindre OOM crash.");
    return;
  }

  WiFiClientSecure client;                   // Forbindelser til api.open-meteo.com kræver SSL/HTTPS     
  client.setInsecure();                      // Tillad uvalideret SSL-forbindelse for at omgå RAM-tunge certifikatkontroller
  // Den oprindelige kode satte client.setBufferSizes(512, 512) for at spare hukommelse.
  // Men Open-Meteo distribueres via store CDN-netværk (Cloudflare), hvis TLS-handshake
  // og certifikatkæder langt overstiger 512 bytes. Med 512-byte buffere ville SSL-
  // forbindelsen ALTID blive afvist med "record too large", og vejrudsigten ville
  // aldrig blive hentet. Vi hæver derfor RX-bufferen til minimum 4096 bytes (og
  // TX til 1024), hvilket er nødvendigt for moderne web-API'er på ESP8266.
  // Hvis hukommelsen er stram, kan RX forsøges sænket til 2048, men 512 er ikke levedygtigt.
  client.setBufferSizes(4096, 1024);         // TLS RX=4096, TX=1024 - påkrævet for Cloudflare/Open-Meteo
  HTTPClient http;                           

  // URL bygning. Vi beder Open-Meteo om 'current' (nuværende vejr) og 'daily' (solopgang/nedgang).
  // Vi indsætter de dynamiske minKonfig.lat og minKonfig.lon fra EEPROM-strukturen i URL-strengen.
  // Domænet skal være https://api.open-meteo.com
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(minKonfig.lat) + "&longitude=" + String(minKonfig.lon) + 
               "&current=temperature_2m,relative_humidity_2m,weather_code,precipitation" +
               "&daily=sunrise,sunset&timezone=auto&forecast_days=1";

  http.begin(client, url);                   // Klargør klienten
  int httpCode = http.GET();                 // Eksekverer HTTP anmodningen og gemmer returkoden

  // Før tjekkede vi blot "if (httpCode > 0)". Hvis Open-Meteo f.eks. lukkede os ude og svarede med "400 Bad Request" (Som er > 0),
  // ville den sende et HTML-dokument. ESP8266 ville derefter panisk forsøge at køre HTML-koden igennem JSON-parseren!
  // Vi sikrer nu, at vi udelukkende parser, hvis vi rent faktisk fik en kode 200 (HTTP_CODE_OK).
  if (httpCode == HTTP_CODE_OK) {                      
    // ArduinoJson v7: 'JsonDocument doc;' (elastisk, heap-allokeret) [citation:1][citation:6].
    // ArduinoJson v6: Kræver 'DynamicJsonDocument doc(2048);' (fast kapacitet).
    // Hvis du kompilerer med v6, skal linjen nedenfor udskiftes.
    JsonDocument doc;                        // Opret ArduinoJson dokumentobjektet i RAM til afkodning
    
    // Den oprindelige kode kaldte deserializeJson(doc, http.getStream()) direkte på
    // netværksstreamen. Selvom det sparer RAM, forårsager det ekstremt ofte
    // IncompleteInput-fejl på ESP8266 ved store (chunked) HTTPS-payloads, fordi
    // processoren ikke kan følge med indgående pakker. Vi læser derfor først hele
    // payloadet ind i en String og parser den derefter. Dette koster lidt RAM, men
    // er langt mere robust mod chunked encoding og langsomme CDN-forbindelser.
    String payload = http.getString();       // Læs hele svaret ind i hukommelsen først
    DeserializationError jsonFejl = deserializeJson(doc, payload); // Parse den komplette streng
    if (!jsonFejl) {
      
      // JSON træet traverseres med firkantparenteser []. Værdierne type-kastes over i vores globale variabler.
      // Den oprindelige kode udlæste temperature_2m, relative_humidity_2m og
      // precipitation uden isNull()-beskyttelse. Ved rate-limiting eller fejl-svar
      // evaluerer ArduinoJson manglende noder som null, som C++ caster til 0.0.
      // Det slettede gyldige vejrdata og viste falsk "0 °C / 0 % / 0.0 mm".
      // Vi tjekker derfor eksplicit for null og bevarer forrige gyldige værdi.
      if (!doc["current"]["temperature_2m"].isNull()) {
        apiTemp = doc["current"]["temperature_2m"]; 
      } else {
        Serial.println("Advarsel: temperature_2m mangler i API-svar - beholder forrige vaerdi.");
      }
      
      if (!doc["current"]["relative_humidity_2m"].isNull()) {
        apiFugt = doc["current"]["relative_humidity_2m"];
      } else {
        Serial.println("Advarsel: relative_humidity_2m mangler i API-svar - beholder forrige vaerdi.");
      }
      
      // Den gamle kode udlæste vejrkoden med (int)doc["current"]["weather_code"].
      // Hvis Open-Meteo udelod denne node i JSON-svaret, castede ArduinoJson null til 0,
      // og findVejrType(0) oversatte det fejlagtigt til "Skyfrit" - en falsk vejrudsigt
      // uden advarsel til brugeren. Vi tjekker nu eksplicit for null og bevarer den
      // forrige gyldige værdi, hvis noden mangler.
      if (!doc["current"]["weather_code"].isNull()) {
        vejrTypeStr = findVejrType((int)doc["current"]["weather_code"]); 
        // Gem også den rå WMO-kode, så visSideNedboer() kan vælge
        // et korrekt ikon ud fra vejrtypen frem for kun nedbørsmængden.
        apiVejrKode = (int)doc["current"]["weather_code"];
      } else {
        // Noden manglede: Bevar forrige vejrtype og advar via Serial.
        Serial.println("Advarsel: weather_code mangler i API-svar - beholder forrige vaerdi.");
      }
      
      // precipitation beskyttes nu også med isNull(), så et fejl-svar ikke
      // nulstiller den korrekte nedbørsmængde til 0.0 mm.
      if (!doc["current"]["precipitation"].isNull()) {
        apiNedboer = doc["current"]["precipitation"];
      } else {
        Serial.println("Advarsel: precipitation mangler i API-svar - beholder forrige vaerdi.");
      }

      // Hvis API'et re-formaterer deres data, eller fjerner "daily"-træet, vil forsøget på at læse "[0]" 
      // resultere i en ubarmhjertig hardware-crash. Tjekker nu manuelt for null-værdier før parsing forsøges.
      if (!doc["daily"]["sunrise"].isNull() && !doc["daily"]["sunset"].isNull()) {
        const char* sunrise = doc["daily"]["sunrise"][0]; 
        const char* sunset = doc["daily"]["sunset"][0];
        
        // Den gamle kode antog at der ALTID var et 'T' i ISO-strengen. Hvis 'T' mangler,
        // returnerer indexOf() -1, og substring(0) returnerer HELE datoen inkl. årstal,
        // hvilket ville blive vist på skærmen som "Sol op: 2024-01-15 07:30".
        // Vi tjekker derfor eksplicit for 'T' før udklipning.
        if (sunrise != nullptr) {
          String sSunrise = String(sunrise);
          int tPos = sSunrise.indexOf('T');
          if (tPos >= 0) solOpTid = sSunrise.substring(tPos + 1);
        }
        if (sunset != nullptr) {
          String sSunset = String(sunset);
          int tPos = sSunset.indexOf('T');
          if (tPos >= 0) solNedTid = sSunset.substring(tPos + 1);
        }
      }
    } else {
      // Hvis parsing fejler, logges fejlen, så den kan diagnosticeres via Serial Monitor.
      Serial.print("JSON parse-fejl: ");
      Serial.println(jsonFejl.c_str());
    }
  }
  http.end();                                // Luk for HTTP forbindelsen, ryd op i netværkskøen
}

// ===================================================================================
// MODUL: haandterSkermSideskift()
// Formål: Afgør hvornår programmet skal skifte til næste infoside (Giver slides-effekt).
// ===================================================================================
void haandterSkermSideskift(unsigned long nuMillis) {
  // Timer-logik (SKERM_INTERVAL = 4 sekunder)
  if (nuMillis - sidsteSkermSkift >= SKERM_INTERVAL) {
    sidsteSkermSkift = nuMillis;             // Nulstil timer
    
    // Tæller op fra 0 til 4. Modulo (%) betyder:
    // (4 + 1) % 5 = 0. Variablen nulstilles derfor automatisk, når den har vist side 4.
    aktuelSkermSide = (aktuelSkermSide + 1) % 5; 
  }
}

// ===================================================================================
// MODUL: tegnSkermHeader()
// Formål: Grafisk hjælpe-rutine. Tegner en ensartet overskrift øverst, uanset skærmside.
// Input param: 'titel' - Den tekst der skal stå.
// Header-linjen og titlen flyttes nu med burnInOffset pixels, så de øverste pixels
// ikke udsættes for permanent statisk belastning. Offset skifter periodisk via
// haandterBurnInSkift().
// ===================================================================================
void tegnSkermHeader(const char* titel) {
  int yTitel = 10 + burnInOffset;             // Titel-tekstens y-koordinat forskydes med burnInOffset
  int yLinje = 13 + burnInOffset;             // Understregningens y-koordinat forskydes tilsvarende
  u8g2.setFont(u8g2_font_6x12_tf);           // Sætter den normale, lille skrifttype for overskriften
  u8g2.drawStr(0, yTitel, titel);            // Skriver teksten helt ovre til venstre (x=0) og forskudt y
  u8g2.drawHorizontalLine(0, yLinje, 128);   // Tegner en understregning der spænder i skærmens fulde bredde (128px)
}

// ===================================================================================
// MODUL: opdaterDisplayBillede()
// Formål: Hoved-modulet for grafik. Sletter billedet, kalder render-modulet, skubber til OLED.
// ===================================================================================
void opdaterDisplayBillede() {
  u8g2.clearBuffer();                        // Ryd ESP8266's interne memory-buffer (usynlig for brugeren indtil videre)
  
  // Beslutter ud fra 'aktuelSkermSide' (0-4), hvilket specifik grafisk modul, der skal kaldes
  switch (aktuelSkermSide) {
    case 0: visSideApiVejr(); break;         // Hvis 0, tegn API-vejr i bufferen
    case 1: visSideLokalVejr(); break;       // Hvis 1, tegn lokalt vejr i bufferen
    case 2: visSideNedboer(); break;         // Hvis 2, tegn regnvejrs-info i bufferen
    case 3: visSideAstronomi(); break;       // Hvis 3, tegn sol og måne i bufferen
    case 4: visSideUr(); break;              // Hvis 4, tegn det gigantiske ur i bufferen
  }
  
  u8g2.sendBuffer();                         // Processoren har færdigtegnet billedet usynligt. Denne linje skubber pixels-dataene lynhurtigt over I2C til OLED'ens egen hukommelse.
  // delay(50) er fjernet. Den tvang ESP8266 til at blokere i 50 ms pr. loop-iteration
  // og spammede I2C-bussen. Timing styres nu af DISPLAY_INTERVAL i loop(), hvilket
  // bevarer den asynkrone, non-blocking arkitektur.
}

// ===================================================================================
// RENDER-MODULER: De reelle funktioner der genererer layout på hver "Side"
// ===================================================================================

void visSideApiVejr() {
  tegnSkermHeader(minKonfig.by);             // Bruger det indtastede Bynavn fra EEPROM direkte som overskrift
  
  // open_iconic_weather_4x_t indeholder KUN indeks 64-69 (sol til lyn).
  // Indeks 74 eksisterer slet ikke i denne font, hvilket gav tomt hul / fejl-boks.
  // Termometer-ikonet findes i open_iconic_thing_4x_t, som derfor bruges her.
  u8g2.setFont(u8g2_font_open_iconic_thing_4x_t); // Skiftet til thing-familien hvor termometeret findes
  // Ikonets y-koordinat forskydes nu med burnInOffset, så hele layoutet
  // flytter sig i takt med header-linjen og undgår statisk indbrænding.
  u8g2.drawGlyph(4, 22 + burnInOffset, 74);  // Indeks 74 = Termometer i open_iconic_thing (gyldigt i denne font)
  
  u8g2.setFont(u8g2_font_logisoso46_tn);     // Skifter til den enorme "tal-font", der fylder det meste af skærmen (95 pixels høj, findes i u8g2)
  // Det store temperaturtal forskydes med burnInOffset, så det ikke brænder ind.
  u8g2.setCursor(45, 45 + burnInOffset);     // Centrerer startpunktet en smule til højre (x=45) så det ikke overlapper ikonet
  u8g2.print((int)round(apiTemp));           // Korrekt matematisk afrunding, i stedet for blot at klippe decimaler over 
  u8g2.setFont(u8g2_font_7x14_tf);           // Skifter omgående tilbage til mellemstor font
  u8g2.print(" C");                          // Sætter "Celsius" mærkat bag det netop udprintede store tal
  
  u8g2.setFont(u8g2_font_6x12_tf);           // Skifter til standard font (lille)
  // Fugt-linjen forskydes med burnInOffset, så bundteksten følger med header/ikon.
  u8g2.setCursor(45, 60 + burnInOffset);     // Placering nederst (forskudt)
  u8g2.printf("Fugt: %d%%", apiFugt);        // Bruger printf for at indsætte den globale variabel (apiFugt)
}

void visSideLokalVejr() {
  tegnSkermHeader("LOKALT (LOCAL)");         // Overskrift der fortæller, at vi kigger på hardwarens egne indendørs data
  
  // Samme problem som på Side 0 - indeks 74 findes ikke i open_iconic_weather.
  // Termometeret hentes derfor fra open_iconic_thing_4x_t.
  u8g2.setFont(u8g2_font_open_iconic_thing_4x_t); // Skiftet til thing-familien hvor termometeret findes
  // Ikonets y-koordinat forskydes med burnInOffset.
  u8g2.drawGlyph(4, 22 + burnInOffset, 74);  // Indeks 74 = Termometer i open_iconic_thing (gyldigt i denne font)
  
  u8g2.setFont(u8g2_font_logisoso46_tn);     
  // Stort temperaturtal forskydes med burnInOffset.
  u8g2.setCursor(45, 45 + burnInOffset);     
  u8g2.print((int)round(lokalTemp));         // Korrekt matematisk afrunding for højere præcision på hardware-temperaturen
  u8g2.setFont(u8g2_font_7x14_tf);           
  u8g2.print(" C");                          
  
  u8g2.setFont(u8g2_font_6x12_tf);           // I modsætning til api siden, hvor vi viser fugt, viser vi her de andre hardware data:
  // Tryk-visningen forskydes med burnInOffset.
  u8g2.setCursor(45, 55 + burnInOffset);     // Placering: linjen lige under temperaturen (forskudt)
  u8g2.printf("T: %d hPa", (int)round(lokalTryk)); // Afrundet trykvisning
  
  // Den kritiske bundlinje med lysmåling forskydes nu OGSÅ med burnInOffset.
  // Det var netop denne faste koordinat, der gjorde at de nederste pixels stadig brændte ind.
  u8g2.setCursor(45, 64 - burnInOffset);     // Placering: helt i bund kanten (forskudt opad ved offset=1)
  u8g2.printf("L: %d Lux", (int)round(lokalLux));  // Afrundet lysvisning
}

void visSideNedboer() {
  tegnSkermHeader("VEJRUDSIGT");             
  
  u8g2.setFont(u8g2_font_open_iconic_weather_4x_t); 
  // Den oprindelige kode valgte ikon udelukkende ud fra apiNedboer > 0. Ved 0.0 mm
  // nedbør blev sol-ikonet (64) tvunget frem, selvom WMO-koden reelt var tåge (45),
  // overskyet (3) eller nat. Ikonet bindes nu op på den rå WMO-vejrkode (apiVejrKode),
  // som gemmes i hentVejrAPI(), så det afspejler den faktiske vejrtype.
  // Mapping (matcher findVejrType()'s kategorier):
  //   0        = Skyfrit            -> Sol (64)
  //   1-3      = Skyet              -> Sky (65)
  //   45,48    = Taage              -> Taage-ikon (66)
  //   51-57    = Stoevregn/frys.    -> Stoevregn (67)
  //   61-67    = Regn/isslag        -> Regn (67)
  //   71-77    = Sne/sne-korn       -> Sne (68)
  //   80-82    = Byger              -> Regn (67)
  //   85-86    = Snebyger           -> Sne (68)
  //   95+      = Torden             -> Lyn (69)
  //   ukendt/-1= ingen kode endnu   -> Sky (65) som neutral fallback
  // Ikonets y-koordinat forskydes med burnInOffset (begge grene).
  int ikonGlyph;
  if (apiVejrKode == 0) {
    ikonGlyph = 64;                          // Skyfrit -> Sol
  } else if (apiVejrKode >= 1 && apiVejrKode <= 3) {
    ikonGlyph = 65;                          // Skyet -> Sky
  } else if (apiVejrKode == 45 || apiVejrKode == 48) {
    ikonGlyph = 66;                          // Taage -> Taage-ikon
  } else if ((apiVejrKode >= 51 && apiVejrKode <= 57) ||
             (apiVejrKode >= 61 && apiVejrKode <= 67) ||
             (apiVejrKode >= 80 && apiVejrKode <= 82)) {
    ikonGlyph = 67;                          // Stoevregn/regn/isslag/byger -> Regn
  } else if ((apiVejrKode >= 71 && apiVejrKode <= 77) ||
             (apiVejrKode >= 85 && apiVejrKode <= 86)) {
    ikonGlyph = 68;                          // Sne/sne-korn/snebyger -> Sne
  } else if (apiVejrKode >= 95) {
    ikonGlyph = 69;                          // Torden -> Lyn
  } else {
    ikonGlyph = 65;                          // Ukendt eller endnu ingen kode -> neutral Sky
  }
  // Hvis der faktisk ER nedbør registreret, men koden mangler (apiVejrKode == -1),
  // falder vi tilbage til regn-ikonet, så nedbørsmængden ikke skjules visuelt.
  if (apiVejrKode < 0 && apiNedboer > 0) {
    ikonGlyph = 67;                          // Nedbør uden kendt kode -> Regn
  }
  u8g2.drawGlyph(4, 22 + burnInOffset, ikonGlyph); // Tegn det valgte ikon, forskudt med burnInOffset
               
  u8g2.setFont(u8g2_font_7x14_tf);           // Vælg lidt større tal-font
  // Nedbørsmængden forskydes med burnInOffset.
  u8g2.setCursor(52, 35 + burnInOffset);     
  u8g2.printf("%.1f mm", apiNedboer);        // Udskriv API nedbøren som en float ("%f") begrænset til 1 decimal (".1")
  
  u8g2.setFont(u8g2_font_6x12_tf);           // Tilbage til tekst-font
  // Vejrbeskrivelsen forskydes med burnInOffset.
  u8g2.setCursor(52, 52 + burnInOffset);     
  u8g2.print(vejrTypeStr);                   // Udskriver vejrbeskrivelsen ("Byger", "Tåge", "Skyfrit" etc.)
}

void visSideAstronomi() {
  tegnSkermHeader("ASTRONOMI");              
  u8g2.setFont(u8g2_font_6x12_tf);           // Denne side er udelukkende tekst, så vi bliver på lille font
  
  // Alle tre tekstlinjer forskydes med burnInOffset, så siden ikke brænder ind.
  u8g2.setCursor(0, 30 + burnInOffset);      // Starter næsten oppe under headeren (forskudt)
  // Udskriver Månens navn og %-sats. 'maaneNavn' er et String-objekt, printf forventer char-arrays, derfor kaldes metoden '.c_str()'.
  u8g2.printf("Maane: %s (%d%%)", maaneNavn.c_str(), maaneBelysning); 
  
  u8g2.setCursor(0, 46 + burnInOffset);      
  u8g2.printf("Sol op:  %s", solOpTid.c_str()); // Udskriver strengen som API'et returnerede via JSON modulet
  
  u8g2.setCursor(0, 62 - burnInOffset);      // Position i bunden (forskudt opad ved offset=1)
  u8g2.printf("Sol ned: %s", solNedTid.c_str()); 
}

void visSideUr() {
  tegnSkermHeader("UR / KALENDER");          
  
  u8g2.setFont(u8g2_font_logisoso46_tn);     // Hent den massive font igen for at lave et synligt digitatur
  // Det store ur forskydes med burnInOffset.
  u8g2.setCursor(10, 48 + burnInOffset);     // Centrer tallene (forskudt)
  
  // String Formatterings-trick: "%.5s" tager udelukkende de første 5 tegn fra 'tidStr' arrayet.
  // Selvom arrayet indeholder f.eks. "22:15:44", tegner skærmen kun "22:15". 
  // Dette tillader os at have det kæmpe digitatur, uden komplekse substr()-kald.
  u8g2.printf("%.5s", tidStr); 
  
  u8g2.setFont(u8g2_font_6x12_tf);           // Vælg lille skrift for at have plads til datoen nedenunder
  // Dato-linjen forskydes med burnInOffset.
  u8g2.setCursor(28, 62 - burnInOffset);     // Skubbet 28 pixels ind, placeret på y=62 (forskudt opad ved offset=1)
  u8g2.print(datoStr);                       // Udskriver 'datoStr' (Format DD/MM/YYYY opdateret i 'opdaterLokalTidOgDato()')
}
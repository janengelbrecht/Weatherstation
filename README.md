# ESP8266 Vejrstation 🌦️

Dette projekt indeholder komplet kode og hardwaredokumentation til en avanceret, asynkron vejrstation bygget omkring en ESP8266 (NodeMCU/Wemos) microcontroller. Vejrstationen kombinerer lokale sensormålinger med online vejrdata og fremviser det hele på en OLED-skærm. Projektet inkluderer desuden et PC-konfigurationsprogram (skrevet i Free Pascal/Lazarus) til nem opsætning via USB.

## ✨ Funktioner

*   **Lokale Målinger:** Temperatur, atmosfærisk tryk (BMP085) og lysintensitet (BH1750).
*   **Online Vejrdata:** Henter automatisk vejrudsigt, temperatur, luftfugtighed og nedbør fra [Open-Meteo API](https://open-meteo.com/).
*   **Astronomi & Tid:** Viser aktuel tid, dato, solopgang/solnedgang samt beregnet månefase baseret på NTP-synkroniseret UNIX-tid.
*   **OLED Display (5 Sider):** Roterer flydende mellem API Vejr, Lokalt Vejr, Vejrudsigt, Astronomi og Ur. Inkluderer burn-in beskyttelse af skærmen.
*   **Cloud Logging (Valgfrit):** Kan automatisk uploade lokale sensordata til [ThingSpeak](https://thingspeak.com/).
*   **PC Konfiguration:** Medfølgende GUI-program (Windows/Linux/macOS) til at indtaste WiFi-oplysninger og geografiske koordinater (Gemmes permanent i ESP8266's EEPROM).
*   **Robust Arkitektur:** Asynkron (non-blocking) kode, hardware debounce på knapper, automatisk WiFi-reconnect og overlevelse i offline-tilstand.

---

## 🛠️ Hardware Komponenter

| Komponent | Type | Formål | I2C Adresse |
| :--- | :--- | :--- | :--- |
| **NodeMCU (ESP8266)** | Microcontroller | Hjernen: kører firmware, WiFi, I2C, Seriel | - |
| **OLED Display** | SSD1306 (128x64) | Viser vejrdata og statusbeskeder | `0x3C` |
| **BMP085** | Tryk/Temp Sensor | Måler lokal temperatur og atmosfærisk tryk | `0x77` |
| **BH1750** | Lyssensor | Måler omgivende lysintensitet (Lux) | `0x23` |
| **Trykknap** | Momentan (N/O) | Aktiverer PC-konfiguration / afbryder program | - |

*(Alle sensorer er bygget til 3.3V logik. Brug ikke 5V på I2C-bussen).*

### Forbindelsesdiagram (Pinout)

I2C-enhederne (OLED, BMP085, BH1750) deler den samme bus og er parallelkoblet.

*   **GPIO 14 (D5):** I2C SCL (Clock)
*   **GPIO 12 (D6):** I2C SDA (Data)
*   **GPIO 13 (D7):** Trykknap (Forbundet til stel/GND)
*   **3V3:** Strøm til alle sensorer
*   **GND:** Stel til alle sensorer og knap

---

## 💻 Software & Afhængigheder (ESP8266)

Koden til ESP8266 kræver følgende biblioteker for at kompilere i Arduino IDE:
*   `ESP8266WiFi`, `ESP8266HTTPClient`, `WiFiClientSecure` (Indbygget i ESP8266 core)
*   `ArduinoJson` (SKAL VÆRE **version 7**)
*   `U8g2` (Til OLED-display)
*   `Wire`, `EEPROM`, `time.h`

*(Bemærk: BMP085 og BH1750 driverne er indlejret direkte i koden for at holde projektet samlet og optimeret).*

### Opsætning af ThingSpeak
Hvis du ønsker at logge dine lokale data til skyen, skal du indsætte din ThingSpeak Write API Key i hovedprogrammet:
```cpp
#define THINGSPEAK_API_KEY "DIN_THINGSPEAK_WRITE_KEY_HER"
```
*(Lad den stå som placeholder for at deaktivere upload).*

---

## 🖥️ PC Konfigurationsprogram (Lazarus / Free Pascal)

For at undgå hardcodede WiFi-koder i C++ koden, konfigureres vejrstationen via det medfølgende Lazarus-projekt (`Weatherstation.lpr` / `Unit1.pas`).

### Sådan fungerer konfigurationen:
1. Tryk og hold hardware-knappen (D7) nede i 1,5 sekund (eller hold den nede mens strømmen tilsluttes) for at sætte vejrstationen i **Parringstilstand**. Displayet vil vise "Forbundet til PC... Venter paa data...".
2. Åbn PC-programmet og vælg den korrekte COM-port.
3. Indtast dit WiFi **SSID**, **Password** og dine geografiske koordinater (**Latitude/Longitude** med max 7 tegn pga. 174-byte protokollen).
4. Klik på **"Send til ESP8266"**.
5. PC'en og ESP8266 udfører et serielt handshake (`9600 8E1`). Dataene brændes i EEPROM (markeret med signaturen `CFG`), og stationen genstarter automatisk.

---

## 🔍 Test Program
Projektet indeholder et dedikeret hardware-testprogram (`HardwareTest.ino` - se dokumentationen). Dette bør flashes som det **første** for at validere dine lodninger og hardwareforbindelser. 
Testprogrammet gennemgår:
1. I2C Bus og adressescan.
2. OLED respons.
3. BMP085 & BH1750 kommunikation og sanity-check af data.
4. Trykknap-aflæsning (bruger-interaktion).
5. EEPROM læs/skriv test.
6. Seriel Handshake test.

Giver den "ALLE TESTS OK!", er din hardware klar til hovedprogrammet.

---

## ⚠️ Vigtige Bemærkninger
* **I2C Routing:** Koden bruger et specifikt hack (`Wire.begin(12, 14)` før og efter `u8g2.begin()`) for at forhindre display-biblioteket i at overskrive de custom I2C-pins.
* **Seriel Forbindelse:** Parring kræver eksakte serielle indstillinger: `9600 baud, 8 databit, Even Parity, 1 stopbit (SERIAL_8E1)`.
* **API Hukommelse:** ArduinoJson benytter v7 elastisk hukommelse, og SSL bufferen er hævet til `RX=4096` for at håndtere Cloudflares TLS-certifikater under Open-Meteo kald. Sænkes dette, vil vejrudsigten fejle.

# Kravspecifikation (Requirements Specification)
**Projekt:** ESP8266 Asynkron Vejrstation
**Version:** 1.0
**Dato:** 30. oktober 2026

## 1. Introduktion
Dette dokument definerer de funktionelle, ikke-funktionelle, hardware- og interfacemæssige krav til "ESP8266 Asynkron Vejrstation". Formålet med systemet er at indsamle, bearbejde og vise lokalt målte vejrdata kombineret med eksterne API-vejrudsigter og astronomiske beregninger. 

Dokumentet tjener som grundlag for verifikation og validering af systemet. Hvert krav er udstyret med et unikt ID (f.eks. `FR-FW-01`) for at sikre sporbarhed (Traceability) gennem udviklings- og testfasen.

Terminologi:
*   **SKAL (MUST):** Et kritisk krav, der skal opfyldes, for at systemet kan accepteres.
*   **BØR (SHOULD):** Et vigtigt krav, der dog kan omgås, hvis der findes en acceptabel workaround.
*   **KAN (MAY):** En valgfri funktionalitet (Nice-to-have).

---

## 2. Hardware Krav (Hardware Requirements - HR)
Disse krav definerer de fysiske komponenter og deres elektriske begrænsninger.

*   **HR-01: Microcontroller:** Systemet SKAL baseres på en ESP8266-platform (f.eks. NodeMCU eller Wemos D1 Mini).
*   **HR-02: Logikniveau:** Hele systemets I/O logik SKAL operere ved 3.3V DC. Ingen sensorer må forsynes med 5V.
*   **HR-03: Display:** Systemet SKAL integrere et 0.96" SSD1306 monokromt OLED-display (128x64 pixels).
*   **HR-04: Tryk & Temp Sensor:** Systemet SKAL integrere en Bosch BMP085 piezoresistiv sensor.
*   **HR-05: Lyssensor:** Systemet SKAL integrere en ROHM BH1750 digital lux-sensor.
*   **HR-06: Bruger-input:** Systemet SKAL indeholde en momentan (Normally Open) trykknap.
*   **HR-07: I2C Routing:** I2C-bussen (SDA og SCL) SKAL føres via henholdsvis GPIO 12 og GPIO 14 (og må ikke benytte ESP8266's standard GPIO 4/5).
*   **HR-08: Knap Routing:** Trykknappen SKAL forbindes mellem GPIO 13 og GND, og processoren SKAL benytte sin interne pull-up modstand.

---

## 3. Funktionelle Krav - Firmware (Functional Requirements Firmware - FR-FW)
Krav til den software, der afvikles direkte på ESP8266-microcontrolleren.

### 3.1. Sensorhåndtering
*   **FR-FW-01:** Firmwaren SKAL indlæse 11 kalibreringskoefficienter fra BMP085's ROM ved opstart.
*   **FR-FW-02:** Firmwaren SKAL beskytte mod hardware-exceptions (Division by Zero) i BMP085 kompensationsalgoritmen (`computeB5`), såfremt korrupte I2C-data modtages.
*   **FR-FW-03:** Firmwaren SKAL sætte BH1750 i `CONTINUOUS_HIGH_RES_MODE` og omregne rådata til lux via konverteringsfaktoren 1.2.
*   **FR-FW-04:** Firmwaren SKAL foretage lokale sensormålinger med et fast interval på 5 sekunder.

### 3.2. Eksterne Data (API & Tid)
*   **FR-FW-05:** Firmwaren SKAL synkronisere den interne UNIX-tæller via NTP-protokollen (f.eks. via `dk.pool.ntp.org`).
*   **FR-FW-06:** Firmwaren SKAL via HTTPS indhente vejrudsigt (Temp, Fugt, WMO-Vejrkode, Nedbør) fra Open-Meteo API med et fast interval på 30 minutter.
*   **FR-FW-07:** Firmwaren SKAL understøtte automatisk parsing af dansk sommertid via en POSIX tidszone-streng.
*   **FR-FW-08:** Firmwaren SKAL lokalt beregne den aktuelle månefase ud fra UNIX-tiden uden at benytte eksterne API-kald, baseret på referencedatoen 6. januar 2000.
*   **FR-FW-09:** Firmwaren KAN uploade lokalt målte data (Temp, Tryk, Lux) til ThingSpeak via HTTP GET hvert 5. minut, forudsat at en gyldig API-nøgle er konfigureret.

### 3.3. Grafisk Brugergrænseflade (GUI på OLED)
*   **FR-FW-10:** Firmwaren SKAL implementere en 5-siders karrusel, der automatisk skifter visning hvert 4. sekund.
*   **FR-FW-11:** Siderne SKAL som minimum omfatte: (1) API Vejr, (2) Lokalt Vejr, (3) Vejrudsigt (inkl. WMO-kategoriseret ikon), (4) Astronomi og (5) Digitalt ur.
*   **FR-FW-12:** Firmwaren SKAL implementere "Burn-in protection" ved at forskyde (offsette) samtlige statiske pixels på Y-aksen med et fast interval (f.eks. 60 sekunder).

### 3.4. Konfiguration og Hukommelse (EEPROM)
*   **FR-FW-13:** Firmwaren SKAL forsøge at læse konfiguration (SSID, Password, By, Lon, Lat) fra EEPROM ved boot.
*   **FR-FW-14:** Firmwaren SKAL validere EEPROM-data vha. signaturen (magic string) `CFG\0`.
*   **FR-FW-15:** Finder firmwaren ingen gyldig konfiguration, SKAL systemet stoppe boot-sekvensen og anmode om PC-konfiguration via displayet.

---

## 4. Funktionelle Krav - PC Software (Functional Requirements PC - FR-PC)
Krav til det Free Pascal/Lazarus-baserede konfigurationsværktøj.

*   **FR-PC-01:** PC-softwaren SKAL levere en grafisk brugergrænseflade (GUI) til indtastning af: SSID, Password, Land, By, Longitude og Latitude.
*   **FR-PC-02:** PC-softwaren SKAL dynamisk fremsøge og opliste tilgængelige serielle COM-porte på værtssystemet (Windows/Linux/macOS).
*   **FR-PC-03:** PC-softwaren SKAL indbygge validering, der afviser input overstigende de fastsatte feltlængder (SSID: 50, Pass: 50, Land: 20, By: 40, Lon: 7, Lat: 7 tegn).
*   **FR-PC-04:** PC-softwaren SKAL polstre (padde) eventuelt manglende tegn i hvert felt med mellemrum (ASCII `0x20`), for at sikre en statisk pakkelængde.
*   **FR-PC-05:** PC-softwaren SKAL afvente trigger-strengen `"klar"` fra ESP8266 inden datapakken overføres.

---

## 5. Interface & Protokol Krav (Interface Requirements - IR)
Krav til systemets kommunikationsgrænseflader (Hardware og Netværk).

*   **IR-01: I2C Hastighed:** I2C-bussen SKAL tvinges i Fast Mode (400 kHz) umiddelbart efter `u8g2.begin()`, da den massive OLED-grafik ellers vil bryde det asynkrone loop-design.
*   **IR-02: Seriel Handshake Protokol:** Den serielle kommunikation med PC'en SKAL foregå ved `9600 baud, 8 databit, Even parity, 1 stopbit (8E1)`.
*   **IR-03: Rå Datastruktur:** Datapakken fra PC til ESP8266 SKAL være præcis 174 bytes med følgende offset-kortlægning:
    *   `000-049` [50 byte]: WiFi SSID
    *   `050-099` [50 byte]: WiFi Password
    *   `100-119` [20 byte]: Land (Reserveret/bagudkompatibel)
    *   `120-159` [40 byte]: Bynavn
    *   `160-166` [07 byte]: Longitude
    *   `167-173` [07 byte]: Latitude
*   **IR-04: JSON Parsing:** Firmwaren SKAL anvende `ArduinoJson` version 7 for at understøtte elastisk hukommelsesallokering under parsing af Open-Meteo svar.
*   **IR-05: Netværks-buffer:** HTTPS-klienten SKAL tvinges til at allokere mindst 4096 bytes RX-buffer (via `setBufferSizes(4096, 1024)`) for at kunne håndtere fragmenterede TLS-handshakes fra Cloudflare CDN-servere.

---

## 6. Ikke-funktionelle Krav (Non-Functional Requirements - NFR)
Krav til systemets overordnede kvalitet, performance og robusthed.

*   **NFR-01: Performance (Non-blocking):** Hovedloopet (`void loop()`) SKAL udformes asynkront ved hjælp af `millis()` timere. Brugen af `delay()` til tidsstyring er ikke tilladt, da knap-aflæsning og I2C målinger ikke må suspenderes.
*   **NFR-02: Pålidelighed (Debounce):** Knapaflæsningen SKAL implementeres med software-debounce (min. 50 ms stabil tilstand) og SKAL kræve et langt tryk (min. 1500 ms) for at afvise elektrisk støj (EMI).
*   **NFR-03: Robusthed (Offline-drift):** Mistet forbindelse til WiFi eller NTP SKAL IKKE resultere i system-crash eller fastfrysning. Systemet SKAL fortsætte driften som en lokal vejrstation og løbende forsøge `WiFi.reconnect()` i baggrunden.
*   **NFR-04: Robusthed (Ugyldig Tid):** Systemet SKAL skjule dato/tid og afbryde måneberegninger, hvis NTP-tiden returnerer et UNIX-tidsstempel under år 2023 (angiver manglende sync). Månefasen må ikke udregnes ud fra falske 1970-datoer.
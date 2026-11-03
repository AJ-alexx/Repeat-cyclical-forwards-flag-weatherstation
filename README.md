# Repeat cyclical forwards flag weatherstation

FANET-Wetter- und Namenspakete mit gesetztem Forward-Flag empfangen und weiterleiten. Die Firmware ist für das **Heltec WiFi LoRa 32 (V3)** mit ESP32-S3 und SX1262 ausgelegt.

## Voraussetzungen

- Arduino IDE 2.x und installiertes Heltec-ESP32-Boardpaket
- Board **Heltec WiFi LoRa 32 (V3)** in der Arduino IDE ausgewählt
- Bibliothek **RadioLib** über den Bibliotheksverwalter installiert
- Heltec WiFi LoRa 32 (V3), passende Antenne und kompatible FANET-Stationen

## Kompilieren und flashen

1. Repository herunterladen oder klonen.
2. `geplantemasterrepeat/geplantemasterrepeat.ino` in der Arduino IDE öffnen.
3. Unter **Werkzeuge > Board** das **Heltec WiFi LoRa 32 (V3)** und den passenden seriellen Port auswählen.
4. RadioLib bei Bedarf über **Sketch > Bibliothek einbinden > Bibliotheken verwalten…** installieren.
5. Kompilieren und auf das Board hochladen.
6. Den seriellen Monitor auf **115200 Baud** stellen. Beim Start sollte `RX ready` erscheinen.

## Arbeitsweise

Die Station empfängt FANET-Pakete und leitet ausschließlich gültige **NAME-Pakete (`0x02`)** und **Wetterpakete (`0x04`)** mit gesetztem Forward-Flag weiter. Andere Pakettypen, ungültige Pakete und Pakete ohne dieses Flag werden verworfen. Vor dem erneuten Senden löscht die Firmware das Forward-Flag, damit eine weitere Station das Paket nicht erneut weiterleitet.

Vor dem Senden wartet die Firmware eine kurze zufällige Zeit und prüft, ob der Funkkanal frei ist. Bereits gesendete oder noch in der Warteschlange befindliche Duplikate werden unterdrückt. Die Warteschlange fasst acht Pakete; Pakete, die länger als 30 Sekunden warten, verfallen. Nach dem Senden hält die Firmware entsprechend der Sendedauer eine Funkpause ein, um den vorgesehenen Sendezeitanteil von höchstens 1 % einzuhalten.

## Empfang und Lernen der Sendeintervalle

Im adaptiven Empfang (`ENABLE_ADAPTIVE_RX = 1`, Standard) bleibt das Funkmodul beim Start zunächst dauerhaft im Empfang. Für jede Kombination aus Absender und Pakettyp lernt die Firmware separat den Empfangsabstand gültiger Pakete mit Forward-Flag:

- NAME: drei Pakete mit zwei aufeinanderfolgenden Intervallen, die höchstens 20 % voneinander abweichen.
- Wetter: vier Pakete mit drei Intervallen, deren größte und kleinste Messung höchstens 25 % des mittleren Intervalls auseinanderliegen.
- Empfangsabstände unter zwei Sekunden zählen nicht als neue Messung.

Bei stabil gelerntem Intervall schaltet die Station den Funkempfang erst kurz vor dem nächsten erwarteten Paket ein und kann dazwischen das Funkmodul sowie – sofern aktiviert – den ESP32 in Light-Sleep versetzen. Verpasste Pakete verbreitern das Empfangsfenster in beide Richtungen, insgesamt jedoch höchstens auf 10 Sekunden einschließlich Startpuffer. Kleine Abweichungen korrigieren den gespeicherten Rhythmus schrittweise.

Stabile Absenderadressen und gelernte NAME- und Wetterintervalle werden dauerhaft im ESP32-NVS gespeichert. Nach einem Neustart kontrolliert die Station gespeicherte Intervalle zunächst mindestens sechs Minuten lang. Sie prüft jedes gespeicherte Protokoll anhand von zwei empfangenen Paketen. Nicht bestätigte Stationen werden nach den im Sketch festgelegten Regeln entfernt; bestätigte Zeitpläne können anschließend wieder zeitgesteuert empfangen werden. Neue Stationen oder messbare Intervalländerungen starten den Lernvorgang erneut.

Nach drei aufeinanderfolgenden verpassten NAME-Fenstern werden NAME und Wetter einer Adresse aus dem Zeitplan entfernt. Nach zehn verpassten Wetterfenstern wird nur der Wetterzeitplan deaktiviert; NAME wird weiter gesucht. Ein später empfangenes passendes Wetterpaket kann den Wetterzeitplan wieder anlernen. Eine neue Station wird nur erkannt, wenn ein gültiges Paket während eines aktiven Empfangsfensters eintrifft.

Die Firmware verwaltet 32 Zeitplanplätze, also höchstens 16 verschiedene Absenderadressen mit je einem Platz für NAME und Wetter. Die erste Erkennung bleibt dauerhaft aktiv, bis alle reservierten Plätze gelernt sind und drei Minuten lang kein passendes Paket eintrifft, spätestens jedoch 20 Minuten. Alle zwölf Stunden startet eine erneute Erkennung. Die seriellen Statusmeldungen enthalten unter anderem `RX`, `TX`, `filtered`, `duplicates`, `dropped` und `cadence=gelernte/erfasste Protokolle`.

## LEDs und Kontrolle

- **GPIO 35 (`PIN_LED`):** Status-LED für das Senden weitergeleiteter Pakete; bei einem fatalen Fehler blinkt sie dauerhaft (200 ms an, 800 ms aus). Der serielle Monitor zeigt dazu `FATAL`, Vorgang und Fehlercode.
- **GPIO 4 (`PIN_AWAKE_LED`):** externe LED leuchtet, solange der ESP32 wach ist, und ist während Light-Sleep aus. Vorwiderstand in Reihe verwenden; Schaltskizze siehe unten.
- **GPIO 5 (`PIN_TX_LED`):** zusätzliche externe Sende-LED leuchtet während jedes Sendeversuchs und erlischt danach auch bei einem Sendefehler. Die interne LED an GPIO 35 bleibt unverändert; die externe Sende-LED zeigt keine fatalen Fehler an.
- **GPIO 6 (`PIN_LED_INHIBIT`):** freier Eingang mit internem Pull-up. Wird GPIO 6 mit GND verbunden, bleiben beide externen LEDs aus. Die interne Status-LED an GPIO 35 bleibt davon unberührt.
- **GPIO 7 (`PIN_EXTERNAL_RESET`):** Reset-Eingang mit internem Pull-up. Wird GPIO 7 mit GND verbunden, startet das Board einmal neu. Bleibt GND anliegen, läuft es danach weiter; erst nach dem Trennen und erneutem Verbinden wird wieder ein Reset ausgelöst.
- **GPIO 2 (`PIN_BATTERY_ADC`):** ADC-Eingang zur Akkuüberwachung über einen externen Spannungsteiler.

Der serielle Monitor läuft mit **115200 Baud**. Im normalen Betrieb werden Statuszähler ungefähr jede Minute ausgegeben.

### Externe Sende-LED anschließen

Eine normale rote oder gelbe LED mit **470-Ohm-Vorwiderstand** anschließen:

```text
Board GPIO 5 ----[ 470 Ohm ]---- Anode (+) LED Kathode (-) ---- Board GND
```

Die Anode ist normalerweise das längere Bein; die Kathode das kürzere Bein an der abgeflachten Gehäuseseite. Nicht ohne Vorwiderstand anschließen. **GPIO 5 ist nicht der 5-V-Anschluss.** Die bestehende Wach-LED an GPIO 4 bleibt angeschlossen. Nach dem Flashen leuchtet die neue LED nur während des blockierenden Sendeaufrufs; auch ein fehlgeschlagener Sendeversuch wird angezeigt.

### Beide externen LEDs mit GND abschalten

GPIO 6 dient als Schalteingang. Ein Schalter oder Jumper zwischen GPIO 6 und GND schaltet die externe Wach-LED (GPIO 4) und die externe Sende-LED (GPIO 5) aus. Im offenen Zustand zieht der interne Pull-up den Eingang auf HIGH und die normale LED-Steuerung bleibt aktiv. Für diesen Schalteingang ist kein zusätzlicher Widerstand nötig.

```text
GPIO 4 ──[ 1 kΩ ]──|>|── GND     Wach-LED
GPIO 5 ──[470 Ω ]──|>|── GND     Sende-LED

GPIO 6 ──────o/ o────── GND      Schalter/Jumper
             offen = LEDs normal
             geschlossen = beide externen LEDs aus

GPIO 7 ──────o/ o────── GND      Reset-Taster/Jumper
             geschlossen = einmaliger Neustart
```

Für GPIO 6 und GPIO 7 sind keine zusätzlichen Widerstände nötig. Der Reset-Eingang weckt das Board auch aus dem Light-Sleep. GPIO 35 (interne Status-LED) wird durch den GPIO-6-Schalter nicht beeinflusst.

## Solar-, Akku- und 3,3-V-Versorgung

Die Skizze zeigt den vorgesehenen Aufbau. Die Panelspannung muss über einen **für Solarpanels und eine einzelne Li-Ion-/LiPo-Zelle geeigneten Solar-Laderegler** zur Batterie geführt werden. Ein einfacher TC4056/TP4056-Lader ist normalerweise kein Solarregler und hat meist keine Lastaufteilung (Power-Path). Das Solarpanel deshalb **nicht direkt** an den Akku, den 3V3-Pin oder ungeregelt an den Eingang eines einfachen TC4056-Moduls anschließen. Falls ein TC4056-Modul verwendet wird, muss dessen Eingang innerhalb der Datenblattgrenzen stabil und strombegrenzt versorgt werden; für gleichzeitiges Laden und Betreiben ist ein Lader mit Power-Path vorzuziehen.

```text
 Solarpanel PV+ / PV-
          │
          ▼
┌─────────────────────────────────────┐
│ Solar-Laderegler für 1S Li-Ion/LiPo  │  Solar-Eingang mit passender
│ Eingangsstrombegrenzung, Power-Path  │  Begrenzung/Ladeschlussspannung
└──────┬────────┬──────────┬──────────┘
       │ BAT+   │ BAT-     │ LOAD+/OUT+ ── Wandler IN+
       │        │          └ LOAD-/OUT- ── Wandler IN- ──┐
       │        │                                         │
       │     Akku-/GND ───────────────────────────────────┤ gemeinsame Masse
       │                                                  │
       ▼                                                  ▼
  1S-Akku mit                                  ┌─────────────────────┐
  Schutzschaltung                              │ 3,3-V-Buck-Boost-   │
       │                                       │ Wandler             │
       │                                       └──────┬────────┬────┘
       │                                          OUT+ │        │ OUT-
       │                                               ▼        └── Board GND
       │                                          Board 3V3
       │
       └── B+/BAT+ ── R1 470 kΩ ──┬── GPIO 2 (ADC)
                                  ├── R2 100 kΩ ── GND
                                  └── C1 100 nF ─── GND

 Board GPIO 4 ── R3 1 kΩ ──|>|── Board-GND
                            Wach-LED

 Board GPIO 5 ── R4 470 Ω ──|>|── Board-GND
                             Sende-LED
```

**Anschlüsse und Bauteile**

- Der **3,3-V-Buck-Boost-Wandler** kommt an den geschützten Batterie-/Last-Ausgang. Sein Ausgang muss auf **3,3 V** eingestellt und vor dem Anschluss gemessen werden. Er muss die Stromspitzen des ESP32 und des SX1262 beim Senden liefern können. `OUT+` geht an den Board-Pin **3V3**, `OUT−` an **GND**. Keine 5 V an den 3V3-Pin anlegen.
- Ladegerät, Wandler, Board und Teiler brauchen eine gemeinsame Masse. Bei Einspeisung in 3V3 den USB-C-Anschluss nicht gleichzeitig verwenden, außer die konkrete Boardrevision erlaubt diese Versorgung ausdrücklich.
- Für GPIO 2 liegt **R1 = 470 kΩ** zwischen Akku-Plus und GPIO 2; **R2 = 100 kΩ** liegt von GPIO 2 nach GND. **C1 = 100 nF keramisch** liegt parallel zu R2, möglichst nahe am GPIO. Akku-Plus niemals direkt an GPIO 2 anschließen. Die Messleitung kommt an die Akku-/Zellspannung, nicht an den 3,3-V-Wandlerausgang.
- Der Sketch rechnet mit einem Teilerfaktor von **5,7**. Mit den genannten Widerständen ergeben 4,2 V am Akku ungefähr 0,74 V am ADC. Bauteiltoleranzen und ADC-Abweichungen können die Anzeige beeinflussen; Akkuanzeige mit einem Multimeter gegenprüfen.
- GPIO 4 steuert die externe Wach-LED: Anode über **1 kΩ** an GPIO 4, Kathode an GND. GPIO 5 steuert die zusätzliche Sende-LED: Anode über **470 Ω** an GPIO 5, Kathode an GND. Beide Vorwiderstände sind erforderlich. GPIO 35 bleibt die interne Sende-/Fehler-LED.

Die Firmware misst die Akkuspannung beim Start und anschließend etwa jede Minute. Bei **3,3 V oder weniger** speichert sie den Unterspannungszustand und wechselt in Deep-Sleep. Im Schutzmodus wacht sie alle **60 Minuten** kurz zur Messung auf; erst oberhalb von **3,6 V** startet sie wieder. Diese Softwareabschaltung ersetzt weder eine Akku-Schutzschaltung noch einen geeigneten Ladecontroller. Deep-Sleep schaltet den externen Wandler nicht ab; dessen Ruhestrom bleibt Teil des Verbrauchs.

Ein einfacher TC4056/TP4056 kann bei angeschlossener Last den Ladevorgang unter Umständen nicht korrekt beenden, weil er keine getrennte Lastversorgung bereitstellt. Ein geeigneter Solar-Laderegler mit Power-Path ist die bessere Wahl. Akkuchemie, Ladeschlussspannung, Panel-Leerlaufspannung und maximalen Eingangsstrom müssen zu den Datenblättern von Akku und Lademodul passen. Vor dem Aufbau die Boardrevision und die Belegung der Anschlüsse prüfen.

## Einstellungen und Funkprofil

- Light-Sleep und adaptiver Empfang sind standardmäßig aktiviert (`ENABLE_LIGHT_SLEEP` und `ENABLE_ADAPTIVE_RX` stehen auf `1`). Zum dauerhaften Empfang können die Werte im Sketch auf `0` gesetzt werden. Bei deaktiviertem adaptivem Empfang bleibt das Funkmodul im normalen Betrieb dauerhaft im Empfang; bei deaktiviertem Light-Sleep bleibt zusätzlich der ESP32 wach.
- Das Funkprofil ist im Sketch auf **868,2 MHz** mit den dort angegebenen FANET-Parametern eingestellt. Frequenz und Sendeleistung müssen zur Region und zum verwendeten Netz passen.

**Hinweis:** Funkbetrieb und Sendeleistung müssen den örtlichen Vorschriften entsprechen. Vor dem Einsatz Antenne, Frequenz und Hardwarekompatibilität prüfen.

## Host-Tests

Die Host-Tests benötigen `g++` und prüfen Start, Weiterleitung und Light-Sleep anhand simulierter Hardware. Sie ersetzen nicht das Kompilieren und Prüfen auf dem Heltec-Board:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -DARDUINO_ARCH_ESP32 \
  -DCONFIG_IDF_TARGET_ESP32S3 -Itests/stubs tests/firmware_test.cpp \
  -o /tmp/fanet-firmware-test
/tmp/fanet-firmware-test
```

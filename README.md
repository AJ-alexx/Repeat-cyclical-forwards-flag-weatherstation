# Repeat cyclical forwards flag weatherstation

FANET-Wetter- und Namenspakete mit gesetztem Forward-Flag empfangen und weiterleiten. Die Firmware ist für das **Heltec WiFi LoRa 32 (V3)** (ESP32-S3 mit SX1262) ausgelegt.

## Voraussetzungen

- Arduino IDE 2.x
- Heltec-ESP32-Boardpaket in der Arduino IDE installiert
- Board **Heltec WiFi LoRa 32 (V3)** ausgewählt
- Bibliothek **RadioLib** über den Bibliotheksverwalter installiert
- Heltec WiFi LoRa 32 (V3), passende Antenne und kompatible FANET-Stationen

## Kompilieren und flashen

1. Repository herunterladen oder klonen.
2. In der Arduino IDE die Datei `geplantemasterrepeat/geplantemasterrepeat.ino` öffnen.
3. Unter **Werkzeuge > Board** das **Heltec WiFi LoRa 32 (V3)** auswählen und den passenden seriellen Port einstellen.
4. RadioLib installieren, falls noch nicht vorhanden: **Sketch > Bibliothek einbinden > Bibliotheken verwalten…**, nach `RadioLib` suchen und installieren.
5. Kompilieren und auf das Board hochladen.
6. Den seriellen Monitor mit **115200 Baud** öffnen. Beim Start sollte `RX ready` erscheinen; Statuszähler werden ungefähr jede Minute ausgegeben.

## Arbeitsweise

Die Station empfängt FANET-Pakete und leitet ausschließlich gültige **NAME-Pakete (`0x02`)** und **Wetterpakete (`0x04`)** weiter, bei denen das Forward-Flag gesetzt ist. Andere Pakettypen, ungültige Pakete und Pakete ohne dieses Flag werden verworfen. Vor dem erneuten Senden löscht die Firmware das Forward-Flag. So wird das Paket von einer weiteren Station mit derselben Weiterleitungsregel nicht erneut weitergeleitet.

Vor dem Senden wartet die Firmware eine kurze zufällige Zeit und prüft, ob der Funkkanal frei ist. Bereits gesendete oder noch in der Warteschlange befindliche Duplikate werden unterdrückt. Die Warteschlange fasst acht Pakete; Pakete, die länger als 30 Sekunden warten, verfallen. Nach dem Senden hält die Firmware entsprechend der Sendedauer eine Funkpause ein, um den in der Firmware vorgesehenen Anteil von höchstens 1 % Sendezeit einzuhalten.

## Empfang und Lernen der Sendeintervalle

Im adaptiven Empfang (`ENABLE_ADAPTIVE_RX = 1`, Standard) bleibt das Funkmodul beim Start zunächst dauerhaft im Empfang. Für jede Kombination aus Absender und Pakettyp lernt die Firmware separat, in welchem Abstand gültige Pakete mit Forward-Flag eintreffen:

- Für ein NAME-Protokoll braucht sie drei empfangene Pakete mit zwei aufeinanderfolgenden, höchstens 20 % voneinander abweichenden Intervallen.
- Für ein Wetterprotokoll braucht sie vier Pakete mit drei Intervallen, deren größte und kleinste Messung höchstens 25 % des mittleren Intervalls auseinanderliegen.
- Empfangsabstände unter zwei Sekunden zählen nicht als neue Messung. So verschieben sehr schnelle Wiederholungen den gelernten Rhythmus nicht.

Sobald ein Intervall stabil gelernt ist, berechnet die Station den nächsten erwarteten Empfang. Sie schaltet den Funkempfang erst kurz vor diesem Zeitpunkt ein und kann dazwischen das Funkmodul und – sofern aktiviert – den ESP32 in Light-Sleep versetzen. Verpasste Pakete verbreitern das Empfangsfenster; kleine Abweichungen korrigieren den gespeicherten Rhythmus schrittweise.

Während der ersten Erkennung sowie nach dem Auftauchen eines neuen oder geänderten Protokolls bleibt der Empfänger dauerhaft aktiv. Sind alle reservierten Protokollplätze gelernt, endet diese Erkennung nach drei Minuten ohne ein weiteres passendes Paket; spätestens nach 20 Minuten endet sie auch bei unvollständigem Lernen. Ist die Erkennung beendet, kann die Station bereits gelernte Intervalle zum zeitgesteuerten Empfang nutzen, auch wenn andere Plätze noch ungelernte Einträge enthalten. Alle zwölf Stunden startet eine erneute Erkennung. Dabei bleiben zuvor gelernte Intervalle erhalten. Die Lernwerte liegen nur im Arbeitsspeicher und gehen beim Neustart oder Stromausfall verloren.

Die Firmware verwaltet 32 Zeitplanplätze. Beim ersten passenden Paket einer Absenderadresse reserviert sie jeweils zwei Plätze: einen für NAME und einen für Wetter. Damit können bis zu 16 verschiedene Absenderadressen verwaltet werden; NAME und Wetter lernen ihre Intervalle trotzdem getrennt. Wird von einer Station nur einer der beiden Pakettypen gesendet, bleibt der andere Platz ungelernte Reserve. Dadurch kann die erste Erkennung in diesem Fall bis zum 20-Minuten-Limit aktiv bleiben. Der serielle Wert `cadence=gelernte/erfasste` zählt gelernte und reservierte Plätze, nicht nur tatsächlich beobachtete Pakettypen. Bei überschrittener Kapazität meldet der serielle Monitor eine Warnung; weitere Absender können dann nicht eingeplant werden.

## LEDs und Kontrolle

- **GPIO 35 (`PIN_LED`):** leuchtet während des Sendens eines weitergeleiteten Pakets und ist im normalen Empfangs- und Wartezustand aus. Bei einem fatalen Fehler blinkt sie dauerhaft: 200 ms an, 800 ms aus. In diesem Fall zeigt der serielle Monitor zusätzlich `FATAL` mit dem fehlgeschlagenen Vorgang und Fehlercode; die Station muss zurückgesetzt und der Fehler untersucht werden.
- **GPIO 4 (`PIN_AWAKE_LED`, externe LED):** leuchtet, solange der ESP32 wach ist, und ist während Light-Sleep aus. Sie zeigt also den Schlafzustand an, nicht den Paketempfang.

Den seriellen Monitor auf **115200 Baud** stellen. Beim Start sind `RX ready` und Meldungen zu Empfangs- und Schlafmodus zu erwarten, sofern die Batterie den Funkbetrieb erlaubt. Im normalen Betrieb gibt `Status` etwa jede Minute die Zähler `RX` (empfangene Funkpakete), `TX` (erfolgreich weitergeleitete Pakete), `filtered` (nicht weitergeleitete Pakete), `duplicates` (erkannte Duplikate), `dropped` (verlorene oder abgelaufene Pakete) und `cadence=gelernte/erfasste Protokolle` aus.

## Batterieschutz

Die Firmware misst die Batteriespannung beim Start und danach etwa alle 60 Sekunden. Sobald eine gültige Messung **unter 3,30 V** liegt, stoppt sie Empfang und Senden, leert die Sendewarteschlange und versetzt das Funkmodul in Schlaf. Der ESP32 geht in **Light-Sleep** und wacht nur für die nächste Batterieprüfung auf; Funkpakete wecken ihn in diesem Zustand nicht auf. Die externe LED an GPIO 4 ist beim Schlafen aus und leuchtet während der kurzen Prüfungen.

Der Funkbetrieb bleibt gesperrt, bis eine gültige Messung **über 3,60 V** liegt. Genau 3,30 V oder 3,60 V ändern den aktuellen Zustand nicht. Nach der Erholung werden die alten Empfangszeitpläne und gespeicherten Duplikate gelöscht. Die Station beginnt erneut mit kontinuierlichem Empfang und lernt die Sendeintervalle von Grund auf. Während der Unterspannungsphase werden keine Pakete gespeichert oder später nachgesendet.

Im seriellen Monitor erscheinen `BATTERY LOW` beim Abschalten, die regelmäßigen Spannungsmessungen und `BATTERY RECOVERED` beim Wiederanlauf. Eine ungültige ADC-Messung wird ausdrücklich als Warnung gemeldet und verändert den aktuellen Schutzstatus nicht; sie kann die Station insbesondere nicht aus dem Batterieschlaf freigeben. Schlägt Light-Sleep fehl, bleibt der Funk bei Unterspannung trotzdem gesperrt und der Fehler wird ausgegeben. Batterieanschluss, Spannungsteiler und `BATTERY_CALIBRATION` müssen zur Hardware passen. Die Software ersetzt keinen hardwareseitigen Tiefentladeschutz.

## Einstellungen und Funkprofil

- Light-Sleep und adaptiver Empfang sind standardmäßig aktiviert (`ENABLE_LIGHT_SLEEP` und `ENABLE_ADAPTIVE_RX` stehen auf `1`). Zum dauerhaften Empfang können die Werte im Sketch auf `0` gesetzt werden. Bei deaktiviertem adaptivem Empfang bleibt das Funkmodul im normalen Betrieb dauerhaft im Empfang; bei deaktiviertem Light-Sleep bleibt zusätzlich der ESP32 wach. Der Batterieschutz und dessen Light-Sleep bleiben unabhängig von diesen Einstellungen aktiv.
- Das Funkprofil ist im Sketch auf **868,2 MHz** mit den dort angegebenen FANET-Parametern eingestellt. Frequenz und Sendeleistung müssen zur Region und zum verwendeten Netz passen.

**Hinweis:** Funkbetrieb und Sendeleistung müssen den örtlichen Vorschriften entsprechen. Vor dem Einsatz Antenne, Frequenz und Hardwarekompatibilität prüfen.

## Tests für den Batterieschutz

Die Host-Tests benötigen `g++` und prüfen Schaltschwellen, Funkabschaltung, wiederholten Schlaf, Wiederanlauf und Fehlerverhalten anhand simulierter Hardware. Sie ersetzen nicht das Kompilieren und Prüfen auf dem Heltec-Board:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -DARDUINO_ARCH_ESP32 \
  -DCONFIG_IDF_TARGET_ESP32S3 -Itests/stubs tests/battery_test.cpp \
  -o /tmp/fanet-battery-test
/tmp/fanet-battery-test
```

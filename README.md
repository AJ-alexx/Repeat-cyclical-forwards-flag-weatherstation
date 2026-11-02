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

Sobald ein Intervall stabil gelernt ist, berechnet die Station den nächsten erwarteten Empfang. Sie schaltet den Funkempfang erst kurz vor diesem Zeitpunkt ein und kann dazwischen das Funkmodul und – sofern aktiviert – den ESP32 in Light-Sleep versetzen. Verpasste Pakete verbreitern das Empfangsfenster in beide Richtungen, insgesamt jedoch höchstens auf 10 Sekunden einschließlich Startpuffer. Kleine Abweichungen korrigieren den gespeicherten Rhythmus schrittweise.

Stabile Absenderadressen sowie gelernte NAME- und Wetterintervalle werden dauerhaft im ESP32-NVS gespeichert. NAME und Wetter werden unabhängig gelernt: Eine Station mit regelmäßigem NAME-Protokoll kann daher auch ohne gelerntes Wetterprotokoll gespeichert und weiter über ihren NAME-Zeitplan empfangen werden. Nach einem Neustart startet ein Kontrollbetrieb: Die Station empfängt mindestens sechs Minuten dauerhaft und prüft jedes gespeicherte Protokoll anhand von zwei empfangenen Paketen auf sein gespeichertes Intervall. Wird eine Station gehört, verlängert sich die Kontrolle, bis auch deren NAME-Meldung mit zwei Paketen geprüft werden konnte. Eine Station, von der in dieser Zeit kein einziges Paket empfangen wurde, wird aus dem festen Speicher gelöscht. Sind alle verbliebenen Protokolle bestätigt, schaltet die Station ohne neue Lernphase auf zeitgesteuerten Empfang mit Sleep/Wakeup um. Neue Stationen oder messbare Intervalländerungen starten den normalen Lernvorgang.

Zusätzlich zählt die Station im Betrieb vollständig verstrichene Empfangsfenster ohne passende Meldung. Nach drei aufeinanderfolgenden verpassten NAME-Fenstern werden NAME und Wetter der Adresse gemeinsam aus dem Zeitplan und Speicher entfernt. Nach zehn verpassten Wetterfenstern wird nur der Wetterzeitplan deaktiviert; NAME wird weiter gesucht. Ein NAME-Paket allein aktiviert die Wettersuche nicht erneut. Trifft später während einer Lernphase wieder ein passendes Wetterpaket ein, wird der Wetterzeitplan neu gelernt und kann wieder aufgenommen werden. Eine vollständig gelöschte Station kann ebenfalls durch ein neues passendes Paket während der Erkennung erneut gelernt werden.

Ein gültiges, bislang nicht gelerntes NAME- oder Wetterpaket mit gesetztem Forward-Flag startet ebenfalls eine neue Lernphase. Das gilt auch für eine zuvor gelöschte Station oder für Wetter, dessen Suche nach zehn Fehlfenstern deaktiviert wurde. Der Neustart-Kontrolllauf behält seine bestehenden eigenen Prüf- und Löschregeln. Kontroll- und Lernphasen dauern mindestens sechs Minuten und warten bei nicht gehörten Stationen außerdem mindestens ein vollständiges gespeichertes Intervall ab. Die daraus entfernten Stationen und deaktivierten Wetterzeitpläne werden gespeichert. Vorhandene Speicherstände des bisherigen Formats werden weiterhin gelesen; ein Zurücksetzen des NVS löscht die gespeicherten Zeitpläne.

**Neue Station während des zeitgesteuerten Betriebs:** Eine noch unbekannte Station wird nur erkannt, wenn ihr Paket eintrifft, während das Funkmodul gerade aktiv empfängt. Wird dann ein gültiges Wetter- oder Namenspaket mit Forward-Flag empfangen, wird es wie andere passende Pakete zur normalen Weiterleitung vorgemerkt. Gleichzeitig legt die Firmware für die neue Absenderadresse je einen Zeitplanplatz für Wetter und NAME an und startet die Erkennung erneut: Der Empfänger bleibt dauerhaft aktiv, um die Sendeintervalle zu lernen. Für den Wetterrhythmus sind vier passende Pakete nötig. Sendet die Station nur Wetterpakete, bleibt ihr NAME-Platz ungelernte Reserve; dann endet die Erkennung erst nach spätestens 20 Minuten, nicht schon nach drei ruhigen Minuten. Sind alle Zeitplanplätze gelernt, endet sie nach drei Minuten ohne ein weiteres passendes Paket. Anschließend wird wieder zeitgesteuert empfangen. Eine neue Station, deren Sendungen während des Schlafens oder außerhalb aktiver Empfangsfenster liegen, kann dabei zunächst unentdeckt bleiben. Ist die Kapazität von 32 Zeitplanplätzen ausgeschöpft, kann eine neue Adresse nicht mehr eingeplant werden; ein bereits empfangenes gültiges Paket kann trotzdem noch nach den normalen Warteschlangen- und Weiterleitungsregeln weitergeleitet werden.

Während der ersten Erkennung sowie nach dem Auftauchen eines neuen oder geänderten Protokolls bleibt der Empfänger dauerhaft aktiv. Sind alle reservierten Protokollplätze gelernt, endet diese Erkennung nach drei Minuten ohne ein weiteres passendes Paket; spätestens nach 20 Minuten endet sie auch bei unvollständigem Lernen. Ist die Erkennung beendet, kann die Station bereits gelernte Intervalle zum zeitgesteuerten Empfang nutzen, auch wenn andere Plätze noch ungelernte Einträge enthalten. Alle zwölf Stunden startet eine erneute Erkennung. Dabei bleiben zuvor gelernte Intervalle von gehörten Stationen erhalten. Stabile Lernwerte werden dauerhaft gespeichert; unvollständige Lernreihen bleiben nur im Arbeitsspeicher.

Die Firmware verwaltet 32 Zeitplanplätze. Beim ersten passenden Paket einer Absenderadresse reserviert sie jeweils zwei Plätze: einen für NAME und einen für Wetter. Damit können bis zu 16 verschiedene Absenderadressen verwaltet werden; NAME und Wetter lernen ihre Intervalle trotzdem getrennt. Wird von einer Station nur einer der beiden Pakettypen gesendet, bleibt der andere Platz ungelernte Reserve. Dadurch kann die erste Erkennung in diesem Fall bis zum 20-Minuten-Limit aktiv bleiben. Der serielle Wert `cadence=gelernte/erfasste` zählt gelernte und reservierte Plätze, nicht nur tatsächlich beobachtete Pakettypen. Bei überschrittener Kapazität meldet der serielle Monitor eine Warnung; weitere Absender können dann nicht eingeplant werden.

## LEDs und Kontrolle

- **GPIO 35 (`PIN_LED`):** leuchtet während des Sendens eines weitergeleiteten Pakets und ist im normalen Empfangs- und Wartezustand aus. Bei einem fatalen Fehler blinkt sie dauerhaft: 200 ms an, 800 ms aus. In diesem Fall zeigt der serielle Monitor zusätzlich `FATAL` mit dem fehlgeschlagenen Vorgang und Fehlercode; die Station muss zurückgesetzt und der Fehler untersucht werden.
- **GPIO 4 (`PIN_AWAKE_LED`, externe LED):** leuchtet, solange der ESP32 wach ist, und ist während Light-Sleep aus. Sie zeigt also den Schlafzustand an, nicht den Paketempfang.

Den seriellen Monitor auf **115200 Baud** stellen. Beim Start sind `RX ready` und Meldungen zu Empfangs- und Schlafmodus zu erwarten. Im normalen Betrieb gibt `Status` etwa jede Minute die Zähler `RX` (empfangene Funkpakete), `TX` (erfolgreich weitergeleitete Pakete), `filtered` (nicht weitergeleitete Pakete), `duplicates` (erkannte Duplikate), `dropped` (verlorene oder abgelaufene Pakete) und `cadence=gelernte/erfasste Protokolle` aus.

## Akku und Solarversorgung

Die Firmware misst die Akkuspannung über einen **externen Spannungsteiler an GPIO 2**. Das ist nötig, wenn das Board über einen 3,3-V-Regler am 3V3-Pin versorgt wird und der Akkuanschluss des Boards frei bleibt. Den Spannungsteiler direkt zwischen Akku und Board-GND anschließen:

```text
Akku + ──┬────────────── Regler IN+
         │
       470 kΩ
         │
         ├────────────── GPIO 2
         ├── 100 nF ──┐
         │            │
       100 kΩ         │
         │            │
Akku − ──┴────────────┴── Board GND ── Regler IN−/OUT−

Regler OUT+ ───────────── Board 3V3
```

Der Kondensator liegt parallel zum 100-kΩ-Widerstand und möglichst nahe an GPIO 2; ein keramischer 100-nF-Kondensator ist ungepolt. Akku-Plus darf nicht direkt an GPIO 2 oder den Board-3V3-Pin. Reglerausgang vor dem Anschluss auf 3,3 V einstellen und messen; der Regler muss den gesamten Akkuspannungsbereich abdecken und bis zur 3,3-V-Abschaltschwelle eine stabile Versorgung liefern. Bei 4,2 V Akkuspannung liegen am GPIO ungefähr 0,74 V an; die Firmware verwendet den Teilerfaktor 5,7. Die tatsächliche Messung hängt von den Widerstandstoleranzen ab und sollte mit einem Multimeter abgeglichen werden.

Die Firmware prüft die Akkuspannung im normalen Betrieb jede Minute. Bei **3,3 V oder weniger** speichert sie den Unterspannungszustand und wechselt in Deep-Sleep. Im Schutzmodus wacht sie nur alle **60 Minuten** kurz zur Messung auf; erst oberhalb von **3,6 V** startet wieder die Lernphase. Der Akkuanschluss am Board bleibt in diesem Aufbau frei; der Spannungsteiler muss dauerhaft mit dem Akku verbunden sein. Die Ladeelektronik beziehungsweise ein passender externer LiPo-Lader muss die Ladeschlussspannung von 4,2 V begrenzen. Deep-Sleep schaltet einen externen Regler nicht ab; dessen Ruhestrom bleibt Teil des Verbrauchs.

Eine geeignete 5-V-Versorgung über USB-C kann das Heltec WiFi LoRa 32 V3 versorgen und einen kompatiblen 1S-Li-Ion-/LiPo-Akku über die Ladeelektronik des Boards laden, wenn der Akku am vorgesehenen Boardanschluss angeschlossen ist. Ein Solarpanel benötigt einen passenden Solarregler beziehungsweise DC/DC-Wandler; es darf nicht direkt am Akku oder ungeregelt an USB-C angeschlossen werden. Boardrevision, zulässigen Akkutyp und Ladehinweise des Herstellers vor dem Aufbau prüfen.

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

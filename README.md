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

## Verhalten und Einstellungen

- Weitergeleitet werden FANET-NAME- (`0x02`) und Wetterpakete (`0x04`) nur, wenn das Forward-Flag gesetzt ist.
- Standardmäßig sind Light-Sleep und adaptiver Empfang aktiviert (`ENABLE_LIGHT_SLEEP` und `ENABLE_ADAPTIVE_RX` stehen auf `1`). Zum dauerhaften Empfang können die Werte im Sketch auf `0` gesetzt werden.
- Das Funkprofil ist im Sketch auf **868,2 MHz** mit den dort angegebenen FANET-Parametern eingestellt. Frequenz und Sendeleistung müssen zur Region und zum verwendeten Netz passen.
- Adaptiver Empfang lernt die Sendeintervalle der Stationen; eine erneute Erkennung startet alle 12 Stunden. Während der Erkennung bleibt der Empfänger durchgehend aktiv.

**Hinweis:** Funkbetrieb und Sendeleistung müssen den örtlichen Vorschriften entsprechen. Vor dem Einsatz Antenne, Frequenz und Hardwarekompatibilität prüfen.

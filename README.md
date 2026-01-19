# ESP32 Temperaturlogger (HKV)

Dieses Projekt beschreibt einen Temperaturlogger auf Basis eines ESP32 und mehrerer
DS18B20-Temperatursensoren.

Der ESP32 misst in festen Intervallen die Temperaturen und überträgt die Messwerte
per WLAN in eine InfluxDB Cloud. Dort werden die Daten mit Zeitstempel gespeichert
und können anschließend mit Grafana visualisiert werden.

Der Fokus des Projekts liegt auf der Messung und Auswertung von Temperaturen an
Heizkreisverteilern (z. B. Fußbodenheizung), etwa für Analyse, Vergleich oder
hydraulisch-thermische Betrachtungen.

## Funktionsübersicht
- ESP32 mit mehreren DS18B20-Sensoren (OneWire)
- zyklische Temperaturmessung
- Übertragung der Messwerte an InfluxDB
- Visualisierung mit Grafana
- Konfiguration über ein Web-Interface
- keine Anpassung des Programmcodes notwendig

## Projektinhalt
- Arduino-Firmware für den ESP32
- Flux-Queries für InfluxDB
- Grafana Dashboard (JSON)
- Ausführliche Schritt-für-Schritt-Anleitung als PDF

## Hinweise
Dieses Projekt ist als Hobby- und Bastelprojekt gedacht.
Es gibt keine Garantie auf Vollständigkeit, Messgenauigkeit oder Eignung
für produktive oder sicherheitskritische Anwendungen.

Die Nutzung erfolgt auf eigene Verantwortung.

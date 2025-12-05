Zwitscherbox Sketch – Funktionsübersicht
1. Firmware & Version
•	Version: 2.4.F
•	Info: "micronano & ChatGPT 11/2025"
________________________________________
2. Hardware
•	ESP8266 D1 Mini als Steuerung
•	MP3-TF-16P V3.0 = DFPlayer Mini für MP3-Wiedergabe
•	RTC DS3231 für Zeitspeicherung
•	HC-SR501 PIR Bewegungssensor 
•	BC337-Transistor für Stromabschaltung MP3Player
•	2xBC547 für Steuerung PIR im Betrieb / PIR DeepSleep wakeup
•	Diverse Pins für Steuerung (Steuer_PIN, BC337_PIN, PIR_PIN, I2C Pins SDA/SCL)
________________________________________
3. Software & Bibliotheken
•	ESP8266WiFi – WLAN-Verbindung
•	LittleFS – Konfiguration speichern
•	ArduinoJson – JSON-Konfigurationsdateien
•	ESP8266WebServer & HTTPUpdateServer – Webinterface und OTA-Updates
•	TimeLib & RTClib – Echtzeituhr und Zeitmanagement
•	DFRobotDFPlayerMini – MP3-Player-Steuerung
________________________________________
4. Konfiguration (ZwitscherConfig)
•	WLAN: ssid, pwd
•	MP3-Player: 11 Ordner (mit Root), 6x Ordnerwechsel, 3x Stummzeiten (von/bis), 3x Lautstärkenänderungen (von/bis), PlayMode (Zufall, nach der Reihe, nur Track 1),
•	PIR: pirSperrZeit einstellbar (Pause nach abgespieltem Track)
•	Auto-Folder: bis zu 6 Zeiträume mit automatischem Ordnerwechsel
•	Lautstärke-Timer: bis zu 3 Zeiträume
________________________________________
5. MP3-Player Funktionen
•	Init: Player starten, Lautstärke setzen
•	Non-blocking Playback:
o	Zufälliger Track, sequenziell oder nur Track 001
o	Stummzeiten werden beachtet
o	Lautstärke-Timer wird berücksichtigt
o	MP3-Status (mp3Playing) überwacht
•	Track fertig erkennen: DFPlayerPlayFinished Event
•	Ordnerwechsel automatisch basierend auf Konfiguration
________________________________________
6. PIR-Trigger
•	Interrupt ausgelöst bei Bewegung
•	Trigger wird nur alle pirSperrZeit Sekunden akzeptiert
•	Überprüft Stummzeiten
•	Spielt MP3 aus aktivem Ordner
•	Startet ggf. WLAN
________________________________________
7. WLAN / Internet
•	Aktiviert temporär bei Bewegung oder Webzugriff
•	Deaktiviert automatisch nach Ablauf (10s Stummzeit, 20s normal)
•	DeepSleep nach WLAN-Timeout
•	WLAN IP & RSSI werden im Webinterface angezeigt
________________________________________
8. Uhrzeit - NTP / RTC
•	Synchronisation nur bei Bedarf:
o	Erste Einrichtung (cfg.lastNtpSync == 0)
o	Oder mehr als 1 Woche seit letztem Sync
•	RTC DS3231 wird aktualisiert
•	Cached RTC wird sekündlich in updateCachedRTC() hochgezählt → non-blocking Zeit
•	Funktionen:
o	nowRTC() → aktuelle Zeit aus Cache
o	timeStr() → Uhrzeit als String
________________________________________
9. Stummzeiten & Lautstärke-Timer
•	Stummzeit:
o	Max. 3 Zeiträume / Tag
o	Kein MP3-Playback in dieser Zeit
•	Volume Timer:
o	Bis zu 3 Zeiträume / Tag
o	Lautstärke automatisch anpassen
________________________________________
10. Webinterface
•	Zeigt aktuelle Uhrzeit, IP-Adresse, Signalstärke
•	Konfigurationsmöglichkeiten:
o	WLAN-SSID / Passwort
o	Default MP3 Ordner
o	PlayMode
o	Lautstärke & Track-Pause
o	Ordner / Tracks / Bezeichnungen
o	Ordnerwechsel Einstellungen
o	Stummzeiten
o	Lautstärke-Timer
•	Buttons:
o	Speichern
o	Neustart
o	Auto-Refresh – kein DeepSleep während Auto-Refresh läuft
•	OTA Updates über /update
________________________________________
11. Loop / Main Tasks
•	Non-blocking:
o	Server-Client Handling
o	PIR-Trigger prüfen
o	WLAN Timeout prüfen
o	RTC Cache aktualisieren
o	WLAN-Zeit ausgeben
o	MP3-Track-Finish erkennen
•	Nutzt yield() für Watchdog-Kompatibilität
________________________________________
12. Besonderheiten / Non-blocking
•	MP3 abspielen ohne die Hauptschleife zu blockieren
•	RTC Cache aktualisiert jede Sekunde, ohne Delay
•	DeepSleep Timer stoppt während MP3-Playback, bzw. Webzugriff
•	Stummzeit und Ordnerwechsel werden laufend geprüft
________________________________________
💡 Zusammengefasst:
Die Zwitscherbox ist ein bewegungsgesteuerter, zeitabhängiger MP3-Spieler, der WLAN temporär aktiviert, Stummzeiten & Lautstärke-Timer berücksichtigt, MP3 Ordner wechseln kann, und sich über ein Webinterface konfigurieren lässt, inklusive Updatemöglichkeit. Alles läuft non-blocking, sodass Sensoren, WLAN, WebUI und MP3 gleichzeitig überwacht werden.


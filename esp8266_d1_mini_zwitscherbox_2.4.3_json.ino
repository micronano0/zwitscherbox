
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <DFRobotDFPlayerMini.h>
#include <SoftwareSerial.h>
#include <ESP8266HTTPUpdateServer.h>
#include <TimeLib.h>
#include <time.h>
#include <Wire.h>
#include "RTClib.h"

#include <ESP8266mDNS.h>

#define MDNS_NAME "zwitscherbox"

// ---------------- VERSION ----------------
#define FW_VERSION "2.4.3 json"
#define FW_INFO "micronano & chatGPT 01/2026"

// ---------------- CONFIG ----------------
struct ZwitscherConfig {
    char ssid[25];
    char pwd[40];
    bool autoFolderEnable;
    bool noSoundTimerEnable;
    int playMode; // 0 = Zufall, 1 = Reihenfolge, 2 = Nur 001
    int volume;
    int timerStart[3];
    int timerEnd[3];
    int folderSelected;
    int trackCount[11];
    uint32_t pirSperrZeit;
    char folderName[11][21];
    int autoFolderStart[6];
    int autoFolderEnd[6];
    int autoFolder[6];
    bool volumeTimerEnable;
    int volumeTimerVol[3];
    int volumeTimerStart[3];
    int volumeTimerEnd[3];
    uint32_t lastNtpSync = 0;
    int lastPlayedTrack[11]; // speichert den letzten Track für jeden Ordner
    uint32_t pirSperrZeitRest;
    uint32_t lastSleepUnix;   // Zeitpunkt des letzten DeepSleep-Starts
    char deviceName[32];
};

ZwitscherConfig cfg;
int savedFolderSelected = -1;

// ---------------- HARDWARE ----------------
#define Tx_PIN        D1
#define Steuer_PIN    D2
#define BC337_PIN     D3
#define SDA_PIN       D4
#define PIR_PIN       D5
#define SCL_PIN       D6
#define Rx_PIN        D7

SoftwareSerial mp3Serial(Rx_PIN, Tx_PIN);
DFRobotDFPlayerMini myDFPlayer;
RTC_DS3231 rtc;
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// ---------------- GLOBALS ----------------
volatile bool pirTriggered = false;
bool PirSperrZeitAbwarten = false;
uint32_t pirSperrZeitcpy = 0;
unsigned long pirHighStart = 0;
unsigned long lastTrackTime = 0;
unsigned long pirSleepRemaining = 0;
unsigned long pirSperrEndTime = 0; // Zeitpunkt, bis wann PIR gesperrt ist
uint32_t slept = 0;

bool wifiActive = false;
unsigned long WIFI_ACTIVE_START = 0;
const unsigned long WIFI_ACTIVE_DURATION = 60000UL; // nach 60 Sekunden ==> DeepSleep
const unsigned long WIFI_STUMM_DURATION  = 10000UL; // nach 10 Sekunden bei Stummzeit

unsigned long lastWifiPrint = 0;
bool apActive = false;


// MP3 non-blocking
bool mp3Playing = false;
unsigned long mp3StartTime = 0;
int mp3Folder = 0;
int mp3Track = 0;
bool playedAfterMute = false;
bool playerReady = false;

bool rtcAvailable = false;

// ---------------- NTP ----------------
#define MY_NTP_SERVER "pool.ntp.org"
#define MY_TZ "CET-1CEST,M3.5.0/02,M10.5.0/03"
#define NTP_MAX_DIFF_SECONDS (5UL * 60UL * 60UL) // 5 Stunden

DateTime cachedRTC;
unsigned long lastRTCMillis = 0;

// ---------------- DEBUG SWITCH ----------------
/*
bool serialDebug = true; // kann zur Laufzeit geändert werden

void debugPrintln(const String &msg) {
    if(serialDebug) Serial.println(msg);
}

void debugPrintf(const char* format, ...) {
    if(!serialDebug) return;
    va_list args;
    va_start(args, format);
    char buffer[256];
    vsnprintf(buffer, sizeof(buffer), format, args);
    Serial.print(buffer);
    va_end(args);
}
*/


const char HTML_HEAD[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body {
    font-family: Arial, sans-serif;
    background: #f5f5f5;
    text-align: center;
    margin: 0;
    padding: 0;
}

h1 {
    margin-top: 20px;
}

.fw-info {
    font-size: 0.9em;
    color: #555;
    margin-bottom: 15px;
}

form {
    background: #fff;
    padding: 20px;
    border-radius: 10px;
    max-width: 650px;
    margin: 20px auto;
    box-shadow: 0 2px 6px rgba(0,0,0,0.15);
}

table {
    width: 100%;
    border-collapse: collapse;
    margin-bottom: 15px;
}

th, td {
    border: 1px solid #ccc;
    padding: 5px;
    text-align: center;
}

th {
    background: #2d4d6f;
    color: white;
}

input[type=text], input[type=password], input[type=number], select {
    width: 100%;
    padding: 5px;
    box-sizing: border-box;
}

.button-group {
    display: flex;
    justify-content: center;
    gap: 15px;
    margin-top: 15px;
}

button, input[type=submit] {
    padding: 8px 16px;
    border: none;
    border-radius: 5px;
    background-color: #8C0000;
    color: white;
    cursor: pointer;
    font-size: 1em;
}

button:hover, input[type=submit]:hover {
    background-color: #45a049;
}

</style>
</head>
<body>
<form action='/save' method='POST'>
)rawliteral";



const char HTML_FOOT[] PROGMEM = R"rawliteral(
<div class="button-group">
    <input type="submit" value="Speichern">
    <button type="button" onclick="location.href='/restart'">Neustart</button>
    <button type="button" onclick="window.open('/autorefresh')">Auto Refresh</button>
</div>
</form>
</body>
</html>
)rawliteral";


// ---------------- ISR ----------------
void ICACHE_RAM_ATTR pirISR() {
    if (millis() < pirSperrEndTime) return; // noch sperren
//    if (millis() - lastTrackTime > cfg.pirSperrZeit * 1000UL) {
        pirTriggered = true;
        pirHighStart = millis();
    }


// ---------------- CONFIG LOAD/SAVE ----------------
void loadConfig() {
    memset(&cfg, 0, sizeof(cfg));
    strlcpy(cfg.deviceName, "zwitscherbox", sizeof(cfg.deviceName));

    cfg.volumeTimerEnable = false;
    for (int i = 0; i < 3; i++) {
        cfg.volumeTimerVol[i] = 20;
        cfg.volumeTimerStart[i] = 0;
        cfg.volumeTimerEnd[i] = 0;
    }
    cfg.autoFolderEnable = true;
    cfg.noSoundTimerEnable = true;
    cfg.volume = 20;
    cfg.folderSelected = 0;
    cfg.pirSperrZeit = 30;
    for (int i = 0; i < 3; i++) { cfg.timerStart[i] = 0; cfg.timerEnd[i] = 0; }
    for (int i = 0; i <= 10; i++) cfg.trackCount[i] = 1;
    for (int i = 0; i < 6; i++) { cfg.autoFolderStart[i] = 0; cfg.autoFolderEnd[i] = 0; cfg.autoFolder[i] = -1; }

    cfg.lastNtpSync = 0; // Standardwert

    if (!LittleFS.begin()) return;
    if (!LittleFS.exists("/config.json")) return;

    File f = LittleFS.open("/config.json", "r");
    if (!f) return;

    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return;

    strlcpy(cfg.deviceName, doc["deviceName"] | "zwitscherbox", sizeof(cfg.deviceName));
    strlcpy(cfg.ssid, doc["ssid"] | "", sizeof(cfg.ssid));
    strlcpy(cfg.pwd, doc["pwd"] | "", sizeof(cfg.pwd));
    cfg.autoFolderEnable = doc["autoFolderEnable"] | true;
    cfg.noSoundTimerEnable = doc["noSoundTimerEnable"] | true;
    cfg.playMode = doc["playMode"] | 0;
    cfg.volume = doc["volume"] | 20;
    cfg.folderSelected = doc["folderSelected"] | 0;
    cfg.pirSperrZeit = doc["pirSperrZeit"] | 30;
    cfg.pirSperrZeitRest = doc["pirSperrZeitRest"] | 0;
    cfg.lastSleepUnix = doc["lastSleepUnix"] | 0;

    for (int i = 0; i < 3; i++) {
        cfg.timerStart[i] = doc["timerStart"][i] | 0;
        cfg.timerEnd[i] = doc["timerEnd"][i] | 0;
    }

    for (int i = 0; i <= 10; i++) {
        cfg.trackCount[i] = doc["trackCount"][i] | 1;
        strlcpy(cfg.folderName[i], doc["folderName"][i] | "", sizeof(cfg.folderName[i]));
    }

    for (int i = 0; i < 6; i++) {
        cfg.autoFolderStart[i] = doc["autoFolderStart"][i] | 0;
        cfg.autoFolderEnd[i] = doc["autoFolderEnd"][i] | 0;
        cfg.autoFolder[i] = doc["autoFolder"][i] | -1;
    }

    cfg.volumeTimerEnable = doc["volumeTimerEnable"] | false;
    for (int i = 0; i < 3; i++) {
        cfg.volumeTimerVol[i]   = doc["volumeTimerVol"][i] | cfg.volume;
        cfg.volumeTimerStart[i] = doc["volumeTimerStart"][i] | 0;
        cfg.volumeTimerEnd[i]   = doc["volumeTimerEnd"][i] | 0;
    }

    cfg.lastNtpSync = doc["lastNtpSync"] | 0;
    for(int i=0;i<=10;i++) cfg.lastPlayedTrack[i] = doc["lastPlayedTrack"][i] | 0;

}

// ---------------- CONFIG SAVE ----------------
void saveConfig() {
    StaticJsonDocument<2048> doc;

    doc["deviceName"] = cfg.deviceName;
    doc["ssid"] = cfg.ssid;
    doc["pwd"] = cfg.pwd;
    doc["autoFolderEnable"] = cfg.autoFolderEnable;
    doc["noSoundTimerEnable"] = cfg.noSoundTimerEnable;
    doc["playMode"] = cfg.playMode;
    doc["volume"] = cfg.volume;
    doc["folderSelected"] = cfg.folderSelected;
    doc["pirSperrZeit"] = cfg.pirSperrZeit;
    doc["pirSperrZeitRest"] = cfg.pirSperrZeitRest;
    doc["lastSleepUnix"] = cfg.lastSleepUnix;

    JsonArray aStart = doc.createNestedArray("timerStart");
    JsonArray aEnd = doc.createNestedArray("timerEnd");
    for (int i = 0; i < 3; i++) { aStart.add(cfg.timerStart[i]); aEnd.add(cfg.timerEnd[i]); }

    JsonArray aTracks = doc.createNestedArray("trackCount");
    JsonArray aNames = doc.createNestedArray("folderName");
    for (int i = 0; i <= 10; i++) { aTracks.add(cfg.trackCount[i]); aNames.add(cfg.folderName[i]); }

    JsonArray aAFStart = doc.createNestedArray("autoFolderStart");
    JsonArray aAFEnd = doc.createNestedArray("autoFolderEnd");
    JsonArray aAF = doc.createNestedArray("autoFolder");
    for (int i = 0; i < 6; i++) { aAFStart.add(cfg.autoFolderStart[i]); aAFEnd.add(cfg.autoFolderEnd[i]); aAF.add(cfg.autoFolder[i]); }

    doc["volumeTimerEnable"] = cfg.volumeTimerEnable;
    JsonArray vVol = doc.createNestedArray("volumeTimerVol");
    JsonArray vStart = doc.createNestedArray("volumeTimerStart");
    JsonArray vEnd = doc.createNestedArray("volumeTimerEnd");
    for (int i = 0; i < 3; i++) {
        vVol.add(cfg.volumeTimerVol[i]);
        vStart.add(cfg.volumeTimerStart[i]);
        vEnd.add(cfg.volumeTimerEnd[i]);
    }

    // 64-bit safe: lastNtpSync
    doc["lastNtpSync"] = cfg.lastNtpSync;
    if(cfg.playMode == 1){ // nur bei "der Reihe nach" speichern
        JsonArray aLastTrack = doc.createNestedArray("lastPlayedTrack");
        for(int i=0;i<=10;i++) aLastTrack.add(cfg.lastPlayedTrack[i]);
    }

    File f = LittleFS.open("/config.json", "w");
    if (!f) return;
    serializeJson(doc, f);
    f.close();
}


// ---------------- UTILITY ----------------
void setRtcFromInternetTime() {
    if (!rtcAvailable) return;             // RTC muss verfügbar sein
    if (WiFi.status() != WL_CONNECTED) return; // WLAN muss aktiv sein

    configTime(MY_TZ, MY_NTP_SERVER);

    time_t t = 0;
    unsigned long start = millis();

    // maximal 10 Sekunden warten
    while (t < 1609459200 && millis() - start < 10000) {
        delay(200);
        t = time(nullptr);
    }

    if (t < 1609459200) return; // keine gültige Zeit vom NTP-Server

    struct tm timeinfo;
    localtime_r(&t, &timeinfo);

    // RTC aktualisieren
    rtc.adjust(DateTime(
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
    ));

    // Cache aktualisieren
    cachedRTC = rtc.now();
    lastRTCMillis = millis();

    // NTP-Zeit merken und config speichern
    cfg.lastNtpSync = t;
    saveConfig();
}

bool needsNtpSync() {
    if (!rtcAvailable) return true;          // ohne RTC immer syncen
    if (cfg.lastNtpSync == 0) return true;   // noch nie synchronisiert

    uint32_t rtcUnix = cachedRTC.unixtime();

    uint32_t diff = (rtcUnix > cfg.lastNtpSync)
                    ? (rtcUnix - cfg.lastNtpSync)
                    : (cfg.lastNtpSync - rtcUnix);

    return (diff > NTP_MAX_DIFF_SECONDS);
}

// ---------------- Hilfsfunktionen ----------------
void printTimeFromUnix(uint32_t unixTime, const char* label) {
    DateTime t = DateTime(unixTime);
    //debugPrintf("%s %02d:%02d:%02d\n", label, t.hour(), t.minute(), t.second());
}

void printDurationHMS(uint32_t seconds, const char* label) {
    uint32_t h = seconds / 3600;
    uint32_t m = (seconds % 3600) / 60;
    uint32_t s = seconds % 60;
    //debugPrintf("%s %02u:%02u:%02u\n", label, h, m, s);
}


// cachedRTC regelmäßig aktualisieren
void updateCachedRTC() {
    unsigned long nowMs = millis();
    unsigned long delta = nowMs - lastRTCMillis;

    if (delta >= 1000) { // jede Sekunde
        cachedRTC = cachedRTC + TimeSpan(delta / 1000); // 1 Sekunde addieren
        lastRTCMillis = nowMs;
    }
}
DateTime nowRTC() { 
    updateCachedRTC();   // Cache vor der Rückgabe aktualisieren
    return cachedRTC; 
}

String timeStr() { // nur für Webseite
    DateTime now = nowRTC();    
    char buf[9];
    sprintf(buf, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
    return String(buf);
}


bool StummZeitAktiv() {
    if (!cfg.noSoundTimerEnable) return false;
    DateTime now = nowRTC();  // RTC-Zeit verwenden
    int currentMinutes = now.hour() * 60 + now.minute();
    
    for (int i = 0; i < 3; i++) {
        if (cfg.timerStart[i] == 0 && cfg.timerEnd[i] == 0) continue;
        int s = cfg.timerStart[i], e = cfg.timerEnd[i];
        if (s <= e && currentMinutes >= s && currentMinutes < e) return true;
        if (s > e && (currentMinutes >= s || currentMinutes < e)) return true;
    }
    return false;
}

void handleAutoFolder() {
    if(!cfg.autoFolderEnable) return;
    DateTime now = nowRTC();
    int minutes = now.hour()*60 + now.minute();
    bool changed = false;
    for(int i=0;i<6;i++){
        if(cfg.autoFolder[i]==-1) continue;
        int s = cfg.autoFolderStart[i], e = cfg.autoFolderEnd[i];
        bool active = (s<=e && minutes>=s && minutes<e) || (s>e && (minutes>=s || minutes<e));
        if(active){
            if(savedFolderSelected==-1) savedFolderSelected = cfg.folderSelected;
            cfg.folderSelected = cfg.autoFolder[i];
            changed = true;
            break;
        }
    }
    if(!changed && savedFolderSelected!=-1){ cfg.folderSelected = savedFolderSelected; savedFolderSelected=-1; }
}

int checkVolumeTimer() {
    int vol = cfg.volume;
    if(!cfg.volumeTimerEnable) return vol;
    DateTime now = nowRTC();
    int minutes = now.hour()*60 + now.minute();
    for(int i=0;i<3;i++){
        int s = cfg.volumeTimerStart[i], e = cfg.volumeTimerEnd[i];
        bool active = (s<=e && minutes>=s && minutes<e) || (s>e && (minutes>=s || minutes<e));
        if(active){ vol = cfg.volumeTimerVol[i]; break; }
    }
    return vol;
}

// ---------------- PLAYER ----------------
bool initPlayer() {
    digitalWrite(BC337_PIN,HIGH); delay(200);
    mp3Serial.begin(9600);
    if(myDFPlayer.begin(mp3Serial)){ 
        playerReady = true; 
        myDFPlayer.volume(cfg.volume);
        return true;
    }
    return false;
}

void playMP3NonBlocking() {
    if (!playerReady) return;
    if(mp3Playing) return; // gerade läuft, nichts tun
    if(pirSperrZeitcpy > 0) return; // nichts spielen, pirSperrZeitcpy ist nicht abgelaufen

    DateTime now = nowRTC();                 // RTC-Zeit nur einmal abfragen
    int currentMinutes = now.hour() * 60 + now.minute();

    // ----- Stummzeit prüfen -----
    if (cfg.noSoundTimerEnable) {
        for (int i = 0; i < 3; i++) {
            int s = cfg.timerStart[i], e = cfg.timerEnd[i];
            if (s == 0 && e == 0) continue;
            bool stumm = (s <= e) ? (currentMinutes >= s && currentMinutes < e)
                                  : (currentMinutes >= s || currentMinutes < e);
            if (stumm) {
                //debugPrintln("Stummzeit aktiv → Track wird nicht abgespielt");
                return;
            }
        }
    }

    int folder = cfg.folderSelected;
    int maxTrack = cfg.trackCount[folder];
    if (maxTrack < 1) maxTrack = 1;

    int trackNum = 1;
    static int lastTrack = 0;

    // Track auswählen
    if(cfg.playMode == 0){ // Zufall
        trackNum = random(1, maxTrack + 1);
    }
    else if(cfg.playMode == 1){ // Reihenfolge
        lastTrack = cfg.lastPlayedTrack[folder];      // letzten Track laden
        lastTrack++;
        if(lastTrack > maxTrack) lastTrack = 1;
        trackNum = lastTrack;
        cfg.lastPlayedTrack[folder] = lastTrack;      // aktualisieren
        saveConfig();                                  // nur hier speichern
    }
    else if(cfg.playMode == 2){ // nur 001
        trackNum = 1;
    }
    // ----- Lautstärke-Timer prüfen -----
    int volToSet = cfg.volume; // Standardlautstärke
    if (cfg.volumeTimerEnable) {
        for (int i = 0; i < 3; i++) {
            int s = cfg.volumeTimerStart[i], e = cfg.volumeTimerEnd[i];
            if (s == 0 && e == 0) continue;
            bool active = (s <= e) ? (currentMinutes >= s && currentMinutes < e)
                                   : (currentMinutes >= s || currentMinutes < e);
            if (active) {
                volToSet = cfg.volumeTimerVol[i];
                break;
            }
        }
    }

    myDFPlayer.volume(volToSet); // Lautstärke vor Trackstart setzen

    // Track abspielen
    if (folder == 0) myDFPlayer.play(trackNum);
    else myDFPlayer.playFolder(folder, trackNum);

    // MP3-Status aktualisieren
    mp3Playing = true;
    mp3StartTime = millis();
    mp3Folder = folder;
    mp3Track = trackNum;
    lastTrackTime = millis();
    //debugPrintf("Track gestartet: Folder %d, Track %d, Volume %d\n", folder, trackNum, volToSet);
    // Track gestartet ==> 
    PirSperrZeitAbwarten = false;
}


void handlePirTrigger() {
    if (!pirTriggered) return;
    if (millis() < pirSperrEndTime) return; // noch sperren

    // Kurze Verzögerung, um Signalstabilität zu prüfen
    if (millis() - pirHighStart < 200) return;

    pirTriggered = false;  // Trigger verarbeiten

    // RTC einmal abfragen
    DateTime now = nowRTC();
    int currentMinutes = now.hour() * 60 + now.minute();

    // ----- Stummzeit prüfen -----
    if (cfg.noSoundTimerEnable) {
        for (int i = 0; i < 3; i++) {
            int s = cfg.timerStart[i], e = cfg.timerEnd[i];
            if (s == 0 && e == 0) continue;
            bool stumm = (s <= e) ? (currentMinutes >= s && currentMinutes < e)
                                  : (currentMinutes >= s || currentMinutes < e);
            if (stumm) {
                //debugPrintln("[handlePirTrigger] PIR ausgelöst, aber Stummzeit aktiv → nichts abspielen");
                return; // Kein Track starten
            }
        }
    }

    // ----- Ordnerwechsel prüfen -----
    handleAutoFolder();

    // Track abspielen
    playMP3NonBlocking();

    // WLAN aktivieren (falls nötig)
    WIFI_ACTIVE_START = millis();
}


void checkWifiTimeout() {
    if(mp3Playing) return; // während Abspielen Timer nicht ablaufen
    if (!wifiActive) return;

    unsigned long maxDuration = StummZeitAktiv() ? WIFI_STUMM_DURATION : WIFI_ACTIVE_DURATION;

    if (millis() - WIFI_ACTIVE_START >= maxDuration) {
        // verbleibende PIR-Sperre für nächsten Boot abspeichern
        if (PirSperrZeitAbwarten && pirSperrZeitcpy > 0) {
            cfg.pirSperrZeitRest = pirSperrZeitcpy;

            // RTC-Zeit vor DeepSleep speichern
            DateTime now = nowRTC();
            cfg.lastSleepUnix = now.unixtime();

            printDurationHMS(cfg.pirSperrZeitRest, "[checkWifiTimeout] PIR Sperrzeit Rest für nächsten Boot:");
            printTimeFromUnix(cfg.lastSleepUnix, "[checkWifiTimeout] DeepSleep-Zeitpunkt gespeichert:");

            saveConfig();
        } 

        // WLAN abschalten und DeepSleep starten
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        wifiActive = false;
        digitalWrite(BC337_PIN, LOW);
        delay(200);
        //debugPrintf("[checkWifiTimeout] Timer abgelaufen => DeepSleep\n");

        // Jetzt DeepSleep starten
        ESP.deepSleep(0);
    }
}


// ---------------- WEBUI ----------------
bool showSavedInfo = false;
void handleRoot() {
    WIFI_ACTIVE_START = millis();
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", ""); // Header senden

    // ----- HEAD + CSS -----
    server.sendContent_P(HTML_HEAD);
    yield();
    
    // ----- Versionsblock -----
    server.sendContent("<div style='background-color:#d0f0c0;padding:10px;border-radius:8px;margin-bottom:10px;'>");
    server.sendContent("<h1 style='margin:0;color:#006400;'>Zwitscherbox Konfiguration</h1>");
    server.sendContent("<div style='font-weight:bold;color:#004d00;'>");
    server.sendContent(FW_INFO); // const char*
    server.sendContent(" ");
    server.sendContent(FW_VERSION); // String oder char*
    server.sendContent("</div></div>");
    
    // ----- Zeit / IP / RAM Block -----
    server.sendContent("<div style='background-color:#c0e0f0;padding:10px;border-radius:8px;margin-bottom:10px;'>");
    server.sendContent("<h2 style='margin:0;'>");
    server.sendContent(timeStr());
    server.sendContent("</h2>");
    server.sendContent("<div>");
    server.sendContent(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "nicht verbunden");
    server.sendContent(" / http://" + String(cfg.deviceName) + ".local / " );
    server.sendContent(WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI())+" dBm" : "-");
    server.sendContent(" / ");
    server.sendContent(String(ESP.getFreeHeap()));
    server.sendContent(" bytes frei / ");
    // RTC als kleiner klickbarer farbiger Button / Link
    String rtcColor = rtcAvailable ? "green" : "red";
    server.sendContent("<a href='/rtc' style='padding:1px 3px; "
                       "background-color:" + rtcColor + "; color:white;"
                       "border-radius:1px; text-decoration:none;'>RTC</a>");
    server.sendContent("</div></div>");
    
    // ----- DeepSleep-Zähler (optional) -----
    //server.sendContent("<div style='background-color:#fff3b0;padding:10px;border-radius:8px;margin-bottom:10px;'>");
    //server.sendContent("DeepSleep in: ");
    //server.sendContent(String(updateWifiCountdown()));
    //server.sendContent(" s</div>");
    
    yield();

    // ----- WLAN + Geräte-Name -----
    server.sendContent("<label>WLAN SSID:<input type='text' name='ssid' value='");
    server.sendContent(cfg.ssid);
    server.sendContent("' maxlength=25></label>");
    server.sendContent("<label>WLAN Passwort:<input type='password' name='pwd' placeholder='********'></label>");
    server.sendContent("<label>Gerätename:<input type='text' name='deviceName' value='");
    server.sendContent(cfg.deviceName);
    server.sendContent("' maxlength=25></label>");
    yield();

    // ----- MP3 Ordner -----
    server.sendContent("<label>Default mp3 Ordner:<select name='folder'>");
    for (int i = 0; i <= 10; i++) {
        server.sendContent("<option value='" + String(i) + "'");
        if(cfg.folderSelected == i) server.sendContent(" selected");
        server.sendContent(">");
        server.sendContent(i == 0 ? "ROOT" : String(i));
        server.sendContent("</option>");
        if(i % 2 == 0) yield();  // Watchdog-freundlich
    }
    server.sendContent("</select></label>");
    yield();

    // ----- Playmode -----
    server.sendContent("<label>abspielen:<select name='playmode'>");
    server.sendContent("<option value='0'" + String(cfg.playMode==0?" selected":"") + ">Zufall</option>");
    server.sendContent("<option value='1'" + String(cfg.playMode==1?" selected":"") + ">nach der Reihe</option>");
    server.sendContent("<option value='2'" + String(cfg.playMode==2?" selected":"") + ">nur 001.mp3</option>");
    server.sendContent("</select></label>");
    yield();

    // ----- Lautstärke / Pause -----
    server.sendContent("<div class='timer-line'><div><label>Lautstärke:</label><input type='number' name='volume' value='");
    server.sendContent(String(cfg.volume));
    server.sendContent("' min=0 max=30></div>");
    server.sendContent("<div><label>Trackpause (1-300s):</label><input type='number' name='pause' value='");
    server.sendContent(String(cfg.pirSperrZeit));
    server.sendContent("' min=1 max=300></div></div><br>");
    yield();

    // ----- Ordnerverwaltung -----
    server.sendContent("<h3>Ordner</h3><table><tr><th>Ordner</th><th>Anzahl Tracks</th><th>Bezeichnung</th></tr>");
    for(int i=0; i<=10; i++) {
        server.sendContent("<tr><td>" + String(i==0?"ROOT":String(i)) + "</td>");
        server.sendContent("<td><input type='number' name='t"+String(i)+"' value='"+String(cfg.trackCount[i])+"' min=0 max=255 style='width:55px;'></td>");
        server.sendContent("<td><input type='text' name='n"+String(i)+"' value='"+cfg.folderName[i]+"' maxlength='20' style='width:155px;'></td></tr>");
        if(i % 2 == 0) yield();
    }
    server.sendContent("</table>");
    yield();

    // ----- AutoFolder -----
    server.sendContent("<h3>Ordnerwechsel <input type='checkbox' name='autoFolderEnable'");
    if(cfg.autoFolderEnable) server.sendContent(" checked");
    server.sendContent("></h3><table><tr><th>Ordner</th><th>von</th><th>bis</th></tr>");
    for(int i=0;i<6;i++){
        server.sendContent("<tr><td><select name='af"+String(i)+"f'>");
        server.sendContent("<option value='-1'" + String(cfg.autoFolder[i]==-1?" selected":"") + ">kein</option>");
        server.sendContent("<option value='0'" + String(cfg.autoFolder[i]==0?" selected":"") + ">ROOT</option>");
        for(int f=1; f<=10; f++){
            server.sendContent("<option value='"+String(f)+"'");
            if(cfg.autoFolder[i]==f) server.sendContent(" selected");
            server.sendContent(">"+String(f)+"</option>");
        }
        server.sendContent("</select></td>");
        server.sendContent("<td><input type='number' name='af"+String(i)+"sh' value='"+String(cfg.autoFolderStart[i]/60)+"' min=0 max=23 style='width:55px;'> : <input type='number' name='af"+String(i)+"sm' value='"+String(cfg.autoFolderStart[i]%60)+"' min=0 max=59 style='width:55px;'></td>");
        server.sendContent("<td><input type='number' name='af"+String(i)+"eh' value='"+String(cfg.autoFolderEnd[i]/60)+"' min=0 max=23 style='width:55px;'> : <input type='number' name='af"+String(i)+"em' value='"+String(cfg.autoFolderEnd[i]%60)+"' min=0 max=59 style='width:55px;'></td></tr>");
        yield();
    }
    server.sendContent("</table>");
    yield();

    // ----- Stummzeiten -----
    server.sendContent("<h3>Stummzeiten <input type='checkbox' name='noSoundTimerEnable'");
    if(cfg.noSoundTimerEnable) server.sendContent(" checked");
    server.sendContent("></h3><table><tr><th>Timer</th><th>von</th><th>bis</th></tr>");
    for(int i=0;i<3;i++){
        server.sendContent("<tr><td>"+String(i+1)+"</td>");
        server.sendContent("<td><input type='number' name='t"+String(i+1)+"sh' value='"+String(cfg.timerStart[i]/60)+"' min=0 max=23 style='width:55px;'> : <input type='number' name='t"+String(i+1)+"sm' value='"+String(cfg.timerStart[i]%60)+"' min=0 max=59 style='width:55px;'></td>");
        server.sendContent("<td><input type='number' name='t"+String(i+1)+"eh' value='"+String(cfg.timerEnd[i]/60)+"' min=0 max=23 style='width:55px;'> : <input type='number' name='t"+String(i+1)+"em' value='"+String(cfg.timerEnd[i]%60)+"' min=0 max=59 style='width:55px;'></td></tr>");
        yield();
    }
    server.sendContent("</table>");
    yield();

    // ----- Lautstärke-Timer -----
    server.sendContent("<h3>Lautstärke <input type='checkbox' name='volumeTimerEnable'");
    if(cfg.volumeTimerEnable) server.sendContent(" checked");
    server.sendContent("></h3><table><tr><th>Lautstärke</th><th>von</th><th>bis</th></tr>");
    for(int i=0;i<3;i++){
        server.sendContent("<tr><td><input type='number' name='v"+String(i+1)+"vol' value='"+String(cfg.volumeTimerVol[i])+"' min=0 max=30 style='width:55px;'></td>");
        server.sendContent("<td><input type='number' name='v"+String(i+1)+"sh' value='"+String(cfg.volumeTimerStart[i]/60)+"' min=0 max=23 style='width:55px;'> : <input type='number' name='v"+String(i+1)+"sm' value='"+String(cfg.volumeTimerStart[i]%60)+"' min=0 max=59 style='width:55px;'></td>");
        server.sendContent("<td><input type='number' name='v"+String(i+1)+"eh' value='"+String(cfg.volumeTimerEnd[i]/60)+"' min=0 max=23 style='width:55px;'> : <input type='number' name='v"+String(i+1)+"em' value='"+String(cfg.volumeTimerEnd[i]%60)+"' min=0 max=59 style='width:55px;'></td></tr>");
        yield();
    }
    server.sendContent("</table>");
    yield();

    if(showSavedInfo) server.sendContent("<div class='info'>Einstellungen gespeichert!</div>");
    showSavedInfo = false;

    server.sendContent_P(HTML_FOOT);
    yield();
}




void handleSave() {
    WIFI_ACTIVE_START = millis(); // WLAN aktiv halten

    // ----- WLAN -----
    if(server.hasArg("ssid")) strlcpy(cfg.ssid, server.arg("ssid").c_str(), sizeof(cfg.ssid));
    if(server.hasArg("pwd") && server.arg("pwd").length() > 0)
        strlcpy(cfg.pwd, server.arg("pwd").c_str(), sizeof(cfg.pwd));

   // Gerätename
    if(server.hasArg("deviceName")) {
        String name = server.arg("deviceName");
        name.trim();                 // Leerzeichen entfernen
        if(name.length() > 25) name = name.substring(0,25); // max 25 Zeichen
        strncpy(cfg.deviceName, name.c_str(), sizeof(cfg.deviceName)-1);
        cfg.deviceName[sizeof(cfg.deviceName)-1] = '\0';  // sicherstellen, dass es nullterminiert ist    
    }


    // ----- Grundfunktionen -----
    if(server.hasArg("pause")) { 
        int p = server.arg("pause").toInt(); 
        cfg.pirSperrZeit = constrain(p, 1, 300);
    }
    if(server.hasArg("volume")) cfg.volume = server.arg("volume").toInt();
    if(server.hasArg("folder")) cfg.folderSelected = server.arg("folder").toInt();
    if(server.hasArg("playmode")) cfg.playMode = server.arg("playmode").toInt();

    // ----- Trackanzahl -----
    for(int i=0;i<=10;i++) {
        if(server.hasArg("t"+String(i))) cfg.trackCount[i] = server.arg("t"+String(i)).toInt();
    }

    // ----- Ordnerbezeichnungen -----
    for(int i=0;i<=10;i++){
        if(server.hasArg("n"+String(i))) strlcpy(cfg.folderName[i], server.arg("n"+String(i)).c_str(), sizeof(cfg.folderName[i]));
    }

    // ----- Stummzeiten -----
    cfg.noSoundTimerEnable = server.hasArg("noSoundTimerEnable");
    for(int i=0;i<3;i++){
        int sh=0, sm=0, eh=0, em=0;
        if(server.hasArg("t"+String(i+1)+"sh")) sh = server.arg("t"+String(i+1)+"sh").toInt();
        if(server.hasArg("t"+String(i+1)+"sm")) sm = server.arg("t"+String(i+1)+"sm").toInt();
        if(server.hasArg("t"+String(i+1)+"eh")) eh = server.arg("t"+String(i+1)+"eh").toInt();
        if(server.hasArg("t"+String(i+1)+"em")) em = server.arg("t"+String(i+1)+"em").toInt();
        cfg.timerStart[i] = sh*60 + sm;
        cfg.timerEnd[i]   = eh*60 + em;
    }

    // ----- AutoFolder -----
    cfg.autoFolderEnable = server.hasArg("autoFolderEnable");
    for(int i=0;i<6;i++){
        int sh=0, sm=0, eh=0, em=0;
        if(server.hasArg("af"+String(i)+"sh")) sh = server.arg("af"+String(i)+"sh").toInt();
        if(server.hasArg("af"+String(i)+"sm")) sm = server.arg("af"+String(i)+"sm").toInt();
        if(server.hasArg("af"+String(i)+"eh")) eh = server.arg("af"+String(i)+"eh").toInt();
        if(server.hasArg("af"+String(i)+"em")) em = server.arg("af"+String(i)+"em").toInt();
        cfg.autoFolderStart[i] = sh*60 + sm;
        cfg.autoFolderEnd[i]   = eh*60 + em;
        if(server.hasArg("af"+String(i)+"f")) cfg.autoFolder[i] = server.arg("af"+String(i)+"f").toInt();
    }

    // ----- Lautstärke-Timer -----
    cfg.volumeTimerEnable = server.hasArg("volumeTimerEnable");
    for(int i=0;i<3;i++){
        int vol = cfg.volume;
        int sh=0, sm=0, eh=0, em=0;
        if(server.hasArg("v"+String(i+1)+"vol")) vol = server.arg("v"+String(i+1)+"vol").toInt();
        if(server.hasArg("v"+String(i+1)+"sh")) sh = server.arg("v"+String(i+1)+"sh").toInt();
        if(server.hasArg("v"+String(i+1)+"sm")) sm = server.arg("v"+String(i+1)+"sm").toInt();
        if(server.hasArg("v"+String(i+1)+"eh")) eh = server.arg("v"+String(i+1)+"eh").toInt();
        if(server.hasArg("v"+String(i+1)+"em")) em = server.arg("v"+String(i+1)+"em").toInt();
        cfg.volumeTimerVol[i]   = vol;
        cfg.volumeTimerStart[i] = sh*60 + sm;
        cfg.volumeTimerEnd[i]   = eh*60 + em;
    }

    // ----- Konfiguration speichern -----
    saveConfig();

    showSavedInfo = true;
    handleRoot(); // direkt zur Übersicht zurück
}

void handleAutoRefresh() {
    WIFI_ACTIVE_START = millis(); // WLAN aktiv halten

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", ""); // Header senden, Body kommt per sendContent

    server.sendContent(F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"));
    server.sendContent(F("<meta name='viewport' content='width=device-width, initial-scale=1'>"));
    server.sendContent(F("<meta http-equiv='refresh' content='10'>")); // Seite alle 10s neu
    server.sendContent(F("<style>body{font-family:Arial;text-align:center;margin-top:20%;}</style>"));
    server.sendContent(F("</head><body>"));
    server.sendContent(F("<h2>Auto-Refresh aktiv</h2>"));
    server.sendContent(F("<p>Seite lädt alle 10 Sekunden neu</p>"));
    server.sendContent(F("<p>Zum Beenden Fenster/Tab schließen</p>"));
    server.sendContent(F("</body></html>"));
}


// ----------------- WebUI Setup -----------------
// ----------------- WebUI Setup -----------------
void setupWebUI() {
    // Root-Seite
    server.on("/", handleRoot);

    // Einstellungen speichern
    server.on("/save", handleSave);

    // AutoRefresh-Seite
    server.on("/autorefresh", handleAutoRefresh);

    // RTC setzen
    server.on("/rtc", HTTP_GET, []() {
        setRtcFromInternetTime();       // Zeit vom Internet holen und RTC setzen
        server.sendHeader("Location", "/"); // zurück zur Hauptseite
        server.send(303);               // Redirect
    });

    // Neustart
    server.on("/restart", []() {
        server.send(200, "text/html", "<html><body><h2>ESP startet neu...</h2></body></html>");
        delay(500);
        ESP.restart();
    });

    // ---------------- Upload Form (GET) ----------------
    server.on("/upload", HTTP_GET, []() {
        server.send(200, "text/html",
            "<h3>config.json Upload</h3>"
            "<form method='POST' action='/upload' enctype='multipart/form-data'>"
            "<input type='file' name='config'>"
            "<input type='submit' value='Hochladen'>"
            "</form>"
        );
    });

    // ---------------- Upload Verarbeiten (POST) ----------------
    server.on("/upload", HTTP_POST, []() {
        // Diese Funktion wird aufgerufen **nachdem der Upload komplett ist**
        server.send(200, "text/html", "Upload abgeschlossen! Bitte ESP neu starten.");
    }, handleFileUpload); // Hier wird der Upload-Puffer verarbeitet

    // ---------------- Download Config ----------------
    server.on("/download", HTTP_GET, handleDownloadConfig);

    // OTA Update
    httpUpdater.setup(&server, "/update");

    // Start Webserver
    server.begin();
    Serial.println("Webserver gestartet!");
}

// ----------------- Upload Handler ----------------
void handleFileUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.print("Upload Start: "); Serial.println(upload.filename);
        // Nur config.json zulassen
        if(upload.filename != "config.json"){
            Serial.println("Nur config.json erlaubt!");
            return;
        }
        File fsUploadFile = LittleFS.open("/config.json", "w");
        if(fsUploadFile) fsUploadFile.close();
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        File fsUploadFile = LittleFS.open("/config.json", "a");
        if (fsUploadFile) {
            fsUploadFile.write(upload.buf, upload.currentSize);
            fsUploadFile.close();
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        Serial.print("Upload fertig: "); Serial.println(upload.filename);
        loadConfig(); // Config direkt nach Upload laden
    }
}

// ----------------- Download Handler ----------------
void handleDownloadConfig() {
    if(!LittleFS.exists("/config.json")){
        server.send(404, "text/plain", "config.json nicht gefunden");
        return;
    }
    File f = LittleFS.open("/config.json", "r");
    server.streamFile(f, "application/json");
    f.close();
}


// Gibt die verbleibende WLAN-Zeit in Sekunden zurück (non-blocking)
int updateWifiCountdown() {
    if(!wifiActive) return 0;
    unsigned long maxDuration = StummZeitAktiv() ? WIFI_STUMM_DURATION : WIFI_ACTIVE_DURATION;
    if (millis() - WIFI_ACTIVE_START >= maxDuration) return 0;
    unsigned long restMs = maxDuration - (millis() - WIFI_ACTIVE_START);
    return (restMs + 999)/1000;
}


// ---------------- SETUP ----------------
void setup(){
    Serial.begin(115200); delay(100); //debugPrintln("");//debugPrintln("");

    // Kondensator-Spannung prüfen
    int wakeUpSpannung = analogRead(A0); 

    // Firmware Version
    //debugPrintln("Firmware: " + String(FW_INFO) + " " + String(FW_VERSION));

    // RTC Clock verbinden
    Wire.begin(SDA_PIN, SCL_PIN);
    rtcAvailable = rtc.begin();
    if(!rtcAvailable){ 
        //debugPrintln("RTC nicht gefunden! Fallback aktiv"); 
    }

    // RTC initial auslesen und Cache setzen
    DateTime now;
    if(rtcAvailable){
        cachedRTC = rtc.now();
        lastRTCMillis = millis();
        now = cachedRTC;
        //debugPrintf("setup - RTC Zeit: %02d:%02d:%02d\n", now.hour(), now.minute(), now.second());
    } else {
        // Fallback: millis() als Sekunden seit Start
        now = DateTime(millis() / 1000);
        //debugPrintln("setup - RTC nicht verfügbar → millis() als Zeitbasis genutzt");
    }

    // LittleFS Werte laden
    loadConfig();

if(cfg.lastSleepUnix > 0){
    slept = now.unixtime() - cfg.lastSleepUnix;  // Dauer seit letztem DeepSleep
    printDurationHMS(slept, "setup - DeepSleep Dauer lt. RTC:");

    if(slept >= cfg.pirSperrZeitRest){  // Sperre abgelaufen
        PirSperrZeitAbwarten = false;
        pirSperrZeitcpy = 0;
        cfg.pirSperrZeitRest = 0;
        //debugPrintln("setup - deepSleepZeit > pirSperrZeit → entsperrt");
    } else { // Restzeit berechnen
        pirSperrZeitcpy = cfg.pirSperrZeitRest - slept;
        PirSperrZeitAbwarten = true;
        cfg.pirSperrZeitRest = pirSperrZeitcpy;
        printDurationHMS(pirSperrZeitcpy, "setup - PIR Sperrzeit Rest:");
    }
} else {
    //debugPrintln("setup - Kein letzter DeepSleep-Zeitpunkt gespeichert");
    pirSperrZeitcpy = 0;
    PirSperrZeitAbwarten = false;
}

    
    // Hardware initialisieren
    pinMode(PIR_PIN, INPUT);
    pinMode(BC337_PIN, OUTPUT);
    pinMode(Steuer_PIN, OUTPUT);
    digitalWrite(Steuer_PIN, LOW);

    // PIR Interrupt setzen    
    attachInterrupt(digitalPinToInterrupt(PIR_PIN), pirISR, RISING);

    // Player initialisieren
    //debugPrintln("");
    if(initPlayer()) {
      //debugPrintln("setup - Player init ok"); 
    } else {
        //debugPrintln("setup - Player init fehlgeschlagen");
    }

    // im Setup gleich losspielen wenn pirSperrZeit, Stummzeit, ...
    // PIR bei Reset auslösen
    //debugPrintln("");
        if(!StummZeitAktiv() && !PirSperrZeitAbwarten){ 
            handleAutoFolder();
            playMP3NonBlocking();
            //debugPrintln("setup - Track sofort gestartet (RTC-Zeit)");
        } else {
            //debugPrintln("setup - Stummzeit aktiv → Track wird nicht abgespielt");
        }

    // WLAN starten
 //   if (strlen(cfg.ssid) > 0) {
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);

        unsigned long wifiStart = millis();   // <--- Startzeit erfassen
        WiFi.begin(cfg.ssid, cfg.pwd);

        while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000)
            delay(200);

        unsigned long wifiStartDauer = millis() - wifiStart;   // <--- Dauer berechnen

        //debugPrintln("");
        //debugPrintf("setup - WLAN-Start dauerte: %d s\n", wifiStartDauer / 1000);

        pirSperrZeitcpy = pirSperrZeitcpy - (wifiStartDauer / 1000);
//debugPrintf("setup - WiFi.status(): %d\n", WiFi.status());

if (WiFi.status() == WL_CONNECTED) {
    //debugPrintln("setup - WLAN verbunden, AP deaktiviert");
    wifiActive = true;

    // ---- mDNS starten ----
    if (MDNS.begin(cfg.deviceName)) {
        MDNS.addService("http", "tcp", 80);
        //debugPrintf("mDNS gestartet: http://%s.local\n", cfg.deviceName);
    } //else {
        //debugPrintln("mDNS Start fehlgeschlagen!");
    //}

    // <<< IP-Ausgabe >>>
    //debugPrintf("setup - WLAN IP: ");
    //debugPrintln(WiFi.localIP().toString());

} else {
    //debugPrintln("setup - WLAN fehlgeschlagen → Starte AP");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Zwitscherbox_AP");
    apActive = true;

    // <<< AP-IP-Ausgabe >>>
    //debugPrintf("setup - AP IP: ");
    //debugPrintln(WiFi.softAPIP().toString());
}
    if (needsNtpSync()) {
        setRtcFromInternetTime();
    }


    // Webserver starten
    setupWebUI();
}

// ---------------- LOOP ----------------
void loop(){
  MDNS.update();

    // Verlängere temporäres WLAN bei jeder Client-Aktivität
    WiFiClient client = server.client();
    if (client || mp3Playing) WIFI_ACTIVE_START = millis(); // WLAN-Zeit zurücksetzen

    server.handleClient();
    handlePirTrigger();
    checkWifiTimeout();


    updateCachedRTC(); // Cache aktualisieren

    if(millis() - lastWifiPrint >= 1000){
        lastWifiPrint = millis();
        DateTime now2 = nowRTC();
        ////debugPrintf("RTC: %02d:%02d:%02d mp3Playing:%d\n", now2.hour(), now2.minute(), now2.second(), mp3Playing);
        int sec = updateWifiCountdown();
    //debugPrintln("");

        //debugPrintf("loop - WLAN verbleibend: %d s\n", sec);
        // wenn Track fertig dann pirSperrZeit abwarten
        if (PirSperrZeitAbwarten && pirSperrZeitcpy > 0) {
            pirSperrZeitcpy--;
            cfg.pirSperrZeitRest = pirSperrZeitcpy;
        //debugPrintf("loop - pirSperrZeitcpy verbleibend: %d s\n", pirSperrZeitcpy);
        } else if (PirSperrZeitAbwarten && pirSperrZeitcpy == 0) {
            PirSperrZeitAbwarten = false;
        }

    }

    if (myDFPlayer.available()) {
        uint8_t type = myDFPlayer.readType();
        int value = myDFPlayer.read(); // zusätzliche Infos, z.B. Tracknummer
    
        if(type == DFPlayerPlayFinished) {
            mp3Playing = false;  // Track ist fertig
            //debugPrintln("loop - Track fertig");
            // wenn Track fertig, dann soll pirSperrZeit anfangen abzulaufen
            PirSperrZeitAbwarten = true;
            // die konfigurierte Zeit soll gewartet werden.
            pirSperrZeitcpy = cfg.pirSperrZeit;
        }
    }
    yield();
}

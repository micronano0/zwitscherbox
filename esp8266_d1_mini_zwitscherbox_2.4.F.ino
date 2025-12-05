
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



// ---------------- VERSION ----------------
#define FW_VERSION "2.4.F"
#define FW_INFO "micronano & ChatGPT 11/2025"

// ---------------- CONFIG ----------------
struct ZwitscherConfig {
    char ssid[25];
    char pwd[40];
    bool autoFolderEnable;
    bool noSoundTimerEnable;
    int manualHour;
    int manualMinute;
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
const unsigned long WIFI_ACTIVE_DURATION = 20000UL; // nach 20 Sekunden ==> DeepSleep
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


// ---------------- NTP ----------------
#define MY_NTP_SERVER "pool.ntp.org"
#define MY_TZ "CET-1CEST,M3.5.0/02,M10.5.0/03"

DateTime cachedRTC;
unsigned long lastRTCMillis = 0;

// ---------------- DEBUG SWITCH ----------------
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
                debugPrintln("Stummzeit aktiv → Track wird nicht abgespielt");
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
    debugPrintf("Track gestartet: Folder %d, Track %d, Volume %d\n", folder, trackNum, volToSet);
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
                debugPrintln("[handlePirTrigger] PIR ausgelöst, aber Stummzeit aktiv → nichts abspielen");
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
        debugPrintf("[checkWifiTimeout] Timer abgelaufen => DeepSleep\n");

        // Jetzt DeepSleep starten
        ESP.deepSleep(0);
    }
}


// ---------------- WEBUI ----------------
bool showSavedInfo = false;
void handleRoot() {
  
    if(strlen(cfg.ssid) == 0 && WiFi.SSID() != ""){
        strlcpy(cfg.ssid, WiFi.SSID().c_str(), sizeof(cfg.ssid));
        strlcpy(cfg.pwd, WiFi.psk().c_str(), sizeof(cfg.pwd));
    }

    String ipStr = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "nicht verbunden";
    String rssiStr = WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) + " dBm" : "-";

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:Arial,sans-serif;margin:10px;padding:0;text-align:center;background:#f5f5f5;}h1,h2,h3{margin:5px;} .fw-info{font-size:0.8em;color:gray;margin-bottom:10px;} form{max-width:600px;margin:auto;background:white;padding:15px;border-radius:10px;box-shadow:0 2px 8px rgba(0,0,0,0.2);}input[type=number],input[type=text],input[type=password],select{width:100%;padding:8px;margin:5px 0;border-radius:5px;border:1px solid #ccc;} input[type=submit],button{padding:10px;font-weight:bold;border:none;border-radius:5px;cursor:pointer;} input[type=submit]{background-color:#4CAF50;color:white;}button{background-color:#f44336;color:white;}table{border-collapse:collapse;margin:10px auto;width:100%;max-width:600px;} th,td{padding:8px 10px;text-align:center;border:1px solid #ccc;} th{background-color:#4CAF50;color:white;} tr:nth-child(even){background-color:#f2f2f2;} tr:hover{background-color:#ddd;} .button-group{display:flex;justify-content:space-between;flex-wrap:wrap;} .button-group input,.button-group button{width:48%;margin:5px 0;} .timer-line{display:flex;justify-content:center;gap:5px;margin:5px 0;} .timer-line input{width:60px;} @media(max-width:500px){input[type=number],input[type=text],input[type=password],select{width:100%;}.button-group input,.button-group button{width:100%;}}</style></head><body>";
    html += "<h1>Zwitscherbox Konfiguration</h1><div class='fw-info'>" + String(FW_INFO) + " " + FW_VERSION + "</div>";
    html += "<h2>" + timeStr() + "</h2>";
    html += "<div class='fw-info'>IP-Adresse: " + ipStr + " / Signalstärke: " + rssiStr +  "</div>";

    html += "<form action='/save' method='POST'>";


    // WLAN
    // SSID (echte SSID eintragen)
    html += "<label>WLAN SSID:<input type='text' name='ssid' value='" + String(cfg.ssid) + "' maxlength=25></label>";

    // Passwort: Platzhalter ***** anzeigen, echtes Passwort bleibt verborgen
    html += "<label>WLAN Passwort:<input type='password' name='pwd' placeholder='********'></label>";

    // MP3 Einstellungen
    html += "<label>Default MP3 Ordner:<select name='folder'>";
    for (int i = 0; i <= 10; i++) {
        html += "<option value='" + String(i) + "'" + String(cfg.folderSelected == i ? " selected" : "") + ">" + (i == 0 ? "ROOT" : "" + String(i)) + "</option>";
    }
    html += "</select></label>";

    html += "<label>spiele:<select name='playmode'>";
    html += "<option value='0'" + String(cfg.playMode == 0 ? " selected" : "") + ">Zufall</option>";
    html += "<option value='1'" + String(cfg.playMode == 1 ? " selected" : "") + ">nach der Reihe</option>";
    html += "<option value='2'" + String(cfg.playMode == 2 ? " selected" : "") + ">nur 001.mp3</option></select></label>";

    // Lautstärke und Pause nach Track nebeneinander
    html += "<div class='timer-line'><div><label for='volume'>Lautstärke:</label><input type='number' id='volume' name='volume' value='" + String(cfg.volume) + "' min=0 max=30></div>";
    html += "<div><label for='pause'>Trackpause (1-300s):</label><input type='number' id='pause' name='pause' value='" + String(cfg.pirSperrZeit) + "' min=1 max=300></div></div><br>";

    // Tracks pro Ordner
    html += "<h3>Ordner</h3><table><tr><th>Ordner</th><th>Anzahl Tracks</th><th>Bezeichnung</th></tr>";
    for (int i = 0; i <= 10; i++) {
        html += "<tr><td>" + (i == 0 ? "ROOT" : "" + String(i)) + "</td>";
        html += "<td><input type='number' name='t" + String(i) + "' value='" + String(cfg.trackCount[i]) + "' min=0 max=255  style='width:55px;'></td>";
        html += "<td><input type='text' name='n" + String(i) + "' value='" + String(cfg.folderName[i]) + "' maxlength='20'  style='width:155px;'></td></tr>";
    }
    html += "</table>";

    // Auto-Folder
    html += "<h3>Ordnerwechsel <input type='checkbox' name='autoFolderEnable'" + String(cfg.autoFolderEnable ? " checked" : "") + "></h3><table><tr><th>Ordner</th><th>von</th><th>bis</th></tr>";
    for (int i = 0; i < 6; i++) {
        html += "<tr><td><select name='af" + String(i) + "f'><option value='-1'" + String(cfg.autoFolder[i] == -1 ? " selected" : "") + ">kein</option>";
        html += "<option value='0'" + String(cfg.autoFolder[i] == 0 ? " selected" : "") + ">ROOT</option>";
        for (int f = 1; f <= 10; f++) {
            html += "<option value='" + String(f) + "'" + String(cfg.autoFolder[i] == f ? " selected" : "") + ">" + String(f) + "</option>";
        }
        html += "</select></td>";
        html += "<td><input type='number' name='af" + String(i) + "sh' value='" + String(cfg.autoFolderStart[i] / 60) + "' min=0 max=23 style='width:55px;'> : ";
        html += "<input type='number' name='af" + String(i) + "sm' value='" + String(cfg.autoFolderStart[i] % 60) + "' min=0 max=59 style='width:55px;'></td>";
        html += "<td><input type='number' name='af" + String(i) + "eh' value='" + String(cfg.autoFolderEnd[i] / 60) + "' min=0 max=23 style='width:55px;'> : ";
        html += "<input type='number' name='af" + String(i) + "em' value='" + String(cfg.autoFolderEnd[i] % 60) + "' min=0 max=59 style='width:55px;'></td></tr>";
    }
    html += "</table>";

    // Kein Ton Timer
    html += "<h3>Stummzeiten <input type='checkbox' name='noSoundTimerEnable'" + String(cfg.noSoundTimerEnable ? " checked" : "") + "></h3><table><tr><th>Timer</th><th>von</th><th>bis</th></tr>";
    for(int i=0;i<3;i++){
        html += "<tr>";
        html += "<td>"+String(i+1)+"</td>";
        html += "<td><input type='number' name='t"+String(i+1)+"sh' value='"+String(cfg.timerStart[i]/60)+"' min=0 max=23 style='width:55px;'> : ";
        html += "<input type='number' name='t"+String(i+1)+"sm' value='"+String(cfg.timerStart[i]%60)+"' min=0 max=59 style='width:55px;'></td>";
        html += "<td><input type='number' name='t"+String(i+1)+"eh' value='"+String(cfg.timerEnd[i]/60)+"' min=0 max=23 style='width:55px;'> : ";
        html += "<input type='number' name='t"+String(i+1)+"em' value='"+String(cfg.timerEnd[i]%60)+"' min=0 max=59 style='width:55px;'></td>";
        html += "</tr>";
    }
    html += "</table>";

// Lautstärke-Timer
html += "<h3>Lautstärke <input type='checkbox' name='volumeTimerEnable'" + String(cfg.volumeTimerEnable ? " checked" : "") + "></h3>";
html += "<table><tr><th>Lautstärke</th><th>von</th><th>bis</th></tr>";
for (int i = 0; i < 3; i++) {
    html += "<tr>";
    html += "<td><input type='number' name='v" + String(i + 1) + "vol' value='" + String(cfg.volumeTimerVol[i]) + "' min=0 max=30 style='width:55px;'></td>";
    html += "<td><input type='number' name='v" + String(i + 1) + "sh' value='" + String(cfg.volumeTimerStart[i] / 60) + "' min=0 max=23 style='width:55px;'> : ";
    html += "<input type='number' name='v" + String(i + 1) + "sm' value='" + String(cfg.volumeTimerStart[i] % 60) + "' min=0 max=59 style='width:55px;'></td>";
    html += "<td><input type='number' name='v" + String(i + 1) + "eh' value='" + String(cfg.volumeTimerEnd[i] / 60) + "' min=0 max=23 style='width:55px;'> : ";
    html += "<input type='number' name='v" + String(i + 1) + "em' value='" + String(cfg.volumeTimerEnd[i] % 60) + "' min=0 max=59 style='width:55px;'></td>";
    html += "</tr>";
}
html += "</table>";
    // Buttons
    html += "<div class='button-group'><input type='submit' value='Speichern'>";
    html += "<button type='button' onclick=\"location.href='/restart'\">Neustart</button>";
    html += "<button type='button' onclick=\"window.open('/autorefresh','_blank')\">Auto-Refresh</button>";
    html += "</div>";
    // Info
    if (showSavedInfo) html += "<div class='info'>Einstellungen gespeichert!</div>";

    html += "</form></body></html>";

    server.sendHeader("Content-Type", "text/html; charset=UTF-8");
    server.send(200, "text/html", html);
    showSavedInfo = false;
}

void handleAutoRefresh() {
    WIFI_ACTIVE_START = millis(); // WLAN aktiv halten

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<meta http-equiv='refresh' content='5'>"; // Seite lädt jede 5s neu
    html += "<style>body{font-family:Arial;text-align:center;margin-top:20%;}</style>";
    html += "</head><body>";
    html += "<h2>Auto-Refresh aktiv</h2>";
    html += "<p>Seite lädt alle 5 Sekunden neu</p>";
    html += "<p>Zum Beenden Fenster/Tab schließen</p>";
    html += "</body></html>";

    server.send(200, "text/html", html);
}


void handleSave() {
    if(server.hasArg("ssid")) strlcpy(cfg.ssid, server.arg("ssid").c_str(), sizeof(cfg.ssid));
    // Passwort nur speichern, wenn Nutzer etwas eingegeben hat
    if(server.hasArg("pwd") && server.arg("pwd").length() > 0) {
        strlcpy(cfg.pwd, server.arg("pwd").c_str(), sizeof(cfg.pwd));
    }
    if(server.hasArg("pause")) { int p = server.arg("pause").toInt(); if(p<1)p=1; else if(p>300)p=300; cfg.pirSperrZeit=p; }
    if(server.hasArg("volume")) cfg.volume = server.arg("volume").toInt();
    if(server.hasArg("folder")) cfg.folderSelected = server.arg("folder").toInt();
    if(server.hasArg("playmode")) cfg.playMode = server.arg("playmode").toInt();

    for(int i=0;i<=10;i++){ if(server.hasArg("t"+String(i))) cfg.trackCount[i] = server.arg("t"+String(i)).toInt(); }
    for(int i=0;i<3;i++){
        int sh=0, sm=0, eh=0, em=0;
        if(server.hasArg("t"+String(i+1)+"sh")) sh = server.arg("t"+String(i+1)+"sh").toInt();
        if(server.hasArg("t"+String(i+1)+"sm")) sm = server.arg("t"+String(i+1)+"sm").toInt();
        if(server.hasArg("t"+String(i+1)+"eh")) eh = server.arg("t"+String(i+1)+"eh").toInt();
        if(server.hasArg("t"+String(i+1)+"em")) em = server.arg("t"+String(i+1)+"em").toInt();
        cfg.timerStart[i]=sh*60+sm; cfg.timerEnd[i]=eh*60+em;
    }
    for(int i=0;i<=10;i++){
        if(server.hasArg("n"+String(i))) strlcpy(cfg.folderName[i], server.arg("n"+String(i)).c_str(), sizeof(cfg.folderName[i]));
    }
    // Auto-Folder
    for(int i=0;i<6;i++){
        int sh=0, sm=0, eh=0, em=0;
        if(server.hasArg("af"+String(i)+"sh")) sh=server.arg("af"+String(i)+"sh").toInt();
        if(server.hasArg("af"+String(i)+"sm")) sm=server.arg("af"+String(i)+"sm").toInt();
        if(server.hasArg("af"+String(i)+"eh")) eh=server.arg("af"+String(i)+"eh").toInt();
        if(server.hasArg("af"+String(i)+"em")) em=server.arg("af"+String(i)+"em").toInt();
        cfg.autoFolderStart[i]=sh*60+sm;
        cfg.autoFolderEnd[i]=eh*60+em;
        if(server.hasArg("af"+String(i)+"f")) cfg.autoFolder[i]=server.arg("af"+String(i)+"f").toInt();
    }
    cfg.autoFolderEnable = server.hasArg("autoFolderEnable");
    cfg.noSoundTimerEnable = server.hasArg("noSoundTimerEnable");

    cfg.volumeTimerEnable = server.hasArg("volumeTimerEnable");
    for(int i=0;i<3;i++){
        int sh=0, sm=0, eh=0, em=0, vol=cfg.volume;
        if(server.hasArg("v"+String(i+1)+"vol")) vol = server.arg("v"+String(i+1)+"vol").toInt();
        if(server.hasArg("v"+String(i+1)+"sh")) sh = server.arg("v"+String(i+1)+"sh").toInt();
        if(server.hasArg("v"+String(i+1)+"sm")) sm = server.arg("v"+String(i+1)+"sm").toInt();
        if(server.hasArg("v"+String(i+1)+"eh")) eh = server.arg("v"+String(i+1)+"eh").toInt();
        if(server.hasArg("v"+String(i+1)+"em")) em = server.arg("v"+String(i+1)+"em").toInt();
        cfg.volumeTimerVol[i]   = vol;
        cfg.volumeTimerStart[i] = sh*60 + sm;
        cfg.volumeTimerEnd[i]   = eh*60 + em;
    }


    saveConfig();
    showSavedInfo = true;
    handleRoot();
}

// ----------------- WebUI Setup -----------------
void setupWebUI() {
    server.on("/", handleRoot);
    server.on("/save", handleSave);
    server.on("/autorefresh", handleAutoRefresh);

    server.on("/restart", [](){
        server.send(200,"text/html","<html><body><h2>ESP startet neu...</h2></body></html>");
        delay(500); ESP.restart();
    });

    // OTA Update
    httpUpdater.setup(&server, "/update");
    server.begin();
}

// Gibt die verbleibende WLAN-Zeit in Sekunden zurück (non-blocking)
int updateWifiCountdown() {
    if(!wifiActive) return 0;
    unsigned long maxDuration = StummZeitAktiv() ? WIFI_STUMM_DURATION : WIFI_ACTIVE_DURATION;
    if (millis() - WIFI_ACTIVE_START >= maxDuration) return 0;
    unsigned long restMs = maxDuration - (millis() - WIFI_ACTIVE_START);
    return (restMs + 999)/1000;
}

// ---------------- Hilfsfunktionen ----------------
void printTimeFromUnix(uint32_t unixTime, const char* label) {
    DateTime t = DateTime(unixTime);
    debugPrintf("%s %02d:%02d:%02d\n", label, t.hour(), t.minute(), t.second());
}

void printDurationHMS(uint32_t seconds, const char* label) {
    uint32_t h = seconds / 3600;
    uint32_t m = (seconds % 3600) / 60;
    uint32_t s = seconds % 60;
    debugPrintf("%s %02u:%02u:%02u\n", label, h, m, s);
}
// ---------------- SETUP ----------------
void setup(){
    Serial.begin(115200); delay(100); debugPrintln("");debugPrintln("");

    // Kondensator-Spannung prüfen
    int wakeUpSpannung = analogRead(A0); 

    // Firmware Version
    debugPrintln("Firmware: " + String(FW_INFO) + " " + String(FW_VERSION));

    // RTC Clock verbinden
    Wire.begin(SDA_PIN, SCL_PIN);
    bool rtcAvailable = rtc.begin();
    if(!rtcAvailable){ 
        debugPrintln("RTC nicht gefunden! Fallback aktiv"); 
    }

    // RTC initial auslesen und Cache setzen
    DateTime now;
    if(rtcAvailable){
        cachedRTC = rtc.now();
        lastRTCMillis = millis();
        now = cachedRTC;
        debugPrintf("setup - RTC Zeit: %02d:%02d:%02d\n", now.hour(), now.minute(), now.second());
    } else {
        // Fallback: millis() als Sekunden seit Start
        now = DateTime(millis() / 1000);
        debugPrintln("setup - RTC nicht verfügbar → millis() als Zeitbasis genutzt");
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
        debugPrintln("setup - deepSleepZeit > pirSperrZeit → entsperrt");
    } else { // Restzeit berechnen
        pirSperrZeitcpy = cfg.pirSperrZeitRest - slept;
        PirSperrZeitAbwarten = true;
        cfg.pirSperrZeitRest = pirSperrZeitcpy;
        printDurationHMS(pirSperrZeitcpy, "setup - PIR Sperrzeit Rest:");
    }
} else {
    debugPrintln("setup - Kein letzter DeepSleep-Zeitpunkt gespeichert");
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
    debugPrintln("");
    if(initPlayer()) {
      debugPrintln("setup - Player init ok"); 
    } else {
        debugPrintln("setup - Player init fehlgeschlagen");
    }

    // im Setup gleich losspielen wenn pirSperrZeit, Stummzeit, ...
    // PIR bei Reset auslösen
    debugPrintln("");
        if(!StummZeitAktiv() && !PirSperrZeitAbwarten){ 
            handleAutoFolder();
            playMP3NonBlocking();
            debugPrintln("setup - Track sofort gestartet (RTC-Zeit)");
        } else {
            debugPrintln("setup - Stummzeit aktiv → Track wird nicht abgespielt");
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

        debugPrintln("");
        debugPrintf("setup - WLAN-Start dauerte: %d s\n", wifiStartDauer / 1000);

        pirSperrZeitcpy = pirSperrZeitcpy - (wifiStartDauer / 1000);
debugPrintf("setup - WiFi.status(): %d\n", WiFi.status());

if (WiFi.status() == WL_CONNECTED) {
    debugPrintln("setup - WLAN verbunden, AP deaktiviert");
    wifiActive = true;

    // <<< IP-Ausgabe >>>
    debugPrintf("setup - WLAN IP: ");
    debugPrintln(WiFi.localIP().toString());

} else {
    debugPrintln("setup - WLAN fehlgeschlagen → Starte AP");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Zwitscherbox_AP");
    apActive = true;

    // <<< AP-IP-Ausgabe >>>
    debugPrintf("setup - AP IP: ");
    debugPrintln(WiFi.softAPIP().toString());
}
 //   }


    // NTP nur, wenn länger als 1 Woche seit letzter Synchronisation
    uint32_t nowUnix = cachedRTC.unixtime();
    if (nowUnix - cfg.lastNtpSync > 7UL * 24UL * 60UL * 60UL || cfg.lastNtpSync == 0) {
        configTime(MY_TZ, MY_NTP_SERVER);
        debugPrintln("setup - Warte auf NTP-Zeit...");

time_t t = 0;
unsigned long ntpStart = millis();

while (t < 1609459200 && millis() - ntpStart < 10000) {  // max 10 Sekunden
    delay(200);
    t = time(nullptr);
}

if (t < 1609459200) {
    Serial.println("NTP: Timeout! Nutze RTC oder Startzeit.");
} else {
    Serial.println("NTP: Erfolgreich synchronisiert.");
}

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
        cfg.lastNtpSync = cachedRTC.unixtime();
        saveConfig();

        debugPrintf("setup - RTC nach NTP: %02d:%02d:%02d\n", cachedRTC.hour(), cachedRTC.minute(), cachedRTC.second());
    }

    // Webserver starten
    setupWebUI();
}

// ---------------- LOOP ----------------
void loop(){
    if (Serial.available()) {
        char c = Serial.read();
        if(c == '1') serialDebug = true;   // Debug an
        else if(c == '0') serialDebug = false; // Debug aus
    }

/*
if(Serial.available()){
    String cmd = Serial.readStringUntil('\n');
    cmd.trim(); // Leerzeichen entfernen
    cmd.toUpperCase();

    if(cmd == "DEBUG ON"){
        serialDebug = true;
        Serial.println("DEBUG aktiviert");
    } else if(cmd == "DEBUG OFF"){
        serialDebug = false;
        Serial.println("DEBUG deaktiviert");
    }
}
 */
 
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
        //debugPrintf("RTC: %02d:%02d:%02d mp3Playing:%d\n", now2.hour(), now2.minute(), now2.second(), mp3Playing);
        int sec = updateWifiCountdown();
    debugPrintln("");

        debugPrintf("loop - WLAN verbleibend: %d s\n", sec);
        // wenn Track fertig dann pirSperrZeit abwarten
        if (PirSperrZeitAbwarten && pirSperrZeitcpy > 0) {
            pirSperrZeitcpy--;
            cfg.pirSperrZeitRest = pirSperrZeitcpy;
        debugPrintf("loop - pirSperrZeitcpy verbleibend: %d s\n", pirSperrZeitcpy);
        } else if (PirSperrZeitAbwarten && pirSperrZeitcpy == 0) {
            PirSperrZeitAbwarten = false;
        }

    }

    if (myDFPlayer.available()) {
        uint8_t type = myDFPlayer.readType();
        int value = myDFPlayer.read(); // zusätzliche Infos, z.B. Tracknummer
    
        if(type == DFPlayerPlayFinished) {
            mp3Playing = false;  // Track ist fertig
            debugPrintln("loop - Track fertig");
            // wenn Track fertig, dann soll pirSperrZeit anfangen abzulaufen
            PirSperrZeitAbwarten = true;
            // die konfigurierte Zeit soll gewartet werden.
            pirSperrZeitcpy = cfg.pirSperrZeit;
        }
    }
    yield();
}

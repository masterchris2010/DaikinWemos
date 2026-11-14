/*
  DaikinWemos 2.11 - ESP8266 Wemos D1 mini / Daikin S21.
  Pagina web e configurazione Wi-Fi incluse: nessun upload LittleFS.
  Arduino IDE: core "esp8266 by ESP8266 Community" 3.1.2,
  scheda "LOLIN(WEMOS) D1 R2 & mini", CPU 80 MHz, flash 4 MB.
  Tutte le librerie usate sono incluse nel core ESP8266.

  D6 / GPIO12 RX <- CYT1076 LV1 / HV1 <- S21 TX (pin 2).
  D7 / GPIO13 TX -> CYT1076 LV2 / HV2 -> S21 RX (pin 3).
  CYT1076 LV = 3V3; HV = 5 V regolati; tutte le masse in comune.
  Alimentazione S21 pin 4 -> step-down -> Wemos 5V. S21 pin 5 = GND.
  Verificare orientamento, tensioni e cablaggio prima di alimentare.
  Per il flash collegare soltanto la USB, scollegando il Daikin.

  Prima configurazione: rete WiFi-daikin-XXXXXX, password daikinsetup.
  Aprire http://192.168.4.1; impostare il Wi-Fi domestico a 2,4 GHz.
  Dopo la connessione: http://daikin-wemos.local oppure IP del router.
  Se il Wi-Fi non e' raggiungibile, il portale torna dopo 25 secondi.
  Non invia comandi di accensione durante l'avvio.

  Protocollo S21 basato sulla documentazione e sul lavoro di:
  https://github.com/MassiPi/DaikinS21 (autore del protocollo nel progetto sorgente)
  https://github.com/revk/xESP32-Faikout/wiki/S21-Protocol
  Questa versione autonoma implementa i comandi di base; non e' il firmware Daikin.
  Licenza di questo sketch: GPL-3.0-or-later.
  La compatibilita' effettiva con FTXM25M2V1B va verificata sul dispositivo.
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <Updater.h>
#include <time.h>
#include <LittleFS.h>
#include <SoftwareSerial.h>
#include <math.h>
#include <stddef.h>

static const uint8_t MODEL_INPUTS = 12, MODEL_HIDDEN = 8;
static const uint16_t MODEL_VALUES = 161;
static const int MODEL_ADDRESS = 512;
static const uint32_t MODEL_MAGIC = 0x44574E31;
struct NeuralModel {
  uint32_t magic;
  float values[MODEL_VALUES];
  uint32_t checksum;
};
static_assert(MODEL_ADDRESS + sizeof(NeuralModel) <= 2048, "Model exceeds EEPROM");
NeuralModel neuralModel = {};
float neuralPrediction = NAN;
String neuralStatus = "Modello non caricato";
static const char ARCHIVE_CURRENT[] = "/learning-current.csv";
static const char ARCHIVE_PREVIOUS[] = "/learning-previous.csv";
static const uint32_t ARCHIVE_FILE_LIMIT = 256UL * 1024;
static const char ARCHIVE_HEADER[] PROGMEM =
  "utc,timestamp,online,version,mode,fan,room,outside,target,internalTarget,rpm,power,turbo,eco,prediction10m\n";
bool archiveReady = false;
uint32_t archiveDue = 0;
String archiveBuffer;
uint8_t archiveBufferedRows = 0;
String archiveStatus = "Archivio da inizializzare";

float learningRooms[11] = {};
uint32_t learningTimes[11] = {};
uint8_t learningNext = 0, learningCount = 0;
uint32_t learningDue = 0, learningSamples = 0;
float learningTrend = NAN;

uint8_t rxPin = D6, txPin = D7;
static const int PIN_SETTINGS_ADDRESS = 416;
static const uint32_t PIN_SETTINGS_MAGIC = 0x44575031;
struct PinSettings {
  uint32_t magic, rx, tx, checksum;
};
static_assert(PIN_SETTINGS_ADDRESS + sizeof(PinSettings) <= MODEL_ADDRESS, "Pin settings overlap model");
static const uint8_t STX = 0x02, ETX = 0x03, ACK = 0x06, NAK = 0x15;
static const uint32_t WIFI_MAGIC = 0x44574D31;
static const char AP_PASSWORD[] = "daikinsetup";

struct WifiSettings {
  uint32_t magic;
  char ssid[33];
  char password[65];
  uint32_t checksum;
};

// Separate record keeps the Wi-Fi settings from earlier sketches readable.
struct NetworkSettings {
  uint32_t magic;
  uint32_t ip, gateway, mask, dns;
  uint32_t checksum;
};

static const uint32_t STATIC_MAGIC = 0x44574950;
static const int NETWORK_OFFSET = 128;
static_assert(sizeof(WifiSettings) <= NETWORK_OFFSET, "EEPROM records overlap");
NetworkSettings networkSettings = {};

struct AcState {
  bool known = false;
  bool power = false;
  char mode = '3';
  char fan = 'A';
  uint8_t rawSetpoint = 76; // 24 C, used only after a valid F1 response.
  float target = 24.0f;
  float room = NAN, outside = NAN, internalTarget = NAN;
  int fanRpm = -1;
  int8_t turbo = -1, eco = -1;
  bool swingKnown = false, swingV = false, swingH = false;
  uint32_t basicRead = 0;
};

WifiSettings wifiSettings = {};
AcState ac;
// Store the update key separately from the Wi-Fi and IP settings.
static const int UPDATE_KEY_OFFSET = 192;
static const uint32_t UPDATE_KEY_MAGIC = 0x44574F54;
struct FirmwareKey {
  uint32_t magic;
  char value[33];
  uint32_t checksum;
};
static_assert(NETWORK_OFFSET + sizeof(NetworkSettings) <= UPDATE_KEY_OFFSET, "EEPROM records overlap");
static_assert(UPDATE_KEY_OFFSET + sizeof(FirmwareKey) <= 512, "EEPROM record too large");
FirmwareKey firmwareKey = {};
static const int HEAT_OFFSET_ADDRESS = 256;
static const uint32_t HEAT_OFFSET_MAGIC = 0x44574854;
struct HeatOffsetSettings {
  uint32_t magic;
  int32_t halfDegrees;
  uint32_t checksum;
};
static_assert(UPDATE_KEY_OFFSET + sizeof(FirmwareKey) <= HEAT_OFFSET_ADDRESS, "EEPROM records overlap");
static_assert(HEAT_OFFSET_ADDRESS + sizeof(HeatOffsetSettings) <= 512, "EEPROM record too large");
int32_t heatOffsetSteps = 0;

static const uint8_t SCHEDULE_LIMIT = 8;
static const int SCHEDULE_ADDRESS = 288;
static const uint32_t SCHEDULE_MAGIC = 0x44575343;
struct ScheduleRule {
  uint16_t minute;
  uint8_t days, halfDegrees;
  char mode;
  uint8_t enabled;
};
struct ScheduleSettings {
  uint32_t magic;
  ScheduleRule rules[SCHEDULE_LIMIT];
  uint32_t checksum;
};
static_assert(HEAT_OFFSET_ADDRESS + sizeof(HeatOffsetSettings) <= SCHEDULE_ADDRESS, "EEPROM records overlap");
static_assert(SCHEDULE_ADDRESS + sizeof(ScheduleSettings) <= 512, "EEPROM record too large");
ScheduleSettings schedule = {};
uint32_t lastScheduleDay[SCHEDULE_LIMIT] = {};
uint32_t nextScheduleCheck = 0;
String scheduleResult;
bool sleepTimerActive = false;
uint32_t sleepDeadline = 0, nextSleepAttempt = 0;
String sleepResult;
bool firmwareComplete = false;
size_t firmwareSize = 0, firmwareReceived = 0;
String firmwareError;

struct S21Trace {
  uint32_t atMs, elapsedMs;
  uint8_t tx[12], rx[80];
  uint8_t txSize, rxSize;
  bool ok, truncated;
  char error[80];
};
static const uint8_t TRACE_COUNT = 12;
S21Trace debugTraces[TRACE_COUNT] = {};
uint8_t traceNext = 0, traceUsed = 0;
static const int DEBUG_SETTINGS_ADDRESS = 384;
static const uint32_t DEBUG_SETTINGS_MAGIC = 0x44574442;
struct DebugSettings {
  uint32_t magic, enabled, checksum;
};
static_assert(SCHEDULE_ADDRESS + sizeof(ScheduleSettings) <= DEBUG_SETTINGS_ADDRESS, "EEPROM records overlap");
static_assert(DEBUG_SETTINGS_ADDRESS + sizeof(DebugSettings) <= MODEL_ADDRESS, "EEPROM records overlap");
static_assert(DEBUG_SETTINGS_ADDRESS + sizeof(DebugSettings) <= PIN_SETTINGS_ADDRESS, "Pin settings overlap debug");
bool debugEnabled = false;

ESP8266WebServer server(80);
DNSServer dns;
SoftwareSerial s21;
bool apActive = false, mdnsActive = false, wasConnected = false;
uint32_t wifiLostAt = 0, nextPoll = 0, restartAt = 0;
uint8_t pollStep = 0;
String apName;
String lastError = "In attesa della prima risposta S21";

// Prototipi espliciti per Arduino IDE.
uint32_t configChecksum(const void *record, size_t size);
void loadNetwork();
bool readNetworkForm(NetworkSettings &settings);
uint32_t ipv4Number(const IPAddress &address);
void appendJsonString(String &json, const char *value);
bool loadWifi();
bool saveWifi(const String &ssid, const String &password, const NetworkSettings &network);
void startAccessPoint();
void maintainWifi();
bool validMode(char value);
bool validFan(char value);
bool exchangeS21(char c0, char c1, const uint8_t *payload, size_t size,
                 bool expectReply, uint8_t *reply, size_t &replySize);
bool queryBasic();
bool querySwing();
void queryTemperature(char command, float &temperature);
bool sendBasic(bool power, char mode, uint8_t setpoint, char fan);
bool sendSwing(bool vertical, bool horizontal);
void pollAircon();
String temperatureJson(float value);
void handleStatus();
void handleSet();
void handleSwing();
void handleSpecial();
bool readSpecial(char command, uint8_t (&data)[4]);
void handleWifi();
void setupRoutes();
void loadFirmwareKey();
void loadPins();
void savePins();
bool validS21Pin(uint32_t pin);
void loadHeatOffset();
void saveHeatOffset();
void loadSchedule();
void readSchedule();
void saveSchedule();
void tickSchedule();
void setSleepTimer();
void tickSleepTimer();
void loadNeuralModel();
bool validNeuralModel(const NeuralModel &model);
float predictNeuralDelta(const float *normalized, const NeuralModel &model);
void uploadNeuralModel();
void tickLearning();
void loadArchive();
bool flushArchive();
void tickArchive();
void initializeArchive();
void downloadArchive();
size_t archiveBytes();
String csvNumber(float value);
uint32_t sleepSeconds();
bool localClock(struct tm &local);
bool validScheduleRule(const ScheduleRule &rule);
bool parseScheduleNumber(const String &value, uint16_t maximum, uint16_t &result);
void receiveFirmware();
void finishFirmware();
bool debugIsActive();
void loadDebugSettings();
void configureDebug();
void handleDebugLog();
void handleDebugProbe();
void appendTrace(String &json, const S21Trace &trace);
void appendHex(String &json, const uint8_t *bytes, size_t size);

static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="it"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Daikin Wemos</title>
<style>
*{box-sizing:border-box}
:root{color-scheme:light;--blue:#006db2;--ink:#152b42;--muted:#4f6579;--line:#dce6ef}
body{margin:0;background:#eef3f8;color:var(--ink);font:16px system-ui,sans-serif}
header{background:#fff;border-bottom:1px solid var(--line)}
.bar,main{max-width:660px;margin:auto;padding:18px 16px}
.bar{display:flex;align-items:center;justify-content:space-between;gap:10px}
.brand{font-weight:800;font-size:20px}.badge{font-size:13px;font-weight:650;padding:6px 10px;border-radius:20px;background:#eaf0f6}
.good{color:#12634c}.bad{color:#9b3c2d}
.card{background:#fff;border:1px solid var(--line);border-radius:20px;padding:20px;margin-bottom:14px}
h1,h2,p{margin:0}h1{font-size:23px}h2{font-size:17px;margin-bottom:14px}
small,.muted{color:var(--muted)}fieldset{border:0;margin:0;padding:0;min-width:0}
.dialrow{display:flex;align-items:center;justify-content:center;gap:clamp(8px,4vw,28px);margin:22px 0 18px}
.dial{border:5px solid #b3d9f2;border-top-color:#007abb;border-right-color:#007abb;border-radius:50%;width:clamp(132px,42vw,220px);aspect-ratio:1;display:grid;place-content:center;text-align:center;background:#f4faff}
.dial label{font-size:13px;color:var(--muted)}.dialvalue{display:flex;align-items:baseline;justify-content:center}
.dial input{background:transparent;border:0;width:2.8em;text-align:center;font-size:clamp(30px,9vw,48px);font-weight:650;padding:0;appearance:textfield;color:var(--ink)}
.dial input::-webkit-inner-spin-button{appearance:none}.degree{font-size:17px;color:var(--muted)}
button,input{font:inherit}button{cursor:pointer}button:disabled{opacity:.5;cursor:default}
button:focus-visible,input:focus-visible{outline:3px solid #007abb;outline-offset:3px}
.step{width:42px;height:42px;border:1px solid #b3d9f2;border-radius:50%;color:var(--blue);background:#f4faff;font-size:25px;flex:none}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}
.grid label{font-size:13px;color:var(--muted)}.wifi input:not([type=radio]){display:block;width:100%;padding:10px 8px;margin:5px 0 0;border:1px solid #bfcfdd;border-radius:9px;background:white;color:var(--ink)}
.primary{width:100%;padding:12px;border:0;border-radius:10px;background:var(--blue);color:white;font-weight:650;margin-top:14px}
.tile{padding:12px;text-align:left;background:#f5f9fd;color:var(--ink);border:1px solid var(--line);border-radius:12px}
.tile strong,.tile span{display:block}.tile span{font-size:13px;margin-top:4px;color:var(--muted)}
.tile[aria-pressed=true]{background:#dceffd;border-color:#007abb}
.check{display:flex;align-items:center;gap:8px;margin:10px 0}.secondary{padding:9px 12px;background:white;color:var(--blue);border:1px solid var(--line);border-radius:9px}
#diagnostic,#network,#message,#wifiMessage{display:block;margin-top:8px}.wifi label{display:block;margin-top:12px}
summary{cursor:pointer;font-weight:650}[hidden]{display:none!important}.wifi .check input{display:inline;width:auto;margin:0 6px 0 0}

.climate{background:linear-gradient(170deg,#eaf6ff,#fff 46%);box-shadow:0 12px 30px #18476d0b}
.dial{background:white;box-shadow:0 10px 28px #005d9b1a}
.step:hover,.tile:not(:disabled):hover{border-color:#007abb}
.control-groups{display:grid;gap:16px}
.choice-group legend{padding:0;font-size:14px;font-weight:650;color:var(--muted);margin-bottom:7px}
.choices{display:flex;flex-wrap:wrap;gap:7px}
.choices.pair{display:grid;grid-template-columns:1fr 1fr}
.choice{position:relative;display:block}
.choice input{position:absolute;opacity:0;width:1px;height:1px}
.choice span{display:flex;min-height:42px;align-items:center;justify-content:center;padding:8px 12px;
  border:1px solid var(--line);border-radius:12px;background:#f8fbfe;color:var(--ink);font-size:14px;cursor:pointer}
.choice input:checked+span{background:var(--blue);border-color:var(--blue);color:white;font-weight:650}
.choice input:focus-visible+span{outline:3px solid #007abb;outline-offset:2px}
.choice input:disabled+span{opacity:.5;cursor:default}
.fan-choices .choice span{min-width:42px}
.wifi .choice-group{margin-top:14px}
.wifi .choice{margin:0}

:root[data-theme="auto"]{--blue:hsl(var(--mode-hue,207) 85% 34%)}
:root[data-theme="auto"] .climate{background:linear-gradient(160deg,hsl(var(--mode-hue,207) 90% 94%),white 62%)}
:root[data-theme="auto"] .dial{border-color:hsl(var(--temp-hue,207) 65% 81%);border-top-color:hsl(var(--temp-hue,207) 84% 38%);border-right-color:hsl(var(--temp-hue,207) 84% 38%)}
:root[data-theme="dark"]{color-scheme:dark;--blue:#156fa9;--ink:#e6eff8;--muted:#a9bfd2;--line:#395069}
:root[data-theme="dark"] body{background:#101b28}
:root[data-theme="dark"] header,:root[data-theme="dark"] .card{background:#192b3e}
:root[data-theme="dark"] .climate{background:linear-gradient(160deg,#223c56,#192b3e 65%)}
:root[data-theme="dark"] .badge,:root[data-theme="dark"] .tile,:root[data-theme="dark"] .choice span,
:root[data-theme="dark"] .step,:root[data-theme="dark"] .secondary,:root[data-theme="dark"] .dial,
:root[data-theme="dark"] .wifi input:not([type=radio]){background:#243a50;color:var(--ink)}
:root[data-theme="dark"] .good{color:#97dfbd}:root[data-theme="dark"] .bad{color:#ffb5a8}
:root[data-theme="dark"] .choice input:checked+span,:root[data-theme="dark"] .tile[aria-pressed=true]{background:#156fa9;color:white}
.temp-readback{font-size:13px;text-align:center;color:var(--muted);margin:0 0 14px}

.schedule-rule{margin:14px 0;padding:13px;border:1px solid var(--line);border-radius:12px}
.schedule-rule .days{display:flex;flex-wrap:wrap;gap:7px;margin-top:9px}
.schedule-rule .days label{display:inline-flex;align-items:center;gap:3px;margin:0}
.schedule-rule .days input{width:auto;display:inline;margin:0}
.schedule-rule .choice-group{margin:12px 0}
.schedule-rule .remove-rule{margin-top:10px}
#scheduleClock,#scheduleResult{margin:8px 0;color:var(--muted)}

.tabs{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:5px;
  padding:5px;margin-bottom:16px;border:1px solid var(--line);border-radius:14px;background:#fff}
.tabs button{min-height:42px;padding:7px 2px;border:1px solid transparent;border-radius:9px;
  background:transparent;color:var(--muted);font-size:13px;font-weight:650}
.tabs button[aria-selected=true]{background:var(--blue);color:white}
.tabs button:hover:not([aria-selected=true]){border-color:var(--line)}
:root[data-theme="dark"] .tabs{background:#192b3e}
@media (max-width:470px){.tabs{grid-template-columns:repeat(3,minmax(0,1fr))}}

.sensor-circles{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:9px;max-width:280px;margin:12px auto 20px}
.sensor-circle{aspect-ratio:1;border:2px solid var(--sensor-border);border-radius:50%;
  background:var(--sensor-bg);color:var(--sensor-ink);display:flex;flex-direction:column;
  justify-content:center;align-items:center;min-width:0;text-align:center}
.sensor-circle strong{font-size:clamp(20px,5vw,24px);font-weight:700;line-height:1.2;margin:4px 0}
.sensor-label{font-size:clamp(10px,2.8vw,12px);font-weight:600}.sensor-unit{font-size:11px}
.sensor-room{--sensor-bg:#fff1e3;--sensor-border:#f1bb80;--sensor-ink:#83410c}
.sensor-outside{--sensor-bg:#e5f6fc;--sensor-border:#88cddf;--sensor-ink:#155269}
.sensor-fan{--sensor-bg:#f1ebfc;--sensor-border:#bfa5e5;--sensor-ink:#61408b}
:root[data-theme="dark"] .sensor-room{--sensor-bg:#3e2d23;--sensor-border:#a37851;--sensor-ink:#ffd1a5}
:root[data-theme="dark"] .sensor-outside{--sensor-bg:#1c3848;--sensor-border:#497b94;--sensor-ink:#a5e6fa}
:root[data-theme="dark"] .sensor-fan{--sensor-bg:#332b48;--sensor-border:#7b66a0;--sensor-ink:#dcc6ff}

a.secondary{display:inline-flex;align-items:center;justify-content:center;text-decoration:none;
  line-height:1.3;min-height:42px;max-width:100%;white-space:normal}
#panel-learning{line-height:1.55}
#panel-learning h2{margin-bottom:22px}
#panel-learning h3{font-size:18px;line-height:1.35;margin:26px 0 12px}
#panel-learning h2+h3{margin-top:0}
#panel-learning p{overflow-wrap:anywhere}
#panel-learning .archive-actions{margin:16px 0 12px}
#panel-learning .archive-note{display:block;line-height:1.55}
#panel-learning .wifi{margin-top:18px}
#panel-learning .wifi label{margin-top:16px}
#panel-learning .wifi small{display:block;margin-top:8px}
#panel-learning [role=status]:not(:empty){margin-top:10px}
#panel-learning .wifi input{min-width:0;max-width:100%}
.wifi input[type=file]{overflow:hidden;font-size:14px;padding:10px}
.wifi input[type=file]::file-selector-button{border:1px solid var(--line);border-radius:6px;
  background:transparent;color:inherit;padding:6px 8px;margin-right:8px;font:inherit;cursor:pointer}

#temperatureChart{display:block;width:100%;height:290px;margin:14px 0 6px}
.chart-periods button[aria-pressed=true]{background:var(--blue);color:white}
.chart-legend{display:flex;flex-wrap:wrap;gap:8px 14px;margin:14px 0;font-size:12px}
#panel-learning .chart-legend label{display:inline-flex;gap:5px;align-items:center;margin:0}
#panel-learning .chart-legend input{width:auto}
.chart-legend label:has([data-line=room]){color:#1976b8}
.chart-legend label:has([data-line=outside]){color:#008478}
.chart-legend label:has([data-line=target]){color:#c26916}
.chart-legend label:has([data-line=prediction]){color:#a16bdf}

html{scroll-padding-top:80px}
body.menu-open{overflow:hidden}
header{position:sticky;top:0;z-index:10;padding-top:env(safe-area-inset-top)}
.bar{gap:12px;padding:10px 16px;flex-wrap:wrap}
.brand{font-size:18px;flex:1;min-width:0}
.menu-toggle,.menu-close{min-width:44px;min-height:44px;border:1px solid var(--line);border-radius:12px;background:transparent;color:var(--ink);font-size:22px}
.menu-toggle{display:flex;gap:8px;align-items:center;justify-content:center;padding:8px 12px;font-size:16px}
.menu-toggle svg{width:22px;height:22px;stroke:currentColor;fill:none;stroke-width:2;flex:none}
.badge{max-width:100%;overflow-wrap:anywhere}
.menu-backdrop{position:fixed;inset:0;background:#08172780;z-index:30}
.section-menu{position:fixed;top:0;right:0;bottom:0;width:min(340px,90vw);z-index:31;
 background:#fff;box-shadow:-12px 0 40px #08172730;overflow-y:auto;overscroll-behavior:contain;
 padding:calc(16px + env(safe-area-inset-top)) 16px calc(20px + env(safe-area-inset-bottom))}
.menu-heading{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:20px}
.menu-heading strong{font-size:19px}
.section-menu .tabs{display:flex;flex-direction:column;gap:8px;padding:0;border:0;background:transparent;margin:0}
.section-menu .tabs button{min-height:50px;padding:12px 16px;text-align:left;font-size:16px;border-radius:12px}
:root[data-theme="dark"] .section-menu{background:#192b3e}
main{padding-bottom:calc(20px + env(safe-area-inset-bottom))}
button,input,a.secondary{touch-action:manipulation}
button,a.secondary,.choice span{min-height:44px}
.step{width:48px;height:48px}
.card{min-width:0}
input{max-width:100%;min-width:0}
.wifi input:not([type=radio]),.schedule-rule input{font-size:16px}
.wifi small{display:block;margin-top:8px;line-height:1.5}
[role=status],small,#diagnostic,.temp-readback{overflow-wrap:anywhere}
.chart-legend label{min-height:44px;cursor:pointer}
.chart-periods{display:grid;grid-template-columns:repeat(4,minmax(0,1fr))}
.chart-periods button{padding:9px 4px;font-size:14px}
.schedule-rule .days label{min-width:44px;min-height:44px;justify-content:center;border:1px solid var(--line);border-radius:9px;padding:4px}
@media(max-width:480px){
 .bar{padding:8px 12px;gap:8px}.bar .badge{flex-basis:100%;width:fit-content;max-width:100%;font-size:12px;padding:4px 8px}
 main{padding:12px 10px calc(16px + env(safe-area-inset-bottom))}
 .card{padding:16px 14px;border-radius:17px;margin-bottom:12px}
 h1{font-size:22px}.dialrow{gap:10px;margin:18px 0 14px}
 .dial{width:clamp(132px,44vw,194px)}
 .mode-choices{display:grid;grid-template-columns:repeat(2,minmax(0,1fr))}
 .mode-choices .choice:last-child{grid-column:1/-1}
 .fan-choices .choice{flex:1 0 44px}.fan-choices .choice:first-child,.fan-choices .choice:last-child{flex-basis:100%}
 .choice span{padding:9px 10px}.sensor-circles{gap:8px;max-width:260px}
 #panel-learning h3{font-size:17px}.chart-legend{gap:4px 12px}
 .schedule-rule{padding:10px}.schedule-rule .grid{grid-template-columns:1fr}
 #pinsForm .choices{display:grid;grid-template-columns:repeat(2,minmax(0,1fr))}
}
@media(prefers-reduced-motion:no-preference){.section-menu:not([hidden]){animation:menu-reveal .18s ease-out}@keyframes menu-reveal{from{transform:translateX(100%)}to{transform:translateX(0)}}}
</style></head><body>
<header><div class="bar"><span class="brand">Daikin Wemos</span>
<button id="menuToggle" class="menu-toggle" type="button" aria-expanded="false" aria-controls="sectionMenu" aria-label="Apri menu sezioni"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3 6h18M3 12h18M3 18h18"/></svg><span id="currentSection">Home</span></button>
<span id="connection" class="badge">Connessione…</span></div></header>
<div id="menuBackdrop" class="menu-backdrop" hidden></div>
<nav id="sectionMenu" class="section-menu" aria-label="Sezioni del Wemos" hidden>
<div class="menu-heading"><strong>Menu</strong><button id="menuClose" class="menu-close" type="button" aria-label="Chiudi menu">×</button></div>
<div class="tabs" role="tablist" aria-label="Sezioni del Wemos" aria-orientation="vertical">
<button id="tab-home" type="button" role="tab" aria-controls="panel-home" aria-selected="true" tabindex="0">Home</button>
<button id="tab-wifi" type="button" role="tab" aria-controls="panel-wifi" aria-selected="false" tabindex="-1">Wi-Fi</button>
<button id="tab-schedule" type="button" role="tab" aria-controls="panel-schedule" aria-selected="false" tabindex="-1">Orari</button>
<button id="tab-firmware" type="button" role="tab" aria-controls="panel-firmware" aria-selected="false" tabindex="-1">Firmware</button>
<button id="tab-debug" type="button" role="tab" aria-controls="panel-debug" aria-selected="false" tabindex="-1">Debug</button>
<button id="tab-learning" type="button" role="tab" aria-controls="panel-learning" aria-selected="false" tabindex="-1">Apprendimento</button>
<button id="tab-preferences" type="button" role="tab" aria-controls="panel-preferences" aria-selected="false" tabindex="-1">Preferenze</button>
</div></nav>
<main id="mainContent">
<div id="panel-home" role="tabpanel" aria-labelledby="tab-home" tabindex="0">
<section class="card climate"><h1>Climatizzatore</h1><small>FTXM25M2V1B · Controllo locale</small>
<small id="diagnostic"></small>
<form id="controls"><fieldset id="basic" disabled>
<div class="dialrow">
<button type="button" class="step" id="minus" aria-label="Diminuisci temperatura">−</button>
<div class="dial"><label for="temp">Temperatura richiesta</label><div class="dialvalue"><input id="temp" name="temp" type="number" min="16" max="30" step="0.5" value="24" required><span class="degree">°C</span></div></div>
<button type="button" class="step" id="plus" aria-label="Aumenta temperatura">+</button>
</div>
<p id="tempReadback" class="temp-readback"></p>
<div class="sensor-circles" aria-label="Sensori del climatizzatore">
<div class="sensor-circle sensor-room"><span class="sensor-label">Ambiente</span><strong id="roomValue">—</strong><span class="sensor-unit">°C</span></div>
<div class="sensor-circle sensor-outside"><span class="sensor-label">Esterno</span><strong id="outsideValue">—</strong><span class="sensor-unit">°C</span></div>
<div class="sensor-circle sensor-fan"><span class="sensor-label">Ventola</span><strong id="fanRpmValue">—</strong><span class="sensor-unit">giri/min</span></div>
</div>
<div class="control-groups">
<fieldset id="powerChoices" class="choice-group"><legend>Alimentazione</legend>
<div class="choices pair">
<label class="choice"><input type="radio" name="power" value="0" required><span>Spento</span></label>
<label class="choice"><input type="radio" name="power" value="1" required><span>Acceso</span></label>
</div></fieldset>
<fieldset id="modeChoices" class="choice-group"><legend>Modalità</legend>
<div class="choices mode-choices">
<label class="choice"><input type="radio" name="mode" value="1" required><span>Automatico</span></label>
<label class="choice"><input type="radio" name="mode" value="2" required><span>Deumidificazione</span></label>
<label class="choice"><input type="radio" name="mode" value="3" required><span>Raffrescamento</span></label>
<label class="choice"><input type="radio" name="mode" value="4" required><span>Riscaldamento</span></label>
<label class="choice"><input type="radio" name="mode" value="6" required><span>Ventilazione</span></label>
</div></fieldset>
<fieldset id="fanChoices" class="choice-group"><legend>Ventola</legend>
<div class="choices fan-choices">
<label class="choice"><input type="radio" name="fan" value="A" required><span>Automatica</span></label>
<label class="choice"><input type="radio" name="fan" value="3" required><span>1</span></label>
<label class="choice"><input type="radio" name="fan" value="4" required><span>2</span></label>
<label class="choice"><input type="radio" name="fan" value="5" required><span>3</span></label>
<label class="choice"><input type="radio" name="fan" value="6" required><span>4</span></label>
<label class="choice"><input type="radio" name="fan" value="7" required><span>5</span></label>
<label class="choice"><input type="radio" name="fan" value="B" required><span>Silenziosa</span></label>
</div></fieldset>
</div></fieldset></form>
<p id="autoApplyStatus" class="muted" role="status"></p>
<p id="message" role="status"></p></section>
<section class="card"><h2>Funzioni speciali</h2>
<div class="grid">
<button class="tile" id="turbo" type="button" aria-pressed="false" disabled><strong>Turbo</strong><span id="turboState">Lettura…</span></button>
<button class="tile" id="eco" type="button" aria-pressed="false" disabled><strong>Eco</strong><span id="ecoState">Lettura…</span></button>
</div><p id="specialMessage" role="status"></p></section>
<section class="card"><h2>Timer di spegnimento</h2>
<form id="sleepForm" class="wifi">
<label for="sleepMinutes">Spegni tra quanti minuti?</label>
<input id="sleepMinutes" name="minutes" type="number" min="1" max="1440" step="1" value="30" required>
<div class="choices" style="margin-top:10px">
<button class="secondary" type="button" data-sleep="15">15 min</button>
<button class="secondary" type="button" data-sleep="30">30 min</button>
<button class="secondary" type="button" data-sleep="60">1 ora</button>
</div>
<button class="primary" id="startSleep" disabled>Avvia timer</button>
</form><p id="sleepCountdown" role="status" class="muted"></p>
<button class="secondary" id="cancelSleep" type="button" disabled>Annulla timer</button>
<p id="sleepMessage" role="status"></p>
</section>
<section class="card"><h2>Direzione dell’aria</h2><form id="swing"><fieldset id="swingFields" disabled>
<label class="check"><input id="vertical" name="vertical" type="checkbox" value="1">Oscillazione verticale</label>
<label class="check"><input id="horizontal" name="horizontal" type="checkbox" value="1">Oscillazione orizzontale</label>
<button class="secondary">Applica oscillazione</button></fieldset></form><p id="swingMessage" role="status"></p></section>
</div>
<section class="card" id="panel-wifi" role="tabpanel" aria-labelledby="tab-wifi" tabindex="0" hidden><h2>Configurazione Wi-Fi</h2><p id="network"></p>
<form id="wifi" class="wifi"><label for="ssid">Nome rete Wi-Fi (2,4 GHz)</label><input id="ssid" name="ssid" maxlength="32" required autocomplete="off">
<label for="password">Password Wi-Fi</label><input id="password" name="password" type="password" maxlength="63" autocomplete="new-password" placeholder="Lascia vuoto per mantenere la password salvata">
<label class="check"><input id="openNetwork" name="openNetwork" type="checkbox" value="1">Rete senza password</label>
<fieldset id="addressModeChoices" class="choice-group"><legend>Assegnazione indirizzo</legend>
<div class="choices pair">
<label class="choice"><input type="radio" name="addressMode" value="dhcp" required><span>DHCP</span></label>
<label class="choice"><input type="radio" name="addressMode" value="static" required><span>IP statico</span></label>
</div></fieldset>
<div id="staticFields" hidden>
<label for="staticIp">Indirizzo IPv4</label><input id="staticIp" name="staticIp" inputmode="decimal" placeholder="192.168.1.50">
<label for="mask">Maschera di sottorete</label><input id="mask" name="mask" inputmode="decimal" placeholder="255.255.255.0">
<label for="gateway">Gateway</label><input id="gateway" name="gateway" inputmode="decimal" placeholder="192.168.0.1">
<label for="dns">DNS (facoltativo)</label><input id="dns" name="dns" inputmode="decimal" placeholder="Predefinito: gateway">
</div>
<button class="primary">Salva e riavvia</button></form><p id="wifiMessage" role="status"></p></section>
<section class="card" id="panel-schedule" role="tabpanel" aria-labelledby="tab-schedule" tabindex="0" hidden><h2>Programmazione settimanale</h2>
<p id="scheduleClock" class="muted">Lettura orologio…</p>
<p id="scheduleResult" role="status"></p>
<form id="scheduleForm" class="wifi">
<div id="scheduleRules"></div>
<p id="scheduleEmpty" class="muted">Nessun orario impostato.</p>
<button id="addSchedule" class="secondary" type="button" disabled>Aggiungi orario</button>
<button id="saveSchedule" class="primary" type="submit" disabled>Salva programmazione</button>
</form><p id="scheduleMessage" role="status"></p>
</section>
<section class="card" id="panel-firmware" role="tabpanel" aria-labelledby="tab-firmware" tabindex="0" hidden><h2>Aggiornamento firmware · v2.11</h2>
<form id="firmwareForm" class="wifi">
<p class="muted">Seleziona il file .bin compilato per il Wemos D1 mini.</p>
<label for="firmwareFile">File firmware</label>
<input id="firmwareFile" type="file" accept=".bin" required>
<label for="firmwareKey">Chiave di aggiornamento</label>
<input id="firmwareKey" type="password" maxlength="32" required autocomplete="off">
<small>La chiave è visibile nel Monitor seriale a 115200 baud dopo il riavvio.</small>
<button class="primary">Carica firmware e riavvia</button>
</form><p id="firmwareMessage" role="status"></p></section>
<section class="card" id="panel-debug" role="tabpanel" aria-labelledby="tab-debug" tabindex="0" hidden><h2>Debug remoto</h2>
<p id="debugState" class="muted" role="status">Lettura dello stato…</p>
<form id="debugForm" class="wifi">
<label for="debugKey">Chiave di aggiornamento</label>
<input id="debugKey" type="password" maxlength="32" autocomplete="off" required>
<div class="grid">
<button class="primary" type="submit" value="1">Attiva debug</button>
<button class="primary" type="submit" value="0">Disattiva</button>
</div></form><p id="debugMessage" role="status"></p>
</section>
<section class="card" id="panel-preferences" role="tabpanel" aria-labelledby="tab-preferences" tabindex="0" hidden><h2>Preferenze</h2>
<fieldset class="choice-group"><legend>Tema</legend><div class="choices">
<label class="choice"><input type="radio" name="theme" value="auto" checked><span>Automatico</span></label>
<label class="choice"><input type="radio" name="theme" value="light"><span>Chiaro</span></label>
<label class="choice"><input type="radio" name="theme" value="dark"><span>Scuro</span></label>
</div></fieldset>
<form class="wifi" id="autoApplyForm">
<label for="autoApplySeconds">Applica la temperatura dopo (secondi)</label>
<input id="autoApplySeconds" type="number" min="0" max="60" step="1" value="3" required>
<small>Il conteggio riparte a ogni modifica. 0 applica subito. Preferenza salvata in questo browser.</small>
<button class="primary">Salva ritardo</button>
</form><p id="autoApplyMessage" role="status"></p>
<form class="wifi" id="heatOffsetForm">
<label for="heatOffset">Compensazione riscaldamento (°C)</label>
<input type="number" id="heatOffset" name="offset" min="-5" max="0" step="0.5" value="0" required>
<small>0 disattiva la compensazione. Con −2,5 °C, una richiesta di 20 °C invia 17,5 °C. La correzione si usa alla prossima modifica della temperatura.</small>
<button class="primary">Salva compensazione</button>
</form><p id="heatOffsetMessage" role="status"></p>
<h3>Collegamento Daikin · TX / RX</h3>
<form class="wifi" id="pinsForm">
<fieldset class="choice-group"><legend>TX del Wemos → RX del Daikin</legend><div class="choices"><label class="choice"><input type="radio" name="tx" value="5"><span>D1 · GPIO5</span></label><label class="choice"><input type="radio" name="tx" value="4"><span>D2 · GPIO4</span></label><label class="choice"><input type="radio" name="tx" value="14"><span>D5 · GPIO14</span></label><label class="choice"><input type="radio" name="tx" value="12"><span>D6 · GPIO12</span></label><label class="choice"><input type="radio" name="tx" value="13" checked><span>D7 · GPIO13</span></label></div></fieldset>
<fieldset class="choice-group"><legend>RX del Wemos ← TX del Daikin</legend><div class="choices"><label class="choice"><input type="radio" name="rx" value="5"><span>D1 · GPIO5</span></label><label class="choice"><input type="radio" name="rx" value="4"><span>D2 · GPIO4</span></label><label class="choice"><input type="radio" name="rx" value="14"><span>D5 · GPIO14</span></label><label class="choice"><input type="radio" name="rx" value="12" checked><span>D6 · GPIO12</span></label><label class="choice"><input type="radio" name="rx" value="13"><span>D7 · GPIO13</span></label></div></fieldset>
<small>Scegli due pin diversi. Il cambio è immediato e rimane salvato anche dopo il riavvio. Per modificare i fili togli alimentazione; mantieni il convertitore di livello. Predefiniti: TX D7, RX D6.</small>
<button class="primary">Salva pin TX / RX</button>
</form><p id="pinsMessage" role="status"></p></section>
<section class="card" id="panel-learning" role="tabpanel" aria-labelledby="tab-learning" tabindex="0" hidden>
<h2>Apprendimento · Osservazione</h2>
<h3>Andamento della temperatura</h3>
<div class="choices chart-periods" role="group" aria-label="Periodo del grafico">
<button type="button" class="secondary" data-hours="1" aria-pressed="false">1 ora</button>
<button type="button" class="secondary" data-hours="6" aria-pressed="true">6 ore</button>
<button type="button" class="secondary" data-hours="24" aria-pressed="false">24 ore</button>
<button type="button" class="secondary" data-hours="0" aria-pressed="false">Tutto</button>
</div>
<div class="chart-legend">
<label><input type="checkbox" data-line="room" checked>Ambiente</label>
<label><input type="checkbox" data-line="outside">Esterno</label>
<label><input type="checkbox" data-line="target" checked>Impostazione Daikin</label>
<label><input type="checkbox" data-line="prediction" checked>Previsione rete a 10 min</label>
</div>
<canvas id="temperatureChart" role="img" aria-label="Grafico delle temperature registrate"></canvas>
<p id="chartStatus" class="muted" role="status">Apri questa scheda per leggere lo storico.</p>
<button type="button" id="refreshChart" class="secondary">Aggiorna grafico</button>
<h3>Raccolta sul Wemos</h3>
<p id="archiveStatus" role="status">Lettura archivio…</p>
<p id="archiveInfo" class="muted"></p>
<p class="archive-actions"><a class="secondary" id="downloadArchive" href="/api/learning/download">Scarica dati CSV</a></p>
<small class="archive-note">Archivio circolare fino a 512 KiB. I dati più vecchi vengono sostituiti. Gli ultimi cinque minuti possono andare persi se manca alimentazione.</small>
<form id="archiveForm" class="wifi" hidden>
<label for="archiveKey">Chiave di aggiornamento</label>
<input id="archiveKey" type="password" maxlength="32" required autocomplete="off">
<label class="check"><input name="confirm" type="checkbox" value="1" required>Autorizzo la preparazione della memoria dei file</label>
<button class="primary">Inizializza archivio</button>
<small>La preparazione cancella eventuali file già presenti nella partizione. Le impostazioni del Wemos sono conservate.</small>
</form><p id="archiveMessage" role="status"></p>
<h3>Modello predittivo</h3>

<p id="learningStatus" role="status">Lettura…</p>
<p id="learningPrediction" class="muted"></p>
<p class="muted">Previsione a 10 minuti con comandi invariati. Il modello lavora in osservazione.</p>
<form id="modelForm" class="wifi">
<label for="modelFile">Modello addestrato sul PC</label><input id="modelFile" type="file" accept=".txt" required>
<label for="modelKey">Chiave di aggiornamento</label><input id="modelKey" type="password" maxlength="32" required autocomplete="off">
<button class="primary">Carica modello</button>
</form><p id="modelMessage" role="status"></p></section>
</main>
<script>
const $=id=>document.getElementById(id);let editing=false,busy=false,fanEdited=false;
const dayNames = ['Dom','Lun','Mar','Mer','Gio','Ven','Sab'];
let scheduleLoaded = false;
let scheduleLoading = false;
let scheduleRowId = 0;
const tabButtons = [...document.querySelectorAll('.tabs [role="tab"]')];
function setMenuOpen(open, restoreFocus = true) {
  $('sectionMenu').hidden = !open;
  $('menuBackdrop').hidden = !open;
  $('menuToggle').setAttribute('aria-expanded', String(open));
  document.body.classList.toggle('menu-open', open);
  $('mainContent').inert = open;
  if (open) $('menuClose').focus();
  else if (restoreFocus) $('menuToggle').focus();
}
$('menuToggle').onclick = () => setMenuOpen($('sectionMenu').hidden);
$('menuClose').onclick = () => setMenuOpen(false);
$('menuBackdrop').onclick = () => setMenuOpen(false);
document.addEventListener('keydown', event => {
  if ($('sectionMenu').hidden) return;
  if (event.key === 'Escape') {event.preventDefault();setMenuOpen(false);return;}
  if (event.key === 'Tab') {
    const focusable = [...$('sectionMenu').querySelectorAll('button')].filter(button=>button.tabIndex>=0);
    const first=focusable[0], last=focusable.at(-1);
    if(event.shiftKey && document.activeElement===first){event.preventDefault();last.focus();}
    else if(!event.shiftKey && document.activeElement===last){event.preventDefault();first.focus();}
  }
});
function activateTab(id, focus = false) {
  const selected = tabButtons.find(button => button.id === id) || tabButtons[0];
  for (const button of tabButtons) {
    const active = button === selected;
    button.setAttribute('aria-selected', String(active));
    button.tabIndex = active ? 0 : -1;
    $(button.getAttribute('aria-controls')).hidden = !active;
  }
  $('currentSection').textContent = selected.textContent;
  if (focus) selected.focus();
  if (selected.id === 'tab-schedule') loadSchedulePanel();
  if (selected.id === 'tab-learning') {
    if (!chartRows.length) loadTemperatureChart();
    else drawTemperatureChart();
  }
}
function navigateTab(button, focus = false) {
  history.replaceState(null, '', '#' + button.id.slice(4));
  activateTab(button.id, focus);
  if (!focus) {
    setMenuOpen(false, false);
    $(button.getAttribute('aria-controls')).focus({preventScroll:true});
    window.scrollTo({top:0,behavior:'instant'});
  }
}
for (const [index, button] of tabButtons.entries()) {
  button.onclick = () => navigateTab(button);
  button.onkeydown = event => {
    let next = index;
    if ((event.key === 'ArrowRight' || event.key === 'ArrowDown')) next = (index + 1) % tabButtons.length;
    else if ((event.key === 'ArrowLeft' || event.key === 'ArrowUp')) next = (index + tabButtons.length - 1) % tabButtons.length;
    else if (event.key === 'Home') next = 0;
    else if (event.key === 'End') next = tabButtons.length - 1;
    else return;
    event.preventDefault();
    navigateTab(tabButtons[next], true);
  };
}
window.addEventListener('hashchange', () => activateTab('tab-' + location.hash.slice(1)));


function scheduleButtons() {
  const count = $('scheduleRules').children.length;
  $('addSchedule').disabled = !scheduleLoaded || count >= 8;
  $('saveSchedule').disabled = !scheduleLoaded;
  $('scheduleEmpty').hidden = count > 0;
}

function addScheduleRule(rule = {enabled:true, days:62, minute:420, mode:'4', temp:20}) {
  if ($('scheduleRules').children.length >= 8) return;
  const row = document.createElement('fieldset');
  row.className = 'schedule-rule';
  row.innerHTML = `
    <label class="check"><input type="checkbox" class="rule-enabled">Orario attivo</label>
    <div class="grid">
      <label>Ora<input type="time" class="rule-time" required></label>
      <label class="rule-temperature">Temperatura °C<input type="number" class="rule-temp" min="16" max="30" step="0.5" required></label>
    </div>
    <fieldset class="choice-group"><legend>Azione</legend><div class="choices rule-modes"></div></fieldset>
    <div class="days"></div>
    <button class="secondary remove-rule" type="button">Rimuovi</button>`;
  row.querySelector('.rule-enabled').checked = rule.enabled;
  row.querySelector('.rule-time').value = String(Math.floor(rule.minute / 60)).padStart(2,'0') + ':' +
                                         String(rule.minute % 60).padStart(2,'0');
  row.querySelector('.rule-temp').value = rule.temp;
  const modeGroup = 'schedule-mode-' + scheduleRowId++;
  function updateRuleTemperature() {
    const off = row.querySelector('.rule-modes input:checked')?.value === '0';
    row.querySelector('.rule-temperature').hidden = off;
    row.querySelector('.rule-temp').disabled = off;
  }
  for (const [mode, caption] of [['4','Riscaldamento'],['3','Raffrescamento'],['1','Automatico'],['0','Spegni']]) {
    const label = document.createElement('label');
    label.className = 'choice';
    const input = document.createElement('input');
    input.type = 'radio';
    input.name = modeGroup;
    input.value = mode;
    input.checked = rule.mode === mode;
    input.addEventListener('change', updateRuleTemperature);
    const text = document.createElement('span');
    text.textContent = caption;
    label.append(input, text);
    row.querySelector('.rule-modes').append(label);
  }
  updateRuleTemperature();
  for (const day of [1,2,3,4,5,6,0]) {
    const label = document.createElement('label');
    const input = document.createElement('input');
    input.type = 'checkbox';
    input.dataset.day = day;
    input.checked = (rule.days & (1 << day)) !== 0;
    label.append(input, dayNames[day]);
    row.querySelector('.days').append(label);
  }
  row.querySelector('.remove-rule').onclick = () => { row.remove(); scheduleButtons(); };
  $('scheduleRules').append(row);
  scheduleButtons();
}

$('addSchedule').onclick = () => addScheduleRule();
async function loadSchedulePanel() {
  if (scheduleLoaded || scheduleLoading) return;
  scheduleLoading = true;
  try {
    const response = await fetch('/api/schedule', {cache:'no-store'});
    if (!response.ok) throw Error('HTTP ' + response.status);
    const rules = await response.json();
    for (const rule of rules) addScheduleRule(rule);
    scheduleLoaded = true;
    scheduleButtons();
  } catch (error) {
    $('scheduleMessage').textContent = 'Lettura non riuscita. Cambia scheda e torna a Orari per riprovare.';
  } finally {
    scheduleLoading = false;
  }
}

$('scheduleForm').onsubmit = event => {
  event.preventDefault();
  if (!scheduleLoaded || busy) return;
  const rows = [...$('scheduleRules').children];
  const data = {count: rows.length};
  for (const [index,row] of rows.entries()) {
    let days = 0;
    for (const input of row.querySelectorAll('[data-day]:checked')) days |= 1 << Number(input.dataset.day);
    const mode = row.querySelector('.rule-modes input:checked');
    if (!days || !mode) {
      $('scheduleMessage').textContent = 'Seleziona modalità e almeno un giorno per ogni orario.';
      return;
    }
    const [hours,minutes] = row.querySelector('.rule-time').value.split(':').map(Number);
    data['days'+index] = days;
    data['minute'+index] = hours * 60 + minutes;
    data['temp'+index] = mode.value === '0' ? 0 : Number(row.querySelector('.rule-temp').value) * 2;
    data['mode'+index] = mode.value;
    data['enabled'+index] = row.querySelector('.rule-enabled').checked ? 1 : 0;
  }
  send('/api/schedule', data, 'scheduleMessage');
};
let controlsRevision = 0;
let lastLiveState = null;
let autoApplySeconds = 3;
let autoApplyHandle = null;
let autoApplyDue = 0;
try {
  const saved = localStorage.getItem('daikin-auto-apply');
  if (saved !== null && /^\d+$/.test(saved) && Number(saved) <= 60) autoApplySeconds = Number(saved);
} catch (error) {}
$('autoApplySeconds').value = autoApplySeconds;

function cancelAutoApply() {
  clearTimeout(autoApplyHandle);
  autoApplyHandle = null;
  autoApplyDue = 0;
  $('autoApplyStatus').textContent = '';
}
function canAutoApply() {
  return lastLiveState?.online && !$('basic').disabled && $('controls').checkValidity();
}
function queueAutoApply(seconds = autoApplySeconds) {
  cancelAutoApply();
  if (!canAutoApply()) return;
  autoApplyDue = performance.now() + seconds * 1000;
  autoApplyHandle = setTimeout(applyTemperatureAutomatically, seconds * 1000);
  $('autoApplyStatus').textContent = seconds ? 'Applicazione tra ' + seconds + ' s' : 'Applicazione in corso…';
}
function applyTemperatureAutomatically() {
  if (!canAutoApply()) { cancelAutoApply(); return; }
  if (busy) {
    $('autoApplyStatus').textContent = 'Applicazione in attesa…';
    autoApplyHandle = setTimeout(applyTemperatureAutomatically, 250);
    return;
  }
  const data = new FormData($('controls'));
  if (!fanEdited) data.delete('fan');
  data.set('compensate','1');
  cancelAutoApply();
  send('/api/set', data, 'message');
}
setInterval(() => {
  if (autoApplyDue > performance.now()) {
    $('autoApplyStatus').textContent = 'Applicazione tra ' + Math.ceil((autoApplyDue - performance.now()) / 1000) + ' s';
  }
}, 250);
$('autoApplyForm').onsubmit = event => {
  event.preventDefault();
  autoApplySeconds = Number($('autoApplySeconds').value);
  cancelAutoApply();
  try {
    localStorage.setItem('daikin-auto-apply', String(autoApplySeconds));
    $('autoApplyMessage').textContent = autoApplySeconds ? 'Ritardo salvato.' : 'Applicazione immediata attiva.';
  } catch (error) {
    $('autoApplyMessage').textContent = 'Ritardo applicato per questa pagina. Salvataggio nel browser non disponibile.';
  }
};

let sleepRemaining = 0;
let sleepObservedAt = 0;
let sleepActive = false;
let sleepStatus = '';
function renderSleepCountdown() {
  const elapsed = Math.floor((performance.now() - sleepObservedAt) / 1000);
  const seconds = Math.max(0, sleepRemaining - elapsed);
  if (!sleepActive) {
    $('sleepCountdown').textContent = sleepStatus || 'Nessun timer attivo';
    return;
  }
  $('sleepCountdown').textContent = seconds
    ? 'Spegnimento tra ' + Math.floor(seconds / 60) + ' min ' + String(seconds % 60).padStart(2, '0') + ' s'
    : 'Spegnimento in corso…';
}
setInterval(renderSleepCountdown, 1000);
$('sleepForm').onsubmit = event => {
  event.preventDefault();
  send('/api/sleep', new FormData(event.target), 'sleepMessage');
};
$('cancelSleep').onclick = () => send('/api/sleep', {minutes:0}, 'sleepMessage');
for (const button of document.querySelectorAll('[data-sleep]')) {
  button.onclick = () => { $('sleepMinutes').value = button.dataset.sleep; };
}
$('archiveForm').onsubmit = async event => {
  event.preventDefault();
  if (busy) return;
  busy = true;
  try {
    const response = await fetch('/api/learning/archive', {
      method:'POST', headers:{'X-Firmware-Key':$('archiveKey').value.trim()},
      body:new URLSearchParams(new FormData(event.target))
    });
    $('archiveMessage').textContent = await response.text();
    $('archiveKey').value = '';
  } catch(error) {
    $('archiveMessage').textContent = 'Inizializzazione non riuscita.';
  } finally {
    busy = false;
    refresh();
  }
};
$('modelForm').onsubmit = async event => {
  event.preventDefault();
  if (busy) return;
  const file = $('modelFile').files[0];
  if (!file || file.size > 14000) {
    $('modelMessage').textContent = 'Scegli il file modello-wemos.txt.';
    return;
  }
  busy = true;
  try {
    const response = await fetch('/api/learning/model', {
      method:'POST',
      headers:{'X-Firmware-Key':$('modelKey').value.trim()},
      body:new URLSearchParams({model:await file.text()})
    });
    $('modelMessage').textContent = await response.text();
    $('modelKey').value = '';
  } catch(error) {
    $('modelMessage').textContent = 'Caricamento non riuscito.';
  } finally {
    busy = false;
    refresh();
  }
};
let chartRows = [];
let chartLoading = false;
let chartHours = 6;
const chartColors = {room:'#1976b8',outside:'#008478',target:'#c26916',prediction:'#a16bdf'};

function parseTemperatureCsv(text) {
  const lines = text.trim().split(/\r?\n/);
  const header = lines.shift().split(',');
  const column = name => header.indexOf(name);
  if (column('timestamp') < 0 || column('room') < 0) throw Error('CSV non riconosciuto');
  const number = (cells,name) => {
    const raw = cells[column(name)];
    if (raw == null || raw === '') return null;
    const value = Number(raw);
    return Number.isFinite(value) ? value : null;
  };
  const rows = [];
  for (const line of lines) {
    const cells = line.split(',');
    const stamp = number(cells,'timestamp');
    if (stamp == null || stamp < 1704067200) continue;
    const online = number(cells,'online') === 1;
    rows.push({time:stamp*1000,
      room:online ? number(cells,'room') : null,
      outside:online ? number(cells,'outside') : null,
      target:online ? number(cells,'target') : null,
      prediction:online ? number(cells,'prediction10m') : null});
  }
  return rows.sort((a,b)=>a.time-b.time);
}

async function loadTemperatureChart() {
  if (chartLoading) return;
  chartLoading = true;
  $('refreshChart').disabled = true;
  $('chartStatus').textContent = 'Caricamento dello storico dal Wemos…';
  try {
    const response = await fetch('/api/learning/download',{cache:'no-store'});
    if (!response.ok) throw Error('Archivio non disponibile. Verifica la raccolta.');
    chartRows = parseTemperatureCsv(await response.text());
    drawTemperatureChart();
  } catch(error) {
    $('chartStatus').textContent = error.message || 'Lettura non riuscita.';
  } finally {
    chartLoading = false;
    $('refreshChart').disabled = false;
  }
}

function drawTemperatureChart() {
  if ($('panel-learning').hidden) return;
  const canvas = $('temperatureChart');
  const width = canvas.getBoundingClientRect().width;
  if (!width) return;
  const height = 290;
  const ratio = window.devicePixelRatio || 1;
  canvas.width = Math.round(width*ratio);
  canvas.height = Math.round(height*ratio);
  const context = canvas.getContext('2d');
  context.scale(ratio,ratio);
  const dark = document.documentElement.dataset.theme === 'dark';
  const muted = dark ? '#b8cadd' : '#52647c';
  const grid = dark ? '#395069' : '#e0e7ef';
  const startX=42, endX=width-12, startY=20, endY=250;
  if (!chartRows.length) {
    $('chartStatus').textContent = 'Nessun campione disponibile.';
    return;
  }
  const latest = chartRows.at(-1).time;
  const cutoff = chartHours ? latest-chartHours*3600000 : chartRows[0].time;
  const selected = chartRows.filter(row=>row.time>=cutoff);
  const enabled = [...document.querySelectorAll('[data-line]:checked')].map(input=>input.dataset.line);
  const series = enabled.map(name=>({name,points:selected.map(row=>({
    time:row.time+(name==='prediction'?600000:0),value:row[name]}))}));
  const values=series.flatMap(line=>line.points.map(point=>point.value)).filter(value=>value!==null);
  if (!values.length) {
    $('chartStatus').textContent = 'Seleziona una serie con letture disponibili.';
    return;
  }
  const min=Math.floor((Math.min(...values)-0.5)*2)/2;
  const max=Math.ceil((Math.max(...values)+0.5)*2)/2;
  const from=selected[0].time;
  const to=Math.max(from+60000,...series.flatMap(line=>line.points.filter(point=>point.value!==null).map(point=>point.time)));
  const x=time=>startX+(time-from)/(to-from)*(endX-startX);
  const y=value=>endY-(value-min)/(max-min)*(endY-startY);
  context.font='11px system-ui';
  context.fillStyle=muted;
  context.textAlign='right';
  for(let i=0;i<=4;i++){
    const value=min+(max-min)*i/4;
    const position=y(value);
    context.strokeStyle=grid;context.lineWidth=1;context.setLineDash([]);
    context.beginPath();context.moveTo(startX,position);context.lineTo(endX,position);context.stroke();
    context.fillText(value.toFixed(1),startX-6,position+4);
  }
  const format=new Intl.DateTimeFormat('it-IT',{timeZone:'Europe/Rome',hour:'2-digit',minute:'2-digit'});
  context.textAlign='center';
  const ticks=width<360?3:4;
  for(let i=0;i<=ticks;i++){
    const stamp=from+(to-from)*i/ticks;
    context.fillText(format.format(new Date(stamp)),x(stamp),endY+20);
  }
  context.textAlign='left';context.fillText('°C',startX,startY-6);
  for(const line of series){
    context.beginPath();context.strokeStyle=chartColors[line.name];context.lineWidth=2;
    context.setLineDash(line.name==='target'?[5,4]:line.name==='prediction'?[2,3]:[]);
    let previous=null;
    for(const point of line.points){
      if(point.value===null){previous=null;continue;}
      if(previous===null || point.time-previous>90000)context.moveTo(x(point.time),y(point.value));
      else context.lineTo(x(point.time),y(point.value));
      previous=point.time;
    }
    context.stroke();
  }
  const dateFormat=new Intl.DateTimeFormat('it-IT',{timeZone:'Europe/Rome',day:'2-digit',month:'2-digit',hour:'2-digit',minute:'2-digit'});
  const predictions=selected.some(row=>row.prediction!==null);
  $('chartStatus').textContent = dateFormat.format(new Date(from))+' – '+dateFormat.format(new Date(latest))+
    ' · '+selected.length+' campioni · ora di Roma'+
    (predictions?' · Le previsioni sono posizionate 10 minuti dopo la lettura.':' · Previsioni disponibili dopo il caricamento di un modello.');
}

$('refreshChart').onclick=loadTemperatureChart;
for(const button of document.querySelectorAll('[data-hours]')){
  button.onclick=()=>{
    chartHours=Number(button.dataset.hours);
    for(const other of document.querySelectorAll('[data-hours]'))other.setAttribute('aria-pressed',String(other===button));
    drawTemperatureChart();
  };
}
for(const input of document.querySelectorAll('[data-line]'))input.onchange=drawTemperatureChart;
new ResizeObserver(()=>drawTemperatureChart()).observe($('temperatureChart'));

const themes = ['auto', 'light', 'dark'];
let theme = 'auto';
try {
  const savedTheme = localStorage.getItem('daikin-theme');
  if (themes.includes(savedTheme)) theme = savedTheme;
} catch (error) {}
document.documentElement.dataset.theme = theme;
const themeChoices = document.querySelectorAll('[name="theme"]');
for (const choice of themeChoices) {
  choice.checked = choice.value === theme;
  choice.onchange = () => {
    document.documentElement.dataset.theme = choice.value;
    drawTemperatureChart();
    try { localStorage.setItem('daikin-theme', choice.value); } catch (error) {}
  };
}
let pinsEdited = false;
$('pinsForm').onchange = () => { pinsEdited = true; };
$('pinsForm').onsubmit = event => {
  event.preventDefault();
  const data = new FormData(event.target);
  if (data.get('rx') === data.get('tx')) {
    $('pinsMessage').textContent = 'TX e RX devono usare due pin diversi.';
    return;
  }
  send('/api/pins', data, 'pinsMessage');
};
let offsetEdited = false;
$('heatOffset').oninput = () => { offsetEdited = true; };
function updateThemeColors() {
  const mode = $('controls').elements.mode.value;
  const temperature = Number($('temp').value);
  const modeHue = ({'1': 245, '2': 174, '3': 209, '4': 24, '6': 155})[mode] ?? 207;
  const range = Math.max(0, Math.min(1, (temperature - 16) / 14));
  const dialHue = mode === '4' ? 39 - range * 29 : mode === '3' ? 218 - range * 30 : modeHue;
  document.documentElement.style.setProperty('--mode-hue', modeHue);
  document.documentElement.style.setProperty('--temp-hue', dialHue);
}
$('controls').addEventListener('input', updateThemeColors);
let wifiInitialized = false;
function updateAddressFields() {
  const staticMode = $('wifi').elements.addressMode.value === 'static';
  $('staticFields').hidden = !staticMode;
  for (const field of ['staticIp', 'mask', 'gateway', 'dns']) {
    $(field).disabled = !staticMode;
    $(field).required = staticMode && field !== 'dns';
  }
}
$('addressModeChoices').onchange = updateAddressFields;
$('wifi').addEventListener('input', () => { wifiInitialized = true; });

function stepTemperature(direction) {
  const input = $('temp');
  if (direction > 0) input.stepUp();
  else input.stepDown();
  editing = true;
  ++controlsRevision;
  updateThemeColors();
  queueAutoApply();
}
$('minus').onclick = () => stepTemperature(-1);
$('plus').onclick = () => stepTemperature(1);
function renderSpecial(feature, value, available) {
  const button = $(feature);
  button.disabled = value == null || !available;
  button.setAttribute('aria-pressed', value === true ? 'true' : 'false');
  $(feature + 'State').textContent = value == null ? 'Stato non disponibile' : value ? 'Attivo' : 'Disattivo';
}
for (const feature of ['turbo', 'eco']) {
  $(feature).onclick = () => {
    const enabled = $(feature).getAttribute('aria-pressed') === 'true' ? '0' : '1';
    send('/api/special', {feature, enabled}, 'specialMessage');
  };
}
$('controls').addEventListener('input', event => {
  editing = true;
  ++controlsRevision;
  if (event.target.name === 'fan') fanEdited = true;
  queueAutoApply(event.target === $('temp') ? autoApplySeconds : 0);
});
$('swing').addEventListener('input',()=>editing=true);
async function refresh(){if(busy)return;try{const r=await fetch('/api/state',{cache:'no-store'});if(!r.ok)throw Error('Errore HTTP '+r.status);const s=await r.json();
lastLiveState = s;
$('archiveStatus').textContent = s.archiveStatus || 'Archivio non disponibile';
$('archiveInfo').textContent = s.archiveReady
  ? Math.round((s.archiveBytes || 0) / 1024) + ' KiB salvati · ' + (s.archiveBufferedRows || 0) + ' campioni in attesa di salvataggio'
  : 'Inizializza la memoria per raccogliere senza PC.';
$('archiveForm').hidden = s.archiveReady === true;
$('downloadArchive').hidden = s.archiveReady !== true;
$('learningStatus').textContent = s.learningStatus || 'Dati di apprendimento non disponibili';
$('learningPrediction').textContent = s.prediction10m == null
  ? 'In attesa di un modello valido e di cinque minuti di letture.'
  : 'Temperatura prevista tra 10 min: ' + s.prediction10m.toFixed(1) + ' °C';
$('scheduleClock').textContent = s.localTime
  ? 'Ora del Wemos · ' + s.localTime + ' (Roma)'
  : 'Orologio in attesa di sincronizzazione. Gli orari restano in pausa.';
$('scheduleResult').textContent = s.scheduleResult || '';
sleepActive = s.sleepActive === true;
sleepRemaining = s.sleepSeconds || 0;
sleepStatus = s.sleepResult || '';
sleepObservedAt = performance.now();
renderSleepCountdown();

$('startSleep').disabled = !s.online || !s.power;
$('cancelSleep').disabled = !sleepActive;
$('debugState').textContent = s.debugActive
  ? 'Attivo sulla rete locale · fino alla disattivazione manuale'
  : 'Disattivato';
$('connection').textContent=s.online?'Daikin collegato':'Nessuna risposta recente dal Daikin';$('connection').className=s.online?'badge good':'badge bad';
renderSpecial('turbo',s.turbo,s.online&&s.power);
renderSpecial('eco',s.eco,s.online&&s.power&&s.mode!=='6');
const t=x=>x==null?'—':x.toFixed(1)+' °C';
$('roomValue').textContent = s.room == null ? '—' : s.room.toFixed(1);
$('outsideValue').textContent = s.outside == null ? '—' : s.outside.toFixed(1);
$('fanRpmValue').textContent = s.fanRpm == null ? '—' : s.fanRpm;
$('diagnostic').textContent=s.error||'Comunicazione S21 attiva';$('basic').disabled=!s.online;$('swingFields').disabled=!s.online||!s.swingKnown;
$('network').textContent='Indirizzo: '+s.ip+' · '+(s.addressMode==='static'?'IP statico':'DHCP');
if (!wifiInitialized) {
  $('ssid').value = s.ssid || '';
  $('wifi').elements.addressMode.value = s.addressMode || 'dhcp';
  for (const field of ['staticIp', 'mask', 'gateway', 'dns']) $(field).value = s[field] || '';
  updateAddressFields();
  wifiInitialized = true;
  if (s.ap && !s.wifiConnected && !location.hash) activateTab('tab-wifi');
}
if (!editing && s.known) {
  const controls = $('controls').elements;
  controls.power.value = s.power ? '1' : '0';
  controls.mode.value = s.mode;
  controls.fan.value = s.fan;
  if (s.requestedTarget != null) $('temp').value = s.requestedTarget;
  else if (s.target != null) $('temp').value = s.target;
  $('vertical').checked = s.vertical;
  $('horizontal').checked = s.horizontal;
  updateThemeColors();
}
if (!offsetEdited) $('heatOffset').value = s.heatOffset ?? 0;
if (!pinsEdited) {
  $('pinsForm').elements.rx.value = String(s.rxPin ?? 12);
  $('pinsForm').elements.tx.value = String(s.txPin ?? 13);
}
$('tempReadback').textContent = s.online && s.target != null
  ? 'Impostazione Daikin: ' + t(s.target) + ' · Obiettivo interno: ' + t(s.internalTarget ?? null)
  : '';
}catch(e){for (const id of ['roomValue', 'outsideValue', 'fanRpmValue']) $(id).textContent = '—';
$('connection').textContent='Wemos non raggiungibile';$('connection').className='badge bad';renderSpecial('turbo',null,false);renderSpecial('eco',null,false);$('basic').disabled=true;$('swingFields').disabled=true;$('startSleep').disabled=true;}}
async function send(path,data,out){if(busy)return;const sentRevision=controlsRevision;busy=true;$(out).textContent='Invio in corso...';try{const r=await fetch(path,{method:'POST',body:new URLSearchParams(data)});const text=await r.text();$(out).textContent=text;if(path==='/api/set'&&(r.ok||r.status===409)&&sentRevision===controlsRevision){editing=false;fanEdited=false;}else if(path==='/api/swing'&&r.ok&&!fanEdited)editing=false;else if(path==='/api/heat-offset'&&r.ok)offsetEdited=false;else if(path==='/api/pins'&&r.ok)pinsEdited=false;}catch(e){$(out).textContent='Connessione interrotta: verifica lo stato prima di riprovare.';}finally{busy=false;refresh();}}
$('controls').onsubmit=e=>{e.preventDefault();queueAutoApply(0);};
$('swing').onsubmit=e=>{e.preventDefault();send('/api/swing',{vertical:$('vertical').checked?'1':'0',horizontal:$('horizontal').checked?'1':'0'},'swingMessage');};
$('heatOffsetForm').onsubmit = event => {
  event.preventDefault();
  send('/api/heat-offset', new FormData(event.target), 'heatOffsetMessage');
};
$('wifi').onsubmit=e=>{e.preventDefault();send('/wifi',new FormData(e.target),'wifiMessage');};
$('firmwareForm').onsubmit = async event => {
  event.preventDefault();
  if (busy) return;
  const file = $('firmwareFile').files[0];
  if (!file || !file.name.toLowerCase().endsWith('.bin')) {
    $('firmwareMessage').textContent = 'Scegli un file firmware .bin.';
    return;
  }
  busy = true;
  const button = event.target.querySelector('button');
  button.disabled = true;
  const data = new FormData();
  data.append('firmware', file);
  $('firmwareMessage').textContent = 'Caricamento in corso. Mantieni alimentato il Wemos.';
  try {
    const response = await fetch('/api/firmware', {
      method: 'POST',
      headers: {'X-Firmware-Key': $('firmwareKey').value.trim(), 'X-Firmware-Size': String(file.size)},
      body: data
    });
    $('firmwareMessage').textContent = await response.text();
    $('firmwareKey').value = '';
    if (response.ok) {
      setTimeout(() => location.reload(), 15000);
      return;
    }
  } catch (error) {
    $('firmwareMessage').textContent = 'Connessione interrotta. Ricarica la pagina per verificare la versione.';
  }
  busy = false;
  button.disabled = false;
};
$('debugForm').onsubmit = async event => {
  event.preventDefault();
  if (busy) return;
  busy = true;
  const key = $('debugKey');
  try {
    const response = await fetch('/api/debug/session', {
      method: 'POST',
      headers: {'X-Firmware-Key': key.value.trim()},
      body: new URLSearchParams({enabled: event.submitter?.value || '1'})
    });
    $('debugMessage').textContent = await response.text();
    if (response.ok) key.value = '';
  } catch (error) {
    $('debugMessage').textContent = 'Connessione interrotta.';
  } finally {
    busy = false;
    refresh();
  }
};
$('wifi').elements.addressMode.value = 'dhcp';
updateAddressFields();
activateTab('tab-' + location.hash.slice(1));
refresh();setInterval(refresh,4000);
</script></body></html>)HTML";

uint32_t configChecksum(const void *record, size_t size) {
  const uint8_t *bytes = static_cast<const uint8_t *>(record);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < size; ++i)
    hash = (hash ^ bytes[i]) * 16777619UL;
  return hash;
}

bool loadWifi() {
  EEPROM.get(0, wifiSettings);
  return wifiSettings.magic == WIFI_MAGIC &&
         wifiSettings.checksum == configChecksum(&wifiSettings, offsetof(WifiSettings, checksum)) &&
         wifiSettings.ssid[0] != '\0' && wifiSettings.ssid[32] == '\0' &&
         wifiSettings.password[64] == '\0';
}

bool saveWifi(const String &ssid, const String &password, const NetworkSettings &network) {
  WifiSettings fresh = {};
  fresh.magic = WIFI_MAGIC;
  ssid.toCharArray(fresh.ssid, sizeof(fresh.ssid));
  password.toCharArray(fresh.password, sizeof(fresh.password));
  fresh.checksum = configChecksum(&fresh, offsetof(WifiSettings, checksum));
  EEPROM.put(0, fresh);
  EEPROM.put(NETWORK_OFFSET, network);
  return EEPROM.commit();
}

void loadNetwork() {
  EEPROM.get(NETWORK_OFFSET, networkSettings);
  if (networkSettings.magic != STATIC_MAGIC ||
      networkSettings.checksum != configChecksum(&networkSettings, offsetof(NetworkSettings, checksum))) {
    networkSettings = {};
  }
}

uint32_t ipv4Number(const IPAddress &address) {
  return (uint32_t(address[0]) << 24) | (uint32_t(address[1]) << 16) |
         (uint32_t(address[2]) << 8) | address[3];
}

bool readNetworkForm(NetworkSettings &settings) {
  const String mode = server.arg("addressMode");
  if (mode.isEmpty() || mode == "dhcp") return true;
  if (mode != "static") return false;

  IPAddress ip, gateway, mask, dnsAddress;
  if (!ip.fromString(server.arg("staticIp").c_str()) ||
      !gateway.fromString(server.arg("gateway").c_str()) ||
      !mask.fromString(server.arg("mask").c_str())) return false;
  const String dnsText = server.arg("dns");
  if (dnsText.isEmpty()) dnsAddress = gateway;
  else if (!dnsAddress.fromString(dnsText.c_str())) return false;

  const uint32_t ipNumber = ipv4Number(ip);
  const uint32_t gatewayNumber = ipv4Number(gateway);
  const uint32_t subnet = ipv4Number(mask);
  const uint32_t hostMask = ~subnet;
  if (subnet == 0 || hostMask < 3 || (hostMask & (hostMask + 1)) != 0 ||
      ip[0] == 0 || ip[0] == 127 || ip[0] >= 224 ||
      dnsAddress[0] == 0 || dnsAddress[0] == 127 || dnsAddress[0] >= 224 ||
      (ipNumber & subnet) != (gatewayNumber & subnet) || ipNumber == gatewayNumber ||
      (ipNumber & hostMask) == 0 || (ipNumber & hostMask) == hostMask ||
      (gatewayNumber & hostMask) == 0 || (gatewayNumber & hostMask) == hostMask) return false;

  settings = {STATIC_MAGIC, uint32_t(ip), uint32_t(gateway), uint32_t(mask), uint32_t(dnsAddress), 0};
  settings.checksum = configChecksum(&settings, offsetof(NetworkSettings, checksum));
  return true;
}

void startAccessPoint() {
  if (apActive) return;
  WiFi.mode(WIFI_AP_STA);
  if (networkSettings.magic == STATIC_MAGIC) {
    const uint32_t subnet = ipv4Number(IPAddress(networkSettings.mask));
    if ((ipv4Number(IPAddress(192, 168, 4, 1)) & subnet) ==
        (ipv4Number(IPAddress(networkSettings.ip)) & subnet)) {
      const IPAddress recovery(172, 31, 255, 1);
      WiFi.softAPConfig(recovery, recovery, IPAddress(255, 255, 255, 0));
    }
  }
  if (!WiFi.softAP(apName.c_str(), AP_PASSWORD)) return;
  dns.start(53, "*", WiFi.softAPIP());
  apActive = true;
  Serial.printf("Portale: %s / password: %s / http://%s\n",
                apName.c_str(), AP_PASSWORD, WiFi.softAPIP().toString().c_str());
}

void maintainWifi() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    Serial.print("Wi-Fi collegato. Pagina web: http://");
    Serial.println(WiFi.localIP());
    if (!mdnsActive) {
      mdnsActive = MDNS.begin("daikin-wemos");
      if (mdnsActive) MDNS.addService("http", "tcp", 80);
    }
  }
  const bool recoveryWindow = networkSettings.magic == STATIC_MAGIC && millis() < 120000UL;
  if (connected && apActive && !recoveryWindow) {
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    apActive = false;
  }
  if (!connected && wasConnected) wifiLostAt = millis();
  if (!connected && (uint32_t)(millis() - wifiLostAt) > 25000UL) startAccessPoint();
  wasConnected = connected;
  if (apActive) dns.processNextRequest();
  if (mdnsActive) MDNS.update();
}

bool validMode(char value) { return value == '1' || value == '2' || value == '3' || value == '4' || value == '6'; }
bool validFan(char value) { return value == 'A' || value == 'B' || (value >= '3' && value <= '7'); }

bool performS21(char c0, char c1, const uint8_t *payload, size_t size,
                bool expectReply, uint8_t *reply, size_t &replySize, S21Trace *trace) {
  while (s21.available()) s21.read();
  uint8_t checksum = (uint8_t)c0 + (uint8_t)c1;
  s21.write(STX); s21.write((uint8_t)c0); s21.write((uint8_t)c1);
  for (size_t i = 0; i < size; ++i) { checksum += payload[i]; s21.write(payload[i]); }
  s21.write(checksum); s21.write(ETX);
  if (trace) {
    trace->tx[trace->txSize++] = STX;
    trace->tx[trace->txSize++] = c0;
    trace->tx[trace->txSize++] = c1;
    for (size_t i = 0; i < size && trace->txSize < sizeof(trace->tx) - 2; ++i)
      trace->tx[trace->txSize++] = payload[i];
    trace->truncated = size > sizeof(trace->tx) - 5;
    trace->tx[trace->txSize++] = checksum;
    trace->tx[trace->txSize++] = ETX;
  }
  bool started = false;
  uint8_t buffer[64]; size_t length = 0;
  const uint32_t start = millis();
  while ((uint32_t)(millis() - start) < 650UL) {
    if (!s21.available()) { delay(1); continue; }
    const uint8_t value = s21.read();
    if (trace) {
      if (trace->rxSize < sizeof(trace->rx)) trace->rx[trace->rxSize++] = value;
      else trace->truncated = true;
    }
    if (!started) {
      if (value == NAK) { lastError = String(c0) + c1 + ": comando non supportato (NAK)"; return false; }
      if (value == ACK && !expectReply) return true;
      if (value == STX) { started = true; length = 0; }
      continue;
    }
    if (value == ETX && length >= 3) {
      uint8_t sum = 0;
      for (size_t i = 0; i + 1 < length; ++i) sum += buffer[i];
      if (sum == buffer[length - 1]) {
        s21.write(ACK);
        if (buffer[0] != (uint8_t)(c0 + 1) || buffer[1] != (uint8_t)c1) {
          lastError = "Risposta S21 inattesa"; return false;
        }
        const size_t dataLength = length - 3;
        if (dataLength > replySize || (dataLength && !reply)) {
          lastError = "Risposta S21 troppo lunga"; return false;
        }
        if (dataLength) memcpy(reply, buffer + 2, dataLength);
        replySize = dataLength;
        return true;
      }
      // ETX puo' anche essere il byte checksum: attendere il terminatore successivo.
    }
    if (length >= sizeof(buffer)) { lastError = "Frame S21 troppo lungo"; return false; }
    buffer[length++] = value;
  }
  lastError = String(c0) + c1 + ": timeout / nessuna risposta valida";
  return false;
}

bool exchangeS21(char c0, char c1, const uint8_t *payload, size_t size,
                 bool expectReply, uint8_t *reply, size_t &replySize) {
  if (!debugIsActive()) return performS21(c0, c1, payload, size, expectReply, reply, replySize, nullptr);

  S21Trace &trace = debugTraces[traceNext];
  trace = {};
  trace.atMs = millis();
  trace.ok = performS21(c0, c1, payload, size, expectReply, reply, replySize, &trace);
  trace.elapsedMs = millis() - trace.atMs;
  if (!trace.ok) lastError.toCharArray(trace.error, sizeof(trace.error));
  traceNext = (traceNext + 1) % TRACE_COUNT;
  if (traceUsed < TRACE_COUNT) ++traceUsed;
  return trace.ok;
}

bool queryBasic() {
  uint8_t data[16]; size_t size = sizeof(data);
  if (!exchangeS21('F', '1', nullptr, 0, true, data, size)) return false;
  const char mode = size >= 2 && (data[1] == '0' || data[1] == '7') ? '1' : (size >= 2 ? data[1] : '\0');
  if (size < 4 || (data[0] != '0' && data[0] != '1') ||
      !validMode(mode) || !validFan(data[3])) {
    lastError = "Risposta F1 non riconosciuta"; return false;
  }
  ac.power = data[0] == '1'; ac.mode = mode; ac.fan = data[3];
  if (!ac.power && sleepTimerActive) {
    sleepTimerActive = false;
    sleepResult = "Split spento. Timer concluso.";
  }
  ac.rawSetpoint = data[2];
  // G1 puo' riportare Auto anche quando la ventola e' in modalita' Silenziosa.
  if (ac.fan == 'A') {
    uint8_t fanData[4]; size_t fanSize = sizeof(fanData);
    if (exchangeS21('R', 'G', nullptr, 0, true, fanData, fanSize) &&
        fanSize == 1 && validFan(fanData[0])) ac.fan = fanData[0];
  }
  // In FAN/DRY il byte temperatura puo' essere un valore speciale.
  if (ac.mode != '2' && ac.mode != '6') ac.target = (data[2] - 28) * 0.5f;
  else ac.target = NAN;
  ac.known = true; ac.basicRead = millis(); lastError = "";
  return true;
}

bool querySwing() {
  uint8_t data[16]; size_t size = sizeof(data);
  if (!exchangeS21('F', '5', nullptr, 0, true, data, size) || size < 1) return false;
  ac.swingV = data[0] & 1; ac.swingH = data[0] & 2; ac.swingKnown = true;
  return true;
}

void queryTemperature(char command, float &temperature) {
  uint8_t data[16]; size_t size = sizeof(data);
  if (!exchangeS21('R', command, nullptr, 0, true, data, size) || size < 4) {
    temperature = NAN; return;
  }
  if (data[0] < '0' || data[0] > '9' || data[1] < '0' || data[1] > '9' ||
      data[2] < '0' || data[2] > '9' || (data[3] != '+' && data[3] != '-')) {
    temperature = NAN; return;
  }
  int value = (data[0] - '0') + 10 * (data[1] - '0') + 100 * (data[2] - '0');
  temperature = (data[3] == '-' ? -value : value) / 10.0f;
}

bool sendBasic(bool power, char mode, uint8_t setpoint, char fan) {
  const uint8_t data[] = {(uint8_t)(power ? '1' : '0'), (uint8_t)mode, setpoint, (uint8_t)fan};
  size_t unused = 0;
  return exchangeS21('D', '1', data, sizeof(data), false, nullptr, unused);
}

bool sendSwing(bool vertical, bool horizontal) {
  const uint8_t flags = (vertical ? 1 : 0) + (horizontal ? 2 : 0) + (vertical && horizontal ? 4 : 0);
  const uint8_t data[] = {(uint8_t)('0' + flags), (uint8_t)((vertical || horizontal) ? '?' : '0'), '0', '0'};
  size_t unused = 0;
  return exchangeS21('D', '5', data, sizeof(data), false, nullptr, unused);
}

void pollAircon() {
  if ((int32_t)(millis() - nextPoll) < 0) return;
  nextPoll = millis() + 1200UL;
  if (!ac.known || (uint32_t)(millis() - ac.basicRead) > 6000UL) {
    queryBasic(); return;
  }
  // Ogni query di stato di base e' alternata con una funzione opzionale.
  const uint8_t slot = pollStep;
  pollStep = (pollStep + 1) % 14;
  switch (slot) {
    case 0: case 2: case 4: case 6: case 8: case 10: case 12: queryBasic(); break;
    case 1: queryTemperature('H', ac.room); break;
    case 3: queryTemperature('a', ac.outside); break;
    case 5: querySwing(); break;
    case 7: {
      uint8_t data[8]; size_t size = sizeof(data);
      const String previousError = lastError;
      ac.fanRpm = -1;
      if (exchangeS21('R', 'L', nullptr, 0, true, data, size) && size == 3 &&
          data[0] >= '0' && data[0] <= '9' && data[1] >= '0' && data[1] <= '9' &&
          data[2] >= '0' && data[2] <= '9') {
        ac.fanRpm = 10 * ((data[0] - '0') + 10 * (data[1] - '0') + 100 * (data[2] - '0'));
      }
      lastError = previousError;
      break;
    }
    case 13: {
      const String previousError = lastError;
      queryTemperature('X', ac.internalTarget);
      lastError = previousError;
      break;
    }
    case 9: case 11: {
      uint8_t data[4];
      const char command = slot == 9 ? '6' : '7';
      readSpecial(command, data);
      break;
    }
  }
}

String temperatureJson(float value) { return isnan(value) ? String("null") : String(value, 1); }

void appendJsonString(String &json, const char *value) {
  json += '"';
  for (; *value; ++value) {
    const uint8_t byte = *value;
    if (byte == '"' || byte == '\\') {
      json += '\\';
      json += *value;
    } else if (byte < 32) {
      char escape[7];
      snprintf(escape, sizeof(escape), "\\u%04x", unsigned(byte));
      json += escape;
    } else {
      json += *value;
    }
  }
  json += '"';
}

void handleStatus() {
  const bool online = ac.known && (uint32_t)(millis() - ac.basicRead) < 12000UL;
  String json;
  json.reserve(440);
  json = F("{\"online\":"); json += online ? "true" : "false";
  json += F(",\"known\":"); json += ac.known ? "true" : "false";
  json += F(",\"power\":"); json += ac.power ? "true" : "false";
  json += F(",\"mode\":\""); json += ac.mode;
  json += F("\",\"fan\":\""); json += ac.fan;
  json += F("\",\"target\":"); json += temperatureJson(ac.target);
  json += F(",\"requestedTarget\":");
  json += temperatureJson(ac.mode == '4' ? ac.target - heatOffsetSteps * 0.5f : ac.target);
  json += F(",\"internalTarget\":"); json += online ? temperatureJson(ac.internalTarget) : "null";
  json += F(",\"rxPin\":"); json += rxPin;
  json += F(",\"txPin\":"); json += txPin;
  json += F(",\"heatOffset\":"); json += String(heatOffsetSteps * 0.5f, 1);
  json += F(",\"turbo\":");
  json += online && ac.turbo >= 0 ? (ac.turbo ? "true" : "false") : "null";
  json += F(",\"eco\":");
  json += online && ac.eco >= 0 ? (ac.eco ? "true" : "false") : "null";
  json += F(",\"fanRpm\":"); json += online && ac.fanRpm >= 0 ? String(ac.fanRpm) : "null";
  json += F(",\"room\":"); json += online ? temperatureJson(ac.room) : "null";
  json += F(",\"outside\":"); json += online ? temperatureJson(ac.outside) : "null";
  json += F(",\"swingKnown\":"); json += ac.swingKnown ? "true" : "false";
  json += F(",\"vertical\":"); json += ac.swingV ? "true" : "false";
  json += F(",\"horizontal\":"); json += ac.swingH ? "true" : "false";
  json += F(",\"ap\":"); json += apActive ? "true" : "false";
  json += F(",\"ip\":\"");
  json += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  json += F("\",\"ssid\":"); appendJsonString(json, wifiSettings.ssid);
  const bool staticMode = networkSettings.magic == STATIC_MAGIC;
  const bool connected = WiFi.status() == WL_CONNECTED;
  json += F(",\"wifiConnected\":"); json += connected ? "true" : "false";
  json += F(",\"addressMode\":"); appendJsonString(json, staticMode ? "static" : "dhcp");
  json += F(",\"staticIp\":");
  appendJsonString(json, staticMode ? IPAddress(networkSettings.ip).toString().c_str() : "");
  json += F(",\"mask\":");
  appendJsonString(json, staticMode ? IPAddress(networkSettings.mask).toString().c_str() :
                        connected ? WiFi.subnetMask().toString().c_str() : "");
  json += F(",\"gateway\":");
  appendJsonString(json, staticMode ? IPAddress(networkSettings.gateway).toString().c_str() :
                        connected ? WiFi.gatewayIP().toString().c_str() : "");
  json += F(",\"dns\":");
  appendJsonString(json, staticMode ? IPAddress(networkSettings.dns).toString().c_str() : "");
  json += F(",\"error\":"); appendJsonString(json, lastError.c_str());
  struct tm local;
  char timeText[24] = {};
  if (localClock(local)) strftime(timeText, sizeof(timeText), "%a %H:%M:%S", &local);
  json += F(",\"localTime\":"); appendJsonString(json, timeText);
  json += F(",\"scheduleResult\":"); appendJsonString(json, scheduleResult.c_str());
  json += F(",\"sleepActive\":"); json += sleepTimerActive ? "true" : "false";
  json += F(",\"sleepSeconds\":"); json += sleepSeconds();
  json += F(",\"sleepResult\":"); appendJsonString(json, sleepResult.c_str());
  json += F(",\"prediction10m\":"); json += online ? temperatureJson(neuralPrediction) : "null";
  json += F(",\"learningStatus\":"); appendJsonString(json, neuralStatus.c_str());
  json += F(",\"learningSamples\":"); json += learningSamples;
  json += F(",\"uptimeSeconds\":"); json += millis() / 1000;
  json += F(",\"archiveReady\":"); json += archiveReady ? "true" : "false";
  json += F(",\"archiveStatus\":"); appendJsonString(json, archiveStatus.c_str());
  json += F(",\"archiveBytes\":"); json += archiveBytes();
  json += F(",\"archiveBufferedRows\":"); json += archiveBufferedRows;
  json += F(",\"version\":\"2.11\",\"debugActive\":");
  json += debugEnabled ? "true" : "false";
  json += '}';
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleSet() {
  const String power = server.arg("power"), mode = server.arg("mode"), fan = server.arg("fan");
  const bool changeFan = server.hasArg("fan");
  const String text = server.arg("temp");
  char *end = nullptr;
  const float target = strtof(text.c_str(), &end);
  if ((power != "0" && power != "1") || mode.length() != 1 || !validMode(mode[0]) ||
      (changeFan && (fan.length() != 1 || !validFan(fan[0]))) || !text.length() || *end != '\0' ||
      !isfinite(target) || target < 16 || target > 30 || fabsf(target * 2 - roundf(target * 2)) > 0.01f) {
    server.send(400, "text/plain; charset=utf-8", "Parametri non validi: temperatura 16-30 °C a passi di 0,5."); return;
  }
  if (!queryBasic()) { server.send(503, "text/plain; charset=utf-8", "Daikin non raggiungibile. Nessun comando inviato."); return; }
  const bool compensate = mode[0] == '4' && server.arg("compensate") == "1";
  const int halfDegrees = lroundf(target * 2) + (compensate ? heatOffsetSteps : 0);
  // The FTXM-M manual specifies a heating range of 10 to 30 degrees.
  if (halfDegrees < 20 || halfDegrees > 60) {
    server.send(400, "text/plain; charset=utf-8", "Temperatura risultante fuori intervallo.");
    return;
  }
  uint8_t setpoint = uint8_t(halfDegrees + 28);
  const bool hasSetpoint = mode[0] != '2' && mode[0] != '6';
  if (!hasSetpoint && mode[0] == ac.mode) setpoint = ac.rawSetpoint;
  const bool requestedPower = power == "1";
  const char requestedFan = changeFan ? fan[0] : ac.fan;
  if (ac.power == requestedPower && ac.mode == mode[0] &&
      ac.rawSetpoint == setpoint && ac.fan == requestedFan) {
    server.send(200, "text/plain; charset=utf-8", "Le impostazioni richieste sono già attive.");
    return;
  }
  if (!sendBasic(requestedPower, mode[0], setpoint, requestedFan)) {
    server.send(502, "text/plain; charset=utf-8", "Comando non confermato. Verifica lo stato del climatizzatore."); return;
  }
  delay(120);
  if (!queryBasic()) {
    server.send(202, "text/plain; charset=utf-8", "Comando ricevuto. Rilettura non riuscita, verifica lo stato.");
  } else if (ac.power != requestedPower || ac.mode != mode[0] ||
             (hasSetpoint && ac.rawSetpoint != setpoint) || ac.fan != requestedFan) {
    server.send(409, "text/plain; charset=utf-8", "Lo stato riletto dal Daikin differisce dalle impostazioni richieste.");
  } else {
    server.send(200, "text/plain; charset=utf-8", "Impostazioni confermate dal Daikin.");
  }
}

void handleSwing() {
  const String vertical = server.arg("vertical"), horizontal = server.arg("horizontal");
  if ((vertical != "0" && vertical != "1") || (horizontal != "0" && horizontal != "1")) {
    server.send(400, "text/plain", "Parametri non validi"); return;
  }
  if (!queryBasic() || !querySwing()) {
    server.send(503, "text/plain; charset=utf-8", "Oscillazione non disponibile o Daikin non raggiungibile."); return;
  }
  if (!sendSwing(vertical == "1", horizontal == "1")) {
    server.send(502, "text/plain; charset=utf-8", "Comando non confermato. Verifica lo stato."); return;
  }
  delay(120);
  if (querySwing()) server.send(200, "text/plain; charset=utf-8", "Comando ricevuto; oscillazione riletta dal Daikin.");
  else server.send(202, "text/plain; charset=utf-8", "Comando ricevuto (ACK), ma rilettura non riuscita.");
}

bool readSpecial(char command, uint8_t (&data)[4]) {
  size_t size = sizeof(data);
  const String previousError = lastError;
  const bool ok = exchangeS21('F', command, nullptr, 0, true, data, size) && size == sizeof(data);
  lastError = previousError;
  if (command == '6') ac.turbo = ok ? ((data[0] & 0x02) != 0) : -1;
  else ac.eco = ok && (data[1] == '0' || data[1] == '2') ? (data[1] == '2') : -1;
  return command == '6' ? ac.turbo >= 0 : ac.eco >= 0;
}

void handleSpecial() {
  const String feature = server.arg("feature");
  const String enabled = server.arg("enabled");
  if ((feature != "turbo" && feature != "eco") || (enabled != "0" && enabled != "1")) {
    server.send(400, "text/plain", "Comando non valido.");
    return;
  }
  const bool on = enabled == "1";
  const bool turbo = feature == "turbo";
  if (!queryBasic() || (on && (!ac.power || (!turbo && ac.mode == '6')))) {
    server.send(409, "text/plain; charset=utf-8", "Accendi il Daikin e scegli una modalità compatibile.");
    return;
  }
  const char command = turbo ? '6' : '7';
  uint8_t data[4];
  if (!readSpecial(command, data)) {
    server.send(503, "text/plain; charset=utf-8", "Il Daikin non ha fornito lo stato di questa funzione.");
    return;
  }
  if ((turbo ? ac.turbo : ac.eco) == on) {
    server.send(200, "text/plain; charset=utf-8", "Impostazione già attiva.");
    return;
  }
  if (turbo) {
    // Preserve the other G6 flags, including Streamer and Comfort.
    if (on) data[0] |= 0x02;
    else data[0] &= ~0x02;
  } else {
    // Keep the existing demand limit. D7 uses zero for its reserved bytes.
    data[1] = on ? '2' : '0';
    data[2] = data[3] = '0';
  }
  size_t unused = 0;
  if (!exchangeS21('D', command, data, sizeof(data), false, nullptr, unused)) {
    server.send(502, "text/plain; charset=utf-8", "Il Daikin non ha confermato il comando.");
    return;
  }
  delay(150);
  const bool confirmed = readSpecial(command, data) && (turbo ? ac.turbo : ac.eco) == on;
  uint8_t other[4];
  readSpecial(turbo ? '7' : '6', other);
  server.send(confirmed ? 200 : 409, "text/plain; charset=utf-8",
              confirmed ? "Impostazione confermata dal Daikin." : "La rilettura non conferma l'impostazione.");
}

void handleWifi() {
  const String ssid = server.arg("ssid");
  String password = server.arg("password");
  if (server.arg("openNetwork") == "1") password = "";
  else if (password.isEmpty() && ssid == wifiSettings.ssid) password = wifiSettings.password;
  NetworkSettings network = {};
  if (!readNetworkForm(network)) {
    server.send(400, "text/plain; charset=utf-8", "Controlla IP, maschera, gateway e DNS. IP e gateway devono essere indirizzi distinti nella stessa sottorete.");
    return;
  }
  if (ssid.length() < 1 || ssid.length() > 32 || password.length() > 63 ||
      (password.length() > 0 && password.length() < 8)) {
    server.send(400, "text/plain; charset=utf-8", "SSID: 1-32 byte. Password: 8-63 caratteri, oppure vuota per rete aperta."); return;
  }
  if (!saveWifi(ssid, password, network)) { server.send(500, "text/plain", "Salvataggio non riuscito"); return; }
  const String address = network.magic == STATIC_MAGIC ? IPAddress(network.ip).toString() : "daikin-wemos.local";
  server.send(200, "text/plain; charset=utf-8", "Salvato. Dopo il riavvio apri http://" + address);
  restartAt = millis() + 1500UL;
}

void loadFirmwareKey() {
  EEPROM.get(UPDATE_KEY_OFFSET, firmwareKey);
  if (firmwareKey.magic != UPDATE_KEY_MAGIC || firmwareKey.value[32] != '\0' ||
      firmwareKey.checksum != configChecksum(&firmwareKey, offsetof(FirmwareKey, checksum))) {
    firmwareKey = {};
    firmwareKey.magic = UPDATE_KEY_MAGIC;
    uint8_t bytes[16];
    ESP.random(bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); ++i) {
      snprintf(firmwareKey.value + 2 * i, 3, "%02x", unsigned(bytes[i]));
    }
    firmwareKey.checksum = configChecksum(&firmwareKey, offsetof(FirmwareKey, checksum));
    EEPROM.put(UPDATE_KEY_OFFSET, firmwareKey);
    if (!EEPROM.commit()) Serial.println("Impossibile salvare la chiave firmware.");
  }
  Serial.printf("Chiave aggiornamento firmware: %s\n", firmwareKey.value);
}

void receiveFirmware() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (Update.isRunning()) Update.end();
    firmwareComplete = false;
    firmwareSize = 0;
    firmwareReceived = 0;
    firmwareError = "";
    if (server.header("X-Firmware-Key") != firmwareKey.value || restartAt) return;
    const String sizeHeader = server.header("X-Firmware-Size");
    char *end = nullptr;
    const unsigned long size = strtoul(sizeHeader.c_str(), &end, 10);
    if (upload.name != "firmware" || !upload.filename.endsWith(".bin") ||
        end == sizeHeader.c_str() || *end || size == 0 || size >= ESP.getFreeSketchSpace()) {
      firmwareError = "File .bin non valido o troppo grande per lo spazio disponibile.";
      return;
    }
    if (archiveReady && !flushArchive()) {
      firmwareError = "Salvataggio dati non riuscito. Scarica il CSV prima di aggiornare.";
      return;
    }
    firmwareSize = size;
    // Reserve one extra byte so end(false) always cancels an unfinished request.
    if (!Update.begin(firmwareSize + 1, U_FLASH)) {
      firmwareError = "Impossibile iniziare: " + Update.getErrorString();
    }
  } else if (upload.status == UPLOAD_FILE_WRITE && Update.isRunning() && firmwareError.isEmpty()) {
    if (firmwareReceived + upload.currentSize > firmwareSize) {
      firmwareError = "Dimensione del file diversa da quella dichiarata.";
      Update.end();
    } else if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      firmwareError = "Scrittura fallita: " + Update.getErrorString();
    } else {
      firmwareReceived += upload.currentSize;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    firmwareComplete = Update.isRunning() && firmwareError.isEmpty() &&
                       upload.totalSize == firmwareSize && firmwareReceived == firmwareSize;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (Update.isRunning()) Update.end();
    firmwareComplete = false;
    firmwareSize = 0;
  }
}

void finishFirmware() {
  if (server.header("X-Firmware-Key") != firmwareKey.value) {
    server.send(403, "text/plain; charset=utf-8", "Chiave di aggiornamento errata.");
    return;
  }
  if (restartAt || !firmwareComplete || !Update.isRunning()) {
    if (Update.isRunning()) Update.end();
    server.send(400, "text/plain; charset=utf-8",
                firmwareError.isEmpty() ? "Caricamento incompleto." : firmwareError);
  } else if (!Update.end(true)) {
    server.send(400, "text/plain; charset=utf-8", "Firmware rifiutato: " + Update.getErrorString());
  } else {
    server.send(200, "text/plain; charset=utf-8", "Firmware ricevuto. Riavvio in corso.");
    restartAt = millis() + 1500UL;
  }
  firmwareComplete = false;
  firmwareSize = 0;
  firmwareError = "";
}

bool debugIsActive() { return debugEnabled; }

void loadDebugSettings() {
  DebugSettings settings = {};
  EEPROM.get(DEBUG_SETTINGS_ADDRESS, settings);
  debugEnabled = settings.magic == DEBUG_SETTINGS_MAGIC && settings.enabled == 1 &&
      settings.checksum == configChecksum(&settings, offsetof(DebugSettings, checksum));
}

void configureDebug() {
  if (server.header("X-Firmware-Key") != firmwareKey.value) {
    server.send(403, "text/plain; charset=utf-8", "Chiave errata.");
    return;
  }
  const String enabled = server.arg("enabled");
  if (enabled != "0" && enabled != "1") {
    server.send(400, "text/plain", "Valore non valido.");
    return;
  }
  DebugSettings settings = {DEBUG_SETTINGS_MAGIC, uint32_t(enabled == "1"), 0};
  settings.checksum = configChecksum(&settings, offsetof(DebugSettings, checksum));
  EEPROM.put(DEBUG_SETTINGS_ADDRESS, settings);
  if (!EEPROM.commit()) {
    server.send(500, "text/plain", "Salvataggio non riuscito.");
    return;
  }
  debugEnabled = settings.enabled == 1;
  traceUsed = 0;
  traceNext = 0;
  server.send(200, "text/plain; charset=utf-8",
              debugEnabled ? "Debug attivo fino alla disattivazione manuale." : "Debug disattivato.");
}

void appendHex(String &json, const uint8_t *bytes, size_t size) {
  static const char hex[] = "0123456789ABCDEF";
  json += '"';
  for (size_t i = 0; i < size; ++i) {
    if (i) json += ' ';
    json += hex[bytes[i] >> 4];
    json += hex[bytes[i] & 15];
  }
  json += '"';
}

void appendTrace(String &json, const S21Trace &trace) {
  json += F("{\"atMs\":"); json += trace.atMs;
  json += F(",\"elapsedMs\":"); json += trace.elapsedMs;
  json += F(",\"ok\":"); json += trace.ok ? "true" : "false";
  json += F(",\"truncated\":"); json += trace.truncated ? "true" : "false";
  json += F(",\"tx\":"); appendHex(json, trace.tx, trace.txSize);
  json += F(",\"rx\":"); appendHex(json, trace.rx, trace.rxSize);
  json += F(",\"error\":"); appendJsonString(json, trace.error);
  json += '}';
}

void handleDebugLog() {
  if (!debugIsActive()) {
    server.send(403, "text/plain", "Debug disattivato.");
    return;
  }
  String json;
  json.reserve(5200);
  json = F("{\"debugActive\":true");
  json += F(",\"uptimeMs\":"); json += millis();
  json += F(",\"freeHeap\":"); json += ESP.getFreeHeap();
  json += F(",\"rssiDbm\":");
  json += WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : "null";
  json += F(",\"frames\":[");
  for (uint8_t i = 0; i < traceUsed; ++i) {
    if (i) json += ',';
    const uint8_t index = (traceNext + TRACE_COUNT - traceUsed + i) % TRACE_COUNT;
    appendTrace(json, debugTraces[index]);
  }
  json += F("]}");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleDebugProbe() {
  if (!debugIsActive() || server.header("X-Debug-Request") != "1") {
    server.send(403, "text/plain", "Sessione debug o header richiesto.");
    return;
  }
  const String command = server.arg("command");
  const char *const allowed[] = {"F1", "F5", "F6", "F7", "RG", "RH", "RI",
                                 "RL", "RX", "Ra", "Rd", "Rg"};
  bool supported = false;
  for (const char *entry : allowed) {
    if (command == entry) { supported = true; break; }
  }
  if (!supported) {
    server.send(400, "text/plain", "Comando di lettura non previsto.");
    return;
  }
  uint8_t response[60];
  size_t size = sizeof(response);
  const String previousError = lastError;
  const uint8_t index = traceNext;
  const bool ok = exchangeS21(command[0], command[1], nullptr, 0, true, response, size);
  lastError = previousError;
  if (traceNext == index) {
    server.send(403, "text/plain", "Debug disattivato.");
    return;
  }
  String json;
  json.reserve(480);
  json = F("{\"command\":"); appendJsonString(json, command.c_str());
  json += F(",\"payload\":"); appendHex(json, response, ok ? size : 0);
  json += F(",\"trace\":"); appendTrace(json, debugTraces[index]);
  json += '}';
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

bool validS21Pin(uint32_t pin) {
  return pin == D1 || pin == D2 || pin == D5 || pin == D6 || pin == D7;
}

void loadPins() {
  PinSettings settings = {};
  EEPROM.get(PIN_SETTINGS_ADDRESS, settings);
  if (settings.magic == PIN_SETTINGS_MAGIC && validS21Pin(settings.rx) &&
      validS21Pin(settings.tx) && settings.rx != settings.tx &&
      settings.checksum == configChecksum(&settings, offsetof(PinSettings, checksum))) {
    rxPin = settings.rx;
    txPin = settings.tx;
  }
}

void savePins() {
  uint16_t rx = 0, tx = 0;
  if (!parseScheduleNumber(server.arg("rx"), 16, rx) ||
      !parseScheduleNumber(server.arg("tx"), 16, tx) ||
      !validS21Pin(rx) || !validS21Pin(tx) || rx == tx) {
    server.send(400, "text/plain; charset=utf-8", "Scegli due pin diversi tra D1, D2, D5, D6 e D7.");
    return;
  }
  if (rx != rxPin || tx != txPin) {
    PinSettings settings = {PIN_SETTINGS_MAGIC, rx, tx, 0};
    settings.checksum = configChecksum(&settings, offsetof(PinSettings, checksum));
    EEPROM.put(PIN_SETTINGS_ADDRESS, settings);
    if (!EEPROM.commit()) {
      server.send(500, "text/plain; charset=utf-8", "Salvataggio pin non riuscito.");
      return;
    }
    s21.end();
    pinMode(txPin, INPUT);
    rxPin = rx;
    txPin = tx;
    s21.begin(2400, EspSoftwareSerial::SWSERIAL_8E2, rxPin, txPin, false);
    ac = AcState{};
    nextPoll = 0;
    pollStep = 0;
    learningCount = learningNext = 0;
    neuralPrediction = NAN;
    lastError = "Pin modificati: in attesa di risposta S21";
  }
  server.send(200, "text/plain; charset=utf-8", "Pin TX / RX salvati e applicati.");
}

void loadHeatOffset() {
  HeatOffsetSettings settings = {};
  EEPROM.get(HEAT_OFFSET_ADDRESS, settings);
  if (settings.magic == HEAT_OFFSET_MAGIC && settings.halfDegrees >= -10 &&
      settings.halfDegrees <= 0 &&
      settings.checksum == configChecksum(&settings, offsetof(HeatOffsetSettings, checksum))) {
    heatOffsetSteps = settings.halfDegrees;
  }
}

void saveHeatOffset() {
  const String value = server.arg("offset");
  char *end = nullptr;
  const float offset = strtof(value.c_str(), &end);
  if (end == value.c_str() || *end || !isfinite(offset) || offset < -5 || offset > 0 ||
      offset * 2 != roundf(offset * 2)) {
    server.send(400, "text/plain; charset=utf-8", "Scegli un valore tra −5 e 0 °C a passi di 0,5.");
    return;
  }
  HeatOffsetSettings settings = {HEAT_OFFSET_MAGIC, int32_t(lroundf(offset * 2)), 0};
  settings.checksum = configChecksum(&settings, offsetof(HeatOffsetSettings, checksum));
  EEPROM.put(HEAT_OFFSET_ADDRESS, settings);
  if (!EEPROM.commit()) {
    server.send(500, "text/plain; charset=utf-8", "Salvataggio non riuscito.");
    return;
  }
  heatOffsetSteps = settings.halfDegrees;
  server.send(200, "text/plain; charset=utf-8", "Compensazione salvata. Si userà alla prossima modifica della temperatura.");
}

bool localClock(struct tm &local) {
  const time_t now = time(nullptr);
  return now >= 1704067200 && localtime_r(&now, &local) != nullptr;
}

bool validScheduleRule(const ScheduleRule &rule) {
  if (!rule.days || rule.days > 127 || rule.minute >= 1440 || rule.enabled > 1) return false;
  // Mode '0' identifies an OFF rule in EEPROM. It is never sent as an S21 mode.
  if (rule.mode == '0') return rule.halfDegrees == 0;
  if (rule.mode != '1' && rule.mode != '3' && rule.mode != '4') return false;
  return rule.halfDegrees >= (rule.mode == '4' ? 32 : 36) && rule.halfDegrees <= 60;
}

void loadSchedule() {
  EEPROM.get(SCHEDULE_ADDRESS, schedule);
  if (schedule.magic != SCHEDULE_MAGIC ||
      schedule.checksum != configChecksum(&schedule, offsetof(ScheduleSettings, checksum))) {
    schedule = {};
    return;
  }
  for (ScheduleRule &rule : schedule.rules) {
    if (!validScheduleRule(rule)) rule = {};
  }
}

void readSchedule() {
  String json = "[";
  for (const ScheduleRule &rule : schedule.rules) {
    if (!rule.days) continue;
    if (json.length() > 1) json += ',';
    json += F("{\"days\":"); json += rule.days;
    json += F(",\"minute\":"); json += rule.minute;
    json += F(",\"temp\":"); json += String(rule.halfDegrees * 0.5f, 1);
    json += F(",\"mode\":\""); json += rule.mode;
    json += F("\",\"enabled\":"); json += rule.enabled ? "true" : "false";
    json += '}';
  }
  json += ']';
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

bool parseScheduleNumber(const String &value, uint16_t maximum, uint16_t &result) {
  if (value.isEmpty() || value.length() > 4) return false;
  uint16_t number = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
    number = number * 10 + value[i] - '0';
  }
  if (number > maximum) return false;
  result = number;
  return true;
}

void saveSchedule() {
  uint16_t count;
  if (!parseScheduleNumber(server.arg("count"), SCHEDULE_LIMIT, count)) {
    server.send(400, "text/plain; charset=utf-8", "Numero di orari non valido.");
    return;
  }
  ScheduleSettings fresh = {};
  fresh.magic = SCHEDULE_MAGIC;
  for (uint8_t i = 0; i < count; ++i) {
    const String suffix(i);
    uint16_t days, minute, temperature, enabled;
    const String mode = server.arg("mode" + suffix);
    if (mode.length() != 1 ||
        !parseScheduleNumber(server.arg("days" + suffix), 127, days) ||
        !parseScheduleNumber(server.arg("minute" + suffix), 1439, minute) ||
        !parseScheduleNumber(server.arg("temp" + suffix), 60, temperature) ||
        !parseScheduleNumber(server.arg("enabled" + suffix), 1, enabled)) {
      server.send(400, "text/plain; charset=utf-8", "Orario non valido.");
      return;
    }
    const ScheduleRule rule = {minute, uint8_t(days), uint8_t(temperature), mode[0], uint8_t(enabled)};
    if (!validScheduleRule(rule)) {
      server.send(400, "text/plain; charset=utf-8", "Controlla giorni e temperatura. Minimo 18 °C in Raffrescamento e Automatico.");
      return;
    }
    for (uint8_t j = 0; j < i; ++j) {
      const ScheduleRule &other = fresh.rules[j];
      if (rule.enabled && other.enabled && rule.minute == other.minute && (rule.days & other.days)) {
        server.send(409, "text/plain; charset=utf-8", "Due orari attivi coincidono nello stesso giorno.");
        return;
      }
    }
    fresh.rules[i] = rule;
  }
  fresh.checksum = configChecksum(&fresh, offsetof(ScheduleSettings, checksum));
  EEPROM.put(SCHEDULE_ADDRESS, fresh);
  if (!EEPROM.commit()) {
    server.send(500, "text/plain; charset=utf-8", "Salvataggio non riuscito.");
    return;
  }
  uint32_t retainedDays[SCHEDULE_LIMIT] = {};
  for (uint8_t i = 0; i < count; ++i) {
    const ScheduleRule &rule = fresh.rules[i];
    for (uint8_t j = 0; j < SCHEDULE_LIMIT; ++j) {
      const ScheduleRule &old = schedule.rules[j];
      if (old.minute == rule.minute && old.mode == rule.mode && old.halfDegrees == rule.halfDegrees) {
        if (lastScheduleDay[j] > retainedDays[i]) retainedDays[i] = lastScheduleDay[j];
      }
    }
  }
  memcpy(lastScheduleDay, retainedDays, sizeof(lastScheduleDay));
  schedule = fresh;
  server.send(200, "text/plain; charset=utf-8", "Programmazione salvata.");
}

void tickSchedule() {
  if (int32_t(millis() - nextScheduleCheck) < 0) return;
  nextScheduleCheck = millis() + 5000UL;
  struct tm local;
  if (!localClock(local)) return;
  const uint16_t minute = local.tm_hour * 60 + local.tm_min;
  const uint32_t day = uint32_t(local.tm_year) * 366 + local.tm_yday;
  for (uint8_t i = 0; i < SCHEDULE_LIMIT; ++i) {
    const ScheduleRule &rule = schedule.rules[i];
    if (!rule.enabled || rule.minute != minute || !(rule.days & (1U << local.tm_wday)) ||
        lastScheduleDay[i] == day) continue;
    char timeText[8];
    strftime(timeText, sizeof(timeText), "%H:%M", &local);
    const bool turnOn = rule.mode != '0';
    bool confirmed = false;
    if (queryBasic()) {
      const char mode = turnOn ? rule.mode : ac.mode;
      const uint8_t setpoint = turnOn
          ? rule.halfDegrees + (mode == '4' ? heatOffsetSteps : 0) + 28
          : ac.rawSetpoint;
      confirmed = ac.power == turnOn && (!turnOn || (ac.mode == mode && ac.rawSetpoint == setpoint));
      if (!confirmed && sendBasic(turnOn, mode, setpoint, ac.fan)) {
        delay(120);
        confirmed = queryBasic() && ac.power == turnOn &&
                    (!turnOn || (ac.mode == mode && ac.rawSetpoint == setpoint));
      }
    }
    scheduleResult = String(timeText) + (turnOn ? " · Accensione " : " · Spegnimento ") +
                     (turnOn ? (confirmed ? "confermata" : "non confermata")
                             : (confirmed ? "confermato" : "non confermato"));
    if (confirmed) lastScheduleDay[i] = day;
    break;
  }
}

uint32_t sleepSeconds() {
  if (!sleepTimerActive) return 0;
  const int32_t remaining = int32_t(sleepDeadline - millis());
  return remaining > 0 ? (uint32_t(remaining) + 999) / 1000 : 0;
}

void setSleepTimer() {
  uint16_t minutes;
  if (!parseScheduleNumber(server.arg("minutes"), 1440, minutes)) {
    server.send(400, "text/plain; charset=utf-8", "Inserisci da 1 a 1440 minuti.");
    return;
  }
  if (!minutes) {
    sleepTimerActive = false;
    sleepResult = "";
    server.send(200, "text/plain; charset=utf-8", "Timer annullato.");
    return;
  }
  if (!queryBasic() || !ac.power) {
    server.send(409, "text/plain; charset=utf-8", "Accendi il Daikin prima di avviare il timer.");
    return;
  }
  sleepDeadline = millis() + uint32_t(minutes) * 60000UL;
  sleepTimerActive = true;
  sleepResult = "";
  nextSleepAttempt = sleepDeadline;
  server.send(200, "text/plain; charset=utf-8", "Timer avviato.");
}

void tickSleepTimer() {
  if (!sleepTimerActive || int32_t(millis() - sleepDeadline) < 0) return;
  if (int32_t(millis() - nextSleepAttempt) < 0) return;
  nextSleepAttempt = millis() + 5000UL;
  if (queryBasic()) {
    if (!sleepTimerActive) return;
    if (sendBasic(false, ac.mode, ac.rawSetpoint, ac.fan)) {
      delay(120);
      if (queryBasic() && !ac.power) {
        sleepTimerActive = false;
        sleepResult = "Spegnimento confermato.";
        return;
      }
    }
  }
  sleepResult = "Spegnimento non confermato. Nuovo tentativo tra 5 secondi.";
  if (uint32_t(millis() - sleepDeadline) >= 60000UL) {
    sleepTimerActive = false;
    sleepResult = "Spegnimento non confermato. Verifica il climatizzatore.";
  }
}

bool validNeuralModel(const NeuralModel &model) {
  if (model.magic != MODEL_MAGIC) return false;
  for (uint16_t i = 0; i < MODEL_VALUES; ++i)
    if (!isfinite(model.values[i]) || fabsf(model.values[i]) > 100000) return false;
  for (uint8_t i = 0; i < MODEL_INPUTS; ++i) {
    if (model.values[12 + i] < 0.0001f ||
        model.values[24 + i] > model.values[36 + i]) return false;
  }
  return true;
}

void loadNeuralModel() {
  EEPROM.get(MODEL_ADDRESS, neuralModel);
  if (!validNeuralModel(neuralModel) ||
      neuralModel.checksum != configChecksum(&neuralModel, offsetof(NeuralModel, checksum))) {
    neuralModel = {};
  }
}

void uploadNeuralModel() {
  if (server.header("X-Firmware-Key") != firmwareKey.value) {
    server.send(403, "text/plain", "Chiave errata.");
    return;
  }
  const String text = server.arg("model");
  if (!text.startsWith("DWMODEL1\n") || text.length() > 14000) {
    server.send(400, "text/plain", "Formato modello non valido.");
    return;
  }
  NeuralModel fresh = {};
  fresh.magic = MODEL_MAGIC;
  const char *cursor = text.c_str() + 9;
  for (uint16_t i = 0; i < MODEL_VALUES; ++i) {
    char *end = nullptr;
    fresh.values[i] = strtof(cursor, &end);
    if (end == cursor) {
      server.send(400, "text/plain", "Coefficienti mancanti.");
      return;
    }
    cursor = end;
  }
  while (*cursor && isspace(static_cast<unsigned char>(*cursor))) ++cursor;
  if (*cursor || !validNeuralModel(fresh)) {
    server.send(400, "text/plain", "Coefficienti non validi.");
    return;
  }
  fresh.checksum = configChecksum(&fresh, offsetof(NeuralModel, checksum));
  EEPROM.put(MODEL_ADDRESS, fresh);
  if (!EEPROM.commit()) {
    server.send(500, "text/plain", "Salvataggio non riuscito.");
    return;
  }
  neuralModel = fresh;
  neuralPrediction = NAN;
  neuralStatus = "Modello caricato. In attesa della prossima lettura.";
  server.send(200, "text/plain", "Modello salvato in modalita osservazione.");
}

float predictNeuralDelta(const float *normalized, const NeuralModel &model) {
  float delta = model.values[160];
  for (uint8_t hidden = 0; hidden < MODEL_HIDDEN; ++hidden) {
    float sum = model.values[144 + hidden];
    for (uint8_t i = 0; i < MODEL_INPUTS; ++i)
      sum += normalized[i] * model.values[48 + i * MODEL_HIDDEN + hidden];
    delta += tanhf(sum) * model.values[152 + hidden];
  }
  return delta;
}

void tickLearning() {
  if (int32_t(millis() - learningDue) < 0) return;
  learningDue = millis() + 30000UL;
  neuralPrediction = NAN;
  if (!ac.known || uint32_t(millis() - ac.basicRead) >= 12000UL || !isfinite(ac.room)) {
    learningCount = 0;
    learningNext = 0;
    neuralStatus = "Temperatura ambiente non disponibile";
    return;
  }
  ++learningSamples;
  learningRooms[learningNext] = ac.room;
  learningTimes[learningNext] = millis();
  learningNext = (learningNext + 1) % 11;
  if (learningCount < 11) ++learningCount;
  if (learningCount < 11) {
    neuralStatus = "Raccolta dello storico di cinque minuti";
    return;
  }
  const uint32_t elapsed = millis() - learningTimes[learningNext];
  if (elapsed < 265000UL || elapsed > 335000UL) {
    learningCount = 0;
    learningNext = 0;
    neuralStatus = "Storico discontinuo. Nuova raccolta.";
    return;
  }
  learningTrend = ac.room - learningRooms[learningNext];
  if (neuralModel.magic != MODEL_MAGIC) {
    neuralStatus = "Dati disponibili. Modello da addestrare sul PC.";
    return;
  }
  const float input[MODEL_INPUTS] = {
    ac.room, ac.outside, ac.target, ac.internalTarget, float(ac.fanRpm), learningTrend,
    float(ac.power), float(ac.mode == '4'), float(ac.mode == '3'), float(ac.mode == '1'),
    float(ac.turbo), float(ac.eco)
  };
  if (ac.fanRpm < 0 || ac.turbo < 0 || ac.eco < 0) {
    neuralStatus = "Sensori incompleti";
    return;
  }
  float normalized[MODEL_INPUTS];
  for (uint8_t i = 0; i < MODEL_INPUTS; ++i) {
    const float value = input[i], scale = neuralModel.values[12 + i];
    const float margin = 0.5f * fmaxf(scale, 0.1f);
    if (!isfinite(value) || value < neuralModel.values[24 + i] - margin ||
        value > neuralModel.values[36 + i] + margin) {
      neuralStatus = "Condizioni fuori dai dati di addestramento";
      return;
    }
    normalized[i] = (value - neuralModel.values[i]) / scale;
  }
  const float delta = predictNeuralDelta(normalized, neuralModel);
  if (!isfinite(delta) || fabsf(delta) > 4) {
    neuralStatus = "Previsione fuori intervallo";
    return;
  }
  neuralPrediction = ac.room + delta;
  neuralStatus = "Previsione in osservazione";
}

void loadArchive() {
  LittleFS.setConfig(LittleFSConfig(false));
  archiveReady = LittleFS.begin();
  archiveStatus = archiveReady ? "Raccolta attiva sul Wemos" : "Archivio da inizializzare";
  archiveBuffer.reserve(2400);
}

size_t archiveBytes() {
  if (!archiveReady) return 0;
  size_t size = 0;
  for (const char *path : {ARCHIVE_PREVIOUS, ARCHIVE_CURRENT}) {
    File file = LittleFS.open(path, "r");
    if (file) size += file.size();
  }
  return size;
}

bool flushArchive() {
  if (archiveBuffer.isEmpty()) return true;
  File current = LittleFS.open(ARCHIVE_CURRENT, "a");
  if (!current) {
    archiveStatus = "Scrittura archivio non riuscita";
    return false;
  }
  if (current.size() + archiveBuffer.length() > ARCHIVE_FILE_LIMIT) {
    current.close();
    if (LittleFS.exists(ARCHIVE_PREVIOUS) && !LittleFS.remove(ARCHIVE_PREVIOUS)) {
      archiveStatus = "Rotazione archivio non riuscita";
      return false;
    }
    if (!LittleFS.rename(ARCHIVE_CURRENT, ARCHIVE_PREVIOUS)) {
      archiveStatus = "Rotazione archivio non riuscita";
      return false;
    }
    current = LittleFS.open(ARCHIVE_CURRENT, "a");
    if (!current) {
      archiveStatus = "Creazione archivio non riuscita";
      return false;
    }
  }
  const size_t previousSize = current.size();
  const size_t written = current.print(archiveBuffer);
  current.flush();
  if (written != archiveBuffer.length()) {
    current.truncate(previousSize);
    archiveStatus = "Memoria archivio piena o errore di scrittura";
    return false;
  }
  archiveBuffer = "";
  archiveBufferedRows = 0;
  archiveStatus = "Raccolta attiva sul Wemos";
  return true;
}

String csvNumber(float value) {
  return isfinite(value) ? String(value, 1) : String("");
}

void tickArchive() {
  if (!archiveReady || int32_t(millis() - archiveDue) < 0) return;
  archiveDue = millis() + 30000UL;
  const time_t now = time(nullptr);
  if (now < 1704067200) {
    archiveStatus = "Raccolta in attesa di sincronizzazione orologio";
    return;
  }
  if (archiveBufferedRows >= 10 && !flushArchive()) return;
  struct tm utc;
  gmtime_r(&now, &utc);
  char date[24];
  strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%SZ", &utc);
  const bool online = ac.known && uint32_t(millis() - ac.basicRead) < 12000UL;
  String row;
  row.reserve(180);
  row = date;
  row += ','; row += uint32_t(now);
  row += ','; row += online ? '1' : '0';
  row += F(",2.11,"); row += ac.mode;
  row += ','; row += ac.fan;
  row += ','; row += online ? csvNumber(ac.room) : "";
  row += ','; row += online ? csvNumber(ac.outside) : "";
  row += ','; row += online ? csvNumber(ac.target) : "";
  row += ','; row += online ? csvNumber(ac.internalTarget) : "";
  row += ','; if (online && ac.fanRpm >= 0) row += ac.fanRpm;
  row += ','; row += ac.power ? '1' : '0';
  row += ','; if (ac.turbo >= 0) row += int(ac.turbo);
  row += ','; if (ac.eco >= 0) row += int(ac.eco);
  row += ','; row += online ? csvNumber(neuralPrediction) : "";
  row += '\n';
  archiveBuffer += row;
  ++archiveBufferedRows;
  if (archiveBufferedRows >= 10) flushArchive();
}

void initializeArchive() {
  if (server.header("X-Firmware-Key") != firmwareKey.value) {
    server.send(403, "text/plain", "Chiave errata.");
    return;
  }
  if (archiveReady) {
    server.send(200, "text/plain", "Archivio gia disponibile.");
    return;
  }
  // Formatting is explicit. A failed mount never automatically erases the file partition.
  if (server.arg("confirm") != "1") {
    server.send(400, "text/plain", "Conferma la preparazione della memoria dei file.");
    return;
  }
  if (!LittleFS.format() || !LittleFS.begin()) {
    server.send(500, "text/plain", "Inizializzazione non riuscita.");
    return;
  }
  archiveReady = true;
  archiveStatus = "Raccolta attiva sul Wemos";
  archiveDue = millis();
  server.send(200, "text/plain", "Archivio pronto. La raccolta parte automaticamente.");
}

void downloadArchive() {
  if (!archiveReady) {
    server.send(503, "text/plain", "Archivio non disponibile.");
    return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=daikin-dati.csv");
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv; charset=utf-8", "");
  server.sendContent_P(ARCHIVE_HEADER);
  for (const char *path : {ARCHIVE_PREVIOUS, ARCHIVE_CURRENT}) {
    File file = LittleFS.open(path, "r");
    if (file) server.sendContent(file, file.size());
  }
  server.sendContent(archiveBuffer);
  server.sendContent("");
}

void setupRoutes() {
  server.collectHeaders("X-Firmware-Key", "X-Firmware-Size", "X-Debug-Request");
  server.on("/api/debug/session", HTTP_POST, configureDebug);
  server.on("/api/debug/log", HTTP_GET, handleDebugLog);
  server.on("/api/debug/probe", HTTP_POST, handleDebugProbe);
  server.on("/api/firmware", HTTP_POST, finishFirmware, receiveFirmware);
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/api/state", HTTP_GET, handleStatus);
  server.on("/api/set", HTTP_POST, handleSet);
  server.on("/api/sleep", HTTP_POST, setSleepTimer);
  server.on("/api/learning/model", HTTP_POST, uploadNeuralModel);
  server.on("/api/learning/archive", HTTP_POST, initializeArchive);
  server.on("/api/learning/download", HTTP_GET, downloadArchive);
  server.on("/api/pins", HTTP_POST, savePins);
  server.on("/api/heat-offset", HTTP_POST, saveHeatOffset);
  server.on("/api/schedule", HTTP_GET, readSchedule);
  server.on("/api/schedule", HTTP_POST, saveSchedule);
  server.on("/api/swing", HTTP_POST, handleSwing);
  server.on("/api/special", HTTP_POST, handleSpecial);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.onNotFound([]() {
    if (apActive) { server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true); server.send(302, "text/plain", ""); }
    else server.send(404, "text/plain", "Not found");
  });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(2048);
  loadPins();
  Serial.printf("\nDaikinWemos 2.11 - RX GPIO%u / TX GPIO%u - S21 2400 8E2\n", rxPin, txPin);
  s21.begin(2400, EspSoftwareSerial::SWSERIAL_8E2, rxPin, txPin, false);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.hostname("daikin-wemos");
  WiFi.setAutoReconnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  loadFirmwareKey();
  loadHeatOffset();
  loadSchedule();
  loadNeuralModel();
  loadDebugSettings();
  loadArchive();
  configTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com");
  char name[32];
  snprintf(name, sizeof(name), "WiFi-daikin-%06X", ESP.getChipId());
  apName = name;
  if (loadWifi()) {
    loadNetwork();
    if (networkSettings.magic == STATIC_MAGIC) {
      startAccessPoint();
      if (!WiFi.config(IPAddress(networkSettings.ip), IPAddress(networkSettings.gateway),
                       IPAddress(networkSettings.mask), IPAddress(networkSettings.dns))) {
        Serial.println("Configurazione IP non applicata. Usa il portale di recupero.");
        setupRoutes();
        return;
      }
    }
    WiFi.begin(wifiSettings.ssid, wifiSettings.password);
    Serial.println("Connessione al Wi-Fi salvato...");
  } else {
    wifiSettings = {};
    startAccessPoint();
  }
  setupRoutes();
}

void loop() {
  server.handleClient();
  // An interrupted HTTP request must not leave an unfinished update open.
  if (Update.isRunning()) {
    Update.end();
    firmwareComplete = false;
  }
  maintainWifi();
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) ESP.restart();
  if (!restartAt) {
    tickSchedule();
    tickSleepTimer();
    pollAircon();
    tickLearning();
    tickArchive();
  }
  delay(1);
}

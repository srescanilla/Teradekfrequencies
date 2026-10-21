/*
  Pantalla de freqüències — Elecrow CrowPanel ESP32-S3 4.2" (ADHESIU VERD) + BLE
  ==========================================================================
  Munta sobre CrowPanel42_v3.h. 5 càmeres sense fil (2-6; la 1 va per cable
  i no hi surt), cada una amb UNA freqüència activa, format "CAM<num>.<nom>".
  2 freqüències de backup compartides entre totes les càmeres.

  ORGANITZACIÓ DE CARPETA — OBLIGATORI:
    pantalla_frequencies_crowpanel/
      pantalla_frequencies_crowpanel.ino
      CrowPanel42_v3.h   (substitueix la v2)

  AJUSTOS DE L'ARDUINO IDE (provats a mà en aquesta placa concreta):
    Placa ............... ESP32S3 Dev Module
    USB CDC On Boot ..... Disabled
    Flash Mode .......... QIO 80MHz
    Flash Size .......... 8MB (64Mb)
    Partition Scheme .... Huge APP (3MB No OTA/1MB SPIFFS)
    PSRAM ............... OPI PSRAM
    Upload Speed ........ 115200
  Llibreria: només "Adafruit GFX Library".

  NAVEGACIÓ:
  - Rotary (Amunt/Avall), a la pàgina principal: mou el cursor entre 7
    caselles (5 càmeres + 2 backups).
  - Conf sobre una casella: entra en mode AJUST per a aquella casella —
    Amunt/Avall la puja/baixa de 5 en 5 MHz, Conf ho desa, Exit ho
    cancel·la (torna al valor d'abans). Res de menús ni llistes per canviar
    un sol valor.
  - MENU: obre el menú (Obrir un lloc sencer / Guardar canvis / Settings).
  - EXIT (fora de mode ajust): un pas enrere.

  "Obrir un lloc" (des del menú) carrega LA GRAELLA SENCERA (les 5 càmeres
  + els 2 backups) d'un lloc desat — ja no hi ha una llista per triar un
  sol camp, només per carregar-ho tot de cop.

  JIGA-Cam: peu amb els 8 canals (CH1-CH7, CHP), 4 columnes x 2 files, amb
  la lletra més grossa que hi càpiga de veritat (mesurat amb getTextBounds).
  CH1 i CH2 marcats com a seleccionats.

  NOM vs NÚMERO a la graella principal: el número es queda SEMPRE a 24pt
  (el que ha de llegir-se de lluny); el nom de la càmera es mesura i es
  tria la lletra més grossa que hi càpiga al costat — mai una mida fixa a
  ull, perquè amb "CAM4.Nachete" ja es va desquadrar un cop calculant-ho
  a mà.

  SETTINGS: "Pantalla" inverteix blanc/negre girant tots els bits del
  buffer just abans d'epd.show() — no cal tocar cap funció de dibuix.

  BLE: dues característiques.
  - CHAR_DATA (llegir/escriure): lectura = graella sencera en text (per
    omplir un formulari web de cop); escriptura = format curt "index:valor"
    (0-4=freqüència d'aquella càmera, 5-6=backup) — un camp per escriptura,
    per no dependre de negociar un MTU gran.
  - CHAR_VENUE (només escriure): desa la graella actual com a lloc nou.

  REFRESC: epd.show() tria sol ràpid/net/complet. epd.tasca() (cada volta
  del loop) neteja la imatge fantasma en repòs i adorm el panell. A
  l'arrencada es força un refresc COMPLET per netejar qualsevol fantasma
  d'una versió anterior flashejada abans.

  SON PROFUND (ESP32 sencer): als 60s d'inactivitat es dorm tot el xip,
  desperta amb qualsevol botó. BLE només actiu despert.
*/

// Refresc: menys ràpids seguits i neteja en repòs més aviat (contra la fantasma)
#define CP_MAX_RAPIDS      0        // mai RÀPID: el ràpid deixava requadres fantasma del cursor
#define CP_DEUTE_NETEJA    3        // amb 3 refrescos sense GC ja es fa la neteja
#define CP_NETEJA_MS       4000UL   // 4 s sense tocar res -> neteja complerta
#define CP_SLEEP_MS        10000UL  // ha de ser MÉS GRAN que CP_NETEJA_MS
#include "CrowPanel42_v3.h"   // v3: espera bé el final del refresc (la v2 no esperava)
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeMonoBold24pt7b.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include "driver/rtc_io.h"

CrowPanel42 epd;

// ---- Model de dades: graella actual ----
const int NUM_CAMS = 5;
struct Cam {
  String name;
  String freq;
};
Cam cams[NUM_CAMS];
String backupFreq[2];
Preferences prefs;

// ---- Model de dades: llocs desats ----
const int MAX_VENUES = 25;
const char* DEFAULT_NAMES[5] = {"CAM2.Diego", "CAM3.Merino", "CAM4.Nachete", "CAM5.Viko", "CAM6.Steady"};
struct Venue {
  String name;
  String camName[5];
  String camFreq[5];
  String backupFreq[2];
  uint8_t jigaMask = 0x03;
};
Venue venues[MAX_VENUES];
int venueCount = 0;

// ---- Referència fixa JIGA-Cam (8 canals) — CH1 i CH2 marcats com a en ús ----
const char* JIGA_CH[8]  = {"CH1", "CH2", "CH3", "CH4", "CH5", "CHP", "CH6", "CH7"};
const char* JIGA_MHZ[8] = {"5650", "5685", "5725", "5760", "5795", "5829", "5868", "5904"};
uint8_t jigaMask = 0x03;   // bit i = canal i en ús (per defecte CH1 i CH2)
bool jigaOn(int i) { return (jigaMask >> i) & 1; }
const int NUM_CELLS = 15; // 0-4 càmeres, 5-6 backup, 7-14 canals JIGA-Cam

// ---- Navegació ----
enum UIMode { NAVIGATE, ACTIONMENU, VENUEPICK, SETTINGS };
UIMode uiMode = NAVIGATE;

int cellSel = 0;          // 0-4 = càmera, 5-6 = backup, 7-14 = JIGA-Cam
bool cursorVisible = false; // en repòs NO hi ha requadre; apareix en tocar el rotary
bool adjusting = false;   // dins de NAVIGATE: ajustant cellSel amb Amunt/Avall
String adjustOriginal;    // valor abans d'entrar en ajust, per si es cancel·la
int venueScroll = 0;

const int NUM_MENU_ITEMS = 3;
const char* MENU_LABELS[NUM_MENU_ITEMS] = {"Obrir un lloc", "Guardar canvis", "Settings"};
int menuSel = 0;

bool invertDisplay = false;
String btName = "Frequencies";   // es pot canviar des de la web (efecte en reiniciar)

int loadedVenueIdx = -1;
bool showSavedFlash = false;
bool sleepingMark = false;   // "zz" a la franja superior: dorm, Bluetooth apagat

bool prevMenu = true, prevExit = true, prevUp = true, prevDown = true, prevConf = true;
unsigned long lastActivityMs = 0;
const unsigned long AWAKE_TIMEOUT_MS = 60000;

#define WAKEUP_PINS_MASK ((1ULL << CP_EXIT) | (1ULL << CP_MENU) | \
                          (1ULL << CP_DOWN) | (1ULL << CP_OK) | (1ULL << CP_UP))

// ---- BLE ----
// UUIDs propis d'aquest projecte. Abans eren els del "Nordic UART" (6e4000xx),
// que són genèrics i els comparteixen altres projectes: l'iPhone els confonia.
#define SERVICE_UUID    "7b1e0001-3f2a-4c8d-9b6e-5a2f8c1d4e70"
#define CHAR_DATA_UUID  "7b1e0002-3f2a-4c8d-9b6e-5a2f8c1d4e70"
#define CHAR_VENUE_UUID "7b1e0003-3f2a-4c8d-9b6e-5a2f8c1d4e70"
BLECharacteristic *pDataChar;
BLECharacteristic *pVenueChar;
volatile bool needsRedraw = false;
volatile bool needsFullRedraw = false;
volatile bool bleConnected = false;
volatile int  pendingLoad = -1;       // ordres de la web que es fan des del loop()
volatile int  pendingDel = -1;
volatile bool pendingRestart = false;
volatile bool pendingSaveCur = false;   // SAVE de la web: desa la pantalla al lloc carregat

// Mentre hi ha el mòbil connectat no s'adorm; en desconnectar torna a anunciar-se
// (sense això, després de la PRIMERA connexió el dispositiu ja no es tornava a veure)
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override { bleConnected = true; lastActivityMs = millis(); needsRedraw = true; }
  void onDisconnect(BLEServer *s) override {
    bleConnected = false; lastActivityMs = millis(); needsRedraw = true;
    BLEDevice::startAdvertising();
  }
};

// ---- Casella (0-4 càmera, 5-6 backup): llegir/escriure de forma unificada ----
String getCellValue(int cell) {
  if (cell < NUM_CAMS) return cams[cell].freq;
  return backupFreq[cell - NUM_CAMS];
}
void setCellValue(int cell, const String &val) {
  if (cell < NUM_CAMS) cams[cell].freq = val;
  else backupFreq[cell - NUM_CAMS] = val;
}
void adjustCellValue(int cell, int delta) {
  int v = getCellValue(cell).toInt() + delta;
  if (v < 0) v = 0;
  setCellValue(cell, String(v));
}

// ---- Graella actual: 5 línies "Nom,Freq" + 1 línia "BACKUP,Freq1,Freq2" ----
String gridToText() {
  String out;
  for (int i = 0; i < NUM_CAMS; i++) out += cams[i].name + "," + cams[i].freq + "\n";
  out += "BACKUP," + backupFreq[0] + "," + backupFreq[1] + "\n";
  out += "JIGA," + String(jigaMask) + "\n";
  out += "INV," + String(invertDisplay ? 1 : 0) + "\n";
  out += "BT," + btName + "\n";
  if (loadedVenueIdx >= 0 && loadedVenueIdx < venueCount)
    out += "LLOC," + String(loadedVenueIdx) + "," + venues[loadedVenueIdx].name + "\n";
  else
    out += "LLOC,-1,\n";
  return out;
}

void textToGrid(const String &text) {
  int start = 0;
  for (int i = 0; i < NUM_CAMS; i++) {
    int nl = text.indexOf('\n', start);
    if (nl == -1) return;
    String line = text.substring(start, nl); line.trim();
    int p1 = line.indexOf(',');
    if (p1 > 0) { cams[i].name = line.substring(0, p1); cams[i].freq = line.substring(p1 + 1); }
    start = nl + 1;
  }
  int nl = text.indexOf('\n', start);
  if (nl == -1) nl = text.length();
  String line = text.substring(start, nl); line.trim();
  if (line.startsWith("BACKUP,")) {
    String rest = line.substring(7);
    int p1 = rest.indexOf(',');
    if (p1 > 0) { backupFreq[0] = rest.substring(0, p1); backupFreq[1] = rest.substring(p1 + 1); }
  }
  int j = text.indexOf("JIGA,");   // opcional: dades antigues no el porten
  if (j >= 0) jigaMask = (uint8_t)text.substring(j + 5).toInt();
}

void saveGrid() {
  prefs.begin("freqs", false);
  prefs.putString("grid", gridToText());
  prefs.end();
}

void loadGrid() {
  prefs.begin("freqs", true);
  String txt = prefs.getString("grid", "");
  prefs.end();
  if (txt.length() > 0) {
    textToGrid(txt);
  } else {
    const char* defNames[5] = {"CAM2.Diego", "CAM3.Merino", "CAM4.Nachete", "CAM5.Viko", "CAM6.Steady"};
    const char* defFreq[5]  = {"6060", "5890", "4920", "5940", "4990"};
    for (int i = 0; i < NUM_CAMS; i++) { cams[i].name = defNames[i]; cams[i].freq = defFreq[i]; }
    backupFreq[0] = "6000";
    backupFreq[1] = "6150";
  }
}

void loadSettings() {
  prefs.begin("settings", true);
  invertDisplay = prefs.getBool("invert", false);
  btName = prefs.getString("btname", "Frequencies");
  prefs.end();
}

// El lloc carregat es desa pel NOM (no per posició), com recomanen els apunts
void saveLoadedVenueName() {
  prefs.begin("settings", false);
  prefs.putString("venue", (loadedVenueIdx >= 0 && loadedVenueIdx < venueCount) ? venues[loadedVenueIdx].name : "");
  prefs.end();
}
void restoreLoadedVenue() {
  prefs.begin("settings", true);
  String n = prefs.getString("venue", "");
  prefs.end();
  for (int i = 0; i < venueCount; i++) if (venues[i].name == n) { loadedVenueIdx = i; return; }
}

void saveSettings() {
  prefs.begin("settings", false);
  prefs.putBool("invert", invertDisplay);
  prefs.putString("btname", btName);
  prefs.end();
}

// ---- Llocs desats ----
String venueToText(int v) {
  String out = venues[v].name + "\n";
  for (int c = 0; c < NUM_CAMS; c++) out += venues[v].camName[c] + "," + venues[v].camFreq[c] + "\n";
  out += venues[v].backupFreq[0] + "," + venues[v].backupFreq[1] + "\n";
  out += "J:" + String(venues[v].jigaMask) + "\n";
  return out;
}

String venuesToText() {
  String out;
  for (int v = 0; v < venueCount; v++) {
    out += venues[v].name + "\n";
    for (int c = 0; c < NUM_CAMS; c++) out += venues[v].camName[c] + "," + venues[v].camFreq[c] + "\n";
    out += venues[v].backupFreq[0] + "," + venues[v].backupFreq[1] + "\n";
    out += "J:" + String(venues[v].jigaMask) + "\n";
    out += "###\n";
  }
  return out;
}

void venuesFromText(const String &text) {
  venueCount = 0;
  int start = 0;
  while (start < (int)text.length() && venueCount < MAX_VENUES) {
    int nl = text.indexOf('\n', start);
    if (nl == -1) break;
    String name = text.substring(start, nl);
    start = nl + 1;
    if (name.length() == 0) break;

    Venue &v = venues[venueCount];
    v.name = name;
    bool ok = true;
    for (int c = 0; c < NUM_CAMS; c++) {
      int lnl = text.indexOf('\n', start);
      if (lnl == -1) { ok = false; break; }
      String line = text.substring(start, lnl);
      int cm = line.indexOf(',');
      if (cm > 0) { v.camName[c] = line.substring(0, cm); v.camFreq[c] = line.substring(cm + 1); }
      else { v.camName[c] = DEFAULT_NAMES[c]; v.camFreq[c] = line; }  // dades antigues sense nom
      start = lnl + 1;
    }
    if (ok) {
      int lnl = text.indexOf('\n', start);
      if (lnl == -1) { ok = false; }
      else {
        String bline = text.substring(start, lnl);
        int p1 = bline.indexOf(',');
        if (p1 > 0) { v.backupFreq[0] = bline.substring(0, p1); v.backupFreq[1] = bline.substring(p1 + 1); }
        else ok = false;
        start = lnl + 1;
      }
    }
    v.jigaMask = 0x03;
    if (text.startsWith("J:", start)) {           // opcional (compatible amb dades antigues)
      int jnl = text.indexOf('\n', start);
      v.jigaMask = (uint8_t)text.substring(start + 2, jnl == -1 ? text.length() : jnl).toInt();
      start = (jnl == -1) ? text.length() : jnl + 1;
    }
    int snl = text.indexOf('\n', start);          // salta "###"
    start = (snl == -1) ? text.length() : snl + 1;
    if (ok) venueCount++;
  }
}

void saveVenues() {
  prefs.begin("freqs", false);
  prefs.putString("venues", venuesToText());
  prefs.end();
}

// Afegeix (o no toca) un lloc per nom. Noms en ASCII: la lletra no té accents.
void ensurePreset(const char* name, const char* const names[5], const char* const freqs[5],
                  const char* b1, const char* b2, uint8_t mask) {
  for (int i = 0; i < venueCount; i++) if (venues[i].name == name) return;
  if (venueCount >= MAX_VENUES) return;
  Venue &v = venues[venueCount++];
  v.name = name;
  for (int c = 0; c < NUM_CAMS; c++) { v.camName[c] = names[c]; v.camFreq[c] = freqs[c]; }
  v.backupFreq[0] = b1; v.backupFreq[1] = b2;
  v.jigaMask = mask;
}

void addPresets() {
  int before = venueCount;
  const char* baN[5] = {"CAM2.Diego", "CAM3.Merino", "CAM4.Nachete", "CAM5.Viko", "CAM6.Steady"};
  const char* baF[5] = {"6060", "5890", "4920", "5940", "4990"};
  ensurePreset("Buenos Aires Movistar Arena", baN, baF, "6000", "6150", 0x03);

  const char* scN[5] = {"CAM2.Diego", "CAM3.Merino", "CAM4.Nachete", "CAM5.Viko", "CAM6.Steady"};
  const char* scF[5] = {"5050", "5890", "4920", "5840", "4990"};
  ensurePreset("Santiago de Chile Santander Arena", scN, scF, "6000", "6150", 0x03);

  // Bogota: backups PENDENTS — 6150 es ara la de Diego i 6010 (Adri) queda a 10 MHz de 6000
  const char* boN[5] = {"CAM2.Diego", "CAM3.Jisus", "CAM4.Nachete", "CAM5.Viko", "CAM6.Adri"};
  const char* boF[5] = {"6150", "6100", "5700", "5900", "6010"};
  ensurePreset("Bogota Movistar Arena", boN, boF, "----", "----", 0x03);

  if (venueCount != before) saveVenues();
}

void loadVenues() {
  prefs.begin("freqs", true);
  String txt = prefs.getString("venues", "");
  prefs.end();
  if (txt.length() > 0) venuesFromText(txt);
  bool wasEmpty = (venueCount == 0);
  prefs.begin("settings", true);
  bool presetsDone = prefs.getBool("presets1", false);
  prefs.end();
  if (!presetsDone) {
    addPresets();
    prefs.begin("settings", false); prefs.putBool("presets1", true); prefs.end();
  }
  if (wasEmpty && venueCount > 0) { loadedVenueIdx = 0; saveLoadedVenueName(); }
}

// Escriptures curtes, una per camp (els apunts de l'altre projecte):
//   "0:6060" .. "6:6150"   freqüència (0-4 càmera, 5-6 backup)
//   "7:1" .. "14:0"        canal JIGA-Cam en ús / lliure
//   "N0:CAM2.Diego"        nom de la càmera 0-4
//   "INV:1"                pantalla invertida (Negre/Blanc)
//   "BTNAME:Freqs Moral"   nom del Bluetooth (reinicia el dispositiu per aplicar-lo)
class DataCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String v = String(c->getValue().c_str());
    v.trim();
    if (v.length() == 0) return;
    lastActivityMs = millis();

    if (v.startsWith("BTNAME:")) {
      String n = v.substring(7); n.trim();
      if (n.length() == 0) return;
      if (n.length() > 20) n = n.substring(0, 20);   // cap a la resposta d'escaneig
      btName = n; saveSettings();
      pendingRestart = true;
      return;
    }
    if (v.startsWith("INV:")) {
      invertDisplay = v.substring(4).toInt() != 0;
      saveSettings();
      needsFullRedraw = true;
      return;
    }
    if (v.startsWith("N") && v.length() > 3 && v.charAt(2) == ':') {
      int i = v.charAt(1) - '0';
      if (i < 0 || i >= NUM_CAMS) return;
      String n = v.substring(3); n.trim();
      if (n.length() == 0) return;
      cams[i].name = n;
      saveGrid();
      needsRedraw = true;
      return;
    }

    int colon = v.indexOf(':');
    if (colon <= 0) return;
    int idx = v.substring(0, colon).toInt();
    String val = v.substring(colon + 1);
    val.trim();
    if (idx < 0 || idx >= NUM_CELLS) return;
    if (idx >= 7) {
      int b = idx - 7;
      if (val.toInt()) jigaMask |= (1 << b); else jigaMask &= ~(1 << b);
    } else {
      setCellValue(idx, val);
    }
    saveGrid();
    needsRedraw = true;
  }
};

// Protocol de llocs (una ordre curta per escriptura; les respostes es LLEGEIXEN
// de la mateixa característica just després):
//   "COUNT"          -> llegeix: nombre de llocs
//   "GET:3"          -> llegeix: el lloc 3 en text
//   "LOAD:3"         -> posa el lloc 3 a la pantalla
//   "DEL:3"          -> esborra el lloc 3
//   Editar/crear:  "VB:3" (o "VB:-1" per un de nou), "VN:<nom lloc>",
//                  "VC0:<nom>,<freq>" .. "VC4:...", "VK:<b1>,<b2>", "VJ:<mask>", "VE" (desa)
Venue editBuf;
int editIdx = -1;

class VenueSaveCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String v = String(c->getValue().c_str());
    v.trim();
    if (v.length() == 0) return;
    lastActivityMs = millis();

    if (v == "COUNT") { c->setValue(String(venueCount).c_str()); return; }
    if (v.startsWith("GET:")) {
      int i = v.substring(4).toInt();
      if (i >= 0 && i < venueCount) c->setValue(venueToText(i).c_str());
      else c->setValue("");
      return;
    }
    if (v.startsWith("LOAD:")) { pendingLoad = v.substring(5).toInt(); c->setValue("OK"); return; }
    if (v == "SAVECUR") {
      if (loadedVenueIdx < 0 || loadedVenueIdx >= venueCount) { c->setValue("ERR:cap lloc"); return; }
      pendingSaveCur = true; c->setValue("OK"); return;
    }
    if (v.startsWith("DEL:"))  { pendingDel  = v.substring(4).toInt(); c->setValue("OK"); return; }

    if (v.startsWith("VB:")) {
      editIdx = v.substring(3).toInt();
      if (editIdx >= 0 && editIdx < venueCount) editBuf = venues[editIdx];
      else {
        editIdx = -1;
        editBuf.name = "";
        for (int k = 0; k < NUM_CAMS; k++) { editBuf.camName[k] = cams[k].name; editBuf.camFreq[k] = cams[k].freq; }
        editBuf.backupFreq[0] = backupFreq[0]; editBuf.backupFreq[1] = backupFreq[1];
        editBuf.jigaMask = jigaMask;
      }
      c->setValue("OK"); return;
    }
    if (v.startsWith("VN:")) { editBuf.name = v.substring(3); editBuf.name.trim(); c->setValue("OK"); return; }
    if (v.startsWith("VC") && v.length() > 4 && v.charAt(3) == ':') {
      int k = v.charAt(2) - '0';
      String rest = v.substring(4);
      int cm = rest.indexOf(',');
      if (k >= 0 && k < NUM_CAMS && cm > 0) { editBuf.camName[k] = rest.substring(0, cm); editBuf.camFreq[k] = rest.substring(cm + 1); }
      c->setValue("OK"); return;
    }
    if (v.startsWith("VK:")) {
      String rest = v.substring(3);
      int cm = rest.indexOf(',');
      if (cm >= 0) { editBuf.backupFreq[0] = rest.substring(0, cm); editBuf.backupFreq[1] = rest.substring(cm + 1); }
      c->setValue("OK"); return;
    }
    if (v.startsWith("VJ:")) { editBuf.jigaMask = (uint8_t)v.substring(3).toInt(); c->setValue("OK"); return; }
    if (v == "VE") {
      if (editBuf.name.length() == 0) { c->setValue("ERR:nom"); return; }
      int target = editIdx;
      if (target < 0) {                                   // nou: si el nom ja existeix, el substitueix
        for (int i = 0; i < venueCount; i++) if (venues[i].name == editBuf.name) { target = i; break; }
      }
      if (target < 0) {
        if (venueCount >= MAX_VENUES) { c->setValue("ERR:ple"); return; }
        target = venueCount++;
      }
      venues[target] = editBuf;
      saveVenues();
      if (target == loadedVenueIdx) saveLoadedVenueName();   // per si n'ha canviat el nom
      needsRedraw = true;
      c->setValue(("OK:" + String(target)).c_str());
      return;
    }
  }
};

// ---------------------------------------------------------------------
// DIBUIX
// ---------------------------------------------------------------------

void centeredBox(const String &str, int x, int yBaseline, int padX, int padY) {
  int16_t x1, y1; uint16_t w, h;
  epd.canvas.getTextBounds(str, x, yBaseline, &x1, &y1, &w, &h);
  epd.canvas.drawRect(x1 - padX, y1 - padY, w + 2 * padX, h + 2 * padY, CP_NEGRE);
}
// Cursor d'edició: requadre doble, més gruixut que els de JIGA-Cam
void cursorBox(const String &str, int x, int yBaseline, int padX, int padY) {
  centeredBox(str, x, yBaseline, padX, padY);
  centeredBox(str, x, yBaseline, padX + 1, padY + 1);
}

// Mides fixes, més petites que abans perquè cap requadre toqui el veí:
//   freqüències 18pt, noms/etiquetes 12pt (9pt si no hi caben), JIGA-Cam 9pt.
const int BOX_PX = 4, BOX_PY = 3;
const int JIGA_PADX = 3, JIGA_PADY = 2;

void drawOverviewPage() {
  epd.canvas.fillScreen(CP_BLANC);
  epd.canvas.setTextColor(CP_NEGRE);
  int16_t bx1, by1; uint16_t bw, bh;

  // Franja superior: sempre el nom del lloc
  epd.canvas.setFont(&FreeMonoBold9pt7b);
  int topStrip = 16;
  epd.canvas.setCursor(4, 12);
  if (showSavedFlash) epd.canvas.print("[DESAT!] ");
  else if (adjusting) epd.canvas.print("[AJUST] ");
  if (loadedVenueIdx >= 0 && loadedVenueIdx < venueCount) epd.canvas.print(venues[loadedVenueIdx].name);
  if (!adjusting && !showSavedFlash) {
    epd.canvas.fillRect(378, 0, 22, topStrip, CP_BLANC);   // lloc net per a l'indicador
    if (sleepingMark) {
      epd.canvas.setCursor(378, 12);
      epd.canvas.print("zz");                                // adormit: BT apagat
    } else if (bleConnected) {
      epd.canvas.fillCircle(390, 8, 5, CP_NEGRE);            // connectat: cercle ple
    } else {
      epd.canvas.drawCircle(390, 8, 5, CP_NEGRE);            // es pot connectar: cercle buit
    }
  }
  epd.canvas.drawFastHLine(0, topStrip, 400, CP_NEGRE);

  // --- Peu JIGA-Cam: 4 columnes fixes de 99px, text centrat a cada una ---
  epd.canvas.setFont(&FreeMonoBold9pt7b);
  epd.canvas.getTextBounds("CH1:5650", 0, 0, &bx1, &by1, &bw, &bh);
  int jCapH = -by1;                              // alçada per sobre la línia base
  int jRowH = bh + 2 * JIGA_PADY + 8;            // requadre + 8px d'aire entre files
  int jHeader = 17;
  int footerH = jHeader + 2 * jRowH;
  int fy = 300 - footerH;
  const int jColW = 99;

  // --- Graella: 6 files entre la franja i el peu ---
  int rowH = (300 - topStrip - footerH - 4) / 6;
  int y0 = topStrip + 2;
  int col0 = 4;

  epd.canvas.setFont(&FreeMonoBold18pt7b);
  epd.canvas.getTextBounds("0000", 0, 0, &bx1, &by1, &bw, &bh);
  int fCapH = -by1;
  uint16_t maxFreqW = 0;
  for (int i = 0; i < NUM_CAMS; i++) {
    epd.canvas.getTextBounds(cams[i].freq, 0, 0, &bx1, &by1, &bw, &bh);
    if (bw > maxFreqW) maxFreqW = bw;
  }

  // Nom: 12pt si hi cap amb la freqüència al costat, si no 9pt
  const GFXfont* nameFont = &FreeMonoBold12pt7b;
  uint16_t maxNameW = 0;
  for (int pass = 0; pass < 2; pass++) {
    epd.canvas.setFont(nameFont);
    maxNameW = 0;
    for (int i = 0; i < NUM_CAMS; i++) {
      epd.canvas.getTextBounds(cams[i].name, 0, 0, &bx1, &by1, &bw, &bh);
      if (bw > maxNameW) maxNameW = bw;
    }
    epd.canvas.getTextBounds("Backup:", 0, 0, &bx1, &by1, &bw, &bh);
    if (bw > maxNameW) maxNameW = bw;
    if (col0 + maxNameW + 24 + maxFreqW + BOX_PX <= 396) break;
    nameFont = &FreeMonoBold9pt7b;
  }
  int col1 = col0 + maxNameW + 24;

  for (int i = 0; i < NUM_CAMS; i++) {
    int yBase = y0 + rowH * i + (rowH + fCapH) / 2;  // centrat vertical a la fila
    epd.canvas.setFont(nameFont);
    epd.canvas.setCursor(col0, yBase); epd.canvas.print(cams[i].name);
    epd.canvas.setFont(&FreeMonoBold18pt7b);
    epd.canvas.setCursor(col1, yBase); epd.canvas.print(cams[i].freq);
    if (cursorVisible && cellSel == i) cursorBox(cams[i].freq, col1, yBase, BOX_PX, BOX_PY);
  }

  // Backup: etiqueta sencera + 2 valors, 18pt si hi caben, si no 12pt
  int yB = y0 + rowH * NUM_CAMS + (rowH + fCapH) / 2;
  epd.canvas.setFont(nameFont);
  epd.canvas.setCursor(col0, yB); epd.canvas.print("Backup:");
  const GFXfont* bFont = &FreeMonoBold18pt7b;
  uint16_t w1, w2;
  epd.canvas.setFont(bFont);
  epd.canvas.getTextBounds(backupFreq[0], 0, 0, &bx1, &by1, &w1, &bh);
  epd.canvas.getTextBounds(backupFreq[1], 0, 0, &bx1, &by1, &w2, &bh);
  if (col1 + w1 + 2 * BOX_PX + 14 + w2 + BOX_PX > 396) {
    bFont = &FreeMonoBold12pt7b;
    epd.canvas.setFont(bFont);
    epd.canvas.getTextBounds(backupFreq[0], 0, 0, &bx1, &by1, &w1, &bh);
  }
  int colB1 = col1, colB2 = col1 + w1 + 2 * BOX_PX + 14;
  epd.canvas.setCursor(colB1, yB); epd.canvas.print(backupFreq[0]);
  epd.canvas.setCursor(colB2, yB); epd.canvas.print(backupFreq[1]);
  if (cursorVisible && cellSel == 5) cursorBox(backupFreq[0], colB1, yB, BOX_PX, BOX_PY);
  if (cursorVisible && cellSel == 6) cursorBox(backupFreq[1], colB2, yB, BOX_PX, BOX_PY);

  // --- Peu JIGA-Cam ---
  epd.canvas.drawFastHLine(0, fy, 400, CP_NEGRE);
  epd.canvas.setFont(&FreeMonoBold9pt7b);
  epd.canvas.setCursor(4, fy + 13);
  epd.canvas.print("JIGA-Cam");
  for (int i = 0; i < 8; i++) {
    int col = i % 4, row = i / 4;
    String t = String(JIGA_CH[i]) + ":" + JIGA_MHZ[i];
    epd.canvas.getTextBounds(t, 0, 0, &bx1, &by1, &bw, &bh);
    int x = 2 + col * jColW + (jColW - (int)bw) / 2;       // centrat dins la columna
    int y = fy + jHeader + row * jRowH + JIGA_PADY + jCapH;
    epd.canvas.setCursor(x, y);
    epd.canvas.print(t);
    if (jigaOn(i)) centeredBox(t, x, y, JIGA_PADX, JIGA_PADY);       // en ús: requadre fix
    if (cursorVisible && cellSel == 7 + i)                             // cursor: barra a sota
      epd.canvas.fillRect(x, y + JIGA_PADY + 3, bw, 2, CP_NEGRE);
  }
}

void drawActionMenu() {
  epd.canvas.fillScreen(CP_BLANC);
  epd.canvas.setTextColor(CP_NEGRE);
  epd.canvas.setFont(&FreeMonoBold18pt7b);
  epd.canvas.setCursor(10, 30);
  epd.canvas.print("Menu");
  epd.canvas.drawFastHLine(0, 42, 400, CP_NEGRE);

  epd.canvas.setFont(&FreeMonoBold12pt7b);
  int rowH = 45, y0 = 85;
  for (int i = 0; i < NUM_MENU_ITEMS; i++) {
    int y = y0 + i * rowH;
    if (i == menuSel) {
      epd.canvas.drawRect(8, y - 24, 384, 32, CP_NEGRE);
      epd.canvas.drawRect(9, y - 23, 382, 30, CP_NEGRE); // doble: es veu encara que hi hagi fantasma
    }
    epd.canvas.setCursor(15, y);
    epd.canvas.print(i == menuSel ? "> " : "  ");
    epd.canvas.print(MENU_LABELS[i]);
  }
  epd.canvas.setFont(&FreeMonoBold9pt7b);
  int yFoot = y0 + NUM_MENU_ITEMS * rowH + 15;
  epd.canvas.setCursor(10, yFoot);
  if (loadedVenueIdx >= 0 && loadedVenueIdx < venueCount) {
    epd.canvas.print("Lloc actual:");
    epd.canvas.setCursor(10, yFoot + 20);
    epd.canvas.print(venues[loadedVenueIdx].name);
  } else {
    epd.canvas.print("(Guardar: cap lloc carregat)");
  }
}

void drawSettingsMenu() {
  epd.canvas.fillScreen(CP_BLANC);
  epd.canvas.setTextColor(CP_NEGRE);
  epd.canvas.setFont(&FreeMonoBold18pt7b);
  epd.canvas.setCursor(10, 30);
  epd.canvas.print("Settings");
  epd.canvas.drawFastHLine(0, 42, 400, CP_NEGRE);

  epd.canvas.setFont(&FreeMonoBold12pt7b);
  epd.canvas.drawRect(8, 61, 384, 32, CP_NEGRE);
  epd.canvas.setCursor(15, 85);
  epd.canvas.print("Pantalla: ");
  epd.canvas.print(invertDisplay ? "Negre/Blanc" : "Blanc/Negre");

  epd.canvas.setFont(&FreeMonoBold9pt7b);
  epd.canvas.setCursor(10, 140);
  epd.canvas.print("Conf per canviar");
}

// Ja només serveix per carregar UN LLOC SENCER (Obrir un lloc, des del menú).
void drawVenuePage(int scroll) {
  epd.canvas.fillScreen(CP_BLANC);
  epd.canvas.setTextColor(CP_NEGRE);
  epd.canvas.setFont(&FreeMonoBold12pt7b);
  epd.canvas.setCursor(10, 25);
  epd.canvas.print("Carrega un lloc");
  epd.canvas.drawFastHLine(0, 35, 400, CP_NEGRE);

  epd.canvas.setFont(&FreeMonoBold9pt7b);
  if (venueCount == 0) {
    epd.canvas.setCursor(10, 70);
    epd.canvas.print("(cap lloc desat - desa'n un per BLE)");
  } else {
    int rowH = 40, y0 = 65;
    for (int r = 0; r < 5; r++) {
      int i = (scroll - 2 + r + venueCount * 10) % venueCount;
      int y = y0 + r * rowH;
      if (r == 2) epd.canvas.drawRect(6, y - 22, 388, 30, CP_NEGRE);
      epd.canvas.setCursor(15, y);
      epd.canvas.print(venues[i].name);
    }
  }
}

void drawCurrentPage(bool forceComplete = false) {
  if (uiMode == NAVIGATE) drawOverviewPage();
  else if (uiMode == ACTIONMENU) drawActionMenu();
  else if (uiMode == SETTINGS) drawSettingsMenu();
  else drawVenuePage(venueScroll); // VENUEPICK

  if (invertDisplay) {
    uint8_t* buf = epd.canvas.getBuffer();
    for (int i = 0; i < CP_BYTES; i++) buf[i] = ~buf[i];
  }
  epd.show(forceComplete);
}

void applySelectedVenue() {
  Venue &v = venues[venueScroll];
  for (int c = 0; c < NUM_CAMS; c++) { cams[c].name = v.camName[c]; cams[c].freq = v.camFreq[c]; }
  backupFreq[0] = v.backupFreq[0];
  backupFreq[1] = v.backupFreq[1];
  jigaMask = v.jigaMask;
  loadedVenueIdx = venueScroll;
  saveLoadedVenueName();
  saveGrid();
}

void saveCurrentGridToLoadedVenue() {
  if (loadedVenueIdx < 0 || loadedVenueIdx >= venueCount) return;
  for (int c = 0; c < NUM_CAMS; c++) { venues[loadedVenueIdx].camName[c] = cams[c].name; venues[loadedVenueIdx].camFreq[c] = cams[c].freq; }
  venues[loadedVenueIdx].backupFreq[0] = backupFreq[0];
  venues[loadedVenueIdx].backupFreq[1] = backupFreq[1];
  venues[loadedVenueIdx].jigaMask = jigaMask;
  saveVenues();
  showSavedFlash = true;
}

void activateMenuItem() {
  if (menuSel == 0) {
    uiMode = VENUEPICK;
    venueScroll = (loadedVenueIdx >= 0) ? loadedVenueIdx : 0;
  } else if (menuSel == 1) {
    saveCurrentGridToLoadedVenue();
    uiMode = NAVIGATE;
  } else {
    uiMode = SETTINGS;
  }
}

void goToSleep() {
  sleepingMark = true;
  bool wasBusy = (uiMode != NAVIGATE || adjusting || cursorVisible);
  if (adjusting) setCellValue(cellSel, adjustOriginal);
  adjusting = false; cursorVisible = false; uiMode = NAVIGATE;
  drawCurrentPage(wasBusy); // sempre es redibuixa: "zz" indica que el Bluetooth està apagat
  epd.sleep();
  BLEDevice::deinit(true);
  rtc_gpio_pullup_en((gpio_num_t)CP_MENU); rtc_gpio_pulldown_dis((gpio_num_t)CP_MENU);
  rtc_gpio_pullup_en((gpio_num_t)CP_EXIT); rtc_gpio_pulldown_dis((gpio_num_t)CP_EXIT);
  rtc_gpio_pullup_en((gpio_num_t)CP_UP);   rtc_gpio_pulldown_dis((gpio_num_t)CP_UP);
  rtc_gpio_pullup_en((gpio_num_t)CP_DOWN); rtc_gpio_pulldown_dis((gpio_num_t)CP_DOWN);
  rtc_gpio_pullup_en((gpio_num_t)CP_OK);   rtc_gpio_pulldown_dis((gpio_num_t)CP_OK);
  esp_sleep_enable_ext1_wakeup(WAKEUP_PINS_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

// OK del rotary gestionat a mà: pulsació CURTA = Conf (en deixar-lo anar),
// pulsació LLARGA (0,8 s) = EXIT. Així EXIT funciona encara que el botó
// físic EXIT no es llegeixi.
bool okDown = false, okLongDone = false;
unsigned long okPressT = 0, okChangeT = 0;
int lastExitRaw = -1;

void checkButtons() {
  bool menuNow = epd.premut(CP_MENU);
  bool exitBtn = epd.premut(CP_EXIT);
  bool upNow   = epd.premut(CP_UP);
  bool downNow = epd.premut(CP_DOWN);

  bool confNow = false, exitLong = false;
  bool okRaw = (digitalRead(CP_OK) == LOW);
  unsigned long t = millis();
  if (okRaw && !okDown && t - okChangeT > CP_DEBOUNCE_MS) {
    okDown = true; okLongDone = false; okPressT = t; okChangeT = t;
  } else if (okRaw && okDown && !okLongDone && t - okPressT > 800) {
    okLongDone = true; exitLong = true;
  } else if (!okRaw && okDown && t - okChangeT > CP_DEBOUNCE_MS) {
    okDown = false; okChangeT = t;
    if (!okLongDone) confNow = true;
  }
  bool exitNow = exitBtn || exitLong;

  // Diagnòstic: què llegeix el pin del botó EXIT (IO1). Obre el Serial Monitor.
  int exitRaw = digitalRead(CP_EXIT);
  if (exitRaw != lastExitRaw) {
    Serial.printf("[EXIT IO%d] %s\n", CP_EXIT, exitRaw == LOW ? "PREMUT" : "amunt");
    lastExitRaw = exitRaw;
  }
  bool acted = false;
  bool cleanRefresh = false;
  UIMode modeBefore = uiMode;

  if (menuNow) {
    if (uiMode == NAVIGATE && !adjusting) { uiMode = ACTIONMENU; menuSel = 0; acted = true; }
    else if (uiMode == ACTIONMENU) { menuSel = (menuSel + 1) % NUM_MENU_ITEMS; acted = true; }
  }

  if (exitNow) {
    if (uiMode == NAVIGATE) {
      // Sempre: surt de l'ajust, treu cursors (no els de JIGA-Cam) i refresc complet
      if (adjusting) setCellValue(cellSel, adjustOriginal); // no confirmat amb Conf: es descarta
      adjusting = false;
      cursorVisible = false;
      cleanRefresh = true;
      acted = true;
    } else if (uiMode == ACTIONMENU) { uiMode = NAVIGATE; acted = true; }
    else if (uiMode == SETTINGS) { uiMode = ACTIONMENU; acted = true; }
    else if (uiMode == VENUEPICK) { uiMode = NAVIGATE; acted = true; }
  }

  if (upNow) {
    if (uiMode == NAVIGATE) {
      if (adjusting) adjustCellValue(cellSel, 5);
      else if (!cursorVisible) cursorVisible = true;          // 1a pulsació: només mostra el cursor
      else cellSel = (cellSel + NUM_CELLS - 1) % NUM_CELLS;
      acted = true;
    } else if (uiMode == ACTIONMENU) { menuSel = (menuSel - 1 + NUM_MENU_ITEMS) % NUM_MENU_ITEMS; acted = true; }
    else if (uiMode == VENUEPICK && venueCount > 0) { venueScroll = (venueScroll - 1 + venueCount) % venueCount; acted = true; }
  }
  if (downNow) {
    if (uiMode == NAVIGATE) {
      if (adjusting) adjustCellValue(cellSel, -5);
      else if (!cursorVisible) cursorVisible = true;
      else cellSel = (cellSel + 1) % NUM_CELLS;
      acted = true;
    } else if (uiMode == ACTIONMENU) { menuSel = (menuSel + 1) % NUM_MENU_ITEMS; acted = true; }
    else if (uiMode == VENUEPICK && venueCount > 0) { venueScroll = (venueScroll + 1) % venueCount; acted = true; }
  }

  if (confNow) {
    if (uiMode == NAVIGATE) {
      if (adjusting) { saveGrid(); adjusting = false; }                    // desa el valor
      else if (!cursorVisible) cursorVisible = true;
      else if (cellSel >= 7) { jigaMask ^= (1 << (cellSel - 7)); saveGrid(); } // JIGA: marca/desmarca
      else { adjusting = true; adjustOriginal = getCellValue(cellSel); }   // freqüència: entra en ajust
      acted = true;
    } else if (uiMode == ACTIONMENU) { activateMenuItem(); acted = true; }
    else if (uiMode == SETTINGS) { invertDisplay = !invertDisplay; saveSettings(); cleanRefresh = true; acted = true; }
    else if (uiMode == VENUEPICK && venueCount > 0) { applySelectedVenue(); uiMode = NAVIGATE; acted = true; }
  }

  if (menuNow || exitNow || upNow || downNow || confNow) {
    Serial.printf("[BTN] menu=%d exit=%d up=%d down=%d ok=%d -> mode=%d menuSel=%d cell=%d ajust=%d\n",
                  menuNow, exitNow, upNow, downNow, confNow, (int)uiMode, menuSel, cellSel, adjusting);
  }

  if (acted) {
    // Canvi de pàgina (menú, settings, llista) = refresc complet: és on més fantasma quedava
    bool full = cleanRefresh || (uiMode != modeBefore);
    drawCurrentPage(full);
    if (exitNow && uiMode == NAVIGATE) epd.sleep();   // imatge neta i quieta
    showSavedFlash = false;
    lastActivityMs = millis();
  }
}

void setup() {
  Serial.begin(115200);

  loadSettings();
  loadGrid();
  loadVenues();
  restoreLoadedVenue(); // sense això, el nom del lloc desapareixia en reiniciar
  lastActivityMs = millis();

  epd.begin();
  epd.canvas.setTextWrap(false);
  drawCurrentPage(true); // complet a l'arrencada: neteja qualsevol fantasma anterior

  BLEDevice::init(btName.c_str());
  BLEDevice::setMTU(247);

  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);

  pDataChar = pService->createCharacteristic(
                CHAR_DATA_UUID,
                BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
              );
  pDataChar->setValue(gridToText().c_str());
  pDataChar->setCallbacks(new DataCallback());

  pVenueChar = pService->createCharacteristic(
                 CHAR_VENUE_UUID,
                 BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
               );
  pVenueChar->setCallbacks(new VenueSaveCallback());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  // UUID de 128 bits + nom no caben junts als 31 bytes de l'anunci: el nom
  // va a la resposta d'escaneig. Sense això l'anunci pot no sortir.
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinInterval(160);
  pAdvertising->setMaxInterval(320);
  BLEDevice::startAdvertising();
}

void loop() {
  checkButtons();

  if (pendingRestart) {                       // nom del Bluetooth nou: cal reiniciar
    delay(300);                               // que la web rebi la confirmació
    ESP.restart();
  }
  if (pendingSaveCur) {
    pendingSaveCur = false;
    saveCurrentGridToLoadedVenue();   // mostra [DESAT!] a la pantalla
    needsRedraw = true;
  }
  if (pendingLoad >= 0) {
    int i = pendingLoad; pendingLoad = -1;
    if (i < venueCount) { venueScroll = i; applySelectedVenue(); uiMode = NAVIGATE; needsFullRedraw = true; }
  }
  if (pendingDel >= 0) {
    int i = pendingDel; pendingDel = -1;
    if (i < venueCount) {
      for (int k = i; k < venueCount - 1; k++) venues[k] = venues[k + 1];
      venueCount--;
      if (loadedVenueIdx == i) loadedVenueIdx = -1;
      else if (loadedVenueIdx > i) loadedVenueIdx--;
      saveVenues();
      saveLoadedVenueName();
      needsRedraw = true;
    }
  }

  if (needsRedraw || needsFullRedraw) {
    pDataChar->setValue(gridToText().c_str());
    drawCurrentPage(needsFullRedraw);
    needsRedraw = false;
    needsFullRedraw = false;
  }
  epd.tasca();
  if (!bleConnected && millis() - lastActivityMs > AWAKE_TIMEOUT_MS) goToSleep();
  delay(5);
}

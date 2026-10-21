/*
  ============================================================================
  CrowPanel42_v2.h — CrowPanel 4,2" (ESP32-S3), revisio de l'ADHESIU VERD
  Versio 2: tres waveforms en lloc de dues, i el flaix fora de la interaccio.
  ============================================================================

  QUE CANVIA RESPECTE DE CrowPanel42.h
  ------------------------------------
  La classe es diu igual (CrowPanel42) i l'API publica es la mateixa, o sigui
  que per provar-ho nomes cal canviar l'#include del .ino. Per tornar enrere,
  canvia'l una altra vegada.

  1) TERCER WAVEFORM "NET" (lutNet). El flaix blanc-negre-negre-blanc el fan
     les taules WW (0x21) i KK (0x24) del GC: obliguen els pixels que NO
     canvien a anar a l'altre extrem i tornar. La taula NET agafa les
     transicions fortes del GC (KW i WK, de dues etapes, que son les que
     maten la imatge fantasma) i hi posa les taules WW i KK del DU, que
     deixen quiets els pixels que no canvien. Resultat: neteja gairebe de
     GC, sense cap flaix. Nomes combina taules teves que ja funcionen; no hi
     ha cap valor inventat.

  2) EL LLINDAR JA NO DISPARA UN GC. Abans, canviar mes d'un 3% dels pixels
     forcava refresc complet. Un numero gran amb setTextSize(6) ja passa del
     3%, o sigui que gairebe cada pulsacio flaixejava. Ara un canvi gran
     dispara un NET, i el llindar puja al 25%.

  3) EL FLAIX ES FA EN REPOS. Quan porta CP_DEUTE_NETEJA refrescos sense GC i
     fa CP_NETEJA_MS que no toques res, tasca() fa el GC i despres adorm la
     pantalla. El flaix continua existint, pero el veus un sol cop quan ja
     has acabat, no enmig de la navegacio. Si prefereixes no veure'l mai,
     posa CP_NETEJA_EN_REPOS a 0 i fes el GC tu quan canviis de pagina.

  4) S'ESCRIU EL BUFFER ANTIC (registre 0x10). El controlador tria quina
     taula aplica a cada pixel (WW / WK / KW / KK) comparant 0x10 amb 0x13.
     La v1 nomes escrivia 0x13, o sigui que el "buffer antic" del controlador
     es quedava tal com l'havia deixat clear(): tot blanc. Amb aixo, cada
     pixel negre es tractava com una transicio blanc->negre a cada refresc,
     encara que no hagues canviat. Ara s'hi escriu anterior[], que ja el
     teniem a la RAM. Es el canvi que mes hauria de notar-se en la imatge
     fantasma. (Si el controlador ja copiava 0x13 a 0x10 tot sol, escriure-hi
     el mateix no fa cap mal: costa uns 12 ms de SPI.)

  5) EL memcpy A anterior[] ES FA DESPRES DEL REFRESC, no abans. Amb el punt
     4 cal la imatge vella encara viva mentre s'envia.

  6) DESPERTAR I ESPERAR BUSY. Despres de sleep() el controlador perd els
     buffers, o sigui que el primer refresc despres de despertar-se es NET i
     torna a enviar anterior[] a 0x10. I es dona 10 ms perque BUSY s'activi
     abans de vigilar-lo: si no, esperaBusy() podia sortir de seguida creient
     que ja havia acabat, i la seguent escriptura SPI queia sobre un refresc
     en marxa (les "taques aleatories").

  PENDENT DE PROVAR A LA PLACA
  ---------------------------
  El punt 1 i el punt 4 no els he pogut provar amb maquinari. Si el NET deixa
  mes fantasma del que esperes, puja CP_MAX_RAPIDS avall (mes NET, menys DU)
  o baixa CP_DEUTE_NETEJA (GC mes sovint). Si el NET surt amb poc contrast,
  el problema es que les transicions necessiten mes fotogrames i llavors si
  que cal tocar les taules.

  La resta del fitxer (pins, botons, SPI, init, lutGC, lutDU) es identica.
  Credit dels registres i les taules GC/DU: codi d'exemple d'Elecrow, carpeta
  "arduino_A_green_circular_sticker_on_the_back".
  ============================================================================
*/
#ifndef CROWPANEL42_V2_H
#define CROWPANEL42_V2_H

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>

// ---------------------------------------------------------------- pins fixos
#define CP_SCK    12
#define CP_MOSI   11
#define CP_RES    47
#define CP_DC     46
#define CP_CS     45
#define CP_BUSY   48
#define CP_PWR     7
#define CP_PWR2   41     // NOMES a la revisio de l'adhesiu verd

// Botons de la placa (tots actius en baix)
#define CP_DOWN    4
#define CP_OK      5
#define CP_UP      6
#define CP_MENU    2
#define CP_EXIT    1

// Pins lliures a la capçalera. Els de "strapping" els llegeix l'ESP32-S3 en
// arrencar: no hi pengis un LED o pot arrencar malament.
//   segurs:      IO8, IO15, IO16, IO17, IO18, IO21
//   strapping:   IO3, IO9, IO14      (evita'ls)
//   USB natiu:   IO19, IO20          (evita'ls si vols conservar el port)

#define CP_W 400
#define CP_H 300
#define CP_BYTES 15000          // 400*300/8

#define CP_BLANC 1              // al canvas, 1 = blanc
#define CP_NEGRE 0              // ... i 0 = negre

// Si la pantalla fa coses rares (taques aleatories que un refresc arregla),
// abaixa primer aixo a 2000000UL; si no, posa CP_HW_SPI a 0.
#ifndef CP_SPI_HZ
  #define CP_SPI_HZ 10000000UL
#endif
#ifndef CP_HW_SPI
  #define CP_HW_SPI 1
#endif

// --------------------------------------------------- politica de refrescos
// RAPID (DU)   : ~300 ms, sense flaix, deixa fantasma acumulada
// NET          : ~1 s,    sense flaix, transicions fortes: treu la fantasma
// COMPLET (GC) : ~3 s,    AMB flaix,   deixa el panell com nou

// Quants RAPID seguits abans de posar-hi un NET.
#ifndef CP_MAX_RAPIDS
  #define CP_MAX_RAPIDS 10
#endif
// Percentatge de pixels canviats que ja demana un NET en lloc d'un RAPID.
#ifndef CP_LLINDAR_PC
  #define CP_LLINDAR_PC 25
#endif
// 1 = fer el COMPLET (amb flaix) quan fa estona que no toques res.
#ifndef CP_NETEJA_EN_REPOS
  #define CP_NETEJA_EN_REPOS 1
#endif
// Refrescos sense COMPLET que s'han d'acumular perque en repos valgui la pena.
#ifndef CP_DEUTE_NETEJA
  #define CP_DEUTE_NETEJA 12
#endif
// Temps sense tocar res abans de fer la neteja en repos (< CP_SLEEP_MS).
#ifndef CP_NETEJA_MS
  #define CP_NETEJA_MS 2500
#endif
#ifndef CP_SLEEP_MS
  #define CP_SLEEP_MS 6000
#endif
#ifndef CP_DEBOUNCE_MS
  #define CP_DEBOUNCE_MS 180
#endif
#ifndef CP_LONG_MS
  #define CP_LONG_MS 800
#endif

class CrowPanel42 {
public:
  enum Refresc { CP_RAPID = 0, CP_NET = 1, CP_COMPLET = 2 };

  GFXcanvas1 canvas;

  CrowPanel42() : canvas(CP_W, CP_H), spi(FSPI) {}

  // ---- engegada ----------------------------------------------------------
  void begin(bool esborra = true) {
    pinMode(CP_PWR2, OUTPUT); digitalWrite(CP_PWR2, HIGH);   // clau a la rev. verda
    pinMode(CP_PWR,  OUTPUT); digitalWrite(CP_PWR,  HIGH);
    pinMode(CP_RES, OUTPUT); pinMode(CP_DC, OUTPUT); pinMode(CP_CS, OUTPUT);
    digitalWrite(CP_CS, HIGH);
    pinMode(CP_BUSY, INPUT);
#if CP_HW_SPI
    spi.begin(CP_SCK, -1, CP_MOSI, -1);
#else
    pinMode(CP_SCK, OUTPUT); pinMode(CP_MOSI, OUTPUT);
#endif
    for (int i = 0; i < 5; i++) { pinMode(pinsBotons[i], INPUT_PULLUP); botoEstat[i] = HIGH; }

    if (esborra) { clear(); }
    else { reset(); init(); despert = true; hiHaAnterior = false; }
    canvas.fillScreen(CP_BLANC);
  }

  // ---- dibuix ------------------------------------------------------------
  // show()      -> tria sol entre RAPID i NET. Mai flaixeja.
  // show(true)  -> forca el COMPLET (net del tot, flaixeja, ~3 s)
  void show(bool complet = false) {
    desperta();

    uint32_t canviats = diferencia();
    uint32_t llindar  = (uint32_t)(CP_W * CP_H) * CP_LLINDAR_PC / 100;

    Refresc mode;
    if (complet || !hiHaAnterior)                                 mode = CP_COMPLET;
    else if (forcaNet || canviats > llindar ||
             rapidsSeguits >= CP_MAX_RAPIDS)                      mode = CP_NET;
    else                                                          mode = CP_RAPID;
    forcaNet = false;

    if (mode == CP_RAPID) rapidsSeguits++; else rapidsSeguits = 0;
    if (mode == CP_COMPLET) deute = 0;     else deute++;

    unsigned long t0 = millis();

    escriu(0x50); dada(0xD7);
    // Buffer antic: el controlador el compara amb el nou per triar, pixel a
    // pixel, quina taula li toca (WW / WK / KW / KK). Sense aixo el DU
    // treballa contra una imatge que no es la que hi ha realment.
    if (hiHaAnterior) { escriu(0x10); bloc(anterior, CP_BYTES); }
    escriu(0x13); bloc(canvas.getBuffer(), CP_BYTES);

    switch (mode) {
      case CP_RAPID: lutDU();  break;
      case CP_NET:   lutNet(); break;
      default:       lutGC();  break;
    }

    int idle = digitalRead(CP_BUSY);   // nivell de "lliure" ABANS d'engegar el refresc
    escriu(0x17); dada(0xA5);
    esperaFi(idle, mode == CP_COMPLET ? 3500 : mode == CP_NET ? 1500 : 500);

    memcpy(anterior, canvas.getBuffer(), CP_BYTES);
    hiHaAnterior = true;
    ultimRefresc = millis();

    Serial.print(F("[EPD] "));
    Serial.print(mode == CP_RAPID ? F("rapid") : mode == CP_NET ? F("net") : F("complet"));
    Serial.print(F(" - ")); Serial.print(canviats); Serial.print(F(" px - "));
    Serial.print(millis() - t0); Serial.print(F(" ms - BUSY: ")); Serial.println(busyInfo);
  }

  // Pantalla tota blanca, amb neteja profunda
  void clear() {
    static uint8_t blanc[250];
    memset(blanc, 0xFF, sizeof(blanc));
    reset(); init();
    escriu(0x10); for (int k = 0; k < 60; k++) bloc(blanc, sizeof(blanc));
    escriu(0x13); for (int k = 0; k < 60; k++) bloc(blanc, sizeof(blanc));
    lutGC();
    { int idle = digitalRead(CP_BUSY); escriu(0x17); dada(0xA5); esperaFi(idle, 3500); }
    despert = true; ultimRefresc = millis();
    // El panell es blanc i ho sabem: guardem-ho com a imatge anterior, aixi
    // el primer dibuix pot ser NET i no cal flaixejar-lo.
    memset(anterior, 0xFF, CP_BYTES);
    hiHaAnterior = true;
    rapidsSeguits = 0; deute = 0; forcaNet = true;
    canvas.fillScreen(CP_BLANC);
  }

  void sleep() {
    if (!despert) return;
    // show() ja espera el final del refresc, així que aquí el panell ja està lliure
    escriu(0x07); dada(0xA5); delay(50);
    despert = false;
  }

  // ---- botons ------------------------------------------------------------
  // premut(pin): true un sol cop per pulsacio, amb antirebot.
  // Crida'l SEMPRE fora d'un "||": si no, pot no arribar a executar-se i la
  // pulsacio es perd.
  bool premut(uint8_t pin) {
    int i = idx(pin); if (i < 0) return false;
    bool ara = digitalRead(pin);
    bool disparat = false;
    if (ara != botoEstat[i] && millis() - botoCanvi[i] > CP_DEBOUNCE_MS) {
      botoCanvi[i] = millis();
      if (ara == LOW) disparat = true;
      botoEstat[i] = ara;
    }
    return disparat;
  }

  // Pulsacio llarga d'un boto (per exemple un menu secundari)
  bool premutLlarg(uint8_t pin) {
    int i = idx(pin); if (i < 0) return false;
    bool ara = (digitalRead(pin) == LOW);
    bool disparat = false;
    if (ara && !llargAvall[i]) { llargAvall[i] = true; llargDes[i] = millis(); llargFet[i] = false; }
    else if (ara && llargAvall[i] && !llargFet[i] && millis() - llargDes[i] > CP_LONG_MS) {
      llargFet[i] = true; disparat = true;
    } else if (!ara && llargAvall[i]) llargAvall[i] = false;
    return disparat;
  }

  // ---- manteniment -------------------------------------------------------
  // Al final del loop(). Fa dues coses: la neteja amb flaix quan fa estona
  // que no toques res, i despres adormir la pantalla per estalviar bateria.
  void tasca() {
    if (!despert) return;
    unsigned long inactiu = millis() - ultimRefresc;
#if CP_NETEJA_EN_REPOS
    if (deute >= CP_DEUTE_NETEJA && inactiu > CP_NETEJA_MS) {
      show(true);            // el flaix passa aqui, quan ja no estas navegant
      sleep();               // ja no hi ha res a fer: a dormir de seguida
      return;
    }
#endif
    if (inactiu > CP_SLEEP_MS) sleep();
  }

  bool estaDespert() const { return despert; }
  uint16_t deuteNeteja() const { return deute; }

private:
  SPIClass spi;
  bool despert = false;
  bool forcaNet = false;
  unsigned long ultimRefresc = 0;
  uint8_t rapidsSeguits = 0;
  uint16_t deute = 0;
  uint8_t anterior[CP_BYTES];
  bool hiHaAnterior = false;

  const uint8_t pinsBotons[5] = {CP_DOWN, CP_OK, CP_UP, CP_MENU, CP_EXIT};
  bool botoEstat[5]; unsigned long botoCanvi[5] = {0,0,0,0,0};
  bool llargAvall[5] = {0,0,0,0,0}; unsigned long llargDes[5] = {0,0,0,0,0};
  bool llargFet[5] = {0,0,0,0,0};

  int idx(uint8_t pin) {
    for (int i = 0; i < 5; i++) if (pinsBotons[i] == pin) return i;
    return -1;
  }

  uint32_t diferencia() {
    if (!hiHaAnterior) return 0xFFFFFFFF;
    const uint8_t* b = canvas.getBuffer();
    uint32_t c = 0;
    for (size_t i = 0; i < CP_BYTES; i++) c += __builtin_popcount((unsigned)(b[i] ^ anterior[i]));
    return c;
  }

  void byteOut(uint8_t d) {
#if CP_HW_SPI
    spi.beginTransaction(SPISettings(CP_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(CP_CS, LOW); spi.transfer(d); digitalWrite(CP_CS, HIGH);
    spi.endTransaction();
#else
    digitalWrite(CP_CS, LOW);
    for (uint8_t i = 0; i < 8; i++) {
      digitalWrite(CP_SCK, LOW);
      digitalWrite(CP_MOSI, (d & 0x80) ? HIGH : LOW);
      digitalWrite(CP_SCK, HIGH);
      d <<= 1;
    }
    digitalWrite(CP_CS, HIGH);
#endif
  }
  void escriu(uint8_t reg) { digitalWrite(CP_DC, LOW);  byteOut(reg); digitalWrite(CP_DC, HIGH); }
  void dada(uint8_t d)     { digitalWrite(CP_DC, HIGH); byteOut(d); }

  void bloc(const uint8_t* buf, size_t n) {
#if CP_HW_SPI
    digitalWrite(CP_DC, HIGH);
    spi.beginTransaction(SPISettings(CP_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(CP_CS, LOW); spi.writeBytes(buf, n); digitalWrite(CP_CS, HIGH);
    spi.endTransaction();
#else
    for (size_t i = 0; i < n; i++) dada(buf[i]);
#endif
  }

  // v3: espera el FINAL del refresc sense suposar la polaritat de BUSY.
  // A la v2, esperaBusy() esperava mentre BUSY==1; amb aquest controlador el
  // refresc "complet" tornava en 38 ms, o sigui que no esperava res. Qualsevol
  // ordre que arribés just després (sobretot sleep()) tallava el refresc a mig
  // fer: la pantalla no canviava i quedaven restes.
  //  1) mira en quin nivell està BUSY abans d'engegar (= lliure);
  //  2) espera fins a 200 ms que canviï (= ocupat) i després que torni;
  //  3) si no canvia mai, espera el temps típic del refresc per no trepitjar-lo.
  const char* busyInfo = "-";
  void esperaFi(int idle, unsigned long reservaMs) {
    unsigned long t0 = millis();
    bool vistOcupat = false;
    while (millis() - t0 < 200) {
      if (digitalRead(CP_BUSY) != idle) { vistOcupat = true; break; }
      delay(1);
    }
    if (vistOcupat) {
      while (digitalRead(CP_BUSY) != idle) {
        delay(1);
        if (millis() - t0 > 15000) { Serial.println(F("[EPD] BUSY no torna: revisio de placa?")); break; }
      }
      busyInfo = (idle == 1) ? "actiu en BAIX" : "actiu en ALT";
    } else {
      unsigned long dt = millis() - t0;
      if (dt < reservaMs) delay(reservaMs - dt);
      busyInfo = "sense senyal, espera fixa";
    }
  }

  void esperaBusy() {
    unsigned long t0 = millis();
    while (digitalRead(CP_BUSY) == 1) {
      delay(1);
      if (millis() - t0 > 15000) { Serial.println(F("[EPD] BUSY no baixa: driver o revisio equivocats?")); return; }
    }
  }

  void reset() {
    digitalWrite(CP_RES, HIGH); delay(10);
    digitalWrite(CP_RES, LOW);  delay(100);
    digitalWrite(CP_RES, HIGH); delay(100);
  }

  void init() {
    escriu(0x00); dada(0x3F); dada(0x4D);
    escriu(0x01); dada(0x03); dada(0x10); dada(0x3F); dada(0x3F); dada(0x03);
    escriu(0x06); dada(0x96); dada(0x96); dada(0x29);
    escriu(0x30); dada(0x09);
    escriu(0x61); dada(0x01); dada(0x90); dada(0x01); dada(0x2C);   // 400 x 300
    escriu(0x82); dada(0x05);
    escriu(0x50); dada(0x97);
    escriu(0x60); dada(0x22);
    escriu(0xE3); dada(0x88);
  }

  void desperta() {
    if (despert) return;
    reset(); delay(20); init(); delay(50);
    despert = true;
    // El controlador ha perdut els seus buffers. anterior[] segueix sent
    // valid (es el que es veu al panell) i el tornarem a enviar a 0x10,
    // pero el primer refresc el fem NET per seguretat.
    forcaNet = true;
  }

  void taula(uint8_t reg, const uint8_t* t) {
    escriu(reg); for (int i = 0; i < 42; i++) dada(t[i]);
  }

  // GC: neteja total. Les taules WW (0x21) i KK (0x24) mouen tambe els
  // pixels que no canvien: aixo es exactament el flaix.
  void lutGC() {
    static const uint8_t a[42]={0x01,0x14,0x0A,0x14,0x00,0x01,0x01};
    static const uint8_t b[42]={0x01,0x54,0x0A,0x94,0x00,0x01,0x01};
    static const uint8_t c[42]={0x01,0x54,0x0A,0x94,0x00,0x01,0x01};
    static const uint8_t d[42]={0x01,0x94,0x0A,0x54,0x00,0x01,0x01};
    static const uint8_t e[42]={0x01,0x94,0x0A,0x54,0x00,0x01,0x01};
    taula(0x20,a); taula(0x21,b); taula(0x22,c); taula(0x23,d); taula(0x24,e);
  }

  // DU: rapid. Una sola etapa a les transicions; els estatics, quiets.
  void lutDU() {
    static const uint8_t a[42]={0x01,0x14,0x00,0x00,0x00,0x01,0x00};
    static const uint8_t b[42]={0x01,0x14,0x00,0x00,0x00,0x01,0x00};
    static const uint8_t c[42]={0x01,0x94,0x00,0x00,0x00,0x01,0x00};
    static const uint8_t d[42]={0x01,0x54,0x00,0x00,0x00,0x01,0x00};
    static const uint8_t e[42]={0x01,0x14,0x00,0x00,0x00,0x01,0x00};
    taula(0x20,a); taula(0x21,b); taula(0x22,c); taula(0x23,d); taula(0x24,e);
  }

  // NET: transicions de dues etapes com el GC (0x22 i 0x23, les que treuen
  // la imatge fantasma) amb els estatics quiets com al DU (0x21 i 0x24).
  // El comu (0x20) es el del GC perque ha de cobrir la durada mes llarga.
  void lutNet() {
    static const uint8_t a[42]={0x01,0x14,0x0A,0x14,0x00,0x01,0x01};  // comu, com GC
    static const uint8_t b[42]={0x01,0x14,0x00,0x00,0x00,0x01,0x00};  // WW, com DU
    static const uint8_t c[42]={0x01,0x54,0x0A,0x94,0x00,0x01,0x01};  // KW, com GC
    static const uint8_t d[42]={0x01,0x94,0x0A,0x54,0x00,0x01,0x01};  // WK, com GC
    static const uint8_t e[42]={0x01,0x14,0x00,0x00,0x00,0x01,0x00};  // KK, com DU
    taula(0x20,a); taula(0x21,b); taula(0x22,c); taula(0x23,d); taula(0x24,e);
  }
};

#endif // CROWPANEL42_V2_H

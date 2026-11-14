/* ============================================================
   GLUON — firmware connecté
   Yeux + réveil + OTA + météo + agenda + messages poussés
   ------------------------------------------------------------
   BIBLIOTHÈQUES À INSTALLER (gestionnaire de bibliothèques) :
     Adafruit GFX      Adafruit SSD1351      ArduinoJson (v7)
   ------------------------------------------------------------
   CÂBLAGE
     OLED   VCC 3V3  GND  DIN 23  CLK 18  CS 5  DC 4  RST 32
     AMPLI  Vin 5V   GND  BCLK 26 LRC 25  DIN 33  GAIN->GND  SD libre
   ------------------------------------------------------------
   BOUTON switch 27 + GND     LED 14 + GND
   ------------------------------------------------------------
   COMPORTEMENTS AUTOMATIQUES
     - écran éteint et LED en veilleuse pendant la plage de nuit
     - lever progressif : dégradé chaud sur l'écran et LED qui monte
       pendant les N minutes précédant l'alarme
     - « Je suis déjà levé » dans l'interface saute la sonnerie du jour
       sans désactiver l'alarme pour les jours suivants
   ------------------------------------------------------------
   BOUTON
     appui court  -> change d'écran (visage / infos)
     appui long   -> veille
     pendant la sonnerie : court = snooze, long = arrêt
     Après l'arrêt de l'alarme, Gluon te raconte ta journée dans une
     bulle : chaque appui court passe à la suite, le dernier le rend
     à son état normal.
   ------------------------------------------------------------
   MISE À JOUR SANS FIL
     Après le premier téléversement par câble, l'IDE Arduino
     proposera "gluon" dans Outils > Port, section réseau.
     Mot de passe OTA : voir OTA_PASS dans config.h.
   ============================================================ */

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>

// --- Animations, facultatives : chaque fichier anim_*.h present dans ce dossier
// --- est pris en compte tout seul. Surveille la taille du programme.
#if __has_include("anim_cfc.h")
  #include "anim_cfc.h"
  #define AVEC_ANIM_CFC
#endif
#if __has_include("anim_fission.h")
  #include "anim_fission.h"
  #define AVEC_ANIM_FIS
#endif
#if __has_include("anim_bragg.h")
  #include "anim_bragg.h"
  #define AVEC_ANIM_BRA
#endif
#if __has_include("anim_orbitales.h")
  #include "anim_orbitales.h"
  #define AVEC_ANIM_ORB
#endif

// ---------- Réglages personnels ----------
// Wi-Fi, mots de passe, position, agenda : tout est dans config.h, qui ne
// doit jamais être publié. Copie config.example.h en config.h et remplis-le.
#if __has_include("config.h")
  #include "config.h"
#else
  #error "config.h manquant : copie config.example.h en config.h, puis remplis-le."
#endif

// ---------- Identité ----------
// Une seule ligne à changer pour le rebaptiser : nom réseau, point
// d'accès de secours, hôte OTA, sauvegarde et titre de la page web.
#define NOM      "gluon"        // http://gluon.local
#define NOM_AFF  "Gluon"        // nom affiché
#define VER_GLUON "v15"         // affiche dans l'interface : verifie que la MAJ est passee

// Puissance d'emission Wi-Fi. 20 dBm = maximum et gros appels de courant.
// 13 dBm suffit largement dans un logement et soulage l'alimentation.
// Repasse a WIFI_POWER_19_5dBm si la portee ne suffit pas.
#define TX_WIFI  WIFI_POWER_13dBm

// ---------- Broches ----------
#define PIN_CS    5
#define PIN_DC    4
#define PIN_RST  32
#define PIN_BTN  27
#define PIN_LED  14
#define I2S_BCLK 26
#define I2S_LRC  25
#define I2S_DIN  33

#define SAMPLE_RATE 32000
#define SCR_W 128
#define SCR_H 128

Adafruit_SSD1351 oled(SCR_W, SCR_H, &SPI, PIN_CS, PIN_DC, PIN_RST);
GFXcanvas16 cv(SCR_W, SCR_H);
WebServer serveur(80);
Preferences prefs;

#define C(r,g,b) (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
uint16_t COL_EYE = C(150, 215, 255);
const uint16_t COL_BG   = C(0, 0, 0);
const uint16_t COL_TXT  = C(120, 160, 190);
const uint16_t COL_VERT = C(0, 255, 102);      // vert
const uint16_t COL_OR   = C(255, 180, 60);

const int EYE_W = 34, EYE_H = 40, EYE_GAP = 16, EYE_R = 10;

// ============================================================
//   I2S
// ============================================================
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #include <ESP_I2S.h>
  I2SClass i2s;
  bool audioInit() {
    i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DIN);
    return i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  }
  void audioWrite(int16_t* b, size_t n) { i2s.write((uint8_t*)b, n); }
  #define LED_ATTACH()  ledcAttach(PIN_LED, 5000, 8)
  #define LED_SET(v)    ledcWrite(PIN_LED, v)
#else
  #include <driver/i2s.h>
  bool audioInit() {
    i2s_config_t cfg = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = SAMPLE_RATE, .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = 0, .dma_buf_count = 8, .dma_buf_len = 256, .use_apll = false };
    i2s_pin_config_t p = { .bck_io_num = I2S_BCLK, .ws_io_num = I2S_LRC,
      .data_out_num = I2S_DIN, .data_in_num = I2S_PIN_NO_CHANGE };
    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) return false;
    return i2s_set_pin(I2S_NUM_0, &p) == ESP_OK;
  }
  void audioWrite(int16_t* b, size_t n) { size_t e; i2s_write(I2S_NUM_0, b, n, &e, portMAX_DELAY); }
  #define LED_ATTACH()  do { ledcSetup(1, 5000, 8); ledcAttachPin(PIN_LED, 1); } while(0)
  #define LED_SET(v)    ledcWrite(1, v)
#endif

// ============================================================
//   SYNTHÉ (tâche dédiée, flux jamais interrompu)
// ============================================================
struct Note { uint16_t freq; uint16_t ms; };

const Note M_REVEIL[] = {{660,90},{0,40},{880,90},{0,40},{1180,140}};
const Note M_DODO[]   = {{880,110},{0,40},{560,160}};
const Note M_BLIP[]   = {{1400,45}};
const Note M_PING[]   = {{1046,70},{0,50},{1318,110}};
// petites syllabes : le bruit qu'il fait quand il "parle"
const Note M_SYL1[]   = {{784,36}};
const Note M_SYL2[]   = {{880,36}};
const Note M_SYL3[]   = {{988,36}};
const Note M_SYL4[]   = {{698,36}};
#define NB(x) (sizeof(x)/sizeof(Note))

// Notes, pour écrire les mélodies lisiblement (Hz)
#define A4 440
#define B4 494
#define C5 523
#define Cd5 554
#define D5 587
#define Dd5 622
#define E5 659
#define F5 698
#define Fd5 740
#define G5 784
#define Gd5 831
#define A5 880
#define B5 988
#define C6 1047
#define Cd6 1109
#define D6 1175
#define E6 1319

// ---------- Sonneries ----------
// Une mélodie par sonnerie, jouée en boucle. L'insistance vient du
// volume, qui monte par paliers de 25 secondes.
const Note S_DOUCE[]   = {{660,180},{0,240},{880,180},{0,900}};
const Note S_CARILLON[]= {{1046,160},{880,160},{698,160},{587,240},{0,700}};
const Note S_INSIST[]  = {{988,110},{0,70},{1319,110},{0,70},
                          {988,110},{0,70},{1319,110},{0,380}};
const Note S_SIRENE[]  = {{700,260},{980,260},{700,260},{980,260},{0,420}};
const Note S_MATIN[]   = {{1046,60},{0,60},{1046,60},{0,60},{1046,60},{0,60},
                          {1046,60},{0,700}};
const Note S_MONTEE[]  = {{523,110},{659,110},{784,110},{1046,110},
                          {784,110},{1046,220},{0,600}};

// --- Airs du domaine public ---

// Gran Vals, Francisco Tarrega (1902). La phrase que tout le monde
// reconnait comme la sonnerie Nokia.
const Note S_GRANVALS[] = {
  {E6,140},{D6,140},{Fd5,280},{Gd5,280},
  {Cd6,140},{B5,140},{D5,280},{E5,280},
  {B5,140},{A5,140},{Cd5,280},{E5,280},
  {A5,560},{0,500}
};

// La Cucaracha, traditionnel mexicain.
const Note S_CUCARACHA[] = {
  {C5,110},{C5,110},{C5,110},{F5,220},{A5,440},{0,160},
  {C5,110},{C5,110},{C5,110},{F5,220},{A5,440},{0,220},
  {F5,180},{F5,180},{E5,180},{E5,180},{D5,180},{D5,180},{C5,520},{0,500}
};

// Lettre a Elise, Beethoven.
const Note S_ELISE[] = {
  {E5,150},{Dd5,150},{E5,150},{Dd5,150},{E5,150},{B4,150},
  {D5,150},{C5,150},{A4,420},{0,500}
};

// Ode a la joie, Beethoven.
const Note S_JOIE[] = {
  {E5,260},{E5,260},{F5,260},{G5,260},
  {G5,260},{F5,260},{E5,260},{D5,260},
  {C5,260},{C5,260},{D5,260},{E5,260},
  {E5,390},{D5,130},{D5,520},{0,450}
};

// Le Beau Danube bleu, Johann Strauss fils.
const Note S_DANUBE[] = {
  {D5,180},{Fd5,180},{A5,540},{0,120},
  {A5,180},{A5,180},{0,180},{A5,180},{A5,180},{0,300},
  {B5,540},{0,120},{B5,180},{B5,180},{0,180},{B5,180},{B5,180},{0,500}
};

// Toccata en re mineur, Bach. Pour les reveils difficiles.
const Note S_TOCCATA[] = {
  {A5,90},{G5,90},{A5,90},{A5,520},{0,140},
  {G5,110},{F5,110},{E5,110},{D5,110},{Cd5,220},{D5,700},{0,450}
};

struct Sonnerie { const char* nom; const Note* n; uint8_t len; };
const Sonnerie SONNERIES[] = {
  { "Douce",     S_DOUCE,    NB(S_DOUCE)    },
  { "Carillon",  S_CARILLON, NB(S_CARILLON) },
  { "Insistante",S_INSIST,   NB(S_INSIST)   },
  { "Sirene",    S_SIRENE,   NB(S_SIRENE)   },
  { "Reveil",    S_MATIN,    NB(S_MATIN)    },
  { "Montee",    S_MONTEE,   NB(S_MONTEE)   },
  { "Gran Vals", S_GRANVALS, NB(S_GRANVALS) },
  { "Cucaracha", S_CUCARACHA,NB(S_CUCARACHA)},
  { "Fur Elise", S_ELISE,    NB(S_ELISE)    },
  { "Ode joie",  S_JOIE,     NB(S_JOIE)     },
  { "Danube",    S_DANUBE,   NB(S_DANUBE)   },
  { "Toccata",   S_TOCCATA,  NB(S_TOCCATA)  },
};
const int NB_SONNERIES = sizeof(SONNERIES) / sizeof(Sonnerie);
uint8_t sonnerie = 0;

volatile const Note* reqMel = nullptr;
volatile int   reqLen = 0;
volatile bool  reqLoop = false, reqNouveau = false;
volatile float reqVol = 1.0, volMaster = 0.9;
volatile bool  sonActif = false;   // vrai tant qu'une melodie joue reellement

void jouer(const Note* m, int len, bool boucle, float vol = 1.0) {
  reqMel = m; reqLen = len; reqLoop = boucle; reqVol = vol; reqNouveau = true;
  sonActif = (m != nullptr);
}
void stopSon() { jouer(nullptr, 0, false); }

void tacheAudio(void*) {
  const int T = 256;
  int16_t buf[T];
  float phase = 0;
  const long FONDU = SAMPLE_RATE / 250;
  const Note* m = nullptr;
  int len = 0, idx = 0; bool boucle = false; float vol = 1.0; long ech = 0;

  for (;;) {
    if (reqNouveau) {
      m = (const Note*)reqMel; len = reqLen; boucle = reqLoop; vol = reqVol;
      idx = 0; ech = 0; phase = 0; reqNouveau = false;
      sonActif = (m != nullptr);
    }
    for (int i = 0; i < T; i += 2) {
      int16_t s = 0;
      if (m && idx < len) {
        Note n = m[idx];
        long duree = (long)SAMPLE_RATE * n.ms / 1000; if (duree < 1) duree = 1;
        if (n.freq > 0) {
          float env = 1.0;
          if (ech < FONDU) env = (float)ech / FONDU;
          else if (duree - ech < FONDU) env = (float)(duree - ech) / FONDU;
          if (env < 0) env = 0;
          phase += 2.0 * PI * n.freq / SAMPLE_RATE;
          if (phase > 2 * PI) phase -= 2 * PI;
          s = (int16_t)(sinf(phase) * 28000 * volMaster * reqVol * env);
        }
        if (++ech >= duree) { ech = 0; if (++idx >= len) { if (boucle) idx = 0; else { m = nullptr; sonActif = false; } } }
      }
      buf[i] = s; buf[i + 1] = s;
    }
    audioWrite(buf, sizeof(buf));
  }
}

// ============================================================
//   ÉTAT
// ============================================================
enum Mood { NEUTRE, HEUREUX, FACHE, FATIGUE, AMOUREUX, SURPRIS };
enum Ecran { E_VISAGE, E_INFOS };

Mood mood = NEUTRE;
Ecran ecran = E_VISAGE;
bool autoMode = true, dodo = false;
uint8_t ledJour = 40, ledNuit = 8;

float offX = 0, offY = 0, tgtX = 0, tgtY = 0, lid = 1.0;
unsigned long tNextBlink = 0, tNextLook = 0, tNextMood = 0, tBlinkStart = 0;
bool blinking = false;

struct Alarme { bool actif = false; uint8_t heure = 7, minute = 30,
                jours = 0b0111110, snooze = 9; } al;
enum EtatAl { AL_REPOS, AL_SONNE, AL_SNOOZE };
EtatAl etatAl = AL_REPOS;
int palierAl = -1;
int dernierDeclJour = -1;      // tm_yday du dernier declenchement
int salutJour = -1;        // tm_yday du dernier bonjour
unsigned long tDebutSonnerie = 0, tFinSnooze = 0;
const unsigned long MAX_SONNERIE = 10UL * 60UL * 1000UL;

uint8_t nuitDebut = 23, nuitFin = 6;    // extinction automatique de l'écran
uint8_t aubeMin   = 15;                 // lever progressif, 0 = désactivé
float   aubeProg  = 0;                  // 0 au début de l'aube, 1 à l'heure pile
int     sauteJour = -1;                 // tm_yday de la journée volontairement sautée

// --- Météo ---
struct Meteo { bool ok = false; float t = 0, tmin = 0, tmax = 0; int code = -1; } meteo;
unsigned long tProchaineMeteo = 0;
bool modeAP = false;                 // vrai si Gluon a du creer son propre reseau
unsigned long tProchainWifi = 0;

// --- Agenda ---
struct Evt { char h[6]; char t[24]; };
Evt agenda[6];
int nbEvt = 0;
bool agendaOk = false;
unsigned long tProchainAgenda = 0;
String motsDistants[6];
int nbMotsDistants = 0;
unsigned long tProchainMatin = 0;
// --- Message poussé ---
char message[121] = "";
unsigned long tFinMessage = 0;

bool btnPrec = HIGH;
unsigned long tAppui = 0;

// ============================================================
//   BULLE DE DIALOGUE
//   Gluon "parle" : le texte s'écrit lettre par lettre avec un
//   petit bruit de syllabe. Un appui passe à la suite.
// ============================================================
#define BULLE_PAGES  8
#define BULLE_LIGNES 5
#define BULLE_COLS   18

char    bullePage[BULLE_PAGES][BULLE_LIGNES][BULLE_COLS + 1];
uint8_t bulleNbLignes[BULLE_PAGES];
int     bulleNbPages = 0, bullePageActive = 0;
int     bulleLettres = 0, bulleTotal = 0;
bool    bulleActive = false;
unsigned long tLettreSuivante = 0;

// Découpe une phrase en lignes de BULLE_COLS caractères, sans couper les mots.
void ajouterPage(const char* texte) {
  if (bulleNbPages >= BULLE_PAGES) return;
  int p = bulleNbPages, ligne = 0, col = 0;
  for (int i = 0; i < BULLE_LIGNES; i++) bullePage[p][i][0] = 0;

  const char* mot = texte;
  while (*mot && ligne < BULLE_LIGNES) {
    const char* fin = mot;
    while (*fin && *fin != ' ') fin++;
    int lm = fin - mot;
    if (lm > BULLE_COLS) lm = BULLE_COLS;
    if (col > 0 && col + 1 + lm > BULLE_COLS) { ligne++; col = 0; if (ligne >= BULLE_LIGNES) break; }
    if (col > 0) { bullePage[p][ligne][col++] = ' '; }
    for (int k = 0; k < lm; k++) bullePage[p][ligne][col++] = mot[k];
    bullePage[p][ligne][col] = 0;
    mot = (*fin) ? fin + 1 : fin;
  }
  bulleNbLignes[p] = ligne + 1;
  bulleNbPages++;
}

int lettresPage(int p) {
  int n = 0;
  for (int i = 0; i < bulleNbLignes[p]; i++) n += strlen(bullePage[p][i]);
  return n;
}

// ============================================================
//   MOTS DU MATIN
//   Sans accents : la police embarquee ne gere que l'ASCII.
//   Le ton evite le "sois positif" force : certains matins sont
//   durs, autant le dire.
// ============================================================
const char* MOTS[] = {
  "Le plus dur est fait : tu es leve.",
  "Tu n'as pas besoin d'etre en forme. Debout, c'est deja bien.",
  "Un pied, puis l'autre. Le reste suivra.",
  "Personne ne demarre a pleine vitesse. Prends ton temps.",
  "Aujourd'hui n'a pas besoin d'etre parfait pour etre bon.",
  "Tu peux avancer lentement. Ca compte quand meme.",
  "Commence par un truc facile. L'elan viendra tout seul.",
  "Rien ne t'oblige a tout regler aujourd'hui.",
  "Si la journee est moyenne, ce sera une journee de passee.",
  "Fais-toi un truc chaud. On verra le reste apres.",
  "Une seule chose a la fois, et ca ira.",
  "Le matin ment souvent. Attends midi pour juger.",
  "Ce que tu feras aujourd'hui suffira.",
};
const int NB_MOTS = sizeof(MOTS) / sizeof(char*);

// Une phrase qui colle a la meteo, au jour ou a la charge du planning.
const char* motContextuel(const struct tm& t) {
  if (meteo.ok) {
    if (meteo.code >= 51 && meteo.code <= 67) return "Il pleut dehors. Parfait pour une tasse fumante.";
    if (meteo.code >= 71 && meteo.code <= 86) return "Il neige. Prends cinq minutes de plus.";
    if (meteo.t < 3)  return "Il fait froid. Couvre-toi bien avant de sortir.";
    if (meteo.t > 27) return "Il va faire chaud. Pense a boire aujourd'hui.";
    if (meteo.code == 0) return "Grand ciel bleu. Sors la tete dehors si tu peux.";
  }
  if (t.tm_wday == 1) return "Lundi. On demarre doucement, personne ne regarde.";
  if (t.tm_wday == 5) return "Vendredi. Le bout est en vue.";
  if (t.tm_wday == 0 || t.tm_wday == 6) return "C'est le week-end. Rien ne presse ce matin.";
  if (agendaOk && nbEvt >= 4) return "Journee chargee. Une chose a la fois, pas deux.";
  if (agendaOk && nbEvt == 0) return "Aucun rendez-vous. La journee est a toi.";
  return nullptr;
}

const char* motDuJour(int graine) {
  if (nbMotsDistants > 0) {
    static String choix;
    choix = motsDistants[((graine * 7 + 3) % nbMotsDistants + nbMotsDistants) % nbMotsDistants];
    return choix.c_str();
  }
  return MOTS[((graine * 7 + 3) % NB_MOTS + NB_MOTS) % NB_MOTS];
}

void construireBriefing() {
  bulleNbPages = 0;
  struct tm t; bool okH = getLocalTime(&t, 5);

  char buf[96];
  if (okH) {
    const char* salut = (t.tm_hour < 11) ? "Bonjour !" : (t.tm_hour < 18) ? "Salut !" : "Bonsoir !";
    snprintf(buf, sizeof(buf), "%s Il est %dh%02d.", salut, t.tm_hour, t.tm_min);
  } else {
    snprintf(buf, sizeof(buf), "Bonjour ! Je n'ai pas l'heure.");
  }
  ajouterPage(buf);

  if (meteo.ok) {
    snprintf(buf, sizeof(buf), "Dehors : %.0f degres, %s. Entre %.0f et %.0f aujourd'hui.",
             meteo.t, libelleMeteo(meteo.code), meteo.tmin, meteo.tmax);
    ajouterPage(buf);
  }

  if (!agendaOk) {
    ajouterPage("Je n'ai pas ton agenda.");
  } else if (nbEvt == 0) {
    ajouterPage("Rien de prevu aujourd'hui. Profites-en !");
  } else {
    snprintf(buf, sizeof(buf), "Tu as %d rendez-vous aujourd'hui.", nbEvt);
    ajouterPage(buf);
    for (int i = 0; i < nbEvt && bulleNbPages < BULLE_PAGES - 1; i++) {
      snprintf(buf, sizeof(buf), "%s : %s", agenda[i].h, agenda[i].t);
      ajouterPage(buf);
    }
  }
  struct tm tt;
  int graine = getLocalTime(&tt, 5) ? tt.tm_yday : (int)(millis() / 1000);
  if (okH) {
    const char* ctx = motContextuel(t);
    if (ctx) ajouterPage(ctx);
  }
  ajouterPage(motDuJour(graine));
}

void demarrerBulle() {
  construireBriefing();
  if (bulleNbPages == 0) return;
  bulleActive = true; bullePageActive = 0;
  bulleLettres = 0; bulleTotal = lettresPage(0);
  tLettreSuivante = millis() + 300;
  dodo = false; mood = HEUREUX;
}

// Retourne false quand il n'y a plus rien à dire.
bool bulleSuivante() {
  if (bulleLettres < bulleTotal) { bulleLettres = bulleTotal; return true; }  // finit la phrase
  bullePageActive++;
  if (bullePageActive >= bulleNbPages) { bulleActive = false; return false; }
  bulleLettres = 0; bulleTotal = lettresPage(bullePageActive);
  tLettreSuivante = millis() + 120;
  return true;
}

void majBulle() {
  if (!bulleActive) return;
  if (bulleLettres >= bulleTotal) return;
  if (millis() < tLettreSuivante) return;

  bulleLettres++;
  tLettreSuivante = millis() + 42;

  // une syllabe toutes les deux lettres, hauteur variable
  if (bulleLettres % 2 == 0) {
    switch (bulleLettres % 8) {
      case 0: jouer(M_SYL1, NB(M_SYL1), false, 0.5); break;
      case 2: jouer(M_SYL2, NB(M_SYL2), false, 0.5); break;
      case 4: jouer(M_SYL3, NB(M_SYL3), false, 0.5); break;
      default: jouer(M_SYL4, NB(M_SYL4), false, 0.5); break;
    }
  }
}

// ============================================================
//   ANIMATIONS SCIENTIFIQUES (recolorées à l'affichage)
//   Les données ne stockent qu'une intensité : la teinte est
//   appliquée à l'affichage, donc elle suit la couleur des yeux.
// ============================================================
struct Anim {
  const uint8_t*  data;
  const uint32_t* off;
  uint16_t n, ms;
};
// La derniere ligne de chaque tableau reste vide : Gluon fonctionne sans
// aucune animation, et un tableau C++ ne peut pas etre vide.
const Anim ANIMS[] = {
#ifdef AVEC_ANIM_CFC
  { ANIM_CFC_DATA, ANIM_CFC_OFF, ANIM_CFC_N, ANIM_CFC_MS },
#endif
#ifdef AVEC_ANIM_FIS
  { ANIM_FIS_DATA, ANIM_FIS_OFF, ANIM_FIS_N, ANIM_FIS_MS },
#endif
#ifdef AVEC_ANIM_BRA
  { ANIM_BRA_DATA, ANIM_BRA_OFF, ANIM_BRA_N, ANIM_BRA_MS },
#endif
#ifdef AVEC_ANIM_ORB
  { ANIM_ORB_DATA, ANIM_ORB_OFF, ANIM_ORB_N, ANIM_ORB_MS },
#endif
  { nullptr, nullptr, 0, 0 },
};
const char* ANIM_NOMS[] = {
#ifdef AVEC_ANIM_CFC
  "Maille CFC",
#endif
#ifdef AVEC_ANIM_FIS
  "Fission",
#endif
#ifdef AVEC_ANIM_BRA
  "Bragg",
#endif
#ifdef AVEC_ANIM_ORB
  "Orbitales",
#endif
  "",
};
const int NB_ANIMS = sizeof(ANIMS) / sizeof(Anim) - 1;

bool animEnCours = false;
int  animIdx = 0, animImage = 0;
unsigned long tImageSuivante = 0, tProchaineAnim = 0;
uint16_t rampe[16];              // 16 nuances dérivées de COL_EYE

void construireRampe() {
  uint8_t r = (COL_EYE >> 11) & 0x1F, g = (COL_EYE >> 5) & 0x3F, b = COL_EYE & 0x1F;
  for (int i = 0; i < 16; i++) {
    float k = i / 15.0;
    rampe[i] = ((uint16_t)(r * k) << 11) | ((uint16_t)(g * k) << 5) | (uint16_t)(b * k);
  }
}

// Signature sans type maison : l'IDE Arduino génère les prototypes
// avant la définition des structures, il ne connaîtrait pas "Anim".
void decoderImage(const uint8_t* data, const uint32_t* off, int img) {
  uint16_t* buf = cv.getBuffer();
  uint32_t p = off[img], fin = off[img + 1];
  int px = 0;
  while (p + 1 < fin && px < SCR_W * SCR_H) {
    uint16_t coul = rampe[data[p] & 0x0F];
    int n = data[p + 1];
    for (int k = 0; k < n && px < SCR_W * SCR_H; k++) buf[px++] = coul;
    p += 2;
  }
  while (px < SCR_W * SCR_H) buf[px++] = COL_BG;
}

void lancerAnim(int quelle) {
  if (NB_ANIMS == 0) return;
  animIdx = (quelle >= 0 && quelle < NB_ANIMS) ? quelle : random(NB_ANIMS);
  animImage = 0; animEnCours = true;
  construireRampe();
  tImageSuivante = millis();
}

// ============================================================
//   SAUVEGARDE
// ============================================================
void sauver() {
  prefs.begin(NOM, false);
  prefs.putBool("act", al.actif);
  prefs.putUChar("h", al.heure); prefs.putUChar("m", al.minute);
  prefs.putUChar("j", al.jours); prefs.putUChar("s", al.snooze);
  prefs.putUShort("col", COL_EYE);
  prefs.putUChar("vol", (uint8_t)(volMaster * 100));
  prefs.putUChar("lj", ledJour);
  prefs.putUChar("son", sonnerie);
  prefs.putUChar("aub", aubeMin);
  prefs.putUChar("nd", nuitDebut); prefs.putUChar("nf", nuitFin);
  prefs.end();
}
void charger() {
  prefs.begin(NOM, true);
  al.actif = prefs.getBool("act", false);
  al.heure = prefs.getUChar("h", 7); al.minute = prefs.getUChar("m", 30);
  al.jours = prefs.getUChar("j", 0b0111110); al.snooze = prefs.getUChar("s", 9);
  COL_EYE = prefs.getUShort("col", C(150, 215, 255));
  volMaster = prefs.getUChar("vol", 90) / 100.0;
  ledJour = prefs.getUChar("lj", 40);
  sonnerie = prefs.getUChar("son", 0);
  aubeMin = prefs.getUChar("aub", 15);
  nuitDebut = prefs.getUChar("nd", 23); nuitFin = prefs.getUChar("nf", 6);
  prefs.end();
}

// ============================================================
//   RÉSEAU : météo et agenda
// ============================================================
const char* libelleMeteo(int c) {
  if (c == 0) return "Ciel clair";
  if (c <= 2) return "Peu nuageux";
  if (c == 3) return "Couvert";
  if (c <= 48) return "Brouillard";
  if (c <= 57) return "Bruine";
  if (c <= 67) return "Pluie";
  if (c <= 77) return "Neige";
  if (c <= 82) return "Averses";
  if (c <= 86) return "Neige";
  return "Orage";
}

bool httpGet(const String& url, String& sortie, size_t maxi = 4096) {
  WiFiClientSecure* client = new WiFiClientSecure;
  if (!client) return false;
  client->setInsecure();                 // pas de vérification de certificat
  client->setTimeout(8000);

  HTTPClient https;
  bool ok = false;
  if (https.begin(*client, url)) {
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int code = https.GET();
    if (code == HTTP_CODE_OK) {
      sortie = https.getString();
      if (sortie.length() > maxi) sortie = sortie.substring(0, maxi);
      ok = true;
    }
    https.end();
  }
  delete client;
  return ok;
}
bool httpGetPlain(const String& url, String& sortie, size_t maxi = 2048) {
  WiFiClient client;
  HTTPClient http;
  bool ok = false;
  if (http.begin(client, url)) {
    http.setTimeout(4000);
    if (http.GET() == HTTP_CODE_OK) {
      sortie = http.getString();
      if (sortie.length() > maxi) sortie = sortie.substring(0, maxi);
      ok = true;
    }
    http.end();
  }
  return ok;
}

void majMatin() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (strlen(MATIN_URL) < 10) return;
  String rep;
  if (!httpGetPlain(String(MATIN_URL), rep, 1024)) return;
  JsonDocument doc;
  if (deserializeJson(doc, rep)) return;
  int n = 0;
  for (JsonVariant v : doc["mots"].as<JsonArray>()) {
    if (n >= 6) break;
    motsDistants[n++] = v.as<const char*>();
  }
  nbMotsDistants = n;
}
void majMeteo() {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(LAT, 4) +
               "&longitude=" + String(LON, 4) +
               "&current=temperature_2m,weather_code" +
               "&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto";
  String rep;
  if (!httpGet(url, rep, 2048)) return;

  JsonDocument doc;
  if (deserializeJson(doc, rep)) return;

  meteo.t    = doc["current"]["temperature_2m"] | 0.0f;
  meteo.code = doc["current"]["weather_code"]   | -1;
  meteo.tmax = doc["daily"]["temperature_2m_max"][0] | 0.0f;
  meteo.tmin = doc["daily"]["temperature_2m_min"][0] | 0.0f;
  meteo.ok   = true;
}

void majAgenda() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (strlen(AGENDA_URL) < 10) return;

  String rep;
  if (!httpGet(String(AGENDA_URL), rep, 3072)) return;

  JsonDocument doc;
  if (deserializeJson(doc, rep)) return;

  nbEvt = 0;
  for (JsonObject e : doc["e"].as<JsonArray>()) {
    if (nbEvt >= 6) break;
    strlcpy(agenda[nbEvt].h, e["h"] | "--:--", sizeof(agenda[nbEvt].h));
    strlcpy(agenda[nbEvt].t, e["t"] | "", sizeof(agenda[nbEvt].t));
    nbEvt++;
  }
  agendaOk = true;
}

// Les requêtes réseau bloquent : on les évite pendant la sonnerie.
// Le Wi-Fi peut tomber en pleine nuit : on le relance sans bloquer.
void majWifi() {
  if (modeAP) return;
  if (WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  if (now < tProchainWifi) return;
  tProchainWifi = now + 30000UL;
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void majReseau() {
  if (etatAl == AL_SONNE) return;
  if (sonActif) return;               // pas de TLS pendant que le son joue
  if (animEnCours) return;            // ni pendant une animation
  unsigned long now = millis();
  if (now > tProchaineMeteo)  { majMeteo();  tProchaineMeteo  = now + 1800000UL; } // 30 min
  if (now > tProchainAgenda)  { majAgenda(); tProchainAgenda  = now + 1860000UL; }
    if (now > tProchainMatin)   { majMatin();   tProchainMatin   = now + 1920000UL; }
}

// ============================================================
//   DESSIN
// ============================================================
void dessinerOeil(int cx, int cy, bool gauche) {
  int h = (int)(EYE_H * lid); if (h < 3) h = 3;
  int w = EYE_W;
  if (mood == SURPRIS) { h = min(h + 8, EYE_H + 8); w = EYE_W + 4; }
  int r = min(EYE_R, h / 2), x = cx - w / 2, y = cy - h / 2;

  if (mood == AMOUREUX && lid > 0.8) {
    uint16_t rose = C(255, 90, 130); int s = w / 2;
    cv.fillCircle(cx - s/2, cy - s/3, s/2, rose);
    cv.fillCircle(cx + s/2, cy - s/3, s/2, rose);
    cv.fillTriangle(cx - s, cy - s/6, cx + s, cy - s/6, cx, cy + s, rose);
    return;
  }
  cv.fillRoundRect(x, y, w, h, r, COL_EYE);
  switch (mood) {
    case HEUREUX: cv.fillCircle(cx, cy + h * 0.85, w * 0.78, COL_BG); break;
    case FACHE: { int xa = gauche ? x : x + w, xb = gauche ? x + w : x;
      cv.fillTriangle(xa, y - 1, xb, y - 1, xb, y + h * 0.45, COL_BG); break; }
    case FATIGUE: { int xa = gauche ? x : x + w, xb = gauche ? x + w : x;
      cv.fillTriangle(xa, y - 1, xb, y - 1, xa, y + h * 0.45, COL_BG); break; }
    default: break;
  }
}

bool enPlageNuit(const struct tm& t) {
  if (nuitDebut < nuitFin) return t.tm_hour >= nuitDebut && t.tm_hour < nuitFin;
  return t.tm_hour >= nuitDebut || t.tm_hour < nuitFin;
}

void ecranVisage(const struct tm& t, bool okH) {
  int derive = okH ? ((t.tm_min / 15) % 3) - 1 : 0;
  int demi = EYE_GAP / 2 + EYE_W / 2;
  int cy = 50 + (int)offY + derive;

  if (!dodo) {
    dessinerOeil(SCR_W/2 - demi + (int)offX + derive, cy, true);
    dessinerOeil(SCR_W/2 + demi + (int)offX + derive, cy, false);
  } else {
    cv.fillRoundRect(SCR_W/2 - demi - EYE_W/2 + derive, cy - 2, EYE_W, 4, 2, C(40,60,80));
    cv.fillRoundRect(SCR_W/2 + demi - EYE_W/2 + derive, cy - 2, EYE_W, 4, 2, C(40,60,80));
  }

  if (okH) {
    char b[6]; sprintf(b, "%02d:%02d", t.tm_hour, t.tm_min);
    cv.setTextSize(2); cv.setTextColor(COL_TXT);
    cv.setCursor(34, 92); cv.print(b);
    if (al.actif) cv.fillCircle(64, 86, 2, C(120, 220, 140));
  }
  if (meteo.ok) {
    char b[16]; sprintf(b, "%.0fC", meteo.t);
    cv.setTextSize(1); cv.setTextColor(COL_TXT);
    cv.setCursor(4, 4); cv.print(b);
  }
  if (!okH) {                        // sans heure, pas d'alarme : ca doit se voir
    cv.setTextSize(1); cv.setTextColor(C(255, 120, 80));
    cv.setCursor(24, 108); cv.print("PAS D'HEURE");
  }
}

void ecranInfos(const struct tm& t, bool okH) {
  cv.setTextSize(1);

  // bandeau météo
  cv.setTextColor(COL_VERT);
  if (meteo.ok) {
    char b[40];
    sprintf(b, "%.0fC  %.0f/%.0f", meteo.t, meteo.tmin, meteo.tmax);
    cv.setCursor(4, 5); cv.print(b);
    cv.setCursor(4, 17); cv.print(libelleMeteo(meteo.code));
  } else {
    cv.setCursor(4, 5); cv.print("meteo indisponible");
  }
  cv.drawFastHLine(4, 30, 120, C(0, 90, 40));

  // agenda
  cv.setTextColor(COL_TXT);
  if (!agendaOk) {
    cv.setCursor(4, 40); cv.print("agenda non configure");
  } else if (nbEvt == 0) {
    cv.setTextColor(COL_VERT);
    cv.setCursor(4, 40); cv.print("Rien aujourd'hui.");
  } else {
    int y = 38;
    for (int i = 0; i < nbEvt && y < 118; i++) {
      cv.setTextColor(COL_OR);  cv.setCursor(4, y);  cv.print(agenda[i].h);
      cv.setTextColor(COL_TXT); cv.setCursor(38, y); cv.print(agenda[i].t);
      y += 14;
    }
  }
}

// Dégradé chaud qui monte doucement, paupières qui s'entrouvrent à la fin.
void ecranAube() {
  for (int y = 0; y < SCR_H; y++) {
    float k = aubeProg * (1.25 - y / 150.0);
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    uint8_t r = (uint8_t)(255 * k);
    uint8_t g = (uint8_t)(140 * k * k);
    uint8_t b = (uint8_t)(50  * k * k * k);
    cv.drawFastHLine(0, y, SCR_W, C(r, g, b));
  }
  int demi = EYE_GAP / 2 + EYE_W / 2;
  int h = 3 + (int)(EYE_H * 0.45 * (aubeProg > 0.7 ? (aubeProg - 0.7) / 0.3 : 0));
  uint16_t oe = C(40, 30, 25);
  cv.fillRoundRect(SCR_W/2 - demi - EYE_W/2, 60 - h/2, EYE_W, h, min(8, h/2), oe);
  cv.fillRoundRect(SCR_W/2 + demi - EYE_W/2, 60 - h/2, EYE_W, h, min(8, h/2), oe);
}

void ecranBulle() {
  // deux petits yeux qui dépassent au-dessus de la bulle
  int cy = 14;
  cv.fillRoundRect(64 - 24, cy - 7, 20, 15, 5, COL_EYE);
  cv.fillRoundRect(64 + 4,  cy - 7, 20, 15, 5, COL_EYE);

  // la bulle
  uint16_t bord = COL_EYE, fond = C(14, 20, 28);
  cv.fillRoundRect(4, 32, 120, 88, 9, fond);
  cv.drawRoundRect(4, 32, 120, 88, 9, bord);
  cv.fillTriangle(30, 33, 46, 33, 34, 24, fond);      // la queue vers les yeux
  cv.drawLine(30, 33, 34, 24, bord);
  cv.drawLine(34, 24, 46, 33, bord);
  cv.drawFastHLine(31, 33, 15, fond);

  // texte tapé lettre par lettre
  cv.setTextSize(1);
  cv.setTextColor(C(226, 240, 250));
  int reste = bulleLettres, y = 42;
  for (int i = 0; i < bulleNbLignes[bullePageActive]; i++) {
    const char* l = bullePage[bullePageActive][i];
    int n = strlen(l);
    int aff = reste > n ? n : reste;
    if (aff > 0) {
      cv.setCursor(11, y);
      for (int k = 0; k < aff; k++) cv.write(l[k]);
    }
    reste -= aff;
    y += 14;
    if (reste <= 0) break;
  }

  // indicateur : suite ou fin
  if (bulleLettres >= bulleTotal) {
    bool dernier = (bullePageActive >= bulleNbPages - 1);
    cv.setTextColor(dernier ? C(120, 220, 140) : C(90, 130, 160));
    cv.setCursor(dernier ? 92 : 100, 108);
    cv.print(dernier ? "fin" : ">>");
  }
}

void ecranMessage() {
  cv.setTextSize(1);
  cv.setTextColor(COL_VERT);
  cv.setCursor(4, 6); cv.print("MESSAGE");
  cv.drawFastHLine(4, 18, 120, C(0, 90, 40));
  cv.setTextColor(C(230, 240, 250));

  // découpe manuelle en lignes de 20 caractères
  int y = 30, col = 0;
  cv.setCursor(4, y);
  for (int i = 0; message[i] && y < 120; i++) {
    if (message[i] == '\n' || col >= 20) { y += 12; col = 0; cv.setCursor(4, y);
                                            if (message[i] == '\n') continue; }
    cv.write(message[i]); col++;
  }
}

void dessiner() {
  struct tm t; bool okH = getLocalTime(&t, 5);

  if (aubeProg > 0.02 && etatAl == AL_REPOS && !bulleActive && !message[0]) {
    cv.fillScreen(COL_BG);
    ecranAube();
    oled.drawRGBBitmap(0, 0, cv.getBuffer(), SCR_W, SCR_H);
    return;
  }

  if (okH && enPlageNuit(t) && etatAl != AL_SONNE && !message[0] && !bulleActive) {
    static unsigned long dernier = 0;
    if (millis() - dernier > 1000) {
      cv.fillScreen(COL_BG);
      oled.drawRGBBitmap(0, 0, cv.getBuffer(), SCR_W, SCR_H);
      dernier = millis();
    }
    return;
  }

  if (animEnCours) {
    if (millis() >= tImageSuivante) {
      decoderImage(ANIMS[animIdx].data, ANIMS[animIdx].off, animImage);
      oled.drawRGBBitmap(0, 0, cv.getBuffer(), SCR_W, SCR_H);
      tImageSuivante = millis() + ANIMS[animIdx].ms;
      if (++animImage >= ANIMS[animIdx].n) { animEnCours = false; tProchaineAnim = 0; }
    }
    return;
  }

  cv.fillScreen(COL_BG);
  if (bulleActive)                          ecranBulle();
  else if (message[0] && millis() < tFinMessage) ecranMessage();
  else if (ecran == E_INFOS)                ecranInfos(t, okH);
  else                                      ecranVisage(t, okH);

  oled.drawRGBBitmap(0, 0, cv.getBuffer(), SCR_W, SCR_H);
}

// ============================================================
//   ANIMATION
// ============================================================
void majClignement() {
  if (dodo) { lid = 1.0; return; }
  unsigned long now = millis();
  if (!blinking && now > tNextBlink) { blinking = true; tBlinkStart = now; }
  if (blinking) {
    unsigned long dt = now - tBlinkStart; const unsigned long D = 170;
    if (dt >= D) { blinking = false; lid = 1.0; tNextBlink = now + random(2200, 6500); }
    else { lid = fabs((float)dt / D - 0.5) * 2.0; if (lid < 0.05) lid = 0.05; }
  }
}
void majRegard() {
  unsigned long now = millis();
  bool nerveux = (etatAl == AL_SONNE);
  if (autoMode && !dodo && now > tNextLook) {
    tgtX = random(-16, 17); tgtY = random(-10, 11);
    tNextLook = now + (nerveux ? random(200, 500) : random(1500, 4500));
  }
  float k = nerveux ? 0.35 : 0.12;
  offX += (tgtX - offX) * k; offY += (tgtY - offY) * k;
}
void majHumeur() {
  if (etatAl == AL_SONNE) { mood = SURPRIS; return; }
  if (!autoMode || dodo) return;
  unsigned long now = millis();
  if (now > tNextMood) {
    int r = random(100);
    if      (r < 50) mood = NEUTRE;
    else if (r < 75) mood = HEUREUX;
    else if (r < 85) mood = FATIGUE;
    else if (r < 93) mood = SURPRIS;
    else             mood = FACHE;
    tNextMood = now + random(6000, 15000);
  }
}
// Il en lance une de temps en temps, quand rien d'autre ne se passe.
void majAnim() {
  unsigned long now = millis();
  if (animEnCours) return;
  if (etatAl != AL_REPOS || dodo || message[0] || bulleActive || ecran != E_VISAGE) {
    tProchaineAnim = now + 240000UL; return;
  }
  struct tm t;
  if (getLocalTime(&t, 5) && enPlageNuit(t)) { tProchaineAnim = now + 240000UL; return; }
  if (tProchaineAnim == 0) { tProchaineAnim = now + random(180000, 360000); return; }
  if (now > tProchaineAnim) { lancerAnim(-1); tProchaineAnim = 0; }
}

void majLed() {
  struct tm t; bool okH = getLocalTime(&t, 5);
  if (etatAl == AL_SONNE) { LED_SET((millis() / 200) % 2 ? 255 : 20); return; }
  if (aubeProg > 0.02) { LED_SET((uint8_t)(ledNuit + (255 - ledNuit) * aubeProg)); return; }
  if (okH && enPlageNuit(t)) { LED_SET(ledNuit); return; }
  LED_SET(dodo ? ledNuit : ledJour);
}

// ============================================================
//   RÉVEIL
// ============================================================
void declencher() {
  etatAl = AL_SONNE; tDebutSonnerie = millis(); palierAl = -1;
  dodo = false; autoMode = true; mood = SURPRIS; ecran = E_VISAGE;
}
void arreter() {
  etatAl = AL_REPOS; stopSon();
  mood = HEUREUX; tNextMood = millis() + 8000;
  jouer(M_REVEIL, NB(M_REVEIL), false);
  tProchainAgenda = 0;              // rafraîchit l'agenda
    { struct tm t; if (getLocalTime(&t, 5)) salutJour = t.tm_yday; }
  demarrerBulle();                  // puis il te raconte ta journée
}
void snoozer() {
  etatAl = AL_SNOOZE; stopSon();
  tFinSnooze = millis() + (unsigned long)al.snooze * 60000UL;
  mood = FATIGUE; tNextMood = millis() + 10000;
  jouer(M_DODO, NB(M_DODO), false);
}

// Combien reste-t-il avant la sonnerie ? Sert au lever progressif.
void majAube() {
  aubeProg = 0;
  if (!al.actif || aubeMin == 0 || etatAl != AL_REPOS) return;
  struct tm t;
  if (!getLocalTime(&t, 5)) return;
  if (t.tm_yday == sauteJour) return;
  int bit = (t.tm_wday + 6) % 7;
  if (!(al.jours & (1 << bit))) return;

  float reste = (al.heure * 60 + al.minute) - (t.tm_hour * 60 + t.tm_min) - t.tm_sec / 60.0;
  if (reste <= 0 || reste > aubeMin) return;
  aubeProg = 1.0 - reste / aubeMin;
  if (aubeProg < 0) aubeProg = 0;
  if (aubeProg > 1) aubeProg = 1;
}

void majReveil() {
  if (etatAl == AL_SONNE) {
    unsigned long ec = millis() - tDebutSonnerie;
    if (palierAl < 0) {                      // la melodie ne demarre qu'une fois
      palierAl = 1;
      const Sonnerie& s = SONNERIES[sonnerie % NB_SONNERIES];
      jouer(s.n, s.len, true, 0.25);
    }
    float k = ec / 90000.0f;                 // montee continue sur 90 secondes
    if (k > 1.0f) k = 1.0f;
    reqVol = 0.25f + 0.75f * k;
    if (ec > MAX_SONNERIE) arreter();
    return;
  }
  if (!al.actif) return;
  struct tm t; if (!getLocalTime(&t, 5)) return;

  int bit = (t.tm_wday + 6) % 7;
  if (!(al.jours & (1 << bit))) return;
  if (t.tm_yday == sauteJour) return;              // « je suis deja leve »
  if (t.tm_yday == dernierDeclJour) return;        // deja sonne aujourd'hui

  // Fenetre de 5 minutes, pas une minute pile : si Gluon etait occupe
  // ou vient de redemarrer, il sonne quand meme.
  int maintenant = t.tm_hour * 60 + t.tm_min;
  int cible      = al.heure * 60 + al.minute;
  if (maintenant >= cible && maintenant < cible + 5) {
    dernierDeclJour = t.tm_yday;
    declencher();
  }
}

void majBouton() {
  static unsigned long tDernierChangement = 0;
  bool b = digitalRead(PIN_BTN);

  // microswitch mecanique : on ignore les rebonds sous 40 ms
  if (b != btnPrec && millis() - tDernierChangement < 40) return;
  if (b != btnPrec) tDernierChangement = millis();

  if (btnPrec == HIGH && b == LOW) tAppui = millis();
  if (btnPrec == LOW && b == HIGH) {
    unsigned long d = millis() - tAppui;

    if (etatAl == AL_SONNE)       { if (d > 1200) arreter(); else snoozer(); }
    else if (etatAl == AL_SNOOZE) arreter();
    else if (d > 1200) {
      dodo = !dodo;
      jouer(dodo ? M_DODO : M_REVEIL, dodo ? NB(M_DODO) : NB(M_REVEIL), false);
    } else if (bulleActive) {
      if (!bulleSuivante()) jouer(M_BLIP, NB(M_BLIP), false);
    } else if (animEnCours) {
      animEnCours = false; tProchaineAnim = 0;     // on coupe l'animation
    } else {
      struct tm t;
      bool premierDuJour = getLocalTime(&t, 5) && !enPlageNuit(t) && t.tm_yday != salutJour;

      if (premierDuJour) {
        salutJour = t.tm_yday;
        demarrerBulle();                           // le bonjour du matin
      } else if (message[0]) {
        message[0] = 0;                            // acquitte le message
      } else {
        ecran = (ecran == E_VISAGE) ? E_INFOS : E_VISAGE;
        jouer(M_BLIP, NB(M_BLIP), false);
      }
      dodo = false;
    }
  }
  btnPrec = b;
}

// ============================================================
//   PAGE WEB
// ============================================================
const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="fr"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>…</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--edge:#2a3038;--txt:#e6edf3;--ac:#96d7ff;--go:#7ee2a8}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;padding:20px 16px 44px;background:var(--bg);color:var(--txt);
 font-family:-apple-system,system-ui,sans-serif}
h1{font-size:22px;margin:0 0 2px}p.sub{margin:0 0 22px;color:#7d8590;font-size:14px}
h2{font-size:12px;text-transform:uppercase;letter-spacing:.09em;color:#7d8590;margin:26px 0 10px;font-weight:600}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.card{background:var(--card);border:1px solid var(--edge);border-radius:16px;padding:16px}
button{background:var(--card);border:1px solid var(--edge);color:var(--txt);padding:15px 12px;
 border-radius:14px;font-size:16px;font-family:inherit;cursor:pointer;transition:.12s;
 display:flex;align-items:center;justify-content:center;gap:9px}
button:active{transform:scale(.96)}button.on{border-color:var(--ac);background:#1b2734}
.wide{grid-column:span 2}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px}.row+.row{margin-top:16px}
input[type=time]{background:#0d1117;border:1px solid var(--edge);color:var(--txt);
 font-size:30px;font-family:inherit;padding:8px 12px;border-radius:12px;width:100%}
input[type=text]{background:#0d1117;border:1px solid var(--edge);color:var(--txt);
 font-size:16px;font-family:inherit;padding:12px;border-radius:12px;width:100%}
input[type=color]{width:52px;height:34px;border:none;background:none;padding:0}
input[type=number]{background:#0d1117;border:1px solid var(--edge);color:var(--txt);
 font-size:16px;padding:9px;border-radius:10px;width:78px;font-family:inherit}
input[type=range]{flex:1;accent-color:var(--ac)}
.days{display:grid;grid-template-columns:repeat(7,1fr);gap:6px;margin-top:14px}
.days button{padding:11px 0;font-size:13px;border-radius:11px}
.sw{width:54px;height:31px;border-radius:16px;background:#30363d;position:relative;border:none;padding:0;flex:none;transition:.15s}
.sw::after{content:"";position:absolute;top:3px;left:3px;width:25px;height:25px;border-radius:50%;background:#fff;transition:.15s}
.sw.on{background:var(--go)}.sw.on::after{transform:translateX(23px)}
.big{font-size:15px;font-weight:600}.ring{background:#3d1d20;border-color:#f0808a;width:100%;margin-bottom:8px}
</style></head><body>
<h1 id="h1">…</h1><p class="sub" id="st">…</p><div id="alerte"></div>

<h2>Message à l'écran</h2>
<div class="card">
  <div class="row"><input type="text" id="msg" placeholder="Pense à la lessive" maxlength="110"></div>
  <div class="row"><button style="width:100%" onclick="env()">Envoyer</button></div>
</div>

<h2>Réveil</h2>
<div class="card">
  <div class="row"><span class="big">Activé</span><button class="sw" id="sw" onclick="g('/toggle')"></button></div>
  <div class="row"><input type="time" id="hh" onchange="save()"></div>
  <div class="days">
    <button onclick="jr(0)" id="j0">L</button><button onclick="jr(1)" id="j1">M</button>
    <button onclick="jr(2)" id="j2">M</button><button onclick="jr(3)" id="j3">J</button>
    <button onclick="jr(4)" id="j4">V</button><button onclick="jr(5)" id="j5">S</button>
    <button onclick="jr(6)" id="j6">D</button></div>
  <div class="row"><span>Snooze</span><span><input type="number" id="sn" min="1" max="30" onchange="save()"> min</span></div>
  <div class="row"><span>Lever progressif</span>
    <span><input type="number" id="au" min="0" max="45" onchange="g('/aube?v='+au.value)"> min</span></div>
  <div class="row"><span>Nuit de</span>
    <span><input type="number" id="nd" min="0" max="23" onchange="nuit()">h à
          <input type="number" id="nf" min="0" max="23" onchange="nuit()">h</span></div>
  <div class="row"><button id="bLeve" style="width:100%" onclick="g('/leve')">Je suis déjà levé</button></div>
</div>

<h2>Son</h2>
<div class="card">
  <div class="row"><span>Volume</span><input type="range" id="vol" min="10" max="100" onchange="g('/vol?v='+vol.value)"></div>
  <div class="row"><button style="width:100%" onclick="g('/test')">Écouter la sonnerie</button></div>
</div>
<h2>Sonnerie</h2>
<div class="grid" id="sons"></div>

<h2>Humeur</h2>
<div class="grid">
  <button onclick="m(0)">😐 Neutre</button><button onclick="m(1)">😊 Heureux</button>
  <button onclick="m(2)">😠 Fâché</button><button onclick="m(3)">😪 Fatigué</button>
  <button onclick="m(4)">😍 Amoureux</button><button onclick="m(5)">😲 Surpris</button>
</div>

<h2>Il te parle</h2>
<div class="grid">
  <button onclick="g('/parle')">Le point du jour</button>
  <button onclick="g('/mot')">Dis-moi un mot</button>
</div>

<h2>Animations</h2>
<div class="grid" id="anims"></div>

<h2>Réglages</h2>
<div class="card" style="margin-bottom:10px">
  <div class="row"><span>Couleur des yeux</span>
    <input type="color" id="col" value="#96d7ff" onchange="g('/color?v='+col.value.substring(1))"></div>
  <div class="row"><span>Lumière du bouton</span>
    <input type="range" id="lj" min="0" max="255" onchange="g('/led?v='+lj.value)"></div>
</div>
<div class="grid">
  <button id="bEcr" onclick="g('/ecran')">Infos / visage</button>
  <button onclick="g('/refresh')">Rafraîchir</button>
  <button id="bAuto" onclick="g('/auto')">Auto</button>
  <button id="bDodo" onclick="g('/dodo')">Dodo</button>
</div>

<script>
let J=0;
function g(u){return fetch(u).then(r=>r.json()).then(j=>{try{sync(j)}catch(e){console.error(e)}})
  .catch(_=>st.textContent='hors ligne')}
function sync(j){
  J=j.jours;
  h1.textContent=j.nom; document.title=j.nom;
  st.textContent=(j.heure?('il est '+j.heure):'heure inconnue')+(j.al=='snooze'?' · snooze':'')
    +(j.meteo?' · '+j.meteo+'C':'')+' · '+j.ver;
  sw.classList.toggle('on',j.actif); hh.value=j.hm; sn.value=j.snooze;
  vol.value=j.vol; lj.value=j.lj;
  for(let i=0;i<7;i++) document.getElementById('j'+i).classList.toggle('on',!!(J>>i&1));
  bAuto.classList.toggle('on',j.auto); bDodo.classList.toggle('on',j.dodo);
  bEcr.classList.toggle('on',j.ecran==1);
  if(j.anims&&!anims.dataset.ok){anims.innerHTML=j.anims.map((n,i)=>
    '<button onclick="g(\'/anim?v='+i+'\')">'+n+'</button>').join('');anims.dataset.ok=1}
  au.value=j.aube; nd.value=j.nd; nf.value=j.nf;
  bLeve.classList.toggle('on',j.leve);
  bLeve.textContent=j.leve?'Alarme sautée aujourd\'hui':'Je suis déjà levé';
  if(j.sons) sons.innerHTML=j.sons.map((n,i)=>'<button class="'+(i==j.son?'on':'')
    +'" onclick="g(\'/sonnerie?v='+i+'\')">'+n+'</button>').join('');
  alerte.innerHTML=j.al=='sonne'
    ?'<button class="ring" onclick="g(\'/stop\')">Arrêter</button><button style="width:100%" onclick="g(\'/snooze\')">Snooze</button>':'';
}
function env(){if(msg.value){g('/msg?t='+encodeURIComponent(msg.value));msg.value=''}}
function nuit(){g('/nuit?d='+nd.value+'&f='+nf.value)}
function save(){const v=hh.value.split(':');g('/alarme?h='+v[0]+'&m='+v[1]+'&j='+J+'&s='+sn.value)}
function jr(i){J^=(1<<i);save()}
function m(i){g('/mood?v='+i)}
g('/state'); setInterval(()=>g('/state'),5000);
</script></body></html>
)HTML";

String etatJSON() {
  struct tm t; bool ok = getLocalTime(&t, 5);
  char hnow[6] = ""; if (ok) sprintf(hnow, "%02d:%02d", t.tm_hour, t.tm_min);
  char hm[6]; sprintf(hm, "%02d:%02d", al.heure, al.minute);

  String s = "{";
  s += "\"nom\":\"" NOM_AFF "\",\"ver\":\"" VER_GLUON "\"";
  s += ",\"heure\":\"" + String(hnow) + "\",\"hm\":\"" + String(hm) + "\"";
  s += ",\"actif\":"  + String(al.actif ? "true" : "false");
  s += ",\"jours\":"  + String(al.jours);
  s += ",\"snooze\":" + String(al.snooze);
  s += ",\"aube\":"   + String(aubeMin);
  s += ",\"nd\":"     + String(nuitDebut);
  s += ",\"nf\":"     + String(nuitFin);
  s += ",\"leve\":"   + String((ok && t.tm_yday == sauteJour) ? "true" : "false");
  s += ",\"vol\":"    + String((int)(volMaster * 100));
  s += ",\"lj\":"     + String(ledJour);
  s += ",\"auto\":"   + String(autoMode ? "true" : "false");
  s += ",\"dodo\":"   + String(dodo ? "true" : "false");
  s += ",\"ecran\":"  + String((int)ecran);
  s += ",\"son\":"    + String(sonnerie);
  s += ",\"meteo\":"  + String(meteo.ok ? String((int)meteo.t) : String("null"));
  s += ",\"al\":\""  + String(etatAl == AL_SONNE ? "sonne" : etatAl == AL_SNOOZE ? "snooze" : "repos") + "\"";

  s += ",\"sons\":[";
  for (int i = 0; i < NB_SONNERIES; i++) { if (i) s += ","; s += "\"" + String(SONNERIES[i].nom) + "\""; }
  s += "],\"anims\":[";
  for (int i = 0; i < NB_ANIMS; i++) { if (i) s += ","; s += "\"" + String(ANIM_NOMS[i]) + "\""; }
  s += "]}";
  return s;
}

void repondre() { serveur.send(200, "application/json", etatJSON()); }

void routes() {
  serveur.on("/",      []() { serveur.send_P(200, "text/html", PAGE); });
  serveur.on("/state", repondre);

  serveur.on("/msg", []() {
    strlcpy(message, serveur.arg("t").c_str(), sizeof(message));
    tFinMessage = millis() + 60000UL;      // affiché 1 min, ou jusqu'à l'appui
    dodo = false; jouer(M_PING, NB(M_PING), false);
    repondre();
  });

  serveur.on("/alarme", []() {
    if (serveur.hasArg("h")) al.heure  = constrain(serveur.arg("h").toInt(), 0, 23);
    if (serveur.hasArg("m")) al.minute = constrain(serveur.arg("m").toInt(), 0, 59);
    if (serveur.hasArg("j")) al.jours  = serveur.arg("j").toInt() & 0x7F;
    if (serveur.hasArg("s")) al.snooze = constrain(serveur.arg("s").toInt(), 1, 30);
    dernierDeclJour = -1;                          // permet de retester le jour meme
    sauver(); jouer(M_BLIP, NB(M_BLIP), false); repondre();
  });

  serveur.on("/toggle",  []() { al.actif = !al.actif; dernierDeclJour = -1; sauver(); jouer(M_BLIP, NB(M_BLIP), false); repondre(); });
  serveur.on("/stop",    []() { arreter(); repondre(); });
  serveur.on("/snooze",  []() { snoozer(); repondre(); });
  serveur.on("/test", []() {
    const Sonnerie& s = SONNERIES[sonnerie % NB_SONNERIES];
    jouer(s.n, s.len, false, 0.9); repondre();
  });
  serveur.on("/sonnerie", []() {
    sonnerie = constrain(serveur.arg("v").toInt(), 0, NB_SONNERIES - 1);
    const Sonnerie& s = SONNERIES[sonnerie];
    jouer(s.n, s.len, false, 0.9); sauver(); repondre();
  });
  serveur.on("/ecran",   []() { ecran = (ecran == E_VISAGE) ? E_INFOS : E_VISAGE; repondre(); });
  serveur.on("/parle", []() { demarrerBulle(); repondre(); });

  // un encouragement seul, a n'importe quel moment
  serveur.on("/mot", []() {
    bulleNbPages = 0;
    ajouterPage(MOTS[random(NB_MOTS)]);
    bulleActive = true; bullePageActive = 0;
    bulleLettres = 0; bulleTotal = lettresPage(0);
    tLettreSuivante = millis() + 200;
    dodo = false; mood = HEUREUX;
    repondre();
  });

  // « je suis deja leve » : saute la prochaine sonnerie sans desactiver l'alarme
  serveur.on("/leve", []() {
    struct tm t;
    if (getLocalTime(&t, 5)) {
      sauteJour = (sauteJour == t.tm_yday) ? -1 : t.tm_yday;
      if (etatAl != AL_REPOS) { arreter(); }
    }
    jouer(M_BLIP, NB(M_BLIP), false); repondre();
  });

  serveur.on("/aube", []() {
    aubeMin = constrain(serveur.arg("v").toInt(), 0, 45); sauver(); repondre();
  });
  serveur.on("/nuit", []() {
    nuitDebut = constrain(serveur.arg("d").toInt(), 0, 23);
    nuitFin   = constrain(serveur.arg("f").toInt(), 0, 23);
    sauver(); repondre();
  });
  serveur.on("/anim", []() {
    lancerAnim(serveur.hasArg("v") ? serveur.arg("v").toInt() : -1);
    repondre();
  });
  serveur.on("/refresh", []() { tProchaineMeteo = 0; tProchainAgenda = 0; repondre(); });
  serveur.on("/horloge", []() { repondre(); });

  serveur.on("/vol", []() {
    volMaster = constrain(serveur.arg("v").toInt(), 10, 100) / 100.0;
    sauver(); jouer(M_BLIP, NB(M_BLIP), false); repondre();
  });
  serveur.on("/led", []() { ledJour = constrain(serveur.arg("v").toInt(), 0, 255); sauver(); repondre(); });
  serveur.on("/mood", []() {
    int v = serveur.arg("v").toInt();
    if (v >= 0 && v <= 5) { mood = (Mood)v; autoMode = false; dodo = false;
                            ecran = E_VISAGE; jouer(M_BLIP, NB(M_BLIP), false); }
    repondre();
  });
  serveur.on("/auto", []() {
    autoMode = !autoMode;
    if (autoMode) { dodo = false; tNextMood = tNextLook = millis(); }
    jouer(M_BLIP, NB(M_BLIP), false); repondre();
  });
  serveur.on("/dodo", []() {
    dodo = !dodo;
    jouer(dodo ? M_DODO : M_REVEIL, dodo ? NB(M_DODO) : NB(M_REVEIL), false);
    repondre();
  });
  serveur.on("/color", []() {
    long v = strtol(serveur.arg("v").c_str(), NULL, 16);
    COL_EYE = C((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v);
    construireRampe(); sauver(); repondre();
  });
  serveur.onNotFound([]() { serveur.send(404, "text/plain", "?"); });
}

// ============================================================
void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());
  pinMode(PIN_BTN, INPUT_PULLUP);
  LED_ATTACH(); LED_SET(0);

  oled.begin(); oled.fillScreen(COL_BG);
  charger();

  // On etale les demarrages : l'ecran, puis la radio, puis l'audio.
  // Tout allumer en meme temps est ce qui fait chuter l'alimentation.
  delay(800);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setTxPower(TX_WIFI);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(250);

  if (WiFi.status() == WL_CONNECTED) {
    configTzTime(TZ_INFO, "fr.pool.ntp.org", "pool.ntp.org");
    Serial.println(WiFi.localIP());
    ArduinoOTA.setHostname(NOM);
    ArduinoOTA.setPassword(OTA_PASS);
    ArduinoOTA.onStart([]() {
      stopSon();
      cv.fillScreen(COL_BG);
      cv.setTextSize(1); cv.setTextColor(COL_VERT);
      cv.setCursor(18, 60); cv.print("MISE A JOUR...");
      oled.drawRGBBitmap(0, 0, cv.getBuffer(), SCR_W, SCR_H);
    });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int total) {
      int w = (int)(112.0 * p / total);
      cv.drawRect(8, 78, 112, 8, C(0, 90, 40));
      cv.fillRect(8, 78, w, 8, COL_VERT);
      oled.drawRGBBitmap(0, 0, cv.getBuffer(), SCR_W, SCR_H);
    });
    ArduinoOTA.begin();
  } else {
    modeAP = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(NOM_AFF, MDP_AP);
    Serial.println(WiFi.softAPIP());
  }

  MDNS.begin(NOM);
  MDNS.addService("http", "tcp", 80);
  routes(); serveur.begin();

  // l'audio demarre en dernier, une fois la radio stabilisee
  delay(300);
  if (!audioInit()) Serial.println("ECHEC init I2S");
  xTaskCreatePinnedToCore(tacheAudio, "audio", 8192, NULL, 2, NULL, 0);

  tNextBlink = millis() + 1500;
  tNextLook  = millis() + 1000;
  tNextMood  = millis() + 8000;
  tProchaineMeteo = millis() + 3000;
  tProchainAgenda = millis() + 6000;

  construireRampe();
  jouer(M_REVEIL, NB(M_REVEIL), false);
}

// L'affichage tourne a 30 images par seconde au lieu de 125 : trois fois
// moins de trafic SPI et de courant moyen, sans difference visible.
const unsigned long MS_IMAGE = 33;
unsigned long tImageEcran = 0;

void loop() {
  ArduinoOTA.handle();
  serveur.handleClient();
  majBouton();
  majWifi();
  majAube();
  majReveil();
  majReseau();
  majClignement();
  majRegard();
  majHumeur();
  majAnim();
  majBulle();
  majLed();

  if (millis() - tImageEcran >= MS_IMAGE) {
    tImageEcran = millis();
    dessiner();
  }
  delay(4);
}

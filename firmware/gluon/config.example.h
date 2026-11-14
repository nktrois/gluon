// Copie ce fichier en config.h, remplis-le, puis supprime la ligne #error.
// config.h contient tes secrets : il ne doit jamais etre publie.
#pragma once

#error "Remplis config.h, puis supprime cette ligne."

// Wi-Fi, en 2,4 GHz : l'ESP32 ne voit pas le 5 GHz.
const char* WIFI_SSID = "TON_RESEAU";
const char* WIFI_PASS = "TON_MOT_DE_PASSE";

// Mot de passe des mises a jour sans fil. Choisis le tien.
const char* OTA_PASS = "CHANGE_MOI";

// Reseau de secours, cree si le Wi-Fi ne repond pas. 8 caracteres au minimum.
#define MDP_AP "CHANGE_MOI"

// Fuseau horaire, ici l'heure de Paris.
const char* TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3";

// Position pour la meteo (Open-Meteo, sans cle ni compte). Paris par defaut.
// Deux decimales suffisent.
const float LAT = 48.85;
const float LON = 2.35;

// URL de ton Apps Script, finit par /exec. Vide = agenda desactive.
// Qui possede cette URL peut lire les titres de tes rendez-vous du jour.
const char* AGENDA_URL = "";

// Facultatif : adresse d'un JSON de phrases pour le matin, de la forme
// {"mots":["Phrase une.","Phrase deux."]}. Six phrases au plus, sans accents.
// Vide = Gluon utilise ses propres phrases.
const char* MATIN_URL = "";

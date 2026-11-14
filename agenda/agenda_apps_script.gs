/* ============================================================
   AGENDA DU JOUR POUR GLUON
   ------------------------------------------------------------
   À DÉPLOYER CHEZ GOOGLE, pas sur l'ESP32 :

   1. Va sur https://script.google.com  ->  Nouveau projet
   2. Colle ce code, remplace le contenu par défaut
   3. Renomme le projet "gluon-agenda"
   4. Déployer  ->  Nouveau déploiement  ->  type "Application web"
        Exécuter en tant que : moi
        Qui a accès        : tout le monde
   5. Autorise l'accès à ton agenda quand il le demande
   6. Copie l'URL qui finit par /exec
   7. Colle-la dans AGENDA_URL de config.h

   L'URL est secrète mais publique : n'importe qui l'ayant peut lire
   les titres de tes événements du jour. Ne la mets pas sur GitHub.
   ============================================================ */

function doGet() {
  var cal = CalendarApp.getDefaultCalendar();
  var events = cal.getEventsForDay(new Date());

  var out = [];
  for (var i = 0; i < events.length && out.length < 6; i++) {
    var e = events[i];
    var heure = e.isAllDayEvent()
      ? "--:--"
      : Utilities.formatDate(e.getStartTime(), cal.getTimeZone(), "HH:mm");

    // titre tronqué : l'écran fait 20 caractères de large
    var titre = e.getTitle().substring(0, 20);

    out.push({ h: heure, t: titre });
  }

  return ContentService
    .createTextOutput(JSON.stringify({ n: out.length, e: out }))
    .setMimeType(ContentService.MimeType.JSON);
}

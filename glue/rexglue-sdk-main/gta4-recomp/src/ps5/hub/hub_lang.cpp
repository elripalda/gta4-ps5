// GTA IV PS5 - the hub's languages (see hub_lang.h).
#include "hub_lang.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__PROSPERO__)
extern "C" int sceSystemServiceParamGetInt(int param_id, int* value);
#endif

namespace gta4ps5::hub::lang {

namespace {

std::atomic<int> g_lang{kEnglish};

// English, then French, German, Spanish and Italian. French puts a
// no-break space ( ) before : ; ? ! and %.
struct Entry {
  const char* text[kLangCount];
};

// clang-format off
const Entry kTable[] = {
    // ---- the tabs and the footer
    {{"Home", "Accueil", "Start", "Inicio", "Home"}},
    {{"Display", "Affichage", "Anzeige", "Pantalla", "Schermo"}},
    {{"Game", "Jeu", "Spiel", "Juego", "Gioco"}},
    {{"Saves", "Sauvegardes", "Spielstände", "Partidas", "Salvataggi"}},
    {{"Trophies", "Trophées", "Trophäen", "Trofeos", "Trofei"}},
    {{"About", "À propos", "Info", "Acerca de", "Info"}},
    {{"Debug", "Débogage", "Debug", "Depuración", "Debug"}},
    {{"Install|tab", "Installation", "Installation", "Instalación", "Installazione"}},
    {{"Play", "Jouer", "Spielen", "Jugar", "Gioca"}},
    {{"Install", "Installer", "Installieren", "Instalar", "Installa"}},
    {{"Back", "Retour", "Zurück", "Volver", "Indietro"}},
    {{"Reset All", "Tout réinitialiser", "Alles zurücksetzen", "Restablecer todo", "Ripristina tutto"}},
    {{"Adjust", "Modifier", "Ändern", "Cambiar", "Modifica"}},
    {{"Move", "Naviguer", "Navigieren", "Navegar", "Naviga"}},
    {{"Back Up", "Copier", "Sichern", "Copiar", "Backup"}},
    {{"Import", "Importer", "Importieren", "Importar", "Importa"}},
    {{"Restore", "Restaurer", "Wiederherstellen", "Restaurar", "Ripristina"}},
    {{"Tab", "Onglet", "Seite", "Pestaña", "Scheda"}},
    {{"Look Again", "Revérifier", "Neu prüfen", "Volver a buscar", "Ricontrolla"}},
    {{"Cancel", "Annuler", "Abbrechen", "Cancelar", "Annulla"}},
    {{"Try Again", "Réessayer", "Nochmal", "Reintentar", "Riprova"}},
    {{"Reset", "Réinitialiser", "Zurücksetzen", "Restablecer", "Ripristina"}},
    {{"Stop", "Arrêter", "Stoppen", "Detener", "Interrompi"}},
    {{"Keep Going", "Continuer", "Weitermachen", "Continuar", "Continua"}},

    // ---- settings: names and values
    {{"Language", "Langue", "Sprache", "Idioma", "Lingua"}},
    {{"Preset", "Préréglage", "Voreinstellung", "Preajuste", "Preimpostazione"}},
    {{"Recommended", "Recommandé", "Empfohlen", "Recomendado", "Consigliato"}},
    {{"Performance", "Performances", "Leistung", "Rendimiento", "Prestazioni"}},
    {{"Quality", "Qualité", "Qualität", "Calidad", "Qualità"}},
    {{"Custom", "Personnalisé", "Eigene", "Personalizado", "Personalizzato"}},
    {{"Resolution", "Résolution", "Auflösung", "Resolución", "Risoluzione"}},
    {{"Draw Distance", "Distance d'affichage", "Sichtweite", "Distancia de dibujado", "Distanza visiva"}},
    {{"Frame Limit", "Limite d'images", "Bildlimit", "Límite de fotogramas", "Limite fotogrammi"}},
    {{"FPS Counter", "Compteur FPS", "FPS-Anzeige", "Contador de FPS", "Contatore FPS"}},
    {{"Refresh Rate", "Fréquence d'affichage", "Bildwiederholrate", "Frecuencia de actualización", "Frequenza di aggiornamento"}},
    {{"Highest Detail Models", "Modèles les plus détaillés", "Höchste Modelldetails", "Modelos al máximo detalle", "Modelli al massimo dettaglio"}},
    {{"Texture Filtering", "Filtrage des textures", "Texturfilterung", "Filtrado de texturas", "Filtro texture"}},
    {{"Enhanced Effects", "Effets améliorés", "Verbesserte Effekte", "Efectos mejorados", "Effetti migliorati"}},
    {{"Shadow Quality", "Qualité des ombres", "Schattenqualität", "Calidad de sombras", "Qualità ombre"}},
    {{"Frame Generation", "Génération d'images", "Bildgenerierung", "Generación de fotogramas", "Generazione fotogrammi"}},
    {{"Anti-Aliasing", "Anticrénelage", "Kantenglättung", "Antialiasing", "Antialiasing"}},
    {{"Motion Blur", "Flou de mouvement", "Bewegungsunschärfe", "Desenfoque de movimiento", "Sfocatura di movimento"}},
    {{"Reflections", "Reflets", "Spiegelungen", "Reflejos", "Riflessi"}},
    {{"Shadow Distance", "Distance des ombres", "Schattenweite", "Distancia de sombras", "Distanza ombre"}},
    {{"Small Object Shadows", "Ombres des petits objets", "Schatten kleiner Objekte", "Sombras de objetos pequeños", "Ombre oggetti piccoli"}},
    {{"Car Reflections", "Reflets des voitures", "Fahrzeugspiegelungen", "Reflejos de coches", "Riflessi auto"}},
    {{"Skip Intro", "Passer l'intro", "Intro überspringen", "Saltar intro", "Salta intro"}},
    {{"PS5 Trophies", "Trophées PS5", "PS5-Trophäen", "Trofeos de PS5", "Trofei PS5"}},
    {{"Centered Car Camera", "Caméra de véhicule centrée", "Zentrierte Fahrzeugkamera", "Cámara de vehículo centrada",
      "Telecamera veicolo centrata"}},
    {{"Car Camera Return", "Retour de la caméra de véhicule", "Rückkehr der Fahrzeugkamera",
      "Retorno de la cámara de vehículo", "Ritorno della telecamera veicolo"}},
    {{"Car Camera Follow Speed", "Vitesse de suivi de la caméra de véhicule", "Nachführtempo der Fahrzeugkamera",
      "Velocidad de seguimiento de la cámara de vehículo", "Velocità di inseguimento della telecamera veicolo"}},
    {{"Water Reflections", "Reflets de l'eau", "Wasserspiegelungen", "Reflejos del agua", "Riflessi dell'acqua"}},
    {{"Film Grain", "Grain de film", "Filmkorn", "Grano de película", "Grana pellicola"}},
    {{"DualSense Features", "Fonctions DualSense", "DualSense-Funktionen", "Funciones DualSense", "Funzioni DualSense"}},
    {{"Menu Music", "Musique du menu", "Menümusik", "Música del menú", "Musica del menu"}},
    {{"Light Bar", "Barre lumineuse", "Lichtleiste", "Barra de luz", "Barra luminosa"}},
    {{"Audio Delay", "Délai du son", "Tonverzögerung", "Retraso del sonido", "Ritardo audio"}},
    {{"Lowest (32 ms)", "Minimal (32\u00A0ms)", "Minimal (32 ms)", "Mínimo (32 ms)", "Minimo (32 ms)"}},
    {{"Low (64 ms)", "Faible (64\u00A0ms)", "Niedrig (64 ms)", "Bajo (64 ms)", "Basso (64 ms)"}},
    {{"Steady (128 ms)", "Stable (128\u00A0ms)", "Stabil (128 ms)", "Estable (128 ms)", "Stabile (128 ms)"}},
    {{"Original (341 ms)", "D'origine (341\u00A0ms)", "Original (341 ms)", "Original (341 ms)",
      "Originale (341 ms)"}},
    {{"Adaptive Triggers", "Gâchettes adaptatives", "Adaptive Trigger", "Gatillos adaptativos", "Grilletti adattivi"}},
    {{"Extra Rumble", "Vibrations supplémentaires", "Zusätzliche Vibration", "Vibración extra", "Vibrazione extra"}},
    {{"Feedback Strength", "Intensité du retour", "Feedback-Stärke", "Intensidad de respuesta",
      "Intensità del feedback"}},
    {{"Light", "Légère", "Leicht", "Suave", "Leggera"}},
    {{"Medium", "Moyenne", "Mittel", "Media", "Media"}},
    {{"Strong", "Forte", "Stark", "Fuerte", "Forte"}},
    {{"Max", "Maximale", "Maximal", "Máxima", "Massima"}},
    {{"Save Logs", "Enregistrer les journaux", "Logs speichern", "Guardar registros", "Salva log"}},
    {{"Log Detail", "Détail des journaux", "Log-Detail", "Detalle del registro", "Dettaglio log"}},
    {{"Renderer Threads", "Threads du rendu", "Renderer-Threads", "Hilos del renderizador", "Thread del renderer"}},
    {{"Game Thread Capture", "Capture du thread de jeu", "Spielthread-Erfassung", "Captura del hilo del juego", "Cattura thread di gioco"}},
    {{"Frame Time Capture", "Temps d'image", "Bildzeit-Aufzeichnung", "Tiempos de fotograma", "Tempi fotogramma"}},
    {{"Freeze Reports", "Rapports de blocage", "Hänger-Berichte", "Informes de bloqueo", "Report blocchi"}},
    {{"GPU Texture Unpacking", "Textures sur GPU", "Texturen auf GPU", "Texturas en GPU", "Texture su GPU"}},
    {{"Frame Timeline", "Chronologie des images", "Bild-Zeitleiste", "Cronología de fotogramas", "Timeline fotogrammi"}},
    {{"Frame Delivery", "Envoi des images", "Bildausgabe", "Envío de fotogramas", "Invio dei fotogrammi"}},
    {{"As Finished", "Dès qu'elles sont prêtes", "Sobald fertig", "Al terminar", "Appena pronti"}},
    {{"Every Vblank", "À chaque rafraîchissement", "Bei jedem Vblank", "En cada refresco", "A ogni refresh"}},
    {{"Texture Watch", "Surveillance des textures", "Texturüberwachung", "Vigilancia de texturas", "Controllo texture"}},
    {{"Light Test", "Test des lumières", "Lichttest", "Prueba de luces", "Test luci"}},
    {{"Cutscenes at 30", "Cinématiques à 30", "Zwischensequenzen mit 30", "Cinemáticas a 30", "Filmati a 30"}},
    {{"No Stencil", "Sans stencil", "Ohne Stencil", "Sin stencil", "Senza stencil"}},
    {{"No Clamp", "Sans clamp", "Ohne Clamp", "Sin clamp", "Senza clamp"}},
    {{"Auto", "Auto", "Auto", "Auto", "Auto"}},
    {{"Off", "Désactivé", "Aus", "Desactivado", "Disattivato"}},
    {{"On", "Activé", "An", "Activado", "Attivato"}},
    {{"Original", "D'origine", "Original", "Original", "Originale"}},
    {{"720p (Original)", "720p (d'origine)", "720p (Original)", "720p (original)", "720p (originale)"}},
    {{"Shorter", "Plus courte", "Kürzer", "Más corta", "Più corta"}},
    {{"1.5x", "1,5x", "1,5x", "1,5x", "1,5x"}},
    {{"30 to 60", "30 à 60", "30 zu 60", "30 a 60", "30 a 60"}},
    {{"60 to 120", "60 à 120", "60 zu 120", "60 a 120", "60 a 120"}},
    {{"FSR 3 (Experimental)", "FSR 3 (expérimental)", "FSR 3 (experimentell)", "FSR 3 (experimental)", "FSR 3 (sperimentale)"}},
    {{"60 Hz", "60\u00A0Hz", "60 Hz", "60 Hz", "60 Hz"}},
    {{"120 Hz", "120\u00A0Hz", "120 Hz", "120 Hz", "120 Hz"}},
    {{"120 Hz with VRR", "120\u00A0Hz avec VRR", "120 Hz mit VRR", "120 Hz con VRR", "120 Hz con VRR"}},
    {{"MSAA 4x + SMAA", "MSAA 4x + SMAA", "MSAA 4x + SMAA", "MSAA 4x + SMAA", "MSAA 4x + SMAA"}},
    {{"High", "Élevée", "Hoch", "Alta", "Alta"}},
    {{"Very High", "Très élevée", "Sehr hoch", "Muy alta", "Molto alta"}},
    {{"Every Frame", "Chaque image", "Jedes Bild", "Cada fotograma", "Ogni fotogramma"}},
    {{"Every Other Frame", "Une image sur deux", "Jedes zweite Bild", "Cada dos fotogramas", "Un fotogramma su due"}},
    {{"After 1 Second", "Après 1\u00A0seconde", "Nach 1 Sekunde", "Tras 1 segundo", "Dopo 1 secondo"}},
    {{"After 2 Seconds", "Après 2\u00A0secondes", "Nach 2 Sekunden", "Tras 2 segundos", "Dopo 2 secondi"}},
    {{"After 3 Seconds", "Après 3\u00A0secondes", "Nach 3 Sekunden", "Tras 3 segundos", "Dopo 3 secondi"}},
    {{"Faster", "Plus rapide", "Schneller", "Más rápida", "Più veloce"}},
    {{"Much Faster", "Bien plus rapide", "Viel schneller", "Mucho más rápida", "Molto più veloce"}},
    {{"Fastest", "La plus rapide", "Am schnellsten", "La más rápida", "La più veloce"}},
    {{"Normal", "Normal", "Normal", "Normal", "Normale"}},
    {{"Detailed", "Détaillé", "Ausführlich", "Detallado", "Dettagliato"}},
    {{"Simple", "Simple", "Einfach", "Simple", "Semplice"}},
    {{"Everything", "Tout", "Alles", "Todo", "Tutto"}},
    {{"Own Cores", "Cœurs dédiés", "Eigene Kerne", "Núcleos propios", "Core dedicati"}},
    {{"Anywhere", "Partout", "Überall", "Cualquiera", "Ovunque"}},
    {{"Compare", "Comparer", "Vergleichen", "Comparar", "Confronta"}},
    {{"Lean", "Allégée", "Schlank", "Ligera", "Leggera"}},
    {{"Classic", "Classique", "Klassisch", "Clásica", "Classica"}},

    // ---- settings: what each does
    {{"The language of the game's text (menus, subtitles and missions) and of this hub. Auto follows the PS5's "
      "language. Voices stay in English, as on the Xbox 360.",
      "La langue des textes du jeu (menus, sous-titres et missions) et de ce hub. Auto suit la langue de la PS5. "
      "Les voix restent en anglais, comme sur Xbox 360.",
      "Die Sprache der Spieltexte (Menüs, Untertitel und Missionen) und dieses Hubs. Auto folgt der Sprache der "
      "PS5. Die Stimmen bleiben auf Englisch, wie auf der Xbox 360.",
      "El idioma de los textos del juego (menús, subtítulos y misiones) y de este hub. Auto sigue el idioma de la "
      "PS5. Las voces siguen en inglés, como en Xbox 360.",
      "La lingua dei testi del gioco (menu, sottotitoli e missioni) e di questo hub. Auto segue la lingua della "
      "PS5. Le voci restano in inglese, come su Xbox 360."}},
    {{"Sets the Display settings below in one go, all with a 60 FPS limit. Recommended is the game as it comes, in 4K. Performance draws at 1440p with fewer cars and people, for the steadiest 60. Quality adds sharper and longer shadows, more detail in the distance, SMAA and the enhanced effects, and can drop below 60 in busy streets. Changing any of them makes it Custom.",
      "Règle d'un coup les réglages d'affichage ci-dessous, tous limités à 60 images par seconde. Recommandé, c'est le jeu tel qu'il est livré, en 4K. Performances affiche en 1440p avec moins de voitures et de passants, pour les 60 images les plus stables. Qualité ajoute des ombres plus nettes et plus lointaines, plus de détails au loin, le SMAA et les effets améliorés, et peut passer sous 60 dans les rues animées. Changer l'un d'eux passe en Personnalisé.",
      "Stellt die Anzeige-Einstellungen darunter auf einmal ein, alle mit 60 Bildern pro Sekunde als Grenze. Empfohlen ist das Spiel, wie es kommt, in 4K. Leistung zeichnet in 1440p mit weniger Autos und Passanten, für die stabilsten 60. Qualität bringt schärfere und weitere Schatten, mehr Details in der Ferne, SMAA und die verbesserten Effekte und kann in belebten Straßen unter 60 fallen. Wer eine davon ändert, hat Eigene.",
      "Ajusta de una vez los ajustes de pantalla de abajo, todos con un límite de 60 FPS. Recomendado es el juego tal como viene, en 4K. Rendimiento dibuja a 1440p con menos coches y gente, para los 60 más estables. Calidad añade sombras más nítidas y lejanas, más detalle a lo lejos, SMAA y los efectos mejorados, y puede bajar de 60 en calles concurridas. Cambiar cualquiera de ellos lo deja en Personalizado.",
      "Imposta in un colpo le impostazioni dello schermo qui sotto, tutte con un limite di 60 FPS. Consigliato è il gioco così com'è, in 4K. Prestazioni disegna a 1440p con meno auto e persone, per i 60 più stabili. Qualità aggiunge ombre più nitide e lontane, più dettagli in lontananza, SMAA e gli effetti migliorati, e può scendere sotto 60 nelle strade affollate. Cambiarne uno lo porta su Personalizzato."}},
    {{"How sharp the game is drawn. 720p is the Xbox 360's. The PS5's graphics card has room for 4K; lower only "
      "helps if the picture stutters.",
      "La netteté de l'image du jeu. 720p est celle de la Xbox 360. La carte graphique de la PS5 a de la "
      "marge pour la 4K ; plus bas n'aide que si l'image saccade.",
      "Wie scharf das Spiel gezeichnet wird. 720p ist die Auflösung der Xbox 360. Die Grafikeinheit der PS5 "
      "schafft 4K; weniger hilft nur, wenn das Bild ruckelt.",
      "La nitidez con la que se dibuja el juego. 720p es la de Xbox 360. La gráfica de PS5 tiene margen para 4K; "
      "bajarla solo ayuda si la imagen da tirones.",
      "La nitidezza con cui viene disegnato il gioco. 720p è quella di Xbox 360. La scheda grafica di PS5 regge "
      "il 4K; abbassarla serve solo se l'immagine scatta."}},
    {{"How far out buildings and the city are drawn in detail. Original is the Xbox 360's. Shorter draws less of "
      "the city and runs faster in busy streets, with more pop-in. Farther costs frame rate, and past 1.5x the "
      "game can run short of memory.",
      "Jusqu'où les bâtiments et la ville sont affichés en détail. D'origine correspond à la Xbox 360. Plus "
      "courte affiche moins de ville et tourne plus vite dans les rues animées, avec plus d'apparitions soudaines. "
      "Plus loin coûte des images par seconde, et au-delà de 1,5x le jeu peut manquer de mémoire.",
      "Wie weit Gebäude und die Stadt detailliert gezeichnet werden. Original entspricht der Xbox 360. Kürzer "
      "zeichnet weniger von der Stadt und läuft in belebten Straßen schneller, mit mehr Pop-in. Weiter kostet "
      "Bildrate, und über 1,5x kann dem Spiel der Speicher ausgehen.",
      "Hasta dónde se dibujan con detalle los edificios y la ciudad. Original es la de Xbox 360. Más corta dibuja "
      "menos ciudad y va más fluida en calles concurridas, con más elementos que aparecen de golpe. Más lejos "
      "cuesta fotogramas, y por encima de 1,5x el juego puede quedarse sin memoria.",
      "Fino a che distanza edifici e città vengono disegnati nel dettaglio. Originale è quella di Xbox 360. Più "
      "corta disegna meno città e va più veloce nelle strade affollate, con più pop-in. Più lontana costa "
      "fotogrammi, e oltre 1,5x il gioco può restare senza memoria."}},
    {{"Shows the frame rate at the left edge of the screen while you play. Simple is frames a second and the "
      "frame time. Detailed adds the slowest frames, a graph of the last ones, the resolution, the TV's refresh "
      "rate, the frame limit and how long the renderer takes.",
      "Affiche la fréquence d'images sur le bord gauche de l'écran pendant que vous jouez. Simple : images par "
      "seconde et durée d'image. Détaillé ajoute les images les plus lentes, un graphique des dernières, la "
      "résolution, la fréquence de la TV, la limite d'images et le temps du rendu.",
      "Zeigt beim Spielen am linken Bildschirmrand die Bildrate. Einfach: Bilder pro Sekunde und Bildzeit. "
      "Ausführlich zeigt dazu die langsamsten Bilder, eine Grafik der letzten, die Auflösung, die Bildwiederholrate "
      "des Fernsehers, die Bildratenbegrenzung und die Zeit des Renderers.",
      "Muestra la tasa de fotogramas en el borde izquierdo de la pantalla mientras juegas. Simple: fotogramas por "
      "segundo y tiempo de fotograma. Detallado añade los fotogramas más lentos, una gráfica de los últimos, la "
      "resolución, la frecuencia de la TV, el límite de fotogramas y el tiempo del renderizador.",
      "Mostra la frequenza dei fotogrammi sul bordo sinistro dello schermo mentre giochi. Semplice: fotogrammi al "
      "secondo e tempo per fotogramma. Dettagliato aggiunge i fotogrammi più lenti, un grafico degli ultimi, la "
      "risoluzione, la frequenza della TV, il limite di fotogrammi e il tempo del renderer."}},
    {{"The most frames a second the game draws. 30 is what the Xbox 360 aimed for and holds steady everywhere; 60 "
      "runs at 50 to 60 in the busiest streets. 40 and 120 are for a 120 Hz Refresh Rate: 40 holds steady and is "
      "smoother than 30; 120 is only reached in quieter places.",
      "Le nombre maximal d'images par seconde. 30 est l'objectif de la Xbox 360 et reste stable partout\u00A0; "
      "60 tourne entre 50 et 60 dans les rues les plus animées. 40 et 120 vont avec une fréquence d'affichage de "
      "120\u00A0Hz\u00A0: 40 reste stable et plus fluide que 30\u00A0; 120 n'est atteint que dans les "
      "endroits calmes.",
      "Die meisten Bilder pro Sekunde, die das Spiel zeichnet. 30 war das Ziel der Xbox 360 und bleibt überall "
      "stabil; 60 läuft in den belebtesten Straßen mit 50 bis 60. 40 und 120 sind für eine Bildwiederholrate von "
      "120 Hz: 40 bleibt stabil und ist flüssiger als 30; 120 wird nur an ruhigeren Orten erreicht.",
      "El máximo de fotogramas por segundo. 30 es lo que buscaba Xbox 360 y se mantiene estable en todas partes; "
      "60 va entre 50 y 60 en las calles más concurridas. 40 y 120 son para una frecuencia de actualización de "
      "120 Hz: 40 se mantiene estable y es más fluido que 30; 120 solo se alcanza en sitios tranquilos.",
      "Il massimo di fotogrammi al secondo. 30 è l'obiettivo di Xbox 360 e resta stabile ovunque; 60 gira tra "
      "50 e 60 nelle strade più affollate. 40 e 120 sono per una frequenza di aggiornamento di 120 Hz: 40 resta "
      "stabile ed è più fluido di 30; 120 si raggiunge solo nei posti più tranquilli."}},
    {{"How many times a second the TV shows a new picture. 120 Hz needs a TV that takes it and 120 Hz output on "
      "in the PS5's settings; without them the game stays at 60 Hz. It's what lets the 40 and 120 FPS limits run "
      "evenly. It switches as soon as you choose it.",
      "Combien de fois par seconde la TV affiche une nouvelle image. 120\u00A0Hz demande une TV compatible et la "
      "sortie 120\u00A0Hz activée dans les paramètres de la PS5\u00A0; sinon le jeu reste à 60\u00A0Hz. C'est "
      "ce qui rend régulières les limites de 40 et 120 images par seconde. Le changement se fait dès que vous le "
      "choisissez.",
      "Wie oft pro Sekunde der Fernseher ein neues Bild zeigt. 120 Hz braucht einen Fernseher, der es kann, und "
      "die 120-Hz-Ausgabe in den Einstellungen der PS5; sonst bleibt das Spiel bei 60 Hz. Erst damit laufen die "
      "Limits 40 und 120 FPS gleichmäßig. Wechselt sofort, wenn du es auswählst.",
      "Cuántas veces por segundo la tele muestra una imagen nueva. 120 Hz necesita una tele compatible y la salida "
      "a 120 Hz activada en los ajustes de la PS5; si no, el juego se queda en 60 Hz. Es lo que hace que los "
      "límites de 40 y 120 FPS vayan regulares. Cambia en cuanto lo eliges.",
      "Quante volte al secondo la TV mostra un'immagine nuova. 120 Hz richiede una TV compatibile e l'uscita a "
      "120 Hz attiva nelle impostazioni della PS5; altrimenti il gioco resta a 60 Hz. È ciò che rende regolari i "
      "limiti di 40 e 120 FPS. Cambia appena lo scegli."}},
    {{"Switch to 120 Hz?", "Passer à 120\u00A0Hz\u00A0?", "Auf 120 Hz umschalten?", "¿Cambiar a 120 Hz?",
      "Passare a 120 Hz?"}},
    {{"The screen goes dark for a moment while the TV changes over.",
      "L'écran devient noir un instant pendant que la TV change de mode.",
      "Der Bildschirm wird kurz schwarz, während der Fernseher umschaltet.",
      "La pantalla se queda en negro un momento mientras la tele cambia.",
      "Lo schermo diventa nero per un attimo mentre la TV cambia modalità."}},
    {{"Switch", "Changer", "Umschalten", "Cambiar", "Cambia"}},
    {{"Later", "Plus tard", "Später", "Más tarde", "Più tardi"}},
    {{"120 Hz from the next start", "120\u00A0Hz au prochain lancement", "120 Hz ab dem nächsten Start",
      "120 Hz desde el próximo inicio", "120 Hz dal prossimo avvio"}},
    {{"Now at 120 Hz", "Maintenant à 120\u00A0Hz", "Jetzt mit 120 Hz", "Ahora a 120 Hz", "Ora a 120 Hz"}},
    {{"Back to 60 Hz", "Retour à 60\u00A0Hz", "Zurück auf 60 Hz", "De vuelta a 60 Hz", "Di nuovo a 60 Hz"}},
    {{"Still at 60 Hz", "Toujours à 60\u00A0Hz", "Weiter bei 60 Hz", "Sigue a 60 Hz", "Ancora a 60 Hz"}},
    {{"Right now: {}", "En ce moment\u00A0: {}", "Gerade: {}", "Ahora mismo: {}", "In questo momento: {}"}},
    {{"The TV didn't take 120 Hz. Check that 120 Hz output is on in the PS5's settings (Screen and Video, Video "
      "Output) and that the TV runs 120 Hz at the resolution the PS5 sends it. 4K at 120 Hz needs HDMI 2.1.",
      "La TV n'a pas accepté le 120\u00A0Hz. Vérifiez que la sortie 120\u00A0Hz est activée dans les paramètres "
      "de la PS5 (Écran et vidéo, Sortie vidéo) et que la TV accepte le 120\u00A0Hz dans la résolution envoyée "
      "par la PS5. La 4K à 120\u00A0Hz demande le HDMI 2.1.",
      "Der Fernseher hat 120 Hz nicht angenommen. Prüf, ob die 120-Hz-Ausgabe in den Einstellungen der PS5 an ist "
      "(Bildschirm und Video, Videoausgabe) und ob der Fernseher 120 Hz in der Auflösung kann, die die PS5 "
      "schickt. 4K mit 120 Hz braucht HDMI 2.1.",
      "La tele no ha aceptado los 120 Hz. Comprueba que la salida a 120 Hz está activada en los ajustes de la PS5 "
      "(Pantalla y vídeo, Salida de vídeo) y que la tele admite 120 Hz en la resolución que le manda la PS5. 4K a "
      "120 Hz necesita HDMI 2.1.",
      "La TV non ha accettato i 120 Hz. Controlla che l'uscita a 120 Hz sia attiva nelle impostazioni della PS5 "
      "(Schermo e video, Uscita video) e che la TV regga i 120 Hz alla risoluzione che le manda la PS5. Il 4K a "
      "120 Hz richiede l'HDMI 2.1."}},
    {{"Shows buildings, cars and people at their most detailed as soon as that detail has loaded, near or far, "
      "instead of simpler versions in the distance. Can cost frame rate in busy streets.",
      "Affiche bâtiments, voitures et passants dans leur version la plus détaillée dès qu'elle est chargée, de "
      "près comme de loin, au lieu de versions simplifiées au loin. Peut coûter des images par seconde dans les "
      "rues animées.",
      "Zeigt Gebäude, Autos und Menschen in ihrer detailliertesten Form, sobald diese geladen ist, nah wie fern, "
      "statt einfacherer Versionen in der Ferne. Kann in belebten Straßen Bildrate kosten.",
      "Muestra edificios, coches y gente con su máximo detalle en cuanto ese detalle se ha cargado, cerca o "
      "lejos, en vez de versiones más simples a distancia. Puede costar fotogramas en calles concurridas.",
      "Mostra edifici, auto e persone al massimo dettaglio appena quel dettaglio è caricato, vicino o lontano, "
      "invece delle versioni più semplici in lontananza. Può costare fotogrammi nelle strade affollate."}},
    {{"AMD FSR 3 adds a frame between each two the game draws. 30 to 60 holds the game at 30 FPS and shows 60, on any TV. 60 to 120 needs a 120 Hz TV and switches to it: the game runs at 60 and shows 120, and drops to 30 to 60 by itself in places it can't hold 60. Frame Limit doesn't apply while it's on. Adds a little input lag and turns on FSR 3 anti-aliasing. Experimental: tell us how it feels.",
      "AMD FSR 3 ajoute une image entre chaque paire d'images du jeu. 30 à 60 tient le jeu à 30\u00A0images par seconde et en affiche 60, sur n'importe quelle TV. 60 à 120 demande une TV 120\u00A0Hz et y passe\u00A0: le jeu tourne à 60 et en affiche 120, et repasse de lui-même en 30 à 60 là où il ne tient pas 60. La limite d'images ne s'applique pas tant qu'elle est active. Ajoute un peu de latence et active l'anticrénelage FSR 3. Expérimental\u00A0: dites-nous ce que vous en pensez.",
      "AMD FSR 3 fügt zwischen je zwei gezeichneten Bildern eines ein. 30 zu 60 hält das Spiel bei 30 Bildern pro Sekunde und zeigt 60, an jedem Fernseher. 60 zu 120 braucht einen 120-Hz-Fernseher und schaltet auf 120 Hz: Das Spiel läuft mit 60 und zeigt 120 und wechselt dort, wo es keine 60 hält, von selbst auf 30 zu 60. Die Bildratenbegrenzung gilt dann nicht. Etwas mehr Eingabeverzögerung; schaltet auch FSR-3-Kantenglättung ein. Experimentell: Sag uns, wie es sich anfühlt.",
      "AMD FSR 3 añade un fotograma entre cada dos que dibuja el juego. 30 a 60 mantiene el juego a 30 FPS y muestra 60, en cualquier tele. 60 a 120 necesita una tele de 120 Hz y cambia a ella: el juego va a 60 y muestra 120, y pasa solo a 30 a 60 donde no aguanta 60. El límite de fotogramas no se aplica mientras está activa. Añade un poco de retraso de entrada y activa el antialiasing FSR 3. Experimental: cuéntanos qué tal va.",
      "AMD FSR 3 aggiunge un fotogramma tra ogni due disegnati dal gioco. 30 a 60 tiene il gioco a 30 FPS e ne mostra 60, su qualsiasi TV. 60 a 120 richiede una TV a 120 Hz e ci passa: il gioco gira a 60 e ne mostra 120, e torna da solo a 30 a 60 dove non tiene i 60. Il limite fotogrammi non vale mentre è attiva. Aggiunge un po' di ritardo nei comandi e attiva l'antialiasing FSR 3. Sperimentale: facci sapere com'è."}},
    {{"Smooths jagged edges. FXAA is the lightest; SMAA is sharper and costs a little more. MSAA 4x + SMAA also "
      "cleans up thin edges in the distance, like stairs and wires, but is heavy on the graphics card: new on the "
      "PS5, try 1440p with it if the game slows down.",
      "Adoucit les bords crénelés. FXAA est le plus léger\u00A0; SMAA est plus net et coûte un peu plus. "
      "MSAA 4x + SMAA lisse aussi les bords fins au loin, comme les escaliers et les câbles, mais pèse lourd sur "
      "la carte graphique\u00A0: nouveau sur PS5, essayez-le en 1440p si le jeu ralentit.",
      "Glättet gezackte Kanten. FXAA ist am leichtesten; SMAA ist schärfer und kostet etwas mehr. MSAA 4x + SMAA "
      "glättet auch dünne Kanten in der Ferne, etwa Treppen und Kabel, fordert die Grafikeinheit aber stark: neu "
      "auf der PS5, probier dazu 1440p, wenn das Spiel langsamer wird.",
      "Suaviza los bordes dentados. FXAA es el más ligero; SMAA es más nítido y cuesta un poco más. MSAA 4x + SMAA "
      "también limpia los bordes finos a lo lejos, como escaleras y cables, pero exige mucho a la gráfica: es "
      "nuevo en PS5, pruébalo a 1440p si el juego se ralentiza.",
      "Ammorbidisce i bordi seghettati. FXAA è il più leggero; SMAA è più nitido e costa un po' di più. "
      "MSAA 4x + SMAA pulisce anche i bordi sottili in lontananza, come scale e cavi, ma pesa molto sulla scheda "
      "grafica: è nuovo su PS5, provalo a 1440p se il gioco rallenta."}},
    {{"Keeps textures seen at an angle sharp, like roads and pavements going into the distance. Original is the "
      "Xbox 360's; 16x is the sharpest and costs very little.",
      "Garde nettes les textures vues en biais, comme les routes et les trottoirs qui partent au loin. D'origine "
      "correspond à la Xbox 360\u00A0; 16x est le plus net et ne coûte presque rien.",
      "Hält schräg gesehene Texturen scharf, etwa Straßen und Gehwege, die in die Ferne laufen. Original entspricht "
      "der Xbox 360; 16x ist am schärfsten und kostet sehr wenig.",
      "Mantiene nítidas las texturas vistas en ángulo, como carreteras y aceras que se pierden a lo lejos. "
      "Original es el de Xbox 360; 16x es el más nítido y apenas cuesta.",
      "Mantiene nitide le texture viste di sbieco, come strade e marciapiedi che si perdono in lontananza. "
      "Originale è quello di Xbox 360; 16x è il più nitido e costa pochissimo."}},
    {{"The shaders from FusionFix, the PC mod: depth of field, a better sky, bloom and fog, and nicer water and "
      "light beams. Off is the Xbox 360's look. New on the PS5: tell us if something looks wrong.",
      "Les shaders de FusionFix, le mod PC\u00A0: profondeur de champ, un ciel, un bloom et un brouillard plus "
      "réussis, une eau et des faisceaux lumineux plus beaux. Désactivé garde le rendu de la Xbox 360. Nouveau "
      "sur PS5\u00A0: dites-nous si quelque chose cloche.",
      "Die Shader aus FusionFix, der PC-Mod: Tiefenschärfe, ein besserer Himmel, Bloom und Nebel sowie schöneres "
      "Wasser und Lichtstrahlen. Aus ist der Look der Xbox 360. Neu auf der PS5: Sag uns, wenn etwas falsch "
      "aussieht.",
      "Los shaders de FusionFix, el mod de PC: profundidad de campo, mejor cielo, bloom y niebla, y agua y haces "
      "de luz más bonitos. Desactivado es el aspecto de Xbox 360. Es nuevo en PS5: avísanos si algo se ve mal.",
      "Gli shader di FusionFix, la mod per PC: profondità di campo, cielo, bloom e nebbia migliori, acqua e fasci "
      "di luce più belli. Disattivato è l'aspetto di Xbox 360. È nuovo su PS5: dicci se qualcosa non va."}},
    {{"How sharp shadows are. High is twice the Xbox 360's detail; Very High doubles it again and asks more of the "
      "graphics card.",
      "La netteté des ombres. Élevée offre deux fois le détail de la Xbox 360\u00A0; Très élevée le double "
      "encore et demande plus à la carte graphique.",
      "Wie scharf Schatten sind. Hoch hat doppelt so viel Detail wie die Xbox 360; Sehr hoch verdoppelt es noch "
      "einmal und fordert die Grafikeinheit stärker.",
      "La nitidez de las sombras. Alta tiene el doble de detalle que Xbox 360; Muy alta lo vuelve a duplicar y "
      "exige más a la gráfica.",
      "La nitidezza delle ombre. Alta ha il doppio del dettaglio di Xbox 360; Molto alta lo raddoppia ancora e "
      "chiede di più alla scheda grafica."}},
    {{"The blur when driving fast or turning quickly.",
      "Le flou quand on roule vite ou qu'on tourne rapidement.",
      "Die Unschärfe bei schneller Fahrt oder schnellen Drehungen.",
      "El desenfoque al conducir rápido o girar de golpe.",
      "La sfocatura quando guidi veloce o giri rapidamente."}},
    {{"How sharp water, mirror and car reflections are. Original is the Xbox 360's; 1080p is cleaner.",
      "La netteté des reflets de l'eau, des miroirs et des voitures. D'origine correspond à la Xbox 360 ; "
      "1080p est plus propre.",
      "Wie scharf Spiegelungen auf Wasser, in Spiegeln und auf Autos sind. Original entspricht der Xbox 360; "
      "1080p ist sauberer.",
      "La nitidez de los reflejos del agua, los espejos y los coches. Original es la de Xbox 360; 1080p se ve más "
      "limpio.",
      "La nitidezza dei riflessi su acqua, specchi e auto. Originale è quella di Xbox 360; 1080p è più pulito."}},
    {{"How far out shadows are drawn. Original is the Xbox 360's; 2x reaches further but costs frame rate in the "
      "city.",
      "Jusqu'où les ombres sont affichées. D'origine correspond à la Xbox 360 ; 2x va plus loin mais "
      "coûte des images par seconde en ville.",
      "Wie weit Schatten gezeichnet werden. Original entspricht der Xbox 360; 2x reicht weiter, kostet in der "
      "Stadt aber Bildrate.",
      "Hasta dónde se dibujan las sombras. Original es la de Xbox 360; 2x llega más lejos pero cuesta fotogramas "
      "en la ciudad.",
      "Fino a che distanza vengono disegnate le ombre. Originale è quella di Xbox 360; 2x arriva più lontano ma "
      "costa fotogrammi in città."}},
    {{"Shadows of small things like signs, bins and debris, about a fifth of what is drawn in busy streets. Off "
      "skips them for a higher frame rate.",
      "Les ombres des petites choses comme les panneaux, les poubelles et les débris, environ un cinquième de ce "
      "qui est affiché dans les rues animées. Désactivé les ignore pour plus d'images par seconde.",
      "Schatten kleiner Dinge wie Schilder, Mülltonnen und Trümmer, etwa ein Fünftel dessen, was in belebten "
      "Straßen gezeichnet wird. Aus lässt sie weg, für eine höhere Bildrate.",
      "Las sombras de cosas pequeñas como señales, papeleras y escombros, cerca de una quinta parte de lo que se "
      "dibuja en calles concurridas. Desactivado las omite para ganar fotogramas.",
      "Le ombre di piccoli oggetti come cartelli, bidoni e detriti, circa un quinto di ciò che viene disegnato "
      "nelle strade affollate. Disattivato le salta per più fotogrammi al secondo."}},
    {{"How often the reflections on cars and glass are redrawn. Every other frame looks the same in motion and "
      "leaves more time for the rest of the frame.",
      "La fréquence à laquelle les reflets sur les voitures et les vitres sont redessinés. Une image sur deux "
      "paraît identique en mouvement et laisse plus de temps au reste de l'image.",
      "Wie oft die Spiegelungen auf Autos und Glas neu gezeichnet werden. Jedes zweite Bild sieht in Bewegung "
      "gleich aus und lässt dem Rest des Bildes mehr Zeit.",
      "Cada cuánto se redibujan los reflejos en coches y cristales. Cada dos fotogramas se ve igual en movimiento "
      "y deja más tiempo para el resto del fotograma.",
      "Ogni quanto vengono ridisegnati i riflessi su auto e vetri. Un fotogramma su due sembra uguale in "
      "movimento e lascia più tempo al resto del fotogramma."}},
    {{"How often the reflection on water is redrawn. Every other frame leaves more time for the rest of the frame; "
      "when the camera turns quickly the reflection can trail a frame behind.",
      "La fréquence à laquelle le reflet sur l'eau est redessiné. Une image sur deux laisse plus de temps au reste "
      "de l'image\u00A0; quand la caméra tourne vite, le reflet peut avoir une image de retard.",
      "Wie oft die Spiegelung auf dem Wasser neu gezeichnet wird. Jedes zweite Bild lässt dem Rest des Bildes mehr "
      "Zeit; dreht sich die Kamera schnell, kann die Spiegelung ein Bild hinterherhängen.",
      "Cada cuánto se redibuja el reflejo del agua. Cada dos fotogramas deja más tiempo para el resto del "
      "fotograma; si la cámara gira deprisa, el reflejo puede ir un fotograma por detrás.",
      "Ogni quanto viene ridisegnato il riflesso sull'acqua. Un fotogramma su due lascia più tempo al resto del "
      "fotogramma; quando la telecamera gira in fretta il riflesso può restare indietro di un fotogramma."}},
    {{"The grainy, film-like look of The Lost and Damned. Off gives it a clean picture; GTA IV and The Ballad of "
      "Gay Tony have none.",
      "L'aspect granuleux façon pellicule de The Lost and Damned. Désactivé, l'image est nette\u00A0; GTA IV et "
      "The Ballad of Gay Tony n'en ont pas.",
      "Der körnige Filmlook von The Lost and Damned. Aus ergibt ein klares Bild; GTA IV und The Ballad of Gay "
      "Tony haben keinen.",
      "El aspecto granulado de película de The Lost and Damned. Desactivado deja la imagen limpia; GTA IV y The "
      "Ballad of Gay Tony no lo tienen.",
      "L'aspetto granuloso da pellicola di The Lost and Damned. Disattivato dà un'immagine pulita; GTA IV e The "
      "Ballad of Gay Tony non ce l'hanno."}},
    {{"How long the car camera stays where you pointed it with the right stick before it swings back behind the "
      "car. Original is the Xbox 360's: it starts swinging back right away.",
      "Combien de temps la caméra de véhicule reste là où vous l'avez tournée avec le stick droit avant de revenir "
      "derrière le véhicule. D'origine, c'est celle de la Xbox 360\u00A0: elle revient tout de suite.",
      "Wie lange die Fahrzeugkamera dort bleibt, wohin du sie mit dem rechten Stick gedreht hast, bevor sie hinter "
      "das Fahrzeug zurückschwenkt. Original entspricht der Xbox 360: Sie schwenkt sofort zurück.",
      "Cuánto tiempo se queda la cámara de vehículo donde la apuntaste con el stick derecho antes de volver detrás "
      "del vehículo. Original es la de Xbox 360: empieza a volver enseguida.",
      "Quanto resta la telecamera veicolo dove l'hai girata con la levetta destra prima di tornare dietro al "
      "veicolo. Originale è quella di Xbox 360: inizia a tornare subito."}},
    {{"How quickly the car camera swings round to follow the car through turns and back behind it. Original is "
      "the Xbox 360's.",
      "La vitesse à laquelle la caméra de véhicule pivote pour suivre le véhicule dans les virages et revenir "
      "derrière lui. D'origine, c'est celle de la Xbox 360.",
      "Wie schnell die Fahrzeugkamera dem Fahrzeug durch Kurven folgt und wieder hinter es schwenkt. Original "
      "entspricht der Xbox 360.",
      "La rapidez con la que la cámara de vehículo gira para seguir al vehículo en las curvas y volver detrás de "
      "él. Original es la de Xbox 360.",
      "Quanto velocemente la telecamera veicolo gira per seguire il veicolo in curva e tornargli dietro. "
      "Originale è quella di Xbox 360."}},
    {{"Puts the camera straight behind the car instead of a little off to the side, like FusionFix's option on "
      "PC. Off is the Xbox 360's.",
      "Place la caméra juste derrière le véhicule au lieu d'un peu sur le côté, comme l'option de FusionFix sur "
      "PC. Désactivé, c'est celle de la Xbox 360.",
      "Setzt die Kamera direkt hinter das Fahrzeug statt etwas zur Seite, wie die Option von FusionFix am PC. "
      "Aus entspricht der Xbox 360.",
      "Coloca la cámara justo detrás del vehículo en vez de un poco a un lado, como la opción de FusionFix en PC. "
      "Desactivada es la de Xbox 360.",
      "Mette la telecamera proprio dietro al veicolo invece che un po' di lato, come l'opzione di FusionFix su PC. "
      "Disattivata è quella di Xbox 360."}},
    {{"Goes straight to the game's start screen, past the logos and the legal screens.",
      "Va directement à l'écran titre du jeu, sans les logos ni les mentions légales.",
      "Geht direkt zum Startbildschirm des Spiels, vorbei an den Logos und Rechtshinweisen.",
      "Va directo a la pantalla de inicio del juego, sin los logotipos ni los avisos legales.",
      "Va direttamente alla schermata iniziale del gioco, saltando loghi e note legali."}},
    {{"Adds the game's trophies to the PS5's own Trophies list, with the ones you've already earned, and shows "
      "the PS5's trophy pop-ups as you earn them. Needs a payload loader (etaHEN, elfldr...).",
      "Ajoute les trophées du jeu à la liste Trophées de la PS5, avec ceux que vous avez déjà obtenus, et affiche "
      "les notifications de trophée de la PS5 quand vous les obtenez. Nécessite un chargeur de payloads (etaHEN, "
      "elfldr...).",
      "Fügt die Trophäen des Spiels der Trophäenliste der PS5 hinzu, samt den bereits verdienten, und zeigt die "
      "Trophäen-Meldungen der PS5, sobald du sie verdienst. Benötigt einen Payload-Loader (etaHEN, elfldr...).",
      "Añade los trofeos del juego a la lista de Trofeos de la PS5, con los que ya hayas conseguido, y muestra los "
      "avisos de trofeo de la PS5 al conseguirlos. Necesita un cargador de payloads (etaHEN, elfldr...).",
      "Aggiunge i trofei del gioco all'elenco Trofei della PS5, con quelli già ottenuti, e mostra le notifiche dei "
      "trofei della PS5 quando li ottieni. Richiede un caricatore di payload (etaHEN, elfldr...)."}},
    {{"Turns the controller extras below on or off together: the light bar, adaptive triggers and extra rumble.",
      "Active ou coupe d'un coup les fonctions de la manette ci-dessous\u00A0: barre lumineuse, gâchettes "
      "adaptatives et vibrations supplémentaires.",
      "Schaltet die Controller-Extras darunter gemeinsam an oder aus: Lichtleiste, adaptive Trigger und zusätzliche "
      "Vibration.",
      "Activa o desactiva a la vez los extras del mando de abajo: la barra de luz, los gatillos adaptativos y la "
      "vibración extra.",
      "Attiva o disattiva insieme le funzioni del controller qui sotto: barra luminosa, grilletti adattivi e "
      "vibrazione extra."}},
    {{"The controller's light: blue in GTA IV, and flashing red and blue like police lights while you're wanted.",
      "La lumière de la manette\u00A0: bleue dans GTA IV, et rouge et bleu clignotants comme un gyrophare quand "
      "vous êtes recherché.",
      "Das Licht des Controllers: blau in GTA IV und rot-blau blinkend wie ein Blaulicht, solange du gesucht wirst.",
      "La luz del mando: azul en GTA IV, y roja y azul intermitente como las de la policía mientras te buscan.",
      "La luce del controller: blu in GTA IV, e rossa e blu lampeggiante come una sirena della polizia quando sei "
      "ricercato."}},
    {{"R2 pulls like a real gun's trigger and kicks with each shot, L2 pushes back while you aim, and both feel "
      "like pedals when you drive.",
      "R2 se presse comme la détente d'une vraie arme et recule à chaque tir, L2 résiste quand vous visez, et les "
      "deux font office de pédales en voiture.",
      "R2 zieht sich wie der Abzug einer echten Waffe und schlägt bei jedem Schuss zurück, L2 drückt beim Zielen "
      "dagegen, und beim Fahren fühlen sich beide wie Pedale an.",
      "R2 se aprieta como el gatillo de un arma real y retrocede con cada disparo, L2 opone resistencia al apuntar, "
      "y al conducir los dos parecen pedales.",
      "R2 si preme come il grilletto di un'arma vera e rincula a ogni colpo, L2 oppone resistenza quando miri, e "
      "alla guida entrambi sembrano pedali."}},
    {{"Adds rumble when you shoot, get hit or are near an explosion, on top of the game's own.",
      "Ajoute des vibrations quand vous tirez, êtes touché ou près d'une explosion, en plus de celles du jeu.",
      "Fügt Vibration hinzu, wenn du schießt, getroffen wirst oder eine Explosion in der Nähe ist, zusätzlich zur "
      "eigenen des Spiels.",
      "Añade vibración al disparar, recibir un impacto o estar cerca de una explosión, además de la del propio "
      "juego.",
      "Aggiunge vibrazione quando spari, vieni colpito o sei vicino a un'esplosione, oltre a quella del gioco."}},
    {{"How far the sound runs behind the picture. Lower is closer to the action; if the sound crackles or cuts "
      "out, pick the next one up. Wireless headsets add a little delay of their own.",
      "Le retard du son sur l'image. Plus bas, le son colle mieux à l'action\u00A0; s'il grésille ou se coupe, "
      "prenez le réglage suivant. Les casques sans fil ajoutent leur propre petit délai.",
      "Wie weit der Ton dem Bild hinterherläuft. Niedriger ist näher am Geschehen; wenn der Ton knistert oder "
      "aussetzt, nimm die nächsthöhere Stufe. Kabellose Headsets fügen selbst etwas Verzögerung hinzu.",
      "Cuánto va el sonido por detrás de la imagen. Más bajo se acerca más a la acción; si el sonido chisporrotea "
      "o se corta, elige el siguiente. Los auriculares inalámbricos añaden un poco de retraso propio.",
      "Quanto il suono è in ritardo rispetto all'immagine. Più basso è più vicino all'azione; se l'audio gracchia "
      "o si interrompe, scegli il livello successivo. Le cuffie wireless aggiungono un po' di ritardo proprio."}},
    {{"How strong the adaptive triggers and the extra rumble feel.",
      "La force des gâchettes adaptatives et des vibrations supplémentaires.",
      "Wie stark sich die adaptiven Trigger und die zusätzliche Vibration anfühlen.",
      "La fuerza de los gatillos adaptativos y de la vibración extra.",
      "Quanto sono forti i grilletti adattivi e la vibrazione extra."}},
    {{"How loud the music is here in the hub. It changes right away.",
      "Le volume de la musique ici, dans le hub. Le changement est immédiat.",
      "Wie laut die Musik hier im Hub ist. Ändert sich sofort.",
      "El volumen de la música aquí en el hub. Cambia al momento.",
      "Il volume della musica qui nell'hub. Cambia subito."}},
    {{"Writes what the game does to /app0/logs while it runs. Send these with a bug report. Takes effect the next "
      "time the app starts.",
      "Écrit ce que fait le jeu dans /app0/logs pendant qu'il tourne. Joignez ces fichiers à un rapport de bug. "
      "Prend effet au prochain lancement de l'application.",
      "Schreibt mit, was das Spiel tut, nach /app0/logs. Schick diese Dateien mit einem Fehlerbericht mit. Wirkt "
      "ab dem nächsten Start der App.",
      "Escribe lo que hace el juego en /app0/logs mientras funciona. Envíalos con cada informe de error. Se "
      "aplica la próxima vez que se abra la aplicación.",
      "Scrive ciò che fa il gioco in /app0/logs mentre è in esecuzione. Inviali con una segnalazione di bug. Ha "
      "effetto al prossimo avvio dell'app."}},
    {{"How much goes into the log. Normal is enough for most reports; Detailed and Everything help with crashes "
      "and freezes but make big files and can slow the game.",
      "Quantité d'informations dans le journal. Normal suffit pour la plupart des rapports ; Détaillé et Tout "
      "aident pour les plantages et les blocages mais créent de gros fichiers et peuvent ralentir le jeu.",
      "Wie viel ins Log kommt. Normal reicht für die meisten Berichte; Ausführlich und Alles helfen bei Abstürzen "
      "und Hängern, erzeugen aber große Dateien und können das Spiel bremsen.",
      "Cuánto se escribe en el registro. Normal basta para la mayoría de informes; Detallado y Todo ayudan con "
      "cuelgues y bloqueos, pero crean archivos grandes y pueden ralentizar el juego.",
      "Quanto finisce nel log. Normale basta per la maggior parte delle segnalazioni; Dettagliato e Tutto "
      "aiutano con crash e blocchi ma creano file grandi e possono rallentare il gioco."}},
    {{"Own Cores gives the game's render thread and the two renderer threads a CPU core each and moves everything "
      "else off them (about 10% faster than Anywhere). Compare switches between two versions of Own Cores every 10 "
      "seconds and notes each in the log.",
      "Cœurs dédiés donne au thread de rendu du jeu et aux deux threads du moteur de rendu un cœur de processeur "
      "chacun et en écarte tout le reste (environ 10 % plus rapide que Partout). Comparer alterne entre deux "
      "versions de Cœurs dédiés toutes les 10 secondes et note chacune dans le journal.",
      "Eigene Kerne gibt dem Render-Thread des Spiels und den beiden Renderer-Threads je einen CPU-Kern und hält "
      "alles andere davon fern (etwa 10 % schneller als Überall). Vergleichen wechselt alle 10 Sekunden zwischen "
      "zwei Varianten von Eigene Kerne und vermerkt jede im Log.",
      "Núcleos propios da al hilo de renderizado del juego y a los dos hilos del renderizador un núcleo de CPU a "
      "cada uno y aparta todo lo demás (alrededor de un 10 % más rápido que Cualquiera). Comparar alterna cada "
      "10 segundos entre dos versiones de Núcleos propios y anota cada una en el registro.",
      "Core dedicati dà al thread di rendering del gioco e ai due thread del renderer un core della CPU ciascuno e "
      "sposta tutto il resto altrove (circa il 10% più veloce di Ovunque). Confronta alterna ogni 10 secondi due "
      "versioni di Core dedicati e annota ciascuna nel log."}},
    {{"How the game's render thread hands each draw to the renderer. Lean holds every buffer and texture once for "
      "two frames instead of once per draw and fetches what a capture reads ahead of time. Same picture either way. "
      "Compare stays on Lean and switches its fetching ahead off and on every 10 seconds, noting each in the log.",
      "Comment le thread de rendu du jeu transmet chaque dessin au moteur de rendu. Allégée retient chaque tampon "
      "et texture une fois pour deux images au lieu d'une fois par dessin, et charge à l'avance ce que la capture "
      "va lire. Même image dans les deux cas. Comparer reste sur Allégée et active ou coupe ce chargement anticipé "
      "toutes les 10 secondes, en le notant dans le journal.",
      "Wie der Render-Thread des Spiels jeden Zeichenaufruf an den Renderer übergibt. Schlank hält jeden Puffer "
      "und jede Textur einmal für zwei Bilder statt einmal pro Zeichenaufruf und lädt vorab, was die Erfassung "
      "liest. Das Bild ist in beiden Fällen gleich. Vergleichen bleibt bei Schlank und schaltet das Vorabladen "
      "alle 10 Sekunden aus und ein, jeweils mit Vermerk im Log.",
      "Cómo pasa el hilo de renderizado del juego cada dibujo al renderizador. Ligera retiene cada búfer y textura "
      "una vez cada dos fotogramas en lugar de una vez por dibujo, y adelanta la lectura de lo que va a capturar. "
      "La imagen es la misma en ambos casos. Comparar se queda en Ligera y activa y desactiva esa lectura "
      "anticipada cada 10 segundos, anotándolo en el registro.",
      "Come il thread di rendering del gioco passa ogni disegno al renderer. Leggera trattiene ogni buffer e "
      "texture una volta ogni due fotogrammi invece che a ogni disegno e legge in anticipo ciò che la cattura "
      "userà. L'immagine è la stessa in entrambi i casi. Confronta resta su Leggera e attiva e disattiva la "
      "lettura anticipata ogni 10 secondi, annotandolo nel log."}},
    {{"Records every frame's time to a CSV file in /app0/logs, for checking stutter and slowdowns.",
      "Enregistre le temps de chaque image dans un fichier CSV dans /app0/logs, pour étudier les saccades et les "
      "ralentissements.",
      "Zeichnet die Zeit jedes Bildes in einer CSV-Datei in /app0/logs auf, um Ruckler und Einbrüche zu prüfen.",
      "Guarda el tiempo de cada fotograma en un archivo CSV en /app0/logs, para revisar tirones y bajones.",
      "Registra il tempo di ogni fotogramma in un file CSV in /app0/logs, per controllare scatti e "
      "rallentamenti."}},
    {{"When the game stops drawing for a few seconds, writes where every thread is to the log.",
      "Quand le jeu cesse d'afficher pendant quelques secondes, écrit dans le journal où en est chaque thread.",
      "Wenn das Spiel einige Sekunden lang nichts mehr zeichnet, schreibt es ins Log, wo jeder Thread steht.",
      "Si el juego deja de dibujar durante unos segundos, escribe en el registro dónde está cada hilo.",
      "Quando il gioco smette di disegnare per qualche secondo, scrive nel log dove si trova ogni thread."}},
    {{"Unpacks the game's textures on the graphics card. Off does it on the CPU the old way: slower, but a test "
      "for broken or missing textures.",
      "Décompresse les textures du jeu sur la carte graphique. Désactivé le fait sur le processeur comme "
      "avant : plus lent, mais utile pour tester des textures cassées ou manquantes.",
      "Entpackt die Texturen des Spiels auf der Grafikeinheit. Aus macht es wie früher auf der CPU: langsamer, "
      "aber ein Test bei kaputten oder fehlenden Texturen.",
      "Desempaqueta las texturas del juego en la gráfica. Desactivado lo hace en la CPU como antes: más lento, "
      "pero sirve para probar texturas rotas o que faltan.",
      "Decomprime le texture del gioco sulla scheda grafica. Disattivato lo fa sulla CPU come prima: più lento, "
      "ma utile per verificare texture rotte o mancanti."}},
    {{"Logs when each stage of every frame finished, to find what holds the frame rate back. The log grows "
      "quickly with it on.",
      "Note quand chaque étape de chaque image se termine, pour trouver ce qui limite les images par seconde. Le "
      "journal grossit vite quand c'est activé.",
      "Protokolliert, wann jede Stufe jedes Bildes fertig war, um zu finden, was die Bildrate bremst. Das Log "
      "wächst damit schnell.",
      "Anota cuándo termina cada etapa de cada fotograma, para encontrar qué frena los fotogramas. El registro "
      "crece rápido si está activado.",
      "Annota quando finisce ogni fase di ogni fotogramma, per capire cosa frena il frame rate. Con questa "
      "opzione il log cresce in fretta."}},
    {{"How the game's frames reach the TV. As Finished sends each frame as soon as it's ready. Every Vblank "
      "redraws the screen at every refresh, as Beta 6 did, with two refreshes more input lag. For comparing "
      "which feels smoother.",
      "Comment les images du jeu arrivent à la TV. Dès qu'elles sont prêtes envoie chaque image aussitôt "
      "terminée. À chaque rafraîchissement redessine l'écran à chaque rafraîchissement, comme la bêta 6, avec "
      "deux rafraîchissements de latence en plus. Pour comparer ce qui paraît le plus fluide.",
      "Wie die Bilder des Spiels zum Fernseher kommen. Sobald fertig schickt jedes Bild, sobald es bereit ist. "
      "Bei jedem Vblank zeichnet den Bildschirm bei jeder Bildwiederholung neu, wie Beta 6, mit zwei "
      "Bildwiederholungen mehr Eingabeverzögerung. Zum Vergleichen, was flüssiger wirkt.",
      "Cómo llegan los fotogramas del juego a la TV. Al terminar envía cada fotograma en cuanto está listo. En "
      "cada refresco redibuja la pantalla en cada refresco, como la beta 6, con dos refrescos más de retraso de "
      "entrada. Para comparar cuál se siente más fluido.",
      "Come arrivano alla TV i fotogrammi del gioco. Appena pronti invia ogni fotogramma non appena è finito. A "
      "ogni refresh ridisegna lo schermo a ogni refresh, come la beta 6, con due refresh in più di input lag. Per "
      "confrontare quale sembra più fluido."}},
    {{"Re-reads textures that change in memory without the game saying so. Off brings back the wrong map and "
      "radar tiles. Only for comparing performance.",
      "Relit les textures qui changent en mémoire sans que le jeu le signale. Désactivé fait revenir les "
      "mauvaises tuiles de la carte et du radar. Seulement pour comparer les performances.",
      "Liest Texturen neu, die sich im Speicher ändern, ohne dass das Spiel es meldet. Aus bringt die falschen "
      "Karten- und Radarkacheln zurück. Nur zum Vergleichen der Leistung.",
      "Vuelve a leer las texturas que cambian en memoria sin que el juego lo diga. Desactivado trae de vuelta "
      "las casillas equivocadas del mapa y del radar. Solo para comparar el rendimiento.",
      "Rilegge le texture che cambiano in memoria senza che il gioco lo segnali. Disattivato riporta i riquadri "
      "sbagliati della mappa e del radar. Solo per confrontare le prestazioni."}},

    // ---- Reset All
    {{"Reset all settings?", "Tout réinitialiser ?", "Alle Einstellungen zurücksetzen?",
      "¿Restablecer todos los ajustes?", "Ripristinare tutte le impostazioni?"}},
    {{"Everything on the Display, Game and Debug pages goes back to the recommended settings. The language stays "
      "as it is.",
      "Tout ce qui se trouve dans Affichage, Jeu et Débogage revient aux réglages recommandés. La langue ne change "
      "pas.",
      "Alles auf den Seiten Anzeige, Spiel und Debug wird auf die empfohlenen Einstellungen zurückgesetzt. Die "
      "Sprache bleibt, wie sie ist.",
      "Todo lo de Pantalla, Juego y Depuración vuelve a los ajustes recomendados. El idioma no cambia.",
      "Tutto nelle schede Schermo, Gioco e Debug torna alle impostazioni consigliate. La lingua resta com'è."}},
    {{"Settings reset", "Réglages réinitialisés", "Einstellungen zurückgesetzt", "Ajustes restablecidos",
      "Impostazioni ripristinate"}},

    // ---- Home
    {{"Not installed yet", "Pas encore installé", "Noch nicht installiert", "Aún no instalado",
      "Non ancora installato"}},
    {{"Press X to set it up from your disc image and title update.",
      "Appuyez sur X pour l'installer depuis votre image disque et la mise à jour du titre.",
      "Drücke X, um es aus deinem Disc-Image und dem Title Update einzurichten.",
      "Pulsa X para instalarlo desde tu imagen de disco y la actualización del título.",
      "Premi X per installarlo dalla tua immagine disco e dall'aggiornamento del titolo."}},

    // ---- Saves
    {{"Restore a backup", "Restaurer une copie", "Backup wiederherstellen", "Restaurar una copia",
      "Ripristina un backup"}},
    {{"Your saves  {}", "Vos sauvegardes  {}", "Deine Spielstände  {}", "Tus partidas  {}",
      "I tuoi salvataggi  {}"}},
    {{"Back Up Now", "Faire une copie maintenant", "Jetzt sichern", "Hacer copia ahora", "Esegui backup ora"}},
    {{"Import Xbox 360 Saves", "Importer des sauvegardes Xbox 360", "Xbox-360-Spielstände importieren",
      "Importar partidas de Xbox 360", "Importa salvataggi Xbox 360"}},
    {{"{} waiting", "{} en attente", "{} bereit", "{} pendientes", "{} in attesa"}},
    {{"None waiting", "Aucune en attente", "Keine bereit", "Ninguna pendiente", "Nessuno in attesa"}},
    {{"Copies all of your GTA IV saves into SaveBackups, named by the date and time.",
      "Copie toutes vos sauvegardes de GTA IV dans SaveBackups, avec la date et l'heure pour nom.",
      "Kopiert alle deine GTA-IV-Spielstände nach SaveBackups, benannt nach Datum und Uhrzeit.",
      "Copia todas tus partidas de GTA IV en SaveBackups, con la fecha y la hora como nombre.",
      "Copia tutti i tuoi salvataggi di GTA IV in SaveBackups, con data e ora come nome."}},
    {{"Bring your Xbox 360 progress over. Copy the save files from your 360 (on its drive they're in "
      "Content/<your profile>/545407F2/00000001) into this folder on the PS5:",
      "Récupérez votre progression Xbox 360. Copiez les fichiers de sauvegarde de votre 360 (sur son disque, "
      "ils sont dans Content/<votre profil>/545407F2/00000001) dans ce dossier de la PS5 :",
      "Hol deinen Xbox-360-Fortschritt rüber. Kopiere die Spielstand-Dateien deiner 360 (auf ihrer Festplatte "
      "unter Content/<dein Profil>/545407F2/00000001) in diesen Ordner auf der PS5:",
      "Trae tu progreso de Xbox 360. Copia los archivos de guardado de tu 360 (en su disco están en "
      "Content/<tu perfil>/545407F2/00000001) en esta carpeta de la PS5:",
      "Porta qui i tuoi progressi di Xbox 360. Copia i file di salvataggio della tua 360 (sul suo disco sono in "
      "Content/<il tuo profilo>/545407F2/00000001) in questa cartella della PS5:"}},
    {{"Press Cross to import them. Your saves are backed up first, and imported files move to import/done.",
      "Appuyez sur X pour les importer. Vos sauvegardes sont d'abord copiées, et les fichiers importés vont dans "
      "import/done.",
      "Drücke X, um sie zu importieren. Deine Spielstände werden vorher gesichert, und importierte Dateien kommen "
      "nach import/done.",
      "Pulsa X para importarlas. Antes se hace una copia de tus partidas, y los archivos importados pasan a "
      "import/done.",
      "Premi X per importarli. Prima viene fatto un backup dei tuoi salvataggi, e i file importati vengono "
      "spostati in import/done."}},
    {{"Files", "Fichiers", "Dateien", "Archivos", "File"}},
    {{"Size", "Taille", "Größe", "Tamaño", "Dimensione"}},
    {{"Type", "Type", "Typ", "Tipo", "Tipo"}},
    {{"Changed", "Modifié", "Geändert", "Modificado", "Modificato"}},
    {{"Restoring puts these saves back. The saves you have now are backed up first.",
      "La restauration remet ces sauvegardes en place. Celles que vous avez maintenant sont copiées d'abord.",
      "Beim Wiederherstellen kommen diese Spielstände zurück. Deine aktuellen Spielstände werden vorher gesichert.",
      "Al restaurar vuelven estas partidas. Antes se hace una copia de las que tienes ahora.",
      "Il ripristino rimette questi salvataggi. Prima viene fatto un backup di quelli che hai ora."}},
    {{"Saved game", "Partie sauvegardée", "Spielstand", "Partida guardada", "Partita salvata"}},
    {{"Game data", "Données de jeu", "Spieldaten", "Datos del juego", "Dati di gioco"}},
    {{"Extra Data", "Données supplémentaires", "Zusatzdaten", "Datos extra", "Dati extra"}},
    {{"Saves backed up", "Sauvegardes copiées", "Spielstände gesichert", "Copia hecha", "Backup completato"}},
    {{"The backup couldn't be made", "La copie n'a pas pu être faite", "Das Backup konnte nicht erstellt werden",
      "No se pudo hacer la copia", "Impossibile creare il backup"}},
    {{"Backup restored", "Copie restaurée", "Backup wiederhergestellt", "Copia restaurada", "Backup ripristinato"}},
    {{"Restoring didn't finish: your saves are in the newest backup",
      "La restauration n'a pas abouti : vos sauvegardes sont dans la copie la plus récente",
      "Wiederherstellen nicht fertig: Deine Spielstände sind im neuesten Backup",
      "La restauración no terminó: tus partidas están en la copia más reciente",
      "Ripristino non completato: i tuoi salvataggi sono nel backup più recente"}},
    {{"Nothing to import", "Rien à importer", "Nichts zu importieren", "Nada que importar", "Niente da importare"}},
    {{"Copy your Xbox 360 save files into /data/homebrew/PPSA94004/import/ on the PS5 first, then come back here.",
      "Copiez d'abord vos fichiers de sauvegarde Xbox 360 dans /data/homebrew/PPSA94004/import/ sur la PS5, "
      "puis revenez ici.",
      "Kopiere zuerst deine Xbox-360-Spielstände nach /data/homebrew/PPSA94004/import/ auf der PS5 und komm dann "
      "hierher zurück.",
      "Primero copia tus archivos de guardado de Xbox 360 en /data/homebrew/PPSA94004/import/ en la PS5 y luego "
      "vuelve aquí.",
      "Prima copia i file di salvataggio Xbox 360 in /data/homebrew/PPSA94004/import/ sulla PS5, poi torna qui."}},
    {{"Xbox 360 saves", "Sauvegardes Xbox 360", "Xbox-360-Spielstände", "Partidas de Xbox 360",
      "Salvataggi Xbox 360"}},
    {{"Restore this backup?", "Restaurer cette copie ?", "Dieses Backup wiederherstellen?",
      "¿Restaurar esta copia?", "Ripristinare questo backup?"}},
    {{"Your saves are replaced by the ones from {}. The saves you have now are backed up first, so nothing is "
      "lost.",
      "Vos sauvegardes sont remplacées par celles de {}. Celles que vous avez maintenant sont copiées d'abord, "
      "rien n'est perdu.",
      "Deine Spielstände werden durch die aus {} ersetzt. Deine aktuellen werden vorher gesichert, es geht also "
      "nichts verloren.",
      "Tus partidas se sustituyen por las de {}. Antes se hace una copia de las que tienes ahora, así que no se "
      "pierde nada.",
      "I tuoi salvataggi vengono sostituiti da quelli di {}. Prima viene fatto un backup di quelli attuali, quindi "
      "non si perde nulla."}},
    {{"{} save imported.", "{} sauvegarde importée.", "{} Spielstand importiert.", "{} partida importada.",
      "{} salvataggio importato."}},
    {{"{} saves imported.", "{} sauvegardes importées.", "{} Spielstände importiert.", "{} partidas importadas.",
      "{} salvataggi importati."}},
    {{"Load them from the game's menu.", "Chargez-les depuis le menu du jeu.", "Lade sie über das Menü des Spiels.",
      "Cárgalas desde el menú del juego.", "Caricali dal menu del gioco."}},
    {{"No saves were imported.", "Aucune sauvegarde n'a été importée.", "Es wurden keine Spielstände importiert.",
      "No se importó ninguna partida.", "Nessun salvataggio importato."}},
    {{"...the log lists the rest.", "...le journal indique le reste.", "...der Rest steht im Log.",
      "...el resto aparece en el registro.", "...il resto è nel log."}},

    // ---- Trophies
    {{"Earned", "Obtenus", "Erhalten", "Obtenidos", "Ottenuti"}},
    {{"Earned|date", "Obtenu le", "Erhalten am", "Obtenido el", "Ottenuto il"}},
    {{"Not earned yet", "Pas encore obtenus", "Noch nicht erhalten", "Aún no obtenidos", "Non ancora ottenuti"}},
    {{"Not earned yet|status", "Pas encore obtenu", "Noch nicht erhalten", "Aún no obtenido",
      "Non ancora ottenuto"}},
    {{"{} of {} earned", "{} sur {} obtenus", "{} von {} erhalten", "{} de {} obtenidos", "{} su {} ottenuti"}},
    {{"Start the game once", "Lancez le jeu une fois", "Starte das Spiel einmal", "Inicia el juego una vez",
      "Avvia il gioco una volta"}},
    {{"The trophy list comes from the game itself: start it once, and the next time you open the hub your "
      "trophies are here, with the ones you've earned.",
      "La liste des trophées vient du jeu lui-même : lancez-le une fois, et la prochaine fois que vous "
      "ouvrirez le hub, vos trophées seront ici, avec ceux que vous avez obtenus.",
      "Die Trophäenliste kommt aus dem Spiel selbst: Starte es einmal, und wenn du den Hub das nächste Mal "
      "öffnest, sind deine Trophäen hier, samt denen, die du erhalten hast.",
      "La lista de trofeos sale del propio juego: inícialo una vez y, la próxima vez que abras el hub, tus "
      "trofeos estarán aquí, con los que hayas obtenido.",
      "L'elenco dei trofei viene dal gioco stesso: avvialo una volta e, la prossima volta che apri l'hub, i tuoi "
      "trofei saranno qui, con quelli che hai ottenuto."}},
    {{"Grand Theft Auto IV Platinum", "Platine Grand Theft Auto IV", "Grand Theft Auto IV Platin",
      "Platino de Grand Theft Auto IV", "Platino di Grand Theft Auto IV"}},
    {{"Hidden trophy", "Trophée caché", "Versteckte Trophäe", "Trofeo oculto", "Trofeo nascosto"}},
    {{"Earn every other GTA IV trophy ({}).", "Obtenez tous les autres trophées de GTA IV ({}).",
      "Erhalte alle anderen GTA-IV-Trophäen ({}).", "Consigue todos los demás trofeos de GTA IV ({}).",
      "Ottieni tutti gli altri trofei di GTA IV ({})."}},
    {{"Keep playing to find out what this one is.", "Continuez à jouer pour découvrir ce trophée.",
      "Spiel weiter, um herauszufinden, was es ist.", "Sigue jugando para descubrir cuál es.",
      "Continua a giocare per scoprire di cosa si tratta."}},
    {{"Bronze Trophy", "Trophée de bronze", "Bronze-Trophäe", "Trofeo de bronce", "Trofeo di bronzo"}},
    {{"Silver Trophy", "Trophée d'argent", "Silber-Trophäe", "Trofeo de plata", "Trofeo d'argento"}},
    {{"Gold Trophy", "Trophée d'or", "Gold-Trophäe", "Trofeo de oro", "Trofeo d'oro"}},
    {{"Platinum Trophy", "Trophée de platine", "Platin-Trophäe", "Trofeo de platino", "Trofeo di platino"}},
    {{"Status", "Statut", "Status", "Estado", "Stato"}},
    {{"{} to go", "Encore {}", "Noch {}", "Faltan {}", "Ne mancano {}"}},

    // ---- About
    {{"Created Liberty Recompiled, the GTA IV recompilation this is built on, with its contributors.",
      "A créé Liberty Recompiled, la recompilation de GTA IV sur laquelle tout ceci repose, avec ses "
      "contributeurs.",
      "Hat Liberty Recompiled geschaffen, die GTA-IV-Rekompilierung, auf der das hier aufbaut, zusammen mit ihren "
      "Mitwirkenden.",
      "Creó Liberty Recompiled, la recompilación de GTA IV en la que se basa esto, junto a sus colaboradores.",
      "Ha creato Liberty Recompiled, la ricompilazione di GTA IV su cui si basa tutto questo, insieme ai suoi "
      "collaboratori."}},
    {{"Brought it to PlayStation 5: the PS5 port and this hub.",
      "L'a amené sur PlayStation 5 : le portage PS5 et ce hub.",
      "Hat es auf die PlayStation 5 gebracht: die PS5-Portierung und diesen Hub.",
      "Lo trajo a PlayStation 5: la versión de PS5 y este hub.",
      "L'ha portato su PlayStation 5: il port per PS5 e questo hub."}},
    {{"LibertyRecompAndroid: the fixes for running it on handheld hardware.",
      "LibertyRecompAndroid : les correctifs pour le faire tourner sur des appareils portables.",
      "LibertyRecompAndroid: die Fixes, damit es auf Handheld-Hardware läuft.",
      "LibertyRecompAndroid: los arreglos para que funcione en dispositivos portátiles.",
      "LibertyRecompAndroid: le correzioni per farlo girare su dispositivi portatili."}},
    {{"RecompTrophy: the native PS5 trophies and their pop-ups.",
      "RecompTrophy : les trophées PS5 natifs et leurs notifications.",
      "RecompTrophy: die nativen PS5-Trophäen und ihre Einblendungen.",
      "RecompTrophy: los trofeos nativos de PS5 y sus avisos.",
      "RecompTrophy: i trofei nativi di PS5 e le loro notifiche."}},
    {{"The static recompiler and runtime that turn the Xbox 360 game into a native program.",
      "Le recompilateur statique et l'environnement d'exécution qui transforment le jeu Xbox 360 en programme "
      "natif.",
      "Der statische Recompiler und die Laufzeitumgebung, die das Xbox-360-Spiel in ein natives Programm "
      "verwandeln.",
      "El recompilador estático y el entorno de ejecución que convierten el juego de Xbox 360 en un programa "
      "nativo.",
      "Il ricompilatore statico e il runtime che trasformano il gioco Xbox 360 in un programma nativo."}},
    {{"Built With", "Réalisé avec", "Erstellt mit", "Hecho con", "Realizzato con"}},
    {{"stb_image, stb_vorbis, dr_mp3 and dr_flac", "stb_image, stb_vorbis, dr_mp3 et dr_flac",
      "stb_image, stb_vorbis, dr_mp3 und dr_flac", "stb_image, stb_vorbis, dr_mp3 y dr_flac",
      "stb_image, stb_vorbis, dr_mp3 e dr_flac"}},
    {{"Oswald and Inter (SIL Open Font License)", "Oswald et Inter (SIL Open Font License)",
      "Oswald und Inter (SIL Open Font License)", "Oswald e Inter (SIL Open Font License)",
      "Oswald e Inter (SIL Open Font License)"}},
    {{"Please Note", "À noter", "Bitte beachten", "Ten en cuenta", "Nota bene"}},
    {{"An unofficial fan project, not affiliated with or endorsed by Rockstar Games, Take-Two "
      "Interactive, Microsoft or Sony. Grand Theft Auto and all related names belong to their owners. No game "
      "files are included: you need your own copy of GTA IV for Xbox 360.",
      "Un projet de fan non officiel, sans lien avec Rockstar Games, Take-Two Interactive, "
      "Microsoft ou Sony, ni approuvé par eux. Grand Theft Auto et tous les noms associés appartiennent à leurs "
      "propriétaires. Aucun fichier du jeu n'est inclus : il vous faut votre propre copie de GTA IV pour "
      "Xbox 360.",
      "Ein inoffizielles Fanprojekt, weder mit Rockstar Games, Take-Two Interactive, Microsoft "
      "oder Sony verbunden noch von ihnen unterstützt. Grand Theft Auto und alle zugehörigen Namen gehören ihren "
      "Inhabern. Es sind keine Spieldateien enthalten: Du brauchst deine eigene Kopie von GTA IV für die "
      "Xbox 360.",
      "Un proyecto de fans no oficial, sin relación con Rockstar Games, Take-Two Interactive, "
      "Microsoft ni Sony, ni respaldado por ellos. Grand Theft Auto y todos los nombres relacionados pertenecen a "
      "sus dueños. No incluye archivos del juego: necesitas tu propia copia de GTA IV para Xbox 360.",
      "Un progetto amatoriale non ufficiale, non affiliato né approvato da Rockstar Games, "
      "Take-Two Interactive, Microsoft o Sony. Grand Theft Auto e tutti i nomi correlati appartengono ai "
      "rispettivi proprietari. Non è incluso alcun file del gioco: ti serve una tua copia di GTA IV per "
      "Xbox 360."}},
    {{"Version {}", "Version {}", "Version {}", "Versión {}", "Versione {}"}},
    // (holding Cross on About for a few seconds: the Debug page and the
    // Display page's rarer settings, shown or hidden)
    {{"Debug options on", "Options de débogage activées", "Debug-Optionen an", "Opciones de depuración activadas",
      "Opzioni di debug attive"}},
    {{"Debug options off", "Options de débogage désactivées", "Debug-Optionen aus",
      "Opciones de depuración desactivadas", "Opzioni di debug disattivate"}},

    // ---- the installer
    {{"Your game files", "Vos fichiers de jeu", "Deine Spieldateien", "Tus archivos del juego",
      "I tuoi file di gioco"}},
    {{"One time only: the game is copied off your Xbox 360 disc image.",
      "Une seule fois : le jeu est copié depuis votre image disque Xbox 360.",
      "Nur einmal: Das Spiel wird aus deinem Xbox-360-Disc-Image kopiert.",
      "Solo una vez: el juego se copia desde tu imagen de disco de Xbox 360.",
      "Una volta sola: il gioco viene copiato dalla tua immagine disco Xbox 360."}},
    {{"Disc Image", "Image disque", "Disc-Image", "Imagen de disco", "Immagine disco"}},
    {{"Game Folder", "Dossier du jeu", "Spielordner", "Carpeta del juego", "Cartella del gioco"}},
    {{"Checking", "Vérification", "Prüfung läuft", "Comprobando", "Verifica in corso"}},
    {{"Missing", "Absent", "Fehlt", "Falta", "Mancante"}},
    {{"Can't use", "Inutilisable", "Unbrauchbar", "No sirve", "Non utilizzabile"}},
    {{"Space", "Espace", "Speicherplatz", "Espacio", "Spazio"}},
    {{"Needs about {} free", "Il faut environ {} libres", "Braucht etwa {} frei", "Necesita unos {} libres",
      "Servono circa {} liberi"}},
    {{"{} free", "{} libres", "{} frei", "{} libres", "{} liberi"}},
    {{"Not found", "Introuvable", "Nicht gefunden", "No encontrado", "Non trovato"}},
    {{"Making sure the disc image is the USA disc of GTA IV. This takes a few seconds.",
      "Vérification que l'image disque est bien le disque américain de GTA IV. Cela prend quelques secondes.",
      "Prüft, ob das Disc-Image die US-Disc von GTA IV ist. Das dauert ein paar Sekunden.",
      "Comprobando que la imagen de disco es el disco estadounidense de GTA IV. Tarda unos segundos.",
      "Verifica che l'immagine disco sia il disco USA di GTA IV. Ci vuole qualche secondo."}},
    {{"Ready", "Prêt", "Bereit", "Listo", "Pronto"}},
    {{"Press Cross to install. Keep the app open until it's done. Your disc image isn't changed.",
      "Appuyez sur X pour installer. Laissez l'application ouverte jusqu'à la fin. Votre image disque n'est pas "
      "modifiée.",
      "Drücke X zum Installieren. Lass die App offen, bis es fertig ist. Dein Disc-Image wird nicht verändert.",
      "Pulsa X para instalar. Deja la aplicación abierta hasta que termine. Tu imagen de disco no se modifica.",
      "Premi X per installare. Tieni aperta l'app finché non ha finito. La tua immagine disco non viene "
      "modificata."}},
    {{"Installs To", "Installé dans", "Installiert nach", "Se instala en", "Installato in"}},
    {{"On {}", "Sur {}", "Auf {}", "En {}", "In {}"}},
    {{"the app's drive", "le disque de l'application", "dem Laufwerk der App", "la unidad de la aplicación",
      "unità dell'app"}},
    {{"internal storage", "le stockage interne", "dem internen Speicher", "el almacenamiento interno",
      "memoria interna"}},
    {{"an exFAT drive", "un disque exFAT", "einem exFAT-Laufwerk", "una unidad exFAT", "unità exFAT"}},
    {{"a FAT drive", "un disque FAT", "einem FAT-Laufwerk", "una unidad FAT", "unità FAT"}},
    {{"a {} drive", "un disque {}", "einem {}-Laufwerk", "una unidad {}", "unità {}"}},
    {{"Can't install yet", "Pas encore installable", "Noch keine Installation möglich",
      "Aún no se puede instalar", "Non è ancora possibile installare"}},
    {{"The Install Folder", "Le dossier d'installation", "Der Installationsordner", "La carpeta de instalación",
      "La cartella di installazione"}},
    {{"The install stopped", "L'installation s'est arrêtée", "Die Installation wurde abgebrochen",
      "La instalación se detuvo", "L'installazione si è interrotta"}},
    {{"What was copied has been removed, so nothing is left half done.",
      "Ce qui avait été copié a été supprimé, rien n'est laissé à moitié.",
      "Was kopiert wurde, ist wieder entfernt, es bleibt nichts halb fertig.",
      "Lo que se copió se ha borrado, así que no queda nada a medias.",
      "Ciò che era stato copiato è stato rimosso, quindi non resta nulla a metà."}},
    {{"What Happened", "Ce qui s'est passé", "Was passiert ist", "Qué ha pasado", "Cosa è successo"}},
    {{"Press Cross to try again. If it stops again, send the log from /data/homebrew/PPSA94004/logs.",
      "Appuyez sur X pour réessayer. Si ça s'arrête encore, envoyez le journal de /data/homebrew/PPSA94004/logs.",
      "Drücke X für einen neuen Versuch. Wenn es wieder abbricht, schick das Log aus "
      "/data/homebrew/PPSA94004/logs.",
      "Pulsa X para reintentarlo. Si vuelve a detenerse, envía el registro de /data/homebrew/PPSA94004/logs.",
      "Premi X per riprovare. Se si ferma di nuovo, invia il log da /data/homebrew/PPSA94004/logs."}},
    {{"Installed", "Installé", "Installiert", "Instalado", "Installato"}},
    {{"GTA IV is set up on your PS5. Press Cross to play.", "GTA IV est installé sur votre PS5. Appuyez sur X pour "
      "jouer.", "GTA IV ist auf deiner PS5 eingerichtet. Drücke X zum Spielen.",
      "GTA IV está instalado en tu PS5. Pulsa X para jugar.", "GTA IV è installato sulla tua PS5. Premi X per "
      "giocare."}},
    {{"Installing", "Installation en cours", "Wird installiert", "Instalando", "Installazione in corso"}},
    {{"Keep the app open until it's done. The PS5 won't go to sleep.",
      "Laissez l'application ouverte jusqu'à la fin. La PS5 ne se mettra pas en veille.",
      "Lass die App offen, bis es fertig ist. Die PS5 geht nicht in den Ruhemodus.",
      "Deja la aplicación abierta hasta que termine. La PS5 no entrará en reposo.",
      "Tieni aperta l'app finché non ha finito. La PS5 non andrà in standby."}},
    {{"All Done", "Terminé", "Fertig", "Terminado", "Completato"}},
    {{"Stopping", "Arrêt", "Wird gestoppt", "Deteniendo", "Interruzione"}},
    {{"Finishing", "Finalisation", "Abschluss", "Terminando", "Completamento"}},
    {{"Step 1 of 2", "Étape 1 sur 2", "Schritt 1 von 2", "Paso 1 de 2", "Passo 1 di 2"}},
    {{"Step 2 of 2", "Étape 2 sur 2", "Schritt 2 von 2", "Paso 2 de 2", "Passo 2 di 2"}},
    {{"The game's files are in place", "Les fichiers du jeu sont en place", "Die Spieldateien sind an Ort und Stelle",
      "Los archivos del juego están en su sitio", "I file del gioco sono al loro posto"}},
    {{"Removing what was copied", "Suppression de ce qui a été copié", "Entferne, was kopiert wurde",
      "Borrando lo que se copió", "Rimozione di ciò che è stato copiato"}},
    {{"Checking the game and putting it in place", "Vérification du jeu et mise en place",
      "Prüfe das Spiel und lege es ab", "Comprobando el juego y colocándolo", "Verifica del gioco e sistemazione"}},
    {{"Unpacking the game's archives", "Décompression des archives du jeu", "Entpacke die Archive des Spiels",
      "Descomprimiendo los archivos del juego", "Estrazione degli archivi del gioco"}},
    {{"Copying the game from your disc image", "Copie du jeu depuis votre image disque",
      "Kopiere das Spiel aus deinem Disc-Image", "Copiando el juego desde tu imagen de disco",
      "Copia del gioco dalla tua immagine disco"}},
    {{"{} written", "{} écrits", "{} geschrieben", "{} escritos", "{} scritti"}},
    {{"Took under a minute", "Moins d'une minute", "Unter einer Minute", "Tardó menos de un minuto",
      "Meno di un minuto"}},
    {{"Took {} min", "Durée : {} min", "Dauer: {} Min.", "Tardó {} min", "Durata: {} min"}},
    {{"Less than a minute left", "Moins d'une minute restante", "Weniger als eine Minute übrig",
      "Queda menos de un minuto", "Manca meno di un minuto"}},
    {{"About {} min left", "Encore {} min environ", "Noch etwa {} Min.", "Quedan unos {} min",
      "Mancano circa {} min"}},
    {{"Working out the time left", "Calcul du temps restant", "Restzeit wird berechnet",
      "Calculando el tiempo restante", "Calcolo del tempo rimanente"}},
    {{"Reading at {}  \xC2\xB7  writing at {}", "Lecture à {}  \xC2\xB7  écriture à {}",
      "Lesen mit {}  \xC2\xB7  Schreiben mit {}", "Leyendo a {}  \xC2\xB7  escribiendo a {}",
      "Lettura a {}  \xC2\xB7  scrittura a {}"}},
    {{" to {}", " vers {}", " auf {}", " en {}", " in {}"}},
    {{"Install stopped. Nothing was kept.", "Installation arrêtée. Rien n'a été conservé.",
      "Installation gestoppt. Nichts wurde behalten.", "Instalación detenida. No se ha guardado nada.",
      "Installazione interrotta. Non è stato tenuto nulla."}},
    {{"Stop installing?", "Arrêter l'installation ?", "Installation stoppen?", "¿Detener la instalación?",
      "Interrompere l'installazione?"}},
    {{"What's been copied so far is removed. You can start over later.",
      "Ce qui a été copié jusqu'ici est supprimé. Vous pourrez recommencer plus tard.",
      "Was bisher kopiert wurde, wird entfernt. Du kannst später neu anfangen.",
      "Se borra lo copiado hasta ahora. Puedes empezar de nuevo más tarde.",
      "Ciò che è stato copiato finora viene rimosso. Puoi ricominciare più tardi."}},
    {{"There's no disc image in the install folder. Copy your GTA IV disc image (.iso) into "
      "/data/homebrew/PPSA94004/install/ and press Triangle.",
      "Il n'y a pas d'image disque dans le dossier d'installation. Copiez votre image disque de GTA IV (.iso) dans "
      "/data/homebrew/PPSA94004/install/ et appuyez sur Triangle.",
      "Im Installationsordner ist kein Disc-Image. Kopiere dein GTA-IV-Disc-Image (.iso) nach "
      "/data/homebrew/PPSA94004/install/ und drücke Dreieck.",
      "No hay ninguna imagen de disco en la carpeta de instalación. Copia tu imagen de disco de GTA IV (.iso) en "
      "/data/homebrew/PPSA94004/install/ y pulsa Triángulo.",
      "Nella cartella di installazione non c'è nessuna immagine disco. Copia la tua immagine disco di GTA IV (.iso) "
      "in /data/homebrew/PPSA94004/install/ e premi Triangolo."}},
    {{"This disc image can't be used: {}\nIt has to be the USA disc of GTA IV, the original release.",
      "Cette image disque n'est pas utilisable : {}\nIl faut le disque américain de GTA IV, l'édition "
      "d'origine.",
      "Dieses Disc-Image ist nicht verwendbar: {}\nEs muss die US-Disc von GTA IV sein, die "
      "Originalveröffentlichung.",
      "Esta imagen de disco no sirve: {}\nTiene que ser el disco estadounidense de GTA IV, la edición original.",
      "Questa immagine disco non si può usare: {}\nDeve essere il disco USA di GTA IV, l'edizione originale."}},
    {{"Title Update 8 is missing. Copy TU_1A581VI_000000K000000.0000000000205 into the install folder, next to the "
      "disc image, and press Triangle.",
      "Il manque le Title Update 8. Copiez TU_1A581VI_000000K000000.0000000000205 dans le dossier d'installation, "
      "à côté de l'image disque, et appuyez sur Triangle.",
      "Title Update 8 fehlt. Kopiere TU_1A581VI_000000K000000.0000000000205 in den Installationsordner neben das "
      "Disc-Image und drücke Dreieck.",
      "Falta el Title Update 8. Copia TU_1A581VI_000000K000000.0000000000205 en la carpeta de instalación, junto a "
      "la imagen de disco, y pulsa Triángulo.",
      "Manca il Title Update 8. Copia TU_1A581VI_000000K000000.0000000000205 nella cartella di installazione, "
      "accanto all'immagine disco, e premi Triangolo."}},
    {{"There isn't enough free space. The install needs about {} and the drive has {} free.",
      "Il n'y a pas assez d'espace libre. L'installation a besoin d'environ {} et le disque a {} de libre.",
      "Nicht genug freier Speicher. Die Installation braucht etwa {}, das Laufwerk hat {} frei.",
      "No hay suficiente espacio libre. La instalación necesita unos {} y la unidad tiene {} libres.",
      "Spazio libero insufficiente. L'installazione richiede circa {} e l'unità ha {} liberi."}},
    // ---- the episodes
    {{"Episodes", "Épisodes", "Episoden", "Episodios", "Episodi"}},
    {{"The Lost and Damned", "The Lost and Damned", "The Lost and Damned", "The Lost and Damned", "The Lost and Damned"}},
    {{"The Ballad of Gay Tony", "The Ballad of Gay Tony", "The Ballad of Gay Tony", "The Ballad of Gay Tony",
      "The Ballad of Gay Tony"}},
    {{"Ready to install|episode", "Prêt à installer", "Bereit zur Installation", "Listo para instalar",
      "Pronto da installare"}},
    {{"Not installed", "Non installé", "Nicht installiert", "No instalado", "Non installato"}},
    {{"{} is installed", "{} est installé", "{} ist installiert", "{} está instalado", "{} è installato"}},
    {{"Johnny Klebitz and The Lost MC. The first of the two episodes.",
      "Johnny Klebitz et The Lost MC. Le premier des deux épisodes.",
      "Johnny Klebitz und The Lost MC. Die erste der beiden Episoden.",
      "Johnny Klebitz y The Lost MC. El primero de los dos episodios.",
      "Johnny Klebitz e The Lost MC. Il primo dei due episodi."}},
    {{"Luis Lopez and nightclub owner Tony Prince. The second of the two episodes.",
      "Luis Lopez et le patron de boîtes de nuit Tony Prince. Le second des deux épisodes.",
      "Luis Lopez und Nachtclubbesitzer Tony Prince. Die zweite der beiden Episoden.",
      "Luis Lopez y Tony Prince, dueño de discotecas. El segundo de los dos episodios.",
      "Luis Lopez e Tony Prince, proprietario di locali notturni. Il secondo dei due episodi."}},
    {{"Installed. When the game starts it asks if you want to play it, the first time.",
      "Installé. Au premier lancement, le jeu vous propose d'y jouer.",
      "Installiert. Beim ersten Start fragt das Spiel, ob du es spielen willst.",
      "Instalado. La primera vez que se inicie, el juego te preguntará si quieres jugarlo.",
      "Installato. Al primo avvio il gioco ti chiede se vuoi giocarlo."}},
    {{"Install GTA IV first. The episodes are added to the installed game.",
      "Installez d'abord GTA IV. Les épisodes s'ajoutent au jeu installé.",
      "Installiere zuerst GTA IV. Die Episoden kommen zum installierten Spiel dazu.",
      "Instala primero GTA IV. Los episodios se añaden al juego instalado.",
      "Installa prima GTA IV. Gli episodi si aggiungono al gioco installato."}},
    {{"Copy the episode's package from your Xbox 360 (on its drive it's in "
      "Content/0000000000000000/545407F2/00000002) into this folder on the PS5:",
      "Copiez le paquet de l'épisode depuis votre Xbox 360 (sur son disque, il est dans "
      "Content/0000000000000000/545407F2/00000002) dans ce dossier de la PS5 :",
      "Kopiere das Paket der Episode von deiner Xbox 360 (auf ihrer Festplatte liegt es unter "
      "Content/0000000000000000/545407F2/00000002) in diesen Ordner auf der PS5:",
      "Copia el paquete del episodio desde tu Xbox 360 (en su disco está en "
      "Content/0000000000000000/545407F2/00000002) a esta carpeta de la PS5:",
      "Copia il pacchetto dell'episodio dalla tua Xbox 360 (sul suo disco si trova in "
      "Content/0000000000000000/545407F2/00000002) in questa cartella della PS5:"}},
    {{"Its package is there. Press Cross to install it.", "Son paquet est là. Appuyez sur X pour l'installer.",
      "Das Paket ist da. Drücke X, um es zu installieren.", "Su paquete ya está ahí. Pulsa X para instalarlo.",
      "Il pacchetto c'è. Premi X per installarlo."}},
    {{"Then press Cross to install it.", "Puis appuyez sur X pour l'installer.", "Dann drücke X, um es zu installieren.",
      "Después pulsa X para instalarlo.", "Poi premi X per installarlo."}},
    {{"There's no package for {} in the install folder. Copy it from your Xbox 360 (on its drive it's in "
      "Content/0000000000000000/545407F2/00000002) into /data/homebrew/PPSA94004/install/ and press Triangle.",
      "Il n'y a pas de paquet pour {} dans le dossier d'installation. Copiez-le depuis votre Xbox 360 (sur son "
      "disque, il est dans Content/0000000000000000/545407F2/00000002) dans /data/homebrew/PPSA94004/install/ et "
      "appuyez sur Triangle.",
      "Im Installationsordner ist kein Paket für {}. Kopiere es von deiner Xbox 360 (auf ihrer Festplatte liegt es "
      "unter Content/0000000000000000/545407F2/00000002) nach /data/homebrew/PPSA94004/install/ und drücke "
      "Dreieck.",
      "No hay ningún paquete de {} en la carpeta de instalación. Cópialo desde tu Xbox 360 (en su disco está en "
      "Content/0000000000000000/545407F2/00000002) a /data/homebrew/PPSA94004/install/ y pulsa Triángulo.",
      "Nella cartella di installazione non c'è nessun pacchetto di {}. Copialo dalla tua Xbox 360 (sul suo disco "
      "si trova in Content/0000000000000000/545407F2/00000002) in /data/homebrew/PPSA94004/install/ e premi "
      "Triangolo."}},
    {{"This package can't be used: {}", "Ce paquet n'est pas utilisable : {}", "Dieses Paket ist nicht verwendbar: {}",
      "Este paquete no sirve: {}", "Questo pacchetto non si può usare: {}"}},
    {{"Episode Package", "Paquet de l'épisode", "Episodenpaket", "Paquete del episodio", "Pacchetto dell'episodio"}},
    {{"Xbox 360 package", "Paquet Xbox 360", "Xbox-360-Paket", "Paquete de Xbox 360", "Pacchetto Xbox 360"}},
    {{"Folder", "Dossier", "Ordner", "Carpeta", "Cartella"}},
    {{"One time only: the episode is copied off its Xbox 360 package.",
      "Une seule fois : l'épisode est copié depuis son paquet Xbox 360.",
      "Nur einmal: Die Episode wird aus ihrem Xbox-360-Paket kopiert.",
      "Solo una vez: el episodio se copia desde su paquete de Xbox 360.",
      "Una volta sola: l'episodio viene copiato dal suo pacchetto Xbox 360."}},
    {{"Making sure the package is the episode. This takes a few seconds.",
      "Vérification que le paquet est bien l'épisode. Cela prend quelques secondes.",
      "Prüft, ob das Paket die Episode ist. Das dauert ein paar Sekunden.",
      "Comprobando que el paquete es el episodio. Tarda unos segundos.",
      "Verifica che il pacchetto sia l'episodio. Ci vuole qualche secondo."}},
    {{"Press Cross to install. Keep the app open until it's done. The package isn't changed.",
      "Appuyez sur X pour installer. Laissez l'application ouverte jusqu'à la fin. Le paquet n'est pas modifié.",
      "Drücke X zum Installieren. Lass die App offen, bis es fertig ist. Das Paket wird nicht verändert.",
      "Pulsa X para instalar. Deja la aplicación abierta hasta que termine. El paquete no se modifica.",
      "Premi X per installare. Tieni aperta l'app finché non ha finito. Il pacchetto non viene modificato."}},
    {{"Installing {}", "Installation de {}", "{} wird installiert", "Instalando {}", "Installazione di {}"}},
    {{"{} is added to your game. When the game starts it asks if you want to play it.",
      "{} est ajouté à votre jeu. Au lancement, le jeu vous propose d'y jouer.",
      "{} ist jetzt in deinem Spiel. Beim Start fragt das Spiel, ob du es spielen willst.",
      "{} se ha añadido a tu juego. Al iniciarse, el juego te preguntará si quieres jugarlo.",
      "{} è stato aggiunto al tuo gioco. All'avvio il gioco ti chiede se vuoi giocarlo."}},
    {{"Copying", "Copie", "Kopieren", "Copiando", "Copia"}},
    {{"The episode's files are in place", "Les fichiers de l'épisode sont en place",
      "Die Dateien der Episode sind an Ort und Stelle", "Los archivos del episodio están en su sitio",
      "I file dell'episodio sono al loro posto"}},
    {{"Checking the episode and putting it in place", "Vérification de l'épisode et mise en place",
      "Prüfe die Episode und lege sie ab", "Comprobando el episodio y colocándolo",
      "Verifica dell'episodio e sistemazione"}},
    {{"Copying the episode from its package", "Copie de l'épisode depuis son paquet",
      "Kopiere die Episode aus ihrem Paket", "Copiando el episodio desde su paquete",
      "Copia dell'episodio dal suo pacchetto"}},
    // ---- City Population
    {{"City Population", "Population de la ville", "Stadtbevölkerung", "Población de la ciudad",
      "Popolazione della città"}},
    {{"Fewest", "Minimale", "Am wenigsten", "Mínima", "Minima"}},
    {{"Fewer", "Réduite", "Weniger", "Menos", "Ridotta"}},
    {{"More", "Plus dense", "Mehr", "Más", "Maggiore"}},
    {{"How many cars and people are out in the city. Original is the Xbox 360's. Fewer takes load off the game in "
      "busy streets and helps it hold its frame rate; More fills the streets and costs frame rate.",
      "Le nombre de voitures et de passants en ville. D'origine correspond à la Xbox 360. Réduite allège le jeu dans "
      "les rues animées et l'aide à rester fluide\u00A0; Plus dense remplit les rues et coûte des images par seconde.",
      "Wie viele Autos und Menschen in der Stadt unterwegs sind. Original entspricht der Xbox 360. Weniger entlastet "
      "das Spiel in belebten Straßen und hilft, die Bildrate zu halten; Mehr füllt die Straßen und kostet Bildrate.",
      "Cuántos coches y peatones hay en la ciudad. Original es la de Xbox 360. Menos alivia el juego en las calles "
      "concurridas y le ayuda a mantener los fotogramas; Más llena las calles y cuesta fotogramas.",
      "Quante auto e persone ci sono in città. Originale è quella di Xbox 360. Ridotta alleggerisce il gioco nelle "
      "strade affollate e lo aiuta a tenere i fotogrammi; Maggiore riempie le strade e costa fotogrammi."}},
};
// clang-format on

struct Found {
  const Entry* entry;
  const char* english;  // what English shows (the key without its note)
};

const std::unordered_map<std::string_view, Found>& Index() {
  static const std::unordered_map<std::string_view, Found> index = [] {
    static std::vector<std::string> heads;  // (kept: T hands out pointers into them)
    heads.reserve(16);
    std::unordered_map<std::string_view, Found> m;
    for (const Entry& e : kTable) {
      const char* key = e.text[kEnglish];
      const char* shown = key;
      if (const char* bar = std::strchr(key, '|')) {
        heads.emplace_back(key, bar);
        shown = heads.back().c_str();
      }
      m.emplace(key, Found{&e, shown});
    }
    return m;
  }();
  return index;
}

}  // namespace

Lang System() {
  int v = 1;  // (SCE_SYSTEM_PARAM_LANG_ENGLISH_US)
#if defined(__PROSPERO__)
  if (sceSystemServiceParamGetInt(1 /* SCE_SYSTEM_SERVICE_PARAM_ID_LANG */, &v) != 0) v = 1;
#else
  if (const char* e = std::getenv("HUB_SYSTEM_LANG")) v = std::atoi(e);  // (the preview)
#endif
  switch (v) {
    case 2:   // French
    case 22:  // French (Canada)
      return kFrench;
    case 3:   // Spanish
    case 20:  // Spanish (Latin America)
      return kSpanish;
    case 4: return kGerman;
    case 5: return kItalian;
    default: return kEnglish;
  }
}

Lang FromSetting(const std::string& value) {
  if (value == "english") return kEnglish;
  if (value == "french") return kFrench;
  if (value == "german") return kGerman;
  if (value == "spanish") return kSpanish;
  if (value == "italian") return kItalian;
  return System();
}

uint32_t XboxLanguage(Lang l) {
  switch (l) {
    case kFrench: return 4;
    case kGerman: return 3;
    case kSpanish: return 5;
    case kItalian: return 6;
    default: return 1;
  }
}

const char* Name(Lang l) {
  static const char* const names[kLangCount] = {"English", "Français", "Deutsch", "Español", "Italiano"};
  return names[l < kLangCount ? l : kEnglish];
}

void Set(Lang l) { g_lang.store(l < kLangCount ? l : kEnglish, std::memory_order_relaxed); }
Lang Current() { return Lang(g_lang.load(std::memory_order_relaxed)); }

const char* T(const char* english) {
  if (!english) return "";
  const Lang l = Current();
  const auto& index = Index();
  if (auto it = index.find(english); it != index.end()) {
    const char* text = it->second.entry->text[l];
    return l != kEnglish && text && *text ? text : it->second.english;
  }
  return english;
}

bool Has(const char* english) { return english && Index().count(english); }

std::string F(const char* english, std::initializer_list<std::string> args) {
  std::string out = T(english);
  size_t from = 0;
  for (const std::string& a : args) {
    const size_t at = out.find("{}", from);
    if (at == std::string::npos) break;
    out.replace(at, 2, a);
    from = at + a.size();
  }
  return out;
}

std::string Units(std::string s) {
  const Lang l = Current();
  if (l == kEnglish) return s;
  for (char& c : s)
    if (c == '.') c = ',';
  if (l == kFrench) {
    for (const auto& [from, to] : {std::pair<const char*, const char*>{"GB", "Go"}, {"MB", "Mo"}, {"KB", "Ko"}}) {
      for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + 2)) s.replace(at, 2, to);
    }
    if (const size_t sp = s.find(' '); sp != std::string::npos) s.replace(sp, 1, " ");
  }
  return s;
}

std::string Date(int64_t t) {
  if (t <= 0) return "-";
  const std::time_t tt = std::time_t(t);
  std::tm tm{};
  localtime_r(&tt, &tm);
  char buf[48];
  const Lang l = Current();
  const char* format = l == kEnglish ? "%b %d, %Y  %H:%M" : l == kGerman ? "%d.%m.%Y  %H:%M" : "%d/%m/%Y  %H:%M";
  std::strftime(buf, sizeof(buf), format, &tm);
  return buf;
}

std::string Clock(int64_t t) {
  // clang-format off
  static const char* const kDays[kLangCount][7] = {
      {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"},
      {"DIM", "LUN", "MAR", "MER", "JEU", "VEN", "SAM"},
      {"SO", "MO", "DI", "MI", "DO", "FR", "SA"},
      {"DOM", "LUN", "MAR", "MI\u00C9", "JUE", "VIE", "S\u00C1B"},
      {"DOM", "LUN", "MAR", "MER", "GIO", "VEN", "SAB"}};
  static const char* const kMonths[kLangCount][12] = {
      {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"},
      {"JANV", "F\u00C9VR", "MARS", "AVR", "MAI", "JUIN", "JUIL", "AO\u00DBT", "SEPT", "OCT", "NOV", "D\u00C9C"},
      {"JAN", "FEB", "M\u00C4R", "APR", "MAI", "JUN", "JUL", "AUG", "SEP", "OKT", "NOV", "DEZ"},
      {"ENE", "FEB", "MAR", "ABR", "MAY", "JUN", "JUL", "AGO", "SEP", "OCT", "NOV", "DIC"},
      {"GEN", "FEB", "MAR", "APR", "MAG", "GIU", "LUG", "AGO", "SET", "OTT", "NOV", "DIC"}};
  // clang-format on
  const std::time_t tt = std::time_t(t);
  std::tm tm{};
  localtime_r(&tt, &tm);
  const Lang l = Current();
  const int hour = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
  char buf[64];
  if (l == kEnglish)
    std::snprintf(buf, sizeof(buf), "%s / %s %d / %d:%02d %s", kDays[l][tm.tm_wday % 7], kMonths[l][tm.tm_mon % 12],
                  tm.tm_mday, hour, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
  else  // (the day before the month)
    std::snprintf(buf, sizeof(buf), "%s / %d %s / %d:%02d %s", kDays[l][tm.tm_wday % 7], tm.tm_mday,
                  kMonths[l][tm.tm_mon % 12], hour, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
  return buf;
}

}  // namespace gta4ps5::hub::lang

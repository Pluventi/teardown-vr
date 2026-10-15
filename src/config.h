#pragma once
#include <initializer_list>
#include <string>

// All user-tweakable settings. Persisted in TeardownVR.ini next to the game exe.
struct Settings
{
    // --- Affichage -----------------------------------------------------------
    std::string language = "en";   // the mod's language: "en" (default) or "fr" (i18n.h Tr; the installer asks; an ini written before this key existed is "fr", ConfigLoad)
    int   renderPreset   = 6;      // qualité d'image VR, index dans kRenderPresets (0 = écran du PC ; 6 = native du casque Quest 3, réglage d'origine du mod)
    float vrAspect       = 0.925f; // largeur/hauteur du champ de vision du casque (mémorisé ; 0.925 = Quest 3 : la bonne taille d'image dès le 1er lancement)
    float fovCoverage    = 1.00f;  // part du champ de vision du casque où le jeu dessine (0.5..1) : noir autour, rendu plus petit d'autant (au prochain lancement du jeu)
    bool  fovCrop        = true;   // (.ini only) 1 = each eye image covers the headset's whole field, the part the game does not draw (fovCoverage < 1) black - any runtime; 0 = as before: the narrower field handed to the runtime (Virtual Desktop showed it stretched over the whole view: zoom, fish-eye), 30..150 %
    float eyeResScale    = 1.00f;  // taille des images envoyées au casque / taille du rendu
    int   refreshRate    = 0;      // 0 = défaut du casque, sinon 72/80/90/120 (72 : le plus de marge)
    int   stereoMode     = 0;      // 0 = stéréo native (2 yeux par image), 1 = yeux alternés (secours)
    bool  perEyeHistory  = true;   // chaque œil garde son propre historique d'image (anticrénelage net)
    int   stereoSameNoise = 1;     // (.ini only) both eyes of a stereo pair draw with the same noise frame (FrameRnd) and sub-pixel jitter (engine.cpp, SameNoiseBegin; needs perEyeHistory); 0 = as before (the right eye one noise frame ahead). The noise is then equal at the same PIXEL of both eyes, not at the same 3D point (offCenter grids, disparity): the prerequisite of a stereo-consistent dither, not that dither itself. TeardownVR_perf.txt "stereoSameNoise=N" overrides it live
    bool  stereoParticles = true;  // pluie, fumée, feu, particules : la même trame de points dans les deux yeux, chaque point éclairé du même côté, et le même grain d'éclairage (ombre douce, soleil) (particles_vr.cpp : l'œil droit prend le bruit là où l'œil gauche voit le même point ; il faut stereoSameNoise=1) ; TeardownVR_perf.txt "stereoParticles=N" le remplace à chaud (tests : 2 = particules seules, 3 = éclairage seul)
    int   stereoParticleMotion = 1; // fumée qui bouge : l'œil droit lisse les particules avec leur propre mouvement, comme l'œil gauche (engine.cpp, Hook_MeshTask : frame B redonne aux particules la position que l'œil droit a dessinée à la paire précédente ; avant, le moteur ne voyait aucun mouvement de particule dans l'œil droit : traînée et lissage différents). 0 = comme avant ; TeardownVR_perf.txt "stereoParticleMotion=N" le remplace à chaud
    int   stereoParity   = -1;     // (.ini only) the right eye's front/back lighting checkerboard of the particles (shaders/gbufferparticles_vr.hlsl, STEREO_PARITY): -1 auto (1 with the VR anti-aliasing modes that cancel that checkerboard, taaMode 2-4; else 0), 0 = round 2 (the left eye's pixel's parity), 1 = own pixel's parity for particles at least 8 px wide, 2 = own pixel's parity for all. The left eye is the same in all. TeardownVR_perf.txt "stereoParity=N" overrides it live
    bool  removeFog      = false;  // retire la brume atmosphérique (le lointain paraît flou avec ; non par défaut : elle fait l'ambiance - pluie, neige)
    bool  removeBloom    = true;   // retire le halo lumineux (bloom) qui voile le lointain
    int   headsetRuntime = 0;      // casque utilisé par le jeu (au prochain lancement) : 0 = automatique (celui qui a le casque au lancement : le runtime actif de Windows, Virtual Desktop, Meta Quest Link ; vr.cpp RuntimeStartup), 1 = Virtual Desktop, 2 = Meta Quest Link / Air Link, 3 = SteamVR, 4 = le runtime OpenXR actif de Windows sans détection (l'ancien xrRuntime=0). Remplace la clé xrRuntime (12/10/2026 : tout le monde repasse en automatique)
    bool  removeDof      = true;   // coupe la profondeur de champ (flou "cinématique" du lointain)
    int   taaMode        = 4;      // anticrénelage : 0 = celui du jeu (flou au loin), 1 = net (doux), 2 = naturel, 3 = très net, 4 = maximum
    bool  offCenter      = true;   // chaque œil rend exactement le champ de vision du casque (projection décentrée)
    bool  syncParticles  = true;   // fumée et étincelles calculées au même instant pour les deux yeux
    bool  syncScriptMotion = true; // objets déplacés par un mod : au même endroit dans les deux yeux
    bool  limitCatchUp   = true;   // après une image en retard : pas de double pas de physique (saccades en chaîne)
    float ipdScale       = 1.00f;  // multiplicateur de l'écart entre les yeux
    float worldScale     = 1.00f;  // échelle du monde (1 = taille réelle)
    float sharpen        = 1.00f;  // netteté (0..1)
    float brightness     = 1.00f;
    bool  hideHud        = false;  // masque le HUD du jeu en VR
    bool  hudFix         = true;   // 2D HUD centred on the view in both eyes (otherwise seen double)
    float hudDistance    = 0.00f;  // apparent HUD distance in metres (0 = infinity, like a gun sight)
    bool  modCrosshairAim = true;  // mods' 2D crosshairs (drawn at the screen centre) moved onto the laser target
    bool  modUiPointer   = true;   // mods' mouse windows (UiMakeInteractive) used with the controllers' rays
    float modUiDistance  = 1.00f;  // those windows are seen at this distance (metres)
    bool  modUiCenterTool = true;  // in them, a ring the mod draws on its tool (revolver ammo menu): as on a desktop, centred in front of the player
    bool  modUiToolFixed = true;   // that ring's window fixed in the room (engine.cpp, PanelToolPlace): opened straight ahead at eye height, it stays when the head turns and follows the player's moves; 0 = as before, head-locked at the centre of the view
    bool  modUiPanel     = true;   // the other windows (menus: weather of Natural Phenomena...) float in the room, moved with a grip
    float modUiPanelScale = 1.0f;  // their size (x 0.8 mm per canvas unit)
    std::string modUiPanelPlaces = "30 540 370 1000 4 -9 1.05;40 710 380 650 -8 -16 0.97";  // where the player put each of them ("x0 y0 w h yaw pitch dist;...", engine.cpp)
    bool  modCameraVr    = true;   // cameras set by mods (AC130 plane view...): look around from them
    float modCameraDepth = 1.00f;  // relief of mod cameras (1 = real size, more = the world looks like a model)
    bool  modCameraZoom  = true;   // a mod camera's zoom (narrower field of view, AC130) magnifies the headset image
    int   modZoomMode    = 0;      // that zoom: 0 = the camera moves toward the aimed point (shapes kept), 1 = magnifier
    bool  laserPointer   = true;   // laser + point de visée 3D

    // --- Confort -------------------------------------------------------------
    int   turnMode       = 1;      // 0 = rotation par crans, 1 = rotation fluide (défaut)
    float snapAngle      = 30.0f;
    float smoothTurnSpeed= 120.0f; // degrés / seconde
    int   moveDirection  = 0;      // 0 = direction de la tête, 1 = main gauche
    float moveSpeed      = 1.00f;
    bool  roomscale      = true;   // le corps suit les déplacements physiques
    float heightOffset   = 0.00f;  // mètres
    bool  physicalHeight = false;  // utiliser la vraie taille du joueur plutôt que celle du jeu
    bool  vignette       = false;
    float vignetteStrength = 0.5f;

    // --- Contrôles -----------------------------------------------------------
    bool  leftHanded     = false;
    int   aimMode        = 0;      // 0 = viser avec la manette, 1 = viser avec la tête
    bool  modAimHand     = true;   // les mods (GetCameraTransform) visent comme le joueur : avec la manette
    bool  dualWield      = true;   // a mod tool of two guns (Total Overdose): one gun in each hand, each trigger fires its own
    bool  haptics        = true;
    bool  offArm         = true;   // the hand that holds no tool (and its forearm) is drawn, following its controller
    bool  offHandPhysics = true;   // that hand is a physical body (Bonelabs-like): stops at walls, pushes objects
    bool  offGrab        = true;   // its grip always grabs (touched / laser-pointed object; crouch = its trigger); off: the grip crouches too
    bool  offHandPointer = true;   // either hand points at 2D interfaces: the one that pulled its trigger on one last (never by its ray alone)
    bool  screenPointer  = true;   // the game's computer screens (the campaign's mission computer): the controllers' lasers move their cursor, the pointing hand's trigger clicks (engine.cpp, ScreenPointerUpdate)
    bool  screenOncePerPair = true; // the screen the player uses is drawn in frame A only: with frameBMode 2 a click there no longer counts twice (engine.cpp, Hook_ScreenRender)
    bool  offLaser       = true;   // a laser from that hand too (stops on our panels); with offGrab its grip pulls what it points at
    bool  grabKeepDistance = true; // an object grabbed with a grip (either hand) stays at the distance it was taken at (no 20 cm pull, no 1.5 m / 1 m minimum); the dominant stick's up / down moves it farther / closer
    float grabMinDistance = 0.50f; // (.ini only) ... the game's grab (dominant hand): at least this far from the eyes, metres (closer, the object would reach into the player)
    float grabStickSpeed  = 1.50f; // (.ini only) ... that stick pushed fully: metres per second
    bool  inventoryOnSwitch = false; // the VR inventory also shows for a moment when the stick changes the tool
    float inventoryDown  = 0.146f;  // VR inventory (worn at the belt): height below the eyes, metres (negative = above)
    float inventoryForward = 0.563f; // VR inventory: distance in front of the body, metres
    float inventoryYaw   = 17.168f;  // VR inventory: direction around the body, degrees (+ = to the right); moved with a grip
    bool  inventoryFollow = true;  // it turns with the body when the player really turns round (never with the head)
    float inventoryFollowAngle = 55.0f;  // (.ini only) ... the head that far from the body's direction, degrees
    float inventoryFollowDelay = 1.2f;   // (.ini only) ... for that long, seconds
    bool  swingGesture   = true;   // frapper avec la masse en faisant le geste
    bool  sledgeTap      = true;   // the game's sledge: a light, short trigger pull is one whole strike (pressed from sledgeTrigger, at least 0.15 s); off: the usual firm pull (55 %)
    float sledgeTrigger  = 0.20f;  // (.ini only) ... trigger travel that strikes (released at half of it)
    bool  toolCollision  = true;   // l'outil en main touche les objets
    bool  toolPhysics    = true;
    float modToolScale   = 1.0f;   // taille des armes de mods (x la taille automatique)   // l'outil est un vrai objet physique (comme Bonelabs)
    bool  toolBlock      = false;  // (sans outil physique) l'outil s'arrête contre les obstacles, sinon il les traverse en poussant
    float toolOffsetX    = 0.0f, toolOffsetY = 0.0f, toolOffsetZ = 0.0f;
    float toolPitch      = 0.0f;   // degrés

    // --- Mods (touches, souris) -----------------------------------------------
    int   modMouse       = 0;      // joystick = souris : 0 = quand un mod prend la caméra (avion, drone...), 1 = jamais
    int   modMouseStick  = 0;      // joystick qui sert de souris : 0 = celui du déplacement, 1 = celui de la rotation
    float modMouseSpeed  = 1.0f;   // vitesse de cette souris (x 1500 points par seconde, joystick à fond)
    std::string quickKey;          // touche rapide (clic court du joystick de déplacement), nom Teardown ("q"...)

    // --- Écran virtuel (menus) --------------------------------------------
    float screenDistance = 2.5f;
    float screenWidth    = 3.0f;
    bool  forceScreen    = false;  // toujours afficher sur l'écran virtuel (pas de 3D)
    bool  pauseWide      = true;   // paused level: a 16:9 virtual screen laid out like the game on a PC monitor (engine.cpp WideState); 0 = as before, the VR image's nearly square shape

    // --- Interface -----------------------------------------------------------
    float uiScale        = 1.0f;
    float uiPanelDistance = 0.90f; // settings / keys panel: where it opens, straight in front of the gaze (metres ahead
    float uiPanelDown    = 0.15f;  //  of the eyes, below them); moved with a grip
    bool  showDesktopUi  = true;
    bool  gpuWatch       = true;   // prévenir quand un autre programme occupe la carte graphique

    // --- Debug (fichier .ini uniquement) --------------------------------------
    bool  debugFakeHmd   = false;  // casque simulé pour tester sans Quest
    bool  particleMotionLog = false; // stereoParticleMotion's counters in the log (engine.cpp, once a minute); TeardownVR_perf.txt "particleMotionLog=1"
    bool  debugLua       = false;  // le script Lua envoie des mesures dans le journal
    bool  debugSyncTest  = false;  // vérifie que chaque image correspond au bon œil
    bool  debugWatchCamera = false; // rétro-ingénierie : qui lit la caméra ?
    bool  debugGpuTrace  = false;  // rétro-ingénierie : trace des commandes GPU d'une image
    bool  debugFovTest   = false;  // mesure du champ de vision réellement rendu
    bool  debugNoAim     = false;  // n'oriente pas le joueur vers la visée (diagnostic)
    bool  debugProfile   = false;  // échantillonne le jeu et explique les images lentes (saccades)
    int   perfTrace      = 0;      // measurement in the log (perf.h): 0 off, 1 per-eye sections + engine profiler + waits + CPU per thread, 2 + each mod's tick, Lua binding census and times
    bool  logSync        = false;  // the log is written by the calling thread, line by line (as before: slower, for a crash that kills the process)

    // --- Performance (fichier .ini uniquement) ----------------------------------
    int   cpuPriority    = 1;      // 1 = the game runs at "above normal" priority while the headset session runs (0 = left as is)
    bool  fastToolShapes = true;   // a part of the held, resized mod tool found from its script handle directly (no list rebuilt each frame)
    bool  fastToolScaleB = true;   // right-eye frame: the held mod tool's size reused from the left-eye frame (same tool, same shapes)
    bool  fastObjectMotion = true; // right-eye object fix: shape index reused (no map rebuilt), memory prefetched
    bool  fastLua        = true;   // the Lua mod's fast paths (poses, tags, cached results: main.lua, fastLua)
    int   frameBMode     = 2;      // right-eye frame B (frameb.h): 0 = full update (as before), 1 = mods whose left-eye tick only read are not ticked again, 2 = no update at all, frame A's per-frame requests replayed. TeardownVR_perf.txt ("frameBMode=N") overrides it live
    int   frameLatency   = 2;      // headset session: frames the game's desktop swap chain may queue (2 = the right eye's CPU work while the GPU draws the left eye; 0 = the game's own, 1). TeardownVR_perf.txt "frameLatency=N" overrides it live
    bool  holdSmooth     = true;   // after the headset runtime held an image (Virtual Desktop waiting for its stream: Wi-Fi, the headset's decoder), the game resumes where it stopped: one refresh of game time instead of the whole hold (no jump ahead, no double physics step after it)
    int   lazyBeginFrame = 1;      // (on since the mock A/B of 2026-10-12: frame A -2.6 ms under explosions, images equal) headset session: 1 = a stereo pair's left-eye frame A starts its scripts and physics while the GPU still draws the previous frame A (the engine's wait for it, TRendererD3D12::beginRender, runs at the frame's first renderer use: same commands, same order; perf.h); 2 = frames B too (test); 0 = off. TeardownVR_perf.txt "lazyBeginFrame=N" overrides it live
    std::string frameBKeepMods;    // frameBMode 1: mods always ticked again in frame B (parts of their script path, comma separated)
    int   jobSpin        = 0;      // frame A of the headset session, the job system's sleeps (perf.cpp): 1 = an idle job thread spins up to jobSpinUs before it sleeps, 2 = the main thread spins up to mainSpinUs in Job::wait before it blocks, 3 = both; 0 = they sleep at once (the game's way). Timing only: same jobs, same split. TeardownVR_perf.txt "jobSpin=N" ("jobSpinUs=N", "mainSpinUs=N") overrides it live
    int   jobSpinUs      = 30;     // jobSpin 1/3: longest spin of an idle job thread, microseconds (halved after each spin that ended in sleep, whole again after one that did not)
    int   mainSpinUs     = 20;     // jobSpin 2/3: longest spin of the main thread in Job::wait, microseconds (not for the callers whose waits are long)
    // Headset stream (Virtual Desktop; vr.cpp FillHidden, perf.h "Headset stream"):
    int   hiddenAreaFill = 0;      // what the lenses never show (the runtime's lens mask, XR_KHR_visibility_mask) cleared to black in each eye image before it goes to the headset, so the encoder spends ~no bits there: 0 off, 1 on, 2 TEST shape for a runtime without a mask (the mock; never the headset's real edge), 3 TEST in the headset: mode 1 drawn pink with no margin (pink seen = a wrong mask). TeardownVR_perf.txt "hiddenAreaFill=N" overrides it live
    float hiddenAreaGuard = 2.0f;  // ... only blocks at least this many degrees inside the hidden part (the headset re-projects a frame to a newer head pose, which shifts it by about that much)
    int   gpuPriority    = 0;      // 1 = the game's GPU scheduling class "below normal" while the headset session runs: Virtual Desktop's streamer (another process) may pre-empt its GPU work (experimental). TeardownVR_perf.txt "gpuPriority=N" overrides it live
    bool  vdStats        = true;   // stream statistics in the log: every Virtual Desktop hold (xrEndFrame longer than a refresh) counted with its duration on the Stereo frames lines, its codec / 2-pass (StreamerSettings.json, read only), the PC's network send rate
    int   parallelParticleSort = 1; // frames without a physics step (every right-eye frame B): the particle depth sort of the 3 particle systems on 3 threads at once (engine.cpp, Hook_PsSort: same results); 2 = each one also checked against the serial sort (log). TeardownVR_perf.txt "parallelParticleSort=N" overrides it live
    int   skipDeadPasses = 2;       // (2 since the mock check of 2026-10-12: 77 exact GPU comparisons during explosions, 0 different; 1 after a headset session confirms it) the bloom chain and the depth of field not drawn while their output provably cannot change the image (removeBloom: bloom factor exactly 0; removeDof: depth of field amount exactly 0; engine.cpp, Hook_BloomRender / Hook_DofRender: same image bit for bit for a finite frame); 2 = also checked on the GPU every ~5 s (exact comparison, log; a difference turns them off until restart). TeardownVR_perf.txt "skipDeadPasses=N" overrides it live
    int   skipShapeJobsB = 0;       // (off by default: the gain is ~0.02 ms and the review found the job also updates the interpolation history of dynamic shapes) render-only right-eye frame B (frameBMode 2): the shape interpolation jobs are not started again, FixObjectMotion writes what they and it would have (engine.cpp, Hook_ShapeJobs: same image); 2 = also checked shape by shape (log). TeardownVR_perf.txt "skipShapeJobsB=N" overrides it live
    int   perfDetail     = 7;      // [Debug] perfTrace >= 1: the "Perf waits / jobs / GPU" lines (perf.h): 1 = blocking job waits (histogram), GPU fence waits by caller, uploads; 2 = every job timed by class (Job::run); 4 = GPU busy / idle per frame (timestamps); 8 = GPU time per pass (timestamps in the game's command list where the pass changes, implies 4); bits added. TeardownVR_perf.txt "perfDetail=N" switches them live
};

// VR image quality: the game renders `lines` pixels high, with the width that
// matches the headset's field of view (0 = keep the PC screen's resolution).
struct RenderPreset { const char* nameEn; const char* nameFr; int lines; };
// The preset's name in the mod's language.
const char* RenderPresetName(int preset);
extern const RenderPreset kRenderPresets[];
extern const int kRenderPresetCount;
// Render size of a preset for the current headset (0x0 for "PC screen"); coverage < 1 (fovCrop): the size of
// that share of the field, each way, at the preset's pixel density.
void RenderPresetSize(int preset, int& w, int& h, float coverage = 1.0f);
// [Affichage] fovCoverage as used: 0.5..1 with fovCrop (beyond the headset's field nothing would be seen), 0.3..1.5
// without (as before).
float FovCoverage();
// The share of the field (and of the preset's size, each way) the game renders from the next start: FovCoverage()
// with fovCrop, 1 without.
float RenderCoverage();

Settings& Cfg();
void      ConfigLoad();
void      ConfigSave();
void      ConfigReset();
// Writes only these settings (their values now, copied) to the ini, on a worker thread, at most
// once per second (keys asked again meanwhile are merged, the latest value wins). For what the
// player changes during the game - a panel moved with a grip: ConfigSave rewrites every key,
// 55-270 ms on the calling thread (measured), and adds every default to a sparse ini. A
// ConfigSave meanwhile drops what is still waiting (it wrote newer values of every key).
void      ConfigSaveKeys(std::initializer_list<const char*> keys);
// [Affichage] headsetRuntime: the installed runtime's .json for that choice (1 Virtual Desktop, 2 Meta Quest Link,
// 3 SteamVR) from the runtimes' own registry list; "" when it is not installed.
std::string ConfigFindXrRuntime(int choice);
// Which of those choices a runtime .json belongs to (from its path), 0 for any other runtime or "".
int         ConfigXrRuntimeKind(const std::string& json);
// Windows' active OpenXR runtime (.json), read where the OpenXR loader reads it (HKLM ...\OpenXR\1 ActiveRuntime);
// "" when none is set or its file is missing.
std::string ConfigActiveXrRuntime();
// The runtime is chosen when the game starts (vr.cpp, RuntimeStartup): headsetRuntime 0 and no [Debug] openxrRuntime.
bool        ConfigXrAutoDetect();
// [Debug] xrAutoRuntimes: the runtimes (.json, separated by ';') a test of that choice asks instead of the real ones,
// the first one standing for Windows' runtime; "" = the real ones.
const std::string& ConfigXrAutoTestRuntimes();

// Directory containing the game exe (with trailing backslash).
std::wstring ModDirectory();

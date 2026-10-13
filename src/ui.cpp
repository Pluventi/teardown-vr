#include "ui.h"
#include "config.h"
#include "engine.h"
#include "frameb.h"
#include "gpu_watch.h"
#include "i18n.h"
#include "input.h"
#include "inventory.h"
#include "log.h"
#include "modkeys.h"
#include "particles_vr.h"
#include "pckeys.h"
#include "perf.h"
#include "render.h"
#include "vr.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace Ui {
namespace {

bool g_ready = false;
bool g_visible = false;
bool g_keysPage = false;         // the window shows the keys panel instead of the settings
ID3D12Resource* g_tex = nullptr;
D3D12_CPU_DESCRIPTOR_HANDLE g_rtv = {};
D3D12_CPU_DESCRIPTOR_HANDLE g_srvCpu = {};
D3D12_GPU_DESCRIPTOR_HANDLE g_srvGpu = {};

float g_dmx = -1, g_dmy = -1, g_dwheel = 0;
bool g_ddown = false;
bool g_vrActive = false, g_vdown = false;
float g_vx = 0, g_vy = 0;
LARGE_INTEGER g_last = {}, g_freq = {};
bool g_dirty = false;
double g_dirtyTime = 0;
int g_lastLuaRequests = 0;
double g_lastLuaCheck = 0;
bool g_luaConnected = false;
GpuWatch::Status g_gpu;          // other programs on the graphics card (refreshed every second)
uint32_t g_gpuSerialSeen = 0;
double g_bannerUntil = 0, g_bannerRepeatAt = 0;
double g_lowResUntil = 0, g_lowResRepeatAt = 0;   // banner "low resolution: restart the game" (VrRenderSizeLate)
bool g_lowResBanner = false;     // this frame's banner is that one (not the graphics card one)
bool g_banner = false;           // this frame shows the warning banner, not the panel
int g_bannerTop = 0;             // texture row of the banner (below the inventory when both are shown)
bool g_inventory = false;        // this frame shows the VR inventory (see inventory.h)
bool g_held = false;             // a hand moves the panel with its grip (SetHeld)

double Seconds()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(g_freq.QuadPart);
}

void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    Render::AllocSrv(cpu, gpu);
}
void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {}

bool CreateTarget()
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = kWidth;
    rd.Height = kHeight;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_R8G8B8A8_UNORM, { 0, 0, 0, 0 } };
    if (FAILED(Render::Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&g_tex))))
        return false;
    g_rtv = Render::AllocRtv();
    Render::Device()->CreateRenderTargetView(g_tex, nullptr, g_rtv);
    Render::AllocSrv(&g_srvCpu, &g_srvGpu);
    Render::Device()->CreateShaderResourceView(g_tex, nullptr, g_srvCpu);
    return true;
}

void ApplyStyle()
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 10;
    s.FrameRounding = 6;
    s.GrabRounding = 6;
    s.TabRounding = 6;
    s.WindowPadding = ImVec2(18, 14);
    s.FramePadding = ImVec2(10, 7);
    s.ItemSpacing = ImVec2(10, 9);
    s.GrabMinSize = 18;
    s.ScrollbarSize = 18;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.96f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.85f, 0.45f, 0.10f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.18f, 0.19f, 0.22f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.95f, 0.55f, 0.15f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.85f, 0.45f, 0.10f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.25f, 0.29f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.95f, 0.55f, 0.15f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.65f, 0.25f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.62f, 0.2f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.22f, 0.23f, 0.27f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.95f, 0.55f, 0.15f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.85f, 0.45f, 0.10f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.22f, 0.23f, 0.27f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.95f, 0.55f, 0.15f, 0.8f);
}

// --- Widgets that mark the config dirty ------------------------------------
void Dirty() { g_dirty = true; g_dirtyTime = Seconds(); }
bool Slider(const char* label, float* v, float lo, float hi, const char* fmt = "%.2f")
{
    bool c = ImGui::SliderFloat(label, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
    if (c) Dirty();
    return c;
}
bool Check(const char* label, bool* v)
{
    bool c = ImGui::Checkbox(label, v);
    if (c) Dirty();
    return c;
}
bool Radio(const char* label, int* v, int value)
{
    bool c = ImGui::RadioButton(label, v, value);
    if (c) Dirty();
    return c;
}
void Help(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}
void Section(const char* title)
{
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.62f, 0.2f, 1.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

// The panel is being moved with a grip (vr.cpp, UpdatePanelGrips): an orange frame around it,
// over everything (inside the window's Begin / End).
void DrawHeldFrame()
{
    if (!g_held) return;
    ImGui::GetForegroundDrawList()->AddRect(ImVec2(2.5f, 2.5f), ImVec2(kWidth - 2.5f, kHeight - 2.5f),
                                            IM_COL32(255, 160, 50, 255), 10.0f, 0, 5.0f);
}

void TabDisplay()
{
    Settings& s = Cfg();
    const VrStatus& st = VrGetStatus();
    // [Affichage] language (i18n.h): the same label and names in both languages (a player who cannot read the
    // panel's language finds it); Tr reads it at each call, so the whole panel switches on the next frame.
    {
        static const char* const kLang[][2] = { { "en", "English" }, { "fr", "Français" } };
        const int lang = LangFr() ? 1 : 0;
        if (ImGui::BeginCombo("Language / Langue", kLang[lang][1])) {
            for (int i = 0; i < 2; ++i)
                if (ImGui::Selectable(kLang[i][1], lang == i)) { s.language = kLang[i][0]; Dirty(); }
            ImGui::EndCombo();
        }
    }
    Section("Image");
    auto presetLabel = [](int i) {
        int w = 0, h = 0;
        RenderPresetSize(i, w, h, RenderCoverage());   // (fovCrop: only the covered part of the field)
        char b[96];
        if (h) snprintf(b, sizeof(b), "%s (%d x %d)", RenderPresetName(i), w, h);
        else snprintf(b, sizeof(b), "%s", RenderPresetName(i));
        return std::string(b);
    };
    if (ImGui::BeginCombo(Tr("Image quality (resolution)", "Qualité d'image (résolution)"),
                          presetLabel(s.renderPreset).c_str())) {
        for (int i = 0; i < kRenderPresetCount; ++i)
            if (ImGui::Selectable(presetLabel(i).c_str(), s.renderPreset == i)) { s.renderPreset = i; Dirty(); }
        ImGui::EndCombo();
    }
    Help(Tr("Size of the image the game renders for each eye, shaped like the\n"
            "headset's field of view. Higher = sharper, but heavier: the game\n"
            "renders 2 images (one per eye) for each headset frame.\n"
            "Applies the next time the game starts (with the headset connected).\n"
            "The game must be in \"Borderless window\" mode (Options > Display).",
            "Taille de l'image calculée par le jeu pour chaque œil, aux proportions\n"
            "du champ de vision du casque. Plus haut = plus net, mais plus lourd :\n"
            "le jeu calcule 2 images (une par œil) pour chaque image du casque.\n"
            "S'applique au prochain lancement du jeu (avec le casque branché).\n"
            "Le jeu doit être en mode \"Plein écran fenêtré\" (Options > Affichage)."));
    int pw = 0, ph = 0;
    RenderPresetSize(s.renderPreset, pw, ph, RenderCoverage());
    ImGui::Text(Tr("Current render: %d x %d", "Rendu actuel : %d x %d"), st.backbufferW, st.backbufferH);
    if (ph && st.sessionRunning && st.backbufferW && (pw != st.backbufferW || ph != st.backbufferH)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), Tr("  ->  %d x %d the next time the game starts",
                                                                "  ->  %d x %d au prochain lancement du jeu"), pw, ph);
    }
    if (st.recommendedH) {
        ImGui::TextDisabled(Tr("The headset recommends %d x %d per eye.", "Le casque conseille %d x %d par œil."),
                            st.recommendedW, st.recommendedH);
        // (fovCrop: a render sized for part of the field has the headset's sharpness on that part)
        if (st.backbufferH && st.backbufferH < st.recommendedH * 85 / 100 * st.cropBase)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                               Tr("Render smaller than the headset's screen: distant things look blurry.",
                                  "Rendu plus petit que l'écran du casque : le lointain paraît flou."));
    }

    float cov = s.fovCoverage * 100;
    if (Slider(Tr("Field of view covered", "Champ de vision couvert"), &cov, 50, s.fovCrop ? 100.0f : 120.0f, Tr("%.0f%%", "%.0f %%")))
        s.fovCoverage = cov / 100;
    if (s.fovCrop)
        Help(Tr("100% = the headset's whole field of view.\n"
                "Less = the game only draws the center of the view: the edges turn black,\n"
                "and the rest keeps its size and shape (no zoom, no magnifier effect).\n"
                "The game then renders a smaller image, just as sharp: smoother.\n"
                "The black border changes at once; the performance gain comes the\n"
                "next time the game starts (the render size is chosen at startup).",
                "100 % = tout le champ de vision du casque.\n"
                "Moins = le jeu ne dessine que le centre de la vue : le tour devient noir,\n"
                "ce qui reste garde sa taille et sa forme (pas de zoom, pas d'effet loupe).\n"
                "Le jeu calcule alors une image plus petite, aussi nette : plus fluide.\n"
                "Le bord noir change tout de suite ; le gain de performance vient au\n"
                "prochain lancement du jeu (la taille du rendu est choisie au démarrage)."));
    else
        Help(Tr("100% = the headset's whole field of view.\nLess = sharper image but black edges.",
                "100 % = tout le champ de vision du casque.\nMoins = image plus nette mais bords noirs."));
    float er = s.eyeResScale * 100;
    if (Slider(Tr("Resolution sent to the headset", "Résolution envoyée au casque"), &er, 50, 200, Tr("%.0f%%", "%.0f %%")))
        s.eyeResScale = er / 100;
    Help(Tr("Size of the images sent to the headset compared with the game's render (instant).\n"
            "Above 100%, the image is only enlarged: no extra detail, since the game still\n"
            "renders the same image. For a sharper image, raise \"Image quality\"\n"
            "(takes effect the next time the game starts).",
            "Taille des images transmises au casque par rapport au rendu du jeu (effet immédiat).\n"
            "Au-dessus de 100 %, l'image est seulement agrandie : pas plus de détails, car le jeu\n"
            "calcule toujours la même image. Pour une image plus nette, monte \"Qualité d'image\"\n"
            "(au prochain lancement du jeu)."));
    if (st.eyeImageW) {
        // (the eye images' size follows the slider at once: EnsureSwapchains, vr.cpp)
        ImGui::TextDisabled(Tr("Images sent to the headset: %d x %d per eye", "Images envoyées au casque : %d x %d par œil"),
                            st.eyeImageW, st.eyeImageH);
        if (s.eyeResScale > 1.001f) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                               Tr(" enlarged: no more detail than the render", " agrandies : pas plus de détails que le rendu"));
        }
    }

    std::string cur = s.refreshRate > 0 ? std::to_string(s.refreshRate) + " Hz" : Tr("Default", "Par défaut");
    if (ImGui::BeginCombo(Tr("Headset refresh rate", "Fréquence du casque"), cur.c_str())) {
        if (ImGui::Selectable(Tr("Default", "Par défaut"), s.refreshRate == 0)) { s.refreshRate = 0; Dirty(); }
        std::vector<float> rates = st.refreshRates;
        if (rates.empty()) rates = { 72, 80, 90, 120 };
        for (float r : rates) {
            std::string l = std::to_string((int)(r + 0.5f)) + " Hz";
            if (ImGui::Selectable(l.c_str(), s.refreshRate == (int)(r + 0.5f))) { s.refreshRate = (int)(r + 0.5f); Dirty(); }
        }
        ImGui::EndCombo();
    }
    Help(Tr("The game must run at 2x this rate (one eye per frame).\nE.g. 72 Hz -> 144 FPS in the game.",
            "Le jeu doit tourner à 2x cette fréquence (un œil par image).\nEx : 72 Hz -> 144 FPS dans le jeu."));

    Section(Tr("VR rendering", "Rendu VR"));
    ImGui::Text(Tr("Stereo:", "Stéréo :"));
    ImGui::SameLine();
    Radio(Tr("Synchronized (recommended)", "Synchronisée (recommandé)"), &s.stereoMode, 0);
    ImGui::SameLine();
    Radio(Tr("Alternating eyes", "Yeux alternés"), &s.stereoMode, 1);
    Help(Tr("Synchronized: both eyes show exactly the same moment of the game,\n"
            "like a native VR game (the game renders each eye's image in turn,\n"
            "with game time frozen for the 2nd eye).\n"
            "Alternating eyes: fallback mode if the game version isn't recognized.",
            "Synchronisée : les deux yeux montrent exactement le même instant du jeu,\n"
            "comme un jeu VR natif (le jeu calcule l'image de chaque œil à la suite,\n"
            "le temps du jeu étant figé pour le 2e œil).\n"
            "Yeux alternés : mode de secours si la version du jeu n'est pas reconnue."));
    Check(Tr("Sharp image per eye (recommended)", "Image nette par œil (recommandé)"), &s.perEyeHistory);
    Help(Tr("The game smooths its image using previous frames (anti-aliasing,\n"
            "lighting). On: each eye keeps its own image history.\n"
            "Off: the two eyes get mixed together (blurrier image).",
            "Le jeu lisse son image avec les images précédentes (anticrénelage,\n"
            "éclairage). Coché : chaque œil garde sa propre mémoire d'image.\n"
            "Décoché : les deux yeux se mélangent (image plus floue)."));
    {
        static const char* kTaaEn[] = { "Game's own (blurry in the distance)", "Sharp (smooth)", "Natural (recommended)",
                                        "Very sharp", "Maximum (flickers a bit)" };
        static const char* kTaaFr[] = { "Celui du jeu (flou au loin)", "Net (doux)", "Naturel (recommandé)", "Très net",
                                        "Maximum (scintille un peu)" };
        const char* const* kTaa = LangFr() ? kTaaFr : kTaaEn;
        int m = s.taaMode < 0 || s.taaMode > 4 ? 2 : s.taaMode;
        if (ImGui::BeginCombo(Tr("Distance sharpness", "Netteté au loin"), kTaa[m])) {
            for (int i = 0; i < 5; ++i)
                if (ImGui::Selectable(kTaa[i], m == i)) { s.taaMode = i; Dirty(); }
            ImGui::EndCombo();
        }
        Help(Tr("The game's anti-aliasing blends each frame with the previous ones. In a\n"
                "headset your head always moves a little, and this blending blurs the\n"
                "distance (sharp only when you turn your head fast).\n"
                "\"Natural\" keeps distant detail (trees) without an oversharpened look.\n"
                "\"Very sharp\": a bit more detail still, slight halo on edges.\n"
                "\"Sharp (smooth)\" smooths more. \"Maximum\": the sharpest, but distant\n"
                "edges flicker. Takes effect immediately.",
                "L'anticrénelage du jeu mélange chaque image avec les précédentes. Dans un\n"
                "casque la tête bouge toujours un peu, et ce mélange rend le lointain flou\n"
                "(net seulement quand on tourne vite la tête).\n"
                "\"Naturel\" garde le détail lointain (arbres) sans effet de sur-netteté.\n"
                "\"Très net\" : encore un peu plus de détail, léger liseré sur les contours.\n"
                "\"Net (doux)\" lisse davantage. \"Maximum\" : le plus net, mais les contours\n"
                "lointains scintillent. Effet immédiat."));
        if (m != 0 && !GameShadersBuilt())
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                               Tr("  -> not in this build: the game's own anti-aliasing is used",
                                  "  -> absent de cette version : l'anticrénelage du jeu est utilisé"));
    }
    {
        // [Performance] frameBMode 2 + frameLatency 2 + lazyBeginFrame 1 (frameb.h, perf.cpp FrameLatency / lazy
        // beginFrame)
        bool fast = s.frameBMode == 2 && s.frameLatency >= 2 && s.lazyBeginFrame >= 1;
        if (Check(Tr("CPU optimization (recommended)", "Optimisation du processeur (recommandé)"), &fast)) {
            s.frameBMode = fast ? 2 : 0;
            s.frameLatency = fast ? 2 : 0;
            s.lazyBeginFrame = fast ? 1 : 0;
        }
        Help(Tr("The right eye is only drawn: the game doesn't redo all its work a 2nd\n"
                "time (scripts, mods, player), the CPU prepares the right eye while\n"
                "the graphics card is still drawing the left one, and the next frame\n"
                "starts its work (scripts, physics) without waiting for the previous\n"
                "one to finish drawing.\n"
                "Same image, less stutter when things explode everywhere.\n"
                "Off: as before (to compare). Takes effect immediately.",
                "L'œil droit est seulement dessiné : le jeu ne refait pas tous ses calculs\n"
                "une 2e fois (scripts, mods, joueur), et le processeur prépare l'œil droit\n"
                "pendant que la carte graphique dessine encore le gauche, et l'image\n"
                "suivante commence ses calculs (scripts, physique) sans attendre la fin du\n"
                "dessin de la précédente.\n"
                "Même image, moins de saccades quand ça explose partout.\n"
                "Décoché : comme avant (pour comparer). Effet immédiat."));
        if (fast && !FrameB::On())
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                               Tr("  -> fully on the next time the game starts", "  -> complet au prochain lancement du jeu"));
    }
    // [Performance] holdSmooth (vr.cpp BeginXrFrame)
    Check(Tr("No jump after a streaming hitch (recommended)", "Reprise sans saut après un blocage du streaming (recommandé)"),
          &s.holdSmooth);
    Help(Tr("When Virtual Desktop holds back a frame (Wi-Fi or headset decoding too slow,\n"
            "especially H.264+ at a very high bitrate), the headset keeps the previous image.\n"
            "On: the game resumes exactly where it stopped, without objects jumping\n"
            "forward, and without running physics twice right after.\n"
            "Off: the game makes up the lost time all at once (as before).",
            "Quand Virtual Desktop retient une image (Wi-Fi ou décodage du casque trop lents,\n"
            "surtout en H.264+ à très haut débit), le casque garde l'image d'avant.\n"
            "Coché : le jeu reprend exactement là où il s'était arrêté, sans que les objets\n"
            "sautent en avant, et sans double calcul de physique juste après.\n"
            "Décoché : le jeu rattrape le temps perdu d'un coup (comme avant)."));
    Check(Tr("Exact field of view per eye", "Champ de vision exact par œil"), &s.offCenter);
    Help(Tr("Each eye of the headset sees farther outward than toward the nose. On:\n"
            "the game renders exactly that field (off-center image), instead of a\n"
            "symmetric image where ~15% of the pixels are never shown. Sharper at the\n"
            "same cost. The image size adjusts the next time the game starts.",
            "Chaque œil du casque voit plus loin vers l'extérieur que vers le nez. Coché :\n"
            "le jeu calcule exactement ce champ (image décentrée), au lieu d'une image\n"
            "symétrique dont ~15 % des pixels ne sont jamais affichés. Plus net à coût\n"
            "égal. La taille d'image s'ajuste au prochain lancement du jeu."));
    Check(Tr("Remove distant haze", "Retirer la brume au loin"), &s.removeFog);
    Help(Tr("The game adds atmospheric haze that makes the distance blurry and\n"
            "washed out (it is computed at low resolution, then smoothed).\n"
            "On: haze removed during VR (restored when you quit). Off (the\n"
            "default): the level's and the weather's haze (rain, snow) stays.",
            "Le jeu ajoute une brume atmosphérique qui rend le lointain flou et\n"
            "délavé (elle est calculée en basse résolution puis lissée).\n"
            "Coché : brume retirée pendant la VR (remise en quittant). Décoché (par\n"
            "défaut) : la brume du niveau et de la météo (pluie, neige) reste."));
    Check(Tr("Remove light glow (bloom)", "Retirer le halo lumineux (bloom)"), &s.removeBloom);
    Help(Tr("The game spreads the light of the sky and lamps into a glow (\"bloom\")\n"
            "that veils distant objects like a blur. On: glow removed during VR.",
            "Le jeu étale la lumière du ciel et des lampes en un halo (\"bloom\") qui\n"
            "voile les objets lointains comme un flou. Coché : halo retiré pendant la VR."));
    Check(Tr("Remove depth blur", "Retirer le flou de profondeur"), &s.removeDof);
    Help(Tr("The game can blur what is far from (or near) the point you look at, like\n"
            "a movie camera (\"depth of field\"). On: never this blur.",
            "Le jeu peut flouter ce qui est loin (ou près) du point regardé, comme une\n"
            "caméra de cinéma (\"profondeur de champ\"). Coché : jamais de ce flou."));
    // [Affichage] stereoParticles (particles_vr.cpp)
    Check(Tr("Same particles in both eyes (rain, smoke)", "Particules identiques pour les deux yeux (pluie, fumée)"),
          &s.stereoParticles);
    Help(Tr("Rain, smoke (extinguisher, explosions), fire and dust are drawn as small,\n"
            "tightly packed dots that the image then smooths. Before, each eye saw\n"
            "different dots on the same smoke: the eyes can't fuse them (flicker,\n"
            "eye strain).\n"
            "On: both eyes see the same dots, at the same spot on each particle,\n"
            "lit from the same side, with the same light grain (soft shadow,\n"
            "sun). The left eye doesn't change. Takes effect immediately.",
            "La pluie, la fumée (extincteur, explosions), le feu et la poussière sont\n"
            "dessinés en petits points serrés, que l'image lisse ensuite. Avant, chaque\n"
            "œil voyait d'autres points sur la même fumée : les yeux ne peuvent pas les\n"
            "fusionner (scintillement, fatigue des yeux).\n"
            "Coché : les deux yeux voient les mêmes points, posés au même endroit de\n"
            "chaque particule, éclairés du même côté, avec le même grain de lumière\n"
            "(ombre douce, soleil). L'œil gauche ne change pas. Effet immédiat."));
    if (s.stereoParticles && !GameShadersBuilt())
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                           Tr("  -> not in this build (made without the game-derived shaders)",
                              "  -> absent de cette version (compilée sans les shaders tirés du jeu)"));
    else if (s.stereoParticles && ParticlesVrStatus() == 0)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                           Tr("  -> unavailable (game version not recognized)",
                              "  -> indisponible (version du jeu non reconnue)"));
    // [Affichage] stereoParticleMotion (engine.cpp, Hook_MeshTask)
    bool particleMotion = s.stereoParticleMotion != 0;
    if (Check(Tr("Moving smoke: same smoothing in both eyes", "Fumée qui bouge : même lissage dans les deux yeux"),
              &particleMotion))
        s.stereoParticleMotion = particleMotion ? 1 : 0;
    Help(Tr("The game smooths smoke from frame to frame by following each particle's\n"
            "motion. Before, the right eye never saw that motion: its smoke smeared\n"
            "and was smoothed differently than in the left eye.\n"
            "On: the right eye follows the smoke's real motion, like the left eye.\n"
            "The left eye doesn't change. Takes effect immediately.",
            "Le jeu lisse la fumée d'une image à l'autre en suivant le mouvement de\n"
            "chaque particule. Avant, l'œil droit ne voyait jamais ce mouvement : sa\n"
            "fumée traînait et était lissée autrement que dans l'œil gauche.\n"
            "Coché : l'œil droit suit le vrai mouvement de la fumée, comme l'œil gauche.\n"
            "L'œil gauche ne change pas. Effet immédiat."));
    if (s.stereoMode == 0 && !EngineStereoAvailable())
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s",
                           Tr("Game version not recognized: alternating eyes used.",
                              "Version du jeu non reconnue : yeux alternés utilisés."));
    Slider(Tr("Sharpening", "Netteté"), &s.sharpen, 0, 1);
    Slider(Tr("Brightness", "Luminosité"), &s.brightness, 0.5f, 1.5f);
    float ipd = s.ipdScale * 100;
    if (Slider(Tr("Eye separation", "Écart entre les yeux"), &ipd, 0, 200, Tr("%.0f%%", "%.0f %%"))) s.ipdScale = ipd / 100;
    Help(Tr("0% = flat image. Above 100%, the 3D effect gets stronger.",
            "0 % = image plate. Augmente l'effet 3D au-delà de 100 %."));
    Slider(Tr("World scale", "Échelle du monde"), &s.worldScale, 0.5f, 2.0f);
    Help(Tr("Higher = you feel taller (the world looks smaller).",
            "Plus grand = tu te sens plus grand (le monde paraît plus petit)."));
    Check(Tr("Hide the game's HUD", "Masquer le HUD du jeu"), &s.hideHud);
    Check(Tr("HUD in the center of view", "HUD au centre de la vue"), &s.hudFix);
    Help(Tr("Each eye sees farther outward than toward the nose: the center of the\n"
            "image isn't straight ahead, and a crosshair drawn in the center (game\n"
            "HUD, mods) was seen double. On: the HUD is placed straight ahead in both\n"
            "eyes (or on a mod camera's axis, where its crosshair aims).\n"
            "The top and edges of the HUD may then go slightly out of the image.",
            "Chaque œil voit plus loin vers l'extérieur que vers le nez : le centre de\n"
            "l'image n'est pas droit devant, et un viseur dessiné au centre (HUD du jeu,\n"
            "mods) était vu dédoublé. Coché : le HUD est placé droit devant dans les deux\n"
            "yeux (ou sur l'axe de la caméra d'un mod, là où son viseur vise).\n"
            "Le haut et les bords du HUD peuvent alors sortir un peu de l'image."));
    if (s.hudFix) {
        Slider(Tr("HUD distance", "Distance du HUD"), &s.hudDistance, 0, 5,
               s.hudDistance < 0.1f ? Tr("at infinity", "à l'infini") : "%.1f m");
        Help(Tr("0 = at infinity, like an aircraft gunsight (best for aiming far away).",
                "0 = à l'infini, comme un viseur d'avion (le mieux pour viser au loin)."));
        Check(Tr("Mod crosshairs on the controller's aim", "Viseurs des mods sur la visée de la manette"), &s.modCrosshairAim);
        Help(Tr("Mod weapons draw their crosshair in the center of the screen (portal gun\n"
                "logo, chaingun crosshair...), so straight ahead of your head in VR,\n"
                "while they shoot where the controller aims. On: that crosshair is placed\n"
                "on the point the laser aims at, at its distance. The rest of the HUD\n"
                "doesn't move.",
                "Les armes de mods dessinent leur viseur au centre de l'écran (logo du\n"
                "portal gun, viseur du chaingun...), donc droit devant la tête en VR, alors\n"
                "qu'elles tirent là où vise la manette. Coché : ce viseur est placé sur le\n"
                "point visé par le laser, à sa distance. Le reste du HUD ne bouge pas."));
        Check(Tr("Mod menus with the controller", "Menus des mods à la manette"), &s.modUiPointer);
        Help(Tr("When a mod opens a window used with the mouse (THE EVERYTHING REVOLVER's\n"
                "bullet menu, Natural Phenomena's weather, item menus...), it is shown\n"
                "sharp, and the controller aiming at it (left or right) moves the mouse:\n"
                "an orange dot. Its trigger = left click (held = drag), its grip = right\n"
                "click (if the window uses it). Pulling the other hand's trigger while\n"
                "aiming at the window gives that hand the pointer.\n"
                "Meanwhile, the left trigger no longer makes you crouch.",
                "Quand un mod ouvre une fenêtre à la souris (menu des balles du revolver\n"
                "THE EVERYTHING REVOLVER, météo de Natural Phenomena, menus d'objets...),\n"
                "elle est affichée nette, et la manette qui la vise (gauche ou droite)\n"
                "déplace la souris : un point orange. Sa gâchette = clic gauche (maintenue\n"
                "= glisser), son grip = clic droit (si la fenêtre s'en sert). Tirer la\n"
                "gâchette de l'autre main en visant la fenêtre lui donne le pointeur.\n"
                "Pendant ce temps, la gâchette gauche ne fait plus s'accroupir."));
        if (s.modUiPointer) {
            Check(Tr("Mod menus as floating windows", "Menus des mods en fenêtre flottante"), &s.modUiPanel);
            Help(Tr("On: a mod menu (Natural Phenomena's weather, item menu...) opens as a\n"
                    "window placed in the room, straight ahead of your gaze. It no longer\n"
                    "follows your head. To move it: squeeze the grip (the one you grab\n"
                    "objects with) while aiming at it, move your hand, let go. If the\n"
                    "window uses right click, aim just outside its edge (the dot turns\n"
                    "blue, its corners are marked) or squeeze the other hand's grip.\n"
                    "Put to the side, it stays there as long as you look ahead, toward it\n"
                    "or in between. If you really turn around or walk away, it follows\n"
                    "you and takes back its place relative to your gaze (straight ahead\n"
                    "if you haven't moved it). Its place is remembered for that menu.\n"
                    "Off: as before, the window stays in front of your head.\n"
                    "(The revolver wheel has its own settings, further down.)",
                    "Coché : un menu de mod (météo de Natural Phenomena, menu d'objets...)\n"
                    "s'ouvre comme une fenêtre posée dans la pièce, droit devant ton regard.\n"
                    "Elle ne suit plus ta tête. Pour la déplacer : serre le grip (le même\n"
                    "que pour attraper un objet) en la visant, bouge la main, relâche. Si la\n"
                    "fenêtre se sert du clic droit, vise juste à côté de son bord (le point\n"
                    "devient bleu, ses coins sont marqués) ou serre le grip de l'autre main.\n"
                    "Mise sur le côté, elle y reste tant que tu regardes devant toi, vers\n"
                    "elle ou entre les deux. Si tu te retournes ou t'éloignes vraiment, elle\n"
                    "te suit et reprend sa place par rapport à ton regard (droit devant si\n"
                    "tu ne l'as pas déplacée). Sa place est retenue pour ce menu.\n"
                    "Décoché : comme avant, la fenêtre reste devant ta tête.\n"
                    "(La roue du revolver a ses propres réglages, plus bas.)"));
            if (s.modUiPanel) {
                float pct = s.modUiPanelScale * 100;
                if (Slider(Tr("Mod menu size", "Taille des menus des mods"), &pct, 50, 200, Tr("%.0f%%", "%.0f %%")))
                    s.modUiPanelScale = pct / 100;
                Help(Tr("Size of the floating windows (100%: Natural Phenomena's weather menu\n"
                        "is 30 x 80 cm, at 1 m).",
                        "Taille des fenêtres flottantes (100 % : la météo de Natural Phenomena\n"
                        "fait 30 x 80 cm, à 1 m)."));
                if (!s.modUiPanelPlaces.empty()) {
                    if (ImGui::Button(Tr("Forget mod menu positions", "Oublier les places des menus des mods"),
                                      ImVec2(380, 0))) {
                        s.modUiPanelPlaces.clear();
                        Dirty();
                    }
                    Help(Tr("Each moved menu will open straight ahead of your gaze again.",
                            "Chaque menu déplacé s'ouvrira de nouveau droit devant ton regard."));
                }
            }
            Slider(Tr("Mod menu distance", "Distance des menus des mods"), &s.modUiDistance, 0.5f, 3, "%.1f m");
            Help(Tr("Distance at which menus open (a moved menu keeps its own)\n"
                    "and at which the revolver wheel is shown.",
                    "Distance à laquelle les menus s'ouvrent (un menu déplacé garde la sienne)\n"
                    "et à laquelle la roue du revolver est vue."));
            Check(Tr("Revolver wheel in front of me", "Roue du revolver devant moi"), &s.modUiCenterTool);
            Help(Tr("THE EVERYTHING REVOLVER's bullet menu (and what a mod draws over its\n"
                    "weapon in its window): drawn as on the PC screen, in front of you.\n"
                    "It no longer follows your hand or wrist. Off: around the weapon in your hand.",
                    "Menu des balles de THE EVERYTHING REVOLVER (et ce qu'un mod dessine sur\n"
                    "son arme dans sa fenêtre) : dessiné comme sur l'écran du PC, devant toi.\n"
                    "Il ne suit plus la main ni le poignet. Décoché : autour de l'arme dans ta main."));
            if (s.modUiCenterTool) {
                Check(Tr("Revolver wheel fixed in the room", "Roue du revolver fixe dans la pièce"), &s.modUiToolFixed);
                Help(Tr("On: the bullet menu opens straight ahead of you, at eye level (even if\n"
                        "you're looking down at your weapon at that moment), and stays in place\n"
                        "when you turn or lower your head. It only follows you when you move\n"
                        "around. The grip doesn't move it (it stays the revolver's right click):\n"
                        "close and reopen the menu (X) to bring it back in front of you.\n"
                        "Off: as before, always in the center of your view.",
                        "Coché : le menu des balles s'ouvre droit devant toi, à hauteur des yeux\n"
                        "(même si tu regardes ton arme en bas à ce moment-là), et reste à sa place\n"
                        "quand tu tournes ou baisses la tête. Il te suit seulement quand tu te\n"
                        "déplaces. Le grip ne le déplace pas (il reste le clic droit du revolver) :\n"
                        "ferme et rouvre le menu (X) pour le remettre devant toi.\n"
                        "Décoché : comme avant, toujours au centre de ta vue."));
            }
        }
    }
    Check(Tr("Mod cameras in VR", "Caméras des mods en VR"), &s.modCameraVr);
    Help(Tr("When a mod forces its own camera (AC130 plane view, drone...): looking\n"
            "straight ahead shows its view, and you look around from it with your\n"
            "head. Off: only the head's rotation is kept.",
            "Quand un mod impose sa caméra (vue de l'avion de l'AC130, drone...) :\n"
            "regarder droit devant montre sa vue, et tu regardes autour de toi depuis\n"
            "elle avec la tête. Décoché : seule la rotation de la tête est gardée."));
    if (s.modCameraVr) {
        Slider(Tr("Mod camera depth", "Relief des caméras de mods"), &s.modCameraDepth, 1, 20, "x%.0f");
        Help(Tr("1 = real size. Higher: scenery seen from afar (from the plane)\n"
                "looks more 3D, like a scale model.",
                "1 = taille réelle. Plus grand : le décor vu de loin (depuis l'avion)\n"
                "paraît plus en relief, comme une maquette."));
        Check(Tr("Mod camera zoom", "Zoom des caméras de mods"), &s.modCameraZoom);
        Help(Tr("When the mod zooms (AC130 plane: right stick up / down), what you aim\n"
                "at gets bigger in the headset, up to x16.\n"
                "Off: the view always keeps its real size.",
                "Quand le mod zoome (avion de l'AC130 : joystick droit haut / bas), ce que tu\n"
                "vises grossit dans le casque, jusqu'à x16.\n"
                "Décoché : la vue garde toujours sa taille réelle."));
        if (s.modCameraZoom) {
            ImGui::Text(Tr("Zoom type:", "Type de zoom :"));
            ImGui::SameLine();
            Radio(Tr("Move closer (recommended)", "Rapprochement (recommandé)"), &s.modZoomMode, 0);
            ImGui::SameLine();
            Radio(Tr("Magnifier", "Loupe"), &s.modZoomMode, 1);
            Help(Tr("Move closer: the camera moves toward the point you aim at, like a drone.\n"
                    "The target gets bigger with no distortion at all, you look around\n"
                    "normally and depth stays natural. The camera always stops before the\n"
                    "ground or a wall.\n"
                    "Magnifier (old mode): the image is enlarged in the headset. Its edges\n"
                    "warp when you turn your head (fisheye effect) and depth looks flat.",
                    "Rapprochement : la caméra avance vers le point visé, comme un drone. La cible\n"
                    "grossit sans aucune déformation, tu regardes autour de toi normalement et le\n"
                    "relief reste naturel. La caméra s'arrête toujours avant le sol ou un mur.\n"
                    "Loupe (ancien mode) : l'image est agrandie dans le casque. Ses bords se\n"
                    "déforment quand on tourne la tête (effet fisheye) et le relief paraît plat."));
        }
    }
    Check(Tr("Aiming laser", "Laser de visée"), &s.laserPointer);

    Section(Tr("Smoothness", "Fluidité"));
    Check(Tr("Mod objects in the same place in both eyes", "Objets des mods au même endroit dans les deux yeux"),
          &s.syncScriptMotion);
    Help(Tr("The right eye's frame runs the mods' scripts again. A mod that moves an\n"
            "object by a fixed step each time (the AC130 plane) moved it between the\n"
            "two eyes: you saw it double, as with a wrong eye separation.\n"
            "On: each eye sees objects in the same place. Takes effect immediately.",
            "L'image de l'œil droit relance les scripts des mods. Un mod qui avance un\n"
            "objet d'un pas fixe à chaque fois (l'avion de l'AC130) le déplaçait entre\n"
            "les deux yeux : on le voyait en double, comme avec un mauvais écart entre\n"
            "les yeux. Coché : chaque œil voit les objets au même endroit. Effet immédiat."));
    Check(Tr("Particles synced between eyes", "Particules synchronisées entre les yeux"), &s.syncParticles);
    Help(Tr("The game computed smoke, sparks and debris during the right eye's frame:\n"
            "the left eye saw them one frame late (up to 10 cm off during\n"
            "explosions). On: both eyes see the same particles, computed with the\n"
            "rest of the game. Takes effect immediately.",
            "Le jeu calculait la fumée, les étincelles et les débris pendant l'image de\n"
            "l'œil droit : l'œil gauche les voyait avec une image de retard (jusqu'à\n"
            "10 cm d'écart pendant les explosions). Coché : les deux yeux voient les\n"
            "mêmes particules, calculées avec le reste du jeu. Effet immédiat."));
    Check(Tr("No catch-up after a stutter", "Pas de rattrapage après une saccade"), &s.limitCatchUp);
    Help(Tr("After a late frame, the game normally runs two physics steps at once to\n"
            "catch up: the next frame is even slower, and stutters pile up during\n"
            "big explosions. On: one step per frame; the game slows down\n"
            "imperceptibly instead.",
            "Après une image en retard, le jeu calcule normalement deux pas de physique\n"
            "d'un coup pour rattraper : l'image suivante est encore plus lente, et les\n"
            "saccades s'enchaînent pendant les grosses explosions. Coché : un seul pas\n"
            "par image ; le jeu ralentit imperceptiblement à la place."));
}

void TabComfort()
{
    Settings& s = Cfg();
    Section(Tr("Turning (right stick)", "Rotation (joystick droit)"));
    Radio(Tr("Snap", "Par crans"), &s.turnMode, 0);
    ImGui::SameLine();
    Radio(Tr("Smooth", "Fluide"), &s.turnMode, 1);
    if (s.turnMode == 0) {
        const int angles[] = { 15, 22, 30, 45, 60, 90 };
        ImGui::Text(Tr("Angle:", "Angle :"));
        for (int a : angles) {
            ImGui::SameLine();
            char l[16];
            snprintf(l, sizeof(l), "%d°", a);
            if (ImGui::RadioButton(l, (int)s.snapAngle == a)) { s.snapAngle = (float)a; Dirty(); }
        }
    } else {
        Slider(Tr("Turn speed", "Vitesse de rotation"), &s.smoothTurnSpeed, 30, 360, "%.0f °/s");
    }

    Section(Tr("Movement (left stick)", "Déplacement (joystick gauche)"));
    Radio(Tr("Where my head looks", "Dans la direction de la tête"), &s.moveDirection, 0);
    ImGui::SameLine();
    Radio(Tr("Where my left hand points", "De la main gauche"), &s.moveDirection, 1);
    Slider(Tr("Move speed", "Vitesse de déplacement"), &s.moveSpeed, 0.3f, 2.0f, "x%.2f");
    Check(Tr("Follow my real movements (roomscale)", "Suivre mes déplacements réels (roomscale)"), &s.roomscale);
    Help(Tr("When you walk around your room, your character moves with you.\n"
            "Walls stop it (your head can then go through them).",
            "Quand tu marches dans ta pièce, ton personnage avance avec toi.\n"
            "Les murs l'arrêtent (ta tête peut alors passer à travers)."));

    Section(Tr("Height", "Taille"));
    Check(Tr("Use my real height", "Utiliser ma vraie taille"), &s.physicalHeight);
    Help(Tr("On: the camera is at your head's real height.\nOff: the game character's height (recommended).",
            "Coché : la caméra est à la hauteur réelle de ta tête.\nDécoché : hauteur du personnage du jeu (conseillé)."));
    Slider(Tr("Height offset", "Décalage de hauteur"), &s.heightOffset, -0.6f, 0.6f, "%.2f m");

    Section(Tr("Anti-nausea", "Anti-nausée"));
    Check(Tr("Vignette while moving", "Vignette pendant les déplacements"), &s.vignette);
    if (s.vignette) Slider(Tr("Vignette strength", "Intensité de la vignette"), &s.vignetteStrength, 0.1f, 1.0f);

    ImGui::Spacing();
    if (ImGui::Button(Tr("Recenter view (F8)", "Recentrer la vue (F8)"), ImVec2(320, 0))) VrRequestRecenter();
}

void TabControls()
{
    Settings& s = Cfg();
    Section(Tr("Hands", "Mains"));
    Check(Tr("Left-handed mode (aim with the left hand)", "Mode gaucher (viser avec la main gauche)"), &s.leftHanded);
    ImGui::Text(Tr("Aim with:", "Viser avec :"));
    ImGui::SameLine();
    Radio(Tr("the controller", "la manette"), &s.aimMode, 0);
    ImGui::SameLine();
    Radio(Tr("the head", "la tête"), &s.aimMode, 1);
    if (s.aimMode == 0) {
        Check(Tr("Mods aim with the controller too", "Les mods visent aussi avec la manette"), &s.modAimHand);
        Help(Tr("On: mods that aim with the camera (the AC130's air strike from the ground,\n"
                "mod weapons, creative mode...) aim where your controller points (the\n"
                "laser), like the game's weapons. Off: they aim where you look (your head).",
                "Coché : les mods qui visent avec la caméra (frappe aérienne de l'AC130 au sol,\n"
                "armes de mods, mode créatif...) visent là où pointe ta manette (le laser), comme\n"
                "les armes du jeu. Décoché : ils visent là où tu regardes (ta tête)."));
        Check(Tr("Dual wield: one weapon in each hand (Total Overdose)",
                 "Armes doubles : une arme dans chaque main (Total Overdose)"), &s.dualWield);
        Help(Tr("A mod weapon made of two pistols (Total Overdose): one pistol in each\n"
                "hand. Each trigger fires its own pistol, from its barrel, toward the\n"
                "point that hand's laser aims at (right trigger = right pistol,\n"
                "left trigger = left pistol), like two real guns.\n"
                "Meanwhile the free hand's grip (left, or right in left-handed mode)\n"
                "makes you crouch (while squeezed): that hand holds a weapon, it\n"
                "doesn't grab anything. The other hand's grip does nothing (the mod's\n"
                "dodge, a roll on the ground seen from outside, is disabled in VR).\n"
                "Off: both pistols in the right hand, as on PC.",
                "Une arme de mod faite de deux pistolets (Total Overdose) : un pistolet dans\n"
                "chaque main. Chaque gâchette tire avec son pistolet, depuis son canon, vers\n"
                "le point visé par le laser de cette main (gâchette droite = pistolet droit,\n"
                "gâchette gauche = pistolet gauche), comme deux vraies armes.\n"
                "Pendant ce temps le grip de la main libre (gauche, ou droite en mode\n"
                "gaucher) fait s'accroupir (tant qu'il est serré) : cette main tient une\n"
                "arme, elle n'attrape rien. Le grip de l'autre main ne fait rien (l'esquive\n"
                "du mod, une roulade au sol vue de l'extérieur, est coupée en VR).\n"
                "Décoché : les deux pistolets dans la main droite, comme sur PC."));
    }
    Check(Tr("Vibration", "Vibrations"), &s.haptics);
    Check(Tr("Free hand's arm (left, or right in left-handed mode)", "Bras de la main libre (gauche, ou droite en mode gaucher)"),
          &s.offArm);
    Help(Tr("Your hand without a tool and its forearm are drawn in the game, with\n"
            "your character's voxels. They follow your controller.",
            "Ta main sans outil et son avant-bras sont dessinés dans le jeu, avec les\n"
            "voxels de ton personnage. Ils suivent ta manette."));
    if (s.offArm) {
        Check(Tr("  Physical hand (like Bonelabs)", "  Main physique (comme Bonelabs)"), &s.offHandPhysics);
        Help(Tr("On: your free hand is a real physical object, as strong as the hand\n"
                "holding the sledge. It goes through nothing: it stops against walls and\n"
                "tables, slides along them, pushes what moves (a very heavy object\n"
                "resists). The controller only vibrates on real contact. It never pushes\n"
                "you and lets shots, the tool and what the game grabs pass through.\n"
                "Off: it goes through everything.",
                "Coché : ta main libre est un vrai objet physique, aussi forte que ta main qui\n"
                "tient la masse. Elle ne traverse rien : elle s'arrête contre les murs et les\n"
                "tables, glisse dessus, pousse ce qui bouge (un objet très lourd résiste). La\n"
                "manette vibre seulement au vrai contact. Elle ne te pousse jamais et laisse\n"
                "passer les tirs, l'outil et ce que le jeu attrape.\n"
                "Décoché : elle traverse tout."));
        if (s.offHandPhysics) {
            Check(Tr("  ... grab with the grip", "  ... attraper avec le grip"), &s.offGrab);
            Help(Tr("On: the free hand's grip grabs, like the other hand's grip.\n"
                    "A movable object under the hand (against the fist or within 8 cm):\n"
                    "you grab it in your hand. With the left hand laser, from a distance\n"
                    "like the right hand: aim at an object (within 3 m), the hand icon\n"
                    "appears on it; squeeze the grip, it stays at its distance at the end\n"
                    "of the laser and follows your hand. Release the grip to drop or throw it.\n"
                    "Crouch: left trigger.\n"
                    "Off: this grip also makes you crouch, like the left trigger.",
                    "Coché : le grip de la main libre attrape, comme le grip de l'autre main.\n"
                    "Un objet qui bouge sous la main (contre le poing ou à moins de 8 cm) :\n"
                    "tu l'attrapes dans la main. Avec le laser de la main gauche, de loin\n"
                    "comme la main droite : vise un objet (à moins de 3 m), l'icône de main\n"
                    "apparaît dessus ; serre le grip, il reste à sa distance au bout du laser\n"
                    "et suit ta main. Relâche le grip pour le lâcher ou le lancer.\n"
                    "S'accroupir : gâchette gauche.\n"
                    "Décoché : ce grip fait aussi s'accroupir, comme la gâchette gauche."));
        }
    }
    Check(Tr("Both hands point at menus", "Les deux mains pointent les menus"), &s.offHandPointer);
    Help(Tr("On: both hands work the menus (settings, inventory, game menus, pause,\n"
            "mod menus). The hand that clicked last (R2 or L2 trigger) keeps the\n"
            "pointer, even if the other hand aims at the menu: the other one only\n"
            "takes it when you press its trigger while aiming at the menu (that click\n"
            "already counts, where it aims). On the menu screen, a small gray square\n"
            "shows where the other hand is aiming.\n"
            "With the left hand laser, its grip also moves our panels\n"
            "(inventory, settings, keys) when it aims at them.\n"
            "Off: only the hand holding the tool.",
            "Coché : les deux mains pilotent les menus (réglages, inventaire, menus du\n"
            "jeu, pause, menus des mods). La main qui a cliqué en dernier (gâchette R2\n"
            "ou L2) garde le pointeur, même si l'autre main vise le menu : l'autre ne le\n"
            "prend que quand tu appuies sur sa gâchette en visant le menu (ce clic compte\n"
            "déjà, là où elle vise). Sur l'écran des menus, un petit carré gris montre\n"
            "où vise l'autre main.\n"
            "Avec le laser de la main gauche, son grip déplace aussi nos panneaux\n"
            "(inventaire, réglages, touches) quand elle les vise.\n"
            "Décoché : seulement la main qui tient l'outil."));
    Check(Tr("Left hand laser", "Laser de la main gauche"), &s.offLaser);
    Help(Tr("A laser also comes out of your left hand (the right one in left-handed\n"
            "mode), like the one of the hand holding the tool. On a menu (settings,\n"
            "inventory, a mod's menu), it stops on it: aim at a name and press the\n"
            "left trigger to pick it (with \"Both hands point at menus\").\n"
            "With the physical hand and \"grab with the grip\", like the right hand:\n"
            "a movable object (up to 10 tons, like the game), within 3 m, gets a\n"
            "white outline, the laser lights up and the game's hand icon appears on it.\n"
            "Squeeze the left grip: you hold it at a distance, at the end of the laser\n"
            "(a heavy object follows more slowly). Release the grip to drop or throw it.\n"
            "Off: no laser, and the grip only grabs what is under the hand.",
            "Un laser part aussi de ta main gauche (la droite en mode gaucher), comme celui\n"
            "de la main qui tient l'outil. Sur un menu (réglages, inventaire, menu d'un\n"
            "mod), il s'arrête dessus : vise un nom et appuie sur la gâchette gauche pour\n"
            "le choisir (avec « Les deux mains pointent les menus »).\n"
            "Avec la main physique et « attraper avec le grip », comme la main droite :\n"
            "un objet mobile (jusqu'à 10 tonnes, comme le jeu), à moins de 3 m, est entouré\n"
            "de blanc, le laser s'éclaire et l'icône de main du jeu apparaît dessus.\n"
            "Serre le grip gauche : tu le tiens à distance, au bout du laser (un objet\n"
            "lourd suit plus lentement). Relâche le grip pour le lâcher ou le lancer.\n"
            "Décoché : pas de laser, et le grip n'attrape que ce qui est sous la main."));
    Check(Tr("Grabbed object: keeps its distance", "Objet attrapé : reste à sa distance"), &s.grabKeepDistance);
    Help(Tr("On: an object grabbed with a grip (right or left hand) stays at the\n"
            "distance where you picked it up, even very close to you (right hand: at\n"
            "least 50 cm from your eyes; left hand: 15 cm from the hand). While you\n"
            "hold it, right stick (left in left-handed mode) up: it moves away (3 m\n"
            "at most); down: it comes closer. Turning (left / right) still works;\n"
            "both hands holding an object: the one grabbed last moves.\n"
            "Off: like the game on PC (pushed out to at least 1.5 m, stick in steps).",
            "Coché : un objet attrapé avec un grip (main droite ou gauche) reste à la\n"
            "distance où tu l'as pris, même tout près de toi (main droite : 50 cm de tes\n"
            "yeux au moins ; main gauche : 15 cm de la main). Pendant que tu le tiens,\n"
            "joystick droit (gauche en mode gaucher) vers le haut : il s'éloigne (3 m au\n"
            "plus) ; vers le bas : il se rapproche. Tourner (gauche / droite) marche\n"
            "toujours ; les deux mains tiennent un objet : celui pris en dernier bouge.\n"
            "Décoché : comme le jeu sur PC (repoussé à 1,5 m au moins, joystick par crans)."));
    Section(Tr("Inventory (weapons and tools)", "Inventaire (armes et outils)"));
    ImGui::TextWrapped(Tr("Click the %s stick: shows or hides the game's inventory, a window floating in front "
                          "of your belt. Turning your head doesn't move it; it follows you when you walk. "
                          "Look at it and aim at a name + trigger to take the tool (a group number: "
                          "the group's next tool). Long list: hold the trigger and move your hand up or "
                          "down to scroll it. To move it: aim at it and squeeze that hand's grip "
                          "(the one that grabs objects), move your hand, let go: it stays there, even the "
                          "next time. Left far from your gaze (behind you): a click brings it back to its "
                          "place around you (straight ahead, or on the side where you put it).",
                          "Clic du joystick %s : affiche ou cache l'inventaire du jeu, une fenêtre qui flotte "
                          "devant ta ceinture. Tourner la tête ne le bouge pas ; il te suit quand tu marches. "
                          "Regarde-le et vise un nom + gâchette pour prendre l'outil (un numéro de groupe : "
                          "l'outil suivant du groupe). Longue liste : gâchette maintenue, monte ou descends la "
                          "main pour la faire défiler. Pour le déplacer : vise-le et serre le grip de cette main "
                          "(celui qui attrape les objets), bouge la main, relâche : il reste là, même la "
                          "prochaine fois. Resté loin de ton regard (derrière toi) : un clic le ramène à sa "
                          "place autour de toi (droit devant, ou du côté où tu l'as posé)."),
                       s.leftHanded ? Tr("left", "gauche") : Tr("right", "droit"));
    Slider(Tr("Height below my eyes", "Hauteur sous les yeux"), &s.inventoryDown, -0.40f, 0.90f, "%.2f m");
    Help(Tr("Larger = lower (toward the belt).\n"
            "Smaller = higher (toward the chest; negative = above the eyes).",
            "Plus grand = plus bas (vers la ceinture).\n"
            "Plus petit = plus haut (vers la poitrine ; négatif = au-dessus des yeux)."));
    Slider(Tr("Distance in front of me", "Distance devant moi"), &s.inventoryForward, 0.20f, 1.00f, "%.2f m");
    Slider(Tr("Side (left - / right +)", "Côté (gauche - / droite +)"), &s.inventoryYaw, -180.0f, 180.0f, "%+.0f °");
    Help(Tr("0 = straight ahead of you. Positive = to your right, negative = to your left\n"
            "(in degrees, all the way around you).\n"
            "Easiest: aim at the inventory and squeeze the grip to put it where you want.",
            "0 = droit devant toi. Positif = sur ta droite, négatif = sur ta gauche\n"
            "(en degrés, tout autour de toi).\n"
            "Le plus simple : vise l'inventaire et serre le grip pour le poser où tu veux."));
    Check(Tr("It follows me when I turn around", "Il me suit quand je me retourne"), &s.inventoryFollow);
    Help(Tr("On: when you really turn around (turned more than 55° for more than\n"
            "a second), the inventory comes back in front of you. A glance to the\n"
            "side doesn't move it, nor does looking at an inventory placed to the side.\n"
            "Off: it stays where it is; if it's far from your gaze, a stick click\n"
            "brings it back to its place around you (straight ahead, or on the side\n"
            "where you put it).",
            "Coché : quand tu te retournes vraiment (tourné de plus de 55° pendant plus\n"
            "d'une seconde), l'inventaire revient devant toi. Un coup d'œil sur le côté ne\n"
            "le bouge pas, ni regarder un inventaire posé sur le côté.\n"
            "Décoché : il reste où il est ; s'il est loin de ton regard, un clic du\n"
            "joystick le ramène à sa place autour de toi (droit devant, ou du côté\n"
            "où tu l'as posé)."));
    if (ImGui::Button(Tr("Put the inventory back at my belt", "Remettre l'inventaire devant la ceinture"), ImVec2(480, 0))) {
        s.inventoryYaw = 0;
        s.inventoryDown = 0.50f;
        s.inventoryForward = 0.40f;
        Dirty();
    }
    Check(Tr("Show it briefly when I switch tools with the stick", "Le montrer un moment quand je change d'outil au joystick"),
          &s.inventoryOnSwitch);
    Help(Tr("Off (recommended): switching tools with the stick (up / down) doesn't\n"
            "show the inventory; only the stick click shows or hides it.\n"
            "On: it also shows for 2 seconds at each switch, just to see\n"
            "(the trigger still fires). A stick click meanwhile keeps it shown.",
            "Décoché (conseillé) : changer d'outil avec le joystick (haut / bas) ne fait pas\n"
            "apparaître l'inventaire ; seul le clic du joystick l'affiche ou le cache.\n"
            "Coché : il s'affiche aussi 2 secondes à chaque changement, juste pour voir\n"
            "(la gâchette tire toujours). Un clic du joystick pendant ce temps le garde affiché."));

    Section(Tr("Tools", "Outils"));
    Check(Tr("Tools collide with objects", "Les outils touchent les objets"), &s.toolCollision);
    Help(Tr("The tool in hand (sledge, weapons...) pushes the movable objects it touches,\n"
            "at your hand's speed, with an arm's strength (a heavy object resists).",
            "L'outil en main (masse, armes...) pousse les objets mobiles qu'il touche,\n"
            "à la vitesse de ta main, avec la force d'un bras (un objet lourd résiste)."));
    if (s.toolCollision) {
        Check(Tr("  Physical tool (like Bonelabs)", "  Outil physique (comme Bonelabs)"), &s.toolPhysics);
        Help(Tr("On: the tool is a real physical object held by your hand with an arm's\n"
                "strength. It goes through nothing: it stops against walls, slides along\n"
                "surfaces, pushes what moves (a heavy object resists and the tool shifts\n"
                "in your hand). If it stays stuck far from your hand, it comes back to it.",
                "Coché : l'outil est un vrai objet physique tenu par ta main avec la force\n"
                "d'un bras. Il ne traverse rien : il s'arrête contre les murs, glisse sur\n"
                "les surfaces, pousse ce qui bouge (un objet lourd résiste et l'outil\n"
                "se décale dans ta main). S'il reste coincé loin de ta main, il y revient."));
        if (!s.toolPhysics) {
            Check(Tr("  ... stop against obstacles", "  ... s'arrêter contre les obstacles"), &s.toolBlock);
            Help(Tr("On: the tool stops at the surface of obstacles. Off: it goes\n"
                    "through them (and still pushes what moves).",
                    "Coché : l'outil s'arrête à la surface des obstacles. Décoché : il les\n"
                    "traverse (et pousse toujours ce qui bouge)."));
        }
    }
    {
        float pct = s.modToolScale * 100;
        if (Slider(Tr("Mod weapon size", "Taille des armes de mods"), &pct, 30, 150, Tr("%.0f%%", "%.0f %%"))) s.modToolScale = pct / 100;
        Help(Tr("Mods draw their weapons for a PC screen, far in front of the camera:\n"
                "held in your hand in VR they looked huge. Each mod weapon is shrunk\n"
                "automatically (even if you install new mods) to the size it has\n"
                "on screen. 100% = that automatic size.",
                "Les mods dessinent leurs armes pour un écran de PC, loin devant la caméra :\n"
                "tenues à la main en VR elles paraissaient énormes. Chaque arme de mod est\n"
                "réduite automatiquement (même si tu installes de nouveaux mods) pour avoir\n"
                "la taille qu'elle a à l'écran. 100 % = cette taille automatique."));
    }
    Check(Tr("Physical sledge", "Masse physique"), &s.swingGesture);
    Help(Tr("The sledge's head really hits what it touches, in every direction, with\n"
            "the strength of the game's sledge (wood, plaster, glass... not metal or\n"
            "concrete). The faster the swing, the bigger the hole. The trigger still\n"
            "does the game's normal swing.",
            "La tête de la masse frappe vraiment ce qu'elle touche, dans tous les sens,\n"
            "avec la solidité de la masse du jeu (bois, plâtre, verre... pas le métal ni\n"
            "le béton). Plus le coup est rapide, plus le trou est grand. La gâchette\n"
            "donne toujours le coup normal du jeu."));
    Check(Tr("Sledge: a light press = a swing", "Masse : un petit appui = un coup"), &s.sledgeTap);
    Help(Tr("On: with the sledge in hand, a short, light press on the trigger does a\n"
            "full game swing, which breaks what the laser aims at up to 3 m, as in\n"
            "the base game: no need to keep the trigger held down.\n"
            "Off: you have to press firmly, as with weapons.",
            "Coché : avec la masse en main, un appui léger et bref sur la gâchette donne\n"
            "un coup entier du jeu, qui casse ce que vise le laser jusqu'à 3 m, comme\n"
            "dans le jeu de base : inutile de garder la gâchette enfoncée.\n"
            "Décoché : il faut appuyer franchement, comme pour les armes."));

    Section(Tr("Tool position in the hand", "Position de l'outil dans la main"));
    Slider(Tr("Left / right", "Gauche / droite"), &s.toolOffsetX, -0.3f, 0.3f, "%.3f m");
    Slider(Tr("Down / up", "Bas / haut"), &s.toolOffsetY, -0.3f, 0.3f, "%.3f m");
    Slider(Tr("Forward / back", "Avant / arrière"), &s.toolOffsetZ, -0.3f, 0.3f, "%.3f m");
    Slider(Tr("Tilt", "Inclinaison"), &s.toolPitch, -60, 60, "%.0f °");

    Section(Tr("Controls (right hand = dominant hand)", "Commandes (main droite = main dominante)"));
    if (ImGui::BeginTable("map", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        // (built at each call: Tr picks the language in use)
        const char* rows[][2] = {
            { Tr("Right trigger", "Gâchette droite"), Tr("Use the tool", "Utiliser l'outil") },
            { Tr("Right grip", "Grip droit"), Tr("Grab / carry an object", "Attraper / porter un objet") },
            { Tr("Right stick left/right", "Joystick droit gauche/droite"), Tr("Turn", "Tourner") },
            { Tr("Right stick up/down", "Joystick droit haut/bas"), Tr("Switch tool", "Changer d'outil") },
            { Tr("Right stick click (short)", "Clic joystick droit (court)"),
              Tr("Inventory: show / hide (far away: bring it back)", "Inventaire : afficher / cacher (loin : le ramener)") },
            { Tr("Grip while aiming at a panel", "Grip en visant un panneau"),
              Tr("Move it (inventory, settings, keys)", "Le déplacer (inventaire, réglages, touches)") },
            { "A", Tr("Jump", "Sauter") },
            { "B", Tr("Flashlight", "Lampe torche") },
            { Tr("Left stick", "Joystick gauche"), Tr("Move", "Se déplacer") },
            { "Y", Tr("Interact / get in and out of a vehicle", "Interagir / monter et descendre d'un véhicule") },
            { Tr("Vehicle: right / left trigger", "Véhicule : gâchette droite / gauche"),
              Tr("Accelerate / brake and reverse", "Accélérer / freiner et reculer") },
            { Tr("Vehicle: left stick", "Véhicule : joystick gauche"), Tr("Steer (A = handbrake)", "Tourner (A = frein à main)") },
            { Tr("Left trigger (L2)", "Gâchette gauche (L2)"), Tr("Crouch (hold)", "S'accroupir (maintenir)") },
            { Tr("Left grip", "Grip gauche"), Tr("Grab (left hand)", "Attraper (main gauche)") },
            { "X", Tr("Map (right stick: zoom and rotate)", "Carte (joystick droit : zoom et tourner)") },
            { Tr("Menu (short press)", "Menu (appui court)"), "Pause" },
            { Tr("Menu (long press)", "Menu (appui long)"), Tr("This settings panel", "Ce panneau de réglages") },
            { Tr("Left stick click (hold)", "Clic joystick gauche (maintenir)"), Tr("Recenter view", "Recentrer la vue") },
            { Tr("Left stick click (short press)", "Clic joystick gauche (appui court)"),
              Tr("Quick key (the last one used in the keys panel)", "Touche rapide (dernière du panneau)") },
            { Tr("Right stick click (held)", "Clic joystick droit (maintenu)"),
              Tr("Keys panel (keyboard, mouse, mods)", "Panneau des touches (clavier, souris, mods)") },
            { Tr("Mod camera (plane, drone...)", "Caméra d'un mod (avion, drone...)"),
              Tr("Left stick = mouse to aim", "Joystick gauche = souris pour viser") },
            { Tr("In menus", "Dans les menus"),
              Tr("Aim at the screen + trigger = click, B = back", "Viser l'écran + gâchette = clic, B = retour") },
        };
        for (auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r[0]);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(r[1]);
        }
        ImGui::EndTable();
    }
}

void TabScreen()
{
    Settings& s = Cfg();
    Section(Tr("Virtual screen (menus, loading, pause)", "Écran virtuel (menus, chargements, pause)"));
    Slider("Distance", &s.screenDistance, 1.0f, 6.0f, "%.1f m");
    Slider(Tr("Width", "Largeur"), &s.screenWidth, 1.0f, 8.0f, "%.1f m");
    Check(Tr("Always play on the virtual screen (no 3D)", "Toujours jouer sur l'écran virtuel (sans 3D)"), &s.forceScreen);
    Help(Tr("Cinema mode: the game is shown on a big screen, without immersive VR.",
            "Mode cinéma : le jeu s'affiche sur un grand écran, sans VR immersive."));
    Check(Tr("16:9 pause menu (like on the PC screen)", "Menu pause en 16:9 (comme sur l'écran du PC)"), &s.pauseWide);
    Help(Tr("When paused in a level, the virtual screen has the shape of a PC screen\n"
            "(16:9) and the pause menu is laid out as on the PC.\n"
            "Off: almost square screen (the shape of the VR image), as before.",
            "En pause dans un niveau, l'écran virtuel a la forme d'un écran de PC\n"
            "(16:9) et le menu pause y est disposé comme sur le PC.\n"
            "Décoché : écran presque carré (la forme de l'image VR), comme avant."));
    Section(Tr("Settings panel", "Panneau de réglages"));
    Slider(Tr("Panel size in VR", "Taille du panneau en VR"), &s.uiScale, 0.6f, 2.0f, "x%.2f");
    ImGui::TextWrapped(Tr("This panel (and the keys panel) floats where it opens, in front of you: turning your "
                          "head doesn't move it. To move it: aim at it and squeeze that hand's grip, "
                          "move your hand, let go. It will then open at that distance and height.",
                          "Ce panneau (et celui des touches) flotte là où il s'ouvre, devant toi : tourner la "
                          "tête ne le bouge pas. Pour le déplacer : vise-le et serre le grip de cette main, "
                          "bouge la main, relâche. Il s'ouvrira ensuite à cette distance et à cette hauteur."));
    Slider(Tr("Distance when opened", "Distance à l'ouverture"), &s.uiPanelDistance, 0.40f, 1.50f, "%.2f m");
    Slider(Tr("Height below my eyes when opened", "Hauteur sous les yeux à l'ouverture"), &s.uiPanelDown, -0.50f, 0.80f,
           "%.2f m");
    Help(Tr("Where the panel opens, straight ahead of your gaze (remembered when you\n"
            "move it with the grip). Negative = above the eyes.\n"
            "Applies the next time it opens, or with \"Put the panel in front of me\".",
            "Où le panneau s'ouvre, droit devant ton regard (retenu quand tu le\n"
            "déplaces avec le grip). Négatif = au-dessus des yeux.\n"
            "S'applique à la prochaine ouverture, ou avec « Replacer le panneau devant moi »."));
    Check(Tr("Also show the panel on the PC screen", "Afficher aussi le panneau sur l'écran du PC"), &s.showDesktopUi);
    Check(Tr("Warn if another program is using the graphics card", "Prévenir si un autre programme occupe la carte graphique"),
          &s.gpuWatch);
    Help(Tr("Another program (AI computation, another game, mining...) can take\n"
            "the graphics card and its memory: the game then drops to 15 FPS for\n"
            "no visible reason. On: a message shows for 10 s in the headset when that\n"
            "happens, then every 5 minutes while it lasts. Windows and VR\n"
            "(Virtual Desktop, Quest Link, SteamVR) don't count.",
            "Un autre programme (calcul d'intelligence artificielle, autre jeu, minage...)\n"
            "peut prendre la carte graphique et sa mémoire : le jeu tombe alors à 15 images/s\n"
            "sans raison visible. Coché : un message s'affiche 10 s dans le casque quand ça\n"
            "arrive, puis toutes les 5 minutes tant que ça dure. Windows et la VR\n"
            "(Virtual Desktop, Quest Link, SteamVR) ne comptent pas."));
    if (ImGui::Button(Tr("Put the panel in front of me", "Replacer le panneau devant moi"), ImVec2(380, 0))) VrPlaceUiPanel();
    ImGui::SameLine();
    if (ImGui::Button(Tr("Default distance and height", "Distance et hauteur d'origine"), ImVec2(380, 0))) {
        s.uiPanelDistance = 0.90f;
        s.uiPanelDown = 0.15f;
        Dirty();
        VrPlaceUiPanel();
    }
}

void TabInfo()
{
    const VrStatus& st = VrGetStatus();
    Section(Tr("Status", "État"));
    ImVec4 col = st.sessionRunning ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(1, 0.6f, 0.3f, 1);
    ImGui::TextColored(col, "%s", st.state.c_str());
    {
        // [Affichage] headsetRuntime (config.h; vr.cpp RuntimeStartup): which headset runtime this game uses, at the
        // next game start - 0: the one that has the headset when the game starts
        static const char* kRtEn[] = { "Automatic (detection)", "Virtual Desktop", "Meta Quest Link / Air Link",
                                       "SteamVR", "The one set in Windows" };
        static const char* kRtFr[] = { "Automatique (détection)", "Virtual Desktop", "Meta Quest Link / Air Link",
                                       "SteamVR", "Celui choisi dans Windows" };
        const char* const* kRt = LangFr() ? kRtFr : kRtEn;
        Settings& s = Cfg();
        const int cur = s.headsetRuntime < 0 || s.headsetRuntime > 4 ? 0 : s.headsetRuntime;
        ImGui::PushItemWidth(330);
        if (ImGui::BeginCombo(Tr("Headset used", "Casque utilisé"), kRt[cur])) {
            for (int i = 0; i < 5; ++i) {
                const bool installed = i == 0 || i == 4 || !ConfigFindXrRuntime(i).empty();
                if (!installed) ImGui::BeginDisabled();
                if (ImGui::Selectable(kRt[i], cur == i)) { s.headsetRuntime = i; Dirty(); }
                if (!installed) ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        Help(Tr("The headset (OpenXR runtime) the game starts with.\n"
                "Automatic: when the game starts, the mod checks where the headset is (Virtual\n"
                "Desktop or Meta Quest Link, cable or Air Link) and takes that one; both\n"
                "connected: the one set in Windows. A headset connected after the game starts\n"
                "is found too (then restart the game for full resolution).\n"
                "The other choices force a runtime. Applies the next time the game starts.",
                "Le casque (runtime OpenXR) avec lequel le jeu démarre.\n"
                "Automatique : au lancement du jeu, le mod regarde où est le casque (Virtual\n"
                "Desktop ou Meta Quest Link, câble ou Air Link) et prend celui-là ; les deux\n"
                "connectés : celui choisi dans Windows. Un casque connecté après le lancement\n"
                "est trouvé aussi (relance alors le jeu pour la pleine résolution).\n"
                "Les autres choix imposent un runtime. S'applique au prochain lancement du jeu."));
        ImGui::SameLine();
        ImGui::TextDisabled(Tr("current: %s", "actuel : %s"), st.runtime.empty() ? "-" : st.runtime.c_str());
        if (cur == 0)
            ImGui::TextDisabled(Tr("Detection: %s", "Détection : %s"), st.runtimeDetect.empty() ? "-" : st.runtimeDetect.c_str());
    }
    ImGui::Text(Tr("Headset: %s", "Casque : %s"), st.system.empty() ? "-" : st.system.c_str());
    ImGui::Text(Tr("Recommended resolution per eye: %d x %d", "Résolution conseillée par œil : %d x %d"), st.recommendedW,
                st.recommendedH);
    ImGui::Text(Tr("Refresh rate: %.0f Hz", "Fréquence : %.0f Hz"), st.refreshRate);
    ImGui::TextColored(g_luaConnected ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(1, 0.6f, 0.3f, 1), Tr("Lua mod: %s", "Mod Lua : %s"),
                       g_luaConnected ? Tr("connected", "connecté")
                                      : Tr("no response (enable the \"Teardown VR\" mod and start a level)",
                                           "pas de réponse (active le mod \"Teardown VR\" et lance un niveau)"));
    ImGui::Text(Tr("Mode: %s", "Mode : %s"),
                st.stereoActive ? Tr("immersive VR (3D)", "VR immersive (3D)") : Tr("virtual screen", "écran virtuel"));
    ImGui::Text(Tr("Synchronized stereo: %s", "Stéréo synchronisée : %s"),
                !EngineStereoAvailable()
                    ? Tr("unavailable (game version not recognized)", "indisponible (version du jeu non reconnue)")
                : Cfg().stereoMode == 0 ? Tr("on", "active")
                                        : Tr("off (alternating eyes)", "désactivée (yeux alternés)"));

    Section(Tr("Performance", "Performances"));
    ImGui::Text(Tr("Game: %.0f FPS    Headset: %.0f FPS", "Jeu : %.0f images/s    Casque : %.0f images/s"), st.gameFps,
                st.vrFps);
    ImGui::Text(Tr("Game image: %d x %d (format %d)", "Image du jeu : %d x %d (format %d)"), st.backbufferW, st.backbufferH,
                st.backbufferFormat);
    ImGui::Text(Tr("Rendered field of view: %.1f° x %.1f°", "Champ de vision rendu : %.1f° x %.1f°"), st.renderFovH,
                st.renderFovV);
    // Other programs on the graphics card: one line only. This tab has ~80 px left
    // above the bottom bar; a taller block puts "Réinitialiser tous les réglages"
    // exactly under "Fermer (F10)" (and the reset button wins the click).
    if (!Cfg().gpuWatch) {
        ImGui::TextDisabled("%s", Tr("Other programs on the graphics card: not monitored (Screen tab).",
                                     "Autres programmes sur la carte graphique : non surveillés (onglet Écran)."));
    } else if (!g_gpu.available) {
        ImGui::TextDisabled("%s", Tr("Other programs on the graphics card: can't be measured on this PC.",
                                     "Autres programmes sur la carte graphique : mesure indisponible sur ce PC."));
    } else if (g_gpu.hogs.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "%s",
                           Tr("No other program is using the graphics card.",
                              "Aucun autre programme n'occupe la carte graphique."));
    } else {
        const GpuWatch::Hog& h = g_gpu.hogs[0];
        ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.2f, 1.0f),
                           Tr("%.32s: %.1f GB of video memory, %.0f%% GPU load",
                              "%.32s : %.1f Go de mémoire vidéo, %.0f %% de calcul"),
                           h.name.c_str(), h.vramGB, h.gpuPct);
        if (g_gpu.hogs.size() > 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("(+ %d)", (int)g_gpu.hogs.size() - 1);
        }
        std::string all;
        for (const GpuWatch::Hog& o : g_gpu.hogs) {
            char line[128];
            snprintf(line, sizeof(line), Tr("%.32s: %.1f GB, %.0f%%\n", "%.32s : %.1f Go, %.0f %%\n"), o.name.c_str(),
                     o.vramGB, o.gpuPct);
            all += line;
        }
        all += Tr("Close these programs or pause them while you play.",
                  "Ferme ces programmes ou mets-les en pause pendant que tu joues.");
        Help(all.c_str());
    }

    Section(Tr("Keyboard shortcuts", "Raccourcis clavier"));
    ImGui::BulletText(Tr("F10: open / close this panel", "F10 : ouvrir / fermer ce panneau"));
    ImGui::BulletText(Tr("F8: recenter the view", "F8 : recentrer la vue"));

    ImGui::Spacing();
    if (ImGui::Button(Tr("Reset all settings", "Réinitialiser tous les réglages"), ImVec2(380, 0))) ConfigReset();
}

// --- Keys panel ----------------------------------------------------------------
// Every PC key for the player in VR: the keys of the active mods (found in their
// scripts, see modkeys.h), a keyboard, the mouse buttons and wheel, and the
// stick used as the mouse. A short tap presses a key briefly; held still, it
// stays down until released; dragged, the list scrolls (like a phone screen).
std::vector<int> g_recent;                // keys used here, most recent first
std::vector<ModKeys::Mod> g_modKeys;      // last scan of the active mods
unsigned g_modKeysSerial = ~0u;
struct Press { int key = -1; double t = 0; bool held = false; } g_press;
// A press dropped (SetHeld: the pointer hand's grip took the panel): until ImGui sees the button
// up, a key activated meanwhile is not this panel's press either - the activation can come one
// Draw after the grip (the pointer is read a present before Draw), then KeyButton's release
// (IsItemDeactivated, wherever the cursor is) would tap the key or latch the modifier.
bool g_dropPress = false;
bool g_heldByPointer = false;
bool g_dragging = false;                  // the pointer moved while held: scrolling, no key
ImGuiID g_dragOwner = 0;                  // the list being dragged
float g_dragScroll = 0;                   // its scroll position when the trigger was pulled
constexpr float kDragStart = 24.0f;       // pixels before a hold becomes a drag (hand tremor)
constexpr double kHoldDelay = 0.35;       // held still this long: the key stays down

void NoteUsed(int key)
{
    g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), key), g_recent.end());
    g_recent.insert(g_recent.begin(), key);
    if (g_recent.size() > 6) g_recent.resize(6);
    // Quick key (short click of the movement stick): the last keyboard key used,
    // not Esc nor a modifier.
    const char* name = PcKeys::Get(key).name;
    if (PcKeys::IsKeyboard(key) && !PcKeys::IsModifier(key) && strcmp(name, "esc") != 0 && Cfg().quickKey != name) {
        Cfg().quickKey = name;
        Dirty();
    }
}

// The text on a key's button: PcKeys gives the keycap of the language in use.
const char* KeyLabel(int key) { return PcKeys::Get(key).label; }

// id: stable for the button's lifetime (an id that moves while held loses the press).
void KeyButton(int key, const ImVec2& size, int id, const char* text = nullptr)
{
    if (key < 0) return;
    ImGui::PushID(id);
    const bool modifier = PcKeys::IsModifier(key);
    const bool lit = modifier && Input::Latched(key);
    if (lit) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.10f, 1.0f));
    ImGui::Button(text ? text : KeyLabel(key), size);
    if (lit) ImGui::PopStyleColor();
    if (ImGui::IsItemActivated() && !g_dropPress) g_press = { key, Seconds(), false };
    if (ImGui::IsItemActive() && !g_dragging && !modifier && g_press.key == key && Seconds() - g_press.t > kHoldDelay) {
        Input::HoldKey(key);
        if (!g_press.held) {
            g_press.held = true;
            NoteUsed(key);
        }
    }
    if (ImGui::IsItemDeactivated() && g_press.key == key) {
        if (!g_dragging) {
            if (modifier) {
                Input::SetLatched(key, !Input::Latched(key));
            } else if (!g_press.held) {
                Input::TapKey(key);
                NoteUsed(key);
            }
        }
        g_press.key = -1;
    }
    ImGui::PopID();
}

// A list that scrolls when dragged with the trigger held.
void BeginDragList(const char* id, float height)
{
    ImGuiID owner = ImGui::GetID(id);
    ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_Borders);
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        g_dragOwner = owner;
        g_dragScroll = ImGui::GetScrollY();
    }
    if (g_dragging && g_dragOwner == owner && io.MouseDown[0])
        ImGui::SetScrollY(std::clamp(g_dragScroll - (io.MousePos.y - io.MouseClickedPos[0].y), 0.0f, ImGui::GetScrollMaxY()));
}

// Where the key already is on the controllers ("" if nowhere).
std::string ControllerHint(const std::string& key)
{
    const bool lh = Cfg().leftHanded;
    if (key == "lmb") return Tr("trigger of the aiming hand", "gâchette de la main qui vise");
    if (key == "rmb") return Tr("grip of the aiming hand", "grip de la main qui vise");
    if (key == "e") return lh ? "B" : "Y";
    if (key == "space") return lh ? "X" : "A";
    if (key == "f") return lh ? "Y" : "B";
    if (key == "tab") return lh ? "A" : "X";
    if (key == "ctrl") return lh ? Tr("right trigger", "gâchette droite") : Tr("left trigger", "gâchette gauche");
    if (key == "esc") return Tr("Menu button", "bouton Menu");
    if (key == "wheelup" || key == "wheeldown") return Tr("turn stick up / down", "joystick de rotation haut / bas");
    return "";
}

// "previousShell", "slow_motion_key" -> "Previous shell", "Slow motion key".
std::string Pretty(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '_' || c == '.' || c == '-' || c == ' ') {
            if (!out.empty() && out.back() != ' ') out += ' ';
            continue;
        }
        if (isupper(c) && i > 0 && islower((unsigned char)s[i - 1]) && !out.empty() && out.back() != ' ') out += ' ';
        out += (char)tolower(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (!out.empty()) out[0] = (char)toupper((unsigned char)out[0]);
    return out;
}

void KeysTabMods(float height)
{
    if (ModKeys::Serial() != g_modKeysSerial) g_modKeys = ModKeys::Get(&g_modKeysSerial);
    BeginDragList("##modkeys", height);
    if (g_modKeys.empty()) {
        ImGui::TextWrapped("%s", ModKeys::Scanning()
                                     ? Tr("Looking for the active mods' keys...", "Recherche des touches des mods actifs...")
                                     : Tr("No mod keys found. Active mods are read when a level starts: start a "
                                          "game, then reopen this panel.",
                                          "Aucune touche de mod trouvée. Les mods actifs sont lus au lancement d'un "
                                          "niveau : lance une partie, puis rouvre ce panneau."));
    }
    for (size_t m = 0; m < g_modKeys.size(); ++m) {
        const ModKeys::Mod& mod = g_modKeys[m];
        Section(mod.name.c_str());
        if (mod.usesMouse)
            ImGui::TextDisabled("%s", Tr("Reads the mouse: when it takes the camera, a stick acts as the mouse (Mouse tab).",
                                         "Lit la souris : quand il prend la caméra, un joystick sert de souris (onglet Souris)."));
        // Keys that are nowhere on the controllers first.
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t e = 0; e < mod.keys.size(); ++e) {
                const ModKeys::Entry& en = mod.keys[e];
                int key = PcKeys::ByName(en.key.c_str());
                std::string hint = ControllerHint(en.key);
                if (key < 0 || (pass == 0) != hint.empty()) continue;
                KeyButton(key, ImVec2(190, 54), 40000 + (int)(m * 100 + e));
                ImGui::SameLine();
                ImGui::BeginGroup();
                std::string label = en.label.empty() ? std::string(Tr("Key used by the mod", "Touche utilisée par le mod"))
                                                     : Pretty(en.label);
                ImGui::TextUnformatted(label.c_str());
                std::string more;
                if (en.input != en.key && en.input != "mousewheel")
                    more = Tr("game action \"", "action du jeu \"") + en.input + "\"";
                if (!hint.empty())
                    more += (more.empty() ? "" : " - ") +
                            std::string(Tr("already on the controller: ", "déjà sur la manette : ")) + hint;
                if (!more.empty()) ImGui::TextDisabled("%s", more.c_str());
                ImGui::EndGroup();
            }
        }
    }
    ImGui::Spacing();
    if (!ModKeys::Scanning() && ImGui::Button(Tr("Search again", "Chercher à nouveau"), ImVec2(300, 54))) ModKeys::Refresh();
    ImGui::EndChild();
}

void KeysTabKeyboard(float height)
{
    // QWERTY layout in English, AZERTY in French (letters are sent by name: "Q" is the key the mods call "q",
    // wherever it sits on the panel). Same width for both: 11 keys at most per row.
    static const char* const kQwerty[][12] = {
        { "esc", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f9", "f11", "f12", nullptr },
        { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "backspace", nullptr },
        { "tab", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", nullptr },
        { "a", "s", "d", "f", "g", "h", "j", "k", "l", "return", nullptr },
        { "shift", "z", "x", "c", "v", "b", "n", "m", "ctrl", "alt", nullptr },
        { "space", "leftarrow", "uparrow", "downarrow", "rightarrow", nullptr },
        { "insert", "delete", "home", "end", "pgup", "pgdown", nullptr },
    };
    static const char* const kAzerty[][12] = {
        { "esc", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f9", "f11", "f12", nullptr },
        { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "backspace", nullptr },
        { "tab", "a", "z", "e", "r", "t", "y", "u", "i", "o", "p", nullptr },
        { "q", "s", "d", "f", "g", "h", "j", "k", "l", "m", "return", nullptr },
        { "shift", "w", "x", "c", "v", "b", "n", "ctrl", "alt", nullptr },
        { "space", "leftarrow", "uparrow", "downarrow", "rightarrow", nullptr },
        { "insert", "delete", "home", "end", "pgup", "pgdown", nullptr },
    };
    const auto& kRows = LangFr() ? kAzerty : kQwerty;
    BeginDragList("##keyboard", height);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 7));
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float w = (ImGui::GetContentRegionAvail().x - gap * 10) / 11;
    for (const auto& row : kRows) {
        for (int i = 0; i < 12 && row[i]; ++i) {
            if (i) ImGui::SameLine();
            int key = PcKeys::ByName(row[i]);
            // ("Backspace" is wider than a key of an 11-key row: shortened here only)
            const char* text = !LangFr() && strcmp(row[i], "backspace") == 0 ? "Bksp" : nullptr;
            KeyButton(key, ImVec2(strcmp(row[i], "space") == 0 ? w * 3 + gap * 2 : w, 62), 20000 + key, text);
        }
    }
    ImGui::PopStyleVar();
    ImGui::TextDisabled("%s", Tr("Shift, Ctrl and Alt stay held until the next key press (or until the panel closes).",
                                 "Maj, Ctrl et Alt restent enfoncées jusqu'au prochain appui (ou jusqu'à la fermeture du "
                                 "panneau)."));
    ImGui::EndChild();
}

void KeysTabMouse(float height)
{
    Settings& s = Cfg();
    BeginDragList("##mouse", height);
    Section(Tr("Buttons and wheel", "Boutons et molette"));
    int id = 30000;
    for (const char* name : { "lmb", "rmb", "mmb" }) {
        if (id > 30000) ImGui::SameLine();
        KeyButton(PcKeys::ByName(name), ImVec2(220, 62), id++);
    }
    KeyButton(PcKeys::ByName("wheelup"), ImVec2(220, 62), id++);
    ImGui::SameLine();
    KeyButton(PcKeys::ByName("wheeldown"), ImVec2(220, 62), id++);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr("(hold = several steps)", "(maintenir = plusieurs crans)"));

    Section(Tr("Stick = mouse (aim with a mod's camera)", "Joystick = souris (viser avec la caméra d'un mod)"));
    ImGui::TextWrapped(Tr("When a mod takes the camera (AC130 plane, drone, turret...), the stick moves the mouse "
                          "to aim, like on PC.",
                          "Quand un mod prend la caméra (avion AC130, drone, tourelle...), le joystick bouge la souris "
                          "pour viser, comme sur PC."));
    int mode = Input::MouseAlways() ? 1 : (s.modMouse == 1 ? 2 : 0);
    if (ImGui::RadioButton(Tr("Automatic (when a mod takes the camera)", "Automatique (quand un mod prend la caméra)"),
                           mode == 0)) {
        Input::SetMouseAlways(false);
        s.modMouse = 0;
        Dirty();
    }
    if (ImGui::RadioButton(Tr("Always (until you change it here)", "Toujours (jusqu'à ce que tu changes ici)"), mode == 1))
        Input::SetMouseAlways(true);
    if (ImGui::RadioButton(Tr("Never", "Jamais"), mode == 2)) {
        Input::SetMouseAlways(false);
        s.modMouse = 1;
        Dirty();
    }
    ImGui::Text(Tr("Stick:", "Joystick :"));
    ImGui::SameLine();
    Radio(Tr("the movement one", "celui du déplacement"), &s.modMouseStick, 0);
    ImGui::SameLine();
    Radio(Tr("the turning one", "celui de la rotation"), &s.modMouseStick, 1);
    Help(Tr("The movement one: your character no longer walks meanwhile\n"
            "(the turn stick keeps turning, and the wheel = zoom).\n"
            "In a vehicle, it's always the turning one (the other one steers).",
            "Celui du déplacement : le personnage ne marche plus pendant ce temps\n"
            "(le joystick de rotation garde la rotation et la molette = zoom).\n"
            "En véhicule, c'est toujours celui de la rotation (l'autre tient le volant)."));
    Slider(Tr("Mouse speed", "Vitesse de la souris"), &s.modMouseSpeed, 0.2f, 4.0f, "x%.2f");
    const bool cam = EngineScriptCamera();
    ImGui::TextColored(cam ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s",
                       cam ? Tr("A mod has taken the camera right now.", "Un mod a pris la caméra en ce moment.")
                           : Tr("No mod has taken the camera right now.", "Aucun mod n'a pris la caméra en ce moment."));
    ImGui::EndChild();
}

void BuildKeys()
{
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseClicked(0)) g_dragging = false;
    if (io.MouseDown[0] && std::abs(io.MousePos.y - io.MouseClickedPos[0].y) > kDragStart) g_dragging = true;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)kWidth, (float)kHeight));
    // (the "###" ids keep the window, its tab bar and the selected tab when the language changes)
    ImGui::Begin(Tr("Teardown VR  -  Keys###keys", "Teardown VR  -  Touches###keys"), nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushItemWidth(430);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Tr("Recent:", "Récentes :"));
    std::vector<int> recent = g_recent;   // (a key used now reorders the list)
    for (int key : recent) {
        ImGui::SameLine();
        float w = std::max(80.0f, ImGui::CalcTextSize(KeyLabel(key)).x + 30.0f);   // (big enough for the laser)
        KeyButton(key, ImVec2(w, 48), 10000 + key);
    }
    if (recent.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Tr("(the keys used here)", "(les touches utilisées ici)"));
    }
    int quick = PcKeys::ByName(Cfg().quickKey.c_str());
    ImGui::TextDisabled(Tr("Quick key, short click of the movement stick: %s",
                           "Touche rapide, clic court du joystick de déplacement : %s"),
                        quick >= 0 ? KeyLabel(quick) : Tr("none (use a key here)", "aucune (utilise une touche ici)"));
    ImGui::TextDisabled("%s", Tr("To move this panel: aim at it and squeeze that hand's grip.",
                                 "Pour déplacer ce panneau : vise-le et serre le grip de cette main."));
    if (ImGui::BeginTabBar("keytabs")) {
        const float listHeight = kHeight - 66.0f - ImGui::GetCursorPosY();   // (down to the bottom bar)
        if (ImGui::BeginTabItem(Tr("Mod keys###modkeys", "Touches des mods###modkeys"))) {
            KeysTabMods(listHeight);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(Tr("Keyboard###keyboard", "Clavier###keyboard"))) {
            KeysTabKeyboard(listHeight);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(Tr("Mouse###mouse", "Souris###mouse"))) { KeysTabMouse(listHeight); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
    ImGui::SetCursorPosY(kHeight - 48.0f);
    ImGui::Separator();
    if (ImGui::Button(Tr("Close (right stick click)", "Fermer (clic joystick droit)"), ImVec2(330, 0))) g_visible = false;
    ImGui::SameLine();
    if (ImGui::Button(Tr("Settings", "Réglages"), ImVec2(160, 0))) g_keysPage = false;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr("Tap = key, hold = held down, drag = scroll",
                                 "Appui = touche, maintenir = enfoncée, glisser = défiler"));
    DrawHeldFrame();
    ImGui::End();
}

void Build()
{
    if (g_keysPage) {
        BuildKeys();
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)kWidth, (float)kHeight));
    // (no scrollbar of its own: the tabs scroll inside their area, the bottom bar stays in place; the "###" ids
    // keep the window, the selected tab and its scroll position when the language changes)
    ImGui::Begin(Tr("Teardown VR  -  Settings###settings", "Teardown VR  -  Réglages###settings"), nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushItemWidth(430);
    if (ImGui::BeginTabBar("tabs")) {
        // Each tab's content scrolls in its own area that ends above the bottom bar (the Affichage tab is
        // taller than the panel: its last lines used to slide under "Fermer"); same widths as before.
        auto body = [](void (*tab)()) {
            ImGui::BeginChild("##tabbody", ImVec2(0, kHeight - 56.0f - ImGui::GetCursorPosY()), ImGuiChildFlags_None);
            ImGui::PushItemWidth(430);
            tab();
            ImGui::PopItemWidth();
            ImGui::EndChild();
        };
        if (ImGui::BeginTabItem(Tr("Display###display", "Affichage###display"))) { body(TabDisplay); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(Tr("Comfort###comfort", "Confort###comfort"))) { body(TabComfort); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(Tr("Controls###controls", "Contrôles###controls"))) { body(TabControls); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(Tr("Screen###screen", "Écran###screen"))) { body(TabScreen); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(Tr("Info###info", "Infos###info"))) { body(TabInfo); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
    ImGui::SetCursorPosY(kHeight - 48.0f);
    ImGui::Separator();
    if (ImGui::Button(Tr("Close (F10)", "Fermer (F10)"), ImVec2(200, 0))) g_visible = false;
    ImGui::SameLine();
    if (ImGui::Button(Tr("Keys", "Touches"), ImVec2(150, 0))) {
        g_keysPage = true;
        ModKeys::Refresh();
    }
    ImGui::SameLine();
    if (VrRenderSizeLate()) {
        // Started before the headset was connected (the VR size is only chosen at the game's start).
        ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.2f, 1.0f), "%s",
                           Tr("Low resolution: restart the game with the headset connected.",
                              "Basse résolution : relance le jeu avec le casque connecté."));
    } else if (!g_gpu.hogs.empty()) {
        // Another program is using the graphics card: said on every tab, in the bottom
        // bar, so no tab content moves down (several tabs already fill the panel).
        ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.2f, 1.0f),
                           Tr("%.32s is also using the graphics card: the game may lag.",
                              "%.32s occupe aussi la carte graphique : le jeu peut ramer."),
                           g_gpu.hogs[0].name.c_str());
    } else if (const char* overlay = Perf::OverlayNames()) {
        // An overlay hooked into the game's images (Steam, NVIDIA): it can make them stutter.
        ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.2f, 1.0f),
                           Tr("%s overlay active: may cause stutters.", "Overlay %s actif : peut causer des saccades."), overlay);
    } else {
        ImGui::TextDisabled("%s", Tr("Settings are saved automatically.", "Les réglages sont enregistrés automatiquement."));
    }
    DrawHeldFrame();
    ImGui::End();
}

// Short warning shown in the headset (and on the PC screen) while the panel is closed.
void BuildBanner()
{
    ImGui::SetNextWindowPos(ImVec2(0, (float)g_bannerTop));
    if (g_lowResBanner) {
        // The game was started before the headset was connected: it draws at the monitor's size.
        const VrStatus& st = VrGetStatus();
        ImGui::SetNextWindowSize(ImVec2((float)kBannerW, (float)kBannerH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 3.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.62f, 0.2f, 1.0f));
        ImGui::Begin("##lowResBanner", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.2f, 1.0f),
                           Tr("Low resolution image (%d x %d)", "Image en basse résolution (%d x %d)"), st.backbufferW,
                           st.backbufferH);
        ImGui::TextUnformatted(Tr("The headset wasn't connected when the game started.",
                                  "Le casque n'était pas connecté au lancement du jeu."));
        ImGui::TextUnformatted(Tr("Restart Teardown with the headset connected (full resolution).",
                                  "Relance Teardown avec le casque connecté (pleine résolution)."));
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return;
    }
    const GpuWatch::Hog& h = g_gpu.hogs[0];
    ImGui::SetNextWindowSize(ImVec2((float)kBannerW, (float)kBannerH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 3.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.62f, 0.2f, 1.0f));
    ImGui::Begin("##gpuBanner", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.2f, 1.0f), "%s",
                       Tr("Another program is using the graphics card:", "Un autre programme utilise la carte graphique :"));
    ImGui::Text(Tr("%.32s  (%.1f GB of video memory, %.0f%% GPU load)", "%.32s  (%.1f Go de mémoire vidéo, %.0f %% de calcul)"),
                h.name.c_str(), h.vramGB, h.gpuPct);
    ImGui::TextUnformatted(Tr("The game may lag. Close it or pause it.", "Le jeu peut ramer. Ferme-le ou mets-le en pause."));
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

} // namespace

void Init(HWND)
{
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_last);
    if (!Render::Ready()) return;
    if (!CreateTarget()) { Log("Ui: render target creation failed"); return; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.MouseDrawCursor = true;
    io.DisplaySize = ImVec2((float)kWidth, (float)kHeight);
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ApplyStyle();

    // (+ U+221E, the infinity sign: some mods show it as an ammo count in the VR inventory)
    // (+ U+2190..2193 arrows for the keys panel's arrow / wheel labels)
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0152, 0x0153, 0x2013, 0x2026, 0x2190, 0x2193, 0x221E, 0x221E, 0 };
    char fontPath[MAX_PATH];
    GetWindowsDirectoryA(fontPath, MAX_PATH);
    strcat_s(fontPath, "\\Fonts\\segoeui.ttf");
    if (GetFileAttributesA(fontPath) != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF(fontPath, 24.0f, nullptr, ranges);
    else
        io.Fonts->AddFontDefault();
    // The VR inventory's fonts: the game's own (after the default font above).
    Inventory::AddFonts(io.Fonts);

    ImGui_ImplDX12_InitInfo ii;
    ii.Device = Render::Device();
    ii.CommandQueue = Render::Queue();
    ii.NumFramesInFlight = 4;
    ii.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = Render::SrvHeap();
    ii.SrvDescriptorAllocFn = SrvAlloc;
    ii.SrvDescriptorFreeFn = SrvFree;
    if (!ImGui_ImplDX12_Init(&ii)) { Log("Ui: ImGui DX12 init failed"); return; }
    g_ready = true;
    Log("Ui: ready");
}

bool Ready() { return g_ready; }
bool Visible() { return g_ready && g_visible; }
bool BannerShown() { return g_ready && g_banner; }
int BannerTop() { return g_bannerTop; }
bool InventoryShown() { return g_ready && g_inventory; }
void SetVisible(bool v) { g_visible = v; }

void Toggle()
{
    if (g_visible && g_keysPage) {
        g_keysPage = false;
        return;
    }
    g_keysPage = false;
    g_visible = !g_visible;
}

void ToggleKeys()
{
    if (g_visible && !g_keysPage) {
        g_keysPage = true;
    } else {
        g_keysPage = true;
        g_visible = !g_visible;
    }
    if (g_visible) ModKeys::Refresh();   // (the active mods may have changed: new level)
}

bool KeysShown() { return g_ready && g_visible && g_keysPage; }

void SetDesktopMouse(float x, float y, bool down, float wheel)
{
    g_dmx = x;
    g_dmy = y;
    g_ddown = down;
    g_dwheel += wheel;
}

void SetVrPointer(bool active, float x, float y, bool down)
{
    g_vrActive = active;
    g_vx = x;
    g_vy = y;
    g_vdown = active && down;
}

void SetHeld(bool held, bool pointerHolds)
{
    const bool byPointer = held && pointerHolds;
    if (byPointer && !g_heldByPointer) {
        g_dropPress = true;   // (cleared by Draw once the button is seen up)
        g_press.key = -1;
    }
    g_heldByPointer = byPointer;
    g_held = held;
}

ID3D12Resource* Draw(ID3D12GraphicsCommandList* cl, D3D12_GPU_DESCRIPTOR_HANDLE* srv)
{
    if (!g_ready) return nullptr;

    // Lua connection watchdog + delayed config save (even while hidden).
    double now = Seconds();
    if (now - g_lastLuaCheck > 1.0) {
        int r = VrGetStatus().luaRequests;
        g_luaConnected = r != g_lastLuaRequests;
        g_lastLuaRequests = r;
        g_lastLuaCheck = now;
        GpuWatch::Get(g_gpu);   // keeps the previous copy if the watch thread is publishing right now
        if (g_gpu.hogs.empty()) {
            g_bannerUntil = 0;
        } else if (g_gpu.serial != g_gpuSerialSeen || now >= g_bannerRepeatAt) {
            // A program started using the graphics card: tell the player in the
            // headset for 10 s, and again every 5 minutes while it lasts.
            g_gpuSerialSeen = g_gpu.serial;
            g_bannerUntil = now + 10.0;
            g_bannerRepeatAt = now + 300.0;
        }
        // Started before the headset was connected: said in the headset for 15 s when the session
        // starts, and again every 10 minutes (only a restart of the game gives the VR resolution).
        if (!VrRenderSizeLate()) {
            g_lowResUntil = g_lowResRepeatAt = 0;
        } else if (now >= g_lowResRepeatAt) {
            g_lowResUntil = now + 15.0;
            g_lowResRepeatAt = now + 600.0;
            Log("Ui: banner - low resolution, the game was started before the headset was connected");
        }
        // (one banner at a time: the graphics card one waits until the low resolution one is over)
        if (now < g_lowResUntil && g_bannerUntil > now) g_bannerUntil = std::max(g_bannerUntil, g_lowResUntil + 10.0);
    }
    if (g_dirty && now - g_dirtyTime > 0.75) {
        ConfigSave();
        g_dirty = false;
    }
    // The VR inventory uses the same texture (top-left corner) while the settings are closed;
    // the warning banner then goes below it (the inventory may stay shown for a long time).
    g_inventory = !g_visible && Inventory::Visible();
    g_lowResBanner = now < g_lowResUntil && VrRenderSizeLate();
    g_banner = !g_visible && (g_lowResBanner || (now < g_bannerUntil && !g_gpu.hogs.empty()));
    g_bannerTop = g_inventory ? kBannerY : 0;
    if (!g_visible && !g_banner && !g_inventory) return nullptr;

    ImGuiIO& io = ImGui::GetIO();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    io.DeltaTime = std::max(1e-4f, float(t.QuadPart - g_last.QuadPart) / float(g_freq.QuadPart));
    g_last = t;
    io.MouseDrawCursor = !g_banner && !g_inventory;   // (the inventory draws its own pointer)
    if (g_banner || g_inventory) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(0, false);
    } else if (g_vrActive) {
        io.AddMousePosEvent(g_vx, g_vy);
        io.AddMouseButtonEvent(0, g_vdown);
    } else {
        io.AddMousePosEvent(g_dmx, g_dmy);
        io.AddMouseButtonEvent(0, g_ddown);
        if (g_dwheel != 0) io.AddMouseWheelEvent(0, g_dwheel);
    }
    g_dwheel = 0;

    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    if (g_inventory) Inventory::Build(io.DeltaTime);
    if (g_banner) BuildBanner();
    if (!g_inventory && !g_banner) Build();
    if (g_dropPress && !io.MouseDown[0]) g_dropPress = false;   // (a new press may begin: see g_dropPress)
    ImGui::Render();

    Render::Barrier(cl, g_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float clear[4] = { 0, 0, 0, 0 };
    cl->ClearRenderTargetView(g_rtv, clear, 0, nullptr);
    cl->OMSetRenderTargets(1, &g_rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { Render::SrvHeap() };
    cl->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
    Render::Barrier(cl, g_tex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    *srv = g_srvGpu;
    return g_tex;
}

} // namespace Ui

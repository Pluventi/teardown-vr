#pragma once
#include <windows.h>
#include <d3d12.h>

// The "Teardown VR" settings application (Dear ImGui), drawn into an offscreen
// texture that is shown both on the desktop window and as a floating panel in
// the headset.
namespace Ui {

void Init(HWND hwnd);
bool Ready();
bool Visible();
void SetVisible(bool v);
// Settings panel (F10, long press on Menu). From the keys panel: back to the settings.
void Toggle();
// Keys panel in the same window (rotation stick click): PC keyboard, mouse
// buttons and wheel, keys of the active mods, stick used as the mouse.
void ToggleKeys();
bool KeysShown();   // visible and showing the keys panel

constexpr int kWidth = 1100;
constexpr int kHeight = 860;

// Renders the UI if visible (or the warning banner, see below). Returns the
// texture (PIXEL_SHADER_RESOURCE state) or nullptr when nothing is shown.
ID3D12Resource* Draw(ID3D12GraphicsCommandList* cl, D3D12_GPU_DESCRIPTOR_HANDLE* srv);

// Warning banner (another program is using the graphics card, see gpu_watch.h):
// shown for 10 s while the panel is closed, drawn in kBannerW x kBannerH pixels
// of the same texture, from row BannerTop(): 0, or kBannerY when the VR inventory
// is drawn in the same texture (top-left) at the same time.
constexpr int kBannerW = 700;
constexpr int kBannerH = 128;
constexpr int kBannerY = kHeight - kBannerH;
// True when the last Draw() produced the banner instead of the panel.
bool BannerShown();
int BannerTop();
// True when the last Draw() produced the VR inventory (top-left
// Inventory::PanelW() x PanelH() pixels, see inventory.h) instead of the panel
// (the banner may be drawn too, see BannerTop()).
bool InventoryShown();

// Desktop mouse, in UI pixels.
void SetDesktopMouse(float x, float y, bool down, float wheel);
// VR laser pointer, in UI pixels. active=false when the ray misses the panel.
void SetVrPointer(bool active, float x, float y, bool down);
// The panel (settings or keys) is being moved with a grip (vr.cpp, UpdatePanelGrips): an
// orange frame around it. pointerHolds: by the hand that points at it (Input::PointerHand) -
// a key press its trigger began just before the grip (a whole-fist squeeze; the grip masks
// that trigger) is dropped: no key tapped, held or latched when ImGui sees it let go.
void SetHeld(bool held, bool pointerHolds);

} // namespace Ui

// VR inventory: the game's tools and weapons (mods included) drawn like the
// game's own tool bar, on a panel attached to the player's body. See inventory.h.
#include "inventory.h"
#include "bridge.h"
#include "input.h"
#include "config.h"
#include "i18n.h"
#include "log.h"
#include "ui.h"
#include "vr.h"

#include <imgui.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace Inventory {
namespace {

constexpr int kMaxTools = 400;
constexpr int kMaxChunks = 9;

// --- Look: the game's tool bar (data/ui/hud.lua, drawTool) ------------------------
// The game lays it out in a 1920x1080 UI. Here kU texture pixels stand for one
// game pixel, and the text and boxes are 1.3 times larger (kF), like the game's
// "large UI" option: the column width stays, the letters grow (read at arm's
// length in the headset).
constexpr float kU = 0.70f, kF = kU * 1.3f;
constexpr float kColW = 230 * kU, kColGap = 20 * kU;   // toolItemWidth, toolGroupSpacing
constexpr float kWrapW = (200 - 13) * kU;               // UiWordWrap(windowToolWidth - 10), large UI
constexpr float kSelMinW = 200 * kU;                    // dark box behind the tool in hand (windowToolWidth)
constexpr float kRowPad = 8 * kF;                       // a name takes its text height + 8
constexpr float kSelRound = 8 * kF;
constexpr float kBox = 28 * kF, kBoxRound = 6 * kF;     // group number box (box-solid-6.png)
constexpr float kBarGap = 2 * kF, kBarH = 4 * kF;       // yellow bar under the group of the tool in hand
constexpr float kListGap = 6 * kF;                      // names -> number boxes
constexpr float kInfoGap = 12 * kF;                     // number boxes -> ammo / tool in hand
constexpr float kPadX = 22, kPadTop = 14, kPadBottom = 12;
constexpr float kMinW = 420;
constexpr int kVisRows = 5;                             // names shown per group, the rest scrolls (like the game)
// Font sizes: Ui:MediumFont(16) for the names, Ui:BoldFont(16) for the group
// numbers, Ui:BoldFont(20) for the tool in hand, Ui:BoldFont(30) for its ammo
// (x 1.23 = Ui.fontScale, x kF), rounded to whole pixels: the fonts are baked at
// these sizes (sharp letters).
constexpr float kNameSize = 18, kNumSize = 18, kToolSize = 22, kAmmoSize = 34;
constexpr float kDragStart = 24;     // pixels of movement before a press becomes a drag (pulling the
                                     // trigger tilts the controller about 1 degree: ~15 px on the panel)
constexpr float kBriefTime = 2.0f;   // seconds shown after a tool change with the stick (option)
constexpr float kFlashTime = 0.3f;   // the tool in hand grows 1.4x then eases back, like the game

ImFont* g_fontName = nullptr;   // medium.ttf (nullptr: the UI's default font)
ImFont* g_fontNum = nullptr;    // bold.ttf
ImFont* g_fontTool = nullptr;
ImFont* g_fontAmmo = nullptr;

// A tool as shown.
struct Entry
{
    int group = 6;
    bool selectable = true;
    std::string ammo;   // UTF-8, "" = nothing shown
    std::string name;   // UTF-8
};

// A tool of the list being received.
struct Staged
{
    bool header = false;
    int group = 6;
    bool selectable = true;
    std::string ammo;
    int chunks = 0;
    unsigned got = 0;   // bit k-1 set: name chunk k received
    std::string part[kMaxChunks];
};

// --- Shared with the game's script thread (Lua messages), under g_mutex ----------
std::mutex g_mutex;
int g_stageVer = -1;
std::vector<Staged> g_stage;
int g_pubVer = 0;                 // list version complete on our side (0 = none)
std::vector<Entry> g_pub;
unsigned g_serial = 1;            // changes whenever g_pub changes
int g_current = 0, g_currentVer = -1;   // tool in hand: 1-based index in list g_currentVer
bool g_locked = false;            // switching tools is not possible right now
int g_selSeq = 0, g_selVer = 0, g_selIdx = 0;   // latest pick, read by Lua
std::atomic<int> g_luaFlags{ 0 };
std::atomic<bool> g_open{ false };

int Hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// "~XX" -> byte. A malformed escape (a lone "~" = empty text) is dropped, and so
// are control characters.
std::string Unescape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '~') {
            out += s[i];
            continue;
        }
        int hi = i + 1 < s.size() ? Hex(s[i + 1]) : -1;
        int lo = i + 2 < s.size() ? Hex(s[i + 2]) : -1;
        if (hi < 0 || lo < 0) continue;
        int v = hi * 16 + lo;
        if (v >= 32) out += (char)v;
        i += 2;
    }
    return out;
}

// "TDVRIx <n integers> <rest>": the integers after the 6-character tag, separated
// by single spaces; rest = everything after the next space, verbatim (may hold
// spaces, may be empty).
bool Fields(const std::string& s, int n, int* out, std::string* rest)
{
    size_t p = 6;
    for (int i = 0; i < n; ++i) {
        if (p >= s.size() || s[p] != ' ') return false;
        size_t b = ++p;
        while (p < s.size() && s[p] != ' ') ++p;
        if (p == b || p - b > 9) return false;
        char tok[12];
        s.copy(tok, p - b, b);
        tok[p - b] = 0;
        char* end = nullptr;
        out[i] = (int)strtol(tok, &end, 10);
        if (*end) return false;
    }
    if (rest) *rest = p + 1 < s.size() ? s.substr(p + 1) : std::string();
    return true;
}

// The pass over the staged list is complete: it becomes the list shown (g_mutex held).
void PublishIfComplete()
{
    for (const Staged& t : g_stage)
        if (!t.header || t.got != (1u << t.chunks) - 1) return;
    std::vector<Entry> list(g_stage.size());
    for (size_t i = 0; i < g_stage.size(); ++i) {
        const Staged& t = g_stage[i];
        std::string esc;
        for (int k = 0; k < t.chunks; ++k) esc += t.part[k];
        list[i].group = t.group;
        list[i].selectable = t.selectable;
        list[i].ammo = t.ammo;
        list[i].name = Unescape(esc);
        if (list[i].name.empty()) list[i].name = "?";
    }
    g_pub.swap(list);
    g_pubVer = g_stageVer;
    ++g_serial;
    static int logged = 0;
    if (logged++ < 20) {
        std::string names;
        for (const Entry& e : g_pub) {
            if (names.size() > 300) { names += " ..."; break; }
            names += (names.empty() ? "" : ", ") + std::to_string(e.group) + ":" + e.name + (e.ammo.empty() ? "" : " [" + e.ammo + "]");
        }
        Log("Inventory: tool list %d received (%zu tools): %s", g_pubVer, g_pub.size(), names.c_str());
    }
}

// --- Present thread -------------------------------------------------------------
// Pinned: shown with the stick click (R3) until the next one; the trigger picks
// and scrolls. Brief (option, off by default): shown for a moment after a tool
// change with the stick, display only (the trigger keeps firing).
enum class Show { Hidden, Brief, Pinned };
Show g_show = Show::Hidden;
bool g_want = false;         // R3 asked for the panel (until the next R3)
bool g_allowed = false;      // on foot, no menu / map / settings (Update)
float g_briefLeft = 0;       // Brief: seconds before the panel hides again
bool g_placePending = false;
bool g_logLayout = false;    // log the layout of the next frame (tests)
bool g_justShown = false;    // first frame after the panel appeared (no selection flash)

// Dominant hand ray and trigger.
bool g_ptrHit = false, g_ptrOn = false;
float g_ptrX = 0, g_ptrY = 0;
bool g_trigDown = false;
bool g_pressGame = false;    // the trigger press in progress is the game's (it began beside the panel or
                             // before it appeared): the tool keeps firing, even across the panel
bool g_pressOwned = false;   // a press that began on the panel (until it is released)
bool g_swallow = false;      // a press that the game must not see (it began on the panel, then it hid)
bool g_pressEvent = false, g_releaseEvent = false;   // for Build()
float g_pressX = 0, g_pressY = 0;
int g_heldBy = -1;           // the hand moving the panel with its grip (SetHeld), -1 = none

// Copy of the shared list, for drawing.
std::vector<Entry> g_view;
int g_viewVer = 0;
unsigned g_viewSerial = 0;
int g_viewCurrent = 0;       // 1-based, 0 = none
bool g_viewLocked = false;
bool g_followCurrent = false;   // scroll the tool in hand into view

// A line of a wrapped name: [a, b) in g_view[...].name.
struct Line { const char* a; const char* b; };

struct Column
{
    int group = 0;
    std::vector<int> tools;      // 0-based indices in g_view
    std::vector<float> top, h;   // per row: offset from the top of the content, height
    std::vector<int> line0, lines;   // per row: first line in g_lines, count
    float content = 0;           // height of all the rows
    float x0 = 0, x1 = 0;        // the column (names centred on it)
    float hx0 = 0, hx1 = 0;      // what the pointer can hit (the gaps split in two)
};
std::vector<Column> g_cols;
std::vector<Line> g_lines;
unsigned g_wrapSerial = 0;        // g_viewSerial the names were wrapped for
struct Scroll { float pos = 0, vel = 0, target = -1; };
std::map<int, Scroll> g_scroll;   // per group (kept when the list changes)
int g_w = (int)kMinW, g_h = 300;  // panel size (pixels) of the last frame
float g_listTop = kPadTop, g_listH = 100;
float g_boxTop = 0, g_infoTop = 0;

struct Sample { float t, pos; };
struct Drag
{
    bool active = false, moved = false;
    int col = -1;
    float x0 = 0, y0 = 0, scroll0 = 0;
    Sample hist[16] = {};
    int n = 0;
};
Drag g_drag;
float g_time = 0;
int g_hoverCol = -1, g_hoverRow = -1;   // row -2 = the group number box
float g_lastTick = -1;
float g_flashT = 0;          // selection flash (seconds left)

void Haptic(float amplitude, float seconds)
{
    if (Cfg().haptics) VrHaptic(Input::PointerHand(), amplitude, seconds);   // (the hand pointing at the panel)
}

ImFont* FontOr(ImFont* f) { return f ? f : ImGui::GetFont(); }

float TextW(ImFont* f, float size, const char* a, const char* b) { return f->CalcTextSizeA(size, FLT_MAX, 0, a, b).x; }

// Word wrap like the game: lines of at most `wrap` pixels, cut at spaces and after
// a '-' ("Lance-" / "roquettes"); a word longer than a line is cut between letters.
void WrapLines(ImFont* f, float size, float wrap, const char* s, const char* end, std::vector<Line>& out)
{
    while (s < end) {
        while (s < end && *s == ' ') ++s;   // (no blank at the start of a line)
        if (s >= end) break;
        const char* fit = nullptr;          // end of the longest line of whole words
        for (const char* p = s; p < end; ++p) {
            const char* stop = *p == ' ' ? p : *p == '-' ? p + 1 : nullptr;
            if (p + 1 == end) stop = end;
            if (!stop || stop == s) continue;
            if (TextW(f, size, s, stop) > wrap) break;
            fit = stop;
        }
        if (!fit) {   // one word longer than the line
            fit = s;
            for (const char* p = s; p < end;) {
                const char* n = p + 1;
                while (n < end && ((unsigned char)*n & 0xC0) == 0x80) ++n;   // (whole UTF-8 letters)
                if (fit != s && TextW(f, size, s, n) > wrap) break;
                fit = p = n;
            }
        }
        out.push_back({ s, fit });
        s = fit;
    }
}

// Upper case like the game's tool name at the bottom of the screen: ASCII, the
// accented Latin-1 letters and "œ".
std::string Upper(const std::string& s)
{
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned char c = (unsigned char)out[i];
        if (c >= 'a' && c <= 'z') {
            out[i] = (char)(c - 32);
        } else if (c == 0xC3 && i + 1 < out.size()) {
            unsigned char d = (unsigned char)out[i + 1];
            if (d >= 0xA0 && d <= 0xBE && d != 0xB7) out[i + 1] = (char)(d - 0x20);   // à..þ (not ÷)
            ++i;
        } else if (c == 0xC5 && i + 1 < out.size() && (unsigned char)out[i + 1] == 0x93) {
            out[i + 1] = (char)0x92;   // œ
            ++i;
        }
    }
    return out;
}

void Hide()
{
    // A trigger press that belongs to the panel stays away from the game until released.
    g_swallow = g_swallow || g_pressOwned;
    g_show = Show::Hidden;
    g_open = false;
    g_pressOwned = g_pressEvent = g_releaseEvent = false;
    g_ptrOn = g_ptrHit = false;
    g_drag = Drag{};
}

// Shows / hides the panel after a change of g_want, g_allowed or g_briefLeft.
void Apply()
{
    const Show next = !g_allowed ? Show::Hidden : g_want ? Show::Pinned : g_briefLeft > 0 ? Show::Brief : Show::Hidden;
    if (next == g_show) return;
    if (g_show == Show::Pinned || next == Show::Hidden) Hide();
    if (next == Show::Hidden) return;
    if (g_show == Show::Hidden) {   // appears
        g_placePending = true;
        g_logLayout = true;
        g_justShown = true;
        g_pressOwned = g_pressEvent = g_releaseEvent = false;
        g_drag = Drag{};
        g_followCurrent = true;
    }
    g_pressGame = g_trigDown;   // (a press from before is the game's)
    g_show = next;
    g_open = true;
}

float MaxScroll(const Column& c) { return std::max(0.0f, c.content - g_listH); }
// Content offset in the list area: short lists sit on the number boxes, like the game.
float Base(const Column& c) { return std::max(0.0f, g_listH - c.content); }

bool Dragging(int col) { return g_drag.active && g_drag.moved && g_drag.col == col; }

// Column under a point of the lists' area, or -1.
int HitColumn(float x, float y)
{
    if (y < g_listTop || y >= g_listTop + g_listH) return -1;
    for (int i = 0; i < (int)g_cols.size(); ++i)
        if (x >= g_cols[i].hx0 && x < g_cols[i].hx1) return i;
    return -1;
}

// Group number box under a point (with the yellow bar and a small margin), or -1.
int HitBox(float x, float y)
{
    if (y < g_boxTop - 6 || y >= g_boxTop + kBox + kBarGap + kBarH + 6) return -1;
    for (int i = 0; i < (int)g_cols.size(); ++i)
        if (x >= g_cols[i].hx0 && x < g_cols[i].hx1) return i;
    return -1;
}

// Row (index in the column) under a point of column c, or -1.
int HitRow(int c, float y)
{
    const Column& col = g_cols[c];
    float local = y - g_listTop - Base(col) + g_scroll[col.group].pos;
    for (int r = 0; r < (int)col.tools.size(); ++r)
        if (local >= col.top[r] && local < col.top[r] + col.h[r]) return r;
    return -1;
}

void Pick(int idx)   // 1-based
{
    bool ok;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ok = g_pubVer == g_viewVer && idx >= 1 && idx <= (int)g_pub.size() && g_pub[idx - 1].selectable && !g_locked;
        if (ok) {
            g_selSeq = g_selSeq % 9999 + 1;
            g_selVer = g_pubVer;
            g_selIdx = idx;
            g_current = idx;          // shown at once; Lua confirms (or corrects) it
            g_currentVer = g_pubVer;
        }
    }
    static int logged = 0;
    if (logged++ < 20) Log("Inventory: tool %d picked (%s)", idx, ok ? "taken" : "not available");
    if (ok) {
        if (idx != g_viewCurrent) g_flashT = kFlashTime;
        g_viewCurrent = idx;
        Haptic(0.35f, 0.03f);
    } else {
        Haptic(0.6f, 0.08f);
    }
}

// Group number pressed: the next tool of the group, like its key on the PC.
void PickInGroup(int c)
{
    const Column& col = g_cols[c];
    const int n = (int)col.tools.size();
    int at = -1;
    for (int r = 0; r < n; ++r)
        if (col.tools[r] + 1 == g_viewCurrent) at = r;
    for (int k = 1; k <= n; ++k) {
        int r = (at + k) % n;
        if (g_view[col.tools[r]].selectable) {
            Pick(col.tools[r] + 1);
            return;
        }
    }
    Haptic(0.6f, 0.08f);
}

void Refresh()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_viewSerial != g_serial) {
        g_viewSerial = g_serial;
        // (a new list brings the tool in hand into view; an ammo update does not: it
        // would scroll back a list the player is looking through)
        if (g_viewVer != g_pubVer) g_followCurrent = true;
        g_view = g_pub;
        g_viewVer = g_pubVer;
        g_cols.clear();
        for (int i = 0; i < (int)g_view.size(); ++i) {
            int g = g_view[i].group;
            auto it = std::find_if(g_cols.begin(), g_cols.end(), [g](const Column& c) { return c.group == g; });
            if (it == g_cols.end()) {
                g_cols.emplace_back();
                g_cols.back().group = g;
                it = g_cols.end() - 1;
            }
            it->tools.push_back(i);
        }
        std::stable_sort(g_cols.begin(), g_cols.end(), [](const Column& a, const Column& b) { return a.group < b.group; });
        if (g_drag.col >= (int)g_cols.size()) g_drag = Drag{};
    }
    int cur = g_currentVer == g_pubVer && g_pubVer == g_viewVer ? g_current : 0;
    if (cur != g_viewCurrent) {
        if (!g_justShown && cur > 0) g_flashT = kFlashTime;   // (changed with the stick, a key...)
        g_viewCurrent = cur;
        g_followCurrent = true;
    }
    g_viewLocked = g_locked;
}

void Layout()
{
    ImFont* fn = FontOr(g_fontName);
    const int n = std::max(1, (int)g_cols.size());
    const float colW = std::min(kColW, (Ui::kWidth - 2 * kPadX - (n - 1) * kColGap) / n);
    const float w = std::max(kMinW, 2 * kPadX + n * colW + (n - 1) * kColGap);
    const float wrap = std::min(kWrapW, colW - 10);
    float x = (w - (n * colW + (n - 1) * kColGap)) * 0.5f;
    const float row1 = kNameSize + kRowPad;   // a one-line name
    float listH = kVisRows * row1;
    // (names wrapped again only when the list changes: g_lines points into g_view)
    const bool wrapNow = g_wrapSerial != g_viewSerial;
    g_wrapSerial = g_viewSerial;
    if (wrapNow) g_lines.clear();
    for (Column& c : g_cols) {
        c.x0 = x;
        c.x1 = x + colW;
        c.hx0 = x - kColGap * 0.5f;
        c.hx1 = x + colW + kColGap * 0.5f;
        x += colW + kColGap;
        const size_t rows = c.tools.size();
        if (wrapNow || c.top.size() != rows) {
            c.top.resize(rows);
            c.h.resize(rows);
            c.line0.resize(rows);
            c.lines.resize(rows);
            float y = 0;
            for (size_t r = 0; r < rows; ++r) {
                const std::string& name = g_view[c.tools[r]].name;
                c.line0[r] = (int)g_lines.size();
                WrapLines(fn, kNameSize, wrap, name.c_str(), name.c_str() + name.size(), g_lines);
                c.lines[r] = (int)g_lines.size() - c.line0[r];
                c.top[r] = y;
                c.h[r] = std::max(1, c.lines[r]) * kNameSize + kRowPad;
                y += c.h[r];
            }
            c.content = y;
        }
        // A short group shows all its names (two-line names make it taller); a long
        // one shows about kVisRows and scrolls.
        if ((int)rows <= kVisRows) listH = std::max(listH, c.content);
    }
    g_listH = std::min(listH, 8 * row1);
    g_listTop = kPadTop;
    g_boxTop = g_listTop + g_listH + kListGap;
    g_infoTop = g_boxTop + kBox + kBarGap + kBarH + kInfoGap;
    g_w = std::min(Ui::kWidth, (int)std::ceil(w));
    g_h = std::min(Ui::kHeight, (int)std::ceil(g_infoTop + kAmmoSize + 2 + kToolSize + kPadBottom));
    if (g_logLayout) {
        g_logLayout = false;
        static int logged = 0;
        if (logged++ < 10) {
            std::string cols;
            for (const Column& c : g_cols) {
                char b[64];
                snprintf(b, sizeof(b), " %d:[%.0f-%.0f]x%zu/%.0f", c.group, c.x0, c.x1, c.tools.size(), c.content);
                cols += b;
            }
            Log("Inventory: panel %dx%d px, lists y=%.0f..%.0f, boxes y=%.0f..%.0f, game fonts %s, columns%s", g_w, g_h,
                g_listTop, g_listTop + g_listH, g_boxTop, g_boxTop + kBox, g_fontName ? "yes" : "NO (default font)",
                cols.c_str());
        }
    }
}

void FollowCurrent()
{
    if (!g_followCurrent || g_viewCurrent <= 0) return;
    g_followCurrent = false;
    for (int ci = 0; ci < (int)g_cols.size(); ++ci) {
        const Column& c = g_cols[ci];
        auto it = std::find(c.tools.begin(), c.tools.end(), g_viewCurrent - 1);
        if (it == c.tools.end()) continue;
        if (Dragging(ci)) return;
        Scroll& s = g_scroll[c.group];
        const int r = (int)(it - c.tools.begin());
        float at = s.target >= 0 ? s.target : s.pos;
        if (c.top[r] < at || c.top[r] + c.h[r] > at + g_listH) {
            s.target = std::clamp(c.top[r] + c.h[r] * 0.5f - g_listH * 0.5f, 0.0f, MaxScroll(c));   // (centred, like the game)
            s.vel = 0;
        }
        return;
    }
}

// Release speed of a drag: over its last 0.12 s (each headset frame is two
// presents with the same hand pose, so single-frame speeds are useless).
float ReleaseSpeed()
{
    int n = std::min(g_drag.n, 16);
    if (n < 2) return 0;
    const Sample& last = g_drag.hist[(g_drag.n - 1) % 16];
    for (int j = 0; j < n - 1; ++j) {   // oldest first
        const Sample& old = g_drag.hist[(g_drag.n - n + j) % 16];
        float dt = last.t - old.t;
        if (dt <= 0.12f) return dt > 0.02f ? std::clamp((last.pos - old.pos) / dt, -4000.0f, 4000.0f) : 0.0f;
    }
    return 0;
}

void HandlePointer()
{
    if (g_pressEvent) {
        g_pressEvent = false;
        g_drag = Drag{};
        g_drag.active = true;
        g_drag.x0 = g_pressX;
        g_drag.y0 = g_pressY;
        g_drag.col = HitColumn(g_pressX, g_pressY);
        if (g_drag.col >= 0) {
            Scroll& s = g_scroll[g_cols[g_drag.col].group];
            s.vel = 0;
            s.target = -1;
            g_drag.scroll0 = s.pos;
        }
    }
    if (g_drag.active && g_drag.col >= 0) {
        const Column& c = g_cols[g_drag.col];
        Scroll& s = g_scroll[c.group];
        float dy = g_ptrY - g_drag.y0;
        if (!g_drag.moved && MaxScroll(c) > 0 && std::abs(dy) > kDragStart) {
            g_drag.moved = true;
            g_drag.y0 += dy > 0 ? kDragStart : -kDragStart;   // no jump when the drag starts
        }
        if (g_drag.moved) {
            // Like a phone: the list follows the hand; past its ends it resists.
            float maxS = MaxScroll(c);
            float raw = g_drag.scroll0 - (g_ptrY - g_drag.y0);
            s.pos = raw < 0 ? raw * 0.35f : raw > maxS ? maxS + (raw - maxS) * 0.35f : raw;
            g_drag.hist[g_drag.n % 16] = { g_time, s.pos };
            ++g_drag.n;
        }
    }
    if (g_releaseEvent) {
        g_releaseEvent = false;
        if (g_drag.active) {
            if (!g_drag.moved) {
                // A tap: the tool (or group number) under the point where the trigger was pressed.
                int c = HitColumn(g_drag.x0, g_drag.y0);
                int r = c >= 0 ? HitRow(c, g_drag.y0) : -1;
                int b = HitBox(g_drag.x0, g_drag.y0);
                if (r >= 0) Pick(g_cols[c].tools[r] + 1);
                else if (b >= 0) PickInGroup(b);
            } else if (g_drag.col >= 0) {
                g_scroll[g_cols[g_drag.col].group].vel = ReleaseSpeed();   // the list glides on
            }
        }
        g_drag = Drag{};
    }
}

void Animate(float dt)
{
    for (int ci = 0; ci < (int)g_cols.size(); ++ci) {
        if (Dragging(ci)) continue;
        const Column& c = g_cols[ci];
        Scroll& s = g_scroll[c.group];
        float maxS = MaxScroll(c);
        if (s.target >= 0) {   // brings the tool in hand into view
            s.pos += (s.target - s.pos) * (1 - std::exp(-12 * dt));
            if (std::abs(s.target - s.pos) < 0.5f) {
                s.pos = s.target;
                s.target = -1;
            }
            continue;
        }
        if (s.vel != 0) {   // glides on after a quick drag, slowing down
            s.pos += s.vel * dt;
            s.vel *= std::exp((s.pos < 0 || s.pos > maxS ? -25.0f : -3.0f) * dt);
            if (std::abs(s.vel) < 15) s.vel = 0;
        }
        float in = std::clamp(s.pos, 0.0f, maxS);
        if (in != s.pos) {   // back inside the list (rubber band)
            s.pos += (in - s.pos) * (1 - std::exp(-14 * dt));
            if (std::abs(in - s.pos) < 0.5f) s.pos = in;
        }
    }
}

ImU32 Rgba(int r, int g, int b, float a) { return IM_COL32(r, g, b, (int)std::lround(std::clamp(a, 0.0f, 1.0f) * 255)); }

// Text with a soft dark shadow: light letters stay readable over a bright floor
// (the game's HUD text has a thin outline for the same reason).
void Text(ImDrawList* dl, ImFont* f, float size, float x, float y, int r, int g, int b, float a, const char* s,
          const char* e, const ImVec4* clip)
{
    dl->AddText(f, size, ImVec2(x + 1.3f, y + 1.3f), Rgba(0, 0, 0, a * 0.6f), s, e, 0, clip);
    dl->AddText(f, size, ImVec2(x, y), Rgba(r, g, b, a), s, e, 0, clip);
}

void Draw()
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* fName = FontOr(g_fontName);
    ImFont* fNum = FontOr(g_fontNum);
    ImFont* fTool = FontOr(g_fontTool);
    ImFont* fAmmo = FontOr(g_fontAmmo);
    const float w = (float)g_w, h = (float)g_h;
    const float listBottom = g_listTop + g_listH;
    const ImVec4 panelClip(0, 0, w, h);

    // A see-through dark plate with soft edges (about 40 % black): the game draws its
    // tool bar straight over the picture; in the headset the plate keeps the dimmed
    // names readable over a bright floor and shows where the hand can point.
    for (int i = 0; i < 3; ++i) {
        float in = i * 6.0f;
        dl->AddRectFilled(ImVec2(in, in), ImVec2(w - in, h - in), Rgba(0, 0, 0, i == 2 ? 0.27f : 0.12f), 20 - in);
    }

    if (g_cols.empty()) {
        const char* msg = g_viewVer == 0 ? Tr("Loading tools...", "Chargement des outils...")
                                         : Tr("No tools", "Aucun outil");
        float tw = TextW(fName, kNameSize, msg, nullptr);
        Text(dl, fName, kNameSize, (w - tw) * 0.5f, g_listTop + (g_listH - kNameSize) * 0.5f, 255, 255, 255, 0.6f, msg,
             nullptr, &panelClip);
    }

    // What the hand points at (a light tick in the hand when it changes).
    int hc = -1, hr = -1;
    if (g_ptrOn && !(g_drag.active && g_drag.moved)) {
        hc = HitColumn(g_ptrX, g_ptrY);
        hr = hc >= 0 ? HitRow(hc, g_ptrY) : -1;
        if (hr < 0) {
            hc = HitBox(g_ptrX, g_ptrY);
            hr = hc >= 0 ? -2 : -1;
        }
        if (hr == -1) hc = -1;
    }
    if ((hc != g_hoverCol || hr != g_hoverRow) && hc >= 0 && g_time - g_lastTick > 0.08f) {
        Haptic(0.08f, 0.01f);
        g_lastTick = g_time;
    }
    g_hoverCol = hc;
    g_hoverRow = hr;

    // The tool in hand: grows 1.4x when it changes, then eases back (easeout 0.3 s).
    const float flashK = g_flashT / kFlashTime;
    const float flash = flashK * flashK;
    int curGroup = -1;
    if (g_viewCurrent >= 1 && g_viewCurrent <= (int)g_view.size()) curGroup = g_view[g_viewCurrent - 1].group;
    const float row1 = kNameSize + kRowPad;

    for (int ci = 0; ci < (int)g_cols.size(); ++ci) {
        const Column& c = g_cols[ci];
        const Scroll& s = g_scroll[c.group];
        const float cx = (c.x0 + c.x1) * 0.5f;
        const bool curCol = c.group == curGroup;
        const float maxS = MaxScroll(c);
        const float base = Base(c);
        // (another group than the tool in hand: 60 %; while switching is locked, 20 %)
        const float colA = curCol ? 1.0f : g_viewLocked ? 0.2f : 0.6f;

        // Names, stacked above the group number (clipped: about kVisRows are seen,
        // the rest scrolls).
        // (a little wider than the column: the tool in hand grows when it changes)
        const ImVec4 clip(c.hx0 - 20, g_listTop, c.hx1 + 20, listBottom);
        dl->PushClipRect(ImVec2(clip.x, clip.y), ImVec2(clip.z, clip.w), true);
        for (int r = 0; r < (int)c.tools.size(); ++r) {
            const float y = g_listTop + base + c.top[r] - s.pos, rh = c.h[r];
            if (y + rh < g_listTop - 20 || y > listBottom + 20) continue;
            const int idx = c.tools[r];
            const Entry& e = g_view[idx];
            const bool cur = idx + 1 == g_viewCurrent, hov = ci == hc && r == hr;
            const float yc = y + rh * 0.5f;
            // (more names above / below: the ones at the edge fade out)
            float edge = 1.0f;
            if (maxS > 0) {
                const float fade = row1 * 0.9f;
                if (s.pos > 0.5f) edge = std::min(edge, (yc - g_listTop) / fade);
                if (s.pos < maxS - 0.5f) edge = std::min(edge, (listBottom - yc) / fade);
                edge = std::clamp(edge, 0.0f, 1.0f);
            }
            float a = cur ? 1.0f : !e.selectable ? 0.2f : colA;
            if (hov) a = std::max(a, e.selectable ? 1.0f : 0.35f);
            a *= edge;
            const float k = cur ? 1.0f + 0.4f * flash : 1.0f;
            const float size = kNameSize * k;
            float textW = 0;
            for (int l = 0; l < c.lines[r]; ++l) {
                const Line& ln = g_lines[c.line0[r] + l];
                textW = std::max(textW, TextW(fName, kNameSize, ln.a, ln.b));
            }
            if (cur || hov) {
                // Dark box behind the tool in hand (black 40 %, like the game), lighter for the pointed one.
                const float bw = std::min(std::max(kSelMinW, textW + 16), c.x1 - c.x0 + kColGap) * k, bh = rh * k;
                dl->AddRectFilled(ImVec2(cx - bw * 0.5f, yc - bh * 0.5f), ImVec2(cx + bw * 0.5f, yc + bh * 0.5f),
                                  Rgba(0, 0, 0, (cur ? 0.4f : 0.0f) + (hov ? 0.25f : 0.0f)), kSelRound * k);
            }
            // Each line centred (UiTextAlignment("center")); light yellow for the tool in hand.
            float ly = yc - c.lines[r] * size * 0.5f;
            for (int l = 0; l < c.lines[r]; ++l) {
                const Line& ln = g_lines[c.line0[r] + l];
                const float lw = TextW(fName, size, ln.a, ln.b);
                if (cur) Text(dl, fName, size, cx - lw * 0.5f, ly, 255, 255, 128, a, ln.a, ln.b, &clip);
                else Text(dl, fName, size, cx - lw * 0.5f, ly, 255, 255, 255, a, ln.a, ln.b, &clip);
                ly += size;
            }
        }
        dl->PopClipRect();

        // A long list: a thin bar shows where it is (the game shows the 5 names around
        // the tool in hand; here the hand can also scroll them).
        if (maxS > 0) {
            const float th = std::max(18.0f, g_listH * g_listH / c.content);
            const float t = std::clamp(s.pos / maxS, 0.0f, 1.0f);
            const float ty = g_listTop + t * (g_listH - th);
            dl->AddRectFilled(ImVec2(c.x1 - 4, g_listTop), ImVec2(c.x1 - 1, listBottom), Rgba(255, 255, 255, 0.10f), 1.5f);
            dl->AddRectFilled(ImVec2(c.x1 - 4, ty), ImVec2(c.x1 - 1, ty + th), Rgba(255, 255, 255, 0.45f), 1.5f);
        }

        // Group number: black on a white rounded box (grey while switching is locked),
        // the group of the tool in hand underlined in light yellow.
        const bool hovBox = ci == hc && hr == -2;
        const float boxA = curCol || hovBox ? 1.0f : 0.6f;
        const int grey = g_viewLocked && !curCol ? 115 : 255;
        const ImVec2 b0(cx - kBox * 0.5f, g_boxTop), b1(cx + kBox * 0.5f, g_boxTop + kBox);
        dl->AddRectFilled(b0, b1, Rgba(grey, grey, grey, boxA), kBoxRound);
        if (hovBox) dl->AddRect(ImVec2(b0.x - 3, b0.y - 3), ImVec2(b1.x + 3, b1.y + 3), Rgba(255, 255, 128, 1.0f), kBoxRound + 2, 0, 2.0f);
        char num[8];
        snprintf(num, sizeof(num), "%d", c.group);
        const ImVec2 ns = fNum->CalcTextSizeA(kNumSize, FLT_MAX, 0, num);
        dl->AddText(fNum, kNumSize, ImVec2(cx - ns.x * 0.5f, g_boxTop + (kBox - ns.y) * 0.5f), Rgba(0, 0, 0, boxA), num);
        if (curCol) {
            const float by = g_boxTop + kBox + kBarGap;
            dl->AddRectFilled(ImVec2(b0.x, by), ImVec2(b1.x, by + kBarH), Rgba(255, 255, 128, 1.0f), 2 * kF);
        }
    }

    // The tool in hand and its ammo, bold and centred, as at the bottom of the PC screen.
    if (g_viewCurrent >= 1 && g_viewCurrent <= (int)g_view.size()) {
        const Entry& e = g_view[g_viewCurrent - 1];
        if (!e.ammo.empty()) {
            float aw = TextW(fAmmo, kAmmoSize, e.ammo.c_str(), nullptr);
            Text(dl, fAmmo, kAmmoSize, (w - aw) * 0.5f, g_infoTop, 255, 255, 255, 1.0f, e.ammo.c_str(), nullptr, &panelClip);
        }
        const std::string up = Upper(e.name);
        float tw = TextW(fTool, kToolSize, up.c_str(), nullptr);
        Text(dl, fTool, kToolSize, std::max(kPadX, (w - tw) * 0.5f), g_infoTop + kAmmoSize + 2, 255, 255, 255, 1.0f,
             up.c_str(), nullptr, &panelClip);
    }

    // Moved with a grip: a light yellow frame (the colour of the tool in hand).
    if (g_heldBy >= 0) dl->AddRect(ImVec2(1.5f, 1.5f), ImVec2(w - 1.5f, h - 1.5f), Rgba(255, 255, 128, 0.9f), 19, 0, 3.0f);

    // Where the hand points (a white dot, like the game's aim dot).
    if (g_ptrOn) {
        const float r = g_drag.active && g_drag.moved ? 6.5f : 4.5f;
        dl->AddCircleFilled(ImVec2(g_ptrX, g_ptrY), r + 2.0f, Rgba(0, 0, 0, 0.75f), 20);
        dl->AddCircleFilled(ImVec2(g_ptrX, g_ptrY), r, Rgba(255, 255, 255, 1.0f), 20);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------
void AddFonts(ImFontAtlas* atlas)
{
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0152, 0x0153, 0x2013, 0x2026, 0 };
    static const ImWchar extra[] = { 0x2026, 0x2026, 0x221E, 0x221E, 0 };   // "…" (cut names), "∞" (ammo of some mods)
    char segoe[MAX_PATH];
    GetWindowsDirectoryA(segoe, MAX_PATH);
    strcat_s(segoe, "\\Fonts\\segoeui.ttf");
    const bool haveSegoe = GetFileAttributesA(segoe) != INVALID_FILE_ATTRIBUTES;
    const std::wstring dir = ModDirectory() + L"data\\ui\\font\\";
    auto add = [&](const wchar_t* file, float size) -> ImFont* {
        std::wstring path = dir + file;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            Log("Inventory: game font %ls not found, default font used", path.c_str());
            return nullptr;
        }
        char utf8[MAX_PATH * 3];
        if (!WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr)) return nullptr;
        ImFontConfig cfg;
        cfg.OversampleH = 3;
        ImFont* f = atlas->AddFontFromFileTTF(utf8, size, &cfg, ranges);
        if (f && haveSegoe) {
            // (letters the game font lacks; the ones it has are kept)
            ImFontConfig merge;
            merge.MergeMode = true;
            atlas->AddFontFromFileTTF(segoe, size, &merge, extra);
        }
        return f;
    };
    g_fontName = add(L"medium.ttf", kNameSize);
    g_fontNum = add(L"bold.ttf", kNumSize);
    g_fontTool = add(L"bold.ttf", kToolSize);
    g_fontAmmo = add(L"bold.ttf", kAmmoSize);
}

// ---------------------------------------------------------------------------
// Lua side
// ---------------------------------------------------------------------------
void HandleLuaMessage(const std::string& s)
{
    if (s.size() < 6) return;
    int f[5] = {};
    std::string rest;
    std::lock_guard<std::mutex> lock(g_mutex);
    switch (s[5]) {
    case 'B':   // "TDVRIB <ver> <count>": a new pass over the whole list
        if (!Fields(s, 2, f, nullptr) || f[1] < 0 || f[1] > kMaxTools) return;
        g_stageVer = f[0];
        g_stage.assign((size_t)f[1], Staged{});
        // The same version sent again: Lua restarted (level reloaded) or missed our
        // acknowledgement; the list it sends now replaces ours.
        if (f[0] == g_pubVer) g_pubVer = 0;
        PublishIfComplete();   // (an empty list is complete at once)
        break;
    case 'T': { // "TDVRIT <ver> <i> <group> <chunks> <selectable> <ammo>"
        if (!Fields(s, 5, f, &rest) || f[0] != g_stageVer || f[1] < 1 || f[1] > (int)g_stage.size()) return;
        Staged& t = g_stage[f[1] - 1];
        t.header = true;
        t.group = f[2];
        t.chunks = std::clamp(f[3], 0, kMaxChunks);
        t.got &= (1u << t.chunks) - 1;
        t.selectable = f[4] != 0;
        t.ammo = Unescape(rest);
        PublishIfComplete();
        break;
    }
    case 'N': { // "TDVRIN <ver> <i> <k> <text>"
        if (!Fields(s, 3, f, &rest) || f[0] != g_stageVer || f[1] < 1 || f[1] > (int)g_stage.size() || f[2] < 1 ||
            f[2] > kMaxChunks)
            return;
        Staged& t = g_stage[f[1] - 1];
        t.part[f[2] - 1] = rest;
        t.got |= 1u << (f[2] - 1);
        if (t.header) PublishIfComplete();
        break;
    }
    case 'A': { // "TDVRIA <ver> <i> <selectable> <ammo>"
        if (!Fields(s, 3, f, &rest)) return;
        std::string ammo = Unescape(rest);
        bool sel = f[2] != 0;
        if (f[0] == g_stageVer && f[1] >= 1 && f[1] <= (int)g_stage.size()) {
            g_stage[f[1] - 1].selectable = sel;
            g_stage[f[1] - 1].ammo = ammo;
        }
        if (f[0] == g_pubVer && f[1] >= 1 && f[1] <= (int)g_pub.size()) {
            Entry& e = g_pub[f[1] - 1];
            if (e.selectable != sel || e.ammo != ammo) {
                e.selectable = sel;
                e.ammo = ammo;
                ++g_serial;
            }
        }
        break;
    }
    case 'C':   // "TDVRIC <ver> <i> <locked>"
        if (!Fields(s, 3, f, nullptr)) return;
        g_currentVer = f[0];
        g_current = f[1];
        g_locked = f[2] != 0;
        break;
    default:
        break;
    }
}

std::string ResponseFields()
{
    char b[80];
    std::lock_guard<std::mutex> lock(g_mutex);
    snprintf(b, sizeof(b), " INV %d %d %d %d %d", g_pubVer, g_selSeq, g_selVer, g_selIdx, g_open.load() ? 1 : 0);
    return b;
}

void SetGameFlags(int luaFlags) { g_luaFlags = luaFlags; }

bool CameraNearPlayer() { return (g_luaFlags.load() & kLuaFlagCamNearPlayer) != 0; }

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------
void Toggle()
{
    if (g_show == Show::Brief) {
        g_want = true;   // shown for a moment: now it stays, and the trigger works on it
    } else {
        g_want = !g_want;
        g_briefLeft = 0;
    }
    Apply();
    static int logged = 0;
    if (logged++ < 30) Log("Inventory: R3 -> %s", g_want ? (g_show == Show::Pinned ? "shown" : "shown when allowed") : "hidden");
}

void ShowBrief()
{
    if (!Cfg().inventoryOnSwitch || g_want) return;
    if (g_luaFlags.load() & 2) return;   // holding an object: the stick (mouse wheel) moves it
    {
        // Switching locked by the game (a mod tool that uses the wheel...): the stick
        // does not change the tool, nothing to show.
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_locked) return;
    }
    g_briefLeft = std::max(g_briefLeft, kBriefTime);
    Apply();
}

void Close()
{
    g_want = false;
    g_briefLeft = 0;
    Apply();
}

void Update(float dt, bool allowed)
{
    g_allowed = allowed;
    if (!allowed) g_briefLeft = 0;
    else if (g_briefLeft > 0) g_briefLeft = std::max(0.0f, g_briefLeft - dt);
    Apply();
}

bool Visible() { return g_show != Show::Hidden; }

bool Interactive() { return g_show == Show::Pinned; }

void SetHeld(int hand)
{
    if (hand >= 0 && hand != g_heldBy && hand == Input::PointerHand() && g_pressOwned) {
        // (a whole-fist squeeze: the trigger pressed on a name just before the grip - the grip masks
        // that trigger, its "release" must not pick the tool: the press is dropped)
        g_pressOwned = g_pressEvent = g_releaseEvent = false;
        g_drag = Drag{};
    }
    g_heldBy = hand;
}

void RequestPlace() { g_placePending = true; }

// (the trigger of the hand pointing at the panel: Input::PointerHand)
static bool CapturesPointerTrigger()
{
    return g_swallow || g_pressOwned || (g_show == Show::Pinned && g_ptrOn && !g_pressGame);
}

bool CapturesTrigger() { return CapturesPointerTrigger() && Input::PointerHand() == (Cfg().leftHanded ? 0 : 1); }

bool CapturesOffTrigger() { return CapturesPointerTrigger() && Input::PointerHand() != (Cfg().leftHanded ? 0 : 1); }

void SetPointer(bool hit, bool onPanel, float x, float y, float trigger, bool focused)
{
    const bool down = g_trigDown ? trigger > 0.35f : trigger > 0.55f;
    const bool pressed = down && !g_trigDown, released = !down && g_trigDown;
    g_trigDown = down;
    const bool pinned = g_show == Show::Pinned;   // (a brief panel is display only)
    // (looking elsewhere, the ray may cross the panel while aiming at the floor: the
    // trigger stays the game's; a press that began on the panel keeps its drag)
    g_ptrHit = pinned && hit && (focused || g_pressOwned);
    g_ptrOn = g_ptrHit && onPanel;
    if (g_ptrHit) {
        g_ptrX = x;
        g_ptrY = y;
    }
    if (released) {
        if (g_pressOwned) g_releaseEvent = true;
        g_pressOwned = g_swallow = g_pressGame = false;
    }
    if (!pressed) return;
    if (pinned && g_ptrOn && !g_pressGame) {
        g_pressOwned = true;
        g_pressEvent = true;
        g_pressX = x;
        g_pressY = y;
    } else {
        g_pressGame = true;   // beside the panel (or not looking at it): the tool fires
    }
}

bool ConsumePlaceRequest()
{
    bool r = g_placePending;
    g_placePending = false;
    return r;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void Build(float dt)
{
    dt = std::clamp(dt, 0.0f, 0.1f);
    g_time += dt;
    g_flashT = std::max(0.0f, g_flashT - dt);
    Refresh();
    Layout();
    FollowCurrent();
    HandlePointer();
    Animate(dt);
    Draw();
    g_justShown = false;
}

int PanelW() { return g_w; }
int PanelH() { return g_h; }

} // namespace Inventory

#pragma once
#include "config.h"

// The mod's two languages ([Affichage] language: "en", the default of a fresh install, or "fr"; the installer asks,
// the settings panel switches it at once). Tr("English", "Français") returns the text of the language in use: both
// are string literals (nothing allocated), picked at each call, so a switch shows on the next frame. Printf-style
// pairs must take the same arguments in the same order. ImGui labels that change with the language and must keep
// their state (tabs, windows) end with a fixed "###id".
inline bool LangFr() { return Cfg().language == "fr"; }
inline const char* Tr(const char* en, const char* fr) { return LangFr() ? fr : en; }

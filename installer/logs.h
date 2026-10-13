#pragma once
#include <windows.h>
#include <string>

// Zips the mod's logs, the player's settings and a description of the PC into outDir (empty: the Desktop) as
// TeardownVR_journaux_<date>.zip; zipPath gets its path, logCount the number of logs found. ERROR_SUCCESS or a
// Windows error.
DWORD CollectLogs(const std::wstring& game, const std::wstring& outDir, std::wstring& zipPath, int& logCount);
// Opens Explorer on the file's folder with the file selected.
void ShowInExplorer(const std::wstring& path);

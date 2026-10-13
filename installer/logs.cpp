// "Préparer mes journaux": the files useful for a bug report, in one zip the player can attach wherever they
// choose (nothing is sent):
//   TeardownVR.log                 the last (or current) session's log, next to the game
//   journaux\TeardownVR_*.log      the sessions before it (the mod keeps the last 10 in <game>\TeardownVR_journaux,
//                                  src/log.cpp ArchivePreviousLog)
//   TeardownVR.ini                 the player's settings
//   infos_PC.txt                   Windows, processor, memory, graphics cards and driver, the active OpenXR runtime,
//                                  the game's exe and the installed mod's DLL (size and date), this installer's version
// Read-only on the game folder (the logs can be read while the game runs). The zip is written uncompressed ("stored":
// any unzip tool reads it, no compression library). Nothing is sent anywhere.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <shlobj.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "logs.h"
#include "payload.h"

#pragma comment(lib, "dxgi.lib")

namespace {

std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool ReadFileShared(const std::wstring& path, std::string& out)
{
    // (the game keeps its log open for writing with read sharing: read it with every sharing mode)
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (256ll << 20);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) != FALSE);
        out.resize(got);   // (a log still growing: what was there)
    }
    CloseHandle(h);
    return ok;
}

// --- a minimal zip writer (stored entries, no zip64: a few MB of text) ---------------------------------------------
uint32_t Crc32(const std::string& d)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : d) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void Le16(std::string& s, uint32_t v) { s += (char)(v & 0xFF); s += (char)((v >> 8) & 0xFF); }
void Le32(std::string& s, uint32_t v) { Le16(s, v & 0xFFFF); Le16(s, v >> 16); }

struct Zip
{
    HANDLE h = INVALID_HANDLE_VALUE;
    std::string central;
    uint32_t offset = 0, count = 0;
    uint16_t time = 0, date = 0;
    bool ok = true;

    void Put(const std::string& s)
    {
        DWORD w = 0;
        if (!ok || !WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr) || w != s.size()) ok = false;
        offset += (uint32_t)s.size();
    }
    void Add(const std::string& name, const std::string& data)
    {
        const uint32_t crc = Crc32(data), size = (uint32_t)data.size(), at = offset;
        std::string lh;
        Le32(lh, 0x04034b50);
        Le16(lh, 20);       // version needed: 2.0
        Le16(lh, 0x0800);   // names in UTF-8
        Le16(lh, 0);        // stored
        Le16(lh, time);
        Le16(lh, date);
        Le32(lh, crc);
        Le32(lh, size);
        Le32(lh, size);
        Le16(lh, (uint32_t)name.size());
        Le16(lh, 0);
        lh += name;
        Put(lh);
        Put(data);
        std::string& c = central;
        Le32(c, 0x02014b50);
        Le16(c, 20);        // made by: 2.0, MS-DOS attributes
        Le16(c, 20);
        Le16(c, 0x0800);
        Le16(c, 0);
        Le16(c, time);
        Le16(c, date);
        Le32(c, crc);
        Le32(c, size);
        Le32(c, size);
        Le16(c, (uint32_t)name.size());
        Le16(c, 0);         // extra
        Le16(c, 0);         // comment
        Le16(c, 0);         // disk
        Le16(c, 0);         // internal attributes
        Le32(c, 0);         // external attributes
        Le32(c, at);
        c += name;
        ++count;
    }
    void Finish()
    {
        const uint32_t cd = offset;
        Put(central);
        std::string e;
        Le32(e, 0x06054b50);
        Le16(e, 0);
        Le16(e, 0);
        Le16(e, count);
        Le16(e, count);
        Le32(e, (uint32_t)central.size());
        Le32(e, cd);
        Le16(e, 0);
        Put(e);
    }
};

// --- the PC's description -----------------------------------------------------------------------------------------
std::wstring RegStr(HKEY root, const wchar_t* key, const wchar_t* name)
{
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &size) !=
        ERROR_SUCCESS)
        return L"?";
    return buf;
}

DWORD RegDword(HKEY root, const wchar_t* key, const wchar_t* name)
{
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : 0;
}

std::string FileLine(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return "absent";
    FILETIME local;
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&fa.ftLastWriteTime, &local);
    FileTimeToSystemTime(&local, &st);
    char b[128];
    snprintf(b, sizeof(b), "%llu octets, modifié le %02u/%02u/%04u %02u:%02u",
             ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow, st.wDay, st.wMonth, st.wYear, st.wHour,
             st.wMinute);
    return b;
}

std::string PcInfo(const std::wstring& game)
{
    std::string s = "\xEF\xBB\xBFTeardown VR - description du PC (pour un rapport de bug)\r\n\r\n";   // (UTF-8 BOM: Notepad)
    SYSTEMTIME now;
    GetLocalTime(&now);
    char b[512];
    snprintf(b, sizeof(b), "Date : %02u/%02u/%04u %02u:%02u\r\nVersion de l'installateur : %s\r\n\r\n", now.wDay,
             now.wMonth, now.wYear, now.wHour, now.wMinute, ToUtf8(kPayloadVersion).c_str());
    s += b;
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    // (Windows 11 still says "Windows 10" in ProductName: the build number tells, 22000 and later)
    std::wstring product = RegStr(HKEY_LOCAL_MACHINE, nt, L"ProductName");
    if (_wtoi(RegStr(HKEY_LOCAL_MACHINE, nt, L"CurrentBuildNumber").c_str()) >= 22000 && product.rfind(L"Windows 10", 0) == 0)
        product.replace(8, 2, L"11");
    snprintf(b, sizeof(b), "Windows : %s %s (build %s.%lu)\r\n", ToUtf8(product).c_str(),
             ToUtf8(RegStr(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion")).c_str(),
             ToUtf8(RegStr(HKEY_LOCAL_MACHINE, nt, L"CurrentBuildNumber")).c_str(),
             (unsigned long)RegDword(HKEY_LOCAL_MACHINE, nt, L"UBR"));
    s += b;
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    snprintf(b, sizeof(b), "Processeur : %s (%lu threads)\r\n",
             ToUtf8(RegStr(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString")).c_str(),
             (unsigned long)si.dwNumberOfProcessors);
    s += b;
    MEMORYSTATUSEX ms{ sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    snprintf(b, sizeof(b), "Mémoire : %.1f Go\r\n", ms.ullTotalPhys / 1073741824.0);
    s += b;
    IDXGIFactory1* f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) {
        IDXGIAdapter1* a = nullptr;
        std::vector<std::string> gpus;   // (the same card can be listed more than once: counted)
        std::vector<int> counts;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            LARGE_INTEGER umd{};
            const bool drv = SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                snprintf(b, sizeof(b), "%s, %.1f Go, pilote %u.%u.%u.%u", ToUtf8(d.Description).c_str(),
                         d.DedicatedVideoMemory / 1073741824.0, drv ? (unsigned)HIWORD(umd.HighPart) : 0,
                         drv ? (unsigned)LOWORD(umd.HighPart) : 0, drv ? (unsigned)HIWORD(umd.LowPart) : 0,
                         drv ? (unsigned)LOWORD(umd.LowPart) : 0);
                size_t k = 0;
                while (k < gpus.size() && gpus[k] != b) ++k;
                if (k == gpus.size()) {
                    gpus.push_back(b);
                    counts.push_back(0);
                }
                ++counts[k];
            }
            a->Release();
        }
        f->Release();
        for (size_t k = 0; k < gpus.size(); ++k)
            s += "Carte graphique : " + gpus[k] +
                 (counts[k] > 1 ? " (vue " + std::to_string(counts[k]) + " fois par Windows)" : std::string()) + "\r\n";
    }
    snprintf(b, sizeof(b), "Runtime OpenXR actif : %s\r\n",
             ToUtf8(RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime")).c_str());
    s += b;
    s += "\r\nDossier du jeu : " + ToUtf8(game) + "\r\n";
    s += "teardown.exe : " + FileLine(game + L"\\teardown.exe") + "\r\n";
    s += "dsound.dll (le mod) : " + FileLine(game + L"\\dsound.dll") + "\r\n";
    s += "mods\\teardownvr\\main.lua : " + FileLine(game + L"\\mods\\teardownvr\\main.lua") + "\r\n";
    return s;
}

std::wstring DesktopDir()
{
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) d = p;
    if (p) CoTaskMemFree(p);
    return d;
}

} // namespace

DWORD CollectLogs(const std::wstring& game, const std::wstring& outDir, std::wstring& zipPath, int& logCount)
{
    logCount = 0;
    const std::wstring dir = outDir.empty() ? DesktopDir() : outDir;
    if (dir.empty()) return ERROR_PATH_NOT_FOUND;
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t name[96];
    swprintf_s(name, L"TeardownVR_journaux_%04u-%02u-%02u_%02u-%02u.zip", now.wYear, now.wMonth, now.wDay, now.wHour,
               now.wMinute);
    zipPath = dir + L"\\" + name;
    const std::wstring tmp = zipPath + L".tmp";
    Zip z;
    z.h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (z.h == INVALID_HANDLE_VALUE) return GetLastError();
    z.time = (uint16_t)((now.wHour << 11) | (now.wMinute << 5) | (now.wSecond / 2));
    z.date = (uint16_t)(((now.wYear - 1980) << 9) | (now.wMonth << 5) | now.wDay);
    std::string data;
    if (ReadFileShared(game + L"\\TeardownVR.log", data)) {
        z.Add("TeardownVR.log", data);
        ++logCount;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((game + L"\\TeardownVR_journaux\\TeardownVR_*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                !ReadFileShared(game + L"\\TeardownVR_journaux\\" + fd.cFileName, data))
                continue;
            z.Add("journaux/" + ToUtf8(fd.cFileName), data);
            ++logCount;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (ReadFileShared(game + L"\\TeardownVR.ini", data)) z.Add("TeardownVR.ini", data);
    z.Add("infos_PC.txt", PcInfo(game));
    z.Finish();
    const bool ok = z.ok && FlushFileBuffers(z.h);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(z.h);
    if (ok && !MoveFileExW(tmp.c_str(), zipPath.c_str(), MOVEFILE_REPLACE_EXISTING)) err = GetLastError();
    if (err != ERROR_SUCCESS) DeleteFileW(tmp.c_str());
    return err;
}

void ShowInExplorer(const std::wstring& path)
{
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (!pidl) return;
    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
}

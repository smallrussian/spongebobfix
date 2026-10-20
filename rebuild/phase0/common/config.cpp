#include "config.h"
#include <windows.h>
#include <algorithm>
#include <mutex>

const std::wstring& GameDir() {
    static std::wstring dir = [] {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring s(path, n);
        size_t slash = s.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring(L".\\") : s.substr(0, slash + 1);
    }();
    return dir;
}

static int ReadInt(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(sec, key, def, ini.c_str());
}

static const wchar_t* kDefaultIni =
    L"; sb2modern settings for SpongeBob SquarePants: Employee of the Month\r\n"
    L"[Display]\r\n"
    L"; 0 = borderless fullscreen window, 1 = normal window (Alt+Enter toggles)\r\n"
    L"WindowMode=0\r\n"
    L"; Windowed size as a multiple of 640x480\r\n"
    L"WindowScale=2\r\n"
    L"; 0 = keep 4:3 aspect, 1 = stretch to fill, 2 = integer scaling only\r\n"
    L"Scaling=0\r\n"
    L"; 0 = nearest, 1 = bilinear, 2 = sharp bilinear\r\n"
    L"Filter=1\r\n"
    L"VSync=1\r\n"
    L"; Frames per second cap (0 = unlimited). The game was designed around 60.\r\n"
    L"FrameLimit=60\r\n"
    L"; -1 = as the game requests, 0 = point, 1 = linear, 2 = anisotropic\r\n"
    L"TextureFilter=-1\r\n"
    L"\r\n"
    L"[Audio]\r\n"
    L"; Leave empty to follow the Windows default output device (switches live).\r\n"
    L"; Otherwise part of a device name, e.g. Device=Headphones\r\n"
    L"Device=\r\n"
    L"LatencyMs=20\r\n"
    L"MasterVolume=100\r\n"
    L"\r\n"
    L"[Compat]\r\n"
    L"NoCD=1\r\n"
    L"PreciseTimer=1\r\n"
    L"ReduceCPU=1\r\n"
    L"\r\n"
    L"[Debug]\r\n"
    L"; 0 = off, 1 = info, 2 = verbose\r\n"
    L"LogLevel=1\r\n";

const Config& GetConfig() {
    static Config cfg;
    static std::once_flag once;
    std::call_once(once, [] {
        std::wstring ini = GameDir() + L"sb2modern.ini";
        if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
            HANDLE h = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                // Write as UTF-16LE with BOM so GetPrivateProfile* reads it as Unicode.
                DWORD written;
                const wchar_t bom = 0xFEFF;
                WriteFile(h, &bom, sizeof(bom), &written, nullptr);
                WriteFile(h, kDefaultIni, (DWORD)(wcslen(kDefaultIni) * sizeof(wchar_t)), &written, nullptr);
                CloseHandle(h);
            }
        }
        cfg.windowMode     = ReadInt(ini, L"Display", L"WindowMode", cfg.windowMode);
        cfg.windowScale    = std::max(1, ReadInt(ini, L"Display", L"WindowScale", cfg.windowScale));
        cfg.scaling        = ReadInt(ini, L"Display", L"Scaling", cfg.scaling);
        cfg.filter         = ReadInt(ini, L"Display", L"Filter", cfg.filter);
        cfg.vsync          = ReadInt(ini, L"Display", L"VSync", cfg.vsync) != 0;
        cfg.frameLimit     = ReadInt(ini, L"Display", L"FrameLimit", cfg.frameLimit);
        cfg.textureFilter  = ReadInt(ini, L"Display", L"TextureFilter", cfg.textureFilter);
        wchar_t dev[256] = {};
        GetPrivateProfileStringW(L"Audio", L"Device", L"", dev, 256, ini.c_str());
        cfg.audioDevice    = dev;
        cfg.audioLatencyMs = ReadInt(ini, L"Audio", L"LatencyMs", cfg.audioLatencyMs);
        cfg.masterVolume   = ReadInt(ini, L"Audio", L"MasterVolume", cfg.masterVolume);
        cfg.noCD           = ReadInt(ini, L"Compat", L"NoCD", cfg.noCD) != 0;
        cfg.preciseTimer   = ReadInt(ini, L"Compat", L"PreciseTimer", cfg.preciseTimer) != 0;
        cfg.reduceCPU      = ReadInt(ini, L"Compat", L"ReduceCPU", cfg.reduceCPU) != 0;
        cfg.logLevel       = ReadInt(ini, L"Debug", L"LogLevel", cfg.logLevel);
    });
    return cfg;
}

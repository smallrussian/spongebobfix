#pragma once
#include <string>

// Settings read from sb2modern.ini next to sb2.exe. Missing keys keep these defaults.
struct Config {
    // [Display]
    int windowMode = 0;        // 0 = borderless fullscreen window, 1 = normal window
    int windowScale = 2;       // windowed size as a multiple of 640x480
    int scaling = 0;           // 0 = keep 4:3, 1 = stretch, 2 = integer multiples only
    int filter = 1;            // 0 = nearest, 1 = bilinear, 2 = sharp bilinear
    bool vsync = true;
    int frameLimit = 60;       // 0 = unlimited; the game was built for ~60 fps
    int textureFilter = -1;    // -1 = as the game asks, 0 = point, 1 = linear, 2 = anisotropic 16x

    // [Audio]
    std::wstring audioDevice;  // empty = follow the Windows default output device
    int audioLatencyMs = 20;
    int masterVolume = 100;    // percent

    // [Compat]
    bool noCD = true;
    bool preciseTimer = true;
    bool reduceCPU = true;

    // [Debug]
    int logLevel = 1;          // 0 = off, 1 = info, 2 = verbose (logs every unimplemented call)
};

const Config& GetConfig();
// Directory containing the game executable, with a trailing backslash.
const std::wstring& GameDir();

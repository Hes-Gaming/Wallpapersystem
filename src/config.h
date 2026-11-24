#pragma once

#include <string>

struct AppConfig
{
    std::wstring wallpaperPath;  // Absolute path to the wallpaper video.
    bool startWithWindows = false;
    int transparency = -1;       // 1 on, 0 off, -1 not in config.md (leave Windows alone).
    bool fullTransparency = false;  // Fully clear taskbar (ClearTaskbar.dll in Explorer).
    bool valid = false;
    std::wstring error;          // Human readable reason when valid == false.
};

// Directory that contains the running executable, without a trailing slash.
std::wstring GetExeDirectory();

// Reads config.md. Search order: executable directory, then current working
// directory. The file must contain a line: Wallpaper = <file>.mp4
// The video is resolved inside the "wallpapers" folder next to config.md.
AppConfig LoadConfig();

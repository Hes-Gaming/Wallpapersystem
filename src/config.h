#pragma once

#include <string>

struct AppConfig
{
    std::wstring wallpaperPath;  // Absolute path to the wallpaper video.
    bool startWithWindows = false;
    bool valid = false;
    std::wstring error;          // Human readable reason when valid == false.
};

// Directory that contains the running executable, without a trailing slash.
std::wstring GetExeDirectory();

// Reads config.md. Search order: executable directory, then current working
// directory. The file must contain a line: Wallpaper = <file>.mp4
// The video is resolved inside the "wallpapers" folder next to config.md.
AppConfig LoadConfig();

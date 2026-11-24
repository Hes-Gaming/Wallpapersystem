#include "config.h"

#include <windows.h>

#include <cstring>
#include <string>

namespace
{

constexpr size_t kMaxTextFileSize = 16u * 1024u * 1024u;

std::string Trim(const std::string& text)
{
    const char* const whitespace = " \t";

    const size_t first = text.find_first_not_of(whitespace);
    if (first == std::string::npos)
        return std::string();

    const size_t last = text.find_last_not_of(whitespace);
    return text.substr(first, last - first + 1);
}

bool FileExists(const std::wstring& path)
{
    if (path.empty())
        return false;

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return false;

    return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ReadFileBytes(const std::wstring& path, std::string& data)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size = {};
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > kMaxTextFileSize)
    {
        CloseHandle(file);
        return false;
    }

    data.resize(static_cast<size_t>(size.QuadPart));

    size_t offset = 0;
    while (offset < data.size())
    {
        const size_t remaining = data.size() - offset;
        const DWORD chunk = static_cast<DWORD>(remaining > 0x10000000u ? 0x10000000u : remaining);

        DWORD read = 0;
        if (ReadFile(file, &data[offset], chunk, &read, nullptr) == FALSE || read == 0)
        {
            CloseHandle(file);
            return false;
        }

        offset += static_cast<size_t>(read);
    }

    CloseHandle(file);
    return true;
}

std::wstring GetCurrentDirectoryString()
{
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;)
    {
        const DWORD length =
            GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0)
            return std::wstring();

        if (length < buffer.size())
        {
            buffer.resize(length);
            return buffer;
        }

        if (buffer.size() >= 32768)
            return std::wstring();

        buffer.resize(buffer.size() * 2);
    }
}

bool HasMp4Extension(const std::string& value)
{
    if (value.size() < 4)
        return false;

    return _stricmp(value.c_str() + (value.size() - 4), ".mp4") == 0;
}

} // namespace

std::wstring GetExeDirectory()
{
    try
    {
        std::wstring buffer(MAX_PATH, L'\0');

        for (;;)
        {
            const DWORD length =
                GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
                return std::wstring();

            if (length < buffer.size())
            {
                buffer.resize(length);
                break;
            }

            if (buffer.size() >= 32768)
                return std::wstring();

            buffer.resize(buffer.size() * 2);
        }

        const std::wstring::size_type slash = buffer.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return std::wstring();

        buffer.resize(slash);
        return buffer;
    }
    catch (...)
    {
        return std::wstring();
    }
}

AppConfig LoadConfig()
{
    AppConfig config;

    try
    {
        std::wstring configDir = GetExeDirectory();
        std::wstring configPath =
            configDir.empty() ? std::wstring() : configDir + L"\\config.md";

        if (!FileExists(configPath))
        {
            const std::wstring currentDir = GetCurrentDirectoryString();
            const std::wstring candidate =
                currentDir.empty() ? std::wstring() : currentDir + L"\\config.md";

            if (!FileExists(candidate))
            {
                config.error = L"config.md was not found next to the executable or in the "
                               L"current working directory.";
                return config;
            }

            configDir = currentDir;
            configPath = candidate;
        }

        std::string bytes;
        if (!ReadFileBytes(configPath, bytes))
        {
            config.error = L"Unable to read config.md: " + configPath;
            return config;
        }

        if (bytes.size() >= 3 &&
            static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB &&
            static_cast<unsigned char>(bytes[2]) == 0xBF)
        {
            bytes.erase(0, 3);
        }

        std::string value;
        bool found = false;

        size_t position = 0;
        while (position <= bytes.size())
        {
            size_t lineEnd = bytes.find('\n', position);
            if (lineEnd == std::string::npos)
                lineEnd = bytes.size();

            std::string line = bytes.substr(position, lineEnd - position);
            position = lineEnd + 1;

            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            const std::string trimmed = Trim(line);
            if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
                continue;

            const size_t equals = trimmed.find('=');
            if (equals == std::string::npos)
                continue;

            const std::string key = Trim(trimmed.substr(0, equals));
            const std::string flag = Trim(trimmed.substr(equals + 1));
            const bool on = _stricmp(flag.c_str(), "true") == 0 ||
                            _stricmp(flag.c_str(), "yes") == 0 || flag == "1";
            if (_stricmp(key.c_str(), "startwithwindows") == 0)
            {
                config.startWithWindows = on;
                continue;
            }
            if (_stricmp(key.c_str(), "transparency") == 0)
            {
                config.transparency = on ? 1 : 0;
                continue;
            }
            if (_stricmp(key.c_str(), "fulltransparency") == 0)
            {
                config.fullTransparency = on;
                continue;
            }
            if (found || _stricmp(key.c_str(), "wallpaper") != 0)
                continue;

            std::string parsed = Trim(trimmed.substr(equals + 1));
            if (parsed.size() >= 2 &&
                ((parsed.front() == '"' && parsed.back() == '"') ||
                 (parsed.front() == '\'' && parsed.back() == '\'')))
            {
                parsed = parsed.substr(1, parsed.size() - 2);
            }

            value = parsed;
            found = true;
        }

        if (!found)
        {
            config.error = L"config.md does not define Wallpaper = <file>.mp4";
            return config;
        }

        if (value.empty())
        {
            config.error = L"config.md contains an empty Wallpaper entry.";
            return config;
        }

        if (value.find("..") != std::string::npos)
        {
            config.error = L"The Wallpaper entry must stay inside the wallpapers folder.";
            return config;
        }

        if (!HasMp4Extension(value))
        {
            config.error =
                L"Unsupported wallpaper file type. Only .mp4 (H.264) videos are supported.";
            return config;
        }

        if (value.size() > 0x7FFFFFFFu)
        {
            config.error = L"config.md contains a Wallpaper entry that is too long.";
            return config;
        }

        const int valueLength = static_cast<int>(value.size());
        UINT codePage = CP_UTF8;
        DWORD conversionFlags = MB_ERR_INVALID_CHARS;

        int wideLength = MultiByteToWideChar(codePage, conversionFlags, value.c_str(),
                                             valueLength, nullptr, 0);
        if (wideLength <= 0)
        {
            codePage = CP_ACP;
            conversionFlags = 0;
            wideLength = MultiByteToWideChar(codePage, conversionFlags, value.c_str(),
                                             valueLength, nullptr, 0);
        }

        if (wideLength <= 0)
        {
            config.error = L"config.md contains a Wallpaper entry that cannot be decoded.";
            return config;
        }

        std::wstring wideValue(static_cast<size_t>(wideLength), L'\0');
        if (MultiByteToWideChar(codePage, conversionFlags, value.c_str(), valueLength,
                                wideValue.data(), wideLength) != wideLength)
        {
            config.error = L"config.md contains a Wallpaper entry that cannot be decoded.";
            return config;
        }

        const std::wstring path = configDir + L"\\wallpapers\\" + wideValue;

        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            config.error = L"Wallpaper file not found: " + path;
            return config;
        }

        config.wallpaperPath = path;
        config.valid = true;
        return config;
    }
    catch (...)
    {
        config.valid = false;
        config.wallpaperPath.clear();
        config.error = L"Unexpected failure while loading config.md.";
        return config;
    }
}

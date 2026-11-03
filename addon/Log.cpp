// Log.cpp : the add-on's log file.

#include "stdafx.h"
#include "Log.h"

namespace
{
    std::wstring g_path;

    std::wstring LogFolder()
    {
        wchar_t buffer[MAX_PATH] = {};
        DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            length = GetTempPathW(MAX_PATH, buffer);

        std::wstring folder(buffer, length);
        if (!folder.empty() && folder.back() != L'\\')
            folder += L'\\';
        folder += L"AlibreCAM\\";
        CreateDirectoryW(folder.c_str(), nullptr);
        return folder;
    }
}

void LogInit()
{
    g_path = LogFolder() + L"AlibreCAM.log";
    Log(L"==== AlibreCAM loaded ====");
}

void Log(const wchar_t* format, ...)
{
    if (g_path.empty())
        return;

    wchar_t message[2048] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME now = {};
    GetLocalTime(&now);

    FILE* file = nullptr;
    if (_wfopen_s(&file, g_path.c_str(), L"a, ccs=UTF-8") == 0 && file != nullptr)
    {
        fwprintf(file, L"%02u:%02u:%02u.%03u %s\n", now.wHour, now.wMinute, now.wSecond,
                 now.wMilliseconds, message);
        fclose(file);
    }
}

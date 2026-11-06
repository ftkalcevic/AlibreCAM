// Help.cpp : opens the add-on's help page in the browser at a section.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "Help.h"

#include <shellapi.h>
#include <shlwapi.h>

#include "Log.h"

#pragma comment(lib, "shlwapi.lib")

namespace fs = std::filesystem;

namespace
{
    fs::path HelpFile()
    {
        wchar_t module[MAX_PATH] = {};
        if (GetModuleFileNameW(AfxGetInstanceHandle(), module, MAX_PATH) == 0)
            return fs::path();
        return fs::path(module).parent_path() / L"help" / L"AlibreCAM.html";
    }

    // The command the default browser opens web links with, such as
    // "C:\...\msedge.exe" --single-argument %1.
    std::wstring BrowserCommand()
    {
        wchar_t command[2048] = {};
        DWORD length = static_cast<DWORD>(std::size(command));
        if (FAILED(AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_COMMAND, L"http", L"open", command, &length)))
            return std::wstring();
        return command;
    }

    // The browser's command with the URL in place of its %1 (or %L), or after it.
    // The URL isn't quoted: it has no spaces, and Chrome's and Edge's
    // --single-argument %1 takes quotes as part of the address.
    std::wstring CommandFor(std::wstring command, const std::wstring& url)
    {
        for (const wchar_t* placeholder : { L"%1", L"%L", L"%l" })
        {
            const size_t at = command.find(placeholder);
            if (at != std::wstring::npos)
                return command.replace(at, 2, url);
        }
        return command + L" " + url;
    }

    bool Start(std::wstring commandLine)
    {
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};
        if (!CreateProcessW(nullptr, &commandLine[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                            &process))
            return false;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }
}

void ShowHelp(const wchar_t* section)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    const fs::path file = HelpFile();
    std::error_code error;
    if (file.empty() || !fs::is_regular_file(file, error))
    {
        Log(L"[help] no help page at %s", file.c_str());
        AfxMessageBox(L"The help page wasn't found. Install the add-on again to put it beside AlibreCAM.dll.",
                      MB_OK | MB_ICONINFORMATION);
        return;
    }

    // file:///C:/.../AlibreCAM.html#section
    wchar_t url[4096] = {};
    DWORD length = static_cast<DWORD>(std::size(url));
    std::wstring target;
    if (SUCCEEDED(UrlCreateFromPathW(file.c_str(), url, &length, 0)))
    {
        target = url;
        if (section != nullptr && *section != L'\0')
            target += std::wstring(L"#") + section;
    }

    const std::wstring command = BrowserCommand();
    if (!target.empty() && !command.empty() && Start(CommandFor(command, target)))
    {
        Log(L"[help] opened %s with %s", target.c_str(), command.c_str());
        return;
    }

    // Without a browser command, open the page at its top.
    Log(L"[help] no browser command for http, so opening %s without its section", file.c_str());
    ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

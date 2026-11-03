// ModelReader.cpp : reads Alibre's model into plain data for CamCore.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "ModelReader.h"
#include "Log.h"

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty())
        return std::string();

    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &result[0], size,
                        nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& text)
{
    if (text.empty())
        return std::wstring();

    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &result[0], size);
    return result;
}

namespace
{
    // A null _bstr_t would crash a std::wstring constructor.
    std::wstring ToWide(const _bstr_t& text)
    {
        const wchar_t* pText = static_cast<const wchar_t*>(text);
        return (pText != nullptr) ? std::wstring(pText) : std::wstring();
    }

    std::string ReadFileGuid(const std::wstring& path)
    {
        std::string guid;
        BSTR rawGuid = nullptr;

        try
        {
            IObjectCollectorPtr constituentGuids;
            IObjectCollectorPtr constituentNames;
            IObjectCollectorPtr constituentPaths;
            theApp.m_pRoot->GetGuidAndConstituentInformation(_bstr_t(path.c_str()), &rawGuid, &constituentGuids,
                                                             &constituentNames, &constituentPaths);
            if (rawGuid != nullptr)
                guid = ToUtf8(rawGuid);
        }
        catch (const _com_error& error)
        {
            Log(L"[model] could not read the assembly GUID: hr=0x%08X", static_cast<unsigned>(error.Error()));
        }

        SysFreeString(rawGuid);
        return guid;
    }
}

bool ReadAssemblyInfo(IADSession* pSession, AssemblyInfo& info)
{
    if (pSession == nullptr)
        return false;

    try
    {
        info.identity.name = ToUtf8(ToWide(pSession->GetName()));

        const std::wstring path = ToWide(pSession->GetfilePath());
        info.path = path;
        info.identity.guid = path.empty() ? std::string() : ReadFileGuid(path);

        Log(L"[model] assembly '%s' path='%s' guid='%s'", FromUtf8(info.identity.name).c_str(), path.c_str(),
            FromUtf8(info.identity.guid).c_str());
        return true;
    }
    catch (const _com_error& error)
    {
        Log(L"[model] could not read the assembly: hr=0x%08X", static_cast<unsigned>(error.Error()));
        return false;
    }
}

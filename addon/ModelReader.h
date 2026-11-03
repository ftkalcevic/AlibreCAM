// ModelReader.h : reads Alibre's model into plain data for CamCore.

#pragma once

#include "CamJob.h"

struct AssemblyInfo
{
    std::filesystem::path path;         // empty until the assembly has been saved
    camcore::AssemblyIdentity identity;
};

// Conversions between Alibre's UTF-16 text and CamCore's UTF-8 text.
std::string ToUtf8(const std::wstring& text);
std::wstring FromUtf8(const std::string& text);

// Returns false and logs the reason if the session can't be read. A missing
// GUID isn't a failure: the identity's GUID is just left empty.
bool ReadAssemblyInfo(IADSession* pSession, AssemblyInfo& info);

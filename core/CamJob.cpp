// CamJob.cpp : reads and writes the .cam file.

#include "CamJob.h"

#include <cctype>
#include <exception>
#include <system_error>

#include <nlohmann/json.hpp>

#include "FileIO.h"

namespace camcore
{
    namespace
    {
        namespace fs = std::filesystem;
        using nlohmann::json;

        const char* const kFormatName = "AlibreCAM";

        LoadResult LoadFailure(LoadStatus status, const std::string& message, int fileVersion = 0)
        {
            LoadResult result;
            result.status = status;
            result.message = message;
            result.fileVersion = fileVersion;
            return result;
        }

        bool EqualIgnoringCase(const std::string& a, const std::string& b)
        {
            if (a.size() != b.size())
                return false;

            for (size_t i = 0; i < a.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            }
            return true;
        }

        std::string PathText(const fs::path& path)
        {
            return path.u8string();
        }
    }

    LoadResult LoadJob(const fs::path& path)
    {
        std::error_code error;
        if (!fs::exists(path, error))
            return LoadFailure(LoadStatus::NotFound, "File not found: " + PathText(path));

        std::string text;
        if (!ReadWholeFile(path, text))
            return LoadFailure(LoadStatus::Unreadable, "Could not read " + PathText(path));

        const json root = json::parse(text, nullptr, false);
        if (root.is_discarded())
            return LoadFailure(LoadStatus::Unreadable, "Not valid JSON: " + PathText(path));

        if (!root.is_object())
            return LoadFailure(LoadStatus::NotACamFile, "Not a .cam file: " + PathText(path));

        const auto format = root.find("format");
        if (format == root.end() || !format->is_string() || format->get<std::string>() != kFormatName)
            return LoadFailure(LoadStatus::NotACamFile, "Not a .cam file: " + PathText(path));

        const auto version = root.find("formatVersion");
        if (version == root.end() || !version->is_number_integer() || version->get<int>() < 1)
            return LoadFailure(LoadStatus::NotACamFile, "The format version is missing or invalid: " + PathText(path));

        const int fileVersion = version->get<int>();
        if (fileVersion > kCamFormatVersion)
        {
            return LoadFailure(LoadStatus::NewerVersion,
                               "This file uses format version " + std::to_string(fileVersion) +
                                   ", but this add-on supports up to version " +
                                   std::to_string(kCamFormatVersion) + ".",
                               fileVersion);
        }

        // Older formats will be migrated here when the format version changes.

        const auto assembly = root.find("assembly");
        if (assembly == root.end() || !assembly->is_object())
            return LoadFailure(LoadStatus::NotACamFile, "The assembly section is missing: " + PathText(path), fileVersion);

        const auto name = assembly->find("name");
        const auto guid = assembly->find("guid");
        if (name == assembly->end() || !name->is_string() || guid == assembly->end() || !guid->is_string())
            return LoadFailure(LoadStatus::NotACamFile, "The assembly section is incomplete: " + PathText(path), fileVersion);

        LoadResult result;
        result.status = LoadStatus::Ok;
        result.fileVersion = fileVersion;
        result.job.assembly.name = name->get<std::string>();
        result.job.assembly.guid = guid->get<std::string>();

        // Files saved before tools existed don't have the tools section.
        const auto tools = root.find("tools");
        if (tools != root.end())
        {
            if (!tools->is_object())
                return LoadFailure(LoadStatus::NotACamFile, "The tools section is invalid: " + PathText(path), fileVersion);

            const auto library = tools->find("library");
            if (library != tools->end() && library->is_string())
                result.job.toolLibraryPath = library->get<std::string>();

            const auto selected = tools->find("selected");
            if (selected != tools->end() && selected->is_string())
                result.job.selectedToolId = selected->get<std::string>();
        }
        return result;
    }

    SaveResult SaveJob(const Job& job, const fs::path& path)
    {
        SaveResult result;

        std::string text;
        try
        {
            const json root =
            {
                { "format", kFormatName },
                { "formatVersion", kCamFormatVersion },
                { "assembly",
                  {
                      { "name", job.assembly.name },
                      { "guid", job.assembly.guid }
                  } },
                { "tools",
                  {
                      { "library", job.toolLibraryPath },
                      { "selected", job.selectedToolId }
                  } },
                { "setup", json::object() },
                { "sequence", json::array() }
            };
            text = root.dump(2) + "\n";
        }
        catch (const std::exception& error)
        {
            result.message = std::string("Could not prepare the file: ") + error.what();
            return result;
        }

        result.ok = WriteFileAtomically(path, text, result.message);
        return result;
    }

    IdentityMatch CompareIdentity(const AssemblyIdentity& a, const AssemblyIdentity& b)
    {
        if (a.guid.empty() || b.guid.empty())
            return IdentityMatch::Unknown;

        return EqualIgnoringCase(a.guid, b.guid) ? IdentityMatch::Same : IdentityMatch::Different;
    }

    fs::path CamPathForAssembly(const fs::path& assemblyPath)
    {
        fs::path result = assemblyPath;
        if (!result.empty())
            result.replace_extension(".cam");
        return result;
    }
}

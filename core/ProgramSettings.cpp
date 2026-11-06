// ProgramSettings.cpp : the add-on's own settings, kept per user.

#include "ProgramSettings.h"

#include <system_error>

#include <nlohmann/json.hpp>

#include "FileIO.h"

namespace camcore
{
    namespace
    {
        namespace fs = std::filesystem;
        using json = nlohmann::json;

        const char* const kFormatName = "AlibreCAM settings";

        const char* ViewSizeName(ViewSize size)
        {
            switch (size)
            {
            case ViewSize::Medium: return "medium";
            case ViewSize::Large:  return "large";
            default:               return "small";
            }
        }

        ProgramSettingsLoadResult LoadFailure(LoadStatus status, const std::string& message)
        {
            ProgramSettingsLoadResult result;
            result.status = status;
            result.message = message;
            return result;
        }
    }

    int ViewIconSize(ViewSize size)
    {
        switch (size)
        {
        case ViewSize::Medium: return 24;
        case ViewSize::Large:  return 32;
        default:               return 16;
        }
    }

    double ViewTextScale(ViewSize size)
    {
        switch (size)
        {
        case ViewSize::Medium: return 1.25;
        case ViewSize::Large:  return 1.5;
        default:               return 1.0;
        }
    }

    ProgramSettingsLoadResult LoadProgramSettings(const fs::path& path)
    {
        std::error_code error;
        if (!fs::exists(path, error))
            return LoadFailure(LoadStatus::NotFound, "File not found: " + path.u8string());

        std::string text;
        if (!ReadWholeFile(path, text))
            return LoadFailure(LoadStatus::Unreadable, "Could not read " + path.u8string());

        const json root = json::parse(text, nullptr, false);
        if (root.is_discarded())
            return LoadFailure(LoadStatus::Unreadable, "Not valid JSON: " + path.u8string());

        const auto format = root.is_object() ? root.find("format") : root.end();
        if (!root.is_object() || format == root.end() || !format->is_string() ||
            format->get<std::string>() != kFormatName)
            return LoadFailure(LoadStatus::NotACamFile, "Not an AlibreCAM settings file: " + path.u8string());

        const auto version = root.find("formatVersion");
        if (version == root.end() || !version->is_number_integer() || version->get<int>() < 1)
            return LoadFailure(LoadStatus::NotACamFile, "The format version is missing or invalid: " + path.u8string());
        if (version->get<int>() > kProgramSettingsFormatVersion)
        {
            return LoadFailure(LoadStatus::NewerVersion,
                               "These settings use format version " + std::to_string(version->get<int>()) +
                                   ", but this add-on supports up to version " +
                                   std::to_string(kProgramSettingsFormatVersion) + ".");
        }

        ProgramSettingsLoadResult result;
        result.status = LoadStatus::Ok;
        const auto viewSize = root.find("viewSize");
        if (viewSize != root.end())
        {
            bool known = false;
            for (const ViewSize candidate : { ViewSize::Small, ViewSize::Medium, ViewSize::Large })
            {
                if (viewSize->is_string() && viewSize->get<std::string>() == ViewSizeName(candidate))
                {
                    result.settings.viewSize = candidate;
                    known = true;
                }
            }
            if (!known)
                return LoadFailure(LoadStatus::NotACamFile, "The view size isn't one this add-on knows: " +
                                                                 path.u8string());
        }

        const auto lastMachine = root.find("lastMachine");
        if (lastMachine != root.end())
        {
            if (!lastMachine->is_string())
                return LoadFailure(LoadStatus::NotACamFile, "The last machine is damaged: " + path.u8string());
            result.settings.lastMachineId = lastMachine->get<std::string>();
        }
        return result;
    }

    SaveResult SaveProgramSettings(const ProgramSettings& settings, const fs::path& path)
    {
        const json root =
        {
            { "format", kFormatName },
            { "formatVersion", kProgramSettingsFormatVersion },
            { "viewSize", ViewSizeName(settings.viewSize) },
            { "lastMachine", settings.lastMachineId }
        };

        SaveResult result;
        result.ok = WriteFileAtomically(path, root.dump(2) + "\n", result.message);
        return result;
    }
}

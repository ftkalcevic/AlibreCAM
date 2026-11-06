// ProgramSettings.h : the add-on's own settings, kept per user. No Alibre or MFC code belongs here.
//
// They're saved as JSON with a format version, like the tool library, in
// %APPDATA%\AlibreCAM\settings.json.

#pragma once

#include <filesystem>
#include <string>

#include "CamJob.h"     // LoadStatus, SaveResult

namespace camcore
{
    constexpr int kProgramSettingsFormatVersion = 1;

    // How big the CAM tab's text and icons are.
    enum class ViewSize
    {
        Small,      // Windows' default GUI font and 16 pixel icons
        Medium,
        Large
    };

    struct ProgramSettings
    {
        ViewSize viewSize = ViewSize::Small;

        // The machine used last, which a job with no machine takes.
        std::string lastMachineId;
    };

    // The icons' size in pixels: 16, 24 or 32.
    int ViewIconSize(ViewSize size);

    // How much bigger than Small the text and controls are: 1, 1.25 or 1.5.
    double ViewTextScale(ViewSize size);

    struct ProgramSettingsLoadResult
    {
        LoadStatus status = LoadStatus::Unreadable;
        ProgramSettings settings;   // the defaults unless status is Ok
        std::string message;        // empty when status is Ok
    };

    // Values a file doesn't have keep their defaults, so older files load.
    ProgramSettingsLoadResult LoadProgramSettings(const std::filesystem::path& path);
    SaveResult SaveProgramSettings(const ProgramSettings& settings, const std::filesystem::path& path);
}

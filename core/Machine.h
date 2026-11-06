// Machine.h : a machine, with its axes and its tools. No Alibre or MFC code belongs here.
//
// Each machine is a file of its own in %APPDATA%\AlibreCAM\machines\, named by
// its ID. A job stores the ID of the machine it uses.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "CamJob.h"     // LoadStatus, SaveResult
#include "ToolLibrary.h"

namespace camcore
{
    // Version 2 added the travel and the spindle's top speed, and version 3 more tool types.
    constexpr int kMachineFormatVersion = 3;

    // Which way the rotary (4th) axis runs.
    enum class RotaryAxis
    {
        MinusX,
        PlusX,
        MinusY,
        PlusY
    };

    // "-X", "+X", "-Y", "+Y".
    std::string RotaryAxisName(RotaryAxis axis);

    struct Machine
    {
        std::string id;     // stable; also the file's name
        std::string name;
        int axes = 3;       // 3, or 4 with a rotary axis
        RotaryAxis rotary = RotaryAxis::PlusY;      // used only with 4 axes

        // How far each axis can move, in mm. Version 1 files have these defaults.
        double travelX = 600.0;
        double travelY = 400.0;
        double travelZ = 100.0;

        double maxSpindleRpm = 24000.0;

        ToolLibrary tools;
    };

    // Returns a sentence for each problem. Empty means the machine is valid.
    std::vector<std::string> ValidateMachine(const Machine& machine);

    struct MachineLoadResult
    {
        LoadStatus status = LoadStatus::Unreadable;
        Machine machine;
        std::string message;    // empty when status is Ok
    };

    MachineLoadResult LoadMachine(const std::filesystem::path& path);
    SaveResult SaveMachine(const Machine& machine, const std::filesystem::path& path);

    // The file a machine is kept in, in the machines folder.
    std::filesystem::path MachineFilePath(const std::filesystem::path& folder, const std::string& id);

    // A machine file found in the machines folder.
    struct MachineEntry
    {
        std::filesystem::path path;
        LoadStatus status = LoadStatus::Unreadable;
        Machine machine;        // when status is Ok
        std::string message;    // otherwise
    };

    // Every .json file in the folder, the readable ones sorted by name first.
    std::vector<MachineEntry> FindMachines(const std::filesystem::path& folder);

    // Importing a tool library into a machine.
    struct ToolImportResult
    {
        int added = 0;
        int replaced = 0;
        int skipped = 0;
        int renumbered = 0;
    };

    // How many of from's tools have an ID the library already has.
    int CountSharedTools(const ToolLibrary& into, const ToolLibrary& from);

    // Adds from's tools, keeping their IDs so jobs that use them still find them.
    // A tool whose ID is already there replaces it, or is skipped, as asked. A
    // tool whose number is taken by another tool gets the lowest free number.
    ToolImportResult ImportTools(ToolLibrary& into, const ToolLibrary& from, bool replaceShared);
}

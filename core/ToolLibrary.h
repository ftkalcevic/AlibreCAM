// ToolLibrary.h : cutting tools and the library file that holds them.
//
// A tool keeps its values in its own unit, so an imperial cutter keeps its exact
// size. Calculations use ToolInMillimetres.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "CamJob.h"

namespace camcore
{
    constexpr int kToolLibraryFormatVersion = 1;

    enum class ToolType
    {
        FlatEndMill,
        Drill
    };

    enum class ToolUnit
    {
        Millimetres,
        Inches
    };

    // The physical tool only. Speeds, feeds and depths vary with the material,
    // so they belong to each operation.
    struct Tool
    {
        std::string id;     // stable, so jobs keep their reference if the tool is renumbered
        int number = 1;     // the T number in G-code
        std::string name;
        ToolType type = ToolType::FlatEndMill;
        ToolUnit unit = ToolUnit::Millimetres;

        double diameter = 0.0;
        double fluteLength = 0.0;
        double overallLength = 0.0;

        // Flat end mills only
        int fluteCount = 2;

        // Drills only
        double pointAngle = 118.0;      // degrees
    };

    struct ToolLibrary
    {
        std::vector<Tool> tools;
    };

    struct ToolLibraryLoadResult
    {
        LoadStatus status = LoadStatus::Unreadable;
        ToolLibrary library;
        std::string message;    // empty when status is Ok
    };

    std::string NewToolId();

    // Returns a sentence for each problem with the tool. Empty means it's valid.
    std::vector<std::string> ValidateTool(const Tool& tool);

    // As above, and also checks that tool numbers and IDs are unique.
    std::vector<std::string> ValidateLibrary(const ToolLibrary& library);

    // The same tool with lengths in mm and feeds in mm per minute.
    Tool ToolInMillimetres(const Tool& tool);

    const Tool* FindToolById(const ToolLibrary& library, const std::string& id);

    // The lowest tool number not already in use.
    int NextFreeToolNumber(const ToolLibrary& library);

    ToolLibraryLoadResult LoadToolLibrary(const std::filesystem::path& path);
    SaveResult SaveToolLibrary(const ToolLibrary& library, const std::filesystem::path& path);

    // A job keeps a library in its own folder as a bare file name, so the job and
    // library can move together. Any other library is stored as a full path.
    std::string StoredLibraryPath(const std::filesystem::path& camPath, const std::filesystem::path& libraryPath);
    std::filesystem::path ResolveLibraryPath(const std::filesystem::path& camPath, const std::string& stored);
}

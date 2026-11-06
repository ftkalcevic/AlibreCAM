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
    // Version 2 added each tool's feeds per material, and version 3 more tool types.
    constexpr int kToolLibraryFormatVersion = 3;

    enum class ToolType
    {
        FlatEndMill,
        Drill,
        BallMill,
        CornerRadiusMill,
        VMill,
        ChamferMill,
        TaperMill,
        ThreadMill
    };

    // As the Tool dialog lists them: "Flat end mill", "Ball mill" and so on.
    std::string ToolTypeTitle(ToolType type);

    // Every type, in the order the Tool dialog lists them.
    const std::vector<ToolType>& ToolTypes();

    // The size fields a type has besides diameter, flute length and overall length.
    struct ToolFields
    {
        bool fluteCount = false;
        bool pointAngle = false;
        bool cornerRadius = false;
        bool includedAngle = false;
        bool tipDiameter = false;
        bool taperAngle = false;
        bool pitch = false;
        bool threadAngle = false;
    };
    ToolFields FieldsOf(ToolType type);

    enum class ToolUnit
    {
        Millimetres,
        Inches
    };

    // A tool's spindle speed and feeds for one material, which fill an
    // operation's Feeds & Speeds tab when asked.
    struct ToolFeeds
    {
        std::string materialId;
        FeedSettings feeds;
    };

    // The physical tool, with its speeds and feeds for each material. An
    // operation keeps its own copy of the feeds, as they can be changed there.
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

        // Every type but drills
        int fluteCount = 2;

        // Drills only
        double pointAngle = 118.0;      // degrees

        // Corner radius mills only
        double cornerRadius = 1.0;

        // V-mills and chamfer mills: the angle across the point. With taper
        // mills, the diameter at the tip (0 is a sharp point).
        double includedAngle = 90.0;    // degrees
        double tipDiameter = 0.0;

        // Taper mills only: each side's angle from the axis.
        double taperAngle = 5.0;        // degrees

        // Thread mills only
        double pitch = 1.0;
        double threadAngle = 60.0;      // degrees

        // At most one for each material.
        std::vector<ToolFeeds> feeds;
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

    // The tool's feeds for a material, or null if it has none.
    const ToolFeeds* FeedsFor(const Tool& tool, const std::string& materialId);

    // The lowest tool number not already in use.
    int NextFreeToolNumber(const ToolLibrary& library);

    ToolLibraryLoadResult LoadToolLibrary(const std::filesystem::path& path);
    SaveResult SaveToolLibrary(const ToolLibrary& library, const std::filesystem::path& path);

    // Older jobs kept a library in their own folder as a bare file name, and any
    // other library as a full path. Tools now belong to the machine.
    std::string StoredLibraryPath(const std::filesystem::path& camPath, const std::filesystem::path& libraryPath);
    std::filesystem::path ResolveLibraryPath(const std::filesystem::path& camPath, const std::string& stored);
}

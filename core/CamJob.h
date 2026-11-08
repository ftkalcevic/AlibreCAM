// CamJob.h : the CAM job and its .cam file. No Alibre or MFC code belongs here.
//
// Text is UTF-8 throughout this library. All lengths will be in millimetres.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Boundary.h"
#include "Clearance.h"
#include "CutLevels.h"
#include "Drill.h"
#include "Engraving.h"
#include "PlungeRoughing.h"
#include "WaterlineRoughing.h"
#include "EntryExit.h"
#include "Facing.h"
#include "Feeds.h"
#include "Origin.h"
#include "HolePocketing.h"
#include "Pocketing.h"
#include "Profiling.h"
#include "Stock.h"
#include "Toolpath.h"

namespace camcore
{
    // The newest .cam format this build can read and the one it writes.
    // Version 2 added Set Origin steps and groups to the sequence.
    // Version 13 added arc moves in the ZX and YZ planes.
    constexpr int kCamFormatVersion = 13;

    // Which assembly a job belongs to. The GUID comes from the assembly file,
    // and is empty if it couldn't be read.
    struct AssemblyIdentity
    {
        std::string name;
        std::string guid;
    };

    enum class StepType
    {
        SetOrigin,
        Group,          // shown as a folder in the tree
        Drill,          // an operation. Operations only go in groups.
        Facing,         // an operation
        Profiling,      // an operation, 2 1/2 axis profiling
        Pocketing,      // an operation, 2 1/2 axis pocketing
        HolePocketing,  // an operation: round holes cut with an end mill on a spiral
        Chamfer,        // an operation: a chamfer along edges, with Profiling's settings too
        Engrave,        // an operation: the tool's centre follows lines
        PlungeRoughing, // an operation, 3 axis: plunges straight down on a packed grid
        WaterlineRoughing,  // an operation, 3 axis: pockets level by level down round the parts
        WaterlineFinishing, // an operation, 3 axis: one lap round the parts at each level
        ParallelFinishing   // an operation, 3 axis: straight rows following the parts' surface
    };

    bool IsOperation(StepType type);

    struct SequenceStep;

    // A waterline operation's settings: Roughing's or Finishing's.
    WaterlineRoughingSettings& WaterlineSettingsOf(SequenceStep& step);
    const WaterlineRoughingSettings& WaterlineSettingsOf(const SequenceStep& step);

    // One step in the ordered sequence under Operations.
    struct SequenceStep
    {
        StepType type = StepType::SetOrigin;
        std::string id;         // stable, from NewStableId
        std::string name;

        // A suppressed step can still be edited, but isn't generated with its
        // folder or the job, isn't posted, and a suppressed Set Origin is skipped.
        bool suppressed = false;

        // Set Origin steps only
        OriginDefinition origin;

        // Operations only: the picked geometry, in the order it's machined, and
        // where the clearance plane is.
        std::vector<PickedGeometry> features;
        ClearanceSettings clearance;

        // Area operations only: the stock box's footprint is part of the area too.
        bool stockFootprint = false;

        // Operations only: the ID of the tool in the job's library, or empty, and
        // the speeds and feeds to use it at.
        std::string toolId;
        FeedSettings feeds;

        // Operations only: the last toolpath generated. Empty until Generate runs.
        Toolpath toolpath;

        // Drill operations only
        DrillSettings drill;

        // Operations that cut in levels, such as Facing
        CutLevelSettings cutLevels;
        EntryExitSettings entryExit;

        // Facing operations only, and the outline Generate found for the area,
        // in the origin's frame. Empty until Generate runs.
        FacingSettings facing;

        // Profiling and Chamfer operations.
        ProfilingSettings profiling;

        // Chamfer operations only.
        ChamferSettings chamfer;

        // Engrave operations only.
        EngravingSettings engraving;

        // Pocketing operations only.
        PocketingSettings pocketing;

        // Hole Pocketing operations only.
        HolePocketingSettings holePocketing;

        // Plunge Roughing operations only.
        PlungeRoughingSettings plungeRoughing;

        // Waterline Roughing operations only.
        WaterlineRoughingSettings waterlineRoughing;

        // Waterline Finishing operations only: the same settings, less the pattern,
        // start points and linear extension.
        WaterlineRoughingSettings waterlineFinishing = WaterlineFinishingDefaults();

        // Parallel Finishing operations only: the same settings, the linear start,
        // angle of cuts and extension (its overtravel) used, the pattern not.
        WaterlineRoughingSettings parallelFinishing = ParallelFinishingDefaults();
        std::vector<Path> boundaries;

        // Groups only. Operations run in this order. Groups don't nest, and a
        // Set Origin stays outside them, so a group can't hold either.
        std::vector<SequenceStep> children;
    };

    // The machine is added in a later step.
    struct Job
    {
        AssemblyIdentity assembly;

        // At most one stock. Its kind is None until one is made.
        Stock stock;

        // The machine (its ID) the job is cut on, and the material (its ID) it's
        // cut from. Empty until chosen.
        std::string machineId;
        std::string materialId;

        // The tool library older jobs (before version 5) used, as StoredLibraryPath
        // returned it. Read but not saved: tools now belong to the machine.
        std::string toolLibraryPath;

        // The ID of the selected tool in the library, or empty.
        std::string selectedToolId;

        // Run from top to bottom. The first step is always a Set Origin.
        std::vector<SequenceStep> sequence;
    };

    enum class LoadStatus
    {
        Ok,
        NotFound,       // no file at this path
        Unreadable,     // the file couldn't be read, or isn't valid JSON
        NotACamFile,    // valid JSON, but not a usable .cam file
        NewerVersion    // written by a newer build, so it mustn't be overwritten
    };

    struct LoadResult
    {
        LoadStatus status = LoadStatus::Unreadable;
        Job job;
        int fileVersion = 0;
        std::string message;    // empty when status is Ok
    };

    struct SaveResult
    {
        bool ok = false;
        std::string message;    // empty when ok
    };

    enum class IdentityMatch
    {
        Same,
        Different,
        Unknown     // at least one GUID is missing, so they can't be compared
    };

    // 32 random hex digits, for IDs that must survive renaming and reordering.
    std::string NewStableId();

    constexpr size_t kNoStepIndex = static_cast<size_t>(-1);

    // The Set Origin a step runs under: the step itself if it's a Set Origin,
    // otherwise the nearest one above it. kNoStepIndex if there's none.
    size_t ActiveOriginIndex(const Job& job, size_t stepIndex);

    // The job's work zero: its first Set Origin that isn't suppressed, or
    // kNoStepIndex if there's none.
    size_t WorkZeroIndex(const Job& job);

    // True if an operation is suppressed, or the folder it's in is.
    bool IsOperationSuppressed(const Job& job, size_t folder, size_t operation);

    // Moving changes the order only, never the level: top-level steps stay in the
    // sequence, and operations stay in groups. `insertBefore` counts positions as
    // they are before the move, and the list's size means the end. Each returns
    // false, changing nothing, for a move that's invalid or changes nothing.

    // Refused if the sequence would no longer start with a Set Origin. On success
    // newIndex is where the step ended up.
    bool MoveStep(Job& job, size_t from, size_t insertBefore, size_t& newIndex);

    // Moves an operation within its group or into another one. On success
    // newIndex is where it ended up in toGroup.
    bool MoveOperation(Job& job, size_t fromGroup, size_t fromIndex, size_t toGroup, size_t insertBefore,
                       size_t& newIndex);

    // Copies a step, or an operation when operation isn't kNoStepIndex, to just
    // below it. A group's copy holds copies of its operations. Every copy gets a
    // new ID, and the copied step's name gets " (copy)". Returns false, changing
    // nothing, if there's no such step. newIndex is where the copy is: in the
    // sequence, or in the group for an operation.
    bool DuplicateStep(Job& job, size_t step, size_t operation, size_t& newIndex);

    // True if two steps have the same saved settings, ignoring the toolpath and
    // boundary that Generate makes from them.
    bool SameSettings(const SequenceStep& a, const SequenceStep& b);

    LoadResult LoadJob(const std::filesystem::path& path);

    // Writes a temporary file and then replaces the target, so a failed save
    // leaves any existing file as it was.
    SaveResult SaveJob(const Job& job, const std::filesystem::path& path);

    IdentityMatch CompareIdentity(const AssemblyIdentity& a, const AssemblyIdentity& b);

    // Bracket.AD_ASM becomes Bracket.cam, in the same folder.
    std::filesystem::path CamPathForAssembly(const std::filesystem::path& assemblyPath);
}

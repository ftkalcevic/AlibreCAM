// FinishingDump.h : saves and loads a finishing operation's whole input, so a
// problem seen in Alibre can be replayed in the tests. No Alibre or MFC code belongs here.

#pragma once

#include <filesystem>
#include <string>

#include "CamJob.h"
#include "ToolLibrary.h"
#include "WaterlineRoughToolpath.h"

namespace camcore
{
    // A loaded dump: the operation, its tool and its input, the input's operation
    // and tool pointing at these. Not to be copied once loaded.
    struct FinishingDump
    {
        SequenceStep operation;
        Tool tool;
        WaterlineRoughToolpathInput input;

        FinishingDump() = default;
        FinishingDump(const FinishingDump&) = delete;
        FinishingDump& operator=(const FinishingDump&) = delete;
    };

    // Writes the input (its operation, tool, area, parts, faces' surfaces and
    // heights; not its callbacks) to path as JSON, and the operation as a job
    // beside it (path with ".cam" added). False with a problem if it can't.
    bool SaveFinishingInput(const WaterlineRoughToolpathInput& input, const std::filesystem::path& path,
                            std::string& problem);

    // Reads what SaveFinishingInput wrote. Faces' NURBS surfaces are prepared.
    bool LoadFinishingInput(const std::filesystem::path& path, FinishingDump& dump, std::string& problem);
}

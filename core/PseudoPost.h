// PseudoPost.h : the built-in post, which writes toolpaths as APT CL data.
// No Alibre or MFC code belongs here.
//
// It stands in until real posts exist, and shows what a post will be given.
// The job has one work zero, the first Set Origin (see the spec's 4th-axis
// notes), so every point is in that frame, and each GOTO carries the tool axis
// as i, j, k. Later origins only change the axis and where the points fall.

#pragma once

#include <string>
#include <vector>

#include "CamJob.h"
#include "ToolLibrary.h"

namespace camcore
{
    // One operation to post, with what it runs under.
    struct PostOperation
    {
        const SequenceStep* origin = nullptr;       // the Set Origin it runs under
        Frame frame;                                // that origin's frame
        const SequenceStep* operation = nullptr;
        const Tool* tool = nullptr;                 // in mm, or null if it isn't in the library
    };

    // The whole program as APT CL data, in mm:
    //   PARTNO, UNITS/MM, then for each operation a $$ comment naming it,
    //   CUTTER and LOADTL when the tool changes, SPINDL/RPM, then RAPID before
    //   each rapid GOTO, FEDRAT/MMPM when the feed changes, DELAY for dwells,
    //   and finally SPINDL/OFF, END and FINI.
    // A $$ comment notes each change of origin, so posting part of a job is
    // still complete.
    std::string WriteAptClData(const std::string& jobName, const Frame& workZero,
                               const std::vector<PostOperation>& operations);
}

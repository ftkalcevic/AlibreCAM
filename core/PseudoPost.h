// PseudoPost.h : writes toolpaths as readable pseudo code, before real posts exist.
// No Alibre or MFC code belongs here.
//
// It shows what a post will be given: each operation in order, with its origin,
// tool, spindle speed and moves. Coordinates are in each origin's frame, in mm.

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
        const Tool* tool = nullptr;                 // null if it isn't in the library
    };

    // The whole program. An ORIGIN line comes before the first operation and
    // whenever the origin changes, and a TOOL line whenever the tool changes, so
    // posting part of a job is still complete.
    std::string WritePseudoPost(const std::string& jobName, const std::vector<PostOperation>& operations);
}

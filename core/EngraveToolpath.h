// EngraveToolpath.h : Engrave's lines and toolpath. No Alibre or MFC code belongs here.
//
// The tool's centre follows the lines. Points are in the operation's origin
// frame, in millimetres.

#pragma once

#include <vector>

#include "Boundary.h"
#include "CamJob.h"
#include "ToolLibrary.h"
#include "Toolpath.h"

namespace camcore
{
    // A line to engrave: an open polyline, or a closed loop that doesn't repeat its first point.
    struct EngraveLine
    {
        Path path;
        bool closed = false;
    };

    // The pieces joined into as few lines as possible, all flat at the level.
    // Pieces whose ends meet (within 0.01 mm, seen from above) join end to end,
    // turned round where needed. Where more than two ends meet, the line carries
    // on along the earliest piece in the list, and the others start lines of
    // their own. Each line starts from the earliest piece left, at a loose end if
    // it has one. Pieces with no length seen from above are left out.
    std::vector<EngraveLine> JoinEngraveLines(const std::vector<Path>& pieces, double level);

    // Everything an engraving toolpath is worked out from.
    struct EngraveToolpathInput
    {
        const SequenceStep* operation = nullptr;
        const Tool* tool = nullptr;             // in millimetres
        std::vector<EngraveLine> lines;         // from JoinEngraveLines, at the cut geometry's height

        bool havePartMaxZ = false;
        double partMaxZ = 0.0;
        bool haveStockMaxZ = false;
        double stockMaxZ = 0.0;
    };

    // The tool's centre follows each line at every cut level from the Cut Levels
    // tab, top down. With Return to Start, every level runs the line the same way,
    // lifting at its end and coming back to its start. With Stay Down, the tool
    // goes straight down to the next level where it is and runs back the other
    // way (a closed loop carries on round the same way), with the entry before the
    // first level and the exit after the last. Lines are cut in turn, each to its
    // full depth (Depth First), or every line at a level before the next (Level
    // First, where every cut lifts and comes back to the start).
    ToolpathResult GenerateEngraveToolpath(const EngraveToolpathInput& input);
}

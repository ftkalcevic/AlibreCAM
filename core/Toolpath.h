// Toolpath.h : the moves an operation makes, as calculated. No Alibre or MFC code belongs here.
//
// Points are in the frame of the origin the operation runs under, in millimetres,
// so Z is the tool axis. Toolpaths are saved, and drawn from what was saved.

#pragma once

#include <string>
#include <vector>

#include "Origin.h"

namespace camcore
{
    enum class MoveKind
    {
        Rapid,      // as fast as the machine goes
        Feed,       // at `feed` mm/min
        Dwell       // stays put for `seconds`
    };

    struct ToolpathMove
    {
        MoveKind kind = MoveKind::Rapid;
        Vec3 to;                // where the move ends. A dwell's is where the tool is.
        double feed = 0.0;      // feeds only
        double seconds = 0.0;   // dwells only
    };

    // The first move's end is where the operation starts.
    struct Toolpath
    {
        std::vector<ToolpathMove> moves;
    };

    // What a toolpath calculation gives back.
    struct ToolpathResult
    {
        bool ok = false;
        Toolpath toolpath;
        std::string problem;    // why it couldn't be made. Empty when ok.
    };
}

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

        // A feed can go round an arc in the frame's XY, about centre, instead of
        // straight. Z changes evenly along it, so it can be a helix.
        bool arc = false;
        Vec3 centre;
        bool clockwise = false; // seen from the frame's +Z
    };

    // The first move's end is where the operation starts.
    struct Toolpath
    {
        std::vector<ToolpathMove> moves;
    };

    // The points a move passes through after from, up to and including its end:
    // just the end for a straight move, or its arc sampled for drawing.
    std::vector<Vec3> MovePoints(const Vec3& from, const ToolpathMove& move);

    // What a toolpath calculation gives back.
    struct ToolpathResult
    {
        bool ok = false;
        Toolpath toolpath;
        std::string problem;    // why it couldn't be made. Empty when ok.

        // Parallel Finishing with a ball mill: how many points were moved onto the
        // true surface, of how many tried, with how many closest point calls.
        size_t refinedPoints = 0;
        size_t refineTries = 0;
        size_t refineCalls = 0;
        // Why the others kept the triangles' answer.
        size_t refineNoAnswer = 0;      // the closest point couldn't be had
        size_t refineOnSurface = 0;     // the ball's centre was on the surface
        size_t refineSideContact = 0;   // the contact was on the ball's side
        size_t refineUnsettled = 0;     // Newton's method didn't settle in 8 steps
        size_t refineOffFace = 0;       // it settled past the face's edges
        size_t refineTooFar = 0;        // it settled more than 0.5 mm from the triangles' answer
        size_t refineShared = 0;        // between two refined points, sharing their correction
    };
}

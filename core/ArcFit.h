// ArcFit.h : turns runs of short straight moves that follow a circle into arc moves. No Alibre or MFC code belongs here.

#pragma once

#include <string>
#include <vector>

#include "Toolpath.h"

namespace camcore
{
    // The machine's arcs: whether its controller takes them (G2 and G3), in which
    // planes, and how far a fitted arc may stray from the straight moves it replaces.
    struct ArcFitting
    {
        bool enabled = true;
        bool xy = true;     // G17
        bool zx = true;     // G18
        bool yz = true;     // G19
        double tolerance = 0.005;   // mm

        bool Allows(ArcPlane plane) const
        {
            if (!enabled)
                return false;
            return plane == ArcPlane::XY ? xy : plane == ArcPlane::ZX ? zx : yz;
        }
        bool operator==(const ArcFitting& other) const
        {
            return enabled == other.enabled && xy == other.xy && zx == other.zx && yz == other.yz &&
                   tolerance == other.tolerance;
        }
    };

    // Returns a sentence for each problem. Empty means the settings are valid.
    std::vector<std::string> ValidateArcFitting(const ArcFitting& fitting);

    struct ArcFitCounts
    {
        size_t movesBefore = 0;
        size_t movesAfter = 0;
        size_t linesReplaced = 0;   // straight moves turned into arcs
        size_t arcsMade = 0;
    };

    // Replaces each run of at least three straight feed moves, at one feed, lying
    // in an allowed plane of the toolpath's frame (all at one Z, Y or X) and along
    // a circle, with arc moves of no more than a half turn each. Every point and
    // every move is within the tolerance of the arc, the arc turns one way
    // throughout, and its radius is 0.01 to 1000 mm. Arcs end exactly where the
    // runs they replace do. Existing arcs, rapids and dwells are kept.
    ArcFitCounts FitArcs(Toolpath& toolpath, const ArcFitting& fitting);

    // Leaves out points of each run of straight feed moves at one feed (never across
    // arcs, rapids, dwells or feed changes) that are within the tolerance of the
    // straight line past them (Douglas-Peucker, in 3D). Generate fits arcs to the
    // dense moves first, then simplifies what stays straight.
    // Returns how many moves were left out.
    size_t SimplifyStraightRuns(Toolpath& toolpath, double tolerance);
}

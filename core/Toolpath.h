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

    // The plane an arc move turns in.
    enum class ArcPlane
    {
        XY,     // seen from +Z: G17
        ZX,     // seen from +Y, Z across and X up: G18
        YZ      // seen from +X, Y across and Z up: G19
    };

    // A point in a plane's own axes: u across, v up, w along its normal, so that
    // anticlockwise seen from the normal is the angle atan2(v, u) growing.
    void ToPlane(ArcPlane plane, const Vec3& point, double& u, double& v, double& w);
    Vec3 FromPlane(ArcPlane plane, double u, double v, double w);

    struct ToolpathMove
    {
        MoveKind kind = MoveKind::Rapid;
        Vec3 to;                // where the move ends. A dwell's is where the tool is.
        double feed = 0.0;      // feeds only
        double seconds = 0.0;   // dwells only

        // A feed can go round an arc about centre instead of straight, in the
        // frame's XY plane (Z may change evenly along it, a helix), or flat in its
        // ZX or YZ plane. Clockwise is seen from the plane's normal (+Z, +Y or +X),
        // as G2 and G3 are.
        bool arc = false;
        Vec3 centre;
        bool clockwise = false;
        ArcPlane plane = ArcPlane::XY;
    };

    // The first move's end is where the operation starts.
    struct Toolpath
    {
        std::vector<ToolpathMove> moves;
    };

    // The points a move passes through after from, up to and including its end:
    // just the end for a straight move, or its arc sampled for drawing.
    std::vector<Vec3> MovePoints(const Vec3& from, const ToolpathMove& move);

    // An arc move's turn from `from`, in radians: positive anticlockwise seen from
    // its plane's normal, negative clockwise, a whole turn when it ends where it starts.
    double ArcMoveSweep(const Vec3& from, const ToolpathMove& move);

    // An arc move's points after from, up to and including its end, no more than
    // `step` radians apart (along the helix for an XY arc that changes Z).
    std::vector<Vec3> ArcMovePoints(const Vec3& from, const ToolpathMove& move, double step);

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
        size_t lapPointsSmoothed = 0;   // Waterline Finishing: lap points put back between their neighbours
        size_t lapFingersRemoved = 0;   // thin fingers and small loops cut off (RemoveThinFingers)
        size_t lapTurnBacks = 0;        // and in the laps left, turns of more than 120 degrees
        size_t lapZigzags = 0;          // and S-bends within 1 mm with a turn of more than 90
        size_t lapWobbles = 0;          // and turns of over 10 degrees one way then the other within 0.5 mm
        size_t lapPointsBefore = 0;     // points in the laps before simplifying, and after
        size_t lapPointsAfter = 0;
    };
}

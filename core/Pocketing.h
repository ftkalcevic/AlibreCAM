// Pocketing.h : the Pocketing operation's own settings. No Alibre or MFC code belongs here.
//
// Distances are in millimetres and angles in degrees.

#pragma once

#include <string>
#include <vector>

#include "Facing.h"     // CutDirection, ToolRelativeSize

namespace camcore
{
    enum class PocketCutType
    {
        Offset,         // passes following the pocket's outline
        Linear,         // straight passes across it
        Spiral,
        Radial
    };

    // Where Offset, Spiral and Radial cuts start.
    enum class PocketStart
    {
        Inside,
        Outside
    };

    // Where Linear cuts start.
    enum class PocketLinearStart
    {
        Bottom,
        Top
    };

    // The Pocketing tab. Each cut type's start is kept, so switching back restores it.
    struct PocketingSettings
    {
        double stockToLeave = 0.0;                      // left on the walls, maybe for a finishing pass
        PocketCutType cutType = PocketCutType::Offset;
        CutDirection direction = CutDirection::Climb;   // Spiral can't be Mixed, so cuts Climb instead
        double cutAngle = 0.0;                          // Linear only: 90 runs along the origin's X, 0 along its Y
        PocketStart start = PocketStart::Inside;
        PocketLinearStart linearStart = PocketLinearStart::Bottom;
        ToolRelativeSize stepover{ true, 50.0, 2.0 };

        // Adaptive clearing: constant engagement at the stepover in place of the
        // pattern, helixing into closed areas.
        bool adaptive = false;
        RampEntry rampEntry;

        // Cut only the stock the operations before this one left, ignoring anything
        // thinner than the minimum.
        bool restMachining = false;
        double restMinimum = 0.1;
    };

    // The direction the cut type actually uses: Spiral can't be Mixed.
    CutDirection PocketDirection(const PocketingSettings& settings);

    // True if the cut type can be Mixed, and if it uses the Angle of Cuts.
    bool PocketAllowsMixed(PocketCutType type);
    bool PocketUsesAngle(PocketCutType type);

    // Returns a sentence for each problem. Empty means the settings are valid.
    std::vector<std::string> ValidatePocketing(const PocketingSettings& settings);
}

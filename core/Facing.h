// Facing.h : the Facing operation's own settings. No Alibre or MFC code belongs here.
//
// Distances are in millimetres.

#pragma once

#include <string>
#include <vector>

namespace camcore
{
    enum class CutDirection
    {
        Climb,
        Conventional,
        Mixed           // both, cutting in each direction
    };

    // A size given as a share of the tool's diameter or as a distance. Both
    // values are kept, so switching back restores the old one.
    struct ToolRelativeSize
    {
        bool usePercent = true;
        double percent = 0.0;       // of the tool's diameter
        double distance = 0.0;
    };

    // The Facing tab.
    struct FacingSettings
    {
        CutDirection direction = CutDirection::Climb;
        ToolRelativeSize stepDistance{ true, 25.0, 2.0 };   // across, between passes
        ToolRelativeSize overtravel{ true, 50.0, 3.0 };     // past the area's edge
    };

    // The size in mm for a tool of this diameter.
    double ResolveSize(const ToolRelativeSize& size, double toolDiameter);

    // Returns a sentence for each problem. Empty means the settings are valid.
    std::vector<std::string> ValidateFacing(const FacingSettings& settings);
}

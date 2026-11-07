// PlungeRoughing.h : the Plunge Roughing operation's own settings. No Alibre or MFC code belongs here.
//
// Distances are in millimetres and angles in degrees.

#pragma once

#include <string>
#include <vector>

#include "Facing.h"     // ToolRelativeSize

namespace camcore
{
    // How the plunges are ordered along each row.
    enum class PlungePathDirection
    {
        Zig,        // every row the same way
        ZigZag      // each row back the other way
    };

    // Which side of the area the first row is on.
    enum class PlungeStartSide
    {
        Bottom,
        Top
    };

    // The Plunge Roughing tab.
    struct PlungeRoughingSettings
    {
        double stockToLeave = 0.5;                          // left on the parts, and on the area's wall
        PlungePathDirection direction = PlungePathDirection::ZigZag;
        PlungeStartSide start = PlungeStartSide::Bottom;
        double cutAngle = 0.0;                              // the rows: 90 runs along the origin's X, 0 along its Y
        ToolRelativeSize stepover{ true, 75.0, 4.5 };       // between neighbouring plunges
        double approach = 2.0;                              // above the stock's top, where the plunge's feed starts
    };

    // Returns a sentence for each problem. Empty means the settings are valid.
    std::vector<std::string> ValidatePlungeRoughing(const PlungeRoughingSettings& settings);
}

// Drill.h : the Drill operation's own settings. No Alibre or MFC code belongs here.
//
// Lengths are in millimetres. The settings shared by every operation (features,
// tool, feeds and speeds, clearance, sorting) are added as their tabs are built.

#pragma once

#include <string>
#include <vector>

namespace camcore
{
    // Where the picked hole geometry sits on the hole.
    enum class GeometryLocation
    {
        AtTop,
        AtBottom
    };

    enum class DwellMode
    {
        Off,
        Seconds,
        Revolutions
    };

    // The Drilling tab.
    struct DrillSettings
    {
        double depth = 0.0;     // how far to drill

        // Drill further by the length of the tool's point, so the full diameter
        // reaches the depth.
        bool addTipToDepth = false;

        GeometryLocation location = GeometryLocation::AtTop;

        // Both values are kept, so switching the mode back restores the old one.
        DwellMode dwell = DwellMode::Off;
        double dwellSeconds = 0.5;
        double dwellRevolutions = 3.0;

        double approachDistance = 2.0;

        // No peck depth means drilling in one pass.
        bool pecking = false;
        double peckDepth = 0.0;
    };

    // Returns a sentence for each problem. Empty means the settings are valid.
    std::vector<std::string> ValidateDrillSettings(const DrillSettings& settings);
}

// WaterlineRoughing.h : the Waterline Roughing operation's own settings. No Alibre or MFC code belongs here.
//
// Distances are in millimetres.

#pragma once

#include <string>
#include <vector>

namespace camcore
{
    // The Waterline Roughing tab.
    struct WaterlineRoughingSettings
    {
        double stockToLeave = 0.5;      // left covering the parts, on their tops and sides
        double stepDown = 2.0;          // the deepest a level may be below the one above it
    };

    // The stepover, for now: half the tool's diameter.
    constexpr double kWaterlineStepoverFraction = 0.5;

    // Returns a sentence for each problem. Empty means the settings are valid.
    std::vector<std::string> ValidateWaterlineRoughing(const WaterlineRoughingSettings& settings);
}

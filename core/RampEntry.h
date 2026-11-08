// RampEntry.h : how the tool gets down into material, by a helix or a ramp. No Alibre or MFC code belongs here.

#pragma once

#include <string>
#include <vector>

namespace camcore
{
    // Shared by adaptive clearing (its helix into closed areas), and later by
    // roughing's entries.
    struct RampEntry
    {
        double helixPercent = 80.0;     // the helix's diameter (its centre's path), % of the tool's diameter
        double rampAngle = 2.0;         // degrees down from horizontal, for the helix and ramps

        bool operator==(const RampEntry& other) const
        {
            return helixPercent == other.helixPercent && rampAngle == other.rampAngle;
        }
    };

    // Returns a sentence for each problem. Empty means the settings are valid.
    std::vector<std::string> ValidateRampEntry(const RampEntry& ramp);
}

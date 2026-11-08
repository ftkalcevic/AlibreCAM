// RampEntry.h : how the tool gets down into material, by a helix or a ramp. No Alibre or MFC code belongs here.

#pragma once

#include <string>
#include <vector>

namespace camcore
{
    // How a cut is entered: straight down, down a ramp along the cut, or down a helix.
    enum class EntryKind
    {
        Plunge,
        Ramp,
        Helix
    };

    // Shared by adaptive clearing (its helix into closed areas; the kind isn't
    // used) and Waterline Roughing's entries.
    struct RampEntry
    {
        EntryKind kind = EntryKind::Helix;
        double helixPercent = 80.0;     // the helix's diameter (its centre's path), % of the tool's diameter
        double rampAngle = 2.0;         // degrees down from horizontal, for the helix and ramps

        bool operator==(const RampEntry& other) const
        {
            return kind == other.kind && helixPercent == other.helixPercent && rampAngle == other.rampAngle;
        }
    };

    // Returns a sentence for each problem. Empty means the settings are valid.
    std::vector<std::string> ValidateRampEntry(const RampEntry& ramp);
}

// Clearance.h : where an operation's clearance plane sits. No Alibre or MFC code belongs here.
//
// Heights are in millimetres, along the Z axis of the origin the operation runs under.

#pragma once

#include <string>
#include <vector>

namespace camcore
{
    enum class ClearanceMode
    {
        Automatic,
        PartMaxZ,       // the highest point of the parts, plus a distance
        StockMaxZ,      // the top of the stock, plus a distance
        AbsoluteZ       // a fixed Z
    };

    // The Clearance tab every operation has. Each value is kept, so switching the
    // mode back restores the old one.
    struct ClearanceSettings
    {
        ClearanceMode mode = ClearanceMode::Automatic;
        double partDistance = 5.0;
        double stockDistance = 5.0;
        double absoluteZ = 10.0;
    };

    // Returns a sentence for each problem. Empty means the settings are valid.
    std::vector<std::string> ValidateClearance(const ClearanceSettings& settings);
}

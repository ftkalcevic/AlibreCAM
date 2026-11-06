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

    // Automatic means this far above the parts, or the stock where that's higher.
    constexpr double kAutomaticClearanceAboveParts = 5.0;

    // The clearance plane's Z in the operation's frame, given the parts' highest
    // point in it, and the stock's when there's a stock. Returns false and sets
    // problem if it can't be worked out.
    bool ClearanceHeight(const ClearanceSettings& clearance, bool havePartMaxZ, double partMaxZ, double& height,
                         std::string& problem, bool haveStockMaxZ = false, double stockMaxZ = 0.0);
}

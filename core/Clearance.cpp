// Clearance.cpp : where an operation's clearance plane sits.

#include "Clearance.h"

namespace camcore
{
    std::vector<std::string> ValidateClearance(const ClearanceSettings& settings)
    {
        // Only the distance the chosen mode uses is checked. An absolute Z can be anything.
        std::vector<std::string> problems;
        if (settings.mode == ClearanceMode::PartMaxZ && !(settings.partDistance >= 0.0))
            problems.push_back("The distance above the parts can't be negative.");
        if (settings.mode == ClearanceMode::StockMaxZ && !(settings.stockDistance >= 0.0))
            problems.push_back("The distance above the stock can't be negative.");
        return problems;
    }
}

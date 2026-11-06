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

    bool ClearanceHeight(const ClearanceSettings& clearance, bool havePartMaxZ, double partMaxZ, double& height,
                         std::string& problem, bool haveStockMaxZ, double stockMaxZ)
    {
        switch (clearance.mode)
        {
        case ClearanceMode::AbsoluteZ:
            height = clearance.absoluteZ;
            return true;

        case ClearanceMode::StockMaxZ:
            if (!haveStockMaxZ)
            {
                problem = "The clearance plane is set from the stock, and there's no stock.";
                return false;
            }
            height = stockMaxZ + clearance.stockDistance;
            return true;

        case ClearanceMode::PartMaxZ:
            if (!havePartMaxZ)
            {
                problem = "The parts' height couldn't be read for the clearance plane.";
                return false;
            }
            height = partMaxZ + clearance.partDistance;
            return true;

        case ClearanceMode::Automatic:
        default:
        {
            // Above the parts, or the stock where it's higher.
            if (!havePartMaxZ && !haveStockMaxZ)
            {
                problem = "The parts' height couldn't be read for the clearance plane.";
                return false;
            }
            double top = havePartMaxZ ? partMaxZ : stockMaxZ;
            if (haveStockMaxZ && stockMaxZ > top)
                top = stockMaxZ;
            height = top + kAutomaticClearanceAboveParts;
            return true;
        }
        }
    }
}

// Facing.cpp : the Facing operation's own settings.

#include "Facing.h"

namespace camcore
{
    double ResolveSize(const ToolRelativeSize& size, double toolDiameter)
    {
        return size.usePercent ? toolDiameter * size.percent / 100.0 : size.distance;
    }

    std::vector<std::string> ValidateFacing(const FacingSettings& settings)
    {
        // Only the value in use is checked. A pass can't step more than the tool's width.
        std::vector<std::string> problems;
        const ToolRelativeSize& step = settings.stepDistance;
        if (step.usePercent ? !(step.percent > 0.0 && step.percent <= 100.0) : !(step.distance > 0.0))
        {
            problems.push_back(step.usePercent ? "The step distance must be more than 0% and at most 100% of the tool."
                                               : "The step distance must be more than 0.");
        }

        const ToolRelativeSize& over = settings.overtravel;
        if (over.usePercent ? !(over.percent >= 0.0) : !(over.distance >= 0.0))
            problems.push_back("The overtravel can't be negative.");

        return problems;
    }
}

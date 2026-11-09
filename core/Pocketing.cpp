// Pocketing.cpp : the Pocketing operation's own settings.

#include "Pocketing.h"

#include <cmath>

namespace camcore
{
    bool PocketAllowsMixed(PocketCutType type)
    {
        return type != PocketCutType::Spiral;
    }

    bool PocketUsesAngle(PocketCutType type)
    {
        return type == PocketCutType::Linear;
    }

    CutDirection PocketDirection(const PocketingSettings& settings)
    {
        if (settings.direction == CutDirection::Mixed && !PocketAllowsMixed(settings.cutType))
            return CutDirection::Climb;
        return settings.direction;
    }

    std::vector<std::string> ValidatePocketing(const PocketingSettings& settings)
    {
        std::vector<std::string> problems;
        if (!std::isfinite(settings.stockToLeave))
            problems.push_back("The stock to leave must be a number.");
        if (settings.restMachining && (!(settings.restMinimum >= 0.0) || !std::isfinite(settings.restMinimum)))
            problems.push_back("The minimum material must be 0 or more.");
        if (PocketUsesAngle(settings.cutType) && !std::isfinite(settings.cutAngle))
            problems.push_back("The angle of cuts must be a number.");

        // Only the value in use is checked. A pass can't step more than the tool's width.
        const ToolRelativeSize& step = settings.stepover;
        if (step.usePercent ? !(step.percent > 0.0 && step.percent <= 100.0) : !(step.distance > 0.0))
        {
            problems.push_back(step.usePercent ? "The stepover must be more than 0% and at most 100% of the tool."
                                               : "The stepover must be more than 0.");
        }
        if (settings.adaptive)
        {
            for (const std::string& problem : ValidateRampEntry(settings.rampEntry))
                problems.push_back(problem);
        }
        return problems;
    }
}

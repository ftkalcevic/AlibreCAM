// PlungeRoughing.cpp : the Plunge Roughing operation's own settings.

#include "PlungeRoughing.h"

#include <cmath>

namespace camcore
{
    std::vector<std::string> ValidatePlungeRoughing(const PlungeRoughingSettings& settings)
    {
        std::vector<std::string> problems;
        if (!(settings.stockToLeave >= 0.0) || !std::isfinite(settings.stockToLeave))
            problems.push_back("The stock to leave must be 0 or more.");
        if (!std::isfinite(settings.cutAngle))
            problems.push_back("The angle of cuts must be a number.");
        if (!(settings.approach >= 0.0) || !std::isfinite(settings.approach))
            problems.push_back("The approach must be 0 or more.");

        // Only the value in use is checked. It has no maximum: plunges further apart
        // than the tool is wide leave plugs between them.
        const ToolRelativeSize& step = settings.stepover;
        if (step.usePercent ? !(step.percent > 0.0 && std::isfinite(step.percent))
                            : !(step.distance > 0.0 && std::isfinite(step.distance)))
        {
            problems.push_back(step.usePercent ? "The stepover must be more than 0% of the tool."
                                               : "The stepover must be more than 0.");
        }
        return problems;
    }
}
